#!/bin/bash
# sdk/software-template/apps/hello-cli/build.sh - 一键构建 hello-cli（Windows + MSYS2 环境）
#
# 就三件事：设 PATH（clang/lld/objcopy 在 MSYS2 里）-> 进本目录 -> 跑 make。
# 之所以还要这个壳：`make` 是 **MSYS2 的 make**，而 clang/lld 在 mingw64 的 bin 下，
# 直接 `make` 会因为 PATH 缺 mingw64 而找不到编译器（该 export 与 user/build_user.sh 同款）。
#
# 用法：
#   bash sdk/software-template/apps/hello-cli/build.sh            # 构建（产物 build64/sdk/）
#   bash sdk/software-template/apps/hello-cli/build.sh clean      # 清理
set -e
cd "$(dirname "$0")"
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
exec make "$@"
