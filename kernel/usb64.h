// usb64.h - USB 主机（UHCI / USB 1.1）+ HID 引导键盘
//
// 范围（这一版做了什么、没做什么，写清楚免得误会）：
//   * 只驱动 **UHCI**（Intel 的 USB 1.1 主控）：PCI vendor=8086，device 见 usb64.cpp；
//     EHCI（USB 2.0）/ xHCI（USB 3.x）**没有**实现 —— 机器上只有 EHCI 时本模块打
//     "[USB64] not found" 后优雅退出（系统照常启动，桌面照常工作）。
//   * 只做**引导协议键盘**（HID bInterfaceClass=3 / bInterfaceSubClass=1）：
//     控制传输（枚举）+ 中断 IN（8 字节报告）。USB 鼠标/存储/集线器**没有**做；
//     设备接在 hub 后面也认不出来（只扫根端口）。
//   * 传输层用**轮询**（不接 IRQ11/IRQ10）：内核里有调度器，由 kusb 内核线程每 ~12ms
//     调一次 usb64_poll64()（见 kernel/task64.cpp 的 kusb 段）。
//   * 键盘事件走**和 PS/2 完全相同的那一条路**：HID 用法码 -> PS/2 集 1 扫描码 ->
//     input.cpp 的 kbd_inject_scancode()（同一套环形队列 + 修饰键 + WIN 键标志 +
//     Ctrl+Shift+Esc 热键）。桌面外壳（gui64.cpp）一行都不用改。
//
// 串口打点（自动验收 tests/usb64_test.py 靠这些行判定，改格式要同步改脚本）：
//   [USB64] uhci pci <bus>:<dev>.<fn> io=<hex> ports=<n>
//   [USB64] frame list @<hex> (1024 entries)
//   [USB64] port <n> connected speed=full|low reset ok
//   [USB64] no device on port <n>
//   [USB64] device addr=0 mps=<n> vendor=<hex> product=<hex>
//   [USB64] config set value=1 ifaces=<n> hid=1 ep_in=<hex> mps=<n>
//   [USB64] hid boot protocol set (8-byte reports)
//   [USB64] hid report key=<code> down=1|0      （只打新按下/释放边沿，不刷屏）
//   [USB64] selftest PASS | selftest FAIL mask=<n> | selftest skipped (<原因>)
//   [USB64] not found | no device on port <n> | enum FAILED stage=<哪一步>
#pragma once
#include <stdint.h>

// 启动链里调用一次（kernel64.cpp 的 os_boot_path 在 net64_init64() 之后）。
// 返回 0 = HID 引导键盘就绪；负值 = 优雅降级（找不到主控 / 没插设备 / 枚举失败），
// 调用方不要因为它非 0 就改变启动流程（系统必须照常起来）。
int usb64_init64();

// 轮询一次：处理"中断端点已完成的报告"并重新武装 TD。
// 由 kusb 内核线程周期调用（也可以在别处调，函数自身不做阻塞等待）。
void usb64_poll64();

// 位掩码自检：0 = 全过。没找到主控 / 没插设备时返回 0（属于合法降级，不算失败）。
int usb64_selftest64();

// 人类可读状态：init / not found / no device / enum failed / ready
const char* usb64_state_str64();

// ---- 只读统计（任务管理器/终端可用；不改任何状态）----
int      usb64_ports64();         // 根端口数（0 = 没有主控）
int      usb64_devices64();       // 已枚举成功的设备数（0 = 没有）
uint64_t usb64_hid_reports64();   // 收到的 8 字节 HID 报告总数
uint64_t usb64_key_events64();    // 按键**按下**事件数（HID 边沿检测后）
