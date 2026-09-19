// taskmgr64.cpp - Windows 10 风格任务管理器（64 位纯 64 位内核版）
//
// 与 32 位参考实现（Vimtu32/kernel/taskmgr.cpp，2470 行）的关系：
//   * 外观/交互口径照抄：顶部菜单行 + 四页标签行 + 内容区 + 状态栏 + 底部主按钮，
//     选中行高亮、列头、分组标题、CPU 热力单元格、性能页折线图、提示行。
//   * **数据全部来自 64 位真实接口**，没有任何 32 位专用模块：
//       窗口/CPU% ：gui64_window_at() / gui64_window_count() / Window::cpu_pct（外壳 TSC 采样）
//       内存归属 ：mem_owner_bytes_64(w->mem_owner)（kmalloc 归属记账）
//       内存规模 ：mem_total_ram_64() / mem_pool_base_64() / mem_pool_end_64() /
//                   page_count_total_64() / page_count_free_64() / heap_used_64() / heap_total_64()
//       外壳性能 ：gui64_cpu_busy_pct() / gui64_fps()
//       时钟     ：ticks64() / g_irq_total64 / PIT_HZ_64 / rtc_get_time64()
//       内存布局 ：memlayout64.h（ML64_*）+ linker64.ld 的 __bss_end（映像尺寸）
//   * 32 位里有、64 位没有的模块**一律不引用**，对应的列/面板如实标注"未移植/未包含"：
//       - 启动页：启动项与 store64 持久化的接线未做（store64 模块本体已可用）→ 三个启动项
//               固定显示"已启用"，不提供假开关；提示行说明"只在本会话生效 / 需要接线"。
//       - 磁盘：批次 B 起真接线 —— ata64 的 IDENTIFY 结果由 hwinfo_set_disk64 填进 hwinfo64，
//               本页读的是型号 + 容量（没有扇区/吞吐计数器，如实写明）。
//       - 性能页硬件详情（批次 B 接线）：CPU 型号/家族/核数/hypervisor、APIC/SMP 状态、
//               内存总量/页池、磁盘型号与容量、显示适配器（帧缓冲指标 + 实测刷新率）、
//               网络（e1000 MAC/收发）、USB（UHCI/HID 计数）—— 全部只读快照，取不到写原因。
//       - 性能页第 4 项"显卡"：**没有 GPU 驱动**，只报帧缓冲指标（分辨率/缩放/刷新率/后备缓冲）。
//   * "进程页"的行 = **真进程**（proc64 进程表，每进程独立 CR3），不是窗口列表：
//       pid/ppid/名字/状态/CR3 ：proc64_info64()（PROC64_MAX 个槽，空槽跳过；cr3 是进程页表根）
//       线程数 / CPU‰          ：task64_proc_threads64() / task64_proc_cpu_permille64()
//                                （以该进程主任务绑定的 proc 指针为键统计任务表；tick=4ms）
//       结束进程               ：proc64_kill64(pid, SIGKILL=9)（规则在 proc64.cpp：idle/当前进程不可杀）
//     窗口仍然只用于状态栏的窗口计数与性能页（窗口不是进程）。
//
// 多实例：每实例状态在 Window::userdata（kmalloc_64 + MEM_OWNER_TMGR_64 记账）。
//   64 位外壳**没有 on_close 回调**（gui64.h 明确"关窗时外壳不会替你释放"），所以本文件
//   用一张静态实例表 g_slots[] 记录 {窗口, 状态}，在 open/reset/draw 时调 tm_reap()：
//   用 gui64_window_alive()（不解引用悬垂指针）判断窗口是否已被外壳关掉，是则回收状态、
//   并补打 "[APP] tmgr closed"。自己主动关窗（Esc/退出菜单/结束自己）走 tm_close_self()，
//   立即回收并打同一条日志。
//
// 绘制约定：客户区内容用**屏幕绝对坐标**（w->client_x + 局部 x），外壳负责裁剪与脏提交；
//   所有几何都从 w->client_w / w->client_h 推导（窗口可自由缩放/最大化）。
// 刷新：250ms 节流刷新显示值 + gui64_dirty(客户区)；1Hz 推曲线采样点；2s 一条诊断日志。
// 运算：纯整数（内核 -mno-sse），百分比一律 乘100 再除，字节格式化用整数小数位。
//
// 串口日志（自动验收口径，原样）：
//   [APP] tmgr opened / [APP] tmgr reset / [APP] tmgr closed
//   [UI] tmgr page proc|perf|startup|detail
//   [UI] tmgr kill task=<id> slot=<槽位> name=<任务名> rc=<0|1>
//   [UI] tmgr layout client=WxH

#include <stdint.h>

// 注意包含顺序：memlayout64.h 的 ML64_* 必须在 mem_64.h 之前（mem_64.h 把 PAGE_SIZE_64
// 定义成宏，先出现宏会让 memlayout64.h 里的 `static const uint32_t PAGE_SIZE_64` 变成非法声明）。
#include "memlayout64.h"
#undef PAGE_SIZE_64
#include "mem_64.h"

#include "gui64.h"
#include "fb.h"
#include "font.h"
#include "input.h"
#include "x86_64.h"
#include "task64.h"     // 任务表快照 + 进程视角统计（task64_proc_threads64 / _cpu_permille64）
#include "proc64.h"     // 进程表快照（Proc64Info / PROC64_MAX / proc64_kill64）—— 进程页的真数据源
#include "debug64.h"     // 串口打点（[UI] tmgr / [APP] tmgr 行）
#include "edid64.h"     // 显示器 EDID（刷新率来自这里；只读，不改 fb 状态）
#include "config64.h"   // 本轮接线：启动页的开关 = config64 的 startup.*（落到 store64 持久化）
// ---- 批次 B：性能页的硬件详情接线（全部只读快照；取不到就写原因）----
#include "hwinfo64.h"   // CPU 型号/家族/核数/hypervisor + PCI 设备 + 磁盘型号/容量
#include "display64.h"  // 运行期显示层（模式清单 + 0x3DA 实测刷新率 + EDID 对比）
#include "net64.h"      // e1000 状态 + 收发计数（性能页"网络"）
#include "e1000_64.h"   // e1000_mac64：网卡 MAC
#include "usb64.h"      // UHCI 控制器/HID 计数（性能页"USB"）
#include "apic64.h"     // IRQ_MODE64_*：中断路由模式（APIC/PIC）
#include "acpi64.h"     // MADT 的 CPU 数（APIC/SMP 状态）
#include "smp64.h"      // SMP 在线 CPU 数

// linker64.ld 提供：内核映像末地址（.bss 之后，已 4KB 对齐）
extern "C" char __bss_end[];

// ---------------- 布局常量（客户区局部坐标；原点 = 客户区左上角） ----------------
#define TM_MENU_H     24      // 顶部菜单行
#define TM_TAB_Y      25      // 标签行起点（菜单行 + 1px 分隔线）
#define TM_TAB_H      30
#define TM_CONTENT_Y  (TM_TAB_Y + TM_TAB_H)   // 55：内容区起点
#define TM_STATUS_H   24      // 状态栏
#define TM_COL_HDR_H  22      // 列头高度
#define TM_GRP_HDR_H  24      // 分组标题高度
#define TM_ROW_H      22      // 行高
#define TM_BTN_W      110     // 底部主按钮
#define TM_BTN_H      26
#define TM_PAGES      4
#define TM_MENU_ITEMS 4
#define TM_DD_ITEM_H  24
#define TM_DD_PAD     4
#define TM_PERF_ITEMS 4       // 性能页左列表：CPU / 内存 / 磁盘 / 显卡（显卡 = 帧缓冲指标，无 GPU 驱动）
#define TM_MAX_ROWS   40      // 进程页一次最多构造的行数
#define TM_MAX_FIELDS 24      // 字段面板（标签+值）行数上限
#define TM_CURVE_N    60      // 曲线保留最近 60 个采样点（1Hz）
#define TM_MAX_INST   4       // 同类窗口多开上限（第 5 次只激活最近的）
#define TM_REFRESH_MS 250     // 数据显示刷新间隔
#define TM_LOG_MS     2000    // 诊断日志节流
#define TM_GH_MIN     56      // 曲线最小高度（再小就不画）
#define TM_GH_MAX     150     // 曲线最大高度
#define TM_PAD_TEXT   14      // 详情面板上下留白
#define TM_LABEL_MAX  28
#define TM_VALUE_MAX  72
#define TM_MIN_W      560     // 窗口最小尺寸（整体，含标题栏）
#define TM_MIN_H      380

// 颜色（0xFFRRGGBB：与 kernel/setup64.cpp、fb.h 的 rgb() 同一口径）
static const uint32_t TM_COL_TEXT      = 0xFF101010;
static const uint32_t TM_COL_TEXT_DIM  = 0xFF606060;
static const uint32_t TM_COL_BORDER    = 0xFFD0D0D0;
static const uint32_t TM_COL_ACCENT    = 0xFF108CE0;
static const uint32_t TM_COL_MENUBG    = 0xFFF5F5F5;
static const uint32_t TM_COL_CLIENT    = 0xFFFFFFFF;
static const uint32_t TM_COL_HDRBG     = 0xFFF0F0F0;
static const uint32_t TM_COL_HOVER     = 0xFFF0F0F0;
static const uint32_t TM_COL_SEL       = 0xFFCCE4F7;
static const uint32_t TM_COL_STATUSBG  = 0xFFF0F0F0;
static const uint32_t TM_COL_GRID      = 0xFFE0E0E0;
static const uint32_t TM_COL_CURVE     = 0xFF0B5FA5;
static const uint32_t TM_COL_CURVEFILL = 0xFFBBD8F0;
static const uint32_t TM_COL_BTN       = 0xFFE1E1E1;
static const uint32_t TM_COL_BTN_HOVER = 0xFFD0D0D0;
static const uint32_t TM_COL_WARN      = 0xFFC00000;
static const uint32_t TM_COL_OFF       = 0xFFBFBFBF;

// ---------------- 每实例状态（Window::userdata 指向 kmalloc_64 的本结构） ----------------
struct TmState {
    uint8_t  page;                 // 0=进程 1=性能 2=启动 3=详细信息
    int      sel_row;              // 进程页 = g_rows 下标（-1 未选）；启动页 = 0..2
    uint8_t  perf_sel;             // 性能页左列表选中项：0=CPU 1=内存 2=磁盘 3=显卡
    uint8_t  perf_logged_sel;      // 已打过 [UI] tmgr perf 行的选中项（0xFF = 还没打过）
    int      scroll;               // 进程页首行下标
    uint8_t  menu_open;            // "文件"下拉是否展开
    uint8_t  menu_sel;             // 下拉键盘选中项
    uint8_t  cpu_now;              // 最近一次外壳忙占比（gui64_cpu_busy_pct）
    uint8_t  mem_now;              // 最近一次页池占用率（自己做整数除法）
    uint32_t refresh_tick;         // 250ms 刷新节流
    uint32_t sample_sec;           // 上次 1Hz 曲线采样的秒数
    uint32_t last_log_tick;        // 诊断日志节流
    uint32_t proc_log_tick;        // 进程页行日志节流（[UI] tmgr proc rows / row 两类行）
    int      proc_rows_logged;     // 上次行日志里的行数（变行数时立即补一条）
    uint32_t curve[TM_CURVE_N];    // CPU 忙占比曲线（1Hz）
    uint32_t curve_mem[TM_CURVE_N];// 页池占用率曲线（1Hz）
    int      curve_n;
    int      layout_w, layout_h;   // 上次记录的客户区尺寸（变化才打布局日志）
    int      notice_len;
    uint8_t  notice_warn;
    char     notice[96];
    uint8_t  logged_page;          // 已打日志的页（避免重复打 page 行）
};

// ---------------- 实例表（外壳无 on_close 钩子，靠自己回收 userdata） ----------------
struct TmSlot { Window* w; TmState* st; };
static TmSlot   g_slots[TM_MAX_INST + 2];
static Window*  g_last_win = nullptr;     // 最近打开/激活的实例（使用前必须自检）
static bool     g_log_once = false;       // 诊断日志：首次立即打一条

// 进程页行表（数据源 = proc64 进程表；静态临时缓冲：内核单线程，
// 同一时刻只有一个窗口在 draw/click，无需加锁）
struct TmRow {
    uint32_t id;                 // 进程 pid（proc64_kill64 的目标）
    uint32_t ppid;               // 父进程 pid
    uint32_t slot;               // 进程表槽位下标（诊断日志用）
    uint8_t  state;              // Proc64State：READY/RUNNING/SLEEP/EXITED
    uint8_t  is_current;         // Proc64Info.is_current
    uint32_t threads;            // 该进程的任务数（task64_proc_threads64 真值）
    uint32_t cpu_permille;       // CPU‰（task64_proc_cpu_permille64，1 秒采样窗口）
    uint64_t cr3;                // 进程页表根（hex 显示）
    uint64_t pages;              // 用户页数（Proc64Info.pages）
    char     name[PROC64_NAME_MAX];   // 进程名（ASCII）
};
static TmRow g_rows[TM_MAX_ROWS];
static int   g_row_n = 0;   // 最近一次构造的行数（诊断用，绘制/点击都靠它对齐）

// 字段面板行（标签 + 值）
struct TmField {
    char label[TM_LABEL_MAX];
    char value[TM_VALUE_MAX];
};
static TmField g_fields[TM_MAX_FIELDS];

// ---------------- 文案 ----------------
static inline const char* T(const char* en, const char* zh) { return gui64_tr(en, zh); }

static void tm_draw(Window* w);   // 前向声明（窗口身份自检要用它）

// ---------------- 小工具：字符串 / 数字（无 libc，全部自己写） ----------------
static void tm_memzero(void* p, int n) {
    uint8_t* b = (uint8_t*)p;
    for (int i = 0; i < n; i++) b[i] = 0;
}

static int tm_stpcpy(char* d, const char* s) {
    int i = 0;
    if (s) { while (s[i]) { d[i] = s[i]; i++; } }
    d[i] = 0;
    return i;
}

static int tm_stpcat(char* d, const char* s) {
    int i = 0;
    while (d[i]) i++;
    return i + tm_stpcpy(d + i, s);
}

// 按上限拷贝（UTF-8 可能被截在多字节中间：绘制侧会跳过非法字节，不会画出乱码方块）
static void tm_strlcpy(char* d, const char* s, int cap) {
    int i = 0;
    if (cap <= 0) return;
    if (s) { while (s[i] && i < cap - 1) { d[i] = s[i]; i++; } }
    d[i] = 0;
}

static void tm_itoa(char* buf, int v) {
    char tmp[12];
    int n = 0;
    if (v < 0) { *buf++ = '-'; v = -v; }
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) *buf++ = tmp[--n];
    *buf = 0;
}

static void tm_utoa64(char* buf, uint64_t v) {
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    int i = 0;
    while (n > 0) buf[i++] = tmp[--n];
    buf[i] = 0;
}

// 定宽十进制（不足补 0），用于日期/时间
static void tm_pad(char* buf, uint32_t v, int digits) {
    char tmp[12];
    int n = 0;
    while (n < digits) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    int i = 0;
    while (n > 0) buf[i++] = tmp[--n];
    buf[i] = 0;
}

static void tm_pct_str(char* buf, int v) {
    char nb[12];
    tm_itoa(nb, v);
    tm_stpcpy(buf, nb);
    tm_stpcat(buf, "%");
}

// "12.3%"：一位小数百分比（传千分数，纯整数运算）
static void tm_pct1_str(char* buf, uint32_t permille) {
    char nb[16];
    tm_utoa64(nb, (uint64_t)(permille / 10u));
    tm_stpcpy(buf, nb);
    tm_stpcat(buf, ".");
    char f[2];
    f[0] = (char)('0' + (int)(permille % 10u));
    f[1] = 0;
    tm_stpcat(buf, f);
    tm_stpcat(buf, "%");
}

