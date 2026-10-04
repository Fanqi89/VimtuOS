// netraw64.h - ★ P9：**原始帧收发** ABI 的内核落点（自有 int 0x80 号 53 `net_raw`）
//
// 一句话：把 e1000 驱动**已经有的**"发一帧 / 收一帧"原样交给 ring3，内核不碰帧里的任何字节。
//
// ============================ 为什么这个 ABI 是"最小补齐"而不是"新功能" ============================
// 架构宪法（docs/内核边界与架构规则.md）要求内核只保留"用户态所的 ABI"；加一个号之前必须逐条对上：
// 架构宪法（docs/内核边界与架构规则.md）要求内核只保留"用户态所必需的 ABI"；加一个号之前必须逐条对上：
//        kernel/e1000_64.cpp 有整套 TX/RX 描述符环（e1000_send64 / e1000_recv64 / e1000_mac64 /
//        e1000_link64），kernel/net64.cpp 已经在用它们做 ARP/ICMP。**本模块没有碰硬件寄存器**，
//        也没有给 e1000 加接口 —— 只用它既有的四个函数（e1000_64.h 一行不改）。
//   ② ring3 **无等价通路**：
//        用户态能拿到的最"底层"的通路是 Linux 号段的 socket(41)/socketpair(55)，全都没有实现
//        （tests/busybox64_test.py 的 GAP_SYSCALLS 里如实列着）；pci_map_bar(48) 只映射 MMIO，
//        而 TX/RX **环**要描述符数组 + 物理连续页 + 与驱动共享的环指针索引，用户态自己再点一遍
//        硬件必然和内核驱动抢同一组寄存器（且 DMA 地址要过内核页池）。所以"自己能收发帧"这件事
//        在 ring3 里**没有**替代路径。
//   ③ 只加"校验 + 转发"，**没有任何策略**：
//        本模块 = 校验（op / 长度 / 缓冲区范围 / 网卡在不在）-> 转给 e1000 的 send/recv。
//        **不做** ARP 应答、不做 IP 解析、不做过滤/路由/NAT/会话表、不缓存任何帧、不做协议识别。
//        进来的是"一整帧以太网帧"，出去也是；帧里是什么（ARP/IPv4/DHCP/DNS/TCP…）内核**看不懂**
//        也不需要看懂 —— 它的字节由 ring3 决定。
//   ④ 策略全在用户态：
//        ARP 缓存/超时、IP 地址与掩码、DHCP、DNS、UDP 端口表、TCP 状态机/重传/窗口、打印格式，
//        全部在 user/net/netd.c（交付 = 系统卷里的 /bin/netd）。内核里搜不到它们的字节。
//
// ============================ 调用契约（与 kernel/syscall64.h 的 "net_raw（53）" 段逐字一致）============
//   int 0x80：rax=53，rdi=op，rsi=arg1，rdx=arg2
//     op=0 tx(frame_va, len)  发**一整帧**（不含 FCS；14 <= len <= 1514）-> 0 = 已交给硬件
//     op=1 rx(buf_va, cap)    取**一帧**（非阻塞；14 <= cap <= 2048）-> >0 = 拷贝到用户缓冲的字节数
//     op=2 mac(out_va)        写 6 字节 MAC 到用户缓冲            -> 0
//     op=3 link()             链路查询（不给缓冲）                 -> 1 = up / 0 = down
//   错误码（自有 ABI 的**小负数码**，与 48/49 同款口径）：
//     -1 EPERM    保留（本号不需要进程上下文；与其它自有号同一张表，文档里如实标注"未使用"）
//     -2 EFAULT   用户缓冲不在用户窗口内/未映射为用户页（越界）
//     -3 EINVAL   op 未知 / 长度结构性非法（tx len < 14；rx cap < 14）
//     -4 EAGAIN   rx 暂时没有帧（**非阻塞**，不是错误）/ tx 描述符有界等待超时（可重试）
//     -5 ENODEV   没有可用网卡（没插 e1000 / 驱动没起来）
//     -6 EMSGSIZE 长度超硬件口径（tx len > 1514；rx cap > 2048）
//
// ============================ 打点（自动验收 grep；格式勿改）============================
//   [NETRAW] tx bytes=<n> ok=<0|1> err=<signed> frames=<n>        （上限 128 行）
//   [NETRAW] rx bytes=<n> frames=<n>                              （只在**真收到帧**时打；上限 128 行）
//   [NETRAW] mac=<aa:bb:cc:dd:ee:ff> link=<up|down>               （mac/link 查询；上限 16 行）
//   [NETRAW] deny op=<n> reason=<bad-op|short|oversize|bad-buf|no-nic> err=<signed>   （上限 48 行）
//   [NETRAW] log cap reached tx=<n> rx=<n>                        （每类到上限时只打一次，防刷屏）
//   上限为什么是 128/128：ring3 的完整栈（/bin/netd 一次运行）收发约 50 帧，原来 32 行会把后面的帧
//   全部截掉 —— 验收就再也拿不到"DNS/UDP/TCP 的帧真的从这一号进出了"的打点。上限仍然有（防刷屏）。
//   为什么不打 EAGAIN：ring3 的收包是**轮询**（每 10ms 一次），没有帧是最常见的状态，
//   逐次打点会把串口刷满 —— 所以 rx 只在 bytes>0 时打，计数仍在（netraw64_stats64）。
//
// ============================ 如实边界 ============================
//   * 只有**单帧**语义：不做分片/重组/环形批量收（一帧一次系统调用；e1000 的 RX 环仍是 32 项，
//     由驱动里的 g_rx_cur 轮转，内核不给用户态暴露环本身）；
//   * 不阻塞：rx 无帧立刻 -EAGAIN（用户态自己 sleep 轮询 —— 策略在 ring3）；
//   * 不做多播过滤/VLAN/混杂开关的**策略**：RCTL 的 UPE|MPE|BAM 是 e1000 初始化时既有的设置
//     （见 kernel/e1000_64.cpp 的步骤 9），本模块**一个寄存器都不改**；
//   * 与 kernel/net64.cpp 的共存：两者都从 e1000 的**同一个** RX 环取帧（见 .cpp 末尾的说明与
//     docs/应用层与系统调用说明.md 的"共存与风险"段）—— net64 只在启动期那一次探测里 poll。
#pragma once
#include <stdint.h>

