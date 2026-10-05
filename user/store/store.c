/* store.c - ★ /bin/store：VimtuOS 应用商店的 **GUI**（纯 ring3；走 /bin/wm 合成器 + user/lib/font64.h）
 *
 * 它证明的闭环：**在图形界面里点一下，包真的进卷、程序真的跑起来**。
 *   ① 拉起 Ring 3 合成器（/lib/wm.elf；内核启动期只认 /bin/wm.elf，桌面期由客户端自己 fork，与
 *      sdk/software-template/apps/hello-gui 完全同一套做法）；
 *   ② 自己的窗口 = 一块 shm 画布（256x60 = 15360 px，单 surface 上限 16384 px）+ font64 画字；
 *   ③ 事件循环：wl_display_dispatch 收 seat 事件（鼠标点击/移动/键盘），自己做命中测试；
 *   ④ 「安装 / 卸载 / 启动」三个按钮调 **与 /bin/vpkg 同一份**的安装引擎（user/store/vs_pkg.c）；
 *      启动 = 写一条路径给**提前 fork 好的 execve 助手**（见下面"为什么提前 fork"）；
 *
 * ★ 两处"体积/内存"上的取舍（都如实写在这里，实测踩出来的）：
 *   1) 本二进制用 -DVS_STORE_GUI 编 vs_pkg.c：**不编** deb 的 gzip/ar/tar（那一段留给 /bin/vpkg），
 *      deb 的安装走 `/bin/vpkg install <name>` 子进程 —— 同一份引擎，一个字节都不重复；
 *   2) 仓库/已装库两张表**不放静态区**（本仓库一律 -fno-zero-initialized-in-bss，全零静态数组会落进
 *      .data 把 64 KiB 主程序装载区顶爆），而是用 vs_pkg.c 的 mmap 缓存 + 这里的指针。
 *
 * 界面布局（像素，XRGB8888；测试按这些常量算点击坐标）：
 *   y 0..13    标题条（底 + 标题文字 + "n/m" 计数）
 *   y 14..25   包 1 行     y 26..37  包 2 行     y 38..49  包 3 行（每行 12 px：[图标][名字][版本][大小][状态]）
 *   y 48..59   按钮条：[Install] [Remove] [Launch] + 右侧状态/进度条
 *   （行区与按钮条重叠 2 px 是有意的：按钮带底框，画在最后，压住行尾。）
 *
 * 打点（tests/vpkg64_test.py 按这些串 grep；格式勿改）：
 *   [STORE] ver=1 pid=<n>
 *   [STORE] wm path=/lib/wm.elf pid=<n>
 *   [STORE] launcher pid=<n> wfd=<n>
 *   [STORE] fonts face=… bytes=… ok=<0|1>
 *   [STORE] surf id=<n> w=256 h=60 shm=<n> map=0
 *   [STORE] repo packages=<n> installed=<n> page=<a>/<b>
 *   [STORE] render f=<n> sel=<n> row0=<name> row1=<name> row2=<name> state0=<..>
 *   [STORE] mouse sx=.. sy=.. x=.. y=..
 *   [STORE] click sx=.. sy=.. btn=.. hit=<install|remove|launch|row|none> sel=<n> name=<..>
 *   [STORE] install begin name=.. / progress pct=.. （引擎打的，前缀就是 [STORE]）
 *   [STORE] install ok name=.. files=.. bytes=..
 *   [STORE] launch name=.. path=/bin/.. pid=<n> via=pipe rc=<n>
 *   [STORE] done frames=<n> reason=<esc|timeout|error>
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "vimtu64.h"
#include "wl.h"
#include "font64.h"
#include "vs.h"

#define SW            256
#define SH            60
#define SW_BYTES      (SW * SH * 4)
#define EV_MAX        32
#define MAX_FRAMES    1200            /* 上限：约 40 s（wm 的 full 模式上限 45 s） */
#define FRAME_MS      30u

/* ---- 布局常量（测试按这些算点击坐标）---- */
#define HDR_H         14
#define ROW_H         12
#define ROW0_Y        14
#define ROWS          3
#define BTN_Y         48
#define BTN_H         12
#define BTN_W         60
#define BTN_INSTALL_X 4
#define BTN_REMOVE_X  68
#define BTN_LAUNCH_X  132

