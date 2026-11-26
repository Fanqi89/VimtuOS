/* user/browser/fetchers/vimtu.c - ★ v0.12.6：NetSurf 的 http/https fetcher，用**我们自己的** socket + mbedTLS
 *
 * 这份文件替换上游的 content/fetchers/curl.c（libcurl 我们不要、也移植不动）：
 *   上游 curl.c  = libcurl 多句柄 + select + 回调；
 *   本文件       = 内核 socket(41/42/44/45) + poll(7) + mbedTLS SSL 上下文 + NetSurf 的 fetch 回调协议。
 *
 * 关键设计（每一条都是被现场逼出来的）：
 *   ① **绝不阻塞事件循环**：内核的 recv 在没数据时会 sleep 最多 3000 ms（sock64.cpp 的默认限）。
 *      所以本 fetcher 每次先 poll(fd, 0) 探可读/可写，再 recv/send —— 探不到就 return，
 *      等 NetSurf 下一次 fetch_poll（约 10 ms 后）再来。BIO 回调同样遵守这一点。
 *   ② mbedTLS 非阻塞：BIO 回 WANT_READ/WANT_WRITE，握手里靠上层重试，而不是在 BIO 里睡死。
 *   ③ 只有在 establish 阶段（DNS + connect）会短暂阻塞：那一步内核侧是 SYN/ACK 等待，
 *      偶尔几十毫秒，可以接受（GUI 会掉 1~2 帧）。
 *   ④ 证书校验**必须**开：CA 读 /etc/ssl/certs/ca.pem（v0.12.4 已随系统卷交付，118 份根证书）。
 *      校验失败 → FETCH_ERROR，绝不静默降级（v0.12.4 的负向对照已经证明错证书必须失败）。
 *   ⑤ 有界时间：每个 fetch 90 秒硬超时 → FETCH_TIMEDOUT（NetSurf 会显示自己的错误页）。
 *
 * 回调协议（NetSurf 的约定，不许自创）：
 *   fetch_set_http_code() → FETCH_HEADER（状态行之外的头逐行发）→ FETCH_DATA（正文可分块）
 *   → FETCH_FINISHED；或 FETCH_REDIRECT / FETCH_ERROR / FETCH_TIMEDOUT。
 *   ★ 每次回调之后都要重新检查 ctx->aborted —— 回调可能让上层把这个 fetch 释放掉。
 *
 * 交付：本文件被 tools/netsurf_core_build_win.sh 编进 libnetsurfcore.a（fetchers 组），
 *       fetch.c 里由 tools/netsurf_patch_fetch.py 幂等插入 fetch_vimtu_register() 调用。
 */

#include <stdbool.h>
#include <stdarg.h>   /* va_list/va_start：send_header_line 用 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <libwapcaplet/libwapcaplet.h>

#include "utils/nsurl.h"
#include "utils/log.h"
#include "utils/corestrings.h"
#include "utils/ring.h"
#include "utils/utils.h"

#include "content/fetch.h"
#include "content/fetchers.h"

/* mbedTLS */
#include "mbedtls/build_info.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"

#ifndef MBEDTLS_ERR_NET_RECV_FAILED
#define MBEDTLS_ERR_NET_RECV_FAILED   -0x004C
#endif
#ifndef MBEDTLS_ERR_NET_SEND_FAILED
#define MBEDTLS_ERR_NET_SEND_FAILED   -0x004E
#endif

#define VIMTU_RX_CHUNK     4096
#define VIMTU_HDR_MAX      (64 * 1024)
#define VIMTU_STALL_MS     90000
#define VIMTU_CA_PATH      "/etc/ssl/certs/ca.pem"

/* 状态机 */
enum vimtu_state {
    VIMTU_IDLE = 0,
    VIMTU_TLS_HANDSHAKE,
    VIMTU_SEND_REQ,
    VIMTU_RECV_HDR,
    VIMTU_RECV_BODY,
    VIMTU_DONE,
    VIMTU_FAILED
};

struct fetch_vimtu_context {
    struct fetch *parent_fetch;
    nsurl *url;

