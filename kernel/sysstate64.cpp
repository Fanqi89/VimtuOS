// sysstate64.cpp - 64 位系统状态机 / 模块注册表 / 健康报告 / ring log 实现
//
// 状态流转（与 32 位语义对齐，按 64 位启动链裁剪）：
//   BOOT --(模块注册完成)--> STARTING --(逐模块复核通过)--> RUNNING
//        --(关机/重启/软重启)--> STOPPING --(逆序停模块 + 落盘)--> STOPPED --(硬复位链)
//   32 位还有 READY（空闲休眠）与 sysctl 任务驱动的逐 tick 推进；64 位的桌面就是任务 0 的
//   消息循环，没有独立的 sysctl 任务，所以推进点改成：桌面首帧 tick -> RUNNING，
//   停止/关机由 gui64 的 sys_reboot64/sys_shutdown64 与终端命令显式调用。
//
// ★ 各模块的 health 钩子读的是**缓存 + 廉价在线量**（避免在蓝屏/报告路径里做磁盘 I/O）：
//   init 钩子负责"探一次真实状态"并存进静态变量（例如 ata64 的 IDENTIFY、vfs64 的 superblock 探测），
//   health 钩子只读缓存与内存计数器。这样健康报告可以在任意时刻（包括 BSOD 现场）安全调用。
#include "sysstate64.h"
#include "debug64.h"      // dbg64_* + 行锁
#include "x86_64.h"       // ticks64 / g_irq_mode64 / TICK_MS_64
#include "mem_64.h"       // 堆 / 物理内存统计
#include "ata64.h"        // DiskInfo / ata64_identify
#include "vfs64.h"        // vfs64_stat（挂载状态）
#include "store64.h"      // store64_carrier64 / slot / generation / key count
#include "task64.h"       // 任务表统计
#include "hwinfo64.h"     // HW64_INFO_MAGIC / hw_info64
#include "acpi64.h"       // acpi_rsdp_addr64 / acpi_cpu_count64
#include "edid64.h"       // edid64_get
#include "net64.h"        // net64_tx_frames64
#include "usb64.h"        // usb64_ports64 / usb64_devices64
#include "smp64.h"        // smp64_online_cpu_count64
#include "gui64.h"        // gui64_screen_w / close_all_windows / window_count
#include "config64.h"     // 配置（落盘 flush）
#include "session64.h"    // 会话（逆序停止时保存/清状态）
#include "panic64.h"      // 看门狗挂起/恢复
#include "app64.h"        // app64_main_part_lba64（找 VimtuFS2 主分区起始 LBA）
#include <stdarg.h>

// ==================== 常量 ====================
#define SYS64_MOD_MAX   24
#define SYS64_STOP_TIMEOUT_TICKS 1000u   // 单个模块停止超时 = 1000 tick = 4 秒

// ==================== 状态 ====================
static Sys64Module g_mods[SYS64_MOD_MAX];
static int   g_mod_state[SYS64_MOD_MAX];
static int   g_mod_count = 0;
static int   g_state = SYS64_STATE_STOPPED;
static bool  g_busy = false;
static uint32_t g_gen = 0;
static int   g_failed = 0;
static int   g_health = SYS64_HEALTH_DOWN;
static const char* g_last_reason = "-";
static uint32_t g_stage_start = 0;

// ==================== ring log ====================
static char g_log[SYS64_LOG_RING][SYS64_LOG_LEN];
static int  g_log_head = 0;
static int  g_log_total = 0;

int sys64_log_count64() {
    return g_log_total < SYS64_LOG_RING ? g_log_total : SYS64_LOG_RING;
}

const char* sys64_log_line64(int i) {
    const int n = sys64_log_count64();
    if (i < 0 || i >= n) return "";
    const int idx = (g_log_head - 1 - i + SYS64_LOG_RING * 2) % SYS64_LOG_RING;
    return g_log[idx];
}

// 写 ring（整行原子）+ 同时打串口（一行只调用一次本函数，避免重复打点）。
void sys64_log64(const char* line) {
    dbg64_line_begin64();
    int i = 0;
    while (line && line[i] && i < SYS64_LOG_LEN - 1) { g_log[g_log_head][i] = line[i]; i++; }
    g_log[g_log_head][i] = 0;
    g_log_head = (g_log_head + 1) % SYS64_LOG_RING;
    g_log_total++;
    if (line) dbg64_str(line);
    dbg64_nl();
    dbg64_line_end64();
}

