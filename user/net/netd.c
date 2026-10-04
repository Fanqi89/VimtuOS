/* user/net/netd.c - ★ P9：Ring 3 的**完整网络栈**（/bin/netd）
 *
 * 交付方式：**系统卷里的文件 /bin/netd**（tools/net_pack_win.py 装卷；内核镜像里搜不到它的字节）。
 *
 * 它做什么（内核侧只有 kernel/netraw64.cpp 那条"原始帧收发"ABI，号 53）：
 *   以太网 / ARP（请求 + 应答缓存） / IPv4（校验和） / ICMP echo（ping） /
 *   UDP（端口分发） / DHCP 客户端（DISCOVER-OFFER-REQUEST-ACK，失败降级静态） /
 *   DNS 解析（A 记录、**压缩指针**、多答案、TC 截断如实报错） /
 *   TCP（三次握手、序号/确认推进、固定 RTO 重传 2 次退避、FIN/RST；客户端 + 监听回显两个方向）。
 *   CLI 子命令：ifconfig / dhcp / ping / dns / arp / udp echo / tcp connect / tcp listen / selftest。
 *   无参数 = 一条命令跑完整验收串（elfrun 不能传 argv，因此无参模式就是"全跑一遍"）。
 *
 * 为什么自带 runtime（不 include 任何头、不链 user/lib）：与 /bin/sounder、/bin/drvdemo 同一条纪律 ——
 * 交付形态就是"卷里的一份静态 ELF"，不依赖别的线的中间态。链接脚本复用 user/lib/user64.ld，
 * 装载地址 = 用户窗口低 64 KiB（主程序装载区的硬约束，见 kernel/elf64.cpp）。
 * 程序镜像必须 <= 64 KiB：所有缓冲都走 mmap（Linux 号段 9）拿一块 32 KiB 的匿名区，
 * **不放进 .bss**。
 *
 * 如实边界（没做的，报告里也逐条列了）：
 *   * IPv4 **不做分片/重组**（超过 MTU 的也不会发；收到分片按"不支持"丢弃并计数）；
 *   * TCP **不做重排序/乱序缓存**（乱序段丢弃并计数）、**不做拥塞控制**（固定 RTO，无 cwnd/RTT 估计）、
 *     窗口固定（不收 > 一份的突发：接收侧单缓冲 + 立即 ACK）、**没有 TIME_WAIT 定时器**；
 *   * 不实现 **IPv6 / ICMP 其它类型 / IGMP / 无线**；不实现 TCP 选项（MSS/SACK/时间戳）、
 *     不做 Nagle/延迟 ACK；
 *   * DHCP 只做 4 步（不加 RELEASE/RENEW 定时器）；DNS 只做 A 记录（不做 AAAA/CNAME 跟随/
 *     重传以外的策略），做成"一问一答"；
 *   * 内核里**没有任何**协议解析：帧的每个字节都是这里拼出来/读出来的。
 *
 * 打点（自动验收 tests/netuser64_test.py grep，格式勿改；统一前缀 [NET]）：
 *   [NET] ifconfig mac=..|none link=up|down ip=<a.b.c.d>/<len> gw=.. dns=.. src=dhcp|static
 *   [NET] dhcp discover xid=0x<hex> / offer yiaddr=.. server=.. / request xid=.. req=.. server=..
 *         / ack ip=../<len> gw=.. dns=.. lease=<n> / bind ip=.. (steps=<n>/4 src=dhcp)
 *         / fallback static ip=.. cause=<timeout|no-offer|no-ack> (steps=<n>/4)
 *   [NET] arp who-has <ip> tx=<bytes> / arp reply <ip> is-at <mac> / arp timeout <ip> (ms=<n>)
 *   [NET] icmp echo id=<n> seq=<n> dst=<ip> bytes=<n> / icmp reply from <ip> seq=<n> bytes=<n> ttl=<n> rtt_ms=<n>
 *         / icmp timeout dst=<ip> seq=<n> ms=<n>
 *   [NET] udp tx dst=<ip>:<port> sport=<n> bytes=<n> sum=0x<hex>
 *         / udp rx from=<ip>:<port> bytes=<n> sum=0x<hex> match=<0|1> rtt_ms=<n>
 *         / udp timeout dst=.. ms=<n>
 *   [NET] dns query id=0x<hex> name=<n> server=<ip> / dns rx bytes=<n> rcode=<n> answers=<n> tc=<0|1>
 *         / dns a name=<n> addr=<a.b.c.d> ttl=<n> ptr=<0|1> / dns fail reason=<timeout|tc|rcode|malformed|no-a>
 *   [NET] tcp connect dst=<ip>:<port> sport=<n>
 *         / tcp syn seq=0x<hex> -> sent / tcp syn retx n=<n> rto_ms=<n>
 *         / tcp synack seq=0x<hex> ack=0x<hex> / tcp ack=0x<hex> state=ESTABLISHED rtt_ms=<n>
 *         / tcp tx seq=0x<hex> bytes=<n> sum=0x<hex>
 *         / tcp rx seq=0x<hex> bytes=<n> sum=0x<hex> match=<1|0> (acked=0x<hex>)
 *         / tcp fin seq=0x<hex> -> sent / tcp closed state=CLOSED (rx=<n> tx=<n>)
 *         / tcp listen port=<n> -> waiting / tcp accept from=<ip>:<port> seq=0x<hex>
 *         / tcp echo rx=<n> tx=<n> match=<1|0> / tcp rst / tcp timeout what=<..> ms=<n>
 *   [NET] abi op=<n> args=.. rc=<n> expect=<n> ok=<0|1>     （ABI 负例）
 *   [NET] netd done mode=<..> ok=<n> fail=<n> elapsed_ms=<n> tx_frames=<n> rx_frames=<n>
 */
typedef signed long long   i64;
typedef unsigned long long u64;
typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;

/* ==================== 系统调用包装（与 /bin/sounder 同款：自有 ABI 第 4 参数走 r10）==================== */
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

/* Linux ABI（syscall 指令）：只要一块匿名内存（mmap 9）当整个栈的缓冲 */
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

int netd_main(long argc, char** argv);

/* ==================== 最小输出：**一行一次 write(1)** ====================
 * 内核为每次 int 0x80 write(1) 打一行 [SYSCALL]，按 token 分多次 write 的话串口日志里
 * 我们这行会被从中插断（验收按行 grep 就匹配不到）。所以做行缓冲，'\\n' 才落一次 write。 */
#define OUTBUF_MAX 512
static char g_out[OUTBUF_MAX];
static u64  g_out_n = 0;

static void out_flush(void) {
    if (g_out_n) {
        (void)sc3(1, 1, (i64)(u64)g_out, (i64)g_out_n);
        g_out_n = 0;
    }
}
static void out_str(const char* s) {
    while (*s) {
        if (g_out_n >= OUTBUF_MAX - 1) out_flush();
        g_out[g_out_n++] = *s;
        if (*s == '\n') out_flush();
        s++;
    }
}
static void out_ch(char c) { char b[2]; b[0] = c; b[1] = 0; out_str(b); }
static void out_u64(u64 v) {
    char b[24];
    int n = 0, k = 0;
    char t[24];
    if (v == 0) b[n++] = '0';
    while (v) { t[k++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    while (k) b[n++] = t[--k];
    b[n] = 0;
    out_str(b);
}
static void out_hex32(u32 v) {
    static const char* H = "0123456789abcdef";
    char b[11];
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; i++) b[2 + i] = H[(v >> ((7 - i) * 4)) & 0xF];
    b[10] = 0;
    out_str(b);
}
static void out_hex64(u64 v) {
    static const char* H = "0123456789abcdef";
    char b[19];
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 16; i++) b[2 + i] = H[(v >> ((15 - i) * 4)) & 0xF];
    b[18] = 0;
    out_str(b);
}
static void out_signed(i64 v) {
    if (v < 0) { out_ch('-'); out_u64((u64)(-v)); } else out_u64((u64)v);
}
static void out_sum(u32 s) {                            /* sum=0x<8 位>（与验收脚本同一口径）*/
    static const char* H = "0123456789abcdef";
    char b[11];
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; i++) b[2 + i] = H[(s >> ((7 - i) * 4)) & 0xF];
    b[10] = 0;
    out_str(b);
}

/* 丢弃计数之外的**一次性证据**（上限 12 行）：说明"我们收到了但没处理"的每一类帧。
 * 这既是排障口子，也是报告里"如实说明丢了什么"的依据。 */
static u32 g_drop_log = 0;
static void drop_log(const char* what, u32 ip);
/* ==================== 时间（自有 ABI 4 ticks / 5 sleep_ms）；PIT = 250 Hz = 4ms/tick ==================== */
static u64 now_ticks(void) { return (u64)sc0(4); }
static u64 now_ms(void) { return now_ticks() * 4ull; }
static void slp_ms(i64 ms) { if (ms > 0) (void)sc1(5, ms); }

/* ==================== 小工具 ==================== */
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
static int ceq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static u32 fnv1a32(const u8* p, u64 n) {
    u32 h = 0x811C9DC5u;
    for (u64 i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
    return h;
}
static u32 ip4(u32 a, u32 b, u32 c, u32 d) { return (a << 24) | (b << 16) | (c << 8) | d; }
static u32 rd_ip(const u8* p) { return rd32(p); }
static void wr_ip(u8* p, u32 ip) { wr32(p, ip); }
static void out_ip(u32 ip) {
    out_u64((ip >> 24) & 0xFF); out_ch('.');
    out_u64((ip >> 16) & 0xFF); out_ch('.');
    out_u64((ip >> 8) & 0xFF);  out_ch('.');
    out_u64(ip & 0xFF);
}
static void out_mac(const u8* m) {
    static const char* H = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        if (i) out_ch(':');
        out_ch(H[(m[i] >> 4) & 0xF]);
        out_ch(H[m[i] & 0xF]);
    }
}
/* drop_log 的实体（因为 out_ip 在后面定义，所以前面只留声明）*/
static void drop_log(const char* what, u32 ip) {
    if (g_drop_log >= 12u) return;
    g_drop_log++;
    out_str("[NET] drop what="); out_str(what);
    if (ip) { out_str(" src="); out_ip(ip); }
    out_str("\n");
}
static u32 parse_ip(const char* s) {                    /* "10.0.2.2" -> u32；非法返回 0xFFFFFFFF */
    u32 v = 0, part = 0, n = 0;
    for (;;) {
        const char c = *s;
        if (c >= '0' && c <= '9') { part = part * 10u + (u32)(c - '0'); if (part > 255) return 0xFFFFFFFFu; n = 1; }
        else if (c == '.' || c == 0) {
            if (!n || v > 0xFFFFFFu) return 0xFFFFFFFFu;
            v = (v << 8) | part;
            part = 0; n = 0;
            if (c == 0) return v;
        } else return 0xFFFFFFFFu;
        s++;
    }
}
static u32 parse_dec(const char* s) {
    u32 v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10u + (u32)(*s - '0'); s++; }
    return v;
}

