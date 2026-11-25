> [← 返回主页](../README.md) · VimtuOS 文档体系

# RELEASE v0.10.4 — 商店验收判据修正：`vpkg64_test` 回到 84/84 全绿

发布日期：2026-11-15
上一版：v0.10.3（本地包管理：`.deb` 的 zstd 解码 + 符号链接真落地）

---

## 1. 一句话

**v0.10.3 留下的 20 条红项全部关掉，`tests/vpkg64_test.py` 从 61 PASS / 20 FAIL 变成 84/84 全绿。**
真因**不是**色差、**不是**打点预算、**不是**本轮引入的回归 —— 是**测试自己量错了位置**：
它拿内核 `[WL64] surface create … pos=964,532`（**创建时的默认槽位**）当窗口原点，
而 Ring 3 合成器 `user/wm/wm.c` 会先用 27 号把新窗口挪到**屏幕正中 (512,370)** 并在那里合成。

本版**不改任何内核/用户态渲染代码**（内核大小与 v0.10.3 逐字节相同），
只改验收判据 —— 而且**只加强，不放宽**。

---

## 2. 两条根因（都有现场字节证据）

### 2.1 A 类 · 2 条：宿主侧没走内核 `/bin → /usr/bin` 别名

| # | 判据 | 失败原因 |
|---|---|---|
| 1 | `⑩ /bin/hello-cli 存在且 == .vap64 的 payload 段` | `vol_read("/bin/hello-cli")` → `None` |
| 2 | `⑩ /bin/hello-cli 是静态 ELF64` | 同上（跟着返 `None`） |

现场证据（宿主直接解析夹具卷）：

```
/bin/hello-cli                        -> None                       ← 卷上**没有 bin 目录**
/usr/bin/hello-cli                    -> 11520 B b'\x7fELF'         ← 与 .vap64 的 payload 逐字节一致
/usr/share/hello-cli/hello-cli.vap64  -> 11562 B b'VAP6'
```

`/bin → /usr/bin`、`/lib → /usr/lib` 是**内核** `vfs64_alias_path64()` 在路径入口做的重写
（`kernel/vfs64.cpp` 的 `kPathAlias[]`），卷上从来没有这两个目录。
客户机里 `run /bin/hello-cli` 一直是好的（`[HELLO-CLI] done` + `summary rc=0` 全 PASS）。

**修法**：`tests/vpkg64_test.py` 里镜像那张别名表（`VOL_ALIAS` + `vol_alias()`），
`vol_read()` 入口先归一。规则照抄内核：整段匹配（`/lib` 不命中 `/libfoo`）、长串在前。

**判据同时加强**（不只是"改对路径就算了"）：

```python
got_bin == want_hello_payload                      # 逐字节
and vol_read(tp, vol, "/usr/bin/hello-cli") == got_bin   # 归一前后是同一份字节
and vol_read(tp, vol, "bin/hello-cli") is None           # 卷上确实没有 bin 目录
```

最后一条把"读错路径返回 None 也满足 `== None` 型假绿"这条路彻底堵死。

### 2.2 B 类 · 18 条：窗口原点取的是**创建时默认槽位**，不是合成器真实位置

日志事实（同一个 pid）：

```
[WL64] surface create id=1 w=256 h=60 shm=0 pid=28 px=15360 fmt=0 slot=0 pos=964,532   ← 内核创建时的默认槽位
[WM] place id=1 pos=512,370 mode=center                                                ← 合成器把它挪走了
[WM] surf id=1 pid=28 rect=512,370,256,60 pix=15360 flags=0x0                           ← 屏上真实矩形
```

老测试用 `pos=964,532` 算矩形和点击坐标，于是：

* **像素探针**在 `(964,532)-(1220,592)` 读的是**桌面壁纸** →
  `count_color(CO_OK)=0 px`、`count_color(CO_ROWSEL)=0 px`；
  而"亮像素 ≥ 300"却**被壁纸顶过去了**（该区 14340 亮像素）—— 这是一条**假绿**。
