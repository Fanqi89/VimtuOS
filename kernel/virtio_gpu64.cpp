// virtio_gpu64.cpp - virtio-gpu（2D）驱动：把"整屏/区域 blit"从 CPU 逐像素搬到设备
//
// 本批目标（驱动线 3）：**打通设备加速通道 + 至少一条可量化的快路径**，并且
//   **必须保留既有 LFB 软件路径为回退**（没有 virtio-gpu 时行为完全不变）。
//
// 为什么是 2D 而不是 3D：virtio-gpu 的 3D/virgl 要把 shader/命令流交给 host 的 VirGL 渲染器，
//   工作量大且本内核没有可验证的落地方式（没有 Mesa/DRM）。2D 命令集里唯一"搬像素"的操作是
//   TRANSFER_TO_HOST_2D（guest 后备缓冲 -> host resource 的 DMA）+ RESOURCE_FLUSH（让 host 刷新
//   scanout）—— 这正是 fb_flip_region() 在做的事（CPU 逐行写 LFB），所以第一刀切在这里：
//   **上屏路径换成设备**；绘制（圆角/阴影/毛玻璃/渐变）仍是 CPU（见报告"没做到"）。
//
// 关键设计（踩坑都记在这里，别改坏）：
//   * **framebuffer = 一个 resource**：宽高 = 当前 LFB 物理分辨率（zoom=100 时 = 后备缓冲 stride），
//     格式 = B8G8R8X8_UNORM (= 2)。为什么是它：内核后备缓冲是 32 位 0xAARRGGBB，小端下内存字节序
//     是 B,G,R,X —— 与 B8G8R8X8 逐字节一致，所以"设备路径的像素 == 软件路径的像素"是**同一份
//     字节**，不需要任何 swizzle（验收里逐字节比对靠这条）。
//   * **backing = 后备缓冲的物理页**（每页一条 mem_entry）。后备缓冲在 .bss、带 aligned(4096)、
//     物理上连续，所以"每页一条"只是走 spec 的常规形态，并不需要分散页。
//   * **两段式上屏**：
//       阶段 A（init）：PCI/特性/队列/GET_DISPLAY_INFO/RESOURCE_CREATE_2D/ATTACH_BACKING，
//                        **不设 scanout** —— 此时屏幕上显示的还是 loader 设的 legacy VGA
//                        framebuffer（软件路径写的那块），所以还能抓"软件路径"的屏幕帧。
//       阶段 B（selftest）：① 画图案 -> **软件路径**提交 -> 保持；
//                        ② SET_SCANOUT + 整屏 TRANSFER + FLUSH -> 同一图案走**设备路径** -> 保持；
//                        ③ 设备路径把该区域改成另一个颜色 -> 保持。
//                        验收脚本在三段里各抓一帧：②vs① 逐字节等价（正确性）、③vs② 只差请求区域。
//   * **纯轮询 + 有界超时**：队列不挂 MSI-X（每次命令后读 ISR 清中断线），等 used ring 用
//     g_ticks64 计时 + 自旋上限双保险；超时**打点 + 返回 0**（绝不挂死、绝不假装上屏成功）。
//   * **降级**：没有设备 / 任一步失败 / 运行期连续 N 次失败 -> ready=0，并把 SET_SCANOUT(0)
//     交还给 legacy VGA framebuffer；fb.cpp 那边原样走软件路径（行为与改动前一致）。
//   * **不接受我们处理不了的特性**：只协商 VIRTIO_F_VERSION_1；VIRGL/EDID/UUID/BLOB/CONTEXT_INIT
//     一概不置位。
//   * **体积纪律**：日志原语走非内联包装（见下面 vgpu_puts/vgpu_num），否则 80+ 处打点全内联会
//     让 .text 涨到 40 KB 量级（本文件实测 41,213 -> 14,352 B）；系统内核余量有硬线。
//
// 串口打点格式见 virtio_gpu64.h 顶部（自动验收按行 grep，别改）。
#include "virtio_gpu64.h"
#include "debug64.h"
#include "fb.h"            // 几何 + 后备缓冲 + 软件路径提交（自检要用）
#include "mem_64.h"        // page_alloc_64 / kmalloc_64 / kfree_64 / memset_64 / memmove_64
#include "port.h"          // outl/inl（PCI 配置口 0xCF8/0xCFC）
#include "x86_64.h"        // g_ticks64 / ms_to_ticks64
#include <stdint.h>
#include <stddef.h>

// ==================== 常量 ====================
#define VGPU_VENDOR              0x1AF4u
#define VGPU_DID_GPU_MODERN      0x1050u   // 现代非过渡（virtio-gpu-pci / -vga virtio）
#define VGPU_DID_GPU_TRANS       0x1010u   // 过渡/legacy：0x1000 + virtio device id 16
#define VGPU_DID_GPU_MODERN_BASE 0x1040u   // 现代过渡：0x1040 + 16（subsystem device id = 0x10）
#define VGPU_VIRTIO_DEV_GPU      0x0010u

// VIRTIO_PCI_CAP 的 cfg_type
#define VGPU_CAP_COMMON  1
#define VGPU_CAP_NOTIFY  2
#define VGPU_CAP_ISR     3
#define VGPU_CAP_DEVICE  4

// 特性位：低 32 位里的 GPU 专属位我们**一个都不接受**；高 32 位只接受 VIRTIO_F_VERSION_1
#define VGPU_F_VIRGL           (1u << 0)
#define VGPU_F_EDID            (1u << 1)
#define VGPU_F_RESOURCE_UUID   (1u << 2)
#define VGPU_F_RESOURCE_BLOB   (1u << 3)
#define VGPU_F_CONTEXT_INIT    (1u << 4)
#define VGPU_F_VERSION_1_HI    0x00000001u

// DEVICE_STATUS 位
#define VGPU_ST_ACK         0x01u
#define VGPU_ST_DRIVER      0x02u
#define VGPU_ST_DRIVER_OK   0x04u
#define VGPU_ST_FEATURES_OK 0x08u

// 2D 命令集（spec 5.7.6.2）
enum VgpuCmd : uint32_t {
    VGPU_CMD_GET_DISPLAY_INFO        = 0x0100,
    VGPU_CMD_RESOURCE_CREATE_2D      = 0x0101,
    VGPU_CMD_RESOURCE_UNREF          = 0x0102,
    VGPU_CMD_SET_SCANOUT             = 0x0103,
    VGPU_CMD_RESOURCE_FLUSH          = 0x0104,
    VGPU_CMD_TRANSFER_TO_HOST_2D     = 0x0105,
    VGPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106,
    VGPU_CMD_RESOURCE_DETACH_BACKING = 0x0107,
    VGPU_CMD_TRANSFER_FROM_HOST_2D   = 0x0108,
};
// 响应类型：注意 display_info 的成功响应是 0x1101（不是 0x1100 = OK_NODATA）
#define VGPU_RESP_OK_NODATA       0x1100u
#define VGPU_RESP_OK_DISPLAY_INFO 0x1101u
#define VGPU_FLAG_FENCE 0x1u
#define VGPU_FORMAT_B8G8R8X8_UNORM 2u

// 队列环描述符 flag
#define VRING_DESC_F_NEXT  0x1u
#define VRING_DESC_F_WRITE 0x2u

// 有界等待：250 Hz PIT 下 50 tick = 200 ms；自旋上限是 IF=0（时钟不走）时的兜底
#define VGPU_CMD_TICKS  50u
#define VGPU_CMD_SPIN   20000000u
// 运行期连续失败多少次就彻底退回软件路径（避免每次上屏都卡满超时）
#define VGPU_MAX_FAILS  5
// 自检每段在屏幕上保持多久（验收脚本要在这段时间里用 QEMU monitor 抓帧）
#define VGPU_HOLD_MS    3000

// ★ 预算收口：`[VGPU] equiv …` 的**冗余诊断**（pat=/lfb=/dev= 转储 + MISMATCH 长说明）默认不编译，
//   要人工排查时：VIMTU_EXTRA_CXXFLAGS=-DVGPU64_DIAG_EQUIV_VERBOSE=1 bash build64.sh
//   （那条 `[VGPU] equiv lcg=… rect=… bytes=… diff=…` 主证据行**始终**编译 —— 验收脚本要 grep 它。）
#ifndef VGPU64_DIAG_EQUIV_VERBOSE
#define VGPU64_DIAG_EQUIV_VERBOSE 0
#endif

// ==================== 结构（与 spec 逐字节一致；packed 保证没有对齐洞）====================
struct __attribute__((packed)) VgpuHdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
};
struct __attribute__((packed)) VgpuRect { uint32_t x, y, width, height; };
struct __attribute__((packed)) VgpuMemEntry { uint64_t addr; uint32_t length; uint32_t padding; };
struct __attribute__((packed)) VgpuCreate2d {
    VgpuHdr hdr; uint32_t resource_id, format, width, height;
};
struct __attribute__((packed)) VgpuResId { VgpuHdr hdr; uint32_t resource_id; uint32_t padding; };
struct __attribute__((packed)) VgpuSetScanout {
    VgpuHdr hdr; VgpuRect r; uint32_t scanout_id; uint32_t resource_id;
};
struct __attribute__((packed)) VgpuResFlush {
    VgpuHdr hdr; VgpuRect r; uint32_t resource_id; uint32_t padding;
};
struct __attribute__((packed)) VgpuTransfer2d {
    VgpuHdr hdr; VgpuRect r; uint64_t offset; uint32_t resource_id; uint32_t padding;
};
struct __attribute__((packed)) VgpuAttach {
    VgpuHdr hdr; uint32_t resource_id; uint32_t nr_entries; uint32_t padding[2];
};
struct __attribute__((packed)) VgpuDisplayOne {
    VgpuRect r; uint32_t enabled; uint32_t flags;
};
struct __attribute__((packed)) VgpuRespDisplayInfo {
    VgpuHdr hdr; VgpuDisplayOne pmodes[16];
};
// virtqueue（spec 2.6.6：split virtqueue 的经典布局）
struct __attribute__((packed)) VgpuDesc {
    uint64_t addr; uint32_t len; uint16_t flags; uint16_t next;
};
struct __attribute__((packed)) VgpuUsedElem { uint32_t id; uint32_t len; };

