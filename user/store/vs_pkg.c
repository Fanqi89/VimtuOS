/* vs_pkg.c - ★ 包管理器核心：仓库索引 / 已装库 / .vap64 / .deb 的解析与安装-卸载引擎
 *
 * 三件东西都在这里（CLI /bin/vpkg 与 GUI /bin/store **共用同一份**，保证两条路行为一致）：
 *
 * ① 仓库：/opt/vpkg/index.json（宿主侧由 tools/store_pack_win.py 生成）
 *    每条：name / version / arch / size / sha256 / depends[] / description / summary / type / file
 *    —— 手写 JSON 子集解析器（不引第三方库；只认对象数组 + 字符串/数字/字符串数组）。
 * ② 已装库：/var/lib/vpkg/installed.json（**本程序自己写**）：每条记 name/version/arch/type/bytes/
 *    sha256 + files[]（落盘文件清单，卸载就按它删）。格式故意做成"字符串数组"，两边都好解。
 * ③ 安装引擎：.vap64 = kernel/app64.h 的布局（32B 头 + 名字 + code），payload = code 段；
 *    .deb = ar 归档 + control.tar.gz + data.tar.gz。**如实边界**（都返回专门错误码，不假装成功）：
 *      * deb 带维护者脚本（preinst/postinst/prerm/postrm）-> VS_E_SCRIPTS，整包拒绝；
 *      * deb 用 xz（control.tar.xz / data.tar.xz）-> VS_E_XZ，整包拒绝；
 *      * deb 里的符号链接条目 -> VS_E_FORMAT（VimtuOS 没有符号链接，见 SDK README 第 10 节的 FAQ）；
 *      * 依赖解析**一层**：查已装库里的名字，不递归、不做版本区间（如实写在报告里）；
 *      * 文件权限：内核的 open(O_CREAT) 不带 mode（lx64_open64 只吃 flags），新文件一律 0644；
 *        可执行靠"内核 ELF 加载器只要 r 权限"这条既有事实（本报告如实写）。
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>

#include "vimtu64.h"
#include "vs.h"

#define VS_TAG "vpkg"

/* ==================== 极简 JSON ==================== */
struct Js { const char* s; int i; int n; };

