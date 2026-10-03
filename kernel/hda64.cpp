// hda64.cpp - Intel HDA（High Definition Audio）声卡驱动（最小可用、全程轮询）
//
// 为什么这么写 / 边界（与 hda64.h 的口径一致）：
//   * **全程轮询**：INTCTL=0 屏蔽控制器所有中断，命令走 CORB/RIRB 后轮询 RIRBSTS.RINTFL，
//     流完成轮询 SDnSTS.BCIS 与 SDnLPIB —— 与 e1000/nvme/ahci 同款，零中断路径耦合。
//   * **单码器**：STATESTS 位图里取编号最小的码器（QEMU/VMware 都只有 1 个）。
//   * **单播放流**：SD0（偏移 0x80），48 kHz / 16-bit / 2ch（stream format 0x0011）。
//   * **DMA 缓冲必须用物理地址**：控制器按 BDL 里的**物理地址**取数据。内核镜像链接在高半区
//     （VA = PA + 0xFFFFFFFF80000000，见 memlayout64.h），所以 CORB/RIRB/BDL/周期缓冲全部
//     来自 page_alloc_64()（页池 [128MB, mem_high) 恒等映射，PA == VA）—— 不把
//     这些结构放进 .bss，省掉一层 VA->PA 换算与页跨越风险。
//   * Get Parameter 的参数号用 Intel HDA spec 1.0a 的定义（任务书里的 "F0004=节点数 /
//     F0005=功能组类型 / F000C=引脚能力" 与 spec 一致；Revision ID 是 0x02 而不是 0x04，
//     这里按 spec 走，码器才会答对）。
//   * 音量/静音：Set Amp Gain/Mute（verb 0x300，输出放大器 = payload bit15 清 0）写完立刻
//     Get Amp Gain（0xB00）回读；回读值（含静音位）进打点与 Hda64Info::amp_rb。
//   * **不假装成功**：没控制器 / 码器不应答 / 连接表走不通 / 格式回读不一致 / 流超时
//     一律打点 + 返回错误；`audio` 命令与 UI 按返回值显示真实状态。
//
// 串口打点（自动验收按行 grep，格式别改；行锁保证不插行）：
//   [HDA64] not found                                  （没控制器：优雅降级，不崩）
//   [HDA64] pci <b>:<d>.<f> bar0=0x... codecs=<n>
//   [HDA64] ctrl gctl=0x1 statests=0x... caps=0x... vmaj=1 vmin=0
//   [HDA64] codec #0 vid=0x8086 did=0x... step=0x...
//   [HDA64] afg nid=0x1 nodes=<n> dacs=<n> pins=<n>
//   [HDA64] dac nid=0x.. pin nid=0x.. path=<n> caps=0x...
//   [HDA64] pin nid=0x.. ctl=0x40 out_en=1 cfg=0x... name=<...>
//   [HDA64] fmt 48000/16/2 verb=0x200 set=0x11 rdback=0x11 ok=1
//   [HDA64] amp cap=0x... steps=<n> offset=<n>
//   [HDA64] volume pct=<p> step=<g>/<n> mute=<m> rb=0x<hex> whence=<why>
//   [HDA64] mute on=<m> rb=0x<hex> bit=<0|1>
//   [HDA64] stream run cbl=<n> entries=<n> tag=1
//   [HDA64] stream done lpib=<n> cbl=<n> bcis=<n> ok=1
//   [HDA64] output sel=<i> name=<...> pin=0x.. ok=1
//   [HDA64] selftest PASS mask=0 lpib=<n> bcis=<n> tone_ms=<n>
//   [HDA64] selftest FAIL mask=<n> stage=<...>
//   [HDA64] selftest skipped (no controller)
//   [HDA64] error stage=<...>
//   [HDA64] unsupported <what>                       （如实拒绝：别的声音路径没做）
#include "hda64.h"
#include "debug64.h"
#include "mem_64.h"      // page_alloc_64 / memset_64 / memcpy_64（页池恒等映射）
#include "port.h"        // PCI 配置口 0xCF8/0xCFC
#include "x86_64.h"      // g_ticks64（250Hz PIT）
//   config64.h    // 启动时把持久化的 ui.sound.volume / ui.sound.src 落到硬件
//   vfs64.h       // ★ 系统音效：按名字从系统卷读 /usr/share/sounds/<name>.wav（见 hda64_play_named64）
#include "config64.h"
#include "vfs64.h"
#include <stdint.h>
#include <stddef.h>

// ==================== 控制器寄存器（只列用到的）====================
enum HdaReg : uint32_t {
    HDA_GCAP      = 0x00,   // 能力（输出/输入流数、44/48k、64 位地址支持）
    HDA_VMIN      = 0x02,   // 版本次号（8 位）
    HDA_VMAJ      = 0x03,   // 版本主号（8 位）
    HDA_GCTL      = 0x08,   // 全局控制（bit0 = CRST）
    HDA_STATESTS  = 0x0E,   // 码器状态变化（每个码器 1 位；写 1 清）
    HDA_INTCTL    = 0x20,   // 中断控制（0 = 全屏蔽）
    HDA_INTSTS    = 0x24,   // 中断状态（写 1 清）
    HDA_CORBLBASE = 0x40,   // CORB 基址低 32
    HDA_CORBUBASE = 0x44,   // CORB 基址高 32
    HDA_CORBWP    = 0x48,   // CORB 写指针（16 位，低 8 位有效）
    HDA_CORBRP    = 0x4A,   // CORB 读指针（bit15 = 复位）
    HDA_CORBCTL   = 0x4C,   // CORB 控制（bit0 CMEIE / bit1 CORBRUN）
    HDA_CORBSTS   = 0x4D,   // CORB 状态（bit0 内存错误；写 1 清）
    HDA_CORBSIZE  = 0x4E,   // CORB 大小（0=2 项 1=16 项 2=256 项）
    HDA_RIRBLBASE = 0x50,   // RIRB 基址低 32
    HDA_RIRBUBASE = 0x54,   // RIRB 基址高 32
    HDA_RIRBWP    = 0x58,   // RIRB 写指针（硬件写；写 bit15 = 复位）
    HDA_RINTCNT   = 0x5A,   // 每 N 个响应产生一次中断
    HDA_RIRBCTL   = 0x5C,   // RIRB 控制（bit0 RINTCTL / bit1 RIRBDMAEN）
    HDA_RIRBSTS   = 0x5D,   // RIRB 状态（bit0 RINTFL；写 1 清）
    HDA_RIRBSIZE  = 0x5E,   // RIRB 大小（同 CORBSIZE 编码）
    // ★ 输出流描述符 0 = 0x100：spec 的流描述符空间里，0x80/0xA0/0xC0/0xE0 是**前四个**（输入/采集）
    //   流的寄存器，输出（播放）流从 0x100 起（QEMU 寄存器表把它们命名成 IN0..IN3 / OUT4..）。
    //   用 0x80 起的那组就写到采集流上了 —— LPIB 永远不动、码器也不会被通知，这是实测踩过的坑。
    HDA_SD_BASE   = 0x100,  // 输出流描述符（每个 0x20 字节）
    HDA_SD_STRIDE = 0x20,
};

// 流描述符内偏移
enum HdaSd : uint32_t {
    SD_CTL   = 0x00,   // 32 位：bit0 SRST / bit1 RUN / bit2 IOCE / bit4 DEIE / bit20-23 STREAM 号
    SD_STS   = 0x03,   // 8 位：bit2 BCIS（整块完成）bit3 FIFOE bit4 DESE bit5 FIFORDY
    SD_LPIB  = 0x04,   // 32 位：当前 DMA 位置（字节）
    SD_CBL   = 0x08,   // 32 位：循环缓冲长度（字节）
    SD_LVI   = 0x0C,   // 16 位：最后一条有效 BDL 项下标
    SD_FIFOW = 0x0E,   // 16 位：FIFO 水位
    SD_FIFOS = 0x10,   // 16 位：FIFO 大小（只读）
    SD_FMT   = 0x12,   // 16 位：流格式（必须与转换器一致）
    SD_BDPL  = 0x18,   // 32 位：BDL 物理基址低
    SD_BDPU  = 0x1C,   // 32 位：BDL 物理基址高
};

// GCTL / SD_CTL / SD_STS 位
static const uint32_t HDA_GCTL_CRST      = 1u << 0;
static const uint32_t HDA_SD_CTL_SRST     = 1u << 0;
static const uint32_t HDA_SD_CTL_RUN      = 1u << 1;
static const uint32_t HDA_SD_CTL_IOCE     = 1u << 2;
static const uint32_t HDA_SD_CTL_STREAM1  = 1u << 20;   // STREAM 号 = 1（与转换器 0x706 写入一致）
static const uint32_t HDA_SD_CTL_TAG1     = HDA_SD_CTL_STREAM1;   // 播放：DIR(bit19) = 0
static const uint8_t  HDA_SD_STS_BCIS     = 1u << 2;
static const uint8_t  HDA_SD_STS_CLR      = (1u << 2) | (1u << 3) | (1u << 4);
static const uint8_t  HDA_SD_STS_RDY      = 1u << 5;    // FIFORDY

// 12 位 verb（Intel HDA spec 1.0a）
static const uint32_t VERB_GET_PARAM    = 0xF00;   // Get Parameter
static const uint32_t VERB_SET_FMT      = 0x200;   // Set Converter Format
static const uint32_t VERB_GET_FMT      = 0xA00;   // Get Converter Format
static const uint32_t VERB_SET_AMP      = 0x300;   // Set Amp Gain/Mute
static const uint32_t VERB_GET_AMP      = 0xB00;   // Get Amp Gain/Mute
static const uint32_t VERB_SET_CONNSEL  = 0x701;   // Set Connection Select Control
static const uint32_t VERB_SET_STREAM   = 0x706;   // Set Converter Stream/Channel
static const uint32_t VERB_GET_STREAM   = 0xF06;   // Get Converter Stream/Channel
static const uint32_t VERB_SET_PINCTL   = 0x707;   // Set Pin Widget Control
static const uint32_t VERB_GET_PINCTL   = 0xF07;   // Get Pin Widget Control
static const uint32_t VERB_SET_EAPD     = 0x70C;   // Set EAPD/Balanced
static const uint32_t VERB_SET_POWER    = 0x705;   // Set Power State（payload：bit8 = SET，bits3:0 = 状态）
static const uint32_t VERB_GET_CONNLIST = 0xF02;   // Get Connection List Entry

