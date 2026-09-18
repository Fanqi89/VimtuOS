// vfs64.cpp - VimtuFS2 实现：超级块 + 空闲块位图 + 64B inode（名字直接放在 inode 里）
//
// 磁盘布局（相对分区起始 LBA，块号从 0 开始；1 块 = 1 扇区 = 512B）：
//   块 0              : 超级块（见下面 VFS_O_* 偏移；末尾 0xAA55；CRC32 自校验）
//   块 1 .. 1+bmn-1   : 空闲块位图（1 = 已用，额外把"分区外"的位也置 1，分配器永不发放）
//   块 bp .. bp+ibn-1 : inode 表（64B/个，8 个/块；inode 0 = 根目录）
//   块 dp .. total-1  : 数据区（文件内容 + 一级间接块，文件最大 = 4*512 + 128*512 = 67584B）
//   bmn = ceil(total/4096)、ibn = ceil(inodes/8)、bp = 1+bmn、dp = bp+ibn
//
// 设计取舍：名字放在 inode 里（见 vfs64.h），所以"目录"没有数据块，一个目录就是
// "父 inode 号相同的 inode 列表"。好处：少一层指针、少一类越界、格式最简单；
// 代价：条目上限 = inode 数（256）、ls 是 O(inode 数)、名字 ≤27B、无硬链接。
//
// 健壮性约定（所有磁盘访问都走这里的三条纪律）：
//   1) 任何盘上结构都先读到静态缓冲（g_sec / g_ind / 局部 64B inode 缓冲）再解析，
//      绝不把盘上字节直接当结构体指针用（避免未对齐/越界读）；
//   2) 每个结构先校验（超级块 magic/版本/CRC/几何重算；inode 类型/名字长度/CRC/大小；
//      块号必须落在数据区内；inode 号必须 < inode 数），不合法一律 -1 + 打点；
//   3) 写文件时"先分配新块 + 写完数据 + 提交 inode，最后才释放旧块"—— 中途失败只
//      可能泄漏几个块，绝不会让 inode 指向已被释放（可能被别人复用）的块。
//
// ATA 用 __attribute__((weak)) 引用（签名与 kernel/ata64.h 逐字一致，符号是 C++ 名）：
//   安装介质内核链接 ata64.o -> 正常读写真盘；系统内核不链接 -> 弱符号为 0，
//   所有操作按"无设备"处理并打印 "[VFS64] no ATA driver linked"，绝不调用空指针。
//   自检里的 64 扇区假盘只是为了不依赖真盘（见 vfs64_selftest64）。

#include "vfs64.h"
#include "debug64.h"
#include "ata64.h"

__attribute__((weak)) bool ata64_read (int drive, uint32_t lba, uint32_t count, void* buf);
__attribute__((weak)) bool ata64_write(int drive, uint32_t lba, uint32_t count, const void* buf);

// ==================== 超级块字段偏移（扇区 0，全部小端）====================
static const uint32_t VFS_O_MAGIC    = 0;    // 8B  "VIMTUFS2"
static const uint32_t VFS_O_VERSION  = 8;    // u32 结构版本 = 2
static const uint32_t VFS_O_SECTOR   = 12;   // u32 扇区大小 = 512
static const uint32_t VFS_O_BLOCK    = 16;   // u32 块大小 = 512
static const uint32_t VFS_O_TOTAL    = 20;   // u32 本卷总块数（= 分区扇区数）
static const uint32_t VFS_O_ROOT     = 24;   // u32 根目录 inode 号（固定 0）
static const uint32_t VFS_O_BITMAP   = 28;   // u32 位图起始块
static const uint32_t VFS_O_BITMAPN  = 32;   // u32 位图块数
static const uint32_t VFS_O_INODE    = 36;   // u32 inode 区起始块
static const uint32_t VFS_O_INODEN   = 40;   // u32 inode 个数
static const uint32_t VFS_O_INOSZ    = 44;   // u32 单个 inode 字节数 = 64
static const uint32_t VFS_O_DATA     = 48;   // u32 数据区起始块
static const uint32_t VFS_O_DATAN    = 52;   // u32 数据区块数
static const uint32_t VFS_O_FLAGS    = 56;   // u32 保留（0；改它就等于改 CRC 覆盖区）
static const uint32_t VFS_O_CRC      = 60;   // u32 超级块 CRC32（覆盖 [0,60)）
static const uint32_t VFS_O_SIG      = 510;  // u16 0xAA55（与老占位超级块同样的人工指纹）
static const uint32_t VFS_SB_CRC_LEN = 60;

// ==================== inode 字段偏移（64B，全部小端）====================
static const uint32_t VFS_I_TYPE    = 0;     // u8  0=空 1=文件 2=目录
static const uint32_t VFS_I_NAMELEN = 1;     // u8  名字长度 0..27（0 仅用于根目录）
static const uint32_t VFS_I_RSVD    = 2;     // u16 保留（0）
static const uint32_t VFS_I_SIZE    = 4;     // u32 文件字节数
static const uint32_t VFS_I_D0      = 8;     // u32 直接块 0..3（偏移 8/12/16/20）
static const uint32_t VFS_I_IND     = 24;    // u32 一级间接块（存 128 个块号）
static const uint32_t VFS_I_PARENT  = 28;    // u32 父目录 inode 号（根 = 自己 = 0）
static const uint32_t VFS_I_NAME    = 32;    // 28B 名字（ASCII，NUL 兜底）
static const uint32_t VFS_I_CRC     = 60;    // u32 inode CRC32（覆盖 [0,60)）
static const uint32_t VFS_I_CRC_LEN = 60;

static_assert(VFS64_BLOCK_BYTES == VFS64_SECTOR_BYTES, "块就是扇区（vfs64_format 也按这个算几何）");
static_assert(VFS64_INODES_PER_BLK * VFS64_INODE_BYTES == VFS64_BLOCK_BYTES, "每块必须正好 8 个 inode");
static_assert(VFS64_BITMAP_BLK_BITS == VFS64_BLOCK_BYTES * 8u, "位图块 512B = 4096 个块位");
static_assert(VFS_I_CRC + 4u == VFS64_INODE_BYTES, "inode 字段必须正好铺满 64B");
static_assert(VFS_O_CRC + 4u <= VFS64_SECTOR_BYTES, "超级块 CRC 必须落在扇区 0 内");

// ==================== 小工具（不依赖 libc）====================
static void zero_bytes(void* p, uint32_t n) {
    uint8_t* d = (uint8_t*)p;
    for (uint32_t i = 0; i < n; i++) d[i] = 0;
}
static void copy_bytes(void* dst, const void* src, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}
static int cmp_bytes(const void* a, const void* b, uint32_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (uint32_t i = 0; i < n; i++) if (x[i] != y[i]) return (x[i] < y[i]) ? -1 : 1;
    return 0;
}
static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}
// 标准 CRC-32（反射 0xEDB88320，初值/末异或 0xFFFFFFFF，与 zlib/zip 一致）：
//   crc32("123456789") == 0xCBF43926（自检里写死断言，防实现漂移）
static uint32_t crc32_64(const uint8_t* p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}

// ==================== 内存状态（全静态 .bss，无 new/delete）====================
// 工作缓冲只有 1 个扇区 + 1 个间接块镜像 + 128 个块号（≈1.5KB）；假盘与自检缓冲只服务于自检。
static uint8_t  g_sec[VFS64_SECTOR_BYTES];            // 唯一工作扇区（先读进来再解析）
static uint8_t  g_ind[VFS64_SECTOR_BYTES];            // 待写出的间接块镜像
static uint32_t g_ptrs[VFS64_INDIRECT_PTRS];          // 读间接块 / 收集旧块号

static bool     g_mounted     = false;
static int      g_drive       = -1;
static uint32_t g_start       = 0;                    // 分区起始绝对 LBA
static uint32_t g_blocks      = 0;                    // 总块数
static uint32_t g_bitmap_start = 0;
static uint32_t g_bitmap_blocks = 0;
static uint32_t g_inode_start  = 0;
static uint32_t g_inode_count  = 0;
static uint32_t g_data_start   = 0;
static uint32_t g_data_blocks  = 0;

