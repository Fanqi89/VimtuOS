> [← 返回主页](../README.md) · VimtuOS 文档体系

# HANDOVER v0.12.3 —— 密码学熵（CSPRNG）+ mbedTLS/NetSurf 源码落地

- 日期：2026-11-26
- 版本：`0.12.3`（`build64.sh` 已 bump；**未发版** —— v0.12.2 刚发，这版是"网络与浏览器攻坚"的第一步）
- 上游基线：`174de1da`（= v0.12.2 发版提交）
- 内核映像：**3,592,832 B**（v0.12.2 是 3,584,096 B，**+8,736 B**）；闸门 15,728,640 B
- 需求原文：`C:\Users\fanqi\Desktop\网络与浏览器生态.txt`

---

## 1. 一句话

按用户"先解决安全随机数，再拉 mbedTLS；然后拉 NetSurf 全家桶"的顺序执行：
**① `getrandom(318)` 换成真 CSPRNG（RDRAND/RDSEED + SHA-256 计数器 DRBG），两条路径都验过；
② mbedTLS 3.6.2 源码到位并用我们自己的 musl 工具链**编出 `libmbedtls.a`（108/108 个 .c，1,662,810 B）；
③ NetSurf 与 12 个依赖库源码全部到位**（含 libcss/libdom/libhubbub/libparserutils/libwapcaplet 等）。

---

## 2. 密码学随机数（这是 TLS 的前置硬条件）

### 2.1 为什么必须先做

旧 `getrandom(318)`（`kernel/syscall64.cpp`）是 `TSC + ticks + xorshift`，注释自己写着
"**不是密码学安全随机**"。TLS 的会话密钥、client random、ECDHE 私钥、DRBG 种子全从它来 ——
弱随机等于没有加密。

### 2.2 新实现（`kernel/rand64.cpp` / `rand64.h`）

| 层 | 做法 |
|---|---|
| 熵源探测 | `CPUID.01h:ECX bit30` = RDRAND；`CPUID.07h:EBX bit18` = RDSEED（启动期探一次，打点） |
| 播种 | RDSEED（64 B，真熵）→ 退 RDRAND → 再退 **TSC 抖动 32 B**（连续 `rdtsc` 差值最低位 + tick 混入） |
| 出字节 | 有 RDRAND 直接取（每 1 KiB 用 RDSEED 重新播种）；否则 SHA-256 **计数器 DRBG**（`block=SHA256(state‖ctr)`，出块即 rekey，后向安全） |
| 兜底 | 输出永不返回全 0 |
| 接线 | `lx64_getrandom64` 改为 `rand64_bytes64`，单次上限 256 B → **4 KiB**（mbedTLS 的 DRBG 一次要 384 B 以上）；用户缓冲写入包 `STAC/CLAC`（SMAP） |

### 2.3 验收（`tests/rand64_test.py`，**新增，17/17 PASS**）

```
① 默认 CPU（qemu64，无 RDRAND）
   [RAND64] selftest PASS csprng=sha256-ctr drbg=1 rdrand=0 rdseed=0 seed_src=tsc-jitter seed_bytes=32 hottest=12 sample=D706A387
② -cpu max（有 RDRAND/RDSEED）
   [RAND64] selftest PASS csprng=sha256-ctr drbg=1 rdrand=1 rdseed=1 seed_src=rdseed seed_bytes=64 hottest=9 sample=8BA5C025
```

判据：自检 PASS、两条路径**种子来源不同**（真降级过）、`hottest ≤ 12`（1024 字节落 256 桶，期望 4）、
`sample ≠ 00000000`、全程无 PANIC。

> **如实记一条隐患**：默认 CPU 那条 `hottest=12` **正好压在上限**（阈值就是 12）。
> 说明 TSC 抖动路径的字节分布偏斜接近边界。下一版应加 whitening（如每次出字节再过一次 SHA-256 压缩）
> 或提高采样量，并把阈值按实测收紧/放宽 —— 现在这条判据"能过但不宽裕"，不许当成"分布很好"。

---

## 3. mbedTLS：源码到位 + 编译打通

