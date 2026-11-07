// edid64.cpp - 运行期 EDID 解析（只读：不改 fb 状态、不写任何硬件寄存器）
//
// 定位与边界见 edid64.h 顶部（只解析第一块 128B / 刷新率 = pclk/(Htotal*Vtotal) /
// 尺寸取 basic display parameters，0xFD 里装的是刷新率范围不是尺寸）。
//
// 串口日志（自动验收按行 grep，格式必须逐字一致）：
//   [EDID64] present=1 ver=1.4 mfg=RHT name=QEMU Monitor size=32x20cm
//   [EDID64] preferred pclk=107300 Vactive=800 Hactive=1280 refresh=75.0Hz
// （上面两行是实测原文：QEMU -vga std 的内置 EDID。没有 EDID 的机器上是下面这行：）
//   [EDID64] present=0 (no EDID from firmware)
//   [EDID64] bad header/checksum
//   [EDID64] selftest PASS   /   [EDID64] selftest FAIL mask=<n>
// 另有两行诊断（前缀同为 [EDID64]，便于人/自动验收核对算术）：
//   [EDID64] verify header=1 checksum=1              验证结果（1 = 过）
//   [EDID64] detail prod=0x1234 serial=0 week=42 year=2014 ext=1 htotal=.. vtotal=.. range_v=..  ← 只在有效时打
//
// 自检位掩码（0 = 全通过）：
//   bit 0  合法合成样本被判坏（头部/校验和验证把好数据当坏数据）
//   bit 1  厂商 ID（3x5bit 压缩）解码错
//   bit 2  产品码 / 序列号解析错
//   bit 3  EDID 版本 / 制造周 / 制造年解析错
//   bit 4  显示器名字（tag 0xFC）解析错
//   bit 5  首选时序（pclk / Hactive / Vactive / 消隐 / 前后沿 / 同步宽度 / 刷新率）解析错
//   bit 6  屏幕尺寸 / 刷新率范围（tag 0xFD）/ 扩展块计数解析错
//   bit 7  坏样本：篡改校验和的块没有被拒绝
//   bit 8  坏样本：篡改头部的块没有被拒绝
//   bit 9  固件给了 EDID（BootInfo.edid_ok = 1）但本模块验证不过（头部/校验和/首选时序缺）
//   bit 10 与固件缓冲的独立复核不一致（hact/vact 用"简单偏移读法"复算的结果不同）
//   bit 11 ★v0.9.23 CEA-861 扩展块解析错（cea/hdmi/vic 计数/扩展 DTD/VIC 分辨率）
//   bit 12 ★v0.9.23 隔行位解析错（1920x1080i 与 1920x1080p 没有各自留一条）
#include "edid64.h"
#include "../bootinfo.h"
#include "debug64.h"

// ---- 引导层固定槽（kernel/memlayout64.h 的内存布局注释：0x7600 EDID 缓冲 128B）----
// memlayout64.h 只给了 RSDP 的 0x7800 常量，EDID 地址没有常量，这里写一份回退值：
// 正常路径用 BootInfo.edid_addr（引导层填的），只有 BootInfo 本身不可用（magic 不对）时才回退。
static const uint32_t EDID64_FALLBACK_ADDR = 0x7600;
static const uint32_t EDID64_BLOCK_BYTES   = 128;
static const uint32_t EDID64_RAW_BYTES     = 256;   // ★ v0.9.23：块 0 + 块 1（CEA-861）
static const uint32_t EDID64_DESC_OFF      = 54;    // 4 个 18 字节描述符的起点
static const uint32_t EDID64_DESC_SIZE     = 18;

static Edid64  g_edid;          // 解析结果（静态存储；valid = false 表示"不知道"）
static uint8_t g_raw[EDID64_RAW_BYTES];    // 块 0(+块 1) 原始字节（仅有效时保留，供独立复核/扩展块解析）
static uint32_t g_raw_len = 0;             // 实读字节数：128 = 只有块 0；256 = 块 1 校验也过
static bool    g_have_raw = false;
static bool    g_inited   = false;
// ★ v0.9.22：显示器支持的模式列表（首选在前；全部来自 EDID 文本）
static Edid64Mode g_modes[EDID64_MODE_MAX];
static int        g_mode_n = 0;
static bool    g_fw_flag  = false;         // BootInfo.edid_ok（固件是否给了 EDID）
// ★ v0.9.23：自检里合成 CEA-861 块（块 1）的解析结论 —— 只用于把"解析器真的工作"打成可见证据
static uint8_t g_st_cea, g_st_vic, g_st_dtd, g_st_hdmi, g_st_il;

// ==================== 无 libc 的最小字符串工具 ====================
static void e64_strcpy(char* dst, const char* src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    if (src) { while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; } }
    dst[i] = 0;
}

static int e64_strcmp(const char* a, const char* b) {
    for (int i = 0;; i++) {
        if (a[i] != b[i]) return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
        if (!a[i]) return 0;
    }
}

// 清成"未知"语义（POD：直接清零 + 三个兜底串）。内核没有 libc，不用 memset。
static void e64_reset(Edid64* o) {
    uint8_t* p = (uint8_t*)o;
    for (uint32_t i = 0; i < (uint32_t)sizeof(Edid64); i++) p[i] = 0;
    e64_strcpy(o->mfg, "???", EDID64_MFG_MAX);
    e64_strcpy(o->name, "unknown", EDID64_NAME_MAX);
    e64_strcpy(o->source, "none", EDID64_SRC_MAX);
}

// 刷新率 * 10 -> 串口（一位小数；内核 -mno-sse，没有浮点）
static void e64_print_refresh(uint32_t x10) {
    dbg64_dec(x10 / 10);
    dbg64_str(".");
    dbg64_dec(x10 % 10);
}

// ==================== 校验（头部 + 校验和） ====================
// 头部：00 FF FF FF FF FF FF 00
static int e64_hdr_ok(const uint8_t* d) {
    if (d[0] != 0x00) return 0;
    for (int i = 1; i <= 6; i++) if (d[i] != 0xFF) return 0;
    return d[7] == 0x00;
}

// 校验和：前 128 字节之和 == 0 (mod 256)
static int e64_sum_ok(const uint8_t* d) {
    uint32_t s = 0;
    for (uint32_t i = 0; i < EDID64_BLOCK_BYTES; i++) s += d[i];
    return (s & 0xFFu) == 0;
}

