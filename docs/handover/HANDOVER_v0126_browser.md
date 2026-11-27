# HANDOVER v0.12.6 · browser —— NetSurf 核心 + 自研 fetcher + Edge 风格前端

- 日期：2026-11-26
- 版本链：`0.12.6` 阶段（**未 bump** `build64.sh` 的 `VIMTUOS_VERSION`；本单只到"能编能链"）
- 本单 HEAD：见 `git log --oneline -1`（提交信息以 `feat(browser): NetSurf 核心编起来` 开头）
- 前置单（按顺序读）：
  1. `docs/handover/HANDOVER_v0124_netsurf_ready.md` —— 五步路线图（**本单完成其中 ②③ 的代码骨架**）
  2. `docs/handover/HANDOVER_v0125_netsurf_libs.md` —— 14 个依赖库怎么编
  3. `docs/handover/HANDOVER_v0124_tls.md` —— TLS 的五个真缺陷

---

## 1. 一句话

NetSurf 的**本体（100 个 .c）已经编成 `libnetsurfcore.a`**，`http/https` 抓取**换成我们自己的
socket + mbedTLS**（`user/browser/fetchers/vimtu.c`，不再需要 libcurl），并用 `wl_surface` 画布做出
**Edge 风格的浏览器外壳**（44 px Chrome：←/→/⟳/⋯ + 地址栏），链接产物 `build64/browser.elf`
**2,773,960 B**。**客人里真正跑通并画出网页这一步尚未验收（UNVERIFIED）**——见 §5。

---

## 2. 现场四条核对

```
git log --oneline -3            -> 见最新提交（feat(browser): ...）
git status --short              -> 只剩 Fonts/*.ttf（第三方字体，历来不提交）
ls -l build64/browser.elf       -> 2,773,960 B
Get-Process qemu-system-x86_64  -> 0
```

| 产物 | 读数 |
|---|---|
| `build64/netsurf/lib/libnetsurfcore.a` | 117 个对象 / 2,044,564 B / 1,162 个已定义符号 |
| `build64/browser.elf` | 2,773,960 B / PT_LOAD=5 / span=2,759,384 / base=`0x100090000` |
| `build64/netsurf/lib/*.a`（14 个依赖库） | 3,885,334 B（v0.12.5 产物，本单未动） |
| `build64/mbedtls/libmbedtls.a` | 1,527,458 B |

---

## 3. 本单新增（**已完成、不要重做**）

### 3.1 核心编译：`tools/netsurf_core_build_win.sh`
按**组**编译，每组一行判据 `[core:<group>] cc ok=N fail=0 archive=… symbols=…`：