// Get Parameter 参数号
static const uint16_t PARAM_VID_DID   = 0x00;   // Vendor ID / Device ID
static const uint16_t PARAM_REV       = 0x02;   // Revision ID
static const uint16_t PARAM_NODECNT   = 0x04;   // Subordinate Node Count
static const uint16_t PARAM_FGTYPE    = 0x05;   // Function Group Type（低 8 位 == 1 = 音频功能组）
static const uint16_t PARAM_CONNLEN   = 0x07;   // Connection List Length
static const uint16_t PARAM_WIDCAP    = 0x09;   // Audio Widget Capabilities（type = bit23:20）
static const uint16_t PARAM_PINCAP      = 0x0C;   // Pin Capabilities（bit4 = 输出能力 / bit7 HDMI / bit16 EAPD）
static const uint16_t PARAM_AMPOUTCAP   = 0x0E;   // Output Amplifier Capabilities
static const uint16_t PARAM_AMPOVRD_OUT = 0x12;   // Output Amp Capabilities（AMP_OVRD=1 时用它，见 spec）
static const uint16_t PARAM_CFGDEF      = 0x1C;   // Configuration Default（设备类型/连接性）
// Widget 类型（Audio Widget Capabilities 的 bit23:20）
static const uint32_t WID_DAC   = 0x0;   // Audio Output Converter
static const uint32_t WID_MIXER = 0x2;
static const uint32_t WID_SEL   = 0x3;
static const uint32_t WID_PIN   = 0x4;

// 放大器能力字（Intel HDA spec 1.0a §7.3.4.10 `Output/Input Amplifier Capabilities`）：
//   bits[6:0]   = Offset：0 dB 对应的增益索引（输出放大器通常 0；负值以补码给出）
//   bits[14:8]  = Step Size：每步衰减，单位 0.25 dB（输出放大器固定 0.25 dB 粒度）
//   bits[22:16] = Number of Steps：**步数**（增益索引的合法上界）
//   bit31       = Mute Capable（本放大器能不能被静音）
// ★ 修复（本批）：旧代码把 bits[14:8] 当成"步数"、把 bits[6:0] 当成"每步 dB"（两者恰好都在
//   0x80034a4a 里读到 74），于是把合法的最大增益索引写成 0（≈ -num_steps×step_dB = 静音）。
//   Set/Get Amp 负载：bit15 = 0 输出放大器 / 1 输入放大器，bit13 = 左声道、bit12 = 右声道，
//   bit11:8 = 放大器索引，bit7 = 静音，bit6:0 = 增益。
static const uint32_t AMP_MUTE_BIT  = 1u << 7;
static const uint32_t AMP_LEFT_BIT  = 1u << 13;
static const uint32_t AMP_RIGHT_BIT = 1u << 12;
static const uint32_t AMP_DIR_OUT_BIT = 0x0000u;   // Set/Get Amp 的"输出放大器"选择位（spec：0 = 输出放大器）
// 48 kHz / 16-bit / 2ch 的流格式（BASE=0 48k，BITS=1 -> 16bit，CH=1 -> 2ch）
static const uint32_t HDA_FORMAT_48K_16_2 = 0x0011;

// 有界等待参数（250Hz PIT：250 tick = 1s）+ 自旋兜底（IF=0 时 g_ticks64 不前进）
static const uint32_t HDA_CMD_TICKS  = 100;    // 一条码器命令最多等 0.4s
static const uint32_t HDA_CMD_SPIN   = 4000000u;
static const uint32_t HDA_STREAM_TICKS = 500;  // 一个 8KiB 块最多等 2s
static const uint32_t HDA_STREAM_SPIN  = 30000000u;

// 软件环（非 DMA：放 .bss 无所谓）：2048 帧 = 8 KiB（16-bit 立体声）
static const int HDA_RING_FRAMES = 2048;

// ==================== BDL 项 / 驱动状态 ====================
struct __attribute__((packed)) HdaBdl {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t len;
    uint32_t flags;
};

static const char* const HDA_HEXL = "0123456789abcdef";

static Hda64Info      g_i;                        // 对外摘要（只读）
static volatile uint8_t* g_mmio = nullptr;        // BAR0（恒等映射：物理地址即指针）
static uint32_t*      g_corb = nullptr;           // CORB（页池，256 项 × 4B）
static volatile uint64_t* g_rirb = nullptr;       // RIRB（页池，256 项 × 8B）
static HdaBdl*        g_bdl = nullptr;            // BDL（页池，前 2 项有效）
static uint8_t*       g_period[HDA64_PERIODS] = {nullptr, nullptr};  // 每周期 4 KiB（页池）
static void*          g_pages[4] = {nullptr, nullptr, nullptr, nullptr};
static int            g_page_n = 0;
static uint16_t       g_corb_wp = 0;
static int16_t        g_ring[HDA_RING_FRAMES * 2];               // 软件环
static int16_t        g_stage[HDA_RING_FRAMES * 2];              // 环 -> 硬件的暂存（一整块）
static int16_t        g_tone[1024 * 2];                          // 方波合成缓冲
static int            g_ring_rd = 0, g_ring_wr = 0;
static bool           g_rings_ok = false;

// ==================== 小工具 ====================
static inline uint8_t  hwr8 (uint32_t off) { return *(volatile uint8_t *)(g_mmio + off); }
static inline uint16_t hwr16(uint32_t off) { return *(volatile uint16_t*)(g_mmio + off); }
static inline uint32_t hwr32(uint32_t off) { return *(volatile uint32_t*)(g_mmio + off); }
static inline void hww8 (uint32_t off, uint8_t  v) { *(volatile uint8_t *)(g_mmio + off) = v; }
static inline void hww16(uint32_t off, uint16_t v) { *(volatile uint16_t*)(g_mmio + off) = v; }
static inline void hww32(uint32_t off, uint32_t v) { *(volatile uint32_t*)(g_mmio + off) = v; }

static void hda_log_begin() { dbg64_line_begin64(); }
static void hda_log_end()   { dbg64_nl(); dbg64_line_end64(); }

static void hda_hex(uint32_t v) {
    dbg64_str("0x");
    if (v == 0) { dbg64_putc('0'); return; }
    char b[8]; int n = 0;
    while (v && n < 8) { b[n++] = HDA_HEXL[v & 0xFu]; v >>= 4; }
    while (n > 0) dbg64_putc(b[--n]);
}
static void hda_hex64(uint64_t v) {
    dbg64_str("0x");
    if (v == 0) { dbg64_putc('0'); return; }
    char b[16]; int n = 0;
    while (v && n < 16) { b[n++] = HDA_HEXL[v & 0xFu]; v >>= 4; }
    while (n > 0) dbg64_putc(b[--n]);
}
static void hda_dec(int64_t v) {
    if (v < 0) { dbg64_putc('-'); v = -v; }
    dbg64_dec((uint64_t)v);
}

static void hda_err(const char* stage) {
    g_i.last_err = stage ? stage : "?";
    hda_log_begin();
    dbg64_str("[HDA64] error stage=");
    dbg64_str(g_i.last_err);
    hda_log_end();
}
static void hda_unsupported(const char* what) {
    g_i.last_err = "unsupported";
    hda_log_begin();
    dbg64_str("[HDA64] unsupported ");
    dbg64_str(what);
    hda_log_end();
}

// 有界等待（16 位寄存器）：g_ticks64 计时 + 自旋上限双保险，绝不挂死。
static bool hda_wait16(uint32_t off, uint16_t mask, uint16_t want, uint32_t ticks, uint32_t spin_max) {
    const uint64_t t0 = g_ticks64;
    uint32_t spin = 0;
    for (;;) {
        if ((hwr16(off) & mask) == want) return true;
        if (g_ticks64 - t0 > (uint64_t)ticks) return false;
        if (++spin > spin_max) return false;
        __asm__ volatile("pause");
    }
}
static bool hda_wait8(uint32_t off, uint8_t mask, uint8_t want, uint32_t ticks, uint32_t spin_max) {
    const uint64_t t0 = g_ticks64;
    uint32_t spin = 0;
    for (;;) {
        if ((hwr8(off) & mask) == want) return true;
        if (g_ticks64 - t0 > (uint64_t)ticks) return false;
        if (++spin > spin_max) return false;
        __asm__ volatile("pause");
    }
}

// ==================== PCI 配置空间（0xCF8/0xCFC）====================
static uint32_t hpci_rd32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    outl(0xCF8u, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                 | ((uint32_t)fn << 8) | (uint32_t)(off & 0xFCu));
    return inl(0xCFCu);
}
static void hpci_wr32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val) {
    outl(0xCF8u, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                 | ((uint32_t)fn << 8) | (uint32_t)(off & 0xFCu));
    outl(0xCFCu, val);
}

