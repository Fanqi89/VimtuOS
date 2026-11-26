> [← 返回主页](../README.md) · VimtuOS 文档体系

# HANDOVER v0.12.3 · net-ready —— 封存单（网络与浏览器攻坚第一段）

- 日期：2026-11-26
- 版本：`0.12.3`（`build64.sh` 已 bump，**未发版** —— 发版门槛见 §9.4）
- 上游基线：`174de1da`（= **v0.12.2 已发版**的提交）
- 本单 HEAD：`5f2b44ff`（= 远端 `main`，tree 回读 14,742 项 / 0 坏项）
- 内核映像：**3,592,832 B**（v0.12.2 是 3,584,096 B，**+8,736 B**）；闸门 15,728,640 B
- 用户需求原文：`C:\Users\fanqi\Desktop\网络与浏览器生态.txt`
- 配套单（细节记录，按"旧单只增不改"保留）：`HANDOVER_v0123_rand_mbedtls.md`（本单是它的**汇总 + 封存**，
  含同一版本的全部结论；两份冲突时以本单为准）

> **本单是"停止开发、立刻封存"时写的**。它的用途：让一个**零上下文**的会话读一份文件就能接着干，
> 不重做已完成的事、不掉进已经踩过的坑。

---

## 1. 一句话

网络与浏览器攻坚的**第一段**做完了：**① 硬件级随机数（CSPRNG）已落地并双路径验收；
② mbedTLS v3.6.2 源码到位并用我们自己的 musl 工具链编出静态库（108/108 个 .c，1,662,810 B）；
③ NetSurf 全家桶 13 个库 + expat + zlib 源码全部到位（按钉死的 commit SHA 可复现）。
④ 顺带把 README 的系统截图换成当前构建现拍的 9 张。**
**下一段从"给 mbedTLS 接熵回调 + HTTPS 探针"开始**（§9 路线图）。

---

## 2. 现场（复现前先核这三条）

```
git log --oneline -3      -> 5f2b44ff / a7109e77 / 9ae5ac37
git status --short        -> 空（工作区干净）
ls build64/kernel64_os.bin -> 3,592,832 B   (VIMTUOS_VERSION="0.12.3")
Get-Process qemu-system-x86_64 -> 0
```

| 项 | 值 |
|---|---|
| HEAD / 远端 main | **`5f2b44ff`**（推走后回读：`remote main after = 5f2b44ff… MATCH`，tree 14,742 项 bad=0） |
| 已发版 tag | `v0.12.2`（annotated，tag 对象 `b72ccdb03d9a213211a96111803b34749588f3f5`） |
| 已发版 Release | **`458890837`**（draft=false / pre=false，2 附件） |
| ISO | 93,458,432 B `sha256:8b63efe7d08cde55e56c4cb7eecdeb3cc63da3c65cd24b1899db130896b9f5a4` |
| IMG | 42,684,928 B `sha256:9688b1d3b392af15e73725cec35e1acaa45ac7d69dd09c81f4865c756ae68d59` |
| 旧版 v0.12.1 附件 | img id `790695413` / iso id `790694664`，id+size+digest+updated_at **逐条未动** |
| mbedTLS 静态库 | `build64/mbedtls/libmbedtls.a` = **1,662,810 B** |
| 上游源码 | `third_party/{mbedtls,netsurf,expat,zlib}/`（**不进仓**，脚本按 SHA 现拉，见 §6.3） |

---

## 3. 已完成的三件事（每件都给硬数字 + 判据 + 复现）

### 3.1 ① 硬件级随机数生成器（CSPRNG）—— 完成

**为什么必须先做**：旧 `getrandom(318)` 是 `TSC + ticks + xorshift`，代码注释自己写着
"**不是密码学安全随机**"。TLS 的会话密钥 / client random / ECDHE 私钥 / DRBG 种子全从这里来，
弱随机等于没有加密 —— 不先修这个，接 mbedTLS 就是在做样子。

**实现**（`kernel/rand64.cpp` + `rand64.h`，新增 296 行）：

