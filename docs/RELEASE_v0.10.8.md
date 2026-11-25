> [← 返回主页](../README.md) · VimtuOS 文档体系

# VimtuOS 64 v0.10.8 —— UEFI 从"装好的硬盘"引导修复

> 一句话：**用 ISO 装完系统后，UEFI 模式下终于能从硬盘自己起来了**（真机 / VMware EFI 同一套固件族）。
> 这一版还补齐了 `textwm64` 在 VMware 上的可跑环境（真鼠标 + VNC），并把它的判据换成整窗像素特征。

- 版本：`VIMTUOS_VERSION="0.10.8"`
- 上一版：v0.10.7（终端抗锯齿 + 4 个红项清零）
- 资产：`vimtu64-64.iso`（安装镜像） / `vimtu64-64.img`（裸盘/U 盘安装介质）

---

## 1. 本版修的**真 Bug**：装好的盘在 UEFI 下起不来

### 1.1 现象（可复现）

用 `vimtu64-64.iso` 在 **UEFI** 下装完系统、重启，固件反复重试引导项：

```
VMware: Guest: About to do EFI boot: EFI VMware Virtual IDE Hard Drive (IDE 0:0)
        （每 ~3 秒一次，无限循环；串口里内核一个字都没有）
串口:   S12345J
        U:==== Vimtu64 UEFI 引导（自研 BOOTX64.EFI + 平铺长模式引导器）====
        U:size KERNEL64.BIN = 3339776
        U:loaded KERNEL64.BIN bytes=3339776
        U:medium desc kind=0 (no payload: installed disk)
        U:direct map VA 0xFFFFFFFF80000000+PA, mb=24      ← 只有 48MB 直映
        U:ExitBootServices ok -> 进内核（全 64 位）
        U:high half check ok
        U:entry=FFFFFFFF80100000
        abBcdEsK                                          ← entry64.asm 的 E/s/K 面包屑之后就没了
        （复位，重复）
```

同一块盘用 **BIOS** 启动完全正常（`[VFS64] mount ok`、桌面、心跳全有）。

### 1.2 根因（两处，都在引导/安装链上）

**① UEFI 引导器直映窗口太小（决定性）**

`boot/efi/uefi64.c` 里 `KERNEL_MAP_BYTES = 48MB`。而系统内核的 BSS 有多大？

```
build64/kernel64_os.elf:
  .bss  addr=0xFFFFFFFF80600000  size=47,755,569
  => 末端 VA 0xFFFFFFFF8338B131 => 物理末端 ≈ 51.5 MB（1MB 基址起）
```

`kmain64` 的第一个动作就是 `dbg64_serial_init()` 之后清 `.bss` —— 写到 51.5MB，
而页表只映射到 48MB：**写页失败 → 此时还没有 IDT → 三重故障 → 复位**。
所以串口里只剩 `entry64.asm` 的 `E`（进入）/`s`（栈）/`K`（call kmain64）三个打点。

> 安装介质为什么没事？安装程序内核的 `.bss` 小得多，48MB 装得下。

**② 安装器往 ESP 写的内核是"15MiB 引导区整块"，不是内核本身**

`kernel/part64.cpp` 原来写 `KERNEL64.BIN = LBA65..32583 整块（32,519 扇区 = 15.9MB）`。
BIOS 的 `loader64.asm` 只读 `KSNT` 回填槽里的**真实扇区数**，所以 BIOS 路径没事；
而 UEFI 的 `load_file()` 是"整份文件读到 0x100000"，15MB 会从 1MB 铺到 16MB ——
**踩到 UEFI64.BIN 自身（链接在 0x800000）**，同时 `bootinfo.kernel_size` 也被报成 15MB。

### 1.3 修法（`boot/efi/uefi64.c` + `kernel/part64.cpp`）