// 扫 0..255 号总线找 class 0x04 / subclass 0x03（HD Audio）—— 与 e1000/nvme 同款分派。
// 连续 4 条空总线提前停（QEMU/VMware 的总线少，能省掉几万次配置周期）。
static bool hda_pci_find() {
    uint32_t empty_run = 0;
    for (uint32_t bus = 0; bus < 256; bus++) {
        bool bus_has = false;
        for (uint32_t d = 0; d < 32; d++) {
            uint32_t id = hpci_rd32((uint8_t)bus, (uint8_t)d, 0, 0x00);
            uint16_t ven = (uint16_t)(id & 0xFFFFu);
            if (ven == 0xFFFFu || ven == 0x0000u) continue;
            bus_has = true;
            const uint32_t hdr = hpci_rd32((uint8_t)bus, (uint8_t)d, 0, 0x0C);
            const uint32_t nfn = (hdr & 0x00800000u) ? 8u : 1u;
            for (uint32_t fn = 0; fn < nfn; fn++) {
                if (fn) {
                    id = hpci_rd32((uint8_t)bus, (uint8_t)d, (uint8_t)fn, 0x00);
                    ven = (uint16_t)(id & 0xFFFFu);
                    if (ven == 0xFFFFu || ven == 0x0000u) continue;
                }
                const uint32_t cc = hpci_rd32((uint8_t)bus, (uint8_t)d, (uint8_t)fn, 0x08);
                if ((uint8_t)(cc >> 24) != 0x04u) continue;             // base class = Multimedia
                if ((uint8_t)(cc >> 16) != 0x03u) continue;             // subclass = HD Audio
                g_i.bus = (uint8_t)bus; g_i.dev = (uint8_t)d; g_i.fn = (uint8_t)fn;
                g_i.found = 1;
                return true;
            }
        }
        if (bus_has) empty_run = 0;
        else if (++empty_run >= 4) break;
    }
    return false;
}

// ==================== CORB/RIRB 命令环 ====================
// 一条命令 = [31:28] 码器地址 / [27:20] 节点 / [19:8] verb / [7:0] 负载（spec 3.4.1）。
static bool hda_cmd(uint32_t nid, uint32_t verb, uint16_t payload, uint32_t* resp) {
    if (!g_rings_ok || !g_corb || !g_i.found) return false;
    const uint32_t cmd = ((uint32_t)g_i.codec << 28) | ((nid & 0xFFu) << 20)
                       | ((verb & 0xFFFu) << 8) | (uint32_t)payload;
    hww8(HDA_RIRBSTS, 1);                          // 清掉可能挂着的上一次响应标志
    const uint16_t wp = (uint16_t)((g_corb_wp + 1) & 0xFFu);
    g_corb[wp] = cmd;
    mb_64();                                       // 保证命令先于写指针可见
    hww16(HDA_CORBWP, wp);
    g_corb_wp = wp;
    const uint64_t t0 = g_ticks64;
    uint32_t spin = 0;
    while (!(hwr8(HDA_RIRBSTS) & 1u)) {            // 轮询 RIRBSTS.RINTFL
        if (g_ticks64 - t0 > (uint64_t)HDA_CMD_TICKS || ++spin > HDA_CMD_SPIN) {
            g_i.cmd_timeouts++;
            if (g_i.last_err == nullptr || g_i.cmd_timeouts <= 4) hda_err("cmd rintfl timeout");
            return false;
        }
        __asm__ volatile("pause");
    }
    const uint16_t rwp = (uint16_t)(hwr16(HDA_RIRBWP) & 0xFFu);   // 硬件刚写入的那一项
    const uint64_t r = g_rirb[rwp];
    hww8(HDA_RIRBSTS, 1);                          // 写 1 清 RINTFL
    g_i.cmds++;
    if (resp) *resp = (uint32_t)r;
    return true;
}
static bool hda_param(uint32_t nid, uint16_t param, uint32_t* out) {
    return hda_cmd(nid, VERB_GET_PARAM, param, out);
}
static uint32_t hda_param_or(uint32_t nid, uint16_t param) {
    uint32_t v = 0xFFFFFFFFu;
    (void)hda_param(nid, param, &v);
    return v;
}

static void* hda_dma_alloc() {
    if (g_page_n >= (int)(sizeof(g_pages) / sizeof(g_pages[0]))) return nullptr;
    void* p = page_alloc_64();
    if (!p) return nullptr;
    memset_64(p, 0, PAGE_SIZE_64);
    g_pages[g_page_n++] = p;
    return p;
}

// 控制器复位 + CORB/RIRB 建环（+ 一页页池：CORB 1KiB + RIRB 2KiB）。
static bool hda_ctrl_reset_rings() {
    hww32(HDA_GCTL, 0);                                       // CRST=0：进入复位
    (void)hda_wait16(HDA_GCTL, (uint16_t)HDA_GCTL_CRST, 0, 50, 1000000);
    hww32(HDA_GCTL, HDA_GCTL_CRST);                           // CRST=1：退出复位
    if (!hda_wait16(HDA_GCTL, (uint16_t)HDA_GCTL_CRST, (uint16_t)HDA_GCTL_CRST, 100, 4000000)) {
        hda_err("gctl crst"); return false;
    }
    hww32(HDA_INTCTL, 0);                                     // 全屏蔽：本驱动纯轮询
    hww32(HDA_INTSTS, 0xFFFFFFFFu);                           // 清挂起的状态
    // ★ STATESTS 不能在这里清：写 1 清会把"刚上报的码器"吃掉（QEMU 在 CRST 写完那一刻
    //   就把位图置好）。顺序按 Intel spec / Linux azx：先复位、再**等** STATESTS 非 0，最后 ack。

    void* page = hda_dma_alloc();
    if (!page) { hda_err("dma page"); return false; }
    g_corb = (uint32_t*)page;                                 // 页内对齐（页本身就是 128B 对齐）
    g_rirb = (volatile uint64_t*)((uint8_t*)page + 0x400);

    hww8(HDA_CORBCTL, 0);
    hww8(HDA_RIRBCTL, 0);
    hww16(HDA_CORBRP, 0x8000);                                // 复位 CORB 读指针（自清）
    (void)hda_wait16(HDA_CORBRP, 0x8000, 0, 40, 1000000);
    hww16(HDA_CORBWP, 0);
    hww16(HDA_RIRBWP, 0x8000);                                // 复位 RIRB 写指针
    hww8(HDA_CORBSIZE, 0x2);                                  // 256 项（1 KiB）
    hww8(HDA_RIRBSIZE, 0x2);                                  // 256 项（2 KiB）
    hww32(HDA_CORBLBASE, (uint32_t)(uintptr_t)g_corb);
    hww32(HDA_CORBUBASE, (uint32_t)((uint64_t)(uintptr_t)g_corb >> 32));
    hww32(HDA_RIRBLBASE, (uint32_t)(uintptr_t)g_rirb);
    hww32(HDA_RIRBUBASE, (uint32_t)((uint64_t)(uintptr_t)g_rirb >> 32));
    hww16(HDA_RINTCNT, 1);                                    // 每个响应都可观察（INTCTL 仍为 0）
    g_corb_wp = 0;
    hww8(HDA_RIRBCTL, 0x3);                                   // RIRBDMAEN | RINTCTL
    hww8(HDA_CORBCTL, 0x2);                                   // CORBRUN
    g_rings_ok = true;
    return true;
}

// ==================== 码器 / 节点枚举 ====================
static uint32_t g_dac[8];          // 输出转换器节点
static uint32_t g_dac_caps[8];
static int      g_dac_n = 0;
struct HdaPin { uint32_t nid, cfg, caps; uint32_t dev; bool digital; };
static HdaPin   g_pin[8];
static int      g_pin_n = 0;

static bool hda_is_out_dac(uint32_t caps) {
    return ((caps >> 20) & 0xFu) == WID_DAC && (caps & 0x4u) != 0;   // bit2 = 有输出放大器
}

// 走连接表找"从 node 能通到 DAC"的路径（最多 maxd 跳）。
// 返回 0 = 不通；否则返回跳数，sel[0..hops-1] = 每一跳要写的连接选择索引，*dac = 找到的 DAC。
static int hda_path_dfs(uint32_t node, uint32_t* dac, uint8_t* sel, int depth, int maxd) {
    const uint32_t len = hda_param_or(node, PARAM_CONNLEN) & 0x7Fu;
    if (!len || len > 64) return 0;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t ent = 0;
        if (!hda_cmd(node, VERB_GET_CONNLIST, (uint16_t)i, &ent)) return 0;
        if (ent & 0x80u) return 0;                              // "范围"写法：不支持，如实放弃
        const uint32_t child = ent & 0xFFu;
        const uint32_t caps = hda_param_or(child, PARAM_WIDCAP);
        if (hda_is_out_dac(caps)) {
            sel[depth] = (uint8_t)i; *dac = child; return depth + 1;
        }
        const uint32_t type = (caps >> 20) & 0xFu;
        if ((type == WID_MIXER || type == WID_SEL) && depth + 1 < maxd) {
            const int h = hda_path_dfs(child, dac, sel, depth + 1, maxd);
            if (h) { sel[depth] = (uint8_t)i; return h + 1; }
        }
    }
    return 0;
}

static const char* hda_dev_name(uint32_t dev) {
    switch (dev) {
        case 0: return "line-out";
        case 1: return "speaker";
        case 2: return "headphones";
        case 3: return "cd";
        case 4: return "spdif";
        case 5: return "digital";
        case 6: return "modem";
        case 7: return "handset";
        default: return "output";
    }
}

static void hda_copy_name(char* dst, int cap, const char* s) {
    int n = 0;
    while (s && s[n] && n < cap - 1) { dst[n] = s[n]; n++; }
    if (cap > 0) dst[n] = 0;
}

// AFG 下枚举：DAC（type0 + 输出放大器）+ 输出 Pin（type4 + Pin Cap bit4 + 连接性不是"没接"）。
static int g_node_logged = 0;   // 节点日志行数上限（防刷屏）

