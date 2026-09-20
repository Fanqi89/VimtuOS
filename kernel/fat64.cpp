// fat64.cpp - 最小 FAT16 写入器（安装程序在目标盘上建 ESP 用）
//
// 与 tools/make_esp.py 的关系：卷格式算法**逐条对齐**（SPC=1、1 个保留扇区、2 份 FAT、
// 512 项固定根目录、BPB 字段与卷标一致）。差别只在实现方式：
//   * Python 版先把整卷放进内存再一次性写出（构建期，随便用内存）；
//   * 内核版按扇区流式写盘（安装期，数据要落真实磁盘，且内核内存有限）。
// 自检（fat64_selftest64）用同一套代码在一段内存卷上做完整往返，保证"卷结构自洽"是
// 被验证过的，而不是靠人读代码。
#include "fat64.h"
#include "ata64.h"
#include "debug64.h"

// ---------------- 卷状态（一次只操作一个卷） ----------------
struct Fat64Chain { uint32_t start, count; };
struct Fat64Dir   { char path[24]; uint32_t cluster; uint32_t next_slot; };

static bool     g_valid    = false;
static int      g_drive    = -1;
static uint32_t g_start    = 0;      // 卷在盘上的起始 LBA
static uint32_t g_fatsz    = 0;      // 每个 FAT 的扇区数
static uint32_t g_data_start = 0;    // 数据区起始（卷相对扇区号）
static uint32_t g_clusters = 0;      // 数据区簇数
static uint32_t g_next_cluster = 2;
static Fat64Chain g_chains[FAT64_MAX_CHAINS];
static int        g_nchains = 0;
static Fat64Dir   g_dirs[FAT64_MAX_DIRS];
static int        g_ndirs   = 0;

// 自检用：把卷建在内存里（不碰真盘）
static bool       g_ram      = false;
static uint8_t*   g_ram_mem  = nullptr;
static uint32_t   g_ram_sectors = 0;

// I/O 缓冲（.bss，不占镜像体积）
static const uint32_t FAT64_IO_SECTORS = 128;                       // 64KB 一块
static uint8_t g_blk[FAT64_IO_SECTORS * FAT64_SECTOR];
static uint8_t g_fat[FAT64_MAX_FAT_SECTORS * FAT64_SECTOR];
static uint8_t g_sec[FAT64_SECTOR];

static void memzero8(uint8_t* p, uint32_t n) { for (uint32_t i = 0; i < n; i++) p[i] = 0; }
static void memcopy8(uint8_t* d, const uint8_t* s, uint32_t n) { for (uint32_t i = 0; i < n; i++) d[i] = s[i]; }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

// ---------------- 卷 I/O（真盘 / 内存卷两种后端） ----------------
static bool vol_write(uint32_t vol_lba, uint32_t count, const uint8_t* data) {
    if (g_ram) {
        if (vol_lba + count > g_ram_sectors) return false;
        memcopy8(g_ram_mem + (uint64_t)vol_lba * FAT64_SECTOR, data, count * FAT64_SECTOR);
        return true;
    }
    if (g_drive < 0) return false;
    return ata64_write(g_drive, g_start + vol_lba, count, data);
}
static bool vol_read(uint32_t vol_lba, uint32_t count, uint8_t* data) {
    if (g_ram) {
        if (vol_lba + count > g_ram_sectors) return false;
        memcopy8(data, g_ram_mem + (uint64_t)vol_lba * FAT64_SECTOR, count * FAT64_SECTOR);
        return true;
    }
    if (g_drive < 0) return false;
    return ata64_read(g_drive, g_start + vol_lba, count, data);
}

// ---------------- 几何：与 make_esp.py 的 _fat16_geometry 同一算法 ----------------
static bool geometry(uint32_t total, uint32_t* out_fatsz, uint32_t* out_clusters, uint32_t* out_data) {
    uint32_t fsz = 1;
    for (;;) {
        const uint32_t data = total - FAT64_RESERVED - FAT64_NUM_FATS * fsz - FAT64_ROOT_SECTORS;
        const uint32_t cl   = data / FAT64_SPC;
        const uint32_t need = ((cl + 2) * 2 + FAT64_SECTOR - 1) / FAT64_SECTOR;
        if (need <= fsz) { *out_fatsz = fsz; *out_clusters = cl; *out_data = data; return true; }
        fsz = need;
        if (fsz > FAT64_MAX_FAT_SECTORS) return false;      // 卷大得不像 FAT16 了
    }
}

