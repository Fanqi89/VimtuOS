// usb64.h - USB 主机（UHCI / USB 1.1）+ HID 引导键盘 + **USB 存储（U 盘，只读）**
//
// 范围（这一版做了什么、没做什么，写清楚免得误会）：
//   * 只驱动 **UHCI**（Intel 的 USB 1.1 主控）：PCI vendor=8086，device 见 usb64.cpp；
//     EHCI（USB 2.0）/ xHCI（USB 3.x）**没有**实现 —— 机器上只有 EHCI 时本模块打
//     "[USB64] not found" 后优雅退出（系统照常启动，桌面照常工作）。
//   * 设备：**最多两台**——1 个 HID 引导键盘（bInterfaceClass=3 / SubClass=1）+ 1 个
//     USB 存储（Class=8 / SubClass=6 / Protocol=0x50 = Bulk-Only Transport）。
//     两台都插着时各自用独立地址（1、2）；第三种设备、集线器后面的设备仍认不出来。
//   * 传输层用**轮询**（不接 IRQ11/IRQ10）：内核里有调度器，由 kusb 内核线程每 ~12ms
//     调一次 usb64_poll64()（见 kernel/task64.cpp 的 kusb 段）。
//   * 键盘事件走**和 PS/2 完全相同的那一条路**：HID 用法码 -> PS/2 集 1 扫描码 ->
//     input.cpp 的 kbd_inject_scancode()（同一套环形队列 + 修饰键 + WIN 键标志 +
//     Ctrl+Shift+Esc 热键）。桌面外壳（gui64.cpp）一行都不用改。
//   * USB 存储走 **BOT（Bulk-Only Transport）+ SCSI 子集**：INQUIRY / TEST UNIT READY /
//     REQUEST SENSE / READ CAPACITY(10) / READ(10)。**本批只读**：没有 WRITE(10)、没有
//     分区表解析、没有拔出检测（热插拔）。驱动器号接入见 kernel/ata64.h 的 ATA64_USB_BASE。
//
// 串口打点（自动验收 tests/usb64_test.py / tests/usbstorage_test.py 靠这些行判定，
// 改格式要同步改脚本）：
//   [USB64] uhci pci <bus>:<dev>.<fn> io=<hex> ports=<n>
//   [USB64] frame list @<hex> (1024 entries)
//   [USB64] port <n> connected speed=full|low reset ok
//   [USB64] no device on port <n>
//   [USB64] device addr=0 mps=<n> vendor=<hex> product=<hex>
//   [USB64] set address=<n> ok
//   [USB64] config set value=1 ifaces=<n> hid=1 ep_in=<hex> mps=<n>        （键盘）
//   [USB64] config set value=1 ifaces=<n> msc=1 ep_in=<hex> ep_out=<hex> mps=<n>  （存储）
//   [USB64] hid boot protocol set (8-byte reports)
//   [USB64] hid report key=<code> down=1|0      （只打新按下/释放边沿，不刷屏）
//   [USB64] selftest PASS | selftest FAIL mask=<n> | selftest skipped (<原因>)
//   [USB64] not found | no device on port <n> | enum FAILED stage=<哪一步>
//   [USBST] iface found class=08 sub=06 proto=50 ep_in=<hex> ep_out=<hex>
//   [USBST] inquiry vendor=<..> product=<..>
//   [USBST] capacity blocks=<n> block_size=<n> bytes=<n> cap_mb=<n>[ cap_gb=<n>]
//   [USBST] read lba=<n> count=<n> ok
//   [USBST] read FAILED lba=<n> reason=<csw status|timeout|nak|block-size|no-device>
//   [USBST] selftest PASS mask=0 | selftest FAIL mask=<n> | selftest skipped (no storage device)
//   [USBST] write refused (USB storage is read-only in this batch) ...     （见 kernel/ata64.cpp）
//   未做（如实）：拔出检测 "[USBST] detached" —— 本批没有热插拔/拔出检测。
#pragma once
#include <stdint.h>

// 启动链里调用一次（kernel64.cpp 的 os_boot_path 在 net64_init64() 之后）。
// 返回 0 = HID 引导键盘就绪；负值 = 优雅降级（找不到主控 / 没插设备 / 枚举失败 / 只插了
// U 盘没插键盘），调用方不要因为它非 0 就改变启动流程（系统必须照常起来）。
int usb64_init64();

// 轮询一次：处理"中断端点已完成的报告"并重新武装 TD。
// 由 kusb 内核线程周期调用（也可以在别处调，函数自身不做阻塞等待）。
void usb64_poll64();

// 位掩码自检：0 = 全过。没找到主控 / 没插设备时返回 0（属于合法降级，不算失败）。
// bit0..bit7 = HID 键盘那套；bit8(256) = 存储设备枚举到了但 BOT 探测失败。
int usb64_selftest64();

// 人类可读状态：init / not found / no device / enum failed / ready
const char* usb64_state_str64();

// ---- USB 存储（U 盘）：Bulk-Only Transport + SCSI 只读 ----
// 语义（如实）：
//   * usb64_msc_count64() = 检测到的 USB 存储设备数（本批 0 或 1）；
//   * 每个"块" = READ CAPACITY(10) 报的 block_size，**只支持 512**（其它值如实拒绝，
//     设备仍然枚举/打点，但不会暴露成块设备）；
//   * usb64_msc_read64() 的 lba/count 单位是 **512 字节扇区**（= ata64 的语义），
//     缓冲区必须恒等映射（内核 .bss/.data 或 page_alloc_64 的页）—— DMA 直接用它的物理地址。
int  usb64_msc_count64();
bool usb64_msc_info64(int idx, char* model, int model_cap, uint64_t* sectors_512);
bool usb64_msc_read64(int idx, uint32_t lba, uint32_t count, void* buf);
int  usb64_msc_selftest64();                 // 0 = 全过（没插 U 盘时也是 0 = 跳过）
const char* usb64_msc_last_reason64();       // 最近一次失败原因（排障/打点用）

// ---- 只读统计（任务管理器/终端可用；不改任何状态）----
int      usb64_ports64();         // 根端口数（0 = 没有主控）
int      usb64_devices64();       // 已枚举成功的设备数（0 = 没有；键盘 + U 盘）
uint64_t usb64_hid_reports64();   // 收到的 8 字节 HID 报告总数
uint64_t usb64_key_events64();    // 按键**按下**事件数（HID 边沿检测后）
