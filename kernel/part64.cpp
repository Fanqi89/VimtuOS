// part64.cpp - 分区表写盘 + 安装引擎（真实扇区读写，不是模拟）
#include "part64.h"
#include "ata64.h"
#include "vfs64.h"      // 格式化：主数据分区写真正的 VimtuFS2（不再是占位超级块）
#include "debug64.h"

// 每块复制多少扇区（64KB 缓冲）。太小 -> 进度跳得碎；太大 -> 单帧耗时明显。
static const uint32_t INSTALL_CHUNK_SECTORS = 128;

// 静态缓冲：64KB（.bss，不占镜像体积）
static uint8_t g_sec[512];
static uint8_t g_io[INSTALL_CHUNK_SECTORS * 512];

// ---------------- 小工具 ----------------
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static void memzero(uint8_t* p, uint32_t n) { for (uint32_t i = 0; i < n; i++) p[i] = 0; }

// ---------------- MBR 读取/解析 ----------------
bool part_read_mbr(int drive, uint8_t* sec512) {
    return ata64_read_sector(drive, 0, sec512);
}

void part_parse_mbr(const uint8_t* sec512, PartInfo out[4]) {
    for (int i = 0; i < 4; i++) {
        const uint8_t* e = sec512 + 446 + i * 16;
        out[i].used     = (e[4] != 0);
        out[i].type     = e[4];
        out[i].bootable = (e[0] == 0x80);
        out[i].start    = rd32(e + 8);
        out[i].sectors  = rd32(e + 12);
    }
}

uint32_t part_unallocated_sectors(const uint8_t* sec512, uint32_t disk_sectors) {
    PartInfo p[4];
    part_parse_mbr(sec512, p);
    uint32_t used = 0;
    for (int i = 0; i < 4; i++) {
        if (!p[i].used) continue;
        uint32_t end = p[i].start + p[i].sectors;
        if (end > used) used = end;
    }
    // 前 9 个扇区（MBR + loader 区）视为已占用
    if (used < PART_BOOT_LBA) used = PART_BOOT_LBA;
    return (disk_sectors > used) ? (disk_sectors - used) : 0;
}

// ---------------- 分区表写入 ----------------
// 在 512 字节扇区里设置第 idx(0..3) 个分区项
static void set_entry(uint8_t* sec512, int idx, uint8_t type, bool bootable,
                      uint32_t start, uint32_t sectors) {
    uint8_t* e = sec512 + 446 + idx * 16;
    memzero(e, 16);
    e[0] = bootable ? 0x80 : 0x00;
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;        // 起始 CHS（LBA 模式下无意义，填常用值）
    e[4] = type;
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;        // 结束 CHS
    wr32(e + 8, start);
    wr32(e + 12, sectors);
}

static void clear_entry(uint8_t* sec512, int idx) {
    memzero(sec512 + 446 + idx * 16, 16);
}

static void set_signature(uint8_t* sec512) {
    sec512[510] = 0x55;
    sec512[511] = 0xAA;
}

// 标准布局：引导分区（0xEF，活动）+ 主分区（0x07）
static void build_standard_table(uint8_t* sec512, uint32_t disk_sectors) {
    for (int i = 0; i < 4; i++) clear_entry(sec512, i);
    set_entry(sec512, 0, 0xEF, true, PART_BOOT_LBA, PART_BOOT_SECS);
    const uint32_t main_secs = (disk_sectors > PART_MAIN_LBA) ? (disk_sectors - PART_MAIN_LBA) : 0;
    if (main_secs > 0) {
        set_entry(sec512, 1, 0x07, false, PART_MAIN_LBA, main_secs);
    }
    set_signature(sec512);
}

