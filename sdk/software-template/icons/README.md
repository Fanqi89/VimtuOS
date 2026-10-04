# `sdk/software-template/icons` —— 应用图标规范与示例

**图标真源在 `gui_rs/assets/`**（应用也放那儿，内核/桌面走同一套查找路径）；本目录只给一份"自绘示例图标" +
一页规范，方便你从零做一个自己的应用图标。

## 一页规范

| 项 | 要求 |
|---|---|
| 目录 | 系统/状态类 → `gui_rs/assets/icons/system/`；应用类 → `gui_rs/assets/icons/apps/`；位图素材 → `gui_rs/assets/images/{wallpapers,avatars}/` |
| 源格式 | **SVG 可以放**（本仓库自研光栅器 `tools/svg2png.py` 支持**纯 fill 路径**；`stroke`/`filter`/`mask`/`<use>`/`<text>`/渐变会**明确报错**） |
| 运行期格式 | 内核只解 **PNG / BMP / JPEG**（JPEG 是**基线**解码：SOF0/SOF1、4:4:4/4:2:2/4:2:0、RSTn、EXIF Orientation；渐进式/算术编码明确拒绝） |
| 尺寸 | **16 / 24 / 32 / 48 px**（系统图标按 24 取、应用图标按 48 取；找不到大档会**只缩小不放大**） |
| 形态 | 图标是**掩码**：白 + alpha；颜色由运行期主题/应用调色板决定（不要烤死颜色） |
| 命名 | 小写 + 连字符（`folder-plus.svg`、`battery-charging.svg`）；卷内路径 `/icons/<system|apps>/<名字>@<尺寸>.png` |
| 许可 | **只收 MIT / ISC / Apache-2.0 / CC0**；落盘许可原文 + 在 `gui_rs/assets/icons/SOURCES.md` 记 `URL + sha256`（禁用 GPL/AGPL 套件、商标 logo、来源不明素材） |
| 示例图标 | 本目录的 `app-template.svg` 是**自绘**的（CC0-1.0，无第三方素材），可以直接改成你自己的 |

## 装进系统卷（外置，不进内核）

```sh
py -3 sdk/software-template/tools/iconpack_ext.py \
      --vol-in build64/shellvol.img --vol-out build64/sdk/iconvol.img \
      --system build64/system.img --disk build64/sdk/icondisk.img
```

内核侧的优先级（真源 `kernel/icons64.cpp:463-475`）：**卷里的 `/icons/<sub>/<名字>@<尺寸>.png` 优先于图标包**，
打点 `[ICON64] load kind=<名字> path=/icons/… src=vfs ok=1`。

★ **如实边界**：内核只主动请求它自己那张表里的 30 个 kind（系统 20 + 应用 10，`kernel/icons64.cpp:47-63`）。
**同名覆盖**立刻生效；**新名字**（`folder`、`copy`、`paste`…）目前是把文件装进卷留档，等内核侧登记对应 kind
才会被请求（本模板不改 `kernel/**`）。当前已入库的图标清单与逐文件 sha256 见 `gui_rs/assets/icons/SOURCES.md`。