// 极简格式化：%s %d %u %x %c %%（内核里没有 vsnprintf；只支持这几种，够打点用）
void sys64_logf64(const char* fmt, ...) {
    char buf[SYS64_LOG_LEN];
    int o = 0;
    va_list ap;
    va_start(ap, fmt);
    for (int i = 0; fmt && fmt[i] && o < SYS64_LOG_LEN - 1; i++) {
        if (fmt[i] != '%') { buf[o++] = fmt[i]; continue; }
        i++;
        const char c = fmt[i];
        if (c == 's') {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            while (*s && o < SYS64_LOG_LEN - 1) buf[o++] = *s++;
        } else if (c == 'd' || c == 'u') {
            int32_t v = va_arg(ap, int32_t);
            char tmp[12]; int n = 0;
            uint32_t u = (uint32_t)v;
            if (v < 0 && c == 'd') { if (o < SYS64_LOG_LEN - 1) buf[o++] = '-'; u = (uint32_t)(-v); }
            if (u == 0) tmp[n++] = '0';
            while (u > 0) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
            while (n > 0 && o < SYS64_LOG_LEN - 1) buf[o++] = tmp[--n];
        } else if (c == 'x') {
            uint32_t v = va_arg(ap, uint32_t);
            static const char* H = "0123456789ABCDEF";
            char tmp[12]; int n = 0;
            if (v == 0) tmp[n++] = '0';
            while (v > 0) { tmp[n++] = H[v & 0xF]; v >>= 4; }
            if (o < SYS64_LOG_LEN - 2) { buf[o++] = '0'; buf[o++] = 'x'; }
            while (n > 0 && o < SYS64_LOG_LEN - 1) buf[o++] = tmp[--n];
        } else if (c == 'c') {
            buf[o++] = (char)va_arg(ap, int);
        } else if (c == '%') {
            buf[o++] = '%';
        } else if (c) {
            buf[o++] = '%';
            if (o < SYS64_LOG_LEN - 1) buf[o++] = c;
        }
    }
    va_end(ap);
    buf[o] = 0;
    sys64_log64(buf);
}

// ==================== 名称 ====================
const char* sysstate64_state_text64() {
    switch (g_state) {
        case SYS64_STATE_STOPPED:  return "STOPPED";
        case SYS64_STATE_BOOT:     return "BOOT";
        case SYS64_STATE_STARTING: return "STARTING";
        case SYS64_STATE_RUNNING:  return "RUNNING";
        case SYS64_STATE_STOPPING: return "STOPPING";
        default:                   return "?";
    }
}

const char* sysstate64_module_state_text64(int st) {
    switch (st) {
        case SYS64_MOD_STOPPED:  return "STOPPED";
        case SYS64_MOD_STARTING: return "STARTING";
        case SYS64_MOD_READY:    return "READY";
        case SYS64_MOD_DEGRADED: return "DEGRADED";
        case SYS64_MOD_FAILED:   return "FAILED";
        default:                 return "?";
    }
}

const char* sysstate64_health_text64() {
    switch (g_health) {
        case SYS64_HEALTH_UP:       return "UP";
        case SYS64_HEALTH_DEGRADED: return "DEGRADED";
        default:                    return "DOWN";
    }
}

// ==================== 各模块的 init / stop / health 钩子 ====================
// 缓存（init 探一次真实状态；health 只读缓存 + 内存量）
static int g_ata_ok = 0, g_vfs_ok = 0, g_store_ok = 0, g_hw_ok = 0;
static int g_acpi_ok = 0, g_edid_ok = 0, g_syscalls_ok = 0, g_cfg_ok = 0;

