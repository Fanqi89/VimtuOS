/* user/svc/drvdemo.c - ★ 本批：Ring 3 驱动服务骨架（pci_map_bar 的真实用户）
 *
 * 交付方式：**系统卷里的文件 /bin/drvdemo**（内核里不含它的字节；见 build64.sh 的那两行断言）。
 * 它证明的事情（与任务书 ③ 一一对应）：
 *   * 用户态可以直接摸设备寄存器：pci_map_bar(48) 拿到 HDA BAR0 / xHCI BAR0 之后，
 *     用**普通的内存读写**读能力/版本/状态寄存器，值与内核驱动自己的打点逐一对照；
 *   * 一次**真实操作**：HDA 的 SD0 流复位握手（写 SD0CTL.SRST=1 -> 回读 bit0=1 -> 清 0 ->
 *     回读 RUN=0 + 读 SD0STS.FIFORDY）—— 这是"能写设备"的证据，不是只读；
 *   * xHCI 侧再把每个端口的 PORTSC 读一遍（CCS/PED/PS），与内核 `[XHCI] port N connected`
 *     的枚举结果对照 —— 两边看到的是**同一台设备**；
 *   * 负例：非 root（-EPERM）、不存在的 BDF（-ENODEV）、I/O 端口 BAR（-EINVAL，不是 MMIO）、
 *     BAR 序号越界（-EINVAL）、坏 out 指针（-EFAULT）；以及"越权不许成功"的两条：
 *     mmap 覆盖设备窗 -> -ENOMEM、munmap 设备窗 -> -EINVAL（内核拒绝，进程不崩）。
 *
 * 为什么自己实现一套最小 runtime（不 include 任何头、不链 user/lib）：
 *   1) 另一条线在改 user/lib 与 user/shell，本文件**不依赖**它们就不会被它们的中间态带崩；
 *   2) 交付形态就是"卷里的一份静态 ELF"，越自足越好（与 A4-2a 的探针同一条纪律）。
 *
 * ABI（与 kernel/syscall64.h 的"用户态设备映射"一节逐字对应）：
 *   int 0x80 自有 ABI：rax=48 pci_map_bar(rdi=bdf, rsi=bar_index, rdx=out_va, r10=out_len)
 *   bdf = (bus << 8) | (dev << 3) | fn
 *   返回 0 = 成功（*out_va / *out_len 已写）；-1 EPERM / -2 EFAULT / -3 EINVAL / -4 ENOMEM / -5 ENODEV
 *   另外用 int 0x80 的 1 write(fd=1) / 3 getpid / 2 exit，用 syscall 指令的 9 mmap / 11 munmap /
 *   107 geteuid / 102 getuid（两侧都验：自有 ABI 与 Linux ABI 在同一进程里可以混用）。
 *
 * 打点（自动验收 tests/drvsvc64_test.py grep，格式勿改）：
 *   DRVDEMO init euid=<n> uid=<n> pid=<n>
 *   DRVDEMO neg noroot euid=<n> rc=<n>
 *   DRVDEMO scan tried=<n> mapped=<n> hda=0x<hex> xhci=0x<hex>
 *   DRVDEMO hda found bdf=0x<hex> va=0x<hex> len=<n> bar=0
 *   DRVDEMO hda regs gcap=0x<hex> vmin=<n> vmaj=<n> statests=0x<hex> gctl=0x<hex>
 *   DRVDEMO hda sd0 ctl=0x<hex> sts=0x<hex> lpib=<n> cbl=<n> fmt=0x<hex>
 *   DRVDEMO hda op srst wr=0x1 rb=0x<hex> ok=<0|1>
 *   DRVDEMO hda op clear wr=0x0 rb=0x<hex> ok=<0|1> sts_rdy=<0|1>
 *   DRVDEMO hda idem va0=0x<hex> va1=0x<hex> len0=<n> len1=<n> same=<0|1>
 *   DRVDEMO xhci found bdf=0x<hex> va=0x<hex> len=<n> bar=0
 *   DRVDEMO xhci caps caplen=0x<hex> ver=0x<hex> hcs1=0x<hex> hcc1=0x<hex> max_slots=<n> max_ports=<n> csz=<n> ac64=<n>
 *   DRVDEMO xhci port n=<n> portsc=0x<hex> ccs=<n> ped=<n> ps=<n>
 *   DRVDEMO neg badbdf bdf=0x<hex> rc=<n>
 *   DRVDEMO neg iobar bdf=0x<hex> bar=0 rc=<n>
 *   DRVDEMO neg badbar bdf=0x<hex> bar=6 rc=<n>
 *   DRVDEMO neg outptr rc=<n>
 *   DRVDEMO neg mmap_dev va=0x<hex> rc=<n>
 *   DRVDEMO neg munmap_dev va=0x<hex> rc=<n>
 *   DRVDEMO done maps=<n> hdaops=<n> rc=0
 *
 * ★ 如实边界（本骨架**不是**一个可用的用户态驱动，别把它当成品）：
 *   * 不申请 MEM/BUS-MASTER 功能位、不做 DMA（没有 DMA 映射 API —— 见报告"还缺什么"逐条）；
 *   * 不接管中断（内核驱动都在轮询，且中断路由没有"投递给用户进程"的概念）；
 *   * 与内核驱动**同时**碰同一设备时没有仲裁协议（谁先初始化、谁负责中断、寄存器并发风险见
 *     docs/应用层与系统调用说明.md 末节）。
 */
