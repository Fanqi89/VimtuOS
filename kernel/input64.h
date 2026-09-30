// input64.h - ★ 用户态输入事件投递（自有 ABI 12：input_poll）
//
// ============================ 为什么要有这一层 ============================
// 既有输入（kernel/input.cpp）只把\"已解码的字节\"放进**内核自己的**环形队列：
// kbd_pop_char / mouse_get_x / mouse_button_pressed。桌面外壳（gui64，任务 0，ring0）读得到，
// ring3 进程一个事件都拿不到 —— Wayland 那一套（客户端 poll 事件）就没有地基。
// 本文件补的就是这块地基：
//   * 一个 **POD、固定布局** 的事件结构（内核与用户程序之间的 ABI，40 字节）；
//   * **每进程一个事件队列**（Proc64 内嵌一份，见 kernel/proc64.*）；input.cpp 在
//     \"键码/鼠标包解码完成\"的位置调用本文件的产出钩子，本文件负责**路由**（焦点/捕获）；
//   * 队列满了**丢最旧**并打点计数（绝不静默丢事件）；
//   * 系统调用侧 = 自有 ABI 12（`input_poll(out, max, flags)`），实现见 ev64_poll64()。
//
// ============================ 焦点 / 指针捕获（最简可行版，如实说明）============================
// 本内核**没有窗口管理器**（桌面外壳是内核里的 ring0 任务 0），所以这里不假装有仲裁策略，
// 规则写成三条、可打点：
//   1) 键盘事件只投给\"**焦点进程**\"。焦点进程由**它自己显式申请**（input_poll 的
//      EV64_FLAG_FOCUS 位）：最简可行的\"我前台、我要键盘\"。焦点值 0 = 没有 ring3 进程
//      持有焦点 -> 键盘事件一条都不进进程队列（内核桌面继续走老的 kbd_pop_char 路径）。
//      申请/释放/进程退出都打 `[EV64] focus pid=<n> prev=<n> reason=<...>`。
//   2) 鼠标事件（移动/按键/滚轮）先给\"**指针捕获者**\"（EV64_FLAG_CAPTURE）；没有捕获者时
//      给**焦点进程**（= 前台窗口所属进程）；两者都没有 -> 不进任何进程队列。
//   3) EV64_FLAG_RELEASE 一次释放焦点与捕获（进程退出/销毁时由 proc64 代它释放）。
//   ★ 如实边界：没有\"点击窗口自动换焦点\"、没有 z 序/前台窗口栈、没有多窗口分摊 ——
//     这些要有窗口管理器才谈得上，属于后续批次（见 docs 的\"输入事件与共享内存\"节）。
//
// ============================ 事件语义（逐条）============================
//   KEY_DOWN / KEY_UP   code = **既有键码**（kernel/input.cpp 的键盘缓冲口径：可打印 ASCII /
//                       控制字符 0x01..0x1A / 0x1B(Esc) / NAV_UP 0xFD、NAV_DOWN 0xFE、
//                       NAV_LEFT 0xFB、NAV_RIGHT 0xFC / KBD_KEY_DELETE 0xFA、KBD_KEY_F2 0xF9、
//                       KBD_KEY_PAGEUP 0xF8、KBD_KEY_PAGEDOWN 0xF7）。
//                       KEY_UP 用**按下时记下的那个键码**（input64.cpp 的 256 项按下表），
//                       所以 `a` 的按下与抬起都是 0x61（不为第二个事件重新解码）。
//                       纯修饰键（Shift/Ctrl/Alt/Caps）**不单独产事件**（它们只出现在 mods 里）。
//   MOUSE_MOVE          x,y = 光标位置（像素）；dx,dy = 本次**实际生效**的位移（与
//                       kernel/input.cpp 的累加器/单步上限一致；y 是屏幕坐标，向下为正）。
//   MOUSE_DOWN / MOUSE_UP  code = 变化的那个键的**位掩码**（EV64_BTN_LEFT/RIGHT/MIDDLE），
//                       buttons = 当前全部按下的键位掩码。
//   WHEEL               dy = 滚轮增量（>0 = 向上/远离用户，与 PS/2 的 Z 同号）；code = 0。
//   mods 所有事件都带：Shift/Ctrl/Alt/Caps 的当前状态（EV64_MOD_*）。
//   t_ms PIT tick 换算的毫秒（250Hz -> 4ms 粒度，与 sleep_ms/ticks 同一时基）。
//
// 串口打点（自动验收 grep，格式勿改）：
//   [EV64] focus pid=<n> prev=<n> reason=request|capture-release|release|exit|policy-none
//   [EV64] attach pid=<n> flags=<hex> focus=<n> capture=<n>
//   [EV64] queue pid=<n> cap=<n> max=<n>
//   [EV64] drop pid=<n> type=<n> total=<n>            （队列满丢最旧；前 8 条 + 每 64 条一条）
//   [EV64] drops pid=<n> total=<n> suppressed=<n>     （被节流掉的 drop 行数，查询/出队时补打）
//   [EV64] poll pid=<n> max=<n> flags=<hex> got=<n> pend=<n> drops=<n>   （有预算地打，防刷屏）
//   [EV64] poll FAILED pid=<n> reason=<...> err=<n>
#pragma once
#include <stdint.h>

