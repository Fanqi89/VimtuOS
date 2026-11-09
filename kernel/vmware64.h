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
// ★ v0.9.19 阶段 1：真正驱动设备（配 FIFO -> CONFIG_DONE -> SYNC/BUSY 往返 -> ENABLE -> UPDATE）。
//   opt-in：只有卷里有 /etc/gpu3d_probe 才动；且要求 SVGA_REG_FB_START == 当前 LFB 且几何一致，
//   否则一律跳过（绝不拿显示去赌）。返回 1 = FIFO 命令通路已验证。
int vmware64_svga_stage1_64(void);

// ★ v0.9.34：**真的让宿主看见**（RegisterMode 下缺的那一步）
//   实测（VMware Workstation + VNC 逐帧取证）：客人把 1024x768x4 全屏画进 LFB，宿主侧
//   看到的是 99.8% 纯黑（768 行里 760 行整行全黑）；把 LFB 映射改成不缓存（PCD）之后
//   仍然是 99%，所以"缓存没落地"这条假说被实测否掉了。vmware.log 给的是另一半答案：
//     `SVGA-ScreenMgr: Screen type changed to RegisterMode` + `SWBScreen: Screen 1 Defined`
//   —— 宿主在 RegisterMode 下用自己的影子屏呈现，**只按 `SVGA_CMD_UPDATE` 声明的矩形**
//   把客人帧缓冲搬进影子屏。没有 UPDATE，客人写多少都没人看。
//   本接口就是那一步：只配 FIFO（CONFIG_DONE + SYNC，**不写 ENABLE** —— ENABLE 会"接管显示"，
//   v0.9.19 实测会卡在桌面之前），之后每次区域提交都追加一条 UPDATE。
//   返回 1 = 通路就绪（update 会真的发出去），0 = 不是 VMware / 几何不符 / FIFO 配不上。
int  vmware64_present_init64(void);
// 提交一块脏矩形（x,y,w,h；已按屏夹过）。**不阻塞**：只写 5 个字 + 推进 NEXT_CMD，
//   每 64 条做一次有界 BUSY 等待。没有初始化成功时是空操作。
void vmware64_update64(int x, int y, int w, int h);
