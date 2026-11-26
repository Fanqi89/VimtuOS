# HANDOVER v0.12.4 · TLS-up —— 真 HTTPS 打通（mbedTLS + 内核 socket 层）

- 日期：2026-11-26
- 版本：`0.12.4`（`build64.sh` 已 bump，**未发版**）
- 上游基线：`9e893c99`（= v0.12.3 封存单的 HEAD）
- 用户需求原文：`C:\Users\fanqi\Desktop\网络与浏览器生态.txt`
- 内核映像：**3,597,216 B**（v0.12.3 是 3,592,832 B，**+4,384 B**）；闸门 15,728,640 B

> 本单的用途：让一个**零上下文**的会话读一份文件就能接着干，不重做已经跑通的事、不掉进已经踩过的坑。
> 纪律照旧：结论必须有串口打点 / 像素判据 / 宿主侧独立解析三者之一；拿不到就写 `UNVERIFIED`。

---

## 1. 一句话

**这一版把 HTTPS 真的打通了**：ring3 的 `/bin/httpsprobe` 用 mbedTLS 3.6.2 连 `example.com:443`，
**协商出 TLS 1.3（TLS_AES/CHACHA20-POLY1305）**，用 `/etc/ssl/certs/ca.pem`（118 份 Mozilla 根证书）
**验证了证书链**（`verify_flags=0`），再用 `GET /` 取回 `HTTP/1.1 200 OK`（928 B 正文）；
**同时**对一个宿主上的自签证书站点握手**必须失败**（`ret=-0x2700`，证书校验失败），
宿主侧独立证据显示那条 TCP 连接**真的连上了**并且**在 TLS 握手中途被客人中止**。
验收脚本 `tests/https64_test.py` **22/22 PASS**。

为了让这一切成立，本轮顺带修掉 **5 个真缺陷**（都不是"顺手优化"，而是"不修就通不过"）：

| # | 缺陷 | 后果 | 修法 |
|---|---|---|---|
| 1 | `clock_gettime(CLOCK_REALTIME)` 返回"开机以来的秒数" | 任何真站点证书都被判 `not valid yet` | 新增 `rtc_epoch64()`（CMOS → Unix 秒，Hinnant 算法），REALTIME 加 RTC 锚点 |
| 2 | 用户栈 16 KiB | TLS 握手栈溢出（握手深处 ring3 #PF） | `USER64_STACK_BYTES64` 16 → **64 KiB**；连带修 `phys_pages[16]` 越界与 5 个 ring3 换栈驱动的硬编码栈顶 |
| 3 | `net64_send_ipv4()` 只对**目的 IP** 发 ARP，没有网关概念 | 公网 TCP 全挂（`arp who-has 172.66.147.243` → connect 失败） | 目的不在本网段时，以太网目的 MAC 用**网关**的 |
| 4 | socket 分配时 `laddr = 0`，而 TCP 伪首部要源地址 | TCP 校验和全错 → 网关**静默丢包**（UDP 因为校验和填 0 才没事） | 分配时就填 `NET64_IP_LOCAL64` |
| 5 | `mbedtls_x509_crt_parse()` 的 `buflen` **必须含结尾 NUL** | 每个证书都以 `X509_INVALID_FORMAT` 失败、`certs=0` → CA 形同虚设（握手"成功"但其实没验证） | 传 `can + 1`（主机侧对照实验逐位证明：`200116` → `certs=0`；`200117` → `certs=117`） |

---

## 2. 现场（复现前先核这三条）

```
git log --oneline -3        -> 见 §12
git status --short          -> 空（工作区干净）
ls -l build64/kernel64_os.bin -> 3,597,216 B   (VIMTUOS_VERSION="0.12.4")
Get-Process qemu-system-x86_64 -> 0
```

