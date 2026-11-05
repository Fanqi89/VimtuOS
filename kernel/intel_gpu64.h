// intel_gpu64.h - P3：Intel 核显的**初步**驱动（本批只到"认得清 + 看得见"，不接管显示）
//
// ==================== 边界（先读，别误期待）====================
//   本批（v0.9.8）只做两件事：
//     ① 认卡：PCI device id -> 家族名（HD 530 / HD 620 / HD 630 / UHD 630 …）+ 代数；
//     ② 资源与 sanity：BAR0（寄存器窗口）与 BAR2 = GTTMMADR（GTT 表）的物理基址/大小，
//        再**只读**采样几十个 GTT 表项，数一数有多少项是 valid。
//   ★ 一个厂商寄存器都不写、一个 GTT 表项都不写、scanout 不接管、显存不分配、命令不提交。
//     也就是说：即使这里每一行判据都是错的，屏幕仍然由 fb.cpp/virtio_gpu64 那条既有路径出，
//     **不可能黑屏**。用户要的 `gpu=intel_safe` 就是这个模式，而且它是**默认**。
//
//   下一批（不在这批里）：GTT 分配 + 帧缓冲对象 + 显示引擎接管（pipe/plane 编程）。
//     那一步才会真正改画面，届时必须做成"失败立刻退回既有路径"。
//
// ==================== 开关（fw_cfg）====================
//   文件 `opt/vimtu/gpu`，串 `k=v;k=v`，键 `mode`：
//     mode=intel_safe （默认；没有该文件/没有该键/值不认识 都是它）  只读探测 + GTT 采样
//     mode=intel      预留给下一批（**本批仍然只读**，只是把 requested 如实打出来）
//     mode=off        完全不碰 Intel 卡（驱动行会写 reason=disabled-by-config）
#pragma once
#include <stdint.h>
#include "gpu64.h"       // Gpu64Dev64

// intel64_mode64() 的取值
static const int INTEL64_MODE64_SAFE = 0;   // intel_safe（默认）
static const int INTEL64_MODE64_FULL = 1;   // intel（本批仍只读）
static const int INTEL64_MODE64_OFF  = 2;   // off

// 对一张 Intel 显示设备做只读探测（打 [INTEL] 行）。
//   返回 1 = 这张卡由 Intel 驱动"接管探测"（[GPU] driver 行会写 status=safe-probe）；
//   返回 0 = 不是 Intel 卡，或配置要求 off（[GPU] driver 行会走统一回退）。
int intel64_probe64(const Gpu64Dev64* d);

// 自检：跑完全部设备后调用一次。没有任何 Intel 显示设备时如实打
//   `[INTEL] selftest skipped reason=no-intel-device`
// 返回 1 = 至少跑过一张 Intel 卡；0 = 跳过。
int intel64_selftest64(void);

// 当前模式（INTEL64_MODE64_*），已解析过就缓存，不重复读 fw_cfg。
int intel64_mode64(void);