// "1.5 GB" / "512.0 MB" / "1024 KB" / "0 B"（纯整数：小数位 = 余数 *10 / 单位）
static void tm_bytes_str(char* out, uint64_t bytes) {
    char nb[24];
    if (bytes >= (1ull << 30)) {
        uint64_t whole = bytes >> 30;
        uint64_t frac = ((bytes & ((1ull << 30) - 1)) * 10) >> 30;
        tm_utoa64(nb, whole);
        tm_stpcpy(out, nb);
        tm_stpcat(out, ".");
        char f[2]; f[0] = (char)('0' + (int)frac); f[1] = 0;
        tm_stpcat(out, f);
        tm_stpcat(out, " GB");
    } else if (bytes >= (1ull << 20)) {
        uint64_t whole = bytes >> 20;
        uint64_t frac = ((bytes & ((1ull << 20) - 1)) * 10) >> 20;
        tm_utoa64(nb, whole);
        tm_stpcpy(out, nb);
        tm_stpcat(out, ".");
        char f[2]; f[0] = (char)('0' + (int)frac); f[1] = 0;
        tm_stpcat(out, f);
        tm_stpcat(out, " MB");
    } else if (bytes >= 1024) {
        tm_utoa64(nb, bytes / 1024);
        tm_stpcpy(out, nb);
        tm_stpcat(out, " KB");
    } else {
        tm_utoa64(nb, bytes);
        tm_stpcpy(out, nb);
        tm_stpcat(out, " B");
    }
}

// "0x100000"（去掉前导 0；0 打印 "0x0"）
static void tm_hex_str(char* out, uint64_t v) {
    static const char* HD = "0123456789ABCDEF";
    char tmp[17];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = HD[v & 0xF]; v >>= 4; }
    int i = 0;
    out[i++] = '0'; out[i++] = 'x';
    while (n > 0) out[i++] = tmp[--n];
    out[i] = 0;
}

// "H:MM:SS"（tick → PIT_HZ_64 = 250Hz）
static void tm_uptime_str(char* out, uint64_t ticks) {
    uint64_t sec = ticks / PIT_HZ_64;
    uint64_t h = sec / 3600, m = (sec / 60) % 60, s = sec % 60;
    char nb[24];
    tm_utoa64(nb, h);
    tm_stpcpy(out, nb);
    tm_stpcat(out, ":");
    tm_pad(nb, (uint32_t)m, 2); tm_stpcat(out, nb);
    tm_stpcat(out, ":");
    tm_pad(nb, (uint32_t)s, 2); tm_stpcat(out, nb);
}

// "名称 (N)"
static void tm_count_label(char* buf, const char* name, int n) {
    char nb[12];
    tm_itoa(nb, n);
    tm_stpcpy(buf, name);
    tm_stpcat(buf, " (");
    tm_stpcat(buf, nb);
    tm_stpcat(buf, ")");
}

// ---------------- 文本绘制（ASCII 用 bahnschrift，中文用 simhei） ----------------
static void tm_text_clip(int x, int y, const char* s, uint32_t fg, int maxw) {
    if (!s || maxw <= 0) return;
    int face = -1;
    int cx = x, used = 0;
    while (*s) {
        int adv = 0;
        uint32_t cp = font_utf8_decode(s, &adv);
        if (cp == 0 || adv <= 0) { s++; continue; }        // 非法字节：跳过，避免死循环
        int gw = (cp < 0x80) ? font_glyph_advance((char)cp) : font_glyph_advance_cp(cp);
        if (gw <= 0) { s += adv; continue; }
        if (used + gw > maxw) break;                        // 裁剪到列宽（不换行、不越界）
        int want = (cp < 0x80) ? 0 : 2;
        if (want != face) { font_select(want); face = want; }
        if (cp < 0x80) font_draw_glyph(cx, y, (char)cp, fg);
        else           font_draw_glyph_cp(cx, y, cp, fg);
        cx += gw;
        used += gw;
        s += adv;
    }
    font_select(0);                                        // 复原，否则调用方拿到中文字体
}

static void tm_text(int x, int y, const char* s, uint32_t fg) {
    if (!s) return;
    tm_text_clip(x, y, s, fg, 1 << 20);
}

static int tm_text_w(const char* s) {
    if (!s) return 0;
    int w = 0;
    while (*s) {
        int adv = 0;
        uint32_t cp = font_utf8_decode(s, &adv);
        if (cp == 0 || adv <= 0) { s++; continue; }
        w += (cp < 0x80) ? font_glyph_advance((char)cp) : font_glyph_advance_cp(cp);
        s += adv;
    }
    return w;
}

// 右对齐画一段文本（数值列 / 列头共用）
static void tm_text_right(int x_right, int y, const char* s, uint32_t fg) {
    if (!s) return;
    int tw = tm_text_w(s);
    tm_text_clip(x_right - tw, y, s, fg, tw + 4);
}

// ---------------- 鼠标位置（客户区局部坐标） ----------------
// 与 32 位一致：物理坐标 -> 渲染坐标（缩放换算），再减客户区原点。
// 缩放为 100% 时两者相同（默认就是 100%）。
static void tm_mouse_client(Window* w, int* cx, int* cy) {
    int pw = fb_phys_width(), ph = fb_phys_height();
    if (pw <= 0) pw = fb_width();
    if (ph <= 0) ph = fb_height();
    int mx = mouse_get_x() * fb_width() / pw;
    int my = mouse_get_y() * fb_height() / ph;
    *cx = mx - w->client_x;
    *cy = my - w->client_y;
}

// ---------------- 日志（自动验收靠这些行） ----------------
// ★ 全部走行锁：本文件的绘制/点击里可能被 PIT 抢占，别的任务（如 task64 的回收日志，
//   它自己是带锁的）会把一行从中间插断 —— 实测被插断过 "[UI] tmgr page proc" 与
//   "[UI] tmgr kill proc ... rc=0"，导致验收脚本假失败。行锁是 begin/end 配对的（可重入）。
static void tm_log_app(const char* what) {
    dbg64_line_begin64();
    dbg64_str("[APP] tmgr ");
    dbg64_str(what);
    dbg64_nl();
    dbg64_line_end64();
}

static void tm_log_page(const char* key) {
    dbg64_line_begin64();
    dbg64_str("[UI] tmgr page ");
    dbg64_str(key);
    dbg64_nl();
    dbg64_line_end64();
}

static void tm_log_layout(int cw, int ch) {
    dbg64_line_begin64();
    dbg64_str("[UI] tmgr layout client=");
    dbg64_dec((uint64_t)cw);
    dbg64_str("x");
    dbg64_dec((uint64_t)ch);
    dbg64_nl();
    dbg64_line_end64();
}

// 结束进程（进程页 = proc64 真进程，所以打 pid/名字 + proc64_kill64 返回码）
static void tm_log_kill_proc(const TmRow* r, int64_t rc) {
    dbg64_line_begin64();
    dbg64_str("[UI] tmgr kill proc pid=");
    dbg64_dec((uint64_t)(r ? r->id : 0));
    dbg64_str(" name=");
    dbg64_str((r && r->name[0]) ? r->name : "?");
    dbg64_str(" sig=9 rc=");
    if (rc < 0) dbg64_putc('-');
    dbg64_dec((uint64_t)(rc < 0 ? -rc : rc));
    dbg64_nl();
    dbg64_line_end64();
}

static void tm_log_str(const char* tag, const char* body) {
    dbg64_line_begin64();
    dbg64_str("[UI] tmgr ");
    dbg64_str(tag);
    if (body) dbg64_str(body);
    dbg64_nl();
    dbg64_line_end64();
}

// ---------------- 实例状态访问 / 回收 ----------------
static bool tm_is_tmgr_window(Window* w) {
    return w && w->app_id == APP_ID_TMGR && w->draw == tm_draw;
}

static TmState* tm_state(Window* w) {
    if (!tm_is_tmgr_window(w)) return nullptr;
    return (TmState*)w->userdata;
}

static void tm_dirty_client(Window* w) {
    if (!w) return;
    gui64_dirty(w->client_x, w->client_y, w->client_w, w->client_h);
}

// 释放状态（归属记账到 TMGR，与分配时对称）
static void tm_free_state(TmState* st) {
    if (!st) return;
    int prev = mem_owner_get_64();
    mem_owner_set_64(MEM_OWNER_TMGR_64);
    kfree_64(st);
    mem_owner_set_64(prev);
}

static void tm_slot_add(Window* w, TmState* st) {
    for (int i = 0; i < (int)(sizeof(g_slots) / sizeof(g_slots[0])); i++) {
        if (!g_slots[i].w || g_slots[i].w == w) { g_slots[i].w = w; g_slots[i].st = st; return; }
    }
}

static void tm_slot_release(Window* w) {
    for (int i = 0; i < (int)(sizeof(g_slots) / sizeof(g_slots[0])); i++) {
        if (g_slots[i].w == w) { g_slots[i].w = nullptr; g_slots[i].st = nullptr; }
    }
}

// 回收被外壳直接关掉的实例：gui64_window_alive() 不解引用悬垂指针，可安全用于死窗口。
// 判活通过后再比对 userdata 身份，避免"地址被新窗口复用"时误释放别人的状态。
static void tm_reap() {
    for (int i = 0; i < (int)(sizeof(g_slots) / sizeof(g_slots[0])); i++) {
        Window* w = g_slots[i].w;
        if (!w) continue;
        bool alive = gui64_window_alive(w);
        if (alive && (!tm_is_tmgr_window(w) || w->userdata != g_slots[i].st)) {
            // 指针被复用成别的窗口：只摘表，不动别人的状态
            g_slots[i].w = nullptr;
            g_slots[i].st = nullptr;
            continue;
        }
        if (alive) continue;
        TmState* st = g_slots[i].st;
        g_slots[i].w = nullptr;
        g_slots[i].st = nullptr;
        if (g_last_win == w) g_last_win = nullptr;
        tm_free_state(st);
        tm_log_app("closed");
    }
}

// 找任意一个存活实例（多实例下内部指针只缓存"最近一个"）
static Window* tm_any_window() {
    if (g_last_win && gui64_window_alive(g_last_win) && tm_is_tmgr_window(g_last_win)) return g_last_win;
    int n = gui64_window_count();
    Window* found = nullptr;
    for (int i = 0; i < n; i++) {
        Window* w = gui64_window_at(i);
        if (tm_is_tmgr_window(w)) found = w;      // 取最后一个（最近的）
    }
    return found;
}

// 主动关窗：先摘状态再销毁窗口（外壳不会释放 userdata）
static void tm_close_self(Window* w) {
    if (!w) return;
    TmState* st = (TmState*)w->userdata;
    w->userdata = nullptr;
    tm_slot_release(w);
    if (g_last_win == w) g_last_win = nullptr;
    tm_free_state(st);
    tm_log_app("closed");
    gui64_destroy_window(w);
}

// ---------------- 提示行（每实例） ----------------
static void tm_notice(TmState* st, const char* s, bool warn) {
    if (!st || !s) return;
    tm_strlcpy(st->notice, s, (int)sizeof(st->notice));
    st->notice_len = tm_text_w(st->notice);
    st->notice_warn = warn ? 1 : 0;
}

// "前缀: 尾巴"（尾巴过长会被截断到缓冲区上限）
static void tm_notice2(TmState* st, const char* prefix, const char* tail, bool warn) {
    if (!st) return;
    char buf[96];
    tm_strlcpy(buf, prefix ? prefix : "", (int)sizeof(buf));
    tm_stpcat(buf, tail ? tail : "");
    tm_notice(st, buf, warn);
}

static void tm_clear_notice(TmState* st) {
    if (!st) return;
    st->notice[0] = 0;
    st->notice_len = 0;
}

// ---------------- 真实数据读取 ----------------
// 进程状态文案：与 proc64.h 的 Proc64State 一一对应（真值，不猜）
static const char* tm_proc_state_text(uint8_t state) {
    switch (state) {
        case PROC64_READY:   return T("Ready", "就绪");
        case PROC64_RUNNING: return T("Running", "运行");
        case PROC64_SLEEP:   return T("Sleeping", "睡眠");
        case PROC64_EXITED:  return T("Exited", "已退出");
        default:             return T("Unknown", "未知");
    }
}

// 页池占用（已用页 / 总页），整数百分比
static int tm_page_used_pct() {
    uint64_t total = page_count_total_64();
    uint64_t free_pages = page_count_free_64();
    if (total == 0) return 0;
    uint64_t used = (total > free_pages) ? (total - free_pages) : 0;
    uint64_t pct = used * 100u / total;
    if (pct > 100) pct = 100;
    return (int)pct;
}

static int tm_heap_used_pct() {
    uint64_t total = heap_total_64();
    if (total == 0) return 0;
    uint64_t pct = heap_used_64() * 100u / total;
    if (pct > 100) pct = 100;
    return (int)pct;
}

// 进程页行表：每行 = 一个**真实进程**（proc64 进程表槽位 0..PROC64_MAX-1，空槽按 proc64_info64()==0 跳过）
// 线程数 / CPU‰ 走 task64 的进程视角统计（以该进程主任务绑定的 proc 指针为键）。
static int tm_build_rows(int max) {
    int n = 0;
    for (int i = 0; i < PROC64_MAX && n < max && n < TM_MAX_ROWS; i++) {
        Proc64Info in;
        if (proc64_info64(i, &in) == 0) continue;     // 空槽：跳过，不造行
        TmRow* r = &g_rows[n];
        r->slot = (uint32_t)i;
        r->id = in.pid;
        r->ppid = in.ppid;
        r->state = (uint8_t)in.state;
        r->is_current = (uint8_t)in.is_current;
        r->cr3 = in.cr3;
        r->pages = in.pages;
        r->threads = (uint32_t)task64_proc_threads64(in.task_id);
        r->cpu_permille = task64_proc_cpu_permille64(in.task_id);
        tm_strlcpy(r->name, in.name, (int)sizeof(r->name));
        n++;
    }
    g_row_n = n;
    return n;
}

// 每次刷新（250ms）更新显示用的 CPU/内存百分比（都是廉价 getter）
static void tm_read_now(TmState* st) {
    if (!st) return;
    st->cpu_now = gui64_cpu_busy_pct();
    st->mem_now = (uint8_t)tm_page_used_pct();
}

// 1Hz：曲线推一个点（满 60 点后整体左移，保留最近 60 秒）
static void tm_push_curve(TmState* st) {
    if (!st) return;
    if (st->curve_n < TM_CURVE_N) {
        st->curve[st->curve_n] = st->cpu_now;
        st->curve_mem[st->curve_n] = st->mem_now;
        st->curve_n++;
    } else {
        for (int i = 1; i < TM_CURVE_N; i++) {
            st->curve[i - 1] = st->curve[i];
            st->curve_mem[i - 1] = st->curve_mem[i];
        }
        st->curve[TM_CURVE_N - 1] = st->cpu_now;
        st->curve_mem[TM_CURVE_N - 1] = st->mem_now;
    }
}

// ---------------- 几何 helper（draw 与 click 必须共用，否则点不中） ----------------
// 窗口可自由缩放：下面所有坐标都从客户区宽高推导，没有写死的屏幕尺寸。
static int tm_status_y(Window* w) {
    int y = w->client_h - TM_STATUS_H;
    if (y < TM_CONTENT_Y + 8) y = TM_CONTENT_Y + 8;
    return y;
}

static int tm_btn_x(Window* w) {
    int x = w->client_w - TM_BTN_W - 20;
    if (x < 8) x = 8;
    return x;
}

static int tm_btn_y(Window* w) {
    int y = w->client_h - TM_STATUS_H - TM_BTN_H - 12;
    if (y < TM_CONTENT_Y + 8) y = TM_CONTENT_Y + 8;
    return y;
}

static int tm_list_top() { return TM_CONTENT_Y + TM_COL_HDR_H + 2; }

static int tm_list_bot(Window* w) {
    int b = tm_btn_y(w) - 6;
    if (b > tm_status_y(w)) b = tm_status_y(w);
    return b;
}

// 进程页行区上沿（列头 + 分组标题之后）
static int tm_rows_top() { return tm_list_top() + TM_GRP_HDR_H; }

