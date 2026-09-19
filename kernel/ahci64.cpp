// ahci64.cpp - VimtuOS 最小 AHCI(SATA) 驱动（真机可用性套件 item 5a）
//
// 设计要点（与 kernel/e1000_64.cpp 同一套做法：PCI 找设备 -> BAR -> MMIO -> 表/环 -> 轮询）：
//   1) PCI：扫 class 0x01 / subclass 0x06 / prog-if 0x01（SATA AHCI 1.0），读 **BAR5 = ABAR**
//      （PCI 偏移 0x24，是 64 位 MMIO BAR —— 高 32 位在偏移 0x28，type 位 bit2 置位时有效）。
//      只在自己找到的那个功能上开 MEM(bit1)+BUS MASTER(bit2)，不碰别的设备。
//   2) 控制器初始化：BIOS/OS Handoff（BOHC）-> GHC.HR 复位 -> GHC.AE=1 -> 清 GHC.IS
//      -> 读 CAP/PI/VS（端口数 = CAP.NP + 1）。复位/握手都有**有界等待**，失败只打点继续
//      （真机固件千奇百怪，绝不能因此不启动）。
//   3) 每个 PI 置位的端口：停端口 -> （只有不就绪时才）重新检测 -> **先启动端口再读 PxSSTS/PxSIG**
//      -> 按签名提示 + IDENTIFY 判决拿型号/容量（0xEC；ATAPI 用 0xA1，失败会互相换一次重试）。
//      命令列表 1KB@1KB 对齐 / FIS 256B@256B 对齐 / 命令表 128B@128B 对齐，全部来自页池。
//   4) 传输：非队列 DMA，命令槽固定 slot 0，一条命令 1..16 个 PRDT 项，一次 ≤128 扇区（64KB）。
//      读写用 READ/WRITE DMA EXT（0x25/0x35，**LBA48**）。写 PxCI 后轮询 PxCI 清零，
//      再查 PxIS 错误位与 PxTFD.ERR/DF；超时 = g_ticks64 计时 + 自旋计数上限（双保险）。
//   5) 物理地址：命令列表/FIS/命令表/DMA 暂存全部来自 page_alloc_64()（低内存，恒等映射 ->
//      物理地址 == 虚拟地址）。上层传进来的缓冲区可能是内核镜像里的高半区对象（.bss 的
//      static 数组），那种情况按直映关系换算 PA = VA - (ML64_KERNEL_VA_BASE - ML64_KERNEL_BASE)；
//      认不出来的指针走 DMA 暂存兜底（暂存池 16 页，每页一项 PRDT）。
//
// ★★★ 实测踩坑记录（这一节的每一条都是真金白银，改代码前先读）：
//   A) **PxSIG 在 QEMU 上读出 0xFFFFFFFF**（哪怕 DET==3、盘确实在）：签名只能当提示，
//      识别必须以 IDENTIFY 为准（0xEC 失败就换 0xA1，反之亦然）—— 见 ahci_identify。
//   B) **PxSIG 只有在端口启动之后才是真值**（QEMU 在 PxCMD.FRE/ST 之前给 0xFFFFFFFF）：
//      所以顺序必须是"先 port_start，再读 DET/SIG，再识别"。
//   C) **PxIS/PxSERR 不要写 0xFFFFFFFF**：实测把这两位全置 1 之后，HBA 会把随后写的 PxCI
//      "收下但不执行"（PxCI 一直挂着、PxIS/PxTFD 一动不动）。只清 DHRS 与 DIAG.X 就够。
//   D) **不要写 PxCMD.ICC（bits 31:28）**：规范说 ICC=1 = Interface Active 看着"应该写"，
//      但实测写进去之后命令同样不被执行。SeaBIOS / Linux libata 都是**读-改-写**、只 OR 进
//      FRE/ST（ICC 保持 0），照抄它们最稳（ICC 原本是给 DevSlp/低功耗状态机用的）。
//   E) **轮询里要 hlt 让出**：模拟器/部分真机的"命令完成"挂在虚拟时钟定时器上（QEMU 的 IDE
//      盘就是 timer_mod 模拟延迟）。guest 一直 pause 死转，vCPU 不回主循环、定时器不触发，
//      PxCI 永远不清零。SeaBIOS 等命令时调 yield()，而 yield() 就是一条 hlt。这里做成
//      "先自旋 N 次、之后 hlt"的混合（IF=0 时退回 pause，绝不挂死）。
//   F) 端口本来 DET==3 时**不要**再写 PxSCTL=0 去"重新检测"（会惊醒一个已经就绪的端口）。
//   G) 命令表必须 128 字节对齐，PRDT 从表内偏移 0x80 起；PRDT 的 DBC 字段是"字节数-1"。
//   H) CFIS 是 20 字节的 H2D Register FIS：命令头 CFL 写 5（dwords），flags.C=1 才更新命令寄存器。
//   I) 每块盘一份命令列表/FIS/命令表（不共用）：共用时一旦两个端口并发就会互相踩。
//   J) ★★ **命令头布局写错 = 命令永远不执行**（item 5b 定位到的硬缺口根因，寄存器证据齐全）：
//       AHCI 命令头 32B 的正确布局 ——
//         DW0 bits[4:0]=CFL(5) bit5=A bit6=W bit7=P bit8=R bit9=B bits[12:11]=PMP
//             **bits[31:16] = PRDTL（PRDT 项数）**；
//         DW1 = PRDBC（HBA 完成后回填，软件必须写 0）；
//         DW2/DW3 = CTBA/CTBAU（命令表物理地址，128B 对齐）；DW4..DW7 保留。
//       旧代码把 PRDTL 写到 DW1、把 CTBA 写到 DW3/DW4（DW2 留 0），于是 HBA 读出的命令表
//       地址 = `0x08010500_00000000`（拿 DW2 当低 32 位、DW3 当高 32 位）→ DMA 映射失败 →
//       **整条命令被静默丢掉**：PxCI 一直挂着、PxIS/PxTFD 一动不动、`-trace ahci_*` 里连一个
//       命令事件都没有（现场：`ci=0x1 is=0x0 tfd=0x130`，端口 SIG/DET 都正常）。
//       定位手段（可复现）：`-trace enable=ahci_*,handle_cmd_*`（★ 那些"命令被跳过"的
//       事件名是 `handle_cmd_*`，只开 `ahci_*` 会把它们全部滤掉！）+ 反汇编
//       `qemu-system-x86_64.exe`（AHCI 读命令表地址的代码就是 `mov 0x8(%rax),%rdx` 后
//       dma map 0x80 字节再查 FIS type==0x27）+ 用 gdb 附到 QEMU 进程核对 AHCIDevice 里
//       `lst`/`cmd`/`cmd_issue` 的真值（当时三个都是"该有的值"，所以问题只能在命令头里）。
//   K) ★ PRDT 项（16B）的字段偏移：+0x00 DBA、+0x04 DBAU、+0x08 **保留**、+0x0C DBC
//       （bits[21:0] = 字节数-1，bit31 = I）。DBC 写到 +0x08 是错的（那是保留 DWORD）：
//       QEMU 在 ahci_populate_sglist 里读的是 `lea 0xc(%rax),%rcx` + `and $0x3fffff,%edx`，
//       写错位置等于 DMA 长度 0，命令即使被受理也搬不动数据。
#include "ahci64.h"
#include "debug64.h"
#include "hwinfo64.h"      // 识别结果填进 hwinfo64 的磁盘表（任务管理器/设置页读它）
#include "memlayout64.h"   // ★ 必须排在 mem_64.h 之前（mem_64.h 里 #define PAGE_SIZE_64 会撞）
#include "mem_64.h"
#include "port.h"
#include "x86_64.h"

