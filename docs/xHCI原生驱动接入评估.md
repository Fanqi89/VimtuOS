# xHCI 原生驱动接入评估（内存配置 / 中断配置 / TinyUSB·CherryUSB 对接）

> 只读分析结论。所有断言带 `文件:行`。凡代码里没有的，直接写"代码里没有，需要新增"。

## 0. 前提纠正（这条最重要）

**不存在"USB 设备走 PS/2 兼容模式"这件事。** 仓库里的 xHCI 原生驱动已经完整实现并在跑：
`kernel/xhci64.cpp`（1825 行）+ `kernel/xhci64.h`。PS/2 8042（`kernel/input.cpp`）是一条**独立输运**，
两条链只在 `kbd_process_scancode()` 汇合，不是"USB 伪装成 PS/2"。

所以本评估的真实问题不是"从零接 xHCI"，而是：**在已有原生 xHCI 轮询驱动之上，补中断、补 hub/热插拔、
评估要不要换成 TinyUSB/CherryUSB 的 class 层**。

---

## 1. 现状盘点

### 1.1 做到哪一步

| 能力 | 证据 |
|---|---|
| PCI 枚举控制器（class 0x0C / sub 0x03 / prog-if 0x30，带能力寄存器复核） | `kernel/xhci64.cpp:394-427` |
| 64 位 MMIO BAR0（BAR 在 4 GiB 之上直接放弃） | `xhci64.cpp:1410-1421`，拒绝逻辑 `:1422-1429` |
| 能力寄存器 CAPLENGTH/HCSPARAMS1-2/HCCPARAMS1/DBOFF/RTSOFF | `xhci64.cpp:1433-1456` |
| Supported Protocol 扩展能力（xECP 是 dword 偏移，端口偏移在 DW2） | `xhci64.cpp:1491-1508` |
| USBCMD.HCRST 软复位 + 等 CNR/HCH | `xhci64.cpp:1554-1571` |
| 命令环 CRCR(RCS=1) | `xhci64.cpp:1578-1579` |
| 事件环 + ERST + ERDP(EHB 写 1 清) | `xhci64.cpp:1582-1599`，读写路径 `466-497` |
| DCBAA + scratchpad 指针数组（max_slots ≤255 才安全） | `xhci64.cpp:1511-1552`（数组在 `+0x800`，`:1543`），上限检查 `:1444-1445` |
| 命令：Enable Slot / Address Device(BSR 两段) / Configure Endpoint | `xhci64.cpp:1054-1056`、`718-731`、`737-792` |
| 控制传输（Setup 包**内联** + IDT，Data 按 MPS 切分，Status 只要事件） | `xhci64.cpp:796-898` |
| 批量传输（Normal TRB + IOC，短包终止） | `xhci64.cpp:902-941` |
| 中断 IN（HID 键盘，报告回来就重新武装） | `xhci64.cpp:944-950` |
| 每端点 Transfer Ring（ep0 / ep_in / ep_out / ep_int） | 结构 `xhci64.cpp:262`，初始化 `1074-1077`，一页 4 个环（`+0x000/0x400/0x800/0xC00`）|
| 每设备 Device Context + Input Context | `xhci64.cpp:1069-1078`（DCBAA[slot] 写入 `:1078`）|
| 读**和**写：控制 IN/OUT、批量 IN/OUT 都有（CBW 走 bulk OUT） | `xhci64.cpp:1276`、`1279`；但存储层只暴露 READ(10)，**没有 WRITE(10)** ⇒ `1308-1379`、`xhci64.h:73` |
| 门铃 DB Target 位序自探（0/1/2 三种约定） | `xhci64.cpp:366-374`、`632-667` |
| 速率协商（PORTSC.Port Speed → Slot Context Speed + EP0 MPS，SS 的 bMaxPacketSize0 是指数编码） | `xhci64.cpp:955-1001`、`1093-1103` |
| 上限：2 台设备 / 64 TRB 环 / 64 事件 | `xhci64.cpp:165-177` |

