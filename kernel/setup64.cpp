// setup64.cpp - Vimtu64 图形化安装程序（Win10 安装程序同款界面与流程）
//
// 设计说明
// --------
// * 界面严格模仿 Windows 10 安装程序：深蓝底、白字、左上角大标题、右下角"下一步"，
//   只是把产品名换成 VimtuOS，并按需求**去掉了"输入产品密钥"那一步**。
// * 这一版把"界面 + 流程 + 磁盘/分区读取"做成真实可用；分区的新建/删除/格式化与
//   系统文件写入（真正的安装动作）在下一批实现，接口已经留在
//   `part_create_auto()` / `part_delete()` / `part_format()` / `install_begin()` 里。
// * 全部绘制走 fb_*（后备缓冲）+ font_*（TrueType），每帧整屏重绘后 fb_flip()。
//   1280x800 下一帧约 4MB 拷贝，QEMU TCG 里也够流畅（安装程序不需要高帧率）。
#include "setup64.h"
#include "fb.h"
#include "font.h"
#include "input.h"
#include "ata64.h"
#include "ahci64.h"      // ★ item 5a：AHCI64_MAX_DEVS（磁盘表按统一驱动器号扩张到 8+N）
#include "hwui64.h"      // ★ item 5a：找不到任何磁盘时，把屏幕硬件检查报告直接画在向导里
#include "part64.h"
#include "x86_64.h"
#include "fat64.h"      // ★ ESP：安装时在目标盘写 FAT32 卷（自检也在安装程序启动时跑一次）
#include "debug64.h"
#include "vfs64.h"      // 安装收尾：把 /etc/user.conf 写进**目标盘的卷**（挂临时卷槽再写，不动分区逻辑）
#include "userconf64.h"  // /etc/user.conf 的文本生成 + 口令加盐哈希（与登录侧共用同一份实现）
// ==================== 调色板（对齐 Win10 安装程序）====================
static const uint32_t C_BG      = 0xFF00549E;   // 深蓝背景 rgb(0,84,158)
static const uint32_t C_BAR     = 0xFF003E78;   // 顶部标题条 rgb(0,62,120)
static const uint32_t C_LIST    = 0xFF00305E;   // 列表底
static const uint32_t C_SEL     = 0xFF0078D7;   // 选中行（Win10 蓝）
static const uint32_t C_TEXT    = 0xFFFFFFFF;
static const uint32_t C_DIM     = 0xFFBFD4E8;
static const uint32_t C_BTN     = 0xFF0078D7;   // 主按钮
static const uint32_t C_BTN_HI  = 0xFF1E90FF;   // 主按钮悬停
static const uint32_t C_BTN_DIS = 0xFF4A6B8A;   // 主按钮禁用
static const uint32_t C_WHITE   = 0xFFFFFFFF;
static const uint32_t C_WARN    = 0xFFFFD166;   // ★ v0.5.14：确认提示/操作结果（琥珀色）

// ==================== 小工具 ====================
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

static void ui_text(int x, int y, const char* s, uint32_t color) {
    font_select(2);                       // face 2 = simhei（中文）
    font_draw_text(x, y, s, color);
}

// 大号文字：先用 TTF 画到屏幕底部的暂存行，再按 scale 放大搬到目标位置。
// （TTF 只有一档字号；放大靠像素复制，这样标题能像 Win10 那样醒目。）
static void ui_text_scaled(int x, int y, const char* s, uint32_t color, uint32_t bg, int scale) {
    font_select(2);
    const int tw = font_text_width(s) + 2;
    const int th = font_line_height() + 2;
    if (tw <= 0 || th <= 0 || tw > fb_width() || th > fb_height()) { ui_text(x, y, s, color); return; }
    const int sx = 0, sy = fb_height() - th;          // 暂存行（每帧会被整屏重绘覆盖）
    fb_fill_rect(sx, sy, tw, th, bg);
    font_draw_text(sx, sy, s, color);
    for (int yy = 0; yy < th; yy++) {
        for (int xx = 0; xx < tw; xx++) {
            const uint32_t c = fb_get_pixel(sx + xx, sy + yy);
            if (c == bg) continue;                    // 只搬非背景像素，放大后更干净
            fb_fill_rect(x + xx * scale, y + yy * scale, scale, scale, c);
        }
    }
    fb_fill_rect(sx, sy, tw, th, bg);                 // 清掉暂存
}

struct UiButton {
    int x, y, w, h;
    const char* label;
    int id;
    bool primary;
    bool enabled;
    bool hover;
};

static void ui_button(UiButton& b, int mx, int my) {
    b.hover = (mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h);
    uint32_t fill = b.primary ? (b.enabled ? (b.hover ? C_BTN_HI : C_BTN) : C_BTN_DIS) : 0;
    if (b.primary) {
        fb_fill_rect(b.x, b.y, b.w, b.h, fill);
    } else {
        // 次要按钮：Win10 里是"描边 + 深色底"
        fb_fill_rect(b.x, b.y, b.w, b.h, b.hover ? 0xFF0A4C86 : 0x00000000);
        fb_draw_rect(b.x, b.y, b.w, b.h, b.hover ? C_WHITE : C_DIM);
    }
    font_select(2);
    const int tw = font_text_width(b.label);
    const int th = font_line_height();
    const int tx = b.x + (b.w - tw) / 2;
    const int ty = b.y + (b.h - th) / 2;
    ui_text(tx, ty, b.label, b.enabled ? C_WHITE : C_DIM);
}

static bool ui_button_hit(const UiButton& b, int mx, int my) {
    return b.enabled && mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h;
}

// 鼠标光标（Win10 风格的白色箭头，简化为几段矩形）
static void draw_cursor(int x, int y) {
    for (int i = 0; i < 14; i++) fb_fill_rect(x, y + i, 2, 1, C_WHITE);
    for (int i = 0; i < 9; i++)  fb_fill_rect(x + i, y + i, 2, 2, C_WHITE);
    fb_fill_rect(x, y, 2, 14, 0xFF000000);
    fb_fill_rect(x + 1, y + 1, 1, 12, C_WHITE);
}

// ==================== 磁盘与分区模型 ====================
struct PartEntry {
    bool     used;
    uint8_t  type;          // MBR 类型码（GPT 分区用 0 表示"GPT 项"）
    uint32_t start;         // 起始 LBA
    uint32_t sectors;       // 扇区数
    char     label[40];     // 显示名（EFI 系统分区 / Microsoft 保留 / 基本数据分区 …）
    bool     is_gpt;
    // ★ v0.5.14：删除/格式化必须知道"这一行来自哪张表、是什么类型" ——
    //   否则在 GPT 盘上会按 MBR 项号去写盘（实测会把 LBA1 的主 GPT 头清零）。
    int      src;           // PART_SRC_MBR / PART_SRC_GPT
    bool     is_esp;        // EFI 系统分区（GPT 类型 GUID，或盘尾那个 0xEF 项）
    bool     is_basic;      // 主数据分区（GPT Microsoft Basic Data / MBR 0x07）
};

struct DiskEntry {
    bool     present;
    bool     atapi;
    char     model[41];
    uint32_t sectors;
    bool     has_mbr;
    bool     has_gpt;
    int      part_count;
    PartEntry parts[8];
    bool     probed;
};

struct ListRow {
    int  drive;             // -1 = 不是磁盘相关行
    int  part;              // -1 = 磁盘标题行 / 未分配行
    bool selectable;        // 只有分区行可选（用于"安装系统"）
    char text[96];
};

// ★ item 5a / item 6：磁盘表按**统一驱动器号**索引（0..3 = PATA，8..15 = AHCI 盘，
//   16.. = NVMe 命名空间，4..7 是保留空洞），所以数组要覆盖到最大驱动器号
//   ATA64_MAX_DRIVE64 - 1（= ATA64_NVME_BASE + NVME64_MAX_NS - 1）；下标 = 驱动器号，不重排。
//   ★ item 6 的**最小必要改动**：原来只覆盖到 ATA64_AHCI_BASE + AHCI64_MAX_DEVS - 1（= 15），
//     而 NVMe 命名空间的驱动器号从 16 起 —— 数组不够就会被下面 `d >= kDiskSlots` 静默跳过
//     （表现：向导里看不到 NVMe 盘 = 装不进去）。编号方案本身没变，只是数组跟着编号方案变大。
static const int kDiskSlots = ATA64_MAX_DRIVE64;
static DiskEntry g_disks[kDiskSlots];
static ListRow   g_rows[24];
// 安装介质本身占据的那块 ATA 盘（U 盘 / hybrid ISO；-1 = 介质不是块盘或尚未判定）。
// 用途：① 在列表里把它标出来；② 默认选中行与"自动选分区"都跳过它，
//       免得用户/自动化一上来就把安装介质格式化掉。
static int       g_medium_drive = -1;
// ★ 批次 O：可移动盘（USB 存储）的标记只打一次串口（build_rows 会被重复调用）
static bool      g_removable_logged = false;
static int       g_row_count = 0;
static int       g_row_sel = -1;

// 从字节里读小端
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t* p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

// 简洁的字符串拼接（无 libc）
static void str_append(char*& d, const char* s, int max) {
    int n = 0;
    while (*s && n < max) { *d++ = *s++; n++; }
    *d = 0;
}
static void str_append_u64(char*& d, uint64_t v, int max) {
    char tmp[24]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 23) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    int m = 0;
    while (n > 0 && m < max) { *d++ = tmp[--n]; m++; }
    *d = 0;
}
// 人类可读容量：扇区 -> MB / GB
static void str_append_size(char*& d, uint64_t sectors, int max) {
    const uint64_t bytes = sectors * 512ULL;
    if (bytes >= 1000ULL * 1000ULL * 1000ULL) {
        str_append_u64(d, bytes / (1000ULL * 1000ULL * 1000ULL), max);
        str_append(d, " GB", max);
    } else {
        str_append_u64(d, bytes / (1000ULL * 1000ULL), max);
        str_append(d, " MB", max);
    }
}

// ==================== v0.5.14：两步确认 + 底部状态行 ====================
// 一次鼠标误点就把分区抹掉是不能接受的（Win10 安装程序也会弹确认框）。这里的做法
// 与"同一个键按两下"等价：第一次按/点 -> 武装（按钮变成"确认删除"/"确认格式化"，
// 底部给出目标分区号）；再按一次同一个键 / 再点同一个按钮 -> 才真写盘。
// Esc、换选中行、换页、按刷新都会取消武装。
static int  g_pending_act = 0;      // 0 = 没有待确认动作；12 = 删除；13 = 格式化
static int  g_pending_row = -1;     // 武装时选中的行
static char g_status[96];           // 底部状态行：上一步的结果或失败原因

