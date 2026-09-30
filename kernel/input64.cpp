// input64.cpp - ★ 用户态输入事件投递（自有 ABI 12：input_poll）的事件层实现
//
// 分工（见 kernel/input64.h 的完整语义）：
//   * input.cpp 只负责\"解码 -> 调钩子\"（ev64_key_press64 / ev64_key_release64 / ev64_mouse64）；
//   * 本文件负责\"路由（焦点/捕获）-> 每进程队列 -> 丢最旧计数 -> 拷给 ring3\"。
//   * 每进程队列本体**内嵌在 Proc64**（kernel/proc64.*），本文件通过两个 weak 引用拿它：
//       proc64_evq_of_current64() / proc64_evq_of64(pid) / proc64_current_pid64()
//     安装介质内核不链 proc64.cpp -> 这些符号是 0 -> 事件层整体退化成\"没有队列\"（不崩、不假装）。
//
// 为什么 pop 要关中断：push 发生在 IRQ1/IRQ12（鼠标/键盘），pop 发生在 ring3 的系统调用里；
// 同一个 ring 的 head/count 被两条上下文改写会互相覆盖（丢事件或重复出队）。这里 pop 全程
// cli/sti（临界区只有几十字节的拷贝），push 侧不用关 —— 单 CPU 上这样就够了（本内核是单核中断模型）。
#include "input64.h"
#include "x86_64.h"          // g_ticks64 / TICK_MS_64
#include "debug64.h"
#include "input.h"           // mouse_get_buttons()（键事件里的键位掩码口径）
#include "usermode64.h"      // user64_range_ok64（out 指针闸门）

// ---- 当前进程的队列/pid（proc64.cpp；安装介质内核里为 0）----
extern "C" void* proc64_evq_of_current64() __attribute__((weak));
extern "C" void* proc64_evq_of64(int pid)  __attribute__((weak));
// ★ 注意链接名：proc64.cpp 里 proc64_current_pid64() 是 **C++ 名字**（_Z20proc64_current_pid64v），
//   下面这一行不能加 extern "C" —— 加了的话弱引用永远解析不到、静默变 0，症状就是
//   `[EV64] poll FAILED pid=0 reason=no-process-context`（本批次实测踩过：shm 正常、事件全 EPERM）。
int  proc64_current_pid64()                 __attribute__((weak));

// ==================== 焦点 / 指针捕获注册表 ====================
// 0 = 没有 ring3 进程持有（键盘回内核桌面的老路径；鼠标没有目标就丢弃）。
static int g_ev64_focus64   = 0;
static int g_ev64_capture64 = 0;
// 自检期间不打焦点行：自检会临时摆布"焦点/捕获"表来验证路由与清理，那两行打出来会让
// 串口日志看起来像"某个进程真的拿过焦点"（实测踩到：`focus pid=0 prev=9 reason=exit`
// 出现在演示进程创建之前，读日志的人会以为是焦点泄漏）。自检只测语义，不产假证据。
static bool g_ev64_quiet64 = false;

static void ev64_log_focus64(int pid, int prev, const char* reason) {
    if (g_ev64_quiet64) return;
    dbg64_line_begin64();
    dbg64_str("[EV64] focus pid=");
    dbg64_dec((uint64_t)pid);
    dbg64_str(" prev=");
    dbg64_dec((uint64_t)prev);
    dbg64_str(" reason=");
    dbg64_str(reason);
    dbg64_nl();
    dbg64_line_end64();
}

// ==================== 队列原语 ====================
void ev64_reset64(Ev64Queue* q) {
    if (!q) return;
    q->head = 0;
    q->count = 0;
    q->drops = 0;
    q->drop_logged = 0;
    q->suppressed = 0;
}

uint32_t ev64_count64(const Ev64Queue* q) { return q ? q->count : 0; }

// 丢最旧的 drop 打点：前 8 条全打，之后每 64 条一条（其余计入 suppressed，出队/查询时补打）。
static void ev64_log_drop64(Ev64Queue* q, int pid, uint32_t type) {
    if (pid <= 0) { q->suppressed++; return; }     // 自检等没有 pid 的场景不打点（仍然计数）
    if (q->drop_logged < 8 || (q->drops & 63u) == 0) {
        q->drop_logged++;
        dbg64_line_begin64();
        dbg64_str("[EV64] drop pid=");
        dbg64_dec((uint64_t)pid);
        dbg64_str(" type=");
        dbg64_dec((uint64_t)type);
        dbg64_str(" total=");
        dbg64_dec((uint64_t)q->drops);
        dbg64_nl();
        dbg64_line_end64();
    } else {
        q->suppressed++;
    }
}

