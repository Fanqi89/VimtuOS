// pci64.cpp - PCI 配置空间 + BAR 解码（最小实现；语义/错误码/风险说明见 pci64.h）
//
// 体积记账（本批 ① 项的报告口径）：本文件只做三件事 —— 0xCF8/0xCFC 访问、BAR 解码、
// 8 条 BAR 缓存。刻意不做的（省字节，也省得与既有驱动打架）：
//   * 不做总线/设备枚举（枚举在 hwinfo64/hwui64 里已经有了，本批不改它们）；
//   * 不做 capability 链遍历（xHCI 的 xECP 走法在 xhci64.cpp 里，本批不改它）；
//   * 不使能 MEM decode（那是“驱动初始化”的活；pci_map_bar 只做映射，不替设备开 DMA）。
#include "pci64.h"
#include "port.h"          // outl/inl（0xCF8/0xCFC 配置口），与各驱动同款

static inline uint32_t pci64_cfg_addr64(uint32_t bdf, uint8_t off) {
    const uint32_t bus = (bdf >> 8) & 0xFFu;
    const uint32_t dev = (bdf >> 3) & 0x1Fu;
    const uint32_t fn  = bdf & 0x7u;
    return 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (uint32_t)(off & 0xFCu);
}

uint32_t pci64_cfg_rd32(uint32_t bdf, uint8_t off) {
    outl(0xCF8u, pci64_cfg_addr64(bdf, off));
    return inl(0xCFCu);
}
void pci64_cfg_wr32(uint32_t bdf, uint8_t off, uint32_t val) {
    outl(0xCF8u, pci64_cfg_addr64(bdf, off));
    outl(0xCFCu, val);
}
uint16_t pci64_cfg_rd16(uint32_t bdf, uint8_t off) {
    const uint32_t v = pci64_cfg_rd32(bdf, (uint8_t)(off & 0xFCu));
    return (uint16_t)((off & 2u) ? (v >> 16) : v);
}

int pci64_dev_present64(uint32_t bdf) {
    if (bdf > PCI64_BDF64_MAX64) return 0;
    const uint16_t ven = (uint16_t)(pci64_cfg_rd32(bdf, 0x00u) & 0xFFFFu);
    return (ven != 0xFFFFu && ven != 0x0000u) ? 1 : 0;
}

// ==================== BAR 缓存（同一 (bdf,bar) 只探一次）====================
// 探一次就会临时改设备的地址译码（见 pci64.h 的风险段），所以缓存既是省时间也是**降风险**。
// 8 条：够"一个用户态驱动服务同时看几个设备"（BDF 是打包整数，比较一次就够）。
static const int PCI64_CACHE64 = 8;
struct Pci64CacheEnt64 {
    uint32_t bdf;
    uint32_t size_lo;        // 低 32 位（大小最大 4 GiB，这里只用低 32 位足够 —— 见下）
    uint8_t  bar;
    uint8_t  is_64;
    uint8_t  used;
    uint8_t  pad;
    uint64_t phys;
};
static Pci64CacheEnt64 g_pci64_cache64[PCI64_CACHE64];

static int pci64_cache_find64(uint32_t bdf, uint8_t bar) {
    for (int i = 0; i < PCI64_CACHE64; i++) {
        if (g_pci64_cache64[i].used && g_pci64_cache64[i].bdf == bdf && g_pci64_cache64[i].bar == bar) return i;
    }
    return -1;
}