| 项 | 值 |
|---|---|
| mbedTLS 静态库 | `build64/mbedtls/libmbedtls.a` = **1,527,458 B**（`cc ok=108 fail=0`；比 v0.12.3 的 1,662,810 B 小，因为关掉了服务端与 mbedtls 自带 socket 封装） |
| 探针 ELF | `build64/httpsprobe.elf` = **867,512 B**（5 个 PT_LOAD，span 754,552 B，base `0x100090000`） |
| CA 包 | `build64/ca.pem` = **200,116 B / 118 份**（`[CABUNDLE] ok=1 … sha256=427c064216761a1e`） |
| 系统卷新增 | `/bin/httpsprobe`、`/etc/ssl/certs/ca.pem`（`build64/tlsvol.img`，逐字节回读自检） |
| 上游源码 | `third_party/{mbedtls,netsurf,expat,zlib,ca}/`（**不进仓**，脚本按钉死版本现拉） |

---

## 3. 已完成的四件事（每件都给硬数字 + 判据 + 复现）

### 3.1 ① 时间基准：CLOCK_REALTIME 变成真墙钟

**为什么必须先做**：mbedTLS 判定证书有效期走 `mbedtls_time()` → `time()` → `clock_gettime(CLOCK_REALTIME)`，
而这个系统原来返回的是"开机以来的秒数"（≈1970-01-01）。**每一个真站点都会被判 `not valid yet`** ——
这类失败最容易被误读成"TLS 实现不对"，所以先修基准。

| 环节 | 做法 |
|---|---|
| 读取 | `kernel/x86_64.cpp` 新增 `rtc_epoch64()`：读 CMOS 0x70/0x71，**真读状态寄存器 B**（BCD/24 小时位），12 小时制的 PM 位按 SDM 处理；秒数不翻才接受（防 0x59→0x00 撕裂） |
| 换算 | `days_from_civil64()`（Howard Hinnant，纯整数，无浮点无 libc）→ `days*86400 + h*3600 + m*60 + s` |
| 接线 | `kernel/syscall64.cpp` 的 `lx64_clock_gettime64`：`clockid==0` 时 `sec = 锚点 + (tick-锚点tick)/250`；**CLOCK_MONOTONIC 不动** |
| 打点 | `[TIME64] clock_gettime realtime anchor=rtc epoch=<n> ticks=<n>`；RTC 读不出来时打 `anchor=NONE rtc UNREADABLE -> cert validity UNTRUSTED`（**绝不冒充 1970 去校验**） |

**验收**：`[TIME64] … epoch=1795684625`，与宿主 `time.time()` 差 **73 s**（RTC 是 UTC、1 秒精度；
SNTP 校时是下一轮第一件事）。

### 3.2 ② mbedTLS 移植层：熵回调 + 配置裁剪 + 静态库

| 项 | 内容 |
|---|---|
| 熵回调 | `user/lib/mbedtls_vimtu_port.c`：`mbedtls_hardware_poll()` → `getrandom(318)`（按 256 B 分段，不依赖内核上限）→ `kernel/rand64.cpp` 的 CSPRNG；`len==0` 时 `*olen=0` 且返回 0（上游契约）；整块全 0 一律当失败 |
| 配置 | `user/lib/mbedtls_vimtu_user_config.h`（`MBEDTLS_USER_CONFIG_FILE`，只写差异）：`MBEDTLS_ENTROPY_HARDWARE_ALT` + `MBEDTLS_NO_PLATFORM_ENTROPY`（系统没有 `/dev/urandom`）、关 `MBEDTLS_SSL_SRV_C` / `MBEDTLS_NET_C` |
| 构建 | `tools/mbedtls_build_win.sh`：配置头**单独放一个 include 目录**（直接 `-I user/lib` 会遮住 musl 的 `stdio.h` → 70 个文件全挂 `unknown type name 'FILE'`） |
| 128 位除法 | `user/lib/compiler_rt_shim.c`：`__udivti3/__umodti3/__divti3/__modti3` 自己写（musl 没有 builtins；msys2 的 `libclang_rt.builtins-x86_64.a` 是 **COFF**，`ld.lld` 拒收） |

**验收**：`[TLS] entropy hw-poll selftest=PASS a=… b=… differ=1 len0_ok=1 nonzero=1`、
`[TLS] drbg seeded random=…`（两次调用输出不同，非全 0）。

### 3.3 ③ ring3 HTTPS 探针（`/bin/httpsprobe`）

