# VimtuOS · Vimtu64

**一个从引导扇区到 ring3 应用全部手写的纯 64 位 x86_64 操作系统。**

没有 Linux 内核、没有现成内核框架、没有 libc —— 长模式引导链、内存管理、**调度器**、**文件系统**、
中断与 **APIC/ACPI**、图形与窗口系统、安装程序，以及 **ring3 用户态与两套系统调用入口**，
全部是本工程自己的 C++ / 汇编代码。
它自带一张"三合一"安装盘（BIOS 光盘 / U 盘 / UEFI），安装界面与步骤照 Windows 10 做（去掉了输入产品密钥那一步）。

> 版本 **0.3.1-beta13** · 目标平台 x86_64（长模式）· 许可 **GPL-3.0**（[LICENSE](LICENSE)）· 仓库 <https://github.com/Fanqi89/VimtuOS>
> 状态（权威判据：`python tests/status_report.py`，本批实测）：**完成 49 / 部分 0 / 未做 0（共 49 项能力）** ——
> 本批（**P2 + P4**）新增两项能力：**开始菜单 + 四个二级弹窗（通知/声音/网络/日历）+ 设备插拔 toast + Caps/Shift/滚轮**、
> **VimtuFS2 v4 权限（uid/gid/mode + owner/group/other rwx 真拦截；v3 旧卷兼容但豁免）**。
> 全量验收（`status_report.py --full` 的 `TESTS` 列表 + 专项/扩展脚本）：**43 个断言脚本**（列表长度实测 43；上一批脚本的结论保持不变）：
> **startmenu64_test（开始菜单：居中 / 离 Dock 11px / 上圆角 24 下 10 / 400×420 / 搜索启动 / 2×4 固定网格 / ESC 两级退出）58 条断言**、
> **panels64_test（四个弹窗 + 设备 toast 滑入滑出）78 条断言**、
> **perm64_test（卷 v4 + owner/group/other rwx 真拦截 + su/sudo 提权 + chmod/chown/umask）95 条断言**、
> **store64_test 55 条**（P1c 登录期自动落盘语义）、**locklogin64_test 88 条**；
> 上一批的 **rust64_test 124**、**gfx64_test 55**、**gui_modern64_test 149** 与 usbstorage(99)/bootlog64(30)/
> fileops64(95)/explorer64(71)/fatread64(61)/fs_tree(87)/fs_term(32)/fd64(34)/bigfile64(61)/multivol64(109)/
> app64(31)/elf64(56)/proc64(67)/sysstate64/boot64_assert 等既有脚本的结论照旧（本批 **0 FAIL**）。

---

## 一、它能做什么

