// demo64.cpp - 演示程序 blob 的"原始区"读取（内核侧；只有元数据，没有字节）
//
// 见 kernel/demo64.h 的说明。这里做两件事：
//   1) **按需**从盘上把"原始区"（system.img 的 LBA 7497 起）读进 .bss 缓冲（ATA PIO/AHCI
//      统一走 ata64_read）；为什么不直接用"loader 平铺加载进内存的那份"：内核 .bss
//      （fb 的 33MB 后备缓冲，物理 0x428000..0x23CC000）盖住了这段物理地址 —— bss_clear_64()
//      会先把它清零、之后的绘制也会覆写，实测第一次跑 demo64 就在 ring3 入口 #UD。
//      所以按老图标包那条纪律：**要用就按 LBA 从盘上读**（读一次缓存，失败就如实打点并
//      让调用方走"系统卷"路径）。
//   2) 按构建期生成的表（路径/偏移/长度）返回指针。
//
// ==================== ★ 本批修复：图标"有时有、有时没有"的根因 ====================
//   缺陷：原始区读取**硬编码 drive 0（PATA 主盘）**，而且失败后 g_demo64_raw_state64 = -1
//   **永久不重试**。只要引导盘不是 PATA-0（QEMU 的 ich9-ahci/SATA、VMware 的 SATA/AHCI、
//   NVMe、任何 drive 0 不存在或读失败的情形），原始区就永远读不到；于是"系统卷里没有
//   /etc/iconpack.bin"的系统（旧卷/用户自建卷/裸镜像）只能回落成程序化绘制的图标 ——
//   同一份系统换台机器/换个固件/换个控制器就变成"图标有时有有时没有"。
//   修法（两条，都只影响"读哪个盘、失败怎么办"）：
//     ① 候选盘按**引导/系统盘优先**：drive64_system_disk64()（C:/系统卷所在驱动器号，
//        来自 kernel/drive64 的盘符表）-> 没有就回退 0（老行为）；
//     ② 失败**不再永久缓存**：最多试 DEMO64_RAW_MAX_TRIES 次；而且一旦"系统盘"从
//        未知变成已知（盘符表扫描完成），就重开一轮 —— 启动早期（盘符扫描之前）那次
//        失败不会把后面的正确来源一起废掉。
//   打点里能看出：第几次、用哪个盘成功/失败、还会不会重试（retry=）。
#include "demo64.h"
#include "demo64_blobtab.h"     // 构建期生成：DEMO64_RAW_LBA / DEMO64_RAW_BYTES / g_demo64_blobtab64[]
#include "ata64.h"              // ata64_read(drive, lba, count, buf)
#include "drive64.h"            // ★ 修复：drive64_system_disk64()（引导/系统盘的驱动器号）
#include "debug64.h"

// 原始区缓冲（.bss；上限 DEMO64_RAW_MAX_BYTES = 512 扇区 = 256KB —— 当前实际 144,129 B）。
static uint8_t g_demo64_raw64[DEMO64_RAW_MAX_BYTES];
static int g_demo64_raw_state64 = 0;      // 0 = 还没读；1 = 已读；-1 = 当前候选盘配置试满 MAX 次仍失败
static uint8_t g_demo64_raw_tries64 = 0;  // ★ 修复：对"当前候选配置"已尝试的次数（失败不永久缓存的依据）
static int g_demo64_raw_syskey64 = -2;    // ★ 修复：上次尝试时的"系统盘签名"（-2 = 还没试过；-1 = 系统盘未知）
static int g_demo64_raw_src64 = -1;       // ★ 本批：当前缓冲**来自哪块盘**（-1 = 没有可用缓冲）

#define DEMO64_RAW_MAX_TRIES 4            // 同一候选盘配置最多试几次（之后每次调用直接返回失败，不再打盘）

// 候选盘（最多 2 个，顺序即优先级）：① 系统盘（C: 所在驱动器号）② drive 0（老路径）。
// 为什么不是 0/8/16/24 全探：0 之外的盘"读成功"并不等于"这块盘上有原始区"，猜错会拿到
// 别的盘 LBA 7497 的随机字节（老代码只读 0 时没有这个问题）。所以只信 drive64 的
// "系统盘"判定，判不出来就退回原来的 drive 0；剩下的交给失败重试机制。
static int d64_candidates64(int* out, int cap) {
    int n = 0;
    const int sys = drive64_system_disk64();
    if (sys >= 0 && n < cap) out[n++] = sys;
    if (n < cap && (n == 0 || out[0] != 0)) out[n++] = 0;
    return n;
}

