// fat64.cpp - 最小 FAT32 写入器（安装程序在目标盘上建 ESP 用）
//
// 与 tools/make_esp.py 的关系：卷格式算法**逐条对齐**（512B 扇区、SPC=1、32 个保留扇区、
// 2 份 32 位 FAT、根目录 = 从簇 2 开始的簇链、FSInfo 扇区 1、备份引导扇区 6、
// BPB 字段与卷标完全一致）。差别只在实现方式：
//   * Python 版先把整卷放进内存再一次性写出（构建期，随便用内存）；
//   * 内核版按扇区流式写盘（安装期，数据要落真实磁盘，且内核内存有限）。
// 自检（fat64_selftest64）用同一套代码在一段**内存卷**上做完整往返，保证"卷结构自洽"
// 是被验证过的，而不是靠人读代码；内存卷从页池临时取页（34MB），跑完就还回去。
#include "fat64.h"
#include "ata64.h"
#include "debug64.h"
#include "mem_64.h"          // page_alloc_64 / page_free_64（自检内存卷用）

// ---------------- 卷状态（一次只操作一个卷） ----------------
struct Fat64Chain { uint32_t start, count; };
struct Fat64Dir   { char path[24]; uint32_t cluster; uint32_t next_slot; };

static bool     g_valid    = false;
static int      g_drive    = -1;
static uint32_t g_start    = 0;      // 卷在盘上的起始 LBA
static uint32_t g_fatsz    = 0;      // 每个 FAT 的扇区数
static uint32_t g_data_start = 0;    // 数据区起始（卷相对扇区号）
static uint32_t g_clusters = 0;      // 数据区簇数
static uint32_t g_next_cluster = FAT64_ROOT_CLUSTER;
static Fat64Chain g_chains[FAT64_MAX_CHAINS];
static int        g_nchains = 0;
static Fat64Dir   g_dirs[FAT64_MAX_DIRS];
static int        g_ndirs   = 0;

// 自检用：把卷建在内存里（不碰真盘）。
// ★ 为什么按 4KB 页拼而不是一个 static 数组：内存卷要 34MB（FAT32 的簇数硬下限决定的），
//   而内核 .bss 已经到 ~40MB，再加 34MB 就撞上物理 0x04000000（64MB）的载荷缓冲
//   （安装介质的 SYSTEM.IMG 就放那儿）—— 会把载荷覆盖掉。改成运行时从页池（>=128MB）
//   取页 + 一张页指针表，既不占 .bss 也不撞载荷。
static bool       g_ram      = false;
static uint8_t*   g_ram_page[FAT64_RAM_MAX_PAGES];
static uint32_t   g_ram_pages   = 0;
static uint32_t   g_ram_sectors = 0;

// I/O 缓冲（.bss，不占镜像体积）
static const uint32_t FAT64_IO_SECTORS = 128;                       // 64KB 一块
static uint8_t g_blk[FAT64_IO_SECTORS * FAT64_SECTOR];
static uint8_t g_fat[FAT64_MAX_FAT_SECTORS * FAT64_SECTOR];         // 48MB 卷的 FAT = 756 扇区
static uint8_t g_sec[FAT64_SECTOR];

static void memzero8(uint8_t* p, uint32_t n) { for (uint32_t i = 0; i < n; i++) p[i] = 0; }
static void memcopy8(uint8_t* d, const uint8_t* s, uint32_t n) { for (uint32_t i = 0; i < n; i++) d[i] = s[i]; }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---------------- 内存卷（自检专用；页池分配，跨页自动切） ----------------
static bool ram_alloc(uint32_t sectors) {
    const uint32_t bytes = sectors * FAT64_SECTOR;
    const uint32_t need  = (bytes + FAT64_RAM_PAGE - 1) / FAT64_RAM_PAGE;
    if (need > FAT64_RAM_MAX_PAGES) return false;
    g_ram_pages = 0;
    for (uint32_t i = 0; i < need; i++) {
        uint8_t* p = (uint8_t*)page_alloc_64();
        if (!p) {                       // 页池不够（内存很小的机器）：把已拿到的还回去
            for (uint32_t k = 0; k < g_ram_pages; k++) page_free_64(g_ram_page[k]);
            g_ram_pages = 0;
            return false;
        }
        memzero8(p, FAT64_RAM_PAGE);
        g_ram_page[g_ram_pages++] = p;
    }
    g_ram_sectors = sectors;
    return true;
}
static void ram_release(void) {
    for (uint32_t k = 0; k < g_ram_pages; k++) page_free_64(g_ram_page[k]);
    g_ram_pages = 0; g_ram_sectors = 0;
}
static bool ram_xfer(uint32_t vol_lba, uint32_t count, uint8_t* dst, const uint8_t* src) {
    if (vol_lba + count > g_ram_sectors) return false;
    uint64_t off = (uint64_t)vol_lba * FAT64_SECTOR;
    uint32_t n   = count * FAT64_SECTOR;
    while (n) {
        const uint32_t pi = (uint32_t)(off / FAT64_RAM_PAGE);
        const uint32_t po = (uint32_t)(off % FAT64_RAM_PAGE);
        if (pi >= g_ram_pages) return false;
        uint32_t chunk = FAT64_RAM_PAGE - po;
        if (chunk > n) chunk = n;
        if (src) memcopy8(g_ram_page[pi] + po, src, chunk);
        if (dst) memcopy8(dst, g_ram_page[pi] + po, chunk);
        if (src) src += chunk;
        if (dst) dst += chunk;
        off += chunk;
        n   -= chunk;
    }
    return true;
}
// 自检直接读内存卷的一个字节/字（只对内存卷有效）
static uint8_t ram_peek(uint32_t off) {
    const uint32_t pi = off / FAT64_RAM_PAGE;
    if (pi >= g_ram_pages) return 0;
    return g_ram_page[pi][off % FAT64_RAM_PAGE];
}
static uint16_t ram_u16(uint32_t off) { return (uint16_t)(ram_peek(off) | ((uint16_t)ram_peek(off + 1) << 8)); }
static uint32_t ram_u32(uint32_t off) {
    return (uint32_t)ram_peek(off) | ((uint32_t)ram_peek(off + 1) << 8)
         | ((uint32_t)ram_peek(off + 2) << 16) | ((uint32_t)ram_peek(off + 3) << 24);
}