| 能力 | 说明 |
| **★ 开始菜单 + 四个二级弹窗 + 设备插拔 toast（批次 P2）** | `kernel/startmenu64.{h,cpp}` + `kernel/panels64.{h,cpp}`（3593 行）：**居中开始菜单**（`x=440 y=293 w=400 h=420`、**底边距 Dock 顶 11px**：`bottom=713 dock_top=724 gap=11`、**上圆角 24 / 下圆角 10**、亚克力 + 双层浅阴影 + 1px 高光边、跟随主题）；**搜索框**（`x=630 y=357 w=194 h=34 r=8`，实时过滤 + 回车启动走 `app64_launch64`）；**2×4 固定网格**（8 个磁贴 108×62，应用名中英双语）；**左上时间 + 年月日**；**状态区四个极简线性图标**（网络→声音→中/英→通知，`[START64] status icon …` 逐个打点）；**左下头像 + 用户名**、**右下设置 + 电源菜单**（关机/重启/锁定；动效只有按下高亮 + 内缩）；**ESC 两级退出**（先关弹窗再关菜单）。**四个二级弹窗**（各 320×380 / 340×104 / 300×380 / 400×450）：**通知**（角标 + 未读计数 + 空态“无通知”）、**声音**（滑块 + 百分比 + 输出源，**如实标注未接音频驱动** `applied=0`）、**网络**（WiFi 区如实空态 `[PANEL64] net wifi count=0 state=no-hardware text=无无线硬件` + 以太网区固定行）、**日历**（**7×6** 网格、滚轮 / 方向键 / PageUp·PageDown 翻月、今天、年份面板）；弹窗全部**亚克力 + 双层浅阴影 + 1px 高光边 + 跟随主题 + 锚点跟随开始菜单**（`follow=menu`）且**不压 Dock / 不超屏**（`no_dock_overlap=1`）；`input.cpp` 加 **Caps 反转 Shift**、**Shift 切中英**、**Intellimouse 4 字节滚轮**（`[INPUT64] wheel mode=1 id=3 packet=4B`）。端到端 `tests/startmenu64_test.py`（58 条断言）+ `tests/panels64_test.py`（78 条断言）。P2 边界见下表 |
| **★ VimtuFS2 v4 权限（uid/gid/mode + 真拦截 + su/sudo 提权，批次 P4）** | `kernel/vfs64.{h,cpp}`：卷格式 **v4**（inode 仍 **128B**：`type@0` / `namelen@1` / `size@4` / `d0..d3@8,12,16,20` / `ind@24` / `parent@28` / `mtime@32` / `nlink@36` / `kind@38` / `name@40..71` / `dind@71` / **`uid@75, gid@77, mode@79`** / 保留 `@81..124` / `CRC@124`（覆盖 `[0,124)`））；**owner / group / other 三段 rwx 真拦截**（读 `r`、写 `w`、遍历 `x`、目录写 `w+x`），拒绝一律 **`-EACCES`(13)** 并打 `[PERM64] deny op=… path=… uid=… gid=… mode=… need=… owner=u:g`（调用方 `[FD64] open FAILED … rc=13`）；**进程 uid/gid/euid/egid**（fork/execve 继承）+ **`getuid/geteuid/setuid/chmod/chown/umask/access/stat` 返回真值**；`su - root` / `su -` / `sudo -i` **真提权**（`[PERM64] cred uid=0 gid=0 euid=0 egid=0 user=root via=su-dash`）；终端 `ls -l` / `chmod` / `chown` / `umask` / `tree` / `su` / `id` 全走真权限。新格式化一律产出 **v4**（`[VFS64] format ok blocks=24759 version=4 inode=128 root=8017 perm=uid/gid/mode@75/77/79 rootmode=0755`）；**v3/v2 旧卷可挂载可读写但不拦截**（卷里没有权限字段，`chmod` 明确回 `volume v3 has no mode field`）。自检 `[VFS64] perm selftest ok owner/mode/other/root/chmod/chown/umask=1`；端到端 `tests/perm64_test.py`（95 条断言，含 v3 旧卷兼容）。P4 边界见下表 |
| **★ 锁屏 + 登录 + 多用户骨架（批次 P1c）** | `kernel/locklogin64.{h,cpp}`（1447 行）：开机第一屏 = **锁屏**（时间 **72px**、年月日 **18px**、白色亚克力、背景**清晰不模糊**）→ 回车/点击 → 背景**清晰→模糊 20px**（动画 250–350ms，取 300ms）→ **登录界面**（**104px** 圆头像、**96×96 圆角 24** 主题渐变登录按钮、有密码时 **360×48** 磨砂密码框 + **48×48** 确认按钮；ESC 平滑反向返回）；`kernel/userdb64.{h,cpp}`（1186 行）：用户库 **`/etc/users.db` v1 文本**（用户名/UID/主目录/头像/加盐哈希；普通用户 uid≥1000、`/home/<名>/Desktop`，root `/root` **不在登录界面**，首启自动建无密码引导用户 `vimtu`）。终端 `useradd/userdel/passwd/users/whoami/id/su/sudo/exit/loginctl lock`；`su - root`/`su -`/`sudo -i` 提权会话身份（euid=0，GUI 用户名/头像不变），`exit` 退回；会话身份只在内存（重启回锁屏）。打点 `[LOCK64]`/`[LOGIN64]`/`[USER64]`；端到端 `tests/locklogin64_test.py`（88 条断言）。**★ 批次 P4 起会话身份真的管权限**：登录/`su`/`sudo` 会把 `uid/gid/euid/egid` 发布进进程凭证（`[PERM64] cred`），文件系统按它做 rwx 拦截（见上表 P4 行）。边界：口令哈希 = 盐（`rdtsc` xorshift，非 CSPRNG）+ SHA-256×1000（非 bcrypt/argon2）、头像 **JPEG 不支持**、每用户桌面只在用户库/会话层（GUI 桌面图标网格未按用户分目录） |
| **★ Windows 11 现代外观（批次 P1）** | `kernel/theme64.{h,cpp}` = **设计 Token 唯一真源**（圆角 14/12/9/24、图标 10；模糊 24/12；透明度 0.45/0.80；双层阴影 `0 2 4 rgba(0,0,0,0.08)` + `0 12 32 rgba(0,0,0,0.12)`；动效 150/225/330ms + `cubic-bezier(0.2,0,0,1)`）+ **7 套主题**（白色(默认)/暗色/蓝白渐变/粉白渐变/粉绿渐变/粉紫渐变/紫白渐变）；`kernel/gfx64.{h,cpp}` = 现代图元层（圆角抗锯齿、双层阴影 mask 缓存、毛玻璃缓存、渐变、**壁纸 6 种适应模式**：填充/适应/拉伸/平铺/居中/跨屏）；`kernel/gui64.cpp` 换成**居中靠下 Dock**（y = 屏高−76、高 60、圆角 24、图标 46、间距 11、悬停放大 1.20 + 邻位让位、点击回弹、运行小圆点、最小化小横杠）。截图 `docs/screenshots/modern_white64.png`、`modern_dark64.png`、`modern_bluegrad64.png`；验收 `tests/gfx64_test.py`（55 条）/ `tests/gui_modern64_test.py`（149 条）。边界：壁纸/主题整屏重建在 QEMU TCG 上约 0.33–0.44s（宿主 CPU 争用可达 3s） |
| **★ Dock 开始按钮用真图 + 内置 PNG 解码（批次 P1）** | `kernel/img64.{h,cpp}`：PNG 解码（inflate **stored/fixed/dynamic 三种块**、色型 **0/2/3/4/6**、多块 IDAT、全部滤波器）+ BMP（未压缩）；**JPEG 未支持**（如实打 `unsupported format`）。`logo/kaisi.png`（158×158，19,810 B）构建期由 objcopy 嵌入系统内核 → 启动期**幂等写入 VimtuFS2**（`[IMG64] install path=/logo/kaisi.png … ok=1`）→ 运行时从卷加载（`[IMG64] load path=/logo/kaisi.png ok=1 … 158x158 (from VimtuFS2 system volume)`、`[DOCK64] start icon src=/logo/kaisi.png size=46 ok=1`），解不开时回落内置图标。**已修的一个真 bug**：inflate 的固定 Huffman 表曾被动态表污染（共享表 + 一次性缓存，真 PNG 末尾的 BFINAL 空固定块解错 → rc=18）——现在**每个固定块重建表**，并加"解真图 + 全图像素 FNV-1a 指纹比对"回归自检（`[IMG64] selftest real ok=1 … fnv=…`）。边界：`[DOCK64] start icon src=` 打点目前是裸路径（不带 `vfs:` 前缀） |
| **★ Rust 正本接入：gui_rs 设计 Token + 主题配色真源（批次 P1）** | `gui_rs/` crate（**`#![no_std]`、无 alloc、无浮点**；33 个 Token + 7 套主题 + panic 钩子）用 **rustup shim 绝对路径的 rustc** 编成 `gui_rs/gui_rs.o`（target `x86_64-unknown-none`），**只链进系统内核**（安装介质 **0 个 Rust 符号**）；启动期自检 `[RUST64] tokens ok themes=7 accent=#RRGGBB selftest PASS`，并与 C++ 主题表交叉核对数量/名字/accent；终端 `rust [tokens\|set N]`；`kernel/rust64.h` 是 C 链接声明（全 POD + UTF-8 `(ptr,len)`，无 Rust 引用/String/泛型）。验收 `tests/rust64_test.py`（124 条：nm/objdump 符号、安装内核 0 符号、体积上限、串口 accent 与 Rust 源码比对） |
| **★ 开机滚屏引导控制台（boot console + dmesg，本批次新增）** | `console64.{h,cpp}`：`dbg64_putc()`（串口/调试口**唯一出口**）加一个 `dbg64_set_sink64()` 钩子，**既有打点一行都不用改**就镜像进 **16 KiB 环形缓冲**（含每条打点的 tick；行内上限 160 字符、超出截断；环形满丢最旧并计数，头部保留 16 行永不丢）→ `fb_init`/`font_init` 之后、**进向导/桌面之前**把这次启动真的跑出来的日志**整屏回放**（黑底 + 终端等宽面 Sarasa Mono + 行首 Linux 风格时间戳 `[    0.123]`，`FAIL/PANIC` 红、`WARN` 黄、其余浅灰；行满整体上移、只提交脏区）→ 有界停留（1200 tick-ms）或**按任意键立即进桌面** → 终端 `dmesg` 回看（头部保留 + 环形缓冲，串口打 `[CON64] dmesg[i]` 证据）→ `boot verbose on|off` 把开关持久化到 config64/store64（下次冷启动不再回放）。打点 `[CON64] ring init|screen ready|replay|live|skip key|selftest|dmesg|boot verbose`；端到端 `tests/bootlog64_test.py`（30 条断言：串口打点 + 黑底像素 + 行间距 + **直读 vram 的滚动证据** + 按键跳过 + dmesg 早期行 + 冷启动 verbose=0 不回放），截图 `docs/screenshots/bootlog64.png` |
| **★ 文件操作：右键菜单 / 复制 / 剪切 / 粘贴 / 重命名 / 删除 / 新建文件夹 / 多选（本批次新增）** | `explorer64.cpp` + `vfs64_rename64`：**右键菜单**（条目：打开/复制/剪切/重命名/删除/属性；空白：新建文件夹/粘贴/刷新/属性；hover 高亮、置灰、Esc/点外部关闭）；**多选**（单击、Ctrl 加选、Shift 范围选、**空白拖动框选**（浅蓝矩形，内核无 alpha 混合故用实色近似）、Ctrl+A 全选、Esc 取消）；**内核内剪贴板**（最多 8 条 `(卷槽, 完整路径)` + 复制/剪切）→ 粘贴：文件**分块复制**（64 KiB/块；单文件 ≤8 MiB，v2 旧卷 67584 B）、**目录递归复制**（深度 ≤4、整棵 ≤96 条，超限如实计入 `skipped`）、**跨卷 C:↔D: 真能粘贴**（源用 `vfs64_*_on64(源槽)` 读、目标按界面盘符写）；**重名策略：自动追加 `(2)`、`(3)`…**（插在扩展名之前；**不带空格** —— VimtuFS2 名字只允许 `0x21..0x7E`，空格不合法）；**剪切 = 复制成功后删源**（先全部复制成功再删，绝不半删）；**重命名**（F2 / 菜单 → 内联编辑，重名一律拒绝）；**删除**（Delete / 菜单 → **两段式确认**：状态栏提示"再按一次 Delete 确认删除（10 秒内有效）"；**非空目录明确提示"目录非空，暂不支持递归删除"**，绝不假装成功）；**属性**（小面板 + 打点：名称/类型/大小/修改日期/所在卷）；**工具栏 6 个按钮**（新建文件夹/复制/剪切/粘贴/重命名/删除，无选中或剪贴板为空时置灰）；**快捷键** Delete / F2 / Ctrl+C / Ctrl+X / Ctrl+V / Ctrl+A / Esc。打点 `[UI] explorer ctxmenu|clip|paste|rename|delete|mkdir|sel|props …`；端到端 `tests/fileops64_test.py`（95 项：像素 + 串口 + 鼠标/键盘注入 + 宿主侧解析 D: 卷字节；双击/框选注入有界重试） |
| **★ USB 存储（U 盘只读，可从 U 盘拷应用；本批次新增）** | `usb64.cpp` 在 UHCI 上认 **USB Mass Storage（Class=08 / SubClass=06 / Protocol=0x50 = Bulk-Only Transport）**：取配置描述符里两个**批量**端点 → **批量传输**（一包一个 TD、最多 64 包 4KB、DATA0/DATA1 逐包翻转、IN 方向**短包即结束**、NAK 由硬件按帧重试、等待用 `g_ticks64` **有界超时**后 abort 整条链）→ **BOT**（CBW 签名 `'USBC'` / CSW 签名 `'USBS'` + Tag 回显 + `dCSWDataResidue` 校验）→ **SCSI 只读子集** `INQUIRY` / `TEST UNIT READY` / `REQUEST SENSE`（出错时打 key/asc/ascq）/ `READ CAPACITY(10)`（块数 + 块大小 → MB/GB 换算打点）/ **`READ(10)`**。摸到的盘**接到既有磁盘抽象**：驱动器号 `ATA64_USB_BASE = 24` → `ata64_identify`（型号取 INQUIRY、容量取 READ CAPACITY）/ `ata64_read`（走 READ(10)），于是 `drive64` 盘符（U 盘就是 `D:`，容量/可用/`ro=1` 由现有代码算）、`fs64`/`fat64`（FAT32 **只读**挂载）、`explorer64`（浏览 + **Ctrl+C/Ctrl+V 把文件从 U 盘拷进 C:**）**一行都不用改**。**只读**：没有 `WRITE(10)`，`ata64_write` 对 USB 驱动器号直接失败并打 `[USBST] write refused`（不假装成功）。打点 `[USBST] iface found|inquiry|capacity|read lba=.. ok|read FAILED reason=..|selftest PASS/FAIL mask=|write refused`；端到端 `tests/usbstorage_test.py`（99 条断言：串口 + 像素 + monitor 键鼠注入 + **宿主侧解析 C: 的 VimtuFS2 卷与 `build64/hello.elf`、`hello.vap` 逐字节比对** + 整根 U 盘镜像 CRC32 前后不变），截图 `docs/screenshots/explorer_usbstick64.png` |
|---|---|
| **四种引导方式 + 装好的盘双固件** | BIOS 光盘（El Torito）、U 盘 / 硬盘（hybrid MBR）、裸盘（MBR→loader→ATA）、**UEFI 自动引导**（自研 PE 桩 + 平铺长模式引导器）。QEMU 与 VMware、BIOS 与 UEFI 四种组合全部实测通过；**"ISO 当 U 盘"形态在 OVMF（UEFI）与 SeaBIOS（BIOS）下也都进安装向导**（`tests/usb_boot_both_fw_test.py` 四档）；装好的盘在 UEFI 与 BIOS 下都能启动（`tests/esp_install_test.py`） |
| **现代磁盘结构（安装盘 + 目标盘都有）** | 自写 GPT（工具链的 xorriso 不生 GPT）与 MBR 双兼容；**FAT32 ESP 也是自写的**（这台机器没有 mkfs.fat/mtools）：48MB 卷、簇数 96736（真 FAT32 的硬下限是 65525 簇）、FSInfo + 备份引导扇区齐全。**安装时会往目标盘写混合 MBR + 盘尾 GPT + 48MB FAT32 ESP**（内核实现在 `kernel/fat64.{h,cpp}`，卷参数与构建期 `tools/make_esp.py` 逐条对齐），ESP 里放 `EFI/BOOT/BOOTX64.EFI` + `UEFI64.BIN` + `KERNEL64.BIN` |
| **Win10 同款安装程序** | 语言 → 现在安装 → 许可条款 → 安装类型 → **磁盘与分区（新建/删除/格式化，真实写盘）** → 复制与真实百分比进度 → 完成并**自动重启**；**无产品密钥步骤** |
| **装完就是一台独立系统** | 开机直接进桌面：桌面图标、任务栏、开始菜单（10 项）、窗口拖拽/缩放/最大化/最小化、脏矩形重绘 |
| **调度器（多任务）** | `task64.cpp`：16 个任务槽、每任务 16KB 内核栈、8ms 时间片轮转、IRQ0 抢占、睡眠/退出/回收、`task_kill64`；启动即 `[TASK64] scheduler up tasks=N`、`kheart` 心跳持续增长；任务管理器"进程"页与终端 `ps/kill` 读的是**真实任务表** |
| **真实文件系统 VimtuFS2（v4：目录树 + 权限）** | `vfs64.cpp`：超级块（magic `VIMTUFS2` + CRC32）、空闲位图、**128B inode**（直接块×4 + 一级间接块 + **二级间接块** → 单文件 ≤**8 MiB**）、**多级路径**（`/dir/sub/file`，`.`/`..` 语义齐全）、inode **mtime**（RTC 打包 u32）与**类型判定**（目录/VAP64/ELF64/文本/二进制存进 inode 的 `kind` 字段）、**★ P4：`uid@75 / gid@77 / mode@79` + owner/group/other rwx 拦截**；`vfs64_format/mount/stat64/list64/opendir+readdir/closedir/mkdir64/create64/write64/read64/unlink64/rmdir64/tree_dump64` + 兼容旧 API；**v3/v2 旧卷仍可挂载可读写但不拦截**（卷里没有权限字段），新格式化一律产出 **v4** |
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
| ⚠️ **多用户 / 登录的边界（批次 P1c；P4 起权限已拦截，剩余边界如实写清）** | ① ~~权限位尚未拦截~~ —— **批次 P4 已落地**：`getuid/geteuid/setuid/chmod/chown/umask/access/stat` 返回真值，`su`/`sudo` 提权后的会话身份**真的参与文件权限判定**（详见上文 P4 行与权限边界行）；剩余边界：**无 ACL/xattr、无 setuid、无附加组（gid=uid）、root 绕过 DAC、v2/v3 旧卷不拦截**；② **口令哈希不是 bcrypt/argon2**：盐 = `rdtsc` xorshift（**非 CSPRNG**，16 B）+ **SHA-256 × 1000 轮**，明文绝不落盘/进串口；③ 头像只支持 **PNG/BMP**，**JPEG 不支持**；④ **每用户桌面目前只在用户库/会话层**（`/home/<名>/Desktop` 真实枚举，登录时打 `[USER64] desktop list`），**GUI 桌面图标网格尚未按用户分目录**；⑤ 会话身份只在内存：软重启/断电后回到开机锁屏（root 不在登录界面），`userdel` 不删主目录；⑥ `su` 对**空口令**目标免密 |
| ⚠️ **现代外观 / 图像 / 测试环境的边界（批次 P1，如实写清）** | ① 壁纸/主题**整屏重建在 QEMU TCG 上约 0.33–0.44s**（空闲状态），宿主 CPU 争用时可达 **3s**；② `[DOCK64] start icon src=` 打点目前是**裸路径**（不带 `vfs:` 前缀）；③ `explorer64`/`fileops64` 各有 1–2 条"鼠标位移模型"断言在 QEMU TCG 下**偶发失败**（基线可复现，属环境抖动，**不是功能缺陷**）；④ 图像解码只支持 PNG（色型 0/2/3/4/6）与未压缩 BMP，**JPEG 未支持**；⑤ 开始按钮图路径的完整来源链：内嵌字节 → 幂等装进 VimtuFS2 → 卷上文件优先 → 解不开才回落内置图标（裸 system.img 没有卷时如实走兜底） |
| ⚠️ **开始菜单 / 弹窗 / toast 的边界（批次 P2 新增，如实写清）** | ① **声卡 / 无线驱动未做**：声音面板只改**内存态**（`[PANEL64] sound volume=… why=drag applied=0`，不接任何音频硬件）、**输出源切换只有界面与打点**；网络面板 WiFi 区永远是**如实空态**（`count=0 state=no-hardware text=无无线硬件`，列表绝不造假），只有以太网区是真值（e1000 `STATUS.LU` 活读）；② **U 盘热插拔事件源不可用**：`usb64` 没有拔出检测，toast 事件源只有 e1000 `STATUS.LU`（网线插拔）+ USB 计数轮询差（`[TOAST64] usb poll … hotplug=unsupported`，没有事件就不编事件）；③ **通知的逐条清除 / 全部清除未在测试里注入点击**（界面有，自动化只覆盖角标、空态、新增、超时自动消失、点击关闭）；④ **电源菜单动效只有按下高亮 + 内缩**（关机/重启/锁定三项真实生效，但没有 Win11 那种展开动画）；⑤ 状态区中/英指示器**只显示「英」**：**没有中文输入法**（Shift 只切计数与指示器，`ime=0`）；⑥ **没有 Win 键热键**开关菜单（点开始按钮 / ESC 两级退出） |
| ⚠️ **权限（批次 P4）的边界（如实写清）** | ① **无 ACL / xattr**：只有 owner / group / other 三段 rwx；② **无 setuid / setgid / sticky 目录位**，也就**没有 setuid 可执行文件提权**路径；③ **无附加组**：没有 `/etc/group`，`gid = uid`（group 段只在 gid 相等时生效）；④ **v2/v3 旧卷不拦截**：卷里没有权限字段（uid 显示 root、mode 默认 0755/0644，`chmod` 明确回 `volume v3 has no mode field`），旧卷要重新格式化才拿到权限；⑤ **root 完全绕过 DAC**（`uid=0` 不做任何检查）；⑥ **FAT32 卷没有权限字段**（只读挂载，不参与权限判定）；⑦ **`su` 对空口令目标免密**（未设密码的用户直接切换）；⑧ 权限主体是**会话身份**（euid/egid 与进程凭证，fork/execve 继承），GUI 侧仍以进程边界隔离 |
| ⚠️ **文件操作的边界（本批次新增，如实写清）** | ① **没有递归删除**：非空目录点删除只会提示"目录非空，暂不支持递归删除"（`delete … rc=1 reason=not-empty`），不假装成功；② **单文件上限 8 MiB**（v2 旧卷 67584 B）：超过的源在粘贴时被**整条跳过**（不是截断复制），大文件粘贴走**分块复制**（64 KiB/块，不占大内存、不静默截断）；③ **没有权限/属主、没有回收站**：删除即真删（`unlink64`/`rmdir64`）；④ **重命名只在同一目录内**（改 inode 的 name 字段），**没有跨目录移动/拖拽**（`vfs64_rename64` 不动 `parent`）；⑤ 目录递归复制有界（深度 ≤4、条目 ≤96），超限的条目计入 `skipped`；⑥ 剪贴板是**内核内的路径列表**（不是文件内容快照、不跨重启、最多 8 条）；⑦ 重名后缀是 `(2)`/`(3)`（**无空格**，空格不是合法文件名字符） |
| ⚠️ **开机滚屏引导控制台的有界性（本批次新增，如实写清）** | ① **滚屏是有界的**：只回放环形缓冲里最近的 320 行、每批 6 行上移（像素拷贝，不做字形重光栅化），最后停 1200 tick-ms —— 之后一定是既有的向导/桌面流程；② **任意键立即结束**（按键会被吞掉，不会影响后面的界面）；③ 环形缓冲 16 KiB：启动后期日志多时最早的**普通**行会被覆盖（`[CON64] replay … dropped=N` 如实计数），只有**头部 16 行**（长模式 / BootInfo / E820 等）永久保留，所以 `dmesg` 里一定看得到最早那几行；④ 缓冲不落盘（`/boot.log` 未做）。 |
| ⚠️ **用户态独立地址空间** | 批次 C 起：proc64 每进程独立 CR3 + fork/execve/wait4/kill（BIOS 路径）；UEFI（固件页表）下默认仍如实降级为共享地址空间模式（`[PROC64] cr3 isolation OFF`）。**批次 D 实测**：在固件 PML4 上就地挂用户窗口（清 CR0.WP 手法）与自带 PML4 + 运行期 `mov cr3` **两条路径都在 QEMU+OVMF 与 VMware EFI 下成功**（`[PROC64] uefi exp result=B mode=isolated`），但默认构建不编这段实验（宏 `PROC64_UEFI_CR3_EXPERIMENT`），见 `docs/UEFI地址空间实验报告.md` |
| ⚠️ **fd 语义（批次 D）** | **每进程 fd 表**（32 槽/张；`Proc64` 持有，终端/桌面用内核表）；fd → 引用计数的 `OpenFile64`（**共享偏移游标**）：`dup/dup2` 共享同一对象、`fork` 逐槽继承、`execve` 默认保留（**无 `O_CLOEXEC`**）、`close` 只是 refs-1；`O_APPEND` 真实现；**`pipe(22)` 真实现**（64 B 环形缓冲、非阻塞：写满短写/读空 `-EAGAIN`），`user/pipe64.asm` 是 fork 后父子各持一端的环回证据 |
| ⚠️ **ELF64 只验证过自有静态程序** | 用 `ld.lld -static -nostdlib` 链接的自己的 ELF64 能 load → ring3 → `syscall` → exit；**glibc / 发行版二进制没有验证过**（缺 vDSO、TLS(FS.base) 的完整语义、信号投递、futex、动态链接与重定位） |
| ⚠️ **VFS 的限制（目录树 v3 + 权限 v4 已落地，仍有边界）** | 支持多级路径 `/dir/sub/file`（`.`/`..`、大小写敏感）；**名字 ≤31B**、**inode 总数 ≤512**、**路径 ≤128B / 16 段**、**目录深度 16**；目录删除只支持 `rmdir` **空目录**（非空必须先清空）；**单文件 ≤8 MiB**（批次 M：二级间接块 128×128 块；v2 旧卷仍 67584B）；**权限/属主自批次 P4 起真有**（卷 v4：`uid@75/gid@77/mode@79` + owner/group/other 三段 rwx 拦截；**v3/v2 旧卷可挂载可读写但不拦截**：卷里没有权限字段，uid 显示 root、mode 默认 0755/0644，`chmod` 明确回 `volume v3 has no mode field`）；仍**无 ACL/xattr、无硬链接/符号链接、无稀疏文件、无 setuid/sticky 位**。**多卷**：最多 4 个可浏览卷同时挂载（系统卷 `C:` + `D:/E:/F:`），第 5 个起如实拒绝（`reason=voltable-full`）；终端 `vol` 切当前卷、`df` 列所有卷。终端 `ls/cat/write/touch/rm/mkdir/df/ls -l/chmod/chown/umask/tree/su/id` 与 ring3 的 `open/read/write/close` 都走 `kernel/fd64.cpp` 的 FD 层直连 VimtuFS2（终端命令的多级路径也已支持；**终端帮助与错误文案已改写成多级路径口径**，文件管理器 UI 也已落地（见上表））。终端另有 `fdtest`（独立游标/dup 共享/O_APPEND/pipe/fork 继承一键演示）。**FAT32 只读浏览的边界**：只读（写/删/改名/建目录/粘贴一律 `-FS64_EROFS` 拒绝）、**无权限字段**（FAT32 本身没有 uid/gid/mode）；扇区固定 512B、每簇扇区数按 BPB（1..128，U 盘常见 8=4KB/簇）；可用空间取**挂载时 FSInfo 快照**（不实时刷新，FSInfo 无效则标 unknown）；LFN 上限为 256B 缓冲内的 UTF-16 码元；无碎片整理/无删除项复用 |
| ⚠️ **设置页接线范围** | 显示/会话分区已接 `config64`/`session64`（真落 store64，跨重启保留）；系统/关于页是只读实测值（设备规格来自 hwinfo64/display64/net64/usb64） |
| ⚠️ **没有 TCP/IP / DHCP / DNS** | 网络只有 IPv4 + ARP + ICMP echo（e1000 轮询收发，无中断收包）；UDP/TCP、路由、DHCP、DNS 都没有；只适配 e1000，VMware 的 vmxnet3 未适配 |
| ⚠️ **USB：UHCI 只读 U 盘 + HID 键盘，没有 EHCI/xHCI/USB3** | 主控只有 UHCI（USB 1.1）：没有 EHCI(USB 2.0) / xHCI(USB 3.x)（机器上只有 EHCI 时打 `[USB64] not found` 后优雅退出）；**U 盘只读**（没有 `WRITE(10)`，`write`/`mkdir`/粘贴一律被拒）、**没有分区表解析**（分区表由上层 `part64`/`drive64` 读，本模块只提供"按扇区读"）、**没有拔出检测（热插拔）**；**hub 后面的设备认不出来**（QEMU 不给显式端口时会把第二个设备挂到一个隐式 hub 后面）；一次最多 2 台设备（1 键盘 + 1 U 盘，各拿一个地址）；块大小 ≠ 512 的盘如实拒绝（打点后不暴露成块设备）；USB 鼠标未做；不接中断（由 `kusb` 线程轮询 + 传输自旋锁互斥） |
| ⚠️ **AP 只是停着** | SMP 能启动 AP 并让它报在线，但**没有多核调度**（调度器仍单核、IRQ0 只在 BSP）、没有 IPI、没有 per-CPU 数据/GDT/TSS，AP 自己的 LAPIC/中断不参与 |
| ⚠️ **ACPI/APIC 覆盖有限** | 只解析 RSDP/RSDT/XSDT/FADT/MADT/HPET/MCFG；关机仍走 ACPI 端口 0x604 + 8042 回退链；x2APIC 未适配；固件页表没映射 LAPIC/IOAPIC 的机器会留在 PIC |
| ✅ **SATA/AHCI 盘可直启（引导层已改 BIOS INT 13h）** | 引导层（`boot/loader64.asm`）的**磁盘启动路径**读内核不再用自写 PATA PIO，改走 **BIOS INT 13h 扩展读（AH=0x42 + DAP）**：驱动器号用固件传进来的 `DL`，分块 64 扇区（32KB、不跨 64KB 边界）读进低内存暂存区 `0x20000`，每批再进一次保护模式搬到 `0x100000`；失败复位磁盘重试 3 次后打 `[LM] int13 read FAILED ah=… lba=… retry=…` 并停机（不静默失败）。**BIOS 不需要把 SATA 设成 IDE 兼容模式** —— QEMU 上"只把装好的盘挂 `ich9-ahci` 启动"已进桌面（`tests/disk_boot_test.py`：`[LM] disk boot via INT 13h dl=0x80` → `[OS] booted from installed disk` → `[GUI64] ready`）；真机仍待复验。磁盘级 IDENTIFY/读写也已在 `--strict-dma` 档 PASS（item 5b 修好命令头布局） |
| ✅ **NVMe 盘现在能装、也能启动（item 6 起）** | 内核侧 `kernel/nvme64.{h,cpp}`：**驱动器号 `16..`** 的命名空间可以直接当安装目标盘（`[PART] … drive=16` → ESP/FAT32/GPT 全流程），**UEFI（OVMF）从这块装好的盘启动进桌面已实测**。仍有的边界：单控制器 / 单队列对（QD=8）/ 单命名空间（NSID 1）/ 全程轮询（无中断/无 MSI-X）/ 只支持 **512B 逻辑块**（4KB 逻辑块的盘如实打点并**不给驱动器号**）/ 单条命令 ≤ 64KB。**BIOS 从 NVMe 启动取决于固件**（BIOS 的 INT 13h 盘号来自固件自己的驱动表；QEMU 11.1 的 SeaBIOS 实测支持，但不少真机 BIOS 不认 NVMe）→ **推荐 UEFI 引导**。真机 NVMe 未测 |
| ⚠️ **没有 INT 13h 扩展读的老固件起不来** | 磁盘路径**没有 PIO 回退**（自写 PIO 在 AHCI 机器上读不到盘，已整段删除）：只有支持 EDD 扩展读（AH=0x42）的固件能启动，1998 年后的固件基本都有；更老的机器也跑不动本系统的 64 位长模式 |
| ⚠️ **安装建 ESP 需要目标盘 ≥ ~60MB** | ESP=48MB（真 FAT32：簇数必须 ≥ 65525，512B 扇区 + SPC=1 时卷下限就 ~33.5MB）+ 主分区至少 8MB + 盘尾 GPT 33 扇区 + 引导区 8009 扇区 = 122730 扇区 ≈ 59.9MiB。更小的盘（含 16MB 回归目标盘）只写老 MBR 布局（BIOS-only），串口打 `[INSTALL] esp skipped (disk too small)`。另外装好的盘只写**盘尾备份 GPT**（主 GPT 头的位置被 loader64.bin 占着）—— 依赖固件"主头无效时用备份头"（EDK2 已实测） |
| ⚠️ **只在虚拟机验证** | QEMU + VMware Workstation 双验证（BIOS 与 UEFI 都跑），**未在真机裸机验证**；UEFI 路径未做签名，测试时 `secureBoot=FALSE` |
| ⚠️ **磁盘仍是 PIO 搬运** | 有了 IRQ14 中断唤醒，但没有 DMA/Bus-Master，读写期间 CPU 仍要逐扇区搬 |
| ⚠️ **没有声音** | 无音频驱动 |
| ✅ **Rust 已参与（批次 P1）** | `gui_rs/` crate（`#![no_std]`、无 alloc、无浮点）是设计 Token + 主题配色的**真源**：rustup shim 绝对路径的 rustc 编成 `gui_rs/gui_rs.o`，**只链进系统内核**（安装介质 0 个 Rust 符号）；启动期 `[RUST64] tokens ok themes=7 accent=#… selftest PASS`，终端 `rust [tokens\|set N]`；验收 `tests/rust64_test.py`（124 条） |

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
9. **每次改动都跑自动验收**：**38 个验收脚本**（本批新增 `rust64_test` 124 / `gfx64_test` 55 / `gui_modern64_test` 149 / `locklogin64_test` 88 条断言），全部是"字节级 + 像素级 + 串口日志"三合一，而不是"看起来能跑"。