extern "C" char __bss_end[];          // 内核镜像高半区上界（判断指针是否落在内核镜像里）

// ==================== AHCI 寄存器（只列用到的）====================
enum : uint32_t {
    AHCI_CAP    = 0x00,   // Host Capabilities（NP = bit4:0，端口数 = NP + 1）
    AHCI_GHC    = 0x04,   // Global Host Control（HR=bit0，IE=bit1，AE=bit31）
    AHCI_IS     = 0x08,   // Interrupt Status（写 1 清）
    AHCI_PI     = 0x0C,   // Ports Implemented（位图）
    AHCI_VS     = 0x10,   // Version
    AHCI_BOHC   = 0x28,   // BIOS/OS Handoff Control（BOS=bit0，OOS=bit1）
    // ---- 端口寄存器（基址 0x100 + 端口号 * 0x80）----
    P_CLB   = 0x00, P_CLBU = 0x04,   // 命令列表基址（1KB 对齐）
    P_FB    = 0x08, P_FBU  = 0x0C,   // FIS 接收区基址（256B 对齐）
    P_IS    = 0x10, P_IE   = 0x14, P_CMD = 0x18,
    P_TFD   = 0x20, P_SIG  = 0x24, P_SSTS = 0x28,
    P_SCTL  = 0x2C, P_SERR = 0x30, P_SACT = 0x34, P_CI = 0x38, P_SNTF = 0x3C,
};

static inline uint32_t PORT_BASE(uint32_t p) { return 0x100u + p * 0x80u; }

static const uint32_t GHC_HR = 1u << 0;
static const uint32_t GHC_AE = 1u << 31;
static const uint32_t BOHC_BOS = 1u << 0;
static const uint32_t BOHC_OOS = 1u << 1;

static const uint32_t CMD_ST   = 1u << 0;    // Start（命令列表引擎）
static const uint32_t CMD_SUD  = 1u << 1;    // Spin Up Device
static const uint32_t CMD_POD  = 1u << 2;    // Power On Device
static const uint32_t CMD_CLO  = 1u << 3;    // Command List Overrun
static const uint32_t CMD_FRE  = 1u << 4;    // FIS Receive Enable
static const uint32_t CMD_CR   = 1u << 15;   // Command List Running（只读镜像位）

// 需要清的状态位（**不要**用 0xFFFFFFFF，见踩坑 C）
static const uint32_t IS_DHRS        = 1u << 0;    // Device to Host Register FIS
static const uint32_t SERR_DIAG_X    = 1u << 16;   // PHY 内部错误

// PxIS 错误位：IFS(27) INFS(26) OFS(25) TFES(30) HBDS(29) HBFS(28)
static const uint32_t IS_ERR_MASK = (1u << 30) | (1u << 29) | (1u << 28) |
                                    (1u << 27) | (1u << 26) | (1u << 25);
// PxTFD：bit0=ERR bit5=DF（设备错误/设备故障）
static const uint32_t TFD_ERR = 1u << 0;
static const uint32_t TFD_DF  = 1u << 5;

// 命令码
static const uint8_t ATA_CMD_IDENTIFY       = 0xEC;   // IDENTIFY DEVICE
static const uint8_t ATA_CMD_IDENTIFY_PKT   = 0xA1;   // IDENTIFY PACKET DEVICE（ATAPI）
static const uint8_t ATA_CMD_READ_DMA_EXT   = 0x25;   // READ DMA EXT（LBA48）
static const uint8_t ATA_CMD_WRITE_DMA_EXT  = 0x35;   // WRITE DMA EXT（LBA48）

// PxSIG
static const uint32_t SIG_ATA   = 0x00000101u;
static const uint32_t SIG_ATAPI = 0xEB140101u;

// 超时（250Hz PIT：250 tick = 1s）+ 自旋兜底（IF=0 时 g_ticks64 不前进，只靠计时会死循环）
static const uint32_t AHCI64_CMD_TICKS   = 250;
static const uint32_t AHCI64_INIT_TICKS  = 50;         // 200ms：复位/端口检测的有界等待
static const uint32_t AHCI64_SPIN_GUARD  = 400000000u;
// 轮询里"先自旋多少次再开始 hlt"（见踩坑 E）
static const uint32_t AHCI64_SPIN_BEFORE_HLT = 4096;

// 一次命令最多搬多少扇区：128 × 512B = 64KB（= DMA 暂存池大小，见 ahci_xfer 的说明）
static const uint32_t AHCI64_CHUNK_SECTORS = 128;
static const int      AHCI64_DMA_PAGES     = 16;       // 16 × 4KB = 64KB

// ==================== 状态 ====================
static Ahci64CtrlInfo g_ctrl;                 // 控制器摘要（found=0 = 没有/没初始化）
static int  g_inited = 0;                     // 幂等标志
static uint8_t g_bus = 0, g_dev_id = 0, g_fn = 0;

// 一台端口设备（ATA 盘或 ATAPI 光驱）经识别后的**最终结果**
struct Ahci64Dev {
    uint8_t  port;        // 硬件端口号
    uint8_t  kind;        // Ahci64Kind
    uint8_t  used;        // 1 = 已分配命令列表/FIS/命令表
    int      drive_no;    // 统一驱动器号（AHCI 盘 = 8 + idx）；ATAPI = -1
    uint32_t sig;
    char     model[41];
    uint64_t sectors;
    uint8_t* cl;          // 命令列表（1KB，1KB 对齐）
    uint8_t* fis;         // FIS 接收区（256B，256B 对齐）
    uint8_t* ct;          // 命令表（128B 对齐；PRDT 从 +0x80 起）
};
static Ahci64Dev g_devs[AHCI64_MAX_DEVS];
static int       g_dev_count   = 0;   // 登记的设备数（ATA + ATAPI）
static int       g_drive_count = 0;   // 其中 ATA 盘数（= ahci64_count64()）

static uint8_t*  g_dma[AHCI64_DMA_PAGES];   // DMA 暂存池（每页 4KB，页池分配，**不保证连续**）
static uint8_t   g_first_write_logged = 0;  // 首次写盘打一行证据（避免刷屏）

static bool ahci_port_start(Ahci64Dev& d);   // 前向声明（ahci_port_restart 会用）

// ==================== PCI 配置空间（0xCF8/0xCFC）====================
// 读：只写地址端口、读数据端口；写：只在**本驱动自己找到的设备**上开 MEM/BUS MASTER。
static uint32_t apci_rd32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    const uint32_t addr = 0x80000000u
                        | ((uint32_t)bus << 16)
                        | ((uint32_t)dev << 11)
                        | ((uint32_t)fn << 8)
                        | (uint32_t)(off & 0xFCu);
    outl(0xCF8u, addr);
    return inl(0xCFCu);
}
static void apci_wr32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val) {
    const uint32_t addr = 0x80000000u
                        | ((uint32_t)bus << 16)
                        | ((uint32_t)dev << 11)
                        | ((uint32_t)fn << 8)
                        | (uint32_t)(off & 0xFCu);
    outl(0xCF8u, addr);
    outl(0xCFCu, val);
}

