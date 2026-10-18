# VimtuOS · Vimtu64

**一个从零手写的 64 位操作系统内核**（x86_64，纯 64 位长模式，不基于 Linux/Windows/任何现成内核），
目标是"能当日常系统用"，而不是教学玩具。

- 语言：C / C++（内核）+ Rust（设计 Token 表）+ 汇编（引导与入口）+ Python（构建与验收）
- 版本：**0.5.4**（版本号不带 `-beta`；小版本 v0.4.x、大版本 v0.5.0…）
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
| 安装程序 | ✅ Win10 风格向导；新建/删除/格式化分区**真实写盘**；进度百分比；装完自动重启 |
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
- 盘符层：PATA/AHCI/NVMe 每块盘 → 分区 → fs 识别 → `C:` / `D:` …

### 4. 桌面（UI）
- Windows 11 风格：Token 化设计（Rust 生成）、毛玻璃、大圆角、双层浅阴影、居中 Dock
- 窗口系统：拖动/八向缩放/最小化飞行/任务栏/Dock 窗口列表、脏矩形局部重绘
- 锁屏 → 登录（多用户、口令 SHA-256×1000+salt、头像/主题/壁纸按用户持久化）
- 开始菜单：固定网格 + 全部应用列表 + **搜索（含用户新装的应用）** + 数字快捷键 1..8
- 应用注册表 `/etc/apps.json`：装完的应用自动进桌面/开始菜单，图标三层优先（包内 PNG → 系统图标包 → 程序化兜底）
- 设置：主题/壁纸/头像/字号/鼠标灵敏度/缩放/声音/Dock 尺寸…约 40 个键持久化（store A/B 双槽 + 世代号）

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
- ❌ **线程**：`clone` 只支持 fork 语义，`CLONE_VM/THREAD` → `-ENOSYS`；**没有 futex**
- ❌ **swap / 页换出**：内存不足直接 `-ENOMEM`
- ❌ **COW**：`fork` 整页物理复制
- ❌ **ASLR**：映像地址确定
- ❌ 内核模块加载（单体内核）
- ⚠️ UEFI 路径下无地址空间隔离（`fork/execve/wait4` 全 `-ENOSYS`，串口如实标注）

### Linux 兼容
- ❌ **socket(41)/connect(42)/accept(43)/sendto(44)** —— 任何 Linux 网络程序（curl/浏览器）都跑不了
- ❌ 部分：`lstat`（无符号链接）、`readlink`（只认 `/proc/self/exe`）、`getrlimit/setrlimit`（无配额）
- ❌ glibc / 发行版二进制**未验证**（`/proc`、`/sys`、pty、完整 TLS/vDSO 都缺）

### 文件系统
- ❌ 日志（journaling）/ 快照 / 配额 / ACL / 符号链接
- ❌ ext4 / NTFS（读不了 Windows 盘）
- ⚠️ 写文件是"整文件覆盖 + 立刻落盘"，单文件上限 67584 B（fd 层）
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
- ❌ 硬件视频解码、HiDPI 完整适配、**输入法（中文 IME）**

### 硬件 / 安全
- ❌ 无线网卡（802.11 / WPA）
- ❌ Secure Boot 签名验证
- ⚠️ W^X 只对 `/bin/vpkg`、`/bin/store` 用链接脚本 + 构建断言保证，不是全局强制
- ⚠️ USB hub / USB 键鼠的完整语义未验证（存储已可用）

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
│  引导（BIOS MBR / UEFI 两段式）→ 长模式 → 内核               │
└─────────────────────────────────────────────────────────────┘
```

- 内核**不再加新功能**（用户定的规矩）：新功能放用户态，内核只做完善与修 bug
- 设计 Token 表（颜色/圆角/间距/动效）由 Rust 生成并链接进内核，UI 与内核共用同一份真源

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

> **哪块盘才会持久化**（v0.5.5 起开机日志直接告诉你）：只有**带 VimtuFS2 主分区（MBR type=0x07）**
> 的盘才真落盘。三块盘的定位别混：
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

- **一个权威判据脚本**：`py -3 tests/status_report.py`（能力清单，当前 **93 完成 / 1 部分 / 0 未做，共 94 项**）
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

## 六、目录结构（顶层）

```
boot/        BIOS/UEFI 引导（MBR、loader64、El Torito、hybrid MBR）
kernel/      内核（~90 个 .cpp：调度/内存/进程/文件/图形/驱动/GUI 外壳）
user/        用户态程序（shell、explorer、编辑器、商店、网络栈…）与自有 lib
gui_rs/      Rust：设计 Token（颜色/圆角/动效）真源
tools/       构建期工具（字体/图标/ISO/ESP/探针/打包）
tests/       77 个验收脚本（Python，串口 + 像素 + 宿主侧独立校验）
docs/        设计说明、发布流程、开发历程、字体许可
Fonts/       Noto Sans 静态字重（西文/中文）
Fonts-open/  Sarasa Mono SC（终端）+ Unifont（兜底）+ 许可原文
```

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
