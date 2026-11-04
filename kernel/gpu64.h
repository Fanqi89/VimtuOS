// gpu64.h - 通用 GPU 层（P1）：显示类 PCI 设备枚举 + 厂商识别 + BAR/显存窗口 + 命令寄存器 + 只读探测
//
// 定位与边界（先读，别误期待）：
//   * 本层**只做"认得出一张显卡"这件事**：扫 PCI class 0x03、认厂商、读出 BAR 与真实窗口大小、
//     打开 mem-decode + bus-master、对三家（Intel / AMD / NVIDIA）各读几个**只读**寄存器做身份确认，
//     最后打一份 `[GPU]` 报告。**没有任何一行**模式设置（KMS）/ 显存管理 / 命令提交 / 2D、3D 加速。
//   * 上屏路径一个字节都不走这里：真实的画面仍然由 kernel/fb.cpp（软件 LFB 逐像素）或
//     kernel/virtio_gpu64.cpp（virtio-gpu 2D 设备路径）负责，本层不接管 scanout。
//   * 厂商原生的模式设置/显存/提交按用户对齐的分期往后走：P2 = AMD（SMU/DCN）、P3 = Intel
//     （display engine/GTT）、P4 = NVIDIA（nouveau 路线，法律风险最高）。P1 只是它们共同的地基。
//
// 分期验收口径（别把 P1 说成"驱动做完了"）：同一份构建在三种图形后端（`-vga std` / virtio-vga /
//   qxl）各冷启动一次都能打出 `[GPU]` 行、未知厂商**如实降级**、桌面照常 `[GUI64] ready`。
#pragma once
#include <stdint.h>

// 最多记录几张卡（QEMU "多张显卡" 夹具最多塞 3~4 张；8 已经远超需要）
static const int GPU64_MAX64 = 8;

// 厂商分类（`Gpu64Dev64::vendor_tag` 的取值；唯一真源在 gpu64.cpp 的 gpu64_vclass64）
static const uint8_t GPU64_VENDOR64_UNKNOWN = 0;
static const uint8_t GPU64_VENDOR64_INTEL   = 1;   // PCI vendor 0x8086
static const uint8_t GPU64_VENDOR64_AMD     = 2;   // PCI vendor 0x1002（图形）/ 0x1022（同门芯片组）
static const uint8_t GPU64_VENDOR64_NVIDIA  = 3;   // PCI vendor 0x10DE

// 一张显示类设备的记录。字段语义按注释，不要在别处重复实现分类逻辑。
struct Gpu64Dev64 {
    uint32_t bdf;         // (bus<<8)|(dev<<3)|fn —— 与 kernel/pci64.h 的打包**同一套**
    uint16_t vendor;      // PCI vendor id（原始值）
    uint16_t device;      // PCI device id（原始值）
    uint32_t class_reg;   // 配置空间 0x08 的原始 dword（revision | prog-if | subclass | class）
    uint8_t  index;       // 本设备在 g_dev[] 里的下标（== 打点里的 idx=）
    uint8_t  vendor_tag;  // GPU64_VENDOR64_*
    uint8_t  known;       // 1 = 认得厂商（Intel/AMD/NVIDIA）；0 = unknown（如实降级）
    uint8_t  bar0_ok;     // 1 = BAR0 是 MMIO 且落在 4GiB 恒等映射里（可以安全解引用）
    uint8_t  bar0_io;     // 1 = BAR0 是 I/O 端口 BAR（本层不用，只报告）
    uint8_t  bar0_64;     // 1 = BAR0 是 64 位地址 BAR
    uint8_t  cmd_ok;      // 1 = mem-decode(bit1) 与 bus-master(bit2) 都已在命令寄存器里置 1
    uint8_t  probe_ok;    // 1 = 厂商专用只读探测真的读到了东西（probe0/probe1/probe2 有效）
    uint16_t cmd_word;    // 命令寄存器（0x04 低 16 位）**回读**值（cmd_ok 由它算，原始值留档）
    uint64_t bar0_phys;   // BAR0 物理基址（bar0_ok=1 时有效）
    uint64_t bar0_size;   // BAR0 真实窗口大小（写全 1 回读法；bar0_ok=1 时有效）
    uint64_t bar2_phys;   // BAR2 基址（AMD 的 MM_INDEX/MM_DATA 间接窗口；0 = 没有）
    uint32_t probe0;      // 厂商探测寄存器 0（NVIDIA=NV_PMC_BOOT_0 / AMD=R_000000 / Intel=PIPEA_CONF）
    uint32_t probe1;      // 厂商探测寄存器 1（Intel=PIPEA_DSL / AMD=MM_INDEX / 其余 0）
    uint32_t probe2;      // 厂商探测寄存器 2（Intel=DSPACNTR / AMD=MM_DATA / 其余 0）
};

// 枚举 + 识别 + 只读探测 + 打点。可重复调用（内部只做一次，第二次起直接返回）。
//   调用点：kernel/kernel64.cpp 里 **fb_init() 之后、vgpu64_init64() 之前**。
//   为什么在那儿：本层会临时改写 BAR（探窗口大小），那段时间设备的地址译码是关的；
//   放在画面上什么东西都还没画之前，风险最小（实现里还会再关一次中断，见 gpu64.cpp 顶部）。
void gpu64_init64(void);

// 找到的显示类设备数量（0 = 一张都没有/没有 PCI），下标 0..n-1
int gpu64_count64(void);

// 取第 idx 张卡的记录；越界返回 nullptr
const Gpu64Dev64* gpu64_dev64(int idx);

// PCI vendor id -> "intel" / "amd" / "nvidia" / "unknown"（纯字符串，无分配）
const char* gpu64_vendor_name64(uint16_t pci_vendor);

// 已经认得的卡数量（known），以及未知厂商的卡数量 —— 报告口径，供上层/验收复用
int gpu64_known_count64(void);
int gpu64_unknown_count64(void);
