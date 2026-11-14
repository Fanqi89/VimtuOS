# TODO 路线图（未做 / 暂缓 / 待复验）

本文件只记**尚未完成或暂时不做**的事项，每条写清：现状、为什么现在不做、补的时候要满足什么判据。
已完成且验收过的能力见 `tests/status_report.py` 与 `docs/开发历程.md`。

---

## 1. USB 存储的 FLUSH（SCSI `SYNCHRONIZE CACHE(10)`）—— **暂缓（用户决定，v0.10.1 起）**

**现状（如实）**：`kernel/ata64.cpp` 的 `ata64_flush64()` 对驱动器号 24..（USB 存储）**不发任何命令**，
直接返回 `false` 并打点：

```
[ATA64] flush drive=<24..> failed path=usb reason=no-sync-cache-command-in-this-build
```

即：**U 盘写入目前没有落盘语义**。安装收尾 / 关机刷盘时 U 盘那一条会记成 fail（不假装成功）。

**为什么现在不补**：USB 侧的写路径（BOT 状态机 + `WRITE(10)` + 写后读回校验）还在演进阶段，
在状态机还不稳的时候加 `SYNCHRONIZE CACHE(10)`（CDB 0x35，同样走 BOT：CBW → 数据阶段 0 字节 →
CSW 检查）只会把"命令超时 / CSW 残留"这类问题混在一起。先把写通道彻底做稳。

**补的时候的判据（不许放松）**：
1. CBW：`dCBWSignature=0x43425355`、`bmCBWFlags=0`（Host→Device，无数据）、`dCBWDataTransferLength=0`、
   `CB[0]=0x35`，`bCBWCBLength=10`；
2. 收尾只看 **CSW**：`dCSWSignature=0x53425355`、`bCSWStatus=0`，`dCSWResidue` 必须为 0；
3. 成功行：`[USBST] flush drive=<n> ok (SYNCHRONIZE CACHE(10), CSW status=0)`；
4. 负向对照：故意发一个 `bCSWStatus != 0` 的用例，必须打 `failed reason=csw-status=<n>`，绝不吞；
5. 端到端：U 盘上 `cfg save` + 宿主侧 `dd` 读回镜像逐字节比对；
6. 回归：`tests/peload64_test.py`、`tests/drv64_test.py` 与 U 盘相关用例不得回退。

**顺带要一起做的**：`ata64_flush_all64()` 目前的汇总行把 U 盘算进 `fail=`；
补完这条之后，文档 `docs/写入落盘FLUSH.md` §5 的"USB 无 flush"边界要一起删掉。

---

## 2. NVMe 的 Flush 路径 —— **代码已就位，判据 UNVERIFIED(env)**

`nvme64_flush64()` 已按 spec 发 NVM `Flush(0x00)`（NSID = 该命名空间），但本机**没有 NVMe 夹具**，
从未实测过。补的时候：给 QEMU 挂 `-device nvme`，走 `ata64_flush_all64()`，断言
`[NVME64] flush nsid=<n> ok (cache synced)` 且 `[ATA64] flush all … fail=0`；
若控制器不支持 Flush（返回 Invalid Field），要如实保留 `FAILED` 并记录到已知边界。

---

## 3. 越界块指针 / 非法 LBA —— **v0.10.1 已结案（保留记录）**

`[VFS64] BAD block=<n> not in data area [<ds>,<blocks>)` 说明有调用方把越界块号送进读/写路径。
在 AHCI 上它会变成一条**非法 LBA 命令**，把设备打进错误态（已有端口恢复兜底，但源头必须钉死）。
v0.10.1 的做法：给打点加 `caller=0x…`（`__attribute__((noinline))` + `__builtin_return_address(0)`），
用 `addr2line` 逐条钉死调用点，然后**在源头拒绝**而不是靠下游 `blk_ok_data()` 兜。

---

## 4. AHCI 侧 ATAPI 光驱读 —— 未做

SATA 光驱能识别（`kind=atapi`、型号正确），但 `PACKET(0xA0)` 命令超时（`cmd timeout port=<p> cmd=0xA0`），
读盘失败。当前后果：ISO 安装**必须用 IDE 光驱**。补的时候要满足：`[AHCI64] atapi read lba=<n> ok`、
ISO9660 主卷描述符读回、`tests/ahci64_test.py --strict-dma` 不退化。

---

## 5. `tests/wm64_test.py` ⑦ 的两条断言 —— **v0.10.1 已修完（88/88）**

`[WM] exit handback … px=0`（期望 Σ窗口面积）与"窗口外逐字节不变 `outside_diff=`"。见 v0.10.1 记录。

---

## 6. UEFI 固件引导 —— v0.10.1 已复验 PASS，之后每版都要跑

安装器已写 ESP（`BOOTX64.EFI + UEFI64.BIN + KERNEL64.BIN`）与盘尾备份 GPT，但**固件引导本身**
一直是 `UNVERIFIED(env)`（本机没有 OVMF 夹具时无法复验）。判据见 `docs/UEFI引导说明.md`。