// ==================== 描述符解析 ====================
// 详细时序描述符（18B；像素时钟 != 0 才算，见 EDID 1.4 §3.10.2）
static void e64_parse_dtd(const uint8_t* d, Edid64* o) {
    o->pclk_khz = (((uint32_t)d[1] << 8) | (uint32_t)d[0]) * 10u;   // 单位 = 10 kHz
    o->h_active = (uint16_t)(d[2] | ((d[4] & 0xF0) << 4));
    o->h_blank  = (uint16_t)(d[3] | ((d[4] & 0x0F) << 8));
    o->v_active = (uint16_t)(d[5] | ((d[7] & 0xF0) << 4));
    o->v_blank  = (uint16_t)(d[6] | ((d[7] & 0x0F) << 8));
    // 前后沿 / 同步宽度：低 8/4 位在 8/9/10，高 2 位打包在 11（hfront/hsync 各 3+8=11 位口径：
    // 低 8 位 + 高 2/2 位；vfront/vsync 是 4+2 = 6 位口径，高 2 位在 11 的 bit3:2 / bit1:0）
    o->h_front = (uint16_t)(d[8] | ((d[11] & 0xC0) << 2));
    o->h_sync  = (uint16_t)(d[9] | ((d[11] & 0x30) << 4));
    o->v_front = (uint16_t)((d[10] >> 4) | ((d[11] & 0x0C) << 2));
    o->v_sync  = (uint16_t)((d[10] & 0x0F) | ((d[11] & 0x03) << 4));
    // ★ v0.9.23：DTD flags（dd[17]）bit7 = 隔行（EDID 1.4 §3.10.2 的 bit7 定义）。
    //   只记这个位，**不改**下面的算术口径：EDID 的 Vtotal 是"每场行数"，
    //   所以算出来的刷新率是**场频**（1024x768@87i -> 87.0），不是帧频。
    o->interlaced = (uint8_t)((d[17] >> 7) & 1u);

    uint32_t ht = (uint32_t)o->h_active + o->h_blank;
    uint32_t vt = (uint32_t)o->v_active + o->v_blank;
    if (ht && vt) {
        // refresh(Hz) = pclk(Hz) / (Htotal * Vtotal)；
        // 保留一位小数（**四舍五入**）-> x10 = pclk_kHz * 10000 / (Htotal * Vtotal) + 半个除数，
        // 全程整数、uint64 防溢出（内核 -mno-sse，没有浮点）。
        uint64_t den = (uint64_t)ht * (uint64_t)vt;
        o->refresh_x10 = (uint32_t)((((uint64_t)o->pclk_khz * 10000ULL) + den / 2) / den);
    } else {
        o->refresh_x10 = 0;
    }
}

// 显示器描述符里的 ASCII 文本（名字 0xFC / 序列号 0xFF 共用）：13 字节，去尾空格/换行
static void e64_parse_text(const uint8_t* d, char* out, int cap) {
    int n = 0;
    for (int i = 5; i < 18 && n < cap - 1; i++) {
        char c = (char)d[i];
        if ((uint8_t)c == 0x00 || c == '\n' || c == '\r') break;  // 0 = 厂商没填满
        if ((uint8_t)c < 0x20) c = ' ';                           // 其它控制字符当空格（不信垃圾字节）
        out[n++] = c;
    }
    while (n > 0 && out[n - 1] == ' ') n--;                       // 去尾空格
    out[n] = 0;
}

// 往模式表里加一条（按 w/h 去重，满就丢——有上限，绝不越界）
// 去重键 = (w, h, 隔行)：1920x1080p 与 1920x1080i 是**两个**模式，不能互相顶掉。
static void e64_mode_add(uint16_t w, uint16_t h, uint32_t refresh_x10, uint8_t kind, uint8_t ilace) {
    if (w < 320u || h < 200u) return;
    for (int i = 0; i < g_mode_n; i++)
        if (g_modes[i].w == w && g_modes[i].h == h && g_modes[i].interlaced == ilace) return;
    if (g_mode_n >= (int)EDID64_MODE_MAX) return;
    Edid64Mode* m = &g_modes[g_mode_n++];
    m->w = w; m->h = h; m->refresh_x10 = refresh_x10; m->kind = kind; m->interlaced = ilace;
}

// ★ v0.9.23：CEA-861 的 VIC（Video Identification Code）表。
//   只放**能逐条核对**的 VIC 1..16 与 31..34（CEA-861-F Table 3 的标清/高清常用段）。
//   表里没有的 VIC 一律不猜分辨率：跳过并计数（[EDID] ext=… skip=<n>）。
//   注意 1..15 里有一半是隔行/半帧（240p/480i），ilace 位如实标注。
struct E64Vic { uint8_t vic; uint16_t w, h; uint8_t hz; uint8_t ilace; };
static const E64Vic E64_VIC[] = {
    {  1,  640,  480,  60, 0 }, {  2,  720,  480,  60, 0 },
    {  3,  720,  480,  60, 0 }, {  4, 1280,  720,  60, 0 },
    {  5, 1920, 1080,  60, 1 }, {  6,  720,  480,  60, 1 },
    {  7,  720,  480,  60, 1 }, {  8,  720,  240,  60, 0 },
    {  9,  720,  240,  60, 0 }, { 10, 2880,  480,  60, 1 },
    { 11, 2880,  480,  60, 1 }, { 12, 2880,  240,  60, 0 },
    { 13, 2880,  240,  60, 0 }, { 14, 1440,  480,  60, 0 },
    { 15, 1440,  480,  60, 0 }, { 16, 1920, 1080,  60, 0 },
    { 31, 1920, 1080,  50, 0 }, { 32, 1920, 1080,  24, 0 },
    { 33, 1920, 1080,  25, 0 }, { 34, 1920, 1080,  30, 0 },
};
#define E64_VIC_N ((int)(sizeof(E64_VIC) / sizeof(E64_VIC[0])))

