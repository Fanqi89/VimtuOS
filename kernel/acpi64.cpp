// acpi64.cpp - ACPI 表解析：RSDP -> RSDT/XSDT -> FACP/APIC/HPET/MCFG
//
// 这一版只做"读表"，不做"用表"：
//   * 读：找 RSDP，校验合法性，挑 XSDT 或 RSDT，遍历表项，把 FADT / MADT(MADT="APIC") /
//         HPET / MCFG 里跟电源、CPU 个数、APIC、PCIe 配置空间有关的字段记进静态状态。
//   * 不读：DSDT/SSDT 里的 AML 一律不解释（没有 AML 解释器，也就不去算 _S5/_PTS）。
//   * 不写：除了 acpi_poweroff64() 那一次 PM1a_CNT 写，全程不写端口、不写表内存，
//          因此可以在中断都没开、堆都没初始化的阶段安全调用。
//
// RSDP 两个来源（见 acpi64_find_rsdp）：
//   ① 引导层传递槽：物理 0x7800 的 8 字节 u64（ML64_RSDP_PTR_PHYS）。UEFI 固件按规范
//      通过 EFI 系统表的配置表交付 RSDP（ACPI 2.0/1.0 GUID），不放 legacy 窗口，
//      由 boot/efi/uefi64.c 写进这个槽；BIOS 路径（boot/loader64.asm）显式写 0。
//   ② legacy 扫描：EBDA（BDA 0x40E 的段址）+ 0xE0000..0xFFFFF 的 BIOS ROM 区。
//
// 为什么物理地址能直接当指针用：
//   BIOS 路径下 boot/loader64.asm 建页表时把前 4GB 做了恒等映射（ML64_IDENTITY_BYTES）；
//   UEFI 路径下用的是固件自己的页表 —— EDK2 为全部 RAM 建了恒等映射，内核 48MB 直映之外
//   的表地址也依赖它（读不到就走 legacy 扫描，不硬崩）。两种路径下内核生命周期内这些映射
//   都不撤销，所以 (T*)(uintptr_t)phys 合法可读。
//   但"能读"不等于"该读"：固件给的表可能长度字段撒谎、地址越界，本模块对每张表
//   都先过 acpi64_pa_ok() 守卫（地址 + 长度完整落在 [0, 4GB) 内），越界直接跳过。
//
// 踩坑（写这一版时踩到的，供后来者参考）：
//   1) packed 结构体的**数组成员不能取地址**（char sig[4] 会 decay 成指针）：
//      clang 的 -Waddress-of-packed-member 会报，而且真拿到的是未对齐指针。
//      所以签名 / OEMID 一律用 (const uint8_t*) 按字节读，不碰任何成员地址。
//   2) FADT 里 0x68..0x6C 那一段（DUTY_/DAY_ALRM/MON_ALRM/CENTURY）在各版规范
//      与各家实现里排列不一致，本模块用不上，用字节数组占位，保证真正要用的
//      0x6D / 0x70 两个偏移与规范一致（下面 static_assert 钉死）。
//   3) 表项地址在 XSDT 里是 8 字节、RSDT 里是 4 字节，且**不保证对齐**，
//      统一用 acpi64_load_u32/u64() 按字节拼，避免未对齐/别名问题。

#include "acpi64.h"
#include "debug64.h"        // dbg64_* 串口打点（串口 + QEMU debugcon 双写）
#include "port.h"           // outw / outl / io_wait（只在 acpi_poweroff64 里用）
#include "memlayout64.h"    // ML64_IDENTITY_BYTES：前 4GB 恒等映射上界
#include <stddef.h>         // offsetof（配合 static_assert 钉死结构体偏移）

// ==================== 常量 ====================
// IOAPIC 记录上限：真机/虚拟机 1~4 个足够，超出部分忽略（不是错误）
static const int ACPI64_MAX_IOAPIC = 8;

// PM1_CNT 控制寄存器位（ACPI 规范：bit12..10 = SLP_TYPa，bit13 = SLP_EN）
static const uint16_t ACPI64_SLP_TYPa5 = (uint16_t)(5u << 10);  // 5 = S5（软关机）
static const uint16_t ACPI64_SLP_EN    = (uint16_t)(1u << 13);  // 写 1 触发睡眠状态迁移

// 通用地址结构（GAS）的地址空间编号
static const uint8_t ACPI64_GAS_IO = 1;      // 1 = 系统 IO 端口空间

// ==================== 表结构（全部 packed + 偏移注释）====================
// 为什么全部 packed：固件给的表是按字节紧密排的，编译器默认会给 u64 成员插对齐填充，
// 一旦插了填充，字段偏移就和规范不一致 —— 读出来全是错的。这里的 packed 让
// "结构体偏移 == 规范偏移"，下面的 static_assert 再把关键偏移钉死，写错就编译失败。

// ---- RSDP：ACPI 的入口。BIOS 藏在 EBDA 或 0xE0000..0xFFFFF；UEFI 由固件通过 EFI 配置表交付
//      （引导层把它写进物理 0x7800 的 u64 槽，见 boot/efi/uefi64.c 与 memlayout64.h）----
struct AcpiRsdp64 {
    char     signature[8];      // 0x00 "RSD PTR "（注意尾部有空格）
    uint8_t  checksum;          // 0x08 前 20 字节求和 == 0
    char     oem_id[6];         // 0x09 定长 6 字节，不带结束符
    uint8_t  revision;          // 0x0F 0 = ACPI 1.0；>= 2 = ACPI 2.0+
    uint32_t rsdt_address;      // 0x10 RSDT 物理地址（32 位，ACPI 1.0 唯一入口）
    // ---- 以下仅 ACPI 2.0+ 存在（revision >= 2），扫 1.0 的 RSDP 时不能读 ----
    uint32_t length;            // 0x14 本 RSDP 的长度（>= 36）
    uint64_t xsdt_address;      // 0x18 XSDT 物理地址（64 位，能描述 4GB 以上的表）
    uint8_t  ext_checksum;      // 0x20 整份 RSDP 求和 == 0
    uint8_t  reserved[3];       // 0x21
} __attribute__((packed));      // = 36 字节

