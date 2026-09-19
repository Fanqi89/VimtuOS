// panic64.cpp - 64 位蓝屏（BSOD）+ 看门狗实现
//
// 绘制约定（像素断言要用）：
//   背景 = 0x000078D4（纯蓝，整屏铺满 fb_fill_rect(0,0,W,H)）
//   文字 = 0x00FFFFFF（纯白）
//   标题 = TrueType（face 0）画在 (48,56)（重画三遍做加粗，保证像素上很实）；
//   正文 = 8x8 位图字体 scale=2（16x16 像素/字）：第 1 行从 (48,102) 开始，行距 26px。
//   ★ 屏幕尺寸与行数会打在串口（[PANIC64] screen w=/h=/lines=），验收脚本据此取样。
//
// 停住语义（受控演示与真实异常一致）：画完 -> 串口现场 -> 停留 hold_ms -> cli + hlt 死循环。
//   不自动重启：QEMU 的验收都带 -no-reboot（一复位虚拟机就退出），而且蓝屏画面要给像素断言看。
#include "panic64.h"
#include "fb.h"           // fb_fill_rect / fb_draw_text / fb_flip / fb_width / fb_height
#include "font.h"         // 标题用 TrueType（保证是"真文字"，不是手画色块）
#include "x86_64.h"       // pt_regs64 / ticks64 / ms_to_ticks64 / rtc
#include "task64.h"       // task_create64 / task_name / sleep
#include "mem_64.h"       // heap_used_64 / heap_total_64
#include "sysstate64.h"   // 状态机（蓝屏现场里带上状态与代次）
#include "debug64.h"      // dbg64_* + 行锁

// ==================== 状态 ====================
static volatile bool     g_panicking = false;
static volatile bool     g_armed = false;
static volatile bool     g_suspended = false;
static volatile bool     g_paused = false;
static volatile uint32_t g_last_kick = 0;
static volatile int      g_fires = 0;
static volatile uint64_t g_wd_checks = 0;
static int               g_wd_task_id = -1;

// 真实 CPU 异常现场（panic64_cpu_exception64 填，bsod_common 打）
static volatile uint64_t g_exc_no = 0, g_exc_err = 0, g_exc_rip = 0, g_exc_cr2 = 0;
static volatile uint64_t g_exc_cs = 0, g_exc_rsp = 0;
static volatile bool     g_exc_valid = false;

int panic64_active64() { return g_panicking ? 1 : 0; }
int panic64_watchdog_armed64() { return g_armed ? 1 : 0; }
int panic64_watchdog_fires64() { return g_fires; }
uint32_t panic64_watchdog_stale_ms64() {
    return (uint32_t)((uint64_t)(ticks64() - g_last_kick) * TICK_MS_64);
}

static void log_str(const char* s) { dbg64_str(s); }
static void log_hex64v(uint64_t v) { dbg64_hex64(v); }

// ==================== 看门狗 ====================
static void wd_task(void* arg) {
    (void)arg;
    for (;;) {
        if (g_armed && !g_suspended && !g_paused && !g_panicking) {
            const uint32_t stale = (uint32_t)(ticks64() - g_last_kick);
            g_wd_checks++;
            // 每 ~10 秒打一行"还在跑且没超时"的证据（250ms * 40）
            if ((g_wd_checks % 40) == 1) {
                dbg64_line_begin64();
                log_str("[WD64] watchdog beat stale=");
                dbg64_dec((uint64_t)stale * TICK_MS_64);
                log_str("ms checks=");
                dbg64_dec(g_wd_checks);
                dbg64_nl();
                dbg64_line_end64();
            }
            if (stale > ms_to_ticks64(WD64_TIMEOUT_MS)) {
                g_fires++;
                dbg64_line_begin64();
                log_str("[WD64] watchdog fire stale=");
                dbg64_dec((uint64_t)stale * TICK_MS_64);
                log_str("ms timeout=");
                dbg64_dec(WD64_TIMEOUT_MS);
                log_str("ms");
                dbg64_nl();
                dbg64_line_end64();
                panic64_bsod64("WATCHDOG_TIMEOUT", (uint64_t)stale * TICK_MS_64);
            }
        }
        task_sleep64(WD64_POLL_MS);
    }
}

