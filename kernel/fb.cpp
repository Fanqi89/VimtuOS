// fb.cpp - Framebuffer 驱动（VBE LFB 32bpp，手搓）
#include "fb.h"
#include "font8x8.h"
#include "../bootinfo.h"
#include "port.h"
// ★ 驱动线 3：显示后端（virtio-gpu 2D 设备路径 / 软件 LFB 路径）。这里**只调用**它，
//   不反向依赖：没有设备时 vgpu64_blit64() 一律返回 0，下面的软件路径一个字节都没变。
#include "virtio_gpu64.h"
#include "debug64.h"     // fb_backend_log64() 的 [FB64] 打点
#include "mem_64.h"       // ★ v0.10.2：memcpy_64()（提交路径按行整段搬，不再逐像素）
#include "x86_64.h"        // ★ ⑭：g_ticks64 / TICK_MS_64（帧节拍的粗睡眠/标定基准）

static uint64_t fb_addr = 0;          // LFB 物理地址（★ v0.9.8：64 位 —— GOP 帧缓冲可能在 4GiB 以上）
static uint8_t  fb_source = 0;        // 0 = VBE(BIOS loader) / 1 = GOP(UEFI loader)
static int      fb_usable = 1;        // 0 = 当前页表下这块显存不可写 -> 一律不碰屏（绝不黑屏）
static int fb_w = 0, fb_h = 0;
static int fb_pitch = 0;
static uint8_t fb_bpp = 32;
static int g_render_w = 0, g_render_h = 0;   // 渲染分辨率（GUI 坐标系；= 物理/缩放）
static int g_zoom = 100;                     // 显示缩放百分比

// ---- 裁剪矩形（半开区间 [x0,x1) × [y0,y1)）：窗口客户区绘制必须被裁剪 ----
static int g_clip_x0 = 0, g_clip_y0 = 0, g_clip_x1 = 0, g_clip_y1 = 0;
static int g_clip_on = 0;      // 0 = 不裁剪（整屏）

// ---- A1：内核侧绘制/提交总开关（1 = 正常；0 = 用户绘图演示期间"内核不再画"）----
// 为什么需要它：A1 要让"屏幕上这一段确实是**用户程序**在画"这件事可验证 —— 演示期间把内核侧
// 的所有绘制/提交都挡掉（含 write(1,..) 的屏幕回显、启动期的局部提交），只放行用户程序自己的
// fb_flip（fb_user_flip64）。开关由 kernel/usermode64.cpp 的 user64_run_fbdemo64() 切换，
// 用户程序退出（或被 kill 的回收路径）时一定恢复。
static int g_kernel_paint64 = 1;

void fb_kernel_paint64(int on) { g_kernel_paint64 = on ? 1 : 0; }
int  fb_kernel_paint_on64() { return g_kernel_paint64; }

void fb_set_clip(int x, int y, int w, int h) {
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > g_render_w) x1 = g_render_w;
    if (y1 > g_render_h) y1 = g_render_h;
    g_clip_x0 = x; g_clip_y0 = y; g_clip_x1 = x1; g_clip_y1 = y1;
    g_clip_on = 1;
}

void fb_reset_clip() {
    g_clip_on = 0;
    g_clip_x0 = 0; g_clip_y0 = 0;
    g_clip_x1 = g_render_w; g_clip_y1 = g_render_h;
}

// 把矩形与裁剪区求交；返回 false = 完全被裁掉（无需绘制）
static inline bool clip_rect(int* x, int* y, int* w, int* h) {
    if (!g_kernel_paint64) return false;      // ★ A1：内核侧绘制关闭时一律不画（含清屏/填充）
    if (g_clip_on) {
        if (*x < g_clip_x0) { *w += *x - g_clip_x0; *x = g_clip_x0; }
        if (*y < g_clip_y0) { *h += *y - g_clip_y0; *y = g_clip_y0; }
        if (*x + *w > g_clip_x1) *w = g_clip_x1 - *x;
        if (*y + *h > g_clip_y1) *h = g_clip_y1 - *y;
    }
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > g_render_w) *w = g_render_w - *x;
    if (*y + *h > g_render_h) *h = g_render_h - *y;
    return (*w > 0 && *h > 0);
}

// 后备缓冲（32bpp 统一格式）：所有绘制先写这里，fb_flip() 一次性提交到 LFB，
// 避免 GUI 每秒整屏重绘时在屏幕上出现中间状态（闪烁/撕裂）。
// 上限 3840x2160x4B ≈ 33MB（支持 4K 分辨率切换），放 .bss（_end 之后）。
// ★ A1：**必须 4KB 对齐** —— 用户态绘图时这整块会被逐页映射给 ring3（见 fb_surface_phys64），
//   对齐之后映射就是"整块原样"，不用做页内偏移，也不会顺带暴露别的内核变量。
// ★ v0.10.2（真机）：上限常量单列出来，fb_init 按它钳制（见下面的几何钳制段）。
#define FB64_BB_MAX_W 3840
#define FB64_BB_MAX_H 2160
// ★ v0.10.2（帧预算）：后备缓冲必须 **2 MiB 对齐** —— 它会被 2 MiB 大页映射给 ring3
//   （大页的硬件前提是物理地址 2 MiB 对齐；这里是 .bss，物理地址与链接地址同余，
//    所以对齐这个数组就等于对齐它的物理地址）。代价是最多 2 MiB 的 .bss 填充，
//   .bss 上界仍远低于载荷暂存窗口 0x04000000（构建期有断言）。
static uint32_t backbuf_storage[FB64_BB_MAX_W * FB64_BB_MAX_H]
    __attribute__((aligned(2 * 1024 * 1024)));
static uint32_t* backbuf = nullptr;

void fb_get_clip64(int* x, int* y, int* w, int* h) {
    if (g_clip_on) {
        *x = g_clip_x0; *y = g_clip_y0;
        *w = g_clip_x1 - g_clip_x0; *h = g_clip_y1 - g_clip_y0;
    } else {
        *x = 0; *y = 0; *w = fb_width(); *h = fb_height();
    }
}

// 后备缓冲（32bpp）基址 + 渲染分辨率：现代图元层（gfx64）直接写这里，避免每像素走函数调用。
uint32_t* fb_surface64(int* w, int* h) {
    if (!backbuf) backbuf = backbuf_storage;
    if (w) *w = fb_width();
    if (h) *h = fb_height();
    return backbuf;
}

// A1：后备缓冲的物理基址（4KB 对齐）+ 字节数。恒等映射（PA 0..4GB）与高半区直映
// （VA = 0xFFFFFFFF80000000 + PA，见 boot/loader64.asm）在引导期都已建好，所以：
//   PA = VA - 0xFFFFFFFF80000000（与 kernel/proc64.cpp、kernel/kernel64.cpp 用的是同一条公式）。
uint64_t fb_surface_phys64(int* out_bytes) {
    uint32_t* bb = fb_surface64(nullptr, nullptr);
    if (!bb) { if (out_bytes) *out_bytes = 0; return 0; }
    if (out_bytes) *out_bytes = fb_width() * fb_height() * 4;
    return (uint64_t)(uintptr_t)bb - 0xFFFFFFFF80000000ULL;
}


// Bochs VBE 扩展寄存器（QEMU stdvga 支持）：0x1CE 索引 / 0x1CF 数据
#define VBE_INDEX_ID      0x0
#define VBE_INDEX_XRES    0x1
#define VBE_INDEX_YRES    0x2
#define VBE_INDEX_BPP     0x3
#define VBE_INDEX_ENABLE  0x4

// 切换显示模式（分辨率）。失败返回 false（显示器/显存不支持）。
bool fb_set_mode(int w, int h) {
    if (w < 640 || h < 480 || w > 3840 || h > 2160) { outb(0x402, '1'); return false; }
    outw(0x1CE, VBE_INDEX_ENABLE); outw(0x1CF, 0x00);       // 先关闭显示
    outw(0x1CE, VBE_INDEX_XRES);   outw(0x1CF, (uint16_t)w);
    outw(0x1CE, VBE_INDEX_YRES);   outw(0x1CF, (uint16_t)h);
    outw(0x1CE, VBE_INDEX_BPP);    outw(0x1CF, 32);
    outw(0x1CE, VBE_INDEX_ENABLE); outw(0x1CF, 0x81);        // 开启 + LFB
    // 回读验证（QEMU/真实硬件不支持时会钳制或返回 0）
    outw(0x1CE, VBE_INDEX_XRES); uint16_t rw = inw(0x1CF);
    outw(0x1CE, VBE_INDEX_YRES); uint16_t rh = inw(0x1CF);
    outb(0x402, '3'); outb(0x402, (uint8_t)(rw >> 8)); outb(0x402, (uint8_t)rw);
    outb(0x402, '4'); outb(0x402, (uint8_t)(rh >> 8)); outb(0x402, (uint8_t)rh);
    if (rw != w || rh != h) { outb(0x402, '5'); return false; }
    fb_w = w; fb_h = h;
    fb_bpp = 32;
    fb_pitch = w * 4;
    backbuf = backbuf_storage;
    uint32_t n = (uint32_t)w * h;
    for (uint32_t i = 0; i < n; i++) backbuf[i] = 0;
    // 按当前缩放重算渲染分辨率
    g_render_w = fb_w * 100 / g_zoom;
    g_render_h = fb_h * 100 / g_zoom;
    return true;
}

// ==================== ★ v0.9.8：帧缓冲来源 + 页表只读走查 ====================
// 背景（真机验收的直接原因）：UEFI 路径下内核**跑在固件页表上**（boot/efi/uefi64.c 不替换
// CR3，只往固件 PML4 里挂了一条高半区映射）。而 GOP 帧缓冲的窗口是**固件**在启动 GOP 时
// 建的 —— 在大显存卡上（RTX 3080 这类 "Above 4G Decoding" 机器）这个窗口经常落在 4GiB
// 以上，而且固件未必把它标成可写。老代码有两处会直接翻车：
//   ① BootInfo 只有 32 位的 lfb_addr，高地址会被截断 -> 拼命往一个野地址写；
//   ② 就算地址对了，页表里若没有这个窗口/是只读，第一笔写就是 #PF（随后 panic 再写屏 ->
//      三重故障 -> **黑屏**）。
// 对策：地址拼成 64 位（见 fb_init），并且**先只读走查页表**，确认首末页都可写才出帧；
//   不可用就整块不碰（`fb_usable = 0`），屏幕停在固件画面上 —— 宁可没画面，也不黑屏。
#define FB64_PA_MASK 0x000FFFFFFFFFF000ull

// 只读走查当前 CR3（恒等映射）下 pa 这一页的映射状态。
//   返回 0 = 未映射 / 1 = 已映射但只读 / 2 = 已映射且可写。**一个页表项都不写。**
static int fb64_pt_state64(uint64_t pa) {
    uint64_t cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    const uint64_t* l4 = (const uint64_t*)(uintptr_t)(cr3 & FB64_PA_MASK);
    const uint64_t e4 = l4[(pa >> 39) & 511u];
    if (!(e4 & 1u)) return 0;
    // 写权限要**每一级**都允许：CR0.WP=1 时 ring0 写只读页同样 #PF。
    int wr = (e4 & 2u) ? 1 : 0;
    const uint64_t* l3 = (const uint64_t*)(uintptr_t)(e4 & FB64_PA_MASK);
    const uint64_t e3 = l3[(pa >> 30) & 511u];
    if (!(e3 & 1u)) return 0;
    if (!(e3 & 2u)) wr = 0;
    if (e3 & 0x80u) return 1 + wr;                       // 1 GiB 大页
    const uint64_t* l2 = (const uint64_t*)(uintptr_t)(e3 & FB64_PA_MASK);
    const uint64_t e2 = l2[(pa >> 21) & 511u];
    if (!(e2 & 1u)) return 0;
    if (!(e2 & 2u)) wr = 0;
    if (e2 & 0x80u) return 1 + wr;                       // 2 MiB 大页
    const uint64_t* l1 = (const uint64_t*)(uintptr_t)(e2 & FB64_PA_MASK);
    const uint64_t e1 = l1[(pa >> 12) & 511u];
    if (!(e1 & 1u)) return 0;
    if (!(e1 & 2u)) wr = 0;
    return 1 + wr;
}

const char* fb64_source_name64(void) { return fb_source == 1 ? "gop" : "vbe"; }
uint64_t fb64_lfb_phys64(void) { return fb_addr; }
int fb64_lfb_usable64(void) { return fb_usable; }