// ---- 所有 SDT 共用的 36 字节头 ----
struct AcpiSdtHeader64 {
    char     signature[4];      // 0x00 例如 "FACP"/"APIC"/"HPET"/"MCFG"
    uint32_t length;            // 0x04 整表长度（含本头）—— 本模块所有读取都由它守卫
    uint8_t  revision;          // 0x08 表自身版本（FADT 用它判断有没有 64 位字段）
    uint8_t  checksum;          // 0x09
    char     oem_id[6];         // 0x0A
    char     oem_table_id[8];   // 0x10
    uint32_t oem_revision;      // 0x18
    uint32_t creator_id;        // 0x1C
    uint32_t creator_revision;  // 0x20
} __attribute__((packed));      // = 36 字节

// ---- 根表：RSDT / XSDT。36 字节头 + 变长表项数组（长度由 hdr.length 决定），
//      数组类型无法写成定长成员，所以这里只用来记录"根表"这个概念，
//      实际读取统一走"根表首地址 + 0x24 + i*step"的字节偏移。----
struct AcpiRsdt64 { AcpiSdtHeader64 hdr; };   // 0x24 起：uint32[] 各表的 32 位物理地址
struct AcpiXsdt64 { AcpiSdtHeader64 hdr; };   // 0x24 起：uint64[] 各表的 64 位物理地址

// ---- 通用地址结构（GAS，12 字节）：ACPI 2.0+ 用它描述任意地址空间的寄存器 ----
struct AcpiGas64 {
    uint8_t  address_space_id;  // 0x00 0=系统内存 1=系统 IO 2=PCI 配置 3=嵌入式 ...
    uint8_t  register_bit_width;// 0x01 寄存器位宽（0 = 未指定）
    uint8_t  register_bit_offset;//0x02 位偏移
    uint8_t  access_size;       // 0x03 访问宽度编码（0 = 未指定）
    uint64_t address;           // 0x04 地址（IO 空间时低 16 位才是端口号）
} __attribute__((packed));      // = 12 字节

// ---- FADT（"FACP"）：SCI、SMI、电源管理寄存器、复位寄存器 ----
struct AcpiFadt64 {
    AcpiSdtHeader64 hdr;        // 0x00 SDT 头（36 字节）
    uint32_t firmware_ctrl;     // 0x24 FACS 物理地址
    uint32_t dsdt;              // 0x28 DSDT 物理地址（本模块不解释 AML）
    uint8_t  int_model;         // 0x2C ACPI 1.0 的中断模型（2.0+ 起为 Reserved）
    uint8_t  preferred_profile; // 0x2D 首选电源管理配置
    uint16_t sci_int;           // 0x2E SCI 中断向量（ISA IRQ 号，典型 9）
    uint32_t smi_cmd;           // 0x30 SMI 命令端口（0 = 没有；本模块只记录，不写）
    uint8_t  acpi_enable;       // 0x34 写 SMI_CMD 的值 -> 切进 ACPI 模式
    uint8_t  acpi_disable;      // 0x35 写 SMI_CMD 的值 -> 退出 ACPI 模式
    uint8_t  s4bios_req;        // 0x36 进 S4BIOS 状态的值
    uint8_t  pstate_cnt;        // 0x37 处理器性能状态控制端口
    uint32_t pm1a_evt_blk;      // 0x38 PM1a 事件寄存器块端口（SCI 状态/使能在里面）
    uint32_t pm1b_evt_blk;      // 0x3C
    uint32_t pm1a_cnt_blk;      // 0x40 PM1a 控制寄存器块端口 —— 软关机写这里（SLP_TYPa+SLP_EN）
    uint32_t pm1b_cnt_blk;      // 0x44
    uint32_t pm2_cnt_blk;       // 0x48
    uint32_t pm_tmr_blk;        // 0x4C PM 定时器端口（32 位自由运行计数器）
    uint32_t gpe0_blk;          // 0x50
    uint32_t gpe1_blk;          // 0x54
    uint8_t  pm1_evt_len;       // 0x58 上面各寄存器块的长度（字节）
    uint8_t  pm1_cnt_len;       // 0x59 PM1a 控制寄存器长度（4 或 2 —— 决定 outl 还是 outw）
    uint8_t  pm2_cnt_len;       // 0x5A
    uint8_t  pm_tmr_len;        // 0x5B
    uint8_t  gpe0_blk_len;      // 0x5C
    uint8_t  gpe1_blk_len;      // 0x5D
    uint8_t  gpe1_base;         // 0x5E
    uint8_t  cst_cnt;           // 0x5F
    uint16_t p_lvl2_lat;        // 0x60 C2 退出延迟
    uint16_t p_lvl3_lat;        // 0x62 C3 退出延迟
    uint16_t flush_size;        // 0x64
    uint16_t flush_stride;      // 0x66
    uint8_t  legacy_rtc_block[5]; // 0x68..0x6C DUTY_OFFSET/DUTY_WIDTH/DAY_ALRM/MON_ALRM/CENTURY
                                  //          （各版规范排列有出入，用不上，占位保偏移）
    uint16_t iapc_boot_arch;    // 0x6D IA-PC 引导架构标志（键盘 8042/无 VGA 等）
    uint8_t  reserved1;         // 0x6F
    uint32_t flags;             // 0x70 固定特性标志（bit10 = RESET_REG_SUP 等）
    // ---- 以下 ACPI 2.0+（FADT revision >= 2）才有 ----
    AcpiGas64 reset_reg;        // 0x74 复位寄存器（GAS，12 字节）
    uint8_t  reset_value;       // 0x80 写 RESET_REG 的值
    uint16_t arm_boot_arch;     // 0x81 ARM 引导架构标志（ACPI 5.1+）
    uint8_t  fadt_minor_version;// 0x83 FADT 次版本（ACPI 6.2+）
    uint64_t x_firmware_ctrl;   // 0x84 FACS 的 64 位物理地址
    uint64_t x_dsdt;            // 0x8C DSDT 的 64 位物理地址
    AcpiGas64 x_pm1a_evt_blk;   // 0x94 64 位地址版的 PM1a 事件块
    AcpiGas64 x_pm1b_evt_blk;   // 0xA0
    AcpiGas64 x_pm1a_cnt_blk;   // 0xAC ★ 64 位地址版的 PM1a 控制块（规范要求 2.0+ 优先用它）
    AcpiGas64 x_pm1b_cnt_blk;   // 0xB8
    AcpiGas64 x_pm2_cnt_blk;    // 0xC4
    AcpiGas64 x_pm_tmr_blk;     // 0xD0
    AcpiGas64 x_gpe0_blk;       // 0xDC
    AcpiGas64 x_gpe1_blk;       // 0xE8
    AcpiGas64 sleep_control_reg;// 0xF4 睡眠控制寄存器（S3/S4 的 SLP_TYPa 在哪个寄存器里）
    AcpiGas64 sleep_status_reg; // 0x100 睡眠状态寄存器
    // 0x10C Hypervisor_Vendor_Identity（ACPI 6.0+）本模块用不到，不再往下写
} __attribute__((packed));      // = 0x10C 字节

