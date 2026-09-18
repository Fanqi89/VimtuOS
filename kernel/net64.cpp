// net64.cpp - 极简以太网 / IPv4 / ARP / ICMP（跑在 kernel/e1000_64.cpp 的轮询收包之上）
//
// 范围（有意做的很小）：
//   * 静态 IPv4 配置（QEMU 用户网络 slirp 固定值，见 net64.h 常量与来源注释）；
//   * 只处理 ARP 和 IPv4/ICMP：ARP request/reply（8 项缓存 + 回 request）、
//     ICMP echo request（回 reply）与 echo reply（记录 + 打点）；其它帧静默丢弃并计数；
//   * 不用中断、没有 DHCP/路由/UDP/TCP —— 收包靠主动 net64_poll64()。
//
// 超时策略（全部有界，绝不卡启动）：
//   * ARP 解析 / ICMP 回包各等 500 个 PIT tick（250Hz -> 2 秒，见 NET64_*_TIMEOUT_TICKS）；
//   * 等待循环里每轮先 poll 一次再 `sti; hlt` 等下一个 tick —— 让 QEMU 有机会处理网络
//     后端（slirp）并推进 g_ticks64，同时不空转烧 CPU；
//   * 没网卡/超时：打印 no link 行并返回负值，调用方（启动链）继续，系统不失败。
//
// 串口日志（验收按行 grep，改前先想清楚；都在 dbg64_line_begin/end 里保证整行原子）：
//   [NET64] config ip=10.0.2.15/24 gw=10.0.2.2 dns=10.0.2.3 mac=aa:bb:cc:dd:ee:ff
//   [NET64] arp who-has 10.0.2.2
//   [NET64] arp reply 10.0.2.2 is-at aa:bb:cc:dd:ee:ff (rx=<n> arp=<n>)
//   [NET64] icmp reply from 10.0.2.2 seq=1 bytes=<n> ttl=<n> (tx=<n> rx=<n>)
//   [NET64] selftest PASS / FAIL mask=<n>
//   [NET64] no link / arp timeout (tx=<n> rx=<n> dropped=<n>)      （优雅跳过用）
#include "net64.h"
#include "e1000_64.h"
#include "debug64.h"
#include "mem_64.h"
#include "x86_64.h"     // g_ticks64 / PIT_HZ_64
#include <stdint.h>

// ==================== 常量 ====================
#define NET64_ETH_HDR      14      // dst[6] src[6] type[2]
#define NET64_IP_HDR       20      // 无选项的 IPv4 头
#define NET64_ARP_LEN      28
#define NET64_ETH_MIN      60      // 以太网最小帧（不含 FCS）：不够要补零
#define NET64_FRAME_MAX    1600
#define NET64_ARP_CACHE_N  8       // 规范要求：8 项 ARP 缓存
#define NET64_TTL          64
#define NET64_ICMP_ID      0x5649  // "VI"
#define NET64_MAX_PAYLOAD  (NET64_FRAME_MAX - NET64_ETH_HDR - NET64_IP_HDR)
#define NET64_POLL_MAX_FRAMES 32   // 单次 poll 最多处理多少帧（防止无限循环）

// 超时：250Hz PIT 下 500 tick = 2 秒。用 tick 而不是忙等次数：QEMU 慢/快都一致。
#define NET64_ARP_TIMEOUT_TICKS   500
#define NET64_ICMP_TIMEOUT_TICKS  500

static const uint8_t NET64_BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ping 载荷（固定内容，便于日志/抓包对照）
static const char NET64_PING_TEXT[] = "VimtuOS net64 ping";

// ==================== 状态 ====================
static bool g_inited = false;
static bool g_up = false;
static uint8_t g_our_mac[6];

struct Net64ArpEntry {
    uint32_t ip;
    uint8_t  mac[6];
    uint8_t  valid;
    uint32_t age;      // 越大越新；满时替换最小者
};
static Net64ArpEntry g_arp[NET64_ARP_CACHE_N];
static uint32_t g_arp_age = 0;

