// input.h - PS/2 键盘与鼠标驱动接口
#pragma once
#include <stdint.h>

// 键盘
void kbd_init();
void kbd_irq();                 // IRQ1 处理
bool kbd_pop_char(uint8_t* c);  // 取一个已转换字符（含 Shift/Caps）
bool kbd_pop_raw(uint8_t* sc);  // 取原始扫描码
bool kbd_has_char();
bool kbd_win_pressed();         // Win 键按下（E0 5B/5C）
void kbd_consume_win();
// Ctrl+Shift+Esc：打开任务管理器（边沿触发；GUI 读取后 consume）
bool kbd_tm_hotkey();
void kbd_consume_tm_hotkey();
void kbd_drain();               // 清空键盘队列（重启的停止阶段丢弃输入）

// ★ 批次 J 新增：特殊键码（复用键盘字符队列，落在 0xFA/0xF9；与方向键 NAV_* 的 0xFB..0xFE 同一约定）
//   为什么要有：Delete / F2 不是可打印字符，旧表里解码成 0（被直接丢掉），文件管理器拿不到。
//   注意：Ctrl+字母仍然按老口径**折成控制字符**（Ctrl+A=0x01、Ctrl+C=0x03、Ctrl+X=0x18、Ctrl+V=0x16），
//   所以应用要判 Ctrl 组合就认这些控制码；kbd_ctrl_pressed() 只是把修饰键状态暴露出来（供 Ctrl 点选）。
#define KBD_KEY_DELETE 0xFAu    // Delete（扫描码集 1 的 E0 53）
#define KBD_KEY_F2     0xF9u    // F2（扫描码 0x3C）
bool kbd_ctrl_pressed();        // Ctrl 当前是否按下（读修饰键状态；不做边沿、不消费）
bool kbd_shift_pressed();       // Shift 当前是否按下（同上）
// ★ 给 USB HID 键盘（kernel/usb64.cpp）用：把一个 PS/2 集 1 扫描码（含 E0 前缀就分两次调）
//   投进**同一个**按键环形队列 —— 复用 Shift/Ctrl/Alt/Caps、方向键 NAV_*、Win 键标志
//   (E0 5B/5C) 与 Ctrl+Shift+Esc 热键的全部逻辑。桌面外壳（gui64.cpp）因此一行都不用改。
void kbd_inject_scancode(uint8_t sc);

// 鼠标
void mouse_init();
void mouse_irq();               // IRQ12 处理
int  mouse_get_x();
int  mouse_get_y();
void mouse_set_pos(int x, int y);   // GUI 将光标限制在屏幕内时写回
void mouse_set_bounds(int w, int h);   // GUI 设定屏幕边界：光标会被驱动钳制在界内
uint8_t mouse_get_buttons();    // bit0=左, bit1=右, bit2=中
bool mouse_has_event();
void mouse_clear_event_flag();

// 鼠标按钮事件（按下瞬间）
bool mouse_button_pressed(int btn);   // 0=左 1=右 2=中
void mouse_consume_pressed(int btn);

// ★ 批次 L：最近一次**左键按下包到达**时刻（ticks64() 的 tick 值；还没按下过 = 0）。
//   双击判定请用它，不要用 GUI 回调时刻：一帧重绘/调度延迟会把"用户的双击"误判成两次单击。
//   （实测：宿主负载高时两次按下包在 200ms 内到达，GUI 回调却被拖到相隔 >500ms。）
uint32_t mouse_press_tick64();
void mouse_drain();                   // 清空按键/事件状态
