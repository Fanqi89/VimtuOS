// config64.cpp - 64 位系统配置实现（默认表 + store64 覆盖表 + 类型化取值）
//
// 数据流（一句话）：
//   store64（VimtuFS2 的 /store.a、/store.b 双槽 + CRC32 + 世代号）
//        ^  |
//        |  +-- config64_set_*() 写 "cfg.<key>" 字符串值（内存里同时更新覆盖表）
//        +------ config64_init64() 读所有 "cfg." 前缀的键 -> 覆盖表
//   取值顺序：覆盖表（store 里被改过的）-> 编译期默认表 kDefs[] -> 调用方给的默认值。
//
// 为什么值在 store 里统一存字符串（而不是二进制）：
//   store64 的 payload 是 KV 记录，天然是"名字 + 字节串"；类型信息只在内核里（默认表 + 自动判型），
//   这样既保持了 store64 的格式不动（它有自己的验收断言），又让 `store dump` 里的人眼可读。
#include "config64.h"
#include "store64.h"      // 真落盘载体（VFS 文件 /store.a|b，无卷时降级裸盘槽）
#include "theme64.h"     // 本批新增键的取值范围/默认值直接取 Token（dock.size/dock.icon/dock.gap）
#include "debug64.h"      // dbg64_* + 行锁
#include "x86_64.h"       // ticks64 / ms_to_ticks64

// ==================== 小工具（内核里没有 libc）====================
static bool c_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static bool c_starts(const char* s, const char* pre) {
    while (*pre) { if (*s++ != *pre++) return false; }
    return true;
}
static int c_len(const char* s) { int n = 0; if (!s) return 0; while (s[n]) n++; return n; }
static void c_copy(char* d, const char* s, int cap) {
    int i = 0;
    if (!d || cap <= 0) return;
    if (!s) s = "";
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
// 十进制解析（可带前导 '-'）：成功返回 0
static int parse_dec(const char* s, int32_t* out) {
    if (!s || !out) return -1;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    if (*s < '0' || *s > '9') return -1;
    int32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        if (v > 100000000) return -1;             // 溢出防护（配置里不会有这么大的数）
        v = v * 10 + (*s - '0');
        s++;
    }
    if (*s) return -1;                            // 尾部还有非数字 = 不是纯整数
    *out = v * sign;
    return 0;
}
// "1/0"、"true/false"、"on/off" -> 0/1；返回 -1 = 不像布尔
static int parse_bool(const char* s) {
    if (!s || !s[0]) return -1;
    if (c_eq(s, "1") || c_eq(s, "true") || c_eq(s, "on") || c_eq(s, "yes") ||
        c_eq(s, "zh") || c_eq(s, "en")) return c_eq(s, "en") ? 0 : 1;
    if (c_eq(s, "0") || c_eq(s, "false") || c_eq(s, "off") || c_eq(s, "no")) return 0;
    return -1;
}
static int fmt_dec(char* out, int cap, int32_t v) {
    char tmp[12]; int n = 0;
    bool neg = v < 0;
    uint32_t u = neg ? (uint32_t)(-v) : (uint32_t)v;
    if (u == 0) tmp[n++] = '0';
    while (u > 0) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
    int o = 0;
    if (neg && o < cap - 1) out[o++] = '-';
    while (n > 0 && o < cap - 1) out[o++] = tmp[--n];
    out[o] = 0;
    return o;
}

