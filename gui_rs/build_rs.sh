#!/bin/bash
# gui_rs/build_rs.sh - 把 gui_rs crate 编成 gui_rs/gui_rs.o（ELF64 目标文件，可直接被 ld.lld 链接进内核）
#
# 用法：
#   bash gui_rs/build_rs.sh            # -> gui_rs/gui_rs.o
#   bash gui_rs/build_rs.sh <输出路径>  # 覆盖产物路径（build64.sh 不用，留给手工实验）
#   RUSTC64=/path/to/rustc bash gui_rs/build_rs.sh   # 手工指定 rustc
#
# 为什么不用 cargo：
#   内核构建（build64.sh）是"一条命令编一个产物"的脚本链，cargo 会引入 target 目录、
#   配置文件与网络/lock 语义；这里直接用 rustc 编**单个 crate**，产物路径可控、可复现。
#   （gui_rs/Cargo.toml 只是给 IDE / 将来切换 cargo 留的清单，构建链不用它。）
#
# ★ 必须用 rustup shim 的**绝对路径**：
#   MSYS2 的 bash 里 $HOME 往往不是 C:\Users\<你>，PATH 里还可能有别的 rustc
#   （MSYS2 自带的/其它工具链），那些没有 x86_64-unknown-none 的 `core`，
#   实测报错 `error[E0463]: can't find crate for `core``。所以下面显式找候选路径
#   并**实际验证**该 toolchain 装了 x86_64-unknown-none 目标，找不到就明确报错退出。
#
# ★ 链接相关参数（高半区内核必须的）：
#   -C panic=abort                    : 不引入 unwinding（内核里 panic = 打印 + 停机）
#   -C relocation-model=static        : 只产生静态重定位，ld.lld 直接可解
#   -C code-model=kernel              : 链接基址在 0xFFFFFFFF80...（高半区），
#                                       小代码模型会给出 R_X86_64_32 溢出错误
#   -C target-feature=-sse,-sse2      : 引导链没有打开 CR4.OSFXSR，Rust 代码里不允许
#                                       出现 xmm 指令（objdump 可核对：无 SSE 助记符）
set -uo pipefail

cd "$(dirname "$0")"
OUT="${1:-$PWD/gui_rs.o}"
case "$OUT" in
    /*) ;;                      # 已经是绝对（MSYS）路径
    *)  OUT="$PWD/$OUT" ;;
esac

# ---------- 1) 找 rustc（rustup shim 绝对路径；找不到/目标缺失都明确报错）----------
RUSTC_BIN="${RUSTC64:-}"
try_rustc() {
    local c="$1"
    [ -n "$c" ] || return 1
    [ -x "$c" ] || return 1
    # 它必须是 rustup 的 shim（能报出 toolchain 与 sysroot），且装了 x86_64-unknown-none
    local sysroot
    sysroot="$("$c" --print sysroot 2>/dev/null | tr '\\' '/')"
    [ -n "$sysroot" ] || return 1
    [ -d "$sysroot/lib/rustlib/x86_64-unknown-none" ] || return 1
    RUSTC_BIN="$c"
    return 0
}

if [ -n "$RUSTC_BIN" ]; then
    if ! try_rustc "$RUSTC_BIN"; then
        echo "ERROR: RUSTC64=$RUSTC64 不可用或该工具链没有 x86_64-unknown-none 目标" >&2
        exit 1
    fi
else
    for cand in \
        "/c/Users/${USERNAME:-fanqi}/.cargo/bin/rustc" \
        "${USERPROFILE_POSIX:-/c/Users/fanqi}/.cargo/bin/rustc" \
        "$HOME/.cargo/bin/rustc" \
        "/c/Users/fanqi/.cargo/bin/rustc"; do
        if try_rustc "$cand"; then break; fi
    done
fi

if [ -z "$RUSTC_BIN" ]; then
    echo "ERROR: 找不到可用的 rustc（需要 rustup 安装的、带 x86_64-unknown-none 目标的工具链）" >&2
    echo "  已试：\$RUSTC64、/c/Users/\${USERNAME}/.cargo/bin/rustc、\$HOME/.cargo/bin/rustc、/c/Users/fanqi/.cargo/bin/rustc" >&2
    echo "  安装：" >&2
    echo "    rustup-init.exe -y --default-host x86_64-pc-windows-gnu" >&2
    echo "    rustup target add x86_64-unknown-none" >&2
    echo "  ★ MSYS2 里 PATH 上自带的 rustc 没有 no_std 的 core，会报 E0463（别用它）。" >&2
    exit 1
fi

# ---------- 2) 编 gui_rs（rustc 直接产出 .o；失败 = 非 0 退出并打印完整命令行）----------
ARGS=(
    "$RUSTC_BIN"
    --edition 2021
    --crate-name gui_rs
    --crate-type staticlib
    --emit "obj=$OUT"
    --target x86_64-unknown-none
    -C opt-level=2
    -C panic=abort
    -C relocation-model=static
    -C code-model=kernel
    -C debuginfo=0
    -C debug-assertions=off
    -C overflow-checks=off
    -C target-feature=-sse,-sse2
    src/lib.rs
)

echo "    rustc=$("$RUSTC_BIN" --version)  [$RUSTC_BIN]"
echo "    cd $PWD && ${ARGS[*]}"

LOG="$(mktemp)"
if ! "${ARGS[@]}" 2>&1 | tee "$LOG"; then
    echo "ERROR: rustc 编译失败。完整命令行：" >&2
    echo "  cd $PWD && ${ARGS[*]}" >&2
    if grep -q 'E0463' "$LOG"; then
        echo "  ^ E0463 = 该 rustc 没有 x86_64-unknown-none 的 core；请 rustup target add x86_64-unknown-none" >&2
        echo "    并且确保用的是 rustup shim 的绝对路径（不是 MSYS2 自带的 rustc）" >&2
    fi
    rm -f "$LOG"
    exit 1
fi
rm -f "$LOG"

if [ ! -s "$OUT" ]; then
    echo "ERROR: rustc 成功退出但没有产出 $OUT" >&2
    exit 1
fi
echo "      gui_rs -> $OUT ($(stat -c%s "$OUT") bytes)"
