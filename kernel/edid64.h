// edid64.h - 运行期 EDID 解析（只读：不改 fb 状态、不写任何硬件寄存器）
//
// 数据从哪来（不要在这里重复探测）：
//   引导层已经把第一块 128 字节 EDID 读到物理 0x7600 了 ——
//   boot/loader64.asm 的 read_edid（VBE/DDC 4F15 BL=01，块 0）在写 BootInfo 时把
//   缓冲地址/长度/成功标志填进 BootInfo.edid_addr / edid_size / edid_ok；
//   见 kernel/memlayout64.h 的内存布局注释（0x7600 EDID 缓冲 128B）。
//   内核虽然跑在高半区，但引导层保留了前 4GB 恒等映射（ML64_IDENTITY_BYTES），
//   所以物理地址可以直接当指针用 —— 本模块不依赖内核堆、不依赖 fb 初始化，
//   在 kernel64.cpp 的 hwinfo/acpi 那一段（fb_init 之前）就能安全调用。
//
// ★ 边界（如实写清，别把没做的说成做了）：
//   1) 块 0（128 字节）全解析；块 1 **只当它是 CEA-861 扩展块**时才解析（[0]=0x02），
//      且只解码三类东西：Video Data Block 的 VIC、Vendor Specific 的 HDMI OUI、
//      扩展块自己的 18B 详细时序。块号 >= 2 的扩展块（块映射 / DisplayID / 厂商扩展）
//      **一律不解析**，只保留 ext_count 这个数字。VIC 表也只放能逐条核对的 1..16 + 31..34，
//      表外 VIC 如实跳过并计数（[EDID] ext=… skip=<n>），绝不编造分辨率。
//   2) 首选时序 = 第一个"详细时序描述符"（像素时钟 != 0）。EDID 规定第一个 DTD
//      就是要首选的那个（EDID 1.3/1.4 §3.10.3），所以不做"按 flag 挑"的额外逻辑。
//   3) 刷新率只用 pclk / (Htotal * Vtotal) 算（整数、保留一位小数）。不含时序
//      交错/双扫修正（DTD flags 的隔行位这里不解释），也不读 0x3DA / CRTC 回扫 ——
//      那是 32 位 display.cpp 的老路，64 位没有那条路，本模块的数字全部来自 EDID 文本。
//   4) 尺寸 cm 取 basic display parameters（offset 21/22）；tag 0xFD 描述符里装的是
//      **刷新率范围**（垂直 Hz / 水平 kHz），不是尺寸，两者分开记录、不混用。
//   5) 本模块只信 BootInfo.edid_ok（引导层验过头部前 4 个字节）；edid_ok = 0 时
//      缓冲内容算无效数据，不解析、不打印任何"猜"出来的字段。
#pragma once
#include <stdint.h>

#define EDID64_NAME_MAX  14u   // 显示器名字：描述符里最多 13 字节 ASCII + NUL
#define EDID64_MFG_MAX    4u   // 厂商 ID：3 字母 + NUL（取不到时写 "???"）
#define EDID64_SRC_MAX    8u   // 来源标记："vbios" / "none"

// 一块 EDID 解析后的结果（POD：无构造/析构/虚表，可静态零初始化）
struct Edid64 {
    bool     valid;              // 头部 + 校验和都对、且拿到了首选时序
    bool     present;            // 固件给了 EDID（BootInfo.edid_ok != 0）
    char     source[EDID64_SRC_MAX];  // "vbios" = 固件缓冲；"none" = 没数据
    uint8_t  version_major;      // EDID 版本 major（1）
    uint8_t  version_minor;      // EDID 版本 minor（3 / 4 ...）
    char     mfg[EDID64_MFG_MAX];// 厂商 ID：EDID 的 3x5bit 压缩解码；取不到 = "???"
    uint16_t product_code;       // 产品码（offset 10..11，小端）
    uint32_t product_serial;     // 序列号（offset 12..15，小端；0 = 厂商没写）
    uint8_t  mfg_week;           // 制造周（0 = 未写；1.4 里 0xFF = "model year" 标记）
    uint16_t mfg_year;           // 制造年（四位年：1990 + offset 17，见 .cpp 的版本口径说明）
    uint8_t  ext_count;          // EDID 扩展块个数（>=1 时本模块只解析块 1，见边界 1)）
    // ---- ★ v0.9.23：块 1（CEA-861）的结论 ----
    uint8_t  interlaced;         // 首选 DTD 是否隔行（DTD flags bit7）
    uint8_t  cea_ok;             // 1 = 块 1 头字节是 0x02（CEA-861），且已解析
    uint8_t  hdmi;               // 1 = 找到 tag 3 且 OUI = 0x000C03（HDMI Licensing）
    uint8_t  vic_used;           // 采纳进模式表的 VIC 条数
    uint8_t  vic_skip;           // VIC 表里没有、如实跳过的条数
    uint8_t  ext_dtd_n;          // 扩展块里解析出的 18B 详细时序条数
    char     name[EDID64_NAME_MAX];   // 显示器名字（tag 0xFC），去尾空格/换行；取不到 = "unknown"