// 扫 0..255 号总线找 AHCI 控制器（class 0x01 / subclass 0x06 / prog-if 0x01）。
// 连续 4 条空总线提前停（与 e1000_64/hwinfo64 同款）。
static bool ahci_pci_find() {
    uint32_t empty_run = 0;
    for (uint32_t bus = 0; bus < 256; bus++) {
        bool bus_has = false;
        for (uint32_t dev = 0; dev < 32; dev++) {
            uint32_t id = apci_rd32((uint8_t)bus, (uint8_t)dev, 0, 0x00);
            uint16_t vendor = (uint16_t)(id & 0xFFFFu);
            if (vendor == 0xFFFFu || vendor == 0x0000u) continue;
            bus_has = true;
            const uint32_t hdr = apci_rd32((uint8_t)bus, (uint8_t)dev, 0, 0x0C);
            const uint32_t nfn = (hdr & 0x00800000u) ? 8u : 1u;
            for (uint32_t fn = 0; fn < nfn; fn++) {
                if (fn != 0) {
                    id = apci_rd32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x00);
                    vendor = (uint16_t)(id & 0xFFFFu);
                    if (vendor == 0xFFFFu || vendor == 0x0000u) continue;
                }
                const uint32_t cc = apci_rd32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x08);
                const uint8_t class_code = (uint8_t)(cc >> 24);
                const uint8_t subclass   = (uint8_t)(cc >> 16);
                const uint8_t prog_if    = (uint8_t)(cc >> 8);
                if (class_code == 0x01 && subclass == 0x06 && prog_if == 0x01) {
                    g_bus = (uint8_t)bus;
                    g_dev_id = (uint8_t)dev;
                    g_fn = (uint8_t)fn;
                    return true;
                }
            }
        }
        if (bus_has) {
            empty_run = 0;
        } else if (++empty_run >= 4u) {
            break;
        }
    }
    return false;
}

// ==================== MMIO ====================
static inline uint32_t ahci_rd(uint32_t off) {
    return *(volatile uint32_t*)(uintptr_t)(g_ctrl.abar + off);
}
static inline void ahci_wr(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(uintptr_t)(g_ctrl.abar + off) = v;
    __asm__ volatile("" ::: "memory");       // 防编译器把 MMIO 写挪位/合并
}
static inline uint32_t prd(uint8_t p, uint32_t reg)            { return ahci_rd(PORT_BASE(p) + reg); }
static inline void     prw(uint8_t p, uint32_t reg, uint32_t v){ ahci_wr(PORT_BASE(p) + reg, v); }

static inline bool ahci_irqs_on() {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0" : "=r"(fl));
    return (fl & 0x200ULL) != 0;              // RFLAGS.IF
}

// 有界轮询：等到 (reg & mask) == want；超时/自旋上限都算失败（绝不挂死）。
// 见踩坑 E：自旋 N 次之后开始 hlt 让出，让宿主机（或真机的设备状态机）有机会推进。
static bool ahci_wait(uint8_t p, uint32_t reg, uint32_t mask, uint32_t want, uint32_t ticks) {
    const uint64_t t0 = g_ticks64;
    const bool can_hlt = ahci_irqs_on();
    uint32_t spin = 0;
    for (;;) {
        if ((prd(p, reg) & mask) == want) return true;
        if (g_ticks64 - t0 > (uint64_t)ticks) return false;
        if (++spin > AHCI64_SPIN_GUARD) return false;
        if (can_hlt && spin > AHCI64_SPIN_BEFORE_HLT) __asm__ volatile("hlt");
        else                                          __asm__ volatile("pause");
    }
}

// GHC.HR 自清等待（定义放在 init 之后也行；这里紧跟 ahci_wait）
static bool ahci_wait_ghc_hr() {
    const uint64_t t0 = g_ticks64;
    uint32_t spin = 0;
    for (;;) {
        if (!(ahci_rd(AHCI_GHC) & GHC_HR)) return true;
        if (g_ticks64 - t0 > (uint64_t)AHCI64_INIT_TICKS) return false;
        if (++spin > AHCI64_SPIN_GUARD) return false;
        __asm__ volatile("pause");
    }
}

// ==================== 物理地址换算 ====================
// 低内存（< 4GB）：恒等映射，PA == VA（e1000/mem64/loader 同款约定）；
// 内核镜像高半区对象（.bss/.data/栈上的 static 数组）：直映关系 PA = VA - 偏移；
// 其余（认不出来）：返回 0 -> 调用方走 DMA 暂存兜底。
static inline uint64_t ahci_pa64(const void* ptr) {
    const uint64_t v = (uint64_t)(uintptr_t)ptr;
    if (v < 0x100000000ULL) return v;
    if (v >= ML64_KERNEL_VA_BASE && v < ((uint64_t)(uintptr_t)__bss_end) + 0x10000ULL) {
        return v - (ML64_KERNEL_VA_BASE - (uint64_t)ML64_KERNEL_BASE);
    }
    return 0;
}

// ==================== 命令下发 ====================
struct Ahci64Prd { uint64_t pa; uint32_t bytes; };

// CFIS：H2D Register FIS（20 字节有效，其余填 0）
static void ahci_build_cfis(uint8_t* cfis, uint8_t cmd, uint64_t lba, uint32_t count) {
    for (int i = 0; i < 64; i++) cfis[i] = 0;
    cfis[0]  = 0x27;                        // FIS type = H2D Register FIS
    cfis[1]  = 0x80;                        // C=1：命令寄存器更新
    cfis[2]  = cmd;
    cfis[3]  = 0;                           // features low
    cfis[4]  = (uint8_t)(lba >> 0);         // LBA0
    cfis[5]  = (uint8_t)(lba >> 8);         // LBA1
    cfis[6]  = (uint8_t)(lba >> 16);        // LBA2
    cfis[7]  = 0x40;                        // device：LBA 模式
    cfis[8]  = (uint8_t)(lba >> 24);        // LBA3
    cfis[9]  = (uint8_t)(lba >> 32);        // LBA4
    cfis[10] = (uint8_t)(lba >> 40);        // LBA5
    cfis[11] = 0;                           // features high
    cfis[12] = (uint8_t)(count >> 0);       // count low
    cfis[13] = (uint8_t)(count >> 8);       // count high
    cfis[14] = 0;                           // ICC
    cfis[15] = 0;                           // control
}

// ---- 诊断打点：端口寄存器快照（启动前后 / 发命令前后各一份）----
// 为什么留着：PxCI 挂着不动时，这 5 个寄存器就是"命令到底有没有进 HBA"的唯一旁证
//（cmd 里有 ST/FRE，tfd 里有 BSY/DRQ/ERR/DF，ssts 里有 DET/IPM，is 里有 DHRS，
//  serr 里有接口错误）—— 排 AHCI 问题基本全靠这一行 + trace。见踩坑 J/K。
static void ahci_dump_regs64(const char* tag, uint8_t p) {
    dbg64_line_begin64();
    dbg64_str("[AHCI64] regs("); dbg64_str(tag); dbg64_str(") port="); dbg64_dec(p);
    dbg64_str(" cmd=0x");  dbg64_hex64(prd(p, P_CMD));
    dbg64_str(" tfd=0x");  dbg64_hex64(prd(p, P_TFD));
    dbg64_str(" ssts=0x"); dbg64_hex64(prd(p, P_SSTS));
    dbg64_str(" is=0x");   dbg64_hex64(prd(p, P_IS));
    dbg64_str(" serr=0x"); dbg64_hex64(prd(p, P_SERR));
    dbg64_nl();
    dbg64_line_end64();
}

