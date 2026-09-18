# VimtuOS · Vimtu64

**一个从引导扇区到 ring3 应用全部手写的纯 64 位 x86_64 操作系统。**

没有 Linux 内核、没有现成内核框架、没有 libc —— 长模式引导链、内存管理、**调度器**、**文件系统**、
中断与 **APIC/ACPI**、图形与窗口系统、安装程序，以及 **ring3 用户态与两套系统调用入口**，
全部是本工程自己的 C++ / 汇编代码。
它自带一张"三合一"安装盘（BIOS 光盘 / U 盘 / UEFI），安装界面与步骤照 Windows 10 做（去掉了输入产品密钥那一步）。

> 版本 **0.1.0** · 目标平台 x86_64（长模式）· 许可 **GPL-3.0**（[LICENSE](LICENSE)）· 仓库 <https://github.com/Fanqi89/VimtuOS>
> 状态（权威判据：`python tests/status_report.py`）：**完成 27 / 部分 0 / 未做 1（共 28 项能力）**，
> 唯一未做项是 **Rust 参与实现（可选要求，本机未装 Rust 工具链）**。
> 全量验收（`--full` + 5 个扩展脚本）：**20 个脚本 / 540 条断言全部 PASS**（2026-09-19 实测）。

---

## 一、它能做什么

| 能力 | 说明 |
|---|---|
| **四种引导方式** | BIOS 光盘（El Torito）、U 盘 / 硬盘（hybrid MBR）、裸盘（MBR→loader→ATA）、**UEFI 自动引导**（自研 PE 桩 + 平铺长模式引导器）。QEMU 与 VMware、BIOS 与 UEFI 四种组合全部实测通过 |
| **现代磁盘结构** | 自写 GPT（工具链的 xorriso 不生 GPT）与 MBR 双兼容；FAT16 ESP 也是自写的（这台机器没有 mkfs.fat/mtools） |
| **Win10 同款安装程序** | 语言 → 现在安装 → 许可条款 → 安装类型 → **磁盘与分区（新建/删除/格式化，真实写盘）** → 复制与真实百分比进度 → 完成并**自动重启**；**无产品密钥步骤** |
| **装完就是一台独立系统** | 开机直接进桌面：桌面图标、任务栏、开始菜单（10 项）、窗口拖拽/缩放/最大化/最小化、脏矩形重绘 |
| **调度器（多任务）** | `task64.cpp`：16 个任务槽、每任务 16KB 内核栈、8ms 时间片轮转、IRQ0 抢占、睡眠/退出/回收、`task_kill64`；启动即 `[TASK64] scheduler up tasks=N`、`kheart` 心跳持续增长；任务管理器"进程"页与终端 `ps/kill` 读的是**真实任务表** |
| **真实文件系统 VimtuFS2** | `vfs64.cpp`：超级块（magic `VIMTUFS2` + CRC32）、空闲位图、64B inode、直接块×4 + 一级间接块 → 单文件 ≤67584B；format / mount / ls / read / write / mkdir / unlink / stat / dump，分区格式化路径已改调它 |
| **设置持久化 store** | `store64.cpp`：A/B 双槽（VimtuFS2 里的 `/store.a`、`/store.b`）+ 世代号 + 头部/payload 双 CRC32 + "先数据后头部"的提交点；实测跨重启保留（第二遍启动 `keys=1` 里仍有 `theme=dark`）；终端 `store dump\|get\|set\|flush` |
| **ring3 用户态** | `usermode64.cpp`：GDT 8 项（内核 0x08/0x10、用户 0x23/0x2B、TSS 0x30）；用户窗口在 4GiB（代码页 R/X、栈页 R/W/NX）；TSS.rsp0 随任务切换维护 |
| **两套系统调用入口** | `int 0x80`（自有 ABI：rax/rdi/rsi/rdx）与 **`syscall` 指令**（MSR EFER.SCE / STAR=0x1B<<48\|0x08<<32 / LSTAR / FMASK，Linux x86_64 号段）双入口并存，帧标记分流；用户指针一律先过 `user64_range_ok64` 范围校验 |
| **两种可安装应用格式** | 自有 **VAP64**（32B 头 + 名字 + 代码段 CRC32，`tools/make_vap.py` 打包）+ **ELF64**（PT_LOAD 段映射、按 p_flags 设页权限、SysV 初始栈/auxv）；启动时把内嵌的 `/hello.vap`、`/hello.elf` 装进 VimtuFS2，终端 `run` 按文件头魔数自动分派、`elfrun` 强制走 ELF64 |
| **硬件详情与固件表** | `hwinfo64.cpp`（CPUID vendor/brand/型号步进/特征/hypervisor + PCI 只读枚举 0xCF8/0xCFC + 磁盘槽）、`acpi64.cpp`（RSDP → RSDT/XSDT → FADT/MADT/HPET/MCFG；UEFI 经 EFI 配置表从固定槽 0x7800 传 RSDP，BIOS 走 legacy 扫描） |
| **运行期显示层** | `edid64.cpp`：解析引导期落在 0x7600 的第一块 128B EDID（厂商/型号/尺寸/首选时序 → 刷新率），实测 `[EDID64] preferred 1280x800 refresh=75.0Hz`，设置页与任务管理器读它 |
| **APIC 接管中断** | `apic64.cpp`：LAPIC + IOAPIC 重定向 IRQ0/1/12/14，8259 全掩码，EOI 走 LAPIC 0xB0；拿不到 ACPI 时回退 8259 PIC 并打印原因；x2APIC 未适配 |
| **SMP（启动 AP）** | `smp64.cpp` + `ap_trampoline64.asm`：跳板 234B @ 物理 0x8000，INIT-SIPI-SIPI + 500ms 有界超时，AP 自己打 `[SMP] ap id=1 online`；`-smp 4` 可上 4 核，1 核 / 无 ACPI 时优雅降级。**注意：AP 起来后只是 `cli; hlt` 停着，没有多核调度** |
| **网络（e1000 + ARP/ICMP）** | `e1000_64.cpp`（PCI 扫描 + MMIO 复位 + 32×16B TX/RX 描述符环 + 轮询收发）+ `net64.cpp`（以太网/IPv4/ARP/ICMP，静态 10.0.2.15）；实测 ARP 与 ICMP 到网关成功，终端 `ping 10.0.2.2` |
| **USB 主机 + HID 键盘** | `usb64.cpp`：UHCI + 控制/中断传输 + HID 引导键盘，按键经 `kbd_inject_scancode()` 注入 PS/2 **同一条队列** —— 桌面零改动即可响应（实测 USB 键盘按键真的打开了终端） |
| **ATA 中断** | `ata64.cpp` 接上 IRQ14（中断唤醒替代 PIO 轮询），超时 / 从通道 / IF=0 自动回退轮询并只告警一次；实测 `irq14 selftest reads=2 irqs=1 polled=0` |
| **桌面外壳与自带应用** | 窗口 z 序 / 拖拽缩放 / 任务栏 / 开始菜单 / 脏矩形 + **扫雷、计算器、终端、设置、任务管理器、我的电脑、系统监视器、关于** |
| **内核自检** | 启动即跑内存、图形、调度器、VFS、store、syscall、ring3、ELF64/VAP64、APIC/SMP、USB、EDID 等自检，结果打到串口供自动验收断言 |

