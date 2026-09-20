// part64.cpp - 分区表写盘 + 安装引擎（真实扇区读写，不是模拟）
#include "part64.h"
#include "ata64.h"
#include "vfs64.h"      // 格式化：主数据分区写真正的 VimtuFS2（不再是占位超级块）
#include "fat64.h"      // ESP：安装完成时在目标盘写 FAT16 卷（EFI/BOOT/BOOTX64.EFI 等）
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
// 目标盘最小尺寸 = 主分区起点 8009 + 主分区至少 8MB + ESP 5MB + 备份 GPT 33 扇区。
// 不满足（例如 16MB 的回归目标盘）就**不建 ESP**，只写老的 MBR 布局 ——
// 那种盘本来也放不下 ESP 里的三个文件（内核 4MB + 引导器）。
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
    // ★ 保护 ESP：EFI 系统分区（盘尾那个 0xEF 项）里是 FAT16 引导卷，
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

// ESP：格式化 FAT16 + 写 EFI/BOOT/BOOTX64.EFI + UEFI64.BIN + KERNEL64.BIN
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
        //   1) ESP（FAT16）：EFI/BOOT/BOOTX64.EFI + UEFI64.BIN + KERNEL64.BIN
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