static void status_clear() { g_status[0] = 0; }
static void status_set(const char* a) {
    char* d = g_status;
    *d = 0;
    str_append(d, a, 84);
}
static void status_set_op(const char* verb, int part_no, const char* tail) {
    char* d = g_status;
    *d = 0;
    str_append(d, verb, 40);
    str_append_u64(d, (uint64_t)part_no, 4);
    str_append(d, tail, 40);
}
static void confirm_cancel(const char* why) {
    if (g_pending_act == 0) return;
    g_pending_act = 0;
    g_pending_row = -1;
    dbg64_str("[SETUP] 确认已取消 reason=");
    dbg64_str(why);
    dbg64_nl();
}

// 把 GPT 类型 GUID 归类成可读名字
static void gpt_type_name(const uint8_t* guid, char* out, int cap) {
    // ESP: C12A7328-F81F-11D2-BA4B-00A0C93EC93B
    static const uint8_t esp[16] = {0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B};
    // MSR: E3C9E316-0B5C-4DB8-817D-F92DF00215AE
    static const uint8_t msr[16] = {0x16,0xE3,0xC9,0xE3,0x5C,0x0B,0xB8,0x4D,0x81,0x7D,0xF9,0x2D,0xF0,0x02,0x15,0xAE};
    bool is_esp = true, is_msr = true;
    for (int i = 0; i < 16; i++) {
        if (guid[i] != esp[i]) is_esp = false;
        if (guid[i] != msr[i]) is_msr = false;
    }
    char* d = out;
    if (is_esp)      str_append(d, "EFI 系统分区", cap);
    else if (is_msr) str_append(d, "Microsoft 保留分区", cap);
    else             str_append(d, "基本数据分区", cap);
}

// v0.5.14：GPT 项的类型标志（删除/格式化按类型选动作，不能只看"是不是 0x07"）
static void gpt_type_flags(const uint8_t* guid, bool* is_esp, bool* is_basic) {
    static const uint8_t esp[16]   = {0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B};
    static const uint8_t basic[16] = {0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7};
    bool e2 = true, b2 = true;
    for (int i = 0; i < 16; i++) {
        if (guid[i] != esp[i]) e2 = false;
        if (guid[i] != basic[i]) b2 = false;
    }
    if (is_esp) *is_esp = e2;
    if (is_basic) *is_basic = b2;
}

// 读取一块磁盘的分区表（MBR，或保护性 MBR 指向的 GPT）
static void probe_disk(int idx) {
    DiskEntry& dk = g_disks[idx];
    dk.part_count = 0;
    dk.has_mbr = false;
    dk.has_gpt = false;
    dk.probed = true;

    static uint8_t sec[512];
    if (!ata64_read_sector(idx, 0, sec)) {
        // 注意：这里**不能**把 present 清成 false —— 设备明明在（IDENTIFY 已认出），
        // 只是首扇区读失败（空盘/读超时）。踩过这个坑：一旦清 present，整个驱动器
        // 会从安装程序的列表里消失，看起来像"没接硬盘"。
        dbg64_str("[DISK] drive=");
        dbg64_dec(idx);
        dbg64_str(" sector0 read FAILED reason=");
        dbg64_dec(ata64_dbg_reason);
        dbg64_str(" err=0x");
        dbg64_hex64(ata64_dbg_err);
        dbg64_str(" polls=");
        dbg64_dec(ata64_dbg_polls);
        dbg64_str(" st=[");
        for (int i = 0; i < 8; i++) { dbg64_hex64(ata64_dbg_st[i]); dbg64_str(" "); }
        dbg64_str("]");
        dbg64_nl();
        return;
    }
    if (sec[510] != 0x55 || sec[511] != 0xAA) return;      // 没有分区表

    // MBR 四项
    bool protective = false;
    for (int i = 0; i < 4; i++) {
        const uint8_t* e = sec + 446 + i * 16;
        const uint8_t type = e[4];
        if (type == 0) continue;
        if (type == 0xEE) { protective = true; continue; }
        if (dk.part_count < 8) {
            PartEntry& p = dk.parts[dk.part_count++];
            p.used = true; p.type = type; p.is_gpt = false;
            p.start = rd32(e + 8);
            p.sectors = rd32(e + 12);
            char* dp = p.label;
            static const char* H = "0123456789ABCDEF";
            char hex[3] = {H[(type >> 4) & 0xF], H[type & 0xF], 0};
            if (type == 0xEF && p.start > PART_BOOT_LBA + PART_BOOT_SECS - 1) {
                // ★ 安装器在盘尾建的 ESP：0xEF 项 + 真 FAT32 卷（UEFI 靠 GPT/它找到引导器）
                //   注意：这里按 **MBR 类型码 0xEF** 判断，不看 FAT 类型串 —— 改成 FAT32 不影响显示。
                str_append(dp, "EFI 系统分区 (MBR 类型 0x", 40);
                str_append(dp, hex, 3);
                str_append(dp, ")", 2);
            } else {
                str_append(dp, "主分区 (MBR 类型 0x", 20);
                str_append(dp, hex, 3);
                str_append(dp, ")", 2);
            }
            p.src = PART_SRC_MBR;
            // 盘尾那个 0xEF 是安装器建的 ESP（真 FAT32，UEFI 靠它引导）
            p.is_esp = (type == 0xEF && p.start > PART_BOOT_LBA + PART_BOOT_SECS - 1);
            p.is_basic = (type == 0x07);
        }
        dk.has_mbr = true;
    }

    if (!protective) return;

    // GPT：LBA1 头 + LBA2 起的分区项（每项 128 字节，通常 128 项）
    static uint8_t hdr[512];
    if (!ata64_read_sector(idx, 1, hdr)) return;
    if (!(hdr[0] == 'E' && hdr[1] == 'F' && hdr[2] == 'I' && hdr[3] == ' ')) return;
    dk.has_gpt = true;
    dk.part_count = 0;                                     // GPT 时以 GPT 项为准
    const uint32_t entries_lba = rd32(hdr + 72);
    const uint32_t entry_size  = rd32(hdr + 84);
    if (entry_size != 128) return;
    for (uint32_t s = 0; s < 4 && dk.part_count < 8; s++) {   // 只读前 4 个扇区（16 项）够用
        static uint8_t tbl[512];
        if (!ata64_read_sector(idx, entries_lba + s, tbl)) return;
        for (int i = 0; i < 4; i++) {
            const uint8_t* e = tbl + i * 128;
            bool nonzero = false;
            for (int k = 0; k < 16; k++) if (e[k]) { nonzero = true; break; }
            if (!nonzero) continue;
            if (dk.part_count >= 8) break;
            PartEntry& p = dk.parts[dk.part_count++];
            p.used = true; p.is_gpt = true; p.type = 0;
            const uint64_t first = (uint64_t)rd32(e + 32) | ((uint64_t)rd32(e + 36) << 32);
            const uint64_t last  = (uint64_t)rd32(e + 40) | ((uint64_t)rd32(e + 44) << 32);
            p.start = (uint32_t)first;
            p.sectors = (last >= first) ? (uint32_t)(last - first + 1) : 0;
            gpt_type_name(e, p.label, 36);
            gpt_type_flags(e, &p.is_esp, &p.is_basic);
            p.src = PART_SRC_GPT;
        }
    }
}

// 枚举所有可枚举的磁盘（PATA 0..3 + AHCI 8..）—— **槽位接口**：每个槽给出一个驱动器号，
// 顺序就是"驱动器号从小到大"，界面上的行顺序也随之稳定（8 号盘排在 0..3 之后）。
// 注意：g_disks[] 以**驱动器号**为下标，所以这里要跳过 4..7 的保留空洞。
static void enumerate_disks() {
    const int slots = ata64_drive_count64();
    for (int s = 0; s < slots; s++) {
        const int d = ata64_slot_to_drive64(s);
        if (d < 0 || d >= kDiskSlots) continue;
        DiskInfo di;
        g_disks[d].probed = false;
        if (ata64_identify(d, &di) && di.present) {
            g_disks[d].present = true;
            g_disks[d].atapi = di.atapi;
            g_disks[d].sectors = (uint32_t)di.sectors;
            for (int k = 0; k < 41; k++) g_disks[d].model[k] = di.model[k];
            if (!di.atapi) probe_disk(d);
        } else {
            g_disks[d].present = false;
        }
    }
    // 串口打一份枚举结果，便于自动验收核对
    for (int s = 0; s < slots; s++) {
        const int d = ata64_slot_to_drive64(s);
        if (d < 0 || d >= kDiskSlots) continue;
        if (!g_disks[d].present) continue;
        dbg64_str("[DISK] ");
        dbg64_dec(d);
        dbg64_str(" model=");
        dbg64_str(g_disks[d].model);
        dbg64_str(g_disks[d].atapi ? " (ATAPI)" : "");
        dbg64_str(" sectors=");
        dbg64_dec(g_disks[d].sectors);
        dbg64_str(g_disks[d].has_gpt ? " GPT" : (g_disks[d].has_mbr ? " MBR" : " no-table"));
        dbg64_str(" parts=");
        dbg64_dec(g_disks[d].part_count);
        dbg64_str(d >= ATA64_NVME_BASE ? " bus=NVMe" : (d >= ATA64_AHCI_BASE ? " bus=AHCI" : " bus=PATA"));
        dbg64_nl();
    }
}

