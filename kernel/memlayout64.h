// memlayout64.h - Vimtu64 磁盘/内存布局常量（64 位引导链的唯一定义点）
//
// 为什么需要这个文件：
//   64 位引导链的 LBA 布局有三处必须严格一致，任何一处改错都会表现为
//   "内核被读到一半就跳过去执行"这种极难排查的故障：
//     1) build64.sh        —— dd 写镜像时的 seek 偏移
//     2) boot/loader64.asm —— 保护模式 ATA 读盘时的起始 LBA 与扇区数
//     3) kernel/store64.*  —— 设置持久化"裸盘降级槽"的 LBA（正常运行不用它，见下面 ★）
//   所以这里集中定义，改动只改这一处（并通过 -DVIMTU_KERNEL_SECTORS= 传给汇编与 C++）。
//
// ★ v0.9.31：内核容量 4 MB -> **15 MB**（用户拍板的"内核空间扩容"，是混合驱动架构的地基）。
//   扩容不是"把一个数字改大"。它同时挪动了三条边界，每条算错都会是灾难：
//     ① 磁盘：引导分区 = 内核容量(15 MiB) + 原始区(演示/字体/图标 blob)，
//             所以主分区起点从 LBA 8009 一路后移到 **32584**（见 kernel/part64.h 的 PART_MAIN_LBA）：
//             v0.9.31 内核 4MB->15MB（+56 扇区是 v0.9.32 的 loader 窗口）；
//     ② 内存：内核映像上界（__bss_end，含 fb 的 33MB 后备缓冲）必须留在**载荷暂存区**(64MB) 之下；
// ★ v0.9.32：引导链改成**显式两阶段**（Stage 1 = MBR，Stage 2 = loader64）：
//   Stage 1 只做一件事 —— 用 INT 13h 把 Stage 2 读进内存（分两块，避开 64 KB 物理边界），
//   所以 Stage 2 的容量不再受"一个 4 KB 窗口"限制。`ML64_LOADER_SECTORS` 就是它的容量，
//   也是 `ML64_KERNEL_LBA` 的唯一来源：内核起点 = 1 + loader 容量。
//   ★ 注意：**UEFI 那条链没有这个限制**（BOOTX64.EFI 把 UEFI64.BIN 读到 0x06000000，
//     上限 256 KB，由 boot/efi/stub.c 的 LOADER_MAX 管）。4 KB 是纯 BIOS/INT 13h 的窗口。
//   磁盘布局（LBA = 512 字节扇区）：
//     LBA 0            : boot.bin      MBR = Stage 1（读 LBA 1..64 → 0x9000，再跳过去）
//     LBA 1..64        : loader64.bin  Stage 2（**容量 32 KB**，v0.9.32 前只有 8 扇区/4 KB）
//     LBA 65..30784    : 内核容量 15 MiB（30,720 扇区 = 15,728,640 B）
//     LBA 30785..32583 : 原始区（1,799 扇区）
//     LBA 32584..32647 : store 裸盘降级槽（64 扇区）
//     LBA 32648..      : 主分区（VimtuFS2 卷）起点 = ML64_IMAGE_SECTORS
//
// 内存布局（★ 地址是**物理**地址，内核通过 0..4GB 恒等映射直接访问）：
//   0x1000    BootInfo（loader 写，内核读；magic 'AUR1'）
//   0x2000    E820 内存条目（最多 64 * 20B）
//   0x7000    VBE 控制器信息
//   0x7400    可用显示模式表（<=16 * 8B）
//   0x7600    EDID 缓冲（256B：块 0 = 0x7600..0x767F，块 1 = 0x7680..0x76FF）
//   0x7800    RSDP 传递槽（8B u64：RSDP 物理地址；0 = 引导层未提供，内核自行扫 legacy 窗口）
//   0x9000    loader 自身
//   0x40000   PML4（64 位页表根，见 boot/loader64.asm 的 lm64_build_paging）
//   0x41000   PDPT（4 项，各覆盖 1GB）
//   0x42000..0x45000  4 张 PD（每张 512 项 * 2MB = 1GB 映射）
//   0x100000  内核加载地址（ML64_KERNEL_BASE，与 linker64.ld 的 . = 0x100000 一致）
//   0x100000 + 15MB = 0x1000000  内核**容量**上界（内核二进制本身必须 <= 15MB）
//   ~0x3200000..      __bss_end：内核映像真实上界（含 fb 的 33MB 后备缓冲）——
//                       必须 < 0x04000000（载荷暂存区），构建期用 nm 断言把它钉死
//   0x04000000        载荷暂存区（安装介质内核把 system.img 整块读到这里；系统内核不用）
//                     预留 32MB：[0x04000000, 0x06000000)
//   0x06000000        UEFI 长模式引导器（UEFI64.BIN，<=256KB）的装载地址
//                     （必须在载荷窗口之上、内核堆之下；改这里要同步 build_uefi.sh 的 -base）
//   0x07000000..0x0A000000  内核堆 48MB（**下限**；运行期取 max(下限, ALIGN(__bss_end))——
//                       内核长得再大也绝不与自身映像重叠，见 kernel/mem64.cpp 的 ★★ 断言）
//   0x0A000000..      物理页池（到 E820 认定的内存上界）
#pragma once

