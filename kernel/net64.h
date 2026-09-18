// net64.h - 极简以太网 / IPv4 / ARP / ICMP 协议栈（跑在 kernel/e1000_64.cpp 之上）
//
// 静态配置（QEMU 用户模式网络 slirp 的固定值，来源：QEMU 文档 "Network options /
// -netdev user"：guest 建议用 10.0.2.15/24，网关 10.0.2.2，DNS 10.0.2.3；无 DHCP）：
//   本机  10.0.2.15 / 24
//   网关  10.0.2.2
//   DNS   10.0.2.3
// MAC 用驱动从网卡读出来的那个（e1000_mac64()）。
//
// 地址编码约定：uint32_t 按**内存小端**存 4 个字节，即 10.0.2.15 -> 字节序列 [10,0,2,15]。
// 这样把 ip 转成 4 字节直接按小端写内存即可，校验和/包头构造都不用再翻字节序。
#pragma once
#include <stdint.h>

#define NET64_IPV4_64(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define NET64_IP_LOCAL64    NET64_IPV4_64(10, 0, 2, 15)     // QEMU 用户网络 guest 地址
#define NET64_IP_GATEWAY64  NET64_IPV4_64(10, 0, 2, 2)      // QEMU 用户网络网关（= 宿主机 alias）
#define NET64_IP_DNS64      NET64_IPV4_64(10, 0, 2, 3)      // QEMU 用户网络 DNS
#define NET64_IP_MASK64     NET64_IPV4_64(255, 255, 255, 0) // /24

// 初始化：e1000_init64() + 打印本机配置 + 一次性验收流程（ARP 问网关 -> ICMP echo
// seq=1 -> 离线自检），全部有界超时；没网卡/超时只打印 no link 行并优雅返回负值。
// 返回 0 = 链路与探测都成功；-1 = 无网卡/超时（系统继续启动，不失败）。
int net64_init64();

// 收包循环：从 e1000_recv64 取帧，只处理 ARP 与 IPv4/ICMP，其余静默丢弃并计数。
// 非阻塞；一次最多处理若干个帧（有上界）。
void net64_poll64();

// 发一个 ARP 请求（广播）问 ip 的 MAC。返回 0 = 已发出；-1 = 无链路/发送失败。
int net64_arp_request64(uint32_t ip);

// 发 ICMP echo（id 固定）并**有界等待**该 (ip, seq) 的 reply：
// 返回 0 = 收到 reply；-2 = ARP 解析超时；-3 = echo 超时；-1 = 无链路/发送失败。
int net64_ping64(uint32_t ip, uint16_t seq);

// 发一个 IPv4 包（自己算 IP 头校验和；以太网帧不足 60 字节自动补零）：
// 目的 MAC 从 ARP 缓存查；不在就先发 ARP 请求并做有界等待（2 秒）。返回 0 = 已发出；
// -1 = 无链路/参数错误/发送失败；-2 = ARP 解析超时。
int net64_send_ipv4(uint32_t dst_ip, uint8_t proto, const uint8_t* payload, int len);

// 离线自检（不需要网卡）：校验和已知样本、IP 头构造/解析往返、ARP 构造/解析往返、
// 以太网最小帧补零。返回 0 = PASS；>0 = 失败位掩码。串口打 [NET64] selftest PASS/FAIL。
int net64_selftest64();

const char* net64_state_str64();   // "uninitialized" / "no link" / "up"
uint64_t net64_tx_frames64();      // 累计发出的以太网帧（含 ARP/ICMP）
uint64_t net64_rx_frames64();      // 累计从驱动取到的帧（含被丢弃的）
uint64_t net64_arp_replies64();    // 累计收到的 ARP reply
uint64_t net64_icmp_replies64();   // 累计收到的 ICMP echo reply
uint64_t net64_dropped64();        // 累计静默丢弃的帧（非 ARP/非 IPv4-ICMP/解析失败）