// ---- 诊断打点：命令头 DW0..DW7 + PRDT 逐项（只打第一条命令，避免刷屏）----
// 这条打点就是这次定位"命令不执行"的关键：把 DW0..DW7 摆出来才能看出
// PRDTL 有没有放进 DW0 的 bits[31:16]、CTBA 有没有放进 DW2/DW3。
static void ahci_dump_hdr64(uint8_t p, const volatile uint32_t* hdr, const uint8_t* ct, int nprd) {
    dbg64_line_begin64();
    dbg64_str("[AHCI64] hdr port="); dbg64_dec(p);
    dbg64_str(" cfl=");   dbg64_dec(hdr[0] & 0x1Fu);
    dbg64_str(" a=");     dbg64_dec((hdr[0] >> 5) & 1u);
    dbg64_str(" w=");     dbg64_dec((hdr[0] >> 6) & 1u);
    dbg64_str(" p=");     dbg64_dec((hdr[0] >> 7) & 1u);
    dbg64_str(" r=");     dbg64_dec((hdr[0] >> 8) & 1u);      // R（Device Reset）必须是 0
    dbg64_str(" b=");     dbg64_dec((hdr[0] >> 9) & 1u);
    dbg64_str(" pmp=");   dbg64_dec((hdr[0] >> 11) & 0xFu);
    dbg64_str(" prdtl="); dbg64_dec(hdr[0] >> 16);            // 规范：PRDTL 在 DW0 的 bits[31:16]
    dbg64_str(" dw0=0x"); dbg64_hex64(hdr[0]);
    dbg64_str(" dw1=0x"); dbg64_hex64(hdr[1]);                // PRDBC（HBA 回填，软件写 0）
    dbg64_str(" dw2=0x"); dbg64_hex64(hdr[2]);                // CTBA 低 32 位
    dbg64_str(" dw3=0x"); dbg64_hex64(hdr[3]);                // CTBAU
    dbg64_str(" ctba=0x");
    dbg64_hex64((uint64_t)hdr[2] | ((uint64_t)hdr[3] << 32));
    dbg64_nl();
    dbg64_line_end64();

    for (int i = 0; i < nprd; i++) {
        const uint8_t* pe = ct + 0x80 + i * 16;
        const uint64_t dba   = *(const volatile uint64_t*)(const void*)pe;
        const uint32_t dbcdw = *(const volatile uint32_t*)(const void*)(pe + 12);
        dbg64_line_begin64();
        dbg64_str("[AHCI64] prdt"); dbg64_dec((uint64_t)i);
        dbg64_str(" dba=0x");  dbg64_hex64(dba);
        dbg64_str(" dbc=0x");  dbg64_hex64(dbcdw & 0x3FFFFFu);
        dbg64_str(" bytes=");  dbg64_dec((uint64_t)(dbcdw & 0x3FFFFFu) + 1u);
        dbg64_str(" i=");      dbg64_dec((dbcdw >> 31) & 1u);
        dbg64_nl();
        dbg64_line_end64();
    }
}

// 命令头/PRDT/寄存器诊断只打一次（第一条命令），之后不再刷屏
static uint8_t g_hdr_dumped = 0;

// 下发一条命令并轮询完成。返回 true = 成功（PxCI 清零 + 无错误位）。
static bool ahci_cmd(int di, uint8_t cmd, uint64_t lba, uint32_t count,
                     const Ahci64Prd* prds, int nprd, bool write, bool atapi_cmd) {
    Ahci64Dev& d = g_devs[di];
    const uint8_t p = d.port;

    // 端口必须在跑（ST=1 才能收命令）
    if (!(prd(p, P_CMD) & CMD_ST)) return false;
    // 上一条命令必须已经结束（PxCI slot0 清零）；给 10 tick 的宽限
    if (!ahci_wait(p, P_CI, 1u, 0u, 10)) return false;
    const bool dump_first = (g_hdr_dumped == 0);                 // 只给第一条命令做逐字段打点
    // ---- 命令表（CFIS/ACMD/PRDT）----
    uint8_t* ct = d.ct;
    ahci_build_cfis(ct, cmd, lba, count);
    for (int i = 0x40; i < 0x80; i++) ct[i] = 0;                 // ACMD（本驱动不用 PACKET 命令）
    for (int i = 0; i < nprd; i++) {
        uint8_t* pe = ct + 0x80 + i * 16;
        // PRDT 项（16B）：+0x00 DBA、+0x04 DBAU、+0x08 **保留**、+0x0C DBC（bits[21:0] = 字节数-1，
        // bit31 = I）。★ 踩坑 K：DBC 在 **+0x0C**，**不是** +0x08（+0x08 是保留 DWORD）。
        *(volatile uint64_t*)(void*)pe       = prds[i].pa;       // DBA / DBAU
        *(volatile uint32_t*)(void*)(pe + 8) = 0;                // 保留（写 0）
        *(volatile uint32_t*)(void*)(pe + 12) =
              ((prds[i].bytes - 1u) & 0x3FFFFFu)                 // DBC = 字节数 - 1
            | ((i == nprd - 1) ? 0x80000000u : 0u);              // 末项 I=1（完成中断；本驱动不用中断）
    }

    // ---- 命令头（命令列表 slot 0，32 字节）----
    // ★ 踩坑 J（AHCI 命令头的正确布局，写错这里 HBA 会**静默丢弃**整条命令）：
    //   DW0 bits[4:0]=CFL(5) bit5=A bit6=W bit7=P bit8=R bit9=B bits[12:11]=PMP
    //       并且 **bits[31:16] = PRDTL（PRDT 项数）**；
    //   DW1 = PRDBC（HBA 完成后回填，软件写 0）；
    //   DW2/DW3 = CTBA/CTBAU（命令表物理地址，128B 对齐）；DW4..DW7 = 保留。
    const uint64_t ct_pa = (uint64_t)(uintptr_t)ct;
    volatile uint32_t* hdr = (volatile uint32_t*)(void*)d.cl;
    hdr[0] = (5u & 0x1Fu)                                        // CFL = 5 dwords（20B 的 H2D FIS）
           | (atapi_cmd ? (1u << 5) : 0u)                        // A（ATAPI 命令；IDENTIFY PACKET DEVICE 不是）
           | (write ? (1u << 6) : 0u)                            // W
           | ((uint32_t)nprd << 16);                             // PRDTL（DW0 的 bits[31:16]，**不是** DW1）
    hdr[1] = 0;                                                   // PRDBC（HBA 回填）
    hdr[2] = (uint32_t)(ct_pa & 0xFFFFFFFFu);                     // CTBA（DW2；128B 对齐由分配保证）
    hdr[3] = (uint32_t)(ct_pa >> 32);                             // CTBAU（DW3）
    hdr[4] = 0; hdr[5] = 0; hdr[6] = 0; hdr[7] = 0;
    __asm__ volatile("" ::: "memory");

    // ---- 诊断：命令头/PRDT 逐字段 + 发命令前的端口寄存器快照（自检/真机排障用，只第一条命令）----
    if (dump_first) {
        ahci_dump_hdr64(p, hdr, ct, nprd);
        ahci_dump_regs64("pre", p);
    }

    // ---- 清状态位、发命令 ----
    // 只清 DHRS 一位（见踩坑 C：**不要**写 0xFFFFFFFF）。
    prw(p, P_IS, IS_DHRS);
    prw(p, P_CI, 1u);                                             // slot 0

    const bool done = ahci_wait(p, P_CI, 1u, 0u, AHCI64_CMD_TICKS);
    const uint32_t is  = prd(p, P_IS);
    const uint32_t tfd = prd(p, P_TFD);
    if (dump_first) {                                            // 发命令后的寄存器快照（含 DHRS 完成位）
        ahci_dump_regs64("post", p);
        g_hdr_dumped = 1;
    }
    prw(p, P_IS, IS_DHRS);                                        // 收尾：别把完成位留给下一条

    if (!done) {
        dbg64_line_begin64();
        dbg64_str("[AHCI64] cmd timeout port=");  dbg64_dec(p);
        dbg64_str(" cmd=0x");                     dbg64_hex64(cmd);
        dbg64_str(" ci=0x");                      dbg64_hex64(prd(p, P_CI));
        dbg64_str(" is=0x");                      dbg64_hex64(is);
        dbg64_str(" tfd=0x");                     dbg64_hex64(tfd); dbg64_nl();
        dbg64_line_end64();
        return false;
    }
    if (is & IS_ERR_MASK) {
        dbg64_line_begin64();
        dbg64_str("[AHCI64] cmd error port=");    dbg64_dec(p);
        dbg64_str(" cmd=0x");                     dbg64_hex64(cmd);
        dbg64_str(" is=0x");                      dbg64_hex64(is);
        dbg64_str(" tfd=0x");                     dbg64_hex64(tfd); dbg64_nl();
        dbg64_line_end64();
        return false;
    }
    if (tfd & (TFD_ERR | TFD_DF)) {
        dbg64_line_begin64();
        dbg64_str("[AHCI64] dev error port=");   dbg64_dec(p);
        dbg64_str(" cmd=0x");                    dbg64_hex64(cmd);
        dbg64_str(" tfd=0x");                    dbg64_hex64(tfd);
        dbg64_str(" err=0x");                    dbg64_hex64((tfd >> 8) & 0xFFu); dbg64_nl();
        dbg64_line_end64();
        return false;
    }
    return true;
}