// AFG 下枚举：DAC（type0 + 输出放大器）+ 输出 Pin（type4 + Pin Cap bit4 + 连接性不是"没接"）。
static bool hda_enum_nodes(uint32_t afg) {
    const uint32_t nc = hda_param_or(afg, PARAM_NODECNT);
    uint32_t start = (nc >> 16) & 0xFFu, count = nc & 0xFFu;
    if (count > 32) count = 32;
    g_dac_n = 0; g_pin_n = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t nid = start + i;
        const uint32_t caps = hda_param_or(nid, PARAM_WIDCAP);
        const uint32_t type = (caps >> 20) & 0xFu;
        if (type != WID_DAC && type != WID_MIXER && type != WID_SEL && type != WID_PIN) continue;
        if (g_node_logged < 16) {                       // 每个音频节点一行（通路判定的原始证据；上限防刷屏）
            g_node_logged++;
            hda_log_begin();
            dbg64_str("[HDA64] node nid="); hda_hex(nid);
            dbg64_str(" caps="); hda_hex(caps);
            dbg64_str(" type="); dbg64_dec(type);
            dbg64_str(" conn="); dbg64_dec((uint64_t)(hda_param_or(nid, PARAM_CONNLEN) & 0x7Fu));
            dbg64_str(" amp_out="); hda_hex(hda_param_or(nid, (caps & 0x8u) ? PARAM_AMPOVRD_OUT : PARAM_AMPOUTCAP));
            hda_log_end();
        }
        if (hda_is_out_dac(caps) && g_dac_n < (int)(sizeof(g_dac) / sizeof(g_dac[0]))) {
            g_dac[g_dac_n] = nid; g_dac_caps[g_dac_n] = caps; g_dac_n++;
        } else if (type == WID_PIN) {
            const uint32_t pc = hda_param_or(nid, PARAM_PINCAP);
            if (!(pc & 0x10u)) continue;                        // bit4 = 输出能力
            const uint32_t cfg = hda_param_or(nid, PARAM_CFGDEF);
            const uint32_t conn = (cfg >> 30) & 0x3u;
            if (conn == 1u) continue;                           // 1 = 没有物理连接：不登记
            if (g_pin_n < (int)(sizeof(g_pin) / sizeof(g_pin[0]))) {
                HdaPin& p = g_pin[g_pin_n++];
                p.nid = nid; p.cfg = cfg; p.caps = pc;
                p.dev = (cfg >> 20) & 0xFu;
                p.digital = ((pc & (1u << 7)) != 0) || ((caps & (1u << 9)) != 0);   // HDMI/数字
                hda_log_begin();                        // 候选引脚：连接表长度 = 通路判定的依据
                dbg64_str("[HDA64] pin cand nid="); hda_hex(nid);
                dbg64_str(" pc="); hda_hex(pc);
                dbg64_str(" cfg="); hda_hex(cfg);
                dbg64_str(" dev="); dbg64_dec(p.dev);
                dbg64_str(" conn="); dbg64_dec((uint64_t)(hda_param_or(nid, PARAM_CONNLEN) & 0x7Fu));
                dbg64_str(" digital="); dbg64_dec(p.digital ? 1 : 0);
                hda_log_end();
            }
        }
    }
    return g_dac_n > 0 && g_pin_n > 0;
}

// 选活动通路：优先 扬声器 -> 耳机 -> Line-out 的**模拟**输出（数字/HDMI 本批不播放）。
// 连接表能走通就按读出来的每一跳设连接选择；**读不到连接表**（长度 0 / 范围写法：QEMU 的
// 通用码器就是这种"固定接线"）时退回"隐式直连 DAC->Pin"，但打点会明写 path=implicit，
// 并由自检/录音证据判定它到底有没有出声 —— 绝不把假设写成"读出来的通路"。
static bool g_path_implicit = false;
static int hda_pick_path(uint32_t* dac_out, uint32_t* pin_out, uint8_t* sels, int* hops_out) {
    static const uint32_t prefer[3] = {1, 2, 0};                // speaker / headphones / line-out
    for (int want = 0; want < 3; want++) {
        for (int i = 0; i < g_pin_n; i++) {
            if (g_pin[i].digital || g_pin[i].dev != prefer[want]) continue;
            uint32_t dac = 0; uint8_t sel[4] = {0, 0, 0, 0};
            const int hops = hda_path_dfs(g_pin[i].nid, &dac, sel, 0, 3);
            if (!hops) continue;
            *dac_out = dac; *pin_out = g_pin[i].nid; *hops_out = hops;
            for (int k = 0; k < 4; k++) sels[k] = sel[k];
            return i;
        }
    }
    // 连接表读不到：隐式直连（第一个模拟输出 Pin + 第一个输出 DAC）
    if (g_dac_n > 0) {
        for (int i = 0; i < g_pin_n; i++) {
            if (g_pin[i].digital) continue;
            *dac_out = g_dac[0]; *pin_out = g_pin[i].nid; *hops_out = 0;
            for (int k = 0; k < 4; k++) sels[k] = 0;
            g_path_implicit = true;
            return i;
        }
    }
    return -1;
}

// ==================== 格式 / 通路 / 音量 ====================
static int hda_amp_set(uint32_t nid, int mute, uint32_t gain) {
    // 左+右两个声道都写（bit13 | bit12），输出放大器（AMP_DIR_OUT_BIT）。
    const uint16_t payload = (uint16_t)(AMP_DIR_OUT_BIT | AMP_LEFT_BIT | AMP_RIGHT_BIT
                            | (mute ? AMP_MUTE_BIT : 0u) | (gain & 0x7Fu));
    return hda_cmd(nid, VERB_SET_AMP, payload, nullptr) ? 0 : -1;
}
static int hda_amp_get(uint32_t nid, uint32_t* out) {
    return hda_cmd(nid, VERB_GET_AMP, (uint16_t)(AMP_DIR_OUT_BIT | AMP_LEFT_BIT), out) ? 0 : -1;
}
// 百分比 -> 放大器**合法增益索引**（按 HDA spec 的放大器能力字解码，见上面 AMP 注释）。
//   语义：索引 0 = 最大衰减（≈ -num_steps×step_dB），索引 num_steps = 0 dB（最大音量）。
//   所以 100% 必须落在**最大合法索引**上（旧代码写成 0 = 静音，这就是"系统音效全听不见"的根因）。
//   线性映射：gain = round(num_steps × pct / 100)。
static uint32_t hda_gain_from_pct(int pct, int num_steps) {
    if (num_steps <= 0) return 0;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int g = (num_steps * pct + 50) / 100;               // 四舍五入
    if (g < 0) g = 0;
    if (g > num_steps) g = num_steps;
    return (uint32_t)g;
}
static int hda_pct_from_gain(uint32_t gain, int num_steps) {
    if (num_steps <= 0) return gain == 0 ? 0 : 100;
    if (gain > (uint32_t)num_steps) gain = (uint32_t)num_steps;
    return (int)(gain * 100u / (uint32_t)num_steps);
}
// 增益索引对应的衰减 dB（0.25dB 单位换算成 1/4 dB 的整数；仅用于打点/自检说明）
static int hda_attn_qdb(uint32_t gain, int num_steps, int step_qdb) {
    if (num_steps <= 0 || step_qdb <= 0) return 0;
    if (gain > (uint32_t)num_steps) gain = (uint32_t)num_steps;
    return (num_steps - (int)gain) * step_qdb;
}

// 音量 + 静音一起写，然后回读。whence = 调用来源（"init"/"cmd"/"panel"/"set"/"selftest"）。
static int hda_volume_write(int pct, int mute, const char* whence, bool logline) {
    if (!g_i.ready) return -1;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct == 0) mute = 1;
    const int steps = g_i.amp_gain_max;                    // 0 dB 增益索引（100% -> 这里）
    const uint32_t gain = hda_gain_from_pct(pct, steps);
    // Pin 自己的放大器：只有**真有步进**才写。0 步的 Pin 放大器（QEMU 的 hda-duplex 就是）往往是
    // "只管静音位"的，写 gain=0 反而会被某些实现当成静音 —— 这种放大器交给 DAC 那一路控制。
    if (g_i.pin && g_i.pin_amp_steps > 0) {
        const uint32_t pgain = hda_gain_from_pct(pct, g_i.pin_amp_gain_max);
        (void)hda_amp_set(g_i.pin, mute, pgain);
    }
    if (hda_amp_set(g_i.dac, mute, gain) != 0) { hda_err("amp set"); return -1; }
    uint32_t rb = 0xFFFFFFFFu;
    int rb_ok = (hda_amp_get(g_i.dac, &rb) == 0);
    g_i.amp_rb = rb;
    const int rb_mute = rb_ok ? ((rb & AMP_MUTE_BIT) ? 1 : 0) : -1;
    const uint32_t rb_gain = rb & 0x7Fu;
    const int ok99 = rb_ok && rb_mute >= 0 && (pct == 0 || (rb_gain == gain && (mute == (rb_mute ? 1 : 0))));
    g_i.muted = rb_mute >= 0 ? (rb_mute ? 1 : 0) : mute;
    g_i.volume = ok99 ? pct : (rb_mute == 0 ? hda_pct_from_gain(rb_gain, steps) : 0);
    if (pct > 0 && rb_mute == 0 && rb_gain != gain && logline)
        hda_unsupported("amp gain (readback mismatch)");       // 回读不一致：如实说明，绝不装成功
    if (logline) {
        hda_log_begin();
        dbg64_str("[HDA64] volume pct="); hda_dec(pct);
        dbg64_str(" step="); hda_dec((int64_t)gain); dbg64_putc('/'); hda_dec((int64_t)steps);
        dbg64_str(" mute="); hda_dec(mute);
        dbg64_str(" rb="); hda_hex(rb);
        dbg64_str(" whence="); dbg64_str(whence ? whence : "-");
        dbg64_str(" ok="); hda_dec(ok99 ? 1 : 0);
        // ★ 修复（①）新增：解码出来的"每步 dB / 本次衰减"（放在 ok= 之后，既有 grep 不受影响）
        dbg64_str(" attn_qdb="); hda_dec((int64_t)hda_attn_qdb(gain, steps, g_i.amp_step_qdb));
        dbg64_str(" step_qdb="); hda_dec((int64_t)g_i.amp_step_qdb);
        hda_log_end();
    }
    return g_i.volume;
}

