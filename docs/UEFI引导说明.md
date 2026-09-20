# UEFI 引导说明（自研 BOOTX64.EFI，不用 gnu-efi）

> 状态（本次更新）：**引导程序本体、ISO 三合一结构、自动引导全部完成并实测通过**。
> 本次新增：① "ISO 当 U 盘"形态 × UEFI/BIOS **双固件四档**实测（`tests/usb_boot_both_fw_test.py`）；
> ② **安装时在目标盘写混合 MBR + 盘尾 GPT + FAT16 ESP**（`kernel/fat64.{h,cpp}` + `kernel/part64.cpp`），
> 装好的盘在 UEFI 与 BIOS 下都能启动（`tests/esp_install_test.py`）。详见 §7/§8。

## 1. 设计：两段式，全 64 位

```
UEFI 固件（x86_64 长模式，EDK2 系：VMware EFI / OVMF）
  └─ 读 ESP（FAT16）里的 \EFI\BOOT\BOOTX64.EFI
      └─ 【第一段】BOOTX64.EFI —— 极小的 PE32+ 桩（自研 PE 头，lld-link 生成）
         只做：LoadedImage → SimpleFileSystem → OpenVolume → 读 UEFI64.BIN → 跳
      └─ 【第二段】UEFI64.BIN —— 平铺长模式二进制（链接在 0x800000，不经任何 PE 校验）
         做：读 KERNEL64.BIN→0x100000 / SYSTEM.IMG→0x04000000、GOP 取帧缓冲、
             写 BootInfo(0x1000)/E820(0x2000)/介质描述符(0x0F00, kind=2)、
             ExitBootServices、建 0..512GB 恒等映射页表、载 GDT、跳 0x100000
      └─ 内核 entry64 → kmain64（安装向导）
```

整条路径**没有任何 16 位或 32 位代码**（UEFI 固件本身就在长模式）。

### 为什么要分两段
实测 EDK2 的 PE 加载器会拒绝我们那个 ~8KB 的 PE（UEFI Shell 报 `is not an image`），
而把引导逻辑做成平铺二进制（不经 PE 校验）就完全绕开了这个限制。

## 2. 构建

```bash
bash build_uefi.sh          # 单独构建（也可直接跑 build64.sh，它已包含这一步）
```
产物：
| 文件 | 说明 |
|---|---|
| `build64/BOOTX64.EFI` | 极小 PE 桩（约 2KB，3 个段） |
| `build64/UEFI64.BIN` | 平铺长模式引导器（约 28KB，加载地址 0x800000） |
| `build64/esp.img` | FAT16 的 ESP（内含 BOOTX64.EFI + KERNEL64.BIN + SYSTEM.IMG + UEFI64.BIN） |

`tools/make_esp.py` 自己实现 FAT16（这台机器没有 mkfs.fat/mtools）；
`tools/make_flat.py` 把 PE 摊平成裸二进制；`tools/make_iso64.py` 负责双模式 ISO。

## 3. ISO 里的 UEFI 结构
* **El Torito 第二引导项**：platform `0xEF`，引导镜像 = `esp.img`（FAT16）
  → UEFI 光盘引导用这个（`xorriso -eltorito-alt-boot -append_partition 2 0xef`）
* **GPT**：`tools/make_iso64.py` 自己写（xorriso 1.5.8 的 `-isohybrid-gpt-basdat` 实测没有生成 GPT），
  分区1 = ISO9660，分区2 = **ESP（类型 C12A7328-…）**
* **MBR**：0x17（ISO 区，活动）+ **0xEF（ESP）** 两项 —— 一部分固件在"只有 MBR"的 U 盘上
  靠 MBR 里的 0xEF 分区找 ESP

## 4. 已验证（可复现）