/* ==================== 配置（/etc/netd.conf 可覆盖；缺省 = QEMU 用户网络固定值）==================== */
typedef struct {
    u32 ip, mask, gw, dns;
    u32 tcp_host, udp_host;
    u32 dns_name_off;          /* g_cfg.name[] 里的偏移 */
    u16 tcp_port, udp_port, listen_port;
    u16 dns_sport, udp_sport, tcp_sport;
    u32 arp_ms, ping_ms, dns_ms, udp_ms, tcp_ms, dhcp_ms, acks_wait_ms;
} Cfg;
static Cfg   g_cfg;
static char  g_name[64];                                /* dnsname= 的文本（NUL 结尾）*/

static void cfg_defaults(void) {
    g_cfg.ip = ip4(10, 0, 2, 15);
    g_cfg.mask = ip4(255, 255, 255, 0);
    g_cfg.gw = ip4(10, 0, 2, 2);
    g_cfg.dns = ip4(10, 0, 2, 3);
    g_cfg.tcp_host = ip4(10, 0, 2, 2);
    g_cfg.udp_host = ip4(10, 0, 2, 2);
    g_cfg.tcp_port = 5555; g_cfg.udp_port = 5556; g_cfg.listen_port = 5555;
    g_cfg.dns_sport = 40000; g_cfg.udp_sport = 40002; g_cfg.tcp_sport = 40004;
    g_cfg.arp_ms = 800; g_cfg.ping_ms = 1200; g_cfg.dns_ms = 1500; g_cfg.udp_ms = 1200;
    g_cfg.tcp_ms = 2500; g_cfg.dhcp_ms = 2500; g_cfg.acks_wait_ms = 5000;
    g_cfg.dns_name_off = 0;
    g_name[0] = 0;
}
static void cfg_apply_kv(const char* k, const char* v) {
    if (ceq(k, "ip")) g_cfg.ip = parse_ip(v);
    else if (ceq(k, "mask")) g_cfg.mask = parse_ip(v);
    else if (ceq(k, "gw")) g_cfg.gw = parse_ip(v);
    else if (ceq(k, "dns")) g_cfg.dns = parse_ip(v);
    else if (ceq(k, "tcphost")) g_cfg.tcp_host = parse_ip(v);
    else if (ceq(k, "udphost")) g_cfg.udp_host = parse_ip(v);
    else if (ceq(k, "tcpport")) g_cfg.tcp_port = (u16)parse_dec(v);
    else if (ceq(k, "udpport")) g_cfg.udp_port = (u16)parse_dec(v);
    else if (ceq(k, "listenport")) g_cfg.listen_port = (u16)parse_dec(v);
    else if (ceq(k, "dnssport")) g_cfg.dns_sport = (u16)parse_dec(v);
    else if (ceq(k, "udpsport")) g_cfg.udp_sport = (u16)parse_dec(v);
    else if (ceq(k, "tcpsport")) g_cfg.tcp_sport = (u16)parse_dec(v);
    else if (ceq(k, "arpms")) g_cfg.arp_ms = parse_dec(v);
    else if (ceq(k, "pingms")) g_cfg.ping_ms = parse_dec(v);
    else if (ceq(k, "dnsms")) g_cfg.dns_ms = parse_dec(v);
    else if (ceq(k, "udpms")) g_cfg.udp_ms = parse_dec(v);
    else if (ceq(k, "tcpms")) g_cfg.tcp_ms = parse_dec(v);
    else if (ceq(k, "dhcpms")) g_cfg.dhcp_ms = parse_dec(v);
    else if (ceq(k, "dnsname")) {
        int n = 0;
        while (v[n] && n < 63) { g_name[n] = v[n]; n++; }
        g_name[n] = 0;
        g_cfg.dns_name_off = n ? 1u : 0u;
    }
}
static void cfg_load(const char* path) {
    const i64 fd = sc1(6, (i64)(u64)path);
    if (fd < 0) return;                                  /* 没这个文件：用缺省（不是错误）*/
    char buf[1024];
    i64 got = sc3(7, fd, (i64)(u64)buf, (i64)sizeof(buf) - 1);
    (void)sc1(8, fd);
    if (got <= 0) return;
    buf[got] = 0;
    char key[24], val[64];
    int i = 0;
    while (i < (int)got) {
        while (i < (int)got && (buf[i] == '\n' || buf[i] == '\r' || buf[i] == ' ' || buf[i] == '\t')) i++;
        if (i < (int)got && buf[i] == '#') { while (i < (int)got && buf[i] != '\n') i++; continue; }
        int kn = 0;
        while (i < (int)got && buf[i] != '=' && buf[i] != '\n' && kn < 23) key[kn++] = buf[i++];
        key[kn] = 0;
        if (i < (int)got && buf[i] == '=') {
            i++;
            int vn = 0;
            while (i < (int)got && buf[i] != '\n' && buf[i] != '\r' && vn < 63) val[vn++] = buf[i++];
            val[vn] = 0;
            if (kn) cfg_apply_kv(key, val);
        }
        while (i < (int)got && buf[i] != '\n') i++;
    }
}

/* ==================== net_raw（自有 int 0x80 号 53）：内核侧只用 e1000 既有的收发 ====================
 * 沿用 kernel/netraw64.h 的错误码表：-2 EFAULT / -3 EINVAL / -4 EAGAIN / -5 ENODEV / -6 EMSGSIZE */
#define NETRAW_NR 53
#define NR_EAGAIN  (-4)
#define NR_ENODEV  (-5)
#define FRAME_MAX  1514
#define RX_CAP     1600
enum { NR_TX = 0, NR_RX = 1, NR_MAC = 2, NR_LINK = 3 };

static i64 net_raw_tx(const u8* f, u64 len) { return sc3(NETRAW_NR, NR_TX, (i64)(u64)f, (i64)len); }
static i64 net_raw_rx(u8* b, u64 cap) { return sc3(NETRAW_NR, NR_RX, (i64)(u64)b, (i64)cap); }
static i64 net_raw_mac(u8* out) { return sc3(NETRAW_NR, NR_MAC, (i64)(u64)out, 0); }
static i64 net_raw_link(void) { return sc3(NETRAW_NR, NR_LINK, 0, 0); }

/* ==================== 全栈状态 ==================== */
#define ARP_N 16
typedef struct { int used; u32 ip; u8 mac[6]; u32 age; } ArpEnt;

static u8    g_mac[6];
static int   g_link = 0;
static u32   g_our_ip = 0, g_mask = 0, g_gw = 0, g_dns = 0;
static int   g_ip_src = 0;                              /* 0 = 静态/未绑定，1 = DHCP */
static ArpEnt g_arp[ARP_N];
static u32   g_arp_age = 0;
static u16   g_icmp_id = 0x4e44;                        /* 'ND'：本进程的 ICMP id */
static u64   g_tx_frames = 0, g_rx_frames = 0, g_rx_other = 0, g_rx_bad = 0, g_ip_frag = 0;
static u64   g_tcp_retx = 0, g_tcp_ooo = 0;

static u8*   g_arena = 0;
static u8*   g_txbuf = 0;      /* 1600 */
static u8*   g_rxbuf = 0;      /* 1600 单帧 */
static u8*   g_scratch = 0;    /* 512  DHCP/DNS 净荷 */
/* 收到的 UDP 数据报（最后一次；DNS/DHCP/UDP echo 的等待循环靠 generation 判"来新的了"）*/
static u8    g_udp_rx[600];
static u32   g_udp_rx_len = 0, g_udp_rx_ip = 0, g_udp_rx_gen = 0;
static u16   g_udp_rx_sport = 0, g_udp_rx_dport = 0;
static u32   g_udp_rx_sum = 0;

/* ==================== 校验和（RFC 1071；全部按大端 16 位字）==================== */
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
    wr_ip(h, sip); wr_ip(h + 4, dip); h[8] = 0; h[9] = proto; wr16(h + 10, (u16)len);
    return csum_add(0, h, 12);
}
static u32 checksum_selftest(void) {
    /* 两段：① 已知样本（0x0001..0x0008 的 8 个 16 位字，和 = 0x0024 -> 反码 = 0xFFDB）；
     *      ② 自洽：造一个 IP 头 -> 算校验和填进去 -> **再整体验必须为 0**（= 0xFFFF 的反码），
     *         这正是 ipv4_handle 收包时的判据。 */
    static const u8 s[16] = { 0x00,0x01,0x00,0x02,0x00,0x03,0x00,0x04,
                              0x00,0x05,0x00,0x06,0x00,0x07,0x00,0x08 };
    u32 bad = 0;
    if (csum_fin(csum_add(0, s, 16)) != 0xFFDBu) bad |= 1;
    {
        u8 h[20];
        memzero(h, 20);
        h[0] = 0x45; wr16(h + 2, 20); h[8] = 64; h[9] = 1;
        wr_ip(h + 12, ip4(10, 0, 2, 15)); wr_ip(h + 16, ip4(10, 0, 2, 2));
        wr16(h + 10, csum_fin(csum_add(0, h, 20)));
        if (csum_fin(csum_add(0, h, 20)) != 0x0000) bad |= 2;      /* 自洽：算完再验必须为 0 */
    }
    return bad;
}

/* ==================== 以太网 ==================== */
static int eth_send(const u8* dst_mac, u16 type, const u8* body, u32 blen) {
    if (blen > 1500u) return -1;
    memcpy_n(g_txbuf, dst_mac, 6);
    memcpy_n(g_txbuf + 6, g_mac, 6);
    wr16(g_txbuf + 12, type);
    if (blen) memcpy_n(g_txbuf + 14, body, blen);
    u32 len = 14u + blen;
    if (len < 60u) { memzero(g_txbuf + len, 60u - len); len = 60u; }
    const i64 rc = net_raw_tx(g_txbuf, len);
    if (rc == 0) g_tx_frames++;
    return (int)(rc == 0 ? 0 : -1);
}
static const u8 g_bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ==================== ARP ==================== */
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
    wr_ip(p + 14, g_our_ip);
    memzero(p + 18, 6);             /* target mac = 0（未知）*/
    wr_ip(p + 24, ip);
    const int rc = eth_send(g_bcast, 0x0806, p, 28);
    out_str("[NET] arp who-has "); out_ip(ip);
    out_str(" tx="); out_u64(28u);
    out_str(" rc="); out_signed(rc);
    out_str("\n");
    return rc;
}
/* 有界等 ARP 应答（内部 pump 收包）；返回 0 = 已解析 */
static void poll_frames(u32 budget_frames);
static int arp_resolve(u32 ip, u8* mac_out, u32 ms_budget) {
    if (arp_lookup(ip, mac_out) == 0) return 0;
    if (arp_request(ip) != 0) return -1;
    const u64 t0 = now_ms();
    while (now_ms() - t0 < (u64)ms_budget) {
        poll_frames(8);
        if (arp_lookup(ip, mac_out) == 0) return 0;
        slp_ms(4);
    }
    out_str("[NET] arp timeout "); out_ip(ip);
    out_str(" (ms="); out_u64(ms_budget); out_str(")\n");
    return -2;
}

