# gui_rs/assets —— 专属静态资源总目录

按要求的结构落地（**只放静态资源，不放代码**）：

```
gui_rs/assets/
├── fonts/                 # 【只放图标字体】(.ttf/.otf)；文本字体在仓库根目录 Fonts/
│   ├── MaterialIcons-Regular.ttf      # Material Icons（Apache-2.0）—— 图标字体，**待图标字体渲染支持**
│   └── LICENSE-MaterialIcons.txt      # Apache-2.0 许可原文
├── icons/                 # 【只放 SVG 矢量图标】
│   ├── LICENSE                        # Bootstrap Icons 的 MIT 许可原文
│   ├── SOURCES.md                     # 逐文件来源 URL + sha256（tools/fetch_icons.py 生成）
│   ├── README.md                      # 命名/映射/生成流程说明
│   ├── system/            # 系统图标 wifi.svg / volume.svg / ethernet.svg / bell.svg …（20 个）
│   └── apps/              # 软件图标 terminal.svg / mypc.svg / recycle.svg / calc.svg …（10 个）
│                          #   ★ kaisi.png = 仓库唯一真源 logo/kaisi.png 的**同源同哈希字节副本**
│                          #     （结构要求开始按钮的图在 apps/ 下；tools/make_iconpack.py 每次构建都校验 sha256）
└── images/
    ├── LICENSE                        # CC0-1.0（本仓库自绘，无第三方素材）
    ├── wallpapers/        # 壁纸（vimtu-gradient-320x200.png，自绘样例；内核壁纸真源仍是 gfx64 程序化壁纸）
    └── avatars/           # 头像（vimtu-user-64.png，自绘样例；锁屏/开始菜单头像真源仍是 userdb64）
```

## 这些资源怎么进系统（**不进内核二进制**）

| 资源 | 构建期处理 | 运行期读取 |
|---|---|---|
| `icons/**/*.svg` | `tools/svg2png.py`（自研"仅填充路径"光栅器）→ 16/24/32/48 px PNG（白 + alpha 掩码）→ `tools/make_iconpack.py` 打包 `build/iconpack.bin` | `kernel/icons64.cpp` 从系统镜像**内核区尾部固定区间**（LBA 7497，`ICON64_PACK_LBA`）读包 → `img64_decode64` 解码 → 按主题 palette 着色上屏 |
| `icons/apps/kaisi.png` | 与 `logo/kaisi.png` 同哈希，仅供结构/留档；内核里嵌的是 `logo/kaisi.png` 的原始字节 | `[DOCK64] start icon src=vfs:/logo/kaisi.png`（VimtuFS2 优先） |
| `fonts/*.ttf` | **不参与**构建（当前内核没有图标字体渲染能力） | 暂无（见 `fonts/README.md`） |
| `images/**` | 不参与构建（样例素材） | 暂无（壁纸/头像真源见上表说明） |

可选覆盖：系统卷里若存在 `/icons/system/<名字>@<尺寸>.png`（或 `/icons/apps/…`），
它**优先于**图标包，打点变成 `src=vfs` —— 装好的盘上换图标不用改内核。

## 许可清单（全部宽松许可，无 GPL/AGPL、无商标素材）

| 套件 | 许可 | 来源 | 落盘许可原文 | 实际取用 |
|---|---|---|---|---|
| Bootstrap Icons | MIT | https://github.com/twbs/icons | `icons/LICENSE` | 30 个 SVG（system 20 + apps 10） |
| Material Icons | Apache-2.0 | https://github.com/google/material-design-icons | `fonts/LICENSE-MaterialIcons.txt` | 1 个图标字体 ttf（0 个 SVG） |
| 自绘壁纸/头像 | CC0-1.0（本仓库声明） | 本仓库 `tools/make_asset_images.py` | `images/LICENSE` | 2 个 PNG |
