/* vpkg.c - ★ /bin/vpkg：VimtuOS 包管理器的**命令行**（纯 ring3）
 *            ★★ v0.9.42（L1）：**同时是 dpkg / apt 的前端** —— 全系统只有这一份 .deb 引擎。
 *
 * ============================ 用法 ============================
 * 终端里：先 `shell` 进 ring3 外壳，再 `run /bin/vpkg <cmd> …`
 *   vpkg list                     列仓库（含"装了没"）与本地已装
 *   vpkg info <name>              单个包的索引信息 + 安装状态 + 落盘文件
 *   vpkg install <name|file>      装（名字 = 查 /opt/vpkg/index.json；含 '/' 或 '.' = 直接给文件）
 *   vpkg install <name> --force-depends
 *                                 缺依赖时**显式**跳过依赖检查（逐条打 state=forced，绝不静默）
 *   vpkg remove <name>            卸（按 /var/lib/vpkg/installed.json 的 files[] 删）
 *   vpkg search <kw>              名字/摘要/描述里不区分大小写地找
 *   vpkg update                   按索引版本比对已装包，列出可升级项
 *
 * ★ v0.9.42（L1）新增：**同一个二进制，按 argv[0] 的 basename 决定前端**（busybox 模式）。
 *   造卷时把 /bin/vpkg 这**一份字节**同时写成 /usr/bin/dpkg 与 /usr/bin/apt
 *   （见 tools/dpkg_pack_win.py），于是：
 *     /usr/bin/dpkg --version | -V        打 dpkg/apt 两行版本（与旧 mini-dpkg 逐字节一致）
 *     /usr/bin/dpkg -i <file.deb>         安装（等价 vpkg install，走**同一个** install_deb）
 *     /usr/bin/dpkg -l | --list           列已装（+ 仓库）
 *     /usr/bin/dpkg -r <name> | --remove  卸载（真按 files[] 删，旧 mini-dpkg 只删记录）
 *     /usr/bin/dpkg -s <name> | --status  单包信息（= vpkg info）
 *     /usr/bin/apt install ./X.deb        安装；`apt install NAME` 如实报"远程仓库未启用"
 *     /usr/bin/apt update                 如实报"远程仓库未启用"（代码 2，不假装成功）
 *     /usr/bin/apt list | remove | purge  对应 list / remove
 *   为什么这样做（L1 的目标就是"只留一份 .deb 引擎"）：v0.7.5 的 user/dpkg/dpkg.c 是
 *   另一套独立实现的 ar + control.tar.gz + data.tar.gz 解析（只认 gzip、靠外部 tar 解包、
 *   不处理依赖、卸载只删记录不删文件）。两套引擎并存 = 两份 bug 面，而且 dpkg 那套连
 *   Debian 原样的 control.tar.xz 都装不了。合并后 **ar/tar/gzip/xz/依赖/文件清单** 全部只有
 *   user/store/vs_*.c 一份；dpkg/apt 只剩**参数翻译 + 输出文案**，本身不含任何包格式代码。
 *   user/dpkg/dpkg.c 已标注废弃、不再编译、不进卷。
 *
 * 退出码：0 = 成功；1..14 = 具体的包错误（**逐类分得开**，见 vs.h 的 VS_E_*）：
 *   1 path（路径/名字非法） 2 hash（sha256 不符） 3 size 4 depends（缺依赖） 5 space（空间不足）
 *   6 installed（已安装） 7 not-installed 8 format（容器坏） 9 scripts（deb 带维护者脚本）
 *   10 xz（xz 流解不开：多块/BCJ/校验等不支持的特性） 11 notfound 12 io 13 nomem 14 perm
 *   （内核/外壳那边另有 127 = 装载失败：那是"程序没跑起来"，与本文件的码不重叠。
 *     apt 的"远程仓库未启用"沿用旧 mini-apt 的 2，那是它自己的前端码，不是 VS_E_HASH。）
 *
 * 打点（tests/vpkg64_test.py 与 tests/linuxapp64_test.py 按这些串 grep；格式勿改）：
 *   [VPKG] ver=1 cmd=<c> argc=<n> force-depends=<0|1>
 *   [VPKG] repo path=… index=… packages=… installed=…
 *   [VPKG] pkg name=… version=… arch=… type=… size=… state=installed|not-installed depends=… summary="…"
 *   [VPKG] provides load path=… entries=… / depends name=… on=… state=… via=…
 *   [VPKG] install begin … / sha256 … / progress pct=… / install file … / db write … / install ok …
 *   [VPKG] remove file … / remove ok …
 *   [VPKG] ERROR code=<名字> msg="…"
 *   [VPKG] done cmd=<c> rc=<n>（rc = 退出码）
 *   ★ 只有 dpkg/apt 前端**多打一行**（vpkg 模式的上面这些行逐字节不变）：
 *   [VPKG] frontend=<dpkg|apt> argv0=<用户给的路径> cmd=<c> argc=<n>
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "vimtu64.h"
#include "vs.h"

/* argv[0] 的 basename（不引 string.h 的 strrchr：只要 4 行，省一次符号解析） */
static const char* s_base(const char* p) {
    const char* b = p;
    if (!p) return "";
    for (const char* q = p; *q; q++) if (*q == '/') b = q + 1;
    return b;
}

