# HANDOVER v0.12.7 · drag-capture —— 第一梯队收口（鼠标捕获 / 幽灵偏移 / 落点真移动 / 只读位）

- 日期：2026-11-27
- 版本链：`0.12.7` 阶段（**未 bump** `build64.sh` 的 `VIMTUOS_VERSION`，仍是 `0.12.4`；本单**不发版**）
- 关联单：`HANDOVER_v0125_ui_fix.md`（本单的前一单，§4 的 1→4 顺序已全部做完）、`HANDOVER_v0126_browser.md`
- 代码提交：`b505a806`（5 files changed, 162 insertions+, 20 deletions-）
- 现场：`kernel64_os.bin` = **3,601,536 B**；QEMU = 0；工作区只剩 `Fonts/*.ttf`（第三方字体，历来不提交）

---

## 1. 一句话

上一单 §4 的 1→4 **全部落地并实机验证**：`gui64` 有了真正的**鼠标捕获**（越过阈值申请、松手立刻释放），
拖拽幽灵的偏移与指针热点合成**唯一真源**（实测偏差 **0 px**），资源管理器拖到桌面走
`deskdir64_move_in64` **真落盘**（`rc=0 ok=1`），`exp_recompute_ro64()` 编进内核并跑出
`fileops64_test` **117/117**。

---

## 2. 四条改动的落点与判据

| # | 改动 | 代码位置 | 判据（实机） |
|---|---|---|---|
| 1 | 鼠标捕获 | `kernel/gui64.h` + `kernel/gui64.cpp` | `[GUI] drag capture on win=0 why=explorer-drag` / `[GUI] drag capture off why=explorer-drag-drop` |
| 2 | 偏移单一真源 | `kernel/gui64.h` 常量 + `gui64.cpp:draw_cursor` + `explorer64.cpp:exp_drag_ghost_rect` | 幽灵板子 = `(1086,156)..(1211,223)`，期望 `光标(1077,149)+(10,8)-1 = (1086,156)` → **偏差 0** |
| 3 | 拖拽落点真移动 | `kernel/explorer64.cpp` 的 `exp_drag_track/exp_mouse_move/exp_mouse_up` | `[DESKDIR] move in why=explorer-drag from=/home/vimtu/move_me.txt to=/home/vimtu/Desktop/move_me.txt vol=0 dir=0 via=rename name=move_me.txt rc=0 ok=1` + `[DESK64] file add kind=9 path=/home/vimtu/Desktop/move_me.txt` |
| 4 | 只读位现算 | `kernel/explorer64.cpp:exp_recompute_ro64`（`8e58caf8` 上一单写的，本单才编进去） | `fileops64_test` = **117/117 PASS**（`[PASS] mkdir /docs`、`新建文件夹成功（mkdir path=/newdir rc=0）`） |

### 2.1 鼠标捕获（`gui64.h` / `gui64.cpp`）

```c
// gui64.h：捕获期 motion/up 只投给持有者（屏幕坐标，不是客户区）
typedef void (*WindowMouseFn)(Window*, int mx, int my, int btn);
typedef void (*WindowMouseUpFn)(Window*, int mx, int my);
void    gui64_capture_mouse64(Window* w, const char* why);
void    gui64_release_mouse64(const char* why);
Window* gui64_mouse_capture64(void);
```

- `handle_mouse_press` 的**第一段**就是捕获旁路：命中测试（窗口 / Dock / 开始菜单 / 桌面图标）整体跳过，
  事件无条件 `press_in_client(捕获窗口)`。语义与 Win32 `SetCapture` 一致：指针在别人身上，
  但**手势的主人**还是发起拖拽的那个窗口。
- `handle_mouse` 里 motion 直投 `on_mouse_move`；左键松开投 `on_mouse_up` 后**立即** release。
- 四条 release 路径：`mouse-up`（松手）/ `owner-gone`（捕获者已销毁）/ `takeover`（换捕获者）/
  `explorer-close`（拖拽源随窗口关掉）。**任何一条漏掉都会把整个桌面卡成点不动**。

> 为什么不能只靠 `on_tick`：外壳原本**没有**鼠标移动分发，explorer 靠 60 Hz 采样 `mouse_get_buttons()` 兜底。
> 旧实机基线里一次"拖到桌面"的落点是 `x=1098`（目标 1101）—— 采样滞后；快速拖放更会整段漏掉 up。