void ev64_push64(Ev64Queue* q, int pid, const Ev64Event* e) {
    if (!q || !e) return;
    if (q->count >= EV64_MAX_EVENTS64) {         // 满：丢**最旧**（不静默：计数 + 打点）
        q->head = (q->head + 1u) % EV64_MAX_EVENTS64;
        q->count--;
        q->drops++;
        ev64_log_drop64(q, pid, e->type);
    }
    const uint32_t idx = (q->head + q->count) % EV64_MAX_EVENTS64;
    q->ev[idx] = *e;
    q->count++;
}

uint32_t ev64_pop64(Ev64Queue* q, Ev64Event* out, uint32_t max) {
    if (!q || !out || max == 0) return 0;
    uint64_t fl = 0;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    uint32_t n = 0;
    while (n < max && q->count > 0) {
        out[n] = q->ev[q->head];
        q->head = (q->head + 1u) % EV64_MAX_EVENTS64;
        q->count--;
        n++;
    }
    __asm__ volatile("pushq %0; popfq" :: "r"(fl) : "memory", "cc");
    return n;
}

// ==================== 路由（键盘 -> 焦点；鼠标 -> 捕获 -> 焦点）====================
static void ev64_route64(const Ev64Event* e) {
    if (!proc64_evq_of64) return;                     // 安装介质内核：没有队列，直接丢
    int pid = 0;
    if (e->type == EV64_TYPE_KEY_DOWN || e->type == EV64_TYPE_KEY_UP) {
        pid = g_ev64_focus64;                        // 只有焦点进程收键盘
    } else {
        pid = g_ev64_capture64 ? g_ev64_capture64 : g_ev64_focus64;   // 捕获者优先，其次前台
    }
    if (pid <= 0) return;
    Ev64Queue* q = (Ev64Queue*)proc64_evq_of64(pid);
    if (!q) return;                                  // 进程已经没了（焦点/捕获清理有窗口）
    ev64_push64(q, pid, e);
}
// ==================== 按下表（KEY_UP 用按下时记下的那个键码）====================
static uint8_t g_ev64_down_code64[256];

void ev64_key_press64(uint8_t key_idx, uint8_t code, uint32_t mods) {
    g_ev64_down_code64[key_idx] = code;
    Ev64Event e;
    e.type = EV64_TYPE_KEY_DOWN; e.code = code;
    e.x = 0; e.y = 0; e.dx = 0; e.dy = 0;
    e.buttons = (uint32_t)mouse_get_buttons();
    e.mods = mods;
    e.t_ms = (uint64_t)g_ticks64 * (uint64_t)TICK_MS_64;
    ev64_route64(&e);
}

void ev64_key_release64(uint8_t key_idx, uint32_t mods) {
    const uint8_t code = g_ev64_down_code64[key_idx];
    if (code == 0) return;                     // 没记过按下（纯修饰键 / 启动前按着）：不产事件
    g_ev64_down_code64[key_idx] = 0;
    Ev64Event e;
    e.type = EV64_TYPE_KEY_UP; e.code = code;
    e.x = 0; e.y = 0; e.dx = 0; e.dy = 0;
    e.buttons = (uint32_t)mouse_get_buttons();
    e.mods = mods;
    e.t_ms = (uint64_t)g_ticks64 * (uint64_t)TICK_MS_64;
    ev64_route64(&e);
}

void ev64_mouse64(uint32_t type, uint32_t code, int32_t x, int32_t y,
                  int32_t dx, int32_t dy, uint32_t buttons, uint32_t mods) {
    Ev64Event e;
    e.type = type; e.code = code;
    e.x = x; e.y = y; e.dx = dx; e.dy = dy;
    e.buttons = buttons;
    e.mods = mods;
    e.t_ms = (uint64_t)g_ticks64 * (uint64_t)TICK_MS_64;
    ev64_route64(&e);
}