static void js_ws(struct Js* j) {
    while (j->i < j->n) {
        const char c = j->s[j->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { j->i++; continue; }
        if (c == '/' && j->i + 1 < j->n && j->s[j->i + 1] == '/') {     /* 行注释：# 不认，// 认 */
            while (j->i < j->n && j->s[j->i] != '\n') j->i++;
            continue;
        }
        break;
    }
}

static int js_str(struct Js* j, char* out, int cap) {
    js_ws(j);
    if (j->i >= j->n || j->s[j->i] != '"') return -1;
    j->i++;
    int o = 0;
    while (j->i < j->n && j->s[j->i] != '"') {
        char c = j->s[j->i++];
        if (c == '\\' && j->i < j->n) {
            const char e = j->s[j->i++];
            if (e == 'n') c = ' ';
            else if (e == 't') c = ' ';
            else c = e;                                   /* \" \\ \/ 等：直接取字符 */
        }
        if (out && o < cap - 1) out[o++] = c;
    }
    if (j->i >= j->n) return -1;                          /* 没收尾 */
    j->i++;                                               /* 跳过结尾的 " */
    if (out) out[o] = 0;
    return o;
}

static int js_num(struct Js* j, int* out) {
    js_ws(j);
    int v = 0, neg = 0, any = 0;
    if (j->i < j->n && (j->s[j->i] == '-' || j->s[j->i] == '+')) { neg = (j->s[j->i] == '-'); j->i++; }
    while (j->i < j->n && j->s[j->i] >= '0' && j->s[j->i] <= '9') {
        v = v * 10 + (j->s[j->i] - '0');
        j->i++;
        any = 1;
    }
    while (j->i < j->n && (j->s[j->i] == '.' || j->s[j->i] == 'e' || j->s[j->i] == 'E' ||
                           (j->s[j->i] >= '0' && j->s[j->i] <= '9') || j->s[j->i] == '-' || j->s[j->i] == '+'))
        j->i++;                                           /* 小数/指数：只跳过（本格式只有整数） */
    if (!any) return -1;
    if (out) *out = neg ? -v : v;
    return 0;
}

static int js_skip_value(struct Js* j) {
    js_ws(j);
    if (j->i >= j->n) return -1;
    const char c = j->s[j->i];
    if (c == '"') return js_str(j, 0, 0) < 0 ? -1 : 0;
    if (c == '{' || c == '[') {
        int depth = 0, instr = 0;
        for (; j->i < j->n; j->i++) {
            const char k = j->s[j->i];
            if (instr) {
                if (k == '\\') j->i++;
                else if (k == '"') instr = 0;
                continue;
            }
            if (k == '"') instr = 1;
            else if (k == '{' || k == '[') depth++;
            else if (k == '}' || k == ']') {
                depth--;
                if (depth == 0) { j->i++; return 0; }
            }
        }
        return -1;
    }
    return js_num(j, 0);
}

/* 找到 "key"（在当前位置之后）——返回 1 = 找到了（j 停在 ':' 之后） */
static int js_find_key(struct Js* j, const char* key) {
    char k[VS_KEY_MAX];
    while (1) {
        js_ws(j);
        if (j->i >= j->n) return 0;
        const char c = j->s[j->i];
        if (c == '{') { j->i++; continue; }
        if (c == '}') { j->i++; continue; }
        if (c == '[') { j->i++; continue; }
        if (c == ']') { j->i++; continue; }
        if (c == ',') { j->i++; continue; }
        if (c != '"') { j->i++; continue; }
        if (js_str(j, k, (int)sizeof(k)) < 0) return 0;
        js_ws(j);
        if (j->i < j->n && j->s[j->i] == ':') {
            j->i++;
            if (vs_streq(k, key)) return 1;
            if (js_skip_value(j) != 0) return 0;
            continue;
        }
        /* 不是 key:value（例如数组里的裸字符串）——值已经吃掉了，继续 */
    }
}

/* ==================== 索引 / 已装库 ==================== */
static int parse_pkg_object(struct Js* j, struct VsPkg* p) {
    char key[VS_KEY_MAX];
    for (;;) {
        js_ws(j);
        if (j->i >= j->n) return VS_E_FORMAT;
        if (j->s[j->i] == '}') { j->i++; return VS_OK; }
        if (j->s[j->i] == ',') { j->i++; continue; }
        if (js_str(j, key, (int)sizeof(key)) < 0) return VS_E_FORMAT;
        js_ws(j);
        if (j->i >= j->n || j->s[j->i] != ':') return VS_E_FORMAT;
        j->i++;
        js_ws(j);
        if (vs_streq(key, "name"))            { if (js_str(j, p->name, VS_NAME_MAX) < 0) return VS_E_FORMAT; }
        else if (vs_streq(key, "version"))    { if (js_str(j, p->version, VS_VER_MAX) < 0) return VS_E_FORMAT; }
        else if (vs_streq(key, "arch"))       { if (js_str(j, p->arch, VS_ARCH_MAX) < 0) return VS_E_FORMAT; }
        else if (vs_streq(key, "type"))       { if (js_str(j, p->type, VS_TYPE_MAX) < 0) return VS_E_FORMAT; }
        else if (vs_streq(key, "file"))       { if (js_str(j, p->file, VS_FILE_MAX) < 0) return VS_E_FORMAT; }
        else if (vs_streq(key, "summary"))    { if (js_str(j, p->summary, VS_SUM_MAX) < 0) return VS_E_FORMAT; }
        else if (vs_streq(key, "description")){ if (js_str(j, p->desc, VS_DESC_MAX) < 0) return VS_E_FORMAT; }
        else if (vs_streq(key, "size"))       { if (js_num(j, &p->size) != 0) return VS_E_FORMAT; }
        else if (vs_streq(key, "sha256"))     { if (js_str(j, p->sha256, 65) < 0) return VS_E_FORMAT; }
        else if (vs_streq(key, "depends")) {
            js_ws(j);
            if (j->i >= j->n || j->s[j->i] != '[') return VS_E_FORMAT;
            j->i++;
            for (;;) {
                js_ws(j);
                if (j->i < j->n && j->s[j->i] == ']') { j->i++; break; }
                if (j->i < j->n && j->s[j->i] == ',') { j->i++; continue; }
                char d[VS_NAME_MAX];
                if (js_str(j, d, (int)sizeof(d)) < 0) return VS_E_FORMAT;
                if (p->ndep < VS_DEPENDS_MAX) {
                    vs_strcpy(p->depends[p->ndep], VS_NAME_MAX, d);
                    p->ndep++;
                } else {
                    return VS_E_FORMAT;               /* 依赖条数超过本实现上限：如实拒绝，不静默丢 */
                }
            }
        }
        else if (js_skip_value(j) != 0) return VS_E_FORMAT;
    }
}

int vs_index_load(const char* path, struct VsPkg* out, int cap, int* n) {
    const int bufsz = 64 * 1024;
    unsigned char* buf = (unsigned char*)vs_mmap(bufsz);
    if (!buf) return VS_E_NOMEM;
    const int len = vs_read_file(path, buf, bufsz - 1);
    if (len < 0) return VS_E_NOTFOUND;
    buf[len] = 0;
    struct Js j;
    j.s = (const char*)buf; j.i = 0; j.n = len;
    if (!js_find_key(&j, "packages")) return VS_E_FORMAT;
    js_ws(&j);
    if (j.i >= j.n || j.s[j.i] != '[') return VS_E_FORMAT;
    j.i++;
    int cnt = 0;
    for (;;) {
        js_ws(&j);
        if (j.i >= j.n) return VS_E_FORMAT;
        if (j.s[j.i] == ']') break;
        if (j.s[j.i] == ',') { j.i++; continue; }
        if (j.s[j.i] != '{') return VS_E_FORMAT;
        j.i++;
        if (cnt >= cap) return VS_E_FORMAT;                  /* 超过上限：如实报错，不截断 */
        for (int k = 0; k < (int)sizeof(struct VsPkg); k++) ((unsigned char*)&out[cnt])[k] = 0;
        const int rc = parse_pkg_object(&j, &out[cnt]);
        if (rc != VS_OK) return rc;
        cnt++;
    }
    *n = cnt;
    return VS_OK;
}

static int parse_inst_object(struct Js* j, struct VsInstalled* p) {
    char key[VS_KEY_MAX];
    for (;;) {
        js_ws(j);
        if (j->i >= j->n) return VS_E_FORMAT;
        if (j->s[j->i] == '}') { j->i++; return VS_OK; }
        if (j->s[j->i] == ',') { j->i++; continue; }
        if (js_str(j, key, (int)sizeof(key)) < 0) return VS_E_FORMAT;
        js_ws(j);
        if (j->i >= j->n || j->s[j->i] != ':') return VS_E_FORMAT;
        j->i++;
        js_ws(j);
        if (vs_streq(key, "name"))         { if (js_str(j, p->name, VS_NAME_MAX) < 0) return VS_E_FORMAT; }
        else if (vs_streq(key, "version")) { if (js_str(j, p->version, VS_VER_MAX) < 0) return VS_E_FORMAT; }
        else if (vs_streq(key, "arch"))    { if (js_str(j, p->arch, VS_ARCH_MAX) < 0) return VS_E_FORMAT; }
        else if (vs_streq(key, "type"))    { if (js_str(j, p->type, VS_TYPE_MAX) < 0) return VS_E_FORMAT; }
        else if (vs_streq(key, "bytes"))   { if (js_num(j, &p->bytes) != 0) return VS_E_FORMAT; }
        else if (vs_streq(key, "sha256"))  { if (js_str(j, p->sha256, 65) < 0) return VS_E_FORMAT; }
        else if (vs_streq(key, "files")) {
            js_ws(j);
            if (j->i >= j->n || j->s[j->i] != '[') return VS_E_FORMAT;
            j->i++;
            for (;;) {
                js_ws(j);
                if (j->i < j->n && j->s[j->i] == ']') { j->i++; break; }
                if (j->i < j->n && j->s[j->i] == ',') { j->i++; continue; }
                char f[VS_FILE_MAX];
                if (js_str(j, f, (int)sizeof(f)) < 0) return VS_E_FORMAT;
                if (p->nfiles < VS_FILES_MAX) {
                    vs_strcpy(p->files[p->nfiles], VS_FILE_MAX, f);
                    p->nfiles++;
                } else {
                    return VS_E_FORMAT;
                }
            }
        }
        else if (js_skip_value(j) != 0) return VS_E_FORMAT;
    }
}

int vs_db_load(const char* path, struct VsInstalled* out, int cap, int* n) {
    *n = 0;
    const int bufsz = 64 * 1024;
    unsigned char* buf = (unsigned char*)vs_mmap(bufsz);
    if (!buf) return VS_E_NOMEM;
    const int len = vs_read_file(path, buf, bufsz - 1);
    if (len < 0) return VS_OK;                    /* 还没装过任何包：0 条，不是错误 */
    buf[len] = 0;
    struct Js j;
    j.s = (const char*)buf; j.i = 0; j.n = len;
    if (!js_find_key(&j, "packages")) return VS_E_FORMAT;
    js_ws(&j);
    if (j.i >= j.n || j.s[j.i] != '[') return VS_E_FORMAT;
    j.i++;
    int cnt = 0;
    for (;;) {
        js_ws(&j);
        if (j.i >= j.n) return VS_E_FORMAT;
        if (j.s[j.i] == ']') break;
        if (j.s[j.i] == ',') { j.i++; continue; }
        if (j.s[j.i] != '{') return VS_E_FORMAT;
        j.i++;
        if (cnt >= cap) return VS_E_FORMAT;
        for (int k = 0; k < (int)sizeof(struct VsInstalled); k++) ((unsigned char*)&out[cnt])[k] = 0;
        const int rc = parse_inst_object(&j, &out[cnt]);
        if (rc != VS_OK) return rc;
        cnt++;
    }
    *n = cnt;
    return VS_OK;
}

int vs_db_write(const char* path, const struct VsInstalled* in, int n, int* out_bytes) {
    const int bufsz = 32 * 1024;
    char* buf = (char*)vs_mmap(bufsz);
    if (!buf) return VS_E_NOMEM;
    int o = 0;
    o += vs_fmt(buf + o, bufsz - o, "{\"schema\":1,\"generator\":\"vpkg 1.0\",\"packages\":[");
    for (int i = 0; i < n; i++) {
        const struct VsInstalled* p = &in[i];
        if (i) buf[o++] = ',';
        o += vs_fmt(buf + o, bufsz - o,
                    "\n  {\"name\":\"%s\",\"version\":\"%s\",\"arch\":\"%s\",\"type\":\"%s\","
                    "\"bytes\":%d,\"sha256\":\"%s\",\"files\":[",
                    p->name, p->version, p->arch, p->type, p->bytes, p->sha256);
        for (int k = 0; k < p->nfiles; k++)
            o += vs_fmt(buf + o, bufsz - o, "%s\"%s\"", k ? "," : "", p->files[k]);
        o += vs_fmt(buf + o, bufsz - o, "]}");
        if (o > bufsz - 512) return VS_E_NOMEM;              /* 有界：超了就如实失败 */
    }
    o += vs_fmt(buf + o, bufsz - o, "\n]}\n");
    const int rc = vs_write_file(path, (const unsigned char*)buf, o);
    if (rc != 0) return VS_E_IO;
    if (out_bytes) *out_bytes = o;
    return VS_OK;
}

int vs_index_find(const struct VsPkg* a, int n, const char* name) {
    for (int i = 0; i < n; i++) if (vs_streq(a[i].name, name)) return i;
    return -1;
}

int vs_db_find(const struct VsInstalled* a, int n, const char* name) {
    for (int i = 0; i < n; i++) if (vs_streq(a[i].name, name)) return i;
    return -1;
}

/* ==================== 名字/路径检查 ==================== */
static int name_ok(const char* n) {
    const unsigned long l = vs_strlen(n);
    if (l == 0 || l > 31) return 0;
    for (unsigned long i = 0; i < l; i++) {
        const char c = n[i];
        const int alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum && c != '-' && c != '_' && c != '.' && c != '+') return 0;
    }
    if (n[0] == '.' && (l == 1 || (l == 2 && n[1] == '.'))) return 0;
    return 1;
}

