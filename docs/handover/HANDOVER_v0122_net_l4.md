> [← 返回主页](../README.md) · VimtuOS 文档体系

# HANDOVER v0.12.2 —— 网络与浏览器生态 · P0「L4 硬化 + DNS 打通」

- 日期：2026-11-25
- 版本：`0.12.2`（`build64.sh` 已 bump；**未发版** —— 见 §7 为什么）
- 基线：`c8979981`（v0.12.1 发版回读之后那次 README 重写）
- 内核映像：**3,584,096 B**（v0.12.1 基线 3,575,808 B，**+8,288 B**）；闸门 15,728,640 B
- 用户需求原文：`C:\Users\fanqi\Desktop\网络与浏览器生态.txt`（两大部分：① OSI L4 + 网络访问 ② 移植开源浏览器 + 打通底层）
- 阶段计划：**[`docs/网络与浏览器生态_v0122攻坚计划.md`](网络与浏览器生态_v0122攻坚计划.md)**

---

## 1. 一句话

本版把**内核 socket 层**（真实应用走的那条栈，`kernel/net64.cpp` + `kernel/sock64.cpp`）从
"局域网级、只在逻辑自检里绿"提升到"字段按线上字节正确、能收 DNS、按序号收 TCP"，
并**第一次让域名解析真的成功**（串口证据 `PING example.com (172.66.147.243)`）。
浏览器移植（NetSurf）与 TLS（mbedTLS）本版**只做了评估与排期**，没有动手 —— 见计划文档 §6/§7。

---

## 2. 盘点：仓库里有两条独立网络栈（这是理解一切的前提）

| | **A. 内核 socket 层** | **B. Ring 3 `netd`** |
|---|---|---|
| 代码 | `kernel/net64.cpp`（ARP/ICMP/IPv4）+ `kernel/sock64.cpp`（TCP/UDP/raw ICMP）+ `kernel/e1000_64.cpp` | `user/net/netd.c`（1,700 行自带栈，只用内核 53 号收发原始帧） |
| 使用者 | busybox（`ping`/`wget`/`nslookup`）、musl 的 `getaddrinfo`、任何 Linux ABI 程序 | 只有 `/bin/netd` 自己 |
| 本版是否动 | **是**（全部改动都在这里） | 否 |

浏览器是 ring3 程序 → 只能走 A。所以 P0 只打 A。

---

## 3. `ping baidu.com` 报错：三个代码根因（全部已修）+ 一个环境限制

| # | 根因 | 证据（修前） | 修法 |
|---|---|---|---|
| ① | 卷上没有 `/etc/resolv.conf`。musl 的 `getaddrinfo` **不内置**默认 nameserver | 全仓 `grep resolv.conf` = 0 命中 | `tools/store_pack_win.py` 构建期预置（0666，`nameserver 10.0.2.3`）；构建日志 `[ok] /etc/resolv.conf = 325 B` |
| ② | `poll()` 对 socket handle 一律回 `POLLNVAL`（handle=200+idx 不在 fd 表）→ musl 等不到 DNS 回包 | 旧 `lx64_poll_core64` 只调 `fd64_ready64` | 新增 `sock64_ready64()`；poll 核心按 socket 类型分派；等 socket 时自己 `net64_poll64()` 泵收包 |
| ③ | **UDP 根本没有 UDP 头**：`sock64_send64` 把 payload 直接交给 `net64_send_ipv4`（它只补 Ethernet+IPv4） | `sendto ret=0x1D` 之后 `poll` 立刻超时（对端看到的"目的端口"是 DNS 事务 ID） | 新增 `s64_build_udp()` 补 8 字节头 |
| ④ | **端口/窗口线上字节序写反**：`wr16(htons(port))` 双反 + 收包侧再反回来 → 内部自洽、自检全绿，但 53 变成 `0x3500` | —— | 本层端口统一主机序，`wr16/rd16` 直接大端读写；删 `s64_htons`；自检新增"`rport=53` 时线上必须是 `00 35`"判据 |
| ⑤ | **`recvmsg(47)` 未实现**（musl 的解析器用它取回包 + 拿源地址） | `[SYSCALL] enosys nr=47`，紧跟在 `poll ret=1` 之后 | 新增 `lx64_recvmsg64` / `lx64_sendmsg64`（单 iov，写死字节偏移） |

