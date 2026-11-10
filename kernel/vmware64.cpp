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
#include "fb.h"            // fb64_lfb_phys64 / fb_phys_width / fb_phys_height / fb64_lfb_usable64
#include "vfs64.h"         // vfs64_stat（opt-in 探针）
#include "x86_64.h"        // g_ticks64 / PIT_HZ_64（有界等待 / 探针时间窗）

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
#define VMW_REG_DEPTH       6u
#define VMW_REG_RED_MASK    9u
#define VMW_REG_GREEN_MASK  10u
#define VMW_REG_BLUE_MASK   11u
#define VMW_REG_FB_OFFSET   14u      // ★ v0.9.35：可见区相对 FB_START 的偏移（一直没读过）
#define VMW_REG_FB_SIZE     16u

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
// ★ v0.9.35：几何全量 dump（新行；上面那行老格式一字不动，既有断言不受影响）。
//   为什么要它：v0.9.34 把缓存属性、UPDATE、ENABLE 三条都实测否掉之后，剩下唯一的
//   候选是"我们写的地址与宿主扫出的地址不是同一块"。`FB_OFFSET` 正是这个差值的来源，
//   而之前在父行里**只读了 FB_START，从没读过 FB_OFFSET / FB_SIZE**。
//   [VMWARE] gpu geom fb_start=0xE8000000 fb_offset=0 fb_size=3145728 bpp=32 bytes_line=4096 vram=134217728
    {
        const uint32_t fboff = vw_reg_rd(&bus, VMW_REG_FB_OFFSET);
        const uint32_t fbsz  = vw_reg_rd(&bus, VMW_REG_FB_SIZE);
        vw_begin(); vw_puts("[VMWARE] gpu geom fb_start=0x"); vw_hex(fbs);
        vw_puts(" fb_offset=");   vw_num(fboff);
        vw_puts(" fb_size=");     vw_num(fbsz);
        vw_puts(" bpp=");         vw_num(bpp);
        vw_puts(" bytes_line=");  vw_num(stride);
        vw_puts(" depth=");       vw_num(vw_reg_rd(&bus, VMW_REG_DEPTH));
        vw_puts(" masks=");       vw_hex(vw_reg_rd(&bus, VMW_REG_RED_MASK));
        vw_puts(",");             vw_hex(vw_reg_rd(&bus, VMW_REG_GREEN_MASK));
        vw_puts(",");             vw_hex(vw_reg_rd(&bus, VMW_REG_BLUE_MASK));
        vw_puts(" vram=");        vw_num(vram);
        vw_puts(" lfb=0x");       vw_hex(fb64_lfb_phys64());
        vw_end();
    }
    return 1;
}

// ==================== 阶段 1（v0.9.19）：真正把设备驱动起来 ====================
// 为什么分阶段：读寄存器只是"看"，`SVGA3D` 要先把 **FIFO** 配好并把命令递进去。
// 这一步做的是：配 FIFO -> CONFIG_DONE -> SYNC/BUSY 往返 -> ENABLE -> UPDATE 提交。
// 两个前提（缺一就不做，绝不拿显示去赌）：
//   ① 卷里有 `/etc/gpu3d_probe`（opt-in：默认启动一个寄存器都不写）
//   ② `SVGA_REG_FB_START` == 我们当前正在用的帧缓冲物理地址（否则 ENABLE 会换屏，可能直接黑屏）

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

// ★ v0.9.36：**字节口径**的 FIFO（vmsvga 规范就是这么定的）。
//   SVGA_FIFO_MIN/MAX/NEXT_CMD/STOP 全部是**字节**偏移，头部（MIN/MAX/NEXT/STOP/CAPS 共 5 个
//   dword = 20 字节）之外的第一个可用位置是 8192（规范里保留区）。之前的两个口径都把 MIN 写成
//   16，命令落在保留区里 —— 宿主照收（BUSY 回 0）但可以完全不理会，这正好解释了
//   `updates n=64 busy=0` 却没有任何画面变化。
#define VMW_FIFO_MIN_BYTES   8192u
#define VMW_CMD_BYTES        20u          // 一条 UPDATE = 5 个 dword

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