* **点击**也落在壁纸上 → 指针永远进不了窗口 → `[STORE] click` **一条都不出现** →
  后面 15 条（安装/进度/启动/进卷/installed.json）全是级联。

在同一张截图上把原点换成 `(512,370)`：

| 原点 | CO_OK | CO_ROWSEL | 亮像素 | 图标区 8x8 |
|---|---|---|---|---|
| **964,532**（旧） | **0** | **0** | 14340 | 全黑 |
| **512,370**（真） | **114** | **2440** | 641 | **64 px 全 (63,181,95)** |

⇒ 系统渲染一直是**对的**（颜色、混色、alpha 全都对），是**测试量错了地方**。
这也解释了为什么"商店起来了、也渲染了、surface 表也有"全 PASS 却什么都没测到。

**修法**：原点改取合成器自己报的 `[WM] surf id=… pid=<store_pid> rect=x,y,w,h`，
兜底才退回 `[WM] place … pos=`。**不用**创建时的 `pos=`。

**判据同时加强**（新增 2 条 + 加严 1 条）：

1. 新增 `合成器给出窗口的屏上真实矩形（[WM] surf … rect=x,y,w,h）`；
2. 新增 `窗口矩形在屏内（原点来自合成器，不是默认槽位）`；
3. `列表渲染有墨迹` 从"亮像素 ≥ 300"加严成 **"亮像素 ≥ 300 **且** 窗口自己的底色
   `CO_BG` ≥ 500 px"** —— 壁纸区从此不可能满足它（实测 `bright=641 bg=1696`）。

### 2.3 顺带修掉的第三个隐患：指针闭环用**陈旧**坐标

老 `click_panel()` 每一轮都拿"整段日志里最后一条 `[STORE] mouse`"去校正模型。
store **只在指针落在自己窗口里时**才打点，所以指针一出窗口，这个"最后一条"就是
**很久以前**的位置 —— 模型被反复重置到老地方，越纠越偏。

新写法 `pointer_to()`：

```
走一程（模型预估）-> 等 0.30 s 让帧循环吃掉最后几包
  -> 取**这一轮之后**才出现的 `[STORE] mouse`（since=）
  -> 用它对模型 resync -> 残差还大就再走一轮（最多 4 轮）
```

为什么必须闭环：qemu monitor 的 `mouse_move` 会被 PS/2 设备**并包**（两条指令合成一个包），
客人按 `dx*17/10` 对**整包**取整，模型按**单包**取整 → 1 px 级漂移会累计。
另：模型起点 `(512,384)` 不是猜的，是 `kernel/input.cpp` 的 `mouse_x = 512; mouse_y = 384;`，
且登录手势只用键盘（两次 `ret`），指针确实还在那儿。

---

## 3. 修复后的现场证据（`tests/vpkg64_test.py`，本版实测）

```
[vpkg64] 商店窗口屏上原点 = (512, 370) 256x60（创建时默认 964,532）
  [PASS] 合成器给出窗口的**屏上真实矩形**（[WM] surf … rect=x,y,w,h）
         [WM] surf id=1 pid=28 rect=512,370,256,60
  [PASS] 列表渲染有墨迹（亮像素 >= 300 且窗口底色 CO_BG >= 500 px）   bright=641 bg=1696
  [PASS] 窗口矩形在屏内（原点来自合成器，不是默认槽位）                rect=512,370 256x60 screen=1280x800
  [PASS] 第 1 行（hello-cli 已装）的**绿图标**在屏上（>= 40 px）       green=114
  [PASS] 选中行底色在屏上（>= 200 px）                                sel=2440
  [PASS] 点第 2 行 -> 选中 gui-demo    [STORE] click sx=63 sy=31 btn=row hit=row sel=1 name=gui-demo
  [PASS] 点「安装」按钮（命中测试 hit=install）
  [PASS] 点击 -> 安装开始（[STORE] click install name=gui-demo）
  [PASS] 安装进度有界打点（progress pct=25/40/70/100）
  [PASS] **GUI 点安装 -> 包真进卷**（[STORE] install ok name=gui-demo）
  [PASS] GUI 的安装走的是同一份引擎（install file + db write 都在）
  [PASS] 第 2 行的图标变成**绿色**（装完了；绿像素 >= 40）             green2=64
  [PASS] 状态条/图标带整体绿像素增多（>= 120）
  [PASS] 点「启动」按钮（命中测试 hit=launch）
  [PASS] **GUI 点启动 -> 装出来的包真跑起来**   [STORE] launch name=gui-demo path=/bin/gui-demo pid=30
  [PASS] 被启动的程序自己的打点（[DEMO-GUI] ver=1 .. done frames=3）  [DEMO-GUI] done frames=3
  [PASS] 被启动的程序提交了 3 帧（[DEMO-GUI] frame f=0/1/2 rc=0/0）
  [PASS] 它的窗口也进了内核 surface 表（[WL64] surface create w=160 h=40）
  [PASS] GUI 安装的 gui-demo 真进卷（/bin/gui-demo == .vap64 payload，10096 B）
  [PASS] GUI 装完后 installed.json = {gui-demo, hello-cli, vpkg-debdemo}
```