## 四、系统架构

```
① 引导层（boot/，12 文件 / 2,928 行）
   BIOS 光盘   El Torito → cdiso.asm（引导桩 + 介质描述符 @0x0F00）→ loader64.asm
   U 盘/裸盘   hybrid_mbr.asm（自搬 0x0600）→ boot.asm（512B MBR）→ loader64.asm
   UEFI        BOOTX64.EFI（自研 PE 桩）→ UEFI64.BIN（平铺长模式引导器）
                   └─ 从 ESP 读 KERNEL64.BIN → 0x100000（安装盘上还有 SYSTEM.IMG → 0x04000000）
   loader64 / UEFI64 负责：E820 / VBE(EDID→0x7600) / RSDP(→0x7800) → 读内核到物理 0x100000
   （磁盘启动 = BIOS INT 13h 扩展读 AH=0x42；光盘 = ATAPI；U 盘 hybrid = 桩先搬好）
                             → 建页表（恒等 + 高半区直映）→ 进长模式 → 跳内核入口

② 内核层（kernel/，114 文件 / 49,609 行）
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

  桌面与多用户（批次 P1 / P1c）
    theme64.cpp/.h    ★ 设计 Token 唯一真源（圆角/模糊/透明度/双层阴影/动效 + 7 套主题 + 主题切换）
    gfx64.cpp/.h      ★ 现代图元层（圆角抗锯齿 / 双层阴影 mask 缓存 / 毛玻璃缓存 / 壁纸 6 适应模式）
    img64.cpp/.h      ★ 图像解码（PNG 色型 0/2/3/4/6 + BMP；JPEG 未支持）
    locklogin64.cpp/.h  ★ 锁屏 + 登录界面（72/18px 时间日期、背景清晰→模糊 20px、104px 头像、ESC 反向）
    userdb64.cpp/.h     ★ 多用户骨架（/etc/users.db v1：加盐 SHA-256×1000、会话身份 su/sudo/exit）
    rust64.h            ★ gui_rs（Rust）C 链接声明（Token / 主题 / 交叉核对 / panic 钩子）

③ 桌面层（theme64 / gfx64 / img64 + gui64.cpp + 应用；设计 Token / 主题配色真源 = Rust `gui_rs`，见 kernel/rust64.h）
  窗口 z 序 / 拖拽缩放 / 最大最小化 / 脏矩形消息循环
  **居中靠下 Dock**（y = 屏高−76、高 60、圆角 24、图标 46、间距 11、悬停放大让位 / 点击回弹 / 运行小圆点 / 最小化小横杠）
  7 套主题 / 壁纸 6 适应模式 / 锁屏 + 登录 + 多用户（批次 P1 / P1c）
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
   gui_rs/build_rs.sh  Rust 侧构建：rustc（rustup shim 绝对路径，x86_64-unknown-none）-> gui_rs/gui_rs.o，只链进系统内核
   tests/*.py     54 个 .py（本批新增 startmenu64_test / panels64_test / perm64_test）：串口断言 + screendump 像素断言 + 目标盘字节断言
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
| `vimtu64-64.img` | 7.9 MB | 安装介质裸盘（把一个 7.9MB 镜像直接当硬盘用；实际 8,328,192 B） |
| `build64/kernel64.bin` | 2.09 MB | 安装程序内核（**2,196,688 B**；含 ATA/AHCI/**NVMe** 驱动、分区/安装引擎、FAT32 ESP 写入器） |
| `build64/kernel64_os.bin` | 3.34 MB | 装进硬盘的系统内核（**3,505,520 B**；调度器/VFS（**v4 权限**）/store/ring3/网络/USB（含 **USB 存储/U 盘只读**）/AHCI/**NVMe**/文件管理器 + **文件操作** + **开始菜单 / 四弹窗 / toast** 都在这里；硬上限 4,096,000 B） |
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
> ★ **开机现象（批次 N）**：QEMU/真机开机后，**硬件检查报告页（约 5 秒，可按键跳过）之后、进向导/桌面之前**，
> 屏上会像 Linux 一样**滚一遍这次启动的内核日志**（黑底 + 等宽英文 + 行首 `[    0.123]` 时间戳，`FAIL/PANIC` 红、
> `WARN` 黄），跑完停约 1.2 秒（**按任意键立即进系统**）。日志本体是 16 KiB 环形缓冲（头部 16 行永久保留），
> 装好后在终端里 `dmesg` 可随时回看；不想看可以 `boot verbose off`（持久化，下次冷启动直接进桌面）。

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
python tests/status_report.py --full   # 再真跑 43 个验收脚本（约 6-8 分钟；含本批新增的 3 个）

# 另有专项脚本（--full 列表之外，建议一起跑）；本批新增的 3 个已进 --full 列表，也可单跑：
python tests/user64_test.py            # ring3 / GDT / 用户页权限
python tests/display64_test.py         # EDID 显示层（设置页里的刷新率）
python tests/app64_test.py             # VAP64：安装 + 校验 + ring3 运行
python tests/store64_test.py           # store：双槽 + CRC + 跨重启持久化
python tests/elf64_test.py             # ELF64 加载器 + syscall 指令号段
python tests/rust64_test.py            # Rust 接入：gui_rs 符号进系统内核 + 安装内核 0 符号 + Token 与源码比对
python tests/gfx64_test.py             # 现代图元层：圆角/双层阴影/毛玻璃/壁纸 6 适应模式
python tests/gui_modern64_test.py      # 新 Dock + 7 套主题 + 开始按钮真图
python tests/locklogin64_test.py       # 锁屏 / 登录 / 多用户骨架
python tests/startmenu64_test.py       # P2 开始菜单：几何 / 搜索 / 2×4 网格 / 电源菜单 / ESC 两级退出
python tests/panels64_test.py          # P2 四弹窗（通知/声音/网络/日历）+ 设备插拔 toast
python tests/perm64_test.py            # P4 卷 v4 权限：rwx 真拦截 + su/sudo 提权 + chmod/chown/umask

python tests/screenshot64.py           # 抓一张桌面真机截图（PNG）
```