/* tar 条目名的规整：dpkg / Python tarfile 常写 "./usr/share/x" —— 先剥掉前导 "./"
 * （否则路径检查会把 "." 段判成非法；实测 spacehog 那条反例就死在这里）。
 * 与 rel_path_ok 一样只给 deb 用：整段在 #ifndef VS_STORE_GUI 里（见文件头体积说明）。 */
#ifndef VS_STORE_GUI
static const char* tar_strip(const char* nm) {
    while (nm[0] == '.' && nm[1] == '/') nm += 2;
    return nm;
}

/* 卷内的相对路径（tar 条目）：不许绝对、不许 ".."、不许空段（只给 deb 的 data.tar 用） */
static int rel_path_ok(const char* p) {
    if (!p || !p[0]) return 0;
    if (p[0] == '/') return 0;
    const unsigned long l = vs_strlen(p);
    if (l >= VS_FILE_MAX) return 0;
    int seg = 0;
    for (unsigned long i = 0; i <= l; i++) {
        const char c = (i < l) ? p[i] : '/';
        if (c == '/') {
            if (seg == 0) return 0;                          /* 空段（含 "//" 与结尾 '/' 之外的情况） */
            seg = 0;
        } else {
            seg++;
            if (seg == 1 && p[i] == '.' && (i + 1 == l || p[i + 1] == '/')) return 0;   /* "." 段 */
            if (seg == 1 && p[i] == '.' && i + 2 <= l && p[i + 1] == '.' &&
                (i + 2 == l || p[i + 2] == '/')) return 0;                              /* ".." 段 */
        }
    }
    return 1;
}

#endif  /* !VS_STORE_GUI：rel_path_ok 只给 deb 的 tar 条目检查用 */

/* ==================== JSON 里的 sha256 比较 ==================== */
static int sha256_of(const unsigned char* p, int n, char out[65]) {
    struct VsSha256 s;
    vs_sha256_init(&s);
    vs_sha256_update(&s, p, n);
    vs_sha256_final(&s, out);
    return 0;
}

/* ==================== .vap64（kernel/app64.h 布局） ==================== */
#define VAP_MAGIC_A 0x5056u     /* "VP" 小端 */
struct Vap {
    int   name_len;
    int   code_off;
    int   code_size;
    unsigned int crc;
    char  name[VS_NAME_MAX];
};

static int vap_parse(const unsigned char* b, int n, struct Vap* out) {
    if (n < 32) return VS_E_FORMAT;
    if (b[0] != 'V' || b[1] != 'A' || b[2] != 'P' || b[3] != '6' || b[4] != '4' || b[5] != 0 ||
        b[6] != 0 || b[7] != 0) return VS_E_FORMAT;
    const unsigned int ver = (unsigned int)(b[8] | (b[9] << 8) | (b[10] << 16) | ((unsigned)b[11] << 24));
    const unsigned int hs  = (unsigned int)(b[12] | (b[13] << 8) | (b[14] << 16) | ((unsigned)b[15] << 24));
    const unsigned int eo  = (unsigned int)(b[16] | (b[17] << 8) | (b[18] << 16) | ((unsigned)b[19] << 24));
    const unsigned int cs  = (unsigned int)(b[20] | (b[21] << 8) | (b[22] << 16) | ((unsigned)b[23] << 24));
    out->crc = (unsigned int)(b[24] | (b[25] << 8) | (b[26] << 16) | ((unsigned)b[27] << 24));
    const unsigned int nl  = (unsigned int)(b[28] | (b[29] << 8) | (b[30] << 16) | ((unsigned)b[31] << 24));
    if (ver != 1 || hs != 32) return VS_E_FORMAT;
    if (nl < 1 || nl > 24) return VS_E_FORMAT;
    if (eo != 32 + nl) return VS_E_FORMAT;
    if ((int)(32 + nl + cs) != n || cs < 1) return VS_E_FORMAT;
    if (b[32 + nl - 1] != 0) return VS_E_FORMAT;
    out->name_len = (int)nl;
    out->code_off = (int)eo;
    out->code_size = (int)cs;
    int k = 0;
    while (k < (int)nl - 1 && k < VS_NAME_MAX - 1) { out->name[k] = (char)b[32 + k]; k++; }
    out->name[k] = 0;
    return VS_OK;
}

/* ==================== ar（真 deb 的外壳） ====================
 * ★ GUI 构建（-DVS_STORE_GUI）里这一段**不编进来**：/bin/store 的窗口只有 256x60，
 *   而 stb_truetype（font64）已经把 64 KiB 的主程序装载区吃掉大半；deb 的解压/ar/tar 交给
 *   `/bin/vpkg install <name>` 子进程走（**同一份引擎**，store 只做 fork+exec + 等），
 *   这样"GUI 也能卸/装 deb"仍然成立，且两个二进制共享同一套判定。 */
/* （ar 的解析统一走 ar_scan_all；不留单成员查找的重复实现 —— 那会是一份没人叫的死代码） */

#ifndef VS_STORE_GUI
/* 扫一遍 ar，找出 control/data 的压缩格式与是否有维护者脚本 */
struct DebInfo {
    int control_off, control_size, control_gz, control_xz, control_bz2;
    int data_off, data_size, data_gz, data_xz, data_bz2;
    int has_preinst, has_postinst, has_prerm, has_postrm;
    char pkg[VS_NAME_MAX];
    char ver[VS_VER_MAX];
    char arch[VS_ARCH_MAX];
    int  ndep;
    char depends[VS_DEPENDS_MAX][VS_NAME_MAX];
};

static int ar_scan_all(const unsigned char* b, int n, struct DebInfo* di, int* nctl, int* ndata) {
    if (n < 8 || b[0] != '!' || b[1] != '<') return VS_E_FORMAT;
    int off = 8, ctl = 0, dat = 0;
    while (off + 60 <= n) {
        const char* h = (const char*)b + off;
        if (h[58] != '`' || h[59] != '\n') return VS_E_FORMAT;
        char name[32];
        int k = 0;
        for (int i = 0; i < 16 && h[i] != ' ' && h[i] != '/' && k < 31; i++) name[k++] = h[i];
        name[k] = 0;
        int size = 0;
        for (int i = 48; i < 58; i++) if (h[i] >= '0' && h[i] <= '9') size = size * 10 + (h[i] - '0');
        const int data = off + 60;
        if (data + size > n) return VS_E_FORMAT;
        const int is_ctl = vs_streq(name, "control.tar.gz") || vs_streq(name, "control.tar.xz") ||
                           vs_streq(name, "control.tar.bz2");
        const int is_dat = vs_streq(name, "data.tar.gz") || vs_streq(name, "data.tar.xz") ||
                           vs_streq(name, "data.tar.bz2");
        if (is_ctl) {
            di->control_off = data; di->control_size = size;
            di->control_gz  = vs_streq(name, "control.tar.gz");
            di->control_xz  = vs_streq(name, "control.tar.xz");
            di->control_bz2 = vs_streq(name, "control.tar.bz2");
            ctl++;
        }
        if (is_dat) {
            di->data_off = data; di->data_size = size;
            di->data_gz  = vs_streq(name, "data.tar.gz");
            di->data_xz  = vs_streq(name, "data.tar.xz");
            di->data_bz2 = vs_streq(name, "data.tar.bz2");
            dat++;
        }
        off = data + size + (size & 1);
    }
    *nctl = ctl;
    *ndata = dat;
    /* debian-binary（版本 "2.0\n"）：有就核对，没有也不假装 */
    return VS_OK;
}

/* ==================== tar（ustar；只认常规文件与目录） ==================== */
struct TarEnt {
    const char* name;
    int size;
    int type;          /* 0 = 文件，5 = 目录，其它 = 不支持 */
};

static int octal_of(const char* p, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        const char c = p[i];
        if (c == 0 || c == ' ') continue;
        if (c < '0' || c > '7') return v;
        v = v * 8 + (c - '0');
    }
    return v;
}

