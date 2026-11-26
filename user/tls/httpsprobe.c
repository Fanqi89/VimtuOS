/* user/tls/httpsprobe.c - ★ v0.12.4：ring3 真 HTTPS 探针（mbedTLS 3.6.2 + 内核 socket 层）
 *
 * 这份程序是"TLS 到底能不能用"的唯一判据。它不 print 一句"成功了"，而是把**每一步**都打出来：
 *   熵（我们自己的 CSPRNG）-> 时间（证书有效期要用）-> CA -> DNS -> TCP -> 握手 ->
 *   协商结果（版本/套件）-> 证书主体/签发/有效期 -> GET / 的状态行 -> 正文字节数。
 * 然后**必须**再跑一个"错证书站点"，握手**必须失败** —— 没有这条负向对照，"能连上"就不算数。
 *
 * 为什么 socket 自己写（不用 mbedTLS 的 net_sockets.c）：
 *   net_sockets 走 select/fcntl/getsockopt 的完整语义；本内核只实现了 Linux socket 的一个子集。
 *   这里直接把内核的 send(44)/recv(45) 包成 mbedtls_ssl_set_bio 的两个回调，路径最短、可观测。
 *
 * 交付：build64/httpsprobe.elf -> 系统卷 /bin/httpsprobe（tools/tls_pack_win.py，逐字节回读自检）
 * 触发：卷上有 /etc/tls_probe.on 时，内核启动期直接跑它一次（kernel/kernel64.cpp 的探针钩子），
 *       于是验收脚本**不需要键盘**（QEMU sendkey 在 1 fps 的 GUI 下会丢字，这条坑踩过多次）。
 * 配置：/etc/tlsprobe.conf（可选），键 = 值，一行一条：
 *       host=example.com     正面目标主机（走 DNS）
 *       port=443             正面目标端口
 *       path=/               请求路径
 *       ca=/etc/ssl/certs/ca.pem   CA 包（PEM，可多份拼接）
 *       negaddr=10.0.2.2     负向目标（**不过 DNS**，直接点 IP；QEMU slirp 里 10.0.2.2 = 宿主）
 *       negport=8443         负向目标端口（宿主上跑一个自签证书的 https 服务）
 *       negskip=0            1 = 跳过负向对照（会如实打 SKIP，不许当成通过）
 *
 * 边界（如实）：
 *   * TLS 1.3 在本版 mbedTLS 配置里是**关**的（3.6 默认关，要另一组 KEY_EXCHANGE_MODE 宏），
 *     所以协商结果会是 TLS 1.2；这不是"没实现"，是本轮先吃 1.2，下一轮开 1.3 再验。
 *   * 时间来自 CMOS RTC（kernel/x86_64.cpp 的 rtc_epoch64），只有 1 秒精度、且假定 RTC 走 UTC。
 *     SNTP 校时是**下一步**；RTC 读不出来时本程序会把 `time src=UNREADABLE` 和
 *     `verify=UNTRUSTED` 一起打出来，绝不当成"验过了"。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/random.h>

/* mbedTLS 的 net_sockets 错误码（include/mbedtls/net_sockets.h）—— 我们关掉了 MBEDTLS_NET_C，
 * 那个头不再被编译进来，但 BIO 回调的返回值语义仍然要遵守：**除了 WANT_* 之外**的负值都是致命错误。
 * 这里按上游的数值抄一份（R 前缀避免与上游宏撞名）。 */
#ifndef MBEDTLS_ERR_NET_RECV_FAILED
#define MBEDTLS_ERR_NET_RECV_FAILED   -0x004C  /**< 接收失败 */
#endif
#ifndef MBEDTLS_ERR_NET_SEND_FAILED
#define MBEDTLS_ERR_NET_SEND_FAILED   -0x004E  /**< 发送失败 */
#endif

#include "mbedtls/build_info.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"

/* mbedtls_hardware_poll / 自检（user/lib/mbedtls_vimtu_port.c） */
unsigned vimtu_mbedtls_hwpoll_selftest(unsigned char* out_a, unsigned char* out_b, size_t n);

/* ==================== 串口输出（直写 fd 1）====================
 * 为什么不用 printf：stdout 在 musl 里默认是全缓冲（它不是 tty，内核没实现 isatty），
 *   于是"探针跑完才一次性吐出来"，中途卡死就一点证据都拿不到。这里每行一次 write(1)。 */