// 把磁盘/分区摊平成可滚动的行列表
// ★ item 5a：同样按**槽位**遍历（PATA 0..3 + AHCI 8..），并标出总线类型，便于真机排障。
static void build_rows() {
    g_row_count = 0;
    const int slots = ata64_drive_count64();
    for (int s = 0; s < slots; s++) {
        const int d = ata64_slot_to_drive64(s);
        if (d < 0 || d >= kDiskSlots) continue;
        if (!g_disks[d].present || g_disks[d].atapi) continue;
        if (g_row_count >= 24) break;
        ListRow& r = g_rows[g_row_count++];
        r.drive = d; r.part = -1; r.selectable = false;
        char* p = r.text;
        str_append(p, "驱动器 ", 20);
        str_append_u64(p, (uint64_t)d, 4);
        str_append(p, d >= ATA64_NVME_BASE ? "  NVMe" : (d >= ATA64_AHCI_BASE ? "  AHCI" : "  PATA"), 8);
        str_append(p, "  ", 4);
        str_append(p, g_disks[d].model, 41);
        str_append(p, "  ", 4);
        str_append_size(p, g_disks[d].sectors, 20);
        str_append(p, g_disks[d].has_gpt ? "  GPT" : (g_disks[d].has_mbr ? "  MBR" : "  未初始化"), 16);
        if (d == g_medium_drive) str_append(p, "  <== 安装介质", 24);
        // ★ 批次 O：USB 存储（U 盘）= **可移动** —— 在行里标出来，且默认不选中（见下面两处默认行选择）。
        //   理由（明确写下来）：① U 盘随时可能被拔走/换机器，系统装在上面会变得不可靠；
        //   ② 它常常正是"安装介质本身"（ISO 写进 U 盘）；③ 本批 USB 存储**只读**，根本装不进去。
        //   注意：安装介质内核（-DVIMTU_INSTALLER_MEDIA=1）**不链 usb64.cpp**，所以驱动器号
        //   24.. 在向导里天然不会出现 —— 这里的分支是"换内核/以后接进来"时的兜底，不是死代码。
        if (d >= ATA64_USB_BASE) {
            str_append(p, "  <== 可移动(不建议安装)", 32);
            if (!g_removable_logged) {
                g_removable_logged = true;
                dbg64_str("[SETUP] removable drive marked (USB storage) drive=");
                dbg64_dec((uint64_t)d);
                dbg64_str(" -> 不建议安装目标, 默认不选中\n");
            }
        }
        uint64_t used = 0;
        for (int i = 0; i < g_disks[d].part_count && g_row_count < 24; i++) {
            ListRow& pr = g_rows[g_row_count++];
            pr.drive = d; pr.part = i; pr.selectable = true;
            char* q = pr.text;
            str_append(q, "    分区 ", 20);
            str_append_u64(q, (uint64_t)(i + 1), 4);
            str_append(q, ": ", 4);
            str_append(q, g_disks[d].parts[i].label, 40);
            if (g_disks[d].parts[i].src == PART_SRC_GPT) str_append(q, " [GPT]", 8);
            str_append(q, "   ", 4);
            str_append_size(q, g_disks[d].parts[i].sectors, 20);
            used += g_disks[d].parts[i].sectors;
        }
        if (g_row_count < 24) {
            ListRow& ur = g_rows[g_row_count++];
            ur.drive = d; ur.part = -2; ur.selectable = false;
            char* q = ur.text;
            str_append(q, "    未分配的空间   ", 30);
            const uint64_t free_sectors = (g_disks[d].sectors > used) ? (uint64_t)(g_disks[d].sectors - used) : 0;
            str_append_size(q, free_sectors, 20);
        }
    }
}

// ==================== 向导状态 ====================
enum SetupPage {
    PAGE_LANG = 0,
    PAGE_NOW,
    PAGE_LICENSE,
    PAGE_TYPE,
    PAGE_USER,          // ★ 新增：用户与时区（在"安装类型"之后、"磁盘与分区"之前）
    PAGE_DISK,
    PAGE_PROGRESS,
    PAGE_DONE,
    PAGE_COUNT
};

struct SetupState {
    SetupPage page;
    int  lang;              // 0 = 中文(简体，中国)，1 = English (United States)
    bool license_ok;
    int  install_type;      // 0 = 升级，1 = 自定义
    int  progress;          // 0..100
    int  stage;             // 进度阶段索引
    uint32_t t_start;
    bool dirty;
    // ---- ★ 新增：用户与时区页 ----
    char user_name[USERCONF64_USER_MAX + 1];   // 默认 "vimtu"
    char user_pw[24];                          // 明文只存内存；写盘前算哈希，随后立即清零
    int  user_focus;                           // 0 = 用户名, 1 = 密码, 2 = 时区, 3 = 只走按钮
    int  tz_idx;                               // kTz[] 下标；默认 0 = China / Asia/Shanghai
    int  tz_open;                              // 1 = 下拉框展开
    int  tz_top;                               // 下拉框滚动窗口的首行
    int  uc_written;                           // 1 = /etc/user.conf 已写进目标卷（幂等）
};

static SetupState g_st;
static UiButton  g_btns[8];
static int       g_btn_count = 0;

static void push_button(int x, int y, int w, int h, const char* label, int id, bool primary, bool enabled) {
    if (g_btn_count >= 8) return;
    UiButton& b = g_btns[g_btn_count++];
    b.x = x; b.y = y; b.w = w; b.h = h;
    b.label = label; b.id = id; b.primary = primary; b.enabled = enabled; b.hover = false;
}

// ==================== 各页面绘制 ====================
// 按钮表：各页面先 push_button() 注册，draw_page() 末尾统一按当前鼠标位置绘制
// （踩坑：一开始只注册没画，结果"下一步"按钮在屏幕上根本不存在。）
static int       g_mx = 0, g_my = 0;      // 当前鼠标位置（供按钮画悬停态）

static void page_header(const char* title) {
    fb_fill_rect(0, 0, fb_width(), 110, C_BAR);
    ui_text_scaled(48, 34, title, C_WHITE, C_BAR, 2);
}

static void draw_lang_page() {
    page_header("VimtuOS 安装程序");
    int y = 170;
    ui_text(80, y, "要安装的语言：", C_TEXT);          y += 34;
    ui_text(120, y, g_st.lang == 0 ? "> 中文(简体，中国)" : "  中文(简体，中国)", C_TEXT); y += 30;
    ui_text(120, y, g_st.lang == 1 ? "> English (United States)" : "  English (United States)", C_TEXT); y += 46;
    ui_text(80, y, "时间和货币格式：", C_TEXT);         y += 34;
    ui_text(120, y, g_st.lang == 0 ? "  中文(简体，中国)" : "  English (United States)", C_DIM); y += 46;
    ui_text(80, y, "键盘或输入法：", C_TEXT);           y += 34;
    ui_text(120, y, g_st.lang == 0 ? "  中文(简体) - 美式键盘" : "  US", C_DIM);
    push_button(fb_width() - 240, fb_height() - 92, 180, 56, "下一步", 1, true, true);
}

static void draw_now_page() {
    page_header("VimtuOS 安装程序");
    push_button((fb_width() - 320) / 2, 360, 320, 72, "现在安装", 1, true, true);
    push_button(56, fb_height() - 92, 240, 56, "修复计算机", 2, false, true);
}

static void draw_license_page() {
    page_header("适用的通知与许可条款");
    ui_text(80, 160, "请阅读许可条款。若接受，请勾选下方的选项。", C_TEXT);
    fb_draw_rect(80, 210, fb_width() - 160, 300, C_DIM);
    ui_text(110, 240, "VimtuOS 以 GNU GPL-3.0 许可发布（全文见仓库 LICENSE）；", C_DIM);
    ui_text(110, 274, "安装即表示你接受该许可条款。本项目仅供学习与测试使用，", C_DIM);
    ui_text(110, 308, "请勿用于存放重要数据的机器。安装程序会把系统文件写入你", C_DIM);
    ui_text(110, 342, "选择的硬盘分区 —— 所选分区上的内容会被覆盖。", C_DIM);
    ui_text(110, 400, "（与 Windows 10 安装程序一致，本安装程序不要求输入产品密钥）", C_DIM);
    ui_text(80, 540, g_st.license_ok ? "[X] 我接受许可条款" : "[  ] 我接受许可条款", C_TEXT);
    push_button(fb_width() - 240, fb_height() - 92, 180, 56, "下一步", 1, true, g_st.license_ok);
    push_button(fb_width() - 440, fb_height() - 92, 180, 56, "上一步", 2, false, true);
}

static void draw_type_page() {
    page_header("选择安装类型");
    push_button(80, 200, fb_width() - 160, 96, "升级：安装 VimtuOS 并保留文件、设置和应用", 1, false, true);
    push_button(80, 320, fb_width() - 160, 96, "自定义：仅安装 VimtuOS (高级)", 2, false, true);
    ui_text(80, 470, "提示：自定义安装可以选择硬盘并创建/删除/格式化分区。", C_DIM);
    push_button(fb_width() - 440, fb_height() - 92, 180, 56, "上一步", 3, false, true);
}

// ==================== ★ 新增页：用户与时区 ====================
// 需求（用户 V1 终版）：
//   1) 用户名（默认 vimtu）+ 密码（留空 = 无密码，必须明说）
//   2) 时区**下拉框**（无地图）；默认 China / Asia/Shanghai
//   3) 下拉框文案随**第一步选的语言**切换：中文 -> "中国 (Asia/Shanghai)"，英文 -> "China (Asia/Shanghai)"；
//      但写到底层的**永远是标准时区串**（如 Asia/Shanghai），显示语言不影响配置。
//   4) 位置：第 2 步（安装类型）之后、第 3 步（磁盘与分区）之前；不改整体 UI 风格。
//
// 为什么时区表里带 off_min：内核没有 tzdata，也没有时区库；安装期把"标准串 + 该串对应偏移"
// 一起写进 /etc/user.conf，登录/时钟直接读偏移即可（标准串仍然保留，作为语义与将来的 tzdata 接口）。
struct SetupTz64 {
    const char* en;      // 英文标签（lang=1 时显示）
    const char* zh;      // 中文标签（lang=0 时显示）
    const char* zone;    // ★ 标准时区串（唯一写盘的东西）
    int         off_min; // 相对 UTC 的分钟数
};
static const SetupTz64 kTz[] = {
    { "China",               "中国",           "Asia/Shanghai",      480 },
    { "China (Hong Kong)",   "中国香港",       "Asia/Hong_Kong",     480 },
    { "China (Taipei)",      "中国台湾",       "Asia/Taipei",        480 },
    { "Japan",               "日本",           "Asia/Tokyo",         540 },
    { "Korea",               "韩国",           "Asia/Seoul",         540 },
    { "Singapore",           "新加坡",         "Asia/Singapore",     480 },
    { "India",               "印度",           "Asia/Kolkata",       330 },
    { "United Arab Emirates","阿联酋",         "Asia/Dubai",         240 },
    { "Russia (Moscow)",     "俄罗斯（莫斯科）","Europe/Moscow",      180 },
    { "Germany",             "德国",           "Europe/Berlin",       60 },
    { "France",              "法国",           "Europe/Paris",        60 },
    { "United Kingdom",      "英国",           "Europe/London",        0 },
    { "United States (East)","美国（东部）",   "America/New_York",  -300 },
    { "United States (West)","美国（西部）",   "America/Los_Angeles",-480 },
    { "Brazil",              "巴西",           "America/Sao_Paulo", -180 },
    { "Australia (Sydney)",  "澳大利亚（悉尼）","Australia/Sydney",   600 },
    { "New Zealand",         "新西兰",         "Pacific/Auckland",   720 },
};
#define TZ64_N ((int)(sizeof(kTz) / sizeof(kTz[0])))