struct Vfs64Geom {
    bool     mounted;
    int      drive;
    uint32_t start;
    uint32_t blocks;
    uint32_t bitmap_start;
    uint32_t bitmap_blocks;
    uint32_t inode_start;
    uint32_t inode_count;
    uint32_t data_start;
    uint32_t data_blocks;
};
static void geom_save(Vfs64Geom* g) {
    g->mounted = g_mounted;   g->drive = g_drive;            g->start = g_start;
    g->blocks = g_blocks;     g->bitmap_start = g_bitmap_start; g->bitmap_blocks = g_bitmap_blocks;
    g->inode_start = g_inode_start; g->inode_count = g_inode_count;
    g->data_start = g_data_start;   g->data_blocks = g_data_blocks;
}
static void geom_restore(const Vfs64Geom* g) {
    g_mounted = g->mounted;   g_drive = g->drive;            g_start = g->start;
    g_blocks = g->blocks;     g_bitmap_start = g->bitmap_start; g_bitmap_blocks = g->bitmap_blocks;
    g_inode_start = g->inode_start; g_inode_count = g->inode_count;
    g_data_start = g->data_start;   g_data_blocks = g->data_blocks;
}

// ==================== 串口打点（统一 [VFS64] 前缀）====================
static void log_line(const char* s) {
    dbg64_str("[VFS64] ");
    dbg64_str(s);
    dbg64_nl();
}
static void log_hex32(uint32_t v) {
    static const char* H = "0123456789ABCDEF";
    char b[9];
    for (int i = 7; i >= 0; i--) { b[i] = H[v & 0xFu]; v >>= 4; }
    b[8] = 0;
    dbg64_str(b);
}
static void log_mount_fail(const char* why) {
    dbg64_str("[VFS64] mount FAILED reason=");
    dbg64_str(why);
    dbg64_nl();
}
static void log_bad_block(uint32_t blk) {
    dbg64_str("[VFS64] BAD block=");
    dbg64_dec(blk);
    dbg64_str(" not in data area [");
    dbg64_dec(g_data_start);
    dbg64_str(",");
    dbg64_dec(g_blocks);
    dbg64_str(")");
    dbg64_nl();
}
static void log_bad_inode(uint32_t idx) {
    dbg64_str("[VFS64] BAD inode=");
    dbg64_dec(idx);
    dbg64_str(" (count=");
    dbg64_dec(g_inode_count);
    dbg64_str(")");
    dbg64_nl();
}

// ==================== 设备层：假盘 / 弱 ATA ====================
static const uint32_t VFS64_FAKE_SECTORS = 64;                          // 自检假盘：32KB
static uint8_t  g_fake_disk[VFS64_FAKE_SECTORS * VFS64_SECTOR_BYTES];
static bool     g_fake_active = false;

static bool ata_linked() {
    return (ata64_read != nullptr) && (ata64_write != nullptr);
}

// 绝对 LBA 读（count 个扇区）；count 上限 255 由这里分块，调用方不用管。
static bool dev_read(uint32_t abs_lba, uint32_t count, void* buf) {
    if (count == 0) return true;
    if (g_fake_active) {
        if ((uint64_t)abs_lba + count > VFS64_FAKE_SECTORS) {
            dbg64_str("[VFS64] fake-disk range fail lba=");
            dbg64_dec(abs_lba);
            dbg64_str(" count=");
            dbg64_dec(count);
            dbg64_nl();
            return false;
        }
        copy_bytes(buf, &g_fake_disk[(uint64_t)abs_lba * VFS64_SECTOR_BYTES], count * VFS64_SECTOR_BYTES);
        return true;
    }
    if (!ata_linked()) { log_line("no ATA driver linked"); return false; }
    uint8_t* d = (uint8_t*)buf;
    while (count > 0) {
        const uint32_t c = (count > 255u) ? 255u : count;
        if (!ata64_read(g_drive, abs_lba, c, d)) {
            dbg64_str("[VFS64] ATA read fail lba=");
            dbg64_dec(abs_lba);
            dbg64_nl();
            return false;
        }
        abs_lba += c;
        d += c * VFS64_SECTOR_BYTES;
        count -= c;
    }
    return true;
}
static bool dev_write(uint32_t abs_lba, uint32_t count, const void* buf) {
    if (count == 0) return true;
    if (g_fake_active) {
        if ((uint64_t)abs_lba + count > VFS64_FAKE_SECTORS) {
            dbg64_str("[VFS64] fake-disk range fail lba=");
            dbg64_dec(abs_lba);
            dbg64_nl();
            return false;
        }
        copy_bytes(&g_fake_disk[(uint64_t)abs_lba * VFS64_SECTOR_BYTES], buf, count * VFS64_SECTOR_BYTES);
        return true;
    }
    if (!ata_linked()) { log_line("no ATA driver linked"); return false; }
    const uint8_t* s = (const uint8_t*)buf;
    while (count > 0) {
        const uint32_t c = (count > 255u) ? 255u : count;
        if (!ata64_write(g_drive, abs_lba, c, s)) {
            dbg64_str("[VFS64] ATA write fail lba=");
            dbg64_dec(abs_lba);
            dbg64_nl();
            return false;
        }
        abs_lba += c;
        s += c * VFS64_SECTOR_BYTES;
        count -= c;
    }
    return true;
}

// ==================== 超级块校验 / 几何 ====================
enum {
    SB_OK = 0,
    SB_ERR_READ = 1,
    SB_ERR_MAGIC,
    SB_ERR_VERSION,
    SB_ERR_CRC,
    SB_ERR_LAYOUT
};
static const char* sb_reason_str(int r) {
    if (r == SB_ERR_READ)    return "read";
    if (r == SB_ERR_MAGIC)   return "magic";
    if (r == SB_ERR_VERSION) return "version";
    if (r == SB_ERR_CRC)     return "crc";
    if (r == SB_ERR_LAYOUT)  return "layout";
    return "?";
}
// 读盘内容来自 g_sec（静态缓冲）。magic -> CRC -> 版本 -> 几何重算，逐项都要过。
static int sb_verify(const uint8_t* sb) {
    static const char magic[8] = { 'V','I','M','T','U','F','S','2' };
    if (cmp_bytes(sb + VFS_O_MAGIC, magic, 8) != 0) return SB_ERR_MAGIC;
    if (rd32(sb + VFS_O_CRC) != crc32_64(sb, VFS_SB_CRC_LEN)) return SB_ERR_CRC;
    if (rd32(sb + VFS_O_VERSION) != VFS64_VERSION) return SB_ERR_VERSION;
    if (rd16(sb + VFS_O_SIG) != 0xAA55u) return SB_ERR_LAYOUT;

    const uint32_t sector = rd32(sb + VFS_O_SECTOR);
    const uint32_t block  = rd32(sb + VFS_O_BLOCK);
    const uint32_t total  = rd32(sb + VFS_O_TOTAL);
    const uint32_t root   = rd32(sb + VFS_O_ROOT);
    const uint32_t bm     = rd32(sb + VFS_O_BITMAP);
    const uint32_t bmn    = rd32(sb + VFS_O_BITMAPN);
    const uint32_t ino    = rd32(sb + VFS_O_INODE);
    const uint32_t inon   = rd32(sb + VFS_O_INODEN);
    const uint32_t inosz  = rd32(sb + VFS_O_INOSZ);
    const uint32_t data   = rd32(sb + VFS_O_DATA);
    const uint32_t datan  = rd32(sb + VFS_O_DATAN);

    if (sector != VFS64_SECTOR_BYTES || block != VFS64_BLOCK_BYTES || inosz != VFS64_INODE_BYTES)
        return SB_ERR_LAYOUT;
    if (total < VFS64_MIN_BLOCKS) return SB_ERR_LAYOUT;
    if (inon == 0 || inon > VFS64_MAX_INODES) return SB_ERR_LAYOUT;
    if (root >= inon) return SB_ERR_LAYOUT;

    // 布局必须"严丝合缝"：位图紧跟超级块，inode 区紧跟位图，数据区紧跟 inode 区
    const uint32_t bmn_calc   = (total + VFS64_BITMAP_BLK_BITS - 1u) / VFS64_BITMAP_BLK_BITS;
    const uint32_t ino_blocks = (inon + VFS64_INODES_PER_BLK - 1u) / VFS64_INODES_PER_BLK;
    if (bm != 1u) return SB_ERR_LAYOUT;
    if (bmn != bmn_calc) return SB_ERR_LAYOUT;
    if (ino != bm + bmn) return SB_ERR_LAYOUT;
    if (data != ino + ino_blocks) return SB_ERR_LAYOUT;
    if (data >= total) return SB_ERR_LAYOUT;
    if (datan != total - data) return SB_ERR_LAYOUT;
    return SB_OK;
}

