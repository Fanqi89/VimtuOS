/* vpkg64.ld - ★ v0.10.3：/bin/vpkg 专用链接脚本（大映像基址 4GiB+576KiB）
 *
 * 为什么必须换（实测）：v0.10.3 把 zstd 解码器（user/store/vs_zstd.c）编进 /bin/vpkg，
 *   映像的 PT_LOAD 总和从 61,851 B 涨到 74,576 B，越过老装载区上界
 *   （kernel/usermode64.h 的 USER64_STACK_VA64 = 4GiB+64KiB），构建期自检直接红：
 *       build64/store/vpkg.elf PT_LOAD 越出主程序装载区（va=0x10000ee80 msz=0x2ef0 <= 0x10000）
 *
 * 为什么搬基址是**内核早已支持的**（不是新机制）：
 *   kernel/elf64.cpp 的 e64_pick_main_layout64() 对 ET_EXEC 有两条路：
 *     ① 段落在 4GiB..4GiB+64KiB  -> 老装载区（历史布局，逐字节不变）；
 *     ② 否则 -> "大映像"：lo=4GiB、hi=USER64_LOADER_BIG_TOP_VA64（≈4GiB+990MiB），
 *        冲突由 e64_region_conflict64() 挡（不得压 [栈..mmap) 与 DEV/SHM 窗）。
 *   于是**基址只要 >= USER64_MMAP_VA64 = 4GiB+576KiB**，就自动落到窗口②：
 *   user/apps/tcc/tcc64.ld、user/busybox/busybox64.ld、user/lua/lua64.ld、
 *   user/make/make64.ld 全是这条先例。
 *
 * 为什么是 0x100090000 而不是别的：它就是 USER64_MMAP_VA64。
 *   比它低会压到 [栈/brk/终端邮箱/自检页] 那段固定分区（inner 冲突检查会拒载）。
 *
 * 其余与 store64.ld 一致（.text.start 第一、收 .l* 段、.data/.bss ALIGN(16)、
 *   W^X 由 user/store/build_store.sh 的"没有任何一页同时 W 与 X"断言逐产物核验）。
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