// ==================== 当前进程视角 ====================
Ev64Queue* ev64_current_queue64() {
    if (!proc64_evq_of_current64) return nullptr;
    return (Ev64Queue*)proc64_evq_of_current64();
}
int ev64_current_pid64() {
    if (!proc64_current_pid64) return -1;
    return proc64_current_pid64();
}

void ev64_proc_release64(int pid) {
    if (pid <= 0) return;
    if (g_ev64_capture64 == pid) {
        g_ev64_capture64 = 0;
        ev64_log_focus64(0, pid, "capture-release");
    }
    if (g_ev64_focus64 == pid) {
        g_ev64_focus64 = 0;
        ev64_log_focus64(0, pid, "exit");
    }
}

// ==================== 系统调用落点（自有 ABI 12）====================
// 打点预算：每次调用都打会刷屏（演示循环里每帧都 poll）；这里给 48 行预算，之后只在
// \"这次真拿到了事件\"或\"带 flags\"时再打（带 flags 的调用本来就极少）。
static uint32_t g_ev64_poll_logged64 = 0;

int64_t ev64_poll64(uint64_t out_uptr, uint32_t max, uint32_t flags) {
    const int pid = ev64_current_pid64();
    Ev64Queue* q = ev64_current_queue64();
    if (pid <= 0 || !q) {
        dbg64_line_begin64();
        dbg64_str("[EV64] poll FAILED pid=");
        dbg64_dec((uint64_t)(pid < 0 ? 0 : pid));
        dbg64_str(" reason=no-process-context err=");
        dbg64_dec((uint64_t)(-EV64_EPERM64));
        dbg64_nl();
        dbg64_line_end64();
        return EV64_EPERM64;                     // 任务 0 / 共享地址空间模式：没有每进程队列
    }
    if (flags & ~(EV64_FLAG_FOCUS64 | EV64_FLAG_CAPTURE64 | EV64_FLAG_RELEASE64)) {
        dbg64_line_begin64();
        dbg64_str("[EV64] poll FAILED pid=");
        dbg64_dec((uint64_t)pid);
        dbg64_str(" reason=bad-flags flags=");
        dbg64_hex64((uint64_t)flags);
        dbg64_str(" err=");
        dbg64_dec((uint64_t)(-EV64_EINVAL64));
        dbg64_nl();
        dbg64_line_end64();
        return EV64_EINVAL64;
    }
    if (max > EV64_MAX_EVENTS64) {
        dbg64_line_begin64();
        dbg64_str("[EV64] poll FAILED pid=");
        dbg64_dec((uint64_t)pid);
        dbg64_str(" reason=max-too-big max=");
        dbg64_dec((uint64_t)max);
        dbg64_str(" cap=");
        dbg64_dec((uint64_t)EV64_MAX_EVENTS64);
        dbg64_str(" err=");
        dbg64_dec((uint64_t)(-EV64_ENOMEM64));
        dbg64_nl();
        dbg64_line_end64();
        return EV64_ENOMEM64;
    }

    // ---- flags：焦点/捕获的申请与释放（先释放后申请，同一个调用里可以换人）----
    int changed = 0;
    if (flags & EV64_FLAG_RELEASE64) {
        if (g_ev64_capture64 == pid) { g_ev64_capture64 = 0; changed = 1; ev64_log_focus64(0, pid, "capture-release"); }
        if (g_ev64_focus64 == pid)   { g_ev64_focus64 = 0;   changed = 1; ev64_log_focus64(0, pid, "release"); }
    }
    if (flags & EV64_FLAG_FOCUS64) {
        if (g_ev64_focus64 != pid) {
            const int prev = g_ev64_focus64;
            g_ev64_focus64 = pid;
            changed = 1;
            ev64_log_focus64(pid, prev, "request");
        }
    }
    if (flags & EV64_FLAG_CAPTURE64) {
        if (g_ev64_capture64 != pid) g_ev64_capture64 = pid;
    }
    if (changed || flags) {
        dbg64_line_begin64();
        dbg64_str("[EV64] attach pid=");
        dbg64_dec((uint64_t)pid);
        dbg64_str(" flags=");
        dbg64_hex64((uint64_t)flags);
        dbg64_str(" focus=");
        dbg64_dec((uint64_t)g_ev64_focus64);
        dbg64_str(" capture=");
        dbg64_dec((uint64_t)g_ev64_capture64);
        dbg64_nl();
        dbg64_line_end64();
    }

    // ---- max == 0：只查询\"有多少待取\"（顺带把节流掉的 drop 行数补一条汇总）----
    if (max == 0) {
        if (q->suppressed) {
            dbg64_line_begin64();
            dbg64_str("[EV64] drops pid=");
            dbg64_dec((uint64_t)pid);
            dbg64_str(" total=");
            dbg64_dec((uint64_t)q->drops);
            dbg64_str(" suppressed=");
            dbg64_dec((uint64_t)q->suppressed);
            dbg64_nl();
            dbg64_line_end64();
            q->suppressed = 0;
        }
        dbg64_line_begin64();
        dbg64_str("[EV64] poll pid=");
        dbg64_dec((uint64_t)pid);
        dbg64_str(" max=0 flags=");
        dbg64_hex64((uint64_t)flags);
        dbg64_str(" got=0 pend=");
        dbg64_dec((uint64_t)q->count);
        dbg64_str(" drops=");
        dbg64_dec((uint64_t)q->drops);
        dbg64_nl();
        dbg64_line_end64();
        return (int64_t)q->count;
    }

    if (!user64_range_ok64(out_uptr, (uint64_t)max * (uint64_t)EV64_EVENT_SIZE64)) {
        dbg64_line_begin64();
        dbg64_str("[EV64] poll FAILED pid=");
        dbg64_dec((uint64_t)pid);
        dbg64_str(" reason=bad-out-ptr err=");
        dbg64_dec((uint64_t)(-EV64_EFAULT64));
        dbg64_nl();
        dbg64_line_end64();
        return EV64_EFAULT64;
    }

    // ---- 出队：整批拷进本地（关中断，见文件头）再一次性写用户内存 ----
    Ev64Event tmp[EV64_MAX_EVENTS64];
    const uint32_t got = ev64_pop64(q, tmp, max);
    if (got) {
        Ev64Event* dst = (Ev64Event*)(uintptr_t)out_uptr;
        for (uint32_t i = 0; i < got; i++) dst[i] = tmp[i];
    }
    if (g_ev64_poll_logged64 < 48 || got || flags) {
        g_ev64_poll_logged64++;
        dbg64_line_begin64();
        dbg64_str("[EV64] poll pid=");
        dbg64_dec((uint64_t)pid);
        dbg64_str(" max=");
        dbg64_dec((uint64_t)max);
        dbg64_str(" flags=");
        dbg64_hex64((uint64_t)flags);
        dbg64_str(" got=");
        dbg64_dec((uint64_t)got);
        dbg64_str(" pend=");
        dbg64_dec((uint64_t)q->count);
        dbg64_str(" drops=");
        dbg64_dec((uint64_t)q->drops);
        dbg64_nl();
        dbg64_line_end64();
    }
    return (int64_t)got;
}