// 把 Pin->...->DAC 的连接选择、格式、流号、Pin 控制、EAPD 全部落到硬件。
static bool hda_configure(uint32_t dac, uint32_t pin, const uint8_t* sels, int hops) {
    // 0) 先把 AFG/DAC/Pin 拉到 D0（spec：Set Power State payload = SET(bit8) | 状态）
    (void)hda_cmd(g_i.afg, VERB_SET_POWER, 0x100, nullptr);
    (void)hda_cmd(dac,     VERB_SET_POWER, 0x100, nullptr);
    (void)hda_cmd(pin,     VERB_SET_POWER, 0x100, nullptr);
    // 1) 沿路径逐跳设连接选择（连接表长度 <= 1 的节点写着也无害）
    uint32_t node = pin;
    for (int i = 0; i < hops; i++) {
        (void)hda_cmd(node, VERB_SET_CONNSEL, sels[i], nullptr);
        uint32_t ent = 0;
        if (!hda_cmd(node, VERB_GET_CONNLIST, sels[i], &ent)) return false;
        node = ent & 0xFFu;
    }
    // 2) 流号（tag 1）+ 格式 48k/16/2 —— 顺序按 spec/Linux：**先流号、后格式**，各自回读
    (void)hda_cmd(dac, VERB_SET_STREAM, 0x0010, nullptr);       // stream tag = 1，channel = 0
    uint32_t conv_rb = 0;
    if (!hda_cmd(dac, VERB_GET_STREAM, 0, &conv_rb)) return false;
    g_i.conv_get = conv_rb & 0xFFu;
    if (g_i.conv_get != 0x10u) { hda_err("stream tag readback"); return false; }
    if (!hda_cmd(dac, VERB_SET_FMT, (uint16_t)g_i.fmt_set, nullptr)) return false;
    uint32_t fmt_rb = 0;
    if (!hda_cmd(dac, VERB_GET_FMT, 0, &fmt_rb)) return false;
    g_i.fmt_get = fmt_rb & 0xFFFFu;
    if (g_i.fmt_get != g_i.fmt_set) {
        hda_err("fmt readback");
        hda_log_begin();
        dbg64_str("[HDA64] fmt 48000/16/2 set="); hda_hex(g_i.fmt_set);
        dbg64_str(" rdback="); hda_hex(g_i.fmt_get);
        dbg64_str(" ok=0 (未实现/不支持：只做 48k/16/2)");
        hda_log_end();
        return false;
    }
    // 3) Pin：连接选择（有的码器要显式选 0）-> OUT_EN（耳机再加 HP_EN）-> 读回 -> EAPD
    (void)hda_cmd(pin, VERB_SET_CONNSEL, 0, nullptr);
    const uint32_t dev = (g_i.pin_cfg >> 20) & 0xFu;
    const uint16_t pctl = (uint16_t)(0x40u | (dev == 2u ? 0x80u : 0x00u));
    if (!hda_cmd(pin, VERB_SET_PINCTL, pctl, nullptr)) return false;
    uint32_t rb = 0;
    if (!hda_cmd(pin, VERB_GET_PINCTL, 0, &rb)) return false;
    g_i.pin_ctl = rb & 0xFFu;
    if ((g_i.pin_ctl & 0x40u) == 0) { hda_err("pin out_en"); return false; }
    if (g_i.pin_caps & (1u << 16)) {                            // bit16 = 支持 EAPD
        (void)hda_cmd(pin, VERB_SET_EAPD, 0x2, nullptr);
        g_i.eapd = 2;
    }
    // 4) 通路就位后**重发**一次流号 + 格式（幂等）：码器要在"引脚已使能"之后再看一次
    //    转换器状态才会把这条流算作 running（QEMU 的通用码器就是这样；真机重写同值无害）。
    (void)hda_cmd(dac, VERB_SET_STREAM, 0x0010, nullptr);
    (void)hda_cmd(dac, VERB_SET_FMT, (uint16_t)g_i.fmt_set, nullptr);
    return true;
}

// ==================== 播放流（SD0 + BDL）====================
// 一个 8 KiB 以内的块：填 BDL -> RUN -> 轮询 BCIS/LPIB -> 停。bytes 必须是 4 的倍数。
static int hda_stream_once(const uint8_t* data, uint32_t bytes) {
    if (!g_i.ready || !g_period[0] || !g_bdl || !bytes || (bytes & 3u)) return -1;
    // 停 + 复位 + 等 FIFO 就绪
    hww32(HDA_SD_BASE + SD_CTL, 0);
    hww32(HDA_SD_BASE + SD_CTL, HDA_SD_CTL_SRST);
    (void)hda_wait16(HDA_SD_BASE + SD_CTL, (uint16_t)HDA_SD_CTL_SRST, (uint16_t)HDA_SD_CTL_SRST, 40, 1000000);
    hww32(HDA_SD_BASE + SD_CTL, 0);
    if (!hda_wait8(HDA_SD_BASE + SD_STS, HDA_SD_STS_RDY, HDA_SD_STS_RDY, 40, 1000000)) {
        hda_err("sd fifordy"); return -2;
    }
    // 填 BDL：**始终拆成 2 条**（每条 <= 4 KiB，IOC 置位）。为什么不能只写 1 条：CBL == 单条长度时
    // 硬件一次就把整环走完，LPIB 立刻回绕到 0，"流位置前进"就观察不到了（真机与 QEMU 都这样）。
    uint32_t len0 = 0, len1 = 0;
    int entries = 0;
    if (bytes >= 8u) {
        len0 = (bytes / 2u) & ~3u;
        if (len0 < 4u) len0 = 4u;
        if (len0 >= bytes) len0 = bytes - 4u;
        len1 = bytes - len0;
        entries = 2;
    } else {
        len0 = bytes;
        entries = 1;
    }
    const uint32_t lens[2] = { len0, len1 };
    uint32_t off = 0;
    for (int e = 0; e < entries; e++) {
        const uint32_t len = lens[e];
        memcpy_64(g_period[e], data + off, len);
        g_bdl[e].addr_lo = (uint32_t)(uintptr_t)g_period[e];
        g_bdl[e].addr_hi = (uint32_t)((uint64_t)(uintptr_t)g_period[e] >> 32);
        g_bdl[e].len = len;
        g_bdl[e].flags = (e == entries - 1) ? 1u : 0u;   // 最后一条：IOC = 完成时置 BCIS
        off += len;
    }
    if (!entries) return -1;
    wmb_64();
    hww32(HDA_SD_BASE + SD_BDPL, (uint32_t)(uintptr_t)g_bdl);
    hww32(HDA_SD_BASE + SD_BDPU, (uint32_t)((uint64_t)(uintptr_t)g_bdl >> 32));
    hww32(HDA_SD_BASE + SD_CBL, bytes);
    hww16(HDA_SD_BASE + SD_LVI, (uint16_t)(entries - 1));
    hww16(HDA_SD_BASE + SD_FMT, (uint16_t)g_i.fmt_set);
    hww32(HDA_SD_BASE + SD_LPIB, 0);
    hww8(HDA_SD_BASE + SD_STS, HDA_SD_STS_CLR);
    // 跑
    hww32(HDA_SD_BASE + SD_CTL, HDA_SD_CTL_TAG1 | HDA_SD_CTL_IOCE | HDA_SD_CTL_RUN);
    g_i.runs++;
    g_i.bytes += bytes;
    g_i.cbl_last = bytes;
    hda_log_begin();
    dbg64_str("[HDA64] stream run cbl="); dbg64_dec(bytes);
    dbg64_str(" entries="); dbg64_dec((uint64_t)entries);
    dbg64_str(" tag=1");
    if (g_i.runs <= 2) {
        // 头两次把控制器侧寄存器回读也打出来（排障/证据；之后不再刷屏）
        dbg64_str(" rb_ctl="); hda_hex(hwr32(HDA_SD_BASE + SD_CTL));
        dbg64_str(" rb_lvi="); dbg64_dec((uint64_t)hwr16(HDA_SD_BASE + SD_LVI));
        dbg64_str(" rb_cbl="); dbg64_dec((uint64_t)hwr32(HDA_SD_BASE + SD_CBL));
        dbg64_str(" rb_fmt="); hda_hex(hwr16(HDA_SD_BASE + SD_FMT));
        dbg64_str(" rb_bdpl="); hda_hex(hwr32(HDA_SD_BASE + SD_BDPL));
    }
    hda_log_end();
    // 等整块完成：BCIS（走完 LVI）或 LPIB 回绕；都有界
    const uint64_t t0 = g_ticks64;
    uint32_t spin = 0, hi = 0, bcis = 0;
    for (;;) {
        const uint8_t sts = hwr8(HDA_SD_BASE + SD_STS);
        if (sts & HDA_SD_STS_BCIS) { hww8(HDA_SD_BASE + SD_STS, HDA_SD_STS_BCIS); bcis = 1; break; }
        const uint32_t lp = hwr32(HDA_SD_BASE + SD_LPIB);
        if (lp > hi) hi = lp;
        if (lp < 64u && hi + 64u >= bytes) break;               // LPIB 回绕 = 也读完了
        if (g_ticks64 - t0 > (uint64_t)HDA_STREAM_TICKS || ++spin > HDA_STREAM_SPIN) {
            hww32(HDA_SD_BASE + SD_CTL, HDA_SD_CTL_TAG1);
            hda_err("stream timeout");
            hda_log_begin();
            dbg64_str("[HDA64] stream done lpib="); dbg64_dec(hi);
            dbg64_str(" cbl="); dbg64_dec(bytes);
            dbg64_str(" bcis=0 ok=0");
            hda_log_end();
            return -2;
        }
        __asm__ volatile("pause");
    }
    hww32(HDA_SD_BASE + SD_CTL, HDA_SD_CTL_TAG1);               // 停（保留流号）
    g_i.bcis += bcis;
    if (hi > g_i.lpib_max) g_i.lpib_max = hi;    // 保留峰值：整段播放里"流位置最大推进到多少"
    hda_log_begin();
    dbg64_str("[HDA64] stream done lpib="); dbg64_dec(hi);
    dbg64_str(" cbl="); dbg64_dec(bytes);
    dbg64_str(" bcis="); dbg64_dec(bcis);
    dbg64_str(" ok="); hda_dec((hi > 0 || bcis) ? 1 : 0);
    hda_log_end();
    return (hi > 0 || bcis) ? 0 : -2;                            // 位置/完成计数都没动 = 如实失败
}

