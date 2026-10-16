/* store64.ld - ★ 应用商店 / 包管理器四个 ring3 产物（/bin/vpkg、/bin/store 与两个示例载荷）的链接脚本
 *
 * 与 user/apps/evshm_demo.ld **只有一处差别**：
 *   `.data` 也从 ALIGN(4096) 放宽到 ALIGN(16)。
 *
 * 为什么必须单独一份（不能改共享的那份）：
 *   * 共享脚本里 `.data : ALIGN(4096)` 是 **W^X 的机制**：段贴在一起之后，**小程序**
 *     （实测 build64/store/demo_cli.elf）的 .text 尾部（可执行）会和紧随其后的 .data
 *     （可写）落在同一个 4 KiB 页里，内核按页取权限并集（kernel/elf64.cpp）会让那一页变 RWX。
 *     共享脚本服务 9 个 ring3 产物（build64.sh 的 evshm/wlclient/wm/drvdemo/wl 客户端…），
 *     其中好几个都是小程序，所以那里必须留页对齐。
 *   * 本脚本只服务 build64/store/ 的四个产物，而 user/store/build_store.sh 的自检里有一条
 *     **"没有任何一页同时 W 与 X"** 的硬断言，逐个产物核验 —— 于是这里的放宽是**被验证过的**，
 *     不是"大概没事"。
 *
 * 为什么非要放宽（体积事实，实测）：
 *   /bin/store 的 .text+.rodata 已经贴到 0x10000f200，页对齐的 .data 会被顶到 0x100010000，
 *   正好等于内核装载窗上界（kernel/usermode64.h 的 USER64_STACK_VA64 = 4GiB+64KiB），
 *   被 kernel/elf64.cpp 的段检查直接拒（`va + memsz <= e64_hi64()`）。
 *   放宽后：.text 0x1d0+54314 -> 0x10000d5da、.rodata(ALIGN 16) -> 0x10000d600+7168 = 0x10000f200、
 *   .data(ALIGN 16) -> 0x10000f200+1409 = 0x10000f781 —— 整幅 63347 B 全在窗内。
 *   /bin/vpkg 在同一份脚本下更宽裕（59595 B）。
 *
 * 其余（`. = 4GiB + SIZEOF_HEADERS`、`.text.start` 排第一、收 .l* 段、无 PT_INTERP/PT_DYNAMIC、
 * 所有 PT_LOAD 必须落在 4GiB..4GiB+64KiB、`.rodata`/`.bss` 的 ALIGN(16)）与 evshm_demo.ld 一致，
 * 那边的长注释解释了 .rodata 那条为什么对**所有**产物都安全。
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

    .rodata : ALIGN(16) {
        *(.rodata)
        *(.rodata.*)
        *(.lrodata)
        *(.lrodata.*)
    }

    .data : ALIGN(16) {                           /* ★ 与共享脚本的唯一差别（W^X 由本目录的自检核验） */
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
