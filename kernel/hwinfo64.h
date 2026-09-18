// hwinfo64.h - 硬件详情（CPU / PCI / 磁盘）公共接口
//
// 定位：给任务管理器 / 设置页面用的"静态硬件清单"。启动期只探测一次，结果以 POD
//       结构体整体暴露（hw_info64()），无堆分配、无锁、只读。
//
// 推荐接线顺序（细节见 hwinfo64.cpp 顶部）：
//     hwinfo_init64();              // CPUID + PCI 枚举，并打 [HW64] 串口日志
//     (void)hwinfo_selftest64();    // 打 [HW64] selftest PASS/FAIL；返回 0 = 全通过
//     hwinfo_set_disk64(i, model, sectors_512, lba48);   // 由 part64/ata64 侧填磁盘信息
//
// 约定：
//   * 字符串一律定长 char 数组 + NUL 结尾，容量宏写死在这里（探测端保证不越界写）。
//   * 探测失败"如实降级"：字符串填 "none"，数字填 0；绝不崩、绝不返回半截数据。
//   * 结构体全是 POD（无构造/析构/虚表），可以静态零初始化、直接放在 .bss。
//   * 只读探测：PCI 枚举只往 0xCF8 写地址、从 0xCFC 读数据，**不写任何设备配置寄存器**。
#pragma once
#include <stdint.h>

// ---- 定长字符串容量（含结尾 NUL）----
#define HW64_CPU_VENDOR_MAX  13u   // CPUID 0：12 字符 + NUL
#define HW64_CPU_BRAND_MAX   49u   // CPUID 0x80000002..4：48 字符 + NUL
#define HW64_HYP_VENDOR_MAX  13u   // CPUID 0x40000000：12 字符 + NUL（无虚拟机则 "none"）
#define HW64_DISK_MODEL_MAX  41u   // 与 ata64.h 的 DiskInfo::model[41] 对齐

// ---- 表容量 ----
#define HW64_PCI_MAX   32u   // 只登记前 32 个 PCI 设备（类计数不受此上限影响）
#define HW64_DISK_MAX   8u   // 最多登记 8 块磁盘

// hwinfo_init64() 跑过之后写进 HwInfo64::magic；自检用它判断"探测是否已发生"
#define HW64_INFO_MAGIC  0x48573436u   // 'HW46'

struct HwCpu64 {
    char     vendor[HW64_CPU_VENDOR_MAX];      // "GenuineIntel" / "AuthenticAMD" / "none"
    char     brand[HW64_CPU_BRAND_MAX];        // 完整品牌串（CPUID 0x80000002..4）
    char     hypervisor[HW64_HYP_VENDOR_MAX];  // "VMwareVMware" / "KVMKVMKVM" / "Microsoft Hv" / "TCGTCGTCGTCG" / "none"
    uint32_t stepping;                  // CPUID 1 EAX[3:0]
    uint32_t model;                     // 解码后的显示 model
    uint32_t family;                    // 解码后的显示 family
    uint32_t cores;                     // 逻辑核数（CPUID 1 EBX[23:16]，最小 1）
    uint32_t leaf_max;                  // CPUID 0 EAX（最大基本叶号）
    uint32_t leaf_ext_max;              // CPUID 0x80000000 EAX（最大扩展叶号）
    uint32_t reg1_ecx;                  // CPUID 1 ECX 原始值（AVX/VMX/hypervisor 位）
    uint32_t reg1_edx;                  // CPUID 1 EDX 原始值（SSE/SSE2/PAE/HTT 位）
    uint32_t reg7_ebx;                  // CPUID 7.0 EBX 原始值（SMEP 位；叶不存在时为 0）
    uint32_t regext1_edx;               // CPUID 0x80000001 EDX 原始值（NX/LM 位）
    bool     smp;                       // 逻辑核 > 1（或 HTT 位）
    bool     pae;
    bool     nx;                        // No-Execute（扩展叶 0x80000001）
    bool     sse;                       // SSE（CPUID 1 EDX bit25）
    bool     sse2;
    bool     avx;
    bool     vmx;                       // Intel VT-x / AMD SVM（CPUID 1 ECX bit5）
    bool     smep;                      // 管理模式执行保护（CPUID 7.0 EBX bit7）
    bool     lm;                        // long mode（扩展叶 0x80000001 EDX bit29）
};

