/* mp64.ld - ★ v0.11.0：MicroPython 解释器（/lib/micropython.bin）的链接脚本
 *
 * 与 user/lua/lua64.ld 逐条同构（那边是 /lib/lua.bin，这边是 /lib/micropython.bin）：
 * 映像钉在 USER64_MMAP_VA64 = 4GiB + 0x90000，由装载驱动 user/micropython/mpdrv.c
 * 用 mmap(9) 申请同一段地址、把段搬进去、改 auxv 后 jmp 进 e_entry。
 *
 * 硬约束（tools/micropython_check_elf.py 逐条断言；写法和 lua_build_win.sh 的 check_lua() 一致）：
 *   * 所有 PT_LOAD 的 p_vaddr >= 4GiB+0x90000，且 p_vaddr+p_memsz <= 4GiB+16MiB；
 *   * 第一个 PT_LOAD 从文件偏移 0 起、p_vaddr = 4GiB+0x90000（驱动按它算 auxv 的 AT_PHDR）；
 *   * 无 PT_INTERP / 无 PT_DYNAMIC（驱动不做任何重定位）；
 *   * 程序头表落在第一个 PT_LOAD 的文件范围内；e_phnum <= 16。
 *
 * -mcmodel=large 的专用段名（.ltext/.lrodata/.ldata/.lbss）必须收：映像在 4GiB，
 * small/medium 代码模型会产生 R_X86_64_32/32S 越界重定位（lld 直接报 out of range）。
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
