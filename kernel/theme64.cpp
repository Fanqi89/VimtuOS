// theme64.cpp - Token 真源的实现：主题表（白/暗/蓝白/粉白/粉绿/粉紫）+ 切换 + 持久化 + 减少动画
//
// 主题表就是数据；所有数值都从 theme64.h 的 token 宏派生（不要在这里再写死几何/模糊/时长）。
// 颜色用 rgb(r,g,b) 直接给（颜色本身是主题数据，允许写在这里；几何/动效不许）。
#include "theme64.h"
#include "config64.h"
#include "rust64.h"     // 主题真源方向：与 Rust 侧主题表交叉核对 + 同步当前主题
#include "gfx64.h"      // 热键 Ctrl+Shift+M 循环壁纸适应模式
#include "debug64.h"    // dbg64_*
#include "x86_64.h"     // ticks64（动画时间基）

// ==================== 主题表 ====================
// 每个主题一组颜色；几何（圆角/模糊/透明度/阴影/时长）全部来自 theme64.h 的 token。
// 非暗色主题的 dock_bg 是**固定默认色**（不跟随壁纸）；暗色主题的 dock_bg 是深灰半透（见 gui64 的玻璃叠加）。
static const Theme64Tokens kThemes[THEME64_THEME_COUNT] = {
    // ---- 0：白（默认）----
    {
        "white", 0,
        rgb(198, 202, 210),                 // win_frame（1px 玻璃边框）
        rgb(246, 247, 249),                 // title_bg（玻璃底）
        rgb(238, 240, 243),                 // title_bg_ina
        rgb(32, 32, 32),                    // title_txt
        rgb(240, 240, 240),                 // client_bg（内容卡片底色；老验收依赖 240,240,240）
        rgb(32, 32, 32),                    // text
        rgb(104, 108, 116),                 // text_dim
        rgb(0, 120, 215),                   // accent（Win11 蓝）
        rgb(16, 110, 190),                  // accent2
        rgb(0, 120, 215),                   // sel_bg（桌面图标选中）
        rgb(255, 255, 255),                 // sel_border
        rgb(32, 32, 32),                    // icon_txt（浅色壁纸上用深字）
        rgb(0, 120, 215),                   // sel_fill
        rgb(64, 160, 255),                  // sel_edge
        rgb(243, 243, 243),                 // dock_bg（固定默认色）
        rgb(255, 255, 255),                 // dock_border
        rgb(40, 44, 52),                    // dock_txt
        rgb(0, 120, 215),                   // dock_dot
        rgb(0, 120, 215),                   // dock_bar（跟随强调色）
        rgb(228, 232, 240),                 // dock_hover_bg
        rgb(224, 224, 224),                 // btn_bg
        rgb(206, 206, 206),                 // btn_bg_hover
        rgb(196, 43, 28),                   // btn_close
        rgb(232, 17, 35),                   // btn_close_hover
        rgb(255, 255, 255),                 // btn_glyph
        rgb(250, 251, 253),                 // wall_a
        rgb(226, 235, 246),                 // wall_b
        rgb(255, 255, 255),                 // wall_blob
        rgb(120, 170, 250),                 // grad_a
        rgb(0, 120, 215),                   // grad_b
        rgb(243, 244, 246),                 // desktop_base（留白底色）
        rgb(0, 0, 0),                       // shadow_near（alpha 见 token）
        rgb(0, 0, 0),                       // shadow_far
        rgb(255, 255, 255),                 // highlight
    },
    // ---- 1：暗 ----
    {
        "dark", 1,
        rgb(72, 74, 82),
        rgb(42, 44, 50),
        rgb(34, 36, 40),
        rgb(240, 240, 242),
        rgb(40, 42, 46),
        rgb(236, 236, 240),
        rgb(158, 160, 168),
        rgb(96, 165, 250),                  // accent
        rgb(59, 130, 246),
        rgb(56, 110, 200),
        rgb(220, 226, 236),
        rgb(238, 238, 242),
        rgb(56, 110, 200),
        rgb(120, 180, 255),
        rgb(32, 32, 36),                    // dock_bg（深灰半透）
        rgb(120, 124, 134),
        rgb(232, 234, 240),
        rgb(96, 165, 250),
        rgb(96, 165, 250),
        rgb(56, 58, 64),
        rgb(60, 60, 66),
        rgb(80, 80, 88),
        rgb(196, 43, 28),
        rgb(232, 17, 35),
        rgb(255, 255, 255),
        rgb(18, 20, 26),
        rgb(38, 44, 58),
        rgb(70, 104, 160),
        rgb(96, 165, 250),
        rgb(59, 130, 246),
        rgb(24, 25, 28),
        rgb(0, 0, 0),
        rgb(0, 0, 0),
        rgb(255, 255, 255),
    },
    // ---- 2：蓝白渐变 ----
    {
        "bluegrad", 0,
        rgb(186, 202, 224),
        rgb(240, 246, 253),
        rgb(230, 238, 248),
        rgb(24, 42, 68),
        rgb(240, 240, 240),
        rgb(24, 36, 56),
        rgb(86, 104, 132),
        rgb(37, 99, 235),
        rgb(29, 78, 216),
        rgb(37, 99, 235),
        rgb(255, 255, 255),
        rgb(24, 36, 56),
        rgb(37, 99, 235),
        rgb(90, 160, 250),
        rgb(238, 245, 255),
        rgb(255, 255, 255),
        rgb(28, 48, 82),
        rgb(37, 99, 235),
        rgb(37, 99, 235),
        rgb(218, 232, 252),
        rgb(224, 224, 224),
        rgb(206, 206, 206),
        rgb(196, 43, 28),
        rgb(232, 17, 35),
        rgb(255, 255, 255),
        rgb(219, 234, 254),                 // wall_a（蓝白渐变）
        rgb(140, 180, 246),                 // wall_b
        rgb(255, 255, 255),
        rgb(96, 165, 250),
        rgb(37, 99, 235),
        rgb(236, 243, 252),
        rgb(0, 0, 0), rgb(0, 0, 0), rgb(255, 255, 255),
    },
    // ---- 3：粉白渐变 ----
    {
        "pinkgrad", 0,
        rgb(224, 196, 214),
        rgb(253, 245, 250),
        rgb(246, 236, 243),
        rgb(60, 26, 46),
        rgb(240, 240, 240),
        rgb(52, 28, 44),
        rgb(128, 94, 114),
        rgb(219, 39, 119),
        rgb(190, 24, 93),
        rgb(219, 39, 119),
        rgb(255, 255, 255),
        rgb(52, 28, 44),
        rgb(219, 39, 119),
        rgb(244, 150, 195),
        rgb(255, 244, 250),
        rgb(255, 255, 255),
        rgb(72, 34, 56),
        rgb(219, 39, 119),
        rgb(219, 39, 119),
        rgb(252, 228, 242),
        rgb(224, 224, 224),
        rgb(206, 206, 206),
        rgb(196, 43, 28),
        rgb(232, 17, 35),
        rgb(255, 255, 255),
        rgb(255, 241, 248),                 // wall_a（粉白渐变）
        rgb(246, 178, 214),                 // wall_b
        rgb(255, 255, 255),
        rgb(244, 114, 182),
        rgb(219, 39, 119),
        rgb(252, 241, 248),
        rgb(0, 0, 0), rgb(0, 0, 0), rgb(255, 255, 255),
    },
    // ---- 4：自定义渐变 1：粉绿渐变 ----
    {
        "pinkgreen", 0,
        rgb(200, 214, 206),
        rgb(248, 250, 248),
        rgb(238, 242, 238),
        rgb(28, 52, 44),
        rgb(240, 240, 240),
        rgb(24, 44, 38),
        rgb(96, 120, 110),
        rgb(16, 160, 120),
        rgb(6, 128, 94),
        rgb(16, 160, 120),
        rgb(255, 255, 255),
        rgb(24, 44, 38),
        rgb(16, 160, 120),
        rgb(80, 200, 160),
        rgb(246, 252, 248),
        rgb(255, 255, 255),
        rgb(26, 58, 46),
        rgb(16, 160, 120),
        rgb(16, 160, 120),
        rgb(226, 246, 238),
        rgb(224, 224, 224),
        rgb(206, 206, 206),
        rgb(196, 43, 28),
        rgb(232, 17, 35),
        rgb(255, 255, 255),
        rgb(255, 232, 240),                 // wall_a（粉）
        rgb(196, 240, 214),                 // wall_b（绿）
        rgb(255, 255, 255),
        rgb(244, 114, 182),                 // grad_a（粉）
        rgb(16, 160, 120),                  // grad_b（绿）
        rgb(240, 248, 244),
        rgb(0, 0, 0), rgb(0, 0, 0), rgb(255, 255, 255),
    },
    // ---- 5：自定义渐变 2：粉紫渐变 ----
    {
        "pinkpurple", 0,
        rgb(212, 202, 226),
        rgb(250, 246, 253),
        rgb(242, 236, 248),
        rgb(46, 30, 66),
        rgb(240, 240, 240),
        rgb(40, 26, 58),
        rgb(112, 96, 132),
        rgb(147, 51, 234),                  // accent
        rgb(124, 34, 200),                  // accent2
        rgb(147, 51, 234),                  // sel_bg
        rgb(255, 255, 255),                 // sel_border
        rgb(40, 26, 58),                    // icon_txt
        rgb(147, 51, 234),                  // sel_fill
        rgb(190, 150, 250),                 // sel_edge
        rgb(250, 244, 255),                 // dock_bg
        rgb(255, 255, 255),
        rgb(48, 32, 70),
        rgb(147, 51, 234),
        rgb(147, 51, 234),
        rgb(240, 228, 252),
        rgb(224, 224, 224),
        rgb(206, 206, 206),
        rgb(196, 43, 28),
        rgb(232, 17, 35),
        rgb(255, 255, 255),
        rgb(255, 228, 244),                 // wall_a（粉）
        rgb(206, 196, 255),                 // wall_b（紫）
        rgb(255, 255, 255),
        rgb(236, 72, 153),                  // grad_a（粉）
        rgb(139, 92, 246),                  // grad_b（紫）
        rgb(248, 244, 252),
        rgb(0, 0, 0), rgb(0, 0, 0), rgb(255, 255, 255),
    },
    // ---- 6：紫白渐变（用户参考图风格板；强调色 #7C4DFF；
    //          与 gui_rs/src/theme.rs 的第 6 个主题同名，含义一致）----
    {
        "purplegrad", 0,
        rgb(206, 196, 226),                 // win_frame
        rgb(248, 245, 255),                 // title_bg
        rgb(240, 236, 250),                 // title_bg_ina
        rgb(40, 30, 62),                    // title_txt
        rgb(240, 240, 240),                 // client_bg（老验收依赖 240,240,240）
        rgb(36, 28, 56),                    // text
        rgb(120, 104, 146),                 // text_dim
        rgb(124, 77, 255),                  // accent = #7C4DFF
        rgb(105, 60, 220),                  // accent2
        rgb(124, 77, 255),                  // sel_bg
        rgb(255, 255, 255),                 // sel_border
        rgb(36, 28, 56),                    // icon_txt
        rgb(124, 77, 255),                  // sel_fill
        rgb(178, 150, 255),                 // sel_edge
        rgb(246, 243, 255),                 // dock_bg（非暗色：固定默认色偏紫）
        rgb(255, 255, 255),                 // dock_border
        rgb(46, 34, 70),                    // dock_txt
        rgb(124, 77, 255),                  // dock_dot
        rgb(124, 77, 255),                  // dock_bar
        rgb(233, 226, 252),                 // dock_hover_bg
        rgb(224, 224, 224),                 // btn_bg
        rgb(206, 206, 206),                 // btn_bg_hover
        rgb(196, 43, 28),                   // btn_close
        rgb(232, 17, 35),                   // btn_close_hover
        rgb(255, 255, 255),                 // btn_glyph
        rgb(232, 224, 250),                 // wall_a（紫）
        rgb(255, 255, 255),                 // wall_b（白）
        rgb(255, 255, 255),                 // wall_blob
        rgb(168, 128, 255),                 // grad_a（紫）
        rgb(255, 255, 255),                 // grad_b（白）
        rgb(248, 245, 255),                 // desktop_base
        rgb(0, 0, 0), rgb(0, 0, 0), rgb(255, 255, 255),
    },
};