**环境限制（不是缺陷，不许当成"已修好"）**：QEMU 用户网络（slirp）**不转发公网 ICMP**，
所以 `ping baidu.com` 在 slirp 下永远不会"变绿"；能证明的是**域名解析成功**（拿到 A 记录）。
真机/桥接下才会真的回 echo。本版验收因此拆成两条判据，不混为一谈。

---

## 4. TCP 大文件：6 个真缺陷（全部已修）

| # | 缺陷 | 真实后果 | 修法 |
|---|---|---|---|
| ① | 收数据段**不校验序号**，直接追加并把 `rcv_nxt` 推到 `seq+dlen` | 对端重传一次 → 同一段被追加两遍 → **下载文件损坏** | 只有 `seq==rcv_nxt` 入缓冲；`<` 算重复（只回 ACK，计数）；`>` 算乱序（丢弃 + dup ACK） |
| ② | 窗口写死 4096；缓冲满仍推进 `rcv_nxt` | 丢的数据**永不重传** | 报真实剩余空间（缓冲 4 KiB→**64 KiB**），满则报 0 且不推进 |
| ③ | 不验 TCP 校验和 | 坏段被当数据 | 伪首部+段算和，非 0 丢弃计数 |
| ④ | 数据段发出去不管，没有重传 | 丢一段 → 永久卡住 | 单段在途 + RTO 500 ms 指数退避到 4 s，ACK 才清 |
| ⑤ | SYN 不带选项（对端按 MSS 536 发） | 吞吐约 1/2.7 | SYN 带 `MSS=1460` |
| ⑥ | `recv` 超时返回 0（与 EOF 不可分） | 慢下载停 3 秒 = 判定"文件结束" → **截断文件** | TCP/UDP 超时返回 `-EAGAIN`；只有 FIN/RST 才回 0；TCP 读超时 20 s |

配套：`recv` 取走数据后立即重发 ACK 通告新窗口（否则对端按旧窗口停住）；新增判据计数器
`sock64_retx64 / rx_dup64 / rx_ooo64 / rx_badsum64 / rx_winzero64`。

**顺带修掉 SMAP 缺口**：`sendto/recvfrom/sendmsg/recvmsg` 里 socket 层直接读写用户缓冲，
原来**没有 `STAC/CLAC`** —— `-cpu max`（SMAP 开）下这些路径会 #PF。现在四处都成对开窗。

---

## 5. 改动文件

| 文件 | 改动 |
|---|---|
| `kernel/sock64.h` | 缓冲 64 KiB；`sock64_ready64()`；判据计数器；`SOCK64_MSS/RTO` |
| `kernel/sock64.cpp` | 按序号收 + 真窗口 + 校验和 + 单段重传 + MSS + `-EAGAIN`；`s64_build_udp()`；去掉字节序双反；自检新增窗口/就绪/端口字节序判据（`[SOCK64] selftest` 文案不变，仍 PASS） |
| `kernel/syscall64.cpp` | poll 核心识别 socket + 泵收包；`recvmsg(47)`/`sendmsg(46)`；`-EAGAIN` 原样透出；四处 `STAC/CLAC`；`#include "net64.h"`（安装程序内核不编） |
| `tools/store_pack_win.py` | 预置 `/etc/resolv.conf`（0666） |
| `build64.sh` | `VIMTUOS_VERSION="0.12.2"` |
| `tests/netl7_64_test.py` | **新增**：DNS + 2 MiB（客人 sha256 对宿主）+ 16 MiB（宿主侧字节数）验收 |
| `docs/网络与浏览器生态_v0122攻坚计划.md` | **新增**：盘点 / 根因 / 分期 / mbedTLS 与 NetSurf 评估 |

`git diff --stat`（本版）：见 §9 现场。

---

## 6. 验收结果（逐条，含"没通过的"）