// 统计数据区里还空着多少块（挂载打点用）。位图必须能全部读出，否则返回 false。
static bool count_free_blocks(uint32_t* out) {
    uint32_t free = 0;
    for (uint32_t m = 0; m < g_bitmap_blocks; m++) {
        if (!dev_read(g_start + g_bitmap_start + m, 1, g_sec)) return false;
        for (uint32_t k = 0; k < VFS64_BITMAP_BLK_BITS; k++) {
            const uint32_t blk = m * VFS64_BITMAP_BLK_BITS + k;
            if (blk < g_data_start || blk >= g_blocks) continue;
            if ((g_sec[k >> 3] & (uint8_t)(1u << (k & 7u))) == 0) free++;
        }
    }
    *out = free;
    return true;
}

// ==================== 块读写 / 位图 ====================
static bool blk_ok_data(uint32_t blk) {
    return blk >= g_data_start && blk < g_blocks;
}
static bool blk_read(uint32_t blk, uint8_t* buf) {
    if (blk >= g_blocks) { log_bad_block(blk); return false; }
    return dev_read(g_start + blk, 1, buf);
}
static bool blk_write(uint32_t blk, const uint8_t* buf) {
    if (blk >= g_blocks) { log_bad_block(blk); return false; }
    return dev_write(g_start + blk, 1, buf);
}

// 位图置位/清位（读-改-写一个位图扇区）。blk 是分区相对块号。
static bool bitmap_set(uint32_t blk, bool used) {
    if (blk >= g_blocks) { log_bad_block(blk); return false; }
    const uint32_t m = blk / VFS64_BITMAP_BLK_BITS;
    const uint32_t k = blk % VFS64_BITMAP_BLK_BITS;
    if (m >= g_bitmap_blocks) { log_bad_block(blk); return false; }
    if (!dev_read(g_start + g_bitmap_start + m, 1, g_sec)) return false;
    const uint8_t mask = (uint8_t)(1u << (k & 7u));
    if (used) g_sec[k >> 3] |= mask;
    else      g_sec[k >> 3] &= (uint8_t)~mask;
    return dev_write(g_start + g_bitmap_start + m, 1, g_sec);
}

// 分配一个数据区空闲块：置位并返回块号；没有空闲返回 0（块 0 是超级块，永远已用，可当失败哨兵）。
static uint32_t alloc_block() {
    for (uint32_t m = 0; m < g_bitmap_blocks; m++) {
        if (!dev_read(g_start + g_bitmap_start + m, 1, g_sec)) return 0;
        for (uint32_t k = 0; k < VFS64_BITMAP_BLK_BITS; k++) {
            const uint32_t blk = m * VFS64_BITMAP_BLK_BITS + k;
            if (blk < g_data_start || blk >= g_blocks) continue;
            if ((g_sec[k >> 3] & (uint8_t)(1u << (k & 7u))) != 0) continue;
            g_sec[k >> 3] |= (uint8_t)(1u << (k & 7u));
            if (!dev_write(g_start + g_bitmap_start + m, 1, g_sec)) return 0;
            return blk;
        }
    }
    return 0;
}

// ==================== inode 读写 / 校验 ====================
static bool inode_load(uint32_t idx, uint8_t* out) {
    if (idx >= g_inode_count) { log_bad_inode(idx); return false; }
    const uint32_t blk = g_inode_start + idx / VFS64_INODES_PER_BLK;
    const uint32_t off = (idx % VFS64_INODES_PER_BLK) * VFS64_INODE_BYTES;
    if (!blk_read(blk, g_sec)) return false;
    copy_bytes(out, g_sec + off, VFS64_INODE_BYTES);
    return true;
}
static bool inode_store(uint32_t idx, const uint8_t* in) {
    if (idx >= g_inode_count) { log_bad_inode(idx); return false; }
    const uint32_t blk = g_inode_start + idx / VFS64_INODES_PER_BLK;
    const uint32_t off = (idx % VFS64_INODES_PER_BLK) * VFS64_INODE_BYTES;
    if (!blk_read(blk, g_sec)) return false;                 // 读-改-写：只替换 64B 槽
    copy_bytes(g_sec + off, in, VFS64_INODE_BYTES);
    return blk_write(blk, g_sec);
}
// 结构合法性：类型/名字长度/CRC/大小。注意空槽（type=0）不校验 CRC——格式化后它就是全 0。
static bool inode_ok(const uint8_t* b, const char** why) {
    const uint8_t t = b[VFS_I_TYPE];
    if (t > VFS64_TYPE_DIR) { *why = "type"; return false; }
    if (b[VFS_I_NAMELEN] > VFS64_NAME_MAX) { *why = "namelen"; return false; }
    if (rd16(b + VFS_I_RSVD) != 0) { *why = "reserved"; return false; }
    if (t == VFS64_TYPE_FREE) return true;
    if (rd32(b + VFS_I_CRC) != crc32_64(b, VFS_I_CRC_LEN)) { *why = "crc"; return false; }
    if (t == VFS64_TYPE_FILE && rd32(b + VFS_I_SIZE) > VFS64_MAX_FILE_BYTES) { *why = "size"; return false; }
    return true;
}
// 找一个空 inode 槽（跳过 0 号根目录）
static bool alloc_inode(uint32_t* out_idx) {
    uint8_t ino[VFS64_INODE_BYTES];
    for (uint32_t i = 1; i < g_inode_count; i++) {
        if (!inode_load(i, ino)) return false;
        if (ino[VFS_I_TYPE] == VFS64_TYPE_FREE) { *out_idx = i; return true; }
    }
    return false;
}
// 读出一级间接块里的 128 个块号（不解析指针内容，只搬运到 g_ptrs）
static bool load_indirect(uint32_t ind, uint32_t* out128) {
    if (!blk_ok_data(ind)) { log_bad_block(ind); return false; }
    if (!blk_read(ind, g_sec)) return false;
    for (uint32_t k = 0; k < VFS64_INDIRECT_PTRS; k++) out128[k] = rd32(g_sec + 4u * k);
    return true;
}
// 释放 inode 引用的全部数据块（直接 + 间接）。遇到越界块号：打印并返回 false，
// 但已经释放的部分保持释放（宁可泄漏，也不越界读写）。
static bool free_file_blocks(const uint8_t* ino) {
    bool ok = true;
    for (uint32_t d = 0; d < VFS64_DIRECT_BLOCKS; d++) {
        const uint32_t b = rd32(ino + VFS_I_D0 + 4u * d);
        if (b == 0) continue;
        if (!blk_ok_data(b)) { log_bad_block(b); ok = false; continue; }
        if (!bitmap_set(b, false)) ok = false;
    }
    const uint32_t ind = rd32(ino + VFS_I_IND);
    if (ind != 0) {
        if (!blk_ok_data(ind)) { log_bad_block(ind); return false; }
        if (!load_indirect(ind, g_ptrs)) return false;
        for (uint32_t k = 0; k < VFS64_INDIRECT_PTRS; k++) {
            const uint32_t b = g_ptrs[k];
            if (b == 0) continue;
            if (!blk_ok_data(b)) { log_bad_block(b); ok = false; continue; }
            if (!bitmap_set(b, false)) ok = false;
        }
        if (!bitmap_set(ind, false)) ok = false;
    }
    return ok;
}

