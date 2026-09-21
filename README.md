# VimtuOS · Vimtu64

**一个从引导扇区到 ring3 应用全部手写的纯 64 位 x86_64 操作系统。**

没有 Linux 内核、没有现成内核框架、没有 libc —— 长模式引导链、内存管理、**调度器**、**文件系统**、
中断与 **APIC/ACPI**、图形与窗口系统、安装程序，以及 **ring3 用户态与两套系统调用入口**，
全部是本工程自己的 C++ / 汇编代码。
它自带一张"三合一"安装盘（BIOS 光盘 / U 盘 / UEFI），安装界面与步骤照 Windows 10 做（去掉了输入产品密钥那一步）。

> 版本 **0.2.0-beta.5** · 目标平台 x86_64（长模式）· 许可 **GPL-3.0**（[LICENSE](LICENSE)）· 仓库 <https://github.com/Fanqi89/VimtuOS>
> 状态（权威判据：`python tests/status_report.py`）：**完成 40 / 部分 0 / 未做 1（共 41 项能力）**，
> 唯一未做项是 **Rust 参与实现（可选要求，本机未装 Rust 工具链）**。
> 全量验收（`--full` + 专项/扩展脚本）：**32 个断言脚本全部 PASS**（本批次实测；上一批断言保持全过，
> 本批次新增 **fileops64_test（文件操作：右键菜单/复制/剪切/粘贴/重命名/删除/新建文件夹/多选）94 条断言**；
> vfs64 自检新增 rename 用例、explorer64 自检新增名字/后缀/多选位图用例；
> multivol64(108)/explorer64(71)/fs_tree(84)/fs_term(32)/fd64(34)/store64(46)/
> app64(31)/elf64(56)/proc64(67)/sysstate64(100) 等既有脚本保持绿）。

---

## 一、它能做什么

