// terminal64.cpp - Vimtu64 终端窗口 + 内建 Shell（64 位版）
//
// 【参考实现】Vimtu32/kernel/terminal.cpp（32 位终端 + Shell，987 行）。功能以它为准：
//   字符网格缓冲 / 滚屏、提示符与欢迎语、行编辑（可打印字符 / 退格 / 回车执行 / Esc 关窗）、
//   命令分发 shell_exec、每个命令的输出文案。
//
// 【契约】只用 kernel/gui64.h 提供的外壳接口：应用不直接改窗口几何（走 gui64_*），
//   绘制一律用屏幕绝对坐标（w->client_x + 局部 x）；外壳已按客户区设好 fb_set_clip 并做脏矩形提交，
//   所以这里只负责"内容 + 客户区里哪块变了"（gui64_dirty）。
//   键盘字符由外壳通过 WindowKeyFn 投递（不直接读 input.h 的键盘队列）。
//
// 【多开】最多 TERM_MAX_INST = 4 个实例；每实例一份 TerminalState（kmalloc_64 一次分配"结构 + 内容缓冲"，
//   归属记账 MEM_OWNER_TERMINAL_64），指针挂 Window::userdata；第 5 次 open 只激活最近创建的那个（照 32 位）。
//
// 【关窗时的内存归属 —— 与 32 位的差异，踩坑点】
//   32 位：gui.cpp 在销毁窗口时按 app_id 统一 kfree(userdata)，应用自己不 free。
//   64 位的 gui64.h 却写明"外壳不会替你释放，自己 on_close 里放"——但 Window 结构里**没有 on_close 钩子**，
//   两种外壳实现（释放 / 不释放 userdata）都可能出现。这里的做法对两种情况都安全：
//     * 自己关（Esc）：先把 w->userdata 置空再 gui64_destroy_window()（外壳按 app_id 释放时看到空指针跳过，
//       kfree_64(nullptr) 本身也安全返回），状态由本文件的"待释放队列"延迟释放；
//     * 外壳关（标题栏 X）：tick 里看到 w->closing 就抢在销毁前接管（同样置空 userdata + 入队延迟释放）；
//     * 外壳一步到位销毁（没给 closing 机会，例如 gui64_close_all_windows）：只从注册表摘除、**不**释放内存
//       （无法判断外壳是否已释放，宁可少释放也不能二次 kfree），代价是这种路径会漏掉一份 ~19KB 状态，
//       有 on_close 钩子后可以彻底消除。
//   待释放队列延迟 TERM_PEND_DELAY 个 tick，且释放前再确认没有任何活窗口的 userdata 指向它。
//
// 【排版】列/行由客户区尺寸推导（不再是编译期常量），默认 60x40：
//     cols = (client_w - PAD_X) / 16,  rows = (client_h - PAD_Y) / 16
//   字符格 16x16 = 内建 8x8 位图字体 × scale 2（fb_draw_char 正好适合终端）。
//   但 8x8 位图字体只有 ASCII 32..126，所以内容缓冲做成**码点网格**（uint16_t，0 = 空）：
//     cp < 0x7F  -> fb_draw_char(...)（等宽、带底色）
//     其它（中文）-> font_draw_glyph_cp(...)（TrueType，CJK 自动落回 simhei 面；只有前景色，底色靠预填）
//   于是"中英混排都是一格一字"，退格 / 滚动 / 重排与 32 位完全一致，中文提示也能显示。
//   拖边框缩放 -> 下一次绘制时重排（保留"旧区域 ∩ 新区域"，越界裁剪），并打一行 [UI] term layout。
//
// 【日志】自动验收依赖下面这些行（原样）：
//   [APP] term opened cols=N rows=M     开窗（含实例内实际排版列/行）
//   [APP] term reset                    会话重置（清屏/清状态）
//   [APP] term closed                   关窗
//   [UI]  term layout client=WxH cols=N rows=M   布局变化（开窗时也打一行）
//   [TERM] cmd <名字> ok|fail            每条命令执行打一行（本轮起固定带 ok/fail —— 新增的真命令
//                                        用它做是否真的实现过的判据；旧格式只有失败才带 fail）
//   [TERM] unsupported <名字>: <原因>     未支持命令的说明（现在只剩 update / preload 两条）
//   [TASK] ps rows=N switches=M          ps/tasks/task/top 打点（任务表真实行数 + 累计切换次数）
//   [TASK] kill id=N ok                  kill <id> 成功（终端的 kill 只走 task_kill64 真路径）
//   [APP64] run cmd path=<p> rc=<n>      run <name> 的命令打点（走 app64_launch64 真路径）
//   [STORE64] cmd <dump|get|set|flush> ...  store 命令打点（走 kernel/store64.cpp 真路径）
//
// 【Shell 命令】32 位能实现、且 64 位有对应子系统的**全部**实现；仍未移植的只有 2 条
//   （update / preload，属于后续批次），它们的提示里有"尚未支持"字样。
//   本轮刚接真的：hw/hwinfo（hwinfo64 的 CPU/PCI）、lspci/pci（PCI 设备表）、disk/ata（ATA IDENTIFY
//   型号/容量 + VimtuFS2 卷几何）、user/userprog（ring3 现状，arg=run 时跑一次用户程序）、
//   cfg/config（config64 类型化配置 + 落盘位置）、syslog/state/health（sysstate64 状态机/模块/健康/ring log）、
//   session（session64 会话策略）、restart --soft（优雅停止 + 硬复位链）、panic/bsod（受控蓝屏）。
//   任务相关命令（ps/tasks/task/top/kill <id>）走内核任务表（kernel/task64.h 的对外 API），
//   没有任何假数据：任务数为 0 时如实打印"无任务数据"。
//   文件系统：本文件内仍有一个最小 ramfs（16 个文件 × 512 字节，RAM only）供 echo >/cat/ls 用；
//   **真文件系统**是 VimtuFS2（kernel/vfs64.cpp，磁盘上的 /store.a|b、/hello.vap、/hello.elf 都在它上面）。
//
// 【约束】只允许整数运算（内核 -mno-sse，无 float/double）；不 include 标准头（无 STL/libc/printf），
//   数字格式化 / 字符串比较 / UTF-8 解码全部是下面的 static 工具。
#include "gui64.h"
#include "fb.h"
#include "font.h"
#include "input.h"
#include "mem_64.h"
#include "x86_64.h"
#include "debug64.h"
#include "task64.h"    // 任务表（Task64Info / task_info64 / task_kill64 / TASK64_*）
#include "app64.h"     // VAP64 应用启动器 + 按魔数自动分派（run <name>：从 VimtuFS2 读出来进 ring3）
#include "elf64.h"     // ELF64 加载器（elfrun <path>；run 也会按文件头自动分派到它）
#include "store64.h"   // 设置持久化 store（store dump|get|set|flush：VFS 文件 /store.a、/store.b）
#include "net64.h"     // 网络（e1000 + ARP/ICMP）：ping 命令走它的真路径
// ---- 本轮接线（终端命令的真实现）----
#include "hwinfo64.h"    // hw/hwinfo + lspci：CPU/PCI 真实枚举结果
#include "ata64.h"       // disk/ata：ATA IDENTIFY（型号/容量；读取自带超时保护）
#include "vfs64.h"       // disk：卷状态；user：盘上的 ring3 程序
#include "usermode64.h"  // user/userprog：ring3 用户窗口地址与页映射查询
#include "config64.h"    // cfg/config：类型化配置 + 存储位置（落在 store64 上）
#include "session64.h"   // session：会话/应用内容策略的真实现
#include "sysstate64.h"  // syslog/state/health：状态机 + 模块表 + 健康 + ring log
#include "panic64.h"     // panic/bsod：受控蓝屏；ping 期间的看门狗停表
#include <stdint.h>

// ==================== 常量 ====================
#define TERM_MAX_INST    4          // 多开上限（照 32 位；第 5 次只激活最新的）
#define TERM_CHAR_SCALE  2          // 8x8 位图字体放大 2 倍 -> 16x16
#define TERM_CELL_W      (8 * TERM_CHAR_SCALE)      // 字符格宽（像素）
#define TERM_CELL_H      (8 * TERM_CHAR_SCALE)      // 字符格高（像素）
#define TERM_COLS_MAX    128        // 内容缓冲列上限（1920 宽的桌面最大化也够）
#define TERM_ROWS_MAX    72         // 内容缓冲行上限
#define TERM_COLS_MIN    20         // 排版列下限（更窄的客户区绘制时按像素裁剪）
#define TERM_ROWS_MIN    6          // 排版行下限
#define TERM_CMD_MAX     256        // 命令行缓冲
#define TERM_DEF_COLS    60         // 新建窗口默认排版（与 32 位一致）
#define TERM_DEF_ROWS    40
#define TERM_BUF_MAX     (TERM_COLS_MAX * TERM_ROWS_MAX)
#define TERM_PAD_X       4          // 排版可用宽 = client_w - PAD_X
#define TERM_PAD_Y       4          // 排版可用高 = client_h - PAD_Y
#define TERM_ZH_DY       (-2)       // TrueType 中文字形相对格顶的微调（em 盒 vs 位图字形对齐）
#define TERM_PEND_MAX    8          // 待释放队列长度
#define TERM_PEND_DELAY  4          // 延迟释放的 tick 数（约 16ms，足够外壳销毁窗口）

// 配色照 32 位：黑底 + 浅灰字 + 绿色块状光标（绿底黑字）
#define TERM_BG      rgb(0x00, 0x00, 0x00)
#define TERM_FG      rgb(0xE0, 0xE0, 0xE0)
#define TERM_CUR     rgb(0x00, 0xC0, 0x00)
#define TERM_CURFG   rgb(0x00, 0x00, 0x00)

// 终端内的最小 ramfs（64 位还没有真实文件系统：32 位的 vfs.cpp 未移植）
#define RAMFS_FILES     16
#define RAMFS_NAME_MAX  32
#define RAMFS_DATA_MAX  512

// ==================== 终端实例状态 ====================
// 每窗口一份：一次 kmalloc = 结构 + 内容缓冲（cells 指向结构之后）
struct TerminalState {
    uint16_t* cells;                 // 内容缓冲：码点网格（0 = 空），行距固定 TERM_COLS_MAX
    int   cols, rows;                // 当前排版列/行（由客户区尺寸推导）
    int   cur_r, cur_c;              // 光标（内容坐标）
    char  cmdline[TERM_CMD_MAX];     // 当前命令行（可打印 ASCII）
    int   cmdlen;
    Window* win;                     // 所属窗口
    int   inst;                      // 实例序号（1 起，日志用）
    bool  used;                      // 已初始化（有效实例）
};

// 实例注册表：紧凑排列，只存指针；存活判断用"指针比较 + 活窗口 userdata 回指"，不解引用可能已释放的状态
static TerminalState* g_inst[TERM_MAX_INST];
static Window*        g_win [TERM_MAX_INST];
// 实例序号的影子副本：清理路径绝不解引用"可能已被外壳释放"的状态，日志用这里的副本
static int            g_inst_no[TERM_MAX_INST];
static int            g_count = 0;
static int            g_next_inst = 1;

// 待释放队列（自己接管下来的状态，等窗口真的消失后再 kfree）
static TerminalState* g_pend[TERM_PEND_MAX];
static uint32_t       g_pend_tick[TERM_PEND_MAX];
static int            g_pend_n = 0;

static bool g_inited = false;

// 文件级缓冲：内核线程栈很小，这些绝不放栈上（-Wall 下也便于审计）
static char g_num[32];                 // 数字格式化
static char g_cmd[64];                 // 命令解析：命令名
static char g_arg1[128];               // 第 1 个参数
static char g_arg2[192];               // 第 2 个参数
static char g_pathbuf[RAMFS_NAME_MAX]; // 文件名（规范化后）
static char g_store_k[32];             // store 命令：key 缓冲（key 最长 31B + NUL）
static char g_store_v[256];            // store 命令：value 缓冲（value 最长 255B + NUL）

// ==================== 最小 ramfs ====================
struct RamFile {
    bool used;
    char name[RAMFS_NAME_MAX];
    int  size;
    char data[RAMFS_DATA_MAX + 1];       // 末尾恒有 NUL，便于整串打印
};
static RamFile g_fs[RAMFS_FILES];
static bool g_fs_ready = false;

// ==================== 小工具（无 libc，全部自己写） ====================
static bool is_ws(char c) { return c == ' ' || c == '\t'; }

static int st_len(const char* s) {
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static int st_cmp(const char* a, const char* b) {
    if (!a) a = "";
    if (!b) b = "";
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static bool st_eq(const char* a, const char* b) { return st_cmp(a, b) == 0; }

static const char* skip_ws(const char* s) {
    if (!s) return "";
    while (*s && is_ws(*s)) s++;
    return s;
}

// 取一个以空白分隔的 token；返回 token 之后的指针（不跳过空白）；out 可为 nullptr
static const char* grab_token(const char* s, char* out, int max) {
    int n = 0;
    if (out && max > 0) out[0] = 0;
    if (!s) return "";
    while (*s && !is_ws(*s)) {
        if (out && n < max - 1) out[n++] = *s;
        s++;
    }
    if (out && max > 0) out[n] = 0;
    return s;
}

// 十进制（无符号）/ 带符号 / 零填充
static int fmt_u64(char* out, int max, uint64_t v) {
    char tmp[24];
    int n = 0, o = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (n > 0 && o < max - 1) out[o++] = tmp[--n];
    if (max > 0) out[o] = 0;
    return o;
}

static int fmt_u64_pad(char* out, int max, uint64_t v, int width) {
    char tmp[24];
    int n = 0, o = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    for (int i = n; i < width && o < max - 1; i++) out[o++] = '0';
    while (n > 0 && o < max - 1) out[o++] = tmp[--n];
    if (max > 0) out[o] = 0;
    return o;
}

static int fmt_i64(char* out, int max, int64_t v) {
    if (v < 0) {
        if (max < 2) { if (max > 0) out[0] = 0; return 0; }
        out[0] = '-';
        int n = fmt_u64(out + 1, max - 1, (uint64_t)(0 - v));
        return n + 1;
    }
    return fmt_u64(out, max, (uint64_t)v);
}

// 十进制解析（全部字符都得是数字）：返回 -1 = 非法/没给
static int parse_dec(const char* s) {
    if (!s || !s[0]) return -1;
    int v = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (int)(s[i] - '0');
        if (v > 100000) return -1;            // 任务 id 不可能这么大（挡住溢出）
    }
    return v;
}

// UTF-8 解码一个字符：返回码点，*adv = 消耗字节数（非法首字节按 1 字节、返回 0 跳过）
static uint32_t utf8_next(const char* s, int* adv) {
    const unsigned char* p = (const unsigned char*)s;
    unsigned char c = p[0];
    if (c < 0x80) { *adv = 1; return (uint32_t)c; }
    if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *adv = 2; return (uint32_t)(((c & 0x1Fu) << 6) | (p[1] & 0x3Fu));
    }
    if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *adv = 3; return (uint32_t)(((c & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu));
    }
    if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *adv = 4;
        return (uint32_t)(((c & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu));
    }
    *adv = 1; return 0;
}

