// config64.h - VimtuOS 64 位系统配置（KV + 类型化取值 + store64 真落盘）
//
// 与 32 位 legacy32/kernel/config.cpp 的关系（移植**语义**，不是照抄代码）：
//   32 位：CFG_MAX_ENTRIES 条 {key,int32 value} 静态表 + 文本序列化到 ramfs 的 /sys/config.cfg；
//   64 位：默认表（编译进内核）+ "被改过的键"覆盖表（落在 store64 的持久化 KV 里，
//          键名 = "cfg.<key>"，载体是 VimtuFS2 的 /store.a、/store.b —— 真落盘，见 store64.h）。
//
// ★ 为什么"默认值不进 store"（这是与 32 位的重要差别，也是刻意的）：
//   32 位把整张表序列化进文件，所以第一次启动就会写一份"默认值"文件。64 位反过来：
//   默认值只存在于下面的编译期表里，**只有被显式改过的键**才写进 store64。
//   这样 store 里永远只有"用户真正设过的项"，且 store64 的 keys 计数（现有验收断言
//   [STORE64] init ... keys=N 直接依赖它）不会被一堆默认值污染。
//
// 类型化取值：int / string / bool（32 位只有 int32；string 与 bool 是本轮补的），
//   都有"带默认值的取值"语义：找不到键时用调用方给的默认值（默认表里也没有时）。
//
// 串口打点（自动验收 grep，格式勿改；行锁保证不被抢占的行插进去）：
//   [CONF64] init carrier=vfs slot=A keys=0             初始化（store64 载体 + 从 store 读到的键数）
//   [CONF64] load <key>=<value>                          从 store64 载入的每个键（一行一个，便于断言）
//   [CONF64] default <key>=<value> type=int              默认表里有、store 里没有的键（前若干条）
//   [CONF64] set key=<k> value=<v> type=<t>              改一个键（内存 + store64，未落盘）
//   [CONF64] flush ok via=vfs slot=B gen=N keys=N        落盘（走 store64_flush64）
//   [CONF64] reset defaults=K                            恢复出厂设置
//   [CONF64] autosave ok keys=N delayed=3s               改动后的延迟落盘
#pragma once
#include <stdint.h>

// ---- 尺寸（与 store64 的 key/value 上限相容：key <= 31B、value <= 255B）----
#define CFG64_KEY_MAX      32     // 键名缓冲（含 NUL；store64 侧再前置 "cfg."，总长 <= 31B）
#define CFG64_STR_MAX      64     // 字符串值上限（含 NUL）
#define CFG64_MAX_ENTRIES  64     // 条目上限（= store64 的 STORE64_MAX_KEYS）
#define CFG64_AUTOSAVE_MS  3000   // 改动后延迟落盘（GUI 循环里推进；0 = 不自动落盘）

// ---- 类型 ----
#define CFG64_T_INT   0
#define CFG64_T_STR   1
#define CFG64_T_BOOL  2

// 初始化：读 store64 里所有 "cfg." 前缀的键 -> 覆盖表；打印 [CONF64] init / load 打点。
// ★ 必须在 store64_init64() 之后调用（否则 store 里一个键都读不到）。
void config64_init64();

// ---- 通用 KV ----
int config64_count64();                        // 可见条目数（默认表 ∪ 覆盖表）
// 第 i 条（0 起）：key/value 拷进调用方缓冲（NUL 结尾）；*type = CFG64_T_*；
// *from_store = 1 表示这条来自 store64（被改过），0 表示用编译期默认值。
// 返回 0 = 有值，-1 = 越界/参数错。
int config64_entry64(int i, char* key, int key_max, char* val, int val_max,
                     int* type, int* from_store);

int config64_get_int64(const char* key, int def);
int config64_get_bool64(const char* key, int def);            // "1/0"、"true/false"、"on/off"
int config64_get_str64(const char* key, const char* def, char* out, int out_max);

int config64_set_int64(const char* key, int v);               // 0 = 成功
int config64_set_bool64(const char* key, int v);
int config64_set_str64(const char* key, const char* v);
// 终端 `cfg set KEY VALUE` 用：按文本自动判类型（纯十进制 -> int；true/false/on/off -> bool；
// 其余 -> string），并按该类型写进配置（不存在于默认表里的键也能建，例如 demo=1）。
int config64_set_auto64(const char* key, const char* text);

int  config64_del64(const char* key);                         // 删掉一条覆盖（回到默认值）

// ---- 落盘 / 出厂 ----
int  config64_flush64();                      // -> store64_flush64()（写非活动槽 + 切换 + gen+1）
int  config64_pending64();                    // 1 = 有未落盘的改动
uint32_t config64_last_change_tick64();       // 最近一次改动时刻（tick）
int  config64_tick64();                       // GUI 循环每帧调用：延迟落盘（>= CFG64_AUTOSAVE_MS）
int  config64_factory_reset64();              // 清覆盖表 + 标记待落盘（默认表不动）
// 可见条目数（默认表 + 覆盖）与"默认表条目数"（打点/报告用）
int  config64_default_count64();

// ---- 报告 / 展示 ----
const char* config64_carrier64();             // "vfs" / "raw" / "none"（store64 载体）
const char* config64_slot64();                // 活动槽 "A" / "B" / "none"
const char* config64_type_name64(int type);   // "int" / "str" / "bool"
// 多行文本报告（终端 cfg 打印）：每条一行 "<key> = <value> [<type>] <src>"。返回写入长度。
int config64_format64(char* out, int maxlen);

// ==================== 语义封装（真正被系统使用的键）====================
// 界面语言：0 = 英文，1 = 简体中文（gui64 启动时读、切换时写）
int  cfg64_lang_zh64();
void cfg64_set_lang_zh64(int zh);
// 桌面图标位置（3 个图标 x/y；gui64 启动时读、拖动结束时写）
int  cfg64_icon_x64(int i);
int  cfg64_icon_y64(int i);
void cfg64_set_icon64(int i, int x, int y);
// 窗口几何：按应用名（"term"）存宽高（窗口尺寸；0 = 没有记录）
int  cfg64_win_w64(const char* app);
int  cfg64_win_h64(const char* app);
void cfg64_set_win64(const char* app, int w, int h);
// 鼠标灵敏度（千分比；1700 = 驱动基线 1.7x，见 gui64 的应用方式）
int  cfg64_mouse_sens64();
void cfg64_set_mouse_sens64(int permille);
// 文字镜像（1 = 外壳 TrueType 文本水平镜像；0 = 正常）
int  cfg64_text_mirror64();
void cfg64_set_text_mirror64(int on);
// 启动项（"terminal" / "monitor" / "desktop" / "health"）
int  cfg64_startup64(const char* item);
void cfg64_set_startup64(const char* item, int on);
// 显示缩放（%）：启动时应用，设置页应用时写回
int  cfg64_zoom64();
void cfg64_set_zoom64(int pct);
// 会话策略 / 会话恢复列表（由 session64 读写）
int  cfg64_session_mode64();
void cfg64_set_session_mode64(int mode);
int  cfg64_session_restore64(char* out, int out_max);
void cfg64_set_session_restore64(const char* list);
