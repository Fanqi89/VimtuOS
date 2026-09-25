// fs64.cpp - 统一文件系统分派层实现（设计/契约见 kernel/fs64.h）
//
// 分派规则只有一句话：**按统一卷号找到卷类型，VimtuFS2 转 vfs64_*_on64 / FAT32 转 fat64_***。
// 所有写操作先在入口处按 readonly 拦一次（FAT 直接 -FS64_EROFS），所以上层永远看不到
// "写了一半"的 FAT 卷；卷表是 drive64 扫描时的登记快照 + 每次查询时的只读刷新。
#include "fs64.h"
#include "debug64.h"

// ==================== 卷表 ====================
static Fs64Vol64 g_vols[FS64_VOL_MAX];
static int       g_cur = FS64_VOL_NONE;

// 只读卷上的写操作：统一打点（绝不假装成功，也绝不写盘）
static void log_reject_ro(int vol, const char* op, const char* path) {
    dbg64_line_begin64();
    dbg64_str("[FS64] reject vol=");
    dbg64_dec((uint64_t)(vol < 0 ? 0 : vol));
    dbg64_str(" ro=1 op=");
    dbg64_str(op);
    if (path) {
        dbg64_str(" path=");
        dbg64_str(path);
    }
    dbg64_str(" (read-only FAT32: write/delete/rename/mkdir are not implemented)");
    dbg64_nl();
    dbg64_line_end64();
}