// ==================== 串口日志 ====================
static void term_log_cmd(const char* name, bool ok) {
    dbg64_str("[TERM] cmd ");
    dbg64_str((name && name[0]) ? name : "?");
    dbg64_str(ok ? " ok" : " fail");
    dbg64_nl();
}

// 未支持命令的日志：<名字> + 具体原因（用英文串，ASCII，方便自动验收 grep）
static void term_log_unsupported(const char* name, const char* reason) {
    dbg64_str("[TERM] unsupported ");
    dbg64_str(name ? name : "?");
    dbg64_str(": ");
    dbg64_str(reason ? reason : "subsystem not ported to 64-bit");
    dbg64_nl();
}

// ==================== 实例查找 / 清理 ====================
// 从窗口取实例状态（app_id + userdata + 回指三重校验，避免把别的窗口的 userdata 当成 TerminalState）
static TerminalState* ts_of(Window* w) {
    if (!w || w->app_id != APP_ID_TERM || !w->userdata) return nullptr;
    TerminalState* ts = (TerminalState*)w->userdata;
    if (!ts->used || ts->win != w) return nullptr;
    return ts;
}

// 第 i 个注册槽是否仍指向活着的终端窗口。只做指针比较（win 指针 + 活窗口的 userdata 回指），
// 不解引用可能已被释放的实例内存。
static bool ts_slot_live(int i) {
    if (i < 0 || i >= g_count) return false;
    Window* w = g_win[i];
    if (!w) return false;
    for (int k = 0; k < g_count; k++) {          // 同一窗口只允许占一个槽（防重复计数）
        if (k != i && g_win[k] == w) return false;
    }
    if (!gui64_window_alive(w)) return false;
    return w->app_id == APP_ID_TERM && w->userdata == (void*)g_inst[i];
}

// 清理已失效的槽（不留悬垂指针）；外壳自己销毁窗口的路径在这里被发现
static void ts_prune() {
    if (!g_inited) return;
    int n = 0;
    for (int i = 0; i < g_count; i++) {
        if (!ts_slot_live(i)) {
            // 外壳自己销毁了窗口（没经过本文件的关窗路径）：只摘除引用，不释放内存
            // （无法判断外壳是否已 kfree(userdata)，宁可少释放也不能二次 kfree）
            dbg64_str("[APP] term closed");
            dbg64_nl();
            dbg64_str("[TERM] closed inst=");
            dbg64_dec((uint64_t)g_inst_no[i]);
            dbg64_str(" by shell (state left to window layer)");
            dbg64_nl();
            continue;
        }
        if (n != i) { g_inst[n] = g_inst[i]; g_win[n] = g_win[i]; g_inst_no[n] = g_inst_no[i]; }
        n++;
    }
    for (int i = n; i < g_count; i++) { g_inst[i] = nullptr; g_win[i] = nullptr; g_inst_no[i] = 0; }
    g_count = n;
}

// 是否有活窗口的 userdata 仍指向该状态（释放前的最后一道保险）
static bool ts_referenced(TerminalState* ts) {
    int n = gui64_window_count();
    for (int i = 0; i < n; i++) {
        Window* w = gui64_window_at(i);
        if (w && w->userdata == (void*)ts) return true;
    }
    return false;
}

// 待释放队列：延迟若干 tick 且确认无人引用后 kfree（force = 开窗/重置时立即清理）
static void ts_reap_pending(bool force) {
    uint32_t now = ticks64();
    int i = 0;
    while (i < g_pend_n) {
        bool due = force || (uint32_t)(now - g_pend_tick[i]) >= TERM_PEND_DELAY;
        if (due && !ts_referenced(g_pend[i])) {
            kfree_64(g_pend[i]);
            g_pend[i] = g_pend[g_pend_n - 1];
            g_pend_tick[i] = g_pend_tick[g_pend_n - 1];
            g_pend_n--;
            continue;
        }
        i++;
    }
}

// 从注册表摘除并把状态交给待释放队列（调用方负责先把 w->userdata 置空）
static void ts_detach(TerminalState* ts) {
    int n = 0;
    for (int i = 0; i < g_count; i++) {
        if (g_inst[i] == ts) continue;
        if (n != i) { g_inst[n] = g_inst[i]; g_win[n] = g_win[i]; g_inst_no[n] = g_inst_no[i]; }
        n++;
    }
    for (int i = n; i < g_count; i++) { g_inst[i] = nullptr; g_win[i] = nullptr; g_inst_no[i] = 0; }
    g_count = n;
    if (!ts) return;
    ts->used = false;
    if (g_pend_n < TERM_PEND_MAX) {
        g_pend[g_pend_n] = ts;
        g_pend_tick[g_pend_n] = ticks64();
        g_pend_n++;
    } else {
        // 队列满（理论上到不了）：不释放也不复用，避免任何悬垂/二次释放
        dbg64_str("[TERM] state free queue full, left unreleased");
        dbg64_nl();
    }
}

// ==================== 内容缓冲操作（按实例） ====================
static inline void ts_cell_set(TerminalState* ts, int r, int c, uint16_t cp) {
    if (r < 0 || r >= TERM_ROWS_MAX || c < 0 || c >= TERM_COLS_MAX) return;
    ts->cells[r * TERM_COLS_MAX + c] = cp;
}

static void ts_clear(TerminalState* ts) {
    for (int i = 0; i < TERM_BUF_MAX; i++) ts->cells[i] = 0;
    ts->cur_r = 0;
    ts->cur_c = 0;
}

// 脏矩形（只用 gui64_dirty，不整屏重绘）
static void ts_dirty_client(TerminalState* ts) {
    Window* w = ts ? ts->win : nullptr;
    if (!w || !w->visible) return;
    gui64_dirty(w->client_x, w->client_y, w->client_w, w->client_h);
}

// 一行字符 + 该行光标块都在同一行内，所以按行标脏即可
static void ts_dirty_row(TerminalState* ts, int row) {
    Window* w = ts ? ts->win : nullptr;
    if (!w || !w->visible) return;
    if (row < 0 || row >= ts->rows) return;
    gui64_dirty(w->client_x, w->client_y + TERM_PAD_Y + row * TERM_CELL_H, w->client_w, TERM_CELL_H);
}

// 排版尺寸：客户区放得下的列/行（不钳制）
static void ts_fit(Window* w, int* cols, int* rows) {
    int c = (w->client_w - TERM_PAD_X) / TERM_CELL_W;
    int r = (w->client_h - TERM_PAD_Y) / TERM_CELL_H;
    if (c < 0) c = 0;
    if (r < 0) r = 0;
    *cols = c;
    *rows = r;
}

// 排版尺寸：客户区推导值钳制到 [下限, 上限]
static void ts_layout(Window* w, int* cols, int* rows) {
    int c, r;
    ts_fit(w, &c, &r);
    if (c < TERM_COLS_MIN) c = TERM_COLS_MIN;
    if (r < TERM_ROWS_MIN) r = TERM_ROWS_MIN;
    if (c > TERM_COLS_MAX) c = TERM_COLS_MAX;
    if (r > TERM_ROWS_MAX) r = TERM_ROWS_MAX;
    *cols = c;
    *rows = r;
}

// 缩放后重排：保留"旧区域 ∩ 新区域"（= 左上角可见部分，越界内容丢弃），其余全部清空。
// 只在 cols/rows 真正变化时执行（不是每帧），所以整块缓冲扫一遍也不贵。
static void ts_reflow(TerminalState* ts, int cols, int rows) {
    if (cols == ts->cols && rows == ts->rows) return;
    if (cols > TERM_COLS_MAX) cols = TERM_COLS_MAX;
    if (rows > TERM_ROWS_MAX) rows = TERM_ROWS_MAX;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    int old_c = ts->cols, old_r = ts->rows;
    for (int r = 0; r < TERM_ROWS_MAX; r++) {
        uint16_t* row = ts->cells + r * TERM_COLS_MAX;
        if (r >= rows || r >= old_r) {                 // 超出新行数 / 新出现的行：整行清空
            for (int c = 0; c < TERM_COLS_MAX; c++) row[c] = 0;
            continue;
        }
        int keep_c = (old_c < cols) ? old_c : cols;    // 列方向：保留 c < min(old_c, cols)
        for (int c = keep_c; c < TERM_COLS_MAX; c++) row[c] = 0;
    }
    ts->cols = cols;
    ts->rows = rows;
    if (ts->cur_r >= rows) ts->cur_r = rows - 1;
    if (ts->cur_c >= cols) ts->cur_c = cols - 1;
    if (ts->cur_r < 0) ts->cur_r = 0;
    if (ts->cur_c < 0) ts->cur_c = 0;
}

// 按窗口当前客户区尺寸同步排版（绘制时调用；用户拖边框缩放后自动重排）。
// 真正发生重排时打一行 [UI] term layout，供自动化验收断言"终端行列数随窗口尺寸变化"。
static void ts_sync_layout(TerminalState* ts, Window* w) {
    int cols, rows;
    int old_c = ts->cols, old_r = ts->rows;
    ts_layout(w, &cols, &rows);
    ts_reflow(ts, cols, rows);
    if (cols != old_c || rows != old_r) {
        dbg64_str("[UI] term layout client=");
        dbg64_dec((uint64_t)w->client_w);
        dbg64_str("x");
        dbg64_dec((uint64_t)w->client_h);
        dbg64_str(" cols=");
        dbg64_dec((uint64_t)cols);
        dbg64_str(" rows=");
        dbg64_dec((uint64_t)rows);
        dbg64_nl();
    }
}

// 内容整体上滚一行（底行清空）
static void ts_scroll(TerminalState* ts) {
    for (int r = 1; r < ts->rows; r++) {
        uint16_t* dst = ts->cells + (r - 1) * TERM_COLS_MAX;
        const uint16_t* src = ts->cells + r * TERM_COLS_MAX;
        for (int c = 0; c < ts->cols; c++) dst[c] = src[c];
    }
    uint16_t* last = ts->cells + (ts->rows - 1) * TERM_COLS_MAX;
    for (int c = 0; c < ts->cols; c++) last[c] = 0;
}

// 换行（到底行则滚动）
static void ts_newline(TerminalState* ts, bool* scrolled) {
    ts->cur_r++;
    ts->cur_c = 0;
    if (ts->cur_r >= ts->rows) {
        ts_scroll(ts);
        ts->cur_r = ts->rows - 1;
        *scrolled = true;
    }
}

// 写一个码点：按该实例当前 cols/rows 裁剪与滚动，并只标脏受影响的区域
static void ts_putc(TerminalState* ts, uint32_t cp) {
    if (!ts || !ts->used || ts->cols < 1 || ts->rows < 1) return;
    int r0 = ts->cur_r;
    bool scrolled = false;
    if (cp == (uint32_t)'\n') {
        ts_newline(ts, &scrolled);
    } else if (cp == (uint32_t)'\b') {
        if (ts->cur_c > 0) ts->cur_c--;
        ts_cell_set(ts, ts->cur_r, ts->cur_c, 0);
    } else if (cp == (uint32_t)'\t') {
        do { ts->cur_c++; } while ((ts->cur_c % 4) && ts->cur_c < ts->cols);
        if (ts->cur_c >= ts->cols) ts_newline(ts, &scrolled);
    } else if (cp >= 0x20) {
        if (ts->cur_c >= ts->cols) ts_newline(ts, &scrolled);   // 行满自动换行
        if (cp > 0xFFFF) cp = (uint32_t)'?';                    // 缓冲是 uint16 码点（BMP）
        ts_cell_set(ts, ts->cur_r, ts->cur_c, (uint16_t)cp);
        ts->cur_c++;
    } else {
        return;   // 其它控制字符：忽略（不动屏幕，也不标脏）
    }
    if (scrolled) {
        ts_dirty_client(ts);                 // 滚动：整块客户区
    } else {
        ts_dirty_row(ts, r0);                // 否则旧光标行 + 新光标行（同一行则重复无害）
        ts_dirty_row(ts, ts->cur_r);
    }
}

// 写一个 UTF-8 串
static void ts_puts(TerminalState* ts, const char* s) {
    if (!ts || !s) return;
    const char* p = s;
    while (*p) {
        int adv = 0;
        uint32_t cp = utf8_next(p, &adv);
        p += adv;
        if (cp == 0 && adv == 1) continue;    // 非法字节跳过
        ts_putc(ts, cp);
    }
}

static void ts_put_u64(TerminalState* ts, uint64_t v) {
    fmt_u64(g_num, (int)sizeof(g_num), v);
    ts_puts(ts, g_num);
}

static void ts_put_u64_pad(TerminalState* ts, uint64_t v, int width) {
    fmt_u64_pad(g_num, (int)sizeof(g_num), v, width);
    ts_puts(ts, g_num);
}

static void ts_put_i64(TerminalState* ts, int64_t v) {
    fmt_i64(g_num, (int)sizeof(g_num), v);
    ts_puts(ts, g_num);
}

// 左对齐补空格（名字列）
static void ts_puts_pad(TerminalState* ts, const char* s, int width) {
    ts_puts(ts, s);
    int n = st_len(s);
    while (n < width) { ts_putc(ts, (uint32_t)' '); n++; }
}

// ==================== ramfs 实现（最小，RAM only） ====================
// 平铺命名空间：去掉前导 '/'，只取第一个 token（"a.txt" == "/a.txt"）
static void ramfs_norm(const char* in, char* out, int max) {
    // 先把结果取到临时缓冲，再写 out：这样 in == out（自别名，例如 rm 里对 g_pathbuf 再规范化一次）
    // 也安全 —— 旧写法先写 out[0] = 0，会把输入串自己清掉，导致 rm/touch 找不到文件。
    char tmp[RAMFS_NAME_MAX];
    int t = 0;
    if (in) {
        while (*in == '/' || is_ws(*in)) in++;
        while (*in && !is_ws(*in) && t < RAMFS_NAME_MAX - 1) tmp[t++] = *in++;
    }
    tmp[t] = 0;
    int o = 0;
    if (out && max > 0) {
        while (o < t && o < max - 1) { out[o] = tmp[o]; o++; }
        out[o] = 0;
    }
}

static int ramfs_find(const char* name) {
    for (int i = 0; i < RAMFS_FILES; i++) {
        if (g_fs[i].used && st_eq(g_fs[i].name, name)) return i;
    }
    return -1;
}

static int ramfs_used_files() {
    int n = 0;
    for (int i = 0; i < RAMFS_FILES; i++) if (g_fs[i].used) n++;
    return n;
}

static int ramfs_used_bytes() {
    int n = 0;
    for (int i = 0; i < RAMFS_FILES; i++) if (g_fs[i].used) n += g_fs[i].size;
    return n;
}