**没做的**（驱动自己写在文件头）：hub、运行期热插拔枚举、多配置/多接口切换、USB 鼠标、带宽协商、
MSI/MSI-X/中断、挂起恢复、存储写。`xhci64.cpp:41-44`。

### 1.2 tests/xhci64_test.py 断言什么

四场景（`:281-434` 键盘 / `:436-562` 键盘+U 盘 / `:564-583` 有主控无设备 / `:585-610` 无主控），
逐条：控制器识别 `:294-313`（含 `proto rev=2 portoff=0x05 ports=4`、`rev=3 portoff=0x01 ports=4`）、
HCRST `:316-318`、cmd/ERST/event/DCBAA/scratchpad `:319-322`、doorbell+enable slot `:323-325`、
MFINDEX delta≠0 `:326-332`、EHB 清 `:333-335`、端口复位+速率 `:338-342`、
`device addr=N speed=high mps=64 vendor=0627 product=0001` `:343-348`、**Transfer Event 落环** `:350-352`、
Configure Endpoint（`ep_in=03 mps=8`）`:353-357`、boot protocol `:358-359`、selftest PASS `:360`、
端到端打字（`key=E3` → `[UI] menu open`，`key=1E` → `[APP] term opened`）`:369-386`、
**跨事件环回卷 30 键 6 条 `ls /` + 零 stray** `:401-425`、存储 SuperSpeed `:452-454`、INQUIRY/CAPACITY `:459-465`、
**READ(10) 的 crc/head 与宿主镜像逐字节核对** `:468-481`、盘符 `disk=24 fs=FAT32` `:489-492`、
存储 I/O 前后键盘都可用 `:496-558`、U 盘镜像 CRC 不变 `:561-562`、两条降级 `:574-580`/`:596-609`。
FORBIDDEN 词表 `:86-87`。

### 1.3 设备事件从哪来（完整调用链）

```
kernel64.cpp:1453   xhci64_init64()            // 在 usb64_init64(:1448) 之后、ehci64_init64(:1464) 之前
task64.cpp:739      usb64_poll64()             // kusb 内核线程，约每 12ms 一次
usb64.cpp:748       xhci64_poll64()
xhci64.cpp:1751     evt_next -> xhci_dispatch(:510)
xhci64.cpp:548      usb64_hid_report64()       // HID 报告 -> PS/2 集 1 用法码
input.cpp:306       kbd_inject_scancode()  -> kbd_process_scancode(:206)
```
PS/2 那条是**另一条**：`input.cpp:675 irq1_handler -> ps2_drain_irq(:545) -> kbd_process_scancode(:206)`。
结论：USB 键盘走的是**原生 xHCI 中断端点 + kusb 轮询**，注入点共用而已。

---

## 2. 内存配置

### 2.1 现有可复用分配器（签名 + 适合度）

