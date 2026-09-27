/* musl_hello64.ld - ★ A3：**musl 静态程序**的链接脚本（tools/musl_build_win.sh 用）
 *
 * 与 user/hello_elf64.ld（A1 的汇编程序用）的关系：
 *   同一条"把映像钉在用户窗口 4GiB、并让 ELF 头/程序头表落在第一个 PT_LOAD 里"的思路
 *   （auxv 的 AT_PHDR 才有值，musl 的 __init_tls 要遍历程序头表找 PT_TLS）。
 *   区别只有一条（**必须**）：本脚本**不丢 .got**。
 *
 * 为什么不能丢 .got（踩过的坑，实测报错原文）：
 *   ld.lld: error: libc.a(exit.o):(function libc_exit_fini: .text+0x27):
 *   relocation R_X86_64_REX_GOTPCRELX out of range: -4294969291 is not in
 *   [-2147483648, 2147483647]; references '__fini_array_start'
 *   musl 的静态目标文件是**小代码模型**编的：对 `__fini_array_start` 这类符号用
 *   GOTPCRELX（PC 相对 + GOT 槽）—— 映像在 4GiB 时 32 位 PC 相对放不下，必须真的分配
 *   一个 GOT 槽；而 user/hello_elf64.ld 的 `/DISCARD/ { *(.got*) }` 会把槽丢掉，于是链接
 *   直接失败。**静态非 PIE** 的 GOT 槽由链接器在链接期填最终绝对地址（不产生 PT_DYNAMIC、
 *   不需要运行期重定位），所以保留 .got 是完全安全的 —— 内核的静态装载路径照样能跑。
 *
 * 段权限仍按"页粒度"分开（内核 elf64.cpp 按页取权限并集）：
 *   .text = R|X、.rodata = R、.data(+.got) = R|W、.bss = R|W。
 * 顺序上 .init_array/.fini_array 放在 .rodata 一起（musl 的 libc_start_init 会按
 * __init_array_start/__init_array_end 遍历它们；本程序没有构造函数，通常是空的）。
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