void panic64_init64() {
    if (g_wd_task_id >= 0) return;
    g_last_kick = ticks64();
    g_wd_task_id = task_create64("watchdog", wd_task, nullptr);
    dbg64_line_begin64();
    log_str("[WD64] watchdog task up id=");
    if (g_wd_task_id < 0) log_str("(none)");
    else                   dbg64_dec((uint64_t)g_wd_task_id);
    log_str(" period=");
    dbg64_dec(WD64_POLL_MS);
    log_str("ms timeout=");
    dbg64_dec(WD64_TIMEOUT_MS);
    log_str("ms");
    dbg64_nl();
    dbg64_line_end64();
}

void panic64_watchdog_arm64() {
    g_armed = true;
    g_last_kick = ticks64();
    dbg64_line_begin64();
    log_str("[WD64] watchdog armed timeout=");
    dbg64_dec(WD64_TIMEOUT_MS);
    log_str("ms source=gui64-frame (kernel/panic64.cpp)");
    dbg64_nl();
    dbg64_line_end64();
}

void panic64_watchdog_kick64() {
    if (g_armed && !g_panicking) g_last_kick = ticks64();
}

void panic64_watchdog_suspend64() {
    if (g_suspended) return;
    g_suspended = true;
    dbg64_line_begin64();
    log_str("[WD64] watchdog suspended (stop/restart in progress)");
    dbg64_nl();
    dbg64_line_end64();
}

void panic64_watchdog_resume64() {
    if (!g_suspended) return;
    g_suspended = false;
    g_last_kick = ticks64();
    dbg64_line_begin64();
    log_str("[WD64] watchdog resumed");
    dbg64_nl();
    dbg64_line_end64();
}

void panic64_watchdog_pause64()  { g_paused = true; g_last_kick = ticks64(); }
void panic64_watchdog_unpause64() { g_paused = false; g_last_kick = ticks64(); }

// ==================== BSOD 绘制 ====================
#define BSOD_BG   0x000078D4u
#define BSOD_FG   0x00FFFFFFu
#define BSOD_DIM  0x00D6E4F7u

// 逐行正文（固定容量；内核里没有 vsnprintf）
#define BSOD_LINES 14
#define BSOD_LINE_LEN 88
static char g_lines[BSOD_LINES][BSOD_LINE_LEN];
static int  g_line_n = 0;

static void line_reset() { g_line_n = 0; }
static char* line_new() {
    if (g_line_n >= BSOD_LINES) return nullptr;
    char* p = g_lines[g_line_n++];
    p[0] = 0;
    return p;
}
static void line_app(char* p, const char* s) {
    if (!p || !s) return;
    int n = 0;
    while (p[n]) n++;
    for (int i = 0; s[i] && n < BSOD_LINE_LEN - 1; i++) p[n++] = s[i];
    p[n] = 0;
}
static void line_dec(char* p, uint64_t v) {
    char t[24]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    char rev[24]; int m = 0;
    while (n > 0) rev[m++] = t[--n];
    rev[m] = 0;
    line_app(p, rev);
}
static void line_hex(char* p, uint64_t v) {
    static const char* H = "0123456789ABCDEF";
    char t[20];
    int n = 0;
    t[n++] = '0'; t[n++] = 'x';
    bool started = false;
    for (int i = 15; i >= 0; i--) {
        const int d = (int)((v >> (i * 4)) & 0xF);
        if (!started && d == 0 && i != 0) continue;
        started = true;
        t[n++] = H[d];
    }
    t[n] = 0;
    line_app(p, t);
}