static uint64_t g_tx_frames = 0;
static uint64_t g_rx_frames = 0;
static uint64_t g_arp_replies = 0;
static uint64_t g_icmp_replies = 0;
static uint64_t g_dropped = 0;

// 最近一次 ICMP echo reply（ping 的匹配用）；不用环形队列，够单线程用
static uint32_t g_last_reply_ip = 0;
static uint16_t g_last_reply_seq = 0;
static uint16_t g_last_reply_id = 0;

// 文件级缓冲：内核线程栈小，协议栈的临时帧绝不放栈上
static uint8_t g_eth_frame[NET64_FRAME_MAX];   // 以太网帧（发送用）
static uint8_t g_ip_frame[NET64_FRAME_MAX];    // IP 包（eth 载荷）
static uint8_t g_rx_frame[NET64_FRAME_MAX];    // 驱动取回的帧
static uint8_t g_icmp_buf[NET64_MAX_PAYLOAD];  // 构造/回显的 ICMP 报文

// ==================== 小工具 ====================
static void n64_line_begin() { dbg64_line_begin64(); }
static void n64_line_end()   { dbg64_nl(); dbg64_line_end64(); }

static void n64_put_ip(uint32_t ip) {
    dbg64_dec((uint64_t)(ip & 0xFFu));
    dbg64_putc('.');
    dbg64_dec((uint64_t)((ip >> 8) & 0xFFu));
    dbg64_putc('.');
    dbg64_dec((uint64_t)((ip >> 16) & 0xFFu));
    dbg64_putc('.');
    dbg64_dec((uint64_t)((ip >> 24) & 0xFFu));
}

static void n64_put_mac(const uint8_t* m) {
    static const char* H = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        if (i) dbg64_putc(':');
        dbg64_putc(H[(m[i] >> 4) & 0xF]);
        dbg64_putc(H[m[i] & 0xF]);
    }
}

static void n64_ip_to_bytes(uint32_t ip, uint8_t* b) {
    b[0] = (uint8_t)(ip & 0xFFu);
    b[1] = (uint8_t)((ip >> 8) & 0xFFu);
    b[2] = (uint8_t)((ip >> 16) & 0xFFu);
    b[3] = (uint8_t)((ip >> 24) & 0xFFu);
}

