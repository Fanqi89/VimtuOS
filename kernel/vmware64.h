// vmware64.h - ★ v0.9.18：VMware SVGA II（15AD:0405）的**只读**能力探测（3D 能力位取证）
//
// 为什么另开一份：QEMU 的 3D 走 virgl，VMware 的 3D 走 **SVGA3D**（完全不同的协议）。
// 在实现 SVGA3D 命令之前，至少要把"这台机器的虚拟 GPU 到底有没有 3D 能力"读出来 ——
// 那只需要读几个 SVGA 寄存器（不 enable 设备、不改显示模式、不写 FIFO）。
//
// 关键一点：VMware 的 SVGA BAR0 常常是 **I/O 端口 BAR**（不是 MMIO），所以本探测同时支持
// 两种访问方式；寄存器索引/数据两个口 = base+0 / base+1。
#pragma once
#include <stdint.h>

// 读 SVGA 寄存器并打一行 [VMWARE] svga …。返回 1 = 读成功（id 可识别），0 = 读不到/不认识。
int vmware64_svga_probe64(uint32_t bdf, int bar0_is_io, uint64_t bar0_base);
