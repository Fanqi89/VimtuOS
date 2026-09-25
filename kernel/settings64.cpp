// settings64.cpp - VimtuOS 设置应用（★ P3：**Windows 11 风格**，左侧导航 240px + 右侧卡片内容）
//
// 与上一版（Win10 风格：左侧窄导航 190 + 字段区）的关系：**重做**。保留的东西与理由写在下面，
// 其余全部按"Win11 设置"重排：
//   * 单实例 / 入口 / 关闭日志（[APP] settings opened|closed|reset）—— 外壳契约与其他验收脚本依赖；
//   * 显示页的实测值口径（分辨率 = fb_phys_*、刷新率 = EDID 首选时序、缩放 = fb_get_zoom）；
//   * 显示页/规格页的两条既有断言行（格式逐字不变，见下面"打点"一节的 ★ 行）：
//       [UI] settings display hz=<x.x>Hz source=edid preferred-timing
//       [UI] settings specs ram=<n>MB cpu=<..> cores=<n> disk=<..> vga=framebuffer WxH@32bpp ...
//     （tests/display64_test.py 与 tests/tmgr_proc_test.py 按这两行的正则断言；首帧打一次）
//
// ==================== Win11 结构 ====================
//   左导航 240px（圆角/亚克力/选中高亮全部走 theme64.h 的 Token）：6 个分组标题 + 组内条目：
//     系统      ：显示 / 声音 / 电源 / 默认应用
//     个性化    ：主题 / 壁纸 / 颜色 / 壁纸适应模式 / Dock 栏 / 字体大小
//     网络      ：以太网 / Wi-Fi
//     用户      ：头像 / 用户名
//     安全      ：密码
//     关于      ：关于（版本 / 驱动 / GPU 加速状态 + 硬件检查报告）
//   右侧内容 = 卡片（亚克力 = Token 的卡片透明度 THEME64_A_CARD + 内容层模糊 THEME64_BLUR_CONTENT
//   + 卡片圆角 THEME64_R_CARD + 双层浅阴影 Token）；每项控件（开关/滑块/下拉/分段/按钮/输入框）
//   都有 hover / **按下态** / **选中态**（颜色全部来自当前主题 Token，本文件不写死任何视觉数字）。
//   切页动效：淡入 + 轻微位移，时长 = theme64_dur64(THEME64_MS_NORMAL)，缓动 = theme64_ease64()
//   （"减少动画"打开时时长退化为 0 = 一帧到位，Token 语义直接生效）。
//
// ==================== 实时生效 + 持久化 ====================
//   每个设置项都：立即改真值 -> 写 config64（-> store64 的 /store.a|b，3 秒内自动落盘）-> 打点。
//   主题/壁纸模式/Dock 走既有模块的 set API（theme64_set_theme64 / gfx64_wall_set_mode64 /
//   cfg64_set_dock_* + gui64_dock_reload64）；字体大小走 font_set_size64()（本批给 font 补的 API）。
//   ★ 启动期生效点：settings64_boot_apply64()（字体大小档 + 自定义渐变壁纸），由 gui64_run 调用。
//
// ==================== 串口打点（自动验收 grep；每类都有上限防刷屏）====================
//   [SET64] open w=.. h=.. client=WxH nav=240 pages=16
//   [SET64] layout client=WxH nav=.. card=.. row=..
//   [SET64] nav w=240 groups=6 items=16                   （左导航宽度 + 分组/条目数）
//   [SET64] nav group idx=.. name=.. y=..
//   [SET64] nav item page=.. grp=.. name=.. x=.. y=.. w=.. h=.. cy=..
//   [SET64] ctl page=.. id=.. kind=.. x=.. y=.. w=.. h=..（每个控件的可点矩形；自动化按它点击）
//   [SET64] page=<i> name=<key> anim=<ms>                 （切页 + 动效时长）
//   [SET64] theme id=.. name=.. reduce_motion=..          （另有 theme64 自己的 [THEME64] apply）
//   [SET64] wall mode=.. name=.. target=desktop|lock sync=0|1
//   [SET64] wall lag src=.. (lock_mode=<a> desktop_mode=<b>)  桌面/锁屏分别设置的状态行
//   [SET64] color a=#.. b=#.. grad=0|1 built=WxH
//   [SET64] dock len=.. auto=<0|1> icon=.. gap=.. size=.. applied=1   （另有 [DOCK64] geom 由外壳打）
//   [SET64] wall mode=.. name=.. target=desktop|lock sync=0|1 lock_mode=<a> desktop_mode=<b>
//   [SET64] default kind=<elf|vap|txt|video> app=.. persisted=1
//   [SET64] sound vol=.. src=.. applied=0 reason=audio driver not implemented yet
//   [SET64] eth link=.. state=.. mac=.. tx=.. rx=..
//   [SET64] wifi present=0 wireless=0 pci_net=1 reason=no wireless hardware
//   [USER64] avatar user=.. src=.. via=settings persisted=1        （头像；走 userdb64 落盘）
//   [USER64] rename ok from=.. to=.. via=settings persisted=1      （改名；重写 /etc/users.db）
//   [SET64] password user=.. set=0|1 algo=sha256 iter=1000 via=settings (plaintext never stored)
//   [SET64] about version=.. build_tag=.. bits=64
//   [SET64] drivers implemented=<n> missing=<n> gpu=software
//   [SET64] hwui lines=.. storage=.. controllers=..（关于页内嵌硬件检查报告时）
//   ★ [UI] settings layout client=WxH nav=.. field=.. / [UI] settings display hz=.. / specs ram=..
//   ★ [UI] settings mode WxH ok|fail / [UI] settings zoom <n> / [UI] settings dropdown open=res items=6
//   ★ [APP] settings opened / closed / reset
//
// 诚实边界（能做的做，做不到的如实写，绝不伪造）：
//   1) 分辨率切换真调 fb_set_mode()（Bochs VBE DISPI + 回读校验）；失败时页面与日志都写"失败"，
//      字段永远显示实测值。刷新率只读（EDID 首选时序；没有 CRTC/0x3DA 控制路径）。
//   2) 声音：**没有音频驱动**（HDA/AC97 都没有）—— 音量/输出源只进内存 + config64，
//      打点明写 "audio driver not implemented yet"，绝不假装生效。
//   3) Wi-Fi：**没有无线硬件**（PCI 扫描 net=1 wired=1 wireless=0）+ 没有 802.11 驱动 —— 如实写
//      "无无线硬件"，不编造 SSID 列表。
//   4) 头像：img64 只解 PNG/BMP（JPEG 未实现，选到 .jpg 就如实报错）。
//   5) 默认应用：本页把"类型 -> 应用"映射**持久化 + 打点**；系统目前没有"按扩展名打开"的关联引擎
//      （文件管理器/终端不会自动按它启动应用）—— 如实标注在页面上。
//   6) 用户名/密码/头像的入口都在这里（设置页是正路）；终端的 passwd/useradd/userdel 保留，视为
//      管理员工具（多用户/权限批次的需求），两者改的是同一份 /etc/users.db。
//
// 编译约束（内核 -mno-sse，零告警 -Wall -Wextra）：只用整数、无浮点、无标准头、不做堆分配
//   （唯一一次 kmalloc 是"自定义渐变壁纸"的像素缓冲，用完 gfx64 自己留副本后立刻 kfree）。
// 绘制约定：屏幕绝对坐标 = w->client_x/y + 客户区局部坐标。

#include "gui64.h"
#include "settings64.h"
#include "fb.h"
#include "font.h"
#include "input.h"
#include "memlayout64.h"     // ★ 必须排在 mem_64.h 之前：mem_64.h 的 #define PAGE_SIZE_64
#include "mem_64.h"          //   会顶掉 memlayout64.h 的同名 static const
#include "debug64.h"
#include "x86_64.h"
#include "config64.h"
#include "theme64.h"         // ★ 视觉 Token 唯一真源（圆角/模糊/阴影/动效/颜色全从这里取）
#include "gfx64.h"           // 圆角 + 阴影 + 毛玻璃 + 壁纸适应模式
#include "startmenu64.h"     // P2 UI 工具箱（混合圆角/亚克力/线性图标/文本）+ 开始菜单几何
#include "img64.h"           // PNG/BMP 解码（壁纸 / 头像）
#include "edid64.h"          // 显示器 EDID（刷新率/名字/尺寸）：只读
#include "display64.h"       // 运行期显示层（模式清单/实测刷新率来源）
#include "net64.h"           // e1000 状态 + 收发计数 + 静态 IP 配置
#include "e1000_64.h"        // e1000_link64()（实时链路状态）/ MAC
#include "panels64.h"        // 声音面板的同一份内存态（音量/输出源）+ 诚实打点口径
#include "locklogin64.h"     // 锁定 = locklogin64_lock64("settings")
#include "hwui64.h"          // 硬件检查报告（与启动期那一页同一份）
#include "rust64.h"          // rust64_build_tag64()：版本/构建标记的单一来源
#include "usb64.h"           // UHCI 控制器 / HID / USB 存储（只读）
#include "ahci64.h"          // AHCI 控制器/端口
#include "nvme64.h"          // NVMe 控制器/命名空间
#include "acpi64.h"          // RSDP/MADT/IOAPIC
#include "smp64.h"           // SMP 在线 CPU
#include "apic64.h"          // IRQ_MODE64_*（中断路由）
#include "hwinfo64.h"        // CPUID + PCI 枚举 + 磁盘
#include "userdb64.h"        // 用户表 / 加盐哈希 / /etc/users.db

// gui64.cpp 提供的 Dock 几何热重载（★ P3 新增的小接线：改完 Dock 长度/图标/间距立刻重算 + 再打
// 一行 [DOCK64] geom 作为几何变化的证据；不动外壳的 Dock 绘制逻辑）。
void gui64_dock_reload64();

// ==================== 尺寸 / 布局常量 ====================
#define SET_W       900          // 窗口整体尺寸（Win11 设置窗口更大，左导航 240 才放得下文字）
#define SET_H       620
#define SET_MIN_W   640
#define SET_MIN_H   420
#define NAV_W       240          // ★ 左导航宽度（需求原文 240px）
#define NAV_W_MIN   150          // 窗口拉到极窄时的下限
#define NAV_PAD_X   14
#define CTL_MAX     40           // 单页控件上限（超出不再登记，绘制照旧）
#define MSG_LEN     112
#define FONT_TILE_H 76           // 主题磁贴高

// 页面（导航顺序 = 分组顺序；页面 key 用于打点与自动化定位）
#define P_DISPLAY  0
#define P_SOUND    1
#define P_POWER    2
#define P_DEFAPP   3
#define P_THEME    4
#define P_WALL     5
#define P_COLOR    6
#define P_FIT      7
#define P_DOCK     8
#define P_FONT     9
#define P_ETH      10
#define P_WIFI     11
#define P_AVATAR   12
#define P_UNAME    13
#define P_PASSWD   14
#define P_ABOUT    15
#define P_COUNT    16

#define GRP_SYS    0
#define GRP_PERSON 1
#define GRP_NET    2
#define GRP_USER   3
#define GRP_SEC    4
#define GRP_ABOUT  5
#define GRP_COUNT  6

struct NavItem { unsigned char grp; const char* zh; const char* en; const char* key; };
static const NavItem kPages[P_COUNT] = {
    { GRP_SYS,    "显示",         "Display",        "display" },
    { GRP_SYS,    "声音",         "Sound",          "sound"   },
    { GRP_SYS,    "电源",         "Power",          "power"   },
    { GRP_SYS,    "默认应用",     "Default apps",   "defapp"  },
    { GRP_PERSON, "主题",         "Themes",         "theme"   },
    { GRP_PERSON, "壁纸",         "Wallpaper",      "wall"    },
    { GRP_PERSON, "颜色",         "Colors",         "color"   },
    { GRP_PERSON, "壁纸适应模式", "Wallpaper fit",  "fit"     },
    { GRP_PERSON, "Dock 栏",      "Dock",           "dock"    },
    { GRP_PERSON, "字体大小",     "Text size",      "font"    },
    { GRP_NET,    "以太网",       "Ethernet",       "eth"     },
    { GRP_NET,    "Wi-Fi",        "Wi-Fi",          "wifi"    },
    { GRP_USER,   "头像",         "Avatar",         "avatar"  },
    { GRP_USER,   "用户名",       "User name",      "uname"   },
    { GRP_SEC,    "密码",         "Password",       "passwd"  },
    { GRP_ABOUT,  "关于",         "About",          "about"   },
};
static const char* kGrpZh[GRP_COUNT] = { "系统", "个性化", "网络", "用户", "安全", "关于" };
static const char* kGrpEn[GRP_COUNT] = { "System", "Personalization", "Network", "Users", "Security", "About" };

// 控件 id（全局唯一；绘制与命中都用它）
enum {
    CID_THEME_TILE0 = 1,           // 1..7 主题磁贴
    CID_THEME_REDUCE = 20,
    CID_RES_DROP = 30, CID_ZOOM_100 = 31, CID_ZOOM_125 = 32, CID_ZOOM_150 = 33,
    CID_DISPLAY_APPLY = 34,
    CID_SND_VOL = 40, CID_SND_SPK = 41, CID_SND_HP = 42,
    CID_PWR_SHUTDOWN = 50, CID_PWR_REBOOT = 51, CID_PWR_LOCK = 52,
    CID_DEF_ROW = 300,             // 300..319 = 4 个类型行 × 5 个应用项（默认应用页）
    CID_WALL_FIELD = 70, CID_WALL_APPLY = 71, CID_WALL_BUILTIN = 72, CID_WALL_PRESET = 73,
    CID_GRAD_ON = 80, CID_GRAD_A_FIELD = 81, CID_GRAD_B_FIELD = 82,
    CID_GRAD_A_APPLY = 83, CID_GRAD_B_APPLY = 84, CID_GRAD_RESET = 85,
    CID_FIT_SYNC = 96,
    CID_WALLFIT_BASE = 90,         // 90..95 = 桌面 6 种适应模式
    CID_LOCKFIT_BASE = 100,        // 100..105 = 锁屏 6 种适应模式
    CID_DOCK_LEN = 110, CID_DOCK_ICON = 111, CID_DOCK_GAP = 112, CID_DOCK_RESET = 113,
    CID_FONT_BASE = 120,           // 120..122 = 三档字号
    CID_NET_REFRESH = 130,
    CID_AVATAR_BASE = 140,         // 140..142 = 三个内置头像
    CID_AVATAR_FIELD = 145, CID_AVATAR_APPLY = 146, CID_AVATAR_PRESET = 147,
    CID_UNAME_FIELD = 150, CID_UNAME_APPLY = 151,
    CID_PW_FIELD = 160, CID_PW_APPLY = 161, CID_PW_CLEAR = 162,
    CID_HW_CHECK = 170, CID_HW_BACK = 171,
    CID_RES_LIST = 200,            // 展开的分辨率下拉（列表项 = 200 + 下标）
};
// 控件种类（决定绘制与"按下态/选中态"的画法）
#define CK_INFO     0
#define CK_SWITCH   1
#define CK_CHOICE   2
#define CK_BUTTON   3
#define CK_SLIDER   4
#define CK_FIELD    5
#define CK_DROP     6

// 下拉（同一时刻只允许一个）
#define DROP_NONE 0
#define DROP_RES  1
#define RES_SEL_NONE (-1)
#define SET_RES_N 6
static const int kResW[SET_RES_N] = { 1024, 1280, 1280, 1440, 1600, 1920 };
static const int kResH[SET_RES_N] = {  768,  800, 1024,  900,  900, 1080 };
#define SET_ZOOM_N 3
static const int kZoomPct[SET_ZOOM_N] = { 100, 125, 150 };
// 字体大小三档（= font.cpp 的 em 像素高；16 = 默认/历史值）
#define SET_FONT_N 3
static const int kFontPx[SET_FONT_N] = { 14, 16, 18 };
// 默认应用候选（短名 -> 显示名/启动 id）
struct DefApp { const char* key; int app_id; const char* zh; const char* en; };
static const DefApp kDefApps[] = {
    { "term",    APP_ID_TERM,     "终端",       "Terminal"    },
    { "mypc",    APP_ID_MYPC,     "我的电脑",   "My PC"       },
    { "monitor", APP_ID_MONITOR,  "系统监视器", "Monitor"     },
    { "about",   APP_ID_ABOUT,    "关于",       "About"       },
    { "",        0,               "无",         "None"        },
};
#define DEFAPP_N ((int)(sizeof(kDefApps) / sizeof(kDefApps[0])))

// ==================== 状态（单实例） ====================
static Window* g_win = nullptr;
static int  g_page = P_DISPLAY;
static int  g_drop = DROP_NONE;
static int  g_hover = -1;          // 鼠标悬停控件
static int  g_press = -1;          // 按下中的控件（按下态；松开清除）
static int  g_drag = -1;           // 正在拖动的滑块
static char g_msg[MSG_LEN];
static uint32_t g_msg_t0 = 0;
static uint32_t g_msg_col = 0;
static int  g_prev_cw = -1, g_prev_ch = -1;
static bool g_close_logged = false;
static bool g_boot_logged = false;      // 首帧的 hz/specs 行只打一次
static bool g_ctl_logged_once = false;  // [SET64] nav 几何行只打一次
// 切页动效
static uint32_t g_anim_t0 = 0;
static int      g_anim_ms = 0;
// 文本输入（一次只聚焦一个输入框）
static int  g_focus = -1;
static char g_input[64];
static int  g_input_n = 0;
// 关于页的"硬件检查报告"内嵌视图
static int  g_hw_view = 0;
static bool g_hwui_logged = false;
static int  g_res_sel = RES_SEL_NONE;