static uint32_t n64_ip_from_bytes(const uint8_t* b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

// 16 位反码求和（RFC 1071）：输入按**大端字**解释（网络字节序），返回折叠后的和。
static uint32_t n64_sum16(const uint8_t* p, int len) {
    uint32_t sum = 0;
    int i = 0;
    while (i + 1 < len) {
        sum += ((uint32_t)p[i] << 8) | (uint32_t)p[i + 1];
        i += 2;
    }
    if (i < len) sum += (uint32_t)p[i] << 8;     // 奇数长度：末字节当高字节
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return sum & 0xFFFFu;
}
static uint16_t n64_csum16(const uint8_t* p, int len) {
    return (uint16_t)(~n64_sum16(p, len) & 0xFFFFu);
}

// 等一个 PIT tick：先 sti（本函数只允许在可中断上下文调用），再 hlt。
// 这样 QEMU 能跑主循环把 slirp 的包投进来、g_ticks64 也会推进。
static void n64_wait_tick() {
    __asm__ volatile("sti; hlt" ::: "memory");
}

// ==================== 帧构造 ====================
// 以太网最小帧补零：len < 60 时补零到 60 并返回 60（发送路径与自检共用，保证自检
// 验的是真代码路径）。frame 必须至少有 60 字节容量。
static int n64_eth_pad(uint8_t* frame, int len) {
    if (len < 0) return -1;
    if (len >= NET64_ETH_MIN) return len;
    for (int i = len; i < NET64_ETH_MIN; i++) frame[i] = 0;
    return NET64_ETH_MIN;
}

// 发一帧以太网：dst MAC + 本机 MAC + ethertype（大端）+ payload，补齐最小帧后交给驱动。
static int n64_send_eth(const uint8_t* dst_mac, uint16_t ethertype,
                        const uint8_t* payload, int plen) {
    if (!g_up) return -1;
    if (!dst_mac || plen < 0 || NET64_ETH_HDR + plen > NET64_FRAME_MAX) return -1;
    memcpy_64(g_eth_frame, dst_mac, 6);
    memcpy_64(g_eth_frame + 6, g_our_mac, 6);
    g_eth_frame[12] = (uint8_t)(ethertype >> 8);
    g_eth_frame[13] = (uint8_t)(ethertype & 0xFFu);
    if (plen > 0) memcpy_64(g_eth_frame + NET64_ETH_HDR, payload, (uint64_t)plen);
    const int len = n64_eth_pad(g_eth_frame, NET64_ETH_HDR + plen);
    if (len < 0) return -1;
    if (e1000_send64(g_eth_frame, len) != 0) return -1;
    g_tx_frames++;
    return 0;
}

// IPv4 头 + 载荷（校验和算好写进头）。返回总长（20+plen）；失败 -1。
static int n64_build_ip(uint8_t* out, int cap, uint32_t src, uint32_t dst,
                        uint8_t proto, const uint8_t* payload, int plen) {
    if (!out || plen < 0 || NET64_IP_HDR + plen > cap || plen > NET64_MAX_PAYLOAD) return -1;
    out[0] = 0x45;                                  // version=4, IHL=5
    out[1] = 0x00;                                  // TOS
    const int total = NET64_IP_HDR + plen;
    out[2] = (uint8_t)(total >> 8);
    out[3] = (uint8_t)(total & 0xFF);
    out[4] = 0; out[5] = 0;                         // ID（不用分片，固定 0）
    out[6] = 0; out[7] = 0;                         // flags/fragment offset = 0
    out[8] = NET64_TTL;
    out[9] = proto;
    out[10] = 0; out[11] = 0;                       // 校验和先清零
    n64_ip_to_bytes(src, out + 12);
    n64_ip_to_bytes(dst, out + 16);
    if (plen > 0) memcpy_64(out + NET64_IP_HDR, payload, (uint64_t)plen);
    const uint16_t c = n64_csum16(out, NET64_IP_HDR);
    out[10] = (uint8_t)(c >> 8);
    out[11] = (uint8_t)(c & 0xFF);
    return total;
}

// IPv4 解析视图（payload 指向 IP 头之后的字节，不拷贝）
struct Net64IpView {
    uint32_t src;
    uint32_t dst;
    uint8_t  proto;
    uint8_t  ttl;
    uint16_t total;         // total length 字段
    int      hdr_len;
    const uint8_t* payload;
    int      payload_len;
};

// 解析 IPv4（宽松：只查版本/IHL/长度边界；入站路径不强制校验和，见 handle_ip）。
static int n64_parse_ip(const uint8_t* p, int len, Net64IpView* out) {
    if (!p || !out || len < NET64_IP_HDR) return -1;
    if ((p[0] >> 4) != 4) return -1;
    const int ihl = (int)(p[0] & 0x0Fu) * 4;
    if (ihl < NET64_IP_HDR || ihl > len) return -1;
    const uint16_t total = (uint16_t)(((uint16_t)p[2] << 8) | p[3]);
    if (total < ihl || total > len) return -1;
    out->src = n64_ip_from_bytes(p + 12);
    out->dst = n64_ip_from_bytes(p + 16);
    out->proto = p[9];
    out->ttl = p[8];
    out->total = total;
    out->hdr_len = ihl;
    out->payload = p + ihl;
    out->payload_len = (int)total - ihl;
    return 0;
}

// ARP 报文（28 字节，以太网/IPv4）
struct Net64ArpView {
    uint16_t oper;
    uint8_t  sha[6];
    uint32_t spa;
    uint8_t  tha[6];
    uint32_t tpa;
};

static int n64_build_arp(uint8_t* out, int cap, uint16_t oper,
                         const uint8_t* sha, uint32_t spa,
                         const uint8_t* tha, uint32_t tpa) {
    if (!out || cap < NET64_ARP_LEN || !sha || !tha) return -1;
    out[0] = 0x00; out[1] = 0x01;                   // htype = Ethernet
    out[2] = 0x08; out[3] = 0x00;                   // ptype = IPv4
    out[4] = 6;    out[5] = 4;                      // hlen / plen
    out[6] = (uint8_t)(oper >> 8);
    out[7] = (uint8_t)(oper & 0xFFu);
    memcpy_64(out + 8, sha, 6);
    n64_ip_to_bytes(spa, out + 14);
    memcpy_64(out + 18, tha, 6);
    n64_ip_to_bytes(tpa, out + 24);
    return NET64_ARP_LEN;
}

static int n64_parse_arp(const uint8_t* p, int len, Net64ArpView* out) {
    if (!p || !out || len < NET64_ARP_LEN) return -1;
    if (p[0] != 0x00 || p[1] != 0x01 || p[2] != 0x08 || p[3] != 0x00) return -1;
    if (p[4] != 6 || p[5] != 4) return -1;
    out->oper = (uint16_t)(((uint16_t)p[6] << 8) | p[7]);
    memcpy_64(out->sha, p + 8, 6);
    out->spa = n64_ip_from_bytes(p + 14);
    memcpy_64(out->tha, p + 18, 6);
    out->tpa = n64_ip_from_bytes(p + 24);
    return 0;
}

// ==================== ARP 缓存 ====================
static void n64_arp_store(uint32_t ip, const uint8_t* mac) {
    if (!mac) return;
    int free_i = -1, old_i = 0;
    for (int i = 0; i < NET64_ARP_CACHE_N; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            memcpy_64(g_arp[i].mac, mac, 6);
            g_arp[i].age = ++g_arp_age;
            return;
        }
        if (!g_arp[i].valid && free_i < 0) free_i = i;
        if (g_arp[i].age < g_arp[old_i].age) old_i = i;
    }
    const int slot = (free_i >= 0) ? free_i : old_i;
    g_arp[slot].ip = ip;
    memcpy_64(g_arp[slot].mac, mac, 6);
    g_arp[slot].valid = 1;
    g_arp[slot].age = ++g_arp_age;
}