static void say(const char* fmt, ...)
{
    char buf[640];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    ssize_t w = write(1, buf, (size_t)n);
    (void)w;
}
/* 二进制 -> 十六进制（只用于把随机数/指纹打进日志） */
static const char* hexdup(const unsigned char* p, size_t n)
{
    /* ★ v0.12.4：两个独立缓冲区。第一版只有一个 static 缓冲，于是
     *   say("… a=%s b=%s", hexdup(a,16), hexdup(b,16)) 里两个参数**指向同一块内存**，
     *   打印出来永远是最后写入的那份（现场：a 与 b 一模一样，看着像"两次熵完全相同"
     *   这种严重怀疑，而自检位其实是对的）。这类"日志骗人"必须当场修，不然下次误导判断。 */
    static char bufs[2][160];
    static int turn;
    char* out = bufs[turn];
    turn ^= 1;
    static const char* H = "0123456789abcdef";
    if (n > 60) n = 60;
    for (size_t i = 0; i < n; i++) { out[i * 2] = H[p[i] >> 4]; out[i * 2 + 1] = H[p[i] & 15]; }
    out[n * 2] = 0;
    return out;
}

/* ==================== 配置（/etc/tlsprobe.conf）==================== */
struct ProbeCfg {
    char host[128], path[128], ca[128], negaddr[64];
    int  port, negport, negskip;
};
static void cfg_defaults(struct ProbeCfg* c)
{
    memset(c, 0, sizeof *c);
    strcpy(c->host, "example.com");
    strcpy(c->path, "/");
    strcpy(c->ca, "/etc/ssl/certs/ca.pem");
    c->port = 443;
    c->negport = 8443;
    c->negskip = 0;
    c->negaddr[0] = 0;
}
static int read_file(const char* path, unsigned char** out, size_t* out_len, size_t max)
{
    *out = NULL; *out_len = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -errno;
    // ★ v0.12.4：多分配 1 字节并**总是写结尾 0**。为什么必须：
    //   mbedtls_x509_crt_parse() 靠 `buf[buflen-1] == '\0' && strstr(...)` 判"这是 PEM 包"，
    //   结尾没有 0 就直接走 DER 分支 -> 对 PEM 文本报 -0x2180 (X509_INVALID_FORMAT)。
    //   实测现场：`[TLS] ca … parse_ret=-8576 certs=0`（certs=0 就是这一条造成的假"CA 无效"）。
    unsigned char* buf = (unsigned char*)malloc(max + 1);
    if (!buf) { close(fd); return -ENOMEM; }
    size_t got = 0;
    while (got < max) {
        ssize_t r = read(fd, buf + got, max - got);
        if (r < 0) { if (errno == EINTR) continue; free(buf); close(fd); return -errno; }
        if (r == 0) break;
        got += (size_t)r;
    }
    close(fd);
    buf[got] = 0;                                 /* 结尾 0（见上面为什么要它） */
    *out = buf; *out_len = got;
    return 0;
}
static void cfg_load(struct ProbeCfg* c)
{
    unsigned char* b = NULL; size_t n = 0;
    if (read_file("/etc/tlsprobe.conf", &b, &n, 4096) != 0) {
        say("[TLS] conf /etc/tlsprobe.conf absent -> builtin defaults (host=%s port=%d)", c->host, c->port);
        return;
    }
    say("[TLS] conf /etc/tlsprobe.conf bytes=%u", (unsigned)n);
    char* p = (char*)b; char* end = p + n;
    while (p < end) {
        char* line = p;
        char* nl = memchr(p, '\n', (size_t)(end - p));
        if (nl) { *nl = 0; p = nl + 1; } else { p = end; }
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* k = line; const char* v = eq + 1;
        if (!strcmp(k, "host"))  { snprintf(c->host, sizeof c->host, "%s", v); }
        else if (!strcmp(k, "port"))    c->port = atoi(v);
        else if (!strcmp(k, "path"))    snprintf(c->path, sizeof c->path, "%s", v);
        else if (!strcmp(k, "ca"))      snprintf(c->ca, sizeof c->ca, "%s", v);
        else if (!strcmp(k, "negaddr")) snprintf(c->negaddr, sizeof c->negaddr, "%s", v);
        else if (!strcmp(k, "negport")) c->negport = atoi(v);
        else if (!strcmp(k, "negskip")) c->negskip = atoi(v);
    }
    free(b);
}