- **自己写 socket 胶水**（不用 mbedTLS 的 `net_sockets.c`）：`mbedtls_ssl_set_bio()` 的 send/recv 直接调内核
  `send(44)/recv(45)`，`-EAGAIN` → `WANT_READ/WANT_WRITE`，握手/读取各 60 s 有界。
- **判据全在串口**（每步一行，不藏在"成功"两个字里）：
  ```
  [TLS] conf /etc/tlsprobe.conf bytes=88
  [TLS] ===== VimtuOS HTTPS probe start (mbedTLS 3.6.2) =====
  [TLS] entropy hw-poll selftest=PASS … differ=1 len0_ok=1 nonzero=1
  [TIME64] clock_gettime realtime anchor=rtc epoch=1795684625 ticks=…
  [TLS] time now=1795684625 utc=2026-11-26T08:57:05Z src=rtc
  [TLS] drbg seeded random=46f0e788dd27125cf4dd51c82bdb1eec
  [TLS] ca fnv1a=3d3bcba1b49b6d35 first24="##\n## Bundle of CA Root "
  [TLS] ca path=/etc/ssl/certs/ca.pem bytes=200116 parse_ret=0 certs=117
  [TLS] pos dns host=example.com ip=172.66.147.243
  [TLS] pos tcp connected ip=172.66.147.243:443
  [TLS] pos handshake ok version=TLSv1.3 suite=TLS1-3-CHACHA20-POLY1305-SHA256 tx=… rx=…
  [TLS] pos cert subject=CN=example.com
  [TLS] pos cert issuer=C=US,O=…
  [TLS] pos cert valid 2026-11-24T23:48:34 .. 2027-02-22T23:53:08 verify_flags=0x00000000
  [TLS] pos GET / sent=…
  [TLS] pos http status="HTTP/1.1 200 OK" body_bytes=928
  [TLS] RESULT positive=pass
  [TLS] neg dns skipped (literal addr=10.0.2.2)
  [TLS] neg tcp connected ip=10.0.2.2:8443
  [TLS] neg handshake FAILED ret=-0x2700 (X509 - Certificate verification failed…) verify_flags=0x00000800
  [TLS] RESULT negative=pass (ret=-0x2700 …) expected=cert-verify-failed
  [TLS] ===== probe done positive=pass negative=pass =====
  [TLS64] probe done pid-exit rc=0 code=0
  ```
- **怎么启动**（关键设计）：卷上有 `/etc/tls_probe.on` 时内核在 `net64_init64()` 之后**起一个真进程**跑它。
  **不能用** `app64_run_any64()/elf64_run64()` 的同步单例装载器 —— 实测报 `[ELF64] reject reason=busy`
  （合成器 `/lib/wm.elf` 还活着，占着那份单例状态）。走 `proc64_create64 + proc64_start_elf64`
  （与终端 `run`/桌面双击同一条路），等进程 EXITED，看门狗暂停/恢复包住它。
- **配置** `/etc/tlsprobe.conf`（键=值）：`host/port/path/ca/negaddr/negport/negskip`。
  负向目标走 **10.0.2.2**（QEMU slirp 里的宿主），**不过 DNS**。

### 3.4 ④ 交付链：装卷 + 构建钩子 + 门禁

| 环节 | 文件 | 判据 |
|---|---|---|
| 装卷 | `tools/tls_pack_win.py`（新，236 行；与 `net_pack_win.py` 同构） | `TLS 卷自检 OK：2 个文件逐字节回读一致`；夹具模式写 `/etc/tls_probe.on` + conf |
| CA 包 | `tools/fetch_ca_bundle.py`（新） | `[CABUNDLE] ok=1 bytes=200116 certs=118`（两个来源、4 次退避重试、`< 80 份就失败`） |
| 探针构建 | `tools/httpsprobe_build_win.sh`（新） | `[HTTPSPROBE] cc ok=3 fail=0` + `elfcheck ok=1 PT_LOAD=5 span=754552 entry_in_load=1 no_interp=1 no_dynamic=1 base=0x100090000`（**base 必须正好 0x100090000**，否则内核 mode-2 装载的 `base != 0`、静态非 PIE 的绝对地址全错位） |
| 构建挂钩 | `build64.sh` | 编 mbedTLS → 编探针 → `probe64.py` 确认内核里**搜不到**探针字节（`64B 探针 offset … accepted`）→ 抓 CA → 装卷 |
| **栈一致性门禁** | `tools/drv_stack_gate.py`（新） | `[DRVSTACK] ok=1 stack=65536 users=5`；五个换栈驱动（bbdrv/tccdrv/luadrv/makedrv/mpdrv）里写死的栈顶必须等于 `USER64_STACK_BYTES64`，**不一致直接构建失败** |

