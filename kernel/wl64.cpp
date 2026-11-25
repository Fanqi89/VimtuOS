// wl64.cpp - ★ A5：Wayland 基础骨架的内核实现（surface 表 + 提交队列 + 最小合成器 + seat 路由）
//
// 接口/语义/错误码/打点格式的**唯一定义点**在 kernel/wl64.h（那份注释是契约，本文件严格实现它）。
// 本文件只做四件事：
//   1) surface 表与生命周期（create/attach/damage/commit/destroy + 按 pid 回收）；
//   2) 最小合成器：把**已提交**的 surface 从共享内存物理页拷进后备缓冲，再用 fb_user_flip64
//      按矩形提交上屏（不改 kernel/fb.*，理由见 wl64.h 的"上屏路径"段）；
//   3) seat：复用 A5 前置 input_poll(12) 的 40 B 事件 + 命中测试，回答"事件属于哪个 surface"；
//   4) 启动期演示驱动（幂等装 /wlclient.elf 进系统卷 -> 真进程跑 -> 有界等待 -> 打点）。
//
// ★ 只进**系统内核**（build64.sh 的 SRCS_OS；安装介质内核不链它，syscall64.cpp 对 15..21 号
//   用弱引用：那时这几个号返回 -1 并打 [SYSCALL] deny，**不假装成功**）。
#include "wl64.h"

#include <stddef.h>

#include "debug64.h"
#include "iso64.h"         // ★ v0.12.0：SMAP（把 rect 写回用户指针要开窗）
#include "x86_64.h"       // g_ticks64 / PIT_HZ_64 / pt_regs64 无关
#include "fb.h"           // fb_width/fb_height/fb_surface64/fb_user_flip64（**只调用**）
// ★ v0.11.1（第二步 wl_surface → Window）：装饰的**单一真源**在 gui64（内核窗口也用它）。
#include "gui64.h"        // gui64_deco_frame64 / gui64_deco_edge64 / gui64_deco_h64
#include "dock64.h"       // ★ v0.11.3：Dock × 窗口生命周期（外部窗口运行项 / 最小化收容 / 高亮）
#include "theme64.h"      // theme64_tokens64()（标题栏底色）
#include "gfx64.h"        // gfx64_fill_round64（标题栏不透明底色）
#include "icons64.h"      // ★ v0.11.4：应用图标 kind / 绘制 / 导出 RGBA（标题栏 + Dock 同源）
#include "font.h"         // ★ v0.11.4：font_raster_text64（标题覆盖率位图，给 Ring 3 合成器）
#include "proc64.h"       // 进程上下文 + shm 对象的只读视图/hold/release
#include "usermode64.h"   // user64_range_ok64（seat 事件出参校验）
#include "mem_64.h"       // page_count_free_64（演示的页池基线核对）
#include "vfs64.h"        // 在系统卷里按路径找 /wlclient.elf（**不内嵌**，见下）
#include "app64.h"        // app64_main_part_lba64（挂系统卷时的分区起点）
// ★ W3 第四批：挂系统卷要能按盘遍历（AHCI-only 机器上 drive 0 是空的 PATA 槽）
extern int ata64_drive_count64();
extern int ata64_slot_to_drive64(int slot);

// 需要调度器提供的有界睡眠（安装介质内核不链 task64 -> 这个弱引用为 0，退化为 PIT 忙等）
extern "C" void task_sleep_ms64(uint32_t ms) __attribute__((weak));

// ★ 交付方式（与 /bin/shell、/bin/tcc、/bin/lua、/bin/gzip、/bin/edit 同一条纪律）：
//   /wlclient.elf **不内嵌进内核** —— 它是"系统卷里的文件"，由验收夹具写进卷（tests/wl64_test.py
//   的夹具盘用 tools/make_shellvol.py 的 Volume 写出 /wlclient.elf）。内核只在卷里按路径找它，
//   找不到就如实打一行 [WL64] demo skipped (no elf on vfs)，**不假装跑过**。
//   为什么这么切（两条理由）：① system.img 的内核余量只剩 ~700 KB 的硬线（tests/a42a64_test.py
//   断言 ≥ 700,000 B），这个 blob 值 ~18 KB；② 内核二进制里搜不到它的字节（build64.sh 有断言）。
static const char WL64_CLIENT_PATH64[] = "/wlclient.elf";
// 演示模式探针：系统卷里存在这个文件 = "完整演示"（本批验收脚本的夹具盘会写它）。
// 为什么要有它（**为别的验收脚本的启动时间着想**）：完整演示要停在 seat 监听里等注入（有界 45 s），
// 而 sh64/musl64/dynlink64/desktop64/gui_modern64 等脚本都是"启动到桌面"的夹具，多等 45 s 会
// 逼近它们的启动超时。所以：没有探针 = 短模式（2 帧 + 立即销毁，~1 s）。
static const char WL64_PROBE_PATH64[] = "/etc/wl64_probe";

// ==================== 小工具 ====================
static inline uint64_t wl64_rdtsc64() {
    uint32_t lo = 0, hi = 0;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
// ==================== 打点包装（★ 体积纪律：必须 noinline）====================
// 为什么：debug64.h 的 dbg64_putc/str/dec/hex/nl 与 dbg64_line_begin/end64 全是 static inline，
// 而 dbg64_putc 里带着"等串口 LSR 空"的自旋循环 —— 每个**调用点**展开就是几百字节的内联代码。
// 本模块有 40+ 处打点（每处 4~10 个字段），实测不包 noinline 时 wl64.o 的 .text = 45,166 B
// （其中 wl64_display_dispatch64 一个函数就 15,709 B、wl64_surface_create64 8,108 B），
// 而系统内核的预算只剩 ~650 KB —— 那是"把日志当代码展开"的代价，不值得。
// 包一层 noinline 之后：每个字段只剩一条 call，打点行为**一字不变**（同样走 dbg64 的行锁 + 串口）。
// （不改 kernel/debug64.h：那是别人那组的地基，别的模块照旧内联，本文件自己收自己的体积。）
static void wl64_puts64(const char* s) __attribute__((noinline));
static void wl64_udec64(uint64_t v)      __attribute__((noinline));
static void wl64_nl64()                  __attribute__((noinline));
static void wl64_begin64()               __attribute__((noinline));
static void wl64_end64()                 __attribute__((noinline));
static void wl64_dec64(int64_t v)        __attribute__((noinline));
static void wl64_puts64(const char* s) { dbg64_str(s); }
static void wl64_udec64(uint64_t v)    { dbg64_dec(v); }
static void wl64_nl64()                { dbg64_nl(); }
static void wl64_begin64()             { dbg64_line_begin64(); }
static void wl64_end64()               { dbg64_line_end64(); }
// 带符号十进制（damage/坐标的负值要如实打出来）
static void wl64_dec64(int64_t v) {
    if (v < 0) { wl64_puts64("-"); wl64_udec64((uint64_t)(-v)); }
    else       { wl64_udec64((uint64_t)v); }
}
// 当前进程的 pid；<= 0 = 没有进程上下文（任务 0 / 共享地址空间模式 / 安装介质内核）
static inline int64_t wl64_pid64() {
    return (int64_t)proc64_current_pid64();
}

// ---- 矩形工具（半开区间 [x,x+w) × [y,y+h)）----
static inline bool wl64_in_rect64(int x, int y, int rx, int ry, int rw, int rh) {
    return rw > 0 && rh > 0 && x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}
// 求交：返回 1 = 有交集（*x/*y/*w/*h 被改成交集）；0 = 完全在外
static int wl64_rect_isect64(int* x, int* y, int* w, int* h, int cx, int cy, int cw, int ch) {
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < cx) x0 = cx;
    if (y0 < cy) y0 = cy;
    if (x1 > cx + cw) x1 = cx + cw;
    if (y1 > cy + ch) y1 = cy + ch;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return (*w > 0 && *h > 0) ? 1 : 0;
}
// 并集（包围盒）：空矩形（w<=0/h<=0）时直接取另一个
static void wl64_rect_union64(int* x, int* y, int* w, int* h, int x2, int y2, int w2, int h2) {
    if (*w <= 0 || *h <= 0) { *x = x2; *y = y2; *w = w2; *h = h2; return; }
    if (w2 <= 0 || h2 <= 0) return;
    const int x0 = (*x < x2) ? *x : x2;
    const int y0 = (*y < y2) ? *y : y2;
    const int x1 = (*x + *w > x2 + w2) ? *x + *w : x2 + w2;
    const int y1 = (*y + *h > y2 + h2) ? *y + *h : y2 + h2;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
}

// ==================== surface 表 ====================
struct Wl64Surface {
    uint32_t used;
    uint32_t id;
    int32_t  pid;
    uint32_t w, h, fmt;
    uint32_t slot;
    int32_t  x, y;                 // 屏幕坐标（面板原点 + 槽位偏移）
    // ---- attach 的缓冲（一个 surface 同时只有一块；缓冲 = 一个 shm 对象 + 4KiB 对齐的 offset）----
    int32_t  shm_id;               // 0 = 还没有缓冲
    uint32_t shm_off;
    uint32_t shm_bytes;
    uint32_t shm_pages;
    uint64_t frames[WL64_MAX_FRAMES64];
    // ---- 内容/提交状态 ----
    uint32_t content;              // 1 = 至少合成过一次（屏上有它的像素）
    uint32_t pending;              // 1 = 已 commit，等 dispatch 合成
    uint32_t have_damage;          // 本提交周期里有 damage 调用
    uint32_t ndamage;              // damage 调用次数（打点用）
    int32_t  dmg_x, dmg_y, dmg_w, dmg_h;   // damage 包围盒（surface 局部坐标；未裁剪）
    // ---- ★ v0.11.1（第二步 wl_surface → Window）：装饰（合成器画，客户端不自绘）----
    uint32_t decor;                // WL64_DECOR_F_WINDOW64 位；0 = 裸 surface（老口径，一个像素不变）
    char     title[WL64_TITLE_MAX64];  // 标题栏文字（UTF-8，63 B + 结尾 0；★ v0.11.4：32 -> 64 B，
                                       //   否则"图片查看器 1280x800"这种中文标题装不下）
    uint32_t deco_logged;          // "deco paint" 打点是否已经打过（防刷屏）
    int32_t  deco_log_x, deco_log_y;   // 上次打点时的位置（挪动后要重打一行）
    // ---- ★ v0.11.2：窗口控制状态（30 号改它；两条合成路都读它）----
    uint32_t collapsed;            // 1 = 折叠（只留 25px 标题带；客户区内容不合成）
    uint32_t maxed;                // 1 = 最大化（装饰框铺满整屏，客户区内容按原尺寸贴左上）
    int32_t  pre_max_x, pre_max_y; // 最大化之前的屏幕位置（还原用）
    // ---- ★ v0.11.3：Dock × 窗口生命周期 ----
    //   minimized = 1：整窗从屏幕摘掉（frame 高度 0 -> 装饰与内容都不合成、命中测试也落空），
    //   Dock 那条运行项画"最小化横杠"；点 Dock 项走 30 号 RESTORE，几何一个像素都不动。
    uint32_t minimized;
    // ---- ★ v0.11.4：应用身份（图标）----
    //   app_kind = icons64 的图标包 kind（0 = 没有身份/不认识的图标名）。
    //   来源：32 号 wl_surface_set_app()；用处：① 标题栏左侧的应用图标（两条合成路同一份 kind）
    //   ② Dock 运行项的图标 ③ 33 号把图标导出成 RGBA 给 Ring 3 合成器。
    uint32_t app_kind;
    char     app_name[WL64_APP_NAME_MAX64];   // 应用身份名（UTF-8，打点/诊断用）
    uint32_t alpha_logged;         // ★ v0.11.4：31 号打点是否已经打过（防刷屏）
    uint32_t alpha_log_w;          // 上次打点时的位图宽（标题变了要重打一行）
};
static Wl64Surface g_wl64_surfs[WL64_MAX_SURFACES64];
static uint32_t    g_wl64_next_id64 = 1;
static uint32_t    g_wl64_live64    = 0;      // 活着的 surface 数（打点/诊断）
static uint32_t    g_wl64_created64 = 0;      // 累计创建（槽位 = created % WL64_MAX_SLOTS64）

// 面板（可见区域）几何：init 里按当前屏幕算一次
static int  g_wl64_px64 = 0, g_wl64_py64 = 0, g_wl64_pw64 = 0, g_wl64_ph64 = 0;
static int  g_wl64_panel_ok64 = 0;            // 1 = 面板可用（fb 已初始化且放得下）
// ★ B-wm：注册过的 Ring 3 合成器（0 = 没有 -> 走 A5 的内核合成老路）。
// 为什么要这个状态位：同一个内核里两条合成路必须**互斥且可判定** —— 有合成器时内核一个像素
// 都不许碰面板（否则"合成在用户态"这句话就是假的）；没有合成器时（/wlclient.elf 的既有验收）
// 行为必须逐条不变。它只决定"谁来合成"，不改任何 surface/缓冲/提交语义。
static int      g_wl64_wm_pid64      = 0;
static uint64_t g_wl64_wm_exports64  = 0;     // 累计导出条数（诊断）
static uint64_t g_wl64_wm_maps64     = 0;     // 累计映射条数（诊断）
static uint64_t g_wl64_wm_acks64     = 0;     // 累计 ack 条数（诊断）
static uint64_t g_wl64_wm_posts64    = 0;     // 累计 seat 投递条数（诊断）
static uint32_t g_wl64_wm_hint_log64 = 0;     // "composite=external" 提示行的打印预算（防刷屏）
static int  g_wl64_painted64 = 0;             // 面板底色/边框是否已经画过（只在第一次合成时画）

// seat：座位 id（每个持有者都是 1，语义 = "这台显示器的那个座位"）+ 每 pid 的键盘焦点 surface
static const uint32_t WL64_SEAT_ID64 = 1;
static const int WL64_FOCUS_SLOTS64  = 16;    // 下标 = pid % 16
static uint32_t g_wl64_focus64[WL64_FOCUS_SLOTS64];
static uint64_t g_wl64_seat_users64 = 0;      // 申请过座位的进程数（诊断）
// 计时标定（rdtsc -> us；打点用）
static uint64_t g_wl64_tsc_per_ms64 = 0;
static uint32_t wl64_us64(uint64_t dt) {
    if (!g_wl64_tsc_per_ms64 || dt == 0) return 0;
    return (uint32_t)(dt * 1000ULL / g_wl64_tsc_per_ms64);
}

// 日志节流（seat 路由行：前 WL64_SEAT_LOG_MAX64 条全打，之后每 128 条一条 + 结算 suppressed）
// ★ v0.10.2：200 -> 64、32 -> 128。拖动一个窗口时每个鼠标事件都会走这里，而本项目
//   的日志出口是 COM1（每个 outb 一次 VM exit，实测 ≈1.4 ms/行）。证据口径不变：
//   前 64 条逐条原文，之后仍然有稀疏采样 + suppressed 结算。
static const uint32_t WL64_SEAT_LOG_MAX64 = 24;

// ★ v0.10.2（帧预算）：其它每事件打点的预算器。max 条照旧全打，之后每 pool 条一条。
//   为什么不干脆删掉：这些行是验收的证据（[WL64] export/ack/seatpost 的格式被
//   tests/*.py 逐字段解析），删了就失去可观测性；加预算只牺牲"第 N 条之后的全量"。
static uint32_t g_wl64_export_logged64 = 0;
static uint32_t g_wl64_ack_logged64    = 0;
static uint32_t g_wl64_post_logged64   = 0;
static bool wl64_hot_log64(uint32_t* slot, uint32_t max, uint32_t pool) {
    const uint32_t n = (*slot)++;
    if (n < max) return true;
    return pool && (((n - max) % pool) == 0);
}
static uint32_t g_wl64_seat_logged64 = 0;
static uint32_t g_wl64_seat_supp64   = 0;

// ---- 查找 ----
static Wl64Surface* wl64_find64(uint32_t id) {
    for (uint32_t i = 0; i < WL64_MAX_SURFACES64; i++) {
        if (g_wl64_surfs[i].used && g_wl64_surfs[i].id == id) return &g_wl64_surfs[i];
    }
    return nullptr;
}
static Wl64Surface* wl64_find_of_pid64(uint32_t id, int pid) {
    Wl64Surface* s = wl64_find64(id);
    return (s && s->pid == pid) ? s : nullptr;
}
// 本进程 id 最大的 surface（键盘焦点的兜底）
static Wl64Surface* wl64_top_of_pid64(int pid) {
    Wl64Surface* best = nullptr;
    for (uint32_t i = 0; i < WL64_MAX_SURFACES64; i++) {
        const Wl64Surface* s = &g_wl64_surfs[i];
        if (!s->used || s->pid != pid) continue;
        if (s->minimized) continue;                 // ★ v0.11.3：最小化的窗口不抢键盘焦点
        if (!best || s->id > best->id) best = &g_wl64_surfs[i];
    }
    return best;
}
static int wl64_count_of_pid64(int pid) {
    int n = 0;
    for (uint32_t i = 0; i < WL64_MAX_SURFACES64; i++) {
        if (g_wl64_surfs[i].used && g_wl64_surfs[i].pid == pid) n++;
    }
    return n;
}
int wl64_surfaces_held64() {
    const int64_t pid = wl64_pid64();
    return (pid > 0) ? wl64_count_of_pid64((int)pid) : 0;
}
static inline uint32_t* wl64_focus_slot64(int pid) {
    return &g_wl64_focus64[(uint32_t)(pid < 0 ? 0 : pid) % (uint32_t)WL64_FOCUS_SLOTS64];
}
// 命中测试：返回最上层的、**有内容**的、包含 (x,y) 的 surface（z 序 = id 升序 -> 取 id 最大者）
static Wl64Surface* wl64_hit64(int x, int y) {
    Wl64Surface* best = nullptr;
    for (uint32_t i = 0; i < WL64_MAX_SURFACES64; i++) {
        Wl64Surface* s = &g_wl64_surfs[i];
        if (!s->used || !s->content) continue;
        if (!wl64_in_rect64(x, y, s->x, s->y, (int)s->w, (int)s->h)) continue;
        if (s->minimized) continue;                 // ★ v0.11.3：收容到 Dock 的窗口不可命中/不可拖
        if (!best || s->id > best->id) best = s;
    }
    return best;
}
// 当前进程 id 的键盘焦点 surface：最近一次指针命中的（还在）优先，否则本进程 id 最大的
static Wl64Surface* wl64_kbd_surface64(int pid) {
    const uint32_t fid = *wl64_focus_slot64(pid);
    if (fid) {
        Wl64Surface* s = wl64_find_of_pid64(fid, pid);
        if (s) return s;
    }
    return wl64_top_of_pid64(pid);
}