// 0 = ok；-1 = 没有空槽；-2 = 名字为空
static int ramfs_write(const char* raw_name, const char* text, int len) {
    ramfs_norm(raw_name, g_pathbuf, RAMFS_NAME_MAX);
    if (!g_pathbuf[0]) return -2;
    int idx = ramfs_find(g_pathbuf);
    if (idx < 0) {
        for (int i = 0; i < RAMFS_FILES; i++) {
            if (!g_fs[i].used) { idx = i; g_fs[i].used = true; g_fs[i].name[0] = 0; break; }
        }
        if (idx < 0) return -1;
        for (int i = 0; i < RAMFS_NAME_MAX; i++) g_fs[idx].name[i] = g_pathbuf[i];
    }
    if (len < 0) len = 0;
    if (len > RAMFS_DATA_MAX) len = RAMFS_DATA_MAX;      // 上限 512 字节：按容量截断
    for (int i = 0; i < len; i++) g_fs[idx].data[i] = text[i];
    g_fs[idx].data[len] = 0;                             // 末尾 NUL（打印时整串用）
    g_fs[idx].size = len;
    return 0;
}

static int ramfs_create(const char* raw_name) {
    ramfs_norm(raw_name, g_pathbuf, RAMFS_NAME_MAX);
    if (!g_pathbuf[0]) return -2;
    if (ramfs_find(g_pathbuf) >= 0) return 1;          // 已存在
    for (int i = 0; i < RAMFS_FILES; i++) {
        if (g_fs[i].used) continue;
        g_fs[i].used = true;
        for (int k = 0; k < RAMFS_NAME_MAX; k++) g_fs[i].name[k] = g_pathbuf[k];
        g_fs[i].size = 0;
        g_fs[i].data[0] = 0;
        return 0;
    }
    return -1;                                          // 无空槽
}

// 0 = ok；-1 = 不存在
static int ramfs_remove(const char* raw_name) {
    ramfs_norm(raw_name, g_pathbuf, RAMFS_NAME_MAX);
    if (!g_pathbuf[0]) return -1;
    int idx = ramfs_find(g_pathbuf);
    if (idx < 0) return -1;
    g_fs[idx].used = false;
    g_fs[idx].name[0] = 0;
    g_fs[idx].size = 0;
    g_fs[idx].data[0] = 0;
    return 0;
}

// 首次使用时建两个说明文件（让 ls / cat 立刻有内容可看）
static void ramfs_init_once() {
    if (g_fs_ready) return;
    g_fs_ready = true;
    for (int i = 0; i < RAMFS_FILES; i++) {
        g_fs[i].used = false;
        g_fs[i].name[0] = 0;
        g_fs[i].size = 0;
        g_fs[i].data[0] = 0;
    }
    const char* readme =
        "Vimtu64 ramfs: 16 slots x 512 bytes, RAM only (lost on reboot).\n"
        "Try: ls / cat readme.txt / write a.txt hello / echo hi > a.txt\n";
    ramfs_write("readme.txt", readme, st_len(readme));
    const char* hello = "hello from VimtuOS 64-bit\n";
    ramfs_write("hello.txt", hello, st_len(hello));
}

// ==================== 绘制 ====================
// 一格：ASCII 走内建位图字体（自带底色），非 ASCII 走 TrueType（只有前景色，底色靠预填）
static void term_draw_cell(int x, int y, uint16_t cp, uint32_t fg, uint32_t bg) {
    if (cp >= 0x20 && cp < 0x7F) {
        fb_draw_char(x, y, (char)cp, fg, bg, TERM_CHAR_SCALE);
    } else if (cp >= 0x20) {
        font_draw_glyph_cp(x, y + TERM_ZH_DY, (uint32_t)cp, fg);
    }
}

static void term_draw(Window* w) {
    TerminalState* ts = ts_of(w);
    int x0 = w->client_x, y0 = w->client_y;
    int cw = w->client_w, chh = w->client_h;
    if (cw < 0) cw = 0;
    if (chh < 0) chh = 0;
    // 客户区（黑底）
    fb_fill_rect(x0, y0, cw, chh, TERM_BG);
    if (!ts) return;
    // 拉边框缩放后按新客户区尺寸重排；打开/最大化动画期间几何是插值出来的，不重排（否则会把 banner 截断）
    if (w->anim_kind == 0) ts_sync_layout(ts, w);
    int fit_c, fit_r;
    ts_fit(w, &fit_c, &fit_r);
    int dc = (ts->cols < fit_c) ? ts->cols : fit_c;      // 越界裁剪：只画客户区真放得下的部分
    int dr = (ts->rows < fit_r) ? ts->rows : fit_r;
    if (dc < 0) dc = 0;
    if (dr < 0) dr = 0;
    for (int r = 0; r < dr; r++) {
        const uint16_t* row = ts->cells + r * TERM_COLS_MAX;
        int ty = y0 + TERM_PAD_Y + r * TERM_CELL_H;
        for (int c = 0; c < dc; c++) {
            uint16_t cp = row[c];
            if (cp >= 0x20) term_draw_cell(x0 + TERM_PAD_X + c * TERM_CELL_W, ty, cp, TERM_FG, TERM_BG);
        }
    }
    // 光标：块状光标（绿底黑字），不依赖 '_' 字形
    if (w->active && ts->cur_r >= 0 && ts->cur_r < dr && ts->cur_c >= 0 && ts->cur_c < dc) {
        int cx = x0 + TERM_PAD_X + ts->cur_c * TERM_CELL_W;
        int cy = y0 + TERM_PAD_Y + ts->cur_r * TERM_CELL_H;
        fb_fill_rect(cx, cy, TERM_CELL_W, TERM_CELL_H, TERM_CUR);
        uint16_t cp = ts->cells[ts->cur_r * TERM_COLS_MAX + ts->cur_c];
        if (cp >= 0x20) term_draw_cell(cx, cy, cp, TERM_CURFG, TERM_CUR);
    }
}

// ==================== Shell：输出与命令 ====================
static void shell_prompt(TerminalState* ts) {
    ts_puts(ts, "vimtu64:~$ ");
}

static void shell_banner(TerminalState* ts) {
    ts_puts(ts, "VimtuOS Terminal v0.1 (64-bit long mode)\n");
    ts_puts(ts, gui64_tr("type 'help' for commands\n", "输入 help 查看命令\n"));
}

static const char* HELP_EN =
    "VimtuOS 64-bit shell commands:\n"
    "  help                  this help\n"
    "  ver, uname            version & architecture\n"
    "  mem, meminfo          memory (page pool / heap / owner)\n"
    "  ps, tasks, top        kernel task table (scheduler task64) + window counts\n"
    "  kill TASKID           terminate a kernel task (ids come from ps)\n"
    "  run NAME|/PATH        load an app from VimtuFS2 and run it in ring3 (magic decides:\\n"
    "                        VAP64 -> int 0x80 path, ELF64 -> syscall path; e.g. run hello.elf)\\n"
    "  elfrun NAME|/PATH     force the ELF64 loader (syscall insn ABI), e.g. elfrun hello.elf\\n"
    "  echo TEXT             print text (echo TEXT > FILE writes a file)\n"
    "  write FILE TEXT       write file (<=512 bytes)\n"
    "  cat FILE / ls         read file / list files\n"
    "  touch FILE / rm FILE  create empty file / delete file\n"
    "  date / time           RTC date / time\n"
    "  uptime                time since boot (ticks/250)\n"
    "  irq                   total interrupt count\n"
    "  perf                  GUI fps / busy% / irq / heap\n"
    "  disp                  display info (real resolution / zoom)\n"
    "  lang [zh|en]          switch Chinese / English\n"
    "  clear                 clear screen\n"
    "  about                 about VimtuOS\n"
    "  reboot                hard restart (8042 -> 0xCF9 -> triple fault)\n"
    "  restart --soft        graceful restart: stop modules + flush session/config, then hard reset\n"
    "  shutdown              power off\n"
    "  set KEY VALUE         setting via config64 (lang / theme / mouse.sens / ... persisted to /store.a|b)\n"
    "  store dump            persistent store: keys/slot/gen/carrier + every key=value (screen + serial)\n"
    "  store get KEY         print KEY=VALUE from the store (or (nil) if absent)\n"
    "  store set KEY VALUE   change the store in memory (run 'store flush' to persist)\n"
    "  store flush           persist to VimtuFS2 /store.a|/store.b (raw-disk fallback if no volume)\n"
    "  ping <ip>             ARP + ICMP echo x3 via e1000/net64 (e.g. ping 10.0.2.2); serial: [NET64] cmd ping\n"
    "  cfg                   config64: type (int/str/bool) + value + default/store source + carrier/slot/gen\n"
    "  cfg get KEY           one config key; cfg set KEY VALUE (or KEY=VALUE); cfg save; cfg reset\n"
    "  syslog                sysstate64 ring log (fixed 64-line circular log) + [SYS64] syslog lines=N\n"
    "  state                 system state machine (BOOT/STARTING/RUNNING/STOPPING/STOPPED) + module table\n"
    "  health                per-module health report ([SYS64] health ok modules=N failed=0)\n"
    "  session               session policy (VOLATILE/PERSIST + per-app keep flags, persisted via config64)\n"
    "  disk, hw, lspci       ATA IDENTIFY (model/capacity) + VimtuFS2 volume; CPU/PCI; PCI device list\n"
    "  user [run]            ring3 status (window/VA/gates/on-disk programs); 'user run' launches /hello.vap\n"
    "  panic <code>, bsod    controlled BSOD: blue screen + serial stop code, halts after 6s (no auto reboot)\n"
    "Not ported yet (prints a reason):\n"
    "  update preload\n";

static const char* HELP_ZH =
    "VimtuOS 64 位 Shell 命令：\n"
    "  help                  本帮助\n"
    "  ver, uname            版本与架构\n"
    "  mem, meminfo          内存（页池 / 堆 / 归属）\n"
    "  ps, tasks, top        内核任务表（调度器 task64）+ 窗口计数\n"
    "  kill TASKID           终止一个内核任务（id 从 ps 拿）\n"
    "  run 名字|/路径        从 VimtuFS2 加载应用并在 ring3 里运行（按文件头魔数自动分派：\\n"
    "                        VAP64 走 int 0x80、ELF64 走 syscall 指令；例如 run hello.elf）\\n"
    "  elfrun 名字|/路径     强制走 ELF64 加载器（syscall 指令 ABI），例如 elfrun hello.elf\\n"
    "  echo TEXT             回显（echo TEXT > FILE 写文件）\n"
    "  write FILE TEXT       写文件（≤512 字节）\n"
    "  cat FILE / ls         读文件 / 列文件\n"
    "  touch FILE / rm FILE  建空文件 / 删文件\n"
    "  date / time           RTC 日期 / 时间\n"
    "  uptime                开机时长（ticks/250）\n"
    "  irq                   中断总数\n"
    "  perf                  GUI 帧率 / 忙占比 / 中断 / 堆\n"
    "  disp                  显示信息（真实分辨率 / 缩放）\n"
    "  lang [zh|en]          中英切换\n"
    "  clear                 清屏\n"
    "  about                 关于 VimtuOS\n"
    "  reboot                硬重启（8042 -> 0xCF9 -> 三重故障）\n"
    "  restart --soft        软重启：优雅停止（停模块 + 会话/配置落盘 flush）后走原有硬复位链\n"
    "  shutdown              关机\n"
    "  set KEY VALUE         经 config64 改设置（lang / theme / mouse.sens ... 落到 /store.a|b）\n"
    "  store dump            设置持久化 store：键数/活动槽/世代号/载体 + 每条 key=value（屏幕 + 串口）\n"
    "  store get KEY         读设置：打印 KEY=VALUE（没有该键打印 (nil)）\n"
    "  store set KEY VALUE   改内存里的设置（要落盘请再敲 store flush）\n"
    "  store flush           落盘到 VimtuFS2 的 /store.a、/store.b（没有卷时才退回裸盘槽区）\n"
    "  ping <ip>             经 e1000/net64 发 ARP + 3 次 ICMP echo（例如 ping 10.0.2.2）；串口打 [NET64] cmd ping\n"
    "  cfg                   config64 配置：类型（int/str/bool）+ 值 + 来源（默认/store）+ 载体/槽/世代号\n"
    "  cfg get KEY           单键查询；cfg set KEY VALUE（或 KEY=VALUE）；cfg save；cfg reset\n"
    "  syslog                sysstate64 的 ring log（固定 64 条循环日志）+ 串口打 [SYS64] syslog lines=N\n"
    "  state                 运行状态机（BOOT/STARTING/RUNNING/STOPPING/STOPPED）+ 模块表\n"
    "  health                各模块健康报告（串口打 [SYS64] health ok modules=N failed=0）\n"
    "  session               会话策略（VOLATILE/PERSIST + 每应用 keep 开关，走 config64 持久化）\n"
    "  disk, hw, lspci       ATA IDENTIFY（型号/容量）+ VimtuFS2 卷；CPU/PCI；PCI 设备列表\n"
    "  user [run]            ring3 现状（用户窗口/VA/门/盘上程序）；user run 直接跑 /hello.vap\n"
    "  panic <code>, bsod    受控蓝屏：蓝底白字屏 + 串口停止码，停留 6 秒后停住（不自动重启）\n"
    "未支持（会说明原因）：\n"
    "  update preload\n";

// 未支持命令 / 未知命令：打印一行明确说明（不静默失败）
static bool shell_unsupported(TerminalState* ts, const char* what, const char* en, const char* zh) {
    ts_puts(ts, what);
    ts_puts(ts, ": ");
    ts_puts(ts, gui64_tr(en, zh));
    ts_putc(ts, (uint32_t)'\n');
    term_log_unsupported(what, en);
    return false;
}

// ---------- 命令实现 ----------
static void cmd_help(TerminalState* ts) {
    ts_puts(ts, gui64_lang_zh() ? HELP_ZH : HELP_EN);
}

static void cmd_ver(TerminalState* ts) {
    ts_puts(ts, "VimtuOS 0.1.0 x86_64 (VimtuOS 64-bit)\n");
    ts_puts(ts, "  arch: x86_64 long mode, 4-level paging, page size ");
    ts_put_u64(ts, (uint64_t)PAGE_SIZE_64);
    ts_puts(ts, " bytes\n");
    ts_puts(ts, "  tick: 250 Hz PIT, ticks=");
    ts_put_u64(ts, (uint64_t)ticks64());
    ts_puts(ts, ", uptime=");
    ts_put_u64(ts, (uint64_t)(ticks64() / PIT_HZ_64));
    ts_puts(ts, " sec\n");
    if (gui64_lang_zh()) ts_puts(ts, "  64 位长模式内核：GDT/IDT/PIC/PIT/RTC + 4 级页表\n");
    else                 ts_puts(ts, "  64-bit long mode kernel: GDT/IDT/PIC/PIT/RTC + 4-level paging\n");
}

static void cmd_mem(TerminalState* ts) {
    uint64_t tk = 0, fk = 0, hk = 0;
    mem_info_64(&tk, &fk, &hk);
    uint64_t ram = mem_total_ram_64();
    uint64_t hu = heap_used_64();
    uint64_t ht = heap_total_64();
    ts_puts(ts, "physical RAM (E820 top): ");
    ts_put_u64(ts, ram / (1024ull * 1024ull));
    ts_puts(ts, " MB (");
    ts_put_u64(ts, ram / 1024ull);
    ts_puts(ts, " KB)\n");
    ts_puts(ts, "page pool:               ");
    ts_put_u64(ts, tk);
    ts_puts(ts, " KB total, ");
    ts_put_u64(ts, fk);
    ts_puts(ts, " KB free (pages ");
    ts_put_u64(ts, page_count_free_64());
    ts_puts(ts, "/");
    ts_put_u64(ts, page_count_total_64());
    ts_puts(ts, " x 4KB)\n");
    ts_puts(ts, "kernel heap:             ");
    ts_put_u64(ts, ht / 1024ull);
    ts_puts(ts, " KB total, ");
    ts_put_u64(ts, hu / 1024ull);
    ts_puts(ts, " KB used\n");
    ts_puts(ts, "terminal states:         ");
    ts_put_u64(ts, mem_owner_bytes_64(MEM_OWNER_TERMINAL_64) / 1024ull);
    ts_puts(ts, " KB (");
    ts_puts(ts, mem_owner_name_64(MEM_OWNER_TERMINAL_64));
    ts_puts(ts, ")\n");
}