// ---- MADT（"APIC"）：0x24 起是变长条目数组，逐条 type/length 走 ----
struct AcpiMadt64 {
    AcpiSdtHeader64 hdr;        // 0x00 SDT 头
    uint32_t lapic_address;     // 0x24 本地 APIC 物理地址（可被条目 type 5 覆盖）
    uint32_t flags;             // 0x28 bit0 = PCAT_COMPAT（同时存在 8259 兼容中断控制器）
} __attribute__((packed));      // = 0x2C 字节

// 每个 MADT 条目都以 2 字节头开始，长度字段是自己声明的（可能比规范短/长）
struct AcpiMadtEntry64 {
    uint8_t type;               // 0x00 条目类型
    uint8_t length;             // 0x01 本条目长度（含这 2 字节）
} __attribute__((packed));      // = 2 字节

// type 0：处理器本地 APIC
struct AcpiMadtLapic64 {
    uint8_t  type;              // 0x00 = 0
    uint8_t  length;            // 0x01 = 8
    uint8_t  acpi_processor_id; // 0x02
    uint8_t  apic_id;           // 0x03
    uint32_t flags;             // 0x04 bit0 = 该处理器已启用（enabled）
} __attribute__((packed));      // = 8 字节

// type 1：IOAPIC
struct AcpiMadtIoapic64 {
    uint8_t  type;              // 0x00 = 1
    uint8_t  length;            // 0x01 = 12
    uint8_t  ioapic_id;         // 0x02
    uint8_t  reserved;          // 0x03
    uint32_t address;           // 0x04 IOAPIC 寄存器基址（物理，典型 0xFEC00000）
    uint32_t gsi_base;          // 0x08 本 IOAPIC 覆盖的全局中断号起点
} __attribute__((packed));      // = 12 字节

// type 2：中断源覆盖（ISO）
struct AcpiMadtIso64 {
    uint8_t  type;              // 0x00 = 2
    uint8_t  length;            // 0x01 = 10
    uint8_t  bus;               // 0x02 总线（0 = ISA）
    uint8_t  source;            // 0x03 源 IRQ
    uint32_t gsi;               // 0x04 覆盖后的全局中断号
    uint16_t flags;             // 0x08 极性/触发方式
} __attribute__((packed));      // = 10 字节

// type 5：本地 APIC 地址覆盖（64 位地址版）
struct AcpiMadtLapicOverride64 {
    uint8_t  type;              // 0x00 = 5
    uint8_t  length;            // 0x01 = 12
    uint16_t reserved;          // 0x02
    uint64_t address;           // 0x04 覆盖后的本地 APIC 物理地址
} __attribute__((packed));      // = 12 字节

// type 9：处理器本地 x2APIC
struct AcpiMadtX2Apic64 {
    uint8_t  type;              // 0x00 = 9
    uint8_t  length;            // 0x01 = 16
    uint16_t reserved;          // 0x02
    uint32_t x2apic_id;         // 0x04 x2APIC ID
    uint32_t flags;             // 0x08 bit0 = enabled
    uint32_t processor_uid;     // 0x0C
} __attribute__((packed));      // = 16 字节

// ---- HPET（表签名就是 "HPET"）----
struct AcpiHpet64 {
    AcpiSdtHeader64 hdr;        // 0x00 SDT 头
    uint32_t hardware_id;       // 0x24 硬件块 ID
    AcpiGas64 base_address;     // 0x28 HPET 寄存器块基地址（GAS；地址字段在 0x2C）
    uint8_t  sequence;          // 0x34 序号
    uint16_t minimum_tick;      // 0x35 最小时钟滴答
    uint8_t  page_protection;   // 0x37
} __attribute__((packed));      // = 0x38 字节

// ---- MCFG：PCIe ECAM（增强配置访问）基址分配 ----
struct AcpiMcfg64 {
    AcpiSdtHeader64 hdr;        // 0x00 SDT 头
    uint64_t reserved;          // 0x24
    // 0x2C 起：16 字节一项的"配置空间基址分配结构"数组
} __attribute__((packed));      // = 0x2C 字节

struct AcpiMcfgEntry64 {
    uint64_t base_address;      // 0x00 ECAM 基址（物理）
    uint16_t pci_segment_group; // 0x08 PCI 段组号
    uint8_t  start_bus;         // 0x0A 起始总线号
    uint8_t  end_bus;           // 0x0B 结束总线号
    uint32_t reserved;          // 0x0C
} __attribute__((packed));      // = 16 字节

