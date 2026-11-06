// vmware64.cpp - ★ v0.9.18：VMware SVGA II 的**只读**能力探测（3D 能力位取证）
//
// ============================ 它做什么 / 不做什么 ============================
// 做：按 SVGA 的寄存器协议读身份与能力位（SVGA_REG_ID / CAPABILITIES / 几何 / 显存 / FIFO），
//     并打一行 `[VMWARE] svga id=… caps=… 3d=…`。
// 不做：不写 SVGA_REG_ENABLE（那是"接管显示"—— 会把当前 VBE 画面换掉）、不写 FIFO、
//       不碰 SVGA3D 命令。**唯一的一次写**是 SVGA_REG_ID 的版本协商（写 0x90000002 再回读），
//       这是驱动初始化前就该做的一步，不影响显示。
//
// 为什么 BAR0 常常不是 MMIO：VMware 的 SVGA II 把寄存器放在 **I/O 端口**（BAR0 = I/O BAR），
//   索引口 = base+0、数据口 = base+1。QEMU 上跑不到这条路径，所以只能在 VMware 里验证。
//
// 寄存器编号（vmware/svga_reg.h 的顺序，别改）：
//   0 ID / 1 ENABLE / 2 WIDTH / 3 HEIGHT / 4 MAX_WIDTH / 5 MAX_HEIGHT / 6 DEPTH /
//   7 BITS_PER_PIXEL / 12 BYTES_PER_LINE / 13 FB_START / 15 VRAM_SIZE /
//   17 CAPABILITIES / 18 MEM_START / 19 MEM_SIZE
// 能力位：3D=0x00010000 / RECT_FILL=0x4 / ALPHA_BLEND=0x80 / GMR=0x100000 /
//         SCREEN_OBJECT_2=0x01000000 / EXTENDED_FIFO=0x00020000
#include "vmware64.h"
#include "debug64.h"
#include "port.h"          // inl / outl

static const char* const VMW_HEXL = "0123456789abcdef";
__attribute__((noinline)) static void vw_puts(const char* s) { dbg64_str(s); }
__attribute__((noinline)) static void vw_ch(char c) { dbg64_putc(c); }
__attribute__((noinline)) static void vw_num(uint64_t v) { dbg64_dec(v); }
__attribute__((noinline)) static void vw_hex(uint64_t v) {
    vw_puts("0x");
    if (!v) { vw_ch('0'); return; }
    char b[16]; int n = 0;
    while (v && n < 16) { b[n++] = VMW_HEXL[v & 0xFu]; v >>= 4; }
    while (n > 0) vw_ch(b[--n]);
}
__attribute__((noinline)) static void vw_begin() { dbg64_line_begin64(); }
__attribute__((noinline)) static void vw_end() { dbg64_nl(); dbg64_line_end64(); }

#define VMW_REG_ID          0u
#define VMW_REG_WIDTH       2u
#define VMW_REG_HEIGHT      3u
#define VMW_REG_MAX_WIDTH   4u
#define VMW_REG_MAX_HEIGHT  5u
#define VMW_REG_BPP         7u
#define VMW_REG_BYTES_LINE  12u
#define VMW_REG_FB_START    13u
#define VMW_REG_VRAM_SIZE   15u
#define VMW_REG_CAPABILITIES 17u
#define VMW_REG_MEM_START   18u
#define VMW_REG_MEM_SIZE    19u

#define VMW_ID_2   0x90000002u
#define VMW_ID_1   0x90000001u

#define VMW_CAP_RECT_FILL      0x00000004u
#define VMW_CAP_ALPHA_BLEND    0x00000080u
#define VMW_CAP_3D             0x00010000u
#define VMW_CAP_EXTENDED_FIFO  0x00020000u
#define VMW_CAP_GMR            0x00100000u
#define VMW_CAP_SCREEN_OBJ_2   0x01000000u

// 寄存器访问（I/O 端口或 MMIO 两种都支持）
struct VmwBus {
    int      is_io;
    uint64_t base;
};
static uint32_t vw_reg_rd(const VmwBus* b, uint32_t idx) {
    if (b->is_io) {
        outl((uint16_t)(b->base + 0u), idx);
        return inl((uint16_t)(b->base + 1u));
    }
    return *(volatile uint32_t*)(uintptr_t)(b->base + (uint64_t)idx * 4ull);
}
static void vw_reg_wr(const VmwBus* b, uint32_t idx, uint32_t v) {
    if (b->is_io) {
        outl((uint16_t)(b->base + 0u), idx);
        outl((uint16_t)(b->base + 1u), v);
        return;
    }
    *(volatile uint32_t*)(uintptr_t)(b->base + (uint64_t)idx * 4ull) = v;
}

