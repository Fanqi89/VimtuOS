// demo64.cpp - ★ 本批：演示程序 blob 的"原始区"读取（内核侧；只有元数据，没有字节）
//
// 见 kernel/demo64.h 的说明。这里只有：表查找 + 物理直映地址换算。**没有任何 ATA 代码** ——
// 原始区就在 loader 已经平铺加载进内存的内核区尾部（LBA 7497..8008 -> 物理地址见下）。
#include "demo64.h"
#include "demo64_blobtab.h"     // 构建期生成：DEMO64_RAW_LBA / DEMO64_BLOB_COUNT / g_demo64_blobtab64[]
#include "memlayout64.h"        // ML64_KERNEL_BASE / ML64_KERNEL_LBA / ML64_SECTOR_BYTES
// 原始区物理地址 = 内核平铺装载基址 + (DEMO64_RAW_LBA - KERNEL_LBA) * 扇区字节数。
// 内核里访问物理地址是常规做法（帧缓冲/安装载荷/页池都这样，见 linker64.ld 的 ★ 说明）。
static const uint64_t DEMO64_RAW_PHYS64 =
    (uint64_t)ML64_KERNEL_BASE +
    ((uint64_t)DEMO64_RAW_LBA - (uint64_t)ML64_KERNEL_LBA) * (uint64_t)ML64_SECTOR_BYTES;

static int d64_streq64(const char* a, const char* b) {
    while (*a != 0 && *a == *b) { a++; b++; }
    return *a == *b;                 // 两个都到结尾才相等
}

const uint8_t* demo64_blob_find64(const char* path, uint32_t* out_size) {
    if (out_size) *out_size = 0;
    if (!path) return nullptr;
    for (uint32_t i = 0; i < (uint32_t)DEMO64_BLOB_COUNT; i++) {
        const Demo64BlobEntry64* e = &g_demo64_blobtab64[i];
        if (!d64_streq64(e->path, path)) continue;
        if (out_size) *out_size = e->size;
        return (const uint8_t*)(uintptr_t)(DEMO64_RAW_PHYS64 + (uint64_t)e->off);
    }
    return nullptr;
}