// ==================== 几何（面板 + 槽位）====================
static void wl64_layout64() {
    const int fw = fb_width(), fh = fb_height();
    const bool fb_ok = (fw > 0 && fh > 0 && fb_surface64(nullptr, nullptr) != nullptr);
    int pw = (int)WL64_PANEL_W64, ph = (int)WL64_PANEL_H64;
    if (pw > fw - 16) pw = fw - 16;
    if (ph > fh - 16) ph = fh - 16;
    if (!fb_ok || pw < 64 || ph < 64) {
        g_wl64_panel_ok64 = 0;
        g_wl64_px64 = g_wl64_py64 = 0;
        g_wl64_pw64 = g_wl64_ph64 = 0;
        return;
    }
    g_wl64_panel_ok64 = 1;
    g_wl64_pw64 = pw;
    g_wl64_ph64 = ph;
    // ★ B-wm：可见区域 = 屏幕**右下角**的"外部面板区"（320x200），**避开 Dock**（Dock 高 60 +
    //   离底边 16 = 76，见 kernel/theme64.h 的 THEME64_DOCK_H/MARGIN 与 gui64 的 g_dock_*）。
    //   为什么从"居中"改成"右下"：内核外壳（gui64/panels64/explorer64）是屏幕的主人，Ring 3
    //   合成器只是**租用一块明确的区域**；贴右下角、避开 Dock 与左侧桌面图标是"互不干扰"最容易
    //   讲清楚的一种切法。谁拥有整屏、如何逐步搬家见 docs/应用层与系统调用说明.md 文末的
    //   "Ring 3 合成器（外壳外置第一步）"节。
    g_wl64_px64 = fw - pw - (int)WL64_PANEL_MARGIN64;
    g_wl64_py64 = fh - ph - (int)WL64_PANEL_BOTTOM64;
    if (g_wl64_px64 < 0) g_wl64_px64 = 0;
    if (g_wl64_py64 < 0) g_wl64_py64 = 0;
}
// 槽位 -> 屏幕位置（surface 创建时定死；组合器策略，见 wl64.h 第 6 条）
static void wl64_place64(Wl64Surface* s, uint32_t slot) {
    s->slot = slot % WL64_MAX_SLOTS64;
    // ★ v0.5.0（大窗口）+ ★ v0.9.7（判据修正）：比**面板区**大的 surface 改为**屏幕居中**。
    //   面板区是给 256x60 这类小面板设计的（右下角 320x200 里两块故意重叠），把 896x560 的浏览器
    //   窗口塞进槽位会被裁成一条 —— 所以"大"的要居中。
    //   ★ v0.9.7 真缺陷修复（wl64_test / wm64_test 的像素断言抓到的）：这里原来的判据是
    //     "比**槽位偏移**大就算大 surface"（wl64_slot_dx64(1)=112 / dy=88），于是 160x96 的 wmclock
    //     与 128x64 的 wmpanel 都被判成"大" -> 走屏幕居中（实测 rects=(560,352)/(576,368)），
    //     而 wl64_blit64 又把小 surface 的可见区裁到右下角外部面板区（948,516,320,200）
    //     -> 交集为空 -> 一个像素都不画（屏幕纯黑）+ `[WM] frame … blends=[0,0]`。
    //   正确判据要和 blit 的裁剪判据**同一把尺子**：比面板（320x200）大才算大 surface。
    {
        const int fw = fb_width(), fh = fb_height();
        // 面板还没算好（panel_ok=0）时按常量判（布局前创建的 surface 才不会全被当成"大"）
        const int pw_ = g_wl64_panel_ok64 ? (int)g_wl64_pw64 : (int)WL64_PANEL_W64;
        const int ph_ = g_wl64_panel_ok64 ? (int)g_wl64_ph64 : (int)WL64_PANEL_H64;
        if (fw > 0 && fh > 0 && ((int)s->w > pw_ || (int)s->h > ph_)) {
            const int cx = (fw - (int)s->w) / 2;
            const int cy = (fh - (int)s->h) / 2;
            s->x = (cx > 0) ? cx : 0;
            s->y = (cy > 0) ? cy : 0;
            return;
        }
    }
    s->x = g_wl64_px64 + (int)wl64_slot_dx64(s->slot);
    s->y = g_wl64_py64 + (int)wl64_slot_dy64(s->slot);
}

// ==================== 合成器（上屏路径）====================
// 像素来源：共享内存对象的物理页（恒等映射直读，见 memlayout64.h）。行可能跨页，所以按 4 字节
// 偏移取页号 + 页内偏移；页号越界（不该发生，attach 已校验）返回 0 而不是越界读。
static inline uint32_t wl64_src_px64(const Wl64Surface* s, uint32_t byte_off) {
    const uint32_t pg = byte_off >> 12;
    if (pg >= s->shm_pages) return 0;
    return *(const uint32_t*)(uintptr_t)(s->frames[pg] + (byte_off & 0xFFFu));
}
// 面板底色 + 边框（只在第一次合成前画一次；之后只按 damage 局部拷贝）
static void wl64_paint_chrome64(uint32_t* dst, int dw, int dh) {
    if (!g_wl64_panel_ok64 || g_wl64_painted64) return;
    const uint32_t bg = 0xFF20242Au, edge = 0xFF5A6472u, bar = 0xFF32384Au;
    for (int y = 0; y < g_wl64_ph64; y++) {
        const int sy = g_wl64_py64 + y;
        if (sy < 0 || sy >= dh) continue;
        uint32_t* row = dst + (uint32_t)sy * (uint32_t)dw;
        for (int x = 0; x < g_wl64_pw64; x++) {
            const int sx = g_wl64_px64 + x;
            if (sx < 0 || sx >= dw) continue;
            uint32_t c = bg;
            if (y < 2 || y >= g_wl64_ph64 - 2 || x < 2 || x >= g_wl64_pw64 - 2) c = edge;
            else if (y < 18) c = bar;
            row[sx] = c;
        }
    }
    fb_user_flip64(g_wl64_px64, g_wl64_py64, g_wl64_pw64, g_wl64_ph64);
    g_wl64_painted64 = 1;
    wl64_begin64();
    wl64_puts64("[WL64] panel x=");  wl64_udec64((uint64_t)g_wl64_px64);
    wl64_puts64(" y=");              wl64_udec64((uint64_t)g_wl64_py64);
    wl64_puts64(" w=");              wl64_udec64((uint64_t)g_wl64_pw64);
    wl64_puts64(" h=");              wl64_udec64((uint64_t)g_wl64_ph64);
    wl64_puts64(" painted=1");
    wl64_nl64();
    wl64_end64();
}
// ==================== ★ v0.11.1：合成期窗口装饰（wl_surface → Window）====================
// 设计（为什么这么切，逐条都是本仓库踩过的坑）：
//   1) **装饰只有一份实现** —— gui64_deco_frame64 / gui64_deco_edge64（kernel/gui64.cpp）。
//      内核窗口（Window）与 wl_surface 窗口因此逐像素同一套外观：阴影 + 24px 标题栏玻璃 +
//      标题文字 + 三个按钮 + 1px 玻璃边框 + 大圆角裁切。**客户端不再自绘标题栏**。
//   2) **装饰不改几何**：surface 的 x/y/w/h 一个字都不动（27 号/槽位的那套判据全部保留），
//      装饰只占掉顶部 WL64_DECOR_H64 行 —— 像素来源的客户区从第 25 行开始，
//      wl64_blit64 的内容循环**跳过**这 25 行（否则客户端的旧标题栏会和合成器的重叠）。
//   3) **遮挡与内容同一把尺子**：更高层 surface 盖住的部分装饰也不落笔。做法是把窗口矩形
//      按遮挡切成最多 WL64_DECO_RECTS64 块、逐块 fb_set_clip 再画。装饰器是逐像素确定的
//      （同一像素画两次结果相同），而每块互不重叠 -> 不会叠色、也不会漏画。
//   4) 只有 `decor` 位为 1 的 surface 走这条路；**裸 surface 的合成路径一个像素都不变**
//      （tests/wl64_test.py 的 91 项、wm64_test 的整屏合成断言因此不受影响）。
static const int WL64_DECO_RECTS64 = 8;

// ★ v0.11.2/0.11.3：装饰框（窗口矩形）。四条口径：
//   正常  = surface 自己那个矩形（x,y,w,h）
//   折叠  = (x, y, w, 25) —— 只留标题带；客户区内容**不合成**（内核路跳过、wm 也跳过）
//   最大化 = (0, 0, fb_w, fb_h) —— 装饰框铺满工作区；客户区内容按比例**缩放**铺满客户区
//            （★ v0.11.3 起；v0.11.2 是原尺寸贴左上，多出来的区域铺 client_bg）
//   最小化 = (x, y, w, 0) —— 整窗摘掉（Dock 收容）；装饰与内容都不合成，命中测试落空
static void wl64_frame64(const Wl64Surface* s, int* fx, int* fy, int* fw, int* fh) {
    if (s->minimized) { *fx = s->x; *fy = s->y; *fw = (int)s->w; *fh = 0; return; }
    if (s->collapsed) { *fx = s->x; *fy = s->y; *fw = (int)s->w; *fh = (int)WL64_DECOR_H64; return; }
    if (s->maxed)     { *fx = 0; *fy = 0; *fw = fb_width(); *fh = fb_height(); return; }
    *fx = s->x; *fy = s->y; *fw = (int)s->w; *fh = (int)s->h;
}
void wl64_frame_of64(uint32_t surf, int32_t* out_xywh) {
    Wl64Surface* s = wl64_find64(surf);
    if (!s || !out_xywh) return;
    int x = 0, y = 0, w = 0, h = 0;
    wl64_frame64(s, &x, &y, &w, &h);
    out_xywh[0] = x; out_xywh[1] = y; out_xywh[2] = w; out_xywh[3] = h;
}

struct Wl64DecoRect { int x, y, w, h; };

// 用 cut 把列表里的矩形切开（原地替换）；返回新的矩形数（顺序无关，互不重叠）。
// 0 面积片段丢弃；列表满（WL64_DECO_RECTS64）时后面的片段直接丢 —— 只有 8 个 surface 的
// 极端重叠才可能发生，丢了最多是"那一小块没重画"，不会画错别人的像素。
static int wl64_deco_sub64(Wl64DecoRect* rs, int n, int cx, int cy, int cw, int ch) {
    Wl64DecoRect out[WL64_DECO_RECTS64];
    int m = 0;
    const int cx1 = cx + cw, cy1 = cy + ch;
    for (int i = 0; i < n; i++) {
        const Wl64DecoRect r = rs[i];
        const int rx1 = r.x + r.w, ry1 = r.y + r.h;
        const int ix0 = (r.x > cx) ? r.x : cx, iy0 = (r.y > cy) ? r.y : cy;
        const int ix1 = (rx1 < cx1) ? rx1 : cx1, iy1 = (ry1 < cy1) ? ry1 : cy1;
        if (ix0 >= ix1 || iy0 >= iy1) { if (m < WL64_DECO_RECTS64) out[m++] = r; continue; }
        const Wl64DecoRect sub[4] = {
            { r.x, r.y, r.w, iy0 - r.y },              // 上
            { r.x, iy1, r.w, ry1 - iy1 },              // 下
            { r.x, iy0, ix0 - r.x, iy1 - iy0 },        // 左
            { ix1, iy0, rx1 - ix1, iy1 - iy0 }         // 右
        };
        for (int k = 0; k < 4 && m < WL64_DECO_RECTS64; k++) {
            if (sub[k].w > 0 && sub[k].h > 0) out[m++] = sub[k];
        }
    }
    for (int i = 0; i < m; i++) rs[i] = out[i];
    return m;
}

// 在 surface 矩形里画窗口装饰；phase 0 = 前半（阴影 + 标题带底 + 玻璃 + 标题文字 + 三按钮），
// phase 1 = 后半（1px 玻璃边框 + 四角圆角裁切）。
// **为什么必须分两相**（本批实测踩到）：阴影的蒙版在窗口内侧也有一圈淡淡的暗边，
// 若把整份装饰都画在客户区内容**之后**，那圈暗边会盖在内容上（实测标题带下面两行被压暗
// 242 -> 201）；而圆角裁切又必须画在内容**之后**，否则底部两个圆角会被内容糊掉。
// 两相顺序 = 内核窗口 draw_window 的同一顺序（阴影/标题栏 -> 客户区内容 -> 边框/圆角）。
// 返回落笔的像素数（0 = 没画：没有装饰位 / 太小 / 全被裁掉）。
static uint32_t wl64_deco_paint64(Wl64Surface* s, int vis_x, int vis_y, int vis_w, int vis_h, int phase) {
    if (!(s->decor & WL64_DECOR_F_WINDOW64)) return 0;
    int FX = 0, FY = 0, FW = 0, FH = 0;
    wl64_frame64(s, &FX, &FY, &FW, &FH);
    // 太小的 surface 画装饰没意义（标题栏就把它盖完了）：按"裸 surface"处理，如实不打点。
    if (FW < 48 || FH < (int)WL64_DECOR_H64 + 16) {
        if (!s->collapsed && !s->maxed) return 0;
    }
    Wl64DecoRect rs[WL64_DECO_RECTS64];
    int n = 0;
    {
        int rx = FX, ry = FY, rw = FW, rh = FH;
        if (wl64_rect_isect64(&rx, &ry, &rw, &rh, vis_x, vis_y, vis_w, vis_h) &&
            wl64_rect_isect64(&rx, &ry, &rw, &rh, 0, 0, fb_width(), fb_height())) {
            rs[0].x = rx; rs[0].y = ry; rs[0].w = rw; rs[0].h = rh;
            n = 1;
        }
    }
    if (n <= 0) return 0;
    // 减去更高层 surface 的可见矩形（与 wl64_blit64 第 5 步"被覆盖就不落笔"同一判据）
    for (uint32_t i = 0; i < WL64_MAX_SURFACES64 && n > 0; i++) {
        const Wl64Surface* h = &g_wl64_surfs[i];
        if (!h->used || !h->content || h->id <= s->id) continue;
        int hx = h->x, hy = h->y, hw = (int)h->w, hh = (int)h->h;
        if (!wl64_rect_isect64(&hx, &hy, &hw, &hh, vis_x, vis_y, vis_w, vis_h)) continue;
        n = wl64_deco_sub64(rs, n, hx, hy, hw, hh);
    }
    if (n <= 0) return 0;
    const Theme64Tokens* t = theme64_tokens64();
    uint32_t area = 0;
    for (int i = 0; i < n; i++) {
        fb_set_clip(rs[i].x, rs[i].y, rs[i].w, rs[i].h);
        if (phase == 0) {
            // 标题栏底色先**不透明**铺一遍：这条合成路上没有"整屏重绘"保证，玻璃若直接叠在
            // 上一帧自己的像素上，会一帧比一帧暗（alpha 累积）。铺完再叠玻璃 = 每帧确定。
            gfx64_fill_round64(FX, FY, FW, (int)WL64_DECOR_H64 + 1, THEME64_R_WINDOW,
                               t->title_bg, 255);
            // ★ v0.11.4：第 7 个参数 = 应用图标 kind（0 = 老口径，标题文字仍在 +8；有图标时文字右移到 +26）
            gui64_deco_frame64(FX, FY, FW, FH, s->title, 1, (int)s->app_kind);
            // ★ v0.11.2：最大化时客户区比 surface 大 —— 先把多出来的那块铺成客户区底色
            //   （内容随后按原尺寸贴左上；不缩放，如实标注）。
            if (s->maxed && !s->collapsed) {
                const int cx = FX + 1, cy = FY + (int)WL64_DECOR_H64;
                const int cw = FW - 2, ch = FH - (int)WL64_DECOR_H64 - 1;
                if (cw > 0 && ch > 0) fb_fill_rect(cx, cy, cw, ch, t->client_bg);
            }
        } else {
            gui64_deco_edge64(FX, FY, FW, FH);
        }
        area += (uint32_t)(rs[i].w * rs[i].h);
    }
    fb_reset_clip();
    // 打点：只在"第一次/位置变了/状态变了"时打（dispatch 每 8ms 一次，逐帧打会刷爆串口）
    if (phase == 0 && (!s->deco_logged || s->deco_log_x != FX || s->deco_log_y != FY)) {
        s->deco_logged = 1;
        s->deco_log_x = FX;
        s->deco_log_y = FY;
        wl64_begin64();
        wl64_puts64("[WL64] deco paint surf="); wl64_udec64(s->id);
        wl64_puts64(" rect=");  wl64_dec64(FX); wl64_puts64(","); wl64_dec64(FY);
        wl64_puts64(",");       wl64_dec64(FW); wl64_puts64(","); wl64_dec64(FH);
        wl64_puts64(" title=\""); wl64_puts64(s->title); wl64_puts64("\"");
        wl64_puts64(" btns=3 corner="); wl64_udec64((uint64_t)THEME64_R_WINDOW);
        wl64_puts64(" hdr=");           wl64_udec64((uint64_t)WL64_DECOR_H64);
        wl64_puts64(" parts=");         wl64_udec64((uint64_t)n);
        wl64_puts64(" collapsed=");     wl64_udec64((uint64_t)(s->collapsed ? 1 : 0));
        wl64_puts64(" maxed=");         wl64_udec64((uint64_t)(s->maxed ? 1 : 0));
        wl64_nl64();
        wl64_end64();
    }
    return area;
}

// 把 surface 的（已裁剪的）矩形拷进后备缓冲并提交上屏。
// 返回实际写入的像素字节数（0 = 完全被裁掉）。
/* ★ v0.11.3：最大化时的**缩放合成**（内容按比例铺满客户区）。
 * 为什么要有：v0.11.2 最大化只是把装饰框铺满，内容按原尺寸贴左上，右边/下边一大片客户区底色 ——
 *   用户看到的是"窗口变大了但画面没变"。这一版按 **fit（保持宽高比）+ 居中** 拉伸，
 *   多余的两条边仍然是 client_bg（letterbox），不做非等比拉伸（那样圆形会变椭圆）。
 * 两条合成路**同一把尺子**：内核这里与 user/wm/wm_blit_slot 的 maxed 分支逐式相同
 *   （scale = min(cw*1024/sw, ch*1024/sh)；ndw = sw*scale/1024；居中 (cw-ndw)/2）。
 * 与 1:1 路径同一条覆盖判据：逐行收集更高层 surface 的 x 区间（≤4 段），被盖住的像素不落笔。 */