| 项 | 结果 |
|---|---|
| 源码 | `third_party/mbedtls/`（`Mbed-TLS/mbedtls` **v3.6.2** tarball，5,247 KB，1,099 个成员） |
| 工具链 | 与 busybox/lua/tcc 同一套：`clang -target x86_64-linux-gnu -nostdinc -isystem third_party/musl/...` |
| 产物 | **`build64/mbedtls/libmbedtls.a` = 1,662,810 B**，`[MBEDTLS] cc ok=108 fail=0 objs=108` |
| 构建入口 | `tools/mbedtls_build_win.sh`（`MBEDTLS_DIR` / `MUSL_DIR` 可覆盖；失败打印前 20 条错误并 exit 1） |
| 还没做 | ① **自定义熵回调**（默认走 `mbedtls_entropy` 的 `/dev/urandom`，我们系统没有 —— 必须改成 `mbedtls_hardware_poll` 直接调内核 `getrandom(318)`）；② 配置裁剪（只留 TLS1.2/1.3 客户端 + X.509 + AES-GCM/ChaCha20-Poly1305 + SHA-2 + ECDHE）；③ 一个 ring3 的 `https://` 探针程序 |

---

## 4. NetSurf 全家桶：源码到位

`py -3 tools/fetch_port_sources.py`（**新增**，用 codeload tarball 而不是 git clone，带重试与"关键文件存在"判据）

| 目录 | .c 数 | 说明 |
|---|---|---|
| `third_party/netsurf/`（本体） | content 78 / desktop 31 / frontends 245 / utils 31 | 引擎核心 + 各前端（我们只取核心，前端自己写） |
| `netsurf/libcss` | 198 | CSS 解析/选择器/层叠（依赖最多的一坨） |
| `netsurf/libdom` | 106 | DOM |
| `netsurf/libhubbub` | 42 | HTML5 解析 |
| `netsurf/libparserutils` | 26 | 字符集/输入流 |
| `netsurf/libnsgif` / `libnsbmp` / `libsvgtiny` / `librosprite` | 3 / 1 / 8 / 4 | 图片 |
| `netsurf/libwapcaplet` / `libnsutils` / `libnspsl` / `libnslog` | 3 / 4 / 2 / 4 | 字符串驻留 / 杂项 / Public Suffix List / 日志 |
| `expat` / `zlib` | 221 / 76 | XML（libdom 的 XML 解析）/ 压缩 |

> 源码抓取踩坑（写进脚本注释）：Windows 上 tar 里的**符号链接**会抛 `OSError(22)` 把整包解到一半，
> 现在跳过非普通文件；另外"关键文件"最初是我凭印象写的路径，8 个库里有 8 个写错 ——
> 判据改成"目录 + .c 计数"，不再靠猜文件名。

---

## 5. 本版改动文件

| 文件 | 改动 |
|---|---|
| `kernel/rand64.cpp` / `.h` | **新增**：CSPRNG（RDRAND/RDSEED + SHA-256 计数器 DRBG + TSC 抖动播种） |
| `kernel/syscall64.cpp` | `getrandom(318)` 改接 CSPRNG；上限 256 B → 4 KiB；写用户缓冲加 `STAC/CLAC` |
| `kernel/kernel64.cpp` | include + 启动期 `rand64_selftest64()`（`[RAND64] selftest …`） |
| `build64.sh` | `VIMTUOS_VERSION="0.12.3"`；`rand64.cpp` 进 `SRCS_CORE` + **两个内核的链接行都加 `rand64.o`**（漏加过 → `undefined symbol: rand64_bytes64`，安装程序内核先挂） |
| `tools/fetch_port_sources.py` | **新增**：16 个上游仓库 tarball 抓取（重试 + 符号链接容错 + `.c` 计数判据） |
| `tools/mbedtls_build_win.sh` | **新增**：用我们的 musl 工具链编 `libmbedtls.a` |
| `tests/rand64_test.py` | **新增**：CSPRNG 两条路径验收（17 条断言） |

`:nothing to see` —— 本版**没动**网络代码（v0.12.2 的 socket 层保持不动）。