## 二、它**不能**做什么（诚实边界，重要）

| 边界 | 现状 |
|---|---|
| ⚠️ **用户态没有独立地址空间** | 所有用户程序共用 4GiB..4GiB+1MiB 的**同一个窗口**；没有 fork / execve / clone，进程之间没有隔离（mmap/brk/mprotect/munmap 都是在这一个窗口里做 bump 分配） |
| ⚠️ **ELF64 只验证过自有静态程序** | 用 `ld.lld -static -nostdlib` 链接的自己的 ELF64 能 load → ring3 → `syscall` → exit；**glibc / 发行版二进制没有验证过**（缺 execve/fork/clone、vDSO、TLS(FS.base) 真生效、信号投递、futex、动态链接与重定位） |
| ⚠️ **VFS 是单层路径** | VimtuFS2 只支持 `/name`（无目录树、无 `.`/`..`、无删除目录）；文件名 ≤27B；最多 256 个 inode；单文件 ≤67584B。**终端里的 `ls/cat/write/touch/rm` 仍是内核内最小 ramfs（16 文件 × 512B，RAM only），没接 VimtuFS2**；真文件系统目前由应用安装/加载与 store 使用 |
| ⚠️ **设置页 UI 还没接 store** | `store64` 本体可用（终端 `store dump\|get\|set\|flush` 真落盘、跨重启保留），但设置页/启动项**没接线**，页面里如实写着"本页设置不落盘" |
| ⚠️ **没有 TCP/IP / DHCP / DNS** | 网络只有 IPv4 + ARP + ICMP echo（e1000 轮询收发，无中断收包）；UDP/TCP、路由、DHCP、DNS 都没有；只适配 e1000，VMware 的 vmxnet3 未适配 |
| ⚠️ **USB 只有 UHCI + HID 引导键盘** | 没有 EHCI(USB 2.0) / xHCI(USB 3.x)，没有 USB 鼠标、U 盘、集线器；只认直接插在根端口上的键盘；不接中断（由 `kusb` 线程轮询） |
| ⚠️ **AP 只是停着** | SMP 能启动 AP 并让它报在线，但**没有多核调度**（调度器仍单核、IRQ0 只在 BSP）、没有 IPI、没有 per-CPU 数据/GDT/TSS，AP 自己的 LAPIC/中断不参与 |
| ⚠️ **ACPI/APIC 覆盖有限** | 只解析 RSDP/RSDT/XSDT/FADT/MADT/HPET/MCFG；关机仍走 ACPI 端口 0x604 + 8042 回退链；x2APIC 未适配；固件页表没映射 LAPIC/IOAPIC 的机器会留在 PIC |
| ⚠️ **只在虚拟机验证** | QEMU + VMware Workstation 双验证（BIOS 与 UEFI 都跑），**未在真机裸机验证**；UEFI 路径未做签名，测试时 `secureBoot=FALSE` |
| ⚠️ **磁盘仍是 PIO 搬运** | 有了 IRQ14 中断唤醒，但没有 DMA/Bus-Master，读写期间 CPU 仍要逐扇区搬 |
| ⚠️ **没有声音** | 无音频驱动 |
| ⚠️ **Rust 未参与** | 全部是 C/C++（clang）+ 汇编；Rust 是可选要求，本机未装工具链 |