// ==================== ABI：事件结构（40 字节，POD，字段顺序固定）====================
struct Ev64Event {
    uint32_t type;      // +0  EV64_TYPE_*
    uint32_t code;      // +4  键码（键）/ 键位掩码（鼠标按下抬起）/ 0（移动、滚轮）
    int32_t  x;         // +8  光标 x（像素）
    int32_t  y;         // +12 光标 y（像素，向下为正）
    int32_t  dx;        // +16 本次实际生效的 x 位移
    int32_t  dy;        // +20 本次实际生效的 y 位移（WHEEL 时 = 滚轮增量）
    uint32_t buttons;   // +24 鼠标键位掩码
    uint32_t mods;      // +28 修饰键掩码
    uint64_t t_ms;      // +32 tick 换算的毫秒
};
static const uint32_t EV64_EVENT_SIZE64 = 40;
static const uint32_t EV64_MAX_EVENTS64 = 32;     // 每进程队列容量（满了丢最旧）

enum Ev64Type : uint32_t {
    EV64_TYPE_KEY_DOWN   = 1,
    EV64_TYPE_KEY_UP     = 2,
    EV64_TYPE_MOUSE_MOVE = 3,
    EV64_TYPE_MOUSE_DOWN = 4,
    EV64_TYPE_MOUSE_UP   = 5,
    EV64_TYPE_WHEEL      = 6
};

// input_poll 的 flags
static const uint32_t EV64_FLAG_FOCUS64   = 0x1u;   // 申请键盘焦点（\"我是前台\"）
static const uint32_t EV64_FLAG_CAPTURE64 = 0x2u;   // 申请指针捕获（鼠标事件优先给捕获者）
static const uint32_t EV64_FLAG_RELEASE64 = 0x4u;   // 释放焦点与捕获（与上面两位可同时给：先释放再申请）

// mods 位（与 input.cpp 的修饰键状态一致）
static const uint32_t EV64_MOD_SHIFT64 = 0x1u;
static const uint32_t EV64_MOD_CTRL64  = 0x2u;
static const uint32_t EV64_MOD_ALT64   = 0x4u;
static const uint32_t EV64_MOD_CAPS64  = 0x8u;

// 鼠标键位掩码（沿用 PS/2 包的口径：bit0 左、bit1 右、bit2 中）
static const uint32_t EV64_BTN_LEFT64   = 0x1u;
static const uint32_t EV64_BTN_RIGHT64  = 0x2u;
static const uint32_t EV64_BTN_MIDDLE64 = 0x4u;

// 错误码（负数；与 syscall64.h 里 A1 的口径一致：小负数，不与 Linux 号段共用）
static const int64_t EV64_EPERM64  = -1;    // 没有进程上下文（任务 0 / 共享地址空间模式）
static const int64_t EV64_EFAULT64 = -2;    // out 指针非法（没通过 user64_range_ok64）
static const int64_t EV64_ENODEV64 = -3;    // 没有事件层（理论上到不了）
static const int64_t EV64_ENOMEM64 = -4;    // 队列容量不足（max 超过队列容量）
static const int64_t EV64_EINVAL64 = -5;    // 参数非法（flags 有未知位）

// ==================== 每进程队列（Proc64 内嵌一份；布局是内核内部事）====================
struct Ev64Queue {
    Ev64Event ev[EV64_MAX_EVENTS64];
    uint32_t  head;          // 下一个出队下标
    uint32_t  count;         // 队列里的条数
    uint32_t  drops;         // 因满而丢弃的最旧事件数
    uint32_t  drop_logged;   // 已经打过的 drop 行数（节流用）
    uint32_t  suppressed;    // 被节流掉的 drop 行数（补打在 [EV64] drops 行里）
};

void     ev64_reset64(Ev64Queue* q);
uint32_t ev64_count64(const Ev64Queue* q);
void     ev64_push64(Ev64Queue* q, int pid, const Ev64Event* e);
uint32_t ev64_pop64(Ev64Queue* q, Ev64Event* out, uint32_t max);

// ==================== 当前进程的队列（weak 引用 proc64；安装介质内核里为 0）====================
// 返回 0 = 没有进程上下文（任务 0 / 共享模式 / 安装介质内核不链 proc64.cpp）。
Ev64Queue* ev64_current_queue64();
int        ev64_current_pid64();

// ==================== 系统调用落点（自有 ABI 12）====================
// 语义：max == 0 时只查询\"有多少待取\"（同时结算 drop 计数）；max > 0 时最多拷 max 条事件
// 到用户内存 out_uptr 并返回条数。flags 见 EV64_FLAG_*。返回负数 = 错误码（EV64_*）。
int64_t ev64_poll64(uint64_t out_uptr, uint32_t max, uint32_t flags);

// ==================== input.cpp 的产出钩子（IRQ 上下文调用）====================
// key_idx：按下表的索引（普通键 = 扫描码；E0 扩展键 = 扫描码 | 0x80）。
void ev64_key_press64(uint8_t key_idx, uint8_t code, uint32_t mods);
void ev64_key_release64(uint8_t key_idx, uint32_t mods);
// 鼠标：一次包解码完成后调用（type = EV64_TYPE_*）。
void ev64_mouse64(uint32_t type, uint32_t code, int32_t x, int32_t y,
                  int32_t dx, int32_t dy, uint32_t buttons, uint32_t mods);

// ==================== 进程退出/销毁时的清理（proc64 调用）====================
// 目标进程如果正持有焦点/捕获，就把它们清掉并打点（reason = \"exit\"）。
void ev64_proc_release64(int pid);

// ==================== 自检（启动期；位掩码，0 = 全过）====================
// bit0 结构体布局（40 字节 + 字段偏移）、bit1 队列语义（满丢最旧 + 计数）、
// bit2 焦点/捕获路由表（申请 -> 路由 -> 释放）、bit3 常量自洽（flags/mods/btn 不重叠）。
int ev64_selftest64();