| 环节 | 做法 |
|---|---|
| 熵源探测 | `CPUID.01h:ECX bit30` = **RDRAND**；`CPUID.07h:EBX bit18` = **RDSEED**（启动探一次，结果进打点） |
| RDRAND 取数 | `rdrand %r0 / setc`，CF=0 时最多重试 10 次（Intel SDM 建议） |
| RDSEED 取数 | 同上（`rdseed`），**只用于播种**与每 1 KiB 重播 |
| 播种顺序 | RDSEED 64 B（真熵）→ 退 RDRAND 64 B → 再退 **TSC 抖动 32 B**（连续 `rdtsc` 差值最低位 256 bit + 每 8 次混入 `g_ticks64` 低字节） |
| 输出 | 有 RDRAND 直取（每 1 KiB 用 RDSEED 重新播种）；否则 **SHA-256 计数器 DRBG**：`block = SHA256(state‖ctr_be64)`，出块后 `state = SHA256(state‖block)`（**出块即 rekey**，后向安全）；丢掉第一块 |
| 兜底 | 输出永不返回全 0（最多重试 8 轮） |
| 接线 | `lx64_getrandom64` 改调 `rand64_bytes64`；单次上限 **256 B → 4 KiB**（mbedTLS 的 DRBG 一次要 384 B 以上）；写用户缓冲包 `STAC/CLAC`（SMAP） |
| 自检 | 启动期 `rand64_selftest64()`：非全 0 / 非全同值 / 字节分布（`hottest ≤ 12`，期望 4）/ 64 个 16 B 块不重复 / 两次调用不同 |

**验收（`tests/rand64_test.py`，新增 183 行，17 条断言，`RESULT: PASS`）**：

```
① 默认 CPU（QEMU qemu64，没有 RDRAND）
   [RAND64] selftest PASS csprng=sha256-ctr drbg=1 rdrand=0 rdseed=0 seed_src=tsc-jitter seed_bytes=32 hottest=12 sample=D706A387
② -cpu max（有 RDRAND/RDSEED）
   [RAND64] selftest PASS csprng=sha256-ctr drbg=1 rdrand=1 rdseed=1 seed_src=rdseed      seed_bytes=64 hottest=9  sample=8BA5C025
③ 判据：自检 PASS、两条路径 seed_src **不同**（证明真降级过）、hottest≤12、sample≠00000000、无 PANIC
```

> **遗留隐患（必须带进下一轮）**：默认 CPU 那条 `hottest=12` **正好压在阈值上限**（阈值就是 12）。
> TSC 抖动路径的字节分布偏斜接近边界。下版应加 whitening（例如每次出字节再过一次 SHA-256 压缩）
> 或提高采样量，并按实测**收紧**阈值。现在这条"能过但不宽裕"，不许当成"分布很好"。

### 3.2 ② mbedTLS —— 源码拉取 + **编译打通**

| 项 | 结果 |
|---|---|
| 上游 | `Mbed-TLS/mbedtls` **v3.6.2**（codeload tarball，5,247 KB，1,099 个成员） |
| 落点 | `third_party/mbedtls/`（只留 `library/ include/ configs/ LICENSE`，tests/programs/docs/framework 已裁掉：145 MB → 8.7 MB） |
| 工具链 | **与 busybox/lua/tcc 同一套**：`clang -target x86_64-linux-gnu -nostdinc -isystem third_party/musl/include -isystem …/arch/x86_64 -isystem …/arch/generic -isystem …/obj/include -mcmodel=large -fno-pic -fno-pie -fno-stack-protector -fno-builtin -O2` |
| 构建入口 | `tools/mbedtls_build_win.sh`（新增；`MBEDTLS_DIR` / `MUSL_DIR` 可覆盖；先 `export PATH=/usr/bin:/bin:/mingw64/bin`，否则 `clang: not found`） |
| 产物 | **`build64/mbedtls/libmbedtls.a` = 1,662,810 B**；`[MBEDTLS] cc ok=108 fail=0 objs=108` |
| 失败行为 | 任何一个 .c 编不过就 `fail>0 → exit 1`，并打印**前 20 条** `error:`（完整见 `build64/mbedtls/cc_fail.log`） |
| **还没做** | ① **自定义熵回调**（默认 `mbedtls_entropy` 走 `/dev/urandom`，我们系统没有这个设备 —— 必须改成 `MBEDTLS_ENTROPY_HARDWARE_ALT` + 我们自己的 `mbedtls_hardware_poll()` 调 `getrandom(318)`）；② `mbedtls_config.h` 裁剪（只留 TLS1.2/1.3 客户端 + X.509 校验 + AES-GCM/ChaCha20-Poly1305 + SHA-2 + ECDHE P-256/X25519）；③ ring3 的 `https://` 探针程序 |

