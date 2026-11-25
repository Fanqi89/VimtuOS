> [← 返回主页](../README.md) · VimtuOS 文档体系

# VimtuOS v0.10.10 — 安装向导：用户与时区页（含 /etc/user.conf 落盘与登录对接）

> 一句话：**安装向导第 2 步与第 3 步之间多了【用户与时区】页** —— 填用户名/密码（可留空）、
> 用下拉框选时区（默认 China / Asia/Shanghai，文案跟着第一步选的语言变），
> 点"安装系统"后这些值以**加盐 SHA-256 哈希**写进目标盘的 `/etc/user.conf`；
> 开机登录读它：**密码留空就跳过校验直接进桌面**，有时区就按时区显示时钟。

本版**不动**整体 UI 风格、**不动**任何底层磁盘分区/镜像克隆逻辑（安装数据通路一行未改）。

---

## 1. 页面位置与交互

### 1.1 位置

```
语言 -> 现在安装 -> 许可条款 -> 安装类型 -> ★【用户与时区】-> 磁盘与分区 -> 安装中 -> 完成
```

代码里的顺序即 `kernel/setup64.cpp` 的 `enum SetupPage`：
`PAGE_LANG / PAGE_NOW / PAGE_LICENSE / PAGE_TYPE / ★PAGE_USER / PAGE_DISK / PAGE_PROGRESS / PAGE_DONE`。

### 1.2 交互（键盘与鼠标都能走完）

| 元素 | 默认值 | 说明 |
|---|---|---|
| 用户名 | `vimtu` | 1..15 个可打印 ASCII（不含空格/斜杠/冒号/等号）；非法输入会被拒绝写入 |
| 密码 | **空** | 屏幕上只回显 `*`，**明文永不落盘、也永不进串口**；页面明写"留空代表无密码" |
| 时区 | `China / Asia/Shanghai`（+480） | 下拉框（无地图），展开 7 行可见、其余滚动 |

键盘：`Tab` 在 用户名 → 密码 → 时区 之间切换；`↑/↓` 改时区（展开时移动选中项）；
`Enter` 展开时收起、未展开时 = 下一步（**全用默认值也能一路回车走完**）；`Backspace` 退格。

鼠标：点输入框聚焦、点时区框展开/收起、点列表某行选中。

### 1.3 国际化动态联动（需求第 4 条）

下拉框**显示**的文案随第一步选的语言切换，**写盘**的永远是标准时区串：

| 第一步选的语言 | 下拉框显示 | 写入 `/etc/user.conf` |
|---|---|---|
| 中文（简体，中国） | `中国` | `timezone=Asia/Shanghai` |
| English (United States) | `China` | `timezone=Asia/Shanghai` |

下拉框右侧同时显示标准串（选哪行一目了然写的是哪个 zone）。时区表共 17 项（中国/港澳台/
日韩/新/印/阿联酋/俄/德/法/英/美东/美西/巴西/澳/新），每项带 `off_min`（相对 UTC 分钟数）。

> 为什么表里要带 `off_min`：内核没有 tzdata、也没有时区库。安装期把"标准串 + 该串的偏移"
> 一起写下去，登录与时钟直接读偏移；标准串保留作为语义（也是将来接 tzdata 的接口）。

---

## 2. 落盘：目标盘的 `/etc/user.conf`

### 2.1 写在哪、什么时候写

`part_install_step()` 返回 `1`（= 载荷已复制完、卷超级块与 CRC 都收尾完毕）之后、
自动重启（2.5 s）之前，`install_write_user_conf()` 执行：

```
vfs64_slot_alloc64()                    // 找一个空卷槽（不激活、不覆盖已有卷）
vfs64_mount_slot64(slot, drive, lba)    // 把**目标盘目标分区**的卷挂进该槽
vfs64_mkdirs_on64(slot, /etc/user.conf) // 建父目录 /etc（已存在当成功）
vfs64_create_on64(slot, /etc/user.conf) // 先建空文件
vfs64_write_on64(slot, /etc/user.conf, text, len)
```

★ **为什么复用 vfs64 的"按槽操作"一族**：目标盘上那块卷就是标准 VimtuFS2。
`*_on64` 系列本来就是"调用期间临时切到该槽、返回前切回、身份按 root"的既定语义，
是内核里唯一经过验收的卷写入路径。自己再写一套"分配块 + 改位图 + 写 inode + 算 CRC"的
离线写入器属于重复实现，且极易与内核侧口径漂移 —— 而且**分区/克隆逻辑一行都不用碰**。

### 2.2 文件格式（纯文本、行式、未知键忽略）

```
# VimtuOS /etc/user.conf - written by the installer wizard
# plaintext password is never stored; pass_* is salted sha256 (1000 rounds)
user=vimtu
pass_algo=sha256-iter1000
pass_set=0
timezone=Asia/Shanghai
tz_offset=480
lang=0
```