// ==================== 名字 / 路径（阶段一：单层）====================
static bool name_valid(const char* name, uint32_t len) {
    if (len == 0 || len > VFS64_NAME_MAX) return false;
    if (len == 1 && name[0] == '.') return false;
    if (len == 2 && name[0] == '.' && name[1] == '.') return false;
    for (uint32_t i = 0; i < len; i++) {
        const char c = name[i];
        if (c < 0x20 || c > 0x7E || c == '/') return false;   // 可打印 ASCII，'/' 是路径分隔符
    }
    return true;
}
// ""、"/"、"//" -> 根目录（is_root=true）；"/name"、"name" -> 单层名。
// 出现第二个 '/'（多级路径）或名字非法 -> -1（阶段一明确拒绝并打点）。
static int path_split(const char* path, char* name, uint32_t* nlen, bool* is_root) {
    if (!path) return -1;
    const char* p = path;
    while (*p == '/') p++;
    if (*p == 0) { *is_root = true; *nlen = 0; name[0] = 0; return 0; }
    *is_root = false;
    uint32_t n = 0;
    while (p[n] != 0) {
        if (p[n] == '/') return -1;
        if (n >= VFS64_NAME_MAX) return -1;
        name[n] = p[n];
        n++;
    }
    name[n] = 0;
    *nlen = n;
    return name_valid(name, n) ? 0 : -1;
}
// 在目录 dir 里按名字找子项（阶段一 dir 恒为 0）。返回 0 = 找到，-1 = 没找到/读盘失败。
static int find_child(uint32_t dir, const char* name, uint32_t nlen, uint32_t* out_idx) {
    uint8_t ino[VFS64_INODE_BYTES];
    for (uint32_t i = 1; i < g_inode_count; i++) {
        if (!inode_load(i, ino)) return -1;
        if (ino[VFS_I_TYPE] == VFS64_TYPE_FREE) continue;
        if (rd32(ino + VFS_I_PARENT) != dir) continue;
        if (ino[VFS_I_NAMELEN] != nlen) continue;
        bool same = true;
        for (uint32_t j = 0; j < nlen; j++) {
            if (ino[VFS_I_NAME + j] != (uint8_t)name[j]) { same = false; break; }
        }
        if (!same) continue;
        const char* why = "?";
        if (!inode_ok(ino, &why)) {                // 名字对上了但结构坏了：不认它，并报出来
            dbg64_str("[VFS64] skip corrupt inode=");
            dbg64_dec(i);
            dbg64_str(" reason=");
            dbg64_str(why);
            dbg64_nl();
            continue;
        }
        if (out_idx != nullptr) *out_idx = i;
        return 0;
    }
    return -1;
}

// ==================== 格式化 ====================
int vfs64_format(int drive, uint32_t start_lba, uint32_t total_sectors) {
    // 参数先校验：不合法绝不碰任何状态（也就不会破坏当前已挂载的卷）
    if (total_sectors < VFS64_MIN_BLOCKS) {
        dbg64_str("[VFS64] format FAILED reason=too-small sectors=");
        dbg64_dec(total_sectors);
        dbg64_nl();
        return -1;
    }
    if (!g_fake_active && !ata_linked()) {
        log_line("no ATA driver linked");
        dbg64_str("[VFS64] format FAILED reason=no-device");
        dbg64_nl();
        return -1;
    }

    // 几何：位图在最前，inode 区居中，数据区占剩下的全部
    const uint32_t bitmap_blocks = (total_sectors + VFS64_BITMAP_BLK_BITS - 1u) / VFS64_BITMAP_BLK_BITS;
    uint32_t inodes = total_sectors / 64u;
    if (inodes < 16u) inodes = 16u;
    if (inodes > VFS64_MAX_INODES) inodes = VFS64_MAX_INODES;
    const uint32_t inode_blocks = (inodes + VFS64_INODES_PER_BLK - 1u) / VFS64_INODES_PER_BLK;
    const uint32_t bitmap_start = 1u;
    const uint32_t inode_start  = bitmap_start + bitmap_blocks;
    const uint32_t data_start   = inode_start + inode_blocks;
    const uint32_t data_blocks  = total_sectors - data_start;

    g_mounted = false;
    g_drive = drive;
    g_start = start_lba;
    g_blocks = total_sectors;
    g_bitmap_start = bitmap_start;
    g_bitmap_blocks = bitmap_blocks;
    g_inode_start = inode_start;
    g_inode_count = inodes;
    g_data_start = data_start;
    g_data_blocks = data_blocks;

    // 1) 超级块（先把 CRC 覆盖区清零，再填字段，最后算 CRC）
    zero_bytes(g_sec, VFS64_SECTOR_BYTES);
    copy_bytes(g_sec + VFS_O_MAGIC, "VIMTUFS2", 8);
    wr32(g_sec + VFS_O_VERSION, VFS64_VERSION);
    wr32(g_sec + VFS_O_SECTOR, VFS64_SECTOR_BYTES);
    wr32(g_sec + VFS_O_BLOCK, VFS64_BLOCK_BYTES);
    wr32(g_sec + VFS_O_TOTAL, total_sectors);
    wr32(g_sec + VFS_O_ROOT, 0);
    wr32(g_sec + VFS_O_BITMAP, bitmap_start);
    wr32(g_sec + VFS_O_BITMAPN, bitmap_blocks);
    wr32(g_sec + VFS_O_INODE, inode_start);
    wr32(g_sec + VFS_O_INODEN, inodes);
    wr32(g_sec + VFS_O_INOSZ, VFS64_INODE_BYTES);
    wr32(g_sec + VFS_O_DATA, data_start);
    wr32(g_sec + VFS_O_DATAN, data_blocks);
    wr32(g_sec + VFS_O_FLAGS, 0);
    wr32(g_sec + VFS_O_CRC, crc32_64(g_sec, VFS_SB_CRC_LEN));
    g_sec[VFS_O_SIG] = 0x55;
    g_sec[VFS_O_SIG + 1] = 0xAA;
    if (!dev_write(start_lba, 1, g_sec)) {
        log_line("format FAILED step=superblock");
        g_mounted = false;
        return -1;
    }

    // 2) 空闲块位图：块 0..data_start-1（超级块/位图自身/inode 区）标已用；
    //    分区外的尾部位也标已用，保证分配器永远不会发放它们。
    for (uint32_t m = 0; m < bitmap_blocks; m++) {
        zero_bytes(g_sec, VFS64_SECTOR_BYTES);
        for (uint32_t k = 0; k < VFS64_BITMAP_BLK_BITS; k++) {
            const uint32_t blk = m * VFS64_BITMAP_BLK_BITS + k;
            if (blk >= total_sectors || blk < data_start) g_sec[k >> 3] |= (uint8_t)(1u << (k & 7u));
        }
        if (!dev_write(start_lba + bitmap_start + m, 1, g_sec)) {
            log_line("format FAILED step=bitmap");
            g_mounted = false;
            return -1;
        }
    }

    // 3) inode 区整体清零（抹掉旧卷残留，避免旧 inode 被当有效条目）
    for (uint32_t i = 0; i < inode_blocks; i++) {
        zero_bytes(g_sec, VFS64_SECTOR_BYTES);
        if (!dev_write(start_lba + inode_start + i, 1, g_sec)) {
            log_line("format FAILED step=inode-area");
            g_mounted = false;
            return -1;
        }
    }

    // 4) 根目录 inode（0 号槽）：目录、无名、size=0
    uint8_t root[VFS64_INODE_BYTES];
    zero_bytes(root, VFS64_INODE_BYTES);
    root[VFS_I_TYPE] = (uint8_t)VFS64_TYPE_DIR;
    wr32(root + VFS_I_PARENT, 0);
    wr32(root + VFS_I_CRC, crc32_64(root, VFS_I_CRC_LEN));
    if (!inode_store(0, root)) {
        log_line("format FAILED step=root-inode");
        g_mounted = false;
        return -1;
    }

    g_mounted = true;
    dbg64_str("[VFS64] format ok blocks=");
    dbg64_dec(total_sectors);
    dbg64_str(" root=");
    dbg64_dec((uint64_t)start_lba + inode_start);   // 根目录 inode 所在扇区的绝对 LBA
    dbg64_nl();
    return 0;
}