踩过的坑：`-DMBEDTLS_CONFIG_FILE='<...>'` 这种带尖括号的宏在 bash 里会被引号吃掉
→ `error: expected "FILENAME" or <FILENAME>`。**直接不定义**（build_info.h 自己有默认值）。

### 3.3 ③ NetSurf 全家桶（13 个库）—— 源码拉取完成

`tools/fetch_port_sources.py`（新增 111 行）：用 **codeload tarball**（不是 git clone）+ 重试 +
符号链接容错 + "关键文件/目录存在"判据，**幂等**（已就位会打 `SKIP`），最后一行 `===== ALL OK =====`。

**钉死的版本（不随上游漂）**：

| 仓库 | ref | 本地目录 | .c |
|---|---|---|---|
| `netsurf-browser/netsurf` | `170d89462442` | `third_party/netsurf/` | content 78 / desktop 31 / frontends 245 / utils 31 |
| `libcss` | `0c4f51901a15` | `netsurf/libcss` | 198 |
| `libdom` | `e4150b65e4e7` | `netsurf/libdom` | 106 |
| `libhubbub` | `68989e60c786` | `netsurf/libhubbub` | 42 |
| `libparserutils` | `c9971265064b` | `netsurf/libparserutils` | 26 |
| `libwapcaplet` | `74e49210e9c5` | `netsurf/libwapcaplet` | 3 |
| `libnsgif` | `b9e3b0aadce3` | `netsurf/libnsgif` | 3 |
| `libnsbmp` | `3406e7e0bfb5` | `netsurf/libnsbmp` | 1 |
| `libnsutils` | `4f0b88241304` | `netsurf/libnsutils` | 4 |
| `libnspsl` | `fb372b56cc83` | `netsurf/libnspsl` | 2 |
| `libnslog` | `6c3445f5e78c` | `netsurf/libnslog` | 4 |
| `libsvgtiny` | `848424e7328e` | `netsurf/libsvgtiny` | 8 |
| `librosprite` | `5a529e37891b` | `netsurf/librosprite` | 4 |
| `libexpat/libexpat` | `R_2_6_4` | `expat/` | 220（只要 `expat/lib/`） |
| `madler/zlib` | `v1.3.1` | `zlib/` | 76 |

**编译：一个都还没开始。** 建议顺序见 §9.3。

---

## 4. README 截图重拍（同一轮追加，已提交）

- 新脚本 **`tools/shots_fresh.py`**：起当前构建的 `build64/system.img`，键盘驱动到 9 个状态，
  每张 `screendump` → Pillow → PNG 落 `docs/images/`；**每张都有串口打点证明"这一帧是什么"**。
- 产物：`01_lock / 02_desktop / 03_startmenu / 04_terminal / 05_mypc / 06_settings / 07_tmgr / 08_about / 09_calc`
  （全部 1280×800；与 `02_desktop` 的差异像素 4.8% / 66.4% / 21.7% / 11.2% / 40.5% / 17.7% / 19.3%；md5 两两不同）。