// 页面几何：绘制与鼠标命中共用同一组常量（避免"看到的"和"点到的"不一致）
static const int UC_FX       = 200;   // 输入框左边界
static const int UC_FW       = 420;   // 输入框宽
static const int UC_FH       = 40;    // 输入框高
static const int UC_NAME_Y   = 170;
static const int UC_PW_Y     = 236;
static const int UC_HINT_DY  = 52;    // "留空代表无密码"提示相对密码框的纵向偏移
static const int UC_TZ_Y     = 336;
static const int UC_TZ_W     = 520;
static const int UC_TZ_ROW_H = 28;
static const int UC_TZ_ROWS  = 7;     // 下拉框可见行数（其余靠滚动）

static const char* uc_tz_label(int idx) {
    if (idx < 0 || idx >= TZ64_N) idx = 0;
    return (g_st.lang == 0) ? kTz[idx].zh : kTz[idx].en;
}
// 下拉框展开时，把选中项保持在可见窗口里
static void uc_tz_ensure_visible() {
    if (g_st.tz_idx < g_st.tz_top) g_st.tz_top = g_st.tz_idx;
    if (g_st.tz_idx >= g_st.tz_top + UC_TZ_ROWS) g_st.tz_top = g_st.tz_idx - UC_TZ_ROWS + 1;
    if (g_st.tz_top < 0) g_st.tz_top = 0;
    if (g_st.tz_top > TZ64_N - UC_TZ_ROWS) g_st.tz_top = (TZ64_N > UC_TZ_ROWS) ? TZ64_N - UC_TZ_ROWS : 0;
}

static void uc_draw_field(int y, const char* text, bool focused, bool is_pw) {
    fb_draw_rect(UC_FX, y - 10, UC_FW, UC_FH, focused ? C_WHITE : C_DIM);
    if (focused) fb_fill_rect(UC_FX + 2, y - 8, UC_FW - 4, UC_FH - 4, C_LIST);
    char shown[40];
    int n = 0;
    if (is_pw) {
        // 密码不回显明文，只回显位数（内核日志同样从不打印明文）
        const int len = (int)sizeof(g_st.user_pw) - 1;
        int l = 0;
        while (l < len && g_st.user_pw[l]) l++;
        for (int i = 0; i < l && n < (int)sizeof(shown) - 1; i++) shown[n++] = '*';
        shown[n] = 0;
        if (l == 0) { ui_text(UC_FX + 12, y, "（留空）", C_DIM); return; }
    } else {
        while (text && text[n] && n < (int)sizeof(shown) - 1) { shown[n] = text[n]; n++; }
        shown[n] = 0;
    }
    ui_text(UC_FX + 12, y, shown, C_TEXT);
    if (focused) {
        // 光标：按字符宽度粗算（等宽字体，16px 下每字符约 8px）
        const int cx = UC_FX + 12 + n * 8 + 1;
        fb_fill_rect(cx, y - 2, 2, 18, C_WHITE);
    }
}

static void draw_user_page() {
    page_header("用户与时区设置");

    int y = UC_NAME_Y;
    ui_text(80, y, "用户名：", C_TEXT);
    uc_draw_field(y, g_st.user_name, g_st.user_focus == 0, false);

    y = UC_PW_Y;
    ui_text(80, y, "密码：", C_TEXT);
    uc_draw_field(y, g_st.user_pw, g_st.user_focus == 1, true);
    ui_text(80, y + UC_HINT_DY, "提示：密码留空代表【无密码】—— 开机将跳过密码校验直接进入桌面。", C_DIM);

    y = UC_TZ_Y;
    ui_text(80, y, "时区：", C_TEXT);
    const bool tz_focus = (g_st.user_focus == 2);
    fb_draw_rect(UC_FX, y - 10, UC_TZ_W, UC_FH, (tz_focus || g_st.tz_open) ? C_WHITE : C_DIM);
    if (tz_focus || g_st.tz_open) fb_fill_rect(UC_FX + 2, y - 8, UC_TZ_W - 4, UC_FH - 4, C_LIST);
    ui_text(UC_FX + 12, y, uc_tz_label(g_st.tz_idx), C_TEXT);
    ui_text(UC_FX + UC_TZ_W - 28, y, g_st.tz_open ? "^" : "v", C_TEXT);

    if (g_st.tz_open) {
        uc_tz_ensure_visible();
        const int ly = y + UC_FH + 6;
        const int lh = UC_TZ_ROWS * UC_TZ_ROW_H;
        fb_fill_rect(UC_FX, ly, UC_TZ_W, lh, C_LIST);
        fb_draw_rect(UC_FX, ly, UC_TZ_W, lh, C_DIM);
        for (int i = 0; i < UC_TZ_ROWS; i++) {
            const int idx = g_st.tz_top + i;
            if (idx >= TZ64_N) break;
            const int ry = ly + i * UC_TZ_ROW_H;
            if (idx == g_st.tz_idx) fb_fill_rect(UC_FX + 2, ry + 1, UC_TZ_W - 4, UC_TZ_ROW_H - 2, C_SEL);
            ui_text(UC_FX + 12, ry + 6, uc_tz_label(idx), C_TEXT);
            ui_text(UC_FX + 260, ry + 6, kTz[idx].zone, C_DIM);   // 标准串一起显示（写盘的就是它）
        }
    }

    push_button(fb_width() - 440, fb_height() - 92, 180, 56, "上一步", 2, false, true);
    push_button(fb_width() - 240, fb_height() - 92, 180, 56, "下一步", 1, true, true);
}
static void draw_disk_page() {
    page_header("你想将 VimtuOS 安装到哪里？");

    const int list_x = 80, list_y = 150;
    const int list_w = fb_width() - 160, list_h = 360;
    fb_fill_rect(list_x, list_y, list_w, list_h, C_LIST);
    fb_draw_rect(list_x, list_y, list_w, list_h, C_DIM);

    if (g_row_count == 0) {
        // ★ item 5a：找不到任何可用磁盘时，**直接把屏幕硬件检查报告画在这里** ——
        //   真机上没有串口，让用户面对一个空白列表发呆是没法排查的（报告里有存储/输入/显示区块，
        //   一眼就能看出"是没接盘 / 是 NVMe 不支持 / 是 SATA 没在 AHCI 模式"）。
        //   报告不放在这里时（有盘）不画，以免干扰正常流程与既有像素断言。
        ui_text(list_x + 24, list_y + 10, "没有检测到可用硬盘（no usable disk）。请检查接线/BIOS 的 SATA 模式后点击\"刷新\"。", C_TEXT);
        {
            // 只打一次日志（每帧重绘都会走到这里，不能刷屏）
            static bool logged = false;
            if (!logged) {
                logged = true;
                dbg64_line_begin64();
                dbg64_str("[HWUI] report drawn in setup wizard (no usable disk)");
                dbg64_nl();
                dbg64_line_end64();
            }
        }
        const int ry = list_y + 44;
        hwui64_draw64(list_x + 6, ry, list_w - 12, list_h - 50, 0);
    }
    const int row_h = 34;
    for (int i = 0; i < g_row_count; i++) {
        const int ry = list_y + 8 + i * row_h;
        if (ry + row_h > list_y + list_h) break;
        if (i == g_row_sel) fb_fill_rect(list_x + 6, ry - 2, list_w - 12, row_h, C_SEL);
        ui_text(list_x + 18, ry, g_rows[i].text, C_TEXT);
    }

    // 按钮行（对齐 Win10：刷新 / 新建 / 删除 / 格式化）
    // ★ v0.5.14：删除/格式化走**两步确认**（见 g_pending_act 的说明）。
    const int by = list_y + list_h + 24;
    const bool row_ok  = (g_row_sel >= 0 && g_row_sel < g_row_count);
    const bool part_selected = (row_ok && g_rows[g_row_sel].selectable);
    const bool arm_del = (g_pending_act == 12 && g_pending_row == g_row_sel);
    const bool arm_fmt = (g_pending_act == 13 && g_pending_row == g_row_sel);
    push_button(80,  by, 140, 48, "刷新",   10, false, true);
    push_button(240, by, 140, 48, "新建",   11, false, row_ok);
    push_button(400, by, 160, 48, arm_del ? "确认删除"   : "删除",
                12, arm_del, part_selected);
    push_button(580, by, 180, 48, arm_fmt ? "确认格式化" : "格式化",
                13, arm_fmt, part_selected);

    push_button(fb_width() - 260, fb_height() - 92, 200, 56,
                part_selected ? "安装系统" : "下一步", 1, true, part_selected);
    push_button(fb_width() - 460, fb_height() - 92, 180, 56, "上一步", 2, false, true);
    if (arm_del || arm_fmt) {
        char buf[128]; char* q = buf; *q = 0;
        str_append(q, arm_del ? "确认删除 分区 " : "确认格式化 分区 ", 24);
        str_append_u64(q, (uint64_t)(g_rows[g_row_sel].part + 1), 4);
        str_append(q, arm_del ? "（再按一次 D 或点\"确认删除\"；Esc 取消）"
                               : "（再按一次 F 或点\"确认格式化\"；Esc 取消）", 64);
        ui_text(80, fb_height() - 76, buf, C_WARN);
    } else if (g_status[0]) {
        ui_text(80, fb_height() - 76, g_status, C_WARN);
    } else {
        ui_text(80, fb_height() - 76, part_selected
                    ? "已选择分区：点右下角\"安装系统\"开始安装（删除/格式化需按两次确认）"
                    : "选择一个分区后可 删除/格式化；\"新建\"会重新初始化所选磁盘的分区表", C_DIM);
    }
}

static const char* kStageText[5] = {
    "正在收集信息",
    "正在安装 VimtuOS 文件",
    "正在安装更新",
    "正在完成安装",
    "即将完成"
};

static void draw_progress_page() {
    page_header("正在安装 VimtuOS");
    const int bx = 80, by = 260, bw = fb_width() - 160, bh = 34;
    fb_fill_rect(bx, by, bw, bh, 0xFF00294F);
    fb_draw_rect(bx, by, bw, bh, C_DIM);
    const int fill = (int)((int64_t)bw * g_st.progress / 100);
    if (fill > 0) fb_fill_rect(bx + 1, by + 1, fill - 2, bh - 2, C_BTN);

    char buf[64]; char* p = buf; *p = 0;
    str_append(p, "已完成 ", 20);
    str_append_u64(p, (uint64_t)g_st.progress, 4);
    str_append(p, "%", 2);
    ui_text(bx, by + bh + 20, buf, C_TEXT);

    ui_text(bx, by + bh + 60, kStageText[g_st.stage >= 0 && g_st.stage < 5 ? g_st.stage : 0], C_DIM);
    ui_text(bx, by + bh + 100, "安装期间请不要关闭计算机。", C_DIM);
}

static void draw_done_page() {
    page_header("安装完成");
    ui_text(80, 200, "VimtuOS 已安装到所选硬盘，计算机将自动重启。", C_TEXT);
    ui_text(80, 250, "（重启后请从硬盘启动；若使用 U 盘/光盘安装介质，请先移除）", C_DIM);
    ui_text(80, 300, "系统将在几秒后自动重启；也可以点右下角\"立即重启\"。", C_DIM);
    push_button(fb_width() - 260, fb_height() - 92, 200, 56, "立即重启", 1, true, true);
}