// ---------------- 卷 I/O（真盘 / 内存卷两种后端） ----------------
static bool vol_write(uint32_t vol_lba, uint32_t count, const uint8_t* data) {
    if (g_ram) return ram_xfer(vol_lba, count, nullptr, data);
    if (g_drive < 0) return false;
    return ata64_write(g_drive, g_start + vol_lba, count, data);
}
static bool vol_read(uint32_t vol_lba, uint32_t count, uint8_t* data) {
    if (g_ram) return ram_xfer(vol_lba, count, data, nullptr);
    if (g_drive < 0) return false;
    return ata64_read(g_drive, g_start + vol_lba, count, data);
}

// ---------------- 几何：与 make_esp.py 的 _fat32_geometry 同一算法 ----------------
// 32 位 FAT 项（每簇 4 字节）；FAT32 **没有**固定根目录区（根目录是一条簇链）。
static bool geometry(uint32_t total, uint32_t* out_fatsz, uint32_t* out_clusters, uint32_t* out_data) {
    uint32_t fsz = 1;
    for (;;) {
        if (total <= FAT64_RESERVED + FAT64_NUM_FATS * fsz) return false;
        const uint32_t data = total - FAT64_RESERVED - FAT64_NUM_FATS * fsz;
        const uint32_t cl   = data / FAT64_SPC;
        const uint32_t need = ((cl + 2) * 4 + FAT64_SECTOR - 1) / FAT64_SECTOR;
        if (need <= fsz) { *out_fatsz = fsz; *out_clusters = cl; *out_data = data; return true; }
        fsz = need;
        if (fsz > FAT64_MAX_FAT_SECTORS) return false;      // 卷大得不像 FAT32 了
    }
}

// ---------------- 簇链 / FAT 表（32 位项） ----------------
static uint32_t cluster_lba(uint32_t c) { return g_data_start + (c - FAT64_ROOT_CLUSTER) * FAT64_SPC; }

static uint32_t fat_value(uint32_t c) {
    if (c == 0) return 0x0FFFFFF8u;                         // 0x0FFFFFFF | media
    if (c == 1) return FAT64_EOF;
    for (int i = 0; i < g_nchains; i++) {
        const uint32_t s = g_chains[i].start, n = g_chains[i].count;
        if (c >= s && c < s + n) return (c + 1 < s + n) ? (c + 1) : FAT64_EOF;
    }
    return 0;
}

static void log_fail(const char* what) {
    dbg64_str("[FAT64] FAIL ");
    dbg64_str(what);
    dbg64_nl();
}

static bool write_fsinfo(void) {
    // FSInfo（FAT32 规范）：free / next-free 都按真实状态写。
    // 有些驱动（含 EDK2 的 FA 路径）按 free 判断剩余空间，所以每次分配后同步更新。
    const uint32_t used = g_next_cluster - FAT64_ROOT_CLUSTER;
    memzero8(g_sec, FAT64_SECTOR);
    wr32(g_sec + 0,   0x41615252u);                         // 起始签名
    wr32(g_sec + 484, 0x61417272u);                         // 结构签名
    wr32(g_sec + 488, (used <= g_clusters) ? (g_clusters - used) : 0);
    wr32(g_sec + 492, (g_next_cluster <= g_clusters + 1) ? g_next_cluster : 0xFFFFFFFFu);
    wr32(g_sec + 508, 0xAA550000u);                         // 尾签名
    g_sec[510] = 0x55; g_sec[511] = 0xAA;
    if (!vol_write(FAT64_FSINFO_SEC, 1, g_sec)) { log_fail("fsinfo-write"); return false; }
    // 备份 FSInfo（规范建议备份区 6..8：6=备份引导，7=备份 FSInfo）
    if (!vol_write(FAT64_FSINFO_SEC + FAT64_BKBOOT_SEC, 1, g_sec)) { log_fail("fsinfo-backup"); return false; }
    return true;
}

// 把两份 FAT 表都写出去（每次分配后调用；48MB 卷 = 756 扇区/份）
static bool write_fats(void) {
    const uint32_t bytes = g_fatsz * FAT64_SECTOR;
    if (bytes > sizeof(g_fat)) { log_fail("fat-size"); return false; }
    memzero8(g_fat, bytes);
    const uint32_t n = g_clusters + 2;
    for (uint32_t c = 0; c < n; c++) {
        wr32(g_fat + c * 4, fat_value(c) & 0x0FFFFFFFu);
    }
    for (uint32_t k = 0; k < FAT64_NUM_FATS; k++) {
        if (!vol_write(FAT64_RESERVED + k * g_fatsz, g_fatsz, g_fat)) { log_fail("fat-write"); return false; }
    }
    return write_fsinfo();
}

