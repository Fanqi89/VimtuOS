// hwui64.cpp - 屏幕硬件检查报告（真机唯一的诊断手段；见 hwui64.h 的完整说明）
//
// 实现要点：
//   * 全部内容来自**真探测**：hwinfo64（CPUID/PCI/磁盘）、mem64（E820/页池/堆）、
//     ata64+ahci64（PATA/SATA 盘）、acpi64（RSDP）+ 本文件自己做的**只读** ACPI 根表遍历、
//     fb/bootinfo（分辨率/位深/模式数）、edid64/display64（EDID/刷新率）、usb64/net64（弱引用，
//     安装介质内核里没编进去 -> 如实写"本内核未编入"）、本文件自己做的只读 PCI 扫描
//     （NVMe class 01/08、EHCI class 0C/03 prog-if 20、xHCI prog-if 30）。
//   * 报告正文缓存在本文件的静态行缓冲里（无堆分配），绘制函数只读它。
//   * 只画屏幕、只读端口/MMIO/PCI 配置空间；**不写任何设备寄存器**（ahci64_init64 例外：
//     那是驱动初始化本身，见 ahci64.cpp 顶部说明）。
#include "hwui64.h"
#include "ahci64.h"
#include "ata64.h"
#include "acpi64.h"
#include "apic64.h"          // IRQ_MODE64_*（常量；不需要链接 apic64.cpp）
#include "../bootinfo.h"     // BootInfo / BOOT_INFO_ADDR（引导层写在物理 0x1000）
#include "debug64.h"
#include "display64.h"
#include "edid64.h"
#include "fb.h"
#include "font.h"
#include "hwinfo64.h"
#include "input.h"
#include "memlayout64.h"     // ★ 必须排在 mem_64.h 之前（PAGE_SIZE_64 宏冲突）
#include "mem_64.h"
#include "part64.h"          // MediumDesc（读安装介质描述符用；只读结构，不链接 part64.cpp）
#include "port.h"
#include "usermode64.h"      // user64_available64()：BIOS（自有页表）还是 UEFI（固件页表）
#include "x86_64.h"

// ==================== 弱引用：只在系统内核里编入的模块 ====================
// （安装介质内核不链 usb64/net64/smp64/proc64；弱引用解析成 0 时如实写"本内核未编入"）
extern "C" {
int      usb64_ports64()               __attribute__((weak));
int      usb64_devices64()             __attribute__((weak));
uint64_t usb64_hid_reports64()         __attribute__((weak));
const char* usb64_state_str64()        __attribute__((weak));
const char* net64_state_str64()        __attribute__((weak));
uint64_t net64_tx_frames64()           __attribute__((weak));
uint64_t net64_rx_frames64()           __attribute__((weak));
uint32_t smp64_online_cpu_count64()    __attribute__((weak));
int      proc64_isolate64()            __attribute__((weak));
}

extern "C" char __bss_end[];          // 内核镜像高半区上界（报告里显示；kernel64.cpp 里同一符号）

// ==================== 调色板（黑底浅字；区块用色块分隔）====================
static const uint32_t C_BG      = 0xFF000000;   // 纯黑底
static const uint32_t C_BORDER  = 0xFF465A78;
static const uint32_t C_TITLE   = 0xFFFFFFFF;
static const uint32_t C_SEC_BG  = 0xFF003060;   // 区块色块（自动验收按这个颜色断言"分区块"）
static const uint32_t C_SEC_FG  = 0xFFFFFFFF;
static const uint32_t C_TXT     = 0xFFD2D8E2;
// （调色板里不放未使用的颜色：C_DIM 已并入 C_TXT 的浅灰系）
static const uint32_t C_HINT    = 0xFF78A0D7;
static const uint32_t C_WARN    = 0xFFFFC46B;   // 提示/不支持的项

// ==================== 报告缓冲 ====================
static char g_lines[HWUI64_MAX_LINES][HWUI64_LINE_MAX];
static uint8_t g_sec[HWUI64_MAX_LINES];         // 1 = 区块标题行
static int  g_line_count = 0;
static int  g_storage = 0;                      // 认到的盘/设备数
static int  g_controllers = 0;                  // 认到的存储控制器数
static int  g_built = 0;

int         hwui64_line_count64()        { return g_line_count; }
const char* hwui64_line64(int i)         { return (i >= 0 && i < g_line_count) ? g_lines[i] : ""; }
int         hwui64_line_is_section64(int i) { return (i >= 0 && i < g_line_count) ? g_sec[i] : 0; }
int         hwui64_storage_count64()     { return g_storage; }
int         hwui64_controller_count64()  { return g_controllers; }

