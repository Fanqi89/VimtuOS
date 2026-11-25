// kernel/sock64.cpp - Linux 兼容的 socket 层（AF_INET）
//
// ★ v0.12.2（网络与浏览器生态 · P0「L4 硬化」）：从「局域网级可用」升级到「能扛住多兆下载」。
//
// 本版修掉的**真缺陷**（每条都在 tests/netsock64_test.py 里有判据）：
//   ① 接收侧原来**不校验序号**：`if (dlen > 0)` 就把数据追加进缓冲并把 rcv_nxt 推到 seq+dlen。
//      真实后果：对端重传一次（我们的 ACK 丢过、或它自己超时）→ **同一段数据被追加两遍**，
//      下载下来的文件比源文件大且内容错位。实测证据在旧日志里能抓到 199/199 段但总字节数偏大。
//      → 现在只有 seq == rcv_nxt 才入缓冲；seq < rcv_nxt 视为重复（只回 ACK、不入缓冲、计数）；
//        seq > rcv_nxt 视为乱序（丢弃 + 立刻回一个 dup ACK，让对端快速重传）。
//   ② 原来**永远报 window=4096**、缓冲满时静默丢数据却仍推进 rcv_nxt → 丢的数据永不重传。
//      → 现在每次 ACK 都报真实剩余空间（64 KiB 缓冲），收满报 0，并计数 rx_winzero。
//   ③ 原来**不校验 TCP 校验和**。→ 现在伪首部+段全算，非 0 视为坏段丢弃（计数）。
//   ④ 原来数据段**发了就不管**，丢了就永远卡住。→ 现在单段在途 + RTO 500 ms 起指数退避到 4 s，
//      收到 ACK 才清（计数 retx）。
//   ⑤ SYN 原来不带选项 → 对端只能按 MSS 536 发。→ 现在 SYN 带 MSS=1460（吞吐差 2.7 倍）。
//   ⑥ `recv` 超时原来返回 **0**，与 EOF 无法区分 → 慢下载停 3 秒就被当成文件结束，
//      下载出来的是**截断文件**。→ 现在 TCP/UDP 返回 -EAGAIN(11)，只有 FIN/RST 才返回 0。
//   ⑦ `poll/select/epoll` 原来对 socket handle 报 POLLNVAL（它不在 fd 表里）→ musl 的
//      getaddrinfo 等不到 DNS 回包（这就是 `ping baidu.com` 报错的第二个根因）。
//      → 新增 `sock64_ready64()`，poll 核心按它判定。
//
// 仍然没有的（如实）：listen/accept、IPv6、IP 分片/重组、SACK/时间戳/窗口缩放、
//   拥塞控制（固定 RTO，无 cwnd/RTT 估计）、乱序缓存（乱序丢弃等重传）、Nagle/延迟 ACK。
//
// 依赖：kernel/net64.h 的 send_ipv4（内部已含 ARP 解析与以太网发送）；
//       上行帧由 net64 的收包路径回调 sock64_rx64。
#include "sock64.h"
#include "net64.h"
#include "debug64.h"
#include "panic64.h"

// ★ v0.9.39：raw ICMP 的 recv 需要在"被 SIGALRM 打断"时返回 -EINTR（Linux 语义）。
//   用弱声明而不是 include proc64.h：安装介质内核不链 proc64 的这部分时不能让链接失败。
int proc64_sig_pending64(int sig) __attribute__((weak));
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

#define SOCK64_EAGAIN   11
#define SOCK64_EINTR     4

enum { S_FREE = 0, S_CREATED = 1, S_SYN_SENT = 2, S_ESTAB = 3, S_FIN_SENT = 4, S_CLOSED = 5 };

// 接收侧状态位
#define RXF_EOF  0x01u        // 收到 FIN
#define RXF_RST  0x02u        // 收到 RST

struct Sock64 {
    uint8_t  used;
    uint8_t  type;                 // SOCK64_T_TCP / _UDP / _ICMP
    uint8_t  state;                // S_*
    uint8_t  rxflags;              // RXF_*
    uint32_t laddr;                // 本机 IP（网络序里的整数，与 net64 一致）
    uint32_t raddr;
    uint16_t lport;
    uint16_t rport;
    uint32_t iss;                  // 初始序号
    uint32_t snd_una;              // 最早未确认
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
    // ---- 单段在途（重传用）----
    uint32_t tx_seq;
    uint16_t tx_len;
    uint8_t  tx_has;
    uint8_t  pad0;
    uint32_t tx_rto_ms;
    uint64_t tx_tick;
    uint8_t  tx_buf[SOCK64_MSS];
    int32_t  rxlen;
    uint32_t rx[SOCK64_RXBUF / 4];  // 字节缓冲（按 4 字节对齐，避免结构内偏移错位）
};