// ==================== v0.9.37：真 SVGA 呈现通路（screen object + GMRFB + 命令流）====================
// 依据 = svga_reg.h（Linux vmwgfx device_include）+ vmwgfx 的三条参考路径：
//   vmw_fifo_init()（FIFO 口径）、vmw_fifo_ping_host()（敲门）、vmw_sou_*（定义屏 + GMRFB blit）。
// 与 v0.9.34～v0.9.36 的四处差别（老写法都停在"命令写进 FIFO 了"这一步）：
//   ① 寄存器号按规范对齐：GMR_ID / GMR_DESCRIPTOR / GMR_MAX_IDS / GMR_MAX_DESC 是
//      **41 / 42 / 43 / 44**。25..28 是 CURSOR_X / CURSOR_Y / CURSOR_ON / HOST_BITS_PER_PIXEL
//      —— 写成 25..28 会去动硬件光标。
//   ② FIFO 的 MIN 取 **SVGA_REG_MEM_REGS<<2**（不足一页取 4096，vmwgfx 同口径）；并且
//      **把 fifo[SVGA_FIFO_BUSY]（索引 290）清零** —— v0.9.36 只写了 MIN/MAX/NEXT/STOP。
//   ③ 提交之后真正的"敲门"是 **fifo[SVGA_FIFO_BUSY]=1 然后再写 SVGA_REG_SYNC**
//      （vmw_fifo_ping_host：cmpxchg(BUSY,0,1)==0 才写 SYNC）。v0.9.36 只写 SYNC、BUSY 恒为 0，
//      宿主看到 BUSY=0 会当成"没有待办"直接返回 —— 这最可能就是
//      `updates n=64 busy=0` 却屏上什么都没有的根因。
//   ④ 呈现改走规范的对象通路：`SVGA_CMD_DEFINE_SCREEN` 定义主屏（backing store =
//      SVGA_GMR_FRAMEBUFFER，也就是我们正在写的那块显存），之后每块脏矩形走
//      `SVGA_CMD_DEFINE_GMRFB` + `SVGA_CMD_BLIT_GMRFB_TO_SCREEN`（vmwgfx vmw_sou_* 的序列）。
//      DEFINE_SCREEN 没做成时**回退**到老的 SVGA_CMD_UPDATE 路径（绝不静默什么都不发）。

// --- 寄存器（svga_reg.h 的 enum，全部按规范编号）---
#define VMW_REG_MEM_REGS        30u      // FIFO 头部保留区（单位：dword）
#define VMW_REG_NUM_DISPLAYS    31u
#define VMW_REG_PITCHLOCK       32u
#define VMW_REG_GMR_ID          41u
#define VMW_REG_GMR_DESCRIPTOR  42u
#define VMW_REG_GMR_MAX_IDS     43u
#define VMW_REG_GMR_MAX_DESC    44u
#define VMW_REG_TRACES          45u
#define VMW_REG_FIFO_CAPS       68u
#define VMW_REG_FENCE           69u      // 宿主处理到 SVGA_CMD_FENCE 后把值写回这里（可回读！）

// --- FIFO 头（MEM 区里的 uint32 索引）---
#define VMW_FIFO_MIN            0u
#define VMW_FIFO_MAX            1u
#define VMW_FIFO_NEXT_CMD       2u
#define VMW_FIFO_STOP           3u
#define VMW_FIFO_CAPABILITIES   4u
#define VMW_FIFO_FLAGS          5u
#define VMW_FIFO_RESERVED       14u
#define VMW_FIFO_BUSY           290u     // ★ 敲门闩：0 = 空闲，1 = 有待处理命令
#define VMW_FIFO_MIN_PAGE       4096u    // vmwgfx：min < PAGE_SIZE 时取 PAGE_SIZE

#define VMW_SYNC_GENERIC        1u

// --- 命令（SVGAFifoCmdId）---
#define VMW_CMD_UPDATE                 1u
#define VMW_CMD_DEFINE_SCREEN         34u
#define VMW_CMD_DESTROY_SCREEN        35u
#define VMW_CMD_FENCE                 30u
#define VMW_CMD_DEFINE_GMRFB          36u
#define VMW_CMD_BLIT_GMRFB_TO_SCREEN  37u

#define VMW_GMR_FRAMEBUFFER     0xFFFFFFFEu   // 指向设备自己的帧缓冲
#define VMW_SCREEN_MUST_BE_SET  1u
#define VMW_SCREEN_IS_PRIMARY   2u
#define VMW_SCREEN_ID_PRIMARY   0u

// 呈现状态。全部静态：内核里没有动态分配的习惯（也不该有）。
struct VwFifo {
    volatile uint32_t* mem;
    uint32_t min;          // 字节
    uint32_t max;          // 字节
    uint32_t next;         // 字节（NEXT_CMD 的镜像，由我们维护）
    int      ok;
};
static VwFifo  g_pres;
static VmwBus  g_pres_bus;
static int      g_pres_screen  = 0;    // 1 = screen object 已定义（走 GMRFB blit）
static int      g_pres_enabled = 0;    // 1 = SVGA_REG_ENABLE 已置 1
static int      g_pres_kick    = 1;    // 1 = 提交后敲 FIFO_BUSY + SYNC
static uint32_t g_pres_pitch   = 0;    // 每行字节
static uint32_t g_pres_w = 0, g_pres_h = 0;
static uint32_t g_pres_n = 0;          // 累计提交块数
static uint32_t g_pres_drop = 0;       // 因 FIFO 满/宿主不消费而丢掉的块数
static uint64_t g_pres_full_next = 0;  // 呈现心跳：下一次"整屏标脏"的 tick（4 Hz）
static uint32_t g_pres_tick_n = 0;     // 心跳跑了多少次（取证：证明定时器真的在推进）
static uint64_t g_pres_regs_next = 0;  // 下一次回读关键寄存器的 tick（30 s）
static int      g_pres_fill_periodic = 0;   // 1 = 每 8 秒给屏刷 1 秒品红（呈现通路自检）