**一句话定位**：*现在的 VimtuOS 是「有现代引导 + 现代安装体验 + 64 位桌面 + 真调度器 + 真文件系统 +
ring3 用户态」的自研单体内核；它能装进硬盘、跑起自己的桌面，也能把自己写的 VAP64 / ELF64 程序
装进自己的文件系统并在 ring3 里运行 —— 但它还不能跑 glibc 级的第三方程序，也没有独立地址空间。*

## 三、技术亮点（值得一看的地方）

1. **纯 64 位，无 32 位业务代码**：16 位实模式只出现在 BIOS 物理必经阶段（读盘、问 E820/VBE）；UEFI 路径 100% 是长模式代码。
2. **内核运行在高半区**：`链接基址 0xFFFFFFFF80100000`，引导层建 `VA = 0xFFFFFFFF80000000 + PA` 的高半区线性直映，同时保留 0..4GB 恒等映射；**低地址与 4GiB 以上的用户窗口都留给用户态**。
3. **调度器用中断帧做上下文**：任务上下文就是 `pt_regs64`（0xD0 字节）本身，抢占发生在 IRQ0 里（`pic_eoi(0)` → `schedule64(r)` → 抬栈 `iretq`），不需要额外的上下文切换汇编；每任务独立 16KB 内核栈，初始帧手工构造后跳统一蹦床。
4. **两套系统调用入口并存**：自有 `int 0x80`（rax/rdi/rsi/rdx）与 Linux 号段的 `syscall` 指令（STAR/LSTAR/FMASK + 专用 16KB syscall 内核栈，和 TSS.rsp0 的耦合被显式切断）；号段语义互不干扰，帧标记分流。
5. **两种应用格式、两条完整链路**：自有 VAP64（32B 头 + 名字 + CRC32，`tools/make_vap.py` 打包）走 `int 0x80`；ELF64 走 `syscall` 指令 —— 从盘上读出来（VimtuFS2）到 ring3 跑完再回收，全程有串口证据。
6. **自研 UEFI 引导（不用 gnu-efi）**：自造 PE32+ 桩（2.5KB）+ 平铺长模式引导器（36KB，链接到 72MB，绕过 EDK2 的 PE 校验）；**RSDP 经固定槽 0x7800 从固件配置表传给内核**。
7. **一切自写工具链**：没有 mkfs.fat、没有 mtools —— FAT16 卷、ISO9660 解析与打包、GPT、LBA 回填、PE 摊平、字体子集化（3 套 TTF）、图标生成、VAP64 打包，全是本仓库的 Python 脚本。
8. **APIC / ACPI / SMP 都带"宁可不启用也不变砖"的回退**：拿不到 ACPI 就留 8259、启动 AP 全程有界超时、每个自检失败都整体回滚，降级路径都有串口证据。
9. **脏矩形重绘 + 光标不 save-under**：鼠标移动只重绘约 **0.03% 的屏幕像素**（自动验收会断言这个数字），这是从"整屏重绘导致鼠标包读慢、位移被聚合放大"的实测故障里换来的设计。
10. **每次改动都跑自动验收**：**20 个验收脚本 / 540 条断言**，全部是"字节级 + 像素级 + 串口日志"三合一，而不是"看起来能跑"。

## 四、系统架构

