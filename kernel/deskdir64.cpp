// deskdir64.cpp - ★ v0.11.7：桌面 = 真实文件夹的实现
//   设计口径、打点格式、边界都在 deskdir64.h 顶部；这里只写实现。
//
// 硬约束（改之前先读）：
//   1) 内核里没有 libc：字符串/数字全部用本文件的 d_* 小工具，不 include <string.h>。
//   2) 每次读盘都 `panic64_watchdog_pause64()` 包起来：目录列 20 条 = 20 次 inode 读，
//      ATA 走"IRQ 超时 -> 轮询"时能到 1 秒级；看门狗在这里误报会直接把"拖个文件"变成重启。
//      （只包**我们自己发起的那一小段**，不是整帧；与 trash64/app64 写盘同款做法。）
//   3) 真移动一律先 stat 源、再算唯一名、最后**一次** rename——中间不留"两边都看不到"的窗口。
#include "deskdir64.h"
#include "vfs64.h"
#include "fs64.h"
#include "debug64.h"
#include "userdb64.h"          // GUI 用户（root -> /root/Desktop，普通用户 -> /home/<name>/Desktop）
#include "trash64.h"           // 桌面条目 -> /Recycle（同卷改名）
#include "explorer64.h"        // explorer64_copy_file64（跨卷搬单文件时复用同一套分块复制）
#include "panic64.h"           // 帧内/用户动作里读写的看门狗临时暂停

#define DESKDIR_FALLBACK "/home/vimtu/Desktop"   // 未登录/没有用户表时的退化目录（与 settings64 同口径）
#define DESKDIR_SCAN_CAP 512                     // 一次扫描最多看多少条（有界；防御坏卷）

// ==================== 状态 ====================
static char          g_dir[VFS64_PATH_MAX];
static Vfs64Dirent64 g_ent[DESKDIR_MAX];
static int           g_n = 0;        // 列出来的条目数（<= DESKDIR_MAX）
static int           g_total = 0;    // 目录里的真实条目总数（含隐藏/超界）
static int           g_hidden = 0;   // 以 '.' 开头的条目数（不列）
static int           g_inited = 0;
static uint32_t      g_scan_no = 0;

// ==================== 小工具（内核里没有 libc）====================
static int d_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static int d_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void d_cpy(char* d, int cap, const char* s) {
    int i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static void d_cat(char* d, int cap, const char* s) {
    int n = d_len(d);
    for (int i = 0; s && s[i] && n < cap - 1; i++) d[n++] = s[i];
    d[n] = 0;
}
static void d_num(char* out, int cap, int v) {
    char t[12];
    int n = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0 && n < (int)sizeof(t)) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    int k = 0;
    while (n > 0 && k < cap - 1) out[k++] = t[--n];
    out[k] = 0;
}
// 目录 + '/' + 名字（桌面目录若以 '/' 结尾就不再补）
static void d_join(char* out, int cap, const char* dir, const char* name) {
    d_cpy(out, cap, dir);
    const int n = d_len(out);
    if (n <= 0 || out[n - 1] != '/') d_cat(out, cap, "/");
    d_cat(out, cap, name);
}

// ==================== 目录绑定 ====================
static void pick_dir(void) {
    const User64Entry* u = userdb64_gui_user64();
    const char* d = (u && u->desktop[0] == '/') ? u->desktop : DESKDIR_FALLBACK;
    if (!d_eq(g_dir, d)) d_cpy(g_dir, (int)sizeof(g_dir), d);
}

const char* deskdir64_dir64() {
    if (!g_dir[0]) d_cpy(g_dir, (int)sizeof(g_dir), DESKDIR_FALLBACK);
    return g_dir;
}
int deskdir64_slot64() { return vfs64_system_slot64(); }
// VimtuFS2 卷在 fs64 里的卷号 == VFS 槽号（fs64.h：FS64_VOL_FAT_BASE = VFS64_SLOT_MAX，
// 即 0..3 号 vfs 槽就是 0..3 号 fs64 卷），所以这里直接返回槽号。
int deskdir64_vol64() { return vfs64_system_slot64(); }