// ==================== 编译期默认表 ====================
struct Cfg64Def {
    const char* key;
    int         type;      // CFG64_T_*
    int32_t     ival;      // INT / BOOL 的值
    const char* sval;      // STR 的值
};
// 与 32 位 config.cpp 的 config_factory_reset() 语义对齐，并按 64 位现状补齐：
//   lang / text_mirror / mouse.sens / icon.x*/y* / win.* / startup.* 都是 32 位有的键；
//   display.zoom 32 位也有；session.mode / sess.restore 由 session64 读写；
//   store.persist 保留（64 位实际的"是否落盘"由 store64 自己探测载体决定，见 store64.h）。
static const Cfg64Def kDefs[] = {
    { "ui.lang",           CFG64_T_INT,  1,    nullptr },   // 0=英文 1=简体中文（gui64 默认 zh）
    { "ui.text_mirror",    CFG64_T_INT,  0,    nullptr },   // 0=正常 1=外壳文本水平镜像
    { "mouse.sens",        CFG64_T_INT,  1700, nullptr },   // 千分比；1700 = 驱动基线
    { "display.zoom",      CFG64_T_INT,  100,  nullptr },   // 100 / 125 / 150
    { "icon.x0",           CFG64_T_INT,  24,   nullptr },
    { "icon.y0",           CFG64_T_INT,  24,   nullptr },
    { "icon.x1",           CFG64_T_INT,  24,   nullptr },
    { "icon.y1",           CFG64_T_INT,  108,  nullptr },   // 24 + ICON_CELL_H(84)
    { "icon.x2",           CFG64_T_INT,  24,   nullptr },
    { "icon.y2",           CFG64_T_INT,  192,  nullptr },   // 24 + 2*84
    { "win.term.w",        CFG64_T_INT,  974,  nullptr },   // 终端客户区 60x40 格 + 边框
    { "win.term.h",        CFG64_T_INT,  678,  nullptr },
    { "startup.terminal",  CFG64_T_INT,  0,    nullptr },   // 启动时自动开终端
    { "startup.monitor",   CFG64_T_INT,  0,    nullptr },   // 启动时自动开系统监视器
    { "startup.desktop",   CFG64_T_INT,  1,    nullptr },   // 桌面图标服务
    { "startup.health",    CFG64_T_INT,  0,    nullptr },   // 启动时打一次健康报告
    { "session.mode",      CFG64_T_INT,  0,    nullptr },   // 0=VOLATILE 1=PERSIST（session64）
    { "sess.restore",      CFG64_T_STR,  0,    ""        }, // 会话恢复列表 "3,2"（应用 id）
    { "store.persist",     CFG64_T_INT,  1,    nullptr },
    { "boot.verbose",      CFG64_T_BOOL, 1,    nullptr },   // ★ 批次 N：开机滚屏引导控制台（1=显示，0=跳过）
    // ★ 本批（Windows 11 现代外观）：主题 / 壁纸 / Dock / 减少动画
    { "ui.theme",          CFG64_T_INT,  0,    nullptr },   // 界面主题 id（0 白=默认 … 5 粉紫；theme64.h）
    { "ui.reduce_motion",  CFG64_T_BOOL, 0,    nullptr },   // 1 = 减少动画（动效全部 0ms/1 帧到位）
    { "ui.wall.mode",      CFG64_T_INT,  0,    nullptr },   // 桌面壁纸适应模式（0 填充=默认）
    { "ui.wall.lock_mode", CFG64_T_INT,  0,    nullptr },   // 锁屏壁纸适应模式（下一波锁屏用）
    { "ui.wall.path",      CFG64_T_STR,  0,    ""        }, // 壁纸文件路径（VimtuFS2 内；空=内置兜底）
    { "ui.avatar.path",    CFG64_T_STR,  0,    ""        }, // 头像文件路径（下一波用）
    { "dock.len",          CFG64_T_INT,  0,    nullptr },   // Dock 长度（0 = 自动）
    { "dock.size",         CFG64_T_INT,  60,   nullptr },   // Dock 高（Token 60）
    { "dock.icon",         CFG64_T_INT,  46,   nullptr },   // 图标边长（Token 44–48 → 46）
    { "dock.gap",          CFG64_T_INT,  11,   nullptr },   // 图标间距（Token 10–12 → 11）
    // ★ P3（Windows 11 风格设置应用）：字体大小档 / 壁纸同步 / 自定义渐变 / 声音 / 默认应用
    { "ui.font.size",      CFG64_T_INT,  16,   nullptr },   // 渲染字号档（14/16/18；= font.cpp 的 em 像素高）
    { "ui.wall.sync",      CFG64_T_BOOL, 1,    nullptr },   // 桌面/锁屏壁纸适应模式同步（1=默认同步）
    { "ui.grad.on",        CFG64_T_BOOL, 0,    nullptr },   // 自定义渐变壁纸开关（0=不用）
    { "ui.grad.a",         CFG64_T_STR,  0,    "#5AA9F0" }, // 自定义渐变起点（#RRGGBB）
    { "ui.grad.b",         CFG64_T_STR,  0,    "#C9A7FF" }, // 自定义渐变终点（#RRGGBB）
    { "ui.sound.volume",   CFG64_T_INT,  42,   nullptr },   // 音量（无音频驱动：内存态 + 持久化，不写硬件）
    { "ui.sound.src",      CFG64_T_INT,  0,    nullptr },   // 输出源 0=音箱 1=耳机（同上）
    { "ui.def.elf",        CFG64_T_STR,  0,    "term"    }, // 默认应用：.elf  -> 终端
    { "ui.def.vap",        CFG64_T_STR,  0,    "term"    }, //              .vap  -> 终端
    { "ui.def.txt",        CFG64_T_STR,  0,    "term"    }, //              .txt  -> 终端
    { "ui.def.video",      CFG64_T_STR,  0,    ""        }, //              视频类 -> 无（本系统没有播放器）
    // ★ P5：桌面交互细节（右键菜单/玻璃选择框/回收站/桌面图标集合）
    { "ui.desktop.icons",  CFG64_T_STR,  0,    "0,1,2|"  }, // 桌面集合|回收站集合（"0,1,2|" = 默认三项，兼容旧行为）
    { "ui.explorer.show_system", CFG64_T_INT, 0, nullptr  }, // 0 = 隐藏系统分区（默认）1 = 显示（终端/设置可开）
};
#define CFG64_DEF_N ((int)(sizeof(kDefs) / sizeof(kDefs[0])))
// 启动日志里最多逐条打印多少个默认值（避免刷屏；总数单独打一行）
#define CFG64_DEFAULT_LOG_MAX 12

// ==================== 覆盖表（来自 store64）====================
struct Cfg64Ov {
    bool    used;
    char    key[CFG64_KEY_MAX];
    int     type;
    int32_t ival;
    char    sval[CFG64_STR_MAX];
};
static Cfg64Ov  g_ov[CFG64_MAX_ENTRIES];
static int      g_ov_n = 0;
static bool     g_inited = false;
static bool     g_pending = false;
static uint32_t g_last_change = 0;
static int      g_loaded_from_store = 0;   // 从 store64 读到的键数（报告用）