| 组 | 内容 | 实测 |
|---|---|---|
| `utils` | utils/*.c（nsoption、messages、filepath…） | 20 个 / 339,002 B |
| `nsurl` | utils/nsurl/*.c | 2 个 / 40,308 B |
| `content` | content/\*.c（fetch、llcache、hlcache、urldb…） | 10 个 / 270,386 B |
| `fetchers` | data、resource、about/、file/ **+ 我们的 vimtu.c** | 20 个 / 141,488 B |
| `handlers` | content/handlers/{css,html,image,text} | 36 个 / 732,302 B |
| `desktop` | desktop/*.c（browser、browser_window、selection…） | 29 个 / 521,744 B |

- 生成物（构建期）：`build64/netsurf/gen/nscfg.h`（`NETSURF_BUILTIN_LOG_FILTER` 等三个宏必须写成
  **真头文件**；走 `-D` 会掉进 shell 引号地狱，实测 `-D…='"all"'` 让 `options.h` 拿到多字符常量→int）、
  `gen/testament.h`（`desktop/version.c` 无条件 include）。
- 图像解码器按可用库裁剪：**SVG/BMP/GIF/ICO 要**；`png.c/jpeg.c/jpegxl.c/webp.c/rsvg*.c/video.c` 排除。
- PDF 输出排除（`save_pdf.c`、`font_haru.c`：没有 libharu）。

### 3.2 抓取：`user/browser/fetchers/vimtu.c`（顶替 libcurl）
- 只做 **HTTP/1.1 GET**；HTTPS 走 mbedTLS（BIO 回调非阻塞：先 `poll(fd,0)` 探可读/可写，探不到返回
  `WANT_READ/WANT_WRITE`，**绝不在 BIO 里睡死**——内核 `recv` 没数据会 sleep 最多 3000 ms）。
- `Connection: close`（一次 fetch 一条连接，不搞 keep-alive）。
- CA 读 `/etc/ssl/certs/ca.pem`；`VERIFY_REQUIRED`；失败即 `FETCH_ERROR`，**不静默降级**。
- 回调协议：`fetch_set_http_code` → `FETCH_HEADER`（逐行）→ `FETCH_DATA`（原文，**不在 fetcher 里解 chunked**）
  → `FETCH_FINISHED`；3xx+Location → `FETCH_REDIRECT`；90 s 硬超时 → `FETCH_TIMEDOUT`。
- 注册：`tools/netsurf_patch_fetch.py` 幂等往 `content/fetch.c` 的 `fetcher_init()` 里插
  `fetch_vimtu_register()`（上游目录不进仓，所以补丁脚本才是唯一真源）。

### 3.3 前端：`user/browser/`（14 个文件）
| 文件 | 作用 |
|---|---|
| `vimtu_abi.h` | 自有 ABI 包装（inline asm `int 0x80`）：shm_create/map、wl_surface_*、set_decor、input_poll |
| `vimtu_canvas.c/.h` | shm→像素→surface；fill/blit/text/flush/ink 统计 |
| `vimtu_plot.c` | 全局 `const struct plotter_table plot`（rectangle/line/polygon/fill/clip/text/bitmap/flush） |
| `vimtu_layout.c` | `gui_layout_table`（width/position/split） |
| `vimtu_bitmap.c/.h` | `gui_bitmap_table` + 自定内部 `struct bitmap` |
| `vimtu_font.c/.h` | 字模（复用 `user/wm/wm_ttf.h`，16px em、ASCII 32..126、4bit 覆盖率）+ UTF-8 解码 |
| `vimtu_table.c` | `netsurf_table` 装配 + 调度器（CLOCK_MONOTONIC）+ window/corewindow/misc/utf8/fetch 表 |
| `vimtu_shell.c` | **Edge 风格 44 px Chrome**：←/→/⟳/⋯（几何绘制）+ 地址栏 + 真值命中的后退/前进/刷新 |
| `vimtu_main.c` | 自写 `_start`→`__libc_start_main`、初始化、事件循环、串口打点 |
| `vimtu_http_*.c`（7 个） | 各 `#include` 一个上游 `utils/http/*.c`（一个上游 .c = 一个 TU） |
| `vimtu_jscript_stub.c` / `vimtu_core_stub.c` | 补齐核心缺失符号（`js_*` 11 个 / `save_pdf` / `fetch_javascript_register` / `fabs`） |

### 3.4 构建 / 交付 / 钩子
- `tools/browser_build_win.sh`：编前端 → 链 `libnetsurfcore.a` + 14 库 + `libmbedtls.a` + musl；
  自检判据 `[BROWSER] cc ok=N fail=0` + `[BROWSER] link ok elf=… PT_LOAD=… span=… base=0x100090000`。
- `tools/browser_pack_win.py`：装 `/bin/browser`（musl 静态 ELF64，2,773,960 B）+ `/etc/browser.on`，
  **逐字节回读**（宿主 sha256 == 卷内 sha256）；`--on` 是夹具开关。
- `kernel/kernel64.cpp`：`[BROWSER64] launch flag=/etc/browser.on pid=… start_rc=… wait=0`
  —— 走**真进程**路径（`proc64_create64 + proc64_start_elf64`），且**不等待**（浏览器常驻，等它退出就等于
  卡死在 `gui64_run` 之前）。

---

## 4. 坑清单（子代理实测，按会咬人排序）

1. **`EXTRA` 一律加 `$NS/` 前缀 → 绝对路径恒假、静默跳过**：`vimtu.c` 传绝对路径时变成
   `$NS/c/Users/…`，核心归档里根本没有 `vimtu.o`，症状是链接期 `undefined symbol: fetch_vimtu_register`。
   **已修**：`netsurf_core_build_win.sh` 现在绝对路径原样收（`case "$e" in /*) …`）。
2. **核心侧还缺两类源**：`utils/http/` 是子目录而 `utils` 组 `RECURSE=0`（缺 `http_parse_content_type`
   等 12 个符号）；`desktop/jscript.c` 与 `content/handlers/javascript/` 整个不在源码树里（缺 11 个
   `js_*`）。→ 用 `user/browser/` 里的 unit/stub 文件补齐，**判据与 third_party 均未动**。
3. **musl 的 `crt1.o` 不能用**：`lea _DYNAMIC(%rip)` 在 4 GiB+ 基址下链接期就炸；必须自写 `_start`
   （照 `user/tls/httpsprobe.c`）。
4. **窗口装饰吃 25 行**：`set_decor` 之后 `kernel/wl64.cpp` 的 blit 直接 `continue` 掉顶部 25 行。
   所以客户内容从 y=25 起画，可用高只有 515：44 px Chrome + 471 px 页面区。
5. **像素上限链条**：`WL64_MAX_PIXELS64 = 524288`（2 MiB / 4 B），`user/lib/wl.h` 里写的 16384 是**过时注释**。
   960×540×4 = 2,073,600 B 已是"像样窗口"的上限。
6. **`netsurf/*.h` 不自带 `utils/errors.h`/`assert.h`**：include 顺序错了满屏 `type name requires a specifier`。
7. **`html_get_box_tree()` 没有类型检查**（上游直接硬转 `html_content*`）：非 HTML 内容进去就是野指针遍历，
   必须先用 `content_get_type()==CONTENT_HTML` 挡。
8. **`browser_window_back/forward` 在 3.12 里不存在**（只有 `*_available`）：真跳转走
   `desktop/browser_history.h` 的 `browser_window_history_back/forward(bw,false)`，按钮亮灭按 `*_available`。

---

## 5. 诚实边界（**这部分是 UNVERIFIED，别当成功**）

第一轮（**旧 `system.img`**，钩子还没编进内核）：串口只到 `[TASK64] kheart`，`[BROWSER64] launch` 一次都没有
—— 由此确认"跑的是旧内核"，不是钩子坏了。

第二轮（**新内核 `kernel64_os.bin` = 3,597,328 B**，含 `[BROWSER64]`；夹具盘 `build64/browser64_test.img`）：
```
[BROWSER64] launch flag=/etc/browser.on pid=26 start_rc=0 wait=0
[BROWSER] init ok
[BROWSER] fatal browser_window_create err=31        <- NSERROR_NOT_IMPLEMENTED
```

结论（诚实版）：
- ✅ **进程真的起来了**（`pid=26 start_rc=0`），**`netsurf_init()` 全过**（url db / llcache / fetchers 都初始化成功），
  说明"musl + NetSurf 核心 + mbedTLS 能在 ring3 里跑"这条最大的不确定性**已经消掉**。
- ❌ **卡在 `browser_window_create()`，err=31 = `NSERROR_NOT_IMPLEMENTED`**；`[BROWSER] open url=` /
  `render boxes=` / `paint frame=` 与**像素判据全部未达**。
- 注意：同一轮串口里 `[WL64] wm TIMEOUT pid=25` —— ring3 合成器 `/lib/wm.elf` 那一路超时，内核改用
  `mode=internal` 的内部合成器（这是既有行为，与本缺陷无因果关系，但"窗口到底由谁来合成"要盯住）。
- 前端留空（代码里各有对应打点）：地址栏键盘输入（`address input UNVERIFIED`）、「⋯」菜单
  （`chrome menu UNVERIFIED`）、`target=_blank` 副窗只做逻辑窗口（`window create secondary detached UNVERIFIED`）、
  `arc`/`path` 返回 OK 不绘制、`bitmap_table.render` 返回 `NSERROR_NOT_IMPLEMENTED`。

---

## 6. 下一步（严格顺序）

1. **重建内核 + 夹具验收**（本单正在做）：
   `bash build64.sh`（含 `[BROWSER64]` 钩子）→
   `py -3 tools/browser_pack_win.py --fixture-img build64/browser64_test.img --from-vol build64/tlsvol.img --browser build64/browser.elf --on --system build64/system.img` →
   起 QEMU（`-display none -serial file:… -vga std -netdev user,…e1000`）→ **锁屏要按两次回车**才进桌面（GUI64 ready）→
   读串口 `[BROWSER64] launch` / `[BROWSER] …` → monitor `screendump` 抓像素算 ink。
2. **把 `build64.sh` 的卷链加第 N 步**：`browser_pack_win.py --vol-in …/tlsvol.img --vol-out …/browservol.img --disk build64/sysdisk.img`
   （本单**故意没改** `build64.sh`，因为还没验收）。
3. **像素判据**：打开 `https://example.com/` 后，页面区必须有墨迹（正文文字）+ 地址栏有 URL 文本；
   再点一次「⟳」必须出现 `paint frame=2`。
4. 之后才谈：真实站点（bing.com）、滚动、下载管理、PNG 解码（要移植 libpng）。

---

## 7. 复现命令

```powershell
cd C:\Users\fanqi\Desktop\VimtuOS\Vimtu64

# 14 个依赖库（若已就位可跳过）
C:\msys64\usr\bin\bash.exe -lc "bash tools/netsurf_lib_build_win.sh"

# NetSurf 本体（100 个 .c / 2 分钟）
C:\msys64\usr\bin\bash.exe -lc "bash tools/netsurf_patch_fetch.py"          # 只需跑一次（幂等）
C:\msys64\usr\bin\bash.exe -lc "bash tools/netsurf_core_build_win.sh"

# 前端 + 链接
C:\msys64\usr\bin\bash.exe -lc "bash tools/browser_build_win.sh"

# 装卷 / 夹具
py -3 tools/browser_pack_win.py --fixture-img build64/browser64_test.img `
      --from-vol build64/tlsvol.img --browser build64/browser.elf --on --system build64/system.img
```

---

## 8. 纪律（沿用）

- 结论必须有**串口打点 / 像素判据 / 宿主侧独立解析**三者之一；拿不到就写 `UNVERIFIED`。
- 「部分绿」不许冒充「全绿」；判据与事实不符时先修脚本，**但不许放宽强度**。
- 长跑/回归一律串行；收尾必须 `Get-Process qemu-system-x86_64` = 0、`git status --short` 只剩 `Fonts/*.ttf`。
- `PowerShell` 内联 `py -3 -c` 带中文/引号极易挂 → 一律写 `.py` 文件。
- `Edit` 工具行号极易错位 → 改前必须 fresh `Read`；批量改优先写 python 按**唯一字面串**替换并 `assert count==1`。

---

## 9. 第三轮：**客人里已经画出窗口了**（本节证据全部来自真机串口）

```
[BROWSER64] launch flag=/etc/browser.on pid=26 start_rc=0 wait=0
[BROWSER] register rc=0 window_create=1 corewindow=1
[BROWSER] init ok
[BROWSER] net dns host=example.com rc=0 ip=104.20.23.154 ms=364
[BROWSER] net http pollout rc=1 rev=4 (4=POLLOUT)
[BROWSER] net http connected=1 sent=62/62
[BROWSER] net http rx=127 head=HTTP/1.1 200 waited=500ms
[BROWSER] render boxes=29
[BROWSER] paint frame=1 ink=0.0%
[BROWSER] paint frame=2 ink=1.4%
[WL64] decor id=1 pid=26 ... title="VimtuOS browser"
[DOCK64] item idx=9 app=0 name=VimtuOS browser ... kind=running win=10001
```

- `netsurf_register` rc=0、window/corewindow 表都装上了；`browser_window_create` 通过；
  **排版真的跑了（boxes=29）**、**真的画了（paint frame=2, ink=1.4%）**、标题同步进内核标题栏、
  Dock 里出现"运行中"的应用项。产物 `build64/browser.elf` = **2,774,712 B**。
- 两个关键真缺陷（已修，别退回）：
  1. **`js_newheap` 返回值写反**：上游官方桩 `content/handlers/javascript/none/none.c` 里
     `js_newheap` 返回 `NSERROR_OK`（`*heap=NULL`），只有 `js_newthread` 才是 `NOT_IMPLEMENTED`。
     手写桩把两者对调 → `browser_window_initialise_common()` 第一步 `return err` →
     `browser_window_create` 返回 **31 = NSERROR_NOT_IMPLEMENTED**。修法：**直接 `#include` 上游 none.c**。
  2. **内建样式表没交数据**：`gui_fetch_table.get_resource_data` 留 NULL → 四张 `resource:*.css` 全
     `UnacceptableType` → 页面被导航到 `about:query/fetcherror` → 那张页又同样失败 → **自持循环**
     （串口每秒一条 `Completing parse`、`ink` 永远 0.0%）。修法：实现
     `get_resource_data/release_resource_data`，给 `.css` 返回合法空样式表。

### 仍未通（UNVERIFIED，别当成功）

- **HTTPS/TLS 握手不动**：明文 HTTP 链路已逐层证明（DNS ✅ / connect ✅ / POLLOUT ✅ / send 62/62 ✅ /
  收到 `HTTP/1.1 200` 127 B ✅），但 `mbedtls_ssl_handshake()` 一直停在 `WANT_READ`，既不成功也不报错。
  待抓点：`VIMTU_VERBOSE_LOG=1` 打开全部 NSLOG；怀疑 `bio_recv`"先 poll 再 recv"与
  `mbedtls_ssl_conf_read_timeout(conf, 0)` 的交互。在此之前首页用**内建 `data:text/html` 页**
  （`net target=https://example.com/` 仍做完整自检）。
- **QEMU `screendump` 的 PPM 与画布不一致**（同一时刻：浏览器数的页面区是白底 +1.4% 墨迹，PPM 里窗口区 91% 暗）。
  像素判据以 `[BROWSER] paint ... ink=` 为准（同一把尺子、直接在真实 shm 画布上数），PPM 只作辅助。
- 内建 CSS 是空占位（合法但无默认边距）；把真正的 `default.css` 打进 `get_resource_data` 即可。
- 地址栏键盘输入仍未做（`address input UNVERIFIED`）。

---

## 10. 第四轮：**真 HTTPS 网页渲染成功 + 截图**

### 根因（TLS 卡 WANT_READ 是**我们自己的** bug，不是 mbedTLS）

本内核网卡是**轮询式**（没有收包中断）。`kernel/syscall64.cpp` 的 `lx64_poll_core64()` 只在"要等待"的分支里才驱动收包：

```
if (ready) return ready;
if (timeout_ms == 0) return 0;          /* timeout=0：立刻返回，一个包都不收 */
...
if (has_sock) net64_poll64();           /* timeout>0：等的时候顺便收包 */
```

而 `vimtu.c` 的 BIO 就绪探测写的是 `poll(&p, 1, 0)` -> `sock64` 的 `rxlen` 永远是 0 -> `bio_recv` 永远返回
`MBEDTLS_ERR_SSL_WANT_READ` -> `mbedtls_ssl_handshake()` 既不成功也不报错。
**修法**：`fd_readable()` 改成 `poll(&p, 1, VIMTU_POLL_WAIT_MS=4)`（4 ms 够收一轮包，又不拖住 10 ms 一帧的
`fetch_poll`）；`fd_writable()` 给 1 ms；头注释里的错误说法一并改掉。

### 第二个真缺陷：登录后浏览器窗口消失

前端只在 `need_paint` 时才 commit；登录（锁屏 -> 桌面）时合成器**整屏重绘**，把客户端 surface 盖掉，
截图里只剩桌面图标与 Dock（实测暗像素 bbox 正好是锁屏时钟盒）。
**修法**：事件循环里每 **500 ms** 无条件再画一帧（同一块 shm，成本 = 一次 2 MiB blit，实测 `us≈416`）。

### 判据（真机串口 + 宿主侧像素解析，均已自查日志）

```
[BROWSER] register rc=0 window_create=1 corewindow=1
[BROWSER] init ok
[BROWSER] open url=https://example.com/
[VIMTU] tls ok TLSv1.3 TLS1-3-CHACHA20-POLY1305-SHA256 host=example.com
[VIMTU] http code=200 host=example.com
[VIMTU] fin body=589 content_len=18446744073709551615 host=example.com
[BROWSER] render boxes=22
[BROWSER] paint frame=6 ink=1.3%      （7..90 稳定 1.3%，保活重贴在起作用）
[WL64] decor id=1 pid=26 ... title="Example Domain"
```

| 像素判据（`docs/images/browser_https_example_com.png`） | 读数 |
|---|---|
| 窗口 960×540 区深色像素 | **0.81%** |
| 标题栏 25 px（含 "Example Domain"） | **0.95%** |
| Chrome 带 44 px（地址栏 + ←/→/⟳/⋯） | **0.68%** |
| 页面区正文 | **0.78%** |
| 窗口区 / 桌面区平均亮度 | **251.5 / 224.9**（窗口确实贴在屏上） |

截图：`docs/images/browser_https_example_com.png`（1280×800，QEMU `screendump` 连拍 4 张取最佳）。

### 仍如实标注的边界

- `content_len=18446744073709551615`（= `(size_t)-1`）= 该响应**没有 Content-Length**（chunked/close 定界），
  所以"字节数 == Content-Length"对本站点不适用；真正判据是 `fin body=589` + `boxes=22` + 页面有墨迹。
- 浏览器路径上的**错证书必须失败**负向对照还没重跑（探针路径的 `https64_test` 22/22 仍有效）。
- Chrome 按钮已在代码里接真值，但**自动化点击证据仍缺**（`UNVERIFIED`）。


---

## 11. 第五轮：文字重叠修复 + 地址栏真交互（已验证）

### ① 文字重叠/错位的真根因：两个度量真源

`vimtu_canvas_text()` 的推进量用的是字模自带的 `g.adv`（8px），而 `vimtu_layout_width/position/split`
用的是常量 `VIMTU_FONT_ADV`（也是 8px）。两者平时相等，一旦字模里出现更宽字形（'m' 是 9px），
「量」与「画」就分叉：NetSurf 按 8px/字排版、绘制按 9px 推进 -> 后面的字符整体右移、行尾与后续片段互相压叠；
而且 8px 步进本身让相邻字母贴在一起（无字距）。

修法（两处，缺一不可）：
1. `vimtu_canvas_text()` 推进改为 `vimtu_font_advance(fstyle)` —— 与 layout 同一个函数，不再有两个真源。
2. `VIMTU_FONT_ADV` 8 -> 9（8px 字身 + 1px 字距）。

像素判据（宿主侧独立统计同一行文本的空白列结构）：

| | 1px 空白列（字母贴死） | >=6px 空白（词间距） | 该行墨迹列数 |
|---|---|---|---|
| 修复前 `browser_https_example_com.png` | 5 | 6 | 356 |
| 修复后 `browser_typed_nav.png` | **0** | 6 | 318 |

### ② 地址栏：真键盘输入（同时挖出第三个真缺陷）

- 缺陷：键盘事件一条都到不了应用。根因是**键盘焦点必须显式申请**
  （`user/lib/vimtu64.h` 的 `V64_EV_FLAG_FOCUS = 0x1`）；不申请，`wl_dispatch` 只有鼠标事件。
  修法：事件循环里每 2 s `v64_input_poll(dummy, 0, 1u)` 重申一次（窗口重新聚焦/合成器重建后能自愈）。
- 输入语义（`kernel/input64.h`：KEY_DOWN 的 `code` 就是可打印 ASCII）：
  可打印 ASCII 追加、`0x08/0x7F` 退格、`0x0D` 回车导航、`0x1B` ESC 取消；
  非编辑态下**任意可打印键自动聚焦地址栏**，第一个字符清掉旧 URL（等价 Edge 全选）。
- 回车时没写协议自动补 `https://`（`nsurl_create` 不接受裸主机名）。

真机判据（串口，逐字可见）：

```
[BROWSER] address implicit focus
[BROWSER] address key 'e' len=1 url=e
...（11 个字符逐个回显）
[BROWSER] address key 'm' len=11 url=example.com
[BROWSER] address nav url=https://example.com
[BROWSER] render boxes=23
```

截图：`docs/images/browser_typed_nav.png`。

### ③ 本轮没做的（如实）

- **下载管理**：`gui_download_table` 仍是 NULL（走核心默认实现），点下载链接不会有文件落地。
  真跑通要补 `download_context` 四个回调 + 往 `/Downloads` 写文件的 VFS 路径。
- **没有发版**：发版闸门（`version64` 8/8、`https64` 22/22、`rand64` 17/17、`netping64` PASS、
  `iso64` 48/48 + 浏览器用例）本轮未跑，按纪律「缺一条就只提交代码 + 交接单，不发版」。
- 试玩盘（不是 Release）：`build64/sysdisk.img`（42,560,000 B = 77,265 扇区），
  内含 `/bin/browser` + `/etc/browser.on`（开机自动拉起浏览器），卷内逐字节回读通过
  （`build64/browservol.img` = 25,876,992 B）。
