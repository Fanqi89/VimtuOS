#!/bin/bash
# tools/dynlink_build_win.sh - ★ A3 下半：构建**动态链接三件套**（我们自己的 ld.so + 演示程序）
#
# 产出（默认都落在 build64/）：
#   build64/ldvimtu.so    我们的动态链接器（ET_DYN，链接基址 0；内核按 AT_BASE 映射到窗口顶）
#   build64/libfoo.so     演示用共享库（ET_DYN；DT_INIT_ARRAY + DT_INIT + SONAME + RELATIVE）
#   build64/dynhello.elf  动态主程序（ET_DYN/PIE，PT_INTERP=/lib/ldvimtu.so，DT_NEEDED=libfoo.so）
#
# 由 build64.sh 在"用户态程序"段调用；三份产物都会 objcopy 内嵌进**系统内核**，启动期由
# kernel/kernel64.cpp 的 dynlink64_install64() 幂等装进 VimtuFS2：
#   /lib/ldvimtu.so   /lib/libfoo.so   /dynhello.elf
#
# 为什么用 ld.lld 而不是 clang 直接链：
#   * ldso/libfoo 是 `-shared`（ET_DYN / 位置无关），dynhello 是 `-pie`（ET_DYN 但要 PT_INTERP）；
#     clang 的 driver 在 --target=x86_64-linux-gnu 下会去找 glibc 的 crt/`-lc`，这里全都不需要；
#   * 链接脚本（user/ldso/*.ld）必须显式给段权限（R|X / R|W 逐页分开）与基址。
#
# 每个产物的硬约束都由 tools/dynlink_check_win.py 在构建期断言（不依赖 binutils）：
#   段数/权限/页对齐、程序头表在第一个 PT_LOAD、解释器 span 放得进 448KiB、
#   三类重定位（RELATIVE/GLOB_DAT/JUMP_SLOT/64）都在、DT_INIT/DT_INIT_ARRAY/SONAME/NEEDED 都在。
set -e

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

OUT_DIR="${1:-$ROOT/build64}"
mkdir -p "$OUT_DIR"

if command -v py >/dev/null 2>&1; then PY="py -3"; else PY="${PYTHON:-python}"; fi
LD=ld.lld
CC="clang --target=x86_64-linux-gnu"

# 用户态编译标志（与 user/build_user.sh 同口径：无 libc、无 PIC 之外的额外要求）：
#   * -fPIC：三份产物都是位置无关（ldso/libfoo 是共享对象；dynhello 是 PIE）；
#   * -fno-stack-protector / -fno-asynchronous-unwind-tables：不需要 SSP/展开表（也没 libc）；
#   * -ffreestanding -nostdlib：不引任何系统头/库（我们的代码自带 typedef 与 syscall 包装）。
#   * -Os（不是 -O2）：解释器要**内嵌进内核**（objcopy 后算系统内核体积，见 build64.sh 结尾
#     的图标包区间断言）。实测 -O2 -> -Os 让 ldvimtu.so 的 .text 从 13109 B 降到 6391 B、
#     文件从 17048 B 降到 9336 B（配合 --strip-all）；功能不受影响（ld.so 没有热路径）。
CFLAGS_APP="-target x86_64-linux-gnu -ffreestanding -nostdlib -fPIC -fno-stack-protector \
 -fno-builtin -fno-asynchronous-unwind-tables -fno-unwind-tables -Os -Wall -Wextra -std=c11"


OBJ="$OUT_DIR/dynlink_build"
mkdir -p "$OBJ"

echo "==> 动态链接器：user/ldso/{ldso.c,ldso_start.S} -> $OUT_DIR/ldvimtu.so"
$CC $CFLAGS_APP -c user/ldso/ldso.c       -o "$OBJ/ldso.o"
$CC -target x86_64-linux-gnu -ffreestanding -nostdlib -fPIC -c user/ldso/ldso_start.S -o "$OBJ/ldso_start.o"
# -Bsymbolic：模块内部引用就地绑定（ld.so 不需要自己的 PLT 间接层；少一层自举风险）。
# -Bsymbolic：模块内部引用就地绑定（ld.so 不需要自己的 PLT 间接层；少一层自举风险）。
# --strip-all：内核只按程序头表装载（读 p_offset 拷字节），.symtab/.strtab/.shstrtab 一次都不用
#   —— 但它们也是内嵌字节的一部分。去掉后 ldvimtu.so 再省 ~800 B（libfoo/dynhello 同理）。
$LD -m elf_x86_64 -shared -Bsymbolic --no-undefined -z noexecstack --strip-all \
    -T user/ldso/ldso.ld -o "$OUT_DIR/ldvimtu.so" "$OBJ/ldso_start.o" "$OBJ/ldso.o"
$PY tools/dynlink_check_win.py ldso "$OUT_DIR/ldvimtu.so"