// 列几何（全部从客户区宽度推导；数字列右对齐，窗口再窄也只是裁剪，不会交叉压字）
static int tm_col_name_x() { return 42; }

// CPU% 列（热力单元格，右端）
static int tm_col_cpu_right(Window* w) { return w->client_w - 12; }

static int tm_col_cpu_w(Window* w) {
    int cw = (w->client_w * 14) / 100;
    if (cw > 76) cw = 76;
    if (cw < 56) cw = 56;
    return cw;
}

static int tm_col_cpu_x(Window* w) { return tm_col_cpu_right(w) - tm_col_cpu_w(w); }

// 被切入次数列（数值右对齐）
static int tm_col_sw_right(Window* w) {
    int x = tm_col_cpu_x(w) - 10;
    if (x < tm_col_name_x() + 150) x = tm_col_name_x() + 150;
    return x;
}

// 累计 tick 列（数值右对齐）
static int tm_col_ticks_right(Window* w) { return tm_col_sw_right(w) - 60; }
static int tm_col_ticks_x(Window* w) { return tm_col_ticks_right(w) - 64; }

// ID 列（数值右对齐；"*" = 当前任务标记写在名字列里）
static int tm_col_id_right(Window* w) {
    int x = (w->client_w * 34) / 100;
    if (x < tm_col_name_x() + 82) x = tm_col_name_x() + 82;
    return x;
}

// 状态列（左对齐；夹在 id 列与 tick 列之间）
static int tm_col_state_x(Window* w) {
    int x = tm_col_id_right(w) + 14;
    int hi = tm_col_ticks_x(w) - 52;
    if (x > hi) x = hi;
    return x;
}

// 标签行 / 菜单行
static const char* tm_tab_label(int i) {
    switch (i) {
        case 0: return T("Processes", "进程");
        case 1: return T("Performance", "性能");
        case 2: return T("Startup", "启动");
        default: return T("Details", "详细信息");
    }
}

static const char* tm_page_key(uint8_t page) {
    switch (page) {
        case 1: return "perf";
        case 2: return "startup";
        case 3: return "detail";
        default: return "proc";
    }
}

static int tm_tab_x(int i) {
    int x = 16;
    for (int k = 0; k < i; k++) x += tm_text_w(tm_tab_label(k)) + 34;
    return x;
}

static int tm_tab_box_w(int i) { return tm_text_w(tm_tab_label(i)) + 34; }

static const char* tm_menu_label(int i) {
    switch (i) {
        case 0: return T("File", "文件");
        case 1: return T("Options", "选项");
        case 2: return T("View", "查看");
        default: return T("Help", "帮助");
    }
}

static const char* tm_menu_item(int i) {
    switch (i) {
        case 0: return T("Run new task", "运行新任务");
        case 1: return T("Restart system", "重启系统");
        case 2: return T("Shut down", "关机");
        default: return T("Exit Task Manager", "退出任务管理器");
    }
}

static int tm_menubtn_x(int i) {
    int x = 4;
    for (int k = 0; k < i; k++) x += tm_text_w(tm_menu_label(k)) + 26;
    return x;
}

static int tm_menubtn_w(int i) { return tm_text_w(tm_menu_label(i)) + 26; }

static int tm_dropdown_w() {
    int w = 0;
    for (int i = 0; i < TM_MENU_ITEMS; i++) {
        int tw = tm_text_w(tm_menu_item(i));
        if (tw > w) w = tw;
    }
    return w + 28;
}

// ---------------- 字段面板数据（全部实测；取不到就如实写原因） ----------------
static void tm_field_set(TmField* f, const char* label, const char* value) {
    tm_strlcpy(f->label, label ? label : "", TM_LABEL_MAX);
    tm_strlcpy(f->value, value ? value : "-", TM_VALUE_MAX);
}

static void tm_field_num(TmField* f, const char* label, uint64_t v, const char* unit) {
    char b[40];
    tm_utoa64(b, v);
    if (unit && unit[0]) tm_stpcat(b, unit);
    tm_field_set(f, label, b);
}

static void tm_field_bytes(TmField* f, const char* label, uint64_t bytes) {
    char b[40];
    tm_bytes_str(b, bytes);
    tm_field_set(f, label, b);
}

// 当前 CR3（运行时实测；PML4 物理地址由 loader 写死在 ML64_PML4_PHYS）
static uint64_t tm_read_cr3() {
    uint64_t v = 0;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
    return v;
}

static int tm_fields_cpu(TmState* st, TmField* out, int max) {
    int n = 0;
    char b[48];
    if (n < max) { tm_field_num(&out[n], T("IRQ total", "中断总数"), (uint64_t)g_irq_total64, ""); n++; }
    if (n < max) { tm_field_num(&out[n], T("Shell CPU", "外壳忙占比"), (uint64_t)gui64_cpu_busy_pct(), "%"); n++; }
    if (n < max) { tm_field_num(&out[n], T("FPS", "帧率"), (uint64_t)gui64_fps(), ""); n++; }
    // 刷新率（批次 B 起优先**实测**）：display64 的采用值（0x3DA 实测 > CRTC 推算 > EDID）；
    // 没有实测又没有 EDID 就如实写原因，绝不编数字。
    if (n < max) {
        const Disp64Info* di = display64_info64();
        char rb[20];
        if (di->refresh_x10 > 0) {
            display64_refresh_str64(rb, (int)sizeof(rb));
            tm_strlcpy(b, rb, (int)sizeof(b));
            tm_stpcat(b, T(" Hz  (src=", " Hz（来源="));
            tm_stpcat(b, display64_src_name64(di->src));
            tm_stpcat(b, T(")", "）"));
            tm_field_set(&out[n], T("Refresh rate", "刷新率"), b);
        } else {
            const Edid64* ed = edid64_get();
            if (ed->valid && ed->refresh_x10) {
                edid64_refresh_str64(rb, (int)sizeof(rb));
                tm_strlcpy(b, rb, (int)sizeof(b));
                tm_stpcat(b, " Hz (EDID)");
                tm_field_set(&out[n], T("Refresh rate", "刷新率"), b);
            } else {
                tm_field_set(&out[n], T("Refresh rate", "刷新率"),
                             T("unknown (0x3DA not measurable; no EDID)", "未知（0x3DA 不可测、也没有 EDID）"));
            }
        }
        n++;
    }
    if (n < max) { tm_field_num(&out[n], T("PIT Hz", "PIT 频率"), (uint64_t)PIT_HZ_64, " Hz"); n++; }
    if (n < max) { tm_field_num(&out[n], T("Ticks", "系统 tick"), (uint64_t)ticks64(), ""); n++; }
    if (n < max) { tm_uptime_str(b, (uint64_t)ticks64()); tm_field_set(&out[n], T("Uptime", "运行时间"), b); n++; }
    {
        uint32_t sum = 0;
        int wc = gui64_window_count();
        for (int i = 0; i < wc; i++) {
            Window* w = gui64_window_at(i);
            if (w) sum += w->cpu_pct;
        }
        if (n < max) { tm_field_num(&out[n], T("Window CPU sum", "窗口 CPU 合计"), sum, "%"); n++; }
        if (n < max) { tm_field_num(&out[n], T("Windows", "窗口数"), (uint64_t)wc, ""); n++; }
    }
    // ---- 批次 B：CPU 身份与 APIC/SMP 状态（hwinfo64 / acpi64 / smp64 / apic64 只读快照）----
    {
        const HwInfo64* hw = hw_info64();
        if (n < max) {
            const char* vend = (hw->magic == HW64_INFO_MAGIC && hw->cpu.vendor[0]) ? hw->cpu.vendor : "unknown";
            tm_field_set(&out[n], T("CPU vendor", "CPU 厂商"), vend);
            n++;
        }
        if (n < max) {
            const char* brand = (hw->magic == HW64_INFO_MAGIC && hw->cpu.brand[0]) ? hw->cpu.brand : "unknown";
            tm_field_set(&out[n], T("CPU brand", "CPU 型号"), brand);
            n++;
        }
        if (n < max) {
            char fb[48];
            tm_strlcpy(fb, T("family=", "家族="), (int)sizeof(fb));
            { char nb[16]; tm_utoa64(nb, hw->magic == HW64_INFO_MAGIC ? hw->cpu.family : 0); tm_stpcat(fb, nb); }
            tm_stpcat(fb, T(" model=", " 型号="));
            { char nb[16]; tm_utoa64(nb, hw->magic == HW64_INFO_MAGIC ? hw->cpu.model : 0); tm_stpcat(fb, nb); }
            tm_stpcat(fb, T(" stepping=", " 步进="));
            { char nb[16]; tm_utoa64(nb, hw->magic == HW64_INFO_MAGIC ? hw->cpu.stepping : 0); tm_stpcat(fb, nb); }
            tm_field_set(&out[n], T("CPU family/model", "CPU 家族/型号"), fb);
            n++;
        }
        if (n < max) {
            char cb[48];
            tm_strlcpy(cb, T("logical cores=", "逻辑核数="), (int)sizeof(cb));
            { char nb[16]; tm_utoa64(nb, hw->magic == HW64_INFO_MAGIC ? (hw->cpu.cores ? hw->cpu.cores : 1) : 1); tm_stpcat(cb, nb); }
            tm_stpcat(cb, T("  hypervisor=", " 虚拟化平台="));
            tm_stpcat(cb, (hw->magic == HW64_INFO_MAGIC && hw->cpu.hypervisor[0]) ? hw->cpu.hypervisor : "none");
            tm_field_set(&out[n], T("CPU cores/hypervisor", "CPU 核数/虚拟化"), cb);
            n++;
        }
        if (n < max) {
            char ab[64];
            tm_strlcpy(ab, T("irq route=", "中断路由="), (int)sizeof(ab));
            tm_stpcat(ab, (g_irq_mode64 == IRQ_MODE64_APIC) ? "APIC(LAPIC+IOAPIC)" : "8259 PIC");
            tm_stpcat(ab, T("  ACPI cpus=", "  ACPI CPU 数="));
            { char nb[16]; tm_utoa64(nb, (uint64_t)acpi_cpu_count64()); tm_stpcat(ab, nb); }
            tm_stpcat(ab, T("  SMP online=", "  SMP 在线="));
            { char nb[16]; tm_utoa64(nb, (uint64_t)smp64_online_cpu_count64()); tm_stpcat(ab, nb); }
            tm_field_set(&out[n], T("APIC/SMP", "APIC/SMP 状态"), ab);
            n++;
        }
    }
    if (n < max) {
        tm_itoa(b, st ? st->curve_n : 0);
        tm_stpcat(b, T(" (1Hz)", " (1Hz)"));
        tm_field_set(&out[n], T("Curve samples", "曲线采样点"), b);
        n++;
    }
    if (n < max) {
        tm_field_set(&out[n], T("Source", "数据来源"),
                     T("shell TSC sampling of window callbacks (Window::cpu_pct)",
                       "外壳对该窗口回调的 TSC 采样（Window::cpu_pct）"));
        n++;
    }
    if (n < max) {
        tm_field_set(&out[n], T("Note", "说明"),
                     T("per-task CPU accounting comes from task64 ticks; the process page uses the proc64 table",
                       "按任务的 CPU 统计来自 task64 的 tick 累计；进程页的数据源是 proc64 进程表"));
        n++;
    }
    return n;
}

static int tm_fields_mem(TmField* out, int max) {
    int n = 0;
    char b[64];
    char a[32], c[32];
    uint64_t pt = page_count_total_64();
    uint64_t pf = page_count_free_64();
    uint64_t pu = (pt > pf) ? (pt - pf) : 0;

    if (n < max) { tm_field_bytes(&out[n], T("RAM top", "物理内存上界"), mem_total_ram_64()); n++; }
    if (n < max) {
        tm_bytes_str(a, mem_pool_base_64());
        tm_bytes_str(c, mem_pool_end_64());
        tm_strlcpy(b, a, (int)sizeof(b));
        tm_stpcat(b, " ~ ");
        tm_stpcat(b, c);
        tm_field_set(&out[n], T("Page pool", "页池范围"), b);
        n++;
    }
    if (n < max) { tm_field_num(&out[n], T("Pages total", "页总数"), pt, ""); n++; }
    if (n < max) { tm_field_num(&out[n], T("Pages used", "已用页"), pu, ""); n++; }
    if (n < max) { tm_field_num(&out[n], T("Pages free", "空闲页"), pf, ""); n++; }
    if (n < max) { tm_field_num(&out[n], T("Page usage", "页占用率"), (uint64_t)tm_page_used_pct(), "%"); n++; }
    if (n < max) { tm_field_bytes(&out[n], T("Heap used", "内核堆已用"), heap_used_64()); n++; }
    if (n < max) { tm_field_bytes(&out[n], T("Heap total", "内核堆总量"), heap_total_64()); n++; }
    if (n < max) { tm_field_num(&out[n], T("Heap usage", "堆占用率"), (uint64_t)tm_heap_used_pct(), "%"); n++; }

    // 按 owner 的明细（mem_owner_name_64 + mem_owner_bytes_64：真实归属记账）
    for (int i = 0; i < MEM_OWNER_COUNT_64 && n < max; i++) {
        const char* nm = mem_owner_name_64(i);
        tm_strlcpy(b, T("owner: ", "归属: "), (int)sizeof(b));
        tm_stpcat(b, nm ? nm : "?");
        tm_field_bytes(&out[n], b, mem_owner_bytes_64(i));
        n++;
    }
    return n;
}

// 磁盘（批次 B 重写）：**ata64 已经在系统内核里**，IDENTIFY 的结果由 ata64 填进 hwinfo64
// 的磁盘表（hwinfo_set_disk64）。本页读的就是那份真值：型号 + 容量；没有盘就如实写"无"。
static int tm_fields_disk(TmField* out, int max) {
    int n = 0;
    char b[64];
    const HwInfo64* hw = hw_info64();
    const bool have = (hw->magic == HW64_INFO_MAGIC);
    int disks = 0;
    if (have) {
        for (uint32_t i = 0; i < HW64_DISK_MAX; i++) if (hw->disks[i].present) disks++;
    }
    if (n < max) {
        tm_field_set(&out[n], T("Driver", "驱动"),
                     T("ata64 (PIO, IRQ14 wait + polling fallback) is in this kernel",
                       "ata64（PIO，IRQ14 等待 + 超时回退轮询）已在本内核里"));
        n++;
    }
    if (n < max) {
        tm_field_num(&out[n], T("Disks (IDENTIFY)", "磁盘数（IDENTIFY）"), (uint64_t)disks, "");
        n++;
    }
    for (uint32_t i = 0; i < HW64_DISK_MAX && n < max; i++) {
        if (!have || !hw->disks[i].present) continue;
        const uint64_t sectors = hw->disks[i].sectors_512;
        char lb[32];
        tm_strlcpy(lb, T("Disk ", "磁盘 "), (int)sizeof(lb));
        { char nb[16]; tm_utoa64(nb, (uint64_t)i); tm_stpcat(lb, nb); }
        char vb[64];
        tm_strlcpy(vb, hw->disks[i].model[0] ? hw->disks[i].model : "(no model string)", (int)sizeof(vb));
        tm_stpcat(vb, "  ");
        tm_bytes_str(b, sectors * 512ull);
        tm_stpcat(vb, b);
        tm_stpcat(vb, T("  (LBA28 PIO; LBA48 flag not claimed)", "（LBA28 PIO；不声称 LBA48）"));
        tm_field_set(&out[n], lb, vb);
        n++;
    }
    if (n < max) {
        if (disks == 0) {
            tm_field_set(&out[n], T("Status", "状态"),
                         T("no ATA disk reported by IDENTIFY (or hwinfo not initialized)",
                           "IDENTIFY 没有报告任何 ATA 盘（或 hwinfo 未初始化）"));
        } else {
            tm_field_set(&out[n], T("Source", "数据来源"),
                         T("ata64 IDENTIFY -> hwinfo_set_disk64 -> hwinfo64 disk table",
                           "ata64 IDENTIFY -> hwinfo_set_disk64 -> hwinfo64 磁盘表"));
        }
        n++;
    }
    if (n < max) {
        tm_strlcpy(b, T("kernel LBA ", "内核 LBA "), (int)sizeof(b));
        char nb[24];
        tm_utoa64(nb, (uint64_t)ML64_KERNEL_LBA); tm_stpcat(b, nb);
        tm_stpcat(b, " + ");
        tm_utoa64(nb, (uint64_t)ML64_KERNEL_SECTORS); tm_stpcat(b, nb);
        tm_stpcat(b, T(" sectors", " 扇区"));
        tm_field_set(&out[n], T("Disk layout", "磁盘布局"), b);
        n++;
    }
    if (n < max) {
        char nb[24];
        tm_strlcpy(b, "store LBA ", (int)sizeof(b));
        tm_utoa64(nb, (uint64_t)ML64_STORE_LBA); tm_stpcat(b, nb);
        tm_stpcat(b, " + ");
        tm_utoa64(nb, (uint64_t)ML64_STORE_SECTORS); tm_stpcat(b, nb);
        tm_stpcat(b, T(" sectors reserved for store fallback slots", " 扇区是 store 兜底槽区"));
        tm_field_set(&out[n], T("Persistence", "持久化"), b);
        n++;
    }
    if (n < max) {
        tm_field_set(&out[n], T("Note", "说明"),
                     T("no sector/throughput counters in this driver (honest: only IDENTIFY + PIO transfers)",
                       "本驱动没有扇区/吞吐计数器（如实：只有 IDENTIFY + PIO 传输）"));
        n++;
    }
    return n;
}