### 2.2 偏移单一真源（消"3cm 错位"）

```c
// kernel/gui64.h
#define GUI64_CURSOR_HOT_X  0    // 热点 = 箭头尖 = 位图 (0,0)
#define GUI64_CURSOR_HOT_Y  0
#define GUI64_DRAG_OFF_X    10   // 幽灵底板左上角相对**热点**
#define GUI64_DRAG_OFF_Y    8
```

- `draw_cursor()` 的像素循环：`g_cur_x + GUI64_CURSOR_HOT_X + i`；
- `exp_drag_ghost_rect()`：`g_drag_x + GUI64_DRAG_OFF_X`（原来是写死的 `+14/+12`）。
- 两处**只读同一组常量**，改一处两边一起动。

### 2.3 拖拽状态机唯一实现（`explorer64.cpp`）

```c
static void exp_drag_track(Window* w, int dmx, int dmy, int btn);   // 唯一实现
static void exp_mouse_move(Window* w, int mx, int my, int btn) { exp_drag_track(...); }   // 捕获直投
static void exp_mouse_up  (Window* w, int mx, int my)          { g_drag_x = mx; g_drag_y = my; exp_drag_finish(w); }
// exp_tick 里只剩 60Hz 兜底采样： if (g_drag_arm >= 0) exp_drag_track(w, mouse_get_x(), ...);
```

- `exp_drag_begin()`：`g_drag_active = 1;` 之后立刻 `gui64_capture_mouse64(g_win, "explorer-drag")`；
- `exp_drag_finish()`：**第一行**就 `gui64_release_mouse64("explorer-drag-drop")`（落点成不成立都放）；
- 落点只读判断：`if (g_ro)` → `if (exp_recompute_ro64())`（按目标盘现算）；
- 落点真移动：桌面 = `desktopops64_add_file64(... "explorer-drag")` → `deskdir64_move_in64`；
  拖进子目录 = `fs64_rename_to64`。

---

## 3. 本轮最值钱的一课：**"日志说 on，状态是 off"的假绿**

第一版捕获编出来之后，实机日志有 `[GUI] drag capture on win=0 why=explorer-drag`，
但**永远没有 `off`**。查下去的过程值得完整记下来（下次别再走一遍）：

1. 断言先红：新加的两条阶段 3 断言里，`capture off` 那条 FAIL。
2. 串口里只有 `on`，没有 `off`；而 `[UI] explorer drag drop` 明明打出来了（说明 `exp_drag_finish` 跑过）。
3. 查内核镜像：`drag capture on win=` **在**，`drag capture off why=` / `takeover` / `owner-gone` / `mouse-up`
   **全不在** → 不是日志被吞，是代码被**优化掉了**。
4. `nm` 看对象：`g_mouse_cap` 这个符号**根本不存在**；`objdump` 看 `gui64_release_mouse64` = `push rbp; ret`（空函数）。
5. 根因：`gui64_capture_mouse64()` 里的 `g_mouse_cap = w;` 被我前一次 `CUT/PUT` 行编辑**吃掉了**
   （`g_mouse_cap_win = win_index(w);` 还在，所以"on"日志照样打印）。于是 `g_mouse_cap` 恒为 `nullptr`，
   编译器把整个捕获状态机判成死代码：release 直接 `ret`，`takeover` 分支删除。
6. 修法：把 `g_mouse_cap = w;` 补回，并**在代码里写明它是真状态不是日志装饰**；再编 → `g_mouse_cap`
   符号出现、release 有实体、四个字符串全在 → 实机 `on`/`off` 成对出现。

> 教训（写进纪律）：**"打了日志"不等于"改了状态"**。凡是"某能力生效"的断言，必须有一条
> **退出路径**的判据（这里 = 松手必须打 `off`），否则日志本身会撒谎。
> 另外：**行号式编辑（CUT/PUT N.=M）在本仓已经三次吃错行** —— 改完必须
> `grep` 关键赋值/关键字符串确认，或用唯一串替换。

---

## 4. 实机证据（全部本机实跑，可复现）

### 4.1 捕获 + 幽灵跟手（`$PI_SCRATCH_DIR\drag_shot.py`，12/12 PASS）

