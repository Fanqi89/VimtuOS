# HANDOVER v0.12.9 · 中文字体 + 回收站闭环 + 第三梯队（多开 / 幽灵窗 / 浏览器进菜单）

- 日期：2026-11-27
- 版本串：**0.12.9**（`build64.sh` 的 `VIMTUOS_VERSION` 已 bump；本单发版）
- 关联单：`HANDOVER_v0128_ui_visual.md`（第二梯队视觉大修）、`HANDOVER_v0127_drag_capture.md`（第一梯队）
- 现场：`kernel64_os.bin` = **3,610,144 B**；`build64/browser.elf` = **2,774,704 B**；
  `build64/sysdisk.img` = 42,560,000 B（含 `/bin/browser`）；QEMU = 0；工作区只剩 `Fonts/*.ttf`

---

## 1. 一句话

六件事一次做完：**中文渲染**加"轻量自动 hinting（近竖直边吸附 1/4 px）+ 覆盖率 gamma 校正 + y 方向子扫描线 8→16"；
**回收站拖拽链路**的 7 条红全部转绿（根因是夹具只按了一次最小化，终端窗口盖住了桌面图标）；
**多开**砍掉两个真堵点（Dock 对卷程序是空操作 + appload 单槽队列）；**幽灵调试窗**关掉
（ring3 `write(1)` 在屏幕 (8,8) 直接画白字黑底并 flip，绕过合成器）；**浏览器进开始菜单**并把
`/bin/browser` 真正装进系统卷（`build64.sh` 以前一次都没调用过浏览器装卷脚本）。

---

## 2. 六项改动（文件 + 因果）

| # | 事项 | 根因 / 改动 | 位置 |
|---|---|---|---|
| 1 | 中文渲染生硬/极细/缺笔画 | 16px 汉字 1px 竖笔骑在像素边界（x=3.4..4.4）时覆盖率被劈成 60/40 两条淡边；线性混合在浅底深字上整体偏淡 | `kernel/font.cpp`：① `add_line()` 对 `|dx| ≤ 1/12 px` 的**近竖直边**把两端 x 吸附到 **1/4 px** 网格（位移 ≤ 1/8 px，形状不动，`FONT64_NO_HINT` 可关）；② 新增 `font_set_gamma64(pct)`，覆盖率 `a' = a + (curve(a)-a)·pct/100`（默认 **60**，`ui.gamma` 可配）；③ `FONT64_AA_SUB` 8 → **16** |
| 2 | 回收站拖拽 7 条红 | **不是产品缺陷**：`Ctrl+Shift+W` 只最小化最上面一个窗口，阶段 2 的终端 (16,16 974×678) 一直盖着桌面文件图标 (112,24) → 按下被 `win_at()` 吃掉 → 拖拽根本没开始（没有 `[UI] desktop icon select kind=9`） | `tests/deskfolder64_test.py`：改成**循环最小化**（≤6 次，带"命中次数"断言）；另修 3 处**过期正则**（`scan` 行多了 `entries=/total=` 字段；`restore` 成功时 `why=` 为空）与 1 处**过期口径**（阶段 5 原来断言"重启后桌面还有 live.txt"= 回收站没生效时的口径） |
| 3 | 除终端外不能多开 | ① `dock_open_app64()` 的 `switch` 只覆盖 8 个内核 app_id，卷程序（viewer/browser，id ≥ 1000）走 `default: break;` —— 点 Dock 什么都不发生、串口一行都没有；② `appload` 只有**一个槽**，连点两下时前一条请求被覆盖且对外恒返 `queued=1` | `kernel/gui64.cpp`：`default` 分支按 app_id 反查注册表 exec → `app_launch_exec64(exec,"dock")`（**每按一次 = 一个新进程**），打点 `[DOCK64] press app=<id> action=open-launch exec=…`；`appload` 改**深度 4 环形队列**，队满如实打 `[APPREG64] launch DROPPED … reason=queue-full` |
| 4 | 启动时的黑底白字调试小窗 | `kernel/syscall64.cpp` 的 `syscall64_screen_puts64()`：**每个 ring3 `write(1)`** 都在 (8,8) `fb_draw_text(白字黑底)` + `fb_flip_region`，**绕过合成器**，只能等桌面整体重绘才被盖掉 | 屏幕镜像改成**夹具开关**：只有卷里存在 `/etc/lxconsole.on` 才画（默认关），串口输出一字不改；打点 `[SYSCALL] screen echo=off (serial only)` |
| 5 | 浏览器进开始菜单 | 开始菜单扫 `/apps`、`/usr/share/applications`、`/apps/desktop` 的 `*.lnk`/`*.desktop`，种子表里**没有** browser | `kernel/app64.cpp` 种子表加 `/apps/browser.lnk`（`name=浏览器 / en=Web Browser / exec=/bin/browser / icon=globe`，与图片查看器同一套：种子 → 动态扫描 → 注册表 → "所有应用"） |
| 6 | `/bin/browser` 根本不在系统卷里 | `build64.sh` **一次都没调用** `tools/browser_pack_win.py`（grep browser 只有注释） | `build64.sh` 在 TLS 装卷之后、`payload_embed` 之前插入浏览器链：`netsurf_lib_build_win.sh` → `netsurf_core_build_win.sh` → `browser_build_win.sh` → `browser_pack_win.py --vol-in tlsvol.img --vol-out browservol.img`（**不带 `--on`**，发行卷不能有夹具开关） |

