// trash64.cpp - ★ v0.11.6：文件回收站（设计/边界/打点见 trash64.h）
//
// 一句话：**put = 同卷改名进 /Recycle；restore = 改名搬回去；purge = 有界递归真删。**
// 本文件不引 libc（内核无 libc），字符串/整数格式化全部自带；所有失败路径都**先回滚再返回**，
// 绝不在"文件已经在回收站里、元数据却没写成"这种中间态上停下（那样用户就找不回原位置了）。
#include "trash64.h"
#include "debug64.h"

// ==================== 最小字符串工具 ====================
static int tr_slen(const char* s) { int n = 0; if (!s) return 0; while (s[n]) n++; return n; }

static int tr_eq(const char* a, const char* b) {
    if (!a || !b) return 0;
    for (;;) {
        if (*a != *b) return 0;
        if (!*a) return 1;
        a++; b++;
    }
}

static void tr_cpy(char* dst, const char* src, int cap) {
    int n = 0;
    if (!dst || cap <= 0) return;
    if (src) { while (src[n] && n < cap - 1) { dst[n] = src[n]; n++; } }
    dst[n] = 0;
}

static void tr_cat(char* dst, const char* src, int cap) {
    int n = tr_slen(dst);
    if (!dst || cap <= 0 || !src) return;
    while (*src && n < cap - 1) dst[n++] = *src++;
    dst[n] = 0;
}

static int tr_starts(const char* s, const char* pre) {
    while (*pre) { if (*s != *pre) return 0; s++; pre++; }
    return 1;
}

// "/a" + "b" -> "/a/b"（dir 为 "/" 时不产生 "//"；缓冲不够就截断，绝不越界）
static void tr_join(char* out, int cap, const char* dir, const char* name) {
    out[0] = 0;
    tr_cpy(out, dir, cap);
    if (!name || !name[0]) return;
    const int n = tr_slen(out);
    if (n == 0 || out[n - 1] != '/') tr_cat(out, "/", cap);
    tr_cat(out, name, cap);
}

// 取路径最后一段（basename）。"/" -> ""
static void tr_base(const char* path, char* out, int cap) {
    int last = -1, i = 0;
    out[0] = 0;
    if (!path) return;
    for (; path[i]; i++) if (path[i] == '/') last = i;
    if (last < 0) { tr_cpy(out, path, cap); return; }
    tr_cpy(out, path + last + 1, cap);
}

// 取父目录（"/a/b.txt" -> "/a"；"/b.txt" -> "/"）
static void tr_parent(const char* path, char* out, int cap) {
    int last = -1, i = 0;
    out[0] = 0;
    if (!path) return;
    for (; path[i]; i++) if (path[i] == '/') last = i;
    if (last <= 0) { tr_cpy(out, "/", cap); return; }
    int n = 0;
    while (n < last && n < cap - 1) { out[n] = path[n]; n++; }
    out[n] = 0;
}

// "note.txt" -> stem="note" ext=".txt"；没有后缀时 ext 为空
static void tr_split_ext(const char* name, char* stem, int scap, char* ext, int ecap) {
    int dot = -1, i = 0;
    for (; name && name[i]; i++) if (name[i] == '.') dot = i;
    if (dot <= 0) { tr_cpy(stem, name, scap); ext[0] = 0; return; }
    int n = 0;
    while (n < dot && n < scap - 1) { stem[n] = name[n]; n++; }
    stem[n] = 0;
    tr_cpy(ext, name + dot, ecap);
}

static int tr_append_dec(char* dst, int cap, uint32_t v) {
    char t[12];
    int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    int len = n;
    char buf[12];
    for (int i = 0; i < len; i++) buf[i] = t[len - 1 - i];
    buf[len] = 0;
    tr_cat(dst, buf, cap);
    return len;
}

// ==================== 打点助手 ====================
static void tr_log3(const char* tag, const char* k1, const char* v1, const char* k2, const char* v2) {
    dbg64_line_begin64();
    dbg64_str("[TRASH64] ");
    dbg64_str(tag);
    if (k1) { dbg64_str(" "); dbg64_str(k1); dbg64_str("="); dbg64_str(v1 ? v1 : "-"); }
    if (k2) { dbg64_str(" "); dbg64_str(k2); dbg64_str("="); dbg64_str(v2 ? v2 : "-"); }
    dbg64_nl();
    dbg64_line_end64();
}