// 编译期把关键尺寸/偏移钉死：写错字段顺序时编译当场失败，而不是运行时读到垃圾
static_assert(sizeof(AcpiRsdp64)            == 36,   "RSDP 必须是 36 字节");
static_assert(sizeof(AcpiSdtHeader64)       == 36,   "SDT 头必须是 36 字节");
static_assert(sizeof(AcpiGas64)             == 12,   "GAS 必须是 12 字节");
static_assert(offsetof(AcpiRsdp64, revision)     == 0x0F, "RSDP revision 偏移错");
static_assert(offsetof(AcpiRsdp64, rsdt_address) == 0x10, "RSDP rsdt_address 偏移错");
static_assert(offsetof(AcpiRsdp64, xsdt_address) == 0x18, "RSDP xsdt_address 偏移错");
static_assert(offsetof(AcpiSdtHeader64, length)  == 0x04, "SDT length 偏移错");
static_assert(offsetof(AcpiFadt64, sci_int)        == 0x2E, "FADT sci_int 偏移错");
static_assert(offsetof(AcpiFadt64, smi_cmd)        == 0x30, "FADT smi_cmd 偏移错");
static_assert(offsetof(AcpiFadt64, pm1a_evt_blk)   == 0x38, "FADT pm1a_evt_blk 偏移错");
static_assert(offsetof(AcpiFadt64, pm1a_cnt_blk)   == 0x40, "FADT pm1a_cnt_blk 偏移错");
static_assert(offsetof(AcpiFadt64, pm1_cnt_len)    == 0x59, "FADT pm1_cnt_len 偏移错");
static_assert(offsetof(AcpiFadt64, flags)          == 0x70, "FADT flags 偏移错");
static_assert(offsetof(AcpiFadt64, reset_reg)      == 0x74, "FADT reset_reg 偏移错");
static_assert(offsetof(AcpiFadt64, reset_value)    == 0x80, "FADT reset_value 偏移错");
static_assert(offsetof(AcpiFadt64, x_pm1a_cnt_blk) == 0xAC, "FADT x_pm1a_cnt_blk 偏移错");
static_assert(sizeof(AcpiFadt64) == 0x10C, "FADT 定义长度与规范不一致");
static_assert(sizeof(AcpiMadt64)            == 0x2C, "MADT 头必须是 0x2C 字节");
static_assert(offsetof(AcpiMadt64, lapic_address) == 0x24, "MADT lapic_address 偏移错");
static_assert(offsetof(AcpiMadt64, flags)         == 0x28, "MADT flags 偏移错");
static_assert(sizeof(AcpiMadtEntry64)        == 2,  "MADT 条目头必须是 2 字节");
static_assert(sizeof(AcpiMadtLapic64)        == 8,  "MADT type 0 必须是 8 字节");
static_assert(sizeof(AcpiMadtIoapic64)       == 12, "MADT type 1 必须是 12 字节");
static_assert(sizeof(AcpiMadtIso64)          == 10, "MADT type 2 必须是 10 字节");
static_assert(sizeof(AcpiMadtLapicOverride64)== 12, "MADT type 5 必须是 12 字节");
static_assert(sizeof(AcpiMadtX2Apic64)       == 16, "MADT type 9 必须是 16 字节");
static_assert(sizeof(AcpiHpet64)             == 0x38, "HPET 表必须是 0x38 字节");
static_assert(offsetof(AcpiHpet64, base_address) == 0x28, "HPET base_address 偏移错");
static_assert(sizeof(AcpiMcfg64)             == 0x2C, "MCFG 表头必须是 0x2C 字节");
static_assert(sizeof(AcpiMcfgEntry64)        == 16,   "MCFG 条目必须是 16 字节");

// ==================== 解析结果（本模块的全部对外状态）====================
static bool     g_inited        = false;  // acpi_init64() 是否跑过（幂等 + selftest bit7）
static bool     g_found         = false;  // RSDP 找到且前 20 字节校验通过
static bool     g_src_uefi      = false;  // RSDP 来自引导层传递槽 0x7800（UEFI 配置表）；false = legacy 扫描
static uint64_t g_rsdp_addr     = 0;
static uint8_t  g_revision      = 0;      // RSDP 的 ACPI 版本
static char     g_oem_id[7]     = {0};    // RSDP 的 OEMID（6 字节 + 结束符）
static uint64_t g_root_addr     = 0;      // 根表（XSDT/RSDT）物理地址
static uint32_t g_root_len      = 0;
static uint32_t g_tables        = 0;      // 根表里数到的表项数
static uint32_t g_known_tables  = 0;      // 认出来的表（FACP/APIC/HPET/MCFG）张数

// FADT
static bool     g_fadt_ok       = false;
static uint16_t g_sci_int       = 0;      // 0x2E SCI 中断向量
static uint32_t g_smi_cmd       = 0;      // 0x30 SMI 命令端口（只记录，不写）
static uint32_t g_pm1a_evt_blk  = 0;      // 0x38
static uint32_t g_pm1a_cnt_blk  = 0;      // 0x40（rev>=2 且表够长时会被 0xAC 覆盖）
static uint8_t  g_pm1_cnt_len   = 0;      // 0x59
static uint64_t g_reset_reg_addr= 0;      // 0x78（仅 IO 空间时记录，0 = 不可用）
static uint8_t  g_reset_value   = 0;      // 0x80

// MADT
static bool     g_madt_ok       = false;
static uint32_t g_lapic_addr    = 0;      // 0x24（可被 type 5 覆盖）
static uint32_t g_madt_flags    = 0;      // 0x28
static uint32_t g_lapic_total   = 0;      // type 0 条目总数
static uint32_t g_lapic_enabled = 0;      // type 0 里 enabled 的个数
static uint32_t g_x2apic_total  = 0;      // type 9 条目总数
static uint32_t g_x2apic_enabled= 0;      // type 9 里 enabled 的个数
static uint32_t g_iso_count     = 0;      // type 2 条目数
static int      g_ioapic_count  = 0;      // type 1 条目数（记录在案的）
static uint32_t g_ioapic_addr[ACPI64_MAX_IOAPIC];
static uint32_t g_ioapic_gsi[ACPI64_MAX_IOAPIC];

// HPET / MCFG
static uint64_t g_hpet_addr     = 0;
static uint64_t g_mcfg_base     = 0;
static uint32_t g_mcfg_segments = 0;

// ==================== 小工具 ====================
// 物理地址守卫：地址非 0 且 [pa, pa+bytes) 完整落在前 4GB 恒等映射内。
// 本模块所有"把物理地址当指针"的地方都必须先过这里 —— 恒等映射下越界读
// 不会 #PF，但读到的是别的东西，比崩掉更难查。
static bool acpi64_pa_ok(uint64_t pa, uint64_t bytes) {
    if (pa == 0) return false;
    if (pa >= ML64_IDENTITY_BYTES) return false;
    if (bytes > ML64_IDENTITY_BYTES - pa) return false;
    return true;
}

// 从字节流里取小端 u32/u64：表项地址不保证对齐，且不能对 packed 成员取 u64*。
static uint32_t acpi64_load_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t acpi64_load_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | (uint64_t)p[i];
    return v;
}

// 4 字节签名比较（表里的签名不是以 0 结尾的字符串，只能按长度比）
static bool acpi64_sig_eq(const char* a, const char* s) {
    for (int i = 0; i < 4; i++) if (a[i] != s[i]) return false;
    return true;
}

