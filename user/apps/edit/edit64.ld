/* edit64.ld - ★ A4-5：/bin/edit 的链接脚本
 *
 * 硬约束与 user/gzip/gzip64.ld、user/apps/evshm_demo.ld 完全相同（kernel/elf64.cpp 会拒绝越界段）：
 *   * 所有 PT_LOAD 落在 4GiB..4GiB+64KiB（USER64_STACK_VA64 之前）—— 编辑器的静态数据只有
 *     渲染行缓存（22×80 B）+ 几个小缓冲（文本/行索引都在运行期 mmap），装得下；
 *   * 无 PT_INTERP / 无 PT_DYNAMIC（内核只做静态装载）；文件 <= 64 KiB；
 *   * ELF 头 + 程序头表落在**第一个 PT_LOAD** 内（auxv 的 AT_PHDR 才有值）；
 *   * 入口必须是映像最前面（.text.start，见 edit_start.S）。
 * -mcmodel=large 的专用段名（.ltext/.lrodata/.ldata/.lbss）都要收，否则数据落到脚本外。
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