// CEA-861 扩展块（块 1）。块 1 布局（CEA-861-B 起稳定）：
//   [0] = 0x02（CEA-861 扩展块标识） [1] = revision [2] = DTD 偏移 d [3] = flags
//   [4 .. d-1] = data block collection（每块：tag = (b>>5)&7 / len = b&0x1F，随后 len 字节）
//   [d .. 126] = 扩展块自己的 18B 详细时序（格式与块 0 相同），全 0 填充即止
//   [127] = 校验和
// 只解码两件事：tag 2 = Video Data Block（VIC）、tag 3 = Vendor Specific（HDMI OUI）。
// 其它 tag（音频 / 扬声器分配 / 视频能力 / 颜色深度 …）如实跳过 —— 它们不是"模式"。
static void e64_parse_cea(const uint8_t* b, Edid64* o) {
    if (b[0] != 0x02u) return;                       // 不是 CEA-861 扩展块：不猜，直接不认
    if (o) o->cea_ok = 1;
    const uint32_t d = b[2];
    const uint32_t dend = (d >= 4u && d <= 127u) ? d : 4u;
    uint32_t i = 4u;
    while (i < dend) {
        const uint8_t hdr = b[i];
        const uint32_t tag = ((uint32_t)hdr >> 5) & 7u, len = (uint32_t)hdr & 0x1Fu;
        i++;
        if (i + len > 128u) break;                   // 越过块尾：这块坏了，不再往下读
        if (tag == 2u) {
            for (uint32_t k = 0; k < len; k++) {
                const uint8_t vic = (uint8_t)(b[i + k] & 0x7Fu);   // bit7 = "native"，先清掉
                int f = 0;
                for (; f < E64_VIC_N; f++) if (E64_VIC[f].vic == vic) break;
                if (f < E64_VIC_N) {
                    e64_mode_add(E64_VIC[f].w, E64_VIC[f].h,
                                 (uint32_t)E64_VIC[f].hz * 10u, 3u, E64_VIC[f].ilace);
                    if (o && o->vic_used < 255u) o->vic_used++;
                } else if (o && o->vic_skip < 255u) {
                    o->vic_skip++;                   // 表里没有：如实计数，不编造分辨率
                }
            }
        } else if (tag == 3u && len >= 3u) {
            // OUI 是 24 位小端：0x000C03 = HDMI Licensing, LLC
            if (b[i] == 0x03u && b[i + 1] == 0x0Cu && b[i + 2] == 0x00u && o) o->hdmi = 1;
        }
        i += len;
    }
    // 扩展块自己的 18B DTD（d 之后）。像素时钟 = 0 或全 0 填充即止。
    uint32_t k = dend;
    while (k + EDID64_DESC_SIZE <= 127u) {
        const uint8_t* dd = b + k;
        if (dd[0] == 0x00u && dd[1] == 0x00u) break;
        Edid64 one;
        e64_reset(&one);
        e64_parse_dtd(dd, &one);
        if (one.pclk_khz != 0u && one.h_active != 0u && one.v_active != 0u) {
            e64_mode_add(one.h_active, one.v_active, one.refresh_x10, 0u, one.interlaced);
            if (o && o->ext_dtd_n < 255u) o->ext_dtd_n++;
        }
        k += EDID64_DESC_SIZE;
    }
}

// 模式列表解析：4 个 DTD + 建立时序（0x23/0x24）+ 标准时序（0x26..0x35）+ 块 1（CEA-861）
// 说明：建立时序按 VESA E-EDID 的表逐位展开（byte 0x25 只有 bit7 = 1152x870@75，其余保留位不解码）；
//      byte 0x24 的 bit3 = 1024x768@87i 是**隔行**，v0.9.23 起保留并标 ilace=1（原来是直接跳过）。
//      b1 = 块 1 的指针（128B），没有就传 nullptr —— 这条路径绝不去读 0x7680 之外的东西。
static void e64_parse_modes(const uint8_t* d, const uint8_t* b1, Edid64* o) {
    g_mode_n = 0;
    if (o) { o->cea_ok = 0; o->hdmi = 0; o->vic_used = 0; o->vic_skip = 0; o->ext_dtd_n = 0; }
    // ① 4 个详细时序描述符（像素时钟 != 0）。第一个就是首选（EDID 1.4 §3.10.3）
    for (uint32_t k = 0; k < 4; k++) {
        const uint8_t* dd = d + EDID64_DESC_OFF + k * EDID64_DESC_SIZE;
        if (dd[0] == 0x00 && dd[1] == 0x00) continue;             // 显示器描述符，不是时序
        Edid64 one;
        e64_reset(&one);
        e64_parse_dtd(dd, &one);
        if (one.pclk_khz == 0u || one.h_active == 0u || one.v_active == 0u) continue;
        e64_mode_add(one.h_active, one.v_active, one.refresh_x10, 0u, one.interlaced);
    }
    // ② 建立时序：byte 0x23
    {
        static const uint16_t EW[8] = { 720, 720, 640, 640, 640, 640, 800, 800 };
        static const uint16_t EH[8] = { 400, 400, 480, 480, 480, 480, 600, 600 };
        static const uint16_t ER[8] = { 700, 880, 600, 670, 720, 750, 560, 600 };
        const uint8_t b = d[0x23];
        for (uint32_t i = 0; i < 8; i++)
            if (b & (uint8_t)(0x80u >> i)) e64_mode_add(EW[i], EH[i], (uint32_t)ER[i], 1u, 0u);
    }
    // ③ 建立时序：byte 0x24
    //    顺序按 E-EDID 的表（bit7 起）：800x600@72 / @75 / 832x624@75 / 保留 /
    //    1024x768@87i / 1024x768@60 / @70 / @75
    //    ★ v0.9.23 顺手修了旧表的错位（旧表把 @60 写成 700、@70 写成 750）
    {
        static const uint16_t EW[8] = { 800, 800, 832, 0, 1024, 1024, 1024, 1024 };
        static const uint16_t EH[8] = { 600, 600, 624, 0, 768,  768,  768,  768  };
        static const uint16_t ER[8] = { 720, 750, 750, 0, 870,  600,  700,  750  };
        static const uint8_t  EI[8] = { 0,   0,   0,   0, 1,    0,    0,    0    };
        const uint8_t b = d[0x24];
        for (uint32_t i = 0; i < 8; i++)
            if (b & (uint8_t)(0x80u >> i)) e64_mode_add(EW[i], EH[i], (uint32_t)ER[i], 1u, EI[i]);
    }
    // ④ byte 0x25：只有 bit7 = 1152x870@75（其余保留位，不解码）
    if (d[0x25] & 0x80u) e64_mode_add(1152, 870, 750u, 1u, 0u);
    // ⑤ 标准时序：8 项 × 2 字节（0x26..0x35）
    //    h = (b1 + 31) * 8；宽高比 0b00=16:10 / 0b01=4:3 / 0b10=5:4 / 0b11=16:9；刷新 = (b2 & 0x3F) + 60
    for (uint32_t i = 0; i < 8; i++) {
        const uint8_t b1x = d[0x26 + i * 2], b2 = d[0x27 + i * 2];
        if ((b1x == 0x01 && b2 == 0x01) || b1x == 0x00) continue;  // 0x0101 / 0x00 = 未用
        const uint32_t hpx = ((uint32_t)b1x + 31u) * 8u;
        const uint32_t asp = (uint32_t)(b2 >> 6);
        uint32_t vpx;
        if (asp == 0u)      vpx = hpx * 10u / 16u;                // 16:10
        else if (asp == 1u) vpx = hpx * 3u / 4u;                   // 4:3
        else if (asp == 2u) vpx = hpx * 4u / 5u;                   // 5:4
        else                vpx = hpx * 9u / 16u;                  // 16:9
        e64_mode_add((uint16_t)hpx, (uint16_t)vpx, (uint32_t)(b2 & 0x3Fu) * 10u + 600u, 2u, 0u);
    }
    // ⑥ ★ v0.9.23：块 1（CEA-861）—— VIC / HDMI OUI / 扩展 DTD 进同一张模式表
    if (b1) e64_parse_cea(b1, o);
}