static int m_mem_init() {
    return (mem_total_ram_64() > 0 && heap_total_64() > 0) ? 0 : -1;
}
static int m_mem_health() {
    if (heap_total_64() == 0) return SYS64_MODH_DOWN;
    return (heap_used_64() <= heap_total_64()) ? SYS64_MODH_UP : SYS64_MODH_DEGRADED;
}
static int m_x86_init()   { return ticks64() > 0 ? 0 : -1; }        // PIT 在跑才是"平台活着"
static int m_x86_health() {
    return (ticks64() > 0 && g_irq_total64 > 0) ? SYS64_MODH_UP : SYS64_MODH_DOWN;
}
static int m_ata_init() {
    DiskInfo di;
    g_ata_ok = (ata64_identify(0, &di) && di.present) ? 1 : 0;
    return g_ata_ok ? 0 : -1;
}
static int m_ata_health()      { return g_ata_ok ? SYS64_MODH_UP : SYS64_MODH_DOWN; }
static int m_vfs_init()        { g_vfs_ok = (vfs64_stat("/", nullptr, nullptr) == 0) ? 1 : 0; return g_vfs_ok ? 0 : -1; }
static int m_vfs_health()      { return g_vfs_ok ? SYS64_MODH_UP : SYS64_MODH_DOWN; }
static int m_store_init() {
    const char* c = store64_carrier64();
    g_store_ok = (c && c[0] == 'v') ? 1 : ((c && c[0] == 'r') ? 1 : 0);   // vfs / raw 都算有载体
    return g_store_ok ? 0 : -1;
}
static int m_store_health()    { return g_store_ok ? SYS64_MODH_UP : SYS64_MODH_DOWN; }
static int m_task_init()       { return (task_count64() >= 1) ? 0 : -1; }
static int m_task_health()     { return (task_count64() >= 1) ? SYS64_MODH_UP : SYS64_MODH_DOWN; }
static int m_apic_init()       { return (g_irq_mode64 == 1) ? 0 : -1; }   // 非关键：PIC 回退也算降级
static int m_apic_health()     { return (g_irq_mode64 == 1) ? SYS64_MODH_UP : SYS64_MODH_DEGRADED; }
static int m_smp_init()        { return 0; }
static int m_smp_health()      { return (smp64_online_cpu_count64() >= 1) ? SYS64_MODH_UP : SYS64_MODH_DEGRADED; }
static int m_hw_init()         { g_hw_ok = (hw_info64()->magic == HW64_INFO_MAGIC) ? 1 : 0; return g_hw_ok ? 0 : -1; }
static int m_hw_health()       { return g_hw_ok ? SYS64_MODH_UP : SYS64_MODH_DOWN; }
static int m_acpi_init()       { g_acpi_ok = (acpi_rsdp_addr64() != 0) ? 1 : 0; return g_acpi_ok ? 0 : -1; }
static int m_acpi_health()     { return g_acpi_ok ? SYS64_MODH_UP : SYS64_MODH_DEGRADED; }
static int m_edid_init()       { g_edid_ok = edid64_get()->valid ? 1 : 0; return g_edid_ok ? 0 : -1; }
static int m_edid_health()     { return g_edid_ok ? SYS64_MODH_UP : SYS64_MODH_DEGRADED; }
static int m_net_init()        { return 0; }   // 驱动/链路状态没有"就绪"保证：非关键
static int m_net_health()      { return (net64_tx_frames64() > 0) ? SYS64_MODH_UP : SYS64_MODH_UNKNOWN; }
static int m_usb_init()        { return 0; }
static int m_usb_health()      { return (usb64_ports64() > 0) ? SYS64_MODH_UP : SYS64_MODH_UNKNOWN; }
static int m_syscall_init()    { g_syscalls_ok = 1; return 0; }   // 启动期已自检（syscall64_selftest64）
static int m_syscall_health()  { (void)g_syscalls_ok; return SYS64_MODH_UNKNOWN; }  // 无门/ABI 状态查询 API
static int m_user_init()       { return 0; }
static int m_user_health()     { return SYS64_MODH_UNKNOWN; }     // 无"ring3 是否可用"的查询 API
static int m_cfg_init()        { g_cfg_ok = (config64_count64() > 0 && config64_default_count64() > 0) ? 1 : 0;
                                 return g_cfg_ok ? 0 : -1; }
static int m_cfg_health()      { return g_cfg_ok ? SYS64_MODH_UP : SYS64_MODH_DOWN; }
static int m_sess_init()       { return (session64_app_count64() > 0) ? 0 : -1; }
static int m_sess_health()     { return (session64_app_count64() > 0) ? SYS64_MODH_UP : SYS64_MODH_DOWN; }
static int m_sysstate_init()   { return 0; }
static int m_sysstate_health() { return SYS64_MODH_UP; }
static int m_gui_init()        { return 0; }                       // 桌面在 gui64_run 里才起来，见 health
static int m_gui_health()      { return (gui64_screen_w() > 0) ? SYS64_MODH_UP : SYS64_MODH_UNKNOWN; }