- **顺手修的真问题**：`.gitignore` 里 `docs/images/` 是**整目录忽略** → README 原来引用的
  `docs/images/GUI2.png` 根本不在仓里（GitHub 上是坏图）。现在改成 `docs/images/*` +
  `!docs/images/0[1-9]_*.png`，9 张图确认被 git 跟踪。
- **两个坑**（写进脚本注释）：① 开始菜单数字快捷键**不是** `[START64] grid` 的顺序（按 grid 按 "6"
  期望终端，实际开的是设置）→ 改走搜索框；② 键盘在 1 fps 的 GUI 下**会丢字**（`calc` 只到 `c`）
  → 打完字先读回 `search q=…`，词对上才回车，否则重打（最多 4 次）。
- **没做到**：想把 `ver` 的版本号也拍进图里，键盘打进内核终端 4 次全失败（拿不到稳定焦点）。
  版本证据 = `08_about.png` 页面上的版本号 + §4 对照表；**没有** OCR 级自动断言。

---

## 5. 验收总表（含未验证项，一条都不许抹）

| 项 | 结果 | 证据 |
|---|---|---|
| 构建（系统内核 + 安装程序内核两条） | **PASS** | `build64.sh` EXIT=0；内核 3,592,832 B |
| `version64_test` | **8/8 PASS** | `VimtuOS 0.12.3 x86_64`；旧串 `0.12.2` 在内核里 **0 次** |
| `rand64_test`（新） | **17/17 PASS** | 见 §3.1 的两行 `[RAND64] selftest` |
| mbedTLS 编译 | **PASS** | `[MBEDTLS] cc ok=108 fail=0 objs=108`，1,662,810 B |
| NetSurf 全家桶源码 | **PASS（到位）** | `===== ALL OK =====` + 各库 .c 计数（§3.3） |
| README 截图 | **PASS** | 9 张、`tools/shots_fresh.py` 六步全 PASS、图全被 git 跟踪 |
| v0.12.2 发版 | **PASS** | Release `458890837`；旧版附件逐条未动 |
| **2 MiB / 16 MiB 下载判据** | **UNVERIFIED（承 v0.12.2）** | 长命令被 QEMU `sendkey` 吞掉；见 `HANDOVER_v0122_net_l4.md` §6 |
| **HTTPS 握手** | **未开始** | 库能编 ≠ 能握手（缺熵回调/时间/探针） |
| **NetSurf 编译与前端** | **未开始** | 源码到位，一行都没编 |

---

## 6. 关键机制与决策（下一轮必须遵守）

### 6.1 两条网络栈（承 v0.12.2）

- **A. 内核 socket 层**（`kernel/net64.cpp` + `sock64.cpp` + `syscall64.cpp`）—— 真实应用走这条；
  v0.12.2 已做：按序号收 + 64 KiB 真窗口 + TCP 校验和 + 单段 RTO 重传 + SYN MSS=1460 + `-EAGAIN`
  + `poll` 看得见 socket（`sock64_ready64`）+ `recvmsg(47)/sendmsg(46)` + `/etc/resolv.conf` 预置。
- **B. Ring 3 `netd`**（`user/net/netd.c`）—— 自带栈的工具进程，只有它自己用。
- **纪律**：浏览器 / TLS 都是 ring3 程序 → 只能走 A；**不许在内核里解析 HTTP/TLS**（格式解析器不拿 ring0 权限）。

### 6.2 源码不进仓（这是推送通道的硬约束，不是偏好）

- mbedTLS + NetSurf 全家桶 + expat + zlib ≈ **38 MB / ~3,800 个文件**；
  第一次提交后 **GitHub `POST /git/trees` 直接 502**（本仓推送靠 Git Data API 逐提交回放，`git push` 一直被 reset）。
- NetSurf 里 **179 个符号链接**在 Windows 上解包会抛 `OSError(22)`，进仓后变成 `120000` 条目、
  API 通道再拒收一次（报"删除文件或链接"）。
- 现状：`.gitignore` 排除 `third_party/{mbedtls,netsurf,expat,zlib}/`，**脚本按 §3.3 的 SHA 现拉**；
  仓里一个符号链接都没有。