static bool alloc_chain(uint32_t n, uint32_t* out_start) {
    if (n == 0) n = 1;
    if (g_next_cluster + n > g_clusters + FAT64_ROOT_CLUSTER) { log_fail("volume-full"); return false; }
    if (g_nchains >= FAT64_MAX_CHAINS) { log_fail("too-many-chains"); return false; }
    g_chains[g_nchains].start = g_next_cluster;
    g_chains[g_nchains].count = n;
    g_nchains++;
    *out_start = g_next_cluster;
    g_next_cluster += n;
    return true;
}

// ---------------- 8.3 短名 / 目录项 ----------------
static bool name83(const char* n, uint8_t out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int p = 0, e = 8, i = 0;
    bool in_ext = false;
    if (!n || !n[0]) return false;
    for (; n[i]; i++) {
        char c = n[i];
        if (c == '.') { if (in_ext) return false; in_ext = true; continue; }
        if (c == ' ' || c == '/' || c == '\\') return false;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (in_ext) { if (e >= 11) return false; out[e++] = (uint8_t)c; }
        else        { if (p >= 8)  return false; out[p++] = (uint8_t)c; }
    }
    return p > 0;
}

// 目录项（32 字节；不含 LFN）。★ FAT32 的起始簇是 **32 位**：
//   低 16 位在偏移 26（DIR_FstClusLO）、高 16 位在偏移 20（DIR_FstClusHI）。
//   FAT16 只写偏移 26 —— 那份代码直接搬到 FAT32 上会让所有簇号变 0（多簇文件全挂）。
static void make_entry(uint8_t out[32], const uint8_t name11[11], uint8_t attr,
                       uint32_t cluster, uint32_t size) {
    memzero8(out, 32);
    memcopy8(out, name11, 11);
    out[11] = attr;
    wr16(out + 20, (uint16_t)((cluster >> 16) & 0xFFFF));
    wr16(out + 26, (uint16_t)(cluster & 0xFFFF));
    wr32(out + 28, size);
}

// 在目录 dir_idx 的第 slot 项写一个目录项。
// FAT32 里**根目录也是数据区的簇链**（dir_idx = 0 的 cluster = 簇 2），不再有固定根目录区。
static bool dir_write_entry(int dir_idx, uint32_t slot, const uint8_t name11[11], uint8_t attr,
                            uint32_t cluster, uint32_t size) {
    if (dir_idx < 0 || dir_idx >= g_ndirs) return false;
    uint8_t ent[32];
    make_entry(ent, name11, attr, cluster, size);
    const uint32_t per_cluster = (FAT64_SPC * FAT64_SECTOR) / 32;
    if (slot >= per_cluster) { log_fail("dir-full (one cluster per dir)"); return false; }
    const uint32_t vol_lba = cluster_lba(g_dirs[dir_idx].cluster) + (slot * 32) / FAT64_SECTOR;
    if (!vol_read(vol_lba, 1, g_sec)) return false;
    memcopy8(g_sec + (slot * 32) % FAT64_SECTOR, ent, 32);
    return vol_write(vol_lba, 1, g_sec);
}

static int find_dir(const char* path) {
    for (int i = 0; i < g_ndirs; i++) {
        const char* a = g_dirs[i].path; const char* b = path;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return i;
    }
    return -1;
}

// "EFI/BOOT/X.BIN" -> 父目录 "EFI/BOOT" + 叶子 "X.BIN"
static bool split_path(const char* path, char* parent, int parent_cap, char* leaf, int leaf_cap) {
    int last = -1, n = 0;
    for (; path[n]; n++) if (path[n] == '/') last = n;
    const int leaf_len = n - last - 1;
    if (leaf_len <= 0 || leaf_len >= leaf_cap) return false;      // 越界检查只看**叶子名**长度
    if (leaf_len > 12) return false;                              // 8.3 短名上限（8+1+3）
    if (last < 0) { parent[0] = 0; }
    else {
        if (last >= parent_cap) return false;
        for (int i = 0; i < last; i++) parent[i] = path[i];
        parent[last] = 0;
    }
    for (int i = last + 1; i < n; i++) leaf[i - last - 1] = path[i];
    leaf[leaf_len] = 0;
    return true;
}