## 六、工程质量（这个项目最值得说的部分）

* **43 个断言脚本**（`status_report.py --full` 的 `TESTS` 列表长度实测 43；上一批脚本的全部 PASS 结论保持不变；**本批（P2 + P4）新增 3 个脚本全 PASS**：**startmenu64_test 58 条**、**panels64_test 78 条**、**perm64_test 95 条**，另有 **store64_test 55 条**（P1c 登录期自动落盘语义）、**locklogin64_test 88 条**、**rust64_test 124 条**、**gfx64_test 55 条**、**gui_modern64_test 149 条** —— 2026-09-25 实测：本批次（P2 + P4）逐脚本复跑既有脚本（0 FAIL），
  并逐脚本复跑既有脚本 —— `usb64_test` 51、`fatread64_test` 61、`explorer64_test` 71、`fileops64_test` 95、
  `multivol64_test` 108、`fs_tree_test` 85、`bootlog64_test` 30、`boot64_assert`、`desktop64_test`、
  `install_flow_test`、`partition_ops_test`、`esp_install_test`、`disk_boot_test`、`iso64_install_test`、
  `iso64_usb_test`(ata/usb)、`usb_boot_both_fw_test`、`uefi64_install_test`、`vmware_install_test`、
  `nvme64_test`、`ahci64_test`(--strict-dma --sata-install)、`app64_test`、`elf64_test`、`proc64_test`、
  `sysstate64_test`、`ui_extra64_test`、`fs_term_test`、`fd64_test`、`store64_test`、`bigfile64_test` 全部 PASS）。
  `--full` 覆盖的脚本与断言数（历史上的一批固定脚本）：

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
  （以及 sysstate64_test / ui_extra64_test / screenshot64 等，条数随实现增长）；本批新增的 `rust64_test` 124、
  `gfx64_test` 55、`gui_modern64_test` 149、`locklogin64_test` 88 也已进 `--full` 列表，逐条实测 PASS。
  **item 6 新增 `tests/nvme64_test.py`（38 条断言，全 PASS）**：NVMe 控制器/命名空间/队列打点 +
  只读自检 + **完整装到 NVMe 盘**（`[PART] … drive=16` / ESP FAT32 / GPT）+ 目标盘与 **ESP 三文件逐字节** +
  **UEFI(OVMF) 从这块盘启动进桌面** + BIOS(SeaBIOS) 如实测（能起来就断言、起不来就如实 SKIP 并说明
  "BIOS 从 NVMe 启动取决于固件"）。