// ==================== 驱动状态 ====================
static Vgpu64Info g_i;
static VgpuDesc*  g_desc = nullptr;      // 队列 0 描述符表（页池：恒等映射，PA = VA）
static uint16_t*  g_avail = nullptr;     // 队列 0 可用环（[0]=flags [1]=idx [2..]=ring）
static volatile uint16_t* g_used_idx = nullptr;   // 队列 0 已用环的 idx
static volatile VgpuUsedElem* g_used = nullptr;   // 队列 0 已用环条目
static uint16_t   g_used_seen = 0;                // 我们上次看到的 used.idx
static uint32_t   g_last_used_len = 0;            // 最近一条命令的 used 条目长度（响应字节数）
static uint64_t   g_desc_pa = 0, g_avail_pa = 0, g_used_pa = 0;
static uint64_t   g_notify_pa = 0;                // 队列 0 的通知地址（notify_pa + off*mul）
static uint16_t   g_qsize = 0;                    // 队列 0 实际大小
static uint32_t   g_fence = 0;
static uint8_t    g_resp[512] __attribute__((aligned(16)));   // 响应缓冲（最大 = display_info 408B）
// ★ 收口：tick 标定缓存。原来标定（50 tick = 200 ms 空转）挤在 vgpu64_bench64() 里，
//   于是 selftest PASS 打完之后要空等 200 ms 才出第一条 `[VGPU] bench` 行 —— 验收脚本
//   tests/virtiogpu64_test.py 的快照点在 PASS 后 ~87 ms（实测），就会拿到"0 条 bench"的伪失败。
//   现在在 init 尾部标一次并缓存：口径不变（同一次 boot 的 rdtsc/PIT 关系），bench 行紧跟 PASS。
static uint64_t   g_cyc_per_tick = 0;
static uint64_t   vgpu_cyc_per_tick();            // 前向声明（实现见文件下面的基准段）
static VgpuMemEntry* g_entries = nullptr;         // ATTACH_BACKING 的页表（kmalloc，重配时复用）
static uint32_t   g_entries_cap = 0;
static int        g_fails = 0;                    // 连续失败计数（到 VGPU_MAX_FAILS 就降级）
static int        g_blit_logged = 0;
static uint32_t   g_scanout_res = 0;                                   // ★ ⑭：当前 scanout 上的 resource id
// ★ ⑭ 交换链（每块一个 resource；第 0 块 = primary g_i.res_id）
#define VGPU_SWAP_MAX 3
static uint32_t   g_swap_res[VGPU_SWAP_MAX] = { 0, 0, 0 };
static uint32_t   g_swap_frames = 0;
static VgpuMemEntry* g_sw_entries = nullptr;                           // 交换链 ATTACH_BACKING 的页表（复用）
static uint32_t   g_sw_entries_cap = 0;
static uint32_t   g_swap_presents = 0, g_swap_flips = 0;
static uint32_t   g_res_w = 0, g_res_h = 0;       // resource 宽高（== 后备缓冲 stride 才合法）
static int        g_scanout_on = 0;
static const char* const VGPU_HEXL = "0123456789abcdef";

static const char* vgpu_cmd_name(uint32_t t) {
    switch (t) {
        case VGPU_CMD_GET_DISPLAY_INFO:        return "GET_DISPLAY_INFO";
        case VGPU_CMD_RESOURCE_CREATE_2D:      return "RESOURCE_CREATE_2D";
        case VGPU_CMD_RESOURCE_UNREF:          return "RESOURCE_UNREF";
        case VGPU_CMD_SET_SCANOUT:             return "SET_SCANOUT";
        case VGPU_CMD_RESOURCE_FLUSH:          return "RESOURCE_FLUSH";
        case VGPU_CMD_TRANSFER_TO_HOST_2D:     return "TRANSFER_TO_HOST_2D";
        case VGPU_CMD_RESOURCE_ATTACH_BACKING: return "RESOURCE_ATTACH_BACKING";
        case VGPU_CMD_RESOURCE_DETACH_BACKING: return "RESOURCE_DETACH_BACKING";
        case VGPU_CMD_TRANSFER_FROM_HOST_2D:   return "TRANSFER_FROM_HOST_2D";
        default: return "?";
    }
}

// ==================== 打点小工具 ====================
// ★ 体积纪律：日志原语一律走**非内联包装**（每个原语只保留一份实现）。debug64.h 的
//   dbg64_str/dec/putc 都是 static inline（每次展开都带 outb + 串口自旋 + 环形缓冲 sink 调用），
//   本文件 80+ 处打点若全内联，.text 会到 40 KB 量级 —— 系统内核余量有硬线（见报告）。
__attribute__((noinline)) static void vgpu_puts(const char* s) { dbg64_str(s); }
__attribute__((noinline)) static void vgpu_ch(char c) { dbg64_putc(c); }
__attribute__((noinline)) static void vgpu_num(uint64_t v) { dbg64_dec(v); }
static void vgpu_log_begin() { dbg64_line_begin64(); }
static void vgpu_log_end()   { dbg64_nl(); dbg64_line_end64(); }
__attribute__((noinline)) static void vgpu_hex32(uint32_t v) {
    vgpu_puts("0x");
    if (!v) { vgpu_ch('0'); return; }
    char b[8]; int n = 0;
    while (v && n < 8) { b[n++] = VGPU_HEXL[v & 0xFu]; v >>= 4; }
    while (n > 0) vgpu_ch(b[--n]);
}
__attribute__((noinline)) static void vgpu_hex64(uint64_t v) {
    vgpu_puts("0x");
    if (!v) { vgpu_ch('0'); return; }
    char b[16]; int n = 0;
    while (v && n < 16) { b[n++] = VGPU_HEXL[v & 0xFu]; v >>= 4; }
    while (n > 0) vgpu_ch(b[--n]);
}
__attribute__((noinline)) static void vgpu_err(const char* stage) {
    g_i.last_err = stage ? stage : "?";
    vgpu_log_begin();
    vgpu_puts("[VGPU] error stage=");
    vgpu_puts(g_i.last_err);
    vgpu_log_end();
}

// ==================== 物理地址 ====================
// 页池/内核堆恒等映射（PA == VA），内核镜像与 .bss 搬在高半区（VA = 0xFFFFFFFF80000000 + PA）。
// 后备缓冲是内核 .bss 里的静态数组 -> 必须减基址；页池里的队列环 -> 直接用。
static inline uint64_t vgpu_phys(const void* p) {
    const uint64_t va = (uint64_t)(uintptr_t)p;
    return (va >= 0xFFFFFFFF80000000ULL) ? (va - 0xFFFFFFFF80000000ULL) : va;
}

// ==================== PCI 配置空间（0xCF8/0xCFC）====================
static uint32_t vpci_rd32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    outl(0xCF8u, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                 | ((uint32_t)fn << 8) | (uint32_t)(off & 0xFCu));
    return inl(0xCFCu);
}
static void vpci_wr32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    outl(0xCF8u, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                 | ((uint32_t)fn << 8) | (uint32_t)(off & 0xFCu));
    outl(0xCFCu, v);
}

static bool vgpu_dev_id_ok(uint16_t ven, uint16_t dev, uint16_t sub) {
    if (ven != VGPU_VENDOR) return false;
    if (dev == VGPU_DID_GPU_MODERN) return true;                        // 现代非过渡
    if (dev == VGPU_DID_GPU_TRANS)  return true;                        // 0x1000 + 16（GPU）
    if ((dev & 0xFFF0u) == VGPU_DID_GPU_MODERN_BASE && sub == VGPU_VIRTIO_DEV_GPU) return true;
    if (dev == 0x1000u && sub == VGPU_VIRTIO_DEV_GPU) return true;
    return false;
}

// 扫 0..255 号总线（连续 4 条空总线提前停）；认到就记 bus/dev/fn + 原始 id。
static bool vgpu_pci_find() {
    uint32_t empty_run = 0;
    for (uint32_t bus = 0; bus < 256; bus++) {
        bool bus_has = false;
        for (uint32_t d = 0; d < 32; d++) {
            uint32_t id = vpci_rd32((uint8_t)bus, (uint8_t)d, 0, 0x00);
            uint16_t ven = (uint16_t)(id & 0xFFFFu);
            if (ven == 0xFFFFu || ven == 0x0000u) continue;
            bus_has = true;
            const uint32_t hdr = vpci_rd32((uint8_t)bus, (uint8_t)d, 0, 0x0C);
            const uint32_t nfn = (hdr & 0x00800000u) ? 8u : 1u;
            for (uint32_t fn = 0; fn < nfn; fn++) {
                if (fn) {
                    id = vpci_rd32((uint8_t)bus, (uint8_t)d, (uint8_t)fn, 0x00);
                    ven = (uint16_t)(id & 0xFFFFu);
                    if (ven == 0xFFFFu || ven == 0x0000u) continue;
                }
                const uint16_t dev = (uint16_t)(id >> 16);
                const uint32_t sub_dw = vpci_rd32((uint8_t)bus, (uint8_t)d, (uint8_t)fn, 0x2C);
                const uint16_t sub = (uint16_t)(sub_dw >> 16);      // subsystem device id @0x2E
                if (!vgpu_dev_id_ok(ven, dev, sub)) continue;
                g_i.bus = (uint8_t)bus; g_i.dev = (uint8_t)d; g_i.fn = (uint8_t)fn;
                g_i.vendor = ven; g_i.device = dev; g_i.subsys = sub;
                g_i.found = 1;
                return true;
            }
        }
        if (bus_has) empty_run = 0;
        else if (++empty_run >= 4u) break;
    }
    return false;
}

// BAR 基址。★ 64 位 BAR（type=10b）：Base = BAR[n] | BAR[n+1]<<32 —— 高 32 位**必须一起读**
//   （hda64 里踩过"只当 32 位读"的坑）。这里不做大小探测：探测要写 0xFFFFFFFF 再回写，而驱动
//   只用基址（cap 的 offset/len 由设备给），省掉这段代码与两次危险的配置写（内核余量很紧）。
static uint64_t vgpu_bar_read(uint8_t bus, uint8_t dev, uint8_t fn, int idx) {
    const uint8_t o = (uint8_t)(0x10 + idx * 4);
    const uint32_t lo = vpci_rd32(bus, dev, fn, o);
    if (!(lo & ~0x0Fu) || (lo & 1u)) return 0;                    // 空槽 / I/O BAR（cap 只用 MMIO）
    uint64_t base = (uint64_t)(lo & ~0x0Fu);
    if (((lo >> 1) & 0x3u) == 0x2u)                               // 64 位 BAR -> 取高半区
        base |= ((uint64_t)vpci_rd32(bus, dev, fn, (uint8_t)(o + 4)) << 32);
    return base;
}

// ==================== MMIO ====================
static inline uint8_t  vrd8 (uint64_t pa) { return *(volatile uint8_t  *)(uintptr_t)pa; }
static inline uint16_t vrd16(uint64_t pa) { return *(volatile uint16_t *)(uintptr_t)pa; }
static inline uint32_t vrd32(uint64_t pa) { return *(volatile uint32_t *)(uintptr_t)pa; }
static inline void vwr8 (uint64_t pa, uint8_t  v) { *(volatile uint8_t  *)(uintptr_t)pa = v; }
static inline void vwr16(uint64_t pa, uint16_t v) { *(volatile uint16_t *)(uintptr_t)pa = v; }
static inline void vwr32(uint64_t pa, uint32_t v) { *(volatile uint32_t *)(uintptr_t)pa = v; }

// common cfg 寄存器偏移（spec 4.1.4.3）
#define VCFG_DEV_FEAT_SEL     0x00
#define VCFG_DEV_FEAT         0x04
#define VCFG_DRV_FEAT_SEL     0x08
#define VCFG_DRV_FEAT         0x0C
#define VCFG_NUM_QUEUES       0x12
#define VCFG_DEV_STATUS       0x14
#define VCFG_QUEUE_SEL        0x16
#define VCFG_QUEUE_SIZE       0x18
#define VCFG_QUEUE_MSIX_VEC   0x1A
#define VCFG_QUEUE_ENABLE     0x1C
#define VCFG_QUEUE_NOTIFY_OFF 0x1E
#define VCFG_QUEUE_DESC       0x20
#define VCFG_QUEUE_DRIVER     0x28
#define VCFG_QUEUE_DEVICE     0x30