| 项目 | 结论 | 证据 |
|---|---|---|
| 固件挂载我们的 FAT16 ESP | ✅ | OVMF UEFI Shell：`FS0:  CDROM(0x1)`，`ls` 列出 EFI/、KERNEL64.BIN、SYSTEM.IMG、UEFI64.BIN |
| 子目录路径解析 | ✅（修过） | 之前 `EFI\BOOT\` 里的文件一律 "Not Found"：**FAT 子目录缺 `.` / `..` 项**；补上后路径解析正常（见 make_esp.py） |
| 固件加载 BOOTX64.EFI | ✅（修过） | 之前 4 段（`.text/.rdata/.data/.reloc`）的 PE 被拒；**`-merge:.rdata=.data` 合成 3 段后可加载**（`load` 返回 "is not a driver" = LoadImage 成功） |
| ESP 内容完整性 | ✅ | 独立 FAT16 读取器（按规范实现）取出的文件与构建产物**逐字节一致** |
| BIOS 光盘 / U 盘路径 | ✅ | `tests/iso64_install_test.py`、`tests/iso64_usb_test.py` 全 PASS（UEFI 改动无回归） |

排查过程中排除过的因素：PE 头（Machine/Subsystem/对齐/入口/SizeOfImage）、重定位内容、
编译选项组合、镜像体积（1.5KB–8KB）、静态缓冲大小、FAT 读写正确性。

## 5. 自动引导：已打通（三个真根因 + 两个环境差异）

早期现象：OVMF 与 VMware EFI 挂上我们的 ISO 后都**不执行** ESP 里的 BOOTX64.EFI
（OVMF：`BdsDxe: failed to load Boot0001 "UEFI QEMU DVD-ROM" ...: Not Found`），
而同一个固件里手动 `load FS0:\EFI\BOOT\BOOTX64.EFI` 又能成功 —— 于是被误判成
"固件如何发现引导目标"的问题。**实际是三个各自独立的真 bug**，全部用工具链与日志定位：

| # | 根因 | 怎么发现的 | 修法 |
|---|---|---|---|
| ① | **PE 首选基址是 0x140000000（5GB）**：lld-link 默认给 x86_64 PE 的 MSVC 风格基址。固件先按它 `ConvertPages` 分配，512MB 虚拟机里必然失败，之后**直接放弃该引导项**转去内置 shell | OVMF 调试日志 `ConvertPages: failed to find range 140000000 - 140003FFF`，范围恰好 = 镜像 SizeOfImage；新写的 `tools/pe_info.py` 打出 `ImageBase = 0x140000000` | `build_uefi.sh` 加 **`-base:0x0`**（EDK2 自己的镜像就是基址 0，可任意放置后用 .reloc 重定位） |
| ② | **ESP 名为 FAT16、簇数却落在 FAT12 区间**：6MB 卷用 2KB 簇只有 3057 簇（< 4085）。EDK2 的 FAT 驱动**按簇数**决定用 12 位还是 16 位读 FAT 表 → 把我们的 16 位 FAT 当 12 位解析 → 簇链变垃圾 → 读文件返回 `EFI_VOLUME_CORRUPTED`。症状极具迷惑性：目录能列、文件能打开、**单簇文件能读**（2048B 的 BOOTX64.EFI 恰好 1 簇），**多簇文件必挂**（28KB 的 UEFI64.BIN 要查 FAT 链）——这正是"手动 load 成功、自动引导失败"的假象来源 | 桩加串口打点读到 `S1234E0A`（0x0A = VOLUME_CORRUPTED）；新写的 `tools/fat_check.py` 打出"按簇数推导的类型 = FAT12，BPB 自称 FAT16" | `tools/make_esp.py` 改 **`SPC = 1`**（512B 簇 → 12157 簇，真 FAT16；ESP 体积不变），并加**硬断言**禁止这类错静默复活 |
| ③ | **进内核时 CS 仍是固件的 0x38**：`efi_enter_kernel` 用 `jmp rcx`（近跳）只改 RIP 不改 CS。内核照跑（同环、长模式），但 **PIT 中断一来**，CPU 把 CS=0x38 压进中断帧，我们的桩 `iretq` 要恢复它时发现 GDT 里没有 index 7 → #GP → PANIC | 内核 panic 打印加上 RIP/err 后读到 `exception 13 err=0x38 rip=...iretq`；反汇编确认故障指令就是 `iretq` | `boot/efi/jump64.asm` 改**远跳**（64 位模式没有 ptr16:64 远跳指令，用 `push CS/RIP + retfq`）。BIOS 路径一直是对的（loader64.asm 本来就是 `jmp 0x08:...`） |

过程中还排掉两个**环境差异**（都很坑，记下来省下次的时间）：

* **文件名必须 UTF-16（CHAR16）**：`EFI_FILE_PROTOCOL::Open` 的 FileName 是 CHAR16*。
  桩与引导器早期都直接传 ASCII `char[]`，固件按 UTF-16 解释成乱码 → EFI_NOT_FOUND →
  桩 `return 2`（固件日志表现为 `Image Return Status = Warning Delete Failure`，
  极易误读成"固件没加载我们的镜像"，其实**已经加载并执行了**）。
* **VMware EFI 下不能切 CR3**：`efi_enter_kernel` 里那条 `mov cr3` 在 VMware EFI（本机无
  VT-x，走二进制翻译+影子页表）下会**立刻复位**；同一份代码在 OVMF 下完全正常。
  已确认不是"表坏了"（换表前 `U:pt check ok`）也不是"值不对"（`U:cr3=806000 low12=0`）。
  由于 UEFI 固件本来就为整机 RAM 与 GOP 帧缓冲建了恒等映射，而内核全程按物理地址访问，
  **不切表没有任何副作用** —— 所以现在 UEFI 路径故意保留固件页表（代码里有详细注释）。

调试这类"静默复位"的经验（都写进代码注释了）：
1. 串口打点要**等字节真的发完**（LSR **bit6 TEMT**，不是 bit5 THRE）—— 否则复位时最后一个
   标记还在移位寄存器里，你会得到假的定位。
2. `out` 的端口号在 **DX**，而 DX 就是 **RDX 低 16 位**；进内核时 RDX 正是 CR3，打点会把它改坏。
3. 内核 panic 必须打印 **RIP/err/RFLAGS/CR2**：只知道"异常号 13"根本定位不了。

## 6. 怎么测

```bash
# VMware（UEFI）：建 VM（firmware=efi）→ 挂 ISO → 用 COM2 按键通道跑完安装
python tests/uefi64_install_test.py

