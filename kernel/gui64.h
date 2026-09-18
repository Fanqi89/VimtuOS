// gui64.h - 64 位桌面外壳与应用的公共契约（移植自 32 位 gui.h，接口 64 位化）
//
// 设计原则（与 32 位 gui.cpp 的关系）：
//   * 行为/功能以 32 位为准（完整参考实现见 Vimtu32/kernel/gui.cpp，4214 行），
//     但接口按 64 位重排：应用拆成独立 .cpp，不再全部塞进 gui.cpp。
//   * 32 位那套"按槽位下标查会话表"的缺陷（APP_ID_TERM==3 会命中毒 TaskMgr 槽）
//     在这里修掉：一切以 app_id 为键。
//   * 窗口内容绘制约定：应用在 draw 回调里用**屏幕绝对坐标**绘制，外壳负责把
//     绘制裁剪到客户区（fb_set_clip）并做脏矩形提交（fb_flip_region）。
//     这与 kernel/fb.h 的裁剪接口、以及安装界面的脏矩形做法一致。
//
// 绘制可用面（都由外壳/驱动提供，应用直接用）：
//   fb_*     ：像素级原语 + 文字（8x8 位图字体，带 fg/bg/scale）
//   font_*   ：TrueType（UTF-8，**只有前景色、没有背景色、没有缩放**）
//              -> 需要底色时自己先 fb_fill_rect 清一块再 font_draw_text
//   rgb(r,g,b) 造色
// 输入：
//   kbd_pop_char()/kbd_has_char()/kbd_win_pressed()+kbd_consume_win()/kbd_tm_hotkey()
//   mouse_get_x()/mouse_get_y()/mouse_get_buttons()/mouse_button_pressed()/mouse_consume_pressed()
// 时间： ticks64()（250Hz）、ms_to_ticks64(ms)、rtc_get_time64/rtc_get_date64
// 内存： kmalloc_64/kfree_64/krealloc_64 + mem_owner_set_64(MEM_OWNER_*_64)
// 串口： dbg64_str/dbg64_dec/dbg64_hex64/dbg64_nl（自动验收靠它读日志，务必打关键行）
#pragma once
#include <stdint.h>

struct BootInfo;
struct Window;

// ==================== 应用 ID ====================
// 与 32 位 APP_ID_* 对齐（1..5），并补齐 32 位里没有独立 ID 的三个内置窗口。
#define APP_ID_NONE      0
#define APP_ID_CALC      1
#define APP_ID_MINES     2
#define APP_ID_TERM      3
#define APP_ID_TMGR      4
#define APP_ID_SETTINGS  5
#define APP_ID_MONITOR   6      // 系统监视器
#define APP_ID_MYPC      7      // 我的电脑
#define APP_ID_ABOUT     8      // 关于 VimtuOS
#define APP_ID_RECYCLE   9      // 回收站

// ==================== 窗口回调 ====================
typedef void (*WindowDrawFn)(Window*);
typedef void (*WindowKeyFn)(Window*, char c);
typedef void (*WindowClickFn)(Window*, int cx, int cy);              // 客户区坐标
typedef void (*WindowClick2Fn)(Window*, int cx, int cy, int button); // 0=左 1=右 2=中
typedef void (*WindowTickFn)(Window*);                               // 每个外壳 tick（约 60Hz）
typedef void (*WindowCloseFn)(Window*);                              // 窗口即将销毁（释放 userdata 用）

// ==================== 窗口 ====================
// 注意：应用**只读**几何字段（x/y/w/h/client_*），改动一律走 gui64_* 接口，
// 这样外壳才能正确维护脏矩形与 z 序。
struct Window {
    int  x, y, w, h;            // 窗口整体几何（含标题栏与 1px 边框）
    int  client_x, client_y;    // 客户区左上角（屏幕坐标）
    int  client_w, client_h;    // 客户区尺寸
    int  min_w, min_h;          // 最小尺寸（拖拽缩放时钳制）
    char title[32];             // UTF-8 标题
    bool visible;
    bool active;
    bool minimized;
    bool maximized;
    int  norm_x, norm_y, norm_w, norm_h;   // 最大化前的几何

    WindowDrawFn   draw;        // 画客户区内容（外壳已设好裁剪）
    WindowKeyFn    on_key;      // 键盘字符（可空）
    WindowClickFn  on_click;    // 左键（可空）
    WindowClick2Fn on_click2;   // 任意键（可空；扫雷右键标旗用这个）
    WindowTickFn   on_tick;     // 定时回调（可空；计时器用这个）
    WindowCloseFn  on_close;    // 外壳销毁窗口前调用（可空）；在此释放 userdata
                                // 注：带该字段前，应用靠 gui64_window_alive() 轮询发现关窗，
                                // 两种方式并存都安全（外壳不会替你释放 userdata）

