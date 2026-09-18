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
#include "edid64.h"
#include "../bootinfo.h"
#include "debug64.h"

// ---- 引导层固定槽（kernel/memlayout64.h 的内存布局注释：0x7600 EDID 缓冲 128B）----
// memlayout64.h 只给了 RSDP 的 0x7800 常量，EDID 地址没有常量，这里写一份回退值：
// 正常路径用 BootInfo.edid_addr（引导层填的），只有 BootInfo 本身不可用（magic 不对）时才回退。
static const uint32_t EDID64_FALLBACK_ADDR = 0x7600;
static const uint32_t EDID64_BLOCK_BYTES   = 128;
static const uint32_t EDID64_DESC_OFF      = 54;    // 4 个 18 字节描述符的起点
static const uint32_t EDID64_DESC_SIZE     = 18;

static Edid64  g_edid;          // 解析结果（静态存储；valid = false 表示"不知道"）
static uint8_t g_raw[EDID64_BLOCK_BYTES];  // 第一块原始字节（仅有效时保留，供独立复核）
static bool    g_have_raw = false;
static bool    g_inited   = false;
static bool    g_fw_flag  = false;         // BootInfo.edid_ok（固件是否给了 EDID）

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
static int e64_verify_and_parse(const uint8_t* d, Edid64* o, int quiet) {
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
    o->valid = true;
    return 1;
}

// ==================== 对外：初始化 ====================
void edid64_init64() {
    e64_reset(&g_edid);
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
    (void)size;   // 只读前 128 字节（第一块）；扩展块不解析，见 edid64.h 边界 1)

    if (!g_fw_flag) {
        // 固件没给 EDID（引导层 edid_ok = 0）：如实降级，一个字都不猜
        dbg64_str("[EDID64] present=0 (no EDID from firmware)");
        dbg64_nl();
        return;
    }

    const uint8_t* d = (const uint8_t*)(uintptr_t)addr;
    Edid64 tmp;
    e64_reset(&tmp);
    if (!e64_verify_and_parse(d, &tmp, 0)) {
        dbg64_str("[EDID64] present=0 (EDID from firmware unusable)");
        dbg64_nl();
        return;   // g_edid 保持 valid = false；selftest 的 bit 9 会如实报出来
    }

    for (uint32_t i = 0; i < EDID64_BLOCK_BYTES; i++) g_raw[i] = d[i];
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
}

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

    d[126] = 1;                                        // 扩展块计数 = 1（只记数字，不解析）

    uint32_t s = 0;
    for (uint32_t i = 0; i < EDID64_BLOCK_BYTES - 1; i++) s += d[i];
    d[127] = (uint8_t)((0x100u - (s & 0xFFu)) & 0xFFu);   // 校验和：和 == 0 mod 256
}

int edid64_selftest64() {
    int mask = 0;
    if (!g_inited) edid64_init64();   // 单独调用也安全（与 hwinfo64 的自检同一约定）

    uint8_t syn[EDID64_BLOCK_BYTES], bad_sum[EDID64_BLOCK_BYTES], bad_hdr[EDID64_BLOCK_BYTES];
    e64_make_sample(syn);
    for (uint32_t i = 0; i < EDID64_BLOCK_BYTES; i++) { bad_sum[i] = syn[i]; bad_hdr[i] = syn[i]; }
    bad_sum[127] ^= 0xFF;   // 校验和故意写坏
    bad_hdr[3] = 0x00;      // 头部里的 0xFF 破坏一个

    Edid64 t;
    e64_reset(&t);
    if (!e64_verify_and_parse(syn, &t, 1)) {
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
    if (e64_verify_and_parse(bad_sum, &b1, 1)) mask |= 1 << 7;   // 坏校验和必须被拒
    Edid64 b2;
    e64_reset(&b2);
    if (e64_verify_and_parse(bad_hdr, &b2, 1)) mask |= 1 << 8;   // 坏头部必须被拒

    if (g_fw_flag && !g_edid.valid) mask |= 1 << 9;              // 固件给了 EDID 但我们验证不过

    if (g_edid.valid && g_have_raw) {
        // 独立复核：用"简单偏移读法"（引导层 read_edid 的老口径：d[56]/d[58] 取 hact，
        // d[59]/d[61] 取 vact）复算一次，和上面按 DTD 位域解析的结果必须一致。
        uint16_t w = (uint16_t)(g_raw[56] | ((uint16_t)(g_raw[58] >> 4) << 8));
        uint16_t h = (uint16_t)(g_raw[59] | ((uint16_t)(g_raw[61] >> 4) << 8));
        if (w != g_edid.h_active || h != g_edid.v_active) mask |= 1 << 10;
    }
    // 没有 EDID（present = 0）**不算失败**：解析逻辑由上面的合成样本自证。

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