### 6.3 构建脚本的两个手写点（改内核源文件时必看）

1. `build64.sh` 的 `SRCS_*` 决定**编译**，但**两个内核的链接行是手写对象列表** ——
   新增 .c 除了进 `SRCS_CORE`，还要在 `-o "$BUILD/kernel64.elf"` 与 `-o "$BUILD/kernel64_os.elf"`
   两行里都加 `xxx.o`。**本轮就漏了这条** → `undefined symbol: rand64_bytes64`（安装程序内核先挂）。
2. `VIMTUOS_VERSION` 只能有**一个**定义（构建期有断言）。

---

## 7. 本段改动文件

| 文件 | 性质 |
|---|---|
| `kernel/rand64.cpp` / `rand64.h` | **新增**：CSPRNG（RDRAND/RDSEED + SHA-256 计数器 DRBG + TSC 抖动播种 + 自检） |
| `kernel/syscall64.cpp` | `getrandom(318)` 改接 CSPRNG；上限 4 KiB；写用户缓冲加 `STAC/CLAC` |
| `kernel/kernel64.cpp` | include + 启动期 `rand64_selftest64()` |
| `build64.sh` | `VIMTUOS_VERSION="0.12.3"`；`rand64.cpp` 进 `SRCS_CORE` + **两条链接行各加 `rand64.o`** |
| `tests/rand64_test.py` | **新增**：CSPRNG 两条路径验收（17 条断言） |
| `tools/fetch_port_sources.py` | **新增**：16 个上游仓库 tarball 抓取（钉 SHA + 重试 + 符号链接容错 + .c 计数） |
| `tools/mbedtls_build_win.sh` | **新增**：用我们的 musl 工具链编 `libmbedtls.a` |
| `tools/shots_fresh.py` | **新增**：README 截图现拍（9 张 + 逐帧串口证据） |
| `docs/images/0[1-9]_*.png` | **新增**：9 张当前构建的截图（`.gitignore` 已放行） |
| `README.md` | 截图区整体替换（含"怎么拍/什么证据"的说明） |
| `.gitignore` | 放行 9 张 README 截图；排除 third_party 上游源码 |
| `docs/handover/HANDOVER_v0122_net_l4.md` | v0.12.2 交接单（含 §11 发版回读） |
| `docs/handover/HANDOVER_v0123_rand_mbedtls.md` | 本版细节记录（本单是它的汇总） |
| `docs/handover/HANDOVER_v0123_net_ready.md` | **本单** |
| `docs/网络与浏览器生态_v0122攻坚计划.md` | 阶段计划（盘点 / 根因 / 分期 / mbedTLS 与 NetSurf 评估） |

---

## 8. 坑清单（按"会咬人"排序，全部实测过）

1. **`build64.sh` 链接行是手写的**（§6.3.1）—— 新 .c 漏加 = 链接期 `undefined symbol`，且**安装程序内核先挂**。
2. **`docs/images/` 原来整目录忽略** —— README 引用的图可能根本不在仓里。加文件后一定 `git ls-files` 核一遍。
3. **QEMU `sendkey` 在 1 fps 的 GUI 下会丢字/吞长命令** —— 任何"打字驱动"的验收都必须
   **读回证据再判定**（开始菜单有 `search q=…`；shell 没有回显通道，所以长命令仍不可靠）。
4. **开始菜单数字快捷键 ≠ `grid` 顺序** —— 用搜索框，不要用数字索引。
5. **mbedTLS 的 `-DMBEDTLS_CONFIG_FILE='<...>'`** 会被 bash 引号吃掉（§3.2 末）。
6. **tar 里的符号链接在 Windows 上抛 `OSError(22)`**，会把整包解到一半（§6.2）。
7. **GitHub API 的 `POST /releases`、`GET /releases/<id>`、`POST /git/trees` 都出现过 502** ——
   所有发布/推送脚本必须**自带指数退避重试**，不能一次 502 就当失败。