static int n64_arp_lookup(uint32_t ip, uint8_t* mac_out) {
    for (int i = 0; i < NET64_ARP_CACHE_N; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            if (mac_out) memcpy_64(mac_out, g_arp[i].mac, 6);
            return 0;
        }
    }
    return -1;
}

// ==================== 收包处理 ====================
static void n64_handle_arp(const uint8_t* p, int len) {
    Net64ArpView a;
    if (n64_parse_arp(p, len, &a) != 0) { g_dropped++; return; }
    if (a.oper == 1) {                                // request：记下来 + 问的是我们就回
        n64_arp_store(a.spa, a.sha);
        if (a.tpa == NET64_IP_LOCAL64) {
            uint8_t reply[NET64_ARP_LEN];
            if (n64_build_arp(reply, NET64_ARP_LEN, 2, g_our_mac, NET64_IP_LOCAL64,
                              a.sha, a.spa) == NET64_ARP_LEN) {
                (void)n64_send_eth(a.sha, 0x0806, reply, NET64_ARP_LEN);
            }
        }
        return;
    }
    if (a.oper == 2) {                                // reply：更新缓存 + 打点
        n64_arp_store(a.spa, a.sha);
        g_arp_replies++;
        n64_line_begin();
        dbg64_str("[NET64] arp reply ");
        n64_put_ip(a.spa);
        dbg64_str(" is-at ");
        n64_put_mac(a.sha);
        dbg64_str(" (rx=");
        dbg64_dec(g_rx_frames);
        dbg64_str(" arp=");
        dbg64_dec(g_arp_replies);
        dbg64_str(")");
        n64_line_end();
        return;
    }
    g_dropped++;                                      // 其它 oper（RARP 等）不管
}