bool part_create_standard(int drive, uint32_t disk_sectors) {
    if (disk_sectors < PART_MAIN_LBA + PART_MAIN_MIN_SECS) {
        dbg64_str("[PART] 磁盘太小，无法创建标准布局 sectors=");
        dbg64_dec(disk_sectors);
        dbg64_nl();
        return false;
    }
        if (!ata64_read_sector(drive, 0, g_sec)) {
            dbg64_str("[PART] 读目标盘 LBA0 失败 drive=");
            dbg64_dec(drive);
            dbg64_str(" reason=");
            dbg64_dec(ata64_dbg_reason);
            dbg64_str(" err=0x");
            dbg64_hex64(ata64_dbg_err);
            dbg64_str(" polls=");
            dbg64_dec(ata64_dbg_polls);
            dbg64_nl();
            return false;
        }

    // 已有分区就拒绝（避免覆盖别人的分区表；界面上会提示先删除）
    PartInfo p[4];
    part_parse_mbr(g_sec, p);
    int existing = 0;
    for (int i = 0; i < 4; i++) if (p[i].used) existing++;
    if (existing > 0) {
        dbg64_str("[PART] 该磁盘已有分区，新建被拒绝 existing=");
        dbg64_dec(existing);
        dbg64_nl();
        return false;
    }

    build_standard_table(g_sec, disk_sectors);
    if (!ata64_write_sector(drive, 0, g_sec)) return false;

    dbg64_str("[PART] 新建分区表 OK drive=");
    dbg64_dec(drive);
    dbg64_str(" boot=");
    dbg64_dec(PART_BOOT_LBA);
    dbg64_str("+");
    dbg64_dec(PART_BOOT_SECS);
    dbg64_str(" main=");
    dbg64_dec(PART_MAIN_LBA);
    dbg64_str("+");
    dbg64_dec(disk_sectors - PART_MAIN_LBA);
    dbg64_nl();
    return true;
}

bool part_delete_entry(int drive, int index) {
    if (index < 1 || index > 4) return false;
    if (!ata64_read_sector(drive, 0, g_sec)) return false;
    clear_entry(g_sec, index - 1);
    set_signature(g_sec);
    if (!ata64_write_sector(drive, 0, g_sec)) return false;
    dbg64_str("[PART] 删除分区 OK drive=");
    dbg64_dec(drive);
    dbg64_str(" index=");
    dbg64_dec(index);
    dbg64_nl();
    return true;
}

bool part_format_partition(int drive, const PartInfo& p, int index) {
    if (!p.used || p.sectors == 0) return false;
    // 清零分区首部 64 个扇区（够抹掉旧的引导记录/超级块；不整盘擦除，避免等待过久）
    uint32_t n = (p.sectors < 64) ? p.sectors : 64;
    memzero(g_io, sizeof(g_io));
    for (uint32_t i = 0; i < n; i += INSTALL_CHUNK_SECTORS) {
        uint32_t c = n - i;
        if (c > INSTALL_CHUNK_SECTORS) c = INSTALL_CHUNK_SECTORS;
        if (!ata64_write(drive, p.start + i, c, g_io)) return false;
    }
    // 主数据分区（0x07）用真文件系统 VimtuFS2：块 0 = 超级块 + 位图 + inode 区 + 数据区
    if (p.type == 0x07) {
        if (vfs64_format(drive, p.start, p.sectors) != 0) {
            // 打印失败原因（vfs64_format 自己会打 [VFS64] format FAILED reason=...）
            return false;
        }
    }
    dbg64_str("[PART] 格式化 OK drive=");
    dbg64_dec(drive);
    dbg64_str(" index=");
    dbg64_dec(index);
    dbg64_str(" start=");
    dbg64_dec(p.start);
    dbg64_str(" sectors=");
    dbg64_dec(p.sectors);
    dbg64_nl();
    return true;
}

// ---------------- 安装：整盘复制 ----------------
int part_find_payload_drive(uint32_t payload_lba, uint32_t* out_sectors) {
    for (int d = 0; d < 4; d++) {
        DiskInfo di;
        if (!ata64_identify(d, &di) || !di.present || di.atapi) continue;
        if (!ata64_read_sector(d, payload_lba, g_sec)) continue;
        const PayloadHeader* h = (const PayloadHeader*)g_sec;
        if (h->magic[0] == 'V' && h->magic[1] == 'I' && h->magic[2] == 'M' &&
            h->magic[3] == 'T' && h->magic[4] == 'U' && h->magic[5] == 'P' &&
            h->magic[6] == 'A' && h->magic[7] == 'Y') {
            if (out_sectors) *out_sectors = h->sectors;
            return d;
        }
    }
    return -1;
}