8. **`hottest=12` 压在阈值上**（§3.1 隐患）—— 熵的分布判据要留余量，别让"刚好过"变成长期假绿。

---

## 9. 下一轮严格路线图（按依赖顺序，不许跳步）

### ① mbedTLS 熵回调对接 + HTTPS 探针测试  ← **从这里开始**

| 子步 | 做法 | 判据（硬） |
|---|---|---|
| ①-1 熵回调 | `mbedtls_config.h` 打开 `MBEDTLS_ENTROPY_HARDWARE_ALT`；写 `user/lib/mbedtls_vimtu_port.c` 实现 `int mbedtls_hardware_poll(void*, unsigned char* out, size_t len, size_t* olen)` → 直接调 `getrandom(318)`；**禁用** `/dev/urandom` 路径（`MBEDTLS_NO_PLATFORM_ENTROPY` 视版本） | 用固定种子跑两次 `mbedtls_ctr_drbg_random`，输出必须不同；`len==0` 时 `*olen=0` 且返回 0 |
| ①-2 配置裁剪 | 只留：TLS 1.2/1.3 客户端、X.509（含 `MBEDTLS_X509_CRT_PARSE_C`）、AES-GCM、ChaCha20-Poly1305、SHA-256/384、ECDHE（P-256/X25519）、RSA 签名验证；关 DTLS/PSK/SSLv3/服务端/自测套件 | 编出来的 `.a` 体积比现在小；`mbedtls_ssl_handshake` 链接通过 |
| ①-3 探针程序 | `user/apps/httpsprobe.c`：`connect` 443 → `mbedtls_ssl_handshake` → 打印协商结果（版本/套件/证书 CN/有效期） → GET `/` → 打印状态行 | 串口出现 `[TLS] handshake ok version=TLS1.3 suite=… cn=… ` + `HTTP/1.1 200` |
| ①-4 时间前置 | 证书要验有效期，本内核 RTC 从 CMOS 读、无 NTP | 若时间不可信：**大声打点** `[TLS] time-unknown` 并允许降级，**绝不静默跳过校验** |
| ①-5 负向对照 | 故意连一个**自签/过期**证书的本地 `https` 服务 | **必须失败**（`verify failed`），不许"能连上就算过" |

### ② 补齐 SNTP 时间同步

- 用已有的 UDP socket + `getrandom` 出的 48 字节 NTP 包，向 `10.0.2.3`/公共 NTP 请求；
  得到 Unix 时间后写进内核 RTC 偏移（新增一个"时间偏移"设置键，随 store64 持久化）。
- 判据：`[SNTP] sync server=… offset_ms=…` 与宿主 `time.time()` 差 < 2 s；失败时**不改系统时间**并打点。
- 为什么放在 TLS 之后：TLS 4 步可以先跑通"能握手"，时间只影响**证书有效期判定**这一条；
  但发版前必须补上（否则连一个真站点就会因"证书未生效"失败）。

### ③ 逐个编译 NetSurf 依赖库

顺序（依赖少的在前，每步都要有 `.a` + 符号数判据）：

```
libwapcaplet → libparserutils → libhubbub → libcss → libdom
             → libnsgif / libnsbmp / libnsutils / libnspsl / libnslog / libsvgtiny / librosprite
             → expat（libdom 的 XML 用）→ zlib（若启用 gzip 内容编码）
```

- 每个库：`tools/<lib>_build_win.sh`（照 `tools/mbedtls_build_win.sh` 的样子写），
  判据 `[<LIB>] cc ok=N fail=0 archive=<bytes>` + `llvm-nm -g --defined-only libx.a | wc -l`。
- 已知风险：`libcss`/`libdom` 有 perl/python 生成步骤（`make` 里的 `gen_*`）；
  Windows 上没有 perl 时要**手写替身脚本**（照着 .pl 的输入输出改 Python），别指望 msys 的 perl 一定在。
- 判据底线：**库能编 ≠ 能用**；每个库编完立刻写一个 5~20 行的 ring3 小探针调用它的公开 API。