| 接口 | 签名/位置 | 适合度 |
|---|---|---|
| `page_alloc_64()` / `page_free_64(void*)` | `mem_64.h:11-12`，实现 `mem64.cpp:191-206` | ★★★ **首选**。4KB 页，返回值就是物理地址；页池 `[0x0A000000, ≤0x8A000000)` 全在 4 GiB 内且恒等映射。xHCI 的 DCBAA/命令环/事件环/ERST/上下文/bounce 全部来自它（`xhci64.cpp:1511-1515`、`1069-1070`、`1545`）。注意**不保证连续**，所以每个结构必须单独占一页（现状正是如此） |
| `kmalloc_64(uint64_t)` / `kfree_64` | `mem_64.h:17-18`，堆 `mem64.cpp:61-62`/`113-116`，`[max(112MB,ALIGN(__bss_end)), +48MB)` | ★★ 指针同样是物理地址（同一恒等窗口），但**块不保证页对齐/连续** ⇒ 只能放软件状态、驱动私有结构、或需要手动 64B 对齐的数据缓冲 |
| `ML64_HEAP_PHYS_FLOOR` / `ML64_POOL_PHYS_FLOOR` | `memlayout64.h:126`/`:128`（112 MB / 160 MB） | 常量，用于断言。`mem64.cpp:127-131` 的 `guard=ok/FAIL` 是运行期闸门 |
| AHCI 的 DMA 暂存池做法 | `ahci64.cpp:128`（`AHCI64_DMA_PAGES=16`）、`:152`、`:737-745`、散射路径 `:513-546` | ★★★ **可直接复用**。它的模型（"一页一项 PRDT，认不出物理地址就 memcpy 进出"）与 xHCI 的"一页一个 Normal TRB"完全同构；xHCI 现在只用了**一个** bounce 页（`xhci64.cpp:1515`、`:909-914`），要支持多页散列读就照 AHCI 抄 |
| `proc64_shm_view64` / `proc64_shm_map64` | `proc64.h:265-281` | ★ 页帧数组（page_alloc 出来的**离散**帧）。只对 ring3 零拷贝有意义，内核 xHCI 不需要 |
| `proc64_devmap64(bdf, bar, phys, len, out_va)` | `proc64.h:311`，槽位常量 `:295-298`（8 槽 × 256 KB） | ★★ ring3 方案的前提：把 xHCI BAR0 映射进用户进程。QEMU 的 BAR0 是 16 KB，256 KB 槽够 |
| `pci64_bar_probe64(bdf, bar, Pci64Bar64*)` | `pci64.h:55`，实现 `pci64.cpp:60-` | ★★ 有真正的 **BAR sizing**（写全 1 回读，`pci64.h:9`/`12-18` 讲清风险与还原）。**xHCI 驱动现在没用它**，自己读了 `0x10/0x14` 但没 sizing（`xhci64.cpp:1410-1421`）。对 xHCI 没必要——BAR 内部布局由 caplen/rtsoff/dboff 决定 |

### 2.2 "PA == VA" 凭什么成立；xHCI 64 位 DMA 指针能不能照用

成立依据（四条互相独立）：
1. `memlayout64.h:116` `ML64_IDENTITY_BYTES = 0x100000000`，`:31` 注释"地址是物理地址，内核通过 0..4GB 恒等映射直接访问"；
2. `boot/loader64.asm:1281-1312` 用 **2 MB 大页**建 0..4 GB 恒等映射（PML4[0] → PDPT[0..3] → 4 张 PD）；
3. `proc64.h:12-20` 明确"PDPT[0..3] 2MB 大页恒等映射 0..4GB"、"恒等映射必须共享：内核堆/页池/LFB 都是按物理地址直接访问的"；
4. `mem64.cpp:55-57` 页池上界 = `0x0A000000 + 2 GiB = 0x8A000000` < 4 GiB ⇒ `page_alloc_64()` 永远给 4 GiB 以内的恒等页。

xHCI 的换算函数已经照这个假设写：`xhci64.cpp:298-305` `xhci_pa64()` —— `<4GiB` 直接用；内核高半区 `.bss/.data`
减去 `ML64_KERNEL_VA_BASE - ML64_KERNEL_BASE` 直映；**认不出来返回 0，调用方退回 bounce**（`:909-914`）。

⇒ **可以照用，现状就是这么做的**。但它有两条硬约束，改代码时不能破：
- 每个 DMA 结构必须**落在单页内且 64B/页对齐**：DCBAA 与 scratchpad 指针数组共用一页（`:1543` 用 `+0x800`），
  恰好 256 项 DCBAA 之后 ⇒ `max_slots > 255` 就会踩，已由 `:1444-1445` 挡住；
- 高半区直映只覆盖"内核镜像本身"（`:302-303` 用 `__bss_end + 0x10000` 封顶），所以**不要把 kmalloc 的地址
  当高半区直映**——堆在低半区恒等窗口里，走的是第一条分支，没问题。

### 2.3 15 MiB 硬闸门会不会被撑爆