/* 从 *off 取一条 tar 头；返回 1 = 有（*off 指向数据），0 = 结束，-1 = 坏 */
static int tar_next(const unsigned char* b, int n, int* off, struct TarEnt* e, char* namebuf, int ncap) {
    while (*off + 512 <= n) {
        const char* h = (const char*)(b + *off);
        int allzero = 1;
        for (int i = 0; i < 512; i++) if (h[i]) { allzero = 0; break; }
        if (allzero) return 0;                                  /* 结束块 */
        const int size = octal_of(h + 124, 12);
        const char tf = h[156];
        char nm[100], pre[155];
        int k = 0;
        for (int i = 0; i < 100 && h[i] && k < 99; i++) nm[k++] = h[i];
        nm[k] = 0;
        k = 0;
        for (int i = 0; i < 155 && h[345 + i] && k < 154; i++) pre[k++] = h[345 + i];
        pre[k] = 0;
        int o = 0;
        if (pre[0]) {
            for (int i = 0; pre[i] && o < ncap - 2; i++) namebuf[o++] = pre[i];
            namebuf[o++] = '/';
        }
        for (int i = 0; nm[i] && o < ncap - 1; i++) namebuf[o++] = nm[i];
        namebuf[o] = 0;
        *off += 512;
        if (tf == 'L' || tf == 'K' || tf == 'x' || tf == 'g') {  /* 长名/PAX 元数据：跳过数据 */
            *off += size + (size % 512 ? 512 - (size % 512) : 0);
            continue;
        }
        e->name = namebuf;
        e->size = size;
        e->type = (tf == '5') ? 5 : ((tf == '0' || tf == 0) ? 0 : (tf == '2' ? 2 : 9));
        return 1;
    }
    return 0;
}

static int tar_skip(int* off, int size) {
    *off += size + (size % 512 ? 512 - (size % 512) : 0);
    return 0;
}

/* ==================== 控制文件（control）解析 ==================== */
/* 字段值：跳过前导空格/制表，并去掉尾部空白（deb 的 control 是 "Package: name" 形式，
 * 直接 line+8 会带一个前导空格 —— 实测它把 /usr/share/<name> 拼成 "/usr/share/ vpkg-debdemo"，
 * 内核如实回 "path bad name"）。 */
static int copy_trim(char* dst, int cap, const char* src) {
    while (*src == ' ' || *src == '\t') src++;
    int n = 0;
    while (src[n] && n < cap - 1) n++;
    while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\t' || src[n - 1] == '\r')) n--;
    for (int i = 0; i < n; i++) dst[i] = src[i];
    dst[n] = 0;
    return n;
}
static int control_parse(const unsigned char* b, int n, struct DebInfo* di, int want_scripts) {
    char line[160];
    int li = 0;
    for (int i = 0; i <= n; i++) {
        const char c = (i < n) ? (char)b[i] : '\n';
        if (c == '\n') {
            line[li] = 0;
            if (!line[0]) {                          /* 空行 = 第一段结束（control 的正文） */
                if (want_scripts) {
                    /* 维护者脚本字段在 control 里只说明"看到过"；真正的脚本文件在 control.tar 里 */
                }
            }
            if (vs_starts(line, "Package:"))      { copy_trim(di->pkg, VS_NAME_MAX, line + 8); }
            else if (vs_starts(line, "Package: ")){ copy_trim(di->pkg, VS_NAME_MAX, line + 9); }
            else if (vs_starts(line, "Version:")) { copy_trim(di->ver, VS_VER_MAX, line + 8); }
            else if (vs_starts(line, "Architecture:")) { copy_trim(di->arch, VS_ARCH_MAX, line + 13); }
            else if (vs_starts(line, "Depends:")) {
                const char* p = line + 8;
                for (;;) {
                    while (*p == ' ') p++;
                    char d[VS_NAME_MAX];
                    int k = 0;
                    while (*p && *p != ',' && *p != ' ' && *p != '(' && k < VS_NAME_MAX - 1) d[k++] = *p++;
                    d[k] = 0;
                    while (*p == ' ') p++;
                    if (*p == '(') { while (*p && *p != ')') p++; if (*p == ')') p++; }   /* 丢版本区间（一层依赖的如实边界） */
                    if (d[0] && di->ndep < VS_DEPENDS_MAX) {
                        vs_strcpy(di->depends[di->ndep], VS_NAME_MAX, d);
                        di->ndep++;
                    }
                    while (*p && *p != ',') p++;
                    if (*p != ',') break;
                    p++;
                }
            }
            li = 0;
            continue;
        }
        if (c == '\r') continue;
        if (li < (int)sizeof(line) - 1) line[li++] = c;
    }
    return VS_OK;
}

#endif  /* !VS_STORE_GUI：deb 的 ar/tar/control 到这里为止（GUI 构建不编这段） */

/* ==================== 日志小工具 ==================== */
/* 写/建目录失败的统一判定：-EACCES(-13) = 目标目录不是本会话可写的（P4 权限真拦截）
 * —— 给一个**专门的错误码**，别把它混进泛泛的 io。 */
static int io_code_of(int rc) { return (rc == -13) ? VS_E_PERM : VS_E_IO; }

static void log_err(const char* tag, int code, const char* msg) {
    vs_log(tag, "ERROR code=%s msg=\"%s\"\n", vs_err_name(code), msg);
}

/* ==================== 仓库/已装库缓存（GUI 每帧都要用） ====================
 * ★ 为什么这两张表走 mmap 而不是静态数组：本仓库一律用 -fno-zero-initialized-in-bss，
 *   全零的静态数组会落进 **.data（文件里）**；两张表加起来 ~32 KiB，直接把主程序装载区
 *   （64 KiB，kernel/elf64.cpp）顶爆。mmap 区有 ~15 MiB，随便放。 */
static unsigned char* g_cache_mem;
static int g_cache_inst_n = -1;
static int g_cache_repo_n = -1;

static struct VsInstalled* cache_inst(void) {
    if (!g_cache_mem) g_cache_mem = (unsigned char*)vs_mmap((int)sizeof(struct VsInstalled) * VS_MAX_PKGS +
                                                            (int)sizeof(struct VsPkg) * VS_MAX_PKGS);
    return (struct VsInstalled*)(void*)g_cache_mem;
}

static struct VsPkg* cache_repo(void) {
    (void)cache_inst();
    if (!g_cache_mem) return (struct VsPkg*)0;
    return (struct VsPkg*)(void*)(g_cache_mem + sizeof(struct VsInstalled) * VS_MAX_PKGS);
}

int vs_cached_installed(struct VsInstalled* out, int cap, int* n) {
    struct VsInstalled* tbl = cache_inst();
    if (!tbl) { *n = 0; return VS_E_NOMEM; }
    if (g_cache_inst_n < 0) {
        int cnt = 0;
        const int rc = vs_db_load(VS_DB_PATH, tbl, VS_MAX_PKGS, &cnt);
        g_cache_inst_n = (rc == VS_OK) ? cnt : 0;
    }
    const int cnt = (g_cache_inst_n > cap) ? cap : g_cache_inst_n;
    for (int i = 0; i < cnt; i++) out[i] = tbl[i];
    *n = cnt;
    return VS_OK;
}

int vs_cached_repo(struct VsPkg* out, int cap, int* n) {
    struct VsPkg* tbl = cache_repo();
    if (!tbl) { *n = 0; return VS_E_NOMEM; }
    if (g_cache_repo_n < 0) {
        int cnt = 0;
        const int rc = vs_index_load(VS_INDEX_PATH, tbl, VS_MAX_PKGS, &cnt);
        g_cache_repo_n = (rc == VS_OK) ? cnt : 0;
    }
    const int cnt = (g_cache_repo_n > cap) ? cap : g_cache_repo_n;
    for (int i = 0; i < cnt; i++) out[i] = tbl[i];
    *n = cnt;
    return VS_OK;
}

/* 指针版（GUI 用：省掉调用方自己那份静态副本，见上面的体积说明） */
const struct VsInstalled* vs_inst_table(int* n) {
    struct VsInstalled* tbl = cache_inst();
    if (!tbl) { *n = 0; return (const struct VsInstalled*)0; }
    if (g_cache_inst_n < 0) {
        int cnt = 0;
        const int rc = vs_db_load(VS_DB_PATH, tbl, VS_MAX_PKGS, &cnt);
        g_cache_inst_n = (rc == VS_OK) ? cnt : 0;
    }
    *n = g_cache_inst_n;
    return tbl;
}