---

## 4. 验收总表（含未验证项，一条都不许抹）

| 项 | 结果 | 证据 |
|---|---|---|
| 构建（系统内核 + 安装程序内核） | **PASS** | `build64.sh` 走完；内核 3,597,216 B |
| `tests/https64_test.py`（新） | **22/22 PASS** | §3.3 的串口行 + 宿主侧 TCP 连接计数 |
| `version64_test` | **8/8 PASS** | `VimtuOS 0.12.4 x86_64`（ring3 `cat /proc/version`）；旧串 0 次 |
| `rand64_test` | **PASS（17 条）** | 默认 CPU `seed_src=tsc-jitter` / `-cpu max` `rdseed=1` |
| `netping64_test` | **PASS** | `3 packets transmitted, 3 received, 0% packet loss` |
| `iso64_test` | **48/48 PASS** | `-cpu max：SMAP 零违规 hits=0`；**首次跑曾出现 hits=1（未复现，见 §5）** |
| HTTPS 正向（真站点 + 真证书链） | **PASS** | TLS 1.3 / `verify_flags=0` / `HTTP/1.1 200 OK` |
| HTTPS 负向（自签证书必须失败） | **PASS** | `ret=-0x2700`，宿主侧 `TCP 连接数=1（TLS 握手中途被中止=1）` |
| CA 包内容（客人读到的字节与宿主一致） | **PASS** | FNV-1a64 `3d3bcba1b49b6d35`（两边逐位相同） |
| **TLS 1.3** | **PASS（意外收获）** | mbedTLS 3.6.2 默认配置就协商到 TLS 1.3；下一轮把它写进配置显式声明 |
| SNTP 校时 | **未开始** | 现在时间来自 CMOS RTC（与宿主差 ~70 s 量级，QEMU RTC 秒级） |
| NetSurf 编译与前端 | **未开始** | 源码到位，一行没编 |
| 浏览器 UI 壳子 | **未开始** | 依赖 ③ |

---

## 5. 未验证 / 未做（如实记录）

1. **`iso64_test` 的 `-cpu max：SMAP 零违规` 首次跑出现过 hits=1**（`[SMAP64] violation` 1 条）。
   用 `scratch/smap_probe3.py`（同配置：sysdisk 副本 + `-cpu max,la57=off,umip=off` + 登录）复现 **0 条**，
   随后把整个 `iso64_test` 复跑一次得到 **48/48 PASS（hits=0）**。判定：**偶发**，但**没有定位到那条 rip
   之前不许当成已查明**。已做的加固：`iso64_test` 在 FAIL 时会打印**第一条 violation 原文**（pid/cr2/rip），
   下次再出现就有据可查（本轮只报了 `hits=1`，白白多花了一轮排查）。
2. **TLS 1.3 未显式声明**：现在靠上游默认值，属于"能用但没锁死"。下一轮在 user config 里显式打开
   `MBEDTLS_SSL_PROTO_TLS1_3` 并加一条"必须是 1.3 或明确降级到 1.2 且说明原因"的判据。
3. **大文件下载判据（2 MiB sha256 / 16 MiB）** 仍是 v0.12.2 留下的 UNVERIFIED（与 HTTPS 无关）。
4. **证书吊销（CRL/OCSP）没做**：mbedTLS 的 CRL 需要额外数据源，本轮只做链校验。

---

## 6. 关键机制与决策（下一轮必须遵守）

### 6.1 "大映像"装载的两条硬约束（这次踩明白了）