```
① 引导层（boot/，12 文件 / 3,575 行）
   BIOS 光盘   El Torito → cdiso.asm（引导桩 + 介质描述符 @0x0F00）→ loader64.asm
   U 盘/裸盘   hybrid_mbr.asm（自搬 0x0600）→ boot.asm（512B MBR）→ loader64.asm
   UEFI        BOOTX64.EFI（自研 PE 桩）→ UEFI64.BIN（平铺长模式引导器）
   loader64 / UEFI64 负责：E820 / VBE(EDID→0x7600) / RSDP(→0x7800) → 读内核到物理 0x100000
                            → 建页表（恒等 + 高半区直映）→ 进长模式 → 跳内核入口

② 内核层（kernel/，69 文件 / 27,599 行）
   入口与基础设施
     kernel64.cpp      入口 / 自检 / 安装程序或系统两条启动路径 / 各子系统初始化顺序
     x86_64.cpp        GDT(8 项含用户段+TSS) / IDT / TSS / PIC / PIT(250Hz) / RTC
     entry64 / isr_stubs64 / isr_*64 / switch64.asm   长模式入口、中断帧、任务切换原语
     mem64.cpp         物理页池（E820 决定）+ 48MB 内核堆 + 归属记账 + 启动自检
     fb.cpp font.cpp   帧缓冲（VBE/LFB、缩放、裁剪、脏区提交）+ TrueType 渲染
     input.cpp         PS/2 键鼠（IRQ1/IRQ12）+ 串口按键通道 + kbd_inject_scancode()
   执行与用户态
     task64.cpp        调度器：16 任务槽 / 16KB 内核栈 / 8ms 时间片 / IRQ0 抢占 / 睡眠退出回收 / kstress
     usermode64.cpp    ring3：用户窗口(4GiB..4GiB+1MiB) / 页映射原语 / 进出 ring3 / TSS.rsp0
     syscall64.cpp + syscall_entry64.asm    int 0x80 自有 ABI + syscall 指令 Linux 号段（MSR 配置）
   存储与持久化
     ata64.cpp         ATA PIO + ATAPI(PACKET) + IRQ14 中断唤醒（超时回退轮询）
     part64.cpp        分区表（MBR/GPT）/ 新建 / 删除 / 格式化（格式化写 VimtuFS2）/ 安装引擎
     vfs64.cpp         VimtuFS2：超级块+CRC32 / 空闲位图 / 64B inode / 读写 mkdir unlink stat dump
     store64.cpp       设置持久化：/store.a、/store.b 双槽 + 世代号 + 双 CRC32（裸盘槽仅降级+WARN）
     app64.cpp         VAP64 安装器 / 校验 / 启动器 + 按魔数自动分派
     elf64.cpp         ELF64 加载器：PT_LOAD 映射、p_flags 权限、SysV 初始栈/auxv、装载回收
   硬件
     hwinfo64.cpp      CPUID（vendor/brand/型号步进/特征/hypervisor）+ PCI 只读枚举 + 磁盘槽
     acpi64.cpp        RSDP → RSDT/XSDT → FADT/MADT/HPET/MCFG（UEFI 走 0x7800，BIOS 走 legacy 扫描）
     apic64.cpp        LAPIC + IOAPIC 重定向 IRQ0/1/12/14、8259 全掩码、EOI 走 LAPIC（失败回退 PIC）
     smp64.cpp + ap_trampoline64.asm   INIT-SIPI-SIPI 启动 AP（跳板 234B @0x8000，有界超时）
     edid64.cpp        运行期显示层：EDID 解析（厂商/型号/尺寸/首选时序→刷新率）
     e1000_64.cpp + net64.cpp          e1000 轮询驱动 + 以太网/IPv4/ARP/ICMP（静态 10.0.2.15）
     usb64.cpp         UHCI + HID 引导键盘（按键注入 PS/2 同一条队列）
   安装
     setup64.cpp       Win10 同款图形安装向导

③ 桌面层（gui64.cpp + 应用）
   窗口 z 序 / 拖拽缩放 / 最大最小化 / 任务栏 / 开始菜单 / 脏矩形消息循环
   扫雷 · 计算器 · 终端 · 设置 · 任务管理器（进程页 = 真任务表）· 我的电脑 · 系统监视器 · 关于

④ 应用层（ring3，用户窗口 4GiB..4GiB+1MiB）
   VAP64   32B 头 + 名字 + 代码段 CRC32，程序用 int 0x80（自有 ABI）
   ELF64   静态 ET_EXEC/ET_DYN + PT_LOAD + SysV 初始栈，程序用 syscall 指令（Linux 号段）
   安装：启动时把内嵌的 hello.vap / hello.elf 装进 VimtuFS2 的 /hello.vap、/hello.elf（幂等）
   启动：终端 run <名字|/路径>（按文件头魔数分派）/ elfrun <路径>（强制 ELF64）
   详见 docs/应用层与系统调用说明.md

⑤ 构建与验证
   build64.sh     唯一构建脚本（clang/lld/nasm/xorriso/Python）
   tools/*.py     自写打包/诊断：make_iso64 / make_esp / make_flat / make_vap / pe_info / fat_check
   tests/*.py     20 个验收脚本（540 断言）：串口断言 + screendump 像素断言 + 目标盘字节断言
```

## 五、快速开始

### 1) 构建（Windows + MSYS2）