static void bsod_draw(int* out_w, int* out_h) {
    const int W = fb_width(), H = fb_height();
    *out_w = W; *out_h = H;
    fb_reset_clip();                                   // 全屏（不受上次窗口裁剪影响）
    fb_fill_rect(0, 0, W, H, BSOD_BG);
    // 标题：大字（TrueType，scale 无 —— 用 face 0 + 大号字重画两遍做加粗）
    const int tx = 48, ty = 56;
    font_select(0);
    font_draw_text(tx, ty, ":(  VimtuOS 64-bit stopped", BSOD_FG);
    font_draw_text(tx + 1, ty, ":(  VimtuOS 64-bit stopped", BSOD_FG);
    font_draw_text(tx, ty + 1, ":(  VimtuOS 64-bit stopped", BSOD_FG);
    const int y0 = ty + 46;
    for (int i = 0; i < g_line_n; i++) {
        fb_draw_text(tx, y0 + i * 26, g_lines[i], i == 0 ? BSOD_FG : BSOD_FG, BSOD_BG, 2);
    }
    // 底部提示（8x8 位图字体，白底蓝字可读性够）
    fb_draw_text(tx, H - 56, "Collecting error information... the machine will not restart automatically.",
                 BSOD_DIM, BSOD_BG, 1);
    fb_flip();
}