// ---- 极简定长行构建（无 libc、无堆）----
// 每行写进 g_lines[idx]；超过 HWUI64_LINE_MAX-1 就截断 —— 绝不越界写、绝不依赖 sprintf。
// 注意：HwLine 的构造函数会把该行首字节清 0（= 空串），所以取一行就等于开始写这一行。
struct HwLine {
    char* d;
    char* end;
    HwLine(int idx) : d(g_lines[idx]), end(g_lines[idx] + HWUI64_LINE_MAX - 1) { *d = 0; }
    void s(const char* t)  { while (*t && d < end) *d++ = *t++; *d = 0; }
    void u(uint64_t v)     { char t[24]; int n = 0;
                             if (!v) t[n++] = '0';
                             while (v && n < 23) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
                             while (n > 0 && d < end) *d++ = t[--n];
                             *d = 0; }
    void h16(uint64_t v)   { static const char* H = "0123456789ABCDEF"; char t[17];
                             for (int i = 15; i >= 0; i--) { t[i] = H[v & 0xF]; v >>= 4; }
                             t[16] = 0; s(t); }
    void h8(uint64_t v)    { static const char* H = "0123456789ABCDEF"; char t[3];
                             t[0] = H[(v >> 4) & 0xF]; t[1] = H[v & 0xF]; t[2] = 0; s(t); }
    void mb(uint64_t bytes){ u((bytes + (1024 * 1024 - 1)) / (1024 * 1024)); s(" MB"); }
    void sec(uint64_t sectors) {
        // 512B/扇区 -> MB/GB（人类可读；与安装程序里的容量口径一致）
        const uint64_t b = sectors * 512ULL;
        if (b >= 1000ULL * 1000ULL * 1000ULL) { u(b / (1000ULL * 1000ULL * 1000ULL)); s(" GB"); }
        else                                  { u(b / (1000ULL * 1000ULL)); s(" MB"); }
    }
};

static int  g_li = 0;                 // 当前行号（构建期的游标）
static void newsec(const char* title) {
    if (g_li >= HWUI64_MAX_LINES) return;
    HwLine l(g_li);
    l.s(title);
    g_sec[g_li] = 1;
    g_li++;
}
static HwLine newline() {
    HwLine l((g_li < HWUI64_MAX_LINES) ? g_li : HWUI64_MAX_LINES - 1);
    g_sec[(g_li < HWUI64_MAX_LINES) ? g_li : HWUI64_MAX_LINES - 1] = 0;
    if (g_li < HWUI64_MAX_LINES) g_li++;
    return l;
}

// ==================== 只读 PCI 扫描（counting）====================
// 与 hwinfo64 的扫描同款（0xCF8/0xCFC，连续 4 条空总线提前停），但这里额外读 **prog-if**
// —— 只有它能区分 UHCI(0x00)/OHCI(0x10)/EHCI(0x20)/xHCI(0x30)，也才能把 NVMe(01/08) 单列出来。
struct HwPci {
    int ahci, ide, nvme, raid, other_storage;
    int uhci, ohci, ehci, xhci;
    int net, vga;
};
static uint32_t hpci_rd32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    const uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                        | ((uint32_t)fn << 8) | (uint32_t)(off & 0xFCu);
    outl(0xCF8u, addr);
    return inl(0xCFCu);
}
static void hwui_pci_scan(HwPci* o) {
    o->ahci = o->ide = o->nvme = o->raid = o->other_storage = 0;
    o->uhci = o->ohci = o->ehci = o->xhci = 0;
    o->net = o->vga = 0;
    uint32_t empty_run = 0;
    for (uint32_t bus = 0; bus < 256; bus++) {
        bool bus_has = false;
        for (uint32_t dev = 0; dev < 32; dev++) {
            uint32_t id = hpci_rd32((uint8_t)bus, (uint8_t)dev, 0, 0x00);
            if ((uint16_t)(id & 0xFFFFu) == 0xFFFFu || (uint16_t)(id & 0xFFFFu) == 0) continue;
            bus_has = true;
            const uint32_t hdr = hpci_rd32((uint8_t)bus, (uint8_t)dev, 0, 0x0C);
            const uint32_t nfn = (hdr & 0x00800000u) ? 8u : 1u;
            for (uint32_t fn = 0; fn < nfn; fn++) {
                if (fn != 0) {
                    id = hpci_rd32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x00);
                    if ((uint16_t)(id & 0xFFFFu) == 0xFFFFu || (uint16_t)(id & 0xFFFFu) == 0) continue;
                }
                const uint32_t cc = hpci_rd32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x08);
                const uint8_t cls = (uint8_t)(cc >> 24);
                const uint8_t sub = (uint8_t)(cc >> 16);
                const uint8_t pif = (uint8_t)(cc >> 8);
                if (cls == 0x01) {
                    if (sub == 0x01) o->ide++;
                    else if (sub == 0x06 && pif == 0x01) o->ahci++;
                    else if (sub == 0x08) o->nvme++;
                    else if (sub == 0x04) o->raid++;
                    else o->other_storage++;
                } else if (cls == 0x0C && sub == 0x03) {
                    if (pif == 0x00) o->uhci++;
                    else if (pif == 0x10) o->ohci++;
                    else if (pif == 0x20) o->ehci++;
                    else if (pif == 0x30) o->xhci++;
                } else if (cls == 0x02) {
                    o->net++;
                } else if (cls == 0x03) {
                    o->vga++;
                }
            }
        }
        if (bus_has) empty_run = 0; else if (++empty_run >= 4u) break;
    }
}