// ---- 端口"停->启"重启 + 一次重试 ----
// 为什么要有：真机上某些 HBA/设备在冷启动后需要一次 link/COMRESET 周期才开始正常执行命令；
// 这里用"停端口 -> 读回 PxCMD 只 OR 进 ST"的最小动作重启（不重编程 CLB/FIS、不动 FRE），
// 然后重发同一条命令。重试仍失败就如实返回 false —— 绝不假装成功。
static void ahci_port_restart(Ahci64Dev& d) {
    uint32_t cmd = prd(d.port, P_CMD);
    if (cmd & CMD_ST) {
        prw(d.port, P_CMD, cmd & ~CMD_ST);
        (void)ahci_wait(d.port, P_CMD, CMD_CLO | CMD_CR, 0, AHCI64_INIT_TICKS);
    }
    cmd = prd(d.port, P_CMD);
    prw(d.port, P_CMD, cmd | CMD_ST);
    (void)ahci_wait(d.port, P_CMD, CMD_ST, CMD_ST, AHCI64_INIT_TICKS);
}

static bool ahci_cmd_retry(int di, uint8_t cmd, uint64_t lba, uint32_t count,
                           const Ahci64Prd* prds, int nprd, bool write, bool atapi_cmd) {
    if (ahci_cmd(di, cmd, lba, count, prds, nprd, write, atapi_cmd)) return true;
    Ahci64Dev& d = g_devs[di];
    ahci_port_restart(d);
    const bool ok = ahci_cmd(di, cmd, lba, count, prds, nprd, write, atapi_cmd);
    dbg64_line_begin64();
    dbg64_str("[AHCI64] port restart retry port="); dbg64_dec(d.port);
    dbg64_str(" cmd=0x"); dbg64_hex64(cmd);
    dbg64_str(" result="); dbg64_str(ok ? "ok" : "fail");
    dbg64_nl();
    dbg64_line_end64();
    return ok;
}

// 一次数据搬运（count 个扇区）：按 ≤128 扇区（64KB）分块下发。
// 为什么 64KB 分块：
//   * DMA 暂存池是 16 个 4KB 页（页池分配**不保证连续**），所以暂存路径天然按每页一项 PRDT，
//     最多 16 项；块大小与暂存池等大，拷贝量刚好对得上；
//   * PRDT 的 DBC 是 22 位（≤ 4MB-1），64KB 远在限内；
//   * 传统 IDE 的"不能跨 64K 边界"是 PIO/IDE 总线约束，**AHCI DMA 没有这条**；这里仍然按
//     64KB 分块，是为了让"直传/暂存"两条路径行为一致、单条命令耗时可控（真机也能预期）。
static bool ahci_xfer(int di, uint8_t cmd, uint32_t lba, uint32_t count, void* buf, bool write) {
    if (count == 0) return true;
    Ahci64Dev& d = g_devs[di];
    uint8_t* p8 = (uint8_t*)buf;

    for (uint32_t done = 0; done < count; ) {
        const uint32_t nsec = (count - done > AHCI64_CHUNK_SECTORS)
                            ? AHCI64_CHUNK_SECTORS : (count - done);
        const uint32_t bytes = nsec * 512u;
        uint8_t* p = p8 + (uint64_t)done * 512u;

        // ---- 直传路径：PA 连续且 2 字节对齐（一个 PRDT 项）----
        const uint64_t pa = ahci_pa64(p);
        Ahci64Prd prds[AHCI64_DMA_PAGES];
        int nprd = 0;
        bool bounce = false;
        if (pa != 0 && (pa & 1u) == 0) {
            prds[0].pa = pa;
            prds[0].bytes = bytes;
            nprd = 1;
        } else {
            // ---- 暂存路径：DMA 只碰页池里那 16 页，数据再逐页 memcpy ----
            bounce = true;
            if (!g_dma[0]) return false;              // 页池没拿到缓冲：如实失败
            uint32_t left = bytes;
            for (int i = 0; i < AHCI64_DMA_PAGES && left > 0; i++) {
                prds[i].pa = (uint64_t)(uintptr_t)g_dma[i];
                prds[i].bytes = (left > 4096u) ? 4096u : left;
                left -= prds[i].bytes;
                nprd++;
            }
            if (left != 0) return false;
            if (write) {
                uint32_t off = 0;
                for (int i = 0; i < nprd; i++) {
                    for (uint32_t k = 0; k < prds[i].bytes; k++) g_dma[i][k] = p[off + k];
                    off += prds[i].bytes;
                }
            }
        }

        if (!ahci_cmd_retry(di, cmd, (uint64_t)lba + done, nsec, prds, nprd, write, false)) {
            dbg64_line_begin64();
            dbg64_str(write ? "[AHCI64] write lba=" : "[AHCI64] read lba=");
            dbg64_dec((uint64_t)lba + done);
            dbg64_str(" count=");
            dbg64_dec(nsec);
            dbg64_str(" failed tfd=0x");
            dbg64_hex64(prd(d.port, P_TFD));
            dbg64_str(" is=0x");
            dbg64_hex64(prd(d.port, P_IS));
            dbg64_str(" serr=0x");
            dbg64_hex64(prd(d.port, P_SERR));
            dbg64_nl();
            dbg64_line_end64();
            return false;
        }

        if (bounce && !write) {
            uint32_t off = 0;
            for (int i = 0; i < nprd; i++) {
                for (uint32_t k = 0; k < prds[i].bytes; k++) p[off + k] = g_dma[i][k];
                off += prds[i].bytes;
            }
        }
        done += nsec;
    }
    return true;
}