struct PsApp { int id; const char* en; const char* zh; };
static const PsApp g_ps_apps[] = {
    { APP_ID_CALC,     "Calculator",  "计算器" },
    { APP_ID_MINES,    "Minesweeper", "扫雷" },
    { APP_ID_TERM,     "Terminal",    "终端" },
    { APP_ID_TMGR,     "TaskMgr",     "任务管理器" },
    { APP_ID_SETTINGS, "Settings",    "设置" },
    { APP_ID_MONITOR,  "Monitor",     "系统监视器" },
    { APP_ID_MYPC,     "MyPC",        "我的电脑" },
    { APP_ID_ABOUT,    "About",       "关于" },
    { APP_ID_RECYCLE,  "Recycle",     "回收站" },
};
static const int g_ps_app_n = (int)(sizeof(g_ps_apps) / sizeof(g_ps_apps[0]));

// ---------- 内核任务表（调度器 task64 的真值；ps / tasks / task / top 共用） ----------

// 任务状态文案：与 task64.h 的 Task64State 一一对应
static const char* task_state_text(uint32_t st) {
    switch (st) {
        case TASK64_READY:   return "ready";
        case TASK64_RUNNING: return "running";
        case TASK64_SLEEP:   return "sleep";
        case TASK64_DEAD:    return "dead";
        default:             return "unknown";
    }
}

// 左对齐补空格（数值列右对齐用；ts_puts_pad 是左对齐版）
static void ts_puts_pad_right(TerminalState* ts, const char* s, int width) {
    int n = st_len(s);
    for (int i = n; i < width; i++) ts_putc(ts, (uint32_t)' ');
    ts_puts(ts, s);
}

static void ts_put_u64_right(TerminalState* ts, uint64_t v, int width) {
    fmt_u64(g_num, (int)sizeof(g_num), v);
    ts_puts_pad_right(ts, g_num, width);
}

// 打点：自动验收 grep 的行（严格照抄：末尾一行，ASCII）
static void task_log_ps(int rows) {
    dbg64_str("[TASK] ps rows=");
    dbg64_dec((uint64_t)rows);
    dbg64_str(" switches=");
    dbg64_dec(task_switch_total64());
    dbg64_nl();
}

// ps / tasks / task / top：真实任务表一次快照（空槽跳过；没有任务数据就如实写"无任务数据"）
static void cmd_ps(TerminalState* ts) {
    ts_puts(ts, gui64_tr("kernel task table (scheduler task64, tick=4ms):\n",
                         "内核任务表（调度器 task64，tick=4ms）：\n"));
    ts_puts(ts, "  id  name            state          ticks  switches\n");
    int rows = 0;
    for (int i = 0; i < TASK64_MAX; i++) {
        Task64Info in;
        if (task_info64(i, &in) == 0) continue;         // 空槽：跳过，不造假行
        ts_puts(ts, "  ");
        ts_put_u64_right(ts, (uint64_t)in.id, 2);
        ts_puts(ts, "  ");
        ts_puts_pad(ts, in.name, 16);
        ts_puts_pad(ts, task_state_text(in.state), 10);
        ts_put_u64_right(ts, in.ticks, 10);
        ts_puts(ts, "  ");
        ts_put_u64_right(ts, in.switches, 8);
        if (in.is_current) ts_puts(ts, "  *");
        ts_putc(ts, (uint32_t)'\n');
        rows++;
    }
    if (rows == 0) ts_puts(ts, gui64_tr("  no task data\n", "  无任务数据\n"));
    ts_puts(ts, gui64_tr("  * = current task; system ticks=", "  * = 当前任务；系统 tick="));
    ts_put_u64(ts, (uint64_t)g_ticks64);
    ts_puts(ts, " (tick=4ms), switches=");
    ts_put_u64(ts, task_switch_total64());
    ts_puts(ts, "\n");

    ts_puts(ts, "windows on desktop: ");
    ts_put_u64(ts, (uint64_t)gui64_window_count());
    ts_puts(ts, "\n");
    for (int i = 0; i < g_ps_app_n; i++) {
        ts_puts(ts, "  ");
        ts_puts_pad(ts, gui64_tr(g_ps_apps[i].en, g_ps_apps[i].zh), 22);
        ts_put_u64(ts, (uint64_t)gui64_app_windows(g_ps_apps[i].id));
        ts_putc(ts, (uint32_t)'\n');
    }

    // 末尾打点（同时进串口日志，供自动验收 grep）
    task_log_ps(rows);
    ts_puts(ts, "[TASK] ps rows=");
    ts_put_u64(ts, (uint64_t)rows);
    ts_puts(ts, " switches=");
    ts_put_u64(ts, task_switch_total64());
    ts_putc(ts, (uint32_t)'\n');
}

static void cmd_date(TerminalState* ts) {
    int y = 0, mo = 0, d = 0, wd = 0;
    rtc_get_date64(&y, &mo, &d, &wd);
    ts_put_u64_pad(ts, (uint64_t)y, 4);
    ts_puts(ts, "-");
    ts_put_u64_pad(ts, (uint64_t)mo, 2);
    ts_puts(ts, "-");
    ts_put_u64_pad(ts, (uint64_t)d, 2);
    ts_puts(ts, " (day ");
    ts_put_i64(ts, (int64_t)wd);
    ts_puts(ts, ")\n");
}

static void cmd_time(TerminalState* ts) {
    int h = 0, m = 0, s = 0;
    rtc_get_time64(&h, &m, &s);
    ts_put_u64_pad(ts, (uint64_t)h, 2);
    ts_puts(ts, ":");
    ts_put_u64_pad(ts, (uint64_t)m, 2);
    ts_puts(ts, ":");
    ts_put_u64_pad(ts, (uint64_t)s, 2);
    ts_putc(ts, (uint32_t)'\n');
}

static void cmd_uptime(TerminalState* ts) {
    uint32_t ticks = ticks64();
    uint32_t sec = ticks / PIT_HZ_64;
    ts_puts(ts, "up ");
    ts_put_u64(ts, (uint64_t)sec);
    ts_puts(ts, " sec (");
    ts_put_u64(ts, (uint64_t)(sec / 3600u));
    ts_puts(ts, ":");
    ts_put_u64_pad(ts, (uint64_t)((sec / 60u) % 60u), 2);
    ts_puts(ts, ":");
    ts_put_u64_pad(ts, (uint64_t)(sec % 60u), 2);
    ts_puts(ts, "), ");
    ts_put_u64(ts, (uint64_t)ticks);
    ts_puts(ts, " ticks\n");
}

static void cmd_irq(TerminalState* ts) {
    ts_puts(ts, "irq total: ");
    ts_put_u64(ts, (uint64_t)g_irq_total64);
    ts_puts(ts, " (PIT 250 Hz, ticks=");
    ts_put_u64(ts, (uint64_t)ticks64());
    ts_puts(ts, ")\n");
}

static void cmd_perf(TerminalState* ts) {
    ts_puts(ts, "fps=");
    ts_put_u64(ts, (uint64_t)gui64_fps());
    ts_puts(ts, " cpu_busy=");
    ts_put_u64(ts, (uint64_t)gui64_cpu_busy_pct());
    ts_puts(ts, "% irq=");
    ts_put_u64(ts, (uint64_t)g_irq_total64);
    ts_puts(ts, " windows=");
    ts_put_u64(ts, (uint64_t)gui64_window_count());
    ts_puts(ts, " heap_used=");
    ts_put_u64(ts, heap_used_64() / 1024ull);
    ts_puts(ts, " KB\n");
}

static void cmd_disp(TerminalState* ts) {
    ts_puts(ts, "display: ");
    ts_put_u64(ts, (uint64_t)fb_width());
    ts_puts(ts, "x");
    ts_put_u64(ts, (uint64_t)fb_height());
    ts_puts(ts, " (physical ");
    ts_put_u64(ts, (uint64_t)fb_phys_width());
    ts_puts(ts, "x");
    ts_put_u64(ts, (uint64_t)fb_phys_height());
    ts_puts(ts, "), zoom ");
    ts_put_u64(ts, (uint64_t)fb_get_zoom());
    ts_puts(ts, "%\n");
    ts_puts(ts, "desktop area: ");
    ts_put_u64(ts, (uint64_t)gui64_screen_w());
    ts_puts(ts, "x");
    ts_put_u64(ts, (uint64_t)gui64_screen_h());
    ts_puts(ts, ", taskbar ");
    ts_put_u64(ts, (uint64_t)gui64_taskbar_h());
    ts_puts(ts, " px, bpp 32\n");
}

static void cmd_ls(TerminalState* ts) {
    ramfs_init_once();
    ts_puts(ts, "ramfs: ");
    ts_put_u64(ts, (uint64_t)ramfs_used_files());
    ts_puts(ts, "/");
    ts_put_u64(ts, (uint64_t)RAMFS_FILES);
    ts_puts(ts, " files, ");
    ts_put_u64(ts, (uint64_t)ramfs_used_bytes());
    ts_puts(ts, "/");
    ts_put_u64(ts, (uint64_t)(RAMFS_FILES * RAMFS_DATA_MAX));
    ts_puts(ts, " bytes used\n");
    for (int i = 0; i < RAMFS_FILES; i++) {
        if (!g_fs[i].used) continue;
        ts_puts(ts, "  ");
        ts_puts_pad(ts, g_fs[i].name, 20);
        ts_put_u64(ts, (uint64_t)g_fs[i].size);
        ts_puts(ts, " B\n");
    }
}

// cat：0 = ok
static bool cmd_cat(TerminalState* ts, const char* name) {
    if (!name || !name[0]) {
        ts_puts(ts, "cat: usage: cat FILE\n");
        return false;
    }
    ramfs_init_once();
    ramfs_norm(name, g_pathbuf, RAMFS_NAME_MAX);
    int idx = ramfs_find(g_pathbuf);
    if (idx < 0) {
        ts_puts(ts, "cat: no such file: ");
        ts_puts(ts, g_pathbuf);
        ts_putc(ts, (uint32_t)'\n');
        return false;
    }
    ts_puts(ts, g_fs[idx].data);
    ts_putc(ts, (uint32_t)'\n');
    return true;
}

// write FILE TEXT
static bool cmd_write(TerminalState* ts, const char* name, const char* text) {
    if (!name || !name[0] || !text || !text[0]) {
        ts_puts(ts, "write: usage: write FILE TEXT\n");
        return false;
    }
    ramfs_init_once();
    int len = st_len(text);
    int rc = ramfs_write(name, text, len);
    if (rc == -2) { ts_puts(ts, "write: empty file name\n"); return false; }
    if (rc == -1) { ts_puts(ts, "write: no free slot (16 files max)\n"); return false; }
    ts_puts(ts, "written to ");
    ts_puts(ts, g_pathbuf);
    ts_puts(ts, " (");
    ts_put_u64(ts, (uint64_t)(ramfs_find(g_pathbuf) >= 0 ? g_fs[ramfs_find(g_pathbuf)].size : 0));
    ts_puts(ts, " bytes");
    if (len > RAMFS_DATA_MAX) ts_puts(ts, ", truncated");
    ts_puts(ts, ")\n");
    return true;
}

static bool cmd_touch(TerminalState* ts, const char* name) {
    if (!name || !name[0]) { ts_puts(ts, "touch: usage: touch FILE\n"); return false; }
    ramfs_init_once();
    int rc = ramfs_create(name);
    if (rc == 0) { ts_puts(ts, "ok, created "); ts_puts(ts, g_pathbuf); ts_putc(ts, (uint32_t)'\n'); return true; }
    if (rc == 1) { ts_puts(ts, "ok, exists "); ts_puts(ts, g_pathbuf); ts_putc(ts, (uint32_t)'\n'); return true; }
    ts_puts(ts, "touch: error (no free slot)\n");
    return false;
}

static bool cmd_rm(TerminalState* ts, const char* name) {
    if (!name || !name[0]) { ts_puts(ts, "rm: usage: rm FILE\n"); return false; }
    ramfs_init_once();
    ramfs_norm(name, g_pathbuf, RAMFS_NAME_MAX);
    if (ramfs_remove(g_pathbuf) != 0) {
        ts_puts(ts, "rm: no such file: ");
        ts_puts(ts, g_pathbuf);
        ts_putc(ts, (uint32_t)'\n');
        return false;
    }
    ts_puts(ts, "removed ");
    ts_puts(ts, g_pathbuf);
    ts_putc(ts, (uint32_t)'\n');
    return true;
}

// echo TEXT | echo TEXT > FILE
static bool cmd_echo(TerminalState* ts, const char* args) {
    int gt = -1;
    for (int i = 0; args && args[i]; i++) if (args[i] == '>') { gt = i; break; }
    if (gt < 0) {
        ts_puts(ts, args ? args : "");
        ts_putc(ts, (uint32_t)'\n');
        return true;
    }
    char text[TERM_CMD_MAX];
    int t = 0;
    int end = gt;
    while (end > 0 && is_ws(args[end - 1])) end--;
    for (int i = 0; i < end && t < TERM_CMD_MAX - 1; i++) text[t++] = args[i];
    text[t] = 0;
    const char* fp = skip_ws(args + gt + 1);
    char path[RAMFS_NAME_MAX];
    grab_token(fp, path, (int)sizeof(path));
    if (!path[0]) { ts_puts(ts, "echo: usage: echo TEXT > FILE\n"); return false; }
    ramfs_init_once();
    int rc = ramfs_write(path, text, t);
    if (rc != 0) { ts_puts(ts, "error: cannot write file\n"); return false; }
    ts_puts(ts, "written to ");
    ts_puts(ts, g_pathbuf);
    ts_putc(ts, (uint32_t)'\n');
    return true;
}

static bool cmd_lang(TerminalState* ts, const char* arg) {
    bool zh = gui64_lang_zh();
    if (!arg || !arg[0]) {
        zh = !zh;                                        // 不带参数 = 切换
    } else if (st_eq(arg, "zh") || st_eq(arg, "cn") || st_eq(arg, "chinese")) {
        zh = true;
    } else if (st_eq(arg, "en") || st_eq(arg, "english")) {
        zh = false;
    } else {
        ts_puts(ts, "lang: usage: lang [zh|en]\n");
        return false;
    }
    gui64_set_lang_zh(zh);
    if (zh) ts_puts(ts, "语言已切换为中文（gui64_lang_zh=1）\n");
    else    ts_puts(ts, "language set to English (gui64_lang_zh=0)\n");
    dbg64_str("[TERM] lang ");
    dbg64_str(zh ? "zh" : "en");
    dbg64_nl();
    return true;
}