// ==================== 状态 ====================
static int  g_theme_id = THEME64_ID_WHITE;
static int  g_reduce_motion = 0;
static bool g_inited = false;
static int  g_applied_theme = -1;          // 上一次 tick 看到的 config 值（外部改动检测）
static int  g_applied_reduce = -1;
static int  g_log_apply = 0;               // apply 打点上限（防刷屏）

static const Theme64Tokens* theme_of(int id) {
    if (id < 0 || id >= THEME64_THEME_COUNT) id = THEME64_ID_WHITE;
    return &kThemes[id];
}

// 前置声明（theme64_init64 在 log_apply 定义之前就要打 apply 行）
static void log_apply(const char* why);

// ==================== 打点小工具 ====================
static void log_hex_color(uint32_t c) {
    static const char* H = "0123456789ABCDEF";
    char b[8];
    b[0] = '#';
    b[1] = H[(c >> 20) & 0xF]; b[2] = H[(c >> 16) & 0xF];
    b[3] = H[(c >> 12) & 0xF]; b[4] = H[(c >> 8) & 0xF];
    b[5] = H[(c >> 4) & 0xF];  b[6] = H[c & 0xF];
    b[7] = 0;
    dbg64_str(b);
}

// ==================== 与 Rust 侧主题表的交叉核对（主题真源方向）====================
// 本批：Rust 侧 gui_rs/src/theme.rs 是“主题表”的方向性真源（`rust64_theme_count64/name64/colors64/set64/current64`）。
// 但 Rust 表目前只有 11 个颜色（window/card/text/text_dim/accent/dock/border_hi/shadow/grad_start/grad_end/is_dark），
// 而外壳绘制需要 34 个（标题栏玻璃/三按钮/选中色/壁纸渐变/桌面底色/双层阴影等），
// 所以本批先做“双表 + 交叉核对 + 同步当前主题”（数量必须一致，名字/accent 逐条对比打点），
// 后续把外壳绘制切到 Rust 表需先给 Rust 表补齐缺失字段（见报告“后续计划”）。
static int g_rust_theme_count = -1;

