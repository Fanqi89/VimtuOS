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

// ★ v0.9.37：真 SVGA 呈现通路（screen object + GMRFB + 命令流）。默认生效，不需要任何 marker。
//   与 v0.9.34～v0.9.36 的四点修正（全部对照 svga_reg.h 与 Linux vmwgfx 的参考实现）：
//     ① GMR 寄存器号是 41/42/43/44（25..28 是光标/HOST_BPP，之前搞错了）；
//     ② FIFO 的 MIN 取 SVGA_REG_MEM_REGS<<2（不足一页取 4096），并在 CONFIG_DONE 之前
//        把 fifo[SVGA_FIFO_BUSY] 清零（vmwgfx vmw_fifo_init 同口径）；单位一律字节；
//     ③ 提交之后的"敲门"是 fifo[SVGA_FIFO_BUSY]=1 再写 SVGA_REG_SYNC
//        （vmw_fifo_ping_host）。之前只写 SYNC、BUSY 恒为 0 —— 宿主会当成"没有待办"。
//     ④ 呈现走 SVGA_CMD_DEFINE_SCREEN（backing store = SVGA_GMR_FRAMEBUFFER）+ 每块脏矩形
//        的 DEFINE_GMRFB + BLIT_GMRFB_TO_SCREEN；define 没做成时回退 SVGA_CMD_UPDATE。
//   返回 1 = 通路就绪（update 会真的发出去），0 = 不是 VMware / 几何不符 / FIFO 被宿主打回。
int  vmware64_present_init64(void);
// 提交一块脏矩形（x,y,w,h；已按屏夹过）。**不阻塞**：写命令 + 敲门，每 64 块做一次有界
//   BUSY 等待并打点（`present n=… stop=… next=… drop=… busy=… used=…`，就是"宿主有没有
//   在消费 FIFO"的取证）。没有初始化成功时是空操作。
void vmware64_update64(int x, int y, int w, int h);

// ★ v0.9.37：**呈现心跳**（定时器驱动）。由 kheart 线程每约 60 ms 调一次，内部限到 4 Hz
//   无条件把整屏标脏一次。为什么必须有：宿主在 RegisterMode 下只把 UPDATE 声明的矩形搬进影子屏，
//   静止桌面没人提交 -> 屏上永远停在旧内容。实测依据见 vmware64.cpp 里这段的注释。
void vmware64_present_tick64(void);

// ★ v0.9.35：**LFB 可见性二分探针**（opt-in：卷上有 /etc/gpu3d_probe 才跑）。
//   目的只有一个：回答"客人写进 LFB 的像素，宿主到底读不读"。
//   做法：在一段时间窗口内，每次区域提交之后**直接**把整屏刷成 8 条饱和色竖条
//   （只写 LFB，不经过后备缓冲/合成器），然后从宿主侧 VNC 抓帧看有没有。
//   * 抓到彩条 -> 宿主读我们的显存；那"桌面看不见"就另有原因（写的位置/时序/格式）。
//   * 抓不到   -> 宿主根本不读我们写的这块；下一步只能走真 SVGA 驱动的 screen 通路。
//   窗口 20 秒（5000 tick @250Hz），足够宿主侧慢慢抓帧。
int  vmware64_lfbprobe_begin64(void);
void vmware64_lfbprobe_tick64(void);