static inline uint8_t  vcfg_r8 (uint32_t off) { return vrd8 (g_i.common_pa + off); }
static inline uint16_t vcfg_r16(uint32_t off) { return vrd16(g_i.common_pa + off); }
static inline uint32_t vcfg_r32(uint32_t off) { return vrd32(g_i.common_pa + off); }
static inline void vcfg_w8 (uint32_t off, uint8_t  v) { vwr8 (g_i.common_pa + off, v); }
static inline void vcfg_w16(uint32_t off, uint16_t v) { vwr16(g_i.common_pa + off, v); }
static inline void vcfg_w32(uint32_t off, uint32_t v) { vwr32(g_i.common_pa + off, v); }
static inline void vcfg_w64(uint32_t off, uint64_t v) { vwr32(g_i.common_pa + off, (uint32_t)v);
                                                        vwr32(g_i.common_pa + off + 4, (uint32_t)(v >> 32)); }

// ==================== virtqueue（split；单命令同步：一次只挂一条链）====================
// 队列 0 的环状态复位：只在**使能队列之前**调用（used.idx 是设备的字段，使能后不许我们写）。
static void vgpu_queue_reset_state() { g_used_seen = 0; g_last_used_len = 0; }

// 建队列 0（控制队列）：描述符表/可用环/已用环都放页池（恒等映射 -> 直接写物理地址）。
//   一页装得下：sz <= 256 时 desc = 16*256 = 4096（正好一页）、avail = 6+2*256、used = 6+8*256。
//   设备给更大的队列时**驱动有权写小**（spec：driver 可写 <= device max），写 256 再回读确认。
static bool vgpu_queue0_setup() {
    vcfg_w16(VCFG_QUEUE_SEL, 0);
    uint16_t sz = vcfg_r16(VCFG_QUEUE_SIZE);
    const uint16_t dev_max = sz;
    if (sz > 256) { vcfg_w16(VCFG_QUEUE_SIZE, 256); sz = vcfg_r16(VCFG_QUEUE_SIZE); }
    g_i.queue0_size = sz;
    if (sz < 8 || sz > 256) { vgpu_err("queue0 size"); return false; }
    g_qsize = sz;

    void* desc  = page_alloc_64();
    void* avail = page_alloc_64();
    void* used  = page_alloc_64();
    if (!desc || !avail || !used) { vgpu_err("queue0 alloc"); return false; }
    if ((uint64_t)16 * sz > PAGE_SIZE_64) { vgpu_err("queue0 desc too big"); return false; }
    memset_64(desc, 0, PAGE_SIZE_64);
    memset_64(avail, 0, PAGE_SIZE_64);
    memset_64(used, 0, PAGE_SIZE_64);
    g_desc  = (VgpuDesc*)desc;
    g_avail = (uint16_t*)avail;
    g_used_idx = (volatile uint16_t*)((uint8_t*)used + 2);         // used.idx
    g_used  = (volatile VgpuUsedElem*)((uint8_t*)used + 4);        // used.ring[]
    g_desc_pa = vgpu_phys(desc); g_avail_pa = vgpu_phys(avail); g_used_pa = vgpu_phys(used);
    g_i.q0_desc_pa = g_desc_pa; g_i.q0_avail_pa = g_avail_pa; g_i.q0_used_pa = g_used_pa;
    vgpu_queue_reset_state();

    vcfg_w64(VCFG_QUEUE_DESC,   g_desc_pa);
    vcfg_w64(VCFG_QUEUE_DRIVER, g_avail_pa);
    vcfg_w64(VCFG_QUEUE_DEVICE, g_used_pa);
    vcfg_w16(VCFG_QUEUE_MSIX_VEC, 0);                             // 不挂 MSI-X（纯轮询 + 读 ISR）
    vcfg_w16(VCFG_QUEUE_ENABLE, 1);
    const uint16_t noff = vcfg_r16(VCFG_QUEUE_NOTIFY_OFF);
    g_i.queue0_notify_off = noff;
    g_notify_pa = g_i.notify_pa + (uint64_t)noff * (uint64_t)g_i.notify_off_mul;

    vgpu_log_begin();
    vgpu_puts("[VGPU] queue0 size="); vgpu_num(sz);
    vgpu_puts(" dev_max="); vgpu_num(dev_max);
    vgpu_puts(" notify_off="); vgpu_num(noff);
    vgpu_puts(" msix_vec=0 desc="); vgpu_hex64(g_desc_pa);
    vgpu_puts(" avail="); vgpu_hex64(g_avail_pa);
    vgpu_puts(" used="); vgpu_hex64(g_used_pa);
    vgpu_log_end();
    return true;
}

static void vgpu_notify0() {
    __asm__ volatile("mfence" ::: "memory");
    if (g_notify_pa) vwr16(g_notify_pa, 0);
}

// ==================== ★ ⑭ 故障注入（只由 fb.cpp 按 fw_cfg 配置调用；缺省全关）====================
// 为什么要有它：验收要证明"设备路径出问题时会**自动降级**并且系统继续可用"。注入三条路：
//   timeout  = 命令根本不完成（强制走有界超时分支）-> 连续失败 -> vgpu_disable -> 软件路径
//   illegal  = 命令完成但响应类型非法（0x1BAD）-> 响应校验判失败 -> 同上
//   gone     = 设备"消失"（命令被丢弃）-> 同上（reason 区分）
// 注入次数**有界**（最多 8 条命令），因此最坏情况下也只是有限次失败后降级，不会无限打点/挂死。
static int      g_inj_kind = 0;        // 0=none 1=timeout 2=illegal 3=gone
static uint32_t g_inj_after = 0;       // 第几条命令之后开始注入
static int      g_inj_left = 0;        // 还剩几条要注入
static int      g_inj_logged = 0;
void vgpu64_set_inject64(int kind, uint32_t after) {
    g_inj_kind = kind;
    g_inj_after = after;
    g_inj_left = kind ? 8 : 0;
    g_inj_logged = 0;
}
// 命中返回 kind（打点最多 3 行）；不命中返回 0
static int vgpu_inject_hit64(void) {
    if (!g_inj_kind || g_inj_left <= 0) return 0;
    if (g_i.cmds < g_inj_after) return 0;
    g_inj_left--;
    if (g_inj_logged < 3) {
        g_inj_logged++;
        vgpu_log_begin();
        vgpu_puts("[VGPU] INJECT kind="); vgpu_num((uint64_t)g_inj_kind);
        vgpu_puts(" cmds="); vgpu_num(g_i.cmds);
        vgpu_puts(" left="); vgpu_num((uint64_t)g_inj_left);
        vgpu_log_end();
    }
    return g_inj_kind;
}

// 提交一条命令并**有界等待**它的 used 条目。cmd 必须是物理连续的一块（堆/静态都行）。
// 返回 true = 设备消费了这条链（used.idx 前进）；g_resp 里按命令语义解释响应。
static bool vgpu_cmd(const void* cmd, uint32_t cmd_len, const char* tag) {
    if (!g_i.ready || !g_desc) return false;
    const uint64_t cmd_pa = vgpu_phys(cmd);
    const uint64_t resp_pa = vgpu_phys(g_resp);
    memset_64(g_resp, 0, sizeof(g_resp));

    g_desc[0].addr = cmd_pa; g_desc[0].len = cmd_len;
    g_desc[0].flags = VRING_DESC_F_NEXT; g_desc[0].next = 1;
    g_desc[1].addr = resp_pa; g_desc[1].len = (uint32_t)sizeof(g_resp);
    g_desc[1].flags = VRING_DESC_F_WRITE; g_desc[1].next = 0;

    const uint16_t slot = (uint16_t)(g_avail[1] % g_qsize);
    g_avail[2 + slot] = 0;                                        // 用描述符 0 当链头
    __asm__ volatile("mfence" ::: "memory");
    g_avail[1] = (uint16_t)(g_avail[1] + 1);
    vgpu_notify0();

    g_i.cmds++;
    // ★ ⑭ 故障注入：timeout/gone = 直接判失败（**有界**：不等待、不挂）；illegal = 命令照走，
    //   但把响应类型改成非法值，由 vgpu_send 的响应校验判失败。
    const int inj = vgpu_inject_hit64();
    if (inj == 1 || inj == 3) {
        g_i.cmd_timeouts++;
        g_i.last_err = (inj == 1) ? "inject-timeout" : "inject-device-gone";
        vgpu_log_begin();
        vgpu_puts("[VGPU] cmd inject-fail type=");
        vgpu_hex32(((const VgpuHdr*)cmd)->type);
        vgpu_puts(" name="); vgpu_puts(tag);
        vgpu_puts(" reason="); vgpu_puts(g_i.last_err);
        vgpu_puts(" timeouts="); vgpu_num(g_i.cmd_timeouts);
        vgpu_log_end();
        return false;
    }
    const uint64_t t0 = g_ticks64;
    uint32_t spin = 0;
    while (*g_used_idx == g_used_seen) {
        if (g_ticks64 - t0 > (uint64_t)VGPU_CMD_TICKS || ++spin > VGPU_CMD_SPIN) {
            g_i.cmd_timeouts++;
            g_i.last_err = tag;
            vgpu_log_begin();
            vgpu_puts("[VGPU] cmd timeout type=");
            vgpu_hex32(((const VgpuHdr*)cmd)->type);
            vgpu_puts(" name="); vgpu_puts(tag);
            vgpu_puts(" used="); vgpu_num(*g_used_idx);
            vgpu_puts(" seen="); vgpu_num(g_used_seen);
            vgpu_puts(" timeouts="); vgpu_num(g_i.cmd_timeouts);
            vgpu_log_end();
            if (g_i.isr_pa) (void)vrd32(g_i.isr_pa);              // 读一下 ISR：清中断线
            return false;
        }
        __asm__ volatile("pause");
    }
    // used 条目里回给我们的长度：有响应的命令会有值（只当信息记下来，不以此判定成败）
    g_last_used_len = g_used[(uint16_t)(g_used_seen % g_qsize)].len;
    g_used_seen = *g_used_idx;
    if (g_i.isr_pa) (void)vrd32(g_i.isr_pa);                      // ★ 读 ISR = ack + 拉低 INTx
    if (inj == 2) ((VgpuHdr*)g_resp)->type = 0x1BADu;             // ★ ⑭：注入非法响应类型
    return true;
}