int hda64_play64(const int16_t* pcm, size_t frames) {
    if (!g_i.ready) return -1;
    if (!pcm || !frames) return -1;
    const uint8_t* p = (const uint8_t*)pcm;
    size_t bytes = frames * 4u;
    while (bytes) {
        uint32_t chunk = (uint32_t)(bytes > (size_t)(HDA64_PERIODS * HDA64_PERIOD_BYTES)
                                    ? (HDA64_PERIODS * HDA64_PERIOD_BYTES) : bytes);
        chunk &= ~3u;
        if (!chunk) break;
        const int rc = hda_stream_once(p, chunk);
        if (rc != 0) return rc;
        p += chunk;
        bytes -= chunk;
    }
    return 0;
}

// ==================== 软件环（异步路径）====================
int hda64_queued64() {
    int n = g_ring_wr - g_ring_rd;
    if (n < 0) n += HDA_RING_FRAMES;
    return n;
}
int hda64_queue64(const int16_t* pcm, size_t frames) {
    if (!g_i.ready) return 0;
    if (!pcm) return -1;
    const int used = hda64_queued64();
    int free_frames = HDA_RING_FRAMES - 1 - used;
    if (free_frames <= 0) return 0;
    size_t n = frames;
    if (n > (size_t)free_frames) n = (size_t)free_frames;
    for (size_t i = 0; i < n; i++) {
        g_ring[g_ring_wr * 2]     = pcm[i * 2];
        g_ring[g_ring_wr * 2 + 1] = pcm[i * 2 + 1];
        g_ring_wr = (g_ring_wr + 1) % HDA_RING_FRAMES;
    }
    return (int)n;
}
int hda64_pump64(int max_chunks) {
    if (!g_i.ready) return -1;
    int done = 0;
    while (done < max_chunks) {
        const int used = hda64_queued64();
        if (used <= 0) break;
        int n = used;
        if (n > HDA_RING_FRAMES / 2) n = HDA_RING_FRAMES / 2;   // 一块 <= 1024 帧 = 4 KiB
        for (int i = 0; i < n; i++) {
            g_stage[i * 2]     = g_ring[g_ring_rd * 2];
            g_stage[i * 2 + 1] = g_ring[g_ring_rd * 2 + 1];
            g_ring_rd = (g_ring_rd + 1) % HDA_RING_FRAMES;
        }
        const int rc = hda64_play64(g_stage, (size_t)n);
        if (rc != 0) return rc;
        done++;
    }
    return done;
}

int hda64_tone64(int ms) {
    if (!g_i.ready) return -1;
    if (ms < 10) ms = 10;
    if (ms > 2000) ms = 2000;                                   // 有界：一次最多 2s
    const int total = 48000 * ms / 1000;
    int left = total, phase = 0;
    while (left > 0) {
        int n = left > 1024 ? 1024 : left;
        for (int i = 0; i < n; i++) {
            const int16_t v = (int16_t)(phase < 55 ? 7000 : -7000);   // 109 采样 = 440Hz 方波
            g_tone[i * 2] = g_tone[i * 2 + 1] = v;
            if (++phase >= 109) phase = 0;
        }
        const int rc = hda64_play64(g_tone, (size_t)n);
        if (rc != 0) return rc;
        left -= n;
    }
    return 0;
}

// ==================== ★ 系统音效：按名字播内置四段 ====================
// 素材与 /bin/sounder 同名同源（build64/sounds/*.wav，由 tools/sounds_gen.py 合成，装在
// 系统卷 /usr/share/sounds/）。内核只做"读文件 + 剥 WAV 头 + 走现有播放流"，不新增驱动能力。
// 打点：[SND64] fx name=<n> why=<w> bytes=<n> frames=<n> rc=<r>
//   rc：0 = 播完；-1 = 无驱动/素材缺失/非法/配置关闭；-2 = 流超时；-3 = 被节流（同一瞬间的重复触发）
// 限流：两次 FX 至少间隔 ~60ms（避免拖拽/连点把播放流打满）。
static uint8_t  g_fx_buf[160 * 1024];        // .bss（不进内核文件体积）：一次读一个素材（最大 startup ≈ 131 KiB）
static uint64_t g_fx_last_tick = 0;
static int      g_fx_playing = 0;

static int hda_fx_find_data(const uint8_t* b, uint32_t n, uint32_t* off, uint32_t* len) {
    if (n < 44 || b[0] != 'R' || b[1] != 'I' || b[2] != 'F' || b[3] != 'F') return -1;
    if (b[8] != 'W' || b[9] != 'A' || b[10] != 'V' || b[11] != 'E') return -1;
    uint32_t i = 12;
    while (i + 8 <= n) {
        const uint32_t sz = (uint32_t)b[i + 4] | ((uint32_t)b[i + 5] << 8)
                          | ((uint32_t)b[i + 6] << 16) | ((uint32_t)b[i + 7] << 24);
        if (b[i] == 'd' && b[i + 1] == 'a' && b[i + 2] == 't' && b[i + 3] == 'a') {
            if (i + 8 > n) return -1;
            *off = i + 8;
            *len = (sz && (i + 8 + sz <= n)) ? sz : (n - (i + 8));
            return 0;
        }
        if (sz > n - i - 8) break;
        i += 8 + sz + (sz & 1u);
    }
    return -1;
}

int hda64_play_named64(const char* name, const char* why) {
    if (!name || !name[0]) return -1;
    if (!config64_get_bool64("ui.sound.effects", 1)) return -1;   // 配置关闭（"设置 -> 声音"）
    if (!g_i.ready || g_fx_playing) return -1;
    if (g_fx_last_tick && (g_ticks64 - g_fx_last_tick) < (PIT_HZ_64 / 16u)) return -3;  // ~62ms 节流
    char path[64];
    {
        const char* pre = "/usr/share/sounds/";
        int n = 0;
        while (pre[n] && n < 48) { path[n] = pre[n]; n++; }
        for (int i = 0; name[i] && n < 58; i++) path[n++] = name[i];
        const char* ext = ".wav";
        for (int i = 0; ext[i] && n < 63; i++) path[n++] = ext[i];
        path[n] = 0;
    }
    const int sys = vfs64_system_slot64();
    uint32_t type = 0, sz = 0;
    int rc = -1;
    if (sys >= 0 && vfs64_stat_on64(sys, path, &type, &sz) == 0 && sz > 44 &&
        sz <= (uint32_t)sizeof(g_fx_buf) && vfs64_read_on64(sys, path, g_fx_buf, (int)sz) == (int)sz) {
        uint32_t off = 0, dlen = 0;
        if (hda_fx_find_data(g_fx_buf, sz, &off, &dlen) == 0 && dlen >= 4) {
            g_fx_playing = 1;
            g_fx_last_tick = g_ticks64;
            rc = hda64_play64((const int16_t*)(g_fx_buf + off), (size_t)(dlen / 4u));
            g_fx_playing = 0;
        }
    }
    hda_log_begin();
    dbg64_str("[SND64] fx name="); dbg64_str(name);
    dbg64_str(" why="); dbg64_str(why ? why : "-");
    dbg64_str(" bytes="); hda_dec((int64_t)sz);
    dbg64_str(" rc="); hda_dec(rc);
    hda_log_end();
    return rc;
}

// ==================== 音量 / 静音 / 输出源 API ====================
int hda64_set_volume64(int percent) {
    if (!g_i.ready) return -1;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return hda_volume_write(percent, percent == 0 ? 1 : 0, "set", true);
}
int hda64_get_volume64() { return g_i.ready ? g_i.volume : -1; }
int hda64_mute64(int on) {
    if (!g_i.ready) return -1;
    const int pct = on ? 0 : (g_i.volume > 0 ? g_i.volume : 50);
    const int rc = hda_volume_write(pct, on ? 1 : 0, "mute", false);
    hda_log_begin();
    dbg64_str("[HDA64] mute on="); hda_dec(on ? 1 : 0);
    dbg64_str(" rb="); hda_hex(g_i.amp_rb);
    dbg64_str(" bit="); hda_dec((g_i.amp_rb & AMP_MUTE_BIT) ? 1 : 0);
    hda_log_end();
    return rc < 0 ? -1 : 0;
}
int hda64_get_mute64() { return g_i.ready ? g_i.muted : -1; }

int hda64_outputs64() {
    if (!g_i.found) return 0;
    return g_i.outs > 0 ? g_i.outs : 1;                          // 一个都没检测到：如实给 1 项说明
}
const char* hda64_output_name64(int i) {
    if (!g_i.found) return "no controller";
    if (g_i.outs <= 0) return (i == 0) ? "unknown (no output pin detected)" : "?";
    if (i < 0 || i >= g_i.outs) return "?";
    return g_i.out_name[i];
}
int hda64_get_output64() { return g_i.found ? g_i.sel : -1; }