// 显卡（批次 B 新增，32 位性能页的第 4 项）：**只报帧缓冲指标** + VGA PCI 设备 + 网络/USB/APIC-SMP。
// 为什么网络/USB 挂在这里：性能页只有 4 个左列表项（CPU/内存/磁盘/显卡），而"适配器/外设"这一档
//   最贴近"显卡"项；每一项都注明数据来源，取不到就写原因。绝不假装有 GPU 驱动。
static int tm_fields_gpu(TmField* out, int max) {
    int n = 0;
    char b[72];
    const Disp64Info* di = display64_info64();
    const HwInfo64* hw = hw_info64();

    if (n < max) {
        tm_field_set(&out[n], T("Adapter", "显示适配器"),
                     T("VimtuOS framebuffer (VBE LFB)  -  no GPU driver",
                       "VimtuOS 帧缓冲（VBE LFB）——没有 GPU 驱动"));
        n++;
    }
    if (n < max) {
        tm_strlcpy(b, T("physical ", "物理 "), (int)sizeof(b));
        char nb[16];
        tm_utoa64(nb, (uint64_t)fb_phys_width()); tm_stpcat(b, nb);
        tm_stpcat(b, "x");
        tm_utoa64(nb, (uint64_t)fb_phys_height()); tm_stpcat(b, nb);
        tm_stpcat(b, " @32bpp pitch=");
        tm_utoa64(nb, (uint64_t)(di->pitch > 0 ? di->pitch : fb_phys_width() * 4)); tm_stpcat(b, nb);
        tm_field_set(&out[n], T("Framebuffer mode", "帧缓冲模式"), b);
        n++;
    }
    if (n < max) {
        tm_strlcpy(b, T("render ", "渲染 "), (int)sizeof(b));
        char nb[16];
        tm_utoa64(nb, (uint64_t)fb_width()); tm_stpcat(b, nb);
        tm_stpcat(b, "x");
        tm_utoa64(nb, (uint64_t)fb_height()); tm_stpcat(b, nb);
        tm_stpcat(b, T("  zoom=", "  缩放="));
        tm_utoa64(nb, (uint64_t)fb_get_zoom()); tm_stpcat(b, nb);
        tm_stpcat(b, "%");
        tm_field_set(&out[n], T("Render mode", "渲染模式"), b);
        n++;
    }
    if (n < max) {
        if (di->refresh_x10 > 0) {
            char rb[16];
            display64_refresh_str64(rb, (int)sizeof(rb));
            tm_strlcpy(b, rb, (int)sizeof(b));
            tm_stpcat(b, T(" Hz  (src=", " Hz（来源="));
            tm_stpcat(b, display64_src_name64(di->src));
            if (di->edid_x10 > 0) {
                tm_stpcat(b, T(", EDID match=", "，EDID 对比="));
                char nb[8];
                tm_utoa64(nb, (uint64_t)di->edid_match);
                tm_stpcat(b, nb);
            }
            tm_stpcat(b, ")");
            tm_field_set(&out[n], T("Refresh rate", "刷新率"), b);
        } else {
            tm_field_set(&out[n], T("Refresh rate", "刷新率"),
                         T("unknown (0x3DA not measurable here; no EDID)",
                           "未知（本平台 0x3DA 不可测；也没有 EDID）"));
        }
        n++;
    }
    if (n < max) {
        tm_field_num(&out[n], T("Back buffer", "后备缓冲"),
                     (uint64_t)fb_phys_width() * (uint64_t)fb_phys_height() * 4ull, " B");
        n++;
    }
    if (n < max) {
        int vga = 0;
        uint16_t vdv = 0, vdd = 0;
        if (hw->magic == HW64_INFO_MAGIC) {
            vga = (int)hw->pci.vga;
            for (uint32_t i = 0; i < hw->pci.count && i < HW64_PCI_MAX; i++) {
                if (hw->pci.devs[i].class_code == 0x03) { vdv = hw->pci.devs[i].vendor; vdd = hw->pci.devs[i].device; break; }
            }
        }
        tm_strlcpy(b, T("PCI display ctrl=", "PCI 显示控制器="), (int)sizeof(b));
        { char nb[16]; tm_utoa64(nb, (uint64_t)vga); tm_stpcat(b, nb); }
        if (vga > 0) {
            const int l0 = tm_stpcat(b, "  first=");
            tm_hex_str(b + l0, ((uint32_t)vdv << 16) | vdd);
        } else {
            tm_stpcat(b, T("  (none enumerated)", "（未枚举到）"));
        }
        tm_field_set(&out[n], T("PCI VGA", "PCI 显卡"), b);
        n++;
    }
    if (n < max) {
        const uint8_t* mac = e1000_mac64();
        tm_strlcpy(b, T("e1000 ", "e1000 "), (int)sizeof(b));
        tm_stpcat(b, net64_state_str64());
        if (mac) {
            tm_stpcat(b, T("  MAC=", "  MAC="));
            static const char* hd = "0123456789abcdef";
            for (int i = 0; i < 6; i++) { char mm[4]; mm[0] = hd[mac[i] >> 4]; mm[1] = hd[mac[i] & 0xF]; mm[2] = (i == 5) ? 0 : ':'; mm[3] = 0; tm_stpcat(b, mm); }
        } else {
            tm_stpcat(b, T("  MAC=none", "  MAC=无"));
        }
        tm_stpcat(b, T(" tx=", " 发送="));
        { char nb[24]; tm_utoa64(nb, net64_tx_frames64()); tm_stpcat(b, nb); }
        tm_stpcat(b, T(" rx=", " 接收="));
        { char nb[24]; tm_utoa64(nb, net64_rx_frames64()); tm_stpcat(b, nb); }
        tm_field_set(&out[n], T("Network", "网络"), b);
        n++;
    }
    if (n < max) {
        tm_strlcpy(b, T("UHCI ", "UHCI "), (int)sizeof(b));
        tm_stpcat(b, usb64_state_str64());
        tm_stpcat(b, T("  ports=", "  端口="));
        { char nb[16]; tm_utoa64(nb, (uint64_t)usb64_ports64()); tm_stpcat(b, nb); }
        tm_stpcat(b, T(" devs=", " 设备="));
        { char nb[16]; tm_utoa64(nb, (uint64_t)usb64_devices64()); tm_stpcat(b, nb); }
        tm_stpcat(b, T(" HID reports=", " HID 报告="));
        { char nb[24]; tm_utoa64(nb, usb64_hid_reports64()); tm_stpcat(b, nb); }
        tm_stpcat(b, T(" key events=", " 按键事件="));
        { char nb[24]; tm_utoa64(nb, usb64_key_events64()); tm_stpcat(b, nb); }
        tm_field_set(&out[n], T("USB host", "USB 主机"), b);
        n++;
    }
    if (n < max) {
        tm_strlcpy(b, T("APIC/SMP ", "APIC/SMP "), (int)sizeof(b));
        tm_stpcat(b, (g_irq_mode64 == IRQ_MODE64_APIC) ? "APIC" : "PIC");
        tm_stpcat(b, T("  ACPI cpus=", "  ACPI CPU 数="));
        { char nb[16]; tm_utoa64(nb, (uint64_t)acpi_cpu_count64()); tm_stpcat(b, nb); }
        tm_stpcat(b, T("  online=", "  在线="));
        { char nb[16]; tm_utoa64(nb, (uint64_t)smp64_online_cpu_count64()); tm_stpcat(b, nb); }
        tm_field_set(&out[n], T("CPU topology", "CPU 拓扑"), b);
        n++;
    }
    if (n < max) {
        tm_field_set(&out[n], T("Note", "说明"),
                     T("no GPU driver: framebuffer metrics only (no 2D/3D acceleration, no VRAM size)",
                       "无 GPU 驱动：只报帧缓冲指标（没有 2D/3D 加速，也不报显存大小）"));
        n++;
    }
    return n;
}

// 详细信息页：架构 / 内核基址 / 映像尺寸 / 页大小 / CR3+PML4 / 内存布局 / 构建特性
static int tm_fields_detail(Window* w, TmField* out, int max) {
    int n = 0;
    char b[64];
    // ★ 用**虚拟**基址算镜像大小（内核已搬高半区：链接在 0xFFFFFFFF80000000）。
    //   __bss_end 是高半区地址，用物理基址 ML64_KERNEL_BASE 会算出天文数字。
    uint64_t image = (uint64_t)(uintptr_t)__bss_end - ML64_KERNEL_VA_BASE;

    if (n < max) {
        tm_field_set(&out[n], T("Architecture", "架构"),
                     T("x86_64 long mode (64-bit only build)", "x86_64 长模式（纯 64 位构建）"));
        n++;
    }
    if (n < max) {
        // 内核已搬高半区：显示**虚拟**基址（物理装载仍是 ML64_KERNEL_BASE=0x100000）
        tm_hex_str(b, ML64_KERNEL_VA_BASE);
        tm_field_set(&out[n], T("Kernel base", "内核基址"), b);
        n++;
    }
    if (n < max) {
        tm_bytes_str(b, image);
        tm_stpcat(b, T("  (base ~ __bss_end)", "（base ~ __bss_end）"));
        tm_field_set(&out[n], T("Image size", "映像尺寸"), b);
        n++;
    }
    if (n < max) {
        tm_hex_str(b, (uint64_t)(uintptr_t)__bss_end);
        tm_field_set(&out[n], T("Image end", "映像末地址"), b);
        n++;
    }
    if (n < max) {
        tm_field_num(&out[n], T("Page size", "页大小"), (uint64_t)PAGE_SIZE_64, " B");
        n++;
    }
    if (n < max) {
        tm_field_set(&out[n], T("Large pages", "大页支持"),
                     T("2 MB (PTE_2MB) and 1 GB (PTE_1GB) mapping APIs available",
                       "支持 2MB（PTE_2MB）与 1GB（PTE_1GB）映射接口"));
        n++;
    }
    if (n < max) {
        tm_hex_str(b, tm_read_cr3());
        tm_field_set(&out[n], T("CR3 (runtime)", "CR3（运行时实测）"), b);
        n++;
    }
    if (n < max) {
        tm_hex_str(b, (uint64_t)ML64_PML4_PHYS);
        tm_stpcat(b, "   PDPT ");
        char h[24];
        tm_hex_str(h, (uint64_t)ML64_PDPT_PHYS);
        tm_stpcat(b, h);
        tm_stpcat(b, "   PD0 ");
        tm_hex_str(h, (uint64_t)ML64_PD0_PHYS);
        tm_stpcat(b, h);
        tm_field_set(&out[n], T("PML4 phys", "PML4 物理地址"), b);
        n++;
    }
    if (n < max) {
        tm_field_bytes(&out[n], T("Identity map", "恒等映射"), (uint64_t)ML64_IDENTITY_BYTES);
        tm_stpcat(out[n].value, T(" (loader, 2MB pages)", "（loader，2MB 页）"));
        n++;
    }
    if (n < max) {
        tm_field_bytes(&out[n], T("RAM top (E820)", "内存上界（E820）"), mem_total_ram_64());
        n++;
    }
    if (n < max) {
        char a2[32], c2[32];
        tm_bytes_str(a2, mem_pool_base_64());
        tm_bytes_str(c2, mem_pool_end_64());
        tm_strlcpy(b, a2, (int)sizeof(b));
        tm_stpcat(b, " ~ ");
        tm_stpcat(b, c2);
        tm_field_set(&out[n], T("Page pool", "物理页池"), b);
        n++;
    }
    if (n < max) {
        tm_bytes_str(b, heap_total_64());
        tm_stpcat(b, T(" fixed kernel heap [80MB,128MB)", " 固定内核堆 [80MB,128MB)"));
        tm_field_set(&out[n], T("Kernel heap", "内核堆"), b);
        n++;
    }
    if (n < max) {
        char nb[24];
        tm_utoa64(nb, page_count_total_64());
        tm_strlcpy(b, nb, (int)sizeof(b));
        tm_stpcat(b, T(" total / ", " 总 / "));
        tm_utoa64(nb, page_count_free_64());
        tm_stpcat(b, nb);
        tm_stpcat(b, T(" free", " 空闲"));
        tm_field_set(&out[n], T("Pages", "物理页"), b);
        n++;
    }
    if (n < max) {
        char nb[24];
        tm_strlcpy(b, "LBA ", (int)sizeof(b));
        tm_utoa64(nb, (uint64_t)ML64_KERNEL_LBA); tm_stpcat(b, nb);
        tm_stpcat(b, " + ");
        tm_utoa64(nb, (uint64_t)ML64_KERNEL_SECTORS); tm_stpcat(b, nb);
        tm_stpcat(b, T(" sectors, image ", " 扇区，映像 "));
        tm_bytes_str(nb, (uint64_t)ML64_IMAGE_BYTES);
        tm_stpcat(b, nb);
        tm_field_set(&out[n], T("Disk image", "磁盘映像"), b);
        n++;
    }
    if (n < max) {
        tm_field_set(&out[n], T("Store area", "持久化保留区"),
                     T("reserved LBA 8009+64; store64 uses VFS /store.a,/store.b when mounted",
                       "已保留 LBA 8009+64；store64 有卷时用 VFS /store.a、/store.b"));
        n++;
    }
    if (n < max) {
        tm_field_set(&out[n], T("Build", "构建特性"),
                     T("freestanding C++17, -mcmodel=kernel, -mno-sse, no STL/libc/float",
                       "freestanding C++17，-mcmodel=kernel，-mno-sse，无 STL/libc/浮点"));
        n++;
    }
    if (n < max) {
        tm_field_set(&out[n], T("Math", "数值口径"),
                     T("integer only: pct = value*100/limit", "全整数：百分比 = 值*100/上限"));
        n++;
    }
    if (n < max) {
        char nb[24];
        tm_utoa64(nb, (uint64_t)fb_width()); tm_strlcpy(b, nb, (int)sizeof(b));
        tm_stpcat(b, "x");
        tm_utoa64(nb, (uint64_t)fb_height()); tm_stpcat(b, nb);
        tm_stpcat(b, T(" render, ", " 渲染，"));
        tm_utoa64(nb, (uint64_t)fb_phys_width()); tm_stpcat(b, nb);
        tm_stpcat(b, "x");
        tm_utoa64(nb, (uint64_t)fb_phys_height()); tm_stpcat(b, nb);
        tm_stpcat(b, T(" physical, zoom ", " 物理，缩放 "));
        tm_utoa64(nb, (uint64_t)fb_get_zoom()); tm_stpcat(b, nb);
        tm_stpcat(b, "%");
        tm_field_set(&out[n], T("Framebuffer", "帧缓冲"), b);
        n++;
    }
    if (n < max) {
        tm_uptime_str(b, (uint64_t)ticks64());
        tm_field_set(&out[n], T("Uptime", "运行时间"), b);
        n++;
    }
    if (n < max) {
        tm_field_num(&out[n], T("IRQ total", "中断总数"), (uint64_t)g_irq_total64, "");
        n++;
    }
    if (n < max) {
        tm_field_num(&out[n], T("Windows", "窗口数"), (uint64_t)gui64_window_count(), "");
        n++;
    }
    if (n < max) {
        tm_field_set(&out[n], T("Instances", "本应用实例"),
                     T("max 4 windows; state in Window::userdata (kmalloc_64)",
                       "最多 4 个窗口；状态挂在 Window::userdata（kmalloc_64）"));
        n++;
    }
    if (n < max) {
        int hh = 0, mm = 0, ss = 0;
        rtc_get_time64(&hh, &mm, &ss);
        tm_pad(b, (uint32_t)hh, 2); tm_stpcat(b, ":");
        char nb[8];
        tm_pad(nb, (uint32_t)mm, 2); tm_stpcat(b, nb); tm_stpcat(b, ":");
        tm_pad(nb, (uint32_t)ss, 2); tm_stpcat(b, nb);
        tm_field_set(&out[n], T("RTC time", "RTC 时间"), b);
        n++;
    }
    (void)w;
    return n;
}

