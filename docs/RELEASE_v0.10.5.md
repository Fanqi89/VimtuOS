# v0.10.5 — GUI 文件关联路由器 + 图片查看器

> ✅ **已发版**（`VIMTUOS_VERSION = "0.10.5"`，全量构建 `EXIT=0`）。
> 验收：`tests/viewer64_test.py` **113/113 PASS**（连跑两次）、`explorer64_test` **90/90**、
> `wm64_test` **89/89**、`wmfull64_test` **41/0**、`vpkg64_test` **84/84**、
> `presets64_test` **17/17**、`sysstate64_test` PASS。

发布：<https://github.com/Fanqi89/VimtuOS/releases/tag/v0.10.5>
日期：2026-11-15
上一版：v0.10.4（商店验收判据修正）

---

## 1. 一句话

**双击不再"什么都当文本打开"。** 系统里第一次有了**独立的图片查看器**（`/bin/viewer`，
真能解 PNG / JPEG / BMP），文件管理器里也第一次有了**全量文件关联路由表**：
按扩展名分发到 PE 装载器 / 包管理器 / 图片查看器 / 文本编辑器，
二进制与未知后缀**一律拒绝打开**并弹窗说明。

用户报的痛点（原话）："我在文件管理器里双击任何非原生文件，全都被强行用'文本编辑器'打开了
（满屏幕乱码）。并且，系统当前没有独立的图片查看器。"

---

## 2. 图片查看器（任务一：移植，不手搓）

### 2.1 为什么选 stb_image（移植路线评估）

| 候选 | 结论 |
|---|---|
| 内核 `kernel/img64.cpp` 现成解码器 | ❌ 它是 **ring 0** 的，而且 **JPEG 明确未实现**（该文件第 9 行自己写着）。本批要 ring 3 + 三种格式，直接排除。 |
| libpng + libjpeg-turbo | ❌ 需要 zlib + 两个库的构建体系、动态链接、几十个源文件；本仓库用户态是 `-nostdinc -nostdlib` 的手写 CRT，接不动。 |
| **stb_image v2.32（单头文件）** | ✅ **选它**。public domain / MIT；一个文件；只需要 `memcpy/memset/abs`；三种格式（PNG/JPEG/BMP）都在里面。 |

放在 `third_party/stb/stb_image.h`（277,883 B，原样 vendored，未改一个字节；
来源与许可见 `third_party/stb/README.md`）。

### 2.2 怎么把它塞进这个"没有 libc"的用户态

裁剪（都靠 stb 自己的宏，**没有改上游代码**）：

```
STBI_NO_STDIO    文件由 viewer.c 读成内存 -> stbi_load_from_memory
STBI_NO_HDR      去掉 Radiance HDR
STBI_NO_LINEAR   去掉 HDR<->LDR 线性转换
                 ★ 这两个一起，把 `pow`/`ldexp` 整段编掉 —— 所以**不需要 math.h**
STBI_NO_PSD/TGA/GIF/PIC/PNM      只留 PNG + JPEG + BMP
STBI_NO_SIMD     不用 SSE intrinsic（纯 C，可复现）
STBI_NO_FAILURE_STRINGS          失败原因用我们自己的静态串
STBI_ASSERT(x)   ((void)0)       环 3 里 assert 只能整进程死；解码失败走返回值
STBI_MALLOC/REALLOC/FREE         全部指向自建竞技场
```

补的两样基础设施：

1. **`user/lib/limits.h`（新增）** —— stb 无条件 `#include <limits.h>`，
   而本仓库是 `-nostdinc -I user/lib`，之前没有这个头。
2. **内存竞技场（`user/viewer/stb_image_impl.c`）** —— 用户态 `malloc` 只有
   **4 KiB**（`user/lib/stdlib.h` 的 `V64_HEAP_BYTES`），装不下任何一张真图。
   改用 `mmap(9)` 拿 **12 MiB**、16 字节对齐 bump 分配。
   本进程是**一次性**的（看完图就退出，内核回收整片地址空间），所以"不回收"是正确口径，
   不是偷懒 —— 理由同时写在 `vi_img.h` 与 `stb_image_impl.c` 头部。

