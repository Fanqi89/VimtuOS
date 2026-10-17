// fontcfg64.cpp —— 最小 fontconfig（v0.4.8）：/etc/fonts.conf 的读/种 + 渲染策略落地
//
// 设计取舍（为什么是"最小实现"而不是移植 fontconfig）：
//   本内核是单体内核、没有用户态动态库，移植 fontconfig 需要 Expat/freetype/dirent 一整套，
//   而实际只有 4 个编译期内嵌字体面 —— 需要的只是"按名字选面 + 调渲染参数"。所以这里实现
//   **配置文件的读写 + 一个两键的解析器**，语义对用户等价（可读、可改、重启生效、有打点可验收）。
//
// 文件格式（每行一条，`#` 开头是注释，`=` 两侧空格随意）：
//   ui.family    = Noto Sans          # 仅记录（族名来自构建期子集，运行期不可换字体文件）
//   ui.stem      = 120                # 笔画加厚百分比：AA 覆盖率 ×1.2（越大笔画越实、越不"发虚"）
//   ui.antialias = on                 # on/off；off = 与终端同一套单色阈值渲染
//   mono.family  = Sarasa Mono SC     # 终端面（三无：始终走单色阈值，本项只记录）
//   mono.mode    = mono               # mono/aa；当前只支持 mono（终端规范要求）
//   cjk.family   = Noto Sans SC
//   fallback.family = Unifont
#include "fontcfg64.h"
#include "debug64.h"
#include "font.h"
#include "vfs64.h"

#define FONTCFG64_PATH  "/etc/fonts.conf"
#define FONTCFG64_MAX   1024

static int  g_stem = 120;             // 默认：笔画加厚 20%（用户反馈"颜色过浅/不够清晰"）
static int  g_aa   = 1;
static int  g_loaded = 0;
static const char* g_src = "file";        // 配置来源：file / default(seed-denied) / ...
static char g_fam_ui[32]   = "Noto Sans";
static char g_fam_mono[32] = "Sarasa Mono SC";
static char g_fam_cjk[32]  = "Noto Sans SC";
static char g_fam_fb[32]   = "Unifont";

static const char kDefaultConf[] =          // ★ 必须是数组：写成指针时 sizeof 只有 8（实测 seed 只写 7 字节）
    "# VimtuOS fontconfig (minimal) - 由内核首次启动时种下，可自由修改，重启生效\n"
    "# 角色 -> 字体面；字体文件是构建期子集并内嵌的 4 个面，运行期不能换文件\n"
    "ui.family       = Noto Sans\n"
    "ui.stem         = 120\n"
    "ui.antialias    = on\n"
    "mono.family     = Sarasa Mono SC\n"
    "mono.mode       = mono\n"
    "cjk.family      = Noto Sans SC\n"
    "fallback.family = Unifont\n";

