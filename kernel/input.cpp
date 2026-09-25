// input.cpp - PS/2 键盘与鼠标驱动（手搓）
#include "input.h"
#include "port.h"
#include "x86_64.h"      // ★ 批次 L：ticks64()（记录左键按下包的到达时刻）
#include "debug64.h"   // ★ P2：Caps/Shift/滚轮打点无条件需要（原来只在 VIMTU_KBD_TRACE 下包含）

// ================ 8042（PS/2 控制器）公共助手 ================
//
// 踩坑记录（M2：安装程序里"按回车出来的是 Esc"，按键全乱）：
//   1) 写 8042 命令字节之前**必须先清空输出缓冲**。BIOS 或鼠标初始化留下的
//      ACK(0xFA)/鼠标字节会被当成"当前命令字节"读回来，再 | 0x01 写回去就变成
//      0xFB：bit6（扫描码集 2 -> 集 1 翻译）被清掉、bit5（鼠标时钟）被置上。
//      后果是键盘送来的是**集 2**扫描码，用集 1 表解码就成了"回车变 Esc"。
//      实测证据（QEMU monitor: sendkey ret）：
//          ps2_keyboard_event ... set 2 xlate 0
//          ps2_put_keycode keycode 0x5a        <- 集 2 的 Return
//      而客人侧收到 0x5a -> 集 1 表把它当成了别的键。
//      （诱因：kernel64.cpp 里 kbd_init/mouse_init 被调了两遍，第二次读到鼠标残留。）
//   2) 键盘 IRQ 只能处理"非鼠标"字节（状态位 0x20）。否则鼠标包字节会被当成扫描码
//      投递出去（实测收到 0x01/0x03 这种根本不存在的按键）。所以现在两个 IRQ 共用
//      一个"按状态位分流"的排空函数。

// ================ PS/2 原始字节跟踪（定位鼠标在真实 BIOS/VMware 下的异常） ================
// 打开 VIMTU_PS2_TRACE 后，每个从 0x60 读到的字节都按 "<来源><两位hex>" 打到串口：
//   'M' = 状态位 0x20（鼠标）  'K' = 键盘；包凑齐时额外打一行 "pkt b0 dx dy -> dx,dy"。
// 用途：VMware 里鼠标乱飞时，一眼看出是"包错位（b0 没有 bit3 同步位）"还是"位移本身就巨大"。
#ifdef VIMTU_PS2_TRACE
static void ps2_trace_byte(char src, uint8_t v) {
    static const char* hexd = "0123456789ABCDEF";
    char buf[4];
    buf[0] = src;
    buf[1] = hexd[(v >> 4) & 0xF];
    buf[2] = hexd[v & 0xF];
    buf[3] = 0;
    dbg64_str(buf);
}
#endif
#define PS2_ST_OBF   0x01        // 输出缓冲有数据（可读）
#define PS2_ST_IBF   0x02        // 输入缓冲满（写之前要等它清）
#define PS2_ST_AUX   0x20        // 该字节来自鼠标（辅助设备）

static void ps2_wait_input_clear() {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(0x64) & PS2_ST_IBF)) return;
        io_wait();
    }
}

static bool ps2_wait_output_full() {
    for (int i = 0; i < 100000; i++) {
        if (inb(0x64) & PS2_ST_OBF) return true;
        io_wait();
    }
    return false;
}

// 丢弃输出缓冲里的残留字节（残留的 ACK/鼠标包会污染后续读命令字节）
static void ps2_drain_out() {
    for (int i = 0; i < 256; i++) {
        if (!(inb(0x64) & PS2_ST_OBF)) return;
        (void)inb(0x60);
    }
}

// 读 8042 命令字节（0x20）。先排空，保证读到的确实是命令字节而不是残留 ACK。
static uint8_t ps2_read_cmd_byte() {
    ps2_drain_out();
    ps2_wait_input_clear();
    outb(0x64, 0x20);
    if (!ps2_wait_output_full()) return 0x45;      // 兜底：经典的 0x45（翻译开/键盘 IRQ 开）
    return inb(0x60);
}