- 闸门：`ML64_KERNEL_SECTORS = 30720`（`memlayout64.h:69`）⇒ 15,728,640 B；构建断言 `build64.sh:56`、`:917-921`。
- 当前：`build64/kernel64_os.bin = 3,166,544 B` ⇒ **余量 12,562,096 B（≈11.98 MiB）**。
- 参照物（实测 `size build64/os/xhci64.o`）：现有**完整** xHCI 驱动 `text=38,145 B / data=12 / bss=744`。
- 估算：TinyUSB host 栈（HCD + HUB + HID + MSC，同功能面）按 30–70 KB text 量级 ⇒ 占余量 0.3%–0.6%。**撑不爆。**
- 另一个更紧的闸门是 `.bss`：`__bss_end` 实测 PA = `0x2D8C000` = 47,759,360 B（`nm build64/kernel64_os.elf`），
  载荷窗 `0x04000000` = 67,108,864 B ⇒ **余量 19.3 MB**（断言 `build64.sh:927-934`）。
  TinyUSB 的静态池（`CFG_TUH_DEVICE_MAX`、端点 FIFO）是几十 KB~几百 KB 级，安全；但**开 `CFG_TUSB_DEBUG`
  走 printf 会直接链接失败**（没有 libc）。

---

## 3. 中断配置

### 3.1 现状

| 层 | 位置 | 事实 |
|---|---|---|
| 8259 PIC | `x86_64.cpp:234-253`（`pic_unmask`/`pic_unmask64`） | 默认模式 |
| LAPIC + IOAPIC | `apic64.cpp` / `apic64.h` | 试探式接管，失败退回 PIC（`apic64.h:6-13`）；模式变量 `g_irq_mode64`（`x86_64.cpp:21`） |
| IOAPIC 路由表 | `apic64.cpp:86-92` | **只有 4 条**：IRQ0→vec32、IRQ1→33、IRQ12→44、IRQ14→46 |
| 其它 IRQ | `apic64.cpp:555-573` | 不在表里就 `"not routed -> ignored (stays masked)"`，重定向项**保持掩码** |
| IDT 注册 | `x86_64.cpp:104-112` `idt_set_gate64()`；`:116-124` `kIsrTable[49]`；`:126-138` `idt_build()` | **只有向量 0..47 + 128** 装了桩；其余 `P=0`（`:134-137`）⇒ 触发即 #GP |
| ISR 桩 | `isr_stubs64.asm:105-106`（`%assign irq 32` 循环） | 32..47 |
| 中断分发 | `x86_64.cpp:344-375` | IRQ 32..47；**硬编码** `irq==1/12/14`（`:370-372`），其余只 EOI 后丢弃（`:373`） |
| EOI | `x86_64.cpp:225-232` | APIC 模式写 LAPIC 0xB0（`apic64_eoi64`，`apic64.h:42`） |
| MSI / MSI-X | — | **全仓库没有实现**。只有三处注释提到它没做（`xhci64.cpp:43`、`ehci64.cpp`、`virtio_gpu64.cpp`）；`pci64.cpp:6` 明确"不做 capability 链遍历"；没有 cap id 0x05/0x11 解析、没有 Message Address/Data 写入、没有向量分配 |

### 3.2 现有驱动是轮询还是中断

全是轮询：
- AHCI：`ahci64.h:9` "**全程轮询** PxCI 清零 / PxIS / PxTFD（不开端口中断、不 unmask IRQ）"
- e1000/net：`net64.cpp:7` "不用中断 … 收包靠主动 `net64_poll64()`"
- UHCI：`usb64.cpp:29` "不接 IRQ11/IRQ10、不使能 USBINTR"
- xHCI：`xhci64.cpp:1580` `IMAN = 0`；`USBCMD.INTE` 宏在 `:89` 定义但**从未使用**；`xhci64.h:11` "全程轮询（不接 MSI/IRQ）"

### 3.3 加 xHCI 中断最少要动哪几处

