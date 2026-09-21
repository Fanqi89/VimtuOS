// drive64.cpp - 盘符 / 驱动器枚举层的实现（只读：绝不格式化、绝不挂载、绝不写盘）
//
// 打点与规则见 kernel/drive64.h（那份头文件是**唯一契约**，这里只写实现细节）。
//
// 实现要点：
//   * 只用 ata64 的**统一驱动器号**接口（PATA 0..3 / AHCI 8.. / NVMe 16..），
//     每块盘读一次 LBA 0（MBR），再对每个分区读一次首扇区做文件系统指纹识别。
//   * VimtuFS2 卷的容量/可用 = 走 vfs64_probe_volume64（超级块几何 + 位图计数，只读）；
//     这条路径**不碰挂载状态**（vfs64 内部用专用缓冲 + dev_read_at(drive)）。
//   * 盘符表是一次扫描的**快照**（静态数组），UI 反复读表不需要重复扫盘。
#include "drive64.h"
#include "debug64.h"
#include "ata64.h"
#include "vfs64.h"
#include "part64.h"          // 只为常量/struct（PART_MAIN_LBA / PartInfo）；**不调用** part64 的函数

// ==================== 小工具 ====================
static void zero_bytes(void* p, uint32_t n) {
    uint8_t* d = (uint8_t*)p;
    for (uint32_t i = 0; i < n; i++) d[i] = 0;
}
static void copy_str(char* dst, uint32_t cap, const char* src) {
    uint32_t i = 0;
    for (; src[i] != 0 && i + 1u < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}
static uint32_t rd16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ==================== 盘符表 ====================
static DriveInfo64 g_entries[DRV64_MAX_ENTRIES];
static int         g_count = 0;
static bool        g_scanned = false;
// 系统盘判定要用到的现场（扫描时记下来，再决定 C:）
static int         g_entry_main[32];      // 每块盘第一个 type=0x07 项的下标（-1 = 没有）
static int         g_entry_main_count = 0;

static uint8_t g_sec[512];                // 唯一工作扇区（MBR / 分区首扇区 / FAT BPB）

static void log_line(const char* s) {
    dbg64_str("[DRV64] ");
    dbg64_str(s);
    dbg64_nl();
}
static void log_skip(uint32_t lba, const char* type, const char* reason) {
    dbg64_str("[DRV64] skip lba=");
    dbg64_dec(lba);
    dbg64_str(" type=");
    dbg64_str(type);
    dbg64_str(" reason=");
    dbg64_str(reason);
    dbg64_nl();
}
static void log_letter(const DriveInfo64& e) {
    dbg64_str("[DRV64] letter=");
    char l[3];
    l[0] = e.letter;
    l[1] = ':';
    l[2] = 0;
    dbg64_str(l);
    dbg64_str(" disk=");
    dbg64_dec((uint64_t)e.disk);
    dbg64_str(" part=");
    dbg64_dec((uint64_t)e.part);
    dbg64_str(" fs=");
    dbg64_str(e.fs);
    dbg64_str(" total_kb=");
    dbg64_dec(e.total_kb);
    dbg64_str(" free_kb=");
    dbg64_dec(e.free_kb);
    dbg64_str(" slot=");                       // ★ 多卷：这个盘符落在哪个 vfs64 卷槽
    if (e.slot != DRV64_SLOT_NONE) dbg64_dec((uint64_t)e.slot); else dbg64_str("-");
    dbg64_nl();
}


// ==================== 文件系统指纹 ====================
// VimtuFS2：magic 命中 + 过 vfs64 的超级块校验（CRC32 + 几何重算）+ 数一遍空闲块。
// 返回 true = 可浏览的卷，*info 已填好几何。卷坏了（magic 对但校验不过）也算"认出来了"，
// 由 *present_magic 告诉调用方，好让打点写 type=unknown reason=no-fs（而不是"未格式化"）。
static bool fs_vimtu_probe(int disk, uint32_t start_lba, bool* present_magic, Vfs64VolInfo64* info) {
    *present_magic = false;
    const uint32_t lba = start_lba;                     // 分区首扇区 = 超级块所在块
    if (!ata64_read(disk, lba, 1, g_sec)) return false;
    if (!(g_sec[0] == 'V' && g_sec[1] == 'I' && g_sec[2] == 'M' && g_sec[3] == 'T' &&
          g_sec[4] == 'U' && g_sec[5] == 'F' && g_sec[6] == 'S' && g_sec[7] == '2')) return false;
    *present_magic = true;
    return (vfs64_probe_volume64(disk, start_lba, info) == 0);
}
// FAT（12/16/32）：合法 BPB 指纹。*out_kind = DRV64_FS_FAT*/UNKNOWN，*out_total_kb = BPB 里的总扇区数/2。
static uint8_t fs_fat_probe(int disk, uint32_t start_lba, uint64_t* out_total_kb) {
    *out_total_kb = 0;
    if (!ata64_read(disk, start_lba, 1, g_sec)) return DRV64_FS_UNKNOWN;
    if (g_sec[510] != 0x55 || g_sec[511] != 0xAA) return DRV64_FS_UNKNOWN;
    const uint8_t jmp = g_sec[0];
    if (!(jmp == 0xEB || jmp == 0xE9)) return DRV64_FS_UNKNOWN;
    uint32_t total = rd16(g_sec + 19);                  // BPB：小卷的总扇区数（16 位）
    const uint32_t total32 = rd32(g_sec + 32);          // BPB：总扇区数（32 位）
    if (total == 0) total = total32;
    const uint8_t spc = g_sec[13];                      // 每簇扇区数（必须 2 的幂且非 0）
    if (spc == 0 || (spc & (spc - 1)) != 0) return DRV64_FS_UNKNOWN;
    if (out_total_kb && total) *out_total_kb = (uint64_t)total / 2u;
    // FAT32 的指纹：偏移 82 = "FAT32   "（FAT12/16 用偏移 54 的 "FAT"）
    static const char f32[8] = { 'F','A','T','3','2',' ',' ',' ' };
    bool is32 = true;
    for (uint32_t i = 0; i < 8; i++) if (g_sec[82 + i] != (uint8_t)f32[i]) { is32 = false; break; }
    if (is32) return DRV64_FS_FAT32;
    if (g_sec[54] == 'F' && g_sec[55] == 'A' && g_sec[56] == 'T') return DRV64_FS_FAT12_16;
    return DRV64_FS_UNKNOWN;
}

// ==================== 扫描 ====================
static void add_entry(const DriveInfo64& e, int* idx_out) {
    if (g_count >= (int)DRV64_MAX_ENTRIES) {
        dbg64_str("[DRV64] table full, entry dropped disk=");
        dbg64_dec((uint64_t)e.disk);
        dbg64_str(" part=");
        dbg64_dec((uint64_t)e.part);
        dbg64_nl();
        if (idx_out) *idx_out = -1;
        return;
    }
    g_entries[g_count] = e;
    if (idx_out) *idx_out = g_count;
    g_count++;
}

int drive64_scan64() {
    g_count = 0;
    g_entry_main_count = 0;
    g_scanned = true;
    zero_bytes(g_entries, (uint32_t)sizeof(g_entries));

    int disks = 0;                 // 真磁盘数（不含 ATAPI）
    int parts = 0;                 // 分区项总数
    int fs_recognized = 0;         // 认出来的文件系统数（VimtuFS2 + FAT）
    int sys_letter_idx = -1;       // 将来拿 C: 的条目
    int main_idx = -1;             // 本盘第一个 type=0x07 项的下标（系统盘判定的兜底依据）

    const int slots = ata64_drive_count64();
    for (int s = 0; s < slots; s++) {
        const int d = ata64_slot_to_drive64(s);
        if (d < 0) continue;
        DiskInfo di;
        if (!ata64_identify(d, &di) || !di.present) continue;      // 不存在的槽跳过
        if (di.atapi) continue;                                    // 光驱不是"磁盘"
        disks++;

        // ---- 读 MBR（签名不对 / 没有分区项 -> 一条"整盘未识别"记录）----
        PartInfo p[4];
        bool mbr_ok = false;
        for (int i = 0; i < 4; i++) {
            p[i].used = false; p[i].type = 0; p[i].bootable = false; p[i].start = 0; p[i].sectors = 0;
        }
        if (ata64_read(d, 0, 1, g_sec)) {
            if (g_sec[510] == 0x55 && g_sec[511] == 0xAA) {
                mbr_ok = true;
                for (int i = 0; i < 4; i++) {
                    const uint8_t* e = g_sec + 446 + i * 16;
                    p[i].used = (e[4] != 0);
                    p[i].type = e[4];
                    p[i].bootable = (e[0] == 0x80);
                    p[i].start = rd32(e + 8);
                    p[i].sectors = rd32(e + 12);
                }
            }
        }
        int used_entries = 0;
        for (int i = 0; i < 4; i++) if (mbr_ok && p[i].used && p[i].sectors > 0) used_entries++;

        if (used_entries == 0) {
            // 未分区 / 不是有效 MBR：仍列出（不占盘符），打点 reason=no-fs
            DriveInfo64 e;
            zero_bytes(&e, (uint32_t)sizeof(e));
            e.present = true;
            e.letter = 0;
            copy_str(e.name, DRV64_NAME_MAX, "未识别磁盘(无分区表)");
            copy_str(e.fs, DRV64_FS_MAX, "unknown");
            e.disk = d;
            e.part = 0;
            e.start_lba = 0;
            e.sectors = (di.sectors > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)di.sectors;
            e.fskind = DRV64_FS_UNKNOWN;
            e.skip = DRV64_SKIP_NOFS;
            add_entry(e, nullptr);
            log_skip(0, "unknown", "no-fs");
            continue;
        }

        // ---- 每个分区：识别文件系统 ----
        for (int i = 0; i < 4; i++) {
            if (!p[i].used || p[i].sectors == 0) continue;
            parts++;
            DriveInfo64 e;
            zero_bytes(&e, (uint32_t)sizeof(e));
            e.slot = DRV64_SLOT_NONE;                          // ★ 多卷：默认没占槽（0 是合法槽号，必须显式置 NONE）
            e.present = true;
            e.disk = d;
            e.part = i + 1;
            e.start_lba = p[i].start;
            e.sectors = p[i].sectors;
            copy_str(e.fs, DRV64_FS_MAX, "unknown");

            bool magic = false;
            Vfs64VolInfo64 vi;
            const bool vimtu_ok = fs_vimtu_probe(d, p[i].start, &magic, &vi);
            uint64_t fat_total_kb = 0;
            const uint8_t fat_kind = vimtu_ok ? DRV64_FS_UNKNOWN : fs_fat_probe(d, p[i].start, &fat_total_kb);

            if (vimtu_ok) {
                fs_recognized++;
                e.fskind = DRV64_FS_VIMTUFS2;
                e.vol_version = vi.version;
                copy_str(e.fs, DRV64_FS_MAX, "VimtuFS2");
                copy_str(e.name, DRV64_NAME_MAX, "本地磁盘");
                e.browsable = true;
                e.total_known = true;
                e.free_known = true;
                e.total_kb = (uint64_t)vi.blocks / 2u;              // 512B/块 -> KB
                e.free_kb = (uint64_t)vi.free_blocks / 2u;
                e.skip = DRV64_SKIP_NONE;
            } else if (p[i].type == 0xEF) {
                // EFI 系统分区（或 P1 那个 0xEF 引导区）：不浏览、不分配盘符
                e.fskind = (fat_kind == DRV64_FS_FAT32) ? DRV64_FS_FAT32 :
                           (fat_kind == DRV64_FS_FAT12_16) ? DRV64_FS_FAT12_16 : DRV64_FS_UNKNOWN;
                if (e.fskind == DRV64_FS_FAT32) { fs_recognized++; copy_str(e.fs, DRV64_FS_MAX, "FAT32"); }
                else if (e.fskind == DRV64_FS_FAT12_16) { fs_recognized++; copy_str(e.fs, DRV64_FS_MAX, "FAT16"); }
                copy_str(e.name, DRV64_NAME_MAX, "EFI 系统分区");
                e.total_known = (fat_total_kb != 0);
                e.total_kb = fat_total_kb;
                e.free_known = false;                               // FAT 的可用空间要读 FSInfo/FAT：没做
                e.browsable = false;
                e.skip = DRV64_SKIP_ESP;
            } else if (fat_kind != DRV64_FS_UNKNOWN) {
                fs_recognized++;
                e.fskind = fat_kind;
                copy_str(e.fs, DRV64_FS_MAX, (fat_kind == DRV64_FS_FAT32) ? "FAT32" : "FAT16");
                copy_str(e.name, DRV64_NAME_MAX, "FAT 卷（只读识别，未实现浏览）");
                e.total_known = (fat_total_kb != 0);
                e.total_kb = fat_total_kb;
                e.free_known = false;
                e.browsable = false;
                e.skip = DRV64_SKIP_NOFS;
            } else {
                copy_str(e.fs, DRV64_FS_MAX, magic ? "VimtuFS2(坏卷)" : "unknown");
                copy_str(e.name, DRV64_NAME_MAX, magic ? "VimtuFS2 卷（超级块校验未通过）" : "未识别分区");
                e.browsable = false;
                e.skip = DRV64_SKIP_NOFS;
            }
            int idx = -1;
            add_entry(e, &idx);
            // 记录该盘第一个 0x07 主分区（系统盘判定的兜底依据）
            if (idx >= 0 && p[i].type == 0x07 && main_idx < 0) main_idx = idx;
        }
        if (main_idx >= 0 && g_entry_main_count < 32) g_entry_main[g_entry_main_count++] = main_idx;
    }

    // ---- ★ 多卷：给每个可浏览卷分配/复用一个 vfs64 卷槽（只读挂载：不改盘上内容）----
    // 幂等依据：先按 (drive, start_lba) 查"是不是已经挂过" —— 系统卷在 os_boot_path 里已经
    // 挂进 0 号槽（vfs64_mount_system64），重扫时直接用回同一个槽，不会把槽表撑爆。
    // 槽不够 / 挂载失败：**如实降级**成不可浏览（reason=voltable-full / mount-failed），
    // 绝不偷偷覆盖别的已挂载卷 —— 那会污染别的数据盘。
    for (int i = 0; i < g_count; i++) {
        DriveInfo64& e = g_entries[i];
        if (!e.browsable) continue;
        int slot = vfs64_slot_find64(e.disk, e.start_lba);
        if (slot < 0) {
            slot = vfs64_slot_alloc64();
            if (slot < 0) {
                e.browsable = false;
                e.skip = DRV64_SKIP_NOSLOT;
                log_skip(e.start_lba, "VimtuFS2", "voltable-full");
                continue;
            }
            if (vfs64_mount_slot64(slot, e.disk, e.start_lba) != 0) {
                e.browsable = false;
                e.skip = DRV64_SKIP_NOFS;
                log_skip(e.start_lba, "VimtuFS2", "mount-failed");
                continue;
            }
        }
        e.slot = (uint8_t)slot;
    }

    // ---- 盘符分配：C: = 系统卷，其余可浏览卷 D:、E:… ----
    // 1) 首选"真正挂载的那个卷"（运行中的系统就在它上面）
    int md = -1;
    uint32_t ml = 0;
    if (vfs64_mounted_volume64(&md, &ml, nullptr) == 0) {
        for (int i = 0; i < g_count; i++) {
            if (!g_entries[i].browsable) continue;
            if (g_entries[i].disk != md || g_entries[i].start_lba != ml) continue;
            sys_letter_idx = i;
            break;
        }
    } else {
        log_line("skipped (VimtuFS2 not mounted) - C: falls back to MBR type=0x07");
    }
    // 2) 兜底：drive 0 的 MBR 里第一个 0x07 项（= app64_main_part_lba64 的规则），
    //    再兜底 3) 枚举顺序里第一个可浏览的 VimtuFS2 卷
    if (sys_letter_idx < 0) {
        for (int k = 0; k < g_entry_main_count; k++) {
            const int i = g_entry_main[k];
            if (i >= 0 && i < g_count && g_entries[i].browsable && g_entries[i].disk == 0) { sys_letter_idx = i; break; }
        }
    }
    if (sys_letter_idx < 0) {
        for (int i = 0; i < g_count; i++) if (g_entries[i].browsable) { sys_letter_idx = i; break; }
    }
    int letter = (int)'C';
    if (sys_letter_idx >= 0) {
        g_entries[sys_letter_idx].letter = (char)letter;
        g_entries[sys_letter_idx].system = true;
        letter++;
    } else {
        log_line("no browsable volume - no drive letter assigned");
    }
    for (int i = 0; i < g_count && letter <= (int)'Z'; i++) {
        if (!g_entries[i].browsable) continue;
        if (g_entries[i].letter != 0) continue;                 // C: 已经给过了
        g_entries[i].letter = (char)letter;
        letter++;
    }

    // ---- 打点：scan / letter / skip ----
    dbg64_str("[DRV64] scan disks=");
    dbg64_dec((uint64_t)disks);
    dbg64_str(" parts=");
    dbg64_dec((uint64_t)parts);
    dbg64_str(" fs=");
    dbg64_dec((uint64_t)fs_recognized);
    dbg64_nl();
    for (int i = 0; i < g_count; i++) {
        if (g_entries[i].letter != 0) log_letter(g_entries[i]);
    }
    for (int i = 0; i < g_count; i++) {
        if (g_entries[i].letter != 0) continue;
        const bool ty_ef = (g_entries[i].skip == DRV64_SKIP_ESP);
        const char* ty = ty_ef ? "0xEF" : (g_entries[i].fskind == DRV64_FS_VIMTUFS2 ? "VimtuFS2" : "unknown");
        log_skip(g_entries[i].start_lba, ty, drive64_skip_reason64(g_entries[i].skip));
    }
    return g_count;
}

int drive64_count64() { return g_count; }

int drive64_info64(int i, DriveInfo64* out) {
    if (!out) { log_line("info64: null out"); return -1; }
    if (i < 0 || i >= g_count || !g_entries[i].present) {
        dbg64_str("[DRV64] info64: index out of range i=");
        dbg64_dec((uint64_t)i);
        dbg64_str(" count=");
        dbg64_dec((uint64_t)g_count);
        dbg64_nl();
        return -1;
    }
    *out = g_entries[i];
    // ★ 多卷：可浏览条目刷新成**实时**容量/可用（走已挂载的卷槽数一遍位图；只读、不改挂载状态）。
    // 为什么在这里做：explorer 的"此电脑"页每张卡片都读一次 info64，写盘（在 D: 上 mkdir/write）之后
    // 立即重绘就能看到可用空间变化；反过来说容量数字永远与卷槽里的真值一致（不是开机快照）。
    if (out->browsable && out->slot != DRV64_SLOT_NONE && out->slot < (uint8_t)VFS64_SLOT_MAX) {
        Vfs64VolInfo64 vi;
        if (vfs64_slot_info64((int)out->slot, nullptr, nullptr, &vi) == 0) {
            out->total_kb = (uint64_t)vi.blocks / 2u;
            out->free_kb = (uint64_t)vi.free_blocks / 2u;
            out->total_known = true;
            out->free_known = true;
        }
    }
    return 0;
}

// ★ 跳过原因的稳定字符串（打点与 UI 共用）
const char* drive64_skip_reason64(uint8_t skip) {
    if (skip == DRV64_SKIP_ESP) return "esp";
    if (skip == DRV64_SKIP_NOFS) return "no-fs";
    if (skip == DRV64_SKIP_NOSLOT) return "voltable-full";
    return "none";
}

// ★ 激活盘符对应的卷：把 vfs64 的"当前卷"切到这个盘。0 = 成功；-1 = 没这个盘符/不可浏览/没占槽。
// 打点：[DRV64] activate letter=D: slot=1 disk=1 lba=8192 ok（失败 reason=<no-letter|not-browsable|no-slot|vfs64>）
int drive64_activate_letter64(char letter) {
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');
    const int i = drive64_by_letter64(letter);
    char lb[3];
    lb[0] = letter ? letter : '?';
    lb[1] = ':';
    lb[2] = 0;
    if (i < 0) {
        dbg64_str("[DRV64] activate letter=");
        dbg64_str(lb);
        dbg64_str(" FAILED reason=no-letter");
        dbg64_nl();
        return -1;
    }
    const DriveInfo64& e = g_entries[i];
    // ★ 打点顺序有讲究：**先**调 vfs64_activate_slot64（它会打自己的 [VFS64] activate 行），
    //   再打这一条完整的 [DRV64] activate 行 —— 否则两行会在串口上交错成
    //   "[DRV64] activate letter=D: slot=1[VFS64] activate ... disk=1 lba=8192 ok"，
    //   按行 grep 就匹配不到（实测踩过的坑）。
    if (!e.browsable) {
        dbg64_str("[DRV64] activate letter=");
        dbg64_str(lb);
        dbg64_str(" slot=");
        if (e.slot != DRV64_SLOT_NONE) dbg64_dec((uint64_t)e.slot); else dbg64_str("-");
        dbg64_str(" FAILED reason=not-browsable skip=");
        dbg64_str(drive64_skip_reason64(e.skip));
        dbg64_nl();
        return -1;
    }
    if (e.slot == DRV64_SLOT_NONE || e.slot >= (uint8_t)VFS64_SLOT_MAX) {
        dbg64_str("[DRV64] activate letter=");
        dbg64_str(lb);
        dbg64_str(" FAILED reason=no-slot");
        dbg64_nl();
        return -1;
    }
    const int vrc = vfs64_activate_slot64((int)e.slot);
    dbg64_str("[DRV64] activate letter=");
    dbg64_str(lb);
    dbg64_str(" slot=");
    dbg64_dec((uint64_t)e.slot);
    if (vrc != 0) {
        dbg64_str(" FAILED reason=vfs64");
        dbg64_nl();
        return -1;
    }
    dbg64_str(" disk=");
    dbg64_dec((uint64_t)e.disk);
    dbg64_str(" lba=");
    dbg64_dec(e.start_lba);
    dbg64_str(" ok");
    dbg64_nl();
    return 0;
}

// ★ 当前活动盘符 = vfs64 当前卷对应的字母；没有可浏览卷时 0
char drive64_current_letter64() {
    const int slot = vfs64_current_slot64();
    if (slot < 0) return 0;
    for (int i = 0; i < g_count; i++) {
        if (!g_entries[i].browsable || g_entries[i].letter == 0) continue;
        if ((int)g_entries[i].slot == slot) return g_entries[i].letter;
    }
    return 0;
}

int drive64_by_letter64(char letter) {
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');
    for (int i = 0; i < g_count; i++) {
        if (g_entries[i].present && g_entries[i].letter == letter) return i;
    }
    return -1;
}

// ==================== 自检 ====================
int drive64_selftest64() {
    int fails = 0;
    if (!g_scanned) {
        log_line("selftest skipped (not scanned)");
        dbg64_str("[DRV64] selftest PASS mask=");
        dbg64_dec(0);
        dbg64_nl();
        return 0;
    }
    if (g_count == 0) {
        log_line("selftest skipped (no disk)");
        dbg64_str("[DRV64] selftest PASS mask=");
        dbg64_dec(0);
        dbg64_nl();
        return 0;
    }

    // bit0：C: 存在且可浏览（一个可浏览卷都没有时如实跳过）
    int browsable = 0;
    for (int i = 0; i < g_count; i++) if (g_entries[i].browsable) browsable++;
    const int ci = drive64_by_letter64('C');
    if (browsable > 0) {
        if (ci < 0 || !g_entries[ci].browsable || !g_entries[ci].system) fails |= 1;
    } else {
        log_line("selftest: no browsable volume (bit0 skipped)");
        if (ci >= 0) fails |= 1;
    }

    // bit1：可浏览条目的容量自洽
    for (int i = 0; i < g_count; i++) {
        const DriveInfo64& e = g_entries[i];
        if (!e.browsable) continue;
        if (!e.total_known || !e.free_known) { fails |= 2; continue; }
        if (e.total_kb == 0 || e.free_kb > e.total_kb) fails |= 2;
    }

    // bit2：盘符唯一 + 从 C 起连续；被跳过的条目没有盘符
    int seen[26] = { 0 };
    int expect = (int)'C';
    for (int i = 0; i < g_count; i++) {
        const DriveInfo64& e = g_entries[i];
        if (e.letter == 0) {
            if (e.browsable) fails |= 4;                        // 可浏览却没有盘符 = 错
            continue;
        }
        if (e.letter < 'C' || e.letter > 'Z') { fails |= 4; continue; }
        if (seen[e.letter - 'C']) { fails |= 4; continue; }
        seen[e.letter - 'C'] = 1;
        if (!e.browsable) fails |= 4;
    }
    for (int c = 0; c < 26; c++) {
        if (!seen[c]) continue;
        if (c + (int)'C' != expect) { fails |= 4; break; }
        expect++;
    }

    // bit3：by_letter64 与表一致
    for (int i = 0; i < g_count; i++) {
        const DriveInfo64& e = g_entries[i];
        if (e.letter == 0) continue;
        const int k = drive64_by_letter64(e.letter);
        if (k < 0 || k >= g_count) { fails |= 8; continue; }
        const DriveInfo64& f = g_entries[k];
        if (f.disk != e.disk || f.part != e.part || f.start_lba != e.start_lba || f.letter != e.letter) fails |= 8;
    }
    if (drive64_by_letter64('A') >= 0 || drive64_by_letter64('B') >= 0) fails |= 8;   // A:/B: 从不分配

    // bit4：幂等（再扫一遍，条目数与 C: 的关键字段不变）
    int c_disk = -1, c_part = -1;
    uint32_t c_start = 0;
    uint64_t c_total = 0;
    char c_letter = 0;
    if (ci >= 0) {
        c_disk = g_entries[ci].disk; c_part = g_entries[ci].part;
        c_start = g_entries[ci].start_lba; c_total = g_entries[ci].total_kb;
        c_letter = g_entries[ci].letter;
    }
    const int n_before = g_count;
    (void)drive64_scan64();
    if (g_count != n_before) fails |= 16;
    if (ci >= 0) {
        const int ci2 = drive64_by_letter64('C');
        if (ci2 < 0) fails |= 16;
        else if (g_entries[ci2].disk != c_disk || g_entries[ci2].part != c_part ||
                 g_entries[ci2].start_lba != c_start || g_entries[ci2].total_kb != c_total ||
                 g_entries[ci2].letter != c_letter) fails |= 16;
    }

    // bit5：skip 条目自洽（没盘符、有原因、起始 LBA 落在盘内）
    for (int i = 0; i < g_count; i++) {
        const DriveInfo64& e = g_entries[i];
        if (e.skip == DRV64_SKIP_NONE) {
            if (!e.browsable && e.letter != 0) fails |= 32;      // 没跳过又不可浏览还占字母 = 错
            continue;
        }
        if (e.letter != 0) fails |= 32;
        if (e.browsable) fails |= 32;
        if (e.part > 0 && e.sectors > 0 && e.start_lba > 0x7FFFFFFFu) fails |= 32;
    }

    // bit6（64）：★ 多卷槽一致性 —— 每个可浏览条目都占一个有效槽，且该槽挂的卷就是条目的
    //   (disk, start_lba)；activate_letter64 与 current_letter64 对得上（有可浏览卷时）。
    //   注意 bit4 已经重扫过一次，这里检查的是**重扫后**的表（也顺带证明槽复用幂等）。
    for (int i = 0; i < g_count; i++) {
        const DriveInfo64& e = g_entries[i];
        if (!e.browsable) continue;
        if (e.slot == DRV64_SLOT_NONE || e.slot >= (uint8_t)VFS64_SLOT_MAX) { fails |= 64; continue; }
        int sd = -1;
        uint32_t sl = 0;
        if (vfs64_slot_info64((int)e.slot, &sd, &sl, nullptr) != 0) { fails |= 64; continue; }
        if (sd != e.disk || sl != e.start_lba) fails |= 64;
    }
    if (browsable > 0) {
        if (drive64_activate_letter64('C') != 0) fails |= 64;              // C: = 系统卷，必须能激活
        else if (drive64_current_letter64() != 'C') fails |= 64;
        // 有第二个可浏览卷（D:）时双向切一次（激活 -> 校验 -> 切回）
        const int di = drive64_by_letter64('D');
        if (di >= 0 && g_entries[di].browsable) {
            if (drive64_activate_letter64('D') != 0) fails |= 64;
            else if (drive64_current_letter64() != 'D') fails |= 64;
            if (drive64_activate_letter64('C') != 0) fails |= 64;
            else if (drive64_current_letter64() != 'C') fails |= 64;
        }
        // 不存在的盘符 / 不可浏览条目：必须 -1 且不崩（也不改当前盘）
        if (drive64_activate_letter64('Z') != -1) fails |= 64;
        if (drive64_current_letter64() != 'C') fails |= 64;
    }

    dbg64_str("[DRV64] selftest ");
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

void drive64_dump64() {
    dbg64_str("[DRV64] dump entries=");
    dbg64_dec((uint64_t)g_count);
    dbg64_str(g_scanned ? " scanned=yes" : " scanned=no");
    dbg64_nl();
    for (int i = 0; i < g_count; i++) {
        const DriveInfo64& e = g_entries[i];
        dbg64_str("[DRV64] dump  [");
        dbg64_dec((uint64_t)i);
        dbg64_str("] ");
        char l[3];
        if (e.letter) { l[0] = e.letter; l[1] = ':'; l[2] = 0; dbg64_str(l); } else dbg64_str("--");
        dbg64_str(" ");
        dbg64_str(e.name);
        dbg64_str(" fs=");
        dbg64_str(e.fs);
        dbg64_str(" disk=");
        dbg64_dec((uint64_t)e.disk);
        dbg64_str(" part=");
        dbg64_dec((uint64_t)e.part);
        dbg64_str(" lba=");
        dbg64_dec(e.start_lba);
        dbg64_str("+");
        dbg64_dec(e.sectors);
        dbg64_str(" total_kb=");
        if (e.total_known) dbg64_dec(e.total_kb); else dbg64_str("unknown");
        dbg64_str(" free_kb=");
        if (e.free_known) dbg64_dec(e.free_kb); else dbg64_str("unknown");
        dbg64_str(" vol_v=");
        if (e.vol_version) dbg64_dec(e.vol_version); else dbg64_str("-");
        dbg64_str(" slot=");
        if (e.slot != DRV64_SLOT_NONE) dbg64_dec((uint64_t)e.slot); else dbg64_str("-");
        dbg64_str(" browsable=");
        dbg64_str(e.browsable ? "yes" : "no");
        dbg64_str(" system=");
        dbg64_str(e.system ? "yes" : "no");
        dbg64_str(" skip=");
        dbg64_str(drive64_skip_reason64(e.skip));
        dbg64_nl();
    }
}

const char* drive64_kind_name64(uint32_t vfs_kind) {
    if (vfs_kind == VFS64_KIND_DIR)  return "dir";
    if (vfs_kind == VFS64_KIND_FILE) return "file";
    if (vfs_kind == VFS64_KIND_VAP)  return "vap";
    if (vfs_kind == VFS64_KIND_ELF)  return "elf";
    if (vfs_kind == VFS64_KIND_TEXT) return "text";
    if (vfs_kind == VFS64_KIND_BIN)  return "bin";
    return "unknown";
}