| 能力 | 说明 |
| **★ 文件操作：右键菜单 / 复制 / 剪切 / 粘贴 / 重命名 / 删除 / 新建文件夹 / 多选（本批次新增）** | `explorer64.cpp` + `vfs64_rename64`：**右键菜单**（条目：打开/复制/剪切/重命名/删除/属性；空白：新建文件夹/粘贴/刷新/属性；hover 高亮、置灰、Esc/点外部关闭）；**多选**（单击、Ctrl 加选、Shift 范围选、**空白拖动框选**（浅蓝矩形，内核无 alpha 混合故用实色近似）、Ctrl+A 全选、Esc 取消）；**内核内剪贴板**（最多 8 条 `(卷槽, 完整路径)` + 复制/剪切）→ 粘贴：文件按"读整个文件再整体写"复制（单文件 ≤67584 B）、**目录递归复制**（深度 ≤4、整棵 ≤96 条，超限如实计入 `skipped`）、**跨卷 C:↔D: 真能粘贴**（源用 `vfs64_*_on64(源槽)` 读、目标按界面盘符写）；**重名策略：自动追加 `(2)`、`(3)`…**（插在扩展名之前；**不带空格** —— VimtuFS2 名字只允许 `0x21..0x7E`，空格不合法）；**剪切 = 复制成功后删源**（先全部复制成功再删，绝不半删）；**重命名**（F2 / 菜单 → 内联编辑，重名一律拒绝）；**删除**（Delete / 菜单 → **两段式确认**：状态栏提示"再按一次 Delete 确认删除（10 秒内有效）"；**非空目录明确提示"目录非空，暂不支持递归删除"**，绝不假装成功）；**属性**（小面板 + 打点：名称/类型/大小/修改日期/所在卷）；**工具栏 6 个按钮**（新建文件夹/复制/剪切/粘贴/重命名/删除，无选中或剪贴板为空时置灰）；**快捷键** Delete / F2 / Ctrl+C / Ctrl+X / Ctrl+V / Ctrl+A / Esc。打点 `[UI] explorer ctxmenu|clip|paste|rename|delete|mkdir|sel|props …`；端到端 `tests/fileops64_test.py`（94 项：像素 + 串口 + 鼠标/键盘注入 + 宿主侧解析 D: 卷字节） |
|---|---|
| **四种引导方式 + 装好的盘双固件** | BIOS 光盘（El Torito）、U 盘 / 硬盘（hybrid MBR）、裸盘（MBR→loader→ATA）、**UEFI 自动引导**（自研 PE 桩 + 平铺长模式引导器）。QEMU 与 VMware、BIOS 与 UEFI 四种组合全部实测通过；**"ISO 当 U 盘"形态在 OVMF（UEFI）与 SeaBIOS（BIOS）下也都进安装向导**（`tests/usb_boot_both_fw_test.py` 四档）；装好的盘在 UEFI 与 BIOS 下都能启动（`tests/esp_install_test.py`） |
| **现代磁盘结构（安装盘 + 目标盘都有）** | 自写 GPT（工具链的 xorriso 不生 GPT）与 MBR 双兼容；**FAT32 ESP 也是自写的**（这台机器没有 mkfs.fat/mtools）：48MB 卷、簇数 96736（真 FAT32 的硬下限是 65525 簇）、FSInfo + 备份引导扇区齐全。**安装时会往目标盘写混合 MBR + 盘尾 GPT + 48MB FAT32 ESP**（内核实现在 `kernel/fat64.{h,cpp}`，卷参数与构建期 `tools/make_esp.py` 逐条对齐），ESP 里放 `EFI/BOOT/BOOTX64.EFI` + `UEFI64.BIN` + `KERNEL64.BIN` |
| **Win10 同款安装程序** | 语言 → 现在安装 → 许可条款 → 安装类型 → **磁盘与分区（新建/删除/格式化，真实写盘）** → 复制与真实百分比进度 → 完成并**自动重启**；**无产品密钥步骤** |
| **装完就是一台独立系统** | 开机直接进桌面：桌面图标、任务栏、开始菜单（10 项）、窗口拖拽/缩放/最大化/最小化、脏矩形重绘 |
| **调度器（多任务）** | `task64.cpp`：16 个任务槽、每任务 16KB 内核栈、8ms 时间片轮转、IRQ0 抢占、睡眠/退出/回收、`task_kill64`；启动即 `[TASK64] scheduler up tasks=N`、`kheart` 心跳持续增长；任务管理器"进程"页与终端 `ps/kill` 读的是**真实任务表** |
| **真实文件系统 VimtuFS2（v3 目录树）** | `vfs64.cpp`：超级块（magic `VIMTUFS2` + CRC32）、空闲位图、**128B inode**（直接块×4 + 一级间接块 → 单文件 ≤67584B）、**多级路径**（`/dir/sub/file`，`.`/`..` 语义齐全）、inode **mtime**（RTC 打包 u32）与**类型判定**（目录/VAP64/ELF64/文本/二进制存进 inode 的 `kind` 字段）；`vfs64_format/mount/stat64/list64/opendir+readdir/closedir/mkdir64/create64/write64/read64/unlink64/rmdir64/tree_dump64` + 兼容旧 API；**v2 旧卷仍可挂载**（按单层语义工作），新格式化一律产出 v3 |
| **盘符与驱动器枚举（"此电脑"的数据来源）** | `drive64.cpp`：枚举 PATA/AHCI(SATA)/NVMe 每块盘 → 读 MBR 分区 → 识别文件系统（VimtuFS2 超级块只读校验 / FAT32 BPB 指纹）→ 盘符表：**系统分区 = `C:`**（依据：vfs64 当前真正挂载的卷，兜底 drive 0 的 MBR `0x07` 项），其余可浏览的 VimtuFS2 卷依次 **`D:`/`E:`…**（**每个可浏览卷在扫描时占一个 vfs64 卷槽**，条目带 `slot=<n>`；`drive64_activate_letter64('D')` = 把当前卷切到那个槽）；**ESP/未知文件系统不占字母但仍列出**（`skip reason=esp|no-fs`）；卷槽满（>4 个可浏览卷）时**如实拒绝**：该条目 `skip reason=voltable-full`、不占字母，绝不覆盖已挂载的卷。每项给显示名/总容量/可用（**实时**：走已挂载槽现数位图）/文件系统/是否可浏览。打点 `[DRV64] scan\|letter=\|skip\|activate\|selftest`；**★ 批次 K：FAT32 只读浏览（含 VFAT 长名）**：`kernel/fat64.cpp` 增加只读读取器（BPB 校验/簇链遍历/LFN(0x0F) 校验和与 UTF-16 拼接/FAT 日期时间），`kernel/fs64.{h,cpp}` 作为统一分派层（VimtuFS2 读写 / FAT32 只读）；FAT12/16 仍只识别不浏览（簇链项位宽不同） |
| **★ 多卷挂载 / 盘符切换（本批次新增）** | `vfs64.cpp` 从"一次只挂一个卷"改成**卷槽表**：至少 4 个槽 （`VFS64_SLOT_MAX`），每槽独立保存 drive/起始 LBA/位图/inode/数据区几何；`vfs64_mount_slot64(slot,…)` / `vfs64_activate_slot64(slot)` / `vfs64_current_slot64()` / `vfs64_slot_info64()`；**inode 扇区缓存按 (slot, drive, lba) 三元组键控**（切卷绝不读到上一个卷的字节）。旧 API（`stat64/read64/write64/ls/opendir…`）保持"作用于当前卷"，fd64 新增 `fd64_open_on64(slot,…)`（打开后锁定该卷）。**系统组件固定写系统卷**：store64/config64/update64/app64/elf64/proc64/sysstate64 一律 `vfs64_*_on64(vfs64_system_slot64(), …)` —— 只在单次调用里临时切卷、返回前原样切回（LIFO 守卫），所以**用户浏览 `D:` 时 3 秒自动落盘也只会写 `C:`**（`tests/multivol64_test.py` 用宿主侧字节比对证明：D: 卷整盘逐字节不变、C: 的 `/store.a` 内容正确）。终端新增 `vol`（列卷/`vol C:|D:` 切当前卷）、`df` 列**所有卷**；Explorer 双击 `D:` 卡片 = `drive64_activate_letter64` 真切卷并列它的根目录（打点 `[UI] explorer enter letter=D: slot=1 ok`）。端到端：**108/108 PASS**（宿主机预置第二个 VimtuFS2 卷 `/docs` + `readme.txt` + `/docs/notes.txt`） |
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
| **桌面外壳与自带应用** | 窗口 z 序 / 拖拽缩放 / 任务栏 / 开始菜单 / 脏矩形 + **扫雷、计算器、终端、设置、任务管理器、文件资源管理器（此电脑）、系统监视器、关于** |
| **★ 文件资源管理器 / 此电脑（UI，本批次新增）** | `explorer64.cpp`：**单窗口导航**（此电脑页 + 盘内浏览）。左**导航窗格**（快速访问 / 此电脑 / 每个驱动器一个节点）、顶部**工具栏**（文件 / 计算机 / 查看）、**面包屑地址栏**（`此电脑 > 本地磁盘 (C:) > apps > demo`，每段可点）与**后退/前进/上级**按钮（hover/按下都有可见反馈）；右侧两视图：**图标视图**（几何画出的分类图标：文件夹/VAP64/ELF64/文本/其它）与**详细信息视图**（名称 / 修改日期（`YYYY-MM-DD HH:MM`）/ 类型 / 大小 四列）；此电脑页每块盘一张卡片：**容量条** + `X 可用，共 Y`（KB→GB/MB 换算，保留一位小数），**ESP/未知文件系统灰字标注"不浏览"且点不进去**；**双击任意可浏览盘符卡片 = 切到那个卷**（C:/D:/E: 都是各自盘上的真实内容）；底部状态栏 `N 个项目`。交互：单击选中、**双击**目录进入 / VAP64 走 `app64_launch64` / ELF64 走 `elf64_run64` / 文本开**只读预览窗口** / 其它给"没有关联的应用"提示；上下键选择、回车打开、Tab 切视图、滚动条分页（有界）。打点 `[UI] explorer …`、自检 `[EXPL] selftest PASS`；端到端 `tests/explorer64_test.py`（71 项：像素 + 串口 + QEMU 鼠标注入） |
| **AHCI(SATA) + 屏幕硬件检查报告（item 5a）** | `ahci64.cpp`：PCI 找 class 01/06/prog-if 01 → BAR5(ABAR，含 64 位高位) → 逐端口 PxCLB/PxFB + PxCMD(FRE→ST) → PxSSTS/PxSIG 判设备 → 非队列 DMA 命令表（CFIS H2D `0x27` + READ/WRITE DMA EXT `0x25/0x35`，LBA48）+ PRDT + PxCI 轮询（`g_ticks64`/自旋双超时、hlt 让出）。**统一驱动器号**：`0..3`=PATA、`8..`=AHCI 盘，`ata64.cpp` 内部按号分派 → 安装程序/分区引擎/VFS/store 的 weak 引用**都不用改**。`hwui64.cpp`：**真机没有串口时的唯一诊断画面** —— 把 CPU/内存/固件与地址空间/中断与 SMP/**存储（PATA 通道 + AHCI 控制器与端口 + NVMe 控制器/命名空间/容量）**/显示（fb+EDID）/输入（PS/2 8042 + UHCI HID；EHCI/xHCI 标"不支持"）/ACPI/网络逐条画在黑底分区块页面上；三处展示：① 启动期约 5 秒（任意键跳过）② 安装程序找不到磁盘时直接画在向导里 ③ 桌面 设置 → **硬件检查**页。指南见 `docs/真机验证指南.md` |
| **NVMe 驱动：能装到 NVMe 盘、UEFI 从这块盘启动（item 6）** | `kernel/nvme64.{h,cpp}`：PCI 找 class `0x01`/subclass `0x08`/prog-if `0x02`（= `0x010802`）→ **BAR0（64 位 MMIO，高 32 位也处理）** → 控制器使能（`CSTS.RDY=0` → `INTMS=0xFFFFFFFF` **全程轮询** → `AQA`/`ASQ`/`ACQ` → `CC`：CSS=0/MPS=0/IOSQES=6/IOCQES=4/EN=1 → `CSTS.RDY=1`）→ Admin 队列（Identify Controller / Identify Namespace(NSID 1，`NSZE`+`LBAF`) / Create I/O CQ+SQ，QD=8）→ I/O 轮询（Read/Write、`NLB` 0-based、**PRP1 + PRP2（两页直接指针 / 三页以上 PRP 列表）**、SQ tail / CQ head 门铃、CQ **phase 位**、`g_ticks64`+自旋双超时）。**驱动器号 `16..`**：`ata64.cpp` 内部按号分派，`ata64_drive_count64()`/`slot_to_drive64()` 把命名空间算进枚举 → 向导里直接能选、能分区、能装。单条命令 ≤ 128 扇区（64KB，内部分块）。QEMU 实测：**识别 → 装到 NVMe 盘（`[PART] … drive=16`、ESP FAT32 fat_ok=1）→ UEFI(OVMF) 从这块盘启动进桌面**（`U:loaded KERNEL64.BIN` → `[OS] booted from installed disk` → `[GUI64] ready`，`tests/nvme64_test.py`）；SeaBIOS 也能从 NVMe 启（真机 BIOS 是否认 NVMe 取决于固件，**推荐 UEFI**）。边界：单控制器/单队列/单命名空间/只支持 512B 逻辑块 |
| **内核自检** | 启动即跑内存、图形、调度器、VFS、store、syscall、ring3、ELF64/VAP64、APIC/SMP、USB、EDID 等自检，结果打到串口供自动验收断言 |