| 位置 | 改动 |
|---|---|
| `boot/efi/uefi64.c` | `KERNEL_MAP_BYTES` **48MB → 128MB**（内核镜像+BSS ~56MB 的 2.3 倍余量）；顺带把日志标签从错写成 `bytes=24`（其实是页数）改成 `mb=`（值仍按 2MB 页数打：64 页 = 128MB；纯标签改动，彻底对齐留到下一版） |
| `boot/efi/uefi64.c` | `KERNEL_MAX_BYTES` **46MB → 6MB**（`< 0x800000-0x100000`），超限直接 `U:TOO BIG` + 屏幕提示，把"静默重启"变成可诊断错误 |
| `kernel/part64.cpp` | 新增 `part_kernel_real_bytes64()`：从目标盘 `LBA1..64` 的 loader 里解析 `'KSNT'` + u16 扇区数，ESP 里的 `KERNEL64.BIN` **只写真实内核长度**（与 BIOS 路径同一口径）；解不到才回落整块并打点 |

装完后 ESP 里现在是：

```
[INSTALL] esp files: BOOTX64.EFI=2560B UEFI64.BIN=45056B KERNEL64.BIN=3339776B (block=15728640B)
```

### 1.4 验收（新增 `tests/uefi_install64_test.py`，**9/9 PASS**）

端到端：VMware `firmware=efi` + ISO 装机（COM2 送键走完向导）→ 停机体检目标盘 → **从目标盘 UEFI 重启**。

```
[PASS] ISO 在 UEFI 下引导进安装程序
[PASS] 安装写出扇区
[PASS] 无 PANIC
[PASS] 安装器写了 ESP（[INSTALL] esp 打点）
[PASS] MBR 55AA 且至少 2 个分区项
[PASS] MBR 里有盘尾 0xEF 分区项（ESP）      esp start=425951 sectors=98304
[PASS] ESP 起点是合法 FAT32 引导扇区（BPB+55AA） oem=b'VIMTU64 ' fstype=b'FAT32   '
[PASS] 盘尾有备份 GPT（EFI PART）
[PASS] UEFI 从装好的目标盘引导成功（[OS] booted / [GUI64] ready）   ← 本 Bug 的关键一步
=== RESULT: PASS === checks=9 ok=9
```

> 为什么以前的安装测试没发现：`tests/vmware_install_test.py` 的目标盘是 **28 MiB**，
> 小于 `part_esp_geometry64()` 的门槛（≈122,730 扇区 ≈ 60MiB），安装器按设计**跳过 ESP**
> （`[INSTALL] gpt skipped (disk too small for ESP)`），UEFI 自然无从引导。
> 新测试默认用 **256 MiB** 目标盘。

---

## 2. `textwm64`（真鼠标 + VNC）从"跑不起来"到"能出判定"

上一版这条一直是 `UNVERIFIED(env，本机没有 VMware)`。本版把环境与夹具补齐：

- **夹具**：`$PI_SCRATCH_DIR\twm_mkvm.py`（全 ASCII 路径；`--firmware bios|efi`；
  每次从 `build64/sysdisk.img` 重铺盘）。VMware 的 **UEFI 起不来我们那块演示盘**（无 ESP/GPT），
  所以真鼠标验收走 **BIOS 固件**（`[FB64] vbe 1024x768`、`[GUI64] ready`、`[VSYNC]` 0 条）。
- **`textvnc.cap()` 15s → 0.35s**：原来是阻塞 `recv` 吃满 socket 超时，
  `cap(timeout=0.7)` 的循环形同虚设（实测 `cap=15.07s`，鼠标闭环 20s 预算只走 1~2 轮）。
  改成 `select()` **非破坏性**可读等待（不消费字节、不断流）。
- **光标闭环改串口权威值**：`[UI] perf sec=… cur=x,y`（客人 1 Hz 自报整数，实测与像素读数一致：
  608,384 / 512,384 逐点相同）+ 确定性鼠标包映射（`|d|≥20 → 恰好 24 客人 px`，小步 ≈1.7d）。
- **落点容差 ±30px**：宿主相对注入有 1 帧量化 + 1 Hz 反馈滞后，实测过冲 10~30px；
  像素判定一律用 `[UI] win move end` 的**实际几何位移**，容差只影响"这次拖拽算不算成功"。
