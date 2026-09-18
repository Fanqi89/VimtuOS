# UEFI 引导说明（自研 BOOTX64.EFI，不用 gnu-efi）

> 状态（截至本次）：**引导程序本体已完成并在固件里验证可加载**；ISO 的 UEFI 双模式结构
> （El Torito 0xEF 项 + GPT + FAT16 ESP）已完成；**固件自动引导这一步尚未打通**（手动执行
> 引导程序可以加载成功，见下文"已验证"与"卡点"）。

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
在自动引导打通后即可全绿。