* **三类证据**：
  * **串口级**：每步都打标记（`[LM64]` / `[G64]` / `[SETUP]` / `[PART]` / `[TASK64]` / `[VFS64]` /
    `[STORE64]` / `[SYSCALL]` / `[USER64]` / `[APP64]` / `[ELF64]` / `[APIC]` / `[SMP]` / `[NET64]` /
    `[USB64]` / `[EDID64]` / `[HW64]` / `[ACPI64]` / `[CON64]` / `[THEME64]` / `[GFX64]` / `[IMG64]` /
    `[DOCK64]` / `[RUST64]` / `[LOCK64]` / `[LOGIN64]` / `[UI]`），验收读日志判定；
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
| M16 | **USB 存储（U 盘只读，批次 O）**：UHCI 上认 Mass Storage BOT（Class=08/SubClass=06/Protocol=0x50）→ 批量传输（TD 链 + toggle + 短包 + 有界超时）→ CBW/CSW + SCSI `INQUIRY`/`READ CAPACITY(10)`/`READ(10)` → **驱动器号 `24..`**（`drive64`/`fat64`/`explorer64` 零改动）→ U 盘 FAT32 只读浏览 + **把 .vap/.elf 从 U 盘拷进 C:**（宿主侧逐字节核对） | ✅ |
| M17 | **Windows 11 现代外观 + Rust 接入（批次 P1）**：`theme64`（设计 Token 唯一真源）+ `gfx64`（圆角/双层阴影/毛玻璃/壁纸 6 适应模式）+ `img64`（PNG/BMP 解码，JPEG 未支持）+ **居中靠下 Dock** + **7 套主题**；`gui_rs`（`#![no_std]` Rust）作为 Token/主题配色真源**只链系统内核** | ✅ |
| M18 | **锁屏 + 登录 + 多用户骨架（批次 P1c）**：`locklogin64`（锁屏 72px/18px、背景清晰→登录模糊 20px、104px 头像、360×48 密码框）+ `userdb64`（`/etc/users.db` 加盐 SHA-256×1000；`su`/`sudo` 只改会话身份；root 不在登录界面；**权限位属 P4，尚未拦截**） | ✅ |
| M19 | **开始菜单 + 四个二级弹窗 + 设备插拔 toast（批次 P2）**：`startmenu64`（居中 400×420、底边距 Dock 顶 11px、上圆角 24 下圆角 10、搜索框 + 2×4 固定网格、状态区四个线性图标、电源菜单、ESC 两级退出）+ `panels64`（通知/声音/网络/日历四弹窗：亚克力 + 双层浅阴影 + 1px 高光边 + 跟随主题 + 锚点跟随开始菜单 + 不压 Dock；设备 toast 右侧滑入 / 停 5s / 可关闭 / 堆叠）+ `input.cpp`（Caps 反转 Shift / Shift 切中英 / Intellimouse 4 字节滚轮） | ✅ |
| M20 | **VimtuFS2 v4 权限（批次 P4）**：inode 128B 加 `uid@75 / gid@77 / mode@79` + **owner/group/other 三段 rwx 真拦截**（`-EACCES`(13) + `[PERM64] deny` 打点）+ 进程 uid/gid/euid/egid（fork/execve 继承）+ `geteuid/setuid/chmod/chown/umask/access/stat` 返回真值 + `su`/`sudo` 真提权（`[PERM64] cred`）；**v3/v2 旧卷可挂载可读写但不拦截** | ✅ |