// 写 8042 命令字节（0x60）。这里按标准值修正，**强制打开扫描码集翻译**：
//   bit0 键盘 IRQ、bit1 鼠标 IRQ、bit2 系统标志 = 1
//   bit3 忽略锁、bit4 键盘时钟禁止、bit5 鼠标时钟禁止、bit7 保留 = 0
//   bit6 扫描码集 2 -> 集 1 翻译 = 1（我们的解码表是集 1）
static void ps2_write_cmd_byte(uint8_t cfg) {
    cfg |= 0x01 | 0x02 | 0x04 | 0x40;
    cfg &= (uint8_t)~(0x08 | 0x10 | 0x20 | 0x80);
    ps2_wait_input_clear();
    outb(0x64, 0x60);
    ps2_wait_input_clear();
    outb(0x60, cfg);
}

// ================ 键盘 ================
// 环形缓冲（中断中写入，主循环读取）
#define KBD_BUF_SIZE 256
static volatile uint8_t kbd_buf[KBD_BUF_SIZE];
static volatile uint32_t kbd_head = 0, kbd_tail = 0;

// 修饰键状态
static volatile bool shift_pressed = false;
static volatile bool ctrl_pressed = false;
static volatile bool alt_pressed = false;
static volatile bool caps_lock = false;
static volatile bool scroll_lock = false;
static volatile bool num_lock = false;

// ★ P2：Caps/Shift/滚轮（见 input.h 的"批次 P2"一段）
static volatile uint32_t shift_toggles = 0;    // Shift 按下边沿累计（"Shift 切换中/英"接线证据）
static int kbd_log_budget = 12;                // [INPUT64] caps/shift 行上限（防刷屏）

// Win 键（E0 5B/5C 按下标志；GUI 读取后清除）
static volatile uint8_t win_key_flag = 0;
static bool e0_pending = false;
// Ctrl+Shift+Esc（任务管理器快捷键）：边沿触发，GUI 读取后清除
static volatile uint8_t tm_hotkey_flag = 0;

// 方向键导航码（复用键盘缓冲；避免与 Ctrl 组合 0x01-0x1A 冲突）
#define NAV_UP    0xFD
#define NAV_DOWN  0xFE
#define NAV_LEFT  0xFB
#define NAV_RIGHT 0xFC

// 扫描码集 1 -> ASCII（无 Shift 的普通键）
static const char kbd_map[128] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// Shift 版本
static const char kbd_map_shift[128] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static void kbd_push(uint8_t c) {
    uint32_t next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next != kbd_tail) {
        kbd_buf[kbd_head] = c;
        kbd_head = next;
    }
}

static bool kbd_pop(uint8_t* c) {
    if (kbd_tail == kbd_head) return false;
    *c = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return true;
}

bool kbd_has_char() { return kbd_tail != kbd_head; }
bool kbd_pop_char(uint8_t* c) { return kbd_pop(c); }
bool kbd_pop_raw(uint8_t* sc) { return kbd_pop(sc); }
bool kbd_win_pressed() { return win_key_flag != 0; }
void kbd_consume_win() { win_key_flag = 0; }
bool kbd_tm_hotkey() { return tm_hotkey_flag != 0; }
void kbd_consume_tm_hotkey() { tm_hotkey_flag = 0; }
bool kbd_ctrl_pressed() { return ctrl_pressed; }     // ★ 批次 J：修饰键状态（供 Ctrl 加选）
bool kbd_shift_pressed() { return shift_pressed; }   // ★ 批次 J：同上（供 Shift 加选）
// ★ P5：Ctrl+Shift+<字母> 的**按键时刻**记录（见 input.h 的说明）。取走即清空。
static volatile uint8_t ctrl_shift_ch = 0;
bool kbd_ctrl_shift_char64(char* out) {
    const uint8_t c = ctrl_shift_ch;
    if (!c) return false;
    ctrl_shift_ch = 0;
    if (out) *out = (char)c;
    return true;
}
void kbd_drain() { kbd_tail = kbd_head; }   // 停止阶段丢弃输入（不再接收新任务）

// ★ P2：Caps / 中英指示（无输入法时恒"英"）/ Shift 边沿计数
bool     kbd_caps_on() { return caps_lock; }
int      kbd_lang64() { return 0; }              // 0 = 英：本批**没有中文输入法**，指示器不许假装有
int      kbd_ime_available64() { return 0; }     // 0 = 无中文输入法（后期批次）
uint32_t kbd_shift_toggles64() { return shift_toggles; }