int deskdir64_ensure64(const char* why) {
    if (!g_dir[0]) pick_dir();
    const int slot = vfs64_system_slot64();
    int rc = -1;
    if (slot >= 0) {
        panic64_watchdog_pause64();
        // ★ mkdirs_core64 只建**父**目录（它把 cut 停在最后一段之前）——最后一段要自己 mkdir
        rc = vfs64_mkdirs_on64(slot, g_dir);
        if (rc == 0) {
            Vfs64Info64 info;
            if (vfs64_stat64_on64(slot, g_dir, &info) != 0) rc = vfs64_mkdir_on64(slot, g_dir);
        }
        panic64_watchdog_unpause64();
    }
    dbg64_line_begin64();
    dbg64_str("[DESKDIR] ensure dir="); dbg64_str(g_dir);
    dbg64_str(" slot="); dbg64_dec((uint64_t)(slot < 0 ? 0xFFFF : (uint32_t)slot));
    dbg64_str(" rc=");   dbg64_dec((uint64_t)(int64_t)rc);
    dbg64_str(" why=");  dbg64_str(why ? why : "-");
    dbg64_str(rc == 0 ? " ok=1" : " ok=0");
    dbg64_nl();
    dbg64_line_end64();
    return rc == 0;
}

// ==================== 扫描（真源：列目录）====================
int deskdir64_scan64(const char* why) {
    if (!g_dir[0]) pick_dir();
    // 旧集合快照（判增删用；名字 + 类型）
    char old_name[DESKDIR_MAX][DESKDIR_NAME_MAX + 1];
    int  old_dir[DESKDIR_MAX];
    const int old_n = g_n;
    for (int i = 0; i < old_n; i++) {
        d_cpy(old_name[i], DESKDIR_NAME_MAX + 1, g_ent[i].name);
        old_dir[i] = (g_ent[i].type == VFS64_TYPE_DIR) ? 1 : 0;
    }

    g_n = 0; g_total = 0; g_hidden = 0;
    const int slot = vfs64_system_slot64();
    if (slot >= 0) {
        uint32_t cursor = 0;
        panic64_watchdog_pause64();
        for (;;) {
            Vfs64Dirent64 d;
            const int r = vfs64_list64_on64(slot, g_dir, &d, 1, &cursor);
            if (r <= 0) break;
            g_total++;
            if (d.name[0] == '.') { g_hidden++; if (g_total > DESKDIR_SCAN_CAP) break; continue; }
            if (g_n < DESKDIR_MAX) g_ent[g_n++] = d;
            if (g_total > DESKDIR_SCAN_CAP) break;         // 有界：坏卷/超大目录不会把帧拖死
        }
        panic64_watchdog_unpause64();
    }

    // 增 / 删（换类型算"改了"：既不算 added 也不算 removed，但整体算变化）
    int added = 0, removed = 0, retyped = 0;
    for (int i = 0; i < g_n; i++) {
        int hit = -1;
        for (int k = 0; k < old_n; k++) if (d_eq(old_name[k], g_ent[i].name)) { hit = k; break; }
        if (hit < 0) { added++; continue; }
        const int nd = (g_ent[i].type == VFS64_TYPE_DIR) ? 1 : 0;
        if (nd != old_dir[hit]) retyped++;
    }
    for (int k = 0; k < old_n; k++) {
        int hit = 0;
        for (int i = 0; i < g_n; i++) if (d_eq(g_ent[i].name, old_name[k])) { hit = 1; break; }
        if (!hit) removed++;
    }
    g_scan_no++;

    const int changed = (added || removed || retyped) ? 1 : 0;
    // 打点：只在"有变化"或调用方明确要（非 fs 轮询）时打，避免 4Hz 刷屏
    if (changed || !why || !d_eq(why, "fs")) {
        dbg64_line_begin64();
        dbg64_str("[DESKDIR] scan why="); dbg64_str(why ? why : "-");
        dbg64_str(" dir=");               dbg64_str(g_dir);
        dbg64_str(" entries=");           dbg64_dec((uint64_t)g_n);
        dbg64_str(" total=");             dbg64_dec((uint64_t)g_total);
        dbg64_str(" added=");             dbg64_dec((uint64_t)added);
        dbg64_str(" removed=");           dbg64_dec((uint64_t)removed);
        dbg64_str(" retyped=");           dbg64_dec((uint64_t)retyped);
        dbg64_str(" hidden=");            dbg64_dec((uint64_t)g_hidden);
        dbg64_str(" slot=");              dbg64_dec((uint64_t)(slot < 0 ? 0xFFFF : (uint32_t)slot));
        dbg64_str(" names=");
        for (int i = 0; i < g_n; i++) { if (i) dbg64_str(","); dbg64_str(g_ent[i].name); }
        if (g_n == 0) dbg64_str("(empty)");
        dbg64_str(changed ? " changed=1" : " changed=0");
        dbg64_nl();
        dbg64_line_end64();
    }
    return changed;
}