// vfs64 槽 -> 卷表登记（幂等；只登记"确实挂载了"的槽）
static bool ensure_vfs64(int slot) {
    if (slot < 0 || slot >= (int)VFS64_SLOT_MAX) return false;
    if (vfs64_slot_used64(slot) != 1) return false;
    Fs64Vol64& v = g_vols[slot];
    int drive = -1;
    uint32_t lba = 0;
    Vfs64VolInfo64 vi;
    const int ok = (vfs64_slot_info64(slot, &drive, &lba, &vi) == 0);
    v.kind = FS64_KIND_VIMTUFS2;
    v.readonly = 0;
    v.disk = drive;
    v.start_lba = lba;
    v.vfs_slot = slot;
    v.fat_slot = -1;
    v.total_known = ok ? 1 : 0;
    v.free_known = ok ? 1 : 0;
    v.total_kb = ok ? ((uint64_t)vi.blocks / 2u) : 0;
    v.free_kb = ok ? ((uint64_t)vi.free_blocks / 2u) : 0;
    for (int i = 0; i < 16; i++) v.fs[i] = 0;
    const char* nm = "VimtuFS2";
    for (int i = 0; nm[i] && i < 15; i++) v.fs[i] = nm[i];
    return true;
}
static int fat_slot_of(int vol) {
    if (vol < FS64_VOL_FAT_BASE || vol >= FS64_VOL_MAX) return -1;
    return vol - FS64_VOL_FAT_BASE;
}
int fs64_init64() {
    for (int s = 0; s < (int)VFS64_SLOT_MAX; s++) {
        if (!ensure_vfs64(s)) {
            // 槽空/未挂载：清掉旧登记（重扫后卷可能被换掉）
            for (uint32_t i = 0; i < sizeof(g_vols[s]); i++) ((uint8_t*)&g_vols[s])[i] = 0;
        }
    }
    for (int v = FS64_VOL_FAT_BASE; v < FS64_VOL_MAX; v++) {
        const int fs = fat_slot_of(v);
        if (fs < 0 || fat64_vol_used64(fs) != 1) {
            for (uint32_t i = 0; i < sizeof(g_vols[v]); i++) ((uint8_t*)&g_vols[v])[i] = 0;
        }
    }
    const int cur = vfs64_current_slot64();
    if (cur >= 0 && cur < (int)VFS64_SLOT_MAX && g_vols[cur].kind == FS64_KIND_VIMTUFS2) g_cur = cur;
    else if (g_cur >= 0 && g_cur < FS64_VOL_MAX && g_vols[g_cur].kind == FS64_KIND_NONE) g_cur = FS64_VOL_NONE;
    return 0;
}
int fs64_vol_count64() {
    int n = 0;
    for (int v = 0; v < FS64_VOL_MAX; v++) if (g_vols[v].kind != FS64_KIND_NONE) n++;
    return n;
}
int fs64_vol_used64(int vol) {
    return (vol >= 0 && vol < FS64_VOL_MAX && g_vols[vol].kind != FS64_KIND_NONE) ? 1 : 0;
}
int fs64_find_vfs64(int slot) {
    if (slot < 0 || slot >= (int)VFS64_SLOT_MAX) return -1;
    if (!ensure_vfs64(slot)) return -1;
    return slot;
}
int fs64_find_fat64(int disk, uint32_t lba) {
    for (int fp = 0; fp < FAT64_VOL_MAX; fp++) {
        const int vol = FS64_VOL_FAT_BASE + fp;
        if (g_vols[vol].kind != FS64_KIND_FAT32) continue;
        if (g_vols[vol].disk == disk && g_vols[vol].start_lba == lba) return vol;
    }
    return -1;
}
int fs64_vol_info64(int vol, Fs64Vol64* out) {
    if (!out) return -1;
    if (vol < 0) vol = fs64_current_vol64();
    if (vol < 0 || vol >= FS64_VOL_MAX || g_vols[vol].kind == FS64_KIND_NONE) return -1;
    Fs64Vol64& v = g_vols[vol];
    if (v.kind == FS64_KIND_VIMTUFS2) {
        (void)ensure_vfs64(v.vfs_slot);                       // 刷新容量（数据盘写盘后可用空间会变）
    } else {
        Fat64Info64 fi;
        if (fat64_vol_info64(v.fat_slot, &fi) == 0) {
            v.total_known = 1;
            v.total_kb = ((uint64_t)fi.clusters * fi.cluster_bytes) / 1024u;
            if (fi.free_clusters != 0xFFFFFFFFu) {
                v.free_known = 1;
                v.free_kb = ((uint64_t)fi.free_clusters * fi.cluster_bytes) / 1024u;
            } else {
                v.free_known = 0;
                v.free_kb = 0;
            }
        }
    }
    *out = v;
    return 0;
}
int fs64_mount_fat64(int disk, uint32_t lba, Fs64Vol64* out) {
    const int existing = fs64_find_fat64(disk, lba);
    int fp = fat64_mount_find64(disk, lba);
    if (fp < 0 && fat64_mount64(disk, lba, &fp) != 0) return -1;
    if (fp < 0 || fp >= FAT64_VOL_MAX) return -1;
    const int vol = FS64_VOL_FAT_BASE + fp;
    Fat64Info64 fi;
    if (fat64_vol_info64(fp, &fi) != 0) return -1;
    Fs64Vol64& v = g_vols[vol];
    for (uint32_t i = 0; i < sizeof(v); i++) ((uint8_t*)&v)[i] = 0;
    v.kind = FS64_KIND_FAT32;
    v.readonly = 1;
    v.disk = disk;
    v.start_lba = lba;
    v.vfs_slot = -1;
    v.fat_slot = fp;
    v.total_known = 1;
    v.total_kb = ((uint64_t)fi.clusters * fi.cluster_bytes) / 1024u;
    if (fi.free_clusters != 0xFFFFFFFFu) {
        v.free_known = 1;
        v.free_kb = ((uint64_t)fi.free_clusters * fi.cluster_bytes) / 1024u;
    }
    const char* nm = "FAT32";
    for (int i = 0; nm[i] && i < 15; i++) v.fs[i] = nm[i];
    if (existing < 0) {
        dbg64_line_begin64();
        dbg64_str("[FS64] mount fat vol=");
        dbg64_dec((uint64_t)vol);
        dbg64_str(" disk=");
        dbg64_dec((uint64_t)disk);
        dbg64_str(" lba=");
        dbg64_dec(lba);
        dbg64_str(" clusters=");
        dbg64_dec(fi.clusters);
        dbg64_str(" spc=");
        dbg64_dec(fi.spc);
        dbg64_str(" ro=1");
        dbg64_nl();
        dbg64_line_end64();
    }
    if (out) *out = v;
    return vol;
}
int fs64_activate64(int vol) {
    if (vol < 0 || vol >= FS64_VOL_MAX || g_vols[vol].kind == FS64_KIND_NONE) {
        dbg64_line_begin64();
        dbg64_str("[FS64] activate FAILED vol=");
        dbg64_dec((uint64_t)(vol < 0 ? 0 : vol));
        dbg64_str(" reason=no-such-volume");
        dbg64_nl();
        dbg64_line_end64();
        return -1;
    }
    if (g_vols[vol].kind == FS64_KIND_VIMTUFS2) {
        if (vfs64_activate_slot64(g_vols[vol].vfs_slot) != 0) return -1;
    }
    g_cur = vol;
    return 0;
}
int fs64_current_vol64() {
    if (g_cur >= 0 && g_cur < FS64_VOL_MAX && g_vols[g_cur].kind != FS64_KIND_NONE) {
        if (g_vols[g_cur].kind == FS64_KIND_FAT32) return g_cur;
        if (vfs64_current_slot64() == g_vols[g_cur].vfs_slot) return g_cur;
    }
    const int s = vfs64_current_slot64();
    if (s >= 0 && s < (int)VFS64_SLOT_MAX && ensure_vfs64(s)) {
        g_cur = s;
        return s;
    }
    return FS64_VOL_NONE;
}
int fs64_is_readonly64(int vol) {
    if (vol < 0) vol = fs64_current_vol64();
    if (vol < 0 || vol >= FS64_VOL_MAX || g_vols[vol].kind == FS64_KIND_NONE) return 1;
    return g_vols[vol].readonly ? 1 : 0;
}
uint32_t fs64_vol_kind64(int vol) {
    if (vol < 0) vol = fs64_current_vol64();
    if (vol < 0 || vol >= FS64_VOL_MAX) return FS64_KIND_NONE;
    return g_vols[vol].kind;
}
// 入口解析：-1 = 当前卷；非法 -> -1
static int resolve_vol(int vol) {
    if (vol < 0) vol = fs64_current_vol64();
    if (vol < 0 || vol >= FS64_VOL_MAX) return -1;
    if (vol < (int)VFS64_SLOT_MAX) (void)ensure_vfs64(vol);
    return (g_vols[vol].kind != FS64_KIND_NONE) ? vol : -1;
}

