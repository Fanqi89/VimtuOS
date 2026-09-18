// hwinfo64.cpp - 硬件详情（CPU / PCI / 磁盘）探测与串口打点
//
// 为什么单独一个文件：
//   安装内核与系统内核都需要一份"这台机器是什么"的静态清单（CPU 能力、PCI 设备、
//   磁盘型号），给任务管理器/设置页面显示。这里保持最小依赖：只用 CPUID、0xCF8/0xCFC
//   配置端口和串口打点，不碰内核堆、不碰 ATA 驱动、不做任何同步 —— 启动早期（IDT 之前）
//   调用也安全。
//
// 三条硬性约束（踩坑后的约定）：
//   1) 只读探测：PCI 侧只往 0xCF8 写地址、从 0xCFC 读数据；**绝不写设备配置寄存器**，
//      连命令位都不开，避免枚举动作本身改变设备状态。
//   2) 优雅降级：某 CPUID 叶不存在 -> 对应字段空/"none"/0；没有 PCI 设备 -> 计数 0。
//      探测函数任何一条路径都必须返回，绝不 #UD/死循环/写坏内存。
//   3) 十六进制统一**小写**输出：自动验收的 grep 正则通常用 [0-9a-f] 字符类，而
//      dbg64_hex64() 打大写（且固定 16 位），所以本文件不用它，改用 hw64_hex_min()
//      与 hw64_hex_digits() 两个小写版本。日志格式（改前想清楚，验收按行 grep）：
//        [HW64] cpu vendor=<vendor> brand=<brand> cores=<n> leafmax=0x<hex> hyp=<hypervisor|none>
//        [HW64] features=SMP:x NX:x PAE:x SSE2:x AVX:x VMX:x SMEP:x LM:x
//        [HW64] pci devices=<n> ide=<n> net=<n> vga=<n>
//        [HW64] pci <bus>:<dev>.<fn> <vendor>:<device> class=0x<hh> rev=0x<hh>
//        [HW64] selftest PASS  或  [HW64] selftest FAIL mask=<n>
//
// 磁盘信息：本模块不读盘。part64/ata64 侧拿到 IDENTIFY 结果后调用 hwinfo_set_disk64()
// 填表（ATA 驱动不在桌面内核里，也不能在这里硬连）。

#include "hwinfo64.h"
#include "debug64.h"
#include "port.h"

// ==================== 内部状态 ====================
// 静态存储（.bss，零初始化）：POD、无静态构造/析构，也不占内核堆。
static HwInfo64 g_info;

// 串口逐个列出前多少个 PCI 设备（登记上限 32；日志打 16 条足够覆盖主要设备）
static const uint32_t HW64_PCI_LOG_MAX = 16u;