static bool cmd_set(TerminalState* ts, const char* key, const char* val) {
    if (!key || !key[0] || !val || !val[0]) {
        ts_puts(ts, "set: usage: set KEY VALUE   (e.g. set lang zh | set theme dark | set mouse.sens 2500)\n");
        return false;
    }
    if (st_eq(key, "lang")) {
        return cmd_lang(ts, val);
    }
    // 本轮起：其它键走 config64 的类型化配置（-> store64 持久化）；`cfg set` 是同一条路。
    const int rc = config64_set_auto64(key, val);
    if (rc != 0) {
        ts_puts(ts, "set: failed (bad key/value; see serial log)\n");
        return false;
    }
    ts_puts(ts, "set "); ts_puts(ts, key); ts_puts(ts, "="); ts_puts(ts, val);
    ts_puts(ts, "  (config64 -> store64; 'store flush' or the 3s autosave writes it)\n");
    return true;
}

// ---------- store：设置持久化（真命令，走 kernel/store64.cpp）----------
//   store dump            屏幕摘要（键数/活动槽/世代号/载体）+ 每条 key=value，同时串口转储
//   store get <key>       有值打印 key=value，没有打印 (nil)
//   store set <key> <v>   只改内存（打印 set ok (in memory, not flushed)），要落盘再敲 store flush
//   store flush           落盘：有 VimtuFS2 卷就写 /store.a|/store.b，没有卷才退回裸盘槽区
// 参数约定：sub = 子命令；key = 子命令后的第 1 个 token；rest = 子命令之后的**整段原文**
//（set 的 value = 去掉 key 那一段之后的剩余，允许带空格的多词 value）。
static const char* store_val_after_key(const char* rest, const char* key) {
    if (!rest) return "";
    if (!key || !key[0]) return rest;
    uint32_t i = 0;
    while (key[i] && rest[i] == key[i]) i++;
    if (key[i] != 0) return rest;                  // rest 不是以 key 开头：原样返回（防御）
    return skip_ws(rest + i);
}
static bool cmd_store(TerminalState* ts, const char* sub, const char* key, const char* rest) {
    if (!sub || !sub[0]) {
        ts_puts(ts, gui64_tr("store: usage: store dump | get <key> | set <key> <value> | flush\n",
                             "store: 用法: store dump | get <key> | set <key> <value> | flush\n"));
        return false;
    }

    if (st_eq(sub, "dump")) {
        dbg64_str("[STORE64] cmd dump keys=");
        dbg64_dec((uint64_t)store64_key_count64());
        dbg64_str(" slot=");
        dbg64_str(store64_slot_name64());
        dbg64_str(" gen=");
        dbg64_dec(store64_generation64());
        dbg64_str(" carrier=");
        dbg64_str(store64_carrier64());
        dbg64_nl();
        store64_dump64();                                  // 串口：每键一行 [STORE64] dump <key>=<value>
        ts_puts(ts, gui64_tr("store dump: keys=", "store 转储：键数="));
        ts_put_u64(ts, (uint64_t)store64_key_count64());
        ts_puts(ts, " slot=");
        ts_puts(ts, store64_slot_name64());
        ts_puts(ts, " gen=");
        ts_put_u64(ts, store64_generation64());
        ts_puts(ts, " carrier=");
        ts_puts(ts, store64_carrier64());
        ts_putc(ts, (uint32_t)'\n');
        // 每条记录也打到屏幕（与串口同一份数据；两块缓冲都在文件级，不放栈上）
        for (int i = 0; i < store64_key_count64(); i++) {
            if (store64_entry64(i, g_store_k, (int)sizeof(g_store_k),
                                   g_store_v, (int)sizeof(g_store_v)) != 0) break;
            ts_puts(ts, "  ");
            ts_puts(ts, g_store_k);
            ts_putc(ts, (uint32_t)'=');
            ts_puts(ts, g_store_v);
            ts_putc(ts, (uint32_t)'\n');
        }
        return true;
    }

    if (st_eq(sub, "get")) {
        if (!key || !key[0]) {
            ts_puts(ts, gui64_tr("store get: usage: store get <key>\n",
                                 "store get: 用法: store get <key>\n"));
            return false;
        }
        const int n = store64_get64(key, g_store_v, (int)sizeof(g_store_v));
        dbg64_str("[STORE64] cmd get key=");
        dbg64_str(key);
        if (n < 0) {
            dbg64_str(" value=(nil)");
            dbg64_nl();
            ts_puts(ts, key);
            ts_puts(ts, "=(nil)\n");
            return true;                                   // 键不存在：如实打印 (nil)，不算命令失败
        }
        dbg64_str(" value=");
        dbg64_str(g_store_v);
        dbg64_str(" len=");
        dbg64_dec((uint64_t)n);
        dbg64_nl();
        ts_puts(ts, key);
        ts_putc(ts, (uint32_t)'=');
        ts_puts(ts, g_store_v);
        ts_putc(ts, (uint32_t)'\n');
        return true;
    }

    if (st_eq(sub, "set")) {
        const char* v = store_val_after_key(rest, key);
        if (!key || !key[0] || !v[0]) {
            ts_puts(ts, gui64_tr("store set: usage: store set <key> <value>\n",
                                 "store set: 用法: store set <key> <value>\n"));
            return false;
        }
        const int rc = store64_set64(key, v);
        dbg64_str("[STORE64] cmd set key=");
        dbg64_str(key);
        dbg64_str(" value=");
        dbg64_str(v);
        dbg64_str(" rc=");
        if (rc < 0) dbg64_putc('-');
        dbg64_dec((uint64_t)(rc < 0 ? -rc : rc));
        dbg64_nl();
        if (rc != 0) {
            ts_puts(ts, gui64_tr("store set: failed (bad key/value or store full; see serial log)\n",
                                 "store set: 失败（key/value 非法或 store 已满，见串口日志）\n"));
            return false;
        }
        ts_puts(ts, gui64_tr("set ok (in memory, not flushed)\n",
                             "已写入内存（尚未落盘，用 store flush 保存）\n"));
        return true;
    }

    if (st_eq(sub, "flush")) {
        const int rc = store64_flush64();
        dbg64_str("[STORE64] cmd flush rc=");
        if (rc < 0) dbg64_putc('-');
        dbg64_dec((uint64_t)(rc < 0 ? -rc : rc));
        dbg64_str(" carrier=");
        dbg64_str(store64_carrier64());
        dbg64_str(" slot=");
        dbg64_str(store64_slot_name64());
        dbg64_str(" gen=");
        dbg64_dec(store64_generation64());
        dbg64_nl();
        if (rc != 0) {
            ts_puts(ts, gui64_tr("store flush: failed (see serial log)\n",
                                 "store flush: 落盘失败（见串口日志）\n"));
            return false;
        }
        ts_puts(ts, gui64_tr("store flush: ok\n", "store flush: 落盘成功\n"));
        return true;
    }

    ts_puts(ts, gui64_tr("store: unknown subcommand: ", "store: 未知子命令："));
    ts_puts(ts, sub);
    ts_puts(ts, gui64_tr("\n  try: store dump | get <key> | set <key> <value> | flush\n",
                         "\n  试试: store dump | get <key> | set <key> <value> | flush\n"));
    return false;
}

// ---------- ping：网络诊断（真走 kernel/net64.cpp 的 ARP/ICMP 路径）----------
// 解析点分十进制 IPv4（每段 0..255）；和 net64 的编码一致：uint32 按内存小端装 4 字节。
static bool parse_ipv4_dotted(const char* s, uint32_t* out) {
    if (!s || !out) return false;
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return false;
        int n = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (int)(*s - '0');
            if (n > 255) return false;
            s++;
        }
        v |= ((uint32_t)n) << (part * 8);
        if (part < 3) {
            if (*s != '.') return false;
            s++;
        }
    }
    if (*s != 0) return false;                 // 末尾还有多余字符（例如 1.2.3.4.5）
    *out = v;
    return true;
}

static void ip4_to_str(char* out, int cap, uint32_t ip) {
    int n = 0;
    for (int i = 0; i < 4 && n < cap - 1; i++) {
        char seg[8];
        fmt_u64(seg, (int)sizeof(seg), (ip >> (i * 8)) & 0xFFu);
        if (i) out[n++] = '.';
        for (int k = 0; seg[k] && n < cap - 1; k++) out[n++] = seg[k];
    }
    out[n] = 0;
}

// ping <ip>：ARP 解析（net64_ping64 内部有界等待）+ 3 次 ICMP echo；
// 屏幕与串口都打 "[NET64] cmd ping <ip> replies=<n> lost=<n>"（自动验收 grep 串口）。
static bool cmd_ping(TerminalState* ts, const char* arg) {
    if (!arg || !arg[0]) {
        ts_puts(ts, gui64_tr("ping: usage: ping <ip>   (e.g. ping 10.0.2.2)\n",
                             "ping: 用法: ping <ip>   （例如 ping 10.0.2.2）\n"));
        return false;
    }
    uint32_t ip = 0;
    if (!parse_ipv4_dotted(arg, &ip)) {
        ts_puts(ts, gui64_tr("ping: bad IPv4 address: ", "ping: IPv4 地址非法："));
        ts_puts(ts, arg);
        ts_putc(ts, (uint32_t)'\n');
        return false;
    }

    // e1000/net64 没起来（没网卡/启动探测失败）：如实说明，不再空等
    const bool up = st_eq(net64_state_str64(), "up");
    if (!up) {
        ts_puts(ts, gui64_tr("ping: net64 not up (no e1000 link or boot probe failed; see serial log)\n",
                             "ping: net64 未就绪（没有 e1000 链路或启动探测失败；见串口日志）\n"));
    }

    // 看门狗停表：一次 ping 最坏是 ARP 2s + 3×ICMP 2s = 8s，会超过 5s 心跳阈值（不是 bug，是长操作）。
    panic64_watchdog_pause64();
    int replies = 0;
    int sent = 0;
    for (uint16_t seq = 1; seq <= 3 && up; seq++) {
        const int rc = net64_ping64(ip, seq);
        sent++;
        if (rc == 0) { replies++; continue; }
        if (rc == -1 || rc == -2) break;                        // 链路/ARP 问题：不再重复等
        if (rc == -3 && replies == 0 && sent >= 2) break;       // 连续超时：不再耗更多时间
    }
    const int lost = 3 - replies;

    char ipstr[20];
    ip4_to_str(ipstr, (int)sizeof(ipstr), ip);
    ts_puts(ts, "[NET64] cmd ping ");
    ts_puts(ts, ipstr);
    ts_puts(ts, " replies=");
    ts_put_u64(ts, (uint64_t)replies);
    ts_puts(ts, " lost=");
    ts_put_u64(ts, (uint64_t)lost);
    ts_putc(ts, (uint32_t)'\n');
    dbg64_line_begin64();
    dbg64_str("[NET64] cmd ping ");
    dbg64_str(ipstr);
    dbg64_str(" replies=");
    dbg64_dec((uint64_t)replies);
    dbg64_str(" lost=");
    dbg64_dec((uint64_t)lost);
    dbg64_nl();
    dbg64_line_end64();
    panic64_watchdog_unpause64();
    return replies > 0;
}

// ==================== 本轮接真的命令（hw / lspci / disk / user / cfg / syslog / state / health / session / panic）====================
// 这些命令原来都是 shell_unsupported 的"尚未支持"桩；现在走各子系统的真 API，输出全是实测值。

// 屏幕上打 0x + 定长十六进制（digits = 4/8/16）
static void ts_put_hex(TerminalState* ts, uint64_t v, int digits) {
    static const char* H = "0123456789ABCDEF";
    char t[20];
    if (digits <= 0 || digits > 16) digits = 16;
    for (int i = 0; i < digits; i++) t[i] = H[(v >> ((digits - 1 - i) * 4)) & 0xF];
    t[digits] = 0;
    ts_puts(ts, "0x");
    ts_puts(ts, t);
}

// ---------- hw / hwinfo：硬件清单（hwinfo64 的 CPUID + PCI 枚举结果）----------
static void cmd_hw(TerminalState* ts) {
    const HwInfo64* hw = hw_info64();
    if (hw->magic != HW64_INFO_MAGIC) {
        ts_puts(ts, "hw: hwinfo64 not initialized (no CPUID/PCI data)\n");
        return;
    }
    ts_puts(ts, "CPU (CPUID, kernel/hwinfo64.cpp):\n");
    ts_puts(ts, "  vendor : "); ts_puts(ts, hw->cpu.vendor[0] ? hw->cpu.vendor : "none"); ts_putc(ts, (uint32_t)'\n');
    ts_puts(ts, "  brand  : "); ts_puts(ts, hw->cpu.brand[0] ? hw->cpu.brand : "none"); ts_putc(ts, (uint32_t)'\n');
    ts_puts(ts, "  family="); ts_put_u64(ts, hw->cpu.family);
    ts_puts(ts, " model="); ts_put_u64(ts, hw->cpu.model);
    ts_puts(ts, " stepping="); ts_put_u64(ts, hw->cpu.stepping);
    ts_puts(ts, " cores="); ts_put_u64(ts, hw->cpu.cores);
    ts_putc(ts, (uint32_t)'\n');
    ts_puts(ts, "  hypervisor: ");
    ts_puts(ts, hw->cpu.hypervisor[0] ? hw->cpu.hypervisor : "none");
    ts_putc(ts, (uint32_t)'\n');
    ts_puts(ts, "  features: lm="); ts_put_u64(ts, hw->cpu.lm ? 1 : 0);
    ts_puts(ts, " pae="); ts_put_u64(ts, hw->cpu.pae ? 1 : 0);
    ts_puts(ts, " nx="); ts_put_u64(ts, hw->cpu.nx ? 1 : 0);
    ts_puts(ts, " sse2="); ts_put_u64(ts, hw->cpu.sse2 ? 1 : 0);
    ts_puts(ts, " avx="); ts_put_u64(ts, hw->cpu.avx ? 1 : 0);
    ts_puts(ts, " vmx="); ts_put_u64(ts, hw->cpu.vmx ? 1 : 0);
    ts_puts(ts, " smep="); ts_put_u64(ts, hw->cpu.smep ? 1 : 0);
    ts_puts(ts, " smp="); ts_put_u64(ts, hw->cpu.smp ? 1 : 0);
    ts_putc(ts, (uint32_t)'\n');
    ts_puts(ts, "PCI (read-only config space enumeration):\n");
    ts_puts(ts, "  devices="); ts_put_u64(ts, hw->pci.count);
    ts_puts(ts, " scanned="); ts_put_u64(ts, hw->pci.scanned);
    ts_puts(ts, " bus_max="); ts_put_u64(ts, hw->pci.bus_max);
    ts_puts(ts, " ide="); ts_put_u64(ts, hw->pci.ide);
    ts_puts(ts, " storage="); ts_put_u64(ts, hw->pci.storage);
    ts_puts(ts, " net="); ts_put_u64(ts, hw->pci.net);
    ts_puts(ts, " vga="); ts_put_u64(ts, hw->pci.vga);
    ts_putc(ts, (uint32_t)'\n');
    ts_puts(ts, "Source: [HW64] serial lines carry the same data\n");
    dbg64_line_begin64();
    dbg64_str("[TERM] hw cpu=");
    dbg64_str(hw->cpu.vendor[0] ? hw->cpu.vendor : "none");
    dbg64_str(" cores=");
    dbg64_dec((uint64_t)hw->cpu.cores);
    dbg64_str(" pci=");
    dbg64_dec((uint64_t)hw->pci.scanned);
    dbg64_str(" listed=");
    dbg64_dec((uint64_t)hw->pci.count);
    dbg64_nl();
    dbg64_line_end64();
}