---

## 7. 外壳交回后的"桌面一致性"（下一批候选，v0.10.1 记录）

现状：wm 接管屏幕时抓到的背景快照是**黑屏**（此时内核外壳还没画过自己的桌面），所以 wm 交回时
"还回去的桌面"是黑的，随后由内核外壳重画；外壳自己还会继续重画（同一对帧 `erased2=29813 /
painted2=30151`）。v0.10.1 的 ⑦ 判据已按此收口（erased=0 / painted>0 / 调色板不回来）。

真要做到"交回后与原桌面逐字节一致"，两条路（按代价排序）：
1. 外壳**先画一帧桌面**再启动 wm（wm 的背景快照因此含壁纸/图标），然后交回即还原；
2. 外壳提供"桌面重绘"接口，wm 交回时请求外壳重画一块矩形并等它完成。
判据：`before_exit` 与 `after_exit` 在窗口之外逐字节相同（旧口径重新成立），且 `erased=0`。

## 8. dpkg 装 `.deb`：还差什么（v0.10.3 更新）

> **★ v0.10.3 已关闭下表前三条**（表格本身保留为当时的审计记录）：
> * **zstd 解码** → `user/store/vs_zstd.c`（自写 RFC 8878 帧解码器，**零依赖**），
>   `.deb` 压缩壳变成 gz / xz / **zst** 三条路；
>   验收 `tools/zstd_selftest.py`：**344/344 PASS**（真 zstd 造的样本逐字节比对 + 截断负向对照），
>   细节与踩坑记录见 `docs/zstd解码器.md`。
> * **符号链接** → tar `type=2` 不再整包拒绝，按内核 syscall 88 建**真链接**
>   （相对目标由内核按“链接所在目录 + 目标”解析），并记进已装文件清单（卸载时删除）；
>   验收 `tests/dpkg64_test.py` 的 **G / H** 两条判据（相对链接 / 绝对链接各一条，都要能 `cat` 出内容）。
> * **上限** → 包 ≤ **16 MiB**、`control.tar` 解出 ≤ **1 MiB**、`data.tar` 解出 ≤ **16 MiB**
>   （上限由系统卷 ~12.6 MiB 决定，不是拍的）。
>
> 下表里**仍然成立**的是：维护者脚本、`/var/lib/dpkg/status` 格式、
> 依赖版本区间、落盘权限 0644、已装库 ≤ 8 个包、只能装到系统卷根。

线上唯一引擎是 `user/store/vs_pkg.c`（`/bin/vpkg` = `/usr/bin/dpkg` = `/usr/bin/apt` 同一份字节，
按 `argv[0]` 分前端；`user/dpkg/dpkg.c` 已废弃不编译）。**已能装**的边界：

`.deb` = ar + `control.tar.{gz,xz,zst}` + `data.tar.{gz,xz,zst}`；包 ≤ **16 MiB**；解压后 payload ≤ **16 MiB**；
**带符号链接**（type=2，相对/绝对都行）；无维护者脚本；装到系统卷根；已装库 ≤ 8 个包。
`tests/dpkg64_test.py` **A–I 九条**已证（A0 sudo / A --version / B 安装 / C payload / D 状态 / E apt 拒绝 /
F zstd+symlink 安装 / G 相对链接可读 / H 绝对链接可读 / I 两包共存）。

挡住"真 Debian 包"的（按优先级）：

| 缺口 | 位置 | 阻断 | 量级 |
|---|---|---|---|
| **zstd 解码器缺失**（Ubuntu 21.10+/dpkg 1.21 默认 `data.tar.zst`；`.zst` 成员根本不匹配 → `VS_E_FORMAT`） | `vs_pkg.c:505-508`、`1115-1118` | 是 | 大 |
| **data.tar 解压上限 4 MiB / 包上限 8 MiB** | `vs_pkg.c:1130`、`1345-1349` | 是 | 中 |
| **符号链接条目整包拒绝**（tar type≠0/5 → `VS_E_FORMAT`；本系统无 symlink） | `vs_pkg.c:1196-1199` | 是 | 中 |
| **维护者脚本整包拒绝**（`VS_E_SCRIPTS=9`，`preinst/postinst/…` 存在即拒） | `vs_pkg.c:1161-1166` | 是 | 大（要 sh + fork/exec + 事务顺序） |
| 已装库上限 8 个包 | `vs.h:52`、`vs_pkg.c:1504-1507` | 是 | 小 |
| 无 `/var/lib/dpkg/status`（自有 `/var/lib/vpkg/installed.json`），真 dpkg 工具链读不了 | `vs_pkg.c:320-366` | 否 | 中 |
| `Depends` 版本区间被丢弃；`Pre-Depends/Provides/Conflicts/Breaks` 不解析 | `vs_pkg.c:627-645` | 否 | 小 |
| 落盘一律 0644（tar 的 mode/uid/gid 不读不应用） | `vs_pkg.c` 文件头 25-26 行 | 否 | 小 |
| 只能装到系统卷根（不能选 ext4/FAT 目标卷） | `vs_pkg.c:1230` | 与 §9 交叉 | 中 |