// ---------------- 绘制小件 ----------------
static void tm_draw_button(Window* w, int x, int y, int bw, int bh, const char* label,
                           bool hover, bool enabled) {
    int ax = w->client_x + x, ay = w->client_y + y;
    uint32_t bg = enabled ? (hover ? TM_COL_BTN_HOVER : TM_COL_BTN) : 0xFFF0F0F0;
    fb_fill_rect(ax, ay, bw, bh, bg);
    fb_draw_rect(ax, ay, bw, bh, enabled ? 0xFFADADAD : TM_COL_BORDER);
    uint32_t fg = enabled ? TM_COL_TEXT : 0xFFA0A0A0;
    int tw = tm_text_w(label);
    tm_text_clip(ax + (bw - tw) / 2, ay + (bh - font_line_height()) / 2, label, fg, bw - 8);
}

// 固定状态开关（本内核没有 store，启动页的开关不提供切换 -> 画成置灰的 "on" 并标 -
static void tm_draw_switch(int ax, int ay, bool on, bool fixed) {
    int sw = 46, sh = 20;
    uint32_t col = fixed ? TM_COL_OFF : (on ? TM_COL_ACCENT : TM_COL_OFF);
    fb_fill_rect(ax, ay, sw, sh, col);
    int kx = on ? (ax + sw - sh + 3) : (ax + 3);
    fb_fill_rect(kx, ay + 3, sh - 6, sh - 6, 0xFFFFFFFF);
    if (fixed) fb_draw_rect(ax, ay, sw, sh, 0xFF9A9A9A);
}

// 行首色块图标（按行号取色，视觉区分用；不是数据）
static uint32_t tm_icon_color(int idx) {
    static const uint32_t cols[6] = {
        0xFF307BC0, 0xFFC07B30, 0xFF30A070, 0xFF9050C0, 0xFFC04040, 0xFF505050
    };
    int i = idx % 6;
    if (i < 0) i += 6;
    return cols[i];
}

// CPU 热力单元格底色
static uint32_t tm_cpu_heat(int cpu) {
    if (cpu <= 0) return 0xFFE8E8E8;
    if (cpu < 10) return 0xFFFFF3C4;
    if (cpu < 40) return 0xFFFFD9A0;
    return 0xFFFFB0A0;
}

// 分组标题前的小三角（展开态向下）
static void tm_draw_triangle(int ax, int ay, uint32_t col) {
    for (int i = 0; i < 4; i++) fb_fill_rect(ax + i, ay + i, 7 - 2 * i, 1, col);
}

// ---------------- 菜单行 + 下拉菜单 ----------------
static void tm_draw_menubar(Window* w, TmState* st) {
    int X = w->client_x, Y = w->client_y;
    int cw = w->client_w;
    fb_fill_rect(X, Y, cw, TM_MENU_H, TM_COL_MENUBG);
    fb_draw_hline(X, Y + TM_MENU_H, cw, TM_COL_BORDER);
    int mc_x = 0, mc_y = 0;
    tm_mouse_client(w, &mc_x, &mc_y);
    for (int i = 0; i < 4; i++) {
        int bx = tm_menubtn_x(i), bw = tm_menubtn_w(i);
        if (bx >= cw) break;
        bool hover = (mc_y >= 0 && mc_y < TM_MENU_H && mc_x >= bx && mc_x < bx + bw);
        bool open = (st->menu_open && i == 0);
        if (hover || open) fb_fill_rect(X + bx, Y, bw, TM_MENU_H, 0xFFE5F1FB);
        tm_text_clip(X + bx + 13, Y + (TM_MENU_H - font_line_height()) / 2,
                     tm_menu_label(i), TM_COL_TEXT, bw - 16);
    }
}

// 下拉菜单：最后画，盖住本窗口全部内容
static void tm_draw_dropdown(Window* w, TmState* st) {
    int X = w->client_x, Y = w->client_y;
    int dw = tm_dropdown_w();
    int dx = tm_menubtn_x(0);
    int dy = TM_MENU_H;
    int dh = TM_DD_PAD * 2 + TM_MENU_ITEMS * TM_DD_ITEM_H;
    fb_fill_rect(X + dx + 3, Y + dy + 3, dw, dh, 0xFFC8C8C8);       // 阴影
    fb_fill_rect(X + dx, Y + dy, dw, dh, TM_COL_CLIENT);
    fb_draw_rect(X + dx, Y + dy, dw, dh, 0xFFA0A0A0);
    int mx = 0, my = 0;
    tm_mouse_client(w, &mx, &my);
    for (int i = 0; i < TM_MENU_ITEMS; i++) {
        int iy = dy + TM_DD_PAD + i * TM_DD_ITEM_H;
        bool hover = (mx >= dx && mx < dx + dw && my >= iy && my < iy + TM_DD_ITEM_H);
        if (hover) fb_fill_rect(X + dx + 1, Y + iy, dw - 2, TM_DD_ITEM_H, 0xFFE5F1FB);
        else if (i == (int)st->menu_sel) fb_fill_rect(X + dx + 1, Y + iy, dw - 2, TM_DD_ITEM_H, 0xFFF0F7FD);
        tm_text_clip(X + dx + 14, Y + iy + (TM_DD_ITEM_H - font_line_height()) / 2,
                     tm_menu_item(i), TM_COL_TEXT, dw - 20);
    }
    {
        int sy = dy + TM_DD_PAD + (TM_MENU_ITEMS - 1) * TM_DD_ITEM_H;
        fb_draw_hline(X + dx + 8, Y + sy, dw - 16, TM_COL_BORDER);
    }
}

// ---------------- 标签行 ----------------
static void tm_draw_tabs(Window* w, TmState* st) {
    int X = w->client_x, Y = w->client_y;
    int cw = w->client_w;
    if (TM_TAB_Y + TM_TAB_H > tm_status_y(w)) return;      // 窗口太矮：不画标签行
    fb_fill_rect(X, Y + TM_TAB_Y, cw, TM_TAB_H, TM_COL_CLIENT);
    int mx = 0, my = 0;
    tm_mouse_client(w, &mx, &my);
    for (int i = 0; i < TM_PAGES; i++) {
        int bx = tm_tab_x(i), bw = tm_tab_box_w(i);
        if (bx >= cw) break;
        int tw = tm_text_w(tm_tab_label(i));
        int tx = bx + (bw - tw) / 2;
        int ty = Y + TM_TAB_Y + (TM_TAB_H - font_line_height()) / 2;
        bool hover = (my >= TM_TAB_Y && my < TM_TAB_Y + TM_TAB_H && mx >= bx && mx < bx + bw);
        uint32_t fg = (i == (int)st->page) ? TM_COL_TEXT : TM_COL_TEXT_DIM;
        if (hover && i != (int)st->page) fb_fill_rect(X + bx, Y + TM_TAB_Y, bw, TM_TAB_H, TM_COL_HOVER);
        tm_text_clip(X + tx, ty, tm_tab_label(i), fg, cw - tx - 2);
        if (i == (int)st->page) fb_fill_rect(X + tx, Y + TM_TAB_Y + TM_TAB_H - 4, tw, 3, TM_COL_ACCENT);
    }
    fb_draw_hline(X, Y + TM_TAB_Y + TM_TAB_H - 1, cw, TM_COL_BORDER);
}

// ---------------- 状态栏 ----------------
static void tm_draw_status(Window* w, TmState* st) {
    int X = w->client_x, Y = w->client_y;
    int cw = w->client_w;
    int sy = tm_status_y(w);
    if (sy >= w->client_h) return;
    fb_fill_rect(X, Y + sy, cw, TM_STATUS_H, TM_COL_STATUSBG);
    fb_draw_hline(X, Y + sy, cw, TM_COL_BORDER);
    int ty = Y + sy + (TM_STATUS_H - font_line_height()) / 2;

    char buf[128];
    char nb[16];
    tm_stpcpy(buf, T("Windows: ", "窗口数: "));
    tm_itoa(nb, gui64_window_count());
    tm_stpcat(buf, nb);
    tm_stpcat(buf, "    CPU: ");
    tm_pct_str(nb, (int)st->cpu_now);
    tm_stpcat(buf, nb);
    tm_stpcat(buf, "    ");
    tm_stpcat(buf, T("Memory(pages): ", "内存(页池): "));
    tm_pct_str(nb, (int)st->mem_now);
    tm_stpcat(buf, nb);
    tm_text_clip(X + 12, ty, buf, TM_COL_TEXT, cw - 24);

    char sbuf[96];
    tm_stpcpy(sbuf, "FPS: ");
    tm_utoa64(nb, (uint64_t)gui64_fps());
    tm_stpcat(sbuf, nb);
    tm_stpcat(sbuf, "   ");
    tm_stpcat(sbuf, T("Uptime: ", "运行: "));
    tm_uptime_str(nb, (uint64_t)ticks64());
    tm_stpcat(sbuf, nb);
    int sw = tm_text_w(sbuf);
    if (tm_text_w(buf) + 24 + sw < cw) tm_text(X + cw - 12 - sw, ty, sbuf, TM_COL_TEXT_DIM);
}

// 提示行（拒绝访问 / 未实现等）：画在底部按钮左侧，持续到下次操作
static void tm_draw_notice(Window* w, TmState* st) {
    if (!st || st->notice_len <= 0) return;
    int X = w->client_x, Y = w->client_y;
    int by = tm_btn_y(w);
    int maxw = tm_btn_x(w) - 12 - 16;
    if (maxw < 40) return;
    tm_text_clip(X + 16, Y + by + (TM_BTN_H - font_line_height()) / 2, st->notice,
                 st->notice_warn ? TM_COL_WARN : TM_COL_TEXT_DIM, maxw);
}

static void tm_draw_main_button(Window* w, const char* label, bool enabled) {
    int bx = tm_btn_x(w), by = tm_btn_y(w);
    int mx = 0, my = 0;
    tm_mouse_client(w, &mx, &my);
    bool hover = (mx >= bx && mx < bx + TM_BTN_W && my >= by && my < by + TM_BTN_H);
    tm_draw_button(w, bx, by, TM_BTN_W, TM_BTN_H, label, hover, enabled);
}

// ---------------- 进程页 ----------------
static void tm_draw_row(Window* w, const TmRow* r, int row_i, int row_y, bool selected, bool hover) {
    int X = w->client_x, Y = w->client_y;
    int cw = w->client_w;
    if (selected) fb_fill_rect(X, Y + row_y, cw, TM_ROW_H, TM_COL_SEL);
    else if (hover) fb_fill_rect(X, Y + row_y, cw, TM_ROW_H, TM_COL_HOVER);

    int id_right = tm_col_id_right(w);
    int state_x = tm_col_state_x(w);
    int ticks_right = tm_col_ticks_right(w);
    int sw_right = tm_col_sw_right(w);
    int cpu_x = tm_col_cpu_x(w);
    int cpu_w = tm_col_cpu_w(w);

    // 图标色块（按行号取色，纯视觉，不是数据）
    fb_fill_rect(X + 20, Y + row_y + 3, 16, 16, tm_icon_color(row_i));
    fb_draw_rect(X + 20, Y + row_y + 3, 16, 16, 0xFF606060);

    int ty = Y + row_y + (TM_ROW_H - font_line_height()) / 2;
    char nb[24];

    // 进程名（"*" = 当前进程，提示行有说明）
    char name[PROC64_NAME_MAX + 2];              // 名字 + "*" 前缀 + 结尾
    tm_stpcpy(name, r->is_current ? "*" : "");
    tm_stpcat(name, r->name);
    tm_text_clip(X + tm_col_name_x(), ty, name, TM_COL_TEXT, id_right - tm_col_name_x() - 6);

    // pid（右对齐）
    tm_utoa64(nb, (uint64_t)r->id);
    tm_text_right(X + id_right, ty, nb, TM_COL_TEXT_DIM);

    // 状态 + ppid（值来自 Proc64Info；状态文案一一对应 Proc64State）
    {
        char sb[48];
        tm_stpcpy(sb, tm_proc_state_text(r->state));
        tm_stpcat(sb, " ppid=");
        tm_utoa64(nb, (uint64_t)r->ppid);
        tm_stpcat(sb, nb);
        tm_text_clip(X + state_x, ty, sb, TM_COL_TEXT_DIM, tm_col_ticks_x(w) - state_x - 6);
    }

    // CR3（右对齐；进程页表根，hex —— `proc list` 里同一列）
    {
        char hb[24];
        tm_hex_str(hb, r->cr3);
        tm_text_right(X + ticks_right, ty, hb, TM_COL_TEXT);
    }

    // 线程数（右对齐；= 该进程在任务表里的任务数，4ms/tick 的调度器真值）
    tm_utoa64(nb, (uint64_t)r->threads);
    tm_text_right(X + sw_right, ty, nb, TM_COL_TEXT);

    // CPU%：热力单元格；口径 = 该进程全部任务的 CPU 时间 / 系统 tick（1 秒采样窗口），见提示行
    fb_fill_rect(X + cpu_x, Y + row_y + 2, cpu_w, TM_ROW_H - 4,
                 tm_cpu_heat((int)(r->cpu_permille / 10u)));
    tm_pct1_str(nb, r->cpu_permille);
    int tx = cpu_x + (cpu_w - tm_text_w(nb)) / 2;
    if (tx < cpu_x) tx = cpu_x;
    tm_text_clip(X + tx, ty, nb, TM_COL_TEXT, cpu_w);
}

// 进程页：确保选中行可见
static void tm_rows_scroll_fix(Window* w, TmState* st, int n) {
    int vis = (tm_list_bot(w) - tm_rows_top()) / TM_ROW_H;
    if (vis < 1) vis = 1;
    if (st->sel_row >= 0) {
        if (st->sel_row < st->scroll) st->scroll = st->sel_row;
        else if (st->sel_row >= st->scroll + vis) st->scroll = st->sel_row - vis + 1;
    }
    int maxs = n - vis;
    if (maxs < 0) maxs = 0;
    if (st->scroll > maxs) st->scroll = maxs;
    if (st->scroll < 0) st->scroll = 0;
}