// ==================== IDENTIFY（型号 + 容量）====================
// 容量口径：**用 IDENTIFY DEVICE（0xEC）**，优先 LBA48（word 83 bit10 支持且 words 100..103 非 0），
// 否则退回 LBA28（words 60/61）。ATAPI 只取型号（0xA1），容量记 0。
// 签名只是提示（见踩坑 A）：0xEC 失败就换 0xA1 重试一次，并把 kind 改成与成功那条一致。
static bool ahci_identify_try(int di, uint8_t cmd) {
    Ahci64Dev& d = g_devs[di];
    uint8_t* tmp = g_dma[0];
    for (int i = 0; i < 512; i++) tmp[i] = 0;

    Ahci64Prd prd1;
    prd1.pa = (uint64_t)(uintptr_t)tmp;
    prd1.bytes = 512;
    // 注意：IDENTIFY PACKET DEVICE 是**设备命令**（不是 PACKET 命令），命令头 A 位 = 0。
    if (!ahci_cmd_retry(di, cmd, 0, 0, &prd1, 1, false, false)) return false;

    const uint16_t* w = (const uint16_t*)tmp;
    // 型号：words 27..46（大端字节序），去尾空格
    for (int i = 0; i < 20; i++) {
        d.model[i * 2]     = (char)(w[27 + i] >> 8);
        d.model[i * 2 + 1] = (char)(w[27 + i] & 0xFF);
    }
    d.model[40] = 0;
    for (int i = 39; i >= 0 && d.model[i] == ' '; i--) d.model[i] = 0;

    if (cmd == ATA_CMD_IDENTIFY) {
        const uint64_t lba48 = (uint64_t)(uint32_t)w[100]
                             | ((uint64_t)(uint32_t)w[101] << 16)
                             | ((uint64_t)(uint32_t)w[102] << 32)
                             | ((uint64_t)(uint32_t)w[103] << 48);
        const uint32_t lba28 = (uint32_t)w[60] | ((uint32_t)w[61] << 16);
        const bool has_lba48 = (w[83] & 0x0400u) != 0;
        d.sectors = (has_lba48 && lba48 != 0) ? lba48 : (uint64_t)lba28;
        d.kind = AHCI64_KIND_ATA;
    } else {
        d.sectors = 0;
        d.kind = AHCI64_KIND_ATAPI;
    }
    return true;
}

static bool ahci_identify(int di) {
    Ahci64Dev& d = g_devs[di];
    if (!g_dma[0]) return false;
    const uint8_t first = (d.kind == AHCI64_KIND_ATAPI) ? ATA_CMD_IDENTIFY_PKT : ATA_CMD_IDENTIFY;
    if (ahci_identify_try(di, first)) return true;
    const uint8_t second = (first == ATA_CMD_IDENTIFY) ? ATA_CMD_IDENTIFY_PKT : ATA_CMD_IDENTIFY;
    return ahci_identify_try(di, second);
}

// ==================== 端口准备 / 启动 ====================
// 停端口（ST=0，等 CLO/CR 落，FRE=0）+ 清状态位 + 需要时重新检测设备。
static void ahci_port_prepare(uint8_t p) {
    // 1) 停：ST=0 -> 等 CLO(bit3)/CR(bit15) 落 -> FRE=0
    uint32_t cmd = prd(p, P_CMD);
    if (cmd & CMD_ST) {
        prw(p, P_CMD, cmd & ~CMD_ST);
        (void)ahci_wait(p, P_CMD, CMD_CLO | CMD_CR, 0, AHCI64_INIT_TICKS);
    }
    cmd = prd(p, P_CMD);
    if (cmd & CMD_FRE) prw(p, P_CMD, cmd & ~CMD_FRE);
    // 2) 清状态位（**只清该清的位**，见踩坑 C）
    prw(p, P_IS, IS_DHRS);
    prw(p, P_SERR, SERR_DIAG_X);
    // 3) 设备检测：**只有当前状态不是"就绪"时才触发一次重新检测**（见踩坑 F）
    if ((prd(p, P_SSTS) & 0xFu) != 3u) {
        prw(p, P_SCTL, 0);                                  // DET=0：让硬件重新检测设备
        (void)ahci_wait(p, P_SSTS, 0xFu, 3u, AHCI64_INIT_TICKS);
    }
}

// 启动端口：写 PxCLB/PxFB（必须在 ST=0 时写）-> FRE=1 -> ST=1。
// 对齐要求（1KB/256B/128B）全部由 ahci64_init64 里的页分配保证。
static bool ahci_port_start(Ahci64Dev& d) {
    const uint64_t cl_pa  = (uint64_t)(uintptr_t)d.cl;
    const uint64_t fis_pa = (uint64_t)(uintptr_t)d.fis;
    prw(d.port, P_CLB,  (uint32_t)(cl_pa & 0xFFFFFFFFu));
    prw(d.port, P_CLBU, (uint32_t)(cl_pa >> 32));
    prw(d.port, P_FB,   (uint32_t)(fis_pa & 0xFFFFFFFFu));
    prw(d.port, P_FBU,  (uint32_t)(fis_pa >> 32));
    prw(d.port, P_IE, 0);                       // 不开端口中断：本驱动全程轮询
    prw(d.port, P_IS, IS_DHRS);
    // ★ 启动端口：照 SeaBIOS / Linux libata 的写法 —— **读-改-写，只 OR 进 FRE / ST**。
    //   不要写 ICC（见踩坑 D）。顺序：先 FRE 再 ST（AHCI 规范 3.3.2 的建议顺序）。
    uint32_t cmd = prd(d.port, P_CMD);
    prw(d.port, P_CMD, cmd | CMD_FRE);
    cmd = prd(d.port, P_CMD);
    prw(d.port, P_CMD, cmd | CMD_ST);
    // 注：CR(bit15)/FR(bit14) 是只读镜像位，回写无害；ST=1 之后硬件会置 CR。
    return ahci_wait(d.port, P_CMD, CMD_ST, CMD_ST, AHCI64_INIT_TICKS);
}