// ---------------- 格式化 ----------------
// format_volume：真正的格式化实现。**用当前 I/O 后端**（真盘 = g_drive，内存 = g_ram）。
// ★ 为什么把"切后端"留在外面：自检要在内存卷上跑同一套代码，而"真盘调用"绝不能
//   意外跑在内存后端上、反之"自检"也绝不能碰真盘 —— 之前这里在实现里强制 g_ram=false，
//   结果自检的 format 落到了真盘 drive 0（实测把安装介质 ISO 的前 2.5MB 覆盖掉了！）。
static int format_volume(uint32_t start_lba, uint32_t sectors) {
    g_valid = false;
    // 太小的卷解不出 65525 簇（FAT32 的硬下限），在**任何 I/O 之前**就拒绝
    if (sectors < FAT64_MIN_SECTORS) {
        dbg64_str("[FAT64] format reject sectors=");
        dbg64_dec(sectors);
        dbg64_str(" (< ");
        dbg64_dec(FAT64_MIN_SECTORS);
        dbg64_str("，装不下 FAT32 的 65525 簇)");
        dbg64_nl();
        return -1;
    }
    uint32_t fatsz = 0, clusters = 0, data = 0;
    if (!geometry(sectors, &fatsz, &clusters, &data)) { log_fail("geometry"); return -1; }
    (void)data;                                            // 数据区扇区数只用于推导，不需要另存
    // ★ 硬断言（与 make_esp.py 一致，镜像版的 FAT16/FAT12 陷阱）：簇数 >= 65525 才是真 FAT32。
    //   少于这个数就应被当作 FAT16 —— EDK2 会按 16 位读我们的 32 位 FAT 表，多簇文件
    //   （UEFI64.BIN / KERNEL64.BIN）读取会报 EFI_VOLUME_CORRUPTED。
    if (clusters < FAT64_CLUSTER_MIN) {
        dbg64_str("[FAT64] format reject clusters=");
        dbg64_dec(clusters);
        dbg64_str(" (< 65525，不满足 FAT32 簇数下界)");
        dbg64_nl();
        return -1;
    }

    g_start = start_lba;                    // 注意：**不动 g_ram/g_drive**（后端由调用方决定）
    g_fatsz = fatsz; g_clusters = clusters;
    g_data_start = FAT64_RESERVED + FAT64_NUM_FATS * fatsz;      // FAT32：没有固定根目录区
    g_next_cluster = FAT64_ROOT_CLUSTER;
    g_nchains = 0; g_ndirs = 1;
    g_dirs[0].path[0] = 0; g_dirs[0].cluster = 0; g_dirs[0].next_slot = 0;

    // ---- 根目录簇（FAT32：根目录是一条以簇 2 开头的簇链）----
    uint32_t root_c = 0;
    if (!alloc_chain(1, &root_c) || root_c != FAT64_ROOT_CLUSTER) { log_fail("root-cluster"); return -1; }
    g_dirs[0].cluster = root_c;

    // ---- 引导扇区（FAT32 BPB；EFI 不执行它的代码，但字段必须自洽）----
    memzero8(g_sec, FAT64_SECTOR);
    g_sec[0] = 0xEB; g_sec[1] = 0x58; g_sec[2] = 0x90;
    memcopy8(g_sec + 3, (const uint8_t*)"VIMTU64 ", 8);         // OEM
    wr16(g_sec + 11, (uint16_t)FAT64_SECTOR);                   // 每扇区字节
    g_sec[13] = (uint8_t)FAT64_SPC;                             // 每簇扇区
    wr16(g_sec + 14, (uint16_t)FAT64_RESERVED);                 // 保留扇区（FAT32 必须 >= 32）
    g_sec[16] = (uint8_t)FAT64_NUM_FATS;
    wr16(g_sec + 17, 0);                                        // ★ FAT32：RootEntCnt 必须 0
    wr16(g_sec + 19, 0);                                        // ★ FAT32：TotSec16 必须 0
    g_sec[21] = FAT64_MEDIA;
    wr16(g_sec + 22, 0);                                        // ★ FAT32：FATSz16 必须 0
    wr16(g_sec + 24, 32);                                       // 每道扇区（CHS 无意义，填常用值）
    wr16(g_sec + 26, 64);                                       // 磁头
    wr32(g_sec + 28, 0);                                        // 隐藏扇区
    wr32(g_sec + 32, sectors);                                  // 总扇区（32 位）
    wr32(g_sec + 36, fatsz);                                    // ★ BPB_FATSz32
    wr16(g_sec + 40, 0);                                        // ExtFlags：0 = 两份 FAT 互为镜像
    wr16(g_sec + 42, 0);                                        // FSVer 0.0
    wr32(g_sec + 44, FAT64_ROOT_CLUSTER);                       // ★ BPB_RootClus = 2
    wr16(g_sec + 48, (uint16_t)FAT64_FSINFO_SEC);               // ★ BPB_FSInfo = 1
    wr16(g_sec + 50, (uint16_t)FAT64_BKBOOT_SEC);               // ★ BPB_BkBootSec = 6
    g_sec[64] = 0x80;                                           // 驱动器号
    g_sec[66] = 0x29;                                           // 扩展引导签名
    wr32(g_sec + 67, 0x56494D54);                               // 卷序号 'VIMT'
    memcopy8(g_sec + 71, (const uint8_t*)"VIMTU64ESP ", 11);    // 卷标
    memcopy8(g_sec + 82, (const uint8_t*)"FAT32   ", 8);        // 文件系统类型
    g_sec[510] = 0x55; g_sec[511] = 0xAA;
    if (!vol_write(0, 1, g_sec)) { log_fail("boot-sector"); return -1; }

    // ---- 备份引导扇区（6 号扇区必须逐字节等于 0 号扇区：固件/修复工具会拿它兜底）----
    if (!vol_write(FAT64_BKBOOT_SEC, 1, g_sec)) { log_fail("backup-boot"); return -1; }

    // ---- FAT 表（含根目录簇的链尾）+ FSInfo（free/next-free）----
    if (!write_fats()) return -1;

    // ---- 根目录簇清零（1 簇 = 512B = 16 个目录项，本用途最多 4 项）----
    memzero8(g_sec, FAT64_SECTOR);
    for (uint32_t i = 0; i < FAT64_SPC; i++) {
        if (!vol_write(cluster_lba(root_c) + i, 1, g_sec)) { log_fail("root-dir"); return -1; }
    }

    g_valid = true;
    dbg64_str("[FAT64] format lba=");
    dbg64_dec(start_lba);
    dbg64_str(" sectors=");
    dbg64_dec(sectors);
    dbg64_str(" fs=FAT32 clusters=");
    dbg64_dec(clusters);
    dbg64_str(" free=");
    dbg64_dec(g_clusters - (g_next_cluster - FAT64_ROOT_CLUSTER));
    dbg64_str(" fat_sectors=");
    dbg64_dec(fatsz);
    dbg64_str(" spc=");
    dbg64_dec(FAT64_SPC);
    dbg64_nl();
    return 0;
}

