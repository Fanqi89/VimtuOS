# `sdk/software-template/packaging` —— 打包 / 装卷 / 校验

| 文件 | 干什么 | 关键点 |
|---|---|---|
| `vap64_pack.py` | 把**平铺二进制**打成 VAP64 应用包；`--verify` 独立复验 | 打包复用真源 `tools/make_vap.py`；**校验用本脚本自己的解析器**（两条路，互不背书） |
| `vtar64_pack.py` | "类 deb/tar"骨架：程序 + 图标 + 元数据 + `SHA256SUMS` 打成一个文件；`--list/--verify/--extract` | 逐条 CRC32 + **整包 sha256**；**不是** dpkg/apt，没有依赖/脚本/签名 |
| `vol_install.py` | 把文件写进 **VimtuFS2 系统卷**（可再拼成可引导盘）；`--check` 独立回读校验 | 复用 `tools/tcc_pack_win.VolumeEdit` 与 `tools/make_shellvol.build_disk`，逐步自证 |

```sh
# VAP64
py -3 sdk/software-template/packaging/vap64_pack.py build64/sdk/hello-cli.bin build64/sdk/hello-cli.vap hello-cli
py -3 sdk/software-template/packaging/vap64_pack.py --verify build64/sdk/hello-cli.vap

# 类 deb/tar 骨架
py -3 sdk/software-template/packaging/vtar64_pack.py -o build64/sdk/hello-cli.vtar64 \
      --add 0755:bin/hello-cli.elf=build64/sdk/hello-cli.elf
py -3 sdk/software-template/packaging/vtar64_pack.py --verify build64/sdk/hello-cli.vtar64

# 装卷 + 拼盘
py -3 sdk/software-template/packaging/vol_install.py \
      --vol-in build64/shellvol.img --vol-out build64/sdk/sdkvol.img \
      --src build64/sdk/hello-cli.elf:/bin/hello-cli.elf:0755 \
      --system build64/system.img --disk build64/sdk/sdkdisk.img

# 只回读校验（独立按卷格式解析）
py -3 sdk/software-template/packaging/vol_install.py --check build64/sdk/sdkvol.img \
      --expect /bin/hello-cli.elf=build64/sdk/hello-cli.elf
```

★ **如实标注：VimtuOS 目前没有包管理器，也没有"应用商店"**（`docs/应用层与系统调用说明.md` →
`## 附 A：update / preload / proc 终端命令` 里也写明 `update` 只是"标记 -> 应用 -> 重启"的机制闭环）。
这里给的是**可校验的分发骨架**：装进卷那一步永远是 `vol_install.py`（或仓库既有的 `tools/*_pack_win.py`）。

安装位置（约定）与模式见上级 `README.md` §7.3。
