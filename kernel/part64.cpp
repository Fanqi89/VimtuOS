// part64.cpp - 分区表写盘 + 安装引擎（真实扇区读写，不是模拟）
#include "part64.h"
#include "ata64.h"
#include "vfs64.h"      // 格式化：主数据分区写真正的 VimtuFS2（不再是占位超级块）
#include "fat64.h"      // ESP：安装完成时在目标盘写 FAT32 卷（EFI/BOOT/BOOTX64.EFI 等）
#include "memlayout64.h" // ML64_KERNEL_LBA / ML64_KERNEL_SECTORS：KERNEL64.BIN 的来源区
#include "debug64.h"

// ==================== 内嵌的 UEFI 引导字节（写进目标盘 ESP 用）====================
// 来源：build64.sh 里 build_uefi.sh 的产物，再由 objcopy 变成 elf64 目标文件链进本内核。
// 符号名由 objcopy 按输入路径生成（从仓库根执行才稳定），与 kernel64.cpp 的用法一致。
// ★ 只嵌进**安装程序内核**（SRCS_INSTALLER）：系统内核不需要写 ESP，省它的体积预算。
extern "C" const uint8_t _binary_build64_BOOTX64_EFI_start[];
extern "C" const uint8_t _binary_build64_BOOTX64_EFI_end[];
extern "C" const uint8_t _binary_build64_UEFI64_BIN_start[];
extern "C" const uint8_t _binary_build64_UEFI64_BIN_end[];
static uint32_t embedded_stub_len64() {
    return (uint32_t)(_binary_build64_BOOTX64_EFI_end - _binary_build64_BOOTX64_EFI_start);
}
static uint32_t embedded_loader_len64() {
    return (uint32_t)(_binary_build64_UEFI64_BIN_end - _binary_build64_UEFI64_BIN_start);
}

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

// ==================== ESP 几何（盘尾、GPT 备份表之前）====================
// 目标盘最小尺寸 = 主分区起点 8009 + 主分区至少 8MB（16384）+ ESP 48MB（98304）+ 备份 GPT 33
//                 = 122730 扇区 ≈ 59.9MiB（ESP 必须放得下**真 FAT32**：簇数 >= 65525）。
// 不满足（例如 16MB 的回归目标盘）就**不建 ESP**，只写老的 MBR 布局 ——
// 那种盘本来也放不下 ESP 里的文件（4MB 内核块 + 引导器），而且 FAT32 的卷下限就 ~33.5MB。
bool part_esp_geometry64(uint32_t disk_sectors, uint32_t* out_start, uint32_t* out_sectors,
                         uint32_t* out_main_sectors) {
    const uint32_t need = PART_MAIN_LBA + PART_ESP_MIN_MAIN_SECS + PART_ESP_SECTORS + PART_GPT_BACKUP_SECTORS;
    if (disk_sectors < need) return false;
    const uint32_t last_usable = disk_sectors - PART_GPT_BACKUP_SECTORS;      // 备份项数组之前的第一个 LBA
    const uint32_t start       = last_usable - PART_ESP_SECTORS;
    if (out_start)        *out_start = start;
    if (out_sectors)      *out_sectors = PART_ESP_SECTORS;
    if (out_main_sectors) *out_main_sectors = start - PART_MAIN_LBA;          // 主分区：8009 .. esp-1
    return true;
}