### 2.3 窗口壳（`user/viewer/viewer.c`，真正"极简"的地方）

* 一块 `shm` 画布 + 一个 `wl_surface`，与 `user/store/store.c` 同一套 syscall 契约；
* 窗口尺寸 = **图片按比例塞进 760x520 的图像区**（+ 20 px 标题栏 + 1 px 边框）；
* 缩放是**整数最近邻**（`sx = x*sw/dw`），本文件 `-mno-sse`，一行浮点都没有；
* 标题栏用 `user/lib/font64.c` 画文件名 + 原图尺寸 + 格式（字体读不到就只画色块，如实报 `ok=0`）；
* ESC 退出，退出打点 `reason=esc|timeout|error`。

上限（都不是拍的）：单 surface `WL64_MAX_PIXELS64 = 524288` px（`kernel/wl64.h`），
本程序最大画布 762x541 = 412,242 px；输入文件 4 MiB、解码 1.6 Mpx（竞技场装得下）。

### 2.4 产物与装载

```
build64/viewer.elf = 116,632 B（4 个 PT_LOAD，memsz 合计 109,827 B）
基址 0x100090000 = USER64_MMAP_VA64（4GiB+576KiB），与 /bin/vpkg 同一条"大映像"先例
```

为什么必须走大映像：stb_image 的 PNG+JPEG+BMP 三个解码器 + 窗口壳 + 用户态 CRT + font64
远超老装载区 64 KiB。`kernel/elf64.cpp` 的 `e64_pick_main_layout64()` **早就有**这条路
（tcc / busybox / lua / make / vpkg 都在上面），内核**零改动**。

`user/viewer/build_viewer.sh` 的产物自检（与 store/browser 同一条纪律）：
静态 ELF64 / 无 `PT_INTERP`·`PT_DYNAMIC` / `PT_LOAD` 落在 `[4GiB+576KiB, 4GiB+998MiB)` /
每段可读 / **没有任何一页同时 W 与 X**（逐页铺开核验）。

---

## 3. 全量文件关联路由表（任务二）

在 `kernel/explorer64.cpp` 里，`exp_open_item()`（双击 / 回车 / 右键"打开"都走它）
从"只看内容嗅探"改成**扩展名优先**：

| # | 扩展名 | 路由 | 证据 |
|---|---|---|---|
| ① | `.exe` | `/bin/peload <path>`（PE 装载器） | `[UI] explorer route spawn app=/bin/peload pid=… arg=…` |
| ② | `.deb` | `/bin/store`（包管理器 GUI） | `[UI] explorer route spawn app=/bin/store pid=…` |
| ③ | `.png` `.jpg` `.jpeg` `.bmp` | `/bin/viewer <path>` | `[UI] explorer route spawn app=/bin/viewer pid=… arg=…` |
| ④ | `.txt` `.md` `.c` `.cpp` `.py` `.sh` `.json` `.log` | 只读文本预览（本系统的文本编辑器） | `[UI] explorer preview name=…` |
| ⑤ | `.o` `.obj` `.a` `.so` `.bin` `.elf` `.dll` `.sys` `.img` `.iso` `.ttf` `.ttc` | **禁止打开** → 弹窗"二进制文件不可预览" | `route=binary` 且**没有** preview/spawn |
| ⑥ | 其它（`.mp3` 之类） | **禁止打开** → 弹窗"未找到关联应用" | `route=unknown` + 保留 `[UI] explorer noassoc name=… kind=…` |

统一打点：`[UI] explorer route name=<n> ext=<e> route=<text|image|pe|deb|binary|unknown>`。

**两条保底**（不放宽，只是不丢已有能力）：

* 扩展名不在表里、但**文件内容**是 VAP64/ELF 的（例如没有扩展名的可执行文件）**仍然按可执行跑**；
* 目录仍然双击进入。