## 二、它**不能**做什么（诚实边界，重要）

| 边界 | 现状 |
|---|---|
| ⚠️ **文件操作的边界（本批次新增，如实写清）** | ① **没有递归删除**：非空目录点删除只会提示"目录非空，暂不支持递归删除"（`delete … rc=1 reason=not-empty`），不假装成功；② **单文件上限 67584 B**：超过的源在粘贴时被**整条跳过**（不是截断复制）；③ **没有权限/属主、没有回收站**：删除即真删（`unlink64`/`rmdir64`）；④ **重命名只在同一目录内**（改 inode 的 name 字段），**没有跨目录移动/拖拽**（`vfs64_rename64` 不动 `parent`）；⑤ 目录递归复制有界（深度 ≤4、条目 ≤96），超限的条目计入 `skipped`；⑥ 剪贴板是**内核内的路径列表**（不是文件内容快照、不跨重启、最多 8 条）；⑦ 重名后缀是 `(2)`/`(3)`（**无空格**，空格不是合法文件名字符） |
| ⚠️ **用户态独立地址空间** | 批次 C 起：proc64 每进程独立 CR3 + fork/execve/wait4/kill（BIOS 路径）；UEFI（固件页表）下默认仍如实降级为共享地址空间模式（`[PROC64] cr3 isolation OFF`）。**批次 D 实测**：在固件 PML4 上就地挂用户窗口（清 CR0.WP 手法）与自带 PML4 + 运行期 `mov cr3` **两条路径都在 QEMU+OVMF 与 VMware EFI 下成功**（`[PROC64] uefi exp result=B mode=isolated`），但默认构建不编这段实验（宏 `PROC64_UEFI_CR3_EXPERIMENT`），见 `docs/UEFI地址空间实验报告.md` |
| ⚠️ **fd 语义（批次 D）** | **每进程 fd 表**（32 槽/张；`Proc64` 持有，终端/桌面用内核表）；fd → 引用计数的 `OpenFile64`（**共享偏移游标**）：`dup/dup2` 共享同一对象、`fork` 逐槽继承、`execve` 默认保留（**无 `O_CLOEXEC`**）、`close` 只是 refs-1；`O_APPEND` 真实现；**`pipe(22)` 真实现**（64 B 环形缓冲、非阻塞：写满短写/读空 `-EAGAIN`），`user/pipe64.asm` 是 fork 后父子各持一端的环回证据 |
| ⚠️ **ELF64 只验证过自有静态程序** | 用 `ld.lld -static -nostdlib` 链接的自己的 ELF64 能 load → ring3 → `syscall` → exit；**glibc / 发行版二进制没有验证过**（缺 vDSO、TLS(FS.base) 的完整语义、信号投递、futex、动态链接与重定位） |
| ⚠️ **VFS 的限制（v3 目录树已落地，仍有边界）** | 支持多级路径 `/dir/sub/file`（`.`/`..`、大小写敏感）；**名字 ≤31B**、**inode 总数 ≤512**、**路径 ≤128B / 16 段**、**目录深度 16**；目录删除只支持 `rmdir` **空目录**（非空必须先清空）；**单文件仍 ≤67584B**（v3 没加二级间接块）；无权限/属主、无硬链接/符号链接、无稀疏文件。v2 旧卷仍能挂载（按单层语义）。**多卷**：最多 4 个可浏览卷同时挂载（系统卷 `C:` + `D:/E:/F:`），第 5 个起如实拒绝（`reason=voltable-full`）；终端 `vol` 切当前卷、`df` 列所有卷。终端 `ls/cat/write/touch/rm/mkdir/df` 与 ring3 的 `open/read/write/close` 都走 `kernel/fd64.cpp` 的 FD 层直连 VimtuFS2（终端命令的多级路径也已支持；**终端帮助与错误文案已改写成多级路径口径**，文件管理器 UI 也已落地（见上表））。终端另有 `fdtest`（独立游标/dup 共享/O_APPEND/pipe/fork 继承一键演示）。**FAT32 只读浏览的边界**：只读（写/删/改名/建目录/粘贴一律 `-FS64_EROFS` 拒绝）；扇区固定 512B、每簇扇区数按 BPB（1..128，U 盘常见 8=4KB/簇）；可用空间取**挂载时 FSInfo 快照**（不实时刷新，FSInfo 无效则标 unknown）；LFN 上限为 256B 缓冲内的 UTF-16 码元；无碎片整理/无删除项复用 |
| ⚠️ **设置页接线范围** | 显示/会话分区已接 `config64`/`session64`（真落 store64，跨重启保留）；系统/关于页是只读实测值（设备规格来自 hwinfo64/display64/net64/usb64） |
| ⚠️ **没有 TCP/IP / DHCP / DNS** | 网络只有 IPv4 + ARP + ICMP echo（e1000 轮询收发，无中断收包）；UDP/TCP、路由、DHCP、DNS 都没有；只适配 e1000，VMware 的 vmxnet3 未适配 |
| ⚠️ **USB 只有 UHCI + HID 引导键盘** | 没有 EHCI(USB 2.0) / xHCI(USB 3.x)，没有 USB 鼠标、U 盘、集线器；只认直接插在根端口上的键盘；不接中断（由 `kusb` 线程轮询） |
| ⚠️ **AP 只是停着** | SMP 能启动 AP 并让它报在线，但**没有多核调度**（调度器仍单核、IRQ0 只在 BSP）、没有 IPI、没有 per-CPU 数据/GDT/TSS，AP 自己的 LAPIC/中断不参与 |
| ⚠️ **ACPI/APIC 覆盖有限** | 只解析 RSDP/RSDT/XSDT/FADT/MADT/HPET/MCFG；关机仍走 ACPI 端口 0x604 + 8042 回退链；x2APIC 未适配；固件页表没映射 LAPIC/IOAPIC 的机器会留在 PIC |
| ✅ **SATA/AHCI 盘可直启（引导层已改 BIOS INT 13h）** | 引导层（`boot/loader64.asm`）的**磁盘启动路径**读内核不再用自写 PATA PIO，改走 **BIOS INT 13h 扩展读（AH=0x42 + DAP）**：驱动器号用固件传进来的 `DL`，分块 64 扇区（32KB、不跨 64KB 边界）读进低内存暂存区 `0x20000`，每批再进一次保护模式搬到 `0x100000`；失败复位磁盘重试 3 次后打 `[LM] int13 read FAILED ah=… lba=… retry=…` 并停机（不静默失败）。**BIOS 不需要把 SATA 设成 IDE 兼容模式** —— QEMU 上"只把装好的盘挂 `ich9-ahci` 启动"已进桌面（`tests/disk_boot_test.py`：`[LM] disk boot via INT 13h dl=0x80` → `[OS] booted from installed disk` → `[GUI64] ready`）；真机仍待复验。磁盘级 IDENTIFY/读写也已在 `--strict-dma` 档 PASS（item 5b 修好命令头布局） |
| ✅ **NVMe 盘现在能装、也能启动（item 6 起）** | 内核侧 `kernel/nvme64.{h,cpp}`：**驱动器号 `16..`** 的命名空间可以直接当安装目标盘（`[PART] … drive=16` → ESP/FAT32/GPT 全流程），**UEFI（OVMF）从这块装好的盘启动进桌面已实测**。仍有的边界：单控制器 / 单队列对（QD=8）/ 单命名空间（NSID 1）/ 全程轮询（无中断/无 MSI-X）/ 只支持 **512B 逻辑块**（4KB 逻辑块的盘如实打点并**不给驱动器号**）/ 单条命令 ≤ 64KB。**BIOS 从 NVMe 启动取决于固件**（BIOS 的 INT 13h 盘号来自固件自己的驱动表；QEMU 11.1 的 SeaBIOS 实测支持，但不少真机 BIOS 不认 NVMe）→ **推荐 UEFI 引导**。真机 NVMe 未测 |
| ⚠️ **没有 INT 13h 扩展读的老固件起不来** | 磁盘路径**没有 PIO 回退**（自写 PIO 在 AHCI 机器上读不到盘，已整段删除）：只有支持 EDD 扩展读（AH=0x42）的固件能启动，1998 年后的固件基本都有；更老的机器也跑不动本系统的 64 位长模式 |
| ⚠️ **安装建 ESP 需要目标盘 ≥ ~60MB** | ESP=48MB（真 FAT32：簇数必须 ≥ 65525，512B 扇区 + SPC=1 时卷下限就 ~33.5MB）+ 主分区至少 8MB + 盘尾 GPT 33 扇区 + 引导区 8009 扇区 = 122730 扇区 ≈ 59.9MiB。更小的盘（含 16MB 回归目标盘）只写老 MBR 布局（BIOS-only），串口打 `[INSTALL] esp skipped (disk too small)`。另外装好的盘只写**盘尾备份 GPT**（主 GPT 头的位置被 loader64.bin 占着）—— 依赖固件"主头无效时用备份头"（EDK2 已实测） |
| ⚠️ **只在虚拟机验证** | QEMU + VMware Workstation 双验证（BIOS 与 UEFI 都跑），**未在真机裸机验证**；UEFI 路径未做签名，测试时 `secureBoot=FALSE` |
| ⚠️ **磁盘仍是 PIO 搬运** | 有了 IRQ14 中断唤醒，但没有 DMA/Bus-Master，读写期间 CPU 仍要逐扇区搬 |
| ⚠️ **没有声音** | 无音频驱动 |
| ⚠️ **Rust 未参与** | 全部是 C/C++（clang）+ 汇编；Rust 是可选要求，本机未装工具链 |