// ==================== 小工具：定长字符串 / 十六进制输出 ====================
static uint32_t hw64_strlen(const char* s) {
    uint32_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void hw64_memzero(void* p, uint32_t n) {
    uint8_t* d = (uint8_t*)p;
    for (uint32_t i = 0; i < n; i++) d[i] = 0;
}

// 定长、ASCII 安全的字符串拷贝（CPUID 字符串两侧全是空格填充）：
//   * 跳前导空格与 NUL；中途遇到 NUL 视为结束（右填充）
//   * 控制字符折叠成 '?'，>=0x80 原样保留（不切断 UTF-8 序列）
//   * 截断到 cap-1，永远 NUL 结尾，最后去尾随空格并清零剩余字节
static void hw64_store_ascii(char* dst, uint32_t cap, const char* src, uint32_t len) {
    if (!dst || !src || cap == 0u) return;
    uint32_t i = 0;
    while (i < len && (src[i] == ' ' || src[i] == 0)) i++;
    uint32_t n = 0;
    for (; i < len && n + 1u < cap; i++) {
        const uint8_t c = (uint8_t)src[i];
        if (c == 0) break;
        dst[n++] = (c >= 0x20u && c != 0x7Fu) ? (char)c : '?';
    }
    while (n > 0u && dst[n - 1u] == ' ') n--;
    dst[n] = 0;
    for (uint32_t k = n + 1u; k < cap; k++) dst[k] = 0;
}

static const char HW64_HEXL[] = "0123456789abcdef";   // 统一小写，见文件头约束 3

// 固定宽度小写十六进制（不带 "0x" 前缀）
static void hw64_hex_digits(uint64_t v, uint32_t digits) {
    for (int i = (int)digits - 1; i >= 0; i--)
        dbg64_putc(HW64_HEXL[(v >> (uint32_t)(i * 4)) & 0xFu]);
}

// 最简小写十六进制（无前导 0；0 打 "0"）
static void hw64_hex_min(uint64_t v) {
    if (v == 0) { dbg64_putc('0'); return; }
    char buf[16];
    uint32_t n = 0;
    while (v != 0 && n < 16u) { buf[n++] = HW64_HEXL[v & 0xFu]; v >>= 4; }
    while (n > 0u) dbg64_putc(buf[--n]);
}

// 特征日志片段：NAME:1 / NAME:0（分隔空格由调用方打，保证行尾没有多余空格）
static void hw64_feat_token(const char* name, bool on) {
    dbg64_str(name);
    dbg64_putc(':');
    dbg64_putc(on ? '1' : '0');
}

// ==================== CPUID ====================
// CPUID 可用性：EFLAGS.ID（bit21）能否翻转。x86_64 上恒为真，但这步没有任何副作用
// （原 flags 原样恢复），保留它满足"探测失败必须优雅降级"的硬性要求。
static bool hw64_cpuid_supported() {
    uint64_t before = 0, after = 0;
    __asm__ volatile(
        "pushfq\n\t"
        "popq %0\n\t"
        "movq %0, %%rax\n\t"
        "xorq $0x200000, %%rax\n\t"     // 翻转 ID 位
        "pushq %%rax\n\t"
        "popfq\n\t"
        "pushfq\n\t"
        "popq %1\n\t"
        "pushq %0\n\t"                  // 恢复原 flags
        "popfq\n\t"
        : "=&r"(before), "=&r"(after)
        :
        : "rax", "cc");
    return ((before ^ after) & 0x200000ull) != 0;
}

// 统一 CPUID 包装：四个返回寄存器都取出来（不需要的直接传 nullptr）
static void hw64_cpuid(uint32_t leaf, uint32_t sub,
                       uint32_t* ra, uint32_t* rb, uint32_t* rc, uint32_t* rd) {
    uint32_t a = 0, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(leaf), "c"(sub));
    if (ra) *ra = a;
    if (rb) *rb = b;
    if (rc) *rc = c;
    if (rd) *rd = d;
}

// 从 CPUID 返回的 4 个字（a,b,c,d）拼 16 字节字符串
static void hw64_regs_to_chars(const uint32_t r[4], char* out) {
    for (uint32_t j = 0; j < 4u; j++) {
        out[j * 4u + 0u] = (char)(r[j] & 0xFFu);
        out[j * 4u + 1u] = (char)((r[j] >> 8) & 0xFFu);
        out[j * 4u + 2u] = (char)((r[j] >> 16) & 0xFFu);
        out[j * 4u + 3u] = (char)((r[j] >> 24) & 0xFFu);
    }
}