// ---------- lspci / pci：PCI 设备列表（真实枚举表，表容量 HW64_PCI_MAX = 32）----------
static void cmd_lspci(TerminalState* ts) {
    const HwInfo64* hw = hw_info64();
    if (hw->magic != HW64_INFO_MAGIC) {
        ts_puts(ts, "lspci: hwinfo64 not initialized (no PCI data)\n");
        return;
    }
    ts_puts(ts, "PCI devices (bus:dev.fn  vendor:device  class:subclass  rev):\n");
    for (uint32_t i = 0; i < hw->pci.count; i++) {
        const HwPciDev64* d = &hw->pci.devs[i];
        ts_puts(ts, "  ");
        ts_put_u64(ts, d->bus); ts_putc(ts, (uint32_t)':');
        ts_put_u64(ts, d->dev); ts_putc(ts, (uint32_t)'.');
        ts_put_u64(ts, d->fn);
        ts_puts(ts, "  ");
        ts_put_hex(ts, d->vendor, 4); ts_putc(ts, (uint32_t)':');
        ts_put_hex(ts, d->device, 4);
        ts_puts(ts, "  class ");
        ts_put_u64(ts, d->class_code); ts_putc(ts, (uint32_t)':');
        ts_put_u64(ts, d->subclass);
        ts_puts(ts, " rev ");
        ts_put_u64(ts, d->rev);
        ts_putc(ts, (uint32_t)'\n');
    }
    ts_puts(ts, "  scanned="); ts_put_u64(ts, hw->pci.scanned);
    ts_puts(ts, " listed="); ts_put_u64(ts, hw->pci.count);
    ts_puts(ts, "  (class counts: ide=");
    ts_put_u64(ts, hw->pci.ide);
    ts_puts(ts, " storage="); ts_put_u64(ts, hw->pci.storage);
    ts_puts(ts, " net="); ts_put_u64(ts, hw->pci.net);
    ts_puts(ts, " vga="); ts_put_u64(ts, hw->pci.vga);
    ts_puts(ts, ")\n");
    dbg64_line_begin64();
    dbg64_str("[TERM] lspci scanned=");
    dbg64_dec((uint64_t)hw->pci.scanned);
    dbg64_str(" listed=");
    dbg64_dec((uint64_t)hw->pci.count);
    dbg64_str(" ide=");
    dbg64_dec((uint64_t)hw->pci.ide);
    dbg64_str(" storage=");
    dbg64_dec((uint64_t)hw->pci.storage);
    dbg64_str(" net=");
    dbg64_dec((uint64_t)hw->pci.net);
    dbg64_str(" vga=");
    dbg64_dec((uint64_t)hw->pci.vga);
    dbg64_nl();
    dbg64_line_end64();
}

// ---------- disk / ata：ATA IDENTIFY（型号/容量）+ VimtuFS2 卷几何 ----------
static bool cmd_disk(TerminalState* ts) {
    ts_puts(ts, "ATA devices (IDENTIFY via kernel/ata64.cpp; PIO with IRQ14 wait + polling fallback):\n");
    int present_n = 0;
    for (int d = 0; d < 4; d++) {
        DiskInfo di;
        if (!ata64_identify(d, &di) || !di.present) {
            ts_puts(ts, "  ata"); ts_put_u64(ts, (uint64_t)d); ts_puts(ts, ": none\n");
            continue;
        }
        present_n++;
        ts_puts(ts, "  ata"); ts_put_u64(ts, (uint64_t)d);
        ts_puts(ts, (d == 0) ? " (primary master)" : (d == 1 ? " (primary slave)"
                        : (d == 2 ? " (secondary master)" : " (secondary slave)")));
        ts_puts(ts, ": ");
        ts_puts(ts, di.atapi ? "[ATAPI] " : "[ATA] ");
        ts_puts(ts, di.model[0] ? di.model : "(no model string)");
        ts_puts(ts, "  sectors=");
        ts_put_u64(ts, di.sectors);
        ts_puts(ts, " (");
        ts_put_u64(ts, di.sectors / 2048);
        ts_puts(ts, " MB)\n");
    }
    Fs64Info fs;
    const int rc = sysstate64_fsinfo64(&fs);
    if (rc == 0) {
        ts_puts(ts, "VimtuFS2 volume @ LBA ");
        ts_put_u64(ts, fs.pb_lba);
        ts_puts(ts, ":\n  blocks=");
        ts_put_u64(ts, fs.total_blocks);
        ts_puts(ts, "  free=");
        ts_put_u64(ts, fs.free_blocks);
        ts_puts(ts, "  inodes=");
        ts_put_u64(ts, fs.inodes);
        ts_puts(ts, "  files=");
        ts_put_u64(ts, fs.files);
        ts_puts(ts, "  used=");
        ts_put_u64(ts, fs.used_bytes);
        ts_puts(ts, " B\n");
    } else {
        ts_puts(ts, "VimtuFS2 volume: none (no partition table / mount failed) at LBA ");
        ts_put_u64(ts, fs.pb_lba);
        ts_putc(ts, (uint32_t)'\n');
    }
    dbg64_line_begin64();
    dbg64_str("[TERM] disk ata_present=");
    dbg64_dec((uint64_t)present_n);
    dbg64_str(" vfs=");
    dbg64_dec((uint64_t)(rc == 0 ? 1 : 0));
    if (rc == 0) {
        dbg64_str(" blocks=");
        dbg64_dec((uint64_t)fs.total_blocks);
        dbg64_str(" free=");
        dbg64_dec((uint64_t)fs.free_blocks);
        dbg64_str(" files=");
        dbg64_dec((uint64_t)fs.files);
    }
    dbg64_nl();
    dbg64_line_end64();
    return true;    // 探测跑到了就算成功；"没有卷"是如实结果（不是命令失败）
}

// ---------- user / userprog：ring3 用户态现状（arg = "run" 时跑一次用户程序）----------
static bool cmd_user(TerminalState* ts, const char* arg) {
    ts_puts(ts, "ring3 user mode (kernel/usermode64.cpp + syscall64.cpp):\n");
    ts_puts(ts, "  user window : 4GiB .. 4GiB+1MiB\n");
    ts_puts(ts, "  code entry  : "); ts_put_hex(ts, USER64_CODE_VA64, 16); ts_putc(ts, (uint32_t)'\n');
    ts_puts(ts, "  user stack  : "); ts_put_hex(ts, USER64_STACK_VA64, 16);
    ts_puts(ts, "  (16KiB)\n");
    ts_puts(ts, "  brk region  : "); ts_put_hex(ts, USER64_BRK_VA64, 16);
    ts_puts(ts, "  (64KiB)\n");
    ts_puts(ts, "  mmap region : "); ts_put_hex(ts, USER64_MMAP_VA64, 16); ts_putc(ts, (uint32_t)'\n');
    ts_puts(ts, "  code page mapped right now: ");
    ts_puts(ts, user64_page_is_user_ok64(USER64_CODE_VA64) ? "yes\n" : "no (no user program running)\n");
    ts_puts(ts, "  gates: int 0x80 (own ABI) + syscall insn (Linux ABI); both self-tested at boot\n");
    uint32_t ty = 0, sz = 0;
    ts_puts(ts, "  on-disk programs (VimtuFS2):\n");
    if (vfs64_stat("/hello.vap", &ty, &sz) == 0) {
        ts_puts(ts, "    /hello.vap  "); ts_put_u64(ts, sz);
        ts_puts(ts, " B (VAP64, int 0x80 ABI)\n");
    } else {
        ts_puts(ts, "    /hello.vap  (not installed)\n");
    }
    if (vfs64_stat("/hello.elf", &ty, &sz) == 0) {
        ts_puts(ts, "    /hello.elf  "); ts_put_u64(ts, sz);
        ts_puts(ts, " B (ELF64, syscall ABI)\n");
    } else {
        ts_puts(ts, "    /hello.elf  (not installed)\n");
    }
    bool ran = false;
    int rc = 0;
    if (arg && st_eq(arg, "run")) {
        ts_puts(ts, "  running user program: /hello.vap ...\n");
        gui64_flip_window(ts->win);
        rc = app64_run_any64("/hello.vap");
        ran = true;
        ts_puts(ts, "  run rc=");
        ts_put_i64(ts, (int64_t)rc);
        ts_putc(ts, (uint32_t)'\n');
    } else {
        ts_puts(ts, "  (use 'user run' to launch /hello.vap in ring3)\n");
    }
    dbg64_line_begin64();
    dbg64_str("[TERM] user ring3 mapped=");
    dbg64_dec((uint64_t)(user64_page_is_user_ok64(USER64_CODE_VA64) ? 1 : 0));
    dbg64_str(" ran=");
    dbg64_dec(ran ? 1 : 0);
    dbg64_str(" rc=");
    dbg64_dec((uint64_t)(rc < 0 ? -rc : rc));
    dbg64_nl();
    dbg64_line_end64();
    return true;
}

// ---------- cfg / config：config64 的配置 + 存储位置 ----------
// 支持：cfg | cfg get KEY | cfg set KEY VALUE | cfg set KEY=VALUE | cfg save | cfg reset
static bool cmd_cfg(TerminalState* ts, const char* sub, const char* arg1, const char* rest) {
    static char cbuf[2048];
    if (!sub || !sub[0]) {
        config64_format64(cbuf, (int)sizeof(cbuf));
        ts_puts(ts, cbuf);
        ts_puts(ts, "storage: store64 carrier=");
        ts_puts(ts, config64_carrier64());
        ts_puts(ts, " slot=");
        ts_puts(ts, config64_slot64());
        ts_puts(ts, " keys=");
        ts_put_u64(ts, (uint64_t)store64_key_count64());
        ts_puts(ts, " gen=");
        ts_put_u64(ts, store64_generation64());
        ts_puts(ts, " pending=");
        ts_put_u64(ts, (uint64_t)config64_pending64());
        ts_putc(ts, (uint32_t)'\n');
        ts_puts(ts, "  carrier=vfs means the values really live in VimtuFS2 /store.a|/store.b\n");
        dbg64_line_begin64();
        dbg64_str("[CONF64] cmd dump keys=");
        dbg64_dec((uint64_t)config64_count64());
        dbg64_str(" carrier=");
        dbg64_str(config64_carrier64());
        dbg64_str(" pending=");
        dbg64_dec((uint64_t)config64_pending64());
        dbg64_nl();
        dbg64_line_end64();
        return true;
    }
    if (st_eq(sub, "get")) {
        if (!arg1 || !arg1[0]) {
            ts_puts(ts, "cfg get: usage: cfg get <key>\n");
            return false;
        }
        char k[CFG64_KEY_MAX], v[CFG64_STR_MAX];
        int type = 0, from = 0;
        const int n = config64_count64();
        for (int i = 0; i < n; i++) {
            if (config64_entry64(i, k, (int)sizeof(k), v, (int)sizeof(v), &type, &from) != 0) break;
            if (!st_eq(k, arg1)) continue;
            ts_puts(ts, k); ts_puts(ts, " = "); ts_puts(ts, v);
            ts_puts(ts, "  ["); ts_puts(ts, config64_type_name64(type));
            ts_puts(ts, from ? ",store]" : ",default]");
            ts_putc(ts, (uint32_t)'\n');
            dbg64_line_begin64();
            dbg64_str("[CONF64] cmd get key=");
            dbg64_str(arg1);
            dbg64_str(" value=");
            dbg64_str(v);
            dbg64_str(" from=");
            dbg64_str(from ? "store" : "default");
            dbg64_nl();
            dbg64_line_end64();
            return true;
        }
        ts_puts(ts, "cfg get: no such key: "); ts_puts(ts, arg1); ts_putc(ts, (uint32_t)'\n');
        dbg64_line_begin64();
        dbg64_str("[CONF64] cmd get key=");
        dbg64_str(arg1);
        dbg64_str(" value=(nil)");
        dbg64_nl();
        dbg64_line_end64();
        return true;
    }
    if (st_eq(sub, "set")) {
        // 两种写法都收：`cfg set KEY VALUE` 与 `cfg set KEY=VALUE`
        char key[CFG64_KEY_MAX];
        char val[CFG64_STR_MAX];
        int kn = 0, vn = 0;
        const char* s = arg1 ? arg1 : "";
        while (s[kn] && s[kn] != '=' && kn < (int)sizeof(key) - 1) { key[kn] = s[kn]; kn++; }
        key[kn] = 0;
        if (s[kn] == '=') {
            const char* p = s + kn + 1;
            while (*p && vn < (int)sizeof(val) - 1) val[vn++] = *p++;
        } else {
            const char* p = rest ? rest : "";
            while (*p == ' ') p++;
            while (*p && vn < (int)sizeof(val) - 1) val[vn++] = *p++;
        }
        val[vn] = 0;
        if (!key[0] || !val[0]) {
            ts_puts(ts, "cfg set: usage: cfg set <key> <value>   (or: cfg set <key>=<value>)\n");
            return false;
        }
        const int rc = config64_set_auto64(key, val);
        if (rc != 0) {
            ts_puts(ts, "cfg set: failed (bad key/value; see serial log)\n");
            return false;
        }
        ts_puts(ts, "set "); ts_puts(ts, key); ts_puts(ts, "="); ts_puts(ts, val);
        ts_puts(ts, "  (in memory; 'store flush' or the 3s autosave writes /store.a|b)\n");
        return true;
    }
    if (st_eq(sub, "save") || st_eq(sub, "flush")) {
        const int rc = config64_flush64();
        if (rc != 0) { ts_puts(ts, "cfg save: failed (see serial log)\n"); return false; }
        ts_puts(ts, "cfg save: ok via "); ts_puts(ts, config64_carrier64());
        ts_puts(ts, " slot="); ts_puts(ts, config64_slot64()); ts_putc(ts, (uint32_t)'\n');
        return true;
    }
    if (st_eq(sub, "reset")) {
        config64_factory_reset64();
        const int rc = config64_flush64();
        ts_puts(ts, "cfg reset: defaults restored and flushed (rc=");
        ts_put_i64(ts, (int64_t)rc);
        ts_puts(ts, ")\n");
        return true;
    }
    ts_puts(ts, "cfg: unknown subcommand: "); ts_puts(ts, sub);
    ts_puts(ts, "\n  try: cfg | cfg get <key> | cfg set <key> <value> | cfg save | cfg reset\n");
    return false;
}

