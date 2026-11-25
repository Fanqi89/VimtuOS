> [← 返回主页](../README.md) · VimtuOS 文档体系

# v0.10.6 — 拖拽残影根治（合成器脏矩形不再被丢弃）+ 用户壁纸入库

> ✅ **已发版**（`VIMTUOS_VERSION = "0.10.6"`，全量构建 `EXIT=0`）。
> 验收见 §5（本版把所有 GUI 套件都复跑了一遍，见 §5 的完整表）。

发布：<https://github.com/Fanqi89/VimtuOS/releases/tag/v0.10.6>
日期：2026-11-16
上一版：v0.10.5（图片查看器 + 全量文件关联路由）

---

## 1. 一句话

**`wmfull64_test` H②「拖动后旧位置不留拖影」那条约 1/6 概率的红，不是抖动 —— 是合成器在丢脏矩形。**
改完之后同一条判据（**旧标题栏色像素数必须 == 0**，没有放宽）连跑两次全绿；
顺带把用户投放的一批壁纸纳入仓库，并把两条 `presets` 验收判据对齐到 v0.10.5 的**装卷壁纸闸门**。

---

## 2. 拖拽残影：证据 → 根因 → 改法 → 防回归

### 2.1 现象（v0.10.5 交接单 §4）

同一份构建连跑 6 次：`41/0`、`41/0`、`39/2`、`41/0`、`41/0`、`41/0`。
失败那次的残留像素 **1088 / 704 / 320 / 192 / 73 / 37**（每次都不一样），
形态固定：**24×14 px，落在旧标题栏右端 close 按钮那一块**（旧矩形 (490,325,300,150) 内 x=766..788 / y=326..346）。

### 2.2 根因（`user/wm/wm.c` 的帧循环）

帧循环的 ⑨（背景快照回填 + 自下而上 blit + `fb_flip`）在 ⑪（收输入）**之前**，而 `dmg_*` 是**本轮局部变量**：

```
⑨ 合成并 flip(dmg)          ← dmg 到这一行为止的部分被画掉
⑩ ack
⑪ poll_event：拖动 -> wm_union(dmg, 旧矩形); wm_union(dmg, 新矩形)
                            ← 这一步在本轮已经画完之后，下一轮 dmg 又是 0 -> **整批被丢掉**
```

唯一的兜底是 ⑪ 末尾那段「本轮没画过 -> 立刻整窗重绘」；它只 union **当前**窗口矩形，
**旧位置不在其中**。于是只要客户端恰好在本轮提交了一帧（⑨ 路径帧），这一轮拖动位移的
「旧位置还给桌面」就永远回不来。

串口实证（改前的 `wmfull_serial.log`，拖动窗口内 34 条帧）：

```
6372 n=737  PAINT_9   dirty=106,0,348,140      ← ⑨ 路径：单窗口矩形
6403 n=741  input=1   dirty=0,0,310,140        ← 下一个 input 帧只覆盖 0..310，栏位 310..454 成残留
6544 n=774  PAINT_9   dirty=204,139,300,150    ← 又是 ⑨ 路径（宽正好 = 窗口宽 300，没有任何并集）
```

残留宽度 **正好 24 px = 一次鼠标步长**（`MOUSE_STEP=24`），位置在**拖走后未被任何后续脏矩形覆盖的那一条边**上 —— 与实测形态完全一致。

### 2.3 改法

`user/wm/wm.c`：

* ⑪ 的脏矩形走独立 `odmg_*`（不再写进本轮的 `dmg_*`）；
* 本轮 ⑨ 没画过 → 立刻画（原行为，交互延迟不变）；
* 本轮 ⑨ 已画过 → **不重复画**（不翻倍帧预算），把 `odmg_*` 存进 `carry_*`，**下一轮 ⑨ 必定画掉**；
* `from_carry` 的帧**不受帧日志采样过滤**（`WM_FRAME_LOG_MAX=64` 之后每 16 帧一条会把补画帧吞掉）——
  它正是"旧位置被补画回去"的唯一日志证据，未来回归时必须能自证。

### 2.4 防回归

* 判据**没动**：`wmfull64_test` H② `n_old == 0`、H③ `n_left3 == 0`（**没有**放宽成 `<= 300`）。
* 补画证据（新日志，拖动窗口内）：⑨ 帧现在画的是 **union**（宽 420 / 396 / 346 > 窗口 300），改前是**单一窗口矩形**（宽 300）。
* 想再验一次：`py -3 tests/wmfull64_test.py`，看 `[settle] s2/s3 try=1 -> clean` + `H. ② … n=0`。

---

## 3. 用户壁纸入库 + 装卷闸门对齐