void theme64_rust_crosscheck64() {
    const uint32_t n = rust64_theme_count64();
    g_rust_theme_count = (int)n;
    int count_ok = (n == (uint32_t)THEME64_THEME_COUNT) ? 1 : 0;
    dbg64_line_begin64();
    dbg64_str("[THEME64] rust cross-check count=");
    dbg64_dec((uint64_t)n);
    dbg64_str(" cpp=");
    dbg64_dec((uint64_t)THEME64_THEME_COUNT);
    dbg64_str(" count_ok=");
    dbg64_dec((uint64_t)count_ok);
    dbg64_str(" current=");
    dbg64_dec((uint64_t)rust64_theme_current64());
    dbg64_nl();
    dbg64_line_end64();
    // 逐主题：名字长度 + accent 对比（一行一个，上限 8 个主题，防刷屏）
    const int lim = (int)n < THEME64_THEME_COUNT ? (int)n : THEME64_THEME_COUNT;
    for (int i = 0; i < lim && i < 8; i++) {
        Rust64ThemeColors rc{};
        uint8_t nm[64];
        const uint32_t nlen = rust64_theme_name64((uint32_t)i, nm, (uint32_t)sizeof(nm));
        const uint32_t rc_ok = rust64_theme_colors64((uint32_t)i, &rc);
        const uint32_t rust_accent = (rc_ok == 0)
            ? (((uint32_t)rc.accent.r << 16) | ((uint32_t)rc.accent.g << 8) | rc.accent.b)
            : 0xFFFFFFFFu;
        const uint32_t cpp_accent = kThemes[i].accent & 0xFFFFFFu;
        dbg64_line_begin64();
        dbg64_str("[THEME64] theme idx=");
        dbg64_dec((uint64_t)i);
        dbg64_str(" cpp=");
        dbg64_str(kThemes[i].name);
        dbg64_str(" accent=");
        log_hex_color(cpp_accent);
        dbg64_str(" rust=");
        if (nlen == 0 || nlen >= 60) {
            dbg64_str("?");
        } else {
            nm[nlen] = 0;
            dbg64_str((const char*)nm);
        }
        dbg64_str(" rust_accent=");
        if (rust_accent == 0xFFFFFFFFu) dbg64_str("?");
        else log_hex_color(rust_accent);
        dbg64_str(" accent_same=");
        dbg64_dec((uint64_t)(rust_accent == cpp_accent ? 1 : 0));
        dbg64_str(" rust_dark=");
        dbg64_dec((uint64_t)(rc_ok == 0 ? (rc.is_dark ? 1 : 0) : 0));
        dbg64_str(" cpp_dark=");
        dbg64_dec((uint64_t)(kThemes[i].dark ? 1 : 0));
        dbg64_nl();
        dbg64_line_end64();
    }
}