// ==================== 页面入口 ====================
static void draw_page() {
    g_btn_count = 0;
    fb_clear(C_BG);
    switch (g_st.page) {
        case PAGE_LANG:     draw_lang_page();     break;
        case PAGE_NOW:      draw_now_page();      break;
        case PAGE_LICENSE:  draw_license_page();  break;
        case PAGE_TYPE:     draw_type_page();     break;
        case PAGE_USER:     draw_user_page();     break;
        case PAGE_DISK:     draw_disk_page();     break;
        case PAGE_PROGRESS: draw_progress_page(); break;
        case PAGE_DONE:     draw_done_page();     break;
        default: break;
    }
    // 统一绘制本页注册的按钮（按当前鼠标位置画悬停态）
    // ★ 踩坑记录：漏掉这一步时，所有按钮都"只注册不画"，屏幕上根本没有"下一步"。
    for (int i = 0; i < g_btn_count; i++) ui_button(g_btns[i], g_mx, g_my);
}

// ---- 安装动作（下一批接真实写盘；界面已能走到这些按钮上）----
static void part_refresh() {
    confirm_cancel("refresh");
    enumerate_disks();
    build_rows();
}
// 自动创建分区表 + 引导分区 + 主分区（GPT：保护性 MBR + ESP + MSR + 主分区）
static bool part_create_auto(int drive, int row_hint);
static bool part_delete(int drive, int part_index);
static int  part_format(int drive, int part_index, char* fs, int fs_cap);
static void install_begin(int drive, int part_index);
// ==================== 事件与主循环 ====================
static void go_page(SetupPage p) {
    confirm_cancel("page");
    g_st.page = p;
    g_st.dirty = true;
    dbg64_str("[SETUP] page=");
    dbg64_dec((uint64_t)p);
    dbg64_nl();
}

static void on_button(int id) {
    switch (g_st.page) {
        case PAGE_LANG:
            if (id == 1) go_page(PAGE_NOW);
            break;
        case PAGE_NOW:
            if (id == 1) go_page(PAGE_LICENSE);
            if (id == 2) { /* 修复计算机：留待后续 */ }
            break;
        case PAGE_LICENSE:
            if (id == 1 && g_st.license_ok) go_page(PAGE_TYPE);
            if (id == 2) go_page(PAGE_NOW);
            break;
        case PAGE_TYPE:
            if (id == 1) { g_st.install_type = 0; go_page(PAGE_USER); }   // 升级（本系统等价于自定义）
            if (id == 2) { g_st.install_type = 1; go_page(PAGE_USER); }
            if (id == 3) go_page(PAGE_LICENSE);
            break;
        case PAGE_USER:
            // ★ 新增页：只有"上一步/下一步"，输入与下拉框在 handle_key / handle_mouse_click 里处理
            if (id == 1) {
                g_st.tz_open = 0;
                g_st.user_focus = 3;
                dbg64_line_begin64();
                dbg64_str("[SETUP] userconf user=");
                dbg64_str(g_st.user_name);
                dbg64_str(" tz=");
                dbg64_str(kTz[g_st.tz_idx].zone);
                dbg64_str(" off=");
                dbg64_dec((uint64_t)(kTz[g_st.tz_idx].off_min < 0 ? 0 : kTz[g_st.tz_idx].off_min));
                if (kTz[g_st.tz_idx].off_min < 0) dbg64_str("-");
                dbg64_str(" pass=");
                dbg64_dec((uint64_t)(g_st.user_pw[0] ? 1 : 0));
                dbg64_str(" lang=");
                dbg64_dec((uint64_t)g_st.lang);
                dbg64_str(" (collected; written to target /etc/user.conf at install end)");
                dbg64_nl();
                dbg64_line_end64();
                go_page(PAGE_DISK);
            }
            if (id == 2) { g_st.tz_open = 0; go_page(PAGE_TYPE); }
            break;
        case PAGE_DISK:
            if (id == 1) {                       // 安装系统
                if (g_row_sel >= 0 && g_row_sel < g_row_count && g_rows[g_row_sel].selectable) {
                    install_begin(g_rows[g_row_sel].drive, g_rows[g_row_sel].part);
                }
            }
            if (id == 2) go_page(PAGE_USER);     // ★ 上一页现在是"用户与时区"
            if (id == 10) { part_refresh(); status_clear(); g_st.dirty = true; }
            if (id == 11) {                       // 新建：写引导分区 + 主分区
                const int d = (g_row_sel >= 0 && g_row_sel < g_row_count) ? g_rows[g_row_sel].drive : -1;
                if (part_create_auto(d, g_row_sel)) {
                    part_refresh();
                    // 选中"刚建好分区的那块盘"上的第一个分区（行索引在 refresh 后会变，按 drive 重找）
                    for (int i = 0; i < g_row_count; i++)
                        if (g_rows[i].selectable && g_rows[i].drive == d) { g_row_sel = i; break; }
                }
                g_st.dirty = true;
            }
            if (id == 12) {                       // 删除分区（两步确认）
                const int row = g_row_sel;
                if (row < 0 || row >= g_row_count || !g_rows[row].selectable) {
                    g_st.dirty = true;
                    break;
                }
                const int d = g_rows[row].drive;
                const int part_no = g_rows[row].part + 1;
                if (!(g_pending_act == 12 && g_pending_row == row)) {
                    g_pending_act = 12;
                    g_pending_row = row;
                    status_clear();
                    dbg64_str("[SETUP] 删除待确认 drive=");
                    dbg64_dec(d);
                    dbg64_str(" part=");
                    dbg64_dec(part_no);
                    dbg64_str(g_disks[d].parts[g_rows[row].part].is_gpt ? " table=gpt" : " table=mbr");
                    dbg64_nl();
                } else {
                    g_pending_act = 0;
                    g_pending_row = -1;
                    if (part_delete(d, g_rows[row].part)) {
                        status_set_op("已删除 分区 ", part_no, "（分区表已同步，首扇区已抹）");
                        part_refresh();
                        // 删除后把光标停回**同一块盘**：优先它的第一个分区，否则它的标题行。
                        // 这样连续删除/格式化不用重新找盘，而且绝不会飘到安装介质盘上去
                        // （行索引在 refresh 后会变，所以按 drive 重找，不能沿用旧索引）。
                        int sel = -1;
                        for (int i = 0; i < g_row_count; i++)
                            if (g_rows[i].selectable && g_rows[i].drive == d) { sel = i; break; }
                        if (sel < 0)
                            for (int i = 0; i < g_row_count; i++)
                                if (g_rows[i].part == -1 && g_rows[i].drive == d) { sel = i; break; }
                        if (sel < 0) sel = (g_row_count > 0) ? 0 : -1;
                        g_row_sel = sel;
                    } else {
                        status_set_op("无法删除 分区 ", part_no, "（见串口 [PART] 行）");
                    }
                }
                g_st.dirty = true;
            }
            if (id == 13) {                       // 格式化分区（两步确认）
                const int row = g_row_sel;
                // ★ 诊断打点（v0.9.28 追 multivol 阶段 2 的"光标没落对行"）：把当前行完整打出来，
                //   不可选时也打 —— 只靠日志里"没有待确认行"是猜不出光标停在哪一行的。
                dbg64_str("[SETUP] fmt attempt sel=");
                dbg64_dec((uint64_t)row);
                dbg64_str(" rows=");
                dbg64_dec((uint64_t)g_row_count);
                if (row >= 0 && row < g_row_count) {
                    dbg64_str(" drive=");
                    dbg64_dec((uint64_t)g_rows[row].drive);
                    dbg64_str(" part=");
                    dbg64_dec((uint64_t)(g_rows[row].part + 1));
                    dbg64_str(g_rows[row].selectable ? " selectable=1" : " selectable=0");
                }
                dbg64_nl();
                if (row < 0 || row >= g_row_count || !g_rows[row].selectable) {
                    g_st.dirty = true;
                    break;
                }
                const int d = g_rows[row].drive;
                const int part_no = g_rows[row].part + 1;
                if (!(g_pending_act == 13 && g_pending_row == row)) {
                    g_pending_act = 13;
                    g_pending_row = row;
                    status_clear();
                    dbg64_str("[SETUP] 格式化待确认 drive=");
                    dbg64_dec(d);
                    dbg64_str(" part=");
                    dbg64_dec(part_no);
                    dbg64_str(g_disks[d].parts[g_rows[row].part].is_gpt ? " table=gpt" : " table=mbr");
                    dbg64_nl();
                } else {
                    g_pending_act = 0;
                    g_pending_row = -1;
                    const int keep = row;
                    char fs[24];
                    fs[0] = 0;
                    const int rc = part_format(d, g_rows[row].part, fs, (int)sizeof(fs));
                    if (rc == 0) {
                        char* q = g_status;
                        *q = 0;
                        str_append(q, "已格式化 分区 ", 24);
                        str_append_u64(q, (uint64_t)part_no, 4);
                        str_append(q, " -> ", 6);
                        str_append(q, fs, 20);
                        part_refresh();
                        // 格式化不动分区表，把光标停回同一行
                        if (keep < g_row_count && g_rows[keep].selectable) g_row_sel = keep;
                    } else if (rc == -2) {
                        char* q = g_status;
                        *q = 0;
                        str_append(q, "无法格式化 分区 ", 26);
                        str_append_u64(q, (uint64_t)part_no, 4);
                        str_append(q, "：EFI 系统分区受保护（UEFI 引导卷）", 60);
                    } else {
                        status_set_op("格式化失败 分区 ", part_no, "（见串口 [PART] 行）");
                    }
                }
                g_st.dirty = true;
            }
            break;
        case PAGE_DONE:
            if (id == 1) {
                // 自动重启：走 8042 复位（与 32 位系统的重启方式一致）
                dbg64_str("[SETUP] rebooting via 8042");
                dbg64_nl();
                for (;;) {
                    uint8_t st;
                    do { st = inb64(0x64); } while (st & 0x02);
                    outb64(0x64, 0xFE);
                    for (volatile int i = 0; i < 1000000; i++) { }
                }
            }
            break;
        default: break;
    }
}