// ★ v0.9.34：VMware SVGA 的"这块脏了"通知（弱符号：安装介质内核不编 vmware64.cpp，这里为空）。
//   为什么必须由 fb 层调：只有 fb 知道"哪一块刚被画完并写进了 LFB"。
//   非 VMware 机器上 vmware64_update64 是空操作（内部 g_upd_ok=0），一个字节的代价。
// ★ v0.9.35：LFB 可见性探针（弱符号：默认启动不存在，只有内存里有 /etc/gpu3d_probe 时才是活的）
void vmware64_lfbprobe_tick64(void) __attribute__((weak));

// ★ v0.9.35：**后备缓冲 vs LFB 逐点比对**（同一像素两处取值，诊断用）。
//   为什么要它：VNC 侧已经证明"宿主确实读我们写的显存"（直接往 LFB 刷的 8 条饱和彩条上屏了），
//   所以"黑"只可能出在我们自己这条链上。这一行把链断在哪儿一次说清：
//     bb 与 lfb 都黑   -> 合成阶段就是黑的（画的时候没画上）
//     bb 有内容、lfb 黑 -> 后备缓冲 -> LFB 的拷贝没落地
//   默认不采样（fb64_mixprobe_arm64(0)），只有带 /etc/gpu3d_probe 的取证启动才打开。
static int g_mix_left64 = 0;
static uint32_t g_mix_skip64 = 0;
void fb64_mixprobe_arm64(int n) { g_mix_left64 = n > 0 ? n : 0; g_mix_skip64 = 0; }
static void fb64_mixsample64(void) {
    if (g_mix_left64 <= 0 || !backbuf || !fb_addr) return;
    // 抽样：每 64 次提交采一次（逐帧采会把串口刷爆 —— 实测一次溢出刷了 2,365 行）。
    if (g_mix_skip64) { g_mix_skip64--; return; }
    g_mix_skip64 = 63;
    g_mix_left64--;
    static const int PX[3] = {20, 512, 512};
    static const int PY[3] = {20, 384, 700};
    volatile uint32_t* lf = (volatile uint32_t*)(uintptr_t)fb_addr;
    dbg64_line_begin64();
    dbg64_str("[FB64] mix n="); dbg64_dec((uint64_t)g_mix_left64);
    dbg64_str(" geom="); dbg64_dec((uint64_t)g_render_w); dbg64_str("x");
    dbg64_dec((uint64_t)g_render_h); dbg64_str(" pitch="); dbg64_dec((uint64_t)fb_pitch);
    for (int i = 0; i < 3; i++) {
        int x = PX[i] < g_render_w ? PX[i] : g_render_w - 1;
        int y = PY[i] < g_render_h ? PY[i] : g_render_h - 1;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        const uint32_t bb = backbuf[y * g_render_w + x] & 0x00FFFFFFu;
        const uint32_t lfv = lf[(uint64_t)y * (uint64_t)fb_w + (uint64_t)x] & 0x00FFFFFFu;
        dbg64_str(" p"); dbg64_dec((uint64_t)i);
        dbg64_str("=");  dbg64_hex64((uint64_t)bb);
        dbg64_str("/");  dbg64_hex64((uint64_t)lfv);
    }
    dbg64_nl();
    dbg64_line_end64();
}
void vmware64_update64(int x, int y, int w, int h) __attribute__((weak));

// ==================== ★ v0.9.9：帧缓冲页表**自补** ====================
// 为什么需要：UEFI 下帧缓冲的映射是**固件**建的。v0.9.8 的只读走查发现"没映射/只读"时只会
// 让 fb_usable=0（不黑屏，但也没画面）。本批升一级：**缺的映射我们自己补**，补完**重新走查
// 确认**，确认不了仍然 fb_usable=0 —— 也就是"能补就补，补不成还是不碰屏"。
//
// 三条硬纪律：
//   ① **只补缺的**：任何已存在的页表项（4 KiB / 2 MiB / 1 GiB，也不管读写位）一律**不动**，
//      把风险限制在"新增"这一种动作上。
//   ② 新页表页来自内核页池（page_alloc_64，物理即虚拟的恒等映射），**先清零再用**。
//   ③ 写页表包在"临时清 CR0.WP"的窗口里（EDK2 把固件页表页标成只读，直接写会 #PF；这与
//      boot/efi/uefi64.c 挂高半区映射是同一个手法），写完立刻恢复**完整 CR0**。
//   只新增"原本不存在"的项 -> 不需要 invlpg（boot/efi/uefi64.c 里有同样的实测记录）。
// 页池分配（实现见 kernel/mem64.cpp）。这里**只声明这一个函数**：直接 include mem_64.h
// 会连带 <stddef.h>，与本项目的 freestanding 头文件布局冲突（已有已知噪音），为一个符号不值得。
void* page_alloc_64();
void page_free_64(void* p);

#define FB64_2MB 0x200000ull

static inline uint64_t fb64_cr0_get64(void) {
    uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v;
}
static inline void fb64_cr0_set64(uint64_t v) {
    __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}
static void fb64_page_zero64(uint64_t pa) {            // -nostdlib：自己清
    volatile uint64_t* p = (volatile uint64_t*)(uintptr_t)pa;
    for (uint32_t i = 0; i < 512u; i++) p[i] = 0;
}

// 为物理区间 [pa, pa+bytes) 补齐恒等映射（2 MiB 大页）。返回**新补的 2 MiB 页数**；
// 0 = 没补（已存在 / 补不了）。绝不改动已存在的项。
static int fb64_remap64(uint64_t pa, uint64_t bytes) {
    if (!pa || !bytes) return 0;
    uint64_t cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t* l4 = (uint64_t*)(uintptr_t)(cr3 & FB64_PA_MASK);
    const uint64_t first = pa & ~(FB64_2MB - 1ull);
    const uint64_t last  = (pa + bytes + FB64_2MB - 1ull) & ~(FB64_2MB - 1ull);
    const uint64_t cr0 = fb64_cr0_get64();
    const uint64_t cr0_nw = cr0 & ~0x10000ull;         // 清 CR0.WP(bit16)
    int made = 0;
    for (uint64_t chunk = first; chunk < last; chunk += FB64_2MB) {
        const uint64_t i4 = (chunk >> 39) & 511u;
        uint64_t* l3 = nullptr;
        if (!(l4[i4] & 1u)) {
            const uint64_t p = (uint64_t)(uintptr_t)page_alloc_64();
            if (!p) break;
            fb64_page_zero64(p);
            fb64_cr0_set64(cr0_nw); l4[i4] = p | 0x3u; fb64_cr0_set64(cr0);   // P|RW
            l3 = (uint64_t*)(uintptr_t)p;
        } else {
            l3 = (uint64_t*)(uintptr_t)(l4[i4] & FB64_PA_MASK);
        }
        const uint64_t i3 = (chunk >> 30) & 511u;
        uint64_t* l2 = nullptr;
        if (!(l3[i3] & 1u)) {
            const uint64_t p = (uint64_t)(uintptr_t)page_alloc_64();
            if (!p) break;
            fb64_page_zero64(p);
            fb64_cr0_set64(cr0_nw); l3[i3] = p | 0x3u; fb64_cr0_set64(cr0);   // PD
            l2 = (uint64_t*)(uintptr_t)p;
        } else if (l3[i3] & 0x80u) {
            continue;                                  // 1 GiB 大页盖着：不改，交给走查判定
        } else {
            l2 = (uint64_t*)(uintptr_t)(l3[i3] & FB64_PA_MASK);
        }
        const uint64_t i2 = (chunk >> 21) & 511u;
        if (l2[i2] & 1u) continue;                     // 已存在（含 2 MiB 大页）：**不动**
        fb64_cr0_set64(cr0_nw); l2[i2] = chunk | 0x83u; fb64_cr0_set64(cr0);  // P|RW|PS
        made++;
    }
    return made;
}

// 把**已存在、恒等映射、但只读**的映射升级为可写（只动 RW 位，不动地址与页大小）。
// 为什么需要：固件完全可能把帧缓冲映射成只读（UEFI BootServices 阶段常见）。这时地址对、映射在，
//   但 ring0 写下去仍然 #PF —— 光"补缺的项"救不了，必须把 RW 位打开。
// 与 fb64_remap64 的区别：那个只新增、不需要 invlpg；这里改的是**已存在**的映射，
//   所以每个改过的页都要 invlpg（TLB 里可能已经缓存了只读项）。
// 返回升级的页数；0 = 没什么可升级的（本来就可写 / 大页 / 非恒等 / 未映射）。
static int fb64_upgrade_ro64(uint64_t pa, uint64_t bytes) {
    if (!pa || !bytes) return 0;
    uint64_t cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t* l4 = (uint64_t*)(uintptr_t)(cr3 & FB64_PA_MASK);
    const uint64_t first = pa & ~(FB64_2MB - 1ull);
    const uint64_t last  = (pa + bytes + FB64_2MB - 1ull) & ~(FB64_2MB - 1ull);
    const uint64_t cr0 = fb64_cr0_get64();
    const uint64_t cr0_nw = cr0 & ~0x10000ull;
    int up = 0;
    for (uint64_t chunk = first; chunk < last; chunk += FB64_2MB) {
        const uint64_t e4 = l4[(chunk >> 39) & 511u];
        if (!(e4 & 1u)) continue;
        const uint64_t* l3 = (const uint64_t*)(uintptr_t)(e4 & FB64_PA_MASK);
        const uint64_t e3 = l3[(chunk >> 30) & 511u];
        if (!(e3 & 1u)) continue;
        if (e3 & 0x80u) continue;                    // 1 GiB 大页：要拆分才能改，本层**不做**（如实跳过）
        uint64_t* l2 = (uint64_t*)(uintptr_t)(e3 & FB64_PA_MASK);
        const uint64_t i2 = (chunk >> 21) & 511u;
        const uint64_t e2 = l2[i2];
        if (!(e2 & 1u)) continue;                    // 没映射 -> 交给 fb64_remap64
        if (e2 & 0x80u) {                            // 2 MiB 大页
            if ((e2 & FB64_PA_MASK) != chunk) continue;      // 不是恒等映射：不猜，跳过
            if (e2 & 2u) continue;                           // 本来就可写
            fb64_cr0_set64(cr0_nw); l2[i2] = e2 | 2u; fb64_cr0_set64(cr0);
            __asm__ volatile("invlpg (%0)" : : "r"((void*)(uintptr_t)chunk) : "memory");
            up++;
        } else {                                     // 4 KiB 页表
            uint64_t* l1 = (uint64_t*)(uintptr_t)(e2 & FB64_PA_MASK);
            for (uint32_t k = 0; k < 512u; k++) {
                const uint64_t e1 = l1[k];
                if (!(e1 & 1u) || (e1 & 2u)) continue;
                if ((e1 & FB64_PA_MASK) != (chunk + (uint64_t)k * 4096ull)) continue;   // 非恒等：跳过
                fb64_cr0_set64(cr0_nw); l1[k] = e1 | 2u; fb64_cr0_set64(cr0);
                __asm__ volatile("invlpg (%0)" : : "r"((void*)(uintptr_t)(chunk + (uint64_t)k * 4096ull)) : "memory");
                up++;
            }
        }
    }
    return up;
}

