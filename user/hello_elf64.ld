ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

/* ============================ 用户程序链接脚本（VimtuOS ELF64）============================
 * 为什么必须自己写：
 *   1) 程序必须链接到**用户窗口**里：kernel/usermode64.h 的 USER64_CODE_VA64 = 4GiB，
 *      且整段要低于 USER64_STACK_VA64 = 4GiB+64KiB（elf64.cpp 会拒绝越界的段，打
 *      [ELF64] reject segment va=... reason=outside-user-window / overlaps-user-region）；
 *      默认脚本会把 ELF 头塞进一个 0x200000 起的独立 LOAD 段（在 4GB 以内）-> 必被拒，
 *      所以必须自己排布。
 *   2) 镜像里不能有 PT_INTERP / 动态段 / 重定位（加载器只做静态装载，不做重定位）。
 *
 * `. = 4GiB + SIZEOF_HEADERS` 这一行是关键技巧（GNU ld / ld.lld 通用）：
 *   让第一个段从文件偏移 0（= ELF 头 + 程序头）开始、映到 4GiB，于是**程序头表也在映像里**，
 *   auxv 的 AT_PHDR 才有意义（= 4GiB + 64）。若不加这一项，头/程序头不属于任何 PT_LOAD，
 *   加载器会把 AT_PHDR 如实填成 0。
 *
 * 段权限：.text = R|X、.rodata = R、.bss = R|W（各段页对齐；同页的段由加载器取权限并集）。
 *   .bss 是 NOBITS -> PT_LOAD 里 p_filesz=0 而 p_memsz=0xb8，正好验证加载器的清零路径。
 * ========================================================================== */
SECTIONS {
    . = 0x0000000100000000 + SIZEOF_HEADERS;

    .text : ALIGN(16) {
        *(.text)
        *(.text.*)
    }

    /* ★ 各段之间必须**页对齐**：加载器是按 4KB 页给权限的（PF_R/PF_W/PF_X -> PTE 位），
       两个权限不同的段挤在同一页时只能取并集（elf64.cpp 按页取并集）。 */
    .rodata : ALIGN(4096) {
        *(.rodata)
        *(.rodata.*)
    }

    .data : ALIGN(4096) {
        *(.data)
        *(.data.*)
    }

    .bss : ALIGN(4096) {
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