// ---- 长度口径（唯一来源；syscall64.cpp 的"长度闸门先于范围校验"用同一组常量）----
#define NETRAW64_OP_TX   0u
#define NETRAW64_OP_RX   1u
#define NETRAW64_OP_MAC  2u
#define NETRAW64_OP_LINK 3u
#define NETRAW64_OP_MAX  NETRAW64_OP_LINK

#define NETRAW64_TX_MIN   14u        // 以太网头（14B）——比这个短的"帧"没有意义
#define NETRAW64_MTU      1514u      // 不含 FCS 的最大帧（e1000_send64 的既有上限）
#define NETRAW64_RX_MIN   14u
#define NETRAW64_RX_MAX   2048u      // e1000 RX 单缓冲（E1000_RX_BUF_SZ）

// ---- 错误码（负数；见上面契约段）----
#define NETRAW64_EPERM    (-1)
#define NETRAW64_EFAULT   (-2)
#define NETRAW64_EINVAL   (-3)
#define NETRAW64_EAGAIN   (-4)
#define NETRAW64_ENODEV   (-5)
#define NETRAW64_EMSGSIZE (-6)

// ============================ 内核侧实现（kernel/netraw64.cpp）============================
// ★ 只链进**系统内核**（build64.sh 的 SRCS_OS）。安装介质内核不链它，kernel/syscall64.cpp
//   对这四个函数用**弱引用**（符号为 0 -> 号 53 返回 -1 并打 [SYSCALL] deny，绝不假装成功）。
//
// tx：frame_va 是**用户态**地址（调用点已过 user64_range_ok64），本函数再确认一次范围，
//     长度闸门按 NETRAW64_TX_MIN/MTU；成功 = 0（已交给硬件），失败 = 负错误码。
int64_t netraw64_tx64(uint64_t frame_va, uint64_t len);
// rx：buf_va 是用户态地址，cap 见 NETRAW64_RX_MIN/MAX；>0 = 已拷贝字节数，-EAGAIN = 暂时没有帧。
int64_t netraw64_rx64(uint64_t buf_va, uint64_t cap);
// mac：把 6 字节 MAC 写进用户缓冲（mac_va 需 6 字节可写）。返回 0 / -ENODEV。
int64_t netraw64_mac64(uint64_t mac_va);
// link：1 = up / 0 = down / -ENODEV = 没有网卡。
int64_t netraw64_link64();

// 统计（给内核自检/诊断用；不属于 ABI）：
uint64_t netraw64_tx_frames64();     // 累计成功交给硬件的帧
uint64_t netraw64_rx_frames64();     // 累计取回用户态的帧
uint64_t netraw64_tx_bytes64();      // 累计 TX 字节
uint64_t netraw64_rx_bytes64();      // 累计 RX 字节
uint64_t netraw64_denied64();        // 累计被校验拦下的调用
