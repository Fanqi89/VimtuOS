# gui_rs/assets/icons —— 【只放 SVG 矢量图标】（+ 一个真源副本）

## 目录

* `system/` —— 系统图标（**掩码**：白 + alpha，运行期按主题 Token 着色，20 个）
  `globe ethernet wifi volume volume-mute bell search close power gear lock reboot
   chevron-left chevron-right plus person usb monitor check battery`
* `apps/` —— 应用图标（掩码 + 运行期按"应用调色板亮色"着色，10 个）
  `mypc recycle terminal settings tmgr mines calc files monitor-app about`
* `apps/kaisi.png` —— **Dock 开始按钮用的真图**。仓库里的唯一真源是 `logo/kaisi.png`；
  这里放的是它的**字节副本（同源同哈希）**，只是为了让结构说明里"Dock 开始按钮的图可以放在 apps/"
  成立。`tools/make_iconpack.py` 每次构建都会核对两边 sha256，不一致直接报错，
  所以**不会出现第二份内容不同的 kaisi**。
* `LICENSE` —— Bootstrap Icons 的 MIT 许可**原文**（就是上游 `LICENSE` 文件）。
* `SOURCES.md` —— 逐文件的上游 URL + sha256（由 `tools/fetch_icons.py` 生成，可复现）。

## 许可与来源

| 套件 | 许可 | 来源 URL | 取用 |
|---|---|---|---|
| Bootstrap Icons | MIT | https://github.com/twbs/icons | 30 个 SVG（本目录全部 SVG 都来自它） |
| Material Icons | Apache-2.0 | https://github.com/google/material-design-icons | 0 个 SVG（只取了它的图标字体，见 `../fonts/`） |

禁用清单（本目录**不含**）：GPL/AGPL 图标集（如 Papirus）、需要商标授权的品牌 logo、
来源不明的"免费图标包"。

## 本仓库稳定名 -> 上游文件名

`tools/fetch_icons.py` 的 `MAP_SYSTEM` / `MAP_APPS` 是唯一映射表，例如：
`volume -> volume-up.svg`、`close -> x-lg.svg`、`reboot -> arrow-clockwise.svg`、
`mypc -> pc-display.svg`、`mines -> grid-3x3-gap.svg`、`about -> info-circle.svg`。

## 从 SVG 到屏幕（构建期，不进内核）

```
gui_rs/assets/icons/**/*.svg
        │  tools/svg2png.py        自研"仅填充路径"光栅器（4x4 超采样；本机没有 magick/rsvg-convert/inkscape/cairosvg）
        ▼
build/icons/{system,apps}/<名字>@<16|24|32|48>.png   白 + alpha 掩码
        │  tools/make_iconpack.py  打包（头 + 32B 条目表 + PNG 块 + 逐条目 CRC32）
        ▼
build/iconpack.bin  ──build64.sh dd──▶  system.img 的 LBA 7497..8008（内核区尾部固定区间）
        │  kernel/icons64.cpp      运行期读包 -> img64 解码 -> 缓存 -> 按 palette 着色上屏
        ▼
Dock / 桌面图标 / 开始菜单状态区与网格 / 面板与 toast / 设置页小图标
```

自研光栅器**支持**：`<path>/<rect>/<circle>/<ellipse>/<polygon>/<polyline>/<line>`、
`M L H V C S Q T A Z`（含相对/隐式 lineto/反射控制点）、nonzero 与 evenodd 填充规则、
`fill/fill-rule/fill-opacity/opacity`、`transform`（matrix/translate/scale/rotate/skew）。
**不支持**（遇到会明确报错，绝不静默画错）：stroke 描边图标、filter/mask/clip-path/`<use>/<text>`、
渐变/图案填充 —— 所以选图限制在纯填充路径的套件（Bootstrap Icons 全部符合）。