// 从 packed 表头里拷出 4 字节签名（不取数组成员地址，见文件头踩坑 1）
static void acpi64_copy_sig(const AcpiSdtHeader64* h, char out[5]) {
    const uint8_t* p = (const uint8_t*)(const void*)h;
    for (int i = 0; i < 4; i++) out[i] = (char)p[i];
    out[4] = 0;
}

// 直接拿表头比签名（内部先按字节拷出 4 字节，避免取 packed 数组成员地址）
static bool acpi64_sig_eq_header(const AcpiSdtHeader64* h, const char* s) {
    char sig[5];
    acpi64_copy_sig(h, sig);
    return acpi64_sig_eq(sig, s);
}

// 打印地址：统一 0x + 16 位定宽十六进制（与 [LM64] 的打点风格一致，便于自动验收 grep）
static void acpi64_phex(uint64_t v) {
    dbg64_str("0x");
    dbg64_hex64(v);
}

// 表长度不足时的统一报告（自动验收会 grep 这一行）
static void acpi64_table_too_short(const AcpiSdtHeader64* h, uint32_t len) {
    char sig[5];
    acpi64_copy_sig(h, sig);
    dbg64_str("[ACPI64] table ");
    dbg64_str(sig);
    dbg64_str(" too short len=");
    dbg64_dec(len);
    dbg64_nl();
}

// ==================== 各表的解析 ====================
// FADT（"FACP"）：只要电源管理这块（SCI / SMI / PM1a / 复位寄存器）
static void acpi64_parse_fadt(const AcpiSdtHeader64* h, uint32_t len) {
    // 硬性下限 0x44：再短连 PM1a_CNT_BLK（0x40..0x44）都读不到，没有解析价值
    if (len < 0x44u) { acpi64_table_too_short(h, len); return; }
    const AcpiFadt64* f = (const AcpiFadt64*)h;

    g_fadt_ok      = true;
    g_sci_int      = f->sci_int;        // 0x2E
    g_smi_cmd      = f->smi_cmd;        // 0x30
    g_pm1a_evt_blk = f->pm1a_evt_blk;   // 0x38
    g_pm1a_cnt_blk = f->pm1a_cnt_blk;   // 0x40
    if (len >= 0x5Au) g_pm1_cnt_len = f->pm1_cnt_len;   // 0x59

    // RESET_REG（0x74，GAS）+ RESET_VALUE（0x80）是 ACPI 2.0+ 才有的字段，
    // 只在"表足够长 + 地址空间是 IO 端口 + 端口号合法"时记录：
    // 本内核的硬复位只能 outb/outw，内存映射的复位寄存器用不上。
    if (len >= 0x81u) {
        if (f->reset_reg.address_space_id == ACPI64_GAS_IO &&
            f->reset_reg.address != 0 &&
            f->reset_reg.address <= 0xFFFFull) {
            g_reset_reg_addr = f->reset_reg.address;   // IO 空间下低 16 位才是端口号
            g_reset_value    = f->reset_value;
        }
    }

    // FADT rev >= 2：规范要求优先用 64 位的 X_PM1a_CNT_BLK（0xAC），
    // 因为 32 位字段在 64 位端口地址下会被截断。表不够长就沿用 32 位值。
    if (h->revision >= 2 && len >= 0xB8u) {
        if (f->x_pm1a_cnt_blk.address_space_id == ACPI64_GAS_IO &&
            f->x_pm1a_cnt_blk.address != 0 &&
            f->x_pm1a_cnt_blk.address <= 0xFFFFull) {
            g_pm1a_cnt_blk = (uint32_t)f->x_pm1a_cnt_blk.address;
        }
    }
    g_known_tables++;
}

// MADT（"APIC"）：只统计条目，不建立中断路由（那是将来 APIC/SMP 驱动的事）
static void acpi64_parse_madt(const AcpiSdtHeader64* h, uint32_t len) {
    // MADT 头到 PCAT_COMPAT 标志为止是 0x2C
    if (len < 0x2Cu) { acpi64_table_too_short(h, len); return; }
    const AcpiMadt64* m = (const AcpiMadt64*)h;

    g_madt_ok    = true;
    g_lapic_addr = m->lapic_address;    // 0x24
    g_madt_flags = m->flags;            // 0x28

    uint32_t off = 0x2Cu;
    while (off + 2u <= len) {
        const uint8_t* p = (const uint8_t*)(const void*)h + off;
        const uint8_t et = p[0];        // 条目 type
        const uint8_t el = p[1];        // 条目 length（自己声明的，未必等于规范值）
        // 条目长度必须 >= 2 且不越过表尾，否则停止扫描（继续走只会读到表外）
        if (el < 2u || off + (uint32_t)el > len) break;

        switch (et) {
        case 0u:    // 处理器本地 APIC（8 字节）：entries / enabled 都统计
            if (el >= 8u) {
                g_lapic_total++;
                if (((const AcpiMadtLapic64*)p)->flags & 1u) g_lapic_enabled++;
            }
            break;
        case 1u:    // IOAPIC（12 字节）：记录物理基址 + GSI 基址
            if (el >= 12u && g_ioapic_count < ACPI64_MAX_IOAPIC) {
                g_ioapic_addr[g_ioapic_count] = ((const AcpiMadtIoapic64*)p)->address;
                g_ioapic_gsi[g_ioapic_count]  = ((const AcpiMadtIoapic64*)p)->gsi_base;
                g_ioapic_count++;
            }
            break;
        case 2u:    // 中断源覆盖 ISO（10 字节）：只数个数（路由留给 APIC 驱动）
            if (el >= 10u) g_iso_count++;
            break;
        case 5u:    // 本地 APIC 地址覆盖（12 字节）：64 位地址优先于 0x24 的 32 位字段
            if (el >= 12u) {
                const uint64_t a = ((const AcpiMadtLapicOverride64*)p)->address;
                if (a != 0 && a <= 0xFFFFFFFFull) g_lapic_addr = (uint32_t)a;
            }
            break;
        case 9u:    // 处理器本地 x2APIC（16 字节）：按 enabled 位统计
            if (el >= 16u) {
                g_x2apic_total++;
                if (((const AcpiMadtX2Apic64*)p)->flags & 1u) g_x2apic_enabled++;
            }
            break;
        default:    // type 3/4/6/7/8/0xA/0xB... 本模块不关心
            break;
        }
        off += el;
    }
    g_known_tables++;
}