// 环形 FIFO 剩余空间（字节）。STOP 由宿主维护（它消费到哪儿），NEXT_CMD 由我们维护。
static uint32_t vw_fifo_space(void) {
    const uint32_t min = g_pres.min, max = g_pres.max;
    if (max <= min + 64u) return 0u;
    uint32_t stop = g_pres.mem[VMW_FIFO_STOP];
    if (stop < min || stop > max) stop = min;          // 宿主写了怪值：当作"没消费"
    const uint32_t cap = max - min;
    uint32_t next = g_pres.next;
    if (next < min || next > max) next = min;
    const uint32_t used = (next >= stop) ? (next - stop) : (cap - (stop - next));
    if (used >= cap) return 0u;
    return cap - used;
}

// 追加一条命令（nwords 个 dword，含命令字）。允许跨环（按 dword 推进，和 vmwgfx 的慢路径同语义）。
// 返回 1 = 写进去了，0 = 空间不够（调用方计入 drop，不阻塞）。
static int vw_emit(const uint32_t* w, uint32_t nwords) {
    if (!g_pres.ok || nwords == 0u) return 0;
    const uint32_t bytes = nwords * 4u;
    if (bytes == 0u || bytes >= (g_pres.max - g_pres.min)) return 0;
    if (vw_fifo_space() < bytes) { g_pres_drop++; return 0; }
    volatile uint32_t* m = g_pres.mem;
    uint32_t off = g_pres.next;
    if (off < g_pres.min || off + 4u > g_pres.max) off = g_pres.min;
    for (uint32_t i = 0; i < nwords; i++) {
        m[off >> 2] = w[i];
        off += 4u;
        if (off >= g_pres.max) off = g_pres.min;
    }
    __asm__ volatile("mfence" ::: "memory");
    m[VMW_FIFO_NEXT_CMD] = off;
    g_pres.next = off;
    return 1;
}

// ★ 真正让宿主"开始干活"的那一下。BUSY != 0 说明宿主正在处理，它自己会回头再扫一遍。
static void vw_kick(void) {
    if (!g_pres.ok) return;
    if (g_pres.mem[VMW_FIFO_BUSY] == 0u) {
        g_pres.mem[VMW_FIFO_BUSY] = 1u;
        __asm__ volatile("mfence" ::: "memory");
        vw_reg_wr(&g_pres_bus, VMW_REG_SYNC, VMW_SYNC_GENERIC);
    }
}

static void vw_cmd_update(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t c[5];
    c[0] = VMW_CMD_UPDATE; c[1] = x; c[2] = y; c[3] = w; c[4] = h;
    (void)vw_emit(c, 5u);
}
static void vw_cmd_gmrfb(void) {
    uint32_t c[5];
    c[0] = VMW_CMD_DEFINE_GMRFB;
    c[1] = VMW_GMR_FRAMEBUFFER;                 // ptr.gmrId：设备自己的帧缓冲
    c[2] = 0u;                                  // ptr.offset
    c[3] = g_pres_pitch;                        // bytesPerLine（字节）
    c[4] = 32u | (24u << 8);                    // bitsPerPixel=32 / colorDepth=24（RGBA 也按 24 上报，vmwgfx 同样处理）
    (void)vw_emit(c, 5u);
}
static void vw_cmd_blit(int32_t sx, int32_t sy, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    uint32_t c[8];
    c[0] = VMW_CMD_BLIT_GMRFB_TO_SCREEN;
    c[1] = (uint32_t)sx; c[2] = (uint32_t)sy;
    c[3] = (uint32_t)x1; c[4] = (uint32_t)y1; c[5] = (uint32_t)x2; c[6] = (uint32_t)y2;
    c[7] = VMW_SCREEN_ID_PRIMARY;
    (void)vw_emit(c, 8u);
}
// SVGAScreenObject 是 packed 44 字节 = 11 个 dword；加命令字正好 12 个 dword。
static void vw_cmd_screen(uint32_t w, uint32_t h, uint32_t pitch) {
    uint32_t c[12];
    c[0]  = VMW_CMD_DEFINE_SCREEN;
    c[1]  = 44u;                                            // structSize
    c[2]  = VMW_SCREEN_ID_PRIMARY;                          // id
    c[3]  = VMW_SCREEN_MUST_BE_SET | VMW_SCREEN_IS_PRIMARY; // flags
    c[4]  = w;  c[5] = h;                                   // size
    c[6]  = 0u; c[7] = 0u;                                  // root
    c[8]  = VMW_GMR_FRAMEBUFFER; c[9] = 0u;                 // backingStore.ptr
    c[10] = pitch; c[11] = 0u;                              // backingStore.pitch / cloneCount
    (void)vw_emit(c, 12u);
}

// 诊断：SVGA_CMD_FRONT_ROP_FILL（29）直接往"屏的 front buffer"刷一块纯色，绕开 GMRFB/帧缓冲的
// 读取 —— 它是"呈现通路本身通不通"的唯一干净探针。rop 用 SVGA_ROP_COPY(0xCC)。
#define VMW_CMD_FRONT_ROP_FILL 29u
#define VMW_ROP_COPY           0xCCu
static uint64_t g_pres_fill_until = 0;      // 0 = 关；非 0 = 到这个 tick 之前每帧刷一次品红

