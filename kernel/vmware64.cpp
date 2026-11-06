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
#include "pci64.h"         // pci64_cfg_rd32（stage1 重新读 BAR0）

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

// stage1 需要用探测阶段记下的 bdf（避免再扫一遍 PCI）
static uint32_t g_vmw_bdf = 0xFFFFFFFFu;
static int      g_vmw_ok = 0;

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
    g_vmw_bdf = bdf;
    g_vmw_ok = 1;
    return 1;
}

// ==================== 阶段 1（v0.9.19）：真正把设备驱动起来 ====================
// 为什么分阶段：读寄存器只是"看"，`SVGA3D` 要先把 **FIFO** 配好并把命令递进去。
// 这一步做的是：配 FIFO -> CONFIG_DONE -> SYNC/BUSY 往返 -> ENABLE -> UPDATE 提交。
// 两个前提（缺一就不做，绝不拿显示去赌）：
//   ① 卷里有 `/etc/gpu3d_probe`（opt-in：默认启动一个寄存器都不写）
//   ② `SVGA_REG_FB_START` == 我们当前正在用的帧缓冲物理地址（否则 ENABLE 会换屏，可能直接黑屏）
#include "fb.h"            // fb64_lfb_phys64 / fb_phys_width / fb_phys_height
#include "vfs64.h"         // vfs64_stat（opt-in 探针）
#include "x86_64.h"        // g_ticks64（有界等待）

#define VMW_REG_ENABLE       1u
#define VMW_REG_CONFIG_DONE  20u
#define VMW_REG_SYNC         21u
#define VMW_REG_BUSY         22u

// FIFO 寄存器（在 MEM 区的 uint32 数组里）
#define VMW_FIFO_MIN         0u
#define VMW_FIFO_MAX         1u
#define VMW_FIFO_NEXT_CMD    2u
#define VMW_FIFO_STOP        3u
#define VMW_FIFO_CAPABILITIES 4u
#define VMW_FIFO_MIN_WORDS   16u          // 头部保留 16 word（寄存器 + capabilities）

#define VMW_CMD_UPDATE       1u           // SVGA_CMD_UPDATE { x, y, w, h }

// 有界等待 SVGA_REG_BUSY 变 0。max_reads = **最多读几次 BUSY**（不是 spin 次数）。
// ★ v0.9.20 修：v0.9.19 这里是"2,000,000 次 spin、每次都 outl+inl"——
//   在 VMware 上**每一次端口 I/O 都是一次 VM-exit**（真机上 1~10 us），
//   2,000,000 次就是几十秒到几分钟的"假卡死"，ENABLE 之后启动停住就是这个。
//   现在：两次端口读之间插 65536 次 pause（纯 CPU，不触发 VM-exit），
//   并把上限收到 max_reads 次读（2000 次 × 约 30 us ≈ 60 ms 封顶）。
static int vw_wait_idle(const VmwBus* b, uint32_t max_reads) {
    for (uint32_t r = 0; r < max_reads; r++) {
        if (vw_reg_rd(b, VMW_REG_BUSY) == 0u) return 1;
        for (uint32_t k = 0; k < 65536u; k++) __asm__ volatile("pause");
    }
    return 0;
}

// 逐段面包屑：`[VMWARE] stage1 <tag> i=<n>`。死在哪一段，下一次串口里一眼可见
// （i= 自增序号，跨三段连续；既有断言只看行尾字段，不受影响）。
static uint32_t g_vw_crumb_i = 0;
__attribute__((noinline)) static void vw_crumb(const char* tag) {
    vw_begin(); vw_puts("[VMWARE] stage1 "); vw_puts(tag);
    vw_puts(" i="); vw_num(++g_vw_crumb_i); vw_end();
}

// 配 FIFO 并做一次 SYNC 往返。max_words: 1 = MAX 用 word 数，0 = 用字节数（两种口径都试一次）。
static int vw_fifo_config(const VmwBus* b, uint32_t mem_start, uint32_t mem_size, int max_words,
                          uint32_t* out_min, uint32_t* out_max) {
    if (mem_size < 4096u || mem_start == 0u) return 0;
    if ((uint64_t)mem_start + (uint64_t)mem_size > 0x100000000ull) return 0;   // 必须在恒等直映区
    volatile uint32_t* fifo = (volatile uint32_t*)(uintptr_t)mem_start;
    vw_reg_wr(b, VMW_REG_CONFIG_DONE, 0u);
    const uint32_t maxv = max_words ? (mem_size / 4u) : mem_size;
    fifo[VMW_FIFO_MIN]  = VMW_FIFO_MIN_WORDS;
    fifo[VMW_FIFO_MAX]  = maxv;
    fifo[VMW_FIFO_NEXT_CMD] = VMW_FIFO_MIN_WORDS;
    fifo[VMW_FIFO_STOP] = VMW_FIFO_MIN_WORDS;
    fifo[VMW_FIFO_CAPABILITIES] = 0u;
    __asm__ volatile("mfence" ::: "memory");
    vw_reg_wr(b, VMW_REG_CONFIG_DONE, 1u);
    // SYNC：让 host 处理 FIFO 里已有的命令并回到空闲
    vw_reg_wr(b, VMW_REG_SYNC, 1u);
    const int idle = vw_wait_idle(b, 2000u);
    *out_min = fifo[VMW_FIFO_MIN];
    *out_max = fifo[VMW_FIFO_MAX];
    return idle;
}