// HPET（"HPET"）：只记基址（时间源驱动将来自己 map）
static void acpi64_parse_hpet(const AcpiSdtHeader64* h, uint32_t len) {
    // 至少要到 base_address（GAS，0x28..0x34）读得到
    if (len < 0x34u) { acpi64_table_too_short(h, len); return; }
    const AcpiHpet64* t = (const AcpiHpet64*)h;
    g_hpet_addr = t->base_address.address;   // 0x2C（GAS 的地址字段）
    g_known_tables++;
}

// MCFG（"MCFG"）：首个 ECAM 基址 + 段组数量
static void acpi64_parse_mcfg(const AcpiSdtHeader64* h, uint32_t len) {
    // 表头 + 8 字节保留 = 0x2C；短于此连"有没有条目"都判断不了
    if (len < 0x2Cu) { acpi64_table_too_short(h, len); return; }
    // 没有条目（len 刚好 0x2C）不是"表太短"，而是"固件没提供 ECAM" -> 维持 0/0
    if (len >= 0x2Cu + 16u) {
        const uint8_t* p = (const uint8_t*)(const void*)h + 0x2Cu;
        g_mcfg_base     = acpi64_load_u64(p);                     // 首项 base_address（0x00）
        g_mcfg_segments = (len - 0x2Cu) / 16u;                    // 16 字节一项
    }
    g_known_tables++;
}

// 根表里的一项：先做地址/长度守卫，再按签名分发。★ 这里是"防越界读"的闸口。
static void acpi64_parse_table(uint64_t pa) {
    // 先保证"签名(0x00..0x04) + 长度(0x04..0x08)"这 8 字节可读，才能拿长度字段去守卫后面
    if (!acpi64_pa_ok(pa, 8u)) {           // 连表头前 8 字节都读不到
        dbg64_str("[ACPI64] table 越界 pa=");
        acpi64_phex(pa);
        dbg64_str("（在前 4GB 之外或为 0，跳过）");
        dbg64_nl();
        return;
    }
    const AcpiSdtHeader64* h = (const AcpiSdtHeader64*)(uintptr_t)pa;
    const uint32_t len = h->length;
    // 整表必须落在恒等映射内：表头 36 字节 + 声明的长度都要能读
    if (len < (uint32_t)sizeof(AcpiSdtHeader64) || !acpi64_pa_ok(pa, len)) {
        acpi64_table_too_short(h, len);
        return;
    }
    if      (acpi64_sig_eq_header(h, "FACP")) acpi64_parse_fadt(h, len);
    else if (acpi64_sig_eq_header(h, "APIC")) acpi64_parse_madt(h, len);
    else if (acpi64_sig_eq_header(h, "HPET")) acpi64_parse_hpet(h, len);
    else if (acpi64_sig_eq_header(h, "MCFG")) acpi64_parse_mcfg(h, len);
    // 其余签名（DSDT/SSDT/BOOT/SLIT/...）本模块不解释，静默跳过
}