static void vw_cmd_front_fill(uint32_t color, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t c[7];
    c[0] = VMW_CMD_FRONT_ROP_FILL;
    c[1] = color; c[2] = x; c[3] = y; c[4] = w; c[5] = h; c[6] = VMW_ROP_COPY;
    (void)vw_emit(c, 7u);
}

// 诊断台阶（只有卷里有 /etc/gpu3d_stage 才启用）：每 10 秒上一级，宿主侧连续抓帧 + 串口台阶行
// 就能把"哪一级开始出画面"对上：
//   A：UPDATE，不敲门，不 ENABLE   （= v0.9.36 的行为，实测 99.87% 黑）
//   B：UPDATE + FIFO_BUSY 敲门      （不开 ENABLE）
//   C：再加 SVGA_REG_ENABLE         （之后不再降级）
// 默认启动**不**走台阶：直接就是 C 的行为（敲门 + ENABLE + RegisterMode 的 UPDATE）。
static int      g_ladder_on = 0;
static uint64_t g_ladder_t0 = 0;
static int      g_ladder_stage = -1;

static void vw_ladder_tick(void) {
    if (!g_ladder_on || !g_pres.ok) return;
    const uint64_t dt = g_ticks64 - g_ladder_t0;
    const uint64_t step = (uint64_t)PIT_HZ_64 * 10u;
    const int s = (dt < step) ? 0 : (dt < step * 2u) ? 1 : 2;
    if (s == g_ladder_stage) return;
    g_ladder_stage = s;
    if (s == 0) {
        g_pres_kick = 0;
    } else if (s == 1) {
        g_pres_kick = 1;
    } else {
        g_pres_kick = 1;
        if (!g_pres_enabled) {
            vw_reg_wr(&g_pres_bus, VMW_REG_ENABLE, 1u);
            const uint32_t en = vw_reg_rd(&g_pres_bus, VMW_REG_ENABLE);
            g_pres_enabled = (en == 1u) ? 1 : 0;
            vw_begin(); vw_puts("[VMWARE] stage C enable="); vw_num(en); vw_end();
        }
    }
    vw_begin(); vw_puts("[VMWARE] stage s=");
    vw_ch((s == 0) ? 'A' : (s == 1) ? 'B' : 'C');
    vw_puts(" kick="); vw_num((uint64_t)g_pres_kick);
    vw_puts(" enable="); vw_num((uint64_t)g_pres_enabled);
    vw_puts(" dt_ms="); vw_num(dt * (uint64_t)TICK_MS_64); vw_end();
}

