/* libfoo.ld - ★ A3 下半：**共享库 libfoo.so** 的链接脚本
 *
 * 与 user/ldso/ldso.ld 的唯一区别：可写段之前插了 `. = ALIGN(4096);`。
 * 为什么只有 .so 需要（为什么不干脆共用一份）：
 *   * 页对齐之后，libfoo.so 的**代码页**是独立的 R|X（mprotect prot=5，**不可写**）、
 *     数据页是独立的 R|W（prot=6）—— 这是 tests/dynlink64_test.py ⑤ 号断言的证据来源；
 *   * 不对齐时 .text 与 .data 挤在同一页，ld.so 的"按页取权限并集"只能给出 R|W|X，
 *     代码页变成可写（Linux 上同理：段不页对齐时权限只能取并集）；
 *   * 解释器（ldvimtu.so）没有这条需求，而多对齐一页要多花 ≈1.3KB **内核预算**（三份产物
 *     都内嵌进系统内核），所以它继续用紧凑排布的 ldso.ld。
 *
 * 其余与 ldso.ld 完全一致（ET_DYN、链接基址 0、头 + 程序头表落在第一个 PT_LOAD 里、
 * 不写 PHDRS 让 lld 按段权限自动分组）—— 理由见 user/ldso/ldso.ld 的详细说明。
 * libfoo 有 .init_array（构造函数）：这里**不能**丢它（ld.so 调用它，那正是 DT_INIT_ARRAY
 * 路径的证据）。 */
ENTRY(_dlstart)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS {
    . = SIZEOF_HEADERS;      /* ELF 头 + 程序头表落在第一个 PT_LOAD 里（文件偏移 0 起） */

    /* ---- 只读/可执行一侧 ---- */
    .dynsym : ALIGN(8)  { *(.dynsym) }
    .gnu.hash : ALIGN(8) { *(.gnu.hash) }
    .hash : ALIGN(8)    { *(.hash) }
    .dynstr : ALIGN(8)  { *(.dynstr) }
    .rela.dyn : ALIGN(8) { *(.rela.dyn) *(.rela*) }
    .rela.plt : ALIGN(8) { *(.rela.plt) }
    .plt : ALIGN(16) { *(.plt) *(.plt.*) }
    .text : ALIGN(16) {
        *(.text)
        *(.text.*)
    }
    .rodata : ALIGN(16) {
        *(.rodata)
        *(.rodata.*)
    }

    /* ---- ★ 页对齐分界：下面这些进**另一个** 4KiB 页（独立 R|W PT_LOAD）。
     * 用"段的地址对齐"这种写法（`.data : ALIGN(4096)`）：实测单纯写一行 `. = ALIGN(4096);`
     * 在本项目的 lld 上**不生效**（段还是挤在一起、代码页仍是 R|W|X）。 */
    .data : ALIGN(4096) {
        *(.data)
        *(.data.*)
        *(.data.rel.ro)
        *(.data.rel.ro.*)
        *(.got)
        *(.got.plt)
    }
    /* .init_array/.fini_array 与 .dynamic 一起进可写段：
     *   * .init_array 是**数据**（一组函数指针），ld.so 读它并调用（DT_INIT_ARRAY）；
     *   * .fini_array 本阶段不调用（dlclose 才有意义），但保留内容以便核对。 */
    .init_array : ALIGN(8) {
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
    }
    .fini_array : ALIGN(8) { KEEP(*(.fini_array)) }
    .dynamic : ALIGN(8) { *(.dynamic) }
    .bss : ALIGN(16) {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.interp)
    }
}
