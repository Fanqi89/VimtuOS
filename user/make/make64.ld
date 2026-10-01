/* make64.ld - ★ B5：**GNU make 自身**（/lib/make.bin）的链接脚本
 *
 * 与 user/lua/lua64.ld 同一条路线（为什么不能走内核主程序装载器）：VimtuOS 的内核 ELF64 装载器
 *   只装"主程序"，而主程序的 PT_LOAD 必须完整落在**用户窗口低 64 KiB**（kernel/usermode64.h 的
 *   4GiB..USER64_STACK_VA64）。静态链接的 GNU make 是几百 KB，装不进 64 KiB。这里把 make 钉在
 *   **4GiB+0x90000 = USER64_MMAP_VA64**：由小驱动 /bin/make（user/make/makedrv.c）用 mmap(9)
 *   申请到同一地址、把段搬进去、改 auxv 后 jmp 进 e_entry。
 *
 * 硬约束（tools/make_build_win.sh 构建期断言，驱动运行期再断言一次）：
 *   * 所有 PT_LOAD 的 p_vaddr 都 >= 4GiB+0x90000，且 span <= 窗口顶 - mmap 起点（~15.4 MiB）；
 *   * 第一个 PT_LOAD 从文件偏移 0 起（p_vaddr = 4GiB+0x90000），ELF 头 + 程序头表在里面
 *     —— 驱动要用它算 auxv 的 AT_PHDR（musl 的 static_init_tls 要遍历程序头找 PT_TLS）；
 *   * 无 PT_INTERP / 无 PT_DYNAMIC（驱动不做任何重定位）。
 *
 * -mcmodel=large 的专用段名（.ltext/.lrodata/.ldata/.lbss）必须收：映像在 4GiB，small/medium
 * 代码模型会产生 R_X86_64_32/32S 越界重定位（lld 直接报 out of range）。
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