// ==================== 挂载 ====================
int vfs64_mount(int drive, uint32_t start_lba) {
    Vfs64Geom saved;
    geom_save(&saved);
    g_mounted = false;
    g_drive = drive;
    g_start = start_lba;

    if (!g_fake_active && !ata_linked()) {
        log_line("no ATA driver linked");
        log_mount_fail("no-device");
        geom_restore(&saved);
        return -1;
    }
    if (!dev_read(start_lba, 1, g_sec)) {
        log_mount_fail("read");
        geom_restore(&saved);
        return -1;
    }
    const int rc = sb_verify(g_sec);
    if (rc != SB_OK) {
        log_mount_fail(sb_reason_str(rc));
        geom_restore(&saved);
        return -1;
    }

    // 校验通过：把几何从超级块搬进内存（搬运时已经全部类型正确，无需再校验范围）
    g_blocks        = rd32(g_sec + VFS_O_TOTAL);
    g_bitmap_start  = rd32(g_sec + VFS_O_BITMAP);
    g_bitmap_blocks = rd32(g_sec + VFS_O_BITMAPN);
    g_inode_start   = rd32(g_sec + VFS_O_INODE);
    g_inode_count   = rd32(g_sec + VFS_O_INODEN);
    g_data_start    = rd32(g_sec + VFS_O_DATA);
    g_data_blocks   = rd32(g_sec + VFS_O_DATAN);
    g_mounted = true;

    uint32_t free_blocks = 0;
    if (!count_free_blocks(&free_blocks)) {
        log_mount_fail("bitmap");
        geom_restore(&saved);
        return -1;
    }
    dbg64_str("[VFS64] mount ok blocks=");
    dbg64_dec(g_blocks);
    dbg64_str(" inodes=");
    dbg64_dec(g_inode_count);
    dbg64_str(" free=");
    dbg64_dec(free_blocks);
    dbg64_nl();
    return 0;
}

// ==================== 读文件 ====================
int vfs64_read(const char* path, void* buf, int max) {
    if (!g_mounted) { log_line("read: not mounted"); return -1; }
    if (!path || !buf || max < 0) { log_line("read: bad args"); return -1; }

    char name[VFS64_NAME_MAX + 1];
    uint32_t nlen = 0;
    bool is_root = false;
    if (path_split(path, name, &nlen, &is_root) != 0) { log_line("read: bad path"); return -1; }
    if (is_root) { log_line("read: path is a directory"); return -1; }

    uint32_t idx = 0;
    if (find_child(0, name, nlen, &idx) != 0) { log_line("read: not found"); return -1; }
    uint8_t ino[VFS64_INODE_BYTES];
    if (!inode_load(idx, ino)) return -1;
    const char* why = "?";
    if (!inode_ok(ino, &why)) {
        dbg64_str("[VFS64] read: bad inode reason=");
        dbg64_str(why);
        dbg64_nl();
        return -1;
    }
    if (ino[VFS_I_TYPE] != VFS64_TYPE_FILE) { log_line("read: not a file"); return -1; }

    const uint32_t size = rd32(ino + VFS_I_SIZE);
    uint32_t left = size;
    if ((uint32_t)max < left) left = (uint32_t)max;
    uint32_t done = 0;
    uint8_t* out = (uint8_t*)buf;
    bool ind_loaded = false;
    for (uint32_t i = 0; left > 0; i++) {
        uint32_t blk = 0;
        if (i < VFS64_DIRECT_BLOCKS) {
            blk = rd32(ino + VFS_I_D0 + 4u * i);
        } else {
            if (!ind_loaded) {
                const uint32_t ind = rd32(ino + VFS_I_IND);
                if (ind == 0) { log_line("read: hole in file (no indirect)"); return -1; }
                if (!load_indirect(ind, g_ptrs)) return -1;
                ind_loaded = true;
            }
            blk = g_ptrs[i - VFS64_DIRECT_BLOCKS];
        }
        if (blk == 0) { log_line("read: hole in file"); return -1; }
        if (!blk_ok_data(blk)) { log_bad_block(blk); return -1; }   // 关键边界检查：数据块必须在数据区
        if (!blk_read(blk, g_sec)) return -1;
        const uint32_t c = (left < VFS64_BLOCK_BYTES) ? left : VFS64_BLOCK_BYTES;
        copy_bytes(out, g_sec, c);
        out += c;
        done += c;
        left -= c;
    }
    return (int)done;
}

// ==================== 写文件 ====================
// 回滚：把本次新分配、但还没提交的块全部标回空闲（inode 槽 0 号是根目录，绝不在这里动）。
static void rollback_new(const uint8_t* ino_new, uint32_t new_ind) {
    for (uint32_t d = 0; d < VFS64_DIRECT_BLOCKS; d++) {
        const uint32_t b = rd32(ino_new + VFS_I_D0 + 4u * d);
        if (b != 0) bitmap_set(b, false);
    }
    for (uint32_t k = 0; k < VFS64_INDIRECT_PTRS; k++) {
        const uint32_t b = rd32(g_ind + 4u * k);
        if (b != 0) bitmap_set(b, false);
    }
    if (new_ind != 0) bitmap_set(new_ind, false);
}

int vfs64_write(const char* path, const void* buf, int len) {
    if (!g_mounted) { log_line("write: not mounted"); return -1; }
    if (!path || !buf || len < 0) { log_line("write: bad args"); return -1; }
    if ((uint32_t)len > VFS64_MAX_FILE_BYTES) { log_line("write: too large"); return -1; }

    char name[VFS64_NAME_MAX + 1];
    uint32_t nlen = 0;
    bool is_root = false;
    if (path_split(path, name, &nlen, &is_root) != 0 || is_root) { log_line("write: bad path"); return -1; }

    uint32_t idx = 0;
    uint8_t old_ino[VFS64_INODE_BYTES];
    const bool exists = (find_child(0, name, nlen, &idx) == 0);
    if (exists) {
        if (!inode_load(idx, old_ino)) return -1;
        const char* why = "?";
        if (!inode_ok(old_ino, &why)) {
            dbg64_str("[VFS64] write: bad inode reason=");
            dbg64_str(why);
            dbg64_nl();
            return -1;
        }
        if (old_ino[VFS_I_TYPE] != VFS64_TYPE_FILE) { log_line("write: path is a directory"); return -1; }
    } else {
        if (!alloc_inode(&idx)) { log_line("write: no free inode"); return -1; }
        zero_bytes(old_ino, VFS64_INODE_BYTES);
    }

    // 新的 inode 镜像（先全部填好，提交前不碰盘；失败时旧 inode 原样保留）
    uint8_t ino_new[VFS64_INODE_BYTES];
    zero_bytes(ino_new, VFS64_INODE_BYTES);
    ino_new[VFS_I_TYPE] = (uint8_t)VFS64_TYPE_FILE;
    ino_new[VFS_I_NAMELEN] = (uint8_t)nlen;
    for (uint32_t i = 0; i < nlen; i++) ino_new[VFS_I_NAME + i] = (uint8_t)name[i];
    wr32(ino_new + VFS_I_PARENT, 0);
    wr32(ino_new + VFS_I_SIZE, (uint32_t)len);
    zero_bytes(g_ind, VFS64_SECTOR_BYTES);

    const uint32_t needed = ((uint32_t)len + VFS64_BLOCK_BYTES - 1u) / VFS64_BLOCK_BYTES;
    uint32_t new_ind = 0;
    if (needed > VFS64_DIRECT_BLOCKS) {
        new_ind = alloc_block();                       // 间接块先分配（指针数组要指向数据块）
        if (new_ind == 0) { log_line("write: no space for indirect block"); return -1; }
    }

    const uint8_t* src = (const uint8_t*)buf;
    for (uint32_t i = 0; i < needed; i++) {
        const uint32_t b = alloc_block();
        if (b == 0) {
            rollback_new(ino_new, new_ind);
            log_line("write: no space (data)");
            return -1;
        }
        if (i < VFS64_DIRECT_BLOCKS) wr32(ino_new + VFS_I_D0 + 4u * i, b);
        else                         wr32(g_ind + 4u * (i - VFS64_DIRECT_BLOCKS), b);

        // 组一个满块（末块补 0，避免把工作缓冲里的旧数据写进去）
        const uint32_t off = i * VFS64_BLOCK_BYTES;
        uint32_t c = (uint32_t)len - off;
        if (c > VFS64_BLOCK_BYTES) c = VFS64_BLOCK_BYTES;
        for (uint32_t j = c; j < VFS64_BLOCK_BYTES; j++) g_sec[j] = 0;
        copy_bytes(g_sec, src + off, c);
        if (!blk_write(b, g_sec)) {
            rollback_new(ino_new, new_ind);
            log_line("write: data block write failed");
            return -1;
        }
    }
    if (new_ind != 0) {
        wr32(ino_new + VFS_I_IND, new_ind);
        if (!blk_write(new_ind, g_ind)) {              // 间接块内容 = 512B 的块号数组
            rollback_new(ino_new, new_ind);
            log_line("write: indirect block write failed");
            return -1;
        }
    }
    wr32(ino_new + VFS_I_CRC, crc32_64(ino_new, VFS_I_CRC_LEN));
    if (!inode_store(idx, ino_new)) {                  // 提交点：新 inode 落盘
        rollback_new(ino_new, new_ind);
        log_line("write: inode commit failed");
        return -1;
    }
    // 旧块在提交之后才释放：中途任何一步失败都只泄漏块，不会让 inode 指向已释放的块
    if (exists) {
        if (!free_file_blocks(old_ino)) log_line("write: WARN old blocks partially freed");
    }
    return len;
}