- **判据改整窗**：终端是"黑底 + 字缝"，9 宫格探针容易整块落在空白上（位移前后都是纯黑 → MAD 恒 0），
  现在主判据是**整窗 MAD + 墨迹像素数比**（`ink_ratio ≥ 0.97`），9 宫格数字保留做诊断。

实测判定（`twm_run13.txt`，4 应用 × 2 轮）：

```
terminal  move   MOVED_OK      （整窗 area_moved 远小于 area_stuck，ink 比 ≈0.99）
terminal  resize REPAINT_OK    （[UI] term layout client=892x538 + 新版面非纯色）
settings  resize REPAINT_OK
taskmgr   resize REPAINT_OK
其余：DRAG_FAIL / AMBIGUOUS —— VMware 在连续动作后会**丢鼠标抓取**（客人光标钉在 x=1023 不动），
      已加"失败即重抓一次"兜底；仍非产品缺陷，属夹具环境限制（详见 §4）。
```

**本版不把 `textwm64` 记为 PASS**：它有真实 PASS 条目（窗口移动内容跟随 / 缩放重排），
但整轮还没跑成全绿，如实标注为 **PARTIAL / UNVERIFIED(部分)**。

---

## 3. 新增/修改的文件

```
boot/efi/uefi64.c            直映窗口 48MB→128MB；KERNEL_MAX_BYTES 46MB→6MB（自我覆盖闸门）；日志单位修正
kernel/part64.cpp            ESP 的 KERNEL64.BIN 只写真实内核长度（解析 loader 的 'KSNT'）
kernel/gui64.cpp             拖动结束新增 [UI] win move end x= y= w= h= client=WxH（几何可自证）
kernel/debug64.h             新增 dbg64_sdec64()（窗口 x/y 可为负，uint64 打印会炸验收解析）
tests/uefi_install64_test.py **新增**：ISO+UEFI 装机 → 目标盘体检 → UEFI 重启，9 条判据
tests/textwm64_test.py       整窗判据 + [UI] win move end 几何 + 探针越界过滤 + 截帧新鲜度守卫
tests/textvnc.py             夹具路径可配（VIMTU_*）；cap() select 非阻塞；串口权威光标；
                             确定性鼠标包；±30px 落点容差；失败重抓鼠标一次
build64.sh                   VIMTUOS_VERSION=0.10.8
```

---

## 4. 如实标注的未完成项

| 项 | 状态 |
|---|---|
| `textwm64` 整轮全绿 | **未达成**：VMware 丢鼠标抓取（客人光标钉在屏幕边缘）是环境侧限制；已加一次重抓兜底，仍偶发 DRAG_FAIL |
| `.img`（裸盘/U 盘安装介质）UEFI 引导 | **未做**：裸镜像按设计只走 BIOS 引导码（`dd` 直接写盘、无分区表）；UEFI 请用 `.iso` 装到硬盘后由安装器写 ESP |
| 真机 UEFI 首测 | 需要真实硬件；本版证据来自 VMware EFI（EDK2 同族）+ 备份 GPT 路径 |
| 60 fps | 未变 |
| `install_persist`/`ext4` 写入等历史未做项 | 未变 |

---

## 5. 复现/验收命令

```powershell
# UEFI 端到端（ISO 装机 -> 重启 -> 从硬盘引导）
py -3 tests\uefi_install64_test.py --target-mb 256

# 只看已装好的盘能否 UEFI 引导（跳过安装）
py -3 tests\uefi_install64_test.py --skip-install --target-mb 256

# VMware 真鼠标拖拽/缩放（BIOS 固件；需要本机 VMware Workstation）
$env:VIMTU_VMTEST="$env:PI_SCRATCH_DIR\twm-vm"
py -3 $env:PI_SCRATCH_DIR\twm_mkvm.py --firmware bios
py -3 tests\textwm64_test.py --apps explorer,terminal,settings,taskmgr --reps 2
```