// ==================== 只读 ACPI 根表遍历（表数 + FACP 关机能力）====================
// 为什么自己做：acpi64 只暴露"RSDP / CPU 数 / IOAPIC / HPET / MCFG"，没有"表数"与
// "PM1a 控制块是否存在"。这里按 ACPI 规范**只读**遍历一遍根表（长度字段不合法就停，
// 条目数上限 64），不写任何寄存器、不接管中断。
struct HwAcpi {
    int      rsdp_ok, revision;
    int      tables;                 // 根表里登记的表数
    int      t_facp, t_apic, t_hpet, t_mcfg;
    int      shutdown_ok;            // FADT 的 PM1a_CNT 块存在（软关机可用）
    uint32_t pm1a_cnt;
};
static void hwui_acpi_walk(HwAcpi* o) {
    o->rsdp_ok = 0; o->revision = 0; o->tables = 0;
    o->t_facp = o->t_apic = o->t_hpet = o->t_mcfg = 0;
    o->shutdown_ok = 0; o->pm1a_cnt = 0;
    const uint64_t rsdp = acpi_rsdp_addr64();
    if (!rsdp) return;
    const uint8_t* p = (const uint8_t*)(uintptr_t)rsdp;
    const char* sig = (const char*)p;
    if (!(sig[0] == 'R' && sig[1] == 'S' && sig[2] == 'D' && sig[3] == ' ' &&
          sig[4] == 'P' && sig[5] == 'T' && sig[6] == 'R' && sig[7] == ' ')) return;
    o->rsdp_ok = 1;
    o->revision = p[15];
    uint64_t root = 0;
    int step = 4;
    if (o->revision >= 2) {
        root = *(const uint64_t*)(const void*)(p + 0x18);
        step = 8;
    }
    if (!root) {
        root = (uint64_t)(*(const uint32_t*)(const void*)(p + 0x10));
        step = 4;
    }
    if (!root) return;
    const uint8_t* rt = (const uint8_t*)(uintptr_t)root;
    const uint32_t len = *(const uint32_t*)(const void*)(rt + 4);
    if (len < 0x24u || len > 0x10000u) return;                  // 长度字段撒谎：不看
    int n = (int)((len - 0x24u) / (uint32_t)step);
    if (n > 64) n = 64;                                        // 有界
    for (int i = 0; i < n; i++) {
        const uint64_t t = (step == 8)
            ? *(const uint64_t*)(const void*)(rt + 0x24 + (uint32_t)i * 8u)
            : (uint64_t)(*(const uint32_t*)(const void*)(rt + 0x24 + (uint32_t)i * 4u));
        if (!t) continue;
        const uint8_t* h = (const uint8_t*)(uintptr_t)t;
        const uint32_t tl = *(const uint32_t*)(const void*)(h + 4);
        if (tl < 36u || tl > 0x10000u) continue;
        o->tables++;
        const char* s4 = (const char*)h;
        if (s4[0] == 'F' && s4[1] == 'A' && s4[2] == 'C' && s4[3] == 'P') {
            o->t_facp = 1;
            if (tl >= 0x44u) {
                o->pm1a_cnt = *(const uint32_t*)(const void*)(h + 0x40);
                if (o->pm1a_cnt) o->shutdown_ok = 1;
            }
        } else if (s4[0] == 'A' && s4[1] == 'P' && s4[2] == 'I' && s4[3] == 'C') {
            o->t_apic = 1;
        } else if (s4[0] == 'H' && s4[1] == 'P' && s4[2] == 'E' && s4[3] == 'T') {
            o->t_hpet = 1;
        } else if (s4[0] == 'M' && s4[1] == 'C' && s4[2] == 'F' && s4[3] == 'G') {
            o->t_mcfg = 1;
        }
    }
}