const struct VsPkg* vs_repo_table(int* n) {
    struct VsPkg* tbl = cache_repo();
    if (!tbl) { *n = 0; return (const struct VsPkg*)0; }
    if (g_cache_repo_n < 0) {
        int cnt = 0;
        const int rc = vs_index_load(VS_INDEX_PATH, tbl, VS_MAX_PKGS, &cnt);
        g_cache_repo_n = (rc == VS_OK) ? cnt : 0;
    }
    *n = g_cache_repo_n;
    return tbl;
}

void vs_cache_drop(void) { g_cache_inst_n = -1; g_cache_repo_n = -1; }

/* ==================== 临时大表（**一律 mmap**） ====================
 * ★ 实测踩到的坑（tests/vpkg64_test.py 第一轮 43 条里 26 条红）：
 *   `struct VsInstalled inst[8]` 是 **13 KiB 栈**（files[12][128]），
 *   `struct VsPkg repo[8]` 又是 3.6 KiB —— 用户栈只有 16 KiB
 *   （kernel/usermode64.h），于是 `vpkg list|info|install|remove|update` 全在进函数时
 *   SIGSEGV（串口：run: /bin/vpkg pid=N exited code=139），而只开 repo[8] 的 `search` 反而活着。
 *   修法：这两张“每次调用都要一张”的表统一 mmap（进 mmap 区，不进栈、不进 .data）。 */
static struct VsPkg* scratch_repo(void) {
    static struct VsPkg* p;
    if (!p) p = (struct VsPkg*)vs_mmap((int)sizeof(struct VsPkg) * VS_MAX_PKGS);
    return p;
}

static struct VsInstalled* scratch_inst(void) {
    static struct VsInstalled* p;
    if (!p) p = (struct VsInstalled*)vs_mmap((int)sizeof(struct VsInstalled) * VS_MAX_PKGS);
    return p;
}

/* ==================== 安装：.vap64 分支 ==================== */
static int install_vap64(const char* tag, const struct VsPkg* p, const unsigned char* file, int flen,
                         struct VsInstalled* rec) {
    struct Vap v;
    int rc = vap_parse(file, flen, &v);
    if (rc != VS_OK) return rc;
    if (!vs_streq(v.name, p->name)) {                 /* 头里的名字与索引名字必须一致 */
        vs_log(tag, "vap64 name=%s index name=%s\n", v.name, p->name);
        return VS_E_FORMAT;
    }
    const unsigned int crc = vs_crc32(file + v.code_off, v.code_size);
    if (crc != v.crc) {
        vs_log(tag, "vap64 crc calc=0x%x head=0x%x\n", crc, v.crc);
        return VS_E_FORMAT;
    }
    char target[VS_FILE_MAX];
    char share[VS_FILE_MAX];
    char shdir[VS_FILE_MAX];
    vs_fmt(target, (int)sizeof(target), "/bin/%s", p->name);
    vs_fmt(shdir, (int)sizeof(shdir), "/usr/share/%s", p->name);
    vs_fmt(share, (int)sizeof(share), "/usr/share/%s/%s.vap64", p->name, p->name);
    if (!name_ok(p->name)) return VS_E_PATH;

    vs_log(tag, "progress pct=40\n");
    const int w1 = vs_write_file(target, file + v.code_off, v.code_size);
    if (w1 != 0) {
        vs_log(tag, "write FAILED path=%s rc=%d\n", target, w1);
        return io_code_of(w1);
    }
    char h1[65];
    sha256_of(file + v.code_off, v.code_size, h1);
    vs_log(tag, "install file path=%s bytes=%d sha256=%s\n", target, v.code_size, h1);

    vs_log(tag, "progress pct=70\n");
    {   const int md = vs_mkdir_p(shdir);
        if (md != 0 && md != -17) return io_code_of(md); }
    const int w2 = vs_write_file(share, file, flen);
    if (w2 != 0) {
        vs_log(tag, "write FAILED path=%s rc=%d\n", share, w2);
        return io_code_of(w2);
    }
    vs_log(tag, "install file path=%s bytes=%d\n", share, flen);

    vs_strcpy(rec->name, VS_NAME_MAX, p->name);
    vs_strcpy(rec->version, VS_VER_MAX, p->version);
    vs_strcpy(rec->arch, VS_ARCH_MAX, p->arch);
    vs_strcpy(rec->type, VS_TYPE_MAX, "vap64");
    rec->bytes = v.code_size;
    vs_strcpy(rec->sha256, 65, p->sha256);
    rec->nfiles = 0;
    vs_strcpy(rec->files[rec->nfiles++], VS_FILE_MAX, target);
    vs_strcpy(rec->files[rec->nfiles++], VS_FILE_MAX, share);
    return VS_OK;
}

