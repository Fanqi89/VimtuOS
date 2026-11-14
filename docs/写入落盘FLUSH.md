# 写入落盘（FLUSH CACHE）与 AHCI 卡死恢复

本批（v0.10.0 之后第一批）要解决的是一个**数据安全**问题，顺带拔掉了它暴露出来的
一个**阻断级**盘控缺陷。

## 1. 问题：写盘"成功"不等于"落盘"

到 v0.10.0 为止，三个块设备驱动**都没有 flush 语义**：

* `kernel/ahci64.h` 自己写着"**不做 FLUSH CACHE EXT** —— 上层必须自己发 flush（本版还没有 flush 语义）"；
* PATA 侧只有 `WRITE SECTORS`，NVMe 侧只有 `Write`。

`READ/WRITE` 返回成功只代表"数据进了设备（或它自己的缓存）"。安装收尾最后写下的
**卷超级块**（`part_volume_grow64` 的收尾）与 **GPT 备份头**如果还在设备缓存里，
掉电 / 宿主强杀 VM 之后就是"装完却挂不上卷"。这正是 v0.10.0 之前那一串
"全新安装后重启挂载失败"里唯一没有被排除掉的一半。

## 2. 实现

新增统一入口（`kernel/ata64.h`）：

```c
bool ata64_flush64(int drive);          // 分派与 ata64_write 完全一致
void ata64_flush_all64(const char* why); // 关机/重启前刷"确实有盘"的槽
```

三条真实命令：

| 驱动 | 命令 | 说明 |
|---|---|---|
| PATA | `FLUSH CACHE EXT(0xEA)`，失败回退 `FLUSH CACHE(0xE7)` | 等待与读写同一条路径（IRQ14 优先 + 160 ms 有界超时回退轮询）；ERR 位一律算失败 |
| AHCI | `0xEA`，H2D FIS，`PRDTL=0`（无数据、无 PRDT 项） | 与读写同一把 IO 锁、同一套"轮询 PxCI + 自报失败现场" |
| NVMe | NVM `Flush(0x00)`，NSID = 该命名空间 | 与 `nvme_xfer` 同一条 I/O 队列 |
| USB 存储 | —— | 本版**没有** SCSI `SYNCHRONIZE CACHE(10)`，如实返回 `false` 并打点，绝不假装刷新成功 |

两个真实调用点：

1. **安装收尾**（`kernel/part64.cpp`）：写完卷 + GPT 之后、宣布"装好了"之前刷目标盘；
2. **关机 / 重启**（`kernel/gui64.cpp` 的 `sys_shutdown64()` / `sys_reboot64()`）：
   先 `sysstate64_stop_all64`（不再有新写），再 `ata64_flush_all64("shutdown"|"reboot")`，最后才断电/复位。

**负向对照**（`kernel/ata64.cpp` 的 `ata64_init64()`）：驱动器号 4..7 是保留空洞，
每次启动故意刷一次，必须打印
`[ATA64] flush drive=4 failed path=none reason=nodrive`
—— 没有这一行，"ok" 就有可能是无条件打印的。

## 3. 顺带拔掉的阻断缺陷：设备报错后 PxCI 卡死，整块盘不可用

验证 AHCI 侧 flush 时拿到的现场（QEMU ich9-ahci，干净的 `build64/sysdisk.img`）：

```
[AHCI64] cmd timeout port=0 cmd=0x25 ci=0x1 is=0x40000002 tfd=0x441   <- 一条非法 LBA 被设备拒绝
[AHCI64] port restart retry port=0 cmd=0x25 result=fail                <- ST 位翻转清不掉 CI
[AHCI64] xfer SKIP reason=cmd-slot-busy di=0 ...                      <- 之后每条命令都在这里失败
[STORE64] flush FAILED via=vfs reason=vfs64_write
[CONF64] flush failed rc=1 via=vfs
```

根因：卡住的 `PxCI` slot 位**只能由硬件在命令完成时清**；设备侧已经报错放弃，
硬件就再也不会去清它。于是 `ahci_cmd` 开头的"上一条命令必须已结束"检查永远失败，
**整块盘直到重启都不可用**（配置存不下、卷写不进、flush 也发不出去）。

修法（`kernel/ahci64.cpp` 的 `ahci_port_recover()`）：当 `PxCI` 仍持有 slot 0 时，
不再只翻转 `ST`，而是做一次完整端口重建 —— 停引擎 → 清 `PxIS`/`PxSERR` →
`PxSCTL=0`（COMRESET，让链路重新建立并顺带把 CI 归零）→ 重挂 `PxCLB/PxFB` → `FRE/ST=1`。
选择器在 `ahci_cmd_retry()`：`stuck = (PxCI & 1) != 0` 决定走"重建"还是老的"停→启"。

实测效果（隔离夹具，启动到桌面）：