```bash
pacman -S --noconfirm --needed mingw-w64-x86_64-clang mingw-w64-x86_64-lld \
                               mingw-w64-x86_64-compiler-rt nasm xorriso
# 还需要 Python 3 + Pillow（字体子集化 / 图标生成）

# ① 先准备字体：仓库**不含**第三方字体（版权原因），需要自备这三个文件
#    Fonts/bahnschrift.ttf、Fonts/ChaparralPro-Regular.ttf、Fonts/simhei.ttf
#    或者把 _otf2ttf.py / _subset_fonts.py / _subsetsimhei.py 改成开源字体（Noto Sans / 思源黑体）

# ② 构建：产出 vimtu64-64.iso（三合一安装盘）
bash build64.sh
```

产物：

| 文件 | 大小 | 说明 |
|---|---|---|
| `vimtu64-64.iso` | 14.9 MB | **三合一安装盘**：BIOS 光盘 + 可写 U 盘 + UEFI |
| `vimtu64-64.img` | 8.3 MB | 安装介质裸盘（把一个 8MB 镜像直接当硬盘用） |
| `build64/kernel64.bin` | 1.66 MB | 安装程序内核 |
| `build64/kernel64_os.bin` | 2.01 MB | 装进硬盘的系统内核（调度器/VFS/store/ring3/网络/USB 都在这里） |
| `build64/BOOTX64.EFI` / `UEFI64.BIN` | 2.5 KB / 36 KB | UEFI 两段式引导 |
| `build64/esp.img` | 6 MB | 手写 FAT16 ESP |

### 2) 五分钟演示（推荐顺序，都是已实测的功能）

```bash
# ① 安装：ISO 当光盘，另给一块空盘当目标盘（网络与 USB 键盘可选但值得加）
qemu-system-x86_64 -cdrom vimtu64-64.iso -drive format=raw,file=target.img \
                   -m 512 -vga std -smp 4 \
                   -netdev user,id=n0 -device e1000,netdev=n0 -usb -device usb-kbd

# ② UEFI 自动引导（OVMF）
qemu-system-x86_64 -drive if=pflash,format=raw,readonly=on,file=<OVMF_CODE.fd> \
                   -drive if=pflash,format=raw,file=<2MB 全零 vars.fd> \
                   -cdrom vimtu64-64.iso -drive format=raw,file=target.img -m 512 -vga std
```

演示顺序（装完后）：

1. **安装向导每步** → 选盘 → 新建分区（真写盘）→ 安装进度百分比 → 装完**自动重启**；
2. 进桌面（不再需要安装介质），任务栏 / 开始菜单开应用；
3. **任务管理器 → 进程页**：看到的是**真任务表**（idle + kheart/kwork/ksum/kusb 内核线程），不再只有内核自己；
4. 终端里依次敲：
   * `ps`（真任务表 + 累计切换次数）、`kill <id>`（真的把任务标死并回收）
   * `run hello.vap`（VAP64：`int 0x80` 自有 ABI）→ 打印 `hello from installed app (VAP64 on VimtuFS2)`
   * `elfrun hello.elf`（ELF64 + `syscall` 指令）→ 打印 `hello from ELF64 (syscall insn)` / `pid=` / `open /hello.elf fd=` / `read=`
   * `ping 10.0.2.2`（e1000 + ARP + ICMP，QEMU 用户网络）
   * `store set theme dark` → `store get theme` → `store flush`（落盘到 VimtuFS2 的 `/store.a|/store.b`）
5. **重启**（终端 `reboot` 或关掉 QEMU 再开）：`store dump` 里 `theme=dark` 还在 —— 设置真的跨重启存下来了。

> 说明：设置**页面**里的开关还没接 store（页面里如实写着），跨重启演示请用终端的 `store` 命令。

VMware 里也一样（**新建虚拟机时 `guestOS` 必须选 64 位：`other-64`**，否则 CPUID 长模式位被屏蔽）。

### 3) 自动验收（每次改动都该跑）

```bash
python tests/status_report.py          # 先看状态：哪些完成/部分/未做（带证据，秒级）
python tests/status_report.py --full   # 再真跑 15 个验收脚本（约 6-8 分钟）

# 另有 5 个专项脚本（--full 列表之外，建议一起跑）：
python tests/user64_test.py            # ring3 / GDT / 用户页权限
python tests/display64_test.py         # EDID 显示层（设置页里的刷新率）
python tests/app64_test.py             # VAP64：安装 + 校验 + ring3 运行
python tests/store64_test.py           # store：双槽 + CRC + 跨重启持久化
python tests/elf64_test.py             # ELF64 加载器 + syscall 指令号段

python tests/screenshot64.py           # 抓一张桌面真机截图（PNG）
```

## 六、工程质量（这个项目最值得说的部分）