static const Cfg64Def* def_find(const char* key) {
    for (int i = 0; i < CFG64_DEF_N; i++) if (c_eq(kDefs[i].key, key)) return &kDefs[i];
    return nullptr;
}
static Cfg64Ov* ov_find(const char* key) {
    for (int i = 0; i < g_ov_n; i++) if (g_ov[i].used && c_eq(g_ov[i].key, key)) return &g_ov[i];
    return nullptr;
}
static Cfg64Ov* ov_alloc(const char* key) {
    Cfg64Ov* e = ov_find(key);
    if (e) return e;
    if (g_ov_n >= CFG64_MAX_ENTRIES) return nullptr;
    e = &g_ov[g_ov_n++];
    e->used = true;
    c_copy(e->key, key, CFG64_KEY_MAX);
    e->type = CFG64_T_INT;
    e->ival = 0;
    e->sval[0] = 0;
    return e;
}

// store64 侧的键名："cfg." + key（store64 键上限 31B -> 我们的键不能超过 27B）
static void store_key(const char* key, char* out, int cap) {
    c_copy(out, "cfg.", cap);
    int n = c_len(out);
    int i = 0;
    while (key[i] && n < cap - 1) out[n++] = key[i++];
    out[n] = 0;
}

// 把覆盖表的一条同步进 store64（字符串值）。返回 0 = 成功。
static int ov_store_sync(const Cfg64Ov* e) {
    char k[CFG64_KEY_MAX + 8];
    char v[CFG64_STR_MAX];
    if (c_len(e->key) > 27) return -1;
    store_key(e->key, k, (int)sizeof(k));
    if (e->type == CFG64_T_STR) c_copy(v, e->sval, sizeof(v));
    else                        fmt_dec(v, (int)sizeof(v), e->ival);
    return store64_set64(k, v);
}

static void mark_change() {
    g_pending = true;
    g_last_change = ticks64();
}

// ==================== 载入 ====================
void config64_init64() {
    if (g_inited) return;
    g_inited = true;
    g_ov_n = 0;
    g_pending = false;
    g_loaded_from_store = 0;

    const int n = store64_key_count64();
    char k[CFG64_KEY_MAX + 8];
    char v[CFG64_STR_MAX];
    // 先数一遍（init 行要报 keys=N），再逐条导入并打印
    for (int i = 0; i < n; i++) {
        if (store64_entry64(i, k, (int)sizeof(k), v, (int)sizeof(v)) != 0) continue;
        if (c_starts(k, "cfg.") && k[4]) g_loaded_from_store++;
    }
    dbg64_line_begin64();
    dbg64_str("[CONF64] init carrier=");
    dbg64_str(store64_carrier64());
    dbg64_str(" slot=");
    dbg64_str(store64_slot_name64());
    dbg64_str(" keys=");
    dbg64_dec((uint64_t)g_loaded_from_store);
    dbg64_str(" defs=");
    dbg64_dec((uint64_t)CFG64_DEF_N);
    dbg64_nl();
    dbg64_line_end64();

    for (int i = 0; i < n; i++) {
        if (store64_entry64(i, k, (int)sizeof(k), v, (int)sizeof(v)) != 0) continue;
        if (!c_starts(k, "cfg.") || !k[4]) continue;
        const char* key = k + 4;
        Cfg64Ov* e = ov_alloc(key);
        if (!e) continue;
        const Cfg64Def* d = def_find(key);
        int32_t iv = 0;
        int bv = parse_bool(v);
        if (d && d->type == CFG64_T_STR) {
            e->type = CFG64_T_STR;
            c_copy(e->sval, v, CFG64_STR_MAX);
        } else if (d && d->type == CFG64_T_BOOL) {
            e->type = CFG64_T_BOOL;
            e->ival = (bv >= 0) ? bv : 0;
        } else if (d && d->type == CFG64_T_INT) {
            e->type = CFG64_T_INT;
            e->ival = (parse_dec(v, &iv) == 0) ? iv : 0;
        } else if (parse_dec(v, &iv) == 0) {          // 默认表里没有的新键（例如 demo=1）
            e->type = CFG64_T_INT;
            e->ival = iv;
        } else if (bv >= 0) {
            e->type = CFG64_T_BOOL;
            e->ival = bv;
        } else {
            e->type = CFG64_T_STR;
            c_copy(e->sval, v, CFG64_STR_MAX);
        }
        dbg64_line_begin64();
        dbg64_str("[CONF64] load ");
        dbg64_str(key);
        dbg64_str("=");
        dbg64_str(v);
        dbg64_str(" type=");
        dbg64_str(config64_type_name64(e->type));
        dbg64_nl();
        dbg64_line_end64();
    }

    // 默认值打点（默认表里、store 里没有的；只打前 CFG64_DEFAULT_LOG_MAX 条）
    int shown = 0, ndef = 0;
    for (int i = 0; i < CFG64_DEF_N; i++) {
        if (ov_find(kDefs[i].key)) continue;
        ndef++;
        if (shown >= CFG64_DEFAULT_LOG_MAX) continue;
        shown++;
        char val[CFG64_STR_MAX];
        if (kDefs[i].type == CFG64_T_STR) c_copy(val, kDefs[i].sval ? kDefs[i].sval : "", sizeof(val));
        else                               fmt_dec(val, (int)sizeof(val), kDefs[i].ival);
        dbg64_line_begin64();
        dbg64_str("[CONF64] default ");
        dbg64_str(kDefs[i].key);
        dbg64_str("=");
        dbg64_str(val);
        dbg64_str(" type=");
        dbg64_str(config64_type_name64(kDefs[i].type));
        dbg64_nl();
        dbg64_line_end64();
    }
    dbg64_line_begin64();
    dbg64_str("[CONF64] defaults n=");
    dbg64_dec((uint64_t)ndef);
    dbg64_str(" shown=");
    dbg64_dec((uint64_t)shown);
    dbg64_nl();
    dbg64_line_end64();
}