# OVMF（快速诊断）：直接引导 ISO 看串口；或在 UEFI Shell 里手动执行
#   load FS0:\EFI\BOOT\BOOTX64.EFI / EFI\BOOT\BOOTX64.EFI
```
`tests/uefi64_install_test.py` 的验收项（串口 `U:` 标记、无 BIOS 桩标记、装入目标盘逐字节）
在自动引导打通后全绿（22 项）。

## 7. U 盘形态 × 双固件（本次实测）

`tests/usb_boot_both_fw_test.py` 把同一个 `vimtu64-64.iso` 当**磁盘/U 盘**引导，四档全绿：

| # | 形态 | 固件 | 关键打点 |
|---|---|---|---|
| A | ISO 挂 `usb-storage`（UHCI + `bootindex=1`） | UEFI（OVMF） | `S12345J` → `U:====` → `U:loaded KERNEL64.BIN/SYSTEM.IMG` → `[LM64] ENTERED LONG MODE` → `[SETUP] 磁盘枚举完成` |
| B | ISO 当 IDE 盘 | UEFI（OVMF） | 同 A（固件不支持 USB 启动时的等价形态） |
| C | ISO 挂 `usb-storage` | BIOS（SeaBIOS） | `I:boot dl=` → `I:hdd mode (USB/disk)` → `I:jump loader64` → `L:media=ram` → 向导第一屏 |
| D | ISO 当 IDE 盘 | BIOS（SeaBIOS） | 同 C |

结论：**"U 盘形态 + UEFI"原生就通**（ISO 里的 El Torito 0xEF 项 + GPT + ESP 三者任一都能让
OVMF 找到 `EFI/BOOT/BOOTX64.EFI`），BIOS 侧走 hybrid MBR → cdiso 桩 → INT 13h 搬内核/载荷。
脚本末尾还有"安装介质 sha256 未变"的断言（真 U 盘/光驱是只读的）。

## 8. 安装时在目标盘写 GPT + ESP（装好的盘也是 UEFI 可启动）

这是本次补齐的最后一环 —— 以前**只有 ISO 有 ESP，装到硬盘上的系统没有**，于是 UEFI 机器
装完重启后固件在盘上找不到任何 FAT 卷。现在安装收尾（`kernel/part64.cpp` + `kernel/fat64.{h,cpp}`）：

1. 在盘尾格式化 **5MB FAT16 ESP**（与 `tools/make_esp.py` 完全相同的卷参数：512B 扇区、
   `SPC=1`、1 保留扇区、2 份 FAT、512 项根目录、簇数硬断言 `[4085, 65525)`）；
2. 写 `EFI/BOOT/BOOTX64.EFI`（内嵌 PE 桩字节）+ `UEFI64.BIN`（内嵌平铺引导器）+
   `KERNEL64.BIN`（**直接读目标盘 LBA 9..8008 的 4MB 系统内核区**）；
3. 写**混合 MBR**（`0xEF` 引导区活动 9+8000 / `0x07` 主分区 / `0xEF` ESP）与**盘尾备份 GPT**
   （项1 主分区 / 项2 ESP，`C12A7328-…`；头与项数组 CRC 都按规范算）。

为什么只写**备份** GPT：主 GPT 头的规范位置是 **LBA 1**，而那里是 `loader64.bin`
（`boot/boot.asm` 从 LBA 1 读 8 扇区）——两者不可兼得。UEFI 规范要求主头无效时使用备份头，
EDK2 实测如此；`tests/esp_install_test.py` 还断言 UEFI 从这块盘启动**之后** LBA 1..8 的
loader 字节没被固件改写（没有"GPT 修复式回写"把引导链踩坏）。

`UEFI64.BIN` 侧的小改动（`boot/efi/uefi64.c`）：`SYSTEM.IMG`（安装载荷）改成**可选** ——
装好的盘上没有它，此时跳过并写介质描述符 `kind=0`；`KERNEL64.BIN` 仍是**必须**（它就是这次要跑的内核）。
安装介质路径完全不变（ISO 的 ESP 里三个文件齐全）。

实测（`tests/esp_install_test.py`，64MB 目标盘挂 AHCI）：

```
[INSTALL] esp: lba=120799 sectors=10240 fat_ok=1
[INSTALL] esp files: BOOTX64.EFI=2560B UEFI64.BIN=36864B KERNEL64.BIN=4096000B
[INSTALL] gpt written (main + esp), pmbr ok main=8009+112790 esp=120799+10240 backup_hdr_lba=131071
UEFI(OVMF)：U:==== → U:loaded KERNEL64.BIN → U:missing SYSTEM.IMG (optional, skipped)
            → [OS] booted from installed disk → [GUI64] ready
BIOS      ：[LM] disk boot via INT 13h dl=0x80 → [OS] booted from installed disk → [GUI64] ready
```

限制（如实）：① 目标盘 < ~17MB（8009 + 8MB 主分区 + 5MB ESP + 33 扇区 GPT 备份）时不建 ESP，
只写老 MBR 布局并在串口打 `[INSTALL] esp skipped (disk too small)`；
② 内嵌 FAT16 写入器只支持"根目录 + 2 层子目录 + 连续簇链 + 8.3 短名 + 只新建"；
③ 只有 `BOOTX64.EFI`（**32 位 UEFI 固件不支持**）；④ Secure Boot 必须关闭（无签名）。