## 八、路线图（未完成的部分）

按依赖顺序（这也是欢迎贡献的方向）：

| 顺序 | 目标 | 现状 | 做完能带来什么 |
|---|---|---|---|
| ① | ~~**独立地址空间 + fork/execve**~~ | 批次 C 已完成（每进程 CR3 + fork/execve/wait4/kill；UEFI 固件页表下如实降级为共享模式） | —— |
| ② | **glibc 级兼容** | 只验证过自有静态 ELF64 | TLS(FS.base) 真生效、信号投递、vDSO、futex、动态链接与重定位；毕业考试是"静态 busybox 起 shell" |
| ③ | **VFS 补齐** | ~~单层路径~~（v3 已支持多级目录树 + 空目录删除 + mtime/类型判定）；~~权限/属主~~（**P4 已完成**：卷 v4 uid/gid/mode + owner/group/other rwx 真拦截 + `chmod`/`chown`/`umask`，v3 旧卷豁免）；~~FAT 浏览~~、~~文件管理器 UI~~（批次 K/J + 本批已完成）；仍缺：宿主机拷文件工具、ACL/xattr、setuid/sticky | 宿主机 ↔ 镜像拷文件工具、更完整的 Unix 权限语义 |
| ④ | **TCP/IP** | 只有 IPv4/ARP/ICMP、静态地址、无中断收包 | DHCP/DNS/UDP/TCP；vmware vmxnet3 适配 |
| ⑤ | **EHCI / xHCI / 集线器 / 鼠标 / U 盘写** | UHCI（USB 1.1）已能驱动 **HID 引导键盘 + U 盘（只读）**（批次 O）；仍缺：EHCI(USB 2.0)/xHCI(USB 3.x)、hub、USB 鼠标、**U 盘写（`WRITE(10)`）**、拔出检测（热插拔） | 更快的 U 盘、鼠标、带 hub 的真机、往 U 盘写文件 |
| ⑥ | **多核调度** | AP 起来后只是 `cli; hlt`；无 IPI、无 per-CPU 数据 | 真 SMP：AP 参与调度与中断；x2APIC |
| ⑦ | **设置页接 store** | store 本体可用，设置页/启动项未接线 | 分辨率/缩放/窗口位置能真的保存 |
| ⑧ | **真机验证** | 只在 QEMU + VMware 验证过 | 真机驱动覆盖（ACPI 变体、EHCI/xHCI、网卡）与稳定性 |
| — | ~~**Rust 参与实现**~~ | **批次 P1 已完成**：`gui_rs`（`#![no_std]`、无 alloc/无浮点）作为设计 Token + 主题配色真源，编成 `gui_rs/gui_rs.o` **只链进系统内核**（安装介质 0 个 Rust 符号） | —— |

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
├── gui_rs/                   # ★ Rust 侧（批次 P1）：设计 Token + 主题配色真源（只链系统内核）
│   ├── build_rs.sh           # rustc（rustup shim 绝对路径，target x86_64-unknown-none）-> gui_rs/gui_rs.o
│   ├── Cargo.toml            # crate 元数据（实际用 rustc 直接编；gui_rs.o 是构建产物，不进仓）
│   └── src/                  # lib.rs / tokens.rs（33 个 Token）/ theme.rs（7 套主题）/ panic.rs（panic 钩子）
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
│   ├── console64.cpp/.h      # ★ 开机滚屏引导控制台（dbg64 出口镜像 -> 16 KiB 环形缓冲 -> 回放/滚屏 + dmesg）
│   ├── ata64.cpp/.h          # PATA PIO + IRQ14 等待（超时回退轮询）
│   ├── ahci64.cpp/.h         # AHCI(SATA) 驱动（非队列 DMA + 轮询）
│   ├── nvme64.cpp/.h         # NVMe 驱动（BAR0 64 位 MMIO + admin/I-O 队列 + PRP）
│   ├── part64.cpp/.h         # 分区表（MBR/GPT）+ 新建/删除/格式化 + 安装引擎（含 ESP）
│   ├── setup64.cpp           # Win10 同款安装向导（无密钥、真分区、真进度、自动重启）
│   ├── gui64.cpp/.h          # 桌面外壳（窗口 / **居中靠下 Dock**（y=屏高−76、高 60、圆角 24、图标 46、间距 11）/ 开始菜单 / 脏矩形 / 桌面图标）
│   ├── explorer64.cpp/.h     # 文件资源管理器 + 此电脑（导航 / 面包屑 / 双视图 / 右键菜单 / 文件操作）
│   ├── calc64.cpp            # 计算器（Q16.16 纯整数）
│   ├── mines64.cpp           # 扫雷（三难度 / 右键标旗 / 键盘）
│   ├── terminal64.cpp        # 终端（命令：ps/run/elfrun/vol/df/ping/store/dmesg/rust/useradd/su/sudo/loginctl…）
│   ├── settings64.cpp        # 设置（显示 / 缩放 / 语言 / 会话 / 设备规格 / 关于）
│   ├── taskmgr64.cpp         # 任务管理器（进程页=真进程 / 性能页含显卡项 / 启动 / 详细信息）
│   ├── theme64.cpp/.h        # ★ 设计 Token 唯一真源（圆角/模糊/透明度/双层阴影/动效 + 7 套主题 + 主题切换）
│   ├── gfx64.cpp/.h          # ★ 现代图元层（圆角抗锯齿 / 双层阴影 mask 缓存 / 毛玻璃缓存 / 壁纸 6 适应模式）
│   ├── img64.cpp/.h          # ★ 图像解码（PNG 色型 0/2/3/4/6 + 多块/动态 Huffman；BMP；JPEG 未支持）
│   ├── locklogin64.cpp/.h    # ★ 锁屏 + 登录界面（72/18px 时间日期、背景清晰→模糊 20px、104px 头像、ESC 反向）
│   ├── userdb64.cpp/.h       # ★ 多用户骨架（/etc/users.db v1：加盐 SHA-256×1000、会话身份 su/sudo/exit）
│   ├── rust64.h              # ★ gui_rs（Rust）C 链接声明（Token / 主题 / 交叉核对 / panic 钩子）
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
├── tests/                    # ⑤ 验收脚本（54 个 .py / 断言 1,000+ 条：串口 + 像素 + 字节）
│   ├── status_report.py      # 状态核查（每项能力绑证据：DONE / PARTIAL / MISSING）
│   ├── boot64_assert.py      # 长模式 / IDT / PIT / BootInfo
│   ├── install_flow_test.py  # 端到端安装 + 装完单独启动
│   ├── partition_ops_test.py # 新建 / 格式化 / 删除 真实写盘（字节级）
│   ├── iso64_install_test.py / iso64_usb_test.py            # 光盘 / U 盘形态
│   ├── vmware_install_test.py / uefi64_install_test.py       # VMware BIOS / UEFI 全流程
│   ├── esp_install_test.py / disk_boot_test.py / usb_boot_both_fw_test.py   # 安装盘与双固件
│   ├── ahci64_test.py / nvme64_test.py                       # SATA / NVMe 目标盘
│   ├── explorer64_test.py / fileops64_test.py                # 文件管理器与文件操作（像素 + 鼠标注入）
│   ├── rust64_test.py / gfx64_test.py / gui_modern64_test.py / locklogin64_test.py   # ★ P1/P1c：Rust 接入 / 现代图元 / 新 Dock / 锁屏登录
│   ├── startmenu64_test.py / panels64_test.py                # ★ P2：开始菜单 / 四弹窗 + 设备 toast
│   ├── perm64_test.py                                        # ★ P4：卷 v4 + rwx 真拦截 + su/sudo 提权
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
└── docs/                     # ⑥ 中文文档（16 篇）与截图
    ├── VimtuOS系统介绍.md      # 对外介绍（含桌面截图）
    ├── 项目状态总览.md         # 能力项 × 证据 × 发布记录
    ├── 应用层与系统调用说明.md  # VAP64 / ELF64 / 两套 syscall / 进程与地址空间 / 离 glibc 差什么
    ├── 发布流程.md             # 版本号规则与发布步骤（每次发布新建标签）
    ├── 真机验证指南.md         # U 盘写入 / BIOS 设置 / 逐阶段期望 / 故障排查 / 兼容性矩阵
    ├── VMware验证指南.md       # VMware 建虚拟机与验证步骤（guestOS 必须 other-64）
    ├── UEFI引导说明.md · UEFI地址空间实验报告.md · 引导链架构.md
    ├── 安装程序说明.md · ISO安装介质说明.md · 桌面栈移植说明.md
    ├── 中断调试记录.md · 重启与任务管理器设计.md · 字体许可说明.md · 64位升级验证记录.md
    ├── fonts/                 # 四份许可全文（OFL-NotoSans / OFL-NotoSansSC / OFL-Sarasa / UNIFONT-LICENSE）
    ├── screenshots/           # 自动抓取的界面截图（桌面 / 此电脑 / 管理器 / BSOD / 任务管理器…）
    └── shots/                 # 历史验证截图集