1. **基址必须正好 = `USER64_MMAP_VA64`（`0x100090000`）**。内核的 mode-2 装载算的是
   `base = user64_find_free64(USER64_MMAP_VA64, span) - min_vaddr`；我们的探针是**静态非 PIE**，
   `base != 0` 就等于"所有绝对地址错位"。`tools/httpsprobe_build_win.sh` 的 elfcheck 把这条钉死。
2. **老装载区（4 GiB..4 GiB+64 KiB）只装得下 < 64 KiB 的小程序**。探针 867 KB 必须落大映像区。

### 6.2 内核启动期"跑一个用户程序"只能用真进程路径

`app64_run_any64()`（同步单例装载器）在**已有 ELF 进程活着**时会 `[ELF64] reject reason=busy`。
要用 `panic64_watchdog_pause64() + proc64_create64 + proc64_start_elf64 + 等 EXITED + destroy + unpause`
（`kernel/kernel64.cpp` 的 `[TLS64]` 钩子就是模板）。

### 6.3 用户栈 64 KiB 是**跨层耦合**，改它要同时改五处

`kernel/usermode64.h` 的 `USER64_STACK_BYTES64` 与 `user/{busybox/bbdrv,apps/tcc/tccdrv,lua/luadrv,
make/makedrv,micropython/mpdrv}.c` 里 `old_top = U64_STACK_VA64 + 0x…` 必须一致。
`tools/drv_stack_gate.py` 已进构建（不一致 = 构建失败）。当年这五处写死 16 KiB 的真实后果：
`[BBDRV] exec …` 之后 `[SIG64] fault #PF err=0x6 cr2=0x000000010013BE40`（busybox/tcc/lua/make/mp 全崩）。

### 6.4 mbedTLS 的两个"静默"陷阱

1. `mbedtls_x509_crt_parse(buf, buflen)` 判 PEM 的条件是 `buf[buflen-1]=='\0' && strstr(…)`，
   **`buflen` 要把结尾 NUL 算进去**，否则它拿 PEM 文本当 DER 解，每个证书都 `INVALID_FORMAT`，
   而且**握手照样能成功**（因为这时 authmode 会退成 NONE）——"看起来通了其实没验"，最危险的一种假绿。
2. `MBEDTLS_ENTROPY_HARDWARE_ALT` 必须和 `MBEDTLS_NO_PLATFORM_ENTROPY` 一起开：只开前者，
   默认熵源还会去开 `/dev/urandom`（本系统没有）。

### 6.5 网络栈分层（承 v0.12.2，本轮补两处）

- 内核 socket 层是**唯一**真实应用路径；`net64_send_ipv4()` 现在是**唯一**的下一跳决策点
  （本网段 → ARP 目的；否则 → ARP 网关）。
- 二进制兼容坑：TCP 伪首部用 `s->laddr`，它必须一开始就是本机地址（`bind()` 不再依赖）。
- UDP 校验和填 0（IPv4 允许）——所以 DNS 一直能用，把 TCP 的校验和问题藏到了现在。

---

## 7. 本段改动文件

