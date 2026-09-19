// preload64.h - VimtuOS 64 位系统级预加载（字形预光栅化 + 桌面图标预缩放 + 实测证据）
//
// 与 32 位 legacy32/kernel/preload.cpp 的关系（语义移植，不照抄代码）：
//   * 只做"确定性收益"的预热：把**首次绘制才做**的两件重活提前到进桌面之前：
//       1) 界面/终端常用字形（ASCII 三个 face + 常用汉字）预光栅化进 font.cpp 的字形缓存；
//       2) 桌面图标（3 张 128x128）+ 开始按钮图标（64x64）预缩放到显示尺寸的位图缓存
//          （gui64.cpp 原来**每帧**对源图做最近邻缩放）。
//   * 必须能证明有效：进桌面之前用 rdtsc64 对"首帧代表工作"（一段界面文本的真实绘制 +
//     一个桌面图标的真实缩放绘制）各测一次——预热前（冷，现场光栅化/缩放）与预热后
//     （热，缓存命中只做混合）。数字是**实测**的，不做任何修饰。
//   * 32 位的界面上有"系统设置 ui.preload"开关；64 位暂不接该开关（只有启动期一次调用），
//     这里如实说明，不假装有开关。
//
// 串口打点（自动验收 tests/preload_update_test.py grep，格式勿改）：
//   [PRELOAD64] init cache_cap=<n> lru_cap=<n> cn_unique=<n>
//   [PRELOAD64] glyphs prewarmed=<n> ms=<n>
//   [PRELOAD64] icons cached=<n>
//   [PRELOAD64] first paint before=<n> after=<n> cycles
//   [PRELOAD64] done glyphs_new=<n> cached=<n>/<cap> cn_unique=<n> icons=<n> ms=<n> runs=<n>
#pragma once
#include <stdint.h>

// 预热统计（终端 `preload` 命令 / 测试读它；纯值拷贝）
struct Preload64Stats {
    uint32_t runs;             // 预热跑过几轮
    int      glyphs_new;       // 本**轮**新光栅化的字形数（第二轮起为 0）
    int      glyphs_cached;    // 预热后缓存里的字形总数
    int      cache_cap;        // 缓存容量上限（font_cache_capacity）
    int      lru_cap;          // 每 face 的 CJK LRU 槽数（font_lru_capacity）
    int      cn_unique;        // 预热汉字串里的唯一汉字数（必须 <= lru_cap 才幂等）
    int      icons_cached;     // 预缩放好的图标位图数（正常 = 4）
    uint32_t ms_glyphs;        // 字形预热耗时（ms；由 TSC 校准换算，口径见 .cpp）
    uint32_t ms_total;         // 全流程耗时（ms）
    uint64_t paint_before;     // 预热前"首帧代表工作"的 cycles（真实测量）
    uint64_t paint_after;      // 预热后同一工作量的 cycles（真实测量）
};

// 初始化：打印 init 打点 + 向 sysstate64 注册 "preload64" 模块。
// 位置要求：os_boot_path 里 sysstate64_start64() **之前**（STARTING 复核会检查本模块）。
void preload64_init64();

// 预热一轮（幂等：字形已缓存的不会重复计入）。返回本**轮**新光栅化的字形数。
// 位置要求：进桌面（gui64_run）之前。
int preload64_run64();

const Preload64Stats* preload64_stats64();
int  preload64_glyphs_new64();     // 最近一轮新光栅化的字形数
int  preload64_glyphs_cached64();  // 缓存里的字形总数
int  preload64_icons_cached64();   // 预缩放图标数
uint32_t preload64_ms64();         // 最近一轮总耗时（ms）

// 终端 `preload` 命令用的一行式报告（不分配、无 libc；写入 out，NUL 结尾）
void preload64_report64(char* out, int maxlen);