// 找出"安装介质本身所在的那块盘"：ISO9660 主卷描述符（PVD）在 **2048 字节** 的 LBA 16 处，
// 偏移 +1 起是 "CD001"。介质在 U 盘/hybrid 场景下是**块设备**（512 字节扇区），
// 所以换算成 512B 扇区要读 LBA 64 —— 踩过这个坑：按 16 去读读到的是别的内容，
// 于是"介质盘"认不出来，向导默认行落在介质盘上，把 ISO 自己给装了。
static const uint32_t ISO_PVD_LBA512 = 16u * 4u;      // 16 个 2048B 扇区 = 64 个 512B 扇区

int part_find_iso_medium_drive(void) {
    for (int d = 0; d < 4; d++) {
        DiskInfo di;
        if (!ata64_identify(d, &di) || !di.present || di.atapi) continue;
        if (di.sectors < ISO_PVD_LBA512 + 1) continue;
        if (!ata64_read_sector(d, ISO_PVD_LBA512, g_sec)) continue;
        if (g_sec[0] == 1 && g_sec[1] == 'C' && g_sec[2] == 'D' &&
            g_sec[3] == '0' && g_sec[4] == '0' && g_sec[5] == '1') return d;
    }
    return -1;
}

// ==================== 介质描述符（ISO 安装介质）====================
// 引导桩 boot/cdiso.asm 把"内核/载荷在哪"写进物理地址 0x0F00；内核启动后那块低内存
// 不会被覆盖（内核镜像在 0x100000 以上），所以这里直接按指针读。
bool part_read_medium_desc(MediumDesc* out) {
    const MediumDesc* md = (const MediumDesc*)(uintptr_t)MEDIUM_DESC_ADDR;
    if (md->magic != MEDIUM_MAGIC) return false;
    if (md->kind != MEDIUM_CD && md->kind != MEDIUM_RAM) return false;
    *out = *md;
    return true;
}

// 用介质描述符初始化安装任务（光盘 / RAM 两种源）
bool part_install_begin_medium(InstallJob* job, const MediumDesc& md, int dst_drive,
                               uint32_t dst_sectors) {
    if (dst_drive < 0 || dst_sectors < PART_MAIN_LBA + PART_MAIN_MIN_SECS) return false;
    if (md.payload_secs == 0) return false;

    job->active          = true;
    job->done            = false;
    job->failed          = false;
    job->src_drive       = -1;
    job->dst_drive       = dst_drive;
    job->payload_lba     = 0;
    job->payload_sectors = md.payload_secs;
    job->copied          = 0;
    job->write_table_at  = dst_sectors;
    job->src_cd          = md.drive;
    if (md.kind == MEDIUM_CD) {
        job->src_kind = INSTALL_SRC_CD;
        job->src_lba  = md.payload_lba;        // 光盘扇区号（2048B）
    } else {
        job->src_kind = INSTALL_SRC_RAM;
        job->src_lba  = md.payload_lba;        // 物理地址
    }
    dbg64_str("[INSTALL] 介质源 kind=");
    dbg64_dec(md.kind);
    dbg64_str(" payload=");
    dbg64_dec((uint64_t)md.payload_lba);
    dbg64_str(" sectors=");
    dbg64_dec(md.payload_secs);
    dbg64_nl();
    return true;
}

