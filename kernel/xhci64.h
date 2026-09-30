// xhci64.h - USB 3.x 主机控制器（xHCI 1.0/1.1）驱动 + HID 引导键盘 + USB 存储（只读）
//
// 范围（这一版做了什么、没做什么，写清楚免得误会）：
//   * 主控：PCI **class 0x0C / subclass 0x03 / prog-if 0x30**（USB 3.0 xHCI，见 PCI 规范 0x0C0330）。
//     找不到时优雅降级：只打一行 "[XHCI] not found"，此后本模块彻底沉默（系统照常启动/桌面照常）。
//   * MMIO：BAR0（64 位 MMIO BAR，恒等映射，前 4GB）——CAPLENGTH/HCSPARAMS1..3/HCCPARAMS1/DBOFF/
//     RTSOFF；USBCMD.HCRST 复位；CONFIG.MaxSlotsEn；DCBAAP；CRCR（命令环）；ERST + 事件环
//     （ERSTSZ/ERSTBA/ERDP，EHB 语义见 kernel/xhci64.cpp 的 evt_next）。
//   * 命令：Enable Slot -> Address Device（Input Context + 设备上下文 + EP0 环）-> Configure Endpoint。
//   * 传输：控制（Setup/Data/Status 三个 TRB）、批量（Normal TRB + 短包）、中断（HID 键盘）。
//     全程**轮询**（不接 MSI/IRQ），由 kusb 内核线程经 usb64_poll64() 调 xhci64_poll64()。
//   * 设备：**最多两台**（1 个 HID 引导键盘 + 1 个 USB 存储），只认直接插在根端口上的设备
//     （不做 hub、不做热插拔、不做 USB3 SuperSpeed 的真实链路训练验证 —— 见 .cpp 末尾的"没做到"）。
//   * 键盘：报告 -> **和 UHCI 完全相同的那一条路**（HID 用法码 -> PS/2 集 1 扫描码 ->
//     input.cpp 的 kbd_inject_scancode()）。所以"xHCI 键盘能打字"与 UHCI 共用同一套桌面注入。
//   * 存储：BOT（Bulk-Only Transport）+ SCSI 只读子集（INQUIRY / TUR / REQUEST SENSE /
//     READ CAPACITY(10) / READ(10)），驱动器号接入见 kernel/ata64.h 的 ATA64_USB_BASE（24..）——
//     与 UHCI 的 U 盘共用同一段驱动器号，顺序：先 UHCI（usb64）后 xHCI。
//
// 串口打点（自动验收 tests/xhci64_test.py 靠这些行判定，改格式要同步改脚本）：
//   [XHCI] pci <b>:<d>.<f> bar0=<hex> caplen=<hex> hcs1=<hex> max_slots=<n> max_ports=<n> csz=<0|1> ver=<hex>
//   [XHCI] proto rev=2|3 portoff=<hex> ports=<n>          （Supported Protocol 扩展能力，如实列出）
//   [XHCI] reset HCRST ok usbcmd=<hex> usbsts=<hex>       （复位完成）
//   [XHCI] cmd ring @<hex> erst @<hex> event ring @<hex> dcbaa @<hex> scratchpad=<n>
//   [XHCI] doorbell mode=<n> enable slot=<n> ok           （DB Target 位序的自探结果）
//   [XHCI] mfindex=0x<hex> delta=0x<hex>                  （时间真的在走：复位/等 PRC 期间采样）
//   [XHCI] erdp ehb cleared erdp=0x<hex>                  （ERDP.EHB 写 1 清语义）
//   [XHCI] port <n> connected speed=low|full|high|super reset ok
//   [XHCI] no device on port <n>
//   [XHCI] device addr=<n> speed=<..> mps=<n> vendor=<hex> product=<hex>
//   [XHCI] config set value=1 ifaces=<n> hid=1 ep_in=<hex> mps=<n> interval=<n>        （键盘）
//   [XHCI] config set value=1 ifaces=<n> msc=1 ep_in=<hex> mps=<n> ep_out=<hex> mps=<n>（存储）
//   [XHCI] hid boot protocol set (8-byte reports)
//   [XHCI] hid report key=<code> down=1|0                 （只打边沿）
//   [XHCI] msc inquiry vendor=<..> product=<..>
//   [XHCI] msc capacity blocks=<n> block_size=<n> bytes=<n>
//   [XHCI] msc READ(10) ok lba=<n> count=<n> bytes=<n> crc=XXXXXXXX
//   [XHCI] port status change port=<n> portsc=<hex>       （运行期事件；本驱动不做热插拔枚举）
//   [XHCI] selftest PASS | selftest FAIL mask=<n> | selftest skipped (<原因>)
//   [XHCI] not found                                      （没有 xHCI 主控 -> 优雅降级）
#pragma once
#include <stdint.h>

// 启动链里调用一次（kernel64.cpp 的 os_boot_path 在 usb64_init64() **之后**）。
// 返回 0 = 至少一台设备（键盘或存储）就绪；负值 = 优雅降级（找不到主控 / 没插设备 / 枚举失败）。
// 调用方不要因为它非 0 就改变启动流程（系统必须照常起来）。
int xhci64_init64();

// 轮询一次：把事件环里已完成的事件处理掉（HID 报告 -> 注入；端口变化 -> 打点）并重新武装中断 TRB。
// 由 kusb 内核线程**经 usb64_poll64()** 周期调用（task64.cpp 不改）；函数自身不做阻塞等待，
// 拿不到锁（有同步批量传输在跑）就直接返回，下次再来。
void xhci64_poll64();

// 位掩码自检：0 = 全过。没找到主控 / 没插设备时返回 0（属合法降级，不算失败）。
// bit0 = 复位/环失败；bit1 = 枚举到设备但一台也没就绪；bit2 = 键盘探测失败；bit3 = 存储探测失败。
int xhci64_selftest64();

// 人类可读状态：init / not found / no device / enum failed / ready
const char* xhci64_state_str64();

// ---- 只读统计 ----
int      xhci64_ports64();          // 根端口数（0 = 没有主控）
int      xhci64_devices64();        // 已枚举成功的设备数（0 = 没有；键盘 + U 盘）
uint64_t xhci64_hid_reports64();    // 收到的 8 字节 HID 报告总数
uint64_t xhci64_key_events64();     // 按键**按下**事件数（HID 边沿检测后）
int      xhci64_doorbell_mode64();  // DB Target 位序的自探结果（0/1/2，排障用）

// ---- USB 存储（BOT + SCSI 只读）：语义与 usb64_msc_* 完全一致（见 usb64.h）----
//   * 只暴露"探测全过"的盘（INQUIRY + READ CAPACITY + READ(10) 都成功）；
//   * lba/count 单位是 **512 字节扇区**；缓冲区必须恒等映射或落在内核镜像里（否则走 DMA 暂存页）；
//   * 只读：没有 WRITE(10)（上层 ata64_write 对 USB 驱动器号直接失败并打点）。
int  xhci64_msc_count64();
bool xhci64_msc_info64(int idx, char* model, int model_cap, uint64_t* sectors_512);
bool xhci64_msc_read64(int idx, uint32_t lba, uint32_t count, void* buf);