static void handle_key(uint8_t ch) {
    dbg64_str("[SETUP] key=0x");
    dbg64_hex64(ch);                                   // 便于自动验收：确认按键真的到达向导
    dbg64_nl();
    if (ch == 0x1B) {                                  // Esc = 取消武装 / 上一步
        if (g_pending_act != 0) { confirm_cancel("esc"); g_st.dirty = true; return; }
        if (g_st.page > PAGE_LANG) go_page((SetupPage)(g_st.page - 1));
        return;
    }
    // ★ 新增页：用户名 / 密码 / 时区下拉（纯键盘也能走完：Tab 换焦点、上下改时区、回车下一步）
    if (g_st.page == PAGE_USER) {
        if (ch == 0x09) {                                  // Tab：用户名 -> 密码 -> 时区 -> 用户名
            g_st.tz_open = 0;
            g_st.user_focus = (g_st.user_focus >= 2) ? 0 : g_st.user_focus + 1;
            g_st.dirty = true;
            return;
        }
        if (ch == 0xFD || ch == 0xFE) {                    // 上/下：展开时移动下拉选中项
            if (g_st.tz_open) {
                int v = g_st.tz_idx + (ch == 0xFE ? 1 : -1);
                if (v < 0) v = TZ64_N - 1;
                if (v >= TZ64_N) v = 0;
                g_st.tz_idx = v;
                uc_tz_ensure_visible();
                g_st.dirty = true;
            } else {
                g_st.user_focus = 2;                        // 未展开：方向键把焦点交给时区
                g_st.dirty = true;
            }
            return;
        }
        if (ch == '\r' || ch == '\n') {                     // 回车：展开时先收起；否则 = 下一步（全用默认值）
            if (g_st.tz_open) { g_st.tz_open = 0; g_st.dirty = true; return; }
            on_button(1);
            return;
        }
        if (ch == 0x08 || ch == 0x7F) {                     // 退格
            if (g_st.user_focus == 0) {
                int n = 0; while (n < USERCONF64_USER_MAX && g_st.user_name[n]) n++;
                if (n > 0) g_st.user_name[n - 1] = 0;
            } else if (g_st.user_focus == 1) {
                int n = 0; while (n < (int)sizeof(g_st.user_pw) - 1 && g_st.user_pw[n]) n++;
                if (n > 0) g_st.user_pw[n - 1] = 0;
            }
            g_st.dirty = true;
            return;
        }
        if (ch >= 0x20 && ch < 0x7F) {                      // 可打印 ASCII
            if (g_st.user_focus == 0) {
                int n = 0; while (n < USERCONF64_USER_MAX && g_st.user_name[n]) n++;
                if (n < USERCONF64_USER_MAX) { g_st.user_name[n] = (char)ch; g_st.user_name[n + 1] = 0; }
            } else if (g_st.user_focus == 1) {
                int n = 0; while (n < (int)sizeof(g_st.user_pw) - 1 && g_st.user_pw[n]) n++;
                if (n < (int)sizeof(g_st.user_pw) - 1) { g_st.user_pw[n] = (char)ch; g_st.user_pw[n + 1] = 0; }
                // 密码永远不进串口：这里不打印任何内容
            } else if (g_st.user_focus == 2) {
                g_st.tz_open = 1;                            // 时区上有按键 = 展开下拉
            }
            g_st.dirty = true;
            return;
        }
        return;
    }
    if (ch == 0xFD) {                                  // 上
        if (g_st.page == PAGE_LANG) { g_st.lang = 0; g_st.dirty = true; }
        else if (g_st.page == PAGE_DISK && g_row_count > 0) {
            g_row_sel = (g_row_sel <= 0) ? g_row_count - 1 : g_row_sel - 1;
            confirm_cancel("sel");
            g_st.dirty = true;
        }
        return;
    }
    if (ch == 0xFE) {                                  // 下
        if (g_st.page == PAGE_LANG) { g_st.lang = 1; g_st.dirty = true; }
        else if (g_st.page == PAGE_DISK && g_row_count > 0) {
            g_row_sel = (g_row_sel < 0 || g_row_sel + 1 >= g_row_count) ? 0 : g_row_sel + 1;
            confirm_cancel("sel");
            g_st.dirty = true;
        }
        return;
    }
    if (ch == ' ' && g_st.page == PAGE_LICENSE) {      // 空格 = 勾选许可
        g_st.license_ok = !g_st.license_ok;
        g_st.dirty = true;
        return;
    }
    if (ch == '\r' || ch == '\n') {                    // 回车 = 主按钮
        if (g_st.page == PAGE_LICENSE) { g_st.license_ok = true; go_page(PAGE_TYPE); return; }
        if (g_st.page == PAGE_DISK) {
            if (g_row_sel >= 0 && g_rows[g_row_sel].selectable) on_button(1);
            return;
        }
        on_button(1);
        return;
    }
    // 磁盘页快捷键（与 Win10 的按钮热键一致）
    if (g_st.page == PAGE_DISK && g_row_sel >= 0) {
        if (ch == 'n' || ch == 'N') on_button(11);
        else if (ch == 'd' || ch == 'D') on_button(12);
        else if (ch == 'f' || ch == 'F') on_button(13);
        else if (ch == 'r' || ch == 'R') on_button(10);
    }
}

static void handle_mouse_click(int mx, int my) {
    for (int i = 0; i < g_btn_count; i++) {
        if (ui_button_hit(g_btns[i], mx, my)) { on_button(g_btns[i].id); return; }
    }
    // ★ 新增页：输入框 / 时区下拉框（命中区与 draw_user_page 用同一组常量）
    if (g_st.page == PAGE_USER) {
        // 用户名输入框
        if (mx >= UC_FX && mx < UC_FX + UC_FW && my >= UC_NAME_Y - 10 && my < UC_NAME_Y - 10 + UC_FH) {
            g_st.user_focus = 0;
            g_st.tz_open = 0;
            g_st.dirty = true;
            return;
        }
        // 密码输入框
        if (mx >= UC_FX && mx < UC_FX + UC_FW && my >= UC_PW_Y - 10 && my < UC_PW_Y - 10 + UC_FH) {
            g_st.user_focus = 1;
            g_st.tz_open = 0;
            g_st.dirty = true;
            return;
        }
        // 时区框本体：点击 = 展开/收起
        if (mx >= UC_FX && mx < UC_FX + UC_TZ_W && my >= UC_TZ_Y - 10 && my < UC_TZ_Y - 10 + UC_FH) {
            g_st.user_focus = 2;
            g_st.tz_open = !g_st.tz_open;
            uc_tz_ensure_visible();
            g_st.dirty = true;
            return;
        }
        // 展开的列表：点某一行 = 选中（写盘的永远是那一行的标准时区串）
        if (g_st.tz_open) {
            const int ly = UC_TZ_Y + UC_FH + 6;
            if (mx >= UC_FX && mx < UC_FX + UC_TZ_W && my >= ly && my < ly + UC_TZ_ROWS * UC_TZ_ROW_H) {
                const int row = (my - ly) / UC_TZ_ROW_H;
                const int idx = g_st.tz_top + row;
                if (idx >= 0 && idx < TZ64_N) {
                    g_st.tz_idx = idx;
                    g_st.tz_open = 0;
                    g_st.user_focus = 2;
                    g_st.dirty = true;
                }
                return;
            }
            // 点到别处：收起下拉（不吞掉这次点击以外的动作）
            g_st.tz_open = 0;
            g_st.dirty = true;
        }
    }
    // 许可页：点文本也能勾选
    if (g_st.page == PAGE_LICENSE && my >= 520 && my <= 570 && mx >= 70 && mx <= 520) {
        g_st.license_ok = !g_st.license_ok;
        g_st.dirty = true;
        return;
    }
    // 语言页：点选语言
    if (g_st.page == PAGE_LANG && mx >= 110 && mx <= 700) {
        if (my >= 180 && my <= 240)      { g_st.lang = 0; g_st.dirty = true; return; }
        if (my >= 240 && my <= 300)      { g_st.lang = 1; g_st.dirty = true; return; }
    }
    // 磁盘页：点列表行
    if (g_st.page == PAGE_DISK) {
        const int list_x = 80, list_y = 150, list_w = fb_width() - 160, list_h = 360;
        if (mx >= list_x && mx < list_x + list_w && my >= list_y && my < list_y + list_h) {
            const int idx = (my - list_y - 8) / 34;
            if (idx >= 0 && idx < g_row_count && idx < list_h / 34) {
                g_row_sel = idx;
                confirm_cancel("sel");
                g_st.dirty = true;
            }
        }
    }
}

// ==================== 安装动作（真实写盘）====================
// 每个动作都真的走 ATA PIO 读/写目标硬盘，并在串口留下可核对的行（供自动验收）。
static int  g_install_drive = -1;
static int  g_install_part  = -1;
// ★ 目标卷的起始 LBA。安装器**恒把系统卷写在主分区**（part_install_begin* 与
//   part_volume_grow64 用的都是 PART_MAIN_LBA），界面选中的"那一行"只决定**哪块盘**。
//   实测踩过：拿 row_part_target() 的 t.start 当目标，得到的是引导分区 LBA 65，
//   mount 必然 reason=magic —— 所以这里用安装器自己的常量，另存一个行起点做兜底。
static uint32_t g_install_start = PART_MAIN_LBA;
static uint32_t g_install_alt_start = 0;   // 界面所选分区起点（GPT/MBR 布局差异时兜底）
static InstallJob g_job;
static uint32_t   g_reboot_at = 0;      // 安装完成后自动重启的时间点（tick）
static bool       g_reboot_armed = false;

// 从行里取出"目标磁盘的总扇区数"
static uint32_t row_disk_sectors(int drive) {
    return (drive >= 0 && drive < kDiskSlots) ? g_disks[drive].sectors : 0;   // ★ 含 AHCI 盘（8..）
}

// 新建：写入标准布局（引导分区 + 主分区）
static bool part_create_auto(int drive, int row_hint) {
    (void)row_hint;
    if (drive < 0) return false;
    const uint32_t sectors = row_disk_sectors(drive);
    const bool ok = part_create_standard(drive, sectors);
    if (!ok) {
        dbg64_str("[SETUP] 新建失败（磁盘已有分区或容量不足）drive=");
        dbg64_dec(drive);
        dbg64_nl();
    }
    return ok;
}

// 把"界面这一行"翻译成写盘目标：**必须带上它是 MBR 项还是 GPT 项** ——
// 老实现只给 MBR 项号，在 GPT 盘上会删错项、格式化还会把 LBA1 的主 GPT 头清零
// （实测记录见 tests/partmgr64_test.py）。
static bool row_part_target(int drive, int part_index, PartTarget* t) {
    if (!t || drive < 0 || drive >= kDiskSlots) return false;
    if (part_index < 0 || part_index >= g_disks[drive].part_count) return false;
    const PartEntry& e = g_disks[drive].parts[part_index];
    t->src       = e.src;
    t->index     = part_index;
    t->start     = e.start;
    t->sectors   = e.sectors;
    t->mbr_type  = e.type;
    t->gpt_esp   = e.is_esp;
    t->gpt_basic = e.is_basic;
    return true;
}