static void e64_parse_block(const uint8_t* d, Edid64* o) {
    o->version_major = d[18];
    o->version_minor = d[19];

    // 厂商 ID：offset 8..9 大端，3 个 5bit 字母（1 = 'A'）
    uint16_t be = (uint16_t)(((uint16_t)d[8] << 8) | (uint16_t)d[9]);
    uint32_t c1 = (be >> 10) & 0x1Fu, c2 = (be >> 5) & 0x1Fu, c3 = be & 0x1Fu;
    if (c1 >= 1 && c1 <= 26 && c2 >= 1 && c2 <= 26 && c3 >= 1 && c3 <= 26) {
        o->mfg[0] = (char)('A' + (int)c1 - 1);
        o->mfg[1] = (char)('A' + (int)c2 - 1);
        o->mfg[2] = (char)('A' + (int)c3 - 1);
        o->mfg[3] = 0;
    } else {
        e64_strcpy(o->mfg, "???", EDID64_MFG_MAX);   // 压缩位域不合法 -> 不编造
    }

    o->product_code   = (uint16_t)(d[10] | ((uint16_t)d[11] << 8));
    o->product_serial = (uint32_t)d[12] | ((uint32_t)d[13] << 8) |
                        ((uint32_t)d[14] << 16) | ((uint32_t)d[15] << 24);

    // ★ 制造周/年的版本口径（这里必须显式判断，别写死一个基址）：
    //   EDID 1.1 / 1.2 / 1.3 / 1.4 的"制造年"字节（offset 17）基址**都是 1990**；
    //   1.4 比 1.3 多的只是"允许 week = 0xFF 表示该字节是 model year"（同样是 1990 基址）。
    //   1.0 也按 1990 口径解释（业界一致做法）。所以下面按版本号逐个分支写明。
    o->mfg_week = d[16];
    {
        uint16_t ybase;
        if (o->version_major == 1 && o->version_minor >= 4)      ybase = 1990;  // 1.4：1990 + v（model year 同）
        else if (o->version_major == 1 && o->version_minor >= 1) ybase = 1990;  // 1.1~1.3：1990 + v
        else                                                     ybase = 1990;  // 1.0 / 未写：同样 1990 口径
        o->mfg_year = (uint16_t)(ybase + d[17]);
    }

    // 屏幕尺寸 cm：basic display parameters（offset 21/22）。
    // 注意：tag 0xFD 描述符里**不是尺寸**，是刷新率范围（见下）。
    o->size_cm_w = d[21];
    o->size_cm_h = d[22];

    // 扩展块计数：> 0 时本模块**只记数字、不解析扩展块**（只解析第一块 128B）。
    o->ext_count = d[126];

    // 4 个 18 字节描述符：第一个"详细时序"= 首选时序；带 tag 的显示器描述符里挑名字 / 范围
    for (uint32_t k = 0; k < 4; k++) {
        const uint8_t* dd = d + EDID64_DESC_OFF + k * EDID64_DESC_SIZE;
        if (dd[0] == 0x00 && dd[1] == 0x00) {
            uint8_t tag = dd[3];   // 显示器描述符：0-1 = 0，2 = 0，3 = tag，5..17 = 数据
            if (tag == 0xFC) {
                char nm[EDID64_NAME_MAX];
                e64_parse_text(dd, nm, (int)sizeof(nm));
                if (nm[0]) e64_strcpy(o->name, nm, EDID64_NAME_MAX);   // 空文本 -> 保留 "unknown"
            } else if (tag == 0xFD) {
                o->range_v_min = dd[5];       // 最小垂直频率 Hz
                o->range_v_max = dd[6];       // 最大垂直频率 Hz
                o->range_h_min = dd[7];       // 最小水平频率 kHz
                o->range_h_max = dd[8];       // 最大水平频率 kHz
                o->range_pclk_max10 = dd[9];  // 最大像素时钟 / 10 MHz
            }
            // 0xFF（序列号串）/ 0xFE（数据串）/ 0x10（dummy）等：本模块不用，忽略
        } else if (o->pclk_khz == 0) {
            e64_parse_dtd(dd, o);             // EDID 规定第一个 DTD = 首选时序
        }
    }
}

// 校验 + 解析。返回 1 = 可用（valid = true）；0 = 头部/校验和错，或没有首选时序。
// quiet = 1 时不打 "bad header/checksum"（自检的**故意坏样本**用例，那行日志留给真数据）。
static int e64_verify_and_parse(const uint8_t* d, const uint8_t* b1, Edid64* o, int quiet) {
    int hdr = e64_hdr_ok(d);
    int sum = e64_sum_ok(d);
    if (!hdr || !sum) {
        if (!quiet) {
            dbg64_str("[EDID64] bad header/checksum");
            dbg64_nl();
            dbg64_str("[EDID64] verify header=");
            dbg64_dec((uint64_t)hdr);
            dbg64_str(" checksum=");
            dbg64_dec((uint64_t)sum);
            dbg64_nl();
        }
        return 0;
    }
    e64_parse_block(d, o);
    if (o->pclk_khz == 0) {   // 连首选时序都没有：这块 EDID 不完整，如实标无效
        if (!quiet) {
            dbg64_str("[EDID64] no detailed timing descriptor (block 0)");
            dbg64_nl();
        }
        return 0;
    }
    if (!quiet) e64_parse_modes(d, b1, o);   // ★ v0.9.22/23：真实模式列表（首选在前 + 块 1）。
                                      //   quiet = 1（自检的合成样本）时**绝不**碰全局表 ——
                                      //   否则 boot 期 selftest 会把真实 EDID 的模式列表冲掉。
    o->valid = true;
    return 1;
}