// ---------- syslog：sysstate64 的 ring log（固定条数循环日志）----------
static void cmd_syslog(TerminalState* ts) {
    const int n = sysstate64_dump_syslog64(24);   // 串口：syslog 行数 + 最近 24 行
    ts_puts(ts, "syslog: lines=");
    ts_put_u64(ts, (uint64_t)n);
    ts_puts(ts, " (ring cap ");
    ts_put_u64(ts, (uint64_t)SYS64_LOG_RING);
    ts_puts(ts, ", newest first)\n");
    int shown = 0;
    for (int i = 0; i < n && shown < 24; i++, shown++) {
        ts_puts(ts, "  ");
        ts_puts(ts, sys64_log_line64(i));
        ts_putc(ts, (uint32_t)'\n');
    }
    if (n > 24) ts_puts(ts, "  ... (older lines are in the serial log)\n");
}

// ---------- state：状态机 + 模块表 ----------
static void cmd_state(TerminalState* ts) {
    static char sbuf[2048];
    sysstate64_report64(sbuf, (int)sizeof(sbuf));
    ts_puts(ts, sbuf);
    ts_puts(ts, "[SYS64] state=");
    ts_puts(ts, sysstate64_state_text64());
    ts_puts(ts, " gen=");
    ts_put_u64(ts, (uint64_t)sysstate64_generation64());
    ts_puts(ts, " modules=");
    ts_put_u64(ts, (uint64_t)sysstate64_module_count64());
    ts_putc(ts, (uint32_t)'\n');
}

// ---------- health：健康报告（真值；串口同时打 [SYS64] health ...）----------
static void cmd_health(TerminalState* ts) {
    static char hbuf[512];
    (void)sysstate64_health_report64(1, hbuf, (int)sizeof(hbuf));
    ts_puts(ts, hbuf);
    ts_puts(ts, "  per-module (state / health):\n");
    const int n = sysstate64_module_count64();
    for (int i = 0; i < n; i++) {
        const int mh = sysstate64_module_health64(i);
        ts_puts(ts, "    ");
        ts_puts_pad(ts, sysstate64_module_name64(i), 12);
        ts_puts(ts, " ");
        ts_puts_pad(ts, sysstate64_module_state_text64(sysstate64_module_state64(i)), 9);
        ts_puts(ts, " ");
        ts_puts(ts, mh == SYS64_MODH_UP ? "UP" : (mh == SYS64_MODH_DEGRADED ? "DEGRADED"
                       : (mh == SYS64_MODH_UNKNOWN ? "unknown" : "DOWN")));
        ts_putc(ts, (uint32_t)'\n');
    }
}

// ---------- session：会话 / 应用内容策略 ----------
// 支持：session | session persist | session volatile | session keep <app_id> on|off
static bool cmd_session(TerminalState* ts, const char* sub, const char* arg1, const char* rest) {
    static char rbuf[1536];
    if (sub && sub[0] && st_eq(sub, "persist")) {
        session64_set_mode64(SESS64_PERSIST);
        ts_puts(ts, "session policy = PERSIST (closing/stopping keeps app state; next boot reopens the saved list)\n");
        return true;
    }
    if (sub && sub[0] && st_eq(sub, "volatile")) {
        session64_set_mode64(SESS64_VOLATILE);
        ts_puts(ts, "session policy = VOLATILE (default: closing a window drops that app's state)\n");
        return true;
    }
    if (sub && sub[0] && st_eq(sub, "keep")) {
        const int id = parse_dec(arg1 ? arg1 : "");
        if (id < 0 || !session64_app_ok64(id)) {
            ts_puts(ts, "session keep: usage: session keep <app_id> on|off   (ids come from 'session')\n");
            return false;
        }
        const bool on = (rest && (st_eq(rest, "on") || st_eq(rest, "1") || st_eq(rest, "true")));
        session64_set_app_keep64(id, on);
        ts_puts(ts, "session keep "); ts_puts(ts, session64_app_name64(id));
        ts_puts(ts, on ? " = on\n" : " = off\n");
        return true;
    }
    if (sub && sub[0]) {
        ts_puts(ts, "session: unknown subcommand: "); ts_puts(ts, sub);
        ts_puts(ts, "\n  try: session | session persist | session volatile | session keep <app_id> on|off\n");
        return false;
    }
    session64_report64(rbuf, (int)sizeof(rbuf));
    ts_puts(ts, rbuf);
    ts_puts(ts, "SESS64 policy=");
    ts_puts(ts, session64_mode_name64(session64_mode64()));
    ts_puts(ts, " apps=");
    ts_put_u64(ts, (uint64_t)session64_app_count64());
    ts_puts(ts, " open=");
    ts_put_u64(ts, (uint64_t)session64_open_count64());
    ts_puts(ts, " resets=");
    ts_put_u64(ts, (uint64_t)session64_reset_count64());
    ts_putc(ts, (uint32_t)'\n');
    return true;
}

// ---------- panic / bsod：受控蓝屏（画屏 + 串口现场 + 停留 6 秒后停住，不自动重启）----------
static void cmd_panic(TerminalState* ts, const char* code) {
    char c[28];
    int n = 0;
    const char* src = (code && code[0]) ? code : "USER_DEMO";
    for (int i = 0; src[i] && i < 27; i++) {
        char ch = src[i];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_') c[n++] = ch;
    }
    c[n] = 0;
    if (n == 0) { const char* d = "USER_DEMO"; for (int i = 0; d[i]; i++) c[n++] = d[i]; c[n] = 0; }
    ts_puts(ts, "entering controlled BSOD: stop=");
    ts_puts(ts, c);
    ts_puts(ts, "  hold=6s then halt (power cycle required; no auto reboot)\n");
    gui64_flip_window(ts->win);
    panic64_controlled64(c, 6000);
}

static void cmd_about(TerminalState* ts) {
    ts_puts(ts,
        "VimtuOS 0.1.0 (VimtuOS 64-bit)\n"
        "  - 64-bit x86 kernel, long mode, hand-written GDT/IDT/PIC/PIT/RTC\n"
        "  - 4-level paging (4KB pages), kernel heap + physical page pool\n"
        "  - drivers: PS/2 keyboard + mouse, VBE LFB framebuffer, TrueType fonts\n"
        "  - desktop shell: windows, apps, taskbar, dirty-rect flips\n"
        "  - scheduler: kernel tasks (task64), used by ps/tasks/kill and the task manager\n"
        "  - terminal: character grid + built-in shell + 16x512B ramfs (RAM only; VimtuFS2 is on disk)\n"
        "  - real filesystem: VimtuFS2 (vfs64), settings persisted by store64 in /store.a|/store.b\n");
    ts_puts(ts, gui64_tr("  - sysstate64: state machine + module registry + health + 64-line ring log (syslog)\n"
                         "  - config64/session64: typed config + session policy, persisted to VimtuFS2 /store.a|b\n"
                         "  - panic64: blue screen (panic/bsod) + watchdog on the gui64 frame heartbeat\n"
                         "  - still not ported: 'update', 'preload' (kept as honest stubs)\n",
                         "  - sysstate64：状态机 + 模块注册表 + 健康报告 + 64 条 ring log（syslog）\n"
                         "  - config64/session64：类型化配置 + 会话策略，持久化在 VimtuFS2 的 /store.a|b\n"
                         "  - panic64：蓝屏（panic/bsod）+ 看门狗（心跳源 = gui64 帧）\n"
                         "  - 仍未移植：update、preload（保留为如实桩）\n"));
}

static void cmd_clear(TerminalState* ts) {
    ts_clear(ts);
    ts_dirty_client(ts);
    gui64_invalidate_window(ts->win);
}

// 命令分发：cmd / arg1 / args（原文，供 echo / write 用）
static void shell_exec(TerminalState* ts, const char* line) {
    const char* p = skip_ws(line);
    p = grab_token(p, g_cmd, (int)sizeof(g_cmd));
    const char* args = skip_ws(p);
    const char* after1 = grab_token(args, g_arg1, (int)sizeof(g_arg1));
    const char* args2 = skip_ws(after1);
    grab_token(args2, g_arg2, (int)sizeof(g_arg2));

    if (g_cmd[0] == 0) {          // 空行：只换行
        ts_putc(ts, (uint32_t)'\n');
        return;
    }

    bool ok = true;
    bool logged = false;

    if (st_eq(g_cmd, "help")) {
        cmd_help(ts);
    } else if (st_eq(g_cmd, "ver") || st_eq(g_cmd, "uname")) {
        cmd_ver(ts);
    } else if (st_eq(g_cmd, "mem") || st_eq(g_cmd, "meminfo")) {
        cmd_mem(ts);
    } else if (st_eq(g_cmd, "ps")) {
        cmd_ps(ts);
    } else if (st_eq(g_cmd, "date")) {
        cmd_date(ts);
    } else if (st_eq(g_cmd, "time")) {
        cmd_time(ts);
    } else if (st_eq(g_cmd, "uptime")) {
        cmd_uptime(ts);
    } else if (st_eq(g_cmd, "irq")) {
        cmd_irq(ts);
    } else if (st_eq(g_cmd, "perf")) {
        cmd_perf(ts);
    } else if (st_eq(g_cmd, "disp") || st_eq(g_cmd, "display")) {
        cmd_disp(ts);
    } else if (st_eq(g_cmd, "ls") || st_eq(g_cmd, "dir")) {
        cmd_ls(ts);
    } else if (st_eq(g_cmd, "cat")) {
        ok = cmd_cat(ts, g_arg1);
    } else if (st_eq(g_cmd, "write") || st_eq(g_cmd, "save")) {
        ok = cmd_write(ts, g_arg1, args2);
    } else if (st_eq(g_cmd, "touch")) {
        ok = cmd_touch(ts, g_arg1);
    } else if (st_eq(g_cmd, "rm") || st_eq(g_cmd, "del")) {
        ok = cmd_rm(ts, g_arg1);
    } else if (st_eq(g_cmd, "echo")) {
        ok = cmd_echo(ts, args);
    } else if (st_eq(g_cmd, "lang") || st_eq(g_cmd, "language")) {
        ok = cmd_lang(ts, g_arg1);
    } else if (st_eq(g_cmd, "set")) {
        ok = cmd_set(ts, g_arg1, g_arg2);
    } else if (st_eq(g_cmd, "about")) {
        cmd_about(ts);
    } else if (st_eq(g_cmd, "clear") || st_eq(g_cmd, "cls")) {
        cmd_clear(ts);
    } else if (st_eq(g_cmd, "reboot") || st_eq(g_cmd, "restart")) {
        if (st_eq(g_cmd, "restart") && st_eq(g_arg1, "--soft")) {
            // 真：软重启 = 优雅停止（逆序停模块 + 会话快照/配置落盘 flush）-> 原有硬复位链。
            // sysstate64_soft_restart64() 不返回（最后进 sys_reboot64），所以先打日志。
            term_log_cmd(g_cmd, true);
            logged = true;
            ts_puts(ts, "restart --soft: graceful stop (modules + session + config flush) -> hard reset chain\n");
            gui64_flip_window(ts->win);
            sysstate64_soft_restart64();
        } else {
            term_log_cmd(g_cmd, true);     // 先打日志：sys_reboot64 之后不一定还会返回
            logged = true;
            ts_puts(ts, gui64_tr("Restarting (sys_reboot64)...\n", "正在重启（sys_reboot64）...\n"));
            gui64_flip_window(ts->win);
            sys_reboot64();
        }
    } else if (st_eq(g_cmd, "shutdown") || st_eq(g_cmd, "poweroff")) {
        term_log_cmd(g_cmd, true);
        logged = true;
        ts_puts(ts, gui64_tr("Shutting down (sys_shutdown64)...\n", "正在关机（sys_shutdown64）...\n"));
        gui64_flip_window(ts->win);
        sys_shutdown64();
    } else if (st_eq(g_cmd, "cfg") || st_eq(g_cmd, "config")) {
        // 真：config64（类型化 KV，落在 store64 上）。cfg | get K | set K V | set K=V | save | reset
        ok = cmd_cfg(ts, g_arg1, g_arg2, args2);
    } else if (st_eq(g_cmd, "kill")) {
        // 真：终止一个内核任务（idle 与当前任务不可终止；规则在 task64.cpp 里）
        int tid = parse_dec(g_arg1);
        if (tid < 0) {
            ts_puts(ts, gui64_tr("kill: usage: kill <task_id>   (ps lists task ids)\n",
                                 "kill: 用法: kill <任务 id>   （ps 可列出任务 id）\n"));
            ok = false;
        } else if (task_kill64((uint32_t)tid) == 0) {
            ts_puts(ts, "[TASK] kill id=");
            ts_put_u64(ts, (uint64_t)tid);
            ts_puts(ts, " ok\n");
            dbg64_str("[TASK] kill id=");
            dbg64_dec((uint64_t)tid);
            dbg64_str(" ok\n");
        } else {
            ts_puts(ts, gui64_tr("kill: invalid or non-terminable task id=",
                                 "kill: 无效或不可终止的任务 id="));
            ts_put_u64(ts, (uint64_t)tid);
            ts_putc(ts, (uint32_t)'\n');
            ok = false;
        }
    } else if (st_eq(g_cmd, "run")) {
        // 真：从 VimtuFS2 读出应用并在 ring3 里跑。**按文件头魔数自动分派**：
        //   "VAP64\0\0\0" -> app64_launch64（平铺代码段 + int 0x80）
        //   "\x7fELF"     -> elf64_run64（ELF64 加载器 + syscall 指令）
        // 这样用户不用记格式；想让某条路径强制走某一侧就用 elf（见下面的 elfrun）。
        if (g_arg1[0] == 0) {
            ts_puts(ts, gui64_tr("run: usage: run <name|/path>   (e.g. run hello.vap | run hello.elf)\\n",
                                 "run: 用法: run <名字|/路径>   （例如 run hello.vap | run hello.elf）\\n"));
            ok = false;
        } else {
            char path[64];
            int plen = 0;
            if (g_arg1[0] != '/') path[plen++] = '/';            // "name" -> "/name"；已带 '/' 就原样
            for (int i = 0; g_arg1[i] && plen < (int)sizeof(path) - 1; i++) path[plen++] = g_arg1[i];
            path[plen] = 0;
            const int rc = app64_run_any64(path);                // 读盘 -> 判魔数 -> 分派 -> ring3 跑
            ts_puts(ts, rc == 0 ? gui64_tr("run: ok\\n", "run: 成功\\n")
                                : gui64_tr("run: failed (see serial log)\\n", "run: 失败（见串口日志）\\n"));
            ts_puts(ts, "[APP64] run cmd path=");                // 屏幕 + 串口同一条打点（自动验收 grep）
            ts_puts(ts, path);
            ts_puts(ts, " rc=");
            if (rc < 0) ts_putc(ts, (uint32_t)'-');
            ts_put_u64(ts, (uint64_t)(rc < 0 ? -rc : rc));
            ts_putc(ts, (uint32_t)'\n');
            dbg64_str("[APP64] run cmd path=");
            dbg64_str(path);
            dbg64_str(" rc=");
            if (rc < 0) dbg64_putc('-');
            dbg64_dec((uint64_t)(rc < 0 ? -rc : rc));
            dbg64_nl();
            ok = (rc == 0);
        }
    } else if (st_eq(g_cmd, "elfrun")) {
        // elfrun 显式走 ELF64 加载器（不做魔数分派），用来把 ELF 路径单独验证出来。
        if (g_arg1[0] == 0) {
            ts_puts(ts, gui64_tr("elfrun: usage: elfrun <name|/path>   (e.g. elfrun hello.elf)\\n",
                                 "elfrun: 用法: elfrun <名字|/路径>   （例如 elfrun hello.elf）\\n"));
            ok = false;
        } else {
            char path[64];
            int plen = 0;
            if (g_arg1[0] != '/') path[plen++] = '/';
            for (int i = 0; g_arg1[i] && plen < (int)sizeof(path) - 1; i++) path[plen++] = g_arg1[i];
            path[plen] = 0;
            const int rc = elf64_run64(path);                    // 读盘 -> ELF64 校验/装载 -> ring3
            ts_puts(ts, rc == 0 ? gui64_tr("elfrun: ok\\n", "elfrun: 成功\\n")
                                : gui64_tr("elfrun: failed (see serial log)\\n", "elfrun: 失败（见串口日志）\\n"));
            ts_puts(ts, "[ELF64] run cmd path=");
            ts_puts(ts, path);
            ts_puts(ts, " rc=");
            if (rc < 0) ts_putc(ts, (uint32_t)'-');
            ts_put_u64(ts, (uint64_t)(rc < 0 ? -rc : rc));
            ts_putc(ts, (uint32_t)'\n');
            dbg64_str("[ELF64] run cmd path=");
            dbg64_str(path);
            dbg64_str(" rc=");
            if (rc < 0) dbg64_putc('-');
            dbg64_dec((uint64_t)(rc < 0 ? -rc : rc));
            dbg64_nl();
            ok = (rc == 0);
        }
    } else if (st_eq(g_cmd, "task") || st_eq(g_cmd, "tasks") || st_eq(g_cmd, "top")) {
        cmd_ps(ts);          // 与 ps 同一份真实快照（top 不做全屏刷新，只打一次）
    } else if (st_eq(g_cmd, "syslog")) {
        // 真：sysstate64 的 ring log（固定 64 条循环日志）
        cmd_syslog(ts);
    } else if (st_eq(g_cmd, "state")) {
        // 真：运行状态机 + 模块表（sysstate64）
        cmd_state(ts);
    } else if (st_eq(g_cmd, "health")) {
        // 真：健康报告（各模块的 state/health 都是实测）
        cmd_health(ts);
    } else if (st_eq(g_cmd, "session")) {
        // 真：会话策略（session64；keep 与策略落在 config64/store64）
        ok = cmd_session(ts, g_arg1, g_arg2, args2);
    } else if (st_eq(g_cmd, "update")) {
        // ★ 仍未移植（后续批次）：如实说明，不静默失败
        ok = shell_unsupported(ts, g_cmd,
                               "not supported yet (update/persistence subsystem not ported to 64-bit)",
                               "尚未支持（更新/持久化子系统未移植到 64 位）");
    } else if (st_eq(g_cmd, "store")) {
        // 真：设置持久化 store（kernel/store64.cpp）。载体优先 VimtuFS2 的 /store.a、/store.b，
        // 没有可用卷时才退回裸盘槽区（那条路会打重叠 WARN）。
        ok = cmd_store(ts, g_arg1, g_arg2, args2);
    } else if (st_eq(g_cmd, "ping")) {
        // 真：e1000 + ARP/ICMP（kernel/net64.cpp）。屏幕 + 串口都打 [NET64] cmd ping 行。
        ok = cmd_ping(ts, g_arg1);
    } else if (st_eq(g_cmd, "disk") || st_eq(g_cmd, "ata")) {
        // 真：ATA IDENTIFY（型号/容量；ata64 的读取自带 IRQ14 等待 + 超时回退轮询）+ VimtuFS2 卷几何
        ok = cmd_disk(ts);
    } else if (st_eq(g_cmd, "hw") || st_eq(g_cmd, "hwinfo")) {
        // 真：hwinfo64 的 CPUID + PCI 枚举结果
        cmd_hw(ts);
    } else if (st_eq(g_cmd, "lspci") || st_eq(g_cmd, "pci")) {
        // 真：PCI 设备表（hwinfo64 的只读枚举）
        cmd_lspci(ts);
    } else if (st_eq(g_cmd, "user") || st_eq(g_cmd, "userprog")) {
        // 真：ring3 现状（用户窗口地址/页映射/盘上的 ring3 程序）；`user run` 直接跑一次用户程序
        ok = cmd_user(ts, g_arg1);
    } else if (st_eq(g_cmd, "preload")) {
        // ★ 仍未移植（后续批次）：如实说明
        ok = shell_unsupported(ts, g_cmd,
                               "not supported yet (preload subsystem not ported to 64-bit)",
                               "尚未支持（预加载子系统未移植到 64 位）");
    } else if (st_eq(g_cmd, "bsod") || st_eq(g_cmd, "panic")) {
        // 真：受控蓝屏（kernel/panic64.cpp）。先打命令日志，再进 BSOD（不返回）
        term_log_cmd(g_cmd, true);
        logged = true;
        cmd_panic(ts, g_arg1);
    } else {
        ts_puts(ts, gui64_tr("command not found: ", "未找到命令："));
        ts_puts(ts, g_cmd);
        ts_putc(ts, (uint32_t)'\n');
        ts_puts(ts, gui64_tr("  (try: help)\n", "  （试试 help）\n"));
        ok = false;
    }

    if (!logged) term_log_cmd(g_cmd, ok);
}

