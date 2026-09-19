// display64.h - 运行期显示层：真实模式清单 + 0x3DA 垂直回扫实测刷新率 + 与 EDID 首选时序对比
//
// 与 32 位 kernel/display.cpp（855 行）的关系：**移植能如实做到的那部分**，做不到的如实标注：
//   移植：
//     1) 模式清单：解析引导期 loader64.asm 放在物理 0x7400 的可用模式表（<=16 项，见
//        kernel/memlayout64.h 的内存布局与 bootinfo.h 的 BootMode / mode_count）；
//     2) 0x3DA 实测刷新率：VGA 输入状态寄存器 1（bit3 = 垂直回扫中，bit0 = display enable），
//        用 g_ticks64 划窗 + 密集采样数上升沿边沿 -> Hz。窗口/样本数都有硬上限（绝不死循环）；
//     3) 与 EDID 首选时序刷新率（kernel/edid64.cpp 从物理 0x7600 解析）对比并打点：
//        [DISP64] vga measure=<n>Hz edid=<m>Hz match=<0|1>；
//     4) CRTC 时序推算作为 0x3DA 不可测时的备用（读 0x3CC/0x3D4/0x3D5 + 与当前分辨率自洽检查）。
//   未移植 / 如实跳过：
//     * 运行期 DDC/EDID 再探测：32 位那套 0x3C2 位操作 I2C 在主板上风险太高（写 Misc Output
//       寄存器一旦恢复失败就会改显示时序），本模块**不做** —— 刷新率/显示器信息只用引导期
//       EDID（一次性）。打点里明确写 ddc runtime re-probe skipped，绝不假装做了。
//     * 运行期切分辨率/改刷新率：Bochs VBE DISPI 的切换仍由 fb_set_mode + 设置页负责
//       （fb.cpp 既有实现）；本模块只**报告**，不改模式、不写任何时序寄存器。
//     * EDID 扩展块（CEA-861）不解析（见 edid64.h 的边界）。
//
// 串口打点（自动验收 grep，格式勿改）：
//   [DISP64] init modes=<n> cur=<W>x<H>@<Hz|unknown> src=edid|vga|crtc|none
//   [DISP64] mode list i=<i> mode=0x<hex> <W>x<H>x<bpp>
//   [DISP64] mode list=<n>                       可用模式总数（清单条数）
//   [DISP64] vga measure=<n>Hz samples=<n>       可测路径
//   [DISP64] vga measure=<n>Hz edid=<m>Hz match=<0|1>
//   [DISP64] vga measure=n/a samples=<n> edges=<e> reason=<...>   不可测路径（如实降级）
//   [DISP64] crtc measure=<n>Hz clk=<kHz> htotal=<n> vtotal=<n>
//   [DISP64] ddc runtime re-probe skipped (i2c bit-bang risk; boot-time EDID only)
//   [DISP64] selftest PASS / [DISP64] selftest FAIL mask=<n>
#pragma once
#include <stdint.h>

#define DISP64_MODE_MAX      16   // 与 bootinfo.h 的 BOOT_MODE_MAX 一致（模式表容量）
#define DISP64_NAME_MAX      24

// 刷新率来源（如实标注；none = 测不到）
#define DISP64_SRC_NONE  0
#define DISP64_SRC_VGA   1        // 0x3DA 垂直回扫实测
#define DISP64_SRC_CRTC  2        // CRTC 时序寄存器推算（需通过自洽检查）
#define DISP64_SRC_EDID  3        // EDID 首选详细时序（显示器报告的时序，不是实测）

// 一个可用显示模式（引导期 VBE 探测的真实值，非硬编码）
struct Disp64Mode {
    uint16_t mode;                // VBE 模式号
    uint16_t width;               // 水平分辨率（像素）
    uint16_t height;              // 垂直分辨率（像素）
    uint8_t  bpp;                 // 色深位
    uint8_t  pad;
};

// 显示层结论（POD，可静态零初始化；display64_init64() 之后只读）
struct Disp64Info {
    int      w, h, bpp, pitch;    // 当前物理模式（BootInfo 实测）
    uint16_t mode_num;            // 当前 VBE 模式号（0 = 无 BootInfo）
    int      mode_count;          // 可用模式条数（0 = 没有清单）
    int      refresh_x10;         // 采用值 ×10（0 = 未知）
    int      src;                 // DISP64_SRC_*
    int      vga_x10;             // 0x3DA 实测 ×10（0 = 不可测）
    int      vga_samples;         // 实测采样次数
    int      vga_edges;           // 实测上升沿数
    int      crtc_x10;            // CRTC 推算 ×10（0 = 不可用）
    int      edid_x10;            // EDID 首选时序 ×10（0 = 无 EDID）
    int      edid_match;          // 1 = 实测/采用值与 EDID 在 ±5% 内一致；0 = 不一致/无法比较
    int      init_done;           // 1 = 已探测过
    int      from_fb;             // 1 = BootInfo 不可用，退回 fb 物理分辨率（如实标注）
};

// 启动期调用一次：解析模式表 + 0x3DA 实测 + CRTC 备用 + EDID 对比 + 打点。幂等。
void display64_init64();

// 重新实测一遍（不重新读 EDID、不碰 DDC）：模式切换后可由设置页/终端调用。
void display64_reprobe64();

// 自检（位掩码，0 = 全过）；内部打印 [DISP64] selftest PASS / FAIL mask=<n>。
int display64_selftest64();

// 只读访问（静态存储，永不为空）
const Disp64Info* display64_info64();
int               display64_mode_count64();
const Disp64Mode* display64_mode_at64(int i);

// 刷新率格式化："60.0"（无单位）。返回 1 = 有值；0 = 未知（此时写 "unknown"，绝不编数字）。
int display64_refresh_str64(char* out, int cap);

// 把当前结论按固定格式打一行（终端 display 命令 / 设置页首帧用）：
//   [DISP64] report WxH@<Hz|unknown> src=<...> vga=<n|n/a> crtc=<n|n/a> edid=<m|n/a> match=<0|1>
void display64_report64();

// 来源名（"edid"/"vga"/"crtc"/"none"）—— 打点与界面文案共用。
const char* display64_src_name64(int src);