```
[PASS] 拖拽中截图
[PASS] 拖拽申请鼠标捕获（[GUI] drag capture on why=explorer-drag）  [GUI] drag capture on win=0 why=explorer-drag
[PASS] 松手释放捕获（[GUI] drag capture off …）                     [GUI] drag capture off why=explorer-drag-drop
[PASS] 松手有落点结算  [UI] explorer drag drop path=/etc dir=1 x=1077 y=149 target=desktop ok=0
  客人光标（松手瞬间）= (1077, 149)  目标=(1080, 150)
[PASS] 零采样滞后：落点 x,y == 指针目标点（±3px）  dx=-3 dy=-1
[PASS] 差分里出现幽灵板子（按行/列密度定位成功）  changed=7784 box=(1086, 156, 1211, 223)
  幽灵板子 = x1086..1211 y156..223  (126x68)  changed=7784
[PASS] 幽灵尺寸 ≈ 124x66（±10px；含 1px 白色边 + 1px 强调描边）  126x68
  期望幽灵左上角 = 光标 + (10,8) = (1087,157)
[PASS] 幽灵左上角 == 光标热点 + (10,8)（±3px，偏移单一真源）  实测=(1086,156) 期望=(1086,156) 偏差=0
=== 汇总：12/12 PASS（拖拽幽灵跟手 · 像素判据）===
```

判据怎么来的（判据本身也是宿主侧独立测量，不看客人自报）：

- 两张 `screendump`：A = 指针顶到左上角、无拖拽；B = 拖拽中（指针不动）。
  逐像素差分后，**按列的命中密度取最长连续段、再在列带内按行取**，得到"板子"矩形；
  指针自己那块（`cx-1..cx+18 / cy-1..cy+22`）挖掉，避免把指针算进包围盒。
- 客人光标位置取**松手瞬间** `[UI] explorer drag drop … x= y=`（截图与松手之间不移动指针）。
- 交付截图：`docs/images/drag_ghost_follow.png`（35,548 B，2× 最近邻放大裁剪块）。
- 整屏证据：`docs/images/drag_ghost_mid.png`（393,274 B，拖拽中的**整屏** 1280×800 —— 资源管理器窗口、
  幽灵板子、指针在同一帧；采样：幽灵板 (1150,190)=(139,181,218)，窗口标题栏 (400,70) 亮度 245.6，
  指针热点 (1077,149)=(0,0,0)）。

> 诚实说明：`dx=-3 dy=-1` 是鼠标闭环标定（`dxe=dye=2`）的量级，**不是**采样滞后；
> 上一单记录的旧基线是 `x=1098`（目标 1101，同样 −3），所以"滞后"这件事在旧代码里也**没有**几像素量级的表现。
> 用户看到的"3cm"最可能是**幽灵在指针出窗后定格**（旧代码只有 60Hz 采样）造成的视觉错位，
> 现在的判据能证明的是：**偏移基准统一 + 每帧跟手 + 松手零漏边沿**。

### 4.2 拖拽回归（`tests/dragdrop64_test.py`，**53/53 PASS**）

```
[PASS] 拖拽成立（[UI] explorer drag begin … min=8）  [UI] explorer drag begin idx=1 name=etc dir=1 sx=441 sy=178 min=8
[PASS] 拖拽越过阈值 -> 申请鼠标捕获  [GUI] drag capture on win=0 why=explorer-drag
[PASS] 松手 -> 立即释放捕获         [GUI] drag capture off why=explorer-drag-drop
[PASS] 真移动如实失败（rc=-1 ok=0 why=rename-failed）  [DESKDIR] move in why=explorer-drag from=/etc to=/home/vimtu/Desktop/etc vol=0 dir=1 via=rename name=etc rc=-1 ok=0 why=rename-failed
[PASS] 拖到文件夹 -> drag move（rc=0 why=- ok=1）  [UI] explorer drag move name=mv_src.txt dir=0 from=/tmp/mv_src.txt to=/tmp/mvdst/mv_src.txt rc=0 why=- ok=1
=== 汇总：53/53 PASS（跨层拖拽：explorer -> 桌面）===
```

对照（**改前同一用例**，旧内核）：`38/43 PASS`，其中阶段 4.5 的 4 条 `/tmp` 刷新/导航断言和
1 条 `阶段 5 不得出现 selftest FAIL` 是红的；本版这 5 条全绿（阶段 4.5 的 `/tmp` 刷新链路恢复）。