static void n64_handle_ip(const uint8_t* p, int len, const uint8_t* src_mac) {
    Net64IpView ip;
    if (n64_parse_ip(p, len, &ip) != 0) { g_dropped++; return; }
    if (ip.dst != NET64_IP_LOCAL64) { g_dropped++; return; }   // 不是发给我们的
    if (ip.proto != 1) { g_dropped++; return; }                // 只处理 ICMP
    if (ip.payload_len < 8) { g_dropped++; return; }           // ICMP 头都不全

    const uint8_t* icmp = ip.payload;
    const uint8_t type = icmp[0];
    if (type == 8) {                                  // echo request -> 回 echo reply
        int icmp_len = ip.payload_len;
        if (icmp_len > NET64_MAX_PAYLOAD) icmp_len = NET64_MAX_PAYLOAD;
        if (icmp_len > 1472) icmp_len = 1472;         // 以太网 MTU 上限（1500-20-8）
        memcpy_64(g_icmp_buf, icmp, (uint64_t)icmp_len);
        g_icmp_buf[0] = 0;                            // echo reply
        g_icmp_buf[1] = 0;
        g_icmp_buf[2] = 0; g_icmp_buf[3] = 0;         // 校验和清零后重算
        const uint16_t c = n64_csum16(g_icmp_buf, icmp_len);
        g_icmp_buf[2] = (uint8_t)(c >> 8);
        g_icmp_buf[3] = (uint8_t)(c & 0xFF);
        // 谁 ping 我们：记进 ARP 缓存，回复直接走源 MAC（同一跳，不必查缓存）
        n64_arp_store(ip.src, src_mac);
        const int n = n64_build_ip(g_ip_frame, NET64_FRAME_MAX, NET64_IP_LOCAL64,
                                   ip.src, 1, g_icmp_buf, icmp_len);
        if (n > 0) (void)n64_send_eth(src_mac, 0x0800, g_ip_frame, n);
        return;
    }
    if (type == 0) {                                  // echo reply -> 记录 + 打点
        const uint16_t id = (uint16_t)(((uint16_t)icmp[4] << 8) | icmp[5]);
        const uint16_t seq = (uint16_t)(((uint16_t)icmp[6] << 8) | icmp[7]);
        g_icmp_replies++;
        g_last_reply_ip = ip.src;
        g_last_reply_seq = seq;
        g_last_reply_id = id;
        n64_line_begin();
        dbg64_str("[NET64] icmp reply from ");
        n64_put_ip(ip.src);
        dbg64_str(" seq=");
        dbg64_dec(seq);
        dbg64_str(" bytes=");
        dbg64_dec((uint64_t)(ip.payload_len - 8));
        dbg64_str(" ttl=");
        dbg64_dec(ip.ttl);
        dbg64_str(" (tx=");
        dbg64_dec(g_tx_frames);
        dbg64_str(" rx=");
        dbg64_dec(g_rx_frames);
        dbg64_str(")");
        n64_line_end();
        return;
    }
    g_dropped++;                                      // 其它 ICMP 类型先不处理
}

static void n64_handle_frame(const uint8_t* f, int len) {
    if (len < NET64_ETH_HDR) { g_dropped++; return; }
    const uint16_t type = (uint16_t)(((uint16_t)f[12] << 8) | f[13]);
    const uint8_t* payload = f + NET64_ETH_HDR;
    const int plen = len - NET64_ETH_HDR;
    if (type == 0x0806) {                             // ARP
        n64_handle_arp(payload, plen);
    } else if (type == 0x0800) {                      // IPv4
        n64_handle_ip(payload, plen, f + 6);
    } else {
        g_dropped++;                                  // 其余以太网类型：静默丢弃并计数
    }
}

