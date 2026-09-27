/* shell64.ld - ★ A4-1：ring3 shell 的链接脚本（user/shell/build_shell.sh 用）
 *
 * 与其它链接脚本的关系：
 *   * 和 tools/musl_hello64.ld 同一条思路 —— `. = 4GiB + SIZEOF_HEADERS`，
 *     让 ELF 头/程序头表落在**第一个 PT_LOAD 里**（内核 elf64.cpp 才能算出 auxv 的 AT_PHDR）；
 *   * 和 user/lib/user64.ld 的区别：那份是给**平铺 blob**（blob 加载器）用的，连 .bss 的零
 *     都要靠 .image_end 哨兵带进镜像；本程序走的是 **ELF64 装载器**（/bin/shell.bin 从系统卷
 *     读出 -> elf64 装载），.bss 由装载器按 p_memsz 映射，所以**不需要**那个哨兵。
 *   * 段名里的 .l*（.ltext/.lrodata/.ldata/.lbss）是 -mcmodel=large 的专用段名（必须收）。
 *
 * 硬约束（内核 elf64.cpp 会拒绝越界段，user/shell/build_shell.sh 提前断言）：
 *   所有 PT_LOAD 必须落在 4GiB..4GiB+64KiB（USER64_STACK_VA64 之前）；无 PT_INTERP/PT_DYNAMIC。
 */
ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS {
    . = 0x0000000100000000 + SIZEOF_HEADERS;      /* 4GiB + 头部：ELF 头/程序头表在首个 LOAD 内 */

    .text : ALIGN(16) {
        *(.text.start)                            /* crt0.S 的 _start 必须第一（blob 兼容） */
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
        *(.got)                                   /* 静态非 PIE：槽里是链接期填好的绝对地址 */
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
        *(.interp)                                /* 静态：不该有动态链接器 */
        *(.dynamic)
        *(.dynsym)
        *(.dynstr)
        *(.hash)
        *(.gnu.hash)
        *(.rela*)
    }
}
