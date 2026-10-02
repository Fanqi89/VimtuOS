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