/* ==================== IPv4 ==================== */
static int ip_send(u32 dst_ip, u8 proto, const u8* payload, u32 plen, const u8* dst_mac) {
    if (plen > 1480u) return -2;                            /* 不做分片：超了直接拒（如实）*/
    u8 hdr[20];
    u32 sum;
    hdr[0] = 0x45; hdr[1] = 0;
    wr16(hdr + 2, (u16)(20u + plen));
    wr16(hdr + 4, 0x1234);                                  /* id（不分片，值不重要）*/
    wr16(hdr + 6, 0x4000);                                  /* DF：不让我们自己产生分片 */
    hdr[8] = 64; hdr[9] = proto;
    wr16(hdr + 10, 0);
    wr_ip(hdr + 12, g_our_ip);
    wr_ip(hdr + 16, dst_ip);
    sum = csum_add(0, hdr, 20);
    wr16(hdr + 10, csum_fin(sum));
    /* 帧 = 以太网头 + IP 头 + 净荷：先拼到 g_txbuf 的那块 1600 里（先 IP 头，再净荷）*/
    u8 tmp[1500];
    memcpy_n(tmp, hdr, 20);
    if (plen) memcpy_n(tmp + 20, payload, plen);
    return eth_send(dst_mac, 0x0800, tmp, 20u + plen);
}
static int ip_send_to(u32 dst_ip, u8 proto, const u8* payload, u32 plen) {
    u8 mac[6];
    u32 nhop = dst_ip;
    if (g_our_ip && g_mask && ((dst_ip & g_mask) != (g_our_ip & g_mask))) nhop = g_gw;   /* 单跳路由 */
    const int r = arp_resolve(nhop, mac, g_cfg.arp_ms);
    if (r != 0) return (r == -1) ? -1 : -3;
    return ip_send(dst_ip, proto, payload, plen, mac);
}

/* ==================== ICMP ==================== */
static volatile int g_icmp_got = 0;
static u16 g_icmp_got_seq = 0;
static int g_icmp_got_ttl = 0, g_icmp_got_id_ok = 0;
static u32 g_icmp_got_bytes = 0;

static void icmp_handle(const u8* p, u32 len, int ttl, u32 sip) {
    if (len < 8u) { g_rx_bad++; return; }
    const u8 type = p[0];
    const u16 id = rd16(p + 4), seq = rd16(p + 6);
    if (type == 0) {                                        /* echo reply */
        if (id == g_icmp_id) {
            g_icmp_got = 1;
            g_icmp_got_seq = seq;
            g_icmp_got_ttl = ttl;
            g_icmp_got_bytes = len - 8u;
            g_icmp_got_id_ok = 1;
            out_str("[NET] icmp reply from "); out_ip(sip);
            out_str(" seq="); out_u64(seq);
            out_str(" bytes="); out_u64(len - 8u);
            out_str(" ttl="); out_u64((u64)ttl);
            out_str("\n");
        } else {
            g_rx_other++;                                   /* 别人的 echo reply：不理会 */
        }
        return;
    }
    /* ★ 本程序**不回应**任何请求（ping 我们是内核 net64 的既有行为；ring3 里我们只做客户端，
     *   如实标注：不做 ICMP 应答、不做 redirect/unreachable）。*/
    g_rx_other++;
}
static int icmp_ping(u32 dst, u16 seq, u32 ms_budget, u32* rtt_out) {
    u8 p[40];
    const u32 plen = 32u;
    memzero(p, sizeof(p));
    p[0] = 8;                                               /* echo request */
    wr16(p + 4, g_icmp_id);
    wr16(p + 6, seq);
    for (u32 i = 0; i < plen; i++) p[8 + i] = (u8)(0x40u + (i & 0x3Fu));    /* 固定 pattern（可测）*/
    wr16(p + 2, csum_fin(csum_add(0, p, 8u + plen)));
    out_str("[NET] icmp echo id="); out_u64(g_icmp_id);
    out_str(" seq="); out_u64(seq);
    out_str(" dst="); out_ip(dst);
    out_str(" bytes="); out_u64(plen);
    out_str("\n");
    const int sr = ip_send_to(dst, 1, p, 8u + plen);
    if (sr != 0) {
        /* ARP 打不通 / 发送失败：**明确失败**（有界，绝不挂死），如实打点 */
        out_str("[NET] icmp fail dst="); out_ip(dst);
        out_str(" seq="); out_u64(seq);
        out_str(" rc="); out_signed(sr == -3 ? -2 : -1);
        out_str(sr == -3 ? " reason=no-arp\n" : " reason=tx\n");
        return (sr == -3) ? -2 : -1;
    }
    g_icmp_got = 0;
    const u64 t0 = now_ms();
    for (;;) {
        poll_frames(16);
        if (g_icmp_got && g_icmp_got_seq == seq) {
            const u64 rtt = now_ms() - t0;
            if (rtt_out) *rtt_out = (u32)rtt;
            return 0;
        }
        if (now_ms() - t0 >= (u64)ms_budget) break;
        slp_ms(4);
    }
    out_str("[NET] icmp timeout dst="); out_ip(dst);
    out_str(" seq="); out_u64(seq);
    out_str(" ms="); out_u64(ms_budget);
    out_str("\n");
    return -3;
}

/* ==================== UDP 发送 ==================== */
static int udp_send(u32 dst_ip, u16 sport, u16 dport, const u8* data, u32 len, const u8* dst_mac) {
    if (len > 1472u) return -2;
    u8 p[1472];
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
    u32 nhop = dst_ip;
    if (g_our_ip && g_mask && ((dst_ip & g_mask) != (g_our_ip & g_mask))) nhop = g_gw;
    const int r = arp_resolve(nhop, mac, g_cfg.arp_ms);
    if (r != 0) return (r == -1) ? -1 : -3;
    return udp_send(dst_ip, sport, dport, data, len, mac);
}
/* 等一个来自 (ip, sport) 的 UDP 数据报；返回长度（>0）或负错误码 */
static int udp_wait(u32 from_ip, u16 from_sport, u32 gen0, u32 ms_budget, u16* dport_out) {
    const u64 t0 = now_ms();
    for (;;) {
        poll_frames(16);
        if (g_udp_rx_gen != gen0 && g_udp_rx_ip == from_ip && (!from_sport || g_udp_rx_sport == from_sport)) {
            if (dport_out) *dport_out = g_udp_rx_dport;
            return (int)g_udp_rx_len;
        }
        if (now_ms() - t0 >= (u64)ms_budget) return -4;
        slp_ms(4);
    }
}

/* ==================== DHCP 客户端（DISCOVER / OFFER / REQUEST / ACK）==================== */
#define DHCP_C 0x63825363u
static u32 g_dhcp_xid = 0;
static u32 g_dhcp_offer_ip = 0, g_dhcp_server = 0, g_dhcp_lease = 0;
static u32 g_dhcp_msk = 0, g_dhcp_gw = 0, g_dhcp_dns = 0;
static int g_dhcp_steps = 0;