// ==================== 对外：初始化 ====================
void edid64_init64() {
    e64_reset(&g_edid);
    g_mode_n = 0;
    g_have_raw = false;
    g_inited   = true;
    g_fw_flag  = false;

    const BootInfo* bi = (const BootInfo*)(uintptr_t)BOOT_INFO_ADDR;
    uint32_t addr = EDID64_FALLBACK_ADDR;
    uint16_t size = (uint16_t)EDID64_BLOCK_BYTES;
    if (bi->magic == BOOT_INFO_MAGIC) {
        if (bi->edid_addr) addr = bi->edid_addr;
        if (bi->edid_size >= EDID64_BLOCK_BYTES) size = bi->edid_size;
        g_fw_flag = (bi->edid_ok != 0);
    }

    if (!g_fw_flag) {
        // 固件没给 EDID（引导层 edid_ok = 0）：如实降级，一个字都不猜
        dbg64_str("[EDID64] present=0 (no EDID from firmware)");
        dbg64_nl();
        return;
    }

    const uint8_t* d = (const uint8_t*)(uintptr_t)addr;
    // ★ v0.9.23：块 1 只在**引导层写了 256 且校验和通过**时才用（引导层两条路径都会先验校验和，
    //   这里再独立验一遍：不信任"上层说它好"）。
    uint32_t raw_len = EDID64_BLOCK_BYTES;
    if ((uint32_t)size >= EDID64_RAW_BYTES && e64_sum_ok(d + EDID64_BLOCK_BYTES))
        raw_len = EDID64_RAW_BYTES;
    Edid64 tmp;
    e64_reset(&tmp);
    if (!e64_verify_and_parse(d, (raw_len == EDID64_RAW_BYTES) ? (d + EDID64_BLOCK_BYTES) : nullptr, &tmp, 0)) {
        dbg64_str("[EDID64] present=0 (EDID from firmware unusable)");
        dbg64_nl();
        return;   // g_edid 保持 valid = false；selftest 的 bit 9 会如实报出来
    }

    for (uint32_t i = 0; i < raw_len; i++) g_raw[i] = d[i];
    for (uint32_t i = raw_len; i < EDID64_RAW_BYTES; i++) g_raw[i] = 0;
    g_raw_len = raw_len;
    g_have_raw = true;
    tmp.present = true;
    e64_strcpy(tmp.source, "vbios", EDID64_SRC_MAX);
    g_edid = tmp;   // POD 整体赋值（无堆、无拷贝构造）

    // ---- 固定格式日志（自动验收 grep 这两行）----
    dbg64_str("[EDID64] present=1 ver=");
    dbg64_dec((uint64_t)tmp.version_major);
    dbg64_str(".");
    dbg64_dec((uint64_t)tmp.version_minor);
    dbg64_str(" mfg=");
    dbg64_str(tmp.mfg);
    dbg64_str(" name=");
    dbg64_str(tmp.name);
    if (tmp.size_cm_w && tmp.size_cm_h) {
        dbg64_str(" size=");
        dbg64_dec((uint64_t)tmp.size_cm_w);
        dbg64_str("x");
        dbg64_dec((uint64_t)tmp.size_cm_h);
        dbg64_str("cm");
    } else {
        dbg64_str(" size=unknown");
    }
    dbg64_nl();

    dbg64_str("[EDID64] preferred pclk=");
    dbg64_dec((uint64_t)tmp.pclk_khz);
    dbg64_str(" Vactive=");
    dbg64_dec((uint64_t)tmp.v_active);
    dbg64_str(" Hactive=");
    dbg64_dec((uint64_t)tmp.h_active);
    dbg64_str(" refresh=");
    e64_print_refresh(tmp.refresh_x10);
    dbg64_str("Hz");
    dbg64_nl();

    // ---- 诊断行（人工/自动验收用：算术与位域各自可核）----
    dbg64_str("[EDID64] detail prod=");
    dbg64_hex64((uint64_t)tmp.product_code);
    dbg64_str(" serial=");
    dbg64_dec((uint64_t)tmp.product_serial);
    dbg64_str(" week=");
    dbg64_dec((uint64_t)tmp.mfg_week);
    dbg64_str(" year=");
    dbg64_dec((uint64_t)tmp.mfg_year);
    dbg64_str(" ext=");
    dbg64_dec((uint64_t)tmp.ext_count);
    dbg64_str(" htotal=");
    dbg64_dec((uint64_t)((uint32_t)tmp.h_active + tmp.h_blank));
    dbg64_str(" vtotal=");
    dbg64_dec((uint64_t)((uint32_t)tmp.v_active + tmp.v_blank));
    dbg64_str(" hfront=");
    dbg64_dec((uint64_t)tmp.h_front);
    dbg64_str(" hsync=");
    dbg64_dec((uint64_t)tmp.h_sync);
    dbg64_str(" vfront=");
    dbg64_dec((uint64_t)tmp.v_front);
    dbg64_str(" vsync=");
    dbg64_dec((uint64_t)tmp.v_sync);
    dbg64_str(" range_v=");
    dbg64_dec((uint64_t)tmp.range_v_min);
    dbg64_str("..");
    dbg64_dec((uint64_t)tmp.range_v_max);
    dbg64_str(" range_h=");
    dbg64_dec((uint64_t)tmp.range_h_min);
    dbg64_str("..");
    dbg64_dec((uint64_t)tmp.range_h_max);
    dbg64_nl();

    // ---- ★ v0.9.22：[EDID] 汇总行（控制台直接可读：厂商 + 型号 + 产品码 + 物理尺寸 + 首选 + 模式数）----
    dbg64_str("[EDID] mfg=");
    dbg64_str(tmp.mfg);
    dbg64_str(" name=");
    dbg64_str(tmp.name);
    dbg64_str(" product=");
    dbg64_dec((uint64_t)tmp.product_code);
    dbg64_str(" serial=");
    dbg64_dec((uint64_t)tmp.product_serial);
    dbg64_str(" year=");
    dbg64_dec((uint64_t)tmp.mfg_year);
    dbg64_str(" cm=");
    if (tmp.size_cm_w && tmp.size_cm_h) {
        dbg64_dec((uint64_t)tmp.size_cm_w); dbg64_str("x"); dbg64_dec((uint64_t)tmp.size_cm_h);
    } else {
        dbg64_str("unknown");
    }
    dbg64_str(" preferred=");
    dbg64_dec((uint64_t)tmp.h_active);
    dbg64_str("x");
    dbg64_dec((uint64_t)tmp.v_active);
    dbg64_str("@");
    e64_print_refresh(tmp.refresh_x10);
    dbg64_str("Hz modes=");
    dbg64_dec((uint64_t)g_mode_n);
    dbg64_str(" src=");
    dbg64_str(tmp.source);
    dbg64_nl();

    // ---- ★ v0.9.23：块 1（CEA-861 扩展块）结论 ----
    //   ext  = EDID 自己报的扩展块个数（d[126]）
    //   cea  = 1 表示块 1 头字节是 0x02 且已被解析
    //   vic  = 采纳进模式表的 VIC 条数 / dtd = 扩展块里的详细时序条数
    //   skip = VIC 表里没有、如实跳过的条数 / blocks = 实读块数（1 或 2）
    dbg64_str("[EDID] ext=");
    dbg64_dec((uint64_t)tmp.ext_count);
    dbg64_str(" cea=");
    dbg64_dec((uint64_t)tmp.cea_ok);
    dbg64_str(" vic=");
    dbg64_dec((uint64_t)tmp.vic_used);
    dbg64_str(" dtd=");
    dbg64_dec((uint64_t)tmp.ext_dtd_n);
    dbg64_str(" hdmi=");
    dbg64_dec((uint64_t)tmp.hdmi);
    dbg64_str(" skip=");
    dbg64_dec((uint64_t)tmp.vic_skip);
    dbg64_str(" blocks=");
    dbg64_dec((uint64_t)(g_raw_len / EDID64_BLOCK_BYTES));
    // b1hdr = 固件实际写在"块 1 缓冲"（addr+128）首字节的内容：0x02 = 真给了 CEA-861 块 1；
    // 0x00 = 固件没给（或者像 QEMU 的 SeaBIOS vgabios 那样无视 CX 又把块 0 写了一遍，
    // 那样 addr+128 会变成 00 FF = 块 0 的头）。这是**实测证据**，不是推测。
    dbg64_str(" b1hdr=");
    dbg64_hex64((uint64_t)((const uint8_t*)(uintptr_t)addr)[EDID64_BLOCK_BYTES]);
    dbg64_nl();

    // 逐条模式（有上限：模式表本身 <= EDID64_MODE_MAX，防刷屏）
    for (int i = 0; i < g_mode_n; i++) {
        dbg64_str("[EDID] mode i=");
        dbg64_dec((uint64_t)i);
        dbg64_str(" ");
        dbg64_dec((uint64_t)g_modes[i].w);
        dbg64_str("x");
        dbg64_dec((uint64_t)g_modes[i].h);
        dbg64_str("@");
        e64_print_refresh(g_modes[i].refresh_x10);
        dbg64_str("Hz kind=");
        dbg64_dec((uint64_t)g_modes[i].kind);
        dbg64_str(" ilace=");
        dbg64_dec((uint64_t)g_modes[i].interlaced);
        dbg64_nl();
    }
}