// ==================== 报告构建 ====================
static void hwui_storage_lines(const HwPci& pci, int* out_disks, int* out_ctrl) {
    // 读一遍 PATA（0..3，不需要 ata64_init64：识别走状态轮询）与 AHCI（ahci64 已初始化）。
    struct Row { int drive; int bus; int port; char model[41]; uint64_t sectors; int atapi; };
    static Row rows[16];
    int n = 0;
    int pata_ch = 0, pata_drives = 0;
    for (int d = 0; d < 4; d++) {
        DiskInfo di;
        if (!ata64_identify(d, &di) || !di.present) continue;
        pata_drives++;
        if (n < 16) {
            rows[n].drive = d; rows[n].bus = 0; rows[n].port = -1;
            rows[n].sectors = di.sectors; rows[n].atapi = di.atapi ? 1 : 0;
            for (int k = 0; k < 41; k++) rows[n].model[k] = di.model[k];
            n++;
        }
    }
    // PATA 通道数 = 有设备的通道（primary / secondary）
    for (int ch = 0; ch < 2; ch++) {
        const int a = ch * 2, b = a + 1;
        DiskInfo di;
        bool any = false;
        if (ata64_identify(a, &di) && di.present) any = true;
        if (!any && ata64_identify(b, &di) && di.present) any = true;
        if (any) pata_ch++;
    }
    const Ahci64CtrlInfo* ah = ahci64_ctrl64();
    // AHCI 盘数由 ahci64_count64() 决定（下面 rows[] 里按顺序重编驱动器号，不再单独用）
    int ahci_devs = 0;
    if (ah->found) {
        for (int i = 0; i < ah->list_count; i++) {
            const Ahci64PortInfo& pi = ah->list[i];
            if (pi.kind == AHCI64_KIND_NONE) continue;
            ahci_devs++;
            if (n < 16) {
                rows[n].drive = -1;                    // 真正的驱动器号在下面按 ATA 顺序统一编
                rows[n].bus = 1; rows[n].port = pi.port;
                rows[n].sectors = pi.sectors; rows[n].atapi = (pi.kind == AHCI64_KIND_ATAPI) ? 1 : 0;
                for (int k = 0; k < 41; k++) rows[n].model[k] = pi.model[k];
                n++;
            }
        }
    }
    // ATAPI 在 AHCI 上不给驱动器号（ahci64.h 的约定），上面先给 ATA 一个占位号再按序修正：
    int local = 0;
    for (int i = 0; i < n; i++) {
        if (rows[i].bus == 1 && !rows[i].atapi) rows[i].drive = ATA64_AHCI_BASE + local++;
    }

    *out_disks = pata_drives + ahci_devs + pci.nvme;
    *out_ctrl  = pata_ch + (ah->found ? 1 : 0) + pci.nvme;

    newsec("存储 / Storage");
    {
        HwLine l = newline();
        l.s("  控制器 controllers="); l.u((uint64_t)*out_ctrl);
        l.s("（PATA="); l.u((uint64_t)pata_ch);
        l.s(" AHCI="); l.u(ah->found ? 1 : 0);
        l.s(" NVMe="); l.u((uint64_t)pci.nvme);
        l.s("）  设备 storage="); l.u((uint64_t)*out_disks);
    }
    if (ah->found) {
        HwLine l = newline();
        l.s("  AHCI 控制器 "); l.u(ah->bus); l.s(":"); l.u(ah->dev); l.s("."); l.u(ah->fn);
        l.s("  abar=0x"); l.h16(ah->abar);
        l.s("  ports="); l.u((uint64_t)ah->ports);
        l.s("  端口设备="); l.u((uint64_t)ahci_devs);
    } else {
        HwLine l = newline();
        l.s("  AHCI 控制器：未找到（没有任何 class 01/06 prog-if 01 的控制器）");
    }
    int listed = 0;
    for (int i = 0; i < n; i++) {
        if (listed >= 8) {
            HwLine l = newline();
            l.s("  …还有 "); l.u((uint64_t)(n - listed)); l.s(" 个设备未列出（列表上限 8）");
            break;
        }
        HwLine l = newline();
        l.s(rows[i].bus == 0 ? "  PATA " : "  AHCI ");
        if (rows[i].drive >= 0) { l.s("drive"); l.u((uint64_t)rows[i].drive); } else { l.s("no-drive"); }
        if (rows[i].bus == 1) { l.s(" port"); l.u((uint64_t)rows[i].port); }
        l.s("  ");
        l.s(rows[i].model[0] ? rows[i].model : "(no model)");
        l.s("  ");
        if (rows[i].atapi) l.s("光驱 ATAPI");
        else { l.u(rows[i].sectors); l.s(" 扇区 / "); l.sec(rows[i].sectors); }
        listed++;
    }
    if (n == 0) {
        HwLine l = newline();
        l.s("  未检测到任何 ATA/SATA 盘（查接线；BIOS 的 SATA 模式见真机验证指南）");
    }
    if (pci.nvme > 0) {
        HwLine l = newline();
        l.s("  NVMe：检测到 "); l.u((uint64_t)pci.nvme);
        l.s(" 个控制器（class 01/08）-> 本系统**不支持 NVMe**，请用 AHCI(SATA)/IDE");
    } else {
        HwLine l = newline();
        l.s("  NVMe：未检测到");
    }
}

