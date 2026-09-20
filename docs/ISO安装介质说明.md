# 64 位安装介质：ISO（光盘 / 虚拟光驱可引导）

M2 原来的安装介质是一张**裸盘镜像**（`vimtu64-64.img`，适合写 U 盘或当 VMware 的硬盘）。
现在补上了**光盘形态**：`vimtu64-64.iso` —— 可直接刻盘、也可以挂进 QEMU/VMware 的虚拟光驱。

## 1. 一条命令构建

```bash
bash build64.sh          # 顺带生成 vimtu64-64.iso（约 56.2MB，附 48MB FAT32 ESP）
```

产物：

| 文件 | 用途 |
|---|---|
| `vimtu64-64.iso` | **光盘安装介质**（BIOS El Torito 引导），本文件的主角 |
| `vimtu64-64.img` | 裸盘安装介质（U 盘/硬盘形态，见第 5 节） |
| `build64/cdiso.bin` | ISO 的引导桩（也作为 ISO 里的一个文件存在） |

## 2. ISO 里有什么

```
CDISO.BIN      引导桩（boot/cdiso.asm）：BIOS 加载到 0x7C00 执行
LOADER64.BIN   loader64（引导桩把它读进 0x9000）
KERNEL64.BIN   安装程序内核（补零到 4MB，loader64 用 ATAPI 读进 0x100000）
SYSTEM.IMG     系统镜像（= build64/system.img，8073 扇区；装到目标盘上的东西）
```

ISO9660 里文件落在哪个 LBA 只有 xorriso 生成之后才知道，所以 `tools/make_iso64.py`
生成后解析 ISO9660 目录，把 LBA **回填**进引导桩里的魔数（`VMLD/VMLB/VMKN/VMSY/VMSS`）。
（ISO9660 没有校验和，直接改 ISO 字节即可 —— 与 32 位构建里 `PLBA` 的做法一致。）

## 3. 引导与读盘流程（为什么这么设计）

```
BIOS(光盘引导) → El Torito 引导镜像 cdiso.bin @0x7C00
   ├─ INT 13h 把 LOADER64.BIN 读进 0x9000（低内存，2 个光盘扇区）
   ├─ 在物理地址 0x0F00 写"介质描述符"（magic VMMD + 光盘驱动器号 + 各文件 LBA）
   └─ jmp 0x9000
loader64
   ├─ 常规：实模式做 E820/VBE/EDID → 进长模式
   ├─ 读 0x0F00 描述符：kind=1（光盘）
   ├─ 用 **ATAPI(PACKET)** 把 KERNEL64.BIN 读进 0x100000（2000 个光盘扇区 = 4MB）
   ├─ 再把 SYSTEM.IMG 读进 0x04000000（64MB 处）
   └─ 描述符改成 kind=2（RAM 源）
安装程序（图形向导）
   ├─ 从描述符取载荷：直接在内存里，写目标盘时就是"内存 → 硬盘"
   └─ 新建/删除/格式化/安装/100%/自动重启（与硬盘介质完全同一套界面与代码）
```

**关键取舍**：loader 进保护模式之后**不碰任何 BIOS 中断**，光盘读盘一律走 ATAPI
（设备级 PACKET 命令）。原因：`boot/cdprobe.asm` 实测过 **VMware 的 BIOS 在"进/出保护模式
往返"一次之后就不再响应 INT 13h/INT 10h**，靠 BIOS 读盘的方案在 VMware 上会死。
ATAPI 只碰端口，与 BIOS 无关，QEMU/VMware/真机一致。

另外：**载荷由 loader 读进内存**，安装程序不再直接读光盘。这样绕开了 QEMU 的一个
IDE 状态机毛病（见第 4 节），也让"装系统"这一步变成纯内存拷贝，更快更稳。

## 4. 本轮踩到的坑（都留了注释）

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | `I:read LOADER64.BIN failed`，AH=0x09 | 引导桩把魔数 `"VMLD"`（4 字节）当 LBA 用了 | 魔数与值分开标号（`vmld_lba` / `vmld_val`） |
| 2 | loader 报 `!`（ATAPI 读失败） | 描述符里的 `drive` 是 BIOS 的 DL（0xE0），不是 ATA 通道号 | loader 依次试 2/3/0/1 号驱动器 |
| 3 | ATAPI 读内核"成功"但内核里字符串全是 0 | `rep insw` 已推进目标指针，代码又 `add rdi, 2048`，镜像被摊成每 2KB 一个空洞 | 去掉重复推进 |
| 4 | 安装阶段 PACKET 命令被当场 ABRT（st=0x41 err=0x04） | 枚举时向光驱发过 0xEC（IDENTIFY DEVICE，光驱会 ABRT），QEMU 的 ATAPI 之后拒收 PACKET | ①识别到 ATAPI 签名改走 0xA1；②载荷改由 loader 读进内存，安装期不再用 ATAPI |
| 5 | 装完目标盘前 4000 扇区全是 0 | 载荷放在 32MB，而**内核 BSS 一直到 ~36.4MB**，内核启动清 BSS 把它抹了 | 载荷改放 64MB（高于 BSS 末尾） |
| 6 | 光盘引导下"新建"报读 LBA0 失败 | `ata64_read/write` 在**选盘之前**读状态寄存器；枚举把空的从盘留在选中态 → 读到 0x00 被当成"无设备" | 先选盘再等状态（read/write/ATAPI 三处都改） |

长度限制提醒：loader64 在介质上只占 **8 个扇区（4096 字节）**，`build64.sh` 会拦超限。

## 5. U 盘（下一步，尚未完成）

把 ISO 写进 U 盘（Rufus / balenaEtcher / `dd`）后，BIOS 会把它当**硬盘**引导，而不是光盘：
- El Torito 引导镜像不会被加载，走的是 U 盘的首扇区（MBR）；
- 我们的 loader 也不能用 ATA PIO 读 U 盘（U 盘在 USB 控制器后面，不在 0x1F0）。

要做的是：**自研 hybrid MBR**（引导桩加一个硬盘分支）+ **unreal mode 搬内存**
（BIOS 只能写 <1MB，需要一次保护模式往返把段限设成 4GB，之后用 32 位寻址直接搬到高内存）。
探针（`boot/cdprobe.asm`）已经给出结论：**QEMU 的 SeaBIOS 可以**（保护模式往返后 INT 13h
仍正常，还能写 1MB 以上），**VMware 的 BIOS 不行**（往返一次就挂）。所以 U 盘路径按"真机
+ QEMU"设计即可，VMware 用虚拟光驱跑 ISO。

当前若把 ISO 当硬盘引导，引导桩会明确打印：
`I:not a CD boot (USB/HDD path is the next step)`。

## 6. 验收

```powershell
python tests\iso64_install_test.py        # QEMU + 光盘引导（18 项断言）
$env:VIMTU_VM_ISO="1"; python tests\vmware_install_test.py   # VMware + 虚拟光驱（21 项）
python tests\install_flow_test.py         # 硬盘介质回归（22 项）
```

三套都 PASS；断言覆盖：引导桩启动 → loader 跳转 → 识别光盘介质 → ATAPI 读内核 →
介质描述符（kind=2）→ 新建分区 → 安装完成 8073 扇区 → 100% → 自动重启 →
目标盘 MBR/分区表/loader/内核**逐字节一致** → 单独启动装好的盘进 `[OS]` 路径。