// ==================== 目录项 / stat ====================
static void fill_from_fat(Fs64Dirent64* out, const Fat64Entry64* e) {
    for (uint32_t i = 0; i < sizeof(*out); i++) ((uint8_t*)out)[i] = 0;
    int i = 0;
    for (; e->name[i] && i < (int)sizeof(out->name) - 1; i++) out->name[i] = e->name[i];
    out->name[i] = 0;
    out->index = 0;
    out->type = (e->attr & 0x10u) ? VFS64_TYPE_DIR : VFS64_TYPE_FILE;
    out->size = e->size;
    out->mtime = e->mtime;
    out->kind = (out->type == VFS64_TYPE_DIR)
                ? VFS64_KIND_DIR
                : vfs64_kind_by_name64(VFS64_TYPE_FILE, e->name, (uint32_t)i);
    out->attr = e->attr;
    out->uid = 0;                                             // ★ P4：FAT 没有属主（恒 0 = root，如实标注）
    out->gid = 0;
    out->mode = (out->type == VFS64_TYPE_DIR) ? (VFS64_S_IFDIR | 0755u) : (VFS64_S_IFREG | 0644u);
}
// 名字长度（FAT 条目给的是 NUL 结尾字符串）
static int fat_name_len(const char* s) {
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}
static void fill_from_vfs(Fs64Dirent64* out, const Vfs64Dirent64* e) {
    for (uint32_t i = 0; i < sizeof(*out); i++) ((uint8_t*)out)[i] = 0;
    int i = 0;
    for (; e->name[i] && i < (int)sizeof(out->name) - 1; i++) out->name[i] = e->name[i];
    out->name[i] = 0;
    out->index = e->index;
    out->type = e->type;
    out->size = e->size;
    out->mtime = e->mtime;
    out->kind = e->kind;
    out->attr = 0;
    out->uid = e->uid;                                            // ★ P4
    out->gid = e->gid;
    out->mode = e->mode;
}

// ==================== ★ P4：统一卷 -> vfs64 的"按当前身份"调用范围 ====================
// 设计（一句话）：**当前卷**直接用非 on64 的 vfs64 API（权限判定照实），跨卷时临时切到目标槽
//   但**不把身份置成 root**（vfs64_scope_enter64/leave64 只换卷）。系统组件固定写系统卷的路径另走
//   vfs64_*_on64()（那一族期间身份 = root，见 kernel/vfs64.cpp 的 Vfs64SlotGuard 说明）。
struct Fs64VfsScope64 {
    int  saved;
    bool entered;
    bool ok;
    explicit Fs64VfsScope64(int vfs_slot) {
        saved = -1;
        entered = false;
        ok = false;
        if (vfs_slot < 0 || vfs_slot >= (int)VFS64_SLOT_MAX) return;
        if (vfs64_current_slot64() == vfs_slot) { ok = true; return; }       // 已经是当前卷
        if (vfs64_scope_enter64(vfs_slot, &saved) != 0) return;
        entered = true;
        ok = true;
    }
    ~Fs64VfsScope64() { if (entered) vfs64_scope_leave64(saved); }
};