static uint32_t wl64_blit_scaled64(Wl64Surface* s, uint32_t* dst, int dw, int dh,
                                   uint32_t deco_px, int vis_x, int vis_y, int vis_w, int vis_h) {
    int FX = 0, FY = 0, FW = 0, FH = 0;
    wl64_frame64(s, &FX, &FY, &FW, &FH);
    const int cx0 = FX + 1, cy0 = FY + (int)WL64_DECOR_H64;
    const int cw = FW - 2, ch = FH - (int)WL64_DECOR_H64 - 1;
    const int sw = (int)s->w, sh = (int)s->h;
    if (cw <= 2 || ch <= 2 || sw <= 0 || sh <= 0) return deco_px;
    const int sxs = cw * 1024 / sw, sys = ch * 1024 / sh;
    const int sc = (sxs < sys) ? sxs : sys;
    int ndw = sw * sc / 1024, ndh = sh * sc / 1024;
    if (ndw < 1) ndw = 1;
    if (ndh < 1) ndh = 1;
    const int ox = cx0 + (cw - ndw) / 2, oy = cy0 + (ch - ndh) / 2;
    const uint32_t pitch = s->w * 4u;
    const uint32_t id_self = s->id;
    uint32_t written = 0;
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    for (int yy = 0; yy < ndh; yy++) {
        const int sy = oy + yy;
        if (sy < 0 || sy >= dh) continue;
        if (sy < vis_y || sy >= vis_y + vis_h) continue;
        /* 覆盖：更高层 surface（有内容）在这一行上的 x 区间 */
        int cov[4][2], ncov = 0;
        for (uint32_t i = 0; i < WL64_MAX_SURFACES64 && ncov < 4; i++) {
            const Wl64Surface* h = &g_wl64_surfs[i];
            if (!h->used || !h->content || h->id <= id_self || h->minimized) continue;
            int hx = h->x, hy = h->y, hw = (int)h->w, hh = (int)h->h;
            if (!wl64_rect_isect64(&hx, &hy, &hw, &hh, vis_x, vis_y, vis_w, vis_h)) continue;
            if (sy < hy || sy >= hy + hh) continue;
            cov[ncov][0] = hx;
            cov[ncov][1] = hx + hw;
            ncov++;
        }
        const int src_row = (yy * sh / ndh) * (int)pitch;                 /* 最近邻：行取样 */
        uint32_t* drow = dst + (uint32_t)sy * (uint32_t)dw;
        for (int xx = 0; xx < ndw; xx++) {
            const int sx = ox + xx;
            if (sx < 0 || sx >= dw) continue;
            if (sx < vis_x || sx >= vis_x + vis_w) continue;
            bool covered = false;
            for (int c = 0; c < ncov; c++)
                if (sx >= cov[c][0] && sx < cov[c][1]) { covered = true; break; }
            if (covered) continue;
            const int col = (xx * sw / ndw) * 4;                          /* 最近邻：列取样 */
            drow[sx] = wl64_src_px64(s, s->shm_off + (uint32_t)src_row + (uint32_t)col);
            if (written == 0) { bx0 = sx; by0 = sy; bx1 = sx + 1; by1 = sy + 1; }
            else {
                if (sx < bx0) bx0 = sx;
                if (sy < by0) by0 = sy;
                if (sx + 1 > bx1) bx1 = sx + 1;
                if (sy + 1 > by1) by1 = sy + 1;
            }
            written += 4;
        }
    }
    const uint32_t deco_px2 = wl64_deco_paint64(s, vis_x, vis_y, vis_w, vis_h, 1);
    /* 上屏矩形 = 内容 bbox ∪ 装饰框（缩放后可能一直没落笔：比如整窗都在屏幕外） */
    int fx0 = bx0, fy0 = by0, fx1 = bx1, fy1 = by1;
    {
        int wx = FX, wy = FY, ww = FW, wh = FH;
        if (wl64_rect_isect64(&wx, &wy, &ww, &wh, vis_x, vis_y, vis_w, vis_h) &&
            wl64_rect_isect64(&wx, &wy, &ww, &wh, 0, 0, dw, dh)) {
            if (written == 0) { fx0 = wx; fy0 = wy; fx1 = wx + ww; fy1 = wy + wh; }
            else {
                if (wx < fx0) fx0 = wx;
                if (wy < fy0) fy0 = wy;
                if (wx + ww > fx1) fx1 = wx + ww;
                if (wy + wh > fy1) fy1 = wy + wh;
            }
        }
    }
    if (deco_px || deco_px2 || written)
        fb_user_flip64(fx0, fy0, fx1 - fx0, fy1 - fy0);
    wl64_begin64();
    wl64_puts64("[WL64] blitscale surf="); wl64_udec64(s->id);
    wl64_puts64(" from=");  wl64_dec64(sw); wl64_puts64("x"); wl64_dec64(sh);
    wl64_puts64(" to=");    wl64_dec64(ndw); wl64_puts64("x"); wl64_dec64(ndh);
    wl64_puts64(" at=");    wl64_dec64(ox); wl64_puts64(","); wl64_dec64(oy);
    wl64_puts64(" client="); wl64_dec64(cw); wl64_puts64("x"); wl64_dec64(ch);
    wl64_puts64(" bytes="); wl64_udec64(written);
    wl64_nl64();
    wl64_end64();
    return written + deco_px + deco_px2;
}
static uint32_t wl64_blit64(Wl64Surface* s, uint32_t* dst, int dw, int dh) {
    // ---- 1) damage 矩形（局部）----
    int rx = 0, ry = 0, rw = (int)s->w, rh = (int)s->h;
    if (s->have_damage) { rx = s->dmg_x; ry = s->dmg_y; rw = s->dmg_w; rh = s->dmg_h; }
    // ---- 2) 裁剪：surface 矩形 ----
    if (!wl64_rect_isect64(&rx, &ry, &rw, &rh, 0, 0, (int)s->w, (int)s->h)) return 0;
    // ★ v0.11.1/v0.11.2：装饰与窗口状态（折叠/最大化）决定"内容落在哪、还要不要落"
    const bool deco = (s->decor & WL64_DECOR_F_WINDOW64) != 0;
    int FX = 0, FY = 0, FW = 0, FH = 0;
    wl64_frame64(s, &FX, &FY, &FW, &FH);
    int dxr = rx + FX, dyr = ry + FY, dwr = rw, dhr = rh;
    // ---- 3) 裁剪：可见区域 ----
    // ★ v0.5.0（大窗口）：**大 surface 的可见区 = 整屏**，小 surface 仍是右下角面板区。
    //   为什么：面板区（320x200）是给 256x60 这类小面板设计的；896x580 的浏览器窗口套进去会被
    //   裁成一条（实测 dst=948,516 只写 97,440 B，屏幕上只看得到右下角一小块）。判定用**尺寸**
    //   而不是 id/序号 —— 小 surface 的合成路径一个像素都不变（store / fontdemo / wmclock 不变）。
    int vis_x = g_wl64_px64, vis_y = g_wl64_py64, vis_w = g_wl64_pw64, vis_h = g_wl64_ph64;
    if ((int)s->w > (int)WL64_PANEL_W64 || (int)s->h > (int)WL64_PANEL_H64 || deco) {
        vis_x = 0; vis_y = 0; vis_w = dw; vis_h = dh;
    }
    if (!wl64_rect_isect64(&dxr, &dyr, &dwr, &dhr, vis_x, vis_y, vis_w, vis_h)) return 0;
    // ---- 4) 裁剪：屏幕 ----
    if (!wl64_rect_isect64(&dxr, &dyr, &dwr, &dhr, 0, 0, dw, dh)) return 0;
    // 局部坐标随之平移（源矩形与目标矩形要对齐）
    rx += dxr - (rx + FX);
    ry += dyr - (ry + FY);
    const uint32_t pitch = s->w * 4u;
    // ★ 装饰分两相（顺序 = 内核窗口 draw_window）：前半（阴影 + 标题带）画在内容**之前**，
    //   后半（边框 + 圆角裁切）画在内容**之后** —— 见 wl64_deco_paint64 的注释。
    const uint32_t deco_px = deco ? wl64_deco_paint64(s, vis_x, vis_y, vis_w, vis_h, 0) : 0;
    // ★ v0.11.2：折叠状态下客户区内容一个像素都不合成（只留标题带）
    if (deco && s->collapsed) { dhr = 0; dwr = 0; }
    // ★ v0.11.3：Dock 收容（最小化）—— 一个像素都不合成（frame 高 0 已经让装饰停手，
    //   这里把内容 blit 也关掉；命中测试走 wl64_hit64 的 minimized 判断）。
    if (s->minimized) { dhr = 0; dwr = 0; }
    // ★ v0.11.3：最大化 = 内容按比例**缩放**铺满客户区（v0.11.2 只在左上贴原尺寸）。
    if (s->maxed && deco && !s->collapsed && s->shm_id)
        return wl64_blit_scaled64(s, dst, dw, dh, deco_px, vis_x, vis_y, vis_w, vis_h);
    // ---- 5) 逐行：再减去"更高层 surface 的可见矩形"盖住的部分 ----
    const uint32_t id_self = s->id;
    int cov[4][2];
    uint32_t written = 0;
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    for (int yy = 0; yy < dhr; yy++) {
        const int sy = dyr + yy;
        // ★ v0.11.1：装饰带（顶部 25 行）不走内容 blit
        if (deco && (ry + yy) < (int)WL64_DECOR_H64) continue;
        int ncov = 0;
        for (uint32_t i = 0; i < WL64_MAX_SURFACES64 && ncov < 4; i++) {
            Wl64Surface* h = &g_wl64_surfs[i];
            if (!h->used || !h->content || h->id <= id_self) continue;
            int hx = h->x, hy = h->y, hw = (int)h->w, hh = (int)h->h;
            if (!wl64_rect_isect64(&hx, &hy, &hw, &hh, vis_x, vis_y, vis_w, vis_h)) continue;
            if (sy < hy || sy >= hy + hh) continue;
            cov[ncov][0] = hx;
            cov[ncov][1] = hx + hw;
            ncov++;
        }
        uint32_t* drow = dst + (uint32_t)sy * (uint32_t)dw;
        const int src_row = (ry + yy) * (int)pitch;
        for (int xx = 0; xx < dwr; xx++) {
            const int sx = dxr + xx;
            bool covered = false;
            for (int c = 0; c < ncov; c++) {
                if (sx >= cov[c][0] && sx < cov[c][1]) { covered = true; break; }
            }
            if (covered) continue;
            const uint32_t v = wl64_src_px64(s, s->shm_off + (uint32_t)src_row + (uint32_t)(rx + xx) * 4u);
            drow[sx] = v;
            if (written == 0) { bx0 = sx; by0 = sy; bx1 = sx + 1; by1 = sy + 1; }
            else {
                if (sx < bx0) bx0 = sx;
                if (sy < by0) by0 = sy;
                if (sx + 1 > bx1) bx1 = sx + 1;
                if (sy + 1 > by1) by1 = sy + 1;
            }
            written += 4;
        }
    }
    // ★ v0.11.1：装饰后半（边框 + 圆角裁切）在内容之后；即使内容一个字节都没写
    //   （damage 只在标题带里）也要画。
    uint32_t deco_px2 = deco ? wl64_deco_paint64(s, vis_x, vis_y, vis_w, vis_h, 1) : 0;
    if (written == 0 && deco_px == 0 && deco_px2 == 0) {
        wl64_begin64();
        wl64_puts64("[WL64] blit surf="); wl64_udec64(s->id);
        wl64_puts64(" shm=");             wl64_dec64(s->shm_id);
        wl64_puts64(" off=");             wl64_udec64(s->shm_off);
        wl64_puts64(" src=");             wl64_dec64(rx); wl64_puts64(","); wl64_dec64(ry);
        wl64_puts64(",");                 wl64_dec64(rw); wl64_puts64(","); wl64_dec64(rh);
        wl64_puts64(" dst=");             wl64_dec64(dxr); wl64_puts64(","); wl64_dec64(dyr);
        wl64_puts64(" bytes=0 clip=none");
        wl64_nl64();
        wl64_end64();
        return 0;
    }
    // ---- 上屏矩形：内容 bbox ∪ 装饰矩形（装饰范围是**整个窗口**，可能远大于这次 damage）----
    int fx0 = bx0, fy0 = by0, fx1 = bx1, fy1 = by1;
    if (deco_px || deco_px2) {
        int wx = FX, wy = FY, ww = FW, wh = FH;
        if (wl64_rect_isect64(&wx, &wy, &ww, &wh, vis_x, vis_y, vis_w, vis_h) &&
            wl64_rect_isect64(&wx, &wy, &ww, &wh, 0, 0, dw, dh)) {
            if (written == 0) { fx0 = wx; fy0 = wy; fx1 = wx + ww; fy1 = wy + wh; }
            else {
                if (wx < fx0) fx0 = wx;
                if (wy < fy0) fy0 = wy;
                if (wx + ww > fx1) fx1 = wx + ww;
                if (wy + wh > fy1) fy1 = wy + wh;
            }
        }
    }
    fb_user_flip64(fx0, fy0, fx1 - fx0, fy1 - fy0);   // 组合器自己的提交（不受内核绘制开关影响）
    wl64_begin64();
    wl64_puts64("[WL64] blit surf="); wl64_udec64(s->id);
    wl64_puts64(" shm=");             wl64_dec64(s->shm_id);
    wl64_puts64(" off=");             wl64_udec64(s->shm_off);
    wl64_puts64(" src=");             wl64_dec64(rx); wl64_puts64(","); wl64_dec64(ry);
    wl64_puts64(",");                 wl64_dec64(rw); wl64_puts64(","); wl64_dec64(rh);
    wl64_puts64(" dst=");             wl64_dec64(dxr); wl64_puts64(","); wl64_dec64(dyr);
    wl64_puts64(" bytes=");           wl64_udec64(written);
    wl64_puts64(" clip=ok");
    wl64_nl64();
    wl64_end64();
    return written;
}
// 把提交队列里 pending 的 surface 按 id 升序合成一遍（dispatch 里调用）。
// 返回本轮合成的 surface 数；bytes 累加写出的字节数。
static uint32_t wl64_flush64(uint32_t* out_bytes) {
    uint32_t* dst = fb_surface64(nullptr, nullptr);
    const int dw = fb_width(), dh = fb_height();
    if (!dst || dw <= 0 || dh <= 0) { if (out_bytes) *out_bytes = 0; return 0; }
    uint32_t n = 0, bytes = 0;
    const uint64_t t0 = wl64_rdtsc64();
    wl64_paint_chrome64(dst, dw, dh);
    // id 升序：命令式地取"当前最小的 pending id"，最多 8 个 -> O(n^2) 但 n <= 8
    for (;;) {
        Wl64Surface* pick = nullptr;
        for (uint32_t i = 0; i < WL64_MAX_SURFACES64; i++) {
            Wl64Surface* s = &g_wl64_surfs[i];
            if (!s->used || !s->pending) continue;
            if (!pick || s->id < pick->id) pick = s;
        }
        if (!pick) break;
        pick->pending = 0;
        if (pick->shm_id && g_wl64_panel_ok64) {
            bytes += wl64_blit64(pick, dst, dw, dh);
            pick->content = 1;
        } else {
            pick->content = 1;   // 没有缓冲的"空提交"：算有内容（但一个字都没写）
        }
        pick->have_damage = 0;
        pick->ndamage = 0;
        n++;
    }
    if (n) {
        const uint32_t us = wl64_us64(wl64_rdtsc64() - t0);
        wl64_begin64();
        wl64_puts64("[WL64] composite n="); wl64_udec64(n);
        wl64_puts64(" bytes=");             wl64_udec64(bytes);
        wl64_puts64(" us=");                wl64_udec64(us);
        wl64_nl64();
        wl64_end64();
    }
    if (out_bytes) *out_bytes = bytes;
    return n;
}