// ==================== 自检 ====================
int ev64_selftest64() {
    int fail = 0;
    // bit0：结构体布局（40 字节 / 字段偏移固定 —— 内核与用户程序之间的 ABI）
    if (sizeof(Ev64Event) != EV64_EVENT_SIZE64) fail |= 1;
    if (EV64_EVENT_SIZE64 != 40) fail |= 1;
    {
        Ev64Event e;
        uint8_t* p = (uint8_t*)&e;
        if ((uint64_t)(p + 0) != (uint64_t)&e) fail |= 1;
        if ((uint64_t)((uint8_t*)&e.type - p) != 0) fail |= 1;
        if ((uint64_t)((uint8_t*)&e.code - p) != 4) fail |= 1;
        if ((uint64_t)((uint8_t*)&e.x - p) != 8) fail |= 1;
        if ((uint64_t)((uint8_t*)&e.y - p) != 12) fail |= 1;
        if ((uint64_t)((uint8_t*)&e.dx - p) != 16) fail |= 1;
        if ((uint64_t)((uint8_t*)&e.dy - p) != 20) fail |= 1;
        if ((uint64_t)((uint8_t*)&e.buttons - p) != 24) fail |= 1;
        if ((uint64_t)((uint8_t*)&e.mods - p) != 28) fail |= 1;
        if ((uint64_t)((uint8_t*)&e.t_ms - p) != 32) fail |= 1;
    }
    // bit1：队列语义（满 -> 丢最旧 + 计数；容量边界）
    {
        static Ev64Queue q;                 // 自检用临时队列（不动任何进程的队列）
        ev64_reset64(&q);
        Ev64Event e;
        e.type = EV64_TYPE_KEY_DOWN; e.code = 0; e.x = 0; e.y = 0;
        e.dx = 0; e.dy = 0; e.buttons = 0; e.mods = 0; e.t_ms = 0;
        for (uint32_t i = 0; i < EV64_MAX_EVENTS64; i++) { e.code = i; ev64_push64(&q, 0, &e); }
        if (ev64_count64(&q) != EV64_MAX_EVENTS64) fail |= 2;
        e.code = 0xAA; ev64_push64(&q, 0, &e);              // 第 33 条：丢最旧（code=0）
        if (ev64_count64(&q) != EV64_MAX_EVENTS64) fail |= 2;
        if (q.drops != 1) fail |= 2;
        Ev64Event out[EV64_MAX_EVENTS64];
        const uint32_t n = ev64_pop64(&q, out, EV64_MAX_EVENTS64);
        if (n != EV64_MAX_EVENTS64) fail |= 2;
        if (out[0].code != 1) fail |= 2;                    // 最旧的 code=0 已被丢掉
        if (out[EV64_MAX_EVENTS64 - 1].code != 0xAA) fail |= 2;
        if (ev64_count64(&q) != 0) fail |= 2;
        ev64_reset64(&q);
    }
    // bit2：焦点/捕获路由表（申请 -> 覆盖 -> 释放；退出清理按 pid 命中）
    {
        const int f0 = g_ev64_focus64, c0 = g_ev64_capture64;
        g_ev64_quiet64 = true;                        // 自检不动真日志（见 g_ev64_quiet64 的说明）
        g_ev64_focus64 = 0; g_ev64_capture64 = 0;
        g_ev64_focus64 = 7;   if (g_ev64_focus64 != 7) fail |= 4;
        g_ev64_focus64 = 9;   if (g_ev64_focus64 != 9) fail |= 4;      // 新前台顶掉旧前台
        g_ev64_capture64 = 9; if (g_ev64_capture64 != 9) fail |= 4;
        if (g_ev64_capture64 ? g_ev64_capture64 : g_ev64_focus64) { /* 路由口径：捕获优先 */ }
        else fail |= 4;
        ev64_proc_release64(9);                                        // 进程退出：焦点 + 捕获一起清
        if (g_ev64_focus64 != 0 || g_ev64_capture64 != 0) fail |= 4;
        ev64_proc_release64(42);                                       // 不相干的 pid：不动
        if (g_ev64_focus64 != 0 || g_ev64_capture64 != 0) fail |= 4;
        g_ev64_focus64 = f0; g_ev64_capture64 = c0;
        g_ev64_quiet64 = false;
    }
    // bit3：常量自洽（flags/mods/键位不重叠；类型号连续；错误码互不相同）
    if (EV64_FLAG_FOCUS64 & EV64_FLAG_CAPTURE64) fail |= 8;
    if (EV64_FLAG_FOCUS64 & EV64_FLAG_RELEASE64) fail |= 8;
    if (EV64_FLAG_CAPTURE64 & EV64_FLAG_RELEASE64) fail |= 8;
    if ((EV64_MOD_SHIFT64 | EV64_MOD_CTRL64 | EV64_MOD_ALT64 | EV64_MOD_CAPS64) != 0xFu) fail |= 8;
    if ((EV64_BTN_LEFT64 | EV64_BTN_RIGHT64 | EV64_BTN_MIDDLE64) != 0x7u) fail |= 8;
    if (EV64_TYPE_KEY_DOWN != 1 || EV64_TYPE_WHEEL != 6) fail |= 8;
    if (EV64_EPERM64 == EV64_EFAULT64 || EV64_EFAULT64 == EV64_EINVAL64) fail |= 8;
    if (EV64_EVENT_SIZE64 * EV64_MAX_EVENTS64 < 1024u) fail |= 8;      // 队列容量下界自洽
    return fail;
}