// ==================== 对外 API ====================
void net64_poll64() {
    if (!g_up) return;
    for (int i = 0; i < NET64_POLL_MAX_FRAMES; i++) {
        const int n = e1000_recv64(g_rx_frame, NET64_FRAME_MAX);
        if (n <= 0) break;
        g_rx_frames++;
        n64_handle_frame(g_rx_frame, n);
    }
}

int net64_arp_request64(uint32_t ip) {
    if (!g_up) return -1;
    uint8_t req[NET64_ARP_LEN];
    if (n64_build_arp(req, NET64_ARP_LEN, 1, g_our_mac, NET64_IP_LOCAL64,
                      NET64_BCAST, ip) != NET64_ARP_LEN) return -1;
    if (n64_send_eth(NET64_BCAST, 0x0806, req, NET64_ARP_LEN) != 0) return -1;
    n64_line_begin();
    dbg64_str("[NET64] arp who-has ");
    n64_put_ip(ip);
    n64_line_end();
    return 0;
}

int net64_ping64(uint32_t ip, uint16_t seq) {
    if (!g_up) return -1;

    // 1) 没 MAC 先 ARP 解析（有界等待）
    uint8_t mac[6];
    if (n64_arp_lookup(ip, mac) != 0) {
        (void)net64_arp_request64(ip);
        const uint64_t t0 = g_ticks64;
        while ((g_ticks64 - t0) < NET64_ARP_TIMEOUT_TICKS) {
            net64_poll64();
            if (n64_arp_lookup(ip, mac) == 0) break;
            n64_wait_tick();
        }
        if (n64_arp_lookup(ip, mac) != 0) return -2;   // ARP 超时
    }

    // 2) ICMP echo request（id 固定、seq 由调用方给）
    const int plen = (int)sizeof(NET64_PING_TEXT) - 1;
    int icmp_len = 8 + plen;
    if (icmp_len > (int)sizeof(g_icmp_buf)) icmp_len = (int)sizeof(g_icmp_buf);
    g_icmp_buf[0] = 8;                                 // echo request
    g_icmp_buf[1] = 0;
    g_icmp_buf[2] = 0; g_icmp_buf[3] = 0;
    g_icmp_buf[4] = (uint8_t)(NET64_ICMP_ID >> 8);
    g_icmp_buf[5] = (uint8_t)(NET64_ICMP_ID & 0xFF);
    g_icmp_buf[6] = (uint8_t)(seq >> 8);
    g_icmp_buf[7] = (uint8_t)(seq & 0xFF);
    memcpy_64(g_icmp_buf + 8, NET64_PING_TEXT, (uint64_t)plen);
    {
        const uint16_t c = n64_csum16(g_icmp_buf, icmp_len);
        g_icmp_buf[2] = (uint8_t)(c >> 8);
        g_icmp_buf[3] = (uint8_t)(c & 0xFF);
    }
    const int n = n64_build_ip(g_ip_frame, NET64_FRAME_MAX, NET64_IP_LOCAL64,
                               ip, 1, g_icmp_buf, icmp_len);
    if (n <= 0) return -1;
    if (n64_send_eth(mac, 0x0800, g_ip_frame, n) != 0) return -1;

    // 3) 有界等这条 echo 的 reply（按 ip+id+seq 匹配，别的回包只记不打乱）
    const uint64_t t0 = g_ticks64;
    while ((g_ticks64 - t0) < NET64_ICMP_TIMEOUT_TICKS) {
        net64_poll64();
        if (g_last_reply_ip == ip && g_last_reply_seq == seq && g_last_reply_id == NET64_ICMP_ID)
            return 0;
        n64_wait_tick();
    }
    return -3;                                         // echo 超时
}

