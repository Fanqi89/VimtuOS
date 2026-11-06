// kernel/gpu64_stub64.cpp - **安装程序内核**的通用 GPU 层弱兜底（构建修复，不是新功能）
//
// 为什么需要它
// ------------
// kernel/kernel64.cpp 是**两份内核共用**的一份源码：它在 fb_init() 之后调 gpu64_init64()。
// 系统内核里由 kernel/gpu64.cpp 实现；但安装程序内核（build64/kernel64.bin）那一遍**不编也不链**
// gpu64.cpp —— 安装介质只是一块装盘向导界面，不需要 GPU 报告，也不该为它多付几百字节
// （见 build64.sh：gpu64.cpp 加在 SRCS_OS 而不是 SRCS_CORE）。结果是安装程序内核链接期报
//   ld.lld: error: undefined symbol: gpu64_init64()
//
// 做法（为什么是 weak）
// --------------------
// 与 kernel/hda64_stub64.cpp 完全同一个套路：weak 空实现让构建先过，且**自愈**——
//   * 现在：安装程序内核用弱兜底（一个空函数 + 如实返回"没有卡"的查询），安装程序里本来
//     也没有任何一处读 GPU 信息；
//   * 以后：哪天有人把 kernel/gpu64.cpp（强定义）也加进安装程序内核的编译/链接，强定义会
//     **直接覆盖**弱兜底，不出重复符号错，这个文件自动变死代码（可删）。
//
// 不改的那条纪律：本文件没有任何新功能、没有策略，也不假装"有显卡"——count 一律 0。
#include "gpu64.h"

__attribute__((weak)) void gpu64_init64(void) {}

__attribute__((weak)) int gpu64_count64(void) { return 0; }
__attribute__((weak)) int gpu64_known_count64(void) { return 0; }
__attribute__((weak)) int gpu64_unknown_count64(void) { return 0; }

__attribute__((weak)) const Gpu64Dev64* gpu64_dev64(int idx) {
    (void)idx;
    return nullptr;                 // 安装程序内核里没有 GPU 记录表
}

__attribute__((weak)) const char* gpu64_vendor_name64(uint16_t pci_vendor) {
    (void)pci_vendor;
    return "unknown";               // 不编造厂商名
}

// ★ v0.9.8：P3 Intel 初步驱动（kernel/intel_gpu64.cpp）同样**只进系统内核**。gpu64.cpp 会调
//   intel64_probe64()/intel64_selftest64()，而 gpu64.cpp 自己也不在安装程序内核里 —— 所以这两条
//   严格来说不会被引用。仍然给 weak 兜底：万一将来有人只把 gpu64.cpp 加进安装内核，链接期
//   会立刻炸，而这里让它"编得过、且如实返回没有 Intel 驱动"，与上面几条同一个套路。
//   语义：没有任何新功能、没有策略、不假装有卡（一律返回 0 = 没接管 / 跳过自检）。
__attribute__((weak)) int intel64_probe64(const Gpu64Dev64* d) { (void)d; return 0; }
__attribute__((weak)) int intel64_selftest64(void) { return 0; }
__attribute__((weak)) int intel64_mode64(void) { return 0; }   // 0 = INTEL64_MODE64_SAFE

// ★ v0.9.12：kernel/drm64.cpp（DRM/KMS + evdev 门面）同样**只进系统内核**（见 build64.sh：
//   它要 proc64 的进程/页表面、fb 的后备缓冲物理页、virtio-gpu 的传输路径 —— 安装介质里
//   一样都没有）。syscall64.cpp / fd64.cpp 对这几条是 weak 引用：这里给空实现，让
//   "哪天有人把 drm64.cpp 也加进安装内核"时链接期立刻能过；语义上一律如实失败/空操作，
//   **不假装有 DRM、也不假装能读输入事件**。
__attribute__((weak)) void drm64_init64(void) {}
__attribute__((weak)) int  drm64_selftest64(void) { return -1; }        // -1 = 这台内核没有 DRM 门面
__attribute__((weak)) int  drm64_ioctl64(uint64_t, uint64_t, uint64_t) { return -25; }   // -ENOTTY
__attribute__((weak)) int64_t evdev64_read64(int, void*, uint64_t) { return -38; }       // -ENOSYS
__attribute__((weak)) int  evdev64_ioctl64(int, uint64_t, uint64_t) { return -25; }      // -ENOTTY
__attribute__((weak)) int  drm64_demo64(const char*) { return 0; }                       // 没有演示