* **20 个验收脚本 / 540 条断言，全部 PASS**（2026-09-19 实测）。`--full` 覆盖 15 个脚本 / **368 条断言**：

  | 脚本 | 断言 | 脚本 | 断言 |
  |---|---|---|---|
  | boot64_assert.py | 16 | desktop64_test.py | 30 |
  | mouse_parse_test.py | 12 | net64_test.py | 18 |
  | install_flow_test.py | 19 | apic64_test.py | 47 |
  | partition_ops_test.py | 18 | smp64_test.py | 52 |
  | screen64_probe.py | 5 | sched_stress_test.py | 18 |
  | iso64_install_test.py | 18 | usb64_test.py | 51 |
  | iso64_usb_test.py | 22 | vmware_install_test.py | 21 |
  | uefi64_install_test.py | 21 | **小计** | **368** |

  另有 5 个专项脚本 / **172 条断言**：`elf64_test` 56、`store64_test` 46、`app64_test` 31、
  `display64_test` 22、`user64_test` 17。
* **三类证据**：
  * **串口级**：每步都打标记（`[LM64]` / `[G64]` / `[SETUP]` / `[PART]` / `[TASK64]` / `[VFS64]` /
    `[STORE64]` / `[SYSCALL]` / `[USER64]` / `[APP64]` / `[ELF64]` / `[APIC]` / `[SMP]` / `[NET64]` /
    `[USB64]` / `[EDID64]` / `[HW64]` / `[ACPI64]` / `[UI]`），验收读日志判定；
  * **像素级**：QEMU `screendump` 抓帧，按颜色占比/位置断言（安装界面、桌面、任务栏、窗口标题栏、脏矩形范围、设置页里的刷新率文字）；
  * **字节级**：目标盘的 MBR/GPT/分区项/loader/内核逐字节比对；VimtuFS2 卷按扇区偏移检查。
* **每个新子系统都带降级证据**：无 ACPI → 留 PIC；`-smp 1` → 单核；无 USB 主控 → `not found`；
  无键盘 → `selftest skipped`；IRQ14 超时 → 回退轮询；store 无卷 → 裸盘降级 + WARN。**一律不变砖**。
* **双模拟器 + 双固件**：QEMU（含 TCG，本机无 VT-x）与 VMware Workstation；BIOS 与 UEFI。
* **调试方法论留在代码里**：每个踩过的坑都写成注释（例：`out` 的端口在 DX 而 DX 是 RDX 低 16 位、
  串口打点要等 TEMT 而不是 THRE、NASM 的 64 位绝对跳转会截断成 rel32、EDK2 的页表是只读的、
  syscall 帧与中断帧共用栈顶会互相覆盖……）。
* **诊断工具随仓库发布**：`tools/pe_info.py`（PE 体检）、`tools/fat_check.py`（FAT 卷按规范体检）。

## 七、开发历程（里程碑）

| 阶段 | 内容 | 状态 |
|---|---|---|
| M0 | BIOS → loader → 长模式 → 64 位 C++ + BootInfo/E820/VBE | ✅ |
| M1 | IDT/TSS/PIC/PIT/RTC、帧缓冲、TrueType、PS/2 键鼠 | ✅ |
| M2 | Win10 同款安装程序 + 真实分区操作 + 进度 + 自动重启 | ✅ |
| M3 | 光盘（ATAPI）、U 盘 hybrid、GPT、手写 FAT16 ESP | ✅ |
| M4 | **32 位工程整体退役**，纯 64 位单构建线 | ✅ |
| M5 | 桌面栈移植：内存管理 + 外壳 + 5 个应用 | ✅ |
| M6 | **UEFI 自动引导打通**（PE 基址 5GB / ESP 名为 FAT16 实为 FAT12 / 进内核 CS 未换） | ✅ |
| M7 | **内核搬高半区**（为应用层让出低地址），四条引导路径全回归 | ✅ |
| M8 | ATA 中断（IRQ14）、运行期显示层（EDID）、硬件详情（CPU/PCI/磁盘） | ✅ |
| M9 | **调度器**（task64：16 槽 / 16KB 栈 / 8ms 时间片 / IRQ0 抢占 / kill / kstress） | ✅ |
| M10 | **真实文件系统 VimtuFS2 + store 持久化**（双槽 + 世代号 + 双 CRC32，跨重启保留） | ✅ |
| M11 | **ring3 用户态 + 两套 syscall 入口**（`int 0x80` 自有 ABI / `syscall` 指令 Linux 号段） | ✅ |
| M12 | **VAP64 应用格式与加载器 + ELF64 加载器**（自有静态程序可从盘上装、可在 ring3 跑） | ✅ |
| M13 | **ACPI 解析 + APIC 接管 + SMP 启动 AP**（均有优雅降级） | ✅ |
| M14 | **网络（e1000 + ARP/ICMP）与 USB 主机（UHCI + HID 键盘）** | ✅ |

## 八、路线图（未完成的部分）

按依赖顺序（这也是欢迎贡献的方向）：