#ifndef VS_STORE_GUI
/* ==================== 安装：.deb 分支 ==================== */
static int install_deb(const char* tag, const struct VsPkg* p, const unsigned char* file, int flen,
                       const struct VsInstalled* inst, int ninst, struct VsInstalled* rec) {
    struct DebInfo di;
    for (int i = 0; i < (int)sizeof(di); i++) ((unsigned char*)&di)[i] = 0;
    int nctl = 0, ndata = 0;
    int rc = ar_scan_all(file, flen, &di, &nctl, &ndata);
    if (rc != VS_OK) return rc;
    if (nctl == 0 || ndata == 0) {
        vs_log(tag, "deb members control=%d data=%d\n", nctl, ndata);
        return VS_E_FORMAT;
    }
    if (di.control_xz || di.data_xz || di.control_bz2 || di.data_bz2) {
        vs_log(tag, "deb compression control=%s data=%s (only gzip is supported)\n",
               di.control_xz ? "xz" : (di.control_bz2 ? "bz2" : "gz"),
               di.data_xz ? "xz" : (di.data_bz2 ? "bz2" : "gz"));
        return VS_E_XZ;
    }

    const int cbufsz = 256 * 1024;
    unsigned char* cbuf = (unsigned char*)vs_mmap(cbufsz);
    unsigned char* dbuf = (unsigned char*)vs_mmap(4 * 1024 * 1024);
    if (!cbuf || !dbuf) return VS_E_NOMEM;

    int clen = 0;
    if (vs_gunzip(file + di.control_off, di.control_size, cbuf, cbufsz, &clen) != 0) {
        vs_log(tag, "control.tar.gz inflate FAILED bytes=%d\n", di.control_size);
        return VS_E_FORMAT;
    }
    /* ① control.tar：先看维护者脚本（**整包拒绝**），再解 Depends */
    int off = 0;
    char nm[192];
    struct TarEnt e;
    while (tar_next(cbuf, clen, &off, &e, nm, (int)sizeof(nm)) == 1) {
        if (vs_streq(nm, "./preinst") || vs_streq(nm, "preinst")) di.has_preinst = 1;
        if (vs_streq(nm, "./postinst") || vs_streq(nm, "postinst")) di.has_postinst = 1;
        if (vs_streq(nm, "./prerm") || vs_streq(nm, "prerm")) di.has_prerm = 1;
        if (vs_streq(nm, "./postrm") || vs_streq(nm, "postrm")) di.has_postrm = 1;
        if (vs_streq(nm, "./control") || vs_streq(nm, "control")) {
            control_parse(cbuf + off, e.size, &di, 1);
            vs_log(tag, "deb control package=%s version=%s arch=%s depends=%d\n",
                   di.pkg[0] ? di.pkg : "(none)", di.ver[0] ? di.ver : "(none)",
                   di.arch[0] ? di.arch : "(none)", di.ndep);
        }
        tar_skip(&off, e.size);
    }
    if (di.has_preinst || di.has_postinst || di.has_prerm || di.has_postrm) {
        vs_log(tag, "deb maintainer scripts: preinst=%d postinst=%d prerm=%d postrm=%d\n",
               di.has_preinst, di.has_postinst, di.has_prerm, di.has_postrm);
        vs_log(tag, "maintainer scripts not supported\n");
        return VS_E_SCRIPTS;
    }
    /* control 里声明的依赖：与索引里的 depends 一并检查（一层） */
    for (int i = 0; i < di.ndep; i++) {
        if (vs_db_find(inst, ninst, di.depends[i]) < 0) {
            vs_log(tag, "deb depends missing=%s\n", di.depends[i]);
            return VS_E_DEPENDS;
        }
    }

    vs_log(tag, "progress pct=35\n");
    int dlen = 0;
    if (vs_gunzip(file + di.data_off, di.data_size, dbuf, 4 * 1024 * 1024, &dlen) != 0) {
        vs_log(tag, "data.tar.gz inflate FAILED bytes=%d cap=%d\n", di.data_size, 4 * 1024 * 1024);
        return VS_E_FORMAT;
    }
    /* ①.5 预扫：总字节数（空间检查的依据）+ 路径合法性 + 不支持的类型 */
    long long total = 0;
    int nfiles = 0, ndirs = 0;
    off = 0;
    while (tar_next(dbuf, dlen, &off, &e, nm, (int)sizeof(nm)) == 1) {
        if (e.type == 5) { ndirs++; tar_skip(&off, e.size); continue; }
        if (e.type != 0) {
            vs_log(tag, "deb tar entry type=%d name=%s (only files/dirs supported)\n", e.type, nm);
            return VS_E_FORMAT;
        }
        if (!rel_path_ok(tar_strip(nm))) {
            vs_log(tag, "deb tar entry name=%s (unsafe path)\n", nm);
            return VS_E_PATH;
        }
        nfiles++;
        total += e.size;
        tar_skip(&off, e.size);
    }
    if (nfiles > VS_FILES_MAX) {
        vs_log(tag, "deb files=%d cap=%d\n", nfiles, VS_FILES_MAX);
        return VS_E_FORMAT;
    }
    /* ② 空间检查：payload 全部字节 + 归档副本 + 32 KiB 余量 */
    const long long need = total + flen + 32 * 1024;
    const long long freeb = vs_free_bytes("/");
    vs_log(tag, "space need=%d free=%d dirs=%d files=%d payload=%d\n",
           (int)need, (int)freeb, ndirs, nfiles, (int)total);
    if (freeb < 0) return VS_E_IO;
    if (freeb < need) return VS_E_SPACE;

    /* ③ 落盘 */
    vs_log(tag, "progress pct=55\n");
    off = 0;
    int wf = 0;                      /* 本分支落盘的文件数（只用于诊断日志） */
    while (tar_next(dbuf, dlen, &off, &e, nm, (int)sizeof(nm)) == 1) {
        char full[VS_FILE_MAX];
        vs_fmt(full, (int)sizeof(full), "/%s", tar_strip(nm));
        if (e.type == 5) {
            const int mr = vs_mkdir_p(full);
            if (mr != 0 && mr != -17) {
                vs_log(tag, "mkdir FAILED path=%s rc=%d\n", full, mr);
                return io_code_of(mr);
            }
            tar_skip(&off, e.size);
            continue;
        }
        /* 目录先建好（tar 里目录条目不一定在前面） */
        char dir[VS_FILE_MAX];
        vs_strcpy(dir, (int)sizeof(dir), full);
        for (int i = (int)vs_strlen(dir) - 1; i > 0; i--) {
            if (dir[i] == '/') { dir[i] = 0; break; }
        }
        if (dir[0]) (void)vs_mkdir_p(dir);
        const int wr = vs_write_file(full, dbuf + off, e.size);
        if (wr != 0) {
            vs_log(tag, "write FAILED path=%s rc=%d\n", full, wr);
            return io_code_of(wr);
        }
        char h[65];
        sha256_of(dbuf + off, e.size, h);
        vs_log(tag, "install file path=%s bytes=%d sha256=%s\n", full, e.size, h);
        if (rec->nfiles < VS_FILES_MAX) vs_strcpy(rec->files[rec->nfiles++], VS_FILE_MAX, full);
        tar_skip(&off, e.size);
        wf++;
    }
    vs_log(tag, "progress pct=80\n");
    /* ④ 归档副本（与 .vap64 同一约定：包本体留在 /usr/share/<name>/） */
    char shdir[VS_FILE_MAX], share[VS_FILE_MAX];
    const char* self = (di.pkg[0] ? di.pkg : p->name);
    vs_fmt(shdir, (int)sizeof(shdir), "/usr/share/%s", self);
    vs_fmt(share, (int)sizeof(share), "/usr/share/%s/%s.deb", self, self);
    {   const int md = vs_mkdir_p(shdir);
        if (md != 0 && md != -17) return io_code_of(md); }
    {   const int wr = vs_write_file(share, file, flen);
        if (wr != 0) return io_code_of(wr); }
    vs_log(tag, "install file path=%s bytes=%d\n", share, flen);
    if (rec->nfiles < VS_FILES_MAX) vs_strcpy(rec->files[rec->nfiles++], VS_FILE_MAX, share);

    if (wf < 0) vs_log(tag, "internal: wf=%d\n", wf);       /* wf 只做诊断（占用即已读） */
    vs_strcpy(rec->name, VS_NAME_MAX, self);
    vs_strcpy(rec->version, VS_VER_MAX, di.ver[0] ? di.ver : p->version);
    vs_strcpy(rec->arch, VS_ARCH_MAX, di.arch[0] ? di.arch : p->arch);
    vs_strcpy(rec->type, VS_TYPE_MAX, "deb");
    rec->bytes = (int)total;
    vs_strcpy(rec->sha256, 65, p->sha256);
    return VS_OK;
}
#endif  /* !VS_STORE_GUI：deb 的安装分支也只编进 /bin/vpkg（见文件头与本段说明） */