// ==================== 初始化 ====================
void ahci64_init64() {
    if (g_inited) return;
    g_inited = 1;
    g_ctrl.found = 0;
    g_dev_count = 0;
    g_drive_count = 0;
    g_first_write_logged = 0;

    dbg64_line_begin64();
    if (!ahci_pci_find()) {
        dbg64_str("[AHCI64] not found");
        dbg64_nl();
        dbg64_line_end64();
        return;                                  // 优雅降级：PATA 路径照常
    }
    dbg64_line_end64();

    // ---- 打开 MEM + BUS MASTER（DMA 必需），读 BAR5 = ABAR（64 位 MMIO BAR）----
    const uint32_t pci_cmd = apci_rd32(g_bus, g_dev_id, g_fn, 0x04);
    apci_wr32(g_bus, g_dev_id, g_fn, 0x04, (pci_cmd & 0xFFFF0000u) | 0x0006u);
    const uint32_t bar5 = apci_rd32(g_bus, g_dev_id, g_fn, 0x24);
    uint64_t abar = (uint64_t)(bar5 & 0xFFFFFFF0u);
    if (bar5 & 0x4u) {                            // 64 位 BAR：高 32 位在偏移 0x28
        const uint32_t bar_hi = apci_rd32(g_bus, g_dev_id, g_fn, 0x28);
        abar |= (uint64_t)bar_hi << 32;
    }
    if (abar == 0 || abar == 0xFFFFFFF0ULL) {
        dbg64_line_begin64();
        dbg64_str("[AHCI64] not found (BAR5 unassigned)");
        dbg64_nl();
        dbg64_line_end64();
        return;
    }
    g_ctrl.abar = abar;
    g_ctrl.bus = g_bus; g_ctrl.dev = g_dev_id; g_ctrl.fn = g_fn;

    // ---- BOHC：BIOS/OS Handoff（真机上固件可能还持有控制器）----
    uint32_t bohc = ahci_rd(AHCI_BOHC);
    if (bohc & BOHC_BOS) {
        ahci_wr(AHCI_BOHC, BOHC_OOS);
        const uint64_t t0 = g_ticks64;
        while ((ahci_rd(AHCI_BOHC) & BOHC_BOS) && (g_ticks64 - t0) < 50u) { __asm__ volatile("pause"); }
        bohc = ahci_rd(AHCI_BOHC);
    }

    // ---- HBA 复位（规范推荐顺序：AE -> 复位 -> 再置 AE）----
    ahci_wr(AHCI_GHC, ahci_rd(AHCI_GHC) | GHC_AE);
    ahci_wr(AHCI_GHC, ahci_rd(AHCI_GHC) | GHC_HR);
    const bool rst_ok = ahci_wait_ghc_hr();
    ahci_wr(AHCI_GHC, ahci_rd(AHCI_GHC) | GHC_AE);
    ahci_wr(AHCI_IS, 0xFFFFFFFFu);                // 清全局中断状态（GHC.IS 是 RW1C）

    g_ctrl.cap = ahci_rd(AHCI_CAP);
    g_ctrl.pi  = ahci_rd(AHCI_PI);
    g_ctrl.vs  = ahci_rd(AHCI_VS);
    g_ctrl.ports = (int)(g_ctrl.cap & 0x1Fu) + 1;

    dbg64_line_begin64();
    dbg64_str("[AHCI64] pci ");
    dbg64_dec(g_ctrl.bus); dbg64_str(":"); dbg64_dec(g_ctrl.dev); dbg64_str("."); dbg64_dec(g_ctrl.fn);
    dbg64_str(" abar=0x"); dbg64_hex64(g_ctrl.abar);
    dbg64_str(" cap=0x");  dbg64_hex64(g_ctrl.cap);
    dbg64_str(" pi=0x");   dbg64_hex64(g_ctrl.pi);
    dbg64_str(" ports=");  dbg64_dec((uint64_t)g_ctrl.ports);
    dbg64_nl();
    dbg64_line_end64();

    dbg64_line_begin64();
    dbg64_str("[AHCI64] hba reset ");
    dbg64_str(rst_ok ? "ok" : "timeout");
    dbg64_str(" bohc=0x"); dbg64_hex64(bohc);
    dbg64_nl();
    dbg64_line_end64();

    // ---- DMA 暂存池（16 × 4KB，页池低内存：PA == VA）----
    for (int i = 0; i < AHCI64_DMA_PAGES; i++) {
        g_dma[i] = (uint8_t*)page_alloc_64();
        if (!g_dma[i]) {                          // 页池不足：只影响"暂存兜底"路径
            for (int k = 0; k < i; k++) { page_free_64(g_dma[k]); g_dma[k] = nullptr; }
            break;
        }
        for (int k = 0; k < 4096; k++) g_dma[i][k] = 0;
    }

    // ---- 逐端口探测 ----
    // ★ 顺序（踩坑 B 逼出来的）：需要时才复位 -> **先启动端口** -> 再读 PxSSTS/PxSIG -> IDENTIFY。
    for (uint32_t p = 0; p < 32u; p++) {
        if (!(g_ctrl.pi & (1u << p))) continue;    // 未实现端口跳过
        ahci_port_prepare((uint8_t)p);

        uint32_t ssts = prd((uint8_t)p, P_SSTS);
        uint32_t sig  = prd((uint8_t)p, P_SIG);
        uint8_t  det  = (uint8_t)(ssts & 0xFu);
        uint8_t  ipm  = (uint8_t)((ssts >> 8) & 0xFu);
        uint8_t  kind = AHCI64_KIND_NONE;
        int slot = -1;                             // g_devs[] 槽位（最后统一回填摘要）

        if (det == 3u && g_dev_count < AHCI64_MAX_DEVS) {
            Ahci64Dev& d = g_devs[g_dev_count];
            slot = g_dev_count;
            d.port = (uint8_t)p;
            d.kind = AHCI64_KIND_NONE;
            d.used = 0;
            d.drive_no = -1;
            d.sig = sig;
            d.model[0] = 0;
            d.sectors = 0;
            d.cl = d.fis = d.ct = nullptr;

            // 一页（4KB）装下三样，对齐要求全部满足（页对齐 -> 1KB/256B/128B 都对齐）
            uint8_t* page = (uint8_t*)page_alloc_64();
            if (!page) {
                slot = -1;
                dbg64_line_begin64();
                dbg64_str("[AHCI64] port skipped (page pool exhausted) port="); dbg64_dec(p); dbg64_nl();
                dbg64_line_end64();
            } else {
                for (int k = 0; k < 4096; k++) page[k] = 0;
                d.cl  = page;             // 0x0000：命令列表 1KB（32 项 × 32B），实际只用 slot 0
                d.fis = page + 1024;      // 0x0400：FIS 接收区 256B（256B 对齐 ✓）
                d.ct  = page + 1280;      // 0x0500：命令表 128+16*16=384B（128B 对齐 ✓）
                if (!ahci_port_start(d)) {
                    dbg64_line_begin64();
                    dbg64_str("[AHCI64] port failed to start port="); dbg64_dec(p); dbg64_nl();
                    dbg64_line_end64();
                    slot = -1;
                } else {
                    // 启动后再读一次：QEMU 下只有这时 PxSSTS/PxSIG 才是真值（踩坑 B）
                    ssts = prd((uint8_t)p, P_SSTS);
                    sig  = prd((uint8_t)p, P_SIG);
                    det  = (uint8_t)(ssts & 0xFu);
                    ipm  = (uint8_t)((ssts >> 8) & 0xFu);
                    d.sig = sig;
                    if (det == 3u) {
                        // 签名只当提示；ahci_identify 会换命令重试并纠正 kind
                        kind = (sig == SIG_ATAPI) ? AHCI64_KIND_ATAPI : AHCI64_KIND_ATA;
                        d.kind = kind;
                        if (!ahci_identify(slot)) {
                            dbg64_line_begin64();
                            dbg64_str("[AHCI64] identify failed port="); dbg64_dec(p);
                            dbg64_str(" tfd=0x");  dbg64_hex64(prd((uint8_t)p, P_TFD));
                            dbg64_str(" serr=0x"); dbg64_hex64(prd((uint8_t)p, P_SERR));
                            dbg64_nl();
                            dbg64_line_end64();
                            d.kind = AHCI64_KIND_NONE;     // 如实：设备在位但认不出来
                            kind = AHCI64_KIND_NONE;
                        }
                    }
                }
            }
        } else if (det == 3u) {
            dbg64_line_begin64();
            dbg64_str("[AHCI64] note: more than ");
            dbg64_dec((uint64_t)AHCI64_MAX_DEVS);
            dbg64_str(" devices on AHCI; extra port skipped port=");
            dbg64_dec(p); dbg64_nl();
            dbg64_line_end64();
        }

        // ---- 端口摘要（报告页用）：所有实现端口都登记一条 ----
        if (g_ctrl.list_count < 32) {
            Ahci64PortInfo& li = g_ctrl.list[g_ctrl.list_count++];
            li.port = (uint8_t)p; li.det = det; li.ipm = ipm; li.kind = kind; li.sig = sig;
            li.model[0] = 0; li.sectors = 0;
            if (slot >= 0) {
                for (int k = 0; k < 41; k++) li.model[k] = g_devs[slot].model[k];
                li.sectors = g_devs[slot].sectors;
                li.kind = g_devs[slot].kind;
            }
        }

        // ---- port= 行（格式固定：自动验收按这行 grep）----
        dbg64_line_begin64();
        dbg64_str("[AHCI64] port=");  dbg64_dec(p);
        dbg64_str(" det=");           dbg64_dec(det);
        dbg64_str(" sig=0x");         dbg64_hex64(sig);
        dbg64_str(" kind=");
        dbg64_str((slot >= 0 && g_devs[slot].kind == AHCI64_KIND_ATA) ? "ata"
                  : ((slot >= 0 && g_devs[slot].kind == AHCI64_KIND_ATAPI) ? "atapi" : "none"));
        dbg64_nl();
        dbg64_line_end64();

        if (slot < 0) continue;
        Ahci64Dev& d = g_devs[slot];
        if (d.kind == AHCI64_KIND_NONE) {           // 设备在位但认不出来：如实登记，不给驱动器号
            d.used = 1;
            g_dev_count++;
            continue;
        }
        d.used = 1;
        if (d.kind == AHCI64_KIND_ATA) {
            d.drive_no = ATA64_AHCI_BASE + g_drive_count;   // 统一驱动器号（AHCI 第 i 块盘 = 8+i）
            g_drive_count++;
        }
        g_dev_count++;

        dbg64_line_begin64();
        if (d.kind == AHCI64_KIND_ATA) {
            dbg64_str("[AHCI64] drive ");
            dbg64_dec((uint64_t)d.drive_no);
            dbg64_str(" model=");    dbg64_str(d.model);
            dbg64_str(" sectors=");  dbg64_dec(d.sectors);
        } else {
            dbg64_str("[AHCI64] atapi port=");
            dbg64_dec(p);
            dbg64_str(" model=");    dbg64_str(d.model);
            dbg64_str(" (只识别，读盘仍走 PATA 的 ATAPI 路径)");
        }
        dbg64_nl();
        dbg64_line_end64();
    }

    g_ctrl.found = 1;

    // hwinfo64 的磁盘表（HW64_DISK_MAX = 8）：0..3 留给 PATA（ata64.cpp 填），
    // 这里把前 4 块 AHCI 盘填到 4..7；超出部分只在报告页/AHCI 打点里出现（不覆盖别人的槽）。
    for (int i = 0; i < g_drive_count && i < 4; i++) {
        for (int k = 0; k < g_dev_count; k++) {
            if (g_devs[k].kind == AHCI64_KIND_ATA && g_devs[k].drive_no == ATA64_AHCI_BASE + i) {
                hwinfo_set_disk64(4 + i, g_devs[k].model, g_devs[k].sectors, 1);
                break;
            }
        }
    }
}

