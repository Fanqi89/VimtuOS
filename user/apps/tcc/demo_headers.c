/* demo_headers.c - ★ A4-2b：走**系统头（musl）**的演示源码（`-c`，带 #include）
 *
 * 与 demo_tiny.c/hello.c 的区别：这一份**真的包含 musl 的系统头**，用来证明
 *   "tcc 的内置搜索路径确实指向系统卷里的 /tcc/include"（构建期由 tools/tcc_pack_win.py
 *   把 musl 的 include/arch/obj 合并成一份装进卷）。包含 unistd.h 会连带 bits/alltypes.h
 *   与 bits/syscall.h 等（几十个头文件），这是"头路径通不通"最直接的证据。
 *
 * 内存提醒（如实标注）：ring3 的 1 MiB 用户窗口里，tcc 的堆只剩 150 KiB 量级，而带系统头的
 *   编译实测需要 460 KB（宿主侧 -DMEM_DEBUG 量出来的），**大概率会 OOM**。
 *   验收脚本因此把它作为"尽力而为"的一项：成功就断言 .o 头字节，失败就如实记录 tcc 的
 *   out-of-memory 原文，而不是假装通过。
 */
#include <unistd.h>

int tcc_headers_probe(void) {
    const char* s = "hdr\n";
    return (int)write(1, s, 4);
}