/* ---- 调色板（XRGB8888，alpha 一律 0xFF = 不覆盖混合）---- */
#define CO_BG      0xFF14181Fu
#define CO_HDR     0xFF23303Fu
#define CO_ROW     0xFF1B2028u
#define CO_ROWSEL  0xFF2E4A6Cu
#define CO_TEXT    0xFFE8ECF2u
#define CO_TEXT_D  0xFF9AA6B8u
#define CO_OK      0xFF3FB55Fu
#define CO_BAD     0xFFC0453Fu
#define CO_BTN     0xFF2C3644u
#define CO_BTN_B   0xFF55617Au
#define CO_CURS    0xFFFFD24Au

static uint32_t*      g_px;
static struct F64Canvas g_cv;
static struct Font64* g_font;
static int g_surf = -1, g_shm = -1;
static int g_rows_sel = 0;
static int g_page = 0;
static int g_status = 0;              /* 0 空闲 / 1 刚装完 / 2 出错 */
static char g_status_msg[48];

/* ★ 仓库/已装库两张表**不放在本进程的静态区**：本仓库一律 -fno-zero-initialized-in-bss，
 *   全零静态数组会落进 .data（文件里），两张表 ~32 KiB 会把 64 KiB 的主程序装载区顶爆。
 *   表本体由 vs_pkg.c 的引擎缓存（mmap 区）持有，这里只要指针。 */
static const struct VsPkg*       g_repo;
static const struct VsInstalled* g_inst;
static int g_nrepo, g_ninst;

static void refresh_tables(void) {
    g_repo = vs_repo_table(&g_nrepo);
    g_inst = vs_inst_table(&g_ninst);
    if (!g_repo) g_repo = (const struct VsPkg*)0;
    if (!g_inst) g_inst = (const struct VsInstalled*)0;
}

static unsigned px(unsigned rgb) { return 0xFF000000u | (rgb & 0xFFFFFFu); }

static void fill_rect(int x, int y, int w, int h, unsigned rgb) {
    const unsigned c = px(rgb);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SW) w = SW - x;
    if (y + h > SH) h = SH - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        uint32_t* row = g_px + (unsigned)yy * SW;
        for (int xx = x; xx < x + w; xx++) row[xx] = c;
    }
}

static void frame_rect(int x, int y, int w, int h, unsigned rgb) {
    fill_rect(x, y, w, 1, rgb);
    fill_rect(x, y + h - 1, w, 1, rgb);
    fill_rect(x, y, 1, h, rgb);
    fill_rect(x + w - 1, y, 1, h, rgb);
}

static void itoa8(int v, char* out) {
    char t[12];
    int k = 0, n = 0;
    unsigned x = (v < 0) ? (unsigned)(-v) : (unsigned)v;
    if (v < 0) out[n++] = '-';
    if (x == 0) t[k++] = '0';
    while (x) { t[k++] = (char)('0' + (int)(x % 10u)); x /= 10u; }
    while (k) out[n++] = t[--k];
    out[n] = 0;
}

static void text(int x, int y, const char* s, int size, unsigned rgb) {
    if (g_font) (void)font_draw_text(g_font, &g_cv, x, y, s, size, px(rgb));
}

static int installed_version_of(const char* name) {
    for (int i = 0; i < g_ninst; i++) if (vs_streq(g_inst[i].name, name)) return i;
    return -1;
}

static int total_pages(void) {
    const int n = (g_nrepo + ROWS - 1) / ROWS;
    return (n < 1) ? 1 : n;
}

