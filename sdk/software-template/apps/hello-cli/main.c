/* sdk/software-template/apps/hello-cli/main.c - 模板示例①：纯命令行程序（C + user/lib 自研最小 libc）
 *
 * 它证明的闭环（构建/装载/跑起来都不依赖内核改动）：
 *   crt0 的 _start -> main -> printf（走自有 ABI write(1)）-> 目录枚举（自有 ABI 50/51/52，
 *   经 include/vimtu/dir64.c 的薄包装）-> 读卷里的文件（自有 ABI open(6)/read(7)）-> exit(code)。
 *
 * 打点（apps/hello-cli/tests/hello_cli_test.py 按这些串 grep；格式别改）：
 *   [HELLO-CLI] ver=1 pid=<n> argv0=<s>
 *   [HELLO-CLI] hello
 *   [HELLO-CLI] cwd=<p>
 *   [HELLO-CLI] dir open path=<p> ok=<0|1> err=<n>
 *   [HELLO-CLI] ent name=<s> kind=<file|dir> size=<n> mtime=<n>
 *   [HELLO-CLI] dir items=<n> dirs=<n> files=<n>
 *   [HELLO-CLI] conf path=/etc/hello.conf bytes=<n> text="<s>"
 *   [HELLO-CLI] conf missing path=/etc/hello.conf err=<n>
 *   [HELLO-CLI] summary items=<n> dirs=<n> files=<n> conf_bytes=<n> rc=0
 *   [HELLO-CLI] done
 *
 * 为什么一行只有一次 printf：本 libc 的 printf 在换行时 flush（user/lib/stdio.c），
 * 一行拆成多次 printf 会被内核的 [SYSCALL] 追踪行插进中间，验收脚本按行 grep 就不可靠。
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "vimtu64.h"
#include "dir64.h"          /* sdk/software-template/include/vimtu/dir64.h */

#define CONF_PATH "/etc/hello.conf"

static int read_text_file(const char* path, char* buf, int cap, int* err_out) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (err_out) *err_out = -1;
        return -1;
    }
    int total = 0;
    /* 只读 cap-1 字节就够展示；read() 返回 0 = EOF，负 = 错误 */
    for (;;) {
        if (total >= cap - 1) break;
        const ssize_t got = read(fd, buf + total, (size_t)(cap - 1 - total));
        if (got <= 0) break;
        total += (int)got;
    }
    (void)close(fd);
    buf[total] = 0;
    for (int i = 0; i < total; i++) {                 /* 折成一行，日志才好 grep */
        if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t') buf[i] = ' ';
    }
    return total;
}

int main(int argc, char** argv) {
    const char* prog = (argv && argc > 0 && argv[0]) ? argv[0] : "(no-argv)";
    printf("[HELLO-CLI] ver=1 pid=%d argv0=%s\n", getpid(), prog);
    printf("[HELLO-CLI] hello\n");

    char cwd[128];
    if (getcwd(cwd, sizeof(cwd))) printf("[HELLO-CLI] cwd=%s\n", cwd);
    else                          printf("[HELLO-CLI] cwd=(failed)\n");

    /* ---- ① 实时目录枚举（自有 ABI 50/51/52）---- */
    int items = 0, dirs = 0, files = 0;
    {
        int err = 0;
        struct VimtuDir* d = vimtu_dir_open("/", &err);
        printf("[HELLO-CLI] dir open path=/ ok=%d err=%d\n", d ? 1 : 0, d ? 0 : err);
        if (d) {
            struct VimtuDirent e;
            int rc = 0;
            while ((rc = vimtu_dir_read(d, &e)) == 1) {
                if (items < 32) {         /* 只打前 32 条，日志有界 */
                    printf("[HELLO-CLI] ent name=%s kind=%s size=%u mtime=%u\n",
                           e.name, (e.kind == VIMTU_DIR_KIND_DIR) ? "dir" : "file",
                           (unsigned)e.size, (unsigned)e.mtime);
                }
                items++;
                if (e.kind == VIMTU_DIR_KIND_DIR) dirs++; else files++;
            }
            if (rc < 0) printf("[HELLO-CLI] dir read err=%d\n", rc);
            printf("[HELLO-CLI] dir close rc=%d\n", vimtu_dir_close(d));
        }
        printf("[HELLO-CLI] dir items=%d dirs=%d files=%d\n", items, dirs, files);
    }

    /* ---- ② 读卷里的文件（可选：没有就如实说没有）---- */
    int conf_bytes = 0;
    {
        char text[192];
        int err = 0;
        const int n = read_text_file(CONF_PATH, text, (int)sizeof(text), &err);
        if (n >= 0) {
            conf_bytes = n;
            printf("[HELLO-CLI] conf path=%s bytes=%d text=\"%s\"\n", CONF_PATH, n, text);
        } else {
            /* 空夹具卷里没有这个文件是**正常**的：打印明确错误码，绝不假装读到 */
            printf("[HELLO-CLI] conf missing path=%s err=%d\n", CONF_PATH, err);
        }
    }

    printf("[HELLO-CLI] summary items=%d dirs=%d files=%d conf_bytes=%d rc=0\n",
           items, dirs, files, conf_bytes);
    printf("[HELLO-CLI] done\n");
    (void)fflush(0);       /* 退出前把行缓冲推出去（否则最后一行可能丢） */
    return 0;
}
