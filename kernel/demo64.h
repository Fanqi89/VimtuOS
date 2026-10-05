// demo64.h - ★ 本批：演示程序 blob 的"原始区"查找（内核侧，声明 + 表项结构）
//
// 为什么有这个东西（体积纪律，任务书的 option (b)）：
//   演示程序的字节**不再内嵌进内核二进制**（构建期有 64B 探针断言）。正常交付路径是
//   **系统卷里的同名文件**（tools/demo_pack_win.py 构建期写入，逐字节回读自检）；
//   而"空夹具盘"（没有卷、或卷里还没有这些文件的盘）需要一个兜底 —— 原始区：
//     * build64.sh 用 dd 把 build64/demo64_raw.bin 写进 system.img 的 LBA 7497 起
//       （内核区尾部；loader 也会把这段平铺加载进内存，但内核 .bss —— fb 的 33MB 后备缓冲 ——
//        盖住了这段物理地址，所以 kernel/demo64.cpp **按 LBA 用 ata64_read 现读**，读一次缓存）；
//     * 内核二进制里只有构建期生成的偏移表（路径/偏移/长度），字节数为 0 —— 这就是"极小兜底"。
//
// ★ 本头文件**不** include 构建期生成的头（demo64_blobtab.h）—— 它只有 kernel/demo64.cpp
//   需要，而 demo64.cpp 是链接内核之前才编译的（那时生成的头才存在）。其它内核源文件
//   只包含本文件，声明式引用下面这个查找函数即可。
#pragma once

#include <stdint.h>

// 原始区偏移表的表项（构建期生成的 g_demo64_blobtab64[] 用；只有路径/偏移/长度）。
struct Demo64BlobEntry64 {
    const char* path;      // 卷内路径（与 kernel 代码里的路径一致）
    uint32_t    off;       // 在原始区里的偏移（16 字节对齐）
    uint32_t    size;      // 字节数
};

// 按卷内路径找原始区里的字节。
//   找到：返回指向原始区的指针（原始区一定在物理 0x100000..0x4FF000 内）并写 *out_size；
//   找不到 / path 为空：返回 nullptr（*out_size = 0）。
const uint8_t* demo64_blob_find64(const char* path, uint32_t* out_size);

// ==================== ★ 本批修复：系统盘未知时的按盘探测入口 ====================
// 背景（图标"有时有、有时没有"）：原始区在**引导盘**上，而"引导盘驱动器号"没有任何
// 直接来源 —— kernel/demo64.cpp 的默认策略是"系统盘（C:/系统卷所在盘）优先，否则退回
// drive 0"。当盘上还没有可挂载的系统卷时（例如刚装完、卷还没建的盘），系统盘判定为
// 未知，退回的 drive 0 在 SATA/AHCI-only 机器上根本不存在 -> 原始区读不到。
// 这两个入口给调用方（kernel/icons64.cpp）一个**带内容校验的**按盘探测手段：
//   demo64_raw_try_drive64(d)：强制从驱动器号 d 读一次原始区（成功 = 1，缓冲已就绪，
//     之后 demo64_blob_find64() 就在这份缓冲里查表）；失败 = 0 且**不动**已有状态。
//   demo64_raw_abandon64()   ：探测全部失败后调用 —— 丢弃这次强制读进来的内容，
//     避免后续调用把"不是我们要的那块盘"的字节误当成 blob。
// 为什么由调用方校验：原始区本身没有 magic（第一个 blob 就是 /hello.elf 的字节），
// 但"要找的东西"（例如图标包 /etc/iconpack.bin）自带 magic+CRC —— 只有调用方知道判据。
int  demo64_raw_try_drive64(int drive);
void demo64_raw_abandon64(void);