static void render(int frame) {
    fill_rect(0, 0, SW, SH, CO_BG & 0xFFFFFFu);
    fill_rect(0, 0, SW, HDR_H, CO_HDR & 0xFFFFFFu);
    text(4, 1, "VimtuOS Store", 8, CO_TEXT);
    {
        char buf[32];
        char num[12];
        int o = 0;
        const char* a = "pkg ";
        for (int i = 0; a[i]; i++) buf[o++] = a[i];
        itoa8(g_page + 1, num); for (int i = 0; num[i]; i++) buf[o++] = num[i];
        buf[o++] = '/';
        itoa8(total_pages(), num); for (int i = 0; num[i]; i++) buf[o++] = num[i];
        buf[o] = 0;
        text(150, 1, buf, 8, CO_TEXT_D);
    }
    for (int r = 0; r < ROWS; r++) {
        const int idx = g_page * ROWS + r;
        const int y = ROW0_Y + r * ROW_H;
        const int sel = (idx == g_rows_sel);
        fill_rect(0, y, SW, ROW_H - 1, (sel ? CO_ROWSEL : CO_ROW) & 0xFFFFFFu);
        if (idx >= g_nrepo) continue;
        const struct VsPkg* p = &g_repo[idx];
        const int ins = installed_version_of(p->name);
        /* 图标：装过 = 绿方块，没装 = 灰方块（像素证据） */
        fill_rect(2, y + 2, 8, 8, ins >= 0 ? (CO_OK & 0xFFFFFFu) : (CO_BTN_B & 0xFFFFFFu));
        text(14, y + 1, p->name, 8, CO_TEXT);
        text(96, y + 1, p->version, 8, CO_TEXT_D);
        {
            char nb[24], num[12];
            int o = 0;
            itoa8(p->size / 1024, num);
            for (int i = 0; num[i]; i++) nb[o++] = num[i];
            nb[o++] = 'k'; nb[o] = 0;
            text(140, y + 1, nb, 8, CO_TEXT_D);
        }
        text(180, y + 1, ins >= 0 ? "installed" : "available", 8,
             ins >= 0 ? (CO_OK & 0xFFFFFFu) : (CO_TEXT_D & 0xFFFFFFu));
    }
    /* 按钮条 */
    const int by = BTN_Y;
    fill_rect(0, by - 2, SW, SH - by + 2, CO_BG & 0xFFFFFFu);
    fill_rect(BTN_INSTALL_X, by, BTN_W, BTN_H, CO_BTN & 0xFFFFFFu);
    frame_rect(BTN_INSTALL_X, by, BTN_W, BTN_H, CO_BTN_B);
    text(BTN_INSTALL_X + 6, by + 2, "Install", 8, CO_TEXT);
    fill_rect(BTN_REMOVE_X, by, 54, BTN_H, CO_BTN & 0xFFFFFFu);
    frame_rect(BTN_REMOVE_X, by, 54, BTN_H, CO_BTN_B);
    text(BTN_REMOVE_X + 6, by + 2, "Remove", 8, CO_TEXT);
    fill_rect(BTN_LAUNCH_X, by, 58, BTN_H, CO_BTN & 0xFFFFFFu);
    frame_rect(BTN_LAUNCH_X, by, 58, BTN_H, CO_BTN_B);
    text(BTN_LAUNCH_X + 6, by + 2, "Launch", 8, CO_TEXT);
    /* 状态 + 进度条（装完=全绿，出错=红一小段，空闲=暗） */
    {
        const int bx = 196, bw = 56;
        fill_rect(bx, by + 3, bw, 6, CO_BTN & 0xFFFFFFu);
        if (g_status == 1) fill_rect(bx, by + 3, bw, 6, CO_OK & 0xFFFFFFu);
        else if (g_status == 2) fill_rect(bx, by + 3, bw / 3, 6, CO_BAD & 0xFFFFFFu);
        frame_rect(bx, by + 3, bw, 6, CO_BTN_B);
        text(bx, by + 10, g_status_msg[0] ? g_status_msg : "-", 8, CO_TEXT_D);
    }
    (void)frame;
}

static void submit(void) {
    (void)wl_surface_attach(g_surf, g_shm, 0);
    (void)wl_surface_damage(g_surf, 0, 0, SW, SH);
    (void)wl_surface_commit(g_surf);
    (void)wl_display_dispatch(0, 0, 0);
}

