/* viewer64.ld - ★ v0.10.5：/bin/viewer（图片查看器）专用链接脚本
 *
 * 基址 = 0x100090000 = USER64_MMAP_VA64（4GiB+576KiB），与 user/store/vpkg64.ld 同一条先例。
 *
 * 为什么不放老装载区（4GiB..4GiB+64KiB）：
 *   查看器要装 vendored stb_image 的 PNG + JPEG + BMP 三个解码器（约 60 KB 机器码）
 *   + 窗口壳 + 用户态 C 运行时 + font64（stb_truetype），PT_LOAD 合计远超 64 KiB。
 *   kernel/elf64.cpp 的 e64_pick_main_layout64() 早就有"大映像"这条真实路径
 *   （tcc / busybox / lua / make / vpkg 都在这条路上）：
 *     ① 段落落在 4GiB..4GiB+64KiB  -> 老装载区（逐字节不变的老布局）
 *     ② 否则 -> lo=4GiB、hi=USER64_LOADER_BIG_TOP_VA64（≈4GiB+990MiB），
 *        冲突由 e64_region_conflict64() 挡（不得压栈/brk/邮箱/自检页/DEV/SHM 窗）
 *   所以基址只要 >= USER64_MMAP_VA64，就自动落到窗口②。
 *
 * 其余与 store64.ld 一致（.text.start 第一、收 .l* 段、.data/.bss ALIGN(16)）。
 *   "没有任何一页同时 W 与 X" 由 user/viewer/build_viewer.sh 的产物自检逐段核验。
 */
ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS {
    . = 0x0000000100090000 + SIZEOF_HEADERS;      /* 4GiB+576KiB = USER64_MMAP_VA64 */

    .text : ALIGN(16) {
        *(.text.start)                            /* crt0.S 的 _start 必须第一 */
        *(.text)
        *(.text.*)
        *(.ltext)
        *(.ltext.*)
    }

    .rodata : ALIGN(16) {
        *(.rodata)
        *(.rodata.*)
        *(.lrodata)
        *(.lrodata.*)
    }

    .data : ALIGN(16) {
        *(.data)
        *(.data.*)
        *(.ldata)
        *(.ldata.*)
        *(.got)
        *(.got.plt)
    }

    .bss : ALIGN(16) {
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