// 提交一条 SVGA_CMD_UPDATE（4 个参数），返回 1 = host 处理完（BUSY 回 0）
static int vw_fifo_update(const VmwBus* b, uint32_t mem_start, uint32_t maxv, uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h, uint32_t* out_next) {
    volatile uint32_t* fifo = (volatile uint32_t*)(uintptr_t)mem_start;
    uint32_t next = fifo[VMW_FIFO_NEXT_CMD];
    // ★ v0.9.20 修（实测定位）：ENABLE / CONFIG_DONE 之后 host 会把 FIFO[NEXT_CMD] 复位成 0（落在
    //   头部保留区 [0, MIN) 里）。原来的 `if (next < MIN) return 0;` 会让 UPDATE **根本不提交**、却
    //   只打一行 sync=TIMEOUT（看起来像 host 不回 SYNC，其实是自己提前退了）。
    //   FIFO 只有我们一个写者，NEXT_CMD 就该归我们维护 —— 越界就从 MIN 重新开始。
    if (next < VMW_FIFO_MIN_WORDS || next >= maxv) next = VMW_FIFO_MIN_WORDS;
    if (next + 5u > maxv) next = VMW_FIFO_MIN_WORDS;      // 不做环绕（一次探针够用）
    if (out_next) *out_next = next + 5u;
    fifo[next + 0u] = VMW_CMD_UPDATE;
    fifo[next + 1u] = x; fifo[next + 2u] = y; fifo[next + 3u] = w; fifo[next + 4u] = h;
    __asm__ volatile("mfence" ::: "memory");
    fifo[VMW_FIFO_NEXT_CMD] = next + 5u;
    vw_reg_wr(b, VMW_REG_SYNC, 1u);
    return vw_wait_idle(b, 2000u);
}