typedef signed long    i64;
typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

/* ==================== 系统调用包装 ==================== */
/* 自有 ABI（int 0x80）：第 4 个参数走 r10（与内核 syscall64.cpp 的 fb_flip/pci_map_bar 同款）。
 * 用显式 clobber("r10") + 手工 mov 写 r10，而不是让编译器自己挑寄存器 —— 后者不可靠。 */
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

/* Linux ABI（syscall 指令）：只用来读身份与验 mmap/munmap 的越权拒绝。 */
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
static i64 pci_map_bar(u64 bdf, u64 bar, u64* out_va, u64* out_len) {
    return sc4(48, (i64)bdf, (i64)bar, (i64)(u64)out_va, (i64)(u64)out_len);
}

/* ==================== 最小输出（自有 ABI write(1) -> 内核控制台）==================== */
static void out_str(const char* s) {
    i64 n = 0;
    while (s[n]) n++;
    if (n) (void)sc3(1, 1, (i64)(u64)s, n);
}
static void out_u64(u64 v) {
    char b[24];
    int n = 0;
    char t[24];
    int k = 0;
    if (v == 0) { b[n++] = '0'; }
    while (v) { t[k++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    while (k) b[n++] = t[--k];
    b[n] = 0;
    out_str(b);
}
static void out_hex(u64 v) {          /* 0x + 小写十六进制（与内核打点同款，便于逐字符对照） */
    static const char* H = "0123456789abcdef";
    char b[19];
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 16; i++) b[2 + i] = H[(v >> ((15 - i) * 4)) & 0xF];
    b[18] = 0;
    out_str(b);
}
static void kv(const char* k, u64 v) { out_str(k); out_u64(v); }
static void kx(const char* k, u64 v) { out_str(k); out_hex(v); }
static void nl(void) { out_str("\n"); }

/* ==================== MMIO 读写（映射进来的就是普通内存）==================== */
#define R8(v, o)     (*(volatile u8  *)((v) + (u64)(o)))
#define R16(v, o)    (*(volatile u16 *)((v) + (u64)(o)))
#define R32(v, o)    (*(volatile u32 *)((v) + (u64)(o)))
#define W32(v, o, x) (*(volatile u32 *)((v) + (u64)(o)) = (u32)(x))

/* ---- HDA 寄存器（与 kernel/hda64.cpp 的枚举逐条对应；只列本程序用到的）---- */
#define HDA_GCAP     0x00u
#define HDA_VMIN     0x02u
#define HDA_VMAJ     0x03u
#define HDA_GCTL     0x08u
#define HDA_STATESTS 0x0Eu
#define HDA_SD_BASE  0x100u      /* ★ 输出流描述符 0（0x80 起的那组是采集流，见 hda64.cpp 的说明）*/
#define SD_CTL       0x00u       /* 32 位：bit0 SRST / bit1 RUN */
#define SD_STS       0x03u       /* 8 位：bit2 BCIS / bit5 FIFORDY */
#define SD_LPIB      0x04u
#define SD_CBL       0x08u
#define SD_FMT       0x12u

/* ---- xHCI 寄存器 ---- */
#define XHCI_CAPLEN     0x00u
#define XHCI_HCIVERS    0x02u
#define XHCI_HCSPARAMS1 0x04u
#define XHCI_HCCPARAMS1 0x10u
#define XHCI_PORTSC(n)  (0x400u + 0x10u * ((u64)(n) - 1u))   /* 端口号从 1 起 */

/* ==================== 候选 BDF ==================== */
/* 为什么是这张表而不是全总线扫描：一次成功的 pci_map_bar 会占掉一个映射槽（8 个），
 * 全扫会把 VGA（16 MiB BAR）之类的设备也拉进来；这几个是 QEMU 上 HDA/xHCI 的常见落点
 * （测试用的是一台固定拓扑的 QEMU：`-device ich9-intel-hda,addr=0x1b.0` +
 * `-device qemu-xhci,addr=0x14.0` —— 也就是 0xD8 / 0xA0 两个）。 */
static const u32 g_cand[] = {
    0x00D8u, /* 00:1b.0 = bus0 dev27 fn0：HDA（q35 与 i440fx 上都在这）*/
    0x00A0u, /* 00:14.0 = bus0 dev20 fn0：xHCI（-device qemu-xhci,addr=0x14.0）*/
    0x0018u, 0x0020u, 0x0028u, 0x0030u, 0x0038u, 0x0040u, 0x0048u,
    0x0060u, 0x00B0u, 0x00B8u, 0x00C0u, 0x00C8u, 0x00D0u, 0x00E0u, 0x00E8u, 0x00F0u,
};

/* ---- 设备识别与寄存器读：**按设备规定的宽度读** ----
 * 本批实测的两条硬经验（都踩过，写在这里免得下次再踩）：
 *   ① 刚建立的设备映射，**第一次**读可能返回旧值（xHCI 的 CAPLENGTH/HCSPARAMS1 实测到过），
 *      所以先做一轮丢弃读（warm-up）再取数；
 *   ② 每个寄存器只能用**它自己的宽度**读：QEMU 的 xHCI 在偏移 0 上是 4 字节寄存器
 *      独立的字节寄存器，用 4 字节读偏移 0 拿到的高两个字节是 0（QEMU 不替你把相邻字节拼起来）。
 *      内核驱动读的就是各自正确的宽度（见 hda64.cpp/xhci64.cpp），这里照做，读出来才能对照。 */
typedef struct BarRegs {
    /* xHCI：偏移 0 的 dword = CAPLENGTH(低 8) + HCIVERSION(高 16) */
    u32 cap0, hcs1, hcc1;
    /* HDA：GCAP = 偏移 0 的 16 位；VMIN/VMAJ = 偏移 2/3 两个字节；GCTL = 偏移 8 的 dword */
    u32 gcap, vmin, vmaj, gctl, statests;
    u32 sdcap;                        /* 偏移 0 的 dword（打点用，宽读） */
} BarRegs;
static void read_bar(u64 va, BarRegs* r) {
    (void)R32(va, 0x00);                                  /* warm-up（丢弃，见上 ①） */
    (void)R32(va, 0x04);
    r->cap0  = R32(va, 0x00);
    r->hcs1  = R32(va, 0x04);
    r->hcc1  = R32(va, 0x10);
    r->gcap  = R16(va, 0x00);
    r->vmin  = R8(va, 0x02);
    r->vmaj  = R8(va, 0x03);
    r->gctl  = R32(va, 0x08);
    r->statests = R16(va, 0x0E);
    r->sdcap = r->cap0;
}
static int looks_like_hda(const BarRegs* r, u64 len) {
    if (len < 0x100u) return 0;
    if (r->vmaj != 1u) return 0;                           /* HDA 规范主版本 = 1 */
    if ((r->gcap & 0x0003u) == 0u) return 0;               /* 至少一个输出流或 64 位地址支持 */
    if ((r->gcap & 0x0F00u) == 0u) return 0;               /* 至少支持一种 44.1/48 kHz */
    return 1;
}
static int looks_like_xhci(const BarRegs* r, u64 len) {
    if (len < 0x40u) return 0;
    const u32 caplen = r->cap0 & 0xFFu;
    const u32 ver = (r->cap0 >> 16) & 0xFFFFu;
    if (caplen < 0x20u || caplen > 0x100u) return 0;
    if ((r->hcs1 & 0xFFu) == 0u) return 0;                 /* MaxSlots 不能是 0 */
    if (((r->hcs1 >> 24) & 0xFFu) == 0u) return 0;         /* MaxPorts 不能是 0 */
    if (ver < 0x0090u) return 0;                           /* xHCI 1.0 起 */
    return 1;
}
/* 返回 1 = 打印了一行（调用方计数） */
static void report_probe(u32 bdf, i64 rc) {
    out_str("DRVDEMO probe bdf=");
    out_hex(bdf);
    kv(" bar=0 rc=", (u64)(rc < 0 ? -rc : rc));
    out_str(rc == 0 ? " ok\n" : " fail\n");
}

/* ==================== 主体 ==================== */
__attribute__((section(".text.start"), noreturn)) void _start(void) {
    const i64 euid = lx6(107, 0, 0, 0, 0, 0, 0);           /* Linux ABI geteuid(107) */
    const i64 uid = lx6(102, 0, 0, 0, 0, 0, 0);            /* Linux ABI getuid(102)  */
    const i64 pid = sc0(3);                                /* 自有 ABI getpid(3) -> 进程 pid */
    out_str("DRVDEMO init");
    kv(" euid=", (u64)(euid < 0 ? 0xFFFFu : (u64)euid));
    kv(" uid=", (u64)(uid < 0 ? 0xFFFFu : (u64)uid));
    kv(" pid=", (u64)(pid < 0 ? 0 : (u64)pid));
    nl();

    /* ---- ① 权限：非 root 必须被明确拒绝（-EPERM = -1），而且不崩 ---- */
    if (euid != 0) {
        u64 va = 0, len = 0;
        const i64 rc = pci_map_bar(0x00D8u, 0, &va, &len);
        out_str("DRVDEMO neg noroot");
        kv(" euid=", (u64)euid);
        kv(" rc=", (u64)(rc < 0 ? -rc : rc));
        nl();
        out_str("DRVDEMO done maps=0 hdaops=0 rc=1 (not-root: pci_map_bar denied, nothing mapped)\n");
        (void)sc1(2, 1);
        for (;;) {}
    }

    /* ---- ② 扫描：找 HDA / xHCI 并映射它们的 BAR0 ---- */
    u64 hda_va = 0, hda_len = 0, xhci_va = 0, xhci_len = 0;
    u32 hda_bdf = 0, xhci_bdf = 0;
    u32 tried = 0, mapped = 0;
    for (u64 i = 0; i < (u64)(sizeof(g_cand) / sizeof(g_cand[0])); i++) {
        const u32 bdf = g_cand[i];
        u64 va = 0, len = 0;
        tried++;
        const i64 rc = pci_map_bar(bdf, 0, &va, &len);
        if (rc != 0) { report_probe(bdf, rc); continue; }
        mapped++;
        BarRegs g;
        read_bar(va, &g);                                  /* 一批自然宽度的读数：识别 + 打点同一批 */
        if (!hda_va && looks_like_hda(&g, len)) { hda_va = va; hda_len = len; hda_bdf = bdf; }
        else if (!xhci_va && looks_like_xhci(&g, len)) { xhci_va = va; xhci_len = len; xhci_bdf = bdf; }
        else {
            out_str("DRVDEMO probe bdf=");
            out_hex(bdf);
            kv(" bar=0 rc=0 ok unclassified len=", len);
            out_str(" cap0=");
            out_hex(g.cap0);
            out_str(" gcap=");
            out_hex(g.gcap);
            out_str(" hcs1=");
            out_hex(g.hcs1);
            nl();
        }
        if (hda_va && xhci_va) break;                      /* 两个都找到了：不再占映射槽 */
    }
    out_str("DRVDEMO scan");
    kv(" tried=", (u64)tried);
    kv(" mapped=", (u64)mapped);
    kx(" hda=", (u64)hda_bdf);
    kx(" xhci=", (u64)xhci_bdf);
    nl();

    u32 hdaops = 0;

    /* ---- ③ HDA：读寄存器 + 一次真实的流复位握手 ---- */
    if (hda_va) {
        out_str("DRVDEMO hda found");
        kx(" bdf=", (u64)hda_bdf);
        kx(" va=", hda_va);
        kv(" len=", hda_len);
        out_str(" bar=0\n");

        const u32 gcap = R16(hda_va, HDA_GCAP);
        const u32 vmin = R8(hda_va, HDA_VMIN);
        const u32 vmaj = R8(hda_va, HDA_VMAJ);
        const u32 statests = R16(hda_va, HDA_STATESTS);
        const u32 gctl = R32(hda_va, HDA_GCTL);
        out_str("DRVDEMO hda regs");
        kx(" gcap=", gcap);
        kv(" vmin=", vmin);
        kv(" vmaj=", vmaj);
        kx(" statests=", statests);
        kx(" gctl=", gctl);
        nl();
        /* 解释（写进日志是为了让"值可解释"这件事可复核，不是猜）：
         *   gcap 的 bit0..3 = 输出流数/64 位地址能力、bit8..11 = 输入流数、bit12..15 = 采样率支持
         *   vmaj/vmin = 1/0（HDA 规范 1.0）；statests 的每一位 = 一个码器上报过状态变化。
         *   内核 hda64 在启动期已经**写过 1 清** STATESTS -> 这里读回 0 是"没有新的未确认变化"，
         *   与内核打点的 statests=0x1（它读到并 ack 掉的那一次）并不矛盾。 */

        const u32 ctl0 = R32(hda_va, HDA_SD_BASE + SD_CTL);
        const u32 sts0 = R8(hda_va, HDA_SD_BASE + SD_STS);
        const u32 lpib0 = R32(hda_va, HDA_SD_BASE + SD_LPIB);
        const u32 cbl0 = R32(hda_va, HDA_SD_BASE + SD_CBL);
        const u32 fmt0 = R16(hda_va, HDA_SD_BASE + SD_FMT);
        out_str("DRVDEMO hda sd0");
        kx(" ctl=", ctl0);
        kx(" sts=", sts0);
        kv(" lpib=", lpib0);
        kv(" cbl=", cbl0);
        kx(" fmt=", fmt0);
        nl();
        /* ★ 真实操作：SD0 流复位握手（写 -> 回读 -> 清 -> 回读）。内核驱动每次播放前后做的
         *   就是同一套动作（见 kernel/hda64.cpp 的 hda_stream_play），所以这一步是"可解释、
         *   可恢复"的：我们把流停掉/复位，内核下次播放会自己重新配置。 */
        W32(hda_va, HDA_SD_BASE + SD_CTL, 0u);                     /* 先保证 RUN=0 */
        W32(hda_va, HDA_SD_BASE + SD_CTL, 1u);                     /* SRST = bit0 = 1 */
        for (volatile int w = 0; w < 200000; w++) {}                /* 有界等一拍（QEMU 立刻生效）*/
        const u32 rb_srst = R32(hda_va, HDA_SD_BASE + SD_CTL);
        const int srst_ok = (rb_srst & 1u) ? 1 : 0;
        out_str("DRVDEMO hda op srst");
        kx(" wr=", 1u);
        kx(" rb=", rb_srst);
        kv(" ok=", (u64)srst_ok);
        nl();
        hdaops++;

        W32(hda_va, HDA_SD_BASE + SD_CTL, 0u);                     /* 清 SRST（流保持停止）*/
        const u32 rb_clr = R32(hda_va, HDA_SD_BASE + SD_CTL);
        const u32 sts_clr = R8(hda_va, HDA_SD_BASE + SD_STS);
        const int run_ok = ((rb_clr & 2u) == 0u) ? 1 : 0;           /* RUN 必须回 0 */
        const int rdy_ok = (sts_clr & 0x20u) ? 1 : 0;               /* FIFORDY = bit5 */
        out_str("DRVDEMO hda op clear");
        kx(" wr=", 0u);
        kx(" rb=", rb_clr);
        kv(" ok=", (u64)run_ok);
        kv(" sts_rdy=", (u64)rdy_ok);
        nl();
        hdaops++;

        /* 幂等：同一个 (bdf, bar) 再调一次 -> 必须是同一个 VA/长度（内核打点 re=1）*/
        u64 va2 = 0, len2 = 0;
        const i64 rc2 = pci_map_bar((u64)hda_bdf, 0, &va2, &len2);
        out_str("DRVDEMO hda idem");
        kx(" va0=", hda_va);
        kx(" va1=", va2);
        kv(" len0=", hda_len);
        kv(" len1=", len2);
        kv(" same=", (u64)((rc2 == 0 && va2 == hda_va && len2 == hda_len) ? 1 : 0));
        nl();
    } else {
        out_str("DRVDEMO hda not found (no candidate BDF answered as HDA)\n");
    }

    /* ---- ④ xHCI：读能力寄存器 + 每个端口的 PORTSC ---- */
    if (xhci_va) {
        out_str("DRVDEMO xhci found");
        kx(" bdf=", (u64)xhci_bdf);
        kx(" va=", xhci_va);
        kv(" len=", xhci_len);
        out_str(" bar=0\n");
        /* ★ CAPLENGTH/HCIVERSION 是**一个 4 字节寄存器**（偏移 0）：QEMU 下按 2 字节读偏移 2 会拿到 0，
         *   所以照 xhci64.cpp 的做法读 dword 再拆。PORTSC 每个端口一个 dword（0x400 + 0x10*(n-1)）。 */
        const u32 cap0 = R32(xhci_va, XHCI_CAPLEN);
        const u32 caplen = cap0 & 0xFFu;
        const u32 ver = (cap0 >> 16) & 0xFFFFu;
        const u32 hcs1 = R32(xhci_va, XHCI_HCSPARAMS1);
        const u32 hcc1 = R32(xhci_va, XHCI_HCCPARAMS1);
        const u32 max_slots = hcs1 & 0xFFu;
        const u32 max_ports = (hcs1 >> 24) & 0xFFu;
        out_str("DRVDEMO xhci caps");
        kx(" caplen=", caplen);
        kx(" ver=", ver);
        kx(" hcs1=", hcs1);
        kx(" hcc1=", hcc1);
        kv(" max_slots=", max_slots);
        kv(" max_ports=", max_ports);
        kv(" csz=", (hcc1 >> 2) & 1u);
        kv(" ac64=", hcc1 & 1u);
        nl();
        for (u32 n = 1; n <= max_ports && n <= 8u; n++) {
            const u32 portsc = R32(xhci_va, XHCI_PORTSC(n));
            out_str("DRVDEMO xhci port");
            kv(" n=", n);
            kx(" portsc=", portsc);
            kv(" ccs=", portsc & 1u);
            kv(" ped=", (portsc >> 1) & 1u);
            kv(" ps=", (portsc >> 10) & 0xFu);
            nl();
        }
    } else {
        out_str("DRVDEMO xhci not found (no candidate BDF answered as xHCI)\n");
    }

    /* ---- ⑤ 负例：明确错误码 + 不崩 ---- */
    {
        u64 va = 0, len = 0;
        /* 不存在的 BDF：bus0 dev31 fn7（QEMU 上没有）-> -ENODEV(-5) */
        i64 rc = pci_map_bar(0x00FFu, 0, &va, &len);
        out_str("DRVDEMO neg badbdf");
        kx(" bdf=", 0x00FFu);
        kv(" rc=", (u64)(rc < 0 ? -rc : rc));
        nl();
        /* 00:01.1 = PIIX3 IDE：它**没有内存 BAR**（命令块是固定 I/O 口 0x1F0…），BMDMA 是 I/O BAR。
         * 这里把 0..5 号 BAR 都试一遍，打出**第一个不是 ENODEV 的结果** —— 命中 I/O BAR 时内核返回
         * -EINVAL(-3)（"不是 MMIO，不给映射"），而不是假装能映射。 */
        for (u64 b = 0; b < 6u; b++) {
            const i64 r2 = pci_map_bar(0x0009u, b, &va, &len);
            out_str("DRVDEMO neg iobar");
            kx(" bdf=", 0x0009u);
            kv(" bar=", b);
            kv(" rc=", (u64)(r2 < 0 ? -r2 : r2));
            nl();
            if (r2 != 0 && r2 != -5) break;                /* 不是"没有这个 BAR"就停下（-3 = I/O 端口）*/
        }
        /* BAR 序号越界（只有 0..5）-> -EINVAL(-3) */
        rc = pci_map_bar(hda_va ? (u64)hda_bdf : 0x00D8u, 6, &va, &len);
        out_str("DRVDEMO neg badbar");
        kx(" bdf=", (u64)(hda_va ? hda_bdf : 0x00D8u));
        out_str(" bar=6");
        kv(" rc=", (u64)(rc < 0 ? -rc : rc));
        nl();
        /* 坏 out 指针（0 号地址不是用户页）-> -EFAULT(-2) */
        rc = pci_map_bar(hda_va ? (u64)hda_bdf : 0x00D8u, 0, (u64*)0, &len);
        out_str("DRVDEMO neg outptr");
        kv(" rc=", (u64)(rc < 0 ? -rc : rc));
        nl();
    }

    /* ---- ⑥ 越权不许成功（映射窗不能被普通 mmap/munmap 碰）---- */
    /* 设备映射窗 = 用户窗口顶部 shm 窗（256 KiB）下方 2 MiB（见 kernel/proc64.h）。
     * 这里用**编译期常量**算出来（与内核常量逐位一致；测试脚本也按同一公式核对）。 */
    const u64 dev_win = 0x0000000100000000UL + 16UL * 1024UL * 1024UL - 256UL * 1024UL - 2UL * 1024UL * 1024UL;
    {
        /* mmap(MAP_FIXED) 想覆盖设备窗 -> 内核必须拒绝 -ENOMEM(-12) */
        const i64 rc = lx6(9, (i64)dev_win, 4096, 3 /*RW*/, 0x10 | 0x02 | 0x20 /*FIXED|PRIVATE|ANON*/, -1, 0);
        out_str("DRVDEMO neg mmap_dev");
        kx(" va=", dev_win);
        kv(" rc=", (u64)(rc < 0 ? -rc : rc));
        nl();
        /* munmap 设备窗 -> 内核必须拒绝 -EINVAL(-22)（拒绝的理由：那里是 MMIO，不是页池的页）*/
        const i64 rc2 = lx6(11, (i64)(hda_va ? hda_va : dev_win), 4096, 0, 0, 0, 0);
        out_str("DRVDEMO neg munmap_dev");
        kx(" va=", hda_va ? hda_va : dev_win);
        kv(" rc=", (u64)(rc2 < 0 ? -rc2 : rc2));
        nl();
    }

    out_str("DRVDEMO done");
    kv(" maps=", (u64)mapped);
    kv(" hdaops=", (u64)hdaops);
    out_str(" rc=0\n");
    (void)sc1(2, 0);                                       /* 自有 ABI exit(2) */
    for (;;) {}
}
