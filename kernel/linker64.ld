/* linker64.ld - Vimtu64 64 位内核链接脚本（LLD / clang 工具链）
 *
 * 与 kernel/linker_lld.ld（32 位）的差异：
 *   OUTPUT_FORMAT 改为 elf64-x86-64 —— 64 位内核镜像必须是 ELF64，
 *   否则 objcopy 产出的 kernel64.bin 里符号重定位按 32 位算，一跑就崩。
 *
 * 其它保持与 32 位一致（loader 是裸平铺加载，不解析 ELF，所以：
 *   - 段顺序必须与"从文件头开始平铺"一致：.text / .rodata / .data / .bss
 *   - .text.entry 必须排在最前面：loader 跳到内核入口的**虚拟地址**，那就是入口）
 *
 * ★ 内核搬高半区（为将来"用户态应用"让出低地址）：
 *   引导层建立一段**高半区线性直映**：  虚拟 = 0xFFFFFFFF80000000 + 物理
 *   内核被平铺装载在物理 0x100000，所以它的链接基址 = 0xFFFFFFFF80100000。
 *
 *   ★ 为什么基址不是 0xFFFFFFFF80000000：直映窗口的起点对应物理 0，
 *     而物理 0 是中断向量表/BDA。若内核链在 0xFFFFFFFF80000000，跳过去就是执行垃圾
 *     （搬高半区时踩过：链接基址与物理装载地址必须满足 VA = BASE + PA）：
 *         链接基址(0xFFFFFFFF80100000) == 直映起点(0xFFFFFFFF80000000) + 装载物理地址(0x100000)
 *
 *   直映之外，引导层**必须保留 0..4GB 恒等映射** —— 内核里帧缓冲(LFB)、
 *   安装载荷(0x04000000)、页池与堆都是按**物理地址**直接访问的。
 */
ENTRY(lm64_entry)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS {
    . = 0xFFFFFFFF80100000;

    .text : {
        KEEP(*(.text.entry))
        *(.text)
        *(.text.*)
    }

    .rodata : ALIGN(16) {
        *(.rodata)
        *(.rodata.*)
    }

    .data : ALIGN(16) {
        *(.data)
        *(.data.*)
    }

    .bss : ALIGN(16) {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    . = ALIGN(4096);
    __bss_end = .;
    _end = .;
}
