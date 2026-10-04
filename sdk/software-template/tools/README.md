# `sdk/software-template/tools` —— 构建自检 / 图标外置 / 验收基座

| 文件 | 干什么 |
|---|---|
| `elf64_check.py` | 构建期自检：ELF64 能被 VimtuOS 装载吗（`PT_LOAD` 在 4 GiB..4 GiB+64 KiB、无 `PT_INTERP`/`PT_DYNAMIC`、程序头表在首个 `PT_LOAD`、入口在段内、≤96 KiB）+ blob ≤ 32768 B（8 页） |
| `iconpack_ext.py` | 把 `gui_rs/assets/icons/**.svg` 光栅化成 16/24/32/48 PNG，并按内核的查找路径 **外置进系统卷** `/icons/{system,apps}/<名字>@<尺寸>.png`（附 `/etc/icons_ext.json` 逐文件 sha256） |
| `sdk_qemu.py` | 验收基座：造夹具盘（system.img + VimtuFS2 卷 + MBR）、起 QEMU（`-serial file:` + telnet monitor）、登录手势、打字、抓屏、像素统计 |

```sh
py -3 sdk/software-template/tools/elf64_check.py build64/sdk/hello-cli.elf build64/sdk/hello-cli.bin
py -3 sdk/software-template/tools/iconpack_ext.py --only folder,copy,paste --no-vol     # 只光栅化
py -3 sdk/software-template/tools/iconpack_ext.py --vol-in build64/shellvol.img \
      --vol-out build64/sdk/iconvol.img --system build64/system.img --disk build64/sdk/icondisk.img
```

**纪律**：`sdk_qemu.py` 不加锁 —— **同一时刻只跑一个 QEMU 重活**；`vm.close()` 必须在 `finally` 里（别把 VM 挂着）。
