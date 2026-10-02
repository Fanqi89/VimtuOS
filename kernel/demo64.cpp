// demo64.cpp - ★ 本批：演示程序 blob 的"原始区"读取（内核侧；只有元数据，没有字节）
//
// 见 kernel/demo64.h 的说明。这里做两件事：
//   1) **按需**从盘上把"原始区"（system.img 的 LBA 7497 起）读进 .bss 缓冲（ATA PIO/AHCI
//      统一走 ata64_read）；为什么不直接用"loader 平铺加载进内存的那份"：内核 .bss
//      （fb 的 33MB 后备缓冲，物理 0x428000..0x23CC000）盖住了这段物理地址 —— bss_clear_64()
//      会先把它清零、之后的绘制也会覆写，实测第一次跑 demo64 就在 ring3 入口 #UD。
//      所以按老图标包那条纪律：**要用就按 LBA 从盘上读**（读一次缓存，失败就如实打点并
//      让调用方走"系统卷"路径）。
//   2) 按构建期生成的表（路径/偏移/长度）返回指针。
#include "demo64.h"
#include "demo64_blobtab.h"     // 构建期生成：DEMO64_RAW_LBA / DEMO64_RAW_BYTES / g_demo64_blobtab64[]
#include "ata64.h"              // ata64_read(drive, lba, count, buf)
#include "debug64.h"

// 原始区缓冲（.bss；上限 DEMO64_RAW_MAX_BYTES = 512 扇区 = 256KB —— 当前实际 144,129 B）。
static uint8_t g_demo64_raw64[DEMO64_RAW_MAX_BYTES];
static int g_demo64_raw_state64 = 0;      // 0 = 还没读；1 = 已读；-1 = 读取失败（不再重试）

// 读一次原始区（幂等）。返回 1 = 缓冲可用；0 = 读失败（已打点）。
static int d64_ensure_raw64(void) {
    if (g_demo64_raw_state64 != 0) return g_demo64_raw_state64 > 0;
    const uint32_t secs = ((uint32_t)DEMO64_RAW_BYTES + 511u) / 512u;
    if (DEMO64_RAW_BYTES == 0 || secs > (DEMO64_RAW_MAX_BYTES / 512u)) {
        g_demo64_raw_state64 = -1;
        return 0;
    }
    // drive 0 = primary master：与系统卷挂载（app64_*）用的是同一块系统盘。
    const bool ok = ata64_read(0, (uint32_t)DEMO64_RAW_LBA, secs, g_demo64_raw64);
    g_demo64_raw_state64 = ok ? 1 : -1;
    dbg64_line_begin64();
    if (ok) {
        dbg64_str("[DEMO64] raw blob region loaded lba=");
        dbg64_dec((uint64_t)DEMO64_RAW_LBA);
        dbg64_str(" bytes=");
        dbg64_dec((uint64_t)DEMO64_RAW_BYTES);
        dbg64_str("（没有系统卷时的兜底来源）\n");
    } else {
        dbg64_str("[DEMO64] raw blob region read FAILED lba=");
        dbg64_dec((uint64_t)DEMO64_RAW_LBA);
        dbg64_str(" secs=");
        dbg64_dec((uint64_t)secs);
        dbg64_str(" -> 演示程序只能来自系统卷\n");
    }
    dbg64_line_end64();
    return ok;
}

static int d64_streq64(const char* a, const char* b) {
    while (*a != 0 && *a == *b) { a++; b++; }
    return *a == *b;                 // 两个都到结尾才相等
}

const uint8_t* demo64_blob_find64(const char* path, uint32_t* out_size) {
    if (out_size) *out_size = 0;
    if (!path) return nullptr;
    if (!d64_ensure_raw64()) return nullptr;
    for (uint32_t i = 0; i < (uint32_t)DEMO64_BLOB_COUNT; i++) {
        const Demo64BlobEntry64* e = &g_demo64_blobtab64[i];
        if (!d64_streq64(e->path, path)) continue;
        if ((uint64_t)e->off + (uint64_t)e->size > (uint64_t)DEMO64_RAW_BYTES) return nullptr;
        if (out_size) *out_size = e->size;
        return &g_demo64_raw64[e->off];
    }
    return nullptr;
}