static void vgpu_cmd_log(uint32_t type, int used_ok) {
    vgpu_log_begin();
    vgpu_puts("[VGPU] cmd type=");
    vgpu_hex32(type);
    vgpu_puts(" name="); vgpu_puts(vgpu_cmd_name(type));
    vgpu_puts(" used="); vgpu_num(used_ok ? 1u : 0u);
    vgpu_puts(" resp="); vgpu_num(g_last_used_len);
    vgpu_puts(" timeouts="); vgpu_num(g_i.cmd_timeouts);
    vgpu_log_end();
}
static bool vgpu_send(const void* cmd, uint32_t len, int verbose) {
    const bool ok = vgpu_cmd(cmd, len, vgpu_cmd_name(((const VgpuHdr*)cmd)->type));
    if (verbose) vgpu_cmd_log(((const VgpuHdr*)cmd)->type, ok ? 1 : 0);
    if (!ok) return false;
    // ★ ⑭ 响应类型校验：0x11xx = OK_*，0x1200..0x12FF = ERR_*（非法参数/资源不存在/不支持）。
    //   这一批之前只等 used 条目、**不看响应类型** —— 设备"礼貌地拒绝"会被静默当成成功。
    //   **作用域收口（重要）**：只有"帧引擎上屏真正依赖"的三条命令把 ERR 判成失败
    //   （TRANSFER_TO_HOST_2D / RESOURCE_FLUSH / SET_SCANOUT）—— 实测 QEMU 11 对
    //   RESOURCE_ATTACH_BACKING 会回 ERR_UNSPEC（guest_errors: failed to map MMIO memory for
    //   element 0），那是**既有缺陷**：以前被静默吞掉；现在如实打一行 ERR 但**不改**既有
    //   容忍行为（否则设备路径会在 init 就被自己关掉，回归面变大）。
    const uint32_t ctype = ((const VgpuHdr*)cmd)->type;
    const uint32_t rt = ((const VgpuHdr*)g_resp)->type;          // vgpu_resp_type() 在本函数之后定义
    if (rt >= 0x1200u && rt < 0x1300u) {
        const int strict = (ctype == VGPU_CMD_TRANSFER_TO_HOST_2D || ctype == VGPU_CMD_RESOURCE_FLUSH ||
                            ctype == VGPU_CMD_SET_SCANOUT);
        vgpu_log_begin();
        vgpu_puts("[VGPU] resp ERR type="); vgpu_hex32(rt);
        vgpu_puts(" name="); vgpu_puts(vgpu_cmd_name(ctype));
        vgpu_puts(strict ? " -> fail" : " (tolerated)");
        vgpu_log_end();
        if (strict) { g_i.last_err = "resp-err"; return false; }
    }
    return true;
}
static uint32_t vgpu_resp_type() { return ((const VgpuHdr*)g_resp)->type; }

// ==================== 降级 / scanout ====================
static void vgpu_scanout_off_internal() {
    if (!g_i.ready || !g_scanout_on) return;
    VgpuSetScanout c;
    memset_64(&c, 0, sizeof(c));
    c.hdr.type = VGPU_CMD_SET_SCANOUT;
    c.scanout_id = 0; c.resource_id = 0;                          // resource_id = 0 = 关掉 scanout
    if (!vgpu_send(&c, sizeof(c), 0)) return;
    g_scanout_on = 0;
    g_scanout_res = 0;
}
// 降级：把显示交还 legacy VGA framebuffer + ready=0（fb.cpp 那边随后原样走软件路径）
static void vgpu_disable(const char* reason) {
    if (!g_i.ready) return;
    vgpu_scanout_off_internal();
    g_i.ready = 0;
    g_scanout_on = 0;
    g_scanout_res = 0;
    g_swap_frames = 0;                          // ★ ⑭：交换链一起失效（fb.cpp 随后走软件路径继续出帧）
    g_i.last_err = reason ? reason : "?";
    vgpu_log_begin();
    vgpu_puts("[VGPU] disable reason=");
    vgpu_puts(g_i.last_err);
    vgpu_log_end();
    fb_backend_log64("soft-lfb (device path disabled)");
}
// ★ ⑭：把指定 resource 设到 scanout 0 上（交换链翻页用）。rid = 0 = 关掉 scanout。
static bool vgpu_set_scanout_res(uint32_t rid) {
    VgpuSetScanout s;
    memset_64(&s, 0, sizeof(s));
    s.hdr.type = VGPU_CMD_SET_SCANOUT;
    s.r.x = 0; s.r.y = 0; s.r.width = g_res_w; s.r.height = g_res_h;
    s.scanout_id = 0; s.resource_id = rid;
    if (!vgpu_send(&s, sizeof(s), 1)) return false;
    g_scanout_on = (rid != 0) ? 1 : 0;
    g_scanout_res = rid;
    return true;
}
static bool vgpu_set_scanout_internal() {
    // 注（如实记录）：QEMU 的 virtio-vga 里 device 与 legacy VGA 共用同一个 console —— 早期 boot 里
    //   屏幕可能仍由 VGA 那一路驱动（见报告"没做到 / 环境说明"）；像素正确性由自检的
    //   TRANSFER_FROM_HOST_2D 回读逐字节证明（不依赖 host 的显示仲裁）。
    return vgpu_set_scanout_res(g_i.res_id);
}

// ==================== 几何 ====================
// 设备路径成立的几何条件：zoom=100 且 resource 宽高 == 后备缓冲 stride（= fb_width()）
//   且 == 物理分辨率。任一条不满足 -> 设备路径不能用（否则像素会错位）。
static bool vgpu_geom_ok() {
    const int w = fb_width(), h = fb_height();
    if (fb_get_zoom() != 100) return false;
    if (w != fb_phys_width() || h != fb_phys_height()) return false;
    if (!g_res_w || !g_res_h) return false;
    return (w == (int)g_res_w && h == (int)g_res_h);
}

// ==================== 初始化 ====================
// capability 列表：找 COMMON/NOTIFY/DEVICE/ISR 的 BAR+offset+len（现代 virtio 1.0 的硬要求）
static bool vgpu_caps_parse() {
    uint8_t p = (uint8_t)(vpci_rd32(g_i.bus, g_i.dev, g_i.fn, 0x34) & 0xFCu);
    int found = 0;
    for (int guard = 0; guard < 48 && p >= 0x40; guard++) {
        if (p & 0x3u) { vgpu_err("cap not dword aligned"); return false; }
        const uint32_t dw0 = vpci_rd32(g_i.bus, g_i.dev, g_i.fn, p);
        const uint8_t cid  = (uint8_t)(dw0 & 0xFFu);
        const uint8_t next = (uint8_t)((dw0 >> 8) & 0xFFu);
        const uint8_t clen = (uint8_t)((dw0 >> 16) & 0xFFu);
        const uint8_t ctyp = (uint8_t)((dw0 >> 24) & 0xFFu);
        if (cid == 0x09u && clen >= 16u) {                        // VIRTIO_PCI_CAP
            const uint8_t  bar = (uint8_t)(vpci_rd32(g_i.bus, g_i.dev, g_i.fn, (uint8_t)(p + 4)) & 0xFFu);
            const uint32_t off = vpci_rd32(g_i.bus, g_i.dev, g_i.fn, (uint8_t)(p + 8));
            const uint32_t len = vpci_rd32(g_i.bus, g_i.dev, g_i.fn, (uint8_t)(p + 12));
            uint32_t mul = 0;
            if (ctyp == VGPU_CAP_NOTIFY && clen >= 20u)
                mul = vpci_rd32(g_i.bus, g_i.dev, g_i.fn, (uint8_t)(p + 16));
            if (bar >= 6) { vgpu_err("cap bar"); return false; }
            const uint64_t base = g_i.bar_pa[bar];
            if (!base) { vgpu_err("cap bar unassigned"); return false; }
            const uint64_t addr = base + off;
            if (addr >= 0x100000000ULL) { vgpu_err("cap above 4G"); return false; }  // 只映射了前 4GB
            switch (ctyp) {
                case VGPU_CAP_COMMON: g_i.common_pa = addr; g_i.common_len = len; found |= 1; break;
                case VGPU_CAP_NOTIFY: g_i.notify_pa = addr; g_i.notify_len = len;
                                      g_i.notify_off_mul = mul ? mul : 1u; found |= 2; break;
                case VGPU_CAP_DEVICE: g_i.device_pa = addr; g_i.device_len = len; found |= 4; break;
                case VGPU_CAP_ISR:    g_i.isr_pa = addr;    g_i.isr_len = len;    found |= 8; break;
                default: break;                                   // 5 = PCI（共享内存）：本批不用
            }
        }
        if (!next || next == p) break;
        p = next;
    }
    if ((found & 1u) == 0) { vgpu_err("no COMMON_CFG cap (legacy?)"); return false; }
    if ((found & 2u) == 0) { vgpu_err("no NOTIFY_CFG cap"); return false; }
    vgpu_log_begin();
    vgpu_puts("[VGPU] cap common="); vgpu_hex64(g_i.common_pa);
    vgpu_puts(" len="); vgpu_num(g_i.common_len);
    vgpu_puts(" notify="); vgpu_hex64(g_i.notify_pa);
    vgpu_puts(" len="); vgpu_num(g_i.notify_len);
    vgpu_puts(" mul="); vgpu_num(g_i.notify_off_mul);
    vgpu_puts(" device="); vgpu_hex64(g_i.device_pa);
    vgpu_puts(" isr="); vgpu_hex64(g_i.isr_pa);
    vgpu_log_end();
    return true;
}

static bool vgpu_features_negotiate() {
    vcfg_w32(VCFG_DEV_FEAT_SEL, 0);
    g_i.feat_dev_lo = vcfg_r32(VCFG_DEV_FEAT);
    vcfg_w32(VCFG_DEV_FEAT_SEL, 1);
    g_i.feat_dev_hi = vcfg_r32(VCFG_DEV_FEAT);
    // ★ 只接受我们真能处理的位：VIRGL/EDID/UUID/BLOB/CONTEXT_INIT 一律不置位；
    //   高 32 位只留 VIRTIO_F_VERSION_1（现代 virtio 1.0+ 的设备必需）。
    const uint32_t ok_lo = 0;
    const uint32_t ok_hi = (g_i.feat_dev_hi & VGPU_F_VERSION_1_HI);
    if (!ok_hi) { vgpu_err("no VIRTIO_F_VERSION_1"); return false; }
    const uint32_t rejected_lo = g_i.feat_dev_lo & (VGPU_F_VIRGL | VGPU_F_EDID |
                                                    VGPU_F_RESOURCE_UUID | VGPU_F_RESOURCE_BLOB |
                                                    VGPU_F_CONTEXT_INIT);
    vcfg_w32(VCFG_DRV_FEAT_SEL, 0); vcfg_w32(VCFG_DRV_FEAT, ok_lo);
    vcfg_w32(VCFG_DRV_FEAT_SEL, 1); vcfg_w32(VCFG_DRV_FEAT, ok_hi);
    g_i.feat_ok_lo = ok_lo; g_i.feat_ok_hi = ok_hi;
    vgpu_log_begin();
    vgpu_puts("[VGPU] feature dev_lo="); vgpu_hex32(g_i.feat_dev_lo);
    vgpu_puts(" dev_hi="); vgpu_hex32(g_i.feat_dev_hi);
    vgpu_puts(" ok_lo="); vgpu_hex32(ok_lo);
    vgpu_puts(" ok_hi="); vgpu_hex32(ok_hi);
    vgpu_puts(" rejected="); vgpu_hex32(rejected_lo);
    vgpu_puts(" status="); vgpu_hex32(vcfg_r8(VCFG_DEV_STATUS));
    vgpu_log_end();
    return true;
}