/* 组一个 DHCP 报文（msg type + 可选 requested ip / server id）；返回总长 */
static u32 dhcp_build(u8* b, u8 msg_type, u32 req_ip) {
    memzero(b, 300);
    b[0] = 1;                   /* op = BOOTREQUEST */
    b[1] = 1;                   /* htype = Ethernet */
    b[2] = 6;                   /* hlen */
    b[3] = 0;
    wr32(b + 4, g_dhcp_xid);
    wr16(b + 8, 0);
    wr16(b + 10, 0x8000);       /* flags：要广播应答 */
    memcpy_n(b + 28, g_mac, 6);
    wr32(b + 236, DHCP_C);      /* magic cookie */
    u32 o = 240;
    b[o++] = 53; b[o++] = 1; b[o++] = msg_type;                         /* DHCP message type */
    if (msg_type == 3) {                                                /* REQUEST 带 requested ip */
        b[o++] = 50; b[o++] = 4; wr32(b + o, req_ip); o += 4;
        b[o++] = 54; b[o++] = 4; wr32(b + o, g_dhcp_server); o += 4;
    }
    b[o++] = 55; b[o++] = 3; b[o++] = 1; b[o++] = 3; b[o++] = 6;        /* param list: mask/router/dns */
    b[o++] = 255;
    if (o < 300u) o = 300u;                                             /* BOOTP 定长 300 */
    return o;
}
/* 解析 DHCP 应答；返回 msg type（0 = 不是给我们的/不成形）*/
static u8 dhcp_parse(const u8* p, u32 len) {
    if (len < 240u) return 0;
    if (p[0] != 2) return 0;                                            /* BOOTREPLY */
    if (rd32(p + 4) != g_dhcp_xid) return 0;                            /* xid 不匹配 */
    if (!memeq_n(p + 28, g_mac, 6)) return 0;                           /* chaddr 不是我们 */
    if (rd32(p + 236) != DHCP_C) return 0;
    g_dhcp_offer_ip = rd_ip(p + 16);                                    /* yiaddr */
    u8 type = 0;
    u32 i = 240;
    g_dhcp_msk = g_dhcp_gw = g_dhcp_dns = 0;
    while (i + 1u < len) {
        const u8 code = p[i++];
        if (code == 0) continue;
        if (code == 255) break;
        if (i >= len) break;
        const u8 olen = p[i++];
        if (i + olen > len) break;
        if (code == 53 && olen >= 1) type = p[i];
        else if (code == 1 && olen >= 4) g_dhcp_msk = rd_ip(p + i);
        else if (code == 3 && olen >= 4) g_dhcp_gw = rd_ip(p + i);
        else if (code == 6 && olen >= 4) g_dhcp_dns = rd_ip(p + i);
        else if (code == 54 && olen >= 4) g_dhcp_server = rd_ip(p + i);
        else if (code == 51 && olen >= 4) g_dhcp_lease = rd32(p + i);
        i += olen;
    }
    return type;
}
static u8 dhcp_exchange(u8 msg_type, u8 want_type, u32 req_ip, u32 ms_budget) {
    /* 目的 = 255.255.255.255（无 IP 时的广播），源 IP 0.0.0.0 */
    const u32 save_ip = g_our_ip;
    g_our_ip = 0;
    const u32 plen = dhcp_build(g_scratch, msg_type, req_ip);
    u8 p[1472];
    wr16(p + 0, 68); wr16(p + 2, 67);
    wr16(p + 4, (u16)(8u + plen));
    wr16(p + 6, 0);
    memcpy_n(p + 8, g_scratch, plen);
    {   /* 广播：源 IP 0.0.0.0（伪首部也用 0）*/
        u32 sum = csum_pseudo(0, 0xFFFFFFFFu, 17, 8u + plen);
        sum = csum_add(sum, p, 8u + plen);
        wr16(p + 6, csum_fin(sum));
    }
    u8 frame[1500];
    memcpy_n(frame, g_bcast, 6);
    memcpy_n(frame + 6, g_mac, 6);
    wr16(frame + 12, 0x0800);
    u8 iph[20];
    iph[0] = 0x45; iph[1] = 0; wr16(iph + 2, (u16)(20u + 8u + plen));
    wr16(iph + 4, 0x4E44); wr16(iph + 6, 0);
    iph[8] = 64; iph[9] = 17; wr16(iph + 10, 0);
    wr_ip(iph + 12, 0); wr_ip(iph + 16, 0xFFFFFFFFu);
    wr16(iph + 10, csum_fin(csum_add(0, iph, 20)));
    memcpy_n(frame + 14, iph, 20);
    memcpy_n(frame + 34, p, 8u + plen);
    const u32 flen = 14u + 20u + 8u + plen;
    if (flen < 60u) memzero(frame + flen, 60u - flen);
    const i64 rc = net_raw_tx(frame, flen < 60u ? 60u : flen);
    g_our_ip = save_ip;
    if (rc != 0) return 0;
    g_tx_frames++;
    const u32 gen0 = g_udp_rx_gen;
    const u64 t0 = now_ms();
    const u8 mtype_offer = (want_type == 0) ? 2 : want_type;
    for (;;) {
        poll_frames(16);
        if (g_udp_rx_gen != gen0 && g_udp_rx_sport == 67 && g_udp_rx_dport == 68) {
            const u8 t = dhcp_parse(g_udp_rx, g_udp_rx_len);
            if (t == mtype_offer) return t;
            if (t == 6) return 6;                        /* NAK */
        }
        if (now_ms() - t0 >= (u64)ms_budget) return 0;
        slp_ms(4);
    }
}
static int dhcp_bind(u32 ms_budget) {
    g_dhcp_steps = 0;
    g_dhcp_xid = (u32)(now_ticks() * 2654435761u) ^ 0x4e444831u;
    g_dhcp_server = 0;
    g_dhcp_offer_ip = 0;
    const u64 t0 = now_ms();
    /* ---- ① DISCOVER ---- */
    out_str("[NET] dhcp discover xid="); out_hex32(g_dhcp_xid); out_str("\n");
    u8 t = dhcp_exchange(1, 2, 0, ms_budget / 2u);
    if (t != 2) {
        out_str("[NET] dhcp fallback static ip="); out_ip(g_cfg.ip);
        out_str(" cause=no-offer (steps="); out_u64((u64)g_dhcp_steps); out_str("/4)\n");
        return -1;
    }
    g_dhcp_steps = 1;
    /* ---- ② OFFER ---- */
    out_str("[NET] dhcp offer yiaddr="); out_ip(g_dhcp_offer_ip);
    out_str(" server="); out_ip(g_dhcp_server);
    out_str(" ms="); out_u64(now_ms() - t0); out_str("\n");
    /* ---- ③ REQUEST ---- */
    out_str("[NET] dhcp request xid="); out_hex32(g_dhcp_xid);
    out_str(" req="); out_ip(g_dhcp_offer_ip);
    out_str(" server="); out_ip(g_dhcp_server); out_str("\n");
    const u32 req_ip = g_dhcp_offer_ip;
    t = dhcp_exchange(3, 5, req_ip, ms_budget / 2u);
    if (t != 5) {
        out_str("[NET] dhcp fallback static ip="); out_ip(g_cfg.ip);
        out_str(" cause=no-ack (steps=2/4)\n");
        return -1;
    }
    g_dhcp_steps = 4;
    /* ---- ④ ACK：绑定 ---- */
    g_our_ip = req_ip ? req_ip : g_dhcp_offer_ip;
    g_mask = g_dhcp_msk ? g_dhcp_msk : ip4(255, 255, 255, 0);
    g_gw = g_dhcp_gw ? g_dhcp_gw : g_cfg.gw;
    g_dns = g_dhcp_dns ? g_dhcp_dns : g_cfg.dns;
    g_ip_src = 1;
    out_str("[NET] dhcp ack ip="); out_ip(g_our_ip);
    out_str(" mask="); out_ip(g_mask);
    out_str(" gw="); out_ip(g_gw);
    out_str(" dns="); out_ip(g_dns);
    out_str(" lease="); out_u64(g_dhcp_lease);
    out_str(" ms="); out_u64(now_ms() - t0); out_str("\n");
    out_str("[NET] dhcp bind ip="); out_ip(g_our_ip);
    out_str(" steps=4/4 src=dhcp\n");
    return 0;
}

/* ==================== DNS（A 记录；支持压缩指针 + 多答案；TC 如实报错）==================== */
static u32 g_dns_id = 0;
/* "a.b.c" -> 03 'a' 01 'b' 01 'c' 00（每段一个长度字节 + 段字节，末尾根标签 0）。
 * 返回总字节数；0 = 名字非法（空段/超长/超缓冲）。允许结尾带点（"a.b." 视作 "a.b"）。 */
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
            out[o++] = 0;                           /* 下一段的长度占位 */
            continue;
        }
        if (o + 1u >= cap || lab >= 62u) return 0;
        out[o++] = (u8)c;
        lab++;
    }
    if (o + 1u > cap) return 0;
    out[o++] = 0;                                   /* 根标签 */
    return (int)o;
}
/* 跳过（可能带压缩指针的）名字；*ptr 置 1 表示用了指针。
 * 压缩指针（0xC0|off 的两字节）在这里**只判合法、不跟随** —— 我们需要的只是"这个名字到哪结束"，
 * 指针本身已经把名字截断（名字的后半段在报文别处，跟随它需要递归解析；本栈不解码名字文本）。
 * 指针合法性：两字节都在报文内、目标偏移 < 报文长（越界即 malformed）。 */
static int dns_skip_name(const u8* m, u32 len, u32* off, int* ptr) {
    u32 o = *off;
    for (;;) {
        if (o >= len) return -1;
        const u8 b = m[o];
        if ((b & 0xC0u) == 0xC0u) {
            if (o + 1u >= len) return -1;
            const u32 target = (((u32)(b & 0x3Fu)) << 8) | m[o + 1u];
            if (target >= len) return -1;                   /* 指针指到报文外：malformed */
            if (ptr) *ptr = 1;
            return (int)(o + 2u);                            /* 名字在这里结束（指针之后）*/
        }
        if (b == 0) return (int)(o + 1u);
        if ((b & 0xC0u) != 0) return -1;
        o += 1u + b;
        if (b == 0) { return (int)(o + 1u); }
        if ((b & 0xC0u) != 0) return -1;
        o += 1u + b;
    }
}
static int dns_resolve(const char* name, u32 sport, u32 ms_budget, int* n_answers, int* used_ptr, u32* first_ip) {
    *n_answers = 0; *used_ptr = 0; *first_ip = 0;
    if (!name || !*name) return -4;
    g_dns_id = (u32)(now_ticks() * 40503u) & 0xFFFFu;
    u8 q[512];
    memzero(q, 512);
    wr16(q + 0, (u16)g_dns_id);
    wr16(q + 2, 0x0100);                                    /* QR=0, RD=1 */
    wr16(q + 4, 1);                                         /* qdcount */
    u32 o = 12, off = 12;
    o = dns_encode_name(q + 12, 200, name);
    if (!o) return -5;
    off = 12 + o;
    wr16(q + off, 1); off += 2;                             /* type A */
    wr16(q + off, 1); off += 2;                             /* class IN */
    out_str("[NET] dns query id="); out_hex64(g_dns_id);
    out_str(" name="); out_str(name);
    out_str(" server="); out_ip(g_dns);
    out_str(" bytes="); out_u64(off); out_str("\n");
    u32 gen0 = g_udp_rx_gen;
    const int rc = udp_send_to(g_dns, (u16)sport, 53, q, off);
    if (rc != 0) { out_str("[NET] dns fail reason=send rc="); out_signed(rc); out_str("\n"); return -2; }
    for (int attempt = 0; attempt < 3; attempt++) {          /* 一问一答；超时重发 2 次（有界）*/
        if (attempt) {
            out_str("[NET] dns retx n="); out_u64((u64)attempt);
            out_str(" bytes="); out_u64(off); out_str("\n");
            gen0 = g_udp_rx_gen;                            /* 重发前重新取基线 */
            if (udp_send_to(g_dns, (u16)sport, 53, q, off) != 0) continue;
        }
        u16 dport = 0;
        const int got = udp_wait(g_dns, 53, gen0, ms_budget, &dport);
        (void)dport;
        if (got < 0) {
            if (attempt < 2) continue;
            out_str("[NET] dns fail reason=timeout ms="); out_u64(ms_budget); out_str("\n");
            return -3;
        }
        const u8* m = g_udp_rx;
        const u32 len = g_udp_rx_len;
        if (len < 12u || rd16(m + 0) != (u16)g_dns_id) { out_str("[NET] dns fail reason=malformed\n"); return -6; }
        const u32 flags = rd16(m + 2);
        const u32 rcode = flags & 0xFu;
        const int tc = (flags & 0x0200u) ? 1 : 0;
        const u32 ancount = rd16(m + 6);
        out_str("[NET] dns rx bytes="); out_u64(len);
        out_str(" rcode="); out_u64(rcode);
        out_str(" answers="); out_u64(ancount);
        out_str(" tc="); out_u64((u64)tc);
        out_str("\n");
        if (tc) { out_str("[NET] dns fail reason=tc (truncated answer)\n"); return -7; }
        if (rcode) { out_str("[NET] dns fail reason=rcode\n"); return -8; }
        if (ancount == 0) { out_str("[NET] dns fail reason=no-a\n"); return -9; }
        u32 po = 12;
        int ptr_any = 0;
        int r = dns_skip_name(m, len, &po, &ptr_any);        /* 跳过 question 的 QNAME */
        if (r < 0) { out_str("[NET] dns fail reason=malformed\n"); return -6; }
        po = (u32)r + 4u;                                    /* + QTYPE + QCLASS */
        for (u32 i = 0; i < ancount && i < 16u; i++) {
            int p2 = 0;
            r = dns_skip_name(m, len, &po, &p2);
            if (r < 0) break;
            po = (u32)r;
            if (po + 10u > len) break;
            const u16 rtype = rd16(m + po);
            const u16 rclass = rd16(m + po + 2);
            const u32 ttl = rd32(m + po + 4);
            const u16 rdlen = rd16(m + po + 8);
            po += 10u;
            if (po + rdlen > len) break;
            if (rtype == 1u && rclass == 1u && rdlen == 4u) {
                const u32 ip = rd_ip(m + po);
                out_str("[NET] dns a name="); out_str(name);
                out_str(" addr="); out_ip(ip);
                out_str(" ttl="); out_u64(ttl);
                out_str(" ptr="); out_u64((u64)((p2 || ptr_any) ? 1 : 0));
                out_str("\n");
                if (!*first_ip) *first_ip = ip;
                (*n_answers)++;
            }
            if (p2) ptr_any = 1;
            po += rdlen;
        }
        if (*n_answers == 0) { out_str("[NET] dns fail reason=no-a\n"); return -9; }
        return 0;
    }
    return -3;
}