### ④ 用 wl_surface 写现代浏览器 UI 壳子（Edge 风格）

- 位置：`user/browser/`（新目录）。外壳自己实现，**不用 NetSurf 的任何旧 UI**：
  标签页 + 地址栏（输入即搜索，默认 `bing.com`）+ 前进/后退/刷新/主页 + 下载管理（进度/取消/落点）+ 状态栏。
- 画法：复用现有桌面栈 —— `shm_create(13)` + `shm_map(14)` 拿画布 → `wl_surface_create(15)` →
  `attach(16)/damage(17)/commit(18)`；**设计 Token 走 `gui_rs` 的表**（圆角/间距/动效），字用内核光栅化（12 号 ABI）+ 抗锯齿。
- NetSurf 接法：`content/fetchers/vimtu.c`（用我们 socket 层 + mbedTLS 实现 `FETCHER_HTTP/HTTPS`，
  接口只有 `setup/start/abort/finalise/poll`）+ `frontends/vimtu/`（以 NetSurf 自带 `framebuffer` 前端为模板，
  把 `gui_table` 的窗口/绘图/字体/位图/剪贴板实现在 wl_surface 上）。
- 判据：打开 `example.com` 后**屏幕像素**有正文墨迹（不是"进程没崩"）；点链接能触发下载；
  下载完成后宿主侧 sha256 与客人一致。
- **不做**：JS（NetSurf 本身没有）、GPU 合成、多进程渲染。

### 9.4 发版门槛（v0.12.3 或 v0.12.4 什么时候可以发）

必须同时满足：① `version64_test` 8/8；② `rand64_test` 17/17；③ `netping64_test` PASS（网络回归）；
④ **HTTPS 探针**：真站点握手成功 **且** 错证书站点必须失败；⑤ 装机/`install_flow_test` 与
`iso64_test` 不回归；⑥ 若声称"能下大文件"，必须有 **2 MiB/16 MiB 的 sha256 或宿主侧字节数**判据。
缺一条就**不发版**，只提交代码 + 交接单。

---

## 10. 复现命令合集（照抄可用）

```powershell
cd C:\Users\fanqi\Desktop\VimtuOS\Vimtu64

# 上游源码（幂等；已就位会 SKIP）
py -3 tools\fetch_port_sources.py

# 内核（12~14 分钟；两种内核都编）
C:\msys64\usr\bin\bash.exe -lc "bash build64.sh"

# 验收
py -3 tests\version64_test.py        # 8/8
py -3 tests\rand64_test.py           # 17/17（默认 CPU + -cpu max 各起一次 QEMU）
py -3 tests\netping64_test.py        # raw ICMP 回归（PASS）

# mbedTLS 静态库
C:\msys64\usr\bin\bash.exe -lc "bash tools/mbedtls_build_win.sh"   # ok=108 fail=0

# README 截图（9 张，落 docs\images）
py -3 tools\shots_fresh.py
```

---

## 11. 接手须读（顺序）

1. 本单（`docs/handover/HANDOVER_v0123_net_ready.md`）—— 你在哪、下一步做什么。
2. `docs/网络与浏览器生态_v0122攻坚计划.md` —— 为什么这么分期、mbedTLS/NetSurf 的评估数字。
3. `docs/handover/HANDOVER_v0122_net_l4.md` —— 内核 socket 层改了什么、**哪两条判据还 UNVERIFIED**。
4. `docs/内核边界与架构规则.md` —— 什么不许进内核（HTTP/TLS 解析必须在 ring3）。
5. 代码顺序建议：`kernel/rand64.cpp` → `kernel/sock64.cpp` → `tools/mbedtls_build_win.sh` → `tools/fetch_port_sources.py`。

> **纪律（全部来自本仓历史血账）**：结论必须有串口打点/像素判据/宿主侧独立解析三者之一；
> 拿不到就写 `UNVERIFIED`；"部分绿"不许冒充"全绿"；判据与事实不符时**先修脚本但不许放宽强度**。