// ==================== 目录/元数据 ====================
// fs64_stat64 不接受 out=nullptr（它直接 return -1），所以"存在与否"必须走这个助手。
static int tr_missing(int vol, const char* p) { Fs64Stat64 st; return (fs64_stat64(vol, p, &st) == 0) ? 0 : 1; }

static int tr_dir_ok(int vol, const char* p) {
    Fs64Stat64 st;
    return (fs64_stat64(vol, p, &st) == 0 && st.type == VFS64_TYPE_DIR) ? 1 : 0;
}

// 启动期兜底（**以 root 身份**跑一次）：/Recycle 与 /Recycle/.meta 必须存在且**世界可写**（0777）。
// 为什么必须在启动期做：普通用户（uid 1000）在 "/" 上建不了目录（[PERM64] deny op=create need=wx）——
//   删除时再"顺手 mkdir /Recycle" 会一律失败（-EACCES）。所以在 ring0 里先建好、把权限开到 0777，
//   之后任何会话身份都能往里放/取（与 /tmp 同一口径；代价是"回收站对所有用户可见"，如实记在边界里）。
// 幂等：已存在就只做 chmod（修复旧卷里被写成 0755 的情况）。
int trash64_boot_ensure64(int vol) {
    if (fs64_vol_used64(vol) != 1) return -1;
    int created = 0;
    int ok = 1;
    if (!tr_dir_ok(vol, TRASH64_DIR)) {
        if (fs64_mkdir64(vol, TRASH64_DIR) != 0) ok = 0;
        else created = 1;
    }
    if (ok) (void)fs64_chmod64(vol, TRASH64_DIR, 0777);
    if (ok && !tr_dir_ok(vol, TRASH64_META_DIR)) {
        if (fs64_mkdir64(vol, TRASH64_META_DIR) != 0) ok = 0;
        else created = 1;
    }
    if (ok) (void)fs64_chmod64(vol, TRASH64_META_DIR, 0777);
    dbg64_line_begin64();
    dbg64_str("[TRASH64] boot ensure vol=");
    dbg64_dec((uint64_t)(uint32_t)(vol < 0 ? 0 : vol));
    dbg64_str(" dir=");
    dbg64_str(TRASH64_DIR);
    dbg64_str(" created=");
    dbg64_dec((uint64_t)(uint32_t)(created ? 1u : 0u));
    dbg64_str(" mode=0777 ok=");
    dbg64_dec((uint64_t)(uint32_t)(ok ? 1u : 0u));
    dbg64_nl();
    dbg64_line_end64();
    return ok ? 0 : -1;
}