// ==================== 生命周期 ====================
// 释放一个 surface（把 attach 的 shm 引用还回去）。refs_after 打点 = 归还后的对象引用数。
static void wl64_free_surface64(Wl64Surface* s, const char* why) {
    if (!s || !s->used) return;
    int refs_after = 0;
    const int32_t old = s->shm_id;
    if (old) {
        static uint64_t tmp[WL64_MAX_FRAMES64];   // ★ v0.5.0：512 条 = 4 KiB，栈上放不下 -> 静态（单线程调用，非重入）
        uint32_t pages = 0, refs = 0;
        (void)proc64_shm_release_id64(old, 0);          // pid=0：组合器（kernel/wl64.cpp）持有的引用
        if (proc64_shm_view64((uint32_t)old, tmp, WL64_MAX_FRAMES64, &pages, &refs) == 0) {
            refs_after = (int)refs;
        }
    }
    // ★ B-wm：把它在**合成器**地址空间里的缓冲映射窗也撤掉（注销 PTE）。
    // 为什么必须做：shm 页帧在引用归零后会被还回页池，留着 PTE 就等于给合成器留了一个
    // "指向已回收物理页"的读写窗口（越权 + 读到别人的页）。页帧本身不在这里回收（属于 shm 对象）。
    if (g_wl64_wm_pid64) {
        const uint64_t wva = wl64_surf_win_va64(s->slot);
        for (uint32_t i = 0; i < (uint32_t)WL64_MAX_FRAMES64; i++) {
            if (!user64_page_is_user_ok64(wva + (uint64_t)i * PAGE_SIZE_64)) break;
            (void)user64_unmap_page64(wva + (uint64_t)i * PAGE_SIZE_64);
        }
        user64_paging_sync64();
    }
    wl64_begin64();
    wl64_puts64("[WL64] surface destroy id="); wl64_udec64(s->id);
    wl64_puts64(" pid=");                      wl64_dec64(s->pid);
    wl64_puts64(" shm=");                      wl64_dec64(old);
    wl64_puts64(" refs_after=");               wl64_udec64((uint64_t)(refs_after < 0 ? 0 : refs_after));
    wl64_puts64(" held=");                     wl64_udec64((uint64_t)wl64_count_of_pid64(s->pid));
    wl64_puts64(" why=");                      wl64_puts64(why);
    wl64_nl64();
    wl64_end64();
    const int pid = s->pid;
    dock64_ext_del64((int)s->id);            // ★ v0.11.3：窗口没了 -> Dock 那条运行项退出运行态
    s->used = 0;
    if (g_wl64_live64 > 0) g_wl64_live64--;
    // 键盘焦点指到它就清掉（下一次键盘事件会退回"本进程 id 最大的 surface"）
    uint32_t* f = wl64_focus_slot64(pid);
    if (*f == s->id) *f = 0;
}
// 按 pid 回收（进程退出/销毁的兜底；幂等）
extern "C" void wl64_proc_release64(int pid) {
    if (pid <= 0) return;
    int surfs = 0, refs_ret = 0;
    for (uint32_t i = 0; i < WL64_MAX_SURFACES64; i++) {
        Wl64Surface* s = &g_wl64_surfs[i];
        if (!s->used || s->pid != pid) continue;
        if (s->shm_id) refs_ret++;
        wl64_free_surface64(s, "exit");
        surfs++;
    }
    *wl64_focus_slot64(pid) = 0;
    // ★ B-wm：合成器进程退出/被杀 -> **交回合成权**（回到 A5 的内核合成老路）。
    // 为什么必须做：内核不能留着一个指向已死进程的"合成权"——否则提交队列永远没人来拉，
    // 表面上看就是"客户端提交了却什么都不显示"（假死）。交回之后内核合成继续工作，
    // 桌面外壳与后来的 /wlclient.elf 都不受影响（验收 ⑦ 钉的就是这一条）。
    if (g_wl64_wm_pid64 == pid) {
        wl64_begin64();
        wl64_puts64("[WL64] composer release pid="); wl64_dec64(pid);
        wl64_puts64(" exports=");                    wl64_udec64(g_wl64_wm_exports64);
        wl64_puts64(" maps=");                       wl64_udec64(g_wl64_wm_maps64);
        wl64_puts64(" acks=");                       wl64_udec64(g_wl64_wm_acks64);
        wl64_puts64(" posts=");                      wl64_udec64(g_wl64_wm_posts64);
        wl64_puts64(" mode=internal");
        wl64_nl64();
        wl64_end64();
        g_wl64_wm_pid64 = 0;
        g_wl64_wm_hint_log64 = 0;
    }
    if (surfs > 0 || refs_ret > 0) {
        wl64_begin64();
        wl64_puts64("[WL64] release pid=");         wl64_dec64(pid);
        wl64_puts64(" surfs=");                     wl64_udec64((uint64_t)surfs);
        wl64_puts64(" shm_refs_ret=");              wl64_udec64((uint64_t)refs_ret);
        wl64_nl64();
        wl64_end64();
    }
}

// ==================== 系统调用落点（号 15..21）====================
// 15 wl_surface_create(w, h, format)
int64_t wl64_surface_create64(uint64_t w, uint64_t h, uint64_t format) {
    const int64_t pid = wl64_pid64();
    if (pid <= 0) return WL64_EPERM64;
    if (!g_wl64_panel_ok64) {
        wl64_begin64();
        wl64_puts64("[WL64] surface create FAILED pid="); wl64_dec64(pid);
        wl64_puts64(" reason=no-output err=");            wl64_dec64(-WL64_ENODEV64);
        wl64_nl64();
        wl64_end64();
        return WL64_ENODEV64;
    }
    const uint64_t px = w * h;
    if (w == 0 || h == 0 || px == 0 || px > (uint64_t)WL64_MAX_PIXELS64 || format != WL64_FORMAT_XRGB888864) {
        wl64_begin64();
        wl64_puts64("[WL64] surface create FAILED pid="); wl64_dec64(pid);
        wl64_puts64(" w=");   wl64_udec64(w);
        wl64_puts64(" h=");   wl64_udec64(h);
        wl64_puts64(" fmt="); wl64_udec64(format);
        wl64_puts64(" cap_px="); wl64_udec64((uint64_t)WL64_MAX_PIXELS64);
        wl64_puts64(" err="); wl64_dec64(-WL64_EINVAL64);
        wl64_nl64();
        wl64_end64();
        return WL64_EINVAL64;
    }
    Wl64Surface* free_s = nullptr;
    for (uint32_t i = 0; i < WL64_MAX_SURFACES64; i++) {
        if (!g_wl64_surfs[i].used) { free_s = &g_wl64_surfs[i]; break; }
    }
    if (!free_s) {
        wl64_begin64();
        wl64_puts64("[WL64] surface create FAILED pid="); wl64_dec64(pid);
        wl64_puts64(" reason=table-full cap=");            wl64_udec64((uint64_t)WL64_MAX_SURFACES64);
        wl64_puts64(" err=");                              wl64_dec64(-WL64_ENOSPC64);
        wl64_nl64();
        wl64_end64();
        return WL64_ENOSPC64;
    }
    // 清零（保留 frames 数组不必清，attach 时会覆写；这里全清最省心）
    for (uint32_t i = 0; i < (uint32_t)sizeof(Wl64Surface); i++) ((uint8_t*)free_s)[i] = 0;
    free_s->used  = 1;
    free_s->id    = g_wl64_next_id64++;
    if (g_wl64_next_id64 == 0) g_wl64_next_id64 = 1;
    free_s->pid   = (int32_t)pid;
    free_s->w     = (uint32_t)w;
    free_s->h     = (uint32_t)h;
    free_s->fmt   = (uint32_t)format;
    free_s->shm_id = 0;
    wl64_place64(free_s, g_wl64_created64++);   // 槽位 = 创建序号 % 槽位数
    g_wl64_live64++;
    wl64_begin64();
    wl64_puts64("[WL64] surface create id="); wl64_udec64(free_s->id);
    wl64_puts64(" w=");                       wl64_udec64(w);
    wl64_puts64(" h=");                       wl64_udec64(h);
    wl64_puts64(" shm=0");                    // 0 = 还没有 attach 缓冲（见 wl64.h 语义 3）
    wl64_puts64(" pid=");                     wl64_dec64(pid);
    wl64_puts64(" px=");                      wl64_udec64(px);
    wl64_puts64(" fmt=");                     wl64_udec64(format);
    wl64_puts64(" slot=");                    wl64_udec64((uint64_t)free_s->slot);
    wl64_puts64(" pos=");                     wl64_dec64(free_s->x); wl64_puts64(","); wl64_dec64(free_s->y);
    wl64_nl64();
    wl64_end64();
    return (int64_t)free_s->id;
}

// 16 wl_surface_attach(surf, shm_id, offset)
int64_t wl64_surface_attach64(uint64_t surf, uint64_t shm_id, uint64_t offset) {
    const int64_t pid = wl64_pid64();
    if (pid <= 0) return WL64_EPERM64;
    Wl64Surface* s = wl64_find_of_pid64((uint32_t)surf, (int)pid);
    if (!s) return WL64_ENOENT64;
    if (shm_id == 0 || shm_id > 0x7FFFFFFFu) return WL64_EINVAL64;
    if (!proc64_shm_has_handle_id64((int32_t)shm_id)) {
        wl64_begin64();
        wl64_puts64("[WL64] attach FAILED surf="); wl64_udec64(surf);
        wl64_puts64(" shm=");                      wl64_udec64(shm_id);
        wl64_puts64(" reason=no-handle err=");     wl64_dec64(-WL64_ENOENT64);
        wl64_nl64();
        wl64_end64();
        return WL64_ENOENT64;
    }
    static uint64_t tmp[WL64_MAX_FRAMES64];       // ★ v0.5.0：同上（512 条，栈放不下）
    uint32_t pages = 0, refs = 0;
    if (proc64_shm_view64((uint32_t)shm_id, tmp, WL64_MAX_FRAMES64, &pages, &refs) != 0) return WL64_ENOENT64;
    if (offset & 3u) return WL64_EINVAL64;                       // 像素必须 4 字节对齐
    const uint64_t need = (uint64_t)s->w * s->h * 4u;
    if (offset + need > (uint64_t)pages * 4096u) {
        wl64_begin64();
        wl64_puts64("[WL64] attach FAILED surf="); wl64_udec64(surf);
        wl64_puts64(" shm=");                      wl64_udec64(shm_id);
        wl64_puts64(" reason=buffer-too-small need="); wl64_udec64(need);
        wl64_puts64(" have=");                     wl64_udec64((uint64_t)pages * 4096u);
        wl64_puts64(" err=");                      wl64_dec64(-WL64_EINVAL64);
        wl64_nl64();
        wl64_end64();
        return WL64_EINVAL64;
    }
    const int32_t prev = s->shm_id;
    // 换缓冲：旧引用还给对象、新对象 +1 引用（同一个对象只持有一个引用；换 offset 不算换缓冲）
    if (prev && prev != (int32_t)shm_id) {
        (void)proc64_shm_release_id64(prev, 0);
        s->shm_id = 0;
    }
    if (s->shm_id != (int32_t)shm_id) {
        if (proc64_shm_hold_id64((uint32_t)shm_id) != 0) return WL64_ENOSPC64;
        s->shm_id = (int32_t)shm_id;
    }
    s->shm_off   = (uint32_t)offset;
    s->shm_bytes = (uint32_t)need;
    s->shm_pages = pages;
    for (uint32_t i = 0; i < WL64_MAX_FRAMES64; i++) s->frames[i] = (i < pages) ? tmp[i] : 0;
    uint32_t pages2 = 0, refs2 = 0;
    if (proc64_shm_view64((uint32_t)shm_id, tmp, WL64_MAX_FRAMES64, &pages2, &refs2) == 0) refs = refs2;
    wl64_begin64();
    wl64_puts64("[WL64] attach id=");   wl64_udec64(s->id);
    wl64_puts64(" shm=");               wl64_udec64(shm_id);
    wl64_puts64(" off=");               wl64_udec64(offset);
    wl64_puts64(" bytes=");             wl64_udec64(need);
    wl64_puts64(" refs=");              wl64_udec64((uint64_t)refs);
    wl64_puts64(" pages=");             wl64_udec64((uint64_t)pages);
    wl64_puts64(" prev=");              wl64_dec64(prev);
    wl64_nl64();
    wl64_end64();
    return 0;
}

// 17 wl_surface_damage(surf, xy, wh)   ★ 打包见 wl64.h（rdi=surf、rsi=(x|y<<32)、rdx=(w|h<<32)）
int64_t wl64_surface_damage64(uint64_t surf, uint64_t xy, uint64_t wh) {
    const int64_t pid = wl64_pid64();
    if (pid <= 0) return WL64_EPERM64;
    Wl64Surface* s = wl64_find_of_pid64((uint32_t)surf, (int)pid);
    if (!s) return WL64_ENOENT64;
    const int32_t x = (int32_t)(uint32_t)(xy & 0xFFFFFFFFu);
    const int32_t y = (int32_t)(uint32_t)(xy >> 32);
    const int32_t w = (int32_t)(uint32_t)(wh & 0xFFFFFFFFu);
    const int32_t h = (int32_t)(uint32_t)(wh >> 32);
    if (w <= 0 || h <= 0) return WL64_EINVAL64;
    // 多矩形并成包围盒（最小实现，见 wl64.h 语义 4）；越界部分留到合成时按 surface 矩形裁掉。
    if (!s->have_damage) {
        s->dmg_x = x; s->dmg_y = y; s->dmg_w = w; s->dmg_h = h;
        s->have_damage = 1;
    } else {
        wl64_rect_union64(&s->dmg_x, &s->dmg_y, &s->dmg_w, &s->dmg_h, x, y, w, h);
    }
    s->ndamage++;
    return 0;
}

// 18 wl_surface_commit(surf)
int64_t wl64_surface_commit64(uint64_t surf) {
    const int64_t pid = wl64_pid64();
    if (pid <= 0) return WL64_EPERM64;
    Wl64Surface* s = wl64_find_of_pid64((uint32_t)surf, (int)pid);
    if (!s) return WL64_ENOENT64;
    const uint32_t full = s->have_damage ? 0u : 1u;
    if (s->shm_id) s->pending = 1;                 // 有缓冲才进提交队列（没有缓冲 = 空提交）
    uint32_t pend = 0;
    for (uint32_t i = 0; i < WL64_MAX_SURFACES64; i++) if (g_wl64_surfs[i].used && g_wl64_surfs[i].pending) pend++;
    wl64_begin64();
    wl64_puts64("[WL64] commit surf="); wl64_udec64(s->id);
    wl64_puts64(" damage=");
    if (full) {
        wl64_puts64("0,0,"); wl64_udec64(s->w); wl64_puts64(","); wl64_udec64(s->h);
    } else {
        wl64_dec64(s->dmg_x); wl64_puts64(","); wl64_dec64(s->dmg_y); wl64_puts64(",");
        wl64_dec64(s->dmg_w); wl64_puts64(","); wl64_dec64(s->dmg_h);
    }
    wl64_puts64(" n=");     wl64_udec64((uint64_t)s->ndamage);
    wl64_puts64(" full=");  wl64_udec64(full);
    wl64_puts64(" buf=");   wl64_dec64(s->shm_id);
    wl64_puts64(" queue="); wl64_udec64((uint64_t)pend);
    wl64_nl64();
    wl64_end64();
    return 0;
}

// 19 wl_surface_destroy(surf)
int64_t wl64_surface_destroy64(uint64_t surf) {
    const int64_t pid = wl64_pid64();
    if (pid <= 0) return WL64_EPERM64;
    Wl64Surface* s = wl64_find_of_pid64((uint32_t)surf, (int)pid);
    if (!s) return WL64_ENOENT64;
    wl64_free_surface64(s, "client");
    return 0;
}

// 20 wl_seat_get()
// 语义：当前进程成为"座位持有者"——复用 A5 前置 input_poll 的 flags 语义申请键盘焦点 + 指针捕获。
// 走 input64.cpp 的 ev64_poll64(max=0, flags)（只申请、不取事件），所以焦点/捕获的打点与 input_poll
// 完全一致（[EV64] attach/focus），不新增一套路由状态。
int64_t wl64_seat_get64() {
    const int64_t pid = wl64_pid64();
    if (pid <= 0) return WL64_EPERM64;
    // ★ W3 第二批：**指针捕获归合成器**。合成器已经在跑时，客户端只申请键盘焦点 ——
    //   否则客户端一调 20 号就把全局捕获从合成器手里抢走（合成器收不到鼠标 -> 拖不动窗口，
    //   也没法把事件路由回客户端，界面看起来"卡死"）。
    //   没有合成器时行为逐条不变（焦点 + 捕获）：A5/tests/wl64_test.py 的既有验收照旧。
    const bool wm_holds = (g_wl64_wm_pid64 != 0 && g_wl64_wm_pid64 != (int)pid);
    const uint64_t evflags = EV64_FLAG_FOCUS64 | (wm_holds ? 0ULL : EV64_FLAG_CAPTURE64);
    const int64_t rc = ev64_poll64(0, 0, evflags);
    if (rc < 0) return rc;
    g_wl64_seat_users64++;
    *wl64_focus_slot64((int)pid) = 0;
    wl64_begin64();
    wl64_puts64("[WL64] seat get id="); wl64_udec64((uint64_t)WL64_SEAT_ID64);
    wl64_puts64(" pid=");               wl64_dec64(pid);
    wl64_puts64(" flags=");               wl64_udec64(evflags);
    wl64_puts64(wm_holds ? " (focus-only: composer holds capture) users="
                         : " (focus+capture) users=");
    wl64_udec64(g_wl64_seat_users64);
    wl64_nl64();
    wl64_end64();
    return (int64_t)WL64_SEAT_ID64;
}

// ★ v0.11.2：装饰窗口的标题带归合成器（三按钮）。这里给出两个小工具：
//   ① 按**装饰框**命中（折叠/最大化后框与 surface 矩形不同）
//   ② 三按钮命中（0=最小化 1=最大化 2=关闭，-1=不是按钮）
//   布局与 gui64 的 draw_title_buttons 完全同一把尺子（槽 30 / 块 24x16 / 右边距 1）。
static Wl64Surface* wl64_hit_frame64(int x, int y) {
    Wl64Surface* best = nullptr;
    for (uint32_t i = 0; i < WL64_MAX_SURFACES64; i++) {
        Wl64Surface* s = &g_wl64_surfs[i];
        if (!s->used || !s->content) continue;
        int fx = 0, fy = 0, fw = 0, fh = 0;
        wl64_frame64(s, &fx, &fy, &fw, &fh);
        if (x < fx || x >= fx + fw || y < fy || y >= fy + fh) continue;
        if (!best || s->id > best->id) best = s;
    }
    return best;
}
static int wl64_btn_hit64(const Wl64Surface* s, int x, int y) {
    if (!(s->decor & WL64_DECOR_F_WINDOW64)) return -1;
    if (!s->collapsed && !s->maxed && ((int)s->h < 48 || (int)s->w < 48)) return -1;
    int fx = 0, fy = 0, fw = 0, fh = 0;
    wl64_frame64(s, &fx, &fy, &fw, &fh);
    const int bx = fx + fw - 1 - 30 * 3;
    for (int b = 0; b < 3; b++) {
        const int bx0 = bx + b * 30;
        if (x >= bx0 && x < bx0 + 30 && y >= fy + 1 && y < fy + (int)WL64_DECOR_H64 - 1) return b;
    }
    return -1;
}

