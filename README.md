# VimtuOS · Vimtu64

**一个从零手写的 64 位操作系统内核**（x86_64，纯 64 位长模式，不基于 Linux/Windows/任何现成内核），
目标是"能当日常系统用"，而不是教学玩具。

- 语言：C / C++（内核）+ Rust（设计 Token 表）+ 汇编（引导与入口）+ Python（构建与验收）
- 版本：**0.9.43**（版本号不带 `-beta`；小版本 v0.4.x、大版本 v0.5.0…）
- 许可：GPL-3.0
- 验收方式：**77 个自动化验收脚本**，每条断言都有串口打点或像素证据（见 [验收](#五验收与工程质量)）

> 想直接看"现在能干什么 / 还差什么"：跳到 [一](#一它现在能做什么) 与 [二](#二它明确做不到什么诚实边界)。
> 逐批历史（P1 → v0.4.6 的完整记录）已拆到 [docs/开发历程.md](docs/开发历程.md)。

---

## 一、它现在能做什么

### 1. 引导与安装
| 能力 | 现状 |
|---|---|
| BIOS 光盘 / U 盘 / 裸盘 | ✅ MBR + El Torito + hybrid ISO；INT 13h 读盘（含 SATA/AHCI） |
| UEFI | ✅ 两段式（极小 PE 桩 + 平铺长模式引导器），OVMF 与 VMware EFI 双通 |
| 分区表 | ✅ MBR + GPT（含安装到 GPT 盘、盘尾备份 GPT） |
| 安装程序 | ✅ Win10 风格向导；新建/删除/格式化分区**真实写盘**（v0.5.14 起 GPT 盘也打对表、删除/格式化**两步确认**）；**装完系统卷 = 分区大小**（v0.5.15，实测 100 GiB 盘装出 `total=209608854` 块且 378 个文件逐字节可读）；**ISO 装盘写入完整 16 MiB 载荷**（v0.5.18 修：修前只写 4.13 MB，`/Wallpapers`、`/Avatars` 等 inode 号靠后的文件全丢）；进度百分比；装完自动重启 |
| 交付物 | `vimtu64-64.iso`（三合一安装盘）、`vimtu64-64.img`（裸盘介质） |

### 2. 内核
- 内存：堆 + 页池 + 归属记账（`[MEM64] selftest`）
- 调度：多任务 + 抢占 + SMP（AP 启动），`[SMP] online=n/n`
- 进程：**每进程独立地址空间**（BIOS 路径），`fork/execve/wait4/kill` 真语义；
  UEFI 固件页表下自动降级为共享地址空间并**如实打点**（绝不假装隔离成立）
- 中断：PIC ↔ APIC/IOAPIC 双模；PIT 250Hz
- 设备：PCI 枚举、ACPI、PS/2 键鼠、PATA/AHCI/NVMe、EHCI/xHCI + USB 存储、e1000、HDA 音频、virtio-gpu(2D)

### 3. 文件系统与存储
- **VimtuFS2 v4**：目录树、权限（uid/gid/mode）、mtime、kind
- FAT32：只读浏览（含 VFAT 长名）+ **U 盘可写**（新建/覆盖/建目录/删除/截断 + 写后读回校验）
- **ext4：只读服务卷**（v0.9.40 起"人真能用"）
  - 解析在 **ring 3 服务**（`/usr/bin/ext4svc`）里，内核**不解析 ext4**：内核只发**反向 RPC**
    （`R3drvReq`：STAT/LIST/READ，150 ms 有界等待 + 每圈让出 CPU），服务把结果写回应答页。
  - 界面侧：`drive64` 认出 ext4 → 给盘符（`ro=1`）→ `fs64` 登记成"服务卷"（`vol=9`，`FS64_KIND_EXT4R`）
    → `list64/stat64/read64/read_range64/ls64` 五处分派都走服务。
  - **实测可用**：文件管理器里双击 `D:` 能列目录、双击 `hello.txt` 弹只读预览（`tests/ext4gui64_test.py` 34/34）；
    内核终端 `vol D` + `ls`/`cat` 也能看（`tests/ext4term64_test.py` 17/17）。
  - 覆盖：extent（depth 0/1）、4 KiB 块、直接/间接块链；**只读**（写入口统一 `-EROFS`）。
- 盘符层：PATA/AHCI/NVMe 每块盘 → 分区 → fs 识别 → `C:` / `D:` …
- **系统卷目录布局**（v0.6.0 重排：系统级集中 + Linux 分类）
  ```
  /                       根只留入口
  ├── System/             ← 系统级集中（Windows 的 C:\Windows 类比）
  │   ├── fonts/          字体（原 /Fonts + /Fonts-open）
  │   ├── icons/          图标包（原 /etc/iconpack.bin、icon_*.bin、logo.bin）
  │   └── themes/         wallpapers/（系统预设壁纸）、avatars/（预设头像）
  ├── usr/
  │   ├── bin/            程序（原 /bin，84 个）
  │   ├── lib/            库（原 /lib；tcc 在 /usr/lib/tcc）
  │   └── share/          demo/、www/、sounds/
  ├── etc/  home/<用户>/Pictures/  var/vpkg/  tmp/
  ```
  你**自己放壁纸**的目录是 `/home/<用户>/Pictures`（开机自动建，"预设壁纸"按钮会把它和
  `/System/themes/wallpapers` 一起枚举）；**内核**不在这棵树上 —— 它在盘上 LBA 65..8008 的裸扇区
  （BIOS 引导读裸扇区，不认文件系统），另有一份在 ESP 分区 `EFI/BOOT/KERNEL64.BIN`。
  旧路径（`/bin`、`/Fonts`、`/Wallpapers` 等）由内核的路径映射层自动重定向并**失败回退**，
  所以旧脚本/旧夹具卷继续能跑。新旧盘并存：**想要新布局要重装**（旧盘里的文件还在旧位置）。

### 4. 桌面（UI）
- 设置里的**预设壁纸/预设头像**（v0.5.16 起真能用）：按钮显示张数，点一次循环一张并立刻持久化
  - 源目录（构建期打进卷，改完要**重新构建**才会进系统）：
    - `build64/presets/` —— 内置预设：`avatar-01..06.png`（预设头像，构建期由 `tools/make_presets.py` 生成）+ `wall-*.png`（内置示例壁纸）
    - `res/wallpapers/` —— **你自己放的壁纸**（png/jpg/jpeg/bmp；非图片按扩展名忽略）
  - 卷内落点：`/Avatars/`（头像）、`/Wallpapers/`（壁纸，两种来源合并）
  - 支持 **PNG / JPEG / BMP**，且**任意分辨率**（v0.5.17 起）：内核在**解码期抽样** ——
    PNG 用流式 inflate 逐行降采样（32 KiB 历史窗 + 2 行反滤波）、JPEG 按 DCT 块 d×d 求平均
    （d≤8，再小用一次缩放收尾）、BMP 逐行/列抽样；**内存只跟目标尺寸有关**，与源分辨率无关
  - 上限：边长 ≤ 16384、源像素 ≤ 32 MP、单文件 ≤ 24 MB（超出如实拒绝并提示）；
    打包期**不再缩图**（原图进卷，质量保住）；24 MP 级大图解码在 QEMU TCG 下约十几秒
  - 逐文件核对：`py -3 tests/presets_audit64_test.py`（秒级，不跑 QEMU）
- 设置里的**改用户名**（v0.5.16 起真落盘）：写 `/etc/users.db` 并同步 `ui.login.last`，
  重启后用户表与锁屏都是新名字；空输入会明确提示（不再静默失败）
- Windows 11 风格：Token 化设计（Rust 生成）、毛玻璃、大圆角、双层浅阴影、居中 Dock
- Dock 是**运行时动态列表**（v0.8.0）：固定项在左、运行项在右（**每窗口一项，不合并**）；固定列表持久化在 `/etc/dock.conf`（首次把默认 8 项写盘一次，之后以文件为准）；宽度 = max(默认宽度, 内容宽度)，加运行项后变宽并重新居中；右键 Dock 图标 / 开始菜单应用图标→ 上下文菜单（固定/取消固定 + 关闭窗口/关闭所有窗口），键盘 ↑↓/Enter/ESC 可用
- 窗口系统：拖动/八向缩放/最小化飞行/任务栏/Dock 窗口列表、脏矩形局部重绘
- 锁屏 → 登录（多用户、口令 SHA-256×1000+salt、头像/主题/壁纸按用户持久化）
- 启动过程（v0.9.2 重写：屏上零测试图 + 真逐行滚屏日志 + 一排 5 点波浪动画）：**① 真日志控制台**（黑底等宽字 + 行首时间戳，**每来一行就真的上滚一行**：滚屏改成后备缓冲直接字搬运，单次 ~5–10 ms，实测 [CON64] replay lines=64 ms=3840 stream=1；启动早期就挂上屏，之后每个阶段结束由 con64_live_pump64() 把这一段**真的刚产生**的行逐行滚上去）→ **② 启动动画**（纯黑底 + 正中我们的 logo + **正下方一排 5 个小圆点做波浪形依次亮起/跳跃**，dots=5 dot_r=6 gap=34 bounce=6 layout=row；每帧只重绘 logo 盒与点阵行两块矩形；时长按**真实墙钟**校准 —— [BOOT64] anim done … wall_ms=4004 rps=1944，需求 3–5 秒；TCG 下 PIT 虚拟时钟比墙钟快 ~1.9 倍，按 tick 算的 4 秒只有 2.1 秒，这正是动画瞬间闪过的真根因）→ **③ 淡出淡入**（动画末 400 ms 淡到纯黑，锁屏用 4×4 Bayer 抖动分 **8 步 ≈12.5%/步** 从黑场揭示，实测 [BOOT64] fade in steps=8 … ms=500 pattern=bayer-4x4 pct=12）→ 锁屏。★ 全程**不可跳过**（按键被吞掉，[CON64] skip disabled=1、skippable=0）★ 进锁屏前屏上**没有任何彩色测试图**：virtio-gpu 改成静默接管显示（[VGPU] scanout quiet），ring3 彩色带/共享内存矩形演示改成 fw_cfg opt-in（[FBDEMO] skipped / [EVSHM] demo skipped）
- ★ 通用 GPU 层（v0.9.4，P1 地基）：内核自己扫 PCI 显示控制器（class 0x03）、认厂商（intel / amd / nvidia / unknown）、用"写全 1 回读"量出 BAR 的真实窗口、打开 mem-decode + bus-master 并**回读确认**，再对三家各读一个**只读**身份寄存器（NVIDIA `NV_PMC_BOOT_0` / AMD `R_000000`+`MM_INDEX/MM_DATA` / Intel `PIPEA_CONF`+`PIPEA_DSL`+`DSPACNTR`），最后打一行 `[GPU] report … accel=none-not-implemented`；实测四种图形后端（`-vga std` / `virtio` / `qxl` / 双 `secondary-vga`）—— Bochs 0x1234 与 virtio 0x1af4 与 QXL 0x1b36 全部**如实降级**（`unknown=1`、`probed=0`、`skipped reason=no-safe-probe`），桌面照常 `[GUI64] ready`。★ 这一批**不含**任何模式设置（KMS）/ 显存管理 / 命令提交 / 2D-3D 加速 —— 那些是 P2(AMD) / P3(Intel) / P4(NVIDIA)；三家的原生驱动在本机**无硬件可取证**，报告里一律写"代码路径已写、真机语义未取证"
- ★ 帧缓冲 64 位化 + P3 Intel 初步驱动（v0.9.8）：**帧缓冲物理地址一路 64 位**（`BootInfo` 尾部新增 `lfb_addr_hi` / `fb_source`，UEFI loader 不再把 GOP 帧缓冲截成 32 位 —— 大显存卡在 "Above 4G Decoding" 下 BAR1 常在 4 GiB 以上，截断就是往野地址写 = 黑屏），并新增 `[FB64] fb source=<vbe|gop> addr=0x… w=… h=… pitch=… hi4g=… pt=… pt_last=… usable=…`：出帧前**只读走查 CR3 四级页表**（含 1 GiB / 2 MiB 大页），首末页每一级都要 P|RW 才允许出帧，否则 `usable=0` **一律不碰屏**（宁可停在固件画面，也不 #PF/黑屏）；P1 通用层新增 `[GPU] driver vendor=… status=…` 分派行 —— **只有 Intel 走 `status=safe-probe mode=readonly`，AMD/NVIDIA/未知厂商走 同一段 `status=none fallback=no-touch` 代码**（不写厂商寄存器、不接管 scanout）；新增 `kernel/intel_gpu64.{h,cpp}`：device id → 家族/代数（HD 530 / HD 620 / HD 630 / UHD 630 …，查不到 `family=unknown` 不猜）、`BAR2 = GTTMMADR` 的 phys/size、GTT 表项**只读**采样（两个候选偏移各 32 项，`valid=` / `alt_valid=` 都打出来）、`opt/vimtu/gpu` 的 `mode=intel_safe`（默认）| `intel` | `off`（新增字符串版 fw_cfg 开关 `fb64_fwcfg_str64()`）；`tests/gpu64_test.py` **156/156**（4 个图形后端）、`gpu64_vmware_probe` **9/9**
- ★ P3 Intel 第二阶段（v0.9.9）：Intel 显示引擎**只读**快照（`[INTEL] pipe a …` / `[INTEL] plane a …`：pipe timing、plane 使能/格式/stride/surf、已编程的 hactive/vactive）+ GTT **空闲段扫描**（`alloc=readonly scan=4096 free_run_start=… free_run_len=…`）+ 一行 `[INTEL] plan takeover=not-yet frame_match=… need=gtt-framebuffer,plane-programming` 说明接管还差什么；仍然**一个厂商寄存器都不写、一个 GTT 表项都不写**。同时把"帧缓冲映射缺失"从"只能放弃出帧"升级成**页表自补**：`fb64_remap64()` 只补**缺的** 2 MiB 恒等映射（已存在的项一律不动，写页表包在临时清 `CR0.WP` 的窗口里，只新增所以不需要 invlpg），补完**重新走查**确认，仍不可写就回到 `usable=0` 不碰屏；`[FB64] fb source=… remap=N` 报新补页数；新增 `opt/vimtu/gpu` 的 `fbtest=remap` 让启动期真跑一遍`[FB64] remap selftest … made=1 walk=rw restored=1 ok=1`（补映射→走查→拆干净并还页）；`tests/gpu64_test.py` **204/204**（5 个夹具）
- ★ GTT 映射自检 + 页表只读映射升级（v0.9.10）：`kernel/intel_gpu64.cpp` 新增**唯一会写 GTT** 的自检 `[INTEL] gtt map selftest start=… pages=4 write=1 readback=1 restored=1 ok=1` —— 只写**当前无效**的表项，写完逐项回读校验、立刻写回无效并还页，**只在 `mode=intel` 下跑**（默认 `intel_safe` 只读），不接管 scanout；`kernel/fb.cpp` 新增 `fb64_upgrade_ro64()`：**映射在但只读**时把 RW 位打开（先校验恒等映射，1 GiB 大页如实跳过，改已存在的页所以每页跟 `invlpg`），`[FB64] fb source=… upgrade=N` 报升级页数；`fbtest=remap` 自检里把新建项**故意改成只读**再升级，于是 `ro_walk=ro upgrade=1 rw_walk=rw` 在 QEMU 里真跑；`tests/gpu64_test.py` **212/212**（5 个夹具）
- ★ **AMD 多核启动修复**（v0.9.11）：AMD+VMware 下 `-smp 2` 以上直接三重故障复位的根因不是"APIC 地址写死"（MADT/LAPIC 地址一直是读表来的），而是**发 INIT 前没刷缓存**：AP 出 INIT 时 `CR0.CD=1`，它读的是内存，BSP 的跳板/共享块/GDT 拷贝源还在缓存里 → AP 执行 0x8000 的**旧字节**（SeaBIOS 跳板区残留）→ 实模式无 IDT → 三重故障复位；新增 `smp64_flush_trampoline64()`（`mfence`+`clflush` 全页+`wbinvd`+`mfence`）并在"**拷完跳板**与**每次写共享块后**”都调用；另修三处：`out 0x92` 先清 bit0（bit0=fast reset，读回可能已是 1）、INIT 后 10ms 改用 **TSC 下限**延时（原来只看 `g_ticks64`，IF=0 时实际可能远短）、SIPI 改成**重发循环**（最多 5 次，每次后查在线标志，并等 ICR 投递完成）；另加 **AP 进度面包屑**（`stage=0/1/2/3`）与
  `[SMP] cpu vendor=<AuthenticAMD|GenuineIntel>` 打点；`tests/smp64_test.py` **52/52**（含 `-smp 2` / `-smp 4`）
- ★ **DRM/KMS + evdev 内核门面**（v0.9.12）：ring3 现在能 `open("/dev/dri/card0")` 与 `open("/dev/input/event0|1")`（两个**设备特判**，与 `/dev/console` 同一套做法：不查盘、不建文件），用既有的 `ioctl(16)` 枚举显示模式（`VERSION` / `GET_CAP` / `GETRESOURCES` / `GETCRTC` / `GETCONNECTOR`）、建一块 **CPU 可写**的 dumb buffer、`PAGE_FLIP` 上屏、`WAIT_VBLANK` 等一次回扫；**dumb buffer 的 ring3 可写性**靠 `MAP_DUMB` 把内核那批物理页逐页挂进**调用方页表**（窗口 = A1 帧缓冲映射区 `USER64_FB_VA64` 后半段 4×8 MiB，在 PDPT[5]、不参与进程回收），**不新增任何 syscall 号**；**混合呈现** = ring3 光栅化 + 内核只做传输与扫描输出（`vgpu64_blit64` → `path=gpu`，无设备则 `fb_soft_flip_region64` → `path=cpu`）；`input_event` 24 B 与 Linux 同尺寸、键鼠分流的非本设备事件**不静默丢**；启动期 `[DRM] selftest … ok=1`（自检失败不算启动失败，且会把开机画面翻回来），ring3 验收程序 `/bin/drmdemo.elf`（opt-in `/etc/drm64_probe`）打完整 `[DRMDEMO]` 阶段行 + `badcmd rc=-25` 反例；附带 `docs/Wayland基础抽象层.md`（四条内核态逐条对照"本批做了什么/还差什么才能跑 wlroots"）与 `tests/drm64_test.py`；**不含** libdrm/libinput/libwayland/wlroots/EGL/GBM
- 开始菜单：固定网格 + 全部应用列表 + **搜索（含用户新装的应用）** + 数字快捷键 1..8
- 应用注册表 `/etc/apps.json`：装完的应用自动进桌面/开始菜单，图标三层优先（包内 PNG → 系统图标包 → 程序化兜底）
- 设置：主题/壁纸/头像/字号/鼠标灵敏度/缩放/声音/Dock 尺寸…约 40 个键持久化（store A/B 双槽 + 世代号）
- ★ 帧时间（v0.8.0 实测，QEMU TCG）：空闲 **1 帧/s、单帧 5~7 ms**（修前 13~17 帧/s、单帧 55~60 ms —— Dock 让位动画永远收敛不到 0，每帧重画 Dock）；脏矩形是**最多 4 块的队列**（修前只有一个包围盒，开始菜单时间 + Dock 时钟会在同一秒合成 824×491 的巨块 = 一次 490 ms）；Dock / 开始菜单在脏区不相交时整段跳过。开始菜单**整块**重绘在 TCG 下仍 ≈280 ms/帧（400×420 亚克力 + 22 图标 + 155 字形，纯填充率）

### 5. 应用（全部用户态）
终端（`/bin/shell.bin`）、文件管理器、文本编辑器、计算器、扫雷、任务管理器、系统监视器、
设置、关于、回收站、**应用商店 + 包管理器**（`.deb` 与自有 VAP64）。

### 6. 终端渲染（v0.4.7 起按"三无原则"）
- **无抗锯齿**：掩码覆盖率 ≥ 50% 就画纯前景色，屏幕上只有前景/背景两色，没有灰色过渡
- **无缩放**：字号固定整数 px（默认 16），不跟随 UI 字号档
- **无次像素**：整字形当单色掩码贴屏，不做 RGB 分量位移
- 网格：ASCII 8px / 汉字 16px（**中英 2:1**），行高取字体度量最大值（不裁 `g` 的下半截）
- 颜色：背景纯黑 `#000000`、正文纯白 `#FFFFFF`、**标准 ANSI 16 色**（`ESC[…m`，含亮青 `#00FFFF` / 亮红 `#FF0000`），全部 100% 不透明
- 字体：Sarasa Mono SC（终端等宽）+ Unifont（缺字兜底）—— 构建期子集化，许可原文在 `docs/fonts/`
- ★ **自动跟到底部（v0.5.9）**：正文超过一屏时一直滚到最后一行（`[TERM64] dmesg follow bottom=1`）；
  终端窗口默认高度按 **Dock 顶边**收口（`[UI] term fit above dock client_h=670 rows=37 win_bottom=712 dock_top=724`），
  否则最底两行会被 Dock 盖住。**边界**：没有回看缓冲（scrollback）—— 只能看到最后一屏，`dmesg` 的正文上限就是窗口行数。
- ★ **终端汉字不会"缺字/缺笔画"（v0.5.12）**：终端按码点切换单色阈值 —— ASCII 用 128（覆盖率 50%）、
  **汉字用 96（37.5%）**。中文面是 Noto Sans SC（比例字体、笔画细），16px 下 1px 笔画覆盖率常只
  30~45%，阈值 128 会把整条横/竖/撇捺判死（看起来就是缺笔画）。实测 banner 第二行 6 个汉字：
  墨点 **312 → 423**（+36%）、笔画断裂（4-连通分量）**80 → 53**（-34%）；仍是纯黑/纯白二值，
  `[FONT64] selftest PASS`、中英 1:2 不变。验收：`tests/termcjk64_test.py`（17/17）。
  （**没有**把汉字塞进等宽面：实测体积 30 KB → 1.06 MB 会越过内核原始区闸门，且屏幕上字形错位 ——
  细节记在 `_subset_fonts.py` 的结论注释里。）

**UI 侧（非终端）另一套策略**：要平滑、要厚、要够黑。v0.4.8 起由最小 fontconfig 管，
`/etc/fonts.conf`（`key = value`）里的 `ui.stem`（笔画加厚百分比，默认 120）与 `ui.antialias`（on/off）
在启动时作用到抗锯齿路径；实测设置页正文对比度 **16.46:1**、次要文字 **9.41:1**（WCAG AAA 是 7:1）。
**v0.4.9 起它是真的可配置**：镜像构建期就预置一份 `0666` 的 `/etc/fonts.conf`（`tools/store_pack_win.py`），

所以内核启动打的是 `[FONTCFG] applied src=file keys=7 stem=120`（不是内置默认），普通会话也改得动。
实测 A/B（只改这一行 `ui.stem` 120 -> 160，同一块设置页客户区）：笔画核心像素 **0.35% -> 0.64%**，
正文对比度仍是 16.46:1 —— 见 `tests/fontq64_test.py`（17/17 PASS）。
★ 如实边界：不是真 fontconfig（无 `<match>/<test>/<edit>` 规则引擎、无字体扫描/枚举，只能在内嵌 4 个面里选）；
改配置要**重启生效**（没有 inotify / 运行期重载）；裸 `system.img`（不带 VimtuFS2 卷）上仍然是内置默认 +
`src=default(seed-denied)`，因为那里没有可写的文件系统。

### 7. Linux 兼容（syscall 指令路径）
覆盖 **56 个调用号**，其中真实现包括：`read/write/open/close/stat/fstat/lseek/mmap/mprotect/munmap/brk/
ioctl/readv/writev/access/pipe(sleep 阻塞)/dup/dup2/nanosleep/getpid/fork/execve/wait4/kill/uname/fcntl/
rename/mkdir/rmdir/unlink/getcwd/clock_gettime/exit_group/openat/getrandom/statfs/fstatfs/arch_prctl/

**getdents64(217)**/setuid/setgid/getuid/geteuid/getgid/getppid/set_tid_address/readlink`，
并在 `write` 路径上按 Linux 语义投递 **SIGPIPE**（管道读端全关 -> `-EPIPE` + 默认动作退出 141），
并新增**最小 `/proc`**（`/proc/version`、`/proc/self/{status,cmdline,exe}`）。
已跑通：musl 静态程序、自有动态链接器、Lua、TCC、gzip、tar、make。
 边界（重要）：**socket 家族（41-44）未实现**、`clone` 线程不支持、无 futex、glibc 二进制未验证 —— 详见第二节。
  （★ v0.4.9 更正：这里原来写着"信号只记录不投递"，那是 v0.4.4 之前的旧状态；信号从 A4-5 起就是**真投递**
   —— 用户栈信号帧 + `rt_sigreturn`，v0.4.8 起管道 EPIPE 也投 `SIGPIPE`，v0.4.9 有了端到端验收
   `tests/sigpipe64_test.py`（`[SIG64] raise … sig=13` + `default action … exit=141`，12/12 PASS）。）

### 8. 浏览器（v0.5.0 起）

`/bin/browser` 是一个**ring3 的真窗口浏览器**：它用 `shm_create` + `wl_surface` 拿一块
**896x580** 的缓冲（v0.5.0 把 shm 单对象上限从 64 KiB 提到 **2 MiB**，否则连这个尺寸都放不下），
用自研的 HTML 子集解析器把文档压成块，再用 `user/lib` 的 TrueType 栈（Noto Sans + Noto Sans SC +
Unifont 兜底）排版折行、逐行绘制；输入走 `wl_display_dispatch` 的 seat 事件。

- 能渲染：`h1/h2/h3` `p` `div` `br` `hr` `ul/li` `pre` `a` `b/strong` `u` `center` `title`，
  HTML 实体（`&lt; &gt; &amp; &quot; &nbsp;` 与数字实体），内联 `style` 的
  `color / background-color / text-align / font-size / font-weight`
- 能读：`/www/index.html`（发行版起始页，构建期用 `tools/browser_pack_win.py` 装进卷）
- 能操作：**点击页面下半 = 下翻一屏、上半 = 上翻一屏**；**ESC 退出**；PageUp / PageDown / ↑ / ↓ / 鼠标滚轮（需要键盘焦点，浏览器启动时用 `wl_seat_get()` 申请）
- **能上网（v0.5.2）**：`run /bin/browser http://<host>[:port][/path]` —— 走内核 `net_raw`（自有 ABI 号 53）自己实现 ARP / DNS(A) / TCP 客户端与 `HTTP/1.0 GET`（`user/browser/net.c`，882 行，`.text` 8,496 B）。QEMU 用户网络下宿主别名是 `10.0.2.2`：`tests/browser_http64_test.py` 起一个宿主 `http.server`，客人抓回来的 `status=200 bytes=300` 与宿主页**逐字节一致**，屏幕上墨迹 1.17%；不可达端口如实 `[BNET] FAIL reason=connect step=3` 并显示内置错误页（**不假装成功**）。
- **还不能**：**HTTPS**（没有 TLS，`https://` 直接拒绝）、图片、表格、CSS 选择器/层叠、
  JavaScript、链接点击跳转（只标出颜色与下划线）、文本选择/输入框、表单提交。
- 网络栈的如实边界：静态地址（10.0.2.15/24、网关 10.0.2.2、DNS 10.0.2.3，**不跑 DHCP**）、
  IPv4 不分片、TCP 无重排序/无拥塞控制（固定 RTO、窗口 1024）、DNS 只做 A 记录、
  `Content-Length` 不看（正文 = 空行之后到 FIN）、同一时刻只有一条连接。
  QEMU 内置 DNS（10.0.2.3）在宿主解析不了域名时会**静默丢包**，所以用域名可能 `reason=dns`；
  用 IP 字面量一定能通（实测就是走 IP 字面量）。

验收：`tests/browser64_test.py` —— **16/16 PASS**（白底 91.1% / 墨迹 2.94% / 链接蓝 632 点 / 664 色；
点击翻页后页面区差异 21,023 点；ESC 退出 `reason=esc`）。
★ v0.5.1 修的一件事（**是应用自己的 bug，不是内核缺能力**）：v0.5.0 的浏览器没调 `wl_seat_get()`，
而内核规则是"键盘只投给焦点进程、鼠标只投给指针捕获者"（`kernel/input64.h`），所以它画得出来却
收不到任何点击/按键。补上这一行后焦点与捕获就位（`[EV64] focus pid=.. reason=request`、
`[EV64] attach .. focus=.. capture=..`），点击翻页与 ESC 都实测通过。
如实边界：窗口是**屏幕居中固定位置**，还不能拖动/缩放；终端**内建** `run` 没有进程上下文（见第二节），
所以要用 `/bin/shell` 里 `run /bin/browser` 启动。

### 9. 办公套件（v0.5.3 起）

`/bin/office` 是**三合一**的 ring3 办公套件（一个进程、一套窗口外壳，复用 `/bin/browser` 验证过的
shm + wl_surface + `wl_seat_get()` + font64 排版）：

- **文档**：纯文本行编辑（行缓冲 + 行索引、UTF-8 按字节移动但插入/删除对齐字符边界），
  方向键/PageUp/PageDown/Home 移动，直接敲字插入、Backspace 删除；
- **表格**：8 列 × 20 行的**定点整数**网格，公式引擎在 `user/office/calc.c`（独立验收）：
  `=1+2*3`、`=A1+B1*2`、`=SUM(A1:A3)`、括号与一元负号；**环引用按拓扑检测**并显示 `#CIRC`
  （另有 `#DIV0/#SYNTAX/#REF/#OVER`）；Tab/方向键移动、Enter 下移；
- **演示**：3 页 × 8 行（标题 + 要点），左右键切换，`F5` 进入**放映**（白底大字、点击/空格翻页、ESC 返回）；
- **保存**：`Ctrl+S` 手动保存；**切模式（F2 循环）与退出时自动落盘** —— 文件是自定义纯文本
  （`/tmp/doc.vwd`、`/tmp/sheet.vts`、`/tmp/deck.vpt`），不兼容任何外部格式。

实测（`tests/office64_test.py` **15/15 PASS**）：窗口 896×580 `map=0`；文档打字到 `col=14`；
切模式时 `[OFFICE] save mode=doc bytes=263 rc=263`，**宿主侧独立解析卷**读回 `/tmp/doc.vwd`
含敲进去的标记串；表格 `=1+2*3 -> value=7 err=0`、`=A1+1 -> value=#CIRC err=-2`；
编辑区墨迹 3.15%；ESC 退出 `reason=esc`。

如实边界：文档**没有富文本/图片/表格内嵌/输入法**（键盘只给 ASCII）；表格**没有小数、函数库、
跨表引用**，除法是整数除法；演示只有 3 页固定文本；`.vwd/.vts/.vpt` 都是自己的格式。

### 10. 应用注册表里的浏览器与办公套件（v0.5.4 起）

`/bin/browser` 与 `/bin/office` 都进了**应用注册表**（`kernel/app64.cpp` 的 `kBuiltinApps64`）：
桌面图标与开始菜单里能看到「浏览器」「办公套件」，点开走 `fork+execve` **真进程**
（所以它们的 `shm` / `wl_surface` / `wl_seat_get()` 都能用），与 `/bin/store`、`/bin/edit` 同一条路径。
开始菜单的固定网格是 2×4=**8 格**，排序为"卷程序 → 用户安装的 → 内置窗口"，所以这两项就在**第 4、5 格**（实测 `[START64] grid apps=14 cells=8 /bin/store | /bin/edit | /bin/netd | /bin/browser | /bin/office | …`）。

★ 为什么它们是**预装应用**而不是"商店里可安装的包"（如实）：`.vap64` 的载荷上限是 **32 KiB**
（`tools/store_pack_win.py` 的 `build_repo()` 里硬断言 `32768 + 64`），而 `browser.elf` **60,816 B**、
`office.elf` **49,712 B** 都超了。要把它们做成"可下载包"，得先把 `.vap64` 的装载缓冲放大
（`kernel/app64.h` + `vs_pkg.c` + `make_vap.py`）—— 这是下一批的事。

实测（`tests/apps64_test.py`）：注册表打点 `[APPREG64] app … exec=/bin/browser` /
`… exec=/bin/office`，`entries=… builtin=**14**`（原 12 + 2），两条都是 `builtin=1 desktop=1`；
宿主侧独立解析卷确认 `/bin/browser`、`/bin/office` 都是 ELF 且在盘上。

### 11. 其他
- 原生 PNG/JPEG（基线+扩展顺序）解码；内置图标包（30 个系统图标 + 应用图标）
- 系统音效（4 段自合成素材 + `/bin/sounder` 用户态播放器，音频 ABI 49）
- ring3 网络栈：`/bin/netd`（DHCP / DNS / TCP / UDP / ARP / ICMP，e1000 驱动在 ring0）
- VSync + 双/三缓冲（`[VSYNC]` 打点、撕裂像素测量）
- 硬件检查报告（设置页"关于"里可看；**启动时不再挡屏**，直接滚动英文 boot log）

---

## 二、它明确**做不到**什么（诚实边界）

这一节比上一节更重要：下列每一条都是**当前代码里确实没有**的东西，别当成"大概支持"。

### 内核 / 进程
- ⚠️ **信号**：投递是**真实现**（用户栈信号帧 + `rt_sigreturn`；用户态 #PF/#GP/#DE 只杀进程、不 PANIC；v0.4.8 起管道 EPIPE 也投 **SIGPIPE**）；边界 = 无 `siginfo`/`sigaltstack`/`SA_RESTART`/实时信号排队/作业控制
- ⚠️ **终端内建 `run` 没有进程上下文**（v0.4.9 实测确认，未修）：终端里 `run /path.elf` 走
  `kernel/elf64.cpp` 的 `user64_enter_at64`（"当前任务直接进 ring3"的同步模型），**不创建 `Proc64`**。
  后果：`p64_current64() == nullptr` —— fd 表退回内核表（`pipe`/读写照常可用），但**信号状态不存在**，
  所以该程序收不到任何信号（管道读端全关时 `write` 仍正确返回 `-EPIPE`，可 `SIGPIPE` 没有任何
  pending 位可置，实测 `/epipe.elf` 打出 `SURVIVED`）。**真进程**路径没有这个问题：`/bin/shell`
  的 `run PATH` 走 `fork(57)+execve(59)+wait4(61)`，子进程有独立 `Proc64`，
  实测 `[SIG64] raise pid=28 sig=13 src=pipe-epipe` -> `[SIG64] default action pid=28 sig=13 exit=141`。
  想跑出完整 Linux 语义，就在 shell 里 `run`，不要用终端内建的那条。
- ⚠️ **线程**：`clone` 只支持 fork 语义，`CLONE_VM/THREAD` → `-ENOSYS`。futex(202) 从 v0.7.1 起**有**
  WAIT/WAKE，但实现是"轮询式等待"（本内核没有等待队列，1 ms 重查一次）—— 语义对、有界超时，
  代价是唤醒最多慢 1 ms；**并发与优先级继承未验证**。所以 pthread 还是跑不起来（缺 `CLONE_THREAD`）
- ❌ **swap / 页换出**：内存不足直接 `-ENOMEM`
- ❌ **COW**：`fork` 整页物理复制
- ❌ **ASLR**：映像地址确定
- ❌ 内核模块加载（单体内核）
- ⚠️ UEFI 路径下无地址空间隔离（`fork/execve/wait4` 全 `-ENOSYS`，串口如实标注）
- ✅ **v0.7.6 修的真缺陷：任务 exit 会跳飞成 `#UD rip=0x47`**（v0.7.1~v0.7.5 一直存在，
  v0.7.5 的发版检查才拦下）。根因是**任务内核栈几何**：16 KB 栈里 SYSCALL 处理器只有
  `0x2000-0x1000-0xD0 = 0xF30`（≈3.9 KB），而 `execve -> VFS -> 磁盘读`（`dev_read_at -> ata_wait_*`）
  实测压到 `stack_top-0x22A0`（处理器侧 4.5 KB），正好盖住"从 ring3 回来"的返回地址槽
  —— gdb 写观察点抓到 `push` 把它写成 `0x40`，`ret` 弹出去就跳飞。
  修法：① 每任务栈 **16 KB -> 32 KB**、入口预留 **0x2000 -> 0x4000**（处理器可用深度 12,080 B，
  入口侧 C 栈仍有 16 KB）；② 蹦床 `user64_resume_tramp64` 改成 **jmp 到静态保存区里的返回地址**
  （不再从可能被压穿的内核栈 `ret`）；③ 退出前打一行
  `[USER64] exit resume rsp=.. ret=.. verdict=ok|bad`，不是内核代码地址就不回蹦床。
  实测：`tests/taskexit64_test.py` **6/6 PASS**、`verdict=ok` **23/23** 条、
  `[PROC64] exit pid=4 code=0` + `wait4 pid=4 status=0`（修前是 `wait4 TIMEOUT pid=4`、
  用户看到 `pid=4294967285` = `-EAGAIN`），带卷启动第一次真正进桌面；内核 +720 B。

### Linux 兼容
- ⚠️ **socket**（v0.7.4）：`socket/connect/sendto/recvfrom/send/recv/shutdown/getsockname/getpeername`
  已实现，TCP 走三次握手 + PSH/ACK 分片（1460 B）、`close` 发 FIN，UDP 收发可用；
  **`accept`(43) / `bind`(49) / `listen`(50) 如实返回 `-EOPNOTSUPP`**（没有服务端）。
  ★ 如实边界：**从未真连过一次网络** —— 只有纯逻辑自检
  `[SOCK64] selftest PASS (client-only: TCP connect/send/recv/close + UDP; no listen/accept)`。
  另外 socket 句柄是 `200+idx`、**不在 fd 表里**，所以 `poll/select`/`epoll` 看不到它。
- ✅ **poll/select 家族**（v0.7.1）：`poll(7)`/`ppoll(271)`/`pselect6(270)`/`select(23)`（`nfds<=64`）+
  `epoll_create1/ctl/wait`（291/233/232）—— 就绪判断实测
  `[VFS64] poll-ready: file=IN|OUT ok badfd=NVAL pipe=IN/OUT/HUP ok`、`epoll=IN+data ok`。
- ✅ **符号链接**（v0.7.0）：`symlink(88)`/`readlink(89)` 可用（`VFS64_TYPE_LINK`，只跟随**最后一级**、
  最多 8 层，目标 <=511 B），实测 `[VFS64] symlink: create/readlink/follow PASS`；
  **`link(86)` 硬链接仍 `-EPERM`**（VimtuFS2 的目录项内嵌在 inode 里，要支持得升卷格式）。
- ✅ **dpkg / apt / vpkg —— 全系统只有**一份** `.deb` 引擎**（★ v0.9.42 / L1）：
  `/usr/bin/dpkg` 与 `/usr/bin/apt` 就是 **`/bin/vpkg` 的同一份字节**（造卷时把同一个 ELF 写到三个路径），
  程序按 `argv[0]` 的 basename 决定前端（busybox 模式）。`dpkg -i/-l/-r/-s/--version`、
  `apt install ./X.deb|update|list|remove` 全部翻译成 `vs_install/vs_list/vs_remove/vs_info`。
  **vpkg 模式的所有日志行逐字节不变**（测试契约不破）。
  引擎能力：`ar` + `control.tar.{gz,**xz**}` + `data.tar.{gz,**xz**}`（xz 是**真 LZMA2 解码**，
  单块 + CRC32/none）、sha256 校验、空间预检、一层依赖 + `/etc/vpkg/provides`（每行必须带见证路径）、
  每包 `files[]` 清单（卸载**真删文件**）。
  如实边界：带 `preinst/postinst` 的包**整包拒绝**（`VS_E_SCRIPTS`）；deb 里的**符号链接条目**拒绝；
  多块 xz / BCJ / Delta 过滤器拒绝；依赖只比名字、不递归、无版本区间。
  旧的那套独立实现（`user/dpkg/dpkg.c`，只认 gzip、靠外部 `tar` 解包、不管依赖、卸载只删记录）
  已废弃、不再编译、不进卷。
  ★ **v0.7.7 实测状态（如实，逐条）**：`tests/dpkg64_test.py` **6/6 PASS** —— `dpkg --version`、
  **`dpkg -i <真 .deb>` 装成功**（ar 解包 + control.tar.gz 解出 -> 读 `./control` -> data.tar.gz 解到 `/`
  -> 追写 `/var/lib/dpkg/status`）、payload 真落地（终端 `cat` 读到 **47 B**）、`dpkg -l` 从状态文件读回记录、
  `apt update` 如实报“当前未启用”。安装需要 root：终端先 `sudo -i`（P4 权限检查会如实拒绝
  uid=1000 建 `/usr/share/...` —— 这是 POSIX 语义，不是缺陷）。
  ★ 性能边界（如实，v0.7.9 实测）：pipe 容量 4096 B（POSIX PIPE_BUF）；v0.7.9 把“磁盘读写变快”
  后，`dpkg -i` 从 v0.7.7 的 25-30 s、v0.7.8 的 20.5 s 降到 **~1 s**（同一份夹具、同一台宿主；整段
  `dpkg -i` 区间里只有 2 个 `kheart beat`）。原因见下面“磁盘 I/O”一节。
- ✅ **真发行版二进制能跑**（★ v0.9.42 / P0）：`PT_INTERP` 动态映像 + **PIE / load bias** +
  **按段分块读盘（16 KiB/次）** 都通了。实测 Debian bookworm 的 `/usr/bin/hello`（PIE + glibc）
  `run` 起来**打印 `Hello, world!` 且退出码 0**；musl 的 `ld-musl-x86_64.so.1`（873,792 B）与
  glibc 的 `ld-linux-x86-64.so.2`（215,000 B）都能被内核装载。
  验收：`tests/linuxapp64_test.py` **36/36**（静态/动态 musl、真 glibc、39 字符长路径、4 个真 .deb）。
  ⚠️ 仍缺（如实）：`/proc` 内容不完整、无 pty、无 vDSO、TLS 只到能启动 glibc 的程度 ——
  所以**不是**"随便哪个发行版程序都能跑"，实测覆盖面见上面那 36 条。
- ✅ **Windows PE：只读体检器 `/bin/peinfo`**（★ v0.9.43 / W0）—— **Windows 生态的第一步，但还不是加载器**。
  ring3 静态 ELF64（22,016 B，自足不链 libc，文件缓冲走 `mmap(9)`），打印 DOS/NT/可选头
  （**PE32 与 PE32+ 两版**）/节表/**导入表逐条**/导出/重定位/TLS/资源目录/CLR/Subsystem，
  并输出一行 **`union` 导入并集**。**不映射节、不做重定位、不填 IAT、不执行任何字节**。
  用法：`run /bin/peinfo /pexcorpus/gui.exe`
  验收：`tests/peinfo64_test.py` **27/27** —— 三份 **mingw-w64 现场编的真 PE**
  （`/pexcorpus/{tiny,cui,gui}.exe`）的头字段/每个节/每条导入/summary/`union`
  与宿主侧独立实现 `tools/pe_crosscheck.py` **逐项一致**；反例（对 ELF）`reject reason=dos` 且退出码 1。
  ★ **它量出来的"Windows API 极简真相"**（推翻了先前凭印象列的清单）：
  * 一个 GUI 程序（`MessageBoxA` + `-mwindows`）**只导入 `USER32.dll` 的 1 个函数**，**没有 gdi32**；
  * CRT 走**老式 `msvcrt.dll`**（`__getmainargs`/`_initterm`/`_iob_func`/`__set_app_type` 那套），**不是 UCRT**；
  * `KERNEL32` 只要 **5~7 个**函数（`GetLastError`/`SetUnhandledExceptionFilter`/`Sleep`/
    `VirtualProtect`/`VirtualQuery`，GUI 再加 `GetStartupInfoA`/`IsDBCSLeadByte`），
    不是三十多个 A/W 双份 —— 且这一批图形 exe **只用 A 版**。
- ✅ **Windows PE：装载器 `/bin/peload`**（★ v0.9.44 / W1）—— **VimtuOS 现在真的会执行原生 Windows PE 机器码**。
  ring3 静态 ELF64（26,768 B），做的事：解析 PE32+ → `mmap` 映像 → 逐节拷贝 → **`IMAGE_REL_BASED_DIR64` 重定位**
  → **填 IAT**（`ExitProcess`/`GetProcAddress`/`LoadLibraryA` 三条桩，经 **Win64→SysV 汇编跳板**：PE 的参数在
  `rcx/rdx` + 32 B 影子空间，我们的 C 在 `rdi/rsi`，直接填 C 函数地址会「看起来生效但退出码随机」）
  → 按节 `mprotect`（先 RW 写内容、再 `r-x` 收紧，**W^X 不做例外**）→ **直接 `call` `AddressOfEntryPoint`** 并拿回返回值。
  **内核 0 行改动**（`kernel64_os.bin` 3,298,432 B 逐字节不变）—— PE 装载纯粹是用户态的事。
  用法：`run /bin/peload /pexcorpus/reloc.exe`
  验收：`tests/peload64_test.py` **23/23 PASS** ——
  * `tiny.exe`（无导入无重定位）→ `[PELOAD] ok … entry_ret=0`；
  * **`reloc.exe` → `entry_ret=42`** —— `.data` 里存着一个绝对指针 `g_self = &g_self`（一条 DIR64），
    装载器没做重定位它会返回 1，做对了才 42（`delta=0xffffffffc0890000` / `applied=1` / `types=0xa`）；
  * `exit.exe`（只导入 `kernel32!ExitProcess`）→ IAT 里填进我们的假桩，`ExitProcess(7)` 让进程**真以 7 退出**；
  * `cui.exe` → **如实失败**：打印 28 条 `[PELOAD] import MISSING`（kernel32 5 + msvcrt 23），退出码 1，
    **绝不带着空 IAT 跳进去**（那是 W2 的输入清单）；
  * 对 ELF 反例 → `reject reason=dos`；全程零 PANIC/零 TRUNC。
  证据截图（QEMU `screendump` 读客人帧缓冲，非宿主窗口）：`docs/w1_peload_reloc.png`、`docs/w1_peload_exitstub.png`。
- ✅ **Windows PE：控制台 exe 真的跑起来了**（★ v0.9.45 / W2）—— `run /bin/peload /pexcorpus/cui.exe`
  在 VimtuOS 终端里打印出它自己 `printf` 的 **"hello from a real Windows console exe"**，退出码 **0**。
  新增 ring3 文件 `user/win/peload_crt.c`（Windows 运行期）：**TEB/PEB**（`%gs:0x30` / `%gs:0x60`，
  TEB 0x400 B + PEB 0x400 B，PEB+0x10 = ImageBaseAddress）+ `kernel32` **10 条** +
  **老式 `msvcrt` 20 个函数 + 4 个数据导入**（共 34 条桩：`__getmainargs`/`_initterm`/`__iob_func` 这套 1998 版 ABI，
  不是 UCRT），外加一个最小的 `vfprintf` 格式化引擎（`%s %c %d %u %x %X %p %%` + 宽度/补零）。
  **内核只 +480 B**：新增私有 syscall **1037/1038 = `IA32_GS_BASE`**（与 158/`ARCH_SET_FS` 同构；
  本内核不用 `swapgs`、没有 per-CPU gs，所以这个 MSR 空闲）。
  ★ **踩到并修掉两个 ABI 陷阱**（都属"看起来正常"型）：① Win64 参数在 `rcx/rdx/r8/r9` + 32 B 影子空间；
  ② **Win64 的 `rdi`/`rsi` 是 callee-saved，SysV 的是 volatile** —— 第一次跑就崩在 mingw `__tmainCRTStartup`
  的 argv 复制循环（`call strlen` 回来后 `rsi` 被踩成 0x100200 → 写到 `0x10109BFF8` → `#PF` → 139）。
  修法：每条跳板 `push/pop %rsi,%rdi` + 构建期**字节级门禁**（`56 57 48 89 cf` 计数 ≥ 28）。
  验收：`tests/peload64_test.py` **34/34** —— 另加**自证语料** `gsprobe.exe`（纯汇编读 `%gs:0x30`/`[0x60]`，
  检查 TEB+0x30 指回自己、`PEB+0x10` 处是 'MZ'、`ImageBase+0x1000` == 入口地址，四条全对才返回 42）；
  `gui.exe` 仍**如实失败**（只缺 `USER32.dll!MessageBoxA`）。
  证据截图：`docs/w2_cui_printf.png`、`docs/w2_gsprobe.png`（QEMU `screendump` 读客人显存）。
- ✅ **Windows PE：GUI 程序弹出真对话框**（★ v0.9.46 / W3 第一批）—— `run /bin/peload /pexcorpus/gui.exe`
  在 VimtuOS 桌面上弹出一个 **300×150 的原生对话框**：蓝色标题栏写 `vimtu-peinfo`、正文是
  `hello from a real Windows GUI exe`、下面一个 `OK` 按钮。
  `MessageBoxA` 走的是**本系统自己的窗口抽象**：`shm_create(13)`+`shm_map(14)` 拿画布 →
  `wl_surface_create(15)` → 直接画像素（8×8 位图字体，`tools/gen_font8.py` 生成 760 B 表）→
  `attach(16)`+`damage(17)`+`commit(18)` → `wl_display_dispatch(21)` 收 seat 事件 → `destroy(19)`。
  **`gdi32` 一个函数都没用上** —— 这正是 W0 用真 exe 量出来的结论（一个 GUI 程序只导入 `USER32!MessageBoxA`）。
  ★ 两个坑：私有 syscall 走 **`int 0x80`**（不是 `syscall` 指令，混了会静默错）；
  必须先 `wl_seat_get`(20) **申请指针捕获**，否则鼠标事件根本不进本进程的队列。
  验收：`tests/peload64_test.py` **38/38 + 1 SKIP(env)** —— caption/text 与 PE 里的字符串逐字符一致、
  `commit rc=0`、**内核日志 `[WL64] blit … dst=964,532 bytes=180000 clip=ok`**、关闭后 `exit code=0`。
  截图：`docs/w3_msgbox.png`（客人显存）+ `docs/w3_msgbox_window.png`（1:1 裁剪），并做了**逐像素核验**
  （外框 300/300 与 150/150、标题栏 6,119/6,258、正文黑字 349 px、OK 按钮 1,601/1,824 且含 27 px 黑字）。
  ⚠️ 仍缺（如实）：对话框只能落在 **320×200 的内核面板**里（`wl64.cpp:323` 拒绝更大的 surface），
  **`/bin/wm` 升级成全屏多窗口合成器还没做**；`gdi32`、标准控件、`DialogBox`、真 `CreateWindowExA` 那批全部未做；
  「鼠标点 OK」这条判据记 `SKIP(env)`（宿主 1366×768 + 1.7× 相对指针无法把光标可靠送到按钮上）。
  ❌ **还没有 PE 加载器**：不能执行任何 `.exe`（W1 才是"映射节 + DIR64 重定位 + 跳到 EntryPoint"）。
  资源管理器双击 `.exe` 仍如实报"无法运行 Windows 可执行文件"。
- ❌ 部分：`lstat`、`getrlimit/setrlimit`（无配额）

### 文件系统
- ❌ 日志（journaling）/ 快照 / 配额 / ACL（符号链接 v0.7.0 起**已有**，见上面"Linux 兼容"）
- ⚠️ **ext4 只读**（v0.9.40）：能列目录、能读文件，**不能写**；**没有 STATFS**，所以 ext4 卷的容量
  如实显示"未知"（不拿分区大小冒充）。解析在 ring-3 服务里，服务挂了读操作会如实报错（不假装成功）。
- ⚠️ **单通道单请求**：内核 ↔ 服务的反向 RPC 一次只有一条请求、每次 ≤ 4 KiB，没有并发也没有缓存
  （100000 B 的文件 = 25 次 RPC，实测每次 40~84 ms）。
- ❌ NTFS（读不了 Windows 系统盘）
- ⚠️ 写文件是"整文件覆盖 + 立刻落盘"，单文件上限 67584 B（fd 层）
- ✅ **已修（v0.9.41）：重启延迟与静默坏读 —— 三层"并发互踩"一起拔掉**。v0.9.40 记的"已知待优化"
  （`drv64_test` 45/48、`lat_ms` 偶发 4860~4896 ms）根因不是"盘慢"，而是同一时刻有两件事在动同一份**没有保护**的共享资源。
  顺着串口证据一层层挖出三层，每层都有实测证据、都有对应的锁：
  ① **AHCI 命令**（命令列表 slot 0 / 命令表 / DMA 暂存页池 `g_dma[16]` 全驱动共享）：两条命令重叠会**静默坏读**
     （读回上一笔内容 → `[ELF64] diag magic … same=0`）并互相 `port_restart` 掐死对方，各自吃满 1 s 命令超时。
     → `Ahci64IoSpan`，五个发命令的 entry point 全进临界区。
  ② **VFS 的文件级 scratch**（`g_sec` / `g_l1..g_l3` / inode 窗口缓存 / `g_newblk` …）：命令不重叠了，
     scratch 还在互踩 → 间接块读成垃圾块号（`[VFS64] BAD block=1572358150`）。→ `Vfs64IoSpan`，
     61 个会碰盘的公开 API 全进临界区。
  ③ **ELF 装载**（`g_loaded64` + 读缓冲 + VFS 全局路径缓存）：两个装载同时跑就 `exec reject reason=read`
     （实测会把 drvwatch 装失败）。→ `E64LoadSpan`。
  三把锁同一套语义：**可重入 + 让出式 + 有界 + 抢不到如实失败**；等锁让出时必须**临时 `sti; hlt` 再按原 IF 关回去**
  （syscall 分发全程 IF=0，纯自旋永远等不到持有者 —— 这条自己踩过一次）。
  实测：重启延迟 **572~676 ms**（原 4860~4896）、崩溃重启探针 **10/10 且零坏读**（原 9/10）、
  `drv64_test` **54/54 连过两次**（判据新增"命令超时 / 坏读重试 / exec reject / 装载失败 / 锁饿死"零容忍）、
  64 MiB 基准恢复 **97 / 89~94 MB/s**。
- ⚠️ **宿主窗口截帧 UNVERIFIED（环境）**：宿主屏 1366×768 装不下 1:1 的 1280×800 客人（QEMU 窗口顶到
  864 高，下沿在屏幕外），整窗截帧只能对上 84%。`tests/ext4gui64_test.py` 因此把这一条记
  `SKIP(env)`（不算通过、也不冤枉产品），客人 screendump 证据完整。
- ✅ **根目录的预设资源目录**（v0.5.8）：`/Wallpapers/` = 预设壁纸、`/Avatars/` = 预设头像，
  **两者分开**。内置 3 张示例壁纸（代码画的 1024×768 PNG）+ 6 张代码画的 128×128 圆形头像；
  想加自己的壁纸：把图丢进仓库的 `res/wallpapers/`（png/jpg/jpeg/bmp），重建后就在 `/Wallpapers/` 里，
  设置页"预设壁纸"按钮会按文件名顺序循环切换。壁纸支持 **JPEG**（`decode_jpeg64`）。
- ⚠️ 系统卷约 12 MB：`/Wallpapers` 里的图**单张建议 ≤ 1 MB**（大图会把卷塞满）；
  运行时不能直接往只读系统卷里拷图，要经 `res/wallpapers/` + 重建。GIF 不支持。
- ⚠️ **持久化只认 VimtuFS2 主分区**：载体不是卷时（单盘跑安装介质 / 盘上没有 `type=0x07` 分区）
  改动只活在内存里。v0.5.5 起开机滚屏会打印 `[CONF64] WARN NOT-PERSISTENT …`，不再让用户靠猜。
  `build64/sysdisk.img` 是**构建产物**（每次 `build64.sh` 重拼），日常请用 `vimtu64-64-disk.img`
  （构建脚本只在它不存在时初始化一次）。实测 `tests/persist64_test.py` 14/14 PASS。

### 图形
- ❌ 真 **Wayland 线协议**：现在的 `wl64` 是**内核自定义 surface ABI**（create/attach/damage/commit/export/ack），
  没有 xdg_shell / libwayland / 协议 XML → Linux 图形程序不能直接用
- ⚠️ GPU 通用加速：virtio-gpu 只做 2D 上屏（flip/blit），**合成仍在 CPU**；无 DRM/KMS、无 3D。
  硬件光标平面（queue 1，v0.5.5）已实现但**默认关闭**（`fw_cfg cursor=1` 才开）：软件光标有 6 种形状，
  硬件平面只有一张 64×64 位图 —— 打开后形状切换就没有了。另外 QEMU 的 cursor 平面**不进
  scanout surface**，所以 `screendump` 抓不到它（验收改为让设备回读位图逐字节比对）。
  软件光标的**箭头位图**在 v0.5.19 修过：桌面那张原本是 **19 行**（后 3 行是 row7..9 的重复，底部会多出
  一块与三角**不相连**的白块 = 看着像"箭头断了一截"），登录/锁屏那张是按"宽度 r/2+1"涂出来的"锤子"、
  且只有左边和顶部有描边。现在两处统一成同一张标准 **16 行**箭头位图（X = 描边、'.' = 填充）。
  像素级验收：`py -3 tests/cursor64_test.py`（42 个描边位置全命中、第 16 行之后不允许有像素）。
- ❌ 硬件视频解码、HiDPI 完整适配、**输入法（中文 IME）**

### 硬件 / 安全
- ❌ 无线网卡（802.11 / WPA）
- ❌ Secure Boot 签名验证
- ⚠️ W^X 只对 `/bin/vpkg`、`/bin/store` 用链接脚本 + 构建断言保证，不是全局强制
- ⚠️ USB hub / USB 键鼠的完整语义未验证（存储已可用）
- ⚠️ **应用启动是"懒"的**（v0.5.7）：点开始菜单/桌面上的卷程序时，内核只登记待启动项，
  由 **appload 内核任务**装载（`load_ms` 实测 1.3~2.0 s）——GUI 帧不再同步等它，
  所以界面一直有响应；装载完窗口才出现。连点两个应用时后一个覆盖前一个（单槽队列，有界）。
  看门狗阈值因此从 5 s 放宽到 **20 s**（它是死锁探测器，不是性能闸门）。
- ⚠️ **ATA 单扇区读偏慢**（实测 ~13 ms/扇区，QEMU 上本该是微秒级）：60 KB 的程序装载约 1.5 s，
  其中绝大部分是数据读本身。已单独记为性能项。

### 生态
- ❌ 浏览器、办公套件、Java/Python 之类大运行时
- ❌ 包依赖只解析一层；`update` 只比对提示不自动升级
- ❌ 休眠/睡眠（S3/S4）

---

## 三、系统架构（一眼看懂）

```
┌─────────────────────────────────────────────────────────────┐
│  用户态（ring3）                                             │
│  /bin/shell.bin 终端 · explorer · edit · calc · mines       │
│  tmgr · monitor · settings · about · store · vpkg · netd     │
│  sounder · wm（ring3 合成器）· Lua/TCC/musl 静态程序         │
├─────────────────────────────────────────────────────────────┤
│  两个系统调用入口（号段完全隔离）                             │
│  ① int 0x80 —— VimtuOS 自有 ABI（窗口/事件/音频/网络…）       │
│  ② syscall 指令 —— Linux ABI（56 个号，见第一节第 7 条）      │
├─────────────────────────────────────────────────────────────┤
│  内核（ring0）                                               │
│  task/proc（进程·地址空间）  vfs64/fs64/store64（文件·设置）  │
│  gui64（窗口·合成·脏矩形）   gfx64（壁纸·模糊·阴影·图元）     │
│  elf64（装载器）             fd64（fd 表·pipe·目录句柄）      │
│  驱动：ata/ahci/nvme · fat64 · ehci/xhci/usb · e1000 · hda    │
│        ps2kbd/mouse · virtio-gpu · pci/acpi/apic/smp · fb     │
├─────────────────────────────────────────────────────────────┤
│  引导：BIOS 两阶段（MBR Stage1 -> 32KB Stage2）/ UEFI(256KB) │
└─────────────────────────────────────────────────────────────┘
```

- 内核**不再加新功能**（用户定的规矩）：新功能放用户态，内核只做完善与修 bug
- 设计 Token 表（颜色/圆角/间距/动效）由 Rust 生成并链接进内核，UI 与内核共用同一份真源


### 混合驱动架构（VimtuOS Unique Hybrid Driver Architecture）
> 由项目发起人 **Fanqi89** 主导设计的 VimtuOS 独创底层契约。**不是**对 Linux 的模仿，也不是对
> Windows NT 的照搬 —— 它有自己的 15 MiB 内核扩容边界与 AHCI/NVMe 在 ring0、文件系统协议在
> ring3 的**特定切分比例**。完整定义见 [`docs/混合驱动架构.md`](docs/混合驱动架构.md)。

```
                    ┌──────────────────────────────────────────────┐
   ring3（协议层）    │ ext4 / NTFS 等复杂文件系统协议解析（服务进程） │  ← 易崩的东西隔离在这里
                    │ ↑ 零拷贝：共享内存 / DMA 映射（不做大块 memcpy）│
                    ├──────────────────────────────────────────────┤
   ring0（控制器）    │ AHCI / NVMe 控制器本身 · 中断控制器 · 基础显卡 │  ← 只留"低延迟 + 不懂协议"
                    └──────────────────────────────────────────────┘
```

- **为什么这么切**：控制器寄存器操作要求延迟与确定性（留在 ring0）；协议解析要能崩、能重启、
  能替换（放 ring3，崩了重启服务进程，内核不受影响）。这条切分线是 VimtuOS 特有的比例，
  既不是"全在 ring0"（宏内核），也不是"全在 ring3"（纯微内核）。
- **零拷贝死命令**：ring3 驱动向 ring0 块设备要数据走**共享内存 / DMA 映射**，不允许大块内存拷贝。
  现有共享内存原语：`shmget/shmat`（Linux ABI）+ 内核 `page_alloc_64` 页池（PA 与 VA 恒等，可直接给 DMA）。
- **内核空间 15 MiB 边界**：v0.9.31 起内核容量从 4 MiB 扩到 **15 MiB**（`ML64_KERNEL_SECTORS=30,720`），
  这是"吸引协议驱动之外的一切进内核"的前提；同时它仍是**有界**的 —— 内核映像上界（含 fb 的 33 MB
  后备缓冲）有构建期闸门，不许越过载荷暂存区，堆区从内核结束地址之上开始分配。
- **层级定位**：当前落地的是**混合驱动架构**，它是通往**混合内核架构**的第一步 —— 保留宏内核的
  高效系统调用（`int 0x80` + `syscall` 两条入口）作为性能主干，同时引入微内核式的 IPC 消息机制
  （共享内存 + 阻塞等待 + 端口级唤醒）保证隔离与可恢复性。
---

## 四、快速开始

### 构建（Windows + MSYS2）
```bash
# 依赖：clang/lld、nasm、Python3 + fontTools + Pillow
py -3 _otf2ttf.py      # 校验/派生字体（源码仓不含字体本体，清单见 FONTS.md；缺失=构建硬失败）
bash build64.sh        # 产出 vimtu64-64.iso / .img，0 error 才算过
```

### 跑起来（QEMU）
```bash
# ① 日常使用（单盘、开箱即用、**改的东西重启还在**）
qemu-system-x86_64 -m 512 -vga std -drive format=raw,file=vimtu64-64-disk.img
# ② 安装到自己的盘：ISO 当光盘 + 一块空盘当目标盘
qemu-system-x86_64 -m 512 -cdrom vimtu64-64.iso \
  -drive format=raw,file=target.img -boot order=d -vga std
# 装完重启即进系统（默认用户 vimtu，无口令；锁屏回车两次进桌面）
```

> **哪块盘才会持久化**（开机日志直接告诉你）：只有**带 VimtuFS2 主分区（MBR type=0x07）**的盘才真落盘。
> 三块盘的定位别混：
> - `vimtu64-64.img` = **安装介质**（引导链 + 安装程序 + 载荷）。单盘启动会进安装向导，
>   它自己**不是**数据盘 —— 拿它当日常盘用，改动重启就回到出厂。
> - `build64/sysdisk.img` = **构建产物**。每跑一次 `build64.sh` 都会被各批 pack 重拼
>   （"重启（= 我又构建了一版）数据就丢"的真相就是这条）。
> - `vimtu64-64-disk.img` = **日常数据盘**（推荐）。构建脚本只在**它不存在时**从 sysdisk.img
>   初始化一次，之后重建**绝不覆盖**。
>
> 载体不是 VimtuFS2 卷时，开机滚屏与串口会打印
> `[CONF64] WARN NOT-PERSISTENT carrier=… reason=no-vimtufs2-volume -> changes in this session will be LOST on restart`。
> 实测：`tests/persist64_test.py`（同一块盘连续起两次）**14/14 PASS**。

> **★ 用 ISO 装到硬盘的朋友注意（v0.5.11 修的严重缺陷）**
> v0.5.10 及更早：安装器只把 **8073 扇区**（MBR+引导+内核）写进目标盘，**12 MiB 的文件系统卷没装进去**。
> 装好的盘启动时会打 `[VFS64] mount FAILED reason=magic -> mount_system FAILED` 与
> `[CONF64] WARN NOT-PERSISTENT`，于是**所有修改都写不进去**（桌面图标位置/壁纸/主题/用户名/密码/
> 新建文件夹全部重启即丢），点应用也打不开（`/bin/*` 在卷里，卷没装上）。
> **v0.5.11 起**：载荷 = 完整磁盘映像（`8009 + 24759 = 32768` 扇区，含卷），安装器打印
> `[INSTALL] 完成：已写 32768 扇区`，装好的盘 LBA 8009 直接就是 `VIMTUFS2` 卷头。
> **所以：请用 v0.5.11 或更新的 ISO 重装一次**，旧 ISO 装出来的盘永远存不住东西。
> 验收：`tests/install_persist64_test.py`（装到 8G 盘 → 改设置/建目录 → 关机重启）**10/10 PASS**：
> `[VFS64] mount_system slot=0 drive=0 start=8009`、`[CONF64] flush ok via=vfs`、
> 重启后 `[CONF64] load ui.theme=3`、宿主侧解析卷仍能读到 `/home/vimtu/pdata`。

> **系统卷有多大？（v0.5.15 起 = 主分区多大，卷就多大，而且文件真能读）**
> 安装收尾会把卷**按主分区大小重建**：重算布局（`bmn = ceil(total/4096)`）→ 把 inode 区 + 数据区
> 整体后移 → **把 inode 与各级间接块里的绝对块号一起平移 Δ**（v0.5.15 修；只搬数据会让每个文件
> 都指向错位的数据）→ 按旧位图重建新位图 → 重写超级块。安装日志给出两行证据：
> `[INSTALL] volume grow ok moved=24751 total=209608854`（100 GiB 盘 = 102348 MiB 卷）与
> `[INSTALL] volume grow relocate delta=51168 inodes=373 indirect_blocks=261`。
> 验收口径是"宿主侧独立解析卷"：`total` 必须等于主分区扇区数、载荷里 378 个文件必须逐字节一致、
> 所有 inode CRC 自洽、文件用到的块在位图里都已用（`tests/volreloc64_test.py`：8 GiB 全量 +
> 100 GiB 窗口，32 条断言）。
> 边界：安装器走 PATA（LBA28 ≤ 128 GiB）；首次挂载 100 GiB 卷要读完 26 MB 位图（约 36 s，进桌面
> 比 8 GiB 慢约 1 分钟）；**inode 上限仍是 512**（空间大 ≠ 文件个数多）；旧盘不会自动长大。
>
> 安装收尾会把卷**按主分区大小重建**：重算布局（`bmn = ceil(total/4096)`）→ 把 inode 区 + 数据区
> 整体后移 → 按旧位图重建新位图 → 重写超级块。安装日志会打
> `[INSTALL] volume grow ok moved=24751 total=16670870`，装好的 8 GiB 盘上 `df`/`[VFS64] mount ok`
> 看到的是 **blocks=16670870 ≈ 7.95 GiB**（空闲 16,666,631 块）。
> 16 MiB 小盘不扩容（主分区只比卷大一点点时直接跳过）。
>
> **用 ISO 装出来的系统为什么以前缺文件？（v0.5.18 修的）**
> `tools/make_iso64.py` 里的 `PAYLOAD_SECTORS` 一直是 **8073**（v0.5.11 之前的 system.img 大小），
> 而 v0.5.11 把数据分区并入安装载荷后 system.img 已经是 **32768 扇区（16 MiB）**。这个值被回填进
> 引导桩的 `VMSS`：loader64 按它从光盘读、安装器按它往目标盘拷 —— 于是 **ISO 装盘只写 4.13 MB**，
> 而系统卷从 LBA 8009 开始，实际只有前 64 个扇区有效 → inode 号靠后的文件（`/Wallpapers`、
> `/Avatars` 以及卷链尾部打包的一切）全部丢失，界面上就表现成"预设壁纸 0 张 / 预设头像 0 张"。
> hybrid 硬盘介质（`vimtu64-64.img`）走的是另一条带 `VIMTUPAY` 头的路径，所以一直正常，把问题盖住了。
> 现在 `VMSS` 按 `SYSTEM.IMG` 的真实字节数推导（= **32768**），秒级回归不跑 QEMU：
> `py -3 tests/isopayload64_test.py`（7 条断言）。
> 边界：**用旧 ISO 装过的盘不会自愈**（文件是真的没写进去），要重装。
>
> **格式化 / 删除分区怎么用？（v0.5.14 起）**
> 磁盘页选中一个**分区**行（标题行/未分配空间行不可选）后，`删除`、`格式化` 才会亮起，
> 键盘快捷键是 `D` / `F`。两者都是**两步确认**：第一次按只"武装"（按钮变成
> `确认删除` / `确认格式化`，底部琥珀色提示目标分区号），再按一次同一个键或再点同一个按钮
> 才真写盘；`Esc`、换行、换页、刷新都会取消。
> 动作语义：删除 = 清掉对应表的那一项（GPT 盘连主/备两份项数组与头 CRC 一起重算，
> 并同步清掉另一张表里描述同一段的项）+ 抹掉该分区首扇区；格式化 = 主数据分区建真
> **VimtuFS2**（FAT32 类分区且 ≥32.5 MB 建真 **FAT32**），EFI 系统分区会被**拒绝**
> （抹了 UEFI 就起不来）。

> **关机请走正常关机**（系统"关机"按钮 / QEMU monitor `quit`）。
> 直接 `kill` QEMU 可能丢掉写缓存里最后几块 —— 这也是"关机就丢"的另一半原因。
> QEMU 想要更稳可以加 `-drive format=raw,file=…,cache=writethrough`（写穿，立刻落盘）。

> **安装光盘的目录结构**（v0.5.6 起是层级式，和 Windows / Ubuntu 安装盘同构）：
> ```
> /BOOT/CDISO.BIN           BIOS El Torito 引导桩
> /BOOT/LOADER64.BIN        实模式准备 + 进长模式 + ATA loader
> /BOOT/KERNEL64.BIN        安装程序内核（补零到 4 MiB）
> /SYSTEM/SYSTEM.IMG        系统载荷（装进目标盘的完整磁盘映像）
> /EFI/BOOT/BOOTX64.EFI     UEFI 引导桩（与 ESP 分区内同一份）
> /EFI/BOOT/UEFI64.BIN      平铺长模式引导器
> /README.TXT  /VERSION     介质自述与版本串
> ```
> ESP 分区（El Torito 0xEF 项指向它）内部仍是标准布局：
> `\EFI\BOOT\BOOTX64.EFI` + `\UEFI64.BIN` + `\KERNEL64.BIN` + `\SYSTEM.IMG`。
> **改目录不影响引导**：引导链读的是回填进镜像的绝对 LBA（`VMLD/VMKN/VMSY` 补丁点），
> 不按文件名查找。三种引导（BIOS 光盘 / UEFI 光盘 / hybrid 硬盘·U 盘）实测都在
> `tests/iso64_layout_test.py` 里：**25/25 PASS**。

---

## 五、验收与工程质量

- **一个权威判据脚本**：`py -3 tests/status_report.py`（能力清单，当前 **95 完成 / 0 部分 / 0 未做，共 95 项**）
- **`--full` 真跑全部验收**：77 个脚本，逐条吐断言；有 FAIL/TIMEOUT 时门禁返回非 0
- **每处修复都要有实测数字**：内核尺寸、耗时、像素差异（`max|d|`）、串口打点原文
- **判据与事实不符时先修脚本**，但**绝不放宽断言强度**（例：锁屏汉字墨高按 em 口径重定，而不是把区间拉大）
- **文档与代码冲突时以实测为准**，并把过期文案改掉（本版就清了三处误导性文字）

常用命令：
```bash
py -3 tests/status_report.py                 # 看能力清单
py -3 tests/status_report.py --full          # 真跑全部验收（数小时）
py -3 tests/<模块>_test.py                   # 单跑一条
```

---

## 六、完整目录树

> 下面是**仓库全量目录树**：主树列到目录级，`kernel/` `tests/` `tools/` `user/` `docs/` 等
> 大目录的文件清单在折叠块里（点开就是完整清单）。
> **不含构建产物**：`build64/`、`vimtu64-64.iso`、`vimtu64-64.img`、`vimtu64-64-disk.img`、
> `LOG.txt`、`.pi/`（本地工作目录）。`tests/` 现有 **115** 个验收脚本。

```
├── Fonts/   (2 项，见下方折叠清单)
├── Fonts-open/   (4 项，见下方折叠清单)
├── boot/   (6 项，见下方折叠清单)
├── build/   (2 项，见下方折叠清单)
├── docs/   (21 项，见下方折叠清单)
├── gui_rs/   (4 项，见下方折叠清单)
├── kernel/   (146 项，见下方折叠清单)
├── legacy32/   (11 项，见下方折叠清单)
├── logo/
│   ├── kaisi.png
│   └── logo.png
├── res/   (1 项，见下方折叠清单)
├── sdk/   (1 项，见下方折叠清单)
├── tests/   (120 项，见下方折叠清单)
├── third_party/   (6 项，见下方折叠清单)
├── tools/   (49 项，见下方折叠清单)
├── user/   (27 项，见下方折叠清单)
├── .gitattributes
├── .gitignore
├── FONTS.md
├── LICENSE
├── LOG.txt
├── README.md
├── _make_icons.py
├── _make_logo.py
├── _make_start_icon.py
├── _otf2ttf.py
├── _subset_fonts.py
├── _subsetsimhei.py
├── ahci_report.ppm
├── ahci_report.ppm.retry0.ppm
├── ahci_report.ppm.retry1.ppm
├── ahci_report.ppm.retry2.ppm
├── bootinfo.h
├── build64.sh
├── build_uefi.sh
└── 构建与运行说明.md
```

<details><summary><b>boot/</b> — 1 个子目录 / 5 个文件</summary>

```
├── efi/
    ├── efi.h
    ├── efi_main.c
    ├── enter64.asm
    ├── jump64.asm
    ├── probe_main.c
    ├── stub.c
    └── uefi64.c
├── boot.asm
├── cdiso.asm
├── hybrid_mbr.asm
├── loader64.asm
└── loader64_atapi.inc
```
</details>

<details><summary><b>docs/</b> — 3 个子目录 / 18 个文件</summary>

```
├── fonts/
    ├── OFL-NotoSans.txt
    ├── OFL-NotoSansSC.txt
    ├── OFL-NotoSerif.txt
    ├── OFL-Sarasa.txt
    └── UNIFONT-LICENSE.txt
├── screenshots/  (22 项)
├── shots/  (37 项)
├── 64位升级验证记录.md
├── ISO安装介质说明.md
├── UEFI地址空间实验报告.md
├── UEFI引导说明.md
├── VMware验证指南.md
├── VimtuOS系统介绍.md
├── 中断调试记录.md
├── 内核边界与架构规则.md
├── 发布流程.md
├── 字体许可说明.md
├── 安装程序说明.md
├── 应用层与系统调用说明.md
├── 开发历程.md
├── 引导链架构.md
├── 桌面栈移植说明.md
├── 真机验证指南.md
├── 重启与任务管理器设计.md
└── 项目状态总览.md
```
</details>

<details><summary><b>kernel/</b> — 0 个子目录 / 146 个文件</summary>

```
├── acpi64.cpp
├── acpi64.h
├── ahci64.cpp
├── ahci64.h
├── ap_trampoline64.asm
├── apic64.cpp
├── apic64.h
├── app64.cpp
├── app64.h
├── ata64.cpp
├── ata64.h
├── calc64.cpp
├── config64.cpp
├── config64.h
├── console64.cpp
├── console64.h
├── debug64.h
├── demo64.cpp
├── demo64.h
├── desktopops64.cpp
├── desktopops64.h
├── display64.cpp
├── display64.h
├── drive64.cpp
├── drive64.h
├── e1000_64.cpp
├── e1000_64.h
├── edid64.cpp
├── edid64.h
├── ehci64.cpp
├── ehci64.h
├── elf64.cpp
├── elf64.h
├── entry64.asm
├── explorer64.cpp
├── explorer64.h
├── fat64.cpp
├── fat64.h
├── fb.cpp
├── fb.h
├── fd64.cpp
├── fd64.h
├── font.cpp
├── font.h
├── font8x8.h
├── fontcfg64.cpp
├── fontcfg64.h
├── fs64.cpp
├── fs64.h
├── gfx64.cpp
├── gfx64.h
├── gui64.cpp
├── gui64.h
├── hda64.cpp
├── hda64.h
├── hda64_stub64.cpp
├── hwinfo64.cpp
├── hwinfo64.h
├── hwui64.cpp
├── hwui64.h
├── icons64.cpp
├── icons64.h
├── img64.cpp
├── img64.h
├── input.cpp
├── input.h
├── input64.cpp
├── input64.h
├── isr_dbg64.asm
├── isr_out64.asm
├── isr_probe64.asm
├── isr_stubs64.asm
├── kernel64.cpp
├── linker64.ld
├── locklogin64.cpp
├── locklogin64.h
├── mem64.cpp
├── mem_64.h
├── memlayout64.h
├── mines64.cpp
├── net64.cpp
├── net64.h
├── netraw64.cpp
├── netraw64.h
├── nvme64.cpp
├── nvme64.h
├── panels64.cpp
├── panels64.h
├── panic64.cpp
├── panic64.h
├── part64.cpp
├── part64.h
├── pci64.cpp
├── pci64.h
├── port.h
├── preload64.cpp
├── preload64.h
├── proc64.cpp
├── proc64.h
├── rust64.h
├── session64.cpp
├── session64.h
├── settings64.cpp
├── settings64.h
├── setup64.cpp
├── setup64.h
├── setup64_keys.h
├── sig64.cpp
├── sig64.h
├── smp64.cpp
├── smp64.h
├── startmenu64.cpp
├── startmenu64.h
├── store64.cpp
├── store64.h
├── switch64.asm
├── syscall64.cpp
├── syscall64.h
├── syscall_entry64.asm
├── sysstate64.cpp
├── sysstate64.h
├── task64.cpp
├── task64.h
├── taskmgr64.cpp
├── terminal64.cpp
├── theme64.cpp
├── theme64.h
├── uefi_gop.h
├── update64.cpp
├── update64.h
├── usb64.cpp
├── usb64.h
├── userdb64.cpp
├── userdb64.h
├── usermode64.cpp
├── usermode64.h
├── vfs64.cpp
├── vfs64.h
├── virtio_gpu64.cpp
├── virtio_gpu64.h
├── wl64.cpp
├── wl64.h
├── x86_64.cpp
├── x86_64.h
├── xhci64.cpp
└── xhci64.h
```
</details>

<details><summary><b>res/</b> — 1 个子目录 / 0 个文件</summary>

```
└── wallpapers/
    ├── 1.jpeg
    └── README.md
```
</details>

<details><summary><b>tests/</b> — 0 个子目录 / 115 个文件</summary>

```
├── a42a64_test.py
├── ahci64_test.py
├── apic64_test.py
├── app64_test.py
├── apps64_test.py
├── bigfile64_test.py
├── boot64_assert.py
├── bootlog64_test.py
├── browser64_test.py
├── browser_http64_test.py
├── busybox64_test.py
├── clicklaunch64_test.py
├── desktop64_test.py
├── desktop_p7a_test.py
├── desktopops64_test.py
├── dir64_test.py
├── disk_boot_test.py
├── display64_test.py
├── display_runtime_test.py
├── drvsvc64_test.py
├── dynlink64_test.py
├── edit64_test.py
├── ehci64_test.py
├── elf64_test.py
├── esp_install_test.py
├── explorer64_test.py
├── fatread64_test.py
├── fbmap64_test.py
├── fd64_test.py
├── fileops64_test.py
├── font64user_test.py
├── fontq64_test.py
├── fonts64_test.py
├── fs_term_test.py
├── fs_tree_test.py
├── gfx64_test.py
├── gpucursor64_test.py
├── gui_modern64_test.py
├── gzip64_test.py
├── hda64_test.py
├── iconboot64_test.py
├── icons64_test.py
├── install_flow_test.py
├── install_persist64_test.py
├── ipc64_test.py
├── iso64_install_test.py
├── iso64_layout_test.py
├── iso64_usb_test.py
├── jpeg64_test.py
├── linuxapp64_test.py
├── locklogin64_test.py
├── logfollow64_test.py
├── lua64_test.py
├── make64_test.py
├── mouse_parse_test.py
├── mouseperf64_test.py
├── multivol64_test.py
├── musl64_test.py
├── net64_test.py
├── netuser64_test.py
├── nvme64_test.py
├── office64_test.py
├── panels64_test.py
├── partmgr64_test.py
├── partition_ops_test.py
├── volreloc64_test.py
├── perm64_test.py
├── persist64_test.py
├── pipe64_test.py
├── preload_update_test.py
├── imgfit64_test.py
├── presets_audit64_test.py
├── presets_user64_test.py
├── presets64_test.py
├── proc64_test.py
├── qemuhelp.py
├── rust64_test.py
├── sched_stress_test.py
├── screen64_probe.py
├── screenshot64.py
├── settings64_test.py
├── sh64_test.py
├── sig64_test.py
├── sigpipe64_test.py
├── smp64_test.py
├── sounds64_test.py
├── soundsegs64_test.py
├── startmenu64_test.py
├── statfs64_test.py
├── status_report.py
├── store64_test.py
├── sysstate64_test.py
├── tar64_test.py
├── taskmgr64_test.py
├── tcc64_test.py
├── term_render64_test.py
├── texthint64_test.py
├── textlock64_test.py
├── textstress64_test.py
├── textvnc.py
├── textwm64_test.py
├── tmgr_proc_test.py
├── uefi64_install_test.py
├── uefi_cr3_experiment_test.py
├── ui_extra64_test.py
├── usb64_test.py
├── usb_boot_both_fw_test.py
├── usbstorage_test.py
├── usbwrite64_test.py
├── user64_test.py
├── userlib64_test.py
├── virtiogpu64_test.py
├── vmware_install_test.py
├── vmware_make_vm.py
├── vmware_mouse_probe.py
├── vpkg64_test.py
├── vsync64_test.py
├── wl64_test.py
├── wm64_test.py
└── xhci64_test.py
```
</details>

<details><summary><b>tools/</b> — 2 个子目录 / 47 个文件</summary>

```
├── i686-elf/
    ├── bin/
    ├── i686-elf/
    ├── include/
    ├── lib/
    ├── libexec/
    └── share/
├── nasm/
    ├── LICENSE
    ├── nasm.exe
    └── ndisasm.exe
├── assets_pack_win.py
├── browser_pack_win.py
├── busybox_build_win.sh
├── busybox_pack_win.py
├── demo_pack_win.py
├── drvdemo_pack_win.py
├── dynlink_build_win.sh
├── dynlink_check_win.py
├── dynlink_payload_win.py
├── edit_pack_win.py
├── fat_check.py
├── fetch_icons.py
├── fontdemo_pack_win.py
├── gzip_build_win.sh
├── gzip_pack_win.py
├── jpeg_make_testset.py
├── kernel_size_report.py
├── lua_build_win.sh
├── lua_pack_win.py
├── lxcorpus_build.py
├── make_asset_images.py
├── make_build_win.sh
├── make_esp.py
├── make_flat.py
├── make_iconpack.py
├── make_iso64.py
├── make_pack_win.py
├── make_presets.py
├── make_shellvol.py
├── make_vap.py
├── musl_build_win.sh
├── musl_hello64.ld
├── net_pack_win.py
├── office_pack_win.py
├── payload_embed_win.py
├── pe_info.py
├── presets_pack_win.py
├── probe64.py
├── sounder_pack_win.py
├── sounds_gen.py
├── store_pack_win.py
├── svg2png.py
├── tar_build_win.sh
├── tar_pack_win.py
├── tcc_build_win.sh
├── tcc_pack_win.py
└── wm_pack_win.py
```
</details>

<details><summary><b>user/</b> — 15 个子目录 / 12 个文件</summary>

```
├── apps/  (16 项)
├── browser/
    ├── www/
    ├── browser.c
    ├── build_browser.sh
    ├── html.c
    ├── html.h
    ├── net.c
    └── net.h
├── busybox/
    ├── bb_config.py
    ├── bbdrv.c
    ├── bbdrv64.ld
    ├── bbdrv_start.S
    ├── bbstart.S
    ├── bbstart.c
    ├── bbwrap.c
    ├── bbwrap64.ld
    ├── bbwrap_start.S
    ├── busybox64.ld
    ├── vimtu_dirent.c
    └── vimtu_fcntl.c
├── gzip/
    ├── gzip.c
    ├── gzip64.ld
    └── gzip_start.S
├── ldso/
    ├── ldso.c
    ├── ldso.ld
    ├── ldso_start.S
    └── libfoo.ld
├── lib/  (27 项)
├── lua/
    ├── demo/
    ├── a44probe.c
    ├── lua64.ld
    ├── lua_compat.c
    ├── lua_start.S
    ├── lua_start.c
    ├── luadrv.c
    ├── luadrv64.ld
    └── luadrv_start.S
├── make/
    ├── demo/
    ├── make64.ld
    ├── make_start.S
    ├── make_start.c
    ├── makedrv.c
    ├── makedrv64.ld
    └── makedrv_start.S
├── net/
    └── netd.c
├── office/
    ├── build_office.sh
    ├── calc.c
    ├── calc.h
    └── office.c
├── shell/
    ├── abi.c
    ├── abi.h
    ├── build_shell.sh
    ├── main.c
    ├── sh64.h
    └── shell64.ld
├── store/
    ├── build_store.sh
    ├── demo_cli.c
    ├── demo_gui.c
    ├── store.c
    ├── store64.ld
    ├── vpkg.c
    ├── vs.h
    ├── vs_gzip.c
    ├── vs_io.c
    ├── vs_pkg.c
    └── vs_xz.c
├── svc/
    └── drvdemo.c
├── tar/
    ├── tar64.c
    ├── tar64.ld
    ├── tar_start.S
    └── tar_start.c
├── wm/
    ├── wm.c
    ├── wmabi.c
    ├── wmabi.h
    ├── wmclock.c
    └── wmpanel.c
├── build_user.sh
├── demo64.asm
├── fbdemo.asm
├── filedemo64.asm
├── hello64.asm
├── hello_elf64.asm
├── hello_elf64.ld
├── pipe64.asm
├── proc64.asm
├── spin64.asm
├── xmmsse.asm
└── xmmsse_elf64.ld
```
</details>

<details><summary><b>Fonts/</b> — 0 个子目录 / 2 个文件</summary>

```
├── OFL-NotoSans.txt
└── fms_metadata.xml
```
</details>

<details><summary><b>Fonts-open/</b> — 0 个子目录 / 4 个文件</summary>

```
├── OFL-NotoSans.txt
├── OFL-NotoSansSC.txt
├── OFL-Sarasa.txt
└── UNIFONT-LICENSE.txt
```
</details>

<details><summary><b>build/</b> — 2 个子目录 / 0 个文件</summary>

```
├── icons/
    ├── apps/
    ├── system/
    └── manifest.json
└── sdk/
    └── icons_ext/
```
</details>

<details><summary><b>gui_rs/</b> — 2 个子目录 / 2 个文件</summary>

```
├── assets/
    ├── fonts/
    ├── icons/
    ├── images/
    └── README.md
├── src/
    ├── lib.rs
    ├── panic.rs
    ├── theme.rs
    └── tokens.rs
├── Cargo.toml
└── build_rs.sh
```
</details>

<details><summary><b>legacy32/</b> — 10 个子目录 / 1 个文件</summary>

```
├── artifacts/
    ├── build/
    └── vimtu64.vmdk
├── boot/
    ├── _loader64_lm.part.asm.txt
    ├── install.asm
    ├── loader
    └── loader.asm
├── build32.sh/
    └── build.sh
├── dev/  (109 项)
├── docs32/
    ├── COMPLETE_ISO_SOLUTION.md
    ├── FINAL_SOLUTION.md
    ├── FIX_SUMMARY.md
    ├── ISO_GENERATION_FIX.md
    ├── ISO_README.md
    ├── SYSTEM_FIX_REPORT.md
    ├── TEST_PLAN.md
    ├── find_iso_tools.txt
    ├── fix_fonttools.txt
    ├── help_cn.txt
    ├── upgrade_plan.md
    ├── 启动脚本修复报告.md
    ├── 启动脚本说明.md
    └── 构建与运行说明（32位时代）.md
├── kernel/  (61 项)
├── prototype/
    ├── boot.asm
    ├── combined.asm
    ├── font.h
    ├── interrupts.cpp
    ├── io.asm
    ├── io.s
    ├── kernel.cpp
    ├── keyboard.asm
    ├── linker.ld
    ├── loader.asm
    └── vga.cpp
├── scripts/
    ├── _patch_install_iso.py
    ├── build.ps1
    ├── build.sh.backup
    ├── create_iso.ps1
    ├── create_iso_with_full_path.sh
    ├── create_iso_with_xorriso.sh
    ├── get-pip.py
    ├── install_tools.ps1
    ├── install_tools_en.ps1
    ├── run.sh
    ├── run_vimtu64.bat
    ├── test_iso_build.sh
    └── 重新生成ISO.sh
├── uefi/
    ├── efi/
    ├── UEFI_INTEGRATION.md
    ├── UEFI_INTEGRATION_COMPLETE.md
    ├── build_uefi.sh
    ├── screenshot.png
    └── test_uefi.sh
├── vmware32vm/
    ├── README.md
    ├── Vimtu64-0.scoreboard
    ├── Vimtu64-1.scoreboard
    ├── Vimtu64-2.scoreboard
    ├── Vimtu64-install.vmx
    ├── Vimtu64.scoreboard
    ├── Vimtu64.vmsd
    ├── Vimtu64.vmx
    ├── Vimtu64.vmxf
    ├── install-disk-flat.vmdk
    ├── install-disk.vmdk
    └── nvram
└── README.md
```
</details>

<details><summary><b>third_party/</b> — 6 个子目录 / 0 个文件</summary>

```
├── busybox/
    ├── busybox-1.36.1/
    ├── LICENSE
    ├── README.vimtu64.md
    └── busybox-1.36.1.tar.bz2
├── lua/
    ├── lua-5.4.7/
    └── README-vimtu64-a4.md
├── lzma/
    ├── 7zTypes.h
    ├── LICENSE
    ├── Lzma2Dec.c
    ├── Lzma2Dec.h
    ├── LzmaDec.c
    ├── LzmaDec.h
    ├── Precomp.h
    └── README.vimtu64.md
├── make/
    ├── make-4.4.1/
    └── README.vimtu64-b5.md
├── musl/  (22 项)
└── tcc/
    ├── tcc-0.9.27/
    └── README.vimtu64-a4b.md
```
</details>

---
## 七、开源与发布

- 仓库：<https://github.com/Fanqi89/VimtuOS>
- 发布：每个版本一个 Release + 两个附件（ISO / IMG）+ 正文附 sha256；**旧版本 Release 永不改动**
- 最新：**v0.5.4**（本版：**浏览器 / 办公套件进应用注册表** = 桌面图标 + 开始菜单可见，点开 fork+execve 真进程；`builtin=14`；`apps64_test` **10/10**；如实说明 `.vap64` 32 KiB 上限使它们只能做预装应用）
- 上一版：**v0.5.3**（本版：**办公套件 `/bin/office`** = 文档/表格/演示三合一，8×20 定点整数公式引擎（四则/引用/SUM/环检测 `#CIRC`），切模式与退出自动落盘，宿主侧独立解析卷读回标记串；`office64_test` **15/15 PASS**）
- 上一版：**v0.5.2**（本版：**浏览器真的能上网** = `/bin/browser` 接内核 `net_raw`（号 53）自己实现 ARP/DNS/TCP + `HTTP/1.0 GET`；宿主 `http.server` -> 客人 `10.0.2.2`，`status=200 bytes=300` 与宿主页逐字节一致、屏幕墨迹 1.17%、不可达端口如实失败；`browser_http64_test` **12/12 PASS**）
- 上一版：**v0.5.1**（本版：**浏览器真正可交互** = 补上 `wl_seat_get()`（键盘焦点 + 指针捕获），点击翻页 / ESC 退出实测通过；`browser64_test` 从 UNVERIFIED 变 **16/16 PASS**。v0.5.0 的"输入路由未通"其实是应用漏调这个 API，不是内核缺能力）
- 上一版：**v0.5.0**（本版：**大窗口 ABI** = shm 单对象 64 KiB -> 2 MiB、大 surface 屏幕居中；**`/bin/browser` ring3 真窗口浏览器** = 896x580 窗口 + HTML 子集排版 + 滚动 + 像素验收）
- 上一版：**v0.4.9**（SIGPIPE **端到端**验收 = 新 ring3 程序 `/epipe.elf` + 真进程 shell；fontconfig **真可配置** = 构建期预置 0666 的 `/etc/fonts.conf` + `ui.stem` A/B；`status_report` 的 blob 外置判据改成**产物自洽**）
- 更早：**v0.4.8**（SIGPIPE 真投递 / UI 笔画加厚 + 对比度 / 最小 fontconfig）、**v0.4.7**（终端三无渲染 / ANSI 16 色 / 启动直出 boot log / getdents64 + /proc）、v0.4.6、v0.4.5、v0.4.4、v0.4.3、v0.2.0-beta.5 …
- 更早：v0.4.6、v0.4.5、v0.4.4、v0.4.3、v0.2.0-beta.5 …

发布流程见 [docs/发布流程.md](docs/发布流程.md)；逐批历史见 [docs/开发历程.md](docs/开发历程.md)。

---

## 八、许可与免责

- 代码：**GPL-3.0**
- 字体：Noto Sans / Sarasa Mono SC / Unifont 均为开放许可，全文在 `docs/fonts/`，
  逐个文件的 URL/版本/协议/SHA256 见 [FONTS.md](FONTS.md)
- 免责：**这是一个仍在开发中的操作系统**。它会崩溃、会丢数据、会不兼容你的硬件。
  不要把它装在唯一一台电脑上，不要在里放重要文件。按 GPL-3.0 的免责条款，作者不承担任何损失责任。

---

## English summary

VimtuOS is a **from-scratch x86_64 operating system kernel** (no Linux/Windows code), written in
C/C++ with Rust-generated design tokens and assembly boot loaders. It boots on BIOS and UEFI,
installs from a hybrid ISO onto MBR/GPT disks, runs a Windows-11-style desktop with a real
window compositor, ships a terminal, file manager, editor, calculator, settings, task manager,
an app store with its own package manager (`.deb` + VAP64), and speaks a **Linux-compatible
syscall ABI** (56 calls, incl. `getdents64` and a minimal `/proc`) — enough for static musl
programs, Lua, TCC, gzip, tar and make.

**Known gaps** (documented, not hidden): no Linux socket family, no threads/futex, signals are
recorded but not delivered, no swap/COW/ASLR, no real Wayland wire protocol (GPU compositing is
CPU-side; virtio-gpu does 2D present only), no wireless, no Secure Boot, no IME, no browser or
office suite.

Everything is verified by 77 automated test scripts with serial-console and pixel-level evidence.
License: GPL-3.0. **Alpha quality — do not use as your only OS.**
