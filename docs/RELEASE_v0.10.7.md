> [← 返回主页](../README.md) · VimtuOS 文档体系

# v0.10.7 — 终端改回抗锯齿（细笔画不再消失）+ 4 个红项清零

> ✅ **已发版**（`VIMTUOS_VERSION = "0.10.7"`，全量构建 `EXIT=0`）。

发布：<https://github.com/Fanqi89/VimtuOS/releases/tag/v0.10.7>
日期：2026-11-16
上一版：v0.10.6（拖拽残影根治 + 用户壁纸入库）

---

## 1. 一句话

**终端不再"非黑即白"。** v0.4.7 起终端走"三无"单色（覆盖率过阈值才点亮），代价是
**1px 细笔画被判死**：`H` 左边那一竖在 16px 下覆盖率只有 30~45%，整条消失 —— 屏幕上
`H` 看起来像 `N`，日志没法读。本版把终端改回**灰阶抗锯齿**（方案 B），细笔画显示为浅色而不是消失；
同时把 AA 混合做成**按行直写**（无除法、无逐像素函数调用），纯 CPU 渲染下不掉帧。

---

## 2. 终端抗锯齿（方案 B）

### 2.1 改了什么

`kernel/terminal64.cpp` 的 `term_draw_cell()`：

```diff
-    font_set_mono64(1);                              // 三无：单色掩码
-    font_set_mono_thresh64(TERM_MONO_THRESHOLD_ASCII);  // ASCII 128 / CJK 96
-    font_draw_glyph(...);  /  font_draw_glyph_cp(...);
-    font_set_mono64(0);
+    font_select(FONT_FACE_MONO);           // 字体面/字号/基线补偿一律不变（等宽面 16px、中英 1:2）
+    font_draw_glyph(...);  /  font_draw_glyph_cp(...);
```

`font_set_mono64()` / `font_set_mono_thresh64()` 这两个 API **保留**（外部仍可强制单色），
但字形贴图不再读它 —— 覆盖率一律按 alpha 与底色混合。

### 2.2 为什么这样就"不再丢笔画"

单色是 `a >= 阈值 ? 前景色 : 不画`；抗锯齿是 `result = fg*a + bg*(1-a)`。
40% 覆盖率的竖笔在单色下等于"整条不点亮"，在 AA 下是 **40% 亮度的浅色线**——笔画在，只是淡。

### 2.3 性能：按行直写 + 无除法

AA 路径原来每像素要跨文件调 `fb_get_pixel()` + `fb_putpixel()`，两次边界/裁剪判断、
两次 `y*stride + x` 乘法，再加 3 次 `/255`。终端改 AA 后一屏字符重绘要走上几十万次。

本版把它压到"每行一次"：

* `kernel/fb.h/.cpp` 新增 **`fb_row64(x0, y, x1, &rx0, &rx1)`**：把边界 + 裁剪 + `y*stride`
  在**一行上只算一次**，返回该行可写区间起点；
* `kernel/font.cpp` 的两条字形贴图路径（缓存路径 + LRU 路径）改成
  `dst = fb_row64(...)` → 行内 `blend_px()` **直写**（不再跨文件调用、不再重复乘法）；
* 混合数学去掉除法：`t/255` → `(t*257+257)>>16`（对 t ∈ [0,65025] **逐值相等**，
  已用脚本全量核对 mismatch=0）。

`draw_blend()` 已删除（两条路径都不再用它，避免 `-Wunused-function`）。

---

## 3. 其余 3 个红项（全部是"判据口径过期"，判据都变严）

| 测试 | 旧红 | 真因 | 改法 |
|---|---|---|---|
| `desktopops64_test` | 69/70 | **不是数据被改写**：§3 复位写 `0,1,2\|` → §6 把"终端"图标**拖进回收站**（`[DESK64] drag to recycle idx=2 kind=2`）→ 写 `0,1\|2` → 重启 load `0,1\|2`、`[DESK64] init items=2 set=0,1\|2 recycle=1`，**完全一致**。测试把 §3 的值写死了 | 期望改成"重启前**最后一次** set 的值"（自证、不写死）+ 新增 `[DESK64] init` 的 items/set/recycle 一致性断言 |
| `wl64_test` | 81/85 | 内核 `[WL64] seat event` 是**节流**的（前 64 条全打、之后每 32 条一条）：注入 38 条日志只剩 24 条 → `surf 分布=[1]`、键盘 n=0、`types=[3]` 全是**证据被吞**；`seat_supp` 还是**被节流的日志行数**，不是丢事件数 | 四条判据改以**客户端** `[WLCLIENT] seat`（逐条无节流）为准、内核行兜底；最后一条换成端到端对账（客户端条数 <= 内核 seatpost 总数 logged+supp，且 >= 4） |
| `font64user_test` | 58/65 | ①`[WL64] wm skipped` 是 v0.5.x 前的行为，现在是**回落** `/lib/wm.elf`；②屏幕位置读了**内核默认槽位**（`[WL64] surface create … pos=448,380`），而合成器把它挪到 (476,408) → 裁到的是**终端文本**（于是 kernel_w=340 vs user_w=133）；③A/B 两面**重叠 16 行**，整块计数把 B 的字算进 A | ①改为核对 `[WL64] wm path fallback=/lib/wm.elf`；②改用 `[WM] surf … rect=` 真实矩形；③只比 A 的 **row 0 带**（不被遮挡）并用与程序**同一口径**（`tol=16/通道`）对账，容差收紧到 ±8% |