// ==================== LFB 的缓存属性（WC / UC）====================
// ★ 为什么单独有这一层（v0.9.34 实测定位的真缺陷）：
//   帧缓冲是**设备内存**，不是普通 RAM。固件把 LFB 映射成可写之后，页表项里没有 PCD/PWT、
//   MTRR 也没为它建项 —— 这块地址就按默认类型 **WB（可缓存）** 走。QEMU 上感觉不到
//   （std vga 的 LFB 就是普通 RAM，扫出电路直接读那块内存）；VMware SVGA 上**致命**：
//   宿主用 `SVGA-ScreenMgr: RegisterMode` + 影子屏 + 脏页跟踪刷新，客人写只停在 CPU cache 里、
//   从不落到客人物理地址 -> 脏页位不置位 -> 宿主看到的是没被改过的黑屏（只有极少数被
//   挤出 cache 的像素偶尔可见）。实测：VMware 上电全过程（含开机 logo/进度条）VNC 抓到
//   99.8% 纯黑、768 行里 760 行整行全黑；同一份内核在 QEMU@1024x768 上纯黑像素只有 42 个。
//   修法：把 LFB 的映射标成**不缓存（PWT|PCD）**，写直达内存，脏页跟踪立刻看见。
//   为什么不顺便上 MTRR WC（更快）：写 MTRR 需要 WRMSR，而固件可以把 MTRR 锁死
//   （锁了就 WRMSR #GP -> ring0 异常 -> 开机 PANIC）。一次"图快"换来真机可能起不来，
//   不值。这里只改**我们自己的页表位**（和 fb64_upgrade_ro64 同一套手法，proven），
//   代价是 LFB 写变成非缓存写（比 WB/WC 慢），换来的是"写下去一定被设备/宿主看见"。
//   要 WC 的加速，得先有可靠的 MTRR 锁定探测 —— 那是单独一批的事，不在本批夹带。
// 页表回退：给 [pa, pa+bytes) 覆盖到的**已存在**映射项加 PWT|PCD（= 不缓存）。
// 只动缓存位，不动地址/页大小/RW；非恒等映射一律跳过（不猜）。返回改过的项数。
// 缓存属性改完之后必须把旧的缓存行/写合并缓冲丢掉：否则新属性要等"下一次访问"才生效，
// 而我们紧接着就要往 LFB 里画第一帧。
static inline void fb64_sfence64(void) { __asm__ volatile("sfence" : : : "memory"); }
static inline void fb64_wbinvd64(void) { __asm__ volatile("wbinvd" : : : "memory"); }

static int fb64_set_uc_pages64(uint64_t pa, uint64_t bytes) {
    if (!pa || !bytes) return 0;
    uint64_t cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t* l4 = (uint64_t*)(uintptr_t)(cr3 & FB64_PA_MASK);
    const uint64_t first = pa & ~(FB64_2MB - 1ull);
    const uint64_t last  = (pa + bytes + FB64_2MB - 1ull) & ~(FB64_2MB - 1ull);
    const uint64_t cr0 = fb64_cr0_get64();
    const uint64_t cr0_nw = cr0 & ~0x10000ull;
    int n = 0;
    for (uint64_t chunk = first; chunk < last; chunk += FB64_2MB) {
        const uint64_t e4 = l4[(chunk >> 39) & 511u];
        if (!(e4 & 1u)) continue;
        const uint64_t* l3 = (const uint64_t*)(uintptr_t)(e4 & FB64_PA_MASK);
        const uint64_t e3 = l3[(chunk >> 30) & 511u];
        if (!(e3 & 1u) || (e3 & 0x80u)) continue;                 // 1 GiB 大页：不拆
        uint64_t* l2 = (uint64_t*)(uintptr_t)(e3 & FB64_PA_MASK);
        const uint64_t i2 = (chunk >> 21) & 511u;
        const uint64_t e2 = l2[i2];
        if (!(e2 & 1u)) continue;
        if (e2 & 0x80u) {                                         // 2 MiB 大页
            if ((e2 & FB64_PA_MASK) != chunk) continue;            // 非恒等：跳过
            if ((e2 & 0x18u) == 0x18u) { n++; continue; }           // 已经 UC
            fb64_cr0_set64(cr0_nw); l2[i2] = e2 | 0x18u; fb64_cr0_set64(cr0);
            __asm__ volatile("invlpg (%0)" : : "r"((void*)(uintptr_t)chunk) : "memory");
            n++;
        } else {
            uint64_t* l1 = (uint64_t*)(uintptr_t)(e2 & FB64_PA_MASK);
            for (uint32_t k = 0; k < 512u; k++) {
                const uint64_t e1 = l1[k];
                if (!(e1 & 1u)) continue;
                const uint64_t va = chunk + (uint64_t)k * 4096ull;
                if ((e1 & FB64_PA_MASK) != va) continue;           // 非恒等：跳过
                if ((e1 & 0x18u) == 0x18u) { n++; continue; }
                fb64_cr0_set64(cr0_nw); l1[k] = e1 | 0x18u; fb64_cr0_set64(cr0);
                __asm__ volatile("invlpg (%0)" : : "r"((void*)(uintptr_t)va) : "memory");
                n++;
            }
        }
    }
    if (n) fb64_wbinvd64();
    return n;
}

// 一行结果打点（新行；老行一律没动）：
//   [FB64] lfb cache=uc pages=2 ok=1                       （页表改成了不缓存）
//   [FB64] lfb cache=wb reason=no-page-found ok=0          （如实失败，不假装）
static void fb64_lfb_cache64(uint64_t pa, uint64_t bytes) {
    const int np = fb64_set_uc_pages64(pa, bytes);
    dbg64_line_begin64();
    dbg64_str("[FB64] lfb ");
    if (np) {
        dbg64_str("cache=uc pages="); dbg64_dec((uint64_t)np);
        dbg64_str(" base=0x");        dbg64_hex64(pa);
        dbg64_str(" ok=1");
    } else {
        dbg64_str("cache=wb reason=no-page-found ok=0");
    }
    dbg64_nl();
    dbg64_line_end64();
}

// 自检：在**一段确定未映射**的地址上真走一遍"补映射 -> 走查确认 -> 原样抹掉并还页"，
//   证明"往当前页表里写项"这条路在当前固件（BIOS 页表 / UEFI 固件页表）下真的可用。
//   候选判据：走查结果为空（= 现在这块地址根本不可用）。于是三种情况都覆盖到：
//     PML4 项缺 -> 建 PDPT 页；PDPT 项缺 -> 建 PD 页；只缺 2 MiB 项 -> 只加一项。
//   复原按"逆序只拆我们建的那一层"，并把新建的页表页**还回页池**；找不到候选就如实 skipped。
//   ★ 全程不读写那段地址本身（只碰页表项），所以对真机无害。
static int fb64_remap_selftest64(void) {
    uint64_t cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t* l4 = (uint64_t*)(uintptr_t)(cr3 & FB64_PA_MASK);
    uint64_t cand = 0;
    for (uint64_t a = 0x3E0000000ull, n = 0; n < 4096u; n++, a += FB64_2MB) {
        if (fb64_pt_state64(a) == 0) { cand = a; break; }   // 当前不可用 = 命中
    }
    if (!cand) {
        dbg64_line_begin64();
        dbg64_str("[FB64] remap selftest skipped reason=no-unmapped-window");
        dbg64_nl(); dbg64_line_end64();
        return 0;
    }
    const uint64_t q4 = (cand >> 39) & 511u;
    const uint64_t q3 = (cand >> 30) & 511u;
    const uint64_t q2 = (cand >> 21) & 511u;
    const uint64_t e4_before = l4[q4];
    uint64_t e3_before = 0;
    if (e4_before & 1u) {
        const uint64_t* p3 = (const uint64_t*)(uintptr_t)(e4_before & FB64_PA_MASK);
        e3_before = p3[q3];
    }
    const uint64_t cr0 = fb64_cr0_get64();
    const uint64_t cr0_nw = cr0 & ~0x10000ull;
    const int made = fb64_remap64(cand, FB64_2MB);
    const int st = fb64_pt_state64(cand);
    // ---- v0.9.10：再把刚建的那条 2 MiB 项**故意改成只读**，验证 RO->RW 升级路径 ----
    // 这一步在 QEMU 里可以真跑（我们就是在改自己的页表），所以它不是"纸面能力"。
    uint64_t* pd = nullptr;
    {
        const uint64_t e4 = l4[q4];
        const uint64_t* p3 = (const uint64_t*)(uintptr_t)(e4 & FB64_PA_MASK);
        const uint64_t e3 = (e4 & 1u) ? p3[q3] : 0;
        pd = (uint64_t*)(uintptr_t)(e3 & FB64_PA_MASK);
    }
    int st_ro = 0, up = 0, st_up = 0;
    if (pd) {
        fb64_cr0_set64(cr0_nw); pd[q2] = cand | 0x81u; fb64_cr0_set64(cr0);   // P|PS，清 RW
        __asm__ volatile("invlpg (%0)" : : "r"((void*)(uintptr_t)cand) : "memory");
        st_ro = fb64_pt_state64(cand);                 // 期望 1 = ro
        up    = fb64_upgrade_ro64(cand, FB64_2MB);     // 期望 >=1
        st_up = fb64_pt_state64(cand);                 // 期望 2 = rw
    }
    // ---- 复原：逆序只拆我们建的那一层，并把新建的页表页还回去 ----
    fb64_cr0_set64(cr0_nw);
    if (!(e4_before & 1u)) {
        const uint64_t p = l4[q4] & FB64_PA_MASK;
        l4[q4] = 0;
        fb64_cr0_set64(cr0);
        if (p) page_free_64((void*)(uintptr_t)p);
    } else if (!(e3_before & 1u)) {
        uint64_t* p3 = (uint64_t*)(uintptr_t)(e4_before & FB64_PA_MASK);
        const uint64_t p = p3[q3] & FB64_PA_MASK;
        p3[q3] = 0;
        fb64_cr0_set64(cr0);
        if (p) page_free_64((void*)(uintptr_t)p);
    } else {
        uint64_t* p2 = (uint64_t*)(uintptr_t)(e3_before & FB64_PA_MASK);
        p2[q2] = 0;
        fb64_cr0_set64(cr0);
    }
    const int st2 = fb64_pt_state64(cand);
    const int ok = (st == 2 && made > 0 && st_ro == 1 && up > 0 && st_up == 2 && st2 == 0) ? 1 : 0;
    dbg64_line_begin64();
    dbg64_str("[FB64] remap selftest pa=0x"); dbg64_hex64(cand);
    dbg64_str(" made=");      dbg64_dec((uint64_t)made);
    dbg64_str(" walk=");      dbg64_str(st == 2 ? "rw" : (st == 1 ? "ro" : "unmapped"));
    dbg64_str(" ro_walk=");   dbg64_str(st_ro == 1 ? "ro" : (st_ro == 2 ? "rw" : "unmapped"));
    dbg64_str(" upgrade=");   dbg64_dec((uint64_t)up);
    dbg64_str(" rw_walk=");   dbg64_str(st_up == 2 ? "rw" : (st_up == 1 ? "ro" : "unmapped"));
    dbg64_str(" restored=");  dbg64_putc(st2 == 0 ? '1' : '0');
    dbg64_str(" ok=");        dbg64_putc(ok ? '1' : '0');
    dbg64_nl(); dbg64_line_end64();
    return ok;
}

// fw_cfg 测试开关：opt/vimtu/gpu 的 fbtest=remap -> 启动时跑一次上面的自检。
// 只用于夹具/真机取证（默认不跑，日志保持干净）。
static int fb64_fbtest_remap64(void) {
    char v[16];
    if (!fb64_fwcfg_str64("opt/vimtu/gpu", "fbtest", v, (int)sizeof(v))) return 0;
    return (v[0] == 'r' && v[1] == 'e' && v[2] == 'm' && v[3] == 'a' && v[4] == 'p') ? 1 : 0;
}

// 一行来源打点（纯 ASCII，新行；老行一律没动）。真机取证就靠它回答
// "这张卡上到底走的哪条路、地址是多少、固件有没有把这块显存映射成可写"。
__attribute__((noinline)) static void fb64_log_source64(int st_first, int st_last, int remapped, int upgraded) {
    dbg64_line_begin64();
    dbg64_str("[FB64] fb source="); dbg64_str(fb64_source_name64());
    dbg64_str(" addr=0x");          dbg64_hex64(fb_addr);
    dbg64_str(" w=");               dbg64_dec((uint64_t)fb_w);
    dbg64_str(" h=");               dbg64_dec((uint64_t)fb_h);
    dbg64_str(" pitch=");           dbg64_dec((uint64_t)fb_pitch);
    dbg64_str(" hi4g=");            dbg64_putc(fb_addr > 0xFFFFFFFFull ? '1' : '0');
    dbg64_str(" pt=");
    dbg64_str(st_first == 0 ? "unmapped" : (st_first == 1 ? "ro" : "rw"));
    dbg64_str(" pt_last=");
    dbg64_str(st_last == 0 ? "unmapped" : (st_last == 1 ? "ro" : "rw"));
    dbg64_str(" usable=");          dbg64_putc(fb_usable ? '1' : '0');
    dbg64_str(" remap=");           dbg64_dec((uint64_t)remapped);   // 自补了几页 2MiB
    dbg64_str(" upgrade=");         dbg64_dec((uint64_t)upgraded);   // RO->RW 升级了几页
    dbg64_nl();
    dbg64_line_end64();
}