**一句话定位**：*现在的 VimtuOS 是「有现代引导 + 现代安装体验 + 64 位桌面 + 真调度器 + 真文件系统 +
ring3 用户态」的自研单体内核；它能装进硬盘、跑起自己的桌面，也能把自己写的 VAP64 / ELF64 程序
 装进自己的文件系统并在 ring3 里运行 —— 但它还不能跑 glibc 级的第三方程序；独立地址空间只在 BIOS 路径生效
（UEFI 路径默认是共享窗口，运行期 CR3 实验已在两条固件路径上验证可行但默认不编）。*

## 三、技术亮点（值得一看的地方）

1. **纯 64 位，无 32 位业务代码**：16 位实模式只出现在 BIOS 物理必经阶段（读盘、问 E820/VBE）；UEFI 路径 100% 是长模式代码。
2. **内核运行在高半区**：`链接基址 0xFFFFFFFF80100000`，引导层建 `VA = 0xFFFFFFFF80000000 + PA` 的高半区线性直映，同时保留 0..4GB 恒等映射；**低地址与 4GiB 以上的用户窗口都留给用户态**。
3. **调度器用中断帧做上下文**：任务上下文就是 `pt_regs64`（0xD0 字节）本身，抢占发生在 IRQ0 里（`pic_eoi(0)` → `schedule64(r)` → 抬栈 `iretq`），不需要额外的上下文切换汇编；每任务独立 16KB 内核栈，初始帧手工构造后跳统一蹦床。
4. **两套系统调用入口并存**：自有 `int 0x80`（rax/rdi/rsi/rdx）与 Linux 号段的 `syscall` 指令（STAR/LSTAR/FMASK + 专用 16KB syscall 内核栈，和 TSS.rsp0 的耦合被显式切断）；号段语义互不干扰，帧标记分流。
5. **两种应用格式、两条完整链路**：自有 VAP64（32B 头 + 名字 + CRC32，`tools/make_vap.py` 打包）走 `int 0x80`；ELF64 走 `syscall` 指令 —— 从盘上读出来（VimtuFS2）到 ring3 跑完再回收，全程有串口证据。
6. **自研 UEFI 引导（不用 gnu-efi）**：自造 PE32+ 桩（2.5KB）+ 平铺长模式引导器（36KB，链接到 72MB，绕过 EDK2 的 PE 校验）；**RSDP 经固定槽 0x7800 从固件配置表传给内核**。
7. **一切自写工具链**：没有 mkfs.fat、没有 mtools —— FAT32 卷、ISO9660 解析与打包、GPT、LBA 回填、PE 摊平、字体子集化（**4 套 TTF**：西文/中文/终端等宽/缺字兜底）、图标生成、VAP64 打包，全是本仓库的 Python 脚本。
8. **APIC / ACPI / SMP 都带"宁可不启用也不变砖"的回退**：拿不到 ACPI 就留 8259、启动 AP 全程有界超时、每个自检失败都整体回滚，降级路径都有串口证据。
9. **每次改动都跑自动验收**：**31 个验收脚本**（≥850 条断言，本轮新增 `tests/usb_boot_both_fw_test.py` 与 `tests/esp_install_test.py`），全部是"字节级 + 像素级 + 串口日志"三合一，而不是"看起来能跑"。

## 四、系统架构

```
① 引导层（boot/，13 文件 / 3,655 行）
   BIOS 光盘   El Torito → cdiso.asm（引导桩 + 介质描述符 @0x0F00）→ loader64.asm
   U 盘/裸盘   hybrid_mbr.asm（自搬 0x0600）→ boot.asm（512B MBR）→ loader64.asm
   UEFI        BOOTX64.EFI（自研 PE 桩）→ UEFI64.BIN（平铺长模式引导器）
                   └─ 从 ESP 读 KERNEL64.BIN → 0x100000（安装盘上还有 SYSTEM.IMG → 0x04000000）
   loader64 / UEFI64 负责：E820 / VBE(EDID→0x7600) / RSDP(→0x7800) → 读内核到物理 0x100000
   （磁盘启动 = BIOS INT 13h 扩展读 AH=0x42；光盘 = ATAPI；U 盘 hybrid = 桩先搬好）
                             → 建页表（恒等 + 高半区直映）→ 进长模式 → 跳内核入口

② 内核层（kernel/，71 文件 / 27,900 行）
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
     part64.cpp        分区表（MBR/混合 MBR/盘尾 GPT）/ 新建 / 删除 / 格式化（VimtuFS2）/ 安装引擎 + ESP 安装
     fat64.cpp         最小 FAT32 写入器（安装时在目标盘建 ESP；卷参数与 tools/make_esp.py 逐条对齐）
     vfs64.cpp         VimtuFS2 v3：超级块+CRC32 / 空闲位图 / 128B inode + mtime/kind / 多级路径 / 目录树遍历 / v2 兼容
                      ★ 多卷：4 个卷槽 + 按槽挂载/激活 + on64（固定系统卷）写路径 + 缓存按 (slot,drive,lba) 键控
     drive64.cpp       盘符与驱动器枚举：所有盘 -> MBR 分区 -> fs 识别 -> C:/D:/E: 盘符表 + 卷槽分配/激活
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
   tests/*.py     27 个断言脚本（≥905 断言）：串口断言 + screendump 像素断言 + 目标盘字节断言
```