// 停止钩子：真做"优雅停止"里能做的部分（关窗；会话/配置落盘在 stop_all64 里统一做）
static int s_gui(uint32_t timeout) {
    (void)timeout;
    const int n = gui64_window_count();
    gui64_close_all_windows();
    sys64_logf64("[SYS64] gui64 stop: closed %d window(s)", n);
    return 0;
}
static int s_sess(uint32_t timeout) {
    (void)timeout;
    session64_reset_all64("stop");
    return 0;
}
static int s_ok(uint32_t timeout) { (void)timeout; return 0; }

// ==================== 模块注册 ====================
int sysstate64_register64(const Sys64Module* m) {
    if (!m || !m->name || g_mod_count >= SYS64_MOD_MAX) return -1;
    g_mods[g_mod_count] = *m;
    g_mod_state[g_mod_count] = SYS64_MOD_STOPPED;
    const int idx = g_mod_count;
    g_mod_count++;
    sys64_logf64("[SYS64] module %s registered", m->name);
    return idx;
}

int sysstate64_module_count64() { return g_mod_count; }

const char* sysstate64_module_name64(int i) {
    return (i >= 0 && i < g_mod_count) ? g_mods[i].name : "-";
}

int sysstate64_module_state64(int i) {
    return (i >= 0 && i < g_mod_count) ? g_mod_state[i] : SYS64_MOD_STOPPED;
}

int sysstate64_module_health64(int i) {
    if (i < 0 || i >= g_mod_count) return SYS64_MODH_UNKNOWN;
    if (g_mod_state[i] != SYS64_MOD_READY && g_mod_state[i] != SYS64_MOD_STARTING)
        return SYS64_MODH_DOWN;
    if (!g_mods[i].health) return SYS64_MODH_UNKNOWN;
    return g_mods[i].health();
}

int sysstate64_module_failed_count64() { return g_failed; }

// 平台/子系统模块（kernel64.cpp 的启动序列里调用；顺序 = 停止的逆序的第一义）
void sys64_register_builtin64() {
    static const Sys64Module kBuiltin[] = {
        { "mem64",         m_mem_init,      s_ok,   m_mem_health,      SYS64_MODF_CRITICAL,                   {0,0,0}, SYS64_STOP_TIMEOUT_TICKS },
        { "x86_64",        m_x86_init,      s_ok,   m_x86_health,      0,                                    {0,0,0}, SYS64_STOP_TIMEOUT_TICKS },
        { "ata64",         m_ata_init,      s_ok,   m_ata_health,      SYS64_MODF_CRITICAL,                  {0,0,0}, SYS64_STOP_TIMEOUT_TICKS },
        { "vfs64",         m_vfs_init,      s_ok,   m_vfs_health,      SYS64_MODF_CRITICAL,                  {0,0,0}, SYS64_STOP_TIMEOUT_TICKS },
        { "store64",       m_store_init,    s_ok,   m_store_health,    SYS64_MODF_CRITICAL,                  {0,0,0}, SYS64_STOP_TIMEOUT_TICKS },
        { "task64",        m_task_init,     s_ok,   m_task_health,     SYS64_MODF_CRITICAL,                  {0,0,0}, SYS64_STOP_TIMEOUT_TICKS },
        { "apic64",        m_apic_init,     s_ok,   m_apic_health,     0,                                    {0,0,0}, SYS64_STOP_TIMEOUT_TICKS },
        { "smp64",         m_smp_init,      s_ok,   m_smp_health,      0,                                    {0,0,0}, SYS64_STOP_TIMEOUT_TICKS },
        { "hwinfo64",      m_hw_init,       s_ok,   m_hw_health,       0,                                    {0,0,0}, SYS64_STOP_TIMEOUT_TICKS },
        { "acpi64",        m_acpi_init,     s_ok,   m_acpi_health,     0,                                    {0,0,0}, SYS64_STOP_TIMEOUT_TICKS },
        { "edid64",        m_edid_init,     s_ok,   m_edid_health,     0,                                    {0,0,0}, SYS64_STOP_TIMEOUT_TICKS },
        { "net64",         m_net_init,      s_ok,   m_net_health,      0,                                    {0,0,0}, SYS64_STOP_TIMEOUT_TICKS },
        { "usb64",         m_usb_init,      s_ok,   m_usb_health,      0,                                    {0,0,0}, SYS64_STOP_TIMEOUT_TICKS },
        { "syscall64",     m_syscall_init,  s_ok,   m_syscall_health,  SYS64_MODF_CRITICAL,                  {0,0,0}, SYS64_STOP_TIMEOUT_TICKS },
        { "usermode64",    m_user_init,     s_ok,   m_user_health,     0,                                    {0,0,0}, SYS64_STOP_TIMEOUT_TICKS },
        { "config64",      m_cfg_init,      s_ok,   m_cfg_health,      SYS64_MODF_CRITICAL,                  {0,0,0}, SYS64_STOP_TIMEOUT_TICKS },
        { "session64",     m_sess_init,     s_sess, m_sess_health,     0,                                    {0,0,0}, SYS64_STOP_TIMEOUT_TICKS },
        { "sysstate64",    m_sysstate_init, s_ok,   m_sysstate_health, SYS64_MODF_PLATFORM,                  {0,0,0}, SYS64_STOP_TIMEOUT_TICKS },
        { "gui64",         m_gui_init,      s_gui,  m_gui_health,      SYS64_MODF_CRITICAL,                  {0,0,0}, SYS64_STOP_TIMEOUT_TICKS },
    };
    for (int i = 0; i < (int)(sizeof(kBuiltin) / sizeof(kBuiltin[0])); i++)
        sysstate64_register64(&kBuiltin[i]);
}