`texthint64_test` 的 3 条（vsoft / holes / levels）**没有改一个字节** —— 终端改回 AA 后原判据自然成立。

---

## 4. 验收（本构建产物，每个测试前还原干净镜像）

每个测试前把 `build64/{system.img,sysdisk.img}` 还原成构建刚产出的干净镜像
（客户机会把 `cfg` 落盘写回这两张图，串行跑多套会互相污染 —— v0.10.6 §11.2 的纪律）。

| 测试 | 结果 |
|---|---|
| `tests/texthint64_test.py` | **22/22 PASS**（vsoft / holes / levels 三条 AA 判据**未改一字**，终端改回 AA 后自然成立） |
| `tests/font64user_test.py` | **66/66 PASS** |
| `tests/term_render64_test.py` | **16/16 PASS**（判据从"无中间灰/颜色数<=4"翻转成"灰阶档>=8 + 无彩色像素 + 灰阶像素>500"） |
| `tests/gui_modern64_test.py` | **PASS**（dock 回弹 `el<=390ms` 仍达标：按行直写把 AA 的开销抵消掉了） |
| `tests/wmfull64_test.py` | **41/0 PASS ×2** |
| `tests/wm64_test.py` | **89/89** |
| `tests/viewer64_test.py` | **113/113** |
| `tests/explorer64_test.py` | **90/90** |
| `tests/vpkg64_test.py` | **84/84** |
| `tests/presets64_test.py` | **17/17** |
| `tests/presets_audit64_test.py` | **PASS** |
| `tests/presets_user64_test.py` | **PASS** |
| `tests/sysstate64_test.py` | **PASS** |
| `tests/dock64_test.py` | **PASS** |
| `tests/startmenu64_test.py` | **63 项全过** |
| `tests/panels64_test.py` | **PASS** |
| `tests/desktop64_test.py` | **PASS** |
| `tests/desktopops64_test.py` | **72/72**（原 69/70；改动见 §3） |
| `tests/settings64_test.py` | **PASS** |
| `tests/office64_test.py` | **15/15** |
| `tests/locklogin64_test.py` | **104/104** |
| `tests/ui_extra64_test.py` | **PASS** |
| `tests/cursor64_test.py` | **PASS** |
| `tests/clicklaunch64_test.py` | **22/22** |
| `tests/taskmgr64_test.py` | **31/31** |
| `tests/termcjk64_test.py` | **PASS** |
| `tests/gfx64_test.py` | **PASS** |
| `tests/imgfit64_test.py` | **PASS** |
| `tests/icons64_test.py` | **PASS** |
| `tests/wl64_test.py` | **85/85**（原 81/85；改动见 §3） |
| `tests/drm64_test.py` | **118/118** |

未验证：`tests/textwm64_test.py` = **UNVERIFIED(env)**（需要 VMware 虚拟机 + VNC，本机没有）。


---

## 5. 已知限制 / 未做

| 项 | 状态 |
|---|---|
| `textwm64_test` | **UNVERIFIED(env)**：需要 VMware VM + VNC，本机没有 |
| 内核 JPEG 壁纸 | 仍只支持 ≤2048 px（装卷阶段拦截 + `[WARN]`） |
| 维护者脚本 / ext4 写入 / AHCI ATAPI 光驱读 / USB FLUSH | 与 v0.10.5/6 一样仍未做 |
| 真机（物理机）测试 | 一直没做 |
| 60 fps | 未达成；本版 AA 未改 fps 目标，但**帧耗时不再因 AA 变差**（见 §2.3） |
| 安装向导重构（用户页/时区页/`/etc/user.conf`） | **完全没做**（方案见 v0.10.5 交接单 §5） |

---

## 6. 资产

由含本版全部改动的全量构建产出（`build64.sh` **EXIT=0**，内核 **3,337,008 B**）：

```
vimtu64-64.iso  84,328,448 B  sha256 81043e41c6517008c837fec8cc0d54a5f9ea925bb12736999726fd44db71c02b
vimtu64-64.img  33,554,432 B  sha256 d9fca768d7240e19b7337b5de1e4f9dd947676264a43730876ba941d308ade29
```

验收纪律（v0.10.6 起，必须照做）：每个测试前把 `build64/{system.img,sysdisk.img}` 从构建刚产出的
干净副本还原 —— 客户机会把 `cfg` 落盘写回这两张图，串行跑多套会互相污染。