pin-based INTx 路线（最省，现实可行）——**4 处**：
1. `kernel/xhci64.cpp`：读 PCI config `0x3C`（INTERRUPT_LINE，现在从不读）；置 `USBCMD.INTE`(bit2) + `IMAN.IE`(bit1)；
   新增 `xhci64_irq64()`：读 USBSTS 判 EINT → **写 1 清 EINT**(bit3) → 调用事件环 drain（复用 `evt_next`/`xhci_dispatch`）。
2. `kernel/x86_64.cpp:363-375`：加设备中断路由。**不要**再硬编码一个 `if (irq==N)`；建议改为
   `void (*g_irq_handlers[16])(void)` + `irq_register64(irq, fn)`，否则每加一个 PCI 驱动都要改 ISR 分发。
3. `kernel/apic64.cpp:86-92`：把该 IRQ 加进 `g_routes[]`（GSI 由 ISO 查表 `:452-459`、`apic64_iso_lookup`:289-301），
   否则 APIC 模式下 `pic_unmask64()`（`x86_64.cpp:244-253`）→ `apic64_unmask_irq64`（`apic64.cpp:555-573`）
   只会打一行 "not routed -> ignored"，中断永远不来。
4. IDT 向量：**如果 IRQ 号落在 0..15（QEMU piix3 上的 PCI IRQ 通常 10/11），向量 42/43 已在
   `x86_64.cpp` 的 `kIsrTable`（`x86_64.cpp:116-124`）与 `isr_stubs64.asm:105-106` 里存在，不用新增桩**。
   只有想用 MSI（任意向量，例如 0x50）才需要：(a) asm 生成新桩、(b) `kIsrTable` 扩容、(c) `idt_build` 的 flags 表、
   (d) `g_routes[].vec` 改成新向量。**这四件事代码里都没有，需要新增。**

MSI/MSI-X 路线（额外，全部需要新增）：解析 PCI cap 链（`0x34` → cap id `0x05`/`0x11`）、写
Message Address（LAPIC 物理基址 `0xFEE00000` | dest）/ Message Data（向量）、分配并安装该向量的 IDT 桩、
APIC 模式下不需要 IOAPIC 重定向项（这也是 MSI 唯一的收益点）。

### 3.4 纯轮询可行性（Event Ring 轮询的坑）

现状就是纯轮询且已经跑通端到端（§1.2 全部断言在 QEMU 上通过）。三个坑，都已有代码级答案：

1. **消费 cycle 必须随回卷翻转**：`xhci64.cpp:474-483` 用长注释记录了"原来 `g_evt_cycle` 恒为 1 ⇒ 第 65 个事件起
   全部不可见 + 重读旧条目打 stray"。测试用 30 键跨回卷验证（`xhci64_test.py:401-425`，要求 `stray = 0`）。
   **这是 Event Ring 轮询最容易死的地方**，任何"改成中断"的重构都不能顺手删掉 `:483`。
2. **ERDP 每个事件都要前移，EHB 需要时写 1 清**：`xhci64.cpp:484-495`（`(erdp & 8) ? next|8 : next`）。
   漏掉它 → 硬件以为还没消费完，事件环写满后停写。
3. **轮询与同步传输争同一事件环**：`g_evt_ready` 在枚举完成前关闭轮询（`xhci64.cpp:1739`，原因写在 `:1600-1603`）；
   互斥用 `try-lock`（`:268-281`，poll 侧 `:1748` 拿不到就返回）。
   ⇒ **如果加中断，必须"中断接管 ⇒ 关掉 kusb 轮询"二选一**，否则立刻重演 `:1600-1603` 记录的竞态。

`IMAN.IE` 与 `EINT` 在纯轮询下**不需要**（现状 `IMAN=0`、`USBCMD.INTE` 从不置位），因为消费靠 cycle 位判定，
不依赖 EINT。

---

## 4. TinyUSB / CherryUSB 对接的坑

### 4.1 编译环境事实

- 内核：`build64.sh:69-73` —— `-target x86_64-elf -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
  -fno-pic -fno-pie -fno-exceptions -fno-rtti -fno-threadsafe-statics -mno-red-zone -mcmodel=kernel
  -mno-sse -mno-sse2 -mno-mmx -mno-avx -O2 -std=c++17`。**没有 libc、没有 CMSIS、没有 RTOS tick。**
