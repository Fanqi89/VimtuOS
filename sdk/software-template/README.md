# VimtuOS 软件开发模板（SDK）

> 本目录是**面向第三方开发者的软件开发模板**：把「在 VimtuOS 上写一个程序、装进系统卷、跑起来」
> 所需的最小骨架、可复制命令与验收脚本放在一起。
> **纯用户态 / 纯文档**：不碰 `kernel/**`，也不要求你重建内核。

## 目录

```
sdk/software-template/
├── README.md                 # 本文件：一页把「怎么写、怎么装、怎么验」讲完
├── apps/
│   ├── hello-cli/            # 纯命令行程序（C + user/lib 自研最小 libc）
│   └── hello-gui/            # 图形程序（/bin/wm 合成器 + shm surface + user/lib/font64.h）
├── include/vimtu/            # 供第三方参考的接口头（ABI 速查；真源在 user/lib）
├── packaging/                # VAP64 与「类 deb/tar」包骨架 + 校验脚本
├── icons/                    # 应用图标（SVG 源 + 光栅化产物说明）
└── tools/                    # 模板自带的构建/打包/校验脚本
```

## 最小上手（3 条命令）

```sh
# 1) 构建命令行示例（产物：build64/sdk/hello_cli.elf）
bash sdk/software-template/apps/hello-cli/build.sh

# 2) 打成 VAP64 包（产物：build64/sdk/hello-cli.vap）
py -3 sdk/software-template/packaging/vap64_pack.py build64/sdk/hello_cli.bin hello-cli

# 3) 装进系统卷并重建镜像
bash build64.sh --user hello-cli
```

详细的「能力边界 / 开发环境 / 三类写法 / GUI 两路 / 装卷 / 打包 / 图标 / 调试 / FAQ」
见下面各节（本文档为初版，示例与证据随提交补齐）。
