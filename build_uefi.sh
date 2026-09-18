#!/bin/bash
# 两段式 UEFI 引导：BOOTX64.EFI（极小 PE 桩） + UEFI64.BIN（平铺长模式引导器 @0x04800000）
#   ★ 引导器地址必须与 boot/efi/stub.c 的 LOADER_ADDR 一致，且**在内核 BSS 之外**
#     （内核 BSS 覆盖物理 0x28E810..0x22E9000，启动时会清零；放 8MB 会把页表清掉）
# 用法：bash build_uefi.sh [outdir]   默认 outdir=build64
set -e
cd /c/Users/fanqi/Desktop/VimtuOS/Vimtu64
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
B="${1:-build64}"
PY="${PYTHON:-python}"

mkdir -p "$B"

CFLAGS_WIN="-target x86_64-unknown-windows -ffreestanding -nostdlib -fno-stack-protector \
 -fno-builtin -fno-asynchronous-unwind-tables -fno-unwind-tables \
 -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-avx -Oz -Wall -I boot/efi"

echo "==> 1) 编译 PE 桩 BOOTX64.EFI（只做 EFI 交互，必须极小）"
clang -x c $CFLAGS_WIN -c boot/efi/stub.c -o "$B/stub.o"
nasm -f win64 boot/efi/jump64.asm -o "$B/jump64.o"
# ★ 必须 -merge:.rdata=.data：实测 EDK2 的 PE 加载器会拒绝**有独立 .rdata 段（4 段）**的镜像
#   （UEFI Shell 报 "is not an image"），合并成 3 段（.text/.data/.reloc）就能正常加载。
#   排查过程见 docs/UEFI引导说明.md：段数、体积、编译选项、重定位、FAT 全部逐一排除过。
# ★ 必须 -base:0x0：lld-link 默认给 x86_64 PE 的 ImageBase 是 0x140000000（5GB，MSVC 风格）。
#   固件（EDK2）会先按这个首选基址 ConvertPages 分配内存，512MB 的虚拟机根本没有 5GB 地址，
#   于是分配失败后**直接放弃加载该引导项**（OVMF 日志：ConvertPages: failed to find range
#   140000000 - 140003FFF → 转去 Booting EFI Internal Shell），表现就是"固件不自动引导"。
#   改成 0 之后固件会把镜像放到任意可用内存，再用 .reloc 重定位（EDK2 自己的镜像就是基址 0）。
#   诊断工具：python tools/pe_info.py build64/BOOTX64.EFI
lld-link -subsystem:efi_application -entry:efi_main -nodefaultlib -machine:x64 \
         -merge:.rdata=.data -base:0x0 \
         -out:"$B/BOOTX64.EFI" "$B/stub.o" "$B/jump64.o"
echo "    BOOTX64.EFI = $(stat -c%s "$B/BOOTX64.EFI") bytes"

echo "==> 2) 编译平铺长模式引导器 UEFI64.BIN（链接到 0x04800000 = 72MB，避开内核 BSS）"
clang -x c $CFLAGS_WIN -c boot/efi/uefi64.c -o "$B/uefi64.o"
lld-link -subsystem:native -entry:uefi_main -nodefaultlib -machine:x64 \
         -base:0x4800000 -align:0x1000 \
         -out:"$B/uefi64.efi" "$B/uefi64.o" "$B/jump64.o"
"$PY" tools/make_flat.py "$B/uefi64.efi" "$B/UEFI64.BIN"
echo "    UEFI64.BIN  = $(stat -c%s "$B/UEFI64.BIN") bytes"
