/* peinfo64.ld - ★ v0.9.43（W0）：/bin/peinfo 的链接脚本
 *
 * 与 user/gzip/gzip64.ld 同一条硬约束（内核 elf64.cpp 会**拒绝**越界段）：
 *   * 所有 PT_LOAD 落在用户窗口低段（4GiB 起，ELF64 装载区）；
 *   * 无 PT_INTERP / 无 PT_DYNAMIC（内核只做静态装载）；
 *   * ELF 头 + 程序头表落在**第一个 PT_LOAD** 里（auxv 的 AT_PHDR 才有值）；
 *   * 入口必须是 .text 最前面（.text.start）。
 * -mcmodel=large 的专用段名（.ltext/.lrodata/.ldata/.lbss）都要收。
 *
 * 与 gzip 的唯一差别：本程序的**静态数据**只有几十字节计数器 + 16 KiB 导入并集缓冲
 * （g_union），文件内容缓冲与节表都在运行期 mmap(9) 里 —— 所以映像大小与实际要看的 PE
 * 完全无关（一个 8 MiB 的 exe 和一个 2 KB 的 exe，本程序的映像一模一样）。
 */
ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS {
    . = 0x0000000100000000 + SIZEOF_HEADERS;

    .text : ALIGN(16) {
        *(.text.start)
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