// ==================== API ====================
const Theme64Tokens* theme64_tokens64() { return theme_of(g_theme_id); }
int  theme64_id64() { return g_theme_id; }
int  theme64_is_dark64() { return theme_of(g_theme_id)->dark ? 1 : 0; }
int  theme64_reduce_motion64() { return g_reduce_motion ? 1 : 0; }
const char* theme64_name64(int id) {
    if (id < 0 || id >= THEME64_THEME_COUNT) return "?";
    return kThemes[id].name;
}

void theme64_init64() {
    if (g_inited) return;
    g_inited = true;
    int id = cfg64_theme64();
    if (id < 0 || id >= THEME64_THEME_COUNT) id = THEME64_ID_WHITE;
    g_theme_id = id;
    g_reduce_motion = cfg64_reduce_motion64() ? 1 : 0;
    g_applied_theme = id;
    g_applied_reduce = g_reduce_motion;
    dbg64_line_begin64();
    dbg64_str("[THEME64] init themes=");
    dbg64_dec((uint64_t)THEME64_THEME_COUNT);
    dbg64_str(" theme=");
    dbg64_dec((uint64_t)g_theme_id);
    dbg64_str(" name=");
    dbg64_str(theme_of(g_theme_id)->name);
    dbg64_str(" dark=");
    dbg64_dec((uint64_t)theme64_is_dark64());
    dbg64_str(" reduce_motion=");
    dbg64_dec((uint64_t)g_reduce_motion);
    dbg64_str(" r_win=");
    dbg64_dec((uint64_t)THEME64_R_WINDOW);
    dbg64_str(" r_card=");
    dbg64_dec((uint64_t)THEME64_R_CARD);
    dbg64_str(" r_btn=");
    dbg64_dec((uint64_t)THEME64_R_BUTTON);
    dbg64_str(" r_dock=");
    dbg64_dec((uint64_t)THEME64_R_DOCK);
    dbg64_str(" blur_bg=");
    dbg64_dec((uint64_t)THEME64_BLUR_BACKDROP);
    dbg64_str(" blur_content=");
    dbg64_dec((uint64_t)THEME64_BLUR_CONTENT);
    dbg64_str(" alpha_bg=");
    dbg64_dec((uint64_t)THEME64_A_BACKDROP);
    dbg64_str(" alpha_card=");
    dbg64_dec((uint64_t)THEME64_A_CARD);
    dbg64_str(" sh_near=0,");
    dbg64_dec((uint64_t)THEME64_SH_N_DY);
    dbg64_str(",");
    dbg64_dec((uint64_t)THEME64_SH_N_BLUR);
    dbg64_str(",");
    dbg64_dec((uint64_t)THEME64_SH_N_A);
    dbg64_str(" sh_far=0,");
    dbg64_dec((uint64_t)THEME64_SH_F_DY);
    dbg64_str(",");
    dbg64_dec((uint64_t)THEME64_SH_F_BLUR);
    dbg64_str(",");
    dbg64_dec((uint64_t)THEME64_SH_F_A);
    dbg64_str(" ms=fast:");
    dbg64_dec((uint64_t)THEME64_MS_FAST);
    dbg64_str(",normal:");
    dbg64_dec((uint64_t)THEME64_MS_NORMAL);
    dbg64_str(",large:");
    dbg64_dec((uint64_t)THEME64_MS_LARGE);
    dbg64_str(",dock:");
    dbg64_dec((uint64_t)THEME64_MS_DOCK);
    dbg64_str(" ease=cubic-bezier(0.2,0,0,1)");
    dbg64_nl();
    dbg64_line_end64();
    theme64_rust_crosscheck64();      // 与 Rust 侧主题表交叉核对（数量/名字/accent）
    log_apply("init");
}