int vmware64_present_init64(void) {
    g_pres.ok = 0; g_pres_screen = 0; g_pres_enabled = 0; g_pres_n = 0; g_pres_drop = 0;
    g_pres_kick = 1; g_ladder_on = 0; g_ladder_stage = -1; g_pres_full_next = 0;
    if (!g_vmw_ok || g_vmw_bdf == 0xFFFFFFFFu) {
        vw_begin(); vw_puts("[VMWARE] present skipped reason=no-svga"); vw_end();
        return 0;
    }
    const uint32_t b0 = pci64_cfg_rd32(g_vmw_bdf, 0x10u);
    if (!(b0 & 1u)) {                                   // BAR0 必须是 I/O（VMware 的常态）
        vw_begin(); vw_puts("[VMWARE] present skipped reason=bar0-not-io"); vw_end();
        return 0;
    }
    VmwBus bus;
    bus.is_io = 1;
    bus.base = (uint64_t)(b0 & ~0x3u);
    const uint32_t mems   = vw_reg_rd(&bus, VMW_REG_MEM_START);
    const uint32_t meml   = vw_reg_rd(&bus, VMW_REG_MEM_SIZE);
    const uint32_t fbs    = vw_reg_rd(&bus, VMW_REG_FB_START);
    const uint64_t lfb    = fb64_lfb_phys64();
    const uint32_t w      = vw_reg_rd(&bus, VMW_REG_WIDTH);
    const uint32_t h      = vw_reg_rd(&bus, VMW_REG_HEIGHT);
    const uint32_t stride = vw_reg_rd(&bus, VMW_REG_BYTES_LINE);
    if (!mems || !meml || !fbs) {
        vw_begin(); vw_puts("[VMWARE] present skipped reason=no-fifo-regs"); vw_end();
        return 0;
    }
    // 只有"SVGA 的 FB 就是我们正在写的 LFB、且几何一致"才动手 —— 否则宿主会去搬一块不是我们画的
    // 内存（画面直接错乱）。这是 stage1 早就定下的同一条安全闸门。
    if ((uint64_t)fbs != lfb || w != (uint32_t)fb_phys_width() || h != (uint32_t)fb_phys_height()) {
        vw_begin(); vw_puts("[VMWARE] present skipped reason=fb-or-geom-mismatch fb=0x");
        vw_hex(fbs); vw_puts(" lfb=0x"); vw_hex(lfb);
        vw_puts(" svga="); vw_num(w); vw_puts("x"); vw_num(h); vw_end();
        return 0;
    }
    // MEM 区必须落在恒等直映的低 4 GiB（我们直接用物理地址当指针）
    if ((uint64_t)mems + (uint64_t)meml > 0x100000000ull || meml < 8192u) {
        vw_begin(); vw_puts("[VMWARE] present skipped reason=mem-unusable mem=0x");
        vw_hex(mems); vw_puts(" len="); vw_num(meml); vw_end();
        return 0;
    }
    // ---- FIFO 初始化（vmwgfx vmw_fifo_init 的口径：MIN 从 MEM_REGS 来，BUSY 清零，字节单位）----
    const uint32_t mem_regs = vw_reg_rd(&bus, VMW_REG_MEM_REGS);
    uint32_t min = (mem_regs > 0u && mem_regs < 4096u) ? (mem_regs << 2) : 0u;
    if (min < VMW_FIFO_MIN_PAGE) min = VMW_FIFO_MIN_PAGE;
    if (min + 256u >= meml) {
        vw_begin(); vw_puts("[VMWARE] present skipped reason=fifo-too-small min=");
        vw_num(min); vw_puts(" max="); vw_num(meml); vw_end();
        return 0;
    }
    volatile uint32_t* fifo = (volatile uint32_t*)(uintptr_t)mems;
    fifo[VMW_FIFO_MIN]  = min;
    fifo[VMW_FIFO_MAX]  = meml;
    __asm__ volatile("mfence" ::: "memory");
    fifo[VMW_FIFO_NEXT_CMD] = min;
    fifo[VMW_FIFO_STOP]     = min;
    fifo[VMW_FIFO_BUSY]     = 0u;          // ★ vmwgfx 在 CONFIG_DONE 之前清零这一格
    fifo[VMW_FIFO_FLAGS]    = 0u;
    __asm__ volatile("mfence" ::: "memory");
    vw_reg_wr(&bus, VMW_REG_CONFIG_DONE, 1u);
    const uint32_t rmin  = fifo[VMW_FIFO_MIN];
    const uint32_t rmax  = fifo[VMW_FIFO_MAX];
    const uint32_t rnext = fifo[VMW_FIFO_NEXT_CMD];
    if (rmin < 64u || rmin + 256u >= rmax) {          // 宿主把 MIN/MAX 打回来 = 它不收这套口径
        vw_begin(); vw_puts("[VMWARE] present skipped reason=fifo-rejected min=");
        vw_num(rmin); vw_puts(" max="); vw_num(rmax); vw_puts(" next="); vw_num(rnext); vw_end();
        return 0;
    }
    g_pres.mem  = fifo;
    g_pres.min  = rmin;
    g_pres.max  = rmax;
    g_pres.next = (rnext >= rmin && rnext < rmax) ? rnext : rmin;
    g_pres.ok   = 1;
    g_pres_bus  = bus;
    g_pres_w    = w;
    g_pres_h    = h;
    g_pres_pitch = (stride >= w * 4u && (stride & 3u) == 0u) ? stride : w * 4u;

    // 诊断模式：只配 FIFO，其余交给台阶（/etc/gpu3d_stage）
    {
        uint32_t t = 0, sz = 0;
        if (vfs64_stat("/etc/gpu3d_stage", &t, &sz) == 0) {
            g_ladder_on = 1;
            g_ladder_t0 = g_ticks64;
            vw_begin(); vw_puts("[VMWARE] present ok=1 path=ladder fifo min=");
            vw_num(rmin); vw_puts(" max="); vw_num(rmax);
            vw_puts(" memregs="); vw_num(mem_regs);
            vw_puts(" pitch="); vw_num(g_pres_pitch);
            vw_puts(" enable=0 screen=0 kick=0 stage=A"); vw_end();
            return 1;
        }
    }

    // ---- SVGA_REG_ENABLE：告诉宿主"客人驱动接管显示"（几何没变，所以不会换模式）----
    vw_reg_wr(&bus, VMW_REG_ENABLE, 1u);
    const uint32_t en = vw_reg_rd(&bus, VMW_REG_ENABLE);
    g_pres_enabled = (en == 1u) ? 1 : 0;
    // ENABLE 之后宿主可能重置 FIFO 簿记（v0.9.20 实测）：只在被打回时才补写，不无脑重配。
    {
        const uint32_t n2 = fifo[VMW_FIFO_NEXT_CMD];
        const uint32_t m2 = fifo[VMW_FIFO_MIN];
        if (m2 < 64u || m2 + 256u >= rmax || n2 < m2 || n2 >= rmax) {
            fifo[VMW_FIFO_MIN] = rmin;
            fifo[VMW_FIFO_MAX] = rmax;
            __asm__ volatile("mfence" ::: "memory");
            fifo[VMW_FIFO_NEXT_CMD] = rmin;
            fifo[VMW_FIFO_STOP]     = rmin;
            fifo[VMW_FIFO_BUSY]     = 0u;
            __asm__ volatile("mfence" ::: "memory");
            g_pres.next = rmin;
        } else {
            g_pres.next = n2;
        }
    }
    // ★ 默认走 **RegisterMode**（不定义 screen object，只用 SVGA_CMD_UPDATE 报告脏矩形）。
    //   依据（v0.9.37 第一轮实测 + 宿主日志）：我们 DEFINE_SCREEN id=0 之后，宿主确实切到了
    //     `SVGA-ScreenMgr: Screen type changed to ScreenObject`
    //   但它紧接着又 `SWBScreen: Screen 1 Destroyed/Defined … flags=0x2` —— 它保留了自己那块
    //   legacy 屏（id 1，primary），我们定义的 screen 0 没有任何人显示，所以还是黑。
    //   RegisterMode 才是"没有 VMware Tools 时宿主的原生呈现路径"：SWBScreen 直接读帧缓冲，
    //   脏矩形由 UPDATE 声明。配上本轮修好的"敲门"（fifo[FIFO_BUSY]=1 + SYNC）才是最短路。
    //   想复现 screen object 通路（含前 4 秒品红自检）：卷里放 /etc/gpu3d_screen。
    {
        uint32_t t = 0, sz = 0;
        if (vfs64_stat("/etc/gpu3d_screen", &t, &sz) == 0) {
            vw_cmd_screen(w, h, g_pres_pitch);
            g_pres_screen = 1;
            g_pres_fill_until = g_ticks64 + (uint64_t)PIT_HZ_64 * 4u;   // 头 4 秒连续刷
            g_pres_fill_periodic = 1;                                   // 之后每 8 秒刷 1 秒
        }
    }
    vw_kick();
    (void)vw_wait_idle(&bus, 400u);        // 有界（约 12 ms 封顶），只为了让第一屏尽快上屏
    const uint32_t stop0 = fifo[VMW_FIFO_STOP];
    vw_begin(); vw_puts("[VMWARE] present ok=1 path=");
    vw_puts(g_pres_screen ? "screen+gmrfb" : "register+update");
    vw_puts(" fifo min=");
    vw_num(rmin); vw_puts(" max="); vw_num(rmax);
    vw_puts(" memregs="); vw_num(mem_regs);
    vw_puts(" pitch="); vw_num(g_pres_pitch);
    vw_puts(" enable="); vw_num(en);
    vw_puts(" screen="); vw_num((uint64_t)g_pres_screen);
    vw_puts(" next="); vw_num(g_pres.next);
    vw_puts(" stop="); vw_num(stop0);
    vw_puts(" units=bytes kick=fifo-busy"); vw_end();
    // ★ 宿主到底有没有**执行**我们的 FIFO 命令？SVGA_CMD_FENCE + 回读 SVGA_REG_FENCE：
    //   规范里宿主处理到 FENCE 就把值写进 SVGA_REG_FENCE。回读一致 = 命令真的被执行；
    //   回读不到 = 命令被"消费掉但跳过"（那 STOP 前进就只是簿记，不代表任何效果）。
    {
        uint32_t c[2];
        c[0] = VMW_CMD_FENCE; c[1] = 0x0000A5A5u;
        (void)vw_emit(c, 2u);
        vw_kick();
        uint32_t got = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < 3000u; i++) {
            got = vw_reg_rd(&bus, VMW_REG_FENCE);
            if (got == 0x0000A5A5u) break;
            for (uint32_t k = 0; k < 4096u; k++) __asm__ volatile("pause");
        }
        vw_begin(); vw_puts("[VMWARE] present fence want=0xa5a5 got=");
        vw_hex(got);
        vw_puts((got == 0x0000A5A5u) ? " ok=1" : " ok=0"); vw_end();
    }
    return 1;
}