bool part_install_begin(InstallJob* job, uint32_t payload_lba, int dst_drive, uint32_t dst_sectors) {
    memzero((uint8_t*)job, sizeof(*job));
    uint32_t payload_sectors = 0;
    const int src = part_find_payload_drive(payload_lba, &payload_sectors);
    if (src < 0 || payload_sectors == 0) {
        dbg64_str("[INSTALL] 找不到安装介质载荷（magic VIMTUPAY）");
        dbg64_nl();
        return false;
    }
    if (src == dst_drive) {
        dbg64_str("[INSTALL] 目标盘就是安装介质所在的盘，拒绝");
        dbg64_nl();
        return false;
    }
    job->active          = true;
    job->src_drive       = src;
    job->dst_drive       = dst_drive;
    job->payload_lba     = payload_lba + 1;       // 载荷头之后的第一个扇区
    job->payload_sectors = payload_sectors;
    job->copied          = 0;
    job->write_table_at  = dst_sectors;
    dbg64_str("[INSTALL] 开始安装：src=");
    dbg64_dec(src);
    dbg64_str(" dst=");
    dbg64_dec(dst_drive);
    dbg64_str(" sectors=");
    dbg64_dec(payload_sectors);
    dbg64_nl();
    return true;
}

int part_install_step(InstallJob* job) {
    if (!job->active || job->done || job->failed) return job->failed ? -1 : 1;

    uint32_t remain = job->payload_sectors - job->copied;
    if (remain == 0) {
        // 复制完成：按目标盘实际大小重写分区表（引导分区 + 主分区）
        if (!ata64_read_sector(job->dst_drive, 0, g_sec)) { job->failed = true; return -1; }
        build_standard_table(g_sec, job->write_table_at);
        if (!ata64_write_sector(job->dst_drive, 0, g_sec)) { job->failed = true; return -1; }
        job->active = false;
        job->done   = true;
        dbg64_str("[INSTALL] 完成：已写 ");
        dbg64_dec(job->copied);
        dbg64_str(" 扇区，分区表已更新");
        dbg64_nl();
        return 1;
    }

    uint32_t chunk = (remain < INSTALL_CHUNK_SECTORS) ? remain : INSTALL_CHUNK_SECTORS;
    // ---- 按源种类读同一块数据到 g_io ----
    if (job->src_kind == INSTALL_SRC_CD) {
        // 光盘：2048B/扇区，读一次覆盖两块（16 个光盘扇区 = 32KB = 64 个硬盘扇区）
        // 为简单起见每次只写 32 个硬盘扇区（16KB），每 64 扇区重新读一次光盘
        static uint8_t cd_buf[16 * 2048];
        const uint32_t off_in_block = job->copied % 64;              // 硬盘扇区偏移
        if (off_in_block == 0) {
            if (!ata64_atapi_read(job->src_cd, job->src_lba + job->copied / 4, 16, cd_buf)) {
                dbg64_str("[INSTALL] 读光盘失败 @");
                dbg64_dec(job->copied);
                dbg64_nl();
                job->failed = true;
                return -1;
            }
        }
        uint32_t cd_chunk = 64 - off_in_block;                       // 本块还剩几个硬盘扇区
        if (cd_chunk > chunk) cd_chunk = chunk;
        for (uint32_t i = 0; i < cd_chunk * 512; i++)
            g_io[i] = cd_buf[off_in_block * 512 + i];
        chunk = cd_chunk;
    } else if (job->src_kind == INSTALL_SRC_RAM) {
        // 引导桩已经把载荷读进内存（U 盘/硬盘引导的 ISO）：直接拷
        const uint8_t* src = (const uint8_t*)(uintptr_t)(job->src_lba + job->copied * 512);
        for (uint32_t i = 0; i < chunk * 512; i++) g_io[i] = src[i];
    } else {
        if (!ata64_read(job->src_drive, job->payload_lba + job->copied, chunk, g_io)) {
            dbg64_str("[INSTALL] 读载荷失败 @");
            dbg64_dec(job->copied);
            dbg64_nl();
            job->failed = true;
            return -1;
        }
    }
    if (!ata64_write(job->dst_drive, job->copied, chunk, g_io)) {
        dbg64_str("[INSTALL] 写目标盘失败 @");
        dbg64_dec(job->copied);
        dbg64_nl();
        job->failed = true;
        return -1;
    }
    job->copied += chunk;
    return 0;
}