```

仓库规模：内核 `kernel/` 118 文件 / 58,381 行、引导 `boot/` 12 文件、Rust `gui_rs/` 7 个文件、验收 `tests/` 54 脚本、
文档 `docs/` 16 篇；**发布进仓库的文件 306 个 / 约 8.2 MB**（含 `docs/screenshots/`、`docs/shots/` 的 PNG 截图与字体
许可全文；第三方字体、打包的交叉编译器、构建产物与退役的 32 位工程都不进仓，见 `.gitignore`）。


## 十、开源与发布状态

**本仓库已开源：<https://github.com/Fanqi89/VimtuOS>** —— 只发**源码**（**306 个文件 / 约 8.2 MB**，含 `docs/` 截图与字体许可全文；`gui_rs/gui_rs.o` 等构建产物不进仓），许可 **GPL-3.0**。

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

**版本与发布**：标签 `v0.3.1-beta13`，Release：<https://github.com/Fanqi89/VimtuOS/releases/tag/v0.3.1-beta13>
（历史版本各自保留安装程序：`v0.3.0-beta12` / `v0.2.3-beta11` / `v0.2.2-beta10` / `v0.2.1-beta9` / `v0.2.1-beta8` / `v0.2.1-beta7` / `v0.2.1-beta6` / `v0.2.0-beta.5` / `v0.2.0-beta.4` / `v0.2.0-beta.3` / `v0.2.0-beta.2` / `v0.1.0-beta.1` —— 见 <https://github.com/Fanqi89/VimtuOS/releases>）
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
a **real file system (VimtuFS2)** with CRC-protected superblock/bitmap/128B inodes, **settings persistence**
(two slots with generation numbers and double CRC32, verified across a real reboot), **ring3 user mode with
two syscall entries** (`int 0x80` with its own ABI, and the `syscall` instruction with Linux x86_64 numbers),
a **self-defined app format (VAP64)** plus an **ELF64 loader** (both install into VimtuFS2 and run in ring3),
**ACPI parsing + LAPIC/IOAPIC takeover + SMP AP bring-up**, an **EDID runtime display layer**, an
**e1000 network stack (ARP/ICMP)** and a **UHCI USB host with HID boot keyboard**.

The latest batch adds a **Windows 11-style look**: design tokens (`theme64.h`), rounded corners, double soft
shadows, acrylic glass, a bottom-centred Dock, **7 themes** and **6 wallpaper fit modes** (`gfx64`/`img64`,
PNG/BMP decode — JPEG is not supported), a **Rust `gui_rs` crate** (`#![no_std]`, no alloc, no float) as the
token/theme source of truth linked into the **system kernel only** (0 Rust symbols in the installer kernel),
and a **lock screen + login + multi-user skeleton** (`/etc/users.db`, salted SHA-256 x1000; `su`/`sudo` switch
only the session identity; permission bits are **not** enforced yet — that is P4, and the salt is not a CSPRNG).