// ==================== 对外 API ====================
int ahci64_count64() { return g_drive_count; }

const Ahci64CtrlInfo* ahci64_ctrl64() { return &g_ctrl; }

// idx（AHCI 盘序号）-> g_devs[] 下标；驱动器号 = ATA64_AHCI_BASE + idx（见 ata64.h 的说明）
static int ahci_drive_index(int idx) {
    const int want = ATA64_AHCI_BASE + idx;
    for (int i = 0; i < g_dev_count; i++)
        if (g_devs[i].kind == AHCI64_KIND_ATA && g_devs[i].drive_no == want) return i;
    return -1;
}

bool ahci64_info64(int idx, DiskInfo* out) {
    out->present = false;
    out->atapi = false;
    out->sectors = 0;
    out->model[0] = 0;
    const int di = ahci_drive_index(idx);
    if (di < 0) return false;
    const Ahci64Dev& d = g_devs[di];
    out->present = true;
    out->atapi = false;                       // 本驱动只给 ATA 盘驱动器号（ATAPI 只识别）
    for (int i = 0; i < 41; i++) out->model[i] = d.model[i];
    out->sectors = d.sectors;
    return true;
}

bool ahci64_read64(int idx, uint32_t lba, uint32_t count, void* buf) {
    const int di = ahci_drive_index(idx);
    if (di < 0 || !buf) return false;
    return ahci_xfer(di, ATA_CMD_READ_DMA_EXT, lba, count, buf, false);
}

bool ahci64_write64(int idx, uint32_t lba, uint32_t count, const void* buf) {
    const int di = ahci_drive_index(idx);
    if (di < 0 || !buf) return false;
    const bool ok = ahci_xfer(di, ATA_CMD_WRITE_DMA_EXT, lba, count, (void*)buf, true);
    if (ok && !g_first_write_logged) {
        g_first_write_logged = 1;              // 首次写盘留一行证据，之后不再打（避免刷屏）
        dbg64_line_begin64();
        dbg64_str("[AHCI64] write lba=");
        dbg64_dec(lba);
        dbg64_str(" count=");
        dbg64_dec(count);
        dbg64_str(" ok (first write since init)");
        dbg64_nl();
        dbg64_line_end64();
    }
    return ok;
}

bool ahci64_selftest64() {
    ahci64_init64();                           // 幂等：保证控制器摘要有效
    if (!g_ctrl.found) {
        dbg64_line_begin64();
        dbg64_str("[AHCI64] selftest skipped (no controller)");
        dbg64_nl();
        dbg64_line_end64();
        return true;                           // 没控制器不算失败（与 e1000/usb 的 skipped 口径一致）
    }
    if (g_drive_count == 0) {
        dbg64_line_begin64();
        dbg64_str("[AHCI64] selftest skipped (no drive)");
        dbg64_nl();
        dbg64_line_end64();
        return true;
    }

    int mask = 0;
    uint8_t* sec = g_dma[0];
    for (int i = 0; i < g_drive_count; i++) {
        const int di = ahci_drive_index(i);
        if (di < 0) { mask |= 8; continue; }
        const Ahci64Dev& d = g_devs[di];
        if (d.model[0] == 0 || d.sectors == 0) mask |= 32;
        if (!sec) { mask |= 16; continue; }
        // 只读：读 LBA 0，证明 DMA 通路真的能搬数据
        if (ahci_xfer(di, ATA_CMD_READ_DMA_EXT, 0, 1, sec, false)) {
            dbg64_line_begin64();
            dbg64_str("[AHCI64] read lba=0 count=1 ok");
            dbg64_nl();
            dbg64_line_end64();
        } else {
            mask |= 16;
        }
    }

    dbg64_line_begin64();
    if (mask == 0) {
        dbg64_str("[AHCI64] selftest PASS");
        dbg64_nl();
        dbg64_line_end64();
        return true;
    }
    dbg64_str("[AHCI64] selftest FAIL mask=");
    dbg64_dec((uint64_t)mask);
    dbg64_nl();
    dbg64_line_end64();
    return false;
}