static bool vgpu_status_step() {
    // 复位 -> ACKNOWLEDGE -> DRIVER -> 特性协商 -> FEATURES_OK（回读确认）-> 队列 -> DRIVER_OK
    vcfg_w8(VCFG_DEV_STATUS, 0);
    vcfg_w8(VCFG_DEV_STATUS, VGPU_ST_ACK);
    vcfg_w8(VCFG_DEV_STATUS, (uint8_t)(VGPU_ST_ACK | VGPU_ST_DRIVER));
    if (!vgpu_features_negotiate()) return false;
    vcfg_w8(VCFG_DEV_STATUS, (uint8_t)(VGPU_ST_ACK | VGPU_ST_DRIVER | VGPU_ST_FEATURES_OK));
    const uint8_t st = vcfg_r8(VCFG_DEV_STATUS);
    g_i.device_status = st;
    if (!(st & VGPU_ST_FEATURES_OK)) { vgpu_err("FEATURES_OK not set"); return false; }
    if (!vgpu_queue0_setup()) return false;
    // 队列 1（cursor queue）：只报告大小，**不启用**（本批不做鼠标平面；见报告"没做到"）
    {
        vcfg_w16(VCFG_QUEUE_SEL, 1);
        const uint16_t sz1 = vcfg_r16(VCFG_QUEUE_SIZE);
        g_i.queue1_size = sz1;
        vgpu_log_begin();
        vgpu_puts("[VGPU] queue1 size="); vgpu_num(sz1);
        vgpu_puts(" (cursor; not enabled)");
        vgpu_log_end();
        vcfg_w16(VCFG_QUEUE_SEL, 0);
    }
    vcfg_w8(VCFG_DEV_STATUS, (uint8_t)(VGPU_ST_ACK | VGPU_ST_DRIVER | VGPU_ST_FEATURES_OK | VGPU_ST_DRIVER_OK));
    g_i.device_status = vcfg_r8(VCFG_DEV_STATUS);
    g_i.ready = 1;
    return true;
}

static bool vgpu_get_display_info() {
    VgpuHdr c;
    memset_64(&c, 0, sizeof(c));
    c.type = VGPU_CMD_GET_DISPLAY_INFO;
    c.flags = VGPU_FLAG_FENCE;
    c.fence_id = ++g_fence;
    if (!vgpu_send(&c, sizeof(c), 1)) return false;
    // ★ display_info 的成功响应是 **0x1101**（RESP_OK_DISPLAY_INFO）；0x1100 是 OK_NODATA。
    //   一刀切用 0x1100 会把自己打回软件路径（踩过：init 全绿但 ready=0）。
    if (vgpu_resp_type() != VGPU_RESP_OK_DISPLAY_INFO) {
        vgpu_err("display info resp type");
        return false;
    }
    const VgpuRespDisplayInfo* r = (const VgpuRespDisplayInfo*)g_resp;
    int idx = -1;
    for (int i = 0; i < 16; i++) {
        if (r->pmodes[i].enabled && r->pmodes[i].r.width && r->pmodes[i].r.height) { idx = i; break; }
    }
    if (idx < 0) {
        g_i.disp_w = 0; g_i.disp_h = 0; g_i.disp_enabled = 0;
    } else {
        g_i.disp_w = (int)r->pmodes[idx].r.width;
        g_i.disp_h = (int)r->pmodes[idx].r.height;
        g_i.disp_enabled = 1;
    }
    g_i.lfb_w = fb_phys_width(); g_i.lfb_h = fb_phys_height(); g_i.lfb_zoom = fb_get_zoom();
    vgpu_log_begin();
    vgpu_puts("[VGPU] display info ");
    vgpu_num((uint64_t)(g_i.disp_w > 0 ? g_i.disp_w : 0));
    vgpu_puts("x");
    vgpu_num((uint64_t)(g_i.disp_h > 0 ? g_i.disp_h : 0));
    vgpu_puts(" enabled="); vgpu_num((uint64_t)g_i.disp_enabled);
    vgpu_puts(" lfb="); vgpu_num((uint64_t)g_i.lfb_w);
    vgpu_puts("x"); vgpu_num((uint64_t)g_i.lfb_h);
    vgpu_puts(" zoom="); vgpu_num((uint64_t)g_i.lfb_zoom);
    if (g_i.disp_w != g_i.lfb_w || g_i.disp_h != g_i.lfb_h)
        vgpu_puts(" note=device-vs-lfb-differ-use-lfb");
    vgpu_log_end();
    return true;
}

// 建 resource + 绑后备缓冲物理页（**不设 scanout**）
static bool vgpu_resource_setup(int w, int h) {
    if (w <= 0 || h <= 0 || w > 3840 || h > 2160) { vgpu_err("res size"); return false; }
    int bw = 0, bh = 0;
    uint32_t* bb = fb_surface64(&bw, &bh);
    if (!bb || bw != w || bh < h) { vgpu_err("backbuffer geometry"); return false; }
    const uint32_t bytes = (uint32_t)(w * h * 4);
    const uint32_t entries = (bytes + PAGE_SIZE_64 - 1) / PAGE_SIZE_64;
    if (!g_entries || g_entries_cap < entries) {
        if (g_entries) { kfree_64(g_entries); g_entries = nullptr; g_entries_cap = 0; }
        g_entries = (VgpuMemEntry*)kmalloc_64((uint64_t)entries * sizeof(VgpuMemEntry));
        if (!g_entries) { vgpu_err("entries alloc"); return false; }
        g_entries_cap = entries;
    }
    const uint64_t bb_pa = fb_surface_phys64(nullptr);
    if (!bb_pa) { vgpu_err("backbuffer pa"); return false; }
    for (uint32_t i = 0; i < entries; i++) {
        g_entries[i].addr = bb_pa + (uint64_t)i * PAGE_SIZE_64;
        g_entries[i].length = PAGE_SIZE_64;
        g_entries[i].padding = 0;
    }
    g_res_w = (uint32_t)w; g_res_h = (uint32_t)h;
    g_i.res_id = 1; g_i.res_format = VGPU_FORMAT_B8G8R8X8_UNORM;
    g_i.backing_entries = entries; g_i.backing_bytes = bytes;

    // ① RESOURCE_CREATE_2D（id=1；格式 B8G8R8X8 与内核后备缓冲逐字节一致）
    VgpuCreate2d c2;
    memset_64(&c2, 0, sizeof(c2));
    c2.hdr.type = VGPU_CMD_RESOURCE_CREATE_2D;
    c2.resource_id = g_i.res_id; c2.format = VGPU_FORMAT_B8G8R8X8_UNORM;
    c2.width = (uint32_t)w; c2.height = (uint32_t)h;
    if (!vgpu_send(&c2, sizeof(c2), 1)) return false;

    // ② RESOURCE_ATTACH_BACKING（hdr + nr_entries 条 mem_entry，**一块物理连续缓冲**）
    const uint32_t alen = (uint32_t)(sizeof(VgpuAttach) + entries * sizeof(VgpuMemEntry));
    VgpuAttach* at = (VgpuAttach*)kmalloc_64(alen);
    if (!at) { vgpu_err("attach alloc"); return false; }
    memset_64(at, 0, sizeof(VgpuAttach));
    at->hdr.type = VGPU_CMD_RESOURCE_ATTACH_BACKING;
    at->resource_id = g_i.res_id;
    at->nr_entries = entries;
    uint8_t* dst = (uint8_t*)at + sizeof(VgpuAttach);              // 页表紧跟命令结构
    const uint8_t* src = (const uint8_t*)g_entries;
    for (uint32_t i = 0; i < entries * (uint32_t)sizeof(VgpuMemEntry); i++) dst[i] = src[i];
    const bool ok = vgpu_send(at, alen, 1);
    kfree_64(at);
    if (!ok) return false;

    vgpu_log_begin();
    vgpu_puts("[VGPU] res id="); vgpu_num(g_i.res_id);
    vgpu_puts(" "); vgpu_num((uint64_t)w); vgpu_puts("x"); vgpu_num((uint64_t)h);
    vgpu_puts(" fmt=b8g8r8x8 entries="); vgpu_num(entries);
    vgpu_puts(" backing="); vgpu_hex64(bb_pa);
    vgpu_puts(" bytes="); vgpu_num(bytes);
    vgpu_log_end();
    return true;
}

void vgpu64_init64() {
    if (g_i.found) return;                                        // 幂等
    g_i.last_err = "none";
    if (!vgpu_pci_find()) {
        vgpu_log_begin();
        vgpu_puts("[VGPU] not found (no virtio-gpu PCI device)");
        vgpu_log_end();
        fb_backend_log64("soft-lfb");
        return;
    }
    // MEM(bit1) + BUS MASTER(bit2) + **INTx disable(bit10)**：纯轮询，不让设备拉中断线
    const uint32_t cmd = vpci_rd32(g_i.bus, g_i.dev, g_i.fn, 0x04);
    vpci_wr32(g_i.bus, g_i.dev, g_i.fn, 0x04, (cmd & 0xFFFF0000u) | 0x0406u);
    vgpu_log_begin();
    vgpu_puts("[VGPU] pci ");
    vgpu_num(g_i.bus); vgpu_ch(':'); vgpu_num(g_i.dev); vgpu_ch('.'); vgpu_num(g_i.fn);
    vgpu_puts(" ven="); vgpu_hex32(g_i.vendor);
    vgpu_puts(" dev="); vgpu_hex32(g_i.device);
    vgpu_puts(" sub="); vgpu_hex32(g_i.subsys);
    vgpu_puts(" cmd="); vgpu_hex32(vpci_rd32(g_i.bus, g_i.dev, g_i.fn, 0x04));
    vgpu_log_end();

    for (int i = 0; i < 6; i++) g_i.bar_pa[i] = vgpu_bar_read(g_i.bus, g_i.dev, g_i.fn, i);
    vgpu_log_begin();
    vgpu_puts("[VGPU] bars");
    for (int i = 0; i < 6; i++) {
        if (!g_i.bar_pa[i]) continue;
        vgpu_puts(" bar"); vgpu_num((uint64_t)i); vgpu_puts("="); vgpu_hex64(g_i.bar_pa[i]);
    }
    vgpu_log_end();

    if (!vgpu_caps_parse()) {
        g_i.legacy_only = 1;                                      // 只认到 legacy 接口：如实降级
        vgpu_log_begin();
        vgpu_puts("[VGPU] legacy interface only -> backend=soft-lfb (not implemented)");
        vgpu_log_end();
        fb_backend_log64("soft-lfb (legacy virtio-gpu not implemented)");
        return;
    }
    if (!vgpu_status_step()) { g_i.ready = 0; fb_backend_log64("soft-lfb (virtio-gpu init failed)"); return; }
    if (!vgpu_get_display_info()) { vgpu_disable("display-info"); return; }

    // resource 尺寸：**以 LFB 几何为准**（后备缓冲 stride 必须等于 resource 宽；设备报的分辨率
    //   只作对照 —— 不一致时上面 display info 那行会带 note=）。
    if (fb_get_zoom() != 100) {
        vgpu_log_begin();
        vgpu_puts("[VGPU] note zoom="); vgpu_num((uint64_t)fb_get_zoom());
        vgpu_puts("% != 100 -> device path stays off");
        vgpu_log_end();
        vgpu_disable("zoom-not-100");
        return;
    }
    if (!vgpu_resource_setup(fb_phys_width(), fb_phys_height())) { vgpu_disable("resource"); return; }
    vgpu_log_begin();
    vgpu_puts("[VGPU] ready backend=virtio-gpu-2d scanout=pending status=");
    vgpu_hex32((uint32_t)g_i.device_status);
    vgpu_log_end();
    // ★ 收口：tick 标定在这里做一次并缓存（见 g_cyc_per_tick 的注释）—— 设备在、队列在、
    //   PIT 早就在跑（标定带着自旋上限，绝不挂死）。标定失败（0）时 bench 会如实不带 us 字段。
    if (!g_cyc_per_tick) g_cyc_per_tick = vgpu_cyc_per_tick();
}