    /* 目标 */
    bool https;
    int port;
    char *host;          /* 头里 Host: 用（含端口则带端口） */
    char *reqpath;       /* 请求行里的路径（含 query） */

    int fd;

    enum vimtu_state state;

    /* 请求 */
    char *req;
    size_t req_len, req_off;

    /* 响应头累积区 */
    char *hdr;
    size_t hdr_len;

    /* 正文 */
    size_t content_len;      /* (size_t)-1 = 未知 */
    size_t body_got;
    bool chunked;
    bool hdr_sent;           /* 头已解析并发给上层 */
    bool finish_sent;
    size_t chunk_left;       /* chunked 解码的剩余字节 */
    bool chunk_end_pending;

    /* TLS */
    mbedtls_ssl_context *ssl;
    mbedtls_ssl_config *conf;
    mbedtls_ctr_drbg_context *drbg;
    mbedtls_entropy_context *entropy;
    mbedtls_x509_crt *cacert;
    bool tls_inited;

    /* 收包原始缓冲（TLS 走 mbedtls_ssl_read 直接进上层，不用它） */
    uint8_t rx[VIMTU_RX_CHUNK];

    uint64_t deadline_ms;

    bool aborted;
    bool locked;

    struct fetch_vimtu_context *r_next, *r_prev;
};

static struct fetch_vimtu_context *ring = NULL;
static bool vimtu_ca_loaded = false;
static mbedtls_x509_crt *vimtu_ca = NULL;      /* 进程内共享一份 CA（200 KB，不能每个 fetch 一份） */

/* ------------------------------------------------------------------ */
/* 小工具                                                             */
/* ------------------------------------------------------------------ */

static uint64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return (uint64_t)time(NULL) * 1000u;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static bool fd_readable(int fd)
{
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    return poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
}

static bool fd_writable(int fd)
{
    struct pollfd p;
    p.fd = fd;
    p.events = POLLOUT;
    p.revents = 0;
    return poll(&p, 1, 0) > 0 && (p.revents & (POLLOUT | POLLHUP | POLLERR)) != 0;
}

static void send_cb(const fetch_msg *msg, struct fetch_vimtu_context *c)
{
    c->locked = true;
    fetch_send_callback(msg, c->parent_fetch);
    c->locked = false;
}

static void send_error(struct fetch_vimtu_context *c, const char *err)
{
    fetch_msg msg;
    msg.type = FETCH_ERROR;
    msg.data.error = err;
    send_cb(&msg, c);
    c->state = VIMTU_FAILED;
}

static void send_header_line(struct fetch_vimtu_context *c, const char *fmt, ...)
{
    char line[512];
    fetch_msg msg;
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0 || n >= (int)sizeof(line)) {
        return;
    }
    msg.type = FETCH_HEADER;
    msg.data.header_or_data.buf = (const uint8_t *)line;
    msg.data.header_or_data.len = (size_t)n;
    send_cb(&msg, c);
}

/* ------------------------------------------------------------------ */
/* mbedTLS BIO（非阻塞：先探再读写）                                   */
/* ------------------------------------------------------------------ */

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    struct fetch_vimtu_context *c = ctx;
    size_t off = 0;

    while (off < len) {
        if (!fd_writable(c->fd)) {
            if (off == 0) {
                return MBEDTLS_ERR_SSL_WANT_WRITE;
            }
            break;
        }
        ssize_t n = send(c->fd, buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (off == 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
                break;
            }
            return MBEDTLS_ERR_NET_SEND_FAILED;
        }
        if (n == 0) return MBEDTLS_ERR_NET_SEND_FAILED;
        off += (size_t)n;
    }
    return (int)off;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    struct fetch_vimtu_context *c = ctx;

    if (!fd_readable(c->fd)) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    for (;;) {
        ssize_t n = recv(c->fd, buf, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
            return MBEDTLS_ERR_NET_RECV_FAILED;
        }
        return (int)n;
    }
}

/* ------------------------------------------------------------------ */
/* URL 解析（不用 nsurl 的组件枚举，直接从规范化字符串切）              */
/* ------------------------------------------------------------------ */

