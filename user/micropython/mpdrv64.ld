/* mpdrv64.ld - ★ v0.11.0：/bin/mp（MicroPython 装载驱动）的链接脚本
 *
 * 逐字拷贝 user/lua/luadrv64.ld（那份已在真机上验证过），只改了本段注释。
 * 与 user/apps/tcc/tccdrv64.ld 同一条硬约束（内核 elf64.cpp 会**拒绝**越界段）：
 *   * 所有 PT_LOAD 落在 4GiB..4GiB+64KiB（USER64_STACK_VA64 之前）—— 驱动本体很小（~15 KB）；
 *   * 无 PT_INTERP / 无 PT_DYNAMIC（内核只做静态装载）；
 *   * ELF 头 + 程序头表落在**第一个 PT_LOAD** 里（auxv 的 AT_PHDR 才有值）；
 *   * 入口必须是 .text 的最前面（.text.start）。
 * -mcmodel=large 的专用段名（.ltext/.lrodata/.ldata/.lbss）都要收。
 *
 * 注意两个 ld 脚本的**输出段名相同**（.text/.rodata/.data/.bss），但基址不同：
 *   本脚本            4GiB + 0        （驱动，走内核主程序装载器）
 *   port/mp64.ld      4GiB + 0x90000  （解释器，由本驱动 mmap 后 jmp 进去）
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