int vmware64_svga_stage1_64(void) {
    // ① opt-in
    uint32_t t = 0, sz = 0;
    static const char PROBE[] = "/etc/gpu3d_probe";
    if (vfs64_stat(PROBE, &t, &sz) != 0) {
        vw_begin(); vw_puts("[VMWARE] stage1 skipped reason=probe=0 (no /etc/gpu3d_probe)"); vw_end();
        return 0;
    }
    // 找 SVGA（复用探测里那份总线信息：这里重新从 PCI 配置读一遍，保持独立）
    const uint32_t bdf = g_vmw_bdf;
    if (!g_vmw_ok || bdf == 0xFFFFFFFFu) {
        vw_begin(); vw_puts("[VMWARE] stage1 skipped reason=no-probe"); vw_end();
        return 0;
    }
    const uint32_t b0 = pci64_cfg_rd32(bdf, 0x10u);
    if (!(b0 & 1u)) {
        vw_begin(); vw_puts("[VMWARE] stage1 skipped reason=bar0-not-io"); vw_end();
        return 0;
    }
    VmwBus bus;
    bus.is_io = 1;
    bus.base = (uint64_t)(b0 & ~0x3u);

    const uint32_t mems = vw_reg_rd(&bus, VMW_REG_MEM_START);
    const uint32_t meml = vw_reg_rd(&bus, VMW_REG_MEM_SIZE);
    const uint32_t fbs  = vw_reg_rd(&bus, VMW_REG_FB_START);
    const uint64_t lfb  = fb64_lfb_phys64();
    vw_begin(); vw_puts("[VMWARE] stage1 pre fifo=0x"); vw_hex(mems);
    vw_puts(" memlen="); vw_num(meml);
    vw_puts(" fb_start=0x"); vw_hex(fbs);
    vw_puts(" lfb=0x"); vw_hex(lfb); vw_end();
    vw_crumb("a=pre");
    // ② FB_START 必须就是我们正在写的 LFB —— 否则 ENABLE 会换屏
    if ((uint64_t)fbs != lfb) {
        vw_begin(); vw_puts("[VMWARE] stage1 skipped reason=fb-mismatch (ENABLE would switch display)"); vw_end();
        return 0;
    }
    // ③ 配 FIFO：先按 word 口径，失败再按字节口径（两种历史写法都试，最多两次）
    uint32_t fmin = 0, fmax = 0;
    int sync_ok = vw_fifo_config(&bus, mems, meml, 1, &fmin, &fmax);
    int units = 1;
    if (!sync_ok) {
        sync_ok = vw_fifo_config(&bus, mems, meml, 0, &fmin, &fmax);
        units = 0;
    }
    vw_begin(); vw_puts("[VMWARE] fifo cfg min="); vw_num(fmin);
    vw_puts(" max="); vw_num(fmax);
    vw_puts(units ? " units=words" : " units=bytes");
    vw_puts(sync_ok ? " sync=ok" : " sync=TIMEOUT"); vw_end();
    if (!sync_ok) return 0;                                // 配不上就别动 ENABLE
    vw_crumb("b=fifo-ok");

    // ★ 实测（v0.9.19）：ENABLE 之后启动会在桌面之前停住（没有 [GUI64] ready / [UI] perf）——
    //   "接管显示"这一步还有别的东西要一起配（模式/位深/scanout 或 VGA 回扫探针的兼容）。
    //   所以 ENABLE + UPDATE 要再开一层（/etc/gpu3d_enable）：默认只验证 FIFO 通路，
    //   绝不因为一个探针把默认启动弄坏。
    {
        uint32_t et = 0, esz = 0;
        if (vfs64_stat("/etc/gpu3d_enable", &et, &esz) != 0) {
            vw_begin(); vw_puts("[VMWARE] stage1 fifo sync=ok enable skipped reason=no-marker"); vw_end();
            return 1;                                      // FIFO 通路已验证（不改变显示）
        }
    }

    // ④ ENABLE：几何必须与我们的 LFB 完全一致，否则就是"换模式"——跳过（不拿显示赌）
    const uint32_t w = vw_reg_rd(&bus, VMW_REG_WIDTH);
    const uint32_t h = vw_reg_rd(&bus, VMW_REG_HEIGHT);
    const uint32_t bpp = vw_reg_rd(&bus, VMW_REG_BPP);
    if (w != (uint32_t)fb_phys_width() || h != (uint32_t)fb_phys_height()) {
        vw_begin(); vw_puts("[VMWARE] stage1 skipped reason=geom-mismatch svga=");
        vw_num(w); vw_puts("x"); vw_num(h);
        vw_puts(" lfb="); vw_num((uint64_t)fb_phys_width()); vw_puts("x");
        vw_num((uint64_t)fb_phys_height()); vw_end();
        return 0;
    }
    vw_reg_wr(&bus, VMW_REG_WIDTH, w ? w : (uint32_t)fb_phys_width());
    vw_reg_wr(&bus, VMW_REG_HEIGHT, h ? h : (uint32_t)fb_phys_height());
    if (!bpp) vw_reg_wr(&bus, VMW_REG_BPP, 32u);
    vw_reg_wr(&bus, VMW_REG_ENABLE, 1u);
    const uint32_t en = vw_reg_rd(&bus, VMW_REG_ENABLE);
    const uint32_t w2 = vw_reg_rd(&bus, VMW_REG_WIDTH);
    const uint32_t h2 = vw_reg_rd(&bus, VMW_REG_HEIGHT);
    vw_begin(); vw_puts("[VMWARE] svga enable="); vw_num(en);
    vw_puts(" w="); vw_num(w2); vw_puts(" h="); vw_num(h2); vw_end();
    vw_crumb("c=enable-written");
    if (en != 1u) return 0;

    // ★ v0.9.20：实测（v0.9.20 第一轮）ENABLE 之后第一个 UPDATE 的 SYNC 不回来（BUSY 一直 1）。
    //   真驱动的顺序是"接管显示之后再把 FIFO 的 CONFIG_DONE 打一次"——host 在 ENABLE 时会重置
    //   FIFO 的簿记（NEXT_CMD/STOP 由 host 侧维护），所以这里按原口径（同一个 MEM 区）**再配一次**，
    //   成功才提交 UPDATE。失败也不影响显示（LFB 仍在扫出），如实记录、不假装。
    {
        uint32_t m2 = 0, x2 = 0;
        const int re = vw_fifo_config(&bus, mems, meml, units, &m2, &x2);
        vw_begin(); vw_puts("[VMWARE] stage1 refifo min="); vw_num(m2);
        vw_puts(" max="); vw_num(x2);
        vw_puts(re ? " sync=ok" : " sync=TIMEOUT"); vw_end();
    }

    // ⑤ 提交一条 SVGA_CMD_UPDATE（64x64）—— 这一步通了就说明 FIFO 命令通路真的活着
    vw_crumb("d=update-enter");
    uint32_t unext = 0;
    const int upd = vw_fifo_update(&bus, mems, fmax, 0u, 0u, 64u, 64u, &unext);
    vw_crumb("e=update-done");
    vw_begin(); vw_puts("[VMWARE] fifo update(0,0,64,64) ");
    vw_puts(upd ? "sync=ok" : "sync=TIMEOUT");
    vw_puts(" next="); vw_num(unext); vw_puts(" max="); vw_num(fmax); vw_end();
    return upd;
}
