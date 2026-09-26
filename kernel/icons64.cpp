// icons64.cpp - 统一"真图标"层实现（本批新增）：磁盘图标包 -> img64 解码 -> 缓存 -> 按 palette 混色上屏
//
// 读代码顺序建议：1) 名称表/常量 2) 包加载与校验 3) 位图加载(VFS 覆盖/包)+缓存 4) 绘制 5) 自检。
// 设计边界（如实写在前面，免得误读）：
//   * 本文件只进**系统内核**（build64.sh 的 SRCS_DESKTOP），安装介质内核不链它 —— 安装向导不画桌面图标。
//   * 图标包在**系统镜像内核区的尾部**（见 icons64.h 顶部的"为什么放这儿"），内核二进制里**不含任何图标字节**：
//     图标数量/尺寸的变化只影响 system.img 里那 512 个扇区（上限 256KB）的包，不影响 4MB 内核区占用。
//   * 所有失败路径都必须让调用方拿到 -1 -> 调用方回落既有程序化绘制（界面不会空）。
#include "icons64.h"

#include "memlayout64.h"     // ML64_KERNEL_LBA / ML64_KERNEL_SECTORS（图标包 LBA 由这两个算出来）
#include "img64.h"           // 解码 PNG（本批的包都是 PNG）
#include "fb.h"              // fb_putpixel / fb_get_pixel / fb_width / fb_height / rgb
#include "debug64.h"         // 串口打点
#include "mem_64.h"          // kmalloc_64 / kfree_64 + 内存归属
#include "ata64.h"           // ata64_read（统一驱动器号）
#include "vfs64.h"           // 可选覆盖：系统卷里的 /icons/**
#include "drive64.h"         // 系统盘（C:）的驱动器号：优先从它读包

// 图标包在磁盘上的起点：内核区**尾部固定区间**（不随包大小漂移）
#define ICON64_PACK_LBA ((uint32_t)(ML64_KERNEL_LBA + ML64_KERNEL_SECTORS) - ICON64_PACK_MAX_SECTORS)

#define ICON64_MAX_ENTRIES   256
#define ICON64_CACHE_MAX     64
#define ICON64_ENTRY_BYTES   32
#define ICON64_HDR_BYTES     24

// 标识"条目 CRC 校验失败"的内部标记位（不写回磁盘；只影响加载时的回落）
#define ICON64_EFLAG_BAD     0x80000000u

static uint8_t  s_hdr[512];                       // 包头扇区（先读一扇确认 magic，再读整包）

// ==================== 图标名表（kind -> 名字 + 子目录；与 tools/make_iconpack.py 的 KINDS 一一对应）====================
struct Icon64Name64 { int kind; const char* name; const char* sub; };
static const Icon64Name64 kIcon64Names[] = {
    { ICON64_K_GLOBE,       "globe",         "system" }, { ICON64_K_ETHERNET,   "ethernet",      "system" },
    { ICON64_K_WIFI,        "wifi",          "system" }, { ICON64_K_VOLUME,     "volume",        "system" },
    { ICON64_K_VOLUME_MUTE, "volume-mute",   "system" }, { ICON64_K_BELL,       "bell",          "system" },
    { ICON64_K_SEARCH,      "search",        "system" }, { ICON64_K_CLOSE,      "close",         "system" },
    { ICON64_K_POWER,       "power",         "system" }, { ICON64_K_GEAR,       "gear",          "system" },
    { ICON64_K_LOCK,        "lock",          "system" }, { ICON64_K_REBOOT,     "reboot",        "system" },
    { ICON64_K_CHEV_L,      "chevron-left",  "system" }, { ICON64_K_CHEV_R,     "chevron-right", "system" },
    { ICON64_K_PLUS,        "plus",          "system" }, { ICON64_K_PERSON,     "person",        "system" },
    { ICON64_K_USB,         "usb",           "system" }, { ICON64_K_MONITOR,    "monitor",       "system" },
    { ICON64_K_CHECK,       "check",         "system" }, { ICON64_K_BATTERY,    "battery",       "system" },
    { ICON64_A_MYPC,        "mypc",          "apps"   }, { ICON64_A_RECYCLE,    "recycle",       "apps"   },
    { ICON64_A_TERMINAL,    "terminal",      "apps"   }, { ICON64_A_SETTINGS,   "settings",      "apps"   },
    { ICON64_A_TMGR,        "tmgr",          "apps"   }, { ICON64_A_MINES,      "mines",         "apps"   },
    { ICON64_A_CALC,        "calc",          "apps"   }, { ICON64_A_FILES,      "files",         "apps"   },
    { ICON64_A_MONITOR,     "monitor-app",   "apps"   }, { ICON64_A_ABOUT,      "about",         "apps"   },
};
#define ICON64_NAME_N ((int)(sizeof(kIcon64Names) / sizeof(kIcon64Names[0])))