## 五、快速开始

### 1) 构建（Windows + MSYS2）

```bash
pacman -S --noconfirm --needed mingw-w64-x86_64-clang mingw-w64-x86_64-lld \
                               mingw-w64-x86_64-compiler-rt nasm xorriso
# 还需要 Python 3 + fontTools + Pillow（字体子集化 / 图标生成）：pip install fonttools pillow
#
# ① 字体（源码仓库**不含字体本体**）：直接跑 `py -3 _otf2ttf.py` 就会按清单校验/下载/派生
#    —— 需要的文件、URL、版本、SHA256 全在 [FONTS.md](FONTS.md) 与 docs/字体许可说明.md 里。
#    目录结构：Fonts/（西文 Noto Sans 五个静态字重）+ Fonts-open/（Noto Sans SC 五个静态字重 +
#    sarasa-mono-sc-regular.ttf 终端等宽 + unifont-14.0.01.ttf 缺字兜底 + 四份许可全文）。
#    ★ 字体缺失/损坏 = **构建硬失败**（校验魔数 + SHA256，绝不静默换字体）；许可本体进仓，
#      另有一份副本在 docs/fonts/。
#    四个内核字体面：face0 西文 / face1 中文 / face2 终端等宽（中英 1:2）/ face3 缺字兜底。

# ② 构建：产出 vimtu64-64.iso（三合一安装盘）
bash build64.sh
```

产物：

| 文件 | 大小 | 说明 |
|---|---|---|
| `vimtu64-64.iso` | 56.2 MB | **三合一安装盘**：BIOS 光盘 + 可写 U 盘 + UEFI |
| `vimtu64-64.img` | 8.3 MB | 安装介质裸盘（把一个 8MB 镜像直接当硬盘用） |
| `build64/kernel64.bin` | 1.89 MB | 安装程序内核（1,980,680 B；含 ATA/AHCI/**NVMe** 驱动、分区/安装引擎、FAT32 ESP 写入器） |
| `build64/kernel64_os.bin` | 2.80 MB | 装进硬盘的系统内核（2,797,944 B；调度器/VFS/store/ring3/网络/USB/AHCI/**NVMe**/文件管理器 + **文件操作** 都在这里） |
| `build64/BOOTX64.EFI` / `UEFI64.BIN` | 2.5 KB / 36 KB | UEFI 两段式引导 |
| `build64/esp.img` | 48 MB | 手写 FAT32 ESP（96736 簇，>= 65525） |

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

* **28 个断言脚本全部 PASS**（2026-09-19 实测；上一批的断言保持全过 + 本批次新增 51 条）。`--full` 覆盖 22 个脚本 / **613 条断言**：

  | 脚本 | 断言 | 脚本 | 断言 |
  |---|---|---|---|
  | boot64_assert.py | 16 | desktop64_test.py | 30 |
  | mouse_parse_test.py | 12 | net64_test.py | 18 |
  | install_flow_test.py | 19 | apic64_test.py | 47 |
  | partition_ops_test.py | 18 | smp64_test.py | 52 |
  | screen64_probe.py | 5 | sched_stress_test.py | 18 |
  | iso64_install_test.py | 18 | usb64_test.py | 51 |
  | iso64_usb_test.py | 22 | vmware_install_test.py | 21 |
  | uefi64_install_test.py | 21 | proc64_test.py | 67 |
  | preload_update_test.py | 40 | tmgr_proc_test.py | 39 |
  | display_runtime_test.py | 19 | fs_term_test.py | 32 |
  | **fd64_test.py（批次 D 新增）** | **34** | **uefi_cr3_experiment_test.py（批次 D 新增）** | **14** |
  | **小计** | **613** | | |

  另有专项脚本：`elf64_test` 56、`store64_test` 46、`app64_test` 31、`display64_test` 22、`user64_test` 17
  （以及 sysstate64_test / ui_extra64_test / screenshot64 等，条数随实现增长）。
  **item 6 新增 `tests/nvme64_test.py`（38 条断言，全 PASS）**：NVMe 控制器/命名空间/队列打点 +
  只读自检 + **完整装到 NVMe 盘**（`[PART] … drive=16` / ESP FAT32 / GPT）+ 目标盘与 **ESP 三文件逐字节** +
  **UEFI(OVMF) 从这块盘启动进桌面** + BIOS(SeaBIOS) 如实测（能起来就断言、起不来就如实 SKIP 并说明
  "BIOS 从 NVMe 启动取决于固件"）。
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
| M3 | 光盘（ATAPI）、U 盘 hybrid、GPT、手写 FAT32 ESP（48MB，簇数 >= 65525） | ✅ |
| M4 | **32 位工程整体退役**，纯 64 位单构建线 | ✅ |
| M5 | 桌面栈移植：内存管理 + 外壳 + 5 个应用 | ✅ |
| M6 | **UEFI 自动引导打通**（PE 基址 5GB / ESP 名为 FAT16 实为 FAT12 / 进内核 CS 未换） | ✅ |
| M7 | **内核搬高半区**（为应用层让出低地址），四条引导路径全回归 | ✅ |
| M8 | ATA 中断（IRQ14）、运行期显示层（EDID）、硬件详情（CPU/PCI/磁盘） | ✅ |
| M9 | **调度器**（task64：16 槽 / 16KB 栈 / 8ms 时间片 / IRQ0 抢占 / kill / kstress） | ✅ |
| H | **文件资源管理器 / 此电脑（UI）**：导航窗格 + 面包屑 + 图标/详细信息双视图 + 容量条 + 双击运行 VAP64/ELF64 + 只读文本预览 | ✅ |
| M10 | **真实文件系统 VimtuFS2 + store 持久化**（双槽 + 世代号 + 双 CRC32，跨重启保留） | ✅ |
| M11 | **ring3 用户态 + 两套 syscall 入口**（`int 0x80` 自有 ABI / `syscall` 指令 Linux 号段） | ✅ |
| M12 | **VAP64 应用格式与加载器 + ELF64 加载器**（自有静态程序可从盘上装、可在 ring3 跑） | ✅ |
| M13 | **ACPI 解析 + APIC 接管 + SMP 启动 AP**（均有优雅降级） | ✅ |
| M14 | **网络（e1000 + ARP/ICMP）与 USB 主机（UHCI + HID 键盘）** | ✅ |
| M15 | **NVMe 驱动（item 6）**：PCI `0x010802` + BAR0(64 位 MMIO) + admin/I-O 队列 + 轮询 PRP 读写 → **驱动器号 `16..`**（上层零改动）→ 可装到 NVMe 盘、**UEFI 从这块盘启动进桌面** | ✅ |