// 真盘入口：**显式切到真盘后端**再格式化（自检绝不走这里）
int fat64_format64(int drive, uint32_t start_lba, uint32_t sectors) {
    if (drive < 0) {
        dbg64_str("[FAT64] format reject drive<0");
        dbg64_nl();
        return -1;
    }
    g_ram = false;
    g_ram_pages = 0; g_ram_sectors = 0;          // 真盘后端：不看页表
    g_drive = drive;
    return format_volume(start_lba, sectors);
}


uint32_t fat64_last_clusters64() { return g_clusters; }


// ---------------- 建目录 ----------------
int fat64_mkdir64(int drive, const char* path8_3) {
    if (!g_valid || (drive != g_drive && !g_ram)) { log_fail("mkdir: volume not ready"); return -1; }
    char parent[24], leaf[16];
    if (!path8_3 || !split_path(path8_3, parent, sizeof(parent), leaf, sizeof(leaf))) {
        log_fail("mkdir: bad path");
        return -1;
    }
    const int pi = find_dir(parent);
    if (pi < 0) { log_fail("mkdir: parent missing"); return -1; }
    if (find_dir(path8_3) >= 0) return 0;                    // 幂等
    if (g_ndirs >= FAT64_MAX_DIRS) { log_fail("mkdir: too many dirs"); return -1; }
    uint8_t nm[11];
    if (!name83(leaf, nm)) { log_fail("mkdir: bad name"); return -1; }

    uint32_t c = 0;
    if (!alloc_chain(1, &c)) return -1;
    const uint32_t slot = g_dirs[pi].next_slot;
    if (!dir_write_entry(pi, slot, nm, 0x10, c, 0)) { log_fail("mkdir: dir entry"); return -1; }
    // ★ FAT32 的目录项是 32 位簇号（低 16 位 @26 + 高 16 位 @20）：make_entry 两个都写。
    //   （自检里会从卷上读回 EFI/BOOT/TEST.BIN 的簇号并沿 32 位链读数据 —— 只写低 16 位的
    //   FAT16 写法会在那里暴露。）
    g_dirs[pi].next_slot++;

    // 新目录的第一、二项固定是 "." 和 ".."（FAT 规范的硬要求：固件/Shell 靠它们解析路径）
    uint8_t buf[FAT64_SECTOR];
    memzero8(buf, sizeof(buf));
    uint8_t dot[11], dotdot[11];
    for (int i = 0; i < 11; i++) { dot[i] = ' '; dotdot[i] = ' '; }
    dot[0] = '.'; dotdot[0] = '.'; dotdot[1] = '.';
    // ".." 的簇号：父目录是根目录时写 0（FAT 惯例："0 = 根"，与 make_esp.py 一致）
    const uint32_t parent_c = (g_dirs[pi].cluster == FAT64_ROOT_CLUSTER) ? 0 : g_dirs[pi].cluster;
    make_entry(buf, dot, 0x10, c, 0);
    make_entry(buf + 32, dotdot, 0x10, parent_c, 0);
    if (!vol_write(cluster_lba(c), 1, buf)) { log_fail("mkdir: dot entries"); return -1; }

    int n = 0;
    while (path8_3[n] && n < (int)sizeof(g_dirs[0].path) - 1) { g_dirs[g_ndirs].path[n] = path8_3[n]; n++; }
    g_dirs[g_ndirs].path[n] = 0;
    g_dirs[g_ndirs].cluster = c;
    g_dirs[g_ndirs].next_slot = 2;
    g_ndirs++;

    if (!write_fats()) return -1;
    dbg64_str("[FAT64] mkdir path=");
    dbg64_str(path8_3);
    dbg64_str(" cluster=");
    dbg64_dec(c);
    dbg64_nl();
    return 0;
}

// ---------------- 写文件（公共部分） ----------------
// 建目录项 + 分配簇链；数据由 caller 用 file_write_data 流式写。
static int file_begin(const char* path8_3, uint32_t len, uint32_t* out_cluster, uint32_t* out_dirs,
                      uint32_t* out_slot) {
    char parent[24], leaf[16];
    if (!path8_3 || !split_path(path8_3, parent, sizeof(parent), leaf, sizeof(leaf))) {
        log_fail("file: bad path");
        return -1;
    }
    const int pi = find_dir(parent);
    if (pi < 0) { log_fail("file: parent missing"); return -1; }
    uint8_t nm[11];
    if (!name83(leaf, nm)) { log_fail("file: bad name"); return -1; }
    const uint32_t ncls = (len + FAT64_SPC * FAT64_SECTOR - 1) / (FAT64_SPC * FAT64_SECTOR);
    uint32_t c = 0;
    if (!alloc_chain(ncls ? ncls : 1, &c)) return -1;
    const uint32_t slot = g_dirs[pi].next_slot;
    if (!dir_write_entry(pi, slot, nm, 0x20, c, len)) { log_fail("file: dir entry"); return -1; }
    g_dirs[pi].next_slot++;
    *out_cluster = c; *out_dirs = g_dirs[pi].cluster; *out_slot = slot;
    return 0;
}

static void log_file(const char* path8_3, uint32_t len, uint32_t c) {
    dbg64_str("[FAT64] file path=");
    dbg64_str(path8_3);
    dbg64_str(" bytes=");
    dbg64_dec(len);
    dbg64_str(" cluster=");
    dbg64_dec(c);
    dbg64_nl();
}