- 用户态：`tools/vmtool_build_win.sh:15`/`:20` —— `-target x86_64-unknown-none-elf -nostdinc -nostdlib -ffreestanding`。
- 内核自有原语：`memcpy_64/memset_64/memcmp_64/strlen_64/strcpy_64`（`mem_64.h:226-233`）、
  `kmalloc_64/kfree_64`（`:17-18`）、`g_ticks64 + ms_to_ticks64`（用法见 `xhci64.cpp:307-312`）、
  串口 `dbg64_*`（`debug64.h`）、`irq_save64/irq_restore64`（`mem64.cpp:41-49`，**是 static，需另写一份**）。

### 4.2 移植层最少要实现哪些

- **时间**：`board_millis()` / `usb_osal_msleep()` → `g_ticks64` + `ms_to_ticks64`（照 `xhci64.cpp:307-312` 的写法，
  自旋上界 `nop_pause()`）。
- **内存**：`osal_malloc/free` → `kmalloc_64/kfree_64`；DMA 结构 → `page_alloc_64/page_free_64`。
  **注意**：TinyUSB 的 `usbh` 会 malloc 每个设备实例与端点 FIFO；CherryUSB 更适合 freestanding（它有可配置静态池）。
- **临界区**：`osal_mutex_*` / 关中断 → 自旋锁（`xhci64.cpp:268-281`）+ 自己的 `irq_save`。
- **日志/断言**：`TU_LOG*`/`printf` → `dbg64_str/dbg64_hex64`；`TU_ASSERT` → 自定义（**没有 `abort()`**）。
- **配置宏**：`CFG_TUH_ENABLED`、`CFG_TUSB_MCU=OPT_MCU_NONE`（裸 port）、`CFG_TUSB_OS=OPT_OS_NONE`。
- **HCD 回调**：`hcd_init/hcd_deinit/hcd_port_reset/hcd_edpt_open/hcd_xfer` 等必须自己实现。

### 4.3 代码事实给出的结论（不是手册背诵）

**TinyUSB / CherryUSB 上游没有 xHCI host 控制器驱动。**
- TinyUSB 的 host 侧把 MCU 抽象成"由芯片提供 `hcd_*` 回调"，社区实现集中在 dwc2 / chipidea / rp2040 / EHCI；
- CherryUSB 有 `usb_hc_ehci.c`（EHCI host），xhci 需要自己写 `usbh_xhci.c`。

本仓库**已经拥有**它们唯一能提供的那部分——class 层：
HID 报告解析（`usb64_hid_report64`，被 `xhci64.cpp:548` 调用）、配置描述符解析（`xhci64.cpp:1006-1041`）、
BOT/SCSI 命令构造（`xhci64.cpp:1262-1291`）、描述符/重试/超时（`:796-898`）。

⇒ 引入这两个栈**不会减少工作量**，反而要多写一层 `hcd_*` 把现有的环/门铃/上下文逻辑包起来。
它们真正带来的是**上游维护的 hub 支持与设备类驱动** —— 而这两个恰好是本仓库缺的（`xhci64.cpp:41-42`）。

### 4.4 放 ring0 还是 ring3

**判断：xHCI 控制器留在 ring0。**

理由（按证据强度排）：
1. 项目自己的架构文档把"PS/2 键盘鼠标、USB xHCI/EHCI 核心"划在 **ring0**，理由写的是"输入延迟直接决定手感"
   —— `docs/混合驱动架构.md:24`；切分原则"控制器在内核、协议在用户态" —— 同文 `:32-34`。
2. ring3 现在**做不了 DMA**：没有"DMA 物理页分配 + 物理地址导出给用户态"的 API。`drvdemo.c:48` 自己写明
   "不申请 MEM/BUS-MASTER 功能位、不做 DMA（没有 DMA 映射 API）"；`docs/应用层与系统调用说明.md:3181` 同结论。
   xHCI 的 DCBAA/环/上下文**全部**是 DMA 结构 ⇒ 没有这个 API 就不可能 ring3。