---

## 3. 实机证据

### 3.1 中英混排 + 多开 + 幽灵窗 + 浏览器菜单 —— `tier3_shot.py` **11/11 PASS**

```
A 导航窗格(中文): {'ink': 2168, 'dark': 603, 'levels': 12, 'ink_pct': 5.16, 'dark_pct': 27.8}
A 内容区(英文):   {'ink': 12348, 'dark': 1579, 'levels': 13, 'ink_pct': 8.57, 'dark_pct': 12.8}
[PASS] A 中文区有墨点 / 英文区有墨点 / 灰阶 >= 6 档 / 暗像素占墨点 >= 25%（gamma 后不再发虚）
C 注册表里 exec=/bin/browser 的条目：['浏览器']            （entries=15）
[PASS] C 浏览器进应用注册表（开始菜单能看到）
D 起 viewer 前 左上 256x40 dark=0
B explorer 双击两次 -> viewer pids=['26', '28']；[WM] surf=[('1','26'), ('2','28')]
[PASS] B 两次双击各起一个 viewer 进程（>= 2 个不同 pid）
[PASS] B 两个 viewer 各有独立 surface（[WM] surf >= 2 个不同 pid）
D 起 viewer 后 左上 256x40 dark=0（起前 0）
[PASS] D 左上角没有多出黑底调试窗（暗像素增量 < 400）
=== 汇总：11/11 PASS ===
```

- 截图：`docs/images/v0129_mixed_text.png`（中英同屏）、`v0129_mixed_text_zoom.png`（3× 放大）、
  `v0129_multi_instance.png`（双击两次后的整屏）、`v0129_two_viewers.png`（两个查看器窗口并排）、
  `v0129_startmenu.png`（开始菜单）。
- **幽灵窗前后的对照（同一夹具）**：v0.12.8 里 `run /bin/viewer /kaisi.png` 之后左上 256×40 的暗像素
  从 **145 → 1570**（`docs/images/ghost_probe_worst.png` 就是那个黑底白字框）；本版起 viewer 前后都是 **0**。

### 3.2 回收站闭环 —— `tests/deskfolder64_test.py` **76/76 PASS**（v0.12.8 是 71/75）

