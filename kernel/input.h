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
void mouse_drain();                   // 清空按键/事件状态