### 3.1 壁纸入库

`res/wallpapers/` 里用户投放的 7 张图（9.6 MiB）现在**纳入 Git 跟踪**：

| 文件 | 尺寸 | 进卷？ |
|---|---|---|
| `1.jpeg` | 2649×1600 | ❌（超 2048 闸门） |
| `2.jpg` | 1920×1080 | ✅ |
| `3.jpg` | 9686×5449 | ❌ |
| `4.jpg` | 3840×2160 | ❌ |
| `5.jpg` | 2560×1600 | ❌ |
| `6.jpg` | 1920×1200 | ✅ |
| `7.jpg` | 1920×1200 | ✅ |

「进卷？」由 v0.10.5 的**装卷闸门**决定（`tools/presets_pack_win.py: JPG_MAXDIM = 2048`，
与内核 `kernel/img64.cpp: JPG64_MAXDIM = 2048` 必须同步）——超限的图**故意不装**，
因为内核解不了，装了只会让"用户选了壁纸、系统静默回落内置壁纸"这种假正常出现。构建日志里逐条 `[WARN]`。

### 3.2 两条 presets 判据对齐（都变严，不是变松）

| 测试 | 旧期望（已过期） | 新期望 |
|---|---|---|
| `presets_audit64_test` | 卷里 = 内置 `wall-*.png` + `res/wallpapers/*` **全部**；`MAX_PX = 16384` | 卷里 = 内置 + `res/wallpapers/` 中**边长 ≤ 2048** 的那些；**新增负向对照**：超限的 4 张**必须不在卷里**（漏进卷 = FAIL） |
| `presets_user64_test` | 点名 `/Wallpapers/1.jpeg` 必须能应用（该文件按闸门不再进卷） | 卷里**任意一张** JPEG（`.jpg/.jpeg`）壁纸能被应用 `ok=1` |

`MAX_PX` 现在写成 `2048` 并注明"装卷闸门必须与内核解码上限一致，这条断言就是那个'另一边'"。

---

## 4. 测试侧修掉的两处假红（判据不变，只让证据自明）

| 位置 | 症状 | 改法 |
|---|---|---|
| `wmfull64_test` D③ | 用一次性快照读 `[WL64] dispatch … composite=external`：这行是内核在客户端**下一次提交**时才打的，日志没刷到就红（同一构建有时红有时绿） | 改 `wait_re` 等它出现（仍要求该行存在） |
| `explorer64_test`「缩放参照系」 | 日志缺行时 `win0/win1` **退化成脚本常量 (120,60)**，打印出来和"真的取到了"一模一样（实测偶发红一次，看日志会误判成几何错） | 详情里加打「resize begin 行=有/缺」「dir\|end 行=有/缺」 |

---

## 5. 本版验收（全部在本构建产物上真跑）

### 5.1 门禁（全绿，全部在本构建产物上真跑）

每个测试前把 `build64/{system.img,sysdisk.img}` 还原成**构建刚产出的干净镜像**
（`SCRATCH/BASE_*.img`）—— 因为客户机会把 `cfg` 落盘写回这两张图，串行跑多套会互相污染
（实测：`panels64_test` 的"打开动画"因此必红，真因是图里残留 `ui.reduce_motion=1`）。

| 测试 | 结果 |
|---|---|
| `tests/wmfull64_test.py` | **41/0 PASS ×2**（连跑两次；H② 旧位置 n=0、H③ 全屏 n=0、`[settle] try=1 -> clean`） |
| `tests/wm64_test.py` | **89/89** |
| `tests/viewer64_test.py` | **113/113** |
| `tests/explorer64_test.py` | **90/90** |
| `tests/vpkg64_test.py` | **84/84** |
| `tests/presets64_test.py` | **17/17** |
| `tests/presets_audit64_test.py` | **PASS**（含新增"超限壁纸必须不在卷里"负向对照） |
| `tests/presets_user64_test.py` | **PASS**（任意 JPEG 壁纸可应用） |
| `tests/sysstate64_test.py` | **PASS ×2** |
| `tests/dock64_test.py` | **PASS** |
| `tests/startmenu64_test.py` | **63 项全过** |
| `tests/gui_modern64_test.py` | **PASS** |
| `tests/panels64_test.py` | **PASS ×2** |
| `tests/desktop64_test.py` | **PASS** |
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
| `tests/drm64_test.py` | **118/118** |

### 5.2 已知红 / 未验证（**本版没修**，全部与本版改动无关；不当作绿）