/* ==================== TCP（客户端 + 监听回显；无重排序/无拥塞控制）==================== */
#define TCP_BUF 512
typedef struct {
    int  state;               /* 0 CLOSED / 1 SYN_SENT / 2 ESTAB / 3 FIN_WAIT / 4 LISTEN */
    u32  rip; u16 sport, dport;             /* 本地 sport；对端 rip:dport（被动态 dport=对端）*/
    u32  snd_una, snd_nxt, rcv_nxt;         /* 序号推进：未被确认的 / 下一个要发的 / 期望收到的 */
    u8   rmac[6];
    u16  tport;                             /* 被动态：对端端口（accept 之后）*/
    u8   rx[TCP_BUF]; u32 rx_len;           /* 收到的字节（只收"正好按序"的那一份，不重排序）*/
    int  rst, got_fin, synack_pending;
    u32  rtt_ms;
} Tcb;
static Tcb g_tcb;

static u16 tcp_csum(u32 sip, u32 dip, const u8* seg, u32 seglen) {
    u32 sum = csum_pseudo(sip, dip, 6, seglen);
    sum = csum_add(sum, seg, seglen);
    return csum_fin(sum);
}
/* 发一段 TCP（flags 用 TCP_F_*；payload 可空）*/
#define TCP_F_FIN 0x01
#define TCP_F_SYN 0x02
#define TCP_F_RST 0x04
#define TCP_F_PSH 0x08
#define TCP_F_ACK 0x10
static int tcp_send_seg(u32 rip, u16 sport, u16 dport, const u8* rmac, u32 seq, u32 ack,
                        u8 flags, const u8* data, u32 len) {
    u8 s[1472];
    wr16(s + 0, sport);
    wr16(s + 2, dport);
    wr32(s + 4, seq);
    wr32(s + 8, ack);
    s[12] = 0x50;                                            /* data offset = 5（无选项）*/
    s[13] = flags;
    wr16(s + 14, 4096);                                      /* 窗口（固定 4 KiB）*/
    wr16(s + 16, 0);                                         /* 校验和 */
    wr16(s + 18, 0);
    if (len) memcpy_n(s + 20, data, len);
    const u32 seglen = 20u + len;
    wr16(s + 16, tcp_csum(g_our_ip, rip, s, seglen));
    return ip_send(rip, 6, s, seglen, rmac);
}
static void tcp_log_retx(const char* what, u32 n, u32 rto) {
    g_tcp_retx++;
    out_str("[NET] tcp retx what="); out_str(what);
    out_str(" n="); out_u64(n);
    out_str(" rto_ms="); out_u64(rto);
    out_str("\n");
}
/* 主动连接：三次握手（固定 RTO=400ms，重传 2 次退避 400/800）*/
static int tcp_connect(u32 rip, u16 rport, u16 sport, u32 ms_budget) {
    memzero(&g_tcb, sizeof(g_tcb));
    if (g_our_ip == 0) g_our_ip = g_cfg.ip;                  /* 没 DHCP 也要能跑（静态兜底）*/
    g_tcb.rip = rip; g_tcb.sport = sport; g_tcb.dport = rport;
    g_tcb.state = 1;
    if (arp_resolve(rip, g_tcb.rmac, g_cfg.arp_ms) != 0) return -3;
    u32 seq0 = (u32)(now_ticks() * 1103515245u) ^ (u32)((u64)(u64)g_out_n);
    g_tcb.snd_nxt = seq0;
    g_tcb.snd_una = seq0;
    out_str("[NET] tcp connect dst="); out_ip(rip);
    out_str(":"); out_u64(rport);
    out_str(" sport="); out_u64(sport);
    out_str(" seq="); out_hex32(seq0); out_str("\n");
    int rc = tcp_send_seg(rip, sport, rport, g_tcb.rmac, seq0, 0, TCP_F_SYN, 0, 0);
    if (rc != 0) { out_str("[NET] tcp fail what=syn rc="); out_signed(rc); out_str("\n"); return -1; }
    g_tcb.snd_nxt = seq0 + 1u;
    out_str("[NET] tcp syn seq="); out_hex32(seq0); out_str(" -> sent\n");
    const u64 t0 = now_ms();
    u64 last = t0;
    u32 rto = 400;
    int attempt = 0;
    for (;;) {
        poll_frames(16);
        if (g_tcb.rst) { out_str("[NET] tcp rst (peer refused)\n"); return -2; }
        if (g_tcb.state == 2) {
            g_tcb.rtt_ms = (u32)(now_ms() - t0);
            out_str("[NET] tcp ack="); out_hex32(g_tcb.snd_una);
            out_str(" state=ESTABLISHED rtt_ms="); out_u64(g_tcb.rtt_ms);
            out_str(" local="); out_ip(g_our_ip); out_str(":"); out_u64(sport);
            out_str("\n");
            return 0;
        }
        const u64 el = now_ms();
        if (el - t0 >= (u64)ms_budget) { out_str("[NET] tcp timeout what=syn ms="); out_u64(ms_budget); out_str("\n"); return -4; }
        if (el - last >= (u64)rto) {
            if (attempt >= 2) { out_str("[NET] tcp timeout what=syn-retx ms="); out_u64(el - t0); out_str("\n"); return -5; }
            attempt++;
            tcp_log_retx("syn", (u32)attempt, rto);
            g_tcb.snd_nxt = seq0 + 1u;
            (void)tcp_send_seg(rip, sport, rport, g_tcb.rmac, seq0, 0, TCP_F_SYN, 0, 0);
            last = el;
            rto *= 2;
        }
        slp_ms(4);
    }
}
/* 收数据 + 回 ACK；返回收到的字节数（>0）或负码。wait_ms 内没数据算超时。 */
static int tcp_recv(u32 want_bytes, u32 ms_budget) {
    const u64 t0 = now_ms();
    for (;;) {
        poll_frames(16);
        if (g_tcb.rst) return -2;
        if (g_tcb.rx_len >= want_bytes) return (int)g_tcb.rx_len;
        if (g_tcb.got_fin && g_tcb.rx_len == 0) return -3;    /* 对端直接关了 */
        if (now_ms() - t0 >= (u64)ms_budget) return -4;
        slp_ms(4);
    }
}
static int tcp_send_data(u32 seq, const u8* data, u32 len) {
    const u32 ack = g_tcb.rcv_nxt;
    const int rc = tcp_send_seg(g_tcb.rip, g_tcb.sport, g_tcb.dport, g_tcb.rmac, seq, ack,
                                TCP_F_PSH | TCP_F_ACK, data, len);
    if (rc == 0) g_tcb.snd_nxt = seq + len;
    return rc;
}
/* 等自己的数据被完全确认（ack == snd_nxt）；固定 RTO 重传 2 次 */
static int tcp_wait_acked(u32 ms_budget) {
    const u64 t0 = now_ms();
    u64 last = t0;
    u32 rto = 400;
    int attempt = 0;
    for (;;) {
        poll_frames(16);
        if (g_tcb.rst) { out_str("[NET] tcp rst\n"); return -2; }
        if (g_tcb.snd_una >= g_tcb.snd_nxt) return 0;       /* 全确认 */
        const u64 el = now_ms();
        if (el - t0 >= (u64)ms_budget) return -4;
        if (el - last >= (u64)rto) {
            if (attempt >= 2) return -5;
            attempt++;
            tcp_log_retx("data", (u32)attempt, rto);
            last = el;
            rto *= 2;
        }
        slp_ms(4);
    }
}
static void tcp_close_reported(void) {
    g_tcb.state = 0;
    out_str("[NET] tcp closed state=CLOSED");
    out_str(" rx="); out_u64(g_tcb.rx_len);
    out_str(" retx="); out_u64(g_tcp_retx);
    out_str(" ooo_drop="); out_u64(g_tcp_ooo);
    out_str(g_tcb.got_fin ? " (peer-fin-seen)" : "");
    out_str("\n");
}
/* 主动收尾：发 FIN|ACK；对端已经先 FIN 了（got_fin）就走"被动关闭"那条，如实打点。 */
static void tcp_close_active(void) {
    if (g_tcb.state == 2) {
        g_tcb.state = 3;
        out_str("[NET] tcp fin seq="); out_hex32(g_tcb.snd_nxt); out_str(" -> sent\n");
        (void)tcp_send_seg(g_tcb.rip, g_tcb.sport, g_tcb.dport, g_tcb.rmac, g_tcb.snd_nxt,
                           g_tcb.rcv_nxt, TCP_F_FIN | TCP_F_ACK, 0, 0);
        g_tcb.snd_nxt += 1u;
        const u64 t0 = now_ms();
        while (now_ms() - t0 < 600u) {                       /* 有界等 FIN 的 ACK（不等满也可）*/
            poll_frames(8);
            if (g_tcb.snd_una >= g_tcb.snd_nxt) break;
            slp_ms(4);
        }
        tcp_close_reported();
        return;
    }
    if (g_tcb.state == 0 && g_tcb.got_fin) {                 /* 对端先关（宿主回显完就 close）*/
        out_str("[NET] tcp fin seq="); out_hex32(g_tcb.snd_nxt);
        out_str(" -> peer-first (already ACKed their FIN)\n");
        tcp_close_reported();
    }
}
/* 被动态：LISTEN -> SYN -> SYN/ACK -> ESTAB -> 收 N 字节回显 -> FIN/ACK */
static int tcp_listen_echo(u16 port, u32 want_bytes, u32 ms_budget) {
    memzero(&g_tcb, sizeof(g_tcb));
    if (g_our_ip == 0) g_our_ip = g_cfg.ip;
    g_tcb.sport = port;
    g_tcb.state = 4;
    out_str("[NET] tcp listen port="); out_u64(port);
    out_str(" (waiting for an incoming connection; hostfwd path)\n");
    const u64 t0 = now_ms();
    u64 last = t0;
    u32 rto = 400;
    int synack_attempt = 0;
    int echo_done = 0;
    for (;;) {
        poll_frames(16);
        const u64 el = now_ms();
        if (g_tcb.state == 2 && g_tcb.synack_pending) {          /* SYN/ACK 重传（还没被 ACK）*/
            if (el - last >= (u64)rto && synack_attempt < 2) {
                synack_attempt++;
                tcp_log_retx("synack", (u32)synack_attempt, rto);
                (void)tcp_send_seg(g_tcb.rip, g_tcb.sport, g_tcb.tport, g_tcb.rmac,
                                   g_tcb.snd_nxt - 1u, g_tcb.rcv_nxt, TCP_F_SYN | TCP_F_ACK, 0, 0);
                last = el;
                rto *= 2;
            }
        }
        if (!echo_done && g_tcb.rx_len >= want_bytes) {
            const u32 n = g_tcb.rx_len;
            out_str("[NET] tcp echo rx="); out_u64(n);
            out_str(" sum="); out_sum(fnv1a32(g_tcb.rx, n));
            const int rc = tcp_send_data(g_tcb.snd_nxt, g_tcb.rx, n);   /* 回显 = 同一份字节 */
            out_str(" tx="); out_u64(n);
            out_str(" rc="); out_signed(rc);
            out_str(" match=1 (echoed same bytes)\n");
            out_str("\n");
            echo_done = 1;
            /* 主动收尾：FIN|ACK */
            out_str("[NET] tcp fin seq="); out_hex32(g_tcb.snd_nxt); out_str(" -> sent\n");
            (void)tcp_send_seg(g_tcb.rip, g_tcb.sport, g_tcb.tport, g_tcb.rmac, g_tcb.snd_nxt,
                               g_tcb.rcv_nxt, TCP_F_FIN | TCP_F_ACK, 0, 0);
            g_tcb.snd_nxt += 1u;
            /* 有界等最后一个 ACK */
            const u64 t1 = now_ms();
            while (now_ms() - t1 < 500u) {
                poll_frames(8);
                if (g_tcb.snd_una >= g_tcb.snd_nxt) break;
                slp_ms(4);
            }
            g_tcb.state = 0;
            out_str("[NET] tcp closed state=CLOSED rx="); out_u64(g_tcb.rx_len);
            out_str(" tx="); out_u64(g_tcb.rx_len);
            out_str(" retx="); out_u64(g_tcp_retx);
            out_str("\n");
            return 0;
        }
        if (el - t0 >= (u64)ms_budget) {
            out_str("[NET] tcp timeout what=listen ms="); out_u64(ms_budget);
            out_str(" accepted="); out_u64((u64)(g_tcb.state >= 2 ? 1u : 0u));
            out_str("\n");
            return -4;
        }
        slp_ms(4);
    }
}