## 八、路线图（未完成的部分）

按依赖顺序（这也是欢迎贡献的方向）：

| 顺序 | 目标 | 现状 | 做完能带来什么 |
|---|---|---|---|
| ① | ~~**独立地址空间 + fork/execve**~~ | 批次 C 已完成（每进程 CR3 + fork/execve/wait4/kill；UEFI 固件页表下如实降级为共享模式） | —— |
| ② | **glibc 级兼容** | 只验证过自有静态 ELF64 | TLS(FS.base) 真生效、信号投递、vDSO、futex、动态链接与重定位；毕业考试是"静态 busybox 起 shell" |
| ③ | **VFS 补齐** | ~~单层路径~~（v3 已支持多级目录树 + 空目录删除 + mtime/类型判定）；仍缺：单文件 >67584B（需二级间接块）、权限/属主、宿主机拷文件工具、FAT 浏览、文件管理器 UI（**本批次已完成**：explorer64.cpp + 	ests/explorer64_test.py 71 项） | FAT 只读浏览 + 更大单文件 + 复制/粘贴/重命名等文件操作 |
| ④ | **TCP/IP** | 只有 IPv4/ARP/ICMP、静态地址、无中断收包 | DHCP/DNS/UDP/TCP；vmware vmxnet3 适配 |
| ⑤ | **EHCI / xHCI / USB 存储 / 集线器 / 鼠标** | 只有 UHCI + 根端口 HID 引导键盘 | U 盘、鼠标、带 hub 的真机 |
| ⑥ | **多核调度** | AP 起来后只是 `cli; hlt`；无 IPI、无 per-CPU 数据 | 真 SMP：AP 参与调度与中断；x2APIC |
| ⑦ | **设置页接 store** | store 本体可用，设置页/启动项未接线 | 分辨率/缩放/窗口位置能真的保存 |
| ⑧ | **真机验证** | 只在 QEMU + VMware 验证过 | 真机驱动覆盖（ACPI 变体、EHCI/xHCI、网卡）与稳定性 |
| — | **Rust 参与实现** | 未用（本机未装工具链） | 可选项：选一个模块（如调度器/文件系统）用 Rust 写即可证明这条路通 |

## 九、目录结构