// 一个 PCI 设备（只保存枚举用到的只读字段）
struct HwPciDev64 {
    uint8_t  bus;          // 0..255
    uint8_t  dev;          // 0..31
    uint8_t  fn;           // 0..7（多功能设备）
    uint8_t  class_code;   // 类（配置空间 offset 0x0B）
    uint8_t  subclass;     // 子类（offset 0x0A）
    uint8_t  rev;          // 版本（offset 0x08）
    uint8_t  pad[2];       // 对齐填充（保持 sizeof 稳定，便于将来扩展）
    uint16_t vendor;       // offset 0x00
    uint16_t device;       // offset 0x02
};

struct HwPci64 {
    uint32_t  count;      // 已登记设备数（<= HW64_PCI_MAX）
    uint32_t  scanned;    // 枚举到的设备总数（不受登记上限影响）
    uint32_t  bus_max;    // 发现设备的最大总线号（0 = 只有总线 0 有设备）
    uint32_t  ide;        // class 0x01 subclass 0x01（IDE 控制器）
    uint32_t  storage;    // class 0x01 全部（含 IDE/AHCI/RAID）
    uint32_t  net;        // class 0x02（网络控制器）
    uint32_t  vga;        // class 0x03（显示控制器）
    HwPciDev64 devs[HW64_PCI_MAX];
};

struct HwDisk64 {
    bool     present;                   // false = 该槽位没有盘（model 为空串）
    bool     lba48;                     // 填表方告知是否支持 LBA48
    uint8_t  pad[6];                    // 对齐填充
    uint64_t sectors_512;               // 总扇区数（512B/扇区）
    char     model[HW64_DISK_MODEL_MAX];// 型号字符串（NUL 结尾）
};

struct HwInfo64 {
    uint32_t  magic;       // == HW64_INFO_MAGIC 表示 hwinfo_init64() 已跑过
    uint32_t  disk_count;  // 实际登记（present）的磁盘数
    HwCpu64   cpu;
    HwPci64   pci;
    HwDisk64  disks[HW64_DISK_MAX];
};

#ifdef __cplusplus
extern "C" {
#endif

// CPU + PCI 探测，并按固定格式打 [HW64] 串口日志。可重复调用（幂等，会重新探测）。
// 磁盘表不在这里重置：由 part64/ata64 侧通过 hwinfo_set_disk64() 填。
void hwinfo_init64();

// 自检：返回失败位掩码，0 = 全部通过。内部会打印
//   [HW64] selftest PASS   或   [HW64] selftest FAIL mask=<n>
// 若还没跑过 hwinfo_init64()，会先自动补一次探测，所以单独调用也安全。
// 位定义：1=CPUID 不可用 2=vendor 非法 4=brand/hyp 超长 8=核数异常
//         16=特征解码与原始寄存器不一致 32=PCI 表不一致 64=磁盘表不一致 128=初始化/API 异常
int hwinfo_selftest64();

// 只读访问整份硬件清单（静态存储，永不为空；未初始化时 magic != HW64_INFO_MAGIC）。
const HwInfo64* hw_info64();

// 磁盘信息由外部（part64 / ata64 侧）填：idx 0..HW64_DISK_MAX-1。
//   model == nullptr 或空串 -> 清掉该槽位（present = false）。
//   sectors_512 为该盘总扇区数；lba48 非 0 表示支持 LBA48。
// 越界 idx 静默忽略（调用方 bug 不该把内核打挂）。调用方应把重试/失败后的
// **最终结果**填进来；本模块不碰 ATA 端口、不读盘。
void hwinfo_set_disk64(int idx, const char* model, uint64_t sectors_512, int lba48);

#ifdef __cplusplus
}
#endif