// ==================== 统一操作 ====================
int fs64_list64(int vol, const char* path, Fs64Dirent64* out, int max, uint32_t* cursor) {
    const int v = resolve_vol(vol);
    if (v < 0 || !out || max <= 0) return -1;
    if (g_vols[v].kind == FS64_KIND_VIMTUFS2) {
        Fs64VfsScope64 sc(g_vols[v].vfs_slot);
        if (!sc.ok) return -1;
        Vfs64Dirent64 tmp;                                    // 逐个转（vfs 的游标直接透传）
        const int n = vfs64_list64(path, &tmp, 1, cursor);
        if (n < 0) return -1;
        if (n == 0) return 0;
        fill_from_vfs(out, &tmp);
        return 1;
    }
    static Fat64Entry64 tmp[32];                              // FAT：一次最多转 32 条（游标透传）
    int want = max;
    if (want > 32) want = 32;
    const int n = fat64_list64(g_vols[v].fat_slot, path, tmp, want, cursor);
    if (n <= 0) return n;
    for (int i = 0; i < n; i++) fill_from_fat(&out[i], &tmp[i]);
    return n;
}

int fs64_ls64(int vol, const char* path, char names[][VFS64_LS_NAME_BUF], int max, uint32_t* sizes) {
    const int v = resolve_vol(vol);
    if (v < 0 || !names || max <= 0) return -1;
    if (g_vols[v].kind == FS64_KIND_VIMTUFS2) {
        Fs64VfsScope64 sc(g_vols[v].vfs_slot);
        if (!sc.ok) return -1;
        return vfs64_ls(path, names, max, sizes);
    }

    static Fat64Entry64 tmp[8];
    int got = 0;
    uint32_t cursor = 0;
    for (;;) {
        const int n = fat64_list64(g_vols[v].fat_slot, path, tmp, 8, &cursor);
        if (n < 0) return -1;
        if (n == 0) break;
        for (int i = 0; i < n && got < max; i++) {
            int k = 0;
            for (; tmp[i].name[k] && k < (int)VFS64_LS_NAME_BUF - 1; k++) names[got][k] = tmp[i].name[k];
            names[got][k] = 0;
            if (sizes) sizes[got] = ((tmp[i].attr & 0x10u) ? 0u : tmp[i].size);
            got++;
        }
        if (got >= max) break;
        if (n < 8) break;
    }
    return got;
}