void fb_init(const BootInfo* bi) {
    // ★ 64 位拼法：低 32 位在老字段里（老内核照旧能读），高 32 位在新字段里（BIOS loader 显式写 0）。
    fb_addr   = (uint64_t)bi->lfb_addr | ((uint64_t)bi->lfb_addr_hi << 32);
    fb_source = (bi->fb_source == 1) ? 1 : 0;            // 只认 1 = GOP，其余一律当 VBE（默认路径）
    fb_w = bi->width;
    fb_h = bi->height;
    fb_bpp = bi->bpp;
    fb_pitch = bi->pitch;
    if (fb_pitch == 0) fb_pitch = fb_w * 4;
    // 页表只读走查：首末页都可写才允许出帧；不可用就**试着自补**（只新增缺的项），
    // 补完再走查一次 —— 仍然不可用就 fb_usable=0（不碰屏）。理由见上面那段注释。
    int s0 = 0, s1 = 0;
    int remapped = 0;
    int upgraded = 0;
    if (fb_addr && fb_w > 0 && fb_h > 0) {
        const uint64_t span = (uint64_t)fb_h * (uint64_t)fb_pitch;
        s0 = fb64_pt_state64(fb_addr);
        s1 = fb64_pt_state64(fb_addr + span - 1u);
        fb_usable = (s0 == 2 && s1 == 2) ? 1 : 0;
        if (!fb_usable) {                                 // ★ v0.9.9：缺映射就自己补
            remapped = fb64_remap64(fb_addr, span);
            s0 = fb64_pt_state64(fb_addr);
            s1 = fb64_pt_state64(fb_addr + span - 1u);
            fb_usable = (s0 == 2 && s1 == 2) ? 1 : 0;
        }
        if (!fb_usable) {                                 // ★ v0.9.10：映射在但只读 -> 升级 RW
            upgraded = fb64_upgrade_ro64(fb_addr, span);
            s0 = fb64_pt_state64(fb_addr);
            s1 = fb64_pt_state64(fb_addr + span - 1u);
            fb_usable = (s0 == 2 && s1 == 2) ? 1 : 0;
        }
    } else {
        fb_usable = 0;                                    // 没有帧缓冲：不出帧，也不崩
    }
    backbuf = backbuf_storage;
    // ★ v0.10.2（帧预算）：把后备缓冲的物理地址与大页对齐情况打出来 —— 这是
    //   "fb_map 能不能走 2 MiB 大页"的唯一判据（tail 与 source 行都不带这个信息）。
    {
        const uint64_t bbpa = (uint64_t)(uintptr_t)backbuf - 0xFFFFFFFF80000000ULL;
        dbg64_line_begin64();
        dbg64_str("[FB64] backbuf pa=0x"); dbg64_hex64(bbpa);
        dbg64_str(" huge="); dbg64_dec((bbpa & (2ull * 1024 * 1024 - 1)) == 0 ? 1 : 0);
        dbg64_nl(); dbg64_line_end64();
    }
    // ★ v0.10.2（真机防黑屏/防写穿）：后备缓冲是写死的 3840x2160。GOP 原生模式可能更大
    //   （4096x2160 / 5120x2880），原来直接按 fb_w*fb_h 清零 = 写穿 .bss（第一次就炸）。
    //   这里复用既有的"渲染区 + 最近邻放大"机制把渲染区钳进上限：分辨率低一档，
    //   但屏幕照常出图（提交走 fb_flip_all64 的缩放路径），绝不黑屏。
    g_render_w = fb_w;
    g_render_h = fb_h;
    g_zoom = 100;
    if (fb_w > FB64_BB_MAX_W || fb_h > FB64_BB_MAX_H) {
        int rw = fb_w, rh = fb_h;
        while (rw > FB64_BB_MAX_W || rh > FB64_BB_MAX_H) { rw = rw * 3 / 4; rh = rh * 3 / 4; }
        if (rw < 640) rw = 640;
        if (rh < 480) rh = 480;
        if (rw > FB64_BB_MAX_W) rw = FB64_BB_MAX_W;
        if (rh > FB64_BB_MAX_H) rh = FB64_BB_MAX_H;
        g_render_w = rw;
        g_render_h = rh;
        g_zoom  = (rw > 0) ? (int)((long long)fb_w * 100 / rw) : 100;
        if (g_zoom < 101) g_zoom = 101;                    // != 100 => 提交走缩放路径
        dbg64_line_begin64();
        dbg64_str("[FB64] geom clamp lfb="); dbg64_dec((uint64_t)fb_w); dbg64_str("x"); dbg64_dec((uint64_t)fb_h);
        dbg64_str(" render="); dbg64_dec((uint64_t)rw); dbg64_str("x"); dbg64_dec((uint64_t)rh);
        dbg64_str(" zoom="); dbg64_dec((uint64_t)g_zoom);
        dbg64_str(" max="); dbg64_dec(FB64_BB_MAX_W); dbg64_str("x"); dbg64_dec(FB64_BB_MAX_H);
        dbg64_nl();
        dbg64_line_end64();
    }
    // 清零后备缓冲（按**渲染区**，绝不越过 backbuf_storage）
    {
        const uint32_t rw = (uint32_t)(g_render_w > 0 ? g_render_w : 0);
        const uint32_t rh = (uint32_t)(g_render_h > 0 ? g_render_h : 0);
        uint32_t n = rw * rh;
        if (n > (uint32_t)(FB64_BB_MAX_W * FB64_BB_MAX_H)) n = (uint32_t)(FB64_BB_MAX_W * FB64_BB_MAX_H);
        for (uint32_t i = 0; i < n; i++) backbuf[i] = 0;
    }
    fb64_log_source64(s0, s1, remapped, upgraded);
    // ★ v0.9.34：LFB 是设备内存，必须带 WC/UC 缓存属性才可能被宿主/真机看见。
    //   放在 log_source 之后：几何/可用性都已定论，属性只影响"写下去多久被看见"。
    if (fb_usable && fb_addr)
        fb64_lfb_cache64(fb_addr, (uint64_t)fb_h * (uint64_t)(fb_pitch ? fb_pitch : fb_w * 4));
    // 夹具/取证开关：opt/vimtu/gpu 的 fbtest=remap -> 真跑一遍页表自补自检
    if (fb64_fbtest_remap64()) fb64_remap_selftest64();
}

int fb_width() { return g_render_w ? g_render_w : fb_w; }
int fb_height() { return g_render_h ? g_render_h : fb_h; }
int fb_phys_width() { return fb_w; }
int fb_phys_height() { return fb_h; }
int fb_get_zoom() { return g_zoom; }

// 设置显示缩放：渲染分辨率 = 物理/缩放，提交时最近邻放大
void fb_set_zoom(int pct) {
    if (pct < 100) pct = 100;
    if (pct > 200) pct = 200;
    g_zoom = pct;
    g_render_w = fb_w * 100 / pct;
    g_render_h = fb_h * 100 / pct;
    uint32_t n = (uint32_t)g_render_w * g_render_h;
    for (uint32_t i = 0; i < n; i++) backbuf[i] = 0;
}

// 后备缓冲 -> LFB（整帧提交）：共享实现（内核侧 fb_flip 与 A1 的 fb_user_flip64 都走它）
static void fb_flip_all64() {
    if (!backbuf || !fb_addr || !fb_usable) return;
    // ★ 驱动线 3：设备路径优先。virtio-gpu 2D 可用时整屏也用**一个区域**提交：
    //   TRANSFER_TO_HOST_2D(整屏) + RESOURCE_FLUSH —— 不再让 CPU 逐像素写 LFB。
    //   返回 0（没有设备/几何不匹配/超时）就原样往下走既有软件路径（这一条是硬要求）。
    if (g_zoom == 100 && vgpu64_blit64(0, 0, fb_w, fb_h)) return;
    if (g_zoom == 100) {
        if (fb_bpp == 32 && fb_pitch == fb_w * 4) {
            // 快速路径：整块逐像素拷贝
            uint32_t* dst = (uint32_t*)fb_addr;
            uint32_t n = (uint32_t)fb_w * fb_h;
            for (uint32_t i = 0; i < n; i++) dst[i] = backbuf[i];
        } else {
            // 慢速路径：逐行按 bpp 转换
            for (int y = 0; y < fb_h; y++) {
                volatile uint8_t* row = (volatile uint8_t*)(fb_addr + y * fb_pitch);
                for (int x = 0; x < fb_w; x++) {
                    uint32_t c = backbuf[y * fb_w + x];
                    if (fb_bpp == 32) {
                        ((volatile uint32_t*)row)[x] = c;
                    } else if (fb_bpp == 24) {
                        row[x * 3] = c & 0xFF;
                        row[x * 3 + 1] = (c >> 8) & 0xFF;
                        row[x * 3 + 2] = (c >> 16) & 0xFF;
                    } else {
                        uint16_t cc = ((c >> 16 & 0xF8) << 8) | ((c >> 8 & 0xFC) << 3) | (c >> 3);
                        ((volatile uint16_t*)row)[x] = cc;
                    }
                }
            }
        }
        fb64_sfence64();   // ★ WC 写合并缓冲落地：sfence 之后这些写对设备全局可见
        if (vmware64_update64) vmware64_update64(0, 0, fb_w, fb_h);   // VMware：整屏脏
        fb64_mixsample64();                                            // 诊断：bb vs lfb 逐点比对
        if (vmware64_lfbprobe_tick64) vmware64_lfbprobe_tick64();      // 诊断：直接刷 LFB 彩条
        return;
    }
    // 缩放路径：最近邻放大渲染区域到整个 LFB
    uint32_t* dst = (uint32_t*)fb_addr;
    int pw = fb_w, ph = fb_h, rw = g_render_w, rh = g_render_h;
    for (int y = 0; y < ph; y++) {
        int sy = y * rh / ph;
        if (sy >= rh) sy = rh - 1;
        const uint32_t* srow = backbuf + sy * rw;
        uint32_t* drow = dst + y * pw;
        for (int x = 0; x < pw; x++) {
            drow[x] = srow[x * rw / pw];
        }
    }
    fb64_sfence64();   // ★ 缩放路径同样要把 WC 缓冲刷出去
    if (vmware64_update64) vmware64_update64(0, 0, fb_w, fb_h);       // VMware：整屏脏
}

// 后备缓冲 -> LFB（整帧提交）：**内核侧**提交入口（A1 演示期间被开关挡住）
void fb_flip() {
    if (!g_kernel_paint64) return;              // ★ A1：内核侧提交关闭（用户程序独占屏幕）
    fb_flip_all64();
}

// 后备缓冲 -> LFB（仅提交指定区域；用于光标等局部更新，避免整帧 3MB 拷贝拖慢跟手）
// 共享实现：fb_flip_region（内核侧，受开关影响）与 fb_user_flip64（用户态提交，不受影响）都用它。
static void fb_blit_region64(int x, int y, int w, int h) {
    if (!backbuf || !fb_addr || !fb_usable) return;
    if (g_zoom != 100) { fb_flip_all64(); return; }   // 缩放时无法局部映射，退化整帧
    // ★ 驱动线 3：设备路径优先（TRANSFER_TO_HOST_2D + RESOURCE_FLUSH 只搬脏矩形）。
    //   真走了设备就直接返回；否则（无设备/降级/超时）原样走下面的软件路径。
    if (vgpu64_blit64(x, y, w, h)) return;
    vgpu64_note_soft_blit64(x, y, w, h);              // 如实统计"这次走的是软件路径"
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;
    if (fb_bpp == 32 && fb_pitch == fb_w * 4) {
        uint32_t* dst = (uint32_t*)fb_addr;
        // ★ v0.10.2（帧预算）：逐像素循环 -> 行级 memcpy。/ 实测（tests/wmperf64_test.py）：
        //   172x108 的提交从 ≈450 µs 降到 ≈110 µs（QEMU TCG）；一帧要提交的像素数直接决定
        //   拖动窗口时的帧率，这条路径是合成器每帧的最后一跳。
        for (int row = 0; row < h; row++) {
            uint32_t* d = dst + (y + row) * fb_w + x;
            const uint32_t* s = backbuf + (y + row) * fb_w + x;
            memcpy_64(d, s, (uint64_t)w * 4u);          /* 显式调用：绝不让编译器展开成 rep movsb */
        }
    } else {
        for (int yy = y; yy < y + h; yy++) {
            volatile uint8_t* row = (volatile uint8_t*)(fb_addr + yy * fb_pitch);
            for (int xx = x; xx < x + w; xx++) {
                uint32_t c = backbuf[yy * fb_w + xx];
                if (fb_bpp == 32) {
                    ((volatile uint32_t*)row)[xx] = c;
                } else if (fb_bpp == 24) {
                    row[xx * 3] = c & 0xFF;
                    row[xx * 3 + 1] = (c >> 8) & 0xFF;
                    row[xx * 3 + 2] = (c >> 16) & 0xFF;
                } else {
                    uint16_t cc = ((c >> 16 & 0xF8) << 8) | ((c >> 8 & 0xFC) << 3) | (c >> 3);
                    ((volatile uint16_t*)row)[xx] = cc;
                }
            }
        }
    }
    fb64_sfence64();   // ★ WC 写合并缓冲落地（局部提交同理）
    if (vmware64_update64) vmware64_update64(x, y, w, h);             // VMware：这块脏了
    fb64_mixsample64();                                                // 诊断：bb vs lfb 逐点比对
    if (vmware64_lfbprobe_tick64) vmware64_lfbprobe_tick64();          // 诊断：直接刷 LFB 彩条
}