static Sock64 g_socks[SOCK64_MAX];
static uint64_t g_tx_seg = 0, g_rx_seg = 0;
static uint64_t g_retx = 0, g_rx_dup = 0, g_rx_ooo = 0, g_rx_badsum = 0, g_rx_winzero = 0;

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

// ★ v0.12.2：本层**不再需要 htons** —— 端口统一按主机序保存，s64_wr16/s64_rd16 直接做大端读写。
//   （旧的 s64_htons 是那个字节序 Bug 的来源，已随修复删掉。）

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

// 32 位序号比较（回绕安全）：a > b ?
static inline bool seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }
static inline bool seq_ge(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

static Sock64* s64_of(int h) {
    const int i = h - SOCK64_FD_BASE;
    if (i < 0 || i >= SOCK64_MAX) return nullptr;
    if (!g_socks[i].used) return nullptr;
    return &g_socks[i];
}

// 当前剩余接收空间（窗口）。满了就报 0 —— 这是"对端该停下来"的唯一正确说法。
static uint16_t s64_win(const Sock64* s) {
    const int32_t free_bytes = (int32_t)SOCK64_RXBUF - s->rxlen;
    if (free_bytes <= 0) return 0;
    if (free_bytes > 65535) return 65535;
    return (uint16_t)free_bytes;
}

// ★ v0.12.2：组一个 UDP 数据报（8 字节头 + payload）。**单独成函数是为了能被自检断言** ——
//   "端口在线上必须是 0x00,0x35（53）" 这种判据只有真正把字节算一遍才抓得住字节序 Bug。
//   返回 8+len；缓冲不够返回 -1。
static int s64_build_udp(const Sock64* s, const uint8_t* payload, int len,
                         uint8_t* out, int cap) {
    if (!s || !out || len < 0) return -1;
    if (len > 1472) len = 1472;                            // 以太网 MTU 内（本层不分片）
    if (cap < 8 + len) return -1;
    s64_wr16(out + 0, s->lport);                           // 源端口（主机序 -> 大端字节）
    s64_wr16(out + 2, s->rport);                           // 目的端口
    s64_wr16(out + 4, (uint16_t)(8 + len));                // UDP 长度
    s64_wr16(out + 6, 0);                                  // 校验和 0 = 不校验（IPv4 允许）
    for (int i = 0; i < len; i++) out[8 + i] = payload[i];
    return 8 + len;
}

// 组一个 TCP 段（**指定序号**）并通过 net64 发出去。payload 可为空。
// syn=true 时带 4 字节 MSS 选项（data offset = 6）。
static int s64_emit_at(Sock64* s, uint32_t seq, uint8_t flags, const uint8_t* payload, int len) {
    uint8_t seg[24 + SOCK64_MSS];
    if (len > SOCK64_MSS) len = SOCK64_MSS;
    const bool with_mss = (flags & TCP_SYN) != 0;
    const int hdr = with_mss ? 24 : 20;
    const int total = hdr + len;
    for (int i = 0; i < hdr; i++) seg[i] = 0;
    // ★ v0.12.2（字节序真 Bug）：端口与窗口在线上必须是**大端**，而本层的 lport/rport
    //   已经统一按**主机序**保存。s64_wr16 本身就是"写两个字节：高字节在前"，所以这里
    //   **不能再套 htons** —— 旧代码 `wr16(htons(port))` 把端口写反了（53 变成 0x3500=13568），
    //   对端根本不认这个端口。又因为收包侧也对称地反了一次，逻辑自检全过、一直没被发现。
    s64_wr16(seg + 0, s->lport);
    s64_wr16(seg + 2, s->rport);
    s64_wr32(seg + 4, seq);
    s64_wr32(seg + 8, (flags & TCP_ACK) ? s->rcv_nxt : 0u);
    seg[12] = (uint8_t)((hdr / 4) << 4);              // data offset
    seg[13] = flags;
    s64_wr16(seg + 14, s64_win(s));                   // ★ 真窗口（原来是写死的 4096，且字节序写反）
    s64_wr16(seg + 16, 0);                            // checksum（先 0）
    s64_wr16(seg + 18, 0);                            // urgent
    if (with_mss) {                                   // ★ MSS=1460：对端才敢按 1460 发
        seg[20] = 2; seg[21] = 4;
        seg[22] = (uint8_t)(SOCK64_MSS >> 8);
        seg[23] = (uint8_t)(SOCK64_MSS & 0xFF);
    }
    for (int i = 0; i < len; i++) seg[hdr + i] = payload[i];

    // 伪首部：src, dst, 0, proto=6, tcp_len
    uint32_t acc = 0;
    acc = s64_sum16((const uint8_t*)&s->laddr, 4, acc);
    acc = s64_sum16((const uint8_t*)&s->raddr, 4, acc);
    acc += 6u;
    acc += (uint32_t)total;
    acc = s64_sum16(seg, total, acc);
    s64_wr16(seg + 16, s64_fold(acc));

    const int rc = net64_send_ipv4(s->raddr, 6, seg, total);
    if (rc == 0) g_tx_seg++;
    return rc;
}

// 发一个段并推进 snd_nxt；带数据的段留一份用于重传
static int s64_tcp_send(Sock64* s, uint8_t flags, const uint8_t* payload, int len) {
    const uint32_t seq = s->snd_nxt;
    const int rc = s64_emit_at(s, seq, flags, payload, len);
    if (rc != 0) return rc;
    s->snd_nxt += (uint32_t)((flags & (TCP_SYN | TCP_FIN)) ? 1u : 0u) + (uint32_t)len;
    if (len > 0) {
        for (int i = 0; i < len; i++) s->tx_buf[i] = payload[i];
        s->tx_seq = seq;
        s->tx_len = (uint16_t)len;
        s->tx_has = 1;
        s->tx_rto_ms = SOCK64_RTO_MS;
        s->tx_tick = g_ticks64;
    }
    return 0;
}

// 在途段超时就重发（单段在途 + 指数退避；收到 ACK 才清）
static void s64_tcp_tick(Sock64* s) {
    if (!s->tx_has) return;
    const uint64_t el_ms = (g_ticks64 - s->tx_tick) * 1000u / PIT_HZ_64;
    if (el_ms < s->tx_rto_ms) return;
    g_retx++;
    if (s64_emit_at(s, s->tx_seq, (uint8_t)(TCP_PSH | TCP_ACK), s->tx_buf, (int)s->tx_len) == 0) {
        dbg64_line_begin64();
        dbg64_str("[SOCK64] retx seq=0x");
        dbg64_hex64(s->tx_seq);
        dbg64_str(" len=");
        dbg64_dec((uint64_t)s->tx_len);
        dbg64_str(" rto=");
        dbg64_dec((uint64_t)s->tx_rto_ms);
        dbg64_str("ms n=");
        dbg64_dec(g_retx);
        dbg64_nl();
        dbg64_line_end64();
    }
    s->tx_tick = g_ticks64;
    s->tx_rto_ms = (s->tx_rto_ms >= 4000u) ? 4000u : (s->tx_rto_ms * 2u);
}

int sock64_create64(int type) {
    if (type != SOCK64_T_TCP && type != SOCK64_T_UDP && type != SOCK64_T_ICMP) return -1;
    for (int i = 0; i < SOCK64_MAX; i++) {
        if (g_socks[i].used) continue;
        Sock64* s = &g_socks[i];
        for (uint32_t k = 0; k < sizeof(Sock64); k++) ((uint8_t*)s)[k] = 0;
        s->used = 1;
        s->type = (uint8_t)type;
        s->state = S_CREATED;
        // ★ raw ICMP 没有"端口"概念（Linux 里 rport 恒 0）——给 0，别造一个假端口出来
        s->lport = (type == SOCK64_T_ICMP) ? 0
                 : (uint16_t)(40000 + (g_ticks64 & 0x3FFu) + (uint16_t)i);   // 临时端口
        s->laddr = 0;
        return SOCK64_FD_BASE + i;
    }
    return -1;
}

// ★ v0.9.39：handle 是否本层 socket（bind/close 的类型判别用；不碰 fd 表）
int sock64_is_socket64(int h) {
    const int i = h - SOCK64_FD_BASE;
    return (i >= 0 && i < SOCK64_MAX && g_socks[i].used) ? 1 : 0;
}

// ★ v0.12.2：poll/select/epoll 的就绪判定（位掩码与 fd64_ready64 同口径）
int sock64_ready64(int h) {
    Sock64* s = s64_of(h);
    if (!s) return -1;
    int r = 0;
    if (s->rxlen > 0 || (s->rxflags & (RXF_EOF | RXF_RST))) r |= 1;      // IN
    if (s->type != SOCK64_T_TCP) r |= 2;                                  // UDP/ICMP 随时可发
    else if (s->state == S_ESTAB || s->state == S_CREATED) r |= 2;         // OUT
    if (s->rxflags & RXF_RST) r |= 4;                                     // HUP
    return r;
}

// ★ v0.9.39：bind(49)。raw ICMP 必须能 bind 成功 —— busybox 的 `xbind()` 一失败就整个退出。
//   如实边界：本内核只有一个本地 IP（net64 的 NET64_IP_LOCAL64），所以"绑地址"只做记录与校验；
//   端口对 ICMP 无意义（内核按 ICMP id 匹配，Linux 同）；TCP 的 bind 仍不支持（那是服务端语义）。
int sock64_bind64(int h, uint32_t ip, uint16_t port) {
    Sock64* s = s64_of(h);
    if (!s) return -88;                                        // -ENOTSOCK
    if (s->type == SOCK64_T_TCP) return -95;                   // -EOPNOTSUPP（服务端未实现）
    if (ip != 0) s->laddr = ip;                                // 0.0.0.0 = 不指定
    if (s->type == SOCK64_T_UDP && port != 0) s->lport = port;
    dbg64_str("[SOCK64] bind h=");
    dbg64_dec((uint64_t)h);
    dbg64_str(" type=");
    dbg64_dec((uint64_t)s->type);
    dbg64_str(" ok=1");
    dbg64_nl();
    return 0;
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
    s->snd_una = s->iss;
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
    if (len > 0 && s->type == SOCK64_T_ICMP) {                 // ★ v0.9.39：raw ICMP（不做任何改写）
        // Linux 的 SOCK_RAW/IPPROTO_ICMP 语义：用户缓冲区 = **完整 ICMP 报文**（8 字节头 + 数据），
        // 内核只补 IPv4 头、ICMP 校验和由用户算。net64_send_ipv4 正是这个口径（内部含 ARP）。
        const int rc = net64_send_ipv4(s->raddr, 1, (const uint8_t*)buf, len);
        if (rc == 0) g_tx_seg++;
        return rc == 0 ? len : -1;
    }
    if (s->type == SOCK64_T_UDP) {
        // ★ v0.12.2（真 Bug，实测抓到）：Linux 的 UDP socket 里**用户只给 payload**，
        //   UDP 头（源端口/目的端口/长度/校验和）必须由内核补。旧代码把 payload 直接丢给
        //   net64_send_ipv4 —— 对端看到的"目的端口"其实是 payload 的头两个字节
        //   （DNS 报文里就是事务 ID），所以查询根本到不了 53 端口、也永远收不到回包：
        //   实测现象 = `[SOCK64] create type=2` + `arp reply 10.0.2.3` + `sendto ret=0x1D`
        //   但下一次 `poll` 直接超时，`nslookup: can't resolve`。
        uint8_t dgram[8 + 1472];
        const int n = s64_build_udp(s, (const uint8_t*)buf, len, dgram, (int)sizeof(dgram));
        if (n <= 0) return -1;
        const int rc = net64_send_ipv4(s->raddr, 17, dgram, n);
        if (rc == 0) g_tx_seg++;
        return rc == 0 ? len : -1;
    }
    if (s->state != S_ESTAB) return -1;
    int sent = 0;
    while (sent < len) {
        const int chunk = (len - sent > SOCK64_MSS) ? SOCK64_MSS : (len - sent);
        // 一段在途：等前一段被 ACK 或超时重传推进（有界等待，不空转）
        uint64_t guard = 0;
        while (s->tx_has && ++guard < 60000u) {
            net64_poll64();
            s64_tcp_tick(s);
            if (!s->tx_has) break;
            s64_sleep_ms(1);
        }
        if (s64_tcp_send(s, (uint8_t)(TCP_PSH | TCP_ACK), (const uint8_t*)buf + sent, chunk) != 0) break;
        sent += chunk;
    }
    return sent;
}

int sock64_recv64(int h, void* buf, int max, uint32_t timeout_ms) {
    Sock64* s = s64_of(h);
    if (!s || !buf || max <= 0) return -1;
    const uint64_t t0 = g_ticks64;
    // ★ v0.12.2：TCP 的默认读超时给到 20 s —— 慢速下载不能因为 3 秒没新数据就被判 EOF；
    //   真的卡住 20 s 时返回 -EAGAIN（"这一刻没数据"），而不是 0（文件结束）。
    const uint32_t to_ms = timeout_ms ? timeout_ms
                                      : ((s->type == SOCK64_T_TCP) ? 20000u : 3000u);
    const uint64_t lim = (uint64_t)to_ms * PIT_HZ_64 / 1000u;
    for (;;) {
        if (s->rxlen > 0) {
            int n = s->rxlen;
            if (n > max) n = max;
            for (int i = 0; i < n; i++) ((uint8_t*)buf)[i] = ((const uint8_t*)s->rx)[i];
            for (int i = n; i < s->rxlen; i++) ((uint8_t*)s->rx)[i - n] = ((const uint8_t*)s->rx)[i];
            s->rxlen -= n;
            // ★ 窗口变化必须**立刻告诉对端**，否则对端会一直按旧窗口停在那里（下载卡死的经典原因）
            if (s->type == SOCK64_T_TCP && s->state == S_ESTAB)
                (void)s64_tcp_send(s, TCP_ACK, nullptr, 0);
            return n;
        }
        // ★ v0.9.39：raw ICMP 被 SIGALRM 打断 -> -EINTR（4）。busybox `ping` 的 recvfrom 见到
        //   EINTR 只 continue（不刷错误），下一次循环就能等到新回复；不会把 1 秒节拍拖成 3 秒超时。
        if (s->type == SOCK64_T_ICMP && proc64_sig_pending64 && proc64_sig_pending64(14)) return -SOCK64_EINTR;
        if (s->type == SOCK64_T_TCP) {
            // ★ v0.12.2：只有 FIN/RST 才算 EOF。原来 S_FIN_SENT/S_CLOSED 也返回 0，
            //   会让"对面还在发、我们只是这一拍没数据"被误判成文件结束。
            if ((s->rxflags & (RXF_EOF | RXF_RST)) && s->rxlen == 0) return 0;
            s64_tcp_tick(s);
        }
        if ((g_ticks64 - t0) >= lim) {
            // ★ v0.12.2：超时语义 = -EAGAIN（TCP/UDP）；ICMP 保持 0（busybox ping 的节拍依赖它）
            return (s->type == SOCK64_T_ICMP) ? 0 : -SOCK64_EAGAIN;
        }
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
        const uint16_t lport = s64_rd16(p + 2);                 // ★ 线上就是大端，rd16 读出来即主机序
        for (int i = 0; i < SOCK64_MAX; i++) {
            Sock64* s = &g_socks[i];
            if (!s->used || s->type != SOCK64_T_UDP) continue;
            if (s->lport != lport) continue;
            s->raddr = src_ip;
            s->rport = s64_rd16(p + 0);
            const int dlen = len - 8;
            for (int k = 0; k < dlen && s->rxlen < SOCK64_RXBUF; k++) ((uint8_t*)s->rx)[s->rxlen++] = p[8 + k];
            return;
        }
        return;
    }
    if (proto == 1) {                                         // ★ v0.9.39：raw ICMP（echo reply）
        // Linux 的 SOCK_RAW/IPPROTO_ICMP 收到的就是**ICMP 报文本身**（含 8 字节头，不含 IPv4 头）。
        // 这里只投递 type=0（echo reply）—— 由 net64 的收包路径筛选后才会进到这里。
        for (int i = 0; i < SOCK64_MAX; i++) {
            Sock64* s = &g_socks[i];
            if (!s->used || s->type != SOCK64_T_ICMP) continue;
            s->raddr = src_ip;
            for (int k = 0; k < len && s->rxlen < SOCK64_RXBUF; k++) ((uint8_t*)s->rx)[s->rxlen++] = p[k];
            s->state = S_ESTAB;
            return;
        }
        return;
    }
    if (proto != 6) return;                                   // 其它协议不管
    if (len < 20) return;
    const uint16_t lport = s64_rd16(p + 2);                     // ★ v0.12.2：去掉多余的一次字节交换
    const uint16_t rport = s64_rd16(p + 0);
    const uint32_t seq = s64_rd32(p + 4);
    const uint8_t flags = p[13];
    const int doff = (int)(p[12] >> 4) * 4;
    if (doff < 20 || doff > len) return;
    const int dlen = len - doff;

    // ★ v0.12.2：TCP 校验和必须为 0（伪首部 + 段整体再取反和）。原来根本不验。
    //   注意：本内核的 IP 是"按内存字节序存的小端整数"，所以 &ip 的 4 个字节正好是线上顺序。
    {
        const uint32_t my_ip = NET64_IP_LOCAL64;
        uint32_t acc = 0;
        acc = s64_sum16((const uint8_t*)&src_ip, 4, acc);
        acc = s64_sum16((const uint8_t*)&my_ip, 4, acc);
        acc += 6u;
        acc += (uint32_t)len;
        acc = s64_sum16(p, len, acc);
        if (s64_fold(acc) != 0) { g_rx_badsum++; return; }
    }

    for (int i = 0; i < SOCK64_MAX; i++) {
        Sock64* s = &g_socks[i];
        if (!s->used || s->type != SOCK64_T_TCP) continue;
        if (s->lport != lport || s->raddr != src_ip || s->rport != rport) continue;
        if (flags & TCP_RST) { s->rxflags |= RXF_RST; s->state = S_CLOSED; return; }

        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && s->state == S_SYN_SENT) {
            s->rcv_nxt = seq + 1;
            s->snd_una = s->iss + 1;
            s->snd_nxt = s->iss + 1;
            s->tx_has = 0;
            (void)s64_tcp_send(s, TCP_ACK, nullptr, 0);       // 第三次握手
            s->state = S_ESTAB;
            dbg64_line_begin64();
            dbg64_str("[SOCK64] estab peer=");
            dbg64_hex64(src_ip);
            dbg64_str(":");
            dbg64_dec((uint64_t)rport);
            dbg64_str(" win=");
            dbg64_dec((uint64_t)s64_win(s));
            dbg64_nl();
            dbg64_line_end64();
            return;
        }
        if (s->state != S_ESTAB && s->state != S_FIN_SENT) return;

        // ★ ACK 推进：只有 ACK 号越过在途段末尾才清重传
        if (flags & TCP_ACK) {
            const uint32_t ack = s64_rd32(p + 8);
            if (seq_gt(ack, s->snd_una)) s->snd_una = ack;
            if (s->tx_has && seq_ge(s->snd_una, s->tx_seq + (uint32_t)s->tx_len)) {
                s->tx_has = 0;
                s->tx_rto_ms = SOCK64_RTO_MS;
            }
        }

        // ★ 数据段：**按序号收**（v0.12.2 的核心修正）
        if (dlen > 0) {
            const int32_t rel = (int32_t)(seq - s->rcv_nxt);
            if (rel == 0) {
                const int32_t room = (int32_t)SOCK64_RXBUF - s->rxlen;
                if (dlen <= room) {
                    for (int k = 0; k < dlen; k++) ((uint8_t*)s->rx)[s->rxlen++] = p[doff + k];
                    s->rcv_nxt += (uint32_t)dlen;
                } else {
                    // 缓冲满：**不推进 rcv_nxt、不入缓冲**，回 0 窗口让对端停下来等我们取走
                    g_rx_winzero++;
                }
                (void)s64_tcp_send(s, TCP_ACK, nullptr, 0);
            } else if (rel < 0) {
                // 重复段（已经收过）：只回 ACK，不再入缓冲 —— 这就是"下载文件不再被重复数据污染"
                g_rx_dup++;
                (void)s64_tcp_send(s, TCP_ACK, nullptr, 0);
            } else {
                // 乱序（中间有洞）：丢弃 + 立刻回 dup ACK，对端看到重复 ACK 会快速重传
                g_rx_ooo++;
                (void)s64_tcp_send(s, TCP_ACK, nullptr, 0);
            }
        }
        if (flags & TCP_FIN) {
            if ((int32_t)(seq + (uint32_t)dlen - s->rcv_nxt) == 0) s->rcv_nxt += 1;
            s->rxflags |= RXF_EOF;
            (void)s64_tcp_send(s, TCP_ACK, nullptr, 0);
            s->state = S_CLOSED;
            dbg64_line_begin64();
            dbg64_str("[SOCK64] fin rx=");
            dbg64_dec((uint64_t)s->rxlen);
            dbg64_str(" dup=");
            dbg64_dec(g_rx_dup);
            dbg64_str(" ooo=");
            dbg64_dec(g_rx_ooo);
            dbg64_str(" badsum=");
            dbg64_dec(g_rx_badsum);
            dbg64_str(" retx=");
            dbg64_dec(g_retx);
            dbg64_nl();
            dbg64_line_end64();
        }
        return;
    }
}