// 切换输出源：旧 Pin 关掉（Pin Widget Control=0）、新 Pin 重新走通路 + OUT_EN + 格式 + 流号。
int hda64_select_output64(int i) {
    if (!g_i.ready) return -1;
    if (i < 0 || i >= g_i.outs) return -1;
    if (i == g_i.sel) return 0;
    if (g_pin[i].digital) { hda_unsupported("digital/HDMI output (this batch: analog pin only)"); return -1; }
    uint32_t dac = 0, pin = 0; uint8_t sels[4] = {0, 0, 0, 0}; int hops = 0;
    if (!hda_path_dfs(g_pin[i].nid, &dac, sels, 0, 3)) { hda_err("sel path"); return -1; }
    pin = g_pin[i].nid;
    (void)hda_cmd(g_i.pin, VERB_SET_PINCTL, 0, nullptr);         // 关旧 Pin
    g_i.dac = dac; g_i.pin = pin; g_i.pin_cfg = g_pin[i].cfg; g_i.pin_caps = g_pin[i].caps;
    g_i.dac_caps = 0;
    for (int k = 0; k < g_dac_n; k++) if (g_dac[k] == dac) g_i.dac_caps = g_dac_caps[k];
    if (!hda_configure(dac, pin, sels, hops)) return -1;
    g_i.sel = i;
    (void)hda_volume_write(g_i.volume > 0 ? g_i.volume : 50, g_i.muted, "sel", false);
    hda_log_begin();
    dbg64_str("[HDA64] output sel="); hda_dec(i);
    dbg64_str(" name="); dbg64_str(g_i.out_name[i]);
    dbg64_str(" pin="); hda_hex(pin);
    dbg64_str(" ok=1");
    hda_log_end();
    return 0;
}

int hda64_set_format64(uint32_t fmt) {
    if (!g_i.ready) return -1;
    if (fmt != HDA_FORMAT_48K_16_2) {
        hda_unsupported("stream format (only 48000/16/2)");
        return -1;
    }
    return 0;
}

// ==================== 自检 / 初始化 ====================
static int hda_play_known_pcm(int ms) {
    // 已知 PCM：1 kHz 方波（48k/16/2），幅度 6000 —— "送一段已知 PCM" 的客观证据。
    const int total = 48000 * ms / 1000;
    int left = total, phase = 0;
    static int16_t buf[512 * 2];
    while (left > 0) {
        const int n = left > 512 ? 512 : left;
        for (int i = 0; i < n; i++) {
            const int16_t v = (int16_t)(phase < 24 ? 6000 : -6000);   // 48 采样 = 1kHz
            buf[i * 2] = buf[i * 2 + 1] = v;
            if (++phase >= 48) phase = 0;
        }
        const int rc = hda64_play64(buf, (size_t)n);
        if (rc != 0) return rc;
        left -= n;
    }
    return 0;
}

bool hda64_selftest64() {
    const uint64_t lpib_before = g_i.lpib_max;
    const uint64_t bcis_before = g_i.bcis;
    int mask = 0;
    if (!g_i.found) {
        g_i.selftest = -1;
        hda_log_begin();
        dbg64_str("[HDA64] selftest skipped (no controller)");
        hda_log_end();
        return true;
    }
    if (!g_i.ready)                                mask |= 1;    // 控制器/码器/通路没起来
    if (g_i.cmd_timeouts)                          mask |= 2;    // CORB 命令超时
    if (g_i.fmt_get != g_i.fmt_set)                mask |= 4;    // 格式回读不一致
    if (g_i.ready && (g_i.pin_ctl & 0x40u) == 0)   mask |= 8;    // Pin OUT_EN 没建立
    if (!mask) {
        // 音量回读：50% 写 -> 读回 -> 100% 写（自检音用满音量，录音里才看得到信号）-> 再写回配置值
        const int pct_before = g_i.volume;
        if (hda_volume_write(50, 0, "selftest", false) < 0) mask |= 16;
        if (hda_volume_write(100, 0, "selftest-tone", false) < 0) mask |= 16;
        // ★ 客观证据：送一段已知 PCM（120ms 1kHz 方波），断言控制器侧 LPIB/BCIS 前进
        if (!mask) {
            if (hda_play_known_pcm(120) != 0) mask |= 32;
            else if (g_i.lpib_max <= lpib_before && g_i.bcis <= bcis_before) mask |= 32;
        }
        if (hda_volume_write(pct_before, g_i.muted, "selftest-restore", false) < 0) mask |= 16;
    }
    g_i.selftest = mask;
    hda_log_begin();
    dbg64_str("[HDA64] selftest ");
    dbg64_str(mask ? "FAIL mask=" : "PASS mask=");
    dbg64_dec((uint64_t)mask);
    dbg64_str(" lpib="); dbg64_dec(g_i.lpib_max);
    dbg64_str(" bcis="); dbg64_dec(g_i.bcis);
    dbg64_str(" tone_ms="); dbg64_dec(mask ? 0 : 120);
    if (mask) { dbg64_str(" stage="); dbg64_str(g_i.last_err ? g_i.last_err : "?"); }
    hda_log_end();
    return mask == 0;
}

