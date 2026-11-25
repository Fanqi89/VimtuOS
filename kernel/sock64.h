// kernel/sock64.h - Linux socket 层（AF_INET：TCP 客户端 / UDP / raw ICMP）
//
// ★ v0.12.2（网络与浏览器生态 · P0）：把这一层从「局域网级可用」升级到「能扛住多兆下载」。
//   本版改动（都有判据，见 tests/netsock64_test.py）：
//     ① 接收侧**按序号收**（seq == rcv_nxt 才入缓冲；重复段只回 ACK 不入缓冲；
//        乱序段丢弃并立即回一个 dup ACK 让对端重传）—— 修掉「重复段被追加 -> 下载文件损坏」；
//     ② **真窗口**：每次 ACK 都报出当前剩余空间（rx 缓冲 64 KiB），收满就报 0，
//        修掉「缓冲满了还报 4096、数据被静默丢掉却把 rcv_nxt 推上去」；
//     ③ **校验和校验**（TCP 伪首部，收到校验和不为 0 视为坏段丢弃并计数）；
//     ④ **数据段重传**：单段在途 + RTO（500 ms 起、指数退避到 4 s），收到 ACK 才清；
//     ⑤ SYN 带 **MSS=1460** 选项（原来不带选项，对端只能按 536 发，吞吐差 2.7 倍）；
//     ⑥ `recv` 超时语义修正：TCP/UDP 返回 **-EAGAIN**，只有收到 FIN/RST 才返回 0（EOF）——
//        修掉「慢速下载停 3 秒就被当成文件结束 -> 下到一半的截断文件」。
//     ⑦ **poll/select/epoll 现在看得见 socket**（`sock64_ready64`），这是 musl 的
//        getaddrinfo（`poll()` 等 DNS 回包）能不能工作的前提。
//
// 仍然没有的（如实）：listen/accept（服务端）、IPv6、IP 分片/重组、TCP 选项除 MSS 外的全部
//   （SACK/时间戳/窗口缩放）、拥塞控制（固定 RTO，无 cwnd/RTT 估计）、乱序缓存（丢乱序不缓存）。
#ifndef VIMTU_SOCK64_H
#define VIMTU_SOCK64_H
#include <stdint.h>

// 为什么只做"客户端"：这一层的目标是让真实 Linux 程序（wget/curl/DNS 解析）能连出去。
// listen/accept（服务端）需要 socket→进程队列的完整语义，单独一轮再做。
// 依赖：kernel/net64.h 的 send_ipv4（含 ARP），上行帧由 net64 的收包路径回调 sock64_rx64。

#define SOCK64_MAX        8
#define SOCK64_RXBUF      65536        // ★ v0.12.2：4 KiB -> 64 KiB（下载吞吐 = 窗口/带宽时延积）
#define SOCK64_MSS        1460         // 以太网 MTU 1500 - 20(IP) - 20(TCP)
#define SOCK64_RTO_MS     500          // 初始 RTO（slirp/局域网量级；连续超时指数退避到 4 s）

#define SOCK64_T_TCP      1
#define SOCK64_T_UDP      2
#define SOCK64_T_ICMP     3          // ★ v0.9.39：raw ICMP（AF_INET, SOCK_RAW, IPPROTO_ICMP）—— busybox `ping` 要的就是它

#define SOCK64_FD_BASE    200          // handle = 200 + idx（与 epoll 的 1000+ 分开）

// 上行分发：net64 收到 IPv4（proto 6/17）时调用
void sock64_rx64(uint8_t proto, uint32_t src_ip, const uint8_t* payload, int len);

int  sock64_create64(int type);                                   // 返回 handle（>=200）或 -1
int  sock64_connect64(int h, uint32_t ip, uint16_t port, uint32_t timeout_ms);
int  sock64_send64(int h, const void* buf, int len);
void sock64_sendto_target64(int h, uint32_t ip, uint16_t port);   // UDP：记下目标地址
int  sock64_recv64(int h, void* buf, int max, uint32_t timeout_ms);
int  sock64_close64(int h);
int  sock64_getname64(int h, uint32_t* ip, uint16_t* port, int peer);
uint64_t sock64_tx_segments64();
uint64_t sock64_rx_segments64();
// ★ v0.12.2：poll/select/epoll 用。位掩码与 fd64_ready64 同口径：
//   1 = IN（有数据 / EOF / 已 RST）、2 = OUT（可发）、4 = HUP；-1 = 不是本层 socket。
int  sock64_ready64(int h);

// ★ v0.9.39：bind(49)。raw ICMP 必须先 bind 成功才算"能 ping"（busybox 的 xbind 失败即退出）。
//   语义：ICMP/UDP 记录本地地址（本内核只有一个本地 IP，端口对 ICMP 无意义）；TCP 仍不支持。
int  sock64_bind64(int h, uint32_t ip, uint16_t port);
// ★ v0.9.39：handle 是不是本层的 socket（给 syscall 的 bind/close 分支做类型判别）
int  sock64_is_socket64(int h);
int  sock64_selftest64(void);                                     // 纯逻辑自检（不发包）

// ★ v0.12.2：加固判据计数器（验收脚本按它们断言"真的按序号收了/真的重传了"）
uint64_t sock64_retx64(void);        // 数据段重传次数
uint64_t sock64_rx_dup64(void);      // 收到「已收过」的重复段（只回 ACK、不入缓冲）
uint64_t sock64_rx_ooo64(void);      // 收到乱序段（洞前面缺数据 -> 丢弃 + dup ACK）
uint64_t sock64_rx_badsum64(void);   // TCP 校验和不对（丢弃）
uint64_t sock64_rx_winzero64(void);  // 收到数据时缓冲已满（丢弃 + 窗口 0，等对端重传）
#endif