```
VimtuOS/
├── build64.sh                # 唯一构建脚本（产出 ISO / IMG / 安装内核 / 系统内核）
├── build_uefi.sh             # UEFI 两段式引导的构建（被 build64.sh 调用）
├── bootinfo.h                # 引导层 → 内核的启动信息结构（BootInfo / E820 / VBE / EDID）
├── 构建与运行说明.md          # 环境准备、构建、运行、验收的步骤说明
├── _make_logo.py             # logo/logo.png → 开机画面 RGBA 资源（240×150）
├── _make_icons.py            # 桌面图标 RGBA 资源（我的电脑/回收站/终端）
├── _make_start_icon.py       # 开始按钮图标（logo/kaisi.png → 24×24）
├── _otf2ttf.py               # 源字体准备：四个面的源文件校验/下载/中文静态字重派生（缺=硬失败）
├── _subset_fonts.py          # face 0/2/3 子集化（Noto Sans / Sarasa Mono SC / Unifont）+ 链路自检
├── _subsetsimhei.py          # face 1 中文子集化（Noto Sans SC 静态 Regular）
├── FONTS.md                  # 字体清单：文件名｜来源 URL｜版本｜协议｜SHA256（含哪些参与构建）
│
├── boot/                     # ① 引导层（16 位实模式只出现在 BIOS 物理必经阶段）
│   ├── boot.asm              # 512B MBR：自搬到 0x0600 再执行
│   ├── hybrid_mbr.asm        # 写进 ISO 第 0 扇区的 hybrid MBR（U 盘/硬盘引导）
│   ├── cdiso.asm             # 光盘引导桩（El Torito）+ 介质描述符
│   ├── loader64.asm          # 实模式准备 + INT 13h 读内核 + 建页表 + 进长模式（≤4096B）
│   ├── loader64_atapi.inc    # ATAPI(PACKET) 读光盘（光盘路径专用）
│   └── efi/                  # UEFI 两段式
│       ├── stub.c            # 自研 PE32+ 极小桩（-base:0x0，2.5KB）
│       ├── uefi64.c          # 平铺长模式引导器（GOP / 内存映射 / 页表 / 跳内核）
│       ├── jump64.asm        # 远跳进内核（换 CS）
│       ├── efi.h             # UEFI 类型定义
│       ├── efi_main.c        # （废弃）早期 UEFI 试验
│       ├── enter64.asm       # （废弃）
│       └── probe_main.c      # （废弃）
│
├── kernel/                   # ② 内核（x86_64 长模式，链接在高半区 0xFFFFFFFF80100000）
│   ├── kernel64.cpp          # 入口 / 清 BSS / 自检 / 两条启动路径（安装向导 或 桌面）
│   ├── linker64.ld           # 链接脚本（高半区基址与段布局）
│   ├── memlayout64.h         # 内存/磁盘布局唯一定义点（LBA、地址、页表位置）
│   ├── entry64.asm           # 内核入口（段选择子 / 栈）
│   ├── x86_64.cpp/.h         # GDT/IDT/TSS/PIC/PIT 250Hz/RTC + IRQ 分派（APIC 与 PIC 双模式）
│   ├── isr_stubs64.asm       # 中断桩（0xD0 帧布局的唯一权威定义）
│   ├── switch64.asm          # 任务上下文切换（抬栈 iretq）
│   ├── mem64.cpp / mem_64.h  # 物理页池 + 48MB 内核堆 + 归属记账 + 页表助手 + libc 例程
│   ├── task64.cpp/.h         # 调度器（16 槽任务表 / 8ms 时间片 / IRQ0 抢占 / 回收 / kstress）
│   ├── proc64.cpp/.h         # 进程与地址空间（每进程 CR3 / fork / execve / wait4 / kill）
│   ├── usermode64.cpp/.h     # ring3 入口与用户窗口（4GiB 起，代码 R/X、栈 R/W/NX）
│   ├── syscall64.cpp/.h      # 系统调用分发（int 0x80 自有 ABI + syscall 指令 Linux 号段）
│   ├── syscall_entry64.asm   # syscall 指令入口（专用内核栈 + 帧校验 + o64 sysret）
│   ├── vfs64.cpp/.h          # VimtuFS2（v3：超级块 CRC / 位图 / inode / 目录树 / 4 槽多卷）
│   ├── fs64.cpp/.h           # 文件系统分派层（VimtuFS2 读写 / FAT32 只读）
│   ├── fd64.cpp/.h           # 每进程 fd 表 + 引用计数打开文件对象 + pipe
│   ├── fat64.cpp/.h          # FAT32 读写器（安装时写 48MB ESP + 只读浏览 + VFAT 长名）
│   ├── store64.cpp/.h        # 设置持久化（/store.a、/store.b 双槽 + 世代号 + 双 CRC32）
│   ├── config64.cpp/.h       # 类型化配置 KV（落在 store 上；含图标位置、语言、缩放）
│   ├── session64.cpp/.h      # 会话 / 应用内容策略
│   ├── sysstate64.cpp/.h     # 运行状态机 + 模块注册表 + 健康报告 + ring log
│   ├── panic64.cpp/.h        # BSOD 蓝屏 + 看门狗
│   ├── preload64.cpp/.h      # 字形与图标预热（首帧 61.3M → 1.80M cycles）
│   ├── update64.cpp/.h       # 更新机制（/update.pending → 应用 → 软重启）
│   ├── app64.cpp/.h          # VAP64 自有应用格式（安装器 / 校验 / 启动器 / 魔数分派）
│   ├── elf64.cpp/.h          # ELF64 加载器（PT_LOAD / p_flags 权限 / SysV 初始栈与 auxv）
│   ├── hwui64.cpp/.h         # 屏幕硬件检查报告（启动期 / 向导无盘 / 设置页 三处展示）
│   ├── ata64.cpp/.h          # PATA PIO + IRQ14 等待（超时回退轮询）
│   ├── ahci64.cpp/.h         # AHCI(SATA) 驱动（非队列 DMA + 轮询）
│   ├── nvme64.cpp/.h         # NVMe 驱动（BAR0 64 位 MMIO + admin/I-O 队列 + PRP）
│   ├── part64.cpp/.h         # 分区表（MBR/GPT）+ 新建/删除/格式化 + 安装引擎（含 ESP）
│   ├── setup64.cpp           # Win10 同款安装向导（无密钥、真分区、真进度、自动重启）
│   ├── gui64.cpp/.h          # 桌面外壳（窗口 / 任务栏 / 开始菜单 / 脏矩形 / 桌面图标）
│   ├── explorer64.cpp/.h     # 文件资源管理器 + 此电脑（导航 / 面包屑 / 双视图 / 右键菜单 / 文件操作）
│   ├── calc64.cpp            # 计算器（Q16.16 纯整数）
│   ├── mines64.cpp           # 扫雷（三难度 / 右键标旗 / 键盘）
│   ├── terminal64.cpp        # 终端（30+ 命令：ps/run/elfrun/vol/ping/store/ping…）
│   ├── settings64.cpp        # 设置（显示 / 缩放 / 语言 / 会话 / 设备规格 / 关于）
│   ├── taskmgr64.cpp         # 任务管理器（进程页=真进程 / 性能页含显卡项 / 启动 / 详细信息）
│   ├── fb.cpp/.h             # 帧缓冲（VBE-LFB、缩放、裁剪、脏区提交）
│   ├── font.cpp/.h font8x8.h # TrueType 渲染（4 个字体面 + 查询链 + 缺字占位；8x8 位图兜底）
│   ├── input.cpp/.h          # PS/2 键鼠（IRQ1/IRQ12）+ 串口按键通道 + USB 注入口
│   ├── display64.cpp/.h      # 运行期显示层（模式清单 / 0x3DA 刷新率实测）
│   ├── edid64.cpp/.h         # EDID 解析（厂商 / 型号 / 尺寸 / 首选时序 / 刷新率）
│   ├── acpi64.cpp/.h         # ACPI 解析（RSDP→RSDT/XSDT→FADT/MADT/HPET/MCFG）
│   ├── apic64.cpp/.h         # LAPIC + IOAPIC 接管（可回退 8259）
│   ├── smp64.cpp/.h          # SMP：INIT-SIPI-SIPI 启动 AP
│   ├── ap_trampoline64.asm   # AP 跳板（234B，按物理 0x8000 汇编）
│   ├── hwinfo64.cpp/.h       # CPUID / PCI 只读枚举 / 磁盘信息
│   ├── e1000_64.cpp/.h       # Intel 82540EM 网卡驱动（轮询）
│   ├── net64.cpp/.h          # 以太网 / IPv4 / ARP / ICMP
│   ├── usb64.cpp/.h          # UHCI + HID 引导键盘（按键注入 PS/2 队列）
│   ├── debug64.h             # 串口打点 + 行级锁
│   ├── port.h                # 端口 IO 助手
│   ├── uefi_gop.h            # GOP 模式信息结构
│   └── isr_dbg64.asm / isr_out64.asm / isr_probe64.asm   # 排障用中断桩（可保留）
│
├── user/                     # ③ ring3 示例程序（构建时嵌进内核，启动时幂等装进系统盘）
│   ├── hello64.asm           # VAP64 示例（int 0x80 自有 ABI）
│   ├── hello_elf64.asm/.ld   # ELF64 示例（syscall 指令 + 用户窗口链接脚本）
│   ├── filedemo64.asm        # 文件读写示例
│   ├── proc64.asm            # 多进程示例（fork / execve / wait4 / kill）
│   ├── pipe64.asm            # 管道示例（父子通信）
│   └── spin64.asm            # 长命进程示例（任务管理器进程页演示）
│
├── tools/                    # ④ 自写打包与诊断工具（Python，纯标准库）
│   ├── make_iso64.py         # ISO9660 + El Torito + GPT + ESP + hybrid MBR 打包
│   ├── make_esp.py           # 手写 FAT32 卷（ESP，48MB，簇数 ≥65525 硬断言）
│   ├── make_vap.py           # VAP64 应用打包器（与 kernel/app64.h 逐字段一致）
│   ├── make_flat.py          # PE 摊平
│   ├── fat_check.py          # FAT12/16/32 卷按规范体检
│   └── pe_info.py            # PE 体检（能告出 ImageBase）
│
├── tests/                    # ⑤ 验收脚本（40+ 个 .py / 断言 1,000+ 条：串口 + 像素 + 字节）
│   ├── status_report.py      # 状态核查（每项能力绑证据：DONE / PARTIAL / MISSING）
│   ├── boot64_assert.py      # 长模式 / IDT / PIT / BootInfo
│   ├── install_flow_test.py  # 端到端安装 + 装完单独启动
│   ├── partition_ops_test.py # 新建 / 格式化 / 删除 真实写盘（字节级）
│   ├── iso64_install_test.py / iso64_usb_test.py            # 光盘 / U 盘形态
│   ├── vmware_install_test.py / uefi64_install_test.py       # VMware BIOS / UEFI 全流程
│   ├── esp_install_test.py / disk_boot_test.py / usb_boot_both_fw_test.py   # 安装盘与双固件
│   ├── ahci64_test.py / nvme64_test.py                       # SATA / NVMe 目标盘
│   ├── explorer64_test.py / fileops64_test.py                # 文件管理器与文件操作（像素 + 鼠标注入）
│   ├── multivol64_test.py / fatread64_test.py                # 多卷挂载 / FAT32 只读浏览
│   ├── fs_tree_test.py / fs_term_test.py / fd64_test.py       # 目录树 / 终端文件命令 / fd 语义
│   ├── store64_test.py / sysstate64_test.py / preload_update_test.py   # 持久化 / 状态机 / 预热与更新
│   ├── proc64_test.py / user64_test.py / elf64_test.py / app64_test.py # 进程 / ring3 / ELF64 / VAP64
│   ├── apic64_test.py / smp64_test.py / sched_stress_test.py  # APIC / SMP / 调度压力
│   ├── net64_test.py / usb64_test.py / tmgr_proc_test.py      # 网络 / USB / 任务管理器进程页
│   ├── desktop64_test.py / ui_extra64_test.py                 # 桌面外壳 / 补回的 UI 功能
│   ├── display64_test.py / display_runtime_test.py            # EDID / 运行期显示层
│   ├── mouse_parse_test.py / screen64_probe.py / screenshot64.py   # 鼠标解码复放 / 像素探测 / 截图
│   └── vmware_make_vm.py / vmware_mouse_probe.py              # VMware 环境辅助
│
├── logo/                     # 开机 logo 与开始按钮图标源图（logo.png / kaisi.png）
│
└── docs/                     # ⑥ 中文文档（14 篇）与截图
    ├── VimtuOS系统介绍.md      # 对外介绍（含桌面截图）
    ├── 项目状态总览.md         # 能力项 × 证据 × 发布记录
    ├── 应用层与系统调用说明.md  # VAP64 / ELF64 / 两套 syscall / 进程与地址空间 / 离 glibc 差什么
    ├── 发布流程.md             # 版本号规则与发布步骤（每次发布新建标签）
    ├── 真机验证指南.md         # U 盘写入 / BIOS 设置 / 逐阶段期望 / 故障排查 / 兼容性矩阵
    ├── UEFI引导说明.md · UEFI地址空间实验报告.md · 引导链架构.md
    ├── 安装程序说明.md · ISO安装介质说明.md · 桌面栈移植说明.md
    ├── 中断调试记录.md · 重启与任务管理器设计.md · 字体许可说明.md · 64位升级验证记录.md
    ├── fonts/                 # 四份许可全文（OFL-NotoSans / OFL-NotoSansSC / OFL-Sarasa / UNIFONT-LICENSE）
    ├── screenshots/           # 自动抓取的界面截图（桌面 / 此电脑 / 管理器 / BSOD / 任务管理器…）
    └── shots/                 # 历史验证截图集
```