int fs64_stat64(int vol, const char* path, Fs64Stat64* out) {
    const int v = resolve_vol(vol);
    if (v < 0 || !out) return -1;
    if (g_vols[v].kind == FS64_KIND_VIMTUFS2) {
        Fs64VfsScope64 sc(g_vols[v].vfs_slot);
        if (!sc.ok) return -1;
        Vfs64Info64 vi;
        const int rc = vfs64_stat64(path, &vi);
        if (rc != 0) return rc;                               // ★ P4：-EACCES 原样透传（缺 x 进不去）
        out->type = vi.type;
        out->size = vi.size;
        out->mtime = vi.mtime;
        out->kind = vi.kind;
        out->attr = 0;
        out->uid = vi.uid;                                    // ★ P4
        out->gid = vi.gid;
        out->mode = vi.mode;
        return 0;
    }
    Fat64Entry64 fe;
    if (fat64_stat64(g_vols[v].fat_slot, path, &fe) != 0) return -1;
    out->type = (fe.attr & 0x10u) ? VFS64_TYPE_DIR : VFS64_TYPE_FILE;
    out->size = fe.size;
    out->mtime = fe.mtime;
    out->kind = (out->type == VFS64_TYPE_DIR)
                ? VFS64_KIND_DIR
                : vfs64_kind_by_name64(VFS64_TYPE_FILE, fe.name, (uint32_t)fat_name_len(fe.name));
    out->attr = fe.attr;
    out->uid = 0;                                             // FAT 没有属主（如实：恒 root/0）
    out->gid = 0;
    out->mode = (out->type == VFS64_TYPE_DIR) ? (VFS64_S_IFDIR | 0755u) : (VFS64_S_IFREG | 0644u);
    return 0;
}
// ★ P4：access(2) 的按卷变体（fd64 在 open 时用它做 r/w 判定；FAT 卷没有权限模型 -> 恒 0）
int fs64_access64(int vol, const char* path, uint32_t mask) {
    const int v = resolve_vol(vol);
    if (v < 0) return -1;
    if (g_vols[v].kind != FS64_KIND_VIMTUFS2) return 0;
    Fs64VfsScope64 sc(g_vols[v].vfs_slot);
    if (!sc.ok) return -1;
    return vfs64_access64(path, mask);
}
// ★ P4：chmod/chown 的按卷变体（终端命令用；越权错误码原样透传）
int fs64_chmod64(int vol, const char* path, uint32_t mode) {
    const int v = resolve_vol(vol);
    if (v < 0) return -1;
    if (g_vols[v].readonly) { log_reject_ro(v, "chmod", path); return -FS64_EROFS; }
    if (g_vols[v].kind != FS64_KIND_VIMTUFS2) return -1;
    Fs64VfsScope64 sc(g_vols[v].vfs_slot);
    if (!sc.ok) return -1;
    return vfs64_chmod64(path, mode);
}
int fs64_chown64(int vol, const char* path, uint32_t uid, uint32_t gid) {
    const int v = resolve_vol(vol);
    if (v < 0) return -1;
    if (g_vols[v].readonly) { log_reject_ro(v, "chown", path); return -FS64_EROFS; }
    if (g_vols[v].kind != FS64_KIND_VIMTUFS2) return -1;
    Fs64VfsScope64 sc(g_vols[v].vfs_slot);
    if (!sc.ok) return -1;
    return vfs64_chown64(path, uid, gid);
}
int fs64_read64(int vol, const char* path, void* buf, int max) {
    const int v = resolve_vol(vol);
    if (v < 0 || !buf || max <= 0) return -1;
    if (g_vols[v].kind == FS64_KIND_VIMTUFS2) {
        Fs64VfsScope64 sc(g_vols[v].vfs_slot);
        if (!sc.ok) return -1;
        return vfs64_read64(path, buf, max);
    }
    uint32_t got = 0;
    if (fat64_read64(g_vols[v].fat_slot, path, buf, (uint32_t)max, &got) != 0) return -1;
    return (int)got;
}
int fs64_read_range64(int vol, const char* path, uint32_t off, void* buf, uint32_t len, uint32_t* out_got) {
    const int v = resolve_vol(vol);
    if (v < 0 || !buf || !out_got) return -1;
    *out_got = 0;
    if (g_vols[v].kind == FS64_KIND_VIMTUFS2) {
        Fs64VfsScope64 sc(g_vols[v].vfs_slot);
        if (!sc.ok) return -1;
        // ★ 批次 M：VimtuFS2 有**按偏移分块读**（二级间接块也支持）——缓冲区直接用调用方的。
        return vfs64_read_at64(path, off, buf, len, out_got);
    }
    return fat64_read_range64(g_vols[v].fat_slot, path, off, buf, len, out_got);
}
// ★ 批次 M：分块写（按偏移，保留原有字节；off > 当前大小 = 空洞补零）。只读卷一律 -FS64_EROFS。
// ★ P4：写权限判定在 vfs64 内部（已有文件 w / 新建父目录 w+x）；越权 -> -EACCES。
int fs64_write_at64(int vol, const char* path, uint32_t off, const void* buf, uint32_t len) {
    const int v = resolve_vol(vol);
    if (v < 0) return -1;
    if (g_vols[v].readonly) { log_reject_ro(v, "write_at", path); return -FS64_EROFS; }
    if (g_vols[v].kind != FS64_KIND_VIMTUFS2) return -1;          // FAT 只读（上面已拒），这里不会到
    Fs64VfsScope64 sc(g_vols[v].vfs_slot);
    if (!sc.ok) return -1;
    return vfs64_write_at64(path, off, buf, len);
}
// 写操作：FAT 一律拒绝（只读卷），VimtuFS2 按当前身份调用（权限判定在 vfs64 内）
int fs64_write64(int vol, const char* path, const void* buf, int len) {
    const int v = resolve_vol(vol);
    if (v < 0) return -1;
    if (g_vols[v].readonly) { log_reject_ro(v, "write", path); return -FS64_EROFS; }
    if (g_vols[v].kind != FS64_KIND_VIMTUFS2) return -1;
    Fs64VfsScope64 sc(g_vols[v].vfs_slot);
    if (!sc.ok) return -1;
    return vfs64_write64(path, buf, len);
}
int fs64_create64(int vol, const char* path) {
    const int v = resolve_vol(vol);
    if (v < 0) return -1;
    if (g_vols[v].readonly) { log_reject_ro(v, "create", path); return -FS64_EROFS; }
    if (g_vols[v].kind != FS64_KIND_VIMTUFS2) return -1;
    Fs64VfsScope64 sc(g_vols[v].vfs_slot);
    if (!sc.ok) return -1;
    return vfs64_create64(path);
}
int fs64_mkdir64(int vol, const char* path) {
    const int v = resolve_vol(vol);
    if (v < 0) return -1;
    if (g_vols[v].readonly) { log_reject_ro(v, "mkdir", path); return -FS64_EROFS; }
    if (g_vols[v].kind != FS64_KIND_VIMTUFS2) return -1;
    Fs64VfsScope64 sc(g_vols[v].vfs_slot);
    if (!sc.ok) return -1;
    return vfs64_mkdir64(path);
}
int fs64_unlink64(int vol, const char* path) {
    const int v = resolve_vol(vol);
    if (v < 0) return -1;
    if (g_vols[v].readonly) { log_reject_ro(v, "unlink", path); return -FS64_EROFS; }
    if (g_vols[v].kind != FS64_KIND_VIMTUFS2) return -1;
    Fs64VfsScope64 sc(g_vols[v].vfs_slot);
    if (!sc.ok) return -1;
    return vfs64_unlink64(path);
}
int fs64_rmdir64(int vol, const char* path) {
    const int v = resolve_vol(vol);
    if (v < 0) return -1;
    if (g_vols[v].readonly) { log_reject_ro(v, "rmdir", path); return -FS64_EROFS; }
    if (g_vols[v].kind != FS64_KIND_VIMTUFS2) return -1;
    Fs64VfsScope64 sc(g_vols[v].vfs_slot);
    if (!sc.ok) return -1;
    return vfs64_rmdir64(path);
}
int fs64_rename64(int vol, const char* old_path, const char* new_name) {
    const int v = resolve_vol(vol);
    if (v < 0) return -1;
    if (g_vols[v].readonly) { log_reject_ro(v, "rename", old_path); return -FS64_EROFS; }
    if (g_vols[v].kind != FS64_KIND_VIMTUFS2) return -1;
    Fs64VfsScope64 sc(g_vols[v].vfs_slot);
    if (!sc.ok) return -1;
    return vfs64_rename64(old_path, new_name);
}
int fs64_free64(int vol, uint32_t* free_blocks, uint32_t* free_bytes, uint32_t* total_blocks) {
    const int v = resolve_vol(vol);
    if (v < 0) return -1;
    if (g_vols[v].kind == FS64_KIND_VIMTUFS2) {
        return vfs64_free_on64(g_vols[v].vfs_slot, free_blocks, free_bytes, total_blocks);   // 无路径：不涉权限
    }
    Fat64Info64 fi;
    if (fat64_vol_info64(g_vols[v].fat_slot, &fi) != 0) return -1;
    const uint64_t total_bytes = (uint64_t)fi.clusters * fi.cluster_bytes;
    if (total_blocks) *total_blocks = (uint32_t)(total_bytes / 512u);
    if (fi.free_clusters == 0xFFFFFFFFu) {
        if (free_blocks) *free_blocks = 0;
        if (free_bytes) *free_bytes = 0;
        return -1;                                            // FSInfo 无效：如实说"未知"
    }
    const uint64_t fb = (uint64_t)fi.free_clusters * fi.cluster_bytes;
    if (free_blocks) *free_blocks = (uint32_t)(fb / 512u);
    if (free_bytes) *free_bytes = (uint32_t)fb;
    return 0;
}
// ---- CRC32（IEEE；按字节位运算，和 zlib.crc32 同一多项式/初值/异或）----
static uint32_t fs64_crc32_update(uint32_t crc, const uint8_t* p, uint32_t n) {
    crc = ~crc;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}