// 后备缓冲 -> LFB（仅提交指定区域；用于光标等局部更新，避免整帧 3MB 拷贝拖慢跟手）
// **内核侧**提交入口：A1 演示期间被"内核侧绘制/提交"开关挡住（用户程序用 fb_user_flip64）。
void fb_flip_region(int x, int y, int w, int h) {
    if (!g_kernel_paint64) return;              // ★ A1：内核侧提交关闭
    fb_blit_region64(x, y, w, h);
}
// A1：**用户程序**的区域提交（fb_flip(10) 的实现）。与 fb_flip_region 的区别只有两点：
//   1) 不被"内核侧绘制开关"挡住 —— 这是用户自己的提交；
//   2) 坐标在这里**再夹一次**（syscall 侧已经夹过，双保险：不越界、不崩）。
void fb_user_flip64(int x, int y, int w, int h) {
    // 夹取在 fb_blit_region64 里做（负 x/y 夹到 0、w/h 夹到边界），这里只转交。
    fb_blit_region64(x, y, w, h);
}

// ★ 驱动线 3：显示后端查询 / 打点 / **强制软件路径**提交（三者都不改变默认行为）
const char* fb_backend_name64() { return vgpu64_backend_name64(); }

// 一行"当前显示后端"（规格要求：启动期打一行 [FB64] backend=...）。几何不在这里重复打
// （[G64] fb render= / [HWUI] 显示区块都有）—— 少一次内联 dbg64 展开就是几百字节（余量很紧）。
void fb_backend_log64(const char* backend) {
    dbg64_line_begin64();
    dbg64_str("[FB64] backend=");
    dbg64_str(backend ? backend : "?");
    dbg64_nl();
    dbg64_line_end64();
}

// 强制走软件路径的区域提交：**不看后端**（设备自检要的就是"软件路径的屏幕内容"）。
void fb_soft_flip_region64(int x, int y, int w, int h) {
    if (!backbuf || !fb_addr || !fb_usable) return;
    if (g_zoom != 100) { fb_flip_all64(); return; }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;
    if (fb_bpp == 32 && fb_pitch == fb_w * 4) {
        uint32_t* dst = (uint32_t*)(uintptr_t)fb_addr;
        for (int row = 0; row < h; row++) {
            uint32_t* d = dst + (y + row) * fb_w + x;
            uint32_t* s = backbuf + (y + row) * fb_w + x;
            for (int i = 0; i < w; i++) d[i] = s[i];
        }
    } else {
        for (int yy = y; yy < y + h; yy++) {
            volatile uint8_t* row = (volatile uint8_t*)(uintptr_t)(fb_addr + (uint32_t)yy * (uint32_t)fb_pitch);
            for (int xx = x; xx < x + w; xx++) {
                uint32_t c = backbuf[yy * fb_w + xx];
                if (fb_bpp == 32)      ((volatile uint32_t*)row)[xx] = c;
                else if (fb_bpp == 24) { row[xx * 3] = c & 0xFF; row[xx * 3 + 1] = (c >> 8) & 0xFF; row[xx * 3 + 2] = (c >> 16) & 0xFF; }
                else                   ((volatile uint16_t*)row)[xx] = (uint16_t)(((c >> 16 & 0xF8) << 8) | ((c >> 8 & 0xFC) << 3) | (c >> 3));
            }
        }
    }
    fb64_sfence64();   // ★ WC 写合并缓冲落地
    if (vmware64_update64) vmware64_update64(x, y, w, h);             // VMware：这块脏了
    fb64_mixsample64();                                                // 诊断：bb vs lfb 逐点比对
    if (vmware64_lfbprobe_tick64) vmware64_lfbprobe_tick64();          // 诊断：直接刷 LFB 彩条
    vgpu64_note_soft_blit64(x, y, w, h);
}

// ---- 逐像素绘制（全部走后备缓冲；受内核侧绘制开关 + 裁剪矩形约束）----
static inline void putpx(int x, int y, uint32_t color) {
    if (!g_kernel_paint64) return;              // ★ A1：内核侧绘制关闭时一律不画
    if (x < 0 || y < 0 || x >= g_render_w || y >= g_render_h) return;
    if (g_clip_on && (x < g_clip_x0 || y < g_clip_y0 || x >= g_clip_x1 || y >= g_clip_y1)) return;
    if (backbuf) {
        backbuf[y * g_render_w + x] = color;   // 全部绘制到后备缓冲（渲染分辨率）
    }
}

void fb_putpixel(int x, int y, uint32_t color) { putpx(x, y, color); }

void fb_clear(uint32_t color) {
    if (!backbuf) return;
    if (!g_kernel_paint64) return;              // ★ A1：内核侧绘制关闭（清屏也算绘制）
    // ★ 清屏必须**尊重裁剪**。安装程序为了鼠标跟手改成"脏矩形重绘"（鼠标移动时只重画
    //   光标新旧位置那一小块），如果这里无视裁剪整屏清屏，那一小块的背景就画不回来
    //   （表现为光标拖影、整屏闪烁），局部重绘也就白做了。
    int x0 = g_clip_on ? g_clip_x0 : 0;
    int y0 = g_clip_on ? g_clip_y0 : 0;
    int x1 = g_clip_on ? g_clip_x1 : g_render_w;
    int y1 = g_clip_on ? g_clip_y1 : g_render_h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_render_w) x1 = g_render_w;
    if (y1 > g_render_h) y1 = g_render_h;
    const int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return;
    // 逐行填充（比逐像素 putpx 快得多：整屏重绘时这一项就能省下大量时间）
    uint32_t* base = backbuf + y0 * g_render_w + x0;
    for (int yy = 0; yy < h; yy++) {
        uint32_t* row = base + yy * g_render_w;
        for (int xx = 0; xx < w; xx++) row[xx] = color;
    }
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!clip_rect(&x, &y, &w, &h)) return;
    if (backbuf) {
        // 快速路径：直接写后备缓冲（32bpp 统一）
        uint32_t* base = backbuf + y * g_render_w + x;
        for (int yy = 0; yy < h; yy++) {
            uint32_t* row = base + yy * g_render_w;
            for (int xx = 0; xx < w; xx++) row[xx] = color;
        }
        return;
    }
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            putpx(xx, yy, color);
}

// 半透明填充：对后备缓冲现有像素做 alpha 混合（alpha 0-255，越大越不透明）
void fb_fill_rect_alpha(int x, int y, int w, int h, uint32_t color, int alpha) {
    if (!clip_rect(&x, &y, &w, &h)) return;
    if (alpha > 255) alpha = 255;
    if (alpha <= 0) return;
    if (!backbuf) return;
    uint8_t nr = (color >> 16) & 0xFF, ng = (color >> 8) & 0xFF, nb = color & 0xFF;
    int inv = 255 - alpha;
    for (int yy = y; yy < y + h; yy++) {
        uint32_t* row = &backbuf[yy * g_render_w + x];
        for (int xx = 0; xx < w; xx++) {
            uint32_t old = row[xx];
            uint8_t orr = (old >> 16) & 0xFF, og = (old >> 8) & 0xFF, ob = old & 0xFF;
            uint8_t r = (uint8_t)((orr * inv + nr * alpha) / 255);
            uint8_t g = (uint8_t)((og * inv + ng * alpha) / 255);
            uint8_t b = (uint8_t)((ob * inv + nb * alpha) / 255);
            row[xx] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
}

void fb_draw_hline(int x, int y, int w, uint32_t color) { fb_fill_rect(x, y, w, 1, color); }
void fb_draw_vline(int x, int y, int h, uint32_t color) { fb_fill_rect(x, y, 1, h, color); }

void fb_draw_rect(int x, int y, int w, int h, uint32_t color) {
    fb_draw_hline(x, y, w, color);
    fb_draw_hline(x, y + h - 1, w, color);
    fb_draw_vline(x, y, h, color);
    fb_draw_vline(x + w - 1, y, h, color);
}

void fb_draw_rect_fill(int x, int y, int w, int h, uint32_t color) {
    fb_fill_rect(x, y, w, h, color);
}

void fb_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg, int scale) {
    int idx = (unsigned char)c - 32;
    if (idx < 0 || idx >= 95) return;
    if (scale < 1) scale = 1;
    const unsigned char* glyph = font8x8[idx];
    for (int row = 0; row < 8; row++) {
        unsigned char bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            bool on = (bits >> (7 - col)) & 1;
            uint32_t color = on ? fg : bg;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    putpx(x + col * scale + sx, y + row * scale + sy, color);
        }
    }
}

void fb_draw_text(int x, int y, const char* s, uint32_t fg, uint32_t bg, int scale) {
    int cx = x;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c == '\n') { cx = x; y += 8 * scale; continue; }
        fb_draw_char(cx, y, c, fg, bg, scale);
        cx += 8 * scale;
    }
}

void fb_draw_text_n(int x, int y, const char* s, int n, uint32_t fg, uint32_t bg, int scale) {
    for (int i = 0; i < n && s[i]; i++) {
        fb_draw_char(x, y, s[i], fg, bg, scale);
        x += 8 * scale;
    }
}

// ---- 镜像版文字（字符水平翻转后绘制）----
static uint8_t fb_flip_byte(uint8_t v) {
    uint8_t r = 0;
    for (int b = 0; b < 8; b++) r |= ((v >> b) & 1) << (7 - b);
    return r;
}

void fb_draw_char_mirror(int x, int y, char c, uint32_t fg, uint32_t bg, int scale) {
    int idx = (unsigned char)c - 32;
    if (idx < 0 || idx >= 95) return;
    if (scale < 1) scale = 1;
    const unsigned char* glyph = font8x8[idx];
    for (int row = 0; row < 8; row++) {
        unsigned char bits = fb_flip_byte(glyph[row]);
        for (int col = 0; col < 8; col++) {
            bool on = (bits >> (7 - col)) & 1;
            uint32_t color = on ? fg : bg;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    putpx(x + col * scale + sx, y + row * scale + sy, color);
        }
    }
}

void fb_draw_text_mirror(int x, int y, const char* s, uint32_t fg, uint32_t bg, int scale) {
    int cx = x;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c == '\n') { cx = x; y += 8 * scale; continue; }
        fb_draw_char_mirror(cx, y, c, fg, bg, scale);
        cx += 8 * scale;
    }
}

// 16x16 位图（1=前景色，0=透明），用于鼠标光标等
void fb_draw_bitmap16(int x, int y, const uint16_t* bmp, int w, int h, uint32_t color) {
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            if (bmp[yy] & (1 << (15 - xx))) {
                putpx(x + xx, y + yy, color);
            }
        }
    }
}

// RGBA 图像绘制到后备缓冲（假设背景为黑色，直接按 alpha 混合）
void fb_blit_rgba(int x, int y, const uint8_t* rgba, int w, int h) {
    if (!backbuf) return;
    if (!g_kernel_paint64) return;              // ★ A1：内核侧绘制关闭（RGBA 直写后备缓冲也算绘制）
    int by0 = 0, by1 = h;      // 行裁剪范围
    if (g_clip_on) {
        if (y < g_clip_y0) by0 = g_clip_y0 - y;
        if (y + by1 > g_clip_y1) by1 = g_clip_y1 - y;
    }
    for (int j = by0; j < by1; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= g_render_h) continue;
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= g_render_w) continue;
            if (g_clip_on && (xx < g_clip_x0 || xx >= g_clip_x1)) continue;
            uint8_t r = rgba[0], g = rgba[1], b = rgba[2], a = rgba[3];
            rgba += 4;
            if (a == 0) continue;
            uint32_t c;
            if (a == 255) {
                c = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            } else {
                c = 0xFF000000u | ((uint32_t)(r * a / 255) << 16)
                                | ((uint32_t)(g * a / 255) << 8)
                                | ((uint32_t)(b * a / 255));
            }
            backbuf[yy * g_render_w + xx] = c;
        }
    }
}