static void usage(void) {
    printf("[VPKG] usage: vpkg list | info <name> | install <name|file.vap64|file.deb> [--force-depends] | "
           "remove <name> | search <kw> | update\n");
}

int main(int argc, char** argv) {
    const char* argv0 = (argc > 0 && argv[0]) ? argv[0] : "vpkg";
    const char* a0 = s_base(argv0);
    const int fe_dpkg = vs_streq(a0, "dpkg");
    const int fe_apt  = vs_streq(a0, "apt");
    /* 前端专属 tag：引擎日志因此能分清"是 vpkg 还是 dpkg 发起的"，而对 vpkg 那条路零影响。 */
    const char* tag = fe_dpkg ? "DPKG" : (fe_apt ? "APT" : VS_TAG_CLI);

    /* ---- --force-depends：显式开关（放在命令前面后面都认）----
     * 只对本条命令有效；把 argv 里那一项摘掉，剩下的按老规矩分派（argc 相应减一）。 */
    int force_depends = 0;
    char* av[24];
    int ac = 0;
    for (int i = 0; i < argc && ac < (int)(sizeof(av) / sizeof(av[0])); i++) {
        if (vs_streq(argv[i], "--force-depends")) { force_depends = 1; continue; }
        av[ac++] = argv[i];
    }
    argv = av;
    argc = ac;

    const char* cmd = (argc > 1) ? argv[1] : "help";
    const char* arg = (argc > 2) ? argv[2] : (const char*)0;

    /* ---- --version / -V：前端专属（文案与旧 mini-dpkg 逐字节一致，tests/dpkg64_test.py 的 A 条钉着它）---- */
    if ((fe_dpkg || fe_apt) && argc >= 2 && (vs_streq(cmd, "--version") || vs_streq(cmd, "-V"))) {
        printf("dpkg 0.7.5 (vimtu-dpkg; .deb = ar + control.tar.gz + data.tar.gz)\n");
        printf("apt  0.7.5 (vimtu-apt; install ./X.deb 走 dpkg，远程仓库未启用)\n");
        return 0;
    }

    /* ---- 前端参数 → 内部 cmd 的翻译（**只有翻译，没有任何包格式代码**）---- */
    if (fe_dpkg) {
        if (vs_streq(cmd, "-i") || vs_streq(cmd, "--install"))            cmd = "install";
        else if (vs_streq(cmd, "-l") || vs_streq(cmd, "--list"))          cmd = "list";
        else if (vs_streq(cmd, "-r") || vs_streq(cmd, "--remove") ||
                 vs_streq(cmd, "-P") || vs_streq(cmd, "--purge"))        cmd = "remove";
        else if (vs_streq(cmd, "-s") || vs_streq(cmd, "--status") ||
                 vs_streq(cmd, "-L") || vs_streq(cmd, "--listfiles"))    cmd = "info";
    } else if (fe_apt) {
        if (vs_streq(cmd, "install")) {
            if (!arg || !arg[0]) { printf("apt: install 需要 .deb 路径\n"); return 1; }
            if (vs_find(arg, ".deb") < 0) {
                printf("apt: 远程仓库安装（apt install NAME）需要 HTTP + 仓库索引 —— 当前未启用\n");
                printf("apt: 本地包请用：apt install ./包名.deb\n");
                return 2;
            }
            cmd = "install";
        } else if (vs_streq(cmd, "update")) {
            printf("apt: update 需要访问远程仓库（HTTP + Release/InRelease 索引）—— 当前未启用\n");
            printf("apt: 已如实返回，不假装成功\n");
            return 2;
        } else if (vs_streq(cmd, "list")) {
            cmd = "list";                                  /* apt list --installed 也走这里 */
        } else if (vs_streq(cmd, "remove") || vs_streq(cmd, "purge")) {
            cmd = "remove";
        }
    }

    if (fe_dpkg || fe_apt)
        printf("[VPKG] frontend=%s argv0=%s cmd=%s argc=%d\n", a0, argv0, cmd, argc);
    /* ★ 这一行 vpkg 模式**逐字节不变**（vpkg64_test / linuxapp64_test 按它 grep）；只在末尾多打 fe 时
     *   会破坏契约，所以前端信息放上面那一行，这里保持原样。 */
    printf("[VPKG] ver=1 cmd=%s argc=%d force-depends=%d\n", cmd, argc, force_depends);

    int rc = VS_OK;
    if (vs_streq(cmd, "list")) {
        rc = vs_list(tag);
    } else if (vs_streq(cmd, "info")) {
        if (!arg) { usage(); rc = VS_E_PATH; }
        else rc = vs_info(tag, arg);
    } else if (vs_streq(cmd, "install")) {
        if (!arg) { usage(); rc = VS_E_PATH; }
        else rc = vs_install(tag, arg, 1, force_depends);
    } else if (vs_streq(cmd, "remove")) {
        if (!arg) { usage(); rc = VS_E_PATH; }
        else rc = vs_remove(tag, arg);
    } else if (vs_streq(cmd, "search")) {
        if (!arg) { usage(); rc = VS_E_PATH; }
        else rc = vs_search(tag, arg);
    } else if (vs_streq(cmd, "update")) {
        rc = vs_update(tag);
    } else {
        usage();
        rc = VS_E_PATH;
    }
    printf("[VPKG] done cmd=%s rc=%d\n", cmd, rc);
    (void)fflush(0);
    return rc;
}