// ==================== 对外：模式列表 ====================
int edid64_mode_count64() {
    return g_edid.valid ? g_mode_n : 0;
}
const Edid64Mode* edid64_mode_at64(int i) {
    if (i < 0 || i >= g_mode_n) return nullptr;
    return &g_modes[i];
}
// ★ v0.9.23：原始字节（块 0，块 1 校验通过时含块 1）。没有有效 EDID 时返回 nullptr。
const uint8_t* edid64_raw64() { return g_have_raw ? g_raw : nullptr; }
int edid64_raw_len64() { return g_have_raw ? (int)g_raw_len : 0; }

// ==================== 对外：getter ====================
const Edid64* edid64_get() {
    return &g_edid;
}

int edid64_refresh_str64(char* out, int cap) {
    if (!out || cap <= 0) return 0;
    if (!g_edid.valid || g_edid.refresh_x10 == 0) {
        e64_strcpy(out, "unknown", cap);
        return 0;
    }
    char tmp[20];
    int n = 0;
    uint32_t whole = g_edid.refresh_x10 / 10;
    uint32_t frac  = g_edid.refresh_x10 % 10;
    char rev[12];
    int rn = 0;
    if (whole == 0) rev[rn++] = '0';
    while (whole > 0 && rn < 11) { rev[rn++] = (char)('0' + (int)(whole % 10)); whole /= 10; }
    while (rn > 0) tmp[n++] = rev[--rn];
    tmp[n++] = '.';
    tmp[n++] = (char)('0' + (int)frac);
    tmp[n] = 0;
    e64_strcpy(out, tmp, cap);
    return 1;
}

