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
#include "x86_64.h"       // g_ticks64 / PIT_HZ_64 / pt_regs64 无关
#include "fb.h"           // fb_width/fb_height/fb_surface64/fb_user_flip64（**只调用**）
#include "proc64.h"       // 进程上下文 + shm 对象的只读视图/hold/release
#include "usermode64.h"   // user64_range_ok64（seat 事件出参校验）
#include "mem_64.h"       // page_count_free_64（演示的页池基线核对）
#include "vfs64.h"        // 在系统卷里按路径找 /wlclient.elf（**不内嵌**，见下）
#include "app64.h"        // app64_main_part_lba64（挂系统卷时的分区起点）

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
};
static Wl64Surface g_wl64_surfs[WL64_MAX_SURFACES64];
static uint32_t    g_wl64_next_id64 = 1;
static uint32_t    g_wl64_live64    = 0;      // 活着的 surface 数（打点/诊断）
static uint32_t    g_wl64_created64 = 0;      // 累计创建（槽位 = created % WL64_MAX_SLOTS64）

// 面板（可见区域）几何：init 里按当前屏幕算一次
static int  g_wl64_px64 = 0, g_wl64_py64 = 0, g_wl64_pw64 = 0, g_wl64_ph64 = 0;
static int  g_wl64_panel_ok64 = 0;            // 1 = 面板可用（fb 已初始化且放得下）
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

// 日志节流（seat 路由行：前 WL64_SEAT_LOG_MAX64 条全打，之后每 32 条一条 + 结算 suppressed）
static const uint32_t WL64_SEAT_LOG_MAX64 = 200;
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
    g_wl64_px64 = (fw - pw) / 2;
    g_wl64_py64 = (fh - ph) / 2;
    if (g_wl64_px64 < 0) g_wl64_px64 = 0;
    if (g_wl64_py64 < 0) g_wl64_py64 = 0;
}
// 槽位 -> 屏幕位置（surface 创建时定死；组合器策略，见 wl64.h 第 6 条）
static void wl64_place64(Wl64Surface* s, uint32_t slot) {
    s->slot = slot % WL64_MAX_SLOTS64;
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
// 把 surface 的（已裁剪的）矩形拷进后备缓冲并提交上屏。
// 返回实际写入的像素字节数（0 = 完全被裁掉）。
static uint32_t wl64_blit64(Wl64Surface* s, uint32_t* dst, int dw, int dh) {
    // ---- 1) damage 矩形（局部）----
    int rx = 0, ry = 0, rw = (int)s->w, rh = (int)s->h;
    if (s->have_damage) { rx = s->dmg_x; ry = s->dmg_y; rw = s->dmg_w; rh = s->dmg_h; }
    // ---- 2) 裁剪：surface 矩形 ----
    if (!wl64_rect_isect64(&rx, &ry, &rw, &rh, 0, 0, (int)s->w, (int)s->h)) return 0;
    int dxr = rx + s->x, dyr = ry + s->y, dwr = rw, dhr = rh;
    // ---- 3) 裁剪：可见区域（面板）----
    if (!wl64_rect_isect64(&dxr, &dyr, &dwr, &dhr, g_wl64_px64, g_wl64_py64, g_wl64_pw64, g_wl64_ph64)) return 0;
    // ---- 4) 裁剪：屏幕 ----
    if (!wl64_rect_isect64(&dxr, &dyr, &dwr, &dhr, 0, 0, dw, dh)) return 0;
    // 局部坐标随之平移（源矩形与目标矩形要对齐）
    rx += dxr - (rx + s->x);
    ry += dyr - (ry + s->y);
    const uint32_t pitch = s->w * 4u;
    // ---- 5) 逐行：再减去"更高层 surface 的可见矩形"盖住的部分 ----
    const uint32_t id_self = s->id;
    int cov[4][2];
    uint32_t written = 0;
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    for (int yy = 0; yy < dhr; yy++) {
        const int sy = dyr + yy;
        int ncov = 0;
        for (uint32_t i = 0; i < WL64_MAX_SURFACES64 && ncov < 4; i++) {
            Wl64Surface* h = &g_wl64_surfs[i];
            if (!h->used || !h->content || h->id <= id_self) continue;
            int hx = h->x, hy = h->y, hw = (int)h->w, hh = (int)h->h;
            if (!wl64_rect_isect64(&hx, &hy, &hw, &hh, g_wl64_px64, g_wl64_py64, g_wl64_pw64, g_wl64_ph64)) continue;
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
    if (written == 0) {
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
    fb_user_flip64(bx0, by0, bx1 - bx0, by1 - by0);   // 组合器自己的提交（不受内核绘制开关影响）
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
        uint64_t tmp[WL64_MAX_FRAMES64];
        uint32_t pages = 0, refs = 0;
        (void)proc64_shm_release_id64(old, 0);          // pid=0：组合器（kernel/wl64.cpp）持有的引用
        if (proc64_shm_view64((uint32_t)old, tmp, WL64_MAX_FRAMES64, &pages, &refs) == 0) {
            refs_after = (int)refs;
        }
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
    uint64_t tmp[WL64_MAX_FRAMES64];
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
    const int64_t rc = ev64_poll64(0, 0, EV64_FLAG_FOCUS64 | EV64_FLAG_CAPTURE64);
    if (rc < 0) return rc;
    g_wl64_seat_users64++;
    *wl64_focus_slot64((int)pid) = 0;
    wl64_begin64();
    wl64_puts64("[WL64] seat get id="); wl64_udec64((uint64_t)WL64_SEAT_ID64);
    wl64_puts64(" pid=");               wl64_dec64(pid);
    wl64_puts64(" flags=3 (focus+capture) users=");
    wl64_udec64(g_wl64_seat_users64);
    wl64_nl64();
    wl64_end64();
    return (int64_t)WL64_SEAT_ID64;
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
    uint32_t bytes = 0;
    (void)wl64_flush64(&bytes);

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
            Wl64Surface* hs = wl64_hit64(e->x, e->y);
            if (hs && hs->pid == pid) {
                se.surf = hs->id;
                se.inside = 1;
                se.sx = e->x - hs->x;
                se.sy = e->y - hs->y;
                *wl64_focus_slot64((int)pid) = hs->id;      // 指针命中 -> 该 surface 成为键盘焦点 surface
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
            if ((g_wl64_seat_supp64 & 31u) == 0) {
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

// 位掩码：bit0 ABI 布局 / bit1 纯函数（矩形） / bit2 无进程上下文的负例 / bit3 几何在屏内
int wl64_selftest64() {
    int fail = 0;

    // ---- bit0：ABI 布局与常量自洽 ----
    if (sizeof(Wl64SeatEvent) != 64) fail |= 1;
    if (offsetof(Wl64SeatEvent, ev) != 0 || offsetof(Wl64SeatEvent, seat) != 40 ||
        offsetof(Wl64SeatEvent, surf) != 44 || offsetof(Wl64SeatEvent, sx) != 48 ||
        offsetof(Wl64SeatEvent, inside) != 56) fail |= 1;
    if (WL64_MAX_PIXELS64 * 4u != 65536u) fail |= 1;                  // = shm 单对象上限
    if (WL64_MAX_FRAMES64 != SHM64_MAX_PAGES64) fail |= 1;
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
        wl64_puts64("[WL64] selftest PASS (abi 15..21 / Wl64SeatEvent 64B / rect clip / no-proc EPERM)\n");
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
    if (vfs64_mount_system64(0, app64_main_part_lba64(0)) != 0) {
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