// ---------------- 簇链 / FAT 表 ----------------
static uint32_t cluster_lba(uint32_t c) { return g_data_start + (c - 2) * FAT64_SPC; }

static uint16_t fat_value(uint32_t c) {
    if (c == 0) return 0xFFF8;                              // 0xFF00 | media
    if (c == 1) return 0xFFFF;
    for (int i = 0; i < g_nchains; i++) {
        const uint32_t s = g_chains[i].start, n = g_chains[i].count;
        if (c >= s && c < s + n) return (c + 1 < s + n) ? (uint16_t)(c + 1) : 0xFFFF;
    }
    return 0;
}

static void log_fail(const char* what) {
    dbg64_str("[FAT64] FAIL ");
    dbg64_str(what);
    dbg64_nl();
}

// 把两份 FAT 表都写出去（每次分配后调用；表很小：5MB 卷只有 40 扇区/份）
static bool write_fats() {
    const uint32_t bytes = g_fatsz * FAT64_SECTOR;
    if (bytes > sizeof(g_fat)) { log_fail("fat-size"); return false; }
    memzero8(g_fat, bytes);
    const uint32_t n = g_clusters + 2;
    for (uint32_t c = 0; c < n; c++) {
        const uint16_t v = fat_value(c);
        g_fat[c * 2] = (uint8_t)(v & 0xFF);
        g_fat[c * 2 + 1] = (uint8_t)(v >> 8);
    }
    for (uint32_t k = 0; k < FAT64_NUM_FATS; k++) {
        if (!vol_write(FAT64_RESERVED + k * g_fatsz, g_fatsz, g_fat)) { log_fail("fat-write"); return false; }
    }
    return true;
}

