/* bbdrv64.ld - VimtuOS64：/bin/busybox（busybox 装载驱动）的链接脚本
 *
 * 与 user/make/makedrv64.ld 同一条硬约束（内核 elf64.cpp 会**拒绝**越界段）：
 *   * 所有 PT_LOAD 落在 4GiB..4GiB+64KiB（USER64_STACK_VA64 之前）—— 驱动本体很小；
 *   * 无 PT_INTERP / 无 PT_DYNAMIC（内核只做静态装载）；
 *   * ELF 头 + 程序头表落在**第一个 PT_LOAD** 里（auxv 的 AT_PHDR 才有值）；
 *   * 入口必须是 .text 的最前面（.text.start）。
 * -mcmodel=large 的专用段名（.ltext/.lrodata/.ldata/.lbss）都要收。
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
