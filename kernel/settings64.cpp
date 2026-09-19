// settings64.cpp - VimtuOS 设置应用（64 位；Win10 风格：左侧导航 + 右侧字段区）
//
// 与 32 位参考实现（Vimtu32/kernel/gui.cpp 的 set_* 一族，4214 行里 2432..3400）的关系：
//   * 布局/几何照抄：客户区尺寸推导全部坐标（窗口拖动缩放后内容自适应）、
//     下拉框落点"看到的 = 点得到的"（同一个 set_list_box 既用于绘制也用于命中）、
//     导航栏选中项浅蓝底 + 左侧蓝条、字段白底 + 灰框、字段标签在框上方 26px。
//   * 入口/单实例/最小尺寸/日志行按 64 位 gui64.h 契约重写（接口按 app_id 为键，
//     不再用 32 位那套"按槽位下标查会话表"）。
//
// ★★ 诚实边界（能做的做，做不到的如实标注，绝不伪造成功）★★
//   1) 显示-分辨率：真调 kernel/fb.cpp 的 fb_set_mode()（Bochs VBE DISPI 寄存器 + 回读校验）。
//      QEMU stdvga 能切；VMware/真机/部分固件会拒绝 —— 失败时页面上与日志里都如实写
//      "切换失败"，并把实测的物理分辨率回填到字段（字段永远显示实测值）。
//   2) 显示-刷新率：真值来自显示器 EDID —— 引导层把第一块 128 字节 EDID 读到物理 0x7600
//      （boot/loader64.asm 的 read_edid），运行期由 kernel/edid64.cpp 解析出首选时序的
//      刷新率（pclk / (Htotal * Vtotal)，保留一位小数）。固件没给 EDID 时字段如实写
//      "未知 (无 EDID)"，绝不编数字。仍不读 0x3DA/CRTC 回扫（64 位没有 32 位 display.cpp
//      那条路），所以刷新率只能看、不能改：点它只打一行日志 + 提示行。
//   3) 会话持久化：store64 模块本体已可用（VimtuFS2 的 /store.a、/store.b；启动路径 init + 终端
//      store 命令），但**设置页/启动项还没接它**；会话页如实写明"关闭窗口即清空该应用状态 /
//      硬重启回默认值"。
//   4) 语言：调外壳 gui64_set_lang_zh()，本页文案与导航、窗口标题立即跟随（gui64_lang_zh() 判断）。
//   5) 没 API 的地方自己补 static 实现（见本文件"缺 API 的自建替代"一节）：CPU 品牌串（CPUID）、
//      整数与 64 位数值转字符串（无 libc）、字符串追加缓冲。
//
// 串口日志（自动验收断言用，行首格式必须逐字一致）：
//   [APP] settings opened
//   [APP] settings closed
//   [APP] settings reset
//   [UI] settings layout client=WxH nav=N field=N
//   [UI] settings mode WxH ok      /  [UI] settings mode WxH fail
//   [UI] settings zoom P
//   [UI] settings lang zh          /  [UI] settings lang en
//   另外还有若干诊断行（前缀不同，便于自动化区分）：
//   [UI] settings page=N (name) / dropdown open=res items=6 / hz from-edid refresh=.. /
//   hz unknown (no EDID) / display hz=<x.x>Hz source=edid preferred-timing （显示页首帧打一次） /
//   apply res=.. zoom=.. / vbe reject want=.. got=.. / single-instance activate /
//   session close-apps n=N / create failed
//
// 编译约束（内核 -mno-sse）：只用整数运算，无 float/double；不 include 任何标准头；
//   不做堆分配（全部状态是静态变量，关窗后由 reset 清干净）。
// 绘制约定：屏幕绝对坐标 = w->client_x/y + 局部坐标。

#include "gui64.h"
#include "fb.h"
#include "font.h"
#include "input.h"
#include "memlayout64.h"    // ★ 必须排在 mem_64.h 之前：mem_64.h 里的 #define PAGE_SIZE_64
                            //   宏会顶掉 memlayout64.h 的同名 static const（两个 64 位头各有一份）
#include "mem_64.h"
#include "debug64.h"
#include "x86_64.h"
#include "edid64.h"      // 显示器 EDID 解析结果（刷新率/名字/尺寸）：只读，不改 fb 状态
#include "config64.h"    // 本轮接线：语言/缩放/会话策略都落在这里（-> store64 持久化）
#include "session64.h"   // 本轮接线：会话页 = session64 的真策略与 keep 开关

// ==================== 常量 ====================
#define SET_W      700          // 窗口整体尺寸（含标题栏/边框；与 32 位 SET_W/SET_H 一致）
#define SET_H      460
#define SET_MIN_W  520          // 与 32 位 gui_min_size(APP_ID_SETTINGS) 的校准值一致
#define SET_MIN_H  360

#define PG_DISPLAY 0            // 页序（本文件导航顺序）：显示 / 系统 / 会话 / 关于
#define PG_SYSTEM  1
#define PG_SESSION 2
#define PG_ABOUT   3
#define PG_COUNT   4

#define DROP_NONE 0
#define DROP_RES  1
#define DROP_ZOOM 2

#define RES_SEL_NONE (-1)

// 分辨率候选（任务给定的固定列表；64 位 fb.h 没有"查询适配器可用模式"的接口，
// 只能列候选 + 切换时靠 fb_set_mode 的回读校验如实报成败）
#define SET_RES_N 6
static const int kResW[SET_RES_N] = { 1024, 1280, 1280, 1440, 1600, 1920 };
static const int kResH[SET_RES_N] = {  768,  800, 1024,  900,  900, 1080 };

// 缩放候选（fb_set_zoom 只接受 100/125/150 这档；驱动侧钳制在 100..200）
#define SET_ZOOM_N 3
static const int kZoomPct[SET_ZOOM_N] = { 100, 125, 150 };

#define MSG_LEN 112

// ---- 调色板（32bpp ARGB，必须带 0xFF 前缀：后备缓冲是原样拷贝到 LFB 的）----
static const uint32_t C_TEXT   = 0xFF1A1A1A;
static const uint32_t C_DIM    = 0xFF666666;
static const uint32_t C_FAINT  = 0xFF9A9A9A;
static const uint32_t C_ACCENT = 0xFF108CE0;
static const uint32_t C_NAVBG  = 0xFFFFFFFF;
static const uint32_t C_PAGEBG = 0xFFF7F7F7;
static const uint32_t C_SEL    = 0xFFE8F0FE;
static const uint32_t C_SEP    = 0xFFDCDCDC;
static const uint32_t C_FIELD  = 0xFFFFFFFF;
static const uint32_t C_BORDER = 0xFF909090;
static const uint32_t C_BTN    = 0xFF0078D7;
static const uint32_t C_BTN_DIS= 0xFF9AB6CE;
static const uint32_t C_WARN   = 0xFFA00000;
static const uint32_t C_OK     = 0xFF107C10;

// ==================== 状态（单实例） ====================
static Window*  g_win = nullptr;
static int      g_page = PG_DISPLAY;
static int      g_drop = DROP_NONE;      // 当前展开的下拉（同一时刻只允许一个）
static int      g_drop_hover = -1;       // 高亮项（列表下标）
static int      g_drop_top = 0;          // 列表滚动起点
static int      g_res_sel = RES_SEL_NONE;// 当前分辨率在候选表里的下标（-1 = 不在候选表里）
static int      g_zoom_sel = 0;          // 当前缩放在候选表里的下标
static char     g_msg[MSG_LEN];
static uint32_t g_msg_t0 = 0;
static uint32_t g_msg_col = C_DIM;
static int      g_prev_cw = -1, g_prev_ch = -1;   // 布局日志去重（每帧打会刷屏）
static bool     g_close_logged = false;           // "[APP] settings closed" 只打一次
static bool     g_hz_logged = false;              // 显示页刷新率来源只在首次出值时打一行（自动验收 grep）

// ==================== 迷你字符串工具（无 libc） ====================
// 缺 API 的自建替代 #1：内核没有 snprintf/strcat，这里做一个定长追加缓冲。
struct Buf {
    char b[256];
    int  n;
};

static void b_init(Buf* s) { s->b[0] = 0; s->n = 0; }

static void b_ch(Buf* s, char c) {
    if (s->n < (int)sizeof(s->b) - 1) { s->b[s->n++] = c; s->b[s->n] = 0; }
}

static void b_str(Buf* s, const char* t) {
    for (int i = 0; t[i]; i++) b_ch(s, t[i]);
}