    // ---- 屏幕尺寸（basic display parameters，0 = 未写）----
    uint8_t  size_cm_w;
    uint8_t  size_cm_h;

    // ---- 刷新率范围（tag 0xFD 描述符；0 = 未写）----
    uint8_t  range_v_min;        // 最小垂直频率 Hz
    uint8_t  range_v_max;        // 最大垂直频率 Hz
    uint8_t  range_h_min;        // 最小水平频率 kHz
    uint8_t  range_h_max;        // 最大水平频率 kHz
    uint8_t  range_pclk_max10;   // 最大像素时钟 / 10 MHz（0 = 未写）

    // ---- 首选详细时序（第一个 DTD）----
    uint32_t pclk_khz;           // 像素时钟 kHz
    uint16_t h_active;           // 水平有效像素
    uint16_t v_active;           // 垂直有效行
    uint16_t h_blank;            // 水平消隐（Htotal = h_active + h_blank）
    uint16_t v_blank;            // 垂直消隐（Vtotal = v_active + v_blank）
    uint16_t h_front;            // 水平前肩（sync offset）
    uint16_t h_sync;             // 水平同步宽度
    uint16_t v_front;            // 垂直前肩
    uint16_t v_sync;             // 垂直同步宽度
    uint32_t refresh_x10;        // 刷新率 * 10（整数算术；内核 -mno-sse 无浮点）
};

// 从 0x7600 读第一块（128B）→ 校验 → 解析 → 按固定格式打 [EDID64] 串口日志。
// 可重复调用（幂等）。固件没给 EDID 时打印
//   [EDID64] present=0 (no EDID from firmware)
// 并让所有 getter 返回 valid = false（绝不编造数字）。校验失败打印
//   [EDID64] bad header/checksum
void edid64_init64();

// 启动期自检：返回 0 = 全通过，非 0 = 失败项位掩码（位含义见 edid64.cpp 顶部）。
// 解析逻辑用**内置合成样本**自证（离线用例），所以真机/虚拟机上没有 EDID 也能通过。
// 内部打印 "[EDID64] selftest PASS" 或 "[EDID64] selftest FAIL mask=<n>"。
int edid64_selftest64();

// ==================== ★ v0.9.22：显示器支持的**模式列表** ====================
// 来源（全部来自 EDID 文本，不猜）：首选 + 其余 3 个详细时序（DTD）、建立时序（0x23/0x24 位图）、
// 标准时序（0x26..0x35，8 项）。**扩展块（CEA-861）仍不解码** —— 见边界 1)。
// kind：0 = DTD（含首选；块 0 与块 1 的都算）/ 1 = 建立时序 / 2 = 标准时序 / 3 = CEA-861 VIC
// interlaced：1 = 隔行（DTD flags bit7 / 已知的 1024x768@87i 建立时序）。
//   注意：**Drm64ModeMode.flags 报 0x10（DRM_MODE_FLAG_INTERLACE）**，但刷新率仍按
//   pclk/(Htotal*Vtotal) 算 —— 隔行模式下 Vtotal 是"每场行数"，所以这个数字是**场频**
//   （1024x768@87i 会显示成 87.0Hz）。要帧频就除以 2，本模块不擅自改口径。
#define EDID64_MODE_MAX 32u
struct Edid64Mode {
    uint16_t w, h;
    uint32_t refresh_x10;      // Hz * 10（建立的固定时序用标称值；标准的用 (b2&0x3F)+60）
    uint8_t  kind;
    uint8_t  interlaced;
};

// 只读访问解析结果（静态存储，永不为空；从未初始化过时 valid = false）。
const Edid64* edid64_get();

// 模式列表（按"首选在前"排；没有 EDID 时返回 0 条）
int                edid64_mode_count64();
const Edid64Mode*  edid64_mode_at64(int i);

// ★ v0.9.23：原始字节（块 0，块 1 校验通过时含块 1）。返回 nullptr = 没有有效 EDID。
//   len 如实反映实读字节数：128 = 只有块 0；256 = 块 1 也在（且校验和通过）。
const uint8_t*     edid64_raw64();
int                edid64_raw_len64();

// 把刷新率写成 "60.0"（无单位，一位小数）。返回 1 = 有值；0 = 未知（此时写 "unknown"）。
// 想显示成 "60.0 Hz" 的调用方自己在后面补单位。
int edid64_refresh_str64(char* out, int cap);
