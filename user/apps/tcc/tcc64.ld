/* tcc64.ld - ★ A4-2b：**tcc 自身**（/lib/tcc.bin）的链接脚本
 *
 * 与其它链接脚本的关系（别混）：
 *   * user/lib/user64.ld、user/shell/shell64.ld、user/apps/tcc/tccdrv64.ld 都把映像钉在
 *     **4GiB**（内核装载区 4GiB..4GiB+64KiB）；
 *   * 本脚本把 tcc 钉在 **4GiB+0x90000 = USER64_MMAP_VA64**：tcc 不走内核的主程序装载器
 *     （64 KiB 装不下 300 KB 的编译器），而是由 /bin/tcc（user/apps/tcc/tccdrv.c）用
 *     mmap(9) 从这块地址起装载 —— 本进程第一次 mmap 的返回地址正好就是 USER64_MMAP_VA64
 *     （kernel/proc64.cpp 的每进程 bump 分配器起点）。驱动会断言两者相等，不等就如实失败。
 *   * 于是 tcc 是**非 PIC、按 4GiB+0x90000 定址**的静态 ELF，装载期不需要任何重定位。
 *
 * 硬约束（tools/tcc_build_win.sh 构建期断言，驱动运行期再断言一次）：
 *   * 所有 PT_LOAD 的 p_vaddr 都 >= 4GiB+0x90000，且 span <= 448 KiB（窗口顶 - mmap 起点）；
 *   * 第一个 PT_LOAD 从文件偏移 0 起（p_vaddr = 4GiB+0x90000），ELF 头 + 程序头表在里面
 *     —— 驱动要用它算 auxv 的 AT_PHDR；
 *   * 无 PT_INTERP / 无 PT_DYNAMIC（驱动不做重定位）。
 *
 * -mcmodel=large 的专用段名（.ltext/.lrodata/.ldata/.lbss）必须收：tcc 的宿主构建环境里
 *   没有它们，但 **VimtuOS 目标**这一份必须用 -mcmodel=large（映像在 4GiB，small/medium
 *   代码模型会产生 R_X86_64_32/32S 越界重定位 —— 实测 lld 直接报 out of range）。
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
     * 理由与 tools/musl_hello64.ld 完全一致（不给边界 lld 会给垃圾值 -> ring3 #PF）。
     * 注意：它们必须**留在同一个输出段里**（不单开 .init_array/.fini_array 输出段），
     * 否则 lld 报 `section: .init_array is not contiguous with other relro sections`（实测）。 */
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