// 21 wl_display_dispatch(timeout_ms, out, max)
// 一轮事件循环：① 合成提交队列（[WL64] composite）② 取 seat 事件 + 命中测试路由（写进 out 数组）。
// 返回：本次投递的 seat 事件条数。timeout_ms > 0 且当前没有事件时有界等待（5ms 一片）。
int64_t wl64_display_dispatch64(uint64_t timeout_ms, uint64_t out_uptr, uint64_t max) {
    const int64_t pid = wl64_pid64();
    if (pid <= 0) return WL64_EPERM64;
    if (max > (uint64_t)EV64_MAX_EVENTS64) return WL64_EINVAL64;
    if (max > 0 && !user64_range_ok64(out_uptr, max * (uint64_t)WL64_SEAT_EVENT_SIZE64)) return WL64_EFAULT64;

    Ev64Queue* q = ev64_current_queue64();
    if (!q) return WL64_ENODEV64;

    // ---- ① 合成（本轮的提交队列）----
    //   ★ B-wm：注册了 Ring 3 合成器就**什么都不合成** —— 提交队列留在表里，由合成器用
    //   wl_surface_export(23) 拉取 + 自己合成 + wl_surface_ack(25) 确认。这里只打一行计数
    //   （前 8 轮），让"内核没有合成"这件事在串口上可核对（[WL64] composite 一行都不会再出现）。
    if (g_wl64_wm_pid64 == 0) {
        uint32_t bytes = 0;
        (void)wl64_flush64(&bytes);
    } else if (g_wl64_wm_hint_log64 < 8) {
        uint32_t pend = 0;
        for (uint32_t i = 0; i < WL64_MAX_SURFACES64; i++)
            if (g_wl64_surfs[i].used && g_wl64_surfs[i].pending) pend++;
        g_wl64_wm_hint_log64++;
        wl64_begin64();
        wl64_puts64("[WL64] dispatch pid="); wl64_dec64(pid);
        wl64_puts64(" composite=external pending="); wl64_udec64((uint64_t)pend);
        wl64_puts64(" owner="); wl64_dec64((int64_t)g_wl64_wm_pid64);
        wl64_nl64();
        wl64_end64();
    }

    // ---- ② 事件（没有就按 timeout 有界等待）----
    Ev64Event raw[EV64_MAX_EVENTS64];
    uint32_t got = 0;
    const uint64_t t0 = g_ticks64;
    const uint32_t max_ticks = (uint32_t)((timeout_ms + TICK_MS_64 - 1) / TICK_MS_64);
    for (;;) {
        got = ev64_pop64(q, raw, (uint32_t)max);
        if (got > 0 || max == 0) break;
        if (!max_ticks || (uint32_t)(g_ticks64 - t0) >= max_ticks) break;
        if (task_sleep_ms64) task_sleep_ms64(5);
        else { const uint64_t w = g_ticks64 + 1; uint64_t g = 0; while (g_ticks64 < w && ++g < 50000000ULL) __asm__ volatile("hlt"); }
    }
    if (got == 0) return 0;

    // ---- ③ 命中测试 + 写进用户数组 ----
    Wl64SeatEvent* out = (Wl64SeatEvent*)(uintptr_t)out_uptr;
    for (uint32_t i = 0; i < got; i++) {
        const Ev64Event* e = &raw[i];
        Wl64SeatEvent se;
        se.ev = *e;
        se.seat = WL64_SEAT_ID64;
        se.surf = 0;
        se.sx = -1;
        se.sy = -1;
        se.inside = 0;
        se.pad = 0;
        const bool is_key = (e->type == EV64_TYPE_KEY_DOWN || e->type == EV64_TYPE_KEY_UP);
        if (is_key) {
            Wl64Surface* ks = wl64_kbd_surface64((int)pid);
            if (ks) { se.surf = ks->id; se.inside = 1; }
        } else {
            // ★ v0.11.2：装饰窗口的**标题带**由合成器（这一层就是内核合成器）接管：
            //   落点在三按钮里 -> 直接执行 30 号动作，**不投给客户端**（否则 PE 的 OK 判定会被弄糊涂）；
            //   落在标题带其它地方 -> 也只是"抓窗"，同样不投。
            Wl64Surface* hs = wl64_hit_frame64(e->x, e->y);
            bool title_band = false;
            if (hs && (hs->decor & WL64_DECOR_F_WINDOW64) && e->type == EV64_TYPE_MOUSE_DOWN) {
                int fx = 0, fy = 0, fw = 0, fh = 0;
                wl64_frame64(hs, &fx, &fy, &fw, &fh);
                if (e->y >= fy + 1 && e->y < fy + (int)WL64_DECOR_H64) {
                    title_band = true;
                    const int b = wl64_btn_hit64(hs, e->x, e->y);
                    if (b >= 0) {
                        // ★ v0.11.3：内核路的"最小化"按钮 = 收容到 Dock（与 /bin/wm 同一条语义）
                        const uint64_t act = (b == 0) ? WL64_CTL_MIN2DOCK64
                                           : (b == 1) ? WL64_CTL_MAX64 : WL64_CTL_CLOSE64;
                        (void)wl64_surface_ctl64(hs->id, act);   // 里面有 [WL64] ctl 打点
                    }
                }
            }
            if (!title_band) {
                Wl64Surface* rh = wl64_hit64(e->x, e->y);
                if (rh && rh->pid == pid) {
                    se.surf = rh->id;
                    se.inside = 1;
                    se.sx = e->x - rh->x;
                    se.sy = e->y - rh->y;
                    *wl64_focus_slot64((int)pid) = rh->id;      // 指针命中 -> 该 surface 成为键盘焦点 surface
                }
            }
        }
        if (max > 0) out[i] = se;
        // 打点（前 WL64_SEAT_LOG_MAX64 条全打，之后每 32 条一条 + 结算 suppressed）
        if (g_wl64_seat_logged64 < WL64_SEAT_LOG_MAX64) {
            g_wl64_seat_logged64++;
            wl64_begin64();
            if (se.surf) {
                wl64_puts64("[WL64] seat event surf="); wl64_udec64(se.surf);
            } else {
                wl64_puts64("[WL64] seat no-surface pid="); wl64_dec64(pid);
            }
            wl64_puts64(" type=");   wl64_udec64(se.ev.type);
            wl64_puts64(" x=");      wl64_dec64(se.ev.x);
            wl64_puts64(" y=");      wl64_dec64(se.ev.y);
            wl64_puts64(" inside="); wl64_udec64((uint64_t)se.inside);
            wl64_puts64(" sx=");     wl64_dec64(se.sx);
            wl64_puts64(" sy=");     wl64_dec64(se.sy);
            wl64_puts64(" code=");   wl64_udec64(se.ev.code);
            wl64_nl64();
            wl64_end64();
        } else {
            g_wl64_seat_supp64++;
            if ((g_wl64_seat_supp64 & 127u) == 0) {
                wl64_begin64();
                wl64_puts64("[WL64] seat drops pid="); wl64_dec64(pid);
                wl64_puts64(" suppressed=");           wl64_udec64(g_wl64_seat_supp64);
                wl64_nl64();
                wl64_end64();
            }
        }
    }
    return (int64_t)got;
}

// ==================== ★ B-wm：Ring 3 合成器的系统调用落点（号 22..26）====================
// 语义/ABI/错误码的唯一定义点在 kernel/wl64.h 的 "B-wm" 段。这一段的共同前提：
//   **只有注册过的合成器进程**能调（否则 EPERM）—— 否则任何进程都能读别人的缓冲（越权）。
static inline bool wl64_is_wm64(int64_t pid) {
    return pid > 0 && g_wl64_wm_pid64 == (int)pid;
}

// 22 wl_composer_get(out_rect)
// 注册当前进程为合成器（幂等）+ 申请键盘焦点与指针捕获（复用 input_poll 的 flags，
// 所以"谁拿输入"与既有 EV64 口径完全一致，不新增一套路由状态）；out_rect != 0 时把内核
// 租给它的"外部面板区"写回去（合成器据此划自己的地盘，不再猜）。
// 返回值：(seat id) | (gpu << 8)：gpu = 1 表示显示后端是 virtio-gpu 2D 设备（fb_backend_name64），
//   Ring 3 合成器据此决定"够大的矩形才值得交给设备提交路径"。**如实**：没设备就是 0（soft-lfb）。
int64_t wl64_composer_get64(uint64_t out_rect_uptr) {
    const int64_t pid = wl64_pid64();
    if (pid <= 0) return WL64_EPERM64;
    if (!g_wl64_panel_ok64) return WL64_ENODEV64;
    if (out_rect_uptr && !user64_range_ok64(out_rect_uptr, (uint64_t)WL64_RECT_SIZE64)) return WL64_EFAULT64;
    const int64_t rc = ev64_poll64(0, 0, EV64_FLAG_FOCUS64 | EV64_FLAG_CAPTURE64);
    if (rc < 0) return rc;
    const bool re = (g_wl64_wm_pid64 == (int)pid);
    if (!re && g_wl64_wm_pid64) {
        // 换人：旧合成器的 surface 不动（它自己在退出路径上 destroy），只把"合成权"交出去。
        wl64_begin64();
        wl64_puts64("[WL64] composer replace prev="); wl64_dec64((int64_t)g_wl64_wm_pid64);
        wl64_puts64(" new=");                         wl64_dec64(pid);
        wl64_nl64();
        wl64_end64();
    }
    g_wl64_wm_pid64 = (int)pid;
    const char* backend = fb_backend_name64();
    const uint32_t gpu = (backend && backend[0] == 'v') ? 1u : 0u;   // "virtio-gpu-2d" vs "soft-lfb"
    wl64_begin64();
    wl64_puts64("[WL64] composer pid=");  wl64_dec64(pid);
    wl64_puts64(" seat=");                wl64_udec64((uint64_t)WL64_SEAT_ID64);
    wl64_puts64(" gpu=");                 wl64_udec64((uint64_t)gpu);
    wl64_puts64(" backend=");             wl64_puts64(backend ? backend : "?");
    wl64_puts64(" surfs=");               wl64_udec64((uint64_t)g_wl64_live64);
    wl64_puts64(" re=");                  wl64_udec64(re ? 1u : 0u);
    // ★ W3 第二批：合成器是**全屏窗口层**，22 号把整屏交给它（原来是右下角 320x200 面板区）。
    //   面板几何仍然打出来（panel=），既有验收按它反推"外壳的地盘"不受影响。
    const int sfw = fb_width(), sfh = fb_height();
    wl64_puts64(" rect=0,0,");            wl64_udec64((uint64_t)(sfw > 0 ? sfw : 0));
    wl64_puts64(",");                     wl64_udec64((uint64_t)(sfh > 0 ? sfh : 0));
    wl64_puts64(" panel=");               wl64_dec64(g_wl64_px64); wl64_puts64(",");
    wl64_dec64(g_wl64_py64);              wl64_puts64(",");
    wl64_dec64(g_wl64_pw64);              wl64_puts64(","); wl64_dec64(g_wl64_ph64);
    wl64_nl64();
    wl64_end64();
    if (out_rect_uptr) {
        Wl64Rect r;
        r.x = 0; r.y = 0;
        r.w = (sfw > 0) ? sfw : 0;
        r.h = (sfh > 0) ? sfh : 0;
        ISO64_STAC64();                                  // ★ v0.12.0：往用户页写 rect
        *(Wl64Rect*)(uintptr_t)out_rect_uptr = r;
        ISO64_CLAC64();
    }
    return (int64_t)WL64_SEAT_ID64 | ((int64_t)gpu << 8);
}

// 23 wl_surface_export(idx, out)：把第 idx 个 surface 槽的只读快照写进 *(Wl64SurfaceInfo*)out。
// 返回 0 = 填好；1 = 这个下标没有 surface（合成器据此结束遍历）；负 = 错误码。
int64_t wl64_surface_export64(uint64_t idx, uint64_t out_uptr) {
    const int64_t pid = wl64_pid64();
    if (pid <= 0) return WL64_EPERM64;
    if (!wl64_is_wm64(pid)) return WL64_EPERM64;
    if (idx >= (uint64_t)WL64_MAX_SURFACES64) return WL64_EINVAL64;
    if (!user64_range_ok64(out_uptr, (uint64_t)WL64_SURFINFO_SIZE64)) {
        wl64_begin64();
        wl64_puts64("[WL64] export FAILED pid="); wl64_dec64(pid);
        wl64_puts64(" reason=bad-out-ptr err=");  wl64_dec64(-WL64_EFAULT64);
        wl64_nl64();
        wl64_end64();
        return WL64_EFAULT64;
    }
    const Wl64Surface* s = &g_wl64_surfs[idx];
    if (!s->used) return 1;
    Wl64SurfaceInfo info;
    info.id = s->id;
    info.pid = s->pid;
    info.w = s->w;
    info.h = s->h;
    info.x = s->x;
    info.y = s->y;
    info.shm_id = s->shm_id;
    info.shm_off = s->shm_off;
    info.shm_bytes = s->shm_bytes;
    info.refs = 0;
    if (s->shm_id) {
        static uint64_t tmp[WL64_MAX_FRAMES64];   // ★ v0.5.0：同上（512 条，栈放不下）
        uint32_t pages = 0, refs = 0;
        if (proc64_shm_view64((uint32_t)s->shm_id, tmp, WL64_MAX_FRAMES64, &pages, &refs) == 0) info.refs = refs;
    }
    // 没有 damage = 整面（与 commit 打点同一口径，见 wl64.h 语义 4）
    info.dmg_x = s->have_damage ? s->dmg_x : 0;
    info.dmg_y = s->have_damage ? s->dmg_y : 0;
    info.dmg_w = s->have_damage ? s->dmg_w : (int32_t)s->w;
    info.dmg_h = s->have_damage ? s->dmg_h : (int32_t)s->h;
    info.flags = (s->pending ? WL64_SURFINFO_PENDING64 : 0u) |
                 (s->content ? WL64_SURFINFO_CONTENT64 : 0u) |
                 (s->shm_id  ? WL64_SURFINFO_HASBUF64  : 0u) |
                 // ★ v0.11.2：装饰/折叠/最大化状态也走 23 号导出 —— 合成器据此画装饰框与客户区
                 ((s->decor & WL64_DECOR_F_WINDOW64) ? WL64_SURFINFO_DECOR64 : 0u) |
                 (s->collapsed ? WL64_SURFINFO_COLLAPSED64 : 0u) |
                 (s->maxed     ? WL64_SURFINFO_MAXED64     : 0u) |
                 (s->minimized ? WL64_SURFINFO_MINIMIZED64 : 0u);   // ★ v0.11.3：Dock 收容态
    // ★ v0.11.2：pad 位改作"客户区底色"（0x00RRGGBB）：最大化时装饰框比 surface 大，
    //   多出来的那块要铺成客户区底色 —— 颜色由内核（主题）给出，合成器不必自己猜主题。
    info.pad = theme64_tokens64()->client_bg & 0x00FFFFFFu;
    *(Wl64SurfaceInfo*)(uintptr_t)out_uptr = info;
    g_wl64_wm_exports64++;
    if (!wl64_hot_log64(&g_wl64_export_logged64, 24u, 4095u)) return 0;
    wl64_begin64();
    wl64_puts64("[WL64] export i=");   wl64_udec64(idx);
    wl64_puts64(" id=");               wl64_udec64(s->id);
    wl64_puts64(" pid=");              wl64_dec64(s->pid);
    wl64_puts64(" rect=");             wl64_dec64(s->x); wl64_puts64(","); wl64_dec64(s->y);
    wl64_puts64(",");                  wl64_udec64(s->w); wl64_puts64(","); wl64_udec64(s->h);
    wl64_puts64(" buf=");              wl64_dec64(s->shm_id);
    wl64_puts64(" off=");              wl64_udec64(s->shm_off);
    wl64_puts64(" dmg=");              wl64_dec64(info.dmg_x); wl64_puts64(",");
    wl64_dec64(info.dmg_y);            wl64_puts64(",");
    wl64_dec64(info.dmg_w);            wl64_puts64(","); wl64_dec64(info.dmg_h);
    wl64_puts64(" flags=");            wl64_udec64((uint64_t)info.flags);
    wl64_nl64();
    wl64_end64();
    return 0;
}

// 24 wl_surface_map(surf, out)：把该 surface attach 的缓冲页映射进**合成器**的地址空间。
// 映射窗 = FB 映射区顶部 16 MiB 里的第 slot 个 2 MiB（见 wl64.h 的 WL64_SURF_WIN_STRIDE64）。
// 幂等：同一个 surface 重复调用只重打一行 re=1（页表项写法相同，重写无副作用）。
int64_t wl64_surface_map64(uint64_t surf, uint64_t out_uptr) {
    const int64_t pid = wl64_pid64();
    if (pid <= 0) return WL64_EPERM64;
    if (!wl64_is_wm64(pid)) return WL64_EPERM64;
    if (!user64_range_ok64(out_uptr, (uint64_t)WL64_SURFMAP_SIZE64)) return WL64_EFAULT64;
    Wl64Surface* s = wl64_find64((uint32_t)surf);
    if (!s) return WL64_ENOENT64;
    if (!s->shm_id || s->shm_pages == 0) return WL64_ENOENT64;   // 还没有缓冲
    const uint64_t va = wl64_surf_win_va64(s->slot);
    const int re = user64_page_is_user_ok64(va) ? 1 : 0;
    for (uint32_t i = 0; i < s->shm_pages && i < (uint32_t)WL64_MAX_FRAMES64; i++) {
        if (!user64_map_phys_page64(va + (uint64_t)i * PAGE_SIZE_64, s->frames[i],
                                    PTE_USER_64 | PTE_WRITE_64 | PTE_NX_64)) {
            wl64_begin64();
            wl64_puts64("[WL64] surfmap FAILED id="); wl64_udec64(s->id);
            wl64_puts64(" page=");                    wl64_udec64((uint64_t)i);
            wl64_puts64(" err=");                     wl64_dec64(-WL64_ENOSPC64);
            wl64_nl64();
            wl64_end64();
            return WL64_ENOSPC64;
        }
    }
    user64_paging_sync64();
    const uint32_t u = user64_page_is_user_ok64(va) ? 1u : 0u;
    Wl64SurfaceMap m;
    m.va = va;
    m.bytes = s->shm_pages * (uint32_t)PAGE_SIZE_64;
    m.pages = s->shm_pages;
    *(Wl64SurfaceMap*)(uintptr_t)out_uptr = m;
    g_wl64_wm_maps64++;
    wl64_begin64();
    wl64_puts64("[WL64] surfmap id=");  wl64_udec64(s->id);
    wl64_puts64(" shm=");               wl64_dec64(s->shm_id);
    wl64_puts64(" va=0x");              wl64_udec64(va);
    wl64_puts64(" bytes=");             wl64_udec64((uint64_t)m.bytes);
    wl64_puts64(" pages=");             wl64_udec64((uint64_t)m.pages);
    wl64_puts64(" re=");                wl64_udec64(re ? 1u : 0u);
    wl64_puts64(" u=");                 wl64_udec64((uint64_t)u);
    wl64_nl64();
    wl64_end64();
    return u ? 0 : WL64_ENOSPC64;
}