Honest boundaries: user address spaces are **per-process** on the BIOS path (proc64: own CR3 +
fork/execve/wait4/kill) and degrade to a shared window on UEFI firmware tables — a runtime CR3
experiment (A: attach the user window into the firmware PML4 with a short CR0.WP window; B: own PML4 +
`mov cr3`) **succeeded on both QEMU/OVMF and VMware EFI** in an opt-in build, but it is off by default
(see `docs/UEFI地址空间实验报告.md`); the ELF64 loader is only
verified with **our own static binaries** (glibc/distro binaries are untested: no vDSO, signals, futex or
dynamic linking); VFS v3 has a **directory tree** (`/dir/sub/file`, `.`/`..`; names ≤31B, ≤512 inodes, files
now up to 8 MiB (2-level indirect blocks, batch M), empty-directory `rmdir` only; v2 volumes still mount) and the terminal's file
commands (`ls/cat/write/touch/rm/mkdir/df`) use the real VimtuFS2 through the fd64 layer (ring3
`open/read/write/close` share it). Since batch D the fd layer is **per-process** with
reference-counted open-file objects: `dup`/`dup2` share one object (shared offset), `fork` inherits the
whole table, `execve` keeps fds (no `O_CLOEXEC` yet), `O_APPEND` is real and `pipe(22)` works
(64-byte ring buffer, non-blocking). There is no TCP/IP stack and

Everything was validated on **QEMU and VMware, BIOS and UEFI — not on bare metal**.

Quality-wise, every change is verified by assertion scripts (the `tests/status_report.py --full` list alone runs
40 scripts — including the new `rust64_test` 124, `gfx64_test` 55, `gui_modern64_test` 149 and `locklogin64_test` 88 —
that check serial logs, screen pixels (QEMU screendumps) and raw disk bytes).
Tooling adds ~3k lines of self-written Python (ISO9660/GPT/FAT32/VAP64 packing, font subsetting, PE/FAT diagnostics).