uint32_t fs64_crc32_file64(int vol, const char* path, uint32_t* out_size, uint32_t max_bytes) {
    if (out_size) *out_size = 0;
    const int v = resolve_vol(vol);
    if (v < 0 || !path) return 0;
    Fs64Stat64 st;
    if (fs64_stat64(v, path, &st) != 0 || st.type != VFS64_TYPE_FILE) return 0;
    if (st.size > max_bytes) return 0;                        // 超上限：如实拒绝（不假装算完）
    if (out_size) *out_size = st.size;
    static uint8_t chunk[32 * 1024];
    uint32_t crc = 0, off = 0;
    while (off < st.size) {
        uint32_t want = st.size - off;
        if (want > sizeof(chunk)) want = sizeof(chunk);
        uint32_t got = 0;
        if (fs64_read_range64(v, path, off, chunk, want, &got) != 0 || got == 0) return 0;
        crc = fs64_crc32_update(crc, chunk, got);
        off += got;
    }
    return crc;
}

// ==================== 自检 ====================
int fs64_selftest64() {
    int fails = 0;
    (void)fs64_init64();
    // bit0：卷表自洽 + 当前卷 stat("/")
    for (int v = 0; v < FS64_VOL_MAX; v++) {
        if (g_vols[v].kind == FS64_KIND_NONE) continue;
        if (g_vols[v].fs[0] == 0 || g_vols[v].total_known == 0) fails |= 1;
        if (g_vols[v].kind == FS64_KIND_FAT32 && g_vols[v].readonly != 1) fails |= 1;
        if (g_vols[v].kind == FS64_KIND_VIMTUFS2 && g_vols[v].readonly != 0) fails |= 1;
    }
    const int cur = fs64_current_vol64();
    if (cur >= 0) {
        Fs64Stat64 st;
        if (fs64_stat64(cur, "/", &st) != 0) fails |= 1;
    }
    // bit1：FAT 卷只读语义（只看 FAT 卷，绝不碰 VimtuFS2 的内容）
    for (int v = FS64_VOL_FAT_BASE; v < FS64_VOL_MAX; v++) {
        if (g_vols[v].kind != FS64_KIND_FAT32) continue;
        if (fs64_write64(v, "/__fs64test.tmp", "x", 1) != -FS64_EROFS) fails |= 2;
        if (fs64_create64(v, "/__fs64test.tmp") != -FS64_EROFS) fails |= 2;
        if (fs64_mkdir64(v, "/__fs64test.dir") != -FS64_EROFS) fails |= 2;
        if (fs64_unlink64(v, "/__fs64test.tmp") != -FS64_EROFS) fails |= 2;
        if (fs64_rmdir64(v, "/__fs64test.dir") != -FS64_EROFS) fails |= 2;
        if (fs64_rename64(v, "/__fs64test.tmp", "x.tmp") != -FS64_EROFS) fails |= 2;
        Fs64Stat64 st;
        if (fs64_stat64(v, "/__fs64test.tmp", &st) == 0) fails |= 2;   // 拒绝必须没有副作用（文件不存在）
    }
    // bit2：无效 vol / 越界请求
    if (fs64_vol_used64(FS64_VOL_MAX) != 0) fails |= 4;
    if (fs64_vol_used64(-1) != 0) fails |= 4;
    if (fs64_activate64(FS64_VOL_MAX) == 0) fails |= 4;
    Fs64Stat64 st;
    if (cur >= 0 && fs64_stat64(cur, "/__no_such_fs64_path__", &st) == 0) fails |= 4;

    dbg64_line_begin64();
    dbg64_str("[FS64] selftest ");
    dbg64_str(fails == 0 ? "PASS" : "FAIL");
    dbg64_str(" mask=");
    dbg64_dec((uint64_t)fails);
    dbg64_str(" vols=");
    dbg64_dec((uint64_t)fs64_vol_count64());
    dbg64_str(" cur=");
    if (cur < 0) dbg64_str("-");
    else {
        dbg64_dec((uint64_t)cur);
        dbg64_str(" kind=");
        dbg64_str(g_vols[cur].kind == FS64_KIND_FAT32 ? "FAT32" : "VimtuFS2");
    }
    dbg64_nl();
    dbg64_line_end64();
    return fails;
}