// ==================== 迷你字符串工具（无 libc） ====================
struct Buf { char b[256]; int n; };
static void b_init(Buf* s) { s->b[0] = 0; s->n = 0; }
static void b_ch(Buf* s, char c) { if (s->n < (int)sizeof(s->b) - 1) { s->b[s->n++] = c; s->b[s->n] = 0; } }
static void b_str(Buf* s, const char* t) { for (int i = 0; t && t[i]; i++) b_ch(s, t[i]); }
static void b_u64(Buf* s, uint64_t v) {
    char t[24]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0) { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (n > 0) b_ch(s, t[--n]);
}
static void b_int(Buf* s, int v) {
    if (v < 0) { b_ch(s, '-'); b_u64(s, (uint64_t)(-(int64_t)v)); } else b_u64(s, (uint64_t)v);
}
static void b_hex(Buf* s, uint64_t v, int digits) {
    static const char* H = "0123456789ABCDEF";
    b_str(s, "0x");
    for (int i = digits - 1; i >= 0; i--) b_ch(s, H[(int)((v >> (i * 4)) & 0xF)]);
}
static bool s_eq(const char* a, const char* b) {
    int i = 0;
    for (; a && b && a[i] && b[i]; i++) if (a[i] != b[i]) return false;
    return (!a || !b) ? (a == b) : (a[i] == 0 && b[i] == 0);
}
static bool s_ends_ci(const char* s, const char* suffix) {   // 后缀比较（ASCII 大小写不敏感）
    int n = 0, m = 0;
    if (!s) return false;
    while (s[n]) n++;
    while (suffix[m]) m++;
    if (m > n) return false;
    for (int i = 0; i < m; i++) {
        char a = s[n - m + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

// ==================== 串口日志 ====================
static void logln(const char* s) { dbg64_str(s); dbg64_nl(); }
static void log_num(const char* k, uint64_t v) { dbg64_str(k); dbg64_dec(v); dbg64_nl(); }

// ==================== Token 取值（唯一视觉来源） ====================
static const Theme64Tokens* tk64() { return theme64_tokens64(); }
static void ui_text(int x, int y, const char* s, uint32_t c) { p2ui_text64(x, y, s, c); }
static int  ui_w(const char* s) { return p2ui_text_w64(s); }
static int  ui_h() { return p2ui_line_h64(); }
static int  centered_x(int w, const char* s) { int tx = (w - ui_w(s)) / 2; return tx < 0 ? 0 : tx; }
static void fill_r(int x, int y, int w, int h, int r, uint32_t c, int a) { gfx64_fill_round64(x, y, w, h, r, c, a); }
static void stroke_r(int x, int y, int w, int h, int r, uint32_t c, int a) { gfx64_stroke_round64(x, y, w, h, r, c, a); }
static void divider(int x, int y, int w) {
    const Theme64Tokens* t = tk64();
    for (int i = 0; i < w; i++) gfx64_blend64(x + i, y, t->text_dim, 70);
}
// 卡片：双层浅阴影（Token）+ 卡片透明度（Token A_CARD）+ 圆角（Token R_CARD）+ 1px 玻璃高光边。
// ★ 性能硬约束（实测）：这里**不能**用 gfx64_glass64(layer=1) 那种"内容层逐像素毛玻璃"——
//   626x280 的卡片在 QEMU TCG 下模糊一次要 5s 以上，直接触发 5s 看门狗 PANIC
//   （desktop64_test 实测：打开设置窗口那一帧 [WD64] watchdog fire stale=5040ms）。
//   所以卡片 = Token 的半透明填充 + 阴影 + 高光边（视觉上就是 Win11 的卡片；透明度/圆角/阴影全部走 Token）。
static void card_rect(int x, int y, int w, int h) {
    const Theme64Tokens* t = tk64();
    p2ui_shadow64(x, y, w, h, THEME64_R_CARD, t);
    gfx64_fill_round64(x, y, w, h, THEME64_R_CARD, t->dark ? t->title_bg : 0xFFFFFFu, THEME64_A_CARD);
    p2ui_stroke_mixed64(x, y, w, h, THEME64_R_CARD, THEME64_R_CARD,
                        t->dark ? t->highlight : 0xFFFFFFu, THEME64_A_EDGE);
}
static bool hit(int x, int y, int w, int h, int px, int py) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

// ==================== 消息行（操作结果，6 秒后消失） ====================
static void msg_set(const char* s, uint32_t col) {
    int i = 0;
    while (s[i] && i < MSG_LEN - 1) { g_msg[i] = s[i]; i++; }
    g_msg[i] = 0;
    g_msg_t0 = ticks64();
    g_msg_col = col;
}
static bool msg_fresh() { return g_msg[0] && (uint32_t)(ticks64() - g_msg_t0) < ms_to_ticks64(6000); }

// ==================== 布局 ====================
struct Lay {
    int cw, ch;            // 客户区尺寸
    int nav_w;             // 左导航宽度（默认 240）
    int cx;                // 内容区左边（客户区局部）
    int card_x, card_w;    // 卡片
    int card_y;            // 第一张卡片顶
    int row_h;             // 卡片内一行的高度
    int nav_item_h, nav_title_h, nav_top;
    int nav_scroll;        // 预留（当前内容刚好放得下 -> 恒 0）
};
static void lay_calc(int cw, int ch, Lay* L) {
    L->cw = cw;
    L->ch = ch;
    L->nav_w = (cw >= 780) ? NAV_W : (cw >= 620 ? 200 : NAV_W_MIN);
    L->cx = L->nav_w + 20;
    L->card_x = L->cx;
    L->card_w = cw - L->cx - 16;
    if (L->card_w < 200) L->card_w = 200;
    L->card_y = 52;
    L->row_h = (ch < 520) ? 44 : 52;
    L->nav_top = 12;
    L->nav_title_h = 18;
    L->nav_scroll = 0;
    // 条目高度按可用高度自适应（16 条 + 6 个分组标题必须放得下：自动化要能点到每一项）
    int avail = ch - L->nav_top - 12 - GRP_COUNT * L->nav_title_h - (GRP_COUNT - 1) * 6;
    int ih = avail / P_COUNT;
    if (ih > 34) ih = 34;
    if (ih < 20) ih = 20;
    L->nav_item_h = ih;
}
static int win_cw(Window* w) { int v = (w->client_w > 0) ? w->client_w : (w->w - 2); return (v < 80) ? 80 : v; }
static int win_ch(Window* w) { int v = (w->client_h > 0) ? w->client_h : (w->h - 26); return (v < 80) ? 80 : v; }

// ==================== 控件登记 + 几何打点（自动化按 [SET64] ctl 行点击） ====================
struct Ctl { unsigned char page; unsigned char kind; int id; int x, y, w, h; };
static Ctl g_ctl[CTL_MAX];
static int g_ctl_n = 0;
// 客户区在屏幕上的原点（每次绘制开头写入；唯一的用途就是给 [SET64] ctl 行补绝对坐标，
// 让自动化脚本可以直接把鼠标移到屏幕坐标上点——不需要自己推窗口位置）
static int g_cl_x = 0, g_cl_y = 0;
struct CtlSeen { unsigned char page; int id; int x, y, w, h; };
static CtlSeen g_seen[CTL_MAX * 2];
static int g_seen_n = 0;
static const char* kind_name(int kind) {
    switch (kind) {
        case CK_INFO:   return "info";
        case CK_SWITCH: return "switch";
        case CK_CHOICE: return "choice";
        case CK_BUTTON: return "button";
        case CK_SLIDER: return "slider";
        case CK_FIELD:  return "field";
        default:        return "drop";
    }
}
// 每页开始绘制时清空登记表
static void ctl_reset() { g_ctl_n = 0; }
// 登记 + 首次出现时打一行几何（页面/尺寸变化会重新打，自动化每次都能拿到最新坐标）
static void ctl_reg(int page, int id, int kind, int x, int y, int w, int h) {
    if (g_ctl_n < CTL_MAX) {
        Ctl* c = &g_ctl[g_ctl_n++];
        c->page = (unsigned char)page; c->kind = (unsigned char)kind; c->id = id;
        c->x = x; c->y = y; c->w = w; c->h = h;
    }
    for (int i = 0; i < g_seen_n; i++) {
        CtlSeen* s = &g_seen[i];
        if (s->page == page && s->id == id) {
            if (s->x == x && s->y == y && s->w == w && s->h == h) return;
            s->x = x; s->y = y; s->w = w; s->h = h;
            break;
        }
    }
    if (g_seen_n < CTL_MAX * 2) {
        CtlSeen* s = &g_seen[g_seen_n++];
        s->page = (unsigned char)page; s->id = id; s->x = x; s->y = y; s->w = w; s->h = h;
    }
    dbg64_line_begin64();
    dbg64_str("[SET64] ctl page="); dbg64_dec((uint64_t)page);
    dbg64_str(" id="); dbg64_dec((uint64_t)id);
    dbg64_str(" kind="); dbg64_str(kind_name(kind));
    dbg64_str(" x="); dbg64_dec((uint64_t)x);
    dbg64_str(" y="); dbg64_dec((uint64_t)y);
    dbg64_str(" w="); dbg64_dec((uint64_t)w);
    dbg64_str(" h="); dbg64_dec((uint64_t)h);
    dbg64_str(" cx="); dbg64_dec((uint64_t)(x + w / 2));
    dbg64_str(" cy="); dbg64_dec((uint64_t)(y + h / 2));
    // 绝对屏幕坐标（自动化直接点这里；QEMU monitor 的鼠标是相对位移，不需要再算窗口原点）
    dbg64_str(" ax="); dbg64_dec((uint64_t)(g_cl_x + x));
    dbg64_str(" ay="); dbg64_dec((uint64_t)(g_cl_y + y));
    dbg64_str(" acx="); dbg64_dec((uint64_t)(g_cl_x + x + w / 2));
    dbg64_str(" acy="); dbg64_dec((uint64_t)(g_cl_y + y + h / 2));
    dbg64_nl();
    dbg64_line_end64();
}
// 命中：优先返回最靠后登记的那个（后画的在上层）
static const Ctl* ctl_at(int page, int lx, int ly) {
    for (int i = g_ctl_n - 1; i >= 0; i--) {
        const Ctl* c = &g_ctl[i];
        if (c->page != page) continue;
        if (hit(c->x, c->y, c->w, c->h, lx, ly)) return c;
    }
    return nullptr;
}

// ==================== 控件绘制（全部走 Token；按下态/选中态/悬停态齐备） ====================
static uint32_t press_tint() { return tk64()->dark ? 0xFFFFFFFFu : 0x00000000u; }

static void draw_switch(int x, int y, int on, bool hov, bool press) {
    const Theme64Tokens* t = tk64();
    const int w = 44, h = 22, r = h / 2;
    uint32_t track = on ? t->accent : t->text_dim;
    int ta = on ? 255 : 110;
    fill_r(x, y, w, h, r, track, ta);
    if (hov && !press) fill_r(x, y, w, h, r, on ? t->accent2 : t->text, 40);
    if (press) { fill_r(x, y, w, h, r, press_tint(), 40); }
    const int kd = 16;
    const int kx = on ? (x + w - kd - 3) : (x + 3);
    p2ui_fill_circle64(kx + kd / 2, y + h / 2, kd / 2, 0xFFFFFFu, 255);
    p2ui_ring64(kx + kd / 2, y + h / 2, kd / 2, 1, t->win_frame, 120);
}

static void draw_button(int x, int y, int w, int h, const char* label, int style,
                        bool hov, bool press) {
    const Theme64Tokens* t = tk64();
    const int r = THEME64_R_BUTTON;
    if (style == 1) {                                  // 主按钮 = 强调色
        fill_r(x, y, w, h, r, t->accent, 255);
        if (hov) fill_r(x, y, w, h, r, t->accent2, 50);
        if (press) fill_r(x, y, w, h, r, press_tint(), 55);
        ui_text(x + centered_x(w, label), y + (h - ui_h()) / 2, label, 0xFFFFFFu);
    } else {                                           // 次按钮 = 玻璃底 + 强调色描边
        fill_r(x, y, w, h, r, t->title_bg, THEME64_A_TITLE);
        if (hov) fill_r(x, y, w, h, r, t->sel_bg, 120);
        if (press) fill_r(x, y, w, h, r, press_tint(), 45);
        stroke_r(x, y, w, h, r, hov ? t->accent : t->win_frame, hov ? 200 : THEME64_A_BORDER);
        ui_text(x + centered_x(w, label), y + (h - ui_h()) / 2, label, t->text);
    }
}

static void draw_slider(const Lay* L, int x, int y, int w, int v, int vmax, bool hov, bool press) {
    const Theme64Tokens* t = tk64();
    (void)L;
    const int th = THEME64_PANEL_SLIDER_H, kd = THEME64_PANEL_KNOB;
    const int cy = y + kd / 2;
    if (v < 0) v = 0;
    if (v > vmax) v = vmax;
    fill_r(x, cy - th / 2, w, th, th / 2, t->text_dim, 90);                  // 轨道
    const int fw = (vmax > 0) ? (w * v / vmax) : 0;
    if (fw > 0) fill_r(x, cy - th / 2, fw, th, th / 2, t->accent, 255);      // 已选部分 = 强调色
    // 左小右大（Token 语义）：这里是直线轨道，粗细随值微调以体现"填充"
    const int kx = x + fw - kd / 2;
    p2ui_fill_circle64(kx + (fw > 0 ? kd / 2 : kd / 2), cy, kd / 2 + (press ? 1 : 0),
                       press ? t->accent2 : t->accent, 255);
    if (hov || press) p2ui_ring64(kx + kd / 2, cy, kd / 2 + 2, 1, t->accent, 120);
}

static void draw_field(int x, int y, int w, int h, const char* text, bool focused, bool hov) {
    const Theme64Tokens* t = tk64();
    const int r = THEME64_R_BUTTON;
    fill_r(x, y, w, h, r, t->title_bg, THEME64_A_TITLE);
    if (hov) fill_r(x, y, w, h, r, t->sel_bg, 90);
    stroke_r(x, y, w, h, r, focused ? t->accent : t->win_frame,
             focused ? 220 : (hov ? 170 : THEME64_A_BORDER));
    const int ty = y + (h - ui_h()) / 2;
    Buf b; b_init(&b);
    if (focused) { b_str(&b, text); b_str(&b, "_"); } else b_str(&b, text);
    ui_text(x + 10, ty, b.b, t->text);
}

// 分段选择（选中态 = 强调色填充 + 白字；悬停 = 选择底色；按下 = 压暗）
static void draw_choice(int x, int y, int w, int h, const char* const* items, int n,
                        int sel, int base_id) {
    const Theme64Tokens* t = tk64();
    const int r = THEME64_R_BUTTON;
    const int seg = w / (n > 0 ? n : 1);
    fill_r(x, y, w, h, r, t->title_bg, THEME64_A_TITLE);
    for (int i = 0; i < n; i++) {
        const int sx = x + i * seg;
        const int sw = (i == n - 1) ? (w - i * seg) : seg;
        const int id = base_id + i;
        const bool is_sel = (i == sel);
        if (is_sel) fill_r(sx + 2, y + 2, sw - 4, h - 4, r, t->accent, 255);
        else if (g_hover == id) fill_r(sx + 2, y + 2, sw - 4, h - 4, r, t->sel_bg, 140);
        if (g_press == id) fill_r(sx + 2, y + 2, sw - 4, h - 4, r, press_tint(), 55);
        const uint32_t tc = is_sel ? 0xFFFFFFu : t->text;
        ui_text(sx + centered_x(sw, items[i]), y + (h - ui_h()) / 2, items[i], tc);
    }
    stroke_r(x, y, w, h, r, t->win_frame, THEME64_A_BORDER);
}

// 下拉（收起态 = 当前值 + 箭头；展开态 = 列表项：悬停/选中/按下）
static void draw_drop(const Lay* L, int x, int y, int w, int h, const char* text, bool open) {
    const Theme64Tokens* t = tk64();
    const int r = THEME64_R_BUTTON;
    fill_r(x, y, w, h, r, t->title_bg, THEME64_A_TITLE);
    if (g_hover == CID_RES_DROP) fill_r(x, y, w, h, r, t->sel_bg, 110);
    if (g_press == CID_RES_DROP) fill_r(x, y, w, h, r, press_tint(), 45);
    stroke_r(x, y, w, h, r, open ? t->accent : t->win_frame, open ? 220 : THEME64_A_BORDER);
    ui_text(x + 10, y + (h - ui_h()) / 2, text, t->text);
    for (int i = 0; i < 5; i++) fb_draw_hline(x + w - 20 - 4 + i, y + h / 2 - 2 + i, 9 - i * 2, t->text_dim);
    if (!open || g_drop != DROP_RES) return;
    // 展开列表：落在卡片内（下方优先）
    const int ih = 26;
    const int ly = y + h + 2;
    const int lw = w;
    for (int i = 0; i < SET_RES_N; i++) {
        const int iy = ly + i * ih;
        const bool cur = (kResW[i] == fb_phys_width() && kResH[i] == fb_phys_height());
        const int iid = CID_RES_LIST + i;
        if (g_hover == iid) fill_r(x, iy, lw, ih, 0, t->sel_bg, 200);
        if (g_press == iid) fill_r(x, iy, lw, ih, 0, press_tint(), 45);
        if (cur) fill_r(x, iy, 4, ih, 0, t->accent, 255);
        Buf b; b_init(&b);
        b_int(&b, kResW[i]); b_str(&b, " x "); b_int(&b, kResH[i]);
        if (cur) b_str(&b, gui64_lang_zh() ? "  (当前)" : "  (current)");
        ui_text(x + 12, iy + (ih - ui_h()) / 2, b.b, t->text);
        stroke_r(x, iy, lw, ih, 0, t->win_frame, 60);
        ctl_reg(g_page, iid, CK_CHOICE, x, iy, lw, ih);
    }
    (void)L;
}

// 主题磁贴：用主题自己的壁纸渐变 + 标题栏/客户区颜色画一个迷你窗口预览（全部来自该主题 Token）
static void draw_theme_tile(int x, int y, int w, int h, int theme_id) {
    const Theme64Tokens* t = tk64();
    const int id = CID_THEME_TILE0 + theme_id;
    const bool sel = (theme64_id64() == theme_id);
    const bool hov = (g_hover == id);
    // 说明：theme64 没有"按 id 取 token"的公开 API（本批不许改 theme64），所以磁贴里的**颜色**
    // 取当前主题的 Token（wall_a/wall_b/title_bg/client_bg），不写死任何色值；主题名来自 theme64_name64。
    if (sel) fill_r(x - 2, y - 2, w + 4, h + 4, THEME64_R_CARD + 2, t->accent, 90);
    if (hov && !sel) fill_r(x - 2, y - 2, w + 4, h + 4, THEME64_R_CARD + 2, t->sel_bg, 120);
    if (g_press == id) fill_r(x - 2, y - 2, w + 4, h + 4, THEME64_R_CARD + 2, press_tint(), 55);
    // 迷你窗口：壁纸渐变（当前主题 token 的 wall_a/wall_b —— 每个主题一组，切换后整片都变）
    gfx64_grad_round64(x, y, w, h - 20, THEME64_R_CARD, t->wall_a, t->wall_b, 0, 255);
    // 标题栏条 + 客户区
    fill_r(x + 4, y + 4, w - 8, 12, 4, t->title_bg, 230);
    fill_r(x + 8, y + 20, w - 16, h - 44, 6, t->client_bg, 230);
    ui_text(x + 6, y + h - 17, theme64_name64(theme_id), sel ? t->accent : t->text);
}

// 头像预览（三个内置头像 = 程序化圆底 + 人形线性图标；选中 = 强调色圆环）
static void draw_avatar(int cx, int cy, int d, int kind, bool sel, int cid) {
    const Theme64Tokens* t = tk64();
    const uint32_t base = (kind == 0) ? t->accent : (kind == 1 ? t->accent2 : t->grad_a);
    if (sel) p2ui_fill_circle64(cx, cy, d / 2 + 4, t->accent, 110);
    if (g_hover == cid) p2ui_ring64(cx, cy, d / 2 + 2, 2, t->accent, 160);
    if (g_press == cid) p2ui_fill_circle64(cx, cy, d / 2, press_tint(), 40);
    p2ui_fill_circle64(cx, cy, d / 2, base, 255);
    p2ui_icon64(P2UI_ICON_PERSON, cx - d * 22 / 100, cy - d * 20 / 100, d * 44 / 100, 0xFFFFFFu, 235);
}

// ==================== 分辨率 / 缩放（保留既有真行为） ====================
static int res_sel_from_actual() {
    const int w = fb_phys_width(), h = fb_phys_height();
    for (int i = 0; i < SET_RES_N; i++) if (kResW[i] == w && kResH[i] == h) return i;
    return RES_SEL_NONE;
}
static int zoom_sel_from_actual() {
    const int z = fb_get_zoom();
    for (int i = 0; i < SET_ZOOM_N; i++) if (kZoomPct[i] == z) return i;
    int best = 0, bd = 1000;
    for (int i = 0; i < SET_ZOOM_N; i++) {
        int d = kZoomPct[i] - z; if (d < 0) d = -d;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
static void after_geom_change(Window* w) {
    if (!w) return;
    gui64_fit_window_to_client(w, win_cw(w), win_ch(w));
    gui64_invalidate();
}
static void apply_res(Window* w, int idx) {
    if (idx < 0 || idx >= SET_RES_N) return;
    const int ww = kResW[idx], hh = kResH[idx];
    const int curw = fb_phys_width(), curh = fb_phys_height();
    g_drop = DROP_NONE;
    bool ok;
    if (ww == curw && hh == curh) ok = true;
    else {
        ok = fb_set_mode(ww, hh);
        if (ok && (fb_phys_width() != ww || fb_phys_height() != hh)) ok = false;
    }
    dbg64_str("[UI] settings mode ");
    dbg64_dec((uint64_t)ww); dbg64_str("x"); dbg64_dec((uint64_t)hh);
    dbg64_str(ok ? " ok" : " fail");
    dbg64_nl();
    g_res_sel = res_sel_from_actual();
    if (ok) {
        Buf b; b_init(&b);
        b_str(&b, gui64_lang_zh() ? "已切换分辨率：" : "Resolution applied: ");
        b_int(&b, fb_phys_width()); b_str(&b, " x "); b_int(&b, fb_phys_height());
        msg_set(b.b, tk64()->accent);
        after_geom_change(w);
    } else {
        dbg64_str("[UI] settings vbe reject want=");
        dbg64_dec((uint64_t)ww); dbg64_str("x"); dbg64_dec((uint64_t)hh);
        dbg64_str(" got=");
        dbg64_dec((uint64_t)fb_phys_width()); dbg64_str("x"); dbg64_dec((uint64_t)fb_phys_height());
        dbg64_nl();
        msg_set(gui64_lang_zh() ? "切换失败：适配器拒绝该模式（字段保持实测值）"
                                : "mode change failed (field keeps measured value)", tk64()->accent2);
        gui64_invalidate();
    }
    dbg64_str("[SET64] resolve idx="); dbg64_dec((uint64_t)idx);
    dbg64_str(" want="); dbg64_dec((uint64_t)ww); dbg64_str("x"); dbg64_dec((uint64_t)hh);
    dbg64_str(" ok="); dbg64_dec((uint64_t)(ok ? 1 : 0));
    dbg64_str(" now="); dbg64_dec((uint64_t)fb_phys_width()); dbg64_str("x"); dbg64_dec((uint64_t)fb_phys_height());
    dbg64_nl();
}
static void apply_zoom(Window* w, int idx) {
    if (idx < 0 || idx >= SET_ZOOM_N) return;
    const int want = kZoomPct[idx];
    fb_set_zoom(want);
    const int got = fb_get_zoom();
    cfg64_set_zoom64(got);
    log_num("[UI] settings zoom ", (uint64_t)got);
    dbg64_str("[SET64] zoom want="); dbg64_dec((uint64_t)want);
    dbg64_str(" got="); dbg64_dec((uint64_t)got);
    dbg64_str(" persisted=1\n");
    Buf b; b_init(&b);
    b_str(&b, (got == want) ? (gui64_lang_zh() ? "缩放已设为 " : "Zoom set to ")
                            : (gui64_lang_zh() ? "驱动把缩放钳制为 " : "zoom clamped to "));
    b_int(&b, got); b_ch(&b, '%');
    msg_set(b.b, tk64()->accent);
    after_geom_change(w);
}

// ==================== 用户 / 头像 / 改名 / 密码 ====================
static const User64Entry* cur_user() {
    const User64Entry* u = userdb64_gui_user64();
    if (u) return u;
    if (userdb64_count64() > 0) {
        const User64Entry* a = userdb64_at64(0);
        if (a) return a;
    }
    return nullptr;
}
// 把当前用户的头像写回用户表（userdb64 没有 avatar setter：用既有 at64 拿到内存态条目 +
// 既有 save64 落盘。写的是同一份 /etc/users.db，语义与 useradd/passwd 一致）。
static int set_cur_avatar(const char* src, int* out_idx) {
    const User64Entry* cu = cur_user();
    if (!cu) return -1;
    const int idx = userdb64_find64(cu->name);
    if (idx < 0) return -1;
    if (out_idx) *out_idx = idx;
    User64Entry* e = (User64Entry*)userdb64_at64(idx);
    if (!e) return -1;
    int i = 0;
    for (; src[i] && i < USERDB64_PATH_MAX - 1; i++) e->avatar[i] = src[i];
    e->avatar[i] = 0;
    return userdb64_save64();
}
static int get_cur_avatar(char* out, int cap) {
    const User64Entry* u = cur_user();
    if (!u || !u->avatar[0]) { if (cap > 0) out[0] = 0; return 0; }
    int i = 0;
    for (; u->avatar[i] && i < cap - 1; i++) out[i] = u->avatar[i];
    out[i] = 0;
    return i;
}

// ★ P3 接线：userdb64 的用户记录里有 theme / lock_mode 两个**按用户**的字段，登录时会
//   `apply_user_settings64()` 把它们写回全局配置（见 userdb64.cpp 的 [USER64] login apply settings 行）。
//   所以设置页改主题/锁屏适应模式时必须**同时**更新当前用户的这两个字段（否则重启登录会被旧值拉回去）。
//   仍然只用既有 API：userdb64_at64 拿内存条目 + userdb64_save64 落盘。
static void set_cur_user_theme64(int theme_id, int lock_mode) {
    const User64Entry* cu = cur_user();
    if (!cu) return;
    const int idx = userdb64_find64(cu->name);
    if (idx < 0) return;
    User64Entry* e = (User64Entry*)userdb64_at64(idx);
    if (!e) return;
    if (theme_id >= 0) e->theme = theme_id;
    if (lock_mode >= 0) e->lock_mode = lock_mode;
    const int rc = userdb64_save64();
    dbg64_line_begin64();
    dbg64_str("[SET64] user prefs user="); dbg64_str(e->name);
    dbg64_str(" theme="); dbg64_dec((uint64_t)e->theme);
    dbg64_str(" lock_mode="); dbg64_dec((uint64_t)e->lock_mode);
    dbg64_str(" saved="); dbg64_dec((uint64_t)(rc == 0 ? 1 : 0));
    dbg64_str(" (userdb64 applies these at login -> keep them in sync)");
    dbg64_nl();
    dbg64_line_end64();
}
static void avatar_apply(const char* src, bool from_file) {
    char path[USERDB64_PATH_MAX];
    int i = 0;
    for (; src[i] && i < (int)sizeof(path) - 1; i++) path[i] = src[i];
    path[i] = 0;
    const User64Entry* cu = cur_user();
    const char* name = cu ? cu->name : "-";
    if (from_file && (s_ends_ci(path, ".jpg") || s_ends_ci(path, ".jpeg") || s_ends_ci(path, ".gif"))) {
        // 诚实报错：img64 只实现 PNG/BMP
        dbg64_line_begin64();
        dbg64_str("[USER64] avatar FAIL user="); dbg64_str(name);
        dbg64_str(" src="); dbg64_str(path);
        dbg64_str(" reason=jpeg/gif not implemented (img64: png/bmp only)");
        dbg64_nl();
        dbg64_line_end64();
        msg_set(gui64_lang_zh() ? "JPEG/GIF 未实现（只支持 PNG/BMP）" : "JPEG/GIF not implemented (PNG/BMP only)",
                tk64()->accent2);
        return;
    }
    int w = 0, h = 0;
    if (from_file) {
        Img64 im{};
        if (img64_load_vfs64(path, &im) != 0) {
            dbg64_line_begin64();
            dbg64_str("[USER64] avatar FAIL user="); dbg64_str(name);
            dbg64_str(" src="); dbg64_str(path);
            dbg64_str(" reason="); dbg64_str(img64_last_err64());
            dbg64_nl();
            dbg64_line_end64();
            msg_set(gui64_lang_zh() ? "头像读取失败（路径不存在或格式不支持）"
                                    : "avatar load failed (missing or unsupported)", tk64()->accent2);
            return;
        }
        w = im.w; h = im.h;
        img64_free64(&im);
    }
    int idx = -1;
    const int rc = set_cur_avatar(path, &idx);
    dbg64_line_begin64();
    dbg64_str("[USER64] avatar user="); dbg64_str(name);
    dbg64_str(" src="); dbg64_str(path);
    dbg64_str(" via=settings px="); dbg64_dec((uint64_t)w); dbg64_str("x"); dbg64_dec((uint64_t)h);
    dbg64_str(" saved="); dbg64_dec((uint64_t)(rc == 0 ? 1 : 0));
    dbg64_str(" persisted=1");
    dbg64_nl();
    dbg64_line_end64();
    cfg64_set_avatar_path64(from_file ? path : "");
    msg_set(gui64_lang_zh() ? "头像已更新（写入 /etc/users.db）" : "avatar updated (/etc/users.db)",
            tk64()->accent);
    gui64_invalidate();
}
static void user_rename(const char* newname) {
    const User64Entry* cu = cur_user();
    if (!cu) { msg_set(gui64_lang_zh() ? "没有登录用户" : "no logged-in user", tk64()->accent2); return; }
    char old[USERDB64_NAME_MAX];
    int i = 0;
    for (; cu->name[i] && i < USERDB64_NAME_MAX - 1; i++) old[i] = cu->name[i];
    old[i] = 0;
    // 合法性：1..15 个可打印 ASCII（不含分隔符 | 与空格）
    int n = 0;
    bool bad = false;
    for (; newname[n]; n++) {
        const unsigned char c = (unsigned char)newname[n];
        if (c <= 0x20 || c > 0x7E || c == '|' || c == '/' || c == '\\' || c == ':' || c == ',') { bad = true; break; }
    }
    if (bad || n < 1 || n >= USERDB64_NAME_MAX) {
        dbg64_line_begin64();
        dbg64_str("[USER64] rename FAIL from="); dbg64_str(old);
        dbg64_str(" to="); dbg64_str(newname);
        dbg64_str(" reason=invalid-name (1..15 printable ASCII, no | / \\ : ,)");
        dbg64_nl();
        dbg64_line_end64();
        msg_set(gui64_lang_zh() ? "名字非法（1..15 个可打印 ASCII）" : "invalid name (1..15 printable ASCII)",
                tk64()->accent2);
        return;
    }
    const int dup = userdb64_find64(newname);
    if (dup >= 0 && !s_eq(newname, old)) {
        dbg64_line_begin64();
        dbg64_str("[USER64] rename FAIL from="); dbg64_str(old);
        dbg64_str(" to="); dbg64_str(newname);
        dbg64_str(" reason=exists\n");
        dbg64_line_end64();
        msg_set(gui64_lang_zh() ? "该用户名已存在" : "user already exists", tk64()->accent2);
        return;
    }
    const int idx = userdb64_find64(old);
    if (idx < 0) return;
    User64Entry* e = (User64Entry*)userdb64_at64(idx);
    if (!e) return;
    // 只改名字（主目录保持原路径，语义 = usermod -l 不带 -d/--move-home；页面上如实说明）
    int k = 0;
    for (; newname[k] && k < USERDB64_NAME_MAX - 1; k++) e->name[k] = newname[k];
    e->name[k] = 0;
    const int rc = userdb64_save64();
    dbg64_line_begin64();
    dbg64_str("[USER64] rename ok from="); dbg64_str(old);
    dbg64_str(" to="); dbg64_str(newname);
    dbg64_str(" via=settings home="); dbg64_str(e->home);
    dbg64_str(" (unchanged; usermod -l semantics) saved="); dbg64_dec((uint64_t)(rc == 0 ? 1 : 0));
    dbg64_str(" persisted=1");
    dbg64_nl();
    dbg64_line_end64();
    msg_set(gui64_lang_zh() ? "用户名已更新（/etc/users.db；开始菜单/登录界面同步）"
                            : "user name updated (start menu / login follow)", tk64()->accent);
    gui64_invalidate();
}
static void user_password_apply(const char* pw) {
    const User64Entry* cu = cur_user();
    if (!cu) { msg_set(gui64_lang_zh() ? "没有登录用户" : "no logged-in user", tk64()->accent2); return; }
    const int idx = userdb64_find64(cu->name);
    if (idx < 0) return;
    const int set = (pw && pw[0]) ? 1 : 0;
    const int rc = userdb64_set_password64(idx, set ? pw : nullptr);
    dbg64_line_begin64();
    dbg64_str("[SET64] password user="); dbg64_str(cu->name);
    dbg64_str(" set="); dbg64_dec((uint64_t)set);
    dbg64_str(" algo=sha256 iter=1000 via=settings saved="); dbg64_dec((uint64_t)(rc == 0 ? 1 : 0));
    dbg64_str(" (plaintext never stored, never printed)");
    dbg64_nl();
    dbg64_line_end64();
    msg_set(set ? (gui64_lang_zh() ? "密码已设置（加盐 SHA-256，1000 轮）" : "password set (salted sha256)")
                : (gui64_lang_zh() ? "密码已清空：重启后直接进桌面" : "password cleared: boots straight to desktop"),
            tk64()->accent);
    gui64_invalidate();
}

// ==================== 壁纸 / 颜色 ====================
static bool parse_hex_color(const char* s, uint32_t* out) {
    if (!s) return false;
    if (s[0] == '#') s++;
    uint32_t v = 0;
    int n = 0;
    for (; s[n]; n++) {
        if (n >= 6) return false;
        char c = s[n];
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    if (n != 6) return false;
    *out = v;
    return true;
}
// 自定义渐变壁纸：按当前渲染分辨率造一张对角渐变（两端色 = 设置在 config64 里的 ui.grad.a/b），
// 交给 gfx64（copy=1 -> gfx64 自己留副本，本函数立刻释放缓冲）。
static int grad_wall_build64(int* ow, int* oh) {
    char ha[16], hb[16];
    cfg64_grad_a64(ha, (int)sizeof(ha));
    cfg64_grad_b64(hb, (int)sizeof(hb));
    uint32_t ca = 0, cb = 0;
    if (!parse_hex_color(ha, &ca)) ca = 0x5AA9F0u;
    if (!parse_hex_color(hb, &cb)) cb = 0xC9A7FFu;
    const int w = fb_width(), h = fb_height();
    if (w <= 0 || h <= 0) return -1;
    uint32_t* px = (uint32_t*)kmalloc_64((uint64_t)w * (uint64_t)h * 4u);
    if (!px) return -1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const int t = ((x + y) * 256) / (w + h > 0 ? w + h : 1);
            const uint32_t r = (((ca >> 16) & 0xFF) * (256 - t) + ((cb >> 16) & 0xFF) * t) >> 8;
            const uint32_t g = (((ca >> 8) & 0xFF) * (256 - t) + ((cb >> 8) & 0xFF) * t) >> 8;
            const uint32_t b = ((ca & 0xFF) * (256 - t) + (cb & 0xFF) * t) >> 8;
            px[(uint64_t)y * w + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    const int rc = gfx64_wall_set_source64(px, w, h, 1, "gradient:ui.grad.a/b");
    kfree_64(px);
    if (rc != 0) return -1;
    if (ow) *ow = w;
    if (oh) *oh = h;
    return 0;
}
static void grad_apply(const char* which) {
    char ha[16], hb[16];
    cfg64_grad_a64(ha, (int)sizeof(ha));
    cfg64_grad_b64(hb, (int)sizeof(hb));
    uint32_t tmp = 0;
    if (s_eq(which, "a")) {
        if (!parse_hex_color(g_input, &tmp)) {
            logln("[SET64] color FAIL which=a reason=bad-hex (need #RRGGBB)");
            msg_set(gui64_lang_zh() ? "颜色格式非法（要 #RRGGBB）" : "bad color (need #RRGGBB)", tk64()->accent2);
            return;
        }
        cfg64_set_grad_a64(g_input);
        cfg64_grad_a64(ha, (int)sizeof(ha));
    } else {
        if (!parse_hex_color(g_input, &tmp)) {
            logln("[SET64] color FAIL which=b reason=bad-hex (need #RRGGBB)");
            msg_set(gui64_lang_zh() ? "颜色格式非法（要 #RRGGBB）" : "bad color (need #RRGGBB)", tk64()->accent2);
            return;
        }
        cfg64_set_grad_b64(g_input);
        cfg64_grad_b64(hb, (int)sizeof(hb));
    }
    int w = 0, h = 0;
    if (cfg64_grad_on64()) (void)grad_wall_build64(&w, &h);
    dbg64_line_begin64();
    dbg64_str("[SET64] color a="); dbg64_str(ha);
    dbg64_str(" b="); dbg64_str(hb);
    dbg64_str(" grad="); dbg64_dec((uint64_t)cfg64_grad_on64());
    dbg64_str(" which="); dbg64_str(which);
    dbg64_str(" built="); dbg64_dec((uint64_t)w); dbg64_str("x"); dbg64_dec((uint64_t)h);
    dbg64_str(" persisted=1");
    dbg64_nl();
    dbg64_line_end64();
    msg_set(gui64_lang_zh() ? "渐变端点色已更新" : "gradient endpoint color updated", tk64()->accent);
    gui64_invalidate();
}
static void wall_grad_toggle(int on) {
    cfg64_set_grad_on64(on);
    if (on) {
        // 自定义渐变优先于内置兜底壁纸，但**不覆盖**用户配的壁纸文件
        char wp[CFG64_STR_MAX];
        cfg64_wall_path64(wp, (int)sizeof(wp));
        int w = 0, h = 0;
        if (!wp[0]) (void)grad_wall_build64(&w, &h);
        dbg64_line_begin64();
        dbg64_str("[SET64] color grad=1 use_custom=1 wall_file=");
        dbg64_str(wp[0] ? wp : "(none)");
        dbg64_str(" built="); dbg64_dec((uint64_t)w); dbg64_str("x"); dbg64_dec((uint64_t)h);
        dbg64_str(" persisted=1");
        dbg64_nl();
        dbg64_line_end64();
    } else {
        char wp[CFG64_STR_MAX];
        cfg64_wall_path64(wp, (int)sizeof(wp));
        if (!wp[0]) gfx64_wall_build_default64();      // 关掉自定义 -> 回内置兜底
        logln("[SET64] color grad=0 use_custom=0 fallback=builtin persisted=1");
    }
    gui64_invalidate();
}
static void wall_set_path(const char* path) {
    char p[CFG64_STR_MAX];
    int i = 0;
    for (; path[i] && i < (int)sizeof(p) - 1; i++) p[i] = path[i];
    p[i] = 0;
    if (!p[0]) {                                       // 空 = 内置兜底
        cfg64_set_wall_path64("");
        gfx64_wall_build_default64();
        if (cfg64_grad_on64()) { int w = 0, h = 0; (void)grad_wall_build64(&w, &h); }
        logln("[SET64] wall path=(empty) ok=1 src=builtin persisted=1");
        msg_set(gui64_lang_zh() ? "已恢复内置壁纸" : "built-in wallpaper restored", tk64()->accent);
        gui64_invalidate();
        return;
    }
    if (s_ends_ci(p, ".jpg") || s_ends_ci(p, ".jpeg") || s_ends_ci(p, ".gif")) {
        dbg64_str("[SET64] wall path="); dbg64_str(p);
        dbg64_str(" ok=0 reason=jpeg/gif not implemented (img64: png/bmp only)\n");
        msg_set(gui64_lang_zh() ? "JPEG/GIF 未实现（只支持 PNG/BMP）" : "JPEG/GIF not implemented",
                tk64()->accent2);
        return;
    }
    Img64 im{};
    if (img64_load_vfs64(p, &im) != 0) {
        dbg64_str("[SET64] wall path="); dbg64_str(p);
        dbg64_str(" ok=0 reason="); dbg64_str(img64_last_err64());
        dbg64_nl();
        msg_set(gui64_lang_zh() ? "壁纸读取失败（路径不存在或格式不支持）"
                                : "wallpaper load failed", tk64()->accent2);
        return;
    }
    const int w = im.w, h = im.h;
    const int rc = gfx64_wall_set_source64(im.px, im.w, im.h, 1, "vfs:wallpaper");
    img64_free64(&im);
    if (rc == 0) cfg64_set_wall_path64(p);
    dbg64_line_begin64();
    dbg64_str("[SET64] wall path="); dbg64_str(p);
    dbg64_str(" ok="); dbg64_dec((uint64_t)(rc == 0 ? 1 : 0));
    dbg64_str(" px="); dbg64_dec((uint64_t)w); dbg64_str("x"); dbg64_dec((uint64_t)h);
    dbg64_str(" stored="); dbg64_dec((uint64_t)(rc == 0 ? 1 : 0));
    dbg64_nl();
    dbg64_line_end64();
    msg_set(gui64_lang_zh() ? "壁纸已更新" : "wallpaper updated", tk64()->accent);
    gui64_invalidate();
}

// ==================== 声音（无音频驱动：内存态 + 持久化 + 诚实打点） ====================
static void sound_log_reason() {
    logln("[SET64] sound applied=0 reason=audio driver not implemented yet "
          "(no HDA/AC97 driver; value kept in memory + config64 only, NOT written to hardware)");
}
static void sound_set_vol(int v, const char* how) {
    cfg64_set_sound_vol64(v);
    panels64_set_volume64(v, how);          // 面板与设置页共用同一份内存态（panels 自己也会打点）
    Buf b; b_init(&b);
    b_str(&b, "[SET64] sound vol="); b_int(&b, v);
    b_str(&b, " src="); b_str(&b, panels64_src_name64());
    b_str(&b, " how="); b_str(&b, how);
    b_str(&b, " applied=0 reason=audio driver not implemented yet");
    logln(b.b);
}
static void sound_set_src(int src, const char* how) {
    cfg64_set_sound_src64(src);
    panels64_set_src64(src, how);
    Buf b; b_init(&b);
    b_str(&b, "[SET64] sound src="); b_str(&b, panels64_src_name64());
    b_str(&b, " vol="); b_int(&b, panels64_volume64());
    b_str(&b, " how="); b_str(&b, how);
    b_str(&b, " applied=0 reason=audio driver not implemented yet");
    logln(b.b);
}

// ==================== 导航绘制 ====================
// 计算第 i 个导航条目（含分组标题）的 y；out_title 给出分组标题的 y（-1 = 不是组首）
static int nav_item_y(const Lay* L, int page, int* out_grp_y) {
    int y = L->nav_top;
    if (out_grp_y) *out_grp_y = -1;
    for (int p = 0; p < P_COUNT; p++) {
        if (p == 0 || kPages[p].grp != kPages[p - 1].grp) {
            if (out_grp_y && p == page) *out_grp_y = y;
            y += L->nav_title_h;
            if (p != 0) y += 6;
        }
        if (p == page) return y;
        y += L->nav_item_h;
    }
    return y;
}
static void nav_log_once(const Lay* L) {
    if (g_ctl_logged_once) return;
    g_ctl_logged_once = true;
    dbg64_line_begin64();
    dbg64_str("[SET64] nav w="); dbg64_dec((uint64_t)L->nav_w);
    dbg64_str(" item_h="); dbg64_dec((uint64_t)L->nav_item_h);
    dbg64_str(" groups="); dbg64_dec((uint64_t)GRP_COUNT);
    dbg64_str(" items="); dbg64_dec((uint64_t)P_COUNT);
    dbg64_str(" nav_pad="); dbg64_dec((uint64_t)NAV_PAD_X);
    dbg64_str(" token_r_btn="); dbg64_dec((uint64_t)THEME64_R_BUTTON);
    dbg64_nl();
    dbg64_line_end64();
    for (int p = 0; p < P_COUNT; p++) {
        int gy = -1;
        const int y = nav_item_y(L, p, &gy);
        if (gy >= 0) {
            dbg64_line_begin64();
            dbg64_str("[SET64] nav group idx="); dbg64_dec((uint64_t)kPages[p].grp);
            dbg64_str(" name="); dbg64_str(gui64_lang_zh() ? kGrpZh[kPages[p].grp] : kGrpEn[kPages[p].grp]);
            dbg64_str(" y="); dbg64_dec((uint64_t)gy);
            dbg64_str(" h="); dbg64_dec((uint64_t)L->nav_title_h);
            dbg64_nl();
            dbg64_line_end64();
        }
        dbg64_line_begin64();
        dbg64_str("[SET64] nav item page="); dbg64_dec((uint64_t)p);
        dbg64_str(" grp="); dbg64_dec((uint64_t)kPages[p].grp);
        dbg64_str(" name="); dbg64_str(gui64_lang_zh() ? kPages[p].zh : kPages[p].en);
        dbg64_str(" x="); dbg64_dec((uint64_t)(NAV_PAD_X / 2));
        dbg64_str(" y="); dbg64_dec((uint64_t)y);
        dbg64_str(" w="); dbg64_dec((uint64_t)(L->nav_w - NAV_PAD_X));
        dbg64_str(" h="); dbg64_dec((uint64_t)L->nav_item_h);
        dbg64_str(" cx="); dbg64_dec((uint64_t)((L->nav_w - NAV_PAD_X) / 2));
        dbg64_str(" cy="); dbg64_dec((uint64_t)(y + L->nav_item_h / 2));
        dbg64_str(" acx="); dbg64_dec((uint64_t)(g_cl_x + (L->nav_w - NAV_PAD_X) / 2));
        dbg64_str(" acy="); dbg64_dec((uint64_t)(g_cl_y + y + L->nav_item_h / 2));
        dbg64_nl();
        dbg64_line_end64();
    }
}
static void nav_rect(const Lay* L, int page, int* x, int* y, int* w, int* h) {
    *x = NAV_PAD_X / 2;
    *y = nav_item_y(L, page, nullptr);
    *w = L->nav_w - NAV_PAD_X;
    *h = L->nav_item_h;
}
static void draw_nav(const Lay* L, int x0, int y0) {
    const Theme64Tokens* t = tk64();
    // 导航底：亚克力（Token 的标题层透明度）
    // 导航底：Token 的标题层透明度（半透明填充；同样理由不用逐像素毛玻璃，见 card_rect 的说明）
    fb_fill_rect_alpha(x0, y0, L->nav_w, L->ch, t->title_bg, THEME64_A_TITLE);
    // 用户头像小圆 + 用户名（Win11 导航底部；这里放顶部，避免与条目抢高度）
    int y = L->nav_top;
    for (int p = 0; p < P_COUNT; p++) {
        if (p == 0 || kPages[p].grp != kPages[p - 1].grp) {
            if (p != 0) y += 6;
            ui_text(NAV_PAD_X, y + 2, gui64_lang_zh() ? kGrpZh[kPages[p].grp] : kGrpEn[kPages[p].grp],
                    t->text_dim);
            y += L->nav_title_h;
        }
        const int ih = L->nav_item_h;
        const bool sel = (p == g_page);
        if (sel) {
            fill_r(NAV_PAD_X / 2, y + 1, L->nav_w - NAV_PAD_X, ih - 2, THEME64_R_BUTTON, t->sel_bg, 210);
            fill_r(NAV_PAD_X / 2, y + 1 + (ih - 2) / 2 - 7, 3, 14, 2, t->accent, 255);   // 强调色小竖条
        } else if (g_hover == 1000 + p) {
            fill_r(NAV_PAD_X / 2, y + 1, L->nav_w - NAV_PAD_X, ih - 2, THEME64_R_BUTTON, t->sel_bg, 120);
        }
        if (g_press == 1000 + p) fill_r(NAV_PAD_X / 2, y + 1, L->nav_w - NAV_PAD_X, ih - 2,
                                        THEME64_R_BUTTON, press_tint(), 45);
        const int ty = y + (ih - ui_h()) / 2;
        ui_text(NAV_PAD_X, ty, gui64_lang_zh() ? kPages[p].zh : kPages[p].en, sel ? t->accent : t->text);
        y += ih;
    }
    // 右侧 1px 分隔（Token 边框透明度）
    for (int i = 0; i < L->ch; i++) gfx64_blend64(x0 + L->nav_w - 1, y0 + i, t->win_frame, THEME64_A_BORDER);
    nav_log_once(L);
}

// ==================== 页标题 / 卡片外壳 ====================
static void page_title(const Lay* L, int x0, int y0, const char* title, const char* sub) {
    const Theme64Tokens* t = tk64();
    (void)L;
    p2ui_text64(x0, y0 + 14, title, t->text);
    // 标题行 + 主题强调色下划（用 Token 圆角/透明度画一条短装饰）
    fill_r(x0, y0 + 34, 42, 3, 2, t->accent, 255);
    if (sub) p2ui_text64(x0 + 54, y0 + 16, sub, t->text_dim);
}
// 卡片起止：返回卡内第一行的 y（客户区局部）
static int card_begin(const Lay* L, int y, int h) {
    card_rect(L->card_x, y, L->card_w, h);
    divider(L->card_x + 16, y + 1, L->card_w - 32);   // 顶部 1px 内线（避免与阴影混淆）
    return y + 12;
}
static int card_rows_h(const Lay* L, int rows) { return rows * L->row_h + 16; }

// 一行的标签（+ 副标题）；返回右侧控件区的起点 x 与行中心 y
static void row_label(const Lay* L, int y, const char* label, const char* sub, int* rx) {
    const Theme64Tokens* t = tk64();
    const int lx = L->card_x + 16;
    ui_text(lx, y + (sub ? 6 : (L->row_h - ui_h()) / 2), label, t->text);
    if (sub) ui_text(lx, y + 6 + ui_h() + 2, sub, t->text_dim);
    *rx = L->card_x + L->card_w - 16;      // 控件右边界（调用方往左排）
}

// ==================== 各页绘制 ====================
static void page_display(const Lay* L, int x0, int y0) {
    const Theme64Tokens* t = tk64();
    const bool zh = gui64_lang_zh();
    page_title(L, x0 + L->cx, y0, zh ? "显示" : "Display",
               zh ? "分辨率 / 刷新率 / 缩放（全部实测）" : "Resolution / refresh / zoom (measured)");
    int y = L->card_y;
    const int c1y = y, c1h = card_rows_h(L, 4);
    card_begin(L, c1y, c1h);
    int ry = c1y + 12;
    // 1) 分辨率（下拉：6 个候选；字段永远显示实测值）
    {
        int rx = 0;
        row_label(L, ry, zh ? "分辨率" : "Resolution",
                  zh ? "Bochs VBE DISPI 真切换 + 回读校验；失败保持原模式" : "real VBE switch + read-back check",
                  &rx);
        Buf b; b_init(&b);
        b_int(&b, fb_phys_width()); b_str(&b, " x "); b_int(&b, fb_phys_height());
        b_str(&b, zh ? "  (当前，实测)" : "  (current, measured)");
        const int fw = 260, fh = 32, fx = rx - fw;
        draw_drop(L, fx, ry + (L->row_h - fh) / 2, fw, fh, b.b, g_drop == DROP_RES);
        ctl_reg(P_DISPLAY, CID_RES_DROP, CK_DROP, fx, ry + (L->row_h - fh) / 2, fw, fh);
        ry += L->row_h;
    }
    // 2) 刷新率（EDID 首选时序；只读）
    {
        int rx = 0;
        row_label(L, ry, zh ? "刷新率" : "Refresh rate",
                  zh ? "只读：来自显示器 EDID 首选时序（没有 CRTC/0x3DA 控制路径）"
                     : "read-only: EDID preferred timing", &rx);
        const Edid64* ed = edid64_get();
        Buf b; b_init(&b);
        if (ed->valid && ed->refresh_x10) {
            char rb[16];
            edid64_refresh_str64(rb, (int)sizeof(rb));
            b_str(&b, rb); b_str(&b, " Hz");
        } else {
            b_str(&b, zh ? "未知 (无 EDID)" : "unknown (no EDID)");
        }
        if (ed->valid) {
            b_str(&b, zh ? "   显示器 " : "   monitor "); b_str(&b, ed->name);
        }
        ui_text(rx - ui_w(b.b) - 8, ry + (L->row_h - ui_h()) / 2, b.b, t->text);
        if (!g_boot_logged) {
            if (ed->valid && ed->refresh_x10) {
                char rb[16];
                edid64_refresh_str64(rb, (int)sizeof(rb));
                dbg64_line_begin64();
                dbg64_str("[UI] settings display hz="); dbg64_str(rb);
                dbg64_str("Hz source=edid preferred-timing");
                dbg64_nl();
                dbg64_line_end64();
            } else {
                logln("[UI] settings display hz=unknown source=none (no EDID from firmware)");
            }
        }
        ry += L->row_h;
    }
    // 3) 缩放（三段选择）
    {
        int rx = 0;
        row_label(L, ry, zh ? "缩放" : "Zoom",
                  zh ? "渲染分辨率 = 物理 / 缩放（fb_set_zoom 真设置，持久化）" : "render = physical / zoom", &rx);
        const char* items[SET_ZOOM_N] = { "100%", "125%", "150%" };
        const int w = 220, h = 32, bx = rx - w;
        draw_choice(bx, ry + (L->row_h - h) / 2, w, h, items, SET_ZOOM_N, zoom_sel_from_actual(), CID_ZOOM_100);
        ctl_reg(P_DISPLAY, CID_ZOOM_100, CK_CHOICE, bx, ry + (L->row_h - h) / 2, w / 3, h);
        ctl_reg(P_DISPLAY, CID_ZOOM_125, CK_CHOICE, bx + w / 3, ry + (L->row_h - h) / 2, w / 3, h);
        ctl_reg(P_DISPLAY, CID_ZOOM_150, CK_CHOICE, bx + 2 * (w / 3), ry + (L->row_h - h) / 2, w / 3, h);
        ry += L->row_h;
    }
    // 4) 应用（幂等：再应用一次当前选中）
    {
        const int bw = 120, bh = 32;
        const int bx = L->card_x + L->card_w - 16 - bw;
        draw_button(bx, ry + (L->row_h - bh) / 2, bw, bh, zh ? "应用" : "Apply", 1,
                    g_hover == CID_DISPLAY_APPLY, g_press == CID_DISPLAY_APPLY);
        ctl_reg(P_DISPLAY, CID_DISPLAY_APPLY, CK_BUTTON, bx, ry + (L->row_h - bh) / 2, bw, bh);
        if (msg_fresh()) ui_text(L->card_x + 16, ry + (L->row_h - ui_h()) / 2, g_msg, g_msg_col);
        ry += L->row_h;
    }
    // 卡片 2：设备规格摘要（[UI] settings specs 行的数据源）
    y = c1y + c1h + 12;
    const int c2h = card_rows_h(L, 5);
    card_begin(L, y, c2h);
    int ry2 = y + 12;
    {
        Buf b; b_init(&b);
        b_str(&b, zh ? "渲染 " : "render ");
        b_int(&b, fb_width()); b_str(&b, " x "); b_int(&b, fb_height());
        b_str(&b, zh ? "   缩放 " : "   zoom "); b_int(&b, fb_get_zoom()); b_ch(&b, '%');
        ui_text(L->card_x + 16, ry2, b.b, t->text); ry2 += L->row_h;
    }
    {
        const Edid64* ed = edid64_get();
        Buf b; b_init(&b);
        if (ed->valid) {
            b_str(&b, "EDID "); b_int(&b, ed->version_major); b_ch(&b, '.'); b_int(&b, ed->version_minor);
            b_str(&b, zh ? "   厂商 " : "   mfg "); b_str(&b, ed->mfg);
            if (ed->size_cm_w && ed->size_cm_h) {
                b_str(&b, zh ? "   尺寸 " : "   size ");
                b_int(&b, ed->size_cm_w); b_str(&b, "x"); b_int(&b, ed->size_cm_h); b_str(&b, "cm");
            }
            b_str(&b, zh ? "   首选 " : "   preferred ");
            b_int(&b, ed->h_active); b_ch(&b, 'x'); b_int(&b, ed->v_active);
        } else {
            b_str(&b, zh ? "EDID 无（刷新率/显示器名不可知）" : "no EDID (refresh/monitor unknown)");
        }
        ui_text(L->card_x + 16, ry2, b.b, t->text_dim); ry2 += L->row_h;
    }
    {
        uint64_t ram = mem_total_ram_64();
        Buf b; b_init(&b);
        b_str(&b, zh ? "内存 " : "RAM ");
        b_u64(&b, ram / (1024 * 1024)); b_str(&b, zh ? " MB（E820 可用上界）" : " MB (E820 top)");
        ui_text(L->card_x + 16, ry2, b.b, t->text_dim); ry2 += L->row_h;
    }
    {
        int w = 0, hh = 0;
        gfx64_wall_surface64(&w, &hh);
        Buf b; b_init(&b);
        b_str(&b, zh ? "壁纸面 " : "wall surface ");
        b_int(&b, w); b_str(&b, "x"); b_int(&b, hh);
        b_str(&b, "   src="); b_str(&b, gfx64_wall_src_desc64());
        ui_text(L->card_x + 16, ry2, b.b, t->text_dim); ry2 += L->row_h;
    }
    {
        const Disp64Info* di = display64_info64();
        Buf b; b_init(&b);
        b_str(&b, zh ? "刷新率来源 " : "refresh src ");
        b_str(&b, display64_src_name64(di->src));
        b_str(&b, "   modes="); b_int(&b, display64_mode_count64());
        b_str(&b, zh ? "（可用模式清单）" : " (mode list)");
        ui_text(L->card_x + 16, ry2, b.b, t->text_dim);
    }
}

static void page_sound(const Lay* L, int x0, int y0) {
    const Theme64Tokens* t = tk64();
    const bool zh = gui64_lang_zh();
    page_title(L, x0 + L->cx, y0, zh ? "声音" : "Sound", zh ? "音量与输出源" : "Volume and output");
    int y = L->card_y;
    const int c1h = card_rows_h(L, 4);
    card_begin(L, y, c1h);
    int ry = y + 12;
    // 音量滑块
    {
        int rx = 0;
        row_label(L, ry, zh ? "音量" : "Volume", nullptr, &rx);
        Buf b; b_init(&b);
        b_int(&b, panels64_volume64()); b_ch(&b, '%');
        ui_text(rx - ui_w(b.b), ry + (L->row_h - ui_h()) / 2, b.b, t->text);
        const int sw = 260, sx = rx - ui_w(b.b) - 16 - sw;
        const int sy = ry + L->row_h / 2 - THEME64_PANEL_KNOB / 2;
        draw_slider(L, sx, sy, sw, panels64_volume64(), 100, g_hover == CID_SND_VOL, g_drag == CID_SND_VOL);
        ctl_reg(P_SOUND, CID_SND_VOL, CK_SLIDER, sx, ry + 6, sw, L->row_h - 12);
        ry += L->row_h;
    }
    // 输出源
    {
        int rx = 0;
        row_label(L, ry, zh ? "输出源" : "Output", zh ? "音箱 / 耳机（只改内存态）" : "speaker / headphones", &rx);
        const char* items[2] = { zh ? "音箱" : "Speaker", zh ? "耳机" : "Headphones" };
        const int w = 220, h = 32, bx = rx - w;
        draw_choice(bx, ry + (L->row_h - h) / 2, w, h, items, 2, panels64_src_name64()[0] == 'h' ? 1 : 0, CID_SND_SPK);
        ctl_reg(P_SOUND, CID_SND_SPK, CK_CHOICE, bx, ry + (L->row_h - h) / 2, w / 2, h);
        ctl_reg(P_SOUND, CID_SND_HP, CK_CHOICE, bx + w / 2, ry + (L->row_h - h) / 2, w / 2, h);
        ry += L->row_h;
        ry += L->row_h;
    }
    {
        ui_text(L->card_x + 16, ry + 4,
                zh ? "注意：音频驱动未实现（没有 HDA/AC97 驱动）：音量和输出源只保存在内存 + config64，"
                     "绝不写硬件，也不会有声音。"
                   : "audio driver not implemented yet: values are memory/config only.",
                t->accent2);
        ry += L->row_h;
    }
    {
        Buf b; b_init(&b);
        b_str(&b, zh ? "打点：" : "log: ");
        b_str(&b, "audio driver not implemented yet");
        b_str(&b, zh ? "（[PANEL64]/[SET64] 都明写，绝不假装生效）" : " ([PANEL64]/[SET64])");
        ui_text(L->card_x + 16, ry + 4, b.b, t->text_dim);
    }
}

static void page_power(const Lay* L, int x0, int y0) {
    const bool zh = gui64_lang_zh();
    const Theme64Tokens* t = tk64();
    page_title(L, x0 + L->cx, y0, zh ? "电源" : "Power", zh ? "关机 / 重启 / 锁定" : "Shut down / Restart / Lock");
    int y = L->card_y;
    const int c1h = card_rows_h(L, 3) + 20;
    card_begin(L, y, c1h);
    const int bw = 150, bh = 40;
    const int gap = 14;
    int bx = L->card_x + 16;
    const int by = y + 24;
    draw_button(bx, by, bw, bh, zh ? "关机" : "Shut down", 0,
                g_hover == CID_PWR_SHUTDOWN, g_press == CID_PWR_SHUTDOWN);
    ctl_reg(P_POWER, CID_PWR_SHUTDOWN, CK_BUTTON, bx, by, bw, bh);
    bx += bw + gap;
    draw_button(bx, by, bw, bh, zh ? "重启" : "Restart", 0,
                g_hover == CID_PWR_REBOOT, g_press == CID_PWR_REBOOT);
    ctl_reg(P_POWER, CID_PWR_REBOOT, CK_BUTTON, bx, by, bw, bh);
    bx += bw + gap;
    draw_button(bx, by, bw, bh, zh ? "锁定" : "Lock", 1,
                g_hover == CID_PWR_LOCK, g_press == CID_PWR_LOCK);
    ctl_reg(P_POWER, CID_PWR_LOCK, CK_BUTTON, bx, by, bw, bh);
    ui_text(L->card_x + 16, y + 24 + bh + 14,
            zh ? "关机 = ACPI S5（acpi_poweroff64，失败会如实回落到 QEMU shutdown）"
               : "shut down = ACPI S5",
            t->text_dim);
    ui_text(L->card_x + 16, y + 24 + bh + 36,
            zh ? "重启 = 键盘控制器 0xCF9 / 三重故障路径（与开始菜单、终端 reboot 同一实现）"
               : "restart = 0xCF9 reset port",
            t->text_dim);
    ui_text(L->card_x + 16, y + 24 + bh + 58,
            zh ? "锁定 = 立即回锁屏（[LOCK64] lock requested by=settings）" : "lock = back to lock screen",
            t->text_dim);
}

static void page_defapp(const Lay* L, int x0, int y0) {
    const bool zh = gui64_lang_zh();
    const Theme64Tokens* t = tk64();
    page_title(L, x0 + L->cx, y0, zh ? "默认应用" : "Default apps",
               zh ? "按类型/扩展名选一个应用（持久化 + 打点）" : "type -> app mapping");
    const char* kinds_zh[CFG64_DEFK_COUNT] = { ".elf 可执行", ".vap 应用", ".txt 文本", "视频类（.mp4/.avi）" };
    const char* kinds_en[CFG64_DEFK_COUNT] = { ".elf files", ".vap apps", ".txt text", "video (.mp4/.avi)" };
    int y = L->card_y;
    const int c1h = card_rows_h(L, CFG64_DEFK_COUNT) ;
    card_begin(L, y, c1h);
    int ry = y + 12;
    for (int k = 0; k < CFG64_DEFK_COUNT; k++) {
        int rx = 0;
        row_label(L, ry, zh ? kinds_zh[k] : kinds_en[k], nullptr, &rx);
        char cur[16];
        cfg64_defapp64(k, cur, (int)sizeof(cur));
        int sel = DEFAPP_N - 1;
        for (int i = 0; i < DEFAPP_N; i++) if (s_eq(cur, kDefApps[i].key)) { sel = i; break; }
        const char* items[DEFAPP_N];
        for (int i = 0; i < DEFAPP_N; i++) items[i] = zh ? kDefApps[i].zh : kDefApps[i].en;
        const int w = 420, h = 32, bx = rx - w;
        // 分段 id 编码：CID_DEF_ELF + kind*4 + seg（kind = 类型行，seg = 应用项）
        draw_choice(bx, ry + (L->row_h - h) / 2, w, h, items, DEFAPP_N, sel,
                    CID_DEF_ROW + k * DEFAPP_N);
        for (int i = 0; i < DEFAPP_N; i++) {
            const int seg = w / DEFAPP_N;
            ctl_reg(P_DEFAPP, CID_DEF_ROW + k * DEFAPP_N + i, CK_CHOICE,
                    bx + i * seg, ry + (L->row_h - h) / 2, seg, h);
        }
        ry += L->row_h;
    }
    ui_text(L->card_x + 16, ry + 4,
            zh ? "如实标注：这套映射现在只做「持久化 + 打点」，系统还没有按扩展名打开的关联引擎"
                 "（文件管理器/终端不会据此自动启动应用）。"
                 : "honest note: mapping is persisted + logged only; no shell association engine yet.",
            t->text_dim);
}

static void page_theme(const Lay* L, int x0, int y0) {
    const Theme64Tokens* t = tk64();
    const bool zh = gui64_lang_zh();
    page_title(L, x0 + L->cx, y0, zh ? "主题" : "Themes",
               zh ? "7 套主题：白/暗/蓝白/粉白/粉绿/粉紫/紫白（圆角/阴影/动效走 Token）"
                  : "7 themes (all geometry from tokens)");
    int y = L->card_y;
    const int per_row = 4;
    const int rows = (THEME64_THEME_COUNT + per_row - 1) / per_row;
    const int tw = (L->card_w - 32 - (per_row - 1) * 12) / per_row;
    const int c1h = rows * (FONT_TILE_H + 12) + 20;
    card_begin(L, y, c1h);
    for (int i = 0; i < THEME64_THEME_COUNT; i++) {
        const int cx = L->card_x + 16 + (i % per_row) * (tw + 12);
        const int cy = y + 12 + (i / per_row) * (FONT_TILE_H + 12);
        draw_theme_tile(cx, cy, tw, FONT_TILE_H, i);
        ctl_reg(P_THEME, CID_THEME_TILE0 + i, CK_CHOICE, cx, cy, tw, FONT_TILE_H);
    }
    // 第二张卡：减少动画
    const int y2 = y + c1h + 12;
    const int c2h = card_rows_h(L, 2);
    card_begin(L, y2, c2h);
    {
        int ry = y2 + 12;
        int rx = 0;
        row_label(L, ry, zh ? "减少动画" : "Reduce motion",
                  zh ? "动效退化到 0ms / 1 帧到位（Token 语义）" : "all animations -> 0ms", &rx);
        const int sw = 44, sh = 22;
        const int sx = rx - sw;
        const int sy = ry + (L->row_h - sh) / 2;
        draw_switch(sx, sy, theme64_reduce_motion64() ? 1 : 0, g_hover == CID_THEME_REDUCE, g_press == CID_THEME_REDUCE);
        ctl_reg(P_THEME, CID_THEME_REDUCE, CK_SWITCH, sx, sy, sw, sh);
        ry += L->row_h;
        Buf b; b_init(&b);
        b_str(&b, zh ? "当前主题：" : "current theme ");
        b_int(&b, theme64_id64()); b_str(&b, " ");
        b_str(&b, theme64_name64(theme64_id64()));
        b_str(&b, zh ? "   持久化键 ui.theme（重启保持）" : "   persisted ui.theme");
        ui_text(L->card_x + 16, ry, b.b, t->text_dim);
    }
}

static void page_wall(const Lay* L, int x0, int y0) {
    const Theme64Tokens* t = tk64();
    const bool zh = gui64_lang_zh();
    page_title(L, x0 + L->cx, y0, zh ? "壁纸" : "Wallpaper",
               zh ? "自定义背景图（VimtuFS2 路径；空 = 内置兜底）" : "custom image path (empty = builtin)");
    int y = L->card_y;
    const int c1h = card_rows_h(L, 4) + 8;
    card_begin(L, y, c1h);
    int ry = y + 12;
    char wp[CFG64_STR_MAX];
    cfg64_wall_path64(wp, (int)sizeof(wp));
    {
        // 当前壁纸来源（真值：gfx64 的记录）
        Buf b; b_init(&b);
        b_str(&b, zh ? "当前来源 " : "current ");
        b_str(&b, gfx64_wall_src_desc64());
        b_str(&b, "   ");
        b_int(&b, gfx64_wall_src_w64()); b_ch(&b, 'x'); b_int(&b, gfx64_wall_src_h64());
        b_str(&b, zh ? (wp[0] ? "（来自文件）" : "（内置兜底 / 自定义渐变）")
                     : (wp[0] ? " (file)" : " (builtin/gradient)"));
        ui_text(L->card_x + 16, ry + 4, b.b, t->text); ry += L->row_h;
    }
    {
        // 路径输入框 + 应用
        int rx = 0;
        row_label(L, ry, zh ? "图片路径" : "Image path",
                  zh ? "PNG/BMP（JPEG 未实现，会如实报错）" : "PNG/BMP (JPEG not implemented)", &rx);
        const int fh = 32, fw = 300;
        const int fx = rx - 120 - 10 - fw;
        const int fy = ry + (L->row_h - fh) / 2;
        draw_field(fx, fy, fw, fh, g_focus == CID_WALL_FIELD ? g_input : wp,
                   g_focus == CID_WALL_FIELD, g_hover == CID_WALL_FIELD);
        ctl_reg(P_WALL, CID_WALL_FIELD, CK_FIELD, fx, fy, fw, fh);
        const int bw = 110;
        draw_button(rx - bw, fy, bw, fh, zh ? "应用" : "Apply", 1,
                    g_hover == CID_WALL_APPLY, g_press == CID_WALL_APPLY);
        ctl_reg(P_WALL, CID_WALL_APPLY, CK_BUTTON, rx - bw, fy, bw, fh);
        ry += L->row_h;
    }
    {
        // 内置兜底 + 预设（真文件：/logo/kaisi.png 由外壳装进 VimtuFS2）
        const int bw = 170, bh = 32;
        int bx = L->card_x + 16;
        draw_button(bx, ry + (L->row_h - bh) / 2, bw, bh, zh ? "内置兜底壁纸" : "Built-in wallpaper", 0,
                    g_hover == CID_WALL_BUILTIN, g_press == CID_WALL_BUILTIN);
        ctl_reg(P_WALL, CID_WALL_BUILTIN, CK_BUTTON, bx, ry + (L->row_h - bh) / 2, bw, bh);
        bx += bw + 10;
        draw_button(bx, ry + (L->row_h - bh) / 2, 220, bh, zh ? "用 /logo/kaisi.png" : "Use /logo/kaisi.png", 0,
                    g_hover == CID_WALL_PRESET, g_press == CID_WALL_PRESET);
        ctl_reg(P_WALL, CID_WALL_PRESET, CK_BUTTON, bx, ry + (L->row_h - bh) / 2, 220, bh);
        ry += L->row_h;
    }
    ui_text(L->card_x + 16, ry + 4,
            zh ? "适应模式（填充/适应/拉伸/平铺/居中/跨屏）在「壁纸适应模式」页设置；本页只管图片来源。"
               : "fit mode lives on the 'Wallpaper fit' page.",
            t->text_dim);
}

static void page_color(const Lay* L, int x0, int y0) {
    const Theme64Tokens* t = tk64();
    const bool zh = gui64_lang_zh();
    page_title(L, x0 + L->cx, y0, zh ? "颜色" : "Colors",
               zh ? "自定义渐变壁纸的两端色（#RRGGBB）" : "custom gradient endpoints (#RRGGBB)");
    char ha[16], hb[16];
    cfg64_grad_a64(ha, (int)sizeof(ha));
    cfg64_grad_b64(hb, (int)sizeof(hb));
    int y = L->card_y;
    const int c1h = card_rows_h(L, 4);
    card_begin(L, y, c1h);
    int ry = y + 12;
    {
        int rx = 0;
        row_label(L, ry, zh ? "使用自定义渐变壁纸" : "Use custom gradient wallpaper",
                  zh ? "打开后桌面壁纸 = 下面两端色的对角渐变（壁纸文件优先）" : "desktop = gradient of the two colors",
                  &rx);
        const int sw = 44, sh = 22, sx = rx - sw, sy = ry + (L->row_h - sh) / 2;
        draw_switch(sx, sy, cfg64_grad_on64(), g_hover == CID_GRAD_ON, g_press == CID_GRAD_ON);
        ctl_reg(P_COLOR, CID_GRAD_ON, CK_SWITCH, sx, sy, sw, sh);
        ry += L->row_h;
    }
    uint32_t va = 0x5AA9F0u, vb = 0xC9A7FFu;
    (void)parse_hex_color(ha, &va);
    (void)parse_hex_color(hb, &vb);
    for (int which = 0; which < 2; which++) {
        int rx = 0;
        row_label(L, ry, which == 0 ? (zh ? "渐变起点色" : "Gradient start")
                                    : (zh ? "渐变终点色" : "Gradient end"),
                  which == 0 ? ha : hb, &rx);
        const int fh = 32, fw = 200;
        const int fx = rx - 110 - 10 - fw;
        const int fy = ry + (L->row_h - fh) / 2;
        const int fid = which == 0 ? CID_GRAD_A_FIELD : CID_GRAD_B_FIELD;
        char shown[16];
        int i = 0;
        const char* src = (g_focus == fid) ? g_input : (which == 0 ? ha : hb);
        for (; src[i] && i < (int)sizeof(shown) - 1; i++) shown[i] = src[i];
        shown[i] = 0;
        draw_field(fx, fy, fw, fh, shown, g_focus == fid, g_hover == fid);
        ctl_reg(P_COLOR, fid, CK_FIELD, fx, fy, fw, fh);
        const int bw = 110, bid = which == 0 ? CID_GRAD_A_APPLY : CID_GRAD_B_APPLY;
        draw_button(rx - bw, fy, bw, fh, zh ? "应用" : "Apply", 1, g_hover == bid, g_press == bid);
        ctl_reg(P_COLOR, bid, CK_BUTTON, rx - bw, fy, bw, fh);
        ry += L->row_h;
    }
    ui_text(L->card_x + 16, ry + 4,
            zh ? "预览（左 = 起点色，右 = 终点色，对角渐变与壁纸生成算法一致）"
               : "preview (diagonal gradient, same algorithm as wallpaper)",
            t->text_dim);
    // 预览条 + 恢复默认
    const int pw = L->card_w - 32 - 150, ph = 40;
    const int px = L->card_x + 16, py = ry + 26;
    if (py + ph < y + c1h) {
        gfx64_grad_round64(px, py, pw, ph, THEME64_R_BUTTON, va, vb, 1, 255);
        stroke_r(px, py, pw, ph, THEME64_R_BUTTON, t->win_frame, THEME64_A_BORDER);
        const int bw = 140, bh = 32;
        const int bx = L->card_x + L->card_w - 16 - bw;
        const int by = py + (ph - bh) / 2;
        draw_button(bx, by, bw, bh, zh ? "恢复默认" : "Reset", 0, g_hover == CID_GRAD_RESET, g_press == CID_GRAD_RESET);
        ctl_reg(P_COLOR, CID_GRAD_RESET, CK_BUTTON, bx, by, bw, bh);
    }
}

static void page_fit(const Lay* L, int x0, int y0) {
    const Theme64Tokens* t = tk64();
    const bool zh = gui64_lang_zh();
    page_title(L, x0 + L->cx, y0, zh ? "壁纸适应模式" : "Wallpaper fit",
               zh ? "桌面与锁屏可分别设置（默认填充）" : "desktop and lock screen, separately");
    const char* names_zh[GFX64_WALL_MODE_COUNT] = { "填充", "适应", "拉伸", "平铺", "居中", "跨屏" };
    const char* names_en[GFX64_WALL_MODE_COUNT] = { "Fill", "Fit", "Stretch", "Tile", "Center", "Span" };
    const char* items[GFX64_WALL_MODE_COUNT];
    for (int i = 0; i < GFX64_WALL_MODE_COUNT; i++) items[i] = zh ? names_zh[i] : names_en[i];
    int y = L->card_y;
    // 卡 1：同步开关
    const int c1h = card_rows_h(L, 1);
    card_begin(L, y, c1h);
    {
        int ry = y + 12, rx = 0;
        row_label(L, ry, zh ? "同步（桌面/锁屏用同一模式）" : "Sync desktop & lock screen",
                  zh ? "关闭后下面两张卡分别独立生效" : "turn off to set them separately", &rx);
        const int sw = 44, sh = 22, sx = rx - sw, sy = ry + (L->row_h - sh) / 2;
        draw_switch(sx, sy, cfg64_wall_sync64(), g_hover == CID_FIT_SYNC, g_press == CID_FIT_SYNC);
        ctl_reg(P_FIT, CID_FIT_SYNC, CK_SWITCH, sx, sy, sw, sh);
    }
    // 卡 2：桌面
    const int y2 = y + c1h + 12;
    const int c2h = card_rows_h(L, 2);
    card_begin(L, y2, c2h);
    {
        int ry = y2 + 12, rx = 0;
        row_label(L, ry, zh ? "桌面壁纸" : "Desktop wallpaper", nullptr, &rx);
        const int w = 480, h = 32, bx = rx - w;
        draw_choice(bx, ry + (L->row_h - h) / 2, w, h, items, GFX64_WALL_MODE_COUNT,
                    gfx64_wall_mode64(), CID_WALLFIT_BASE);
        for (int i = 0; i < GFX64_WALL_MODE_COUNT; i++) {
            const int seg = w / GFX64_WALL_MODE_COUNT;
            ctl_reg(P_FIT, CID_WALLFIT_BASE + i, CK_CHOICE, bx + i * seg,
                    ry + (L->row_h - h) / 2, seg, h);
        }
        ry += L->row_h;
        Buf b; b_init(&b);
        b_str(&b, zh ? "当前桌面模式 " : "desktop mode ");
        b_int(&b, gfx64_wall_mode64()); b_str(&b, " "); b_str(&b, gfx64_wall_mode_name64(gfx64_wall_mode64()));
        b_str(&b, "    ");
        b_str(&b, gfx64_wall_last_geom64());
        ui_text(L->card_x + 16, ry + 2, b.b, t->text_dim);
    }
    // 卡 3：锁屏
    const int y3 = y2 + c2h + 12;
    const int c3h = card_rows_h(L, 2);
    card_begin(L, y3, c3h);
    {
        int ry = y3 + 12, rx = 0;
        row_label(L, ry, zh ? "锁屏壁纸" : "Lock screen wallpaper",
                  zh ? "锁屏下次绘制时生效（[LOCK64] wall src=.. lock_mode=..）" : "applied when the lock screen paints",
                  &rx);
        const int w = 480, h = 32, bx = rx - w;
        draw_choice(bx, ry + (L->row_h - h) / 2, w, h, items, GFX64_WALL_MODE_COUNT,
                    cfg64_lock_wall_mode64(), CID_LOCKFIT_BASE);
        for (int i = 0; i < GFX64_WALL_MODE_COUNT; i++) {
            const int seg = w / GFX64_WALL_MODE_COUNT;
            ctl_reg(P_FIT, CID_LOCKFIT_BASE + i, CK_CHOICE, bx + i * seg,
                    ry + (L->row_h - h) / 2, seg, h);
        }
        ry += L->row_h;
        Buf b; b_init(&b);
        b_str(&b, zh ? "当前锁屏模式 " : "lock mode ");
        b_int(&b, cfg64_lock_wall_mode64()); b_str(&b, " ");
        b_str(&b, gfx64_wall_mode_name64(cfg64_lock_wall_mode64()));
        b_str(&b, zh ? "   同步=" : "   sync="); b_int(&b, cfg64_wall_sync64());
        ui_text(L->card_x + 16, ry + 2, b.b, t->text_dim);
    }
}

static void page_dock(const Lay* L, int x0, int y0) {
    const Theme64Tokens* t = tk64();
    const bool zh = gui64_lang_zh();
    page_title(L, x0 + L->cx, y0, zh ? "Dock 栏" : "Dock",
               zh ? "长度（横向宽度）/ 图标尺寸 / 图标间距，实时生效" : "length / icon size / gap, live");
    int y = L->card_y;
    const int c1h = card_rows_h(L, 5) + 16;
    card_begin(L, y, c1h);
    int ry = y + 12;
    // 长度
    {
        int rx = 0;
        row_label(L, ry, zh ? "长度（横向宽度）" : "Length (width)",
                  zh ? "0 = 自动（按图标数/尺寸/间距算）" : "0 = auto", &rx);
        Buf b; b_init(&b);
        const int len = cfg64_dock_len64();
        if (len > 0) { b_str(&b, zh ? "固定 " : "fixed "); b_int(&b, len); b_str(&b, " px"); }
        else b_str(&b, zh ? "自动" : "auto");
        ui_text(rx - ui_w(b.b), ry + (L->row_h - ui_h()) / 2, b.b, t->text);
        const int sw = 240, sx = rx - ui_w(b.b) - 16 - sw;
        const int sy = ry + L->row_h / 2 - THEME64_PANEL_KNOB / 2;
        draw_slider(L, sx, sy, sw, len > 0 ? (len - 320) / 8 : 0, 100,
                    g_hover == CID_DOCK_LEN, g_drag == CID_DOCK_LEN);
        ctl_reg(P_DOCK, CID_DOCK_LEN, CK_SLIDER, sx, ry + 6, sw, L->row_h - 12);
        ry += L->row_h;
    }
    // 大小（图标尺寸 + 间距联动）
    {
        int rx = 0;
        row_label(L, ry, zh ? "大小（图标尺寸）" : "Size (icon)",
                  zh ? "44..48 px（Token 默认 46）；高度由 Token 固定 60" : "44..48 px", &rx);
        Buf b; b_init(&b);
        b_int(&b, cfg64_dock_icon64()); b_str(&b, " px");
        ui_text(rx - ui_w(b.b), ry + (L->row_h - ui_h()) / 2, b.b, t->text);
        const int sw = 240, sx = rx - ui_w(b.b) - 16 - sw;
        const int sy = ry + L->row_h / 2 - THEME64_PANEL_KNOB / 2;
        draw_slider(L, sx, sy, sw, cfg64_dock_icon64() - 44, 4, g_hover == CID_DOCK_ICON, g_drag == CID_DOCK_ICON);
        ctl_reg(P_DOCK, CID_DOCK_ICON, CK_SLIDER, sx, ry + 6, sw, L->row_h - 12);
        ry += L->row_h;
    }
    // 间距
    {
        int rx = 0;
        row_label(L, ry, zh ? "图标间距" : "Icon gap", zh ? "4..24 px（Token 默认 11）" : "4..24 px", &rx);
        Buf b; b_init(&b);
        b_int(&b, cfg64_dock_gap64()); b_str(&b, " px");
        ui_text(rx - ui_w(b.b), ry + (L->row_h - ui_h()) / 2, b.b, t->text);
        const int sw = 240, sx = rx - ui_w(b.b) - 16 - sw;
        const int sy = ry + L->row_h / 2 - THEME64_PANEL_KNOB / 2;
        draw_slider(L, sx, sy, sw, cfg64_dock_gap64() - 4, 20, g_hover == CID_DOCK_GAP, g_drag == CID_DOCK_GAP);
        ctl_reg(P_DOCK, CID_DOCK_GAP, CK_SLIDER, sx, ry + 6, sw, L->row_h - 12);
        ry += L->row_h;
    }
    {
        // 恢复默认（Token 值）
        const int bw = 150, bh = 32;
        const int bx = L->card_x + L->card_w - 16 - bw;
        draw_button(bx, ry + (L->row_h - bh) / 2, bw, bh, zh ? "恢复默认" : "Reset to tokens", 0,
                    g_hover == CID_DOCK_RESET, g_press == CID_DOCK_RESET);
        ctl_reg(P_DOCK, CID_DOCK_RESET, CK_BUTTON, bx, ry + (L->row_h - bh) / 2, bw, bh);
        ry += L->row_h;
    }
    {
        Buf b; b_init(&b);
        b_str(&b, zh ? "实际生效：" : "in effect: ");
        b_str(&b, "dock.len="); b_int(&b, cfg64_dock_len64());
        b_str(&b, " dock.icon="); b_int(&b, cfg64_dock_icon64());
        b_str(&b, " dock.gap="); b_int(&b, cfg64_dock_gap64());
        b_str(&b, " dock.size="); b_int(&b, cfg64_dock_size64());
        b_str(&b, zh ? "（外壳 Dock 高度按 Token THEME64_DOCK_H=60 绘制；dock.size 已持久化）"
                     : " (dock height fixed by token)");
        ui_text(L->card_x + 16, ry + 2, b.b, t->text_dim);
        ui_text(L->card_x + 16, ry + 2 + ui_h() + 2,
                zh ? "每次改动都会重新算几何并再打一行 [DOCK64] geom（w/icon/gap 变化可核对）"
                   : "each change re-runs [DOCK64] geom",
                t->text_dim);
    }
}

static void page_font(const Lay* L, int x0, int y0) {
    const Theme64Tokens* t = tk64();
    const bool zh = gui64_lang_zh();
    page_title(L, x0 + L->cx, y0, zh ? "字体大小" : "Text size",
               zh ? "三档：小 14 / 中 16 / 大 18（全系统 TrueType 文本）" : "3 steps: 14 / 16 / 18");
    int y = L->card_y;
    const int c1h = card_rows_h(L, 3) + 10;
    card_begin(L, y, c1h);
    int ry = y + 12;
    const char* items[SET_FONT_N] = { zh ? "小 (14)" : "Small (14)", zh ? "中 (16)" : "Medium (16)",
                                      zh ? "大 (18)" : "Large (18)" };
    int sel = 1;
    for (int i = 0; i < SET_FONT_N; i++) if (kFontPx[i] == font_get_size64()) sel = i;
    {
        int rx = 0;
        row_label(L, ry, zh ? "字号档" : "Size", zh ? "立即改渲染字号（不等重启）" : "applies immediately", &rx);
        const int w = 330, h = 34, bx = rx - w;
        draw_choice(bx, ry + (L->row_h - h) / 2, w, h, items, SET_FONT_N, sel, CID_FONT_BASE);
        for (int i = 0; i < SET_FONT_N; i++) {
            const int seg = w / SET_FONT_N;
            ctl_reg(P_FONT, CID_FONT_BASE + i, CK_CHOICE, bx + i * seg, ry + (L->row_h - h) / 2, seg, h);
        }
        ry += L->row_h;
    }
    {
        // 预览：同一句话用当前字号渲染
        int rx = 0;
        row_label(L, ry, zh ? "预览" : "Preview", nullptr, &rx);
        ui_text(rx - ui_w(zh ? "设置 VimtuOS 字体预览" : "VimtuOS settings preview") ,
                ry + (L->row_h - ui_h()) / 2, zh ? "设置 VimtuOS 字体预览" : "VimtuOS settings preview", t->text);
        ry += L->row_h;
    }
    {
        Buf b; b_init(&b);
        b_str(&b, zh ? "当前 em=" : "em="); b_int(&b, font_get_size64());
        b_str(&b, zh ? "px 行高=" : "px line="); b_int(&b, font_line_height());
        b_str(&b, zh ? "px   持久化键 ui.font.size（启动期由 settings64_boot_apply64 生效）"
                     : "px   persisted ui.font.size");
        ui_text(L->card_x + 16, ry + 4, b.b, t->text_dim);
    }
}

static void page_eth(const Lay* L, int x0, int y0) {
    const Theme64Tokens* t = tk64();
    const bool zh = gui64_lang_zh();
    page_title(L, x0 + L->cx, y0, zh ? "以太网" : "Ethernet",
               zh ? "e1000（Intel 82540EM）真实链路状态" : "e1000 real link state");
    int y = L->card_y;
    const int c1h = card_rows_h(L, 6) + 10;
    card_begin(L, y, c1h);
    int ry = y + 12;
    const uint8_t* mac = e1000_mac64();
    {
        int rx = 0;
        row_label(L, ry, zh ? "适配器" : "Adapter",
                  zh ? "PCI 扫描命中的 Intel 网卡（hwinfo64 真枚举）" : "PCI enumerated Intel NIC", &rx);
        const HwInfo64* hw = hw_info64();
        Buf b; b_init(&b);
        bool found = false;
        if (hw->magic == HW64_INFO_MAGIC) {
            for (uint32_t i = 0; i < hw->pci.count; i++) {
                const HwPciDev64* d = &hw->pci.devs[i];
                if (d->class_code == 0x02 && d->subclass == 0x00) {
                    b_str(&b, "Intel 82540EM @");
                    b_int(&b, d->bus); b_str(&b, ":"); b_int(&b, d->dev); b_str(&b, "."); b_int(&b, d->fn);
                    b_str(&b, "  vendor="); b_hex(&b, d->vendor, 4);
                    found = true;
                    break;
                }
            }
        }
        if (!found) b_str(&b, net64_state_str64());
        ui_text(rx - ui_w(b.b), ry + (L->row_h - ui_h()) / 2, b.b, t->text);
        ry += L->row_h;
    }
    {
        int rx = 0;
        const int link = e1000_link64();
        row_label(L, ry, zh ? "链路状态" : "Link", zh ? "真读 e1000 STATUS.LU（不是猜）" : "read from STATUS.LU", &rx);
        Buf b; b_init(&b);
        b_str(&b, link ? (zh ? "已连接（link up）" : "link up") : (zh ? "未连接（link down）" : "link down"));
        b_str(&b, "   state="); b_str(&b, net64_state_str64());
        ui_text(rx - ui_w(b.b), ry + (L->row_h - ui_h()) / 2, b.b, link ? t->accent : t->accent2);
        ry += L->row_h;
    }
    {
        int rx = 0;
        row_label(L, ry, zh ? "MAC 地址" : "MAC", nullptr, &rx);
        Buf b; b_init(&b);
        if (mac) {
            static const char* const HD = "0123456789abcdef";
            for (int i = 0; i < 6; i++) {
                b_ch(&b, HD[mac[i] >> 4]); b_ch(&b, HD[mac[i] & 0xF]);
                if (i != 5) b_ch(&b, ':');
            }
        } else b_str(&b, zh ? "无（驱动未初始化）" : "none");
        ui_text(rx - ui_w(b.b), ry + (L->row_h - ui_h()) / 2, b.b, t->text);
        ry += L->row_h;
    }
    {
        int rx = 0;
        row_label(L, ry, zh ? "IPv4 配置" : "IPv4", zh ? "静态配置（无 DHCP）：net64.h 的 QEMU 用户网络口径" : "static, no DHCP", &rx);
        Buf b; b_init(&b);
        b_str(&b, "10.0.2.15/24  gw 10.0.2.2  dns 10.0.2.3");
        ui_text(rx - ui_w(b.b), ry + (L->row_h - ui_h()) / 2, b.b, t->text);
        ry += L->row_h;
    }
    {
        Buf b; b_init(&b);
        b_str(&b, zh ? "帧计数：发送 " : "frames: tx ");
        b_u64(&b, net64_tx_frames64());
        b_str(&b, zh ? "  接收 " : "  rx "); b_u64(&b, net64_rx_frames64());
        b_str(&b, "  ARP "); b_u64(&b, net64_arp_replies64());
        b_str(&b, "  ICMP "); b_u64(&b, net64_icmp_replies64());
        b_str(&b, zh ? "  丢弃 " : "  dropped "); b_u64(&b, net64_dropped64());
        ui_text(L->card_x + 16, ry, b.b, t->text_dim);
        ry += L->row_h;
    }
    {
        const int bw = 150, bh = 32;
        const int bx = L->card_x + L->card_w - 16 - bw;
        draw_button(bx, ry + (L->row_h - bh) / 2, bw, bh, zh ? "刷新" : "Refresh", 1,
                    g_hover == CID_NET_REFRESH, g_press == CID_NET_REFRESH);
        ctl_reg(P_ETH, CID_NET_REFRESH, CK_BUTTON, bx, ry + (L->row_h - bh) / 2, bw, bh);
        if (msg_fresh()) ui_text(L->card_x + 16, ry + (L->row_h - ui_h()) / 2, g_msg, g_msg_col);
    }
}

static void page_wifi(const Lay* L, int x0, int y0) {
    const Theme64Tokens* t = tk64();
    const bool zh = gui64_lang_zh();
    page_title(L, x0 + L->cx, y0, zh ? "Wi-Fi" : "Wi-Fi",
               zh ? "如实显示：本机没有无线硬件，也没有无线驱动" : "honest: no wireless hardware/driver");
    const HwInfo64* hw = hw_info64();
    int wired = 0, wireless = 0, net = 0;
    if (hw->magic == HW64_INFO_MAGIC) {
        for (uint32_t i = 0; i < hw->pci.count; i++) {
            const HwPciDev64* d = &hw->pci.devs[i];
            if (d->class_code != 0x02) continue;
            net++;
            if (d->subclass == 0x80) wireless++;
            else wired++;
        }
    }
    // 打点：进入本页时打一次（不是每帧打 —— 否则会把串口刷满）
    static int wifi_logged_page = -1;
    if (wifi_logged_page != g_page) {
        wifi_logged_page = g_page;
        dbg64_line_begin64();
        dbg64_str("[SET64] wifi present="); dbg64_dec((uint64_t)wireless);
        dbg64_str(" wireless="); dbg64_dec((uint64_t)wireless);
        dbg64_str(" pci_net="); dbg64_dec((uint64_t)net);
        dbg64_str(" wired="); dbg64_dec((uint64_t)wired);
        dbg64_str(" reason=no wireless hardware (PCI class 0x02 subclass 0x80 = 0) and no 802.11 driver");
        dbg64_nl();
        dbg64_line_end64();
    }
    int y = L->card_y;
    const int c1h = card_rows_h(L, 4) + 10;
    card_begin(L, y, c1h);
    int ry = y + 12;
    {
        int rx = 0;
        row_label(L, ry, zh ? "无线适配器" : "Wireless adapter",
                  zh ? "PCI 扫描（class 02 / subclass 80）计数" : "PCI scan count", &rx);
        Buf b; b_init(&b);
        b_str(&b, zh ? "无无线硬件（wireless=" : "none (wireless=");
        b_int(&b, wireless); b_str(&b, zh ? "）" : ")");
        ui_text(rx - ui_w(b.b), ry + (L->row_h - ui_h()) / 2, b.b, t->accent2);
        ry += L->row_h;
    }
    {
        int rx = 0;
        row_label(L, ry, zh ? "有线适配器" : "Wired adapter", nullptr, &rx);
        Buf b; b_init(&b);
        b_int(&b, wired); b_str(&b, zh ? " 个（e1000；Wi-Fi 与它无关）" : " (e1000)");
        ui_text(rx - ui_w(b.b), ry + (L->row_h - ui_h()) / 2, b.b, t->text);
        ry += L->row_h;
    }
    ui_text(L->card_x + 16, ry, zh ? "本页绝不会列出 SSID / 信号强度 —— 没有无线驱动就没有这些数据。"
                                   : "no SSID list is fabricated: without a driver there is no data.",
            t->text);
    ry += L->row_h;
    ui_text(L->card_x + 16, ry,
            zh ? "未实现：802.11 驱动、USB 无线网卡（EHCI/xHCI 也都没实现）、WPA 握手。" : "not implemented: 802.11 driver.",
            t->text_dim);
}

static void page_avatar(const Lay* L, int x0, int y0) {
    const Theme64Tokens* t = tk64();
    const bool zh = gui64_lang_zh();
    page_title(L, x0 + L->cx, y0, zh ? "头像" : "Avatar",
               zh ? "3 个内置头像 + 本地图片路径（PNG/BMP）" : "3 builtin + local image (PNG/BMP)");
    const User64Entry* cu = cur_user();
    char av[USERDB64_PATH_MAX];
    get_cur_avatar(av, (int)sizeof(av));
    int y = L->card_y;
    const int c1h = card_rows_h(L, 2) + 84;
    card_begin(L, y, c1h);
    int ry = y + 12;
    {
        int rx = 0;
        row_label(L, ry, cu ? cu->name : (zh ? "（未登录）" : "(not logged in)"),
                  zh ? "当前用户（与锁屏/开始菜单同一份用户表）" : "same userdb as lock screen / start menu", &rx);
        Buf b; b_init(&b);
        b_str(&b, zh ? "当前头像 " : "avatar ");
        b_str(&b, av[0] ? av : "builtin (procedural)");
        ui_text(rx - ui_w(b.b), ry + (L->row_h - ui_h()) / 2, b.b, t->text_dim);
        ry += L->row_h;
    }
    // 3 个内置头像
    {
        ui_text(L->card_x + 16, ry, zh ? "内置头像（点一个即可选中，写入用户表并持久化）"
                                       : "builtin avatars (click to select)", t->text);
        ry += 24;
        int cx = L->card_x + 16 + 32;
        for (int i = 0; i < 3; i++) {
            const int id = CID_AVATAR_BASE + i;
            const bool sel = (!av[0]) ? (i == 0) : false;
            draw_avatar(cx, ry + 28, 56, i, sel, id);
            ctl_reg(P_AVATAR, id, CK_CHOICE, cx - 28, ry, 56, 56);
            Buf b; b_init(&b);
            b_str(&b, "builtin:"); b_int(&b, i);
            ui_text(cx - ui_w(b.b) / 2, ry + 60, b.b, sel ? t->accent : t->text_dim);
            cx += 96;
        }
        ry += 84;
    }
    // 本地图片路径
    const int y2 = y + c1h + 12;
    const int c2h = card_rows_h(L, 2) + 26;
    card_begin(L, y2, c2h);
    ry = y2 + 12;
    {
        int rx = 0;
        row_label(L, ry, zh ? "本地图片路径" : "Local image path",
                  zh ? "PNG/BMP（VimtuFS2 路径）；JPEG 未实现，会如实报错" : "PNG/BMP; JPEG not implemented", &rx);
        const int fh = 32, fw = 300;
        const int fx = rx - 240 - 10 - fw;
        const int fy = ry + (L->row_h - fh) / 2;
        draw_field(fx, fy, fw, fh, g_focus == CID_AVATAR_FIELD ? g_input : (av[0] ? av : ""),
                   g_focus == CID_AVATAR_FIELD, g_hover == CID_AVATAR_FIELD);
        ctl_reg(P_AVATAR, CID_AVATAR_FIELD, CK_FIELD, fx, fy, fw, fh);
        const int bw = 120;
        draw_button(rx - 120 - 10 - bw, fy, bw, fh, zh ? "应用" : "Apply", 1,
                    g_hover == CID_AVATAR_APPLY, g_press == CID_AVATAR_APPLY);
        ctl_reg(P_AVATAR, CID_AVATAR_APPLY, CK_BUTTON, rx - 120 - 10 - bw, fy, bw, fh);
        draw_button(rx - 120, fy, 120, fh, zh ? "用内置图" : "Use PNG", 0,
                    g_hover == CID_AVATAR_PRESET, g_press == CID_AVATAR_PRESET);
        ctl_reg(P_AVATAR, CID_AVATAR_PRESET, CK_BUTTON, rx - 120, fy, 120, fh);
        ry += L->row_h;
    }
    if (msg_fresh()) ui_text(L->card_x + 16, ry, g_msg, g_msg_col);
    else ui_text(L->card_x + 16, ry,
                 zh ? "提示：「用内置图」= /logo/kaisi.png（外壳装进 VimtuFS2 的真 PNG，158x158）"
                    : "tip: /logo/kaisi.png is a real PNG installed by the shell",
                 t->text_dim);
}

static void page_uname(const Lay* L, int x0, int y0) {
    const Theme64Tokens* t = tk64();
    const bool zh = gui64_lang_zh();
    page_title(L, x0 + L->cx, y0, zh ? "用户名" : "User name",
               zh ? "改名会重写 /etc/users.db；开始菜单/登录界面立即同步" : "rewrites /etc/users.db");
    const User64Entry* cu = cur_user();
    int y = L->card_y;
    const int c1h = card_rows_h(L, 3) + 20;
    card_begin(L, y, c1h);
    int ry = y + 12;
    {
        int rx = 0;
        row_label(L, ry, zh ? "当前用户名" : "Current", nullptr, &rx);
        ui_text(rx - ui_w(cu ? cu->name : "-"), ry + (L->row_h - ui_h()) / 2, cu ? cu->name : "-", t->text);
        ry += L->row_h;
    }
    {
        int rx = 0;
        row_label(L, ry, zh ? "新用户名" : "New name",
                  zh ? "1..15 个可打印 ASCII（不能含 | / \\ : ,）" : "1..15 printable ASCII", &rx);
        const int fh = 32, fw = 280;
        const int fx = rx - 120 - 10 - fw;
        const int fy = ry + (L->row_h - fh) / 2;
        const char* shown = (g_focus == CID_UNAME_FIELD) ? g_input : (cu ? cu->name : "");
        draw_field(fx, fy, fw, fh, shown, g_focus == CID_UNAME_FIELD, g_hover == CID_UNAME_FIELD);
        ctl_reg(P_UNAME, CID_UNAME_FIELD, CK_FIELD, fx, fy, fw, fh);
        const int bw = 120;
        draw_button(rx - bw, fy, bw, fh, zh ? "改名" : "Rename", 1, g_hover == CID_UNAME_APPLY, g_press == CID_UNAME_APPLY);
        ctl_reg(P_UNAME, CID_UNAME_APPLY, CK_BUTTON, rx - bw, fy, bw, fh);
        ry += L->row_h;
    }
    if (msg_fresh()) ui_text(L->card_x + 16, ry, g_msg, g_msg_col);
    ry += L->row_h;
    ui_text(L->card_x + 16, ry,
            zh ? "语义说明：等同 usermod -l（主目录路径保持不变，不搬 /home 目录）；密码/头像/主题等该用户"
                 "的其他字段全部保留。"
               : "same as usermod -l (home path unchanged; password/avatar kept)",
            t->text_dim);
}

static void page_passwd(const Lay* L, int x0, int y0) {
    const Theme64Tokens* t = tk64();
    const bool zh = gui64_lang_zh();
    page_title(L, x0 + L->cx, y0, zh ? "密码" : "Password",
               zh ? "当前用户的口令（加盐 SHA-256，1000 轮）" : "salted SHA-256, 1000 rounds");
    const User64Entry* cu = cur_user();
    const bool has_pw = cu && cu->hash[0];
    int y = L->card_y;
    const int c1h = card_rows_h(L, 4) + 24;
    card_begin(L, y, c1h);
    int ry = y + 12;
    {
        int rx = 0;
        row_label(L, ry, zh ? "用户" : "User", nullptr, &rx);
        ui_text(rx - ui_w(cu ? cu->name : "-"), ry + (L->row_h - ui_h()) / 2, cu ? cu->name : "-", t->text);
        ry += L->row_h;
    }
    {
        int rx = 0;
        row_label(L, ry, zh ? "口令状态" : "State", nullptr, &rx);
        Buf b; b_init(&b);
        b_str(&b, has_pw ? (zh ? "已设置（登录/锁屏会弹密码框）" : "set (login asks for password)")
                         : (zh ? "未设置（直接进桌面）" : "not set (straight to desktop)"));
        ui_text(rx - ui_w(b.b), ry + (L->row_h - ui_h()) / 2, b.b, has_pw ? t->accent : t->accent2);
        ry += L->row_h;
    }
    {
        int rx = 0;
        row_label(L, ry, zh ? "新密码" : "New password",
                  zh ? "输入后点「设置」；点「清空」= 不设密码（重启后直接进桌面）"
                     : "type then Set; Clear = no password", &rx);
        const int fh = 32, fw = 280;
        const int fx = rx - 120 - 10 - fw;
        const int fy = ry + (L->row_h - fh) / 2;
        // 密码框：不回显明文（显示 * 号；明文只在内存里活到提交）
        char mask[40];
        int n = 0;
        const char* src = (g_focus == CID_PW_FIELD) ? g_input : "";
        for (; src[n] && n < (int)sizeof(mask) - 1; n++) mask[n] = '*';
        mask[n] = 0;
        draw_field(fx, fy, fw, fh, mask, g_focus == CID_PW_FIELD, g_hover == CID_PW_FIELD);
        ctl_reg(P_PASSWD, CID_PW_FIELD, CK_FIELD, fx, fy, fw, fh);
        const int bw = 120;
        draw_button(rx - bw, fy, bw, fh, zh ? "设置" : "Set", 1, g_hover == CID_PW_APPLY, g_press == CID_PW_APPLY);
        ctl_reg(P_PASSWD, CID_PW_APPLY, CK_BUTTON, rx - bw, fy, bw, fh);
        ry += L->row_h;
    }
    {
        const int bw = 150, bh = 32;
        const int bx = L->card_x + 16;
        draw_button(bx, ry + (L->row_h - bh) / 2, bw, bh, zh ? "清空密码" : "Clear password", 0,
                    g_hover == CID_PW_CLEAR, g_press == CID_PW_CLEAR);
        ctl_reg(P_PASSWD, CID_PW_CLEAR, CK_BUTTON, bx, ry + (L->row_h - bh) / 2, bw, bh);
        if (msg_fresh()) ui_text(bx + bw + 16, ry + (L->row_h - ui_h()) / 2, g_msg, g_msg_col);
        ry += L->row_h;
    }
    ui_text(L->card_x + 16, ry,
            zh ? "这是修改密码的**正路**（主题/头像/用户名/密码都只能在这里改）；终端里的 passwd/useradd/"
                 "userdel 保留为管理员工具（多用户/权限批次需要），改的是同一份 /etc/users.db。"
               : "the GUI path; terminal passwd/useradd stay as admin tools (same userdb).",
            t->text_dim);
}

// ★ 版本号唯一真源 = build64.sh 的 VIMTUOS_VERSION（编译期宏，发布时只改那一处）。
//   独立编译这个文件（例如 IDE 语法检查）而没有该宏时，如实打 unknown，不编造版本号。
#ifndef VIMTUOS_VERSION_STR
#define VIMTUOS_VERSION_STR "unknown"
#endif

// ---- 驱动 / GPU / 版本（关于页）----
static void about_log_once() {
    static bool done = false;
    if (done) return;
    done = true;
    // 版本：唯一真源 = build64.sh 的 VIMTUOS_VERSION（编译期宏 VIMTUOS_VERSION_STR），
    //       别在这里写死版本串；构建标记仍取 rust64 的 BUILD_TAG
    dbg64_line_begin64();
    dbg64_str("[SET64] about version=VimtuOS " VIMTUOS_VERSION_STR " x86_64 build_tag=");
    const uint8_t* tag = rust64_build_tag64();
    const uint32_t tl = rust64_build_tag_len64();
    if (tag && tl > 0 && tl < 96) {
        for (uint32_t i = 0; i < tl; i++) {
            const char c = (char)tag[i];
            if (c >= 0x20 && c <= 0x7E) { char t[2] = { c, 0 }; dbg64_str(t); }
        }
    } else dbg64_str("(none)");
    dbg64_str(" bits=64 (long mode)");
    dbg64_nl();
    dbg64_line_end64();
    dbg64_str("[SET64] drivers implemented=");
    dbg64_dec((uint64_t)12);
    dbg64_str(" missing=");
    dbg64_dec((uint64_t)4);
    dbg64_str(" gpu=software (no accelerator driver; CPU rasterizer into the LFB)");
    dbg64_nl();
    dbg64_str("[SET64] gpu accel=none renderer=software reason=no GPU driver (no 2D/3D accel, no DRM/KMS)");
    dbg64_nl();
}
static void page_about(const Lay* L, int x0, int y0) {
    const Theme64Tokens* t = tk64();
    const bool zh = gui64_lang_zh();
    about_log_once();
    page_title(L, x0 + L->cx, y0, zh ? "关于" : "About",
               zh ? "版本 / 驱动 / GPU 加速状态" : "version / drivers / GPU");
    if (g_hw_view) {
        // 内嵌硬件检查报告（与启动期那一页同一份 hwui64 内容）
        const int px = x0 + L->card_x, py = y0 + L->card_y;
        const int pw = L->card_w, ph = L->ch - L->card_y - 52;
        const int n = hwui64_build64();
        const int back_w = 120, back_h = 30;
        (void)hwui64_draw64(px + 8, py, pw - 16, ph - back_h - 8, 0);
        const int bxx = px + pw - back_w - 10;
        const int byy = py + ph - back_h - 2;
        draw_button(bxx, byy, back_w, back_h, zh ? "返回" : "Back", 0, g_hover == CID_HW_BACK, g_press == CID_HW_BACK);
        ctl_reg(P_ABOUT, CID_HW_BACK, CK_BUTTON, bxx, byy, back_w, back_h);
        if (!g_hwui_logged) {
            g_hwui_logged = true;
            dbg64_line_begin64();
            dbg64_str("[SET64] hwui lines="); dbg64_dec((uint64_t)n);
            dbg64_str(" storage="); dbg64_dec((uint64_t)hwui64_storage_count64());
            dbg64_str(" controllers="); dbg64_dec((uint64_t)hwui64_controller_count64());
            dbg64_str(" page=about-embed");
            dbg64_nl();
            dbg64_line_end64();
        }
        return;
    }
    int y = L->card_y;
    // 卡 1：版本
    const int c1h = card_rows_h(L, 3);
    card_begin(L, y, c1h);
    int ry = y + 12;
    {
        ui_text(L->card_x + 16, ry + 4, "VimtuOS " VIMTUOS_VERSION_STR " x86_64", t->text);
        Buf b; b_init(&b);
        b_str(&b, zh ? "  64 位长模式内核（clang++ -target x86_64-elf, C++17, -mno-sse）"
                     : "  64-bit long mode kernel");
        ui_text(L->card_x + 16 + ui_w("VimtuOS " VIMTUOS_VERSION_STR " x86_64"), ry + 4, b.b, t->text_dim);
        ry += L->row_h;
    }
    {
        const uint8_t* tag = rust64_build_tag64();
        const uint32_t tl = rust64_build_tag_len64();
        Buf b; b_init(&b);
        b_str(&b, zh ? "构建标记（rust64 BUILD_TAG，不重复写死）：" : "build tag (rust64 BUILD_TAG): ");
        if (tag && tl > 0 && tl < 96) {
            for (uint32_t i = 0; i < tl; i++) {
                const char c = (char)tag[i];
                if (c >= 0x20 && c <= 0x7E) b_ch(&b, c);
            }
        } else b_str(&b, "(none)");
        ui_text(L->card_x + 16, ry + 4, b.b, t->text_dim);
        ry += L->row_h;
    }
    {
        const HwInfo64* hw = hw_info64();
        Buf b; b_init(&b);
        b_str(&b, zh ? "CPU " : "CPU ");
        b_str(&b, (hw->magic == HW64_INFO_MAGIC && hw->cpu.brand[0]) ? hw->cpu.brand : "unknown");
        b_str(&b, zh ? "   核 " : "  cores "); b_u64(&b, hw->cpu.cores ? hw->cpu.cores : 1);
        b_str(&b, zh ? "   内存 " : "  ram "); b_u64(&b, mem_total_ram_64() / (1024 * 1024)); b_str(&b, " MB");
        b_str(&b, zh ? "   内核堆 " : "  heap ");
        b_u64(&b, heap_used_64() / 1024); b_str(&b, "/"); b_u64(&b, heap_total_64() / 1024); b_str(&b, " KB");
        ui_text(L->card_x + 16, ry + 4, b.b, t->text_dim);
    }
    // 卡 2：驱动（已实现 / 未实现）
    const int y2 = y + c1h + 12;
    const int c2h = card_rows_h(L, 6) + 8;
    card_begin(L, y2, c2h);
    ry = y2 + 12;
    {
        const Ahci64CtrlInfo* a = ahci64_ctrl64();
        const Nvme64CtrlInfo* n = nvme64_ctrl64();
        const int ahci_on = (a && a->found) ? 1 : 0;
        const int nvme_on = (n && n->found) ? 1 : 0;
        Buf b; b_init(&b);
        b_str(&b, "[已实现] ");
        b_str(&b, "ATA(PIO/LBA28+48) ");
        b_str(&b, "AHCI("); b_int(&b, ahci_on); b_str(&b, ",ports="); b_int(&b, ahci64_count64()); b_str(&b, ") ");
        b_str(&b, "NVMe("); b_int(&b, nvme_on); b_str(&b, ",ns="); b_int(&b, nvme64_count64()); b_str(&b, ") ");
        ui_text(L->card_x + 16, ry + 4, b.b, t->text);
        ry += L->row_h;
    }
    {
        Buf b; b_init(&b);
        b_str(&b, "USB UHCI ");
        b_str(&b, usb64_state_str64());
        b_str(&b, zh ? "  端口 " : "  ports "); b_u64(&b, (uint64_t)usb64_ports64());
        b_str(&b, zh ? "  设备 " : "  devs "); b_u64(&b, (uint64_t)usb64_devices64());
        b_str(&b, "  HID "); b_u64(&b, usb64_hid_reports64());
        b_str(&b, zh ? "  USB 存储(只读) " : "  USB MSC(RO) "); b_int(&b, usb64_msc_count64());
        ui_text(L->card_x + 16, ry + 4, b.b, t->text);
        ry += L->row_h;
    }
    {
        Buf b; b_init(&b);
        b_str(&b, "e1000 ");
        b_str(&b, net64_state_str64());
        b_str(&b, zh ? "  链路 " : "  link "); b_int(&b, e1000_link64());
        b_str(&b, "  PS/2 8042 (KBD+MOU)");
        b_str(&b, zh ? "  中断 " : "  irq ");
        b_str(&b, (g_irq_mode64 == IRQ_MODE64_APIC) ? "APIC(LAPIC+IOAPIC)" : "8259 PIC");
        b_str(&b, "  SMP "); b_u64(&b, (uint64_t)smp64_online_cpu_count64());
        b_str(&b, "/"); b_u64(&b, (uint64_t)acpi_cpu_count64());
        ui_text(L->card_x + 16, ry + 4, b.b, t->text);
        ry += L->row_h;
    }
    {
        const Edid64* ed = edid64_get();
        Buf b; b_init(&b);
        b_str(&b, "ACPI "); b_hex(&b, acpi_rsdp_addr64(), 5);
        b_str(&b, zh ? "  版本 " : "  rev "); b_int(&b, acpi_revision64());
        b_str(&b, zh ? "  IOAPIC " : "  ioapic "); b_int(&b, acpi_ioapic_count64());
        b_str(&b, zh ? "  EDID " : "  edid "); b_str(&b, ed->valid ? "ok" : "none");
        b_str(&b, zh ? "  帧缓冲 " : "  fb ");
        b_int(&b, fb_phys_width()); b_ch(&b, 'x'); b_int(&b, fb_phys_height()); b_str(&b, "@32bpp");
        ui_text(L->card_x + 16, ry + 4, b.b, t->text);
        ry += L->row_h;
    }
    {
        ui_text(L->card_x + 16, ry + 4,
                zh ? "[未实现] HDA/AC97 声卡驱动、EHCI/xHCI（USB 2.0/3.0）、无线网卡（802.11）、GPU 加速"
                     "（无 2D/3D/DRM 驱动）"
                   : "[missing] HDA/AC97 audio, EHCI/xHCI, 802.11 wireless, GPU acceleration",
                t->accent2);
        ry += L->row_h;
    }
    {
        const int bw = 220, bh = 32;
        const int bx = L->card_x + 16;
        draw_button(bx, ry + (L->row_h - bh) / 2, bw, bh, zh ? "显示硬件检查报告" : "Hardware report", 0,
                    g_hover == CID_HW_CHECK, g_press == CID_HW_CHECK);
        ctl_reg(P_ABOUT, CID_HW_CHECK, CK_BUTTON, bx, ry + (L->row_h - bh) / 2, bw, bh);
        ry += L->row_h;
    }
    // 卡 3：GPU 加速状态（如实：未实现）
    const int y3 = y2 + c2h + 12;
    const int c3h = card_rows_h(L, 2);
    card_begin(L, y3, c3h);
    {
        ui_text(L->card_x + 16, y3 + 12 + 4,
                zh ? "GPU 加速状态：**未实现** —— 当前为软件渲染" : "GPU acceleration: not implemented (software)",
                t->text);
        Buf b; b_init(&b);
        b_str(&b, zh ? "所有绘制（圆角/阴影/毛玻璃/渐变）都由 CPU 光栅化到帧缓冲；没有 DRM/KMS、没有 2D/3D"
                       " 加速器驱动、没有 GPU 命令提交。"
                     : "all rendering is CPU rasterized into the framebuffer; no accelerator driver.");
        ui_text(L->card_x + 16, y3 + 12 + 4 + ui_h() + 6, b.b, t->text_dim);
    }
}

// ==================== 页面分派 ====================
static void draw_page(const Lay* L, int x0, int y0) {
    switch (g_page) {
        case P_DISPLAY: page_display(L, x0, y0); break;
        case P_SOUND:   page_sound(L, x0, y0);   break;
        case P_POWER:   page_power(L, x0, y0);   break;
        case P_DEFAPP:  page_defapp(L, x0, y0);  break;
        case P_THEME:   page_theme(L, x0, y0);   break;
        case P_WALL:    page_wall(L, x0, y0);    break;
        case P_COLOR:   page_color(L, x0, y0);   break;
        case P_FIT:     page_fit(L, x0, y0);     break;
        case P_DOCK:    page_dock(L, x0, y0);    break;
        case P_FONT:    page_font(L, x0, y0);    break;
        case P_ETH:     page_eth(L, x0, y0);     break;
        case P_WIFI:    page_wifi(L, x0, y0);    break;
        case P_AVATAR:  page_avatar(L, x0, y0);  break;
        case P_UNAME:   page_uname(L, x0, y0);   break;
        case P_PASSWD:  page_passwd(L, x0, y0);  break;
        default:        page_about(L, x0, y0);   break;
    }
}

// ==================== 动效（切页：淡入 + 轻微位移；时长/缓动全走 Token） ====================
static void page_go(int p, const char* how) {
    if (p < 0 || p >= P_COUNT) return;
    if (p == g_page) return;
    g_page = p;
    g_drop = DROP_NONE;
    g_focus = -1;
    g_hw_view = 0;
    g_anim_ms = (int)theme64_dur64(THEME64_MS_NORMAL);
    g_anim_t0 = ticks64();
    g_prev_cw = -1;                 // 强制重打布局/控件几何（新页的坐标给自动化用）
    g_seen_n = 0;
    dbg64_line_begin64();
    dbg64_str("[SET64] page="); dbg64_dec((uint64_t)g_page);
    dbg64_str(" name="); dbg64_str(kPages[g_page].key);
    dbg64_str(" anim="); dbg64_dec((uint64_t)g_anim_ms);
    dbg64_str(" how="); dbg64_str(how ? how : "click");
    dbg64_str(" (fade + translateY, token THEME64_MS_NORMAL / ease token)");
    dbg64_nl();
    dbg64_line_end64();
    log_num("[UI] settings page=", (uint64_t)g_page);
    gui64_invalidate();
}
static int anim_prog() {            // 0..256 缓动后的进度（256 = 到位）
    if (g_anim_ms <= 0) return 256;
    const uint32_t dt = (uint32_t)(ticks64() - g_anim_t0);
    uint32_t ms = dt * TICK_MS_64;
    if (ms >= (uint32_t)g_anim_ms) return 256;
    return theme64_ease64((int)(ms * 256 / (uint32_t)g_anim_ms));
}

// ==================== 主绘制 ====================
static void set_draw(Window* w) {
    const Theme64Tokens* t = tk64();
    const int cw = win_cw(w), ch = win_ch(w);
    const int x0 = w->client_x, y0 = w->client_y;
    Lay L;
    lay_calc(cw, ch, &L);
    if (cw != g_prev_cw || ch != g_prev_ch) {
        g_prev_cw = cw;
        g_prev_ch = ch;
        g_seen_n = 0;                // 尺寸变了 -> 控件几何要重新打
        dbg64_line_begin64();
        dbg64_str("[UI] settings layout client="); dbg64_dec((uint64_t)cw);
        dbg64_str("x"); dbg64_dec((uint64_t)ch);
        dbg64_str(" nav="); dbg64_dec((uint64_t)L.nav_w);
        dbg64_str(" field="); dbg64_dec((uint64_t)L.card_w);
        dbg64_str(" row="); dbg64_dec((uint64_t)L.row_h);
        dbg64_nl();
        dbg64_line_end64();
        dbg64_line_begin64();
        dbg64_str("[SET64] layout client="); dbg64_dec((uint64_t)cw);
        dbg64_str("x"); dbg64_dec((uint64_t)ch);
        dbg64_str(" nav="); dbg64_dec((uint64_t)L.nav_w);
        dbg64_str(" card="); dbg64_dec((uint64_t)L.card_w);
        dbg64_str(" row="); dbg64_dec((uint64_t)L.row_h);
        dbg64_str(" page="); dbg64_dec((uint64_t)g_page);
        dbg64_nl();
        dbg64_line_end64();
    g_cl_x = x0;
    g_cl_y = y0;
    // 客户区底：主题客户区底色（Token）；卡片再叠亚克力
    fb_fill_rect(x0, y0, cw, ch, t->client_bg);
    // 左导航（亚克力）
    draw_nav(&L, x0, y0);
    // 右侧内容（带切页动效：位移 + 淡入）
    ctl_reset();
    const int prog = anim_prog();
    const int dy = (256 - prog) * 14 / 256;
    const int ax = x0 + L.nav_w;
    const int aw = cw - L.nav_w;
    {
        // 裁剪在动效期间只允许内容区重绘（避免位移画到导航上）
        fb_set_clip(ax, y0, aw, ch);
        draw_page(&L, ax, y0 + dy);
        fb_set_clip(x0, y0, cw, ch);
        // 内容区绝对矩形（自动化按它裁剪像素比较；也是"切页后右侧内容变了"的取样区）。
        // 去重：同一页同一矩形只打一次（悬停/动效会重绘很多帧，不能每帧都写串口）。
        static int vp = -1, vx = -1, vy = -1, vw = -1, vh = -1;
        if (vp != g_page || vx != ax || vy != y0 || vw != aw || vh != ch) {
            vp = g_page; vx = ax; vy = y0; vw = aw; vh = ch;
            dbg64_line_begin64();
            dbg64_str("[SET64] view page="); dbg64_dec((uint64_t)g_page);
            dbg64_str(" name="); dbg64_str(kPages[g_page].key);
            dbg64_str(" x="); dbg64_dec((uint64_t)ax);
            dbg64_str(" y="); dbg64_dec((uint64_t)y0);
            dbg64_str(" w="); dbg64_dec((uint64_t)aw);
            dbg64_str(" h="); dbg64_dec((uint64_t)ch);
            dbg64_str(" anim="); dbg64_dec((uint64_t)g_anim_ms);
            dbg64_nl();
            dbg64_line_end64();
        }
        }
        if (prog < 256) {
            // 淡入：用客户区底色按剩余进度盖一层（Token 透明度的整数混合）
            const int a = 255 - prog * 255 / 256;
            if (a > 0) fb_fill_rect_alpha(ax, y0, aw, ch, t->client_bg, a);
        }
    }
    // 消息行（全局最后画，覆盖内容）
    if (msg_fresh()) {
        const int mw = L.card_w;
        const int my = y0 + L.card_y - 26;
        if (my > y0) {
            fill_r(x0 + L.card_x, my, mw, 22, THEME64_R_BUTTON, t->sel_bg, 200);
            ui_text(x0 + L.card_x + 10, my + (22 - ui_h()) / 2, g_msg, g_msg_col);
        }
    }
    // ★ 首帧的既有断言行（格式逐字不变；display64_test / tmgr_proc_test 依赖）
    if (!g_boot_logged) {
        g_boot_logged = true;
        const HwInfo64* hw = hw_info64();
        const Disp64Info* di = display64_info64();
        const bool hw_ok = (hw->magic == HW64_INFO_MAGIC);
        const char* dm = "none";
        if (hw_ok) {
            for (uint32_t i = 0; i < HW64_DISK_MAX; i++) {
                if (hw->disks[i].present) { dm = hw->disks[i].model[0] ? hw->disks[i].model : "(no model)"; break; }
            }
        }
        dbg64_line_begin64();
        dbg64_str("[UI] settings specs ram=");
        dbg64_dec(mem_total_ram_64() / (1024ull * 1024ull));
        dbg64_str("MB cpu=");
        dbg64_str((hw_ok && hw->cpu.vendor[0]) ? hw->cpu.vendor : "unknown");
        dbg64_str(" cores=");
        dbg64_dec((uint64_t)(hw_ok ? (hw->cpu.cores ? hw->cpu.cores : 1) : 1));
        dbg64_str(" disk=");
        dbg64_str(dm);
        dbg64_str(" vga=framebuffer ");
        dbg64_dec((uint64_t)fb_phys_width());
        dbg64_str("x");
        dbg64_dec((uint64_t)fb_phys_height());
        dbg64_str("@32bpp refresh=");
        if (di->refresh_x10 > 0) {
            char rb[16];
            display64_refresh_str64(rb, (int)sizeof(rb));
            dbg64_str(rb);
        } else dbg64_str("unknown");
        dbg64_str("Hz src=");
        dbg64_str(display64_src_name64(di->src));
        dbg64_str(" net=");
        dbg64_str(net64_state_str64());
        dbg64_str(" usb=");
        dbg64_str(usb64_state_str64());
        dbg64_str(" apic=");
        dbg64_str((g_irq_mode64 == IRQ_MODE64_APIC) ? "APIC" : "PIC");
        dbg64_str(" acpi_cpus=");
        dbg64_dec((uint64_t)acpi_cpu_count64());
        dbg64_str(" smp_online=");
        dbg64_dec((uint64_t)smp64_online_cpu_count64());
        dbg64_nl();
        dbg64_line_end64();
        const int n = hwui64_build64();      // 关于页的"硬件检查报告"内容同一份
        (void)n;
    }
}

// ==================== 交互：鼠标 ====================
static void focus_begin(int id, const char* initial) {
    g_focus = id;
    g_input_n = 0;
    g_input[0] = 0;
    if (initial) {
        for (int i = 0; initial[i] && i < (int)sizeof(g_input) - 1; i++) {
            g_input[i] = initial[i];
            g_input_n++;
        }
        g_input[g_input_n] = 0;
    }
}
static void dock_apply(const char* why) {
    gui64_dock_reload64();      // 外壳重算几何 + 再打一行 [DOCK64] geom
    Buf b; b_init(&b);
    b_str(&b, "[SET64] dock len="); b_int(&b, cfg64_dock_len64());
    b_str(&b, " auto="); b_int(&b, cfg64_dock_len64() > 0 ? 0 : 1);
    b_str(&b, " icon="); b_int(&b, cfg64_dock_icon64());
    b_str(&b, " gap="); b_int(&b, cfg64_dock_gap64());
    b_str(&b, " size="); b_int(&b, cfg64_dock_size64());
    b_str(&b, " why="); b_str(&b, why);
    b_str(&b, " applied=1 persisted=1 ([DOCK64] geom follows)");
    logln(b.b);
}
// 滑块：按下/拖动时按鼠标 x 反算值
static void slider_drag(int id, int x, int w, int mx) {
    const int v = (w > 0) ? ((mx - x) * 100 / w) : 0;
    int cv = v < 0 ? 0 : (v > 100 ? 100 : v);
    switch (id) {
        case CID_SND_VOL:
            sound_set_vol(cv, "drag");
            break;
        case CID_DOCK_LEN: {
            const int len = (cv <= 2) ? 0 : (320 + cv * 8);
            cfg64_set_dock_len64(len);
            dock_apply("slider");
            break;
        }
        case CID_DOCK_ICON: {
            const int px = 44 + (cv * 4 + 50) / 100;
            cfg64_set_dock_icon64(px);
            dock_apply("slider");
            break;
        }
        case CID_DOCK_GAP: {
            const int px = 4 + (cv * 20 + 50) / 100;
            cfg64_set_dock_gap64(px);
            dock_apply("slider");
            break;
        }
        default: break;
    }
    gui64_invalidate();
}
static void set_click(Window* w, int cx, int cy0) {
    Lay L;
    lay_calc(win_cw(w), win_ch(w), &L);
    // 内容坐标要减掉切页动效的位移（与 set_tick 一致：控件登记的是"到位后"的矩形）
    const int cy = cy0 - (256 - anim_prog()) * 14 / 256;
    // ---- 左导航（条目 + 分组标题：条目点击切页）----
    if (cx < L.nav_w) {
        for (int p = 0; p < P_COUNT; p++) {
            int nx, ny, nw, nh;
            nav_rect(&L, p, &nx, &ny, &nw, &nh);
            if (hit(nx, ny, nw, nh, cx, cy)) {
                if (p != g_page) page_go(p, "nav");
                gui64_invalidate();
                return;
            }
        }
        return;
    }
    // ---- 下拉列表项优先（展开时覆盖内容）----
    if (g_drop == DROP_RES) {
        const Ctl* c = ctl_at(g_page, cx, cy);
        if (c && c->id >= CID_RES_LIST && c->id < CID_RES_LIST + SET_RES_N) {
            apply_res(w, c->id - CID_RES_LIST);
            g_press = -1;
            gui64_invalidate();
            return;
        }
        g_drop = DROP_NONE;
    }
    // ---- 普通控件 ----
    const Ctl* c = ctl_at(g_page, cx, cy);
    if (!c) {
        g_focus = -1;
        gui64_invalidate();
        return;
    }
    const int id = c->id;
    g_press = id;
    switch (id) {
        // ---- 关于 / 硬件检查 ----
        case CID_HW_CHECK: g_hw_view = 1; g_hwui_logged = false; break;
        case CID_HW_BACK:  g_hw_view = 0; break;
        // ---- 显示 ----
        case CID_RES_DROP:
            g_drop = (g_drop == DROP_RES) ? DROP_NONE : DROP_RES;
            g_res_sel = res_sel_from_actual();
            dbg64_str("[UI] settings dropdown open=res items=");
            dbg64_dec((uint64_t)SET_RES_N);
            dbg64_nl();
            break;
        case CID_ZOOM_100: apply_zoom(w, 0); break;
        case CID_ZOOM_125: apply_zoom(w, 1); break;
        case CID_ZOOM_150: apply_zoom(w, 2); break;
        case CID_DISPLAY_APPLY:
            dbg64_str("[UI] settings apply res=");
            dbg64_dec((uint64_t)fb_phys_width()); dbg64_str("x"); dbg64_dec((uint64_t)fb_phys_height());
            dbg64_str(" zoom="); dbg64_dec((uint64_t)fb_get_zoom());
            dbg64_nl();
            apply_zoom(w, zoom_sel_from_actual());
            break;
        // ---- 声音 ----
        case CID_SND_VOL:
            g_drag = id;
            slider_drag(id, c->x, c->w, cx);
            sound_log_reason();
            break;
        case CID_SND_SPK: sound_set_src(0, "settings-click"); break;
        case CID_SND_HP:  sound_set_src(1, "settings-click"); break;
        // ---- 电源 ----
        case CID_PWR_SHUTDOWN:
            logln("[SET64] power action=shutdown via=spec (ACPI S5 -> acpi_poweroff64)");
            sys_shutdown64();
            break;
        case CID_PWR_REBOOT:
            logln("[SET64] power action=reboot via=spec");
            sys_reboot64();
            break;
        case CID_PWR_LOCK:
            logln("[SET64] power action=lock via=spec (locklogin64_lock64)");
            locklogin64_lock64("settings");
            break;
        // ---- 主题 ----
        case CID_THEME_TILE0: case CID_THEME_TILE0 + 1: case CID_THEME_TILE0 + 2:
        case CID_THEME_TILE0 + 3: case CID_THEME_TILE0 + 4: case CID_THEME_TILE0 + 5:
        case CID_THEME_TILE0 + 6: {
            const int id2 = id - CID_THEME_TILE0;
            theme64_set_theme64(id2, "settings");
            dbg64_line_begin64();
            dbg64_str("[SET64] theme id="); dbg64_dec((uint64_t)id2);
            dbg64_str(" name="); dbg64_str(theme64_name64(id2));
            dbg64_str(" reduce_motion="); dbg64_dec((uint64_t)theme64_reduce_motion64());
            dbg64_str(" persisted=1 (config64 ui.theme)");
            dbg64_nl();
            dbg64_line_end64();
            set_cur_user_theme64(id2, -1);      // 用户记录里的 theme 一起更新（登录时会写回全局）
            msg_set(gui64_lang_zh() ? "主题已切换（重启后保持）" : "theme applied (persisted)", tk64()->accent);
            gui64_invalidate();
            break;
        }
        case CID_THEME_REDUCE:
            theme64_set_reduce_motion64(theme64_reduce_motion64() ? 0 : 1, "settings");
            log_num("[SET64] theme reduce_motion=", (uint64_t)theme64_reduce_motion64());
            gui64_invalidate();
            break;
        // ---- 壁纸 ----
        case CID_WALL_FIELD:
            focus_begin(id, "");
            break;
        case CID_WALL_APPLY:
            wall_set_path(g_focus == CID_WALL_FIELD ? g_input : "");
            break;
        case CID_WALL_BUILTIN:
            wall_set_path("");
            break;
        case CID_WALL_PRESET:
            wall_set_path("/logo/kaisi.png");
            break;
        // ---- 颜色 ----
        case CID_GRAD_ON:
            wall_grad_toggle(cfg64_grad_on64() ? 0 : 1);
            break;
        case CID_GRAD_A_FIELD: case CID_GRAD_B_FIELD: {
            char cur[16];
            if (id == CID_GRAD_A_FIELD) cfg64_grad_a64(cur, (int)sizeof(cur));
            else cfg64_grad_b64(cur, (int)sizeof(cur));
            focus_begin(id, cur);
            break;
        }
        case CID_GRAD_A_APPLY: grad_apply("a"); break;
        case CID_GRAD_B_APPLY: grad_apply("b"); break;
        case CID_GRAD_RESET:
            cfg64_set_grad_a64("#5AA9F0");
            cfg64_set_grad_b64("#C9A7FF");
            if (cfg64_grad_on64()) { int gw = 0, gh = 0; (void)grad_wall_build64(&gw, &gh); }
            logln("[SET64] color a=#5AA9F0 b=#C9A7FF reset=1 persisted=1");
            break;
        // ---- 壁纸适应模式 ----
        case CID_FIT_SYNC:
            cfg64_set_wall_sync64(cfg64_wall_sync64() ? 0 : 1);
            log_num("[SET64] wall sync=", (uint64_t)cfg64_wall_sync64());
            break;
        // ---- Dock ----
        case CID_DOCK_LEN: case CID_DOCK_ICON: case CID_DOCK_GAP:
            g_drag = id;
            slider_drag(id, c->x, c->w, cx);
            break;
        case CID_DOCK_RESET:
            cfg64_set_dock_len64(0);
            cfg64_set_dock_icon64(THEME64_DOCK_ICON);
            cfg64_set_dock_gap64(THEME64_DOCK_GAP);
            cfg64_set_dock_size64(THEME64_DOCK_H);
            dock_apply("reset-to-token");
            break;
        // ---- 字体大小 ----
        case CID_FONT_BASE: case CID_FONT_BASE + 1: case CID_FONT_BASE + 2: {
            const int idx = id - CID_FONT_BASE;
            font_set_size64(kFontPx[idx]);
            cfg64_set_font_size64(kFontPx[idx]);
            Buf b; b_init(&b);
            b_str(&b, "[SET64] font size="); b_int(&b, font_get_size64());
            b_str(&b, " line="); b_int(&b, font_line_height());
            b_str(&b, " persisted=1 (ui.font.size)");
            logln(b.b);
            msg_set(gui64_lang_zh() ? "字号已更新（重启后保持）" : "text size applied (persisted)", tk64()->accent);
            gui64_invalidate();
            break;
        }
        // ---- 默认应用（每行 5 段；id 编码 = CID_DEF_ELF + kind + i*64）----
        case CID_NET_REFRESH:
            dbg64_line_begin64();
            dbg64_str("[SET64] eth link="); dbg64_dec((uint64_t)e1000_link64());
            dbg64_str(" state="); dbg64_str(net64_state_str64());
            dbg64_str(" tx="); dbg64_dec(net64_tx_frames64());
            dbg64_str(" rx="); dbg64_dec(net64_rx_frames64());
            dbg64_str(" how=refresh-button");
            dbg64_nl();
            dbg64_line_end64();
            msg_set(gui64_lang_zh() ? "已重新读取链路状态" : "link state re-read", tk64()->accent);
            break;
        case CID_AVATAR_FIELD:
            focus_begin(id, "");
            break;
        case CID_AVATAR_APPLY:
            avatar_apply(g_focus == CID_AVATAR_FIELD ? g_input : "", true);
            break;
        case CID_AVATAR_PRESET:
            avatar_apply("/logo/kaisi.png", true);
            break;
        case CID_UNAME_FIELD:
            focus_begin(id, "");
            break;
        case CID_UNAME_APPLY:
            user_rename(g_focus == CID_UNAME_FIELD ? g_input : "");
            break;
        case CID_PW_FIELD:
            focus_begin(id, "");
            break;
        case CID_PW_APPLY: {
            char pw[64];
            int i = 0;
            const char* src = (g_focus == CID_PW_FIELD) ? g_input : "";
            for (; src[i] && i < (int)sizeof(pw) - 1; i++) pw[i] = src[i];
            pw[i] = 0;
            // 用完立刻抹掉内存里的明文（不进任何日志）
            for (int k = 0; k < (int)sizeof(g_input); k++) g_input[k] = 0;
            g_input_n = 0;
            user_password_apply(pw);
            for (int k = 0; k < (int)sizeof(pw); k++) pw[k] = 0;
            break;
        }
        case CID_PW_CLEAR:
            user_password_apply("");
            break;
        default: break;
    }
    // 默认应用分段（id 编码：CID_DEF_ROW + kind*DEFAPP_N + seg）
    if (id >= CID_DEF_ROW && id < CID_DEF_ROW + CFG64_DEFK_COUNT * DEFAPP_N) {
        const int off = id - CID_DEF_ROW;
        const int kind = off / DEFAPP_N;
        const int seg = off % DEFAPP_N;
        if (seg < DEFAPP_N && kind < CFG64_DEFK_COUNT) {
            cfg64_set_defapp64(kind, kDefApps[seg].key);
            const char* kn[CFG64_DEFK_COUNT] = { "elf", "vap", "txt", "video" };
            dbg64_line_begin64();
            dbg64_str("[SET64] default kind="); dbg64_str(kn[kind]);
            dbg64_str(" app="); dbg64_str(kDefApps[seg].key[0] ? kDefApps[seg].key : "none");
            dbg64_str(" name="); dbg64_str(gui64_lang_zh() ? kDefApps[seg].zh : kDefApps[seg].en);
            dbg64_str(" persisted=1 (ui.def."); dbg64_str(kn[kind]); dbg64_str(")");
            dbg64_nl();
            dbg64_line_end64();
            msg_set(gui64_lang_zh() ? "默认应用已更新" : "default app updated", tk64()->accent);
        }
    }
    // 壁纸适应模式（桌面 / 锁屏分别设；同步开关打开时桌面改锁屏也跟着改）
    if (id >= CID_WALLFIT_BASE && id < CID_WALLFIT_BASE + GFX64_WALL_MODE_COUNT) {
        const int mode = id - CID_WALLFIT_BASE;
        gfx64_wall_set_mode64(mode, "settings");
        if (cfg64_wall_sync64()) cfg64_set_lock_wall_mode64(mode);
        dbg64_line_begin64();
        dbg64_str("[SET64] wall mode="); dbg64_dec((uint64_t)mode);
        dbg64_str(" name="); dbg64_str(gfx64_wall_mode_name64(mode));
        dbg64_str(" target=desktop sync="); dbg64_dec((uint64_t)cfg64_wall_sync64());
        dbg64_str(" lock_mode="); dbg64_dec((uint64_t)cfg64_lock_wall_mode64());
        dbg64_str(" desktop_mode="); dbg64_dec((uint64_t)gfx64_wall_mode64());
        dbg64_nl();
        dbg64_line_end64();
        msg_set(gui64_lang_zh() ? "桌面壁纸适应模式已更新" : "desktop fit mode updated", tk64()->accent);
        gui64_invalidate();
    }
    if (id >= CID_LOCKFIT_BASE && id < CID_LOCKFIT_BASE + GFX64_WALL_MODE_COUNT) {
        const int mode = id - CID_LOCKFIT_BASE;
        cfg64_set_lock_wall_mode64(mode);
        set_cur_user_theme64(-1, mode);     // 用户记录里的 lock_mode 一起更新（登录时会写回全局）
        if (cfg64_wall_sync64()) gfx64_wall_set_mode64(mode, "settings-sync");
        dbg64_line_begin64();
        dbg64_str("[SET64] wall mode="); dbg64_dec((uint64_t)mode);
        dbg64_str(" name="); dbg64_str(gfx64_wall_mode_name64(mode));
        dbg64_str(" target=lock sync="); dbg64_dec((uint64_t)cfg64_wall_sync64());
        dbg64_str(" lock_mode="); dbg64_dec((uint64_t)cfg64_lock_wall_mode64());
        dbg64_str(" desktop_mode="); dbg64_dec((uint64_t)gfx64_wall_mode64());
        dbg64_str(" (applied when the lock screen paints -> [LOCK64] wall src=.. lock_mode=)");
        dbg64_nl();
        dbg64_line_end64();
        msg_set(gui64_lang_zh() ? "锁屏壁纸适应模式已更新" : "lock fit mode updated", tk64()->accent);
        gui64_invalidate();
    }
    if (id >= CID_AVATAR_BASE && id < CID_AVATAR_BASE + 3) {
        Buf b; b_init(&b);
        b_str(&b, "builtin:"); b_int(&b, id - CID_AVATAR_BASE);
        avatar_apply(b.b, false);
    }
    gui64_invalidate();
}

// ==================== 交互：键盘 ====================
static void set_close(Window* w) {
    if (!g_close_logged) { logln("[APP] settings closed"); g_close_logged = true; }
    g_win = nullptr;
    g_drop = DROP_NONE;
    g_drag = -1;
    g_focus = -1;
    if (w) gui64_destroy_window(w);
}
static void set_key(Window* w, char c) {
    const unsigned char cc = (unsigned char)c;
    // ---- 输入框优先吃键（可打印字符 / 退格 / 回车提交 / ESC 取消）----
    if (g_focus >= 0) {
        if (cc == 0x1B) { g_focus = -1; gui64_invalidate(); return; }
        if (cc == 8) {
            if (g_input_n > 0) g_input[--g_input_n] = 0;
            gui64_invalidate();
            return;
        }
        if (cc == '\n' || cc == '\r') {
            const int id = g_focus;
            g_focus = -1;
            if (id == CID_WALL_FIELD) wall_set_path(g_input);
            else if (id == CID_GRAD_A_FIELD || id == CID_GRAD_B_FIELD) grad_apply(id == CID_GRAD_A_FIELD ? "a" : "b");
            else if (id == CID_AVATAR_FIELD) avatar_apply(g_input, true);
            else if (id == CID_UNAME_FIELD) user_rename(g_input);
            else if (id == CID_PW_FIELD) {
                char pw[64];
                int i = 0;
                for (; g_input[i] && i < (int)sizeof(pw) - 1; i++) pw[i] = g_input[i];
                pw[i] = 0;
                for (int k = 0; k < (int)sizeof(g_input); k++) g_input[k] = 0;
                g_input_n = 0;
                user_password_apply(pw);
                for (int k = 0; k < (int)sizeof(pw); k++) pw[k] = 0;
            }
            gui64_invalidate();
            return;
        }
        if (cc >= 0x20 && cc < 0x7F) {
            if (g_input_n < (int)sizeof(g_input) - 1) {
                g_input[g_input_n++] = (char)cc;
                g_input[g_input_n] = 0;
            }
            gui64_invalidate();
            return;
        }
        return;
    }
    if (cc == 0x1B) {                       // ESC：先收下拉/报告视图，再关窗
        if (g_drop != DROP_NONE) { g_drop = DROP_NONE; gui64_invalidate(); return; }
        if (g_hw_view) { g_hw_view = 0; gui64_invalidate(); return; }
        set_close(w);
        return;
    }
    if (g_drop != DROP_NONE) {
        if (cc == '\n' || cc == '\r') {
            g_drop = DROP_NONE;
            gui64_invalidate();
            return;
        }
        g_drop = DROP_NONE;
        gui64_invalidate();
        return;
    }
    // 无鼠标也能操作（也便于自动化）：左右方向键翻页 / 1..6 跳分组首页 / 0..9 直选页 / U 向上
    if (cc == 0xFB || cc == 0xFC) {         // 左/右
        const int d = (cc == 0xFC) ? 1 : -1;
        page_go((g_page + d + P_COUNT) % P_COUNT, "key");
        return;
    }
    if (cc == 0xFD || cc == 0xFE) {         // 上/下：同组内移动
        const int d = (cc == 0xFE) ? 1 : -1;
        page_go((g_page + d + P_COUNT) % P_COUNT, "key");
        return;
    }
    if (cc >= '1' && cc <= '6') {           // 数字 = 分组首页
        for (int p = 0; p < P_COUNT; p++) {
            if (kPages[p].grp == (int)(cc - '1')) { page_go(p, "key-group"); return; }
        }
        return;
    }
    if (cc == '0') { page_go(P_DISPLAY, "key"); return; }
    if (cc == 'h' || cc == 'H') {           // 硬件检查报告视图（与关于页的按钮等价；无鼠标也能开）
        g_hw_view = g_hw_view ? 0 : 1;
        if (g_hw_view) g_hwui_logged = false;
        gui64_invalidate();
        return;
    }
    if (cc == 'l' || cc == 'L') {           // 语言（保留既有能力）
        const bool zh = !gui64_lang_zh();
        gui64_set_lang_zh(zh);
        if (g_win && gui64_window_alive(g_win)) gui64_set_title(g_win, zh ? "设置" : "Settings");
        logln(zh ? "[UI] settings lang zh" : "[UI] settings lang en");
        g_seen_n = 0;
        gui64_invalidate();
        return;
    }
    // 主题热键（Ctrl+Shift+T/M/R）由外壳先消费；这里不再重复
}

// ==================== 交互：tick（悬停 / 拖动 / 按下态清除 / 动效重绘） ====================
static void set_tick(Window* w) {
    if (w->closing && !g_close_logged) {
        logln("[APP] settings closed");
        g_close_logged = true;
    }
    const int mx = mouse_get_x() - w->client_x;
    const int my = mouse_get_y() - w->client_y;
    const uint8_t btn = mouse_get_buttons();
    if (!(btn & 1)) {                        // 松开：清按下态与拖动
        if (g_press >= 0 || g_drag >= 0) {
            g_press = -1;
            g_drag = -1;
            gui64_invalidate_window(w);
        }
    }
    // 悬停（导航条目 = 1000 + page）
    int h = -1;
    Lay L;
    lay_calc(win_cw(w), win_ch(w), &L);
    if (mx >= 0 && my >= 0 && mx < L.nav_w && my < L.ch) {
        for (int p = 0; p < P_COUNT; p++) {
            int nx, ny, nw, nh;
            nav_rect(&L, p, &nx, &ny, &nw, &nh);
            if (hit(nx, ny, nw, nh, mx, my)) { h = 1000 + p; break; }
        }
    } else if (mx > L.nav_w) {
        // 内容区：命中的控件（控件表是上一帧登记的，够用；每帧都会刷新）
        const int ly = my - (256 - anim_prog()) * 14 / 256;
        const Ctl* c = ctl_at(g_page, mx, ly);
        if (c) h = c->id;
    }
    if (h != g_hover) {
        g_hover = h;
        gui64_invalidate_window(w);
    }
    // 拖动滑块：鼠标 x 实时反算（实时生效）
    if (g_drag >= 0 && (btn & 1)) {
        for (int i = 0; i < g_ctl_n; i++) {
            const Ctl* c = &g_ctl[i];
            if (c->page == g_page && c->id == g_drag) {
                slider_drag(g_drag, c->x, c->w, mx);
                break;
            }
        }
    }
    // 动效进行中：每帧重绘
    if (g_anim_ms > 0) {
        if (anim_prog() < 256) gui64_invalidate_window(w);
        else g_anim_ms = 0;
    }
}

// ==================== 应用入口 ====================
void app_settings_open64() {
    logln("[APP] settings opened");
    if (g_win && gui64_window_alive(g_win)) {
        logln("[UI] settings single-instance activate");
        gui64_set_active(g_win);
        return;
    }
    if (g_win && !gui64_window_alive(g_win)) {
        if (!g_close_logged) logln("[APP] settings closed");
        g_win = nullptr;
    }
    g_close_logged = false;
    g_page = P_DISPLAY;
    g_drop = DROP_NONE;
    g_hover = -1;
    g_press = -1;
    g_drag = -1;
    g_focus = -1;
    g_hw_view = 0;
    g_msg[0] = 0;
    g_res_sel = res_sel_from_actual();
    g_prev_cw = -1;
    g_prev_ch = -1;
    g_seen_n = 0;
    g_boot_logged = false;
    g_anim_ms = (int)theme64_dur64(THEME64_MS_NORMAL);
    g_anim_t0 = ticks64();

    const bool zh = gui64_lang_zh();
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

    dbg64_line_begin64();
    dbg64_str("[SET64] open w="); dbg64_dec((uint64_t)SET_W);
    dbg64_str(" h="); dbg64_dec((uint64_t)SET_H);
    dbg64_str(" nav="); dbg64_dec((uint64_t)NAV_W);
    dbg64_str(" pages="); dbg64_dec((uint64_t)P_COUNT);
    dbg64_str(" groups="); dbg64_dec((uint64_t)GRP_COUNT);
    dbg64_str(" theme="); dbg64_dec((uint64_t)theme64_id64());
    dbg64_str(" theme_name="); dbg64_str(theme64_name64(theme64_id64()));
    dbg64_str(" theme");
    dbg64_nl();
    dbg64_line_end64();
    dbg64_str("[UI] settings dropdowns res="); dbg64_dec((uint64_t)SET_RES_N);
    dbg64_str(" zoom="); dbg64_dec((uint64_t)SET_ZOOM_N);
    dbg64_str(" font="); dbg64_dec((uint64_t)SET_FONT_N);
    dbg64_str(" res_sel="); dbg64_dec((uint64_t)(g_res_sel < 0 ? 999 : g_res_sel));
    dbg64_nl();
}

void app_settings_reset64() {
    const bool alive = (g_win && gui64_window_alive(g_win));
    if (!alive && g_win && !g_close_logged) {
        logln("[APP] settings closed");
        g_close_logged = true;
    }
    if (!alive) g_win = nullptr;
    g_drop = DROP_NONE;
    g_hover = -1;
    g_press = -1;
    g_drag = -1;
    g_focus = -1;
    g_hw_view = 0;
    g_prev_cw = -1;
    g_prev_ch = -1;
    g_seen_n = 0;
    g_boot_logged = false;
    logln("[APP] settings reset");
}

// ==================== 启动期生效（gui64_run 调一次） ====================
void settings64_boot_apply64() {
    const int px = cfg64_font_size64();
    font_set_size64(px);
    const int grad = cfg64_grad_on64();
    char wp[CFG64_STR_MAX];
    cfg64_wall_path64(wp, (int)sizeof(wp));
    const char* wall = wp[0] ? "file" : "builtin";
    int gw = 0, gh = 0;
    if (!wp[0] && grad) {
        if (grad_wall_build64(&gw, &gh) == 0) wall = "gradient";
    }
    dbg64_line_begin64();
    dbg64_str("[SET64] boot apply font="); dbg64_dec((uint64_t)font_get_size64());
    dbg64_str(" grad="); dbg64_dec((uint64_t)grad);
    dbg64_str(" wall="); dbg64_str(wall);
    if (gw) { dbg64_str(" px="); dbg64_dec((uint64_t)gw); dbg64_str("x"); dbg64_dec((uint64_t)gh); }
    dbg64_str(" (font_set_size64 + optional custom gradient wallpaper)");
    dbg64_nl();
    dbg64_line_end64();
    // 默认应用映射（4 个类型的当前值）——重启后"设置保持"的证据行
    {
        static const char* const kn[CFG64_DEFK_COUNT] = { "elf", "vap", "txt", "video" };
        dbg64_line_begin64();
        dbg64_str("[SET64] default boot");
        for (int k = 0; k < CFG64_DEFK_COUNT; k++) {
            char app[16];
            cfg64_defapp64(k, app, (int)sizeof(app));
            dbg64_str(" "); dbg64_str(kn[k]); dbg64_str("=");
            dbg64_str(app[0] ? app : "none");
        }
        dbg64_str(" persisted=1");
        dbg64_nl();
        dbg64_line_end64();
    }
}