static bool part_delete(int drive, int part_index) {
    PartTarget t;
    if (!row_part_target(drive, part_index, &t)) return false;
    return part_delete_partition64(drive, row_disk_sectors(drive), t);
}

static int part_format(int drive, int part_index, char* fs, int fs_cap) {
    PartTarget t;
    if (!row_part_target(drive, part_index, &t)) return -1;
    return part_format_partition64(drive, row_disk_sectors(drive), t, fs, fs_cap);
}

static void install_begin(int drive, int part_index) {
    g_install_drive = drive;
    g_install_part  = part_index;
    g_st.progress = 0;
    g_st.stage = 0;
    g_st.t_start = ticks64();
    // 介质描述符存在（ISO 安装介质：引导桩写在 0x0F00）→ 按描述符取载荷；
    // 否则老路径：在硬盘上找带 "VIMTUPAY" 头的安装介质。
    MediumDesc md;
    const bool has_md = part_read_medium_desc(&md);
    const bool ok_begin = has_md
        ? part_install_begin_medium(&g_job, md, drive, row_disk_sectors(drive))
        : part_install_begin(&g_job, VIMTU_PAYLOAD_LBA, drive, row_disk_sectors(drive));
    // ★ 目标卷在**主分区**（安装器与 part_volume_grow64 都用 PART_MAIN_LBA）。
    //   另存"界面所选分区起点"做兜底（布局与预期不同时再试一次）。
    g_install_start = PART_MAIN_LBA;
    g_install_alt_start = 0;
    {
        PartTarget t;
        if (row_part_target(drive, part_index, &t) && t.start != g_install_start) g_install_alt_start = t.start;
    }
    if (!ok_begin) {
        dbg64_str("[SETUP] 安装无法开始（未找到安装介质载荷）");
        dbg64_nl();
        g_st.progress = 0;
        go_page(PAGE_DISK);
        return;
    }
    dbg64_str("[SETUP] install_begin drive=");
    dbg64_dec(drive);
    dbg64_str(" part=");
    dbg64_dec(part_index);
    dbg64_str(" payload_sectors=");
    dbg64_dec(g_job.payload_sectors);
    dbg64_nl();
    go_page(PAGE_PROGRESS);
}

// 进度推进：由**真实写入的扇区数**驱动（不是按时间假装）
// ★ 安装收尾：把向导收集到的 用户名 / 密码哈希 / 时区 写进**目标盘**的 /etc/user.conf
//
// 为什么挂临时卷槽再写，而不是直接改扇区：
//   目标盘上那块卷就是标准 VimtuFS2；vfs64 已经有经过验收的 mount_slot + mkdirs_on + write_on
//   三件套（宏内核里"按槽操作"的那一族：调用期间临时切到该槽、返回前切回、身份按 root）。
//   自己再写一套"分配块 + 更新位图 + 写 inode + 算 CRC"的离线写入器属于重复实现、且极易与
//   内核侧口径漂移 —— 这里坚决复用，也不碰任何分区/克隆逻辑。
static void install_write_user_conf() {
    if (g_st.uc_written) return;
    g_st.uc_written = 1;                       // 只写一次（进度条每帧都会走到收尾分支）
    if (g_install_drive < 0 || g_install_start == 0) {
        dbg64_line_begin64();
        dbg64_str("[SETUP] user.conf SKIP reason=no-target drive=");
        dbg64_dec((uint64_t)(g_install_drive < 0 ? 0 : g_install_drive));
        if (g_install_drive < 0) dbg64_str("-");
        dbg64_str(" start=");
        dbg64_dec(g_install_start);
        dbg64_nl();
        dbg64_line_end64();
        return;
    }
    char txt[USERCONF64_FILE_MAX];
    const SetupTz64& tz = kTz[(g_st.tz_idx >= 0 && g_st.tz_idx < TZ64_N) ? g_st.tz_idx : 0];
    const int n = userdb64_userconf_build64(txt, (int)sizeof(txt), g_st.user_name, g_st.user_pw,
                                            tz.zone, tz.off_min, g_st.lang);
    const int pass_set = (g_st.user_pw[0] != 0) ? 1 : 0;   // 抹掉明文前先记下"是否设了口令"
    // 明文口令用完立刻从内存里抹掉（它不进日志、不进镜像，只在这里变成哈希）
    for (int i = 0; i < (int)sizeof(g_st.user_pw); i++) g_st.user_pw[i] = 0;
    if (n <= 0) {
        dbg64_line_begin64();
        dbg64_str("[SETUP] user.conf BUILD FAILED rc=");
        dbg64_dec((uint64_t)(n < 0 ? 1 : n));
        dbg64_nl();
        dbg64_line_end64();
        return;
    }
    const int slot = vfs64_slot_alloc64();
    if (slot < 0) {
        dbg64_str("[SETUP] user.conf SKIP reason=no-free-slot (VFS64_SLOT_MAX reached)\n");
        return;
    }
    uint32_t used_start = g_install_start;
    int mrc = vfs64_mount_slot64(slot, g_install_drive, used_start);
    if (mrc != 0 && g_install_alt_start != 0) {
        // 兜底：万一某天布局变成"系统卷落在界面选中的那个分区"，再试一次并如实记下用了哪个
        used_start = g_install_alt_start;
        mrc = vfs64_mount_slot64(slot, g_install_drive, used_start);
        dbg64_str("[SETUP] user.conf retry at alt start lba=");
        dbg64_dec(used_start);
        dbg64_str(" rc=");
        dbg64_dec((uint64_t)(mrc == 0 ? 0 : 1));
        dbg64_nl();
    }
    if (mrc != 0) {
        dbg64_line_begin64();
        dbg64_str("[SETUP] user.conf SKIP reason=mount-failed slot=");
        dbg64_dec((uint64_t)slot);
        dbg64_str(" drive=");
        dbg64_dec((uint64_t)g_install_drive);
        dbg64_str(" lba=");
        dbg64_dec(g_install_start);
        dbg64_nl();
        dbg64_line_end64();
        return;
    }
    // ★ 建父目录：必须**显式** mkdir("/etc")。实测 vfs64_mkdirs_on64() 传"文件路径"时
    //   并不会把最后一级的父目录建出来（打点 `path component not found path=/etc/user.conf`），
    //   所以这里先 mkdir 目录本身（已存在返回错误，忽略），再补一次逐级建。
    const int mkd = vfs64_mkdir_on64(slot, "/etc");
    (void)vfs64_mkdirs_on64(slot, "/etc");
    // 先显式建一个空文件（已存在返回错误，忽略）：write 的语义是"写到已解析的 inode"，
    // 不先 create 的话"目标卷上还没有这个文件"会被当成失败（实测过同类调用点）。
    const int crc = vfs64_create_on64(slot, USERCONF64_PATH);
    const int wrc = vfs64_write_on64(slot, USERCONF64_PATH, txt, n);
    dbg64_line_begin64();
    dbg64_str("[SETUP] user.conf write slot=");
    dbg64_dec((uint64_t)slot);
    dbg64_str(" drive=");
    dbg64_dec((uint64_t)g_install_drive);
    dbg64_str(" lba=");
    dbg64_dec(used_start);
    dbg64_str(" path=");
    dbg64_str(USERCONF64_PATH);
    dbg64_str(" bytes=");
    dbg64_dec((uint64_t)n);
    dbg64_str(" user=");
    dbg64_str(g_st.user_name);
    dbg64_str(" tz=");
    dbg64_str(tz.zone);
    dbg64_str(" pass=");
    dbg64_dec((uint64_t)pass_set);
    dbg64_str(" mkdir=");
    dbg64_dec((uint64_t)mkd);
    dbg64_str(" create=");
    dbg64_dec((uint64_t)crc);
    dbg64_str(" write=");
    dbg64_dec((uint64_t)wrc);
    dbg64_str(" rc=");
    dbg64_dec((uint64_t)((wrc >= 0) ? 0 : 1));
    dbg64_nl();
    dbg64_line_end64();
    dbg64_line_begin64();
    dbg64_str("[SETUP] userconf applied user=");
    dbg64_str(g_st.user_name);
    dbg64_str(" tz=");
    dbg64_str(tz.zone);
    dbg64_str(" off=");
    if (tz.off_min < 0) dbg64_str("-");
    dbg64_dec((uint64_t)(tz.off_min < 0 ? -tz.off_min : tz.off_min));
    dbg64_str(" pass_set=");
    dbg64_dec((uint64_t)pass_set);
    dbg64_str(" lang=");
    dbg64_dec((uint64_t)g_st.lang);
    dbg64_str(" (login reads this file at boot)\n");
    dbg64_line_end64();
}
static void progress_tick() {
    // 复制阶段：进度由**真实写入的扇区数**驱动（不是按时间假装）
    // ★ 踩坑记录：这里以前写成 `if (g_st.page != PAGE_PROGRESS) return;`，于是
    //   安装完成跳到完成页之后，下面的"等待 2.5 秒自动重启"永远轮不到执行 ——
    //   端到端测试里唯一 FAIL 的就是这条（装完不重启）。改成"复制阶段"条
    //   件判断后，完成页仍会每帧走到重启倒计时。
    if (g_st.page == PAGE_PROGRESS && g_job.active) {
        const int rc = part_install_step(&g_job);        // 每帧写一块（128 扇区 = 64KB）
        if (g_job.payload_sectors) {
            g_st.progress = (int)((uint64_t)g_job.copied * 100 / g_job.payload_sectors);
            if (g_st.progress > 100) g_st.progress = 100;
        }
        g_st.stage = imin(4, g_st.progress / 22);
        g_st.dirty = true;
        if (rc == 1) {
            dbg64_str("[SETUP] 安装完成 100%（真实写入 ");
            dbg64_dec(g_job.copied);
            dbg64_str(" 扇区）");
            dbg64_nl();
            // ★ 复制已经全部落盘（part_install_step 返回 1 = 收尾 CRC/超级块都写完了）——
            //   这时才把 /etc/user.conf 写进目标卷：挂临时卷槽、建 /etc、写文件，再自动重启。
            install_write_user_conf();
            // 按需求：安装完成后自动重启
            g_reboot_at = ticks64() + ms_to_ticks64(2500);
            g_reboot_armed = true;
            g_st.page = PAGE_DONE;
            g_st.dirty = true;
        } else if (rc < 0) {
            dbg64_str("[SETUP] 安装失败（见上面的 [INSTALL] 行）");
            dbg64_nl();
            go_page(PAGE_DISK);
        }
        return;
    }

    // 已完成后等 2.5 秒 -> 自动重启（用户也可以点"立即重启"）
    if (g_reboot_armed && g_st.page == PAGE_DONE) {
        if ((int32_t)(ticks64() - g_reboot_at) >= 0) {
            dbg64_str("[SETUP] 自动重启（8042 复位）");
            dbg64_nl();
            for (;;) {
                uint8_t st;
                do { st = inb64(0x64); } while (st & 0x02);
                outb64(0x64, 0xFE);
                for (volatile int i = 0; i < 1000000; i++) { }
            }
        }
        g_st.dirty = true;      // 让完成页也有刷新（便于观察倒计时）
    }
}

