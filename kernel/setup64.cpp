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

        uint64_t used = 0;
        for (int i = 0; i < g_disks[d].part_count && g_row_count < 24; i++) {
            ListRow& pr = g_rows[g_row_count++];
            pr.drive = d; pr.part = i; pr.selectable = true;
            char* q = pr.text;
            str_append(q, "    分区 ", 20);
            str_append_u64(q, (uint64_t)(i + 1), 4);
            str_append(q, ": ", 4);
            str_append(q, g_disks[d].parts[i].label, 40);
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
    const int by = list_y + list_h + 24;
    push_button(80,  by, 140, 48, "刷新",   10, false, true);
    push_button(240, by, 140, 48, "新建",   11, false, g_row_sel >= 0);
    push_button(400, by, 140, 48, "删除",   12, false, g_row_sel >= 0 && g_rows[g_row_sel].selectable);
    push_button(560, by, 160, 48, "格式化", 13, false, g_row_sel >= 0 && g_rows[g_row_sel].selectable);

    const bool part_selected = (g_row_sel >= 0 && g_row_sel < g_row_count && g_rows[g_row_sel].selectable);
    push_button(fb_width() - 260, fb_height() - 92, 200, 56,
                part_selected ? "安装系统" : "下一步", 1, true, part_selected);
    push_button(fb_width() - 460, fb_height() - 92, 180, 56, "上一步", 2, false, true);
    ui_text(80, fb_height() - 76, part_selected
                ? "已选择分区：点右下角\"安装系统\"开始安装"
                : "选择一个分区后，右下角会出现\"安装系统\"", C_DIM);
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
static void part_refresh() { enumerate_disks(); build_rows(); }
// 自动创建分区表 + 引导分区 + 主分区（GPT：保护性 MBR + ESP + MSR + 主分区）
static bool part_create_auto(int drive, int row_hint);
static bool part_delete(int drive, int part_index);
static bool part_format(int drive, int part_index);
static void install_begin(int drive, int part_index);
// ==================== 事件与主循环 ====================
static void go_page(SetupPage p) {
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
            if (id == 1) { g_st.install_type = 0; go_page(PAGE_DISK); }   // 升级（本系统等价于自定义）
            if (id == 2) { g_st.install_type = 1; go_page(PAGE_DISK); }
            if (id == 3) go_page(PAGE_LICENSE);
            break;
        case PAGE_DISK:
            if (id == 1) {
                if (g_row_sel >= 0 && g_row_sel < g_row_count && g_rows[g_row_sel].selectable) {
                    install_begin(g_rows[g_row_sel].drive, g_rows[g_row_sel].part);
                }
            }
            if (id == 2) go_page(PAGE_TYPE);
            if (id == 10) { part_refresh(); g_st.dirty = true; }
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
            if (id == 12) {                       // 删除：清掉 MBR 里对应的分区项
                const int d = (g_row_sel >= 0 && g_row_sel < g_row_count) ? g_rows[g_row_sel].drive : -1;
                if (g_row_sel >= 0 && g_row_sel < g_row_count &&
                    part_delete(g_rows[g_row_sel].drive, g_rows[g_row_sel].part)) {
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
                }
                g_st.dirty = true;
            }
            if (id == 13) {                       // 格式化：清分区首部（主分区写占位超级块）
                if (part_format(g_rows[g_row_sel].drive, g_rows[g_row_sel].part)) {
                    part_refresh();
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
    if (ch == 0x1B) {                                  // Esc = 上一步
        if (g_st.page > PAGE_LANG) go_page((SetupPage)(g_st.page - 1));
        return;
    }
    if (ch == 0xFD) {                                  // 上
        if (g_st.page == PAGE_LANG) { g_st.lang = 0; g_st.dirty = true; }
        else if (g_st.page == PAGE_DISK && g_row_count > 0) {
            g_row_sel = (g_row_sel <= 0) ? g_row_count - 1 : g_row_sel - 1;
            g_st.dirty = true;
        }
        return;
    }
    if (ch == 0xFE) {                                  // 下
        if (g_st.page == PAGE_LANG) { g_st.lang = 1; g_st.dirty = true; }
        else if (g_st.page == PAGE_DISK && g_row_count > 0) {
            g_row_sel = (g_row_sel < 0 || g_row_sel + 1 >= g_row_count) ? 0 : g_row_sel + 1;
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
                g_st.dirty = true;
            }
        }
    }
}

// ==================== 安装动作（真实写盘）====================
// 每个动作都真的走 ATA PIO 读/写目标硬盘，并在串口留下可核对的行（供自动验收）。
static int  g_install_drive = -1;
static int  g_install_part  = -1;
static InstallJob g_job;
static uint32_t   g_reboot_at = 0;      // 安装完成后自动重启的时间点（tick）
static bool       g_reboot_armed = false;

// 从行里取出"目标磁盘的总扇区数"
static uint32_t row_disk_sectors(int drive) {
    return (drive >= 0 && drive < kDiskSlots) ? g_disks[drive].sectors : 0;   // ★ 含 AHCI 盘（8..）
}

// 取出该行对应的分区信息（MBR 里的第 part 项，1-based）
static bool row_part_info(int drive, int part, PartInfo* out) {
    if (drive < 0 || part < 0) return false;
    static uint8_t sec[512];
    if (!ata64_read_sector(drive, 0, sec)) return false;
    PartInfo p[4];
    part_parse_mbr(sec, p);
    if (part >= 4 || !p[part].used) return false;
    *out = p[part];
    return true;
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

static bool part_delete(int drive, int part_index) {
    if (drive < 0 || part_index < 0) return false;
    return part_delete_entry(drive, part_index + 1);      // 界面是 0-based，MBR 项是 1-based
}

static bool part_format(int drive, int part_index) {
    PartInfo p;
    if (!row_part_info(drive, part_index, &p)) return false;
    return part_format_partition(drive, p, part_index + 1);
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
        // 默认选中"目标磁盘"那一行：优先选不是安装介质的磁盘（避免用户一上来就把介质盘分区）
        g_row_sel = (g_row_count > 0) ? 0 : -1;
        for (int i = 0; i < g_row_count; i++) {
            if (g_rows[i].part == -1 && g_rows[i].drive != g_medium_drive) { g_row_sel = i; break; }
        }
    }
    // 如果这块盘上已经有分区，直接选中第一个分区（省一步）；同样跳过安装介质盘
    for (int i = 0; i < g_row_count; i++)
        if (g_rows[i].selectable && g_rows[i].drive != g_medium_drive) { g_row_sel = i; break; }

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
