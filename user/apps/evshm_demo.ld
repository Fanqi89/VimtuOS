/* evshm_demo.ld - ★ A5 前置：/evshm.elf（user/apps/evshm_demo.c）的链接脚本
 *
 * 与 user/shell/shell64.ld 同一条思路（那份是 ring3 shell 用的），差别只在段名收得更省：
 *   * `. = 4GiB + SIZEOF_HEADERS` —— ELF 头/程序头表落在**第一个 PT_LOAD 内**，
 *     kernel/elf64.cpp 才能算出 auxv 的 AT_PHDR；
 *   * `.text.start` 排第一（user/lib/crt0.S 的 _start 必须顶在映像首字节）；
 *   * .l*（.ltext/.lrodata/.ldata/.lbss）是 -mcmodel=large 的专用段名，必须收，
 *     否则数据会落到脚本外面（kernel/usermode64.h 的注释里记过这个坑）；
 *   * 无 PT_INTERP / PT_DYNAMIC / 重定位（内核只做静态装载）。
 *
 * 硬约束（kernel/elf64.cpp 会拒绝越界段）：所有 PT_LOAD 必须落在
 * 4GiB .. USER64_STACK_VA64（= 4GiB+64KiB），无 PT_INTERP/PT_DYNAMIC，文件 <= 96 KiB。
 */
ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS {
    . = 0x0000000100000000 + SIZEOF_HEADERS;      /* 4GiB + 头部：ELF 头/程序头表在首个 LOAD 内 */

    .text : ALIGN(16) {
        *(.text.start)                            /* crt0.S 的 _start 必须第一 */
        *(.text)
        *(.text.*)
        *(.ltext)
        *(.ltext.*)
    }
    /* ★ 段对齐（本批：给 /bin/vpkg、/bin/store 的应用注册表代码腾装载区）
     *   只放宽 **.rodata / .bss**（ALIGN(4096) -> ALIGN(16)）；**.data 保持 ALIGN(4096)**
     *   —— 那一条是 W^X 的机制，不是浪费，见下面"为什么 .data 必须保持页对齐"。
     *   为什么原来浪费：内核的**主程序**装载窗是硬的 64 KiB（kernel/elf64.cpp 的
     *   e64_lo64/e64_hi64），而 .rodata 紧跟在 .text 后面；.rodata 再来一条页对齐，
     *   等于在 .text 尾部**白扔最多 4 KiB**，还会把 .data 再顶高一页。
     *   实测（同一份 /bin/vpkg 代码）：.rodata 从 0x10000d000 退到 0x10000c060（省 4006 B），
     *   PT_LOAD 的 memsz 合计 59595 B 一个字节没变，但 .data 因此回到 0x10000f000
     *   （旧布局 .data@0x100010000 正好 == e64_hi64，被内核装载器直接拒）；
     *   /bin/store 同理：.rodata 0x10000e000 -> 0x10000d5e0、.data 0x100010000 -> 0x10000f000。
     *   为什么 `.data` 必须保持页对齐（W^X，本批实测踩过）：段贴在一起后，**小程序**的
     *   .text 尾部（可执行）会和紧随其后的 .data（可写）落在**同一个 4 KiB 页**里 ——
     *   内核按页取权限并集（kernel/elf64.cpp），那一页就变成 RWX。本批第一版把 .data 也
     *   放宽到 ALIGN(16)，立刻被 user/store/build_store.sh 新增的"没有一页同时 W 与 X"
     *   断言抓到（build64/store/demo_cli.elf page=0x100001000）。所以 .data 的页对齐留着。
     *   对既有验收的影响：相关脚本（busybox/tcc/lua/tar/gzip/netuser…）只断言
     *   "PT_LOAD 落在 4GiB..+64KiB"，没有一条钉死具体 p_vaddr（已 grep tests/*.py 复核），
     *   所以这条改动不动任何断言。 */
    .rodata : ALIGN(16) {
        *(.rodata)
        *(.rodata.*)
        *(.lrodata)
        *(.lrodata.*)
    }

    .data : ALIGN(4096) {       /* ★ 页对齐是 W^X 的机制（实测：放宽就出 RWX 页），别动 */
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
