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
// 【排版】列/行由客户区尺寸推导（不再是编译期常量），默认 60x40 个**汉字格**：
//     cols = (client_w - PAD_X) / 8    （8px = 半格）、rows = (client_h - PAD_Y) / 16
//   内容缓冲是**码点网格**（uint16_t，0 = 空），每格宽度按字体推进宽度算：
//     ASCII  -> 终端等宽面（face 2 = Sarasa Mono SC；8px = 汉字宽的一半）
//     其它    -> font_draw_glyph_cp(...)（查询链：当前面 -> 中文面(16px) -> 兜底面；只有前景色，底色靠预填）
//   ★ 中英 1:2：两个 ASCII 正好占一个汉字格的宽度。旧版是内建 8x8 位图字体 ×2 = 16x16 整格；
//     现在整格都用等宽面 TrueType，宽度由字体度量决定（见 kernel/font.cpp 的 font_selftest）。
//   退格 / 滚动 / 重排语义与 32 位一致；拖边框缩放重排时打一行 [UI] term layout。
//
// 【日志】自动验收依赖下面这些行（原样）：
//   [APP] term opened cols=N rows=M     开窗（含实例内实际排版列/行）
//   [APP] term reset                    会话重置（清屏/清状态）
//   [APP] term closed                   关窗
//   [UI]  term layout client=WxH cols=N rows=M   布局变化（开窗时也打一行）
//   [TERM] cmd <名字> ok|fail            每条命令执行打一行（本轮起固定带 ok/fail —— 新增的真命令
//                                        用它做是否真的实现过的判据；旧格式只有失败才带 fail）
//   [TERM] unsupported <名字>: <原因>     未支持命令的说明（**现在没有命令走这条路了**：最后两条
//                                        update / preload 在本轮接真；函数保留但无调用者 -> 已删）
//   [TASK] ps rows=N switches=M          ps/tasks/task/top 打点（任务表真实行数 + 累计切换次数）
//   [TASK] kill id=N ok                  kill <id> 成功（终端的 kill 只走 task_kill64 真路径）
//   [APP64] run cmd path=<p> rc=<n>      run <name> 的命令打点（走 app64_launch64 真路径）
//   [STORE64] cmd <dump|get|set|flush> ...  store 命令打点（走 kernel/store64.cpp 真路径）
//
// 【Shell 命令】32 位能实现、且 64 位有对应子系统的**全部**实现；**没有"尚未支持"的命令了**
//   （批次 A 后半把最后两条桩 update / preload 接成真实现；终端新增 proc 命令管理 proc64 真进程）。
//   此前的批次已接真：hw/hwinfo（hwinfo64 的 CPU/PCI）、lspci/pci（PCI 设备表）、disk/ata（ATA IDENTIFY
//   型号/容量 + VimtuFS2 卷几何）、user/userprog（ring3 现状，arg=run 时跑一次用户程序）、
//   cfg/config（config64 类型化配置 + 落盘位置）、syslog/state/health（sysstate64 状态机/模块/健康/ring log）、
//   session（session64 会话策略）、restart --soft（优雅停止 + 硬复位链）、panic/bsod（受控蓝屏）。
//   任务相关命令（ps/tasks/task/top/kill <id>）走内核任务表（kernel/task64.h 的对外 API），
//   没有任何假数据：任务数为 0 时如实打印"无任务数据"。
//   文件系统：**真的文件系统**是 VimtuFS2（kernel/vfs64.cpp，磁盘上的 /store.a|b、/hello.vap、
//   /hello.elf 都在它上面）。批次 B 起：ls/cat/write/touch/rm/mkdir/df/echo > 全部走 kernel/fd64.cpp
//   的 FD 层（**多级路径** "/dir/sub/name"、单文件上限 **8 MiB**（v2 卷 67584 B）、无权限；rm 不能删目录），
//   原来那份 16x512B 的 RAM-only ramfs 已删除。
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
#include "edid64.h"      // display edid：引导期 EDID（0x7600）解析结果
#include "ata64.h"       // disk/ata：ATA IDENTIFY（型号/容量；读取自带超时保护）
#include "vfs64.h"       // disk：卷状态；user：盘上的 ring3 程序
#include "drive64.h"     // vol：盘符/卷槽表（多卷：vol C:|D: = 切换当前卷）
#include "fs64.h"        // ★ 批次 K：统一卷号 + FAT32 只读语义（ls/cat 可用，写类命令明确报"只读卷"）
#include "fd64.h"        // 文件命令的 FD 层（32 项；多级路径 /dir/sub/name、单文件 <= 8 MiB）
#include "explorer64.h"  // ★ 批次 M：bigtest copy 复用文件管理器的复制引擎（explorer64_copy_file64）
#include "display64.h"   // display [modes|hz|edid]：模式清单 + 0x3DA 实测刷新率 + EDID 对比
#include "usermode64.h"  // user/userprog：ring3 用户窗口地址与页映射查询
#include "config64.h"    // cfg/config：类型化配置 + 存储位置（落在 store64 上）
#include "session64.h"   // session：会话/应用内容策略的真实现
#include "sysstate64.h"  // syslog/state/health：状态机 + 模块表 + 健康 + ring log
#include "panic64.h"     // panic/bsod：受控蓝屏；ping 期间的看门狗停表
// ---- 批次 A 后半：最后两条"未移植"桩接真 + proc64 真进程 ----
#include "preload64.h"   // preload：字形/图标预热统计（启动期已跑一轮）
#include "update64.h"    // update：标记文件 -> 应用 -> store/重启（边界见 update64.h）
#include "proc64.h"      // proc：proc64 进程表（list / run / kill）
#include "console64.h"   // ★ 批次 N：dmesg（开机滚屏引导控制台的启动日志缓冲）+ boot verbose
#include "rust64.h"     // ★ Rust 模块（gui_rs）：设计 Token + 主题配色（terminal `rust` 命令用它）
// ★ 本批（P1c）：多用户骨架（userdb64）+ 锁屏/登录（loginctl lock）
#include "userdb64.h"
#include "locklogin64.h"

// ==================== 常量 ====================
#define TERM_MAX_INST    4          // 多开上限（照 32 位；第 5 次只激活最新的）
#define TERM_CHAR_SCALE  2          // 格高基准（8x8 × 2 = 16px；ASCII 字形现在来自等宽面 TTF）
#define TERM_CELL_W      (8 * TERM_CHAR_SCALE)      // 一个**汉字格**宽（像素）= 16
#define TERM_HALF_W      (TERM_CELL_W / 2)          // 半格 = ASCII 推进宽度 8px（等宽面 ASCII 宽 = 汉字宽/2）
#define TERM_CELL_H      (8 * TERM_CHAR_SCALE)      // 字符格高（像素）= 16
#define TERM_COLS_MAX    256        // 内容缓冲列上限（8px 单位：1920 宽最大化 = 240 列，留余量）
#define TERM_ROWS_MAX    72         // 内容缓冲行上限
#define TERM_COLS_MIN    20         // 排版列下限（8px 单位 = 10 个汉字格）
#define TERM_ROWS_MIN    6          // 排版行下限
#define TERM_CMD_MAX     256        // 命令行缓冲
#define TERM_DEF_COLS    120        // 新建窗口默认排版（120 半格 = 60 汉字格 = 960px，与旧版像素尺寸一致）
#define TERM_DEF_ROWS    40
#define TERM_BUF_MAX     (TERM_COLS_MAX * TERM_ROWS_MAX)
#define TERM_PAD_X       4          // 排版可用宽 = client_w - PAD_X
#define TERM_PAD_Y       4          // 排版可用高 = client_h - PAD_Y
#define TERM_MONO_DY     (-2)       // 等宽面 ASCII 字形相对格顶的微调（与 TERM_ZH_DY 同一基线补偿）
#define TERM_ZH_DY       (-2)       // TrueType 中文字形相对格顶的微调（em 盒 vs 位图字形对齐）
#define TERM_PEND_MAX    8          // 待释放队列长度
#define TERM_PEND_DELAY  4          // 延迟释放的 tick 数（约 16ms，足够外壳销毁窗口）

// 配色照 32 位：黑底 + 浅灰字 + 绿色块状光标（绿底黑字）
#define TERM_BG      rgb(0x00, 0x00, 0x00)
#define TERM_FG      rgb(0xE0, 0xE0, 0xE0)
#define TERM_CUR     rgb(0x00, 0xC0, 0x00)
#define TERM_CURFG   rgb(0x00, 0x00, 0x00)

// 批次 B：终端文件命令**不再用 ramfs** —— 全部走 kernel/fd64.cpp 的 FD 层（底层 VimtuFS2）。
// 限制（帮助里也如实写）：**路径是多级的**（"/dir/sub/name"，v3 目录树起；单段 ≤31B、整条 ≤128B、≤16 层）、
// 单文件 <= **8 MiB**（批次 M：二级间接块；v2 旧卷仍 67584 B）、无权限；
// rm 只能删文件（删空目录用 rmdir 原语，终端暂未暴露）；写文件按偏移即时落盘（无脏页）。
// `cat` 大文件最多打 4096 B（明确提示截断，不静默）；`bigtest` 是给自动验收用的生成/校验入口。

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
static char g_pathbuf[FD64_PATH_MAX];  // 文件路径（规范化后的 "/name"）
static char g_store_k[32];             // store 命令：key 缓冲（key 最长 31B + NUL）
static char g_store_v[256];            // store 命令：value 缓冲（value 最长 255B + NUL）


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