int fat64_write_file64(int drive, const char* path8_3, const uint8_t* data, uint32_t len) {
    if (!g_valid || (drive != g_drive && !g_ram)) { log_fail("file: volume not ready"); return -1; }
    if (!data && len) { log_fail("file: null data"); return -1; }
    uint32_t c = 0, d0 = 0, slot = 0;
    if (file_begin(path8_3, len, &c, &d0, &slot) != 0) return -1;
    (void)d0; (void)slot;
    const uint32_t total_bytes = ((len + FAT64_SPC * FAT64_SECTOR - 1) / (FAT64_SPC * FAT64_SECTOR))
                                 * FAT64_SPC * FAT64_SECTOR;      // 末簇补零
    uint32_t done = 0;
    while (done < total_bytes) {
        uint32_t n = total_bytes - done;
        if (n > sizeof(g_blk)) n = sizeof(g_blk);
        memzero8(g_blk, n);
        if (done < len) {
            uint32_t avail = len - done;
            if (avail > n) avail = n;
            memcopy8(g_blk, data + done, avail);
        }
        const uint32_t secs = (n + FAT64_SECTOR - 1) / FAT64_SECTOR;
        if (!vol_write(cluster_lba(c) + done / FAT64_SECTOR, secs, g_blk)) { log_fail("file: data write"); return -1; }
        done += secs * FAT64_SECTOR;
    }
    if (!write_fats()) return -1;
    log_file(path8_3, len, c);
    return 0;
}

int fat64_write_file_from_disk64(int drive, const char* path8_3, int src_drive,
                                 uint32_t src_lba, uint32_t len) {
    if (!g_valid || drive != g_drive) { log_fail("file: volume not ready"); return -1; }
    if (src_drive < 0 || len == 0) { log_fail("file: bad source"); return -1; }
    uint32_t c = 0, d0 = 0, slot = 0;
    if (file_begin(path8_3, len, &c, &d0, &slot) != 0) return -1;
    (void)d0; (void)slot;
    const uint32_t total_bytes = ((len + FAT64_SPC * FAT64_SECTOR - 1) / (FAT64_SPC * FAT64_SECTOR))
                                 * FAT64_SPC * FAT64_SECTOR;
    uint32_t done = 0;
    while (done < total_bytes) {
        uint32_t n = total_bytes - done;
        if (n > sizeof(g_blk)) n = sizeof(g_blk);
        const uint32_t secs = (n + FAT64_SECTOR - 1) / FAT64_SECTOR;
        // 源数据不足一扇区的尾部先清零（本用途 len 是 512 的整数倍，这里只是兜底）
        if (done + secs * FAT64_SECTOR > len) memzero8(g_blk, sizeof(g_blk));
        if (!ata64_read(src_drive, src_lba + done / FAT64_SECTOR, secs, g_blk)) {
            log_fail("file: source read");
            return -1;
        }
        if (!vol_write(cluster_lba(c) + done / FAT64_SECTOR, secs, g_blk)) { log_fail("file: data write"); return -1; }
        done += secs * FAT64_SECTOR;
    }
    if (!write_fats()) return -1;
    log_file(path8_3, len, c);
    return 0;
}

