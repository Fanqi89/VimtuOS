# VimtuOS Fonts Manifest（字体清单：文件名｜来源 URL｜版本｜协议｜SHA256）

内核**四个字体面**（见 `kernel/font.cpp`）与实际使用的源文件：

| 面 | 用途 | 源文件 | 产物（objcopy 符号） |
|----|------|--------|----------------------|
| 0 | 西文 UI（英文/数字/符号） | `Fonts/NotoSans-Regular.ttf` | `build/font_bahnschrift.ttf` |
| 1 | 中文（界面 + 常用汉字） | `Fonts-open/NotoSansSC-Regular.ttf` | `build/font_simhei.ttf` |
| 2 | 终端等宽（中英严格 1:2） | `Fonts-open/sarasa-mono-sc-regular.ttf` | `build/font_mono.ttf` |
| 3 | 缺字兜底（前三个面都没有的码点） | `Fonts-open/unifont-14.0.01.ttf` | `build/font_fallback.ttf` |

规则（与 `_otf2ttf.py` 的实现一致）：
- **字体本体不进仓**（`.gitignore` 只放行许可文件）；许可同时留一份在 `docs/fonts/`。
- **缺失/损坏 = 构建硬失败**：`_otf2ttf.py` 逐个校验"魔数 + 大小 + SHA256 + 必须有 glyf/loca（拒绝 CFF/OTTO）"，
  缺文件就按下面 URL 下载；绝不"换个字体继续构建"。
- 中文面的五个静态字重是**派生文件**（官方只发可变字体与 OTF/CFF）：`varLib.instancer` pin `wght`，
  下表的 SHA256 是实际产物（可复现）。终端面/兜底面是**静态字体**，不需要 instancer。

---

## Fonts/（西文 UI 字重）

| 文件名 | 来源 URL | 版本 | 协议 | SHA256 |
|--------|----------|------|------|--------|
| NotoSans-Regular.ttf ★参与构建 | https://github.com/notofonts/noto-fonts/raw/main/hinted/ttf/NotoSans/NotoSans-Regular.ttf | Noto Sans 2.008 | SIL OFL 1.1 | b85c38ecea8a7cfb39c24e395a4007474fa5a4fc864f6ee33309eb4948d232d5 |
| NotoSans-Thin.ttf（可选字重） | https://github.com/notofonts/noto-fonts/raw/main/hinted/ttf/NotoSans/NotoSans-Thin.ttf | Noto Sans 2.008 | SIL OFL 1.1 | 4dff742907c3816bf9c0e7ead025afc1ae8474a3315382fbf54f3217eaba3c68 |
| NotoSans-Light.ttf（可选字重） | https://github.com/notofonts/noto-fonts/raw/main/hinted/ttf/NotoSans/NotoSans-Light.ttf | Noto Sans 2.008 | SIL OFL 1.1 | 74ffdd438f2ae232371ceb00444dae93e2590d263a4ff4c5e2c8385aa8012fdb |
| NotoSans-Bold.ttf（可选字重） | https://github.com/notofonts/noto-fonts/raw/main/hinted/ttf/NotoSans/NotoSans-Bold.ttf | Noto Sans 2.008 | SIL OFL 1.1 | c976e4b1b99edc88775377fcc21692ca4bfa46b6d6ca6522bfda505b28ff9d6a |
| NotoSans-Black.ttf（可选字重） | https://github.com/notofonts/noto-fonts/raw/main/hinted/ttf/NotoSans/NotoSans-Black.ttf | Noto Sans 2.008 | SIL OFL 1.1 | 2ea74ab862678045678cd0843e12fa17e100e7ad10b20f220fea6bb7b0ae88a5 |
| OFL-NotoSans.txt（许可，进仓） | https://github.com/notofonts/noto-fonts/raw/main/LICENSE | — | SIL OFL 1.1 | 0dab92d0544f7b233403f14b84a663bdbfa746982eda629e7f4f9ffe1b036feb |

## Fonts-open/（中文 + 终端等宽 + 缺字兜底）

