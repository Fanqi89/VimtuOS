# HANDOVER v0.12.11 · term-ui + elf-spawn —— 终端配色/换行 + 应用起不来的根因

- 日期：2026-11-28
- 版本串：`0.12.11`（`build64.sh` 唯一真源）· 内核 `kernel64_os.bin` = 3,614,400 B
- 关联单：`HANDOVER_v0129_ui_bugs.md`（焦点/菜单收口）、`HANDOVER_v0129_cjk_tier3.md`
- 本单性质：用户实机报的两类问题，**一类是阻断级根因（已修并取到实据）**，一类是 UI 细节（颜色已验、换行待像素复核）

---

## 0. 阻断级：开始菜单 / 文件管理器里启动 ELF 全部失败

**用户现象**：开始菜单点"浏览器""图片查看器"没反应；文件管理器双击 ELF，任务管理器底部红字 `[ELF64] ...`。

**实据（夹具串口，v0.12.10 原样内核）**：

```
[APP64] dispatch path=/bin/viewer magic=elf64 -> elf64_run64
[ELF64] reject reason=busy
[ELF64] launch FAILED path=/bin/viewer reason=load
[TERM] cmd run fail
```

**根因**：`app64_run_any64()` 把 ELF 分派给 `elf64_run64()` —— 那是**同步单例装载器**
（`g_loaded64` 全局只有一份，装载完要等子进程退出才 `elf64_unload64()` 回收）。
Ring 3 合成器 `/lib/wm.elf` 一起来就永久占着那份状态（启动日志里 wm 被 `kill sig=9`，
`elf64_run_loaded64()` 永远回不到 `elf64_unload64()`），此后**任何** ELF 都 `busy` 失败。
与 NetSurf 动态库、mbedTLS、KPTI/SMEP、CWD **全部无关** —— 连装载都没开始。

**修法**（`kernel/app64.cpp`）：ELF 一律走**真进程**路径 —— `proc64_create64()` +
`proc64_start_elf64()`（每进程 CR3，互不干扰），与 `kernel64.cpp` 的 tlsprobe、
`terminal64.cpp` 的 `run` 完全同一条已验收路径；`proc64_create64` 建不出进程时
（安装介质 / UEFI 固件页表 = 共享地址空间模式）回退老的同步装载器并打
`[APP64] elf spawn fallback path=… reason=no-proc`。

**新判据（已跑绿）**：

```
[APP64] elf spawn path=/bin/viewer pid=26 via=proc64_start_elf64
[APP64] run cmd path=/bin/viewer rc=0
[ELF64] load path=/bin/browser entry=0000000100090200 segs=5 size=2774704 … layout=1
[APPREG64] launch exec=/bin/browser via=search-enter kind=elf pid=27 rc=0 load_ms=30444
[BROWSER] paint frame=180 ink=1.3%
```
全程 **0 次** `[ELF64] reject reason=busy`。截图 `docs/images/v01211_browser.png`。

**新发现（下一轮要处理）**：`/bin/browser` 装载 **load_ms≈30,000（30 秒）**，2,774,704 B 的
卷读沿 segmented + chunk=16384 路径走，`lookup_hits=594`。功能通了但用户会以为"没反应" ——
下一轮第一位：按段预读/加大 chunk/页表批量映射，把它压到 2 秒内（判据：`load_ms` < 2000）。

---

## 1. 终端 UI（`kernel/terminal64.cpp`）

颜色是 **per-cell** 的：`ts->attr` 在 `ts_putc` 写字符时盖进 `attrs[]`，`term_attr_fg()`
按 `0x40 | idx | bold<<4` 解回 `kTermAnsi16[]`。所以只改两处 `shell_*`：