仓库规模：内核 `kernel/` 94 文件 / 4 万余行、引导 `boot/` 12 文件、验收 `tests/` 40+ 脚本、
文档 `docs/` 14 篇；**发布进仓库的源码约 2 MB**（第三方字体、打包的交叉编译器、构建产物与退役的
32 位工程都不进仓，见 `.gitignore`）。


## 十、开源与发布状态

**本仓库已开源：<https://github.com/Fanqi89/VimtuOS>** —— 只发**源码**（181 个文件 / 约 1.9 MB），许可 **GPL-3.0**。

**没有进仓的东西**（都在 `.gitignore` 里，按体积排序）：

| 未发布 | 体积 | 原因 |
|---|---|---|
| `tools/i686-elf/` | 849 MB | 打包的第三方交叉编译器（README 里给安装方式即可） |
| `legacy32/` | 770 MB | 已退役的 32 位工程，建议将来单独立仓作"移植参考" |
| `Fonts/` | 2.8 MB | 西文 UI 字重（Noto Sans Thin/Light/Regular/Bold/Black，SIL OFL 1.1）——只有 `OFL-NotoSans.txt` 进仓 |
| `Fonts-open/` | 75 MB | 中文/等宽/兜底源字体（Noto Sans SC 静态字重 + 官方 VF、Sarasa Mono SC、GNU Unifont）——只有 `OFL-*.txt` 与 `UNIFONT-LICENSE.txt` 进仓；下载见 [FONTS.md](FONTS.md) |
| `build64/` + `build/` | 80 MB | 构建产物 |
| `vimtu64-64.iso` / `.img` | 56 MB / 8 MB | 构建产物（内嵌的字体子集是 OFL/GPL 开源字体，可分发；Release 附件里有） |
| `target-*.img` / `*.log` | ~64 MB | 验收测试残留 |

**发布二进制（ISO/IMG）的字体问题已解决 + 本轮重构为四个字体面**：安装镜像里的字体子集已经从
`bahnschrift` / `ChaparralPro` / `simhei`（Windows 商业字体，不可再分发）换成开源字体族：
**face0 Noto Sans（西文）/ face1 Noto Sans SC（中文）/ face2 Sarasa Mono SC（终端等宽，中英严格 1:2）/
face3 GNU Unifont（缺字兜底，只收前三个面都没有的界面/终端码点）**，衬线面已退役；
`_otf2ttf.py` / `_subset_fonts.py` / `_subsetsimhei.py` 读 `Fonts/` 与 `Fonts-open/`，输出文件名
（`build/font_bahnschrift.ttf` / `font_simhei.ttf` / `font_mono.ttf` / `font_fallback.ttf`）与内核里的
objcopy 符号名保持不变。运行时打点：`[FONT64] faces=4 …` / `mono ascii=8 cjk=16 ratio=2` /
`fallback hit cp=0x… face=3` / `selftest PASS mask=…`（验收：`py -3 tests\fonts64_test.py`）。
许可与派生说明见 [docs/字体许可说明.md](docs/字体许可说明.md)。**因此 ISO/IMG 可以直接公开分发。**

**版本与发布**：标签 `v0.2.1-beta6`，Release：<https://github.com/Fanqi89/VimtuOS/releases/tag/v0.2.1-beta6>
（历史版本各自保留安装程序：`v0.2.0-beta.5` / `v0.2.0-beta.4` / `v0.2.0-beta.3` / `v0.2.0-beta.2` / `v0.1.0-beta.1` —— 见 <https://github.com/Fanqi89/VimtuOS/releases>）
（预发布；**安装盘已附上**：`vimtu64-64.iso` 三合一安装盘 + `vimtu64-64.img` 裸盘介质）。

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

**安装镜像内嵌的字体子集**（`Noto Sans` / `Noto Serif` / `Noto Sans SC` 的字形子集）不在 GPL-3.0 范围内：
它们是各自许可（**SIL Open Font License 1.1**）下的派生物，随附版权与许可声明见
[docs/字体许可说明.md](docs/字体许可说明.md)（含字体版本、来源 URL 与许可全文要点）。

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

Honest boundaries: user address spaces are **per-process** on the BIOS path (proc64: own CR3 +
fork/execve/wait4/kill) and degrade to a shared window on UEFI firmware tables — a runtime CR3
experiment (A: attach the user window into the firmware PML4 with a short CR0.WP window; B: own PML4 +
`mov cr3`) **succeeded on both QEMU/OVMF and VMware EFI** in an opt-in build, but it is off by default
(see `docs/UEFI地址空间实验报告.md`); the ELF64 loader is only
verified with **our own static binaries** (glibc/distro binaries are untested: no vDSO, signals, futex or
dynamic linking); VFS v3 has a **directory tree** (`/dir/sub/file`, `.`/`..`; names ≤31B, ≤512 inodes, files
still ≤67584B, empty-directory `rmdir` only; v2 volumes still mount) and the terminal's file
commands (`ls/cat/write/touch/rm/mkdir/df`) use the real VimtuFS2 through the fd64 layer (ring3
`open/read/write/close` share it). Since batch D the fd layer is **per-process** with
reference-counted open-file objects: `dup`/`dup2` share one object (shared offset), `fork` inherits the
whole table, `execve` keeps fds (no `O_CLOEXEC` yet), `O_APPEND` is real and `pipe(22)` works
(64-byte ring buffer, non-blocking). There is no TCP/IP stack and

Everything was validated on **QEMU and VMware, BIOS and UEFI — not on bare metal**.

Quality-wise, every change is verified by **28 assertion scripts** (22 of them in
`tests/status_report.py --full`) that check serial logs, screen pixels (QEMU screendumps) and raw disk bytes.
Tooling adds ~3k lines of self-written Python (ISO9660/GPT/FAT32/VAP64 packing, font subsetting, PE/FAT diagnostics).