/* ==================== socket BIO（内核 send(44)/recv(45)）==================== */
struct Bio { int fd; long rx_bytes, tx_bytes; };

/* 返回发出的字节数，或 mbedTLS 的 WANT 码（外层循环会重试）。
 * 为什么要重试循环：本内核的 send 在窗口满时返回 -EAGAIN（Linux 语义），
 *   而 mbedTLS 的 BIO 契约里"发出去 n 字节"和"一点都发不出去"是两种返回值。 */
static int bio_send(void* ctx, const unsigned char* buf, size_t len)
{
    struct Bio* b = (struct Bio*)ctx;
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(b->fd, buf + off, len - off, 0);
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
    b->tx_bytes += (long)off;
    return (int)off;
}

/* 返回读到的字节数；0 = 对端关闭（mbedTLS 会当成 close_notify/EOF）；
 * -EAGAIN 时返回 WANT_READ 让上层重试（**不要**在这里睡死，否则握手里没法做超时）。 */
static int bio_recv(void* ctx, unsigned char* buf, size_t len)
{
    struct Bio* b = (struct Bio*)ctx;
    for (;;) {
        ssize_t n = recv(b->fd, buf, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
            return MBEDTLS_ERR_NET_RECV_FAILED;
        }
        b->rx_bytes += (long)n;
        return (int)n;
    }
}

/* ==================== 一次完整的 TLS 取回 ==================== */
struct TlsResult {
    int  ok;                  /* 握手 + 校验都过 */
    int  ret;                 /* 最后一步的 mbedTLS 错误码 */
    char version[32];
    char suite[96];
    uint32_t verify_flags;
    char subject[256], issuer[256];
    char not_before[24], not_after[24];
    char status_line[128];
    long body_bytes;
};

static void x509_time_str(const mbedtls_x509_time* t, char* out, size_t n)
{
    snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02d", t->year, t->mon, t->day, t->hour, t->min, t->sec);
}

static int tcp_connect_ip(const char* ip, int port, char* errbuf, size_t en)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
        snprintf(errbuf, en, "bad-ip");
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { snprintf(errbuf, en, "socket errno=%d", errno); return -1; }
    int rc = connect(fd, (struct sockaddr*)&sa, sizeof sa);
    if (rc != 0) {
        snprintf(errbuf, en, "connect errno=%d", errno);
        close(fd);
        return -1;
    }
    return fd;
}

/* expect_fail=1：这是负向对照，握手**必须**失败（且失败原因要是证书校验）。
 * 返回 0 = 符合预期。 */
static int tls_fetch(const struct ProbeCfg* cfg, const char* label,
                     const char* host_or_ip, int port, int use_dns,
                     mbedtls_x509_crt* cacert, mbedtls_ctr_drbg_context* drbg,
                     struct TlsResult* out, int expect_fail)
{
    memset(out, 0, sizeof *out);
    out->ret = 0x7FFF;

