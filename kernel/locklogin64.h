// locklogin64.h - 锁屏 + 登录界面 + 密码框 + 多用户选择（Windows 11 风格，全部走 theme64/gfx64 的 Token）
//
// 位置与顺序（**开机后第一屏**）：
//     开机滚屏（console64） -> 开机动画（gui64 的 boot_logo_fade_in） -> **锁屏** -> 登录 -> 桌面
//   gui64_run() 在 boot_logo_fade_in() 之后调用 locklogin64_run64()：这个函数**不返回**直到登录成功，
//   所以 '[GUI64] ready'（桌面出现）必定晚于 '[LOCK64] lock screen shown'。
//   安装介质内核（-DVIMTU_INSTALLER_MEDIA）不链本模块（见 build64.sh），安装向导不受影响。
//
// ==================== 需求原文里的数字（必须照做）====================
//   锁屏：居中偏上显示 时间（上）+ 年月日（下）+ 星期几；时间大号 **64–80px**（取 72，按**字形墨高**量）、
//         年月日 **16–20px**（取 18）；风格 = **白色亚克力半透明**（Token：THEME64_A_CARD + 圆角 THEME64_R_CARD）；
//         背景**保持清晰、不做模糊**（壁纸/渐变来自 gfx64 的整屏壁纸面；锁屏壁纸模式读 ui.wall.lock_mode，默认填充）
//   登录：回车/鼠标点击 -> 背景**清晰 -> 模糊 20px**，动画 **250–350ms**（取 300ms；Token 动效口径）；
//         ESC -> 回到锁屏（**同一动画反向**，平滑，不闪切）
//   登录界面：圆形头像 **96–112px**（取 104，带轻微外阴影）；登录按钮 **96×96、圆角 24、主题渐变**
//         流程：回车/点击 -> 无密码直接进桌面；有密码弹 **360×48 磨砂密码框** + 右侧 **48×48 白底半透明「确认」按钮**
//   用户：只显示**普通用户**；root 默认隐藏（userdb64 保证）
//
// ==================== 串口打点（自动验收 grep；每类有上限防刷屏）====================
//   [LOCK64] lock screen shown time=HH:MM date=YYYY-MM-DD weekday=星期N blur_bg=0
//   [LOCK64] lock text time_ink=72px time_box=x,y,w,h date_ink=18px date_box=x,y,w,h
//   [LOCK64] bg tile x=<..> y=<..> wh=32x32 var=<n> src=wall-surface blur=0 (sharp)
//   [LOCK64] wall src=<desc> lock_mode=<n> desktop_mode=<m> applied=<n|m> screen=WxH
//   [LOCK64] userlist n=<n> names=A,B root=hidden=1 sel=0
//   [LOCK64] avatar user=A src=builtin:1 (procedural)
//   [LOCK64] auto login user=A delay=8000ms (ui.login.auto=1; set 0 to require a key/click)
//   [LOCK64] lock requested by=terminal (loginctl lock)
//   [LOGIN64] login screen shown blur=20 anim=300ms user=A password=none|required
//   [LOGIN64] blur tile x=<..> y=<..> wh=32x32 r=20 var_src=<n> var_dst=<n> ms=<n> tile=1280x800
//   [LOGIN64] blur anim dir=in|out t=<ms> v=<0..20>          （动画采样；单调，可断言 250–350ms 内 20->0）
//   [LOGIN64] lock screen restored (ESC) anim=300ms blur 20->0
//   [LOGIN64] user select idx=<n> user=A avatar=builtin:1
//   [LOGIN64] login button key=enter|click
//   [LOGIN64] password prompt user=A box=360x48 btn=48x48
//   [LOGIN64] password wrong user=A attempt=<n>（**绝不打印输入内容**）
//   [LOGIN64] login ok user=A uid=1001
//   [LOGIN64] login cancel (ESC from password box)
#pragma once
#include <stdint.h>

// 屏幕状态（桌面外壳只认这一个状态位）
#define LOCKLOGIN_STATE_LOCK     0
#define LOCKLOGIN_STATE_LOGIN    1
#define LOCKLOGIN_STATE_DESKTOP  2

// 需求原文的数字（改之前先读需求）
#define LOCKLOGIN_TIME_INK     72      // 时间字形墨高（需求 64–80）
#define LOCKLOGIN_DATE_INK     18      // 年月日字形墨高（需求 16–20）
#define LOCKLOGIN_AVATAR_PX    104     // 头像直径（需求 96–112）
#define LOCKLOGIN_BTN_PX       96      // 登录按钮边长（需求 96×96）
#define LOCKLOGIN_BTN_R        24      // 登录按钮圆角（需求 24）
#define LOCKLOGIN_PW_W         360     // 密码框宽（需求 360×48）
#define LOCKLOGIN_PW_H         48      // 密码框高
#define LOCKLOGIN_PW_BTN       48      // 确认按钮 48×48
#define LOCKLOGIN_BLUR_PX      20      // 登录背景模糊半径（需求 20px）
#define LOCKLOGIN_ANIM_MS      300     // 清晰<->模糊动画时长（需求 250–350ms）

// 初始化：读 ui.login.* / ui.wall.lock_mode；把模糊用的缓冲准备好。必须在 theme64/gfx64 可用之后调用。
void locklogin64_init64();

// 开机路径：从锁屏开始跑（内部循环），登录成功后返回（state=DESKTOP）。gui64_run 调用。
void locklogin64_run64();
// 终端 `loginctl lock`：把屏幕状态切回锁屏（输入与整屏重绘交给 gui64 主循环的 hook）。
void locklogin64_lock64(const char* why);

int  locklogin64_state64();                 // LOCKLOGIN_STATE_*
const char* locklogin64_state_name64();
int  locklogin64_active64();                // 1 = 锁屏/登录层正在显示（桌面外壳不该画窗口）
// gui64 主循环的 hook：状态 != DESKTOP 时由本层接管（返回 1 = 本帧已被消费）
int  locklogin64_tick64();
// 纯逻辑自检（几何/动效区间/用户列表过滤；不建窗口、不碰盘）
int  locklogin64_selftest64();