// ==================== 取值 ====================
int config64_get_int64(const char* key, int def) {
    if (!key) return def;
    const Cfg64Ov* e = ov_find(key);
    if (e) {
        if (e->type == CFG64_T_STR) { int32_t v = 0; return (parse_dec(e->sval, &v) == 0) ? (int)v : def; }
        return (int)e->ival;
    }
    const Cfg64Def* d = def_find(key);
    if (d) {
        if (d->type == CFG64_T_STR) { int32_t v = 0; return (parse_dec(d->sval, &v) == 0) ? (int)v : def; }
        return (int)d->ival;
    }
    return def;
}

int config64_get_bool64(const char* key, int def) {
    if (!key) return def;
    const Cfg64Ov* e = ov_find(key);
    if (e) {
        if (e->type == CFG64_T_STR) { int b = parse_bool(e->sval); return (b >= 0) ? b : def; }
        return e->ival ? 1 : 0;
    }
    const Cfg64Def* d = def_find(key);
    if (d) {
        if (d->type == CFG64_T_STR) { int b = parse_bool(d->sval); return (b >= 0) ? b : def; }
        return d->ival ? 1 : 0;
    }
    return def;
}

int config64_get_str64(const char* key, const char* def, char* out, int out_max) {
    if (!out || out_max <= 0) return -1;
    c_copy(out, def ? def : "", out_max);
    if (!key) return 0;
    const Cfg64Ov* e = ov_find(key);
    if (e) {
        if (e->type == CFG64_T_STR) c_copy(out, e->sval, out_max);
        else                        fmt_dec(out, out_max, e->ival);
        return 0;
    }
    const Cfg64Def* d = def_find(key);
    if (d) {
        if (d->type == CFG64_T_STR) c_copy(out, d->sval ? d->sval : "", out_max);
        else                        fmt_dec(out, out_max, d->ival);
    }
    return 0;
}

// ==================== 写值 ====================
static int set_common(const char* key, int type, int32_t iv, const char* sv) {
    if (!key || !key[0] || c_len(key) > 27) return -1;
    if (type == CFG64_T_STR && (!sv || c_len(sv) >= CFG64_STR_MAX)) return -1;
    Cfg64Ov* e = ov_alloc(key);
    if (!e) return -1;
    e->type = type;
    e->ival = iv;
    if (type == CFG64_T_STR) c_copy(e->sval, sv, CFG64_STR_MAX);
    else                     e->sval[0] = 0;
    if (ov_store_sync(e) != 0) return -1;    // 同步进 store64 内存表（不落盘）
    mark_change();
    return 0;
}

int config64_set_int64(const char* key, int v)   { return set_common(key, CFG64_T_INT,  v, nullptr); }
int config64_set_bool64(const char* key, int v)  { return set_common(key, CFG64_T_BOOL, v ? 1 : 0, nullptr); }
int config64_set_str64(const char* key, const char* v) { return set_common(key, CFG64_T_STR, 0, v); }