// 25 wl_surface_ack(surf)：合成器声明"这条我合成完了" —— 清 pending/damage、content=1
// （content=1 之后内核的命中测试才会把事件路由到它：与"屏上有它的像素"同一口径）。
int64_t wl64_surface_ack64(uint64_t surf) {
    const int64_t pid = wl64_pid64();
    if (pid <= 0) return WL64_EPERM64;
    if (!wl64_is_wm64(pid)) return WL64_EPERM64;
    Wl64Surface* s = wl64_find64((uint32_t)surf);
    if (!s) return WL64_ENOENT64;
    const uint32_t was_pending = s->pending;
    const int w = s->have_damage ? s->dmg_w : (int)s->w;
    const int h = s->have_damage ? s->dmg_h : (int)s->h;
    s->pending = 0;
    s->have_damage = 0;
    s->ndamage = 0;
    s->content = 1;
    g_wl64_wm_acks64++;
    if (!wl64_hot_log64(&g_wl64_ack_logged64, 16u, 1023u)) return 0;
    wl64_begin64();
    wl64_puts64("[WL64] ack id=");      wl64_udec64(s->id);
    wl64_puts64(" dmg=");
    wl64_dec64(was_pending ? s->dmg_x : 0); wl64_puts64(",");
    wl64_dec64(was_pending ? s->dmg_y : 0); wl64_puts64(",");
    wl64_dec64(w);                          wl64_puts64(",");
    wl64_dec64(h);
    wl64_puts64(" pending=");           wl64_udec64((uint64_t)was_pending);
    wl64_nl64();
    wl64_end64();
    return 0;
}

// 26 wl_seat_post(out_ev)：合成器把一条事件**投回**目标 surface 拥有者的队列（用户态路由）。
// *ev = Wl64SeatEvent（64 B），只用 surf 做寻址（其余字段就是事件本体）。返回投递到的 pid。
// 为什么这么切：合成器是唯一知道"哪块 surface 在最上面"的用户态实体（它自己合成），
//   所以命中测试搬到了用户态；内核只负责"把这条事件放进 pid 的队列"（不猜路由）。
int64_t wl64_seat_post64(uint64_t out_ev_uptr) {
    const int64_t pid = wl64_pid64();
    if (pid <= 0) return WL64_EPERM64;
    if (!wl64_is_wm64(pid)) return WL64_EPERM64;
    if (!user64_range_ok64(out_ev_uptr, (uint64_t)WL64_SEAT_EVENT_SIZE64)) return WL64_EFAULT64;
    const Wl64SeatEvent* se = (const Wl64SeatEvent*)(uintptr_t)out_ev_uptr;
    Wl64Surface* s = wl64_find64(se->surf);
    if (!s) return 0;                                   // 目标没了：如实返回 0（不投给任何人）
    void* qv = proc64_evq_of64(s->pid);
    if (!qv) return 0;
    ev64_push64((Ev64Queue*)qv, s->pid, &se->ev);
    g_wl64_wm_posts64++;
    if (!wl64_hot_log64(&g_wl64_post_logged64, 16u, 255u)) return (int64_t)s->pid;
    wl64_begin64();
    wl64_puts64("[WL64] seatpost surf="); wl64_udec64(s->id);
    wl64_puts64(" pid=");                 wl64_dec64(s->pid);
    wl64_puts64(" type=");                wl64_udec64(se->ev.type);
    wl64_puts64(" x=");                   wl64_dec64(se->ev.x);
    wl64_puts64(" y=");                   wl64_dec64(se->ev.y);
    wl64_nl64();
    wl64_end64();
    return (int64_t)s->pid;
}

// 27 wl_surface_move(surf, xy)：把 surface 的屏幕位置改成 (x|y<<32)。★ W3 第二批（窗口拖动）
//   权限：**拥有者进程**（客户端自己要求"我要在哪"）或**注册合成器**（窗口管理器拖动窗口），
//         别的进程一律 EPERM（不许把别人的窗口挪走）。
//   夹取：允许部分出屏（可以拖到边上），但至少留 WL64_MOVE_KEEP64 像素在屏内 —— 否则窗口会变成
//         "在屏外、看不见也点不到"的死窗口，而内核这边没有任何办法把它拿回来。
//   注意：**不改 z 序**（层级仍按 id 升序，最小实现的老口径），也不改尺寸。
static const int WL64_MOVE_KEEP64 = 32;
int64_t wl64_surface_move64(uint64_t surf, uint64_t xy) {
    const int64_t pid = wl64_pid64();
    if (pid <= 0) return WL64_EPERM64;
    Wl64Surface* s = wl64_find64((uint32_t)surf);
    if (!s) return WL64_ENOENT64;
    const bool owner = (s->pid == (int32_t)pid);
    if (!owner && !wl64_is_wm64(pid)) return WL64_EPERM64;
    int32_t x = (int32_t)(uint32_t)(xy & 0xFFFFFFFFu);
    int32_t y = (int32_t)(uint32_t)(xy >> 32);
    const int fw = fb_width(), fh = fb_height();
    const int w = (int)s->w, h = (int)s->h;
    if (fw > 0) {
        if (x > fw - WL64_MOVE_KEEP64) x = fw - WL64_MOVE_KEEP64;
        if (x < -(w - WL64_MOVE_KEEP64)) x = -(w - WL64_MOVE_KEEP64);
    }
    if (fh > 0) {
        if (y > fh - WL64_MOVE_KEEP64) y = fh - WL64_MOVE_KEEP64;
        if (y < -(h - WL64_MOVE_KEEP64)) y = -(h - WL64_MOVE_KEEP64);
    }
    const int32_t ox = s->x, oy = s->y;
    s->x = x;
    s->y = y;
    wl64_begin64();
    wl64_puts64("[WL64] move id=");  wl64_udec64(s->id);
    wl64_puts64(" pid=");            wl64_dec64(pid);
    wl64_puts64(" pos=");            wl64_dec64(x); wl64_puts64(","); wl64_dec64(y);
    wl64_puts64(" from=");           wl64_dec64(ox); wl64_puts64(","); wl64_dec64(oy);
    wl64_puts64(" by=");             wl64_puts64(owner ? "owner" : "composer");
    wl64_nl64();
    wl64_end64();
    return 0;
}

// 28 wl_surface_set_decor(surf, flags, title_uptr)：把 surface 声明成**窗口**（★ v0.11.1）
//   语义见 kernel/wl64.h 的 WL64_ABI_SURFACE_DECOR64 段。要点：
//   ① 权限与 27 号一致（拥有者 / 注册合成器），别的进程 EPERM；
//   ② flags 只认 bit0（1 = 有装饰），其余位必须为 0 —— 不猜未知语义，直接 EINVAL；
//   ③ title 是用户态 C 串，**逐字节过 user64_range_ok64** 再拷（最多 31 B + 结尾 0），
//      指针 0 = 保留原标题；串没有结尾 0 也不会越过上限（第 31 字节处强制截断）；
//   ④ 装饰不改几何/不改 z 序，只是"合成器怎么画"。已经 attach 过缓冲的 surface 立刻
//      重新入队合成一次 —— 否则设置完要等下一次 commit 才看得见效果（验收会读不到
//      `[WL64] deco paint` 那一行）。
int64_t wl64_surface_set_decor64(uint64_t surf, uint64_t flags, uint64_t title_uptr) {
    const int64_t pid = wl64_pid64();
    if (pid <= 0) return WL64_EPERM64;
    Wl64Surface* s = wl64_find64((uint32_t)surf);
    if (!s) return WL64_ENOENT64;
    const bool owner = (s->pid == (int32_t)pid);
    if (!owner && !wl64_is_wm64(pid)) return WL64_EPERM64;
    if ((flags & ~(uint64_t)WL64_DECOR_F_WINDOW64) != 0) return WL64_EINVAL64;
    char tb[WL64_TITLE_MAX64];                 // ★ v0.11.4：32 -> 64 B（中文标题 3 B/字）
    for (uint32_t i = 0; i < (uint32_t)sizeof(tb); i++) tb[i] = 0;
    if (title_uptr) {
        uint32_t n = 0;
        for (; n + 1u < (uint32_t)sizeof(tb); n++) {
            if (!user64_range_ok64(title_uptr + (uint64_t)n, 1)) return WL64_EFAULT64;
            const char c = *(const char*)(uintptr_t)(title_uptr + (uint64_t)n);
            tb[n] = c;
            if (!c) break;
        }
        tb[sizeof(tb) - 1u] = 0;
    }
    bool changed = (s->decor != (uint32_t)flags);
    if (title_uptr) {
        for (uint32_t i = 0; i < (uint32_t)sizeof(tb); i++) {
            if (s->title[i] != tb[i]) { changed = true; break; }
        }
    }
    s->decor = (uint32_t)flags;
    if (title_uptr) {
        for (uint32_t i = 0; i < (uint32_t)sizeof(s->title); i++) s->title[i] = tb[i];
    }
    s->deco_logged = 0;                       // 状态变了：下一次合成重新打一行 deco paint
    if (s->shm_id) s->pending = 1;            // 已 attach 过缓冲 -> 立刻重新合成一遍
    /* ★ v0.11.3：一变成"窗口"就在 Dock 上登记一条运行项（最小化收容/点图标恢复/活动高亮
     *   都要有它）。裸 surface（flags=0）不登记 —— 面板/时钟那些不该出现在任务栏里。 */
    if (s->decor & WL64_DECOR_F_WINDOW64) {
        if (dock64_ext_add64((int)s->id, s->title, (int)s->app_kind) == 1) {
            dock64_ext_title64((int)s->id, s->title);
            dock64_ext_icon64((int)s->id, (int)s->app_kind);
        }
    } else {
        dock64_ext_del64((int)s->id);
    }
    if (changed) {
        wl64_begin64();
        wl64_puts64("[WL64] decor id=");  wl64_udec64(s->id);
        wl64_puts64(" pid=");             wl64_dec64(pid);
        wl64_puts64(" flags=");           wl64_udec64(flags);
        wl64_puts64(" mode=");            wl64_puts64((flags & WL64_DECOR_F_WINDOW64) ? "window" : "bare");
        wl64_puts64(" hdr=");             wl64_udec64((uint64_t)WL64_DECOR_H64);
        wl64_puts64(" title=\"");         wl64_puts64(s->title); wl64_puts64("\"");
        wl64_nl64();
        wl64_end64();
    }
    return 0;
}

// 30 wl_surface_ctl(surf, action)：标题栏三个按钮的**真实语义**（★ v0.11.2）
//   1 close     ：给拥有者投一条 type=7 的 CLOSE 事件（队列里排在这条之后的都还在），然后**销毁** surface；
//                 "先投事件再销毁"是刻意的：客户端要靠 surf=id 认出是哪扇窗被关了。
//   2 collapse  ：折叠开关。折叠后装饰框只有 25px 标题带，客户区内容不合成（两条合成路都按 23 号导出
//                 的 WL64_SURFINFO_COLLAPSED64 位判断）。
//   3 maximize  ：最大化开关。装饰框铺满整屏，客户区内容按**原尺寸**贴 (0,25)，多出来的区域用客户区底色补
//                 —— surface 尺寸是客户端定的，内核**不缩放**内容（如实标注，不假装能拉伸）。
//   三个动作都会让 surface 重新入队合成（pending=1），否则要等下一次客户端提交才看得见效果。
//   权限：拥有者 或 注册合成器 或 **内核自己**（pid <= 0）。
//   ★ v0.11.3：内核自己这一路是为 Dock 点击开的 —— Dock 在 ring0 的画/点路径里，没有进程上下文，
//     原来的 `pid <= 0 -> EPERM` 让"点 Dock 项恢复"永远无效（实测 [DOCK64] press action=ext-restore
//     打了、[WL64] ctl 一行都没有）。用户态拿不到 pid<=0，越权面没有变化。
int64_t wl64_surface_ctl64(uint64_t surf, uint64_t action) {
    const int64_t pid = wl64_pid64();
    const bool by_kernel = (pid <= 0);
    Wl64Surface* s = wl64_find64((uint32_t)surf);
    if (!s) return WL64_ENOENT64;
    const bool owner = (!by_kernel && s->pid == (int32_t)pid);
    if (!by_kernel && !owner && !wl64_is_wm64(pid)) return WL64_EPERM64;
    if (action != WL64_CTL_CLOSE64 && action != WL64_CTL_COLLAPSE64 && action != WL64_CTL_MAX64 &&
        action != WL64_CTL_MIN2DOCK64 && action != WL64_CTL_RESTORE64)
        return WL64_EINVAL64;

    if (action == WL64_CTL_CLOSE64) {
        // ① 先给拥有者投 CLOSE 事件（客户端用它结束事件循环；投不到不是错误 —— 进程可能已经走了）
        void* qv = proc64_evq_of64(s->pid);
        if (qv) {
            Ev64Event ev;
            for (uint32_t i = 0; i < (uint32_t)sizeof(ev); i++) ((uint8_t*)&ev)[i] = 0;
            ev.type = WL64_EV_CLOSE64;
            ev.x = s->x;
            ev.y = s->y;
            ev.t_ms = (uint32_t)(g_ticks64 * (uint64_t)TICK_MS_64);
            ev64_push64((Ev64Queue*)qv, s->pid, &ev);
        }
        const uint32_t sid = s->id;
        const int32_t spid = s->pid;
        (void)wl64_surface_destroy64(surf);          // 拥有者自己销毁也走同一条路（幂等）
        wl64_begin64();
        wl64_puts64("[WL64] ctl id=");   wl64_udec64(sid);
        wl64_puts64(" pid=");            wl64_dec64(spid);
        wl64_puts64(" action=close by="); wl64_puts64(by_kernel ? "kernel" : owner ? "owner" : "composer");
        wl64_puts64(" notify=");         wl64_udec64(qv ? 1u : 0u);
        wl64_puts64(" destroyed=1");
        wl64_nl64();
        wl64_end64();
        return 0;
    }
    if (action == WL64_CTL_MIN2DOCK64) {
        /* ★ v0.11.3：最小化收容到 Dock —— 整窗从屏幕摘掉（frame 高 0），几何**不动**，
         *   所以 restore 一定逐像素回到原位（不是"重新摆一次"，是"同一个矩形再出现"）。 */
        s->minimized = 1;
        s->collapsed = 0;
    } else if (action == WL64_CTL_RESTORE64) {
        s->minimized = 0;
        s->collapsed = 0;
        s->maxed = 0;
    } else if (action == WL64_CTL_COLLAPSE64) {
        s->collapsed = s->collapsed ? 0u : 1u;
        if (s->collapsed) s->maxed = 0u;            // 折叠与最大化互斥（折叠时先还原最大化状态）
        s->minimized = 0;
    } else {                                         // WL64_CTL_MAX64
        s->minimized = 0;
        if (!s->maxed) {
            s->pre_max_x = s->x;
            s->pre_max_y = s->y;
            s->maxed = 1u;
            s->collapsed = 0u;
            s->x = 0;
            s->y = 0;
        } else {
            s->maxed = 0u;
            s->x = s->pre_max_x;
            s->y = s->pre_max_y;
        }
    }
    s->deco_logged = 0;                              // 状态变了：下一次合成重打一行 deco paint
    if (s->shm_id) s->pending = 1;
    /* ★ v0.11.3：把"最小化收容"状态同步给 Dock 运行项（横杠/圆点/胶囊的判据）。 */
    if ((s->decor & WL64_DECOR_F_WINDOW64) && (action == WL64_CTL_MIN2DOCK64 ||
        action == WL64_CTL_RESTORE64 || action == WL64_CTL_COLLAPSE64 || action == WL64_CTL_MAX64))
        dock64_ext_state64((int)s->id, (int)(s->minimized ? 1 : 0));
    /* ★ v0.11.3：内核合成路（没有 ring3 合成器）—— 最小化/恢复后必须让 GUI 把这块地重画一遍，
     *   否则旧窗口像素会留在屏上（内核路的 blit 只负责"新内容上屏"，不做背景回填）。
     *   有合成器时由合成器的脏矩形机制处理（它每轮从 23 号读 pending/damage）。 */
    if (g_wl64_wm_pid64 == 0) {
        gui64_dirty((int)s->x, (int)s->y, (int)s->w, (int)s->h);
        int qx = 0, qy = 0, qw = 0, qh = 0;
        wl64_frame64(s, &qx, &qy, &qw, &qh);
        if (qw > 0 && qh > 0 && qh != (int)s->h) gui64_dirty(qx, qy, qw, qh);
    }
    int fx = 0, fy = 0, fw = 0, fh = 0;
    wl64_frame64(s, &fx, &fy, &fw, &fh);
    wl64_begin64();
    wl64_puts64("[WL64] ctl id=");    wl64_udec64(s->id);
    wl64_puts64(" pid=");             wl64_dec64(pid);
    wl64_puts64(" action=");
    wl64_puts64((action == WL64_CTL_COLLAPSE64) ? "collapse" :
                (action == WL64_CTL_MAX64)      ? "maximize" :
                (action == WL64_CTL_MIN2DOCK64) ? "minimize" : "restore");
    wl64_puts64(" by=");              wl64_puts64(by_kernel ? "kernel" : owner ? "owner" : "composer");
    wl64_puts64(" collapsed=");       wl64_udec64((uint64_t)(s->collapsed ? 1 : 0));
    wl64_puts64(" maxed=");           wl64_udec64((uint64_t)(s->maxed ? 1 : 0));
    wl64_puts64(" minimized=");       wl64_udec64((uint64_t)(s->minimized ? 1 : 0));
    wl64_puts64(" frame=");           wl64_dec64(fx); wl64_puts64(","); wl64_dec64(fy);
    wl64_puts64(",");                 wl64_dec64(fw); wl64_puts64(","); wl64_dec64(fh);
    wl64_nl64();
    wl64_end64();
    return 0;
}

// 29 wl_surface_title(surf, out, cap)：把标题栏文字读给合成器（★ v0.11.2）
//   为什么需要：装饰归合成器画，标题文字得由合成器渲染 —— 它必须有办法**读到**标题。
//   （28 号是客户端写给内核，29 号是合成器从内核读回；方向相反，不合并。）
int64_t wl64_surface_title64(uint64_t surf, uint64_t out_uptr, uint64_t cap) {
    const int64_t pid = wl64_pid64();
    if (pid <= 0) return WL64_EPERM64;
    Wl64Surface* s = wl64_find64((uint32_t)surf);
    if (!s) return WL64_ENOENT64;
    const bool owner = (s->pid == (int32_t)pid);
    if (!owner && !wl64_is_wm64(pid)) return WL64_EPERM64;
    if (cap == 0 || cap > 64) return WL64_EINVAL64;
    if (!user64_range_ok64(out_uptr, cap)) return WL64_EFAULT64;
    uint64_t n = 0;
    while (n + 1 < cap && s->title[n]) {
        *(char*)(uintptr_t)(out_uptr + n) = s->title[n];
        n++;
    }
    *(char*)(uintptr_t)(out_uptr + n) = 0;
    return (int64_t)(n + 1);
}