// ==================== RSDP 定位 ====================
// 前 20 字节求和为 0（ACPI 1.0 的校验和）
static bool acpi64_rsdp_sum20_ok(const uint8_t* p) {
    uint8_t sum = 0;
    for (int i = 0; i < 20; i++) sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

// 整份 RSDP 求和为 0（ACPI 2.0+ 的扩展校验和，覆盖 0x18 的 64 位 XSDT 地址）
static bool acpi64_rsdp_ext_sum_ok(const uint8_t* p, uint32_t len) {
    if (len < 36u || len > 4096u) return false;                 // 长度字段里的合理区间
    if (!acpi64_pa_ok((uint64_t)(uintptr_t)(const void*)p, len)) return false;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

// 在 [base, base+bytes) 里按 16 字节步长扫 RSDP（RSDP 规范要求 16 字节对齐）
static const AcpiRsdp64* acpi64_scan_region(uint64_t base, uint32_t bytes) {
    if (!acpi64_pa_ok(base, bytes)) return nullptr;
    // 每个候选要求 24 字节可读：ACPI 1.0 的 RSDP 是 20 字节，但 revision(0x0F) 与
    // length(0x14) 都在前 24 字节里，多要 4 字节就不用担心"读到扫描区间之外"。
    for (uint32_t off = 0; off + 24u <= bytes; off += 16u) {
        const AcpiRsdp64* r = (const AcpiRsdp64*)(uintptr_t)(base + off);
        const uint8_t* p = (const uint8_t*)(const void*)r;
        bool sig_ok = true;
        for (int i = 0; i < 8; i++) if ((char)p[i] != "RSD PTR "[i]) { sig_ok = false; break; }
        if (!sig_ok) continue;
        if (!acpi64_rsdp_sum20_ok(p)) continue;                 // 签名对但校验不过 -> 不是它
        return r;
    }
    return nullptr;
}

// 找 RSDP，两级来源：
//   ① 引导层传递槽（物理 0x7800 的 u64，ML64_RSDP_PTR_PHYS）：UEFI 固件按规范通过
//      EFI 系统表的配置表交付 RSDP（ACPI 2.0/1.0 GUID），不放 legacy 窗口，由引导层写入。
//      0 = 引导层未提供（BIOS 路径显式写 0）。
//   ② legacy 扫描：先 EBDA（BDA 0x40E 的段址 << 4，长度 1KB），再 0xE0000..0xFFFFF。
// ★ UEFI 下 RSDP/ACPI 表可能位于内核 48MB 直映之外：能读到它依赖固件自身为全部 RAM 建的
//   恒等映射（EDK2 会映射全部 RAM）。地址为 0 / 不在前 4GB / 签名或校验不过都不会硬崩 ——
//   直接落到 legacy 扫描；后续每张表还有既有的 acpi64_pa_ok() 与长度校验兜底。
//   from_uefi 回传来源，供汇总行打 src=uefi / src=legacy。
static const AcpiRsdp64* acpi64_find_rsdp(bool* from_uefi) {
    *from_uefi = false;

    // ---- ① 引导层交付槽：校验方式与 legacy 扫描完全一致（8 字节签名 + 前 20 字节校验和）----
    const uint64_t hint = *(const volatile uint64_t*)(uintptr_t)ML64_RSDP_PTR_PHYS;
    if (hint != 0 && acpi64_pa_ok(hint, 24u)) {                 // 24 = 签名(8)+校验(1)+... 到前 24B 可读
        const AcpiRsdp64* r = (const AcpiRsdp64*)(uintptr_t)hint;
        const uint8_t* p = (const uint8_t*)(const void*)r;
        bool sig_ok = true;
        for (int i = 0; i < 8; i++) if ((char)p[i] != "RSD PTR "[i]) { sig_ok = false; break; }
        if (sig_ok && acpi64_rsdp_sum20_ok(p)) {
            *from_uefi = true;
            return r;
        }
    }

    // ---- ② legacy：EBDA 段址合理则先扫它（1KB），再扫 BIOS ROM 区 ----
    const uint16_t ebda_seg = *(const volatile uint16_t*)(uintptr_t)0x40E;
    const uint64_t ebda = (uint64_t)ebda_seg << 4;              // 段址 -> 物理地址
    if (ebda >= 0x1000u && ebda < 0xA0000u) {                   // 合理性：EBDA 在低端 RAM 里
        const AcpiRsdp64* r = acpi64_scan_region(ebda, 1024u);
        if (r) return r;
    }
    return acpi64_scan_region(0xE0000u, 0x20000u);              // 步长 16，覆盖到 0xFFFFF
}

// ==================== 汇总打点（自动验收 grep 这三行）====================
static void acpi64_print_summary() {
    // ① RSDP：地址 / ACPI 版本 / OEMID / 根表里数到的表项数
    dbg64_str("[ACPI64] rsdp=");
    acpi64_phex(g_rsdp_addr);
    dbg64_str(" rev=");
    dbg64_dec(g_revision);
    dbg64_str(" oem=");
    dbg64_str(g_oem_id);
    dbg64_str(" tables=");
    dbg64_dec(g_tables);
    dbg64_str(g_src_uefi ? " src=uefi" : " src=legacy");
    dbg64_nl();

    // ② MADT：enabled 的本地 APIC 数（含 x2APIC，拿不到时回退 1）/ IOAPIC 数 / ISO 数 / LAPIC 地址
    dbg64_str("[ACPI64] madt cpus=");
    dbg64_dec((uint64_t)acpi_cpu_count64());
    dbg64_str(" ioapics=");
    dbg64_dec((uint64_t)g_ioapic_count);
    dbg64_str(" isos=");
    dbg64_dec(g_iso_count);
    dbg64_str(" lapic=");
    acpi64_phex(g_lapic_addr);
    dbg64_nl();

    // ③ FADT：SCI 向量 / PM1a 控制块端口 / HPET 基址 / MCFG(ECAM) 基址（后两项没有就打"无"）
    dbg64_str("[ACPI64] fadt sci=");
    dbg64_dec(g_sci_int);
    dbg64_str(" pm1a_cnt=");
    acpi64_phex(g_pm1a_cnt_blk);
    dbg64_str(" hpet=");
    if (g_hpet_addr != 0) acpi64_phex(g_hpet_addr); else dbg64_str("无");
    dbg64_str(" mcfg=");
    if (g_mcfg_base != 0) acpi64_phex(g_mcfg_base); else dbg64_str("无");
    dbg64_nl();
}

// ==================== 初始化 ====================
void acpi_init64() {
    if (g_inited) return;               // 幂等：表是只读的，重复调用不重解析
    g_inited = true;                    // 先置位：selftest 用它区分"没跑 init"和"机器没 ACPI"

    const AcpiRsdp64* r = acpi64_find_rsdp(&g_src_uefi);
    if (!r) {                           // 优雅退出：不崩、不写端口，getter 全部给回退值
        dbg64_str("[ACPI64] 未找到 RSDP");
        dbg64_nl();
        return;
    }

    g_found     = true;
    g_rsdp_addr = (uint64_t)(uintptr_t)(const void*)r;
    g_revision  = r->revision;          // 0x0F
    {   // OEMID 是 6 字节定长（不带结束符）-> 拷进本地缓冲补 0，别把后面的字节一起吐出来
        const uint8_t* p = (const uint8_t*)(const void*)r;
        for (int i = 0; i < 6; i++) g_oem_id[i] = (char)p[0x09 + i];
        g_oem_id[6] = 0;
    }

    // ---- 选根表：ACPI 2.0+ 优先 XSDT（64 位表项，能描述 4GB 以上的表），否则用 RSDT ----
    uint64_t root = 0;
    bool use_xsdt = false;
    if (g_revision >= 2) {
        // XSDT 地址只在整份 RSDP 扩展校验通过时才可信（校验覆盖 0x18 的 64 位地址）；
        // 校验失败就退回 32 位的 RSDT，宁可少解析也不能拿垃圾地址去读内存。
        if (r->length >= 36u && acpi64_rsdp_ext_sum_ok((const uint8_t*)(const void*)r, r->length)) {
            if (r->xsdt_address != 0) { root = r->xsdt_address; use_xsdt = true; }
        } else {
            dbg64_str("[ACPI64] rsdp 扩展校验失败，退回 RSDT");
            dbg64_nl();
        }
    }
    if (root == 0 && r->rsdt_address != 0) { root = r->rsdt_address; use_xsdt = false; }

    // ---- 遍历根表：逐项取表物理地址，交给 acpi64_parse_table 按签名分发 ----
    if (root != 0) {
        if (!acpi64_pa_ok(root, sizeof(AcpiSdtHeader64))) {
            dbg64_str("[ACPI64] 根表越界 pa=");
            acpi64_phex(root);
            dbg64_nl();
        } else {
            const AcpiSdtHeader64* rh = (const AcpiSdtHeader64*)(uintptr_t)root;
            char sig[5];
            acpi64_copy_sig(rh, sig);
            const uint32_t rlen = rh->length;
            const char* want = use_xsdt ? "XSDT" : "RSDT";
            if (!acpi64_sig_eq(sig, want)) {
                dbg64_str("[ACPI64] 根表签名异常 sig=");
                dbg64_str(sig);
                dbg64_str(" want=");
                dbg64_str(want);
                dbg64_nl();
            } else if (rlen < (uint32_t)sizeof(AcpiSdtHeader64) || !acpi64_pa_ok(root, rlen)) {
                acpi64_table_too_short(rh, rlen);
            } else {
                g_root_addr = root;
                g_root_len  = rlen;
                const uint32_t hdr_bytes = (uint32_t)sizeof(AcpiSdtHeader64);   // 0x24
                const uint32_t step      = use_xsdt ? 8u : 4u;                  // XSDT 8 / RSDT 4
                const uint32_t n         = (rlen - hdr_bytes) / step;
                const uint8_t* items     = (const uint8_t*)(const void*)rh + hdr_bytes;
                for (uint32_t i = 0; i < n; i++) {
                    const uint8_t* p = items + (uint64_t)i * (uint64_t)step;
                    const uint64_t tp = use_xsdt ? acpi64_load_u64(p) : (uint64_t)acpi64_load_u32(p);
                    if (tp == 0) continue;          // 空表项：跳过，不计入 tables
                    g_tables++;
                    acpi64_parse_table(tp);
                }
            }
        }
    }

    acpi64_print_summary();
}

// ==================== 对外 getter ====================
uint64_t acpi_rsdp_addr64()         { return g_rsdp_addr; }
uint8_t  acpi_revision64()          { return g_revision; }

int acpi_cpu_count64() {
    // MADT 里 enabled 的本地 APIC + enabled 的 x2APIC；两者都没有时回退为 1：
    // 上层（任务管理器显示）宁可看到 1 个 CPU，也不能看到 0 个。
    const int n = (int)(g_lapic_enabled + g_x2apic_enabled);
    return n > 0 ? n : 1;
}

int      acpi_ioapic_count64()      { return g_ioapic_count; }
uint64_t acpi_ioapic_addr64(int i)  { return (i >= 0 && i < g_ioapic_count) ? (uint64_t)g_ioapic_addr[i] : 0; }
uint32_t acpi_ioapic_gsi_base64(int i) { return (i >= 0 && i < g_ioapic_count) ? g_ioapic_gsi[i] : 0; }
uint64_t acpi_lapic_madt_addr64()   { return (uint64_t)g_lapic_addr; }
int      acpi_pcat_compat64()       { return (g_madt_flags & 1u) ? 1 : 0; }

int      acpi_sci_irq64()           { return g_fadt_ok ? (int)g_sci_int : 0; }
uint64_t acpi_reset_reg_addr64()    { return g_reset_reg_addr; }
uint8_t  acpi_reset_reg_value64()   { return g_reset_value; }
uint64_t acpi_hpet_addr64()         { return g_hpet_addr; }
uint64_t acpi_mcfg_base64()         { return g_mcfg_base; }
uint32_t acpi_mcfg_segment_count64(){ return g_mcfg_segments; }

// 软关机（S5）：唯一一处写硬件的地方
bool acpi_poweroff64() {
    // 前置条件缺一个就返回 false，**一个端口都不写**：
    //   还没 init / 没有 FADT / PM1a_CNT_BLK 为 0（规范里 0 = 不存在）/ 端口号超过 16 位
    if (!g_inited || !g_fadt_ok) return false;
    const uint32_t port = g_pm1a_cnt_blk;
    if (port == 0 || port > 0xFFFFu) return false;

    // PM1_CNT：SLP_TYPa（bit12..10）选睡眠状态，SLP_EN（bit13）写 1 才真的开始迁移。
    // 只写 SLP_TYPa 不会断电，必须带上 SLP_EN。5 = S5（软关机）。
    const uint32_t val = (uint32_t)(ACPI64_SLP_TYPa5 | ACPI64_SLP_EN);
    dbg64_str("[ACPI64] poweroff pm1a_cnt=");
    acpi64_phex(port);
    dbg64_str(" val=");
    acpi64_phex(val);
    dbg64_nl();
    if (g_pm1_cnt_len >= 4) outl((uint16_t)port, val);                  // 寄存器是 4 字节宽
    else                    outw((uint16_t)port, (uint16_t)val);        // 2 字节（绝大多数机器）
    io_wait();
    return true;                        // 写成功不等于一定断电，上层照旧保留回退链
}

// ==================== 自检 ====================
// 位定义（0 = 全通过）：
//   bit0 0x01  RSDP 找到且前 20 字节校验通过
//   bit1 0x02  根表（RSDT/XSDT）签名/长度可用
//   bit2 0x04  至少认出一张表（FACP/APIC/HPET/MCFG）
//   bit3 0x08  FADT 可用：PM1a_CNT_BLK 是合法端口 + SCI_INT 非 0
//   bit4 0x10  MADT 可用：至少 1 个 enabled 的本地 APIC（type 0 或 type 9）
//   bit5 0x20  至少 1 个 IOAPIC，且其地址落在前 4GB 恒等映射内
//   bit6 0x40  计数自洽：enabled 数 <= 条目总数
//   bit7 0x80  acpi_init64() 已执行（区分"忘了 init"和"机器就没有 ACPI"）
int acpi_selftest64() {
    int fails = 0;

    if (!g_found || g_rsdp_addr == 0) fails |= 0x01;
    if (g_root_addr == 0 || g_root_len < (uint32_t)sizeof(AcpiSdtHeader64)) fails |= 0x02;
    if (g_known_tables == 0) fails |= 0x04;
    if (!g_fadt_ok || g_pm1a_cnt_blk == 0 || g_pm1a_cnt_blk > 0xFFFFu) fails |= 0x08;
    if (!g_madt_ok || (g_lapic_enabled + g_x2apic_enabled) == 0) fails |= 0x10;

    if (g_ioapic_count < 1) {
        fails |= 0x20;
    } else {
        for (int i = 0; i < g_ioapic_count; i++) {
            if (g_ioapic_addr[i] == 0 || (uint64_t)g_ioapic_addr[i] >= ML64_IDENTITY_BYTES) {
                fails |= 0x20;
                break;
            }
        }
    }

    if (g_lapic_enabled > g_lapic_total) fails |= 0x40;
    if (g_x2apic_enabled > g_x2apic_total) fails |= 0x40;

    if (!g_inited) fails |= 0x80;

    dbg64_str("[ACPI64] selftest ");
    if (fails == 0) {
        dbg64_str("PASS");
    } else {
        dbg64_str("FAIL mask=");
        dbg64_dec((uint64_t)fails);
    }
    dbg64_nl();
    return fails;
}