static void tm_draw_proc(Window* w, TmState* st) {
    int X = w->client_x, Y = w->client_y;
    int cw = w->client_w;
    int n = tm_build_rows(TM_MAX_ROWS);          // 行数据源 = proc64 进程表（每进程独立 CR3）
    int total = proc64_count64();                // 真实进程数（proc64 自己的计数）
    if (n > 0 && st->sel_row >= n) st->sel_row = n - 1;
    if (n == 0) st->sel_row = -1;
    tm_rows_scroll_fix(w, st, n);

    // 新增打点（自动验收 grep）：[UI] tmgr proc rows=N + 每行内容；2 秒节流，行数变化立即补一条。
    {
        const uint32_t now = ticks64();
        if (st->proc_rows_logged != n ||
            (uint32_t)(now - st->proc_log_tick) >= ms_to_ticks64(TM_LOG_MS)) {
            st->proc_rows_logged = n;
            st->proc_log_tick = now;
            dbg64_line_begin64();
            dbg64_str("[UI] tmgr proc rows=");
            dbg64_dec((uint64_t)n);
            dbg64_str(" total=");
            dbg64_dec((uint64_t)total);
            dbg64_nl();
            dbg64_line_end64();
            for (int i = 0; i < n; i++) {
                const TmRow* r = &g_rows[i];
                dbg64_line_begin64();
                dbg64_str("[UI] tmgr proc row pid=");
                dbg64_dec((uint64_t)r->id);
                dbg64_str(" ppid=");
                dbg64_dec((uint64_t)r->ppid);
                dbg64_str(" name=");
                dbg64_str(r->name);
                dbg64_str(" state=");
                dbg64_dec((uint64_t)r->state);
                dbg64_str(" cr3=0x");
                dbg64_hex64(r->cr3);
                dbg64_str(" threads=");
                dbg64_dec((uint64_t)r->threads);
                dbg64_str(" cpu_permille=");
                dbg64_dec((uint64_t)r->cpu_permille);
                dbg64_str(" pages=");
                dbg64_dec(r->pages);
                dbg64_nl();
                dbg64_line_end64();
            }
        }
    }

    // 列头（列几何与行共用 tm_col_*，窗口缩放时两边一起变）
    int id_right = tm_col_id_right(w);
    int state_x = tm_col_state_x(w);
    int ticks_x = tm_col_ticks_x(w);
    int ticks_right = tm_col_ticks_right(w);
    int sw_right = tm_col_sw_right(w);
    int cpu_x = tm_col_cpu_x(w);
    int cpu_w = tm_col_cpu_w(w);
    if (TM_CONTENT_Y + TM_COL_HDR_H <= tm_status_y(w)) {
        fb_fill_rect(X, Y + TM_CONTENT_Y, cw, TM_COL_HDR_H, TM_COL_HDRBG);
        int hy = Y + TM_CONTENT_Y + (TM_COL_HDR_H - font_line_height()) / 2;
        tm_text_clip(X + tm_col_name_x(), hy, T("Process", "进程"), TM_COL_TEXT_DIM,
                     id_right - tm_col_name_x() - 6);
        tm_text_right(X + id_right, hy, "PID", TM_COL_TEXT_DIM);
        tm_text_clip(X + state_x, hy, T("Status / PPID", "状态/父进程"), TM_COL_TEXT_DIM,
                     ticks_x - state_x - 6);
        tm_text_right(X + ticks_right, hy, T("CR3", "CR3"), TM_COL_TEXT_DIM);
        tm_text_right(X + sw_right, hy, T("Thr", "线程"), TM_COL_TEXT_DIM);
        {
            const char* hl = "CPU%";
            int tw = tm_text_w(hl);
            int tx = cpu_x + (cpu_w - tw) / 2;
            if (tx < cpu_x) tx = cpu_x;
            tm_text_clip(X + tx, hy, hl, TM_COL_TEXT_DIM, cpu_w);
        }
        fb_draw_hline(X, Y + TM_CONTENT_Y + TM_COL_HDR_H, cw, TM_COL_BORDER);
    }

    int mcx = 0, mcy = 0;
    tm_mouse_client(w, &mcx, &mcy);
    int top = tm_list_top(), bot = tm_list_bot(w);
    // 底部提示带（数据来源 + CPU/CR3 口径）：窗口够高就预留，保证口径写在进程页上
    const int note_h = 34;
    int rows_bot = bot;
    if (bot - top >= TM_GRP_HDR_H + 3 * TM_ROW_H + note_h) rows_bot = bot - note_h;

    // 分组标题（行 = proc64 真进程；数量是 proc64_count64() 真值）
    int y = top;
    if (y + TM_GRP_HDR_H <= rows_bot) {
        char hdr[48];
        tm_count_label(hdr, T("Processes", "进程"), total);
        fb_fill_rect(X + 4, Y + y, cw - 8, TM_GRP_HDR_H, 0xFFFAFAFA);
        tm_draw_triangle(X + 14, Y + y + 10, 0xFF505050);
        tm_text_clip(X + 28, Y + y + (TM_GRP_HDR_H - font_line_height()) / 2, hdr, TM_COL_TEXT, cw - 40);
        y += TM_GRP_HDR_H;
    }

    if (n == 0) {
        // 进程表为空：如实写一行"无进程数据"，绝不造假行
        if (y + TM_ROW_H <= rows_bot) {
            tm_text_clip(X + tm_col_name_x(), Y + y + (TM_ROW_H - font_line_height()) / 2,
                         T("No process data (terminal: 'proc run spin')",
                           "无进程数据（终端可敲 proc run spin）"), TM_COL_TEXT_DIM,
                         cw - tm_col_name_x() - 12);
            y += TM_ROW_H;
        }
    } else {
        int hover_i = -1;
        for (int i = st->scroll; i < n; i++) {
            if (y + TM_ROW_H > rows_bot) break;
            if (mcy >= y && mcy < y + TM_ROW_H && mcx >= 4 && mcx < cw - 4) hover_i = i;
            tm_draw_row(w, &g_rows[i], i, y, i == st->sel_row, i == hover_i);
            y += TM_ROW_H;
        }
    }

    // 提示行：数据来源与口径（如实写，和列头一起构成列的口径说明）
    if (rows_bot < bot) {
        int ly = rows_bot + 4;
        tm_text_clip(X + 16, Y + ly,
                     T("Source: proc64 process table (per-process CR3); kernel tasks are in terminal 'ps'",
                       "来源：proc64 进程表（每进程独立 CR3）；内核任务表见终端 ps"),
                     TM_COL_TEXT_DIM, cw - 32);
        tm_text_clip(X + 16, Y + ly + 16,
                     T("CPU% = process task ticks / system ticks (1s window); Enter = kill(SIGKILL); * = current",
                       "CPU% = 该进程任务时间/系统 tick（1 秒窗口）；回车 = kill(SIGKILL)；* = 当前进程"),
                     TM_COL_TEXT_DIM, cw - 32);
    }

    tm_draw_main_button(w, T("End process", "结束进程"), st->sel_row >= 0 && st->sel_row < n);
}


// ---------------- 性能页 ----------------
// 曲线：有计数器才画数据；没有计数器（磁盘）只画网格并如实写"无计数器"。
static void tm_draw_graph(Window* w, int gx, int gy, int gw, int gh,
                          const uint32_t* curve, int n, int cur, const char* caption,
                          bool has_counter) {
    int ax = w->client_x + gx, ay = w->client_y + gy;
    if (gw < 20 || gh < 20) return;
    fb_fill_rect(ax, ay, gw, gh, TM_COL_CLIENT);
    for (int i = 1; i < 4; i++) {
        fb_draw_hline(ax, ay + gh * i / 4, gw, TM_COL_GRID);
        fb_draw_vline(ax + gw * i / 4, ay, gh, TM_COL_GRID);
    }
    fb_draw_rect(ax, ay, gw, gh, 0xFFC0C0C0);

    if (has_counter && curve && n > 0) {
        int step = (gw - 2) / (TM_CURVE_N - 1);
        if (step < 1) step = 1;
        int base = ay + gh - 1;
        int hmax = gh - 2;
        if (hmax < 2) hmax = 2;
        if (n > TM_CURVE_N) n = TM_CURVE_N;
        if (n >= 2) {
            // 曲线下方半透明填充
            for (int i = 0; i + 1 < n; i++) {
                int v0 = (int)curve[i], v1 = (int)curve[i + 1];
                int x0 = ax + 1 + i * step;
                int y0 = base - (v0 * hmax) / 100;
                int y1 = base - (v1 * hmax) / 100;
                int ytop = (y0 < y1) ? y0 : y1;
                if (base - ytop > 0) fb_fill_rect_alpha(x0, ytop, step, base - ytop, TM_COL_CURVEFILL, 80);
            }
            // 2px 折线：逐列描点（分段直线不留断点）
            for (int i = 0; i + 1 < n; i++) {
                int v0 = (int)curve[i], v1 = (int)curve[i + 1];
                int x0 = ax + 1 + i * step;
                int y0 = base - (v0 * hmax) / 100;
                int y1 = base - (v1 * hmax) / 100;
                for (int k = 0; k < step; k++) {
                    int yy = y0 + (y1 - y0) * k / step;
                    fb_fill_rect(x0 + k, yy, 1, 2, TM_COL_CURVE);
                }
            }
            int xe = ax + 1 + (n - 1) * step;
            int ye = base - ((int)curve[n - 1] * hmax) / 100;
            fb_fill_rect(xe - 1, ye, 3, 2, TM_COL_CURVE);
        }
    }
    // 左上角曲线名，右上角当前值 / 无计数器
    tm_text_clip(ax + 6, ay + 3, caption, TM_COL_TEXT_DIM, gw - 80);
    if (has_counter && cur >= 0) {
        char vb[16];
        tm_pct_str(vb, cur);
        tm_text(ax + gw - tm_text_w(vb) - 6, ay + 3, vb, TM_COL_TEXT_DIM);
    } else {
        const char* s = T("no counter", "无计数器");
        tm_text(ax + gw - tm_text_w(s) - 6, ay + 3, s, TM_COL_TEXT_DIM);
    }
}

struct TmPerfLayout {
    int  top, bot;      // 内容区上下沿（客户区局部坐标）
    int  lh;            // 字段面板行距
    int  item_h;        // 左列表行高
    int  list_w;        // 左列表宽度（0 = 太窄，不画列表）
    int  gx, gw;        // 曲线区 x / 宽度
    int  gh;            // 曲线高度（0 = 不画）
    int  det_top;       // 字段面板首行 y
    int  det_rows;      // 字段面板每列行数
    int  two_col;       // 是否两列排布
};

static void tm_perf_layout(Window* w, int rows_want, TmPerfLayout* L) {
    int top = TM_CONTENT_Y;
    int bot = tm_status_y(w);
    int below = bot - (top + 4);
    if (below < 40) below = 40;

    int item_h = (below < 320) ? 20 : 26;
    int list_h = item_h * TM_PERF_ITEMS + 4;

    int lh = font_line_height() + 2;
    if (lh > 20) lh = 20;
    if (lh < 14) lh = 14;

    int cw = w->client_w;
    int list_w = 0, gx = 8, gw = cw - 20;
    if (cw >= 520) {
        list_w = 120;
        if (list_w > cw / 3) list_w = cw / 3;
        gx = 8 + list_w + 12;
        gw = cw - gx - 12;
        if (gw < 80) { list_w = 0; gx = 8; gw = cw - 20; }
    }
    if (gw < 40) gw = 40;

    int two = (cw >= 700 && rows_want > 10) ? 1 : 0;
    int per_col = two ? (rows_want + 1) / 2 : rows_want;
    if (per_col > TM_MAX_FIELDS) per_col = TM_MAX_FIELDS;

    // 字段面板优先：先扣掉它的高度，再决定曲线能有多高
    int det_rows = per_col;
    int room = below - det_rows * lh - TM_PAD_TEXT;
    while (det_rows > 0 && room < list_h) {
        det_rows--;
        room = below - det_rows * lh - TM_PAD_TEXT;
    }
    int gh = 0;
    if (room >= list_h) {
        gh = room;
        if (gh > TM_GH_MAX) gh = TM_GH_MAX;
    }

    int top_block = (list_h > gh) ? list_h : gh;
    L->top = top;
    L->bot = bot;
    L->lh = lh;
    L->item_h = item_h;
    L->list_w = list_w;
    L->gx = gx;
    L->gw = gw;
    L->gh = gh;
    L->two_col = two;
    L->det_rows = det_rows;
    L->det_top = top + 4 + top_block + 6;
    if (L->det_top > bot) L->det_top = bot;
}

// 字段面板绘制（label 暗色 + value 常规；宽窗口下两列排布）
static void tm_draw_fields(Window* w, const TmField* f, int n, int top, int bot,
                           int det_rows, int lh, bool two_col) {
    int X = w->client_x, Y = w->client_y;
    int cw = w->client_w;
    if (n <= 0 || bot - top < lh) return;
    int cols = two_col ? 2 : 1;
    int colw = (cw - 16) / cols;
    int labw = 150;
    if (labw > colw * 60 / 100) labw = colw * 60 / 100;
    if (labw < 40) labw = 40;
    int limit = det_rows * cols;
    for (int i = 0; i < n && i < limit; i++) {
        int col = 0, row = i;
        if (two_col && i >= det_rows) { col = 1; row = i - det_rows; }
        int ry = top + row * lh;
        if (ry + lh > bot) break;
        int rx = X + 8 + col * colw;
        tm_text_clip(rx, Y + ry, f[i].label, TM_COL_TEXT_DIM, labw - 6);
        tm_text_clip(rx + labw, Y + ry, f[i].value, TM_COL_TEXT, colw - labw - 8);
    }
}

// 批次 B：性能页的硬件详情打点（自动验收 grep： [UI] tmgr perf gpu=... hw=...）
// 只在"选中项变化"时打一行（每次 draw 都打会刷屏；选择变化由键盘/点击驱动）。
static void tm_log_perf_hw64(TmState* st, int sel) {
    (void)st;
    const HwInfo64* hw = hw_info64();
    const Disp64Info* di = display64_info64();
    const uint8_t* mac = e1000_mac64();
    dbg64_line_begin64();
    dbg64_str("[UI] tmgr perf sel=");
    dbg64_str(sel == 0 ? "cpu" : (sel == 1 ? "memory" : (sel == 2 ? "disk" : "gpu")));
    dbg64_str(" gpu=framebuffer ");
    dbg64_dec((uint64_t)fb_phys_width()); dbg64_str("x"); dbg64_dec((uint64_t)fb_phys_height());
    dbg64_str("@32bpp zoom="); dbg64_dec((uint64_t)fb_get_zoom()); dbg64_str("% refresh=");
    if (di->refresh_x10 > 0) {
        char rb[16];
        display64_refresh_str64(rb, (int)sizeof(rb));
        dbg64_str(rb);
    } else {
        dbg64_str("unknown");
    }
    dbg64_str(" src="); dbg64_str(display64_src_name64(di->src));
    dbg64_str(" edid_match="); dbg64_dec((uint64_t)di->edid_match);
    dbg64_str(" hw=cpu=");
    dbg64_str((hw->magic == HW64_INFO_MAGIC && hw->cpu.vendor[0]) ? hw->cpu.vendor : "unknown");
    dbg64_str(" cores=");
    dbg64_dec((uint64_t)(hw->magic == HW64_INFO_MAGIC ? (hw->cpu.cores ? hw->cpu.cores : 1) : 1));
    dbg64_str(" hyp=");
    dbg64_str((hw->magic == HW64_INFO_MAGIC && hw->cpu.hypervisor[0]) ? hw->cpu.hypervisor : "none");
    dbg64_str(" ram_mb=");
    dbg64_dec(mem_total_ram_64() / (1024ull * 1024ull));
    dbg64_str(" page_pool_kb=");
    {
        uint64_t pt = 0, pf = 0, hk = 0;
        mem_info_64(&pt, &pf, &hk);
        dbg64_dec(pt);
        dbg64_str(" page_free_kb=");
        dbg64_dec(pf);
    }
    dbg64_str(" disk=");
    {
        const char* dm = "none";
        uint64_t dsec = 0;
        if (hw->magic == HW64_INFO_MAGIC) {
            for (uint32_t i = 0; i < HW64_DISK_MAX; i++) {
                if (!hw->disks[i].present) continue;
                dm = hw->disks[i].model[0] ? hw->disks[i].model : "(no model)";
                dsec = hw->disks[i].sectors_512;
                break;
            }
        }
        dbg64_str(dm);
        dbg64_str(" disk_mb=");
        dbg64_dec(dsec / 2048u);
    }
    dbg64_str(" net=");
    dbg64_str(net64_state_str64());
    if (mac) {
        static const char* const HD = "0123456789abcdef";
        dbg64_str(" mac=");
        for (int i = 0; i < 6; i++) {
            char mm[4];
            mm[0] = HD[mac[i] >> 4]; mm[1] = HD[mac[i] & 0xF];
            mm[2] = (i == 5) ? 0 : ':'; mm[3] = 0;
            dbg64_str(mm);
        }
    }
    dbg64_str(" tx="); dbg64_dec(net64_tx_frames64());
    dbg64_str(" rx="); dbg64_dec(net64_rx_frames64());
    dbg64_str(" usb="); dbg64_str(usb64_state_str64());
    dbg64_str(" usb_ports="); dbg64_dec((uint64_t)usb64_ports64());
    dbg64_str(" usb_devs="); dbg64_dec((uint64_t)usb64_devices64());
    dbg64_str(" apic="); dbg64_str((g_irq_mode64 == IRQ_MODE64_APIC) ? "APIC" : "PIC");
    dbg64_str(" acpi_cpus="); dbg64_dec((uint64_t)acpi_cpu_count64());
    dbg64_str(" smp_online="); dbg64_dec((uint64_t)smp64_online_cpu_count64());
    dbg64_str(" page_size="); dbg64_dec((uint64_t)PAGE_SIZE_64);
    dbg64_nl();
    dbg64_line_end64();
}