// ==================== 状态机 ====================
int sysstate64_state64() { return g_state; }
uint32_t sysstate64_generation64() { return g_gen; }

void sysstate64_begin64() {
    g_state = SYS64_STATE_BOOT;
    g_stage_start = ticks64();
    g_gen = 0;
    g_failed = 0;
    g_busy = false;
    g_health = SYS64_HEALTH_DOWN;
    g_mod_count = 0;
    sys64_log64("[SYS64] state=BOOT");
}

// STARTING：依次复核模块（init 钩子 = 只读复核，不重做初始化）
int sysstate64_start64() {
    if (g_state == SYS64_STATE_RUNNING) return 0;
    g_state = SYS64_STATE_STARTING;
    g_stage_start = ticks64();
    sys64_log64("[SYS64] state=STARTING");
    for (int i = 0; i < g_mod_count; i++) {
        g_mod_state[i] = SYS64_MOD_STARTING;
        const int r = g_mods[i].init ? g_mods[i].init() : 0;
        if (r == 0) {
            g_mod_state[i] = SYS64_MOD_READY;
            sys64_logf64("[SYS64] module %s init ok (try=0)", g_mods[i].name);
        } else if (g_mods[i].flags & SYS64_MODF_CRITICAL) {
            g_mod_state[i] = SYS64_MOD_FAILED;
            g_failed++;
            sys64_logf64("[SYS64] module %s FAILED (critical, rc=%d)", g_mods[i].name, (int)r);
        } else {
            g_mod_state[i] = SYS64_MOD_DEGRADED;
            sys64_logf64("[SYS64] module %s DEGRADED (non-critical, rc=%d)", g_mods[i].name, (int)r);
        }
    }
    sys64_logf64("[SYS64] stage STARTING done elapsed=%dms", (int)((ticks64() - g_stage_start) * TICK_MS_64));
    return g_failed;
}

// GUI 主循环每帧调用：第一次推进到 RUNNING（= 桌面真的起来了），并打一次健康报告
int sysstate64_tick64() {
    if (g_busy) return 0;
    g_busy = true;
    int entered = 0;
    if (g_state == SYS64_STATE_STARTING) {
        g_state = SYS64_STATE_RUNNING;
        g_gen++;
        sys64_logf64("[SYS64] state=RUNNING gen=%d modules=%d", (int)g_gen, g_mod_count);
        (void)sysstate64_health_report64(1, nullptr, 0);
        entered = 1;
    }
    g_busy = false;
    return entered;
}