// 处理一个键盘扫描码（集 1；E0 前缀的扩展键在这里单独走）
static void kbd_process_scancode(uint8_t sc) {
    if (sc == 0xE0) { e0_pending = true; return; }
    if (sc == 0xE1) { e0_pending = false; return; }
    if (e0_pending) {
        e0_pending = false;
        // Win 键按下（边沿触发：释放不清除，GUI 读取后 consume 才清除）
        if (sc == 0x5B || sc == 0x5C) win_key_flag = 1;
        else if (sc == 0x48) kbd_push(NAV_UP);               // 上方向
        else if (sc == 0x50) kbd_push(NAV_DOWN);             // 下方向
        else if (sc == 0x4B) kbd_push(NAV_LEFT);             // 左方向
        else if (sc == 0x4D) kbd_push(NAV_RIGHT);            // 右方向
        else if (sc == 0x49) kbd_push((uint8_t)KBD_KEY_PAGEUP);    // ★ P2：PageUp（日历切年）
        else if (sc == 0x51) kbd_push((uint8_t)KBD_KEY_PAGEDOWN);  // ★ P2：PageDown（日历切年）
        else if (sc == 0x53) kbd_push((uint8_t)KBD_KEY_DELETE);  // ★ 批次 J：Delete（E0 53）
        return;
    }

    // ★ 批次 J：F2（扫描码 0x3C，不是可打印字符，旧表解码成 0 -> 直接丢掉；显式投递键码）
    if (sc == 0x3C) { kbd_push((uint8_t)KBD_KEY_F2); return; }


    // 修饰键
    // ★ P2：Shift 按下边沿累计（"Shift 切换中/英"这条需求的接线证据；没有中文输入法时 lang 恒为"英"）
    if ((sc == 0x2A || sc == 0x36) && !shift_pressed) {
        shift_toggles++;
        if (kbd_log_budget > 0) {
            kbd_log_budget--;
            dbg64_line_begin64();
            dbg64_str("[INPUT64] shift toggle count=");
            dbg64_dec((uint64_t)shift_toggles);
            dbg64_str(" lang=");
            dbg64_str(kbd_lang64() ? "中" : "英");
            dbg64_str(" ime=");
            dbg64_dec((uint64_t)kbd_ime_available64());
            dbg64_str(" (no Chinese IME -> indicator stays 英; IME is a later batch)");
            dbg64_nl();
            dbg64_line_end64();
        }
    }
    if (sc == 0x2A || sc == 0x36) shift_pressed = true;
    if (sc == 0xAA || sc == 0xB6) shift_pressed = false;
    if (sc == 0x1D) ctrl_pressed = true;
    if (sc == 0x9D) ctrl_pressed = false;
    if (sc == 0x38) alt_pressed = true;
    if (sc == 0xB8) alt_pressed = false;
    if (sc == 0x3A) {
        caps_lock = !caps_lock;
        if (kbd_log_budget > 0) {
            kbd_log_budget--;
            dbg64_line_begin64();
            dbg64_str("[INPUT64] caps on=");
            dbg64_dec(caps_lock ? 1 : 0);
            dbg64_str(" (letters switch case; Caps+Shift = lowercase)");
            dbg64_nl();
            dbg64_line_end64();
        }
    }
    if (sc == 0x45) num_lock = !num_lock;
    if (sc == 0x46) scroll_lock = !scroll_lock;

    // 仅处理按下（扫描码 < 0x80）
    if (sc < 0x80) {
        // Esc（扫描码 0x01）不在字符映射表里，显式投递 0x1B，否则各窗口的
        // Esc 分支（关闭设置窗口等）永远不会触发；Ctrl+Shift+Esc 作为
        // 任务管理器快捷键单独上报（不产生字符）。
        if (sc == 0x01) {
            if (ctrl_pressed && shift_pressed) tm_hotkey_flag = 1;
            else kbd_push(0x1B);
        }
        char c = shift_pressed ? kbd_map_shift[sc] : kbd_map[sc];
        if (sc == 0x01) c = 0;   // Esc 已单独处理
        if (c) {
            // ★ P2：Caps Lock 对字母生效（标准语义：Caps 反转 Shift 对字母的作用）
            //   旧写法第二条 `if (caps && c >= 'A' ..)` 把 Caps+Shift 也折成大写（等于 Shift 失效），
            //   这里改成：小写字母 -> 大写；大写字母（= Shift 按下的结果）-> 小写。
            if (caps_lock && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            else if (caps_lock && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            // Ctrl 组合
            if (ctrl_pressed) {
                if (c >= 'a' && c <= 'z') c = c - 'a' + 1;   // Ctrl-A = 0x01 等
                else if (shift_pressed && c >= 'A' && c <= 'Z') {
                    // ★ P5：Ctrl+Shift+<字母>（大写形态）—— 按键时刻的修饰键状态记下来，
                    //   外壳稍后取走（处理按键时 Shift/Ctrl 可能已经松开，见 input.h）。
                    ctrl_shift_ch = (uint8_t)c;
                }
            }
            kbd_push((uint8_t)c);
        }
    }
}

// ★ USB HID 键盘注入点（kernel/usb64.cpp 调用）：与 PS/2 共用 kbd_process_scancode，
//   也就是共用同一个环形队列、同一套修饰键/热键状态。调用方负责关中断（usb64.cpp 里
//   用 pushfq/cli/sti 包住"E0 前缀 + 主码"这两个字节），避免和 IRQ1 的 kbd_push 竞争 head。
void kbd_inject_scancode(uint8_t sc) {
    kbd_process_scancode(sc);
}

void kbd_init() {
    // 等待 8042 就绪
    ps2_wait_input_clear();
    // 启用键盘接口
    outb(0x64, 0xAE);
    io_wait();
    // 显式启用键盘 IRQ（8042 command byte bit0=1），并强制打开扫描码集翻译（bit6=1）。
    // 关键：ps2_read_cmd_byte() 会先排空输出缓冲，避免把残留 ACK 当命令字节写回去
    // （那会把 bit6 清掉 -> 键盘变成集 2 码 -> 解码全乱，见文件头的踩坑记录）。
    const uint8_t before = ps2_read_cmd_byte();
    ps2_write_cmd_byte((uint8_t)(before | 0x01));
    const uint8_t after = ps2_read_cmd_byte();
    ps2_drain_out();
#ifdef VIMTU_KBD_TRACE
    dbg64_str("[KBD] cmd_byte before=0x");
    dbg64_hex64(before);
    dbg64_str(" after=0x");
    dbg64_hex64(after);
    dbg64_str((after & 0x40) ? " translate=on" : " translate=OFF!");
    dbg64_nl();
#endif
    (void)before; (void)after;   // 只在 VIMTU_KBD_TRACE 下打点：关掉时不留未用变量告警
}

// ================ 鼠标 ================
// ================ 鼠标 ================
// 单步上限（像素）：每处理一个包最多让光标走这么多，剩下的留在累加器里。
// 这是"手感"的关键 —— 即使设备给来 ±255 的聚合位移，光标也不会一步窜到边界。
#define MOUSE_STEP_MAX 24
// 累加器上限（像素）：手停下来之后光标不该继续飘很远。
#define MOUSE_ACC_MAX  96
// 屏幕边界：默认**不钳制**（保持原行为；32 位桌面在自己的虚拟坐标里钳制并写回），
// 由 GUI 调用 mouse_set_bounds() 显式开启。安装界面向导前会调一次。
static volatile int mouse_max_x = 0x7FFFFFFF, mouse_max_y = 0x7FFFFFFF;
static volatile int mouse_x = 0, mouse_y = 0;
static volatile int mouse_accum_x = 0, mouse_accum_y = 0;
static volatile uint8_t mouse_buttons = 0;
static volatile int mouse_packet_cycle = 0;
static volatile uint8_t mouse_packet[4];
static volatile bool mouse_has_data = false;

// ★ 批次 L：按下事件用**小计数器**而不是布尔标志 —— 一次 IRQ 排空里可能同时到"按下+松开+按下"
//   （宿主负载高、客人排空间隔变长时实测出现），布尔标志会把两次按下并成一次，双击就丢了第一个。
//   上限 4：极端积压时也不会无界增长（GUI 每次循环消费一个）。
static volatile uint8_t pressed_left = 0, pressed_right = 0, pressed_middle = 0;
// ★ P5：**释放**事件也要计数 —— 帧采样（比对上一帧按钮位）会漏：按下+松开落在同一帧时
//   （拖拽/大重绘后的那几帧）release 边沿永远看不到，桌面图标的**双击**与拖拽结束就会失效。
static volatile uint8_t released_left = 0, released_right = 0, released_middle = 0;
static volatile uint32_t mouse_left_press_tick = 0;   // 最近一次左键按下的**包到达**时刻

bool mouse_button_pressed(int btn) {
    if (btn == 0) return pressed_left != 0;
    if (btn == 1) return pressed_right != 0;
    return pressed_middle != 0;
}
void mouse_consume_pressed(int btn) {
    if (btn == 0) { if (pressed_left) pressed_left--; }
    else if (btn == 1) { if (pressed_right) pressed_right--; }
    else { if (pressed_middle) pressed_middle--; }
}
bool mouse_button_released64(int btn) {
    if (btn == 0) return released_left != 0;
    if (btn == 1) return released_right != 0;
    return released_middle != 0;
}
void mouse_consume_released64(int btn) {
    if (btn == 0) { if (released_left) released_left--; }
    else if (btn == 1) { if (released_right) released_right--; }
    else { if (released_middle) released_middle--; }
}
uint32_t mouse_press_tick64() { return mouse_left_press_tick; }
bool mouse_has_event() { return mouse_has_data; }
void mouse_clear_event_flag() { mouse_has_data = false; }

// ★ P2：滚轮 = Intellimouse 的**第 4 字节**（Z）。设备侧握手见 mouse_try_wheel_mode64()：
//   只有设备回了 ID=3（滚轮鼠标）才切 4 字节包；否则老实按 3 字节解析（不猜、不假装）。
static volatile int mouse_wheel_mode = 0;    // 1 = 4 字节包（滚轮可用）
static volatile int mouse_wheel_acc = 0;     // 累计 Z（>0 = 滚轮向上；GUI 每帧 mouse_pop_wheel64 取走）
static uint8_t   mouse_pkt_size = 3;
static int       mouse_wheel_log_budget = 6;

int mouse_wheel_mode64() { return mouse_wheel_mode; }
int mouse_pop_wheel64() { const int v = mouse_wheel_acc; mouse_wheel_acc = 0; return v; }

// ==================== ★ P5：光标形状（状态在驱动层；切换由桌面外壳按命中区域调用）====================
static volatile int mouse_cursor_shape = MOUSE_CUR_ARROW;
static int mouse_cursor_log_budget = 80;      // [INPUT64] cursor 行上限（防刷屏脚本乱按）
static const char* kCursorNames[] = {
    "arrow", "text", "wait", "size-h", "size-v", "size-d1", "size-d2", "move"
};
const char* mouse_cursor_name64(int shape) {
    if (shape < 0 || shape > MOUSE_CUR_MOVE) return "arrow";
    return kCursorNames[shape];
}
int mouse_get_cursor64() { return mouse_cursor_shape; }
void mouse_set_cursor64(int shape) {
    if (shape < 0 || shape > MOUSE_CUR_MOVE) shape = MOUSE_CUR_ARROW;
    const int prev = mouse_cursor_shape;
    if (prev == shape) return;
    mouse_cursor_shape = shape;
    if (mouse_cursor_log_budget > 0) {
        mouse_cursor_log_budget--;
        dbg64_line_begin64();
        dbg64_str("[INPUT64] cursor shape=");
        dbg64_str(mouse_cursor_name64(shape));
        dbg64_str(" prev=");
        dbg64_str(mouse_cursor_name64(prev));
        dbg64_str(" (driver state; switched by gui64 hit-test)");
        dbg64_nl();
        dbg64_line_end64();
    }
}

// 处理一个**鼠标**字节（3 或 4 字节包状态机）。键盘 IRQ 读到鼠标字节时也走这里。
static void mouse_process_byte(uint8_t data) {
    const uint8_t pkt = mouse_pkt_size;
    mouse_packet[mouse_packet_cycle] = data;
    mouse_packet_cycle++;
    if (mouse_packet_cycle != (int)pkt) return;
    mouse_packet_cycle = 0;
    const uint8_t b0 = mouse_packet[0];
    // 检查同步位（包首字节的 bit3 恒为 1）
    if (!(b0 & 0x08)) {
        // 丢包/错位：滑动重同步 —— 若后续字节带同步位，把它当成新包头继续
#ifdef VIMTU_PS2_TRACE
        ps2_trace_byte('!', b0);
#endif
        int k = 1;
        for (; k < (int)pkt; k++) if (mouse_packet[k] & 0x08) break;
        if (k < (int)pkt) {
            for (int i = k; i < (int)pkt; i++) mouse_packet[i - k] = mouse_packet[i];
            mouse_packet_cycle = (int)pkt - k;
        }
        return;
    }
    // ★ 位移的符号位在 **b0 的 bit4(X) / bit5(Y)**，两个数据字节是**无符号幅值**
    //   （Linux psmouse 也是这套约定：sign ? byte-256 : byte）。
    //   这里原先写成 (int8_t) 符号扩展 —— 只要设备报出 128..255 的正向幅值就会读成负数：
    //   实测 VMware 快速移动时 b0=0x08、X 字节 0x88/0x91/0x96（= +136/+145/+150），
    //   旧写法读成 −120/−111/−106，光标于是**朝反方向飞**、撞边后贴着屏幕边缘来回抖，
    //   也就是用户看到的"箭头只在窗口最边缘跑"。QEMU 的位移都很小（|d|<128）两种写法等价，
    //   所以这个 bug 在 QEMU 里一直没露头。
    int dx = (b0 & 0x10) ? (int)mouse_packet[1] - 256 : (int)mouse_packet[1];
    int dy = (b0 & 0x20) ? (int)mouse_packet[2] - 256 : (int)mouse_packet[2];
    // 溢出保护（标准 PS/2：bit6=X 溢出，bit7=Y 溢出；bit4/bit5 是符号位，不能当溢出）
    if (b0 & 0x40) dx = dx > 0 ? 127 : -127;
    if (b0 & 0x80) dy = dy > 0 ? 127 : -127;
    // ---- 位移累加器：既不失真、也不"一步窜到屏幕边上" ----
    // 设备给的"一包位移"是它两次采样之间的位移。客人读包偏慢时（安装界面每帧整屏重绘，
    // 一帧几十毫秒）这 100Hz 的包会攒在一起，实测 VMware 单包能到 ±255 像素。
    // 直接累加的话光标一帧就冲到边界、贴着边滑（用户看到的"鼠标一动就围着窗口边缘动"）。
    // 这里的做法：把它加到累加器，每次最多走出来 MOUSE_STEP_MAX 像素，剩余留到下一包；
    // 累加器本身也有上限（MOUSE_ACC_MAX），避免手停之后光标还在飘。
    mouse_accum_x += dx * 17 / 10;      // 灵敏度 ×1.7（历史上调出来的手感）
    mouse_accum_y += dy * 17 / 10;
    if (mouse_accum_x >  MOUSE_ACC_MAX) mouse_accum_x =  MOUSE_ACC_MAX;
    if (mouse_accum_x < -MOUSE_ACC_MAX) mouse_accum_x = -MOUSE_ACC_MAX;
    if (mouse_accum_y >  MOUSE_ACC_MAX) mouse_accum_y =  MOUSE_ACC_MAX;
    if (mouse_accum_y < -MOUSE_ACC_MAX) mouse_accum_y = -MOUSE_ACC_MAX;

    int sx = mouse_accum_x;
    int sy = mouse_accum_y;
    if (sx >  MOUSE_STEP_MAX) sx =  MOUSE_STEP_MAX;
    if (sx < -MOUSE_STEP_MAX) sx = -MOUSE_STEP_MAX;
    if (sy >  MOUSE_STEP_MAX) sy =  MOUSE_STEP_MAX;
    if (sy < -MOUSE_STEP_MAX) sy = -MOUSE_STEP_MAX;
    mouse_accum_x -= sx;
    mouse_accum_y -= sy;
    mouse_x += sx;
    mouse_y -= sy;   // 屏幕坐标 y 向下
    // 边界钳制：**上下界都要钳**。以前只钳了 0 下界，越界位置一路累积（越跑越远），
    // 光标要么被画到屏幕外、要么贴着最边上那一列/行滑动，看起来就是"只在边缘动"。
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x > mouse_max_x - 1) mouse_x = mouse_max_x - 1;
    if (mouse_y > mouse_max_y - 1) mouse_y = mouse_max_y - 1;
    const uint8_t nb = (uint8_t)(b0 & 0x07);
    const uint8_t old = mouse_buttons;
    // ★ 批次 L：左键按下记录**包到达时刻**（双击窗口用，见 mouse_press_tick64）；按下事件计数（不并包）
    if ((nb & 1) && !(old & 1)) { mouse_left_press_tick = ticks64(); if (pressed_left < 4) pressed_left++; }
    if ((nb & 2) && !(old & 2)) { if (pressed_right < 4) pressed_right++; }
    if ((nb & 4) && !(old & 4)) { if (pressed_middle < 4) pressed_middle++; }
    if (!(nb & 1) && (old & 1)) { if (released_left < 4) released_left++; }
    if (!(nb & 2) && (old & 2)) { if (released_right < 4) released_right++; }
    if (!(nb & 4) && (old & 4)) { if (released_middle < 4) released_middle++; }
    mouse_buttons = nb;
    mouse_has_data = true;
    // ★ P2：滚轮（4 字节包的第 4 字节 = Z，二补码；标准 PS/2/Intellimouse 约定：+1 = 向上/远离用户）
    if (pkt == 4) {
        const int8_t z = (int8_t)mouse_packet[3];
        if (z) {
            mouse_wheel_acc += z;
            if (mouse_wheel_acc > 1000) mouse_wheel_acc = 1000;
            if (mouse_wheel_acc < -1000) mouse_wheel_acc = -1000;
            if (mouse_wheel_log_budget > 0) {
                mouse_wheel_log_budget--;
                dbg64_line_begin64();
                dbg64_str("[INPUT64] wheel z=");
                dbg64_dec((uint64_t)(uint32_t)(int32_t)z);
                dbg64_str(" acc=");
                dbg64_dec((uint64_t)(uint32_t)(int32_t)mouse_wheel_acc);
                dbg64_str(" packet=4B");
                dbg64_nl();
                dbg64_line_end64();
            }
        }
    }
#ifdef VIMTU_PS2_TRACE
    // 结果光标位置（4 位 hex + 4 位 hex），用来确认它不再贴在边缘
    dbg64_str("|c");
    dbg64_hex64((uint64_t)(uint32_t)mouse_x & 0xFFFF);
    dbg64_str(",");
    dbg64_hex64((uint64_t)(uint32_t)mouse_y & 0xFFFF);
    dbg64_nl();
#endif
}

// 排空 8042 输出缓冲，并按状态位把每个字节分给对的处理器。
// 键盘 IRQ1 与鼠标 IRQ12 共用这一个函数：一次中断就把缓冲读空
// （只读 1 个会让队列积压 -> 丢释放事件 -> 8042 typematic 自动重复产生重复字符；
//  鼠标包错位则会看到 ±127 的假位移）。
static void ps2_drain_irq() {
    for (int guard = 0; guard < 64; guard++) {
        uint8_t st = inb(0x64);
        if (!(st & PS2_ST_OBF)) return;
        uint8_t data = inb(0x60);
#ifdef VIMTU_PS2_TRACE
        ps2_trace_byte((st & PS2_ST_AUX) ? 'M' : 'K', data);
#endif
        if (st & PS2_ST_AUX) mouse_process_byte(data);
        else                 kbd_process_scancode(data);
    }
}

void kbd_irq()   { ps2_drain_irq(); }
void mouse_irq() { ps2_drain_irq(); }

static void mouse_wait_write() {
    for (int i = 0; i < 10000; i++) {
        if (!(inb(0x64) & PS2_ST_IBF)) return;
    }
}
static void mouse_wait_read() {
    for (int i = 0; i < 10000; i++) {
        if (inb(0x64) & PS2_ST_OBF) return;
    }
}
static void mouse_write(uint8_t cmd) {
    mouse_wait_write();
    outb(0x64, 0xD4);
    mouse_wait_write();
    outb(0x60, cmd);
}
static uint8_t mouse_read() {
    mouse_wait_read();
    return inb(0x60);
}

// ★ P2：滚轮握手。返回 1 = 设备是滚轮鼠标（成功切到 4 字节包）。
//   序列（Intellimouse 规范 / Linux psmouse 同款）：采样率 200 -> 100 -> 80（魔数），
//   再 0xF2 读设备 ID：3 = 滚轮 5 键，0 = 老 3 字节鼠标。任何一步没有 ACK（0xFA）就如实放弃。
static volatile int mouse_wheel_id = -1;
static int mouse_wheel_id64() { return mouse_wheel_id; }
static int mouse_try_wheel_mode64() {
    const uint8_t rates[3] = {200, 100, 80};
    for (int i = 0; i < 3; i++) {
        mouse_write(0xF3);                        // Set Sample Rate
        if (mouse_read() != 0xFA) { mouse_wheel_id = -1; ps2_drain_out(); return 0; }
        mouse_write(rates[i]);
        if (mouse_read() != 0xFA) { mouse_wheel_id = -1; ps2_drain_out(); return 0; }
    }
    mouse_write(0xF2);                            // Get Device ID
    if (mouse_read() != 0xFA) { mouse_wheel_id = -1; ps2_drain_out(); return 0; }
    const uint8_t id = mouse_read();
    mouse_wheel_id = id;
    ps2_drain_out();                              // 握手期间可能夹带的采样包
    return (id == 3 || id == 4) ? 1 : 0;
}

void mouse_init() {
    // 启用辅助设备（鼠标）
    mouse_wait_write(); outb(0x64, 0xA8);
    // 打开 IRQ12（bit1）并解除鼠标时钟禁止（bit5=0）；同样先排空再读，
    // 命令字节里 bit6 翻译必须保持为 1（ps2_write_cmd_byte 会强制）。
    const uint8_t before = ps2_read_cmd_byte();
    ps2_write_cmd_byte((uint8_t)(before | 0x02));
    const uint8_t after = ps2_read_cmd_byte();
#ifdef VIMTU_KBD_TRACE
    dbg64_str("[MOUSE] cmd_byte before=0x");
    dbg64_hex64(before);
    dbg64_str(" after=0x");
    dbg64_hex64(after);
    dbg64_nl();
#endif
    (void)before; (void)after;   // 同上：只在 VIMTU_KBD_TRACE 下打点
    // 默认设置
    mouse_write(0xF6);
    uint8_t ack1 = mouse_read();
    (void)ack1;
    // 启用数据报告
    mouse_write(0xF4);
    uint8_t ack2 = mouse_read();
    (void)ack2;
    // 清空输出缓冲：丢弃初始化残留/初始采样（如 QEMU 启动时遗留的 0xFF），
    // 避免污染后续 3 字节包的同步位判断。
    for (int i = 0; i < 100 && (inb(0x64) & PS2_ST_OBF); i++) inb(0x60);
    // QEMU 可能在 F4 后异步发送初始采样包，忙等一小段再清一次，避免首个
    // 数据包错位成 (b0, dx, dy) 污染累计位移（实测 QEMU mouse_move 的
    // 第一包曾出现 dx=8,dy=100 的异常值）。
    for (int t = 0; t < 4; t++) {
        for (volatile int w = 0; w < 30000; w++);
        for (int i = 0; i < 100 && (inb(0x64) & PS2_ST_OBF); i++) inb(0x60);
    }
    // ★ P2：滚轮握手（Intellimouse 4 字节包）——采样率魔数序列 200/100/80 + 读设备 ID（0xF2）。
    //   只有设备明确回 ID=3/4（滚轮/5 键）才切 4 字节包；否则老实留在 3 字节（不猜、不假装）。
    //   做在 F4 之后、清缓冲之前：握手期间设备可能在流里插入字节，随后统一排空。
    mouse_wheel_mode = mouse_try_wheel_mode64();
    mouse_pkt_size = mouse_wheel_mode ? 4 : 3;
    {
        mouse_wheel_log_budget = 6;
        dbg64_line_begin64();
        dbg64_str("[INPUT64] wheel mode=");
        dbg64_dec((uint64_t)mouse_wheel_mode);
        dbg64_str(" id=");
        dbg64_dec((uint64_t)mouse_wheel_id64());
        dbg64_str(mouse_wheel_mode ? " packet=4B (Intellimouse; QEMU monitor: mouse_move dx dy dz)"
                                   : " packet=3B (device has no wheel; scroll keys unavailable)");
        dbg64_nl();
        dbg64_line_end64();
    }
    // 初始位置：屏幕中央（由 GUI 设置，这里给默认）
    mouse_x = 512;
    mouse_y = 384;
}

int  mouse_get_x() { return (int)mouse_x; }
int  mouse_get_y() { return (int)mouse_y; }
void mouse_set_pos(int x, int y) { mouse_x = x; mouse_y = y; }
// GUI 设定屏幕边界：光标位置会被钳制在 [0, w-1] × [0, h-1]（鼠标驱动自己钳，
// 这样即使设备给出 ±255 的聚合位移，也不会把位置累积到屏幕外）
void mouse_set_bounds(int w, int h) {
    if (w > 0) mouse_max_x = w;
    if (h > 0) mouse_max_y = h;
}
uint8_t mouse_get_buttons() { return (uint8_t)mouse_buttons; }

// IRQ 包装（x86.cpp / x86_64.cpp 分发）
extern "C" void irq1_handler() { ps2_drain_irq(); }
extern "C" void irq12_handler() { ps2_drain_irq(); }

// 停止阶段丢弃输入：清空鼠标按键/事件标志（鼠标坐标保留）
void mouse_drain() {
    pressed_left = 0;
    pressed_right = 0;
    pressed_middle = 0;
    released_left = 0;
    released_right = 0;
    released_middle = 0;
    mouse_has_data = false;
    mouse_packet_cycle = 0;
}