// ★ 驱动线 3：读 LFB 的像素（自检用；只支持 32bpp，别的 bpp 返回 0）。
uint32_t fb_lfb_pixel64(int x, int y) {
    if (!fb_addr || x < 0 || y < 0 || x >= fb_w || y >= fb_h) return 0;
    if (fb_bpp != 32) return 0;
    return ((volatile uint32_t*)(uintptr_t)fb_addr)[y * (fb_pitch / 4) + x];
}

uint32_t fb_get_pixel(int x, int y) {
    if (x < 0 || y < 0 || x >= g_render_w || y >= g_render_h) return 0;
    if (backbuf) return backbuf[y * g_render_w + x];
    if (fb_bpp == 32) return *(volatile uint32_t*)(fb_addr + y * fb_pitch + x * 4);
    return 0;
}

// ==================== ★ 用户计划 ⑭：VSync + 交换链 + 混合渲染（GPU 主 / CPU 次）====================
// 全部行为由 QEMU fw_cfg 的 "opt/vimtu/vsync" 配置串开启（缺省：立刻返回 0，零打点零副作用，
// 既有启动路径一个字节都不变）。为什么用 fw_cfg 当配置通道：验收脚本只能改 QEMU 命令行
// （-fw_cfg）；本仓库的 loader 不传 cmdline，fw_cfg 端口 0x510/0x511 在 QEMU 上恒在，
// 没有该通道的机器（VMware/真机）读回 0xFF -> 条目数越界 -> 直接放弃（有界）。
//
// 配置串（';' 分隔；值里不许有逗号 —— QEMU 的 -fw_cfg 按逗号切属性）：
//   demo=1              开帧引擎（缺省 0 = 关）
//   bufs=2|3            交换链块数（缺省 2）
//   gpu=auto|on|off     设备路径策略（缺省 auto = 有设备就用）
//   frames=N            跑多少帧（缺省 240）
//   fps=N               目标刷新率（缺省 60）
//   inject=none|timeout|illegal|gone   故障注入种类（缺省 none）
//   inj_at=N            第几个 virtio-gpu 命令之后开始注入（缺省 120）
//   bench=0|1           末尾做"设备 vs 软件整帧提交"耗时对比（缺省 1）

// ---- 打点小工具（非内联：一份实现。体积纪律与 virtio_gpu64.cpp 相同）----
__attribute__((noinline)) static void fb64_puts(const char* s) { dbg64_str(s); }
__attribute__((noinline)) static void fb64_num(uint64_t v) { dbg64_dec(v); }
static void fb64_log_begin() { dbg64_line_begin64(); }
static void fb64_log_end() { dbg64_nl(); dbg64_line_end64(); }

static inline uint64_t fb64_rdtsc(void) {
    uint32_t lo = 0, hi = 0;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}
static int fb64_irqs_on(void) {                       // hlt 只在 IF=1 时用（否则会睡死）
    uint64_t fl = 0;
    __asm__ volatile("pushfq; popq %0" : "=r"(fl));
    return (fl & 0x200ull) ? 1 : 0;
}

// ---------- ⑭-1 配置通道：fw_cfg 文件目录（键 0x19）+ 一条字符串文件 ----------
// 【实测踩坑】selector 是"大端 16 位"，但 0x510/0x511 两个字节里低字节在 0x511（与数据口
// 重叠）；QEMU 11 对"两次 8 位写"的解释跟老版本/文档写法不一定一致（本批第一次实测就没读到
// 目录：读回的条目数越界 -> 直接放弃 -> 帧引擎一行不打）。所以这里**四种写法都试一遍**，
// 谁能让目录条目数落到 1..64 就固定用它（每种只读 4 字节，有界，绝不挂）。
static uint8_t fb64_fwcfg_rd8(void) { return inb(0x511); }
static uint32_t fb64_fwcfg_rd32(void) {               // fw_cfg 的数字字段一律大端
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = (v << 8) | (uint32_t)fb64_fwcfg_rd8();
    return v;
}
static int g_fwcfg_variant = -1;                      // -1 = 还没探测出来
// ★ 真缺陷修复（本批修订）：原来"选条目"和"读目录条目数（4B）"是同一个函数 —— 于是**每选一次
//   都会从数据口吃掉前 4 字节**。对目录（0x0019）这正好是条目数（本意），但对**一般文件**就是
//   文件的前 4 个字节：配置串 "demo=1;bufs=2;…" 的前 4 字节 "demo" 被吃掉 ->
//   fb64_cfg_find("demo") 在整个串里都找不到 -> `[VSYNC] cfg present but demo=0` ->
//   **帧引擎在任何机器上都起不来**（①~④ 的实测数据一个都拿不到；实测：加 4 字节前缀
//   "xxxx;demo=1;…" 就正常起来 —— 那就是本缺陷的硬证据）。
//   现在拆开："只切换"与"切换+读条目数"，探测/读目录走后者（语义不变），读一般文件走前者。
static void fb64_fwcfg_sel_write(int variant, uint16_t sel) {
    switch (variant) {
        case 0: outb(0x510, (uint8_t)(sel >> 8)); outb(0x510, (uint8_t)(sel & 0xFF)); break;
        case 1: outb(0x510, (uint8_t)(sel & 0xFF)); outb(0x510, (uint8_t)(sel >> 8)); break;
        case 2: outw(0x510, (uint16_t)((sel << 8) | (sel >> 8))); break;
        default: outw(0x510, sel); break;
    }
}
static int fb64_fwcfg_sel_try(int variant, uint16_t sel) {
    fb64_fwcfg_sel_write(variant, sel);
    const uint32_t cnt = fb64_fwcfg_rd32();           // 目录首 4 字节 = 条目数（大端）
    return (cnt >= 1 && cnt <= 64) ? (int)cnt : -1;
}
static void fb64_fwcfg_select(uint16_t sel) {         // 用已确定的那种写法选条目
    fb64_fwcfg_sel_write(g_fwcfg_variant, sel);       // ★ 不读数据口：第 0 字节 = 文件第 0 字节
}
// 探测：返回 1 = 找到可用的写法（g_fwcfg_variant 固定下来），0 = 这台机器没有可读的 fw_cfg
static int fb64_fwcfg_probe(void) {
    for (int v = 0; v < 4; v++) {
        if (fb64_fwcfg_sel_try(v, 0x0019) > 0) { g_fwcfg_variant = v; return 1; }
    }
    return 0;
}
// 找一条名字匹配的文件（name = "opt/vimtu/vsync"）。返回 1 = 找到（*sel/*size 填好）。
// 有界：条目数上界 64、每条固定 64 字节 -> 最多 64 次。
static int fb64_fwcfg_find(const char* name, uint16_t* sel, uint32_t* size) {
    if (g_fwcfg_variant < 0 && !fb64_fwcfg_probe()) return 0;
    const int cnt = fb64_fwcfg_sel_try(g_fwcfg_variant, 0x0019);
    if (cnt < 1) return 0;
    for (int i = 0; i < cnt; i++) {
        const uint32_t sz = fb64_fwcfg_rd32();                   // 文件长度（大端）
        const uint16_t s = (uint16_t)(((uint32_t)fb64_fwcfg_rd8() << 8) | (uint32_t)fb64_fwcfg_rd8());
        (void)fb64_fwcfg_rd8(); (void)fb64_fwcfg_rd8();          // reserved（2B）
        char nm[56];
        for (int k = 0; k < 56; k++) nm[k] = (char)fb64_fwcfg_rd8();
        nm[55] = 0;                                              // 保证有结尾
        int eq = 1;
        for (int k = 0; ; k++) {
            if (nm[k] != name[k]) { eq = 0; break; }
            if (!name[k]) break;
        }
        if (eq) { *sel = s; *size = sz; return 1; }
    }
    return 0;
}
static char g_vs_cfg[192];
static int  fb64_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }
static void fb64_cfg_load(void) {
    g_vs_cfg[0] = 0;
    uint16_t sel = 0; uint32_t sz = 0;
    if (!fb64_fwcfg_find("opt/vimtu/vsync", &sel, &sz)) return;
    if (sz == 0 || sz > sizeof(g_vs_cfg) - 1) return;
    fb64_fwcfg_select(sel);
    int n = 0;
    for (uint32_t i = 0; i < sz; i++) {
        const char c = (char)fb64_fwcfg_rd8();
        if (!c) break;
        g_vs_cfg[n++] = c;
    }
    g_vs_cfg[n] = 0;
}
static const char* fb64_cfg_find(const char* key) {
    const int kl = fb64_strlen(key);
    const char* p = g_vs_cfg;
    while (*p) {
        int m = 1;
        for (int i = 0; i < kl; i++) if (p[i] != key[i]) { m = 0; break; }
        if (m && p[kl] == '=') return p + kl + 1;
        while (*p && *p != ';') p++;
        if (*p == ';') p++;
    }
    return nullptr;
}
static int fb64_cfg_int(const char* key, int def) {
    const char* p = fb64_cfg_find(key);
    if (!p) return def;
    int v = 0, any = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (int)(*p - '0'); p++; any = 1; }
    return any ? v : def;
}
// ==================== ★ v0.9.2：通用 fw_cfg 开关 ====================
// 与 fb64_cfg_load() 的区别：那个只认死文件名 opt/vimtu/vsync（帧引擎专用）。这里按文件名查，
// 让"启动期屏上测试图/演示"之类的东西也能用 fw_cfg 做 opt-in —— 判据是**显式配置才开**，
// 没有该文件一律返回 def（默认 0 = 关）。每次调用重读（只在启动早期/验收路径调，条目数上界 64，成本有界）。
// ★ v0.9.8：按文件名查 fw_cfg 文件、取 key= 之后到 ';' 为止的**字符串值**，写进 out（含结尾 0）。
//   返回 1 = 找到；0 = 没有该文件/该键（*out 一律置空，调用方据此回退到默认值）。
//   为什么需要它：`opt/vimtu/gpu` 的开关是 `mode=intel_safe` 这种**枚举串**，不是整数；
//   老的 fb64_fwcfg_flag64() 只认数字，硬套会把它读成 def，等于开关失灵。
//   有界：条目数上界 64、文件长度上界 191、值长度上界 cap-1。
int fb64_fwcfg_str64(const char* file, const char* key, char* out, int cap) {
    if (!out || cap <= 0) return 0;
    out[0] = 0;
    uint16_t sel = 0; uint32_t sz = 0;
    char buf[192];
    if (!fb64_fwcfg_find(file, &sel, &sz)) return 0;
    if (sz == 0 || sz > sizeof(buf) - 1) return 0;
    fb64_fwcfg_select(sel);
    int n = 0;
    for (uint32_t i = 0; i < sz; i++) {
        const char c = (char)fb64_fwcfg_rd8();
        if (!c) break;
        buf[n++] = c;
    }
    buf[n] = 0;
    const int kl = fb64_strlen(key);
    const char* p = buf;
    while (*p) {
        int m = 1;
        for (int i = 0; i < kl; i++) if (p[i] != key[i]) { m = 0; break; }
        if (m && p[kl] == '=') {
            p += kl + 1;
            int o = 0;
            while (*p && *p != ';' && o < cap - 1) out[o++] = *p++;
            out[o] = 0;
            return 1;
        }
        while (*p && *p != ';') p++;
        if (*p == ';') p++;
    }
    return 0;
}

// 整数版（v0.9.2 起的老接口）：改成复用上面的字符串版，省掉一份重复的扫描代码。
int fb64_fwcfg_flag64(const char* file, const char* key, int def) {
    char v[24];
    if (!fb64_fwcfg_str64(file, key, v, (int)sizeof(v))) return def;
    int n = 0, any = 0;
    while (v[n] >= '0' && v[n] <= '9') { n++; any = 1; }
    if (!any) return def;
    int out = 0;
    for (int i = 0; i < n; i++) out = out * 10 + (int)(v[i] - '0');
    return out;
}

