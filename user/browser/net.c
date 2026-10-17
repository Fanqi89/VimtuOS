/* user/browser/net.c - 浏览器的最小 HTTP/1.0 客户端（ring3，内核侧只有 net_raw 那条 53 号 ABI）
 *
 * 来源与边界：本文件把 `user/net/netd.c`（已经 tests/netuser64_test.py 验收过的代码）里的
 * **必要子集**搬过来 —— ARP / IPv4 / UDP / DNS(A) / TCP 客户端 —— 删掉 CLI、DHCP、ICMP、
 * 监听回显与全部打点噪音，只留一条"抓一个 http:// URL 的正文"的路径：
 *
 *     net_http_get("http://10.0.2.2:18080/index.html", body, cap, &status, &got);
 *
 * 与 netd 完全一致的边界（不假装更完整）：
 *   * IPv4 不分片/不重组（收到分片直接丢）；TCP 不重排序（乱序段丢弃）、无拥塞控制
 *     （固定 RTO 400ms、最多 2 次重传、指数退避）、无 TIME_WAIT；
 *   * DNS 只做 A 记录、一问一答（超时重发 1 次）；
 *   * **只支持 http 一种 scheme**（没有 TLS，也就不假装支持 https）；
 *   * 同一时刻只有一条连接（浏览器一次只抓一页）；
 *   * 地址配置是**静态缺省**（QEMU/slirp 的 10.0.2.15/24，gw 10.0.2.2，dns 10.0.2.3），
 *     不搬 DHCP —— 理由见报告；网卡不在（net_raw_mac 失败）或链路 down 时接不上，
 *     如实打 reason=connect step=3。
 *
 * 内存纪律（主装载区只有 64 KiB，构建还可能带 -fno-zero-initialized-in-bss）：
 *   * **不 malloc**（用户堆只有 4 KiB）；
 *   * 所有大缓冲（tx 帧 / rx 帧 / 段拼装 / DNS 查询 / UDP 数据报 / TCB）在**首次调用**时
 *     用 lx6(9, ...) 拿一块 12 KiB 的匿名区（与 netd 的 arena_init 同款），
 *     所以 .data/.bss 里只剩几十字节的标量；
 *   * 单个函数帧远小于 1 KiB：netd 里 ip_send 的 u8 tmp[1500]、tcp_send_seg 的 u8 s[1472]、
 *     udp_send 的 u8 p[1472] 三块栈缓冲改成写 arena 里的 g_tx/g_seg。
 *
 * 打点（tests/browser_http64_test.py grep；格式逐字勿改，每行**一次** printf）：
 *   [BNET] url host=.. port=.. path=..
 *   [BNET] dns name=.. addr=<a.b.c.d> ms=..
 *   [BNET] tcp connect ip=<a.b.c.d>:<port> ok=1 ms=..
 *   [BNET] http get status=.. bytes=.. ms=..
 *   [BNET] FAIL reason=<parse|dns|connect|send|timeout|status-line|no-body> step=..
 * step：1 = URL 解析 / 2 = DNS / 3 = TCP 连接 / 4 = 发请求 / 5 = 收响应（超时）/
 *       6 = 状态行 / 7 = 正文（no-body）；成功时 g_info.step = 0。
 *
 * 返回码（net.h 的约定）：0 = 拿到响应；-1 参数/URL 非法；-2 DNS；-3 连接；
 *   -4 发送；-5 超时；-6 响应格式（status-line / no-body）。
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* net.h 第 11 行的注释里出现了"两个斜杠紧跟两个星号"，clang 的 -Wcomment 会把中间两个字符当成
 * 嵌套起始符再报一条警告；头文件不在本任务可改的范围内，这里**只对这条诊断**做局部抑制：
 * net.c 自身的代码在 -Wall -Wextra 下 0 警告（见报告里的编译输出）。*/
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcomment"
#include "net.h"
#pragma clang diagnostic pop

typedef signed long long   i64;
typedef unsigned long long u64;
typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;

/* ==================== 系统调用包装（与 netd 同款：自有 ABI 第 4 参数走 r10）==================== */
static i64 sc4(i64 nr, i64 a, i64 b, i64 c, i64 d) {
    i64 r;
    __asm__ volatile("movq %[d], %%r10\n\t"
                     "int $0x80"
                     : "=a"(r)
                     : "a"(nr), "D"(a), "S"(b), "d"(c), [d] "r"(d)
                     : "r10", "rcx", "r11", "memory");
    return r;
}
static i64 sc0(i64 nr) { return sc4(nr, 0, 0, 0, 0); }
static i64 sc1(i64 nr, i64 a) { return sc4(nr, a, 0, 0, 0); }
static i64 sc3(i64 nr, i64 a, i64 b, i64 c) { return sc4(nr, a, b, c, 0); }