    char ip[64] = {0};
    if (use_dns) {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        const int gr = getaddrinfo(host_or_ip, NULL, &hints, &res);
        if (gr != 0 || !res) {
            say("[TLS] %s dns FAIL host=%s gai=%d", label, host_or_ip, gr);
            out->ret = -1000 - gr;
            return -1;
        }
        struct sockaddr_in* sin = (struct sockaddr_in*)res->ai_addr;
        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip);
        freeaddrinfo(res);
        say("[TLS] %s dns host=%s ip=%s", label, host_or_ip, ip);
    } else {
        snprintf(ip, sizeof ip, "%s", host_or_ip);
        say("[TLS] %s dns skipped (literal addr=%s)", label, ip);
    }

    char ebuf[96] = {0};
    const int fd = tcp_connect_ip(ip, port, ebuf, sizeof ebuf);
    if (fd < 0) {
        say("[TLS] %s tcp FAIL ip=%s:%d %s", label, ip, port, ebuf);
        out->ret = -2000;
        return -1;
    }
    say("[TLS] %s tcp connected ip=%s:%d", label, ip, port);

    static mbedtls_ssl_context ssl;              /* 静态：16 KiB 的用户栈放不下这两个大对象 */
    static mbedtls_ssl_config  conf;
    static struct Bio         bio;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    bio.fd = fd; bio.rx_bytes = bio.tx_bytes = 0;

    int ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) { say("[TLS] %s config_defaults ret=-0x%04x", label, (unsigned)-ret); out->ret = ret; goto done; }

    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, drbg);
    if (cacert != NULL && cacert->next != NULL) {
        mbedtls_ssl_conf_ca_chain(&conf, cacert, NULL);
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        /* 没有 CA 包：**不许**假装校验证书。降级必须大声打点，并且在结果里体现出来。 */
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
        say("[TLS] %s WARN no-ca -> authmode=NONE (certificate NOT verified)", label);
    }

    ret = mbedtls_ssl_setup(&ssl, &conf);
    if (ret != 0) { say("[TLS] %s ssl_setup ret=-0x%04x", label, (unsigned)-ret); out->ret = ret; goto done; }
    ret = mbedtls_ssl_set_hostname(&ssl, host_or_ip);
    if (ret != 0) { say("[TLS] %s set_hostname ret=-0x%04x", label, (unsigned)-ret); out->ret = ret; goto done; }
    mbedtls_ssl_set_bio(&ssl, &bio, bio_send, bio_recv, NULL);

    {   /* 握手：WANT_READ/WANT_WRITE 要重试；同时给一个**有界**的总时长（本内核 recv 会 -EAGAIN）*/
        const time_t t0 = time(NULL);
        int tries = 0;
        do {
            ret = mbedtls_ssl_handshake(&ssl);
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (time(NULL) - t0 > 60) { ret = MBEDTLS_ERR_SSL_TIMEOUT; break; }
                if (++tries > 400000) { ret = MBEDTLS_ERR_SSL_TIMEOUT; break; }
                continue;
            }
            break;
        } while (1);
    }
    out->ret = ret;
    if (ret != 0) {
        char eb[128] = {0};
        mbedtls_strerror(ret, eb, sizeof eb);
        out->verify_flags = mbedtls_ssl_get_verify_result(&ssl);
        say("[TLS] %s handshake FAILED ret=-0x%04x (%s) tx=%ld rx=%ld verify_flags=0x%08x",
            label, (unsigned)-ret, eb, bio.tx_bytes, bio.rx_bytes, (unsigned)out->verify_flags);
        if (out->verify_flags) {
            char vi[384] = {0};
            mbedtls_x509_crt_verify_info(vi, sizeof vi, "  ", out->verify_flags);
            say("[TLS] %s verify-info:%s", label, vi);
        }
        goto done;
    }

    snprintf(out->version, sizeof out->version, "%s", mbedtls_ssl_get_version(&ssl));
    snprintf(out->suite, sizeof out->suite, "%s", mbedtls_ssl_get_ciphersuite(&ssl));
    out->verify_flags = mbedtls_ssl_get_verify_result(&ssl);
    say("[TLS] %s handshake ok version=%s suite=%s tx=%ld rx=%ld",
        label, out->version, out->suite, bio.tx_bytes, bio.rx_bytes);

    {
        const mbedtls_x509_crt* peer = mbedtls_ssl_get_peer_cert(&ssl);
        if (peer) {
            mbedtls_x509_dn_gets(out->subject, sizeof out->subject, &peer->subject);
            mbedtls_x509_dn_gets(out->issuer, sizeof out->issuer, &peer->issuer);
            x509_time_str(&peer->valid_from, out->not_before, sizeof out->not_before);
            x509_time_str(&peer->valid_to, out->not_after, sizeof out->not_after);
            say("[TLS] %s cert subject=%s", label, out->subject);
            say("[TLS] %s cert issuer=%s", label, out->issuer);
            say("[TLS] %s cert valid %s .. %s verify_flags=0x%08x",
                label, out->not_before, out->not_after, (unsigned)out->verify_flags);
            say("[TLS] %s cert serial.len=%u", label, (unsigned)peer->serial.len);
        } else {
            say("[TLS] %s cert NONE (server sent no certificate)", label);
        }
    }
    if (out->verify_flags != 0) {
        /* 校验没过却握手成功：只可能出现在 authmode=NONE。如实记，不许算通过。 */
        say("[TLS] %s WARN handshake ok but verify_flags=0x%08x (NOT verified)",
            label, (unsigned)out->verify_flags);
        out->ok = 0;
        ret = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
        out->ret = ret;
        goto done;
    }

    {   /* HTTP/1.1 GET（Connection: close -> 读到 EOF 为止） */
        char req[512];
        const int rn = snprintf(req, sizeof req,
            "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: VimtuOS-httpsprobe/0.12.4\r\n"
            "Accept: */*\r\nConnection: close\r\n\r\n", cfg->path, host_or_ip);
        size_t sent = 0;
        while (sent < (size_t)rn) {
            const int w = mbedtls_ssl_write(&ssl, (const unsigned char*)req + sent, (size_t)rn - sent);
            if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (w <= 0) {
                char eb[128] = {0}; mbedtls_strerror(w, eb, sizeof eb);
                say("[TLS] %s request write FAILED ret=-0x%04x (%s)", label, (unsigned)-w, eb);
                out->ret = w; goto done;
            }
            sent += (size_t)w;
        }
        say("[TLS] %s GET %s sent=%u", label, cfg->path, (unsigned)sent);

        char resp[2048];
        size_t have = 0;
        long total = 0;
        int body_seen = 0;
        const time_t t0 = time(NULL);
        for (;;) {
            unsigned char chunk[1024];
            const int r = mbedtls_ssl_read(&ssl, chunk, sizeof chunk);
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (time(NULL) - t0 > 60) { say("[TLS] %s read TIMEOUT after %ld B", label, total); break; }
                continue;
            }
            if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
            if (r < 0) {
                char eb[128] = {0}; mbedtls_strerror(r, eb, sizeof eb);
                say("[TLS] %s read FAILED ret=-0x%04x (%s) after %ld B", label, (unsigned)-r, eb, total);
                out->ret = r;
                break;
            }
            if (have < sizeof resp - 1) {
                size_t cp = (size_t)r;
                if (cp > sizeof resp - 1 - have) cp = sizeof resp - 1 - have;
                memcpy(resp + have, chunk, cp);
                have += cp;
                resp[have] = 0;
            }
            /* 正文起点：第一个 \r\n\r\n 之后（只为了数正文字节，不做完整 HTTP 解析） */
            if (!body_seen) {
                char* hdr_end = strstr(resp, "\r\n\r\n");
                if (hdr_end) { body_seen = 1; total = (long)r; }
            } else {
                total += r;
            }
        }
        out->body_bytes = body_seen ? total : 0;
        if (have >= 5 && !strncmp(resp, "HTTP/", 5)) {
            char* e = strstr(resp, "\r\n");
            size_t sl = e ? (size_t)(e - resp) : have;
            if (sl > sizeof out->status_line - 1) sl = sizeof out->status_line - 1;
            memcpy(out->status_line, resp, sl);
            out->status_line[sl] = 0;
            say("[TLS] %s http status=\"%s\" body_bytes=%ld", label, out->status_line, out->body_bytes);
        } else {
            say("[TLS] %s http NO status line (first bytes not HTTP/) have=%u", label, (unsigned)have);
        }
        out->ok = (have >= 12 && !strncmp(resp, "HTTP/1.", 7));
        out->ret = 0;
    }

