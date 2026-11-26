/* httpsprobe64.ld - VimtuOS64：HTTPS 探针（/bin/httpsprobe）的链接脚本
 *
 * 与 user/busybox/busybox64.ld 同一条纪律，区别只在"谁把它装进去"：
 *   * 基址 = 0x100090000 = USER64_MMAP_VA64（4GiB+576KiB）。为什么必须是这个值而不是 4GiB：
 *     内核 elf64.cpp 的装载区有两条：老装载区 [4GiB, 4GiB+64KiB)（只给 < 64 KiB 的小程序）
 *     和"大映像"区 [USER64_MMAP_VA64, USER64_LOADER_BIG_TOP_VA64)。本程序 + mbedTLS
 *     肯定 > 64 KiB，落老装载区会被 reject reason=outside-user-window。
 *     **而且**基址要**正好**等于 USER64_MMAP_VA64：大映像区的装载步骤是
 *     `base = user64_find_free64(USER64_MMAP_VA64, span) - min_vaddr`，只有
 *     min_vaddr == USER64_MMAP_VA64 时 base 才是 0 —— 本程序是**静态非 PIE**（-fno-pie），
 *     base 一旦不为 0，所有绝对地址都会错位。
 *   * 所有 PT_LOAD 的 p_vaddr >= 基址，且第一个 PT_LOAD 从文件偏移 0 起、p_vaddr == 基址
 *     （ELF 头 + 程序头表在里面，auxv 的 AT_PHDR 才有值；musl 的 static_init_tls 靠它找 PT_TLS）。
 *   * 无 PT_INTERP / 无 PT_DYNAMIC（静态链接，装载期不做任何重定位）。
 *
 * -mcmodel=large 的专用段名（.ltext/.lrodata/.ldata/.lbss）必须收：映像在 4GiB+，
 * small/medium 代码模型会产生 R_X86_64_32/32S 越界重定位（链接期直接报错，不是运行期怪事）。
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