#include <stdint.h>

// ---- 磁盘（LBA 单位 = 512 字节扇区）----
static const uint64_t ML64_SECTOR_BYTES    = 512;
// ---- 引导链容量（★ v0.9.32：Stage 2 从 8 扇区/4 KB 放大到 64 扇区/32 KB）----
//   ML64_LOADER_SECTORS 是 **BIOS Stage 2（loader64.bin）** 的容量，也是内核起点的唯一来源。
//   改这里要同步：boot/boot.asm 的 LOADER_SECTORS（Stage 1 按它读盘）与 build64.sh 的 LOADER_SECTORS。
static const uint32_t ML64_LOADER_SECTORS  = 64;       // 32 KB
static const uint32_t ML64_LOADER_MAX_BYTES = ML64_LOADER_SECTORS * (uint32_t)ML64_SECTOR_BYTES;
// 内核起始 LBA = 1（MBR 占 LBA 0）+ loader 容量。v0.9.32 前是 9（= 1 + 8）。
static const uint32_t ML64_KERNEL_LBA      = 1 + ML64_LOADER_SECTORS;   // 65
// ★ 内核容量：15 MiB = 30,720 扇区（= 15,728,640 B）。这就是内核体积的**硬闸门**：
//   build64.sh 里两条断言的分子、tools/kernel_size_report.py 的上限、tests/a42a64_test.py 的判据
//   全部由这一个数派生，别在别处再写第二份。
static const uint32_t ML64_KERNEL_SECTORS  = 30720;
static const uint32_t ML64_KERNEL_MAX_BYTES = ML64_KERNEL_SECTORS * (uint32_t)ML64_SECTOR_BYTES;

// ---- 原始区（演示程序 / 中文字体 / 图标包 blob）：紧接在内核容量之后 ----
//   为什么在容量之外而不是"内核区尾部"：老布局把原始区塞在内核区里（LBA 6208 起），
//   于是内核预算 = (原始区起点 - 9) * 512，原始区越大内核越小 —— 15MB 是个整数承诺，
//   不能拿它去养 blob。现在原始区单独占 [30785, 32584)，内核拿满 15MB。
static const uint32_t ML64_RAW_LBA         = ML64_KERNEL_LBA + ML64_KERNEL_SECTORS;   // 30785
// 原始区预留扇区数（构建期断言：真实 demo64_raw.bin 不得超过它）。
//   实测内容 = 920,911 B（演示 blob 193,344 + 中文面字体 733,584 + 图标包 + 对齐）
//   -> ceil(920,911 / 512) = 1,799 扇区；这里留 0 扇区余量并把它变成**构建期断言**
//   （要加 blob 就同时改这个数 + part64.h 的 PART_BOOT_SECS，两处一起动）。
static const uint32_t ML64_RAW_SECTORS     = 1799;
static const uint32_t ML64_RAW_MAX_BYTES   = ML64_RAW_SECTORS * (uint32_t)ML64_SECTOR_BYTES;

// ★★ ML64_STORE_LBA / ML64_STORE_SECTORS（LBA 32584..32647，64 扇区 = 32KB）**与安装程序创建的
//    数据分区完全重叠**：kernel/part64.cpp 的 build_standard_table 把主分区（type 0x07）起点写成
//    PART_MAIN_LBA = 32584（= PART_BOOT_LBA 9 + PART_BOOT_SECS 32519），恰好就是 ML64_STORE_LBA；
//    kernel/vfs64.cpp 的 VimtuFS2 超级块、块位图、inode 表、数据块全都落在这个位置。
//    所以这块裸盘保留区**只能作为"没有可用卷"时的降级槽**（kernel/store64.cpp 会打醒目 WARN），
//    正常运行必须用文件系统里的槽文件 /store.a、/store.b（各 16KB = 一个槽，VFS 载体）。
//    ——写成裸盘读写会把 VimtuFS2 的超级块/inode 覆盖掉（反之亦然），这曾经是一条真缺陷。
static const uint32_t ML64_STORE_LBA       = ML64_RAW_LBA + ML64_RAW_SECTORS;          // 32584（= 数据分区起点）
static const uint32_t ML64_STORE_SECTORS   = 64;       // 32KB 降级保留区（A/B 双槽，各 32 扇区）
static const uint32_t ML64_IMAGE_SECTORS   = ML64_STORE_LBA + ML64_STORE_SECTORS;     // 32648
static const uint32_t ML64_IMAGE_BYTES     = ML64_IMAGE_SECTORS * (uint32_t)ML64_SECTOR_BYTES;