// 逆序优雅停止：停模块 +（会话快照 / 配置）落盘
int sysstate64_stop_all64(const char* reason) {
    if (g_state == SYS64_STATE_STOPPING || g_state == SYS64_STATE_STOPPED) return 0;
    g_state = SYS64_STATE_STOPPING;
    g_stage_start = ticks64();
    g_last_reason = reason ? reason : "user";
    panic64_watchdog_suspend64();
    sys64_logf64("[SYS64] state=STOPPING reason=%s modules=%d", g_last_reason, g_mod_count);

    int stopped = 0, failed = 0;
    for (int i = g_mod_count - 1; i >= 0; i--) {
        if (g_mod_state[i] == SYS64_MOD_STOPPED) { stopped++; continue; }
        g_mod_state[i] = SYS64_MOD_STARTING;
        const uint32_t deadline = ticks64() + g_mods[i].timeout_ticks;
        int r = 0;
        for (;;) {
            r = g_mods[i].stop ? g_mods[i].stop(g_mods[i].timeout_ticks) : 0;
            if (r != -3) break;
            if ((int32_t)(ticks64() - deadline) >= 0) break;
            task_sleep64(TICK_MS_64 * 4);
        }
        if (r == -3) {
            sys64_logf64("[SYS64] module %s stop TIMEOUT -> force clean", g_mods[i].name);
            g_mod_state[i] = SYS64_MOD_STOPPED;
            stopped++;
            continue;
        }
        if (r < 0) {
            sys64_logf64("[SYS64] module %s stop FAILED (rc=%d)", g_mods[i].name, (int)r);
            g_mod_state[i] = SYS64_MOD_FAILED;
            failed++;
            continue;
        }
        g_mod_state[i] = SYS64_MOD_STOPPED;
        stopped++;
        sys64_logf64("[SYS64] module %s stop ok", g_mods[i].name);
    }

    // 状态落盘：会话快照 -> config64 -> store64 双槽 flush（真写盘，走 VFS 载体）
    const int open_n = session64_save64();
    const int frc = config64_flush64();
    sys64_logf64("[SYS64] persist session open=%d flush rc=%d", open_n, frc);
    sys64_logf64("[SYS64] state=STOPPED stopped=%d failed=%d elapsed=%dms",
                 stopped, failed, (int)((ticks64() - g_stage_start) * TICK_MS_64));
    g_state = SYS64_STATE_STOPPED;
    return stopped;
}

// 软重启：优雅停止（停模块 + 落盘 flush）-> 再走原有硬复位链（gui64 的 sys_reboot64，不返回）
void sysstate64_soft_restart64() {
    sys64_log64("[SYS64] soft restart requested: graceful stop -> hard reset chain");
    sysstate64_stop_all64("soft-restart");
    sys64_log64("[SYS64] soft restart: entering hard reset chain (sys_reboot64)");
    extern void sys_reboot64();
    sys_reboot64();
    for (;;) __asm__ volatile("cli; hlt");
}

// ==================== 健康报告 ====================
int sysstate64_health64() {
    bool degraded = false;
    int failed = 0;
    for (int i = 0; i < g_mod_count; i++) {
        const int st = g_mod_state[i];
        if (st == SYS64_MOD_FAILED) {
            failed++;
            if (g_mods[i].flags & SYS64_MODF_CRITICAL) { g_health = SYS64_HEALTH_DOWN; return g_health; }
            degraded = true;
            continue;
        }
        const int h = sysstate64_module_health64(i);
        if (h == SYS64_MODH_DOWN) {
            if (g_mods[i].flags & SYS64_MODF_CRITICAL) { g_health = SYS64_HEALTH_DOWN; return g_health; }
            degraded = true;
        } else if (h == SYS64_MODH_DEGRADED) {
            degraded = true;
        }
    }
    (void)failed;
    g_health = degraded ? SYS64_HEALTH_DEGRADED : SYS64_HEALTH_UP;
    return g_health;
}