// ==================== 删除 / 建目录 / 属性 ====================
int vfs64_unlink(const char* path) {
    if (!g_mounted) { log_line("unlink: not mounted"); return -1; }
    if (!path) { log_line("unlink: bad args"); return -1; }

    char name[VFS64_NAME_MAX + 1];
    uint32_t nlen = 0;
    bool is_root = false;
    if (path_split(path, name, &nlen, &is_root) != 0 || is_root) { log_line("unlink: bad path"); return -1; }

    uint32_t idx = 0;
    if (find_child(0, name, nlen, &idx) != 0) { log_line("unlink: not found"); return -1; }
    uint8_t ino[VFS64_INODE_BYTES];
    if (!inode_load(idx, ino)) return -1;
    const char* why = "?";
    if (!inode_ok(ino, &why)) {
        dbg64_str("[VFS64] unlink: bad inode reason=");
        dbg64_str(why);
        dbg64_nl();
        return -1;
    }
    if (ino[VFS_I_TYPE] == VFS64_TYPE_DIR) { log_line("unlink: refuse to remove a directory"); return -1; }
    // inode 里有越界块号时拒绝删除（保持"能删掉的一定是结构自洽的项"）
    if (!free_file_blocks(ino)) { log_line("unlink: corrupt inode (not removed)"); return -1; }

    uint8_t empty[VFS64_INODE_BYTES];
    zero_bytes(empty, VFS64_INODE_BYTES);              // type=0 = 空槽，等价于删掉
    if (!inode_store(idx, empty)) { log_line("unlink: inode clear failed"); return -1; }
    return 0;
}

int vfs64_mkdir(const char* path) {
    if (!g_mounted) { log_line("mkdir: not mounted"); return -1; }
    if (!path) { log_line("mkdir: bad args"); return -1; }

    char name[VFS64_NAME_MAX + 1];
    uint32_t nlen = 0;
    bool is_root = false;
    if (path_split(path, name, &nlen, &is_root) != 0 || is_root) { log_line("mkdir: bad path"); return -1; }
    uint32_t dummy = 0;
    if (find_child(0, name, nlen, &dummy) == 0) { log_line("mkdir: already exists"); return -1; }

    uint32_t idx = 0;
    if (!alloc_inode(&idx)) { log_line("mkdir: no free inode"); return -1; }
    uint8_t ino[VFS64_INODE_BYTES];
    zero_bytes(ino, VFS64_INODE_BYTES);
    ino[VFS_I_TYPE] = (uint8_t)VFS64_TYPE_DIR;
    ino[VFS_I_NAMELEN] = (uint8_t)nlen;
    for (uint32_t i = 0; i < nlen; i++) ino[VFS_I_NAME + i] = (uint8_t)name[i];
    wr32(ino + VFS_I_PARENT, 0);                        // 阶段一：父目录固定是根
    wr32(ino + VFS_I_CRC, crc32_64(ino, VFS_I_CRC_LEN));
    if (!inode_store(idx, ino)) { log_line("mkdir: inode write failed"); return -1; }
    return 0;
}

int vfs64_stat(const char* path, uint32_t* type, uint32_t* size) {
    if (!g_mounted) { log_line("stat: not mounted"); return -1; }
    if (!path) { log_line("stat: bad args"); return -1; }

    char name[VFS64_NAME_MAX + 1];
    uint32_t nlen = 0;
    bool is_root = false;
    if (path_split(path, name, &nlen, &is_root) != 0) { log_line("stat: bad path"); return -1; }
    if (is_root) {
        if (type) *type = VFS64_TYPE_DIR;
        if (size) *size = 0;
        return 0;
    }
    uint32_t idx = 0;
    if (find_child(0, name, nlen, &idx) != 0) { log_line("stat: not found"); return -1; }
    uint8_t ino[VFS64_INODE_BYTES];
    if (!inode_load(idx, ino)) return -1;
    const char* why = "?";
    if (!inode_ok(ino, &why)) {
        dbg64_str("[VFS64] stat: bad inode reason=");
        dbg64_str(why);
        dbg64_nl();
        return -1;
    }
    if (type) *type = ino[VFS_I_TYPE];
    if (size) *size = (ino[VFS_I_TYPE] == VFS64_TYPE_FILE) ? rd32(ino + VFS_I_SIZE) : 0;
    return 0;
}

// ==================== 列举目录 ====================
int vfs64_ls(const char* path, char names[][32], int max, uint32_t* sizes) {
    if (!g_mounted) { log_line("ls: not mounted"); return -1; }
    if (!path || !names || !sizes || max <= 0) { log_line("ls: bad args"); return -1; }

    char name[VFS64_NAME_MAX + 1];
    uint32_t nlen = 0;
    bool is_root = false;
    if (path_split(path, name, &nlen, &is_root) != 0) { log_line("ls: bad path"); return -1; }
    uint32_t dir = 0;
    if (!is_root) {
        uint32_t idx = 0;
        if (find_child(0, name, nlen, &idx) != 0) { log_line("ls: not found"); return -1; }
        uint8_t ino[VFS64_INODE_BYTES];
        if (!inode_load(idx, ino)) return -1;
        const char* why = "?";
        if (!inode_ok(ino, &why) || ino[VFS_I_TYPE] != VFS64_TYPE_DIR) {
            log_line("ls: not a directory");
            return -1;
        }
        dir = idx;
    }

    int count = 0;
    uint8_t ino[VFS64_INODE_BYTES];
    for (uint32_t i = 1; i < g_inode_count; i++) {
        if (!inode_load(i, ino)) return -1;
        if (ino[VFS_I_TYPE] == VFS64_TYPE_FREE) continue;
        if (rd32(ino + VFS_I_PARENT) != dir) continue;
        const char* why = "?";
        if (!inode_ok(ino, &why)) continue;              // 结构坏的项不进结果（find/read 会打点）
        if (count >= max) {
            dbg64_str("[VFS64] ls: truncated at max=");
            dbg64_dec((uint64_t)max);
            dbg64_nl();
            break;
        }
        const uint32_t l = ino[VFS_I_NAMELEN];
        for (uint32_t j = 0; j < l; j++) names[count][j] = (char)ino[VFS_I_NAME + j];
        names[count][l] = 0;                             // l <= 27 < 32，安全
        sizes[count] = (ino[VFS_I_TYPE] == VFS64_TYPE_FILE) ? rd32(ino + VFS_I_SIZE) : 0;
        count++;
    }
    return count;
}