| 位置 | 改动 | 判据 |
|---|---|---|
| `shell_banner()` :883 | `ts->attr = 0x40|4`（ANSI 4 = `#0000AA` 深蓝），写完复位 `0` | 整屏深蓝像素 **1480** 个 |
| `shell_prompt()` :866 | `ts->attr = 0x40|0x10|2`（idx 2 + bold → `kTermAnsi16[10]` = `#55FF55` 亮绿），写完复位 `0` | 整屏亮绿像素 **761** 个 |

回车强制换行：`term_key()` 的 `'\n'` 分支里，`ts->cmdline[ts->cmdlen] = 0;` **之后、任何输出之前**
插入 `ts_putc(ts, (uint32_t)'\n');`（现 `terminal64.cpp:5611`）。这样口令/su 交互行也一起覆盖。

**验证状态（如实）**：
- 深蓝 / 亮绿：像素计数已过（`docs/images/v01211_terminal_open.png`）。
- 换行：`[TERM] cmd ver` 证明命令真的执行了；但"输出确实在下一行"这条**像素判据没跑干净** ——
  夹具用"整屏黑区 bbox"定位终端，结果框到了别的深色窗口，行分析无效（我的 `ink` 行合并阈值
  18px 行距 vs 2px 容差也不对）。**下一轮必须重做**：用 `[UI] win geom title=… app=<终端>`
  取客户区，或直接用 `[APP] term opened cols=120 rows=37` + 窗口原点算单元格，再按
  "提示符行墨点宽 < 45% 终端宽"+"下一行墨点左缘 ≈ 左边界"判定。

---

## 2. 已验证 / 未验证清单

| 项 | 状态 | 证据 |
|---|---|---|
| ELF 启动（viewer/browser，终端与开始菜单两条入口） | **已验证** | `[APP64] elf spawn … pid=26` / `[APPREG64] launch … pid=27 rc=0` + `[BROWSER] paint frame=180` |
| `[ELF64] reject reason=busy` 绝迹 | **已验证** | 全程 grep 为 0 |
| 欢迎信息深蓝 | **已验证** | blue_px=1480 |
| 提示符亮绿 | **已验证** | green_px=761 |
| 回车强制换行 | **代码已改；像素判据 UNVERIFIED** | `[TERM] cmd ver` 执行；截图待人工看 |
| 浏览器装载耗时 30 s | **缺陷（未修）** | `load_ms=30444` |
| 图片查看器从开始菜单（不带文件名） | **未验证** | 需确认无参启动是开选择器还是直接退出 |

**回归**：本轮只跑 `cc2.sh` 单文件编译 + 完整 `build64.sh`（EXIT=0，内核 3,614,400 B / ISO 97,067,008 B）。
`fileops64` / `deskfolder64` / `version64` 等**未重跑** —— 下一轮开工先补跑，尤其
`version64_test`（版本串已 bump 到 0.12.11）。

## 3. 下一轮开工顺序

1. `version64_test` + `fileops64` + `deskfolder64` 全量回归（bump 后必跑）。
2. 浏览器 `load_ms` 从 30 s 压到 2 s 内（判据 `load_ms` < 2000）。
3. 换行像素判据重做（见 §1 末尾的定位方法），把它固化成 `tests/` 里的用例。
4. 图片查看器无参启动语义（开选择器 / 打开默认图 / 给提示），别静默退出。

## 4. 踩坑（本轮新增）

- **`PowerShell` 内联 `py -3 -c` 带引号必挂**：本轮又挂 3 次（`VIMTUOS_VERSION=\"…\"` 直接被吃掉）。
  一律写 `.py` 文件再跑。
- **`[ELF64] reject reason=busy` 会伪装成"库找不到/KPTI 误伤"**：先看是 `reject` 还是 `load FAILED`；
  `reject reason=busy` = 单例装载器被占，跟被测程序一个字节都没关系。
- **夹具判据要锚在窗口几何上**：用"整屏黑区 bbox"找终端 = 必然框错窗口（本轮踩）。
- 串口**没有** `VimtuOS x.y.z` 行（版本串只在内核启动横幅/屏幕上），别再拿它当版本判据。
