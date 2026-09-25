// vfs64.cpp - VimtuFS2 实现（v3：真正的目录树 + inode 时间戳 + 类型判定；v2 旧卷仍可挂载）
//
// 磁盘布局（相对分区起始 LBA，块号从 0 开始；1 块 = 1 扇区 = 512B）：
//   块 0              : 超级块（见下面 VFS_O_* 偏移；末尾 0xAA55；CRC32 自校验）
//   块 1 .. 1+bmn-1   : 空闲块位图（1 = 已用，额外把"分区外"的位也置 1，分配器永不发放）
//   块 bp .. bp+ibn-1 : inode 表（v3 = 128B/个 4 个/块、v2 = 64B/个 8 个/块；inode 0 = 根目录）
//   块 dp .. total-1  : 数据区（文件内容 + 一级/二级间接块；单文件最大 = 8 MiB，见 vfs64.h）
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
static const uint32_t VFS_I_NAMELEN = 1;     // u8  名字长度（v2 ≤27 / v3/v4 ≤31；0 仅用于根目录）
static const uint32_t VFS_I_RSVD    = 2;     // u16 保留（0）
static const uint32_t VFS_I_SIZE    = 4;     // u32 文件字节数
static const uint32_t VFS_I_D0      = 8;     // u32 直接块 0..3（偏移 8/12/16/20）
static const uint32_t VFS_I_IND     = 24;    // u32 一级间接块（128 个数据块号）
static const uint32_t VFS_I_PARENT  = 28;    // u32 父目录 inode 号（根 = 自己 = 0）
// v3/v4 追加字段（v2 里 32..59 是名字、60 是 CRC）：
static const uint32_t VFS_I3_MTIME  = 32;    // u32 打包时间戳
static const uint32_t VFS_I3_NLINK  = 36;    // u16 链接数
static const uint32_t VFS_I3_KIND   = 38;    // u8  类型判定缓存
static const uint32_t VFS_I3_RSVD2  = 39;    // u8  保留（0）
static const uint32_t VFS_I3_NAME   = 40;    // 31B 名字
// ★ 批次 M：v3 inode 的 `dind`（二级间接块指针）= **原保留区首 4 字节（偏移 71）**；保留区缩到 [75,124)。
//    v2 卷没有这个字段（它的 32..59 是名字、60 是 CRC）→ v2 卷单文件上限仍是 67584 B。
static const uint32_t VFS_I3_DIND   = 71;    // u32 二级间接块（0 = 没有）
// ★ P4：v4 再把保留区首 6 字节变成 uid/gid/mode（v3 卷这 6 字节**必须为 0** -> 读作默认 root/0755/0644）。
//    与 vfs64.h 的 VFS64_INO_*_OFF 是同一处定义（下面 static_assert 钉死）。
static const uint32_t VFS_I4_UID    = 75;    // u16 属主 uid
static const uint32_t VFS_I4_GID    = 77;    // u16 属主 gid
static const uint32_t VFS_I4_MODE   = 79;    // u16 类型位 + 权限位（S_IF*|0777）
static const uint32_t VFS_I4_RSVD3  = 81;    // 保留区起点 [81,124) 必须全 0
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
    uint32_t dind_off;         // ★ 批次 M：二级间接块指针偏移（0 = 该版本没有这一层）
    uint32_t uid_off;          // ★ P4：uid 偏移（0 = 该版本没有权限字段 -> 拦截关闭）
    uint32_t gid_off;          // ★ P4：gid 偏移（0 = 没有）
    uint32_t mode_off;         // ★ P4：mode 偏移（0 = 没有）
};
static constexpr Vfs64Layout VFS_LAY_V2 = { VFS64_VERSION_V2, 64u,  8u,
                                            VFS_I2_NAME, VFS64_NAME_MAX_V2, 0u, 0u, 0u, VFS_I2_CRC, 0u,
                                            0u, 0u, 0u };
// v3：与 v4 **inode 大小/每块个数/名字/CRC 全同**，只有权限三字段缺失（保留区是 [75,124)）
static constexpr Vfs64Layout VFS_LAY_V3 = { VFS64_VERSION_V3, 128u, 4u,
                                            VFS_I3_NAME, VFS64_NAME_MAX, VFS_I3_MTIME, VFS_I3_NLINK,
                                            VFS_I3_KIND, 124u, VFS_I3_DIND,
                                            0u, 0u, 0u };
// v4：当前格式化产出（权限字段在 75/77/79；保留区 [81,124)）
static constexpr Vfs64Layout VFS_LAY_V4 = { VFS64_VERSION,    128u, 4u,
                                            VFS_I3_NAME, VFS64_NAME_MAX, VFS_I3_MTIME, VFS_I3_NLINK,
                                            VFS_I3_KIND, 124u, VFS_I3_DIND,
                                            VFS_I4_UID, VFS_I4_GID, VFS_I4_MODE };

static_assert(VFS64_BLOCK_BYTES == VFS64_SECTOR_BYTES, "块就是扇区（vfs64_format 也按这个算几何）");
static_assert(VFS64_INODES_PER_BLK * VFS64_INODE_BYTES == VFS64_BLOCK_BYTES, "v3/v4：每块必须正好 4 个 inode");
static_assert(VFS64_INODES_PER_BLK_V2 * VFS64_INODE_BYTES_V2 == VFS64_BLOCK_BYTES, "v2：每块必须正好 8 个 inode");
static_assert(VFS64_BITMAP_BLK_BITS == VFS64_BLOCK_BYTES * 8u, "位图块 512B = 4096 个块位");
static_assert(VFS_I3_DIND + 4u <= VFS_I4_UID, "v4 的 dind 必须在 uid 之前（不越界）");
static_assert(VFS_I4_MODE + 2u <= VFS_I4_RSVD3, "v4 的 mode 必须在保留区之前");
static_assert(VFS_I4_RSVD3 < 124u, "v4 inode 保留区必须在 CRC 之前");
static_assert(VFS_LAY_V4.crc_off + 4u == 128u, "v4 inode 字段必须正好铺满 128B");
static_assert(VFS_LAY_V3.crc_off + 4u == 128u, "v3 inode 字段必须正好铺满 128B");
static_assert(VFS_LAY_V2.crc_off + 4u == 64u, "v2 inode 字段必须正好铺满 64B");
static_assert(VFS_LAY_V3.dind_off == VFS_I3_DIND && VFS_LAY_V4.dind_off == VFS_I3_DIND, "dind 在 v3/v4 都存在");
static_assert(VFS_LAY_V2.dind_off == 0u && VFS_LAY_V2.uid_off == 0u, "v2 没有 dind / 权限字段");
static_assert(VFS_LAY_V3.uid_off == 0u && VFS_LAY_V3.mode_off == 0u, "★ v3 没有权限字段（拦截关闭的依据）");
static_assert(VFS_LAY_V4.uid_off == VFS64_INO_UID_OFF && VFS_LAY_V4.gid_off == VFS64_INO_GID_OFF &&
              VFS_LAY_V4.mode_off == VFS64_INO_MODE_OFF && VFS_LAY_V4.uid_off == VFS_I4_UID,
              "v4 权限字段偏移必须与 vfs64.h 逐字节一致");
static_assert(VFS64_L2_FIRST_BLOCK == 132u, "第一个走二级间接的 fs 块必须是 132（4 直接 + 128 一级）");
static_assert(VFS64_MAX_MAP_BLOCKS == 16516u, "映射能力 = 4 + 128 + 128*128");
static_assert(VFS64_MAX_FILE_BYTES == 8388608u, "★ 单文件上限 = 8 MiB（16384 块）");
static_assert(VFS64_MAX_FILE_BYTES <= VFS64_MAX_MAP_BLOCKS * VFS64_BLOCK_BYTES, "上限不能超过映射能力");
static_assert(VFS64_MAX_FILE_BYTES_V2 == 67584u, "v2 卷上限 = 4 + 128 块（与 vfs64.h 的描述一致）");
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
static uint32_t g_ptrs[VFS64_INDIRECT_PTRS];          // 释放/搬运时用的 128 个块号
static uint8_t  g_ino_cache[VFS64_SECTOR_BYTES];      // inode 表"当前扇区"缓存（扫描时少读盘）
// ★ 批次 M：大文件（二级间接）用的工作镜像与记账（都只在一次文件操作内有效，操作串行、不重入）
static uint8_t  g_l1[VFS64_SECTOR_BYTES];             // 一级间接块镜像（inode->ind）
static uint8_t  g_l2[VFS64_SECTOR_BYTES];             // 二级间接块镜像（inode->dind）
static uint8_t  g_l3[VFS64_SECTOR_BYTES];             // 二级间接块里"当前子块"的镜像
static uint32_t g_rm_ind[VFS64_INDIRECT_PTRS];        // 只读路径：一级间接块的 128 个块号
static uint32_t g_rm_dind[VFS64_INDIRECT_PTRS];       // 只读路径：二级间接块的 128 个子块号
static uint32_t g_rm_child[VFS64_INDIRECT_PTRS];      // 只读路径：当前子块的 128 个块号
// 本次操作**新分配**的块号（失败回滚用）。上限 = 映射能力 + 间接块数 + 余量：
// 一次调用最多把整个文件链建起来（write_stream 整文件重写），所以按最坏情况开表。
static uint32_t g_newblk[VFS64_MAX_MAP_BLOCKS + VFS64_DIND_CHILDREN + 16u];
static uint32_t g_newn = 0;
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

static bool inode_load(uint32_t idx, uint8_t* out);          // 前置声明（perm_check_idx64 要用）
// ==================== ★ P4：凭证（credentials）+ 权限判定 ====================
// 为什么是"全局一份"而不是给每个 API 加参数：
//   本文件里 40+ 个公开入口、上百处内部调用，加参数会把改动面铺满整棵树（漏一处就是权限洞）。
//   凭证只有两个来源（会话身份 / 进程身份），且都在**上下文切换点**变化，所以收在一处：
static const uint32_t VFS64_PERM_LOG_MAX = 128u;               // 上限放大一点：长会话（GUI+终端）也够用
//     * ring3 进程 —— proc64 记录每进程凭证，任务切换时调 vfs64_set_proc_cred64()
//       （proc64 的钩子由 kernel/task64.cpp 的 task_apply_ctx64 调用；切回内核线程 → have=0 恢复会话身份）；
//     * 其它（启动早期、安装介质内核、系统组件）—— 保持默认 root（0/0/0/0）。
//   判定只在**当前卷的布局有权限字段（v4）**时生效：v2/v3 旧卷没有字段，拦截关闭（见 mount 的 legacy 打点）。
static Vfs64Cred64 g_cred      = { 0, 0, 0, 0 };                 // 当前生效凭证（默认 = root）
static Vfs64Cred64 g_sess_cred = { 0, 0, 0, 0 };                 // 会话身份（进程身份退出后回到它）
static uint32_t    g_umask64   = VFS64_UMASK_DEFAULT;
static uint32_t    g_perm_denies = 0;                            // [PERM64] deny 行计数（有上限，防刷屏）
// ★ P4：**凭证覆盖**（_on64 的"临时 root"）。为什么需要单独一套状态：
//   _on64 的调用体内可能被 PIT 抢占换任务；任务切换钩子（vfs64_set_proc_cred64）会把凭证刷成
//   被调度任务的凭证（任务 0 -> 会话身份）。若不管，覆盖就在半途丢失（实测：useradd 的 mkdir
//   会被自己的权限检查拒掉）。做法：记录"覆盖归谁"（任务 id）与覆盖值；切换回来的是覆盖者时
//   重新装上覆盖值，别的任务照常按自己的身份走；覆盖退出时把进入前的凭证原样还回。
static uint32_t    g_cred_hold_depth = 0;
static uint32_t    g_cred_hold_owner = 0xFFFFFFFFu;              // 覆盖者的任务 id（无 task64 = 不追踪）
static Vfs64Cred64 g_cred_hold_cred  = { 0, 0, 0, 0 };           // 覆盖期间的凭证（root）
extern "C" uint32_t task_current_id_64() __attribute__((weak));  // 安装介质内核没有调度器（弱引用）
// 打点：固定打印 4 位八进制（"0644"），与 ls -l / 报告口径一致
static void log_octal4(uint32_t v) {
    char b[5];
    b[0] = (char)('0' + ((v >> 9) & 7u));
    b[1] = (char)('0' + ((v >> 6) & 7u));
    b[2] = (char)('0' + ((v >> 3) & 7u));
    b[3] = (char)('0' + (v & 7u));
    b[4] = 0;
    dbg64_str(b);
}
// 当前卷是否**开启**权限判定（v4 卷；v2/v3 旧卷没有 uid/gid/mode 字段 -> 关闭）
static bool perm_enforced64() { return g_mounted && g_lay->mode_off != 0; }

