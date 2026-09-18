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
// 镜像布局（与 32 位构建的 vimtu64.img 布局保持一致，只是内核区变大）：
//   LBA 0            : boot.bin      （512B MBR，读 LBA 1..8 到 0x9000）
//   LBA 1..8         : loader64.bin  （实模式准备 + 32 位 ATA 读内核 + 进长模式）
//   LBA 9..8008      : kernel64.bin  （最多 4,000 KB）
//   LBA 8009..8072   : 设置持久化保留区（64 扇区 = 32KB，A/B 双份）—— **仅降级用**，见下面 ★
//   = 8073 扇区 / 4,133,376 字节
//
// 内存布局：
//   0x1000    BootInfo（loader 写，内核读；magic 'AUR1'）
//   0x2000    E820 内存条目（最多 64 * 20B）
//   0x7000    VBE 控制器信息
//   0x7400    可用显示模式表（<=16 * 8B）
//   0x7600    EDID 缓冲（128B）
//   0x7800    RSDP 传递槽（8B u64：RSDP 物理地址；0 = 引导层未提供，内核自行扫 legacy 窗口）
//   0x9000    loader 自身
//   0x40000   PML4（64 位页表根，见 boot/loader64.asm 的 lm64_build_paging）
//   0x41000   PDPT（4 项，各覆盖 1GB）
//   0x42000..0x45000  4 张 PD（每张 512 项 * 2MB = 1GB 映射）
//   0x100000  内核加载地址（KERNEL_BASE，与 linker64.ld 的 . = 0x100000 一致）
#pragma once

#include <stdint.h>

// ---- 磁盘（LBA 单位 = 512 字节扇区）----
static const uint64_t ML64_SECTOR_BYTES    = 512;
static const uint32_t ML64_KERNEL_LBA      = 9;        // 内核起始 LBA（boot.bin + 8 扇区 loader）
static const uint32_t ML64_KERNEL_SECTORS  = 8000;     // 内核区扇区数（4,096,000 B）
static const uint32_t ML64_KERNEL_MAX_BYTES = ML64_KERNEL_SECTORS * (uint32_t)ML64_SECTOR_BYTES;
// ★★ ML64_STORE_LBA / ML64_STORE_SECTORS（LBA 8009..8072，64 扇区 = 32KB）**与安装程序创建的
//    数据分区完全重叠**：kernel/part64.cpp 的 build_standard_table 把主分区（type 0x07）起点写成
//    PART_MAIN_LBA = 8009（= PART_BOOT_LBA 9 + PART_BOOT_SECS 8000），恰好就是 ML64_STORE_LBA；
//    kernel/vfs64.cpp 的 VimtuFS2 超级块、块位图、inode 表、数据块全都落在这个位置。
//    所以这块裸盘保留区**只能作为"没有可用卷"时的降级槽**（kernel/store64.cpp 会打醒目 WARN），
//    正常运行必须用文件系统里的槽文件 /store.a、/store.b（各 16KB = 一个槽，VFS 载体）。
//    ——写成裸盘读写会把 VimtuFS2 的超级块/inode 覆盖掉（反之亦然），这曾经是一条真缺陷。
static const uint32_t ML64_STORE_LBA       = ML64_KERNEL_LBA + ML64_KERNEL_SECTORS;   // 8009（= 数据分区起点）
static const uint32_t ML64_STORE_SECTORS   = 64;       // 32KB 降级保留区（A/B 双槽，各 32 扇区）
static const uint32_t ML64_IMAGE_SECTORS   = ML64_STORE_LBA + ML64_STORE_SECTORS;     // 8073
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
static const uint64_t ML64_KERNEL_VA_BASE  = 0xFFFFFFFF80100000ULL;
static const uint32_t ML64_PML4_PHYS       = 0x40000;  // 页表区起点（loader 构建）
static const uint32_t ML64_PDPT_PHYS       = 0x41000;
static const uint32_t ML64_PD0_PHYS        = 0x42000;
static const uint32_t ML64_PDPT_HI_PHYS    = 0x46000;  // 高半区 PDPT（本次新增）
static const uint32_t ML64_PD_HI_PHYS      = 0x47000;  // 高半区 PD（映射内核镜像本身）
static const uint64_t ML64_IDENTITY_BYTES  = 0x100000000ULL;  // 长模式前 4GB 恒等映射

// ---- 64 位页表布局（内核侧内核堆用它自建页表，见 kernel/paging64.cpp）----
static const uint32_t PAGE_SIZE_64  = 4096;
static const uint32_t PAGE_SIZE_2M  = 0x200000;
