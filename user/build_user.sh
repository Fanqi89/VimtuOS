#!/bin/bash
# build_user.sh - Vimtu64 用户态 C 程序交叉编译（★ A2：我们自己的最小 libc + int 0x80 自有 ABI）
#
# 用法：
#   bash user/build_user.sh <name> [outdir]     # 编 user/apps/<name>.c -> <outdir>/user_<name>.elf/.bin
#   bash user/build_user.sh --all  [outdir]     # 编 user/apps/*.c（每个都出一份）
#   outdir 缺省 = build64（仓库根的构建目录）
#
# 等价入口（build64.sh 里的同一个脚本，见其"用户程序交叉编译"段）：
#   bash build64.sh --user <name>
#
# 工具链（MSYS2 自带；实测 clang 22.1.8 / ld.lld 22.1.8）：
#   C:\msys64\mingw64\bin\clang.exe、ld.lld.exe、objcopy.exe（usr/bin）
#
# 编译选项逐条理由：
#   --target=x86_64-unknown-none-elf  裸机目标（不引入任何宿主 CRT / 头 / 库）
#   -nostdinc -I user/lib             ★ 只用我们自己的头文件：**零外部依赖**（连 <stdint.h>
#                                     都用 user/lib 里那份，编译器的资源头一个都不碰）
#   -ffreestanding -nostdlib -fno-builtin
#   -mcmodel=large                    ★ 必须：程序链接在 4GiB（= kernel/usermode64.h 的
#                                     USER64_CODE_VA64）。small 模型对全局变量生成 R_X86_64_32S
#                                     绝对重定位，lld 会直接报 "relocation R_X86_64_32S out of range:
#                                     4294971392 is not in [...]"（本批次实测遇到过）；large 模型
#                                     全用 64 位绝对地址（movabs），平铺 blob 拷到链接地址即可正确寻址
#   -fno-pic -fno-pie                 不要 GOT/PLT（平铺 blob 没有动态链接器）
#   -fno-stack-protector -mno-red-zone  裸机：没有 %fs 的栈保护、中断不用红区
#   -mno-sse -mno-sse2 -mno-mmx -mno-avx  ★ 内核与用户态都不开 FP/SSE；别让编译器偷偷用 xmm
#   -ffunction-sections -fdata-sections + --gc-sections  只把用到的函数链进 blob
#                                     （hello.c 不该带上 fbdemo 的绘制代码，内核镜像才省得下）
#   -O2 -std=c11 -Wall -Wextra        ★ -Wextra 零告警（本批次的交付要求）
# 产出：
#   <outdir>/user_<name>.elf   静态 ELF64（链接地址 4GiB，可 readelf/objdump 核对）
#   <outdir>/user_<name>.bin   **平铺 blob**（objcopy -O binary）= 内核里 objcopy 内嵌的那份
# 体积红线：blob 必须 <= 32768 字节（8 页）—— kernel/usermode64.cpp 的 blob 加载器上限，
#   超了它会在 ring3 之前拒绝启动（[USER64] run FAILED (blob too big)），这里提前拦住。
set -e

cd "$(dirname "$0")/.."                     # 仓库根
ROOT="$(pwd)"
LIBSRC_NAMES="syscall.c string.c stdlib.c stdio.c fb.c"
LIB_S="crt0.S syscall.S"
LDSCRIPT="user/lib/user64.ld"
MAX_BLOB=$((8 * 4096))

if [ "${1:-}" = "--all" ]; then
    OUT="${2:-build64}"
    for f in user/apps/*.c; do
        bash user/build_user.sh "$(basename "${f%.c}")" "$OUT"
    done
    exit 0
fi

NAME="${1:-}"
OUT="${2:-build64}"
if [ -z "$NAME" ]; then
    echo "用法：bash user/build_user.sh <name> [outdir]   或   bash user/build_user.sh --all [outdir]" >&2
    exit 2
fi
SRC="user/apps/$NAME.c"
if [ ! -f "$SRC" ]; then
    echo "找不到用户程序源码：$SRC" >&2
    exit 2
fi

export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
CC="clang --target=x86_64-unknown-none-elf"
LD=ld.lld
OBJCOPY="${OBJCOPY:-objcopy.exe}"

UCFLAGS="--target=x86_64-unknown-none-elf -nostdinc -I user/lib \
 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
 -fno-zero-initialized-in-bss -mcmodel=large -mno-red-zone \
 -mno-sse -mno-sse2 -mno-mmx -mno-avx -fno-asynchronous-unwind-tables -fno-unwind-tables \
 -ffunction-sections -fdata-sections -std=c11 -O2 -Wall -Wextra"
ASFLAGS="--target=x86_64-unknown-none-elf -nostdinc -ffreestanding -fno-pic -fno-pie \
 -ffunction-sections -fdata-sections -Wall -Wextra"

mkdir -p "$OUT" "$OUT/uapps/$NAME"
OBJ="$OUT/uapps/$NAME"
LIBOBJS=""

# ---- 1) 运行时（user/lib/*.c + *.S）----
for src in $LIBSRC_NAMES; do
    o="$OBJ/lib_${src%.c}.o"
    $CC $UCFLAGS -c "user/lib/$src" -o "$o"
    LIBOBJS="$LIBOBJS $o"
done
for src in $LIB_S; do
    o="$OBJ/lib_${src%.S}_asm.o"
    $CC $ASFLAGS -c "user/lib/$src" -o "$o"
    LIBOBJS="$LIBOBJS $o"
done

# ---- 2) 应用程序 ----
$CC $UCFLAGS -c "$SRC" -o "$OBJ/app_$NAME.o"

# ---- 3) 链接（平铺基址 4GiB，见 user/lib/user64.ld）+ gc-sections ----
$LD -m elf_x86_64 -T "$LDSCRIPT" --gc-sections -o "$OUT/user_$NAME.elf" \
    $LIBOBJS "$OBJ/app_$NAME.o"

# ---- 4) 平铺 blob（.image_end 哨兵保证 .bss 的零也被算进长度，见链接脚本）----
$OBJCOPY -O binary "$OUT/user_$NAME.elf" "$OUT/user_$NAME.bin"

BLOB_SZ=$(stat -c%s "$OUT/user_$NAME.bin")
ELF_SZ=$(stat -c%s "$OUT/user_$NAME.elf")
echo "    user/$NAME.c -> $OUT/user_$NAME.elf ($ELF_SZ B) / $OUT/user_$NAME.bin (blob $BLOB_SZ B)"
if [ "$BLOB_SZ" -gt "$MAX_BLOB" ]; then
    echo "ERROR: $NAME 的 blob $BLOB_SZ B 超过加载器上限 $MAX_BLOB B（8 页）" >&2
    exit 1
fi
