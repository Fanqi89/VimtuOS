/* hello.c - ★ A4-2b：**tcc 的链接演示**源码（`tcc hello.c -o hello` -> 真能跑的 ELF64）
 *
 * 交付方式：本文件放在系统卷的 /tcc/demo/hello.c，验收脚本在 ring3 里跑
 *     run /bin/tcc /tcc/demo/hello.c -o /tmp/hello
 *     run /tmp/hello
 * 产物是**静态 ELF64**、映像钉在 4GiB 装载区（tcc 的 x86_64-link.c 里 ELF_START_ADDR 已改成
 * 0x100000000，见 third_party/tcc/README.vimtu64-a4b.md 的"打过的补丁"），所以内核的
 * elf64 装载器能直接把它跑起来。
 *
 * 为什么故意不 #include：用户窗口 1 MiB、tcc 的堆只剩 150 KiB 量级（见 README 的内存账）。
 *   写 `extern long write(...)` 一行比包含 <unistd.h>（连带 bits/alltypes.h 等一串）省得多。
 *   系统调用号与 VimtuOS 的 Linux 兼容号段一致：1 = write。
 */
typedef unsigned long size_t;
extern long write(int fd, const void* buf, size_t n);

int main(void) {
    const char* msg = "hello from TinyCC inside VimtuOS ring3\n";
    size_t n = 0;
    while (msg[n]) n++;
    write(1, msg, n);
    return 0;
}