int vmware64_svga_probe64(uint32_t bdf, int bar0_is_io, uint64_t bar0_base) {
    (void)bdf;
    if (bar0_base == 0ull) {
        vw_begin(); vw_puts("[VMWARE] svga skipped reason=no-bar0"); vw_end();
        return 0;
    }
    VmwBus bus;
    bus.is_io = bar0_is_io ? 1 : 0;
    bus.base = bar0_base;

    uint32_t id = vw_reg_rd(&bus, VMW_REG_ID);
    // 版本协商：只有 ID 还不可识别时才写一次（写 ID 不影响显示，只定协议版本）
    if (id != VMW_ID_2 && id != VMW_ID_1) {
        vw_reg_wr(&bus, VMW_REG_ID, VMW_ID_2);
        id = vw_reg_rd(&bus, VMW_REG_ID);
        if (id != VMW_ID_2) {
            vw_reg_wr(&bus, VMW_REG_ID, VMW_ID_1);
            id = vw_reg_rd(&bus, VMW_REG_ID);
        }
    }
    const uint32_t caps = vw_reg_rd(&bus, VMW_REG_CAPABILITIES);
    const uint32_t w    = vw_reg_rd(&bus, VMW_REG_WIDTH);
    const uint32_t h    = vw_reg_rd(&bus, VMW_REG_HEIGHT);
    const uint32_t mw   = vw_reg_rd(&bus, VMW_REG_MAX_WIDTH);
    const uint32_t mh   = vw_reg_rd(&bus, VMW_REG_MAX_HEIGHT);
    const uint32_t bpp  = vw_reg_rd(&bus, VMW_REG_BPP);
    const uint32_t stride = vw_reg_rd(&bus, VMW_REG_BYTES_LINE);
    const uint32_t fbs  = vw_reg_rd(&bus, VMW_REG_FB_START);
    const uint32_t vram = vw_reg_rd(&bus, VMW_REG_VRAM_SIZE);
    const uint32_t mems = vw_reg_rd(&bus, VMW_REG_MEM_START);
    const uint32_t meml = vw_reg_rd(&bus, VMW_REG_MEM_SIZE);

    const int known = (id == VMW_ID_2 || id == VMW_ID_1);
    if (!known) {
        // 读不到可识别的 ID：如实报"这个 BAR 上没有 SVGA 寄存器"，不做任何猜测
        vw_begin(); vw_puts("[VMWARE] svga unreadable bar0="); vw_hex(bar0_base);
        vw_puts(bar0_is_io ? " bar=io id=" : " bar=mmio id="); vw_hex(id); vw_end();
        return 0;
    }
    vw_begin(); vw_puts("[VMWARE] svga id="); vw_hex(id);
    vw_puts(" caps="); vw_hex(caps);
    vw_puts((caps & VMW_CAP_3D) ? " 3d=1" : " 3d=0");
    vw_puts((caps & VMW_CAP_RECT_FILL) ? " rect_fill=1" : " rect_fill=0");
    vw_puts((caps & VMW_CAP_ALPHA_BLEND) ? " alpha=1" : " alpha=0");
    vw_puts((caps & VMW_CAP_GMR) ? " gmr=1" : " gmr=0");
    vw_puts((caps & VMW_CAP_SCREEN_OBJ_2) ? " screen2=1" : " screen2=0");
    vw_puts((caps & VMW_CAP_EXTENDED_FIFO) ? " fifo_ext=1" : " fifo_ext=0");
    vw_puts(" geom="); vw_num(w); vw_puts("x"); vw_num(h);
    vw_puts(" max="); vw_num(mw); vw_puts("x"); vw_num(mh);
    vw_puts(" bpp="); vw_num(bpp); vw_puts(" stride="); vw_num(stride);
    vw_puts(" fb="); vw_hex(fbs); vw_puts(" vram="); vw_num(vram);
    vw_puts(" mem="); vw_hex(mems); vw_puts(" memlen="); vw_num(meml);
    vw_puts(bar0_is_io ? " port=io" : " port=mmio");
    vw_end();
    return 1;
}
