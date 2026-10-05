// ehci64.h - USB 2.0 主机控制器（EHCI）驱动 + USB 存储（BOT/SCSI，★ 可写）
//
// 范围（这一版做了什么、没做什么，写清楚免得误会）：
//   * 主控：PCI **class 0x0C / subclass 0x03 / prog-if 0x20**（USB 2.0 EHCI）。找不到就优雅降级：
//     只打一行 "[EHCI] not found"，之后本模块彻底沉默（系统照常启动、桌面照常工作）。
//   * MMIO：**BAR0**（64 位 MMIO BAR，恒等映射，前 4GB）——CAPLENGTH / HCSPARAMS / HCCPARAMS /
//     DBOFF(仅打点) / **EECP -> USBLEGSUP BIOS handoff**；USBCMD.HCRESET 软复位；
//     CONFIGFLAG=1（把所有端口路由给 EHCI）；**异步调度**（ASYNCLISTADDR + 一个自环 QH + qTD 链）；
//     PORTSC 轮询（CCS/CSC/PED/PE/PortOwner）。
//   * 端口策略（如实）：**只接管 High-speed 端口**。PortOwner=1 的端口是伴随控制器（UHCI/OHCI）的，
//     本驱动**不抢**（只打点 owner=1）；复位后 PED 没置起来（= 设备是 Full/Low speed）的端口
//     写 PortOwner=1 交还伴随控制器（打点 owner=1 speed=full|low）。**不做 companion 端口上的传输**，
//     所以 FS/LS 设备（键鼠）在纯 EHCI 端口上不被本驱动使用。
//   * 设备：**最多一台 USB 存储（U 盘）**（Class=8 / SubClass=6 / Protocol=0x50 = Bulk-Only Transport，
//     两个批量端点）。★ **HID 键盘在 EHCI 上没做**（中断传输）—— 键盘由 UHCI（kernel/usb64.cpp）与
//     xHCI（kernel/xhci64.cpp）覆盖；这条边界在报告里如实写明。
//   * 传输：控制（SETUP/DATA/STATUS 三段 qTD）+ 批量（qTD 链），全程**轮询**（不接 USB IRQ），
//     由 kusb 内核线程经 usb64_poll64() -> ehci64_poll64() 驱动（见 kernel/task64.cpp）。
//   * 存储：BOT（Bulk-Only Transport）+ SCSI 子集（INQUIRY / TUR / REQUEST SENSE / READ CAPACITY(10) /
//     READ(10) + **WRITE(10)**）。语义与 usb64_msc_* **完全一致**（见 kernel/usb64.h）：
//     写后读回逐字节校验、**读/写都做越界拒绝**（读越界在本驱动里也提前拒绝，见 .cpp 说明）、
//     块大小只支持 512。驱动器号接入 kernel/ata64.h 的 ATA64_USB_BASE（24..）——
//     ★ 本轮：**经 kernel/usb64.cpp 的 usb64_msc_* 门面**接入（门面 = UHCI + EHCI 聚合，
//     索引顺序：UHCI(0..u-1)、EHCI(u..u+e-1)，之后才是 xHCI；见 kernel/usb64.h）。
//
// 串口打点（自动验收 tests/ehci64_test.py 靠这些行判定，改格式要同步改脚本）：
//   [EHCI] not found                                       （没有 EHCI 主控 -> 优雅降级）
//   [EHCI] pci <bus>:<dev>.<fn> mmio=<hex> ports=<n> caplen=<n>
//   [EHCI] reset ok hcs=<hex> hcc=<hex> dboff=<n>          （软复位 + 能力寄存器快照）
//   [EHCI] bios handoff ee=<hex> took=<0|1>               （USBLEGSUP/OSOWN；没有 EECP 时不打）
//   [EHCI] async qh=<hex> qtd=<hex> qtds=<n>
//   [EHCI] port <n> owner=<0|1> speed=<high|full|low> ccs=<0|1>
//   [EHCI] port <n> reset ok speed=high ped=1
//   [EHCI] device addr=<n> mps=<n> vendor=<hex> product=<hex>
//   [EHCI] set address=<n> ok
//   [EHCI] control setup <bRequest> ok                    （每个成功控制请求一行；bRequest = 2 位十六进制）
//   [EHCI] bulk ok bytes=<n>
//   [EHCI] config set value=1 ifaces=<n> msc=1 ep_in=<hex> ep_out=<hex> mps=<n>
//   [EHCI] enum FAILED stage=<..> rc=<n>                  （**只在启动期真失败**时；热插拔重试失败另打一行）
//   [EHCI] hotplug port=<n> attach failed stage=<..> rc=<n>（重试失败；不冒充整机枚举失败）
//   [EHCI] port <n> attached/detached                     （运行期热插拔；本驱动会重枚举）
//   [EHCI] selftest PASS | selftest FAIL mask=<n> | selftest skipped (no controller)
// 存储（U 盘）那套沿用 UHCI 的既有格式（[USBST] 族，见 kernel/usb64.h 顶部）：
//   [USBST] iface found class=08 sub=06 proto=50 ep_in=.. ep_out=..
//   [USBST] inquiry vendor=.. product=.. rmb=<0|1>
//   [USBST] capacity blocks=<n> block_size=<n> bytes=<n> cap_mb=<n>[ cap_gb=<x.yy>]
//   [USBST] read lba=<n> count=<n> ok / read FAILED lba=<n> count=<n> reason=<..>
//   [USBST] write lba=<n> count=<n> ok / write FAILED lba=<n> count=<n> reason=<..>
//   [USBST] write verify lba=<n> count=<n> ok (read back, byte-for-byte)
//   [USBST] write-bounds probe blocks=<n> lba=blocks rejected=1 lba=blocks-1 count=2 rejected=1
//     （★ 越界的**写**边界探针；两次都必须被拒绝且不碰介质 —— 与 UHCI 同一条纪律）
//   [USBST] read-bounds probe blocks=<n> …（越界的**读**边界探针；同上）
//   [USBST] storage attached on EHCI -> rescan drive letters (usb drives=<n>)   （★ 门面在 kernel/usb64.cpp 里打）
#pragma once
#include <stdint.h>