// ==================== ★ v0.11.4：31/32/33（中文标题位图 / 应用身份 / 应用图标）====================
// 语义与权限见 kernel/wl64.h 的 WL64_ABI_SURFACE_TITLEALPHA64/SETAPP64/APPICON64 段。
static uint32_t g_wl64_title_logged64 = 0;      // 31 号打点预算（前 16 条全打，之后每 512 条一条）
static int wl64_title_is_utf8_64(const char* s) {
    if (!s) return 0;
    for (int i = 0; s[i]; i++) if ((uint8_t)s[i] >= 0x80) return 1;   // 有非 ASCII 字节 = 需要 CJK 字形
    return 0;
}
// 这一节的**存在理由**：/bin/wm 的装载窗口只有 64 KiB，装不下 stb_truetype（font64.o 94,800 B），
//   v0.11.3 只能给它一张构建期预光栅化的 ASCII 字模 —— 结果"中文标题在 wm 路下不显示"。
//   这里把字形/图标的所有权收回到内核（唯一来源），合成器只做 alpha 混合。
static uint8_t g_wl64_title_bmp64[WL64_TITLE_BMP_W64 * WL64_TITLE_BMP_H64];

int64_t wl64_surface_title_alpha64(uint64_t surf, uint64_t out_uptr, uint64_t cap) {
    const int64_t pid = wl64_pid64();
    if (pid <= 0) return WL64_EPERM64;
    Wl64Surface* s = wl64_find64((uint32_t)surf);
    if (!s) return WL64_ENOENT64;
    const bool owner = (s->pid == (int32_t)pid);
    if (!owner && !wl64_is_wm64(pid)) return WL64_EPERM64;
    const uint64_t need = (uint64_t)WL64_TITLE_BMP_W64 * (uint64_t)WL64_TITLE_BMP_H64;
    if (cap < need) return WL64_EINVAL64;
    if (!user64_range_ok64(out_uptr, need)) return WL64_EFAULT64;
    const int w = font_raster_text64(s->title, g_wl64_title_bmp64, (int)WL64_TITLE_BMP_W64,
                                     (int)WL64_TITLE_BMP_W64, (int)WL64_TITLE_BMP_H64);
    for (uint64_t i = 0; i < need; i++) *(uint8_t*)(uintptr_t)(out_uptr + i) = g_wl64_title_bmp64[i];
    /* ★ v0.11.4：数一下**真的落了墨**的像素（ink）。为什么要有这个数：宽 w 只说明"推进了几像素"，
     *   字形缺失时 font_raster_text64 仍然按 em 宽度推进 —— 光看 w 无法区分"画了字"与"全是空白"。
     *   ink > 0 才是"这套字形真的画得出来"的硬证据（中文标题就靠它，见 tests/viewer64_test.py）。 */
    uint32_t ink = 0;
    for (uint64_t i = 0; i < need; i++) if (g_wl64_title_bmp64[i]) ink++;
    /* 打点：只在"这一条 surface 的位图宽变了"或还没打过时打一行 —— 合成器每帧都会来取，
     *   逐帧打会刷爆 COM1（每个 outb 一次 VM exit）。 */
    if (!s->alpha_logged || s->alpha_log_w != (uint32_t)w) {
        const bool first = !s->alpha_logged;
        s->alpha_logged = 1;
        s->alpha_log_w = (uint32_t)w;
        if (first || wl64_hot_log64(&g_wl64_title_logged64, 16u, 512u)) {
            wl64_begin64();
            wl64_puts64("[WL64] title bmp surf="); wl64_udec64(s->id);
            wl64_puts64(" pid=");                  wl64_dec64(pid);
            wl64_puts64(" w=");                    wl64_dec64(w);
            wl64_puts64(" h=");                    wl64_udec64((uint64_t)WL64_TITLE_BMP_H64);
            wl64_puts64(" bytes=");                wl64_udec64(need);
            wl64_puts64(" utf8=");                 wl64_udec64((uint64_t)wl64_title_is_utf8_64(s->title));
            wl64_puts64(" ink=");                  wl64_udec64((uint64_t)ink);
            wl64_puts64(" title=\"");              wl64_puts64(s->title); wl64_puts64("\"");
            wl64_nl64();
            wl64_end64();
        }
    }
    return ((int64_t)w << 16) | (int64_t)WL64_TITLE_BMP_H64;
}

int64_t wl64_surface_set_app64(uint64_t surf, uint64_t name_uptr) {
    const int64_t pid = wl64_pid64();
    if (pid <= 0) return WL64_EPERM64;
    Wl64Surface* s = wl64_find64((uint32_t)surf);
    if (!s) return WL64_ENOENT64;
    const bool owner = (s->pid == (int32_t)pid);
    if (!owner && !wl64_is_wm64(pid)) return WL64_EPERM64;
    char nb[WL64_APP_NAME_MAX64];
    for (uint32_t i = 0; i < (uint32_t)sizeof(nb); i++) nb[i] = 0;
    if (name_uptr) {
        for (uint32_t n = 0; n + 1u < (uint32_t)sizeof(nb); n++) {
            if (!user64_range_ok64(name_uptr + (uint64_t)n, 1)) return WL64_EFAULT64;
            const char c = *(const char*)(uintptr_t)(name_uptr + (uint64_t)n);
            nb[n] = c;
            if (!c) break;
        }
        nb[sizeof(nb) - 1u] = 0;
    }
    int kind = nb[0] ? icons64_kind_by_name64(nb) : 0;
    if (kind < 0) kind = 0;                 // 不认识的图标名 = 没有图标（不报错：身份是可选装饰）
    bool changed = (s->app_kind != (uint32_t)kind);
    if (changed) for (uint32_t i = 0; i < (uint32_t)sizeof(s->app_name); i++) {
        if (s->app_name[i] != nb[i]) { changed = true; break; }
    }
    s->app_kind = (uint32_t)kind;
    for (uint32_t i = 0; i < (uint32_t)sizeof(s->app_name); i++) s->app_name[i] = nb[i];
    s->deco_logged = 0;
    if (s->shm_id) s->pending = 1;
    /* Dock 运行项的图标跟着走（外部窗口在 28 号就登记了运行项，那时还不知道应用身份） */
    if (s->decor & WL64_DECOR_F_WINDOW64) dock64_ext_icon64((int)s->id, kind);
    if (changed) {
        wl64_begin64();
        wl64_puts64("[WL64] app id=");  wl64_udec64(s->id);
        wl64_puts64(" pid=");           wl64_dec64(pid);
        wl64_puts64(" name=\"");        wl64_puts64(s->app_name); wl64_puts64("\"");
        wl64_puts64(" kind=");          wl64_dec64(kind);
        wl64_puts64(" icon=\"");        wl64_puts64(kind ? icons64_kind_name64(kind) : "-"); wl64_puts64("\"");
        wl64_nl64();
        wl64_end64();
    }
    return 0;
}

int64_t wl64_surface_app_icon64(uint64_t surf, uint64_t out_uptr, uint64_t cap) {
    const int64_t pid = wl64_pid64();
    if (pid <= 0) return WL64_EPERM64;
    Wl64Surface* s = wl64_find64((uint32_t)surf);
    if (!s) return WL64_ENOENT64;
    const bool owner = (s->pid == (int32_t)pid);
    if (!owner && !wl64_is_wm64(pid)) return WL64_EPERM64;
    if (!s->app_kind) return -1;                       // 没有身份 = 没有图标（合成器如实不画）
    const uint64_t px = (uint64_t)WL64_APP_ICON_PX64;
    const uint64_t need = px * px * 4u;
    if (cap < need) return WL64_EINVAL64;
    if (!user64_range_ok64(out_uptr, need)) return WL64_EFAULT64;
    static uint8_t g_wl64_icon_rgba64[WL64_APP_ICON_PX64 * WL64_APP_ICON_PX64 * 4];
    if (icons64_export_rgba64((int)s->app_kind, (int)px, g_wl64_icon_rgba64,
                              (int)px, (int)px, icons64_app_color64((int)s->app_kind)) != 0) {
        return -1;                                     // 图标包里没有这张图（如实返回，不画）
    }
    for (uint64_t i = 0; i < need; i++) *(uint8_t*)(uintptr_t)(out_uptr + i) = g_wl64_icon_rgba64[i];
    return (int64_t)s->app_kind;
}

// 诊断：当前注册的合成器 pid（0 = 没有）
int wl64_composer_pid64() { return g_wl64_wm_pid64; }
// ★ v0.11.3：活动窗口 = 最高 id 的、有内容且未最小化的 surface（0 = 没有窗口）。
//   Dock 用它给"当前活动窗口"那条运行项画高亮；窗口最小化/关闭后自动落回下一个。
int wl64_active_surface64() {
    uint32_t best = 0;
    for (uint32_t i = 0; i < WL64_MAX_SURFACES64; i++) {
        const Wl64Surface* s = &g_wl64_surfs[i];
        if (!s->used || !s->content || s->minimized) continue;
        if (s->id > best) best = s->id;
    }
    return (int)best;
}
// ==================== 初始化 / 标定 / 自检 ====================
// rdtsc -> us 标定：用 PIT tick（250Hz）量一小段时间的周期数。为了不浪费启动时间只量 ~20ms。
// 标定失败（dt=0 / tsc 不动）时 g_wl64_tsc_per_ms64 = 0 -> us 打 0（如实，不编数）。
static void wl64_calib64() {
    const uint64_t t0 = wl64_rdtsc64();
    const uint64_t k0 = g_ticks64;
    if (task_sleep_ms64) {
        task_sleep_ms64(20);
    } else {
        uint64_t guard = 0;
        while ((g_ticks64 - k0) < 5u && ++guard < 200000000ULL) __asm__ volatile("hlt");
    }
    const uint64_t dt = g_ticks64 - k0;
    const uint64_t t1 = wl64_rdtsc64();
    if (dt > 0 && t1 > t0) g_wl64_tsc_per_ms64 = (t1 - t0) / (dt * (uint64_t)TICK_MS_64);
    else                   g_wl64_tsc_per_ms64 = 0;
}

// 位掩码：bit0 ABI 布局 / bit1 纯函数（矩形） / bit2 无进程上下文的负例 / bit3 几何在屏内 /
//         bit4 ★ B-wm 合成器 ABI（22..26 的结构体布局 + 映射窗不与后备缓冲重叠）
int wl64_selftest64() {
    int fail = 0;

    // ---- bit0：ABI 布局与常量自洽 ----
    if (sizeof(Wl64SeatEvent) != 64) fail |= 1;
    if (offsetof(Wl64SeatEvent, ev) != 0 || offsetof(Wl64SeatEvent, seat) != 40 ||
        offsetof(Wl64SeatEvent, surf) != 44 || offsetof(Wl64SeatEvent, sx) != 48 ||
        offsetof(Wl64SeatEvent, inside) != 56) fail |= 1;
    if ((uint64_t)WL64_MAX_PIXELS64 * 4u != SHM64_SLOT_BYTES64) fail |= 1;   // = shm 单对象上限（v0.5.0：2 MiB）
    if (WL64_MAX_FRAMES64 != SHM64_MAX_PAGES64) fail |= 1;                   // = 512 页
    if (WL64_MAX_SLOTS64 < 2) fail |= 1;                             // 要能演示重叠
    if (wl64_slot_dx64(0) >= wl64_slot_dx64(1)) fail |= 1;            // 槽位 1 在槽位 0 右边（重叠的前提）
    if (wl64_slot_dy64(0) >= wl64_slot_dy64(1)) fail |= 1;

    // ---- bit1：纯函数（求交/并集/命中）----
    {
        int x = 0, y = 0, w = 100, h = 50;
        if (!wl64_rect_isect64(&x, &y, &w, &h, 10, 10, 20, 20)) fail |= 2;
        if (x != 10 || y != 10 || w != 20 || h != 20) fail |= 2;        // 目标完全在裁剪框内 -> 原样
        x = -50; y = -50; w = 60; h = 60;
        if (!wl64_rect_isect64(&x, &y, &w, &h, 0, 0, 40, 40)) fail |= 2;
        if (x != 0 || y != 0 || w != 10 || h != 10) fail |= 2;           // 越界被裁到 (0,0,10,10)
        x = 100; y = 100; w = 10; h = 10;
        if (wl64_rect_isect64(&x, &y, &w, &h, 0, 0, 40, 40)) fail |= 2;  // 完全在外 -> 0
        x = 0; y = 0; w = 0; h = 0;
        wl64_rect_union64(&x, &y, &w, &h, 20, 30, 10, 5);
        if (x != 20 || y != 30 || w != 10 || h != 5) fail |= 2;          // 空矩形 -> 取另一个
        wl64_rect_union64(&x, &y, &w, &h, 25, 32, 20, 20);
        if (x != 20 || y != 30 || w != 25 || h != 22) fail |= 2;         // 包围盒
        if (!wl64_in_rect64(5, 5, 0, 0, 10, 10)) fail |= 2;
        if (wl64_in_rect64(10, 5, 0, 0, 10, 10)) fail |= 2;              // 半开区间：右边界不算命中
        if (wl64_in_rect64(5, 5, 0, 0, 0, 10)) fail |= 2;                // 空矩形永远不命中
    }

    // ---- bit2：没有进程上下文时必须是负错误码（不崩、不建 surface）----
    if (wl64_pid64() <= 0) {
        if (wl64_surface_create64(160, 96, 0) != WL64_EPERM64) fail |= 4;
        if (wl64_surface_attach64(1, 1, 0) != WL64_EPERM64) fail |= 4;
        if (wl64_surface_damage64(1, 0x0000001000000010ULL, 0x0000001000000010ULL) != WL64_EPERM64) fail |= 4;
        if (wl64_surface_commit64(1) != WL64_EPERM64) fail |= 4;
        if (wl64_surface_destroy64(1) != WL64_EPERM64) fail |= 4;
        if (wl64_seat_get64() != WL64_EPERM64) fail |= 4;
        if (wl64_display_dispatch64(0, 0, 1) != WL64_EPERM64) fail |= 4;
        // ★ B-wm：合成器专属调用在**没有合成器 / 没有进程上下文**时一律 EPERM（不假装有合成器）
        if (wl64_surface_export64(0, 0) != WL64_EPERM64) fail |= 4;
        if (wl64_surface_map64(1, 0) != WL64_EPERM64) fail |= 4;
        if (wl64_surface_ack64(1) != WL64_EPERM64) fail |= 4;
        if (wl64_seat_post64(0) != WL64_EPERM64) fail |= 4;
        if (wl64_composer_get64(0) != WL64_EPERM64) fail |= 4;   // 没有进程上下文：不许注册成功
        if (wl64_surface_move64(1, 0) != WL64_EPERM64) fail |= 4; // ★ 27 号同样不许凭空挪窗口
        if (wl64_surface_set_decor64(1, 1, 0) != WL64_EPERM64) fail |= 4;  // ★ 28 号：不许凭空设装饰
        // ★ v0.11.3：权限模型变了 —— **内核上下文（pid<=0）也允许调 30 号**（Dock 点击那条路没有进程）。
        //   所以这里没有 1 号 surface 时的正确答案是"找不到"（ENOENT），不再是"没权限"。
        if (wl64_surface_ctl64(1, 1) != WL64_ENOENT64) fail |= 4;
    }

    // ---- bit3：面板/槽位几何在屏内（放得下才检查）----
    if (g_wl64_panel_ok64) {
        const int fw = fb_width(), fh = fb_height();
        if (g_wl64_px64 < 0 || g_wl64_py64 < 0) fail |= 8;
        if (g_wl64_px64 + g_wl64_pw64 > fw || g_wl64_py64 + g_wl64_ph64 > fh) fail |= 8;
        if (g_wl64_pw64 < 64 || g_wl64_ph64 < 64) fail |= 8;
        for (uint32_t slot = 0; slot < WL64_MAX_SLOTS64; slot++) {
            const int sx = g_wl64_px64 + (int)wl64_slot_dx64(slot);
            const int sy = g_wl64_py64 + (int)wl64_slot_dy64(slot);
            if (sx < g_wl64_px64 || sy < g_wl64_py64) fail |= 8;
            if (sx + 64 > g_wl64_px64 + g_wl64_pw64 || sy + 64 > g_wl64_py64 + g_wl64_ph64) fail |= 8;
        }
    }
    // ---- bit4 起：与面板无关（面板放不下时也要查 ABI 布局）----

    // ---- bit4：★ B-wm 合成器 ABI（22..26）的结构体布局 + 映射窗不与后备缓冲重叠 ----
    if (WL64_SURFINFO_SIZE64 != 64 || WL64_SURFMAP_SIZE64 != 16 || WL64_RECT_SIZE64 != 16) fail |= 16;
    if (sizeof(Wl64SurfaceInfo) != 64 || sizeof(Wl64SurfaceMap) != 16 || sizeof(Wl64Rect) != 16) fail |= 16;
    if (offsetof(Wl64SurfaceInfo, id) != 0 || offsetof(Wl64SurfaceInfo, pid) != 4 ||
        offsetof(Wl64SurfaceInfo, x) != 16 || offsetof(Wl64SurfaceInfo, shm_id) != 24 ||
        offsetof(Wl64SurfaceInfo, dmg_x) != 40 || offsetof(Wl64SurfaceInfo, flags) != 56) fail |= 16;
    if (offsetof(Wl64SurfaceMap, va) != 0 || offsetof(Wl64SurfaceMap, bytes) != 8 ||
        offsetof(Wl64SurfaceMap, pages) != 12) fail |= 16;
    if (WL64_SURF_WIN_BYTES64 != (uint64_t)WL64_MAX_SURFACES64 * WL64_SURF_WIN_STRIDE64) fail |= 16;
    // ★ 不变量：映射窗步长必须装得下单 surface 的最大缓冲（像素上限 * 4B）
    if (WL64_SURF_WIN_STRIDE64 < (uint64_t)WL64_MAX_PIXELS64 * 4ULL) fail |= 16;
    if (WL64_ABI_SURFACE_MOVE64 != 27) fail |= 16;      // ★ W3 第二批：27 = wl_surface_move
    if (WL64_ABI_SURFACE_DECOR64 != 28) fail |= 16;     // ★ v0.11.1：28 = wl_surface_set_decor
    if (WL64_ABI_SURFACE_CTL64 != 30) fail |= 16;       // ★ v0.11.2：30 = wl_surface_ctl
    if (WL64_EV_CLOSE64 < 7u) fail |= 16;             // 事件类型不能与 input64 的 1..6 撞
    if (WL64_DECOR_H64 != 25) fail |= 16;               // 装饰占高必须 == gui64 的 TITLE_H + BORDER
    if ((uint32_t)gui64_deco_h64() != WL64_DECOR_H64) fail |= 16;   // 同一把尺子（两边不许漂）
    if (WL64_MOVE_KEEP64 < 1 || WL64_MOVE_KEEP64 > 128) fail |= 16;   // 夹取余量必须"小但非零"
    {
        // 映射窗必须在 FB 映射区内，且**不压到后备缓冲**（否则合成器会把自己映射进显存里）
        int bb_bytes = 0;
        (void)fb_surface_phys64(&bb_bytes);
        if (wl64_surf_win_base64() < USER64_FB_VA64) fail |= 16;
        // ★ 不许压在 DRM 映射窗上（drm64.h：+8MiB .. +40MiB，4 槽 × 8 MiB）
        if (wl64_surf_win_base64() < USER64_FB_VA64 + USER64_FB_DRM_END64) fail |= 16;
        if (wl64_surf_win_base64() + WL64_SURF_WIN_BYTES64 > USER64_FB_VA64 + USER64_FB_BYTES64) fail |= 16;
        if (bb_bytes > 0 && (uint64_t)bb_bytes > wl64_surf_win_base64() - USER64_FB_VA64) fail |= 16;
        if (wl64_surf_win_va64((uint32_t)WL64_MAX_SURFACES64 - 1u) + WL64_SURF_WIN_STRIDE64 >
            USER64_FB_VA64 + USER64_FB_BYTES64) fail |= 16;
    }
    if (g_wl64_wm_pid64 != 0) fail |= 16;        // 自检时不该有合成器（注册只走 22 号）
    return fail;
}