---

## 6. 验收

| 项 | 结果 |
|---|---|
| 构建（两个内核） | **PASS**，内核 **3,592,832 B**（+8,736 B），安装程序内核同样过 |
| `version64_test` | **8/8 PASS**（`VimtuOS 0.12.3`；旧串 0.12.2 = 0 次） |
| `rand64_test`（新） | **17/17 PASS**（两条熵路径 + 分布 + 非常数 + 无 PANIC） |
| mbedTLS 编译 | **PASS** `ok=108 fail=0`，`libmbedtls.a` 1,662,810 B |
| NetSurf 全家桶 | **源码到位**（见 §4 的 .c 计数）；**编译未开始** |
| v0.12.2 发版 | 已完成（见 `HANDOVER_v0122_net_l4.md` §11：tag `b72ccdb0…` / Release `458890837`） |

---

## 7. 下一轮（严格按依赖顺序）

1. **mbedTLS 端口层**：`MBEDTLS_ENTROPY_HARDWARE_ALT` + 我们自己的 `mbedtls_hardware_poll()`（调 `getrandom(318)`）；
   裁剪 `mbedtls_config.h`；写 `tests/tls64_test.py` 的**握手判据**（含"错证书必须失败"的负向对照）。
2. **可信时间**：证书有效期要判，本内核 RTC 从 CMOS 读、无 NTP → 先做 SNTP 或"时间未知时大声降级"。
3. **`/bin/wget`（HTTP/1.1）** → 再升级成 `https://`（接 mbedTLS），这是 NetSurf fetcher 的原型。
4. **NetSurf 依赖库逐个编译**（libwapcaplet → libparserutils → libhubbub → libcss → libdom → 图片库），
   每编完一个都要有 `.a` + 符号数判据。
5. **NetSurf 前端**：`frontends/vimtu/`（wl_surface 上画，Edge 风格外壳：标签页/地址栏/前进后退/下载管理）。
6. 遗留：CSPRNG 分布偏斜（§2.3 那条 `hottest=12`）。

---

## 8. 源码**不进仓**（发版流程的真实约束）

mbedTLS / NetSurf 全家桶 / expat / zlib 一共 **~38 MB、约 3,800 个文件**。第一次把它们提交进去后：
**GitHub 的 Git Data API 创建 tree 直接 502**（`POST /git/trees` 单次几千个条目过网关就挂），
而本仓的推送通道正是 API 逐提交回放（`git push` 在这台机器上一直被 `Connection was reset` 打断）。

所以按仓里既有的"字体不进仓"惯例：**源码不进仓，脚本按钉死的 SHA 现拉**。

- `.gitignore` 里加了 `third_party/{mbedtls,netsurf,expat,zlib}/`（带注释说明）
- `tools/fetch_port_sources.py` 的 `TARGETS` 里**版本钉死**：
  mbedTLS `v3.6.2`；NetSurf 全家桶到具体 commit（`netsurf` `170d89462442`、
  `libcss` `0c4f51901a15`、`libdom` `e4150b65e4e7`、`libhubbub` `68989e60c786`、
  `libparserutils` `c9971265064b`、`libwapcaplet` `74e49210e9c5`、`libnsgif` `b9e3b0aadce3`、
  `libnsbmp` `3406e7e0bfb5`、`libnsutils` `4f0b88241304`、`libnspsl` `fb372b56cc83`、
  `libnslog` `6c3445f5e78c`、`libsvgtiny` `848424e7328e`、`librosprite` `5a529e37891b`；
  `expat` `R_2_6_4`、`zlib` `v1.3.1`）—— 不随上游漂。
- 另：tar 里的**符号链接**在 Windows 上会抛 `OSError(22)`（NetSurf 有 179 个），
  第一次提交时它们变成 git 的 `120000` 条目、API 通道直接拒收（"删除文件或链接"）——
  现在抓取时跳过非普通文件，仓里**一个符号链接都没有**。

## 9. 现场