int deskdir64_init64(const char* why) {
    pick_dir();
    g_inited = 1;
    const int slot = vfs64_system_slot64();
    int exists = 0;
    if (slot >= 0) {
        Vfs64Info64 info;
        panic64_watchdog_pause64();
        exists = (vfs64_stat64_on64(slot, g_dir, &info) == 0 && info.type == VFS64_TYPE_DIR) ? 1 : 0;
        panic64_watchdog_unpause64();
    }
    if (!exists) (void)deskdir64_ensure64("init");
    (void)deskdir64_scan64(why ? why : "init");
    dbg64_line_begin64();
    dbg64_str("[DESKDIR] init dir=");  dbg64_str(g_dir);
    dbg64_str(" vol=");                dbg64_dec((uint64_t)(slot < 0 ? 0xFFFF : (uint32_t)slot));
    dbg64_str(" exists=");             dbg64_dec((uint64_t)exists);
    dbg64_str(" entries=");            dbg64_dec((uint64_t)g_n);
    dbg64_str(" total=");              dbg64_dec((uint64_t)g_total);
    dbg64_str(exists ? " mode=dir" : " mode=created");
    dbg64_nl();
    dbg64_line_end64();
    return g_n;
}

int deskdir64_rebind64(const char* why) {
    char was[VFS64_PATH_MAX];
    d_cpy(was, (int)sizeof(was), g_dir);
    pick_dir();
    if (d_eq(was, g_dir)) return 0;
    dbg64_line_begin64();
    dbg64_str("[DESKDIR] rebind why="); dbg64_str(why ? why : "-");
    dbg64_str(" from="); dbg64_str(was[0] ? was : "(none)");
    dbg64_str(" to=");   dbg64_str(g_dir);
    dbg64_nl();
    dbg64_line_end64();
    (void)deskdir64_init64("rebind");
    return 1;
}

// ==================== 条目访问 ====================
int deskdir64_count64() { return g_n; }
const char* deskdir64_name64(int i) { return (i >= 0 && i < g_n) ? g_ent[i].name : ""; }
int deskdir64_is_dir64(int i) { return (i >= 0 && i < g_n && g_ent[i].type == VFS64_TYPE_DIR) ? 1 : 0; }
uint32_t deskdir64_size64(int i) { return (i >= 0 && i < g_n) ? g_ent[i].size : 0u; }
int deskdir64_path64(int i, char* out, int cap) {
    if (i < 0 || i >= g_n || !out || cap <= 0) return -1;
    d_join(out, cap, g_dir, g_ent[i].name);
    return d_len(out);
}
int deskdir64_find64(const char* name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < g_n; i++) if (d_eq(g_ent[i].name, name)) return i;
    return -1;
}
int deskdir64_name_of64(const char* path, char* out, int cap) {
    if (!path || !out || cap <= 0) return 0;
    const char* b = path;
    for (const char* p = path; *p; p++) if (*p == '/') b = p + 1;
    d_cpy(out, cap, b);
    return out[0] ? 1 : 0;
}

