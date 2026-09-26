# gui_rs/assets/fonts —— 【只放图标字体】

| 文件 | 套件 | 许可 | 来源 |
|---|---|---|---|
| `MaterialIcons-Regular.ttf` | Material Icons（Google） | Apache-2.0（原文见 `LICENSE-MaterialIcons.txt`） | https://github.com/google/material-design-icons（`font/MaterialIcons-Regular.ttf`） |

**状态：待图标字体渲染支持（当前没有用上，不假装用上了）**

理由（实测口径）：64 位内核的文本渲染是 `kernel/font.cpp` 的 TrueType 字形光栅化，它按**文本码位**
取字形（`font_draw_text` / `font_utf8_decode`），没有"用私有区码位当图标画"的接口，也没有
"图标字号/线宽/着色跟随 Token"这层语义；本批的图标是**位图**路线（构建期光栅化成 PNG，运行期
`kernel/icons64.cpp` 解码 + 上屏）。

要把这个字体真正用起来，需要另外一块工作（本批未做）：
1. `font.cpp` 增加"图标字体面"注册（把 ttf 作为第 5 个 face 装进内核）；
2. 提供一个 `iconfont_draw(cp, x, y, size, color)` 的接口（复用现有字形光栅化 + 缓存）；
3. 字体进内核会占体积（本 ttf ≈ 349KB，内核区仅剩 ~390KB 余量）—— 更合适的是走
   "图标字体放 VimtuFS2 / 图标包"，运行期从盘上读，而不是嵌进内核。

文字字体（Noto Sans 等）按需求放在仓库根目录 `Fonts/`，不在本目录。