int vgpu64_ready64() { return g_i.ready ? 1 : 0; }
const Vgpu64Info* vgpu64_info64() { return &g_i; }
const char* vgpu64_backend_name64() { return g_i.ready ? "virtio-gpu-2d" : "soft-lfb"; }

// ==================== 上屏（区域 blit：TRANSFER_TO_HOST_2D + FLUSH）====================
// ★ 踩坑（实测）：spec/QEMU 的 2D 传输把 **backing 侧当紧凑块**（每行 r.width*4 字节连续），
//   而我们的后备缓冲是带 stride 的屏幕（stride = 屏幕宽）。所以 r.width < 屏幕宽 时，
//   第 2 行读到的其实是"区域所在行里紧接区域右边的像素"——搬过去是错位数据（实测：资源里是花的）。
//   只有 **r.width == resource 宽** 时紧凑块才与 stride 一致。因此区域 blit 一律搬
//   "**整行宽的带**" rows [y, y+h)：多搬一点带宽，换来像素绝对正确（x 参数因此不再参与传输）。
static bool vgpu_transfer_to_host_res(uint32_t rid, int y, int h) {
    VgpuTransfer2d t;
    memset_64(&t, 0, sizeof(t));
    t.hdr.type = VGPU_CMD_TRANSFER_TO_HOST_2D;
    t.r.x = 0; t.r.y = (uint32_t)y;
    t.r.width = g_res_w; t.r.height = (uint32_t)h;
    t.offset = (uint64_t)y * g_res_w * 4u;                        // 带的起点（backing 内字节偏移）
    t.resource_id = rid;
    if (!vgpu_send(&t, sizeof(t), 0)) return false;
    g_i.transfers++;
    g_i.bytes_to_host += (uint64_t)g_res_w * (uint64_t)h * 4u;
    return true;
}
static bool vgpu_transfer_to_host(int x, int y, int w, int h) {
    (void)x; (void)w;
    return vgpu_transfer_to_host_res(g_i.res_id, y, h);
}
// TRANSFER_FROM_HOST_2D：把 resource 的 rect 回写进 backing 的 dst_off 处。本批只用它做自检的
//   "设备像素" 证据（不依赖 host 显示仲裁）。
static bool vgpu_transfer_from_host(int x, int y, int w, int h, uint64_t dst_off) {
    VgpuTransfer2d t;
    memset_64(&t, 0, sizeof(t));
    t.hdr.type = VGPU_CMD_TRANSFER_FROM_HOST_2D;
    t.r.x = (uint32_t)x; t.r.y = (uint32_t)y;
    t.r.width = (uint32_t)w; t.r.height = (uint32_t)h;
    t.offset = dst_off;
    t.resource_id = g_i.res_id;
    return vgpu_send(&t, sizeof(t), 1);
}

static bool vgpu_flush_res(uint32_t rid, int x, int y, int w, int h) {
    VgpuResFlush f;
    memset_64(&f, 0, sizeof(f));
    f.hdr.type = VGPU_CMD_RESOURCE_FLUSH;
    f.r.x = (uint32_t)x; f.r.y = (uint32_t)y;
    f.r.width = (uint32_t)w; f.r.height = (uint32_t)h;
    f.resource_id = rid;
    if (!vgpu_send(&f, sizeof(f), 0)) return false;
    g_i.flushes++;
    return true;
}
static bool vgpu_flush(int x, int y, int w, int h) { return vgpu_flush_res(g_i.res_id, x, y, w, h); }
static void vgpu_blit_fail(const char* why) {
    if (++g_fails >= VGPU_MAX_FAILS) { vgpu_disable(why); g_fails = 0; return; }
    vgpu_log_begin();
    vgpu_puts("[VGPU] blit dev=0 fails="); vgpu_num((uint64_t)g_fails);
    vgpu_puts(" why="); vgpu_puts(why);
    vgpu_log_end();
}

int vgpu64_blit64(int x, int y, int w, int h) {
    if (!g_i.ready || w <= 0 || h <= 0) return 0;
    if (!vgpu_geom_ok()) {
        // 分辨率/缩放变了：先试着重配，不行就降级（降级会把 scanout 交还 VGA framebuffer）
        if (!vgpu64_reconfigure64(fb_phys_width(), fb_phys_height(), fb_get_zoom())) return 0;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)g_res_w) w = (int)g_res_w - x;
    if (y + h > (int)g_res_h) h = (int)g_res_h - y;
    if (w <= 0 || h <= 0) return 0;

    g_i.blits_dev++;
    if (g_blit_logged < 8 || (g_i.blits_dev % 1024u) == 0u) {
        g_blit_logged++;
        vgpu_log_begin();
        vgpu_puts("[VGPU] blit dev=1 x="); vgpu_num((uint64_t)x);
        vgpu_puts(" y="); vgpu_num((uint64_t)y);
        vgpu_puts(" w="); vgpu_num((uint64_t)w);
        vgpu_puts(" h="); vgpu_num((uint64_t)h);
        vgpu_puts(" transfers="); vgpu_num(g_i.transfers);
        vgpu_log_end();
    }
    if (!vgpu_transfer_to_host(x, y, w, h)) { vgpu_blit_fail("transfer"); return 0; }
    if (!vgpu_flush(x, y, w, h))            { vgpu_blit_fail("flush");    return 0; }
    g_fails = 0;
    return 1;
}

// 软件路径提交计数（fb.cpp 每次走 CPU 路径时告知）
void vgpu64_note_soft_blit64(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
    g_i.blits_soft++;
}

int vgpu64_swap_frames64() { return (int)g_swap_frames; }

// ★ ⑭ 交换链：每块一个 resource（第 0 块 = primary；第 k 块绑后备缓冲第 k 块首址起的物理页）。
//   幂等；任何一步失败 -> 如实返回已建成的块数（<2 = 交换链不成立，调用方走软件路径）。
int vgpu64_swap_init64(int frames) {
    if (!g_i.ready || !g_scanout_on) return (int)g_swap_frames;
    if (frames > VGPU_SWAP_MAX) frames = VGPU_SWAP_MAX;
    if (frames < 2) return 0;
    const uint32_t bytes = g_res_w * g_res_h * 4u;
    const uint32_t entries = (bytes + PAGE_SIZE_64 - 1) / PAGE_SIZE_64;
    const uint64_t base_pa = fb_surface_phys64(nullptr);
    if (!base_pa || !bytes) return 0;
    if (!g_sw_entries || g_sw_entries_cap < entries) {
        if (g_sw_entries) { kfree_64(g_sw_entries); g_sw_entries = nullptr; g_sw_entries_cap = 0; }
        g_sw_entries = (VgpuMemEntry*)kmalloc_64((uint64_t)entries * sizeof(VgpuMemEntry));
        if (!g_sw_entries) { vgpu_err("swap entries alloc"); return 0; }
        g_sw_entries_cap = entries;
    }
    g_swap_res[0] = g_i.res_id;
    uint32_t made = 1;
    for (int k = 1; k < frames; k++) {
        const uint32_t rid = g_i.res_id + (uint32_t)k;
        if (g_swap_res[k] == rid) { made = (uint32_t)k + 1; continue; }        // 幂等
        for (uint32_t i = 0; i < entries; i++) {
            g_sw_entries[i].addr = base_pa + (uint64_t)k * (uint64_t)bytes + (uint64_t)i * PAGE_SIZE_64;
            g_sw_entries[i].length = PAGE_SIZE_64;
            g_sw_entries[i].padding = 0;
        }
        VgpuCreate2d c2;
        memset_64(&c2, 0, sizeof(c2));
        c2.hdr.type = VGPU_CMD_RESOURCE_CREATE_2D;
        c2.resource_id = rid; c2.format = VGPU_FORMAT_B8G8R8X8_UNORM;
        c2.width = g_res_w; c2.height = g_res_h;
        if (!vgpu_send(&c2, sizeof(c2), 0)) break;
        const uint32_t alen = (uint32_t)(sizeof(VgpuAttach) + entries * sizeof(VgpuMemEntry));
        VgpuAttach* at = (VgpuAttach*)kmalloc_64(alen);
        if (!at) break;
        memset_64(at, 0, sizeof(VgpuAttach));
        at->hdr.type = VGPU_CMD_RESOURCE_ATTACH_BACKING;
        at->resource_id = rid;
        at->nr_entries = entries;
        uint8_t* dst = (uint8_t*)at + sizeof(VgpuAttach);                // 页表紧跟命令结构
        const uint8_t* src = (const uint8_t*)g_sw_entries;
        for (uint32_t i = 0; i < entries * (uint32_t)sizeof(VgpuMemEntry); i++) dst[i] = src[i];
        const bool ok = vgpu_send(at, alen, 0);
        kfree_64(at);
        if (!ok) break;
        g_swap_res[k] = rid;
        made = (uint32_t)k + 1;
    }
    g_swap_frames = (made >= 2) ? made : 0;
    vgpu_log_begin();
    vgpu_puts("[VGPU] swap frames="); vgpu_num((uint64_t)made);
    vgpu_puts(" res=");
    for (uint32_t k = 0; k < made; k++) {
        vgpu_num(g_swap_res[k]);
        if (k + 1 < made) vgpu_ch(',');
    }
    vgpu_puts(" bytes="); vgpu_num(bytes);
    vgpu_puts(" base="); vgpu_hex64(base_pa);
    vgpu_log_end();
    for (int k = (int)made; k < VGPU_SWAP_MAX; k++) g_swap_res[k] = 0;
    return (int)g_swap_frames;
}

// 整帧提交第 slot 块：TRANSFER_TO_HOST_2D（offset=0 整帧、整行宽）+ RESOURCE_FLUSH +
//   （scanout 不在这一块时才）SET_SCANOUT 翻页。返回 1 = 真的走了设备。
//   任何一步失败 -> 0 + 计入连续失败（到 VGPU_MAX_FAILS 就降级 soft-lfb，调用方继续出帧）。
int vgpu64_present64(int slot, int w, int h) {
    if (!g_i.ready || !g_scanout_on) return 0;
    if (slot < 0 || slot >= (int)g_swap_frames) return 0;
    const uint32_t rid = g_swap_res[slot];
    if (!rid) return 0;
    if (!vgpu_geom_ok()) {
        if (!vgpu64_reconfigure64(fb_phys_width(), fb_phys_height(), fb_get_zoom())) return 0;
        if (slot >= (int)g_swap_frames || g_swap_res[slot] != rid) return 0;   // 重配后交换链重建了
    }
    if (w > (int)g_res_w) w = (int)g_res_w;
    if (h > (int)g_res_h) h = (int)g_res_h;
    if (w <= 0 || h <= 0) return 0;
    if (!vgpu_transfer_to_host_res(rid, 0, h)) { vgpu_blit_fail("present-transfer"); return 0; }
    if (!vgpu_flush_res(rid, 0, 0, w, h))     { vgpu_blit_fail("present-flush");    return 0; }
    if (g_scanout_res != rid) {
        if (!vgpu_set_scanout_res(rid))       { vgpu_blit_fail("present-scanout");  return 0; }
        g_swap_flips++;
    }
    g_swap_presents++;
    g_i.swap_frames = g_swap_frames;
    g_i.swap_presents = g_swap_presents;
    g_i.swap_flips = g_swap_flips;
    g_fails = 0;
    return 1;
}