| | 修前 | 修后 |
|---|---|---|
| `cmd error` | 9（随后整盘不可用） | **0** |
| `[STORE64] selftest` | `flush FAILED` | **PASS**（write=16384 read=16384） |
| 端口恢复打点 | 无 | `[AHCI64] port recover port=2 ci0=0x1 ci_now=0x0 link=up start=ok` |

## 4. 验收证据（全部实测）

`tests/flush64_test.py`（新增；128 MiB 目标盘完整安装 → q35/ich9-ahci 起同一块盘 → 桌面 → 终端 `shutdown`）：

```
[PASS] 安装走完（[SETUP] 安装完成）            -- [SETUP] 安装完成 100%（真实写入 57343 扇区）
[PASS] A1 安装收尾真的发了一次 FLUSH          -- [ATA64] flush drive=1 ok path=pata
[PASS] A2 安装器确认缓存已刷到介质            -- [INSTALL] flush drive=1 ok (cache synced to media)
[PASS] B  负向对照：保留空洞 drive=4 必须失败  -- [ATA64] flush drive=4 failed path=none reason=nodrive
[PASS] C0 AHCI 盘挂上系统卷                   -- [VFS64] mount_system slot=0 drive=8 start=32584
[PASS] C1b 卡死 PxCI 被完整端口重建救回        -- [AHCI64] port recover port=2 ci0=0x1 ci_now=0x0 link=up start=ok
[PASS] C1 AHCI 真的发出 FLUSH CACHE EXT 并收到设备确认 -- [AHCI64] flush dev=0 port=0 ok (cache synced)
[PASS] C2 统一驱动号 8（AHCI）走到 flush       -- [ATA64] flush drive=8 ok path=ahci
[PASS] C3 关机汇总：每条真盘都刷成功          -- [ATA64] flush all why=shutdown drives=1 ok=1 fail=0
[PASS] C4 整链没有 PANIC
=== 汇总：10/10 PASS ===
```

回归（本轮实跑）：

| 测试 | 结果 |
|---|---|
| `tests/flush64_test.py` | **10 / 10 PASS** |
| `tests/ahci64_test.py` | **PASS**（`[AHCI64] selftest PASS`、`read lba=0 count=1 ok`、`[DISK] 8 … bus=AHCI`） |
| `tests/install_persist64_test.py` | **13 / 13 PASS**（含 `volume grow ok moved=24751 total=16646295`、`[CONF64] flush ok via=vfs`、重启后 `ui.theme=3` 读回、宿主侧独立解析卷） |
| `tests/power64_test.py` | **PASS**（关机链路判据一条没放松） |

构建：`build64.sh` 全量 `EXIT=0`，系统内核 **3,306,816 → 3,315,312 B**。

## 5. 已知边界（如实）

* **NVMe Flush 未实测**：本机没有 NVMe 夹具，代码路径已按 spec 写好（`NVM Flush 0x00`），
  判据状态 **UNVERIFIED(env)**，不写成"已验证"。
* **USB 存储没有 flush**：本版没有 `SYNCHRONIZE CACHE(10)`，`ata64_flush64(24..)` 如实返回
  `failed path=usb reason=no-sync-cache-command-in-this-build`。
* **AHCI 侧 ATAPI 读仍然失败**（`cmd timeout port=2 cmd=0xA0`）：SATA 光驱只能识别不能读，
  ISO 安装仍必须用 IDE 光驱。本批的端口恢复让它在失败后**不再拖垮整块盘**，但读盘本身没修。
* **非法块指针读取仍然存在**（未修，仅记录）：PATA 启动日志里能稳定看到
  `[VFS64] BAD block=71 not in data area [6,64)` —— 有调用方把越界块号送进读路径；
  在 AHCI 上它会变成一条非法 LBA 命令。本批修的是"这一条之后整块盘别死"，不是它本身。
* **驱动器自己的 FUA/缓存策略不在我们手里**：`flush ok` 只代表设备确认刷新完成。

## 6. 打点契约（验收 grep 用，勿改）

```
[ATA64] flush drive=<n> ok path=pata|ahci|nvme
[ATA64] flush drive=<n> failed path=<pata|ahci|nvme|usb|none> reason=<why>
[ATA64] flush all why=<shutdown|reboot> drives=<n> ok=<k> fail=<m>
[AHCI64] flush dev=<idx> port=<p> ok (cache synced) | FAILED (...)
[AHCI64] port recover port=<p> ci0=0x.. ci_now=0x.. link=up|down start=ok|fail
[AHCI64] port restart retry port=<p> cmd=0x.. via=recover|restart result=ok|fail
[NVME64] flush nsid=<n> ok (cache synced) | FAILED
[INSTALL] flush drive=<n> ok (cache synced to media) | FAILED (...)
```