// apply 打点：init 时必然打一行；运行期每次切换打一行（上限 24 行防刷屏）
static void log_apply(const char* why) {
    const Theme64Tokens* t = theme_of(g_theme_id);
    if (g_log_apply >= 24) return;
    g_log_apply++;
    dbg64_line_begin64();
    dbg64_str("[THEME64] apply theme=");
    dbg64_dec((uint64_t)g_theme_id);
    dbg64_str(" name=");
    dbg64_str(t->name);
    dbg64_str(" dark=");
    dbg64_dec((uint64_t)(t->dark ? 1 : 0));
    dbg64_str(" accent=");
    log_hex_color(t->accent);
    dbg64_str(" dock=");
    log_hex_color(t->dock_bg);
    dbg64_str(" wall=");
    log_hex_color(t->wall_a);
    dbg64_str("->");
    log_hex_color(t->wall_b);
    dbg64_str(" why=");
    dbg64_str(why ? why : "?");
    dbg64_nl();
    dbg64_line_end64();
    dbg64_line_begin64();
    dbg64_str("[THEME64] motion reduce=");
    dbg64_dec((uint64_t)g_reduce_motion);
    dbg64_str(" fast=");
    dbg64_dec((uint64_t)theme64_dur64(THEME64_MS_FAST));
    dbg64_str(" normal=");
    dbg64_dec((uint64_t)theme64_dur64(THEME64_MS_NORMAL));
    dbg64_str(" large=");
    dbg64_dec((uint64_t)theme64_dur64(THEME64_MS_LARGE));
    dbg64_str(" dock=");
    dbg64_dec((uint64_t)theme64_dur64(THEME64_MS_DOCK));
    dbg64_str("ms");
    dbg64_nl();
    dbg64_line_end64();
}

