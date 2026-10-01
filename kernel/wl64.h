// wl64.h - ★ A5：Wayland 基础骨架（surface / buffer / commit / seat / 合成器）
//
// ============================ 这是什么、不是什么 ============================
// 这是**按 Wayland 术语设计的最小内核接口 + 一个最小合成器**，目的是给"移植真 Wayland"
// 打地基：客户端在**用户态**画进共享缓冲（A5 前置的 shm_create(13)/shm_map(14)）→
// `wl_surface_commit` 提交 → 内核合成器把已提交的 surface 画进后备缓冲并**真的上屏**
// （fb_user_flip64，见下面"上屏路径"）→ 客户端用 `wl_display_dispatch` 收 seat 事件
// （复用 A5 前置 input_poll(12) 的 40 B 事件 + 路由语义）。
//
// **不是**真 Wayland（逐条差距见 docs/应用层与系统调用说明.md 文末"Wayland 基础骨架（A5）"节）：
//   * 没有 Unix domain socket、没有 `wl_display` 协议编解码 —— 调用走 int 0x80 自有号段；
//   * 没有 memfd/wl_shm 池、没有 wl_buffer 对象与 release 事件 —— 缓冲 = shm_create(13) 的对象；
//   * 没有 wl_surface 状态机（attach/damage/commit 之外的状态、帧回调、subsurface、输入区域）；
//   * 没有 DRM/KMS 原子提交、没有 epoll 事件循环、没有客户端库 libwayland-client；
//   * surface 的**位置/尺寸/层级由这个最小组合器定**（固定面板 + 槽位 + id 升序 z 序），
//     真 Wayland 里这些由 shell/合成器策略与 xdg-shell 决定。
//
// ============================ 调用号（int 0x80 自有 ABI）============================
//   15 wl_surface_create(w, h, format)               -> surface id (>=1) / 负错误码
//   16 wl_surface_attach(surf, shm_id, offset)       -> 0 / 负错误码
//   17 wl_surface_damage(surf, xy, wh)               -> 0 / 负错误码   ★见下"damage 寄存器打包"
//   18 wl_surface_commit(surf)                       -> 0 / 负错误码（入提交队列，dispatch 里合成）
//   19 wl_surface_destroy(surf)                      -> 0 / 负错误码
//   20 wl_seat_get()                                 -> seat id (>=1) / 负错误码
//   21 wl_display_dispatch(timeout_ms, out, max)     -> 本次投递给客户端的 seat 事件条数 / 负错误码
//
// 参数寄存器（与 A1/A5 前置同一条约定）：rdi/rsi/rdx/r10 = 参数 1..4；rax = 返回（负数 = 错误）。
// ★ damage 的寄存器打包（4 个参数放不下 5 个分量）：rdi=surf、rsi=(x | y<<32)、rdx=(w | h<<32)。
//   两个 32 位半区都按**有符号**解释：x/y 允许为负（合成时按 surface 矩形裁剪），w/h 必须 > 0。
//   用户态包装（user/lib/wl.c 的 wl_surface_damage）负责打包，调用方写的是 (surf,x,y,w,h)。
//
// ============================ 语义（提交模型 / 缓冲生命周期 / 事件路由）============================
// 1) 未提交不显示：surface 只有经过 attach + commit 之后才有内容；客户端写共享缓冲却不 commit，
//    屏幕上**一个像素都不会变**（测试 tests/wl64_test.py 断言这一条）。
// 2) 提交是**异步的**：commit 只把 surface 放进提交队列并打点 `[WL64] commit`；真正的合成发生在
//    `wl_display_dispatch`（事件循环）里 —— 一轮 dispatch 会把队列里的提交按 **id 升序**合成一遍，
//    然后打一行 `[WL64] composite n=.. bytes=.. us=..`（n = 本轮合成的 surface 数）。
//    也就是说客户端必须调 dispatch（真 Wayland 里是 wl_display_flush/roundtrip 的等价物）。
// 3) 双缓冲 / 缓冲生命周期（**最小实现，如实标注**）：一个 surface 同时只 attach **一块**缓冲；
//    attach 会在 shm 对象上**持有一个引用**（`[SHM64] hold`/`refs` 打点），换缓冲或销毁 surface
//    时把旧引用还回去。合成是**同步的**（dispatch 里当场把像素拷进后备缓冲 + 提交上屏），
//    所以"提交返回之后旧缓冲就可以重用"成立：客户端可以 ping-pong 两块缓冲（两块不同的 shm
//    对象，或同一对象里两个 4KiB 对齐的 offset），客户端自己保证不写"当前已 attach 的那块"。
//    真 Wayland 用 wl_buffer + release 事件表达这件事（我们没有 release 事件）。
// 4) damage：`wl_surface_damage` 可以调多次，**最小实现把它们并成一个包围盒**（真 Wayland 用矩形
//    列表/区域）。**一次 commit 期间一个 damage 都没有 = 整面**（damage 为空 -> 整面）。
//    合成时只把 damage 矩形里的像素拷进后备缓冲：框外的屏幕像素**不动**（测试断言"框外差≈0"）。
// 5) 裁剪：damage 会依次被 ① surface 矩形 ② 可见区域（面板）③ 更高层 surface 的可见矩形
//    ④ 屏幕 裁剪；surface 比缓冲大/缓冲比对象小的组合一律在 attach 时用错误码拒绝（不猜）。
// 6) 布局与 z 序（组合器策略，不是协议）：surface 落在**固定可见区域**（一块面板）里的槽位上
//    （槽位 = 创建序号 % WL64_MAX_SLOTS64），z 序 = **surface id 升序**（后创建的在上面），
//    重叠处上层遮住下层。真 Wayland 里位置/层级由 shell 定（xdg_surface.configure/commit）。
// 7) seat（座位）：`wl_seat_get()` 让当前进程成为座位持有者（申请键盘焦点 + 指针捕获 ——
//    复用 A5 前置 input_poll 的 flags 语义）并返回 seat id。事件投递（dispatch 的 out 数组）
//    用的就是 input_poll 的 40 B 结构（Ev64Event），**追加** 4 个字段回答"这条事件属于哪个
//    surface、坐标在不在它里面"（见 Wl64SeatEvent）：
//      * 指针事件（MOVE/DOWN/UP/WHEEL）：对 (x,y) 做**全表命中测试**（id 升序里的最上层、
//        且内容已提交的 surface）；命中者属于本进程 -> surf=该 id、inside=1、sx/sy=局部坐标；
//        命中者是**别的进程**的 surface 或没命中 -> surf=0、inside=0、sx=sy=-1。
//      * 键盘事件（KEY_DOWN/UP）：路由到本进程**最近一次指针命中的** surface（没有就取本进程
//        id 最大的 surface）；inside=1、sx=sy=-1（键盘没有坐标）。
// 8) 错误码（负数；口径与 A1/A5 前置一致，不与 Linux 号段共用）：见下面 WL64_E* 常量。
//
// ============================ 上屏路径（为什么这么做）============================
// 组合器**不改 kernel/fb.***：它用 fb_surface64() 拿后备缓冲基址直写像素（像用户程序一样），
// 再用 **fb_user_flip64(x,y,w,h)** 提交矩形上屏。为什么不用 fb_flip_region()/fb_fill_rect()：
// 那些是"内核侧绘制/提交"入口，会被 A1 的 fb_kernel_paint64(0) 开关挡掉（那个开关的语义是
// "用户程序独占屏幕"）；而组合器的提交在语义上就是**用户内容的提交**，走 fb_user_flip64
// 既不受开关影响，也不改 fb 的任何代码。像素**来源**是共享内存对象的物理页（恒等映射直读，
// 见 kernel/proc64.cpp 的 SHM64 段与 memlayout64.h 的恒等映射）。
//
// 串口打点（tests/wl64_test.py 按这些串 grep；格式勿改）：
//   [WL64] init fb=<w>x<h> panel=<x>,<y>,<w>,<h> slots=<n> bpp_us=<n>
//   [WL64] panel x=<n> y=<n> w=<n> h=<n> painted=<n>
//   [WL64] surface create id=<n> w=<n> h=<n> shm=<0|id> pid=<n> px=<n> fmt=<n>
//   [WL64] attach id=<surf> shm=<id> off=<n> bytes=<n> refs=<n> pages=<n> prev=<0|id>
//   [WL64] commit surf=<n> damage=<x>,<y>,<w>,<h> n=<n> full=<0|1> queue=<n>
//   [WL64] blit surf=<n> shm=<id> off=<n> src=<x>,<y>,<w>,<h> dst=<x>,<y> bytes=<n> clip=<ok|none>
//   [WL64] composite n=<n> bytes=<n> us=<n>
//   [WL64] seat event surf=<n> type=<n> x=<n> y=<n> inside=<0|1> sx=<n> sy=<n> code=<n>
//   [WL64] seat no-surface pid=<n> type=<n> x=<n> y=<n>            （指针不在自己任何 surface 上）
//   [WL64] seat drops pid=<n> suppressed=<n>
//   [WL64] surface destroy id=<n> pid=<n> shm=<0|id> refs_after=<n> held=<n> why=<client|exit>
//   [WL64] release pid=<n> surfs=<n> shm_refs_ret=<n>               （进程退出/销毁时的兜底回收）
//   [WL64] selftest PASS / [WL64] selftest FAIL mask=<n>
//   [WL64] demo start pid=<n> mode=<full|short> path=<p> pool_free=<n>
//   [WL64] demo done pid=<n> exited=<n> ticks=<n> pool_free=<n> pool_delta=<±n> surfs=<n> seat_users=<n> seat_logged=<n> seat_supp=<n>
//   [WL64] demo skipped (...)
#pragma once
#include <stdint.h>
#include "input64.h"      // 复用 40 B 的 Ev64Event（seat 事件 = 它 + 路由字段）