| 文件 | 性质 |
|---|---|
| `kernel/rand64.cpp/.h` | （v0.12.3 已有，本轮只被调用） |
| `kernel/x86_64.cpp/.h` | **新增** `rtc_epoch64()`（CMOS→Unix 秒；状态寄存器 B 真读） |
| `kernel/syscall64.cpp` | `clock_gettime(CLOCK_REALTIME)` 加 RTC 锚点 + `[TIME64]` 打点 |
| `kernel/usermode64.h` | 用户栈 16 KiB → **64 KiB** |
| `kernel/usermode64.cpp` | **修越界**：`phys_pages[16]` → `8 + STACK_BYTES64/PAGE_SIZE`（+ static_assert） |
| `kernel/net64.cpp` | `net64_send_ipv4()` **默认网关路由**（TCP/UDP 都受用） |
| `kernel/sock64.cpp` | socket 分配时 `laddr = NET64_IP_LOCAL64`（TCP 校验和根因） |
| `kernel/kernel64.cpp` | `[TLS64]` 探针钩子（真进程路径、240 s 有界等待、看门狗 pause/unpause） |
| `user/lib/mbedtls_vimtu_user_config.h` | **新增**：mbedTLS 用户覆盖配置（熵源、只做客户端） |
| `user/lib/mbedtls_vimtu_port.c` | **新增**：`mbedtls_hardware_poll` → `getrandom(318)` + 自检 |
| `user/lib/compiler_rt_shim.c` | **新增**：128 位整数除法/取余（`__udivti3` 等） |
| `user/tls/httpsprobe.c` | **新增**：ring3 HTTPS 探针（BIO + 握手 + 证书信息 + GET + 负向对照） |
| `user/tls/httpsprobe64.ld` | **新增**：大映像链接脚本（基址 0x100090000） |
| `user/{busybox/bbdrv,apps/tcc/tccdrv,lua/luadrv,make/makedrv,micropython/mpdrv}.c` | 换栈驱动的"内核栈顶"16 KiB → 64 KiB |
| `tools/mbedtls_build_win.sh` | 用户覆盖配置（独立 include 目录）|
| `tools/httpsprobe_build_win.sh` | **新增**：编探针 + ELF 硬约束自检 |
| `tools/tls_pack_win.py` | **新增**：装 `/bin/httpsprobe` + `/etc/ssl/certs/ca.pem`；夹具模式写开关/配置 |
| `tools/fetch_ca_bundle.py` | **新增**：抓 CA 包（不进仓） |
| `tools/drv_stack_gate.py` | **新增**：用户栈一致性门禁（构建期） |
| `tests/https64_test.py` | **新增**：HTTPS 验收（22 条判据 + 宿主侧独立证据） |
| `build64.sh` | 版本 0.12.4；栈门禁；mbedTLS/探针/CA/装卷四步 |
| `.gitignore` | 排除 `third_party/ca/` |
| `docs/handover/HANDOVER_v0124_tls.md` | **本单** |

---

## 8. 坑清单（按"会咬人"排序，全部本轮实测）

1. **`[ELF64] reject reason=busy`**：合成器活着时不能用同步单例装载器（§6.2）。
2. **改用户栈 = 五处硬编码要跟着改**（§6.3）；漏改的症状是 `#PF cr2=0x10013BE40`，看起来像 TLS 崩了。
3. **`phys_pages[16]` 越界**：栈从 4 页变 16 页后 8+16=24 > 16，把 `code_pages` 踩成 168468480，第三次
   user64 演示卡死。修法是按常量算容量 + static_assert，不是"把 16 改成 40"。
4. **mbedTLS PEM 的 `buflen` 必须含 NUL**（§6.4.1）——最危险，因为它同时让"握手成功"变成假绿。
5. **`-I user/lib` 会遮住 musl 的 `stdio.h`**：mbedTLS 108 个文件里 70 个报 `unknown type name 'FILE'`。
   配置头要单独放一个目录。
6. **`ld.lld` 不收 COFF builtins**：msys2 的 `libclang_rt.builtins-x86_64.a` 是 Windows 目标，
   只能自己写 `__udivti3`（§3.2）。
7. **日志自己会骗人**：`hexdup()` 只有一个 static 缓冲，`say("a=%s b=%s", hexdup(a), hexdup(b))` 打印
   两份相同的值，看着像"熵坏了"。改两个缓冲。
8. **QEMU `sendkey` 在 1 fps GUI 下丢字/吞长命令**：本轮所有验收**一个键盘事件都不发**，
   全部走"夹具卷开关 + 读串口"。

---

## 9. 下一轮严格路线图（按依赖顺序）

### ① SNTP 校时（半天）

- 用现有 UDP socket 发 48 字节 NTP 请求到 `10.0.2.3`（slirp 里有 NTP？）或 `pool.ntp.org`；
- 得到 Unix 秒后写进内核"时间偏移"（新增设置键，随 store64 持久化），`[TIME64]` 的锚点加上它；
- 判据：`[SNTP] sync server=… offset_ms=…`，与宿主 `time.time()` 差 **< 2 s**；失败时**不改时间**并打点。

### ② 逐个编译 NetSurf 依赖（1~2 天）