static int fb64_cfg_str(const char* key, const char* val) {      // 值到 ';' 或串尾为止
    const char* p = fb64_cfg_find(key);
    if (!p) return 0;
    for (int i = 0; ; i++) {
        const char a = p[i];
        const char ea = (a == ';' || a == 0) ? 0 : a;
        if (ea != val[i]) return 0;
        if (!val[i]) break;
    }
    return 1;
}

// ---------- ⑭-2 交换链（后备缓冲切成 2..3 块；整帧提交）----------
#define FB64_SWAP_MAX 3
static uint32_t* g_sw_slot[FB64_SWAP_MAX] = { nullptr, nullptr, nullptr };
static int g_sw_n = 1;          // 块数（1 = 没开：行为与改动前完全一致）
static int g_sw_front = 0;      // 在屏上的块
static int g_sw_back = 0;       // 当前绘制块（== backbuf）
static int g_sw_allow_gpu = 1;  // 帧引擎允许设备路径（gpu=off -> 0）

int fb_swap_count64() { return g_sw_n; }
int fb_swap_front64() { return g_sw_front; }

int fb_swap_enable64(int n) {
    const int w = fb_width(), h = fb_height();
    if (w <= 0 || h <= 0) return 1;
    const uint64_t frame_px = (uint64_t)w * (uint64_t)h;
    int maxn = (int)((uint64_t)(sizeof(backbuf_storage) / sizeof(backbuf_storage[0])) / frame_px);
    if (maxn > FB64_SWAP_MAX) maxn = FB64_SWAP_MAX;
    if (n > maxn) n = maxn;
    if (n < 2) { g_sw_n = 1; backbuf = backbuf_storage; return 1; }
    g_sw_n = n;
    for (int i = 0; i < n; i++) g_sw_slot[i] = backbuf_storage + (uint64_t)i * frame_px;
    g_sw_front = 0;                    // 0 号块就是"当前在屏上的内容"（设备路径 = primary resource）
    g_sw_back = 1;                     // 先画 1 号块 —— 绝不动正在被扫描出的那块
    backbuf = g_sw_slot[g_sw_back];
    return n;
}

// 软件路径整帧提交：**原地**写 LFB（Bochs VBE 没有页翻转原语；宿主扫描可能看到中间态 -> 如实标）
static void fb64_present_soft(int slot) {
    if (!fb_addr || !fb_usable) return;
    uint32_t* src = g_sw_slot[slot];
    if (!src) return;
    const int w = fb_w, h = fb_h;
    if (g_zoom != 100) {                       // 缩放：走既有整帧路径（按 render 尺寸放大）
        uint32_t* save = backbuf;
        backbuf = src;
        fb_flip_all64();
        backbuf = save;
        return;
    }
    if (fb_bpp == 32 && fb_pitch == w * 4) {
        uint32_t* dst = (uint32_t*)fb_addr;
        const uint32_t n = (uint32_t)w * (uint32_t)h;
        for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
    } else {
        for (int y = 0; y < h; y++) {
            volatile uint8_t* row = (volatile uint8_t*)(fb_addr + y * fb_pitch);
            for (int x = 0; x < w; x++) {
                const uint32_t c = src[(uint64_t)y * (uint64_t)w + (uint64_t)x];
                if (fb_bpp == 32) ((volatile uint32_t*)row)[x] = c;
                else if (fb_bpp == 24) { row[x * 3] = c & 0xFF; row[x * 3 + 1] = (c >> 8) & 0xFF; row[x * 3 + 2] = (c >> 16) & 0xFF; }
                else ((volatile uint16_t*)row)[x] = (uint16_t)(((c >> 16 & 0xF8) << 8) | ((c >> 8 & 0xFC) << 3) | (c >> 3));
            }
        }
    }
    vgpu64_note_soft_blit64(0, 0, w, h);
}

static int g_sw_degrade_logged = 0;

// 整帧提交当前绘制块：设备路径 = 整帧 TRANSFER+FLUSH（+ 需要时 SET_SCANOUT 翻页）；
// 失败/不可用 -> 本帧自动走软件路径（**继续出帧**，绝不 PANIC / 绝不挂死）。
int fb_frame_present64() {
    if (g_sw_n < 2) { fb_flip(); return 0; }
    const int slot = g_sw_back;
    int dev = 0;
    if (g_sw_allow_gpu && vgpu64_ready64()) {
        dev = vgpu64_present64(slot, fb_width(), fb_height()) ? 1 : 0;
        if (!dev && !g_sw_degrade_logged) {
            g_sw_degrade_logged = 1;
            fb64_log_begin();
            fb64_puts("[FB64] present dev=0 -> soft-lfb (帧引擎自动降级，继续出帧)");
            fb64_log_end();
        }
    }
    if (!dev) fb64_present_soft(slot);
    g_sw_front = slot;
    g_sw_back = (slot + 1) % g_sw_n;
    backbuf = g_sw_slot[g_sw_back];
    return dev;
}

// ---------- ⑭-3 帧节拍：硬件回扫（有则用）/ 节拍器（估计 + 粗睡 + 细自旋）----------
static uint64_t g_vs_cyc_per_ms = 0;
static uint64_t fb64_calib_cyc_per_ms(void) {
    const uint64_t t0 = g_ticks64, c0 = fb64_rdtsc();
    uint32_t spin = 0;
    while (g_ticks64 - t0 < 20 && ++spin < 200000000u) __asm__ volatile("pause");   // 80 ms
    const uint64_t dt = g_ticks64 - t0;
    if (!dt) return 0;
    return (fb64_rdtsc() - c0) * (uint64_t)TICK_MS_64 / dt;
}
// VGA 输入状态寄存器 1（0x3DA）bit3 = 垂直回扫/垂直同步期
static inline int fb64_vga_vretrace(void) { return (inb(0x3DA) & 0x08) ? 1 : 0; }
// 实测 200 ms 里的上升沿数；返回 1 = 信号可信（能用硬件节拍）
static int fb64_vblank_probe(uint64_t* edges_out, uint64_t* ms_out) {
    const uint64_t t0 = g_ticks64;
    uint64_t edges = 0;
    int last = fb64_vga_vretrace();
    uint32_t spin = 0;
    while (g_ticks64 - t0 < 50) {                       // 50 tick = 200 ms
        const int v = fb64_vga_vretrace();
        if (v && !last) edges++;
        last = v;
        if (++spin > 200000000u) break;                 // 有界
    }
    const uint64_t dt = g_ticks64 - t0;
    *edges_out = edges;
    const uint64_t ms = dt * (uint64_t)TICK_MS_64;
    *ms_out = ms;
    if (dt == 0) return 0;
    const uint64_t hz = edges * 1000ull / ms;
    return (edges >= 5 && hz >= 25 && hz <= 300) ? 1 : 0;      // 25..300 Hz 才算可信
}
// 等下一次垂直回扫**起点**（先等当前回扫结束，再等下一个上升沿）。有界（2 tick = 8 ms 上限）。
static int fb64_vblank_wait(void) {
    const uint64_t t0 = g_ticks64;
    uint32_t spin = 0;
    while (fb64_vga_vretrace()) {                       // ① 当前回扫期内：等它结束
        if (g_ticks64 - t0 > 2 || ++spin > 20000000u) return 0;
    }
    spin = 0;
    while (!fb64_vga_vretrace()) {                      // ② 等下一次回扫开始（= 新一帧的起点）
        if (g_ticks64 - t0 > 2 || ++spin > 20000000u) return 0;
    }
    return 1;
}
// ★ v0.9.12：DRM/KMS 门面的复用入口（薄的转发，逻辑仍在本文件的 static 函数里 ——
//   一处实现，两个调用方：既有的 vsync 帧引擎 + 新的 kernel/drm64.cpp）。
int fb64_vblank_probe_export64(uint64_t* edges_out, uint64_t* ms_out) {
    uint64_t e = 0, ms = 0;
    const int r = fb64_vblank_probe(&e, &ms);
    if (edges_out) *edges_out = e;
    if (ms_out)    *ms_out = ms;
    return r;
}
int fb64_vblank_wait_export64(void) { return fb64_vblank_wait(); }
// 节拍器：粗等用 hlt 睡到"还剩 1 ms"，细等用 rdtsc 自旋（对齐到目标时刻）
static void fb64_wait_until(uint64_t deadline_cyc) {
    if (fb64_irqs_on()) {
        for (int guard = 0; guard < 64; guard++) {
            const uint64_t now = fb64_rdtsc();
            if ((int64_t)(deadline_cyc - now) <= (int64_t)g_vs_cyc_per_ms) break;
            __asm__ volatile("hlt");
        }
    }
    uint64_t spin = 0;
    while ((int64_t)(deadline_cyc - fb64_rdtsc()) > 0) {
        __asm__ volatile("pause");
        if (++spin > 4000000000u) break;                // 兜底（时钟异常时不死循环）
    }
}

// ---------- ⑭-4 帧统计（中位数 / p95 / 抖动）+ 8 条色带图案 ----------
#define FB64_GAP_MAX 1024
static uint64_t g_vs_gap[FB64_GAP_MAX];
static int      g_vs_gap_n = 0;
static uint64_t g_vs_frames = 0;
static uint64_t g_vs_med = 0, g_vs_p95 = 0, g_vs_jit = 0;
static const char* g_vs_mode = "off";
static uint64_t g_vs_gpu_frames = 0, g_vs_cpu_frames = 0;

