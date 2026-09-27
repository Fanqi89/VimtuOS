/* hello.c - ★ A2：第一个用 **C** 写的 Vimtu64 用户程序（ring3、走自有 ABI）
 *
 * 它证明的最小闭环（验收：tests/userlib64_test.py）：
 *   crt0（.text.start 的 _start）-> main(argc, argv) -> puts/printf（我们自己的行缓冲 + write(1)）
 *   -> return -> exit(code) -> 自有 ABI exit(2) -> 内核回到 ring0。
 * 构建/运行：见 user/build_user.sh 与 build64.sh 的"用户程序交叉编译"段；
 *   内核在 os_boot_path 里用 user64_run_capp64() 跑这个 blob（见 kernel/kernel64.cpp）。 */
#include <stdio.h>
#include <unistd.h>

int main(int argc, char** argv) {
    const char* prog = (argv && argc > 0 && argv[0]) ? argv[0] : "(no-argv)";

    puts("hello from Vimtu64 C userland");                  /* puts：字符串 + '\n' */
    printf("[HELLO] argc=%d argv0=%s pid=%d ticks=%lu\n",
           argc, prog, getpid(), vimtu64_ticks());
    return 0;
}