**已经能用、别重做**：ar 解析（含 2 字节对齐/GNU+BSD 名字）、control 字段解析、**gzip 与 xz/LZMA2
真解码**（CRC32/CRC64 真校验）、tar 解包（ustar/`./` 剥离/PAX 长名跳过）、路径穿越防护、
字节/文件数/空间预检、**sha256 与索引比对**、**卸载真删文件 + 逐级 rmdir 收空目录**、
一层依赖 + `/etc/vpkg/provides` 见证、`/etc/apps.json` 注册表增删、14 个可分错误码。

## 9. ext4 写入：还差什么（v0.10.2 审计，**未做**）

现状：**纯只读**。闸门在 `kernel/fs64.cpp:235`（`v.readonly = 1`），九个写入口
（`fs64.cpp:742/759/772/786/795/804/813/822/832`）一律 `-FS64_EROFS`；ring-3 服务
`user/drivers/ext4svc.c` 的协议只有 `STAT/LIST/READ`（`kernel/r3drv_abi.h:107-109`）。

**底层块写通道整条已通并已验收**（别重做）：`R3DRV_F_WRITE` → `r3drv64_write64`/`kick` →
`ahci64_write_frames64`（WRITE DMA EXT，一帧一 PRDT 零拷贝）/ PATA `ata64_write` →
`ata64_range_ok64` 容量闸门。`tests/ext4write64_test.py` 16/16 = guest 写 + 另区回读逐字节 +
宿主独立 CRC + "变化块集合恰好 {16000..16007}"。

缺口（按依赖顺序）：

| # | 缺口 | 位置 | 阻断 |
|---|---|---|---|
| 1 | **多块组**：inode 表硬编码第 0 组（GD 只取 +0x08） | `ext4svc.c:195-203` | 是（真 `mkfs.ext4` 卷多块组） |
| 2 | **64BIT / metadata_csum / flex_bg / journal 重放**：夹具刻意不含，真卷必有 | `ext4svc.c:194`、`tools/mkfs_ext4_win.py:465-468` | 是（真卷挂不上） |
| 3 | 协议缺写 op：`WRITE/CREATE/UNLINK/MKDIR/TRUNCATE/RENAME/STATFS` | `r3drv_abi.h:107-109`、`ext4vol64.h:34-41` | 是 |
| 4 | fs64 的 ext4 只读闸门（去掉 `readonly=1` 后九个入口要各有 EXT4R 分支） | `fs64.cpp:235` 等 | 是 |
| 5 | 块分配（`bg_block_bitmap` + 位图回写 + 组/超级块空闲计数） | 无 | 是 |
| 6 | inode 分配（`bg_inode_bitmap` + inode 表写入 + 计数 + `s_first_ino`） | 无 | 是 |
| 7 | extent 树插入/分裂（现在只读，`depth ≤ 1`） | `ext4svc.c:518-552` | 是 |
| 8 | 目录项插入（切 `rec_len` 或追加块 + 更新 `i_size`/`i_links_count`） | `ext4svc.c:555-583` | 是 |
| 9 | jbd2 日志（真卷带 `has_journal`；不写日志直接改元数据 → e2fsck 报错）；或明确"仅无日志卷" | 无 | 是（对真卷） |
| 10 | ring-3 侧 FLUSH 入口（`ahci64_flush64` 已实现但 ring3 够不着） | `r3drv_abi.h:48` | 否（但断电必损） |
| 11 | 时间戳 / inode 校验和 / STATFS / O_CREAT 的 mode | 无 | 否 |

**结论（如实）**：ext4 不是"接一根线"，缺的是**整套元数据写引擎**；而在 ext4 可写之前，
"把 `.deb` 装到 ext4 卷"这条需求本身无解（§8 最后一行与 §9 第 3/4 条交叉）。
建议顺序：先做 §9 的 1+2（读侧通真卷），再做 3+4+5+6（能建空文件/分配块），再做 7+8（能写目录项），
最后决定 jbd2 与 FLUSH 的语义。

## 10. 真机（v0.10.2 之后）

见 `docs/真机启动加固.md` 的 §3：8 条"必须在真机上验证才有意义"的假设。真机首测要抓的原始行：

```
U:size SYSTEM.IMG = …            U:payload sectors=…
U:bootinfo lfb=… lfb64=… src=gop hi=0|1
[FB64] source=… usable=1 …       [FB64] geom clamp …（若分辨率超 3840x2160）
[G64] … bar0=… mmio=ok|absent
[AHCI64] pci … abar=…            [NVME64] pci … bar0=…（或 skip reason=bar0-above-4g）
```