// ==================== 规模/常量 ====================
static const uint32_t WL64_MAX_SURFACES64 = 8;    // surface 表槽数（每进程最多这么多，够演示/验收）
static const uint32_t WL64_MAX_SLOTS64    = 2;    // 可见区域的槽位数（>1 才能演示重叠/z 序）
static const uint32_t WL64_PANEL_W64      = 288;  // 可见区域（面板）逻辑尺寸；放不下时按屏幕缩小
static const uint32_t WL64_PANEL_H64      = 200;
// 槽位相对面板原点的偏移（槽位 = 创建序号 % WL64_MAX_SLOTS64；两块槽位**故意重叠**，用于验证 z 序）。
// 用内联函数而不是 static const 数组：头文件被多个 TU 包含，未使用的数组可能触发 -Wunused-const-variable。
static inline uint32_t wl64_slot_dx64(uint32_t slot) { return (slot & 1u) ? 112u : 16u; }
static inline uint32_t wl64_slot_dy64(uint32_t slot) { return (slot & 1u) ?  88u : 16u; }
static const uint32_t WL64_MAX_PIXELS64   = 16384;        // 单 surface 像素上限（= 64KiB 缓冲 / 4B）
static const uint32_t WL64_MAX_FRAMES64   = 16;           // = proc64.h 的 SHM64_MAX_PAGES64