| 顺序 | 目标 | 现状 | 做完能带来什么 |
|---|---|---|---|
| ① | **独立地址空间 + fork/execve** | 所有用户程序共用一个 4GiB 窗口 | 真进程隔离；`syscall` 号段的 execve/fork/clone 才有意义 |
| ② | **glibc 级兼容** | 只验证过自有静态 ELF64 | TLS(FS.base) 真生效、信号投递、vDSO、futex、动态链接与重定位；毕业考试是"静态 busybox 起 shell" |
| ③ | **VFS 补齐** | 单层路径、无目录树/无 rmdir、单文件 ≤67584B、终端文件命令还是 ramfs | 多级目录 + 目录删除 + 终端接真文件系统 |
| ④ | **TCP/IP** | 只有 IPv4/ARP/ICMP、静态地址、无中断收包 | DHCP/DNS/UDP/TCP；vmware vmxnet3 适配 |
| ⑤ | **EHCI / xHCI / USB 存储 / 集线器 / 鼠标** | 只有 UHCI + 根端口 HID 引导键盘 | U 盘、鼠标、带 hub 的真机 |
| ⑥ | **多核调度** | AP 起来后只是 `cli; hlt`；无 IPI、无 per-CPU 数据 | 真 SMP：AP 参与调度与中断；x2APIC |
| ⑦ | **设置页接 store** | store 本体可用，设置页/启动项未接线 | 分辨率/缩放/窗口位置能真的保存 |
| ⑧ | **真机验证** | 只在 QEMU + VMware 验证过 | 真机驱动覆盖（ACPI 变体、EHCI/xHCI、网卡）与稳定性 |
| — | **Rust 参与实现** | 未用（本机未装工具链） | 可选项：选一个模块（如调度器/文件系统）用 Rust 写即可证明这条路通 |

## 九、目录结构

```
build64.sh              唯一构建脚本（产出 ISO / IMG / 两个内核）
build_uefi.sh           UEFI 两段式引导的构建（被 build64.sh 调用）
boot/                   引导层（16 位实模式 + 长模式入口，物理必经）  12 文件 / 3,575 行
  boot.asm              512B MBR
  loader64.asm          实模式准备 + ATA 读内核 + 进长模式 + 跳高半区内核
  loader64_atapi.inc    ATAPI(PACKET) 读光盘
  cdiso.asm             光盘引导桩（与 U 盘硬盘分支共用）
  hybrid_mbr.asm        写进 ISO 第 0 扇区的 hybrid MBR
  efi/                  UEFI：stub.c(PE 桩) + uefi64.c(平铺引导器) + jump64.asm + efi.h
kernel/                 64 位内核 + 桌面 + 应用  69 文件 / 27,599 行
  kernel64.cpp          入口 / 自检 / 两条启动路径 / 子系统初始化顺序
  task64.*              调度器（任务表 / 时间片 / 抢占 / 回收 / kstress）
  vfs64.*               VimtuFS2 文件系统
  store64.*             设置持久化（/store.a、/store.b 双槽）
  usermode64.*          ring3 用户态（用户窗口 / 页映射原语 / TSS.rsp0）
  syscall64.* + syscall_entry64.asm   两套系统调用入口 + Linux 号段表
  app64.*               自有应用格式 VAP64（安装器 / 校验 / 加载器 / 魔数分派）
  elf64.*               ELF64 加载器（PT_LOAD / p_flags 权限 / SysV 初始栈）
  hwinfo64.* acpi64.* apic64.* smp64.* edid64.*      硬件与固件
  e1000_64.* net64.*    网卡驱动 + 以太网/IPv4/ARP/ICMP
  usb64.*               UHCI + HID 引导键盘
  ap_trampoline64.asm   AP 跳板（234B，按物理 0x8000 汇编）
  gui64.* calc64.cpp mines64.cpp terminal64.cpp settings64.cpp taskmgr64.cpp
  mem64.cpp x86_64.cpp fb.cpp font.cpp input.cpp ata64.cpp part64.cpp setup64.cpp ...
tools/                  自写打包/诊断工具（Python，6 脚本 / 1,092 行；根目录另有 6 个 _*.py 资源脚本）
  make_vap.py           VAP64 应用打包器（与 kernel/app64.h 逐字段一致）
tests/                  验收脚本（24 个 .py，其中 20 个是断言脚本 / 540 断言）
user/                   ring3 示例程序（4 文件 / 459 行）
  hello64.asm           VAP64 示例（int 0x80 自有 ABI）
  hello_elf64.asm/.ld   ELF64 示例（syscall 指令 Linux 号段 + 用户窗口链接脚本）
docs/                   架构、安装、UEFI、桌面栈、应用层与系统调用、状态总览等中文文档
docs/screenshots/       自动抓取的桌面截图（desktop64.png）
```

## 十、开源与发布状态

**本仓库已开源：<https://github.com/Fanqi89/VimtuOS>** —— 只发**源码**（181 个文件 / 约 1.9 MB），许可 **GPL-3.0**。

**没有进仓的东西**（都在 `.gitignore` 里，按体积排序）：