// ---------------- 离线自检 ----------------
// 目的：把"卷结构自洽"变成可执行的证据，而不是靠人读代码：
//   1) 坏参数被拒（卷太小 / 32MB 这种"装不下 65525 簇"的卷 / 未格式化就写文件）
//   2) 内存卷上做完整往返：format -> mkdir EFI -> mkdir EFI/BOOT -> 写 3000B 模式文件 -> 逐字节读回
//   3) 卷结构逐项核对（FAT32）：BPB（FATSz16=0/FATSz32/RootClus=2/FSInfo=1/BkBootSec=6/
//      类型串 "FAT32"）、FSInfo 三个签名 + free/next-free、备份引导扇区逐字节等于扇区 0、
//      根目录簇链（簇 2 且链尾 0x0FFFFFFF）、两份 FAT 逐字节一致、32 位目录项、
//      以及 **簇数 >= 65525 硬断言**
//   4) 自检只在内存卷上跑：真盘格式化必须走 fat64_format64（显式切后端）
int fat64_selftest64() {
    uint32_t mask = 0;
    // 1) 坏参数：
    //    * 512 扇区的卷 -> 必须 -1（在任何 I/O 之前就拒绝）
    //    * ★ 1024*1024 簇的经典陷阱：32MB（65536 扇区）也**不够** —— 解出的簇数 < 65525，
    //      这种卷按规范应被当作 FAT16，必须拒绝（镜像版的 FAT16/FAT12 陷阱）
    if (fat64_format64(0, 0, 512) == 0) mask |= 1;
    if (fat64_format64(0, 0, 65536) == 0) mask |= 1;
    if (fat64_format64(0, 0, FAT64_MIN_SECTORS - 1) == 0) mask |= 1;
    if (fat64_format64(-1, 0, FAT64_SELFTEST_SECTORS) == 0) mask |= 1;    // drive<0 也必须拒绝
    uint8_t one[FAT64_SECTOR];
    memzero8(one, sizeof(one));
    // 2) 未格式化（g_valid=false）就写文件 -> 必须 -1
    if (fat64_write_file64(0, "X.BIN", one, sizeof(one)) == 0) mask |= 2;
    if (fat64_mkdir64(0, "EFI") == 0) mask |= 4;

    // 3) 内存卷往返（34MB = 69632 扇区 -> 68512 簇 >= 65525，真 FAT32）
    //    ★ 关键：这里**显式切到内存后端**再调 format_volume —— 自检绝不碰真盘。
    //      （旧实现里 format 内部强制 g_ram=false，自检会把 drive 0 的真盘格式化掉；
    //        实测把安装介质 ISO 的前 2.5MB 覆盖成 FAT 卷，见 format_volume 的注释。）
    if (!ram_alloc(FAT64_SELFTEST_SECTORS)) {
        // 页池不够（内存极小的机器）：如实报 SKIP，不要假装 PASS，更不要退回去碰真盘
        dbg64_str("[FAT64] selftest SKIP (no free pages for 34MB ram volume)");
        dbg64_nl();
        return 0;
    }
    g_ram = true; g_ram_sectors = FAT64_SELFTEST_SECTORS; g_drive = -1;
    if (format_volume(0, FAT64_SELFTEST_SECTORS) != 0) {
        mask |= 8;
    } else {
        // ---- BPB：从卷里读回来核对（不是"我们刚写的那份内存"）----
        if (!vol_read(0, 1, g_sec)) mask |= 16;
        else {
            if (rd16(g_sec + 11) != FAT64_SECTOR) mask |= 16;                  // BytsPerSec
            if (g_sec[13] != FAT64_SPC) mask |= 16;                            // SecPerClus
            if (rd16(g_sec + 14) != FAT64_RESERVED) mask |= 16;                // RsvdSecCnt
            if (g_sec[16] != FAT64_NUM_FATS) mask |= 16;                       // NumFATs
            if (rd16(g_sec + 17) != 0) mask |= 16;                             // RootEntCnt = 0
            if (rd16(g_sec + 22) != 0) mask |= 16;                             // ★ FATSz16 = 0
            if (rd32(g_sec + 32) != FAT64_SELFTEST_SECTORS) mask |= 16;        // TotSec32
            if (rd32(g_sec + 36) != g_fatsz) mask |= 16;                       // ★ FATSz32
            if (rd32(g_sec + 44) != FAT64_ROOT_CLUSTER) mask |= 16;            // ★ RootClus = 2
            if (rd16(g_sec + 48) != FAT64_FSINFO_SEC) mask |= 16;              // ★ FSInfo = 1
            if (rd16(g_sec + 50) != FAT64_BKBOOT_SEC) mask |= 16;              // ★ BkBootSec = 6
            if (g_sec[64] != 0x80 || g_sec[66] != 0x29) mask |= 16;
            if (g_sec[510] != 0x55 || g_sec[511] != 0xAA) mask |= 16;
            if (g_sec[0] != 0xEB || g_sec[2] != 0x90) mask |= 16;
            for (int i = 0; i < 8 && !(mask & 16); i++) {                      // 类型串 "FAT32   "
                if (g_sec[82 + i] != (uint8_t)"FAT32   "[i]) mask |= 16;
            }
        }
        // ★ 硬断言：簇数 >= 65525（见 fat64.h 顶部说明）
        if (g_clusters < FAT64_CLUSTER_MIN) mask |= 262144;
        // ---- FSInfo（扇区 1）：三个签名 + free/next-free；备份 FSInfo（扇区 7）同内容 ----
        if (!vol_read(FAT64_FSINFO_SEC, 1, g_sec)) mask |= 524288;
        else {
            if (rd32(g_sec + 0) != 0x41615252u) mask |= 524288;
            if (rd32(g_sec + 484) != 0x61417272u) mask |= 524288;
            if (rd32(g_sec + 508) != 0xAA550000u) mask |= 524288;
            if (g_sec[510] != 0x55 || g_sec[511] != 0xAA) mask |= 524288;
            static uint8_t fsinfo[FAT64_SECTOR];
            memcopy8(fsinfo, g_sec, FAT64_SECTOR);
            if (rd32(g_sec + 488) != g_clusters - (g_next_cluster - FAT64_ROOT_CLUSTER)) mask |= 524288;
            if (rd32(g_sec + 492) != g_next_cluster) mask |= 524288;
            if (!vol_read(FAT64_FSINFO_SEC + FAT64_BKBOOT_SEC, 1, g_sec)) mask |= 524288;
            else {
                for (uint32_t i = 0; i < FAT64_SECTOR; i++) if (g_sec[i] != fsinfo[i]) mask |= 524288;
            }
        }
        // ---- 备份引导扇区（扇区 6）逐字节等于扇区 0 ----
        static uint8_t bs0[FAT64_SECTOR], bkb[FAT64_SECTOR];
        if (!vol_read(0, 1, bs0) || !vol_read(FAT64_BKBOOT_SEC, 1, bkb)) mask |= 1048576;
        else {
            for (uint32_t i = 0; i < FAT64_SECTOR; i++) if (bs0[i] != bkb[i]) mask |= 1048576;
        }
        // ---- FAT[0]/FAT[1]/根目录簇链 ----
        const uint32_t fat_off = FAT64_RESERVED * FAT64_SECTOR;
        if (ram_u32(fat_off) != 0x0FFFFFF8u) mask |= 4096;                 // FAT[0] = media
        if (ram_u32(fat_off + 4) != 0x0FFFFFFFu) mask |= 8192;             // FAT[1]
        if (ram_u32(fat_off + FAT64_ROOT_CLUSTER * 4) != FAT64_EOF) mask |= 2097152;  // 根簇链尾
        for (uint32_t k = 1; k < FAT64_NUM_FATS; k++) {
            const uint32_t o2 = fat_off + k * g_fatsz * FAT64_SECTOR;
            for (uint32_t i = 0; i < g_fatsz * FAT64_SECTOR; i++) {
                if (ram_peek(fat_off + i) != ram_peek(o2 + i)) { mask |= 16384; break; }
            }
        }
        if (fat64_mkdir64(0, "EFI") != 0) mask |= 32;
        if (fat64_mkdir64(0, "EFI/BOOT") != 0) mask |= 32;
        // 3000B 模式文件（跨 6 个簇 -> 必须走簇链）
        static uint8_t pat[3000];
        for (uint32_t i = 0; i < sizeof(pat); i++) pat[i] = (uint8_t)(0xA5 ^ (i * 7));
        if (fat64_write_file64(0, "EFI/BOOT/TEST.BIN", pat, sizeof(pat)) != 0) {
            mask |= 64;
        } else {
            // 找 EFI/BOOT 的簇号（从内存卷里解析，不复用内存里的目录表）
            const uint32_t root_off = (g_data_start + (FAT64_ROOT_CLUSTER - FAT64_ROOT_CLUSTER)
                                       * FAT64_SPC) * FAT64_SECTOR;
            uint32_t efi_c = 0;
            for (uint32_t i = 0; i < 16; i++) {
                const uint32_t o = root_off + i * 32;
                if (ram_peek(o) == 0) break;
                if (ram_peek(o) == 0xE5 || ram_peek(o + 11) == 0x0F) continue;
                if (ram_peek(o) == 'E' && ram_peek(o + 1) == 'F' && ram_peek(o + 2) == 'I'
                    && (ram_peek(o + 11) & 0x10)) {
                    // ★ 32 位目录项：低 16 位 @26 + 高 16 位 @20
                    efi_c = (ram_u16(o + 20) << 16) | ram_u16(o + 26);
                }
            }
            if (efi_c < 2) mask |= 128;
            else {
                const uint32_t efi_off = (g_data_start + (efi_c - FAT64_ROOT_CLUSTER) * FAT64_SPC)
                                         * FAT64_SECTOR;
                // "." / ".." 必须在；EFI 的父目录是根 -> ".." 写 0（FAT 惯例）
                if (!(ram_peek(efi_off + 0) == '.' && ram_peek(efi_off + 32) == '.'
                      && ram_peek(efi_off + 33) == '.')) mask |= 128;
                if (((ram_u16(efi_off + 32 + 20) << 16) | ram_u16(efi_off + 32 + 26)) != 0) mask |= 128;
                uint32_t boot_c = 0;
                for (int i = 2; i < 16; i++) {
                    const uint32_t o = efi_off + i * 32;
                    if (ram_peek(o) == 0) break;
                    if (ram_peek(o) == 'B' && ram_peek(o + 1) == 'O' && ram_peek(o + 2) == 'O'
                        && ram_peek(o + 3) == 'T' && (ram_peek(o + 11) & 0x10)) {
                        boot_c = (ram_u16(o + 20) << 16) | ram_u16(o + 26);
                    }
                }
                if (boot_c < 2) mask |= 256;
                else {
                    const uint32_t bo_off = (g_data_start + (boot_c - FAT64_ROOT_CLUSTER) * FAT64_SPC)
                                            * FAT64_SECTOR;
                    uint32_t f_c = 0, f_size = 0;
                    for (int i = 2; i < 16; i++) {
                        const uint32_t o = bo_off + i * 32;
                        if (ram_peek(o) == 0) break;
                        if (ram_peek(o) == 'T' && ram_peek(o + 1) == 'E' && ram_peek(o + 2) == 'S'
                            && ram_peek(o + 3) == 'T') {
                            f_c    = (ram_u16(o + 20) << 16) | ram_u16(o + 26);
                            f_size = ram_u32(o + 28);
                        }
                    }
                    if (f_c < 2 || f_size != sizeof(pat)) mask |= 512;
                    else {
                        // 沿**32 位**簇链读回，逐字节比对
                        uint32_t off = 0, c = f_c, guard = 0;
                        while (c >= FAT64_ROOT_CLUSTER && guard < 64) {
                            const uint32_t cl = (g_data_start + (c - FAT64_ROOT_CLUSTER) * FAT64_SPC)
                                                * FAT64_SECTOR;
                            for (uint32_t i = 0; i < FAT64_SPC * FAT64_SECTOR && off < sizeof(pat); i++) {
                                if (ram_peek(cl + i) != pat[off]) { mask |= 1024; break; }
                                off++;
                            }
                            const uint32_t nx = ram_u32(fat_off + c * 4);
                            if (nx >= 0x0FFFFFF8u) break;
                            c = nx; guard++;
                        }
                        if (off != sizeof(pat)) mask |= 2048;
                    }
                }
            }
        }
        if (fat64_mkdir64(0, "NOPE/SUB") == 0) mask |= 32768;         // 父目录不存在 -> 必须失败
        if (fat64_write_file64(0, "ZZZZZZZZZ.BIN", pat, 16) == 0) mask |= 65536;  // 主名 > 8 -> 必须失败
    }
    // 收尾：自检不留下"活着的假卷"，也确认**它真的只动了内存卷**（内存卷里有 BPB、g_drive 无效），
    //       并把 34MB 页还给页池（不然每次启动都漏 34MB）
    if (ram_u16(11) == 0) mask |= 131072;      // 内存卷没被格式化（BPB 空）——不该发生
    if (g_drive >= 0) mask |= 131072;          // 自检期间 g_drive 必须无效（没碰真盘）
    g_ram = false; g_drive = -1; g_valid = false;
    ram_release();

    dbg64_str("[FAT64] selftest ");
    if (mask == 0) { dbg64_str("PASS"); dbg64_str(" (FAT32, clusters="); dbg64_dec(FAT64_CLUSTER_MIN); dbg64_str("+)"); }
    else { dbg64_str("FAIL mask="); dbg64_hex64(mask); }
    dbg64_nl();
    return (int)mask;
}