static bool alloc_chain(uint32_t n, uint32_t* out_start) {
    if (n == 0) n = 1;
    if (g_next_cluster + n > g_clusters + 2) { log_fail("volume-full"); return false; }
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

// 目录项（32 字节；不含 LFN）
static void make_entry(uint8_t out[32], const uint8_t name11[11], uint8_t attr,
                       uint32_t cluster, uint32_t size) {
    memzero8(out, 32);
    memcopy8(out, name11, 11);
    out[11] = attr;
    wr16(out + 26, (uint16_t)(cluster & 0xFFFF));           // 起始簇低 16 位（FAT16 只用这个）
    wr32(out + 28, size);
}

// 在目录 dir_idx（0 = 根）的第 slot 项写一个目录项
static bool dir_write_entry(int dir_idx, uint32_t slot, const uint8_t name11[11], uint8_t attr,
                            uint32_t cluster, uint32_t size) {
    if (dir_idx < 0 || dir_idx >= g_ndirs) return false;
    uint8_t ent[32];
    make_entry(ent, name11, attr, cluster, size);
    uint32_t vol_lba;
    if (g_dirs[dir_idx].cluster == 0) {
        // 根目录：固定区（保留扇区 + 2 份 FAT 之后）
        vol_lba = FAT64_RESERVED + FAT64_NUM_FATS * g_fatsz + (slot * 32) / FAT64_SECTOR;
    } else {
        vol_lba = cluster_lba(g_dirs[dir_idx].cluster) + (slot * 32) / FAT64_SECTOR;
    }
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
    if (sectors < 4096) {                                    // 太小的卷不可能是 FAT16
        dbg64_str("[FAT64] format reject sectors=");
        dbg64_dec(sectors);
        dbg64_nl();
        return -1;
    }
    uint32_t fatsz = 0, clusters = 0, data = 0;
    if (!geometry(sectors, &fatsz, &clusters, &data)) { log_fail("geometry"); return -1; }
    (void)data;                                            // 数据区扇区数只用于推导，不需要另存
    // ★ 硬断言（与 make_esp.py 一致）：卷必须是真 FAT16，否则 EDK2 按 FAT12 解析本卷，
    //   多簇文件（UEFI64.BIN / KERNEL64.BIN）读取报 EFI_VOLUME_CORRUPTED。
    if (clusters < FAT64_CLUSTER_MIN || clusters >= FAT64_CLUSTER_MAX) {
        dbg64_str("[FAT64] format reject clusters=");
        dbg64_dec(clusters);
        dbg64_str(" (FAT16 区间 [");
        dbg64_dec(FAT64_CLUSTER_MIN);
        dbg64_str(",");
        dbg64_dec(FAT64_CLUSTER_MAX);
        dbg64_str("))");
        dbg64_nl();
        return -1;
    }

    g_start = start_lba;                    // 注意：**不动 g_ram/g_drive**（后端由调用方决定）
    g_fatsz = fatsz; g_clusters = clusters;
    g_data_start = FAT64_RESERVED + FAT64_NUM_FATS * fatsz + FAT64_ROOT_SECTORS;
    g_next_cluster = 2; g_nchains = 0; g_ndirs = 1;
    g_dirs[0].path[0] = 0; g_dirs[0].cluster = 0; g_dirs[0].next_slot = 0;

    // ---- 引导扇区（BPB；EFI 不执行它的代码，但字段必须自洽）----
    memzero8(g_sec, FAT64_SECTOR);
    g_sec[0] = 0xEB; g_sec[1] = 0x3C; g_sec[2] = 0x90;
    memcopy8(g_sec + 3, (const uint8_t*)"VIMTU64 ", 8);      // OEM
    wr16(g_sec + 11, (uint16_t)FAT64_SECTOR);                // 每扇区字节
    g_sec[13] = (uint8_t)FAT64_SPC;                          // 每簇扇区
    wr16(g_sec + 14, (uint16_t)FAT64_RESERVED);              // 保留扇区
    g_sec[16] = (uint8_t)FAT64_NUM_FATS;
    wr16(g_sec + 17, (uint16_t)FAT64_ROOT_ENTRIES);
    wr16(g_sec + 19, (uint16_t)(sectors < 0x10000 ? sectors : 0));
    g_sec[21] = FAT64_MEDIA;
    wr16(g_sec + 22, (uint16_t)fatsz);
    wr16(g_sec + 24, 32);                                    // 每道扇区（CHS 无意义，填常用值）
    wr16(g_sec + 26, 64);                                    // 磁头
    wr32(g_sec + 28, 0);                                     // 隐藏扇区
    wr32(g_sec + 32, sectors >= 0x10000 ? sectors : 0);      // 总扇区（32 位）
    g_sec[36] = 0x80;                                        // 驱动器号
    g_sec[38] = 0x29;                                        // 扩展引导签名
    wr32(g_sec + 39, 0x56494D54);                            // 卷序号 'VIMT'
    memcopy8(g_sec + 43, (const uint8_t*)"VIMTU64ESP ", 11); // 卷标
    memcopy8(g_sec + 54, (const uint8_t*)"FAT16   ", 8);     // 文件系统类型
    g_sec[510] = 0x55; g_sec[511] = 0xAA;
    if (!vol_write(0, 1, g_sec)) { log_fail("boot-sector"); return -1; }

    if (!write_fats()) return -1;

    // ---- 根目录清零（512 项 * 32B = 32 扇区）----
    memzero8(g_sec, FAT64_SECTOR);
    for (uint32_t i = 0; i < FAT64_ROOT_SECTORS; i++) {
        if (!vol_write(FAT64_RESERVED + FAT64_NUM_FATS * g_fatsz + i, 1, g_sec)) {
            log_fail("root-dir");
            return -1;
        }
    }

    g_valid = true;
    dbg64_str("[FAT64] format lba=");
    dbg64_dec(start_lba);
    dbg64_str(" sectors=");
    dbg64_dec(sectors);
    dbg64_str(" clusters=");
    dbg64_dec(clusters);
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
    g_ram = false; g_ram_mem = nullptr; g_ram_sectors = 0;
    g_drive = drive;
    return format_volume(start_lba, sectors);
}


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
    g_dirs[pi].next_slot++;

    // 新目录的第一、二项固定是 "." 和 ".."（FAT 规范的硬要求：固件/Shell 靠它们解析路径）
    uint8_t buf[FAT64_SECTOR];
    memzero8(buf, sizeof(buf));
    uint8_t dot[11], dotdot[11];
    for (int i = 0; i < 11; i++) { dot[i] = ' '; dotdot[i] = ' '; }
    dot[0] = '.'; dotdot[0] = '.'; dotdot[1] = '.';
    make_entry(buf, dot, 0x10, c, 0);
    make_entry(buf + 32, dotdot, 0x10, g_dirs[pi].cluster, 0);
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
//   1) 坏参数被拒（卷太小 / 未格式化就写文件）
//   2) 内存卷上做完整往返：format -> mkdir EFI -> mkdir EFI/BOOT -> 写 3000B 模式文件 -> 逐字节读回
//   3) 卷结构逐项核对：BPB 字段、FAT[0]/FAT[1]、根目录 EFI 项、子目录 "."/".."、簇链以 0xFFFF 结尾
int fat64_selftest64() {
    uint32_t mask = 0;
    // 1) 坏参数：512 扇区的卷不可能是 FAT16 -> 必须 -1（在任何 I/O 之前就拒绝）
    if (fat64_format64(0, 0, 512) == 0) mask |= 1;
    if (fat64_format64(-1, 0, 5120) == 0) mask |= 1;          // drive<0 也必须拒绝
    uint8_t one[FAT64_SECTOR];
    memzero8(one, sizeof(one));
    // 2) 未格式化（g_valid=false）就写文件 -> 必须 -1
    if (fat64_write_file64(0, "X.BIN", one, sizeof(one)) == 0) mask |= 2;
    if (fat64_mkdir64(0, "EFI") == 0) mask |= 4;

    // 3) 内存卷往返（5120 扇区 = 2.5MB -> 簇数落在 FAT16 区间）
    //    ★ 关键：这里**显式切到内存后端**再调 format_volume —— 自检绝不碰真盘。
    //      （旧实现里 format 内部强制 g_ram=false，自检会把 drive 0 的真盘格式化掉；
    //        实测把安装介质 ISO 的前 2.5MB 覆盖成 FAT16 卷，见 format_volume 的注释。）
    static uint8_t ram_vol[5120 * FAT64_SECTOR];
    memzero8(ram_vol, sizeof(ram_vol));
    g_ram = true; g_ram_mem = ram_vol; g_ram_sectors = 5120; g_drive = -1;
    if (format_volume(0, 5120) != 0) {
        mask |= 8;
    } else {
        // BPB 从卷里读回来核对（不是"我们刚写的那份内存"）
        if (!vol_read(0, 1, g_sec)) mask |= 16;
        else {
            if (rd16(g_sec + 11) != FAT64_SECTOR) mask |= 16;
            if (g_sec[13] != FAT64_SPC) mask |= 16;
            if (rd16(g_sec + 14) != FAT64_RESERVED) mask |= 16;
            if (g_sec[16] != FAT64_NUM_FATS) mask |= 16;
            if (rd16(g_sec + 17) != FAT64_ROOT_ENTRIES) mask |= 16;
            if (rd16(g_sec + 22) != g_fatsz) mask |= 16;
            if (g_sec[510] != 0x55 || g_sec[511] != 0xAA) mask |= 16;
            if (g_sec[0] != 0xEB || g_sec[2] != 0x90) mask |= 16;
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
            const uint32_t root_off = (FAT64_RESERVED + FAT64_NUM_FATS * g_fatsz) * FAT64_SECTOR;
            uint32_t efi_c = 0;
            for (uint32_t i = 0; i < FAT64_ROOT_ENTRIES; i++) {
                const uint8_t* e = ram_vol + root_off + i * 32;
                if (e[0] == 0) break;
                if (e[0] == 0xE5 || e[11] == 0x0F) continue;
                if (e[0] == 'E' && e[1] == 'F' && e[2] == 'I' && (e[11] & 0x10)) efi_c = rd16(e + 26);
            }
            if (efi_c < 2) mask |= 128;
            else {
                const uint32_t efi_off = (g_data_start + (efi_c - 2) * FAT64_SPC) * FAT64_SECTOR;
                // "." / ".." 必须在
                if (!(ram_vol[efi_off + 0] == '.' && ram_vol[efi_off + 32] == '.' && ram_vol[efi_off + 33] == '.'))
                    mask |= 128;
                uint32_t boot_c = 0;
                for (int i = 2; i < 16; i++) {
                    const uint8_t* e = ram_vol + efi_off + i * 32;
                    if (e[0] == 0) break;
                    if (e[0] == 'B' && e[1] == 'O' && e[2] == 'O' && e[3] == 'T' && (e[11] & 0x10)) boot_c = rd16(e + 26);
                }
                if (boot_c < 2) mask |= 256;
                else {
                    const uint32_t bo_off = (g_data_start + (boot_c - 2) * FAT64_SPC) * FAT64_SECTOR;
                    uint32_t f_c = 0, f_size = 0;
                    for (int i = 2; i < 16; i++) {
                        const uint8_t* e = ram_vol + bo_off + i * 32;
                        if (e[0] == 0) break;
                        if (e[0] == 'T' && e[1] == 'E' && e[2] == 'S' && e[3] == 'T') {
                            f_c = rd16(e + 26);
                            f_size = (uint32_t)e[28] | ((uint32_t)e[29] << 8) | ((uint32_t)e[30] << 16) | ((uint32_t)e[31] << 24);
                        }
                    }
                    if (f_c < 2 || f_size != sizeof(pat)) mask |= 512;
                    else {
                        // 沿簇链读回，逐字节比对
                        uint32_t off = 0, c = f_c, guard = 0;
                        const uint32_t fat_off = FAT64_RESERVED * FAT64_SECTOR;
                        while (c >= 2 && guard < 64) {
                            const uint32_t cl = (g_data_start + (c - 2) * FAT64_SPC) * FAT64_SECTOR;
                            for (uint32_t i = 0; i < FAT64_SPC * FAT64_SECTOR && off < sizeof(pat); i++) {
                                if (ram_vol[cl + i] != pat[off]) { mask |= 1024; break; }
                                off++;
                            }
                            const uint16_t nx = rd16(ram_vol + fat_off + c * 2);
                            if (nx >= 0xFFF8) break;
                            c = nx; guard++;
                        }
                        if (off != sizeof(pat)) mask |= 2048;
                    }
                }
            }
            // FAT[0]/FAT[1] 保留项
            const uint32_t fat_off = FAT64_RESERVED * FAT64_SECTOR;
            if (rd16(ram_vol + fat_off) != 0xFFF8) mask |= 4096;
            if (rd16(ram_vol + fat_off + 2) != 0xFFFF) mask |= 8192;
            // 两份 FAT 必须一致
            if (rd16(ram_vol + fat_off) != rd16(ram_vol + fat_off + g_fatsz * FAT64_SECTOR)) mask |= 16384;
        }
        if (fat64_mkdir64(0, "NOPE/SUB") == 0) mask |= 32768;         // 父目录不存在 -> 必须失败
        if (fat64_write_file64(0, "ZZZZZZZZZ.BIN", pat, 16) == 0) mask |= 65536;  // 主名 > 8 -> 必须失败
    }
    // 收尾：自检不留下"活着的假卷"，也必须确认**它真的只动了内存**（ram_vol 里有卷、g_drive 无效）
    if (rd16(ram_vol + 11) != 0) { /* 内存卷已被格式化（BPB 有内容）——预期 */ }
    else mask |= 131072;
    g_ram = false; g_ram_mem = nullptr; g_ram_sectors = 0; g_valid = false; g_drive = -1;

    dbg64_str("[FAT64] selftest ");
    if (mask == 0) { dbg64_str("PASS"); }
    else { dbg64_str("FAIL mask="); dbg64_hex64(mask); }
    dbg64_nl();
    return (int)mask;
}
