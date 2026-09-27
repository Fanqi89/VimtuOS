#!/bin/bash
# tools/musl_build_win.sh - ★ A3：用 **musl**（third_party/musl）构建 Vimtu64 的静态用户程序
#
# 为什么单开一个脚本（不塞进 user/build_user.sh）：
#   user/build_user.sh 用的是**我们自研的最小 libc**（user/lib，自有 ABI + int 0x80），它的
#   `--all` 会把 user/apps/*.c 全部用那套头文件编一遍 —— 而 user/apps/muslhello.c 是
#   **musl 程序**（musl 的头文件 + musl 的 libc.a + 自写 _start），两者不能混。
#
# 产出（默认）：
#   build64/musl_hello.elf   静态 ELF64 EXEC（链接在用户窗口 4GiB 起，无 PT_INTERP / 无重定位）
# 由 build64.sh 在"用户态程序"段调用；产出的 ELF 由 objcopy 内嵌进**系统内核**
# （objcopy 的符号名：_binary_build64_musl_hello_elf_start/_end），启动期由内核把它
# 幂等装进 VimtuFS2（/musl_hello.elf）再从文件系统读出来、作为**真进程**进 ring3。
#
# ==================== SSE：musl 用，内核在 ring3 入口打开 ====================
#   musl 的默认静态构建里**大量使用 SSE**（clang 自动向量化出来的 movaps/xorps/movups：
#   __init_libc 清 auxv 数组、mallocng 清元数据、open 保存 xmm……）。VimtuOS 原来的长模式
#   入口只设了 CR4.PAE（boot/loader64.asm），CR4.OSFXSR=0 时 ring3 任何 SSE 指令 = #UD ——
#   实测症状：musl 程序跑到 malloc 就 `[PANIC] cpu exception 6`，rip 指向一条 movaps。
#   本轮的处理（见 kernel/usermode64.cpp 的 u64_fpu_enable64）：
#     * 进 ring3 之前打开 CR4.OSFXSR|OSXMMEXCPT、清 CR0.EM/TS，并打一行
#       `[USER64] fpu sse=1 cr4=… cr0=…` 留证据；
#     * **没有** FPU/xmm 上下文切换（switch64.asm 不保存 xmm）——只保证单进程内 SSE 状态
#       连续；多进程同时用 SSE 会互相污染。这是本轮如实标注的边界（docs 里有整段说明）。
#   本脚本因此**不再**要求 libc.a 是"无 SSE"版本（那样会撞上 musl 的 math/x86_64/fma.c：
#   `-mno-sse` 直接编不过："SSE register return with SSE disabled"）；我们自己的程序
#   （user/apps/muslhello.c）仍然用 -mno-sse* 编 —— 它不需要 SSE，少一个变量。
#
# 工具链与路径（都在本文件里显式写出，方便复现）：
#   clang --target=x86_64-linux-gnu（MSYS2 mingw64 自带 clang 22.x）
#   ld.lld（lld）
#   musl 头文件：third_party/musl/{include, arch/x86_64, arch/generic, obj/include}
#   musl 静态库：third_party/musl/lib/libc.a（A2 的产物；缺失时本脚本会用 make +
#     tools/musl_archive_win.py 重建 —— Windows 上最后一步归档必定因命令行过长失败，
#     所以归档交给那个脚本，复现命令见 README.vimtu64-a2.md）
#
# 链接脚本用 tools/musl_hello64.ld（不是 user/hello_elf64.ld —— 那个会丢掉 .got，
#   musl 的 exit.o 有 GOTPCRELX 引用，丢了直接链接失败；详见链接脚本头部）。
#   它同样把映像钉在 0x0000000100000000、并让 ELF 头 + 程序头表落在第一个 PT_LOAD 里
#   （`SIZEOF_HEADERS` 技巧）—— auxv 的 AT_PHDR 因此才有值（musl 的 __init_libc/
#   static_init_tls 要按 AT_PHDR/AT_PHENT/AT_PHNUM 遍历程序头表找 PT_TLS）。
#
# 硬约束（本脚本提前拦住，别让内核在 ring3 前才报错）：
#   1) 所有 PT_LOAD 必须落在 4GiB..4GiB+64KiB（= kernel/usermode64.h 的装载区，
#      上界是 USER64_STACK_VA64；elf64.cpp 会拒绝越界的段）；
#   2) 无 PT_INTERP / 无 PT_DYNAMIC（内核只做静态装载，不做重定位）；
#   3) 入口落在某个 PT_LOAD 内、程序头表 ≤ 16 项（ELF64_MAX_PHDR64）、且程序头表在第一个
#      PT_LOAD 里（否则 auxv 的 AT_PHDR 会是 0，musl 启动期会读崩）。
set -e

