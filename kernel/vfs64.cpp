// vfs64.cpp - VimtuFS2 实现（v3：真正的目录树 + inode 时间戳 + 类型判定；v2 旧卷仍可挂载）
//
// 磁盘布局（相对分区起始 LBA，块号从 0 开始；1 块 = 1 扇区 = 512B）：
//   块 0              : 超级块（见下面 VFS_O_* 偏移；末尾 0xAA55；CRC32 自校验）
//   块 1 .. 1+bmn-1   : 空闲块位图（1 = 已用，额外把"分区外"的位也置 1，分配器永不发放）
//   块 bp .. bp+ibn-1 : inode 表（v3 = 128B/个 4 个/块、v2 = 64B/个 8 个/块；inode 0 = 根目录）
//   块 dp .. total-1  : 数据区（文件内容 + 一级间接块，文件最大 = 4*512 + 128*512 = 67584B）
//   bmn = ceil(total/4096)、ibn = ceil(inodes/每块 inode 数)、bp = 1+bmn、dp = bp+ibn
//
// 设计取舍（**目录树**）：
//   名字放在 inode 里（见 vfs64.h 的"目录表示"），所以"目录"没有数据块 ——
//   一个目录 = "父 inode 号相同的 inode 列表"。好处：少一层指针、少一类越界、格式最简单；
//   代价：条目上限 = inode 数（512）、查名字/列目录是 O(inode 数) 线性扫描、没有硬链接。
//   多级路径靠 path_resolve 逐段解析（'.'/'..' 都实现），所有索引先范围校验再用。
//
// 健壮性约定（所有磁盘访问都走这里的三条纪律）：
//   1) 任何盘上结构都先读到静态缓冲（g_sec / g_ino_cache / g_ind / 局部 128B inode 缓冲）再解析，
//      绝不把盘上字节直接当结构体指针用（避免未对齐/越界读）；
//   2) 每个结构先校验（超级块 magic/版本/CRC/几何重算；inode 类型/名字长度/CRC/大小/保留区；
//      块号必须落在数据区内；inode 号必须 < inode 数），不合法一律 -1 + 打点；
//   3) 写文件时"先分配新块 + 写完数据 + 提交 inode，最后才释放旧块"—— 中途失败只
//      可能泄漏几个块，绝不会让 inode 指向已被释放（可能被别人复用）的块。
//
// ATA 用 __attribute__((weak)) 引用（签名与 kernel/ata64.h 逐字一致，符号是 C++ 名）：
//   安装介质内核链接 ata64.o -> 正常读写真盘；自检里的 64 扇区假盘只是为了不依赖真盘。

#include "vfs64.h"
#include "debug64.h"
#include "ata64.h"
#include "x86_64.h"          // rtc_get_date64 / rtc_get_time64（mtime 用）
#include "part64.h"          // 只读常量（PART_MAIN_LBA / PartInfo）；**不调用** part64 的函数

__attribute__((weak)) bool ata64_read (int drive, uint32_t lba, uint32_t count, void* buf);
__attribute__((weak)) bool ata64_write(int drive, uint32_t lba, uint32_t count, const void* buf);

// ==================== 超级块字段偏移（扇区 0，全部小端；与 v2 完全一致）====================
static const uint32_t VFS_O_MAGIC    = 0;    // 8B  "VIMTUFS2"
static const uint32_t VFS_O_VERSION  = 8;    // u32 结构版本（2 = 旧卷，3 = 目录树）
static const uint32_t VFS_O_SECTOR   = 12;   // u32 扇区大小 = 512
static const uint32_t VFS_O_BLOCK    = 16;   // u32 块大小 = 512
static const uint32_t VFS_O_TOTAL    = 20;   // u32 本卷总块数（= 分区扇区数）
static const uint32_t VFS_O_ROOT     = 24;   // u32 根目录 inode 号（固定 0）
static const uint32_t VFS_O_BITMAP   = 28;   // u32 位图起始块
static const uint32_t VFS_O_BITMAPN  = 32;   // u32 位图块数
static const uint32_t VFS_O_INODE    = 36;   // u32 inode 区起始块
static const uint32_t VFS_O_INODEN   = 40;   // u32 inode 个数
static const uint32_t VFS_O_INOSZ    = 44;   // u32 单个 inode 字节数（v2 = 64、v3 = 128）
static const uint32_t VFS_O_DATA     = 48;   // u32 数据区起始块
static const uint32_t VFS_O_DATAN    = 52;   // u32 数据区块数
static const uint32_t VFS_O_FLAGS    = 56;   // u32 保留（0；改它就等于改 CRC 覆盖区）
static const uint32_t VFS_O_CRC      = 60;   // u32 超级块 CRC32（覆盖 [0,60)）
static const uint32_t VFS_O_SIG      = 510;  // u16 0xAA55
static const uint32_t VFS_SB_CRC_LEN = 60;

// ==================== inode 字段偏移 ====================
// **两个版本共用的前缀**（0..31 字节完全同布局）：
static const uint32_t VFS_I_TYPE    = 0;     // u8  0=空 1=文件 2=目录
static const uint32_t VFS_I_NAMELEN = 1;     // u8  名字长度（v2 ≤27 / v3 ≤31；0 仅用于根目录）
static const uint32_t VFS_I_RSVD    = 2;     // u16 保留（0）
static const uint32_t VFS_I_SIZE    = 4;     // u32 文件字节数
static const uint32_t VFS_I_D0      = 8;     // u32 直接块 0..3（偏移 8/12/16/20）
static const uint32_t VFS_I_IND     = 24;    // u32 一级间接块（128 个块号）
static const uint32_t VFS_I_PARENT  = 28;    // u32 父目录 inode 号（根 = 自己 = 0）
// v3 追加字段（v2 里 32..59 是名字、60 是 CRC）：
static const uint32_t VFS_I3_MTIME  = 32;    // u32 打包时间戳
static const uint32_t VFS_I3_NLINK  = 36;    // u16 链接数
static const uint32_t VFS_I3_KIND   = 38;    // u8  类型判定缓存
static const uint32_t VFS_I3_RSVD2  = 39;    // u8  保留（0）
static const uint32_t VFS_I3_NAME   = 40;    // 31B 名字
static const uint32_t VFS_I3_RSVD3  = 71;    // 保留区 [71,124) 必须全 0
// v2 的名字/CRC：
static const uint32_t VFS_I2_NAME   = 32;    // 28B 名字（只用前 27）
static const uint32_t VFS_I2_CRC    = 60;    // u32 inode CRC32（覆盖 [0,60)）

static const uint32_t VFS64_INODE_BYTES_MAX = 128u;

// ==================== 版本 -> inode 布局（**唯一定义点**：这一张表 + vfs64.h 的文件头）====================
struct Vfs64Layout {
    uint32_t version;          // 卷版本
    uint32_t inode_bytes;      // 记录大小
    uint32_t inodes_per_blk;   // 每块几个
    uint32_t name_off;         // 名字起始偏移
    uint32_t name_max;         // 名字最大字节数
    uint32_t mtime_off;        // mtime 偏移（0 = 该版本没有这个字段）
    uint32_t nlink_off;        // nlink 偏移（0 = 没有）
    uint32_t kind_off;         // kind 偏移（0 = 没有）
    uint32_t crc_off;          // inode CRC 偏移
};
static constexpr Vfs64Layout VFS_LAY_V2 = { VFS64_VERSION_V2, 64u,  8u,
                                            VFS_I2_NAME, VFS64_NAME_MAX_V2, 0u, 0u, 0u, VFS_I2_CRC };
static constexpr Vfs64Layout VFS_LAY_V3 = { VFS64_VERSION,    128u, 4u,
                                            VFS_I3_NAME, VFS64_NAME_MAX, VFS_I3_MTIME, VFS_I3_NLINK,
                                            VFS_I3_KIND, 124u };

static_assert(VFS64_BLOCK_BYTES == VFS64_SECTOR_BYTES, "块就是扇区（vfs64_format 也按这个算几何）");
static_assert(VFS64_INODES_PER_BLK * VFS64_INODE_BYTES == VFS64_BLOCK_BYTES, "v3：每块必须正好 4 个 inode");
static_assert(VFS64_INODES_PER_BLK_V2 * VFS64_INODE_BYTES_V2 == VFS64_BLOCK_BYTES, "v2：每块必须正好 8 个 inode");
static_assert(VFS64_BITMAP_BLK_BITS == VFS64_BLOCK_BYTES * 8u, "位图块 512B = 4096 个块位");
static_assert(VFS_I3_RSVD3 + 1u <= 124u, "v3 inode 保留区必须在 CRC 之前");
static_assert(VFS_LAY_V3.crc_off + 4u == 128u, "v3 inode 字段必须正好铺满 128B");
static_assert(VFS_LAY_V2.crc_off + 4u == 64u, "v2 inode 字段必须正好铺满 64B");
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
static bool str_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}
static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
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

// ==================== ★ 多卷：卷槽表 + "当前卷"====================
// 为什么长这样（设计取舍写清）：
//   * 这份实现原本是**单卷**的：超级块几何放在一组全局变量里，vfs64_mount/format 直接改它们。
//     多卷需求（文件管理器里 C:/D:/E: 都能点进去）最省痛的做法不是把 2000 行里的 g_drive/g_start…
//     全都加一遍参数（那样每一处都可能漏改），而是：
//       1) 把几何收进 Vfs64Geom，卷槽表 g_vol[VFS64_SLOT_MAX] **每槽一份**；
//       2) g_cur_slot 指向"当前卷"，下面用宏把原来的全局名映射成 g_vol[g_cur_slot] 的字段 ——
//          于是既有代码（路径解析/inode/位图/读写/遍历）**逐字不动**地作用于当前卷；
//       3) 切卷 = 改 g_cur_slot（外加 inode 缓存键控），没有任何数据搬运。
//   * **inode 扇区缓存按 (slot, drive, lba) 三元组键控**（g_ino_cache_slot/drive/lba）：
//     这是切卷最容易出的 bug —— 缓存只有 LBA 而没有卷身份时，切到另一块盘的同号 LBA 会
//     把上一个卷的 inode 字节当本卷的用。三元组键控 + 任何写盘都失效（dev_write）双保险。
//   * 系统卷槽 g_system_slot 由 vfs64_mount_system64 / vfs64_format 记录；store64/config64/
//     update64/app64/elf64/proc64/sysstate64 一律用 vfs64_*_on64(vfs64_system_slot64(), …) ——
//     "当前卷"只在那一次调用期间被临时换掉，返回前**原样切回**（LIFO 守卫）。所以无论用户正在
//     浏览 D: 还是 E:，3 秒自动落盘/关机落盘都只写系统卷 C:。
struct Vfs64Geom {
    bool     mounted;
    int      drive;
    uint32_t start, blocks, bitmap_start, bitmap_blocks;
    uint32_t inode_start, inode_count, data_start, data_blocks;
    const Vfs64Layout* lay;
};

static uint8_t  g_sec[VFS64_SECTOR_BYTES];            // 唯一工作扇区（先读进来再解析）
static uint8_t  g_ind[VFS64_SECTOR_BYTES];            // 待写出的间接块镜像
static uint32_t g_ptrs[VFS64_INDIRECT_PTRS];          // 释放文件时搬运的 128 个块号
static uint8_t  g_ino_cache[VFS64_SECTOR_BYTES];      // inode 表"当前扇区"缓存（扫描时少读盘）
static int      g_ino_cache_slot  = -1;               // ★ 缓存键 = (slot, drive, lba)
static int      g_ino_cache_drive = -1;
static uint32_t g_ino_cache_lba   = 0xFFFFFFFFu;
static bool     g_ino_cache_valid = false;

static Vfs64Geom g_vol[VFS64_SLOT_MAX];               // 卷槽表（0 号槽 = 系统卷）
static int       g_cur_slot      = 0;                 // 当前卷槽（恒为合法下标；mounted=false 表示没挂卷）
static int       g_system_slot   = -1;                // 系统卷槽（C:）
static int       g_vol_switch_depth = 0;              // 临时切卷守卫的嵌套计数（诊断用；0 = 不在按槽调用里）

// 既有代码零改动的关键：这些名字原来是全局变量，现在是"当前卷"的字段别名。
#define g_mounted      (g_vol[g_cur_slot].mounted)
#define g_drive        (g_vol[g_cur_slot].drive)
#define g_start        (g_vol[g_cur_slot].start)
#define g_blocks       (g_vol[g_cur_slot].blocks)
#define g_bitmap_start (g_vol[g_cur_slot].bitmap_start)
#define g_bitmap_blocks (g_vol[g_cur_slot].bitmap_blocks)
#define g_inode_start  (g_vol[g_cur_slot].inode_start)
#define g_inode_count  (g_vol[g_cur_slot].inode_count)
#define g_data_start   (g_vol[g_cur_slot].data_start)
#define g_data_blocks  (g_vol[g_cur_slot].data_blocks)
#define g_lay          (g_vol[g_cur_slot].lay)

// inode 缓存失效（写盘、重挂载时调用；宁可多读，绝不给旧字节）
static void ino_cache_invalidate() {
    g_ino_cache_valid = false;
    g_ino_cache_slot = -1;
    g_ino_cache_drive = -1;
    g_ino_cache_lba = 0xFFFFFFFFu;
}
// 缓存命中判定：**卷身份**（slot/drive/lba）三项全对才算命中
static bool ino_cache_hit(int drive, uint32_t lba) {
    return g_ino_cache_valid && g_ino_cache_slot == g_cur_slot &&
           g_ino_cache_drive == drive && g_ino_cache_lba == lba;
}