static int hit_button(int sx, int sy) {
    if (sy < BTN_Y || sy >= BTN_Y + BTN_H) return 0;
    if (sx >= BTN_INSTALL_X && sx < BTN_INSTALL_X + BTN_W) return 1;
    if (sx >= BTN_REMOVE_X && sx < BTN_REMOVE_X + 54) return 2;
    if (sx >= BTN_LAUNCH_X && sx < BTN_LAUNCH_X + 58) return 3;
    return 0;
}

static const char* sel_name(void) {
    if (g_rows_sel < 0 || g_rows_sel >= g_nrepo) return "";
    return g_repo[g_rows_sel].name;
}

/* ==================== 启动助手（★ 为什么"提前 fork"） ====================
 * 内核 proc64 的 fork 是**整页物理复制**，上限 PROC64_FORK_MAX_PAGES = 256 页（≈1 MiB），
 * 超了直接 -ENOMEM（kernel/proc64.cpp:58）。而 user/lib/font64 的光栅临时区就是 **1 MiB**，
 * 本进程加载字体之后再 fork 必然撞上限（实测：[STORE] launch ... pid=-1，程序没起来）。
 * 改内核？不改。做法：**在加载字体之前**就 fork 一个只做 execve 的小助手（那时本进程只有
 * ELF + 栈 ≈ 几十页），之后用既有的 pipe(22) 把"要启动的路径"写给助手，助手 execve 过去
 * —— 助手的 pid 就变成了那个程序的 pid（execve 保留 oid）。助手不在时退回自己 fork（如实）。 */
static int g_launch_w = -1;
static int g_launch_pid = -1;
/* 助手自己的兜底存活时间：本进程退出后它靠管道 EOF（read 返回 0）收工 */
static int start_launcher(void) {
    int fds[2];
    if (__v64_syscall(22 /*pipe*/, (long)(unsigned long)fds, 0, 0, 0, 0) != 0) return -1;
    const int pid = fork();
    if (pid == 0) {
        char buf[64];
        for (;;) {
            int n = 0;
            while (n < (int)sizeof(buf) - 1) {
                /* ★ 走 Linux 兼容号段（V64_LX_READ=0）：本 libc 的 read()/write() 是**自有 ABI**
                 *   （控制台专用），对管道 fd 会回 -1 —— 实测踩到过（rc=-1）。 */
                const long r = __v64_syscall(V64_LX_READ, fds[0],
                                             (long)(unsigned long)(buf + n),
                                             (long)((int)sizeof(buf) - 1 - n), 0, 0);
                if (r <= 0) break;
                n += (int)r;
            }
            if (n <= 0) continue;
            buf[n] = 0;
            for (int i = 0; i < n; i++) if (buf[i] == '\n') buf[i] = 0;
            char* av[2];
            char* env[1];
            av[0] = buf;
            av[1] = 0;
            env[0] = 0;
            (void)__v64_syscall(59 /*execve*/, (long)(unsigned long)buf, (long)(unsigned long)av,
                                (long)(unsigned long)env, 0, 0);
            /* 装载失败：如实在串口说一声（不假装启动成功），然后接着等下一次请求 */
            printf("[STORE] launcher exec FAILED path=%s\n", buf);
            vimtu64_stdout_flush64();
        }
    }
    if (pid < 0) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        return -1;
    }
    (void)close(fds[0]);
    g_launch_w = fds[1];
    g_launch_pid = pid;
    vs_log(VS_TAG_GUI, "launcher pid=%d wfd=%d\n", pid, g_launch_w);
    return pid;
}

/* .deb 交给 `/bin/vpkg install <name>` 子进程（**同一份引擎**，CLI 版才有 ar/tar/gzip；
 * 本 GUI 二进制的装载区要留给 stb_truetype）。等它有界（与 shell 的 run 同款 5 s 窗口续等）。 */