// ==================== 混合 MBR ====================
// 老布局（无 ESP）：P1 引导分区（0xEF，活动，9+8000）+ P2 主分区（0x07，8009..盘尾）
// 新布局（有 ESP）：上面两项 + P3 EFI 系统分区（0xEF，盘尾 ESP）；P2 收缩到 ESP 之前
// 说明：这里**不写 0xEE 保护项** —— EDK2 的 PartitionValidMbr 见到 0xEE 会直接放弃
//   MBR 解析路径（EFI_PROTECTED_PARTITION），而我们要同时保留"MBR 里找 0xEF 分区"
//   这条老固件的兜底路径。GPT（盘尾备份头）本身就是权威的分区描述。
static void build_standard_table(uint8_t* sec512, uint32_t disk_sectors) {
    for (int i = 0; i < 4; i++) clear_entry(sec512, i);
    set_entry(sec512, 0, 0xEF, true, PART_BOOT_LBA, PART_BOOT_SECS);
    uint32_t esp_start = 0, esp_sectors = 0, main_secs = 0;
    const bool has_esp = part_esp_geometry64(disk_sectors, &esp_start, &esp_sectors, &main_secs);
    if (!has_esp) {
        main_secs = (disk_sectors > PART_MAIN_LBA) ? (disk_sectors - PART_MAIN_LBA) : 0;
    }
    if (main_secs > 0) {
        set_entry(sec512, 1, 0x07, false, PART_MAIN_LBA, main_secs);
    }
    if (has_esp) {
        set_entry(sec512, 2, 0xEF, false, esp_start, esp_sectors);   // MBR-only 固件的兜底
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
    // 保护性 0xEE 项不是一个真分区（它是 GPT 盘在 MBR 里的占位），不算"已有分区" ——
    // 否则把一块原来装过 Windows 的盘删空之后仍然新建不了分区表。
    int existing = 0;
    for (int i = 0; i < 4; i++) if (p[i].used && p[i].type != 0xEE) existing++;
    if (existing > 0) {
        dbg64_str("[PART] 该磁盘已有分区，新建被拒绝 existing=");
        dbg64_dec(existing);
        dbg64_nl();
        return false;
    }

    // ★ v0.5.14：新建 = 重新初始化分区表 -> 旧 GPT（主/备头）一并抹掉。
    //   不抹的话"删掉分区再新建"会留下盘尾 GPT 仍然描述着旧分区，UEFI 起来照样按它
    //   找 ESP（表现：分区删了、盘好像还认得）。
    if (part_gpt_wipe64(drive, disk_sectors)) {
        if (!ata64_read_sector(drive, 0, g_sec)) return false;   // g_sec 刚才被 wipe 用掉了
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
    {
        uint32_t esp_start = 0, esp_sectors = 0, esp_main = 0;
        if (part_esp_geometry64(disk_sectors, &esp_start, &esp_sectors, &esp_main)) {
            dbg64_dec(esp_main);
            dbg64_str(" esp=");
            dbg64_dec(esp_start);
            dbg64_str("+");
            dbg64_dec(esp_sectors);
        } else {
            dbg64_dec(disk_sectors - PART_MAIN_LBA);
        }
    }
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
    // ★ 保护 ESP：EFI 系统分区（盘尾那个 0xEF 项）里是 FAT32 引导卷，
    //   抹掉它 UEFI 就再也起不来该盘。P1（LBA 9 起的"引导分区"，也是 0xEF 类型）
    //   保持可格式化 —— 老行为/既有测试都依赖它。
    if (p.type == 0xEF && p.start > PART_BOOT_LBA + PART_BOOT_SECS - 1) {
        dbg64_str("[PART] 拒绝格式化 EFI 系统分区（UEFI 引导卷）start=");
        dbg64_dec(p.start);
        dbg64_nl();
        return false;
    }
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

// ==================== GPT（盘尾备份头 + 项数组）+ ESP 安装 ====================
// 为什么只有备份 GPT：LBA 1 是 loader64.bin（boot.bin 从 LBA 1 读 8 扇区），
//   主 GPT 头按规范必须在 LBA 1 —— 两者不可兼得。UEFI 规范要求固件在"主头无效"时
//   使用备份头，EDK2（OVMF / VMware EFI）实测确实如此（tests/esp_install_test.py 里
//   OVMF 从这样一块盘启动成功，并且启动后 LBA 1..8 的 loader 字节没被改写）。
//   MBR 里同时留一项 0xEF 指向 ESP 作为兜底（UEFI 规范允许 MBR 上的 ESP）。
static const uint8_t GPT_TYPE_BASIC[16] = {                     // EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
    0xA2,0xA0,0xD0,0xEB, 0xE5,0xB9, 0x33,0x44, 0x87,0xC0, 0x68,0xB6,0xB7,0x26,0x99,0xC7
};
static const uint8_t GPT_TYPE_ESP[16]   = {                     // C12A7328-F81F-11D2-BA4B-00A0C93EC93B
    0x28,0x73,0x2A,0xC1, 0x1F,0xF8, 0xD2,0x11, 0xBA,0x4B, 0x00,0xA0,0xC9,0x3E,0xC9,0x3B
};
static const uint8_t GPT_DISK_GUID[16]  = {                     // 56494D54-4F53-0001-0000-000000000001 ('VIMT'...)
    0x54,0x4D,0x49,0x56, 0x53,0x4F, 0x01,0x00, 0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x01
};
static const uint8_t GPT_PART1_GUID[16] = {
    0x54,0x4D,0x49,0x56, 0x53,0x4F, 0x02,0x00, 0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x02
};
static const uint8_t GPT_PART2_GUID[16] = {
    0x54,0x4D,0x49,0x56, 0x53,0x4F, 0x03,0x00, 0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x03
};

// CRC32（zlib 同一多项式 0xEDB88320；GPT 头/项数组都用它）
static uint32_t part_crc32(const uint8_t* p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return c ^ 0xFFFFFFFFu;
}

static void gpt_put_entry(uint8_t* ents, int idx, const uint8_t type[16], const uint8_t uniq[16],
                          uint64_t first, uint64_t last, const char* name) {
    uint8_t* e = ents + idx * 128;
    memzero(e, 128);
    for (int i = 0; i < 16; i++) { e[i] = type[i]; e[16 + i] = uniq[i]; }
    for (int i = 0; i < 8; i++) {
        e[32 + i] = (uint8_t)((first >> (8 * i)) & 0xFF);
        e[40 + i] = (uint8_t)((last >> (8 * i)) & 0xFF);
    }
    int i = 0;
    while (name[i] && i < 36) { e[56 + i * 2] = (uint8_t)name[i]; i++; }   // UTF-16LE（ASCII 高位 0）
}

// ==================== v0.5.14：GPT 定位 / 读项 / 清项 ====================
// 为什么需要这一层（真缺陷，见 tests/partmgr64_test.py 的实测）：界面上的"分区 N"
// 可能来自 GPT；而旧实现只按 MBR 项号写盘，在 GPT 盘上会删错项、甚至把 LBA1 的
// 主 GPT 头清零。这里把"表"显式化：界面给 src+index，内核重读表拿真实几何。
static uint64_t rd64(const uint8_t* p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
static bool eq16(const uint8_t* a, const uint8_t* b) {
    for (int i = 0; i < 16; i++) if (a[i] != b[i]) return false;
    return true;
}
static void copy_str(char* d, int cap, const char* s) {
    if (!d || cap <= 0) { return; }
    int i = 0;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

// 头自检：magic + 头大小 92 + 头 CRC。只认 magic 会把被别的工具写坏的盘当好盘，
// 然后照着垃圾项数组去删分区 —— 判据必须包括 CRC。
// ★ 踩坑记录（本版实测）：头 CRC 的定义是"把 HeaderCRC32 字段置 0 后对 92 字节求
//   CRC32"。第一版没清零就直接算，于是**所有合法 GPT 头都被判成坏头**（表现：
//   "[PART] 删除失败：这块盘上没有可用的 GPT"，虽然盘上的头完全正常）。
static bool gpt_hdr_ok64(const uint8_t* h_in) {
    uint8_t h[92];
    for (int i = 0; i < 92; i++) h[i] = h_in[i];
    const uint32_t stored = rd32(h + 16);
    h[16] = 0; h[17] = 0; h[18] = 0; h[19] = 0;
    if (!(h[0] == 'E' && h[1] == 'F' && h[2] == 'I' && h[3] == ' ')) return false;
    if (rd32(h + 12) != 92) return false;
    return stored == part_crc32(h, 92);
}

// 读指定 LBA 上的 GPT 头（主头 LBA1 / 备份头 盘尾）
static bool gpt_locate_at64(int drive, uint32_t lba, uint32_t disk_sectors,
                            bool primary, GptLoc* out) {
    if (out) out->valid = false;
    if (!out || !ata64_read_sector(drive, lba, g_sec)) return false;
    if (!gpt_hdr_ok64(g_sec)) return false;
    const uint32_t ent_lba = rd32(g_sec + 72);
    const int cnt = (int)rd32(g_sec + 80);
    const int sz  = (int)rd32(g_sec + 84);
    if (sz != 128 || cnt <= 0 || cnt > 1024) return false;
    if (ent_lba < 2 || ent_lba >= disk_sectors) return false;
    out->valid = true;
    out->primary = primary;
    out->hdr_lba = lba;
    out->ent_lba = ent_lba;
    out->ent_count = cnt;
    out->ent_size = sz;
    return true;
}

bool part_gpt_locate64(int drive, uint32_t disk_sectors, GptLoc* out) {
    if (!out) return false;
    out->valid = false;
    if (drive < 0 || disk_sectors < 40) return false;
    if (gpt_locate_at64(drive, 1, disk_sectors, true, out)) return true;
    return gpt_locate_at64(drive, disk_sectors - 1, disk_sectors, false, out);
}

// 项数组整段读进 g_io（128 项 * 128B = 16KB < 64KB）
static bool gpt_read_entries64(int drive, const GptLoc& loc, int* out_bytes) {
    const int bytes = loc.ent_count * loc.ent_size;
    if (bytes <= 0 || bytes > (int)sizeof(g_io)) return false;
    const uint32_t secs = (uint32_t)((bytes + 511) / 512);
    if (!ata64_read(drive, loc.ent_lba, secs, g_io)) return false;
    if (out_bytes) *out_bytes = bytes;
    return true;
}

// 把 g_io 里的项数组写回，并重算项数组 CRC 与头 CRC（MyLBA/AlternateLBA 一律不动）
static bool gpt_write_entries64(int drive, const GptLoc& loc, int bytes) {
    const uint32_t secs = (uint32_t)((bytes + 511) / 512);
    if (!ata64_write(drive, loc.ent_lba, secs, g_io)) return false;
    if (!ata64_read_sector(drive, loc.hdr_lba, g_sec)) return false;
    if (!gpt_hdr_ok64(g_sec)) return false;
    wr32(g_sec + 88, part_crc32(g_io, bytes));
    wr32(g_sec + 16, 0);
    wr32(g_sec + 16, part_crc32(g_sec, 92));
    return ata64_write_sector(drive, loc.hdr_lba, g_sec);
}

bool part_gpt_entry64(int drive, const GptLoc& loc, int index, GptEntryInfo* out) {
    if (!out || !loc.valid || index < 0 || index >= loc.ent_count) return false;
    int bytes = 0;
    if (!gpt_read_entries64(drive, loc, &bytes)) return false;
    const uint8_t* e = g_io + (uint32_t)index * (uint32_t)loc.ent_size;
    bool nonzero = false;
    for (int i = 0; i < 16; i++) if (e[i]) { nonzero = true; break; }
    out->used = nonzero;
    out->is_esp = nonzero && eq16(e, GPT_TYPE_ESP);
    out->is_basic = nonzero && eq16(e, GPT_TYPE_BASIC);
    out->first = nonzero ? rd64(e + 32) : 0;
    out->last  = nonzero ? rd64(e + 40) : 0;
    return true;
}

static bool gpt_clear_at64(int drive, const GptLoc& loc, int index, uint32_t* out_first) {
    int bytes = 0;
    if (!gpt_read_entries64(drive, loc, &bytes)) return false;
    uint8_t* e = g_io + (uint32_t)index * (uint32_t)loc.ent_size;
    if (out_first) *out_first = (uint32_t)rd64(e + 32);
    memzero(e, (uint32_t)loc.ent_size);
    return gpt_write_entries64(drive, loc, bytes);
}

bool part_gpt_delete_entry64(int drive, uint32_t disk_sectors, int index) {
    GptLoc loc;
    if (!part_gpt_locate64(drive, disk_sectors, &loc)) return false;
    if (index < 0 || index >= loc.ent_count) return false;
    GptEntryInfo gi;
    if (!part_gpt_entry64(drive, loc, index, &gi) || !gi.used) return false;
    const uint32_t first = (uint32_t)gi.first;
    bool ok = gpt_clear_at64(drive, loc, index, 0);
    // 另一份（主/备按规范逐项镜像 -> 同号项就是要清的那一项）
    const uint32_t other_lba = loc.primary ? (disk_sectors - 1) : 1u;
    GptLoc other;
    if (other_lba != loc.hdr_lba &&
        gpt_locate_at64(drive, other_lba, disk_sectors, !loc.primary, &other)) {
        const bool ok2 = gpt_clear_at64(drive, other, index, 0);
        if (!ok2) ok = false;
        dbg64_str("[PART] GPT 另一份项数组同步清理 ent_lba=");
        dbg64_dec(other.ent_lba);
        dbg64_str(ok2 ? " ok" : " FAIL");
        dbg64_nl();
    }
    dbg64_str("[PART] GPT 删除项 index=");
    dbg64_dec(index);
    dbg64_str(" start=");
    dbg64_dec(first);
    dbg64_nl();
    return ok;
}

// 抹掉盘上所有 GPT 头（新建分区表前调用）。只清**确实定位到**的头，不盲写 LBA1 ——
// LBA1 在本系统的布局里是 loader64.bin，盲写会把安装介质自己的引导链打断。
bool part_gpt_wipe64(int drive, uint32_t disk_sectors) {
    if (drive < 0 || disk_sectors < 40) return false;
    bool wiped = false;
    GptLoc loc;
    if (gpt_locate_at64(drive, 1, disk_sectors, true, &loc)) {
        memzero(g_sec, 512);
        if (ata64_write_sector(drive, 1, g_sec)) {
            wiped = true;
            dbg64_str("[PART] 抹掉主 GPT 头 lba=1");
            dbg64_nl();
        }
    }
    if (gpt_locate_at64(drive, disk_sectors - 1, disk_sectors, false, &loc)) {
        memzero(g_sec, 512);
        if (ata64_write_sector(drive, disk_sectors - 1, g_sec)) {
            wiped = true;
            dbg64_str("[PART] 抹掉备份 GPT 头 lba=");
            dbg64_dec(disk_sectors - 1);
            dbg64_nl();
        }
    }
    return wiped;
}

// MBR 里"起点和这一段相同"的项清掉（我们自己的安装盘是 MBR 项 + 盘尾 GPT 双份）。
// 保护性 0xEE 项**永远不动** —— 清掉它 MBR 固件就再也解析不了这块盘。
static bool mbr_clear_by_start64(int drive, uint32_t start, char* out_type, int cap) {
    (void)out_type; (void)cap;
    if (!ata64_read_sector(drive, 0, g_sec)) return false;
    PartInfo p[4];
    part_parse_mbr(g_sec, p);
    bool changed = false;
    for (int i = 0; i < 4; i++) {
        if (!p[i].used || p[i].start != start || p[i].type == 0xEE) continue;
        memzero(g_sec + 446 + i * 16, 16);
        changed = true;
        dbg64_str("[PART] MBR 同段项同步清理 index=");
        dbg64_dec(i + 1);
        dbg64_nl();
    }
    if (!changed) return false;
    g_sec[510] = 0x55;
    g_sec[511] = 0xAA;
    return ata64_write_sector(drive, 0, g_sec);
}

bool part_delete_partition64(int drive, uint32_t disk_sectors, const PartTarget& t) {
    if (drive < 0 || disk_sectors == 0) return false;
    uint32_t start = t.start;
    uint32_t sectors = t.sectors;
    bool ok = false;
    if (t.src == PART_SRC_GPT) {
        GptLoc loc;
        if (!part_gpt_locate64(drive, disk_sectors, &loc)) {
            dbg64_str("[PART] 删除失败：这块盘上没有可用的 GPT（主/备头都没过 CRC 自检）drive=");
            dbg64_dec(drive);
            dbg64_nl();
            return false;
        }
        GptEntryInfo gi;
        if (!part_gpt_entry64(drive, loc, t.index, &gi) || !gi.used) {
            dbg64_str("[PART] 删除失败：GPT 项不存在 index=");
            dbg64_dec(t.index);
            dbg64_nl();
            return false;
        }
        start = (uint32_t)gi.first;
        sectors = (gi.last >= gi.first) ? (uint32_t)(gi.last - gi.first + 1) : 0;
        ok = part_gpt_delete_entry64(drive, disk_sectors, t.index);
        mbr_clear_by_start64(drive, start, 0, 0);
    } else {
        if (t.index < 0 || t.index > 3) return false;
        if (!ata64_read_sector(drive, 0, g_sec)) return false;
        PartInfo p[4];
        part_parse_mbr(g_sec, p);
        if (!p[t.index].used) {
            dbg64_str("[PART] 删除失败：MBR 项不存在 index=");
            dbg64_dec(t.index + 1);
            dbg64_nl();
            return false;
        }
        if (p[t.index].type == 0xEE) {
            dbg64_str("[PART] 删除失败：保护性 MBR 项（0xEE）不是真分区 index=");
            dbg64_dec(t.index + 1);
            dbg64_nl();
            return false;
        }
        start = p[t.index].start;
        sectors = p[t.index].sectors;
        memzero(g_sec + 446 + t.index * 16, 16);
        g_sec[510] = 0x55;
        g_sec[511] = 0xAA;
        ok = ata64_write_sector(drive, 0, g_sec);
        // GPT（主或备）里描述同一段的那一项也清掉，否则"删了分区，盘尾 GPT 还留着它"
        GptLoc loc;
        if (part_gpt_locate64(drive, disk_sectors, &loc)) {
            for (int i = 0; i < loc.ent_count; i++) {
                GptEntryInfo gi;
                if (!part_gpt_entry64(drive, loc, i, &gi) || !gi.used) continue;
                if ((uint32_t)gi.first != start) continue;
                if (!part_gpt_delete_entry64(drive, disk_sectors, i)) ok = false;
                break;
            }
        }
    }
    // 抹掉被删分区的首扇区：旧超级块/引导记录留着，别的工具仍会把它当成一个活卷
    uint32_t wiped = 0;
    if (ok && sectors) {
        memzero(g_io, 512);
        if (ata64_write_sector(drive, start, g_io)) wiped = 1;
    }
    dbg64_str("[PART] 删除分区 OK drive=");
    dbg64_dec(drive);
    dbg64_str(t.src == PART_SRC_GPT ? " table=gpt index=" : " table=mbr index=");
    dbg64_dec(t.index + 1);
    dbg64_str(" start=");
    dbg64_dec(start);
    dbg64_str(" sectors=");
    dbg64_dec(sectors);
    dbg64_str(" wiped=");
    dbg64_dec(wiped);
    dbg64_nl();
    return ok;
}

int part_format_partition64(int drive, uint32_t disk_sectors, const PartTarget& t,
                            char* fs_out, int fs_cap) {
    copy_str(fs_out, fs_cap, "");
    if (drive < 0 || disk_sectors == 0) return -1;
    const bool gpt = (t.src == PART_SRC_GPT);
    uint32_t start = t.start;
    uint32_t sectors = t.sectors;
    uint8_t mtype = t.mbr_type;
    bool is_esp = t.gpt_esp;
    bool is_basic = t.gpt_basic;
    if (gpt) {
        GptLoc loc;
        GptEntryInfo gi;
        if (!part_gpt_locate64(drive, disk_sectors, &loc) ||
            !part_gpt_entry64(drive, loc, t.index, &gi) || !gi.used) {
            dbg64_str("[PART] 格式化失败：读不到 GPT 项 index=");
            dbg64_dec(t.index);
            dbg64_nl();
            return -1;
        }
        start = (uint32_t)gi.first;
        sectors = (gi.last >= gi.first) ? (uint32_t)(gi.last - gi.first + 1) : 0;
        is_esp = gi.is_esp;
        is_basic = gi.is_basic;
    } else {
        if (t.index < 0 || t.index > 3) return -1;
        if (!ata64_read_sector(drive, 0, g_sec)) return -1;
        PartInfo p[4];
        part_parse_mbr(g_sec, p);
        if (!p[t.index].used) {
            dbg64_str("[PART] 格式化失败：MBR 项不存在 index=");
            dbg64_dec(t.index + 1);
            dbg64_nl();
            return -1;
        }
        if (p[t.index].type == 0xEE) {
            copy_str(fs_out, fs_cap, "refused-protective");
            dbg64_str("[PART] 拒绝格式化保护性 MBR 项（0xEE 不是真分区）index=");
            dbg64_dec(t.index + 1);
            dbg64_nl();
            return -2;
        }
        start = p[t.index].start;
        sectors = p[t.index].sectors;
        mtype = p[t.index].type;
        // 盘尾那个 0xEF 是安装器建的 ESP（真 FAT32）——抹了 UEFI 就起不来
        if (mtype == 0xEF && start > PART_BOOT_LBA + PART_BOOT_SECS - 1) is_esp = true;
    }
    if (sectors == 0) {
        dbg64_str("[PART] 格式化失败：分区扇区数为 0 index=");
        dbg64_dec(t.index + 1);
        dbg64_nl();
        return -1;
    }
    if (is_esp) {
        copy_str(fs_out, fs_cap, "refused-esp");
        dbg64_str("[PART] 拒绝格式化 EFI 系统分区（UEFI 引导卷）start=");
        dbg64_dec(start);
        dbg64_nl();
        return -2;
    }
    // 清分区首部 64 扇区（抹掉旧引导记录/超级块；不整盘擦，避免等待过久）
    uint32_t n = (sectors < 64) ? sectors : 64;
    memzero(g_io, sizeof(g_io));
    for (uint32_t i = 0; i < n; i += INSTALL_CHUNK_SECTORS) {
        uint32_t c = n - i;
        if (c > INSTALL_CHUNK_SECTORS) c = INSTALL_CHUNK_SECTORS;
        if (!ata64_write(drive, start + i, c, g_io)) {
            dbg64_str("[PART] 格式化失败：清首部写盘失败 lba=");
            dbg64_dec(start + i);
            dbg64_nl();
            return -1;
        }
    }
    // 真文件系统：主数据分区（MBR 0x07 / GPT Microsoft Basic Data）-> VimtuFS2；
    //               FAT32 类分区（MBR 0x0B/0x0C/0x0E 且够大）-> FAT32；其余只清首部。
    const char* fs = "blank";
    if (gpt ? is_basic : (mtype == 0x07)) {
        if (vfs64_format(drive, start, sectors) != 0) {
            copy_str(fs_out, fs_cap, "FAILED");
            dbg64_str("[PART] 格式化失败：VimtuFS2 建卷未成功 start=");
            dbg64_dec(start);
            dbg64_nl();
            return -1;
        }
        fs = "VimtuFS2";
    } else if (!gpt && (mtype == 0x0B || mtype == 0x0C || mtype == 0x0E)) {
        if (fat64_format64(drive, start, sectors) == 0) {
            fs = "FAT32";
        } else {
            fs = "blank";          // 分区太小装不下 FAT32：如实降级（fat64 自己会打 reject 行）
        }
    }
    copy_str(fs_out, fs_cap, fs);
    dbg64_str("[PART] 格式化 OK drive=");
    dbg64_dec(drive);
    dbg64_str(gpt ? " table=gpt index=" : " table=mbr index=");
    dbg64_dec(t.index + 1);
    dbg64_str(" start=");
    dbg64_dec(start);
    dbg64_str(" sectors=");
    dbg64_dec(sectors);
    dbg64_str(" fs=");
    dbg64_str(fs);
    dbg64_nl();
    return 0;
}

// 写备份 GPT：项数组在第 (总扇区-33) LBA，头在最后一个 LBA
static bool part_write_backup_gpt64(int drive, uint32_t disk_sectors, uint32_t esp_start,
                                    uint32_t esp_sectors, uint32_t main_secs) {
    const uint32_t ent_lba = disk_sectors - PART_GPT_BACKUP_SECTORS;
    const uint32_t hdr_lba = disk_sectors - 1;
    const uint32_t ent_bytes = PART_GPT_ENTRIES * 128;
    if (ent_bytes > sizeof(g_io)) return false;
    memzero(g_io, ent_bytes);
    gpt_put_entry(g_io, 0, GPT_TYPE_BASIC, GPT_PART1_GUID,
                  PART_MAIN_LBA, PART_MAIN_LBA + main_secs - 1, "VIMTU64 MAIN");
    gpt_put_entry(g_io, 1, GPT_TYPE_ESP, GPT_PART2_GUID,
                  esp_start, esp_start + esp_sectors - 1, "VIMTU64 ESP");
    const uint32_t ent_crc = part_crc32(g_io, ent_bytes);

    uint8_t h[512];
    memzero(h, 512);
    h[0] = 'E'; h[1] = 'F'; h[2] = 'I'; h[3] = ' '; h[4] = 'P'; h[5] = 'A'; h[6] = 'R'; h[7] = 'T';
    wr32(h + 8, 0x00010000);                              // 版本 1.0
    wr32(h + 12, 92);                                     // 头大小
    wr32(h + 16, 0);                                      // 头 CRC（填完再算）
    wr32(h + 20, 0);
    for (int i = 0; i < 8; i++) {                          // MyLBA（备份头位置）
        h[24 + i] = (uint8_t)(((uint64_t)hdr_lba >> (8 * i)) & 0xFF);
        h[32 + i] = (uint8_t)((1ULL >> (8 * i)) & 0xFF);   // AlternateLBA = 1（那里是 loader，故意无效）
        h[40 + i] = (uint8_t)((34ULL >> (8 * i)) & 0xFF);  // FirstUsableLBA
        h[48 + i] = (uint8_t)(((uint64_t)(disk_sectors - PART_GPT_BACKUP_SECTORS - 1) >> (8 * i)) & 0xFF);
        h[72 + i] = (uint8_t)(((uint64_t)ent_lba >> (8 * i)) & 0xFF);
    }
    for (int i = 0; i < 16; i++) h[56 + i] = GPT_DISK_GUID[i];
    wr32(h + 80, PART_GPT_ENTRIES);
    wr32(h + 84, 128);
    wr32(h + 88, ent_crc);
    wr32(h + 16, part_crc32(h, 92));

    if (!ata64_write(drive, ent_lba, ent_bytes / 512, g_io)) {
        dbg64_str("[INSTALL] GPT 项数组写失败 ent_lba=");
        dbg64_dec(ent_lba);
        dbg64_nl();
        return false;
    }
    if (!ata64_write_sector(drive, hdr_lba, h)) {
        dbg64_str("[INSTALL] GPT 备份头写失败 hdr_lba=");
        dbg64_dec(hdr_lba);
        dbg64_nl();
        return false;
    }
    return true;
}

// ESP：格式化 **FAT32** + 写 EFI/BOOT/BOOTX64.EFI + UEFI64.BIN + KERNEL64.BIN
// KERNEL64.BIN 的内容 = 目标盘 LBA 9..8008 的 4MB 内核区（**从目标盘自己读**）：
//   安装程序内核装不下这份 4MB 副本（内核区上限 4MB），而载荷已经把系统内核拷到了那里。
static int part_install_esp64(int drive, uint32_t esp_start, uint32_t esp_sectors) {
    if (fat64_format64(drive, esp_start, esp_sectors) != 0) {
        dbg64_str("[INSTALL] esp: lba=");
        dbg64_dec(esp_start);
        dbg64_str(" sectors=");
        dbg64_dec(esp_sectors);
        dbg64_str(" fat_ok=0");
        dbg64_nl();
        return -1;
    }
    dbg64_str("[INSTALL] esp: lba=");
    dbg64_dec(esp_start);
    dbg64_str(" sectors=");
    dbg64_dec(esp_sectors);
    dbg64_str(" fs=FAT32 clusters=");
    dbg64_dec(fat64_last_clusters64());
    dbg64_str(" fat_ok=1");
    dbg64_nl();

    const uint32_t stub_len  = embedded_stub_len64();
    const uint32_t uefi_len  = embedded_loader_len64();
    const uint32_t kern_len  = ML64_KERNEL_SECTORS * 512;      // 4,096,000（内核区整块）
    bool ok = true;
    if (fat64_mkdir64(drive, "EFI") != 0) ok = false;
    if (ok && fat64_mkdir64(drive, "EFI/BOOT") != 0) ok = false;
    if (ok && fat64_write_file64(drive, "EFI/BOOT/BOOTX64.EFI",
                                 _binary_build64_BOOTX64_EFI_start, stub_len) != 0) ok = false;
    if (ok && fat64_write_file64(drive, "UEFI64.BIN",
                                 _binary_build64_UEFI64_BIN_start, uefi_len) != 0) ok = false;
    if (ok && fat64_write_file_from_disk64(drive, "KERNEL64.BIN", drive,
                                           ML64_KERNEL_LBA, kern_len) != 0) ok = false;
    dbg64_str("[INSTALL] esp files: BOOTX64.EFI=");
    dbg64_dec(stub_len);
    dbg64_str("B UEFI64.BIN=");
    dbg64_dec(uefi_len);
    dbg64_str("B KERNEL64.BIN=");
    dbg64_dec(kern_len);
    dbg64_str(ok ? "B" : "B (FAIL)");
    dbg64_nl();
    return ok ? 0 : -1;
}

// 安装收尾：写混合 MBR（+ 可选 ESP 项）与盘尾备份 GPT。
// 打点：无 ESP 时明确说明（盘太小），有 ESP 时给"gpt written (main + esp), pmbr ok"。
static bool part_write_gpt64(int drive, uint32_t disk_sectors) {
    uint32_t esp_start = 0, esp_sectors = 0, main_secs = 0;
    const bool has_esp = part_esp_geometry64(disk_sectors, &esp_start, &esp_sectors, &main_secs);
    if (!ata64_read_sector(drive, 0, g_sec)) return false;
    build_standard_table(g_sec, disk_sectors);
    if (!ata64_write_sector(drive, 0, g_sec)) return false;
    if (!has_esp) {
        dbg64_str("[INSTALL] gpt skipped (disk too small for ESP), mbr ok sectors=");
        dbg64_dec(disk_sectors);
        dbg64_nl();
        return true;
    }
    if (!part_write_backup_gpt64(drive, disk_sectors, esp_start, esp_sectors, main_secs)) return false;
    dbg64_str("[INSTALL] gpt written (main + esp), pmbr ok main=");
    dbg64_dec(PART_MAIN_LBA);
    dbg64_str("+");
    dbg64_dec(main_secs);
    dbg64_str(" esp=");
    dbg64_dec(esp_start);
    dbg64_str("+");
    dbg64_dec(esp_sectors);
    dbg64_str(" backup_hdr_lba=");
    dbg64_dec(disk_sectors - 1);
    dbg64_nl();
    return true;
}

// ---------------- 安装：整盘复制 ----------------
// ★ item 5a：枚举改成"槽位"接口（PATA 0..3 + AHCI 8..），不再写死 0..3 ——
//   否则 SATA 盘上的安装介质/目标盘会被漏掉。驱动号语义见 kernel/ata64.h。
int part_find_payload_drive(uint32_t payload_lba, uint32_t* out_sectors) {
    const int slots = ata64_drive_count64();
    for (int s = 0; s < slots; s++) {
        const int d = ata64_slot_to_drive64(s);
        if (d < 0) continue;
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
    const int slots = ata64_drive_count64();          // ★ item 5a：含 AHCI 盘（U 盘挂 SATA 时也要认出来）
    for (int s = 0; s < slots; s++) {
        const int d = ata64_slot_to_drive64(s);
        if (d < 0) continue;
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


// ==================== ★ v0.5.13：安装后把卷按主分区大小扩容 ====================
// 背景（用户报告）：用 ISO 装到 8G 盘，主分区覆盖整盘，但**系统卷只有 12 MiB**
//   （载荷里的卷是构建期按 16MB 模板生成的）。这里在安装收尾把卷"长大"到主分区大小。
// 为什么不用改盘格式：VimtuFS2 的布局是"位图紧跟超级块、inode 区紧跟位图、数据区紧跟 inode 区"，
//   所以扩容必须**重排**：新 bmn = ceil(new_total/4096)、新 data = 1+bmn+ceil(inon/4)。
//   数据块整体后移（新位置永远 >= 旧位置 -> 从后往前搬，不会自覆盖），位图按旧位图平移重建，
//   最后重写超级块（version 仍是 4，内核与 mount 校验完全不用改）。
static uint32_t pg_crc32(const uint8_t* b, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
    }
    return ~c;
}

// 超级块字段偏移（与 kernel/vfs64.cpp 的 VFS_O_* 一致；这里复制一份，避免暴露内部常量）
#define PG_O_MAGIC   0u
#define PG_O_VERSION 8u
#define PG_O_TOTAL   20u
#define PG_O_ROOT    24u
#define PG_O_BITMAP  28u
#define PG_O_BITMAPN 32u
#define PG_O_INODE   36u
#define PG_O_INODEN  40u
#define PG_O_INOSZ   44u
#define PG_O_DATA    48u
#define PG_O_DATAN   52u
#define PG_O_FLAGS   56u
#define PG_O_CRC     60u
#define PG_O_SIG     510u
#define PG_SB_CRC_LEN 60u

// 返回 1 = 扩容成功；0 = 无需扩容（盘太小/已够大）；-1 = 失败（保持原卷可用）
// 把一个"块号数组块"（一级间接块，或二级间接块里的子块）里的绝对块号整体平移 delta。
// 只动落在旧数据区 [data_old, old_total) 的号；0 = 空槽不动。没有改动就不写盘。
static bool pg_patch_ptr_block64(int drive, uint32_t base_lba, uint32_t blk,
                                 uint32_t data_old, uint32_t old_total, uint32_t delta,
                                 uint32_t* counter) {
    if (!ata64_read(drive, base_lba + blk, 1, g_sec)) return false;
    bool dirty = false;
    for (uint32_t i = 0; i < VFS64_INDIRECT_PTRS; i++) {
        const uint32_t v = rd32(g_sec + i * 4);
        if (v >= data_old && v < old_total) { wr32(g_sec + i * 4, v + delta); dirty = true; }
    }
    if (!dirty) return true;
    if (counter) (*counter)++;
    return ata64_write(drive, base_lba + blk, 1, g_sec);
}

static int part_volume_grow64(int drive, uint32_t base_lba, uint32_t main_sectors) {
    static const char magic[8] = { 'V','I','M','T','U','F','S','2' };
    if (!ata64_read(drive, base_lba, 1, g_sec)) return -1;
    for (int i = 0; i < 8; i++) if (g_sec[PG_O_MAGIC + i] != (uint8_t)magic[i]) return 0;   // 没有卷：不动
    if (pg_crc32(g_sec, PG_SB_CRC_LEN) != rd32(g_sec + PG_O_CRC)) return -1;
    const uint32_t ver = rd32(g_sec + PG_O_VERSION);
    if (ver != 4u && ver != 3u) return 0;                       // 只处理 v3/v4（v2 太老，不动）
    const uint32_t old_total = rd32(g_sec + PG_O_TOTAL);
    const uint32_t old_bmn = rd32(g_sec + PG_O_BITMAPN);
    const uint32_t old_ino = rd32(g_sec + PG_O_INODE);   // inode 区起点（随 bmn 变）
    const uint32_t inon = rd32(g_sec + PG_O_INODEN);
    const uint32_t data_old = rd32(g_sec + PG_O_DATA);
    if (old_total == 0 || old_bmn == 0 || data_old == 0 || data_old >= old_total) return -1;
    if (inon == 0 || inon > VFS64_MAX_INODES) return -1;

    uint32_t new_total = main_sectors;
    if (new_total <= old_total + 4096u) return 0;               // 盘没比卷大多少：不值得搬
    const uint32_t bmn_new = (new_total + VFS64_BITMAP_BLK_BITS - 1u) / VFS64_BITMAP_BLK_BITS;
    const uint32_t ino_blocks = (inon + VFS64_INODES_PER_BLK - 1u) / VFS64_INODES_PER_BLK;
    const uint32_t ino_new = 1u + bmn_new;
    const uint32_t data_new = ino_new + ino_blocks;
    if (data_new >= new_total) return 0;
    if (data_new <= data_old) return 0;                         // 理论上必真（bmn 只增）

    dbg64_line_begin64();
    dbg64_str("[INSTALL] volume grow total=");
    dbg64_dec(old_total);
    dbg64_str("->");
    dbg64_dec(new_total);
    dbg64_str(" data=");
    dbg64_dec(data_old);
    dbg64_str("->");
    dbg64_dec(data_new);
    dbg64_str(" inode=");
    dbg64_dec(old_ino);
    dbg64_str("->");
    dbg64_dec(ino_new);
    dbg64_str(" bmn=");
    dbg64_dec(old_bmn);
    dbg64_str("->");
    dbg64_dec(bmn_new);
    dbg64_nl();
    dbg64_line_end64();

    // ---- 1) **inode 区 + 数据区**整体后移 ----
    //   ★ 关键：新布局里 inode 区起点从 old_ino(=1+bmn_old) 变成 ino_new(=1+bmn_new)，
    //     只搬数据区会让 inode 区留在原位（现在属于位图区）—— 卷能过 magic/CRC/几何校验，
    //     但根目录 inode 读到的是垃圾 -> mount_system FAILED（本批踩过：扩容后 NOT-PERSISTENT）。
    //   inode 区长度不变（inon 未变、每块 4 个 inode），所以 inode 区和数据区可以当成
    //   **一整段 [old_ino, old_total)** 平移 (ino_new - old_ino) 块。
    //   新位置恒 >= 旧位置 -> 从后往前搬，不会自覆盖。
    if (old_ino == 0 || old_ino >= old_total) return -1;
    uint32_t moved = 0;
    uint32_t blk = old_total;
    while (blk > old_ino) {
        uint32_t n = blk - old_ino;
        if (n > INSTALL_CHUNK_SECTORS) n = INSTALL_CHUNK_SECTORS;
        const uint32_t src = old_ino + (blk - old_ino) - n;      // 本批起始旧块
        const uint32_t dst = ino_new + (blk - old_ino) - n;      // 对应新块
        if (!ata64_read(drive, base_lba + src, n, g_io)) return -1;
        if (!ata64_write(drive, base_lba + dst, n, g_io)) return -1;
        blk -= n;
        moved += n;
    }

    // ---- 2) 旧位图**先读进内存**，再动新位图 ----
    //   ★ 踩坑（v0.5.13 的实现在这里顺序错了）：先把新位图区 [1, 1+bmn_new) 整体清零，
    //     然后才"按旧位图平移"去读旧位图 —— 而旧位图正好落在被清零的那段里
    //     （旧卷 12 MiB -> 旧位图 = 扇区 1..7）。结果读到的全是 0，"哪些块被占用"整个丢了：
    //     装完 free 比正确值还多，数据块随时会被新分配覆盖。
    if (old_bmn > 32u) {
        dbg64_line_begin64();
        dbg64_str("[INSTALL] volume grow skipped: 旧位图过大 bmn=");
        dbg64_dec(old_bmn);
        dbg64_str("（上限 32 扇区 = 16 KiB，即旧卷 <= 128 MiB）");
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    static uint8_t g_bm_old[32 * 512];
    if (!ata64_read(drive, base_lba + 1u, old_bmn, g_bm_old)) return -1;

    // ---- 2b) 清空新位图（多出来的部分原先是数据区）----
    memzero(g_io, sizeof(g_io));
    for (uint32_t i = 0; i < bmn_new; i += INSTALL_CHUNK_SECTORS) {
        uint32_t c = bmn_new - i;
        if (c > INSTALL_CHUNK_SECTORS) c = INSTALL_CHUNK_SECTORS;
        if (!ata64_write(drive, base_lba + 1u + i, c, g_io)) return -1;
    }

    // ---- 3a) 头部（块 0..data_new-1）全部置"已用" ----
    const uint32_t bm_bit = VFS64_BITMAP_BLK_BITS;
    {
        const uint32_t full = data_new / bm_bit;            // 整扇区都是 1 的部分
        const uint32_t rem = data_new % bm_bit;             // 末尾扇区里前 rem 位是 1
        for (uint32_t done = 0; done < full;) {
            uint32_t c = full - done;
            if (c > INSTALL_CHUNK_SECTORS) c = INSTALL_CHUNK_SECTORS;
            for (uint32_t k = 0; k < c * 512u; k++) g_io[k] = 0xFF;
            if (!ata64_write(drive, base_lba + 1u + done, c, g_io)) return -1;
            done += c;
        }
        if (rem) {
            if (!ata64_read(drive, base_lba + 1u + full, 1, g_sec)) return -1;
            for (uint32_t b = 0; b < rem / 8u; b++) g_sec[b] = 0xFF;
            const uint32_t left = rem % 8u;
            if (left) g_sec[rem / 8u] = (uint8_t)((1u << left) - 1u);
            if (!ata64_write(drive, base_lba + 1u + full, 1, g_sec)) return -1;
        }
    }

    // ---- 3b) 旧位图 -> 新位图：新块号 = data_new + (旧块号 - data_old) ----
    //   逐个旧"已用"位处理，按新位图扇区聚合成一次读-改-写（保留 3a 刚置上的头部位）。
    {
        const uint32_t old_datan = old_total - data_old;
        uint32_t cur_sec = 0xFFFFFFFFu;
        uint32_t marked = 0;
        for (uint32_t j = 0; j < old_datan; j++) {
            if (!(g_bm_old[j / 8u] & (uint8_t)(1u << (j % 8u)))) continue;    // 旧：空闲
            const uint32_t nb = data_new + j;
            if (nb >= new_total) continue;
            const uint32_t sec = nb / bm_bit;
            const uint32_t bit = nb % bm_bit;
            if (sec != cur_sec) {
                if (cur_sec != 0xFFFFFFFFu) {
                    if (!ata64_write(drive, base_lba + 1u + cur_sec, 1, g_sec)) return -1;
                }
                cur_sec = sec;
                if (!ata64_read(drive, base_lba + 1u + sec, 1, g_sec)) return -1;
            }
            g_sec[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
            marked++;
        }
        if (cur_sec != 0xFFFFFFFFu) {
            if (!ata64_write(drive, base_lba + 1u + cur_sec, 1, g_sec)) return -1;
        }
        dbg64_line_begin64();
        dbg64_str("[INSTALL] volume grow bitmap rebuilt used_data_blocks=");
        dbg64_dec(marked);
        dbg64_nl();
        dbg64_line_end64();
    }

    // ---- 3c) 卷外（>= new_total）的位必须置 1：分配器永不发放 ----
    {
        const uint32_t bm_sec = 1u + new_total / bm_bit;
        const uint32_t bit = new_total % bm_bit;
        if (bm_sec < 1u + bmn_new) {
            if (!ata64_read(drive, base_lba + bm_sec, 1, g_sec)) return -1;
            for (uint32_t k = bit; k < bm_bit; k++) g_sec[k / 8u] |= (uint8_t)(1u << (k % 8u));
            if (!ata64_write(drive, base_lba + bm_sec, 1, g_sec)) return -1;
        }
    }

    // ---- 3d) v0.5.15：**重定位块号** ----
    //   inode 的 d0..d3 / ind / dind 里存的是**绝对块号**；整段后移 Δ 之后它们全过期。
    //   v0.5.13 只搬数据、没改指针 —— 装完卷大小对了，但每个文件都指向错位的数据
    //   （实测 [VFS64] BAD block=... not in data area、skip corrupt inode reason=ind range、
    //     /hello.elf /proc64.elf 等全找不到、桌面起不来）。
    //   这里把每个 inode 的指针、以及它引用的一级/二级间接块内容一起平移 Δ。
    {
        const uint32_t delta = ino_new - old_ino;
        const uint32_t inodes_per_blk = 512u / 128u;         // v3/v4：128 B/个 -> 4 个/块
        uint32_t ino_fixed = 0, ptr_fixed = 0;
        static uint8_t g_dind[512];
        for (uint32_t b = 0; b < ino_blocks; b += 16u) {
            uint32_t c = ino_blocks - b;
            if (c > 16u) c = 16u;                            // 16 块 = 8 KiB = 64 个 inode
            if (!ata64_read(drive, base_lba + ino_new + b, c, g_io)) return -1;
            bool dirty = false;
            const uint32_t nin = c * inodes_per_blk;
            for (uint32_t k = 0; k < nin; k++) {
                uint8_t* rec = g_io + k * 128u;
                if (rec[0] != 1 && rec[0] != 2) continue;     // 空槽 / 未知类型：不动
                bool touched = false;
                for (int d = 0; d < 4; d++) {
                    const uint32_t v = rd32(rec + 8 + d * 4);
                    if (v >= data_old && v < old_total) { wr32(rec + 8 + d * 4, v + delta); touched = true; }
                }
                const uint32_t ind = rd32(rec + 24);
                if (ind >= data_old && ind < old_total) { wr32(rec + 24, ind + delta); touched = true; }
                const uint32_t dind = rd32(rec + 71);
                if (dind >= data_old && dind < old_total) { wr32(rec + 71, dind + delta); touched = true; }
                if (!touched) continue;
                ino_fixed++;
                dirty = true;
            }
            if (!dirty) continue;
            // 改了指针的 inode 必须重算 CRC32（覆盖 [0,124)）—— 否则整卷被判定为坏 inode
            for (uint32_t k = 0; k < nin; k++) {
                uint8_t* rec = g_io + k * 128u;
                if (rec[0] != 1 && rec[0] != 2) continue;
                wr32(rec + 124, pg_crc32(rec, 124));
            }
            if (!ata64_write(drive, base_lba + ino_new + b, c, g_io)) return -1;
        }
        // 间接块：inode 里的 ind/dind 已经换成新块号，直接按新块号去改内容
        for (uint32_t b = 0; b < ino_blocks; b += 16u) {
            uint32_t c = ino_blocks - b;
            if (c > 16u) c = 16u;
            if (!ata64_read(drive, base_lba + ino_new + b, c, g_io)) return -1;
            const uint32_t nin = c * inodes_per_blk;
            for (uint32_t k = 0; k < nin; k++) {
                const uint8_t* rec = g_io + k * 128u;
                if (rec[0] != 1 && rec[0] != 2) continue;
                const uint32_t ind = rd32(rec + 24);
                if (ind) {
                    if (!pg_patch_ptr_block64(drive, base_lba, ind, data_old, old_total, delta, &ptr_fixed))
                        return -1;
                }
                const uint32_t dind = rd32(rec + 71);
                if (!dind) continue;
                if (!ata64_read(drive, base_lba + dind, 1, g_dind)) return -1;
                bool d2 = false;
                for (uint32_t i = 0; i < VFS64_INDIRECT_PTRS; i++) {
                    const uint32_t ch = rd32(g_dind + i * 4);
                    if (ch >= data_old && ch < old_total) { wr32(g_dind + i * 4, ch + delta); d2 = true; }
                }
                if (d2 && !ata64_write(drive, base_lba + dind, 1, g_dind)) return -1;
                for (uint32_t i = 0; i < VFS64_INDIRECT_PTRS; i++) {
                    const uint32_t ch = rd32(g_dind + i * 4);
                    if (!ch) continue;
                    if (!pg_patch_ptr_block64(drive, base_lba, ch, data_old, old_total, delta, &ptr_fixed))
                        return -1;
                }
            }
        }
        dbg64_line_begin64();
        dbg64_str("[INSTALL] volume grow relocate delta=");
        dbg64_dec(delta);
        dbg64_str(" inodes=");
        dbg64_dec(ino_fixed);
        dbg64_str(" indirect_blocks=");
        dbg64_dec(ptr_fixed);
        dbg64_nl();
        dbg64_line_end64();
    }

    // ---- 5) 重写超级块 ----
    if (!ata64_read(drive, base_lba, 1, g_sec)) return -1;
    wr32(g_sec + PG_O_TOTAL, new_total);
    wr32(g_sec + PG_O_BITMAP, 1u);
    wr32(g_sec + PG_O_BITMAPN, bmn_new);
    wr32(g_sec + PG_O_INODE, ino_new);
    wr32(g_sec + PG_O_DATA, data_new);
    wr32(g_sec + PG_O_DATAN, new_total - data_new);
    wr32(g_sec + PG_O_FLAGS, 0u);
    wr32(g_sec + PG_O_CRC, pg_crc32(g_sec, PG_SB_CRC_LEN));
    g_sec[PG_O_SIG] = 0x55; g_sec[PG_O_SIG + 1] = 0xAA;
    if (!ata64_write(drive, base_lba, 1, g_sec)) return -1;

    dbg64_line_begin64();
    dbg64_str("[INSTALL] volume grow ok moved=");
    dbg64_dec(moved);
    dbg64_str(" total=");
    dbg64_dec(new_total);
    dbg64_str(" (");
    dbg64_dec((uint64_t)new_total / 2048u);
    dbg64_str(" MiB)");
    dbg64_nl();
    dbg64_line_end64();
    return 1;
}

int part_install_step(InstallJob* job) {
    if (!job->active || job->done || job->failed) return job->failed ? -1 : 1;

    uint32_t remain = job->payload_sectors - job->copied;
    if (remain == 0) {
        // 复制完成：先把"完成"行打出去（老验收 grep 这一行），再做 ESP/GPT 这两步可能较慢的收尾。
        dbg64_str("[INSTALL] 完成：已写 ");
        dbg64_dec(job->copied);
        dbg64_str(" 扇区，分区表已更新");
        dbg64_nl();
        // ★ 目标盘 UEFI 可启动的最后一块拼图：
        //   1) ESP（FAT32，48MB）：EFI/BOOT/BOOTX64.EFI + UEFI64.BIN + KERNEL64.BIN
        //      （KERNEL64.BIN 直接读目标盘 LBA 9..8008 —— 载荷已经把系统内核拷到那里）
        //   2) 混合 MBR（0xEF 引导区 + 0x07 主分区 + 0xEF ESP）+ 盘尾备份 GPT
        uint32_t esp_start = 0, esp_sectors = 0, esp_main_secs = 0;
        if (part_esp_geometry64(job->write_table_at, &esp_start, &esp_sectors, &esp_main_secs)) {
            (void)part_install_esp64(job->dst_drive, esp_start, esp_sectors);
        } else {
            dbg64_str("[INSTALL] esp skipped (disk too small) sectors=");
            dbg64_dec(job->write_table_at);
            dbg64_nl();
        }
        // ★ v0.5.13：把卷按主分区大小扩容（8G 盘不再只有 12 MiB 卷）。
        //   必须在写 GPT 之前做：这里要知道主分区扇区数（有 ESP 时主分区到 esp-1 为止）。
        {
            uint32_t esp_start = 0, esp_sectors = 0, esp_main_secs = 0;
            uint32_t main_secs;
            if (part_esp_geometry64(job->write_table_at, &esp_start, &esp_sectors, &esp_main_secs))
                main_secs = esp_main_secs;
            else
                main_secs = (job->write_table_at > PART_MAIN_LBA) ? (job->write_table_at - PART_MAIN_LBA) : 0;
            if (main_secs > PART_MAIN_LBA) {
                const int gr = part_volume_grow64(job->dst_drive, PART_MAIN_LBA, main_secs);
                if (gr < 0) {
                    dbg64_line_begin64();
                    dbg64_str("[INSTALL] volume grow FAILED (keep original volume)");
                    dbg64_nl();
                    dbg64_line_end64();
                }
            }
        }
        if (!part_write_gpt64(job->dst_drive, job->write_table_at)) { job->failed = true; return -1; }
        job->active = false;
        job->done   = true;
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