void theme64_set_theme64(int id, const char* why) {
    if (id < 0 || id >= THEME64_THEME_COUNT) return;
    const bool changed = (id != g_theme_id);
    g_theme_id = id;
    g_applied_theme = id;
    (void)rust64_theme_set64((uint32_t)id);      // 同步 Rust 侧“当前主题”（真源方向：一处切换两边跟上）
    cfg64_set_theme64(id);                 // 跨重启持久化（config64 -> store64，3 秒去抖落盘）
    if (changed) gfx64_wall_invalidate64();  // 壁纸/玻璃缓存按主题重建（渐变主题换壁纸）
    log_apply(why);
}

void theme64_cycle_theme64(const char* why) {
    theme64_set_theme64((g_theme_id + 1) % THEME64_THEME_COUNT, why);
}

void theme64_set_reduce_motion64(int on, const char* why) {
    g_reduce_motion = on ? 1 : 0;
    g_applied_reduce = g_reduce_motion;
    cfg64_set_reduce_motion64(g_reduce_motion);
    log_apply(why);
}

uint32_t theme64_dur64(int base_ms) {
    if (!g_reduce_motion) return (uint32_t)base_ms;
    return 0;      // 减少动画：0ms = 1 帧到位
}

int theme64_ease64(int t) {
    if (t <= 0) return 0;
    if (t >= 256) return 256;
    // ease-out 三次：e = 1 - (1-t)^3（定点 Q8；逼近 cubic-bezier(0.2,0,0,1)，见 token 注释）
    const int u = 256 - t;
    int e = 256 - (((u * u) >> 8) * u >> 8);
    if (e < 0) e = 0;
    if (e > 256) e = 256;
    return e;
}

