// kernel/sock64.cpp - ★ v0.7.4（P2 第三批）：Linux 兼容的 socket 层（AF_INET）
//
// 目标：让真实 Linux 程序（apk 下载包、curl）能**连出去**。所以只做客户端：
//   TCP：connect / send / recv / close（三次握手 + PSH/ACK 收发 + FIN）
//   UDP：sendto / recvfrom（无连接）
// 没有做的（如实）：listen/accept（服务端）、SOCK_RAW、IPv6、TCP 选项协商、拥塞控制、窗口缩放。
//   TCP 的重传只做"SYN 超时重发一次"，数据段不等 ACK —— 这是**局域网/slirp 级可用**的实现。
//
// 依赖：kernel/net64.h 的 send_ipv4（内部已含 ARP 解析与以太网发送）；
//       上行帧由 net64 的收包路径回调 sock64_rx64。
#include "sock64.h"
#include "net64.h"
#include "debug64.h"
#include "panic64.h"

extern volatile uint64_t g_ticks64;
#ifndef PIT_HZ_64
#define PIT_HZ_64 250u
#endif
extern "C" void task_sleep_ms64(uint32_t ms) __attribute__((weak));

#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_PSH 0x08u
#define TCP_ACK 0x10u

enum { S_FREE = 0, S_CREATED = 1, S_SYN_SENT = 2, S_ESTAB = 3, S_FIN_SENT = 4, S_CLOSED = 5 };

struct Sock64 {
    uint8_t  used;
    uint8_t  type;                 // SOCK64_T_TCP / _UDP
    uint8_t  state;                // S_*
    uint8_t  pad;
    uint32_t laddr;                // 本机 IP（网络序里的整数，与 net64 一致）
    uint16_t lport;
    uint16_t rport;
    uint32_t raddr;
    uint32_t iss;                  // 初始序号
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
    int32_t  rxlen;
    uint8_t  rx[SOCK64_RXBUF];
};

static Sock64 g_socks[SOCK64_MAX];
static uint64_t g_tx_seg = 0, g_rx_seg = 0;

static void s64_sleep_ms(uint32_t ms) {
    if (task_sleep_ms64) task_sleep_ms64(ms);
    else {
        const uint64_t until = g_ticks64 + (uint64_t)(ms * PIT_HZ_64 / 1000u) + 1;
        uint64_t g = 0;
        while (g_ticks64 < until && ++g < 2000000u) __asm__ volatile("hlt");
    }
}