// ==================== 状态打印（验收 grep）====================
void vfs64_dump64() {
    dbg64_str("[VFS64] dump mounted=");
    dbg64_str(g_mounted ? "yes" : "no");
    if (!g_mounted) { dbg64_nl(); return; }
    uint32_t free_blocks = 0;
    count_free_blocks(&free_blocks);
    dbg64_str(" drive=");
    dbg64_dec((uint64_t)g_drive);
    dbg64_str(" start=");
    dbg64_dec(g_start);
    dbg64_str(" blocks=");
    dbg64_dec(g_blocks);
    dbg64_str(" inodes=");
    dbg64_dec(g_inode_count);
    dbg64_str(" data=");
    dbg64_dec(g_data_start);
    dbg64_str(" free=");
    dbg64_dec(free_blocks);
    dbg64_nl();

    char names[8][32];
    uint32_t sizes[8];
    const int n = vfs64_ls("/", names, 8, sizes);
    dbg64_str("[VFS64] dump root entries=");
    if (n < 0) dbg64_str("ERR");
    else dbg64_dec((uint64_t)n);
    dbg64_nl();
    for (int i = 0; i < n && i < 8; i++) {
        uint32_t t = 0;
        uint32_t sz = 0;
        if (vfs64_stat(names[i], &t, &sz) != 0) t = 0;
        dbg64_str("[VFS64] dump   ");
        dbg64_str(names[i]);
        dbg64_str(" type=");
        dbg64_str((t == VFS64_TYPE_DIR) ? "dir" : (t == VFS64_TYPE_FILE) ? "file" : "?");
        dbg64_str(" size=");
        dbg64_dec(sizes[i]);
        dbg64_nl();
    }
}

// ==================== 自检（不需要真盘）====================
static uint8_t g_sel_a[4096];        // 自检读写缓冲（4KB 模式，覆盖直接块 + 间接块）
static uint8_t g_sel_b[4096];