done:
    mbedtls_ssl_close_notify(&ssl);       /* 尽力而为；失败无所谓（对端可能已关） */
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    close(fd);
    (void)expect_fail;
    return 0;
}

/* main 的定义在下面（探针主体），_start 那一段要先看到它的原型 */
int main(void);

/* ==================== 启动（自写 _start，与 user/apps/muslhello.c 同一条纪律）====================
 * 为什么不用 musl 的 crt1.o：crt_arch.h 里有 `lea _DYNAMIC(%rip),%rsi`，本映像静态链接在
 *   4GiB+576KiB，_DYNAMIC 无处解析 -> 链接期 R_X86_64_PC32 out of range。
 *   v0.12.4 实测现象：`ld.lld: warning: cannot find entry symbol _start`，产物 8,056 B、entry=0
 *   （只有两个 .o 被链进去，libc.a 一个成员都没拉）。
 *   所以自己写 _start：把内核摆好的 SysV 初始栈（argc/argv/envp/auxv）交给 musl 的
 *   __libc_start_main。rdi = 初始 SP、rsp 16 字节对齐后 call —— 与 musl 的 crt_arch.h 口径一致。 */
extern int __libc_start_main(int (*main_fn)(int, char**, char**), int argc, char** argv,
                             void (*init_dummy)(void), void (*fini_dummy)(void),
                             void (*ldso_dummy)(void));
