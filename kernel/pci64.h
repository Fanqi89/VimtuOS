// pci64.h - PCI 配置空间 + BAR 解码（最小公共实现；用户态设备映射 pci_map_bar(22) 的底层）
//
// 为什么单独有这么一个文件（而不是复用某个驱动里的那份）：
//   kernel/ahci64.cpp / e1000_64.cpp / hda64.cpp / nvme64.cpp / usb64.cpp / hwui64.cpp /
//   hwinfo64.cpp 各自都有一份**私有 static** 的 0xCF8/0xCFC 访问器（各自前缀 apci_/epci_/hpci_/…），
//   它们互不可见、也不该被改动（驱动内部实现不在本批可改范围）。本文件是**新增**的、只服务
//   "从用户态安全地映射一个设备 BAR" 这件事的最小集合：
//     * 配置空间读写（同款 0xCF8/0xCFC 机制，16/32 位）；
//     * BAR 解码：I/O vs MMIO、64 位 BAR 的高 32 位、**真实大小**（写全 1 回读法）。
//
// ★ 与内核既有驱动的唯一冲突面（如实写在这里，不藏）：
//   BAR 的**大小**只能靠"写全 1 -> 回读 -> 还原"探出来，那一刻设备的地址译码被临时改掉。
//   本文件把风险压到最小：
//     ① 只对**内存 BAR** 做（I/O BAR 连写都不写：低字节 bit0=1 直接判掉）；
//     ② 写完立刻按原值还原，并**回读确认**还原成功（对不上就报 ENODEV，绝不把设备地址留在错值上）；
//     ③ 结果进 8 条缓存（pci64_bar_cache64）：同一个 (bdf, bar) 第二次调用**不再动寄存器**。
//   即便如此，"内核驱动正在用这个 BAR 时被我们临时改一下译码"仍是一个**真实存在的竞态**，
//   见 docs/应用层与系统调用说明.md 的"用户态设备映射与驱动服务骨架"一节（共存风险逐条列出）。
#pragma once
#include <stdint.h>

// BDF 编码（内核内部与用户 ABI **同一套**）：bdf = (bus << 8) | (dev << 3) | (fn)
//   用整数而不是结构体的理由：系统调用只有 4 个通用参数，打包成一个 u64 最省。
static const uint32_t PCI64_BDF64_MAX64 = (255u << 8) | (31u << 3) | 7u;   // 0xFFFF

// 错误码：与 kernel/syscall64.h 的 PCIMAP64_* 是同一张表（负数，rax 返回）
static const int PCI64_EPERM64  = -1;   // 权限/上下文（没有进程上下文 —— 这里是内部防御）
static const int PCI64_EFAULT64 = -2;   // 用户指针非法
static const int PCI64_EINVAL64 = -3;   // bdf/bar_index 不合法，或该 BAR 不是 MMIO（是 I/O 端口）
static const int PCI64_ENOMEM64 = -4;   // 映射窗满 / 页表页不足 / 映射落地失败
static const int PCI64_ENODEV64 = -5;   // 没有这个设备 / 该 BAR 未实现 / BAR 在恒等映射之外

// BAR 探测结果（只对内存 BAR 有意义）
struct Pci64Bar64 {
    uint64_t phys;      // 物理基址（64 位 BAR 已合并高 32 位；已按实际大小对齐）
    uint64_t size;      // **实际大小**（写全 1 回读法；2 的幂、>= 16 B）
    uint8_t  index;     // BAR 序号（0..5）
    uint8_t  is_io;     // 1 = I/O 端口 BAR（此时 phys/size 无意义，返回 -EINVAL）
    uint8_t  is_64;     // 1 = 64 位地址 BAR（占两个 BAR 槽）
    uint8_t  ok;        // 1 = 上面三个字段有效
};

// 配置空间访问（bdf 已打包；off 会被对齐到 4 字节）
uint32_t pci64_cfg_rd32(uint32_t bdf, uint8_t off);
void     pci64_cfg_wr32(uint32_t bdf, uint8_t off, uint32_t val);
uint16_t pci64_cfg_rd16(uint32_t bdf, uint8_t off);

// 1 = 该 BDF 上有设备（vendor id 既不是 0xFFFF 也不是 0）；0 = 没有
int pci64_dev_present64(uint32_t bdf);

// BAR 解码：0 = 成功（out 已填、可直接映射）；负数 = PCI64_* 错误码。
//   * bar_index > 5 / bdf 编码非法 -> -EINVAL
//   * 没有设备 / BAR 未实现（读回 0）/ 地址在恒等映射之外（>= 4GiB）-> -ENODEV
//   * BAR 是 I/O 端口（低 bit0 = 1）-> -EINVAL（is_io=1）
int pci64_bar_probe64(uint32_t bdf, uint8_t bar_index, Pci64Bar64* out);