int sysstate64_health_report64(int log_to_serial, char* out, int maxlen) {
    const int h = sysstate64_health64();
    int failed = 0, degraded = 0, unknown = 0, up = 0;
    for (int i = 0; i < g_mod_count; i++) {
        const int st = g_mod_state[i];
        const int mh = sysstate64_module_health64(i);
        if (st == SYS64_MOD_FAILED || mh == SYS64_MODH_DOWN) failed++;
        else if (st == SYS64_MOD_DEGRADED || mh == SYS64_MODH_DEGRADED) degraded++;
        else if (mh == SYS64_MODH_UNKNOWN) unknown++;
        else up++;
    }
    if (log_to_serial) {
        if (h == SYS64_HEALTH_UP)
            sys64_logf64("[SYS64] health ok modules=%d failed=0 degraded=%d unknown=%d up=%d",
                         g_mod_count, degraded, unknown, up);
        else if (h == SYS64_HEALTH_DEGRADED)
            sys64_logf64("[SYS64] health degraded modules=%d failed=%d degraded=%d unknown=%d",
                         g_mod_count, failed, degraded, unknown);
        else
            sys64_logf64("[SYS64] health down modules=%d failed=%d degraded=%d unknown=%d",
                         g_mod_count, failed, degraded, unknown);
        for (int i = 0; i < g_mod_count; i++) {
            const int mh = sysstate64_module_health64(i);
            sys64_logf64("[SYS64] health module %s state=%s health=%s",
                         g_mods[i].name, sysstate64_module_state_text64(g_mod_state[i]),
                         mh == SYS64_MODH_UP ? "UP" : (mh == SYS64_MODH_DEGRADED ? "DEGRADED" :
                         (mh == SYS64_MODH_UNKNOWN ? "unknown" : "DOWN")));
        }
    }
    if (out && maxlen > 0) {
        int pos = 0;
        // 只支持定长拼装（无 libc）
        const char* pre = "health: ";
        for (int i = 0; pre[i] && pos < maxlen - 1; i++) out[pos++] = pre[i];
        const char* ht = sysstate64_health_text64();
        for (int i = 0; ht[i] && pos < maxlen - 1; i++) out[pos++] = ht[i];
        const char* mid = "  modules=";
        for (int i = 0; mid[i] && pos < maxlen - 1; i++) out[pos++] = mid[i];
        {
            char t[12]; int n = 0, v = g_mod_count;
            char rev[12];
            if (v == 0) rev[n++] = '0';
            while (v > 0) { rev[n++] = (char)('0' + (v % 10)); v /= 10; }
            while (n > 0 && pos < maxlen - 1) t[0] = 0, out[pos++] = rev[--n];
        }
        const char* tail = "  (per-module detail is in the serial log)\n";
        for (int i = 0; tail[i] && pos < maxlen - 1; i++) out[pos++] = tail[i];
        out[pos] = 0;
    }
    return h;
}

// ==================== syslog 转储 ====================
int sysstate64_dump_syslog64(int max) {
    const int n = sys64_log_count64();
    sys64_logf64("[SYS64] syslog lines=%d ring=%d total=%d", n, (int)SYS64_LOG_RING, g_log_total);
    int shown = 0;
    for (int i = 0; i < n && shown < max; i++, shown++) {
        dbg64_line_begin64();
        dbg64_str("[SYS64] syslog[");
        dbg64_dec((uint64_t)i);
        dbg64_str("] ");
        dbg64_str(sys64_log_line64(i));
        dbg64_nl();
        dbg64_line_end64();
    }
    return n;
}

// ==================== 报告（终端 state 用）====================
int sysstate64_report64(char* out, int maxlen) {
    if (!out || maxlen <= 0) return 0;
    int pos = 0;
    struct W {
        static void s(char* o, int& p, int m, const char* v) {
            for (int i = 0; v && v[i] && p < m - 1; i++) o[p++] = v[i];
        }
        static void d(char* o, int& p, int m, int v) {
            char rev[12]; int n = 0;
            if (v == 0) rev[n++] = '0';
            while (v > 0) { rev[n++] = (char)('0' + (v % 10)); v /= 10; }
            while (n > 0 && p < m - 1) o[p++] = rev[--n];
        }
    };
    W::s(out, pos, maxlen, "state: ");
    W::s(out, pos, maxlen, sysstate64_state_text64());
    W::s(out, pos, maxlen, "  gen=");   W::d(out, pos, maxlen, (int)g_gen);
    W::s(out, pos, maxlen, "  modules="); W::d(out, pos, maxlen, g_mod_count);
    W::s(out, pos, maxlen, "  failed=");  W::d(out, pos, maxlen, g_failed);
    W::s(out, pos, maxlen, "  reason=");  W::s(out, pos, maxlen, g_last_reason);
    W::s(out, pos, maxlen, "\n");
    for (int i = 0; i < g_mod_count; i++) {
        W::s(out, pos, maxlen, "  ");
        W::s(out, pos, maxlen, g_mods[i].name);
        W::s(out, pos, maxlen, "  state=");
        W::s(out, pos, maxlen, sysstate64_module_state_text64(g_mod_state[i]));
        const int mh = sysstate64_module_health64(i);
        W::s(out, pos, maxlen, "  health=");
        W::s(out, pos, maxlen, mh == SYS64_MODH_UP ? "UP" :
                                  (mh == SYS64_MODH_DEGRADED ? "DEGRADED" :
                                  (mh == SYS64_MODH_UNKNOWN ? "unknown" : "DOWN")));
        W::s(out, pos, maxlen, (g_mods[i].flags & SYS64_MODF_CRITICAL) ? "  critical\n" : "\n");
        if (pos >= maxlen - 8) break;
    }
    out[pos] = 0;
    return pos;
}

