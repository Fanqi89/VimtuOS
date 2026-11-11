// kernel/sock64.h - ★ v0.7.4（P2 第三批）：Linux socket 层（AF_INET，UDP + 最小 TCP 客户端）
#ifndef VIMTU_SOCK64_H
#define VIMTU_SOCK64_H
#include <stdint.h>

// 为什么只做"客户端"：这一层的目标是让真实 Linux 程序（apk 下载包、curl）能连出去。
// listen/accept（服务端）需要 socket→进程队列的完整语义，单独一轮再做。
// 依赖：kernel/net64.h 的 send_ipv4（含 ARP），上行帧由 net64 的收包路径回调 sock64_rx64。

#define SOCK64_MAX        8
#define SOCK64_RXBUF      4096

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
// ★ v0.9.39：bind(49)。raw ICMP 必须先 bind 成功才算"能 ping"（busybox 的 xbind 失败即退出）。
//   语义：ICMP/UDP 记录本地地址（本内核只有一个本地 IP，端口对 ICMP 无意义）；TCP 仍不支持。
int  sock64_bind64(int h, uint32_t ip, uint16_t port);
// ★ v0.9.39：handle 是不是本层的 socket（给 syscall 的 bind/close 分支做类型判别）
int  sock64_is_socket64(int h);
int  sock64_selftest64(void);                                     // 纯逻辑自检（不发包）
#endif