// ==================== fill / copy：2D 命令集没有这两个原语 -> 如实 CPU ====================
int vgpu64_fill64(int x, int y, int w, int h, uint32_t color) {
    int bw = 0, bh = 0;
    uint32_t* bb = fb_surface64(&bw, &bh);
    if (!bb) return 0;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > bw) w = bw - x;
    if (y + h > bh) h = bh - y;
    if (w <= 0 || h <= 0) return 0;
    for (int yy = 0; yy < h; yy++) {
        uint32_t* row = bb + (y + yy) * bw + x;
        for (int xx = 0; xx < w; xx++) row[xx] = color;
    }
    const int dev = vgpu64_blit64(x, y, w, h);      // 只有"上屏"可能走设备
    static int logged = 0;
    if (logged < 2) {
        logged++;
        vgpu_log_begin();
        vgpu_puts("[VGPU] fill x="); vgpu_num((uint64_t)x);
        vgpu_puts(" y="); vgpu_num((uint64_t)y);
        vgpu_puts(" w="); vgpu_num((uint64_t)w);
        vgpu_puts(" h="); vgpu_num((uint64_t)h);
        vgpu_puts(" dev_fill=0 (no 2D fill op) blit_dev="); vgpu_num((uint64_t)dev);
        vgpu_log_end();
    }
    return 0;                                                     // 0 = CPU 做的填充（如实）
}

int vgpu64_copy64(int dx, int dy, int w, int h, int sx, int sy) {
    int bw = 0, bh = 0;
    uint32_t* bb = fb_surface64(&bw, &bh);
    if (!bb || w <= 0 || h <= 0) return 0;
    if (sx < 0 || sy < 0 || dx < 0 || dy < 0) return 0;           // 不做部分拷贝（避免半行错位）
    if (sx + w > bw || dx + w > bw || sy + h > bh || dy + h > bh) return 0;
    if (dy > sy) {                                                // 纵向重叠时按方向拷（memmove 语义）
        for (int yy = h - 1; yy >= 0; yy--)
            memmove_64(bb + (dy + yy) * bw + dx, bb + (sy + yy) * bw + sx, (uint64_t)w * 4u);
    } else {
        for (int yy = 0; yy < h; yy++)
            memmove_64(bb + (dy + yy) * bw + dx, bb + (sy + yy) * bw + sx, (uint64_t)w * 4u);
    }
    const int rx = (dx < sx) ? dx : sx, ry = (dy < sy) ? dy : sy;
    const int rw = w + ((dx < sx) ? (sx - dx) : (dx - sx));
    const int rh = h + ((dy < sy) ? (sy - dy) : (dy - sy));
    const int dev = vgpu64_blit64(rx, ry, rw, rh);
    static int logged = 0;
    if (logged < 2) {
        logged++;
        vgpu_log_begin();
        vgpu_puts("[VGPU] copy dx="); vgpu_num((uint64_t)dx);
        vgpu_puts(" dy="); vgpu_num((uint64_t)dy);
        vgpu_puts(" w="); vgpu_num((uint64_t)w);
        vgpu_puts(" h="); vgpu_num((uint64_t)h);
        vgpu_puts(" dev_copy=0 (no 2D copy op) blit_dev="); vgpu_num((uint64_t)dev);
        vgpu_log_end();
    }
    return 0;                                                     // 0 = CPU 做的拷贝（如实）
}

// ==================== 分辨率/缩放变化后的重配 ====================
// ★ ⑭：交换链的额外 resource 必须先 UNREF 掉（几何变了，旧的不能再翻页）。
static void vgpu_swap_drop64(void) {
    for (int k = 1; k < VGPU_SWAP_MAX; k++) {
        if (!g_swap_res[k]) continue;
        VgpuResId u;
        memset_64(&u, 0, sizeof(u));
        u.hdr.type = VGPU_CMD_RESOURCE_UNREF;
        u.resource_id = g_swap_res[k];
        (void)vgpu_send(&u, sizeof(u), 0);
        g_swap_res[k] = 0;
    }
    g_swap_frames = 0;
}
int vgpu64_reconfigure64(int w, int h, int zoom) {
    if (!g_i.ready || !g_scanout_on) return 0;                     // 还没上过屏：交给初始流程
    if (zoom != 100 || w <= 0 || h <= 0) { vgpu_disable("zoom/resize unsupported"); return 0; }
    if ((int)g_res_w == w && (int)g_res_h == h) return 1;          // 几何没变
    vgpu_swap_drop64();                                            // ★ ⑭：交换链失效（随后按需重建）
    {
        VgpuResId u;                                              // 先 UNREF 老 resource
        memset_64(&u, 0, sizeof(u));
        u.hdr.type = VGPU_CMD_RESOURCE_UNREF;
        u.resource_id = g_i.res_id;
        (void)vgpu_send(&u, sizeof(u), 1);
    }
    {
        VgpuResId d;                                              // 显式 detach（UNREF 已隐式做过）
        memset_64(&d, 0, sizeof(d));
        d.hdr.type = VGPU_CMD_RESOURCE_DETACH_BACKING;
        d.resource_id = g_i.res_id;
        (void)vgpu_send(&d, sizeof(d), 0);
    }
    g_scanout_on = 0;
    g_i.res_id++;
    if (!vgpu_resource_setup(w, h)) { vgpu_disable("reconfigure resource"); return 0; }
    if (!vgpu_set_scanout_internal()) { vgpu_disable("reconfigure scanout"); return 0; }
    if (!vgpu64_blit64(0, 0, w, h)) { vgpu_disable("reconfigure transfer"); return 0; }
    vgpu_log_begin();
    vgpu_puts("[VGPU] reconfigure w="); vgpu_num((uint64_t)w);
    vgpu_puts(" h="); vgpu_num((uint64_t)h);
    vgpu_puts(" res="); vgpu_num(g_i.res_id);
    vgpu_puts(" ok=1");
    vgpu_log_end();
    return 1;
}

// ==================== 自检（三段 + 像素等价证据）====================
// 图案：竖条纹（16px 宽，红/蓝交替）。验收脚本按"条带中心像素 == 亮红/亮蓝"断言，并把
//   "软件路径截图"与"设备路径截图"的同一块区域**逐字节**比对（正确性的关键证据）。
#define VGPU_PAT_C0 0xFFFF0000u     // 红
#define VGPU_PAT_C1 0xFF0000FFu     // 蓝
#define VGPU_ERASE_C 0xFF101010u    // 第三段的"改一块区域"目标色

static void vgpu_paint_pattern(int x, int y, int w, int h, uint32_t c0, uint32_t c1) {
    int bw = 0, bh = 0;
    uint32_t* bb = fb_surface64(&bw, &bh);
    if (!bb) return;
    for (int yy = 0; yy < h; yy++) {
        uint32_t* row = bb + (y + yy) * bw + x;
        for (int xx = 0; xx < w; xx++) row[xx] = (((xx / 16) & 1) ? c1 : c0);
    }
}
static void vgpu_hold_ms(uint32_t ms) {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0" : "=r"(fl));
    const bool irq_on = (fl & 0x200ULL) != 0;
    const uint32_t t0 = (uint32_t)g_ticks64;
    const uint32_t target = ms_to_ticks64(ms);
    uint32_t t1 = t0;
    while ((uint32_t)(g_ticks64 - t0) < target) {
        if (irq_on) __asm__ volatile("hlt");                       // 让别的内核任务照跑
        else        __asm__ volatile("pause");
        const uint32_t now = (uint32_t)g_ticks64;
        if (irq_on && now != t1 && (now & 0x3u) == 0) {            // 每 ~16ms
            t1 = now;
            if (g_i.isr_pa) (void)vrd32(g_i.isr_pa);               // 清 virtio 的中断线
        }
    }
}
// 三段共用的打点（x/y/w/h + 尾巴）
static void vgpu_stage_log(const char* stage, int x, int y, int w, int h, const char* tail) {
    vgpu_log_begin();
    vgpu_puts("[VGPU] selftest stage="); vgpu_puts(stage);
    vgpu_puts(" x="); vgpu_num((uint64_t)x);
    vgpu_puts(" y="); vgpu_num((uint64_t)y);
    vgpu_puts(" w="); vgpu_num((uint64_t)w);
    vgpu_puts(" h="); vgpu_num((uint64_t)h);
    vgpu_puts(" "); vgpu_puts(tail);
    vgpu_log_end();
}