| 文件名 | 来源 URL | 版本 | 协议 | SHA256 |
|--------|----------|------|------|--------|
| NotoSansSC-Regular.ttf ★参与构建 | 派生：上表 VF pin `wght=400`（`py -3 _otf2ttf.py`） | Noto Sans SC 2.004-H2 | SIL OFL 1.1 | 5a461e5f078a4c07a154b5ad5f83fdf89ea9dd08861e27435ca95520056568b0 |
| NotoSansSC-Light.ttf（派生 wght=300） | 同上（VF pin 300） | Noto Sans SC 2.004-H2 | SIL OFL 1.1 | a86990e29cfd1f8f179f0946fa1f65563d51aace81df370ec21dbff6e89b0ff0 |
| NotoSansSC-Medium.ttf（派生 wght=500） | 同上（VF pin 500） | Noto Sans SC 2.004-H2 | SIL OFL 1.1 | 23216ea389f0ea6b87b1f7a063d2cb33d65aafb714f459e820ccadb2fa2f2f40 |
| NotoSansSC-Bold.ttf（派生 wght=700） | 同上（VF pin 700） | Noto Sans SC 2.004-H2 | SIL OFL 1.1 | 64eab14d1cc0c9e748abad548decdfd48417b745a2861a973317561c050229e0 |
| NotoSansSC-Black.ttf（派生 wght=900） | 同上（VF pin 900） | Noto Sans SC 2.004-H2 | SIL OFL 1.1 | 344b29ba64a9d451f6a2896da4b43ce32682c4cf1ee936e45575be5ef7f4d107 |
| NotoSansSC[wght].ttf（派生源，可变字体） | https://github.com/google/fonts/raw/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf | Noto Sans SC 2.004-H2（wght 100..900） | SIL OFL 1.1 | a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da |
| OFL-NotoSansSC.txt（许可，进仓） | https://github.com/google/fonts/raw/main/ofl/notosanssc/OFL.txt | — | SIL OFL 1.1 | 1c05c68c34f9708415aada51f17e1b0092d2cea709bf4a94cd38114f9e73d7d9 |
| sarasa-mono-sc-regular.ttf ★参与构建 | https://github.com/be5invis/Sarasa-Gothic/releases/download/v1.0.41/SarasaMonoSC-TTF-Unhinted-1.0.41.7z 里的 `SarasaMonoSC-Regular.ttf`（整包 SHA256 `6e3ac724c4bf7d099aa44a2cc24ccdd4a3234c3248b13b3a4a76d570e8c79a26`，49736292 B） | Sarasa Gothic 1.0.41（Unhinted） | SIL OFL 1.1 | fdd22c533bf15d72dbfe83f83712717ad19433a076be697aa4b15155efbea929 |
| sarasa-mono-sc-bold.ttf（可选字重） | 同上压缩包里的 `SarasaMonoSC-Bold.ttf` | Sarasa Gothic 1.0.41（Unhinted） | SIL OFL 1.1 | 5e38695212e8a4cdf80c48b4d63761ff90e91ff8758acc7427a9fa242d1fbdd9 |
| OFL-Sarasa.txt（许可，进仓） | https://github.com/be5invis/Sarasa-Gothic/raw/v1.0.41/LICENSE | Sarasa Gothic 1.0.41 | SIL OFL 1.1 | 32c932e0dbae4f6e6386964bbc2d04178707665a05ca65cf636241af13d50a53 |
| unifont-14.0.01.ttf ★参与构建 | https://unifoundry.com/pub/unifont/unifont-14.0.01/font-builds/unifont-14.0.01.ttf | GNU Unifont 14.0.01 | GPL-2.0+ with font exception / SIL OFL 1.1（双许可） | c632666c659ccdfdb841f151aa6cc48cb987e093b90806f5af3d5a4bea7c54a5 |
| UNIFONT-LICENSE.txt（许可，进仓） | https://unifoundry.com/LICENSE.txt | — | 同上（双许可） | 1e74cb82bf476843e97c2596297b04219b1a7e51f7238944a8c031cb9401fa87 |

---

## 与原始需求的差异（如实记录）

1. **目录结构**：按用户定义落地 `Fonts/`（西文）+ `Fonts-open/`（中文/等宽/兜底）+ 本文件。衬线面（Noto Serif / Chaparral）已删除。
2. **中文面静态字重**：官方（google/fonts、notofonts/noto-cjk）**只提供可变字体与 OTF/CFF 静态实例**，
   没有静态 TTF；本管线又要求 TrueType(glyf) 轮廓（`kernel/font.cpp` 的 `face_init`/`rasterize_glyph`），
   所以静态字重由官方 VF 派生（可复现，SHA256 见上表）。
3. **兜底面版本**：`unifont-17.0.06` **不存在**；且 Unifont 从 15.x 起只提供 OTF(CFF) + HEX/BDF/PCF，
   **最后一个带 TTF 构建的官方版本是 14.0.01**，故取它（TTF/glyf，BMP 覆盖 57090 字形）。
   若要 17.x 的覆盖度，需按 `.hex` 重新构造 TTF（本轮未做）。
4. **Sarasa Mono SC**：仓库源码树里**没有**预构建 TTF，只在 release 的 7z/zip 里；故记录整包 + 成员双 SHA256。
5. 之前的 `Fonts-open/NotoSerif-Regular.ttf`、`Fonts-open/NotoSans-Regular.ttf`（与 `Fonts/` 重复）已清理。

## 复核命令

```
py -3 _otf2ttf.py            # 源字体：存在就校验（魔数/大小/SHA256/glyf-loca），缺失就下载，中文面派生静态字重
py -3 _subsetsimhei.py       # face 1 子集（先跑，face 3 的码点集合要按前三面的**产物**算）
py -3 _subset_fonts.py       # face 0/2/3 子集 + 链路自检（需求表里每个码点四个面至少一个能画）
py -3 tests\fonts64_test.py  # 四面端到端验收：[FONT64] 打点 + 1:2 度量 + 兜底命中 + 终端像素
```