// 像素格式（与 A1 的 Fb64Info.format 同一口径）
static const uint32_t WL64_FORMAT_XRGB888864 = 0;

// ==================== seat 事件（ABI：64 字节 POD，字段顺序固定）====================
// 前 40 字节就是 input_poll 的事件本体（**逐字节相同**，所以两边的 C 结构可以直接互转）；
// 追加字段回答"事件属于哪个 surface、坐标是否在它里面"。
struct Wl64SeatEvent {
    Ev64Event ev;        // +0   ← 与 kernel/input64.h 的 Ev64Event 布局一致（40 B）
    uint32_t  seat;      // +40  wl_seat_get() 返回的座位 id
    uint32_t  surf;      // +44  路由到的 surface id；0 = 没有（指针不在自己任何 surface 上 / 没有 surface）
    int32_t   sx;        // +48  surface **局部**坐标 x；inside=0 或键盘事件 = -1
    int32_t   sy;        // +52  surface 局部坐标 y
    uint32_t  inside;    // +56  1 = 事件坐标落在该 surface 矩形内（键盘事件恒 1）
    uint32_t  pad;       // +60  对齐填充（结构 64 B）
};
static const uint32_t WL64_SEAT_EVENT_SIZE64 = 64;
static_assert(sizeof(Wl64SeatEvent) == 64, "Wl64SeatEvent 必须是 64 字节（ABI）");
static_assert(EV64_EVENT_SIZE64 == 40, "seat 事件的前 40 字节必须是 input_poll 的 40 字节事件");