#include "setup64_keys.h"          /* 串口按键通道（COM2）实现，见该文件头说明 */
// ==================== 主循环 ====================
[[noreturn]] void setup64_run(const BootInfo* bi) {
    (void)bi;
    dbg64_str("[SETUP] 安装程序启动");

    // ---- FAT32 写入器自检（ESP 用）：内存卷上做完整往返，坏参数必须被拒 ----
    // 位置理由：在任何真盘 I/O 之前跑（自检只碰内存卷），结果打一行 [FAT64] selftest 供验收。
    (void)fat64_selftest64();

    // ---- ATA：注册/打开 IRQ14（安装程序全程在用 ATA），读写走中断驱动等待 + 超时回退轮询 ----
    ata64_init64();

    g_st.page = PAGE_LANG;
    g_st.lang = 0;
    g_st.license_ok = false;
    g_st.install_type = 1;
    g_st.progress = 0;
    g_st.stage = 0;
    // ★ 新增页默认值（用户 V1 需求）：用户名 vimtu、密码留空、时区 China / Asia/Shanghai
    {
        static const char kDefUser[] = "vimtu";
        int i = 0;
        for (; kDefUser[i] && i < USERCONF64_USER_MAX; i++) g_st.user_name[i] = kDefUser[i];
        g_st.user_name[i] = 0;
    }
    for (int i = 0; i < (int)sizeof(g_st.user_pw); i++) g_st.user_pw[i] = 0;
    g_st.user_focus = 0;
    g_st.tz_idx = 0;                 // kTz[0] = China / Asia/Shanghai / +480
    g_st.tz_open = 0;
    g_st.tz_top = 0;
    g_st.uc_written = 0;
    g_st.dirty = true;
    // 注意：这里不要调用 ata64_diag()（它发 IDENTIFY 但不读走那 256 字数据，
    // 会把设备留在"命令被中止"的状态，导致随后的 sector0 读取失败——已实测确认）。
    // 光驱（ATAPI）自检：ISO 形态的安装介质要靠它读盘（光盘不是硬盘，走 PACKET 命令）。
    // 有光驱就校验 ISO9660 主卷描述符，结果打进串口供自动验收断言。
    {
        int cd_drive = -1;
        if (ata64_atapi_selftest(&cd_drive)) {
            dbg64_str("[SETUP] 光驱就绪 drive=");
            dbg64_dec(cd_drive);
            dbg64_nl();
        }
    }
    part_refresh();
    dbg64_str("[SETUP] 磁盘枚举完成 rows=");
    dbg64_dec(g_row_count);
    dbg64_nl();
    // 默认选中"目标磁盘"那一行：优先选**不是安装介质**的磁盘（避免用户一上来就把介质盘分区，
    // 也避免误把系统装回安装介质）；建好分区后选中行会自动移到第一个分区上（见 on_button(11)）。
    // 介质描述符（ISO 安装介质）：有就把它打进串口，并且不再去硬盘上找载荷
    MediumDesc md;
    const bool has_md = part_read_medium_desc(&md);
    if (has_md) {
        dbg64_str("[SETUP] 介质描述符 OK kind=");
        dbg64_dec(md.kind);
        dbg64_str(" drive=");
        dbg64_dec(md.drive);
        dbg64_str(" payload_lba=");
        dbg64_dec(md.payload_lba);
        dbg64_str(" payload_secs=");
        dbg64_dec(md.payload_secs);
        dbg64_nl();
    }
    {
        // 判定"安装介质自己占着哪块盘"，让向导默认行与自动选分区都绕开它：
        //   * 无描述符（介质是裸盘/U 盘，走硬盘引导）→ 按载荷头魔数找；
        //   * 描述符 kind=1（光盘）→ 介质是 ATAPI 光驱，根本不在磁盘列表里，无需排除；
        //   * 描述符 kind=2（RAM 源，即 hybrid ISO 写进 U 盘/被当硬盘挂）→ 介质**就是**一块
        //     ATA 盘，而且会被枚举进列表；用 ISO9660 主卷描述符指纹把它认出来。
        if (has_md) {
            g_medium_drive = (md.kind == MEDIUM_RAM) ? part_find_iso_medium_drive() : -1;
        } else {
            g_medium_drive = part_find_payload_drive(VIMTU_PAYLOAD_LBA, 0);
        }
        if (g_medium_drive >= 0) {
            dbg64_str("[SETUP] 安装介质所在盘 drive=");
            dbg64_dec(g_medium_drive);
            dbg64_str("（向导默认行会跳过它）");
            dbg64_nl();
            build_rows();                  // 重建一次，把"<== 安装介质"标上去
        }
        // 默认选中"目标磁盘"那一行：优先选不是安装介质的磁盘（避免用户一上来就把介质盘分区）；
        // ★ 批次 O：**可移动盘（USB 存储，驱动器号 >= ATA64_USB_BASE）也不默认选中** ——
        //   选它当安装目标没有任何意义（本批它只读，装不进去），但"默认行"会被自动化直接回车确认，
        //   所以必须在这一步就绕开（不然一次回车就把光标停在 U 盘上）。
        g_row_sel = (g_row_count > 0) ? 0 : -1;
        for (int i = 0; i < g_row_count; i++) {
            if (g_rows[i].drive >= ATA64_USB_BASE) continue;              // 可移动盘：跳过
            if (g_rows[i].part == -1 && g_rows[i].drive != g_medium_drive) { g_row_sel = i; break; }
        }
    }
    // 如果这块盘上已经有分区，直接选中第一个分区（省一步）；同样跳过安装介质盘与可移动盘
    for (int i = 0; i < g_row_count; i++) {
        if (g_rows[i].drive >= ATA64_USB_BASE) continue;                  // 可移动盘：跳过
        if (g_rows[i].selectable && g_rows[i].drive != g_medium_drive) { g_row_sel = i; break; }
    }
    // ★ 诊断打点（v0.9.28）：把磁盘页的行清单与默认选中行摊开打出来。
    //   为什么必须打：自动化按键是"4 次回车翻页 -> 下移 -> f"，而光标停在哪一行
    //   在串口里**完全看不出来**（UI 只画屏幕）—— 出问题时只能猜。
    {
        const int show = (g_row_count < 8) ? g_row_count : 8;
        dbg64_str("[SETUP] disk rows=");
        dbg64_dec((uint64_t)g_row_count);
        dbg64_str(" default_sel=");
        dbg64_dec((uint64_t)g_row_sel);
        dbg64_str(" medium=");
        dbg64_dec((uint64_t)g_medium_drive);
        dbg64_nl();
        for (int i = 0; i < show; i++) {
            dbg64_str("[SETUP]   row=");
            dbg64_dec((uint64_t)i);
            dbg64_str(" drive=");
            dbg64_dec((uint64_t)g_rows[i].drive);
            dbg64_str(" part=");
            dbg64_dec((uint64_t)(g_rows[i].part + 1));
            dbg64_str(g_rows[i].selectable ? " sel" : " -");
            dbg64_str(" text=");
            dbg64_str(g_rows[i].text);
            dbg64_nl();
        }
    }

    // 鼠标初始位置放屏幕中间，避免"光标在角落看不见"
    // 鼠标初始位置放屏幕中间，并把屏幕边界告诉鼠标驱动（驱动会把光标钳在界内，
    // 这样 VMware 那种 ±255 的聚合位移也不会把位置累积到屏幕外贴着边缘跑）
    mouse_set_bounds(fb_width(), fb_height());
    mouse_set_pos(fb_width() / 2, fb_height() / 2);

    com2_init();                      /* 打开串口按键通道 COM2（无人值守安装用） */
    uint32_t last_buttons = 0;
    int last_cx = -1, last_cy = -1;
    for (;;) {
        // ---- 输入 ----
        com2_poll_keys();                 /* 串口送来的"按键"先投递（与真键盘同一条路径） */
        uint8_t ch;
        while (kbd_pop_char(&ch)) handle_key(ch);

        const int mx = mouse_get_x(), my = mouse_get_y();
        g_mx = mx; g_my = my;             // 供按钮画悬停态
        const uint32_t btns = mouse_get_buttons();
        if ((btns & 1) && !(last_buttons & 1)) handle_mouse_click(mx, my);
        if (mouse_has_event()) mouse_clear_event_flag();
        last_buttons = btns;

        // ---- 状态推进 ----
        progress_tick();

        // ---- 绘制 ----
        // 关键：**鼠标移动只重绘光标新旧位置那一小块**（脏矩形），不做整屏重绘。
        // 为什么必须这样：整屏重绘 1280x800x4B = 4MB 拷贝，在模拟器里一帧几十毫秒，客人读
        // PS/2 包的速度被拖慢，设备侧就把多次移动聚合成一个大位移（VMware 实测单包 ±255），
        // 于是光标一步窜到屏幕边界、之后贴着边滑 —— 也就是"鼠标一动就围着窗口边缘动"。
        // 现在：整屏只在本页状态变化（g_st.dirty）时重绘；鼠标移动走局部重绘 + fb_flip_region。
        const bool cursor_moved = (mx != last_cx || my != last_cy);
        if (g_st.dirty) {
            fb_reset_clip();
            draw_page();
            draw_cursor(mx, my);
            fb_flip();
            last_cx = mx; last_cy = my;
            g_st.dirty = false;
        } else if (cursor_moved) {
            const int CW = 18, CH = 20;      // 光标包围盒（含描边/阴影）
            int x0 = (last_cx < mx ? last_cx : mx) - 1;
            int y0 = (last_cy < my ? last_cy : my) - 1;
            int w  = (mx > last_cx ? mx - last_cx : last_cx - mx) + CW;
            int h  = (my > last_cy ? my - last_cy : last_cy - my) + CH;
            // 1) 把这一小块的背景重画回来（draw_page 在裁剪下只画这块；fb_clear 已支持裁剪）
            fb_set_clip(x0, y0, w, h);
            draw_page();
            fb_reset_clip();
            // 2) 在新位置画光标，3) 只把这一小块提交到屏幕
            draw_cursor(mx, my);
            fb_flip_region(x0 - 1, y0 - 1, w + 2, h + 2);
            last_cx = mx; last_cy = my;
        }
        __asm__ volatile("hlt");           // 等 PIT（4ms）或键鼠中断
    }
}
