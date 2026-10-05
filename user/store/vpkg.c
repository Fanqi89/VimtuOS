/* vpkg.c - ★ /bin/vpkg：VimtuOS 包管理器的**命令行**（纯 ring3）
 *
 * 用法（终端里：先 `shell` 进 ring3 外壳，再 `run /bin/vpkg <cmd> …`）：
 *   vpkg list                     列仓库（含"装了没"）与本地已装
 *   vpkg info <name>              单个包的索引信息 + 安装状态 + 落盘文件
 *   vpkg install <name|file>      装（名字 = 查 /opt/vpkg/index.json；含 '/' 或 '.' = 直接给文件）
 *   vpkg remove <name>            卸（按 /var/lib/vpkg/installed.json 的 files[] 删）
 *   vpkg search <kw>              名字/摘要/描述里不区分大小写地找
 *   vpkg update                   按索引版本比对已装包，列出可升级项
 *
 * 退出码：0 = 成功；1..13 = 具体的包错误（**逐类分得开**，见 vs.h 的 VS_E_*）：
 *   1 path（路径/名字非法） 2 hash（sha256 不符） 3 size 4 depends（缺依赖） 5 space（空间不足）
 *   6 installed（已安装） 7 not-installed 8 format（容器坏） 9 scripts（deb 带维护者脚本）
 *   10 xz（deb 用 xz） 11 notfound 12 io 13 nomem
 *   （内核/外壳那边另有 127 = 装载失败：那是"程序没跑起来"，与本文件的码不重叠。）
 *
 * 打点（tests/vpkg64_test.py 按这些串 grep；格式勿改）：
 *   [VPKG] ver=1 cmd=<c> argc=<n>
 *   [VPKG] repo path=… index=… packages=… installed=…
 *   [VPKG] pkg name=… version=… arch=… type=… size=… state=installed|not-installed depends=… summary="…"
 *   [VPKG] install begin … / sha256 … / progress pct=… / install file … / db write … / install ok …
 *   [VPKG] remove file … / remove ok …
 *   [VPKG] ERROR code=<名字> msg="…"
 *   [VPKG] done cmd=<c> rc=<n>（rc = 退出码）
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "vimtu64.h"
#include "vs.h"

static void usage(void) {
    printf("[VPKG] usage: vpkg list | info <name> | install <name|file.vap64|file.deb> | "
           "remove <name> | search <kw> | update\n");
}

int main(int argc, char** argv) {
    const char* cmd = (argc > 1) ? argv[1] : "help";
    printf("[VPKG] ver=1 cmd=%s argc=%d\n", cmd, argc);

    int rc = VS_OK;
    if (vs_streq(cmd, "list")) {
        rc = vs_list(VS_TAG_CLI);
    } else if (vs_streq(cmd, "info")) {
        if (argc < 3) { usage(); rc = VS_E_PATH; }
        else rc = vs_info(VS_TAG_CLI, argv[2]);
    } else if (vs_streq(cmd, "install")) {
        if (argc < 3) { usage(); rc = VS_E_PATH; }
        else rc = vs_install(VS_TAG_CLI, argv[2], 1);
    } else if (vs_streq(cmd, "remove")) {
        if (argc < 3) { usage(); rc = VS_E_PATH; }
        else rc = vs_remove(VS_TAG_CLI, argv[2]);
    } else if (vs_streq(cmd, "search")) {
        if (argc < 3) { usage(); rc = VS_E_PATH; }
        else rc = vs_search(VS_TAG_CLI, argv[2]);
    } else if (vs_streq(cmd, "update")) {
        rc = vs_update(VS_TAG_CLI);
    } else {
        usage();
        rc = VS_E_PATH;
    }
    printf("[VPKG] done cmd=%s rc=%d\n", cmd, rc);
    (void)fflush(0);
    return rc;
}