// 弹簧回弹：16 点采样表（整数；>256 = 过冲）。x 轴 = 归一化时间 0..256。
static const int kSpring[17] = {
    0, 152, 246, 292, 300, 284, 262, 250, 243, 245, 250, 253, 255, 256, 256, 256, 256
};
int theme64_spring64(int t) {
    if (t <= 0) return 0;
    if (t >= 256) return 256;
    const int i = t * 16 / 256;
    const int frac = (t * 16) % 256;
    const int a = kSpring[i], b = kSpring[i + 1];
    return a + (b - a) * frac / 256;
}

int theme64_tick64() {
    // config64 里的值可能被外部改过（终端 `cfg set ui.theme 2` / `cfg set ui.reduce_motion 1`）→ 实时生效
    if (!g_inited) return 0;
    int changed = 0;
    const int th = cfg64_theme64();
    if (th != g_applied_theme && th >= 0 && th < THEME64_THEME_COUNT) {
        theme64_set_theme64(th, "cfg");
        changed = 1;
    }
    const int rm = cfg64_reduce_motion64() ? 1 : 0;
    if (rm != g_applied_reduce) {
        g_reduce_motion = rm;
        g_applied_reduce = rm;
        log_apply("cfg");
        changed = 1;
    }
    return changed;
}

int theme64_hotkey64(uint8_t c, int ctrl, int shift) {
    if (!ctrl) return 0;
    // 把三种形态归一成小写字母：
    //   1) Ctrl+字母：input.cpp 折成控制码（T=0x14 / N=0x0E / R=0x12）；
    //   2) Ctrl+Shift+字母：input.cpp 先用 Shift 表取到大写字母，再减 'a'+1 会**下溢**成
    //      0xF4/0xEE/0xF2 这样的值（实测；不改 input.cpp 以免影响别处，这里按字符表放行）；
    //   3) 直接送来 'T'/'t'（未来若 input.cpp 改了映射也仍然可用）。
    char letter = 0;
    switch (c) {
        case 0x14: case 'T': case 't': case 0xF4: letter = 't'; break;
        case 0x0E: case 'N': case 'n': case 0xEE: letter = 'n'; break;
        case 0x12: case 'R': case 'r': case 0xF2: letter = 'r'; break;
        default: break;
    }
    if (!letter) { (void)shift; return 0; }
    c = (uint8_t)letter;
    // 控制码：Ctrl+T=0x14（Shift 不影响字母控制码）；Ctrl+M=0x0D 与回车冲突，按 raw 不区分，
    // 所以壁纸模式用 Ctrl+Shift+N(0x0E) 更稳妥 —— 见下面注释。
    if (c == 't') {                        // Ctrl(+Shift)+T：循环主题
        theme64_cycle_theme64("hotkey ctrl+shift+t");
        return 1;
    }
    if (c == 'n') {                        // Ctrl(+Shift)+N：循环壁纸适应模式
        const int m = (gfx64_wall_mode64() + 1) % GFX64_WALL_MODE_COUNT;
        gfx64_wall_set_mode64(m, "hotkey ctrl+shift+n");
        return 1;
    }
    if (c == 'r') {                        // Ctrl(+Shift)+R：减少动画开关
        theme64_set_reduce_motion64(!g_reduce_motion, "hotkey ctrl+shift+r");
        return 1;
    }
    (void)shift;
    return 0;
}