static bool parse_url(struct fetch_vimtu_context *c)
{
    const char *u = nsurl_access(c->url);
    const char *p, *host_start, *host_end, *path;
    char *hp;
    size_t host_len;

    if (u == NULL) return false;

    if (strncasecmp(u, "https://", 8) == 0) {
        c->https = true;
        p = u + 8;
        c->port = 443;
    } else if (strncasecmp(u, "http://", 7) == 0) {
        c->https = false;
        p = u + 7;
        c->port = 80;
    } else {
        return false;
    }

    host_start = p;
    /* host 结束于 '/' 或 '?' 或 '#' 或串尾 */
    while (*p && *p != '/' && *p != '?' && *p != '#') {
        /* userinfo@：跳过（我们不发 basic auth） */
        if (*p == '@') {
            host_start = p + 1;
        }
        p++;
    }
    host_end = p;

    path = (*p == '\0') ? "/" : (*p == '#' ? "/" : p);

    /* host[:port] */
    host_len = (size_t)(host_end - host_start);
    hp = strchr(host_start, ':');
    if (hp != NULL && hp < host_end) {
        c->port = atoi(hp + 1);
        host_len = (size_t)(hp - host_start);
    }
    if (host_len == 0 || host_len > 255) {
        return false;
    }

    c->reqpath = strdup(path);
    if (c->reqpath == NULL) return false;

    /* Host 头：默认端口不带，非默认端口要带 */
    c->host = malloc(host_len + 16);
    if (c->host == NULL) return false;
    memcpy(c->host, host_start, host_len);
    c->host[host_len] = '\0';
    if (!((c->https && c->port == 443) || (!c->https && c->port == 80))) {
        size_t l = strlen(c->host);
        snprintf(c->host + l, 16, ":%d", c->port);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* 建立连接（唯一会短暂阻塞的地方）                                    */
/* ------------------------------------------------------------------ */

static bool do_connect(struct fetch_vimtu_context *c)
{
    struct addrinfo hints, *res = NULL;
    char portstr[16];
    struct sockaddr_in *sin;
    int rc;

    snprintf(portstr, sizeof(portstr), "%d", c->port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(c->host, portstr, &hints, &res);
    if (rc != 0 || res == NULL) {
        NSLOG(netsurf, INFO, "vimtu: dns failed host=%s gai=%d", c->host, rc);
        send_error(c, "DNS 解析失败（检查 /etc/resolv.conf 与网关）");
        return false;
    }
    sin = (struct sockaddr_in *)res->ai_addr;

    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0) {
        freeaddrinfo(res);
        send_error(c, "socket() 失败");
        return false;
    }
    if (connect(c->fd, (struct sockaddr *)sin, sizeof(*sin)) != 0) {
        char ip[64] = {0};
        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
        freeaddrinfo(res);
        NSLOG(netsurf, INFO, "vimtu: connect failed %s:%d errno=%d", ip, c->port, errno);
        send_error(c, "TCP 连接失败");
        return false;
    }
    freeaddrinfo(res);
    return true;
}

/* ------------------------------------------------------------------ */
/* 发请求                                                             */
/* ------------------------------------------------------------------ */

static bool build_request(struct fetch_vimtu_context *c)
{
    size_t cap = strlen(c->host) + strlen(c->reqpath) + 512;
    c->req = malloc(cap);
    if (c->req == NULL) return false;

    /* HTTP/1.1 + Connection: close：我们不做 keep-alive（一次 fetch 一条连接，
     * 简单且不会因为复用连接把 TLS 状态搞乱） */
    c->req_len = (size_t)snprintf(c->req, cap,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: VimtuOS/0.12.6 (NetSurf 3.12; vimtu frontend)\r\n"
        "Accept: text/html,application/xhtml+xml,image/svg+xml,image/*;q=0.8,*/*;q=0.5\r\n"
        "Accept-Language: zh-CN,zh;q=0.9,en;q=0.8\r\n"
        "Accept-Encoding: identity\r\n"
        "Connection: close\r\n"
        "\r\n",
        c->reqpath, c->host);
    c->req_off = 0;
    return true;
}

/* ------------------------------------------------------------------ */
/* TLS 初始化（每连接一次）                                            */
/* ------------------------------------------------------------------ */

static bool load_ca(void)
{
    FILE *f;
    long sz;
    unsigned char *buf;
    size_t got;
    int rc;

    if (vimtu_ca_loaded) {
        return vimtu_ca != NULL;
    }
    vimtu_ca_loaded = true;

    f = fopen(VIMTU_CA_PATH, "rb");
    if (f == NULL) {
        NSLOG(netsurf, INFO, "vimtu: CA 缺失 %s", VIMTU_CA_PATH);
        return false;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        return false;
    }
    /* ★ 多分配 1 字节并**写结尾 0**：mbedtls_x509_crt_parse 判 PEM 靠
     *   `buf[buflen-1]=='\0' && strstr(...)`；少这一字节 PEM 会被当 DER，
     *   结果是 certs=0 却握手“成功”（v0.12.4 踩过的假绿）。 */
    buf = malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        return false;
    }
    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;

    vimtu_ca = calloc(1, sizeof(*vimtu_ca));
    if (vimtu_ca == NULL) {
        free(buf);
        return false;
    }
    mbedtls_x509_crt_init(vimtu_ca);
    rc = mbedtls_x509_crt_parse(vimtu_ca, buf, got + 1);
    free(buf);
    if (rc != 0) {
        NSLOG(netsurf, INFO, "vimtu: CA parse ret=-0x%04X", (unsigned)-rc);
        mbedtls_x509_crt_free(vimtu_ca);
        free(vimtu_ca);
        vimtu_ca = NULL;
        return false;
    }
    return true;
}

static bool tls_setup(struct fetch_vimtu_context *c)
{
    int rc;
    static const char *pers = "vimtu-fetch";

    if (!load_ca()) {
        send_error(c, "CA 证书包缺失或无法解析（/etc/ssl/certs/ca.pem）");
        return false;
    }

    c->ssl = calloc(1, sizeof(*c->ssl));
    c->conf = calloc(1, sizeof(*c->conf));
    c->drbg = calloc(1, sizeof(*c->drbg));
    c->entropy = calloc(1, sizeof(*c->entropy));
    c->cacert = vimtu_ca;          /* 共享，不释放 */
    if (c->ssl == NULL || c->conf == NULL || c->drbg == NULL || c->entropy == NULL) {
        send_error(c, "内存不足（TLS 上下文）");
        return false;
    }

    mbedtls_ssl_init(c->ssl);
    mbedtls_ssl_config_init(c->conf);
    mbedtls_ctr_drbg_init(c->drbg);
    mbedtls_entropy_init(c->entropy);
    c->tls_inited = true;

    rc = mbedtls_ctr_drbg_seed(c->drbg, mbedtls_entropy_func, c->entropy,
                               (const unsigned char *)pers, strlen(pers));
    if (rc != 0) {
        send_error(c, "TLS 熵源初始化失败");
        return false;
    }
    rc = mbedtls_ssl_config_defaults(c->conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        send_error(c, "TLS 默认配置失败");
        return false;
    }
    mbedtls_ssl_conf_authmode(c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(c->conf, c->cacert, NULL);
    mbedtls_ssl_conf_rng(c->conf, mbedtls_ctr_drbg_random, c->drbg);
    mbedtls_ssl_conf_read_timeout(c->conf, 0);

    rc = mbedtls_ssl_setup(c->ssl, c->conf);
    if (rc != 0) {
        send_error(c, "TLS ssl_setup 失败");
        return false;
    }
    rc = mbedtls_ssl_set_hostname(c->ssl, c->host);
    if (rc != 0) {
        send_error(c, "TLS set_hostname 失败");
        return false;
    }
    mbedtls_ssl_set_bio(c->ssl, c, bio_send, bio_recv, NULL);

    c->state = VIMTU_TLS_HANDSHAKE;
    return true;
}

/* ------------------------------------------------------------------ */
/* 响应头解析                                                         */
/* ------------------------------------------------------------------ */

static int parse_status_line(const char *hdr, int *code_out)
{
    /* "HTTP/1.1 200 OK\r\n" */
    const char *sp = strchr(hdr, ' ');
    if (sp == NULL) return -1;
    *code_out = atoi(sp + 1);
    return 0;
}

/* 在头里找某个字段（大小写不敏感），返回值指针到值起点，长度写入 *vlen */
static const char *find_hdr_value(const char *hdr, size_t hlen, const char *name,
                                 size_t *vlen)
{
    size_t nlen = strlen(name);
    const char *p = hdr;
    const char *end = hdr + hlen;

    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (eol == NULL) break;
        if ((size_t)(eol - p) > nlen && strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            const char *ve = eol;
            while (ve > v && (ve[-1] == '\r' || ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            *vlen = (size_t)(ve - v);
            return v;
        }
        p = eol + 1;
    }
    return NULL;
}

/* 把响应头（每行）发给上层；返回 false 表示被 abort */
static bool emit_headers(struct fetch_vimtu_context *c, int code)
{
    const char *p = strstr(c->hdr, "\r\n");
    size_t vlen;
    const char *v;
    char line[512];

    /* 状态行之后的每一行 */
    while (p != NULL && (size_t)(p - c->hdr) < c->hdr_len) {
        const char *line_start = p + 2;
        const char *eol = strstr(line_start, "\r\n");
        size_t llen;
        if (eol == NULL) break;
        llen = (size_t)(eol - line_start);
        if (llen == 0) break;
        if (llen < sizeof(line)) {
            memcpy(line, line_start, llen);
            line[llen] = '\0';
            send_header_line(c, "%s", line);
            if (c->aborted) return false;
        }
        p = eol;
    }

    /* Set-Cookie：交给 urldb 的 cookie 罐（llcache 会调 fetch_set_cookie） */
    v = find_hdr_value(c->hdr, c->hdr_len, "Set-Cookie", &vlen);
    if (v != NULL && vlen < 400) {
        char ck[420];
        memcpy(ck, v, vlen);
        ck[vlen] = '\0';
        fetch_set_cookie(c->parent_fetch, ck);
    }

    (void)code;
    return true;
}

/* ------------------------------------------------------------------ */
/* 状态机一次步进                                                     */
/* ------------------------------------------------------------------ */

static void step(struct fetch_vimtu_context *c)
{
    if (c->aborted || c->state == VIMTU_DONE || c->state == VIMTU_FAILED) {
        return;
    }

    if (now_ms() > c->deadline_ms) {
        fetch_msg msg;
        msg.type = FETCH_TIMEDOUT;
        send_cb(&msg, c);
        c->state = VIMTU_FAILED;
        return;
    }

    switch (c->state) {
    case VIMTU_TLS_HANDSHAKE: {
        int rc = mbedtls_ssl_handshake(c->ssl);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return;                                  /* 下次 poll 再来 */
        }
        if (rc != 0) {
            char ebuf[128];
            mbedtls_strerror(rc, ebuf, sizeof(ebuf));
            NSLOG(netsurf, INFO, "vimtu: TLS handshake FAILED ret=-0x%04X %s",
                  (unsigned)-rc, ebuf);
            send_error(c, "TLS 握手/证书校验失败");
            return;
        }
        NSLOG(netsurf, INFO, "vimtu: TLS ok %s %s",
              mbedtls_ssl_get_version(c->ssl), mbedtls_ssl_get_ciphersuite(c->ssl));
        c->state = VIMTU_SEND_REQ;
        /* 落到 SEND_REQ，同一次 poll 里把请求发出去 */
    }
    /* fallthrough */
    case VIMTU_SEND_REQ: {
        while (c->req_off < c->req_len) {
            ssize_t n;
            if (c->https) {
                int rc = mbedtls_ssl_write(c->ssl, (const unsigned char *)c->req + c->req_off,
                                           c->req_len - c->req_off);
                if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                    return;
                }
                if (rc < 0) {
                    send_error(c, "TLS 发送失败");
                    return;
                }
                n = rc;
            } else {
                if (!fd_writable(c->fd)) {
                    return;
                }
                n = send(c->fd, c->req + c->req_off, c->req_len - c->req_off, 0);
                if (n < 0) {
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return;
                    send_error(c, "TCP 发送失败");
                    return;
                }
            }
            c->req_off += (size_t)n;
        }
        c->state = VIMTU_RECV_HDR;
        c->hdr = malloc(VIMTU_HDR_MAX);
        if (c->hdr == NULL) {
            send_error(c, "内存不足（响应头缓冲）");
            return;
        }
        c->hdr_len = 0;
        c->content_len = (size_t)-1;
    }
    /* fallthrough */
    case VIMTU_RECV_HDR:
    case VIMTU_RECV_BODY: {
        uint8_t *dst;
        size_t want;
        int got;

        /* ① 先把可读的数据读进来：头还没完就读进 hdr，头完了就直发正文 */
        if (c->state == VIMTU_RECV_HDR) {
            dst = (uint8_t *)c->hdr + c->hdr_len;
            want = VIMTU_HDR_MAX - c->hdr_len - 1;
            if (want == 0) {
                send_error(c, "响应头过大");
                return;
            }
        } else {
            dst = c->rx;
            want = sizeof(c->rx);
        }

        if (c->https) {
            got = mbedtls_ssl_read(c->ssl, dst, want);
            if (got == MBEDTLS_ERR_SSL_WANT_READ || got == MBEDTLS_ERR_SSL_WANT_WRITE) {
                return;
            }
            if (got == 0) got = 0;                    /* close_notify / EOF */
        } else {
            if (!fd_readable(c->fd)) {
                return;
            }
            got = (int)recv(c->fd, dst, want, 0);
            if (got < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return;
                send_error(c, "TCP 接收失败");
                return;
            }
        }

        if (c->state == VIMTU_RECV_HDR) {
            c->hdr_len += (size_t)got;
            c->hdr[c->hdr_len] = '\0';

            char *hend = strstr(c->hdr, "\r\n\r\n");
            if (hend == NULL) {
                if (got == 0) {
                    send_error(c, "服务器在响应头之前断开");
                }
                return;                              /* 继续收头 */
            }

            /* 头齐了 */
            {
                size_t hlen = (size_t)(hend + 4 - c->hdr);
                size_t body_off = hlen;
                size_t body_in_buf = c->hdr_len - body_off;
                int code = 0;
                size_t vlen = 0;
                const char *v;
                fetch_msg msg;

                if (parse_status_line(c->hdr, &code) != 0 || code == 0) {
                    send_error(c, "无法解析 HTTP 状态行");
                    return;
                }
                fetch_set_http_code(c->parent_fetch, (http_response_code)code);
                NSLOG(netsurf, INFO, "vimtu: HTTP %d %s", code, c->host);

                /* 3xx 且有 Location → 让 llcache 去跟（它自己会重定向） */
                v = find_hdr_value(c->hdr, hlen, "Location", &vlen);
                if (code >= 300 && code < 400 && v != NULL && vlen > 0 && vlen < 2048) {
                    char loc[2048];
                    memcpy(loc, v, vlen);
                    loc[vlen] = '\0';
                    msg.type = FETCH_REDIRECT;
                    msg.data.redirect = loc;
                    send_cb(&msg, c);
                    c->state = VIMTU_DONE;
                    return;
                }

                v = find_hdr_value(c->hdr, hlen, "Content-Length", &vlen);
                if (v != NULL && vlen > 0) {
                    char tmp[32];
                    if (vlen < sizeof(tmp)) {
                        memcpy(tmp, v, vlen);
                        tmp[vlen] = '\0';
                        c->content_len = (size_t)strtoull(tmp, NULL, 10);
                    }
                }
                v = find_hdr_value(c->hdr, hlen, "Transfer-Encoding", &vlen);
                if (v != NULL && vlen >= 7 && strncasecmp(v, "chunked", 7) == 0) {
                    c->chunked = true;
                }

                if (!emit_headers(c, code)) {
                    return;                          /* aborted */
                }
                c->hdr_sent = true;
                c->state = VIMTU_RECV_BODY;

                /* 头缓冲里已经带上的正文部分：搬到 rx 前面重新走一遍正文路径 */
                if (body_in_buf > 0) {
                    size_t n = body_in_buf;
                    if (n > sizeof(c->rx)) n = sizeof(c->rx);
                    memmove(c->rx, c->hdr + body_off, n);
                    /* 直接就地处理这 n 字节 */
                    {
                        msg.type = FETCH_DATA;
                        msg.data.header_or_data.buf = c->rx;
                        msg.data.header_or_data.len = n;
                        send_cb(&msg, c);
                        c->body_got += n;
                        if (c->aborted) return;
                    }
                }
                return;
            }
        }

        /* 正文路径 */
        if (got > 0) {
            fetch_msg msg;
            msg.type = FETCH_DATA;
            /* ★ 不做 chunked 解码：NetSurf 的 llcache 只关心字节流，
             *   而 "Transfer-Encoding: chunked" 的解析在 llcache 里有一份
             *   （content/llcache.c 处理），这里若重复解码会破坏数据。
             *   实测约定：fetcher 只负责把**原文**字节往上送。 */
            msg.data.header_or_data.buf = dst;
            msg.data.header_or_data.len = (size_t)got;
            send_cb(&msg, c);
            c->body_got += (size_t)got;
            if (c->aborted) return;
            return;
        }

        /* got == 0：对端关闭 = 响应结束 */
        {
            fetch_msg msg;
            if (c->content_len != (size_t)-1 && c->body_got < c->content_len) {
                NSLOG(netsurf, INFO, "vimtu: 正文截断 got=%zu want=%zu",
                      c->body_got, c->content_len);
                send_error(c, "正文不完整（连接提前关闭）");
                return;
            }
            msg.type = FETCH_FINISHED;
            send_cb(&msg, c);
            c->finish_sent = true;
            c->state = VIMTU_DONE;
        }
        return;
    }

    case VIMTU_IDLE:
    case VIMTU_DONE:
    case VIMTU_FAILED:
    default:
        return;
    }
}

/* ------------------------------------------------------------------ */
/* fetcher 操作表                                                     */
/* ------------------------------------------------------------------ */

static void vimtu_ctx_free(struct fetch_vimtu_context *c)
{
    if (c->fd >= 0) {
        /* 先 close_notify 再 close：不给服务器留半开连接 */
        if (c->https && c->ssl != NULL && c->tls_inited) {
            (void)mbedtls_ssl_close_notify(c->ssl);
        }
        close(c->fd);
        c->fd = -1;
    }
    if (c->tls_inited) {
        mbedtls_ssl_free(c->ssl);
        mbedtls_ssl_config_free(c->conf);
        mbedtls_ctr_drbg_free(c->drbg);
        mbedtls_entropy_free(c->entropy);
        free(c->ssl);
        free(c->conf);
        free(c->drbg);
        free(c->entropy);
        c->ssl = NULL;
        c->conf = NULL;
        c->drbg = NULL;
        c->entropy = NULL;
        c->tls_inited = false;
    }
    free(c->host);
    free(c->reqpath);
    free(c->req);
    free(c->hdr);
    if (c->url != NULL) {
        nsurl_unref(c->url);
        c->url = NULL;
    }
    free(c);
}

static bool fetch_vimtu_initialise(lwc_string *scheme)
{
    NSLOG(netsurf, INFO, "vimtu: initialise %s", lwc_string_data(scheme));
    return true;
}

static void fetch_vimtu_finalise(lwc_string *scheme)
{
    NSLOG(netsurf, INFO, "vimtu: finalise %s", lwc_string_data(scheme));
    if (vimtu_ca != NULL) {
        mbedtls_x509_crt_free(vimtu_ca);
        free(vimtu_ca);
        vimtu_ca = NULL;
        vimtu_ca_loaded = false;
    }
}

static bool fetch_vimtu_can_fetch(const nsurl *url)
{
    const char *u = nsurl_access(url);
    if (u == NULL) return false;
    return (strncasecmp(u, "http://", 7) == 0 || strncasecmp(u, "https://", 8) == 0);
}

static void *fetch_vimtu_setup(struct fetch *parent_fetch, nsurl *url,
                               bool only_2xx, bool downgrade_tls,
                               const char *post_urlenc,
                               const struct fetch_multipart_data *post_multipart,
                               const char **headers)
{
    struct fetch_vimtu_context *c;

    (void)only_2xx;
    (void)downgrade_tls;
    (void)post_urlenc;
    (void)post_multipart;
    (void)headers;

    c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->parent_fetch = parent_fetch;
    c->url = nsurl_ref(url);
    c->fd = -1;
    c->state = VIMTU_IDLE;
    c->content_len = (size_t)-1;
    c->deadline_ms = now_ms() + VIMTU_STALL_MS;

    RING_INSERT(ring, c);
    return c;
}

static bool fetch_vimtu_start(void *ctx)
{
    struct fetch_vimtu_context *c = ctx;

    if (!parse_url(c)) {
        send_error(c, "不支持的 URL（本 fetcher 只做 http/https）");
        return true;
    }
    if (!do_connect(c)) {
        return true;             /* 错误已发给上层，poll 里会收尾 */
    }
    if (!build_request(c)) {
        send_error(c, "内存不足（请求行）");
        return true;
    }
    c->deadline_ms = now_ms() + VIMTU_STALL_MS;

    if (c->https) {
        if (!tls_setup(c)) {
            return true;
        }
    } else {
        c->state = VIMTU_SEND_REQ;
    }
    return true;
}

static void fetch_vimtu_abort(void *ctx)
{
    struct fetch_vimtu_context *c = ctx;
    /* 只打标记：可能正处在回调里，真正的释放放到 poll 里做 */
    c->aborted = true;
}

static void fetch_vimtu_free(void *ctx)
{
    struct fetch_vimtu_context *c = ctx;
    vimtu_ctx_free(c);
}

static void fetch_vimtu_poll(lwc_string *scheme)
{
    struct fetch_vimtu_context *c, *save_ring = NULL;

    (void)scheme;

    while (ring != NULL) {
        c = ring;
        RING_REMOVE(ring, c);

        if (c->locked) {
            RING_INSERT(save_ring, c);
            continue;
        }

        if (!c->aborted) {
            step(c);
        }

        if (c->aborted || c->state == VIMTU_DONE || c->state == VIMTU_FAILED) {
            fetch_remove_from_queues(c->parent_fetch);
            fetch_free(c->parent_fetch);
            continue;
        }

        RING_INSERT(save_ring, c);
    }

    ring = save_ring;
}

nserror fetch_vimtu_register(void)
{
    lwc_string *scheme;
    const struct fetcher_operation_table fetcher_ops = {
        .initialise = fetch_vimtu_initialise,
        .acceptable = fetch_vimtu_can_fetch,
        .setup = fetch_vimtu_setup,
        .start = fetch_vimtu_start,
        .abort = fetch_vimtu_abort,
        .free = fetch_vimtu_free,
        .poll = fetch_vimtu_poll,
        .fdset = NULL,
        .finalise = fetch_vimtu_finalise
    };
    nserror err;

    scheme = lwc_string_ref(corestring_lwc_http);
    err = fetcher_add(scheme, &fetcher_ops);
    if (err != NSERROR_OK) {
        NSLOG(netsurf, INFO, "vimtu: 注册 http 失败");
        return err;
    }

    scheme = lwc_string_ref(corestring_lwc_https);
    err = fetcher_add(scheme, &fetcher_ops);
    if (err != NSERROR_OK) {
        NSLOG(netsurf, INFO, "vimtu: 注册 https 失败");
        return err;
    }

    NSLOG(netsurf, INFO, "vimtu: 已接管 http/https（socket+mbedTLS）");
    return NSERROR_OK;
}
