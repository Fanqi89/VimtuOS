/* ============================ FPU/xmm 回归程序链接脚本（VimtuOS ELF64）============================
 * 为什么不用 user/hello_elf64.ld：那个脚本把 .rodata/.data/.bss 各自**页对齐**（便于验证"按段给
 * 权限"），代价是 4KB 一格的空洞。本程序只做 FPU 回归、对段权限没有断言，而它是**内嵌进系统
 * 内核**的字节（objcopy -> .bin，见 build64.sh 结尾的图标包区间断言），4KB 的空洞就是白扔内核
 * 预算：原样（hello_elf64.ld + 符号表）= 9456 B，本脚本 + --strip-all = 约 2.3 KB。
 *
 * 为什么可以贴紧排布（安全性依据）：
 *   * 内核装载器（kernel/elf64.cpp 的 e64_seg_map_union64）**不做 p_offset 与 p_vaddr 的页同余
 *     检查**，也不 mmap 文件：它按页映射、然后逐字节 `dst[b] = p[p_offset + b]` 拷贝；
 *   * 同一页被两个段覆盖时：user64_map_page64 发现页已存在会**复用同一物理页**（只改权限位），
 *     不会重新分配 —— 所以 .text(R|X) 与 .rodata(R)/.bss(RW) 挤在第一页不会丢内容；
 *     权限按页取并集（该页最终 RWX，只影响这条测试程序，不影响任何断言）。
 *
 * `. = 4GiB + SIZEOF_HEADERS`：让 ELF 头 + 程序头表属于第一个 PT_LOAD（auxv 的 AT_PHDR 才有值）。
 * ========================================================================== */
ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS {
    . = 0x0000000100000000 + SIZEOF_HEADERS;

    .text : ALIGN(16) {
        *(.text)
        *(.text.*)
    }

    .rodata : ALIGN(16) {
        *(.rodata)
        *(.rodata.*)
    }

    /* NOBITS：文件里不占字节（p_filesz = 0、p_memsz > 0，正好再走一遍内核的清零路径） */
    .bss : ALIGN(16) {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.interp)
        *(.dynamic)
        *(.dynsym)
        *(.dynstr)
        *(.hash)
        *(.gnu.hash)
        *(.rela*)
        *(.got*)
    }
}