密码非空时多两行（`pass_salt=<32 hex>` / `pass_hash=<64 hex>`），算法与 `/etc/users.db`
**完全相同**：`h0 = SHA256(salt(16B) || pw)`，再叠 999 轮 `h = SHA256(h || salt)`。

> ★ 解析器的容错：`pass_set=1` 但 salt/hash 缺失 → **降级成 `pass_set=0`** 并打 `warn=8`。
> 宁可放行也不能把用户永久锁在门外（代价是那条配置本身是坏的，日志里有痕迹）。

### 2.3 打点（验收用）

```
[SETUP] userconf user=vimtu tz=Asia/Shanghai off=480 pass=0 lang=0 (collected; ...)
[SETUP] user.conf write slot=0 drive=8 lba=32584 path=/etc/user.conf bytes=NNN user=vimtu tz=Asia/Shanghai pass=0 rc=0
[SETUP] userconf applied user=vimtu tz=Asia/Shanghai off=480 pass_set=0 lang=0 (login reads this file at boot)
[USERCONF] load path=/etc/user.conf user=vimtu tz=Asia/Shanghai off=480 pass=0 lang=0 bytes=NNN warn=0
```

失败路径都有 `reason=`：`no-target` / `no-free-slot` / `mount-failed` / `BUILD FAILED`。

---

## 3. 登录与时钟对接（`kernel/locklogin64.cpp`）

1. **启动就读配置**：`locklogin64_init64()` 调 `userdb64_userconf_load64()`（幂等），
   并把安装时选的用户名优先作为登录界面默认选中项（找不到才退回"上次登录的用户"）。
2. **口令策略**：只对"配置里那个用户"生效 —— 别的账户仍按 `/etc/users.db` 自己的 hash 走
   （多用户语义不变）。
   * `pass_set=0` → `submit_login64()` 直接 `do_login64()`，**跳过密码校验进桌面**；
   * `pass_set=1` → 弹密码框，按 `user.conf` 的 salt+hash 校验；输错清空输入、打
     `[LOGIN64] password wrong`、`attempt=N`。
3. **时区生效**：`refresh_time64()` 改用 `userdb64_local_time64/date64`（RTC 是 UTC，
   加 `tz_offset` 后跨零点连日期一起进位）。锁屏/登录页的大号时钟与
   任务栏时钟（`kernel/panels64.cpp` 5 处时间 + 4 处日期）全部走同一份换算。

登录界面那行打点仍是老格式（`password=required|none`），只是**判定依据**变成了
user.conf 的有效策略 —— 老验收脚本不用改判据。

---

## 4. 为了"一份 SHA-256"做的小重构（重要，别退回）

安装程序内核与系统内核是**两遍编译**，安装程序那一遍只链一部分目标文件
（不含 `userdb64.o` —— 它依赖 theme64/gfx64/img64）。而哈希是安装期就要算的。

做法：

| 文件 | 角色 |
|---|---|
| `kernel/sha256_64.h`（新） | SHA-256 + hex 编解码 + `pw_hash64` + `salt_new64`，**header-only（static inline）** |
| `kernel/userconf64.h`（新） | `/etc/user.conf` 的解析 / 生成 / 校验 / 本地时间，**header-only** |
| `kernel/userdb64.cpp` | 删掉自己那份 SHA-256，改为 `#include "sha256_64.h"`（调用点名字不变） |

这样**不用改任何链接表**，两份内核共用同一份实现；也避免"装的时候设的密码，开机验不过"。

---

## 5. 验收

### 5.1 走完向导的按键序列（新增页 = 多一次回车）

`tests/install_flow_test.py` / `tests/esp_install_test.py` / `tests/uefi_install64_test.py` /
`tests/vmware_install_test.py` 的按键序列都在"类型 -> 磁盘"之间补了一次回车
（默认值：`vimtu` / 密码留空 / `Asia/Shanghai`）。

### 5.2 新增判据（只加强不改弱）

`install_flow_test`：
```
[PASS] 向导收集到用户名/时区（[SETUP] userconf user=）
[PASS] 安装收尾写入目标盘 /etc/user.conf（slot=/drive=/lba=/path=）
[PASS] user.conf 写入无失败（rc=0 / 无 SKIP / 无 BUILD FAILED）
[PASS] 安装默认值：用户名 vimtu + 时区 Asia/Shanghai
```
`uefi_install64_test`：`[PASS] 安装收尾写入 /etc/user.conf`。

### 5.3 实测记录（本版真实产物）

```
内核 kernel64_os.bin   = 3,354,896 B
安装程序 kernel64.bin  = 1,894,160 B
sysdisk.img           = 38,167,040 B（522 个文件；卷 total 41,961 扇区 / data_start 524）
iso  = 93,136,896 B  sha256 D33B00A51E8D891AC5DE62AEFD00E8FD9E5D416BD58A443F0D6F68D38787184D
img  = 42,361,856 B  sha256 BF6D62A67D5D503626D97CDE9478EFF43C630EB139CAB3CBADB26BE8FFF42188
```