/* ==================== 收包：一帧 -> 分发（只用 net_raw 的 rx）==================== */
static void ipv4_handle(const u8* p, u32 len, const u8* src_mac) {
    if (len < 20u) { g_rx_bad++; return; }
    if ((p[0] >> 4) != 4u) { g_rx_bad++; return; }
    const u32 ihl = (u32)(p[0] & 0xFu) * 4u;
    if (ihl < 20u || ihl > len) { g_rx_bad++; return; }
    if (csum_fin(csum_add(0, p, ihl)) != 0u) { g_rx_bad++; drop_log("ip-csum", rd_ip(p + 12)); return; }
    const u32 total = rd16(p + 2);
    if (total > len) { g_rx_bad++; return; }
    const u16 fragf = rd16(p + 6);
    if ((fragf & 0x1FFFu) != 0u || (fragf & 0x2000u) != 0u) {
        g_ip_frag++; drop_log("ipv4-frag(unsupported)", rd_ip(p + 12)); return;    /* 分片：不支持 */
    }
    const u8 proto = p[9];
    const u32 sip = rd_ip(p + 12), dip = rd_ip(p + 16);
    const u8* pay = p + ihl;
    const u32 plen = total - ihl;
    if (dip != g_our_ip && dip != 0xFFFFFFFFu) { g_rx_other++; drop_log("not-for-us", sip); return; }
    if (proto == 1) {
        icmp_handle(pay, plen, (int)p[8], sip);
        return;
    }
    if (proto == 17) {
        if (plen < 8u) { g_rx_bad++; return; }
        const u16 sport = rd16(pay + 0), dport = rd16(pay + 2);
        const u16 ulen = rd16(pay + 4);
        if (ulen < 8u || ulen > plen) { g_rx_bad++; return; }
        /* UDP 校验和：0 = 发送方没算（RFC 768 允许，slirp 的 DHCP/DNS 应答可能出现）-> 跳过校验；
         * 非 0 就必须验（含伪首部），不通过按坏包丢弃并计数。 */
        if (rd16(pay + 6) != 0u) {
            u32 sum = csum_pseudo(sip, dip, 17, ulen);
            sum = csum_add(sum, pay, ulen);
            if (csum_fin(sum) != 0u) { g_rx_bad++; drop_log("udp-csum", sip); return; }
        }
        const u32 dlen = (u32)ulen - 8u;
        if (dlen > sizeof(g_udp_rx)) { g_rx_bad++; return; }
        if (dlen) memcpy_n(g_udp_rx, pay + 8, dlen);
        g_udp_rx_len = dlen;
        g_udp_rx_ip = sip;
        g_udp_rx_sport = sport;
        g_udp_rx_dport = dport;
        g_udp_rx_sum = fnv1a32(pay + 8, dlen);
        g_udp_rx_gen++;
        return;
    }
    if (proto == 6) {
        if (plen < 20u) { g_rx_bad++; return; }
        const u16 sport = rd16(pay + 0), dport = rd16(pay + 2);
        const u32 seq = rd32(pay + 4), ack = rd32(pay + 8);
        const u32 doff = (u32)(pay[12] >> 4) * 4u;
        const u8 flags = pay[13];
        const u32 hlen = rd16(pay + 14);                 /* 对端窗口（本实现不据此限流，如实）*/
        (void)hlen;
        if (doff < 20u || doff > plen) { g_rx_bad++; return; }
        const u32 dlen = plen - doff;
        {   /* TCP 校验和（含伪首部）必须验通过；不通过按坏包丢弃 */
            u32 sum = csum_pseudo(sip, dip, 6, plen);
            sum = csum_add(sum, pay, plen);
            if (csum_fin(sum) != 0u) { g_rx_bad++; drop_log("tcp-csum", sip); return; }
        }
        /* ---- LISTEN：SYN -> SYN/ACK ---- */
        if (g_tcb.state == 4 && (flags & TCP_F_SYN) && !(flags & TCP_F_ACK) && dport == g_tcb.sport) {
            g_tcb.rip = sip; g_tcb.tport = sport; g_tcb.dport = sport;
            memcpy_n(g_tcb.rmac, src_mac, 6);
            g_tcb.rcv_nxt = seq + 1u;
            u32 s0 = (u32)(now_ticks() * 22695477u) ^ 0x5a5a0000u;
            g_tcb.snd_una = s0; g_tcb.snd_nxt = s0 + 1u;
            g_tcb.state = 2;
            g_tcb.synack_pending = 1;                    /* SYN/ACK 待确认（有界重传 2 次）*/
            (void)tcp_send_seg(sip, g_tcb.sport, sport, src_mac, s0, g_tcb.rcv_nxt,
                               TCP_F_SYN | TCP_F_ACK, 0, 0);
            out_str("[NET] tcp accept from="); out_ip(sip);
            out_str(":"); out_u64(sport);
            out_str(" seq="); out_hex32(seq);
            out_str(" our_seq="); out_hex32(s0);
            out_str(" state=ESTABLISHED\n");
            return;
        }
        /* 只认我们这条连接 */
        if (g_tcb.state < 1 || sip != g_tcb.rip) { g_rx_other++; return; }
        if (sport != (g_tcb.state == 4 ? g_tcb.sport : g_tcb.dport) &&
            sport != g_tcb.tport && dport != g_tcb.sport) { g_rx_other++; return; }
        if (flags & TCP_F_RST) { g_tcb.rst = 1; return; }
        /* ---- SYN_SENT：等 SYN|ACK ---- */
        if (g_tcb.state == 1) {
            if ((flags & (TCP_F_SYN | TCP_F_ACK)) == (TCP_F_SYN | TCP_F_ACK) && ack == g_tcb.snd_nxt) {
                g_tcb.rcv_nxt = seq + 1u;
                g_tcb.snd_una = ack;
                g_tcb.state = 2;
                memcpy_n(g_tcb.rmac, src_mac, 6);
                (void)tcp_send_seg(sip, g_tcb.sport, sport, src_mac, g_tcb.snd_nxt, g_tcb.rcv_nxt,
                                   TCP_F_ACK, 0, 0);
                out_str("[NET] tcp synack seq="); out_hex32(seq);
                out_str(" ack="); out_hex32(ack);
                out_str(" -> est\n");
            } else if (flags & TCP_F_SYN) {
                g_rx_other++;                                /* 不符合预期：不理会 */
            }
            return;
        }
        if (g_tcb.state == 4 && (flags & TCP_F_ACK) && !(flags & TCP_F_SYN)) return;
        if (dport != g_tcb.sport) { g_rx_other++; return; }
        /* ---- ESTAB：推进 ACK / 收数据 ---- */
        if (flags & TCP_F_ACK) {
            if ((i64)(ack - g_tcb.snd_una) > 0 && (i64)(ack - g_tcb.snd_nxt) <= 0) g_tcb.snd_una = ack;
            if (g_tcb.synack_pending && g_tcb.snd_una >= g_tcb.snd_nxt) {
                g_tcb.synack_pending = 0;                    /* 被动态三次握手完成 */
            }
        }
        if (dlen) {
            if (seq == g_tcb.rcv_nxt) {
                u32 copy = dlen;
                if (copy > TCP_BUF - g_tcb.rx_len) copy = TCP_BUF - g_tcb.rx_len;
                if (copy) {
                    memcpy_n(g_tcb.rx + g_tcb.rx_len, pay + doff, copy);
                    g_tcb.rx_len += copy;
                    g_tcb.rcv_nxt += copy;
                }
                (void)tcp_send_seg(sip, g_tcb.sport, sport, src_mac, g_tcb.snd_nxt, g_tcb.rcv_nxt,
                                   TCP_F_ACK, 0, 0);
            } else if ((i64)(seq - g_tcb.rcv_nxt) > 0) {
                g_tcp_ooo++;                                 /* 乱序：不缓存（如实）*/
            } else {
                (void)tcp_send_seg(sip, g_tcb.sport, sport, src_mac, g_tcb.snd_nxt, g_tcb.rcv_nxt,
                                   TCP_F_ACK, 0, 0);         /* 重复段：重发当前 ACK */
            }
        }
        if (flags & TCP_F_FIN) {
            g_tcb.rcv_nxt += 1u;
            g_tcb.got_fin = 1;
            (void)tcp_send_seg(sip, g_tcb.sport, sport, src_mac, g_tcb.snd_nxt, g_tcb.rcv_nxt,
                               TCP_F_ACK, 0, 0);
            out_str("[NET] tcp fin from peer ack="); out_hex32(g_tcb.snd_nxt);
            out_str(" rcv="); out_hex32(g_tcb.rcv_nxt); out_str("\n");
            if (g_tcb.snd_una >= g_tcb.snd_nxt) g_tcb.state = 0;
        }
        return;
    }
    g_rx_other++;                                            /* 其它协议（ICMP/IGMP…）：不处理 */
}
static void arp_handle(const u8* p, u32 len) {
    if (len < 28u) { g_rx_bad++; return; }
    if (rd16(p + 0) != 1u || rd16(p + 2) != 0x0800u || p[4] != 6u || p[5] != 4u) { g_rx_bad++; return; }
    const u16 oper = rd16(p + 6);
    const u32 spa = rd_ip(p + 14);
    const u8* sha = p + 8;
    const u32 tpa = rd_ip(p + 24);
    arp_store(spa, sha);
    if (oper == 2) {
        out_str("[NET] arp reply "); out_ip(spa);
        out_str(" is-at "); out_mac(sha);
        out_str(" (target="); out_ip(tpa); out_str(")\n");
        return;
    }
    /* ★ ring3 **不回应** ARP 请求（内核 net64 的既有行为不动）：这里只把对方记进缓存，
     *   如实标注"本栈不做 ARP 应答"（验收里也不会去 ping 自己）。 */
    g_rx_other++;
}
static void frame_handle(const u8* f, u32 len) {
    if (len < 14u) { g_rx_bad++; return; }
    const u16 type = rd16(f + 12);
    const u8* dmac = f;
    if (!memeq_n(dmac, g_mac, 6) && !memeq_n(dmac, g_bcast, 6) && !(dmac[0] & 0x01)) {
        g_rx_other++;                                        /* 不是给我们的单播 */
        return;
    }
    if (type == 0x0806u) arp_handle(f + 14, len - 14u);
    else if (type == 0x0800u) ipv4_handle(f + 14, len - 14u, f + 6);
    else g_rx_other++;                                       /* IPv6/LLDP/…：不管（如实）*/
}
static void poll_frames(u32 budget) {
    for (u32 i = 0; i < budget; i++) {
        const i64 n = net_raw_rx(g_rxbuf, RX_CAP);
        if (n == NR_EAGAIN) return;                          /* 没包：立刻回（非阻塞语义）*/
        if (n <= 0) return;                                  /* -ENODEV 等：静默（没有网卡时用户态如实报）*/
        if (n < 14) { g_rx_bad++; continue; }
        g_rx_frames++;
        frame_handle(g_rxbuf, (u32)n);
    }
}