// 挂载状态的整份快照（mount 失败时要恢复回去，别把已挂载的卷弄丢）
static void geom_save(Vfs64Geom* g) {
    *g = g_vol[g_cur_slot];
}
static void geom_restore(const Vfs64Geom* g) {
    g_vol[g_cur_slot] = *g;
    ino_cache_invalidate();
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
// 坏路径打点：说明原因 + 路径前 40 字节（不整条打，避免超长行）
static void log_path_bad(const char* why, const char* path) {
    char b[41];
    uint32_t i = 0;
    if (path) { for (; path[i] && i < 40u; i++) b[i] = path[i]; }
    b[i] = 0;
    dbg64_str("[VFS64] path ");
    dbg64_str(why);
    dbg64_str(" path=");
    dbg64_str(b);
    dbg64_nl();
}
static void log_op_fail(const char* op, const char* why) {
    dbg64_str("[VFS64] ");
    dbg64_str(op);
    dbg64_str(": ");
    dbg64_str(why);
    dbg64_nl();
}

// ==================== 设备层：假盘 / 弱 ATA ====================
static const uint32_t VFS64_FAKE_SECTORS = 64;                          // 自检假盘：32KB
static uint8_t  g_fake_disk[VFS64_FAKE_SECTORS * VFS64_SECTOR_BYTES];
static bool     g_fake_active = false;

static bool ata_linked() {
    return (ata64_read != nullptr) && (ata64_write != nullptr);
}

// 绝对 LBA 读（count 个扇区）**指定驱动器号**；count 上限 255 由这里分块，调用方不用管。
// 为什么有 _at 版本：drive64 要枚举**别的盘**，不能动 g_drive（挂载状态属于别人）。
static bool dev_read_at(int drive, uint32_t abs_lba, uint32_t count, void* buf) {
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
        if (!ata64_read(drive, abs_lba, c, d)) {
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
static bool dev_write_at(int drive, uint32_t abs_lba, uint32_t count, const void* buf) {
    if (count == 0) return true;
    ino_cache_invalidate();             // 任何写盘都让 inode 扇区缓存失效（宁可多读，绝不给旧字节）
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
        if (!ata64_write(drive, abs_lba, c, s)) {
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
static bool dev_read(uint32_t abs_lba, uint32_t count, void* buf) {
    return dev_read_at(g_drive, abs_lba, count, buf);
}
static bool dev_write(uint32_t abs_lba, uint32_t count, const void* buf) {
    return dev_write_at(g_drive, abs_lba, count, buf);
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

struct Vfs64SbGeo {
    const Vfs64Layout* lay;
    uint32_t version;
    uint32_t blocks, root, bitmap_start, bitmap_blocks;
    uint32_t inode_start, inode_count, data_start, data_blocks;
};

// 读盘内容来自调用方给的缓冲。magic -> CRC -> 版本 -> 几何重算，逐项都要过。
// 版本 2 与 3 都接受（v2 旧卷继续能挂：只是 inode 布局不同、没有 mtime/kind）。
static int sb_verify(const uint8_t* sb, Vfs64SbGeo* geo) {
    static const char magic[8] = { 'V','I','M','T','U','F','S','2' };
    if (cmp_bytes(sb + VFS_O_MAGIC, magic, 8) != 0) return SB_ERR_MAGIC;
    if (rd32(sb + VFS_O_CRC) != crc32_64(sb, VFS_SB_CRC_LEN)) return SB_ERR_CRC;
    const uint32_t ver = rd32(sb + VFS_O_VERSION);
    const Vfs64Layout* lay = nullptr;
    if (ver == VFS64_VERSION)         lay = &VFS_LAY_V3;
    else if (ver == VFS64_VERSION_V2) lay = &VFS_LAY_V2;
    else                              return SB_ERR_VERSION;
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

    if (sector != VFS64_SECTOR_BYTES || block != VFS64_BLOCK_BYTES || inosz != lay->inode_bytes)
        return SB_ERR_LAYOUT;
    if (total < VFS64_MIN_BLOCKS) return SB_ERR_LAYOUT;
    if (inon == 0 || inon > VFS64_MAX_INODES) return SB_ERR_LAYOUT;
    if (root >= inon) return SB_ERR_LAYOUT;

    // 布局必须"严丝合缝"：位图紧跟超级块，inode 区紧跟位图，数据区紧跟 inode 区
    const uint32_t bmn_calc   = (total + VFS64_BITMAP_BLK_BITS - 1u) / VFS64_BITMAP_BLK_BITS;
    const uint32_t ino_blocks = (inon + lay->inodes_per_blk - 1u) / lay->inodes_per_blk;
    if (bm != 1u) return SB_ERR_LAYOUT;
    if (bmn != bmn_calc) return SB_ERR_LAYOUT;
    if (ino != bm + bmn) return SB_ERR_LAYOUT;
    if (data != ino + ino_blocks) return SB_ERR_LAYOUT;
    if (data >= total) return SB_ERR_LAYOUT;
    if (datan != total - data) return SB_ERR_LAYOUT;

    if (geo) {
        geo->lay = lay;              geo->version = ver;
        geo->blocks = total;         geo->root = root;
        geo->bitmap_start = bm;      geo->bitmap_blocks = bmn;
        geo->inode_start = ino;      geo->inode_count = inon;
        geo->data_start = data;      geo->data_blocks = datan;
    }
    return SB_OK;
}

// 统计数据区里还空着多少块（挂载/探测打点用）。base_lba = 卷起始绝对 LBA，位图必须能全部读出。
static bool count_free_at(int drive, uint32_t base_lba, uint32_t bitmap_start, uint32_t bitmap_blocks,
                          uint32_t blocks, uint32_t data_start, uint32_t* out) {
    uint32_t free_n = 0;
    for (uint32_t m = 0; m < bitmap_blocks; m++) {
        if (!dev_read_at(drive, base_lba + bitmap_start + m, 1, g_sec)) return false;
        for (uint32_t k = 0; k < VFS64_BITMAP_BLK_BITS; k++) {
            const uint32_t blk = m * VFS64_BITMAP_BLK_BITS + k;
            if (blk < data_start || blk >= blocks) continue;
            if ((g_sec[k >> 3] & (uint8_t)(1u << (k & 7u))) == 0) free_n++;
        }
    }
    *out = free_n;
    return true;
}
// 已在挂载状态里的卷：等价于用当前几何调用上面那个
static bool count_free_blocks(uint32_t* out) {
    return count_free_at(g_drive, g_start, g_bitmap_start, g_bitmap_blocks, g_blocks, g_data_start, out);
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
// inode 记录永远落在**单个扇区**内（64/128 都能整除 512），所以下面这条断言成立：
static bool inode_slot(uint32_t idx, uint32_t* out_blk, uint32_t* out_off) {
    if (idx >= g_inode_count) { log_bad_inode(idx); return false; }
    const uint32_t ib = g_lay->inode_bytes;
    const uint32_t byte_off = idx * ib;
    *out_blk = g_inode_start + byte_off / VFS64_BLOCK_BYTES;
    *out_off = byte_off % VFS64_BLOCK_BYTES;
    if (*out_off + ib > VFS64_BLOCK_BYTES) {         // 理论不可达（布局表已保证）；防御性拒绝
        log_line("inode slot crosses block boundary (layout bug)");
        return false;
    }
    return true;
}
// inode 扇区缓存：**键 = (slot, drive, lba)** —— 切卷/换盘后即使 LBA 相同也一定不命中，
// 绝不会把上一个卷的 inode 字节当本卷的用（多卷里最容易出的 bug，见文件头"多卷"一节）。
static bool inode_load(uint32_t idx, uint8_t* out) {
    uint32_t blk = 0, off = 0;
    if (!inode_slot(idx, &blk, &off)) return false;
    const uint32_t lba = g_start + blk;
    if (!ino_cache_hit(g_drive, lba)) {
        if (!dev_read(lba, 1, g_ino_cache)) { ino_cache_invalidate(); return false; }
        g_ino_cache_slot = g_cur_slot;               // 记下**卷身份**，不只是 LBA
        g_ino_cache_drive = g_drive;
        g_ino_cache_lba = lba;
        g_ino_cache_valid = true;
    }
    copy_bytes(out, g_ino_cache + off, g_lay->inode_bytes);
    return true;
}
static bool inode_store(uint32_t idx, const uint8_t* in) {
    uint32_t blk = 0, off = 0;
    if (!inode_slot(idx, &blk, &off)) return false;
    const uint32_t lba = g_start + blk;
    if (!ino_cache_hit(g_drive, lba)) {
        if (!dev_read(lba, 1, g_ino_cache)) { ino_cache_invalidate(); return false; }
        g_ino_cache_slot = g_cur_slot;
        g_ino_cache_drive = g_drive;
        g_ino_cache_lba = lba;
        g_ino_cache_valid = true;
    }
    copy_bytes(g_ino_cache + off, in, g_lay->inode_bytes);
    if (!dev_write(lba, 1, g_ino_cache)) { ino_cache_invalidate(); return false; }
    ino_cache_invalidate();                           // 写后失效：以后要用就重读
    return true;
}
// 结构合法性：类型/名字长度/保留字段/CRC/大小/（v3）时间与保留区。
// 空槽（type=0）不校验 CRC —— 格式化后它就是全 0。
static bool inode_ok(const uint8_t* b, const char** why) {
    const uint8_t t = b[VFS_I_TYPE];
    if (t > VFS64_TYPE_DIR) { *why = "type"; return false; }
    if (b[VFS_I_NAMELEN] > g_lay->name_max) { *why = "namelen"; return false; }
    if (rd16(b + VFS_I_RSVD) != 0) { *why = "reserved"; return false; }
    if (t == VFS64_TYPE_FREE) return true;
    if (rd32(b + g_lay->crc_off) != crc32_64(b, g_lay->crc_off)) { *why = "crc"; return false; }
    if (t == VFS64_TYPE_FILE && rd32(b + VFS_I_SIZE) > VFS64_MAX_FILE_BYTES) { *why = "size"; return false; }
    if (g_lay->version == VFS64_VERSION) {
        if (b[VFS_I3_RSVD2] != 0) { *why = "rsvd2"; return false; }
        for (uint32_t i = VFS_I3_RSVD3; i < g_lay->crc_off; i++)
            if (b[i] != 0) { *why = "rsvd3"; return false; }
        for (uint32_t i = VFS_I3_NAME + g_lay->name_max; i < VFS_I3_RSVD3; i++)
            if (b[i] != 0) { *why = "name pad"; return false; }
        if (b[VFS_I3_KIND] > VFS64_KIND_MAX) { *why = "kind"; return false; }
        if (rd16(b + VFS_I3_NLINK) == 0) { *why = "nlink"; return false; }
        const uint32_t mt = rd32(b + VFS_I3_MTIME);
        if (mt != 0) {
            const uint32_t mo = (mt >> 22) & 0xFu;
            const uint32_t dy = (mt >> 17) & 0x1Fu;
            const uint32_t hh = (mt >> 12) & 0x1Fu;
            const uint32_t mi = (mt >> 6)  & 0x3Fu;
            const uint32_t ss =  mt        & 0x3Fu;
            if (mo < 1u || mo > 12u || dy < 1u || dy > 31u || hh > 23u || mi > 59u || ss > 59u) {
                *why = "mtime"; return false;
            }
        }
        if (t == VFS64_TYPE_DIR) {                    // 目录不许有数据块（也没有 size）
            if (rd32(b + VFS_I_SIZE) != 0) { *why = "dir size"; return false; }
            for (uint32_t d = 0; d < VFS64_DIRECT_BLOCKS; d++)
                if (rd32(b + VFS_I_D0 + 4u * d) != 0) { *why = "dir ptr"; return false; }
            if (rd32(b + VFS_I_IND) != 0) { *why = "dir ind"; return false; }
        }
    }
    return true;
}
// 找一个空 inode 槽（跳过 0 号根目录）
static bool alloc_inode(uint32_t* out_idx) {
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    for (uint32_t i = 1; i < g_inode_count; i++) {
        if (!inode_load(i, ino)) return false;
        if (ino[VFS_I_TYPE] == VFS64_TYPE_FREE) { *out_idx = i; return true; }
    }
    return false;
}
// 读出一级间接块里的 128 个块号（不解析指针内容，只搬运到局部缓冲）
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

// ==================== 名字 / 路径（v3：多级 + '.'/'..'）====================
static bool name_valid(const char* name, uint32_t len) {
    if (len == 0 || len > g_lay->name_max) return false;
    if (len == 1 && name[0] == '.') return false;
    if (len == 2 && name[0] == '.' && name[1] == '.') return false;
    for (uint32_t i = 0; i < len; i++) {
        const char c = name[i];
        if (c < 0x21 || c > 0x7E || c == '/') return false;   // 可打印 ASCII，'/' 是路径分隔符
    }
    return true;
}
// 在目录 dir 里按名字找子项。返回 0 = 找到（*out_idx），-1 = 没找到/读盘失败。
static int find_child(uint32_t dir, const char* name, uint32_t nlen, uint32_t* out_idx) {
    if (nlen == 0 || nlen > g_lay->name_max) return -1;
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    for (uint32_t i = 1; i < g_inode_count; i++) {
        if (!inode_load(i, ino)) return -1;
        if (ino[VFS_I_TYPE] == VFS64_TYPE_FREE) continue;
        if (rd32(ino + VFS_I_PARENT) != dir) continue;
        if (ino[VFS_I_NAMELEN] != nlen) continue;
        if (cmp_bytes(ino + g_lay->name_off, name, nlen) != 0) continue;
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
// 路径解析（**多级**）。语义见 vfs64.h：".." 在根目录仍是根（POSIX）；大小写敏感。
// 返回：0  = 解析成功，*out_ino 是终点 inode；
//       1  = **最后一段不存在**，但父目录有效（此时 *out_parent/*out_name/*out_nlen 已填）
//            —— 只有 want_parent=true 时才会返回 1；
//      -1  = 非法路径 / 中间分量不存在 / 读盘失败（已打点）。
static int path_resolve(const char* path, bool want_parent,
                        uint32_t* out_ino, uint32_t* out_parent, char* out_name, uint32_t* out_nlen) {
    if (!path) { log_path_bad("null", path); return -1; }
    uint32_t total = 0;
    while (path[total] != 0) total++;
    if (total > VFS64_PATH_MAX) { log_path_bad("too long", path); return -1; }

    uint32_t cur = 0;                                   // 根目录
    uint32_t depth = 0;
    const char* p = path;
    for (;;) {
        while (*p == '/') p++;                          // 冗余分隔符等价于一个
        if (*p == 0) break;                             // 路径结束 -> cur 就是终点
        char seg[VFS64_NAME_MAX + 1];
        uint32_t n = 0;
        while (p[n] != 0 && p[n] != '/') {
            if (n >= g_lay->name_max) { log_path_bad("segment too long", path); return -1; }
            seg[n] = p[n];
            n++;
        }
        seg[n] = 0;
        const bool at_end = (p[n] == 0);
        p += n;

        if (n == 1 && seg[0] == '.') continue;           // "." = 自己
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {  // ".." = 父目录（根的父亲是根）
            uint8_t ino[VFS64_INODE_BYTES_MAX];
            if (!inode_load(cur, ino)) return -1;
            const uint32_t par = rd32(ino + VFS_I_PARENT);
            cur = (par < g_inode_count) ? par : 0;
            continue;
        }
        if (depth >= VFS64_PATH_DEPTH_MAX) { log_path_bad("too deep", path); return -1; }
        depth++;
        if (!name_valid(seg, n)) { log_path_bad("bad name", path); return -1; }

        uint32_t idx = 0;
        if (find_child(cur, seg, n, &idx) != 0) {
            if (at_end && want_parent) {                 // 最后一段不存在：把父目录 + 名字交回去
                if (out_parent) *out_parent = cur;
                if (out_name) copy_bytes(out_name, seg, n + 1);
                if (out_nlen) *out_nlen = n;
                return 1;
            }
            log_path_bad("component not found", path);
            return -1;
        }
        cur = idx;
    }
    if (out_ino) *out_ino = cur;
    return 0;
}
// 载入并校验一个 inode；失败打点。-1 = 失败。
static int inode_load_ok(uint32_t idx, uint8_t* buf, const char* op) {
    if (!inode_load(idx, buf)) return -1;
    const char* why = "?";
    if (!inode_ok(buf, &why)) {
        dbg64_str("[VFS64] ");
        dbg64_str(op);
        dbg64_str(": bad inode reason=");
        dbg64_str(why);
        dbg64_nl();
        return -1;
    }
    return 0;
}

// ==================== 时间戳 / 类型判定 ====================
uint32_t vfs64_pack_time64(int year, int month, int day, int hour, int minute, int second) {
    if (year < 2000 || year > 2063) return 0;
    if (month < 1 || month > 12) return 0;
    if (day < 1 || day > 31) return 0;
    if (hour < 0 || hour > 23) return 0;
    if (minute < 0 || minute > 59) return 0;
    if (second < 0 || second > 59) return 0;
    return ((uint32_t)(year - 2000) << 26) | ((uint32_t)month << 22) | ((uint32_t)day << 17) |
           ((uint32_t)hour << 12) | ((uint32_t)minute << 6) | (uint32_t)second;
}
void vfs64_unpack_time64(uint32_t packed, Vfs64Time64* out) {
    if (!out) return;
    if (packed == 0) {
        out->year = 0; out->month = 0; out->day = 0;
        out->hour = 0; out->minute = 0; out->second = 0;
        return;
    }
    out->year   = (uint16_t)(2000u + ((packed >> 26) & 0x3Fu));
    out->month  = (uint8_t)((packed >> 22) & 0xFu);
    out->day    = (uint8_t)((packed >> 17) & 0x1Fu);
    out->hour   = (uint8_t)((packed >> 12) & 0x1Fu);
    out->minute = (uint8_t)((packed >> 6)  & 0x3Fu);
    out->second = (uint8_t)( packed        & 0x3Fu);
}
// RTC 读一次并打包。RTC 不可信（月 0/13、日 0、年 < 2000…）时的回退值：
//   2025-01-01 00:00:00 —— **固定且有文档**，保证 mtime 非 0（UI 至少能显示一个"有效"时间），
//   并打一次告警行（不刷屏：只打第一次）。
uint32_t vfs64_now64() {
    int y = 0, mo = 0, d = 0, wd = 0;
    int h = 0, mi = 0, s = 0;
    rtc_get_date64(&y, &mo, &d, &wd);
    rtc_get_time64(&h, &mi, &s);
    uint32_t packed = vfs64_pack_time64(y, mo, d, h, mi, s);
    if (packed == 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            log_line("warn rtc invalid -> mtime fallback 2025-01-01 00:00:00");
        }
        packed = vfs64_pack_time64(2025, 1, 1, 0, 0, 0);
    }
    return packed;
}
void vfs64_time_str64(uint32_t packed, char* ymd, int ymd_cap, char* hms, int hms_cap) {
    char yb[11];
    char hb[9];
    if (packed == 0) {
        static const char z1[] = "0000-00-00";
        static const char z2[] = "00:00:00";
        if (ymd && ymd_cap > 10) copy_bytes(ymd, z1, 11);
        if (hms && hms_cap > 8)  copy_bytes(hms, z2, 9);
        return;
    }
    Vfs64Time64 t;
    vfs64_unpack_time64(packed, &t);
    for (int i = 0; i < 10; i++) yb[i] = 0;
    // 手写十进制（不依赖 libc）：YYYY-MM-DD
    uint32_t yv = t.year;
    yb[0] = (char)('0' + (yv / 1000u) % 10u);
    yb[1] = (char)('0' + (yv / 100u) % 10u);
    yb[2] = (char)('0' + (yv / 10u) % 10u);
    yb[3] = (char)('0' + yv % 10u);
    yb[4] = '-';
    yb[5] = (char)('0' + (t.month / 10u));
    yb[6] = (char)('0' + (t.month % 10u));
    yb[7] = '-';
    yb[8] = (char)('0' + (t.day / 10u));
    yb[9] = (char)('0' + (t.day % 10u));
    hb[0] = (char)('0' + (t.hour / 10u));
    hb[1] = (char)('0' + (t.hour % 10u));
    hb[2] = ':';
    hb[3] = (char)('0' + (t.minute / 10u));
    hb[4] = (char)('0' + (t.minute % 10u));
    hb[5] = ':';
    hb[6] = (char)('0' + (t.second / 10u));
    hb[7] = (char)('0' + (t.second % 10u));
    hb[8] = 0;
    if (ymd) {
        int i = 0;
        for (; i < 10 && i + 1 < ymd_cap; i++) ymd[i] = yb[i];
        ymd[i] = 0;
    }
    if (hms) {
        int i = 0;
        for (; i < 8 && i + 1 < hms_cap; i++) hms[i] = hb[i];
        hms[i] = 0;
    }
}
// 扩展名判定（保守：只认一小撮明确是文本的后缀）
static bool ext_is_text(const char* name, uint32_t nlen) {
    if (nlen < 4) return false;
    const char* e = nullptr;
    for (uint32_t i = nlen; i > 0; i--) {
        if (name[i - 1] == '.') { e = name + i - 1; break; }
    }
    if (!e) return false;
    return str_eq(e, ".txt") || str_eq(e, ".md") || str_eq(e, ".cfg") || str_eq(e, ".log") ||
           str_eq(e, ".ini") || str_eq(e, ".json") || str_eq(e, ".c") || str_eq(e, ".h");
}
uint32_t vfs64_kind_of_data64(const void* data, uint32_t len, const char* name, uint32_t name_len) {
    const uint8_t* p = (const uint8_t*)data;
    if (len >= 8u && p) {                                    // VAP64 头："VAP64\0\0\0"
        if (p[0] == 'V' && p[1] == 'A' && p[2] == 'P' && p[3] == '6' && p[4] == '4' &&
            p[5] == 0 && p[6] == 0 && p[7] == 0) return VFS64_KIND_VAP;
    }
    if (len >= 4u && p) {                                    // ELF64："\x7fELF"
        if (p[0] == 0x7Fu && p[1] == 'E' && p[2] == 'L' && p[3] == 'F') return VFS64_KIND_ELF;
    }
    if (name && ext_is_text(name, name_len)) return VFS64_KIND_TEXT;
    if (len == 0) return VFS64_KIND_FILE;                    // 空文件：没有内容可判
    // 可打印字符启发式：只看前 256 字节，允许 \t \r \n，可打印比例 >= 90% 就当文本
    const uint32_t probe = (len < 256u) ? len : 256u;
    uint32_t printable = 0;
    for (uint32_t i = 0; i < probe; i++) {
        const uint8_t c = p[i];
        if ((c >= 0x20u && c <= 0x7Eu) || c == '\t' || c == '\r' || c == '\n') printable++;
    }
    if (printable * 10u >= probe * 9u) return VFS64_KIND_TEXT;
    return VFS64_KIND_BIN;
}
uint32_t vfs64_kind_by_name64(uint32_t type, const char* name, uint32_t name_len) {
    if (type == VFS64_TYPE_DIR) return VFS64_KIND_DIR;
    if (name && name_len >= 4) {
        const char* e = nullptr;
        for (uint32_t i = name_len; i > 0; i--) if (name[i - 1] == '.') { e = name + i - 1; break; }
        if (e) {
            if (str_eq(e, ".vap")) return VFS64_KIND_VAP;
            if (str_eq(e, ".elf")) return VFS64_KIND_ELF;
        }
        if (ext_is_text(name, name_len)) return VFS64_KIND_TEXT;
    }
    return (type == VFS64_TYPE_FILE) ? VFS64_KIND_FILE : VFS64_KIND_NONE;
}
const char* vfs64_kind_str64(uint32_t kind) {
    if (kind == VFS64_KIND_DIR)  return "dir";
    if (kind == VFS64_KIND_VAP)  return "vap";
    if (kind == VFS64_KIND_ELF)  return "elf";
    if (kind == VFS64_KIND_TEXT) return "text";
    if (kind == VFS64_KIND_BIN)  return "bin";
    if (kind == VFS64_KIND_FILE) return "file";
    return "unknown";
}
// 目录内容变了：刷新父目录的 mtime（**尽力而为**：失败只打 WARN，不把已成功的操作判失败）
static void touch_dir(uint32_t idx, int nlink_delta) {
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    if (inode_load_ok(idx, ino, "touch") != 0) return;
    if (g_lay->mtime_off != 0) wr32(ino + g_lay->mtime_off, vfs64_now64());
    if (nlink_delta != 0 && g_lay->nlink_off != 0) {
        int nl = (int)rd16(ino + g_lay->nlink_off) + nlink_delta;
        if (nl < 1) nl = 1;
        wr16(ino + g_lay->nlink_off, (uint16_t)nl);
    }
    if (g_lay->crc_off != 0) wr32(ino + g_lay->crc_off, crc32_64(ino, g_lay->crc_off));
    if (!inode_store(idx, ino)) log_line("WARN dir touch failed");
}

// ==================== 格式化（产出 v3）====================
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

    // 几何：位图在最前，inode 区居中，数据区占剩下的全部（inode 记录大小按 v3 = 128B）
    const uint32_t bitmap_blocks = (total_sectors + VFS64_BITMAP_BLK_BITS - 1u) / VFS64_BITMAP_BLK_BITS;
    uint32_t inodes = total_sectors / 64u;
    if (inodes < 16u) inodes = 16u;
    if (inodes > VFS64_MAX_INODES) inodes = VFS64_MAX_INODES;
    const uint32_t inode_blocks = (inodes + VFS_LAY_V3.inodes_per_blk - 1u) / VFS_LAY_V3.inodes_per_blk;
    const uint32_t bitmap_start = 1u;
    const uint32_t inode_start  = bitmap_start + bitmap_blocks;
    const uint32_t data_start   = inode_start + inode_blocks;
    const uint32_t data_blocks  = total_sectors - data_start;

    // ★ 多卷：格式化 = 造一个**卷**（安装器 / 自检路径）。如果还没有登记过系统卷槽，就把当前槽
    //   登记为系统卷槽 —— 之后 store64/config64/update64 等"固定写系统卷"的组件才有明确目标。
    //   （系统内核的正常路径是 vfs64_mount_system64，它在挂载时就把 0 号槽登记成系统卷槽。）
    if (g_system_slot < 0) {
        g_system_slot = g_cur_slot;
        dbg64_str("[VFS64] system slot=");
        dbg64_dec((uint64_t)g_system_slot);
        dbg64_str(" (set by format)");
        dbg64_nl();
    }

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
    g_lay = &VFS_LAY_V3;
    ino_cache_invalidate();

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

    // 4) 根目录 inode（0 号槽）：目录、无名、size=0、parent=自己、nlink=1、mtime=现在
    uint8_t root[VFS64_INODE_BYTES_MAX];
    zero_bytes(root, VFS64_INODE_BYTES_MAX);
    root[VFS_I_TYPE] = (uint8_t)VFS64_TYPE_DIR;
    wr32(root + VFS_I_PARENT, 0);
    wr32(root + VFS_I3_MTIME, vfs64_now64());
    wr16(root + VFS_I3_NLINK, 1);
    root[VFS_I3_KIND] = (uint8_t)VFS64_KIND_DIR;
    wr32(root + VFS_LAY_V3.crc_off, crc32_64(root, VFS_LAY_V3.crc_off));
    if (!inode_store(0, root)) {
        log_line("format FAILED step=root-inode");
        g_mounted = false;
        return -1;
    }

    g_mounted = true;
    dbg64_str("[VFS64] format ok blocks=");
    dbg64_dec(total_sectors);
    dbg64_str(" version=");
    dbg64_dec(VFS64_VERSION);
    dbg64_str(" inode=");
    dbg64_dec(VFS64_INODE_BYTES);
    dbg64_str(" root=");
    dbg64_dec((uint64_t)start_lba + inode_start);   // 根目录 inode 所在扇区的绝对 LBA
    dbg64_nl();
    return 0;
}

// ==================== 挂载 / 只读探测 ====================
// 把探测/挂载得到的几何搬进内存（调用方已保证字段合法）
static void geom_apply(const Vfs64SbGeo& g) {
    g_blocks        = g.blocks;
    g_bitmap_start  = g.bitmap_start;
    g_bitmap_blocks = g.bitmap_blocks;
    g_inode_start   = g.inode_start;
    g_inode_count   = g.inode_count;
    g_data_start    = g.data_start;
    g_data_blocks   = g.data_blocks;
    g_lay           = g.lay;
    ino_cache_invalidate();
}
int vfs64_mount(int drive, uint32_t start_lba) {
    Vfs64Geom saved;
    geom_save(&saved);
    g_mounted = false;
    g_drive = drive;
    g_start = start_lba;
    ino_cache_invalidate();

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
    Vfs64SbGeo geo;
    const int rc = sb_verify(g_sec, &geo);
    if (rc != SB_OK) {
        log_mount_fail(sb_reason_str(rc));
        geom_restore(&saved);
        return -1;
    }
    geom_apply(geo);
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
    dbg64_str(" version=");
    dbg64_dec(g_lay->version);
    dbg64_str(" inode=");
    dbg64_dec(g_lay->inode_bytes);
    dbg64_str("B");
    dbg64_nl();
    return 0;
}
int vfs64_probe_volume64(int drive, uint32_t start_lba, Vfs64VolInfo64* out) {
    static uint8_t sec[VFS64_SECTOR_BYTES];               // 专用缓冲：不动 g_sec / 不动挂载状态
    if (!g_fake_active && !ata_linked()) {
        log_line("probe: no ATA driver linked");
        return -1;
    }
    if (!dev_read_at(drive, start_lba, 1, sec)) return -1;
    Vfs64SbGeo geo;
    if (sb_verify(sec, &geo) != SB_OK) return -1;
    uint32_t free_blocks = 0;
    const bool ok = count_free_at(drive, start_lba, geo.bitmap_start, geo.bitmap_blocks,
                                  geo.blocks, geo.data_start, &free_blocks);
    if (!ok) return -1;
    if (out) {
        out->version = geo.version;
        out->blocks = geo.blocks;
        out->inodes = geo.inode_count;
        out->inode_bytes = geo.lay->inode_bytes;
        out->bitmap_start = geo.bitmap_start;
        out->bitmap_blocks = geo.bitmap_blocks;
        out->data_start = geo.data_start;
        out->data_blocks = geo.data_blocks;
        out->free_blocks = free_blocks;
    }
    return 0;
}
int vfs64_mounted_volume64(int* drive, uint32_t* start_lba, Vfs64VolInfo64* out) {
    if (!g_mounted) return -1;
    if (drive) *drive = g_drive;
    if (start_lba) *start_lba = g_start;
    if (out) {
        uint32_t free_blocks = 0;
        (void)count_free_blocks(&free_blocks);
        out->version = g_lay->version;
        out->blocks = g_blocks;
        out->inodes = g_inode_count;
        out->inode_bytes = g_lay->inode_bytes;
        out->bitmap_start = g_bitmap_start;
        out->bitmap_blocks = g_bitmap_blocks;
        out->data_start = g_data_start;
        out->data_blocks = g_data_blocks;
        out->free_blocks = free_blocks;
    }
    return 0;
}


// ==================== ★ 多卷：卷槽 API + 按槽调用守卫 ====================
// 打点都带 slot=<n>：自动验收能把"切到哪块盘"与盘符对上（[UI] explorer enter letter=D: slot=2 ok）。
static void log_slot_bad(const char* op, int slot) {
    dbg64_str("[VFS64] ");
    dbg64_str(op);
    dbg64_str(": bad slot=");
    dbg64_dec((uint64_t)slot);
    dbg64_str(" (slots=");
    dbg64_dec((uint64_t)VFS64_SLOT_MAX);
    dbg64_str(")");
    dbg64_nl();
}

int vfs64_slot_used64(int slot) {
    if (slot < 0 || slot >= (int)VFS64_SLOT_MAX) return 0;
    return g_vol[slot].mounted ? 1 : 0;
}
int vfs64_current_slot64() { return g_mounted ? g_cur_slot : -1; }
int vfs64_system_slot64()  { return g_system_slot; }        // 没挂过/没格式化过系统卷时是 -1（调用方如实失败）

int vfs64_slot_alloc64() {
    for (int i = 0; i < (int)VFS64_SLOT_MAX; i++) if (!g_vol[i].mounted) return i;
    return -1;                                              // 卷表满：调用方必须如实拒绝，绝不覆盖已有卷
}
int vfs64_slot_find64(int drive, uint32_t start_lba) {
    for (int i = 0; i < (int)VFS64_SLOT_MAX; i++) {
        if (!g_vol[i].mounted) continue;
        if (g_vol[i].drive == drive && g_vol[i].start == start_lba) return i;
    }
    return -1;
}
int vfs64_slot_info64(int slot, int* drive, uint32_t* start_lba, Vfs64VolInfo64* out) {
    if (slot < 0 || slot >= (int)VFS64_SLOT_MAX || !g_vol[slot].mounted) {
        log_slot_bad("slot_info64", slot);
        return -1;
    }
    const Vfs64Geom& v = g_vol[slot];
    if (drive) *drive = v.drive;
    if (start_lba) *start_lba = v.start;
    if (out) {
        uint32_t free_blocks = 0;
        // 只读：显式传 (drive, 起始 LBA) 数一遍位图 —— **不切换当前卷、不碰挂载状态**。
        if (!count_free_at(v.drive, v.start, v.bitmap_start, v.bitmap_blocks,
                           v.blocks, v.data_start, &free_blocks)) return -1;
        out->version = v.lay->version;
        out->blocks = v.blocks;
        out->inodes = v.inode_count;
        out->inode_bytes = v.lay->inode_bytes;
        out->bitmap_start = v.bitmap_start;
        out->bitmap_blocks = v.bitmap_blocks;
        out->data_start = v.data_start;
        out->data_blocks = v.data_blocks;
        out->free_blocks = free_blocks;
    }
    return 0;
}

// ---- 把一个卷挂进指定槽（不改变当前卷）----
int vfs64_mount_slot64(int slot, int drive, uint32_t start_lba) {
    if (slot < 0 || slot >= (int)VFS64_SLOT_MAX) { log_slot_bad("mount_slot64", slot); return -1; }
    const int saved_cur = g_cur_slot;
    if (slot == saved_cur) {                                // 挂进当前槽：mount 内部自己 save/restore
        const int rc0 = vfs64_mount(drive, start_lba);
        dbg64_str("[VFS64] mount slot=");
        dbg64_dec((uint64_t)slot);
        dbg64_str(rc0 == 0 ? " ok (current)" : " FAILED (current)");
        dbg64_nl();
        return rc0;
    }
    g_cur_slot = slot;                                      // 只让 mount 改这个槽的几何
    const int rc = vfs64_mount(drive, start_lba);
    g_cur_slot = saved_cur;                                 // 当前卷立刻换回（挂载不是激活）
    dbg64_str("[VFS64] mount slot=");
    dbg64_dec((uint64_t)slot);
    dbg64_str(rc == 0 ? " ok drive=" : " FAILED drive=");
    dbg64_dec((uint64_t)drive);
    dbg64_str(" start=");
    dbg64_dec(start_lba);
    dbg64_nl();
    return rc;
}

// ---- 激活一个槽 = 把"当前卷"切过去 ----
int vfs64_activate_slot64(int slot) {
    if (slot < 0 || slot >= (int)VFS64_SLOT_MAX) { log_slot_bad("activate_slot64", slot); return -1; }
    if (!g_vol[slot].mounted) {
        dbg64_str("[VFS64] activate slot=");
        dbg64_dec((uint64_t)slot);
        dbg64_str(" FAILED reason=not-mounted");
        dbg64_nl();
        return -1;
    }
    g_cur_slot = slot;
    // 缓存按 (slot, drive, lba) 键控，切卷不需要清 —— 这里只打点，证明"切到哪个卷"。
    dbg64_str("[VFS64] activate slot=");
    dbg64_dec((uint64_t)slot);
    dbg64_str(" drive=");
    dbg64_dec((uint64_t)g_drive);
    dbg64_str(" start=");
    dbg64_dec(g_start);
    dbg64_str(" blocks=");
    dbg64_dec(g_blocks);
    dbg64_str(" version=");
    dbg64_dec(g_lay->version);
    dbg64_nl();
    return 0;
}

// ---- 挂系统卷：0 号槽 + 记成系统卷槽 + 激活 ----
int vfs64_mount_system64(int drive, uint32_t start_lba) {
    const int slot = 0;                                     // 约定：系统卷固定 0 号槽（盘符 C:）
    const int existed = vfs64_slot_find64(drive, start_lba);
    if (existed >= 0) {
        // 已经挂过（例如二次初始化）：直接把它记成系统卷槽并激活，不重复挂载。
        g_system_slot = existed;
        if (vfs64_activate_slot64(existed) != 0) return -1;
    } else {
        if (vfs64_mount_slot64(slot, drive, start_lba) != 0) {
            log_line("mount_system FAILED (mount)");
            return -1;
        }
        g_system_slot = slot;

        if (vfs64_activate_slot64(slot) != 0) return -1;
    }

    dbg64_str("[VFS64] mount_system slot=");
    dbg64_dec((uint64_t)g_system_slot);
    dbg64_str(" drive=");
    dbg64_dec((uint64_t)drive);
    dbg64_str(" start=");
    dbg64_dec(start_lba);
    dbg64_nl();
    return 0;
}

// 卷槽表串口打印（终端 `vol` / 自动验收：[VFS64] slots n=<n> system=<s> current=<c>）
void vfs64_slots_dump64() {
    int used = 0;
    for (int i = 0; i < (int)VFS64_SLOT_MAX; i++) if (g_vol[i].mounted) used++;
    dbg64_str("[VFS64] slots n=");
    dbg64_dec((uint64_t)VFS64_SLOT_MAX);
    dbg64_str(" used=");
    dbg64_dec((uint64_t)used);
    dbg64_str(" system=");
    if (g_system_slot >= 0) dbg64_dec((uint64_t)g_system_slot); else dbg64_str("-");
    dbg64_str(" current=");
    if (g_mounted) dbg64_dec((uint64_t)g_cur_slot); else dbg64_str("-");
    dbg64_nl();
    for (int i = 0; i < (int)VFS64_SLOT_MAX; i++) {
        dbg64_str("[VFS64] slot=");
        dbg64_dec((uint64_t)i);
        if (!g_vol[i].mounted) { dbg64_str(" used=no"); dbg64_nl(); continue; }
        uint32_t free_blocks = 0;
        const bool freed = count_free_at(g_vol[i].drive, g_vol[i].start, g_vol[i].bitmap_start,
                                         g_vol[i].bitmap_blocks, g_vol[i].blocks,
                                         g_vol[i].data_start, &free_blocks);
        dbg64_str(" used=yes drive=");
        dbg64_dec((uint64_t)g_vol[i].drive);
        dbg64_str(" start=");
        dbg64_dec(g_vol[i].start);
        dbg64_str(" blocks=");
        dbg64_dec(g_vol[i].blocks);
        dbg64_str(" version=");
        dbg64_dec(g_vol[i].lay ? g_vol[i].lay->version : 0u);
        dbg64_str(" free=");
        if (freed) dbg64_dec(free_blocks); else dbg64_str("?");
        dbg64_str(i == g_cur_slot ? " current=yes" : "");
        dbg64_nl();
    }
}

// ---- 临时切卷守卫：_on64 的实现基础 ----
// 构造 = 当前卷换成 slot（必须已挂载）；析构 = **原样换回**进来时那个槽。
//   * 嵌套（理论上不会：_on64 内部只调旧 API）：计数 + LIFO 恢复，语义不乱。
//   * 中断：vfs64 的调用点都在任务上下文（GUI 主循环 / 终端 / 自检）；即便被抢断，别的上下文
//     也只会以同样的守卫方式切卷，每次恢复的都是"自己进来时"的卷 —— 当前卷不会丢。
//   * inode 缓存按 (slot, drive, lba) 键控，所以切卷不必清缓存，也不会读到别的卷的字节。
struct Vfs64SlotGuard {
    int  saved;
    bool active;
    explicit Vfs64SlotGuard(int slot) {
        active = false;
        if (slot < 0 || slot >= (int)VFS64_SLOT_MAX || !g_vol[slot].mounted) return;
        if (g_vol_switch_depth > 0) log_line("WARN nested volume switch (LIFO restore)");
        saved = g_cur_slot;
        g_cur_slot = slot;
        g_vol_switch_depth++;
        active = true;
    }
    ~Vfs64SlotGuard() {
        if (!active) return;
        g_vol_switch_depth--;
        g_cur_slot = saved;                                 // ★ 原样切回：用户的浏览卷不受影响
    }
};
// 守卫失败（槽非法/没挂载）时的统一打点：说明"这次按槽调用没有落到任何卷上"，绝不静默写错地方。
static int on64_slot_unavailable(const char* op, int slot) {
    dbg64_str("[VFS64] ");
    dbg64_str(op);
    dbg64_str("_on64: slot=");
    dbg64_dec((uint64_t)slot);
    dbg64_str(" not mounted");
    dbg64_nl();
    return -1;
}

// ---- 按槽别名（系统组件固定写系统卷用的入口）----
int vfs64_stat_on64(int slot, const char* path, uint32_t* type, uint32_t* size) {
    Vfs64SlotGuard g(slot);
    if (!g.active) return on64_slot_unavailable("stat", slot);
    return vfs64_stat(path, type, size);
}
int vfs64_stat64_on64(int slot, const char* path, Vfs64Info64* out) {
    Vfs64SlotGuard g(slot);
    if (!g.active) return on64_slot_unavailable("stat64", slot);
    return vfs64_stat64(path, out);
}
int vfs64_read_on64(int slot, const char* path, void* buf, int max) {
    Vfs64SlotGuard g(slot);
    if (!g.active) return on64_slot_unavailable("read", slot);
    return vfs64_read(path, buf, max);
}
int vfs64_write_on64(int slot, const char* path, const void* buf, int len) {
    Vfs64SlotGuard g(slot);
    if (!g.active) return on64_slot_unavailable("write", slot);
    return vfs64_write(path, buf, len);
}
int vfs64_mkdir_on64(int slot, const char* path) {
    Vfs64SlotGuard g(slot);
    if (!g.active) return on64_slot_unavailable("mkdir", slot);
    return vfs64_mkdir(path);
}
int vfs64_create_on64(int slot, const char* path) {
    Vfs64SlotGuard g(slot);
    if (!g.active) return on64_slot_unavailable("create", slot);
    return vfs64_create64(path);
}
int vfs64_unlink_on64(int slot, const char* path) {
    Vfs64SlotGuard g(slot);
    if (!g.active) return on64_slot_unavailable("unlink", slot);
    return vfs64_unlink(path);
}
int vfs64_rmdir_on64(int slot, const char* path) {
    Vfs64SlotGuard g(slot);
    if (!g.active) return on64_slot_unavailable("rmdir", slot);
    return vfs64_rmdir64(path);
}
int vfs64_ls_on64(int slot, const char* path, char names[][VFS64_LS_NAME_BUF], int max, uint32_t* sizes) {
    Vfs64SlotGuard g(slot);
    if (!g.active) return on64_slot_unavailable("ls", slot);
    return vfs64_ls(path, names, max, sizes);
}
int vfs64_list64_on64(int slot, const char* path, Vfs64Dirent64* out, int max, uint32_t* cursor) {
    Vfs64SlotGuard g(slot);
    if (!g.active) return on64_slot_unavailable("list64", slot);
    return vfs64_list64(path, out, max, cursor);
}
int vfs64_tree_dump64_on64(int slot, const char* path, int max_entries, int max_depth) {
    Vfs64SlotGuard g(slot);
    if (!g.active) return on64_slot_unavailable("tree_dump64", slot);
    return vfs64_tree_dump64(path, max_entries, max_depth);
}

// ★ 批次 J：同目录改名 / 空间查询 的按槽入口（explorer 复制/剪切/粘贴时按显式卷操作）
int vfs64_rename_on64(int slot, const char* old_path, const char* new_name) {
    Vfs64SlotGuard g(slot);
    if (!g.active) return on64_slot_unavailable("rename", slot);
    return vfs64_rename64(old_path, new_name);
}
int vfs64_free_on64(int slot, uint32_t* free_blocks, uint32_t* free_bytes, uint32_t* total_blocks) {
    Vfs64SlotGuard g(slot);
    if (!g.active) return on64_slot_unavailable("free", slot);
    return vfs64_free64(free_blocks, free_bytes, total_blocks);
}
// ==================== 读文件 ====================
int vfs64_read64(const char* path, void* buf, int max) {
    if (!g_mounted) { log_op_fail("read64", "not mounted"); return -1; }
    if (!path || !buf || max < 0) { log_op_fail("read64", "bad args"); return -1; }

    uint32_t idx = 0;
    if (path_resolve(path, false, &idx, nullptr, nullptr, nullptr) != 0) {
        log_op_fail("read64", "not found");
        return -1;
    }
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    if (inode_load_ok(idx, ino, "read64") != 0) return -1;
    if (ino[VFS_I_TYPE] != VFS64_TYPE_FILE) { log_op_fail("read64", "not a file"); return -1; }

    const uint32_t size = rd32(ino + VFS_I_SIZE);
    uint32_t left = size;
    if ((uint32_t)max < left) left = (uint32_t)max;
    uint32_t done = 0;
    uint8_t* out = (uint8_t*)buf;
    bool ind_loaded = false;
    uint32_t ptrs[VFS64_INDIRECT_PTRS];
    for (uint32_t i = 0; left > 0; i++) {
        uint32_t blk = 0;
        if (i < VFS64_DIRECT_BLOCKS) {
            blk = rd32(ino + VFS_I_D0 + 4u * i);
        } else {
            if (i - VFS64_DIRECT_BLOCKS >= VFS64_INDIRECT_PTRS) { log_op_fail("read64", "block index out of range"); return -1; }
            if (!ind_loaded) {
                const uint32_t ind = rd32(ino + VFS_I_IND);
                if (ind == 0) { log_op_fail("read64", "hole in file (no indirect)"); return -1; }
                if (!load_indirect(ind, ptrs)) return -1;
                ind_loaded = true;
            }
            blk = ptrs[i - VFS64_DIRECT_BLOCKS];
        }
        if (blk == 0) { log_op_fail("read64", "hole in file"); return -1; }
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
int vfs64_read(const char* path, void* buf, int max) { return vfs64_read64(path, buf, max); }

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

int vfs64_write64(const char* path, const void* buf, int len) {
    if (!g_mounted) { log_op_fail("write64", "not mounted"); return -1; }
    if (!path || !buf || len < 0) { log_op_fail("write64", "bad args"); return -1; }
    if ((uint32_t)len > VFS64_MAX_FILE_BYTES) { log_op_fail("write64", "too large"); return -1; }

    uint32_t idx = 0, parent = 0, nlen = 0;
    char nm[VFS64_NAME_MAX + 1];
    const int pr = path_resolve(path, true, &idx, &parent, nm, &nlen);
    if (pr < 0) { log_op_fail("write64", "bad path"); return -1; }

    const bool exists = (pr == 0);
    uint8_t old_ino[VFS64_INODE_BYTES_MAX];
    if (exists) {
        if (inode_load_ok(idx, old_ino, "write64") != 0) return -1;
        if (old_ino[VFS_I_TYPE] != VFS64_TYPE_FILE) { log_op_fail("write64", "path is a directory"); return -1; }
        // 覆盖已有文件：父目录与名字以 inode 里的为准（"." 之类也会走到这里）
        parent = rd32(old_ino + VFS_I_PARENT);
        nlen = old_ino[VFS_I_NAMELEN];
        copy_bytes(nm, old_ino + g_lay->name_off, nlen);
        nm[nlen] = 0;
    } else {
        if (!alloc_inode(&idx)) { log_op_fail("write64", "no free inode"); return -1; }
        zero_bytes(old_ino, VFS64_INODE_BYTES_MAX);
    }

    // 新的 inode 镜像（先全部填好，提交前不碰盘；失败时旧 inode 原样保留）
    uint8_t ino_new[VFS64_INODE_BYTES_MAX];
    zero_bytes(ino_new, VFS64_INODE_BYTES_MAX);
    ino_new[VFS_I_TYPE] = (uint8_t)VFS64_TYPE_FILE;
    ino_new[VFS_I_NAMELEN] = (uint8_t)nlen;
    copy_bytes(ino_new + g_lay->name_off, nm, nlen);
    wr32(ino_new + VFS_I_PARENT, parent);
    wr32(ino_new + VFS_I_SIZE, (uint32_t)len);
    if (g_lay->mtime_off != 0) wr32(ino_new + g_lay->mtime_off, vfs64_now64());
    if (g_lay->nlink_off != 0) wr16(ino_new + g_lay->nlink_off, 1);
    if (g_lay->kind_off != 0) ino_new[g_lay->kind_off] = (uint8_t)vfs64_kind_of_data64(buf, (uint32_t)len, nm, nlen);
    zero_bytes(g_ind, VFS64_SECTOR_BYTES);

    const uint32_t needed = ((uint32_t)len + VFS64_BLOCK_BYTES - 1u) / VFS64_BLOCK_BYTES;
    if (needed > VFS64_DIRECT_BLOCKS + VFS64_INDIRECT_PTRS) { log_op_fail("write64", "size exceeds block map"); return -1; }
    uint32_t new_ind = 0;
    if (needed > VFS64_DIRECT_BLOCKS) {
        new_ind = alloc_block();                       // 间接块先分配（指针数组要指向数据块）
        if (new_ind == 0) { log_op_fail("write64", "no space for indirect block"); return -1; }
    }

    const uint8_t* src = (const uint8_t*)buf;
    for (uint32_t i = 0; i < needed; i++) {
        const uint32_t b = alloc_block();
        if (b == 0) {
            rollback_new(ino_new, new_ind);
            log_op_fail("write64", "no space (data)");
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
            log_op_fail("write64", "data block write failed");
            return -1;
        }
    }
    if (new_ind != 0) {
        wr32(ino_new + VFS_I_IND, new_ind);
        if (!blk_write(new_ind, g_ind)) {              // 间接块内容 = 512B 的块号数组
            rollback_new(ino_new, new_ind);
            log_op_fail("write64", "indirect block write failed");
            return -1;
        }
    }
    wr32(ino_new + g_lay->crc_off, crc32_64(ino_new, g_lay->crc_off));
    if (!inode_store(idx, ino_new)) {                  // 提交点：新 inode 落盘
        rollback_new(ino_new, new_ind);
        log_op_fail("write64", "inode commit failed");
        return -1;
    }
    // 旧块在提交之后才释放：中途任何一步失败都只泄漏块，不会让 inode 指向已释放的块
    if (exists) {
        if (!free_file_blocks(old_ino)) log_line("write64: WARN old blocks partially freed");
    } else {
        touch_dir(parent, 0);                          // 新建：刷新父目录 mtime（尽力而为）
    }
    return len;
}
int vfs64_write(const char* path, const void* buf, int len) { return vfs64_write64(path, buf, len); }

int vfs64_create64(const char* path) {
    if (!g_mounted) { log_op_fail("create64", "not mounted"); return -1; }
    if (!path) { log_op_fail("create64", "bad args"); return -1; }
    uint32_t idx = 0;
    const int pr = path_resolve(path, false, &idx, nullptr, nullptr, nullptr);
    if (pr == 0) {
        uint8_t ino[VFS64_INODE_BYTES_MAX];
        if (inode_load_ok(idx, ino, "create64") != 0) return -1;
        if (ino[VFS_I_TYPE] != VFS64_TYPE_FILE) { log_op_fail("create64", "path is a directory"); return -1; }
        return 0;                                      // 已存在且是文件 = 幂等成功
    }
    static const char empty[1] = { 0 };
    return (vfs64_write64(path, empty, 0) == 0) ? 0 : -1;
}

// ==================== 建目录 / 删除 ====================
int vfs64_mkdir64(const char* path) {
    if (!g_mounted) { log_op_fail("mkdir64", "not mounted"); return -1; }
    if (!path) { log_op_fail("mkdir64", "bad args"); return -1; }

    uint32_t idx = 0, parent = 0, nlen = 0;
    char nm[VFS64_NAME_MAX + 1];
    const int pr = path_resolve(path, true, &idx, &parent, nm, &nlen);
    if (pr < 0) { log_op_fail("mkdir64", "bad path / parent not found"); return -1; }
    if (pr == 0) { log_op_fail("mkdir64", "already exists"); return -1; }
    uint8_t pino[VFS64_INODE_BYTES_MAX];               // 父目录必须真的是目录
    if (inode_load_ok(parent, pino, "mkdir64") != 0) return -1;
    if (pino[VFS_I_TYPE] != VFS64_TYPE_DIR) { log_op_fail("mkdir64", "parent is not a directory"); return -1; }

    uint32_t slot = 0;
    if (!alloc_inode(&slot)) { log_op_fail("mkdir64", "no free inode"); return -1; }
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    zero_bytes(ino, VFS64_INODE_BYTES_MAX);
    ino[VFS_I_TYPE] = (uint8_t)VFS64_TYPE_DIR;
    ino[VFS_I_NAMELEN] = (uint8_t)nlen;
    copy_bytes(ino + g_lay->name_off, nm, nlen);
    wr32(ino + VFS_I_PARENT, parent);
    if (g_lay->mtime_off != 0) wr32(ino + g_lay->mtime_off, vfs64_now64());
    if (g_lay->nlink_off != 0) wr16(ino + g_lay->nlink_off, 1);
    if (g_lay->kind_off != 0) ino[g_lay->kind_off] = (uint8_t)VFS64_KIND_DIR;
    wr32(ino + g_lay->crc_off, crc32_64(ino, g_lay->crc_off));
    if (!inode_store(slot, ino)) { log_op_fail("mkdir64", "inode write failed"); return -1; }
    touch_dir(parent, (g_lay->nlink_off != 0) ? 1 : 0);   // 父目录：mtime 刷新 + nlink+1（子目录数）
    return 0;
}
int vfs64_mkdir(const char* path) { return vfs64_mkdir64(path); }

int vfs64_unlink64(const char* path) {
    if (!g_mounted) { log_op_fail("unlink64", "not mounted"); return -1; }
    if (!path) { log_op_fail("unlink64", "bad args"); return -1; }

    uint32_t idx = 0;
    if (path_resolve(path, false, &idx, nullptr, nullptr, nullptr) != 0) {
        log_op_fail("unlink64", "not found");
        return -1;
    }
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    if (inode_load_ok(idx, ino, "unlink64") != 0) return -1;
    if (ino[VFS_I_TYPE] == VFS64_TYPE_DIR) { log_op_fail("unlink64", "refuse to remove a directory (use rmdir64)"); return -1; }
    // inode 里有越界块号时拒绝删除（保持"能删掉的一定是结构自洽的项"）
    if (!free_file_blocks(ino)) { log_op_fail("unlink64", "corrupt inode (not removed)"); return -1; }

    const uint32_t parent = rd32(ino + VFS_I_PARENT);
    uint8_t empty[VFS64_INODE_BYTES_MAX];
    if (!inode_store(idx, empty)) { log_op_fail("unlink64", "inode clear failed"); return -1; }
    touch_dir(parent, 0);
    return 0;
}
int vfs64_unlink(const char* path) { return vfs64_unlink64(path); }

int vfs64_rmdir64(const char* path) {
    if (!g_mounted) { log_op_fail("rmdir64", "not mounted"); return -1; }
    if (!path) { log_op_fail("rmdir64", "bad args"); return -1; }

    uint32_t idx = 0;
    if (path_resolve(path, false, &idx, nullptr, nullptr, nullptr) != 0) {
        log_op_fail("rmdir64", "not found");
        return -1;
    }
    if (idx == 0) { log_op_fail("rmdir64", "refuse to remove the root directory"); return -1; }
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    if (inode_load_ok(idx, ino, "rmdir64") != 0) return -1;
    if (ino[VFS_I_TYPE] != VFS64_TYPE_DIR) { log_op_fail("rmdir64", "not a directory"); return -1; }

    // 必须空：扫一遍看有没有 parent == idx 的活条目
    uint8_t child[VFS64_INODE_BYTES_MAX];
    for (uint32_t i = 1; i < g_inode_count; i++) {
        if (!inode_load(i, child)) return -1;
        if (child[VFS_I_TYPE] == VFS64_TYPE_FREE) continue;
        if (rd32(child + VFS_I_PARENT) != idx) continue;
        log_op_fail("rmdir64", "directory not empty");
        return -1;
    }
    const uint32_t parent = rd32(ino + VFS_I_PARENT);
    uint8_t empty[VFS64_INODE_BYTES_MAX];
    zero_bytes(empty, VFS64_INODE_BYTES_MAX);
    if (!inode_store(idx, empty)) { log_op_fail("rmdir64", "inode clear failed"); return -1; }
    touch_dir(parent, -1);                             // 父目录：mtime 刷新 + nlink-1
    return 0;
    return 0;
}

// ==================== 改名（同目录）/ 空间查询（批次 J）====================
// 为什么只做同目录：目录表示 = "parent 字段相同的 inode 集合"（见 vfs64.h），改名只动 name 字段 +
// mtime + CRC —— 一个 inode 落盘就完事，没有中间态、不会出现"两边都看不到"的窗口。
// 跨目录移动要改 parent 并且维护两个目录的 nlink，属于另一档复杂度，本批**明说不做**。
int vfs64_rename64(const char* old_path, const char* new_name) {
    if (!g_mounted) { log_op_fail("rename64", "not mounted"); return -1; }
    if (!old_path || !new_name) { log_op_fail("rename64", "bad args"); return -1; }

    uint32_t nlen = 0;
    while (new_name[nlen] != 0) nlen++;
    if (!name_valid(new_name, nlen)) {                 // 空/超长/含 '/'/不可打印/'.'/'..' 全在这里被拒
        log_op_fail("rename64", "bad new name");
        return -1;
    }

    uint32_t idx = 0;
    if (path_resolve(old_path, false, &idx, nullptr, nullptr, nullptr) != 0) {
        log_op_fail("rename64", "not found");
        return -1;
    }
    if (idx == 0) { log_op_fail("rename64", "refuse to rename the root directory"); return -1; }

    uint8_t ino[VFS64_INODE_BYTES_MAX];
    if (inode_load_ok(idx, ino, "rename64") != 0) return -1;
    const uint32_t parent = rd32(ino + VFS_I_PARENT);
    const uint32_t onlen = ino[VFS_I_NAMELEN];
    if (onlen == nlen && cmp_bytes(ino + g_lay->name_off, new_name, nlen) == 0) return 0;   // 同名 = 幂等

    uint32_t exist = 0;
    if (find_child(parent, new_name, nlen, &exist) == 0) {
        log_op_fail("rename64", "target name already exists in the same directory");
        return -1;
    }
    char old_name[VFS64_NAME_MAX + 1];
    copy_bytes(old_name, ino + g_lay->name_off, onlen);
    old_name[onlen] = 0;

    zero_bytes(ino + g_lay->name_off, g_lay->name_max);          // 先清干净再写（v3 的 name pad 必须为 0）
    copy_bytes(ino + g_lay->name_off, new_name, nlen);
    ino[VFS_I_NAMELEN] = (uint8_t)nlen;
    if (g_lay->mtime_off != 0) wr32(ino + g_lay->mtime_off, vfs64_now64());
    wr32(ino + g_lay->crc_off, crc32_64(ino, g_lay->crc_off));
    if (!inode_store(idx, ino)) { log_op_fail("rename64", "inode write failed"); return -1; }
    touch_dir(parent, 0);                                        // 父目录 mtime 刷新（尽力而为）

    dbg64_str("[VFS64] rename ok idx=");
    dbg64_dec(idx);
    dbg64_str(" old=");
    dbg64_str(old_name);
    dbg64_str(" new=");
    dbg64_str(new_name);
    dbg64_nl();
    return 0;
}

int vfs64_free64(uint32_t* free_blocks, uint32_t* free_bytes, uint32_t* total_blocks) {
    if (!g_mounted) { log_op_fail("free64", "not mounted"); return -1; }
    uint32_t n = 0;
    if (!count_free_blocks(&n)) { log_op_fail("free64", "bitmap read failed"); return -1; }
    if (free_blocks) *free_blocks = n;
    if (free_bytes) *free_bytes = n * VFS64_BLOCK_BYTES;
    if (total_blocks) *total_blocks = g_data_blocks;
    return 0;
}

// ==================== 属性 / 遍历 ====================
static void dirent_from_inode(const uint8_t* ino, uint32_t idx, Vfs64Dirent64* d) {
    const uint32_t nl = ino[VFS_I_NAMELEN];
    zero_bytes(d->name, VFS64_NAME_MAX + 1);
    copy_bytes(d->name, ino + g_lay->name_off, nl);
    d->name[nl] = 0;
    d->index = idx;
    d->type = ino[VFS_I_TYPE];
    d->size = (d->type == VFS64_TYPE_FILE) ? rd32(ino + VFS_I_SIZE) : 0;
    d->mtime = (g_lay->mtime_off != 0) ? rd32(ino + g_lay->mtime_off) : 0;
    if (g_lay->kind_off != 0) {
        const uint8_t k = ino[g_lay->kind_off];
        d->kind = (k <= VFS64_KIND_MAX) ? k : VFS64_KIND_NONE;
    } else {
        d->kind = vfs64_kind_by_name64(d->type, d->name, nl);   // v2 卷：没有 kind 字段，按扩展名保守判
    }
}
// 列目录的**核心**（按 inode 号）：从 *cursor 开始最多 max 条。返回条数（0 = 结束），-1 = 参数错。
static int list_inode64(uint32_t dir, Vfs64Dirent64* out, int max, uint32_t* cursor) {
    if (!out || max <= 0) { log_op_fail("list64", "bad args"); return -1; }
    uint32_t c = cursor ? *cursor : 0u;
    if (c < 1u) c = 1u;
    int n = 0;
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    while (c < g_inode_count && n < max) {
        const uint32_t i = c;
        c++;                                            // 游标先推进：跳过的条目也要能前进（否则会死循环）
        if (!inode_load(i, ino)) return -1;
        if (ino[VFS_I_TYPE] == VFS64_TYPE_FREE) continue;
        if (rd32(ino + VFS_I_PARENT) != dir) continue;
        if (ino[VFS_I_NAMELEN] == 0) continue;          // 根目录自己不算条目
        const char* why = "?";
        if (!inode_ok(ino, &why)) continue;             // 结构坏的项不进结果（find/stat 会打点）
        dirent_from_inode(ino, i, &out[n]);
        n++;
    }
    if (cursor) *cursor = c;
    return n;
}
int vfs64_list64(const char* path, Vfs64Dirent64* out, int max, uint32_t* cursor) {
    if (!g_mounted) { log_op_fail("list64", "not mounted"); return -1; }
    if (!path) { log_op_fail("list64", "bad args"); return -1; }
    uint32_t idx = 0;
    if (path_resolve(path, false, &idx, nullptr, nullptr, nullptr) != 0) {
        log_op_fail("list64", "path not found");
        return -1;
    }
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    if (inode_load_ok(idx, ino, "list64") != 0) return -1;
    if (ino[VFS_I_TYPE] != VFS64_TYPE_DIR) { log_op_fail("list64", "not a directory"); return -1; }
    return list_inode64(idx, out, max, cursor);
}
int vfs64_stat64(const char* path, Vfs64Info64* out) {
    if (!g_mounted) { log_op_fail("stat64", "not mounted"); return -1; }
    if (!path || !out) { log_op_fail("stat64", "bad args"); return -1; }
    uint32_t idx = 0;
    if (path_resolve(path, false, &idx, nullptr, nullptr, nullptr) != 0) {
        log_op_fail("stat64", "not found");
        return -1;
    }
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    if (inode_load_ok(idx, ino, "stat64") != 0) return -1;
    zero_bytes(out, (uint32_t)sizeof(*out));
    out->index = idx;
    out->type = ino[VFS_I_TYPE];
    out->size = (out->type == VFS64_TYPE_FILE) ? rd32(ino + VFS_I_SIZE) : 0;
    out->parent = rd32(ino + VFS_I_PARENT);
    out->mtime = (g_lay->mtime_off != 0) ? rd32(ino + g_lay->mtime_off) : 0;
    out->nlink = (g_lay->nlink_off != 0) ? rd16(ino + g_lay->nlink_off) : 1u;
    const uint32_t nl = ino[VFS_I_NAMELEN];
    out->name_len = nl;
    copy_bytes(out->name, ino + g_lay->name_off, nl);
    out->name[nl] = 0;
    if (g_lay->kind_off != 0) {
        const uint8_t k = ino[g_lay->kind_off];
        out->kind = (k <= VFS64_KIND_MAX) ? k : VFS64_KIND_NONE;
    } else {
        out->kind = vfs64_kind_by_name64(out->type, out->name, nl);
    }
    return 0;
}
int vfs64_stat(const char* path, uint32_t* type, uint32_t* size) {
    Vfs64Info64 in;
    if (vfs64_stat64(path, &in) != 0) return -1;
    if (type) *type = in.type;
    if (size) *size = in.size;
    return 0;
}

// ---- 目录游标（opendir / readdir / closedir）----
// ★ 多卷：游标记住**打开时的卷身份**（slot/drive/start）。readdir 期间即使用户切了卷，
//   也会按记住的槽去读，绝不拿另一个卷的 inode 表当这个目录的内容；卷被重挂/换卷则如实报错。
struct Vfs64DirStream {
    bool     used;
    int      slot;     // 打开时的卷槽
    int      drive;    // 打开时的驱动器号
    uint32_t start;    // 打开时的卷起始 LBA
    uint32_t dir;      // 目录 inode 号
    uint32_t cursor;   // 下一个待扫描的 inode 下标
};
static Vfs64DirStream g_streams[VFS64_DIRSTREAM_MAX];

// 游标还指向当初那个卷吗？（同一个槽被重挂到别的盘 -> 不认，避免读到不相干的字节）
static bool stream_vol_ok(const Vfs64DirStream* st) {
    if (st->slot < 0 || st->slot >= (int)VFS64_SLOT_MAX || !g_vol[st->slot].mounted) return false;
    return g_vol[st->slot].drive == st->drive && g_vol[st->slot].start == st->start;
}

int vfs64_opendir64(const char* path, int* out_handle) {
    if (!g_mounted) { log_op_fail("opendir64", "not mounted"); return -1; }
    if (!path || !out_handle) { log_op_fail("opendir64", "bad args"); return -1; }
    uint32_t idx = 0;
    if (path_resolve(path, false, &idx, nullptr, nullptr, nullptr) != 0) {
        log_op_fail("opendir64", "path not found");
        return -1;
    }
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    if (inode_load_ok(idx, ino, "opendir64") != 0) return -1;
    if (ino[VFS_I_TYPE] != VFS64_TYPE_DIR) { log_op_fail("opendir64", "not a directory"); return -1; }
    for (uint32_t i = 0; i < VFS64_DIRSTREAM_MAX; i++) {
        if (g_streams[i].used) continue;
        g_streams[i].used = true;
        g_streams[i].slot = g_cur_slot;                 // ★ 记住卷身份
        g_streams[i].drive = g_drive;
        g_streams[i].start = g_start;
        g_streams[i].dir = idx;
        g_streams[i].cursor = 1u;
        *out_handle = (int)i;
        return 0;
    }
    log_op_fail("opendir64", "too many open dir streams");
    return -1;
}
int vfs64_readdir64(int handle, Vfs64Dirent64* out) {
    if (handle < 0 || handle >= (int)VFS64_DIRSTREAM_MAX) { log_op_fail("readdir64", "bad handle"); return -1; }
    Vfs64DirStream* st = &g_streams[handle];
    if (!st->used) { log_op_fail("readdir64", "handle not open"); return -1; }
    if (!out) { log_op_fail("readdir64", "bad args"); return -1; }
    if (!stream_vol_ok(st)) { log_op_fail("readdir64", "volume changed (handle is stale)"); return -1; }
    Vfs64SlotGuard g(st->slot);                          // 按打开时的卷操作（返回前切回当前卷）
    if (!g.active) { log_op_fail("readdir64", "volume not mounted"); return -1; }
    const int n = list_inode64(st->dir, out, 1, &st->cursor);
    if (n < 0) return -1;
    return n;                                   // 1 = 一条；0 = 枚举结束
}
int vfs64_closedir64(int handle) {
    if (handle < 0 || handle >= (int)VFS64_DIRSTREAM_MAX) { log_op_fail("closedir64", "bad handle"); return -1; }
    if (!g_streams[handle].used) { log_op_fail("closedir64", "handle not open"); return -1; }
    g_streams[handle].used = false;
    g_streams[handle].slot = -1;
    g_streams[handle].drive = -1;
    g_streams[handle].start = 0;
    g_streams[handle].dir = 0;
    g_streams[handle].cursor = 1u;
    return 0;
}

// ---- 兼容 API：vfs64_ls（[32] 名字缓冲）----
int vfs64_ls(const char* path, char names[][VFS64_LS_NAME_BUF], int max, uint32_t* sizes) {
    if (!g_mounted) { log_op_fail("ls", "not mounted"); return -1; }
    if (!path || !names || !sizes || max <= 0) { log_op_fail("ls", "bad args"); return -1; }
    uint32_t cursor = 0;
    int count = 0;
    Vfs64Dirent64 page[8];
    bool truncated = false;
    for (;;) {
        const int n = vfs64_list64(path, page, 8, &cursor);
        if (n < 0) return -1;
        if (n == 0) break;
        for (int i = 0; i < n; i++) {
            if (count >= max) { truncated = true; break; }
            uint32_t j = 0;
            for (; j < VFS64_LS_NAME_BUF - 1u && page[i].name[j] != 0; j++) names[count][j] = page[i].name[j];
            names[count][j] = 0;                    // 兼容缓冲 32B：超过 31B 截断（v3 上限就是 31）
            sizes[count] = page[i].size;
            count++;
        }
        if (truncated) break;
    }
    if (truncated) {
        dbg64_str("[VFS64] ls: truncated at max=");
        dbg64_dec((uint64_t)max);
        dbg64_nl();
    }
    return count;
}

// ==================== 目录树打印（验收/诊断；有界）====================
struct TreeDumpCtx {
    int budget;
    int max_depth;
    int printed;
    bool over;
};
static void tree_walk(uint32_t dir, const char* prefix, int depth, TreeDumpCtx* ctx) {
    if (ctx->over) return;
    if (depth > ctx->max_depth) return;
    uint32_t cursor = 0;
    Vfs64Dirent64 page[8];
    for (;;) {
        const int n = list_inode64(dir, page, 8, &cursor);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if (ctx->printed >= ctx->budget) { ctx->over = true; return; }
            char full[VFS64_PATH_MAX + VFS64_NAME_MAX + 4];
            uint32_t p = 0;
            for (uint32_t k = 0; prefix[k] != 0 && p + 1u < (uint32_t)sizeof(full); k++) full[p++] = prefix[k];
            if (p == 0 || full[p - 1] != '/') { if (p + 1u < (uint32_t)sizeof(full)) full[p++] = '/'; }
            for (uint32_t k = 0; page[i].name[k] != 0 && p + 1u < (uint32_t)sizeof(full); k++) full[p++] = page[i].name[k];
            full[p] = 0;
            char ymd[12];
            char hms[10];
            vfs64_time_str64(page[i].mtime, ymd, (int)sizeof(ymd), hms, (int)sizeof(hms));
            dbg64_str("[VFS64] tree ");
            dbg64_str(full);
            dbg64_str(" type=");
            dbg64_str((page[i].type == VFS64_TYPE_DIR) ? "dir" : (page[i].type == VFS64_TYPE_FILE) ? "file" : "?");
            dbg64_str(" size=");
            dbg64_dec(page[i].size);
            dbg64_str(" mtime=0x");
            log_hex32(page[i].mtime);
            dbg64_str(" ymd=");
            dbg64_str(ymd);
            dbg64_str(" hms=");
            dbg64_str(hms);
            dbg64_str(" kind=");
            dbg64_str(vfs64_kind_str64(page[i].kind));
            dbg64_str(" idx=");
            dbg64_dec(page[i].index);
            dbg64_nl();
            ctx->printed++;
            if (page[i].type == VFS64_TYPE_DIR && depth < ctx->max_depth) {
                tree_walk(page[i].index, full, depth + 1, ctx);
                if (ctx->over) return;
            }
        }
    }
}
int vfs64_tree_dump64(const char* path, int max_entries, int max_depth) {
    if (!g_mounted) { log_op_fail("tree", "not mounted"); return -1; }
    if (!path || max_entries <= 0 || max_depth < 0) { log_op_fail("tree", "bad args"); return -1; }
    uint32_t idx = 0;
    if (path_resolve(path, false, &idx, nullptr, nullptr, nullptr) != 0) {
        log_op_fail("tree", "path not found");
        return -1;
    }
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    if (inode_load_ok(idx, ino, "tree") != 0) return -1;
    if (ino[VFS_I_TYPE] != VFS64_TYPE_DIR) { log_op_fail("tree", "not a directory"); return -1; }
    TreeDumpCtx ctx;
    ctx.budget = max_entries;
    ctx.max_depth = max_depth;
    ctx.printed = 0;
    ctx.over = false;
    dbg64_str("[VFS64] tree root=");
    dbg64_str(path);
    dbg64_str(" max_entries=");
    dbg64_dec((uint64_t)max_entries);
    dbg64_str(" max_depth=");
    dbg64_dec((uint64_t)max_depth);
    dbg64_nl();
    tree_walk(idx, path, 0, &ctx);
    if (ctx.over) {
        dbg64_str("[VFS64] tree truncated at budget=");
        dbg64_dec((uint64_t)max_entries);
        dbg64_nl();
    }
    dbg64_str("[VFS64] tree entries=");
    dbg64_dec((uint64_t)ctx.printed);
    dbg64_nl();
    return ctx.printed;
}

// ==================== 状态打印（验收 grep）====================
void vfs64_dump64() {
    dbg64_str("[VFS64] dump mounted=");
    dbg64_str(g_mounted ? "yes" : "no");
    dbg64_str(" slot=");
    dbg64_dec((uint64_t)g_cur_slot);
    dbg64_str(" system_slot=");
    dbg64_dec((uint64_t)g_system_slot);
    dbg64_str(" slots=");
    dbg64_dec((uint64_t)VFS64_SLOT_MAX);
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
    dbg64_str(" version=");
    dbg64_dec(g_lay->version);
    dbg64_str(" inode=");
    dbg64_dec(g_lay->inode_bytes);
    dbg64_str("B");
    dbg64_nl();

    char names[8][VFS64_LS_NAME_BUF];
    uint32_t sizes[8];
    const int n = vfs64_ls("/", names, 8, sizes);
    dbg64_str("[VFS64] dump root entries=");
    if (n < 0) dbg64_str("ERR");
    else dbg64_dec((uint64_t)n);
    dbg64_nl();
    for (int i = 0; i < n && i < 8; i++) {
        Vfs64Info64 in;
        char full[VFS64_NAME_MAX + 2];
        full[0] = '/';
        uint32_t k = 0;
        for (; names[i][k] != 0 && k + 2u < (uint32_t)sizeof(full); k++) full[k + 1] = names[i][k];
        full[k + 1] = 0;
        const bool ok = (vfs64_stat64(full, &in) == 0);
        dbg64_str("[VFS64] dump   ");
        dbg64_str(names[i]);
        dbg64_str(" type=");
        dbg64_str(ok ? ((in.type == VFS64_TYPE_DIR) ? "dir" : "file") : "?");
        dbg64_str(" size=");
        dbg64_dec(sizes[i]);
        dbg64_str(" kind=");
        dbg64_str(vfs64_kind_str64(ok ? in.kind : VFS64_KIND_NONE));
        dbg64_str(" mtime=0x");
        log_hex32(ok ? in.mtime : 0u);
        dbg64_nl();
    }
}

// ==================== 自检（不需要真盘）====================
static uint8_t g_sel_a[4096];        // 自检读写缓冲（4KB 模式，覆盖直接块 + 间接块）
static uint8_t g_sel_b[4096];

// 在 ls 结果里找名字，返回下标（找不到 -1）
static int sel_find(char names[][VFS64_LS_NAME_BUF], int n, const char* want) {
    for (int i = 0; i < n; i++) {
        if (str_eq(names[i], want)) return i;
    }
    return -1;
}
int vfs64_selftest64() {
    int fails = 0;
    Vfs64Geom saved;
    geom_save(&saved);

    // ---- 切到 64 扇区内存假盘（真盘状态在 saved 里，最后恢复）----
    g_fake_active = true;
    ino_cache_invalidate();
    zero_bytes(g_fake_disk, (uint32_t)sizeof(g_fake_disk));
    for (uint32_t i = 0; i < (uint32_t)sizeof(g_sel_a); i++) g_sel_a[i] = (uint8_t)((i * 7u + 3u) & 0xFFu);
    zero_bytes(g_sel_b, (uint32_t)sizeof(g_sel_b));

    char names[8][VFS64_LS_NAME_BUF];
    uint32_t sizes[8];
    uint32_t t = 0;
    uint32_t sz = 0;

    // ---- bit0：格式化 + 挂载（v3）----
    if (vfs64_format(0, 0, VFS64_FAKE_SECTORS) != 0) {
        fails |= 1;
        log_line("fake-disk format FAIL");
    } else {
        Vfs64VolInfo64 vi;
        if (g_lay->version != VFS64_VERSION) { fails |= 1; log_line("fake-disk format version FAIL"); }
        if (vfs64_mount(0, 0) != 0) { fails |= 1; log_line("fake-disk mount FAIL"); }
        else if (vfs64_mounted_volume64(nullptr, nullptr, &vi) != 0 || vi.version != VFS64_VERSION ||
                 vi.inode_bytes != VFS64_INODE_BYTES) {
            fails |= 1;
            log_line("fake-disk mount identity FAIL");
        }
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
        for (int i = 0; i < 32; i++) longname[i] = (char)('a' + (i % 26));
        longname[32] = 0;                                      // 32 > VFS64_NAME_MAX(31)

        if (crc32_64((const uint8_t*)"123456789", 9) != 0xCBF43926u) ok = false;   // CRC 实现漂移
        if (vfs64_mkdir("docs") == 0) ok = false;              // 已存在
        if (vfs64_mkdir("") == 0) ok = false;                  // 空名（= 根目录，已存在）
        if (vfs64_mkdir("/") == 0) ok = false;                 // 根目录
        if (vfs64_unlink("docs") == 0) ok = false;             // 目录不可删（unlink 拒绝）
        if (vfs64_read("docs", g_sel_b, 16) != -1) ok = false; // 读目录
        if (vfs64_ls("beta.bin", names, 4, sizes) != -1) ok = false;   // 对文件 ls
        if (vfs64_ls("/", names, 0, sizes) != -1) ok = false;          // max=0
        if (vfs64_write("x/y", g_sel_a, 8) != -1) ok = false;          // 父目录 /x 不存在
        if (vfs64_write(longname, g_sel_a, 8) != -1) ok = false;       // 名字过长（32 > 31）
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
        // 坏超级块 3：版本号改成不认识的 9 -> 必须拒绝（不是"随便啥都认"）
        if (dev_read(g_start, 1, g_sel_b)) {
            const uint32_t v0 = rd32(g_sel_b + VFS_O_VERSION);
            wr32(g_sel_b + VFS_O_VERSION, 9u);
            wr32(g_sel_b + VFS_O_CRC, crc32_64(g_sel_b, VFS_SB_CRC_LEN));
            if (dev_write(g_start, 1, g_sel_b)) {
                if (vfs64_mount(0, 0) == 0) ok = false;
                wr32(g_sel_b + VFS_O_VERSION, v0);
                wr32(g_sel_b + VFS_O_CRC, crc32_64(g_sel_b, VFS_SB_CRC_LEN));
                if (!dev_write(g_start, 1, g_sel_b)) ok = false;
            } else ok = false;
        } else ok = false;
        if (vfs64_mount(0, 0) != 0) ok = false;

        // ★ 下面两条注入会改写 inode 表的**第一个扇区**（槽 0 = 根目录也在这块里）。
        //   所以先把整扇区备份下来，注入测试做完**原样写回** —— 否则根目录 inode 被抹掉，
        //   后面的 tree/path 自检就全废了（这是实测踩过的坑）。
        uint8_t ino_sec_backup[VFS64_SECTOR_BYTES];
        bool have_ino_backup = dev_read(g_start + g_inode_start, 1, ino_sec_backup);
        if (!have_ino_backup) ok = false;

        // 伪造"数据块指针越界"的 inode（槽 1，CRC 是对的）：read 必须拒绝，绝不越界读
        if (dev_read(g_start + g_inode_start, 1, g_sel_b)) {
            uint8_t* bad = g_sel_b + VFS64_INODE_BYTES;        // v3：槽 1 在扇区内偏移 128
            zero_bytes(bad, VFS64_INODE_BYTES);
            bad[VFS_I_TYPE] = (uint8_t)VFS64_TYPE_FILE;
            bad[VFS_I_NAMELEN] = 4;
            for (uint32_t i = 0; i < 4; i++) bad[VFS_I3_NAME + i] = (uint8_t)("hack"[i]);
            wr32(bad + VFS_I_SIZE, 512);
            wr32(bad + VFS_I3_MTIME, vfs64_pack_time64(2025, 1, 1, 0, 0, 0));
            wr16(bad + VFS_I3_NLINK, 1);
            bad[VFS_I3_KIND] = (uint8_t)VFS64_KIND_BIN;
            wr32(bad + VFS_I_D0, g_blocks + 7);                // 越界块号
            wr32(bad + VFS_LAY_V3.crc_off, crc32_64(bad, VFS_LAY_V3.crc_off));
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
            for (uint32_t i = 0; i < 5; i++) bad[VFS_I3_NAME + i] = (uint8_t)("hack2"[i]);
            wr32(bad + VFS_I_SIZE, 128);
            wr16(bad + VFS_I3_NLINK, 1);
            wr32(bad + VFS_LAY_V3.crc_off, 0x12345678u);       // 故意写错
            if (dev_write(g_start + g_inode_start, 1, g_sel_b)) {
                if (vfs64_stat("hack2", &t, &sz) == 0) ok = false;
                zero_bytes(g_sel_b, VFS64_SECTOR_BYTES);
                if (!dev_write(g_start + g_inode_start, 1, g_sel_b)) ok = false;
            } else ok = false;
        } else ok = false;

        // 把注入期间被改写的 inode 扇区恢复原样（根目录 inode 必须回到盘上）
        if (have_ino_backup && !dev_write(g_start + g_inode_start, 1, ino_sec_backup)) ok = false;
        if (vfs64_stat("/", &t, &sz) != 0 || t != VFS64_TYPE_DIR) ok = false;   // 根目录必须还在

        // 分区太小：format 必须直接拒绝（且不能改坏当前卷）
        if (vfs64_format(0, 0, VFS64_MIN_BLOCKS - 1u) == 0) ok = false;
        if (vfs64_mount(0, 0) != 0) ok = false;                // 当前卷还在
        if (!ok) fails |= 32;
    }

    // ---- bit8：多级目录树（v3 的核心）----
    if (!(fails & 1)) {
        bool ok = true;
        Vfs64Info64 in;
        Vfs64Dirent64 d[16];
        uint32_t cur = 0;

        if (vfs64_mkdir64("/efi") != 0) ok = false;
        if (vfs64_mkdir64("/apps") != 0) ok = false;
        if (vfs64_mkdir64("/apps/demo") != 0) ok = false;
        if (vfs64_mkdir64("/nope/x") == 0) ok = false;         // 父目录不存在：必须被拒
        if (vfs64_mkdir64("/apps") == 0) ok = false;           // 已存在：必须被拒
        if (vfs64_write64("/apps/demo/a.txt", "hello tree", 10) != 10) ok = false;

        // 遍历 /：必须能看到 apps 与 efi，且类型 = 目录
        const int n1 = vfs64_list64("/", d, 16, &cur);
        bool saw_apps = false, saw_efi = false;
        for (int i = 0; i < (n1 < 0 ? 0 : n1); i++) {
            if (str_eq(d[i].name, "apps") && d[i].type == VFS64_TYPE_DIR && d[i].kind == VFS64_KIND_DIR) saw_apps = true;
            if (str_eq(d[i].name, "efi")  && d[i].type == VFS64_TYPE_DIR)                         saw_efi = true;
        }
        if (n1 < 4 || !saw_apps || !saw_efi) ok = false;

        // 分页游标：一次只要 1 条，也能把 4+ 条走完（且不重不漏）
        cur = 0;
        int paged = 0;
        for (;;) {
            const int one = vfs64_list64("/", d, 1, &cur);
            if (one < 0) { ok = false; break; }
            if (one == 0) break;
            paged++;
            if (paged > 64) { ok = false; break; }
        }
        if (paged != n1) ok = false;

        // 遍历 /apps/demo：只能有 a.txt（文件，10 字节）
        cur = 0;
        const int n2 = vfs64_list64("/apps/demo", d, 8, &cur);
        if (n2 != 1 || !str_eq(d[0].name, "a.txt") || d[0].type != VFS64_TYPE_FILE || d[0].size != 10) ok = false;

        // stat64：类型/大小/父目录/名字
        Vfs64Info64 demo;
        const bool have_demo = (vfs64_stat64("/apps/demo", &demo) == 0);
        if (!have_demo || demo.type != VFS64_TYPE_DIR || demo.kind != VFS64_KIND_DIR) ok = false;
        if (vfs64_stat64("/apps/demo/a.txt", &in) != 0) ok = false;
        else {
            if (in.type != VFS64_TYPE_FILE || in.size != 10) ok = false;
            if (!have_demo || in.parent != demo.index) ok = false;
            if (!str_eq(in.name, "a.txt")) ok = false;
        }
        if (vfs64_stat64("/apps", &in) != 0) ok = false;
        else if (in.nlink < 2) ok = false;                     // 有一个子目录 demo -> nlink >= 2

        // opendir / readdir / closedir：单条游标也要能走完
        int h = -1;
        if (vfs64_opendir64("/apps/demo", &h) != 0) ok = false;
        else {
            int got = 0;
            Vfs64Dirent64 e;
            for (;;) {
                const int r = vfs64_readdir64(h, &e);
                if (r < 0) { ok = false; break; }
                if (r == 0) break;
                got++;
                if (!str_eq(e.name, "a.txt")) ok = false;
            }
            if (got != 1) ok = false;
            if (vfs64_closedir64(h) != 0) ok = false;
            if (vfs64_readdir64(h, &e) == 1) ok = false;       // 关闭后再读必须失败/结束
        }
        dbg64_str("[VFS64] fake-disk tree root_entries=");
        dbg64_dec((uint64_t)((n1 < 0) ? 0 : n1));
        dbg64_str(" demo_entries=");
        dbg64_dec((uint64_t)((n2 < 0) ? 0 : n2));
        dbg64_nl();
        if (!ok) fails |= 256;
    }

    // ---- bit9：路径语义（'.' / '..' / 冗余分隔符 / 超长段 / 超深 / 非法字符）----
    if (!(fails & 1)) {
        bool ok = true;
        Vfs64Info64 in;
        uint32_t apps_idx = 0xFFFFFFFFu;
        if (vfs64_stat64("/apps", &in) != 0) ok = false;
        else apps_idx = in.index;
        if (vfs64_stat64("/apps/.", &in) != 0 || in.type != VFS64_TYPE_DIR) ok = false;
        if (vfs64_stat64("/apps/demo/..", &in) != 0 || in.index != apps_idx) ok = false;
        if (vfs64_stat64("//apps///demo//", &in) != 0 || in.type != VFS64_TYPE_DIR) ok = false;
        if (vfs64_stat64("apps/demo/a.txt", &in) != 0 || in.size != 10) ok = false;      // 相对 = 从根开始
        if (vfs64_stat64("", &in) != 0 || in.index != 0) ok = false;                    // "" = 根
        if (vfs64_stat64("/../..", &in) != 0 || in.index != 0) ok = false;              // POSIX：根的父亲还是根
        if (vfs64_write64("/../..", "x", 1) != -1) ok = false;                          // 对根写文件：拒绝
        if (vfs64_mkdir64("/../..") != -1) ok = false;                                  // 对根建目录：拒绝
        if (vfs64_unlink64("/../..") != -1) ok = false;                                 // 删根：拒绝
        // 超长段（32 > 31）
        char longname[VFS64_NAME_MAX + 8];
        for (uint32_t i = 0; i < VFS64_NAME_MAX + 1u; i++) longname[i] = 'z';
        longname[VFS64_NAME_MAX + 1u] = 0;
        if (vfs64_stat64(longname, &in) == 0) ok = false;
        if (vfs64_mkdir64(longname) == 0) ok = false;
        // 超深路径（17 段 > VFS64_PATH_DEPTH_MAX = 16）
        char deep[VFS64_PATH_MAX + 1];
        uint32_t p = 0;
        for (uint32_t i = 0; i < VFS64_PATH_DEPTH_MAX + 1u; i++) { deep[p++] = '/'; deep[p++] = 'a'; }
        deep[p] = 0;
        if (vfs64_stat64(deep, &in) == 0) ok = false;
        // 非法字符（控制字符）与垃圾路径
        static const char bad1[] = { '/', 'a', 0x01, 'b', 0 };
        if (vfs64_stat64(bad1, &in) == 0) ok = false;
        if (vfs64_stat64("/apps/demo/a.txt/", &in) != 0) ok = false;   // 结尾 '/' 忽略 -> 仍能找到文件
        if (!ok) fails |= 512;
    }

    // ---- bit10：mtime 与类型判定 ----
    if (!(fails & 1)) {
        bool ok = true;
        Vfs64Info64 in;
        if (vfs64_stat64("/apps/demo/a.txt", &in) != 0) ok = false;
        else {
            if (in.mtime == 0) ok = false;                      // 必须记住时间
            Vfs64Time64 tm;
            vfs64_unpack_time64(in.mtime, &tm);
            if (tm.year < 2000 || tm.year > 2063) ok = false;
            if (tm.month < 1 || tm.month > 12 || tm.day < 1 || tm.day > 31) ok = false;
            if (tm.hour > 23 || tm.minute > 59 || tm.second > 59) ok = false;
            if (in.kind != VFS64_KIND_TEXT) ok = false;         // "hello tree" + .txt -> 文本
        }
        // 打包/解包往返 + 越界拒绝
        const uint32_t pk = vfs64_pack_time64(2025, 12, 31, 23, 59, 58);
        Vfs64Time64 t2;
        vfs64_unpack_time64(pk, &t2);
        if (t2.year != 2025 || t2.month != 12 || t2.day != 31 || t2.hour != 23 || t2.minute != 59 || t2.second != 58) ok = false;
        if (vfs64_pack_time64(1999, 1, 1, 0, 0, 0) != 0) ok = false;
        if (vfs64_pack_time64(2025, 13, 1, 0, 0, 0) != 0) ok = false;
        if (vfs64_pack_time64(2025, 1, 32, 0, 0, 0) != 0) ok = false;
        if (vfs64_now64() == 0) ok = false;                     // 绝不返回 0
        // 类型判定（纯函数）
        static const char vap_head[13] = { 'V','A','P','6','4',0,0,0,'r','e','s','t',0 };
        if (vfs64_kind_of_data64(vap_head, 12, "x.vap", 5) != VFS64_KIND_VAP) ok = false;
        static const uint8_t elf_head[16] = { 0x7F,'E','L','F', 2,1,1,0, 0,0,0,0, 0,0,0,0 };
        if (vfs64_kind_of_data64(elf_head, 16, "x", 1) != VFS64_KIND_ELF) ok = false;
        if (vfs64_kind_of_data64("plain text\n", 11, "x", 1) != VFS64_KIND_TEXT) ok = false;
        static const uint8_t bin_head[8] = { 0x00,0x01,0x02,0x03,0xFF,0xFE,0x00,0x10 };
        if (vfs64_kind_of_data64(bin_head, 8, "x.bin", 5) != VFS64_KIND_BIN) ok = false;
        if (vfs64_kind_by_name64(VFS64_TYPE_DIR, "d", 1) != VFS64_KIND_DIR) ok = false;
        if (vfs64_kind_by_name64(VFS64_TYPE_FILE, "a.elf", 5) != VFS64_KIND_ELF) ok = false;
        // 写一个 ELF 头文件 -> kind 必须落盘成 ELF
        if (vfs64_write64("/apps/p.elf", elf_head, 16) != 16) ok = false;
        else if (vfs64_stat64("/apps/p.elf", &in) != 0 || in.kind != VFS64_KIND_ELF) ok = false;
        if (vfs64_unlink64("/apps/p.elf") != 0) ok = false;
        if (!ok) fails |= 1024;
    }

    // ---- bit11：代际/几何 + 重挂载持久化 + 空目录删除 ----
    if (!(fails & 1)) {
        bool ok = true;
        // 1) 超级块代际/几何断言（v3 + 128B inode + CRC + 55AA）
        if (dev_read(g_start, 1, g_sel_b)) {
            if (rd32(g_sel_b + VFS_O_VERSION) != VFS64_VERSION) ok = false;
            if (rd32(g_sel_b + VFS_O_INOSZ) != VFS64_INODE_BYTES) ok = false;
            if (rd32(g_sel_b + VFS_O_INODEN) == 0 || rd32(g_sel_b + VFS_O_INODEN) > VFS64_MAX_INODES) ok = false;
            if (rd32(g_sel_b + VFS_O_CRC) != crc32_64(g_sel_b, VFS_SB_CRC_LEN)) ok = false;
            if (g_sel_b[VFS_O_SIG] != 0x55 || g_sel_b[VFS_O_SIG + 1] != 0xAA) ok = false;
            const uint32_t bmn = rd32(g_sel_b + VFS_O_BITMAPN);
            const uint32_t ino_b = rd32(g_sel_b + VFS_O_INODE);
            const uint32_t datab = rd32(g_sel_b + VFS_O_DATA);
            const uint32_t total = rd32(g_sel_b + VFS_O_TOTAL);
            if (bmn != (total + VFS64_BITMAP_BLK_BITS - 1u) / VFS64_BITMAP_BLK_BITS) ok = false;
            if (ino_b != 1u + bmn) ok = false;
            if (datab != ino_b + (rd32(g_sel_b + VFS_O_INODEN) + VFS64_INODES_PER_BLK - 1u) / VFS64_INODES_PER_BLK) ok = false;
            if (rd32(g_sel_b + VFS_O_DATAN) != total - datab) ok = false;
        } else ok = false;
        // 2) 重新挂载（假盘内容保留）-> 子目录里的文件还在，且 mtime/inode 号不变
        Vfs64Info64 before;
        Vfs64Info64 after;
        const bool had = (vfs64_stat64("/apps/demo/a.txt", &before) == 0);
        if (!had || before.mtime == 0) ok = false;
        if (vfs64_mount(0, 0) != 0) ok = false;
        if (vfs64_stat64("/apps/demo/a.txt", &after) != 0) ok = false;
        else if (after.index != before.index || after.mtime != before.mtime || after.size != before.size) ok = false;
        // 3) 删除：文件 + 空目录（非空目录必须被拒）
        if (vfs64_unlink64("/apps/demo/a.txt") != 0) ok = false;
        if (vfs64_stat64("/apps/demo/a.txt", &after) == 0) ok = false;
        if (vfs64_rmdir64("/apps") == 0) ok = false;           // 非空（还有 demo）
        if (vfs64_rmdir64("/apps/demo") != 0) ok = false;      // 空了可以删
        if (vfs64_rmdir64("/apps") != 0) ok = false;
        if (vfs64_rmdir64("/") == 0) ok = false;               // 根目录不可删
        if (vfs64_rmdir64("/efi") != 0) ok = false;            // 空目录可以删
        if (vfs64_stat64("/efi", &after) == 0) ok = false;
        log_line("fake-disk tree/path/time/remount checks done");
        if (!ok) fails |= 2048;
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
        char nm[8][VFS64_LS_NAME_BUF];
        uint32_t szs[8];
        const int n = vfs64_ls("/", nm, 8, szs);
        const int s = vfs64_stat("/", &t, &sz);
        Vfs64Info64 ri;
        const int s64 = vfs64_stat64("/", &ri);
        if (n < 0 || s != 0 || t != VFS64_TYPE_DIR || s64 != 0) {
            fails |= 128;
            log_line("real-disk probe FAIL");
        } else {
            dbg64_str("[VFS64] real-disk probe ok entries=");
            dbg64_dec((uint64_t)n);
            dbg64_str(" version=");
            dbg64_dec((uint32_t)(g_lay->version));
            dbg64_str(" inode=");
            dbg64_dec((uint32_t)(g_lay->inode_bytes));
            dbg64_str("B root_nlink=");
            dbg64_dec(ri.nlink);
            dbg64_str(" mtime=0x");
            log_hex32(ri.mtime);
            dbg64_nl();
        }
    }

    // ---- bit12（4096）：★ 多卷 —— 卷槽表 / 切换 / 卷身份缓存键控 / on64 守卫 / 表满拒绝 ----
    // 做法：在**内存假盘**上造两个独立小卷（同一驱动器号、不同起始 LBA：A@0、B@32 各 32 扇区）。
    // 同一块盘不同 LBA 正好是"只按 LBA 键控会串卷"的最坏情形 —— 读 B 的 inode 必须拿到 B 的字节。
    // 位置：放在真盘只读探测**之后**（bit7 已经跑完），跑完把槽 0 的真卷现场原样恢复。
    {
        Vfs64Geom saved_mv;
        geom_save(&saved_mv);                       // 当前槽（真系统卷 / 无卷）现场
        const int save_cur = g_cur_slot;
        const int save_sys = g_system_slot;
        g_fake_active = true;
        bool ok = true;
        int mv_step = 0;
        char rb[8];

        // --- 槽 0：卷 A（LBA 0，32 扇区）；槽 1：卷 B（LBA 32，32 扇区）---
        g_cur_slot = 0;
        if (vfs64_format(0, 0, 32) != 0) { mv_step = __LINE__; ok = false; }
        if (vfs64_write("/a.txt", "AAAA", 4) != 4) { mv_step = __LINE__; ok = false; }          // ★ A 卷里的文件（后面逐字节比对）
        g_cur_slot = 1;
        if (vfs64_format(0, 32, 32) != 0) { mv_step = __LINE__; ok = false; }
        if (vfs64_write("/b.txt", "BBBB", 4) != 4) { mv_step = __LINE__; ok = false; }

        if (vfs64_slot_used64(0) != 1 || vfs64_slot_used64(1) != 1) { mv_step = __LINE__; ok = false; }
        if (vfs64_slot_find64(0, 0) != 0 || vfs64_slot_find64(0, 32) != 1) { mv_step = __LINE__; ok = false; }
        Vfs64VolInfo64 vi;
        if (vfs64_slot_info64(1, nullptr, nullptr, &vi) != 0 || vi.blocks != 32) { mv_step = __LINE__; ok = false; }

        // --- 切到 A：只能看到 A 的文件（卷独立 + 缓存不串卷）---
        if (vfs64_activate_slot64(0) != 0) { mv_step = __LINE__; ok = false; }
        if (vfs64_read("a.txt", rb, 8) != 4 || cmp_bytes(rb, "AAAA", 4) != 0) { mv_step = __LINE__; ok = false; }
        if (vfs64_read("b.txt", rb, 8) != -1) { mv_step = __LINE__; ok = false; }

        // --- 切到 B：读 b.txt 必须拿到 B 的 inode（这一步专门抓"缓存没带卷身份"的 bug）---
        if (vfs64_activate_slot64(1) != 0) { mv_step = __LINE__; ok = false; }
        if (vfs64_current_slot64() != 1) { mv_step = __LINE__; ok = false; }
        if (vfs64_read("b.txt", rb, 8) != 4 || cmp_bytes(rb, "BBBB", 4) != 0) { mv_step = __LINE__; ok = false; }
        if (vfs64_read("a.txt", rb, 8) != -1) { mv_step = __LINE__; ok = false; }
        if (vfs64_write("/onlyB.txt", "B2", 2) != 2) { mv_step = __LINE__; ok = false; }
        if (vfs64_mkdir64("/dirB") != 0) { mv_step = __LINE__; ok = false; }

        // --- 回 A：A 的内容还在，且 B 的文件在 A 上不存在（双卷独立）---
        if (vfs64_activate_slot64(0) != 0) { mv_step = __LINE__; ok = false; }
        if (vfs64_read("a.txt", rb, 8) != 4 || cmp_bytes(rb, "AAAA", 4) != 0) { mv_step = __LINE__; ok = false; }
        if (vfs64_stat("onlyB.txt", nullptr, nullptr) == 0) { mv_step = __LINE__; ok = false; }
        if (vfs64_stat64("/dirB", nullptr) == 0) { mv_step = __LINE__; ok = false; }
        { Vfs64Info64 tmpi; if (vfs64_stat64("/dirB", &tmpi) == 0) { mv_step = __LINE__; ok = false; } }   // 参数合法：测的是"不存在"
        // --- on64（显式槽）在切到 B 之后按槽 0 读 A：读完**当前卷必须还是 B**（守卫 LIFO 切回）---
        if (vfs64_activate_slot64(1) != 0) { mv_step = __LINE__; ok = false; }
        zero_bytes(rb, sizeof(rb));
        if (vfs64_read_on64(0, "a.txt", rb, 8) != 4 || cmp_bytes(rb, "AAAA", 4) != 0) { mv_step = __LINE__; ok = false; }
        if (vfs64_current_slot64() != 1) { mv_step = __LINE__; ok = false; }                    // ★ 没被 on64 改掉
        if (vfs64_read_on64(0, "b.txt", rb, 8) != -1) { mv_step = __LINE__; ok = false; }       // A 上没有 b.txt
        if (vfs64_current_slot64() != 1) { mv_step = __LINE__; ok = false; }
        if (vfs64_stat_on64(1, "/onlyB.txt", nullptr, nullptr) != 0) { mv_step = __LINE__; ok = false; }
        if (vfs64_current_slot64() != 1) { mv_step = __LINE__; ok = false; }

        // --- 槽表满（4 槽全占）与非法槽号：如实拒绝、不崩、不覆盖已有卷 ---
        if (vfs64_mount_slot64(2, 0, 0) != 0) { mv_step = __LINE__; ok = false; }               // 同一个卷挂进别的槽（只读语义）
        if (vfs64_mount_slot64(3, 0, 32) != 0) { mv_step = __LINE__; ok = false; }
        if (vfs64_slot_alloc64() != -1) { mv_step = __LINE__; ok = false; }                     // 表满 -> 必须 -1
        if (vfs64_mount_slot64((int)VFS64_SLOT_MAX, 0, 0) == 0) { mv_step = __LINE__; ok = false; }
        if (vfs64_activate_slot64((int)VFS64_SLOT_MAX) == 0) { mv_step = __LINE__; ok = false; }
        if (vfs64_activate_slot64(2) != 0) { mv_step = __LINE__; ok = false; }                  // 已挂载的槽能激活
        if (vfs64_read("a.txt", rb, 8) != 4) { mv_step = __LINE__; ok = false; }                // 槽 2 = 卷 A
        if (vfs64_read_on64(99, "a.txt", rb, 8) != -1) { mv_step = __LINE__; ok = false; }      // 非法槽号：负返回 + 打点
        if (vfs64_current_slot64() != 2) { mv_step = __LINE__; ok = false; }                    // 非法槽号也不改当前卷

        // --- 收尾：清掉自检用的槽 1..3，恢复当前槽与系统卷槽现场 ---
        for (int s = 1; s < (int)VFS64_SLOT_MAX; s++) g_vol[s] = Vfs64Geom();
        g_cur_slot = save_cur;
        geom_restore(&saved_mv);
        g_system_slot = save_sys;
        g_fake_active = false;
        dbg64_str("[VFS64] multivol selftest ");
        dbg64_str(ok ? "ok" : "FAIL");
        dbg64_nl();
        if (!ok) fails |= 4096;
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