| 测试 | 结果 | 结论与下一步 |
|---|---|---|
| `tests/desktopops64_test.py` | 69/70 | 唯一红：**配置值往返被改写** —— `[STORE64] set cfg.ui.desktop.icons=0,1,2\|` 之后（等到了新的 `[CONF64] autosave ok`）重启，读回来是 `0,1\|2`。store64 的记录是长度前缀二进制（`u8 klen + u8 vlen + key + value`），`\|` 本身无害 → 怀疑"设置页复位写 config"与"`desktopops64_tick64` 状态回写"两路写者抢同一键。用户可见影响：复位默认图标后重启，桌面图标集合可能回到旧值。 |
| `tests/wl64_test.py` | 81/85 | 4 条是 **wl seat 事件路由**（重叠时投给最上层 surface / 键盘 type=1,2 / 鼠标 type=4,5 / `seat_supp` 统计），在 `kernel/wl64.cpp` 的 dispatch 里，与 ring3 合成器无关。 |
| `tests/font64user_test.py` | 58/65 | 7 条是**内核 vs ring3 字形位图/笔位/二值掩码 IoU** 的对齐判据；另 1 条要求"卷里没有 `/bin/wm.elf` 时内核如实打 `[WL64] wm skipped`"—— v0.5.x 起合成器改走 `/lib/wm.elf` 且由 ring3 自己 fork，这条期望已过期。 |
| `tests/texthint64_test.py` | 19/22 | 3 条是**字体 hinting 质量**（竖直子扫描软抗锯齿 vsoft>=0.90 / 断笔孔数 <=32 / 汉字覆盖档位 >=8），属字体栈质量问题，需单独一批处理。 |
| `tests/textwm64_test.py` | **UNVERIFIED(env)** | 需要 VMware 虚拟机 + VNC（`tests/textvnc.py` 等 `[LOCK64] bg blur ready`），本机没有该 VM，脚本抛 `OSError`。不是判据失败。 |

> 这 5 套都不在本项目的发版门禁列表里，本版之前也没有任何一版把它们跑绿过
> （v0.10.5 交接单 §8 列为"未复跑"）。本版把它们**全部跑了一遍并如实记录**，
> 结论是：**本版的改动没有引入任何一条新的红**。


---

## 6. 已知限制 / 未做（别当绿）

| 项 | 状态 |
|---|---|
| 内核 JPEG 壁纸 | 仍只支持 ≤2048 px；超限在**装卷阶段**拦截 + `[WARN]`；真支持 4K 需**解码期降采样** |
| 图片查看器 | 只 PNG/JPEG/BMP；只有整数最近邻缩放；无 EXIF 方向 |
| 维护者脚本 / ext4 写入 / AHCI ATAPI 光驱读 / USB FLUSH | 与 v0.10.5 一样仍未做 |
| 真机（物理机）测试 | 一直没做；用本版 ISO 验 `docs/真机启动加固.md` §3 的 8 条假设 |
| 60 fps | 未达成（17.5~19.6 fps）；大页开关默认关闭 |
| 安装向导重构（用户页/时区页/`/etc/user.conf`） | **完全没做**（方案见 v0.10.5 交接单 §5） |

---

## 7. 资产

由含本修复的全量构建产出（`build64.sh` **EXIT=0**，内核 **3,336,864 B**，
`/lib/wm.elf` **27,592 B** sha256 `01a229725ae6afa9` 回读一致）：

```
vimtu64-64.iso  84,328,448 B  sha256 7d8e93994e094170bdec436e2b45e5249f12f92f90523c62774b7b7e2d2441ea
vimtu64-64.img  33,554,432 B  sha256 09979cc5e5521be868ced50143dffb47b322396b07ce8243a88b2d725772ff81
```

构建日志里的装卷闸门（`res/wallpapers/` 4 张超 2048 px，按设计跳过并告警）：

```
[WARN] 系统卷放不下这 4 个预设文件，已跳过（用户投放目录太大）：
[WARN]   跳过 /Wallpapers/1.jpeg = 300184 B（尺寸超过内核 JPEG 解码上限 2048）
[WARN]   跳过 /Wallpapers/3.jpg  = 6812528 B（尺寸超过内核 JPEG 解码上限 2048）
[WARN]   跳过 /Wallpapers/4.jpg  = 1860845 B（尺寸超过内核 JPEG 解码上限 2048）
[WARN]   跳过 /Wallpapers/5.jpg  = 341387 B（尺寸超过内核 JPEG 解码上限 2048）
```

运行验收时的纪律（本版新增，**下一版必须照做**）：每个测试前把
`build64/{system.img,sysdisk.img}` 从构建刚产出的干净副本还原 —— 客户机会把 `cfg`
落盘写回这两张图，串行跑多套会互相污染（实测 `panels64_test` 因此必红）。