// 启动链里调用一次（kernel64.cpp 的 os_boot_path，在 xhci64_init64() **之后**）。
// 返回 0 = 存储设备就绪；负值 = 优雅降级（找不到主控 / 没插设备 / 枚举失败）。调用方不要因为它非 0
// 就改变启动流程（系统必须照常起来）。
int ehci64_init64();

// 轮询一次：看根端口有没有插拔（CCS 变化），有就重枚举。由 kusb 内核线程**经 usb64_poll64()** 调用
// （task64.cpp 不改）。函数自身不做阻塞等待：拿不到传输锁就直接返回，下次再来。
// ★ P8b 纪律（与 UHCI 同一条）：① 基线（g_port_ccs/g_hp_armed）只在 ehci64_init64() **全部收尾之后**
//   建立 —— poll 在 arm 之前直接返回，不会把刚枚举好的设备当成"新插入"；② 端口上还是本驱动**已经在用**
//   的设备（g_msc.present）时**不复位、不重枚举**（多一次复位会让正在工作的设备离开总线）；
//   ③ 热插拔重试失败只打 [EHCI] hotplug … attach failed（不冒充 [EHCI] enum FAILED / [USB64] enum FAILED）。
void ehci64_poll64();

// 位掩码自检：0 = 全过。没找到主控 / 没插设备时返回 0（属合法降级，不算失败）。
// bit0 = 复位/内存结构没建起来；bit1 = 控制器没在跑（USBCMD.RS 读回 0）；bit2 = 端口数不合理；
// bit3 = ASYNCLISTADDR 读回不对；bit4 = 有端口设备但一台也没就绪；bit5 = U 盘枚举到了但 BOT 探测没过；
// bit6 = 越界读的**边界探针**没按预期被拒（读路径的守卫坏了才置位）；
// bit7 = 越界写的**边界探针**没按预期被拒（写路径的守卫坏了才置位；与 UHCI 的 bit7 同义）。
int ehci64_selftest64();

// ★ 运行期热插拔的基线是否已经建立（= ehci64_init64() 全部收尾完成）。
// 给门面（kernel/usb64.cpp）用来**在初始化完成之后**才建立"EHCI 存储数量变化 -> 重扫盘符"的基线：
// 返回 false 之前一律不当事件（与 P8b 的教训同一条：初始化没收尾就比对会踩半初始化状态）。
bool ehci64_hotplug_ready64();

// 人类可读状态：init / not found / no device / enum failed / ready
const char* ehci64_state_str64();

// ---- 只读统计 ----
int ehci64_ports64();          // 根端口数（0 = 没有主控）
int ehci64_devices64();        // 已枚举成功的设备数（本驱动最多 1 = U 盘）
uint32_t ehci64_link_ok64();   // 复位后 PED=1（真正跑起来的）端口数

// ---- USB 存储（BOT + SCSI，★ 可写）：语义与 usb64_msc_* / xhci64_msc_* 完全一致（见 kernel/usb64.h）----
//   * 只暴露"探测全过"的盘（INQUIRY + TUR + READ CAPACITY + READ(10) 都成功）；
//   * lba/count 单位是 **512 字节扇区**；缓冲区必须恒等映射或落在内核镜像里（否则走 DMA 暂存页）；
//   * **可写**：WRITE(10) + 写后读回逐字节校验 + 越界拒绝（与 UHCI 的 U 盘同一条语义）。
int  ehci64_msc_count64();
bool ehci64_msc_info64(int idx, char* model, int model_cap, uint64_t* sectors_512);
bool ehci64_msc_read64(int idx, uint32_t lba, uint32_t count, void* buf);
bool ehci64_msc_write64(int idx, uint32_t lba, uint32_t count, const void* buf);
const char* ehci64_msc_last_reason64();