static uint32_t s64_rd32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void s64_wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint16_t s64_rd16(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static void s64_wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

// HTONL/HTONS：net64 的 IP 是"网络序整数"，端口在这层统一用**主机序**，打包时再转
static uint16_t s64_htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

// 16 位反码和（返回 CPU 序，写入前要 htons）
static uint32_t s64_sum16(const uint8_t* p, int len, uint32_t acc) {
    int i = 0;
    for (; i + 1 < len; i += 2) acc += ((uint32_t)p[i] << 8) | p[i + 1];
    if (i < len) acc += ((uint32_t)p[i] << 8);
    return acc;
}
static uint16_t s64_fold(uint32_t acc) {
    while (acc >> 16) acc = (acc & 0xFFFFu) + (acc >> 16);
    return (uint16_t)(~acc & 0xFFFFu);
}

static Sock64* s64_of(int h) {
    const int i = h - SOCK64_FD_BASE;
    if (i < 0 || i >= SOCK64_MAX) return nullptr;
    if (!g_socks[i].used) return nullptr;
    return &g_socks[i];
}

// 组 TCP 段并通过 net64 发出去（payload 可为空）
static int s64_tcp_send(Sock64* s, uint8_t flags, const uint8_t* payload, int len) {
    uint8_t seg[40 + 1460];
    if (len > 1460) len = 1460;
    for (int i = 0; i < 20; i++) seg[i] = 0;
    s64_wr16(seg + 0, s64_htons(s->lport));
    s64_wr16(seg + 2, s64_htons(s->rport));
    s64_wr32(seg + 4, s->snd_nxt);
    s64_wr32(seg + 8, (flags & TCP_ACK) ? s->rcv_nxt : 0u);
    seg[12] = (uint8_t)(5u << 4);                    // data offset = 5（无选项）
    seg[13] = flags;
    s64_wr16(seg + 14, s64_htons(4096));              // window
    s64_wr16(seg + 16, 0);                            // checksum（先 0）
    s64_wr16(seg + 18, 0);
    if (len > 0) for (int i = 0; i < len; i++) seg[20 + i] = payload[i];

    // 伪首部：src, dst, 0, proto=6, tcp_len
    uint32_t acc = 0;
    acc = s64_sum16((const uint8_t*)&s->laddr, 4, acc);
    acc = s64_sum16((const uint8_t*)&s->raddr, 4, acc);
    acc += 6u;
    acc += (uint32_t)(20 + len);
    acc = s64_sum16(seg, 20 + len, acc);
    s64_wr16(seg + 16, s64_fold(acc));

    const int rc = net64_send_ipv4(s->raddr, 6, seg, 20 + len);
    if (rc == 0) {
        s->snd_nxt += (uint32_t)((flags & (TCP_SYN | TCP_FIN)) ? 1u : 0u) + (uint32_t)len;
        g_tx_seg++;
    }
    return rc;
}

int sock64_create64(int type) {
    if (type != SOCK64_T_TCP && type != SOCK64_T_UDP) return -1;
    for (int i = 0; i < SOCK64_MAX; i++) {
        if (g_socks[i].used) continue;
        Sock64* s = &g_socks[i];
        for (uint32_t k = 0; k < sizeof(Sock64); k++) ((uint8_t*)s)[k] = 0;
        s->used = 1;
        s->type = (uint8_t)type;
        s->state = S_CREATED;
        s->lport = (uint16_t)(40000 + (g_ticks64 & 0x3FFu) + (uint16_t)i);   // 临时端口
        return SOCK64_FD_BASE + i;
    }
    return -1;
}

int sock64_close64(int h) {
    Sock64* s = s64_of(h);
    if (!s) return -1;
    if (s->type == SOCK64_T_TCP && s->state == S_ESTAB) {
        (void)s64_tcp_send(s, (uint8_t)(TCP_FIN | TCP_ACK), nullptr, 0);
        s->state = S_FIN_SENT;
    }
    s->used = 0;
    return 0;
}

// TCP 三次握手（有界等待）
int sock64_connect64(int h, uint32_t ip, uint16_t port, uint32_t timeout_ms) {
    Sock64* s = s64_of(h);
    if (!s || s->type != SOCK64_T_TCP) return -1;
    if (s->state != S_CREATED) return -1;
    s->raddr = ip;
    s->rport = port;
    s->iss = g_ticks64 * 4u + 1u;
    s->snd_nxt = s->iss;
    s->rcv_nxt = 0;
    if (s64_tcp_send(s, TCP_SYN, nullptr, 0) != 0) return -1;
    s->state = S_SYN_SENT;
    const uint64_t t0 = g_ticks64;
    const uint64_t lim = (uint64_t)((timeout_ms ? timeout_ms : 3000u) * PIT_HZ_64 / 1000u);
    uint32_t last_retry = (uint32_t)g_ticks64;
    while ((g_ticks64 - t0) < lim) {
        net64_poll64();
        if (s->state == S_ESTAB) return 0;
        if (s->state == S_CLOSED) return -1;                 // 收到 RST
        if ((uint32_t)g_ticks64 - last_retry > PIT_HZ_64) {  // 1s 重发一次 SYN
            last_retry = (uint32_t)g_ticks64;
            s->snd_nxt = s->iss;
            (void)s64_tcp_send(s, TCP_SYN, nullptr, 0);
        }
        s64_sleep_ms(1);
    }
    s->state = S_CLOSED;
    return -2;                                               // 超时
}

void sock64_sendto_target64(int h, uint32_t ip, uint16_t port) {
    Sock64* s = s64_of(h);
    if (!s) return;
    s->raddr = ip;
    s->rport = port;
}

int sock64_send64(int h, const void* buf, int len) {
    Sock64* s = s64_of(h);
    if (!s || !buf || len < 0) return -1;
    if (s->type == SOCK64_T_UDP) {
        const int rc = net64_send_ipv4(s->raddr, 17, (const uint8_t*)buf, len);
        if (rc == 0) g_tx_seg++;
        return rc == 0 ? len : -1;
    }
    if (s->state != S_ESTAB) return -1;
    int sent = 0;
    while (sent < len) {
        const int chunk = (len - sent > 1460) ? 1460 : (len - sent);
        if (s64_tcp_send(s, (uint8_t)(TCP_PSH | TCP_ACK), (const uint8_t*)buf + sent, chunk) != 0) break;
        sent += chunk;
    }
    return sent;
}

int sock64_recv64(int h, void* buf, int max, uint32_t timeout_ms) {
    Sock64* s = s64_of(h);
    if (!s || !buf || max <= 0) return -1;
    const uint64_t t0 = g_ticks64;
    const uint64_t lim = (uint64_t)((timeout_ms ? timeout_ms : 3000u) * PIT_HZ_64 / 1000u);
    for (;;) {
        if (s->rxlen > 0) {
            int n = s->rxlen;
            if (n > max) n = max;
            for (int i = 0; i < n; i++) ((uint8_t*)buf)[i] = s->rx[i];
            for (int i = n; i < s->rxlen; i++) s->rx[i - n] = s->rx[i];
            s->rxlen -= n;
            return n;
        }
        if (s->type == SOCK64_T_TCP && (s->state == S_CLOSED || s->state == S_FIN_SENT)) return 0;   // EOF
        if ((g_ticks64 - t0) >= lim) return 0;               // 超时（0 字节，不假装有数据）
        net64_poll64();
        s64_sleep_ms(1);
    }
}

int sock64_getname64(int h, uint32_t* ip, uint16_t* port, int peer) {
    Sock64* s = s64_of(h);
    if (!s) return -1;
    if (ip) *ip = peer ? s->raddr : s->laddr;
    if (port) *port = peer ? s->rport : s->lport;
    return 0;
}

// ---------------- 上行分发（net64 收包回调）----------------
void sock64_rx64(uint8_t proto, uint32_t src_ip, const uint8_t* p, int len) {
    if (!p || len <= 0) return;
    g_rx_seg++;
    if (proto == 17) {                                        // UDP
        if (len < 8) return;
        const uint16_t lport = s64_htons(s64_rd16(p + 2));
        for (int i = 0; i < SOCK64_MAX; i++) {
            Sock64* s = &g_socks[i];
            if (!s->used || s->type != SOCK64_T_UDP) continue;
            if (s->lport != lport) continue;
            s->raddr = src_ip;
            s->rport = s64_htons(s64_rd16(p + 0));
            const int dlen = len - 8;
            for (int k = 0; k < dlen && s->rxlen < SOCK64_RXBUF; k++) s->rx[s->rxlen++] = p[8 + k];
            return;
        }
        return;
    }
    if (proto != 6) return;                                   // 其它协议不管
    if (len < 20) return;
    const uint16_t lport = s64_htons(s64_rd16(p + 2));
    const uint16_t rport = s64_htons(s64_rd16(p + 0));
    const uint32_t seq = s64_rd32(p + 4);
    const uint8_t flags = p[13];
    const int doff = (int)(p[12] >> 4) * 4;
    const int dlen = len - doff;
    for (int i = 0; i < SOCK64_MAX; i++) {
        Sock64* s = &g_socks[i];
        if (!s->used || s->type != SOCK64_T_TCP) continue;
        if (s->lport != lport || s->raddr != src_ip || s->rport != rport) continue;
        if (flags & TCP_RST) { s->state = S_CLOSED; return; }
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && s->state == S_SYN_SENT) {
            s->rcv_nxt = seq + 1;
            s->snd_nxt = s->iss + 1;
            (void)s64_tcp_send(s, TCP_ACK, nullptr, 0);       // 第三次握手
            s->state = S_ESTAB;
            return;
        }
        if (dlen > 0) {                                       // 数据段
            for (int k = 0; k < dlen && s->rxlen < SOCK64_RXBUF; k++) s->rx[s->rxlen++] = p[doff + k];
            s->rcv_nxt = seq + (uint32_t)dlen;
            (void)s64_tcp_send(s, TCP_ACK, nullptr, 0);
        }
        if (flags & TCP_FIN) {
            s->rcv_nxt += 1;
            (void)s64_tcp_send(s, TCP_ACK, nullptr, 0);
            s->state = S_CLOSED;
        }
        return;
    }
}