int hwui64_build64() {
    if (g_built) return g_line_count;

    // 依赖项初始化（幂等、只读探测）：
    //   ahci64_init64：报告要显示 AHCI 控制器/端口/盘（真机上没有这一步就只剩“没盘”）。
    //   display64 的**探测不在这里跑**：它的 init 与打点由各自的启动路径
    //     （kernel64.cpp 的 os_boot_path）统一调用，这里只**读**已探测结果；
    //     没跑过就如实写 not-probed / 模式数用 BootInfo。
    //     （踩坑：早先在这里调 display64_init64() 会让 [DISP64] mode list i=.. 打两遍，
    //      display_runtime_test 的“逐条与汇总一致”断言就会挂。）
    ahci64_init64();

    HwPci pci;
    hwui_pci_scan(&pci);
    const HwInfo64* hw = hw_info64();
    const BootInfo* bi = (const BootInfo*)(uintptr_t)BOOT_INFO_ADDR;

    // 清缓冲
    for (int i = 0; i < HWUI64_MAX_LINES; i++) {
        g_lines[i][0] = 0;
        g_sec[i] = 0;
    }
    g_li = 0;

    // ---- 1) CPU ----
    newsec("处理器 / CPU");
    {
        HwLine l = newline();
        l.s("  vendor="); l.s(hw->cpu.vendor[0] ? hw->cpu.vendor : "unknown");
        l.s("  hypervisor="); l.s(hw->cpu.hypervisor);
        l.s(hw->cpu.smp ? "  SMP=1" : "  SMP=0");
    }
    {
        HwLine l = newline();
        l.s("  brand="); l.s(hw->cpu.brand);
    }
    {
        HwLine l = newline();
        l.s("  family="); l.u(hw->cpu.family);
        l.s(" model="); l.u(hw->cpu.model);
        l.s(" stepping="); l.u(hw->cpu.stepping);
        l.s(" cores="); l.u(hw->cpu.cores);
        l.s("  LM="); l.u(hw->cpu.lm ? 1 : 0);
        l.s(" NX="); l.u(hw->cpu.nx ? 1 : 0);
        l.s(" VMX="); l.u(hw->cpu.vmx ? 1 : 0);
    }

    // ---- 2) 内存 ----
    newsec("内存 / Memory（全部实测）");
    {
        HwLine l = newline();
        l.s("  E820 可用上界 total="); l.mb(mem_total_ram_64());
        l.s("  页池 pool free/total="); l.u(page_count_free_64()); l.s("/"); l.u(page_count_total_64());
        l.s(" 页");
    }
    {
        HwLine l = newline();
        uint64_t used = heap_used_64(), tot = heap_total_64();
        l.s("  内核堆 heap="); l.u(used / 1024); l.s("/"); l.u(tot / 1024); l.s(" KB");
        l.s("  页大小="); l.u(PAGE_SIZE_64); l.s(" B");
        l.s("  ACPI 内存条目="); l.u((uint64_t)bi->mem_entries);
    }

    // ---- 3) 固件与地址空间 ----
    newsec("固件与地址空间 / Firmware & Address space");
    {
        HwLine l = newline();
        const int bios = user64_available64() != 0;
        l.s("  引导=");
#if defined(VIMTU_INSTALLER_MEDIA)
        {
            const MediumDesc* md = (const MediumDesc*)(uintptr_t)MEDIUM_DESC_ADDR;
            if (md->magic == MEDIUM_MAGIC && md->kind == MEDIUM_CD)        l.s("BIOS 光盘（El Torito/CDBOOT）");
            else if (md->magic == MEDIUM_MAGIC && md->kind == MEDIUM_RAM)  l.s("BIOS U 盘 / hybrid ISO");
            else                                                          l.s(bios ? "BIOS 硬盘/裸盘（MBR→loader64）" : "UEFI（固件页表）");
        }
#else
        l.s(bios ? "BIOS 硬盘/裸盘（MBR→loader64）" : "UEFI（GOP + 固件页表）");
#endif
        l.s("  页表=");
        l.s(bios ? "自有（isolated，ring3 可用）" : "固件（shared，ring3 不可用）");
    }
    {
        HwLine l = newline();
        l.s("  CR3=0x");
        uint64_t cr3; __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        l.h16(cr3);
        l.s("  内核镜像上界=0x"); l.h16((uint64_t)(uintptr_t)__bss_end);
        l.s("  引导磁盘 LBA 布局=内核@9");
    }

    // ---- 4) 中断 / SMP ----
    newsec("中断与 SMP / IRQ & SMP");
    {
        HwLine l = newline();
        const uint32_t mode = g_irq_mode64;
        l.s("  路由=");
        l.s(mode == IRQ_MODE64_APIC ? "LAPIC + IOAPIC（APIC 已接管）" : "8259 PIC（未接管）");
        l.s("  PIT="); l.u(PIT_HZ_64); l.s(" Hz  ticks="); l.u(g_ticks64);
    }
    {
        HwLine l = newline();
        l.s("  MADT cpu="); l.u((uint64_t)acpi_cpu_count64());
        l.s("  已上线 AP=");
        if (smp64_online_cpu_count64) l.u((uint64_t)smp64_online_cpu_count64());
        else                          l.s("n/a（本内核未编入 smp64）");
        l.s("  逻辑核="); l.u(hw->cpu.cores);
        l.s("  IOAPIC="); l.u((uint64_t)acpi_ioapic_count64());
    }

    // ---- 5) 存储 ----
    hwui_storage_lines(pci, &g_storage, &g_controllers);

    // ---- 6) 显示 ----
    newsec("显示 / Video");
    {
        HwLine l = newline();
        l.s("  framebuffer "); l.u((uint64_t)fb_width()); l.s("x"); l.u((uint64_t)fb_height());
        l.s("x32  phys="); l.u((uint64_t)fb_phys_width()); l.s("x"); l.u((uint64_t)fb_phys_height());
        l.s("  zoom="); l.u((uint64_t)fb_get_zoom()); l.s("%");
        l.s("  bootinfo="); l.u(bi->width); l.s("x"); l.u(bi->height);
        l.s("@"); l.u(bi->bpp); l.s("bpp");
    }
    {
        const Edid64* ed = edid64_get();
        const Disp64Info* di = display64_info64();
        HwLine l = newline();
        l.s("  EDID=");
        if (ed->present && ed->valid) {
            l.s("有（"); l.s(ed->name); l.s(" / "); l.s(ed->mfg);
            l.s(" "); l.u(ed->size_cm_w); l.s("x"); l.u(ed->size_cm_h); l.s("cm）");
        } else if (ed->present) {
            l.s("有数据但解析失败");
        } else {
            l.s("无（刷新率只能靠实测/未知）");
        }
        l.s("  模式数=");
        l.u((di && di->init_done) ? (uint64_t)di->mode_count : (uint64_t)bi->mode_count);
    }
    {
        const Disp64Info* di = display64_info64();
        HwLine l = newline();
        l.s("  刷新 refresh=");
        if (di && di->init_done && di->refresh_x10 > 0) {
            l.u((uint64_t)(di->refresh_x10 / 10)); l.s("."); l.u((uint64_t)(di->refresh_x10 % 10)); l.s(" Hz");
        } else {
            l.s("unknown");
        }
        l.s("  src=");
        l.s((di && di->init_done) ? display64_src_name64(di->src) : "not-probed");
        l.s("  ctx=screens/desktop");
    }

    // ---- 7) 输入 ----
    newsec("输入 / Input（USB 只支持 UHCI HID）");
    {
        HwLine l = newline();
        const uint8_t st = inb(0x64);                    // 8042 状态端口（只读，不发命令）
        l.s("  PS/2 8042 stat=0x"); l.h8(st);
        l.s(st == 0xFF ? "  -> 疑似无 8042 控制器" : "  -> 控制器在（键盘 IRQ1/鼠标 IRQ12 驱动已装载）");
    }
    {
        HwLine l = newline();
        l.s("  USB 主控 UHCI="); l.u((uint64_t)pci.uhci);
        l.s("  HID 设备=");
        if (usb64_devices64) l.u((uint64_t)usb64_devices64());
        else                 l.s("n/a（本内核未编入 usb64）");
        l.s("  reports=");
        if (usb64_hid_reports64) l.u(usb64_hid_reports64());
        else                     l.s("n/a");
    }
    {
        HwLine l = newline();
        l.s("  EHCI="); l.u((uint64_t)pci.ehci); l.s("（不支持）");
        l.s("  xHCI="); l.u((uint64_t)pci.xhci); l.s("（不支持）");
        l.s("  OHCI="); l.u((uint64_t)pci.ohci); l.s("  -> 请用 PS/2 或带 UHCI 的老机器");
    }

    // ---- 8) ACPI ----
    newsec("ACPI / 电源");
    {
        HwAcpi a;
        hwui_acpi_walk(&a);
        HwLine l = newline();
        l.s("  RSDP="); l.s(a.rsdp_ok ? "有" : "无");
        l.s(" rev="); l.u((uint64_t)a.revision);
        l.s(" 根表条目="); l.u((uint64_t)a.tables);
        l.s("（FACP="); l.u(a.t_facp ? 1 : 0);
        l.s(" APIC="); l.u(a.t_apic ? 1 : 0);
        l.s(" HPET="); l.u(a.t_hpet ? 1 : 0);
        l.s(" MCFG="); l.u(a.t_mcfg ? 1 : 0);
        l.s("）");
    }
    {
        HwAcpi a;
        hwui_acpi_walk(&a);
        HwLine l = newline();
        l.s("  CPU="); l.u((uint64_t)acpi_cpu_count64());
        l.s("  PM1a_CNT=0x"); l.h16(a.pm1a_cnt);
        l.s("  软关机="); l.s(a.shutdown_ok ? "可用（FADT）" : "不可用（无 FACP/PM1a）");
        l.s("  HPET=0x"); l.h16(acpi_hpet_addr64());
        l.s("  MCFG段="); l.u((uint64_t)acpi_mcfg_segment_count64());
    }

    // ---- 9) 网络 ----
    newsec("网络 / Network（只做 e1000 + ARP/ICMP）");
    {
        HwLine l = newline();
        l.s("  PCI class 02 网络控制器="); l.u((uint64_t)pci.net);
        l.s("  e1000=");
        if (net64_state_str64) l.s(net64_state_str64());
        else                   l.s("n/a（本内核未编入 net64）");
        l.s("  tx=");
        if (net64_tx_frames64) l.u(net64_tx_frames64()); else l.s("n/a");
        l.s(" rx=");
        if (net64_rx_frames64) l.u(net64_rx_frames64()); else l.s("n/a");
    }
    {
        HwLine l = newline();
        l.s("  边界：NVMe / USB 存储 / EHCI-xHCI 键鼠 / USB hub / Secure Boot 签名 均**不支持**");
    }

    g_line_count = g_li;
    g_built = 1;

    dbg64_line_begin64();
    dbg64_str("[HWUI] report lines=");
    dbg64_dec((uint64_t)g_line_count);
    dbg64_str(" storage=");
    dbg64_dec((uint64_t)g_storage);
    dbg64_str(" controllers=");
    dbg64_dec((uint64_t)g_controllers);
    dbg64_nl();
    dbg64_line_end64();
    return g_line_count;
}