static void tm_draw_perf(Window* w, TmState* st) {
    int X = w->client_x, Y = w->client_y;
    int cw = w->client_w;

    int sel = (int)st->perf_sel;
    if (sel < 0 || sel >= TM_PERF_ITEMS) sel = 0;
    if (st->perf_logged_sel != (uint8_t)sel) {   // 批次 B：选中项变化 -> 打一行硬件详情（验收 grep）
        st->perf_logged_sel = (uint8_t)sel;
        tm_log_perf_hw64(st, sel);
    }
    int nf = 0;
    if (sel == 0)      nf = tm_fields_cpu(st, g_fields, TM_MAX_FIELDS);
    else if (sel == 1) nf = tm_fields_mem(g_fields, TM_MAX_FIELDS);
    else if (sel == 2) nf = tm_fields_disk(g_fields, TM_MAX_FIELDS);
    else               nf = tm_fields_gpu(g_fields, TM_MAX_FIELDS);

    TmPerfLayout L;
    tm_perf_layout(w, nf, &L);
    if (L.bot - L.top < 20) return;

    // 1) 左侧列表（CPU / 内存 / 磁盘 / 显卡）：选中项高亮 + 左侧强调条
    static const char* const label_en[TM_PERF_ITEMS] = { "CPU", "Memory", "Disk", "GPU" };
    static const char* const label_zh[TM_PERF_ITEMS] = { "CPU", "内存", "磁盘", "显卡" };
    if (L.list_w > 0) {
        int lw = L.list_w;
        if (8 + lw > L.gx - 8) lw = L.gx - 16;
        for (int i = 0; i < TM_PERF_ITEMS; i++) {
            int iy = L.top + 4 + i * L.item_h;
            if (iy + L.item_h > L.bot) break;
            bool s = (sel == i);
            fb_fill_rect(X + 8, Y + iy, lw, L.item_h, s ? 0xFFE5F1FB : TM_COL_CLIENT);
            if (s) fb_fill_rect(X + 8, Y + iy, 3, L.item_h, TM_COL_ACCENT);
            tm_text_clip(X + 22, Y + iy + (L.item_h - font_line_height()) / 2,
                         gui64_lang_zh() ? label_zh[i] : label_en[i], TM_COL_TEXT, lw - 16);
        }
    }

    // 2) 曲线（磁盘/显卡没有使用率计数器 -> 只画框并写"无计数器"，绝不画假曲线）
    if (L.gh >= TM_GH_MIN) {                       // 布局保证 gh >= 列表高，否则曲线不画
        const uint32_t* curve = nullptr;
        int cn = 0, cur = -1;
        bool has = false;
        if (sel == 0)      { curve = st->curve;     cn = st->curve_n; cur = (int)st->cpu_now; has = true; }
        else if (sel == 1) { curve = st->curve_mem; cn = st->curve_n; cur = (int)st->mem_now; has = true; }
        const char* cap = gui64_lang_zh() ? label_zh[sel] : label_en[sel];
        tm_draw_graph(w, L.gx, L.top + 4, L.gw, L.gh, curve, cn, cur, cap, has);
    }

    // 3) 字段面板（分隔线 + 字段名/实测值；两列排布时按列优先填充）
    if (L.det_rows > 0 && nf > 0) {
        fb_draw_hline(X + 8, Y + L.det_top - 4, cw - 16, TM_COL_GRID);
        tm_draw_fields(w, g_fields, nf, L.det_top, L.bot, L.det_rows, L.lh, L.two_col != 0);
    }
}

// ---------------- 启动页 ----------------
static const char* tm_startup_key(int i) {
    switch (i) {
        case 0: return "monitor";
        case 1: return "desktop";
        default: return "health";
    }
}

static const char* tm_startup_name(int i) {
    switch (i) {
        case 0: return T("System Monitor", "系统监视器");
        case 1: return T("Desktop Shell", "桌面外壳");
        default: return T("Health Check", "健康检查");
    }
}

static void tm_draw_startup(Window* w, TmState* st) {
    int X = w->client_x, Y = w->client_y;
    int cw = w->client_w;
    int top = TM_CONTENT_Y;
    int name_x = 16;
    int pub_x = (cw * 40) / 100;
    if (pub_x < 150) pub_x = 150;
    int sw_x = cw - 110;
    if (sw_x < 120) sw_x = 120;
    int sts_x = sw_x - 130;
    int lo = pub_x + 80;
    if (sts_x < lo) sts_x = lo;
    int rowh = 30;
    int bot = tm_list_bot(w);

    if (top + TM_COL_HDR_H <= tm_status_y(w)) {
        fb_fill_rect(X, Y + top, cw, TM_COL_HDR_H, TM_COL_HDRBG);
        int hy = Y + top + (TM_COL_HDR_H - font_line_height()) / 2;
        tm_text_clip(X + name_x, hy, T("Name", "名称"), TM_COL_TEXT_DIM, pub_x - name_x - 6);
        tm_text_clip(X + pub_x, hy, T("Publisher", "发布者"), TM_COL_TEXT_DIM, sts_x - pub_x - 6);
        tm_text_clip(X + sts_x, hy, T("Status", "状态"), TM_COL_TEXT_DIM, sw_x - sts_x - 6);
        fb_draw_hline(X, Y + top + TM_COL_HDR_H, cw, TM_COL_BORDER);
    }

    int mcx = 0, mcy = 0;
    tm_mouse_client(w, &mcx, &mcy);
    int last_bot = top + TM_COL_HDR_H + 2;
    for (int i = 0; i < 3; i++) {
        int ry = top + TM_COL_HDR_H + 2 + i * rowh;
        if (ry + rowh > bot) break;
        last_bot = ry + rowh;
        bool s = (i == st->sel_row);
        bool hover = (mcy >= ry && mcy < ry + rowh && mcx >= 4 && mcx < cw - 4);
        if (s) fb_fill_rect(X, Y + ry, cw, rowh, TM_COL_SEL);
        else if (hover) fb_fill_rect(X, Y + ry, cw, rowh, TM_COL_HOVER);
        char nb[64];
        tm_stpcpy(nb, tm_startup_name(i));
        tm_stpcat(nb, " (");
        tm_stpcat(nb, tm_startup_key(i));
        tm_stpcat(nb, ")");
        int ty = Y + ry + (rowh - font_line_height()) / 2;
        tm_text_clip(X + name_x, ty, nb, TM_COL_TEXT, pub_x - name_x - 6);
        tm_text_clip(X + pub_x, ty, "VimtuOS", TM_COL_TEXT_DIM, sts_x - pub_x - 6);
        // 本轮接线：启动项状态来自 config64（startup.<key>），开关是**真开关**（点击即改 + 落盘）
        const bool on = cfg64_startup64(tm_startup_key(i)) ? true : false;
        const char* sts = on ? T("Enabled", "已启用") : T("Disabled", "已禁用");
        tm_text_clip(X + sts_x, ty, sts, on ? TM_COL_TEXT : TM_COL_TEXT_DIM, sw_x - sts_x - 6);
        tm_draw_switch(X + sw_x, Y + ry + 5, on, false);
    }

    // 说明（本轮已接线，文案必须与事实一致）
    if (last_bot + 18 <= bot) {
        int ly = last_bot + 6;
        const char* note1 = T("Startup items come from config64 (keys startup.*) and are persisted by store64",
                              "启动项来自 config64 的 startup.*，由 store64 真落盘（VimtuFS2 的 /store.a|b），");
        const char* note2 = T("into VimtuFS2 /store.a|b. Click a row to toggle; 'cfg save' flushes immediately.",
                              "点一行即切换；要立刻落盘可敲 cfg save（否则 3 秒内自动落盘）。");
        tm_text_clip(X + 16, Y + ly, note1, TM_COL_TEXT_DIM, cw - 32);
        tm_text_clip(X + 16, Y + ly + 16, note2, TM_COL_TEXT_DIM, cw - 32);
    }

    const char* label = T("Config", "配置");
    tm_draw_main_button(w, label, false);
}

// ---------------- 详细信息页 ----------------
static void tm_draw_detail(Window* w, TmState* st) {
    (void)st;                                   // 详细信息页是静态字段，不用实例状态
    int X = w->client_x, Y = w->client_y;
    int cw = w->client_w;
    int n = tm_fields_detail(w, g_fields, TM_MAX_FIELDS);

    int top = TM_CONTENT_Y;
    int bot = tm_status_y(w);
    int lh = font_line_height() + 2;
    if (lh > 20) lh = 20;
    if (lh < 14) lh = 14;

    if (top + TM_COL_HDR_H <= bot) {
        fb_fill_rect(X, Y + top, cw, TM_COL_HDR_H, TM_COL_HDRBG);
        int hy = Y + top + (TM_COL_HDR_H - font_line_height()) / 2;
        tm_text_clip(X + 12, hy, T("Property", "属性"), TM_COL_TEXT_DIM, 150);
        tm_text_clip(X + 162, hy, T("Value", "值"), TM_COL_TEXT_DIM, cw - 174);
        fb_draw_hline(X, Y + top + TM_COL_HDR_H, cw, TM_COL_BORDER);
    }

    int fields_top = top + TM_COL_HDR_H + 4;
    bool two = (cw >= 700 && n > 10);
    int per_col = two ? (n + 1) / 2 : n;
    int room = (bot - fields_top) / (lh > 0 ? lh : 1);
    if (per_col > room) per_col = room;
    if (per_col < 1) per_col = 1;
    tm_draw_fields(w, g_fields, n, fields_top, bot, per_col, lh, two);
}

// ---------------- 窗口绘制回调 ----------------
static void tm_draw(Window* w) {
    if (!w) return;
    TmState* st = tm_state(w);
    if (!st) return;

    tm_reap();                                   // 回收被外壳关掉的实例（补打 closed 日志）

    uint32_t now = ticks64();

    // 客户区尺寸变化 -> 布局日志（四页坐标全部由客户区推导，变化即重排）
    if (w->client_w != st->layout_w || w->client_h != st->layout_h) {
        st->layout_w = w->client_w;
        st->layout_h = w->client_h;
        tm_log_layout(w->client_w, w->client_h);
    }
    // 首帧打一条当前页日志（初始页 = 进程，之后每次真正切页都会再打）
    if (st->logged_page == 0xFF) {
        st->logged_page = st->page;
        tm_log_page(tm_page_key(st->page));
    }

    // 250ms 节流刷新显示值 + 请求客户区脏重绘（不整屏重绘）
    if ((uint32_t)(now - st->refresh_tick) >= ms_to_ticks64(TM_REFRESH_MS)) {
        st->refresh_tick = now;
        tm_read_now(st);
        tm_dirty_client(w);
    }
    // 1Hz 采样：曲线推点
    uint32_t sec = now / PIT_HZ_64;
    if (sec != st->sample_sec) {
        st->sample_sec = sec;
        tm_push_curve(st);
    }

    int status_y = tm_status_y(w);
    if (status_y > TM_CONTENT_Y) {
        fb_fill_rect(w->client_x, w->client_y + TM_CONTENT_Y, w->client_w,
                     status_y - TM_CONTENT_Y, TM_COL_CLIENT);
    }

    tm_draw_menubar(w, st);
    tm_draw_tabs(w, st);
    switch (st->page) {
        case 1:  tm_draw_perf(w, st); break;
        case 2:  tm_draw_startup(w, st); break;
        case 3:  tm_draw_detail(w, st); break;
        default: tm_draw_proc(w, st); break;
    }
    tm_draw_status(w, st);
    tm_draw_notice(w, st);
    if (st->menu_open) tm_draw_dropdown(w, st);   // 最后画：下拉盖住本窗口全部内容

    // 诊断日志：每 2 秒最多一条（首次立即打；多实例共享节流）
    if (!g_log_once || (uint32_t)(now - st->last_log_tick) >= ms_to_ticks64(TM_LOG_MS)) {
        g_log_once = true;
        st->last_log_tick = now;
        dbg64_line_begin64();
        dbg64_str("[UI] tmgr rows=");
        dbg64_dec((uint64_t)gui64_window_count());
        dbg64_str(" cpu=");
        dbg64_dec((uint64_t)gui64_cpu_busy_pct());
        dbg64_str(" mem=");
        dbg64_dec((uint64_t)tm_page_used_pct());
        dbg64_str(" fps=");
        dbg64_dec((uint64_t)gui64_fps());
        // 任务表口径（进程页行数据源；新增字段，不改动 [UI] tmgr rows= 前缀）
        dbg64_str(" tasks=");
        dbg64_dec((uint64_t)task_count64());
        dbg64_str(" switches=");
        dbg64_dec(task_switch_total64());
        // 行锁现场（诊断用）：正常情况下 lock=1 depth=1（本行自己持有）
        dbg64_str(" ll=");
        dbg64_dec((uint64_t)(uint32_t)g_dbg64_line_lock);
        dbg64_str("/");
        dbg64_dec((uint64_t)(uint32_t)g_dbg64_line_depth);
        dbg64_nl();
        dbg64_line_end64();
    }
}

// ---------------- 页切换 ----------------
static void tm_set_page(Window* w, TmState* st, int page) {
    if (page < 0 || page >= TM_PAGES) return;
    if ((int)st->page != page) {
        st->page = (uint8_t)page;
        st->scroll = 0;                       // 换页后滚动从头上开始（各页行数不同）
        tm_clear_notice(st);
    }
    if (st->logged_page != st->page) {
        st->logged_page = st->page;
        tm_log_page(tm_page_key(st->page));
    }
    tm_dirty_client(w);
}

// ---------------- 菜单动作 ----------------
static void tm_menu_action(Window* w, TmState* st, int idx) {
    st->menu_open = 0;
    switch (idx) {
        case 0:
            // 本内核没有运行时创建应用的接口（应用 = 各模块自己创建的窗口）
            tm_notice(st, T("Run new task: no runtime process API in this kernel",
                            "运行新任务：本内核没有运行时创建应用的接口"), false);
            tm_log_str("run new task: unsupported (no spawn API)", nullptr);
            break;
        case 1:
            tm_log_str("restart requested", nullptr);
            sys_reboot64();
            return;                            // 不再碰 w
        case 2:
            tm_log_str("shutdown requested", nullptr);
            sys_shutdown64();
            return;
        default:
            tm_close_self(w);                  // 先摘状态再销毁；返回后不能再碰 w
            return;
    }
    tm_dirty_client(w);
}