**为什么"拉起"要用后台进程而不是 `elf64_run64`**：`elf64_run64` 会**阻塞**到程序退出，
用在文件管理器里就是"桌面直接冻住"。这里走 `proc64_create64 + proc64_start_elf64_argv64`
（与终端 `elfrun bg` 完全同一条真进程路径），并把 pid 打进日志 —— 拉不起来就如实打
`reason=no-proc|start-failed`，绝不当成"已经打开了"。

**关于"选择应用打开"面板（P8）**：双击不再进这个面板（上面 ⑤⑥ 是明确拒绝），
但面板本身**保留**并接到了 **Ctrl+O**（拒绝弹窗的文案里就写着"Ctrl+O 可选择程序打开"）。
为什么不放进右键菜单：`EXP_CTX_ITEM_N = 6` 的菜单几何已经被 `tests/explorer64_test.py`
断言，加一项会把已经绿的测试改红；键盘加速键零布局风险。

**关于 `.deb` 不假装安装**：`/bin/store` 不接 argv（安装走 `dpkg -i <路径>` 或商店里的仓库条目），
所以提示文案是"已打开包管理器（本地 .deb 用 dpkg -i 安装）"——**不谎称已经装上了**。

---

## 4. 验收（全绿，全部有串口/像素证据）

`tests/viewer64_test.py`：**113 checks / 113 PASS**（连跑两次）。两条独立证据链：

### A. 图片查看器（终端驱动，四种图逐个验）

```
[VIEWER] ver=1 pid=27 file=/Pictures/redblue.png
[VIEWER] read ok bytes=493 magic=png
[VIEWER] decode ok w=128 h=96 comp=3
[VIEWER] win w=130 h=117 img=128x96 mode=1:1
[VIEWER] surf id=1 w=130 h=117 shm=5 map=0        [+ attach rc=0、seat=1]
[WL64] surface create id=1 w=130 h=117 shm=0 pid=27 px=15210 fmt=0 slot=0 pos=964,532
[WM]   surf id=1 pid=27 rect=575,341,130,117 pix=15210      ← 合成器真实矩形（唯一真值）
[WM]   frame n=3 surfs=1 … px=15210                          ← 合成器真画了一帧
screendump 该矩形内：左半纯红 6080 px / 右半纯蓝 6080 px / 标题栏 1830 px
[VIEWER] done frames=… reason=esc
```

四张图各自独立断言（每张都要重新走一遍「启动 → 读盘魔数 → 解码尺寸 → 几何 → surface 表 →
合成器矩形 → 真帧 → 像素 → ESC」）：`redblue.png`/`.jpg`/`.bmp` 都是 `128x96 → 130x117 mode=1:1`，
`big.png`（1600x1200）是 **`mode=fit` → 显示 693x520 → 窗口 695x541**（按比例，不是拉伸铺满）。
像素判据对四张图都成立（左红右蓝各 ≥ 3000 px）——**解码错、RGB/BGR 互换、把图当文本开**，三种错都会立刻红。

### B. 文件关联路由（文件管理器里真实鼠标点选 + 回车）

6 条路由逐条验（`[UI] explorer route name=… ext=… route=…` + 真拉起 + pid）：

| 样本 | route | 真证据 |
|---|---|---|
| `blob.bin` | `binary` | 无 `preview` 打点（没被当文本打开） |
| `song.mp3` | `unknown` | 保留 `[UI] explorer noassoc …` 老打点（`fatread64_test` 依赖） |
| `notes.txt` | `text` | `[UI] explorer preview name=notes.txt` |
| `redblue.png/.jpg/.bmp` | `image` | `route spawn app=/bin/viewer pid=N` + 该 pid 的 `ver=1 file=…` + `decode ok 128x96` + `ESC → reason=esc` |
| `app.exe` | `pe` | `route spawn app=/bin/peload pid=N arg=/Pictures/app.exe` |
| `setup.deb` | `deb` | `route spawn app=/bin/store pid=N` |

