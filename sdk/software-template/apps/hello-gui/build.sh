#!/bin/bash
# sdk/software-template/apps/hello-gui/build.sh - 一键构建 hello-gui（Windows + MSYS2 环境）
#
# 用法：
#   bash sdk/software-template/apps/hello-gui/build.sh            # 构建（产物 build64/sdk/）
#   bash sdk/software-template/apps/hello-gui/build.sh clean      # 清理
set -e
cd "$(dirname "$0")"
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
exec make "$@"