3. ring3 现在**拿不到中断**：`docs/应用层与系统调用说明.md:3198` "`pci_map_bar` 不碰中断相关的 capability，
   用户态也拿不到中断"。而 xHCI 的事件环语义天然要"有新事件就叫一声"。
4. 没有设备归属仲裁表（同文 `:3185`），内核驱动与用户态同时摸同一 BAR 的竞态是**真实存在**的
   （`pci64.h:11-18` 逐条列了风险）。
5. ring3 唯一现成能力：`pci_map_bar`（`syscall64.h:224`，号 48）已能把 xHCI BAR0 映射进进程，
   `/bin/drvdemo` 已经真的读过 PORTSC 与内核枚举结果对照（`user/svc/drvdemo.c:5-10`、`:36-38`）。

⇒ ring3 的合理位置是**协议层**（BOT/SCSI 命令构造、FAT 解析），用 `shm_map` 零拷贝传扇区；
控制器的环/门铃/中断必须 ring0。要真把 HID class 层搬 ring3，先补 §4.4 的 DMA 导出 + 中断投递两个 API。

---

## 5. 分步落地清单（每步独立可验收）

> 因为现状已完整，清单是"补中断 / 补 hub / 换 class 层 / 开写"，不是"从零写驱动"。

### Step 1 — 给 xHCI 加 pin-based 中断（最小可独立验收）
- 改：`kernel/xhci64.cpp`（读 `0x3C`；`USBCMD |= INTE`；`IMAN |= IE`；新 `xhci64_irq64()` 判 EINT + **写 1 清** +
  drain 事件环）、`kernel/x86_64.cpp`（`:363-375` 改成 `irq_register64()` 注册表）、
  `kernel/apic64.cpp`（`:86-92` 加一条 route，GSI 走 `:452-459`）、`kernel/x86_64.h`（声明注册函数）
- 验收：串口出现 `[XHCI] irq line=<n> unmask ok` 与递增的 `[XHCI] irq count=<n>`；
  `tests/xhci64_test.py` 全过（尤其 `:350-352` transfer evt、`:423-425` stray=0、`:86-87` FORBIDDEN 零命中）。
- 前提：**同时把 kusb 对 xHCI 的轮询关掉**（`xhci64.cpp:1747` 的 `g_evt_ready`），否则重演 `:1600-1603` 竞态。

### Step 2 — 中断模式下回归"跨事件环回卷 + 存储混跑"
- 改：`tests/xhci64_test.py`（新增一组断言：irq 计数递增 + 30 键 6 条 `ls /` + `xhci=1` 盘符 + READ(10) CRC 一致）
- 验收：新增断言全 PASS，且 `stray transfer evt` 仍为 0。

### Step 3 — hub 支持（真需求，现在是硬缺口）
- 改：`kernel/xhci64.cpp` —— 加 class 9（Hub）描述符解析 + `GetPortStatus/ClearPortFeature/SetPortFeature`
  控制传输（现有 `xhci_control` 已够用）；`XHCI_MAX_DEV` 2 → 8（`:165`）
- 验收：`-device qemu-xhci -device usb-hub -device usb-kbd` 下串口出现
  `[XHCI] hub addr=<n> ports=<n>` 与 hub 后面的 `[XHCI] device addr=<n>`，且键盘仍能打开开始菜单。

### Step 4 — 运行期热插拔重枚举
- 改：`kernel/xhci64.cpp:553-563`（Port Status Change 分支现在只打点 + 写 1 清）→ 调 `xhci_enum_port()`；
  `:139-147` 加 Disable Slot(TRB type 13) 释放槽与 DCBAA 项
- 验收：monitor `device_add usb-kbd` → 新 `[XHCI] port N connected …`；`device_del` → `[XHCI] slot=N released`。

