// panic64.h - VimtuOS 64 位蓝屏（BSOD）+ 看门狗
//
// 与 32 位 legacy32/kernel/panic.cpp 的关系（移植语义）：
//   32 位：panic_bsod(stop_code, eip) —— 画一屏 Win10 风格的错误界面（绿底白字），
//          5 秒进度后走状态机硬重启；wd_init/wd_kick/wd_expired —— 调度器检测"GUI 心跳超时"。
//   64 位：同样是"停止码 + 现场信息 + 整屏绘制"，但
//     * 屏幕是**蓝底白字**（本任务要求），文案里带上 64 位现场（异常号/err/RIP/CR2/状态机/
//       堆/任务/看门狗过期毫秒）；
//     * 受控演示（终端 panic/bsod）**停留 hold_ms 后停住**，不自动重启 —— 自动验收要在蓝屏画面
//       上做像素断言，重启会把画面冲掉（而且 QEMU 的 -no-reboot 会直接退出虚拟机）；
//     * 看门狗语义的"64 位对应物" = gui64 的**帧/tick 心跳**（32 位是 GUI 循环心跳，一一对应）：
//       gui64 主循环每帧调 panic64_watchdog_kick64()，看门狗任务每 250ms 检查一次，
//       超过 5000ms 没心跳 -> 打状态 + 进 BSOD。
//
// ★ 为什么看门狗的"armed"行前缀是 [WD64] 而不是 [PANIC64]：
//   多个既有验收脚本（tests/ui_extra64_test.py、tests/store64_test.py、tests/sched_stress_test.py）
//   把子串 "PANIC" 列为**禁止出现**（正常启动日志里不允许有 panic 字样）。看门狗 armed 是正常
//   启动路径的一部分，所以它必须用一个不含 "PANIC" 的前缀；真正的蓝屏行仍然用 [PANIC64]
//   （只在真的进 BSOD 时出现，与既有断言的语义一致）。
//
// 串口打点（自动验收 grep，格式勿改；行锁保证整行不被抢占）：
//   [WD64] watchdog task up id=N                       看门狗任务已创建
//   [WD64] watchdog armed timeout=5000ms source=gui64-frame   已武装（心跳源 = gui64 每帧）
//   [WD64] watchdog beat stale=0ms checks=N            每 ~10 秒一行（证明看门狗在跑且没超时）
//   [WD64] watchdog fire stale=NNNNms                 真的超时了（正常启动日志里不该出现）
//   [WD64] watchdog suspended / resumed                停止/重启期间挂起与恢复
//   [PANIC64] stop=<code> detail=0x<hex>               进入 BSOD（停止码 + 现场）
//   [PANIC64] screen w=1280 h=800 bg=0x000078D4 lines=N  屏幕已画好（像素断言的坐标依据）
//   [PANIC64] hold=6000ms                             受控演示停留时长
//   [PANIC64] halt (power cycle required)             停住（不再返回）
#pragma once
#include <stdint.h>

// ---- 看门狗 ----
#define WD64_TIMEOUT_MS   5000u    // 心跳超时阈值（与 32 位一致：5 秒）
#define WD64_POLL_MS      250u     // 检查周期

void     panic64_init64();                       // 建看门狗任务（在 task_start64() 之后调用）
void     panic64_watchdog_arm64();               // 武装（桌面首帧时调用）
void     panic64_watchdog_kick64();              // 心跳（gui64 主循环每帧）
void     panic64_watchdog_suspend64();           // 挂起（停止/重启动画期间）
void     panic64_watchdog_resume64();            // 恢复（重新计数）
void     panic64_watchdog_pause64();             // 临时停表（长操作，如 ping 最多 8 秒）
void     panic64_watchdog_unpause64();
uint32_t panic64_watchdog_stale_ms64();          // 当前心跳落后毫秒数
int      panic64_watchdog_armed64();
int      panic64_watchdog_fires64();             // 触发次数（正常情况下 0）
int      panic64_active64();                     // 1 = 已经在 BSOD 里

// ---- BSOD ----
// 受控演示（终端 `panic <code>` / `bsod <code>`）：画蓝屏 + 串口现场 + 停留 hold_ms -> 停住。
// 不返回。hold_ms = 0 时用默认 6000ms。
[[noreturn]] void panic64_controlled64(const char* code, uint32_t hold_ms);
// 内部公共路径：画屏 + 打点 + 停住（detail 是现场附加数值，可为 0）
[[noreturn]] void panic64_bsod64(const char* code, uint64_t detail);
// 真实 CPU 异常路径：kernel/x86_64.cpp 的 [PANIC] cpu exception 打完之后调用这里接着画 BSOD。
// 用 weak 引用（安装程序内核不链 panic64.cpp，链接期不能因此失败）。
extern "C" void panic64_cpu_exception64(uint64_t int_no, void* frame);
