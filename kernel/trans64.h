// trans64.h - ★ v0.9.3：统一的状态机 + 基于 delta_time 的时间轴（界面过渡动画共用）
//
// 为什么要有这个文件（用户需求原文）：
//   "请用统一的状态机和基于 delta_time 的时间轴来驱动：第一段（锁屏→登录界面）300 毫秒 …
//    第二段（登录界面→主桌面）400 毫秒 …"
//
// 用法（两段过渡共用同一套状态机）：
//     Trans64 tr; trans64_begin(&tr, 300);
//     for (;;) { int p = trans64_p256(&tr); …; if (trans64_done(&tr)) break; }
//
// ★ delta_time 的两个坑（本内核实测，别改成"数帧"）：
//   1) 时长必须按**墙钟**算：TCG 下 PIT 的 250Hz 虚拟时钟比宿主墙钟快 1.6~1.9 倍
//      （v0.9.2 实测 rps=1574~1944），按 tick 数出来的 400ms 屏幕上只有 ~210ms；
//   2) 不能按帧数推进：TCG 全屏合成一帧可能 100~300ms，帧率不稳 → 动画时长会跑飞。
//   所以这里把"tick 差"用 RTC 校准出来的 rps 换算成墙钟毫秒（wall64_ratio_x1000()）。
#pragma once
#include <stdint.h>

// （自包含的最小实现：只依赖 x86_64.h 的 g_ticks64 / PIT_HZ_64 与 wall64_ratio_x1000()）
#include "x86_64.h"
#include "theme64.h"     // theme64_ease64（ease-out 曲线）

typedef struct Trans64 {
    uint32_t t0;         // 起始 tick
    int      dur_ms;     // 目标墙钟时长（需求里的 300 / 400）
    int      rps;        // 每个墙钟秒对应多少 guest ms（1000 = 原生；TCG ~1500-2000）
    int      frames;     // 已经画了多少帧
    int      last_p;     // 上一次的进度（0..256），避免重复绘制同一进度
} Trans64;

static inline void trans64_begin(Trans64* t, int dur_ms) {
    int rps = wall64_ratio_x1000();
    if (rps < 300 || rps > 8000) rps = 1000;          // 拿不到锚点：按原生走（如实降级）
    t->t0 = (uint32_t)g_ticks64;
    t->dur_ms = dur_ms > 0 ? dur_ms : 1;
    t->rps = rps;
    t->frames = 0;
    t->last_p = -1;
}

// 已经过去的**墙钟**毫秒（delta_time 的唯一来源）
static inline int trans64_dt_ms(const Trans64* t) {
    const uint64_t dt_guest = (uint64_t)((uint32_t)g_ticks64 - t->t0) * (uint64_t)TICK_MS_64;
    return (int)(dt_guest * 1000u / (uint64_t)(t->rps > 0 ? t->rps : 1000));
}

// 进度：0..256，已经过 theme64_ease64()（ease-out：开头快、结尾慢）
static inline int trans64_p256(const Trans64* t) {
    const int dt = trans64_dt_ms(t);
    if (dt <= 0) return 0;
    if (dt >= t->dur_ms) return 256;
    int tt = (int)((int64_t)dt * 256 / t->dur_ms);
    if (tt < 0) tt = 0;
    if (tt > 256) tt = 256;
    const int e = theme64_ease64(tt);
    return e < 0 ? 0 : (e > 256 ? 256 : e);
}

static inline int trans64_done(const Trans64* t) { return trans64_dt_ms(t) >= t->dur_ms; }
