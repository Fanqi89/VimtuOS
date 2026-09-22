// console64.h - 开机滚屏引导控制台（boot console）+ 启动日志环形缓冲 + dmesg
//
// 需求（批次 N）：每次开机/重启**进入桌面之前**，把内核启动期打在串口上的日志
//   像 Linux 那样以滚动英文文本显示在屏幕上（"跑一次代码后才进入系统"的观感）。
//
// 数据流（一句话）：
//   dbg64_putc()（串口/调试口的**唯一出口**）-> con64 的 sink 逐字符攒行
//     -> 环形缓冲（16 KiB，保存最近的若干行；行内文本上限 160 字符，超出截断为 "..."）
//   fb_init()+font_init() 之后 -> con64_boot_screen64()：回放缓冲里已有的全部行（滚屏）
//     -> 实时追加新打点 -> **不停留**直接回到既有流程（向导 / 桌面）；回放期间按任意键可提前结束。
//
// ★ 为什么缓冲的状态放在 .data 而不是 .bss（关键踩坑点）：
//   kmain64() 在第 3 步会 bss_clear_64() 清整个 .bss（CPU 复位不清 RAM，必须自己清）。
//   本模块要在**清 .bss 之前**就开始缓存（[LM64] ENTERED LONG MODE 那几行也要有），
//   所以 g_ring64 用 section(".data.con64") 显式放进 .data —— 清 .bss 不会碰它。
//   而 dbg64.h 里的 sink 函数指针是零初始化（.bss），清完必须重新挂钩：见 con64_rehook64()。
//
// 串口打点（自动验收 grep，格式勿改；行锁保证不被抢占的行插进去）：
//   [CON64] ring init bytes=<n> lines=<n> text_max=<n>   环形缓冲就绪（bytes=容量，lines=按最大行长的行数容量）
//   [CON64] screen ready cols=<n> rows=<n>               屏幕控制台几何（等宽面 8px/字符）
//   [CON64] replay lines=<n> dropped=<n> ms=<n>         回放完成（lines=本次真的画上去的行数）
//   [CON64] live lines=<n> drawn=<n>                    进入实时追加模式（n=当前缓冲行数）
//   [CON64] skip key=1                                  用户在回放期间按键 -> 立即结束（不停留）
//   [CON64] verbose=0 skipped lines=<n>                 boot.verbose=0：不画屏（缓冲/dmesg 照常）
//   [CON64] selftest PASS mask=0x0                      / FAIL mask=0x<hex>
//   [CON64] dmesg lines=<n> head=<n> dropped=<n>        终端 dmesg：缓冲规模
//   [CON64] dmesg[<i>] <[    0.000000] 行正文>          终端 dmesg 的串口证据（有界：头尾各若干行）
//   [CON64] boot verbose=on|off persisted=<0|1>         终端 `boot verbose on|off`
#pragma once
#include <stdint.h>

// ==================== 尺寸 / 上限（也就是"有界"的来源）====================
#define CON64_RING_BYTES   16384   // 环形缓冲总字节数（规格：8~16 KiB；这里取 16 KiB）
#define CON64_TEXT_MAX     160     // 单行文本上限（含截断标记 "..."）：超出部分丢弃
#define CON64_HEAD_LINES   16      // 头部保留行数（见下）
#define CON64_HEAD_TEXT    160     // 头部保留区的单行上限
#define CON64_SCROLL_TOTAL_TICKS 1000  // 滚屏节奏的目标总时长（1000 tick ≈ 4000ms；全程都在滚、滚完不停留）
#define CON64_SCROLL_MAX_PER_LINE 20   // 每行最多等多少 tick（20 ≈ 80ms）；行多时降到 1 tick/行
#define CON64_REPLAY_MAX   64      // 一次最多回放多少行（★ 性能：只画最后 64 行 = 一屏多；更早的看终端 dmesg）

// ==================== 日志等级（屏幕着色 + dmesg 判定）====================
#define CON64_LV_INFO   0          // 浅灰
#define CON64_LV_WARN   1          // 黄
#define CON64_LV_ERROR  2          // 红

// ==================== 生命周期 ====================
// 最早初始化：kmain64() 里 dbg64_serial_init() 之后、bss_clear_64() **之前**调。
// 幂等：重复调用只重新挂钩 sink，不动缓冲内容（见 con64_rehook64）。
void con64_init64();
// bss_clear_64() 之后调：dbg64 的 sink 指针在 .bss 里，被清掉了要重新挂上（缓冲不受影响）。
void con64_rehook64();

// ==================== 查询（缓冲 / 头部保留 / dmesg）====================
int con64_inited64();                 // 1 = 已初始化
int con64_ring_bytes64();             // 环形缓冲字节数（16384）
int con64_text_max64();               // 单行文本上限（160）
int con64_buffered_lines64();         // 缓冲里当前行数（最近的 N 行）
int con64_dropped_lines64();          // 因缓冲满被覆盖丢弃的行数（累计）
int con64_head_lines64();             // 头部保留区里的行数（<= CON64_HEAD_LINES）
// 取缓冲里的第 i 行（0 = 最旧）。返回文本长度（0 = 没有该行）；tick/level 可传 nullptr。
int con64_line64(int i, uint64_t* tick, int* level, char* out, int out_max);
// 取头部保留区的第 i 行（0 = 最早的那一行，通常是 [LM64] ENTERED LONG MODE）。
int con64_trunc_lines64();            // 因超过 CON64_TEXT_MAX 被截断的行数（行尾有 "..."）
// 终端 `dmesg` 的数据源：dropped==0 时就是缓冲内容；dropped>0 时 = 头部保留行 + 省略标记 + 缓冲内容。
int con64_dmesg_count64();
int con64_dmesg_text64(int i, char* out, int out_max);        // 带 [    0.000000] 前缀的一行
int con64_dmesg_plain64(int i, char* out, int out_max);       // 不带前缀（标记行/判据用）
// `dmesg` 的串口证据（有界）：打印前 first_n 行 + 后 last_n 行，前缀 [CON64] dmesg[<i>]
void con64_dmesg_dump_serial64(int first_n, int last_n);

// ==================== 屏幕控制台 ====================
// 在安装向导 / 桌面之前调用（安装介质内核与系统内核各一处）：
//   1) 算几何 + 打 [CON64] screen ready；
//   2) [CON64] selftest；
//   3) 回放缓冲里的全部行（滚屏；期间按键 -> 立即结束回放）；
//   4) [CON64] live：停留期间新来的打点继续实时追加；
//   5) 有界停留 CON64_STAY_MS（任意键立即结束）。
// verbose=0：不画屏，只打 [CON64] verbose=0 skipped（缓冲/dmesg 照常工作）。
// 返回 1 = 用户按了键跳过。
int con64_boot_screen64(int verbose);

// ==================== 自检 ====================
// 返回失败掩码（0 = PASS）。打点 [CON64] selftest PASS mask=0x0 / FAIL mask=0x<hex>。
//   bit0 环形缓冲容量/初始化；bit1 缓存的早期行数 >= 10；bit2 时间戳格式；
//   bit3 等级判定（FAIL/WARN/默认）；bit4 屏幕几何（cols/rows 合理）。
int con64_selftest64();

// ==================== 小工具（自检 / 终端复用）====================
int con64_classify64(const char* s);            // 按关键字判等级：FAIL/PANIC -> 红，WARN -> 黄，其余 info
const char* con64_level_name64(int lv);         // "info" / "warn" / "error"
// Linux 风格时间戳：tick(250Hz) -> "[    0.123456]"（宽度 5+6，右对齐）
int con64_stamp64(uint64_t tick, char* out, int out_max);