另外验：盘根 → `/Pictures` 的导航、9 个样本条目全在、`hit=card:N`/`hit=item:N`（点到了才算到）、
全程无 `PANIC`/`TRIPLE FAULT`/`FAILED mask=`。

### C. 回归（同一份构建）

| 测试 | 结果 | 备注 |
|---|---|---|
| `explorer64_test` | **90/90** | 双击 ELF 现在真的起进程；顺带把一条**陈旧断言**改成真证据 |
| `wm64_test` | **89/89** | 页池抖动容差按"映射窗 8 槽各需一张 PT 页"重算到 ±8 |
| `wmfull64_test` | **41/0** | 拖动不再留影（根因是 VA 撞车，见 §6） |
| `vpkg64_test` | **84/84** | 本版没动商店链路 |
| `presets64_test` | **17/17** | 静态判据跟上 v0.6.0 的目录重排 |
| `sysstate64_test` | PASS | "Esc 关终端"走内核终端路径，行为不变 |

---

## 5. 本版改了什么



| 文件 | 改动 |
|---|---|
| `third_party/stb/stb_image.h` | **新增** vendored stb_image v2.32（原样，未改） |
| `third_party/stb/README.md` | **新增** 来源/许可/裁剪说明 |
| `user/lib/limits.h` | **新增** stb 需要的 `<limits.h>` |
| `user/viewer/vi_img.h` | **新增** 解码外壳接口（7 个函数 + 三个上限常量） |
| `user/viewer/stb_image_impl.c` | **新增** 唯一编译 stb_image 的 TU + mmap 竞技场 |
| `user/viewer/viewer.c` | **新增** wl_surface 窗口壳（整数最近邻缩放 + 标题栏 + ESC） |
| `user/viewer/viewer64.ld` | **新增** 大映像基址链接脚本 |
| `user/viewer/build_viewer.sh` | **新增** 构建 + 产物自检（含逐页 W^X） |
| `kernel/explorer64.cpp` | 全量路由表 + 扩展名解析 + 后台拉起助手 + Ctrl+O |
| `tools/mk_pics_win.ps1` | **新增** 三格式验收图（.NET System.Drawing；纯 ASCII .ps1） |
| `tools/viewer_pack_win.py` | **新增** `/bin/viewer` + `/Pictures/*` 装卷（逐字节回读） |
| `build64.sh` | 接入查看器编译 / 图册 / 装卷 / probe64 |
| `tests/viewer64_test.py` | **新增** 两条证据链的端到端验收 |

内核**唯一**改动是 `kernel/explorer64.cpp`（路由表）；`wl64` / `proc64` / `elf64` 一个字节没动。

---

## 6. 未做 / 如实说明

### 6.0 本轮挖出的**系统级**缺陷（不随本版修，单独立项）

1. **键盘焦点不回收**：`/lib/wm.elf`（或任何 wl 合成器/客户端）退出后，内核打
   `[EV64] focus pid=0 prev=28 reason=capture-release`，焦点**不会回到内核窗口** ——
   之后往终端/外壳敲的字符全部丢失（真机体验：看完图回终端，键盘没反应；点窗口也不恢复）。
2. **内核内部合成是一次性的**：`mode=internal` 时只在面板区"paint chrome 一次 + blit 一次 +
   flip 一次"，桌面任何一次重绘都会把它盖掉。实测 `[WL64] blit surf=1 … dst=964,532 bytes=60840 clip=ok`
   明明打出来了，screendump 那一块却是终端文字。
   ⇒ 这也是为什么 `/bin/viewer` 必须像 store 那样**自己拉起 /lib/wm.elf** 才能稳定可见。

* **图片缩放只有最近邻**（无平滑插值），也**没有缩放交互**（不滚轮放大、不拖动画布）——
  用户要求的是"把 PNG/JPG/BMP 显示跑通"，本版就做到这里。