```
[PASS] 按在桌面最左的文件图标上 -> 选中（select kind=9）  item=(3, 9, 112, 24, 1)
[PASS] 拖进回收站（[DESKDIR] trash … rc=0 entry=…）  [DESKDIR] trash why=desktop-icon name=live.txt … rc=0 entry=live.txt
[PASS] 回收站记录了原位置（[TRASH64] put orig=<桌面>/<名字>）  [TRASH64] put name=live.txt orig=/home/vimtu/Desktop/live.txt type=file vol=0 rc=0
[PASS] 桌面条目消失（[DESKDIR] scan … removed=1）  … entries=3 … added=0 removed=1 … names=docs2,pic.png,move_me.txt
[PASS] 重启后桌面没有它（live.txt 已进 /Recycle）
[PASS] 回收站里还有那一项（[TRASH64] init … items>=1）
[PASS] 回收站窗口里看到它（[RECYCLE64] files … n=1 virt=0）
[PASS] 恢复成功（[RECYCLE64] restore … real=1 rc=0）  [RECYCLE64] restore kind=3 name=live.txt to=/home/vimtu/Desktop/live.txt rc=0 why= real=1
[PASS] 恢复后桌面重新列出（[DESKDIR] scan why=restore … added=1）
===== 汇总：76/76 PASS =====
```

### 3.3 回归（本轮全跑）

| 用例 | 结果 |
|---|---|
| `version64_test` | **8/8 PASS**（串口里 `VimtuOS 0.12.9 x86_64`，旧串 0 次） |
| `deskfolder64_test` | **76/76 PASS** |
| `fileops64_test` | **117/117 PASS** |
| `startmenu64_test` | **63 条全过** |
| `dock64_test` | **RESULT: PASS**（固定项 7 / 持久化往返 / toggle 全过） |
| `tier3_shot.py`（新） | **11/11 PASS** |

---

## 4. 这一轮踩到的坑（都写进脚本注释了）

1. **`Ctrl+Shift+W` 只最小化一个窗口**（`top_visible_win64()`）。任何"要在桌面上点到图标"的夹具
   都必须**循环**最小化，或先断言窗口真的没了。
2. **ring3 合成器起来后内核窗口拿不回键盘焦点**（`[WL64] seat get … focus-only: composer holds capture`）：
   实测第一个 viewer 起完之后，终端里再敲 `run …` 连 `[TERM] run argv` 都不出现 → 多开取证改走**鼠标**
   （资源管理器双击文件 = `exp_route_spawn64`，每进程一个实例）。
3. **`screendump` 与双缓冲有时机差**（v0.12.8 已记）在本轮又抓到一次：四角判据的对照帧里窗口还在 →
   差分全 0。判据改成"先用内容见证点证明窗口真在/真不在"，再判角点。
4. **过期判据三连**（`deskfolder64_test`）：内核日志加字段（`entries=/total=`）后旧正则 `\S+` 永远不匹配；
   `why=` 成功时为空；阶段 5 的口径依赖"回收站不生效"。→ 修脚本时**判据本身不改**，只修匹配与口径。
5. **`fileops64_test` 的 `selftest FAIL` 是日志交织假红**：`[DESK64] selftest PASS mask=` 与
   `[TASK64] kwork` 抢行时会打成 `… mask=[TASK64] kwork …`。重跑即过（117/117）。
6. **`sendkey` 的 `/` 是 `slash`**，不是 `/`；否则会静默丢失（本例把 `run /bin/viewer` 打成了 `/binviewer`）。

---

## 5. 下一轮

1. **键盘焦点回归**：ring3 合成器 hold 住 seat/capture 时，内核窗口（终端）应能通过点击拿回键盘焦点
   —— 这是"多开之后还能继续操作"的前提（本轮只证明了多开本身）。
2. **浏览器从开始菜单真正启动一次**（`/bin/browser` 已在卷里、注册表已见；下一轮做端到端截图 +
   `[BROWSER64] launch` 证据）。
3. 中文渲染继续打磨：CJK 竖笔的**相位吸附**目前只在竖直边上做，横笔（y 方向）还靠 16 条子扫描线；
   可评估 `ui.gamma` 的最大舒服值（60 是当前实测不发糊的上限）。
4. 第二梯队剩余：右键菜单暗色主题对比度、Dock hover 放大的 mip 链。
