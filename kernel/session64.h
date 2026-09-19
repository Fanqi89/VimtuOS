// session64.h - 64 位会话 / 应用内容策略（会话策略 + 应用保留开关 + 启动恢复）
//
// 与 32 位 legacy32/kernel/session.cpp 的关系（移植语义）：
//   32 位：SESSION_VOLATILE / SESSION_PERSIST 两种策略 + 应用注册表（keep 开关存在 config 里）+
//          "关窗即清状态 / 停止阶段 reset_all" + 会话快照写 ramfs（/sys/session.cfg）。
//   64 位：同样两种策略，应用表按**应用 id**登记（gui64.h 的 APP_ID_*，外壳本身就是按 id 索引的），
//          keep 开关与策略都落在 config64（-> store64 -> VimtuFS2 的 /store.a|b，真落盘）；
//          会话快照 = config64 的 "sess.restore"（当前打开的应用 id 列表）。
//
// 语义（与 32 位一致的地方刻意保持一致）：
//   VOLATILE（默认）：软件内容不保存 —— 关窗即清该应用状态；停止/重启时清全部应用状态。
//   PERSIST：关窗与停止都不清状态；下次启动按 sess.restore 列表把应用**恢复打开**。
//
// 为什么重置动作走 tick（不在关窗回调里直接做）：
//   gui64 的关窗路径正在销毁窗口，若在里面同步调 app_xxx_reset64()（它又会 gui64_destroy_window），
//   就是递归销毁。所以 session64_app_closed64() 只登记"待清理"，由 gui64 主循环每帧调
//   session64_tick64() 在**没有窗口销毁在栈上**的时候执行真正的 reset。
//
// 串口打点（自动验收 grep，格式勿改）：
//   [SESS64] policy=VOLATILE apps=5 keep=0                         初始化
//   [SESS64] mode=PERSIST                                          策略切换
//   [SESS64] keep terminal=1                                       某个应用改为保留
//   [SESS64] reset app=calculator reason=close                     关窗清状态（真动作）
//   [SESS64] reset-all apps=3 reason=stop                          停止阶段清全部
//   [SESS64] save open=2 list=3,2 mode=VOLATILE                    会话快照（写 config64）
//   [SESS64] restore open=1 list=3 mode=PERSIST                    启动恢复
#pragma once
#include <stdint.h>

#define SESS64_APPS_MAX  9          // 登记的应用上限（gui64 的 5 个应用 + 监视器/我的电脑/关于/回收站）

// 策略
#define SESS64_VOLATILE  0
#define SESS64_PERSIST   1

// 初始化（在 config64_init64() 之后调用）：读策略 + 登记应用表。
void session64_init64();

// 策略
int         session64_mode64();                       // SESS64_VOLATILE / SESS64_PERSIST
const char* session64_mode_name64(int mode);
void        session64_set_mode64(int mode);           // 写 config64（-> store64，标记待落盘）

// 应用表（按 gui64 的 APP_ID_* 登记；id 0 = 空槽）
int         session64_app_count64();                  // 已登记应用数
const char* session64_app_name64(int app_id);
bool        session64_app_ok64(int app_id);           // 该 id 是否已登记
bool        session64_app_keep64(int app_id);         // 关窗/停止时是否保留状态
void        session64_set_app_keep64(int app_id, bool keep);   // 写 config64（"sess.keep.<id>"）

// 生命周期钩子
void        session64_app_opened64(int app_id);       // 打开时调用（登记/日志，策略在关闭与停止时执行）
void        session64_app_closed64(int app_id);       // 关窗（VOLATILE 且 keep=off -> 下一帧真清状态）
void        session64_tick64();                       // GUI 主循环每帧调用：执行待清理的 reset
int         session64_reset_all64(const char* why);   // 停止阶段：按策略清全部应用状态，返回清了几个
int         session64_reset_count64();                // 累计清理次数（报告用）
const char* session64_last_reset_name64();            // 最近一次被清的应用名（"-" = 无）

// 会话快照
int         session64_open_count64();                 // 当前打开的应用实例数（问外壳要真值）
int         session64_save64();                       // 记录当前打开的应用 -> config64 的 sess.restore
int         session64_restore64();                    // 启动时按策略恢复应用（PERSIST 才做），返回打开个数

// 报告（终端 `session` 命令用）：多行文本，返回写入长度
int         session64_report64(char* out, int maxlen);