// ==================== 对外：自检（内置合成样本，离线自证解析逻辑）====================
// 合成样本：EDID 1.4 / 厂商 "VMT" / 产品码 0x1234 / 序列号 0x12345678 /
//           2021 年第 10 周 / 名字 "VMTU-SYNTH" / 52x33cm /
//           首选时序 1920x1080@60.0Hz（pclk 148500 kHz，Htotal 2200，Vtotal 1125）/
//           刷新率范围 50..75Hz、30..83kHz / 扩展块计数 = 1（不解析扩展块）
// 期望值就是上面这些常量（下面逐项核对）；坏样本 = 篡改校验和 / 篡改头部各一份。
static void e64_make_sample(uint8_t* d) {
    for (uint32_t i = 0; i < EDID64_BLOCK_BYTES; i++) d[i] = 0x00;
    d[0] = 0x00;                                       // 头部 00 FF FF FF FF FF FF 00
    for (int i = 1; i <= 6; i++) d[i] = 0xFF;
    d[7] = 0x00;
    uint32_t mfg = ((uint32_t)22 << 10) | ((uint32_t)13 << 5) | (uint32_t)20;   // V=22 M=13 T=20
    d[8] = (uint8_t)(mfg >> 8);
    d[9] = (uint8_t)(mfg & 0xFF);
    d[10] = 0x34; d[11] = 0x12;                        // 产品码 0x1234（小端）
    d[12] = 0x78; d[13] = 0x56; d[14] = 0x34; d[15] = 0x12;   // 序列号 0x12345678
    d[16] = 10; d[17] = 31;                            // 第 10 周 / 1990+31 = 2021
    d[18] = 1; d[19] = 4;                              // EDID 1.4
    d[20] = 0x80;                                      // 数字输入
    d[21] = 52; d[22] = 33;                            // 52 x 33 cm
    d[23] = 0x78;                                      // gamma 2.2
    d[24] = 0x0A;                                      // features

    uint8_t* t = d + EDID64_DESC_OFF;                  // 描述符 0：首选详细时序
    t[0] = 0x02; t[1] = 0x3A;                          // 14850 * 10 kHz = 148500 kHz
    t[2] = (uint8_t)(1920 & 0xFF);
    t[3] = (uint8_t)(280 & 0xFF);                      // Htotal 2200 - 1920
    t[4] = (uint8_t)((((1920 >> 8) & 0x0F) << 4) | ((280 >> 8) & 0x0F));
    t[5] = (uint8_t)(1080 & 0xFF);
    t[6] = (uint8_t)(45 & 0xFF);                       // Vtotal 1125 - 1080
    t[7] = (uint8_t)((((1080 >> 8) & 0x0F) << 4) | ((45 >> 8) & 0x0F));
    t[8] = 88;                                         // 水平前肩 88
    t[9] = 44;                                         // 水平同步宽度 44
    t[10] = (uint8_t)((3 << 4) | 5);                   // 垂直前肩 3 / 垂直同步宽度 5
    // 垂直前肩/同步宽度是 4 + 2 位口径：低 4 位在 t[10] 的高低半字节，高 2 位在 t[11] 的 bit3:2 / bit1:0。
    // （这里 3 与 5 都能装进 4 位，所以 t[11] 的垂直位全 0 —— 曾经写成 ">> 2" 导致自检 bit 5 报错，
    //   自检就是为这类位域错误留的。）
    t[11] = (uint8_t)((((88 >> 8) & 0x03) << 6) | (((44 >> 8) & 0x03) << 4) |
                      (((3 >> 4) & 0x03) << 2) | ((5 >> 4) & 0x03));
    t[17] = 0x1E;                                      // flags：逐行、数字分离同步

    uint8_t* n = d + EDID64_DESC_OFF + EDID64_DESC_SIZE;    // 描述符 1：显示器名字
    n[3] = 0xFC;
    for (int i = 5; i < 18; i++) n[i] = ' ';
    const char* nm = "VMTU-SYNTH";
    for (int i = 0; i < 13 && nm[i]; i++) n[5 + i] = (uint8_t)nm[i];

    uint8_t* r = d + EDID64_DESC_OFF + 2 * EDID64_DESC_SIZE;   // 描述符 2：刷新率范围
    r[3] = 0xFD;
    r[5] = 50; r[6] = 75; r[7] = 30; r[8] = 83; r[9] = 17;

    uint8_t* u = d + EDID64_DESC_OFF + 3 * EDID64_DESC_SIZE;   // 描述符 3：dummy（必须被忽略）
    u[3] = 0x10;

    d[126] = 1;                                        // 扩展块计数 = 1（块 1 = 下面的合成 CEA-861）

    uint32_t s = 0;
    for (uint32_t i = 0; i < EDID64_BLOCK_BYTES - 1; i++) s += d[i];
    d[127] = (uint8_t)((0x100u - (s & 0xFFu)) & 0xFFu);   // 校验和：和 == 0 mod 256
}

// ★ v0.9.23：合成的 CEA-861 扩展块（128B）。用来离线自证块 1 的解析：
//   [0]=0x02 / [2]=13（DTD 偏移）/ data block: tag2 len2 {VIC 4, 16} + tag3 len5 {HDMI OUI}
//   扩展 DTD（偏移 13）= 1920x1080@60 **隔行**（flags bit7 = 1）。
static void e64_make_cea(uint8_t* b) {
    for (uint32_t i = 0; i < EDID64_BLOCK_BYTES; i++) b[i] = 0x00;
    b[0] = 0x02;                                   // CEA-861 扩展块标识
    b[1] = 3;                                      // revision 3
    b[2] = 13;                                     // DTD offset = 4 + 9（两个 data block 共 9 字节）
    b[3] = 0x00;
    b[4] = 0x42; b[5] = 4;  b[6] = 16;             // Video Data Block：VIC 4（720p60）/ 16（1080p60）
    b[7] = 0x65;                                   // Vendor Specific，len 5
    b[8] = 0x03; b[9] = 0x0C; b[10] = 0x00;        // OUI = 0x000C03（HDMI Licensing）
    b[11] = 0x00; b[12] = 0x00;
    uint8_t* t = b + 13;                           // 扩展 DTD：1920x1080@60 隔行
    t[0] = 0x25; t[1] = 0x1D;                      // 7425 * 10 kHz = 74.25 MHz
    t[2] = (uint8_t)(1920 & 0xFF); t[3] = (uint8_t)(280 & 0xFF);
    t[4] = (uint8_t)((((1920 >> 8) & 0x0F) << 4) | ((280 >> 8) & 0x0F));
    t[5] = (uint8_t)(1080 & 0xFF); t[6] = (uint8_t)(45 & 0xFF);
    t[7] = (uint8_t)((((1080 >> 8) & 0x0F) << 4) | ((45 >> 8) & 0x0F));
    t[8] = 88; t[9] = 44; t[10] = (uint8_t)((3 << 4) | 5); t[11] = 0;
    t[17] = 0x9E;                                  // bit7 = 隔行（+ 数字分离同步，与块 0 样本同口径）
    uint32_t s = 0;
    for (uint32_t i = 0; i < EDID64_BLOCK_BYTES - 1; i++) s += b[i];
    b[127] = (uint8_t)((0x100u - (s & 0xFFu)) & 0xFFu);
}