uint64_t sock64_tx_segments64() { return g_tx_seg; }
uint64_t sock64_rx_segments64() { return g_rx_seg; }
uint64_t sock64_retx64() { return g_retx; }
uint64_t sock64_rx_dup64() { return g_rx_dup; }
uint64_t sock64_rx_ooo64() { return g_rx_ooo; }
uint64_t sock64_rx_badsum64() { return g_rx_badsum; }
uint64_t sock64_rx_winzero64() { return g_rx_winzero; }

// ---------------- 纯逻辑自检（不发包，可在 fake 路径跑）----------------
// 校验：handle 编解码、状态机初值、TCP 校验和算法（已知向量）、窗口计算、就绪位
int sock64_selftest64() {
    bool ok = true;
    const int h = sock64_create64(SOCK64_T_TCP);
    if (h != SOCK64_FD_BASE + 0) ok = false;
    Sock64* s = h >= 0 ? s64_of(h) : nullptr;
    if (!s || s->state != S_CREATED || s->type != SOCK64_T_TCP) ok = false;
    if (sock64_create64(99) >= 0) ok = false;                 // 非法类型必须拒绝
    // 校验和：把 [00 01] 的 16 位和取反，应为 0xFFFE
    uint8_t probe[2] = { 0x00, 0x01 };
    if (s64_fold(s64_sum16(probe, 2, 0)) != 0xFFFEu) ok = false;
    // ★ v0.12.2：窗口口径（空 = 65535 上限，收满 = 0）
    if (s) {
        if (s64_win(s) != 65535u) ok = false;
        s->rxlen = SOCK64_RXBUF;
        if (s64_win(s) != 0u) ok = false;
        s->rxlen = 0;
        if ((sock64_ready64(h) & 2) == 0) ok = false;          // CREATED 的 TCP 可写（connect 用）
    }
    if (sock64_ready64(h + 99) != -1) ok = false;              // 越界 handle 必须 -1
    // ★ v0.12.2 判据：**端口必须按大端写在线上**（用户报的 DNS 失败就是这里错的）。
    //   rport=53 -> 线上必须是 0x00,0x35；lport=0x1234 -> 0x12,0x34。写反了这两个断言必红。
    {
        const int uh = sock64_create64(SOCK64_T_UDP);
        Sock64* u = uh >= 0 ? s64_of(uh) : nullptr;
        if (!u) ok = false;
        else {
            uint8_t dg[8 + 4];
            u->lport = 0x1234;
            u->rport = 53;
            const int n = s64_build_udp(u, (const uint8_t*)"ABCD", 4, dg, (int)sizeof(dg));
            if (n != 12) ok = false;
            if (dg[0] != 0x12 || dg[1] != 0x34) ok = false;     // 源端口
            if (dg[2] != 0x00 || dg[3] != 0x35) ok = false;     // 目的端口 = 53
            if (dg[4] != 0x00 || dg[5] != 0x0C) ok = false;     // UDP 长度 = 12
            if (dg[8] != 'A' || dg[11] != 'D') ok = false;      // payload 原样
            (void)sock64_close64(uh);
        }
    }
    if (h >= 0) (void)sock64_close64(h);
    dbg64_line_begin64();
    dbg64_str("[SOCK64] selftest ");
    dbg64_str(ok ? "PASS" : "FAIL");
    dbg64_str(" (client-only: TCP connect/send/recv/close + UDP + raw ICMP; no listen/accept)");
    dbg64_nl();
    dbg64_line_end64();
    return ok ? 0 : -1;
}