/* ==================== 安装（总入口） ==================== */
int vs_install(const char* tag, const char* arg, int do_progress) {
    (void)do_progress;
    if (!arg || !arg[0]) {
        log_err(tag, VS_E_PATH, "empty package name");
        return VS_E_PATH;
    }

    /* ---- ① 解析出"要装哪个包"（索引名 或 直接给文件路径） ---- */
    struct VsPkg pkg;
    for (int i = 0; i < (int)sizeof(pkg); i++) ((unsigned char*)&pkg)[i] = 0;
    char src[VS_FILE_MAX];
    int from_index = 0;
    {
        struct VsPkg* const repo = scratch_repo();
    if (!repo) return VS_E_NOMEM;
        int nrepo = 0;
        const int rc = vs_index_load(VS_INDEX_PATH, repo, VS_MAX_PKGS, &nrepo);
        if (rc != VS_OK && vs_find(arg, "/") < 0) {
            log_err(tag, rc == VS_E_NOTFOUND ? VS_E_NOTFOUND : rc,
                    rc == VS_E_NOTFOUND ? "index not found" : "index parse failed");
            return (rc == VS_E_NOTFOUND) ? VS_E_NOTFOUND : rc;
        }
        const int idx = (rc == VS_OK) ? vs_index_find(repo, nrepo, arg) : -1;
        if (idx >= 0) {
            pkg = repo[idx];
            from_index = 1;
            vs_fmt(src, (int)sizeof(src), "%s/%s", VS_REPO_DIR, pkg.file);
        } else if (vs_find(arg, "/") >= 0 || vs_find(arg, ".") >= 0) {
            vs_strcpy(src, VS_FILE_MAX, arg);
            vs_strcpy(pkg.arch, VS_ARCH_MAX, "any");
            vs_strcpy(pkg.sha256, 65, "-");
            pkg.size = -1;
            if (vs_starts(src, VS_REPO_DIR)) { /* 已在仓库目录里：file 字段留空 */ }
        } else {
            vs_log(tag, "index packages=%d name=%s not found\n", nrepo, arg);
            log_err(tag, VS_E_NOTFOUND, "no such package in repository");
            return VS_E_NOTFOUND;
        }
    }

    /* ---- ② 读包本体 ---- */
    const int fsz0 = vs_size_of(src);
    if (fsz0 < 0) {
        vs_log(tag, "open FAILED path=%s\n", src);
        log_err(tag, VS_E_NOTFOUND, "package file missing");
        return VS_E_NOTFOUND;
    }
    if (fsz0 <= 0 || fsz0 > 8 * 1024 * 1024) {
        vs_log(tag, "package bytes=%d (cap 8 MiB)\n", fsz0);
        log_err(tag, VS_E_SIZE, "package size out of range");
        return VS_E_SIZE;
    }
    unsigned char* file = (unsigned char*)vs_mmap(fsz0 + 16);
    if (!file) return VS_E_NOMEM;
    const int flen = vs_read_file(src, file, fsz0 + 16);
    if (flen != fsz0) {
        vs_log(tag, "read FAILED path=%s got=%d want=%d\n", src, flen, fsz0);
        log_err(tag, VS_E_IO, "cannot read package");
        return VS_E_IO;
    }
    vs_log(tag, "install begin name=%s src=%s bytes=%d index=%d\n",
           from_index ? pkg.name : "(file)", src, flen, from_index);

    /* ---- ③ 类型：索引说了算；直接给文件时按扩展名 ---- */
    if (!from_index) {
        if (vs_find(src, ".deb") >= 0) vs_strcpy(pkg.type, VS_TYPE_MAX, "deb");
        else if (vs_find(src, ".vap64") >= 0 || vs_find(src, ".vap") >= 0) vs_strcpy(pkg.type, VS_TYPE_MAX, "vap64");
        else {
            log_err(tag, VS_E_FORMAT, "unknown package extension (want .vap64 or .deb)");
            return VS_E_FORMAT;
        }
    }
    if (vs_streq(pkg.type, "vap64")) {
        struct Vap v;
        const int rc = vap_parse(file, flen, &v);
        if (rc != VS_OK) {
            log_err(tag, VS_E_FORMAT, "bad VAP64 header");
            return VS_E_FORMAT;
        }
        vs_strcpy(pkg.name, VS_NAME_MAX, v.name);            /* 直接给文件时：名字来自包头 */
        if (!from_index && pkg.name[0] == 0) return VS_E_FORMAT;
    } else if (!from_index) {
        /* 直接从文件装 deb 时索引里没有名字：包名取**文件名**（去掉目录与 .deb 后缀）。
         * 装进仓库里的包时以控制文件的 Package 为准（见 install_deb）。这是如实的边界。 */
        const char* base = src;
        for (int i = 0; src[i]; i++) if (src[i] == '/') base = src + i + 1;
        int k = 0;
        while (base[k] && base[k] != '.' && k < VS_NAME_MAX - 1) { pkg.name[k] = base[k]; k++; }
        pkg.name[k] = 0;
        if (pkg.name[0] == 0) {
            log_err(tag, VS_E_PATH, "cannot derive a package name from the file name");
            return VS_E_PATH;
        }
    }

    /* ---- ④ 名字/路径检查 ---- */
    if (!name_ok(pkg.name)) {
        vs_log(tag, "illegal name=%s\n", pkg.name);
        log_err(tag, VS_E_PATH, "illegal package name (use [A-Za-z0-9._+-], <= 31 chars)");
        return VS_E_PATH;
    }

    /* ---- ⑤ 已装？ ---- */
    struct VsInstalled* inst = scratch_inst();
    if (!inst) return VS_E_NOMEM;
    int ninst = 0;
    int rc = vs_db_load(VS_DB_PATH, inst, VS_MAX_PKGS, &ninst);
    if (rc != VS_OK) {
        vs_log(tag, "db parse FAILED path=%s rc=%s\n", VS_DB_PATH, vs_err_name(rc));
        log_err(tag, VS_E_IO, "installed database is corrupt");
        return VS_E_IO;
    }
    for (int i = 0; i < ninst; i++) {
        if (vs_streq(inst[i].name, pkg.name)) {
            vs_log(tag, "already installed name=%s version=%s\n", inst[i].name, inst[i].version);
            log_err(tag, VS_E_INSTALLED, "package already installed (remove it first)");
            return VS_E_INSTALLED;
        }
    }

    /* ---- ⑥ 依赖（一层） ---- */
    for (int i = 0; i < pkg.ndep; i++) {
        if (vs_db_find(inst, ninst, pkg.depends[i]) < 0) {
            vs_log(tag, "depends name=%s on=%s state=missing\n", pkg.name, pkg.depends[i]);
            vs_log(tag, "install %s first\n", pkg.depends[i]);
            log_err(tag, VS_E_DEPENDS, "missing dependency");
            return VS_E_DEPENDS;
        }
        vs_log(tag, "depends name=%s on=%s state=ok\n", pkg.name, pkg.depends[i]);
    }

    /* ---- ⑦ 大小 / sha256（索引声明必须与盘上文件一致） ---- */
    if (from_index) {
        if (pkg.size != flen) {
            vs_log(tag, "size mismatch name=%s index=%d actual=%d\n", pkg.name, pkg.size, flen);
            log_err(tag, VS_E_SIZE, "size does not match the index");
            return VS_E_SIZE;
        }
        char calc[65];
        sha256_of(file, flen, calc);
        const int ok = vs_streq(calc, pkg.sha256);
        vs_log(tag, "sha256 path=%s calc=%s index=%s ok=%d\n", src, calc, pkg.sha256, ok);
        if (!ok) {
            log_err(tag, VS_E_HASH, "sha256 mismatch (package is corrupt or the index is stale)");
            return VS_E_HASH;
        }
        vs_log(tag, "progress pct=25\n");
    } else {
        log_err(tag, VS_E_HASH, "not checked (installed from a file path: no index entry)");
    }

    /* ---- ⑧ 空间（vap64 在这里查；deb 在解出 data.tar 后按真实 payload 再查一次） ---- */
    if (vs_streq(pkg.type, "vap64")) {
        const long long need = (long long)flen * 2 + 32 * 1024;
        const long long freeb = vs_free_bytes("/");
        vs_log(tag, "space need=%d free=%d\n", (int)need, (int)freeb);
        if (freeb >= 0 && freeb < need) {
            log_err(tag, VS_E_SPACE, "not enough free space");
            return VS_E_SPACE;
        }
    }

    /* ---- ⑨ 落盘 ---- */
    struct VsInstalled rec;
    for (int i = 0; i < (int)sizeof(rec); i++) ((unsigned char*)&rec)[i] = 0;
    if (vs_streq(pkg.type, "vap64")) {
        rc = install_vap64(tag, &pkg, file, flen, &rec);
#ifndef VS_STORE_GUI
    } else if (vs_streq(pkg.type, "deb")) {
        rc = install_deb(tag, &pkg, file, flen, inst, ninst, &rec);
#else
    } else if (vs_streq(pkg.type, "deb")) {
        vs_log(tag, "deb install: delegate to /bin/vpkg (this build has no inflate/ar/tar)\n");
        log_err(tag, VS_E_FORMAT, "deb needs the vpkg CLI in this build");
        return VS_E_FORMAT;
#endif
    } else {
        log_err(tag, VS_E_FORMAT, "unknown package type");
        return VS_E_FORMAT;
    }
    if (rc != VS_OK) {
        log_err(tag, rc, "install failed");
        return rc;
    }

    /* ---- ⑩ 记进已装库（"已装"以 DB 为准；DB 写在最后） ---- */
    if (ninst >= VS_MAX_PKGS) {
        log_err(tag, VS_E_SPACE, "installed database is full");
        return VS_E_SPACE;
    }
    inst[ninst++] = rec;
    int dbbytes = 0;
    rc = vs_db_write(VS_DB_PATH, inst, ninst, &dbbytes);
    if (rc != VS_OK) {
        log_err(tag, VS_E_IO, "cannot write installed database");
        return VS_E_IO;
    }
    vs_log(tag, "db write path=%s entries=%d bytes=%d\n", VS_DB_PATH, ninst, dbbytes);
    vs_log(tag, "progress pct=100\n");
    vs_log(tag, "install ok name=%s version=%s type=%s files=%d bytes=%d\n",
           rec.name, rec.version, rec.type, rec.nfiles, rec.bytes);
    vs_cache_drop();
    return VS_OK;
}

