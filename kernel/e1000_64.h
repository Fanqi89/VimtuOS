// e1000_64.h - Intel 82540EM (e1000) 网卡驱动对外接口（纯轮询收发，不要求中断）
//
// 设计边界：
//   * 只做"能收能发"的最小集合，供 kernel/net64.cpp 的 ARP/ICMP 用；
//   * 没网卡/初始化失败一律返回负值，调用方优雅降级（绝不 panic/死等）；
//   * 不开中断、不 unmask PIC：收发全靠轮询，不碰中断路径。
#pragma once
#include <stdint.h>

// 初始化：PCI 扫描（0xCF8/0xCFC）找 e1000 -> 打开 MEM/BUS MASTER -> BAR0 直接当指针
// -> 软复位 + 清 MTA/流控 + 读 MAC + 建 TX/RX 描述符环 + 开链路。
// 返回 0 = 可用；-1 = 没找到网卡（已打 [E1000] not found）；其它负值 = 找到了但不可用。
int e1000_init64();

// 发一帧以太网帧（不含 FCS；长度 <= 1514）。返回 0 = 已交给硬件；-1 = 失败/等描述符超时。
int e1000_send64(const void* frame, int len);

// 非阻塞收帧：>0 = 收到的字节数（已拷贝到 buf，最多 max 字节）；0 = 暂时没有；-1 = 驱动不可用。
int e1000_recv64(void* buf, int max);

const uint8_t* e1000_mac64();   // 读到的 6 字节 MAC；没有网卡/未初始化时返回 nullptr
uint64_t e1000_tx64();          // 累计成功提交给硬件的 TX 帧数
uint64_t e1000_rx64();          // 累计从 RX 描述符环取走的帧数
int e1000_link64();             // 1 = STATUS.LU 链路 up；0 = down/不可用

// 自检：改一次寄存器（IMS）读回 + 描述符环地址回读 + MAC/RCTL 合理性。
// 返回 0 = PASS；>0 = 失败位掩码；-1 = 没网卡（串口打 skipped，不算失败）。
int e1000_selftest64();
