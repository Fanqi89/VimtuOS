# `sdk/software-template/apps/hello-cli` —— 示例①：纯命令行程序

* `main.c` —— 源码（C + `user/lib` 自研最小 libc；目录枚举走 `include/vimtu/dir64.h` 包装）
* `Makefile` —— 构建（选项与 `user/build_user.sh` 同款；**一次出两个产物**：ELF64 与平铺 blob）
* `build.sh` —— 设好 PATH 后调 `make`（Windows + MSYS2 环境）
* `tests/hello_cli_test.py` —— 端到端验收（真 QEMU：登录 -> 开终端 -> `elfrun` -> grep 打点）

```sh
# 构建（产物 build64/sdk/hello-cli.elf / .bin）
bash sdk/software-template/apps/hello-cli/build.sh
# 自检（PT_LOAD 必须落在 4GiB..4GiB+64KiB、无 PT_INTERP/PT_DYNAMIC、blob <= 8 页）
bash sdk/software-template/apps/hello-cli/build.sh check
# 清理
bash sdk/software-template/apps/hello-cli/build.sh clean
# 验收
py -3 sdk/software-template/apps/hello-cli/tests/hello_cli_test.py
```

它演示的三件事：**①** printf 走自有 ABI `write(1)`（一行只 printf 一次，日志才不会被 `[SYSCALL]` 追踪行拆开）；
**②** `dir_open/dir_read/dir_close`(50/51/52) 实时枚举 `/`；**③** 读卷里的文件（`open`/`read`）+ 缺文件时如实报错误码。