static void hw64_cpu_detect(HwCpu64* cpu) {
    hw64_memzero(cpu, (uint32_t)sizeof(*cpu));
    // 默认降级值：任何一步失败都保持这套"如实但安全"的输出
    hw64_store_ascii(cpu->vendor, HW64_CPU_VENDOR_MAX, "none", 4u);
    hw64_store_ascii(cpu->brand, HW64_CPU_BRAND_MAX, "none", 4u);
    hw64_store_ascii(cpu->hypervisor, HW64_HYP_VENDOR_MAX, "none", 4u);
    cpu->cores = 1u;

    if (!hw64_cpuid_supported()) return;

    uint32_t a = 0, b = 0, c = 0, d = 0;
    hw64_cpuid(0u, 0u, &a, &b, &c, &d);
    cpu->leaf_max = a;
    if (a == 0u) return;                       // 连叶 0 都没有：保持降级值

    // 厂商串按 EBX, EDX, ECX 拼（"GenuineIntel" 的标准字节序）
    {
        char v[13];
        const uint32_t r[3] = { b, d, c };
        for (uint32_t i = 0; i < 3u; i++) {
            v[i * 4u + 0u] = (char)(r[i] & 0xFFu);
            v[i * 4u + 1u] = (char)((r[i] >> 8) & 0xFFu);
            v[i * 4u + 2u] = (char)((r[i] >> 16) & 0xFFu);
            v[i * 4u + 3u] = (char)((r[i] >> 24) & 0xFFu);
        }
        v[12] = 0;
        hw64_store_ascii(cpu->vendor, HW64_CPU_VENDOR_MAX, v, 12u);
    }

    // 扩展叶上限：品牌串 + NX/LM
    hw64_cpuid(0x80000000u, 0u, &a, &b, &c, &d);
    cpu->leaf_ext_max = a;
    if (a >= 0x80000004u) {                    // 0x80000002..4 三个叶才够 48 字符品牌串
        char brand[HW64_CPU_BRAND_MAX];
        for (uint32_t leaf = 0; leaf < 3u; leaf++) {
            uint32_t ea = 0, eb = 0, ec = 0, ed = 0;
            hw64_cpuid(0x80000002u + leaf, 0u, &ea, &eb, &ec, &ed);
            const uint32_t r[4] = { ea, eb, ec, ed };
            hw64_regs_to_chars(r, &brand[leaf * 16u]);
        }
        brand[48] = 0;
        hw64_store_ascii(cpu->brand, HW64_CPU_BRAND_MAX, brand, 48u);
        if (cpu->brand[0] == 0) hw64_store_ascii(cpu->brand, HW64_CPU_BRAND_MAX, "none", 4u);
    }
    if (a >= 0x80000001u) {
        hw64_cpuid(0x80000001u, 0u, &a, &b, &c, &d);
        cpu->regext1_edx = d;
        cpu->nx = (d & (1u << 20)) != 0;
        cpu->lm = (d & (1u << 29)) != 0;
    }

    // 叶 1：家族/型号/步进、逻辑核数、基础特征、虚拟机提示位
    if (cpu->leaf_max >= 1u) {
        hw64_cpuid(1u, 0u, &a, &b, &c, &d);
        cpu->stepping = a & 0xFu;
        const uint32_t base_model  = (a >> 4) & 0xFu;
        const uint32_t base_family = (a >> 8) & 0xFu;
        const uint32_t ext_model   = (a >> 16) & 0xFu;
        const uint32_t ext_family  = (a >> 20) & 0xFFu;
        cpu->family = (base_family == 0xFu) ? (base_family + ext_family) : base_family;
        cpu->model  = ((cpu->family == 0x6u) || (cpu->family == 0xFu))
                      ? (base_model + (ext_model << 4)) : base_model;
        cpu->cores = (b >> 16) & 0xFFu;
        if (cpu->cores == 0u) cpu->cores = 1u;         // 老 CPU 该字段为 0 -> 按单核算
        cpu->reg1_ecx = c;
        cpu->reg1_edx = d;
        cpu->sse  = (d & (1u << 25)) != 0;
        cpu->sse2 = (d & (1u << 26)) != 0;
        cpu->pae  = (d & (1u << 6)) != 0;
        cpu->smp  = (cpu->cores > 1u) || ((d & (1u << 28)) != 0);   // 多核或 HTT 提示位
        cpu->avx  = (c & (1u << 28)) != 0;
        cpu->vmx  = (c & (1u << 5)) != 0;

        // 虚拟机识别：只有 hypervisor present 位（ECX bit31）为 1 时叶 0x40000000 才有效。
        // 已知签名："VMwareVMware" / "KVMKVMKVM" / "Microsoft Hv" / "TCGTCGTCGTCG"（QEMU TCG）。
        if (((c & (1u << 31)) != 0) && cpu->leaf_max >= 0x40000000u) {
            uint32_t ha = 0, hb = 0, hc = 0, hd = 0;
            hw64_cpuid(0x40000000u, 0u, &ha, &hb, &hc, &hd);
            (void)ha;                                  // 这里的 EAX 是厂商串最大叶号，用不到
            char hv[13];
            const uint32_t r[4] = { hb, hc, hd, 0u };
            hw64_regs_to_chars(r, hv);                 // 只取前 12 字节
            hv[12] = 0;
            hw64_store_ascii(cpu->hypervisor, HW64_HYP_VENDOR_MAX, hv, 12u);
            if (cpu->hypervisor[0] == 0)
                hw64_store_ascii(cpu->hypervisor, HW64_HYP_VENDOR_MAX, "none", 4u);
        }
    }

    // 叶 7.0：SMEP（叶不存在时保持 0）
    if (cpu->leaf_max >= 7u) {
        hw64_cpuid(7u, 0u, &a, &b, &c, &d);
        cpu->reg7_ebx = b;
        cpu->smep = (b & (1u << 7)) != 0;
    }
}