uint64_t sock64_tx_segments64() { return g_tx_seg; }
uint64_t sock64_rx_segments64() { return g_rx_seg; }

// ---------------- 纯逻辑自检（不发包，可在 fake 路径跑）----------------
// 校验：handle 编解码、状态机初值、TCP 校验和算法（用一个已知向量）
int sock64_selftest64(void) {
    bool ok = true;
    const int h = sock64_create64(SOCK64_T_TCP);
    if (h != SOCK64_FD_BASE + 0) ok = false;
    Sock64* s = h >= 0 ? s64_of(h) : nullptr;
    if (!s || s->state != S_CREATED || s->type != SOCK64_T_TCP) ok = false;
    if (sock64_create64(99) >= 0) ok = false;                 // 非法类型必须拒绝
    // 校验和：把 [00 01] 的 16 位和取反，应为 0xFFFE
    uint8_t probe[2] = { 0x00, 0x01 };
    if (s64_fold(s64_sum16(probe, 2, 0)) != 0xFFFEu) ok = false;
    if (h >= 0) (void)sock64_close64(h);
    dbg64_line_begin64();
    dbg64_str("[SOCK64] selftest ");
    dbg64_str(ok ? "PASS" : "FAIL");
    dbg64_str(" (client-only: TCP connect/send/recv/close + UDP; no listen/accept)");
    dbg64_nl();
    dbg64_line_end64();
    return ok ? 0 : -1;
}