// ==================== 状态 ====================
struct Icon64Entry { uint32_t kind, size, off, len, crc, flags, w, h; };
static Icon64Entry g_e[ICON64_MAX_ENTRIES];
static int         g_n = 0;                 // 条目数
static int         g_kinds = 0;             // 不同 kind 数
static uint8_t*    g_pack = nullptr;        // 整包（kmalloc；不释放 —— 系统级资源）
static uint32_t    g_pack_bytes = 0;
static uint32_t    g_pack_fnv = 0;
static int         g_pack_drive = -1;
static uint32_t    g_pack_lba = ICON64_PACK_LBA;
static bool        g_tried = false;
static bool        g_vfs_icons = false;     // 系统卷里存在 /icons（则 VFS 里的 PNG 优先）

// 位图缓存：(kind, 实际取的尺寸) -> 解码结果
struct Icon64CacheEntry { int used; uint32_t kind; uint32_t size; Img64 im; };
static Icon64CacheEntry g_cache[ICON64_CACHE_MAX];
static Icon64CacheEntry g_cache_overflow;    // 缓存满时的兜底槽（下一次覆盖，只保证不越界）
static int g_cache_n = 0;

// 打点预算（防刷屏）
static int g_load_budget = 240;
static int g_fb_budget = 96;
static uint8_t g_fb_logged[256];            // kind -> 已打过回落点（kind <= 255）
static uint32_t g_loads = 0, g_fallbacks = 0;