int net64_send_ipv4(uint32_t dst_ip, uint8_t proto, const uint8_t* payload, int len) {
    if (!g_up) return -1;
    if (!payload || len < 0 || (NET64_IP_HDR + len) > NET64_FRAME_MAX) return -1;

    // 目的 MAC 不在缓存里：发 ARP 请求并做有界等待（和 ping64 的第一步同款）
    uint8_t mac[6];
    if (n64_arp_lookup(dst_ip, mac) != 0) {
        (void)net64_arp_request64(dst_ip);
        const uint64_t t0 = g_ticks64;
        while ((g_ticks64 - t0) < NET64_ARP_TIMEOUT_TICKS) {
            net64_poll64();
            if (n64_arp_lookup(dst_ip, mac) == 0) break;
            n64_wait_tick();
        }
        if (n64_arp_lookup(dst_ip, mac) != 0) return -2;    // ARP 超时
    }

    const int n = n64_build_ip(g_ip_frame, NET64_FRAME_MAX, NET64_IP_LOCAL64,
                               dst_ip, proto, payload, len);
    if (n <= 0) return -1;
    if (n64_send_eth(mac, 0x0800, g_ip_frame, n) != 0) return -1;
    return 0;
}

const char* net64_state_str64() {
    if (!g_inited) return "uninitialized";
    return g_up ? "up" : "no link";
}

uint64_t net64_tx_frames64()    { return g_tx_frames; }
uint64_t net64_rx_frames64()    { return g_rx_frames; }
uint64_t net64_arp_replies64()  { return g_arp_replies; }
uint64_t net64_icmp_replies64() { return g_icmp_replies; }
uint64_t net64_dropped64()      { return g_dropped; }

// ==================== 初始化 + 启动期一次性验收流程 ====================
static void n64_no_link_line() {
    n64_line_begin();
    dbg64_str("[NET64] no link / arp timeout (tx=");
    dbg64_dec(g_tx_frames);
    dbg64_str(" rx=");
    dbg64_dec(g_rx_frames);
    dbg64_str(" dropped=");
    dbg64_dec(g_dropped);
    dbg64_str(")");
    n64_line_end();
}

int net64_init64() {
    if (g_inited) return g_up ? 0 : -1;
    g_inited = true;

    // (a) 网卡：初始化 + 驱动自检（没网卡时打 not found / skipped，不崩）
    const int erc = e1000_init64();
    (void)e1000_selftest64();
    const uint8_t* mac = e1000_mac64();

    if (erc != 0 || !mac) {
        g_up = false;
        (void)net64_selftest64();                      // (d) 离线自检：没网卡也照跑（必过）
        n64_no_link_line();                            // (e) 优雅跳过
        return -1;
    }
    memcpy_64(g_our_mac, mac, 6);
    g_up = true;

    n64_line_begin();
    dbg64_str("[NET64] config ip=");
    n64_put_ip(NET64_IP_LOCAL64);
    dbg64_str("/24 gw=");
    n64_put_ip(NET64_IP_GATEWAY64);
    dbg64_str(" dns=");
    n64_put_ip(NET64_IP_DNS64);
    dbg64_str(" mac=");
    n64_put_mac(g_our_mac);
    n64_line_end();

    // (b) ARP 问网关，有界等 reply（handler 会在收到时打 arp reply 行）
    bool arp_ok = false;
    if (net64_arp_request64(NET64_IP_GATEWAY64) == 0) {
        uint8_t gw_mac[6];
        const uint64_t t0 = g_ticks64;
        while ((g_ticks64 - t0) < NET64_ARP_TIMEOUT_TICKS) {
            net64_poll64();
            if (n64_arp_lookup(NET64_IP_GATEWAY64, gw_mac) == 0) { arp_ok = true; break; }
            n64_wait_tick();
        }
    }

    // (c) ICMP echo 给网关 seq=1（handler 会在收到时打 icmp reply 行）
    bool icmp_ok = false;
    if (arp_ok) icmp_ok = (net64_ping64(NET64_IP_GATEWAY64, 1) == 0);

    // (d) 协议栈离线自检：不依赖网卡（校验和/IP/ARP/最小帧补零），放在探测之后打点
    const int sst = net64_selftest64();

    // (e) 任一环超时/失败：打印有界降级说明，但启动继续、自检不算失败
    if (!arp_ok || !icmp_ok) {
        n64_line_begin();
        dbg64_str("[NET64] probe arp=");
        dbg64_str(arp_ok ? "ok" : "timeout");
        dbg64_str(" icmp=");
        dbg64_str(icmp_ok ? "ok" : "timeout/no-reply");
        n64_line_end();
        n64_no_link_line();
    }
    (void)sst;
    return (arp_ok && icmp_ok) ? 0 : -1;
}