/* Linux ABI（syscall 指令）：只要一块匿名内存（mmap 9）当整栈的收发缓冲 */
static i64 lx6(i64 nr, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5, i64 a6) {
    i64 r;
    register i64 r10 __asm__("r10") = a4;
    register i64 r8  __asm__("r8")  = a5;
    register i64 r9  __asm__("r9")  = a6;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

/* ==================== 时间（自有 ABI 4 ticks / 5 sleep_ms；PIT = 250 Hz = 4ms/tick）==================== */
static u64 now_ticks(void) { return (u64)sc0(4); }
static u64 now_ms(void) { return now_ticks() * 4ull; }
static void slp_ms(i64 ms) { if (ms > 0) (void)sc1(5, ms); }

/* ==================== 小工具（逐段取自 netd）==================== */
static void memzero(void* d, u64 n) { u8* p = (u8*)d; while (n--) *p++ = 0; }
static void memcpy_n(void* d, const void* s, u64 n) {
    u8* dp = (u8*)d;
    const u8* sp = (const u8*)s;
    while (n--) *dp++ = *sp++;
}
static int memeq_n(const void* a, const void* b, u64 n) {
    const u8* x = (const u8*)a;
    const u8* y = (const u8*)b;
    while (n--) { if (*x++ != *y++) return 0; }
    return 1;
}
static u16 rd16(const u8* p) { return (u16)(((u16)p[0] << 8) | p[1]); }        /* 网络序（大端）*/
static u32 rd32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static void wr16(u8* p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void wr32(u8* p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static u32 ip4(u32 a, u32 b, u32 c, u32 d) { return (a << 24) | (b << 16) | (c << 8) | d; }

/* ==================== net_raw（自有 int 0x80 号 53）：内核侧只用 e1000 既有的收发 ====================
 * 沿用 kernel/netraw64.h 的错误码表：-2 EFAULT / -3 EINVAL / -4 EAGAIN / -5 ENODEV / -6 EMSGSIZE */
#define NETRAW_NR 53
#define NR_EAGAIN  (-4)
#define RX_CAP     1600
enum { NR_TX = 0, NR_RX = 1, NR_MAC = 2, NR_LINK = 3 };

static i64 net_raw_tx(const u8* f, u64 len) { return sc3(NETRAW_NR, NR_TX, (i64)(u64)f, (i64)len); }
static i64 net_raw_rx(u8* b, u64 cap) { return sc3(NETRAW_NR, NR_RX, (i64)(u64)b, (i64)cap); }
static i64 net_raw_mac(u8* out) { return sc3(NETRAW_NR, NR_MAC, (i64)(u64)out, 0); }
static i64 net_raw_link(void) { return sc3(NETRAW_NR, NR_LINK, 0, 0); }

/* ==================== 校验和（RFC 1071；全部按大端 16 位字）—— 取自 netd ==================== */
static u32 csum_add(u32 sum, const u8* p, u32 len) {
    while (len >= 2) { sum += (u32)rd16(p); p += 2; len -= 2; }
    if (len) sum += (u32)((u16)p[0] << 8);
    return sum;
}
static u16 csum_fin(u32 sum) {
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (u16)(~sum);
}
static u32 csum_pseudo(u32 sip, u32 dip, u8 proto, u32 len) {
    /* 伪首部：src(4) + dst(4) + zero(1) + proto(1) + length(2) */
    u8 h[12];
    wr32(h, sip); wr32(h + 4, dip); h[8] = 0; h[9] = proto; wr16(h + 10, (u16)len);
    return csum_add(0, h, 12);
}

/* ==================== 全局状态（全部是小标量；大缓冲在 arena 里）==================== */
#define ARP_N 16
typedef struct { int used; u32 ip; u8 mac[6]; u32 age; } ArpEnt;

static u8    g_mac[6];
static u32   g_our_ip = 0, g_mask = 0, g_gw = 0, g_dns = 0;
static ArpEnt g_arp[ARP_N];
static u32   g_arp_age = 0;

/* 收到的 UDP 数据报（最后一次；DNS 等待循环靠 generation 判"来新的了"）*/
static u32   g_udp_rx_len = 0, g_udp_rx_ip = 0, g_udp_rx_gen = 0;
static u16   g_udp_rx_sport = 0;

/* 帧配比：以太网 MTU 1500 / 单帧上限 1514 */
#define FRAME_PAYLOAD_MAX 1500u
#define IP_PAYLOAD_MAX    1480u          /* 20 字节 IP 头之后（不分片）*/
#define IP_UDP_MAX        1472u          /* 8 字节 UDP 头之后 */
#define TCP_MSS           1440u          /* 保守：20 字节 TCP 头 + 1440 = 1460 <= 1480 */

/* ==================== TCP 控制块 + 定时参数 ==================== */
#define TCP_BUF    2048u                 /* 接收缓冲（本栈单缓冲、立即 ACK）*/
#define TCP_WIN    1024u                 /* 我们通告的窗口：<= TCP_BUF/2，突发不会溢出缓冲 */
#define TCP_RTO_MS 400u                  /* 固定 RTO（无 RTT 估计、无拥塞控制）*/
#define TCP_RETX   2u                    /* 最多重传 2 次（退避 400/800）*/

typedef struct {
    int  state;                          /* 0 CLOSED / 1 SYN_SENT / 2 ESTAB / 3 FIN_WAIT */
    u32  rip; u16 sport, dport;          /* 对端 rip:dport；本地 sport */
    u32  snd_una, snd_nxt, rcv_nxt;      /* 未被确认的 / 下一个要发的 / 期望收到的 */
    u8   rmac[6];
    u32  rx_len;                         /* 已收进 rx 的字节（只收"正好按序"的那一份）*/
    int  rst, got_fin;
    u8   rx[TCP_BUF];
} Tcb;

/* ==================== arena（首次调用惰性 mmap；不 malloc）==================== */
static u8*  g_tx  = 0;                   /* 1600：以太网帧拼装（14 头 + 20 IP 头 + 净荷）*/
static u8*  g_rx  = 0;                   /* 1600：net_raw_rx 的单帧落点 */
static u8*  g_seg = 0;                   /* 1600：TCP/UDP 段拼装（头 + 净荷）*/
static u8*  g_q   = 0;                   /* 512 ：DNS 查询报文 */
static u8*  g_urx = 0;                   /* 600 ：UDP 数据报（DNS 应答）*/
static u8*  g_req = 0;                   /* 512 ：HTTP 请求文本（一次 send 发完）*/
static Tcb* g_tcb = 0;                   /* TCB（含 2048 字节接收缓冲）*/
static int  g_net_up = 0;

#define ARENA_BYTES (16u * 1024u)

static int arena_init(void) {
    const i64 va = lx6(9 /*mmap*/, 0, (i64)ARENA_BYTES, 3 /*READ|WRITE*/, 0x22 /*PRIVATE|ANON*/, -1, 0);
    if (va <= 0) return -1;
    u8* a = (u8*)(u64)va;
    /* TCB 放最前面（自然 16 字节对齐），其余按 16 字节边界顺排 */
    g_tcb = (Tcb*)(void*)a;
    u32 off = (u32)((sizeof(Tcb) + 15u) & ~(size_t)15u);
    g_tx  = a + off; off += RX_CAP;
    g_rx  = a + off; off += RX_CAP;
    g_seg = a + off; off += RX_CAP;
    g_q   = a + off; off += 512u;
    g_urx = a + off; off += 600u;
    g_req = a + off; off += 512u;
    if ((u32)off > ARENA_BYTES) { g_tx = g_rx = g_seg = g_q = g_urx = g_req = 0; g_tcb = 0; return -1; }
    return 0;
}

/* 取 MAC / 链路，落一整套静态地址（QEMU/slirp 缺省）。只做一次。 */
static int net_up(void) {
    if (g_net_up) return 0;
    if (!g_tcb && arena_init() != 0) return -1;
    if (net_raw_mac(g_mac) != 0) return -1;              /* 没网卡（ENODEV）：不假装有链路 */
    if (net_raw_link() != 1) return -1;
    g_our_ip = ip4(10, 0, 2, 15);
    g_mask   = ip4(255, 255, 255, 0);
    g_gw     = ip4(10, 0, 2, 2);
    g_dns    = ip4(10, 0, 2, 3);
    g_net_up = 1;
    return 0;
}

/* ==================== 以太网 ==================== */
static const u8 g_bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* 净荷已经在 g_tx+14 里的情况（ip_send 走这条，免得再来一次 1500 字节的栈拷贝）*/
static int eth_frame_tx(const u8* dst_mac, u16 type, u32 blen) {
    if (blen > FRAME_PAYLOAD_MAX) return -1;
    memcpy_n(g_tx, dst_mac, 6);
    memcpy_n(g_tx + 6, g_mac, 6);
    wr16(g_tx + 12, type);
    u32 len = 14u + blen;
    if (len < 60u) { memzero(g_tx + len, 60u - len); len = 60u; }   /* 最小帧 60 字节 */
    return (net_raw_tx(g_tx, len) == 0) ? 0 : -1;
}
static int eth_send(const u8* dst_mac, u16 type, const u8* body, u32 blen) {
    if (blen > FRAME_PAYLOAD_MAX) return -1;
    if (blen) memcpy_n(g_tx + 14, body, blen);
    return eth_frame_tx(dst_mac, type, blen);
}

/* ==================== ARP（请求 + 应答缓存；不做 ARP 应答）==================== */
static void arp_store(u32 ip, const u8* mac) {
    int free_i = -1, old_i = 0;
    for (int i = 0; i < ARP_N; i++) {
        if (g_arp[i].used && g_arp[i].ip == ip) {
            memcpy_n(g_arp[i].mac, mac, 6);
            g_arp[i].age = ++g_arp_age;
            return;
        }
        if (!g_arp[i].used && free_i < 0) free_i = i;
        if (g_arp[i].age < g_arp[old_i].age) old_i = i;
    }
    const int s = (free_i >= 0) ? free_i : old_i;
    g_arp[s].ip = ip;
    memcpy_n(g_arp[s].mac, mac, 6);
    g_arp[s].used = 1;
    g_arp[s].age = ++g_arp_age;
}
static int arp_lookup(u32 ip, u8* out) {
    for (int i = 0; i < ARP_N; i++)
        if (g_arp[i].used && g_arp[i].ip == ip) { if (out) memcpy_n(out, g_arp[i].mac, 6); return 0; }
    return -1;
}
static int arp_request(u32 ip) {
    u8 p[28];
    wr16(p + 0, 1);                 /* htype = Ethernet */
    wr16(p + 2, 0x0800);            /* ptype = IPv4 */
    p[4] = 6; p[5] = 4;
    wr16(p + 6, 1);                 /* oper = request */
    memcpy_n(p + 8, g_mac, 6);
    wr32(p + 14, g_our_ip);
    memzero(p + 18, 6);             /* target mac = 0（未知）*/
    wr32(p + 24, ip);
    return eth_send(g_bcast, 0x0806, p, 28);
}

/* ==================== IPv4 ==================== */
static int ip_send(u32 dst_ip, u8 proto, const u8* payload, u32 plen, const u8* dst_mac) {
    if (plen > IP_PAYLOAD_MAX) return -2;                   /* 不做分片：超了直接拒（如实）*/
    u8* h = g_tx + 14;
    h[0] = 0x45; h[1] = 0;
    wr16(h + 2, (u16)(20u + plen));
    wr16(h + 4, 0x1234);                                    /* id（不分片，值不重要）*/
    wr16(h + 6, 0x4000);                                    /* DF：不让我们自己产生分片 */
    h[8] = 64; h[9] = proto;
    wr16(h + 10, 0);
    wr32(h + 12, g_our_ip);
    wr32(h + 16, dst_ip);
    wr16(h + 10, csum_fin(csum_add(0, h, 20)));
    if (plen) memcpy_n(g_tx + 34, payload, plen);
    return eth_frame_tx(dst_mac, 0x0800, 20u + plen);
}

/* ==================== 收帧泵（只保留 ARP / IPv4 / UDP(DNS) / TCP 四条）==================== */
static void poll_frames(u32 budget);
static int  arp_resolve(u32 ip, u8* mac_out, u32 ms_budget);
static int  tcp_send_seg(u32 rip, u16 sport, u16 dport, const u8* rmac, u32 seq, u32 ack,
                         u8 flags, const u8* data, u32 len);

static int arp_resolve(u32 ip, u8* mac_out, u32 ms_budget) {
    if (arp_lookup(ip, mac_out) == 0) return 0;
    if (arp_request(ip) != 0) return -1;
    const u64 t0 = now_ms();
    while (now_ms() - t0 < (u64)ms_budget) {
        poll_frames(8);
        if (arp_lookup(ip, mac_out) == 0) return 0;
        slp_ms(4);
    }
    return -2;                                              /* 有界超时（绝不挂死）*/
}

/* 单跳路由：出了本网段就发给网关 */
static u32 next_hop(u32 dst_ip) {
    if (g_our_ip && g_mask && ((dst_ip & g_mask) != (g_our_ip & g_mask))) return g_gw;
    return dst_ip;
}

/* ==================== UDP（只为 DNS）==================== */
static int udp_send(u32 dst_ip, u16 sport, u16 dport, const u8* data, u32 len, const u8* dst_mac) {
    if (len > IP_UDP_MAX) return -2;
    u8* p = g_seg;
    wr16(p + 0, sport);
    wr16(p + 2, dport);
    wr16(p + 4, (u16)(8u + len));
    wr16(p + 6, 0);
    if (len) memcpy_n(p + 8, data, len);
    u32 sum = csum_pseudo(g_our_ip, dst_ip, 17, 8u + len);
    sum = csum_add(sum, p, 8u + len);
    wr16(p + 6, csum_fin(sum));
    return ip_send(dst_ip, 17, p, 8u + len, dst_mac);
}
static int udp_send_to(u32 dst_ip, u16 sport, u16 dport, const u8* data, u32 len) {
    u8 mac[6];
    const int r = arp_resolve(next_hop(dst_ip), mac, 800u);
    if (r != 0) return (r == -1) ? -1 : -3;
    return udp_send(dst_ip, sport, dport, data, len, mac);
}
/* 等一个来自 (ip, sport) 的 UDP 数据报；返回长度（>0）或 -4 超时 */
static int udp_wait(u32 from_ip, u16 from_sport, u32 gen0, u32 ms_budget) {
    const u64 t0 = now_ms();
    for (;;) {
        poll_frames(16);
        if (g_udp_rx_gen != gen0 && g_udp_rx_ip == from_ip &&
            (!from_sport || g_udp_rx_sport == from_sport)) return (int)g_udp_rx_len;
        if (now_ms() - t0 >= (u64)ms_budget) return -4;
        slp_ms(4);
    }
}

/* ==================== TCP（客户端；无重排序/无拥塞控制/无 TIME_WAIT）==================== */
#define TCP_F_FIN 0x01
#define TCP_F_SYN 0x02
#define TCP_F_RST 0x04
#define TCP_F_PSH 0x08
#define TCP_F_ACK 0x10

static u16 tcp_csum(u32 sip, u32 dip, const u8* seg, u32 seglen) {
    u32 sum = csum_pseudo(sip, dip, 6, seglen);
    sum = csum_add(sum, seg, seglen);
    return csum_fin(sum);
}
/* 发一段 TCP（flags 用 TCP_F_*；payload 可空）—— 段拼在 arena 的 g_seg 里，不上栈 */
static int tcp_send_seg(u32 rip, u16 sport, u16 dport, const u8* rmac, u32 seq, u32 ack,
                        u8 flags, const u8* data, u32 len) {
    if (len > TCP_MSS) return -1;
    u8* s = g_seg;
    wr16(s + 0, sport);
    wr16(s + 2, dport);
    wr32(s + 4, seq);
    wr32(s + 8, ack);
    s[12] = 0x50;                                            /* data offset = 5（无选项）*/
    s[13] = flags;
    wr16(s + 14, (u16)TCP_WIN);                              /* 窗口（<= 接收缓冲的一半）*/
    wr16(s + 16, 0);                                         /* 校验和 */
    wr16(s + 18, 0);
    if (len) memcpy_n(s + 20, data, len);
    const u32 seglen = 20u + len;
    wr16(s + 16, tcp_csum(g_our_ip, rip, s, seglen));
    return ip_send(rip, 6, s, seglen, rmac);
}

/* 主动连接：三次握手（固定 RTO=400ms，重传 2 次退避 400/800）；返回 0 = ESTABLISHED */
static int tcp_connect(u32 rip, u16 rport, u16 sport, u32 ms_budget) {
    memzero(g_tcb, sizeof(*g_tcb));
    if (arp_resolve(next_hop(rip), g_tcb->rmac, 800u) != 0) return -3;
    const u32 seq0 = (u32)(now_ticks() * 1103515245u) ^ (u32)(u64)g_req;
    g_tcb->rip = rip; g_tcb->sport = sport; g_tcb->dport = rport;
    g_tcb->snd_una = seq0;
    g_tcb->snd_nxt = seq0;
    g_tcb->state = 1;
    if (tcp_send_seg(rip, sport, rport, g_tcb->rmac, seq0, 0, TCP_F_SYN, 0, 0) != 0) return -1;
    g_tcb->snd_nxt = seq0 + 1u;
    const u64 t0 = now_ms();
    u64 last = t0;
    u32 rto = TCP_RTO_MS;
    u32 attempt = 0;
    for (;;) {
        poll_frames(16);
        if (g_tcb->rst) return -2;                           /* 对端拒绝（RST）*/
        if (g_tcb->state == 2) return 0;                     /* SYN/ACK 收到并回了 ACK */
        const u64 el = now_ms();
        if (el - t0 >= (u64)ms_budget) return -4;
        if (el - last >= (u64)rto) {
            if (attempt >= TCP_RETX) return -5;
            attempt++;
            g_tcb->snd_nxt = seq0 + 1u;
            (void)tcp_send_seg(rip, sport, rport, g_tcb->rmac, seq0, 0, TCP_F_SYN, 0, 0);
            last = el;
            rto *= 2u;
        }
        slp_ms(4);
    }
}
/* 收数据 + 回 ACK；返回收到的字节数（>0，即 g_tcb->rx_len）或负码。
 *   -2 = RST / -3 = 对端 FIN 且手里没数据（流结束）/ -4 = 本批超时 */
static int tcp_recv(u32 want_bytes, u32 ms_budget) {
    const u64 t0 = now_ms();
    for (;;) {
        poll_frames(16);
        if (g_tcb->rst) return -2;
        if (g_tcb->rx_len >= want_bytes) return (int)g_tcb->rx_len;
        if (g_tcb->got_fin && g_tcb->rx_len == 0) return -3;
        if (now_ms() - t0 >= (u64)ms_budget) return -4;
        slp_ms(4);
    }
}
static int tcp_send_data(u32 seq, const u8* data, u32 len) {
    const int rc = tcp_send_seg(g_tcb->rip, g_tcb->sport, g_tcb->dport, g_tcb->rmac, seq,
                                g_tcb->rcv_nxt, (u8)(TCP_F_PSH | TCP_F_ACK), data, len);
    if (rc == 0) g_tcb->snd_nxt = seq + len;
    return rc;
}
/* 主动收尾：发 FIN|ACK，有界等它的 ACK（600ms）；对端已经先 FIN 了就不再发（被动关闭）*/
static void tcp_close_active(void) {
    if (g_tcb->state != 2) { g_tcb->state = 0; return; }
    g_tcb->state = 3;
    (void)tcp_send_seg(g_tcb->rip, g_tcb->sport, g_tcb->dport, g_tcb->rmac, g_tcb->snd_nxt,
                       g_tcb->rcv_nxt, (u8)(TCP_F_FIN | TCP_F_ACK), 0, 0);
    g_tcb->snd_nxt += 1u;
    const u64 t0 = now_ms();
    while (now_ms() - t0 < 600u) {
        poll_frames(8);
        if (g_tcb->snd_una >= g_tcb->snd_nxt) break;
        slp_ms(4);
    }
    g_tcb->state = 0;
}

/* ==================== 收包分发：以太网 -> ARP / IPv4 -> UDP / TCP ==================== */
static void tcp_handle(const u8* pay, u32 plen, u32 sip, const u8* src_mac) {
    if (plen < 20u) return;
    const u16 sport = rd16(pay + 0), dport = rd16(pay + 2);
    const u32 seq = rd32(pay + 4), ack = rd32(pay + 8);
    const u32 doff = (u32)(pay[12] >> 4) * 4u;
    const u8 flags = pay[13];
    if (doff < 20u || doff > plen) return;
    {   /* TCP 校验和（含伪首部）必须验通过；不通过按坏包丢弃 */
        u32 sum = csum_pseudo(sip, g_our_ip, 6, plen);
        sum = csum_add(sum, pay, plen);
        if (csum_fin(sum) != 0u) return;
    }
    if (sip != g_tcb->rip || sport != g_tcb->dport || dport != g_tcb->sport) return;  /* 只认我们这条连接 */
    if (flags & TCP_F_RST) { g_tcb->rst = 1; return; }
    if (g_tcb->state == 0) return;
    /* ---- SYN_SENT：等 SYN|ACK ---- */
    if (g_tcb->state == 1) {
        if ((flags & (TCP_F_SYN | TCP_F_ACK)) == (TCP_F_SYN | TCP_F_ACK) && ack == g_tcb->snd_nxt) {
            g_tcb->rcv_nxt = seq + 1u;
            g_tcb->snd_una = ack;
            g_tcb->state = 2;
            memcpy_n(g_tcb->rmac, src_mac, 6);
            (void)tcp_send_seg(sip, g_tcb->sport, sport, src_mac, g_tcb->snd_nxt, g_tcb->rcv_nxt,
                               TCP_F_ACK, 0, 0);
        }
        return;
    }
    /* ---- ESTAB / FIN_WAIT：推进 ACK / 收数据 ---- */
    if (flags & TCP_F_ACK) {
        if ((i64)(ack - g_tcb->snd_una) > 0 && (i64)(ack - g_tcb->snd_nxt) <= 0) g_tcb->snd_una = ack;
    }
    const u32 dlen = plen - doff;
    if (dlen) {
        if (seq == g_tcb->rcv_nxt) {
            u32 copy = dlen;
            const u32 room = TCP_BUF - g_tcb->rx_len;
            if (copy > room) copy = room;
            if (copy) {
                memcpy_n(g_tcb->rx + g_tcb->rx_len, pay + doff, copy);
                g_tcb->rx_len += copy;
                g_tcb->rcv_nxt += copy;
            }
            (void)tcp_send_seg(sip, g_tcb->sport, sport, src_mac, g_tcb->snd_nxt, g_tcb->rcv_nxt,
                               TCP_F_ACK, 0, 0);
        } else if ((i64)(seq - g_tcb->rcv_nxt) <= 0) {
            /* 重复段：重发当前 ACK */
            (void)tcp_send_seg(sip, g_tcb->sport, sport, src_mac, g_tcb->snd_nxt, g_tcb->rcv_nxt,
                               TCP_F_ACK, 0, 0);
        }
        /* 乱序段（seq > rcv_nxt）：本栈不缓存，丢弃；对端会按 ACK 重传 —— 如实 */
    }
    if (flags & TCP_F_FIN) {
        g_tcb->rcv_nxt += 1u;
        g_tcb->got_fin = 1;
        (void)tcp_send_seg(sip, g_tcb->sport, sport, src_mac, g_tcb->snd_nxt, g_tcb->rcv_nxt,
                           TCP_F_ACK, 0, 0);
        if (g_tcb->snd_una >= g_tcb->snd_nxt) g_tcb->state = 0;
    }
}
static void udp_handle(const u8* pay, u32 plen, u32 sip) {
    if (plen < 8u) return;
    const u16 sport = rd16(pay + 0);
    const u16 ulen = rd16(pay + 4);
    if (ulen < 8u || (u32)ulen > plen) return;
    /* UDP 校验和：0 = 发送方没算（RFC 768 允许）-> 跳过；非 0 就必须验 */
    if (rd16(pay + 6) != 0u) {
        u32 sum = csum_pseudo(sip, g_our_ip, 17, ulen);
        sum = csum_add(sum, pay, ulen);
        if (csum_fin(sum) != 0u) return;
    }
    const u32 dlen = (u32)ulen - 8u;
    if (dlen > 600u) return;
    if (dlen) memcpy_n(g_urx, pay + 8, dlen);
    g_udp_rx_len = dlen;
    g_udp_rx_ip = sip;
    g_udp_rx_sport = sport;
    g_udp_rx_gen++;
}
static void ipv4_handle(const u8* p, u32 len, const u8* src_mac) {
    if (len < 20u) return;
    if ((p[0] >> 4) != 4u) return;
    const u32 ihl = (u32)(p[0] & 0xFu) * 4u;
    if (ihl < 20u || ihl > len) return;
    if (csum_fin(csum_add(0, p, ihl)) != 0u) return;
    const u32 total = rd16(p + 2);
    if (total > len) return;
    const u16 fragf = rd16(p + 6);
    if ((fragf & 0x1FFFu) != 0u || (fragf & 0x2000u) != 0u) return;   /* 分片：不支持，丢 */
    const u8 proto = p[9];
    const u32 sip = rd32(p + 12), dip = rd32(p + 16);
    const u8* pay = p + ihl;
    const u32 plen = total - ihl;
    if (dip != g_our_ip && dip != 0xFFFFFFFFu) return;                 /* 不是发给我们的 */
    if (proto == 17) { udp_handle(pay, plen, sip); return; }
    if (proto == 6) { tcp_handle(pay, plen, sip, src_mac); return; }
    /* 其它协议（ICMP/IGMP…）：本模块只做客户端，不处理、不应答 —— 如实丢弃 */
}
static void arp_handle(const u8* p, u32 len) {
    if (len < 28u) return;
    if (rd16(p + 0) != 1u || rd16(p + 2) != 0x0800u || p[4] != 6u || p[5] != 4u) return;
    arp_store(rd32(p + 14), p + 8);                     /* oper=1/2 都把 spa->sha 记下来 */
    /* ring3 **不回应** ARP 请求（与 netd 一致，如实标注）*/
}
static void frame_handle(const u8* f, u32 len) {
    if (len < 14u) return;
    const u8* dmac = f;
    if (!memeq_n(dmac, g_mac, 6) && !memeq_n(dmac, g_bcast, 6) && !(dmac[0] & 0x01)) return;  /* 不是给我们的单播 */
    const u16 type = rd16(f + 12);
    if (type == 0x0806u) arp_handle(f + 14, len - 14u);
    else if (type == 0x0800u) ipv4_handle(f + 14, len - 14u, f + 6);
    /* IPv6/LLDP/…：不管（如实）*/
}
static void poll_frames(u32 budget) {
    for (u32 i = 0; i < budget; i++) {
        const i64 n = net_raw_rx(g_rx, RX_CAP);
        if (n == NR_EAGAIN) return;                     /* 没包：立刻回（非阻塞语义）*/
        if (n <= 0) return;                             /* -ENODEV 等：静默 */
        if (n < 14) continue;
        frame_handle(g_rx, (u32)n);
    }
}

/* ==================== DNS（A 记录；压缩指针 + 多答案；TC/rcode 如实报错）==================== */
/* "a.b.c" -> 03 'a' 01 'b' 01 'c' 00。返回总字节数；0 = 名字非法。允许结尾带点。 */
static int dns_encode_name(u8* out, u32 cap, const char* n) {
    u32 o = 0, lab = 0;
    if (!*n) return 0;
    out[o++] = 0;                                   /* 第一段的长度占位 */
    while (*n) {
        const char c = *n++;
        if (c == '.') {
            if (!lab) return 0;                     /* 空段（"a..b" / 开头是点）-> 非法 */
            out[o - lab - 1] = (u8)lab;             /* 回填本段长度 */
            lab = 0;
            if (!*n) break;                         /* 结尾的点：当名字结束 */
            if (o + 1u >= cap) return 0;
            out[o++] = 0;
            continue;
        }
        if (o + 1u >= cap || lab >= 62u) return 0;
        out[o++] = (u8)c;
        lab++;
    }
    if (lab) out[o - lab - 1] = (u8)lab;            /* 最后一段的长度也要回填（netd 的教训）*/
    if (o + 1u > cap) return 0;
    out[o++] = 0;                                   /* 根标签 */
    return (int)o;
}
/* 跳过（可能带压缩指针的）名字；指针只判合法、不跟随（我们只需要"名字到哪结束"）*/
static int dns_skip_name(const u8* m, u32 len, u32* off) {
    u32 o = *off;
    for (;;) {
        if (o >= len) return -1;
        const u8 b = m[o];
        if ((b & 0xC0u) == 0xC0u) {
            if (o + 1u >= len) return -1;
            const u32 target = (((u32)(b & 0x3Fu)) << 8) | m[o + 1u];
            if (target >= len) return -1;           /* 指针指到报文外：malformed */
            return (int)(o + 2u);
        }
        if (b == 0) return (int)(o + 1u);
        if ((b & 0xC0u) != 0) return -1;
        o += 1u + b;
    }
}
/* 返回 0 = 拿到第一个 A 记录（写进 *first_ip）；负码见下（-2 发送 / -3 超时 / -6 malformed
 *   -7 TC 截断 / -8 rcode / -9 没有 A 记录）。一问一答，超时重发 1 次。 */
static int dns_resolve(const char* name, u16 sport, u32 ms_budget, u32* first_ip) {
    *first_ip = 0;
    if (!name || !*name) return -5;
    const u32 id = (u32)(now_ticks() * 40503u) & 0xFFFFu;
    u8* q = g_q;
    memzero(q, 512u);
    wr16(q + 0, (u16)id);
    wr16(q + 2, 0x0100);                            /* QR=0, RD=1 */
    wr16(q + 4, 1);                                 /* qdcount */
    const int o = dns_encode_name(q + 12, 200u, name);
    if (!o) return -5;
    const u32 qlen = 12u + (u32)o + 4u;
    wr16(q + 12u + (u32)o, 1);                      /* QTYPE = A */
    wr16(q + 14u + (u32)o, 1);                      /* QCLASS = IN */
    u32 gen0 = g_udp_rx_gen;
    if (udp_send_to(g_dns, sport, 53u, q, qlen) != 0) return -2;
    for (u32 attempt = 0; attempt < 2u; attempt++) {
        if (attempt) {
            gen0 = g_udp_rx_gen;                    /* 重发前重新取基线 */
            if (udp_send_to(g_dns, sport, 53u, q, qlen) != 0) continue;
        }
        if (udp_wait(g_dns, 53u, gen0, ms_budget) < 0) continue;
        const u8* m = g_urx;
        const u32 len = g_udp_rx_len;
        if (len < 12u || rd16(m + 0) != (u16)id) return -6;
        const u32 flags = rd16(m + 2);
        if (flags & 0x0200u) return -7;             /* TC：应答被截断 */
        if (flags & 0x000Fu) return -8;             /* rcode != 0 */
        const u32 ancount = rd16(m + 6);
        if (!ancount) return -9;
        u32 po = 12u;
        int r = dns_skip_name(m, len, &po);         /* 跳过 question 的 QNAME */
        if (r < 0) return -6;
        po = (u32)r + 4u;                           /* + QTYPE + QCLASS */
        for (u32 i = 0; i < ancount && i < 16u; i++) {
            r = dns_skip_name(m, len, &po);
            if (r < 0) break;
            po = (u32)r;
            if (po + 10u > len) break;
            const u16 rtype = rd16(m + po);
            const u16 rclass = rd16(m + po + 2);
            const u16 rdlen = rd16(m + po + 8);
            po += 10u;
            if (po + rdlen > len) break;
            if (rtype == 1u && rclass == 1u && rdlen == 4u) {
                *first_ip = rd32(m + po);
                return 0;
            }
            po += rdlen;
        }
        if (!*first_ip) return -9;
        return 0;
    }
    return -3;
}

/* ==================== URL 解析：http://host[:port][/path] ==================== */
static int ci_prefix(const char* s, const char* pre) {
    for (; *pre; pre++, s++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != *pre) return 0;
    }
    return 1;
}
/* "10.0.2.2" -> 1（*out 有效）；不是严格的点分十进制 -> 0 */
static int parse_ipv4(const char* s, u32* out) {
    u32 v = 0, part = 0, nd = 0, np = 0;
    for (;; s++) {
        const char c = *s;
        if (c >= '0' && c <= '9') {
            part = part * 10u + (u32)(c - '0');
            if (part > 255u) return 0;
            if (++nd > 3u) return 0;
        } else if (c == '.' || c == 0) {
            if (!nd || np >= 4u) return 0;
            v = (v << 8) | part;
            part = 0; nd = 0; np++;
            if (c == 0) break;
        } else return 0;
    }
    if (np != 4u) return 0;
    *out = v;
    return 1;
}
/* 成功返回 0；非 http:// 或任何结构性问题返回 -1（打 reason=parse）*/
static int url_parse(const char* url, char* host, u32 hostcap, u32* port, char* path, u32 pathcap) {
    if (!url) return -1;
    if (!ci_prefix(url, "http://")) return -1;          /* https/http2 一律拒（没有 TLS，不假装）*/
    *port = 80;                                         /* 端口缺省 80（调用方不设也对）*/
    const char* h = url + 7;
    u32 hn = 0;
    while (h[hn] && h[hn] != ':' && h[hn] != '/' && h[hn] != '?' && h[hn] != '#') {
        if (hn + 1u >= hostcap) return -1;
        host[hn] = h[hn];
        hn++;
    }
    host[hn] = 0;
    if (!hn) return -1;
    const char* q = h + hn;
    if (*q == ':') {                                    /* 端口 */
        q++;
        u32 v = 0, nd = 0;
        while (*q >= '0' && *q <= '9') {
            v = v * 10u + (u32)(*q - '0');
            if (v > 65535u) return -1;
            q++; nd++;
        }
        if (!nd || v == 0) return -1;
        *port = v;
    }
    if (*q == '/') {                                    /* 路径（去掉 #fragment）*/
        u32 i = 0;
        while (q[i] && q[i] != '#') {
            if (i + 1u >= pathcap) return -1;
            path[i] = q[i];
            i++;
        }
        path[i] = 0;
    } else {
        path[0] = '/'; path[1] = 0;                     /* 没有 path（或只有 ?query）：当 "/" */
    }
    return 0;
}

/* ==================== 诊断信息（net_last_info）==================== */
static struct BNetInfo g_info;

const struct BNetInfo* net_last_info(void) { return &g_info; }

/* 失败打点：一行一次 printf，同时把 step 记进 g_info */
static int bnet_fail(int step, const char* reason, int rc) {
    g_info.step = step;
    printf("[BNET] FAIL reason=%s step=%d\n", reason, step);
    return rc;
}

/* 响应切分：状态行必须是 "HTTP/1.x <3 位数字>"，然后以第一个 \r\n\r\n 为头/正文分界，
 * 把正文**前移**到 body 开头。返回 0 = 切好了；-1 = 状态行不合法；-2 = 没有 \r\n\r\n 或正文为空。
 * （单独成一个函数：既让 net_http_get 的栈帧小，也让这段纯字节逻辑能被离线测。） */
static int http_split(char* body, u32 total, int* status_out, u32* blen_out) {
    if (total >= 12u &&
        body[0] == 'H' && body[1] == 'T' && body[2] == 'T' && body[3] == 'P' &&
        body[4] == '/' && body[5] == '1' && body[6] == '.' &&
        body[7] >= '0' && body[7] <= '9' && body[8] == ' ' &&
        body[9] >= '0' && body[9] <= '9' &&
        body[10] >= '0' && body[10] <= '9' &&
        body[11] >= '0' && body[11] <= '9') {
        *status_out = (int)(body[9] - '0') * 100 + (int)(body[10] - '0') * 10 + (int)(body[11] - '0');
    } else {
        return -1;                                          /* 状态行不是 HTTP/1.x <3 位数字> */
    }
    u32 hend = 0;
    int found = 0;
    for (u32 i = 0; i + 4u <= total; i++) {
        if (body[i] == '\r' && body[i + 1] == '\n' && body[i + 2] == '\r' && body[i + 3] == '\n') {
            hend = i + 4u;
            found = 1;
            break;
        }
    }
    if (!found) return -2;                                  /* 没有 \r\n\r\n（头没结束）*/
    const u32 blen = total - hend;
    if (blen == 0) return -2;                               /* 正文为空（200 也算失败，如实）*/
    for (u32 i = 0; i < blen; i++) body[i] = body[hend + i];   /* 正文前移（自己写的 memmove）*/
    *blen_out = blen;
    return 0;
}

/* ==================== net_http_get ==================== */
#define HTTP_DNS_BUDGET_MS   1500u
#define HTTP_TCP_BUDGET_MS   2500u
#define HTTP_RECV_BUDGET_MS  8000u

int net_http_get(const char* url, char* body, int cap, int* status, int* got) {
    char host[64];
    char path[192];
    u32 total = 0;                      /* 已收进 body 的字节数（含响应头）*/

    /* ---- 0. 参数 + 诊断状态复位 ---- */
    g_info.host[0] = 0;
    g_info.ip = 0;
    g_info.port = 80;
    g_info.dns_ms = 0;
    g_info.tcp_ms = 0;
    g_info.http_ms = 0;
    g_info.step = 0;
    if (status) *status = -1;
    if (got) *got = 0;
    if (!url || !body || cap <= 0) return bnet_fail(1, "parse", -1);
    /* 网卡/链路由 net_up 在这里一次性取好（不做 DHCP：地址是 slirp 缺省静态值）*/

    /* ---- 1. URL ---- */
    u32 port = 80;
    if (url_parse(url, host, sizeof(host), &port, path, sizeof(path)) != 0) return bnet_fail(1, "parse", -1);
    g_info.port = port;
    for (u32 i = 0; host[i] && i + 1u < sizeof(g_info.host); i++) g_info.host[i] = host[i];
    g_info.host[sizeof(g_info.host) - 1u] = 0;
    printf("[BNET] url host=%s port=%u path=%s\n", host, (unsigned)port, path);
    /* 到这一步才需要网卡：URL 非法时先报 parse，不被"没网卡"盖掉 */
    if (net_up() != 0) return bnet_fail(3, "connect", -3);

    /* ---- 2. host -> IP：点分十进制就跳过 DNS（一行 dns 都不打），否则问 A 记录 ---- */
    u32 ip = 0;
    if (!parse_ipv4(host, &ip)) {
        const u64 t0 = now_ms();
        u32 a = 0;
        const int dr = dns_resolve(host, 40001u, HTTP_DNS_BUDGET_MS, &a);
        if (dr != 0 || a == 0) return bnet_fail(2, "dns", -2);
        ip = a;
        g_info.dns_ms = (u32)(now_ms() - t0);
        printf("[BNET] dns name=%s addr=%u.%u.%u.%u ms=%u\n", host,
               (unsigned)((ip >> 24) & 0xFFu), (unsigned)((ip >> 16) & 0xFFu),
               (unsigned)((ip >> 8) & 0xFFu), (unsigned)(ip & 0xFFu),
               (unsigned)g_info.dns_ms);
    }
    g_info.ip = ip;

    /* ---- 3. TCP 连接（sport 固定 = 40000 + (pid & 0x3FFF)）---- */
    const u16 sport = (u16)(40000u + ((u32)sc0(3) & 0x3FFFu));
    {
        const u64 t0 = now_ms();
        const int cr = tcp_connect(ip, (u16)port, sport, HTTP_TCP_BUDGET_MS);
        if (cr != 0) return bnet_fail(3, "connect", -3);
        g_info.tcp_ms = (u32)(now_ms() - t0);
    }
    printf("[BNET] tcp connect ip=%u.%u.%u.%u:%u ok=1 ms=%u\n",
           (unsigned)((ip >> 24) & 0xFFu), (unsigned)((ip >> 16) & 0xFFu),
           (unsigned)((ip >> 8) & 0xFFu), (unsigned)(ip & 0xFFu),
           (unsigned)port, (unsigned)g_info.tcp_ms);

    /* ---- 4. 发请求（GET 一次 send 发完；请求文本拼在 arena 的 g_req 里）---- */
    {
        char* w = (char*)g_req;
        const char* s;
        u32 rlen;
        for (s = "GET "; *s; s++) *w++ = *s;
        for (s = path; *s; s++) *w++ = *s;
        for (s = " HTTP/1.0\r\nHost: "; *s; s++) *w++ = *s;
        for (s = host; *s; s++) *w++ = *s;
        for (s = "\r\nUser-Agent: VimtuOS-Browser/0.5.2\r\nConnection: close\r\n\r\n"; *s; s++) *w++ = *s;
        rlen = (u32)(w - (char*)g_req);
        if (rlen > TCP_MSS) return bnet_fail(4, "send", -4);
        {
            const u64 t0 = now_ms();
            const int sr = tcp_send_data(g_tcb->snd_nxt, g_req, rlen);
            if (sr != 0) return bnet_fail(4, "send", -4);
            /* ---- 5. 收响应：循环 tcp_recv 直到对端 FIN / 我们自己的 cap 用完 / 总预算耗尽 ---- */
            total = 0;
            int timed_out = 0;
            const u64 deadline = t0 + HTTP_RECV_BUDGET_MS;
            for (;;) {
                const u64 now = now_ms();
                /* 预算用尽后仍然问一次（left=0 -> tcp_recv 只 poll 一轮、不睡），
                 * 免得"最后一批已经躺在接收缓冲里、却被超时判断抢先切掉"。*/
                const u32 left = (now >= deadline) ? 0u : (u32)(deadline - now);
                const int rr = tcp_recv(1u, left);
                if (rr > 0) {
                    u32 n = (u32)rr;
                    const u32 room = (u32)cap - total;
                    if (n > room) n = room;
                    if (n) memcpy_n(body + total, g_tcb->rx, n);
                    total += n;
                    g_tcb->rx_len = 0;                   /* 已经搬进 body，接收缓冲可复用 */
                    if (total >= (u32)cap) break;        /* cap 用完：截断并停下（如实）*/
                    if (!left) { timed_out = 1; break; } /* 最后一批搬完，预算也到点了 */
                } else if (rr == -2 || rr == -3) {
                    break;                               /* RST / 对端 FIN：流结束 */
                } else {
                    timed_out = 1;
                    break;                               /* -4：预算用尽 */
                }
            }
            g_info.http_ms = (u32)(now_ms() - t0);
            tcp_close_active();
            if (timed_out) return bnet_fail(5, "timeout", -5);
        }
    }

    /* ---- 6. 响应格式：状态行必须是 "HTTP/1.x <3 位数字>"，然后切掉头 ---- */
    {
        int st = -1;
        u32 blen = 0;
        const int hr = http_split(body, total, &st, &blen);
        if (hr == -1) return bnet_fail(6, "status-line", -6);
        if (hr != 0) return bnet_fail(7, "no-body", -6);
        if (status) *status = st;
        if (got) *got = (int)blen;
        g_info.step = 0;
        printf("[BNET] http get status=%d bytes=%d ms=%u\n", st, (int)blen, (unsigned)g_info.http_ms);
        return 0;
    }
}