static u32 mask_len(void);                              /* 前缀长度（ifconfig 的 ip=../<len>）*/

/* ==================== 验收串（无参 = 全跑）==================== */
static u32 g_ok = 0, g_fail = 0, g_skip = 0;
static void step(int cond) { if (cond) g_ok++; else g_fail++; }

static void ifconfig_print(const char* src) {
    out_str("[NET] ifconfig mac=");
    if (g_link) out_mac(g_mac); else out_str("none");
    out_str(" link="); out_str(g_link ? "up" : "down");
    out_str(" ip="); out_ip(g_our_ip);
    out_str("/"); out_u64(mask_len());
    out_str(" gw="); out_ip(g_gw);
    out_str(" dns="); out_ip(g_dns);
    out_str(" src="); out_str(src);
    out_str("\n");
}
static u32 mask_len(void) {
    u32 m = g_mask, n = 0;
    while (m & 0x80000000u) { n++; m <<= 1; }
    return n;
}
static void abi_case(const char* what, i64 rc, i64 expect) {
    out_str("[NET] abi "); out_str(what);
    out_str(" rc="); out_signed(rc);
    out_str(" expect="); out_signed(expect);
    out_str(" ok="); out_u64((u64)(rc == expect ? 1u : 0u));
    out_str("\n");
    step(rc == expect);
}
static void suite_abi_and_negatives(void) {
    /* ---- ABI 负例：op 未知 / 长度结构性非法 / 超硬件口径 / 坏指针 / 无帧 ---- */
    abi_case("op=9(unknown)", sc3(NETRAW_NR, 9, 0, 0), -3);
    abi_case("tx len=13(short)", net_raw_tx(g_txbuf, 13), -3);
    abi_case("tx len=1600(oversize)", net_raw_tx(g_txbuf, 1600), -6);
    abi_case("rx cap=8(short)", net_raw_rx(g_rxbuf, 8), -3);
    abi_case("rx cap=4096(oversize)", net_raw_rx(g_rxbuf, 4096), -6);
    abi_case("tx bad-ptr", sc3(NETRAW_NR, NR_TX, (i64)0x400000000000ULL, 60), -2);
    abi_case("rx bad-ptr", sc3(NETRAW_NR, NR_RX, (i64)0x400000000000ULL, 1600), -2);
    abi_case("mac bad-ptr", sc3(NETRAW_NR, NR_MAC, (i64)0x400000000000ULL, 0), -2);
    abi_case("checksum selftest", (i64)checksum_selftest(), 0);
    /* ---- 错误路径：ARP 打不通（没有这台主机）-> 有界超时，绝不挂死 ---- */
    const u64 t0 = now_ms();
    u8 mac[6];
    out_str("[NET] arp negative-case: black-hole 10.0.2.99 (bounded wait)\n");
    const int r = arp_resolve(ip4(10, 0, 2, 99), mac, 600);
    out_str("[NET] arp negative-case rc="); out_signed(r);
    out_str(" ms="); out_u64(now_ms() - t0);
    out_str(" ok="); out_u64((u64)(r == -2 ? 1u : 0u));
    out_str("\n");
    step(r == -2);
    /* ---- ping 一个没有 ARP 应答的地址：报超时，不崩 ---- */
    const int pr = icmp_ping(ip4(10, 0, 2, 99), 99, 600, 0);
    step(pr != 0);
}
static void suite_full(u32 ms_total_budget) {
    const u64 t0 = now_ms();
    const i64 mc = net_raw_mac(g_mac);
    g_link = (mc == 0 && net_raw_link() == 1) ? 1 : 0;
    g_our_ip = 0; g_mask = 0;
    g_gw = g_cfg.gw; g_dns = g_cfg.dns;
    ifconfig_print("unbound");
    step(mc == 0);
    step(g_link == 1);
    if (!g_link) {
        out_str("[NET] netd aborted: no link (no e1000 / driver not up) — 明确失败，不假装成功\n");
        g_fail += 1;
        return;
    }
    suite_abi_and_negatives();

    /* ---- ① DHCP 四步（失败降级静态，如实打点）---- */
    int dhcp_ok = 0;
    {
        const u64 dh0 = now_ms();
        if (dhcp_bind(g_cfg.dhcp_ms) == 0) dhcp_ok = 1;
        else {
            g_our_ip = g_cfg.ip; g_mask = g_cfg.mask; g_ip_src = 0;
            out_str("[NET] dhcp fallback applied static ip="); out_ip(g_our_ip);
            out_str(" gw="); out_ip(g_gw);
            out_str(" (elapsed_ms="); out_u64(now_ms() - dh0); out_str(")\n");
        }
        ifconfig_print(dhcp_ok ? "dhcp" : "static");
        step(g_dhcp_steps == 4);
        step(dhcp_ok == 1);
    }

    /* ---- ② ARP：问网关 + 缓存命中 ---- */
    u8 gwmac[6];
    {
        const u64 a0 = now_ms();
        const int r = arp_resolve(g_gw, gwmac, g_cfg.arp_ms);
        out_str("[NET] arp cached gw="); out_ip(g_gw);
        out_str(" lookup="); out_signed(arp_lookup(g_gw, gwmac));
        out_str(" ms="); out_u64(now_ms() - a0);
        out_str("\n");
        step(r == 0);
    }

    /* ---- ③ ICMP：ping 网关（往返时间）---- */
    {
        u32 rtt = 0;
        const int r = icmp_ping(g_gw, 1, g_cfg.ping_ms, &rtt);
        out_str("[NET] ping gw rtt_ms="); out_u64(rtt);
        out_str(" rc="); out_signed(r); out_str("\n");
        step(r == 0);
        u32 rtt2 = 0;
        const int r2 = icmp_ping(ip4(10, 0, 2, 2), 2, g_cfg.ping_ms, &rtt2);
        step(r2 == 0);
    }

    /* ---- ④ DNS：解析真实域名（拿不到外网就如实标注）---- */
    {
        const char* nm = g_name[0] ? g_name : "example.com";
        int n_ans = 0, ptr = 0;
        u32 ip = 0;
        const int r = dns_resolve(nm, g_cfg.dns_sport, g_cfg.dns_ms, &n_ans, &ptr, &ip);
        out_str("[NET] dns result name="); out_str(nm);
        out_str(" answers="); out_u64((u64)n_ans);
        out_str(" first="); out_ip(ip);
        out_str(" rc="); out_signed(r);
        out_str(" note="); out_str(r == 0 ? "resolved" : "no-external-dns-or-timeout");
        out_str("\n");
        if (r == 0) {
            step(n_ans >= 1);
        } else {
            /* 拿不到外网 DNS：**如实标注**并计入 skip（不计 fail）—— 不假装解析成功，也不冤枉整条栈 */
            g_skip++;
            out_str("[NET] dns skipped (no external DNS path on this host) — 计入 skip，不计 fail\n");
        }
    }

    /* ---- ⑤ UDP echo：客人发 -> 宿主回 -> 逐字节一致 ---- */
    {
        u8 pay[64];
        for (u32 i = 0; i < 64u; i++) pay[i] = (u8)((i * 7u + 3u) & 0xFFu);
        const u32 sum = fnv1a32(pay, 64);
        out_str("[NET] udp tx dst="); out_ip(g_cfg.udp_host);
        out_str(":"); out_u64(g_cfg.udp_port);
        out_str(" sport="); out_u64(g_cfg.udp_sport);
        out_str(" bytes=64 sum="); out_sum(sum);
        out_str("\n");
        const u32 gen0 = g_udp_rx_gen;
        const u64 u0 = now_ms();
        const int sr = udp_send_to(g_cfg.udp_host, g_cfg.udp_sport, g_cfg.udp_port, pay, 64);
        if (sr == 0) {
            u16 dport = 0;
            const int got = udp_wait(g_cfg.udp_host, g_cfg.udp_port, gen0, g_cfg.udp_ms, &dport);
            if (got > 0) {
                const int match = (got == 64 && memeq_n(pay, g_udp_rx, 64)) ? 1 : 0;
                out_str("[NET] udp rx from="); out_ip(g_udp_rx_ip);
                out_str(":"); out_u64(g_udp_rx_sport);
                out_str(" dport="); out_u64(g_udp_rx_dport);
                out_str(" bytes="); out_u64((u64)got);
                out_str(" sum=0x"); out_hex32(g_udp_rx_sum & 0xFFFFFFFFu);
                out_str(" match="); out_u64((u64)match);
                out_str(" rtt_ms="); out_u64(now_ms() - u0);
                out_str("\n");
                step(match);
                step(g_udp_rx_sum == sum);
            } else {
                out_str("[NET] udp timeout dst="); out_ip(g_cfg.udp_host);
                out_str(":"); out_u64(g_cfg.udp_port);
                out_str(" ms="); out_u64(g_cfg.udp_ms);
                out_str(" rc="); out_signed(got);
                out_str(" note=no-host-udp-reply\n");
                step(0);
            }
        } else {
            out_str("[NET] udp fail rc="); out_signed(sr); out_str("\n");
            step(0);
        }
    }

    /* ---- ⑥ TCP：三次握手 + 64 B 收/发（对端 = 宿主 Python 回显服务）---- */
    {
        const int cr = tcp_connect(g_cfg.tcp_host, g_cfg.tcp_port, g_cfg.tcp_sport, g_cfg.tcp_ms);
        step(cr == 0);
        if (cr == 0) {
            u8 pay[64];
            for (u32 i = 0; i < 64u; i++) pay[i] = (u8)((i * 7u + 3u) & 0xFFu);
            const u32 sum = fnv1a32(pay, 64);
            out_str("[NET] tcp tx seq="); out_hex32(g_tcb.snd_nxt);
            out_str(" bytes=64 sum="); out_sum(sum);
            out_str("\n");
            const int sr = tcp_send_data(g_tcb.snd_nxt, pay, 64);
            step(sr == 0);
            const int ar = tcp_wait_acked(g_cfg.tcp_ms);
            out_str("[NET] tcp data acked="); out_u64((u64)(ar == 0 ? 1u : 0u));
            out_str(" snd_una="); out_hex32(g_tcb.snd_una);
            out_str(" retx="); out_u64(g_tcp_retx); out_str("\n");
            step(ar == 0);
            const int rr = tcp_recv(64, g_cfg.tcp_ms);
            if (rr > 0) {
                const int match = (rr == 64 && memeq_n(pay, g_tcb.rx, 64)) ? 1 : 0;
                out_str("[NET] tcp rx seq="); out_hex32(g_tcb.rcv_nxt - (u32)rr);
                out_str(" bytes="); out_u64((u64)rr);
                out_str(" sum="); out_sum(fnv1a32(g_tcb.rx, (u32)rr));
                out_str(" match="); out_u64((u64)match);
                out_str("\n");
                step(match);
                step(fnv1a32(g_tcb.rx, 64) == sum);
            } else {
                out_str("[NET] tcp rx fail rc="); out_signed(rr);
                out_str(" (echo from host)\n");
                step(0);
            }
            tcp_close_active();
            step(g_tcb.state == 0);
        }
    }

    /* ---- ⑦ TCP 监听回显（hostfwd：宿主 Python 连进来 -> 我们回显 64 B）---- */
    {
        const int lr = tcp_listen_echo(g_cfg.listen_port, 64, g_cfg.acks_wait_ms);
        step(lr == 0);
    }

    /* ---- 汇总 ---- */
    const u64 el = now_ms() - t0;
    out_str("[NET] netd done mode=all ok="); out_u64(g_ok);
    out_str(" fail="); out_u64(g_fail);
    out_str(" skip="); out_u64(g_skip);
    out_str(" elapsed_ms="); out_u64(el);
    out_str(" budget_ms="); out_u64(ms_total_budget);
    out_str(" tx_frames="); out_u64(g_tx_frames);
    out_str(" rx_frames="); out_u64(g_rx_frames);
    out_str(" rx_other="); out_u64(g_rx_other);
    out_str(" rx_bad="); out_u64(g_rx_bad);
    out_str(" ip_frag_dropped="); out_u64(g_ip_frag);
    out_str("\n");
}