static int probe_main(int argc, char** argv, char** envp);
static uint64_t* g_probe_sp;                  /* _start 记下初始 SP，普通函数里再解引用 */

__attribute__((naked, noreturn, used, section(".text.start")))
void _start(void) {
    __asm__ volatile(
        "xor %rbp, %rbp\n\t"
        "mov %rsp, %rdi\n\t"
        "and $-16, %rsp\n\t"
        "call probe_start_c\n\t"
        "hlt\n\t");
}
__attribute__((used, noinline))
static void probe_start_c(uint64_t* sp) {
    g_probe_sp = sp;
    const int argc = (int)sp[0];
    char** argv = (char**)(sp + 1);
    __libc_start_main(probe_main, argc, argv, 0, 0, 0);
    _exit(127);                               /* __libc_start_main 不返回；兜底也不掉回 hlt */
}
static int probe_main(int argc, char** argv, char** envp)
{
    (void)argc; (void)argv; (void)envp;
    return main();
}
int main(void)
{
    struct ProbeCfg cfg;
    cfg_defaults(&cfg);
    cfg_load(&cfg);

    say("[TLS] ===== VimtuOS HTTPS probe start (mbedTLS %s) =====", MBEDTLS_VERSION_STRING);

    /* ---- ① 熵：这是"能不能做 TLS"的地基，先自证 ---- */
    {
        unsigned char a[32], b[32];
        const unsigned f = vimtu_mbedtls_hwpoll_selftest(a, b, sizeof a);
        say("[TLS] entropy hw-poll selftest=%s a=%s b=%s differ=%d len0_ok=%d nonzero=%d",
            f == 0 ? "PASS" : "FAIL", hexdup(a, 16), hexdup(b, 16),
            (f & 1u) ? 0 : 1, (f & 2u) ? 0 : 1, (f & 4u) ? 0 : 1);
        if (f != 0) { say("[TLS] RESULT positive=fail negative=skip reason=entropy"); return 2; }
    }

    /* ---- ② 时间：证书有效期判定要用真墙钟（RTC 锚点）---- */
    {
        const time_t now = time(NULL);
        struct tm tmv;
        char iso[40] = "?";
        if (gmtime_r(&now, &tmv)) {
            snprintf(iso, sizeof iso, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        }
        const int sane = (now > 1577836800LL /* 2020-01-01 */ && now < 2524608000LL /* 2050 */);
        say("[TLS] time now=%ld utc=%s src=%s", (long)now, iso, sane ? "rtc" : "UNREADABLE/UNTRUSTED");
        if (!sane) {
            say("[TLS] WARN wall clock not trustworthy -> certificate validity CANNOT be verified");
        }
    }

    /* ---- ③ DRBG（种子来自上面的硬件熵）---- */
    static mbedtls_entropy_context entropy;
    static mbedtls_ctr_drbg_context drbg;
    static mbedtls_x509_crt cacert;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509_crt_init(&cacert);
    {
        static const char pers[] = "vimtu-https-probe";
        const int r = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                            (const unsigned char*)pers, sizeof pers - 1);
        if (r != 0) {
            char eb[128] = {0}; mbedtls_strerror(r, eb, sizeof eb);
            say("[TLS] drbg_seed FAILED ret=-0x%04x (%s)", (unsigned)-r, eb);
            say("[TLS] RESULT positive=fail negative=skip reason=drbg");
            return 2;
        }
        unsigned char rnd[16];
        mbedtls_ctr_drbg_random(&drbg, rnd, sizeof rnd);
        say("[TLS] drbg seeded random=%s", hexdup(rnd, sizeof rnd));
    }

    /* ---- ④ CA 包 ---- */
    {
        unsigned char* ca = NULL; size_t can = 0;
        const int rr = read_file(cfg.ca, &ca, &can, 512 * 1024);
        if (rr != 0) {
            say("[TLS] ca read FAIL path=%s err=%d -> verification impossible (negative control only)", cfg.ca, rr);
        } else {
            /* ★ v0.12.4 诊断（决定性判据，不是"再看一眼"）：把读到的整块 CA 包做一次 FNV-1a64，
             *   宿主侧可以对 build64/ca.pem 算同一个值逐位比对。这样"PEM 解析失败"能立刻分清是
             *     ① 客人读到**内容不对**（VFS 二级间接读大文件出错 / 截断）——FNV 与宿主不一致；
             *     ② 内容对、mbedTLS 解析不认（配置或库的问题）——FNV 与宿主一致。
             *   没有这一行，两种原因在串口上长得一模一样（都只是 parse_ret=-8576）。 */
            {
                uint64_t fnv = 0xCBF29CE484222325ULL;
                for (size_t i = 0; i < can; i++) { fnv ^= (uint64_t)ca[i]; fnv *= 0x100000001B3ULL; }
                char head[25];
                for (int i = 0; i < 24; i++) head[i] = (i < (int)can && ca[i] >= 0x20 && ca[i] < 0x7F)
                                                     ? (char)ca[i] : '.';
                head[24] = 0;
                say("[TLS] ca fnv1a=%016llx first24=\"%s\"",
                    (unsigned long long)fnv, head);
            }
            int parsed = 0;
            /* PEM 包里有一百多份证书：mbedtls_x509_crt_parse 会一路解析下去 */
            /* ★ v0.12.4（真坑，主机侧复现 + 逐位证据）：
             *   `mbedtls_x509_crt_parse()` 用
             *     `buf[buflen-1] == '\0' && strstr(buf, "-----BEGIN CERTIFICATE-----")`
             *   来判断"这是 PEM 包"。**buflen 必须把结尾那个 NUL 算进去**，
             *   否则它会走 DER 分支、拿 200 KB 的 PEM 文本当 DER 解 -> 每个证书都
             *   `X509 - The CRT/CRL/CSR format is invalid` (-0x2180)，certs=0。
             *   主机侧对照实验（同一份 ca.pem、mbedTLS 3.6.2 默认配置）：
             *     parse(buflen=200116)      ret=-8576 certs=0
             *     parse(buflen=200116+1 NUL) ret=0     certs=117
             *   所以这里传 can + 1（read_file 已在 buf[got] 写了 0）。 */
            const int pr = mbedtls_x509_crt_parse(&cacert, ca, can + 1);
            for (const mbedtls_x509_crt* c = &cacert; c; c = c->next) parsed++;
            say("[TLS] ca path=%s bytes=%u parse_ret=%d certs=%d", cfg.ca, (unsigned)can, pr, parsed - 1);
            free(ca);
            if (parsed <= 1) say("[TLS] WARN no usable CA certificate -> authmode will be NONE");
        }
    }

    /* ---- ⑤ 正面目标：真站点 ---- */
    struct TlsResult pos, neg;
    tls_fetch(&cfg, "pos", cfg.host, cfg.port, 1, &cacert, &drbg, &pos, 0);
    const int pos_ok = (pos.ok && pos.ret == 0);
    say("[TLS] RESULT positive=%s", pos_ok ? "pass" : "fail");

    /* ---- ⑥ 负向对照：错证书必须失败 ---- */
    int neg_ok = 0, neg_ran = 0;
    if (cfg.negaddr[0] == 0 || cfg.negskip) {
        say("[TLS] negative SKIPPED (negaddr empty or negskip=1) -- 不许当成通过");
    } else {
        neg_ran = 1;
        tls_fetch(&cfg, "neg", cfg.negaddr, cfg.negport, 0, &cacert, &drbg, &neg, 1);
        const int verify_failed = (neg.ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) ||
                                  (neg.ret == MBEDTLS_ERR_SSL_BAD_CERTIFICATE) ||
                                  (neg.ret == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE && neg.verify_flags != 0);
        neg_ok = (neg.ok == 0) && (neg.ret != 0) && (verify_failed || neg.verify_flags != 0);
        say("[TLS] RESULT negative=%s (ret=-0x%04x verify_flags=0x%08x) expected=cert-verify-failed",
            neg_ok ? "pass" : "fail", (unsigned)-neg.ret, (unsigned)neg.verify_flags);
    }

    say("[TLS] ===== probe done positive=%s negative=%s =====",
        pos_ok ? "pass" : "fail",
        neg_ran ? (neg_ok ? "pass" : "fail") : "skip");

    mbedtls_x509_crt_free(&cacert);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    if (!pos_ok) return 1;
    if (neg_ran && !neg_ok) return 1;
    return 0;
}