static uint64_t fb64_stat_med(uint64_t* a, int n) {     // 插入排序 + 取中位（一次性，n<=1024）
    for (int i = 1; i < n; i++) {
        const uint64_t v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
    return n ? a[n / 2] : 0;
}
static const uint32_t g_vs_pal[8] = {
    0xFF0000u, 0x00FF00u, 0x0000FFu, 0xFFFF00u, 0xFF00FFu, 0x00FFFFu, 0xFF8000u, 0x8000FFu,
};
// 第 f 帧的图案：第 i 条带的颜色 = pal[(i + f) % 8]（帧与帧之间**整帧都变**；验收脚本靠这 8 个
// 颜色反推"这一屏属于哪一帧"——同一张截图里出现两个不同帧的带序 = 撕裂）。
static void fb64_draw_bands(uint32_t* buf, int w, int h, uint64_t f) {
    for (int b = 0; b < 8; b++) {
        const uint32_t c = g_vs_pal[(uint32_t)((b + (int)(f & 7ull)) & 7)];
        const int y0 = (int)((int64_t)h * b / 8);
        const int y1 = (int)((int64_t)h * (b + 1) / 8);
        for (int y = y0; y < y1; y++) {
            uint32_t* row = buf + (uint64_t)y * (uint64_t)w;
            for (int x = 0; x < w; x++) row[x] = c;
        }
    }
}

static const char* fb64_inj_name(int k) {
    switch (k) {
        case 1: return "timeout";
        case 2: return "illegal";
        case 3: return "gone";
        default: return "none";
    }
}

// 末尾对比：同一帧内容，设备整帧提交 vs 软件整帧提交（rdtsc 多轮中位数）
static void fb64_bench_present(int slot, int w, int h, int n_runs) {
    uint64_t dev_cyc[9], soft_cyc[9];
    if (n_runs > 9) n_runs = 9;
    for (int i = 0; i < n_runs; i++) {
        uint64_t c0 = fb64_rdtsc();
        const int ok = (g_sw_allow_gpu && vgpu64_ready64()) ? vgpu64_present64(slot, w, h) : 0;
        dev_cyc[i] = ok ? (fb64_rdtsc() - c0) : 0;
        c0 = fb64_rdtsc();
        fb64_present_soft(slot);
        soft_cyc[i] = fb64_rdtsc() - c0;
    }
    const uint64_t md = fb64_stat_med(dev_cyc, n_runs);
    const uint64_t ms_ = fb64_stat_med(soft_cyc, n_runs);
    const uint64_t to_us = g_vs_cyc_per_ms ? (g_vs_cyc_per_ms / 1000ull) : 0;
    fb64_log_begin();
    fb64_puts("[VSYNC] bench present_n="); fb64_num((uint64_t)n_runs);
    fb64_puts(" px="); fb64_num((uint64_t)w * (uint64_t)h);
    if (to_us) {
        fb64_puts(" gpu_med_us="); fb64_num(md / to_us);
        fb64_puts(" soft_med_us="); fb64_num(ms_ / to_us);
        fb64_puts(" ratio_x100="); fb64_num(ms_ ? (md * 100ull / ms_) : 0);
    } else {
        fb64_puts(" gpu_med_cyc="); fb64_num(md);
        fb64_puts(" soft_med_cyc="); fb64_num(ms_);
    }
    fb64_log_end();
}

// ==================== 帧引擎主体 ====================
__attribute__((noinline)) static int fb_vsync_engine64(int bufs, int gpu_mode, int frames,
                                                       int fps, int bench_on) {
    const int w = fb_width(), h = fb_height();
    const int n_sw = fb_swap_enable64(bufs);
    if (n_sw < 2) {
        fb64_log_begin();
        fb64_puts("[VSYNC] swap unavailable frames=1 (后备缓冲放不下两块) -> skip");
        fb64_log_end();
        return 0;
    }
    g_sw_allow_gpu = (gpu_mode == 0) ? 0 : 1;          // gpu=off 强制软件路径
    // 设备路径：设备可用 + 交换链每块一个 resource（SET_SCANOUT 翻页）
    int dev_frames = g_sw_allow_gpu ? vgpu64_swap_init64(n_sw) : 0;
    if (dev_frames < 2) dev_frames = 0;
    const int dev_on = dev_frames ? 1 : 0;
    if (!dev_on) g_sw_allow_gpu = 0;                   // 交换链不成立就别每帧白试设备
    if (gpu_mode == 1 && !dev_on) {
        fb64_log_begin();
        fb64_puts("[VSYNC] gpu=on but device/swap unavailable -> cpu path (如实降级)");
        fb64_log_end();
    }
    if (fps < 1) fps = 60;
    if (fps > 240) fps = 240;
    const uint64_t target_us = 1000000ull / (uint64_t)fps;
    const uint64_t period_cyc = g_vs_cyc_per_ms ? (g_vs_cyc_per_ms * 1000ull / (uint64_t)fps) : 0;

    // 帧节拍来源：先探硬件垂直回扫；没有就标 mode=pace（按刷新率估计 + 睡眠/自旋混合）
    uint64_t vb_edges = 0, vb_ms = 0;
    const int hw = fb64_vblank_probe(&vb_edges, &vb_ms);
    g_vs_mode = hw ? "hw" : "pace";

    fb64_log_begin();
    fb64_puts("[VSYNC] demo start mode="); fb64_puts(g_vs_mode);
    fb64_puts(" bufs="); fb64_num((uint64_t)n_sw);
    fb64_puts(" gpu="); fb64_puts(gpu_mode == 1 ? "on" : (gpu_mode == 0 ? "off" : "auto"));
    fb64_puts(" backend="); fb64_puts(vgpu64_backend_name64());
    fb64_puts(" dev_frames="); fb64_num((uint64_t)dev_frames);
    fb64_puts(" target_us="); fb64_num(target_us);
    fb64_puts(" frames="); fb64_num((uint64_t)frames);
    fb64_puts(" vblank_edges="); fb64_num(vb_edges);
    fb64_puts(" vblank_ms="); fb64_num(vb_ms);
    fb64_puts(" in_place="); fb64_num(dev_on ? 0ull : 1ull);
    fb64_log_end();

    uint64_t t_next = fb64_rdtsc();
    uint64_t t_prev = 0;
    int path_prev = -1;
    const uint64_t start_cyc = fb64_rdtsc();
    const uint64_t to_us = g_vs_cyc_per_ms ? (g_vs_cyc_per_ms / 1000ull) : 0;
    for (int f = 0; f < frames; f++) {
        if (hw) (void)fb64_vblank_wait();
        else if (period_cyc) fb64_wait_until(t_next);
        const uint64_t t0 = fb64_rdtsc();
        fb64_draw_bands(backbuf, w, h, (uint64_t)f);            // CPU：逻辑 + 像素（8 条色带整体变化）
        const uint64_t t1 = fb64_rdtsc();
        const int dev = fb_frame_present64();                   // 整帧提交（GPU 优先 / 失败降级 CPU）
        const uint64_t t2 = fb64_rdtsc();
        const uint64_t gap = t_prev ? (t0 - t_prev) : 0;
        if (gap && g_vs_gap_n < FB64_GAP_MAX) g_vs_gap[g_vs_gap_n++] = to_us ? (gap / to_us) : gap;
        t_prev = t0;
        g_vs_frames++;
        if (dev) g_vs_gpu_frames++; else g_vs_cpu_frames++;
        // 逐帧打点（有界：头 4 帧 + 每 100 帧 + 最后一帧）
        if (f < 4 || (f % 100) == 0 || f == frames - 1) {
            fb64_log_begin();
            fb64_puts("[VSYNC] frame n="); fb64_num((uint64_t)f);
            fb64_puts(" buf="); fb64_num((uint64_t)g_sw_front);
            fb64_puts(" bufs="); fb64_num((uint64_t)n_sw);
            fb64_puts(" who="); fb64_puts(dev ? "gpu" : "cpu");
            if (to_us) {
                fb64_puts(" draw_us="); fb64_num((t1 - t0) / to_us);
                fb64_puts(" pres_us="); fb64_num((t2 - t1) / to_us);
                fb64_puts(" gap_us="); fb64_num(gap / to_us);
            }
            fb64_puts(" mode="); fb64_puts(g_vs_mode);
            fb64_log_end();
        }
        // 降级事件（GPU -> CPU 的路径切换）：只打一次，随后继续出帧
        const int path_now = dev ? 1 : 0;
        if (path_prev == 1 && path_now == 0) {
            fb64_log_begin();
            fb64_puts("[VSYNC] degrade frame="); fb64_num((uint64_t)f);
            fb64_puts(" reason="); fb64_puts(vgpu64_info64()->last_err);
            fb64_puts(" -> path=soft backend="); fb64_puts(vgpu64_backend_name64());
            fb64_puts(" (继续出帧)");
            fb64_log_end();
        }
        path_prev = path_now;
        t_next += period_cyc ? period_cyc : 1;
        if ((int64_t)(t_next - fb64_rdtsc()) < 0) t_next = fb64_rdtsc();   // 落后了就重锚
    }
    const uint64_t end_cyc = fb64_rdtsc();

    // ---- 统计：帧间隔中位数 / p95 / 抖动（相对中位数的平均绝对偏差）----
    if (g_vs_gap_n > 0) {
        const int n = g_vs_gap_n;
        g_vs_med = fb64_stat_med(g_vs_gap, n);
        int i95 = (int)((int64_t)n * 95 / 100);
        if (i95 >= n) i95 = n - 1;
        if (i95 < 0) i95 = 0;
        g_vs_p95 = g_vs_gap[i95];
        uint64_t sum = 0;
        for (int i = 0; i < n; i++) sum += (g_vs_gap[i] > g_vs_med) ? (g_vs_gap[i] - g_vs_med) : (g_vs_med - g_vs_gap[i]);
        g_vs_jit = sum / (uint64_t)n;
    }
    fb64_log_begin();
    fb64_puts("[VSYNC] stats frames="); fb64_num(g_vs_frames);
    fb64_puts(" bufs="); fb64_num((uint64_t)n_sw);
    fb64_puts(" mode="); fb64_puts(g_vs_mode);
    fb64_puts(" target_us="); fb64_num(target_us);
    fb64_puts(" med_us="); fb64_num(g_vs_med);
    fb64_puts(" p95_us="); fb64_num(g_vs_p95);
    fb64_puts(" jitter_us="); fb64_num(g_vs_jit);
    fb64_puts(" samples="); fb64_num((uint64_t)g_vs_gap_n);
    fb64_puts(" gpu_frames="); fb64_num(g_vs_gpu_frames);
    fb64_puts(" cpu_frames="); fb64_num(g_vs_cpu_frames);
    if (to_us && frames > 0) {
        fb64_puts(" avg_frame_us="); fb64_num((end_cyc - start_cyc) / to_us / (uint64_t)frames);
    }
    fb64_log_end();

    // ---- 收尾：把最后画好的那一帧交回 0 号块（primary resource 绑的就是它）----
    uint32_t* last = g_sw_slot[g_sw_front];
    if (last && last != backbuf_storage) {
        const uint32_t npx = (uint32_t)w * (uint32_t)h;
        for (uint32_t i = 0; i < npx; i++) backbuf_storage[i] = last[i];
    }
    backbuf = backbuf_storage;
    g_sw_n = 1; g_sw_front = 0; g_sw_back = 0;
    if (bench_on) fb64_bench_present(0, w, h, 5);
    int ok0 = 0;
    if (gpu_mode != 0 && vgpu64_ready64()) ok0 = vgpu64_present64(0, w, h);
    if (!ok0) fb64_present_soft(0);
    fb64_log_begin();
    fb64_puts("[VSYNC] demo done frames="); fb64_num(g_vs_frames);
    fb64_puts(" backend="); fb64_puts(vgpu64_backend_name64());
    fb64_log_end();
    return (int)g_vs_frames;
}

const char* fb_vsync_mode64() { return g_vs_mode; }
void fb_vsync_stats64(uint64_t* frames, uint64_t* med_us, uint64_t* p95_us, uint64_t* jit_us) {
    if (frames) *frames = g_vs_frames;
    if (med_us) *med_us = g_vs_med;
    if (p95_us) *p95_us = g_vs_p95;
    if (jit_us) *jit_us = g_vs_jit;
}

// 入口：读配置 -> 设注入 -> 跑帧引擎（没有 fw_cfg 配置时一行不打、直接返回 0）
int fb_vsync_run64() {
    fb64_cfg_load();
    if (g_vs_cfg[0] == 0) return 0;
    const int bufs = fb64_cfg_int("bufs", 2);
    const int gpu_mode = fb64_cfg_str("gpu", "off") ? 0 : (fb64_cfg_str("gpu", "on") ? 1 : 2);
    const int frames = fb64_cfg_int("frames", 240);
    const int fps = fb64_cfg_int("fps", 60);
    const int bench_on = fb64_cfg_str("bench", "0") ? 0 : 1;
    int inj = 0;
    if (fb64_cfg_str("inject", "timeout")) inj = 1;
    else if (fb64_cfg_str("inject", "illegal")) inj = 2;
    else if (fb64_cfg_str("inject", "gone")) inj = 3;
    const int inj_at = fb64_cfg_int("inj_at", 120);
    if (!fb64_cfg_int("demo", 0)) {
        fb64_log_begin();
        fb64_puts("[VSYNC] cfg present but demo=0 -> skip bufs="); fb64_num((uint64_t)bufs);
        fb64_log_end();
        return 0;
    }
    if (!g_vs_cyc_per_ms) g_vs_cyc_per_ms = fb64_calib_cyc_per_ms();
    fb64_log_begin();
    fb64_puts("[VSYNC] cfg bufs="); fb64_num((uint64_t)bufs);
    fb64_puts(" gpu="); fb64_puts(gpu_mode == 1 ? "on" : (gpu_mode == 0 ? "off" : "auto"));
    fb64_puts(" frames="); fb64_num((uint64_t)frames);
    fb64_puts(" fps="); fb64_num((uint64_t)fps);
    fb64_puts(" inject="); fb64_puts(fb64_inj_name(inj));
    fb64_puts(" inj_at="); fb64_num((uint64_t)inj_at);
    fb64_puts(" bench="); fb64_num((uint64_t)bench_on);
    fb64_puts(" cyc_per_ms="); fb64_num(g_vs_cyc_per_ms);
    fb64_log_end();
    if (inj) vgpu64_set_inject64(inj, (uint32_t)inj_at);
    return fb_vsync_engine64(bufs, gpu_mode, frames, fps, bench_on);
}

// ★ v0.5.5：硬件光标平面的开关入口（fw_cfg `cursor=1`）。
//   为什么缺省关：软件光标有 6 种形状（箭头/移动/文本/转圈/缩放），硬件光标只有一张 64x64 位图，
//   打开后形状切换就没有了。它换来的是"移鼠标不再整帧重绘"——所以做成显式开关，不偷偷改变观感。
int fb_cursor_run64() {
    fb64_cfg_load();
    if (g_vs_cfg[0] == 0) return 0;
    if (!fb64_cfg_int("cursor", 0)) return 0;
    const int on = vgpu64_cursor_enable64();
    fb64_log_begin();
    fb64_puts("[VSYNC] cursor cfg=1 -> cursor=");
    fb64_puts(on ? "hw" : "soft");
    fb64_log_end();
    return on;
}