// 读一次原始区（成功即缓存；失败按"候选配置"最多重试 DEMO64_RAW_MAX_TRIES 次）。
// 返回 1 = 缓冲可用；0 = 本次读失败（已打点；下次调用可能换盘再试）。
static int d64_ensure_raw64(void) {
    if (g_demo64_raw_state64 > 0) return 1;
    const int syskey = drive64_system_disk64();       // -1 = 系统盘还不知道（盘符表没扫过/没有 C:）
    if (g_demo64_raw_state64 < 0) {
        // 当前配置已试满：只有"系统盘"这一线索变了（例如盘符扫描完成）才重开一轮，
        // 否则如实返回失败，避免每次调用都去磨一块不存在的盘。
        if (syskey == g_demo64_raw_syskey64) return 0;
        g_demo64_raw_state64 = 0;
        g_demo64_raw_tries64 = 0;
    }
    const uint32_t secs = ((uint32_t)DEMO64_RAW_BYTES + 511u) / 512u;
    if (DEMO64_RAW_BYTES == 0 || secs > (DEMO64_RAW_MAX_BYTES / 512u)) {
        g_demo64_raw_state64 = -1;
        return 0;
    }
    int cand[2];
    const int nc = d64_candidates64(cand, 2);
    if (g_demo64_raw_tries64 < 0xFFu) g_demo64_raw_tries64++;
    g_demo64_raw_syskey64 = syskey;
    int used = -1;
    for (int i = 0; i < nc; i++) {
        if (ata64_read(cand[i], (uint32_t)DEMO64_RAW_LBA, secs, g_demo64_raw64)) { used = cand[i]; break; }
    }
    const bool ok = (used >= 0);
    g_demo64_raw_src64 = ok ? used : -1;              // ★ 本批：来源盘（打点/验收对齐用）
    if (ok) {
        g_demo64_raw_state64 = 1;
    } else if (g_demo64_raw_tries64 >= DEMO64_RAW_MAX_TRIES) {
        g_demo64_raw_state64 = -1;                    // 试满：这一配置下不再打盘（换配置会重开一轮）
    }
    dbg64_line_begin64();
    if (ok) {
        dbg64_str("[DEMO64] raw blob region loaded lba=");
        dbg64_dec((uint64_t)DEMO64_RAW_LBA);
        dbg64_str(" bytes=");
        dbg64_dec((uint64_t)DEMO64_RAW_BYTES);
        dbg64_str(" drive=");
        dbg64_dec((uint64_t)used);
        dbg64_str(" attempt=");
        dbg64_dec((uint64_t)g_demo64_raw_tries64);
        dbg64_str("（没有系统卷时的兜底来源）\n");
    } else {
        dbg64_str("[DEMO64] raw blob region read FAILED lba=");
        dbg64_dec((uint64_t)DEMO64_RAW_LBA);
        dbg64_str(" secs=");
        dbg64_dec((uint64_t)secs);
        dbg64_str(" drives=");
        for (int i = 0; i < nc; i++) {
            if (i != 0) dbg64_str(",");
            dbg64_dec((uint64_t)cand[i]);
        }
        dbg64_str(" attempt=");
        dbg64_dec((uint64_t)g_demo64_raw_tries64);
        dbg64_str(" retry=");
        dbg64_dec(g_demo64_raw_state64 == 0 ? 1 : 0);
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

// ==================== ★ 本批修复：按盘探测（供"系统盘未知"时逐盘找包）====================
// 语义见 kernel/demo64.h。要点：强制读成功 = 这份原始区就是"当前原始区"（查表命中的
// 指针都指向它），所以调用方**必须**在判定"里面没有要找的东西"后调用 demo64_raw_abandon64()。
int demo64_raw_try_drive64(int drive) {
    const uint32_t secs = ((uint32_t)DEMO64_RAW_BYTES + 511u) / 512u;
    if (drive < 0 || DEMO64_RAW_BYTES == 0 || secs > (DEMO64_RAW_MAX_BYTES / 512u)) return 0;
    if (!ata64_read(drive, (uint32_t)DEMO64_RAW_LBA, secs, g_demo64_raw64)) return 0;
    g_demo64_raw_state64 = 1;                          // 缓冲可用（就是这块盘）
    g_demo64_raw_src64 = drive;                        // ★ 本批：来源盘
    g_demo64_raw_syskey64 = drive64_system_disk64();
    dbg64_line_begin64();
    dbg64_str("[DEMO64] raw probe drive=");
    dbg64_dec((uint64_t)drive);
    dbg64_str(" loaded lba=");
    dbg64_dec((uint64_t)DEMO64_RAW_LBA);
    dbg64_str(" bytes=");
    dbg64_dec((uint64_t)DEMO64_RAW_BYTES);
    dbg64_nl();
    dbg64_line_end64();
    return 1;
}

void demo64_raw_abandon64(void) {
    g_demo64_raw_state64 = -1;                         // 丢弃：后续调用不再"用"这份内容
    g_demo64_raw_src64 = -1;                           // ★ 本批：来源盘作废（不再声称来自哪块盘）
    g_demo64_raw_syskey64 = drive64_system_disk64();
}

// ==================== ★ 本批：来源盘查询 + 只读元数据（见 kernel/demo64.h）====================
int demo64_raw_drive64(void) {
    return (g_demo64_raw_state64 > 0) ? g_demo64_raw_src64 : -1;
}

int demo64_blob_meta64(const char* path, uint32_t* out_lba, uint32_t* out_skip,
                       uint32_t* out_size, uint32_t* out_max_secs) {
    if (!path || !out_lba || !out_skip || !out_size || !out_max_secs) return -1;
    if (DEMO64_RAW_BYTES == 0) return -1;
    for (uint32_t i = 0; i < (uint32_t)DEMO64_BLOB_COUNT; i++) {
        const Demo64BlobEntry64* e = &g_demo64_blobtab64[i];
        if (!d64_streq64(e->path, path)) continue;
        if (e->size == 0) return -1;
        if ((uint64_t)e->off + (uint64_t)e->size > (uint64_t)DEMO64_RAW_BYTES) return -1;   // 表项越界：如实拒绝
        const uint32_t first_sec = e->off / 512u;                 // blob 起点所在扇区（原始区内的相对扇区号）
        const uint32_t raw_total  = ((uint32_t)DEMO64_RAW_BYTES + 511u) / 512u;
        if (first_sec >= raw_total) return -1;
        *out_lba      = (uint32_t)DEMO64_RAW_LBA + first_sec;
        *out_skip     = e->off - first_sec * 512u;
        *out_size     = e->size;
        *out_max_secs = raw_total - first_sec;                    // 钳在原始区内：绝不越读
        return 0;
    }
    return -1;
}