// ==================== 小工具（内核里没有 libc）====================
static int ic_strlen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static void ic_strcpy(char* d, const char* s, int cap) {
    int i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static void ic_strcat(char* d, const char* s, int cap) {
    int i = ic_strlen(d), k = 0;
    while (s && s[k] && i < cap - 1) d[i++] = s[k++];
    d[i] = 0;
}
// 十进制进 buf（不用 dbg64_dec：这里是要拼进字符串里的）
static void ic_dec(char* b, int v, int cap) {
    char t[16];
    int n = 0, k = 0;
    if (v <= 0) t[n++] = '0';
    while (v > 0 && n < 15) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0 && k < cap - 1) b[k++] = t[--n];
    b[k] = 0;
}
static void ic_put_dec(int v) { if (v < 0) { dbg64_str("-"); v = -v; } dbg64_dec((uint64_t)v); }
static void ic_put_hex8(uint32_t v) {
    static const char* H = "0123456789abcdef";
    char b[9];
    for (int i = 0; i < 8; i++) b[i] = H[(v >> ((7 - i) * 4)) & 0xF];
    b[8] = 0;
    dbg64_str(b);
}
static uint32_t ic_rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t ic_crc32(const uint8_t* p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
static uint32_t ic_fnv1a(const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static const Icon64Name64* ic_name(int kind) {
    for (int i = 0; i < ICON64_NAME_N; i++) if (kIcon64Names[i].kind == kind) return &kIcon64Names[i];
    return nullptr;
}

// ==================== 打点 ====================
static void ic_log_load(int kind, const char* path, int size, const char* src) {
    if (g_load_budget <= 0) return;
    g_load_budget--;
    g_loads++;
    dbg64_line_begin64();
    dbg64_str("[ICON64] load kind=");
    dbg64_str(icons64_kind_name64(kind));
    dbg64_str(" path=");
    dbg64_str(path);
    dbg64_str(" size=");
    ic_put_dec(size);
    dbg64_str(" src=");
    dbg64_str(src);
    dbg64_str(" ok=1");
    dbg64_nl();
    dbg64_line_end64();
}
static void ic_log_fallback(int kind, const char* reason) {
    g_fallbacks++;
    const int idx = (kind >= 0 && kind < 256) ? kind : 255;
    if (g_fb_logged[idx] || g_fb_budget <= 0) return;      // 同一 kind 只打一次（防刷屏）
    g_fb_logged[idx] = 1;
    g_fb_budget--;
    dbg64_line_begin64();
    dbg64_str("[ICON64] fallback kind=");
    dbg64_str(icons64_kind_name64(kind));
    dbg64_str(" reason=");
    dbg64_str(reason);
    dbg64_str(" (programmatic draw kept)");
    dbg64_nl();
    dbg64_line_end64();
}

// ==================== 包加载 ====================
static bool ic_pack_ok(const uint8_t* p, uint32_t n) {
    static const char M[ICON64_PACK_MAGIC_LEN + 1] = "VIMTUI01";
    if (n < ICON64_HDR_BYTES) return false;
    for (uint32_t i = 0; i < ICON64_PACK_MAGIC_LEN; i++) if (p[i] != (uint8_t)M[i]) return false;
    return ic_rd32(p + 8) == 1u;                       // version
}

static int ic_parse_pack(uint8_t* p, uint32_t n, int* out_bad) {
    const uint32_t entries = ic_rd32(p + 12);
    const uint32_t total = ic_rd32(p + 16);
    if (entries == 0 || entries > ICON64_MAX_ENTRIES) return -1;
    if (total < ICON64_HDR_BYTES + ICON64_ENTRY_BYTES * entries || total > n) return -1;
    g_n = (int)entries;
    g_pack_bytes = total;
    int bad = 0;
    const uint32_t base = ICON64_HDR_BYTES + ICON64_ENTRY_BYTES * entries;
    for (uint32_t i = 0; i < entries; i++) {
        const uint8_t* e = p + ICON64_HDR_BYTES + i * ICON64_ENTRY_BYTES;
        Icon64Entry& t = g_e[i];
        t.kind = ic_rd32(e + 0);
        t.size = ic_rd32(e + 4);
        t.off = ic_rd32(e + 8);
        t.len = ic_rd32(e + 12);
        t.crc = ic_rd32(e + 16);
        t.flags = ic_rd32(e + 20);
        t.w = ic_rd32(e + 24);
        t.h = ic_rd32(e + 28);
        if (t.size == 0 || t.w == 0 || t.h == 0 || t.len == 0 || t.len > ICON64_BLOB_MAX_BYTES ||
            t.off < base || t.off + t.len > total) {
            t.flags |= ICON64_EFLAG_BAD;               // 越界条目：标记坏，绝不越读
            bad++;
            continue;
        }
        if (ic_crc32(p + t.off, t.len) != t.crc) { t.flags |= ICON64_EFLAG_BAD; bad++; }
    }
    if (out_bad) *out_bad = bad;
    // 统计不同 kind（与名称表交集）
    g_kinds = 0;
    for (int i = 0; i < ICON64_NAME_N; i++) {
        for (int j = 0; j < g_n; j++) {
            if (g_e[j].kind == (uint32_t)kIcon64Names[i].kind) { g_kinds++; break; }
        }
    }
    g_pack_fnv = ic_fnv1a(p, total);
    return 0;
}

// 前向声明（init 里预热要用到，定义在下面）
static const Img64* ic_bitmap(int kind, int want, const char** why);

// 候选盘：系统盘（C: 所在驱动器）优先，其次 0 / 8 / 16 / 24（PATA / AHCI / NVMe / USB 的首盘）
static int ic_candidate_drives(int* out, int cap) {
    int n = 0;
    const int ci = drive64_by_letter64('C');
    if (ci >= 0 && n < cap) {
        DriveInfo64 info{};
        if (drive64_info64(ci, &info) == 0 && info.disk >= 0) out[n++] = info.disk;
    }
    static const int extra[4] = { 0, 8, 16, 24 };
    for (int i = 0; i < 4 && n < cap; i++) {
        bool dup = false;
        for (int k = 0; k < n; k++) if (out[k] == extra[i]) { dup = true; break; }
        if (!dup) out[n++] = extra[i];
    }
    return n;
}

int icons64_init64() {
    if (g_tried) return g_n;
    g_tried = true;
    // ---- 0) 可选覆盖源的探测：系统卷里有没有 /icons 目录（没有就不做任何 VFS 探测，
    //         免得每个图标都留一行 not-found 噪声）----
    if (vfs64_system_slot64() >= 0) {
        Vfs64Info64 si{};
        if (vfs64_stat64("/icons", &si) == 0) g_vfs_icons = true;
    }
    // ---- 1) 逐盘找图标包（正常的系统盘 = C: 所在盘，退而求其次试 0/8/16/24）----
    int cand[5];
    int nc = ic_candidate_drives(cand, 5);
    int tried = 0, last = -1;
    for (int i = 0; i < nc; i++) {
        const int d = cand[i];
        tried++;
        last = d;
        if (!ata64_read(d, ICON64_PACK_LBA, 1u, s_hdr)) continue;
        if (!ic_pack_ok(s_hdr, sizeof(s_hdr))) continue;
        const uint32_t total = ic_rd32(s_hdr + 16);
        if (total < ICON64_HDR_BYTES || total > ICON64_PACK_MAX_BYTES) continue;
        // ★ 整扇区读：磁盘读永远是 512 的整数倍，缓冲区必须按**扇区对齐后的大小**分配，
        //   否则 ata64_read 会写超出 total（包尾那不到 512 字节）—— 会踩坏堆。
        const uint32_t sectors = (total + 511u) / 512u;
        const uint32_t alloc = sectors * 512u;
        uint8_t* buf = (uint8_t*)kmalloc_64(alloc);
        if (!buf) {                                       // 内存不足：如实打点，之后全走回落
            dbg64_line_begin64();
            dbg64_str("[ICON64] init pack alloc FAILED bytes=");
            dbg64_dec(alloc);
            dbg64_nl();
            dbg64_line_end64();
            break;
        }
        mem_owner_set_64(MEM_OWNER_GUI_64);
        if (!ata64_read(d, ICON64_PACK_LBA, sectors, buf) || !ic_pack_ok(buf, total)) {
            kfree_64(buf);
            continue;
        }
        int bad = 0;
        if (ic_parse_pack(buf, total, &bad) != 0) {
            kfree_64(buf);
            continue;
        }
        g_pack = buf;
        g_pack_drive = d;
        dbg64_line_begin64();
        dbg64_str("[ICON64] init pack lba=");
        dbg64_dec((uint64_t)g_pack_lba);
        dbg64_str(" drive=");
        ic_put_dec(d);
        dbg64_str(" bytes=");
        dbg64_dec((uint64_t)g_pack_bytes);
        dbg64_str(" entries=");
        dbg64_dec((uint64_t)g_n);
        dbg64_str(" icons=");
        dbg64_dec((uint64_t)g_kinds);
        dbg64_str(" bad=");
        dbg64_dec((uint64_t)bad);
        dbg64_str(" ok=");
        dbg64_dec(bad == 0 ? 1 : 0);
        dbg64_str(" fnv=");
        ic_put_hex8(g_pack_fnv);
        dbg64_str(" vfs_icons=");
        dbg64_dec(g_vfs_icons ? 1 : 0);
        dbg64_nl();
        dbg64_line_end64();
        break;
    }
    if (!g_pack) {
        dbg64_line_begin64();
        dbg64_str("[ICON64] init pack absent reason=no-magic-or-read lba=");
        dbg64_dec((uint64_t)g_pack_lba);
        dbg64_str(" drives-tried=");
        ic_put_dec(tried);
        dbg64_str(" last-drive=");
        ic_put_dec(last);
        dbg64_str(" -> programmatic fallback stays");
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    // ---- 2) 预解码"界面必用"的那一档：系统图标 24px（状态区 22 / 面板 20~26 / 网格 22 都由它缩小），
    //         应用图标 48px（Dock 46 / 桌面 40 由它缩小）。这样每个 kind 都有一条 [ICON64] load 证据，
    //         而且首帧绘制路径里不做解码。----
    for (int i = 0; i < ICON64_NAME_N; i++) {
        const int kind = kIcon64Names[i].kind;
        const int want = (kind >= ICON64_A_MYPC) ? 48 : 24;
        (void)ic_bitmap(kind, want, nullptr);
    }
    icons64_dump64();
    return g_n;
}

int icons64_available64() { if (!g_tried) (void)icons64_init64(); return g_pack ? 1 : 0; }
int icons64_kind_count64() { if (!g_tried) (void)icons64_init64(); return g_kinds; }
const char* icons64_kind_name64(int kind) {
    const Icon64Name64* n = ic_name(kind);
    return n ? n->name : "unknown";
}

// ==================== 位图加载 + 缓存 ====================
// 选尺寸：优先"不小于请求的最小档"（缩放只做缩小，最清晰）；都比请求小就取最大档。
static const Icon64Entry* ic_pick(int kind, int want) {
    const Icon64Entry* best = nullptr;
    for (int i = 0; i < g_n; i++) {
        if (g_e[i].kind != (uint32_t)kind) continue;
        if (g_e[i].flags & ICON64_EFLAG_BAD) continue;
        if (!best) { best = &g_e[i]; continue; }
        const bool b_short = best->size < (uint32_t)want;
        const bool c_short = g_e[i].size < (uint32_t)want;
        if (b_short && !c_short) best = &g_e[i];                                              // 换成"够大"的
        else if (b_short == c_short && !b_short && g_e[i].size < best->size) best = &g_e[i];   // 都够大：取更小
        else if (b_short == c_short && b_short && g_e[i].size > best->size) best = &g_e[i];    // 都不够：取更大
    }
    return best;
}

static Icon64CacheEntry* ic_cache_find(int kind, uint32_t size) {
    for (int i = 0; i < g_cache_n; i++)
        if (g_cache[i].used && g_cache[i].kind == (uint32_t)kind && g_cache[i].size == size) return &g_cache[i];
    return nullptr;
}

static const Img64* ic_bitmap(int kind, int want, const char** why) {
    // 懒初始化：任何一次图标请求（包括 preload64 的图标预热 —— 它早于 gui64_run）都先把包读进来。
    // 幂等：g_tried 在 init 一开头就置位，init 内部再调本函数不会递归。
    if (!g_tried) (void)icons64_init64();
    if (why) *why = "no-pack";
    if (!g_pack) { ic_log_fallback(kind, "no-pack"); return nullptr; }
    const Icon64Entry* e = ic_pick(kind, want);
    if (!e) { if (why) *why = "no-entry"; ic_log_fallback(kind, "no-entry"); return nullptr; }
    if (Icon64CacheEntry* c = ic_cache_find(kind, e->size)) return &c->im;

    const Icon64Name64* nm = ic_name(kind);
    char path[96];
    char vpath[96];
    vpath[0] = 0;
    Img64 im{};
    bool via_vfs = false;
    // 1) VFS 覆盖（可选）：/icons/<sub>/<name>@<size>.png
    if (g_vfs_icons && nm) {
        ic_strcpy(vpath, "/icons/", (int)sizeof(vpath));
        ic_strcat(vpath, nm->sub, (int)sizeof(vpath));
        ic_strcat(vpath, "/", (int)sizeof(vpath));
        ic_strcat(vpath, nm->name, (int)sizeof(vpath));
        ic_strcat(vpath, "@", (int)sizeof(vpath));
        char sz[12];
        ic_dec(sz, (int)e->size, (int)sizeof(sz));
        ic_strcat(vpath, sz, (int)sizeof(vpath));
        ic_strcat(vpath, ".png", (int)sizeof(vpath));
        if (img64_load_vfs64(vpath, &im) == 0) via_vfs = true;
    }
    // 2) 图标包
    if (!via_vfs) {
        if (e->flags & ICON64_EFLAG_BAD) {
            if (why) *why = "crc-fail";
            ic_log_fallback(kind, "crc-fail");
            return nullptr;
        }
        mem_owner_set_64(MEM_OWNER_GUI_64);
        if (img64_decode64(g_pack + e->off, (int)e->len, &im) != 0) {
            if (why) *why = "decode-fail";
            ic_log_fallback(kind, "decode-fail");
            return nullptr;
        }
        if ((uint32_t)im.w != e->w || (uint32_t)im.h != e->h) {          // 尺寸不符：如实拒绝
            img64_free64(&im);
            if (why) *why = "size-mismatch";
            ic_log_fallback(kind, "size-mismatch");
            return nullptr;
        }
    }
    // 缓存（有上限；满了用兜底槽，照画不误）
    Icon64CacheEntry* slot = nullptr;
    for (int i = 0; i < ICON64_CACHE_MAX; i++) {
        if (!g_cache[i].used) { slot = &g_cache[i]; if (i >= g_cache_n) g_cache_n = i + 1; break; }
    }
    if (!slot) slot = &g_cache_overflow;
    slot->used = 1;
    slot->kind = (uint32_t)kind;
    slot->size = e->size;
    slot->im = im;

    if (via_vfs) {
        ic_log_load(kind, vpath, (int)e->size, "vfs");
    } else {
        ic_strcpy(path, "pack:", (int)sizeof(path));
        ic_strcat(path, nm ? nm->sub : "?", (int)sizeof(path));
        ic_strcat(path, "/", (int)sizeof(path));
        ic_strcat(path, nm ? nm->name : "?", (int)sizeof(path));
        ic_strcat(path, "@", (int)sizeof(path));
        char sz[12];
        ic_dec(sz, (int)e->size, (int)sizeof(sz));
        ic_strcat(path, sz, (int)sizeof(path));
        ic_strcat(path, ".png", (int)sizeof(path));
        ic_log_load(kind, path, (int)e->size, "pack");
    }
    if (why) *why = nullptr;
    return &slot->im;
}

// ==================== 绘制 ====================
int icons64_handle64(int kind) {
    if (!g_tried) (void)icons64_init64();
    if (!g_pack) return -1;
    for (int i = 0; i < g_n; i++) if (g_e[i].kind == (uint32_t)kind) return i + 1;
    return -1;
}

int icons64_draw_a64(int h, int x, int y, int size, uint32_t palette, int alpha) {
    int kind = -1;
    if (h >= 1 && h <= g_n) kind = (int)g_e[h - 1].kind;
    if (kind < 0) return -1;
    if (size <= 0 || alpha <= 0) return -1;
    const Img64* im = ic_bitmap(kind, size, nullptr);
    if (!im) return -1;
    const uint32_t col = palette & 0xFFFFFFu;
    for (int j = 0; j < size; j++) {
        const int sy = (int)((int64_t)j * im->h / size);
        for (int i = 0; i < size; i++) {
            const int sx = (int)((int64_t)i * im->w / size);
            const uint32_t c = im->px[(uint64_t)sy * (uint64_t)im->w + (uint64_t)sx];
            int a = (int)((c >> 24) & 0xFFu);
            if (a == 0) continue;
            if (alpha < 255) a = a * alpha / 255;
            if (a <= 0) continue;
            const int px = x + i, py = y + j;
            if (px < 0 || py < 0 || px >= fb_width() || py >= fb_height()) continue;
            uint32_t out;
            if (a >= 252) {
                out = 0xFF000000u | col;
            } else {
                const uint32_t bg = fb_get_pixel(px, py);
                const uint32_t r = (((col >> 16) & 0xFFu) * (uint32_t)a + ((bg >> 16) & 0xFFu) * (uint32_t)(255 - a)) / 255u;
                const uint32_t g = (((col >> 8) & 0xFFu) * (uint32_t)a + ((bg >> 8) & 0xFFu) * (uint32_t)(255 - a)) / 255u;
                const uint32_t b = ((col & 0xFFu) * (uint32_t)a + (bg & 0xFFu) * (uint32_t)(255 - a)) / 255u;
                out = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
            fb_putpixel(px, py, out);
        }
    }
    return 0;
}

int icons64_draw64(int h, int x, int y, int size, uint32_t palette) {
    return icons64_draw_a64(h, x, y, size, palette, 255);
}

int icons64_export_rgba64(int kind, int want, uint8_t* dst, int dw, int dh, uint32_t palette) {
    if (!dst || dw <= 0 || dh <= 0) return -1;
    const Img64* im = ic_bitmap(kind, want, nullptr);
    if (!im) return -1;
    const uint8_t r = (uint8_t)((palette >> 16) & 0xFFu);
    const uint8_t g = (uint8_t)((palette >> 8) & 0xFFu);
    const uint8_t b = (uint8_t)(palette & 0xFFu);
    for (int j = 0; j < dh; j++) {
        const int sy = (int)((int64_t)j * im->h / dh);
        for (int i = 0; i < dw; i++) {
            const int sx = (int)((int64_t)i * im->w / dw);
            const uint32_t c = im->px[(uint64_t)sy * (uint64_t)im->w + (uint64_t)sx];
            uint8_t* q = dst + (((size_t)j * (size_t)dw) + (size_t)i) * 4u;
            q[0] = r; q[1] = g; q[2] = b;
            q[3] = (uint8_t)((c >> 24) & 0xFFu);
        }
    }
    return 0;
}

int icons64_draw_kind64(int kind, int x, int y, int size, uint32_t palette, int alpha) {
    const int h = icons64_handle64(kind);
    if (h < 0) { ic_log_fallback(kind, "no-pack"); return -1; }
    return icons64_draw_a64(h, x, y, size, palette, alpha);
}

uint32_t icons64_app_color64(int kind) {
    switch (kind) {
        case ICON64_A_MYPC:     return rgb(0x7F, 0xD1, 0xFF);   // 淡蓝
        case ICON64_A_RECYCLE:  return rgb(0x86, 0xE2, 0xC0);   // 薄荷
        case ICON64_A_TERMINAL: return rgb(0x9C, 0xE3, 0x8A);   // 草绿
        case ICON64_A_SETTINGS: return rgb(0xC3, 0xD0, 0xFF);   // 淡紫蓝
        case ICON64_A_TMGR:     return rgb(0xFF, 0xC9, 0x78);   // 琥珀
        case ICON64_A_MINES:    return rgb(0xFF, 0x9E, 0x9E);   // 淡红
        case ICON64_A_CALC:     return rgb(0x8F, 0xE9, 0xE0);   // 青
        case ICON64_A_FILES:    return rgb(0xFF, 0xD9, 0x8F);   // 沙金
        case ICON64_A_MONITOR:  return rgb(0x9F, 0xD8, 0xFF);   // 天蓝
        case ICON64_A_ABOUT:    return rgb(0xD6, 0xC4, 0xFF);   // 淡紫
        default:                return 0xFFFFFFu;
    }
}

// ==================== 自检 / 统计 ====================
int icons64_selftest64() {
    int mask = 0;
    if (!g_tried) (void)icons64_init64();
    if (g_pack) {
        if (g_n <= 0 || g_kinds <= 0) mask |= 1;                        // bit0：包内容自洽
        for (int i = 0; i < g_n; i++) {
            if (g_e[i].flags & ICON64_EFLAG_BAD) { mask |= 2; break; }  // bit1：没有坏条目
        }
        if (!ic_cache_find(ICON64_K_ETHERNET, 24) && !ic_cache_find(ICON64_K_GLOBE, 24)) mask |= 4;
        if (icons64_handle64(ICON64_A_TERMINAL) <= 0) mask |= 8;        // bit3：核心 kind 可查到
    } else {
        // 没有包不算失败：只要求"回落路径可用"（取不到就必须是 -1，不崩、不画半个）
        if (icons64_handle64(ICON64_K_ETHERNET) != -1) mask |= 16;
        if (icons64_draw_kind64(ICON64_K_ETHERNET, 0, 0, 24, 0xFFFFFFu, 255) != -1) mask |= 32;
    }
    dbg64_line_begin64();
    dbg64_str("[ICON64] selftest ");
    dbg64_str(mask == 0 ? "PASS" : "FAIL");
    dbg64_str(" mask=");
    dbg64_dec((uint64_t)mask);
    dbg64_str(" pack=");
    dbg64_dec(g_pack ? 1 : 0);
    dbg64_str(" kinds=");
    dbg64_dec((uint64_t)g_kinds);
    dbg64_str(" loaded=");
    dbg64_dec((uint64_t)g_loads);
    dbg64_str(" fallback=");
    dbg64_dec((uint64_t)g_fallbacks);
    dbg64_nl();
    dbg64_line_end64();
    return mask;
}

void icons64_dump64() {
    int budget = 200;
    for (int i = 0; i < ICON64_NAME_N && budget > 0; i++) {
        const int kind = kIcon64Names[i].kind;
        const int h = icons64_handle64(kind);
        int cached = 0, bad = 0, sizes = 0, best = 0;
        for (int j = 0; j < g_n; j++) {
            if (g_e[j].kind != (uint32_t)kind) continue;
            sizes++;
            if (g_e[j].flags & ICON64_EFLAG_BAD) bad++;
            if (!best || g_e[j].size < (uint32_t)best) best = (int)g_e[j].size;
        }
        for (int c = 0; c < g_cache_n; c++) if (g_cache[c].used && g_cache[c].kind == (uint32_t)kind) cached++;
        budget--;
        dbg64_line_begin64();
        dbg64_str("[ICON64] dump kind=");
        dbg64_str(kIcon64Names[i].name);
        dbg64_str(" sub=");
        dbg64_str(kIcon64Names[i].sub);
        dbg64_str(" handle=");
        ic_put_dec(h);
        dbg64_str(" sizes=");
        ic_put_dec(sizes);
        dbg64_str(" min=");
        ic_put_dec(best);
        dbg64_str(" cached=");
        ic_put_dec(cached);
        dbg64_str(" bad=");
        ic_put_dec(bad);
        dbg64_nl();
        dbg64_line_end64();
    }
}
