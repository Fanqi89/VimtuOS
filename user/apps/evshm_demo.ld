/* evshm_demo.ld - ★ A5 前置：/evshm.elf（user/apps/evshm_demo.c）的链接脚本
 *
 * 与 user/shell/shell64.ld 同一条思路（那份是 ring3 shell 用的），差别只在段名收得更省：
 *   * `. = 4GiB + SIZEOF_HEADERS` —— ELF 头/程序头表落在**第一个 PT_LOAD 内**，
 *     kernel/elf64.cpp 才能算出 auxv 的 AT_PHDR；
 *   * `.text.start` 排第一（user/lib/crt0.S 的 _start 必须顶在映像首字节）；
 *   * .l*（.ltext/.lrodata/.ldata/.lbss）是 -mcmodel=large 的专用段名，必须收，
 *     否则数据会落到脚本外面（kernel/usermode64.h 的注释里记过这个坑）；
 *   * 无 PT_INTERP / PT_DYNAMIC / 重定位（内核只做静态装载）。
 *
 * 硬约束（kernel/elf64.cpp 会拒绝越界段）：所有 PT_LOAD 必须落在
 * 4GiB .. USER64_STACK_VA64（= 4GiB+64KiB），无 PT_INTERP/PT_DYNAMIC，文件 <= 96 KiB。
 */
ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS {
    . = 0x0000000100000000 + SIZEOF_HEADERS;      /* 4GiB + 头部：ELF 头/程序头表在首个 LOAD 内 */

    .text : ALIGN(16) {
        *(.text.start)                            /* crt0.S 的 _start 必须第一 */
        *(.text)
        *(.text.*)
        *(.ltext)
        *(.ltext.*)
    }

    .rodata : ALIGN(4096) {
        *(.rodata)
        *(.rodata.*)
        *(.lrodata)
        *(.lrodata.*)
    }

    .data : ALIGN(4096) {
        *(.data)
        *(.data.*)
        *(.ldata)
        *(.ldata.*)
        *(.got)
        *(.got.plt)
    }

    .bss : ALIGN(4096) {
        *(.bss)
        *(.bss.*)
        *(.lbss)
        *(.lbss.*)
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
    }
}