顺序：`libwapcaplet → libparserutils → libhubbub → libcss → libdom →`
`libnsgif/libnsbmp/libnsutils/libnspsl/libnslog/libsvgtiny/librosprite → expat → zlib`。
每库一个 `tools/<lib>_build_win.sh`（照 mbedTLS 那份写）+ `[<LIB>] cc ok=N fail=0 archive=<bytes>`
+ `llvm-nm -g --defined-only` 符号数 + 一个 5~20 行 ring3 探针。
**风险**：libcss/libdom 的 `make` 里有 perl/python 代码生成步骤 → 手写 Python 替身。

### ③ 浏览器 UI 壳子（`user/browser/`）

- 标签页 + 地址栏（默认 bing.com）+ 前进/后退/刷新/主页 + 下载管理 + 状态栏；
- `shm_create(13)`/`shm_map(14)` 拿画布 → `wl_surface_create(15)` → `attach/damage/commit`；
- 设计 Token 走 `gui_rs`；字用内核光栅化（12 号 ABI）。
- NetSurf 接法：`content/fetchers/vimtu.c`（基于本轮的内核 socket + mbedTLS）+ `frontends/vimtu/`
  （以 `framebuffer` 前端为模板）。
- 判据：打开 `example.com` 后**屏幕像素**有正文墨迹；点链接能触发下载；下载完宿主侧 sha256 一致。

### ④ 发版门槛（v0.12.4 什么时候可以发）

必须同时满足：① `version64_test` 8/8；② `rand64_test` 17/17；③ `netping64_test` PASS；
④ **`https64_test` 22/22**（含负向对照）；⑤ `iso64_test` 全绿（含 `-cpu max` SMAP 零违规，
且 FAIL 时必须能指出是哪一条 violation）；⑥ 装机/`install_flow_test` 不回归。

---

## 10. 复现命令合集（照抄可用）

```powershell
cd C:\Users\fanqi\Desktop\VimtuOS\Vimtu64

# 上游源码（幂等；已就位会 SKIP）
py -3 tools/fetch_port_sources.py

# 内核 + mbedTLS + 探针 + CA + 装卷（12~22 分钟）
C:\msys64\usr\bin\bash.exe -lc "bash build64.sh"

# ★ HTTPS 验收（自己起自签 https 服务 + 造夹具盘 + 跑 QEMU；3~5 分钟）
py -3 tests/https64_test.py

# 其它验收
py -3 tests/version64_test.py        # 8/8
py -3 tests/rand64_test.py           # 17 条
py -3 tests/netping64_test.py        # raw ICMP 回归
py -3 tests/iso64_test.py            # 安装/隔离/SMAP（长）

# 单独重编探针（11 秒~1 分钟，不动内核）
C:\msys64\usr\bin\bash.exe -lc "bash tools/httpsprobe_build_win.sh build64"
py -3 tools/tls_pack_win.py --vol-in build64/viewervol.img --vol-out build64/tlsvol.img `
      --probe build64/httpsprobe.elf --ca build64/ca.pem

# 用户栈一致性门禁（构建期也会自己跑）
py -3 tools/drv_stack_gate.py
```

---

## 11. 接手须读（顺序）

1. 本单 —— 你在哪、下一步做什么。
2. `docs/handover/HANDOVER_v0123_net_ready.md` —— 上一段的封存状态与四步路线图。
3. `docs/网络与浏览器生态_v0122攻坚计划.md` —— 为什么这么分期、mbedTLS/NetSurf 的评估数字。
4. `docs/内核边界与架构规则.md` —— **HTTP/TLS 解析必须在 ring3**（本轮守住了：内核只做 socket/时间/网关）。
5. 代码顺序：`kernel/net64.cpp`（下一跳）→ `kernel/sock64.cpp`（laddr/伪首部）→
   `kernel/syscall64.cpp`（clock_gettime）→ `user/tls/httpsprobe.c` → `tools/httpsprobe_build_win.sh`。
6. 想看"为什么"，`tools/drv_stack_gate.py` 与 `user/tls/httpsprobe.c` 的文件头注释里有本轮最贵的两个教训。

---

## 12. 本单提交（写入时回填）

- 提交信息见 §12 所在 commit；`HANDOVER.md` 索引顶部已加一行。