// ==================== BSOD 主体（不返回）====================
[[noreturn]] static void bsod_common(const char* code, uint64_t detail, uint32_t hold_ms) {
    if (g_panicking) {                       // 已经蓝屏了：直接停住（不重复画）
        for (;;) __asm__ volatile("cli; hlt");
    }
    g_panicking = true;
    g_armed = false;                         // 停止看门狗（避免它重复触发）
    if (hold_ms == 0) hold_ms = 6000;

    // ---- 串口现场（第一行就是停止码，格式被验收脚本依赖）----
    dbg64_line_begin64();
    log_str("[PANIC64] stop=");
    log_str(code ? code : "UNKNOWN");
    log_str(" detail=");
    log_hex64v(detail);
    log_str(" state=");
    log_str(sysstate64_state_text64());
    log_str(" gen=");
    dbg64_dec((uint64_t)sysstate64_generation64());
    dbg64_nl();
    dbg64_line_end64();
    if (g_exc_valid) {
        dbg64_line_begin64();
        log_str("[PANIC64] cpu exception no=");
        dbg64_dec(g_exc_no);
        log_str(" err=");
        log_hex64v(g_exc_err);
        log_str(" rip=");
        log_hex64v(g_exc_rip);
        log_str(" cs=");
        log_hex64v(g_exc_cs);
        log_str(" rsp=");
        log_hex64v(g_exc_rsp);
        log_str(" cr2=");
        log_hex64v(g_exc_cr2);
        dbg64_nl();
        dbg64_line_end64();
    }

    // ---- 组织正文行 ----
    line_reset();
    char* p;
    p = line_new();
    if (p) { line_app(p, "Stop code: "); line_app(p, code ? code : "UNKNOWN"); }
    p = line_new();
    if (p) { line_app(p, "Detail: "); line_hex(p, detail); }
    if (g_exc_valid) {
        p = line_new();
        if (p) {
            line_app(p, "CPU exception: no="); line_dec(p, g_exc_no);
            line_app(p, " err="); line_hex(p, g_exc_err);
        }
        p = line_new();
        if (p) {
            line_app(p, "RIP="); line_hex(p, g_exc_rip);
            line_app(p, " CS="); line_hex(p, g_exc_cs);
        }
        p = line_new();
        if (p) {
            line_app(p, "RSP="); line_hex(p, g_exc_rsp);
            line_app(p, " CR2="); line_hex(p, g_exc_cr2);
        }
    }
    p = line_new();
    if (p) {
        line_app(p, "System state: "); line_app(p, sysstate64_state_text64());
        line_app(p, "  modules="); line_dec(p, (uint64_t)sysstate64_module_count64());
        line_app(p, "  failed="); line_dec(p, (uint64_t)sysstate64_module_failed_count64());
    }
    p = line_new();
    if (p) {
        line_app(p, "Health: "); line_app(p, sysstate64_health_text64());
        line_app(p, "  gen="); line_dec(p, (uint64_t)sysstate64_generation64());
    }
    p = line_new();
    if (p) {
        int hh = 0, mm = 0, ss = 0, yy = 0, mo = 0, dd = 0, wd = 0;
        rtc_get_time64(&hh, &mm, &ss);
        rtc_get_date64(&yy, &mo, &dd, &wd);
        (void)wd;
        line_app(p, "Time: ");
        line_dec(p, (uint64_t)yy); line_app(p, "-");
        line_dec(p, (uint64_t)mo); line_app(p, "-");
        line_dec(p, (uint64_t)dd); line_app(p, " ");
        line_dec(p, (uint64_t)hh); line_app(p, ":");
        line_dec(p, (uint64_t)mm); line_app(p, ":");
        line_dec(p, (uint64_t)ss);
    }
    p = line_new();
    if (p) {
        line_app(p, "Uptime: "); line_dec(p, (uint64_t)(ticks64() / PIT_HZ_64));
        line_app(p, "s  ticks="); line_dec(p, (uint64_t)ticks64());
    }
    p = line_new();
    if (p) {
        line_app(p, "Heap: used="); line_dec(p, heap_used_64() / 1024);
        line_app(p, "KB total="); line_dec(p, heap_total_64() / 1024);
        line_app(p, "KB");
    }
    p = line_new();
    if (p) {
        line_app(p, "Task: "); line_app(p, task_current_name64());
        line_app(p, " id="); line_dec(p, (uint64_t)task_current_id64());
        line_app(p, " tasks="); line_dec(p, (uint64_t)task_count64());
    }
    p = line_new();
    if (p) {
        line_app(p, "Watchdog: armed="); line_dec(p, g_armed ? 1 : 0);
        line_app(p, " stale="); line_dec(p, (uint64_t)panic64_watchdog_stale_ms64());
        line_app(p, "ms fires="); line_dec(p, (uint64_t)g_fires);
        line_app(p, " checks="); line_dec(p, g_wd_checks);
    }
    p = line_new();
    if (p) { line_app(p, "Syslog: lines="); line_dec(p, (uint64_t)sys64_log_count64());
             line_app(p, "  (terminal 'syslog' lists them)"); }

    // ---- 画屏（关中断，避免任何绘制把它盖掉）----
    const uint64_t fl = dbg64_irq_save64();
    int sw = 0, sh = 0;
    bsod_draw(&sw, &sh);
    dbg64_line_begin64();
    log_str("[PANIC64] screen w=");
    dbg64_dec((uint64_t)sw);
    log_str(" h=");
    dbg64_dec((uint64_t)sh);
    log_str(" bg=0x000078D4 fg=0x00FFFFFF lines=");
    dbg64_dec((uint64_t)g_line_n);
    log_str(" title_y=56 body_y=102 line_h=26");
    dbg64_nl();
    log_str("[PANIC64] hold=");
    dbg64_dec(hold_ms);
    log_str("ms then halt (no auto reboot)");
    dbg64_nl();
    dbg64_line_end64();
    dbg64_irq_restore64(fl);

    // ---- 停留 hold_ms（PIT 继续跑，画面保持）----
    const uint32_t t0 = ticks64();
    while ((uint32_t)(ticks64() - t0) < ms_to_ticks64(hold_ms)) {
        __asm__ volatile("sti; hlt");
    }

    dbg64_line_begin64();
    log_str("[PANIC64] halt (power cycle required)");
    dbg64_nl();
    dbg64_line_end64();
    for (;;) __asm__ volatile("cli; hlt");
}


void panic64_bsod64(const char* code, uint64_t detail) {
    bsod_common(code, detail, 6000);         // 真实异常/看门狗：同样停住，便于事后取屏
    for (;;) __asm__ volatile("cli; hlt");   // 不可达（bsod_common 不返回）
}

void panic64_controlled64(const char* code, uint32_t hold_ms) {
    bsod_common(code, 0, hold_ms);
    for (;;) __asm__ volatile("cli; hlt");   // 不可达
}

extern "C" void panic64_cpu_exception64(uint64_t int_no, void* frame) {
    const pt_regs64* r = (const pt_regs64*)frame;
    g_exc_no = int_no;
    g_exc_valid = true;
    if (r) {
        g_exc_err = r->err_code;
        g_exc_rip = r->rip;
        g_exc_cs  = r->cs & 0xFFFF;
        g_exc_rsp = r->rsp;
    }
    uint64_t cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    g_exc_cr2 = cr2;
    bsod_common("CPU_EXCEPTION", int_no, 6000);
    for (;;) __asm__ volatile("cli; hlt");   // 不可达
}