| 项 | 结果 | 证据 |
|---|---|---|
| 构建 | **PASS** | `build64.sh` EXIT=0；内核 3,584,096 B（+8,288 B） |
| 版本门禁 `version64_test` | **PASS 8/8** | `VimtuOS 0.12.2 x86_64`；旧串 `0.12.1` 在内核里 **0 次** |
| `netping64_test`（raw ICMP 回归） | **PASS** | `3 packets transmitted, 3 packets received`、`0% packet loss`、`[SOCK64] create … type=3 proto=1` |
| **域名解析** | **PASS（有串口证据）** | `PING example.com (172.66.147.243)`；`[SYSCALL] insn nr=47 … ret=0x3D/0x55`（recvmsg 拿到 61/85 字节 DNS 回包）；`[FD64] open path=/etc/resolv.conf` |
| **2 MiB 下载 sha256 一致** | **UNVERIFIED** | `tests/netl7_64_test.py` 的 `wget` 步骤没能真正执行（见下） |
| **16 MiB 下载** | **UNVERIFIED** | 同上 |

### 为什么两条下载没验成（必须写清楚，不许冒充绿）

`tests/netl7_64_test.py` 用 QEMU monitor `sendkey` 往 Ring 3 shell 里打命令。实测**长命令会被吞**：
`run /bin/busybox wget -O /tmp/d.bin http://10.0.2.2:5899/b2.bin`（62 字符）在 6 次运行里
有 4 次只到达 `un /bin/busybox wget` 或 `wget`（无 URL），宿主 HTTP 服务端因此一次请求都没收到。
同一个 harness 打**短命令**（36 字符的 `run /bin/busybox ping -c 3 10.0.2.2`）是稳定的 ——
`netping64_test` 连跑通过。所以这是**测试输入通道的限制**（PS/2 环 + 1 fps 的 GUI 帧节拍下按键会丢），
不是 TCP 代码的结论。

> 下一轮先解决"命令怎么可靠送进 ring3 shell"（候选：给 shell 加串口输入通道 / 用 fw_cfg 传命令 /
> 把长命令写成卷上的脚本文件只敲短命令），再验 2 MiB 与 16 MiB。**在此之前不许说"大文件下载已通"。**

---

## 7. 为什么本版没发版

发版门禁要求"验收全绿 + 旧版附件逐条回读"。本次核心能力（域名解析）已绿，但
**用户明确要求的两条大文件判据仍未验证**（§6）。按"部分绿不许冒充全绿"的纪律，本版
**只提交代码与文档，不打 tag、不建 Release**。下一轮把输入通道解决、两条下载判据跑绿后再发。

---

## 8. 下一轮（按依赖顺序）

1. **命令输入通道**（阻塞项）：让验收脚本能可靠地让 ring3 shell 执行任意长命令。
2. **2 MiB / 16 MiB 下载判据**：客人 `sha256sum` 对宿主 sha256；16 MiB 用宿主侧字节数 + 吞吐。
3. `/bin/wget`（自有 HTTP/1.1 GET：Range、302、Content-Length、Connection: close）——
   既是 P1 的产品件，也是 NetSurf fetcher 的原型。
4. **密码学熵**：`getrandom(318)` 目前自述"非密码学安全"；TLS 前必须换成 RDRAND/RDSEED + CSPRNG。
5. mbedTLS 移植（计划 §6）；NetSurf 依赖库与前端（计划 §7）。

---

## 9. 现场

```
HEAD（本版提交）   = 见 git log 顶部（提交信息以 "feat(net,l4)" 开头）
基线               = c8979981
内核映像           = 3,584,096 B（闸门 15,728,640 B）
iso / img          = 93,458,432 B / 42,684,416 B（构建产物，未发版）
两种构建           = kernel64.bin（安装程序，不链 socket/net64）与 kernel64_os.bin 都 EXIT=0
串口证据（DNS）    = PING example.com (172.66.147.243) / nr=47 ret=0x3D,0x55 / resolv.conf 325 B
```

### 复现命令

```powershell
bash build64.sh                       # 12~14 分钟
py -3 tests\version64_test.py         # 8/8
py -3 tests\netping64_test.py         # raw ICMP 回归（PASS）
py -3 tests\netl7_64_test.py          # DNS + 下载（下载两条当前 UNVERIFIED，见 §6）
```