cd "$(dirname "$0")/.."                       # 仓库根（与 build64.sh 同一口径）
ROOT="$(pwd)"
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

MUSL_DIR="${MUSL_DIR:-$ROOT/third_party/musl}"
OUT="${1:-$ROOT/build64/musl_hello.elf}"
SRC="${2:-$ROOT/user/apps/muslhello.c}"
LDSCRIPT="${3:-$ROOT/tools/musl_hello64.ld}"
OBJ="${MUSL_BUILD_OBJ:-$(dirname "$OUT")/musl_build}"

# 我们自己这一份用 -mno-sse*（见文件头）：它不需要 SSE，链接器/内核都少一个变量。
NO_SSE_FLAGS="-mno-sse -mno-sse2 -mno-mmx -mno-avx"

# python 入口：优先 py -3（Windows 原生），退回 python（MSYS2）
if command -v py >/dev/null 2>&1; then PY="py -3"; else PY="${PYTHON:-python}"; fi

if [ ! -f "$SRC" ]; then echo "找不到 musl 程序源码：$SRC" >&2; exit 2; fi
if [ ! -f "$LDSCRIPT" ]; then echo "找不到链接脚本：$LDSCRIPT" >&2; exit 2; fi
if [ ! -f "$MUSL_DIR/include/stdio.h" ]; then echo "找不到 musl 源码树：$MUSL_DIR" >&2; exit 2; fi

LIBC_A="$MUSL_DIR/lib/libc.a"
if [ ! -f "$LIBC_A" ]; then
    echo "==> 缺 musl 静态库，按 A2 的配方重建（make + tools/musl_archive_win.py）"
    echo "    注意：Windows 上 make 的最后一步归档必定失败（命令行过长），这是已知现象；"
    echo "    目标文件在那一步之前全部编完，归档交给 tools/musl_archive_win.py。"
    mkdir -p "$MUSL_DIR/obj"
    ( cd "$MUSL_DIR" && make -j6 lib/libc.a >/dev/null 2>&1 || true )
    ( cd "$MUSL_DIR" && $PY tools/musl_archive_win.py )
fi
MEMBERS=$(ar t "$LIBC_A" 2>/dev/null | wc -l)
SIZE=$(stat -c%s "$LIBC_A" 2>/dev/null || echo 0)
if [ "${MEMBERS:-0}" -lt 1000 ] || [ "$SIZE" -lt 1500000 ]; then
    echo "ERROR: musl libc.a 不完整（成员=$MEMBERS 大小=$SIZE）——手动跑下面这条看错误：" >&2
    echo "       cd third_party/musl && make -j6 lib/libc.a && py -3 tools/musl_archive_win.py" >&2
    exit 2
fi
echo "    musl 静态库：$LIBC_A（成员=$MEMBERS 大小=$SIZE）"

mkdir -p "$OBJ" "$(dirname "$OUT")"

echo "==> musl 用户程序：$SRC -> $OUT"
echo "    头文件：$MUSL_DIR/{include,arch/x86_64,arch/generic,obj/include}"
clang --target=x86_64-linux-gnu -nostdinc \
      -isystem "$MUSL_DIR/include" -isystem "$MUSL_DIR/arch/x86_64" -isystem "$MUSL_DIR/arch/generic" -isystem "$MUSL_DIR/obj/include" \
      -O2 -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
      -D_XOPEN_SOURCE=700 -mcmodel=large $NO_SSE_FLAGS \
      -ffunction-sections -fdata-sections \
      -Wall -Wextra -std=c11 \
      -c "$SRC" -o "$OBJ/muslhello.o"