static void b_u64(Buf* s, uint64_t v) {
    char t[24];
    int n = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0) { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (n > 0) b_ch(s, t[--n]);
}

static void b_int(Buf* s, int v) {
    if (v < 0) { b_ch(s, '-'); b_u64(s, (uint64_t)(-(int64_t)v)); }
    else b_u64(s, (uint64_t)v);
}

static void b_hex(Buf* s, uint64_t v, int digits) {
    static const char* H = "0123456789ABCDEF";
    b_str(s, "0x");
    for (int i = digits - 1; i >= 0; i--) b_ch(s, H[(int)((v >> (i * 4)) & 0xF)]);
}

// ==================== 串口日志 ====================
static void logln(const char* s) { dbg64_str(s); dbg64_nl(); }

static void log_dec(const char* k, uint64_t v) { dbg64_str(k); dbg64_dec(v); dbg64_nl(); }

// ==================== 缺 API 的自建替代 #2：CPUID ====================
// 64 位内核没有 32 位的 hwinfo/cpu 接口，系统页的"处理器"一行自己读 CPUID。
static void cpuid64(uint32_t leaf, uint32_t sub, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

// 取 CPU 厂商串（12 字节，来自 CPUID leaf 0）
static void cpu_vendor(char* out, int cap) {
    uint32_t a, b, c, d;
    cpuid64(0, 0, &a, &b, &c, &d);
    uint32_t v[3];
    v[0] = b; v[1] = d; v[2] = c;    // EBX,EDX,ECX 顺序
    int k = 0;
    for (int i = 0; i < 12 && k < cap - 1; i++) {
        char ch = (char)((v[i / 4] >> ((i % 4) * 8)) & 0xFF);
        if (ch < 0x20 || ch > 0x7E) ch = ' ';
        out[k++] = ch;
    }
    out[k] = 0;
    while (k > 0 && out[k - 1] == ' ') out[--k] = 0;
}

// 取 CPU 品牌串（48 字节，来自 CPUID leaf 0x80000002..4）。
// 返回 false = 该 CPU/模拟器不提供（调用方必须回退，不能显示垃圾）。
static bool cpu_brand(char* out, int cap) {
    uint32_t a, b, c, d;
    cpuid64(0x80000000u, 0, &a, &b, &c, &d);
    if (a < 0x80000004u) return false;
    char raw[49];
    int k = 0;
    for (uint32_t leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
        cpuid64(leaf, 0, &a, &b, &c, &d);
        uint32_t w[4];
        w[0] = a; w[1] = b; w[2] = c; w[3] = d;
        for (int i = 0; i < 16; i++) {
            char ch = (char)((w[i / 4] >> ((i % 4) * 8)) & 0xFF);
            raw[k++] = (ch < 0x20 || ch > 0x7E) ? ' ' : ch;   // 非可打印一律替换，防乱码
        }
    }
    raw[48] = 0;
    int s = 0;
    while (s < 48 && raw[s] == ' ') s++;                      // 去前导空格
    int e = 47;
    while (e >= s && raw[e] == ' ') e--;                      // 去尾随空格
    if (e < s) return false;                                  // 全空 = 不可用
    int o = 0;
    for (int i = s; i <= e && o < cap - 1; i++) {
        char ch = raw[i];
        // 压掉连续空格（CPUID 品牌串里常见多空格）
        if (ch == ' ' && o > 0 && out[o - 1] == ' ') continue;
        out[o++] = ch;
    }
    out[o] = 0;
    return o > 0;
}

// ==================== 布局几何（照 32 位 set_geom / set_sy） ====================
struct Lay {
    int cw, ch;              // 客户区尺寸
    int nav_w;               // 左导航宽度（窄窗口自动收窄）
    int cx0;                 // 内容区左边（客户区局部坐标）
    int fx, fw, fh;          // 下拉字段：左边 / 宽 / 高
    int nav_pitch, nav_hh;   // 导航项行距 / 行高
    int y_hdr;               // 页面小标题
    int y_res, y_hz, y_zoom; // 显示页三个字段的 y
    int btn_x, btn_y, btn_w, btn_h;   // 应用按钮
    int y_info;              // 显示页信息行起点
    int y_rows;              // 系统页/会话页第一行
    int y_note, y_note2;     // 会话页说明行
    int y_act;               // 会话页动作按钮
    int y_foot;              // 页脚说明
    int lang_y, lang_h, lang_w, lang_x1, lang_x2;   // 导航底部语言切换
};

// 纵向"自然坐标"（以 436 高客户区为基准）→ 实际 y：客户区变矮时按比例压缩行距，
// 保证所有内容都在窗口内（绘制与命中共用，绝不出界）。
static int lay_sy(int ch, int nat_y) {
    if (nat_y <= 62) return nat_y;
    int sc = ch * 256 / 436;
    if (sc > 256) sc = 256;
    if (sc < 150) sc = 150;
    return 62 + (nat_y - 62) * sc / 256;
}

static void lay_calc(int cw, int ch, Lay* L) {
    L->cw = cw;
    L->ch = ch;
    L->nav_w = (cw >= 560) ? 190 : (cw >= 430 ? 150 : 118);
    L->cx0 = L->nav_w + 22;
    L->fx = L->nav_w + 26;
    L->fw = cw - L->fx - 24;
    if (L->fw > 420) L->fw = 420;
    if (L->fw < 120) L->fw = 120;
    L->fh = (ch >= 420) ? 34 : 28;
    L->nav_pitch = (ch < 320) ? 32 : 42;
    L->nav_hh = (ch < 320) ? 28 : 38;

    L->y_hdr  = lay_sy(ch, 74);
    L->y_res  = lay_sy(ch, 84);
    L->y_hz   = lay_sy(ch, 148);
    L->y_zoom = lay_sy(ch, 212);
    L->btn_w = 120;
    if (L->btn_w > L->fw) L->btn_w = L->fw;
    L->btn_h = 30;
    L->btn_x = L->fx;
    L->btn_y = L->y_zoom + L->fh + 16;
    L->y_info = L->btn_y + L->btn_h + 14;
    L->y_rows = lay_sy(ch, 104);
    L->y_note = lay_sy(ch, 244);
    L->y_note2 = lay_sy(ch, 264);
    L->y_act = lay_sy(ch, 300);
    L->y_foot = lay_sy(ch, 340);

    L->lang_h = 26;
    L->lang_w = (L->nav_w - 24) / 2;
    if (L->lang_w < 30) L->lang_w = 30;
    L->lang_x1 = 8;
    L->lang_x2 = 8 + L->lang_w + 8;
    L->lang_y = ch - L->lang_h - 10;
    if (L->lang_y < 0) L->lang_y = 0;
}

// 客户区尺寸：外壳会填 client_w/client_h；万一没填就退回"窗口尺寸 - 标题栏/边框"估算。
static int win_cw(Window* w) {
    int v = (w->client_w > 0) ? w->client_w : (w->w - 2);
    return (v < 60) ? 60 : v;
}

static int win_ch(Window* w) {
    int v = (w->client_h > 0) ? w->client_h : (w->h - 26);
    return (v < 60) ? 60 : v;
}

// ==================== 文本绘制 ====================
// face 2 = simhei（子集含 ASCII 32..126 + GB2312 一级汉字），中英混排一律用它，
// 这样英文模式下也不会出现"某 face 缺字形"的空洞。
static int ui_width(const char* s) {
    font_select(2);
    return font_text_width(s);
}

static void ui_text(int x, int y, const char* s, uint32_t color) {
    font_select(2);
    font_draw_text(x, y, s, color);
}

// 文字在 [0,w) 里水平居中时的左端 x（用于按钮/语言按钮的居中排版）
static int centered_x(int w, const char* s) {
    int tx = (w - ui_width(s)) / 2;
    return tx < 0 ? 0 : tx;
}

// ==================== 消息行（操作结果 / 不支持原因，6 秒后消失） ====================
static void msg_set(const char* s, uint32_t col) {
    int i = 0;
    while (s[i] && i < MSG_LEN - 1) { g_msg[i] = s[i]; i++; }
    g_msg[i] = 0;
    g_msg_t0 = ticks64();
    g_msg_col = col;
}

static bool msg_fresh() {
    if (!g_msg[0]) return false;
    return (uint32_t)(ticks64() - g_msg_t0) < ms_to_ticks64(6000);
}

// ==================== 候选表 <-> 实测值 ====================
static int res_sel_from_actual() {
    int w = fb_phys_width(), h = fb_phys_height();
    for (int i = 0; i < SET_RES_N; i++) if (kResW[i] == w && kResH[i] == h) return i;
    return RES_SEL_NONE;
}

static int zoom_sel_from_actual() {
    int z = fb_get_zoom();
    for (int i = 0; i < SET_ZOOM_N; i++) if (kZoomPct[i] == z) return i;
    // 实测值不在候选表里（驱动钳制/别处改过）：挑最接近的候选作为显示项
    int best = 0, bd = 1000;
    for (int i = 0; i < SET_ZOOM_N; i++) {
        int d = kZoomPct[i] - z;
        if (d < 0) d = -d;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

// ==================== 动作：分辨率 / 缩放 / 语言 ====================
// 分辨率/缩放变化后：屏幕（渲染分辨率）尺寸变了，窗口可能出界，交给外壳重新钳制并整屏重绘。
static void after_geom_change(Window* w) {
    if (!w) return;
    gui64_fit_window_to_client(w, win_cw(w), win_ch(w));
    gui64_invalidate();
}

// 分辨率切换：真调 fb_set_mode；成败都如实回报（页面文字 + 串口日志）。
static void apply_res(Window* w, int idx) {
    if (idx < 0 || idx >= SET_RES_N) return;
    int ww = kResW[idx], hh = kResH[idx];
    int curw = fb_phys_width(), curh = fb_phys_height();
    g_drop = DROP_NONE;
    g_drop_hover = -1;

    bool ok;
    if (ww == curw && hh == curh) {
        ok = true;                          // 已是该模式：无需重新编程寄存器，如实报 ok
    } else {
        ok = fb_set_mode(ww, hh);
        if (ok && (fb_phys_width() != ww || fb_phys_height() != hh)) ok = false;  // 回读校验再确认一次
    }

    dbg64_str("[UI] settings mode ");
    dbg64_dec((uint64_t)ww); dbg64_str("x"); dbg64_dec((uint64_t)hh);
    dbg64_str(ok ? " ok" : " fail");
    dbg64_nl();

    if (ok) {
        g_res_sel = res_sel_from_actual();
        Buf b; b_init(&b);
        b_str(&b, gui64_lang_zh() ? "已切换分辨率：" : "Resolution applied: ");
        b_int(&b, fb_phys_width()); b_str(&b, " x "); b_int(&b, fb_phys_height());
        msg_set(b.b, C_OK);
        after_geom_change(w);
    } else {
        g_res_sel = res_sel_from_actual();   // 字段永远显示实测值，不显示"以为切成功"的值
        // （实测值不在候选表里时 g_res_sel = RES_SEL_NONE，下拉里也就不标"当前"）
        dbg64_str("[UI] settings vbe reject want=");
        dbg64_dec((uint64_t)ww); dbg64_str("x"); dbg64_dec((uint64_t)hh);
        dbg64_str(" got=");
        dbg64_dec((uint64_t)fb_phys_width()); dbg64_str("x");
        dbg64_dec((uint64_t)fb_phys_height());
        dbg64_nl();
        Buf b; b_init(&b);
        b_str(&b, gui64_lang_zh() ? "切换失败：适配器拒绝该模式（保持 " : "mode change failed (kept ");
        b_int(&b, fb_phys_width()); b_str(&b, " x "); b_int(&b, fb_phys_height());
        b_str(&b, gui64_lang_zh() ? "）" : ")");
        msg_set(b.b, C_WARN);
        gui64_invalidate();
    }
}

// 缩放切换：真调 fb_set_zoom，日志打**实际生效值**（驱动把超出范围的档位钳制过）
static void apply_zoom(Window* w, int idx) {
    if (idx < 0 || idx >= SET_ZOOM_N) return;
    int want = kZoomPct[idx];
    g_drop = DROP_NONE;
    g_drop_hover = -1;
    fb_set_zoom(want);
    int got = fb_get_zoom();
    g_zoom_sel = zoom_sel_from_actual();

    log_dec("[UI] settings zoom ", (uint64_t)got);
    // 本轮接线：缩放写进 config64（-> store64 /store.a|b，3 秒内自动落盘），下次启动沿用
    cfg64_set_zoom64(got);
    dbg64_line_begin64();
    dbg64_str("[CONF64] set key=display.zoom value=");
    dbg64_dec((uint64_t)got);
    dbg64_str(" type=int (persisted via store64)");
    dbg64_nl();
    dbg64_line_end64();

    Buf b; b_init(&b);
    if (got == want) {
        b_str(&b, gui64_lang_zh() ? "缩放已设为 " : "Zoom set to ");
    } else {
        b_str(&b, gui64_lang_zh() ? "驱动把缩放钳制为 " : "zoom clamped to ");
    }
    b_int(&b, got); b_ch(&b, '%');
    msg_set(b.b, (got == want) ? C_OK : C_WARN);
    after_geom_change(w);
}
// 语言切换：调外壳，本页文案/导航/标题立即跟随
static void lang_set(bool zh) {
    gui64_set_lang_zh(zh);
    if (g_win && gui64_window_alive(g_win)) {
        gui64_set_title(g_win, zh ? "设置" : "Settings");
    }
    logln(zh ? "[UI] settings lang zh" : "[UI] settings lang en");
    msg_set(zh ? "界面语言：中文" : "UI language: English", C_OK);
    gui64_invalidate();
}

// 应用按钮：把当前选中的分辨率与缩放再应用一次（与"选下拉即生效"一致，幂等）
static void do_apply(Window* w) {
    dbg64_str("[UI] settings apply res=");
    dbg64_dec((uint64_t)(g_res_sel >= 0 ? kResW[g_res_sel] : fb_phys_width()));
    dbg64_str("x");
    dbg64_dec((uint64_t)(g_res_sel >= 0 ? kResH[g_res_sel] : fb_phys_height()));
    dbg64_str(" zoom=");
    dbg64_dec((uint64_t)kZoomPct[g_zoom_sel]);
    dbg64_nl();
    if (g_res_sel >= 0) apply_res(w, g_res_sel);
    apply_zoom(w, g_zoom_sel);
    msg_set(gui64_lang_zh() ? "已应用显示设置" : "display settings applied", C_OK);
    gui64_invalidate();
}

// ==================== 下拉列表落点（绘制与命中共用同一份） ====================
struct ListBox {
    int x, y, w, item_h, visible, top, total;
};

static int drop_count() {
    return (g_drop == DROP_RES) ? SET_RES_N : (g_drop == DROP_ZOOM ? SET_ZOOM_N : 0);
}

static int drop_field_y(const Lay* L) {
    return (g_drop == DROP_RES) ? L->y_res : L->y_zoom;
}

// 优先向下展开；下方放不下就向上；都放不下取空间较大的一侧并滚动。
static void list_box(const Lay* L, int field_y, int n, ListBox* box) {
    const int item_h = 24;
    int below_y = field_y + L->fh + 2;
    int avail_b = L->ch - below_y - 6;
    int avail_a = field_y - 4;
    bool use_below = (avail_b >= n * item_h) || (avail_b >= avail_a);
    int avail = use_below ? avail_b : avail_a;
    int visible = avail / item_h;
    if (visible < 1) visible = 1;
    if (visible > n) visible = n;
    int top = g_drop_top;
    if (top > n - visible) top = n - visible;
    if (top < 0) top = 0;
    box->item_h = item_h;
    box->visible = visible;
    box->top = top;
    box->total = n;
    box->x = L->fx;
    box->w = L->fw;
    box->y = use_below ? below_y : (field_y - 4 - visible * item_h);
    if (box->y < 2) box->y = 2;
}

// 保证高亮项落在可见区内（打开下拉与键盘上下移动时调用）
static void drop_ensure_visible(const Lay* L, int field_y, int n) {
    if (n <= 0 || g_drop_hover < 0) return;
    ListBox lb;
    list_box(L, field_y, n, &lb);
    if (g_drop_hover < lb.top) g_drop_top = g_drop_hover;
    else if (g_drop_hover >= lb.top + lb.visible) g_drop_top = g_drop_hover - lb.visible + 1;
    if (g_drop_top < 0) g_drop_top = 0;
    if (g_drop_top > n - lb.visible) g_drop_top = n - lb.visible;
    if (g_drop_top < 0) g_drop_top = 0;
}

// ==================== 绘制元件 ====================
static void draw_field(int x, int y, int w, int h, bool open, const char* text,
                       uint32_t tc, bool arrow) {
    fb_fill_rect(x, y, w, h, C_FIELD);
    fb_draw_rect(x, y, w, h, open ? C_ACCENT : C_BORDER);
    ui_text(x + 12, y + (h - font_line_height()) / 2, text, tc);
    if (arrow) {
        int ax = x + w - 20, ay = y + h / 2 - 2;
        for (int i = 0; i < 5; i++) fb_draw_hline(ax - 4 + i, ay + i, 9 - i * 2, 0xFF444444);
    }
}

static void draw_button(int x, int y, int w, int h, const char* label, bool primary, bool enabled) {
    if (primary) {
        fb_fill_rect(x, y, w, h, enabled ? C_BTN : C_BTN_DIS);
    } else {
        fb_fill_rect(x, y, w, h, 0xFFE9E9E9);
        fb_draw_rect(x, y, w, h, C_BORDER);
    }
    int ty = y + (h - font_line_height()) / 2;
    ui_text(x + centered_x(w, label), ty, label, primary ? 0xFFFFFFFF : C_TEXT);
}

static bool hit(int x, int y, int w, int h, int px, int py) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

// 页面小标题 + 分隔线
static void draw_page_title(const Lay* L, int x0, int y0, const char* title) {
    ui_text(x0 + L->cx0, y0 + 18, title, C_TEXT);
    fb_draw_hline(x0 + L->cx0, y0 + 46, L->cw - L->cx0 - 22, C_SEP);
}

// 展开的下拉列表（最后绘制：覆盖内容区，且一定落在客户区里）
static void draw_dropdown(const Lay* L, int x0, int y0) {
    if (g_drop == DROP_NONE) return;
    int n = drop_count();
    if (n <= 0) return;
    int field_y = drop_field_y(L);
    ListBox lb;
    list_box(L, field_y, n, &lb);
    bool zh = gui64_lang_zh();
    int lx = x0 + lb.x, ly = y0 + lb.y;
    for (int k = 0; k < lb.visible; k++) {
        int i = lb.top + k;
        int iy = ly + k * lb.item_h;
        bool sel = (g_drop == DROP_RES) ? (i == g_res_sel) : (i == g_zoom_sel);
        fb_fill_rect(lx, iy, lb.w, lb.item_h, (i == g_drop_hover) ? C_SEL : C_FIELD);
        if (sel) fb_fill_rect(lx, iy, 4, lb.item_h, C_ACCENT);
        Buf b; b_init(&b);
        if (g_drop == DROP_RES) {
            b_int(&b, kResW[i]); b_str(&b, " x "); b_int(&b, kResH[i]);
            if (kResW[i] == fb_phys_width() && kResH[i] == fb_phys_height())
                b_str(&b, zh ? "  (当前)" : "  (current)");
        } else {
            b_int(&b, kZoomPct[i]); b_ch(&b, '%');
            if (kZoomPct[i] == fb_get_zoom()) b_str(&b, zh ? "  (当前)" : "  (current)");
        }
        ui_text(lx + 14, iy + (lb.item_h - font_line_height()) / 2, b.b, C_TEXT);
    }
    fb_draw_rect(lx, ly, lb.w, lb.visible * lb.item_h, C_BORDER);
    if (lb.visible < lb.total) {   // 还有更多项：如实标出上下滚动
        fb_fill_rect(lx + lb.w - 8, ly + 2, 6, 4, 0xFF808080);
        fb_fill_rect(lx + lb.w - 8, ly + lb.visible * lb.item_h - 6, 6, 4, 0xFF808080);
    }
}

// ==================== 页面：显示 ====================
static void draw_display(Window* w, const Lay* L, int x0, int y0) {
    (void)w;
    bool zh = gui64_lang_zh();
    draw_page_title(L, x0, y0, zh ? "显示" : "Display");

    int fx = x0 + L->fx;
    int fw = L->fw, fh = L->fh;

    // 1) 分辨率：字段显示**实测物理模式**，下拉是候选表
    ui_text(x0 + L->cx0, y0 + L->y_res - 26, zh ? "分辨率" : "Resolution", C_TEXT);
    {
        Buf b; b_init(&b);
        b_int(&b, fb_phys_width()); b_str(&b, " x "); b_int(&b, fb_phys_height());
        b_str(&b, zh ? "  (当前，实测)" : "  (current, measured)");
        draw_field(fx, y0 + L->y_res, fw, fh, g_drop == DROP_RES, b.b, C_TEXT, true);
    }

    // 2) 刷新率：真值 = 显示器 EDID 首选时序（引导层读进 0x7600 -> kernel/edid64.cpp 解析）。
    //    没有 EDID 时如实写"未知 (无 EDID)"；本字段只读（没有下拉、没有 CRTC 控制）。
    ui_text(x0 + L->cx0, y0 + L->y_hz - 26, zh ? "刷新率" : "Refresh rate", C_TEXT);
    {
        const Edid64* ed = edid64_get();
        Buf b; b_init(&b);
        if (ed->valid && ed->refresh_x10) {
            char rb[16];
            edid64_refresh_str64(rb, (int)sizeof(rb));
            b_str(&b, rb);
            b_str(&b, zh ? " Hz  (EDID 首选时序)" : " Hz  (EDID preferred timing)");
            draw_field(fx, y0 + L->y_hz, fw, fh, false, b.b, C_TEXT, false);
        } else {
            b_str(&b, zh ? "未知 (无 EDID)" : "unknown (no EDID)");
            draw_field(fx, y0 + L->y_hz, fw, fh, false, b.b, C_FAINT, false);
        }
        // 自动验收用：显示页首次画出刷新率时打一行，说明这个数字从哪来（有 EDID / 没有）。
        // 只打一次（g_hz_logged），免得 250ms 刷新把它刷屏。
        if (!g_hz_logged) {
            g_hz_logged = true;
            if (ed->valid && ed->refresh_x10) {
                char rb[16];
                edid64_refresh_str64(rb, (int)sizeof(rb));
                dbg64_str("[UI] settings display hz=");
                dbg64_str(rb);
                dbg64_str("Hz source=edid preferred-timing");
                dbg64_nl();
            } else {
                logln("[UI] settings display hz=unknown source=none (no EDID from firmware)");
            }
        }
    }

    // 3) 缩放：真设置（fb_set_zoom）
    ui_text(x0 + L->cx0, y0 + L->y_zoom - 26, zh ? "缩放" : "Zoom", C_TEXT);
    {
        Buf b; b_init(&b);
        b_int(&b, fb_get_zoom()); b_ch(&b, '%');
        b_str(&b, zh ? "  (当前)" : "  (current)");
        draw_field(fx, y0 + L->y_zoom, fw, fh, g_drop == DROP_ZOOM, b.b, C_TEXT, true);
    }

    // 4) 应用按钮（选中即生效；这里再应用一次，幂等）
    draw_button(x0 + L->btn_x, y0 + L->btn_y, L->btn_w, L->btn_h,
                zh ? "应用" : "Apply", true, true);

    // 5) 硬件实测信息行 + 操作结果（放不下就不画，绝不溢出窗口；消息行排在最后，
    //    与"应用"按钮之间不会重叠）
    {
        int y = y0 + L->y_info;
        int lh = 20;
        int maxy = y0 + L->ch - 4;
        if (y + lh <= maxy) {
            Buf b; b_init(&b);
            b_str(&b, zh ? "渲染分辨率 " : "render ");
            b_int(&b, fb_width()); b_str(&b, " x "); b_int(&b, fb_height());
            b_str(&b, zh ? "  物理 " : "   physical ");
            b_int(&b, fb_phys_width()); b_str(&b, " x "); b_int(&b, fb_phys_height());
            b_str(&b, zh ? "  缩放 " : "   zoom ");
            b_int(&b, fb_get_zoom()); b_ch(&b, '%');
            ui_text(x0 + L->cx0, y, b.b, C_DIM);
            y += lh;
        }
        // 显示器身份 + EDID 摘要（有 EDID 才写名字/尺寸/版本；没有就如实写未知）
        if (y + lh <= maxy) {
            const Edid64* ed = edid64_get();
            Buf b; b_init(&b);
            if (ed->valid) {
                b_str(&b, zh ? "显示器 " : "monitor ");
                b_str(&b, ed->name);
                if (ed->size_cm_w && ed->size_cm_h) {
                    b_str(&b, zh ? "  尺寸 " : "  size ");
                    b_int(&b, ed->size_cm_w); b_str(&b, "x"); b_int(&b, ed->size_cm_h);
                    b_str(&b, "cm");
                } else {
                    b_str(&b, zh ? "  尺寸未知" : "  size unknown");
                }
            } else {
                b_str(&b, zh ? "显示器 未知（固件没给 EDID）" : "monitor unknown (no EDID from firmware)");
            }
            ui_text(x0 + L->cx0, y, b.b, C_DIM);
            y += lh;
        }
        if (y + lh <= maxy) {
            const Edid64* ed = edid64_get();
            Buf b; b_init(&b);
            if (ed->valid) {
                b_str(&b, "EDID ");
                b_int(&b, ed->version_major); b_ch(&b, '.'); b_int(&b, ed->version_minor);
                b_str(&b, zh ? "  厂商 " : "  mfg ");
                b_str(&b, ed->mfg);
                b_str(&b, zh ? "  首选 " : "  preferred ");
                b_int(&b, ed->h_active); b_ch(&b, 'x'); b_int(&b, ed->v_active);
                b_ch(&b, '@');
                char rb[16];
                edid64_refresh_str64(rb, (int)sizeof(rb));
                b_str(&b, rb); b_str(&b, "Hz  pclk=");
                b_int(&b, (int)ed->pclk_khz); b_str(&b, "kHz");
            } else {
                b_str(&b, zh ? "EDID 无（刷新率/显示器名不可知）"
                             : "no EDID (refresh rate / monitor name unknown)");
            }
            ui_text(x0 + L->cx0, y, b.b, C_DIM);
            y += lh;
        }
        if (y + lh <= maxy) {
            ui_text(x0 + L->cx0, y,
                    zh ? "切换走 Bochs VBE DISPI（QEMU 可切；VMware/真机可能拒绝）"
                       : "mode switch via Bochs VBE DISPI (QEMU ok; VMware/real HW may refuse)",
                    C_DIM);
            y += lh;
        }
        if (y + lh <= maxy && msg_fresh()) {
            ui_text(x0 + L->cx0, y, g_msg, g_msg_col);
            y += lh;
        }
    }
}

// ==================== 页面：系统（全部实测数据） ====================
static void draw_row(int lx, int lx_val, int y, const char* label, const char* value) {
    ui_text(lx, y, label, C_TEXT);
    ui_text(lx_val, y, value, C_DIM);
}

static void draw_system(const Lay* L, int x0, int y0) {
    bool zh = gui64_lang_zh();
    draw_page_title(L, x0, y0, zh ? "系统" : "System");
    ui_text(x0 + L->cx0, y0 + L->y_hdr,
            zh ? "设备规格（全部为主机实测值）" : "Device specifications (all measured)",
            C_ACCENT);

    uint64_t ram      = mem_total_ram_64();
    uint64_t h_used   = heap_used_64();
    uint64_t h_total  = heap_total_64();
    uint64_t pool_kb  = 0, pool_free_kb = 0, heap_kb = 0;
    mem_info_64(&pool_kb, &pool_free_kb, &heap_kb);

    char brand[52];
    if (!cpu_brand(brand, (int)sizeof(brand))) cpu_vendor(brand, (int)sizeof(brand));
    if (!brand[0]) {
        // 连厂商串都拿不到（极罕见）：如实写"未知"，不编造型号
        int i = 0; const char* u = "unknown";
        while (u[i]) { brand[i] = u[i]; i++; }
        brand[i] = 0;
    }

    int lx = x0 + L->cx0;
    int lxv = lx + 190;
    int y = y0 + L->y_rows;
    const int pitch = 24;

    {
        Buf b; b_init(&b);
        b_str(&b, brand);
        draw_row(lx, lxv, y, zh ? "处理器" : "Processor", b.b);
        y += pitch;
    }
    draw_row(lx, lxv, y, zh ? "架构" : "Architecture",
             "x86-64 (long mode, 64-bit)");   // 编译目标 x86_64-elf + 长模式引导链 = 事实
    y += pitch;
    {
        Buf b; b_init(&b);
        b_u64(&b, ram / (1024 * 1024));
        b_str(&b, zh ? " MB（E820 可用上界 " : " MB (E820 usable up to ");
        b_u64(&b, ram); b_str(&b, zh ? " 字节）" : " bytes)");
        draw_row(lx, lxv, y, zh ? "内存总量" : "Memory total", b.b);
        y += pitch;
    }
    {
        Buf b; b_init(&b);
        b_u64(&b, pool_free_kb); b_str(&b, zh ? " KB 空闲 / " : " KB free / ");
        b_u64(&b, pool_kb); b_str(&b, zh ? " KB 页池" : " KB page pool");
        draw_row(lx, lxv, y, zh ? "物理页池" : "Page pool", b.b);
        y += pitch;
    }
    {
        Buf b; b_init(&b);
        b_u64(&b, h_used / 1024); b_str(&b, " / ");
        b_u64(&b, h_total / 1024); b_str(&b, zh ? " KB（内核堆 " : " KB (kernel heap ");
        if (h_total) b_u64(&b, h_used * 100 / h_total); else b_u64(&b, 0);
        b_str(&b, "%）");
        draw_row(lx, lxv, y, zh ? "堆用量" : "Heap used", b.b);
        y += pitch;
    }
    {
        Buf b; b_init(&b);
        b_int(&b, fb_phys_width()); b_str(&b, " x "); b_int(&b, fb_phys_height());
        b_str(&b, " @32bpp");
        draw_row(lx, lxv, y, zh ? "物理分辨率" : "Physical mode", b.b);
        y += pitch;
    }
    {
        Buf b; b_init(&b);
        b_int(&b, fb_width()); b_str(&b, " x "); b_int(&b, fb_height());
        b_str(&b, zh ? "（= 物理 / 缩放）" : " (= physical / zoom)");
        draw_row(lx, lxv, y, zh ? "渲染分辨率" : "Render mode", b.b);
        y += pitch;
    }
    {
        Buf b; b_init(&b);
        b_int(&b, fb_get_zoom()); b_ch(&b, '%');
        b_str(&b, " (fb_get_zoom)");
        draw_row(lx, lxv, y, zh ? "缩放" : "Zoom", b.b);
        y += pitch;
    }
    {
        Buf b; b_init(&b);
        b_str(&b, "kernel LBA "); b_u64(&b, ML64_KERNEL_LBA);
        b_str(&b, "+"); b_u64(&b, ML64_KERNEL_SECTORS);
        b_str(&b, "  store LBA "); b_u64(&b, ML64_STORE_LBA);
        b_str(&b, "+"); b_u64(&b, ML64_STORE_SECTORS);
        draw_row(lx, lxv, y, zh ? "磁盘布局" : "Disk layout", b.b);
        y += pitch;
    }
    {
        Buf b; b_init(&b);
        b_u64(&b, ML64_IMAGE_SECTORS); b_str(&b, zh ? " 扇区 / " : " sectors / ");
        b_u64(&b, ML64_IMAGE_BYTES); b_str(&b, zh ? " 字节（整镜像）" : " bytes (image)");
        draw_row(lx, lxv, y, zh ? "镜像总量" : "Image size", b.b);
        y += pitch;
    }
    if (y + font_line_height() <= y0 + L->ch - 4) {
        ui_text(lx, y, zh ? "数据来源：E820 / fb 驱动 / 堆统计，全部为实测"
                          : "Sources: E820, fb driver, heap stats (all measured)",
                C_FAINT);
    }
}

// ==================== 页面：会话 / 应用 ====================
struct AppRow { int id; const char* zh; const char* en; };
static const AppRow kApps[] = {
    { APP_ID_TERM,     "终端",       "Terminal"     },
    { APP_ID_CALC,     "计算器",     "Calculator"   },
    { APP_ID_MINES,    "扫雷",       "Minesweeper"  },
    { APP_ID_TMGR,     "任务管理器", "Task Manager" },
    { APP_ID_SETTINGS, "设置",       "Settings"     },
};
#define APP_N ((int)(sizeof(kApps) / sizeof(kApps[0])))

static void draw_session(const Lay* L, int x0, int y0) {
    bool zh = gui64_lang_zh();
    draw_page_title(L, x0, y0, zh ? "会话 / 应用" : "Session / Apps");
    ui_text(x0 + L->cx0, y0 + L->y_hdr,
            zh ? "应用（数字 = 打开实例数；[x] = 关窗/停止时保留状态，点行可切换）"
               : "Apps (number = open instances; [x] = keep state on close/stop; click a row to toggle)",
            C_ACCENT);

    int lx = x0 + L->cx0;
    int y = y0 + L->y_rows;
    const int pitch = 24;
    static int g_sess_rows[APP_N];      // 每行的 y（命中共用，免得两处几何漂移）
    for (int i = 0; i < APP_N; i++) {
        g_sess_rows[i] = y;
        const int n = gui64_app_windows(kApps[i].id);
        const bool keep = session64_app_ok64(kApps[i].id) ? session64_app_keep64(kApps[i].id) : true;
        Buf b; b_init(&b);
        b_str(&b, keep ? "[x] " : "[ ] ");
        b_str(&b, zh ? kApps[i].zh : kApps[i].en);
        b_str(&b, "   ");
        b_u64(&b, (uint64_t)(n < 0 ? 0 : n));
        b_str(&b, zh ? " 个实例" : " instance(s)");
        ui_text(lx, y, b.b, keep ? C_TEXT : C_DIM);
        y += pitch;
    }

    ui_text(x0 + L->cx0, y0 + L->y_note,
            zh ? "策略（真持久化：落 store64 的 /store.a|b，3 秒内自动落盘）"
               : "Policy (really persisted: store64 /store.a|b, autosaved within 3s)",
            C_DIM);
    // 策略切换按钮：画在 y_note2 那一行（点击 = 切换 VOLATILE/PERSIST）
    const bool persist = (session64_mode64() == SESS64_PERSIST);
    draw_button(x0 + L->cx0, y0 + L->y_note2, 320, 26,
                persist ? (zh ? "策略：保存（PERSIST）—— 点击切换" : "Policy: PERSIST (click to switch)")
                        : (zh ? "策略：不保存（VOLATILE）—— 点击切换" : "Policy: VOLATILE (click to switch)"),
                persist, true);

    draw_button(x0 + L->cx0, y0 + L->y_act, 260, 30,
                zh ? "关闭全部应用窗口（清状态）" : "Close all app windows",
                false, true);

    if (y0 + L->y_foot + font_line_height() <= y0 + L->ch - 4) {
        ui_text(lx, y0 + L->y_foot,
                zh ? "VOLATILE：关窗即清该应用状态；PERSIST：不清状态，且下次启动按会话列表恢复应用。"
                   : "VOLATILE: closing a window drops that app's state. PERSIST: state kept and apps reopened at boot.",
                C_FAINT);
    }
}

// ==================== 页面：关于 ====================
static void draw_about(const Lay* L, int x0, int y0) {
    bool zh = gui64_lang_zh();
    draw_page_title(L, x0, y0, zh ? "关于" : "About");
    int lx = x0 + L->cx0;
    int y = y0 + L->y_rows - 24;
    const int pitch = 24;

    ui_text(lx, y, "Vimtu64 v2.0.1  (x86-64)", C_TEXT); y += pitch;
    ui_text(lx, y, zh ? "手写 64 位 x86-64 操作系统（长模式，无 32 位兼容层）"
                      : "Hand-written 64-bit x86-64 OS (long mode)",
            C_DIM); y += pitch;

    {
        Buf b; b_init(&b);
        b_str(&b, zh ? "构建：clang++ -target x86_64-elf，freestanding，C++17，-mno-sse"
                     : "Build: clang++ -target x86_64-elf, freestanding, C++17, -mno-sse");
        ui_text(lx, y, b.b, C_DIM); y += pitch;
    }
    {
        Buf b; b_init(&b);
        b_str(&b, zh ? "内核基址：KERNEL_BASE = " : "Kernel base: KERNEL_BASE = ");
        b_hex(&b, ML64_KERNEL_BASE, 6);
        b_str(&b, zh ? "，前 4GB 恒等映射 " : ", identity map ");
        b_hex(&b, ML64_IDENTITY_BYTES, 8);
        ui_text(lx, y, b.b, C_DIM); y += pitch;
    }
    {
        Buf b; b_init(&b);
        b_str(&b, "PML4 "); b_hex(&b, ML64_PML4_PHYS, 5);
        b_str(&b, "  PDPT "); b_hex(&b, ML64_PDPT_PHYS, 5);
        b_str(&b, "  PD0 ");  b_hex(&b, ML64_PD0_PHYS, 5);
        ui_text(lx, y, b.b, C_DIM); y += pitch;
    }
    {
        Buf b; b_init(&b);
        b_str(&b, zh ? "内核区 LBA " : "kernel LBA ");
        b_u64(&b, ML64_KERNEL_LBA); b_str(&b, "+"); b_u64(&b, ML64_KERNEL_SECTORS);
        b_str(&b, zh ? "（" : " (");
        b_u64(&b, ML64_KERNEL_MAX_BYTES); b_str(&b, zh ? " 字节）" : " bytes)");
        ui_text(lx, y, b.b, C_DIM); y += pitch;
    }
    {
        Buf b; b_init(&b);
        b_str(&b, zh ? "持久化区 LBA " : "store LBA ");
        b_u64(&b, ML64_STORE_LBA); b_str(&b, "+"); b_u64(&b, ML64_STORE_SECTORS);
        b_str(&b, zh ? "（store 兜底槽区；有 VFS 时优先 /store.a、/store.b）"
                     : " (store fallback slots; VFS /store.a,/store.b preferred)");
        ui_text(lx, y, b.b, C_DIM); y += pitch;
    }
    {
        Buf b; b_init(&b);
        b_str(&b, zh ? "镜像总量 " : "image ");
        b_u64(&b, ML64_IMAGE_SECTORS); b_str(&b, zh ? " 扇区 / " : " sectors / ");
        b_u64(&b, ML64_IMAGE_BYTES); b_str(&b, zh ? " 字节，扇区 " : " bytes, sector ");
        b_u64(&b, ML64_SECTOR_BYTES); b_str(&b, zh ? " 字节" : " bytes");
        ui_text(lx, y, b.b, C_DIM); y += pitch;
    }
    {
        Buf b; b_init(&b);
        b_str(&b, zh ? "内核堆 " : "kernel heap ");
        b_u64(&b, heap_used_64() / 1024); b_str(&b, " / ");
        b_u64(&b, heap_total_64() / 1024); b_str(&b, zh ? " KB，缩放 " : " KB, zoom ");
        b_int(&b, fb_get_zoom()); b_ch(&b, '%');
        ui_text(lx, y, b.b, C_DIM); y += pitch;
    }
    if (y + font_line_height() <= y0 + L->ch - 4) {
        ui_text(lx, y, zh ? "测试阶段；已实现：调度器/多任务、VimtuFS2 文件系统、store 持久化（/store.a、/store.b）、EDID 显示层、hwinfo、ACPI 解析、ring3 + int 0x80 + syscall 指令、VAP64/ELF64 应用。"
                          : "Testing phase; implemented: scheduler/multitasking, VimtuFS2, store persistence (/store.a, /store.b), EDID display layer, hwinfo, ACPI parsing, ring3 + int 0x80 + syscall insn, VAP64/ELF64 apps.",
                C_FAINT);
    }
}

// ==================== 导航栏 + 语言切换 ====================
static const char* kNavZh[PG_COUNT] = { "显示", "系统", "会话", "关于" };
static const char* kNavEn[PG_COUNT] = { "Display", "System", "Session", "About" };
static const char* kPageName[PG_COUNT] = { "display", "system", "session", "about" };

static void draw_nav(const Lay* L, int x0, int y0) {
    bool zh = gui64_lang_zh();
    fb_fill_rect(x0, y0, L->nav_w, L->ch, C_NAVBG);
    for (int i = 0; i < PG_COUNT; i++) {
        int ny = y0 + 14 + i * L->nav_pitch;
        if (i == g_page) {
            fb_fill_rect(x0, ny, L->nav_w, L->nav_hh, C_SEL);
            fb_fill_rect(x0, ny, 4, L->nav_hh, C_ACCENT);
        }
        ui_text(x0 + 18, ny + (L->nav_hh - font_line_height()) / 2,
                zh ? kNavZh[i] : kNavEn[i], (i == g_page) ? C_ACCENT : C_TEXT);
    }
    fb_draw_vline(x0 + L->nav_w - 1, y0, L->ch, C_SEP);

    // 底部：界面语言（中/英）——常驻可见，切了本页全部文案与导航立即跟随
    int ly = y0 + L->lang_y;
    if (L->lang_y > L->nav_hh + 14 + (PG_COUNT - 1) * L->nav_pitch + 10)
        ui_text(x0 + 12, ly - 20, zh ? "语言" : "Language", C_FAINT);
    const char* l1 = "中文";
    const char* l2 = (L->nav_w >= 150) ? "English" : "EN";
    fb_fill_rect(x0 + L->lang_x1, ly, L->lang_w, L->lang_h, zh ? C_SEL : 0xFFF2F2F2);
    fb_draw_rect(x0 + L->lang_x1, ly, L->lang_w, L->lang_h, zh ? C_ACCENT : C_BORDER);
    ui_text(x0 + L->lang_x1 + centered_x(L->lang_w, l1),
            ly + (L->lang_h - font_line_height()) / 2, l1, zh ? C_ACCENT : C_DIM);

    fb_fill_rect(x0 + L->lang_x2, ly, L->lang_w, L->lang_h, zh ? 0xFFF2F2F2 : C_SEL);
    fb_draw_rect(x0 + L->lang_x2, ly, L->lang_w, L->lang_h, zh ? C_BORDER : C_ACCENT);
    ui_text(x0 + L->lang_x2 + centered_x(L->lang_w, l2),
            ly + (L->lang_h - font_line_height()) / 2, l2, zh ? C_DIM : C_ACCENT);
}

// ==================== 主绘制 ====================
static void set_draw(Window* w) {
    int cw = win_cw(w), ch = win_ch(w);
    int x0 = w->client_x, y0 = w->client_y;
    Lay L;
    lay_calc(cw, ch, &L);

    // 布局可观测：客户区尺寸变化时写一行（每帧写会刷屏），自动化验收用
    if (cw != g_prev_cw || ch != g_prev_ch) {
        g_prev_cw = cw;
        g_prev_ch = ch;
        dbg64_str("[UI] settings layout client=");
        dbg64_dec((uint64_t)cw); dbg64_str("x"); dbg64_dec((uint64_t)ch);
        dbg64_str(" nav="); dbg64_dec((uint64_t)L.nav_w);
        dbg64_str(" field="); dbg64_dec((uint64_t)L.fw);
        dbg64_nl();
    }

    // 背景：左导航白底 + 右内容浅灰底
    fb_fill_rect(x0, y0, cw, ch, C_PAGEBG);
    draw_nav(&L, x0, y0);
    fb_fill_rect(x0 + L.nav_w, y0, cw - L.nav_w, ch, C_PAGEBG);

    switch (g_page) {
        case PG_DISPLAY: draw_display(w, &L, x0, y0); break;
        case PG_SYSTEM:  draw_system(&L, x0, y0);     break;
        case PG_SESSION: draw_session(&L, x0, y0);    break;
        default:         draw_about(&L, x0, y0);      break;
    }

    draw_dropdown(&L, x0, y0);   // 下拉最后画（覆盖内容）
}

// ==================== 交互：鼠标 ====================
static void drop_open(const Lay* L, int which) {
    g_drop = which;
    g_drop_top = 0;
    g_drop_hover = (which == DROP_RES) ? g_res_sel : g_zoom_sel;
    int n = drop_count();
    int fy = drop_field_y(L);
    drop_ensure_visible(L, fy, n);
    dbg64_str("[UI] settings dropdown open=");
    dbg64_str(which == DROP_RES ? "res" : "zoom");
    dbg64_str(" items=");
    dbg64_dec((uint64_t)n);
    dbg64_nl();
}

static void set_click(Window* w, int cx, int cy) {
    int cw = win_cw(w), ch = win_ch(w);
    Lay L;
    lay_calc(cw, ch, &L);

    // ---- 左导航（含底部语言切换）----
    if (cx < L.nav_w) {
        if (hit(L.lang_x1, L.lang_y, L.lang_w, L.lang_h, cx, cy)) {
            if (!gui64_lang_zh()) lang_set(true);
            return;
        }
        if (hit(L.lang_x2, L.lang_y, L.lang_w, L.lang_h, cx, cy)) {
            if (gui64_lang_zh()) lang_set(false);
            return;
        }
        if (cy >= 14) {
            int idx = (cy - 14) / L.nav_pitch;
            if (idx >= 0 && idx < PG_COUNT) {
                g_page = idx;
                g_drop = DROP_NONE;
                g_drop_hover = -1;
                dbg64_str("[UI] settings page=");
                dbg64_dec((uint64_t)g_page);
                dbg64_str(" (");
                dbg64_str(kPageName[g_page]);
                dbg64_str(")");
                dbg64_nl();
                gui64_invalidate();
            }
        }
        return;
    }

    // ---- 显示页字段 ----
    if (g_page == PG_DISPLAY) {
        bool in_x = (cx >= L.fx && cx < L.fx + L.fw);
        if (in_x && hit(L.fx, L.y_res, L.fw, L.fh, cx, cy)) {
            if (g_drop == DROP_RES) { g_drop = DROP_NONE; gui64_invalidate(); }
            else drop_open(&L, DROP_RES);
            gui64_invalidate();
            return;
        }
        if (in_x && hit(L.fx, L.y_hz, L.fw, L.fh, cx, cy)) {
            // 刷新率：真值来自 EDID 首选时序，只读（64 位没有 CRTC/0x3DA 那条控制路径）。
            // 有值时如实报出数字来源，没有时如实说"未知（无 EDID）"—— 两种情况都只打日志。
            const Edid64* ed = edid64_get();
            if (ed->valid && ed->refresh_x10) {
                char rb[16];
                edid64_refresh_str64(rb, (int)sizeof(rb));
                dbg64_str("[UI] settings hz from-edid refresh=");
                dbg64_str(rb);
                dbg64_str(" (read-only)");
                dbg64_nl();
                msg_set(gui64_lang_zh() ? "刷新率来自显示器 EDID 首选时序（只读，不可改）"
                                        : "refresh rate from EDID preferred timing (read-only)",
                        C_DIM);
            } else {
                logln("[UI] settings hz unknown (no EDID from firmware)");
                msg_set(gui64_lang_zh() ? "刷新率未知：固件没给 EDID（不编造数字）"
                                        : "refresh rate unknown: no EDID from firmware",
                        C_WARN);
            }
            g_drop = DROP_NONE;
            gui64_invalidate();
            return;
        }
        if (in_x && hit(L.fx, L.y_zoom, L.fw, L.fh, cx, cy)) {
            if (g_drop == DROP_ZOOM) { g_drop = DROP_NONE; gui64_invalidate(); }
            else drop_open(&L, DROP_ZOOM);
            gui64_invalidate();
            return;
        }
        if (hit(L.btn_x, L.btn_y, L.btn_w, L.btn_h, cx, cy)) {
            do_apply(w);
            return;
        }
    }

    // ---- 会话页：策略切换 / 应用 keep 开关 / 关闭全部应用 ----
    if (g_page == PG_SESSION) {
        // 策略切换按钮（画在 y_note2 那一行）
        if (hit(L.cx0, L.y_note2, 320, 26, cx, cy)) {
            const bool persist = (session64_mode64() == SESS64_PERSIST);
            session64_set_mode64(persist ? SESS64_VOLATILE : SESS64_PERSIST);
            dbg64_str("[UI] settings session policy=");
            dbg64_str(session64_mode_name64(session64_mode64()));
            dbg64_nl();
            msg_set(gui64_lang_zh()
                        ? (persist ? "策略已切为 VOLATILE（关窗即清状态）"
                                   : "策略已切为 PERSIST（保留状态，下次启动恢复应用）")
                        : (persist ? "policy = VOLATILE (state dropped on close)"
                                   : "policy = PERSIST (state kept, apps reopened at boot)"),
                    C_OK);
            gui64_invalidate();
            return;
        }
        // 应用行：点一下切换该应用的 keep 开关
        for (int i = 0; i < APP_N; i++) {
            const int ry = L.y_rows + i * 24;
            if (cy >= ry - 2 && cy < ry + 22 && cx >= L.cx0 - 2 && cx < L.cx0 + 420) {
                const bool keep = session64_app_keep64(kApps[i].id);
                session64_set_app_keep64(kApps[i].id, !keep);
                msg_set(gui64_lang_zh() ? "已切换该应用的保留状态（写入 config64/store64）"
                                        : "keep flag toggled (written to config64/store64)", C_OK);
                gui64_invalidate();
                return;
            }
        }
        if (hit(L.cx0, L.y_act, 260, 30, cx, cy)) {
            int n = 0;
            static const int ids[] = { APP_ID_CALC, APP_ID_MINES, APP_ID_TERM, APP_ID_TMGR,
                                       APP_ID_MONITOR, APP_ID_MYPC, APP_ID_RECYCLE, APP_ID_ABOUT };
            for (int i = 0; i < (int)(sizeof(ids) / sizeof(ids[0])); i++) {
                n += gui64_close_app(ids[i]);      // 真动作：窗口关掉 -> session64 按策略清状态
            }
            dbg64_str("[UI] settings session close-apps n=");
            dbg64_dec((uint64_t)n);
            dbg64_nl();
            msg_set(gui64_lang_zh() ? "已关闭其它应用窗口（VOLATILE 下状态已清）"
                                    : "other app windows closed (state cleared under VOLATILE)", C_OK);
            gui64_invalidate();
            return;
        }
    }

    // ---- 展开的下拉：命中列表项则应用，否则收起 ----
    if (g_drop != DROP_NONE) {
        int n = drop_count();
        int fy = drop_field_y(&L);
        ListBox lb;
        list_box(&L, fy, n, &lb);
        if (n > 0 && cx >= lb.x && cx < lb.x + lb.w &&
            cy >= lb.y && cy < lb.y + lb.visible * lb.item_h) {
            int idx = lb.top + (cy - lb.y) / lb.item_h;
            if (idx >= 0 && idx < n) {
                if (g_drop == DROP_RES) apply_res(w, idx);
                else apply_zoom(w, idx);
                gui64_invalidate();
                return;
            }
        }
        g_drop = DROP_NONE;
        g_drop_hover = -1;
        gui64_invalidate();
    }
}

// ==================== 交互：键盘 ====================
// 关窗（Esc 或外壳关闭请求）：上报日志，把状态收干净。
static void set_close(Window* w) {
    if (!g_close_logged) { logln("[APP] settings closed"); g_close_logged = true; }
    g_win = nullptr;                 // 先断引用：销毁过程里若回调 reset 不会再报一次
    g_drop = DROP_NONE;
    g_drop_hover = -1;
    if (w) gui64_destroy_window(w);
}

static void set_key(Window* w, char c) {
    unsigned char cc = (unsigned char)c;
    if (cc == 0x1B) {                       // Esc：先收下拉，再关窗
        if (g_drop != DROP_NONE) { g_drop = DROP_NONE; g_drop_hover = -1; gui64_invalidate(); return; }
        set_close(w);
        return;
    }
    if (g_drop != DROP_NONE) {
        int n = drop_count();
        if (cc == 0xFD || cc == 0xFE) {     // 上/下：移动高亮
            if (g_drop_hover < 0) g_drop_hover = (g_drop == DROP_RES) ? g_res_sel : g_zoom_sel;
            if (cc == 0xFD) { if (g_drop_hover > 0) g_drop_hover--; }
            else { if (g_drop_hover < n - 1) g_drop_hover++; }
            Lay L;
            lay_calc(win_cw(w), win_ch(w), &L);
            drop_ensure_visible(&L, drop_field_y(&L), n);
            gui64_invalidate();
            return;
        }
        if (cc == '\n' || cc == '\r' || cc == ' ') {   // 回车/空格：应用高亮项
            int idx = (g_drop_hover >= 0) ? g_drop_hover
                                         : ((g_drop == DROP_RES) ? g_res_sel : g_zoom_sel);
            if (g_drop == DROP_RES) apply_res(w, idx);
            else apply_zoom(w, idx);
            gui64_invalidate();
            return;
        }
        g_drop = DROP_NONE;                 // 其它键：收起
        g_drop_hover = -1;
        gui64_invalidate();
        return;
    }
    // 无鼠标也能操作（也便于自动化）：左右方向键翻页 / 0-3 直选 / L 切语言
    if (cc == 0xFB || cc == 0xFC) {
        g_page = (g_page + (cc == 0xFC ? 1 : PG_COUNT - 1)) % PG_COUNT;
        dbg64_str("[UI] settings page=");
        dbg64_dec((uint64_t)g_page);
        dbg64_str(" ("); dbg64_str(kPageName[g_page]); dbg64_str(")");
        dbg64_nl();
        gui64_invalidate();
        return;
    }
    if (cc >= '0' && cc <= '3') {
        g_page = cc - '0';
        dbg64_str("[UI] settings page=");
        dbg64_dec((uint64_t)g_page);
        dbg64_str(" ("); dbg64_str(kPageName[g_page]); dbg64_str(")");
        dbg64_nl();
        gui64_invalidate();
        return;
    }
    if (cc == 'l' || cc == 'L') { lang_set(!gui64_lang_zh()); return; }
}

// ==================== 交互：tick（下拉高亮跟随鼠标 + 关窗兜底） ====================
static void set_tick(Window* w) {
    // 关窗日志兜底：外壳若只是打了 closing 标记（尚未销毁），这里补一条
    if (w->closing && !g_close_logged) {
        logln("[APP] settings closed");
        g_close_logged = true;
    }
    if (g_drop == DROP_NONE) return;
    Lay L;
    lay_calc(win_cw(w), win_ch(w), &L);
    int n = drop_count();
    int fy = drop_field_y(&L);
    ListBox lb;
    list_box(&L, fy, n, &lb);
    int mx = mouse_get_x() - w->client_x;
    int my = mouse_get_y() - w->client_y;
    int h = -1;
    if (hit(lb.x, lb.y, lb.w, lb.visible * lb.item_h, mx, my))
        h = lb.top + (my - lb.y) / lb.item_h;
    if (h != g_drop_hover) {
        g_drop_hover = h;
        gui64_invalidate_window(w);
    }
}

// ==================== 应用入口 ====================
void app_settings_open64() {
    logln("[APP] settings opened");

    // 单实例：已有窗口就只激活（不新建）
    if (g_win && gui64_window_alive(g_win)) {
        logln("[UI] settings single-instance activate");
        gui64_set_active(g_win);
        return;
    }
    // 上一条窗口是被"点 X / 任务栏关闭"带走的（外壳没给 on_close 钩子）：
    // 悬空指针用 gui64_window_alive 判定，并在这里补上关窗日志。
    if (g_win && !gui64_window_alive(g_win)) {
        if (!g_close_logged) logln("[APP] settings closed");
        g_win = nullptr;
    }
    g_close_logged = false;

    // 刷新下拉/字段状态为**当前实测值**
    g_page = PG_DISPLAY;
    g_drop = DROP_NONE;
    g_drop_hover = -1;
    g_drop_top = 0;
    g_msg[0] = 0;
    g_res_sel = res_sel_from_actual();
    g_zoom_sel = zoom_sel_from_actual();
    g_prev_cw = -1;
    g_prev_ch = -1;                     // 强制下一次 draw 打布局日志

    bool zh = gui64_lang_zh();
    int wx = (gui64_screen_w() - SET_W) / 2;
    int wy = (gui64_screen_h() - gui64_taskbar_h() - SET_H) / 2;
    if (wx < 0) wx = 0;
    if (wy < 0) wy = 0;

    g_win = gui64_create_window(zh ? "设置" : "Settings", wx, wy, SET_W, SET_H,
                                set_draw, set_key, set_click, APP_ID_SETTINGS);
    if (!g_win) {
        logln("[UI] settings create failed (no window slot)");
        return;
    }
    gui64_set_min_size(g_win, SET_MIN_W, SET_MIN_H);
    gui64_set_tick(g_win, set_tick);
    gui64_set_active(g_win);

    dbg64_str("[UI] settings dropdowns res=");
    dbg64_dec((uint64_t)SET_RES_N);
    dbg64_str(" zoom=");
    dbg64_dec((uint64_t)SET_ZOOM_N);
    dbg64_str(" hz=0 (no display layer) res_sel=");
    dbg64_dec((uint64_t)(g_res_sel < 0 ? 999 : g_res_sel));
    dbg64_str(" zoom_sel=");
    dbg64_dec((uint64_t)g_zoom_sel);
    dbg64_nl();
}

void app_settings_reset64() {
    // 外壳在"关窗即清状态"/重启时按策略调用 reset 钩子。这是本内核唯一能拿到
    // "窗口已关"的时机（gui64.h 的 Window 没有 on_close 字段）：窗口已不在活动窗口
    // 表里 → 如实补一条关窗日志；窗口还活着（策略性清状态）→ 保留引用，别误报关窗。
    bool alive = (g_win && gui64_window_alive(g_win));
    if (!alive && g_win && !g_close_logged) {
        logln("[APP] settings closed");
        g_close_logged = true;
    }
    if (!alive) g_win = nullptr;
    g_drop = DROP_NONE;
    g_drop_hover = -1;
    g_drop_top = 0;
    g_prev_cw = -1;
    g_prev_ch = -1;
    g_hz_logged = false;
    logln("[APP] settings reset");
}