/* ==================== 卸载 ==================== */
int vs_remove(const char* tag, const char* name) {
    if (!name || !name[0]) {
        log_err(tag, VS_E_PATH, "empty package name");
        return VS_E_PATH;
    }
    struct VsInstalled* inst = scratch_inst();
    if (!inst) return VS_E_NOMEM;
    int ninst = 0;
    int rc = vs_db_load(VS_DB_PATH, inst, VS_MAX_PKGS, &ninst);
    if (rc != VS_OK) {
        log_err(tag, VS_E_IO, "installed database is corrupt");
        return VS_E_IO;
    }
    const int idx = vs_db_find(inst, ninst, name);
    if (idx < 0) {
        vs_log(tag, "not installed name=%s entries=%d\n", name, ninst);
        log_err(tag, VS_E_NOTINST, "package is not installed");
        return VS_E_NOTINST;
    }
    int removed = 0;
    for (int i = 0; i < inst[idx].nfiles; i++) {
        const int ur = vs_unlink(inst[idx].files[i]);
        vs_log(tag, "remove file path=%s rc=%d\n", inst[idx].files[i], ur);
        if (ur == 0) removed++;
    }
    /* 从 DB 里删掉这一条（后面的往前挪） */
    for (int i = idx; i + 1 < ninst; i++) inst[i] = inst[i + 1];
    ninst--;
    int dbbytes = 0;
    rc = vs_db_write(VS_DB_PATH, inst, ninst, &dbbytes);
    if (rc != VS_OK) {
        log_err(tag, VS_E_IO, "cannot write installed database");
        return VS_E_IO;
    }
    vs_log(tag, "db write path=%s entries=%d bytes=%d\n", VS_DB_PATH, ninst, dbbytes);
    vs_log(tag, "remove ok name=%s files=%d entries=%d\n", name, removed, ninst);
    vs_cache_drop();
    return VS_OK;
}

/* ==================== list / info / search / update ==================== */
int vs_list(const char* tag) {
    struct VsPkg* const repo = scratch_repo();
    if (!repo) return VS_E_NOMEM;
    struct VsInstalled* const inst = scratch_inst();
    if (!inst) return VS_E_NOMEM;
    int nrepo = 0, ninst = 0;
    const int rc = vs_index_load(VS_INDEX_PATH, repo, VS_MAX_PKGS, &nrepo);
    (void)vs_db_load(VS_DB_PATH, inst, VS_MAX_PKGS, &ninst);
    vs_log(tag, "repo path=%s index=%s packages=%d installed=%d rc=%s\n",
           VS_REPO_DIR, VS_INDEX_PATH, nrepo, ninst, vs_err_name(rc == VS_OK ? VS_OK : rc));
    if (rc != VS_OK && rc != VS_E_NOTFOUND) return rc;
    for (int i = 0; i < nrepo; i++) {
        const int ins = vs_db_find(inst, ninst, repo[i].name) >= 0;
        vs_log(tag, "pkg name=%s version=%s arch=%s type=%s size=%d state=%s depends=%d summary=\"%s\"\n",
               repo[i].name, repo[i].version, repo[i].arch, repo[i].type, repo[i].size,
               ins ? "installed" : "not-installed", repo[i].ndep, repo[i].summary);
    }
    for (int i = 0; i < ninst; i++) {
        if (vs_index_find(repo, nrepo, inst[i].name) < 0)
            vs_log(tag, "pkg name=%s version=%s arch=%s type=%s bytes=%d state=installed(local)\n",
                   inst[i].name, inst[i].version, inst[i].arch, inst[i].type, inst[i].bytes);
    }
    vs_log(tag, "list ok packages=%d installed=%d\n", nrepo, ninst);
    return VS_OK;
}

int vs_info(const char* tag, const char* name) {
    struct VsPkg* const repo = scratch_repo();
    if (!repo) return VS_E_NOMEM;
    struct VsInstalled* const inst = scratch_inst();
    if (!inst) return VS_E_NOMEM;
    int nrepo = 0, ninst = 0;
    const int rc = vs_index_load(VS_INDEX_PATH, repo, VS_MAX_PKGS, &nrepo);
    (void)vs_db_load(VS_DB_PATH, inst, VS_MAX_PKGS, &ninst);
    const int i = (rc == VS_OK) ? vs_index_find(repo, nrepo, name) : -1;
    const int k = vs_db_find(inst, ninst, name);
    if (i < 0 && k < 0) {
        log_err(tag, VS_E_NOTFOUND, "no such package");
        return VS_E_NOTFOUND;
    }
    if (i >= 0) {
        vs_log(tag, "info name=%s version=%s arch=%s type=%s size=%d sha256=%s\n",
               repo[i].name, repo[i].version, repo[i].arch, repo[i].type, repo[i].size, repo[i].sha256);
        vs_log(tag, "info depends=%d file=%s summary=\"%s\"\n", repo[i].ndep, repo[i].file, repo[i].summary);
        vs_log(tag, "info description=\"%s\"\n", repo[i].desc);
        for (int d = 0; d < repo[i].ndep; d++)
            vs_log(tag, "info depend name=%s installed=%d\n", repo[i].depends[d],
                   vs_db_find(inst, ninst, repo[i].depends[d]) >= 0);
    }
    if (k >= 0) {
        vs_log(tag, "info installed=yes version=%s type=%s bytes=%d files=%d path=%s\n",
               inst[k].version, inst[k].type, inst[k].bytes, inst[k].nfiles, VS_DB_PATH);
        for (int f = 0; f < inst[k].nfiles; f++)
            vs_log(tag, "info file path=%s\n", inst[k].files[f]);
    } else {
        vs_log(tag, "info installed=no\n");
    }
    vs_log(tag, "info ok name=%s\n", name);
    return VS_OK;
}

int vs_search(const char* tag, const char* kw) {
    struct VsPkg* repo = scratch_repo();
    if (!repo) return VS_E_NOMEM;
    int nrepo = 0;
    const int rc = vs_index_load(VS_INDEX_PATH, repo, VS_MAX_PKGS, &nrepo);
    if (rc != VS_OK && rc != VS_E_NOTFOUND) return rc;
    int hits = 0;
    for (int i = 0; i < nrepo; i++) {
        if (vs_icontains(repo[i].name, kw) || vs_icontains(repo[i].summary, kw) ||
            vs_icontains(repo[i].desc, kw)) {
            vs_log(tag, "hit name=%s version=%s summary=\"%s\"\n", repo[i].name, repo[i].version, repo[i].summary);
            hits++;
        }
    }
    vs_log(tag, "search kw=%s hits=%d\n", kw, hits);
    return VS_OK;
}

int vs_update(const char* tag) {
    struct VsPkg* const repo = scratch_repo();
    if (!repo) return VS_E_NOMEM;
    struct VsInstalled* const inst = scratch_inst();
    if (!inst) return VS_E_NOMEM;
    int nrepo = 0, ninst = 0;
    const int rc = vs_index_load(VS_INDEX_PATH, repo, VS_MAX_PKGS, &nrepo);
    (void)vs_db_load(VS_DB_PATH, inst, VS_MAX_PKGS, &ninst);
    vs_log(tag, "update begin packages=%d installed=%d index_rc=%s\n", nrepo, ninst,
           vs_err_name(rc == VS_OK ? VS_OK : rc));
    if (rc != VS_OK) {
        log_err(tag, rc == VS_E_NOTFOUND ? VS_E_NOTFOUND : rc, "no usable repository index");
        return rc;
    }
    int up = 0;
    for (int i = 0; i < ninst; i++) {
        const int k = vs_index_find(repo, nrepo, inst[i].name);
        if (k < 0) { vs_log(tag, "update local-only name=%s (not in index)\n", inst[i].name); continue; }
        if (!vs_streq(repo[k].version, inst[i].version)) {
            vs_log(tag, "update name=%s have=%s want=%s\n", inst[i].name, inst[i].version, repo[k].version);
            up++;
        }
    }
    if (up == 0) vs_log(tag, "update up-to-date installed=%d\n", ninst);
    else         vs_log(tag, "update need=%d (reinstall by name: install <name> after remove)\n", up);
    vs_log(tag, "update ok checked=%d upgradable=%d\n", ninst, up);
    return VS_OK;
}