```
=== RESULT: PASS ===  checks=84 ok=84
EXIT=0
```

判据条数 **82 → 84**（新增 2 条，一条没删；`tests/status_report.py` 里的门禁数字已同步改成 84/84）。

---

## 4. 验收矩阵（本版实测）

| 验收 | 结果 |
|---|---|
| `tests/vpkg64_test.py` | **84/84 PASS（exit=0）**（上一版 61 PASS / 20 FAIL） |
| 全量构建 `build64.sh` | **EXIT=0** |
| 内核 `build64/kernel64_os.bin` | **3,331,744 B**（与 v0.10.3 逐字节同值：本版未动内核） |

---

## 5. 本版改了什么（文件清单）

| 文件 | 改动 |
|---|---|
| `tests/vpkg64_test.py` | 镜像内核 `kPathAlias` + `vol_alias()`；窗口原点改取 `[WM] surf … rect=`；`CO_BG` 自证；`pointer_to()` 新鲜真值闭环；新增/加严 3 条判据 |
| `tests/status_report.py` | 门禁数字 `82/82` → `84/84`（3 处） |
| `build64.sh` | `VIMTUOS_VERSION="0.10.4"` |
| `README.md` | 版本行 + 发布列表 |
| `docs/RELEASE_v0.10.4.md` | 本文件 |
| `docs/开发历程.md` / `docs/TODO_路线图.md` | 追加本轮段 |

**内核 / 用户态渲染代码：零改动。** `user/wm/wm.c`、`user/store/store.c`、
`kernel/wl64.cpp`、`kernel/input.cpp` 一个字节都没动 —— 渲染本来就是对的。

---

## 6. 为什么"优化 wm.c"是错的方向（记一笔，别下次再走错）

原先的嫌疑有三个，全部被证伪：

1. **"alpha 被抹成 0，合成器走了半透明分支"** —— `store.c` 的
   `fill_rect(…, (sel ? CO_ROWSEL : CO_ROW) & 0xFFFFFFu)` 看着危险，
   但 `fill_rect()` 里的 `px()` 会重新补上 `0xFF000000`，所以落进画布的就是
   `0xFF2E4A6C` 这种不透明色，`wm_blit_slot()` 走 `a == 0xFF → 直接拷贝` 分支。
   实测图标区 64 px **精确等于** `(63,181,95)`，一个混色像素都没有。
2. **"v0.10.2 的打点预算把夹具依赖的日志吞了"** —— `[STORE] click` 是**应用自己**
   `printf` 的，不经过内核预算；它一条不出现的唯一原因是**点击真的没送到窗口里**。
3. **"v0.10.2 的混色/帧节拍改坏了"** —— 换成正确原点后，颜色逐像素精确匹配，
   与混色公式无关。

真正的教训：**"亮像素多"这种间接判据会漂移**，必须配一条"只有这个窗口才有的东西"
（本版用 `CO_BG` 窗口底色）才叫闭环。