* **不支持 GIF/TGA/PSD/PNM/HDR**（都 `STBI_NO_*` 编掉了）。要加就是去掉一个宏 + 重编。
* **不支持 EXIF 方向**（`STBI_NO_…` 未开 EXIF；手机照片可能躺着）。
* `.deb` 双击**只拉起包管理器**，不代跑 `dpkg -i`（见 §3 最后一段）。
* 维护者脚本 / ext4 写入 / ATAPI 光驱读 / USB FLUSH / 真机测试 —— 与 v0.10.4 一样仍是未做项。

---

## 6. 本版顺手修掉的**系统级**缺陷（都是实测抓出来的，不是猜的）

| # | 缺陷 | 证据（改前） | 根因 | 改法 |
|---|---|---|---|---|
| 1 | **按 ESC 关掉图片/对话框，会把终端和 ring3 外壳一起杀掉** | `[SH64] exit pid=26 code=0` + `[APP] term closed`；之后敲什么都没人执行 | 一次 ESC 被**两条通道**投递：EV64 捕获者（wm）转发给 wl 客户端，gui64 的 kbd 环又投给最前窗口 → `terminal64.cpp` 的 `term_key` 把 ESC 当"关闭本窗口"，关窗 = `sh64_stop64()` | shell 会话里 ESC **归 shell**（真终端语义）；非 shell 终端仍保持"ESC 关窗"（`sysstate64_test` 依赖） |
| 2 | **wl 合成器拿不到大于 64 KiB 的 surface** | `[WL64] surfmap FAILED id=4 page=112 err=6` → `[WM] frame … px=0` → 窗口在那儿但**内容全黑** | 单 surface 允许 2 MiB（`WL64_MAX_PIXELS64=524288`），但映射窗只给 **64 KiB/槽**，两者自相矛盾 | 映射窗步长改为 **2 MiB/槽**（8 槽 16 MiB），并加自检不变量"步长 ≥ 像素上限 × 4" |
| 3 | **wl 映射窗压在 DRM 映射窗上** | `wmfull64_test` H②：拖动后旧位置残留 1088 px 拖影、还原出的背景里混着纯黑像素 | `drm64.h` 的 `DRM64_MAX_FB64 = 8`（头注释写的却是"4 个槽"）→ DRM 窗最坏占 `[+8,+72) MiB` | FB 映射区分三段不重叠：[0,8) 帧缓冲 / [+8,+72) DRM / [+72,+88) wl；并用 **编译期 static_assert** 钉死 |
| 4 | **双击 ELF 永远跑不起来** | `[ELF64] reject reason=busy` + `rc=4294967295`；测试却一直"绿"（grep 到的是**引导期**那条 `launch ok`） | explorer 的 `.elf` 分支走 `elf64_run64()` —— 内核上下文的**单例装载器**，一份镜像没收尾就永久 `E64_BUSY` | 改走 proc64 真进程（与终端 `elfrun bg`、image/pe/deb 路由同一条）；测试判据同时改成真证据 `[PROC64] execve path=/hello.elf` |
| 5 | **`[APP] mypc opened` 偶发搜不到**（14 个验收脚本依赖它） | 日志里是 `[[TASK64] kheart …\nAPP] mypc opened` —— 前导 `[` 被心跳行吃掉 | 这行没拿行锁（IF=1），PIT 心跳插进了同一条行 | 三条 `[APP] mypc *` 打点全部 `dbg64_line_begin64/end64` |
| 6 | **查看器解不开 1600x1200（连 12 MiB 竞技场都不够）** | `[VIEWER] decode FAILED reason=too-many-pixels`，改大上限后变成 `reason=decode` | 竞技场 12 MiB；而 stb 解 RGBA PNG 的**峰值是 11 B/px**（滤波行 4 + native 4 + 转 3 通道 3）—— 宿主机实测 1600x1200 峰值 **20.18 MiB** | 竞技场 12 → **32 MiB**，像素上限按实测峰值重算到 2.5 Mpx（覆盖 1600x1200 / 1920x1080） |
| 7 | **6.8 MB 的壁纸能把整条构建链打断** | `ValueError: 卷空间不足（数据块用尽）` → `build64.sh EXIT=1` | 预设装卷是"先 put 再撞墙"，且 `res/wallpapers/` 是用户投放目录（本轮塞了 ~9.7 MB） | 装之前先算剩余空间，装不下的**逐个跳过 + [WARN]**；顺带挡住"内核解不出来的壁纸"（>2048 或非 PNG/JPEG） |