// ==================== PCI（0xCF8/0xCFC 配置机制，只读）====================
static uint32_t hw64_pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    const uint32_t addr = 0x80000000u
                        | ((uint32_t)bus << 16)
                        | ((uint32_t)dev << 11)
                        | ((uint32_t)fn << 8)
                        | (uint32_t)(off & 0xFCu);
    outl(0xCF8u, addr);
    return inl(0xCFCu);
}

// 枚举 0..255 号总线。只登记前 HW64_PCI_MAX 个设备（类计数不受上限影响）。
// 提前停扫条件：连续 4 条总线一个设备都没有（其后不可能再有设备），避免在
// 配置机制坏掉的机器上空转 256*32 次端口读。
static void hw64_pci_scan(HwPci64* pci) {
    hw64_memzero(pci, (uint32_t)sizeof(*pci));
    uint32_t empty_run = 0;
    for (uint32_t bus = 0u; bus < 256u; bus++) {
        bool bus_has = false;
        for (uint32_t dev = 0u; dev < 32u; dev++) {
            uint32_t id = hw64_pci_cfg_read32((uint8_t)bus, (uint8_t)dev, 0u, 0x00u);
            uint16_t vendor = (uint16_t)(id & 0xFFFFu);
            if (vendor == 0xFFFFu || vendor == 0x0000u) continue;   // 无设备
            bus_has = true;

            // header type（offset 0x0E）bit7 = 多功能设备
            const uint32_t hdr = hw64_pci_cfg_read32((uint8_t)bus, (uint8_t)dev, 0u, 0x0Cu);
            const uint32_t nfn = (hdr & 0x00800000u) ? 8u : 1u;
            for (uint32_t fn = 0u; fn < nfn; fn++) {
                if (fn != 0u) {
                    id = hw64_pci_cfg_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x00u);
                    vendor = (uint16_t)(id & 0xFFFFu);
                    if (vendor == 0xFFFFu || vendor == 0x0000u) continue;
                }
                const uint16_t device = (uint16_t)(id >> 16);
                const uint32_t cls = hw64_pci_cfg_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x08u);
                const uint8_t rev  = (uint8_t)(cls & 0xFFu);
                const uint8_t sub  = (uint8_t)((cls >> 16) & 0xFFu);
                const uint8_t base = (uint8_t)((cls >> 24) & 0xFFu);

                pci->scanned++;
                if (pci->count < HW64_PCI_MAX) {
                    HwPciDev64* e = &pci->devs[pci->count++];
                    e->bus = (uint8_t)bus;
                    e->dev = (uint8_t)dev;
                    e->fn  = (uint8_t)fn;
                    e->class_code = base;
                    e->subclass = sub;
                    e->rev = rev;
                    e->pad[0] = 0;
                    e->pad[1] = 0;
                    e->vendor = vendor;
                    e->device = device;
                }
                // 类统计：class bit7 是"老设备没反映真实类码"的标记，比较时掩掉。
                // ide 按规范只算 0x0101；storage 把整个 0x01 类都算上，显示更全。
                const uint8_t base7 = (uint8_t)(base & 0x7Fu);
                if (base7 == 0x01u) {
                    pci->storage++;
                    if (sub == 0x01u) pci->ide++;
                } else if (base7 == 0x02u) {
                    pci->net++;
                } else if (base7 == 0x03u) {
                    pci->vga++;
                }
            }
        }
        if (bus_has) {
            pci->bus_max = bus;
            empty_run = 0;
        } else if (++empty_run >= 4u) {
            break;
        }
    }
}

