// hwui64.h - 屏幕硬件检查报告（真机没有串口时**唯一**的诊断手段）
//
// 为什么需要它：
//   真机上我们既没有串口日志、也没有别的输出通道。装不上/开不起来的时候，用户能看到的
//   只有屏幕。这一页把内核**真探测到的**硬件事实逐条画在屏幕上（中英双语、黑底浅字、
//   分区块），让"黑屏/找不到盘/键盘没反应"这类问题当场可判：
//     * 找不到盘   -> 看"存储"区块：有没有 AHCI 控制器/端口、盘型号、NVMe（本系统不支持）
//     * 键盘没反应 -> 看"输入"区块：PS/2 8042 在不在、USB 只有 UHCI（EHCI/xHCI 不支持）
//     * 黑屏/花屏   -> 看"显示"区块：framebuffer 分辨率/位深/EDID/模式数
//
// 三个展示时机（互不冲突，全部复用同一份报告内容与同一个绘制函数）：
//   1) 启动期：进安装程序/桌面**之前**显示约 5 秒（任意键跳过）—— kernel64.cpp 的 kmain64
//      -> hwui64_show64(5000)
//   2) 安装程序找不到任何磁盘时：直接在向导里画这份报告（不让人面对空白列表）
//      -> setup64.cpp 的 draw_disk_page
//   3) 桌面：设置应用的第 5 页"硬件检查"（h/w check）-> settings64.cpp
//
// 串口打点（自动验收 grep）：
//   [HWUI] report lines=<n> storage=<n> controllers=<n>      每次**首次**构建报告时一行
//   [HWUI] report shown ms=<n> skipped=<0|1>                 启动期展示结束时一行
//   [HWUI] report drawn in setup wizard (no usable disk)      向导无盘时一行
//   [HWUI] selftest PASS / FAIL mask=<n>
//
// 边界（如实写清）：
//   * ATAPI 光驱若挂在 AHCI 上：只识别（型号），**读盘不支持**（见 ahci64.h）。
//   * NVMe / EHCI / xHCI 只**检测并标注"不支持"**，不做任何驱动。
//   * "PS/2 键鼠是否存在"只能给到"8042 控制器在不在 + 驱动已装载"，真机上有没有接设备
//     由硬件决定（USB 键鼠本系统不支持，报告里会一起说明）。
#pragma once
#include <stdint.h>

// 报告最多多少行（够放所有区块；超出部分在页面上如实标注"还有 N 行未显示"）
#define HWUI64_MAX_LINES   34
#define HWUI64_LINE_MAX   128

// 构建报告（幂等；内部会确保 ahci64/display64 已初始化，全部只读探测）。
// 返回行数（>=1）。行内容同时缓存在内部，供 hwui64_draw64 与自动验收读取。
int hwui64_build64();

int         hwui64_line_count64();
const char* hwui64_line64(int i);
int         hwui64_line_is_section64(int i);   // 1 = 区块标题行（绘制时画色块）

// 统计（对应 [HWUI] report lines= 打点）：storage = 认到的盘数，controllers = 认到的控制器数
int hwui64_storage_count64();
int hwui64_controller_count64();

// 把报告画到矩形区域（黑底浅字 + 区块色块）；返回实际画了多少行。
// hint != 0 时在底部画"按任意键继续 / press any key"提示。
int hwui64_draw64(int x, int y, int w, int h, int hint);

// 启动期展示：画整屏报告并最多等待 ms 毫秒（任意键提前结束），结束时打
// "[HWUI] report shown ms=<n> skipped=<0|1>"。返回 1 = 用户按键跳过。
int hwui64_show64(uint32_t ms);

// 自检：报告构建成功 + 行数/统计自洽（位掩码，0 = 通过）；内部打 [HWUI] selftest 行。
int hwui64_selftest64();