// ---- 小工具（不依赖 libc 的 strcmp/trim/atoi）----
static int fc_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}
static const char* fc_skip_ws(const char* p) { while (*p == ' ' || *p == '\t') p++; return p; }
static int fc_trim_len(const char* p) {
    int n = 0;
    while (p[n] && p[n] != '\n' && p[n] != '\r') n++;
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    return n;
}
static int fc_starts(const char* line, const char* key) {
    int i = 0;
    while (key[i]) { if (line[i] != key[i]) return 0; i++; }
    return 1;
}
static void fc_copy(char* dst, int cap, const char* src, int n) {
    int i = 0;
    while (i < n && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

// 解析一行；返回 1 = 这行被认了
static int fc_apply_line(const char* line) {
    const char* p = fc_skip_ws(line);
    if (!*p || *p == '#' || *p == '\n' || *p == '\r') return 0;
    const char* eq = p;
    while (*eq && *eq != '=') eq++;
    if (!*eq) return 0;                                  // 没有 '='：忽略（不猜）
    const char* val = fc_skip_ws(eq + 1);
    const int vlen = fc_trim_len(val);

    if (fc_starts(p, "ui.stem")) {
        int v = 0, any = 0;
        for (int i = 0; i < vlen; i++) {
            if (val[i] < '0' || val[i] > '9') break;
            v = v * 10 + (val[i] - '0');
            any = 1;
            if (v > 1000) { v = 1000; break; }
        }
        if (any) { g_stem = v; return 1; }
        return 0;
    }
    if (fc_starts(p, "ui.antialias")) {
        g_aa = (val[0] == 'o' && val[1] == 'f') ? 0 : 1;   // "off" -> 0，其余按开
        return 1;
    }
    if (fc_starts(p, "ui.family"))    { fc_copy(g_fam_ui,   (int)sizeof(g_fam_ui),   val, vlen); return 1; }
    if (fc_starts(p, "mono.family"))  { fc_copy(g_fam_mono, (int)sizeof(g_fam_mono), val, vlen); return 1; }
    if (fc_starts(p, "cjk.family"))   { fc_copy(g_fam_cjk,  (int)sizeof(g_fam_cjk),  val, vlen); return 1; }
    if (fc_starts(p, "fallback.family")) { fc_copy(g_fam_fb, (int)sizeof(g_fam_fb), val, vlen); return 1; }
    if (fc_starts(p, "mono.mode"))    { return 1; }        // 只记录：终端恒为 mono（三无规范）
    return 0;
}

static void fc_log(const char* why) {
    dbg64_line_begin64();
    dbg64_str("[FONTCFG] ");
    dbg64_str(why);
    dbg64_str(" path=");
    dbg64_str(FONTCFG64_PATH);
    dbg64_str(" ui_stem=");
    dbg64_dec((uint64_t)g_stem);
    dbg64_str(" aa=");
    dbg64_dec((uint64_t)g_aa);
    dbg64_str(" ui=");
    dbg64_str(g_fam_ui);
    dbg64_str(" mono=");
    dbg64_str(g_fam_mono);
    dbg64_str(" cjk=");
    dbg64_str(g_fam_cjk);
    dbg64_str(" fb=");
    dbg64_str(g_fam_fb);
    dbg64_str(" faces=");
    dbg64_dec((uint64_t)font_face_count());
    dbg64_nl();
    dbg64_line_end64();
}

int fontcfg64_init64() {
    if (g_loaded) return 0;
    static char buf[FONTCFG64_MAX];
    int n = (int)vfs64_read64(FONTCFG64_PATH, buf, (uint32_t)(sizeof(buf) - 1));
    if (n <= 0) {
        // 没有就种一份默认（幂等；写失败只打点，绝不影响启动）
        int len = 0;                                  // 用数组长度，不依赖 sizeof 的指针陷阱
        while (kDefaultConf[len]) len++;
        const int rc = vfs64_write64(FONTCFG64_PATH, kDefaultConf, (uint32_t)len);
        dbg64_line_begin64();
        dbg64_str("[FONTCFG] seed path=");
        dbg64_str(FONTCFG64_PATH);
        dbg64_str(" bytes=");
        dbg64_dec((uint64_t)len);
        dbg64_str(" rc=");
        dbg64_dec((uint64_t)(rc < 0 ? 1 : 0));
        dbg64_str(" raw=");
        dbg64_dec((uint64_t)(uint32_t)(rc < 0 ? -rc : rc));
        dbg64_nl();
        dbg64_line_end64();
        // ★ 写不进去**不是故障**：/etc 是 uid=0 的目录，普通会话（uid=1000）无权在里头建文件
        //   （实测 [PERM64] deny 同源：/etc/apps.json 也是靠"夹具期预建 0666"才写得进）。
        //   所以这条路退化成"用内置默认"，并且**把来源如实打进日志**（src=default）。
        //   想在盘上改渲染策略的用户/发行版，只需在任何有写权限的镜像构建步骤里预置
        //   /etc/fonts.conf（格式见本文件头部注释），本函数下次启动就会读它。
        if (rc < 0) { g_src = "default(seed-denied)"; }
        else {
            n = (int)vfs64_read64(FONTCFG64_PATH, buf, (uint32_t)(sizeof(buf) - 1));
            if (n <= 0) g_src = "default(readback-empty)";
        }
    }
    if (n > FONTCFG64_MAX - 1) n = FONTCFG64_MAX - 1;
    buf[n] = 0;

    int applied = 0;
    const char* p = buf;
    while (*p) {
        const char* e = p;
        while (*e && *e != '\n') e++;
        char line[160];
        const int len = (int)(e - p);
        fc_copy(line, (int)sizeof(line), p, len);
        if (fc_apply_line(line)) applied++;
        p = (*e) ? e + 1 : e;
    }
    g_loaded = 1;

    // 落地到字体层：笔画加厚 + UI 抗锯齿开关（终端的 mono 由终端的绘制路径强制，不受这里影响）
    font_set_stem64(g_stem);
    font_set_ui_aa64(g_aa);
    fc_log("load");
    dbg64_line_begin64();
    dbg64_str("[FONTCFG] applied src=");
    dbg64_str(g_src);
    dbg64_str(" keys=");
    dbg64_dec((uint64_t)applied);
    dbg64_str(" stem=");
    dbg64_dec((uint64_t)font_stem64());
    dbg64_str(" ui_aa=");
    dbg64_dec((uint64_t)font_ui_aa64());
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

const char* fontcfg64_path64() { return FONTCFG64_PATH; }
int fontcfg64_stem64() { return g_stem; }
int fontcfg64_aa64() { return g_aa; }

int fontcfg64_role_face64(const char* role) {
    if (!role) return -1;
    if (fc_eq(role, "ui"))       return FONT_FACE_ASCII;
    if (fc_eq(role, "mono"))     return FONT_FACE_MONO;
    if (fc_eq(role, "cjk"))      return FONT_FACE_CJK;
    if (fc_eq(role, "fallback")) return FONT_FACE_FALLBACK;
    return -1;
}

const char* fontcfg64_role_family64(const char* role) {
    if (!role) return "";
    if (fc_eq(role, "ui"))       return g_fam_ui;
    if (fc_eq(role, "mono"))     return g_fam_mono;
    if (fc_eq(role, "cjk"))      return g_fam_cjk;
    if (fc_eq(role, "fallback")) return g_fam_fb;
    return "";
}