int edid64_selftest64() {
    int mask = 0;
    if (!g_inited) edid64_init64();   // 单独调用也安全（与 hwinfo64 的自检同一约定）

    uint8_t syn[EDID64_BLOCK_BYTES], bad_sum[EDID64_BLOCK_BYTES], bad_hdr[EDID64_BLOCK_BYTES];
    e64_make_sample(syn);
    for (uint32_t i = 0; i < EDID64_BLOCK_BYTES; i++) { bad_sum[i] = syn[i]; bad_hdr[i] = syn[i]; }
    bad_sum[127] ^= 0xFF;   // 校验和故意写坏
    bad_hdr[3] = 0x00;      // 头部里的 0xFF 破坏一个

    // ★ v0.9.22：模式列表的自证（保存 / 恢复全局表 —— 自检只验证逻辑，绝不改真实结果）
    {
        Edid64Mode sv[EDID64_MODE_MAX];
        const int svn = g_mode_n;
        for (int i = 0; i < svn; i++) sv[i] = g_modes[i];
        uint8_t ext[EDID64_BLOCK_BYTES];
        e64_make_cea(ext);
        Edid64 ce;
        e64_reset(&ce);
        e64_parse_modes(syn, ext, &ce);
        if (g_mode_n < 1) mask |= 1 << 10;              // 模式列表一条都没解析出来
        // bit 11：块 1（CEA-861）解析 —— cea 标识 / HDMI OUI / 两个 VIC / 一条扩展 DTD / VIC 4 = 720p
        if (ce.cea_ok != 1 || ce.hdmi != 1 || ce.vic_used != 2 || ce.ext_dtd_n != 1) {
            mask |= 1 << 11;
        } else {
            int have720 = 0;
            for (int i = 0; i < g_mode_n; i++)
                if (g_modes[i].w == 1280u && g_modes[i].h == 720u && g_modes[i].kind == 3u) have720 = 1;
            if (!have720) mask |= 1 << 11;
        }
        // bit 12：隔行位 —— 1920x1080i（扩展 DTD）与 1920x1080p（块 0 首选）必须各自留一条
        {
            int have_p = 0, have_i = 0;
            for (int i = 0; i < g_mode_n; i++) {
                if (g_modes[i].w != 1920u || g_modes[i].h != 1080u) continue;
                if (g_modes[i].interlaced) have_i = 1; else have_p = 1;
            }
            if (!have_p || !have_i) mask |= 1 << 12;
        }
        g_st_cea = ce.cea_ok; g_st_vic = ce.vic_used; g_st_dtd = ce.ext_dtd_n; g_st_hdmi = ce.hdmi;
        g_st_il = 0;
        for (int i = 0; i < g_mode_n; i++) if (g_modes[i].interlaced) g_st_il = 1;
        for (int i = 0; i < svn; i++) g_modes[i] = sv[i];
        g_mode_n = svn;
    }

    Edid64 t;
    e64_reset(&t);
    if (!e64_verify_and_parse(syn, nullptr, &t, 1)) {
        mask |= 1 << 0;
    } else {
        if (e64_strcmp(t.mfg, "VMT") != 0) mask |= 1 << 1;
        if (t.product_code != 0x1234 || t.product_serial != 0x12345678u) mask |= 1 << 2;
        if (t.version_major != 1 || t.version_minor != 4 || t.mfg_week != 10 || t.mfg_year != 2021) mask |= 1 << 3;
        if (e64_strcmp(t.name, "VMTU-SYNTH") != 0) mask |= 1 << 4;
        if (t.pclk_khz != 148500 || t.h_active != 1920 || t.v_active != 1080 ||
            t.h_blank != 280 || t.v_blank != 45 ||
            t.h_front != 88 || t.h_sync != 44 || t.v_front != 3 || t.v_sync != 5 ||
            t.refresh_x10 != 600) mask |= 1 << 5;
        if (t.size_cm_w != 52 || t.size_cm_h != 33 ||
            t.range_v_min != 50 || t.range_v_max != 75 ||
            t.range_h_min != 30 || t.range_h_max != 83 ||
            t.range_pclk_max10 != 17 || t.ext_count != 1) mask |= 1 << 6;
    }

    Edid64 b1;
    e64_reset(&b1);
    if (e64_verify_and_parse(bad_sum, nullptr, &b1, 1)) mask |= 1 << 7;   // 坏校验和必须被拒
    Edid64 b2;
    e64_reset(&b2);
    if (e64_verify_and_parse(bad_hdr, nullptr, &b2, 1)) mask |= 1 << 8;   // 坏头部必须被拒

    if (g_fw_flag && !g_edid.valid) mask |= 1 << 9;              // 固件给了 EDID 但我们验证不过

    if (g_edid.valid && g_have_raw) {
        // 独立复核：用"简单偏移读法"（引导层 read_edid 的老口径：d[56]/d[58] 取 hact，
        // d[59]/d[61] 取 vact）复算一次，和上面按 DTD 位域解析的结果必须一致。
        uint16_t w = (uint16_t)(g_raw[56] | ((uint16_t)(g_raw[58] >> 4) << 8));
        uint16_t h = (uint16_t)(g_raw[59] | ((uint16_t)(g_raw[61] >> 4) << 8));
        if (w != g_edid.h_active || h != g_edid.v_active) mask |= 1 << 10;
    }
    // 没有 EDID（present = 0）**不算失败**：解析逻辑由上面的合成样本自证。

    // 合成块 1（CEA-861）的解析结论 —— 块 1 解析器是否真的工作，看这行
    dbg64_str("[EDID64] selftest ext synthetic cea=");
    dbg64_dec((uint64_t)g_st_cea);
    dbg64_str(" vic=");
    dbg64_dec((uint64_t)g_st_vic);
    dbg64_str(" dtd=");
    dbg64_dec((uint64_t)g_st_dtd);
    dbg64_str(" hdmi=");
    dbg64_dec((uint64_t)g_st_hdmi);
    dbg64_str(" ilace=");
    dbg64_dec((uint64_t)g_st_il);
    dbg64_nl();
    if (mask == 0) {
        dbg64_str("[EDID64] selftest PASS");
        dbg64_nl();
    } else {
        dbg64_str("[EDID64] selftest FAIL mask=");
        dbg64_dec((uint64_t)mask);
        dbg64_nl();
    }
    return mask;
}