void vfs64_set_cred64(uint32_t uid, uint32_t gid, uint32_t euid, uint32_t egid) {
    g_sess_cred.uid = uid; g_sess_cred.gid = gid; g_sess_cred.euid = euid; g_sess_cred.egid = egid;
    g_cred = g_sess_cred;
}
void vfs64_get_cred64(Vfs64Cred64* out) { if (out) *out = g_cred; }
void vfs64_set_proc_cred64(int have, uint32_t uid, uint32_t gid, uint32_t euid, uint32_t egid) {
    if (g_cred_hold_depth > 0) {                                 // 覆盖期间：只认"覆盖者回来"
        if (task_current_id_64) {
            if (task_current_id_64() == g_cred_hold_owner) { g_cred = g_cred_hold_cred; return; }
        } else {
            return;                                              // 没有任务表：覆盖优先
        }
    }
    if (have) { g_cred.uid = uid; g_cred.gid = gid; g_cred.euid = euid; g_cred.egid = egid; }
    else      { g_cred = g_sess_cred; }                          // 内核线程/任务 0：回到会话身份
}
// 当前卷是否有权限字段（v4 = 1；v2/v3 旧卷 = 0 -> 拦截关闭）
int vfs64_perm_fields64() { return perm_enforced64() ? 1 : 0; }
// umask：新建文件/目录的模式 = 默认模式 & ~umask（返回旧值）。本系统 umask 是**全局一份**（如实标注）。
uint32_t vfs64_umask64(uint32_t new_mask) {
    const uint32_t old = g_umask64;
    g_umask64 = new_mask & 0777u;
    return old;
}
uint32_t vfs64_get_umask64() { return g_umask64; }
// 注意：need 用 VFS64_S_I*USR（9 位口径的调用方掩码），这里先折算成 3 位 rwx 再比 ——
//   两者混用会让"other 有 x 却被判无 x"（实测踩过：所有非 root 的 traversal 全被拒）。
static uint32_t perm_need3(uint32_t need) {
    uint32_t n = 0;
    if (need & VFS64_S_IRUSR) n |= 4u;
    if (need & VFS64_S_IWUSR) n |= 2u;
    if (need & VFS64_S_IXUSR) n |= 1u;
    return n;
}
static bool perm_can64(uint32_t iuid, uint32_t igid, uint32_t mode, uint32_t need) {
    if (g_cred.euid == 0) return true;
    const uint32_t n = perm_need3(need);
    if (n == 0) return true;
    uint32_t bits;
    if (g_cred.euid == iuid)      bits = (mode >> 6) & 7u;
    else if (g_cred.egid == igid) bits = (mode >> 3) & 7u;
    else                          bits = mode & 7u;
    return (bits & n) == n;
}
// 从 inode 读属主/组/模式（v2/v3 用默认值：root:root + 类型默认 0755/0644）
static uint32_t ino_uid_of64(const uint8_t* b) {
    return (g_lay->uid_off != 0) ? (uint32_t)rd16(b + g_lay->uid_off) : 0u;
}
static uint32_t ino_gid_of64(const uint8_t* b) {
    return (g_lay->gid_off != 0) ? (uint32_t)rd16(b + g_lay->gid_off) : 0u;
}
static uint32_t ino_mode_of64(const uint8_t* b, uint32_t type) {
    if (g_lay->mode_off != 0) {
        const uint32_t m = (uint32_t)rd16(b + g_lay->mode_off);
        if (m != 0) return m;
    }
    return (type == VFS64_TYPE_DIR) ? VFS64_LEGACY_DIR_MODE : VFS64_LEGACY_FILE_MODE;
}
// 统一拒绝打点（有上限）：[PERM64] deny op=… path=… uid=… mode=…
static int perm_deny64(const char* op, const char* path, uint32_t iuid, uint32_t igid,
                       uint32_t mode, uint32_t need) {
    if (g_perm_denies < VFS64_PERM_LOG_MAX) {
        g_perm_denies++;
        char p[41];
        uint32_t i = 0;
        if (path) { for (; path[i] && i < 40u; i++) p[i] = path[i]; }
        p[i] = 0;
        dbg64_str("[PERM64] deny op=");
        dbg64_str(op);
        dbg64_str(" path=");
        dbg64_str(p);
        dbg64_str(" uid=");
        dbg64_dec(g_cred.euid);
        dbg64_str(" gid=");
        dbg64_dec(g_cred.egid);
        dbg64_str(" mode=");
        log_octal4(mode & VFS64_S_IRWX);
        dbg64_str(" need=");
        if (need & VFS64_S_IRUSR) dbg64_str("r");
        if (need & VFS64_S_IWUSR) dbg64_str("w");
        if (need & VFS64_S_IXUSR) dbg64_str("x");
        dbg64_str(" owner=");
        dbg64_dec(iuid);
        dbg64_str(":");
        dbg64_dec(igid);
        dbg64_nl();
    }
    return -VFS64_EACCES;
}
// 判定一个已经载入的 inode。返回 0 = 允许；-EACCES = 拒绝（已打点）。
static int perm_check_ino64(const uint8_t* ino, const char* op, const char* path, uint32_t need) {
    if (!perm_enforced64()) return 0;
    const uint32_t type = ino[VFS_I_TYPE];
    const uint32_t iuid = ino_uid_of64(ino);
    const uint32_t igid = ino_gid_of64(ino);
    const uint32_t mode = ino_mode_of64(ino, type);
    if (perm_can64(iuid, igid, mode, need)) return 0;
    return perm_deny64(op, path, iuid, igid, mode, need);
}
// 判定一个 inode 号（失败自己打点：读盘失败 -> -1）
static int perm_check_idx64(uint32_t idx, const char* op, const char* path, uint32_t need) {
    if (!perm_enforced64()) return 0;
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    if (!inode_load(idx, ino)) return -1;
    if (ino[VFS_I_TYPE] == VFS64_TYPE_FREE) return -1;
    return perm_check_ino64(ino, op, path, need);
}
// 目录写（建/删/改名）需要 w+x —— Linux 语义
#define VFS64_NEED_WX (VFS64_S_IWUSR | VFS64_S_IXUSR)
#define VFS64_NEED_R  (VFS64_S_IRUSR)
#define VFS64_NEED_W  (VFS64_S_IWUSR)
#define VFS64_NEED_X  (VFS64_S_IXUSR)
// 新建 inode 的模式：默认模式 & ~umask（只对权限位生效，类型位保留）
static uint32_t new_mode64(uint32_t base_type_mode) {
    return (base_type_mode & VFS64_S_IFMT) | ((base_type_mode & VFS64_S_IRWX) & ~g_umask64);
}