static int install_deb_via_cli(const char* name) {
    const char* vp = "/bin/vpkg";
    if (!vs_exists(vp)) {
        vs_log(VS_TAG_GUI, "delegate missing path=%s\n", vp);
        vs_strcpy(g_status_msg, 48, "no /bin/vpkg");
        g_status = 2;
        return 1;
    }
    const int pid = fork();
    if (pid == 0) {
        char* av[4];
        char* env[1];
        av[0] = (char*)vp;
        av[1] = (char*)"install";
        av[2] = (char*)name;
        av[3] = 0;
        env[0] = 0;
        (void)__v64_syscall(59, (long)(unsigned long)vp, (long)(unsigned long)av,
                            (long)(unsigned long)env, 0, 0);
        _exit(127);
    }
    vs_log(VS_TAG_GUI, "delegate cmd=install name=%s pid=%d\n", name, pid);
    if (pid <= 0) {
        vs_strcpy(g_status_msg, 48, "fork failed");
        g_status = 2;
        return 1;
    }
    int st = 0;
    int r = wait4(pid, &st, 0, 0);
    for (int guard = 0; r == -11 && guard < 60; guard++) r = wait4(pid, &st, 0, 0);
    const int code = (r > 0) ? ((st >> 8) & 0xFF) : -1;
    vs_log(VS_TAG_GUI, "delegate done name=%s pid=%d code=%d\n", name, r, code);
    vs_strcpy(g_status_msg, 48, (code == 0) ? "installed ok" : "install failed");
    g_status = (code == 0) ? 1 : 2;
    return code;
}

static int do_install(void) {
    const char* name = sel_name();
    if (!name[0]) { vs_strcpy(g_status_msg, 48, "no package"); g_status = 2; return 1; }
    vs_log(VS_TAG_GUI, "click install name=%s\n", name);
    /* 类型看索引：deb 走 /bin/vpkg 子进程（本二进制没有 inflate/ar/tar），.vap64 在本进程里装 */
    int is_deb = 0;
    for (int i = 0; i < g_nrepo; i++)
        if (vs_streq(g_repo[i].name, name) && vs_streq(g_repo[i].type, "deb")) is_deb = 1;
    const int rc = is_deb ? install_deb_via_cli(name) : vs_install(VS_TAG_GUI, name, 1);
    if (rc == VS_OK) {
        vs_strcpy(g_status_msg, 48, "installed ok");
        g_status = 1;
    } else {
        char b[48];
        int o = 0;
        const char* e = vs_err_name(rc);
        for (int i = 0; e[i] && o < 30; i++) b[o++] = e[i];
        b[o++] = ' '; b[o++] = '!'; b[o++] = '!'; b[o] = 0;
        vs_strcpy(g_status_msg, 48, b);
        g_status = 2;
    }
    refresh_tables();                                            /* 引擎已清缓存：重读 */
    return rc;
}

static int do_remove(void) {
    const char* name = sel_name();
    if (!name[0]) { vs_strcpy(g_status_msg, 48, "no package"); g_status = 2; return 1; }
    vs_log(VS_TAG_GUI, "click remove name=%s\n", name);
    const int rc = vs_remove(VS_TAG_GUI, name);
    if (rc == VS_OK) { vs_strcpy(g_status_msg, 48, "removed ok"); g_status = 1; }
    else             { vs_strcpy(g_status_msg, 48, "not installed"); g_status = 2; }
    refresh_tables();
    return rc;
}

/* 启动 = 把路径写给助手（助手 execve）；先看落盘清单里有没有 /bin/<name> */
static void launch_clicked_pkg(void) {
    const char* name = sel_name();
    char path[VS_FILE_MAX];
    vs_fmt(path, (int)sizeof(path), "%s/%s", VS_BIN_DIR, name);
    int have = 0;
    const int iv = installed_version_of(name);
    if (iv >= 0) {
        for (int f = 0; f < g_inst[iv].nfiles; f++)
            if (vs_streq(g_inst[iv].files[f], path)) have = 1;
    }
    if (!have) {
        vs_log(VS_TAG_GUI, "launch name=%s path=%s state=not-installed\n", name, path);
        vs_strcpy(g_status_msg, 48, "not installed");
        g_status = 2;
        return;
    }
    if (g_launch_w >= 0) {                       /* 正常路径：写给提前 fork 的助手 */
        char msg[64];
        vs_fmt(msg, (int)sizeof(msg), "%s\n", path);
        const long w = __v64_syscall(V64_LX_WRITE, g_launch_w, (long)(unsigned long)msg,
                                     (long)vs_strlen(msg), 0, 0);
        vs_log(VS_TAG_GUI, "launch name=%s path=%s pid=%d via=pipe rc=%d\n", name, path,
               g_launch_pid, (int)w);
        if (w > 0) { vs_strcpy(g_status_msg, 48, "launched ok"); g_status = 1; }
        else       { vs_strcpy(g_status_msg, 48, "pipe failed"); g_status = 2; }
        return;
    }
    const int pid = fork();                      /* 兜底：没有助手就自己 fork（可能撞 256 页上限） */
    if (pid == 0) {
        char* av[2];
        char* env[1];
        av[0] = path;
        av[1] = 0;
        env[0] = 0;
        (void)__v64_syscall(59 /*execve*/, (long)(unsigned long)path, (long)(unsigned long)av,
                            (long)(unsigned long)env, 0, 0);
        _exit(127);
    }
    vs_log(VS_TAG_GUI, "launch name=%s path=%s pid=%d via=fork\n", name, path, pid);
    if (pid > 0) { vs_strcpy(g_status_msg, 48, "launched ok"); g_status = 1; }
    else         { vs_strcpy(g_status_msg, 48, "fork failed"); g_status = 2; }
}

