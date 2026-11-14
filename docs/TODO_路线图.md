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