// 排版尺寸：客户区放得下的列/行（不钳制；列的单位是 8px 半格 = 等宽面 ASCII 推进宽度）
static void ts_fit(Window* w, int* cols, int* rows) {
    int c = (w->client_w - TERM_PAD_X) / TERM_HALF_W;
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

// ---------------- 变宽网格的推进宽度（终端等宽面：ASCII 8px / 汉字 16px）----------------
// 一个码点在终端里占多少像素：ASCII 走等宽面（face 2），其它走查询链（汉字落中文面 16px、
// 兜底字形 8/16px）。★ "中英 1:2" 就是靠字体度量落地：渲染 / 换行 / 光标全用它。
static int term_adv(uint32_t cp) {
    font_select(FONT_FACE_MONO);
    if (cp < 0x80) return font_glyph_advance((char)cp);
    return font_glyph_advance_cp(cp);
}

// 网格单位 = 等宽面 ASCII 的推进宽度（构建期 _subset_fonts.py 断言它是 8px = 汉字宽的一半）
static int term_unit() {
    font_select(FONT_FACE_MONO);
    int w = font_glyph_advance(' ');
    return (w > 0) ? w : TERM_HALF_W;
}

// 客户区可用的横向像素（排版/换行用）
static int ts_usable_w(TerminalState* ts) {
    int w = (ts && ts->win) ? ts->win->client_w : 0;
    w -= TERM_PAD_X;
    return w > 0 ? w : 0;
}

// 一行里前 c 个码点的累计像素宽度（变宽网格：每格宽度 = term_adv(该码点)）
static int ts_row_px(TerminalState* ts, int r, int c) {
    if (!ts || r < 0 || r >= TERM_ROWS_MAX) return 0;
    const uint16_t* row = ts->cells + r * TERM_COLS_MAX;
    int w = 0;
    for (int i = 0; i < c && i < TERM_COLS_MAX; i++) {
        uint16_t cp = row[i];
        w += (cp >= 0x20) ? term_adv((uint32_t)cp) : term_unit();
    }
    return w;
}

// 写一个码点：按该实例当前 cols/rows 与**像素宽度**裁剪与滚动，并只标脏受影响的区域
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
        if (cp > 0xFFFF) cp = (uint32_t)'?';                    // 缓冲是 uint16 码点（BMP）
        int adv = term_adv(cp);
        // 行满自动换行：格子数满 **或** 像素宽度放不下（ASCII 半格 + 汉字整格混排不会溢出客户区）
        if (ts->cur_c >= ts->cols || ts_row_px(ts, ts->cur_r, ts->cur_c) + adv > ts_usable_w(ts))
            ts_newline(ts, &scrolled);
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


// ==================== 绘制 ====================
// 一格：ASCII 走**终端等宽面**（face 2，Sarasa Mono SC，推进 8px = 汉字宽的一半，自带底色靠预填），
// 其余（汉字/制表符/兜底字形）走 font_draw_glyph_cp 的查询链（当前面 -> 中文面 -> 兜底面）。
// ★ 与旧版的区别：旧版 ASCII 走内建 8x8 位图字体（fb_draw_char ×2 = 16x16），
//   现在整格都用等宽面 TrueType —— 于是"中英 1:2"是字体自己的度量：ASCII 8px、汉字 16px。
static void term_draw_cell(int x, int y, int w, uint16_t cp, uint32_t fg, uint32_t bg) {
    if (bg != TERM_BG || w != TERM_CELL_W) fb_fill_rect(x, y, w, TERM_CELL_H, bg);
    if (cp < 0x20) return;
    font_select(FONT_FACE_MONO);                 // 终端字符格的字体面
    if (cp < 0x7F) font_draw_glyph(x, y + TERM_MONO_DY, (char)cp, fg);
    else           font_draw_glyph_cp(x, y + TERM_ZH_DY, (uint32_t)cp, fg);
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
    int right = x0 + cw;                                  // 右侧像素裁剪（变宽网格：整行宽度按推进累加）
    for (int r = 0; r < dr; r++) {
        const uint16_t* row = ts->cells + r * TERM_COLS_MAX;
        int ty = y0 + TERM_PAD_Y + r * TERM_CELL_H;
        int tx = x0 + TERM_PAD_X;
        for (int c = 0; c < dc; c++) {
            uint16_t cp = row[c];
            int adv = (cp >= 0x20) ? term_adv((uint32_t)cp) : term_unit();
            if (tx >= right) break;
            if (cp >= 0x20) term_draw_cell(tx, ty, adv, cp, TERM_FG, TERM_BG);
            tx += adv;
        }
    }
    // 光标：块状光标（绿底黑字），宽度 = 该格码点的推进宽度（ASCII 半格 / 汉字整格），不依赖 '_' 字形
    if (w->active && ts->cur_r >= 0 && ts->cur_r < dr && ts->cur_c >= 0 && ts->cur_c < dc) {
        int cx = x0 + TERM_PAD_X + ts_row_px(ts, ts->cur_r, ts->cur_c);
        int cy = y0 + TERM_PAD_Y + ts->cur_r * TERM_CELL_H;
        uint16_t cp = ts->cells[ts->cur_r * TERM_COLS_MAX + ts->cur_c];
        int adv = (cp >= 0x20) ? term_adv((uint32_t)cp) : term_unit();
        if (cx < right) {
            fb_fill_rect(cx, cy, adv, TERM_CELL_H, TERM_CUR);
            if (cp >= 0x20) term_draw_cell(cx, cy, adv, cp, TERM_CURFG, TERM_CUR);
        }
    }
}

// ==================== Shell：输出与命令 ====================
// ★ 本批（P1c）：提示符 = **会话身份**（su - root / su - / sudo -i 之后变 root，提示符变 '#'）
static void shell_prompt(TerminalState* ts) {
    const char* u = userdb64_session_name64();
    if (u && u[0] && u[0] != '-') {
        ts_puts(ts, u);
        ts_puts(ts, userdb64_root_session64() ? "@vimtu64:~# " : "@vimtu64:~$ ");
    } else {
        ts_puts(ts, "vimtu64:~$ ");          // 还没登录（理论上到不了这里）：退回旧提示符
    }
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
    "  run NAME|/PATH        load an app from VimtuFS2 and run it in ring3 (magic decides:\n"
    "                        VAP64 -> int 0x80 path, ELF64 -> syscall path; e.g. run hello.elf)\n"
    "  elfrun NAME|/PATH     force the ELF64 loader (syscall insn ABI), e.g. elfrun hello.elf\n"
    "  echo TEXT             print text (echo TEXT > FILE writes a real file)\n"
    "  write FILE TEXT       write a real file (overwrite; text is limited by the command line; "
    "multi-level /dir/name; the FILE itself may be up to 8 MiB)\\\\n"
    "  cat FILE / ls, dir    read file / list a directory with sizes (real disk, not ramfs; ls = volume root;\\\\n"
    "                        cat shows at most 4096 B and says so when a file is larger)\\\\n"
    "  bigtest [1mb|8mb|limit|recycle|all]  write+verify big files through the real fd64/vfs64 path\\\\n"
    "  touch FILE / rm FILE  create empty file / delete file (rm cannot delete directories)\\n"
    "  mkdir DIR / df        create a directory (multi-level, parent must exist) / ALL volumes: 1K blocks+free\\n"
    "  vol [C:|D:]           list volumes & drive letters / switch the CURRENT volume (ls/cat/write/mkdir/rm/run\\n"
    "                        all act on the CURRENT volume; the explorer switches it when you open a drive card)\\n"
    "  date / time           RTC date / time\n"
    "  uptime                time since boot (ticks/250)\n"
    "  irq                   total interrupt count\n"
    "  perf                  GUI fps / busy% / irq / heap\n"
    "  display [modes|hz|edid] runtime display layer: boot mode list (0x7400) / measured 0x3DA refresh\n"
    "                        vs EDID preferred timing / EDID identity; 'disp' = the same summary\n"
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
    "  syslog                sysstate64 ring log (fixed 64-line circular log) + [SYS64] syslog lines=N\n"
    "  dmesg                 boot console log (16 KiB ring + head keep; screen + [CON64] dmesg[i] serial dump)\n"
    "  boot verbose on|off   show/skip the scrolling boot console before the desktop (persisted: boot.verbose)\n"
    "  state                 system state machine (BOOT/STARTING/RUNNING/STOPPING/STOPPED) + module table\n"
    "  health                per-module health report ([SYS64] health ok modules=N failed=0)\n"
    "  session               session policy (VOLATILE/PERSIST + per-app keep flags, persisted via config64)\n"
    "  disk, hw, lspci       ATA IDENTIFY (model/capacity) + VimtuFS2 volume; CPU/PCI; PCI device list\n"
    "  user [run]            ring3 status (window/VA/gates/on-disk programs); 'user run' launches /hello.vap\n"
    "  panic <code>, bsod    controlled BSOD: blue screen + serial stop code, halts after 6s (no auto reboot)\n"
    "  update status         update subsystem: version / applied (store) / pending marker + done file\n"
    "  update pending <ver>  stage /update.pending (marker text ver=<ver>); applied at next boot\n"
    "  update apply          apply the marker now (store + /update.done) then soft-restart; NOT a real upgrade package\n"
    "  preload [run]         glyph prewarm + icon pre-scale stats (rdtsc64 first-paint before/after); run = again\n"
    "  proc list             proc64 process table (pid/ppid/state/tasks/CR3/name) - real processes with per-process CR3\n"
    "  proc run NAME|/PATH   create + start a real proc64 process (built-in: spin -> long-lived /spin.elf)\n"
    "  proc kill PID [SIG]   signal a proc64 process (default SIGKILL=9; the task manager process page uses this)\n"
    "  rust [tokens|set N]   Rust module (gui_rs): design tokens + theme palette; rust set N switches theme\n"
    "  ---- multi-user / session identity (★ P4: uid/gid/mode permission bits ARE enforced) ----\n"
    "  useradd NAME          create a normal user (uid >= 1000; home /home/NAME owns itself, mode 0700)\n"
    "  userdel NAME          remove a user record (home dir kept; root / last normal user refused)\n"
    "  passwd [NAME] [PW]    set a password (PW omitted = hidden interactive input; PW='-' clears;\n"
    "                        stored as salt + SHA-256 x1000 iterations, never plaintext)\n"
    "  users                 user table: root (hidden in the login UI) + normal users + session ids\n"
    "  whoami / id           session identity: real uid/gid/euid/egid (su/sudo change euid/egid)\n"
    "  su [-] [NAME]         switch the SESSION identity (default root; prompts for the target's password\n"
    "                        when it has one; GUI user/avatar unchanged)\n"
    "  sudo -i               same as above (only -i is implemented)\n"
    "  exit                  leave the root session (back to the GUI user)\n"
    "  chmod MODE PATH       change the mode (octal, e.g. chmod 600 /a.txt; root or the owner only)\n"
    "  chown USER[:GROUP] P  change owner/group (root only; GROUP defaults to the user's gid)\n"
    "  umask [MASK]          show/set the umask for newly created files (default 022)\n"
    "  ls [-l] [PATH]        list a directory; -l shows -rw-r--r-- owner group size name (default /)\n"
    "  tree [PATH]           dump the directory tree to the serial log (each line has uid/gid/mode)\n"
    "  loginctl lock         lock the screen (press Enter/click, then sign in again)\n"
    "No 'not supported' commands remain: update/preload were the last two and are real now.\n";
static const char* HELP_ZH =
    "VimtuOS 64 位 Shell 命令：\n"
    "  help                  本帮助\n"
    "  ver, uname            版本与架构\n"
    "  mem, meminfo          内存（页池 / 堆 / 归属）\n"
    "  ps, tasks, top        内核任务表（调度器 task64）+ 窗口计数\n"
    "  kill TASKID           终止一个内核任务（id 从 ps 拿）\n"
    "  run 名字|/路径        从 VimtuFS2 加载应用并在 ring3 里运行（按文件头魔数自动分派：\n"
    "                        VAP64 走 int 0x80、ELF64 走 syscall 指令；例如 run hello.elf）\n"
    "  elfrun 名字|/路径     强制走 ELF64 加载器（syscall 指令 ABI），例如 elfrun hello.elf\n"
    "  echo TEXT             回显（echo TEXT > FILE 写**真文件**）\n"
    "  write FILE TEXT       写**真文件**（整体覆盖；文本受**命令行长度**限制，文件本身可到 8 MiB；多级路径）\\\\n"
    "  cat FILE / ls, dir    读文件 / 列目录（带大小；cat 最多打 4096 B，超过会**明确提示**被截断）\\\\n"
    "  bigtest [1mb|8mb|limit|recycle|all]  走真 fd64/vfs64 路径生成/校验大文件（详见串口 [BIG64] 打点）\\\\n"
    "  touch FILE / rm FILE  建空文件 / 删文件（rm 不能删目录）\\n"
    "  mkdir DIR / df        建目录（**多级**，父目录必须已存在）/ **所有卷**的块数与空闲块（512B 块，VimtuFS2）\\n"
    "  vol [C:|D:]           列出卷与盘符 / 切换**当前卷**（ls/cat/write/mkdir/rm/run 都作用在当前卷上；\\n"
    "                        在文件管理器里双击盘符卡片 = 把当前卷切到那块盘）\\n"
    "  date / time           RTC 日期 / 时间\n"
    "  uptime                开机时长（ticks/250）\n"
    "  irq                   中断总数\n"
    "  perf                  GUI 帧率 / 忙占比 / 中断 / 堆\n"
    "  display [modes|hz|edid] 运行期显示层：引导模式清单（0x7400）/ 0x3DA 实测刷新率与 EDID 对比 /\n"
    "                        EDID 身份与首选时序；disp 等同于摘要\n"
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
    "  dmesg                 开机滚屏引导控制台的启动日志（16 KiB 环形缓冲 + 头部保留；屏幕 + 串口 [CON64] dmesg[i]）\\n"
    "  boot verbose on|off   进桌面前是否显示滚屏引导日志（持久化键 boot.verbose）\\n"
    "  state                 运行状态机（BOOT/STARTING/RUNNING/STOPPING/STOPPED）+ 模块表\\n"
    "  health                各模块健康报告（串口打 [SYS64] health ok modules=N failed=0）\n"
    "  session               会话策略（VOLATILE/PERSIST + 每应用 keep 开关，走 config64 持久化）\n"
    "  disk, hw, lspci       ATA IDENTIFY（型号/容量）+ VimtuFS2 卷；CPU/PCI；PCI 设备列表\n"
    "  user [run]            ring3 现状（用户窗口/VA/门/盘上程序）；user run 直接跑 /hello.vap\n"
    "  panic <code>, bsod    受控蓝屏：蓝底白字屏 + 串口停止码，停留 6 秒后停住（不自动重启）\n"
    "  update status         更新子系统：当前版本 / 已应用（store）/ 标记文件与完成文件\n"
    "  update pending <ver>  写入 /update.pending（标记文本 ver=<ver>），下次启动时应用\n"
    "  update apply          立即应用标记（store + /update.done）并软重启；**不是真正的升级包**\n"
    "  preload [run]         字形预热 + 图标预缩放统计（rdtsc64 实测首帧前后 cycles）；run = 再跑一轮\n"
    "  proc list             proc64 进程表（pid/ppid/状态/线程数/CR3/名字）—— 每进程独立 CR3 的真进程\n"
    "  proc run 名字|/路径   创建并启动一个真 proc64 进程（内置：spin -> 长命 /spin.elf）\n"
    "  proc kill PID [SIG]   给 proc64 进程发信号（默认 SIGKILL=9；任务管理器进程页回车走的就是它）\n"
    "  rust [tokens|set N]   Rust 模块（gui_rs）：设计 Token + 主题配色；rust set N 切换主题\n"
    "  ---- 多用户 / 会话身份（★ P4：uid/gid/mode 权限位**真的拦截**）----\n"
    "  useradd 名字          建普通用户（uid >= 1000；主目录 /home/名字 = 属主自己、0700）\n"
    "  userdel 名字          删用户记录（主目录保留；root / 最后一个普通用户拒绝）\n"
    "  passwd [名字] [口令]  设口令（省略口令 = 交互式**隐藏输入**；口令写 - 就清掉；只存盐 + SHA-256×1000 轮）\n"
    "  users                 用户表：root（登录界面隐藏）+ 普通用户 + 当前 GUI/会话身份\n"
    "  whoami / id           会话身份：uid/gid/euid/egid 都是真值（su/sudo 会改 euid/egid）\n"
    "  su [-] [名字]         切**会话身份**（默认 root；目标有口令时交互式输口令；GUI 用户名/头像不变）\n"
    "  sudo -i               同上（sudo 只实现了 -i）\n"
    "  exit                  退出 root 会话（回到 GUI 用户）\n"
    "  chmod 模式 路径       改模式（八进制，如 chmod 600 /a.txt；**root 或属主**才行）\n"
    "  chown 用户[:组] 路径  改属主/组（**只有 root** 能改；组缺省 = 用户的 gid）\n"
    "  ls [-l] [路径]        列目录；-l 显示 -rw-r--r-- 属主 组 大小 名字（缺省路径 = /）\n"
    "  tree [路径]           把目录树打到串口（每行带 uid/gid/mode；验收证据）\n"
    "  ls [-l] [路径]        列目录；-l 显示 -rw-r--r-- 属主 组 大小 名字（缺省路径 = /）\n"
    "  loginctl lock         回锁屏（回车/点击后重新登录）\n"
    "没有\"未支持\"命令了：最后两条 update / preload 已接真。\n";
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
// arg = "diag" 时额外调用 task64_diag64()（逐槽一行 + 汇总，见 task64.cpp；串口打点
//   [TASK64] diag slot=... 供人工/脚本核验新增的任务统计 API）。
static void cmd_ps(TerminalState* ts, const char* arg) {
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
    if (arg && st_eq(arg, "diag")) {
        ts_puts(ts, gui64_tr("task64 diag: one line per slot on the serial log ([TASK64] diag slot=...)\n",
                             "task64 诊断：串口每槽一行（[TASK64] diag slot=...）\n"));
        task64_diag64();
    }
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

// 前置声明：ts_put_hex 的实现在本文件后面（display modes 要按 4 位十六进制打印模式号）
static void ts_put_hex(TerminalState* ts, uint64_t v, int digits);

// ==================== display / disp：运行期显示层（kernel/display64.cpp）====================
// display           摘要（实测刷新率 + 来源 + 模式清单条数 + EDID 对比）
// display modes     引导期 loader 放在物理 0x7400 的可用模式清单（<=16 条，真值）
// display hz        刷新率三条来源分别是什么：0x3DA 实测 / CRTC 推算 / EDID 首选时序
// display edid      EDID 显示器身份与首选时序（引导期读进 0x7600 的那 128B）
// 说明：运行期 DDC 再探测**没做**（见 display64.h），这里显示的就是引导期 EDID + 启动时实测。
static bool cmd_display(TerminalState* ts, const char* sub) {
    const Disp64Info* di = display64_info64();
    char hz[16];

    if (!sub || !sub[0]) {
        display64_report64();                                   // 串口同一份结论（自动验收 grep）
        ts_puts(ts, "display: render ");
        ts_put_u64(ts, (uint64_t)fb_width()); ts_puts(ts, "x"); ts_put_u64(ts, (uint64_t)fb_height());
        ts_puts(ts, "  physical ");
        ts_put_u64(ts, (uint64_t)fb_phys_width()); ts_puts(ts, "x"); ts_put_u64(ts, (uint64_t)fb_phys_height());
        ts_puts(ts, "  zoom ");
        ts_put_u64(ts, (uint64_t)fb_get_zoom()); ts_puts(ts, "%\n");
        ts_puts(ts, "  refresh: ");
        if (di->refresh_x10 > 0) {
            display64_refresh_str64(hz, (int)sizeof(hz));
            ts_puts(ts, hz); ts_puts(ts, " Hz (source=");
            ts_puts(ts, display64_src_name64(di->src));
            ts_puts(ts, ")");
        } else {
            ts_puts(ts, "unknown (0x3DA not measurable; no EDID preferred timing)");
        }
        ts_puts(ts, "\n  mode list: ");
        ts_put_u64(ts, (uint64_t)di->mode_count);
        ts_puts(ts, " entries (use 'display modes')\n");
        ts_puts(ts, "  desktop area: ");
        ts_put_u64(ts, (uint64_t)gui64_screen_w()); ts_puts(ts, "x"); ts_put_u64(ts, (uint64_t)gui64_screen_h());
        ts_puts(ts, ", taskbar ");
        ts_put_u64(ts, (uint64_t)gui64_taskbar_h());
        ts_puts(ts, " px, bpp 32\n");
        ts_puts(ts, "  DDC runtime re-probe: not implemented (boot-time EDID only; see display64.h)\n");
        dbg64_line_begin64();
        dbg64_str("[TERM] cmd display modes=");
        dbg64_dec((uint64_t)di->mode_count);
        dbg64_str(" hz=");
        if (di->refresh_x10 > 0) { display64_refresh_str64(hz, (int)sizeof(hz)); dbg64_str(hz); }
        else                     dbg64_str("unknown");
        dbg64_str(" src=");
        dbg64_str(display64_src_name64(di->src));
        dbg64_nl();
        dbg64_line_end64();
        return true;
    }

    if (st_eq(sub, "modes")) {
        ts_puts(ts, "startup VBE mode list (boot loader probe at physical 0x7400):\n");
        int n = display64_mode_count64();
        for (int i = 0; i < n; i++) {
            const Disp64Mode* m = display64_mode_at64(i);
            if (!m) break;
            ts_puts(ts, "  mode=0x");
            ts_put_hex(ts, m->mode, 4);
            ts_puts(ts, "  ");
            ts_put_u64(ts, (uint64_t)m->width); ts_puts(ts, "x"); ts_put_u64(ts, (uint64_t)m->height);
            ts_puts(ts, "x"); ts_put_u64(ts, (uint64_t)m->bpp);
            if (m->mode == di->mode_num) ts_puts(ts, "   <- current");
            ts_putc(ts, (uint32_t)'\n');
        }
        if (n == 0) ts_puts(ts, "  (no mode table from boot loader on this boot path)\n");
        ts_puts(ts, "  (switch is still Bochs VBE DISPI via the settings page; this list is read-only)\n");
        dbg64_line_begin64();
        dbg64_str("[TERM] cmd display modes=");
        dbg64_dec((uint64_t)n);
        dbg64_nl();
        dbg64_line_end64();
        return true;
    }

    if (st_eq(sub, "hz")) {
        ts_puts(ts, "refresh rate sources (measured first, then reported):\n");
        ts_puts(ts, "  0x3DA vretrace measured : ");
        if (di->vga_x10 > 0) {
            ts_put_u64(ts, (uint64_t)(di->vga_x10 / 10)); ts_putc(ts, (uint32_t)'.');
            ts_put_u64(ts, (uint64_t)(di->vga_x10 % 10)); ts_puts(ts, " Hz");
        } else {
            ts_puts(ts, "not measurable (samples=");
            ts_put_u64(ts, (uint64_t)di->vga_samples);
            ts_puts(ts, " edges=");
            ts_put_u64(ts, (uint64_t)di->vga_edges);
            ts_puts(ts, ")");
        }
        ts_putc(ts, (uint32_t)'\n');
        ts_puts(ts, "  CRTC timing (fallback)  : ");
        if (di->crtc_x10 > 0) {
            ts_put_u64(ts, (uint64_t)(di->crtc_x10 / 10)); ts_putc(ts, (uint32_t)'.');
            ts_put_u64(ts, (uint64_t)(di->crtc_x10 % 10)); ts_puts(ts, " Hz");
        } else {
            ts_puts(ts, "not usable (CRTC not programmed for this mode)");
        }
        ts_putc(ts, (uint32_t)'\n');
        ts_puts(ts, "  EDID preferred timing   : ");
        if (di->edid_x10 > 0) {
            edid64_refresh_str64(hz, (int)sizeof(hz));
            ts_puts(ts, hz); ts_puts(ts, " Hz");
        } else {
            ts_puts(ts, "no EDID from firmware");
        }
        ts_putc(ts, (uint32_t)'\n');
        ts_puts(ts, "  adopted                 : ");
        if (di->refresh_x10 > 0) {
            display64_refresh_str64(hz, (int)sizeof(hz));
            ts_puts(ts, hz); ts_puts(ts, " Hz (src=");
            ts_puts(ts, display64_src_name64(di->src)); ts_puts(ts, ")");
        } else {
            ts_puts(ts, "unknown");
        }
        ts_puts(ts, "  measured/EDID match=");
        ts_put_u64(ts, (uint64_t)di->edid_match);
        ts_putc(ts, (uint32_t)'\n');
        dbg64_line_begin64();
        dbg64_str("[TERM] cmd display hz src=");
        dbg64_str(display64_src_name64(di->src));
        dbg64_str(" vga=");
        dbg64_dec((uint64_t)di->vga_x10);
        dbg64_str(" edid=");
        dbg64_dec((uint64_t)di->edid_x10);
        dbg64_str(" match=");
        dbg64_dec((uint64_t)di->edid_match);
        dbg64_nl();
        dbg64_line_end64();
        return true;
    }

    if (st_eq(sub, "edid")) {
        const Edid64* ed = edid64_get();
        if (!ed->valid) {
            ts_puts(ts, "EDID: none from firmware (refresh rate / monitor name unknown)\n");
        } else {
            ts_puts(ts, "EDID (first 128-byte block at physical 0x7600):\n  monitor ");
            ts_puts(ts, ed->name);
            ts_puts(ts, "  mfg="); ts_puts(ts, ed->mfg);
            ts_puts(ts, "  ver=");
            ts_put_u64(ts, (uint64_t)ed->version_major); ts_putc(ts, (uint32_t)'.');
            ts_put_u64(ts, (uint64_t)ed->version_minor);
            ts_puts(ts, "  size=");
            ts_put_u64(ts, (uint64_t)ed->size_cm_w); ts_puts(ts, "x");
            ts_put_u64(ts, (uint64_t)ed->size_cm_h); ts_puts(ts, "cm");
            ts_puts(ts, "  ext=");
            ts_put_u64(ts, (uint64_t)ed->ext_count);
            ts_putc(ts, (uint32_t)'\n');
            ts_puts(ts, "  preferred timing: ");
            ts_put_u64(ts, (uint64_t)ed->h_active); ts_puts(ts, "x"); ts_put_u64(ts, (uint64_t)ed->v_active);
            ts_puts(ts, " @ ");
            edid64_refresh_str64(hz, (int)sizeof(hz));
            ts_puts(ts, hz); ts_puts(ts, " Hz  pclk=");
            ts_put_u64(ts, (uint64_t)ed->pclk_khz); ts_puts(ts, " kHz\n");
            ts_puts(ts, "  (extended blocks and runtime DDC re-probe are NOT implemented - see display64.h)\n");
        }
        dbg64_line_begin64();
        dbg64_str("[TERM] cmd display edid present=");
        dbg64_dec((uint64_t)(ed->valid ? 1 : 0));
        dbg64_nl();
        dbg64_line_end64();
        return true;
    }

    ts_puts(ts, "display: usage: display [modes|hz|edid]\n");
    return false;
}

// ==================== 文件命令（批次 B：全部走 kernel/fd64.cpp 的 FD 层 -> VimtuFS2）====================
// 限制（help 里也如实写）：**路径是多级的**（单段 ≤31B、整条 ≤128B、≤16 层）、单文件 <= 8 MiB；
//   ★ P4：**权限位真的拦**（读要 r、进目录要 x、写要 w —— root 绕过；详见 vfs64.h 的"★ P4 权限"）。
// rm 只能删文件；mkdir 支持多级；写文件是整体覆盖 + 立刻落盘。

// ★ P4：把 mode 变成 ls -l 那样的字符串（"drwxr-xr-x"）；type 用目录位判断。
static void ts_mode_str64(char out[11], uint32_t mode, uint32_t type) {
    out[0] = (type == VFS64_TYPE_DIR) ? 'd' : '-';
    for (int g = 0; g < 3; g++) {
        const uint32_t bits = (mode >> (6 - 3 * g)) & 7u;
        out[1 + 3 * g] = (bits & 4u) ? 'r' : '-';
        out[2 + 3 * g] = (bits & 2u) ? 'w' : '-';
        out[3 + 3 * g] = (bits & 1u) ? 'x' : '-';
    }
    out[10] = 0;
}
// uid/gid -> 名字（本系统 gid = uid 一个主组；root 两侧都是 "root"）；找不到用户时打数字。
static void ts_user_name64(TerminalState* ts, uint32_t uid) {
    const char* n = userdb64_name_for_uid64(uid);
    if (n && n[0]) ts_puts(ts, n);
    else           ts_put_u64(ts, (uint64_t)uid);
}

// ls [-l] [路径]：列目录（缺省 = 当前卷的根目录）。-l 给"权限串 属主 组 大小 名字"。
static void cmd_ls(TerminalState* ts, const char* a1, const char* a2) {
    bool long_fmt = false;
    const char* path = "/";
    if (a1 && a1[0]) {
        if (st_eq(a1, "-l")) { long_fmt = true; if (a2 && a2[0]) path = a2; }
        else path = a1;
    }
    // ★ 根目录单独处理：fd64_norm_path64 是"文件路径"规范化（拒绝 "/"），缺省路径就是 "/"
    if (st_eq(path, "/") || st_eq(path, "//")) {
        g_pathbuf[0] = '/';
        g_pathbuf[1] = 0;
    } else if (fd64_norm_path64(path, g_pathbuf, (int)sizeof(g_pathbuf)) != 0) {
        ts_puts(ts, "ls: bad path (multi-level /dir/sub/name, <=128 B, <=16 segments)\n");
        return;
    }
    // 只用目录句柄列一次（fd64 内部把这次扫描缓存进句柄；不再额外做一次 vfs64_ls +
    // 每个条目一次 stat —— 那在真机上是秒级 I/O，会把 GUI 看门狗饿到）。
    const int dfd = fd64_opendir64(g_pathbuf);
    if (dfd < 3) {
        ts_puts(ts, gui64_tr("ls: cannot open ", "ls: 打不开 "));
        ts_puts(ts, g_pathbuf);
        ts_puts(ts, gui64_tr(" (missing / denied / not a directory / no volume)\n",
                             "（不存在 / 被权限拒绝 / 不是目录 / 没有卷）\n"));
        dbg64_line_begin64();
        dbg64_str("[TERM] cmd ls entries=0 denied-or-missing path=");
        dbg64_str(g_pathbuf);
        dbg64_nl();
        dbg64_line_end64();
        return;
    }
    ts_puts(ts, fs64_is_readonly64(-1)
                 ? "current volume (FAT32, read-only):\n"
                 : "current volume (VimtuFS2):\n");
    int rows = 0;
    uint64_t bytes = 0;
    for (;;) {
        char nm[FD64_NAME_MAX];
        uint32_t ty = 0, sz = 0;
        const int r = fd64_readdir64(dfd, nm, (int)sizeof(nm), &ty, &sz);
        if (r <= 0) break;
        if (long_fmt) {
            // 长格式：每个条目一次 stat（-- P4 的 uid/gid/mode 只能从 stat 拿）
            char full[VFS64_PATH_MAX + VFS64_NAME_MAX + 4];
            int f = 0;
            for (int i = 0; g_pathbuf[i] && f < (int)sizeof(full) - 2; i++) full[f++] = g_pathbuf[i];
            if (f == 0 || full[f - 1] != '/') full[f++] = '/';
            for (int i = 0; nm[i] && f < (int)sizeof(full) - 1; i++) full[f++] = nm[i];
            full[f] = 0;
            Fs64Stat64 si;
            uint32_t m = 0, uid = 0, gid = 0;
            if (fs64_stat64(-1, full, &si) == 0) { m = si.mode; uid = si.uid; gid = si.gid; }
            char ms[11];
            ts_mode_str64(ms, m, ty);
            ts_puts(ts, ms);
            ts_puts(ts, "  ");
            ts_user_name64(ts, uid);
            ts_puts(ts, " ");
            ts_user_name64(ts, gid);
            ts_puts(ts, "  ");
            ts_put_u64(ts, (uint64_t)sz);
            ts_puts(ts, "  ");
            ts_puts(ts, nm);
            ts_putc(ts, (uint32_t)'\n');
            // ★ 串口证据行（自动验收 grep；有界：目录句柄缓存最多 16 条，不会刷屏）
            dbg64_line_begin64();
            dbg64_str("[TERM] ls -l ");
            dbg64_str(full);
            dbg64_str(" ");
            dbg64_str(ms);
            dbg64_str(" uid=");
            dbg64_dec(uid);
            dbg64_str(" gid=");
            dbg64_dec(gid);
            dbg64_str(" user=");
            dbg64_str(userdb64_name_for_uid64(uid)[0] ? userdb64_name_for_uid64(uid) : "-");
            dbg64_str(" group=");
            dbg64_str(userdb64_name_for_uid64(gid)[0] ? userdb64_name_for_uid64(gid) : "-");
            dbg64_str(" size=");
            dbg64_dec(sz);
            dbg64_nl();
            dbg64_line_end64();
        } else {
            ts_puts(ts, "  ");
            ts_puts_pad(ts, nm, 20);
            ts_put_u64(ts, (uint64_t)sz);
            ts_puts(ts, ty == VFS64_TYPE_DIR ? " B  <DIR>\n" : " B\n");
        }
        bytes += sz;
        rows++;
    }
    (void)fd64_close64(dfd);
    ts_puts(ts, "  total: ");
    ts_put_u64(ts, (uint64_t)rows);
    ts_puts(ts, " entries, ");
    ts_put_u64(ts, bytes);
    ts_puts(ts, " bytes\n");
    dbg64_line_begin64();
    dbg64_str("[TERM] cmd ls entries=");
    dbg64_dec((uint64_t)rows);
    dbg64_str(" bytes=");
    dbg64_dec(bytes);
    dbg64_str(" path=");
    dbg64_str(g_pathbuf);
    dbg64_str(long_fmt ? " long=1" : " long=0");
    dbg64_nl();
    dbg64_line_end64();
}

// ★ P4：chmod MODE PATH（八进制；root 或属主）。越权/旧卷/不存在都如实报错（不假装成功）。
static bool cmd_chmod(TerminalState* ts, const char* mode_s, const char* path) {
    if (!mode_s || !mode_s[0] || !path || !path[0]) {
        ts_puts(ts, "chmod: usage: chmod MODE PATH   (MODE is octal, e.g. 600 / 644 / 755)\n");
        return false;
    }
    uint32_t mode = 0;
    int digits = 0;
    for (int i = 0; mode_s[i]; i++) {
        if (mode_s[i] < '0' || mode_s[i] > '7') { ts_puts(ts, "chmod: MODE must be octal (0..7 per digit)\n"); return false; }
        mode = mode * 8u + (uint32_t)(mode_s[i] - '0');
        digits++;
        if (digits > 4) break;
    }
    if (digits == 0) { ts_puts(ts, "chmod: MODE must be octal\n"); return false; }
    if (fd64_norm_path64(path, g_pathbuf, (int)sizeof(g_pathbuf)) != 0) {
        ts_puts(ts, "chmod: bad path (multi-level /dir/sub/name)\n");
        return false;
    }
    const int rc = fs64_chmod64(-1, g_pathbuf, mode);
    if (rc == 0) {
        ts_puts(ts, "chmod: ok mode=");
        ts_put_u64(ts, (uint64_t)mode);
        ts_puts(ts, " path=");
        ts_puts(ts, g_pathbuf);
        ts_putc(ts, (uint32_t)'\n');
        return true;
    }
    if (rc == -13) ts_puts(ts, "chmod: permission denied (only the owner or root can chmod)\n");
    else           ts_puts(ts, "chmod: failed (not found / only root or the owner / volume v2-v3 has no mode field)\n");
    return false;
}
// ★ P4：chown USER[:GROUP] PATH（只有 root 能改；组缺省 = 该用户的 gid，本系统 gid = uid）
static bool cmd_chown(TerminalState* ts, const char* owner_s, const char* path) {
    if (!owner_s || !owner_s[0] || !path || !path[0]) {
        ts_puts(ts, "chown: usage: chown USER[:GROUP] PATH   (only root can chown)\n");
        return false;
    }
    char user[USERDB64_NAME_MAX];
    char group[USERDB64_NAME_MAX];
    int n = 0;
    while (owner_s[n] && owner_s[n] != ':' && n < (int)sizeof(user) - 1) { user[n] = owner_s[n]; n++; }
    user[n] = 0;
    group[0] = 0;
    if (owner_s[n] == ':') {
        int g = 0;
        for (int i = n + 1; owner_s[i] && g < (int)sizeof(group) - 1; i++) group[g++] = owner_s[i];
        group[g] = 0;
    }
    const int idx = userdb64_find64(user);
    if (idx < 0) { ts_puts(ts, "chown: no such user: "); ts_puts(ts, user); ts_putc(ts, (uint32_t)'\n'); return false; }
    const User64Entry* e = userdb64_at64(idx);
    const uint32_t uid = e ? e->uid : 0u;
    uint32_t gid = (uid == 0) ? 0u : uid;
    if (group[0]) {
        const int gi = userdb64_find64(group);
        if (gi < 0) { ts_puts(ts, "chown: no such group: "); ts_puts(ts, group); ts_putc(ts, (uint32_t)'\n'); return false; }
        const User64Entry* ge = userdb64_at64(gi);
        gid = (ge && ge->uid != 0) ? ge->uid : 0u;
    }
    if (fd64_norm_path64(path, g_pathbuf, (int)sizeof(g_pathbuf)) != 0) {
        ts_puts(ts, "chown: bad path (multi-level /dir/sub/name)\n");
        return false;
    }
    const int rc = fs64_chown64(-1, g_pathbuf, uid, gid);
    if (rc == 0) {
        ts_puts(ts, "chown: ok uid=");
        ts_put_u64(ts, (uint64_t)uid);
        ts_puts(ts, " gid=");
        ts_put_u64(ts, (uint64_t)gid);
        ts_puts(ts, " path=");
        ts_puts(ts, g_pathbuf);
        ts_putc(ts, (uint32_t)'\n');
        return true;
    }
    if (rc == -13) ts_puts(ts, "chown: permission denied (only root can chown)\n");
    else           ts_puts(ts, "chown: failed (only root can chown / not found / volume v2-v3 has no uid-gid fields)\n");
    return false;
}
// ★ P4：umask [掩码]（八进制；无参数 = 显示当前值）。只影响**新建**文件/目录的模式。
// 屏幕上与串口里都按**3 位八进制**打印（0xx），与 chmod 的口径一致。
static void ts_put_oct3(TerminalState* ts, uint32_t v) {
    char b[4];
    b[0] = (char)('0' + ((v >> 6) & 7u));
    b[1] = (char)('0' + ((v >> 3) & 7u));
    b[2] = (char)('0' + (v & 7u));
    b[3] = 0;
    ts_puts(ts, b);
}
static bool cmd_umask(TerminalState* ts, const char* arg) {
    if (!arg || !arg[0]) {
        ts_puts(ts, "umask: current = 0");
        ts_put_oct3(ts, vfs64_get_umask64());
        ts_puts(ts, " (octal; new files 666 & ~umask, new dirs 777 & ~umask)\\n");
        dbg64_line_begin64();
        dbg64_str("[TERM] cmd umask value=0");
        {
            char b[4];
            const uint32_t v = vfs64_get_umask64();
            b[0] = (char)('0' + ((v >> 6) & 7u));
            b[1] = (char)('0' + ((v >> 3) & 7u));
            b[2] = (char)('0' + (v & 7u));
            b[3] = 0;
            dbg64_str(b);
        }
        dbg64_nl();
        dbg64_line_end64();
        return true;
    }
    uint32_t m = 0;
    for (int i = 0; arg[i]; i++) {
        if (arg[i] < '0' || arg[i] > '7') { ts_puts(ts, "umask: MASK must be octal (0..7 per digit)\\n"); return false; }
        m = m * 8u + (uint32_t)(arg[i] - '0');
    }
    const uint32_t old = vfs64_umask64(m);
    ts_puts(ts, "umask: 0");
    ts_put_oct3(ts, old);
    ts_puts(ts, " -> 0");
    ts_put_oct3(ts, vfs64_get_umask64());
    ts_putc(ts, (uint32_t)'\n');
    dbg64_line_begin64();
    dbg64_str("[TERM] cmd umask set old=0");
    {
        char b[4];
        b[0] = (char)('0' + ((old >> 6) & 7u));
        b[1] = (char)('0' + ((old >> 3) & 7u));
        b[2] = (char)('0' + (old & 7u));
        b[3] = 0;
        dbg64_str(b);
    }
    dbg64_str(" new=0");
    {
        char b[4];
        const uint32_t v = vfs64_get_umask64();
        b[0] = (char)('0' + ((v >> 6) & 7u));
        b[1] = (char)('0' + ((v >> 3) & 7u));
        b[2] = (char)('0' + (v & 7u));
        b[3] = 0;
        dbg64_str(b);
    }
    dbg64_nl();
    dbg64_line_end64();
    return true;
}

// ★ P4：tree [路径] —— 把目录树打到串口（每行带 uid/gid/mode；验收证据 + 诊断）
static bool cmd_tree(TerminalState* ts, const char* arg) {
    const char* p = (arg && arg[0]) ? arg : "/";
    if (fd64_norm_path64(p, g_pathbuf, (int)sizeof(g_pathbuf)) != 0) {
        ts_puts(ts, "tree: bad path (multi-level /dir/sub/name)\n");
        return false;
    }
    const int n = vfs64_tree_dump64(g_pathbuf, 64, 4);
    if (n < 0) {
        ts_puts(ts, "tree: not found / not a directory / permission denied (see serial [VFS64] lines)\n");
        return false;
    }
    ts_puts(ts, "tree: entries=");
    ts_put_u64(ts, (uint64_t)n);
    ts_puts(ts, " (serial: [VFS64] tree <path> ... uid= gid= mode=)\n");
    return true;
}

// cat FILE：真读（FD 层 -> vfs64 -> 磁盘）
// ★ 批次 M：单文件上限已经是 8 MiB，**cat 不再无界刷屏**：最多打 CAT_MAX_OUT 字节，超过就明确提示
//   "只显示前 N 字节 + 全文 CRC / 大小"（**不是静默截断**：屏幕上与串口里都有 truncated=1 打点）。
#define CAT_MAX_OUT 4096
static bool cmd_cat(TerminalState* ts, const char* name) {
    if (!name || !name[0]) { ts_puts(ts, "cat: usage: cat FILE\n"); return false; }
    if (fd64_norm_path64(name, g_pathbuf, (int)sizeof(g_pathbuf)) != 0) {
        ts_puts(ts, "cat: bad path (multi-level /dir/sub/name, <=128 B, <=16 segments)\n");
        return false;
    }
    const int fd = fd64_open64(g_pathbuf, FD64_O_RDONLY);
    if (fd < 3) {
        ts_puts(ts, "cat: cannot open ");
        ts_puts(ts, g_pathbuf);
        ts_puts(ts, " (missing / is a directory / no volume)\n");

        return false;
    }
    // ★ 批次 M：**先 stat 拿真实大小**，最多只读/打 CAT_MAX_OUT 字节 —— 不再"读完整个文件来数 total"
    //   （8 MiB 的文件那样读要几万次扇区读，会把 GUI 看门狗饿到 —— 这是实测踩过的坑）。
    static char cbuf[512];
    uint32_t total = 0;
    {
        Fs64Stat64 st;
        if (fs64_stat64(-1, g_pathbuf, &st) == 0 && st.type == VFS64_TYPE_FILE) total = st.size;
    }
    int printed = 0;
    while (printed < (int)CAT_MAX_OUT) {
        int want = (int)CAT_MAX_OUT - printed;
        if (want > (int)sizeof(cbuf) - 1) want = (int)sizeof(cbuf) - 1;
        const int r = fd64_read64(fd, cbuf, want);
        if (r <= 0) break;
        cbuf[r] = 0;
        ts_puts(ts, cbuf);
        printed += r;
    }
    (void)fd64_close64(fd);
    const bool truncated = (total > (uint32_t)printed);
    ts_putc(ts, (uint32_t)'\n');
    if (truncated) {
        // 明确提示（不是静默截断）：屏幕上写清"还有多少没显示"和怎么拿全文 CRC
        char m[176];
        int n = 0;
        const char* a = "cat: output truncated (file is ";
        for (int i = 0; a[i] && n < 120; i++) m[n++] = a[i];
        { char t[16]; int k = 0; uint32_t v = total; if (!v) t[k++] = '0'; while (v && k < 15) { t[k++] = (char)('0' + (v % 10)); v /= 10; } while (k) m[n++] = t[--k]; }
        const char* b = " B; showed first ";
        for (int i = 0; b[i] && n < 130; i++) m[n++] = b[i];
        { char t[16]; int k = 0; uint32_t v = (uint32_t)printed; if (!v) t[k++] = '0'; while (v && k < 15) { t[k++] = (char)('0' + (v % 10)); v /= 10; } while (k) m[n++] = t[--k]; }
        const char* c2 = " B - use 'fatcheck ";
        for (int i = 0; c2[i] && n < 150; i++) m[n++] = c2[i];
        for (int i = 0; g_pathbuf[i] && n < 160; i++) m[n++] = g_pathbuf[i];
        const char* d2 = "' for the full CRC)\n";
        for (int i = 0; d2[i] && n < 175; i++) m[n++] = d2[i];
        m[n] = 0;
        ts_puts(ts, m);
    }
    dbg64_line_begin64();
    dbg64_str("[TERM] cmd cat bytes=");
    dbg64_dec((uint64_t)printed);
    dbg64_str(" total=");
    dbg64_dec((uint64_t)total);
    dbg64_str(" truncated=");
    dbg64_dec(truncated ? 1u : 0u);
    dbg64_nl();
    dbg64_line_end64();
    return true;
}
// fatcheck [PATH]：把当前卷上的文件按块读一遍算 CRC32（IEEE，zlib 同多项式），串口打
//   [FAT64] crc path=<p> size=<n> crc32=<HEX8>
// 不带参数时核对 ESP 的三个文件（EFI/BOOT/BOOTX64.EFI、UEFI64.BIN、KERNEL64.BIN）——
// 自动验收拿宿主侧对 build64/ 构建产物算的同一个 CRC 逐项比对（最强证据：读出来的字节一致）。
// 只读：不写盘；CRC32 用逐位实现（内核无表/无 SSE）。
static bool cmd_fatcheck(TerminalState* ts, const char* arg) {
    static const char* fixed[3] = { "EFI/BOOT/BOOTX64.EFI", "UEFI64.BIN", "KERNEL64.BIN" };
    const char* one = (arg && arg[0]) ? arg : nullptr;
    ts_puts(ts, "fatcheck (read-only CRC32 of the current volume; byte-for-byte vs build64/):\n");
    // ★ 大文件（4MB 的 KERNEL64.BIN）按簇链读要几十次 PIO —— 会超过 5 秒心跳阈值：
    //   与 ping 一样暂停看门狗（不是 bug，是长操作）。
    panic64_watchdog_pause64();
    int checked = 0, failed = 0;
    for (int k = 0; k < (one ? 1 : 3); k++) {
        const char* p = one ? one : fixed[k];
        char path[FD64_PATH_MAX];
        int pl = 0;
        if (p[0] != '/' && pl < (int)sizeof(path) - 1) path[pl++] = '/';
        for (int i = 0; p[i] && pl < (int)sizeof(path) - 1; i++) path[pl++] = p[i];
        path[pl] = 0;
        uint32_t size = 0;
        const uint32_t crc = fs64_crc32_file64(-1, path, &size, FAT64_READ_MAX_BYTES);
        ts_puts(ts, "  ");
        ts_puts_pad(ts, path, 24);
        if (crc == 0) {
            ts_puts(ts, "  MISSING / too large (no CRC)\n");
            dbg64_line_begin64();
            dbg64_str("[FAT64] crc path=");
            dbg64_str(path);
            dbg64_str(" size=0 crc32=00000000 (missing)");
            dbg64_nl();
            dbg64_line_end64();
            failed++;
            continue;
        }
        ts_puts(ts, "  size=");
        ts_put_u64(ts, (uint64_t)size);
        ts_puts(ts, "  crc32=0x");
        {
            static const char* H = "0123456789ABCDEF";
            char hx[9];
            for (int i = 0; i < 8; i++) hx[i] = H[(crc >> ((7 - i) * 4)) & 0xF];
            hx[8] = 0;
            ts_puts(ts, hx);
        }
        ts_putc(ts, (uint32_t)'\n');
        dbg64_line_begin64();
        dbg64_str("[FAT64] crc path=");
        dbg64_str(path);
        dbg64_str(" size=");
        dbg64_dec((uint64_t)size);
        dbg64_str(" crc32=");
        dbg64_hex64((uint64_t)crc);
        dbg64_nl();
        dbg64_line_end64();
        checked++;
    }
    ts_puts(ts, "  checked=");
    ts_put_u64(ts, (uint64_t)checked);
    ts_puts(ts, " failed=");
    ts_put_u64(ts, (uint64_t)failed);
    panic64_watchdog_unpause64();
    return failed == 0;
    return failed == 0;
}

// ==================== ★ 批次 M：bigtest（大文件 / 二级间接块 / 上限边界 / 回收）====================
// 为什么放在终端里：大文件只能由**盘上跑着的内核自己生成**（8 MiB 没法用键盘敲进去），
// 自动验收（tests/bigfile64_test.py）需要一条能复现、能打点的路径。本命令**只走公共上层 API**
// （fd64 -> fs64 -> vfs64），所以它跑通就等于"终端/应用层读写大文件跑通"。
// 字节模式 pat64b 与 kernel/vfs64.cpp 自检、宿主侧 tests/bigfile64_test.py 用**同一个公式**，
// 所以宿主可以独立重算 CRC，再解析 raw 镜像逐块核对（不是只看内核自己报的数）。
// 打点（自动验收 grep，格式勿改）：
//   [BIG64] write path=<p> bytes=<n> blocks=<n> free_before=<n> free_after=<n> crc32=0x<hex> rc=0
//   [BIG64] over path=<p> rc=<n> size_before=<n> size_after=<n> partial=0 crc_ok=<0|1>
//   [BIG64] recycle rounds=<n> write_bytes=<n> free_before=<n> free_after=<n> delta=<n>
//   [BIG64] selftest ok mask=0   /   [BIG64] selftest FAIL mask=<n>
#define BIG64_CHUNK 65536u
static uint8_t g_big64_buf[BIG64_CHUNK];
static uint8_t pat64b(uint32_t off) {                 // 与 vfs64.cpp 的 pat64 / 宿主侧 pat64 同公式
    return (uint8_t)((off * 31u + (off >> 8) * 7u + (off >> 16) * 11u + 0xA5u) & 0xFFu);
}
static void big64_fill(uint32_t off, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) g_big64_buf[i] = pat64b(off + i);
}
static uint32_t big64_crc_step(uint32_t crc, const uint8_t* p, uint32_t n) {   // zlib 口径
    crc = ~crc;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}
static uint32_t big64_expected_crc(uint32_t size) {
    uint32_t crc = 0, off = 0;
    while (off < size) {
        uint32_t n = size - off;
        if (n > BIG64_CHUNK) n = BIG64_CHUNK;
        big64_fill(off, n);
        crc = big64_crc_step(crc, g_big64_buf, n);
        off += n;
    }
    return crc;
}
static void big64_hex8(char* out, uint32_t v) {
    static const char* H = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++) out[i] = H[(v >> ((7 - i) * 4)) & 0xF];
    out[8] = 0;
}
// 用 fd 层分块写（调用方缓冲 64KB；不准备整文件缓冲）——走的正是终端/复制粘贴的同一条写路径
static int big64_write(const char* path, uint32_t size) {
    const int fd = fd64_open64(path, FD64_O_WRONLY | FD64_O_CREAT | FD64_O_TRUNC);
    if (fd < 3) return -1;
    uint32_t off = 0;
    int bad = 0;
    while (off < size) {
        uint32_t n = size - off;
        if (n > BIG64_CHUNK) n = BIG64_CHUNK;
        big64_fill(off, n);
        if (fd64_write64(fd, g_big64_buf, (int)n) != (int)n) { bad = 1; break; }
        off += n;
    }
    (void)fd64_close64(fd);
    return bad ? -1 : 0;
}
static uint32_t big64_free() {
    uint32_t fb = 0;
    if (fs64_free64(-1, &fb, nullptr, nullptr) != 0) return 0xFFFFFFFFu;
    return fb;
}
// 分块读回**逐字节**比对（不是只看 CRC：逐块比对能指出第一处不一致的偏移）
static int big64_verify(const char* path, uint32_t size, uint32_t* out_crc, uint32_t* out_bad) {
    const int fd = fd64_open64(path, FD64_O_RDONLY);
    if (fd < 3) return -1;
    uint32_t off = 0, crc = 0, bad = 0xFFFFFFFFu;
    while (off < size) {
        uint32_t n = size - off;
        if (n > BIG64_CHUNK) n = BIG64_CHUNK;
        const int r = fd64_read64(fd, g_big64_buf, (int)n);
        if (r <= 0) { (void)fd64_close64(fd); return -1; }
        for (int i = 0; i < r; i++)
            if (g_big64_buf[i] != pat64b(off + (uint32_t)i) && bad == 0xFFFFFFFFu) bad = off + (uint32_t)i;
        crc = big64_crc_step(crc, g_big64_buf, (uint32_t)r);
        off += (uint32_t)r;
    }
    (void)fd64_close64(fd);
    if (out_crc) *out_crc = crc;
    if (out_bad) *out_bad = bad;
    return 0;
}
// bigtest copy <src> <字母>：把 src 复制到该盘符的根目录 —— **走文件管理器粘贴用的同一段代码**
// （explorer64_copy_file64：空间预检 + 分块 read_range/write_at）；跨卷（C: -> D:）也一样。
static int big64_vol_of_letter(char L) {
    for (int i = 0; i < drive64_count64(); i++) {
        DriveInfo64 di;
        if (drive64_info64(i, &di) != 0) continue;
        if (di.letter == L && di.vol >= 0) return di.vol;
    }
    return -1;
}
static bool cmd_bigtest_copy(TerminalState* ts, const char* rest) {
    // 解析 "  <src>  <字母>"
    char src[FD64_PATH_MAX];
    int sn = 0;
    const char* p = rest;
    while (p && (*p == ' ' || *p == '\t')) p++;
    for (; p && *p && *p != ' ' && *p != '\t' && sn < (int)sizeof(src) - 1; p++) src[sn++] = *p;
    src[sn] = 0;
    while (p && (*p == ' ' || *p == '\t')) p++;
    if (sn == 0 || !p || !*p) {
        ts_puts(ts, "bigtest copy: usage: bigtest copy /big1.bin D\n");
        dbg64_line_begin64();
        dbg64_str("[BIG64] copy FAILED reason=args src=");
        dbg64_str(src);
        dbg64_str(" rest=\"");
        dbg64_str(rest ? rest : "(null)");
        dbg64_str("\"");
        dbg64_nl();
        dbg64_line_end64();
        return false;
    }
    char L = (*p >= 'a' && *p <= 'z') ? (char)(*p - 'a' + 'A') : *p;
    const int dvol = big64_vol_of_letter(L);
    const int ro = fs64_is_readonly64(-1);
    dbg64_line_begin64();
    dbg64_str("[BIG64] copy-args src=");
    dbg64_str(src);
    dbg64_str(" letter=");
    dbg64_putc((char)L);
    dbg64_str(" dvol=");
    dbg64_dec((uint64_t)(dvol < 0 ? 0xFFFFFFFFu : (uint32_t)dvol));
    dbg64_str(" cur_ro=");
    dbg64_dec((uint64_t)(ro ? 1 : 0));
    dbg64_nl();
    dbg64_line_end64();
    if (ro) { ts_puts(ts, "bigtest copy: current volume is read-only\n"); return false; }
    if (dvol < 0) { ts_puts(ts, "bigtest copy: no such drive letter\n"); return false; }
    char dst[FD64_PATH_MAX];
    int n = 0;
    dst[n++] = '/';
    {
        const char* base = src;
        for (const char* p = src; *p; p++) if (*p == '/') base = p + 1;
        for (int i = 0; base[i] && n < (int)sizeof(dst) - 1; i++) dst[n++] = base[i];
    }
    dst[n] = 0;
    char norm[FD64_PATH_MAX];
    if (fd64_norm_path64(src, norm, (int)sizeof(norm)) != 0) { ts_puts(ts, "bigtest copy: bad src path\\n"); return false; }
    panic64_watchdog_pause64();
    int why = 0;
    const int rc = explorer64_copy_file64(fs64_current_vol64(), norm, dvol, dst, &why);
    Fs64Stat64 st;
    uint32_t crc = 0, size = 0;
    const bool have_dst = (fs64_stat64(dvol, dst, &st) == 0);
    if (have_dst) { size = st.size; crc = fs64_crc32_file64(dvol, dst, nullptr, FAT64_READ_MAX_BYTES); }
    dbg64_line_begin64();
    dbg64_str("[BIG64] copy src=");
    dbg64_str(norm);
    dbg64_str(" dst_vol=");
    dbg64_dec((uint64_t)dvol);
    dbg64_str(" dst=");
    dbg64_str(dst);
    dbg64_str(" bytes=");
    dbg64_dec(size);
    dbg64_str(" crc32=0x");
    { char hx[9]; big64_hex8(hx, crc); dbg64_str(hx); }
    dbg64_str(" rc=");
    dbg64_dec((uint64_t)(rc == 0 ? 0 : 1));
    dbg64_str(" why=");
    dbg64_dec((uint64_t)why);
    dbg64_nl();
    dbg64_line_end64();
    ts_puts(ts, rc == 0 ? "bigtest copy: ok (see [BIG64] copy on serial)\\n"
                        : "bigtest copy: FAILED (see [BIG64] copy ... why= on serial)\\n");
    panic64_watchdog_unpause64();
    return rc == 0;
}
static bool cmd_bigtest(TerminalState* ts, const char* arg) {
    const char* mode = (arg && arg[0]) ? arg : "all";
    if (st_eq(mode, "copy")) return cmd_bigtest_copy(ts, (arg && arg[0]) ? arg + 4 : "");
    if (fs64_is_readonly64(-1)) { ts_puts(ts, "bigtest: read-only volume (FAT32)\n"); return false; }
    const uint32_t MB1 = 1024u * 1024u;
    const uint32_t BIG = VFS64_MAX_FILE_BYTES - VFS64_BLOCK_BYTES;      // 上限 - 512B
    // 「文件管理器里的大小显示」：用的是 explorer64.cpp 的 fmt_bytes64（图标/详细视图 + 属性面板同一份）
    {
        char s1[24], s2[24];
        explorer64_fmt_bytes64(MB1, s1, (int)sizeof(s1));
        explorer64_fmt_bytes64(BIG, s2, (int)sizeof(s2));
        dbg64_line_begin64();
        dbg64_str("[BIG64] fmt bytes=");
        dbg64_dec(MB1);
        dbg64_str(" text=");
        dbg64_str(s1);
        dbg64_str(" | bytes=");
        dbg64_dec(BIG);
        dbg64_str(" text=");
        dbg64_str(s2);
        dbg64_nl();
        dbg64_line_end64();
        ts_puts(ts, "bigtest: explorer size display (same formatter as the UI)\\n");
    }
    panic64_watchdog_pause64();                     // 大文件 I/O 是长操作（与 fatcheck 同口径）
    char hx[9];
    int fails = 0;

    if (st_eq(mode, "all") || st_eq(mode, "1mb")) {
        ts_puts(ts, "bigtest 1mb: write+verify 1 MiB through fd64/fs64/vfs64\\n");
        const uint32_t f0 = big64_free();
        const uint32_t need = vfs64_blocks_for_bytes64(MB1);
        const int w = big64_write("/big1.bin", MB1);
        const uint32_t f1 = big64_free();
        uint32_t crc = 0, bad = 0;
        const int v = (w == 0) ? big64_verify("/big1.bin", MB1, &crc, &bad) : -1;
        const uint32_t exp = big64_expected_crc(MB1);
        if (w != 0 || v != 0 || crc != exp || bad != 0xFFFFFFFFu || f0 < f1 || (f0 - f1) != need) fails |= 1;
        big64_hex8(hx, crc);
        dbg64_line_begin64();
        dbg64_str("[BIG64] write path=/big1.bin bytes=");
        dbg64_dec(MB1);
        dbg64_str(" blocks=");
        dbg64_dec(need);
        dbg64_str(" free_before=");
        dbg64_dec(f0);
        dbg64_str(" free_after=");
        dbg64_dec(f1);
        dbg64_str(" crc32=0x");
        dbg64_str(hx);
        dbg64_str(" rc=");
        dbg64_dec((uint64_t)((w == 0 && v == 0 && crc == exp) ? 0u : 1u));
        dbg64_nl();
        dbg64_line_end64();
    }
    if (st_eq(mode, "all") || st_eq(mode, "8mb")) {
        ts_puts(ts, "bigtest 8mb: write+verify (limit-512B) through fd64/fs64/vfs64\\n");
        const uint32_t f0 = big64_free();
        const uint32_t need = vfs64_blocks_for_bytes64(BIG);
        const int w = big64_write("/big8.bin", BIG);
        const uint32_t f1 = big64_free();
        uint32_t crc = 0, bad = 0;
        const int v = (w == 0) ? big64_verify("/big8.bin", BIG, &crc, &bad) : -1;
        const uint32_t exp = big64_expected_crc(BIG);
        if (w != 0 || v != 0 || crc != exp || bad != 0xFFFFFFFFu || f0 < f1 || (f0 - f1) != need) fails |= 2;
        big64_hex8(hx, crc);
        dbg64_line_begin64();
        dbg64_str("[BIG64] write path=/big8.bin bytes=");
        dbg64_dec(BIG);
        dbg64_str(" blocks=");
        dbg64_dec(need);
        dbg64_str(" free_before=");
        dbg64_dec(f0);
        dbg64_str(" free_after=");
        dbg64_dec(f1);
        dbg64_str(" crc32=0x");
        dbg64_str(hx);
        dbg64_str(" rc=");
        dbg64_dec((uint64_t)((w == 0 && v == 0 && crc == exp) ? 0u : 1u));
        dbg64_nl();
        dbg64_line_end64();
    }
    if (st_eq(mode, "all") || st_eq(mode, "limit")) {
        // 上限边界：先补到**正好 8 MiB**（允许），再多写 1 字节 -> 必须被拒（-EFBIG）。
        // 两次写之后都要验：大小没变、整文件内容仍逐字节等于模式（"不产生半截文件"）。
        ts_puts(ts, "bigtest limit: grow to exactly the 8 MiB cap, then 1 byte more (must be refused)\\n");
        Fs64Stat64 st;
        const uint32_t MAXB = VFS64_MAX_FILE_BYTES;
        if (fs64_stat64(-1, "/big8.bin", &st) != 0) {
            ts_puts(ts, "  /big8.bin missing (run 'bigtest 8mb' first)\\n");
            fails |= 4;
        } else {
            uint32_t fill = (MAXB > st.size) ? (MAXB - st.size) : 0u;          // 还差多少到上限（512 B）
            int rc_fill = 0;
            if (fill > 0) {                                                    // ① 补到正好上限：允许
                const int fd = fd64_open64("/big8.bin", FD64_O_WRONLY);
                if (fd < 3) rc_fill = -1;
                else {
                    (void)fd64_lseek64(fd, (int64_t)st.size, FD64_SEEK_SET);
                    uint32_t o = 0;
                    while (o < fill) {
                        uint32_t n = fill - o;
                        if (n > BIG64_CHUNK) n = BIG64_CHUNK;
                        for (uint32_t i = 0; i < n; i++) g_big64_buf[i] = pat64b(st.size + o + i);
                        if (fd64_write64(fd, g_big64_buf, (int)n) != (int)n) { rc_fill = -2; break; }
                        o += n;
                    }
                    (void)fd64_close64(fd);
                }
            }
            // ② 站在**正好上限**的末尾再写 1 字节：必须被拒
            int rc = -1;
            const int fd2 = fd64_open64("/big8.bin", FD64_O_WRONLY);
            if (fd2 >= 3) {
                (void)fd64_lseek64(fd2, (int64_t)MAXB, FD64_SEEK_SET);
                g_big64_buf[0] = 0x5A;
                rc = fd64_write64(fd2, g_big64_buf, 1);
                (void)fd64_close64(fd2);
            }
            Fs64Stat64 st2;
            const bool stat_ok = (fs64_stat64(-1, "/big8.bin", &st2) == 0);
            uint32_t crc2 = 0;
            const int v2 = big64_verify("/big8.bin", MAXB, &crc2, nullptr);
            const uint32_t exp2 = big64_expected_crc(MAXB);
            const bool ok = (rc_fill == 0) && (rc < 0) && stat_ok && st2.size == MAXB && v2 == 0 && crc2 == exp2;
            if (!ok) fails |= 4;
            dbg64_line_begin64();
            dbg64_str("[BIG64] over path=/big8.bin rc=");
            dbg64_dec((uint64_t)(rc < 0 ? -rc : rc));
            dbg64_str(" size_before=");
            dbg64_dec(MAXB);
            dbg64_str(" size_after=");
            dbg64_dec(stat_ok ? st2.size : 0xFFFFFFFFu);
            dbg64_str(" partial=0 crc_ok=");
            dbg64_dec((crc2 == exp2) ? 1u : 0u);
            dbg64_str(" free=");
            dbg64_dec(big64_free());
            dbg64_nl();
            dbg64_line_end64();
        }
    }
    if (st_eq(mode, "all") || st_eq(mode, "recycle")) {
        ts_puts(ts, "bigtest recycle: write/delete x2, free blocks must return to baseline\\n");
        const uint32_t f0 = big64_free();
        for (int round = 0; round < 2; round++) {
            if (big64_write("/rec64.bin", 2u * MB1) != 0) fails |= 8;
            if (fs64_unlink64(-1, "/rec64.bin") != 0) fails |= 8;
        }
        const uint32_t f1 = big64_free();
        if (f0 != f1) fails |= 8;
        dbg64_line_begin64();
        dbg64_str("[BIG64] recycle rounds=2 write_bytes=");
        dbg64_dec(2u * MB1);
        dbg64_str(" free_before=");
        dbg64_dec(f0);
        dbg64_str(" free_after=");
        dbg64_dec(f1);
        dbg64_str(" delta=");
        dbg64_dec((f0 > f1) ? (f0 - f1) : (f1 - f0));
        dbg64_nl();
        dbg64_line_end64();
    }
    dbg64_line_begin64();
    dbg64_str("[BIG64] selftest ");
    dbg64_str(fails == 0 ? "ok mask=0" : "FAIL mask=");
    if (fails != 0) dbg64_dec((uint64_t)fails);
    dbg64_nl();
    dbg64_line_end64();
    ts_puts(ts, fails == 0 ? "bigtest: PASS (see [BIG64] lines on serial)\n"
                           : "bigtest: FAIL (see [BIG64] selftest FAIL mask= on serial)\n");
    panic64_watchdog_unpause64();
    return fails == 0;
}

// write FILE TEXT（整体覆盖；路径多级）。★ 批次 M：**命令行本身**只能敲进几百字节，
//   所以这条命令写的文件很小 —— 它受"命令行长度"限制，而**不是** 8 MiB 的文件上限；
//   想生成大文件请用 `bigtest`（内核自己按模式生成）。超过 8 MiB 的文件 fd64 会如实拒绝。
static bool cmd_write(TerminalState* ts, const char* name, const char* text) {
    if (!name || !name[0] || !text) {
        ts_puts(ts, "write: usage: write FILE TEXT\n");
        return false;
    }
    if (fd64_norm_path64(name, g_pathbuf, (int)sizeof(g_pathbuf)) != 0) {
        ts_puts(ts, "write: bad path (multi-level /dir/sub/name, <=128 B, <=16 segments)\n");
        return false;
    }
    // ★ 批次 K：只读卷（FAT32）上写类命令**明确报"只读卷"**（fd64 也会拒，这里是给用户看清楚）
    if (fs64_is_readonly64(-1)) {
        ts_puts(ts, "write: read-only volume (FAT32): writing is not implemented (see [FS64] reject on serial)\\n");
        return false;
    }
    const int len = st_len(text);
    if (len > (int)FD64_FILE_MAX) {                    // 只会被"命令行长度"触发，永远不会到这里
        ts_puts(ts, "write: too large (single file limit 8 MiB / 8388608 B)\\\\n");
        return false;
    }
    const int fd = fd64_open64(g_pathbuf, FD64_O_WRONLY | FD64_O_CREAT | FD64_O_TRUNC);
    if (fd < 3) { ts_puts(ts, "write: cannot open (no volume / bad path)\\\\n"); return false; }
    const int w = fd64_write64(fd, text, len);
    (void)fd64_close64(fd);
    if (w < 0) { ts_puts(ts, "write: failed (no space / over the 8 MiB single-file limit)\\n"); return false; }
    ts_puts(ts, "written ");
    ts_put_u64(ts, (uint64_t)w);
    ts_puts(ts, " bytes to ");
    ts_puts(ts, g_pathbuf);
    ts_putc(ts, (uint32_t)'\n');
    return true;
}

// touch FILE：不存在就建空文件（真落盘）
static bool cmd_touch(TerminalState* ts, const char* name) {
    if (!name || !name[0]) { ts_puts(ts, "touch: usage: touch FILE\n"); return false; }
    if (fs64_is_readonly64(-1)) {           // ★ 批次 K：只读卷（FAT32）上不能建文件
        ts_puts(ts, "touch: read-only volume (FAT32)\n");
        return false;
    }
    if (fd64_norm_path64(name, g_pathbuf, (int)sizeof(g_pathbuf)) != 0) {
        ts_puts(ts, "touch: bad path (multi-level /dir/sub/name, <=128 B, <=16 segments)\n");
        return false;
    }
    Fs64Stat64 st;
    const bool exists = (fs64_stat64(-1, g_pathbuf, &st) == 0);
    const int fd = fd64_open64(g_pathbuf, FD64_O_WRONLY | FD64_O_CREAT);
    if (fd < 3) { ts_puts(ts, "touch: failed (no volume / bad path)\n"); return false; }
    (void)fd64_close64(fd);
    ts_puts(ts, exists ? "ok, exists " : "ok, created ");
    ts_puts(ts, g_pathbuf);
    ts_putc(ts, (uint32_t)'\n');
    return true;
}

// rm / del FILE：真删（fs64_unlink；目录会被拒绝并如实说明；FAT 卷明确报"只读卷"）
static bool cmd_rm(TerminalState* ts, const char* name) {
    if (!name || !name[0]) { ts_puts(ts, "rm: usage: rm FILE\n"); return false; }
    if (fs64_is_readonly64(-1)) {           // ★ 批次 K
        ts_puts(ts, "rm: read-only volume (FAT32): deleting is not implemented\n");
        return false;
    }
    if (fd64_norm_path64(name, g_pathbuf, (int)sizeof(g_pathbuf)) != 0) {
        ts_puts(ts, "rm: bad path (multi-level /dir/sub/name, <=128 B, <=16 segments)\n");
        return false;
    }
    Fs64Stat64 st;
    if (fs64_stat64(-1, g_pathbuf, &st) != 0) {
        ts_puts(ts, "rm: no such file: ");
        ts_puts(ts, g_pathbuf);
        ts_putc(ts, (uint32_t)'\n');
        return false;
    }
    if (st.type == VFS64_TYPE_DIR) {
        ts_puts(ts, "rm: ");
        ts_puts(ts, g_pathbuf);
        ts_puts(ts, " is a directory (deleting directories is not supported here)\n");
        return false;
    }
    if (fs64_unlink64(-1, g_pathbuf) != 0) {
        ts_puts(ts, "rm: failed (see serial log for the reason)\n");
        return false;
    }
    ts_puts(ts, "removed ");
    ts_puts(ts, g_pathbuf);
    ts_putc(ts, (uint32_t)'\n');
    return true;
}

// mkdir DIR：fs64_mkdir（多级，父目录必须存在；FAT 卷明确报"只读卷"）
static bool cmd_mkdir(TerminalState* ts, const char* name) {
    if (!name || !name[0]) { ts_puts(ts, "mkdir: usage: mkdir DIR\n"); return false; }
    if (fs64_is_readonly64(-1)) {           // ★ 批次 K
        ts_puts(ts, "mkdir: read-only volume (FAT32): creating folders is not implemented\n");
        return false;
    }
    if (fd64_norm_path64(name, g_pathbuf, (int)sizeof(g_pathbuf)) != 0) {
        ts_puts(ts, "mkdir: bad path (multi-level /dir/sub/name, parent must exist)\n");
        return false;
    }
    Fs64Stat64 st;
    if (fs64_stat64(-1, g_pathbuf, &st) == 0) {
        if (st.type == VFS64_TYPE_DIR) { ts_puts(ts, "mkdir: exists "); ts_puts(ts, g_pathbuf); ts_putc(ts, (uint32_t)'\n'); return true; }
        ts_puts(ts, "mkdir: path exists and is a file\n");
        return false;
    }
    if (fs64_mkdir64(-1, g_pathbuf) != 0) { ts_puts(ts, "mkdir: failed (see serial log)\n"); return false; }
    ts_puts(ts, "created directory ");
    ts_puts(ts, g_pathbuf);
    ts_putc(ts, (uint32_t)'\n');
    return true;
}

// df：卷总块/空闲块（sysstate64 的只读探测快照）+ 实时文件统计（vfs64_ls）
// ★ 多卷：df 的"所有卷"清单。**独立于系统卷快照是否可用** —— 128MB 级系统卷的快照受
//   sysstate64 旧上限限制（bm_use<=8 = 16MB 封顶），但盘符/卷槽/实时容量来自 drive64 的
//   已挂载槽（vfs64_slot_info64 现数位图），所以在任何卷规模下都能给出真值。
//   打点：[TERM] cmd df volumes=<n> current=<C:|->（fs_term_test 依赖的旧行也照旧打）
static void df_print_volumes(TerminalState* ts) {
    if (drive64_count64() == 0) (void)drive64_scan64();
    const int dn = drive64_count64();
    const char cur = drive64_current_letter64();
    int shown = 0;
    ts_puts(ts, "  volumes (* = current volume used by ls/cat/write/mkdir/rm/run):\n");
    for (int i = 0; i < dn; i++) {
        DriveInfo64 d;
        if (drive64_info64(i, &d) != 0 || !d.present) continue;
        if (!d.browsable || !d.letter) continue;
        ts_puts(ts, (cur == d.letter) ? "   * " : "     ");
        char l[3]; l[0] = d.letter; l[1] = ':'; l[2] = 0;
        ts_puts_pad(ts, l, 4);
        ts_puts(ts, " slot=");
        ts_put_u64(ts, (uint64_t)d.slot);
        ts_puts(ts, " fs=");
        ts_puts(ts, d.fs);
        ts_puts(ts, " total_kb=");
        ts_put_u64(ts, d.total_kb);
        ts_puts(ts, " free_kb=");
        ts_put_u64(ts, d.free_kb);
        ts_puts(ts, d.system ? " system\n" : "\n");
        shown++;
    }
    ts_puts(ts, "  browsable volumes=");
    ts_put_u64(ts, (uint64_t)shown);
    ts_puts(ts, " vfs64 slots=");
    ts_put_u64(ts, (uint64_t)VFS64_SLOT_MAX);
    ts_putc(ts, (uint32_t)'\n');
    dbg64_line_begin64();
    dbg64_str("[TERM] cmd df volumes=");
    dbg64_dec((uint64_t)shown);
    dbg64_str(" current=");
    if (cur) { char cb[3]; cb[0] = cur; cb[1] = ':'; cb[2] = 0; dbg64_str(cb); } else dbg64_str("-");
    dbg64_nl();
    dbg64_line_end64();
}

static bool cmd_df(TerminalState* ts) {
    // 注意打点顺序：**先**打旧行 "[TERM] cmd df blocks=..."（fs_term/fs_tree 的 wait_for 就等它），
    // 卷清单（df_print_volumes）放在后面/或无卷分支里 —— 顺序反了会让旧脚本在"等 df 行"时提前返回。
    Fs64Info fs;
    const int rc = sysstate64_fsinfo64(&fs);
    char names[FD64_MAX][FD64_NAME_MAX];
    uint32_t sizes[FD64_MAX];
    const int n = vfs64_ls("/", names, (int)FD64_MAX, sizes);
    uint64_t live_files = (n > 0) ? (uint64_t)n : 0;
    uint64_t live_bytes = 0;
    for (int i = 0; i < n; i++) live_bytes += sizes[i];
    if (rc != 0 || !fs.ok) {
        ts_puts(ts, "df: no VimtuFS2 volume (no partition table / mount failed)\n");
        df_print_volumes(ts);                   // ★ 多卷：系统卷快照读不到也要列出盘符/卷槽
        return true;
    }
    const uint64_t used = (uint64_t)fs.total_blocks - (uint64_t)fs.free_blocks;
    ts_puts(ts, "Filesystem   1K-blocks      Used Available Use%  Files\n");
    ts_puts(ts, "VimtuFS2      ");
    ts_put_u64(ts, (uint64_t)fs.total_blocks / 2);          // 1K 块 = 2 个 512B 块
    ts_puts(ts, "  ");
    ts_put_u64(ts, used / 2);
    ts_puts(ts, "  ");
    ts_put_u64(ts, (uint64_t)fs.free_blocks / 2);
    ts_puts(ts, "  ");
    ts_put_u64(ts, fs.total_blocks ? (used * 100u / (uint64_t)fs.total_blocks) : 0);
    ts_puts(ts, "%  ");
    ts_put_u64(ts, live_files);
    ts_putc(ts, (uint32_t)'\n');
    ts_puts(ts, "  blocks: total=");
    ts_put_u64(ts, (uint64_t)fs.total_blocks);
    ts_puts(ts, " (512B) free=");
    ts_put_u64(ts, (uint64_t)fs.free_blocks);
    ts_puts(ts, " used=");
    ts_put_u64(ts, used);
    ts_puts(ts, "   files=");
    ts_put_u64(ts, live_files);
    ts_puts(ts, " bytes=");
    ts_put_u64(ts, live_bytes);
    ts_putc(ts, (uint32_t)'\n');
    ts_puts(ts, "  (system volume: bitmap snapshot from the boot probe; file list/bytes are live; VimtuFS2 v3 tree)\n");

    dbg64_line_begin64();
    dbg64_str("[TERM] cmd df blocks=");
    dbg64_dec((uint64_t)fs.total_blocks);
    dbg64_str(" free=");
    dbg64_dec((uint64_t)fs.free_blocks);
    dbg64_str(" files=");
    dbg64_dec(live_files);
    dbg64_nl();
    dbg64_line_end64();
    df_print_volumes(ts);                       // ★ 多卷：最后再列所有卷（含实时容量与当前卷标记）
    return true;
}

// vol：多卷命令。
//   vol          -> 列出所有盘符/卷槽（打点 [VOL] list n=<n> current=<C:|->）
//   vol C:|D:    -> 切换"当前卷"（drive64_activate_letter64；打点 [VOL] switch letter=D: slot=1 ok）
// 说明：ls/cat/write/touch/rm/mkdir/run 等文件命令都以**当前卷**为根，所以这个命令就是"选盘"。
static bool cmd_vol(TerminalState* ts, const char* arg) {
    if (arg && arg[0]) {
        char letter = arg[0];
        if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');
        if (letter < 'A' || letter > 'Z' || (arg[1] != 0 && arg[1] != ':')) {
            ts_puts(ts, "vol: usage: vol | vol C: | vol D:\n");
            dbg64_line_begin64();
            dbg64_str("[VOL] usage bad-arg");
            dbg64_nl();
            dbg64_line_end64();
            return false;
        }
        const int di = drive64_by_letter64(letter);
        DriveInfo64 d;
        int slot = -1;
        bool ro = false;
        if (di >= 0 && drive64_info64(di, &d) == 0) {
            ro = d.readonly ? true : false;
            slot = (d.fskind == DRV64_FS_FAT32) ? -1 : (int)d.slot;   // FAT 卷不占 vfs64 槽（打点写 '-'）
        }
        const int rc = drive64_activate_letter64(letter);
        char lb[3]; lb[0] = letter; lb[1] = ':'; lb[2] = 0;
        if (rc == 0) {
            ts_puts(ts, "vol: current volume = ");
            ts_puts(ts, lb);
            ts_puts(ts, "  slot=");
            ts_put_u64(ts, (uint64_t)(slot < 0 ? 0 : slot));
            if (ro) ts_puts(ts, "  ro=1 (FAT32 read-only)");
            ts_puts(ts, "  (ls/cat/write/mkdir/rm/run now act on this volume)\n");
        } else {
            ts_puts(ts, "vol: cannot switch to ");
            ts_puts(ts, lb);
            ts_puts(ts, " (no such letter / not browsable / no slot; see the list below)\n");
        }
        dbg64_line_begin64();
        dbg64_str(rc == 0 ? "[VOL] switch letter=" : "[VOL] switch FAILED letter=");
        dbg64_str(lb);
        dbg64_str(" slot=");
        if (slot >= 0) dbg64_dec((uint64_t)slot); else dbg64_str("-");
        dbg64_nl();
        dbg64_line_end64();
        return rc == 0;
    }

    // 列表：盘符 / 卷槽 / 容量 / 是否当前
    if (drive64_count64() == 0) (void)drive64_scan64();
    const int dn = drive64_count64();
    const char cur = drive64_current_letter64();
    int n = 0;
    ts_puts(ts, "volumes (unified volume table -> drive letters; ro=1 = read-only FAT32):\n");
    for (int i = 0; i < dn; i++) {
        DriveInfo64 d;
        if (drive64_info64(i, &d) != 0 || !d.present) continue;
        ts_puts(ts, "  ");
        if (d.letter) {
            char l[3]; l[0] = d.letter; l[1] = ':'; l[2] = 0;
            ts_puts(ts, (cur == d.letter) ? "* " : "  ");
            ts_puts_pad(ts, l, 4);
            ts_puts(ts, "slot=");
            ts_put_u64(ts, (d.fskind == DRV64_FS_FAT32) ? (uint64_t)d.vol : (uint64_t)d.slot);
            ts_puts(ts, " fs=");
            ts_puts(ts, d.fs);
            ts_puts(ts, " total_kb=");
            ts_put_u64(ts, d.total_kb);
            ts_puts(ts, " free_kb=");
            ts_put_u64(ts, d.free_kb);
            if (d.readonly) ts_puts(ts, " ro");
            ts_puts(ts, d.system ? " system" : "");
            ts_puts(ts, " disk=");
            ts_put_u64(ts, (uint64_t)d.disk);
            ts_puts(ts, " lba=");
            ts_put_u64(ts, d.start_lba);
            ts_putc(ts, (uint32_t)'\n');
            dbg64_line_begin64();                       // ★ 批次 K：串口证据行（含 ro）
            dbg64_str("[VOL] vol letter=");
            char lb[3]; lb[0] = d.letter; lb[1] = ':'; lb[2] = 0;
            dbg64_str(lb);
            dbg64_str(" fs=");
            dbg64_str(d.fs);
            dbg64_str(" ro=");
            dbg64_dec(d.readonly ? 1 : 0);
            dbg64_str(" total_kb=");
            dbg64_dec(d.total_kb);
            dbg64_str(" free_kb=");
            dbg64_dec(d.free_kb);
            dbg64_nl();
            dbg64_line_end64();
            n++;
        } else {
            ts_puts(ts, "  --  (no letter) fs=");
            ts_puts(ts, d.fs);
            ts_puts(ts, " skip=");
            ts_puts(ts, drive64_skip_reason64(d.skip));
            ts_putc(ts, (uint32_t)'\n');
        }
    }
    ts_puts(ts, "  current=");
    if (cur) { char l[3]; l[0] = cur; l[1] = ':'; l[2] = 0; ts_puts(ts, l); } else ts_puts(ts, "(none)");
    ts_puts(ts, "  browsable=");
    ts_put_u64(ts, (uint64_t)n);
    ts_puts(ts, "  vfs64 slots=");
    ts_put_u64(ts, (uint64_t)VFS64_SLOT_MAX);
    ts_putc(ts, (uint32_t)'\n');
    dbg64_line_begin64();
    dbg64_str("[VOL] list n=");
    dbg64_dec((uint64_t)n);
    dbg64_str(" current=");
    if (cur) { char l[3]; l[0] = cur; l[1] = ':'; l[2] = 0; dbg64_str(l); } else dbg64_str("-");
    dbg64_str(" slots=");
    dbg64_dec((uint64_t)VFS64_SLOT_MAX);
    dbg64_nl();
    dbg64_line_end64();
    vfs64_slots_dump64();                                   // 卷槽表（串口，验收用）
    return true;
}

// echo TEXT | echo TEXT > FILE（重定向走 FD 层，真落盘）
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
    char path[FD64_PATH_MAX];
    grab_token(fp, path, (int)sizeof(path));
    if (!path[0]) { ts_puts(ts, "echo: usage: echo TEXT > FILE\n"); return false; }
    return cmd_write(ts, path, text);
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
// 这些命令原来都是"尚未支持"的桩；现在走各子系统的真 API，输出全是实测值。

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
            // 空格写法：rest 是"第一个参数之后的整段原文"（以 KEY 开头）—— 值必须取
            // KEY 之后的那一段（保留 VALUE 里的空格），绝不能把 KEY 一起塞进值里。
            // （旧实现直接把 rest 当值，`cfg set ui.theme 1` 会变成 key=ui.theme value="ui.theme 1"。）
            const char* p = store_val_after_key(rest, key);
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
// ---------- dmesg：开机滚屏引导控制台的启动日志（kernel/console64.cpp）----------
// 数据源：16 KiB 环形缓冲（最近的 N 行）+ 头部保留区（最早的 16 行，环形覆盖不到它们）。
//   顺序 = 头部保留行 -> 省略标记（dropped>0 时）-> 环形缓冲内容（最旧 -> 最新）。
// 屏幕：全部行打进终端（可滚动回看）；串口：有界证据（前 first 行 + 后 last 行，带 [CON64] dmesg[i] 前缀，
//   自动验收就 grep 这个；全量只在屏幕上，避免 250+ 行刷串口拖慢桌面）。
static void cmd_dmesg(TerminalState* ts) {
    const int n = con64_dmesg_count64();
    ts_puts(ts, "dmesg: lines=");
    ts_put_u64(ts, (uint64_t)n);
    ts_puts(ts, " (ring ");
    ts_put_u64(ts, (uint64_t)(con64_ring_bytes64() / 1024));
    ts_puts(ts, " KiB, text_max=");
    ts_put_u64(ts, (uint64_t)con64_text_max64());
    ts_puts(ts, ", buffered=");
    ts_put_u64(ts, (uint64_t)con64_buffered_lines64());
    ts_puts(ts, ", head_keep=");
    ts_put_u64(ts, (uint64_t)con64_head_lines64());
    ts_puts(ts, ", dropped=");
    ts_put_u64(ts, (uint64_t)con64_dropped_lines64());
    ts_puts(ts, ", trunc=");
    ts_put_u64(ts, (uint64_t)con64_trunc_lines64());
    ts_puts(ts, ")\n");
    for (int i = 0; i < n; i++) {
        char lb[CON64_TEXT_MAX + 64];
        if (con64_dmesg_text64(i, lb, (int)sizeof lb) <= 0) continue;
        ts_puts(ts, lb);
        ts_putc(ts, (uint32_t)'\n');
    }
    con64_dmesg_dump_serial64(16, 16);      // 串口证据（有界）
}

// ---------- boot：进桌面之前的滚屏引导控制台开关（持久化到 config64/store64）----------
// 支持：boot | boot verbose | boot verbose on|off
static bool cmd_boot(TerminalState* ts, const char* sub, const char* val) {
    if (!sub || !sub[0]) {
        ts_puts(ts, "boot: verbose=");
        ts_puts(ts, config64_get_bool64("boot.verbose", 1) ? "on" : "off");
        ts_puts(ts, "  (boot.verbose; show/skip the scrolling boot console before the desktop)\n");
        ts_puts(ts, "  usage: boot verbose on|off\n");
        return true;
    }
    if (!st_eq(sub, "verbose")) {
        ts_puts(ts, "boot: usage: boot [verbose [on|off]]\n");
        return false;
    }
    if (!val || !val[0]) {
        ts_puts(ts, "boot verbose: ");
        ts_puts(ts, config64_get_bool64("boot.verbose", 1) ? "on" : "off");
        ts_puts(ts, "\n");
        return true;
    }
    int want = -1;
    if (st_eq(val, "on") || st_eq(val, "1") || st_eq(val, "true")) want = 1;
    else if (st_eq(val, "off") || st_eq(val, "0") || st_eq(val, "false")) want = 0;
    if (want < 0) {
        ts_puts(ts, "boot verbose: usage: boot verbose on|off\n");
        return false;
    }
    const int rc_set = config64_set_bool64("boot.verbose", want);
    const int rc_flush = (rc_set == 0) ? config64_flush64() : -1;

    ts_puts(ts, "boot verbose = ");
    ts_puts(ts, want ? "on" : "off");
    ts_puts(ts, " -> config64 boot.verbose (persisted via store64; takes effect at next boot), flush rc=");
    ts_put_u64(ts, (uint64_t)(rc_flush < 0 ? 0 : rc_flush));
    ts_putc(ts, (uint32_t)'\n');
    dbg64_line_begin64();
    dbg64_str("[CON64] boot verbose=");
    dbg64_str(want ? "on" : "off");
    dbg64_str(" persisted=");
    dbg64_dec((uint64_t)(rc_flush == 0 ? 1 : 0));
    dbg64_str(" set_rc=");
    dbg64_dec((uint64_t)(rc_set < 0 ? 0 : rc_set));
    dbg64_str(" flush_rc=");
    dbg64_dec((uint64_t)(rc_flush < 0 ? 0 : rc_flush));
    dbg64_nl();
    dbg64_line_end64();
    return (rc_set == 0) && (rc_flush == 0);
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

// ==================== 批次 A 后半：最后两条桩接真（preload / update）+ proc64 真进程 ====================

// ---------- preload：字形/图标预热统计（真值来自 kernel/preload64.cpp；启动期已跑一轮） ----------
static bool cmd_preload(TerminalState* ts, const char* sub) {
    if (sub && sub[0] && st_eq(sub, "run")) {
        const int n = preload64_run64();               // 再跑一轮：第二轮 glyphs_new=0 = 缓存幂等
        ts_puts(ts, "preload run: new_glyphs=");
        ts_put_u64(ts, (uint64_t)(n < 0 ? -n : n));
        ts_putc(ts, (uint32_t)'\n');
        return true;
    }
    char rep[256];
    preload64_report64(rep, (int)sizeof(rep));
    ts_puts(ts, "preload (glyph prewarm + icon pre-scale; measured with rdtsc64):\n  ");
    ts_puts(ts, rep);
    ts_puts(ts, "\n  first_paint_* = the same real draw (sample text + one desktop icon) before/after prewarm\n");
    dbg64_line_begin64();
    dbg64_str("[PRELOAD64] cmd report ");
    dbg64_str(rep);
    dbg64_nl();
    dbg64_line_end64();
    return true;
}

// ---------- update：标记文件 -> 应用 -> store/ring log/重启（边界见 kernel/update64.h） ----------
static bool cmd_update(TerminalState* ts, const char* sub, const char* arg1) {
    if (!sub || !sub[0] || st_eq(sub, "status")) {
        char rep[256];
        update64_report64(rep, (int)sizeof(rep));
        ts_puts(ts, "update status: ");
        ts_puts(ts, rep);
        ts_putc(ts, (uint32_t)'\n');
        ts_puts(ts, gui64_tr(
            "  boundary: marker->apply->restart loop only; NOT a real upgrade package (no kernel replacement/diff/signature)\n",
            "  边界：只是\"标记 -> 应用 -> 重启\"的机制闭环，不是真正的升级包（没有内核替换/差分/签名）\n"));
        dbg64_line_begin64();
        dbg64_str("[UPDATE64] cmd status ");
        dbg64_str(rep);
        dbg64_nl();
        dbg64_line_end64();
        return true;
    }

    if (st_eq(sub, "pending")) {
        if (!arg1 || !arg1[0]) {
            ts_puts(ts, "update pending: usage: update pending <ver>   (e.g. update pending 0.2.0)\n");
            return false;
        }
        const int rc = update64_write_pending64(arg1);
        if (rc != 0) {
            ts_puts(ts, "update pending: failed (bad version string or no writable volume; see serial log)\n");
            return false;
        }
        ts_puts(ts, "[UPDATE64] cmd pending ver=");
        ts_puts(ts, arg1);
        ts_puts(ts, " path=");
        ts_puts(ts, update64_pending_path64());
        ts_puts(ts, " (applies at next boot / on 'update apply')\n");
        return true;
    }

    if (st_eq(sub, "apply")) {
        char pend[96];
        if (!update64_pending_exists64(pend, (int)sizeof(pend))) {
            ts_puts(ts, "update apply: no pending marker (/update.pending absent)\n");
            return true;
        }
        ts_puts(ts, "update apply: applying ");
        ts_puts(ts, update64_pending_path64());
        ts_puts(ts, " ...\n");
        if (update64_apply64() != 1) {
            ts_puts(ts, "update apply: pending marker invalid (kept; see serial log)\n");
            return false;
        }
        ts_puts(ts, "update apply: applied (store + /update.done), restarting to finish ...\n");
        dbg64_line_begin64();
        dbg64_str("[UPDATE64] cmd apply -> soft restart\n");
        dbg64_line_end64();
        gui64_flip_window(ts->win);
        sysstate64_soft_restart64();
    }

    ts_puts(ts, "update: unknown subcommand: ");
    ts_puts(ts, sub);
    ts_puts(ts, "\n  try: update status | update pending <ver> | update apply\n");
    return false;
}

// ---------- proc：proc64 真进程（list / run / kill）；`proc run spin` 用内嵌的 spin64.elf ----------
// 为什么终端需要这条命令：启动期多进程演示跑完进程就走了，桌面起来时进程表为空；任务管理器
//   进程页要显示"真进程"、且要能 kill，需要一个运行期可创建的**长命**进程（spin64.elf 永不退出）。
extern "C" const uint8_t _binary_build64_spin64_elf_start[];
extern "C" const uint8_t _binary_build64_spin64_elf_end[];
extern "C" const uint8_t _binary_build64_filedemo64_elf_start[];
extern "C" const uint8_t _binary_build64_filedemo64_elf_end[];

// 内嵌程序 -> VimtuFS2（幂等：已装过就跳过）。两个：长命 spin（proc run）与
// filedemo（ring3 读文件演示：open "/t.txt" + read + write；见 user/filedemo64.asm）。
static int term_install_blob64(const char* path, const uint8_t* start, const uint8_t* end, const char* tag) {
    uint32_t t = 0, sz = 0;
    if (vfs64_stat(path, &t, &sz) == 0) return 0;              // 幂等：已装过就跳过
    const int len = (int)(end - start);
    if (len <= 0) return -1;
    const int rc = vfs64_write(path, start, len);
    if (rc < 0) return -1;
    dbg64_line_begin64();
    dbg64_str("[TERM] install ");
    dbg64_str(path);
    dbg64_str(" bytes=");
    dbg64_dec((uint64_t)len);
    dbg64_str(" (");
    dbg64_str(tag);
    dbg64_str(")\n");
    dbg64_line_end64();
    return 0;
}
static int term_install_spin64() {
    return term_install_blob64("/spin.elf", _binary_build64_spin64_elf_start,
                               _binary_build64_spin64_elf_end, "long-lived proc target");
}
static int term_install_filedemo64() {
    return term_install_blob64("/filedemo.elf", _binary_build64_filedemo64_elf_start,
                               _binary_build64_filedemo64_elf_end, "ring3 file demo");
}
// 开终端时幂等安装内嵌程序（`run filedemo` / `proc run spin` 因此总是可用；没有卷时静默失败）
static void term_install_builtins64() {
    (void)term_install_spin64();
    (void)term_install_filedemo64();
}

// pid 当前的状态（-1 = 进程表里没有/已收尸；否则 Proc64State 值）
static int term_proc_state64(int pid) {
    for (int i = 0; i < PROC64_MAX; i++) {
        Proc64Info in;
        if (proc64_info64(i, &in) == 0) continue;
        if ((int)in.pid == pid) return (int)in.state;
    }
    return -1;
}

// pid 对应的任务 id（0 = 没有这个进程/没有任务）——"任务是否活过第一次调度"靠它查任务表
static uint32_t term_proc_task_id64(int pid) {
    for (int i = 0; i < PROC64_MAX; i++) {
        Proc64Info in;
        if (proc64_info64(i, &in) == 0) continue;
        if ((int)in.pid == pid) return in.task_id;
    }
    return 0;
}

// 任务 id 当前的状态（-1 = 任务表里找不到 = 已被回收）；值 = Task64State
static int term_task_state64(uint32_t task_id) {
    if (task_id == 0) return -1;
    for (int i = 0; i < TASK64_MAX; i++) {
        Task64Info in;
        if (task_info64(i, &in) == 0) continue;
        if (in.id == task_id) return (int)in.state;
    }
    return -1;
}


static const char* term_proc_state_text(uint32_t st) {
    switch (st) {
        case PROC64_READY:   return "ready";
        case PROC64_RUNNING: return "running";
        case PROC64_SLEEP:   return "sleep";
        case PROC64_EXITED:  return "exited";
        default:             return "unknown";
    }
}

static bool cmd_proc(TerminalState* ts, const char* sub, const char* arg1, const char* arg2) {
    if (!sub || !sub[0] || st_eq(sub, "list") || st_eq(sub, "ps")) {
        ts_puts(ts, gui64_tr("proc64 process table (per-process CR3):\n",
                             "proc64 进程表（每进程独立 CR3）：\n"));
        ts_puts(ts, "  pid  ppid state     tasks  cr3               name\n");
        int rows = 0;
        for (int i = 0; i < PROC64_MAX; i++) {
            Proc64Info in;
            if (proc64_info64(i, &in) == 0) continue;
            ts_puts(ts, "  ");
            ts_put_u64_right(ts, (uint64_t)in.pid, 3);
            ts_puts(ts, "  ");
            ts_put_u64_right(ts, (uint64_t)in.ppid, 3);
            ts_puts(ts, "  ");
            ts_puts_pad(ts, term_proc_state_text(in.state), 8);
            ts_put_u64_right(ts, (uint64_t)task64_proc_threads64(in.task_id), 5);
            ts_puts(ts, "  ");
            ts_put_hex(ts, in.cr3, 16);
            ts_puts(ts, "  ");
            ts_puts(ts, in.name);
            ts_puts(ts, "\n");
            rows++;
        }
        if (rows == 0) ts_puts(ts, gui64_tr("  no process data (use 'proc run spin')\n",
                                            "  无进程数据（可敲 proc run spin）\n"));
        dbg64_line_begin64();
        dbg64_str("[PROC64] cmd list rows=");
        dbg64_dec((uint64_t)rows);
        dbg64_str(" total=");
        dbg64_dec((uint64_t)proc64_count64());
        dbg64_nl();
        dbg64_line_end64();
        return true;
    }

    if (st_eq(sub, "run")) {
        if (!arg1 || !arg1[0]) {
            ts_puts(ts, "proc run: usage: proc run <name|/path>   (built-in: spin -> /spin.elf)\n");
            return false;
        }
        char path[PROC64_PATH_MAX];
        int pl = 0;
        const char* nm = arg1;
        if (st_eq(arg1, "spin") || st_eq(arg1, "/spin.elf")) {
            if (term_install_spin64() != 0) {
                ts_puts(ts, "proc run: cannot install /spin.elf (no VimtuFS2 volume?)\n");
                return false;
            }
            nm = "spin";
            const char* p = "/spin.elf";
            for (int i = 0; p[i] && pl < (int)sizeof(path) - 1; i++) path[pl++] = p[i];
        } else {
            if (arg1[0] != '/') path[pl++] = '/';
            for (int i = 0; arg1[i] && pl < (int)sizeof(path) - 1; i++) path[pl++] = arg1[i];
        }
        path[pl] = 0;
        char name[PROC64_NAME_MAX];
        int nn = 0;
        for (int i = 0; nm[i] && nm[i] != '/' && nn < PROC64_NAME_MAX - 1; i++) name[nn++] = nm[i];
        name[nn] = 0;
        if (nn == 0) { name[0] = 'p'; name[1] = 0; }

        // 创建 + 启动。批次 B 起**没有**"脏槽位重试"了：in_ring3 位由 task64 的
        //   kill/reap/exit/force-remove 路径清（见 usermode64.h 的 user64_slot_release64），
        //   所以复用一个被 kill 的 ring3 进程的任务槽也能一次进 ring3（kernel64.cpp 里有
        //   启动期回归自检：4 轮 kill+run 同一槽全过）。
        // 成功判据（对内置长命程序 spin）= **任务状态**已越过"第一次被调度"：RUNNING/SLEEP
        //   说明它已经进了 ring3。只看进程 state 不行（nanosleep 不把进程 state 置成 SLEEP），
        //   只看"没死"也不行（可能还是 READY，下一秒才失败）。等待期间暂停 GUI 看门狗
        //   （最多 ~400ms），否则 task 0 被占住会触发 watchdog fire（实测踩过）。
        const bool expect_alive = st_eq(arg1, "spin") || st_eq(arg1, "/spin.elf");
        if (expect_alive) panic64_watchdog_pause64();
        const int pid = proc64_create64(name, 0);
        if (pid < 0) {
            if (expect_alive) panic64_watchdog_unpause64();
            ts_puts(ts, gui64_tr("proc run: create failed (shared address space mode / no slot / no pages)\n",
                                 "proc run: 建进程失败（共享地址空间模式 / 槽满 / 页不足）\n"));
            return false;
        }
        const int rc = proc64_start_elf64(pid, path);
        if (rc != 0) {
            proc64_destroy64(pid);
            if (expect_alive) panic64_watchdog_unpause64();
            ts_puts(ts, "proc run: start failed (see serial log)\n");
            return false;
        }
        if (expect_alive) {
            const uint32_t tid = term_proc_task_id64(pid);
            int reached = 0;
            for (int k = 0; k < 40; k++) {            // 最多 40 × 10ms = 400ms
                task_sleep64(10);
                const int pst = term_proc_state64(pid);
                const int tst = term_task_state64(tid);
                if (pst < 0 || pst == PROC64_EXITED) break;                 // 进程已退出：失败
                if (tst < 0 || tst == TASK64_DEAD) break;                   // 任务已死：入口失败
                if (tst == TASK64_RUNNING || tst == TASK64_SLEEP) {
                    // 越过第一次调度后再确认一次（排除"刚好采样在失败入口的微秒窗口里"）
                    task_sleep64(5);
                    const int pst2 = term_proc_state64(pid);
                    const int tst2 = term_task_state64(tid);
                    if (pst2 == PROC64_EXITED || tst2 < 0 || tst2 == TASK64_DEAD) break;
                    reached = 1;
                    break;
                }
                // READY：还没被调度到，继续等
            }
            panic64_watchdog_unpause64();
            if (!reached) {
                dbg64_line_begin64();
                dbg64_str("[TERM] proc run spin died early pid=");
                dbg64_dec((uint64_t)pid);
                dbg64_str(" tid=");
                dbg64_dec((uint64_t)tid);
                dbg64_str("\n");
                dbg64_line_end64();
                ts_puts(ts, "proc run: process died before entering ring3 (see serial log)\n");
                return false;
            }
        }
        if (expect_alive) panic64_watchdog_unpause64();
        if (pid < 0) {
            ts_puts(ts, "proc run: process keeps dying early (see [USER64] enter FAILED in serial log)\n");
            return false;
        }
        ts_puts(ts, "[TERM] proc run path=");
        ts_puts(ts, path);
        ts_puts(ts, " pid=");
        ts_put_u64(ts, (uint64_t)pid);
        ts_puts(ts, " (real proc64 process: own CR3 + own task)\n");
        dbg64_line_begin64();
        dbg64_str("[TERM] proc run path=");
        dbg64_str(path);
        dbg64_str(" pid=");
        dbg64_dec((uint64_t)pid);
        dbg64_str(" rc=0\n");
        dbg64_line_end64();
        return true;
    }

    if (st_eq(sub, "kill")) {
        const int pid = parse_dec(arg1 ? arg1 : "");
        int sig = parse_dec(arg2 ? arg2 : "");
        if (sig < 0) sig = 9;
        if (pid < 0) {
            ts_puts(ts, "proc kill: usage: proc kill <pid> [sig]   (pid from 'proc list')\n");
            return false;
        }
        const int64_t rc = proc64_kill64(pid, sig);
        ts_puts(ts, "[PROC64] cmd kill pid=");
        ts_put_u64(ts, (uint64_t)pid);
        ts_puts(ts, " sig=");
        ts_put_u64(ts, (uint64_t)sig);
        ts_puts(ts, " rc=");
        ts_put_i64(ts, rc);
        ts_putc(ts, (uint32_t)'\n');
        return rc == 0;
    }

    ts_puts(ts, "proc: unknown subcommand: ");
    ts_puts(ts, sub);
    ts_puts(ts, "\n  try: proc list | proc run <name|/path> | proc kill <pid> [sig]\n");
    return false;
}

static void cmd_about(TerminalState* ts) {
    ts_puts(ts,
        "VimtuOS 0.1.0 (VimtuOS 64-bit)\n"
        "  - 64-bit x86 kernel, long mode, hand-written GDT/IDT/PIC/PIT/RTC\n"
        "  - 4-level paging (4KB pages), kernel heap + physical page pool\n"
        "  - drivers: PS/2 keyboard + mouse, VBE LFB framebuffer, TrueType fonts\n"
        "  - desktop shell: windows, apps, taskbar, dirty-rect flips\n"
        "  - scheduler: kernel tasks (task64), used by ps/tasks/kill and the task manager\n"
        "  - terminal: character grid + built-in shell; files are REAL now (kernel/fd64.cpp -> VimtuFS2)\n"
        "  - real filesystem: VimtuFS2 (vfs64); /spin.elf + /filedemo.elf installed on terminal open;\n"
        "    settings persisted by store64 in /store.a|/store.b\n");
    ts_puts(ts, gui64_tr("  - sysstate64: state machine + module registry + health + 64-line ring log (syslog)\n"
                         "  - config64/session64: typed config + session policy, persisted to VimtuFS2 /store.a|b\n"
                         "  - panic64: blue screen (panic/bsod) + watchdog on the gui64 frame heartbeat\n"
                         "  - batch A2: task64 stats/critical/slice/force-remove+diag, preload64 (glyph+icon prewarm),\n"
                         "    update64 (marker->apply->restart loop; NOT a real upgrade package), proc64 process page in the task manager\\n"
                         "  - batch B: runtime display layer (boot mode list + measured 0x3DA refresh vs EDID preferred),\\n"
                         "    display/hz/edid command, hwinfo wired into task manager + settings, terminal files on the real\\n"
                         "    VimtuFS2 via kernel/fd64 (ring3 open/read/close too), GPU item in the perf page, in_ring3 slot fix\\n",
                         "  - sysstate64：状态机 + 模块注册表 + 健康报告 + 64 条 ring log（syslog）\n"
                         "  - config64/session64：类型化配置 + 会话策略，持久化在 VimtuFS2 的 /store.a|b\n"
                         "  - panic64：蓝屏（panic/bsod）+ 看门狗（心跳源 = gui64 帧）\n"
                         "  - 批次 A 后半：task64 统计/关键任务/时间片/强制移除+诊断、preload64（字形+图标预热）、\n"
                         "    update64（标记->应用->重启闭环；不是真正的升级包）、任务管理器进程页接 proc64 真进程\\n"
                         "  - 批次 B：运行期显示层（引导模式清单 + 0x3DA 实测刷新率与 EDID 首选时序对比）、display/hz/edid 命令、\\n"
                         "    hwinfo 接进任务管理器与设置页、终端文件命令走真 VimtuFS2（fd64；ring3 open/read/close 同源）、\\n"
                         "    性能页补\\\"显卡\\\"项、in_ring3 槽位 kill 后复用的缺陷根治\\n"));
}

// ==================== rust：Rust 模块（gui_rs）运行期接口 ====================
// 这是"Rust 侧供 C++ 调用"的第二个调用点（第一个是启动期自检，见 kernel64.cpp）：
//   主题表 / 设计 Token / 配色计算全部从 Rust 取（C++ 侧不各存一份常量）。
// 打点（自动验收 grep）：[TERM] rust cmd=<sub> rc=<0|1>
static void rust64_put_hex6(TerminalState* ts, uint32_t v) {
    static const char* H = "0123456789ABCDEF";
    char buf[7];
    for (int i = 5; i >= 0; i--) { buf[i] = H[v & 0xF]; v >>= 4; }
    buf[6] = 0;
    ts_puts(ts, buf);
}

static int32_t rust64_tok_id(const char* name) {
    uintptr_t n = 0;
    while (name[n]) n++;
    return rust64_token_lookup64((const uint8_t*)name, n);
}

// 串口打点（自动验收 grep）：[RUST64] term rust cmd=<sub> theme=<i> accent=#RRGGBB tokens=<n>
static void rust64_dbg_hex6(uint32_t v) {
    static const char* H = "0123456789ABCDEF";
    char buf[7];
    for (int i = 5; i >= 0; i--) { buf[i] = H[v & 0xF]; v >>= 4; }
    buf[6] = 0;
    dbg64_str(buf);
}

static void rust64_term_log64(const char* what, uint32_t theme) {
    dbg64_line_begin64();
    dbg64_str("[RUST64] term rust cmd=");
    dbg64_str(what);
    dbg64_str(" theme=");
    dbg64_dec((uint64_t)theme);
    dbg64_str(" accent=#");
    rust64_dbg_hex6(rust64_accent_rgb64(theme));
    dbg64_str(" tokens=");
    dbg64_dec((uint64_t)rust64_token_count64());
    dbg64_nl();
    dbg64_line_end64();
}

static void rust64_put_tok_px(TerminalState* ts, const char* name) {
    const int32_t id = rust64_tok_id(name);
    ts_puts(ts, " ");
    ts_puts(ts, name);
    ts_puts(ts, "=");
    if (id < 0) { ts_puts(ts, "?"); return; }
    const int32_t px = rust64_token_px64((uint32_t)id);
    const uint32_t pm = rust64_token_permille64((uint32_t)id);
    if (px >= 0) ts_put_i64(ts, px);
    else { ts_put_u64(ts, (uint64_t)pm); ts_puts(ts, "permille"); }
}

// rust | rust tokens | rust set <idx>
static bool cmd_rust(TerminalState* ts, const char* sub, const char* arg2) {
    const uint32_t n = rust64_theme_count64();
    uint8_t name[48];

    if (st_eq(sub, "set") || st_eq(sub, "theme")) {
        const int idx = parse_dec(arg2 ? arg2 : "");
        if (idx < 0 || (uint32_t)idx >= n) {
            ts_puts(ts, gui64_tr("rust set: usage: rust set <0..", "rust set: 用法: rust set <0.."));
            ts_put_u64(ts, (uint64_t)(n - 1));
            ts_puts(ts, gui64_tr(">   (rust lists them)\n", ">   （rust 会列出全部主题）\n"));
            return false;
        }
        if (rust64_theme_set64((uint32_t)idx) != 0) {
            ts_puts(ts, "rust set: rust64_theme_set64() failed\n");
            return false;
        }
        const uint32_t cur = rust64_theme_current64();
        ts_puts(ts, "[RUST64] theme set idx=");
        ts_put_u64(ts, (uint64_t)cur);
        ts_puts(ts, " accent=#");
        rust64_put_hex6(ts, rust64_accent_rgb64(cur));
        ts_puts(ts, gui64_tr(" (theme palette is read back from Rust on demand)\n",
                             "（配色是每次按需从 Rust 侧读回来的）\n"));
        rust64_term_log64("set", cur);   // 串口证据：[RUST64] term rust cmd=set ...
        return true;
    }

    if (st_eq(sub, "tokens")) {
        const uint32_t tn = rust64_token_count64();
        ts_puts(ts, "[RUST64] design tokens (gui_rs, count=");
        ts_put_u64(ts, (uint64_t)tn);
        ts_puts(ts, ")\n");
        for (uint32_t i = 0; i < tn; i++) {
            const uint32_t len = rust64_token_name64(i, name, sizeof(name));
            const int32_t px = rust64_token_px64(i);
            const uint32_t pm = rust64_token_permille64(i);
            ts_puts(ts, "  [");
            ts_put_u64(ts, (uint64_t)i);
            ts_puts(ts, "] ");
            for (uint32_t j = 0; j < len && j < (uint32_t)sizeof(name); j++) ts_putc(ts, (uint32_t)name[j]);
            ts_puts(ts, " = ");
            if (px >= 0) { ts_put_i64(ts, px); ts_puts(ts, " px/ms"); }
            else         { ts_put_u64(ts, (uint64_t)pm); ts_puts(ts, " permille"); }
            ts_putc(ts, '\n');
        }
        rust64_term_log64("tokens", rust64_theme_current64());
        return true;
    }

    if (sub[0] != 0) {
        ts_puts(ts, gui64_tr("rust: unknown subcommand; try: rust | rust tokens | rust set <idx>\n",
                             "rust: 未知子命令；试试: rust | rust tokens | rust set <idx>\n"));
        return false;
    }

    // 默认：当前主题（名字 + 配色 + 阴影）+ 全部主题的 accent + 关键 Token 抽样
    const uint32_t cur = rust64_theme_current64();
    ts_puts(ts, "[RUST64] gui_rs (no_std Rust) theme=");
    ts_put_u64(ts, (uint64_t)cur);
    ts_puts(ts, " name=");
    {
        const uint32_t len = rust64_theme_name64(cur, name, sizeof(name));
        for (uint32_t j = 0; j < len && j < (uint32_t)sizeof(name); j++) ts_putc(ts, (uint32_t)name[j]);
    }
    ts_puts(ts, " accent=#");
    rust64_put_hex6(ts, rust64_accent_rgb64(cur));
    ts_puts(ts, gui64_tr(" (all ", " （全部 "));
    ts_put_u64(ts, (uint64_t)n);
    ts_puts(ts, gui64_tr(" themes: ", " 个主题: "));
    rust64_term_log64("info", cur);

    for (uint32_t i = 0; i < n; i++) {
        const uint32_t len = rust64_theme_name64(i, name, sizeof(name));
        ts_puts(ts, "[");
        ts_put_u64(ts, (uint64_t)i);
        ts_puts(ts, "]");

        for (uint32_t j = 0; j < len && j < (uint32_t)sizeof(name); j++) ts_putc(ts, (uint32_t)name[j]);
        ts_puts(ts, "=#");
        rust64_put_hex6(ts, rust64_accent_rgb64(i));
        ts_puts(ts, " ");
    }
    ts_puts(ts, ")\n");
    Rust64ThemeColors c;
    if (rust64_theme_colors64(cur, &c) == 0) {
        ts_puts(ts, "  window_bg=");
        rust64_put_hex6(ts, ((uint32_t)c.window_bg.r << 16) | ((uint32_t)c.window_bg.g << 8) | c.window_bg.b);
        ts_puts(ts, " card=");
        rust64_put_hex6(ts, ((uint32_t)c.card_bg.r << 16) | ((uint32_t)c.card_bg.g << 8) | c.card_bg.b);
        ts_puts(ts, " text=");
        rust64_put_hex6(ts, ((uint32_t)c.text.r << 16) | ((uint32_t)c.text.g << 8) | c.text.b);
        ts_puts(ts, " dock=");
        rust64_put_hex6(ts, ((uint32_t)c.dock.r << 16) | ((uint32_t)c.dock.g << 8) | c.dock.b);
        ts_puts(ts, " dark=");
        ts_put_u64(ts, (uint64_t)c.is_dark);
        ts_putc(ts, '\n');
        Rust64Shadow sh;
        if (rust64_theme_shadow64(cur, 0, &sh) == 0) {
            ts_puts(ts, "  shadow near dy=");
            ts_put_i64(ts, sh.dy);
            ts_puts(ts, " blur=");
            ts_put_u64(ts, (uint64_t)sh.blur);
            ts_puts(ts, " alpha=");
            ts_put_u64(ts, (uint64_t)sh.color.a);
            if (rust64_theme_shadow64(cur, 1, &sh) == 0) {
                ts_puts(ts, "  far dy=");
                ts_put_i64(ts, sh.dy);
                ts_puts(ts, " blur=");
                ts_put_u64(ts, (uint64_t)sh.blur);
                ts_puts(ts, " alpha=");
                ts_put_u64(ts, (uint64_t)sh.color.a);
            }
            ts_putc(ts, '\n');
        }
    }
    ts_puts(ts, "  tokens:");
    rust64_put_tok_px(ts, "radius.window");
    rust64_put_tok_px(ts, "radius.button");
    rust64_put_tok_px(ts, "blur.background");
    rust64_put_tok_px(ts, "alpha.backdrop_material");
    rust64_put_tok_px(ts, "alpha.content_card");
    rust64_put_tok_px(ts, "motion.normal_ms");
    rust64_put_tok_px(ts, "ease.x1");
    ts_puts(ts, "   (rust tokens = all)\n");
    return true;
}

static void cmd_clear(TerminalState* ts) {
    ts_clear(ts);
    ts_dirty_client(ts);
    gui64_invalidate_window(ts->win);
}
// ==================== ★ 本批（P1c）：多用户 / 会话身份命令（locklogin64 + userdb64）====================
// 语义（**P4 之前不做权限拦截**，只把身份做成真实可查字段；见 kernel/userdb64.h 开头）：
//   useradd NAME              建用户（uid >= 1000；主目录 /home/NAME、桌面 /home/NAME/Desktop）
//   userdel NAME              删用户记录（不删主目录；不能删 root、也不能删最后一个普通用户）
//   passwd [NAME] [PW]        设/改口令（NAME 缺省 = 当前会话用户；PW 缺省 = 交互式**隐藏输入**；
//                             PW 为空串 = 清除口令）。只存盐 + SHA-256 迭代哈希，绝不落明文
//   users                     用户表（标出 root（登录界面里隐藏）与普通用户 + 当前 GUI / 会话身份）
//   whoami / id               会话身份：uid/gid/euid/egid 都是真值（su/sudo 改 euid/egid）
//   su [-] [名字]             切**会话身份**（默认 root；目标有口令且调用方非 root 时交互式输口令；
//                             GUI 的用户名/头像不变）—— ★ P4：切换后 VFS 真的按新身份拦
//   sudo -i                   同上（sudo 只实现了 -i）
//   exit                      退回 GUI 用户（本来就不是 root 会话时如实提示）
//   chmod/chown/umask/ls -l   ★ P4：改模式 / 改属主 / 掩码 / 长格式列目录（见各自 usage）
//   loginctl lock             回锁屏（重新登录后回到桌面）
static int  g_pw_pending = 0;                 // 1 = 下一行输入是口令（不回显）
static char g_pw_user[USERDB64_NAME_MAX];
// ★ P4：su 的口令交互（与 passwd 同一套"隐藏输入"机制：下一行不回显；空行/错口令 = 取消）
static int  g_su_pending = 0;
static char g_su_user[USERDB64_NAME_MAX];
static char g_su_via[16];
static void cmd_su_line(TerminalState* ts, const char* line);       // 定义在 cmd_su 之后
static void cmd_passwd_line(TerminalState* ts, const char* line) {
    // 空行 = **取消**（不改口令）：避免"注入的按键丢了、只剩一个回车"时把口令误清掉。
    if (!line || !line[0]) {
        ts_puts(ts, "passwd: cancelled (empty input; password unchanged)\\n");
        g_pw_pending = 0;
        g_pw_user[0] = 0;
        return;
    }
    const int idx = userdb64_find64(g_pw_user);
    const int rc = (idx >= 0) ? userdb64_set_password64(idx, line) : -1;
    if (rc == 0) {
        ts_puts(ts, "passwd: password updated (salt + SHA-256, 1000 iterations)\\n");
    } else {
        ts_puts(ts, "passwd: FAILED (user gone or disk write failed)\\n");
    }
    g_pw_pending = 0;
    g_pw_user[0] = 0;
}

static bool cmd_useradd(TerminalState* ts, const char* name) {
    if (!name || !name[0]) { ts_puts(ts, "useradd: usage: useradd NAME\n"); return false; }
    uint32_t uid = 0;
    if (userdb64_add64(name, &uid) != 0) {
        ts_puts(ts, "useradd: FAILED (exists / bad name / table full)\n");
        return false;
    }
    ts_puts(ts, "useradd: created ");
    ts_puts(ts, name);
    ts_puts(ts, " uid=");
    ts_put_u64(ts, (uint64_t)uid);
    ts_puts(ts, " home=/home/");
    ts_puts(ts, name);
    ts_puts(ts, " desktop=/home/");
    ts_puts(ts, name);
    ts_puts(ts, "/Desktop  password=none (use 'passwd ");
    ts_puts(ts, name);
    ts_puts(ts, " PW' to set one)\n");
    return true;
}

static bool cmd_userdel(TerminalState* ts, const char* name) {
    if (!name || !name[0]) { ts_puts(ts, "userdel: usage: userdel NAME\n"); return false; }
    if (userdb64_del64(name) != 0) {
        ts_puts(ts, "userdel: FAILED (no such user / root / last normal user)\n");
        return false;
    }
    ts_puts(ts, "userdel: removed ");
    ts_puts(ts, name);
    ts_puts(ts, " (home directory kept)\n");
    return true;
}

static bool cmd_passwd(TerminalState* ts, const char* name, const char* pw) {
    char who[USERDB64_NAME_MAX];
    const char* src = (name && name[0]) ? name : userdb64_session_name64();
    int i = 0;
    while (src && src[i] && i < (int)sizeof(who) - 1) { who[i] = src[i]; i++; }
    who[i] = 0;
    if (!who[0] || who[0] == '-') { ts_puts(ts, "passwd: usage: passwd [NAME] [PW]\n"); return false; }
    const int idx = userdb64_find64(who);
    if (idx < 0) { ts_puts(ts, "passwd: no such user: "); ts_puts(ts, who); ts_puts(ts, "\n"); return false; }
    if (pw && pw[0]) {                          // 非交互形式：passwd NAME PW
        if (pw[0] == '-' && !pw[1]) {           //   PW 为 "-" = 清口令（回到免密码直接登录）
            if (userdb64_set_password64(idx, "") != 0) { ts_puts(ts, "passwd: write failed\n"); return false; }
            ts_puts(ts, "passwd: password cleared (direct login)\n");
            return true;
        }
        if (userdb64_set_password64(idx, pw) != 0) { ts_puts(ts, "passwd: write failed\n"); return false; }
        ts_puts(ts, "passwd: password updated (stored as salted SHA-256; never plaintext)\n");
        return true;
    }
    g_pw_pending = 1;                           // 交互形式：下一行隐藏输入（空行 = 取消）
    i = 0;
    while (who[i] && i < (int)sizeof(g_pw_user) - 1) { g_pw_user[i] = who[i]; i++; }
    g_pw_user[i] = 0;
    ts_puts(ts, "New password for ");
    ts_puts(ts, who);
    ts_puts(ts, " (input hidden; empty line cancels): ");
    return true;
}

static void cmd_users(TerminalState* ts) {
    static char buf[2048];
    const int n = userdb64_report64(buf, (int)sizeof(buf));
    for (int i = 0; i < n; i++) ts_putc(ts, (uint32_t)(unsigned char)buf[i]);
    // ★ 同一份报告也进串口（自动验收要 grep `[USER64] users count=.. root=1 normal=..`；有界）
    dbg64_line_begin64();
    for (int i = 0; i < n; i++) {
        const char c = buf[i];
        if (c == '\n') dbg64_nl(); else dbg64_putc(c);
    }
    dbg64_line_end64();
}

static void cmd_whoami(TerminalState* ts) {
    const char* u = userdb64_session_name64();
    ts_puts(ts, u);
    ts_putc(ts, (uint32_t)'\n');
    dbg64_line_begin64();
    dbg64_str("[USER64] whoami user=");
    dbg64_str(u);
    dbg64_str(" euid=");
    dbg64_dec((uint64_t)userdb64_euid64());
    dbg64_str(" gui=");
    dbg64_str(userdb64_gui_user64() ? userdb64_gui_user64()->name : "-");
    dbg64_str(" root_session=");
    dbg64_dec((uint64_t)userdb64_root_session64());
    dbg64_nl();
    dbg64_line_end64();
}

// ★ P4：id 显示**真实** uid/gid/euid/egid（终端 = 会话身份；没有附加组 —— 如实标注）
static void cmd_id(TerminalState* ts) {
    Vfs64Cred64 c;
    vfs64_get_cred64(&c);
    ts_puts(ts, "uid=");
    ts_put_u64(ts, (uint64_t)c.uid);
    ts_puts(ts, "(");
    ts_user_name64(ts, c.uid);
    ts_puts(ts, ") gid=");
    ts_put_u64(ts, (uint64_t)c.gid);
    ts_puts(ts, "(");
    ts_user_name64(ts, c.gid);
    ts_puts(ts, ") euid=");
    ts_put_u64(ts, (uint64_t)c.euid);
    ts_puts(ts, " egid=");
    ts_put_u64(ts, (uint64_t)c.egid);
    ts_puts(ts, " groups=(");
    ts_user_name64(ts, c.gid);
    ts_puts(ts, ")  [no supplementary groups]\\n");
    dbg64_line_begin64();
    dbg64_str("[PERM64] id uid=");
    dbg64_dec(c.uid);
    dbg64_str(" gid=");
    dbg64_dec(c.gid);
    dbg64_str(" euid=");
    dbg64_dec(c.euid);
    dbg64_str(" egid=");
    dbg64_dec(c.egid);
    dbg64_str(" user=");
    dbg64_str(userdb64_session_name64());
    dbg64_str(" root=");
    dbg64_dec(userdb64_root_session64() ? 1u : 0u);
    dbg64_nl();
    dbg64_line_end64();
}

// ★ P4：su [-] [名字] —— 切会话身份。口令规则（Linux 的裁剪版，如实标注）：
//   * 目标 == 当前会话用户：直接报"已经是它"（Linux 的 su self 也不问口令）；
//   * 当前 euid == 0：直接切（root 不需要口令）；
//   * 目标**没有口令**：直接切（与登录界面"没设密码直接登录"一致）；
//   * 否则：交互式隐藏输入口令（下一行），错口令/空行 = 取消。
static bool cmd_su(TerminalState* ts, const char* a1, const char* a2) {
    const char* target = "root";                // 支持：su | su - | su - root | su root | su - A | su A
    const char* via = "su";
    if (a1 && a1[0]) {
        if (st_eq(a1, "-")) { via = "su-dash"; if (a2 && a2[0]) target = a2; }
        else { target = a1; via = "su"; }
    }
    const int ti = userdb64_find64(target);
    if (ti < 0) {
        ts_puts(ts, "su: no such user: ");
        ts_puts(ts, target);
        ts_putc(ts, (uint32_t)'\n');
        return false;
    }
    const int ci = userdb64_find64(userdb64_session_name64());
    if (ci == ti) {
        ts_puts(ts, "su: already ");
        ts_puts(ts, target);
        ts_puts(ts, " (session identity unchanged)\\n");
        return true;
    }
    const User64Entry* te = userdb64_at64(ti);
    const bool need_pw = (te && te->hash[0] && userdb64_euid64() != 0);
    if (need_pw) {
        g_su_pending = 1;
        int i = 0;
        for (; target[i] && i < (int)sizeof(g_su_user) - 1; i++) g_su_user[i] = target[i];
        g_su_user[i] = 0;
        for (i = 0; via[i] && i < (int)sizeof(g_su_via) - 1; i++) g_su_via[i] = via[i];
        g_su_via[i] = 0;
        ts_puts(ts, "Password: ");
        dbg64_line_begin64();
        dbg64_str("[TERM] cmd su password-prompt user=");
        dbg64_str(g_su_user);
        dbg64_nl();
        dbg64_line_end64();
        return true;
    }
    if (userdb64_su64(target, via) != 0) {
        ts_puts(ts, "su: FAILED (not logged in / no such user)\\n");
        return false;
    }
    ts_puts(ts, "su: session identity is now ");
    ts_puts(ts, target);
    ts_puts(ts, " (euid=");
    ts_put_u64(ts, (uint64_t)userdb64_euid64());
    ts_puts(ts, "); permission checks are ENFORCED (P4); GUI user/avatar unchanged.\\n");
    return true;
}
// su 的口令行（隐藏输入；空行/错口令 = 取消）
static void cmd_su_line(TerminalState* ts, const char* line) {
    const int ti = userdb64_find64(g_su_user);
    const bool ok = (ti >= 0) && line && line[0] && (userdb64_check_password64(ti, line) == 1);
    const bool cancelled = (!line || !line[0]);
    char user[USERDB64_NAME_MAX];
    char via[16];
    int i = 0;
    for (; g_su_user[i] && i < (int)sizeof(user) - 1; i++) user[i] = g_su_user[i];
    user[i] = 0;
    for (i = 0; g_su_via[i] && i < (int)sizeof(via) - 1; i++) via[i] = g_su_via[i];
    via[i] = 0;
    g_su_pending = 0;
    g_su_user[0] = 0;
    g_su_via[0] = 0;
    if (!ok) {
        dbg64_line_begin64();
        dbg64_str(cancelled ? "[TERM] cmd su cancelled user=" : "[TERM] cmd su auth-failure user=");
        dbg64_str(user);
        dbg64_nl();
        dbg64_line_end64();
        ts_puts(ts, cancelled ? "su: cancelled (empty input)\\n" : "su: Authentication failure\\n");
        return;
    }
    if (userdb64_su64(user, via[0] ? via : "su") != 0) {
        ts_puts(ts, "su: FAILED (not logged in)\\n");
        return;
    }
    ts_puts(ts, "su: ok, session identity = ");
    ts_puts(ts, user);
    ts_puts(ts, " (euid=");
    ts_put_u64(ts, (uint64_t)userdb64_euid64());
    ts_puts(ts, "); permission checks are ENFORCED (P4)\\n");
}

static bool cmd_sudo(TerminalState* ts, const char* a1) {
    if (!a1 || !st_eq(a1, "-i")) {
        ts_puts(ts, "sudo: only 'sudo -i' is implemented in this batch (no command execution yet)\\n");
        return false;
    }
    if (userdb64_session_root64("sudo-i") != 0) {
        ts_puts(ts, "sudo: FAILED (not logged in / no root record)\\n");
        return false;
    }
    ts_puts(ts, "sudo: session identity is now root (uid=0); permission checks are ENFORCED (P4);\\n");
    ts_puts(ts, "      the GUI user/avatar is unchanged.\\n");
    return true;
}

static bool cmd_exit(TerminalState* ts) {
    if (userdb64_session_exit64() == 0) {
        ts_puts(ts, "exit: back to ");
        ts_puts(ts, userdb64_session_name64());
        ts_puts(ts, " (euid=");
        ts_put_u64(ts, (uint64_t)userdb64_euid64());
        ts_puts(ts, ")\n");
        return true;
    }
    ts_puts(ts, "exit: this is not a root session (the terminal window stays open; use Esc to close)\n");
    return false;
}

static bool cmd_loginctl(TerminalState* ts, const char* a1) {
    if (!a1 || !a1[0]) {
        ts_puts(ts, "loginctl: usage: loginctl lock   (lock the screen; only 'lock' is implemented)\n");
        ts_puts(ts, "loginctl: session user=");
        ts_puts(ts, userdb64_session_name64());
        ts_puts(ts, " euid=");
        ts_put_u64(ts, (uint64_t)userdb64_euid64());
        ts_puts(ts, " gui=");
        ts_puts(ts, userdb64_gui_user64() ? userdb64_gui_user64()->name : "-");
        ts_putc(ts, (uint32_t)'\n');
        return true;
    }
    if (st_eq(a1, "lock")) {
        ts_puts(ts, "loginctl: locking (press Enter/click on the lock screen, then sign in again)\n");
        locklogin64_lock64("terminal");
        return true;
    }
    ts_puts(ts, "loginctl: unknown verb (only 'lock' is implemented)\n");
    return false;
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
        cmd_ps(ts, g_arg1);
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
        ok = cmd_display(ts, g_arg1);
    } else if (st_eq(g_cmd, "ls") || st_eq(g_cmd, "dir")) {
        cmd_ls(ts, g_arg1, g_arg2);                 // ★ P4：ls [-l] [路径]
    } else if (st_eq(g_cmd, "cat")) {
        ok = cmd_cat(ts, g_arg1);
    } else if (st_eq(g_cmd, "fatcheck")) {
        // ★ 批次 K：只读 CRC32 校验（默认核对 ESP 的三个文件；用于"读出来的字节与构建产物一致"的可执行证据）
        ok = cmd_fatcheck(ts, g_arg1);
    } else if (st_eq(g_cmd, "bigtest")) {
        // ★ 批次 M：大文件（二级间接块）生成/校验/上限边界/回收 —— 自动验收的入口
        // bigtest copy <src> <字母> 要用"第一个参数之后的全部"（args2），其余模式只用 g_arg1
        ok = st_eq(g_arg1, "copy") ? cmd_bigtest_copy(ts, args2) : cmd_bigtest(ts, g_arg1);
    } else if (st_eq(g_cmd, "write") || st_eq(g_cmd, "save")) {
        ok = cmd_write(ts, g_arg1, args2);
    } else if (st_eq(g_cmd, "touch")) {
        ok = cmd_touch(ts, g_arg1);
    } else if (st_eq(g_cmd, "rm") || st_eq(g_cmd, "del")) {
        ok = cmd_rm(ts, g_arg1);
    } else if (st_eq(g_cmd, "echo")) {
        ok = cmd_echo(ts, args);
    } else if (st_eq(g_cmd, "mkdir") || st_eq(g_cmd, "md")) {
        // 真：vfs64_mkdir（阶段一：父目录固定为根，单层）
        ok = cmd_mkdir(ts, g_arg1);
    } else if (st_eq(g_cmd, "df")) {
        // 真：VimtuFS2 卷几何（总块/空闲块/已用）+ 实时文件统计 + **所有卷**清单
        ok = cmd_df(ts);
    } else if (st_eq(g_cmd, "vol")) {
        // ★ 多卷：vol 列出卷/盘符；vol C:|D: 切换"当前卷"（ls/cat/write/... 都作用在当前卷上）
        ok = cmd_vol(ts, g_arg1);
    } else if (st_eq(g_cmd, "fdtest")) {
        // 批次 D：FD 语义演示（独立游标 / dup 共享游标 / O_APPEND / pipe 环回）。
        // 实现在 fd64.cpp（fd64_demo64），这里只负责跑 + 一行命令级打点（自动验收 grep）。
        const int fdmask = fd64_demo64();
        ok = (fdmask == 0);
        ts_puts(ts, "FD semantics demo (kernel table): independent cursors + dup-shared offset + ");
        ts_puts(ts, "O_APPEND + pipe ring-back -> see serial ([FD64] demo ...)\n");
        ts_puts(ts, "  table=");
        ts_put_u64(ts, (uint64_t)FD64_MAX);
        ts_puts(ts, "  per-process=1  fd_test_mask=");
        ts_put_u64(ts, (uint64_t)fdmask);
        ts_putc(ts, (uint32_t)'\n');
    } else if (st_eq(g_cmd, "fd")) {
        // 真：FD 层现状（当前表的 fd 槽 / refs / 游标）；也打 [FD64] dump 行
        fd64_dump64();
        ts_puts(ts, "FD layer dump written to serial ([FD64] dump ...); table=");
        ts_put_u64(ts, (uint64_t)FD64_MAX);
        ts_puts(ts, "  file_max=");
        ts_put_u64(ts, (uint64_t)FD64_FILE_MAX);
        ts_putc(ts, (uint32_t)'\n');
    } else if (st_eq(g_cmd, "lang") || st_eq(g_cmd, "language")) {
        ok = cmd_lang(ts, g_arg1);
    } else if (st_eq(g_cmd, "set")) {
        ok = cmd_set(ts, g_arg1, g_arg2);
    } else if (st_eq(g_cmd, "about")) {
        cmd_about(ts);
    } else if (st_eq(g_cmd, "rust")) {
        // ★ Rust 模块（gui_rs）：设计 Token + 主题配色（rust | rust tokens | rust set <idx>）
        ok = cmd_rust(ts, g_arg1, g_arg2);
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
        cmd_ps(ts, g_arg1);  // 与 ps 同一份真实快照（top 不做全屏刷新，只打一次）
    } else if (st_eq(g_cmd, "syslog")) {
        // 真：sysstate64 的 ring log（固定 64 条循环日志）
        cmd_syslog(ts);
    } else if (st_eq(g_cmd, "dmesg")) {
        // ★ 批次 N：开机滚屏引导控制台的启动日志缓冲（头部保留 + 16 KiB 环形缓冲）
        cmd_dmesg(ts);
    } else if (st_eq(g_cmd, "boot")) {
        // ★ 批次 N：进桌面前的滚屏引导控制台开关（boot | boot verbose | boot verbose on|off）
        ok = cmd_boot(ts, g_arg1, g_arg2);
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
        // 真：update 子系统（kernel/update64.cpp）—— status / pending <ver> / apply
        ok = cmd_update(ts, g_arg1, g_arg2);
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
        // 真：预热统计（kernel/preload64.cpp；启动期已跑过，`preload run` 可再跑一轮验证幂等）
        ok = cmd_preload(ts, g_arg1);
    } else if (st_eq(g_cmd, "proc")) {
        // 真：proc64 进程表（list / run / kill）；`proc run spin` 跑内嵌的长命程序 /spin.elf
        ok = cmd_proc(ts, g_arg1, g_arg2, args2);
    } else if (st_eq(g_cmd, "useradd")) {
        ok = cmd_useradd(ts, g_arg1);
    } else if (st_eq(g_cmd, "userdel")) {
        ok = cmd_userdel(ts, g_arg1);
    } else if (st_eq(g_cmd, "passwd")) {
        ok = cmd_passwd(ts, g_arg1, g_arg2);
    } else if (st_eq(g_cmd, "users")) {
        cmd_users(ts);
    } else if (st_eq(g_cmd, "whoami")) {
        cmd_whoami(ts);
    } else if (st_eq(g_cmd, "id")) {
        cmd_id(ts);
    } else if (st_eq(g_cmd, "su")) {
        ok = cmd_su(ts, g_arg1, g_arg2);
    } else if (st_eq(g_cmd, "sudo")) {
        ok = cmd_sudo(ts, g_arg1);
    } else if (st_eq(g_cmd, "chmod")) {                     // ★ P4：chmod MODE PATH
        ok = cmd_chmod(ts, g_arg1, g_arg2);
    } else if (st_eq(g_cmd, "chown") || st_eq(g_cmd, "chgrp")) {   // ★ P4：chown USER[:GROUP] PATH
        ok = cmd_chown(ts, g_arg1, g_arg2);
    } else if (st_eq(g_cmd, "umask")) {                     // ★ P4：umask [MASK]
        ok = cmd_umask(ts, g_arg1);
    } else if (st_eq(g_cmd, "tree")) {                      // ★ P4：tree [路径]（串口行带 uid/gid/mode）
        ok = cmd_tree(ts, g_arg1);
    } else if (st_eq(g_cmd, "exit") || st_eq(g_cmd, "logout")) {
        ok = cmd_exit(ts);
    } else if (st_eq(g_cmd, "loginctl")) {
        ok = cmd_loginctl(ts, g_arg1);
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
        if (g_pw_pending) cmd_passwd_line(ts, ts->cmdline);   // ★ P1c：交互式口令那一行
        else if (g_su_pending) cmd_su_line(ts, ts->cmdline);   // ★ P4：su 的口令行（隐藏输入）
        else shell_exec(ts, ts->cmdline);
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
            // ★ P1c：口令输入不回显（只打 '*'）—— 明文绝不进屏/串口
            ts_putc(ts, (g_pw_pending || g_su_pending) ? (uint32_t)'*' : (uint32_t)(unsigned char)c);
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
    term_install_builtins64();      // /spin.elf + /filedemo.elf（幂等；终端文件命令现在走真 FS）
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

    // 目标客户区 = 默认 60x40 个**汉字格**（120x40 个半格；像素尺寸与旧版 60x40 位图格完全一致）
    int cw  = TERM_DEF_COLS * TERM_HALF_W + TERM_PAD_X;
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