    void*   userdata;           // 应用私有状态（关窗时外壳不会替你释放，自己 on_close 里放）
    uint8_t app_id;             // APP_ID_*
    uint8_t mem_owner;          // MEM_OWNER_*_64（内存归属统计用）
    uint64_t cpu_cycles;        // 外壳用 TSC 统计的该窗口回调耗时（任务管理器的 CPU% 列）
    uint8_t  cpu_pct;           // 最近一个采样周期的 CPU 占比（%）

    Window* next;               // z 序链表（外壳内部，应用勿动）
    // 外壳内部字段
    uint32_t anim_t0, anim_dur;
    uint8_t  anim_kind;         // 0=无 1=打开 2=最大化/还原
    uint8_t  closing;           // 已请求关闭
    uint8_t  resizing;
};

// ==================== 外壳 API（应用可用）====================
// --- 窗口生命周期 ---
Window* gui64_create_window(const char* title, int x, int y, int w, int h,
                            WindowDrawFn draw, WindowKeyFn on_key,
                            WindowClickFn on_click, int app_id);
void    gui64_destroy_window(Window* w);
void    gui64_set_active(Window* w);
void    gui64_set_title(Window* w, const char* utf8);
void    gui64_set_min_size(Window* w, int min_w, int min_h);
void    gui64_set_click2(Window* w, WindowClick2Fn fn);     // 补挂右键回调
void    gui64_set_tick(Window* w, WindowTickFn fn);         // 补挂定时回调
void    gui64_set_geom(Window* w, int x, int y, int cw, int ch);  // 按客户区尺寸重排窗口
int     gui64_fit_window_to_client(Window* w, int cw, int ch);    // 只在不小于最小尺寸时改

// --- 重绘 ---
void    gui64_invalidate();                  // 整屏重绘
void    gui64_invalidate_window(Window* w);  // 只重绘某个窗口
void    gui64_flip_window(Window* w);        // 立即把该窗口区域提交上屏
void    gui64_dirty(int x, int y, int w, int h);

// --- 查询 ---
int     gui64_screen_w();
int     gui64_screen_h();
int     gui64_taskbar_h();
bool    gui64_start_menu_open();
Window* gui64_top_window();
Window* gui64_window_at(int i);              // 遍历所有窗口（0..gui64_window_count()-1）
int     gui64_window_count();
int     gui64_app_windows(int app_id);
bool    gui64_click_is_right();
bool    gui64_window_alive(Window* w);       // 指针是否仍是活动窗口（防重复打开时用悬空指针）
void    gui64_close_all_windows();
int     gui64_close_app(int app_id);         // 关掉某应用的全部窗口（任务管理器"结束任务"用）

// --- 语言（中/英切换；外壳提供，应用用它选标题与文案）---
bool        gui64_lang_zh();
const char* gui64_tr(const char* en, const char* zh);
void        gui64_set_lang_zh(bool zh);      // 设置页切换中/英

uint32_t    gui64_fps();                     // 外壳实测帧率（性能页/系统监视器用）
uint8_t     gui64_cpu_busy_pct();            // 外壳忙占比（性能页用）
// --- 重启 / 关机（外壳实现，终端/开始菜单都调）---
void    sys_reboot64();
void    sys_shutdown64();

// ==================== 各应用入口（一个应用一个 .cpp）====================
// 约定：xxx_open64() 创建（或激活已有的）窗口；xxx_reset64() 清空会话状态。
// 打开时必须在串口打一行 "[APP] xxx opened ..."，关闭打 "[APP] xxx closed"，
// 自动验收靠这些行判定。（文案见各应用文件顶部注释）
void app_calc_open64();
void app_calc_reset64();

void app_mines_open64();
void app_mines_reset64();

void app_term_open64();
void app_term_reset64();

void app_tmgr_open64();
void app_tmgr_reset64();

void app_settings_open64();
void app_settings_reset64();

// 这三个是轻量内置窗口（"我的电脑 / 系统监视器 / 关于"），32 位里没有独立文件；
// 由 shell 提供实现（见 kernel/gui64.cpp），此处只声明。
void app_monitor_open64();
void app_monitor_reset64();
void app_mypc_open64();
void app_about_open64();
void app_recycle_open64();

// ==================== 桌面外壳主入口 ====================
// 由 kernel64.cpp 的已安装系统启动路径调用（不返回）。内部会：
//   初始化外壳状态 -> 注册内置窗口 -> 跑消息循环（输入分发 + 脏矩形重绘）。
[[noreturn]] void gui64_run(const BootInfo* bi);

// 外壳自检（启动期调用，返回 0 = 通过；打印 "[GUI64] selftest PASS"）
int gui64_selftest();