// ==================== VimtuFS2 卷使用情况（缓存一次）====================
// 读超级块的偏移与 kernel/vfs64.cpp 的 sb_verify 一一对应：
//   0 magic(8) / 8 ver / 12 sector_bytes / 16 block_bytes / 20 total_blocks /
//   28 bitmap_start / 32 bitmap_blocks / 36 inode_start / 40 inode_count / 44 inode_bytes /
//   48 data_start / 52 data_blocks / 60 crc32
static Fs64Info g_fs;
static bool     g_fs_probed = false;

// 主分区起始 LBA：与 app64/vfs64 用的是同一条规则（MBR 的 0x07 项，读不到退回 PART_MAIN_LBA）
static uint32_t fsinfo_part_lba() {
    return app64_main_part_lba64(0);
}

int sysstate64_fsinfo64(Fs64Info* out) {
    if (!out) return -1;
    if (!g_fs_probed) {
        g_fs_probed = true;
        g_fs.ok = 0;
        g_fs.total_blocks = g_fs.free_blocks = g_fs.files = g_fs.used_bytes = 0;
        g_fs.pb_lba = fsinfo_part_lba();
        g_fs.inodes = 0;
        static uint8_t sec[512];
        if (ata64_read_sector(0, g_fs.pb_lba, sec)) {
            // magic "VIMTUFS2"
            const char* m = "VIMTUFS2";
            bool magic_ok = true;
            for (int i = 0; i < 8; i++) if (sec[i] != (uint8_t)m[i]) { magic_ok = false; break; }
            const uint32_t total  = *(const uint32_t*)(sec + 20);
            const uint32_t bm_st  = *(const uint32_t*)(sec + 28);
            const uint32_t bm_n   = *(const uint32_t*)(sec + 32);
            const uint32_t inodes = *(const uint32_t*)(sec + 40);
            const uint32_t data_st = *(const uint32_t*)(sec + 48);
            uint32_t bm_use = bm_n;
            if (magic_ok && total >= 32 && total <= 0x00FFFFFFu && data_st < total && bm_use >= 1 && bm_use <= 8) {
                // 空闲块 = 位图里 [data_start, total) 的 0 位（1 位 = 1 块）
                uint32_t freeb = 0;
                for (uint32_t b = 0; b < bm_use; b++) {
                    if (!ata64_read_sector(0, g_fs.pb_lba + bm_st + b, sec)) break;
                    for (uint32_t k = 0; k < 4096; k++) {
                        const uint32_t blk = b * 4096 + k;
                        if (blk < data_st || blk >= total) continue;
                        if (!(sec[k >> 3] & (1u << (k & 7)))) freeb++;
                    }
                }
                g_fs.ok = 1;
                g_fs.total_blocks = total;
                g_fs.free_blocks = freeb;
                g_fs.inodes = inodes;
            }
        }
        // 文件数/字节数走真 VFS API（挂载过才有值；没挂载保持 0）
        static char names[64][32];
        static uint32_t sizes[64];
        const int n = vfs64_ls("/", names, 64, sizes);
        if (n > 0) {
            g_fs.files = (uint32_t)n;
            uint32_t used = 0;
            for (int i = 0; i < n; i++) used += sizes[i];
            g_fs.used_bytes = used;
        }
    }
    *out = g_fs;
    return g_fs.ok ? 0 : -1;
}
