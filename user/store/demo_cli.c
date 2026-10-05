/* demo_cli.c - ★ 应用商店里的**示例 CLI 包**（.vap64 的 payload；被 vpkg 装成 /bin/demo-cli）
 *
 * 它证明的是"装进去的程序能跑起来"：`run /bin/demo-cli` 会看到下面这些行。
 * 刻意**只用 user/lib 的 stdio + 目录枚举**（不碰图形），ELF 体积小，正好落在 VAP64 的
 * code 段上限（32768 B）之内 —— 这是 .vap64 能承载它的前提。
 *
 * 打点（tests/vpkg64_test.py 按这些串 grep；格式勿改）：
 *   [DEMO-CLI] ver=1 pid=<n>
 *   [DEMO-CLI] hello from an installed .vap64 package
 *   [DEMO-CLI] cwd=<p>
 *   [DEMO-CLI] dir path=/ entries=<n> dirs=<n> files=<n>
 *   [DEMO-CLI] done rc=0
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "vimtu64.h"

int main(void) {
    printf("[DEMO-CLI] ver=1 pid=%d\n", getpid());
    printf("[DEMO-CLI] hello from an installed .vap64 package\n");

    char cwd[128];
    if (getcwd(cwd, sizeof(cwd))) printf("[DEMO-CLI] cwd=%s\n", cwd);
    else                          printf("[DEMO-CLI] cwd=(failed)\n");

    /* 实时目录枚举（自有 ABI 50/51/52）：证明装在 /bin 里的程序能读卷 */
    int entries = 0, dirs = 0, files = 0;
    const long fd = __v64_int80(50 /*dir_open*/, (long)(unsigned long)"/", 0, 0, 0);
    if (fd >= 0) {
        char buf[64 * 4];
        long got;
        for (;;) {
            got = __v64_int80(51 /*dir_read*/, fd, (long)(unsigned long)buf, (long)sizeof(buf), 0);
            if (got <= 0) break;
            long off = 0;
            while (off < got) {
                const unsigned nl = (unsigned)((unsigned char)buf[off] |
                                               ((unsigned char)buf[off + 1] << 8));
                if (nl == 0 || nl > 31) break;
                const unsigned kind = (unsigned char)buf[off + 2];
                if (kind == 2) dirs++; else files++;
                entries++;
                off += 12 + (long)nl + 1;
                if (entries >= 64) break;
            }
            if (entries >= 64) break;
        }
        (void)__v64_int80(52 /*dir_close*/, fd, 0, 0, 0);
    }
    printf("[DEMO-CLI] dir path=/ entries=%d dirs=%d files=%d\n", entries, dirs, files);
    printf("[DEMO-CLI] done rc=0\n");
    (void)fflush(0);
    return 0;
}