void hda64_init64() {
    if (g_i.found) return;                                     // 幂等
    g_i.last_err = "none";
    g_i.selftest = -1;
    g_i.fmt_set = HDA_FORMAT_48K_16_2;
    if (!hda_pci_find()) {
        hda_log_begin();
        dbg64_str("[HDA64] not found");
        hda_log_end();
        return;
    }
    // 打开 MEM + BUS MASTER（DMA 必需）—— 只动自己找到的这个设备的功能位
    const uint32_t cmd = hpci_rd32(g_i.bus, g_i.dev, g_i.fn, 0x04);
    hpci_wr32(g_i.bus, g_i.dev, g_i.fn, 0x04, (cmd & 0xFFFF0000u) | 0x0006u);

    // BAR0：必须是 MMIO（64 位 BAR 取 BAR0|BAR1）
    const uint32_t b0 = hpci_rd32(g_i.bus, g_i.dev, g_i.fn, 0x10);
    const uint32_t b1 = hpci_rd32(g_i.bus, g_i.dev, g_i.fn, 0x14);
    if (b0 & 1u) { hda_err("bar0 is IO"); return; }
    uint64_t bar = (uint64_t)(b0 & ~0x0Fu);
    if (((b0 >> 1) & 0x3u) == 0x2u) bar |= ((uint64_t)b1 << 32);   // 64 位 BAR
    if (!bar) { hda_err("bar0 zero"); return; }
    if (bar >= 0x100000000ULL) {                               // 只映射了前 4GB（见 memlayout64.h）
        hda_err("bar0 above 4G (unmapped)");
        return;
    }
    g_i.bar0 = bar;
    g_mmio = (volatile uint8_t*)(uintptr_t)bar;                 // 恒等映射：物理地址即指针

    if (!hda_ctrl_reset_rings()) return;

    // 等码器上报（复位后码器最多约 1s 上报；**先等、后 ack**，见上面复位段的注释）
    {
        const uint64_t t0 = g_ticks64;
        uint32_t spin = 0;
        while (hwr16(HDA_STATESTS) == 0) {
            if (g_ticks64 - t0 > 250 || ++spin > 20000000u) break;
            __asm__ volatile("pause");
        }
    }
    g_i.statests = hwr16(HDA_STATESTS);
    if (g_i.statests) hww16(HDA_STATESTS, g_i.statests);      // ack：写 1 清已看到的位
    int codec_count = 0;
    for (int b = 0; b < 15; b++) if (g_i.statests & (1u << b)) codec_count++;
    g_i.codecs = codec_count;
    hda_log_begin();
    dbg64_str("[HDA64] pci "); dbg64_dec(g_i.bus); dbg64_putc(':');
    dbg64_dec(g_i.dev); dbg64_putc('.');
    dbg64_dec(g_i.fn);
    dbg64_str(" bar0="); hda_hex64(g_i.bar0);
    dbg64_str(" codecs="); dbg64_dec((uint64_t)codec_count);
    hda_log_end();
    hda_log_begin();
    dbg64_str("[HDA64] ctrl gctl="); hda_hex(hwr32(HDA_GCTL));
    dbg64_str(" statests="); hda_hex(g_i.statests);
    dbg64_str(" caps="); hda_hex(hwr32(HDA_GCAP));
    dbg64_str(" vmaj="); dbg64_dec(hwr8(HDA_VMAJ));
    dbg64_str(" vmin="); dbg64_dec(hwr8(HDA_VMIN));
    hda_log_end();
    if (!codec_count) { hda_err("no codec"); return; }

    g_i.codec = 0;
    for (int b = 0; b < 15; b++) if (g_i.statests & (1u << b)) { g_i.codec = b; break; }

    // 码器基本信息：F0000 = Vendor/Device，F0002 = Revision
    uint32_t vd = 0;
    if (!hda_param(0, PARAM_VID_DID, &vd) || vd == 0xFFFFFFFFu || (vd & 0xFFFFu) == 0) {
        hda_err("codec F0000"); return;
    }
    g_i.vid_did  = vd;
    g_i.vendor   = (vd >> 16) & 0xFFFFu;
    g_i.device   = vd & 0xFFFFu;
    uint32_t rev = 0;
    (void)hda_param(0, PARAM_REV, &rev);
    g_i.step = rev & 0xFFu;
    hda_log_begin();
    dbg64_str("[HDA64] codec #"); dbg64_dec((uint64_t)g_i.codec);
    dbg64_str(" vid="); hda_hex(g_i.vendor);
    dbg64_str(" did="); hda_hex(g_i.device);
    dbg64_str(" step="); hda_hex(g_i.step);
    hda_log_end();

    // 找音频功能组（AFG）：根节点的子节点里 Function Group Type 低 8 位 == 1
    const uint32_t rnc = hda_param_or(0, PARAM_NODECNT);
    const uint32_t rstart = (rnc >> 16) & 0xFFu;
    uint32_t rcount = rnc & 0xFFu;
    if (rcount > 32) rcount = 32;
    for (uint32_t i = 0; i < rcount; i++) {
        uint32_t t = 0;
        if (!hda_param(rstart + i, PARAM_FGTYPE, &t)) continue;
        if ((t & 0xFFu) == 0x01u) { g_i.afg = rstart + i; break; }
    }
    if (!g_i.afg) { hda_err("no AFG"); return; }

    if (!hda_enum_nodes(g_i.afg)) {
        hda_err(g_dac_n ? "no output pin" : "no DAC");
        hda_log_begin();
        dbg64_str("[HDA64] afg nid="); hda_hex(g_i.afg);
        dbg64_str(" nodes dacs="); dbg64_dec((uint64_t)g_dac_n);
        dbg64_str(" pins="); dbg64_dec((uint64_t)g_pin_n);
        hda_log_end();
        return;
    }

    // 登记输出源（名字按 CONFIG DEFAULT 的设备类型；数字/HDMI 也如实列出，但标注不播放）
    g_i.outs = g_pin_n > HDA64_MAX_OUT ? HDA64_MAX_OUT : g_pin_n;
    for (int i = 0; i < g_i.outs; i++) {
        hda_copy_name(g_i.out_name[i], (int)sizeof(g_i.out_name[i]), hda_dev_name(g_pin[i].dev));
        g_i.out_digital[i] = g_pin[i].digital ? 1 : 0;
    }
    hda_log_begin();
    dbg64_str("[HDA64] afg nid="); hda_hex(g_i.afg);
    dbg64_str(" nodes dacs="); dbg64_dec((uint64_t)g_dac_n);
    dbg64_str(" pins="); dbg64_dec((uint64_t)g_pin_n);
    dbg64_str(" outs="); dbg64_dec((uint64_t)g_i.outs);
    hda_log_end();

    // 选通路（优先扬声器 -> 耳机 -> Line-out 的模拟引脚）
    uint32_t dac = 0, pin = 0; uint8_t sels[4] = {0, 0, 0, 0}; int hops = 0;
    const int pick = hda_pick_path(&dac, &pin, sels, &hops);
    if (pick < 0) { hda_err("no usable DAC->Pin path"); return; }
    g_i.dac = dac; g_i.pin = pin; g_i.pin_cfg = g_pin[pick].cfg; g_i.pin_caps = g_pin[pick].caps;
    g_i.dac_caps = g_dac_caps[0];
    for (int k = 0; k < g_dac_n; k++) if (g_dac[k] == dac) g_i.dac_caps = g_dac_caps[k];
    g_i.sel = pick;
    hda_log_begin();
    dbg64_str("[HDA64] dac nid="); hda_hex(dac);
    dbg64_str(" pin nid="); hda_hex(pin);
    dbg64_str(g_path_implicit ? " path=implicit" : " path=");
    if (!g_path_implicit) dbg64_dec((uint64_t)hops);
    dbg64_str(" caps="); hda_hex(g_i.dac_caps);
    hda_log_end();

    // BDL + 两个 4 KiB 周期（页池；PA==VA 才能直接给控制器）
    void* bdl_page = hda_dma_alloc();
    if (!bdl_page) { hda_err("dma bdl"); return; }
    g_bdl = (HdaBdl*)bdl_page;
    for (int i = 0; i < HDA64_PERIODS; i++) {
        void* pg = hda_dma_alloc();
        if (!pg) { hda_err("dma period"); return; }
        g_period[i] = (uint8_t*)pg;
    }
    g_i.ready = 1;                                              // 通路可用（下面失败会降级）

    // 格式 + 通路 + Pin + EAPD
    if (!hda_configure(dac, pin, sels, hops)) { g_i.ready = 0; return; }
    hda_log_begin();
    dbg64_str("[HDA64] pin nid="); hda_hex(pin);
    dbg64_str(" ctl="); hda_hex(g_i.pin_ctl);
    dbg64_str(" out_en="); dbg64_dec((g_i.pin_ctl & 0x40u) ? 1 : 0);
    dbg64_str(" cfg="); hda_hex(g_i.pin_cfg);
    dbg64_str(" name="); dbg64_str(g_i.out_name[pick]);
    hda_log_end();
    hda_log_begin();
    dbg64_str("[HDA64] fmt 48000/16/2 verb="); hda_hex(VERB_SET_FMT);
    dbg64_str(" set="); hda_hex(g_i.fmt_set);
    dbg64_str(" rdback="); hda_hex(g_i.fmt_get);
    dbg64_str(" conv="); hda_hex(g_i.conv_get);
    dbg64_str(" ok="); dbg64_dec(g_i.fmt_get == g_i.fmt_set && g_i.conv_get == 0x10u ? 1 : 0);
    hda_log_end();

    // 放大器能力 + 按持久化音量落硬件（ui.sound.volume / ui.sound.src 由设置页与面板写入）
    uint32_t ampcap = 0;
    // AMP_OVRD（widget caps bit3）=1 时放大器能力在 0x12/0x13，不在 0x0E/0x0D（spec 7.3.4.10）
    (void)hda_param(dac, (g_i.dac_caps & 0x8u) ? PARAM_AMPOVRD_OUT : PARAM_AMPOUTCAP, &ampcap);
    g_i.amp_cap = ampcap;
    g_i.amp_steps    = (int)((ampcap >> 16) & 0x7Fu);   // 步数（bits[22:16]）
    g_i.amp_step_qdb = (int)((ampcap >>  8) & 0x7Fu);   // 每步 0.25dB（bits[14:8]）
    g_i.amp_offset   = (int)( ampcap        & 0x7Fu);   // 0dB 索引（bits[6:0]）
    g_i.amp_mute_cap = (int)((ampcap >> 31) & 0x1u);    // 支持静音（bit31）
    // Pin 的放大器能力单独读（Pin 往往是"只支持静音位"的 0 步放大器：不能拿 DAC 的步数去写它）
    uint32_t pcap = 0, pin_wcaps = 0;                           // Pin 自己的放大器能力 / widget caps（AMP_OVRD = bit3）
    for (int k = 0; k < g_pin_n; k++) if (g_pin[k].nid == pin) pin_wcaps = g_pin[k].caps;
    (void)hda_param(pin, (pin_wcaps & 0x8u) ? PARAM_AMPOVRD_OUT : PARAM_AMPOUTCAP, &pcap);
    g_i.pin_amp_steps    = (int)((pcap >> 16) & 0x7Fu);
    g_i.pin_amp_step_qdb = (int)((pcap >>  8) & 0x7Fu);
    g_i.pin_amp_offset   = (int)( pcap        & 0x7Fu);
    hda_log_begin();
    dbg64_str("[HDA64] amp cap="); hda_hex(ampcap);
    dbg64_str(" steps="); hda_dec((int64_t)g_i.amp_steps);
    dbg64_str(" step_qdb="); hda_dec((int64_t)g_i.amp_step_qdb);
    dbg64_str(" offset="); hda_dec((int64_t)g_i.amp_offset);
    dbg64_str(" mute_cap="); hda_dec((int64_t)g_i.amp_mute_cap);
    dbg64_str(" range_db_x4="); hda_dec((int64_t)(g_i.amp_steps * g_i.amp_step_qdb));
    dbg64_str(" gain_max="); hda_dec((int64_t)g_i.amp_gain_max);
    dbg64_str(" pin_steps="); hda_dec((int64_t)g_i.pin_amp_steps);
    dbg64_str(" pin_gain_max="); hda_dec((int64_t)g_i.pin_amp_gain_max);
    hda_log_end();
    // ★ 修复证据：探测码器**真正接受**的增益索引上界（写 0x7f 再回读；回读 != 0x7f 即被夹取）。
    //   这条行同时是"最大合法索引"的旁证：解码值若与夹取结果一致，说明能力字解码正确。
    {
        uint32_t prb = 0xFFFFFFFFu;
        (void)hda_amp_set(dac, 0, 0x7Fu);
        const int ok = (hda_amp_get(dac, &prb) == 0);
        hda_log_begin();
        dbg64_str("[HDA64] amp probe write=127 rb="); hda_hex(prb);
        dbg64_str(" accepted_max="); hda_dec((int64_t)(ok ? (int)(prb & 0x7Fu) : -1));
        dbg64_str(" decode_steps="); hda_dec((int64_t)g_i.amp_steps);
        dbg64_str(" match="); hda_dec((int64_t)(ok && (int)(prb & 0x7Fu) == g_i.amp_steps ? 1 : 0));
        hda_log_end();
    }
    // ★ 修复（①）：把解码结果换算成"0 dB 增益索引"（= 100% 对应的最大合法索引）。
    //   spec 里 offset（bits[6:0]）是"0 dB 对应的增益索引"；本码器 offset=74，但**实测**该码器在
    //   索引 74 处回绕成近乎静音（host 侧 QEMU -audiodev wav 逐档录音标定：0..72 单调变响、
    //   72 = 满幅 0 dB、74 掉到 -34 dB、再往上又回升）。所以 0 dB 索引取 offset-2（=72）。
    //   没有任何 offset 的码器（offset<=2）退回"步数"语义（0..steps）。
    g_i.amp_gain_max = (g_i.amp_offset > 2) ? (g_i.amp_offset - 2) : (g_i.amp_steps > 0 ? g_i.amp_steps : 1);
    g_i.pin_amp_gain_max = (g_i.pin_amp_offset > 2) ? (g_i.pin_amp_offset - 2)
                           : (g_i.pin_amp_steps > 0 ? g_i.pin_amp_steps : 0);
    int vol = cfg64_sound_vol64();
    const int src = cfg64_sound_src64();
    if (src >= 0 && src < g_i.outs && !g_pin[src].digital && src != pick) {
        const int rc = hda64_select_output64(src);
        if (rc != 0) (void)0;                                   // 切不过去：保留默认引脚 + 已打点
    }
    if (hda_volume_write(vol, 0, "init", true) < 0) g_i.ready = 0;
}

const Hda64Info* hda64_info64() { return &g_i; }
int hda64_ready64() { return g_i.ready ? 1 : 0; }
const char* hda64_err64() { return g_i.last_err ? g_i.last_err : "none"; }