### Step 5 — （可选）只把 HID class 层换成 TinyUSB，控制器仍用现有 xHCI 代码
- 改：新增 `kernel/tusb_port64.cpp`（`osal_*` + `hcd_*` 包装现有 `XhciRing`）；`build64.sh` 加 `.c` 编译规则与
  `-I third_party/tinyusb`（**注意**：`build64.sh:69` 只定义了 CXXFLAGS，没有 CC 规则，需要新增）
- 验收：`tests/xhci64_test.py:369-386` 的 `hid report` 行格式与行为**完全不变**（格式不变即证 class 层替换成功）。

### Step 6 — 存储可写（当前只读）
- 改：`kernel/xhci64.cpp` 加 WRITE(10) `0x2A`（+FUA）；`xhci64.h:73` 与 `ata64.cpp` 的 USB 驱动器号写路径
- 验收：把 `tests/xhci64_test.py:561-562` 的"U 盘 CRC 不变"改成"写入后 CRC 变化且回读逐字节一致"。

### Step 7 — （只有要做 ring3 驱动才需要）补两个 API
- 改：`kernel/syscall64.h` 新增 DMA 物理页导出（或 `shm_map` + 物理地址返回）+ 中断投递；
  `proc64_devmap64`（`proc64.h:311`）已有 MMIO 映射可直接用
- 验收：`drvdemo` 能在用户态自己建一个 Transfer Ring 并收到一次 Transfer Event（串口打点）。

---

## 6. 诚实边界

**已经验证过的（QEMU）**：`-device qemu-xhci` 是完整 xHCI 模拟 —— 控制/批量/中断传输、Transfer Event、
SuperSpeed 链路（U 盘 `PORTSC speed=4`、`bMaxPacketSize0=9` 指数编码=512）、跨事件环回卷都实测通过
（`tests/xhci64_test.py:1-45` 记录了逐条证据）。USB 键盘的接法：`-device qemu-xhci -device usb-kbd`
（HS，vendor `0627` product `0001`，EP0 MPS 64）；`sendkey` 在有 USB 键盘时**只发给 USB 键盘**
（`xhci64_test.py:24-25`），这就是端到端打字证据的来源。

**一定会卡住的一步**：指望 TinyUSB/CherryUSB 提供"现成 xHCI 控制器驱动"。两个上游都没有 xhci host HCD
（TinyUSB host 侧只有 dwc2/chipidea/rp2040/EHCI 一类；CherryUSB 只有 `usb_hc_ehci.c`）。这一步不是"改内存/中断配置"，
是**重写 HCD**——而这件事 `kernel/xhci64.cpp` 已经做完 1825 行。想省事只有一条路：保留现有控制器代码，
只把 class 层（hub / 设备类）从上游借过来。

**中断路线会卡住的点**：MSI/MSI-X 在本仓库是**完全空白** —— 没有 PCI cap 链遍历（`pci64.cpp:6`）、
没有向量分配、IDT 只有 32..47 有桩（`x86_64.cpp:116-124`、`isr_stubs64.asm:105-106`）、
`g_routes[]` 只有 4 条（`apic64.cpp:86-92`）。走 MSI 必须先补"cap 链 + 动态向量 + 桩/IDT 扩容"三件事，缺一不可。
走 pin-based INTx 则不需要新桩（IRQ10/11 → 向量 42/43 已存在）。

**没验证过的硬件事实**：USB3 Streams / Max Burst / U1-U2-U3 链路状态切换（`xhci64.cpp:40`）；
真机 xHCI 的 scratchpad 需求（QEMU 是 0，代码按 `HCSPARAMS2` 最多备 16 页，`:1540-1552`）；
真机 PCI INTx 线路由（固件给的 `0x3C` 值本仓库从未读过）。

**容量不是风险**：15 MiB 余量 11.98 MiB（现 3,166,544 B / 上限 15,728,640 B），现有完整 xHCI 驱动只占 38 KB；
真正的容量风险是 `.bss` 撞 `0x04000000` 载荷窗（当前余量 19.3 MB），以及**开 debug printf 会直接链接失败**。