echo "==> 共享库：user/apps/libfoo.c -> $OUT_DIR/libfoo.so"
$CC $CFLAGS_APP -c user/apps/libfoo.c -o "$OBJ/libfoo.o"
# -init,foo_init：让链接器产出 DT_INIT（与 .init_array 一起验证 ld.so 的初始化调用）。
# -soname libfoo.so：依赖按名字去重/匹配要用。
# ★ 这里**故意不加 -Bsymbolic**（ld.so 自己才加）：④ 号断言要的就是"同名符号由 ld.so
#   在装载期按'主程序优先'解析" —— .so 内部对 foo_dup 的引用只有走自己的 PLT 才能被
#   解析到主程序那一份（加了 -Bsymbolic 就就地绑定、永远拿到 .so 自己的）。
$LD -m elf_x86_64 -shared --no-undefined -z noexecstack --strip-all \
    -init foo_init -soname libfoo.so \
    -T user/ldso/libfoo.ld -e foo_init -o "$OUT_DIR/libfoo.so" "$OBJ/libfoo.o"
$PY tools/dynlink_check_win.py libfoo "$OUT_DIR/libfoo.so"

echo "==> 动态主程序：user/apps/dynhello.{c,start.S} -> $OUT_DIR/dynhello.elf"
$CC $CFLAGS_APP -c user/apps/dynhello.c -o "$OBJ/dynhello.o"
# ★ 入口桩必须用**汇编**（user/apps/dynhello_start.S，4 条指令）：ld.so 是 `jmp` 到入口的，
#   此时 rsp 指向内核建的初始栈（[rsp]=argc）；C 函数的 push 序言会把 rsp 挪到 argc 槽下面，
#   于是 argc 读成野值 -> argv 越界 #PF（实测 cr2=0x90001EA20）。理由详见那个 .S 的注释。
$CC -target x86_64-linux-gnu -ffreestanding -nostdlib -fPIC \
    -c user/apps/dynhello_start.S -o "$OBJ/dynhello_start.o"

# 用 lld 的**默认脚本**（不写 -T）+ `--image-base=0x100000000`：
#   * --image-base 把整个 PIE 钉到 4GiB（内核装载区），且第一个 PT_LOAD 从 off=0 / va=4GiB
#     起、ELF 头 + 程序头表都在里面（AT_PHDR 才有值）；
#   * ★ 实测教训：用自定义链接脚本（user/ldso/ldso.ld 那种）链这个主程序时，lld 会把
#     .data.rel.ro / .init_array 输出段连着重定位一起丢掉（GLOB_DAT/JUMP_SLOT 还在，
#     RELATIVE/64/DT_INIT_ARRAY 全没了，.so 自己的 .init_array 反而正常保留）；
#     默认脚本 + --image-base 出来的四类重定位（8/6/7/1）齐全，见 dynlink_check_win.py。
#   * libfoo.so 必须**当链接输入**：foo_* 才会被当成"动态未定义符号"、生成 PLT 桩 +
#     R_X86_64_JUMP_SLOT / GOT 槽 + R_X86_64_GLOB_DAT / DT_NEEDED（只写 -z undefs 时
#     lld 把未定义符号当 0，PLT32 直接 "out of range" 报错）。
#   * -init dh_init：主程序的 DT_INIT（ld.so 必须调用它；主程序这条路径不用 .init_array，
#     原因见上面那条实测记录 —— DT_INIT_ARRAY 由 libfoo.so 负责验证）。
#   * -E（--export-dynamic）：把主程序的全局符号（dh_main / dh_init / **foo_dup**）放进
#     本程序的 .dynsym —— ④ 号断言要的就是 libfoo.so 的 PLT 槽由 ld.so 解析到主程序的
#     foo_dup（不导出 .dynsym 里就没有它，.so 会解析失败或解析到自己的那份）。
#   * MSYS2_ARG_CONV_EXCL：MSYS2 会把 `--dynamic-linker=/lib/...` 改写成 Windows 路径
#     （实测变成 C:/msys64/lib/ldvimtu.so）；排除项写 `--dynamic-linker=*` 不生效，
#     写 `--dynamic-linker`（不带 =）才生效。
MSYS2_ARG_CONV_EXCL="--dynamic-linker" \
$LD -m elf_x86_64 -pie --image-base=0x100000000 --strip-all -E \
    --dynamic-linker=/lib/ldvimtu.so -init dh_init -z noexecstack \
    -o "$OUT_DIR/dynhello.elf" "$OBJ/dynhello_start.o" "$OBJ/dynhello.o" "$OUT_DIR/libfoo.so"
$PY tools/dynlink_check_win.py dynhello "$OUT_DIR/dynhello.elf"
echo "    ldvimtu.so   = $(stat -c%s "$OUT_DIR/ldvimtu.so") B"
echo "    libfoo.so    = $(stat -c%s "$OUT_DIR/libfoo.so") B"
echo "    dynhello.elf = $(stat -c%s "$OUT_DIR/dynhello.elf") B（PT_INTERP=$( $PY tools/dynlink_check_win.py interp "$OUT_DIR/dynhello.elf" )）"
echo "    内嵌进系统内核的三份字节合计 = $(( $(stat -c%s "$OUT_DIR/ldvimtu.so") + $(stat -c%s "$OUT_DIR/libfoo.so") + $(stat -c%s "$OUT_DIR/dynhello.elf") )) B（内核余量见 build64.sh 结尾的体积检查）"