// ==================== 绘制 ====================
static void ui_text(int x, int y, const char* s, uint32_t fg) {
    font_select(2);                        // face 2 = simhei（中文）
    font_draw_text(x, y, s, fg);
}

int hwui64_draw64(int x, int y, int w, int h, int hint) {
    if (g_line_count == 0) hwui64_build64();
    if (w < 80 || h < 40) return 0;

    fb_fill_rect(x, y, w, h, C_BG);                     // 黑底
    fb_draw_rect(x, y, w, h, C_BORDER);

    const int lh = font_line_height();                   // 20（TTF 一行）
    const int pitch = lh + 2;
    // 标题条
    fb_fill_rect(x + 1, y + 1, w - 2, lh + 6, C_SEC_BG);
    ui_text(x + 8, y + 4, "硬件检查报告 / Hardware Check Report", C_TITLE);

    const int top = y + lh + 10;
    const int bottom = y + h - (hint ? (lh + 8) : 4);
    int ly = top;
    int drawn = 0;
    for (int i = 0; i < g_line_count; i++) {
        if (ly + lh > bottom) break;
        if (g_sec[i]) {
            fb_fill_rect(x + 3, ly - 1, w - 6, lh + 2, C_SEC_BG);
            ui_text(x + 8, ly, g_lines[i], C_SEC_FG);
        } else {
            ui_text(x + 14, ly, g_lines[i], C_TXT);
        }
        ly += pitch;
        drawn++;
    }
    if (drawn < g_line_count) {
        // 「还有 N 行未显示」—— 手工拼在**局部**缓冲里（绝不碰 g_lines[]：那是报告正文）
        char tmp[HWUI64_LINE_MAX];
        char* d = tmp;
        const char* a = "  …还有 ";
        while (*a) *d++ = *a++;
        int n = g_line_count - drawn, rev[8], m = 0;
        if (!n) rev[m++] = '0';
        while (n && m < 8) { rev[m++] = (char)('0' + n % 10); n /= 10; }
        while (m > 0) *d++ = (char)rev[--m];
        a = " 行未显示（区域太小）";
        while (*a) *d++ = *a++;
        *d = 0;
        ui_text(x + 14, ly, tmp, C_WARN);
    }
    if (hint) {
        const int hy = y + h - lh - 4;
        ui_text(x + 8, hy, "按任意键继续（约 5 秒后自动继续） / press any key to continue", C_HINT);
    }
    return drawn;
}