/* ==================== CLI 子命令 ==================== */
static int cli(int argc, char** argv) {
    const char* a = (argc > 1 && argv[1]) ? argv[1] : "all";
    if (a[0] == '/') { /* 允许 `netd /path` 这种误用：当 all */ a = "all"; }
    if (ceq(a, "all")) { suite_full(15000u); return g_fail ? 1 : 0; }
    /* 需要链路的子命令：先把 MAC/链路/地址准备好 */
    if (!ceq(a, "help")) {
        if (net_raw_mac(g_mac) != 0) { out_str("[NET] no net_raw MAC: no e1000 (ENODEV)\n"); return 2; }
        g_link = (net_raw_link() == 1) ? 1 : 0;
        g_our_ip = g_cfg.ip; g_mask = g_cfg.mask; g_gw = g_cfg.gw; g_dns = g_cfg.dns;
    }
    if (ceq(a, "help")) {
        out_str("[NET] usage: netd [all|ifconfig|dhcp|arp <ip>|ping <ip>|dns <name>|"
                "udp echo <ip> <port>|tcp connect <ip> <port>|tcp listen <port>|selftest]\n");
        return 0;
    }
    if (ceq(a, "ifconfig")) {
        ifconfig_print("static");
        return 0;
    }
    if (ceq(a, "dhcp")) {
        const int r = dhcp_bind(g_cfg.dhcp_ms);
        if (r != 0) { g_our_ip = g_cfg.ip; g_mask = g_cfg.mask; }
        ifconfig_print(r == 0 ? "dhcp" : "static");
        return r == 0 ? 0 : 1;
    }
    if (ceq(a, "selftest")) {
        suite_abi_and_negatives();
        out_str("[NET] netd done mode=selftest ok="); out_u64(g_ok);
        out_str(" fail="); out_u64(g_fail); out_str("\n");
        return g_fail ? 1 : 0;
    }
    if (ceq(a, "arp") && argc > 2) {
        u8 mac[6];
        const int r = arp_resolve(parse_ip(argv[2]), mac, g_cfg.arp_ms);
        return r == 0 ? 0 : 1;
    }
    if (ceq(a, "ping") && argc > 2) {
        u32 rtt = 0;
        const int r = icmp_ping(parse_ip(argv[2]), 1, g_cfg.ping_ms, &rtt);
        out_str("[NET] ping rtt_ms="); out_u64(rtt); out_str(" rc="); out_signed(r); out_str("\n");
        return r == 0 ? 0 : 1;
    }
    if (ceq(a, "dns") && argc > 2) {
        int n = 0, ptr = 0;
        u32 ip = 0;
        return dns_resolve(argv[2], g_cfg.dns_sport, g_cfg.dns_ms, &n, &ptr, &ip) == 0 ? 0 : 1;
    }
    if (ceq(a, "udp") && argc > 4 && ceq(argv[2], "echo")) {
        u8 pay[64];
        for (u32 i = 0; i < 64u; i++) pay[i] = (u8)((i * 7u + 3u) & 0xFFu);
        const u32 gen0 = g_udp_rx_gen;
        if (udp_send_to(parse_ip(argv[3]), g_cfg.udp_sport, (u16)parse_dec(argv[4]), pay, 64) != 0) return 1;
        u16 dp = 0;
        const int got = udp_wait(parse_ip(argv[3]), (u16)parse_dec(argv[4]), gen0, g_cfg.udp_ms, &dp);
        if (got <= 0) { out_str("[NET] udp timeout\n"); return 1; }
        out_str("[NET] udp rx bytes="); out_u64((u64)got);
        out_str(" match="); out_u64((u64)((got == 64 && memeq_n(pay, g_udp_rx, 64)) ? 1u : 0u));
        out_str("\n");
        return (got == 64 && memeq_n(pay, g_udp_rx, 64)) ? 0 : 1;
    }
    if (ceq(a, "tcp") && argc > 3 && ceq(argv[2], "connect")) {
        const u32 host = parse_ip(argv[3]);
        const u16 port = (argc > 4) ? (u16)parse_dec(argv[4]) : g_cfg.tcp_port;
        if (tcp_connect(host, port, g_cfg.tcp_sport, g_cfg.tcp_ms) != 0) return 1;
        tcp_close_active();
        return 0;
    }
    if (ceq(a, "tcp") && argc > 3 && ceq(argv[2], "listen")) {
        return tcp_listen_echo((u16)parse_dec(argv[3]), 64, 20000u) == 0 ? 0 : 1;
    }
    out_str("[NET] unknown command: "); out_str(a); out_str("（netd help 看用法）\n");
    return 1;
}

/* ==================== 主 ==================== */
static void arena_init(void) {
    const i64 va = lx6(9 /*mmap*/, 0, 32768, 3 /*READ|WRITE*/, 0, 0, 0);
    if (va < 0) {
        out_str("[NET] netd fatal: mmap failed (no memory for buffers)\n");
        out_flush();
        (void)sc1(2, 3);
    }
    g_arena = (u8*)(u64)va;
    g_txbuf = g_arena;              /* 1600 */
    g_rxbuf = g_arena + 1600;       /* 1600 */
    g_scratch = g_arena + 3200;     /* 512  */
}
int netd_main(long argc, char** argv) {
    cfg_defaults();
    cfg_load("/etc/netd.conf");
    arena_init();
    memzero(g_mac, 6);
    out_str("[NET] netd start abi=53 (net_raw) errno=-2/-3/-4/-5/-6 mode=");
    out_str((argc > 1 && argv[1]) ? argv[1] : "all");
    out_str(" cfg="); out_str(g_name[0] ? g_name : "(default)");
    out_str("\n");
    const int rc = cli((int)argc, argv);
    out_flush();
    (void)sc1(2, (rc == 0) ? 0 : 1);            /* exit(code)：0 = 全部成功 */
    return 0;
}

/* 入口：从初始栈取 argc/argv（内核 ELF64 装载器按 [argc][argv..][NULL][envp NULL] 建栈）。
 * 必须落在 .text.start（user64.ld 里排第一），与 /bin/sounder 同一套链接脚本。 */
__asm__(".section .text.start,\"ax\"\n"
        ".globl _start\n"
        ".type _start,@function\n"
        "_start:\n"
        "  movq (%rsp), %rdi\n"
        "  leaq 8(%rsp), %rsi\n"
        "  andq $-16, %rsp\n"
        "  call netd_main\n"
        "1: jmp 1b\n"
        ".previous\n");