void wl64_init64() {
    wl64_layout64();
    wl64_calib64();
    const int sf = wl64_selftest64();
    wl64_begin64();
    wl64_puts64("[WL64] init fb=");   wl64_udec64((uint64_t)fb_width());
    wl64_puts64("x");                 wl64_udec64((uint64_t)fb_height());
    wl64_puts64(" panel=");           wl64_dec64(g_wl64_px64); wl64_puts64(","); wl64_dec64(g_wl64_py64);
    wl64_puts64(",");                 wl64_dec64(g_wl64_pw64); wl64_puts64(","); wl64_dec64(g_wl64_ph64);
    wl64_puts64(" slots=");           wl64_udec64((uint64_t)WL64_MAX_SLOTS64);
    wl64_puts64(" surf_max=");        wl64_udec64((uint64_t)WL64_MAX_SURFACES64);
    wl64_puts64(" out=");             wl64_udec64((uint64_t)(g_wl64_panel_ok64 ? 1 : 0));
    wl64_puts64(" tsc_per_ms=");      wl64_udec64(g_wl64_tsc_per_ms64);
    wl64_nl64();
    wl64_end64();
    wl64_begin64();
    if (sf == 0) {
        wl64_puts64("[WL64] selftest PASS (abi 15..21 + composer 22..26 + move 27 + decor 28 + ctl 30 / events 64B / info 64B / rect clip / no-proc EPERM)\n");
    } else {
        wl64_puts64("[WL64] selftest FAIL mask=");
        wl64_udec64((uint64_t)sf);
        wl64_nl64();
    }
    wl64_end64();
}

// ==================== 启动期演示（/wlclient.elf：**卷交付**）====================
// 交付方式见文件头：/wlclient.elf 不内嵌，是系统卷里的文件（夹具写进去）。这里只**查在不在**：
// 不在就返回 0 并打一行跳过（调用方再打 demo skipped），在就把 vol_bytes 带回去给 demo 打点用。
static int wl64_client_probe64(const char* path, uint32_t* out_bytes) {
    uint32_t t = 0, sz = 0;
    const int sys = vfs64_system_slot64();
    if (sys < 0) return 0;
    if (vfs64_stat_on64(sys, path, &t, &sz) != 0) return 0;
    if (out_bytes) *out_bytes = sz;
    wl64_begin64();
    wl64_puts64("[WL64] client on volume path="); wl64_puts64(path);
    wl64_puts64(" bytes=");                       wl64_udec64(sz);
    wl64_nl64();
    wl64_end64();
    return 1;
}

int wl64_demo64(const char* path) {
    const char* p = (path && path[0] == '/') ? path : WL64_CLIENT_PATH64;
    wl64_init64();                                    // 几何 + 标定 + 自检（打点）
    if (proc64_isolate64() == 0) {
        wl64_begin64();
        wl64_puts64("[WL64] demo skipped (shared address space mode: no per-process address space)\n");
        wl64_end64();
        return 0;
    }
    bool wl_sys_ok = (vfs64_mount_system64(0, app64_main_part_lba64(0)) == 0);
    if (!wl_sys_ok) {
        for (int wsd = 1; wsd < ata64_drive_count64(); wsd++) {
            const int wdd = ata64_slot_to_drive64(wsd);
            if (wdd <= 0) continue;
            const uint32_t wdl = app64_main_part_lba64(wdd);
            if (wdl == 0) continue;
            if (vfs64_mount_system64(wdd, wdl) == 0) {
                wl64_begin64();
                wl64_puts64("[WL64] system volume drive="); wl64_udec64((uint64_t)wdd);
                wl64_puts64(" scanned=1 (drive 0 has no VimtuFS2 volume; this disk is not on PATA)\n");
                wl64_end64();
                wl_sys_ok = true;
                break;
            }
        }
    }
    if (!wl_sys_ok) {
        wl64_begin64();
        wl64_puts64("[WL64] demo skipped (no system volume)\n");
        wl64_end64();
        return 0;
    }
    uint32_t t = 0, sz = 0, vol_bytes = 0;
    // 探针文件 + 客户端在不在卷里（两者都是**卷里的文件**，见文件头的交付说明）
    const bool want_full = (vfs64_stat(WL64_PROBE_PATH64, &t, &sz) == 0);
    if (!wl64_client_probe64(p, &vol_bytes)) {
        wl64_begin64();
        wl64_puts64("[WL64] demo skipped (no elf on vfs) path="); wl64_puts64(p);
        wl64_nl64();
        wl64_end64();
        return 0;
    }
    const bool full = want_full;
    // argv 只是留档：客户端的模式判据与内核**同一个文件**（/etc/wl64_probe），不靠 argv
    // （user/lib/crt0.S 会把 argc/argv 合成成 1/"user64" —— 见 build64.sh 里的说明）。
    const char* av_short[1];
    const char* av_full[2];
    const char* const* av = av_short;
    uint32_t argc = 1;
    av_short[0] = p;
    if (full) { av_full[0] = p; av_full[1] = "--full"; av = av_full; argc = 2; }

    const uint64_t free_before = page_count_free_64();
    const int pid = proc64_create64("wlclient", 0);
    if (pid < 0) {
        wl64_begin64();
        wl64_puts64("[WL64] demo skipped (create failed)\n");
        wl64_end64();
        return 0;
    }
    if (proc64_start_elf64_argv64(pid, p, av, argc) != 0) {
        proc64_destroy64(pid);
        wl64_begin64();
        wl64_puts64("[WL64] demo skipped (start failed)\n");
        wl64_end64();
        return 0;
    }
    wl64_begin64();
    wl64_puts64("[WL64] demo start pid=");  wl64_udec64((uint64_t)pid);
    wl64_puts64(" mode=");                  wl64_puts64(full ? "full" : "short");
    wl64_puts64(" from_volume=");          wl64_udec64(vol_bytes);
    wl64_puts64(" pool_free=");            wl64_udec64(free_before);
    wl64_nl64();
    wl64_end64();

    // 有界等待（full 模式最长 ~90 s：客户端自己 3 阶段；short 模式 ~20 s）
    const uint32_t limit_sec = full ? 90u : 20u;
    const uint64_t t0 = g_ticks64;
    while (proc64_sig_state_of64(pid) != nullptr &&
           (g_ticks64 - t0) < (uint64_t)PIT_HZ_64 * (uint64_t)limit_sec) {
        if (task_sleep_ms64) task_sleep_ms64(2);
        else { const uint64_t w = g_ticks64 + 1; uint64_t g = 0; while (g_ticks64 < w && ++g < 50000000ULL) __asm__ volatile("hlt"); }
    }
    const int exited = (proc64_sig_state_of64(pid) == nullptr) ? 1 : 0;
    if (!exited) {
        wl64_begin64();
        wl64_puts64("[WL64] demo TIMEOUT pid="); wl64_udec64((uint64_t)pid);
        wl64_puts64(" (still running)\n");
        wl64_end64();
        (void)proc64_kill64(pid, 9);
    }
    wl64_proc_release64(pid);                       // 兜底回收（正常路径客户端已经销毁过：幂等、没表面时静默）
    if (proc64_find64(pid)) proc64_destroy64(pid);
    const uint64_t free_after = page_count_free_64();
    wl64_begin64();
    wl64_puts64("[WL64] demo done pid=");   wl64_udec64((uint64_t)pid);
    wl64_puts64(" exited=");                wl64_udec64((uint64_t)exited);
    wl64_puts64(" ticks=");                 wl64_udec64(g_ticks64 - t0);
    wl64_puts64(" pool_free=");             wl64_udec64(free_after);
    wl64_puts64(" pool_delta=");
    if (free_after >= free_before) { wl64_puts64("+"); wl64_udec64(free_after - free_before); }
    else                           { wl64_puts64("-"); wl64_udec64(free_before - free_after); }
    wl64_puts64(" surfs=");                 wl64_udec64((uint64_t)g_wl64_live64);
    wl64_puts64(" seat_users=");            wl64_udec64(g_wl64_seat_users64);
    wl64_puts64(" seat_logged=");           wl64_udec64(g_wl64_seat_logged64);
    wl64_puts64(" seat_supp=");             wl64_udec64(g_wl64_seat_supp64);
    wl64_nl64();
    wl64_end64();
    return 0;
}

// ==================== ★ B-wm：Ring 3 合成器的启动期驱动（/bin/wm + 两个客户端：**卷交付**）====
// 交付纪律与 /wlclient.elf 完全相同：三个 ELF **都不内嵌进内核**，是系统卷里的文件（验收夹具/
// tools/wm_pack_win.py 写进卷）；卷里没有 /bin/wm.elf 就如实打一行 [WL64] wm skipped 并返回。
// 模式判据与客户端**同一个文件**（/etc/wm_probe）：有 = full（再起 wmclock/wmpanel 两个真客户端，
// 有界等待 90 s）；没有 = short（只起 wm 一个，它自己映射 fb + 注册合成器 + 合成约 1 s 就退出）。
static const char WL64_WM_PATH64[]      = "/bin/wm.elf";
// ★ 修：构建实际把合成器写在 /lib/wm.elf（build64.sh），契约路径是 /bin/wm.elf；
//   两个都认，卷里谁在就用谁（否则打包路径一变，合成器就被整个跳过）。
static const char WL64_WM_LIB64[]       = "/lib/wm.elf";
static const char WL64_WMCLOCK_PATH64[] = "/wmclock.elf";
static const char WL64_WMPANEL_PATH64[] = "/wmpanel.elf";
static const char WL64_WM_PROBE64[]     = "/etc/wm_probe";

// 起一个 ring3 进程（不等待）：成功返回 pid（>=1），失败打点并返回 0。
static int wl64_wm_spawn64(const char* name, const char* path) {
    const int pid = proc64_create64(name, 0);
    if (pid < 0) {
        wl64_begin64();
        wl64_puts64("[WL64] wm spawn FAILED name="); wl64_puts64(name);
        wl64_puts64(" reason=create err="); wl64_dec64(pid);
        wl64_nl64();
        wl64_end64();
        return 0;
    }
    if (proc64_start_elf64(pid, path) != 0) {
        proc64_destroy64(pid);
        wl64_begin64();
        wl64_puts64("[WL64] wm spawn FAILED name="); wl64_puts64(name);
        wl64_puts64(" path="); wl64_puts64(path);
        wl64_puts64(" reason=start"); wl64_nl64();
        wl64_end64();
        return 0;
    }
    return pid;
}

int wl64_wm64(const char* wm_path) {
    const char* p = (wm_path && wm_path[0] == '/') ? wm_path : WL64_WM_PATH64;
    wl64_init64();                                    // 几何 + 标定 + 自检（含 composer ABI 位）
    if (proc64_isolate64() == 0) {
        wl64_begin64();
        wl64_puts64("[WL64] wm skipped (shared address space mode: no per-process address space)\n");
        wl64_end64();
        return 0;
    }
    bool wl_sys_ok = (vfs64_mount_system64(0, app64_main_part_lba64(0)) == 0);
    if (!wl_sys_ok) {
        for (int wsd = 1; wsd < ata64_drive_count64(); wsd++) {
            const int wdd = ata64_slot_to_drive64(wsd);
            if (wdd <= 0) continue;
            const uint32_t wdl = app64_main_part_lba64(wdd);
            if (wdl == 0) continue;
            if (vfs64_mount_system64(wdd, wdl) == 0) {
                wl64_begin64();
                wl64_puts64("[WL64] system volume drive="); wl64_udec64((uint64_t)wdd);
                wl64_puts64(" scanned=1 (drive 0 has no VimtuFS2 volume; this disk is not on PATA)\n");
                wl64_end64();
                wl_sys_ok = true;
                break;
            }
        }
    }
    if (!wl_sys_ok) {
        wl64_begin64();
        wl64_puts64("[WL64] wm skipped (no system volume)\n");
        wl64_end64();
        return 0;
    }
    uint32_t t = 0, sz = 0, vol_bytes = 0;
    const bool full = (vfs64_stat(WL64_WM_PROBE64, &t, &sz) == 0);
    bool wl_wm_found = wl64_client_probe64(p, &vol_bytes);
    if (!wl_wm_found && wl64_client_probe64(WL64_WM_LIB64, &vol_bytes)) {
        p = WL64_WM_LIB64;
        wl_wm_found = true;
        wl64_begin64();
        wl64_puts64("[WL64] wm path fallback=/lib/wm.elf (/bin/wm.elf not in volume)\n");
        wl64_end64();
    }
    if (!wl_wm_found) {         // 复用既有的"卷里有这个文件吗"打点函数
        wl64_begin64();
        wl64_puts64("[WL64] wm skipped (no elf on vfs) path="); wl64_puts64(p);
        wl64_nl64();
        wl64_end64();
        return 0;
    }
    const uint64_t free_before = page_count_free_64();
    int pid[3] = { 0, 0, 0 };
    pid[0] = wl64_wm_spawn64("wm", p);
    if (pid[0] && full) {
        pid[1] = wl64_wm_spawn64("wmclock", WL64_WMCLOCK_PATH64);
        pid[2] = wl64_wm_spawn64("wmpanel", WL64_WMPANEL_PATH64);
    }
    wl64_begin64();
    wl64_puts64("[WL64] wm start pid=");    wl64_udec64((uint64_t)pid[0]);
    wl64_puts64(" mode=");                  wl64_puts64(full ? "full" : "short");
    wl64_puts64(" clients=");               wl64_udec64((uint64_t)((pid[1] ? 1 : 0) + (pid[2] ? 1 : 0)));
    wl64_puts64(" from_volume=");           wl64_udec64(vol_bytes);
    wl64_puts64(" pool_free=");             wl64_udec64(free_before);
    wl64_nl64();
    wl64_end64();
    if (!pid[0]) return 0;

    // 有界等待：合成器自己决定什么时候退出（full 最长 90 s；short 20 s 足够它映射几帧）
    const uint32_t limit_sec = full ? 90u : 20u;
    const uint64_t t0 = g_ticks64;
    while ((g_ticks64 - t0) < (uint64_t)PIT_HZ_64 * (uint64_t)limit_sec) {
        int live = 0;
        for (int i = 0; i < 3; i++) if (pid[i] && proc64_sig_state_of64(pid[i]) != nullptr) live++;
        if (live == 0) break;
        if (task_sleep_ms64) task_sleep_ms64(5);
        else { const uint64_t w = g_ticks64 + 1; uint64_t g = 0; while (g_ticks64 < w && ++g < 50000000ULL) __asm__ volatile("hlt"); }
    }
    // 收尾：没退出的强杀；surface/shm 兜底回收；每个进程都销毁（幂等）
    int exited = 0;
    for (int i = 0; i < 3; i++) {
        if (!pid[i]) continue;
        const bool gone = (proc64_sig_state_of64(pid[i]) == nullptr);
        if (gone) exited++;
        else {
            wl64_begin64();
            wl64_puts64("[WL64] wm TIMEOUT pid="); wl64_udec64((uint64_t)pid[i]);
            wl64_puts64(" still_running=1"); wl64_nl64();
            wl64_end64();
            (void)proc64_kill64(pid[i], 9);
        }
        wl64_proc_release64(pid[i]);                   // 幂等：正常路径客户端自己销毁过
        if (proc64_find64(pid[i])) proc64_destroy64(pid[i]);
    }
    const uint64_t free_after = page_count_free_64();
    wl64_begin64();
    wl64_puts64("[WL64] wm done procs=");      wl64_udec64((uint64_t)((pid[0] ? 1 : 0) + (pid[1] ? 1 : 0) + (pid[2] ? 1 : 0)));
    wl64_puts64(" exited=");                   wl64_udec64((uint64_t)exited);
    wl64_puts64(" ticks=");                    wl64_udec64(g_ticks64 - t0);
    wl64_puts64(" pool_free=");                wl64_udec64(free_after);
    wl64_puts64(" pool_delta=");
    if (free_after >= free_before) { wl64_puts64("+"); wl64_udec64(free_after - free_before); }
    else                           { wl64_puts64("-"); wl64_udec64(free_before - free_after); }
    wl64_puts64(" surfs=");                    wl64_udec64((uint64_t)g_wl64_live64);
    wl64_puts64(" composer=");                 wl64_udec64((uint64_t)(g_wl64_wm_pid64 ? 1 : 0));
    wl64_puts64(" exports=");                  wl64_udec64(g_wl64_wm_exports64);
    wl64_puts64(" maps=");                     wl64_udec64(g_wl64_wm_maps64);
    wl64_puts64(" acks=");                     wl64_udec64(g_wl64_wm_acks64);
    wl64_puts64(" posts=");                    wl64_udec64(g_wl64_wm_posts64);
    wl64_nl64();
    wl64_end64();
    return 0;
}