int config64_set_auto64(const char* key, const char* text) {
    if (!key || !key[0] || !text) return -1;
    // 类型判定：默认表里已有的键沿用它的类型；新键按文本内容猜（纯十进制 -> int；布尔词 -> bool）
    const Cfg64Def* d = def_find(key);
    int type = d ? d->type : -1;
    int32_t iv = 0;
    int bv = parse_bool(text);
    if (type < 0) {
        if (c_eq(text, "true") || c_eq(text, "false") || c_eq(text, "on") || c_eq(text, "off") ||
            c_eq(text, "yes") || c_eq(text, "no")) type = CFG64_T_BOOL;
        else if (parse_dec(text, &iv) == 0) type = CFG64_T_INT;
        else type = CFG64_T_STR;
    }
    int rc;
    if (type == CFG64_T_BOOL) {
        int b = (bv >= 0) ? bv : (parse_dec(text, &iv) == 0 ? (iv ? 1 : 0) : 0);
        rc = config64_set_bool64(key, b);
    } else if (type == CFG64_T_INT) {
        if (parse_dec(text, &iv) != 0 && bv >= 0) rc = config64_set_int64(key, bv);
        else if (parse_dec(text, &iv) != 0)       return -1;
        else                                      rc = config64_set_int64(key, (int)iv);
    } else {
        rc = config64_set_str64(key, text);
    }
    if (rc != 0) return rc;
    // 打点：内容 + 类型（自动验收 grep）
    Cfg64Ov* e = ov_find(key);
    dbg64_line_begin64();
    dbg64_str("[CONF64] set key=");
    dbg64_str(key);
    dbg64_str(" value=");
    dbg64_str(text);
    dbg64_str(" type=");
    dbg64_str(config64_type_name64(e ? e->type : type));
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

int config64_del64(const char* key) {
    // store64 没有"删键"API（要删得改 store64.cpp，不在本次范围）。这里的语义是
    // **回默认值**：把覆盖表里的条目重置成默认表的值（新键则置 0/空串），并写回 store。
    // 不假装删除 —— 报告里如实说明这条限制。
    if (!key) return -1;
    const Cfg64Def* d = def_find(key);
    if (d) {
        if (d->type == CFG64_T_STR) return config64_set_str64(key, d->sval ? d->sval : "");
        return config64_set_int64(key, (int)d->ival);
    }
    Cfg64Ov* e = ov_find(key);
    if (!e) return -1;
    return config64_set_int64(key, 0);
}

int config64_factory_reset64() {
    g_ov_n = 0;
    // 恢复出厂：内存里立刻回到默认表；store 里的旧键仍在（store64 无删键 API），
    // 所以这里把默认表的每个键按默认值写回 store，保证"下次启动读到的就是默认值"。
    for (int i = 0; i < CFG64_DEF_N; i++) {
        Cfg64Ov* e = ov_alloc(kDefs[i].key);
        if (!e) break;
        e->type = kDefs[i].type;
        e->ival = kDefs[i].ival;
        if (kDefs[i].type == CFG64_T_STR) c_copy(e->sval, kDefs[i].sval ? kDefs[i].sval : "", CFG64_STR_MAX);
        else                              e->sval[0] = 0;
        ov_store_sync(e);
    }
    // 非默认表里的键（用户自建，如 demo）清零（它们没有"默认值"可言）
    for (int i = 0; i < g_ov_n; i++) {
        if (!g_ov[i].used || def_find(g_ov[i].key)) continue;
        g_ov[i].type = CFG64_T_INT;
        g_ov[i].ival = 0;
        g_ov[i].sval[0] = 0;
        ov_store_sync(&g_ov[i]);
    }
    mark_change();
    dbg64_line_begin64();
    dbg64_str("[CONF64] reset defaults=");
    dbg64_dec((uint64_t)CFG64_DEF_N);
    dbg64_str(" keys=");
    dbg64_dec((uint64_t)config64_count64());
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

// ==================== 落盘 ====================
int config64_flush64() {
    // ★ 只在**有 VimtuFS2 卷**（carrier=vfs）时落盘。原因（不是偷懒，是如实）：
    //   没有卷时 store64 会退到裸盘槽区 LBA 8009..8072 —— 那块区域与安装程序建的数据分区
    //   **完全重叠**（store64.h / memlayout64.h 里都有醒目说明）。配置改动的自动落盘/关机落盘
    //   会往那里写，从而"污染"一块本该只读的镜像（实测：拖个桌面图标就会改掉 build64/system.img）。
    //   所以这条降级路径只做只读载入；要真正持久化请给磁盘分区/格式化（安装程序默认就会做）。
    const char* carrier = store64_carrier64();
    if (!carrier || carrier[0] != 'v') {
        g_pending = false;      // 不重试：没有卷时永远写不了
        dbg64_line_begin64();
        dbg64_str("[CONF64] flush skipped carrier=");
        dbg64_str(carrier ? carrier : "none");
        dbg64_str(" (no VimtuFS2 volume; raw slot area overlaps the data partition, not writing)");
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    const int rc = store64_flush64();
    if (rc == 0) g_pending = false;
    dbg64_line_begin64();
    if (rc == 0) {
        dbg64_str("[CONF64] flush ok via=");
        dbg64_str(store64_carrier64());
        dbg64_str(" slot=");
        dbg64_str(store64_slot_name64());
        dbg64_str(" gen=");
        dbg64_dec(store64_generation64());
        dbg64_str(" keys=");
        dbg64_dec((uint64_t)store64_key_count64());
    } else {
        dbg64_str("[CONF64] flush failed rc=");
        dbg64_dec((uint64_t)(rc < 0 ? -rc : rc));
        dbg64_str(" via=");
        dbg64_str(store64_carrier64());
    }
    dbg64_nl();
    dbg64_line_end64();
    return rc;
}

int config64_pending64() { return g_pending ? 1 : 0; }
uint32_t config64_last_change_tick64() { return g_last_change; }

// GUI 循环每帧调用：改动后延迟 CFG64_AUTOSAVE_MS 落盘（避免频繁写盘；磁盘写不在中断里做）。
int config64_tick64() {
    if (!g_pending) return 0;
    if ((uint32_t)(ticks64() - g_last_change) < ms_to_ticks64(CFG64_AUTOSAVE_MS)) return 0;
    const int rc = config64_flush64();
    dbg64_line_begin64();
    dbg64_str("[CONF64] autosave ");
    dbg64_str(rc == 0 ? "ok keys=" : "failed keys=");
    dbg64_dec((uint64_t)store64_key_count64());
    dbg64_str(" delayed=");
    dbg64_dec((uint64_t)(CFG64_AUTOSAVE_MS / 1000));
    dbg64_str("s");
    dbg64_nl();
    dbg64_line_end64();
    return rc == 0 ? 1 : -1;
}

// ==================== 枚举 / 报告 ====================
int config64_default_count64() { return CFG64_DEF_N; }

// 可见条目 = 默认表条目 ∪ 覆盖表里"新键"（不在默认表里的），一次遍历给出第 i 条
static int entry_at(int i, const Cfg64Def** d_out, const Cfg64Ov** o_out) {
    if (i < 0) return -1;
    if (i < CFG64_DEF_N) {
        const Cfg64Def* d = &kDefs[i];
        *d_out = d;
        *o_out = ov_find(d->key);
        return 0;
    }
    int k = i - CFG64_DEF_N;
    for (int j = 0; j < g_ov_n; j++) {
        if (!g_ov[j].used) continue;
        if (def_find(g_ov[j].key)) continue;      // 默认表里的已经被上面算过
        if (k == 0) { *d_out = nullptr; *o_out = &g_ov[j]; return 0; }
        k--;
    }
    return -1;
}

int config64_count64() {
    int n = CFG64_DEF_N;
    for (int j = 0; j < g_ov_n; j++) {
        if (!g_ov[j].used || def_find(g_ov[j].key)) continue;
        n++;
    }
    return n;
}

int config64_entry64(int i, char* key, int key_max, char* val, int val_max,
                     int* type, int* from_store) {
    const Cfg64Def* d = nullptr;
    const Cfg64Ov* o = nullptr;
    if (entry_at(i, &d, &o) != 0) return -1;
    const char* k = d ? d->key : o->key;
    if (key && key_max > 0) c_copy(key, k, key_max);
    int t = o ? o->type : (d ? d->type : CFG64_T_INT);
    if (type) *type = t;
    if (from_store) *from_store = o ? 1 : 0;
    if (val && val_max > 0) {
        if (t == CFG64_T_STR) c_copy(val, o ? o->sval : (d->sval ? d->sval : ""), val_max);
        else                  fmt_dec(val, val_max, o ? o->ival : d->ival);
    }
    return 0;
}

const char* config64_carrier64() { return store64_carrier64(); }
const char* config64_slot64()    { return store64_slot_name64(); }

const char* config64_type_name64(int type) {
    switch (type) {
        case CFG64_T_INT:  return "int";
        case CFG64_T_STR:  return "str";
        case CFG64_T_BOOL: return "bool";
        default:           return "?";
    }
}

static void app(char* out, int& pos, int max, const char* s) {
    if (!s) return;
    for (int i = 0; s[i] && pos < max - 1; i++) out[pos++] = s[i];
}
static void app_int(char* out, int& pos, int max, int v) {
    char t[12];
    fmt_dec(t, (int)sizeof(t), v);
    app(out, pos, max, t);
}

int config64_format64(char* out, int maxlen) {
    if (!out || maxlen <= 0) return 0;
    int pos = 0;
    app(out, pos, maxlen, "# config64: ");
    app_int(out, pos, maxlen, config64_count64());
    app(out, pos, maxlen, " keys (");
    app_int(out, pos, maxlen, config64_default_count64());
    app(out, pos, maxlen, " defaults + ");
    int store_n = 0;
    for (int i = 0; i < g_ov_n; i++) if (g_ov[i].used) store_n++;
    app_int(out, pos, maxlen, store_n);
    app(out, pos, maxlen, " from store)\n");
    app(out, pos, maxlen, "# carrier=");
    app(out, pos, maxlen, config64_carrier64());
    app(out, pos, maxlen, " slot=");
    app(out, pos, maxlen, config64_slot64());
    app(out, pos, maxlen, " gen=");
    {
        char t[24]; int n = 0;
        uint64_t g = store64_generation64();
        char rev[24];
        if (g == 0) rev[n++] = '0';
        while (g > 0) { rev[n++] = (char)('0' + (g % 10)); g /= 10; }
        int o = 0;
        while (n > 0) t[o++] = rev[--n];
        t[o] = 0;
        app(out, pos, maxlen, t);
    }
    app(out, pos, maxlen, " pending=");
    app_int(out, pos, maxlen, config64_pending64());
    app(out, pos, maxlen, "\n");
    const int n = config64_count64();
    char k[CFG64_KEY_MAX], v[CFG64_STR_MAX];
    for (int i = 0; i < n; i++) {
        int type = 0, from = 0;
        if (config64_entry64(i, k, (int)sizeof(k), v, (int)sizeof(v), &type, &from) != 0) break;
        app(out, pos, maxlen, "  ");
        app(out, pos, maxlen, k);
        app(out, pos, maxlen, " = ");
        app(out, pos, maxlen, v);
        app(out, pos, maxlen, "  [");
        app(out, pos, maxlen, config64_type_name64(type));
        app(out, pos, maxlen, from ? ",store]" : ",default]");
        app(out, pos, maxlen, "\n");
        if (pos >= maxlen - 4) break;
    }
    out[pos] = 0;
    return pos;
}

// ==================== 语义封装 ====================
int  cfg64_lang_zh64() { return config64_get_int64("ui.lang", 1) ? 1 : 0; }
void cfg64_set_lang_zh64(int zh) { config64_set_int64("ui.lang", zh ? 1 : 0); }

static void icon_key64(int i, char axis, char* buf, int cap) {
    c_copy(buf, "icon.", cap);
    int n = c_len(buf);
    if (n < cap - 2) buf[n++] = axis;
    if (n < cap - 1) buf[n++] = (char)('0' + (i % 10));
    buf[n] = 0;
}
int  cfg64_icon_x64(int i) { char k[CFG64_KEY_MAX]; icon_key64(i, 'x', k, (int)sizeof(k)); return config64_get_int64(k, 24); }
int  cfg64_icon_y64(int i) { char k[CFG64_KEY_MAX]; icon_key64(i, 'y', k, (int)sizeof(k)); return config64_get_int64(k, 24 + i * 84); }
void cfg64_set_icon64(int i, int x, int y) {
    char k[CFG64_KEY_MAX];
    icon_key64(i, 'x', k, (int)sizeof(k)); config64_set_int64(k, x);
    icon_key64(i, 'y', k, (int)sizeof(k)); config64_set_int64(k, y);
}

static void win_key64(const char* wapp, char axis, char* buf, int cap) {
    c_copy(buf, "win.", cap);
    int n = c_len(buf);
    for (int i = 0; wapp[i] && n < cap - 3; i++) buf[n++] = wapp[i];
    if (n < cap - 2) buf[n++] = '.';
    if (n < cap - 1) buf[n++] = axis;
    buf[n] = 0;
}
int  cfg64_win_w64(const char* wapp) { char k[CFG64_KEY_MAX]; win_key64(wapp, 'w', k, (int)sizeof(k)); return config64_get_int64(k, 0); }
int  cfg64_win_h64(const char* wapp) { char k[CFG64_KEY_MAX]; win_key64(wapp, 'h', k, (int)sizeof(k)); return config64_get_int64(k, 0); }
void cfg64_set_win64(const char* wapp, int w, int h) {
    if (!wapp) return;
    char k[CFG64_KEY_MAX];
    win_key64(wapp, 'w', k, (int)sizeof(k)); config64_set_int64(k, w);
    win_key64(wapp, 'h', k, (int)sizeof(k)); config64_set_int64(k, h);
}

int  cfg64_mouse_sens64() { return config64_get_int64("mouse.sens", 1700); }
void cfg64_set_mouse_sens64(int permille) {
    if (permille < 800) permille = 800;        // 0.8x（照 32 位的钳制范围）
    if (permille > 2500) permille = 2500;      // 2.5x
    config64_set_int64("mouse.sens", permille);
}

int  cfg64_text_mirror64() { return config64_get_int64("ui.text_mirror", 0) ? 1 : 0; }
void cfg64_set_text_mirror64(int on) { config64_set_int64("ui.text_mirror", on ? 1 : 0); }

static void startup_key64(const char* item, char* buf, int cap) {
    c_copy(buf, "startup.", cap);
    int n = c_len(buf);
    for (int i = 0; item[i] && n < cap - 1; i++) buf[n++] = item[i];
    buf[n] = 0;
}
int  cfg64_startup64(const char* item) {
    if (!item || !item[0]) return 0;
    char k[CFG64_KEY_MAX];
    startup_key64(item, k, (int)sizeof(k));
    return config64_get_int64(k, 0);
}
void cfg64_set_startup64(const char* item, int on) {
    if (!item || !item[0]) return;
    char k[CFG64_KEY_MAX];
    startup_key64(item, k, (int)sizeof(k));
    config64_set_int64(k, on ? 1 : 0);
}

int  cfg64_zoom64() { return config64_get_int64("display.zoom", 100); }
void cfg64_set_zoom64(int pct) { config64_set_int64("display.zoom", pct); }

int  cfg64_session_mode64() { return config64_get_int64("session.mode", 0) ? 1 : 0; }
void cfg64_set_session_mode64(int mode) { config64_set_int64("session.mode", mode ? 1 : 0); }
int  cfg64_session_restore64(char* out, int out_max) { return config64_get_str64("sess.restore", "", out, out_max); }
void cfg64_set_session_restore64(const char* list) { config64_set_str64("sess.restore", list ? list : ""); }

// ==================== 本批（Windows 11 现代外观）的语义封装 ====================
int  cfg64_theme64() { return config64_get_int64("ui.theme", 0); }
void cfg64_set_theme64(int id) {
    if (id < 0) id = 0;
    if (id > THEME64_THEME_COUNT - 1) id = THEME64_THEME_COUNT - 1;
    config64_set_int64("ui.theme", id);
}

int  cfg64_reduce_motion64() { return config64_get_int64("ui.reduce_motion", 0) ? 1 : 0; }
void cfg64_set_reduce_motion64(int on) { config64_set_int64("ui.reduce_motion", on ? 1 : 0); }

int  cfg64_wall_mode64() {
    int m = config64_get_int64("ui.wall.mode", 0);
    if (m < 0 || m > 5) m = 0;
    return m;
}
void cfg64_set_wall_mode64(int mode) {
    if (mode < 0 || mode > 5) mode = 0;
    config64_set_int64("ui.wall.mode", mode);
}

int  cfg64_lock_wall_mode64() {
    int m = config64_get_int64("ui.wall.lock_mode", 0);
    if (m < 0 || m > 5) m = 0;
    return m;
}
void cfg64_set_lock_wall_mode64(int mode) {
    if (mode < 0 || mode > 5) mode = 0;
    config64_set_int64("ui.wall.lock_mode", mode);
}

int  cfg64_wall_path64(char* out, int out_max) { return config64_get_str64("ui.wall.path", "", out, out_max); }
void cfg64_set_wall_path64(const char* path) { config64_set_str64("ui.wall.path", path ? path : ""); }
int  cfg64_avatar_path64(char* out, int out_max) { return config64_get_str64("ui.avatar.path", "", out, out_max); }
void cfg64_set_avatar_path64(const char* path) { config64_set_str64("ui.avatar.path", path ? path : ""); }

int  cfg64_dock_len64() { int v = config64_get_int64("dock.len", 0); return v < 0 ? 0 : v; }
void cfg64_set_dock_len64(int len) { config64_set_int64("dock.len", len < 0 ? 0 : len); }
int  cfg64_dock_size64() {
    int v = config64_get_int64("dock.size", THEME64_DOCK_H);
    if (v < 44) v = 44;            // 需求：Dock 高 60（Token）→ 允许 44..80
    if (v > 80) v = 80;
    return v;
}
void cfg64_set_dock_size64(int h) { config64_set_int64("dock.size", h); }
int  cfg64_dock_icon64() {
    int v = config64_get_int64("dock.icon", THEME64_DOCK_ICON);
    if (v < 44) v = 44;            // 需求：图标 44–48px（Token 46）
    if (v > 48) v = 48;
    return v;
}
void cfg64_set_dock_icon64(int px) { config64_set_int64("dock.icon", px); }
int  cfg64_dock_gap64() {
    int v = config64_get_int64("dock.gap", THEME64_DOCK_GAP);
    if (v < 0) v = 0;
    if (v > 40) v = 40;
    return v;
}
void cfg64_set_dock_gap64(int px) { config64_set_int64("dock.gap", px); }

// ==================== ★ P3：设置应用用到的语义封装 ====================
// 字体大小档：只在 14/16/18 三档里取值（font.cpp 的 em 像素高；16 = 本批默认，
// 也正是 FONT_SIZE_PX 的历史值 —— 保持不变就不会影响既有像素断言）。
int  cfg64_font_size64() {
    int v = config64_get_int64("ui.font.size", 16);
    if (v < 14) v = 14;
    if (v > 18) v = 18;
    return v;
}
void cfg64_set_font_size64(int px) {
    if (px < 14) px = 14;
    if (px > 18) px = 18;
    config64_set_int64("ui.font.size", px);
}

int  cfg64_wall_sync64() { return config64_get_bool64("ui.wall.sync", 1) ? 1 : 0; }
void cfg64_set_wall_sync64(int on) { config64_set_bool64("ui.wall.sync", on ? 1 : 0); }

int  cfg64_grad_on64() { return config64_get_bool64("ui.grad.on", 0) ? 1 : 0; }
void cfg64_set_grad_on64(int on) { config64_set_bool64("ui.grad.on", on ? 1 : 0); }
int  cfg64_grad_a64(char* out, int out_max) { return config64_get_str64("ui.grad.a", "#5AA9F0", out, out_max); }
void cfg64_set_grad_a64(const char* hex) { config64_set_str64("ui.grad.a", hex ? hex : ""); }
int  cfg64_grad_b64(char* out, int out_max) { return config64_get_str64("ui.grad.b", "#C9A7FF", out, out_max); }
void cfg64_set_grad_b64(const char* hex) { config64_set_str64("ui.grad.b", hex ? hex : ""); }

int  cfg64_sound_vol64() {
    int v = config64_get_int64("ui.sound.volume", 42);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}
void cfg64_set_sound_vol64(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    config64_set_int64("ui.sound.volume", vol);
}
int  cfg64_sound_src64() { return config64_get_int64("ui.sound.src", 0) ? 1 : 0; }
void cfg64_set_sound_src64(int src) { config64_set_int64("ui.sound.src", src ? 1 : 0); }

// ★ P5：桌面图标集合（desktopops64 读写）："<桌面集合>|<回收站集合>"，如 "0,1,2|"。
// 空串 = 桌面一个图标都不显示（用户把图标都删了）；默认值 "0,1,2|" 与旧行为完全一致。
int  cfg64_desktop_icons64(char* out, int out_max) {
    return config64_get_str64("ui.desktop.icons", "0,1,2|", out, out_max);
}
void cfg64_set_desktop_icons64(const char* set) {
    config64_set_str64("ui.desktop.icons", set ? set : "0,1,2|");
}
// ★ P5：文件资源管理器是否显示系统分区（0 = 隐藏，默认；需求原文："系统分区默认是隐藏状态的"）
int  cfg64_explorer_show_system64() { return config64_get_int64("ui.explorer.show_system", 0) ? 1 : 0; }
void cfg64_set_explorer_show_system64(int on) { config64_set_int64("ui.explorer.show_system", on ? 1 : 0); }

const char* cfg64_defapp_key64(int kind) {
    switch (kind) {
        case CFG64_DEFK_ELF:   return "ui.def.elf";
        case CFG64_DEFK_VAP:   return "ui.def.vap";
        case CFG64_DEFK_TXT:   return "ui.def.txt";
        case CFG64_DEFK_VIDEO: return "ui.def.video";
        default:               return "";
    }
}
int cfg64_defapp64(int kind, char* out, int out_max) {
    const char* k = cfg64_defapp_key64(kind);
    if (!k[0] || out_max <= 0) return -1;
    if (out) out[0] = 0;
    char dflt[8];
    dflt[0] = 0;
    if (kind != CFG64_DEFK_VIDEO) {
        dflt[0] = 't'; dflt[1] = 'e'; dflt[2] = 'r'; dflt[3] = 'm'; dflt[4] = 0;
    }
    return config64_get_str64(k, dflt, out, out_max);
}
void cfg64_set_defapp64(int kind, const char* app) {
    const char* k = cfg64_defapp_key64(kind);
    if (!k[0]) return;
    config64_set_str64(k, app ? app : "");
}