// ==================== 离线自检 ====================
int net64_selftest64() {
    int fails = 0;

    // bit0：校验和已知样本。
    //   20 字节全 0x00 -> 10 个字 0x0000，和=0，反码 = 0xFFFF；
    //   20 字节全 0xFF -> 10 个字 0xFFFF，和=0x9FFF6 -> 折叠 0xFFFF，反码 = 0x0000。
    {
        uint8_t z[20];
        for (int i = 0; i < 20; i++) z[i] = 0x00;
        if (n64_csum16(z, 20) != 0xFFFFu) fails |= 1;
        for (int i = 0; i < 20; i++) z[i] = 0xFF;
        if (n64_csum16(z, 20) != 0x0000u) fails |= 1;
    }

    // bit1：IPv4 构造/解析往返（字段、长度、载荷、校验和自洽）
    {
        const uint8_t payload[4] = {0xAA, 0xBB, 0xCC, 0xDD};
        uint8_t pkt[64];
        const int n = n64_build_ip(pkt, (int)sizeof(pkt), NET64_IP_LOCAL64,
                                   NET64_IP_GATEWAY64, 1, payload, 4);
        Net64IpView v;
        if (n != 24) fails |= 2;
        if (n64_sum16(pkt, NET64_IP_HDR) != 0xFFFFu) fails |= 2;   // 含校验和的头和应折为 0xFFFF
        if (n64_parse_ip(pkt, n, &v) != 0) {
            fails |= 2;
        } else {
            if (v.src != NET64_IP_LOCAL64 || v.dst != NET64_IP_GATEWAY64) fails |= 2;
            if (v.proto != 1 || v.ttl != NET64_TTL || v.total != 24 || v.payload_len != 4) fails |= 2;
            for (int i = 0; i < 4; i++) if (v.payload[i] != payload[i]) fails |= 2;
        }
    }

    // bit2：ARP 构造/解析往返
    {
        const uint8_t sha[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
        const uint8_t tha[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        uint8_t arp[NET64_ARP_LEN];
        Net64ArpView v;
        if (n64_build_arp(arp, (int)sizeof(arp), 1, sha, NET64_IP_LOCAL64,
                          tha, NET64_IP_GATEWAY64) != NET64_ARP_LEN) {
            fails |= 4;
        } else if (n64_parse_arp(arp, NET64_ARP_LEN, &v) != 0) {
            fails |= 4;
        } else {
            if (v.oper != 1 || v.spa != NET64_IP_LOCAL64 || v.tpa != NET64_IP_GATEWAY64) fails |= 4;
            for (int i = 0; i < 6; i++) if (v.sha[i] != sha[i] || v.tha[i] != tha[i]) fails |= 4;
        }
    }

    // bit3：以太网最小帧补零（20 字节 -> 60 字节，尾巴全 0，原有字节不动）
    {
        uint8_t fr[64];
        for (int i = 0; i < 64; i++) fr[i] = 0xA5;
        const int n = n64_eth_pad(fr, 20);
        if (n != NET64_ETH_MIN) fails |= 8;
        if (fr[19] != 0xA5) fails |= 8;
        for (int i = 20; i < NET64_ETH_MIN; i++) if (fr[i] != 0x00) fails |= 8;
    }

    n64_line_begin();
    dbg64_str("[NET64] selftest ");
    if (fails == 0) {
        dbg64_str("PASS");
    } else {
        dbg64_str("FAIL mask=");
        dbg64_dec((uint64_t)fails);
    }
    n64_line_end();
    return fails;
}
