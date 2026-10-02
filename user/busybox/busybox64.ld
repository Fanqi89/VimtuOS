/* busybox64.ld - VimtuOS64：busybox 本体（卷里 /lib/busybox.bin）的链接脚本
 *
 * 硬约束（tools/busybox_build_win.sh 构建期断言，user/busybox/bbdrv.c 运行期再断言一次）：
 *   * 所有 PT_LOAD 的 p_vaddr >= USER64_MMAP_VA64 = 4GiB+0x90000，span 不越用户窗口顶（4GiB+16MiB）；
 *   * 第一个 PT_LOAD 从文件偏移 0 起、p_vaddr == 4GiB+0x90000，ELF 头 + 程序头表在里面
 *     —— 驱动要用它算 auxv 的 AT_PHDR（musl 的 static_init_tls 靠 AT_PHDR 遍历程序头找 PT_TLS）；
 *   * 无 PT_INTERP / 无 PT_DYNAMIC（驱动不做任何重定位）。
 *
 * -mcmodel=large 的专用段名（.ltext/.lrodata/.ldata/.lbss）必须收：映像在 4GiB，
 * small/medium 代码模型会产生 R_X86_64_32/32S 越界重定位。
 */
ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS {
    . = 0x0000000100090000 + SIZEOF_HEADERS;      /* 4GiB+576KiB + 头部 */

    .text : ALIGN(16) {
        *(.text.start)
        *(.text)
        *(.text.*)
        *(.ltext)
        *(.ltext.*)
    }

    /* musl 的 exit 会遍历 init/fini 数组的边界符号；显式给边界（空数组 start==end）。
     * 它们必须留在同一个输出段里，否则 lld 报 "not contiguous with other relro sections"。 */
    .rodata : ALIGN(4096) {
        *(.rodata)
        *(.rodata.*)
        *(.lrodata)
        *(.lrodata.*)
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
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
        *(.tdata)
        *(.tbss)
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