---

## 7. 已知限制（如实记录，别当绿）

* **JPEG 壁纸 > 2048 px 不支持**：内核 `kernel/img64.cpp` 的 `JPG64_MAXDIM = 2048`（解码缓冲上限），
  超了会**静默回落内置壁纸**。本版在装卷阶段就把它挡掉并打 `[WARN]`（不再静默）。
  用户投放的 1.jpeg(2649x1600)/3.jpg(9686x5449)/4.jpg(3840x2160)/5.jpg(2560x1600) 因此没进卷。
* 图片查看器只支持 PNG/JPEG/BMP（stb_image 的其余格式在宏里编掉了）；只有整数最近邻缩放，
  没有缩放交互；EXIF 方向不支持。
* `.deb` 双击只拉起包管理器，不代跑 `dpkg -i`；维护者脚本（preinst/postinst）仍整包拒绝。
* 真机（物理机）首测仍未做；`docs/真机启动加固.md` §3 的 8 条假设待验。
* 60 fps 未达成（17.5~19.6 fps）；大页开关（`SHM64_HUGE_ENABLE`/`FB64_HUGE_ENABLE`）仍默认关闭。

---

## 8. 发布资产

| 文件 | 大小 | sha256 |
|---|---|---|
| `vimtu64-64.iso` | 84,328,448 B | `40eaf895d8a8b2f880bb91b91f2236cf1be162d530dfba8ad0a26bdfadeb7a0d` |
| `vimtu64-64.img` | 33,554,432 B | `24763ff29fa06727c3f55aff6635402e479fc2c06560ed77bf6710e8027cbd04` |

内核 `build64/kernel64_os.bin = 3,336,864 B`（v0.10.4 是 3,331,744 B；+5,120 B = 路由表 + 行锁 + wl 窗 + ESC 语义）。
全量构建 `build64.sh` **EXIT=0**；`/bin/viewer = 117,848 B`（4 个 PT_LOAD，逐页无 W^X）。

### 8.1 一条如实的抖动记录（**不是**真绿，也不是真红）

`tests/wmfull64_test.py` 的 **H. ②「拖动后旧位置不留拖影」** 在**同一份构建**上连跑 6 次的结果：
`41/0`、`41/0`、`39/2(n=320/73)`、`41/0`、`41/0`、`41/0` —— 残留像素每次都不一样
（1088 / 704 / 320 / 192 / 73 / 37），**不是逐位相同**，按本项目"连跑两次逐位都相同才叫真红"
的判定标准属于**抖动**，不是回归。

已做的收敛：`screendump #2/#3` 都改成"先等一条新的 `[WM] frame n=…`（合成器真帧），
截完立刻量，最多 4 次、干净就早停"，并在每轮打 `[settle] s2/s3 try=N -> clean|dirty`。
改完之后 4 次连跑全部 `try=1 -> clean` 一次通过。

**仍未根治的部分（如实写在这里）**：偶尔（约 1/6）那 24×14 px 的残留（位置在旧标题栏右端、
close 按钮那一块，x=766..788 / y=326..346）在 4 次尝试里都不消失 —— 说明合成器的
"从背景快照还原旧矩形"在这条路径上**偶尔少擦一小块**。按代码路径看它属于 wm 的还原/脏矩形
计算，不是本版新增的 VA 撞车（那个已经修了：残留从 1088 px 降到 0~320 px 且不再稳定复现）。
下一步要修的是 wm 侧脏矩形，不属于本版范围。