// 在 ls 结果里找名字，返回下标（找不到 -1）
static int sel_find(char names[][32], int n, const char* want) {
    for (int i = 0; i < n; i++) {
        const char* a = names[i];
        const char* b = want;
        while (*a != 0 && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return i;
    }
    return -1;
}

int vfs64_selftest64() {
    int fails = 0;
    Vfs64Geom saved;
    geom_save(&saved);

    // ---- 切到 64 扇区内存假盘（真盘状态在 saved 里，最后恢复）----
    g_fake_active = true;
    zero_bytes(g_fake_disk, (uint32_t)sizeof(g_fake_disk));
    for (uint32_t i = 0; i < (uint32_t)sizeof(g_sel_a); i++) g_sel_a[i] = (uint8_t)((i * 7u + 3u) & 0xFFu);
    zero_bytes(g_sel_b, (uint32_t)sizeof(g_sel_b));

    char names[8][32];
    uint32_t sizes[8];
    uint32_t t = 0;
    uint32_t sz = 0;

    // ---- bit0：格式化 + 挂载 ----
    if (vfs64_format(0, 0, VFS64_FAKE_SECTORS) != 0) {
        fails |= 1;
        log_line("fake-disk format FAIL");
    } else {
        log_line("fake-disk format ok");
        if (vfs64_mount(0, 0) != 0) { fails |= 1; log_line("fake-disk mount FAIL"); }
    }

    // ---- bit1：写 3000B（跨 4 个直接块 + 间接块）-> 读回逐字节比对 ----
    if (!(fails & 1)) {
        const int LEN = 3000;
        if (vfs64_write("/alpha.bin", g_sel_a, LEN) != LEN) {
            fails |= 2;
            log_line("fake-disk write FAIL");
        } else {
            zero_bytes(g_sel_b, (uint32_t)sizeof(g_sel_b));
            const int got = vfs64_read("alpha.bin", g_sel_b, (int)sizeof(g_sel_b));
            if (got != LEN || cmp_bytes(g_sel_a, g_sel_b, (uint32_t)LEN) != 0) {
                fails |= 2;
                log_line("fake-disk write/read FAIL");
            } else {
                dbg64_str("[VFS64] fake-disk write/read ok len=");
                dbg64_dec((uint64_t)LEN);
                dbg64_str(" crc=0x");
                log_hex32(crc32_64(g_sel_a, (uint32_t)LEN));
                dbg64_nl();
            }
        }
    }

    // ---- bit2：目录列举（alpha.bin 3000 / beta.bin 700 / docs 目录）----
    if (!(fails & 1)) {
        const int w2 = vfs64_write("beta.bin", g_sel_a, 700);
        const int m1 = vfs64_mkdir("/docs");
        const int n = vfs64_ls("/", names, 8, sizes);
        bool ok = (w2 == 700) && (m1 == 0) && (n == 3);
        if (ok) {
            const int ia = sel_find(names, n, "alpha.bin");
            const int ib = sel_find(names, n, "beta.bin");
            const int id = sel_find(names, n, "docs");
            ok = (ia >= 0 && sizes[ia] == 3000) &&
                 (ib >= 0 && sizes[ib] == 700) &&
                 (id >= 0 && sizes[id] == 0);
        }
        dbg64_str("[VFS64] fake-disk ls entries=");
        dbg64_dec((uint64_t)((n < 0) ? 0 : n));
        dbg64_nl();
        if (!ok) fails |= 4;
    }

    // ---- bit3：stat（类型 / 大小 / 不存在）----
    if (!(fails & 1)) {
        bool ok = true;
        if (vfs64_stat("alpha.bin", &t, &sz) != 0 || t != VFS64_TYPE_FILE || sz != 3000) ok = false;
        if (vfs64_stat("/beta.bin", &t, &sz) != 0 || t != VFS64_TYPE_FILE || sz != 700) ok = false;
        if (vfs64_stat("docs", &t, &sz) != 0 || t != VFS64_TYPE_DIR || sz != 0) ok = false;
        if (vfs64_stat("/", &t, &sz) != 0 || t != VFS64_TYPE_DIR) ok = false;
        if (vfs64_stat("nope.bin", &t, &sz) != -1) ok = false;
        if (!ok) fails |= 8;
    }

    // ---- bit4：覆盖写 + unlink ----
    if (!(fails & 1)) {
        uint8_t pat[128];
        uint8_t back[128];
        for (uint32_t i = 0; i < (uint32_t)sizeof(pat); i++) {
            pat[i] = (uint8_t)(0xA0u + (i & 0x0Fu));
            back[i] = 0;
        }
        bool ok = (vfs64_write("alpha.bin", pat, (int)sizeof(pat)) == (int)sizeof(pat));
        if (ok) {
            const int got = vfs64_read("alpha.bin", back, (int)sizeof(back));
            if (got != (int)sizeof(pat) || cmp_bytes(pat, back, (uint32_t)sizeof(pat)) != 0) ok = false;
        }
        if (vfs64_unlink("/alpha.bin") != 0) ok = false;
        if (vfs64_stat("alpha.bin", &t, &sz) != -1) ok = false;
        if (vfs64_read("alpha.bin", back, 64) != -1) ok = false;
        if (vfs64_mkdir("beta.bin") == 0) ok = false;          // 与已有文件重名必须被拒
        if (!ok) fails |= 16;
    }

    // ---- bit5：边界与损坏拒绝（越界块号 / 坏 magic / 坏 CRC / 非法入参）----
    if (!(fails & 1)) {
        bool ok = true;
        char longname[40];
        for (int i = 0; i < 30; i++) longname[i] = (char)('a' + (i % 26));
        longname[30] = 0;                                      // 30 > VFS64_NAME_MAX(27)

        if (crc32_64((const uint8_t*)"123456789", 9) != 0xCBF43926u) ok = false;   // CRC 实现漂移
        if (vfs64_mkdir("docs") == 0) ok = false;              // 已存在
        if (vfs64_mkdir("") == 0) ok = false;                  // 空名
        if (vfs64_mkdir("/") == 0) ok = false;                 // 根目录
        if (vfs64_unlink("docs") == 0) ok = false;             // 目录不可删
        if (vfs64_read("docs", g_sel_b, 16) != -1) ok = false; // 读目录
        if (vfs64_ls("beta.bin", names, 4, sizes) != -1) ok = false;   // 对文件 ls
        if (vfs64_ls("/", names, 0, sizes) != -1) ok = false;          // max=0
        if (vfs64_write("x/y", g_sel_a, 8) != -1) ok = false;          // 多级路径（阶段一拒绝）
        if (vfs64_write(longname, g_sel_a, 8) != -1) ok = false;       // 名字过长
        if (vfs64_write("huge.bin", g_sel_a, (int)(VFS64_MAX_FILE_BYTES + 1)) != -1) ok = false;
        if (vfs64_stat(nullptr, &t, &sz) != -1) ok = false;
        if (vfs64_read(nullptr, g_sel_b, 8) != -1) ok = false;
        if (vfs64_write("beta.bin", nullptr, 8) != -1) ok = false;

        // 坏超级块 1：magic 被改 -> mount 必须失败；改回去必须又能挂
        if (dev_read(g_start, 1, g_sel_b)) {
            g_sel_b[VFS_O_MAGIC] ^= 0x01;
            if (dev_write(g_start, 1, g_sel_b)) {
                if (vfs64_mount(0, 0) == 0) ok = false;
                g_sel_b[VFS_O_MAGIC] ^= 0x01;
                if (!dev_write(g_start, 1, g_sel_b)) ok = false;
            } else ok = false;
        } else ok = false;
        // 坏超级块 2：改保留字段（布局重算查不到它，只有 CRC 能发现）
        if (dev_read(g_start, 1, g_sel_b)) {
            g_sel_b[VFS_O_FLAGS] ^= 0xFF;
            if (dev_write(g_start, 1, g_sel_b)) {
                if (vfs64_mount(0, 0) == 0) ok = false;
                g_sel_b[VFS_O_FLAGS] ^= 0xFF;
                if (!dev_write(g_start, 1, g_sel_b)) ok = false;
            } else ok = false;
        } else ok = false;
        if (vfs64_mount(0, 0) != 0) ok = false;

        // 伪造"数据块指针越界"的 inode（槽 1，CRC 是对的）：read 必须拒绝，绝不越界读
        if (dev_read(g_start + g_inode_start, 1, g_sel_b)) {
            uint8_t* bad = g_sel_b + VFS64_INODE_BYTES;
            zero_bytes(bad, VFS64_INODE_BYTES);
            bad[VFS_I_TYPE] = (uint8_t)VFS64_TYPE_FILE;
            bad[VFS_I_NAMELEN] = 4;
            for (uint32_t i = 0; i < 4; i++) bad[VFS_I_NAME + i] = (uint8_t)("hack"[i]);
            wr32(bad + VFS_I_SIZE, 512);
            wr32(bad + VFS_I_D0, g_blocks + 7);                // 越界块号
            wr32(bad + VFS_I_CRC, crc32_64(bad, VFS_I_CRC_LEN));
            if (dev_write(g_start + g_inode_start, 1, g_sel_b)) {
                if (vfs64_read("hack", g_sel_b, 16) != -1) ok = false;
                if (vfs64_stat("hack", &t, &sz) != 0) ok = false;   // inode 本身结构是合法的
                zero_bytes(g_sel_b, VFS64_SECTOR_BYTES);
                if (!dev_write(g_start + g_inode_start, 1, g_sel_b)) ok = false;   // 清掉伪造项
            } else ok = false;
        } else ok = false;

        // 伪造"inode CRC 不对"：stat/read 必须拒绝
        if (dev_read(g_start + g_inode_start, 1, g_sel_b)) {
            uint8_t* bad = g_sel_b + VFS64_INODE_BYTES;
            zero_bytes(bad, VFS64_INODE_BYTES);
            bad[VFS_I_TYPE] = (uint8_t)VFS64_TYPE_FILE;
            bad[VFS_I_NAMELEN] = 5;
            for (uint32_t i = 0; i < 5; i++) bad[VFS_I_NAME + i] = (uint8_t)("hack2"[i]);
            wr32(bad + VFS_I_SIZE, 128);
            wr32(bad + VFS_I_CRC, 0x12345678u);                // 故意写错
            if (dev_write(g_start + g_inode_start, 1, g_sel_b)) {
                if (vfs64_stat("hack2", &t, &sz) == 0) ok = false;
                zero_bytes(g_sel_b, VFS64_SECTOR_BYTES);
                if (!dev_write(g_start + g_inode_start, 1, g_sel_b)) ok = false;
            } else ok = false;
        } else ok = false;

        // 分区太小：format 必须直接拒绝（且不能改坏当前卷）
        if (vfs64_format(0, 0, VFS64_MIN_BLOCKS - 1u) == 0) ok = false;
        if (vfs64_mount(0, 0) != 0) ok = false;                // 当前卷还在
        if (!ok) fails |= 32;
    }

    // ---- bit6：空间耗尽 + 删除后回收（假盘数据区只有几十块，很快写满）----
    if (!(fails & 1)) {
        char fname[16];
        int ok_files = 0;
        bool full = false;
        for (uint32_t i = 0; i < 16; i++) {
            fname[0] = 'f';
            fname[1] = (char)('0' + (int)(i / 10));
            fname[2] = (char)('0' + (int)(i % 10));
            fname[3] = '.';
            fname[4] = 'b';
            fname[5] = 'i';
            fname[6] = 'n';
            fname[7] = 0;
            if (vfs64_write(fname, g_sel_a, (int)sizeof(g_sel_a)) == (int)sizeof(g_sel_a)) ok_files++;
            else { full = true; break; }
        }
        bool ok = (ok_files >= 1) && full;
        if (ok) {
            // 最后一个写成功的文件必须能完整读回（证明"写满"的过程没有写坏已提交数据）
            const int last = ok_files - 1;
            fname[0] = 'f';
            fname[1] = (char)('0' + (last / 10));
            fname[2] = (char)('0' + (last % 10));
            fname[3] = '.';
            fname[4] = 'b';
            fname[5] = 'i';
            fname[6] = 'n';
            fname[7] = 0;
            zero_bytes(g_sel_b, (uint32_t)sizeof(g_sel_b));
            const int got = vfs64_read(fname, g_sel_b, (int)sizeof(g_sel_b));
            if (got != (int)sizeof(g_sel_a) || cmp_bytes(g_sel_a, g_sel_b, (uint32_t)sizeof(g_sel_a)) != 0) ok = false;
            // 删掉它 -> 位图回收，必须又能写一个新文件
            if (vfs64_unlink(fname) != 0) ok = false;
            char gname[8] = { 'g','0','0','.','b','i','n', 0 };
            if (vfs64_write(gname, g_sel_a, 512) != 512) ok = false;
            if (vfs64_unlink(gname) != 0) ok = false;
        }
        if (!ok) fails |= 64;
    }

    // ---- 恢复真盘状态 ----
    g_fake_active = false;
    geom_restore(&saved);

    // ---- bit7：真盘只读探测（绝不在这里格式化/写盘）----
    if (!ata_linked()) {
        log_line("real-disk tests skipped (no ATA)");
    } else if (!g_mounted) {
        log_line("real-disk mount probe skipped (no mounted fs)");
    } else {
        const int n = vfs64_ls("/", names, 8, sizes);
        const int s = vfs64_stat("/", &t, &sz);
        if (n < 0 || s != 0 || t != VFS64_TYPE_DIR) {
            fails |= 128;
            log_line("real-disk probe FAIL");
        } else {
            dbg64_str("[VFS64] real-disk probe ok entries=");
            dbg64_dec((uint64_t)n);
            dbg64_nl();
        }
    }

    dbg64_str("[VFS64] selftest ");
    if (fails == 0) {
        dbg64_str("PASS");
        dbg64_nl();
    } else {
        dbg64_str("FAIL mask=");
        dbg64_dec((uint64_t)fails);
        dbg64_nl();
    }
    return fails;
}