int main(void) {
    printf("[STORE] ver=1 pid=%d\n", getpid());

    /* ---- ① 仓库 / 已装库（读一次，后面用缓存）---- */
    refresh_tables();
    printf("[STORE] repo packages=%d installed=%d page=1/%d\n", g_nrepo, g_ninst, total_pages());

    /* ---- ② 合成器：先看哪个路径在（/lib/wm.elf 优先；内核只认 /bin/wm.elf）---- */
    const char* wm = VS_WM_PATH;
    if (!vs_exists(wm)) {
        if (vs_exists(VS_WM_ALT_PATH)) wm = VS_WM_ALT_PATH;
        else {
            printf("[STORE] wm missing (want %s or %s) - GUI cannot composite\n", VS_WM_PATH, VS_WM_ALT_PATH);
            printf("[STORE] done frames=0 reason=error\n");
            (void)fflush(0);
            return 1;
        }
    }
    const int wmpid = fork();
    if (wmpid == 0) {
        char* av[2];
        char* env[1];
        av[0] = (char*)wm;
        av[1] = 0;
        env[0] = 0;
        (void)__v64_syscall(59, (long)(unsigned long)wm, (long)(unsigned long)av,
                            (long)(unsigned long)env, 0, 0);
        _exit(127);
    }
    printf("[STORE] wm path=%s pid=%d\n", wm, wmpid);

    /* ---- ②.5 启动助手：必须在 font64 的 1 MiB 光栅区**之前** fork（理由见 start_launcher）---- */
    (void)start_launcher();

    vimtu64_sleep_ms(2500);            /* 合成器要映射 33 MB 后备缓冲 + 标定 rdtsc（SDK 模板同款）*/

    /* ---- ③ 字体（从系统卷读；没有就只画色块，如实报 ok=0）---- */
    g_font = font_load("/Fonts/NotoSans-Regular.ttf");
    {
        struct F64FaceInfo fi;
        int ok = 0, bytes = 0;
        if (g_font && font64_face_info(g_font, 0, &fi) == 0) { ok = fi.used; bytes = (int)fi.bytes; }
        printf("[STORE] fonts face=%s bytes=%d ok=%d\n", "/Fonts/NotoSans-Regular.ttf", bytes, ok);
        if (g_font) (void)font64_set_spacing(g_font, F64_SPACING_GRID12);
    }

    /* ---- ④ 画布 + surface ---- */
    g_shm = shm_create(SW_BYTES);
    g_surf = wl_surface_create(SW, SH, V64_WL_FORMAT_XRGB8888);
    void* va = 0;
    const int mrc = shm_map(g_shm, 0, SW_BYTES, &va);
    printf("[STORE] surf id=%d w=%d h=%d shm=%d map=%d\n", g_surf, SW, SH, g_shm, mrc);
    if (g_surf <= 0 || g_shm <= 0 || mrc != 0 || !va) {
        printf("[STORE] FAILED surface/shm rc=%d/%d/%d\n", g_surf, g_shm, mrc);
        printf("[STORE] done frames=0 reason=error\n");
        (void)fflush(0);
        return 1;
    }
    g_px = (uint32_t*)va;
    g_cv.px = g_px;
    g_cv.w = SW;
    g_cv.h = SH;

    /* ---- ⑤ 帧循环 ---- */
    int frame = 0, reason = 0;
    int pending_click = 0, click_x = 0, click_y = 0;
    for (; frame < MAX_FRAMES; frame++) {
        render(frame);
        submit();
        if (frame == 0) {
            const int r0 = (g_nrepo > 0) ? 0 : -1;
            printf("[STORE] render f=%d sel=%d row0=%s row1=%s row2=%s state0=%s\n", frame, g_rows_sel,
                   (r0 >= 0) ? g_repo[0].name : "-",
                   (g_nrepo > 1) ? g_repo[1].name : "-",
                   (g_nrepo > 2) ? g_repo[2].name : "-",
                   (r0 >= 0) ? ((installed_version_of(g_repo[0].name) >= 0) ? "installed" : "available") : "-");
        }
        struct Wl64SeatEvent ev[EV_MAX];
        const int got = wl_display_dispatch(FRAME_MS, ev, EV_MAX);
        for (int i = 0; i < got; i++) {
            const struct Ev64Event* e = &ev[i].ev;
            if (e->type == V64_EV_MOUSE_MOVE && ev[i].inside && ev[i].sx >= 0) {
                printf("[STORE] mouse sx=%d sy=%d x=%d y=%d\n", ev[i].sx, ev[i].sy, e->x, e->y);
                /* 指针画在窗口里（像素证据：光标处有亮黄小十字） */
                render(frame);
                {
                    const int cx = ev[i].sx, cy = ev[i].sy;
                    if (cx >= 2 && cy >= 2 && cx < SW - 3 && cy < SH - 3) {
                        for (int k = -2; k <= 2; k++) {
                            fill_rect(cx + k, cy - 1, 1, 3, CO_CURS);
                            fill_rect(cx - 1, cy + k, 3, 1, CO_CURS);
                        }
                    }
                }
                submit();
            } else if (e->type == V64_EV_MOUSE_DOWN && ev[i].inside && ev[i].sx >= 0) {
                pending_click = 1;
                click_x = ev[i].sx;
                click_y = ev[i].sy;
                printf("[STORE] click sx=%d sy=%d btn=%d inside=1\n", click_x, click_y, e->code);
            } else if (e->type == V64_EV_KEY_DOWN && e->code == V64_KEY_ESC) {
                printf("[STORE] key esc -> quit\n");
                reason = 1;
            }
        }
        if (reason) break;

        if (pending_click) {
            pending_click = 0;
            const int btn = hit_button(click_x, click_y);
            const char* nm = sel_name();
            if (btn == 0 && click_y >= ROW0_Y && click_y < ROW0_Y + ROWS * ROW_H) {
                const int r = (click_y - ROW0_Y) / ROW_H;
                const int idx = g_page * ROWS + r;
                if (idx < g_nrepo) g_rows_sel = idx;
                printf("[STORE] click sx=%d sy=%d btn=row hit=row sel=%d name=%s\n",
                       click_x, click_y, g_rows_sel, sel_name());
            } else {
                printf("[STORE] click sx=%d sy=%d btn=button hit=%s sel=%d name=%s\n",
                       click_x, click_y, (btn == 1) ? "install" : ((btn == 2) ? "remove" :
                       ((btn == 3) ? "launch" : "none")), g_rows_sel, nm);
                if (btn == 1)      (void)do_install();
                else if (btn == 2) (void)do_remove();
                else if (btn == 3) launch_clicked_pkg();
            }
            render(frame);
            submit();
        }
    }
    if (!reason) reason = (frame >= MAX_FRAMES) ? 2 : 0;
    printf("[STORE] done frames=%d reason=%s\n", frame,
           (reason == 1) ? "esc" : ((reason == 2) ? "timeout" : "error"));
    (void)wl_surface_destroy(g_surf);
    (void)fflush(0);
    return 0;
}
