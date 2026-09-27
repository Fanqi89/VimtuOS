/* ldso.ld - ★ A3 下半：**我们自己写的动态链接器**（ld.so）的链接脚本
 *
 * 与 user/hello_elf64.ld / tools/musl_hello64.ld 的关键区别：那份是 **ET_EXEC**（钉死在
 * 4GiB 的装载区），本份必须是 **ET_DYN**（位置无关、链接基址 0）—— 因为：
 *   1) 内核（kernel/elf64.cpp 的 e64_load_interp64）给解释器挑的基址是"用户窗口顶部往下
 *      span 页"（避免与主程序的装载区、mmap/brk 区打架），运行期基址只能靠 AT_BASE 告诉它；
 *   2) 链接基址 0 时，脚本里的绝对地址（.data 里的指针、GOT 槽）都以"0 + 偏移"形式出现，
 *      运行期由 ld.so 自己的 R_X86_64_RELATIVE 修正 —— 这正是本文件要验证的第一条路径。
 *
 * 三条硬约束（内核装载解释器时按它们检查，见 e64_load_interp64；构建期由
 * tools/dynlink_check_win.py 断言）：
 *   * 段必须**逐页对齐**：内核按页给权限（R|X / R|W 分开，不能同页混）；
 *   * ELF 头 + 程序头表必须落在**第一个 PT_LOAD** 里（p_offset = 0 起）：ld.so 自己用
 *     `(Elf64_Ehdr*)AT_BASE` 读自己的程序头找 PT_DYNAMIC（AT_BASE 就是第一个段的基址）；
 *   * 整个 span 要放得进 [USER64_MMAP_VA64, 窗口顶]（448 KiB）。
 * 脚本写法上的讲究：**不写 PHDRS 块**，交给 lld 按段权限自动分组（实测这样第一段就是
 * p_offset=0 / p_vaddr=0 的 R 段，头 + 程序头表都在里面）；一旦手写 PHDRS，lld 会把第一个
 * 段钉在第一个 section 的地址上、把头排除在段外（AT_BASE 处就既没有 ELF 头、也没有程序头表）。
 *
 * ★ 本脚本同时被 **libfoo.so**（user/apps/libfoo.c）复用：两者都是"链接基址 0 + 位置无关"
 *   的共享对象，段权限分工完全一样。区别只有一条：libfoo 有 .init_array（构造函数）——
 *   这里**不能**丢它（ld.so 调用它，那正是 DT_INIT_ARRAY 路径的证据）；ldso.c 没有构造函数，
 *   所以同一个脚本对它没有副作用。
 */
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

    /* ---- 可写一侧（ld.so 自己**不做**页对齐）----
     * ★ A3 下半说明：只有 **libfoo.so** 需要"代码页 / 数据页分开"（那样它的代码页才会是
     *   R|X、可被 tests/dynlink64_test.py 的 ⑤ 断言看到；见 user/ldso/libfoo.ld）。
     *   解释器自己不需要这条（没有任何断言依赖它的页权限），而页对齐要多花一页 ≈1.3KB
     *   内核预算 —— 所以这里保持紧凑排布，libfoo 用另一份脚本。 */
    .data : ALIGN(16) {
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