// ---- 内存 ----
static const uint32_t ML64_KERNEL_BASE     = 0x100000; // 内核被**平铺加载**的物理地址
// ★ 引导层 -> 内核的固定传递槽（照 0x7000 VBE / 0x7400 模式表 / 0x7600 EDID 的风格）：
//   物理 0x7800 的 8 字节 u64 = RSDP 物理地址。UEFI（boot/efi/uefi64.c）从 EFI 系统表的
//   配置表里按 ACPI 2.0/1.0 GUID 找到后写入；BIOS（boot/loader64.asm）显式写 0，
//   内核（kernel/acpi64.cpp）读到 0 就自己扫 EBDA / 0xE0000..0xFFFFF 的 legacy 窗口。
static const uint32_t ML64_RSDP_PTR_PHYS   = 0x7800;
// ★ 内核运行的虚拟基址（链接基址，见 kernel/linker64.ld）。
//   "镜像有多大"这类计算必须用本常量（__bss_end - ML64_KERNEL_VA_BASE），
//   用 ML64_KERNEL_BASE（物理）会得到一个天文数字 —— 搬高半区时踩过。
//   ★ 直映关系：VA = 0xFFFFFFFF80000000 + PA，所以内核（PA 0x100000）链在
//     0xFFFFFFFF80100000。两者必须满足 VA == 直映起点 + 装载物理地址。
//   ★ 高半区直映用的是 2MB 大页、覆盖物理 0..1GB（boot/loader64.asm 的 PDPT_hi[510]），
//     所以 15MB 内核完全落在已有的映射里，不需要新增页表项。
static const uint64_t ML64_KERNEL_VA_BASE  = 0xFFFFFFFF80100000ULL;
static const uint32_t ML64_PML4_PHYS       = 0x40000;  // 页表区起点（loader 构建）
static const uint32_t ML64_PDPT_PHYS       = 0x41000;
static const uint32_t ML64_PD0_PHYS        = 0x42000;
static const uint32_t ML64_PDPT_HI_PHYS    = 0x46000;  // 高半区 PDPT
static const uint32_t ML64_PD_HI_PHYS      = 0x47000;  // 高半区 PD（映射内核镜像本身）
static const uint64_t ML64_IDENTITY_BYTES  = 0x100000000ULL;  // 长模式前 4GB 恒等映射

// ---- 载荷暂存 / 引导器 / 内核堆 / 页池的四段边界（★ v0.9.31 重新划定）----
//   老布局：载荷 64MB 起 4.13MB、内核堆 80..128MB、页池 128MB 起。
//   15MB 内核 + 更大的载荷（引导区 15MB 让 system.img 从 16MB 长到 ~29MB）之后，
//   "载荷 64..93MB" 会顶穿 "堆 80MB"，所以整段上移。四段的关系必须永远成立：
//       __bss_end < PAYLOAD < PAYLOAD_MAX <= LOADER_STUB < HEAP < POOL
static const uint32_t ML64_PAYLOAD_PHYS        = 0x04000000;   // 64MB 载荷暂存区起点
static const uint32_t ML64_PAYLOAD_MAX_BYTES   = 0x02000000;   // 预留 32MB（实测载荷 ~28MiB）
static const uint32_t ML64_LOADER_STUB_PHYS    = 0x06000000;   // 96MB（UEFI64.BIN，<=256KB）
static const uint32_t ML64_HEAP_PHYS_FLOOR     = 0x07000000;   // 112MB 内核堆下限
static const uint32_t ML64_HEAP_BYTES          = 0x03000000;   // 48MB
static const uint32_t ML64_POOL_PHYS_FLOOR     = 0x0A000000;   // 160MB 物理页池下限

// ---- 64 位页表布局（内核侧内核堆用它自建页表，见 kernel/paging64.cpp）----
static const uint32_t PAGE_SIZE_64  = 4096;
static const uint32_t PAGE_SIZE_2M  = 0x200000;