// ==================== BAR 解码 ====================
int pci64_bar_probe64(uint32_t bdf, uint8_t bar_index, Pci64Bar64* out) {
    if (!out) return PCI64_EINVAL64;
    if (bdf > PCI64_BDF64_MAX64 || bar_index > 5u) return PCI64_EINVAL64;
    out->phys = 0; out->size = 0; out->index = bar_index;
    out->is_io = 0; out->is_64 = 0; out->ok = 0;

    if (!pci64_dev_present64(bdf)) return PCI64_ENODEV64;

    // 缓存命中：直接给上次的结论（不再写配置空间）
    {
        const int ci = pci64_cache_find64(bdf, bar_index);
        if (ci >= 0) {
            out->phys  = g_pci64_cache64[ci].phys;
            out->size  = (uint64_t)g_pci64_cache64[ci].size_lo;
            out->is_64 = g_pci64_cache64[ci].is_64;
            out->ok    = 1;
            return 0;
        }
    }

    const uint8_t off = (uint8_t)(0x10u + 4u * bar_index);
    const uint32_t lo0 = pci64_cfg_rd32(bdf, off);
    if (lo0 & 1u) {                                     // bit0 = 1：I/O 端口 BAR（本调用只映射 MMIO）
        out->is_io = 1;
        return PCI64_EINVAL64;                          // ★ 连写都不写（见 pci64.h 的风险段）
    }
    if (lo0 == 0) return PCI64_ENODEV64;                // BAR 未实现（读回全 0）

    // 类型位 bits[2:1]：0b10 = 64 位地址 BAR（占两个 BAR 槽）。与 hda64/xhci64 的判法一致。
    const int is64 = (((lo0 >> 1) & 0x3u) == 0x2u) ? 1 : 0;
    uint32_t hi0 = 0;
    if (is64) {
        if (bar_index == 5u) return PCI64_EINVAL64;     // 64 位 BAR 不可能是最后一个槽
        hi0 = pci64_cfg_rd32(bdf, (uint8_t)(off + 4u));
    }
    // ★ 真实大小 = 写全 1 -> 回读 -> 取反 + 1（标准做法）。写完**立刻**按原值还原并回读确认。
    pci64_cfg_wr32(bdf, off, 0xFFFFFFFFu);
    if (is64) pci64_cfg_wr32(bdf, (uint8_t)(off + 4u), 0xFFFFFFFFu);
    const uint32_t lo1 = pci64_cfg_rd32(bdf, off);
    const uint32_t hi1 = is64 ? pci64_cfg_rd32(bdf, (uint8_t)(off + 4u)) : 0u;
    pci64_cfg_wr32(bdf, off, lo0);
    if (is64) pci64_cfg_wr32(bdf, (uint8_t)(off + 4u), hi0);
    if (pci64_cfg_rd32(bdf, off) != lo0) return PCI64_ENODEV64;   // 还原失败：宁可报错，不留错地址
    if (is64 && pci64_cfg_rd32(bdf, (uint8_t)(off + 4u)) != hi0) return PCI64_ENODEV64;

    uint64_t mask = (uint64_t)(lo1 & ~0xFull);          // 保留地址位，掩掉类型位
    if (is64) mask |= ((uint64_t)hi1 << 32);
    if (mask == 0) return PCI64_ENODEV64;               // 报 0 大小：这个 BAR 不存在
    // ★ 尺寸必须在 **该 BAR 的地址宽度**里取反 —— 这是本批实测踩到的真 bug：
    //   32 位 BAR 的 mask 高 32 位是 0，若在 64 位里算 `~mask + 1`，高 32 位会全变成 1，
    //   于是 16 KiB 的 HDA BAR 被算成 0xFFFFFFFF_00004000（≈18 EB）-> 超出映射槽 -> ENOMEM。
    //   64 位 BAR（高 32 位参与）才用 64 位取反。cache 里只存低 32 位 —— 修正后 32 位 BAR 的
    //   尺寸天然落在低 32 位里，64 位 BAR 的尺寸在本机（QEMU）也都 <= 4 GiB，够用。
    const uint64_t size = is64 ? ((~mask) + 1ull)
                               : (uint64_t)(uint32_t)((~(uint32_t)mask) + 1u);

    uint64_t phys = (uint64_t)(lo0 & ~0xFull);
    if (is64) phys |= ((uint64_t)hi0 << 32);
    phys &= ~(size - 1ull);                             // 按**实际大小**对齐（不盲信寄存器里的地址位）
    if (phys == 0 || phys >= 0x100000000ull) return PCI64_ENODEV64;  // 恒等映射只到 4GiB（memlayout64.h）

    out->phys  = phys;
    out->size  = size;
    out->is_64 = (uint8_t)is64;
    out->ok    = 1;

    {   // 进缓存（满了就整体不缓存 —— 不搬移、不淘汰，逻辑最少）
        for (int i = 0; i < PCI64_CACHE64; i++) {
            if (g_pci64_cache64[i].used) continue;
            g_pci64_cache64[i].used    = 1;
            g_pci64_cache64[i].bdf     = bdf;
            g_pci64_cache64[i].bar     = bar_index;
            g_pci64_cache64[i].is_64   = (uint8_t)is64;
            g_pci64_cache64[i].size_lo = (uint32_t)size;
            g_pci64_cache64[i].phys    = phys;
            break;
        }
    }
    return 0;
}
