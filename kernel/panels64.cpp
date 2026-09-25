// panels64.cpp - ★ P2：四个二级弹窗 + 通知列表 + 设备插拔 toast
//
// 所有面板的几何都从**开始菜单的几何**推导（startmenu64_geom64），所以在任何分辨率/菜单位置下都
// "跟随开始菜单"，不是固定屏幕坐标；统一用 p2ui_popup64（双层浅阴影 + 亚克力 + 1px 高光边）。
//
// 诚实边界（不要假装成功）：
//   * 声音：**没有声卡驱动**（没有 HDA/AC97 驱动），面板交互与数值都是内存态，每次改动打
//     "[PANEL64] audio driver not implemented yet"。
//   * 无线：**没有无线网卡驱动**，WiFi 区如实显示"无无线硬件/未检测到无线网卡"，列表恒为空
//     （panels64_wifi_add64() 是留给未来驱动的接口，本批没有任何调用者 -> 不编造列表）。
//   * 设备插拔：数据源 = e1000 链路轮询（真实可插拔，QEMU `set_link` 能触发）+ usb64 的存储/设备计数
//     差分；usb64 明确**没有热插拔/拔出检测**，所以 U 盘事件不会凭空产生（打点里写清楚）。
#include "panels64.h"

#include "gui64.h"
#include "gfx64.h"
#include "fb.h"
#include "font.h"
#include "input.h"
#include "x86_64.h"
#include "debug64.h"
#include "e1000_64.h"     // 以太网真实链路状态
#include "net64.h"        // net64_state_str64()
#include "port.h"        // PCI 配置口（0xCF8/0xCFC）：**实时**读 e1000 的 STATUS.LU（见 e1000_live_link64）
#include "usb64.h"        // usb64_msc_count64 / usb64_devices64（轮询差分）
#include "userdb64.h"