// 提交一块脏矩形。**不阻塞**：写命令 + 敲门；每 64 块做一次有界 BUSY 等待并打点
// （打点里的 stop/next/drop 就是"宿主到底有没有在消费"的取证）。
void vmware64_update64(int x, int y, int w, int h) {
    if (!g_pres.ok) return;
    if (w <= 0 || h <= 0) return;
    const int W = (int)g_pres_w, H = (int)g_pres_h;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    // 前 8 次一律整屏：初始化之前已经画进 LFB 的内容（开机屏 / 锁屏 / 壁纸）都在里面，
    // 它们当时还没有提交通道，靠这几次整屏提交补发出去；之后才走逐脏矩形。
    if (g_pres_n < 8u) { x = 0; y = 0; w = W; h = H; }
    // ★ 呈现心跳**不在这里**：update64 只在"有东西被画"时被调用，静止桌面几乎不调它。
    //   2 Hz 整屏标脏由 vmware64_present_tick64() 在 kheart 线程里驱动（见下面那个函数）。
    if (g_ladder_on) vw_ladder_tick();
    // 空间不够：先让一拍（有界），还不够就丢这块（drop 计数会进打点，取证用）
    if (vw_fifo_space() < 16u * 4u) {
        for (uint32_t k = 0; k < 4096u; k++) __asm__ volatile("pause");
    }
    if (g_pres_screen) {
        vw_cmd_gmrfb();
        vw_cmd_blit(x, y, x, y, x + w, y + h);
    } else {
        vw_cmd_update((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
    }
    // 诊断（只在 /etc/gpu3d_screen 模式下、且只在前 4 秒）：给 screen 0 的 front buffer 刷一条
    // 品红。用途只有一个 —— 把"呈现通路本身通不通"和"宿主读到的像素是不是我们的"分开：
    //   * 屏上出品红   => 呈现通路通，问题在"读 GMRFB/帧缓冲"这一步；
    //   * 屏上还是黑   => 连"我们让它画"都不成立，得换通路（寄存器/模式/ENABLE）。
    if (g_pres_fill_until && g_ticks64 < g_pres_fill_until) {
        vw_cmd_front_fill(0x00FF00FFu, 0, 0, W, H);
    }
    if (g_pres_kick) vw_kick();
    g_pres_n++;
    if ((g_pres_n & 63u) == 0u) {
        const int idle = vw_wait_idle(&g_pres_bus, 400u);
        vw_begin(); vw_puts("[VMWARE] present n="); vw_num(g_pres_n);
        vw_puts(" stop="); vw_num(g_pres.mem[VMW_FIFO_STOP]);
        vw_puts(" next="); vw_num(g_pres.next);
        vw_puts(" drop="); vw_num(g_pres_drop);
        vw_puts(" busy="); vw_num(idle ? 0u : 1u);
        vw_puts(" used="); vw_num((uint64_t)(g_pres.max - g_pres.min) - (uint64_t)vw_fifo_space());
        vw_puts(" tick="); vw_num(g_pres_tick_n);
        vw_end();
    }
}

// ==================== v0.9.37：呈现心跳（定时器驱动，不依赖"有没有人画"）====================
// 为什么必须是定时器：v0.9.37 第三轮实测——`[FB64] mix` 在**桌面阶段**给出
//   `p0=F8F9FB/F8F9FB p2=0078D7/0078D7`（壁纸色 + Dock 强调蓝，bb 与 lfb 完全一致），
//   也就是客人帧缓冲里就是正确桌面；而宿主侧仍然是 99.87% 纯黑。
//   锁屏阶段宿主能正常显示（抓到 black=0.00%），差别只有一个：锁屏有动画 -> UPDATE 密集。
//   ⇒ 宿主是"按 UPDATE 声明的矩形"把客人帧缓冲搬进影子屏。静止桌面没人提交，那块内容就
//     永远停在"登录瞬间被清黑"的版本上。所以心跳必须由**定时器**推进，不能挂在
//     update64（它只在有绘制时被调用，静止桌面几乎不调）。
// 由 kheart 线程每约 60 ms 调一次，内部限到 4 Hz：一次 20 字节命令换"画面一定会收敛"。
void vmware64_present_tick64(void) {
    if (!g_pres.ok) return;
    // ---- 每 30 秒把关键寄存器回读一遍 ----
    // 为什么要它：如果宿主在"登录/切桌面"那一刻改了扫描偏移（FB_OFFSET/PITCHLOCK）或几何，
    // 我们写的位置就不再是它读的位置 —— 回读是唯一能在客人侧看到这件事的办法。
    if (g_ticks64 >= g_pres_regs_next) {
        g_pres_regs_next = g_ticks64 + (uint64_t)PIT_HZ_64 * 30u;
        vw_begin(); vw_puts("[VMWARE] present regs fb_offset=");
        vw_num(vw_reg_rd(&g_pres_bus, VMW_REG_FB_OFFSET));
        vw_puts(" pitchlock="); vw_num(vw_reg_rd(&g_pres_bus, VMW_REG_PITCHLOCK));
        vw_puts(" w=");         vw_num(vw_reg_rd(&g_pres_bus, VMW_REG_WIDTH));
        vw_puts(" h=");         vw_num(vw_reg_rd(&g_pres_bus, VMW_REG_HEIGHT));
        vw_puts(" bpp=");       vw_num(vw_reg_rd(&g_pres_bus, VMW_REG_BPP));
        // ★ 直接读 LFB 的两个点（壁纸 / Dock 位置）：这是"客人显存里到底有没有桌面"的唯一当场证据。
        //   之前只能靠 [FB64] mix（它只挂 flip 路径，静止桌面之后就不采了），所以拿不到"几秒之后
        //   帧缓冲还是不是桌面"。有了这两格，日志自己就能回答"该看见的东西在不在显存里"。
        {
            volatile uint32_t* lf = (volatile uint32_t*)(uintptr_t)fb64_lfb_phys64();
            if (lf && g_pres_pitch && g_pres_h > 700u) {
                const uint32_t wpx = g_pres_pitch >> 2;
                vw_puts(" lfbtl=");   vw_hex((uint64_t)(lf[20u * wpx + 20u] & 0x00FFFFFFu));
                vw_puts(" lfbdock="); vw_hex((uint64_t)(lf[700u * wpx + (g_pres_w >> 1)] & 0x00FFFFFFu));
            }
        }
        vw_puts(" enable=");    vw_num(vw_reg_rd(&g_pres_bus, VMW_REG_ENABLE));
        vw_puts(" fence=");     vw_hex(vw_reg_rd(&g_pres_bus, VMW_REG_FENCE));
        vw_puts(" tick=");      vw_num(g_pres_tick_n);
        vw_end();
    }
    if (g_ticks64 < g_pres_full_next) return;
    g_pres_full_next = g_ticks64 + (uint64_t)PIT_HZ_64 / 4u;      // 4 Hz
    g_pres_tick_n++;
    const int W = (int)g_pres_w, H = (int)g_pres_h;
    if (W <= 0 || H <= 0) return;
    if (g_pres_screen) {
        vw_cmd_gmrfb();
        vw_cmd_blit(0, 0, 0, 0, W, H);
    } else {
        vw_cmd_update(0u, 0u, (uint32_t)W, (uint32_t)H);
    }
    // 呈现通路自检（只在 /etc/gpu3d_screen 模式）：每 8 秒的头 1 秒把屏刷成品红。
    // 品红是**宿主自己画的颜色**，不读我们的帧缓冲 —— 它出现就说明"命令被执行 + 呈现通路通"，
    // 那么"桌面看不见"就只剩"宿主读不到我们写的像素"这一种解释。
    if (g_pres_fill_periodic) {
        const uint64_t win = (uint64_t)PIT_HZ_64 * 8u;
        if ((g_ticks64 % win) < (uint64_t)PIT_HZ_64) {
            vw_cmd_front_fill(0x00FF00FFu, 0, 0, (uint32_t)W, (uint32_t)H);
        }
    }
    if (g_pres_kick) vw_kick();
    g_pres_n++;
}

// ==================== v0.9.35：LFB 可见性二分探针 ====================
// 自己的标记（不跟 /etc/gpu3d_probe 共用）：实测这个探针每秒 5 次刷整屏会把宿主 VNC 编码器
// 打瘫（之后抓帧全部超时），所以想抓帧取证时必须把它关掉。
static int      g_probe_on = 0;
static uint64_t g_probe_until = 0;
static uint32_t g_probe_paints = 0;
static uint64_t g_probe_next_log = 0;

int vmware64_lfbprobe_begin64(void) {
    g_probe_on = 0;
    uint32_t t = 0, sz = 0;
    if (vfs64_stat("/etc/gpu3d_lfbprobe", &t, &sz) != 0) return 0;
    if (!g_vmw_ok || !fb64_lfb_usable64()) return 0;
    g_probe_until = g_ticks64 + (uint64_t)PIT_HZ_64 * 20u;    // 20 秒窗口
    g_probe_paints = 0;
    g_probe_next_log = g_ticks64;
    g_probe_on = 1;
    vw_begin(); vw_puts("[VMWARE] lfbprobe on stripes=8 window_ms=");
    vw_num((uint64_t)PIT_HZ_64 * 20u * (uint64_t)TICK_MS_64);
    vw_puts(" lfb=0x"); vw_hex(fb64_lfb_phys64());
    vw_puts(" geom="); vw_num((uint64_t)fb_phys_width()); vw_puts("x");
    vw_num((uint64_t)fb_phys_height()); vw_end();
    return 1;
}

// 直接在 LFB 上刷 8 条竖条（饱和色）+ 顶部 16 行黑条做"方向标"。
// 不经过后备缓冲、不动裁剪矩形 —— 这一笔就是纯粹的"我往显存写，宿主看得见吗"。
void vmware64_lfbprobe_tick64(void) {
    if (!g_probe_on) return;
    if (g_ticks64 > g_probe_until) {
        g_probe_on = 0;
        vw_begin(); vw_puts("[VMWARE] lfbprobe done paints="); vw_num(g_probe_paints); vw_end();
        return;
    }
    const int w = fb_phys_width(), h = fb_phys_height();
    if (w <= 0 || h <= 0) return;
    volatile uint32_t* lfb = (volatile uint32_t*)(uintptr_t)fb64_lfb_phys64();
    if (!lfb) return;
    static const uint32_t col[8] = {
        0x00FF0000u, 0x00FF8000u, 0x00FFFF00u, 0x0000FF00u,
        0x0000FFFFu, 0x000000FFu, 0x00FF00FFu, 0x00FFFFFFu
    };
    for (int y = 0; y < h; y++) {
        volatile uint32_t* row = lfb + (uint64_t)y * (uint64_t)w;
        for (int x = 0; x < w; x++) {
            row[x] = (y < 16) ? 0x00000000u : col[(x * 8) / w];   // 顶部 16 行留黑做方向标
        }
    }
    __asm__ volatile("sfence" : : : "memory");
    g_probe_paints++;
    if (g_ticks64 >= g_probe_next_log) {
        g_probe_next_log = g_ticks64 + (uint64_t)PIT_HZ_64;      // 每秒一行
        vw_begin(); vw_puts("[VMWARE] lfbprobe paint="); vw_num(g_probe_paints);
        vw_puts(" t="); vw_num(g_ticks64); vw_end();
    }
}
