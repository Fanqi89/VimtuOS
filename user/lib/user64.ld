/* user64.ld - 用户态 C 程序链接脚本（Vimtu64 用户态 C 运行时）
 *
 * 目标：产出一个**平铺 blob**（objcopy -O binary），它能被 kernel/usermode64.cpp 的 blob
 * 加载器原样拷到用户代码页（kernel/usermode64.h: USER64_CODE_VA64 = 4GiB）后直接跑。
 *
 * ★ 为什么链接地址必须是 4GiB：加载器把 blob 拷到那个 VA，程序用的是**链接期定死的绝对地址**
 *   （编译用 -mcmodel=large，见 user/build_user.sh），所以链接地址 = 运行地址，差一个字节都不行。
 * ★ .text.start 段必须排第一（crt0.S 的 _start 在那里）：加载器跳的是 blob 的首字节。
 * ★ 段名里的 .l*（.ltext/.lrodata/.ldata/.lbss）是 -mcmodel=large 专用段名，漏了它们
 *   数据就会落到脚本外面（实测过：.lbss 会被放到脚本外，blob 长度算错）。
 * ★ .image_end：一个 1 字节的哨兵段，作用是**让 objcopy -O binary 把 .bss 的零也算进 blob**。
 *   .bss 是 NOBITS，本身不进二进制；没有它 blob 会在 .data 结束处截断，
 *   而 .bss 尾部的页可能落在加载器映射的页之外 -> ring3 一写就 #PF。 */
ENTRY(_start)

SECTIONS
{
    . = 0x0000000100000000;                     /* 4GiB：= USER64_CODE_VA64 */

    .text : {
        *(.text.start)                          /* _start 必须第一 */
        *(.text .text.* .ltext .ltext.*)
    }
    .rodata : ALIGN(16) { *(.rodata .rodata.* .lrodata .lrodata.*) }
    .data   : ALIGN(16) { *(.data .data.* .ldata .ldata.*) }
    .bss    : ALIGN(16) { *(.bss .bss.* .lbss .lbss.*) *(COMMON) }
    .image_end : { BYTE(0); }

    /DISCARD/ : { *(.comment) *(.note.GNU-stack) *(.note*) *(.eh_frame*) *(.eh_frame) }
}