// ==================== 窗口回调 ====================
// 关窗第一步：把状态从注册表摘除、w->userdata 置空、状态交给待释放队列并立即回收，最后打关窗日志。
// 返回是否确实接管了一个状态。Esc 路径接管后由调用方销毁窗口；外壳已在关的窗口交给外壳销毁。
// 为什么可以立即 kfree：状态唯一的对外引用就是 w->userdata（已置空），注册表也摘掉了，
//   之后再没有任何"活窗口 -> 状态"的路径（ts_reap_pending 还会用 ts_referenced 再确认一次）。
//   不这么做的话，"关掉最后一个终端"后将没有活窗口触发 tick，状态要等到下次开窗/重置才回收。
static bool term_release_state(Window* w) {
    TerminalState* ts = ts_of(w);
    if (!ts) return false;
    int inst = ts->inst;
    w->userdata = nullptr;      // 先断引用：外壳若按 app_id kfree(userdata)，看到空指针就跳过
    ts_detach(ts);              // 摘除注册表 + 入待释放队列
    dbg64_str("[APP] term closed");
    dbg64_nl();
    dbg64_str("[TERM] closed inst=");
    dbg64_dec((uint64_t)inst);
    dbg64_str(" remaining=");
    dbg64_dec((uint64_t)g_count);
    dbg64_nl();
    ts_reap_pending(true);      // 已无人引用 -> 立即回收（宿主自测里断言"关窗后无泄漏"）
    return true;
}

static void term_key(Window* w, char c) {
    TerminalState* ts = ts_of(w);
    if (!ts || !ts->used) return;
    if (c == 0x1B) {                       // Esc 关闭本窗口（与计算器/扫雷/设置一致）
        term_release_state(w);             // 先接管状态（避免外壳释放 / 本文件释放撞车）
        gui64_destroy_window(w);
        return;
    }
    if (c == '\n' || c == '\r') {          // 回车执行
        ts->cmdline[ts->cmdlen] = 0;
        ts_putc(ts, (uint32_t)'\n');
        shell_exec(ts, ts->cmdline);
        ts->cmdlen = 0;
        shell_prompt(ts);
    } else if (c == '\b' || c == 0x7F) {   // 退格
        if (ts->cmdlen > 0) {
            ts->cmdlen--;
            ts_putc(ts, (uint32_t)'\b');
        }
    } else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F) {
        if (ts->cmdlen < TERM_CMD_MAX - 1) {
            ts->cmdline[ts->cmdlen++] = c;
            ts_putc(ts, (uint32_t)(unsigned char)c);
        }
    }
}

// 每个外壳 tick：处理"外壳要关窗"、清理已消失的实例、收割待释放队列
static void term_tick(Window* w) {
    // 1) 外壳已经在关这个窗口（点标题栏 X / 外壳清理）：抢在销毁前把状态接管下来。
    //    这里**不**销毁窗口：外壳既然置了 closing，就由外壳自己完成销毁（避免在 tick 里改窗口链表）。
    if (w && w->closing && term_release_state(w)) return;
    // 2) 外壳一步到位销毁的实例（没给 closing 机会）：只摘除引用，不冒险释放
    ts_prune();
    // 3) 收割自己接管下来的状态
    ts_reap_pending(false);
}

// ==================== 实例创建 / 打开 / 重置 ====================
static TerminalState* ts_new(Window* win) {
    int off = (int)((sizeof(TerminalState) + 1) & ~(size_t)1);      // cells 的起点（2 字节对齐）
    uint64_t bytes = (uint64_t)off + (uint64_t)TERM_BUF_MAX * sizeof(uint16_t);
    int prev = mem_owner_get_64();
    mem_owner_set_64(MEM_OWNER_TERMINAL_64);      // 归属记账：任务管理器/内存归属统计
    TerminalState* ts = (TerminalState*)kmalloc_64(bytes);
    mem_owner_set_64(prev);
    if (!ts) return nullptr;
    ts->cells = (uint16_t*)((char*)ts + off);
    ts->cols = 0;
    ts->rows = 0;
    ts->cur_r = 0;
    ts->cur_c = 0;
    ts->cmdline[0] = 0;
    ts->cmdlen = 0;
    ts->win = win;
    ts->inst = 0;
    ts->used = false;
    for (int i = 0; i < TERM_BUF_MAX; i++) ts->cells[i] = 0;
    return ts;
}

static void term_init_once() {
    if (g_inited) return;
    g_inited = true;
    for (int i = 0; i < TERM_MAX_INST; i++) { g_inst[i] = nullptr; g_win[i] = nullptr; g_inst_no[i] = 0; }
    g_count = 0;
    g_next_inst = 1;
    for (int i = 0; i < TERM_PEND_MAX; i++) { g_pend[i] = nullptr; g_pend_tick[i] = 0; }
    g_pend_n = 0;
    ramfs_init_once();
}

// 入口：开终端（每次调用都新建窗口 = 多开；已有 4 个时只激活最近的实例）
void app_term_open64() {
    term_init_once();
    ts_prune();                                    // 先清掉已被外壳直接销毁的实例
    ts_reap_pending(true);
    int live = gui64_app_windows(APP_ID_TERM);     // 外壳侧的真实窗口数（与注册表取较大者）
    if (g_count >= TERM_MAX_INST || live >= TERM_MAX_INST) {
        TerminalState* last = (g_count > 0) ? g_inst[g_count - 1] : nullptr;
        Window* lw = last ? last->win : nullptr;
        if (lw && gui64_window_alive(lw)) gui64_set_active(lw);
        dbg64_str("[TERM] open limited (");
        dbg64_dec((uint64_t)live);
        dbg64_str("/");
        dbg64_dec((uint64_t)TERM_MAX_INST);
        dbg64_str(") activate inst=");
        dbg64_dec(last ? (uint64_t)last->inst : 0);
        dbg64_nl();
        return;
    }

    // 目标客户区 = 默认 60x40 格
    int cw  = TERM_DEF_COLS * TERM_CELL_W + TERM_PAD_X;
    int chh = TERM_DEF_ROWS * TERM_CELL_H + TERM_PAD_Y;
    int W = gui64_screen_w(), H = gui64_screen_h();
    int off = (g_count % TERM_MAX_INST) * 24;      // 级联摆放，避免完全重叠
    int px = 16 + off, py = 16 + off;
    int ww = cw + 10, hh = chh + 34;               // 初值（外壳会按标题栏/边框再修正）
    if (ww > W - 8) ww = W - 8;
    if (hh > H - 8) hh = H - 8;
    if (ww < 64) ww = 64;
    if (hh < 64) hh = 64;

    Window* win = gui64_create_window(gui64_tr("Terminal - vimtu64", "终端 - vimtu64"),
                                      px, py, ww, hh, term_draw, term_key, nullptr, APP_ID_TERM);
    if (!win) {
        dbg64_str("[TERM] open failed: window alloc failed");
        dbg64_nl();
        return;
    }
    int dw = win->w - win->client_w;  if (dw < 0) dw = 0;
    int dh = win->h - win->client_h;  if (dh < 0) dh = 0;
    gui64_set_min_size(win, TERM_COLS_MIN * TERM_CELL_W + TERM_PAD_X + dw,
                            TERM_ROWS_MIN * TERM_CELL_H + TERM_PAD_Y + dh);
    gui64_fit_window_to_client(win, cw, chh);      // 把客户区精确设成 60x40 格

    // 摆到屏内
    if (px + win->w > W) px = W - win->w;
    if (py + win->h > H) py = H - win->h;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px != win->x || py != win->y) gui64_set_geom(win, px, py, win->client_w, win->client_h);

    TerminalState* ts = ts_new(win);
    if (!ts) {
        gui64_destroy_window(win);                 // userdata 还是 nullptr，外壳不会误释放
        dbg64_str("[TERM] open failed: state alloc failed");
        dbg64_nl();
        return;
    }
    ts->used = true;
    ts->inst = g_next_inst++;
    win->userdata = ts;
    g_inst[g_count] = ts;
    g_inst_no[g_count] = ts->inst;
    g_win[g_count] = win;
    g_count++;
    gui64_set_tick(win, term_tick);

    ts_sync_layout(ts, win);                       // 推导 cols/rows（并打 [UI] term layout）
    ts->cmdlen = 0;
    shell_banner(ts);
    shell_prompt(ts);
    gui64_invalidate_window(win);

    // 自动验收断言行：开窗的实例内实际排版
    dbg64_str("[APP] term opened cols=");
    dbg64_dec((uint64_t)ts->cols);
    dbg64_str(" rows=");
    dbg64_dec((uint64_t)ts->rows);
    dbg64_nl();
    dbg64_str("[TERM] opened inst=");
    dbg64_dec((uint64_t)ts->inst);
    dbg64_str(" cols=");
    dbg64_dec((uint64_t)ts->cols);
    dbg64_str(" rows=");
    dbg64_dec((uint64_t)ts->rows);
    dbg64_str(" windows=");
    dbg64_dec((uint64_t)(live + 1));
    dbg64_nl();
}

// 会话重置：清空所有实例的屏幕与命令行（"软件内容不保存"），照 32 位 terminal_reset
void app_term_reset64() {
    term_init_once();
    ts_prune();
    ts_reap_pending(true);
    int n = 0;
    for (int i = 0; i < g_count; i++) {
        TerminalState* ts = g_inst[i];
        if (!ts || !ts->used) continue;
        ts_clear(ts);
        ts->cmdlen = 0;
        shell_prompt(ts);                          // 清屏后补回提示符（32 位留空白屏；这里更可用）
        ts_dirty_client(ts);
        gui64_invalidate_window(ts->win);
        n++;
    }
    dbg64_str("[APP] term reset");
    dbg64_nl();
    dbg64_str("[TERM] reset insts=");
    dbg64_dec((uint64_t)n);
    dbg64_nl();
}