### 4.3 落点真移动 + 桌面目录（`tests/deskfolder64_test.py`，**64/74**）

```
[PASS] 真移动成功（[DESKDIR] move in … via=rename rc=0 ok=1）
   [DESKDIR] move in why=explorer-drag from=/home/vimtu/move_me.txt to=/home/vimtu/Desktop/move_me.txt vol=0 dir=0 via=rename name=move_me.txt rc=0 ok=1
[PASS] 桌面项由目录条目生成（[DESK64] file add kind=9 path=<桌面>/<名字>）
   /home/vimtu/Desktop/move_me.txt
=== 汇总：64/74 PASS ===
```

`64/74` 与 v0.11.7 基线**逐条相同**，10 条失败全是已知的第二梯队缺陷：
回收站拖入链路（`select kind=9` / `[DESKDIR] trash` / `[RECYCLE64] files` / `restore` 共 7 条）
+ **窗口底角圆角**（3 条，`底角是空的` / `白边全是直线` / `左右对称`）。

### 4.4 只读位（`tests/fileops64_test.py`，**117/117 PASS**）

```
[PASS] mkdir /docs
[PASS] 新建文件夹成功（mkdir path=/newdir rc=0）
[PASS] 右键空白 -> 新建文本文件 -> 立刻出现 newfile.txt
[PASS] 复制/剪切/粘贴/重命名/删除 全 PASS
=== RESULT: PASS ===  checks=117 ok=117
```

上一单记为 `checks=117 ok=116`（唯一失败是视觉项"编辑框白底 黑=0"），本版**该项也过了**。

---

## 5. 下一轮（第二梯队，按用户报的清单）

1. **窗口底角圆角**：`deskfolder64_test` 已有 3 条像素 FAIL 就是它（上两角有、下两角没有）。
   位置：`gui64_deco_frame64` / `gui64_deco_edge64`（内核与 `wl64` 合成共用）。
2. **终端字体笔画丢失（h→n）**：`kernel/terminal64.cpp` 的 mono/无 AA 路径；注意
   `kernel-terminal-cjk-mono-threshold` 那条技能里记的"阈值要按 codepoint 变"。
3. **右键菜单样式崩坏**：`deskops` / `explorer` 两套菜单几何与主题 Token 对齐。
4. **Dock / 开始菜单精细度**；**开始菜单丢 `browser.elf`**（`app64` 注册表扫描 vs `/etc/browser.on`）；
   **除终端外不能多开**（`app_launch_exec64` 的单例判定）；**图片查看器双击无反应**；
   **启动左上角调试小窗**；**重命名输入框左右方向键光标**。

浏览器侧遗留（第三批）：`gui_download_table` 仍是 NULL（下载 + `/Downloads` 写盘）；
"错证书必须失败"的负向对照；发版闸门全套。

---

## 6. 现场核对（四条命令）

```
git log --oneline -2          -> b505a806 / f48b1e28
git status --short            -> 只剩 Fonts/*.ttf（+ Fonts/fms_metadata.xml）
ls -l build64/kernel64_os.bin -> 3,601,536 B
Get-Process qemu-system-x86_64 -> 0
```

## 7. 本轮踩坑清单（别重复）

- **行号式编辑吃行**：`CUT/PUT N.=M` 在本仓已三次吃错行（这次吃掉的是 `g_mouse_cap = w;`）。
  改完**必须** `grep` 关键赋值与关键字符串确认。
- **`bash -lc` 里没有 clang**：`build64.sh` 自己 `export PATH="/c/msys64/mingw64/bin:..."`，
  单独诊断性编译要照抄这一行（`CXX="clang++ --target=x86_64-elf"`）。
- **`deskfolder64_test.py` 的登录超时 240 s 很紧**：本机启动到锁屏约 200 s（TCG / 512 MB），
  实测连续两次只因 `[LOCK64] bg blur ready` 迟到而整轮失败；重跑即过（**不是**回归）。
- **`docs/images/*` 被 `.gitignore` 排除**：交付截图要 `git add -f`。
- 长跑一律串行；收尾必须 `Get-Process qemu-system-x86_64` = 0。