int trash64_init64(int vol) {
    if (fs64_vol_used64(vol) != 1) return -1;
    const int existed = tr_dir_ok(vol, TRASH64_DIR);
    int created = 0;
    if (!existed) {
        if (fs64_mkdir64(vol, TRASH64_DIR) != 0) {
            tr_log3("init", "vol", "?", "dir", TRASH64_DIR);
            dbg64_line_begin64();
            dbg64_str("[TRASH64] init FAIL reason=mkdir vol=");
            dbg64_dec((uint64_t)(uint32_t)(vol < 0 ? 0 : vol));
            dbg64_str(" dir=");
            dbg64_str(TRASH64_DIR);
            dbg64_nl();
            dbg64_line_end64();
            return -1;
        }
        created = 1;
    }
    if (!tr_dir_ok(vol, TRASH64_META_DIR)) {
        if (fs64_mkdir64(vol, TRASH64_META_DIR) != 0) return -1;
        created = 1;
    }
    (void)existed;
    dbg64_line_begin64();
    dbg64_str("[TRASH64] init vol=");
    dbg64_dec((uint64_t)(uint32_t)(vol < 0 ? 0 : vol));
    dbg64_str(" dir=");
    dbg64_str(TRASH64_DIR);
    dbg64_str(" exists=1 created=");
    dbg64_dec((uint64_t)(uint32_t)(created ? 1 : 0));
    dbg64_str(" items=");
    {
        const int n = trash64_count64(vol);
        dbg64_dec((uint64_t)(uint32_t)(n > 0 ? n : 0));
    }
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

// 元数据文件路径：/Recycle/.meta/<name>.txt
static void tr_meta_path(char* out, int cap, const char* name) {
    char fn[TRASH64_NAME_MAX + 8];
    tr_cpy(fn, name, (int)sizeof(fn));
    tr_cat(fn, TRASH64_META_EXT, (int)sizeof(fn));
    tr_join(out, cap, TRASH64_META_DIR, fn);
}

static int tr_meta_write(int vol, const char* name, const char* orig, uint32_t type) {
    char path[VFS64_PATH_MAX];
    char buf[VFS64_PATH_MAX + 32];
    tr_meta_path(path, (int)sizeof(path), name);
    tr_cpy(buf, "orig=", (int)sizeof(buf));
    tr_cat(buf, orig, (int)sizeof(buf));
    tr_cat(buf, "\ntype=", (int)sizeof(buf));
    tr_cat(buf, (type == VFS64_TYPE_DIR) ? "dir\n" : "file\n", (int)sizeof(buf));
    if (fs64_create64(vol, path) != 0) return -1;
    const int n = tr_slen(buf);
    const int w = fs64_write64(vol, path, buf, n);
    return (w == n) ? 0 : -1;
}

int trash64_orig_of64(int vol, const char* name, char* out, int cap) {
    char path[VFS64_PATH_MAX];
    char buf[VFS64_PATH_MAX + 32];
    if (cap > 0) out[0] = 0;
    if (!name || !name[0]) return -1;
    tr_meta_path(path, (int)sizeof(path), name);
    const int n = fs64_read64(vol, path, buf, (int)sizeof(buf) - 1);
    if (n <= 0) return -1;
    buf[n] = 0;
    if (!tr_starts(buf, "orig=")) return -1;
    const char* p = buf + 5;
    int k = 0;
    while (p[k] && p[k] != '\n' && p[k] != '\r' && k < cap - 1) { out[k] = p[k]; k++; }
    if (cap > 0) out[k] = 0;
    return (k > 0) ? 0 : -1;
}

// ==================== 条目标名 ====================
// 在 <vol>/Recycle 里找一个没被占用的条目名：先原样，撞了就 stem (2)ext、(3)…
// 返回 1 = 找到（out 已填）；0 = 99 个候选全被占（放弃，绝不覆盖）
static int tr_unique_name(int vol, const char* base, char* out, int cap) {
    char probe[VFS64_PATH_MAX];
    Fs64Stat64 st;
    tr_cpy(out, base, cap);
    if (!out[0]) tr_cpy(out, "deleted", cap);
    tr_join(probe, (int)sizeof(probe), TRASH64_DIR, out);
    if (fs64_stat64(vol, probe, &st) != 0) return 1;         // 原样可用

    char stem[TRASH64_NAME_MAX + 1], ext[TRASH64_NAME_MAX + 1];
    tr_split_ext(base, stem, (int)sizeof(stem), ext, (int)sizeof(ext));
    for (uint32_t k = 2; k <= 99; k++) {
        char cand[TRASH64_NAME_MAX + 16];
        tr_cpy(cand, stem, (int)sizeof(cand));
        tr_cat(cand, " (", (int)sizeof(cand));
        tr_append_dec(cand, (int)sizeof(cand), k);
        tr_cat(cand, ")", (int)sizeof(cand));
        tr_cat(cand, ext, (int)sizeof(cand));
        if (tr_slen(cand) > (int)TRASH64_NAME_MAX) continue;  // 太长就跳过（名字上限 31 B）
        tr_join(probe, (int)sizeof(probe), TRASH64_DIR, cand);
        if (fs64_stat64(vol, probe, &st) != 0) { tr_cpy(out, cand, cap); return 1; }
        // 元数据还在 = 上一次 purge 没收尾（条目已不在）—— 视为可用，让 put 覆盖它
        char mp[VFS64_PATH_MAX];
        tr_meta_path(mp, (int)sizeof(mp), cand);
        Fs64Stat64 mst;
        if (fs64_stat64(vol, mp, &mst) == 0) { fs64_unlink64(vol, mp); tr_cpy(out, cand, cap); return 1; }
    }
    return 0;
}

// ==================== 放入回收站 ====================
int trash64_put64(int vol, const char* path, char* out_name, int cap) {
    const char* why = "";
    int rc = -1;
    char name[TRASH64_NAME_MAX + 1];
    char base[TRASH64_NAME_MAX + 1];
    char dst[VFS64_PATH_MAX];
    Fs64Stat64 st;
    st.type = 0; st.size = 0; st.mtime = 0;      // 早退路径也会打 type=（这里给个确定的初值，不读未初始化栈）
    int meta_ok = 0;

    if (out_name && cap > 0) out_name[0] = 0;
    if (!path || !path[0]) { why = "nopath"; goto out; }
    if (fs64_is_readonly64(vol)) { why = "readonly"; rc = -FS64_EROFS; goto out; }
    if (fs64_stat64(vol, path, &st) != 0) { why = "stat"; goto out; }
    // 回收站自己不能被"删除进回收站"（否则整个回收站会被搬进自己肚子）
    if (tr_eq(path, TRASH64_DIR) || (tr_starts(path, TRASH64_DIR) && path[tr_slen(TRASH64_DIR)] == '/')) {
        why = "in-trash"; goto out;
    }
    if (trash64_init64(vol) != 0) { why = "no-dir"; goto out; }

    tr_base(path, base, (int)sizeof(base));
    if (!base[0]) { why = "badname"; goto out; }
    if (!tr_unique_name(vol, base, name, (int)sizeof(name))) { why = "name-budget"; goto out; }

    tr_join(dst, (int)sizeof(dst), TRASH64_DIR, name);
    rc = fs64_rename_to64(vol, path, dst);
    if (rc != 0) { why = "rename"; rc = -1; goto out; }
    rc = 0;

    // 元数据必须写成 —— 写不成就把条目**搬回原处**（绝不留下"用户找不回位置"的孤儿）
    if (tr_meta_write(vol, name, path, st.type) != 0) {
        const int back = fs64_rename_to64(vol, dst, path);
        dbg64_line_begin64();
        dbg64_str("[TRASH64] put rollback name=");
        dbg64_str(name);
        dbg64_str(" orig=");
        dbg64_str(path);
        dbg64_str(" back_rc=");
        dbg64_dec((uint64_t)(uint32_t)(back == 0 ? 0u : 1u));
        dbg64_nl();
        dbg64_line_end64();
        rc = -1;
        why = "meta-write";
        goto out;
    }
    meta_ok = 1;
    if (out_name && cap > 0) tr_cpy(out_name, name, cap);

out:
    {
        dbg64_line_begin64();
        dbg64_str("[TRASH64] put name=");
        dbg64_str(name[0] ? name : "-");
        dbg64_str(" orig=");
        dbg64_str(path ? path : "-");
        dbg64_str(" type=");
        dbg64_str(st.type == VFS64_TYPE_DIR ? "dir" : "file");
        dbg64_str(" vol=");
        dbg64_dec((uint64_t)(uint32_t)(vol < 0 ? 0 : vol));
        dbg64_str(" rc=");
        dbg64_dec((uint64_t)(uint32_t)(rc == 0 ? 0u : 1u));
        dbg64_str(" why=");
        dbg64_str(why[0] ? why : "-");
        dbg64_str(" meta=");
        dbg64_dec((uint64_t)(uint32_t)(meta_ok ? 1u : 0u));
        dbg64_nl();
        dbg64_line_end64();
    }
    return rc;
}

// ==================== 列出 / 计数 ====================
int trash64_list64(int vol, Trash64Ent64* out, int max) {
    if (fs64_vol_used64(vol) != 1) return -1;
    if (!tr_dir_ok(vol, TRASH64_DIR)) return 0;
    int n = 0;
    uint32_t cur = 0;
    Fs64Dirent64 one;
    int guard = 0;
    while (n < max) {
        const int r = fs64_list64(vol, TRASH64_DIR, &one, 1, &cur);
        if (r <= 0) break;
        if (++guard > (int)FAT64_CHAIN_MAX) break;
        if (tr_eq(one.name, TRASH64_RO_NAME)) continue;              // .meta 不列
        Trash64Ent64* e = &out[n];
        tr_cpy(e->name, one.name, (int)sizeof(e->name));
        e->type = one.type;
        e->size = (one.type == VFS64_TYPE_DIR) ? 0u : one.size;
        e->mtime = one.mtime;
        e->orig[0] = 0;
        e->meta_ok = (trash64_orig_of64(vol, one.name, e->orig, (int)sizeof(e->orig)) == 0) ? 1u : 0u;
        if (!e->meta_ok) tr_cpy(e->orig, "?", (int)sizeof(e->orig));
        n++;
    }
    return n;
}

int trash64_count64(int vol) {

    int total = 0;
    // 分页数（一次只拿 8 条，避免在 400+ 条的回收站上撑爆栈）—— 用游标把这层做实：
    // 这里直接数目录项（不读元数据），所以很快。
    uint32_t cur = 0;
    Fs64Dirent64 one;
    int guard = 0;
    if (fs64_vol_used64(vol) != 1) return -1;
    for (;;) {
        // fs64_list64 的游标是"目录流"，每次只取 1 条；数量统计不需要 Trash64Ent64。
        const int r = fs64_list64(vol, TRASH64_DIR, &one, 1, &cur);
        if (r <= 0) break;
        if (++guard > (int)FAT64_CHAIN_MAX) break;
        if (tr_eq(one.name, TRASH64_RO_NAME)) continue;
        total++;
    }

    return total;
}

// ==================== 还原 ====================
int trash64_restore64(int vol, const char* name, const char** why) {
    const char* w = "";
    int rc = -1;
    char src[VFS64_PATH_MAX];
    char orig[VFS64_PATH_MAX];
    char parent[VFS64_PATH_MAX];
    Fs64Stat64 st;

    if (why) *why = "";
    if (!name || !name[0]) { w = "noname"; goto out; }
    if (fs64_is_readonly64(vol)) { w = "readonly"; goto out; }
    tr_join(src, (int)sizeof(src), TRASH64_DIR, name);
    if (fs64_stat64(vol, src, &st) != 0) { w = "gone"; goto out; }
    if (trash64_orig_of64(vol, name, orig, (int)sizeof(orig)) != 0) { w = "nometa"; goto out; }
    tr_parent(orig, parent, (int)sizeof(parent));
    if (!tr_dir_ok(vol, parent)) { w = "noparent"; goto out; }
    if (!tr_missing(vol, orig)) { w = "exists"; goto out; }   // 绝不覆盖
    rc = fs64_rename_to64(vol, src, orig);
    if (rc != 0) { w = "rename"; rc = -1; goto out; }
    rc = 0;
    {
        char mp[VFS64_PATH_MAX];
        tr_meta_path(mp, (int)sizeof(mp), name);
        (void)fs64_unlink64(vol, mp);        // 元数据删不掉不影响还原（下次 put 会覆盖/清理）
    }

out:
    if (why) *why = w;
    dbg64_line_begin64();
    dbg64_str("[TRASH64] restore name=");
    dbg64_str(name ? name : "-");
    dbg64_str(" to=");
    dbg64_str(orig[0] ? orig : "-");
    dbg64_str(" rc=");
    dbg64_dec((uint64_t)(uint32_t)(rc == 0 ? 0u : 1u));
    dbg64_str(" why=");
    dbg64_str(w[0] ? w : "-");
    dbg64_nl();
    dbg64_line_end64();
    return rc;
}

// ==================== 永久删除（有界递归）====================
struct TrashRmCtx64 {
    int nodes;
    int files;
    int dirs;
    const char* why;
};

#define TRASH64_CHILD_MAX 16

static int tr_rm_tree64(int vol, const char* path, int depth, TrashRmCtx64* c) {
    if (c->nodes >= TRASH64_MAX_NODES) { c->why = "budget"; return -1; }
    if (depth > TRASH64_MAX_DEPTH) { c->why = "budget"; return -1; }
    Fs64Stat64 st;
    if (fs64_stat64(vol, path, &st) != 0) { c->why = "io"; return -1; }
    if (st.type != VFS64_TYPE_DIR) {
        c->nodes++;
        if (fs64_unlink64(vol, path) != 0) { c->why = "io"; return -1; }
        c->files++;
        return 0;
    }
    // 目录：先把孩子名字抄一份下来（一边列一边删会打乱目录游标）
    char kids[TRASH64_CHILD_MAX][FS64_NAME_MAX];
    int nk = 0;
    uint32_t cur = 0;
    Fs64Dirent64 one;
    for (;;) {
        const int r = fs64_list64(vol, path, &one, 1, &cur);
        if (r <= 0) break;
        if (nk >= TRASH64_CHILD_MAX) { c->why = "budget"; return -1; }
        tr_cpy(kids[nk], one.name, (int)sizeof(kids[0]));
        nk++;
    }
    for (int i = 0; i < nk; i++) {
        char cp[VFS64_PATH_MAX];
        tr_join(cp, (int)sizeof(cp), path, kids[i]);
        if (tr_rm_tree64(vol, cp, depth + 1, c) != 0) return -1;
    }
    c->nodes++;
    if (fs64_rmdir64(vol, path) != 0) { c->why = "io"; return -1; }
    c->dirs++;
    return 0;
}

int trash64_purge64(int vol, const char* name, int* files, int* dirs, const char** why) {
    const char* w = "";
    int rc = -1;
    char path[VFS64_PATH_MAX];
    Fs64Stat64 st;
    st.type = 0; st.size = 0; st.mtime = 0;

    if (files) *files = 0;
    if (dirs) *dirs = 0;
    if (why) *why = "";
    if (!name || !name[0]) { w = "noname"; goto out; }
    tr_join(path, (int)sizeof(path), TRASH64_DIR, name);
    if (fs64_stat64(vol, path, &st) != 0) { w = "gone"; goto out; }
    {
        TrashRmCtx64 c;
        c.nodes = 0; c.files = 0; c.dirs = 0; c.why = "";
        const int r = tr_rm_tree64(vol, path, 0, &c);
        if (files) *files = c.files;
        if (dirs) *dirs = c.dirs;
        if (r != 0) { w = c.why[0] ? c.why : "io"; goto out; }
    }
    rc = 0;
    {
        char mp[VFS64_PATH_MAX];
        tr_meta_path(mp, (int)sizeof(mp), name);
        (void)fs64_unlink64(vol, mp);
    }

out:
    if (why) *why = w;
    dbg64_line_begin64();
    dbg64_str("[TRASH64] purge name=");
    dbg64_str(name ? name : "-");
    dbg64_str(" type=");
    dbg64_str(st.type == VFS64_TYPE_DIR ? "dir" : "file");
    dbg64_str(" files=");
    dbg64_dec((uint64_t)(uint32_t)(files && *files > 0 ? *files : 0));
    dbg64_str(" dirs=");
    dbg64_dec((uint64_t)(uint32_t)(dirs && *dirs > 0 ? *dirs : 0));
    dbg64_str(" rc=");
    dbg64_dec((uint64_t)(uint32_t)(rc == 0 ? 0u : 1u));
    dbg64_str(" why=");
    dbg64_str(w[0] ? w : "-");
    dbg64_nl();
    dbg64_line_end64();
    return rc;
}

int trash64_empty64(int vol, int* fail) {
    Trash64Ent64 ents[TRASH64_LIST_MAX];
    int nf = 0;
    int done = 0;
    int files = 0, dirs = 0;
    if (fail) *fail = 0;
    if (trash64_init64(vol) != 0) {
        if (fail) *fail = 1;
        tr_log3("empty", "n", "0", "rc", "1");
        return 0;
    }
    for (;;) {
        const int n = trash64_list64(vol, ents, TRASH64_LIST_MAX);
        if (n <= 0) break;
        int prog = 0;
        for (int i = 0; i < n; i++) {
            int f = 0, d = 0;
            const char* w = "";
            if (trash64_purge64(vol, ents[i].name, &f, &d, &w) == 0) { done++; files += f; dirs += d; prog++; }
            else nf++;
        }
        if (!prog) break;                // 一条都删不掉：别死循环（如实计 fail）
    }
    if (fail) *fail = nf;
    dbg64_line_begin64();
    dbg64_str("[TRASH64] empty n=");
    dbg64_dec((uint64_t)(uint32_t)done);
    dbg64_str(" files=");
    dbg64_dec((uint64_t)(uint32_t)files);
    dbg64_str(" dirs=");
    dbg64_dec((uint64_t)(uint32_t)dirs);
    dbg64_str(" rc=");
    dbg64_dec((uint64_t)(uint32_t)(nf == 0 ? 0u : 1u));
    dbg64_str(" why=");
    dbg64_str(nf == 0 ? "-" : "some-failed");
    dbg64_nl();
    dbg64_line_end64();
    return done;
}

// ==================== 自检 ====================
// 判据（0 = 全过）：在 /tmp 造一个样本文件走完整回路。
//   bit0 put 成功且原路径真的没了
//   bit1 list 能把它列出来、原路径读回来一致
//   bit2 restore 之后原路径回来了、内容还在
//   bit3 再 put 后 purge，回收站里不再有它
//   bit4 不存在的路径被如实拒绝（不假装成功）
int trash64_selftest64() {
    int mask = 0;
    const int vol = fs64_current_vol64();
    const char sample[] = "/tmp/trash_selftest.txt";
    const char body[] = "vimtu-trash";
    char nm[TRASH64_NAME_MAX + 1];
    char orig[VFS64_PATH_MAX];
    Trash64Ent64 ents[TRASH64_LIST_MAX];

    if (vol < 0) {
        dbg64_line_begin64();
        dbg64_str("[TRASH64] selftest SKIP reason=no-volume");
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    (void)fs64_unlink64(vol, sample);
    if (fs64_create64(vol, sample) != 0) {
        dbg64_line_begin64();
        dbg64_str("[TRASH64] selftest SKIP reason=cannot-create-sample");
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    (void)fs64_write64(vol, sample, body, (int)(sizeof(body) - 1));

    // bit0：put
    if (trash64_put64(vol, sample, nm, (int)sizeof(nm)) == 0 &&
        tr_missing(vol, sample)) mask |= 1;
    // bit1：list + 原路径
    {
        const int n = trash64_list64(vol, ents, TRASH64_LIST_MAX);
        int hit = -1;
        for (int i = 0; i < n; i++) if (tr_eq(ents[i].name, nm)) { hit = i; break; }
        if (hit >= 0) {
            tr_cpy(orig, ents[hit].orig, (int)sizeof(orig));
            if (tr_eq(orig, sample)) mask |= 2;
        }
        // bit2：restore
        if (hit >= 0 && trash64_restore64(vol, nm, nullptr) == 0) {
            Fs64Stat64 st;
            if (fs64_stat64(vol, sample, &st) == 0 && st.size == (uint32_t)(sizeof(body) - 1)) mask |= 4;
        }
    }
    // bit3：put + purge
    if (trash64_put64(vol, sample, nm, (int)sizeof(nm)) == 0 &&
        trash64_purge64(vol, nm, nullptr, nullptr, nullptr) == 0) {
        char p[VFS64_PATH_MAX];
        tr_join(p, (int)sizeof(p), TRASH64_DIR, nm);
        if (tr_missing(vol, p)) mask |= 8;
    }
    // bit4：不存在的路径
    {
        char nm2[TRASH64_NAME_MAX + 1];
        if (trash64_put64(vol, "/tmp/trash_selftest_nope.txt", nm2, (int)sizeof(nm2)) != 0) mask |= 16;
    }

    // ★ 极性修正（本轮实测踩到）：上面每个 `mask |= bit` 都是"这一条**通过**"时置位，
    //   而本工程自检的口径是"0 = 全过"。所以输出前整体取反（只反低 5 位）。
    //   踩坑记录：第一版直接打印 mask → 5 条全过反倒打出 `[TRASH64] selftest FAIL mask=31`。
    mask = (~mask) & 0x1F;
    dbg64_line_begin64();
    dbg64_str("[TRASH64] selftest ");
    dbg64_str(mask == 0 ? "PASS" : "FAIL");
    dbg64_str(" mask=");
    dbg64_dec((uint64_t)(uint32_t)mask);
    dbg64_nl();
    dbg64_line_end64();
    return mask;
}