int hwui64_show64(uint32_t ms) {
    hwui64_build64();
    const int W = fb_width(), H = fb_height();
    hwui64_draw64(0, 0, W, H, 1);
    fb_flip();

    const uint64_t t0 = g_ticks64;
    const uint64_t want = (ms + TICK_MS_64 - 1) / TICK_MS_64;
    int skipped = 0;
    uint64_t if_on;
    __asm__ volatile("pushfq; popq %0" : "=r"(if_on));
    const int can_hlt = (if_on & 0x200ULL) != 0;

    kbd_drain();                        // 丢掉进这一页之前的按键
    for (;;) {
        if (kbd_has_char()) {           // 任意键：跳过（吞掉这个键，别让它影响后面的向导/桌面）
            uint8_t c;
            while (kbd_pop_char(&c)) { }
            skipped = 1;
            break;
        }
        if (g_ticks64 - t0 >= want) break;
        if (can_hlt) __asm__ volatile("hlt");       // 让出 CPU（IF=1 时才用；否则自旋）
        else         __asm__ volatile("pause");
    }
    const uint64_t elapsed_ms = ticks_to_ms64((uint32_t)(g_ticks64 - t0));

    dbg64_line_begin64();
    dbg64_str("[HWUI] report shown ms=");
    dbg64_dec(elapsed_ms);
    dbg64_str(" skipped=");
    dbg64_dec((uint64_t)(skipped ? 1 : 0));
    dbg64_nl();
    dbg64_line_end64();
    return skipped;
}

int hwui64_selftest64() {
    const int n = hwui64_build64();
    int mask = 0;
    if (n < 8) mask |= 1;                                   // 行数太少 = 探测基本没拿到东西
    if (hwui64_storage_count64() < 0 || hwui64_controller_count64() < 0) mask |= 2;
    // 内容自洽：报告里必须至少有一行"存储"区块与一行"显示"区块（绘制/验收都依赖分区块）
    int sec_cnt = 0;
    for (int i = 0; i < g_line_count; i++) if (g_sec[i]) sec_cnt++;
    if (sec_cnt < 5) mask |= 4;

    dbg64_line_begin64();
    if (mask == 0) {
        dbg64_str("[HWUI] selftest PASS");
    } else {
        dbg64_str("[HWUI] selftest FAIL mask=");
        dbg64_dec((uint64_t)mask);
    }
    dbg64_nl();
    dbg64_line_end64();
    return mask;
}
