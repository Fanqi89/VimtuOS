/* tar64.ld - ★ B5：tar（/bin/tar）的链接脚本
 *
 * 与 tools/musl_hello64.ld 同一条路线（它是从那份拷来的，只改了文件名/说明）：
 *   静态 musl、非 PIC、把映像钉在用户窗口 4GiB，且**保留 .got**。
 *   为什么不能丢 .got：musl 的静态目标文件小代码模型对 __fini_array_start
 *   这类符号用 GOTPCRELX：映像在 4GiB 时 32 位 PC 相对放不下，必须真分配
 *   一个 GOT 槽（静态非 PIE 的槽由链接器在链接期填绝对地址，不产生 PT_DYNAMIC）。
 *   详细理由（含实测报错原文）见 tools/musl_hello64.ld 的文件头。
 *
 * 硬约束（tools/tar_build_win.sh 构建期断言）：所有 PT_LOAD 落在
 *   4GiB..4GiB+64KiB、无 PT_INTERP/PT_DYNAMIC、程序头表在首个 PT_LOAD 里、文件 <= 64 KiB。
 */
ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS {
    . = 0x0000000100000000 + SIZEOF_HEADERS;      /* 4GiB + 头部：ELF 头/程序头表落在首个 LOAD 内 */

    .text : ALIGN(16) {
        *(.text)
        *(.text.*)
    }

    /* ★ __init_array_start/__init_array_end、__fini_array_start/__fini_array_end 必须**显式**
     *   给边界符号（这里两个数组都是空的 -> start == end）。
     *   为什么：musl 的 libc_start_init/exit 会遍历这两个数组（exit.c 的 __libc_exit_fini：
     *   `for (a = &__fini_array_end; a > &__fini_array_start; ...) (*--a)()`）。
     *   它们是 weak hidden 符号；脚本不给边界时 lld 给的是**垃圾值**（实测：end=0x100000000、
     *   start 是一个更大的地址 -> 循环体里 `call *-0x8(%rcx)` 读 [0xFFFFFFF8] -> ring3 #PF
     *   err=5 cr2=0xFFFFFFF8 —— 第一次跑 musl 时踩的正是这一条）。
     *   本程序没有构造函数/析构函数 -> 两个数组都为空（start == end，循环体一次都不进）。
     *   将来若真有构造/析构函数，这两个段会落在可写一侧；边界符号仍是真值。 */
    .rodata : ALIGN(4096) {
        *(.rodata)
        *(.rodata.*)
    }

    .init_array : ALIGN(8) {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    }

    .fini_array : ALIGN(8) {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
    }

    .data : ALIGN(4096) {
        *(.data)
        *(.data.*)
        *(.got)
        *(.got.plt)                                /* 静态非 PIE：槽里是链接期填好的绝对地址 */
    }

    .bss : ALIGN(4096) {
        *(.bss)
        *(.bss.*)
        *(COMMON)
        *(.tdata)
        *(.tbss)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.interp)                                 /* 静态：不该有动态链接器 */
        *(.dynamic)
        *(.dynsym)
        *(.dynstr)
        *(.hash)
        *(.gnu.hash)
        *(.rela*)
    }
}