// ==================== 串口日志（格式见文件头，改前先想清楚验收）====================
static void hw64_cpu_log(const HwCpu64* cpu) {
    dbg64_str("[HW64] cpu vendor=");
    dbg64_str(cpu->vendor);
    dbg64_str(" brand=");
    dbg64_str(cpu->brand);
    dbg64_str(" cores=");
    dbg64_dec(cpu->cores);
    dbg64_str(" leafmax=0x");
    hw64_hex_min(cpu->leaf_max);
    dbg64_str(" hyp=");
    dbg64_str(cpu->hypervisor);
    dbg64_nl();

    dbg64_str("[HW64] features=");
    hw64_feat_token("SMP", cpu->smp);   dbg64_putc(' ');
    hw64_feat_token("NX", cpu->nx);     dbg64_putc(' ');
    hw64_feat_token("PAE", cpu->pae);   dbg64_putc(' ');
    hw64_feat_token("SSE2", cpu->sse2); dbg64_putc(' ');
    hw64_feat_token("AVX", cpu->avx);   dbg64_putc(' ');
    hw64_feat_token("VMX", cpu->vmx);   dbg64_putc(' ');
    hw64_feat_token("SMEP", cpu->smep); dbg64_putc(' ');
    hw64_feat_token("LM", cpu->lm);
    dbg64_nl();
}

static void hw64_pci_log(const HwPci64* pci) {
    dbg64_str("[HW64] pci devices=");
    dbg64_dec(pci->count);
    dbg64_str(" ide=");
    dbg64_dec(pci->ide);
    dbg64_str(" net=");
    dbg64_dec(pci->net);
    dbg64_str(" vga=");
    dbg64_dec(pci->vga);
    dbg64_nl();

    const uint32_t n = (pci->count < HW64_PCI_LOG_MAX) ? pci->count : HW64_PCI_LOG_MAX;
    for (uint32_t i = 0u; i < n; i++) {
        const HwPciDev64* e = &pci->devs[i];
        dbg64_str("[HW64] pci ");
        dbg64_dec(e->bus);
        dbg64_putc(':');
        dbg64_dec(e->dev);
        dbg64_putc('.');
        dbg64_dec(e->fn);
        dbg64_putc(' ');
        hw64_hex_digits(e->vendor, 4u);
        dbg64_putc(':');
        hw64_hex_digits(e->device, 4u);
        dbg64_str(" class=0x");
        hw64_hex_digits(e->class_code, 2u);
        dbg64_str(" rev=0x");
        hw64_hex_digits(e->rev, 2u);
        dbg64_nl();
    }
}