// ==================== 唯一名（撞名 -> stem(2).ext）====================
// 与 explorer64 的 suffix_name_pure 同一口径：插在扩展名之前、**不带空格**
// （VimtuFS2 的名字只允许 0x21..0x7E，空格非法）。只碰最后一个 '.'，且不是首字符（隐藏文件不切）。
static int uniq_build64(const char* name, int k, char* out, int cap) {
    if (!name || !name[0] || !out || cap <= 1) return 0;
    int dot = -1;
    for (int i = 0; name[i]; i++) if (name[i] == '.' && i > 0) dot = i;
    const int stem_n = (dot >= 0) ? dot : d_len(name);
    char num[12];
    d_num(num, (int)sizeof(num), k);
    const int suf_n = d_len(num) + 2;              // "(k)"
    int n = 0;
    int room = cap - 1 - suf_n - ((dot >= 0) ? d_len(name + dot) : 0);
    if (room < 1) room = 1;
    for (int i = 0; i < stem_n && n < room; i++) out[n++] = name[i];
    out[n++] = '(';
    for (int i = 0; num[i] && n < cap - 1; i++) out[n++] = num[i];
    out[n++] = ')';
    if (dot >= 0) for (int i = dot; name[i] && n < cap - 1; i++) out[n++] = name[i];
    out[n] = 0;
    return 1;
}
int deskdir64_unique_name64(const char* name, char* out, int cap) {
    if (!name || !name[0]) return 0;
    const int slot = vfs64_system_slot64();
    char p[VFS64_PATH_MAX];
    Vfs64Info64 st;
    // 原名没被占用：直接用（调用方一般是"目标已存在"才进来，但这里也要自洽）
    d_join(p, (int)sizeof(p), g_dir, name);
    if (slot < 0 || vfs64_stat64_on64(slot, p, &st) != 0) { d_cpy(out, cap, name); return 1; }
    for (int k = 2; k <= 99; k++) {
        char cand[DESKDIR_NAME_MAX + 1];
        if (!uniq_build64(name, k, cand, (int)sizeof(cand))) return 0;
        d_join(p, (int)sizeof(p), g_dir, cand);
        if (vfs64_stat64_on64(slot, p, &st) != 0) { d_cpy(out, cap, cand); return 1; }
    }
    return 0;                                   // 99 个都撞：如实失败（绝不覆盖）
}