| 未发布 | 体积 | 原因 |
|---|---|---|
| `tools/i686-elf/` | 849 MB | 打包的第三方交叉编译器（README 里给安装方式即可） |
| `legacy32/` | 770 MB | 已退役的 32 位工程，建议将来单独立仓作"移植参考" |
| `Fonts/` | 684 MB | 第三方字体，**版权原因不能公开** |
| `build64/` + `build/` | 80 MB | 构建产物 |
| `vimtu64-64.iso` / `.img` | 22 MB | 构建产物（且 ISO 内嵌商业字体子集，见下） |
| `target-*.img` / `*.log` | ~64 MB | 验收测试残留 |

**发布二进制（ISO/IMG）之前的硬约束**：安装镜像里嵌着 `bahnschrift` / `ChaparralPro` / `simhei` 的字体子集，
**不能公开分发**。要先做两步：① 把 `_otf2ttf.py` / `_subset_fonts.py` / `_subsetsimhei.py` 指向开源字体
（Noto Sans / 思源黑体，SIL OFL）；② 重建并重跑一次验收（`py -3 tests/status_report.py --full`）——
之后才可以把 ISO 作为 Release 附件发出去。

**版本与发布**：标签 `v0.1.0-beta.1`，Release：<https://github.com/Fanqi89/VimtuOS/releases/tag/v0.1.0-beta.1>
（预发布 / 源码版，无二进制附件）。

```bash
# 日常：改完代码这样提交推送（★ 不要用网页拖拽上传 —— 那会绕过 .gitignore）
git status --short          # 先看会提交什么
git add -A
git commit -m "说明这次改了什么"
git push
```

⚠️ 踩坑：`.gitignore` **不支持行尾注释** —— 写成 `tools/i686-elf/   # 849 MB` 时，整行（含 `#` 与后面的字）
会被当成一个匹配不到的图案，看起来排除了其实一个都没排除。注释一律单独占一行（仓库里的 `.gitignore` 就是这么写的）。

## 十一、许可（GPL-3.0）

本项目以 **GNU General Public License v3.0** 发布（全文见 [LICENSE](LICENSE)）：你可以自由使用、修改、分发，
但衍生的分发物必须以同样的许可开源，且不提供任何担保。安装程序的许可页写的就是这一条。

## 十二、免责声明

本项目是**学习与实验性质**的自研操作系统：不保证稳定性与数据安全，**不要**在存有重要数据的机器上安装。
VMware/QEMU 等虚拟机是推荐的试用方式。安装程序会**真实写入**你所选磁盘的分区表与扇区。

---

## English summary (for a quick pitch)

**VimtuOS (Vimtu64)** is a hand-written, **pure 64-bit x86_64 long-mode operating system** — from the
boot sector to ring3 user programs, with no Linux kernel, no kernel framework and no libc.
It boots four ways (BIOS CD via El Torito, USB/hybrid MBR, bare disk, and **UEFI** via a self-made PE stub
plus a flat long-mode loader), ships a **Windows-10-style installer** (create/delete/format partitions for
real, real progress, auto-reboot — minus the product-key step), and once installed it boots straight into a
desktop shell with its own apps: **Minesweeper, Calculator, Terminal, Settings and a Windows-10-style Task
Manager**.

On top of that it now has a **preemptive scheduler** (16 task slots, 16KB kernel stacks, 8ms slices, IRQ0),
a **real file system (VimtuFS2)** with CRC-protected superblock/bitmap/64B inodes, **settings persistence**
(two slots with generation numbers and double CRC32, verified across a real reboot), **ring3 user mode with
two syscall entries** (`int 0x80` with its own ABI, and the `syscall` instruction with Linux x86_64 numbers),
a **self-defined app format (VAP64)** plus an **ELF64 loader** (both install into VimtuFS2 and run in ring3),
**ACPI parsing + LAPIC/IOAPIC takeover + SMP AP bring-up**, an **EDID runtime display layer**, an
**e1000 network stack (ARP/ICMP)** and a **UHCI USB host with HID boot keyboard**.

Honest boundaries: user programs share **one 4GiB window** — no independent address spaces, no
fork/execve; the ELF64 loader is only verified with **our own static binaries** (glibc/distro binaries are
untested: no vDSO, TLS, signals, futex or dynamic linking); VFS is **single-level** (`/name`, ≤27B names,
≤67584B files) and the terminal's own file commands still use an in-kernel ramfs; there is no TCP/IP (ARP +
ICMP only), no EHCI/xHCI/USB storage/hub, and the APs only spin in `cli; hlt` (no multi-core scheduling).
Everything was validated on **QEMU and VMware, BIOS and UEFI — not on bare metal**.

Quality-wise, every change is verified by **20 test scripts / 540 assertions** that check serial logs, screen
pixels (QEMU screendumps) and raw disk bytes. Kernel + bootloader + ring3 samples ≈ **31.6k lines** of
self-written Python tooling (ISO9660/GPT/FAT16/VAP64 packing, font subsetting, PE/FAT diagnostics).