// ==================== 对外 API ====================
extern "C" {

void hwinfo_init64() {
    // CPU 每次重新探测（幂等）；失败时保持 "none"/0 的降级值
    hw64_cpu_detect(&g_info.cpu);
    // PCI 只读枚举
    hw64_pci_scan(&g_info.pci);
    g_info.magic = HW64_INFO_MAGIC;
    // 注意：这里**不重置磁盘表**（由 part64/ata64 侧通过 setter 填，可能在 init 前后发生）
    hw64_cpu_log(&g_info.cpu);
    hw64_pci_log(&g_info.pci);
}

const HwInfo64* hw_info64() {
    return &g_info;                 // 静态存储，永不为空
}

int hwinfo_selftest64() {
    // 单独调用也能工作：没跑过 init 时先自动补一次探测（幂等）
    if (g_info.magic != HW64_INFO_MAGIC) hwinfo_init64();

    int fails = 0;

    // bit0：CPUID 通路可用（EFLAGS.ID 可翻转）
    if (!hw64_cpuid_supported()) fails |= 1;
    // bit1：厂商串非空且不越界（必须 NUL 结尾）
    if (g_info.cpu.vendor[0] == 0 ||
        hw64_strlen(g_info.cpu.vendor) >= HW64_CPU_VENDOR_MAX) fails |= 2;
    // bit2：brand / hypervisor 长度在容量内（可以为空 -> "none"，但绝不能越界）
    if (hw64_strlen(g_info.cpu.brand) >= HW64_CPU_BRAND_MAX ||
        hw64_strlen(g_info.cpu.hypervisor) >= HW64_HYP_VENDOR_MAX) fails |= 4;
    // bit3：逻辑核数在合理范围
    if (g_info.cpu.cores < 1u || g_info.cpu.cores > 1024u) fails |= 8;
    // bit4：特征位解码与原始 CPUID 寄存器一致（防止位号写错）
    {
        const HwCpu64* c = &g_info.cpu;
        const bool sse_raw  = (c->reg1_edx & (1u << 25)) != 0;
        const bool sse2_raw = (c->reg1_edx & (1u << 26)) != 0;
        const bool pae_raw  = (c->reg1_edx & (1u << 6))  != 0;
        const bool avx_raw  = (c->reg1_ecx & (1u << 28)) != 0;
        const bool vmx_raw  = (c->reg1_ecx & (1u << 5))  != 0;
        const bool smep_raw = (c->reg7_ebx   & (1u << 7)) != 0;
        const bool nx_raw   = (c->regext1_edx & (1u << 20)) != 0;
        const bool lm_raw   = (c->regext1_edx & (1u << 29)) != 0;
        if (c->sse != sse_raw || c->sse2 != sse2_raw || c->pae != pae_raw ||
            c->avx != avx_raw || c->vmx != vmx_raw || c->smep != smep_raw ||
            c->nx != nx_raw || c->lm != lm_raw) fails |= 16;
    }
    // bit5：PCI 表一致性（0 个设备不算失败：无 PCI 环境属于合法降级）
    if (g_info.pci.count > HW64_PCI_MAX || g_info.pci.bus_max > 255u ||
        g_info.pci.ide > g_info.pci.storage) fails |= 32;
    for (uint32_t i = 0u; i < g_info.pci.count; i++) {
        const uint16_t v = g_info.pci.devs[i].vendor;
        if (v == 0xFFFFu || v == 0u) fails |= 32;
    }
    // bit6：磁盘表一致性（present 条目必须有型号和扇区数；计数与 present 数一致）
    {
        uint32_t present = 0;
        for (uint32_t i = 0u; i < HW64_DISK_MAX; i++) {
            const HwDisk64* d = &g_info.disks[i];
            if (!d->present) continue;
            present++;
            if (d->model[0] == 0 || hw64_strlen(d->model) >= HW64_DISK_MODEL_MAX ||
                d->sectors_512 == 0) fails |= 64;
        }
        if (present != g_info.disk_count) fails |= 64;
    }
    // bit7：API/初始化自洽（getter 必须指向同一份静态数据；init 必须留下 magic）
    if (hw_info64() != &g_info || g_info.magic != HW64_INFO_MAGIC) fails |= 128;

    dbg64_str("[HW64] selftest ");
    if (fails == 0) {
        dbg64_str("PASS");
    } else {
        dbg64_str("FAIL mask=");
        dbg64_dec((uint64_t)fails);
    }
    dbg64_nl();
    return fails;
}

void hwinfo_set_disk64(int idx, const char* model, uint64_t sectors_512, int lba48) {
    if (idx < 0 || idx >= (int)HW64_DISK_MAX) return;   // 越界静默忽略，调用方 bug 不该崩内核
    HwDisk64* d = &g_info.disks[idx];
    hw64_memzero(d, (uint32_t)sizeof(*d));
    d->lba48 = (lba48 != 0);
    d->sectors_512 = sectors_512;
    if (model && model[0] != 0) {
        hw64_store_ascii(d->model, HW64_DISK_MODEL_MAX, model, hw64_strlen(model));
        d->present = (d->model[0] != 0);
    }
    if (!d->present) d->sectors_512 = 0;                // 空槽位不留半截数据

    // 重算"实际登记的磁盘数"（present 条目数），供 GUI 直接使用
    uint32_t n = 0;
    for (uint32_t i = 0u; i < HW64_DISK_MAX; i++)
        if (g_info.disks[i].present) n++;
    g_info.disk_count = n;
}

}  // extern "C"