// ==================== 真移动 / 改名 / 回收站 ====================
int deskdir64_move_in64(int src_vol, const char* src_path, const char* why, char* out_name, int cap) {
    if (out_name && cap > 0) out_name[0] = 0;
    if (!src_path || src_path[0] != '/') {
        dbg64_str("[DESKDIR] move in reject reason=bad-path\n");
        return 0;
    }
    if (!g_dir[0]) pick_dir();
    const int vol_sys = deskdir64_vol64();
    char nm[DESKDIR_NAME_MAX + 1];
    if (!deskdir64_name_of64(src_path, nm, (int)sizeof(nm))) {
        dbg64_line_begin64();
        dbg64_str("[DESKDIR] move in reject reason=no-basename from="); dbg64_str(src_path);
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    char dst[VFS64_PATH_MAX];
    d_join(dst, (int)sizeof(dst), g_dir, nm);

    // ① 已经在桌面上（同一个路径）：幂等成功（不搬、不复制）
    if (d_eq(src_path, dst)) {
        if (out_name && cap > 0) d_cpy(out_name, cap, nm);
        dbg64_line_begin64();
        dbg64_str("[DESKDIR] move in why="); dbg64_str(why ? why : "-");
        dbg64_str(" from="); dbg64_str(src_path);
        dbg64_str(" to=");   dbg64_str(dst);
        dbg64_str(" rc=0 via=idempotent name="); dbg64_str(nm);
        dbg64_nl();
        dbg64_line_end64();
        return 1;
    }

    // ② 源卷只读 / 源不存在
    const char* w = "-";
    int ok = 0;
    if (vol_sys < 0) { w = "no-sysvol"; return 0; }
    if (src_vol != vol_sys && fs64_is_readonly64(src_vol)) w = "readonly";
    Fs64Stat64 sst;
    int is_dir = 0;
    if (d_eq(w, "-")) {
        if (fs64_stat64(src_vol, src_path, &sst) != 0) w = "no-src";
        else is_dir = (sst.type == VFS64_TYPE_DIR) ? 1 : 0;
    }

    // ③ 撞名：目标已存在 -> 唯一化（绝不覆盖已有条目）
    if (d_eq(w, "-")) {
        Vfs64Info64 st;
        const int slot = vfs64_system_slot64();
        if (slot >= 0 && vfs64_stat64_on64(slot, dst, &st) == 0) {
            char uniq[DESKDIR_NAME_MAX + 1];
            if (!deskdir64_unique_name64(nm, uniq, (int)sizeof(uniq))) w = "no-name-room";
            else {
                d_cpy(nm, (int)sizeof(nm), uniq);
                d_join(dst, (int)sizeof(dst), g_dir, nm);
            }
        }
    }

    // ④ 真移动：同卷 = 一次 rename；跨卷 = 复制 + 删源（目录跨卷不支持，如实拒绝）
    const char* via = "rename";
    if (d_eq(w, "-")) {
        panic64_watchdog_pause64();
        if (src_vol == vol_sys) {
            const int rc = fs64_rename_to64(src_vol, src_path, dst);
            if (rc == 0) ok = 1; else w = "rename-failed";
        } else if (is_dir) {
            w = "crossvol-dir";
        } else {
            via = "copy+unlink";
            int cw = 0;
            if (explorer64_copy_file64(src_vol, src_path, vol_sys, dst, &cw) == 0) {
                if (fs64_unlink64(src_vol, src_path) == 0) ok = 1; else w = "unlink-src-failed";
            } else {
                w = "copy-failed";
            }
        }
        panic64_watchdog_unpause64();
    }

    if (ok && out_name && cap > 0) d_cpy(out_name, cap, nm);
    dbg64_line_begin64();
    dbg64_str("[DESKDIR] move in why="); dbg64_str(why ? why : "-");
    dbg64_str(" from="); dbg64_str(src_path);
    dbg64_str(" to=");   dbg64_str(dst);
    dbg64_str(" vol=");  dbg64_dec((uint64_t)(uint32_t)vol_sys);
    dbg64_str(" dir=");  dbg64_dec((uint64_t)is_dir);
    dbg64_str(" via=");  dbg64_str(via);
    dbg64_str(" name="); dbg64_str(nm);
    dbg64_str(ok ? " rc=0 ok=1" : " rc=-1 ok=0");
    if (!ok) { dbg64_str(" why="); dbg64_str(w); }
    dbg64_nl();
    dbg64_line_end64();
    return ok;
}

int deskdir64_move_out64(const char* name, int dst_vol, const char* dst_path, const char* why) {
    if (!name || !name[0] || !dst_path || dst_path[0] != '/') return 0;
    if (!g_dir[0]) pick_dir();
    const int vol_sys = deskdir64_vol64();
    if (vol_sys < 0) return 0;
    char src[VFS64_PATH_MAX];
    d_join(src, (int)sizeof(src), g_dir, name);
    const char* w = "-";
    int ok = 0;
    if (dst_vol != vol_sys) w = "crossvol";
    else {
        panic64_watchdog_pause64();
        const int rc = fs64_rename_to64(vol_sys, src, dst_path);
        panic64_watchdog_unpause64();
        if (rc == 0) ok = 1; else w = "rename-failed";
    }
    dbg64_line_begin64();
    dbg64_str("[DESKDIR] move out why="); dbg64_str(why ? why : "-");
    dbg64_str(" name="); dbg64_str(name);
    dbg64_str(" to=");   dbg64_str(dst_path);
    dbg64_str(" vol=");  dbg64_dec((uint64_t)(uint32_t)vol_sys);
    dbg64_str(ok ? " rc=0 via=rename ok=1" : " rc=-1 via=rename ok=0");
    if (!ok) { dbg64_str(" why="); dbg64_str(w); }
    dbg64_nl();
    dbg64_line_end64();
    return ok;
}

int deskdir64_rename64(const char* old_name, const char* new_name) {
    if (!old_name || !old_name[0] || !new_name || !new_name[0]) return 0;
    if (!g_dir[0]) pick_dir();
    const int vol_sys = deskdir64_vol64();
    if (vol_sys < 0) return 0;
    char src[VFS64_PATH_MAX];
    d_join(src, (int)sizeof(src), g_dir, old_name);
    panic64_watchdog_pause64();
    const int rc = fs64_rename64(vol_sys, src, new_name);     // 只改最后一段（同目录）
    panic64_watchdog_unpause64();
    dbg64_line_begin64();
    dbg64_str("[DESKDIR] rename dir="); dbg64_str(g_dir);
    dbg64_str(" name="); dbg64_str(old_name);
    dbg64_str(" to=");   dbg64_str(new_name);
    dbg64_str(rc == 0 ? " rc=0 ok=1" : " rc=-1 ok=0");
    dbg64_nl();
    dbg64_line_end64();
    return rc == 0;
}

int deskdir64_trash64(const char* name, const char* why, char* out_name, int cap) {
    if (out_name && cap > 0) out_name[0] = 0;
    if (!name || !name[0]) return 0;
    if (!g_dir[0]) pick_dir();
    const int vol_sys = deskdir64_vol64();
    if (vol_sys < 0) return 0;
    char src[VFS64_PATH_MAX], entry[TRASH64_NAME_MAX + 1];
    d_join(src, (int)sizeof(src), g_dir, name);
    entry[0] = 0;
    panic64_watchdog_pause64();
    const int rc = trash64_put64(vol_sys, src, entry, (int)sizeof(entry));
    panic64_watchdog_unpause64();
    const int ok = (rc == 0) ? 1 : 0;
    if (ok && out_name && cap > 0) d_cpy(out_name, cap, entry);
    dbg64_line_begin64();
    dbg64_str("[DESKDIR] trash why="); dbg64_str(why ? why : "-");
    dbg64_str(" name="); dbg64_str(name);
    dbg64_str(" dir=");  dbg64_str(g_dir);
    dbg64_str(" rc=");   dbg64_dec((uint64_t)(int64_t)rc);
    dbg64_str(" entry="); dbg64_str(entry[0] ? entry : "-");
    dbg64_nl();
    dbg64_line_end64();
    return ok;
}

// ==================== 自检（纯逻辑：不碰盘）====================
int deskdir64_selftest64() {
    int fails = 0;      // 每一位 = 一条判据
    char b[DESKDIR_NAME_MAX + 1], p[VFS64_PATH_MAX];

    // ① 唯一名插在扩展名之前、不带空格：readme.txt -> readme(2).txt
    if (!uniq_build64("readme.txt", 2, b, (int)sizeof(b)) || !d_eq(b, "readme(2).txt")) fails |= 1;
    // ② 没有扩展名：noext -> noext(3)
    if (!uniq_build64("noext", 3, b, (int)sizeof(b)) || !d_eq(b, "noext(3)")) fails |= 2;
    // ③ 隐藏文件（首字符就是 '.'）不切扩展名：.bashrc -> .bashrc(2)
    if (!uniq_build64(".bashrc", 2, b, (int)sizeof(b)) || !d_eq(b, ".bashrc(2)")) fails |= 4;
    // ④ 路径最后一段
    if (!deskdir64_name_of64("/home/vimtu/Desktop/a.txt", b, (int)sizeof(b)) || !d_eq(b, "a.txt")) fails |= 8;
    // ⑤ 目录拼接（目录以 '/' 结尾时不补第二个 '/'）
    d_join(p, (int)sizeof(p), "/home/vimtu/Desktop/", "a.txt");
    if (!d_eq(p, "/home/vimtu/Desktop/a.txt")) fails |= 16;
    // ⑥ 十进制（后续日志/比对都用它）
    d_num(b, (int)sizeof(b), 4095);
    if (!d_eq(b, "4095")) fails |= 32;

    const int mask = (~fails) & 0x3F;
    dbg64_line_begin64();
    dbg64_str("[DESKDIR] selftest ");
    dbg64_str(mask == 0x3F ? "PASS" : "FAIL");
    dbg64_str(" mask=");
    dbg64_dec((uint64_t)mask);
    dbg64_str(" dir=");
    dbg64_str(deskdir64_dir64());
    dbg64_str(" inited=");
    dbg64_dec((uint64_t)g_inited);
    dbg64_str(" scans=");
    dbg64_dec((uint64_t)g_scan_no);
    dbg64_nl();
    dbg64_line_end64();
    return mask;
}