# --gc-sections：只把真正用到的 musl 目标文件链进来（libc.a 里 1300+ 个成员，全链会撑爆装载区）
ld.lld -m elf_x86_64 -static --gc-sections -z noexecstack \
       -T "$LDSCRIPT" -o "$OUT" "$OBJ/muslhello.o" "$LIBC_A"

# ---- 内核装载路径的硬约束自检（纯 Python 解析 ELF 头/程序头，不依赖 binutils）----
$PY - "$OUT" <<'PYEOF'
import struct, sys
path = sys.argv[1]
d = open(path, "rb").read()
assert d[:4] == b"\x7fELF", "不是 ELF"
assert d[4] == 2 and d[5] == 1, "不是 ELF64 小端"
etype, machine = struct.unpack_from("<HH", d, 16)
assert etype in (2, 3), "e_type 必须是 ET_EXEC/ET_DYN"
assert machine == 0x3E, "e_machine 必须是 x86_64"
entry = struct.unpack_from("<Q", d, 24)[0]
phoff = struct.unpack_from("<Q", d, 32)[0]
phentsize, phnum = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
assert phentsize == 56, "e_phentsize 必须是 56"
assert 0 < phnum <= 16, "e_phnum 必须 <= 16（kernel/elf64.h 的 ELF64_MAX_PHDR64）"
LO, HI = 0x100000000, 0x100000000 + 0x10000          # 4GiB..4GiB+64KiB（USER64_STACK_VA64 之前）
segs, nload = [], 0
first_off = first_filesz = None
for i in range(phnum):
    p = phoff + i * phentsize
    ptype = struct.unpack_from("<I", d, p)[0]
    poff, pva, _ppa, pfsz, pmsz, _pa = struct.unpack_from("<QQQQQQ", d, p + 8)
    if ptype == 3:
        raise SystemExit("ERROR: 出现 PT_INTERP（内核只做静态装载，没有动态链接器）")
    if ptype == 2:
        raise SystemExit("ERROR: 出现 PT_DYNAMIC（静态链接不该有）")
    if ptype != 1:
        continue
    nload += 1
    if first_off is None:
        first_off, first_filesz = poff, pfsz
    assert pmsz >= pfsz, "p_memsz < p_filesz"
    assert poff + pfsz <= len(d), "段文件范围越界"
    assert pva >= LO and pva + pmsz <= HI, "PT_LOAD 越出装载区 4GiB..4GiB+64KiB（va=0x%x memsz=0x%x）" % (pva, pmsz)
    assert (pva + pmsz + 0xFFF) & ~0xFFF <= HI, "PT_LOAD 页对齐后压到用户栈区"
    segs.append((pva, pmsz))
assert nload > 0, "没有 PT_LOAD"
assert any(va <= entry < va + msz for va, msz in segs), "入口不在任何 PT_LOAD 内"
# 程序头表必须在第一个 PT_LOAD 里（否则内核算不出 AT_PHDR，musl 的 __init_tls 会读崩）
assert first_off == 0 and phoff + phnum * phentsize <= first_filesz, \
    "程序头表不在第一个 PT_LOAD 内（auxv AT_PHDR 会是 0）"
print("    musl ELF 自检 OK：entry=0x%x phnum=%d segs=%d size=%d B span=0x%x..0x%x" %
      (entry, phnum, nload, len(d), segs[0][0], segs[-1][0] + segs[-1][1]))
PYEOF

# ---- 记录 SSE/AVX 指令条数（**不是**错误：ring3 入口已开 CR4.OSFXSR，见文件头与
#      kernel/usermode64.cpp 的 u64_fpu_enable64）----
if command -v objdump >/dev/null 2>&1; then
    SSE_N=$(objdump -d "$OUT" | grep -c -E "(xmm|ymm|zmm)" || true)
    echo "    SSE/AVX 指令：${SSE_N:-0} 条（来自 musl 的静态库；ring3 已开 CR4.OSFXSR/OSXMMEXCPT，可正常执行）"
else
    echo "    [!] 找不到 objdump：跳过 SSE/AVX 指令计数（只是计数，不影响构建）" >&2
fi

echo "    musl 程序 = $(stat -c%s "$OUT") B（内核装载区上限 64KiB；objcopy 后内嵌进系统内核）"