// ==================== 错误码（负数）====================
static const int64_t WL64_EPERM64  = -1;   // 没有进程上下文 / surface 不属于本进程
static const int64_t WL64_EFAULT64 = -2;   // 用户指针非法（没通过 user64_range_ok64）
static const int64_t WL64_ENOENT64 = -3;   // 没有这个 surface / 没有这个 shm 对象 / 本进程没有该句柄
static const int64_t WL64_EINVAL64 = -4;   // 参数非法（尺寸/格式/offset/越界/max 超上限/未知格式）
static const int64_t WL64_ENODEV64 = -5;   // 没有可用的显示（fb 未初始化 / 面板放不下）
static const int64_t WL64_ENOSPC64 = -6;   // 资源满（surface 表满 / 引用计数无法 +1）

// ==================== 启动期 ====================
// 几何/面板布局 + 计时标定（rdtsc -> us）+ 自检；幂等，可重复调用（每次都会重打 init 行）。
// 位置：os_boot_path 里 ring3 演示之前（kernel64.cpp 的 wl64_demo64 之前自动调用）。
void wl64_init64();
int  wl64_selftest64();                 // 位掩码，0 = 全过（bit0 ABI 布局 / bit1 纯函数 / bit2 负例 / bit3 几何）

// ==================== 系统调用落点（int 0x80 号 15..21）====================
int64_t wl64_surface_create64(uint64_t w, uint64_t h, uint64_t format);
int64_t wl64_surface_attach64(uint64_t surf, uint64_t shm_id, uint64_t offset);
int64_t wl64_surface_damage64(uint64_t surf, uint64_t xy, uint64_t wh);
int64_t wl64_surface_commit64(uint64_t surf);
int64_t wl64_surface_destroy64(uint64_t surf);
int64_t wl64_seat_get64();
// timeout_ms：没有事件时最多等这么久（0 = 只查一次不等待）；out = Wl64SeatEvent 数组，max <= 32。
// 返回：本次投递给客户端的 seat 事件条数（>= 0）；提交队列的合成在同一轮里完成（[WL64] composite）。
int64_t wl64_display_dispatch64(uint64_t timeout_ms, uint64_t out_uptr, uint64_t max);

// ==================== 给 proc64 的回收钩子（进程退出/销毁时调用）====================
// surface 与它 attach 的 shm 引用都按 pid 回收（幂等：没有 surface 时只打一行 held=0 的 release 行）。
// **必须是 extern "C"**：proc64.cpp 用弱引用取它（与 ev64_proc_release64 同一条纪律）。
extern "C" void wl64_proc_release64(int pid);

// 当前进程持有的 surface 数（诊断/自检用；没有进程上下文 = 0）
int wl64_surfaces_held64();

// ==================== 启动期演示（/wlclient.elf：真进程 + ring3）====================
// ★ 交付方式：/wlclient.elf **不内嵌进内核** —— 它是**系统卷里的文件**（与 /bin/shell、/bin/tcc、
//   /bin/lua、/bin/gzip、/bin/edit 同一条纪律；验收夹具把它写进卷）。内核只在卷里按路径找它，
//   找不到就如实打一行 [WL64] demo skipped (no elf on vfs)，不假装跑过。
//   以**真进程**跑 user/apps/wlclient.c：
//   wl_seat_get -> wl_surface_create x2 -> shm_create/shm_map（A 两块缓冲、B 一块两块缓冲）
//   -> 每帧画进共享缓冲 -> damage + commit -> wl_display_dispatch 收 seat 事件 -> 销毁 surface 退出。
// 两种模式（**为其它验收脚本的启动时间着想**，见 wl64.cpp 里 WL64_PROBE_PATH64 的说明）：
//   * full ：系统卷里存在 /etc/wl64_probe（本批验收脚本的夹具盘写的）-> 完整 3 阶段演示（~15 s + 监听）；
//   * short：没有它 -> 2 帧提交 + 立即销毁退出（~1 s），别的脚本只多几行 [WL64] 打点。
// 返回 0 = 跑完（含跳过/超时，只打点）。
int wl64_demo64(const char* path);