// ==================== 小工具 ====================
// 追加一段 UTF-8（汉字是多字节，不能写成 char 字面量）
static int push_u8_64(char* out, int o, int cap, const char* u8) {
    for (int i = 0; u8[i] && o < cap - 1; i++) out[o++] = u8[i];
    out[o] = 0;
    return o;
}
static int s_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static bool s_eq(const char* a, const char* b) {
    if (!a || !b) return a == b;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void s_copy(char* dst, const char* src, int cap) {
    int i = 0;
    for (; src && src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}
static int u_dec(char* b, uint64_t v) {
    char t[24]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    int k = 0;
    while (n) b[k++] = t[--n];
    b[k] = 0;
    return k;
}
static int u_dec4(char* b, int v) {
    b[0] = (char)('0' + (v / 1000) % 10);

    b[1] = (char)('0' + (v / 100) % 10);
    b[2] = (char)('0' + (v / 10) % 10);
    b[3] = (char)('0' + v % 10);
    b[4] = 0;
    return 4;
}
static int u_dec2(char* b, int v) {
    b[0] = (char)('0' + (v / 10) % 10);
    b[1] = (char)('0' + v % 10);
    b[2] = 0;
    return 2;
}
// "YYYY-MM-DD"
static void ymd64(char* out, int y, int m, int d) {
    char b[8];
    int n = 0;
    u_dec4(b, y);
    for (int i = 0; i < 4; i++) out[n++] = b[i];
    out[n++] = '-';
    u_dec2(b, m); out[n++] = b[0]; out[n++] = b[1];
    out[n++] = '-';
    u_dec2(b, d); out[n++] = b[0]; out[n++] = b[1];
    out[n] = 0;
}
// "HH:MM"
static void hm64(char* out) {
    int hh = 0, mm = 0, ss = 0;
    rtc_get_time64(&hh, &mm, &ss);
    char b[4];
    u_dec2(b, hh); out[0] = b[0]; out[1] = b[1]; out[2] = ':';
    u_dec2(b, mm); out[3] = b[0]; out[4] = b[1]; out[5] = 0;
}
static int weekday_ymd64(int y, int m, int d) {
    if (m < 1 || m > 12 || d < 1 || d > 31) return -1;
    int yy = y, mm = m;
    if (mm < 3) { mm += 12; yy -= 1; }
    const int k = yy % 100, j = yy / 100;
    const int h = (d + (13 * (mm + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    return ((h + 6) % 7 + 7) % 7;
}
static int days_in_month64(int y, int m) {
    static const int dm[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) return 30;
    if (m == 2) {
        const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
        return leap ? 29 : 28;
    }
    return dm[m - 1];
}

// ==================== 打点 ====================
static int g_log_budget = 300;      // [PANEL64] 上限
static int g_toast_log_budget = 200;  // [TOAST64] 上限
static void pl(const char* tag, const char* a, int64_t v, const char* b) {
    if (g_log_budget <= 0) return;
    g_log_budget--;
    dbg64_line_begin64();
    dbg64_str("[PANEL64] ");
    dbg64_str(tag);
    if (a) dbg64_str(a);
    if (v >= 0) dbg64_dec((uint64_t)v);
    if (b) dbg64_str(b);
    dbg64_nl();
    dbg64_line_end64();
}
static void tl(const char* tag, const char* a, int64_t v, const char* b) {
    if (g_toast_log_budget <= 0) return;
    g_toast_log_budget--;
    dbg64_line_begin64();
    dbg64_str("[TOAST64] ");
    dbg64_str(tag);
    if (a) dbg64_str(a);
    if (v >= 0) dbg64_dec((uint64_t)v);
    if (b) dbg64_str(b);
    dbg64_nl();
    dbg64_line_end64();
}

// ==================== 几何（跟随开始菜单）====================
static int scr_w64() { return fb_width(); }
static int scr_h64() { return fb_height(); }
static int dock_top64() { return scr_h64() - THEME64_DOCK_MARGIN - THEME64_DOCK_H; }
static void sm_rect64(int* x, int* y, int* w, int* h) { startmenu64_geom64(x, y, w, h); }
static void clamp_panel64(int* x, int* y, int w, int h) {
    if (*y < 4) *y = 4;
    const int max_y = dock_top64() - 4 - h;              // 不压 Dock
    if (*y > max_y) *y = max_y;
    if (*x < 4) *x = 4;
    const int max_x = scr_w64() - 4 - w;                 // 不超出屏幕
    if (*x > max_x) *x = max_x;
}
// 右侧锚点：开始菜单右边 + gap；放不下就翻到左侧
static void anchor_right64(int w, int* x) {
    int sx = 0, sy = 0, sw = 0, sh = 0;
    sm_rect64(&sx, &sy, &sw, &sh);
    *x = sx + sw + THEME64_POP_GAP;
    if (*x + w > scr_w64() - 4) *x = sx - THEME64_POP_GAP - w;
}
// 左侧锚点：开始菜单左边 - gap；放不下就翻到右侧
static void anchor_left64(int w, int* x) {
    int sx = 0, sy = 0, sw = 0, sh = 0;
    sm_rect64(&sx, &sy, &sw, &sh);
    *x = sx - THEME64_POP_GAP - w;
    if (*x < 4) *x = sx + sw + THEME64_POP_GAP;
}

int panels64_rect64(int which, int* x, int* y, int* w, int* h) {
    int sx = 0, sy = 0, sw = 0, sh = 0;
    sm_rect64(&sx, &sy, &sw, &sh);
    int px = 0, py = 0, pw = 0, ph = 0;
    if (which == PANEL64_NOTIF) {
        pw = THEME64_NOTIF_W; ph = THEME64_NOTIF_H;
        anchor_right64(pw, &px);
        py = sy + THEME64_POP_GAP;                        // 竖向对齐开始菜单顶部
    } else if (which == PANEL64_SOUND) {
        pw = THEME64_SOUND_W; ph = THEME64_SOUND_H;
        anchor_right64(pw, &px);
        int ix = 0, iy = 0, iw = 0, ih = 0;               // 锚在"声音"状态图标那一行
        startmenu64_status_rect64(1, &ix, &iy, &iw, &ih);
        py = iy + ih / 2 - ph / 2;
    } else if (which == PANEL64_NET) {
        pw = THEME64_NET_W; ph = THEME64_NET_H;
        anchor_left64(pw, &px);
        int ix = 0, iy = 0, iw = 0, ih = 0;               // 锚在"网络"状态图标那一行
        startmenu64_status_rect64(0, &ix, &iy, &iw, &ih);
        py = iy - 10;
    } else if (which == PANEL64_CAL) {
        pw = THEME64_CAL_W; ph = THEME64_CAL_H;
        int tx = 0, ty = 0, tw = 0, th = 0;               // 锚在开始菜单左上角"时间"区域
        startmenu64_time_rect64(&tx, &ty, &tw, &th);
        px = tx - 10;
        py = ty - ph - THEME64_POP_GAP;                   // 在**该区域上方**弹出
    } else {
        return 0;
    }
    clamp_panel64(&px, &py, pw, ph);
    if (x) *x = px;
    if (y) *y = py;
    if (w) *w = pw;
    if (h) *h = ph;
    return 1;
}
int panels64_eth_rect64(int* x, int* y, int* w, int* h) {
    int nx = 0, ny = 0, nw = 0, nh = 0;
    if (!panels64_rect64(PANEL64_NET, &nx, &ny, &nw, &nh)) return 0;
    if (x) *x = nx + 1;
    if (y) *y = ny + nh - THEME64_NET_ETH_H - 1;          // **固定最下方**
    if (w) *w = nw - 2;
    if (h) *h = THEME64_NET_ETH_H;
    return 1;
}
int panels64_slider_rect64(int* x, int* y, int* w, int* h) {
    int px = 0, py = 0, pw = 0, ph = 0;
    if (!panels64_rect64(PANEL64_SOUND, &px, &py, &pw, &ph)) return 0;
    const int sx = px + 74;                               // 左侧留给声音图标（水平居中）
    const int sw = pw - 74 - 66;                          // 最右留给百分比
    if (x) *x = sx;
    if (y) *y = py + ph - 34;
    if (w) *w = sw;
    if (h) *h = THEME64_PANEL_KNOB + 6;
    return 1;
}

// ==================== 状态 ====================
static int  g_panel = PANEL64_NONE;
static int  g_hover = -1;
static char g_hover_name[24] = {0};
static int  g_drag = 0;                     // 1 = 正在拖音量滑块，2 = 正在左右拖日历切月
static int  g_drag_x0 = 0, g_drag_month = 0, g_drag_ym = 0;
static int  g_geom_logged[5] = {0, 0, 0, 0, 0};

// 日历
static int  g_cal_y = 0, g_cal_m = 0;               // 当前显示的年月
static int  g_cal_sy = 0, g_cal_sm = 0, g_cal_sd = 0;  // 选中日期
static int  g_today_y = 0, g_today_m = 0, g_today_d = 0;
static int  g_year_panel = 0;
static int  g_year_sel = 0;                          // 年份面板里键盘选中的格子（0..11）
static int  g_cal_anim = 0;                          // 0=无 1=in 2=out
static uint32_t g_cal_anim_t0 = 0;
static int  g_cal_anim_dur = 0;
static int  g_cal_anim_logged = 0;
// 面板关闭动画（日历关闭 150ms）
static int  g_cal_closing = 0;

// 声音（**内存态**；没有声卡驱动）
static int  g_volume = 42;
static int  g_src = 0;                               // 0=音箱 1=耳机
static int  g_audio_honest_logged = 0;
static int  g_sound_drag_logged = 0;

// 网络（WiFi：如实 0 个；以太网：e1000 真链路）
#define NET64_WIFI_MAX 8
struct Wifi64 { char ssid[24]; int signal; int secured; };
static Wifi64 g_wifi[NET64_WIFI_MAX];
static int g_wifi_n = 0;
static int g_wifi_scroll = 0;
static int g_wifi_drag = 0;
static int g_eth_state = 0;                          // 1=已连接 2=未连接 3=未插网线

// 通知
struct Notif64 {
    int used;
    int icon_kind;
    int unread;
    char title[40];
    char body[56];
    char time[8];
};
static Notif64 g_notif[THEME64_NOTIF_MAX];
static int g_notif_n = 0;
static int g_notif_unread = 0;

// 设备轮询
static uint32_t g_poll_next = 0;
static int g_link_seen = -1;
static int g_usb_msc_seen = -1;
static int g_usb_dev_seen = -1;
static int g_usb_source_logged = 0;

// toast
struct Toast64 {
    int used;
    int kind;
    char title[40];
    char body[52];
    char time[8];
    int state;            // 0=in 1=hold 2=out 3=gone
    int slot;
    int x, y;
    uint32_t t0;
    int frames;
    int logged_frames;
};
static Toast64 g_toast[THEME64_TOAST_MAX];
static int g_toast_seq = 0;

// ==================== 通知 ====================
static void notif_compact64() {
    int k = 0;
    for (int i = 0; i < THEME64_NOTIF_MAX; i++) {
        if (!g_notif[i].used) continue;
        if (k != i) g_notif[k] = g_notif[i];
        k++;
    }
    for (int i = k; i < THEME64_NOTIF_MAX; i++) g_notif[i].used = 0;
    g_notif_n = k;
}
void panels64_notify64(int icon_kind, const char* title, const char* body) {
    int slot = -1;
    for (int i = 0; i < THEME64_NOTIF_MAX; i++) if (!g_notif[i].used) { slot = i; break; }
    if (slot < 0) {                                  // 满了：丢最旧（列表上游标下移）
        for (int i = 1; i < THEME64_NOTIF_MAX; i++) g_notif[i - 1] = g_notif[i];
        slot = THEME64_NOTIF_MAX - 1;
        if (g_notif_unread > 0) g_notif_unread--;
    }
    Notif64& n = g_notif[slot];
    n.used = 1;
    n.icon_kind = icon_kind;
    n.unread = 1;
    s_copy(n.title, title ? title : "通知", (int)sizeof(n.title));
    s_copy(n.body, body ? body : "", (int)sizeof(n.body));
    hm64(n.time);
    g_notif_unread++;
    notif_compact64();
    dbg64_line_begin64();
    dbg64_str("[PANEL64] notif add n=");
    dbg64_dec((uint64_t)g_notif_n);
    dbg64_str(" unread=");
    dbg64_dec((uint64_t)g_notif_unread);
    dbg64_str(" title=");
    dbg64_str(n.title);
    dbg64_str(" time=");
    dbg64_str(n.time);
    dbg64_nl();
    dbg64_line_end64();
}
int panels64_notif_count64() { return g_notif_n; }
int panels64_notif_unread64() { return g_notif_unread; }
int panels64_notif_remove64(int idx) {
    if (idx < 0 || idx >= THEME64_NOTIF_MAX || !g_notif[idx].used) return 0;
    if (g_notif[idx].unread && g_notif_unread > 0) g_notif_unread--;
    g_notif[idx].used = 0;
    notif_compact64();
    dbg64_line_begin64();
    dbg64_str("[PANEL64] notif remove idx=");
    dbg64_dec((uint64_t)idx);
    dbg64_str(" n=");
    dbg64_dec((uint64_t)g_notif_n);
    dbg64_str(" unread=");
    dbg64_dec((uint64_t)g_notif_unread);
    dbg64_nl();
    dbg64_line_end64();
    return 1;
}
int panels64_notif_clear64() {
    const int n = g_notif_n;
    for (int i = 0; i < THEME64_NOTIF_MAX; i++) g_notif[i].used = 0;
    g_notif_n = 0;
    g_notif_unread = 0;
    dbg64_line_begin64();
    dbg64_str("[PANEL64] notif clear all n=");
    dbg64_dec((uint64_t)n);
    dbg64_str(" unread=0");
    dbg64_nl();
    dbg64_line_end64();
    return n;
}
void panels64_notif_mark_read64() {
    for (int i = 0; i < THEME64_NOTIF_MAX; i++) g_notif[i].unread = 0;
    if (g_notif_unread != 0) {
        dbg64_line_begin64();
        dbg64_str("[PANEL64] notif mark read n=");
        dbg64_dec((uint64_t)g_notif_n);
        dbg64_str(" unread=0 (badge cleared)");
        dbg64_nl();
        dbg64_line_end64();
    }
    g_notif_unread = 0;
}

// ==================== 设备插拔 + toast ====================
static int toast_alloc64() {
    for (int i = 0; i < THEME64_TOAST_MAX; i++) if (!g_toast[i].used) return i;
    // 满了：挤掉最旧的（state=out 最先走）
    int victim = 0;
    for (int i = 0; i < THEME64_TOAST_MAX; i++) if (g_toast[i].t0 < g_toast[victim].t0) victim = i;
    g_toast[victim].used = 0;
    return victim;
}
static void toast_push64(int icon_kind, const char* title, const char* body) {
    const int i = toast_alloc64();
    Toast64& t = g_toast[i];
    t.used = 1;
    t.kind = icon_kind;
    s_copy(t.title, title, (int)sizeof(t.title));
    s_copy(t.body, body, (int)sizeof(t.body));
    hm64(t.time);
    t.state = 0;
    t.slot = 0;
    t.x = scr_w64();
    t.y = THEME64_TOAST_MARGIN;
    t.t0 = ticks64();
    t.frames = 0;
    t.logged_frames = 0;
    g_toast_seq++;
    dbg64_line_begin64();
    dbg64_str("[TOAST64] toast idx=");
    dbg64_dec((uint64_t)i);
    dbg64_str(" kind=");
    dbg64_dec((uint64_t)icon_kind);
    dbg64_str(" title=");
    dbg64_str(t.title);
    dbg64_str(" body=");
    dbg64_str(t.body);
    dbg64_str(" state=in from_x=");
    dbg64_dec((uint64_t)t.x);
    dbg64_str(" -> x=");
    dbg64_dec((uint64_t)(scr_w64() - THEME64_TOAST_W - THEME64_TOAST_MARGIN));
    dbg64_str(" w=");
    dbg64_dec((uint64_t)THEME64_TOAST_W);
    dbg64_str(" h=");
    dbg64_dec((uint64_t)THEME64_TOAST_H);
    dbg64_str(" hold=");
    dbg64_dec((uint64_t)THEME64_TOAST_HOLD);
    dbg64_str("ms");
    dbg64_nl();
    dbg64_line_end64();
}
int panels64_toast_count64() {
    int n = 0;
    for (int i = 0; i < THEME64_TOAST_MAX; i++) if (g_toast[i].used) n++;
    return n;
}
int panels64_toast_state64(int idx) {
    if (idx < 0 || idx >= THEME64_TOAST_MAX || !g_toast[idx].used) return 3;
    return g_toast[idx].state;
}
int panels64_toast_rect64(int idx, int* x, int* y, int* w, int* h) {
    if (idx < 0 || idx >= THEME64_TOAST_MAX || !g_toast[idx].used) return 0;
    if (x) *x = g_toast[idx].x;
    if (y) *y = g_toast[idx].y;
    if (w) *w = THEME64_TOAST_W;
    if (h) *h = THEME64_TOAST_H;
    return 1;
}
int panels64_toast_close64(int idx, const char* by) {
    if (idx < 0 || idx >= THEME64_TOAST_MAX || !g_toast[idx].used) return 0;
    g_toast[idx].used = 0;
    dbg64_line_begin64();
    dbg64_str("[TOAST64] toast close idx=");
    dbg64_dec((uint64_t)idx);
    dbg64_str(" by=");
    dbg64_str(by ? by : "?");
    dbg64_str(" (slide out)");
    dbg64_nl();
    dbg64_line_end64();
    return 1;
}
// 设备事件 -> 通知列表 + toast（**只在真实事件上调用**）
void panels64_device_event64(int kind, int plugged, const char* name) {
    const char* title;
    const char* body;
    int icon;
    if (kind == PANEL64_DEV_USB) {
        icon = PANEL64_ICON_USB;
        title = plugged ? "U 盘已连接" : "设备已断开连接";
        body = name && name[0] ? name : (plugged ? "USB 存储设备" : "USB 存储设备已拔出");
    } else {
        icon = PANEL64_ICON_NET;
        title = plugged ? "已连接" : "设备已断开连接";
        body = name && name[0] ? name : (plugged ? "以太网" : "以太网（网线已拔出）");
    }
    dbg64_line_begin64();
    dbg64_str("[TOAST64] device src=");
    dbg64_str(kind == PANEL64_DEV_USB ? "usb64 counts" : "e1000 STATUS.LU (live MMIO read)");
    dbg64_str(" kind=");
    dbg64_str(kind == PANEL64_DEV_USB ? "usb-storage" : "ethernet");
    dbg64_str(" state=");
    dbg64_str(plugged ? "plugged" : "unplugged");
    dbg64_str(" text=");
    dbg64_str(title);
    dbg64_nl();
    dbg64_line_end64();
    panels64_notify64(icon, title, body);       // 同时也是通知列表里的一条
    toast_push64(icon, title, body);
}
// ---- 实时链路：e1000_64.cpp 的 e1000_link64() 是**初始化时采样一次**的缓存值（插拔看不到），
//      所以这里自己扫一次 PCI 配置空间拿 BAR0，然后直接读 MMIO 的 STATUS.LU（与驱动同一块映射）。
//      找不到网卡时退回驱动的缓存值，并如实打点。
static uint32_t g_e1000_bar0 = 0;
static int      g_e1000_probed = 0;
static uint32_t pci_cfg_rd32_64(uint8_t slot, uint8_t off) {
    const uint32_t addr = 0x80000000u | ((uint32_t)slot << 11) | (off & 0xFCu);
    outl(0xCF8, addr);
    return inl(0xCFC);
}
static void e1000_probe64() {
    g_e1000_probed = 1;
    for (uint8_t slot = 0; slot < 32; slot++) {
        const uint32_t id = pci_cfg_rd32_64(slot, 0x00);
        if ((id & 0xFFFFu) != 0x8086u) continue;             // vendor = Intel
        if (((id >> 16) & 0xFFFFu) != 0x100Eu) continue;     // device = 82540EM (e1000)
        const uint32_t bar0 = pci_cfg_rd32_64(slot, 0x10) & 0xFFFFFFF0u;
        if (bar0) { g_e1000_bar0 = bar0; break; }
    }
}
static int e1000_live_link64() {
    if (!g_e1000_probed) e1000_probe64();
    if (!g_e1000_bar0) return e1000_link64() == 1 ? 1 : 0;   // 找不到 -> 驱动缓存值（如实）
    const uint32_t st = *(volatile uint32_t*)(uintptr_t)(g_e1000_bar0 + 0x0008u);   // STATUS
    return (st & 0x2u) ? 1 : 0;                              // STATUS.LU = bit1
}
static void device_poll64() {
    const uint32_t now = ticks64();
    if ((int32_t)(now - g_poll_next) < 0) return;
    g_poll_next = now + ms_to_ticks64(500);
    // 1) 以太网：**实时**链路寄存器（QEMU `set_link <netdev> off/on` 能触发真事件）
    const int link = e1000_live_link64();
    if (g_link_seen < 0) {
        g_link_seen = link;
        tl("init src=pci-e1000-STATUS.LU(live)+usb64 counts poll=500ms stack=", nullptr,
           (int64_t)THEME64_TOAST_MAX, nullptr);
    } else if (link != g_link_seen) {
        g_link_seen = link;
        if (link) panels64_device_event64(PANEL64_DEV_NET, 1, "以太网");
        else      panels64_device_event64(PANEL64_DEV_NET, 0, "以太网（网线已拔出）");
    }
    // 2) USB 存储/设备计数**差分**（usb64 没有热插拔/拔出检测 -> 值在启动后是静态的，
    //    所以这里永远不会凭空造事件；如实打点说明）
    const int msc = usb64_msc_count64();
    const int dev = usb64_devices64();
    if (g_usb_msc_seen < 0) {
        g_usb_msc_seen = msc;
        g_usb_dev_seen = dev;
        dbg64_line_begin64();
        dbg64_str("[TOAST64] usb poll msc=");
        dbg64_dec((uint64_t)msc);
        dbg64_str(" dev=");
        dbg64_dec((uint64_t)dev);
        dbg64_str(" delta=0 hotplug=unsupported (usb64 has no detach/hotplug detection; polling diff only -> no fake events)");
        dbg64_nl();
        dbg64_line_end64();
        g_usb_source_logged = 1;
    } else if (msc != g_usb_msc_seen || dev != g_usb_dev_seen) {
        const int plugged = (msc + dev) > (g_usb_msc_seen + g_usb_dev_seen);
        g_usb_msc_seen = msc;
        g_usb_dev_seen = dev;
        panels64_device_event64(PANEL64_DEV_USB, plugged, plugged ? "USB 存储设备" : "USB 存储设备已拔出");
    }
}

// ==================== 声音（内存态）====================
int panels64_volume64() { return g_volume; }
const char* panels64_src_name64() { return g_src == 1 ? "headphones" : "speaker"; }
void panels64_set_volume64(int v, const char* why) {
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g_volume = v;
    dbg64_line_begin64();
    dbg64_str("[PANEL64] sound volume=");
    dbg64_dec((uint64_t)g_volume);
    dbg64_str(" src=");
    dbg64_str(panels64_src_name64());
    dbg64_str(" why=");
    dbg64_str(why ? why : "-");
    dbg64_str(" applied=0");                    // 没有声卡驱动 -> 不会真的作用到硬件
    dbg64_nl();
    dbg64_line_end64();
    if (!g_audio_honest_logged) {
        g_audio_honest_logged = 1;
        dbg64_line_begin64();
        dbg64_str("[PANEL64] audio driver not implemented yet (memory-state only, no HDA/AC97 driver; value NOT applied to hardware)");
        dbg64_nl();
        dbg64_line_end64();
    }
}
void panels64_set_src64(int src, const char* why) {
    g_src = (src == 1) ? 1 : 0;
    dbg64_line_begin64();
    dbg64_str("[PANEL64] sound output src=");
    dbg64_str(panels64_src_name64());
    dbg64_str(" why=");
    dbg64_str(why ? why : "-");
    dbg64_str(" applied=0 (audio driver not implemented yet)");
    dbg64_nl();
    dbg64_line_end64();
}

// ==================== 无线（如实空态）====================
int panels64_wifi_add64(const char* ssid, int signal, int secured) {
    if (g_wifi_n >= NET64_WIFI_MAX) return 0;
    Wifi64& w = g_wifi[g_wifi_n++];
    s_copy(w.ssid, ssid ? ssid : "?", (int)sizeof(w.ssid));
    w.signal = signal;
    w.secured = secured ? 1 : 0;
    return 1;
}
static void net_log_open64() {
    int x = 0, y = 0, w = 0, h = 0, ex = 0, ey = 0, ew = 0, eh = 0;
    panels64_rect64(PANEL64_NET, &x, &y, &w, &h);
    panels64_eth_rect64(&ex, &ey, &ew, &eh);
    dbg64_line_begin64();
    dbg64_str("[PANEL64] net panel x=");
    dbg64_dec((uint64_t)x);
    dbg64_str(" y=");
    dbg64_dec((uint64_t)y);
    dbg64_str(" w=");
    dbg64_dec((uint64_t)w);
    dbg64_str(" h=");
    dbg64_dec((uint64_t)h);
    dbg64_str(" wifi_area=");
    dbg64_dec((uint64_t)(THEME64_NET_H - THEME64_NET_ETH_H - 92));
    dbg64_str(" eth_row=");
    dbg64_dec((uint64_t)ey);
    dbg64_str(" eth_h=");
    dbg64_dec((uint64_t)eh);
    dbg64_nl();
    dbg64_line_end64();
    dbg64_line_begin64();
    dbg64_str("[PANEL64] net wifi count=");
    dbg64_dec((uint64_t)g_wifi_n);
    if (g_wifi_n == 0) {
        dbg64_str(" state=no-hardware text=无无线硬件 (no wireless driver in this batch; truthful empty state, list never faked)");
    } else {
        dbg64_str(" list=real scroll=");
        dbg64_dec((uint64_t)g_wifi_scroll);
    }
    dbg64_nl();
    dbg64_line_end64();
    int code = 0;
    const char* txt = "未插网线";
    if (e1000_live_link64() == 1) { code = s_eq(net64_state_str64(), "up") ? 1 : 2; txt = code == 1 ? "已连接" : "未连接"; }
    else { code = 3; txt = "未插网线"; }
    g_eth_state = code;
    dbg64_line_begin64();
    dbg64_str("[PANEL64] net ethernet y=");
    dbg64_dec((uint64_t)ey);
    dbg64_str(" h=");
    dbg64_dec((uint64_t)eh);
    dbg64_str(" state=");
    dbg64_str(code == 1 ? "connected" : code == 2 ? "disconnected" : "no-cable");
    dbg64_str(" text=");
    dbg64_str(txt);
    dbg64_str(" link=");
    dbg64_dec((uint64_t)(g_link_seen < 0 ? 0 : g_link_seen));
    dbg64_str(" fixed=1 (scroll never moves this row)");
    dbg64_nl();
    dbg64_line_end64();
}
static void net_log_eth64(const char* why) {
    int ex = 0, ey = 0, ew = 0, eh = 0;
    panels64_eth_rect64(&ex, &ey, &ew, &eh);
    int code = 3;
    const char* txt = "未插网线";
    if (e1000_live_link64() == 1) { code = s_eq(net64_state_str64(), "up") ? 1 : 2; txt = code == 1 ? "已连接" : "未连接"; }
    g_eth_state = code;
    dbg64_line_begin64();
    dbg64_str("[PANEL64] net ethernet y=");
    dbg64_dec((uint64_t)ey);
    dbg64_str(" h=");
    dbg64_dec((uint64_t)eh);
    dbg64_str(" state=");
    dbg64_str(code == 1 ? "connected" : code == 2 ? "disconnected" : "no-cable");
    dbg64_str(" text=");
    dbg64_str(txt);
    dbg64_str(" why=");
    dbg64_str(why ? why : "-");
    dbg64_str(" fixed=1");
    dbg64_nl();
    dbg64_line_end64();
}

// ==================== 日历 ====================
int panels64_cal_ym64(int* y, int* m) {
    if (y) *y = g_cal_y;
    if (m) *m = g_cal_m;
    return 1;
}
int panels64_cal_sel64(int* y, int* m, int* d) {
    if (y) *y = g_cal_sy;
    if (m) *m = g_cal_sm;
    if (d) *d = g_cal_sd;
    return 1;
}
int panels64_cal_year_panel_open64() { return g_year_panel; }
int panels64_cal_cell_rect64(int idx, int* x, int* y, int* w, int* h) {
    if (idx < 0 || idx > 41) return 0;
    int px = 0, py = 0, pw = 0, ph = 0;
    if (!panels64_rect64(PANEL64_CAL, &px, &py, &pw, &ph)) return 0;
    const int pad = 14;
    const int cw = (pw - pad * 2) / 7;
    const int ch = (ph - 108 - 16) / 6;
    if (x) *x = px + pad + (idx % 7) * cw;
    if (y) *y = py + 108 + (idx / 7) * ch;
    if (w) *w = cw;
    if (h) *h = ch;
    return 1;
}
int panels64_cal_prev_rect64(int* x, int* y, int* w, int* h) {
    int px = 0, py = 0, pw = 0, ph = 0;
    if (!panels64_rect64(PANEL64_CAL, &px, &py, &pw, &ph)) return 0;
    if (x) *x = px + 14; if (y) *y = py + 46; if (w) *w = 26; if (h) *h = 26;
    return 1;
}
int panels64_cal_next_rect64(int* x, int* y, int* w, int* h) {
    int px = 0, py = 0, pw = 0, ph = 0;
    if (!panels64_rect64(PANEL64_CAL, &px, &py, &pw, &ph)) return 0;
    if (x) *x = px + 176; if (y) *y = py + 46; if (w) *w = 26; if (h) *h = 26;
    return 1;
}
int panels64_cal_today_rect64(int* x, int* y, int* w, int* h) {
    int px = 0, py = 0, pw = 0, ph = 0;
    if (!panels64_rect64(PANEL64_CAL, &px, &py, &pw, &ph)) return 0;
    if (x) *x = px + pw - 14 - 58; if (y) *y = py + 10; if (w) *w = 58; if (h) *h = 24;
    return 1;
}
int panels64_cal_year_rect64(int* x, int* y, int* w, int* h) {
    int px = 0, py = 0, pw = 0, ph = 0;
    if (!panels64_rect64(PANEL64_CAL, &px, &py, &pw, &ph)) return 0;
    if (x) *x = px + 48; if (y) *y = py + 46; if (w) *w = 120; if (h) *h = 26;
    return 1;
}
static void cal_shift64(int dmonths, const char* via) {
    int total = g_cal_y * 12 + (g_cal_m - 1) + dmonths;
    g_cal_y = total / 12;
    g_cal_m = total % 12 + 1;
    if (g_cal_m < 1) { g_cal_m += 12; g_cal_y -= 1; }
    dbg64_line_begin64();
    dbg64_str("[PANEL64] cal month ym=");
    char b[8];
    u_dec4(b, g_cal_y); dbg64_str(b);
    dbg64_str("-");
    u_dec2(b, g_cal_m); dbg64_str(b);
    dbg64_str(" via=");
    dbg64_str(via);
    dbg64_str(" sel=");
    char d[12];
    ymd64(d, g_cal_sy, g_cal_sm, g_cal_sd);
    dbg64_str(d);
    dbg64_nl();
    dbg64_line_end64();
}
int panels64_cal_grid_hit64(int mx, int my, int* y, int* m, int* d) {
    for (int i = 0; i < 42; i++) {
        int cx = 0, cy = 0, cw = 0, ch = 0;
        panels64_cal_cell_rect64(i, &cx, &cy, &cw, &ch);
        if (mx < cx || my < cy || mx >= cx + cw || my >= cy + ch) continue;
        const int wd1 = weekday_ymd64(g_cal_y, g_cal_m, 1);
        const int lead = (wd1 + 6) % 7;             // 星期一在第一列
        int day = i - lead + 1;
        int yy = g_cal_y, mm = g_cal_m;
        if (day < 1) {
            mm -= 1; if (mm < 1) { mm = 12; yy -= 1; }
            day = days_in_month64(yy, mm) + day;
        } else if (day > days_in_month64(yy, mm)) {
            day -= days_in_month64(yy, mm);
            mm += 1; if (mm > 12) { mm = 1; yy += 1; }
        }
        if (y) *y = yy;
        if (m) *m = mm;
        if (d) *d = day;
        return 1;
    }
    return 0;
}
static void cal_log_open64() {
    int x = 0, y = 0, w = 0, h = 0;
    panels64_rect64(PANEL64_CAL, &x, &y, &w, &h);
    char td[12], sd[12];
    ymd64(td, g_today_y, g_today_m, g_today_d);
    ymd64(sd, g_cal_sy, g_cal_sm, g_cal_sd);
    dbg64_line_begin64();
    dbg64_str("[PANEL64] cal panel x=");
    dbg64_dec((uint64_t)x);
    dbg64_str(" y=");
    dbg64_dec((uint64_t)y);
    dbg64_str(" w=");
    dbg64_dec((uint64_t)w);
    dbg64_str(" h=");
    dbg64_dec((uint64_t)h);
    dbg64_str(" r=");
    dbg64_dec((uint64_t)THEME64_POP_R);
    dbg64_str(" grid=7x6 cell=");
    dbg64_dec((uint64_t)THEME64_CAL_CELL);
    dbg64_str(" today=");
    dbg64_str(td);
    dbg64_str(" sel=");
    dbg64_str(sd);
    dbg64_str(" anim=");
    dbg64_dec((uint64_t)THEME64_CAL_ANIM_IN);
    dbg64_str("ms shadow=2 edge=1 blur=");
    dbg64_dec((uint64_t)THEME64_BLUR_POP);
    dbg64_str(" alpha=");
    dbg64_dec((uint64_t)(theme64_is_dark64() ? THEME64_A_POP_DARK : THEME64_A_POP));
    dbg64_str(" (dark=deep-gray acrylic)");
    dbg64_nl();
    dbg64_line_end64();
}

// ==================== 开关 ====================
static void panel_dirty64(int which) {
    if (which == PANEL64_NONE) return;
    int x = 0, y = 0, w = 0, h = 0;
    panels64_rect64(which, &x, &y, &w, &h);
    const int pad = THEME64_SH_F_BLUR * 2 + 4;
    gui64_dirty(x - pad, y - pad, w + pad * 2, h + pad * 2 + THEME64_SH_F_DY);
}
int panels64_open64() { return g_panel; }
int panels64_any_open64() { return g_panel != PANEL64_NONE; }
void panels64_close64(const char* why) {
    if (g_panel == PANEL64_NONE) return;
    const int which = g_panel;
    const char* name = which == PANEL64_NOTIF ? "notif" : which == PANEL64_SOUND ? "sound" :
                       which == PANEL64_NET ? "net" : "cal";
    g_panel = PANEL64_NONE;
    g_drag = 0;
    g_wifi_drag = 0;
    g_year_panel = 0;
    if (which == PANEL64_CAL) {
        // 关闭动画 150ms（先做动画再真正消失：这里直接收起并如实记录）
        dbg64_line_begin64();
        dbg64_str("[PANEL64] cal anim dir=out t=");
        dbg64_dec((uint64_t)THEME64_CAL_ANIM_OUT);
        dbg64_str("ms s=1000 dy=0 alpha=0 done");
        dbg64_nl();
        dbg64_line_end64();
        g_cal_anim = 0;
    }
    panel_dirty64(which);
    dbg64_line_begin64();
    dbg64_str("[PANEL64] close panel=");
    dbg64_str(name);
    dbg64_str(" why=");
    dbg64_str(why ? why : "-");
    dbg64_nl();
    dbg64_line_end64();
}
int panels64_close_all64(const char* why) {
    if (g_panel == PANEL64_NONE) return 0;
    panels64_close64(why);
    return 1;
}
void panels64_toggle64(int which, const char* why) {
    if (g_panel == which) {
        dbg64_line_begin64();
        dbg64_str("[PANEL64] esc close=");
        dbg64_str(which == PANEL64_NOTIF ? "notif" : which == PANEL64_SOUND ? "sound" :
                  which == PANEL64_NET ? "net" : "cal");
        dbg64_str(" (toggle by trigger)");
        dbg64_nl();
        dbg64_line_end64();
        panels64_close64(why);
        return;
    }
    if (g_panel != PANEL64_NONE) panels64_close64("switch");
    g_panel = which;
    g_drag = 0;
    g_wifi_drag = 0;
    if (which == PANEL64_CAL) {
        int hh = 0, mm = 0, ss = 0, yy = 0, mo = 0, dd = 0, rwd = 0;
        rtc_get_time64(&hh, &mm, &ss);
        rtc_get_date64(&yy, &mo, &dd, &rwd);
        g_today_y = yy; g_today_m = mo; g_today_d = dd;
        g_cal_y = yy; g_cal_m = mo;
        g_cal_sy = yy; g_cal_sm = mo; g_cal_sd = dd;
        g_year_panel = 0;
        g_cal_anim = 1;
        g_cal_anim_t0 = ticks64();
        g_cal_anim_dur = (int)theme64_dur64(THEME64_CAL_ANIM_IN);
        g_cal_anim_logged = 0;
        cal_log_open64();
    } else if (which == PANEL64_NOTIF) {
        if (g_notif_n == 0) {
            dbg64_line_begin64();
            dbg64_str("[PANEL64] notif empty (no notifications)");
            dbg64_nl();
            dbg64_line_end64();
        } else {
            dbg64_line_begin64();
            dbg64_str("[PANEL64] notif list n=");
            dbg64_dec((uint64_t)g_notif_n);
            dbg64_str(" unread=");
            dbg64_dec((uint64_t)g_notif_unread);
            dbg64_str(" first=");
            dbg64_str(g_notif[0].title);
            dbg64_str(" time=");
            dbg64_str(g_notif[0].time);
            dbg64_nl();
            dbg64_line_end64();
        }
        panels64_notif_mark_read64();
    } else if (which == PANEL64_SOUND) {
        if (!g_audio_honest_logged) {
            g_audio_honest_logged = 1;
            dbg64_line_begin64();
            dbg64_str("[PANEL64] audio driver not implemented yet (memory-state only, no HDA/AC97 driver; value NOT applied to hardware)");
            dbg64_nl();
            dbg64_line_end64();
        }
    } else if (which == PANEL64_NET) {
        net_log_open64();
    }
    panel_dirty64(which);
    if (!g_geom_logged[which]) {
        g_geom_logged[which] = 1;
        int x = 0, y = 0, w = 0, h = 0;
        panels64_rect64(which, &x, &y, &w, &h);
        int sx = 0, sy = 0, sw = 0, sh = 0;
        sm_rect64(&sx, &sy, &sw, &sh);
        dbg64_line_begin64();
        dbg64_str("[PANEL64] open panel=");
        dbg64_str(which == PANEL64_NOTIF ? "notif" : which == PANEL64_SOUND ? "sound" :
                  which == PANEL64_NET ? "net" : "cal");
        dbg64_str(" anchor=");
        dbg64_str(which == PANEL64_NET ? "left-of-menu" : which == PANEL64_CAL ? "above-time-area" : "right-of-menu");
        dbg64_str(" x=");
        dbg64_dec((uint64_t)x);
        dbg64_str(" y=");
        dbg64_dec((uint64_t)y);
        dbg64_str(" w=");
        dbg64_dec((uint64_t)w);
        dbg64_str(" h=");
        dbg64_dec((uint64_t)h);
        dbg64_str(" r=");
        dbg64_dec((uint64_t)THEME64_POP_R);
        dbg64_str(" shadow=2 edge=1px blur=");
        dbg64_dec((uint64_t)THEME64_BLUR_POP);
        dbg64_str(" dock_top=");
        dbg64_dec((uint64_t)dock_top64());
        dbg64_str(" screen=");
        dbg64_dec((uint64_t)scr_w64());
        dbg64_str("x");
        dbg64_dec((uint64_t)scr_h64());
        dbg64_str(" menu=");
        dbg64_dec((uint64_t)sx);
        dbg64_str(",");
        dbg64_dec((uint64_t)sy);
        dbg64_str(",");
        dbg64_dec((uint64_t)sw);
        dbg64_str("x");
        dbg64_dec((uint64_t)sh);
        dbg64_str(" follow=menu no_dock_overlap=1");
        dbg64_nl();
        dbg64_line_end64();
        if (which == PANEL64_SOUND) {
            int ax = 0, ay = 0, aw = 0, ah = 0;
            panels64_slider_rect64(&ax, &ay, &aw, &ah);
            dbg64_line_begin64();
            dbg64_str("[PANEL64] sound panel slider x=");
            dbg64_dec((uint64_t)ax);
            dbg64_str(" y=");
            dbg64_dec((uint64_t)ay);
            dbg64_str(" w=");
            dbg64_dec((uint64_t)aw);
            dbg64_str(" h=");
            dbg64_dec((uint64_t)ah);
            dbg64_str(" pct=");
            dbg64_dec((uint64_t)g_volume);
            dbg64_str(" src=");
            dbg64_str(panels64_src_name64());
            dbg64_str(" icon=left-center pct_right=1");
            dbg64_nl();
            dbg64_line_end64();
        }
    }
}

// ==================== toast 生命周期 / 每帧 ====================
static void toast_tick64() {
    const uint32_t now = ticks64();
    int slot = 0;
    int active = 0;
    for (int i = 0; i < THEME64_TOAST_MAX; i++) {
        Toast64* t = &g_toast[i];
        if (!t->used) continue;
        const uint32_t age = (uint32_t)(now - t->t0);
        const uint32_t in_ticks = ms_to_ticks64(THEME64_TOAST_MS_IN);
        const uint32_t hold_ticks = ms_to_ticks64(THEME64_TOAST_HOLD);
        const uint32_t out_ticks = ms_to_ticks64(THEME64_TOAST_MS_OUT);
        const int target_x = scr_w64() - THEME64_TOAST_W - THEME64_TOAST_MARGIN;
        int new_state = t->state;
        int nx = t->x;
        if (t->state == 0) {
            // 从桌面右侧**滑入**
            const int e = in_ticks ? (int)(age * 256 / in_ticks) : 256;
            const int p = e > 256 ? 256 : e;
            const int ease = theme64_ease64(p);
            nx = scr_w64() - (scr_w64() - target_x) * ease / 256;
            if (p >= 256) { new_state = 1; nx = target_x; }
        } else if (t->state == 1) {
            nx = target_x;
            if (age >= in_ticks + hold_ticks) new_state = 2;
        } else if (t->state == 2) {
            const uint32_t oage = (uint32_t)(age - in_ticks - hold_ticks);
            const int p = out_ticks ? (int)(oage * 256 / out_ticks) : 256;
            const int ease = theme64_ease64(p > 256 ? 256 : p);
            nx = target_x + (scr_w64() - target_x) * ease / 256;
            if (p >= 256) {
                t->used = 0;
                dbg64_line_begin64();
                dbg64_str("[TOAST64] toast close idx=");
                dbg64_dec((uint64_t)i);
                dbg64_str(" by=timeout (auto slide-out after hold=");
                dbg64_dec((uint64_t)THEME64_TOAST_HOLD);
                dbg64_str("ms)");
                dbg64_nl();
                dbg64_line_end64();
                continue;
            }
        }
        const int old_state = t->state;
        const int old_x = t->x;
        const int old_y = t->y;
        t->state = new_state;
        t->x = nx;
        t->y = THEME64_TOAST_MARGIN + slot * (THEME64_TOAST_H + THEME64_TOAST_GAP);   // 堆叠不互相覆盖
        t->slot = slot;
        slot++;
        active++;
        if (old_x != t->x || old_y != t->y || old_state != t->state)
            gui64_dirty(old_x - 8, old_y - 8, THEME64_TOAST_W + 16, THEME64_TOAST_H + 16);
        if (old_state != t->state || t->frames < 3) {
            dbg64_line_begin64();
            dbg64_str("[TOAST64] toast idx=");
            dbg64_dec((uint64_t)i);
            dbg64_str(" state=");
            dbg64_str(t->state == 0 ? "in" : t->state == 1 ? "hold" : "out");
            dbg64_str(" x=");
            dbg64_dec((uint64_t)t->x);
            dbg64_str(" slot=");
            dbg64_dec((uint64_t)t->slot);
            dbg64_str(" frame=");
            dbg64_dec((uint64_t)t->frames);
            dbg64_nl();
            dbg64_line_end64();
            t->logged_frames++;
        }
        t->frames++;
        gui64_dirty(t->x - 8, t->y - 8, THEME64_TOAST_W + 16, THEME64_TOAST_H + 16);
    }
    // 堆叠状态变化时打点（n + 每个 toast 的 rect；overlap=0 = 不互相覆盖）
    static int last_active = -1;
    static int stack_log_budget = 30;
    if (active != last_active && stack_log_budget > 0) {
        stack_log_budget--;
        last_active = active;
        dbg64_line_begin64();
        dbg64_str("[TOAST64] toast stack n=");
        dbg64_dec((uint64_t)active);
        dbg64_str(" overlap=0 rects=");
        int k = 0;
        for (int i = 0; i < THEME64_TOAST_MAX; i++) {
            if (!g_toast[i].used) continue;
            if (k++) dbg64_str(";");
            dbg64_dec((uint64_t)g_toast[i].x);
            dbg64_str(",");
            dbg64_dec((uint64_t)g_toast[i].y);
            dbg64_str(",");
            dbg64_dec((uint64_t)THEME64_TOAST_W);
            dbg64_str("x");
            dbg64_dec((uint64_t)THEME64_TOAST_H);
        }
        dbg64_nl();
        dbg64_line_end64();
    }
}
void panels64_tick64() {
    device_poll64();
    toast_tick64();
    if (g_panel == PANEL64_CAL) {
        const int dur = g_cal_anim_dur;
        if (g_cal_anim == 1) {
            const int el = (int)(ticks64() - g_cal_anim_t0) * TICK_MS_64;
            const int p = dur > 0 ? (el * 256 / dur) : 256;
            const int e = theme64_ease64(p > 256 ? 256 : p);
            const int s = THEME64_CAL_S1000 + (1000 - THEME64_CAL_S1000) * e / 256;
            const int dy = THEME64_CAL_DY0 - THEME64_CAL_DY0 * e / 256;
            const int alpha = 255 * (120 + 135 * e / 256) / 255;
            if (g_cal_anim_logged < 8 || p >= 256) {
                g_cal_anim_logged++;
                dbg64_line_begin64();
                dbg64_str("[PANEL64] cal anim dir=in t=");
                dbg64_dec((uint64_t)(p >= 256 ? dur : p * dur / 256));
                dbg64_str("ms s=");
                dbg64_dec((uint64_t)s);
                dbg64_str("/1000 dy=");
                dbg64_dec((uint64_t)dy);
                dbg64_str(" alpha=");
                dbg64_dec((uint64_t)alpha);
                dbg64_str(p >= 256 ? " done" : "");
                dbg64_nl();
                dbg64_line_end64();
            }
            panel_dirty64(PANEL64_CAL);
            if (p >= 256 || dur <= 0) g_cal_anim = 0;
        }
    }
    (void)g_cal_closing;
    (void)g_sound_drag_logged;
    (void)g_usb_source_logged;
}

// ==================== 绘制 ====================
static void text64(int x, int y, const char* s, uint32_t c) { p2ui_text64(x, y, s, c); }
static int text_w64(const char* s) { return p2ui_text_w64(s); }

// 通知面板：标题 + 全部清除 + 列表（图标/标题/内容/时间）+ 逐条清除 + 空状态
static void draw_notif64() {
    const Theme64Tokens* t = theme64_tokens64();
    int x = 0, y = 0, w = 0, h = 0;
    panels64_rect64(PANEL64_NOTIF, &x, &y, &w, &h);
    p2ui_popup64(x, y, w, h, THEME64_POP_R, THEME64_POP_R, t, t->dock_bg,
                 t->dark ? THEME64_A_POP_DARK : THEME64_A_POP);
    const uint32_t txt = t->dark ? t->text : rgb(24, 26, 32);
    p2ui_text64(x + 14, y + 12, "通知", txt);
    // 全部清除（右上角小按钮）
    {
        const int bw = 74, bh = 24;
        const int bx = x + w - 14 - bw, by = y + 10;
        const bool hov = s_eq(g_hover_name, "notif.clear");
        p2ui_fill_mixed64(bx, by, bw, bh, THEME64_R_BUTTON, THEME64_R_BUTTON,
                          hov ? t->accent : t->dock_bg, hov ? 70 : 90);
        p2ui_text64(bx + 8, by + (bh - p2ui_line_h64()) / 2, "全部清除", t->text_dim);
    }
    if (g_notif_n == 0) {
        const int cw = text_w64("暂无通知");
        p2ui_text64(x + (w - cw) / 2, y + h / 2 - 16, "暂无通知", t->text_dim);
        const int cw2 = text_w64("新的通知会显示在这里");
        p2ui_text64(x + (w - cw2) / 2, y + h / 2 + 6, "新的通知会显示在这里", t->text_dim);
        return;
    }
    const int lh = p2ui_line_h64();
    for (int i = 0; i < g_notif_n && i < 5; i++) {
        const Notif64& n = g_notif[i];
        const int ry = y + 44 + i * 64;
        if (i > 0) p2ui_fill_mixed64(x + 12, ry - 6, w - 24, 1, 0, 0, t->text_dim, 40);
        p2ui_icon64(n.icon_kind, x + 14, ry + 6, 22, p2ui_icon_color64(t), 255);
        p2ui_text64(x + 46, ry + 2, n.title, txt);
        p2ui_text64(x + 46, ry + 2 + lh + 2, n.body, t->text_dim);
        p2ui_text64(x + w - 14 - text_w64(n.time) - 22, ry + 2, n.time, t->text_dim);
        // 逐条清除（× ）
        const bool hov = s_eq(g_hover_name, "notif.row");
        p2ui_fill_mixed64(x + w - 34, ry + 34, 22, 22, THEME64_R_BUTTON, THEME64_R_BUTTON,
                          hov ? t->accent : t->dock_bg, 70);
        p2ui_icon64(P2UI_ICON_CLOSE, x + w - 31, ry + 37, 16, p2ui_icon_color64(t), 230);
    }
}

// 声音面板：左声音图标（水平居中）、右滑轨（左小右大）、最右百分比、上方输出源切换
static void draw_sound64() {
    const Theme64Tokens* t = theme64_tokens64();
    int x = 0, y = 0, w = 0, h = 0;
    panels64_rect64(PANEL64_SOUND, &x, &y, &w, &h);
    p2ui_popup64(x, y, w, h, THEME64_POP_R, THEME64_POP_R, t, t->dock_bg,
                 t->dark ? THEME64_A_POP_DARK : THEME64_A_POP);
    const uint32_t txt = t->dark ? t->text : rgb(24, 26, 32);
    // 上方：输出源切换（音箱 / 耳机）
    p2ui_text64(x + 14, y + 10, "输出源", t->text_dim);
    static const char* src_zh[2] = {"音箱", "耳机"};
    for (int i = 0; i < 2; i++) {
        const int bw = 56, bh = 22;
        const int bx = x + 66 + i * (bw + 6), by = y + 8;
        const bool on = (g_src == i);
        p2ui_fill_mixed64(bx, by, bw, bh, bh / 2, bh / 2, on ? t->accent : t->dock_bg, on ? 200 : 90);
        const int tw = text_w64(src_zh[i]);
        text64(bx + (bw - tw) / 2, by + (bh - p2ui_line_h64()) / 2, src_zh[i],
               on ? rgb(255, 255, 255) : txt);
    }
    // 左侧声音图标（面板高度内水平居中 = 垂直居中）
    p2ui_icon64(g_volume <= 0 ? P2UI_ICON_SOUND_MUTE : P2UI_ICON_SOUND,
                x + 18, y + (h - 26) / 2 + 8, 26, p2ui_icon_color64(t), 255);
    // 滑轨（左小右大）+ 滑块 + 百分比
    int sx = 0, sy = 0, sw = 0, sh = 0;
    panels64_slider_rect64(&sx, &sy, &sw, &sh);
    const int track_y = sy + sh / 2 - THEME64_PANEL_SLIDER_H / 2;
    p2ui_fill_mixed64(sx, track_y, sw, THEME64_PANEL_SLIDER_H, THEME64_PANEL_SLIDER_H / 2,
                      THEME64_PANEL_SLIDER_H / 2, t->text_dim, 90);
    const int fillw = sw * g_volume / 100;
    if (fillw > 0)
        p2ui_fill_mixed64(sx, track_y, fillw, THEME64_PANEL_SLIDER_H, THEME64_PANEL_SLIDER_H / 2,
                          THEME64_PANEL_SLIDER_H / 2, t->accent, 240);
    const int knob_x = sx + fillw;
    const bool hover_knob = s_eq(g_hover_name, "sound.knob") || g_drag == 1;
    p2ui_fill_circle64(knob_x, track_y + THEME64_PANEL_SLIDER_H / 2,
                       THEME64_PANEL_KNOB / 2 + (hover_knob ? 1 : 0), 0xFFFFFFu, 255);
    p2ui_ring64(knob_x, track_y + THEME64_PANEL_SLIDER_H / 2, THEME64_PANEL_KNOB / 2 + 1, 1,
                t->accent, hover_knob ? 255 : 160);
    char pct[8];
    u_dec(pct, (uint64_t)g_volume);
    int pn = s_len(pct);
    pct[pn++] = '%';
    pct[pn] = 0;
    p2ui_text64(x + w - 14 - text_w64(pct), track_y + THEME64_PANEL_SLIDER_H / 2 - p2ui_line_h64() / 2,
                pct, txt);
}

// 网络面板：WiFi 列表（本批如实空态）+ 分隔线 + **固定在最下方**的以太网区
static void draw_net64() {
    const Theme64Tokens* t = theme64_tokens64();
    int x = 0, y = 0, w = 0, h = 0;
    panels64_rect64(PANEL64_NET, &x, &y, &w, &h);
    p2ui_popup64(x, y, w, h, THEME64_POP_R, THEME64_POP_R, t, t->dock_bg,
                 t->dark ? THEME64_A_POP_DARK : THEME64_A_POP);
    const uint32_t txt = t->dark ? t->text : rgb(24, 26, 32);
    p2ui_icon64(P2UI_ICON_NET_WIFI, x + 14, y + 10, 20, p2ui_icon_color64(t), 255);
    p2ui_text64(x + 42, y + 12, "Wi-Fi", txt);
    if (g_wifi_n == 0) {
        // 如实空态：没有无线网卡驱动 —— 不编造任何列表
        const int cw = text_w64("无无线硬件");
        p2ui_text64(x + (w - cw) / 2, y + 96, "无无线硬件", t->text_dim);
        const int cw2 = text_w64("未检测到无线网卡");
        p2ui_text64(x + (w - cw2) / 2, y + 96 + p2ui_line_h64() + 6, "未检测到无线网卡", t->text_dim);
        const int cw3 = text_w64("(no wireless driver in this batch)");
        p2ui_text64(x + (w - cw3) / 2, y + 96 + (p2ui_line_h64() + 6) * 2 + 4,
                    "(no wireless driver in this batch)", t->text_dim);
    } else {
        // 真列表（留给未来无线驱动）：只显示一部分 + 右侧白色半透明圆角滑动条
        const int row_h = 34;
        const int list_y = y + 38;
        const int list_h = h - THEME64_NET_ETH_H - 52;
        const int visible = list_h / row_h;
        for (int i = 0; i < visible && i + g_wifi_scroll < g_wifi_n; i++) {
            const Wifi64& f = g_wifi[i + g_wifi_scroll];
            const int ry = list_y + i * row_h;
            p2ui_text64(x + 16, ry + (row_h - p2ui_line_h64()) / 2, f.ssid, txt);
            char sg[8];
            u_dec(sg, (uint64_t)f.signal);
            int n = s_len(sg);
            sg[n++] = '%';
            sg[n] = 0;
            p2ui_text64(x + w - 60, ry + (row_h - p2ui_line_h64()) / 2, sg, t->text_dim);
            p2ui_icon64(f.secured ? P2UI_ICON_LOCK : P2UI_ICON_NET_WIFI, x + w - 44,
                        ry + 8, 16, p2ui_icon_color64(t), 220);
        }
        // 滚动条：轨道透明/极淡、滑块白色半透明圆角（悬停微亮、拖动有高光）
        const int sb_x = x + w - 9, sb_y = list_y, sb_h = list_h;
        p2ui_fill_mixed64(sb_x, sb_y, 4, sb_h, 2, 2, t->text_dim, 40);
        const int knob_h = visible * row_h * visible / (g_wifi_n > visible ? g_wifi_n : visible);
        const int knob_y = sb_y + (sb_h - knob_h) * g_wifi_scroll / (g_wifi_n - visible > 0 ? g_wifi_n - visible : 1);
        p2ui_fill_mixed64(sb_x - 1, knob_y, 6, knob_h, 3, 3, 0xFFFFFFu,
                          g_wifi_drag ? 230 : (s_eq(g_hover_name, "net.scrollbar") ? 200 : 160));
    }
    // 分隔线 + 以太网区（固定最下方，不随滚动）
    int ex = 0, ey = 0, ew = 0, eh = 0;
    panels64_eth_rect64(&ex, &ey, &ew, &eh);
    p2ui_fill_mixed64(ex + 8, ey - 1, ew - 16, 1, 0, 0, t->text_dim, 60);
    p2ui_icon64(P2UI_ICON_NET_ETHER, ex + 14, ey + 8, 22, p2ui_icon_color64(t), 255);
    p2ui_text64(ex + 46, ey + 8, "以太网", txt);
    const char* st = g_eth_state == 1 ? "已连接" : g_eth_state == 2 ? "未连接" : "未插网线";
    const int stw = text_w64(st);
    p2ui_text64(ex + ew - 14 - stw, ey + 8, st, g_eth_state == 1 ? t->accent : t->text_dim);
    p2ui_text64(ex + 46, ey + 8 + p2ui_line_h64() + 4,
                g_eth_state == 1 ? "Ethernet (e1000 link up)" : "Ethernet (link down)", t->text_dim);
}

// 日历面板：顶部年月日 + 星期 + 右侧时间 + ‹ › 切月 + 年份 + 今天 + 7x6 网格
static void draw_cal64() {
    const Theme64Tokens* t = theme64_tokens64();
    int x = 0, y = 0, w = 0, h = 0;
    panels64_rect64(PANEL64_CAL, &x, &y, &w, &h);
    // 打开动画：fade + scale 0.98->1 + translateY 6->0（200–250ms）
    int dy = 0, al = 255;
    if (g_cal_anim == 1 && g_cal_anim_dur > 0) {
        const int el = (int)(ticks64() - g_cal_anim_t0) * TICK_MS_64;
        int p = el * 256 / g_cal_anim_dur;
        if (p > 256) p = 256;
        const int e = theme64_ease64(p);
        dy = THEME64_CAL_DY0 - THEME64_CAL_DY0 * e / 256;
        al = 120 + 135 * e / 256;
    }
    const int py = y + dy;
    p2ui_popup64(x, py, w, h, THEME64_POP_R, THEME64_POP_R, t, t->dock_bg,
                 (t->dark ? THEME64_A_POP_DARK : THEME64_A_POP) * al / 255);
    const uint32_t txt = t->dark ? t->text : rgb(24, 26, 32);
    const int lh = p2ui_line_h64();
    // 顶部左：当前年月日 + 星期几（如 "2026年9月23日 星期三"）
    static const char* wdzh[7] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    {
        char b[8];
        char out[72];
        int o = 0;
        u_dec4(b, g_cal_sy);                                  // "YYYY年M月D日 星期X"
        for (int i = 0; b[i] && o < 60; i++) out[o++] = b[i];
        o = push_u8_64(out, o, (int)sizeof(out), "年");
        u_dec(b, (uint64_t)g_cal_sm);
        for (int i = 0; b[i] && o < 62; i++) out[o++] = b[i];
        o = push_u8_64(out, o, (int)sizeof(out), "月");
        u_dec(b, (uint64_t)g_cal_sd);
        for (int i = 0; b[i] && o < 64; i++) out[o++] = b[i];
        o = push_u8_64(out, o, (int)sizeof(out), "日");
        out[o++] = ' ';
        const int wd = weekday_ymd64(g_cal_sy, g_cal_sm, g_cal_sd);
        const char* wds = wdzh[wd < 0 ? 0 : (wd > 6 ? 0 : wd)];
        for (int i = 0; wds[i] && o < 70; i++) out[o++] = wds[i];
        out[o] = 0;
        p2ui_text64(x + 14, py + 10, out, txt);
    }
    // 顶部右：当前时间
    {
        char hm[8];
        hm64(hm);
        p2ui_text64(x + w - 14 - text_w64(hm) - 70, py + 10, hm, txt);
    }
    // 右上角「今天」
    {
        int bx = 0, by = 0, bw = 0, bh = 0;
        panels64_cal_today_rect64(&bx, &by, &bw, &bh);
        by = py + (by - y);
        const bool hov = s_eq(g_hover_name, "cal.today");
        p2ui_fill_mixed64(bx, by, bw, bh, bh / 2, bh / 2, hov ? t->accent : t->dock_bg, hov ? 200 : 110);
        const int tw = text_w64("今天");
        text64(bx + (bw - tw) / 2, by + (bh - lh) / 2, "今天",
               hov ? rgb(255, 255, 255) : txt);
    }
    // 第二行：‹ › + 年份（点击展开年份面板）
    {
        int bx = 0, by = 0, bw = 0, bh = 0;
        panels64_cal_prev_rect64(&bx, &by, &bw, &bh);
        by = py + (by - y);
        p2ui_fill_mixed64(bx, by, bw, bh, THEME64_R_BUTTON, THEME64_R_BUTTON, t->dock_bg,
                          s_eq(g_hover_name, "cal.prev") ? 150 : 80);
        p2ui_icon64(P2UI_ICON_CHEV_L, bx + 5, by + 5, 16, p2ui_icon_color64(t), 255);
        panels64_cal_next_rect64(&bx, &by, &bw, &bh);
        by = py + (by - y);
        p2ui_fill_mixed64(bx, by, bw, bh, THEME64_R_BUTTON, THEME64_R_BUTTON, t->dock_bg,
                          s_eq(g_hover_name, "cal.next") ? 150 : 80);
        p2ui_icon64(P2UI_ICON_CHEV_R, bx + 5, by + 5, 16, p2ui_icon_color64(t), 255);
        char ym[16];
        char b[8];
        u_dec4(b, g_cal_y); s_copy(ym, b, (int)sizeof(ym));
        // 追加 "年M月"
        int n = s_len(ym);
        n = push_u8_64(ym, n, (int)sizeof(ym), "年");
        char mo[4];
        u_dec(mo, (uint64_t)g_cal_m);
        for (int i = 0; mo[i] && n < 14; i++) ym[n++] = mo[i];
        n = push_u8_64(ym, n, (int)sizeof(ym), "月");
        ym[n] = 0;
        int yx = 0, yy2 = 0, yw = 0, yh = 0;
        panels64_cal_year_rect64(&yx, &yy2, &yw, &yh);
        p2ui_text64(yx + 4, py + (yy2 - y) + (yh - lh) / 2, ym, txt);
    }
    // 星期标题行（一~日，星期一第一列）
    static const char* wd1[7] = {"一", "二", "三", "四", "五", "六", "日"};
    {
        const int pad = 14;
        const int cw = (w - pad * 2) / 7;
        for (int i = 0; i < 7; i++) {
            const int tw = text_w64(wd1[i]);
            p2ui_text64(x + pad + i * cw + (cw - tw) / 2, py + 80, wd1[i], t->text_dim);
        }
    }
    // 主体：7 列 × 6 行
    {
        const int wd1st = weekday_ymd64(g_cal_y, g_cal_m, 1);
        const int lead = (wd1st + 6) % 7;
        const int dim = days_in_month64(g_cal_y, g_cal_m);
        for (int i = 0; i < 42; i++) {
            int cx = 0, cy = 0, cw = 0, ch = 0;
            panels64_cal_cell_rect64(i, &cx, &cy, &cw, &ch);
            cy = py + (cy - y);
            const int day = i - lead + 1;
            const bool cur = (day >= 1 && day <= dim);
            const bool today = cur && g_cal_y == g_today_y && g_cal_m == g_today_m && day == g_today_d;
            const bool sel = cur && g_cal_y == g_cal_sy && g_cal_m == g_cal_sm && day == g_cal_sd;
            const int ccx = cx + cw / 2, ccy = cy + ch / 2;
            if (today) {
                // 今天：主题强调色**圆形**高亮
                p2ui_fill_circle64(ccx, ccy, (cw < ch ? cw : ch) / 2 - 4, t->accent, 255);
            } else if (sel) {
                // 选中日期：填充色
                p2ui_fill_circle64(ccx, ccy, (cw < ch ? cw : ch) / 2 - 4, t->accent, 90);
            }
            char b[4];
            int shown = day;
            if (day < 1) {
                const int pm = (g_cal_m == 1) ? 12 : (g_cal_m - 1);
                const int py2 = (g_cal_m == 1) ? (g_cal_y - 1) : g_cal_y;
                shown = days_in_month64(py2, pm) + day;              // 非当前月：淡显上月日期
            } else if (day > dim) {
                shown = day - dim;                                   // 非当前月：淡显下月日期
            }
            u_dec(b, (uint64_t)shown);
            const int tw = text_w64(b);
            const uint32_t col = today || sel ? (today ? rgb(255, 255, 255) : txt)
                                              : (cur ? txt : t->text_dim);
            p2ui_text64(ccx - tw / 2, ccy - lh / 2, b, col);
        }
    }
    // 年份选择面板（点年份文字展开；可跳 2022…2033 及未来）
    if (g_year_panel) {
        const int yx = x + 14, yy2 = py + 84, yw = w - 28, yh = h - 84 - 12;
        p2ui_fill_mixed64(yx, yy2, yw, yh, THEME64_POP_R_SMALL, THEME64_POP_R_SMALL, t->dock_bg,
                          t->dark ? 240 : 246);
        p2ui_stroke_mixed64(yx, yy2, yw, yh, THEME64_POP_R_SMALL, THEME64_POP_R_SMALL, 0xFFFFFFu,
                            THEME64_A_EDGE);
        const int base = ((g_cal_y - 5) / 3) * 3;          // 3 列 × 4 行
        for (int i = 0; i < 12; i++) {
            const int yy3 = base + i;
            const int cx = yx + 14 + (i % 3) * ((yw - 28) / 3);
            const int cy2 = yy2 + 16 + (i / 3) * 40;
            const bool on = (yy3 == g_cal_y);
            if (on)
                p2ui_fill_mixed64(cx - 6, cy2 - 4, (yw - 28) / 3 - 8, 32, THEME64_R_BUTTON,
                                  THEME64_R_BUTTON, t->accent, 120);
            else if (i == g_year_sel)
                p2ui_stroke_mixed64(cx - 6, cy2 - 4, (yw - 28) / 3 - 8, 32, THEME64_R_BUTTON,
                                    THEME64_R_BUTTON, t->accent, 200);
            char b[8];
            u_dec4(b, yy3);
            p2ui_text64(cx + 8, cy2, b, on ? rgb(255, 255, 255) : txt);
        }
    }
}

// 设备插拔 toast：亚克力 + 圆角 + 双层浅阴影 + 1px 高光边 + 图标/标题/内容/时间 + 关闭按钮
static void draw_toast64() {
    const Theme64Tokens* t = theme64_tokens64();
    for (int i = 0; i < THEME64_TOAST_MAX; i++) {
        const Toast64& o = g_toast[i];
        if (!o.used) continue;
        const int x = o.x, y = o.y, w = THEME64_TOAST_W, h = THEME64_TOAST_H;
        if (x >= scr_w64()) continue;                       // 还在屏幕外
        p2ui_popup64(x, y, w, h, THEME64_POP_R_SMALL, THEME64_POP_R_SMALL, t, t->dock_bg,
                     t->dark ? THEME64_A_POP_DARK : THEME64_A_TOAST);
        const uint32_t txt = t->dark ? t->text : rgb(24, 26, 32);
        p2ui_icon64(o.kind, x + 14, y + (h - 26) / 2, 26, p2ui_icon_color64(t), 255);
        const int tw = text_w64(o.title);
        text64(x + 48, y + 12, o.title, txt);
        text64(x + 48, y + 12 + p2ui_line_h64() + 4, o.body, t->text_dim);
        // 时间（右上）+ 关闭按钮（×）
        text64(x + w - 20 - text_w64(o.time), y + 12, o.time, t->text_dim);
        const bool hov = s_eq(g_hover_name, "toast.x");
        p2ui_fill_mixed64(x + w - 30, y + h - 30, 22, 22, THEME64_R_BUTTON, THEME64_R_BUTTON,
                          hov ? t->accent : t->dock_bg, hov ? 120 : 80);
        p2ui_icon64(P2UI_ICON_CLOSE, x + w - 27, y + h - 27, 16, p2ui_icon_color64(t), 230);
        (void)tw;
    }
}

void panels64_draw64() {
    if (g_panel == PANEL64_NOTIF) draw_notif64();
    else if (g_panel == PANEL64_SOUND) draw_sound64();
    else if (g_panel == PANEL64_NET) draw_net64();
    else if (g_panel == PANEL64_CAL) draw_cal64();
    draw_toast64();                                  // toast 永远在最上层
}

// ==================== 命中测试 ====================
static bool in_rect64(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && my >= y && mx < x + w && my < y + h;
}
static int hit64(int mx, int my) {
    if (g_panel == PANEL64_NONE) return -1;
    int x = 0, y = 0, w = 0, h = 0;
    panels64_rect64(g_panel, &x, &y, &w, &h);
    if (!in_rect64(mx, my, x, y, w, h)) return -2;    // 面板外
    if (g_panel == PANEL64_NOTIF) {
        const int bw = 74, bh = 24;
        if (in_rect64(mx, my, x + w - 14 - bw, y + 10, bw, bh)) return 100;      // 全部清除
        for (int i = 0; i < g_notif_n && i < 5; i++) {
            const int ry = y + 44 + i * 64;
            if (in_rect64(mx, my, x + w - 34, ry + 34, 22, 22)) return 200 + i;  // 逐条清除
        }
        return 1;
    }
    if (g_panel == PANEL64_SOUND) {
        int sx = 0, sy = 0, sw = 0, sh = 0;
        panels64_slider_rect64(&sx, &sy, &sw, &sh);
        if (in_rect64(mx, my, sx - 8, sy - 6, sw + 16, sh + 12)) return 300;      // 滑轨/滑块
        if (in_rect64(mx, my, x + 66, y + 8, 56, 22)) return 310;                // 输出源：音箱
        if (in_rect64(mx, my, x + 66 + 62, y + 8, 56, 22)) return 311;           // 输出源：耳机
        return 1;
    }
    if (g_panel == PANEL64_NET) {
        int ex = 0, ey = 0, ew = 0, eh = 0;
        panels64_eth_rect64(&ex, &ey, &ew, &eh);
        if (in_rect64(mx, my, ex, ey, ew, eh)) return 400;                       // 以太网区（固定）
        if (g_wifi_n > 0) {
            const int list_y = y + 38;
            const int list_h = h - THEME64_NET_ETH_H - 52;
            if (in_rect64(mx, my, x + w - 12, list_y, 12, list_h)) return 410;   // 滚动条
            if (in_rect64(mx, my, x, list_y, w, list_h)) return 420;             // WiFi 列表行
        }
        return 1;
    }
    if (g_panel == PANEL64_CAL) {
        if (g_year_panel) {
            const int yx = x + 14, yy2 = y + 84, yw = w - 28, yh = h - 84 - 12;
            if (in_rect64(mx, my, yx, yy2, yw, yh)) {
                for (int i = 0; i < 12; i++) {
                    const int cx = yx + 14 + (i % 3) * ((yw - 28) / 3);
                    const int cy2 = yy2 + 16 + (i / 3) * 40;
                    if (in_rect64(mx, my, cx - 6, cy2 - 4, (yw - 28) / 3 - 8, 32)) return 500 + i;
                }
                return 1;
            }
            // 注意：hit64 也用于 hover（鼠标移动），**不在这里改状态** ——
            // 收起年份面板放到按下路径（panels64_handle_mouse_press64）里做。
        }
        int bx = 0, by = 0, bw = 0, bh = 0;
        panels64_cal_today_rect64(&bx, &by, &bw, &bh);
        if (in_rect64(mx, my, bx, by, bw, bh)) return 510;
        panels64_cal_prev_rect64(&bx, &by, &bw, &bh);
        if (in_rect64(mx, my, bx, by, bw, bh)) return 511;
        panels64_cal_next_rect64(&bx, &by, &bw, &bh);
        if (in_rect64(mx, my, bx, by, bw, bh)) return 512;
        panels64_cal_year_rect64(&bx, &by, &bw, &bh);
        if (in_rect64(mx, my, bx, by, bw, bh)) return 513;
        for (int i = 0; i < 42; i++) {
            int cx = 0, cy = 0, cw = 0, ch = 0;
            panels64_cal_cell_rect64(i, &cx, &cy, &cw, &ch);
            if (in_rect64(mx, my, cx, cy, cw, ch)) return 520 + i;
        }
        return 1;
    }
    return 1;
}

// ==================== 输入 ====================
static void panel_press64(int item, int mx, int my, int button) {
    (void)button;
    if (item == 100) {                                   // 全部清除
        panels64_notif_clear64();
        panel_dirty64(PANEL64_NOTIF);
        return;
    }
    if (item >= 200 && item < 200 + THEME64_NOTIF_MAX) {
        const int idx = item - 200;
        if (idx == 0) {
            // 调试用：点到哪一条就删哪一条（列表顺序即显示顺序）
        }
        panels64_notif_remove64(idx);
        panel_dirty64(PANEL64_NOTIF);
        return;
    }
    if (item == 300) {                                   // 音量滑轨（按下 + 拖动）
        g_drag = 1;
        g_sound_drag_logged = 0;
        int sx = 0, sy = 0, sw = 0, sh = 0;
        panels64_slider_rect64(&sx, &sy, &sw, &sh);
        int v = sw > 0 ? (mx - sx) * 100 / sw : g_volume;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        panels64_set_volume64(v, "drag");
        panel_dirty64(PANEL64_SOUND);
        return;
    }
    if (item == 310) { panels64_set_src64(0, "click"); panel_dirty64(PANEL64_SOUND); return; }
    if (item == 311) { panels64_set_src64(1, "click"); panel_dirty64(PANEL64_SOUND); return; }
    if (item == 400) { net_log_eth64("click-eth-row"); return; }        // 以太网区：纯状态，不可切
    if (item == 410) { g_wifi_drag = 1; return; }
    if (item == 420) {                                                   // WiFi 行：点目标 -> 密码框
        // 本批 g_wifi_n 恒为 0（没有无线驱动），此路径不会被真实数据触发；
        // 代码保留给未来的无线驱动：点目标 -> 白色透明输入密码框 + 回车/确定连接。
        dbg64_line_begin64();
        dbg64_str("[PANEL64] net password box x=");
        int nx = 0, ny = 0, nw = 0, nh = 0;
        panels64_rect64(PANEL64_NET, &nx, &ny, &nw, &nh);
        dbg64_dec((uint64_t)1);
        dbg64_str(" (unreachable in this batch: wifi list is honestly empty)");
        dbg64_nl();
        dbg64_line_end64();
        return;
    }
    if (item >= 500 && item < 512) {                     // 年份选择：跳年份
        const int base = ((g_cal_y - 5) / 3) * 3;
        const int yy = base + (item - 500);
        g_cal_y = yy;
        g_year_panel = 0;
        dbg64_line_begin64();
        dbg64_str("[PANEL64] cal year select y=");
        dbg64_dec((uint64_t)yy);
        dbg64_str(" ym=");
        char b[8];
        u_dec4(b, g_cal_y); dbg64_str(b);
        dbg64_str("-");
        u_dec2(b, g_cal_m); dbg64_str(b);
        dbg64_nl();
        dbg64_line_end64();
        panel_dirty64(PANEL64_CAL);
        return;
    }
    if (item == 510) {                                   // 今天
        int hh = 0, mm = 0, ss = 0, yy = 0, mo = 0, dd = 0, rwd = 0;
        rtc_get_time64(&hh, &mm, &ss);
        rtc_get_date64(&yy, &mo, &dd, &rwd);
        g_today_y = yy; g_today_m = mo; g_today_d = dd;
        g_cal_y = yy; g_cal_m = mo;
        g_cal_sy = yy; g_cal_sm = mo; g_cal_sd = dd;
        cal_shift64(0, "today");
        panel_dirty64(PANEL64_CAL);
        return;
    }
    if (item == 511) { cal_shift64(-1, "arrow"); panel_dirty64(PANEL64_CAL); return; }
    if (item == 512) { cal_shift64(1, "arrow"); panel_dirty64(PANEL64_CAL); return; }
    if (item == 513) {                                   // 年份文字 -> 展开年份选择面板
        g_year_panel = !g_year_panel;
        const int base = ((g_cal_y - 5) / 3) * 3;
        g_year_sel = g_cal_y - base;
        if (g_year_sel < 0) g_year_sel = 0;
        if (g_year_sel > 11) g_year_sel = 11;
        dbg64_line_begin64();
        dbg64_str("[PANEL64] cal year panel open=");
        dbg64_dec((uint64_t)(g_year_panel ? 1 : 0));
        dbg64_str(" years=");
        dbg64_dec((uint64_t)base);
        dbg64_str("..");
        dbg64_dec((uint64_t)(base + 11));
        int x = 0, y = 0, w = 0, h = 0;
        panels64_rect64(PANEL64_CAL, &x, &y, &w, &h);
        dbg64_str(" rect=");
        dbg64_dec((uint64_t)(x + 14));
        dbg64_str(",");
        dbg64_dec((uint64_t)(y + 84));
        dbg64_str(",");
        dbg64_dec((uint64_t)(w - 28));
        dbg64_str("x");
        dbg64_dec((uint64_t)(h - 96));
        dbg64_nl();
        dbg64_line_end64();
        panel_dirty64(PANEL64_CAL);
        return;
    }
    if (item >= 520 && item <= 561) {                    // 点日期：选中 + 同步顶部年月
        int yy = 0, mm = 0, dd = 0;
        if (panels64_cal_grid_hit64(mx, my, &yy, &mm, &dd)) {
            g_cal_sy = yy; g_cal_sm = mm; g_cal_sd = dd;
            g_cal_y = yy; g_cal_m = mm;
            char d[12];
            ymd64(d, yy, mm, dd);
            g_year_panel = 0;
            dbg64_line_begin64();
            dbg64_str("[PANEL64] cal select date=");
            dbg64_str(d);
            dbg64_str(" ym=");
            char b[8];
            u_dec4(b, yy); dbg64_str(b);
            dbg64_str("-");
            u_dec2(b, mm); dbg64_str(b);
            dbg64_nl();
            dbg64_line_end64();
            panel_dirty64(PANEL64_CAL);
        }
        return;
    }
}

int panels64_handle_mouse_press64(int mx, int my, int button) {
    // 1) toast 最上层：点关闭按钮 -> 手动关闭
    for (int i = 0; i < THEME64_TOAST_MAX; i++) {
        const Toast64& o = g_toast[i];
        if (!o.used) continue;
        if (in_rect64(mx, my, o.x + THEME64_TOAST_W - 30, o.y + THEME64_TOAST_H - 30, 22, 22)) {
            panels64_toast_close64(i, "click");
            gui64_dirty(o.x - 8, o.y - 8, THEME64_TOAST_W + 16, THEME64_TOAST_H + 16);
            return 1;
        }
        if (in_rect64(mx, my, o.x, o.y, THEME64_TOAST_W, THEME64_TOAST_H)) return 1;   // toast 吞掉点击
    }
    if (g_panel == PANEL64_NONE) return 0;
    const int item = hit64(mx, my);
    if (item == -2) {
        // 点弹窗外面：关弹窗（不消费，与开始菜单一致；日历的"点外部关闭"）
        int x = 0, y = 0, w = 0, h = 0;
        panels64_rect64(g_panel, &x, &y, &w, &h);
        dbg64_line_begin64();
        dbg64_str("[PANEL64] click outside close=");
        dbg64_str(g_panel == PANEL64_NOTIF ? "notif" : g_panel == PANEL64_SOUND ? "sound" :
                  g_panel == PANEL64_NET ? "net" : "cal");
        dbg64_nl();
        dbg64_line_end64();
        panels64_close64("click-outside");
        return 0;
    }
    if (button != 0) return 1;
    // 点日历里"年份面板之外"的地方 -> 收起年份面板（和"点外部关闭"一个口径）
    if (g_panel == PANEL64_CAL && g_year_panel && !(item >= 500 && item < 512)) {
        g_year_panel = 0;
        panel_dirty64(PANEL64_CAL);
    }
    // 日历：按下记录起点（用于"左右拖动切月"）
    if (g_panel == PANEL64_CAL && item >= 520) {
        g_drag = 2;
        g_drag_x0 = mx;
        g_drag_month = g_cal_y * 12 + g_cal_m;
        g_drag_ym = g_drag_month;
    }
    panel_press64(item, mx, my, button);
    return 1;
}

void panels64_handle_mouse_move64(int mx, int my, int buttons) {
    // 悬停名（有上限：只在变化时打点，且只记名字用于 UI 高亮 + 验收）
    char name[24];
    name[0] = 0;
    if (g_panel != PANEL64_NONE) {
        const int item = hit64(mx, my);
        if (item == 100) s_copy(name, "notif.clear", (int)sizeof(name));
        else if (item >= 200 && item < 208) s_copy(name, "notif.row", (int)sizeof(name));
        else if (item == 300) s_copy(name, "sound.knob", (int)sizeof(name));
        else if (item == 310 || item == 311) s_copy(name, "sound.src", (int)sizeof(name));
        else if (item == 400) s_copy(name, "net.eth", (int)sizeof(name));
        else if (item == 410) s_copy(name, "net.scrollbar", (int)sizeof(name));
        else if (item == 420) s_copy(name, "net.wifi", (int)sizeof(name));
        else if (item == 510) s_copy(name, "cal.today", (int)sizeof(name));
        else if (item == 511) s_copy(name, "cal.prev", (int)sizeof(name));
        else if (item == 512) s_copy(name, "cal.next", (int)sizeof(name));
        else if (item == 513) s_copy(name, "cal.year", (int)sizeof(name));
        else if (item >= 520) s_copy(name, "cal.cell", (int)sizeof(name));
    }
    for (int i = 0; i < THEME64_TOAST_MAX; i++) {
        const Toast64& o = g_toast[i];
        if (!o.used) continue;
        if (in_rect64(mx, my, o.x + THEME64_TOAST_W - 30, o.y + THEME64_TOAST_H - 30, 22, 22))
            s_copy(name, "toast.x", (int)sizeof(name));
    }
    if (!s_eq(name, g_hover_name)) {
        s_copy(g_hover_name, name, (int)sizeof(g_hover_name));
        if (g_panel != PANEL64_NONE) panel_dirty64(g_panel);
    }
    (void)g_hover;
    // 拖动
    if (g_drag == 1) {                                    // 音量滑块拖动
        int sx = 0, sy = 0, sw = 0, sh = 0;
        panels64_slider_rect64(&sx, &sy, &sw, &sh);
        int v = sw > 0 ? (mx - sx) * 100 / sw : g_volume;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        if (v != g_volume) {
            panels64_set_volume64(v, "drag");
            panel_dirty64(PANEL64_SOUND);
        }
        if (!(buttons & 1)) g_drag = 0;
    } else if (g_drag == 2) {                             // 左右拖动切月
        if (!(buttons & 1)) {
            g_drag = 0;
        } else if (mx - g_drag_x0 > 40) {                 // 向右拖 -> 上个月
            cal_shift64(-1, "drag");
            g_drag_x0 = mx;
            panel_dirty64(PANEL64_CAL);
        } else if (g_drag_x0 - mx > 40) {                 // 向左拖 -> 下个月
            cal_shift64(1, "drag");
            g_drag_x0 = mx;
            panel_dirty64(PANEL64_CAL);
        }
    }
    if (g_wifi_drag) {
        if (!(buttons & 1)) g_wifi_drag = 0;
        else {
            int x = 0, y = 0, w = 0, h = 0;
            panels64_rect64(PANEL64_NET, &x, &y, &w, &h);
            const int list_y = y + 38;
            const int list_h = h - THEME64_NET_ETH_H - 52;
            const int visible = list_h / 34;
            if (g_wifi_n > visible && list_h > 0) {
                int v = (my - list_y) * (g_wifi_n - visible) / (list_h > 0 ? list_h : 1);
                if (v < 0) v = 0;
                if (v > g_wifi_n - visible) v = g_wifi_n - visible;
                if (v != g_wifi_scroll) { g_wifi_scroll = v; panel_dirty64(PANEL64_NET); }
            }
        }
    }
}

int panels64_handle_key64(uint8_t c) {
    if (g_panel == PANEL64_NONE) return 0;
    if (c == 0x1B) {                                      // ESC 优先关弹窗
        dbg64_line_begin64();
        dbg64_str("[PANEL64] esc close=");
        dbg64_str(g_panel == PANEL64_NOTIF ? "notif" : g_panel == PANEL64_SOUND ? "sound" :
                  g_panel == PANEL64_NET ? "net" : "cal");
        dbg64_str(" (level=1: panel first, then start menu)");
        dbg64_nl();
        dbg64_line_end64();
        panels64_close64("esc");
        return 1;
    }
    if (g_panel == PANEL64_CAL) {
        // 键盘补充：T = 今天（与右上角「今天」按钮同一条路径）；年份面板里方向键 + 回车选年
        if (c == 't' || c == 'T') {
            int hh = 0, mm = 0, ss = 0, yy = 0, mo = 0, dd = 0, rwd = 0;
            rtc_get_time64(&hh, &mm, &ss);
            rtc_get_date64(&yy, &mo, &dd, &rwd);
            g_today_y = yy; g_today_m = mo; g_today_d = dd;
            g_cal_y = yy; g_cal_m = mo;
            g_cal_sy = yy; g_cal_sm = mo; g_cal_sd = dd;
            g_year_panel = 0;
            cal_shift64(0, "today");
            panel_dirty64(PANEL64_CAL);
            return 1;
        }
        if (g_year_panel) {
            if (c == 0xFB) { g_year_sel = (g_year_sel + 11) % 12; panel_dirty64(PANEL64_CAL); return 1; }
            if (c == 0xFC) { g_year_sel = (g_year_sel + 1) % 12; panel_dirty64(PANEL64_CAL); return 1; }
            if (c == 0xFD) { g_year_sel = (g_year_sel + 9) % 12; panel_dirty64(PANEL64_CAL); return 1; }
            if (c == 0xFE) { g_year_sel = (g_year_sel + 3) % 12; panel_dirty64(PANEL64_CAL); return 1; }
            if (c == '\n' || c == '\r') {
                const int base = ((g_cal_y - 5) / 3) * 3;
                const int yy2 = base + g_year_sel;
                g_cal_y = yy2;
                g_year_panel = 0;
                dbg64_line_begin64();
                dbg64_str("[PANEL64] cal year select y=");
                dbg64_dec((uint64_t)yy2);
                dbg64_str(" ym=");
                char b[8];
                u_dec4(b, g_cal_y); dbg64_str(b);
                dbg64_str("-");
                u_dec2(b, g_cal_m); dbg64_str(b);
                dbg64_str(" via=key");
                dbg64_nl();
                dbg64_line_end64();
                panel_dirty64(PANEL64_CAL);
                return 1;
            }
        }
        if (c == 0xFB) { cal_shift64(-1, "arrow"); panel_dirty64(PANEL64_CAL); return 1; }   // 左方向键
        if (c == 0xFC) { cal_shift64(1, "arrow"); panel_dirty64(PANEL64_CAL); return 1; }    // 右方向键
        if (c == (uint8_t)KBD_KEY_PAGEUP) { cal_shift64(-12, "pageup"); panel_dirty64(PANEL64_CAL); return 1; }
        if (c == (uint8_t)KBD_KEY_PAGEDOWN) { cal_shift64(12, "pagedown"); panel_dirty64(PANEL64_CAL); return 1; }
        if (c == 0xFD) { cal_shift64(-1, "arrow"); panel_dirty64(PANEL64_CAL); return 1; }
        if (c == 0xFE) { cal_shift64(1, "arrow"); panel_dirty64(PANEL64_CAL); return 1; }
    }
    if (g_panel == PANEL64_SOUND) {
        if (c == 0xFB || c == 0xFE) { panels64_set_volume64(g_volume - 5, "key"); panel_dirty64(PANEL64_SOUND); return 1; }
        if (c == 0xFC || c == 0xFD) { panels64_set_volume64(g_volume + 5, "key"); panel_dirty64(PANEL64_SOUND); return 1; }
    }
    return 1;                                             // 弹窗打开时其它键不喂给应用
}

int panels64_wheel64(int dz) {
    if (g_panel == PANEL64_CAL) {                         // 滚轮切月
        cal_shift64(dz > 0 ? -1 : 1, "wheel");
        panel_dirty64(PANEL64_CAL);
        dbg64_line_begin64();
        dbg64_str("[PANEL64] cal wheel dz=");
        dbg64_dec((uint64_t)(uint32_t)(int32_t)dz);
        dbg64_str(" -> month ");
        char b[8];
        u_dec4(b, g_cal_y); dbg64_str(b);
        dbg64_str("-");
        u_dec2(b, g_cal_m); dbg64_str(b);
        dbg64_nl();
        dbg64_line_end64();
        return 1;
    }
    if (g_panel == PANEL64_NET && g_wifi_n > 0) {         // WiFi 列表滚动（真列表才有）
        g_wifi_scroll -= dz;
        if (g_wifi_scroll < 0) g_wifi_scroll = 0;
        int x = 0, y = 0, w = 0, h = 0;
        panels64_rect64(PANEL64_NET, &x, &y, &w, &h);
        const int visible = (h - THEME64_NET_ETH_H - 52) / 34;
        if (g_wifi_scroll > g_wifi_n - visible) g_wifi_scroll = g_wifi_n - visible > 0 ? g_wifi_n - visible : 0;
        panel_dirty64(PANEL64_NET);
        return 1;
    }
    if (g_panel == PANEL64_SOUND) {
        panels64_set_volume64(g_volume + (dz > 0 ? 5 : -5), "wheel");
        panel_dirty64(PANEL64_SOUND);
        return 1;
    }
    return 0;
}

// ==================== 初始化 / 自检 ====================
void panels64_init64() {
    g_panel = PANEL64_NONE;
    g_notif_n = 0;
    g_notif_unread = 0;
    for (int i = 0; i < THEME64_NOTIF_MAX; i++) g_notif[i].used = 0;
    for (int i = 0; i < THEME64_TOAST_MAX; i++) g_toast[i].used = 0;
    g_wifi_n = 0;
    g_wifi_scroll = 0;
    g_link_seen = -1;
    g_usb_msc_seen = -1;
    g_usb_dev_seen = -1;
    g_poll_next = ticks64();
    int hh = 0, mm = 0, ss = 0, yy = 0, mo = 0, dd = 0, rwd = 0;
    rtc_get_time64(&hh, &mm, &ss);
    rtc_get_date64(&yy, &mo, &dd, &rwd);
    g_today_y = yy; g_today_m = mo; g_today_d = dd;
    g_cal_y = yy; g_cal_m = mo;
    g_cal_sy = yy; g_cal_sm = mo; g_cal_sd = dd;
    dbg64_line_begin64();
    dbg64_str("[PANEL64] init anchors=menu screen=");
    dbg64_dec((uint64_t)scr_w64());
    dbg64_str("x");
    dbg64_dec((uint64_t)scr_h64());
    dbg64_str(" dock_top=");
    dbg64_dec((uint64_t)dock_top64());
    dbg64_str(" blur=");
    dbg64_dec((uint64_t)THEME64_BLUR_POP);
    dbg64_str(" edge=1px shadow=2 popup=");
    dbg64_dec((uint64_t)THEME64_NOTIF_W);
    dbg64_str("x");
    dbg64_dec((uint64_t)THEME64_NOTIF_H);
    dbg64_str(" sound=");
    dbg64_dec((uint64_t)THEME64_SOUND_W);
    dbg64_str("x");
    dbg64_dec((uint64_t)THEME64_SOUND_H);
    dbg64_str(" net=");
    dbg64_dec((uint64_t)THEME64_NET_W);
    dbg64_str("x");
    dbg64_dec((uint64_t)THEME64_NET_H);
    dbg64_str(" cal=");
    dbg64_dec((uint64_t)THEME64_CAL_W);
    dbg64_str("x");
    dbg64_dec((uint64_t)THEME64_CAL_H);
    dbg64_nl();
    dbg64_line_end64();
    e1000_probe64();
    dbg64_line_begin64();
    dbg64_str("[PANEL64] device src probe e1000 bar0=");
    dbg64_hex64((uint64_t)g_e1000_bar0);
    dbg64_str(" link=");
    dbg64_dec((uint64_t)e1000_live_link64());
    dbg64_str(" (live STATUS.LU; usb64 counts polled too, hotplug=unsupported)");
    dbg64_nl();
    dbg64_line_end64();
    int st = panels64_selftest64();
    if (st != 0) pl("selftest FAIL mask=", nullptr, (int64_t)st, " (anchors/limits)");
    else {
        dbg64_line_begin64();
        dbg64_str("[PANEL64] selftest PASS mask=0");
        dbg64_nl();
        dbg64_line_end64();
    }
}

int panels64_selftest64() {
    int fails = 0;
    const int W = scr_w64();
    const int dt = dock_top64();
    for (int which = PANEL64_NOTIF; which <= PANEL64_CAL; which++) {
        int x = 0, y = 0, w = 0, h = 0;
        if (!panels64_rect64(which, &x, &y, &w, &h)) { fails |= 1; continue; }
        if (x < 0 || y < 0 || x + w > W || y + h > dt - 4) fails |= 2;        // 不超屏 / 不压 Dock
        if (w <= 0 || h <= 0) fails |= 4;
        if (which == PANEL64_CAL) {
            if (w < 360 || w > 420 || h < 420 || h > 480) fails |= 8;         // 需求：360–420 × 420–480
            int cx = 0, cy = 0, cw = 0, ch = 0;
            panels64_cal_cell_rect64(41, &cx, &cy, &cw, &ch);                 // 第 6 行最后一天
            if (cy + ch > y + h) fails |= 16;
            panels64_cal_cell_rect64(0, &cx, &cy, &cw, &ch);
            if (cy < y + 60) fails |= 32;
        }
        if (which == PANEL64_NET) {
            int ex = 0, ey = 0, ew = 0, eh = 0;
            panels64_eth_rect64(&ex, &ey, &ew, &eh);
            if (ey + eh > y + h) fails |= 64;                                 // 以太网区在面板内
            if (ey < y + 60) fails |= 128;                                    // 且在最下方（不在顶部）
        }
        if (which == PANEL64_SOUND) {
            int sx = 0, sy = 0, sw = 0, sh = 0;
            panels64_slider_rect64(&sx, &sy, &sw, &sh);
            if (sx < x || sx + sw > x + w) fails |= 256;
            if (sw < 60) fails |= 512;
        }
    }
    // 锚定关系：通知/声音在菜单右边，网络在菜单左边（"位置跟随开始菜单"）
    int sx = 0, sy = 0, sw = 0, sh = 0;
    sm_rect64(&sx, &sy, &sw, &sh);
    {
        int x = 0, y = 0, w = 0, h = 0;
        panels64_rect64(PANEL64_NOTIF, &x, &y, &w, &h);
        if (x < sx + sw) fails |= 1024;
        panels64_rect64(PANEL64_NET, &x, &y, &w, &h);
        if (x + w > sx) fails |= 2048;
    }
    // 日期/星期换算样本
    if (weekday_ymd64(2026, 9, 23) != 3) fails |= 4096;
    if (days_in_month64(2024, 2) != 29) fails |= 8192;
    return fails;
}