```
HEAD            = `9ae5ac3738f112fa7f06357fc016dc762a47b44d`（= 远端 main）
上游基线        = 174de1da（v0.12.2 发版提交）
内核映像        = 3,592,832 B（闸门 15,728,640 B）
mbedTLS         = third_party/mbedtls（v3.6.2）/ build64/mbedtls/libmbedtls.a 1,662,810 B
NetSurf 全家桶  = third_party/netsurf/（本体 + 12 个依赖库）
CSPRNG 证据     = [RAND64] selftest PASS csprng=sha256-ctr drbg=1 rdrand=0/1 seed_src=tsc-jitter|rdseed …
```

### 复现命令

```powershell
py -3 tools\fetch_port_sources.py          # 拉 mbedTLS + NetSurf 全家桶源码（幂等：已就位会 SKIP）
bash build64.sh                            # 12~14 分钟
py -3 tests\version64_test.py              # 8/8
py -3 tests\rand64_test.py                 # CSPRNG 17 条
C:\msys64\usr\bin\bash.exe -lc "bash tools/mbedtls_build_win.sh"   # ok=108 fail=0
```

---

## 10. README 截图重拍（同一轮的追加工作）

用户反馈"README 里的系统截图都不是当前系统最新的"。于是新写了
**[`tools/shots_fresh.py`](../tools/shots_fresh.py)**，从**当前构建**（`build64/system.img`，
内核 3,592,832 B / v0.12.3）现拍 9 张，替换掉 README 里那批旧图：

| 文件 | 内容 | 这一帧的证据（串口） |
|---|---|---|
| `docs/images/01_lock.png` | 锁屏 | `[LOCK64] bg blur ready` |
| `docs/images/02_desktop.png` | 桌面 | `[GUI64] ready` |
| `docs/images/03_startmenu.png` | 开始菜单 | `[START64] open why=win-key`（网格 `apps=14 cells=8`） |
| `docs/images/04_terminal.png` | 终端 | `[APP] term opened cols=120 rows=37` |
| `docs/images/05_mypc.png` | 我的电脑 | `[START64] search q=mypc hits=1 first=My Computer` + `[APP] mypc opened` |
| `docs/images/06_settings.png` | 设置 | `… q=settings hits=1 first=Settings` + `[APP] settings opened` |
| `docs/images/07_tmgr.png` | 任务管理器 | `… q=tmgr hits=1 first=Task Manager` + `[APP] tmgr opened` |
| `docs/images/08_about.png` | 关于（**页面上就是版本号**） | `… q=about hits=1 first=About VimtuOS` + `[APP] about opened` |
| `docs/images/09_calc.png` | 计算器 | `… q=calc hits=1 first=Calculator` + `[APP] calc opened` |

全部 1280×800，逐张与 `02_desktop` 做过差异像素统计（4.8% / 66.4% / 21.7% / 11.2% / 40.5% /
17.7% / 19.3% …），确认**不是同一帧重复截图**；每张的 md5 互不相同。

### 重拍过程踩的两个坑（都已修进脚本注释）

1. **数字快捷键 ≠ 开始菜单网格顺序**。我先按 `[START64] grid` 的顺序按数字（期望 "6"=终端），
   实际开的是**设置** —— 全拍错了。改成走**搜索框**：串口打
   `[START64] search q=<词> hits=N first=<App 名> (app=<id>)`，命中唯一时回车才开对。
2. **键盘在 1 fps 的 GUI 下会丢字**：`calc` 只到 `c`，于是打开了 "App Store"。修法：
   打完字先**读回 `search q=…`**，只有"读回来的词 == 目标词"才按回车，否则重打（最多 4 次）。
   这条和 v0.12.2 里 `wget` 长命令被吞是同一个根因，这里用"读回校验 + 重试"绕过去了。

### 没做到的（如实）

想在终端里敲 `ver` / `cat /proc/version` 把版本号也拍进图里，但**键盘打进内核终端 4 次全失败**
（终端窗口拿不到稳定焦点）。所以版本证据改成：`08_about.png` 这一页本身显示版本号 +
本节这张"每张图对应哪条串口打点"的表。**没有**把"截图里的版本号像素"做成自动断言（缺 OCR）。