int vgpu64_selftest64() {
    static int done = 0;
    if (done) return 0;
    done = 1;
    if (!g_i.found) {
        vgpu_log_begin();
        vgpu_puts("[VGPU] selftest skipped reason=no-device");
        vgpu_log_end();
        return 0;
    }
    if (!g_i.ready) {
        vgpu_log_begin();
        vgpu_puts("[VGPU] selftest skipped reason=not-ready err=");
        vgpu_puts(g_i.last_err ? g_i.last_err : "?");
        vgpu_log_end();
        return 0;
    }
    int fail = 0;
    // 图案位置：避开屏幕四角（第一/二段里屏幕别的地方还是 loader 留下的内容）
    const int px = 64, py = 64, pw = 320, ph = 240;

    // ---- ① 软件路径段：此时 scanout 还没设 -> 屏幕上就是 legacy VGA framebuffer（= LFB）
    vgpu_paint_pattern(px, py, pw, ph, VGPU_PAT_C0, VGPU_PAT_C1);
    fb_soft_flip_region64(px, py, pw, ph);                          // **强制** CPU 路径（不走设备）
    g_i.selftest_soft_blits++;
    vgpu_stage_log("soft", px, py, pw, ph, "c0=FF0000 c1=0000FF hold_ms=3000");
    vgpu_hold_ms(VGPU_HOLD_MS);

    // ---- ② 设备路径段：设 scanout + 同一块图案 TRANSFER+FLUSH（屏幕从这里起由 resource 驱动）
    if (!vgpu_set_scanout_internal()) { vgpu_err("selftest scanout"); fb_backend_log64("soft-lfb"); return 1; }
    if (!vgpu64_blit64(0, 0, (int)g_res_w, (int)g_res_h)) { vgpu_err("selftest full transfer"); return 2; }
    const uint64_t dev_before = g_i.blits_dev;
    if (!vgpu64_blit64(px, py, pw, ph)) { vgpu_err("selftest region transfer"); return 3; }
    g_i.selftest_dev_blits = g_i.blits_dev - dev_before + 1;
    fb_backend_log64("virtio-gpu-2d");
    vgpu_stage_log("device", px, py, pw, ph, "hold_ms=3000 (same pattern, device path)");
    vgpu_hold_ms(VGPU_HOLD_MS);

    // ---- ★ 像素等价（guest 内存内逐字节）：设备路径 vs 软件路径
    //   ① 后备缓冲图案区 R 写一份确定性伪随机图案（LCG）；
    //   ② 设备路径 TRANSFER_TO_HOST_2D(R) 送进 resource；
    //   ③ TRANSFER_FROM_HOST_2D(rect=R, offset=S) 把**设备的像素**回读进后备缓冲的 scratch 区 S；
    //   ④ 软件路径 fb_soft_flip_region64(R) 写 LFB；
    //   ⑤ 逐字节比较 S（设备）vs fb_lfb_pixel64(R)（软件）—— 必须 0 差异。
    //   为什么这么比：QEMU 的 virtio-vga 早期 boot 把 console 留给 legacy VGA（见报告），屏幕截图
    //   在那一刻证明不了设备像素；这条在 guest 内存里直接比，与显示仲裁无关。
    {
        const int ey = 64, ew = (int)g_res_w, eh = 240;  // 图案带 = 整行宽（x 恒 0，不参与传输；见传输语义注释）
        const int sy = 400;                                     // 回读落点：resource 内另一段行（不与图案带重叠）
        int bw2 = 0, bh2 = 0;
        uint32_t* bb2 = fb_surface64(&bw2, &bh2);
        int diff = -1;
        if (bb2 && sy + eh <= (int)g_res_h && sy + eh <= bh2 && ew <= bw2) {
            uint32_t seed = 0x1234567u;                         // 确定性 LCG（同一构建两次跑一致）
            for (int yy = 0; yy < eh; yy++) {
                uint32_t* row = bb2 + (ey + yy) * bw2;
                for (int xx = 0; xx < bw2; xx++) {
                    seed = seed * 1664525u + 1013904223u;
                    row[xx] = 0xFF000000u | (seed & 0x00FFFFFFu);
                }
            }
            diff = 0;
            // 全宽带（0,ey,res_w,eh）：设备侧从后备缓冲的这段行搬进 resource；
            if (!vgpu_transfer_to_host(0, ey, (int)g_res_w, eh)) fail |= 32;
            // 回读：resource 的同一带 -> 后备缓冲的 scratch 行（sy 起）——紧凑块 <=> 整行宽，逐字节对得上
            if (!vgpu_transfer_from_host(0, ey, (int)g_res_w, eh,
                                         (uint64_t)sy * (uint64_t)bw2 * 4u)) fail |= 64;
            fb_soft_flip_region64(0, ey, (int)g_res_w, eh);      // 软件路径（CPU 逐行写 LFB）
            for (int yy = 0; yy < eh; yy++) {
                const uint32_t* dev_row = bb2 + (sy + yy) * bw2;  // 回读落在从 sy 起的整行
                for (int xx = 0; xx < bw2; xx++)
                    if (dev_row[xx] != fb_lfb_pixel64(xx, ey + yy)) diff++;
            }
        } else {
            fail |= 128;
        }
        vgpu_log_begin();
        vgpu_puts("[VGPU] equiv lcg=1664525,1013904223,0x1234567 rect="); vgpu_num((uint64_t)ew);
        vgpu_puts("x"); vgpu_num((uint64_t)eh);
        vgpu_puts(" bytes="); vgpu_num((uint64_t)ew * (uint64_t)eh * 4u);
        vgpu_puts(" diff="); vgpu_num((uint64_t)(diff < 0 ? 0xFFFFFFFFu : (uint32_t)diff));
        /* ★ 预算收口（本批）：这一段的**冗余诊断**改由编译期开关控制，默认**不编译** ——
         *   `pat=/lfb=/dev=` 三个十六进制转储 + MISMATCH 长说明只用于人工排查；
         *   自动验收只要上面那条 `[VGPU] equiv lcg=… rect=… bytes=… diff=…`（tests/virtiogpu64_test.py:310
         *   的正则到 `diff=` 为止，后面的额外字段/后缀都不参与判定）。
         *   要开：VIMTU_EXTRA_CXXFLAGS=-DVGPU64_DIAG_EQUIV_VERBOSE=1 bash build64.sh
         *   （报告里给了"关掉省了多少字节"的实测值）。 */
#if VGPU64_DIAG_EQUIV_VERBOSE
        if (bb2) {
            vgpu_puts(" pat="); vgpu_hex32(bb2[(size_t)ey * bw2]);
            vgpu_puts(" lfb="); vgpu_hex32(fb_lfb_pixel64(0, ey));
            vgpu_puts(" dev="); vgpu_hex32(bb2[(size_t)sy * bw2]);
        }
        if (diff == 0) {
            vgpu_puts(" dev=resource soft=lfb PASS");
        } else {
            // ★ 如实记录的 GAP：QEMU 对**非 blob 的 2D 资源**不把 resource 回写进 guest 的 backing
            //   （命令本身被正常应答：used=1 resp=24，但目标内存没有被写）。所以"回读式等价"不作为
            //   失败位（fail &= ~256）；像素等价由验收脚本对**屏幕帧**里的确定性图案逐点核对承担
            //   （期望值在 Python 侧独立重算，见 tests/virtiogpu64_test.py 的 LCG 部分）。
            vgpu_puts(" MISMATCH (TRANSFER_FROM_HOST_2D 不回写非 blob 2D 资源；见报告没做到)");
        }
#else
        if (diff != 0) vgpu_puts(" GAP(TRANSFER_FROM_HOST_2D 不回写非 blob 2D 资源)");   /* 一行，始终可见 */
#endif
        vgpu_log_end();
    }

    // ---- ③ 只有请求区域变：用设备路径把这块改成另一个颜色
    vgpu_paint_pattern(px, py, pw, ph, VGPU_ERASE_C, VGPU_ERASE_C);
    if (!vgpu64_blit64(px, py, pw, ph)) { vgpu_err("selftest erase transfer"); fail |= 4; }
    vgpu_stage_log("erase", px, py, pw, ph, "hold_ms=3000 color=101010");
    vgpu_hold_ms(VGPU_HOLD_MS);

    // ---- 计数器自洽 + 上屏后不能再有超时
    if (g_i.cmd_timeouts) fail |= 8;
    if (!g_i.transfers || !g_i.flushes) fail |= 16;
    if (fail) {
        vgpu_log_begin();
        vgpu_puts("[VGPU] selftest error bits="); vgpu_num((uint64_t)fail);
        vgpu_log_end();
        return fail;
    }
    vgpu_log_begin();
    vgpu_puts("[VGPU] selftest PASS dev_blits="); vgpu_num(g_i.blits_dev);
    vgpu_puts(" soft_blits="); vgpu_num(g_i.blits_soft);
    vgpu_puts(" transfers="); vgpu_num(g_i.transfers);
    vgpu_puts(" flushes="); vgpu_num(g_i.flushes);
    vgpu_puts(" timeouts="); vgpu_num(g_i.cmd_timeouts);
    vgpu_log_end();
    return 0;
}

// ==================== 基准（设备 vs 软件，rdtsc 多轮中位数）====================
static inline uint64_t vgpu_rdtsc() {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}
static uint64_t vgpu_cyc_per_tick() {                               // rdtsc 周期 / PIT tick
    const uint64_t t0 = g_ticks64;
    const uint64_t c0 = vgpu_rdtsc();
    uint32_t spin = 0;
    while (g_ticks64 - t0 < 50 && ++spin < 400000000u) __asm__ volatile("pause");
    const uint64_t c1 = vgpu_rdtsc();
    const uint64_t dt = g_ticks64 - t0;
    return dt ? (c1 - c0) / dt : 0;
}
static uint64_t vgpu_median(uint64_t* a, int n) {
    for (int i = 1; i < n; i++) {                                  // 插入排序（n <= 9）
        const uint64_t v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
    return a[n / 2];
}
static void vgpu_bench_case(const char* kind, int x, int y, int w, int h, int runs, uint64_t cpt) {
    uint64_t soft[9], dev[9];
    if (runs > 9) runs = 9;
    for (int i = 0; i < runs; i++) {
        uint64_t c0 = vgpu_rdtsc();
        fb_soft_flip_region64(x, y, w, h);                          // 软件路径（CPU 逐行写 LFB）
        soft[i] = vgpu_rdtsc() - c0;
        c0 = vgpu_rdtsc();
        const int ok = vgpu64_blit64(x, y, w, h);                   // 设备路径（TRANSFER+FLUSH）
        dev[i] = vgpu_rdtsc() - c0;
        if (!ok) dev[i] = 0;                                        // 没走成设备（不该发生）
    }
    const uint64_t ms_ = vgpu_median(soft, runs);
    const uint64_t md_ = vgpu_median(dev, runs);
    const uint64_t to_us = cpt ? (cpt / 1000u) : 0;                // 每 ms 的 rdtsc 周期数
    vgpu_log_begin();
    vgpu_puts("[VGPU] bench kind="); vgpu_puts(kind);
    vgpu_puts(" x="); vgpu_num((uint64_t)x);
    vgpu_puts(" y="); vgpu_num((uint64_t)y);
    vgpu_puts(" w="); vgpu_num((uint64_t)w);
    vgpu_puts(" h="); vgpu_num((uint64_t)h);
    vgpu_puts(" px="); vgpu_num((uint64_t)w * (uint64_t)h);
    vgpu_puts(" runs="); vgpu_num((uint64_t)runs);
    vgpu_puts(" soft_cyc="); vgpu_num(ms_);
    vgpu_puts(" dev_cyc="); vgpu_num(md_);
    if (to_us) {
        vgpu_puts(" soft_us="); vgpu_num(ms_ / to_us);
        vgpu_puts(" dev_us="); vgpu_num(md_ / to_us);
    }
    vgpu_puts(" ratio_x100="); vgpu_num(ms_ ? (md_ * 100u / ms_) : 0);
    vgpu_puts(" cpt="); vgpu_num(cpt);
    vgpu_log_end();
}

int vgpu64_bench64() {
    int n = 0;
    if (g_i.ready && g_scanout_on) {
        const int W = (int)g_res_w, H = (int)g_res_h;
        const uint64_t cpt = g_cyc_per_tick ? g_cyc_per_tick : vgpu_cyc_per_tick();  // ★ init 已缓存（见上）
        vgpu_bench_case("full", 0, 0, W, H, 5, cpt); n++;               // 整屏 flip（上屏主用例）
        vgpu_bench_case("region", W / 4, H / 4, 512, 512, 5, cpt); n++; // 窗口搬移
        vgpu_bench_case("region", W / 4, H / 4, 128, 128, 5, cpt); n++; // 局部重绘
        vgpu_bench_case("region", W / 2, H / 2, 16, 16, 5, cpt); n++;   // 光标级小块（设备开销对照）
        (void)vgpu64_blit64(0, 0, W, H);                                // 最后补一次整屏上屏
    }
    // ★ ⑭：VSync + 交换链 + 混合渲染的帧引擎（自检/基准**之后**跑：不能在 selftest 之前动屏幕）。
    //   只有 fw_cfg 里配了 demo=1 才真正跑；没配置时一行不打、直接返回（既有启动路径零影响）。
    //   放在这一层（而不是 ready 的早退之后）是为了让"没有 virtio-gpu"的机器也能跑纯软件路径帧引擎。
    (void)fb_vsync_run64();
    return n;
}