// ---------------- 结束进程 ----------------
// 进程页的行 = proc64 真进程，所以"结束进程"= proc64_kill64(pid, SIGKILL=9)：
// 可终止性由 proc64 裁决（idle/当前进程不可杀；无此 pid 返回负错误码）。成功/失败都打点。
static void tm_kill_selected(Window* w, TmState* st) {
    int n_rows = tm_build_rows(TM_MAX_ROWS);     // 刷新行表（进程可能在两次操作间变化了）
    if (st->sel_row < 0 || st->sel_row >= n_rows) {
        tm_notice(st, T("Select a process row first.", "请先选择一个进程行"), false);
        tm_dirty_client(w);
        return;
    }
    TmRow row = g_rows[st->sel_row];             // 拷贝：后面还会重建行表
    // 现场打点：kill 一定在 GUI 任务（kmain，task id 0）上下文里执行；如果这里出现别的
    //   task/proc，就是"当前进程判定"出了问题（proc64_kill64 会拒绝 kill self）。
    dbg64_line_begin64();
    dbg64_str("[UI] tmgr kill ctx task=");
    dbg64_dec((uint64_t)task_current_id64());
    dbg64_str(" proc=0x");
    dbg64_hex64((uint64_t)(uintptr_t)task_proc_of_current64());
    dbg64_str(" target=");
    dbg64_dec((uint64_t)row.id);
    dbg64_nl();
    dbg64_line_end64();
    const int64_t rc = proc64_kill64((int)row.id, 9);   // SIGKILL
    tm_log_kill_proc(&row, rc);

    if (rc == 0) tm_notice2(st, T("Process killed (SIGKILL): ", "已终止进程（SIGKILL）: "), row.name, false);
    else         tm_notice2(st, T("Cannot kill process (idle/current/absent): ",
                                  "无法终止进程（idle/当前/不存在）: "), row.name, true);

    int m = tm_build_rows(TM_MAX_ROWS);
    if (m == 0) st->sel_row = -1;
    else if (st->sel_row >= m) st->sel_row = m - 1;
    tm_dirty_client(w);
}

// ---------------- 键盘 ----------------
static void tm_move_sel(Window* w, TmState* st, int dir) {
    if (st->page == 1) {                          // 性能页：左列表 CPU/内存/磁盘
        int v = (int)st->perf_sel + dir;
        if (v < 0) v = 0;
        if (v > TM_PERF_ITEMS - 1) v = TM_PERF_ITEMS - 1;
        st->perf_sel = (uint8_t)v;
        tm_clear_notice(st);
        tm_dirty_client(w);
        return;
    }
    if (st->page == 2) {                          // 启动页：3 行
        int v = st->sel_row + dir;
        if (v < 0) v = 0;
        if (v > 2) v = 2;
        st->sel_row = v;
        tm_clear_notice(st);
        tm_dirty_client(w);
        return;
    }
    if (st->page == 3) return;                    // 详细信息页只有静态字段
    int n = tm_build_rows(TM_MAX_ROWS);           // 进程页
    if (n <= 0) { st->sel_row = -1; tm_dirty_client(w); return; }
    int s = st->sel_row;
    if (s < 0 || s > n - 1) s = (dir > 0) ? -1 : n;
    s += dir;
    if (s < 0) s = 0;
    if (s > n - 1) s = n - 1;
    st->sel_row = s;
    tm_rows_scroll_fix(w, st, n);
    tm_clear_notice(st);
    tm_dirty_client(w);
}

static void tm_key(Window* w, char c) {
    TmState* st = tm_state(w);
    if (!st) return;
    unsigned char cc = (unsigned char)c;          // char 有符号：先转无符号再比较

    // 下拉菜单展开时：键盘导航优先（input.cpp 的方向键码：0xFD/0xFE）
    if (st->menu_open) {
        if (cc == 0x1B) { st->menu_open = 0; tm_dirty_client(w); return; }
        if (cc == 0xFD) {
            st->menu_sel = (uint8_t)(((int)st->menu_sel + TM_MENU_ITEMS - 1) % TM_MENU_ITEMS);
            tm_dirty_client(w);
            return;
        }
        if (cc == 0xFE) {
            st->menu_sel = (uint8_t)(((int)st->menu_sel + 1) % TM_MENU_ITEMS);
            tm_dirty_client(w);
            return;
        }
        if (cc == '\n' || cc == '\r') { tm_menu_action(w, st, (int)st->menu_sel); return; }
        st->menu_open = 0;
    }

    if (cc == 0x1B) {                             // Esc：关窗
        tm_close_self(w);
        return;
    }
    if (cc == 0xFB || cc == 0xFC) {               // 左右方向：切标签页
        int np = (int)st->page;
        if (cc == 0xFB) np = (np + TM_PAGES - 1) % TM_PAGES;
        else            np = (np + 1) % TM_PAGES;
        tm_set_page(w, st, np);
        return;
    }
    if (cc == 0xFD || cc == 0xFE) {               // 上下方向：移动选中项
        tm_move_sel(w, st, (cc == 0xFD) ? -1 : 1);
        return;
    }
    if (cc == '\n' || cc == '\r') {               // Enter：当前页主操作
        if (st->page == 0) { tm_kill_selected(w, st); return; }
        if (st->page == 2) {
            tm_notice(st, T("Fixed startup item: not wired to store64, toggle would be session-only",
                            "启动项固定启用：未接 store64，开关只在本会话生效"), false);
            tm_dirty_client(w);
        }
        return;
    }
}

// ---------------- 鼠标回调（客户区局部坐标） ----------------
static void tm_click_proc(Window* w, int cx, int cy) {
    TmState* st = tm_state(w);
    if (!st) return;
    int bx = tm_btn_x(w), by = tm_btn_y(w);
    if (cx >= bx && cx < bx + TM_BTN_W && cy >= by && cy < by + TM_BTN_H) {
        tm_kill_selected(w, st);
        return;
    }
    int n = tm_build_rows(TM_MAX_ROWS);
    int bot = tm_list_bot(w);
    int y = tm_rows_top();
    for (int i = st->scroll; i < n; i++) {
        if (y + TM_ROW_H > bot) break;
        if (cy >= y && cy < y + TM_ROW_H && cx >= 4 && cx < w->client_w - 4) {
            st->sel_row = i;
            tm_clear_notice(st);
            tm_dirty_client(w);
            return;
        }
        y += TM_ROW_H;
    }
}

static void tm_click_perf(Window* w, int cx, int cy) {
    TmState* st = tm_state(w);
    if (!st) return;
    TmPerfLayout L;
    tm_perf_layout(w, 0, &L);                      // 列表几何只与客户区尺寸有关
    if (L.list_w <= 0) return;
    int lw = L.list_w;
    if (8 + lw > L.gx - 8) lw = L.gx - 16;
    for (int i = 0; i < TM_PERF_ITEMS; i++) {
        int iy = L.top + 4 + i * L.item_h;
        if (iy + L.item_h > L.bot) break;
        if (cx >= 8 && cx < 8 + lw && cy >= iy && cy < iy + L.item_h) {
            if ((int)st->perf_sel != i) {
                st->perf_sel = (uint8_t)i;
                tm_clear_notice(st);
                static const char* const kPerfKey[TM_PERF_ITEMS] = { "cpu", "memory", "disk", "gpu" };
                tm_log_str("perf item=", kPerfKey[i]);
            }
            tm_dirty_client(w);
            return;
        }
    }
}

static void tm_click_startup(Window* w, int cx, int cy) {
    TmState* st = tm_state(w);
    if (!st) return;
    int bot = tm_list_bot(w);
    if (cx < 4 || cx >= w->client_w - 4) return;      // 行外（边缘）不选中
    for (int i = 0; i < 3; i++) {
        int ry = TM_CONTENT_Y + TM_COL_HDR_H + 2 + i * 30;
        if (ry + 30 > bot) break;
        if (cy < ry || cy >= ry + 30) continue;
        st->sel_row = i;
        // 本轮接线：真开关 —— 改 config64 的 startup.<key>（落到 store64；3 秒内自动落盘）
        const char* key = tm_startup_key(i);
        const bool on = cfg64_startup64(key) ? true : false;
        cfg64_set_startup64(key, !on ? 1 : 0);
        tm_notice(st, (!on ? T("Startup item enabled (config64 saved; applies at next boot)",
                               "启动项已启用（写入 config64，下次启动生效）")
                          : T("Startup item disabled (config64 saved)",
                              "启动项已禁用（已写入 config64）")), true);
        tm_log_str("startup toggle ", key);
        tm_dirty_client(w);
        return;
    }
}

static void tm_click(Window* w, int cx, int cy) {
    TmState* st = tm_state(w);
    if (!st) return;

    // 1. 下拉菜单优先
    if (st->menu_open) {
        int dw = tm_dropdown_w();
        int dx = tm_menubtn_x(0);
        int dy = TM_MENU_H;
        int dh = TM_DD_PAD * 2 + TM_MENU_ITEMS * TM_DD_ITEM_H;
        if (cx >= dx && cx < dx + dw && cy >= dy + TM_DD_PAD && cy < dy + dh) {
            int idx = (cy - dy - TM_DD_PAD) / TM_DD_ITEM_H;
            if (idx >= 0 && idx < TM_MENU_ITEMS) {
                tm_menu_action(w, st, idx);
                return;                            // 可能已销毁窗口
            }
        }
        st->menu_open = 0;
        tm_dirty_client(w);
    }

    // 2. 菜单行
    if (cy >= 0 && cy < TM_MENU_H) {
        for (int i = 0; i < 4; i++) {
            int bx = tm_menubtn_x(i), bw = tm_menubtn_w(i);
            if (cx >= bx && cx < bx + bw) {
                if (i == 0) {
                    st->menu_open = 1;
                    st->menu_sel = 0;
                    tm_clear_notice(st);
                } else {
                    tm_notice(st, T("This menu is not implemented yet.", "该菜单暂未实现"), false);
                }
                tm_dirty_client(w);
                return;
            }
        }
        return;
    }

    // 3. 标签行
    if (cy >= TM_TAB_Y && cy < TM_TAB_Y + TM_TAB_H) {
        for (int i = 0; i < TM_PAGES; i++) {
            int bx = tm_tab_x(i), bw = tm_tab_box_w(i);
            if (cx >= bx && cx < bx + bw) {
                tm_set_page(w, st, i);
                return;
            }
        }
        return;
    }

    // 4. 内容区
    if (cy < TM_CONTENT_Y) return;
    if (cy >= tm_status_y(w)) return;              // 状态栏
    switch (st->page) {
        case 1:  tm_click_perf(w, cx, cy); break;
        case 2:  tm_click_startup(w, cx, cy); break;
        case 3:  break;                            // 详细信息页：静态字段，无点击目标
        default: tm_click_proc(w, cx, cy); break;
    }
}

// ---------------- 对外入口（gui64.h） ----------------
void app_tmgr_reset64() {
    tm_log_app("reset");
    // 会话清理：状态都在各窗口的 userdata 里。外壳没有 on_close 钩子，这里统一摘掉并释放，
    // 窗口本身由调用方（外壳/重启流程）负责销毁。
    for (int i = 0; i < (int)(sizeof(g_slots) / sizeof(g_slots[0])); i++) {
        Window* w = g_slots[i].w;
        TmState* st = g_slots[i].st;
        g_slots[i].w = nullptr;
        g_slots[i].st = nullptr;
        if (!w) { tm_free_state(st); continue; }
        if (gui64_window_alive(w) && tm_is_tmgr_window(w) && w->userdata == st) w->userdata = nullptr;
        tm_free_state(st);
    }
    g_last_win = nullptr;
}

void app_tmgr_open64() {
    tm_reap();

    int inst = gui64_app_windows(APP_ID_TMGR);

    // 多开上限：已满则只激活最近一个实例，并在该窗口提示
    if (inst >= TM_MAX_INST) {
        Window* w = tm_any_window();
        if (w) {
            gui64_set_active(w);
            TmState* st = tm_state(w);
            if (st) {
                tm_notice(st, T("Maximum 4 Task Manager windows.", "最多同时打开 4 个任务管理器窗口"), true);
                tm_dirty_client(w);
            }
        }
        tm_log_str("instance limit reached", nullptr);
        return;
    }

    // 几何：默认尺寸按屏幕/任务栏收敛，且不小于最小尺寸
    int sw = gui64_screen_w(), sh = gui64_screen_h();
    int tb = gui64_taskbar_h();
    int ww = 780, wh = 600;
    if (ww > sw - 24) ww = sw - 24;
    if (ww < TM_MIN_W) ww = TM_MIN_W;
    int avail_h = sh - tb - 16;
    if (wh > avail_h) wh = avail_h;
    if (wh < TM_MIN_H) wh = TM_MIN_H;
    int x = (sw - ww) / 2;
    int y = (sh - tb - wh) / 2;
    if (inst > 0) {                                 // 多实例：错开摆放（与 32 位一致）
        x += 26 * inst;
        y += 22 * inst;
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + ww > sw) x = sw - ww;
    if (y + wh > sh - tb) y = sh - tb - wh;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    // 每实例状态：归属记账到 MEM_OWNER_TMGR（进程页"内存"列显示的是真实记账）
    int prev_owner = mem_owner_get_64();
    mem_owner_set_64(MEM_OWNER_TMGR_64);
    TmState* st = (TmState*)kmalloc_64((uint64_t)sizeof(TmState));
    if (!st) {
        mem_owner_set_64(prev_owner);
        tm_log_str("open failed: out of heap", nullptr);
        return;
    }
    tm_memzero(st, (int)sizeof(TmState));
    st->page = 0;
    st->sel_row = -1;                               // 未选中：先选一行再操作
    st->perf_sel = 0;
    st->perf_logged_sel = 0xFF;                     // 批次 B：首帧画性能页时补一条 [UI] tmgr perf 行
    st->scroll = 0;
    st->menu_open = 0;
    st->menu_sel = 0;
    st->refresh_tick = ticks64();
    st->sample_sec = 0xFFFFFFFFu;                   // 首帧立即采样一次
    st->last_log_tick = 0;
    st->proc_log_tick = 0;                          // 进程页行日志：首帧 proc_rows_logged(-1)!=0 会立即打
    st->proc_rows_logged = -1;
    st->curve_n = 0;
    st->layout_w = -1;
    st->layout_h = -1;
    st->notice_len = 0;
    st->notice[0] = 0;
    st->notice_warn = 0;
    st->logged_page = 0xFF;                         // 首帧打一条 page 行
    tm_read_now(st);

    char title[32];
    if (inst > 0) {
        char nb[8];
        tm_itoa(nb, inst + 1);
        tm_strlcpy(title, T("Task Manager", "任务管理器"), (int)sizeof(title));
        tm_stpcat(title, " (");
        tm_stpcat(title, nb);
        tm_stpcat(title, ")");
    } else {
        tm_strlcpy(title, T("Task Manager", "任务管理器"), (int)sizeof(title));
    }

    Window* win = gui64_create_window(title, x, y, ww, wh, tm_draw, tm_key, tm_click, APP_ID_TMGR);
    mem_owner_set_64(prev_owner);
    if (!win) {
        tm_free_state(st);                          // 没建成窗口：状态也不能泄漏
        tm_log_str("create window failed", nullptr);
        return;
    }
    win->userdata = st;                             // 外壳不释放 userdata：本文件用 g_slots + tm_reap() 回收
    win->mem_owner = MEM_OWNER_TMGR_64;             // 内存归属标签（进程页/性能页按它统计）
    gui64_set_min_size(win, TM_MIN_W, TM_MIN_H);
    tm_slot_add(win, st);
    g_last_win = win;

    tm_log_app("opened");
    dbg64_str("[APP] tmgr open geom x=");
    dbg64_dec((uint64_t)x);
    dbg64_str(" y=");
    dbg64_dec((uint64_t)y);
    dbg64_str(" w=");
    dbg64_dec((uint64_t)ww);
    dbg64_str(" h=");
    dbg64_dec((uint64_t)wh);
    dbg64_str(" inst=");
    dbg64_dec((uint64_t)(inst + 1));
    dbg64_nl();
    gui64_set_active(win);
    gui64_invalidate_window(win);
}