**绿**（本次实测）：

| 测试 | 结果 | 说明 |
|---|---|---|
| `install_flow_test`（BIOS 装机） | **PASS** | 新增 4 条判据全绿；宿主侧逐文件校验通过（其余 522 个文件 size+crc32 与载荷逐个一致、几何未 relayout、`/etc/user.conf` 内容正确） |
| `uefi_install64_test` | **11/11 PASS** | 新增 2 条（user.conf 写入 + 无 SKIP/BUILD FAILED）；`pa=0x10000000`、ESP 合法 FAT32 全绿 |
| `locklogin64_test` | **104/104 PASS** | 口令流程、默认选中"上次登录用户"、只选 root 等既有契约全部保持 |
| `panels64_test` | **PASS** | 任务栏时钟改走时区换算后无回归 |
| `settings64_test` | **PASS** | 设置页口令/主题路径（同样走 userdb64）无回归 |
| `sysstate64_test` | **PASS** | 系统状态面板（含时钟）无回归 |

**口令哈希路径端到端自证**（`$PI_SCRATCH_DIR\p47_pw_e2e.py` + `p48_pw_boot.py`，装机时输入 `s3cret42`）：

```
[PASS] 安装时收集到 pass_set=1（口令非空）
[PASS] /etc/user.conf 在卷里且 size>0                     size=345
[PASS] 卷里有 pass_set=1 / pass_salt= / pass_hash=
[PASS] 卷里**没有**明文口令（s3cret42）                    ← 明文不落盘
[PASS] 卷里有标准时区串 Asia/Shanghai
[PASS] 登录界面 password=required
[PASS] 按回车后弹密码框
[PASS] 错口令被拒（[LOGIN64] password wrong）
[PASS] 错口令后没有进桌面                                  login ok 次数=0
[PASS] 正确口令进桌面（[LOGIN64] login ok user=vimtu）
```

开机侧同时能看到 `[USERCONF] load … user=vimtu tz=Asia/Shanghai off=480 pass=1 bytes=345 warn=0`，
说明配置真的从目标盘的 `/etc/user.conf` 读出来了（不是内存里传下来的）。

### 5.4 未绿的三条（如实标注，均与本次改动无关，证据在下面）

| 测试 | 结果 | 判定与证据 |
|---|---|---|
| `esp_install_test` | FAIL（1 条） | 卡在 `128MB PATA：写路径没有 irq14 超时回退` —— 串口里 `[ATA64] irq14 timeout -> fallback to polling` 出现在**载荷复制收尾**（`[SETUP] page=6` 之后、`[INSTALL] 完成` 之前），**早于**本版新增的 user.conf 写入；且回退生效、安装照样成功。v0.10.9 的回归里这条是绿的 → **flan（小盘 PATA 时序）**，非本版回归 |
| `sh64_test` | 58/59 | 卡在"串口行原子性自检"：命中 2 行（`[FAT64] mkdir … [TASK64] kheart`、`[DESK64] selftest … [TASK64] kheart`）—— 是 `fat64.cpp` / `desktopops64.cpp` 里**老的多段拼行没加行锁**，与 v0.10.9 修 `[GUI64] icon fallback` 同类；v0.10.9 的 sweep 里这条同样是 58/59 → **既有潜在缺陷 + 时序抖动** |
| `perm64_test` | 93/95 | 两条期望值相对**当前镜像**已过期：①"格式化块数 = 主分区扇区数（24759）"——v0.10.9 起镜像按内容伸缩，现为 41,961；②"那一刻 /etc、/home 不存在"——现镜像里 `/etc` 早已存在（`/etc/fonts.conf`、`/etc/r3drv.conf`…）。该测试**不在 v0.10.9 的回归清单里**，属测试夹具漂移 |

★ 这三条都不是本版功能引入的，本版也没有把它们改判据糊过去；已如实留给下一次清理。

---

## 6. 明确未做的（如实标注）

| 项 | 状态 |
|---|---|
| 时区**夏令时**（DST） | **未做**。表里存的是"该时区标准偏移"，北美/欧洲的夏令时不会被切换。真要做需要 tzdata 表或规则引擎 |
| 时区列表 | 17 个常见区，不是全量 IANA 记录 |
| 用户名对 `/etc/users.db` 的联动 | 安装期**只写 `/etc/user.conf`**，不新建/改写 users.db 用户表（用户表由系统首启建立）。若安装时填了 users.db 里不存在的名字，登录界面不会凭空多出一个账户 —— 这是刻意的最小改动 |
| 密码强度校验 | 未做（不设最小长度） |
| 真机 UEFI 首测 | 未做（本版 UEFI 证据仍来自 VMware EFI，EDK2 同族） |