// ==================== 设备层：假盘 / 弱 ATA ====================
// ★ 批次 M：自检假盘从 64 扇区（32KB）扩大到 **8192 扇区（4MB）** —— 32KB 的卷装不下
//   超过 67584 B 的文件，二级间接块就没法在自检里真的走一遍。分两个区用：
//     [0, VFS64_FAKE_SMALL_SECTORS)      —— bit0..bit12（既有自检：目录树/路径/**空间耗尽**/多卷…）
//     [VFS64_FAKE_BIG_LBA, 尾部)          —— bit13..bit15（大文件：二级间接 / 回收 / 上限边界）
//   小卷仍是 **64 扇区（32KB）**：bit6 的"写满 -> 删除 -> 再写"就是靠它才跑得动（数据区只有几十块）。
static const uint32_t VFS64_FAKE_SECTORS       = 8192;                  // 假盘容量（4MB）
static const uint32_t VFS64_FAKE_SMALL_SECTORS = 64;                    // bit0..11 的卷（32KB，保持原语义）
static const uint32_t VFS64_FAKE_BIG_LBA       = 4096;                  // 大文件自检卷的起始 LBA
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
// 版本 2 / 3 / 4 都接受（v2/v3 旧卷继续能挂：inode 布局不同、没有权限字段；v4 = 当前格式）。
static int sb_verify(const uint8_t* sb, Vfs64SbGeo* geo) {
    static const char magic[8] = { 'V','I','M','T','U','F','S','2' };
    if (cmp_bytes(sb + VFS_O_MAGIC, magic, 8) != 0) return SB_ERR_MAGIC;
    if (rd32(sb + VFS_O_CRC) != crc32_64(sb, VFS_SB_CRC_LEN)) return SB_ERR_CRC;
    const uint32_t ver = rd32(sb + VFS_O_VERSION);
    const Vfs64Layout* lay = nullptr;
    if (ver == VFS64_VERSION)             lay = &VFS_LAY_V4;
    else if (ver == VFS64_VERSION_V3)     lay = &VFS_LAY_V3;
    else if (ver == VFS64_VERSION_V2)     lay = &VFS_LAY_V2;
    else                                  return SB_ERR_VERSION;
    if (rd16(sb + VFS_O_SIG) != 0xAA55u) return SB_ERR_LAYOUT;
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
// 结构合法性：类型/名字长度/保留字段/CRC/大小/（v3/v4）时间与保留区/（v4）权限字段。
// 空槽（type=0）不校验 CRC —— 格式化后它就是全 0。
static bool inode_ok(const uint8_t* b, const char** why) {
    const uint8_t t = b[VFS_I_TYPE];
    if (t > VFS64_TYPE_DIR) { *why = "type"; return false; }
    if (b[VFS_I_NAMELEN] > g_lay->name_max) { *why = "namelen"; return false; }
    if (rd16(b + VFS_I_RSVD) != 0) { *why = "reserved"; return false; }
    if (t == VFS64_TYPE_FREE) return true;
    if (rd32(b + g_lay->crc_off) != crc32_64(b, g_lay->crc_off)) { *why = "crc"; return false; }
    // ★ 批次 M：上限按**当前卷布局**算（v3/v4 = 8 MiB、v2 = 67584）—— 不能拿 v3 的上限去放行 v2 的 inode。
    const uint32_t lim = (g_lay->dind_off != 0) ? VFS64_MAX_FILE_BYTES : VFS64_MAX_FILE_BYTES_V2;
    if (t == VFS64_TYPE_FILE && rd32(b + VFS_I_SIZE) > lim) { *why = "size"; return false; }
    if (g_lay->version == VFS64_VERSION || g_lay->version == VFS64_VERSION_V3) {
        if (b[VFS_I3_RSVD2] != 0) { *why = "rsvd2"; return false; }
        // 保留区起点：v3 从 75 起（权限字段的位置也是保留区，必须全 0）；v4 从 81 起
        const uint32_t rsvd_from = (g_lay->uid_off != 0) ? VFS_I4_RSVD3 : VFS_I3_DIND + 4u;
        for (uint32_t i = rsvd_from; i < g_lay->crc_off; i++)
            if (b[i] != 0) { *why = "rsvd3"; return false; }
        if (g_lay->version == VFS64_VERSION) {
            // ★ P4：v4 的权限三字段必须自洽（uid/gid 任意；mode 的类型位必须与 type 对得上、权限位 ≤ 0777）
            const uint32_t um = (uint32_t)rd16(b + VFS_I4_MODE);
            const uint32_t want_ifmt = (t == VFS64_TYPE_DIR) ? VFS64_S_IFDIR : VFS64_S_IFREG;
            if ((um & VFS64_S_IFMT) != want_ifmt) { *why = "mode type"; return false; }
            if ((um & ~(VFS64_S_IFMT | VFS64_S_IRWX)) != 0) { *why = "mode bits"; return false; }
        }
        // 名字区之后到 `dind` 之间必须是 0（★ 批次 M：**止于 VFS_I3_DIND(71) 而不是保留区** ——
        // 71..74 现在是有意义的 dind 字段，早期版本把它当保留区，正是这里最容易写错的地方）
        for (uint32_t i = VFS_I3_NAME + g_lay->name_max; i < VFS_I3_DIND; i++)
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
            if (rd32(b + VFS_I3_DIND) != 0) { *why = "dir dind"; return false; }
        } else if (t == VFS64_TYPE_FILE) {
            // ★ 批次 M：间接块指针**必须在数据区内**（越界一律拒绝：读/删路径都不会去碰它）；且
            //   用到二级间接（size > 132 块）的文件必须同时有一级间接块（几何自洽）。
            const uint32_t ind = rd32(b + VFS_I_IND);
            const uint32_t dind = rd32(b + VFS_I3_DIND);
            if (ind != 0 && !blk_ok_data(ind)) { *why = "ind range"; return false; }
            if (dind != 0 && !blk_ok_data(dind)) { *why = "dind range"; return false; }
            if (dind != 0 && ind == 0) { *why = "dind without ind"; return false; }
            const uint32_t blocks = (rd32(b + VFS_I_SIZE) + VFS64_BLOCK_BYTES - 1u) / VFS64_BLOCK_BYTES;
            if (blocks > VFS64_DIRECT_BLOCKS && ind == 0) { *why = "missing ind"; return false; }
            if (blocks > VFS64_L2_FIRST_BLOCK && dind == 0) { *why = "missing dind"; return false; }
            if (blocks > VFS64_MAX_MAP_BLOCKS) { *why = "too many blocks"; return false; }
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
// 读出一个间接块里的 128 个块号（不解析指针内容，只搬运到调用方缓冲；越界直接拒绝）
static bool load_indirect(uint32_t ind, uint32_t* out128) {
    if (!blk_ok_data(ind)) { log_bad_block(ind); return false; }
    if (!blk_read(ind, g_sec)) return false;
    for (uint32_t k = 0; k < VFS64_INDIRECT_PTRS; k++) out128[k] = rd32(g_sec + 4u * k);
    return true;
}
// 释放 inode 引用的全部数据块（直接 + 一级间接 + **二级间接**及其 128 个子块）。
// 遇到越界块号：打印并返回 false，但已经释放的部分保持释放（宁可泄漏，也不越界读写）。
// ★ 批次 M：二级间接块自己 + 它的每个子块 + 子块里的数据块都要回收 —— 写-删循环不能漏块。
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
    const uint32_t dind = (g_lay->dind_off != 0) ? rd32(ino + g_lay->dind_off) : 0u;
    if (dind != 0) {
        if (!blk_ok_data(dind)) { log_bad_block(dind); return false; }
        if (!load_indirect(dind, g_ptrs)) return false;                  // 二级间接块：128 个子块号
        for (uint32_t c = 0; c < VFS64_DIND_CHILDREN; c++) {
            const uint32_t child = g_ptrs[c];
            if (child == 0) continue;
            if (!blk_ok_data(child)) { log_bad_block(child); ok = false; continue; }
            if (!load_indirect(child, g_rm_child)) { ok = false; continue; }   // 子块：128 个数据块号
            for (uint32_t k = 0; k < VFS64_INDIRECT_PTRS; k++) {
                const uint32_t b = g_rm_child[k];
                if (b == 0) continue;
                if (!blk_ok_data(b)) { log_bad_block(b); ok = false; continue; }
                if (!bitmap_set(b, false)) ok = false;
            }
            if (!bitmap_set(child, false)) ok = false;
        }
        if (!bitmap_set(dind, false)) ok = false;
    }
    return ok;
}

// ==================== ★ 批次 M：块映射（直接 / 一级间接 / 二级间接）====================
// 三个层级的唯一实现点。设计要点（为什么长这样）：
//   * 读用一个"顺序游标"（g_rm_ind / g_rm_dind / g_rm_child）：顺序扫 fs 块时每级只读一次盘；
//   * 写用"RAM 镜像 + 最后统一落盘"（g_l1 / g_l2 / g_l3）：
//       1) 先把要新建的数据块全部分配 + 写好 + 记进镜像（**此时盘上的指针树还没动**）；
//       2) 再把镜像里的指针块按 子块 -> 二级 -> 一级 的顺序写盘；
//       3) 最后提交 inode（size/CRC）。
//     所以任何一步失败都不会让盘上的 inode 指向"已释放/没写过"的块；第 1 步失败可以
//     **回滚**（把新分配的块标回空闲），第 2/3 步失败只可能泄漏块（与既有 write64 同口径）。
//   * 新分配的块号记在 g_newblk[] 里（回滚与"块数记账"都用它）。
static bool alloc_track(uint32_t* out_blk) {
    const uint32_t b = alloc_block();
    if (b == 0) return false;
    if (g_newn >= (uint32_t)(sizeof(g_newblk) / sizeof(g_newblk[0]))) {   // 表满：宁可不写
        bitmap_set(b, false);
        log_line("alloc track table full");
        return false;
    }
    g_newblk[g_newn++] = b;
    *out_blk = b;
    return true;
}
static void alloc_rollback() {
    for (uint32_t i = 0; i < g_newn; i++) bitmap_set(g_newblk[i], false);
    g_newn = 0;
}
static void alloc_forget() { g_newn = 0; }

// 写用的映射上下文（一次文件操作一份）
struct Vfs64MapCtx {
    uint8_t* ino;              // inode 镜像（d0..d3 / ind / dind 可改；提交前盘上不变）
    bool     l1_loaded, l1_dirty;
    bool     l2_loaded, l2_dirty;
    bool     l3_loaded, l3_dirty;
    uint32_t l3_child;         // 当前 l3 对应二级间接块里的第几个子块（0xFFFFFFFF = 无）
    uint32_t l3_blk;           // 当前子块的卷块号
    uint32_t max_fs_blocks;    // 本次操作最多会用到多少个 fs 块（用于拒绝超上限）
    bool     used_l2;          // 本次是否真的碰过二级间接（诊断/自检用）
};

static void ctx_init(Vfs64MapCtx* m, uint8_t* ino) {
    m->ino = ino;
    m->l1_loaded = m->l1_dirty = false;
    m->l2_loaded = m->l2_dirty = false;
    m->l3_loaded = m->l3_dirty = false;
    m->l3_child = 0xFFFFFFFFu;
    m->l3_blk = 0;
    m->max_fs_blocks = 0;
    m->used_l2 = false;
}
// 一级间接块：装载 / 按需分配
static bool ctx_l1_load(Vfs64MapCtx* m) {
    if (m->l1_loaded) return true;
    const uint32_t b = rd32(m->ino + VFS_I_IND);
    if (b != 0) {
        if (!blk_ok_data(b)) { log_bad_block(b); return false; }
        if (!blk_read(b, g_l1)) return false;
    } else {
        zero_bytes(g_l1, VFS64_SECTOR_BYTES);
    }
    m->l1_loaded = true;
    return true;
}
static bool ctx_l1_ensure(Vfs64MapCtx* m) {
    if (rd32(m->ino + VFS_I_IND) != 0) return ctx_l1_load(m);
    uint32_t b = 0;
    if (!alloc_track(&b)) return false;
    wr32(m->ino + VFS_I_IND, b);
    zero_bytes(g_l1, VFS64_SECTOR_BYTES);
    m->l1_loaded = true;
    m->l1_dirty = true;
    return true;
}
// 二级间接块：装载 / 按需分配
static bool ctx_l2_load(Vfs64MapCtx* m) {
    if (g_lay->dind_off == 0) { log_op_fail("map", "no double indirect on this volume (v2)"); return false; }
    if (m->l2_loaded) return true;
    const uint32_t b = rd32(m->ino + g_lay->dind_off);
    if (b != 0) {
        if (!blk_ok_data(b)) { log_bad_block(b); return false; }
        if (!blk_read(b, g_l2)) return false;
    } else {
        zero_bytes(g_l2, VFS64_SECTOR_BYTES);
    }
    m->l2_loaded = true;
    return true;
}
static bool ctx_l2_ensure(Vfs64MapCtx* m) {
    if (g_lay->dind_off == 0) { log_op_fail("map", "no double indirect on this volume (v2)"); return false; }
    if (rd32(m->ino + g_lay->dind_off) != 0) return ctx_l2_load(m);
    uint32_t b = 0;
    if (!alloc_track(&b)) return false;
    wr32(m->ino + g_lay->dind_off, b);
    zero_bytes(g_l2, VFS64_SECTOR_BYTES);
    m->l2_loaded = true;
    m->l2_dirty = true;
    m->used_l2 = true;
    return true;
}
// 把"当前子块"的镜像写盘（切换子块 / 收尾时调用）
static bool ctx_l3_flush(Vfs64MapCtx* m) {
    if (!m->l3_loaded || !m->l3_dirty) return true;
    if (!blk_write(m->l3_blk, g_l3)) return false;
    m->l3_dirty = false;
    return true;
}
// 当前子块（二级间接块里第 child 个指针指向的"一级间接块"）：装载 / 按需分配
static bool ctx_l3_load(Vfs64MapCtx* m, uint32_t child) {
    if (m->l3_loaded && m->l3_child == child) return true;
    if (!ctx_l3_flush(m)) return false;                 // 换子块前先把上一个写盘（指针不能丢）
    m->l3_loaded = false;
    if (!ctx_l2_load(m)) return false;
    m->l3_child = child;
    m->l3_blk = rd32(g_l2 + 4u * child);
    if (m->l3_blk != 0) {
        if (!blk_ok_data(m->l3_blk)) { log_bad_block(m->l3_blk); return false; }
        if (!blk_read(m->l3_blk, g_l3)) return false;
    } else {
        zero_bytes(g_l3, VFS64_SECTOR_BYTES);
    }
    m->l3_loaded = true;
    return true;
}
static bool ctx_l3_ensure(Vfs64MapCtx* m, uint32_t child) {
    if (!ctx_l2_ensure(m)) return false;
    if (rd32(g_l2 + 4u * child) != 0) return ctx_l3_load(m, child);
    if (!ctx_l3_flush(m)) return false;      // ★ 换子块前必须先把上一个写盘（否则那 128 个指针全丢 -> 空洞）
    m->l3_loaded = false;
    uint32_t b = 0;
    if (!alloc_track(&b)) return false;
    wr32(g_l2 + 4u * child, b);
    m->l2_dirty = true;
    m->l3_child = child;
    m->l3_blk = b;
    zero_bytes(g_l3, VFS64_SECTOR_BYTES);
    m->l3_loaded = true;
    m->l3_dirty = true;
    m->used_l2 = true;
    return true;
}


// ---- 写侧：按 fs 块号查/设映射块号（顺序访问友好；需要时按需分配间接块）----
// map_get：*out = 0 表示该 fs 块**还没映射**（文件尾部的空洞是正常的；空洞出现在 size 以内 = 损坏）。
static bool map_get(Vfs64MapCtx* m, uint32_t i, uint32_t* out_blk) {
    *out_blk = 0;
    if (i < VFS64_DIRECT_BLOCKS) { *out_blk = rd32(m->ino + VFS_I_D0 + 4u * i); return true; }
    if (i < VFS64_L2_FIRST_BLOCK) {
        if (!ctx_l1_load(m)) return false;
        *out_blk = rd32(g_l1 + 4u * (i - VFS64_DIRECT_BLOCKS));
        return true;
    }
    if (i >= VFS64_MAX_MAP_BLOCKS) { log_op_fail("map", "fs block index beyond map capacity"); return false; }
    const uint32_t child = (i - VFS64_L2_FIRST_BLOCK) / VFS64_INDIRECT_PTRS;
    if (!ctx_l3_load(m, child)) return false;
    *out_blk = rd32(g_l3 + 4u * ((i - VFS64_L2_FIRST_BLOCK) % VFS64_INDIRECT_PTRS));
    return true;
}
// map_put：把块号写进**RAM 镜像**（间接块按需分配）；落盘统一由 map_flush 负责
static bool map_put(Vfs64MapCtx* m, uint32_t i, uint32_t blk) {
    if (i < VFS64_DIRECT_BLOCKS) { wr32(m->ino + VFS_I_D0 + 4u * i, blk); return true; }
    if (i < VFS64_L2_FIRST_BLOCK) {
        if (!ctx_l1_ensure(m)) return false;
        wr32(g_l1 + 4u * (i - VFS64_DIRECT_BLOCKS), blk);
        m->l1_dirty = true;
        return true;
    }
    if (i >= VFS64_MAX_MAP_BLOCKS) { log_op_fail("map", "fs block index beyond map capacity"); return false; }
    const uint32_t child = (i - VFS64_L2_FIRST_BLOCK) / VFS64_INDIRECT_PTRS;
    if (!ctx_l3_ensure(m, child)) return false;
    wr32(g_l3 + 4u * ((i - VFS64_L2_FIRST_BLOCK) % VFS64_INDIRECT_PTRS), blk);
    m->l3_dirty = true;
    return true;
}
// 指针块落盘：**子块 -> 二级间接 -> 一级间接**（数据块早已写好；顺序保证"先有被指的人，再有指针"）
static bool map_flush(Vfs64MapCtx* m) {
    if (!ctx_l3_flush(m)) { log_op_fail("map", "child block write failed"); return false; }
    if (m->l2_dirty) {
        const uint32_t b = rd32(m->ino + g_lay->dind_off);
        if (b == 0 || !blk_write(b, g_l2)) { log_op_fail("map", "double-indirect write failed"); return false; }
        m->l2_dirty = false;
    }
    if (m->l1_dirty) {
        const uint32_t b = rd32(m->ino + VFS_I_IND);
        if (b == 0 || !blk_write(b, g_l1)) { log_op_fail("map", "indirect write failed"); return false; }
        m->l1_dirty = false;
    }
    return true;
}

// ---- 只读游标（一次读取内复用：顺序扫时每一级只读一次盘）----
struct Vfs64RMap {
    bool     ind_loaded, dind_loaded;
    uint32_t ind_blk, dind_blk;
    uint32_t child_idx;        // 0xFFFFFFFF = 还没装载任何子块
    uint32_t child_blk;
};
static void rmap_init(Vfs64RMap* r) {
    r->ind_loaded = r->dind_loaded = false;
    r->ind_blk = r->dind_blk = 0;
    r->child_idx = 0xFFFFFFFFu;
    r->child_blk = 0;
}
// 取 fs 块 i 的卷块号（0 = 未映射）；-1 = 指针越界/读盘失败
static bool rmap_get(Vfs64RMap* r, const uint8_t* ino, uint32_t i, uint32_t* out) {
    *out = 0;
    if (i < VFS64_DIRECT_BLOCKS) { *out = rd32(ino + VFS_I_D0 + 4u * i); return true; }
    if (i < VFS64_L2_FIRST_BLOCK) {
        if (!r->ind_loaded) {
            const uint32_t b = rd32(ino + VFS_I_IND);
            zero_bytes(g_rm_ind, (uint32_t)sizeof(g_rm_ind));
            if (b != 0) {
                if (!blk_ok_data(b)) { log_bad_block(b); return false; }
                if (!load_indirect(b, g_rm_ind)) return false;
            }
            r->ind_loaded = true;
            r->ind_blk = b;
        }
        *out = g_rm_ind[i - VFS64_DIRECT_BLOCKS];
        return true;
    }
    if (i >= VFS64_MAX_MAP_BLOCKS) { log_op_fail("read", "fs block index beyond map capacity"); return false; }
    if (g_lay->dind_off == 0) { log_op_fail("read", "no double indirect on this volume (v2)"); return false; }
    if (!r->dind_loaded) {
        const uint32_t b = rd32(ino + g_lay->dind_off);
        zero_bytes(g_rm_dind, (uint32_t)sizeof(g_rm_dind));
        if (b != 0) {
            if (!blk_ok_data(b)) { log_bad_block(b); return false; }
            if (!load_indirect(b, g_rm_dind)) return false;
        }
        r->dind_loaded = true;
        r->dind_blk = b;
    }
    const uint32_t child = (i - VFS64_L2_FIRST_BLOCK) / VFS64_INDIRECT_PTRS;
    if (r->child_idx != child) {
        const uint32_t cb = g_rm_dind[child];
        zero_bytes(g_rm_child, (uint32_t)sizeof(g_rm_child));
        if (cb != 0) {
            if (!blk_ok_data(cb)) { log_bad_block(cb); return false; }
            if (!load_indirect(cb, g_rm_child)) return false;
        }
        r->child_idx = child;
        r->child_blk = cb;
    }
    *out = g_rm_child[(i - VFS64_L2_FIRST_BLOCK) % VFS64_INDIRECT_PTRS];
    return true;
}

// ---- 纯函数：写 bytes 字节需要的块数（数据块 + 间接块）/ 当前卷的真实上限 ----
uint32_t vfs64_blocks_for_bytes64(uint32_t bytes) {
    if (bytes == 0) return 0;
    const uint32_t data = (bytes + VFS64_BLOCK_BYTES - 1u) / VFS64_BLOCK_BYTES;
    uint32_t n = data;
    if (data > VFS64_DIRECT_BLOCKS) n += 1u;                                   // 一级间接块
    if (data > VFS64_L2_FIRST_BLOCK)                                           // 二级间接块 + 子块
        n += 1u + ((data - VFS64_L2_FIRST_BLOCK + VFS64_INDIRECT_PTRS - 1u) / VFS64_INDIRECT_PTRS);
    return n;
}
uint32_t vfs64_max_file_bytes64() {
    if (!g_mounted) return 0u;
    return (g_lay->dind_off != 0) ? VFS64_MAX_FILE_BYTES : VFS64_MAX_FILE_BYTES_V2;
}

// ---- 写一段字节到 fs 块区间（src == nullptr 表示写 0 = 补空洞）----
// 缺块按需分配（新块先清零再写）；部分覆盖时先把原块读出来。返回 false = 失败（调用方回滚）。
static bool write_range_ctx(Vfs64MapCtx* m, uint32_t off, const uint8_t* src, uint32_t len) {
    const uint32_t first = off / VFS64_BLOCK_BYTES;
    const uint32_t last  = (off + len + VFS64_BLOCK_BYTES - 1u) / VFS64_BLOCK_BYTES;
    for (uint32_t i = first; i < last; i++) {
        uint32_t blk = 0;
        if (!map_get(m, i, &blk)) return false;
        bool fresh = false;
        if (blk == 0) {
            if (!alloc_track(&blk)) { log_op_fail("write_at64", "no space (data block)"); return false; }
            if (!map_put(m, i, blk)) return false;
            zero_bytes(g_sec, VFS64_SECTOR_BYTES);
            if (!blk_write(blk, g_sec)) { log_op_fail("write_at64", "new block write failed"); return false; }
            fresh = true;
        } else if (!blk_ok_data(blk)) {
            log_bad_block(blk);
            return false;
        }
        if (src == nullptr && fresh) continue;                 // 新块已经是 0：补零不必再写一遍
        const uint32_t blk_off = i * VFS64_BLOCK_BYTES;
        const uint32_t c0 = (off > blk_off) ? (off - blk_off) : 0u;
        const uint32_t c1 = ((off + len) < (blk_off + VFS64_BLOCK_BYTES))
                            ? (off + len - blk_off) : VFS64_BLOCK_BYTES;
        if (!fresh) { if (!blk_read(blk, g_sec)) return false; }   // 部分覆盖：读出原内容
        for (uint32_t k = c0; k < c1; k++)
            g_sec[k] = (src != nullptr) ? src[(blk_off + k) - off] : (uint8_t)0;
        if (!blk_write(blk, g_sec)) { log_op_fail("write_at64", "data block write failed"); return false; }
    }
    return true;
}

// ---- 部分写（盘上 inode 由调用方提交；本函数只改 ino 镜像 + 盘上的数据/指针块）----
static int write_at_inode(uint8_t* ino, uint32_t off, const void* buf, uint32_t len, bool* out_used_l2) {
    if (out_used_l2) *out_used_l2 = false;
    const uint32_t lim = (g_lay->dind_off != 0) ? VFS64_MAX_FILE_BYTES : VFS64_MAX_FILE_BYTES_V2;
    if (off > lim || len > (lim - off)) {                      // ★ 上限检查在**任何写盘之前**
        log_op_fail("write_at64", "too large (over the single-file limit)");
        return -1;
    }
    const uint32_t old_size = rd32(ino + VFS_I_SIZE);
    const uint32_t new_end  = off + len;
    const uint32_t new_size = (old_size > new_end) ? old_size : new_end;
    const uint32_t need_blocks = (new_size + VFS64_BLOCK_BYTES - 1u) / VFS64_BLOCK_BYTES;
    const uint32_t have_blocks = (old_size + VFS64_BLOCK_BYTES - 1u) / VFS64_BLOCK_BYTES;

    // 空预检（保守）：新数据块 + 最坏情况下的间接块（一级 1、二级 1 + 每组子块 1）+ 1 块余量。
    // 目的是"空间不足**先失败**，不写一半"；真实分配仍会再检查一次（alloc_block 返回 0 时回滚）。
    {
        uint32_t ptr_max = 0;
        if (need_blocks > VFS64_DIRECT_BLOCKS) ptr_max += 1u;
        if (need_blocks > VFS64_L2_FIRST_BLOCK)
            ptr_max += 2u + ((need_blocks - VFS64_L2_FIRST_BLOCK + VFS64_INDIRECT_PTRS - 1u) / VFS64_INDIRECT_PTRS);
        const uint32_t data_need = (need_blocks > have_blocks) ? (need_blocks - have_blocks) : 0u;
        uint32_t fb = 0;
        if (vfs64_free64(&fb, nullptr, nullptr) != 0) { log_op_fail("write_at64", "free space unknown"); return -1; }
        if (fb < data_need + ptr_max + 1u) { log_op_fail("write_at64", "no space (pre-flight)"); return -1; }
    }

    Vfs64MapCtx m;
    ctx_init(&m, ino);
    m.max_fs_blocks = need_blocks;
    if (off > old_size) {                                      // ★ 空洞补零（不做稀疏文件）
        if (!write_range_ctx(&m, old_size, nullptr, off - old_size)) { alloc_rollback(); return -1; }
    }
    if (len > 0) {
        if (!write_range_ctx(&m, off, (const uint8_t*)buf, len)) { alloc_rollback(); return -1; }
    }
    if (!map_flush(&m)) return -1;                             // 指针已可能落盘：不再回滚（只泄漏，不损坏）
    wr32(ino + VFS_I_SIZE, new_size);
    if (g_lay->mtime_off != 0) wr32(ino + g_lay->mtime_off, vfs64_now64());
    if (g_lay->kind_off != 0 && off == 0 && len > 0) {          // kind：只在"写文件头"时按首块重判
        const uint32_t k = (len < VFS64_BLOCK_BYTES) ? len : VFS64_BLOCK_BYTES;
        ino[g_lay->kind_off] = (uint8_t)vfs64_kind_of_data64(buf, k, nullptr, 0);
    }
    wr32(ino + g_lay->crc_off, crc32_64(ino, g_lay->crc_off));
    alloc_forget();
    if (out_used_l2) *out_used_l2 = m.used_l2;
    return 0;
}

// ---- 流式建链（整体重写）：建**全新**块链，数据由 src 回调提供（want ≤ 512，必须写满）----
// 新块号写进 ino_new 镜像 + 间接块镜像并落盘；失败时（建链阶段）释放本次新分配的块。
static int build_chain_stream64(uint8_t* ino_new, uint32_t len, Vfs64Src64 src, void* ctx) {
    const uint32_t lim = (g_lay->dind_off != 0) ? VFS64_MAX_FILE_BYTES : VFS64_MAX_FILE_BYTES_V2;
    if (len > lim) { log_op_fail("write_stream64", "too large (over the single-file limit)"); return -1; }
    const uint32_t needed = (len + VFS64_BLOCK_BYTES - 1u) / VFS64_BLOCK_BYTES;
    if (needed > VFS64_MAX_MAP_BLOCKS) { log_op_fail("write_stream64", "size exceeds block map"); return -1; }
    {
        uint32_t fb = 0;
        if (vfs64_free64(&fb, nullptr, nullptr) != 0) { log_op_fail("write_stream64", "free space unknown"); return -1; }
        if (fb < vfs64_blocks_for_bytes64(len) + 1u) { log_op_fail("write_stream64", "no space (pre-flight)"); return -1; }
    }
    Vfs64MapCtx m;
    ctx_init(&m, ino_new);
    m.max_fs_blocks = needed;
    for (uint32_t i = 0; i < needed; i++) {
        uint32_t blk = 0;
        if (!alloc_track(&blk)) { alloc_rollback(); log_op_fail("write_stream64", "no space (data block)"); return -1; }
        if (!map_put(&m, i, blk)) { alloc_rollback(); return -1; }
        const uint32_t off = i * VFS64_BLOCK_BYTES;
        uint32_t c = len - off;
        if (c > VFS64_BLOCK_BYTES) c = VFS64_BLOCK_BYTES;
        zero_bytes(g_sec, VFS64_SECTOR_BYTES);                  // 末块尾部补 0（不写工作缓冲里的旧字节）
        if (src != nullptr && src(ctx, off, g_sec, c) != 0) {
            alloc_rollback();
            log_op_fail("write_stream64", "source callback failed");
            return -1;
        }
        if (!blk_write(blk, g_sec)) { alloc_rollback(); log_op_fail("write_stream64", "data block write failed"); return -1; }
    }
    if (!map_flush(&m)) return -1;
    alloc_forget();
    return 0;
}

// ---- 只读：把 inode 里 [off, off+len) 的字节拷进 buf（跨级都对；*got = 实际读到的字节数）----
static int read_at_inode(const uint8_t* ino, uint32_t off, void* buf, uint32_t len, uint32_t* got) {
    if (got) *got = 0;
    if (!buf) { log_op_fail("read_at64", "bad args"); return -1; }
    const uint32_t size = rd32(ino + VFS_I_SIZE);
    if (off >= size) return 0;                                  // EOF：0 字节（正常）
    uint32_t want = size - off;
    if (want > len) want = len;
    if (want == 0) return 0;
    Vfs64RMap r;
    rmap_init(&r);
    uint32_t done = 0;
    uint8_t* out = (uint8_t*)buf;
    const uint32_t first = off / VFS64_BLOCK_BYTES;
    const uint32_t last  = (off + want + VFS64_BLOCK_BYTES - 1u) / VFS64_BLOCK_BYTES;
    for (uint32_t i = first; i < last; i++) {
        uint32_t blk = 0;
        if (!rmap_get(&r, ino, i, &blk)) { log_op_fail("read_at64", "bad block chain"); return -1; }
        if (blk == 0) { log_op_fail("read_at64", "hole in file (corrupt chain)"); return -1; }
        if (!blk_ok_data(blk)) { log_bad_block(blk); return -1; }
        if (!blk_read(blk, g_sec)) return -1;
        const uint32_t blk_off = i * VFS64_BLOCK_BYTES;
        const uint32_t c0 = (off > blk_off) ? (off - blk_off) : 0u;
        const uint32_t c1 = ((off + want) < (blk_off + VFS64_BLOCK_BYTES))
                            ? (off + want - blk_off) : VFS64_BLOCK_BYTES;
        for (uint32_t k = c0; k < c1; k++) out[done++] = g_sec[k];
    }
    if (got) *got = done;
    return 0;
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
//            —— 只有 want_parent=true 时才会返回 1；
//      -1  = 非法路径 / 中间分量不存在 / 读盘失败（已打点）；
//      -EACCES = ★ P4：某一级目录缺 x（进不去，已打 [PERM64] deny 行）。
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
        // ★ P4：进目录/遍历路径要求每一级目录都有 x（Linux 语义；root 与 v2/v3 旧卷在内部直接放行）。
        //   注意：**在解析下一段之前**检查 cur —— 最后一级"目标自身"的 x 不属于遍历（stat 目标只要父目录 x）。
        if (perm_enforced64()) {
            const int px = perm_check_idx64(cur, "traverse", path, VFS64_NEED_X);
            if (px != 0) return -VFS64_EACCES;          // px == -1（读盘失败）也按权限错返回：已打点
        }
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

    // 几何：位图在最前，inode 区居中，数据区占剩下的全部（inode 记录大小按 v4 = 128B）
    const uint32_t bitmap_blocks = (total_sectors + VFS64_BITMAP_BLK_BITS - 1u) / VFS64_BITMAP_BLK_BITS;
    uint32_t inodes = total_sectors / 64u;
    if (inodes < 16u) inodes = 16u;
    if (inodes > VFS64_MAX_INODES) inodes = VFS64_MAX_INODES;
    const uint32_t inode_blocks = (inodes + VFS_LAY_V4.inodes_per_blk - 1u) / VFS_LAY_V4.inodes_per_blk;
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
    g_lay = &VFS_LAY_V4;                                // ★ P4：新格式化一律产出 v4（带权限字段）
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

    // 4) 根目录 inode（0 号槽）：目录、无名、size=0、parent=自己、nlink=1、mtime=现在、
    //    ★ P4：uid=gid=0（root）、mode = S_IFDIR|0755（root 的 / 就是 0755；用户不能往根目录写）
    uint8_t root[VFS64_INODE_BYTES_MAX];
    zero_bytes(root, VFS64_INODE_BYTES_MAX);
    root[VFS_I_TYPE] = (uint8_t)VFS64_TYPE_DIR;
    wr32(root + VFS_I_PARENT, 0);
    wr32(root + VFS_I3_MTIME, vfs64_now64());
    wr16(root + VFS_I3_NLINK, 1);
    root[VFS_I3_KIND] = (uint8_t)VFS64_KIND_DIR;
    wr16(root + VFS_I4_UID, 0);
    wr16(root + VFS_I4_GID, 0);
    wr16(root + VFS_I4_MODE, (uint16_t)(VFS64_S_IFDIR | 0755u));
    wr32(root + VFS_LAY_V4.crc_off, crc32_64(root, VFS_LAY_V4.crc_off));
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
    dbg64_str(" perm=uid/gid/mode@");
    dbg64_dec(VFS_I4_UID);
    dbg64_str("/");
    dbg64_dec(VFS_I4_GID);
    dbg64_str("/");
    dbg64_dec(VFS_I4_MODE);
    dbg64_str(" rootmode=0755");
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
    dbg64_str(perm_enforced64() ? " perm=on" : " perm=off");
    dbg64_nl();
    // ★ P4：旧卷（v2/v3）没有 uid/gid/mode 字段 -> 权限拦截**如实关闭**（打一行说清楚；不假装拦了）
    if (!perm_enforced64()) {
        dbg64_str("[PERM64] legacy volume v");
        dbg64_dec(g_lay->version);
        dbg64_str(": no uid/gid/mode fields -> permission checks disabled");
        dbg64_str(" (owner shows as root, mode = default 0755/0644)\n");
        dbg64_nl();
    }
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
// 构造 = 当前卷换成 slot（必须已挂载）；析构 = **原样换回**进来时那个槽（凭证也一并还原）。
//   * 嵌套（理论上不会：_on64 内部只调旧 API）：计数 + LIFO 恢复，语义不乱。
//   * 中断：vfs64 的调用点都在任务上下文（GUI 主循环 / 终端 / 自检）；即便被抢断，别的上下文
//     也只会以同样的守卫方式切卷，每次恢复的都是"自己进来时"的卷 —— 当前卷不会丢。
//   * inode 缓存按 (slot, drive, lba) 键控，所以切卷不必清缓存，也不会读到别的卷的字节。
//     img64/app64/elf64/proc64/sysstate64/update64 这些"固定写系统卷"的系统组件不被自己的权限检查
//     卡住 —— 规格明文要求）；用户侧的当前卷操作走**非 on64** 入口（身份照实，见 fs64 的 self 路径）。
struct Vfs64SlotGuard {
    int  saved;
    bool active;
    bool rooted;
    Vfs64Cred64 saved_cred;
    explicit Vfs64SlotGuard(int slot, bool as_root = false) {
        active = false;
        rooted = false;
        if (slot < 0 || slot >= (int)VFS64_SLOT_MAX || !g_vol[slot].mounted) return;
        if (g_vol_switch_depth > 0) log_line("WARN nested volume switch (LIFO restore)");
        if (as_root) {
            saved_cred = g_cred;
            if (g_cred_hold_depth == 0) {                      // 最外层覆盖：记下"归谁"
                g_cred_hold_cred.uid = 0; g_cred_hold_cred.gid = 0;
                g_cred_hold_cred.euid = 0; g_cred_hold_cred.egid = 0;
                g_cred_hold_owner = task_current_id_64 ? task_current_id_64() : 0xFFFFFFFFu;
            }
            g_cred_hold_depth++;
            g_cred = g_cred_hold_cred;
            rooted = true;
        }
        saved = g_cur_slot;
        g_cur_slot = slot;
        g_vol_switch_depth++;
        active = true;
    }
    ~Vfs64SlotGuard() {
        if (!active) return;
        if (rooted) {
            g_cred_hold_depth--;
            if (g_cred_hold_depth == 0) g_cred_hold_owner = 0xFFFFFFFFu;
            g_cred = saved_cred;                            // ★ 身份原样还给调用方
        }
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
// ★ fs64 用：把"当前卷"临时切到 slot，但**不改调用方身份**（权限判定照常）。返回 0 / -1。
int vfs64_scope_enter64(int slot, int* out_saved) {
    if (slot < 0 || slot >= (int)VFS64_SLOT_MAX || !g_vol[slot].mounted) return -1;
    if (out_saved) *out_saved = g_cur_slot;
    g_cur_slot = slot;
    return 0;
}
void vfs64_scope_leave64(int saved_slot) {
    if (saved_slot >= 0 && saved_slot < (int)VFS64_SLOT_MAX) g_cur_slot = saved_slot;
}

// ---- 按槽别名（系统组件固定写系统卷用的入口；期间身份 = root，见 Vfs64SlotGuard）----
int vfs64_stat_on64(int slot, const char* path, uint32_t* type, uint32_t* size) {
    Vfs64SlotGuard g(slot, true);
    if (!g.active) return on64_slot_unavailable("stat", slot);
    return vfs64_stat(path, type, size);
}
int vfs64_stat64_on64(int slot, const char* path, Vfs64Info64* out) {
    Vfs64SlotGuard g(slot, true);
    if (!g.active) return on64_slot_unavailable("stat64", slot);
    return vfs64_stat64(path, out);
}
int vfs64_read_on64(int slot, const char* path, void* buf, int max) {
    Vfs64SlotGuard g(slot, true);
    if (!g.active) return on64_slot_unavailable("read", slot);
    return vfs64_read(path, buf, max);
}
int vfs64_write_on64(int slot, const char* path, const void* buf, int len) {
    Vfs64SlotGuard g(slot, true);
    if (!g.active) return on64_slot_unavailable("write", slot);
    return vfs64_write(path, buf, len);
}
// ★ 批次 M：大文件读写/流式写的按槽变体（fs64 固定卷读写用）
int vfs64_read_at_on64(int slot, const char* path, uint32_t off, void* buf, uint32_t len, uint32_t* out_got) {
    Vfs64SlotGuard g(slot, true);
    if (!g.active) { if (out_got) *out_got = 0; return on64_slot_unavailable("read_at", slot); }
    return vfs64_read_at64(path, off, buf, len, out_got);
}
int vfs64_write_at_on64(int slot, const char* path, uint32_t off, const void* buf, uint32_t len) {
    Vfs64SlotGuard g(slot, true);
    if (!g.active) return on64_slot_unavailable("write_at", slot);
    return vfs64_write_at64(path, off, buf, len);
}
int vfs64_write_stream_on64(int slot, const char* path, uint32_t len, Vfs64Src64 src, void* ctx) {
    Vfs64SlotGuard g(slot, true);
    if (!g.active) return on64_slot_unavailable("write_stream", slot);
    return vfs64_write_stream64(path, len, src, ctx);
}
int vfs64_mkdir_on64(int slot, const char* path) {
    Vfs64SlotGuard g(slot, true);
    if (!g.active) return on64_slot_unavailable("mkdir", slot);
    return vfs64_mkdir(path);
}
int vfs64_create_on64(int slot, const char* path) {
    Vfs64SlotGuard g(slot, true);
    if (!g.active) return on64_slot_unavailable("create", slot);
    return vfs64_create64(path);
}
int vfs64_unlink_on64(int slot, const char* path) {
    Vfs64SlotGuard g(slot, true);
    if (!g.active) return on64_slot_unavailable("unlink", slot);
    return vfs64_unlink(path);
}
int vfs64_rmdir_on64(int slot, const char* path) {
    Vfs64SlotGuard g(slot, true);
    if (!g.active) return on64_slot_unavailable("rmdir", slot);
    return vfs64_rmdir64(path);
}
int vfs64_ls_on64(int slot, const char* path, char names[][VFS64_LS_NAME_BUF], int max, uint32_t* sizes) {
    Vfs64SlotGuard g(slot, true);
    if (!g.active) return on64_slot_unavailable("ls", slot);
    return vfs64_ls(path, names, max, sizes);
}
int vfs64_list64_on64(int slot, const char* path, Vfs64Dirent64* out, int max, uint32_t* cursor) {
    Vfs64SlotGuard g(slot, true);
    if (!g.active) return on64_slot_unavailable("list64", slot);
    return vfs64_list64(path, out, max, cursor);
}
int vfs64_tree_dump64_on64(int slot, const char* path, int max_entries, int max_depth) {
    Vfs64SlotGuard g(slot, true);
    if (!g.active) return on64_slot_unavailable("tree_dump64", slot);
    return vfs64_tree_dump64(path, max_entries, max_depth);
}

// ★ 批次 J：同目录改名 / 空间查询 的按槽入口（explorer 复制/剪切/粘贴时按显式卷操作）
int vfs64_rename_on64(int slot, const char* old_path, const char* new_name) {
    Vfs64SlotGuard g(slot, true);
    if (!g.active) return on64_slot_unavailable("rename", slot);
    return vfs64_rename64(old_path, new_name);
}
int vfs64_free_on64(int slot, uint32_t* free_blocks, uint32_t* free_bytes, uint32_t* total_blocks) {
    Vfs64SlotGuard g(slot, true);
    if (!g.active) return on64_slot_unavailable("free", slot);
    return vfs64_free64(free_blocks, free_bytes, total_blocks);
}
// ★ P4：chmod/chown 的按槽入口（userdb64 给 /home/<user> 设属主/模式用；期间身份 = root）
int vfs64_chmod_on64(int slot, const char* path, uint32_t mode) {
    Vfs64SlotGuard g(slot, true);
    if (!g.active) return on64_slot_unavailable("chmod", slot);
    return vfs64_chmod64(path, mode);
}
int vfs64_chown_on64(int slot, const char* path, uint32_t uid, uint32_t gid) {
    Vfs64SlotGuard g(slot, true);
    if (!g.active) return on64_slot_unavailable("chown", slot);
    return vfs64_chown64(path, uid, gid);
}
// ==================== 读文件（★ 批次 M：按偏移分块，跨一级/二级间接）====================
// 载入并校验路径 -> 文件 inode，返回 0（*out_idx）；-1 = 失败；-EACCES = 权限不足（已打点）。
// want_rw：★ P4 由调用方给出需要的位（读=NEED_R、写=NEED_W）；0 = 只查存在/类型（write_resolve 自己判）。
static int lookup_file64(const char* path, const char* op, uint32_t* out_idx, uint8_t* ino) {
    uint32_t idx = 0;
    const int pr = path_resolve(path, false, &idx, nullptr, nullptr, nullptr);
    if (pr != 0) {
        if (pr == -VFS64_EACCES) { log_op_fail(op, "permission denied (traverse)"); return -VFS64_EACCES; }
        log_op_fail(op, "not found");
        return -1;
    }
    if (inode_load_ok(idx, ino, op) != 0) return -1;
    if (ino[VFS_I_TYPE] != VFS64_TYPE_FILE) { log_op_fail(op, "not a file"); return -1; }
    if (out_idx) *out_idx = idx;
    return 0;
}
// 路径版：读 [off, off+len)（**缓冲区由调用方给**；*out_got = 实际读到的字节数）。
int vfs64_read_at64(const char* path, uint32_t off, void* buf, uint32_t len, uint32_t* out_got) {
    if (out_got) *out_got = 0;
    if (!g_mounted) { log_op_fail("read_at64", "not mounted"); return -1; }
    if (!path || !buf) { log_op_fail("read_at64", "bad args"); return -1; }
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    const int lk = lookup_file64(path, "read_at64", nullptr, ino);
    if (lk != 0) return lk;
    const int pc = perm_check_ino64(ino, "read", path, VFS64_NEED_R);      // ★ P4：读要 r
    if (pc != 0) { log_op_fail("read_at64", "permission denied (r)"); return pc; }
    return read_at_inode(ino, off, buf, len, out_got);
}
// 兼容旧 API：整读前 max 字节（= read_at64(path, 0, buf, max)）。
int vfs64_read64(const char* path, void* buf, int max) {
    if (!g_mounted) { log_op_fail("read64", "not mounted"); return -1; }
    if (!path || !buf || max < 0) { log_op_fail("read64", "bad args"); return -1; }
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    const int lk = lookup_file64(path, "read64", nullptr, ino);
    if (lk != 0) return lk;
    const int pc = perm_check_ino64(ino, "read", path, VFS64_NEED_R);      // ★ P4：读要 r
    if (pc != 0) { log_op_fail("read64", "permission denied (r)"); return pc; }
    uint32_t got = 0;
    if (read_at_inode(ino, 0, buf, (uint32_t)max, &got) != 0) return -1;
    return (int)got;
}
int vfs64_read(const char* path, void* buf, int max) { return vfs64_read64(path, buf, max); }

// ==================== 写文件（★ 批次 M：流式建链 + 部分写）====================
// 数据源：内存缓冲（vfs64_write64 用）
struct Vfs64MemSrc64 { const uint8_t* p; };
static int mem_src64(void* ctx, uint32_t off, void* dst, uint32_t want) {
    const Vfs64MemSrc64* s = (const Vfs64MemSrc64*)ctx;
    copy_bytes(dst, s->p + off, want);
    return 0;
}

// 解析"要写的路径"：返回 0 = 已有文件（*idx 有效、new_file=false）；1 = 要新建（父目录/名字已回填）；
// -1 = 失败；-EACCES = 权限不足（已打点）。语义与旧 write64 完全一致。
// ★ P4：这里**同时做写权限判定**（已有文件要 w；新建要父目录 w+x）—— 写路径只有这一处入口。
static int write_resolve64(const char* path, uint32_t* idx, uint32_t* parent, char* nm, uint32_t* nlen,
                           bool* new_file, uint8_t* old_ino) {
    uint32_t i = 0, par = 0, nl = 0;
    char name[VFS64_NAME_MAX + 1];
    const int pr = path_resolve(path, true, &i, &par, name, &nl);
    if (pr < 0) {
        if (pr == -VFS64_EACCES) { log_op_fail("write", "permission denied (traverse)"); return -VFS64_EACCES; }
        log_op_fail("write", "bad path");
        return -1;
    }
    *new_file = (pr != 0);
    if (pr == 0) {
        if (inode_load_ok(i, old_ino, "write") != 0) return -1;
        if (old_ino[VFS_I_TYPE] != VFS64_TYPE_FILE) { log_op_fail("write", "path is a directory"); return -1; }
        // 已有文件：写要 w（覆盖/截断/部分写都是写）
        const int pc = perm_check_ino64(old_ino, "write", path, VFS64_NEED_W);
        if (pc != 0) { log_op_fail("write", "permission denied (w)"); return pc; }
        par = rd32(old_ino + VFS_I_PARENT);
        nl = old_ino[VFS_I_NAMELEN];
        copy_bytes(name, old_ino + g_lay->name_off, nl);
        name[nl] = 0;
    } else {
        // 新建：父目录必须存在且是目录（path_resolve 已保证存在），并且要 w+x
        uint8_t pino[VFS64_INODE_BYTES_MAX];
        if (inode_load_ok(par, pino, "write") != 0) return -1;
        if (pino[VFS_I_TYPE] != VFS64_TYPE_DIR) { log_op_fail("write", "parent is not a directory"); return -1; }
        const int pc = perm_check_ino64(pino, "create", path, VFS64_NEED_WX);
        if (pc != 0) { log_op_fail("write", "permission denied (dir w+x)"); return pc; }
    }
    *idx = i;
    *parent = par;
    *nlen = nl;
    copy_bytes(nm, name, nl + 1);
    return 0;
}
// 填一个"新文件"的 inode 镜像（type/namelen/name/parent/nlink/★P4 uid+gid+mode；size 由调用方按长度写）
// ★ P4：新建文件的属主 = **当前 euid/egid**；模式 = S_IFREG | (0666 & ~umask)（默认 0644）。
static void fill_new_file_ino64(uint8_t* ino, const char* nm, uint32_t nlen, uint32_t parent, uint32_t len) {
    zero_bytes(ino, VFS64_INODE_BYTES_MAX);
    ino[VFS_I_TYPE] = (uint8_t)VFS64_TYPE_FILE;
    ino[VFS_I_NAMELEN] = (uint8_t)nlen;
    copy_bytes(ino + g_lay->name_off, nm, nlen);
    wr32(ino + VFS_I_PARENT, parent);
    wr32(ino + VFS_I_SIZE, len);
    if (g_lay->mtime_off != 0) wr32(ino + g_lay->mtime_off, vfs64_now64());
    if (g_lay->nlink_off != 0) wr16(ino + g_lay->nlink_off, 1);
    if (g_lay->uid_off != 0) {
        wr16(ino + g_lay->uid_off, (uint16_t)g_cred.euid);
        wr16(ino + g_lay->gid_off, (uint16_t)g_cred.egid);
        wr16(ino + g_lay->mode_off, (uint16_t)new_mode64(VFS64_S_IFREG | 0666u));
    }
}

// ★ 整体重写（不存在则创建）：建全新块链 -> 提交 inode -> 最后释放旧块。
// 语义与旧 vfs64_write64 逐条一致（中途失败只泄漏块，绝不让 inode 指向已释放的块）。
// ★ P4：写权限判定在 write_resolve64 里（已有文件 w / 新建父目录 w+x），这里只透传错误码。
int vfs64_write_stream64(const char* path, uint32_t len, Vfs64Src64 src, void* ctx) {
    if (!g_mounted) { log_op_fail("write_stream64", "not mounted"); return -1; }
    if (!path) { log_op_fail("write_stream64", "bad args"); return -1; }
    uint32_t idx = 0, parent = 0, nlen = 0;
    char nm[VFS64_NAME_MAX + 1];
    bool is_new = false;
    uint8_t old_ino[VFS64_INODE_BYTES_MAX];
    const int wr = write_resolve64(path, &idx, &parent, nm, &nlen, &is_new, old_ino);
    if (wr != 0) return wr;
    if (is_new && !alloc_inode(&idx)) { log_op_fail("write_stream64", "no free inode"); return -1; }
    // 已有文件：old_ino 由 write_resolve64 载入并校验（父目录/名字也以 inode 里的为准）；
    // 新文件：idx 由上一步分配（**绝不能是 0** —— inode 0 是根目录）

    uint8_t ino_new[VFS64_INODE_BYTES_MAX];
    fill_new_file_ino64(ino_new, nm, nlen, parent, len);
    if (!is_new && g_lay->uid_off != 0) {                       // ★ P4：覆盖已有文件**保留属主/模式/组**
        copy_bytes(ino_new + g_lay->uid_off, old_ino + g_lay->uid_off, 6);   // uid(2)+gid(2)+mode(2)（Linux 语义：
    }                                                          //  重写不改属主、不改 mode；只有新建才按 euid/umask）
    if (build_chain_stream64(ino_new, len, src, ctx) != 0) {
        if (is_new) alloc_forget();
        return -1;
    }
    if (g_lay->kind_off != 0) {                                 // kind：只看首块（流式写没整份缓冲）
        uint8_t head[VFS64_BLOCK_BYTES];
        zero_bytes(head, VFS64_BLOCK_BYTES);
        if (len > 0 && src != nullptr) {
            const uint32_t c = (len < VFS64_BLOCK_BYTES) ? len : VFS64_BLOCK_BYTES;
            if (src(ctx, 0, head, c) != 0) { log_op_fail("write_stream64", "source head failed"); return -1; }
        }
        ino_new[g_lay->kind_off] = (uint8_t)vfs64_kind_of_data64(head, (len < VFS64_BLOCK_BYTES) ? len : VFS64_BLOCK_BYTES,
                                                                 nm, nlen);
    }
    wr32(ino_new + g_lay->crc_off, crc32_64(ino_new, g_lay->crc_off));
    if (!inode_store(idx, ino_new)) {                           // 提交点
        log_op_fail("write_stream64", "inode commit failed");
        return -1;
    }
    if (!is_new) {
        if (!free_file_blocks(old_ino)) log_line("write_stream64: WARN old blocks partially freed");
    } else {
        touch_dir(parent, 0);                                   // 新建：刷新父目录 mtime（尽力而为）
    }
    return (int)len;
}
// 兼容旧 API：整文件重写（buf 必须装得下 len 字节）
int vfs64_write64(const char* path, const void* buf, int len) {
    if (!g_mounted) { log_op_fail("write64", "not mounted"); return -1; }
    if (!path || (!buf && len > 0) || len < 0) { log_op_fail("write64", "bad args"); return -1; }
    const uint32_t lim = (g_lay->dind_off != 0) ? VFS64_MAX_FILE_BYTES : VFS64_MAX_FILE_BYTES_V2;
    if ((uint32_t)len > lim) { log_op_fail("write64", "too large"); return -1; }
    Vfs64MemSrc64 s;
    s.p = (const uint8_t*)buf;
    return vfs64_write_stream64(path, (uint32_t)len, (len > 0) ? mem_src64 : nullptr, &s);
}
int vfs64_write(const char* path, const void* buf, int len) { return vfs64_write64(path, buf, len); }

// ★ 部分写 / 追加（不存在则创建）：保留原有字节，缺块按需分配，off > size 的空洞补零。
// 返回 0 = 成功；-1 = 失败（**盘上 inode 不变**）；-EACCES = 权限不足（write_resolve 已打点）。
int vfs64_write_at64(const char* path, uint32_t off, const void* buf, uint32_t len) {
    if (!g_mounted) { log_op_fail("write_at64", "not mounted"); return -1; }
    if (!path || (!buf && len > 0)) { log_op_fail("write_at64", "bad args"); return -1; }
    uint32_t idx = 0, parent = 0, nlen = 0;
    char nm[VFS64_NAME_MAX + 1];
    bool is_new = false;
    uint8_t old_ino[VFS64_INODE_BYTES_MAX];
    const int wr = write_resolve64(path, &idx, &parent, nm, &nlen, &is_new, old_ino);
    if (wr != 0) return wr;
    if (is_new) {
        if (!alloc_inode(&idx)) { log_op_fail("write_at64", "no free inode"); return -1; }
        fill_new_file_ino64(old_ino, nm, nlen, parent, 0);
    }
    if (len == 0) {
        if (is_new) {                                           // 建空文件：只提交 inode
            wr32(old_ino + g_lay->crc_off, crc32_64(old_ino, g_lay->crc_off));
            if (!inode_store(idx, old_ino)) { log_op_fail("write_at64", "inode commit failed"); return -1; }
            touch_dir(parent, 0);
        }
        return 0;
    }
    if (write_at_inode(old_ino, off, buf, len, nullptr) != 0) return -1;
    if (!inode_store(idx, old_ino)) { log_op_fail("write_at64", "inode commit failed"); return -1; }
    if (is_new) touch_dir(parent, 0);
    return 0;
}
// 建空文件（父目录必须存在；已存在且是文件 = 0 幂等；是目录 = -1）。
// ★ P4：新建走 write 的权限判定（父目录 w+x），已存在则要 w（覆盖提交一次空内容 = 截断语义）。
int vfs64_create64(const char* path) {
    if (!g_mounted) { log_op_fail("create64", "not mounted"); return -1; }
    if (!path) { log_op_fail("create64", "bad args"); return -1; }
    uint32_t idx = 0;
    const int pr = path_resolve(path, false, &idx, nullptr, nullptr, nullptr);
    if (pr == -VFS64_EACCES) return -VFS64_EACCES;
    if (pr == 0) {
        uint8_t ino[VFS64_INODE_BYTES_MAX];
        if (inode_load_ok(idx, ino, "create64") != 0) return -1;
        if (ino[VFS_I_TYPE] != VFS64_TYPE_FILE) { log_op_fail("create64", "path is a directory"); return -1; }
        return 0;                                      // 已存在且是文件 = 幂等成功
    }
    static const char empty[1] = { 0 };
    const int w = vfs64_write64(path, empty, 0);
    return (w == 0) ? 0 : w;                            // -1 / -EACCES 原样透传
}

// ==================== 建目录 / 删除 ====================
// 建目录（多级：父目录必须存在）。★ P4：父目录需要 w+x；新目录属主 = 当前 euid，模式 = 0777 & ~umask。
int vfs64_mkdir64(const char* path) {
    if (!g_mounted) { log_op_fail("mkdir64", "not mounted"); return -1; }
    if (!path) { log_op_fail("mkdir64", "bad args"); return -1; }

    uint32_t idx = 0, parent = 0, nlen = 0;
    char nm[VFS64_NAME_MAX + 1];
    const int pr = path_resolve(path, true, &idx, &parent, nm, &nlen);
    if (pr < 0) {
        if (pr == -VFS64_EACCES) { log_op_fail("mkdir64", "permission denied (traverse)"); return -VFS64_EACCES; }
        log_op_fail("mkdir64", "bad path / parent not found");
        return -1;
    }
    if (pr == 0) { log_op_fail("mkdir64", "already exists"); return -1; }
    uint8_t pino[VFS64_INODE_BYTES_MAX];               // 父目录必须真的是目录
    if (inode_load_ok(parent, pino, "mkdir64") != 0) return -1;
    if (pino[VFS_I_TYPE] != VFS64_TYPE_DIR) { log_op_fail("mkdir64", "parent is not a directory"); return -1; }
    const int pc = perm_check_ino64(pino, "mkdir", path, VFS64_NEED_WX);   // ★ P4：目录写要 w+x
    if (pc != 0) { log_op_fail("mkdir64", "permission denied (dir w+x)"); return pc; }

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
    if (g_lay->uid_off != 0) {                          // ★ P4：属主/模式
        wr16(ino + g_lay->uid_off, (uint16_t)g_cred.euid);
        wr16(ino + g_lay->gid_off, (uint16_t)g_cred.egid);
        wr16(ino + g_lay->mode_off, (uint16_t)new_mode64(VFS64_S_IFDIR | 0777u));
    }
    wr32(ino + g_lay->crc_off, crc32_64(ino, g_lay->crc_off));
    if (!inode_store(slot, ino)) { log_op_fail("mkdir64", "inode write failed"); return -1; }
    touch_dir(parent, (g_lay->nlink_off != 0) ? 1 : 0);   // 父目录：mtime 刷新 + nlink+1（子目录数）
    return 0;
}
int vfs64_mkdir(const char* path) { return vfs64_mkdir64(path); }

// 删除普通文件。★ P4：删除只要**父目录 w+x**（Linux 不在删的时候看文件本身的权限）。
int vfs64_unlink64(const char* path) {
    if (!g_mounted) { log_op_fail("unlink64", "not mounted"); return -1; }
    if (!path) { log_op_fail("unlink64", "bad args"); return -1; }

    uint32_t idx = 0;
    const int pr = path_resolve(path, false, &idx, nullptr, nullptr, nullptr);
    if (pr != 0) {
        if (pr == -VFS64_EACCES) return -VFS64_EACCES;
        log_op_fail("unlink64", "not found");
        return -1;
    }
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    if (inode_load_ok(idx, ino, "unlink64") != 0) return -1;
    if (ino[VFS_I_TYPE] == VFS64_TYPE_DIR) { log_op_fail("unlink64", "refuse to remove a directory (use rmdir64)"); return -1; }
    // ★ P4：父目录 w+x（先判权限再动块：被拒时盘上什么都不变）
    const int pc = perm_check_idx64(rd32(ino + VFS_I_PARENT), "unlink", path, VFS64_NEED_WX);
    if (pc != 0) { log_op_fail("unlink64", "permission denied (dir w+x)"); return pc; }
    // inode 里有越界块号时拒绝删除（保持"能删掉的一定是结构自洽的项"）
    if (!free_file_blocks(ino)) { log_op_fail("unlink64", "corrupt inode (not removed)"); return -1; }

    const uint32_t parent = rd32(ino + VFS_I_PARENT);
    uint8_t empty[VFS64_INODE_BYTES_MAX];
    if (!inode_store(idx, empty)) { log_op_fail("unlink64", "inode clear failed"); return -1; }
    touch_dir(parent, 0);
    return 0;
}
int vfs64_unlink(const char* path) { return vfs64_unlink64(path); }

// 删除空目录。★ P4：父目录 w+x（并且目标目录本身会被删掉，不需要目标自己的权限 —— Linux 语义）。
int vfs64_rmdir64(const char* path) {
    if (!g_mounted) { log_op_fail("rmdir64", "not mounted"); return -1; }
    if (!path) { log_op_fail("rmdir64", "bad args"); return -1; }

    uint32_t idx = 0;
    const int pr = path_resolve(path, false, &idx, nullptr, nullptr, nullptr);
    if (pr != 0) {
        if (pr == -VFS64_EACCES) return -VFS64_EACCES;
        log_op_fail("rmdir64", "not found");
        return -1;
    }
    if (idx == 0) { log_op_fail("rmdir64", "refuse to remove the root directory"); return -1; }
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    if (inode_load_ok(idx, ino, "rmdir64") != 0) return -1;
    if (ino[VFS_I_TYPE] != VFS64_TYPE_DIR) { log_op_fail("rmdir64", "not a directory"); return -1; }
    const int pc = perm_check_idx64(rd32(ino + VFS_I_PARENT), "rmdir", path, VFS64_NEED_WX);   // ★ P4
    if (pc != 0) { log_op_fail("rmdir64", "permission denied (dir w+x)"); return pc; }

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
// 改名（同目录）。★ P4：所在目录需要 w+x。
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
    const int pr = path_resolve(old_path, false, &idx, nullptr, nullptr, nullptr);
    if (pr != 0) {
        if (pr == -VFS64_EACCES) return -VFS64_EACCES;
        log_op_fail("rename64", "not found");
        return -1;
    }
    if (idx == 0) { log_op_fail("rename64", "refuse to rename the root directory"); return -1; }

    uint8_t ino[VFS64_INODE_BYTES_MAX];
    if (inode_load_ok(idx, ino, "rename64") != 0) return -1;
    const uint32_t parent = rd32(ino + VFS_I_PARENT);
    const int pc = perm_check_idx64(parent, "rename", old_path, VFS64_NEED_WX);   // ★ P4
    if (pc != 0) { log_op_fail("rename64", "permission denied (dir w+x)"); return pc; }
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

// ==================== ★ P4：chmod / chown / access ====================
// 规则（Linux 语义的裁剪版，如实写明取舍）：
//   * chmod：**root 或属主**可以改；参数只接受"类型位 + 0777"（不实现 setuid/setgid/sticky 位）。
//     非属主且非 root -> -EPERM(-1)（并打点 [PERM64] deny op=chmod）；找不到/旧卷无字段 -> -1。
//   * chown：**只有 root** 能改属主（属主自己也不能改，Linux 的实际行为就是 root-only）；否则 -EPERM(-1)。
//     uid/gid 传 (uint32_t)-1 表示"不改这一项"（与 Linux 的 -1 语义一致）。
//   * access：只按 r/w/x 三段判定（root 恒通过）。
static int chmod64_common(const char* path, uint32_t mode) {
    if (!g_mounted) { log_op_fail("chmod64", "not mounted"); return -1; }
    if (!path) { log_op_fail("chmod64", "bad args"); return -1; }
    if ((mode & VFS64_S_IFMT) == 0) mode |= VFS64_S_IFREG;      // 只给权限位时按普通文件类型补
    if ((mode & ~(VFS64_S_IFMT | VFS64_S_IRWX)) != 0) { log_op_fail("chmod64", "bad mode"); return -1; }
    if (!perm_enforced64()) {                                   // v2/v3 旧卷：没有字段可写，如实拒绝
        dbg64_str("[PERM64] chmod: volume v");
        dbg64_dec(g_lay->version);
        dbg64_str(" has no mode field (unsupported in this batch)\n");
        dbg64_nl();
        return -1;
    }
    uint32_t idx = 0;
    const int pr = path_resolve(path, false, &idx, nullptr, nullptr, nullptr);
    if (pr != 0) {
        if (pr == -VFS64_EACCES) return -VFS64_EACCES;
        log_op_fail("chmod64", "not found");
        return -1;
    }
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    if (inode_load_ok(idx, ino, "chmod64") != 0) return -1;
    const uint32_t iuid = ino_uid_of64(ino);
    const uint32_t type = ino[VFS_I_TYPE];
    if (g_cred.euid != 0 && g_cred.euid != iuid) {               // 非 root 且非属主 -> EPERM
        dbg64_str("[PERM64] deny op=chmod path=");
        char p[41];
        uint32_t i = 0;
        for (; path[i] && i < 40u; i++) p[i] = path[i];
        p[i] = 0;
        dbg64_str(p);
        dbg64_str(" uid=");
        dbg64_dec(g_cred.euid);
        dbg64_str(" owner=");
        dbg64_dec(iuid);
        dbg64_str(" (only the owner or root can chmod)\n");
        dbg64_nl();
        return -VFS64_EPERM;
    }
    const uint32_t want_ifmt = (type == VFS64_TYPE_DIR) ? VFS64_S_IFDIR : VFS64_S_IFREG;
    const uint16_t nm = (uint16_t)(want_ifmt | (mode & VFS64_S_IRWX));
    wr16(ino + g_lay->mode_off, nm);
    if (g_lay->mtime_off != 0) wr32(ino + g_lay->mtime_off, vfs64_now64());
    wr32(ino + g_lay->crc_off, crc32_64(ino, g_lay->crc_off));
    if (!inode_store(idx, ino)) { log_op_fail("chmod64", "inode write failed"); return -1; }
    dbg64_str("[VFS64] chmod ok path=");
    dbg64_str(path);
    dbg64_str(" mode=");
    log_octal4(mode & VFS64_S_IRWX);
    dbg64_nl();
    return 0;
}
int vfs64_chmod64(const char* path, uint32_t mode) { return chmod64_common(path, mode); }

static int chown64_common(const char* path, uint32_t uid, uint32_t gid) {
    if (!g_mounted) { log_op_fail("chown64", "not mounted"); return -1; }
    if (!path) { log_op_fail("chown64", "bad args"); return -1; }
    if (!perm_enforced64()) {
        dbg64_str("[PERM64] chown: volume v");
        dbg64_dec(g_lay->version);
        dbg64_str(" has no uid/gid fields (unsupported in this batch)\n");
        dbg64_nl();
        return -1;
    }
    if (g_cred.euid != 0) {                                      // 只有 root 能改属主
        dbg64_str("[PERM64] deny op=chown path=");
        char p[41];
        uint32_t i = 0;
        for (; path[i] && i < 40u; i++) p[i] = path[i];
        p[i] = 0;
        dbg64_str(p);
        dbg64_str(" uid=");
        dbg64_dec(g_cred.euid);
        dbg64_str(" (only root can chown)\n");
        dbg64_nl();
        return -VFS64_EPERM;
    }
    uint32_t idx = 0;
    const int pr = path_resolve(path, false, &idx, nullptr, nullptr, nullptr);
    if (pr != 0) {
        if (pr == -VFS64_EACCES) return -VFS64_EACCES;
        log_op_fail("chown64", "not found");
        return -1;
    }
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    if (inode_load_ok(idx, ino, "chown64") != 0) return -1;
    if (uid != 0xFFFFFFFFu) wr16(ino + g_lay->uid_off, (uint16_t)uid);
    if (gid != 0xFFFFFFFFu) wr16(ino + g_lay->gid_off, (uint16_t)gid);
    if (g_lay->mtime_off != 0) wr32(ino + g_lay->mtime_off, vfs64_now64());
    wr32(ino + g_lay->crc_off, crc32_64(ino, g_lay->crc_off));
    if (!inode_store(idx, ino)) { log_op_fail("chown64", "inode write failed"); return -1; }
    dbg64_str("[VFS64] chown ok path=");
    dbg64_str(path);
    dbg64_str(" uid=");
    dbg64_dec(ino_uid_of64(ino));
    dbg64_str(" gid=");
    dbg64_dec(ino_gid_of64(ino));
    dbg64_nl();
    return 0;
}
int vfs64_chown64(const char* path, uint32_t uid, uint32_t gid) { return chown64_common(path, uid, gid); }

// access(2)：mask 用 Linux 低 3 位（4=r / 2=w / 1=x；0 = 只查存在）。
int vfs64_access64(const char* path, uint32_t mask) {
    if (!g_mounted) { log_op_fail("access64", "not mounted"); return -1; }
    if (!path) { log_op_fail("access64", "bad args"); return -1; }
    uint32_t idx = 0;
    const int pr = path_resolve(path, false, &idx, nullptr, nullptr, nullptr);
    if (pr != 0) return (pr == -VFS64_EACCES) ? -VFS64_EACCES : -1;
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    if (inode_load_ok(idx, ino, "access64") != 0) return -1;
    uint32_t need = 0;
    if (mask & 4u) need |= VFS64_NEED_R;
    if (mask & 2u) need |= VFS64_NEED_W;
    if (mask & 1u) need |= VFS64_NEED_X;
    if (need == 0) return 0;
    const int pc = perm_check_ino64(ino, "access", path, need);
    return (pc == 0) ? 0 : pc;
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
    d->uid = ino_uid_of64(ino);                                 // ★ P4（旧卷 -> 0 = root）
    d->gid = ino_gid_of64(ino);
    d->mode = ino_mode_of64(ino, d->type);                      // 旧卷 -> 默认 0755/0644
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
// 列目录（路径版）。★ P4：需要 r（**列目录不要求目录有 x** —— 与 Linux 一致：x 只用于遍历路径）。
int vfs64_list64(const char* path, Vfs64Dirent64* out, int max, uint32_t* cursor) {
    if (!g_mounted) { log_op_fail("list64", "not mounted"); return -1; }
    if (!path) { log_op_fail("list64", "bad args"); return -1; }
    uint32_t idx = 0;
    const int pr = path_resolve(path, false, &idx, nullptr, nullptr, nullptr);
    if (pr != 0) {
        if (pr == -VFS64_EACCES) return -VFS64_EACCES;
        log_op_fail("list64", "path not found");
        return -1;
    }
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    if (inode_load_ok(idx, ino, "list64") != 0) return -1;
    if (ino[VFS_I_TYPE] != VFS64_TYPE_DIR) { log_op_fail("list64", "not a directory"); return -1; }
    const int pc = perm_check_ino64(ino, "list", path, VFS64_NEED_R);
    if (pc != 0) { log_op_fail("list64", "permission denied (r)"); return pc; }
    return list_inode64(idx, out, max, cursor);
}
// 查属性。★ P4：stat 只看路径遍历（父目录 x）—— 不需要目标自己的 r（Linux 语义），
// 查属性。★ P4：stat 只看路径遍历（父目录 x）—— 不需要目标自己的 r（Linux 语义），
//   uid/gid/mode 一并给出（旧卷：uid=gid=0、mode = 默认 0755/0644）。
int vfs64_stat64(const char* path, Vfs64Info64* out) {
    if (!g_mounted) { log_op_fail("stat64", "not mounted"); return -1; }
    if (!path || !out) { log_op_fail("stat64", "bad args"); return -1; }
    uint32_t idx = 0;
    const int pr = path_resolve(path, false, &idx, nullptr, nullptr, nullptr);
    if (pr != 0) {
        if (pr == -VFS64_EACCES) return -VFS64_EACCES;
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
    out->uid = ino_uid_of64(ino);                                // ★ P4
    out->gid = ino_gid_of64(ino);
    out->mode = ino_mode_of64(ino, out->type);
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
    const int pr = path_resolve(path, false, &idx, nullptr, nullptr, nullptr);
    if (pr != 0) {
        if (pr == -VFS64_EACCES) return -VFS64_EACCES;
        log_op_fail("opendir64", "path not found");
        return -1;
    }
    uint8_t ino[VFS64_INODE_BYTES_MAX];
    if (inode_load_ok(idx, ino, "opendir64") != 0) return -1;
    if (ino[VFS_I_TYPE] != VFS64_TYPE_DIR) { log_op_fail("opendir64", "not a directory"); return -1; }
    const int pc = perm_check_ino64(ino, "opendir", path, VFS64_NEED_R);   // ★ P4：r
    if (pc != 0) { log_op_fail("opendir64", "permission denied (r)"); return pc; }
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
            dbg64_str(" uid=");                                  // ★ P4：行尾追加（既有断言的字段顺序不变）
            dbg64_dec(page[i].uid);
            dbg64_str(" gid=");
            dbg64_dec(page[i].gid);
            dbg64_str(" mode=");
            log_octal4(page[i].mode & VFS64_S_IRWX);
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
        dbg64_str(" uid=");
        dbg64_dec(ok ? in.uid : 0u);
        dbg64_str(" gid=");
        dbg64_dec(ok ? in.gid : 0u);
        dbg64_str(" mode=");
        log_octal4(ok ? (in.mode & VFS64_S_IRWX) : 0u);
        dbg64_nl();
    }
    Vfs64Cred64 cr;
    vfs64_get_cred64(&cr);
    dbg64_str("[PERM64] dump cred uid=");
    dbg64_dec(cr.uid);
    dbg64_str(" gid=");
    dbg64_dec(cr.gid);
    dbg64_str(" euid=");
    dbg64_dec(cr.euid);
    dbg64_str(" egid=");
    dbg64_dec(cr.egid);
    dbg64_str(" umask=");
    log_octal4(vfs64_get_umask64());
    dbg64_str(" enforce=");
    dbg64_dec((g_mounted && g_lay->mode_off != 0) ? 1u : 0u);
    dbg64_str(" volume=v");
    dbg64_dec(g_mounted ? g_lay->version : 0u);
    dbg64_nl();
}

// ==================== 自检（不需要真盘）====================
static uint8_t g_sel_a[4096];        // 自检读写缓冲（4KB 模式，覆盖直接块 + 间接块）
static uint8_t g_sel_b[4096];

// ---- ★ 批次 M：大文件自检用的工具（确定性字节模式 / 流式 CRC / 生成式数据源）----
// 字节模式**必须与宿主侧 tests/bigfile64_test.py 的 pat64() 完全一致**（两边独立算同一个 CRC）。
static uint8_t pat64(uint32_t off) {
    return (uint8_t)((off * 31u + (off >> 8) * 7u + (off >> 16) * 11u + 0xA5u) & 0xFFu);
}
// 与 kernel/fs64.cpp 同口径的流式 CRC32（zlib：反射 0xEDB88320 / 初值·末异或 0xFFFFFFFF）
static uint32_t crc32_cont64(uint32_t crc, const uint8_t* p, uint32_t n) {
    crc = ~crc;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}
// 生成式数据源（写 1.5MB 大文件不用准备 1.5MB 缓冲）：dst[0..want) = pat64(off + i)
static int sel_src64(void* ctx, uint32_t off, void* dst, uint32_t want) {
    (void)ctx;
    uint8_t* d = (uint8_t*)dst;
    for (uint32_t i = 0; i < want; i++) d[i] = pat64(off + i);
    return 0;
}

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
    // ★ P4：自检按**系统组件口径**在 root 凭证下跑（它检查的是卷格式/几何/大文件这条链）；
    //   权限本身由 bit16 用**显式切换的凭证**覆盖（root -> 1234 -> root），跑完原样还回去。
    Vfs64Cred64 sel_cred;
    vfs64_get_cred64(&sel_cred);
    vfs64_set_proc_cred64(1, 0, 0, 0, 0);

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

    if (vfs64_format(0, 0, VFS64_FAKE_SMALL_SECTORS) != 0) {
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
    // ---- bit16(65536)：★ P4 权限（**v4 卷**：三段判定 + root 绕过 + chmod/chown + umask）----
    // 为什么放在 bit0 刚格式化完的位置：此时假卷是**新的 v4 卷**、根目录 0755 root:root；
    //   本段末尾把凭证与 umask 恢复，bit1..bit15 继续在 root 凭证下跑（= 系统组件的同一口径）。
    if (!(fails & 1)) {
        bool ok = true;
        Vfs64Cred64 cred0;
        vfs64_get_cred64(&cred0);
        const uint32_t umask0 = vfs64_umask64(VFS64_UMASK_DEFAULT);
        Vfs64Info64 pinfo;
        char pbuf[8];
        // ① root 建目录/文件：属主 = 调用方（root）、模式 = 默认 & ~umask
        int step = 0;
        step = 1;
        if (vfs64_mkdir64("/perm") != 0) ok = false;
        if (ok && vfs64_write64("/perm/f.txt", "hello", 5) != 5) ok = false;
        if (ok && (vfs64_stat64("/perm", &pinfo) != 0 || pinfo.uid != 0 || pinfo.gid != 0 ||
                   (pinfo.mode & VFS64_S_IRWX) != 0755u || (pinfo.mode & VFS64_S_IFMT) != VFS64_S_IFDIR)) {
            ok = false;
            log_line("perm: dir owner/mode FAIL");
        }
        if (ok && (vfs64_stat64("/perm/f.txt", &pinfo) != 0 ||
                   (pinfo.mode & VFS64_S_IRWX) != 0644u || (pinfo.mode & VFS64_S_IFMT) != VFS64_S_IFREG)) {
            ok = false;
            log_line("perm: file mode FAIL");
        }
        // ② root 把 /perm 收成 0700 + 文件 0600：别的用户连 x 都没有（进不去）
        if (ok && vfs64_chmod64("/perm", VFS64_S_IFDIR | 0700u) != 0) ok = false;
        if (ok && vfs64_chmod64("/perm/f.txt", VFS64_S_IFREG | 0600u) != 0) ok = false;
        vfs64_set_cred64(1234, 1234, 1234, 1234);                 // uid=gid=1234 的普通用户
        if (ok && vfs64_stat64("/perm/f.txt", &pinfo) != -VFS64_EACCES) ok = false;    // 目录无 x：进不去
        if (ok && vfs64_read64("/perm/f.txt", pbuf, 5) != -VFS64_EACCES) ok = false;
        if (ok && vfs64_mkdir64("/perm/sub") != -VFS64_EACCES) ok = false;
        // ③ root 先把目录放宽到 0755（让 1234 能进目录）-> 再验证：
        //    非属主 chmod / 非 root chown 一律 -EPERM；other 有 r 无 w -> 读通过、写被拒（-EACCES）
        vfs64_set_cred64(cred0.uid, cred0.gid, cred0.euid, cred0.egid);       // 回到 root（自检本体）
        if (ok && vfs64_chmod64("/perm", VFS64_S_IFDIR | 0755u) != 0) ok = false;
        if (ok && vfs64_chmod64("/perm/f.txt", VFS64_S_IFREG | 0644u) != 0) ok = false;
        vfs64_set_cred64(1234, 1234, 1234, 1234);
        if (ok && vfs64_chmod64("/perm/f.txt", VFS64_S_IFREG | 0600u) != -VFS64_EPERM) {
            ok = false;                                           // 非属主不能 chmod
            log_line("perm: non-owner chmod was not denied FAIL");
        }
        if (ok && vfs64_chown64("/perm/f.txt", 1234u, 1234u) != -VFS64_EPERM) ok = false;  // 非 root 不能 chown
        vfs64_set_cred64(1234, 1234, 1234, 1234);
        if (ok && vfs64_read64("/perm/f.txt", pbuf, 5) != 5) ok = false;         // other: r
        if (ok && vfs64_write64("/perm/f.txt", "x", 1) != -VFS64_EACCES) {
            ok = false;                                           // other: 没有 w
            log_line("perm: other write was not denied FAIL");
        }
        if (ok && vfs64_mkdir64("/perm/sub") != -VFS64_EACCES) {
            ok = false;                                           // 目录 w 不给
            log_line("perm: dir write without w was not denied FAIL");
        }
        if (ok && vfs64_list64("/", nullptr, 0, nullptr) != -1) ok = false;       // 参数错仍然 -1（不是权限错）
        step = 4;
        vfs64_set_cred64(cred0.uid, cred0.gid, cred0.euid, cred0.egid);
        if (ok && vfs64_chmod64("/perm/f.txt", VFS64_S_IFREG | 0000u) != 0) ok = false;
        if (ok && vfs64_read64("/perm/f.txt", pbuf, 5) != 5) ok = false;
        if (ok && vfs64_write64("/perm/f.txt", "root", 4) != 4) ok = false;
        // ⑤ root chown 给普通用户 + 属主自己可读写；umask 077 影响新文件模式
        step = 5;
        vfs64_set_cred64(1234, 1234, 1234, 1234);
        if (ok && vfs64_read64("/perm/f.txt", pbuf, 4) != -VFS64_EACCES) ok = false;   // 0000：属主也不行
        step = 6;
        vfs64_set_cred64(cred0.uid, cred0.gid, cred0.euid, cred0.egid);
        if (ok && vfs64_chmod64("/perm/f.txt", VFS64_S_IFREG | 0600u) != 0) ok = false;
        if (ok && vfs64_chown64("/perm/f.txt", 1234u, 1234u) != 0) ok = false;   // root 把属主给 1234（下面验"属主自己"）
        vfs64_set_cred64(1234, 1234, 1234, 1234);
        if (ok && vfs64_read64("/perm/f.txt", pbuf, 4) != 4) ok = false;         // 属主 r
        if (ok && vfs64_write64("/perm/f.txt", "me", 2) != 2) ok = false;        // 属主 w
        vfs64_set_cred64(cred0.uid, cred0.gid, cred0.euid, cred0.egid);
        (void)vfs64_umask64(0077u);
        step = 7;
        if (ok && vfs64_write64("/perm/u.txt", "u", 1) != 1) ok = false;
        if (ok && (vfs64_stat64("/perm/u.txt", &pinfo) != 0 || (pinfo.mode & VFS64_S_IRWX) != 0600u)) {
            ok = false;
            log_line("perm: umask 077 FAIL");
        }
        // ⑥ chown 到另一个 uid 后：属主读通过、原属主（root 之外的 1234）被拒
        if (ok && vfs64_chown64("/perm/u.txt", 999u, 999u) != 0) ok = false;
        vfs64_set_cred64(1234, 1234, 1234, 1234);
        if (ok && vfs64_read64("/perm/u.txt", pbuf, 1) != -VFS64_EACCES) ok = false;   // other 无 r
        if (ok && vfs64_chmod64("/perm/u.txt", VFS64_S_IFREG | 0644u) != -VFS64_EPERM) ok = false;  // 非属主不许
        vfs64_set_cred64(cred0.uid, cred0.gid, cred0.euid, cred0.egid);
        if (ok && vfs64_chmod64("/perm/u.txt", VFS64_S_IFREG | 0644u) != 0) ok = false;  // root 改回 0644
        vfs64_set_cred64(1234, 1234, 1234, 1234);
        if (ok && vfs64_read64("/perm/u.txt", pbuf, 1) != 1) ok = false;         // 0644：other r
        step = 8;
        // ★ 清理前必须回到原凭证：/perm 是 root 的（这个时刻 cred 还是 1234，删不掉）
        vfs64_set_cred64(cred0.uid, cred0.gid, cred0.euid, cred0.egid);
        (void)vfs64_umask64(umask0);
        (void)vfs64_unlink64("/perm/u.txt");
        (void)vfs64_unlink64("/perm/f.txt");
        (void)vfs64_rmdir64("/perm");
        if (ok) {
            dbg64_str("[VFS64] perm selftest ok owner/mode/other/root/chmod/chown/umask=1");
            dbg64_nl();
        } else {
            dbg64_str("[VFS64] perm selftest FAIL step=");
            dbg64_dec((uint64_t)step);
            dbg64_nl();
            fails |= 65536;
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
            wr16(bad + VFS_I4_UID, 0);                            // ★ P4：v4 的权限字段必须自洽
            wr16(bad + VFS_I4_GID, 0);
            wr16(bad + VFS_I4_MODE, (uint16_t)(VFS64_S_IFREG | 0644u));
            wr32(bad + VFS_I_D0, g_blocks + 7);                // 越界块号
            wr32(bad + VFS_LAY_V4.crc_off, crc32_64(bad, VFS_LAY_V4.crc_off));
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
            wr16(bad + VFS_I4_UID, 0);                            // ★ P4：权限字段自洽（要测的是 CRC，不是 mode）
            wr16(bad + VFS_I4_GID, 0);
            wr16(bad + VFS_I4_MODE, (uint16_t)(VFS64_S_IFREG | 0644u));
            wr32(bad + VFS_LAY_V4.crc_off, 0x12345678u);       // 故意写错
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
        dbg64_str(" step=");                                  // mv_step：失败时是出错那一行的行号（0 = 全过）
        dbg64_dec((uint64_t)mv_step);
        dbg64_nl();
        if (!ok) fails |= 4096;
    }
    // ---- bit13..15（8192/16384/32768）：★ 批次 M —— 二级间接块（大文件）/ 回收 / 上限边界 ----
    // 在假盘的**后半段**另起一个 2MB 卷（前半段既有自检的内容不动），走真实读写路径。
    {
        Vfs64Geom saved_big;
        geom_save(&saved_big);
        const int save_cur_b = g_cur_slot;
        const int save_sys_b = g_system_slot;
        bool ok = true;
        g_fake_active = true;
        g_cur_slot = 0;
        uint32_t fb0 = 0, fb1 = 0, fb2 = 0;
        const uint32_t BIGLEN = 1536u * 1024u;                        // 3072 个块（跨一级 + 二级间接）
        const uint32_t blk_need = vfs64_blocks_for_bytes64(BIGLEN);
        static uint8_t chunk[4096];

        if (vfs64_format(0, VFS64_FAKE_BIG_LBA, VFS64_FAKE_SECTORS - VFS64_FAKE_BIG_LBA) != 0) ok = false;
        if (vfs64_free64(&fb0, nullptr, nullptr) != 0) ok = false;
        if (g_lay->version != VFS64_VERSION || g_lay->dind_off == 0) ok = false;   // 大文件自检必须是 v3 卷

        // ---- bit13(8192)：流式写 1.5MB（生成式数据源）-> 分块读回逐字节 + CRC 一致 + 块数记账 ----
        if (ok) {
            if (vfs64_write_stream64("/big.bin", BIGLEN, sel_src64, nullptr) != (int)BIGLEN) {
                ok = false;
                log_line("bigfile write FAIL");
            }
            if (vfs64_free64(&fb1, nullptr, nullptr) != 0) ok = false;
            if (fb0 < fb1 || (fb0 - fb1) != blk_need) {               // 正好少掉"数据块 + 一级 + 二级 + 子块"
                ok = false;
                log_line("bigfile block accounting FAIL");
            }
            uint8_t ino[VFS64_INODE_BYTES_MAX];
            if (lookup_file64("/big.bin", "selftest.big", nullptr, ino) != 0) ok = false;
            else if (rd32(ino + VFS_I3_DIND) == 0 || rd32(ino + VFS_I_IND) == 0) {
                ok = false;
                log_line("bigfile dind/ind not used FAIL");           // 真的用了二级间接块才算数
            } else if (rd32(ino + VFS_I_SIZE) != BIGLEN) {
                ok = false;
                log_line("bigfile size FAIL");
            }
            uint32_t off = 0, crc = 0, bad_at = 0xFFFFFFFFu;
            while (off < BIGLEN) {
                uint32_t want = BIGLEN - off;
                if (want > sizeof(chunk)) want = sizeof(chunk);
                uint32_t got = 0;
                if (vfs64_read_at64("/big.bin", off, chunk, want, &got) != 0 || got != want) {
                    ok = false;
                    log_line("bigfile read FAIL");
                    break;
                }
                for (uint32_t i = 0; i < got; i++)
                    if (chunk[i] != pat64(off + i) && bad_at == 0xFFFFFFFFu) bad_at = off + i;
                crc = crc32_cont64(crc, chunk, got);
                off += got;
            }
            if (bad_at != 0xFFFFFFFFu) { ok = false; log_line("bigfile byte compare FAIL"); }
            uint32_t got2 = 123;
            if (vfs64_read_at64("/big.bin", BIGLEN, chunk, 16, &got2) != 0 || got2 != 0) {
                ok = false;                                          // off >= size：0 字节（正常 EOF），不是错误
                log_line("bigfile EOF read FAIL");
            }
            // ★ 回收的一半：删掉这个 1.5MB 文件后，空闲块数必须回到写之前的基线（间接块/子块都不能漏）
            if (vfs64_unlink64("/big.bin") != 0) ok = false;
            uint32_t fb_del = 0;
            if (vfs64_free64(&fb_del, nullptr, nullptr) != 0 || fb_del != fb0) {
                ok = false;
                log_line("bigfile unlink leak FAIL");
            }
            dbg64_str("[VFS64] bigfile ok bytes=");
            dbg64_dec(BIGLEN);
            dbg64_str(" blocks=");
            dbg64_dec(blk_need);
            dbg64_str(" ind=1 dind=1 free_delta=");
            dbg64_dec(fb0 - fb1);
            dbg64_str(" unlink_back_to_base=");
            dbg64_dec((fb_del == fb0) ? 1u : 0u);
            dbg64_str(" crc=0x");
            log_hex32(crc);
            dbg64_nl();
            if (!ok) fails |= 8192;
        }

        // ---- bit14(16384)：回收不泄漏（写-删-写 2 轮回到基线）+ 空间不足**预检**不留半截 ----
        if (ok) {
            bool rec = true;
            uint32_t fbA = 0;
            if (vfs64_free64(&fbA, nullptr, nullptr) != 0) rec = false;      // ★ 本段的基线（此刻 /big.bin 还在）
            for (int round = 0; round < 2; round++) {
                if (vfs64_write_stream64("/rec.bin", BIGLEN, sel_src64, nullptr) != (int)BIGLEN) rec = false;
                if (vfs64_unlink64("/rec.bin") != 0) rec = false;
                uint32_t fbn = 0;
                if (vfs64_free64(&fbn, nullptr, nullptr) != 0 || fbn != fbA) rec = false;
            }
            // 上限：off 落在上限之内、"跨过上限 1 字节"的部分写 -> 必须被上限检查先拒掉
            const uint32_t over_byte = pat64(0);
            const uint32_t off_over = VFS64_MAX_FILE_BYTES - 4u;
            chunk[0] = (uint8_t)over_byte;
            if (vfs64_write_at64("/huge.bin", off_over, chunk, 8) != -1) {
                rec = false;
                log_line("bigfile over-limit not rejected FAIL");
            }
            uint32_t ty = 0, sz = 0;
            if (vfs64_stat("/huge.bin", &ty, &sz) == 0) rec = false;          // 没留下半截文件
            // 真·空间预检：想写 4MiB 而卷只有 2MB -> 预检直接失败（也不留文件）
            if (vfs64_write_stream64("/big2.bin", 4u * 1024u * 1024u, sel_src64, nullptr) != -1) {
                rec = false;
                log_line("bigfile ENOSPC pre-flight not refused FAIL");
            }
            if (vfs64_stat("/big2.bin", &ty, &sz) == 0) rec = false;
            if (vfs64_free64(&fb2, nullptr, nullptr) != 0 || fb2 != fbA) rec = false;   // 失败的尝试不留垃圾块
            dbg64_str("[VFS64] recycle ok rounds=2 free_before=");
            dbg64_dec(fbA);
            dbg64_str(" free_after=");
            dbg64_dec(fb2);
            dbg64_str(" delta=");
            dbg64_dec((fbA > fb2) ? (fbA - fb2) : (fb2 - fbA));
            dbg64_nl();
            if (!rec) fails |= 16384;
        }

        // ---- bit15(32768)：上限边界 + 整体重写（变小要释放多余块）+ 覆盖成 0 字节后全部回收 ----
        if (ok) {
            bool lim = true;
            uint8_t tiny[8];
            for (uint32_t i = 0; i < sizeof(tiny); i++) tiny[i] = pat64(i);
            uint32_t fbL = 0;
            if (vfs64_free64(&fbL, nullptr, nullptr) != 0) lim = false;      // ★ 本段自己的基线
            // ① 越过上限：write_at 从 off = 上限 开始写 1 字节、write64 写"上限 + 1"字节 —— 都必须被拒
            if (vfs64_write_at64("/lim.bin", VFS64_MAX_FILE_BYTES, tiny, 1) != -1) lim = false;
            if (vfs64_write64("/lim.bin", tiny, (int)(VFS64_MAX_FILE_BYTES + 1)) != -1) lim = false;
            uint32_t ty = 0, sz = 0;
            if (vfs64_stat("/lim.bin", &ty, &sz) == 0) lim = false;          // 两个请求都不该建出文件
            // ② 上限内的普通写（1 块）+ 整体重写成 700KB（多余块必须释放）
            if (vfs64_write_at64("/lim.bin", 0, tiny, 8) != 0) lim = false;
            uint32_t f1 = 0;
            if (vfs64_free64(&f1, nullptr, nullptr) != 0 || fbL < f1 || (fbL - f1) != 1u) lim = false;
            if (vfs64_write_stream64("/lim.bin", 700u * 1024u, sel_src64, nullptr) != (int)(700u * 1024u)) lim = false;
            uint32_t f700 = 0;
            if (vfs64_free64(&f700, nullptr, nullptr) != 0) lim = false;
            if (fbL < f700 || (fbL - f700) != vfs64_blocks_for_bytes64(700u * 1024u)) lim = false;
            uint32_t got = 0;
            if (vfs64_read_at64("/lim.bin", 700u * 1024u, chunk, 16, &got) != 0 || got != 0) lim = false;
            if (vfs64_read_at64("/lim.bin", 699u * 1024u, chunk, 1024, &got) != 0 || got != 1024) lim = false;
            for (uint32_t i = 0; i < got; i++) if (chunk[i] != pat64(699u * 1024u + i)) lim = false;
            // ③ 整体重写成 0 字节：全部块回收，文件仍然是合法空文件
            if (vfs64_write64("/lim.bin", tiny, 0) != 0) lim = false;
            uint32_t f0 = 0;
            if (vfs64_free64(&f0, nullptr, nullptr) != 0 || f0 != fbL) lim = false;
            if (vfs64_stat("/lim.bin", &ty, &sz) != 0 || sz != 0 || ty != VFS64_TYPE_FILE) lim = false;
            dbg64_str("[VFS64] limit ok max=");
            dbg64_dec(VFS64_MAX_FILE_BYTES);
            dbg64_str(" over_rejected=2 rewrite_shrink=1 truncate_free=1");
            dbg64_nl();
            if (!lim) fails |= 32768;
        }

        g_fake_active = false;
        geom_restore(&saved_big);
        g_cur_slot = save_cur_b;
        g_system_slot = save_sys_b;
    }
    vfs64_set_cred64(sel_cred.uid, sel_cred.gid, sel_cred.euid, sel_cred.egid);
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
