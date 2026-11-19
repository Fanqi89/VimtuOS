// mem64.cpp - 64 位内存管理：物理页池 + 内核堆 + 归属记账 + 编译器辅助例程
//
// 为什么单独一个文件（而不是照抄 32 位 mem.cpp）：
//   64 位内核里同时存在几处**固定占用**，堆和页池必须一个都不碰：
//     0x0007C000                内核栈（entry64.asm: mov rsp, 0x7C000）
//     0x00100000..__bss_end     内核映像 + .bss（含 fb 的 33MB 后备缓冲）—— **大小随内核变**
//     0x04000000..0x06000000    安装载荷暂存区（仅"安装介质"内核用；系统内核不用）
//     0x06000000..0x06040000    UEFI64.BIN（UEFI 长模式引导器；跳进内核之后已死）
//   32 位版把堆起点设为 ALIGN(_end)，把页 bitmap 也写在 _end 处，两者同址 ——
//   结果第一次 kmalloc 就把 FreeBlock 头写进了 bitmap，页分配器元数据被静默破坏。
//   64 位版直接把 bitmap 放进 .bss，从结构上消除这个缺陷。
//
// ★ v0.9.31（内核容量 15MB 那一批）把堆/页池彻底改成"跟着内核走"：
//   * 旧布局是**写死**的 [80MB,128MB) 堆 + 128MB 起的页池，而载荷暂存区固定 64MB 起。
//     内核容量升到 15MB 之后载荷也变长（system.img 16MB -> ~28MB），"载荷 64..93MB"
//     会直接顶穿写死的 80MB 堆。写死的地址在容量变大的那天一定会出事。
//   * 现在的规则：**堆起点 = max(堆下限 112MB, ALIGN_UP(__bss_end, 2MB))**，
//     即"内核长到哪儿，堆就从那之上开始，绝不会与自己映像重叠"；堆长度仍固定 48MB；
//     页池从堆顶开始，一直到 E820 认定的内存上界。
//   * 并且运行期把这条关系当**断言**打出来（[MEM64] layout ... guard=ok/fail），
//     构建期再有一条 nm 断言（build64.sh：__bss_end < 载荷暂存区 0x04000000）。
//     两头夹住之后，"内核把堆踩了"或"载荷把内核踩了"这类事故不可能再静默发生。

// ★ 包含顺序是硬约定：memlayout64.h 必须**先于** mem_64.h —— 后者把 PAGE_SIZE_64 / PAGE_SIZE_2M
//   定义成宏，先出现会让 memlayout64.h 里的同名 `static const` 变成非法声明
//   （别的文件里也有同样的注释，见 ahci64.cpp / ehci64.cpp / taskmgr64.cpp 的说明）。
#include "memlayout64.h"        // 磁盘/内存布局唯一定义点（堆下限 / 载荷窗口 / 页池下限）
#include "mem_64.h"
#include "port.h"
#include "debug64.h"
#include "../bootinfo.h"

extern "C" char __bss_end[];    // 内核映像高半区上界（= 高半区 VA；物理 = VA - ML64_KERNEL_VA_BASE）

// ==================== 中断屏蔽（保存 / 恢复 IF）====================
// 为什么不用裸 cli()/sti()：这几个函数会在两种危险场景被调用 ——
//   1) 启动早期，IDT 还没装载：此时无条件 sti() 会把 PIT/PS2 的待处理中断放进来，
//      而 CPU 拿不到向量表 -> #GP -> 没有处理程序 -> 三重故障（机器复位）；
//   2) 已经处于中断处理程序内部：无条件 sti() 会打开中断嵌套，堆链表可能被并发破坏。
// 所以统一用 irq_save64()/irq_restore64()：进入时保存 IF 原值，退出时按原样恢复。
static inline uint64_t irq_save64() {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    cli();
    return flags & 0x200ull;        // 只关心 IF 位
}
static inline void irq_restore64(uint64_t if_bit) {
    if (if_bit) sti();
}

// ==================== 布局常量与运行期布局 ====================
#define MEM64_PAGE        4096ull
// ★★ v0.11.0 内存扩容：bitmap 64 KiB -> **256 KiB**。
//   旧值 64KiB 只能记 64K*8 = 524,288 页 = 2 GiB 页池，于是 e820_mem_high64() 里的
//   `if (highest > MEM64_POOL_MAX) highest = MEM64_POOL_MAX;` 把 4GB 机器的 E820 上界
//   夹到 2.16 GiB —— "分配了 4GB、系统只认 2GB"的根因就是这两行。
//   （实测 -m 4096：`[LM64] E820 usable_above_1M_kb=4193152`（4.0 GiB）而
//     `[MEM64] pool=[0x0A000000,0x8A000000) pages=524288`（2.0 GiB）。）
//   256 KiB 覆盖 2,097,152 页 = 8 GiB 地址跨度（4GB/8GB 机器都够）。
//   位图仍在 .bss（+192 KiB）：__bss_end 约 50.9 MB，离 64 MiB 载荷窗口还有 13 MB 余量，
//   build64.sh 的 nm 闸门（__bss_end < 0x04000000）仍然成立 —— 再放大必须重核那条断言。
#define MEM64_BITMAP_BYTES (256u * 1024u)
// 位图能覆盖的最大地址跨度 = 下限 + 位图位数 * 4KiB（**不再是**"机器内存上限"，见下一条）
#define MEM64_POOL_MAX     ((uint64_t)ML64_POOL_PHYS_FLOOR + (uint64_t)MEM64_BITMAP_BYTES * 8ull * MEM64_PAGE)
// ★★ 直映硬边界：内核靠 **0..4GB 恒等映射**直接访问物理地址
//   （boot/loader64.asm 的 lm64_build_paging 建 PML4[0] -> PDPT 4 项 -> 4 张 PD，
//    每项 2MiB 大页；UEFI 路径沿用固件自己的映射）。
//   4GB 机器的 E820 第二段可用区在 **4GB 以上**（above-4G remap，实测
//   `E820[6] base=0x100000000 len=0x40000000 type=1`），内核**没有那段映射**，
//   把它划进页池等于第一次触碰就 #PF。所以页池上界必须再被这一条夹一次，
//   把"可用但内核够不着"的部分单独记账、如实打进串口，而不是假装用上了。
#define MEM64_DIRECT_MAP_BYTES ((uint64_t)ML64_IDENTITY_BYTES)   // 0x1_0000_0000 = 4 GiB

// ★★ 运行期布局（v0.9.31）：堆起点跟着内核结束地址走，页池跟着堆顶走。
//   在 mem_init_64() 里一次性算好，之后所有地方（含自检）只读这两个全局量。
static uint64_t g_heap_base  = ML64_HEAP_PHYS_FLOOR;
static uint64_t g_heap_limit = (uint64_t)ML64_HEAP_PHYS_FLOOR + ML64_HEAP_BYTES;
static inline uint64_t align_up64(uint64_t v, uint64_t a) { return (v + a - 1ull) & ~(a - 1ull); }

static uint8_t  g_page_bitmap[MEM64_BITMAP_BYTES];
static uint64_t g_pool_base   = ML64_POOL_PHYS_FLOOR;   // 运行期由 mem64_layout64() 重算
static uint64_t g_pool_end    = 0;      // 由 E820 决定（再被位图跨度与直映边界夹住）
static uint64_t g_page_span   = 0;      // ★ v0.11.0：池**跨度**页数（含 PCI 空洞等不可用页）
static uint64_t g_page_total  = 0;      // ★ v0.11.0：真可用页数（= E820 usable 段落在池内的部分）
static uint64_t g_page_free   = 0;
static uint64_t g_pool_rover  = 0;      // 分配游标（避免每次都从 0 扫）
// ★ v0.11.0：总内存的两个新口径（旧口径"页池上界地址"在有空洞的机器上是错的）
static uint64_t g_ram_usable   = 0;     // E820 usable 总量（>=1MB 段），与 dump_e820 同口径
static uint64_t g_ram_above4g  = 0;     // 其中 4GB 以上（可用但无直映，不进页池）

// ==================== 物理页池 ====================
static inline void bitmap_set64(uint64_t idx, bool used) {
    uint8_t mask = (uint8_t)(1u << (idx & 7));
    if (used) g_page_bitmap[idx >> 3] |=  mask;
    else      g_page_bitmap[idx >> 3] &= (uint8_t)~mask;
}
static inline bool bitmap_get64(uint64_t idx) {
    return (g_page_bitmap[idx >> 3] >> (idx & 7)) & 1u;
}

static uint64_t e820_mem_high64(const BootInfo* bi) {
    uint64_t highest = 0;
    if (bi && bi->magic == BOOT_INFO_MAGIC && bi->mem_map_addr) {
        const E820Entry* e = (const E820Entry*)(uintptr_t)bi->mem_map_addr;
        const uint32_t n = (bi->mem_entries < 64) ? bi->mem_entries : 64;
        for (uint32_t i = 0; i < n; i++) {
            if (e[i].type != 1) continue;
            const uint64_t base = ((uint64_t)e[i].base_high << 32) | e[i].base_low;
            const uint64_t len  = ((uint64_t)e[i].len_high  << 32) | e[i].len_low;
            if (base < 0x100000ull || len == 0) continue;
            const uint64_t end = base + len;
            if (end > highest) highest = end;
        }
    }
    if (highest == 0) highest = 0x10000000ull;      // 保底：按 256MB 算
    if (highest > MEM64_POOL_MAX)        highest = MEM64_POOL_MAX;         // ① 位图跨度够不够
    if (highest > MEM64_DIRECT_MAP_BYTES) highest = MEM64_DIRECT_MAP_BYTES; // ② 恒等映射够不够
    return highest;
}


// ★★ v0.11.0：按 E820 **usable 段**收集区间，并给出两个统计口径。
//   为什么必须按段（而不是像旧实现那样把 [pool_base, high) 当成一整段连续 RAM）：
//     4GB 机器的 E820 是 `[1MiB, 3.0GiB) usable` + `[3.0GiB, 4GiB) PCI 空洞(MMIO)` +
//     `[4GiB, 5GiB) usable`。中间那 1 GiB 是**设备地址空间**，当成 RAM 分配出去就是
//     写设备寄存器 / 写坏数据。旧实现因为页池被封顶在 2.16 GiB，恰好没跨到空洞，
//     所以这个缺陷一直没暴露 —— 放开上限的同一次改动必须把它一起修掉。
//   total_bytes ：E820 usable 总量（只算 1MB 以上，与 kernel64.cpp dump_e820() 的
//                 `usable_above_1M_kb` 完全同口径，便于两边对账）。
//   above4g     ：其中落在直映边界以上的部分（可用、但内核够不着）。
struct Mem64Range64 { uint64_t base; uint64_t end; };

static uint32_t e820_usable_ranges64(const BootInfo* bi, Mem64Range64* out, uint32_t max_n,
                                     uint64_t* total_bytes, uint64_t* above4g_bytes) {
    uint64_t total = 0, above = 0;
    uint32_t cnt = 0;
    if (bi && bi->magic == BOOT_INFO_MAGIC && bi->mem_map_addr) {
        const E820Entry* e = (const E820Entry*)(uintptr_t)bi->mem_map_addr;
        const uint32_t n = (bi->mem_entries < 64) ? bi->mem_entries : 64;
        for (uint32_t i = 0; i < n; i++) {
            if (e[i].type != 1) continue;
            uint64_t b = ((uint64_t)e[i].base_high << 32) | e[i].base_low;
            const uint64_t len = ((uint64_t)e[i].len_high << 32) | e[i].len_low;
            if (len == 0) continue;
            uint64_t top = b + len;
            if (b < 0x100000ull) b = 0x100000ull;      // 低端 640KB 归引导链/BIOS，不算
            if (top <= b) continue;
            total += (top - b);
            if (top > MEM64_DIRECT_MAP_BYTES) {
                const uint64_t lo = (b > MEM64_DIRECT_MAP_BYTES) ? b : MEM64_DIRECT_MAP_BYTES;
                if (top > lo) above += (top - lo);
            }
            if (out && cnt < max_n) {
                // 只收"能交给页池"的那一段，且夹在直映边界之内
                const uint64_t rb = b;
                const uint64_t rt = (top < MEM64_DIRECT_MAP_BYTES) ? top : MEM64_DIRECT_MAP_BYTES;
                if (rt > rb) { out[cnt].base = rb; out[cnt].end = rt; cnt++; }
            }
        }
    }
    if (total_bytes) *total_bytes = total;
    if (above4g_bytes) *above4g_bytes = above;
    return cnt;
}

// 计算运行期堆/页池布局（★ 与 memlayout64.h 的四段边界一起读）
//   规则（顺序即断言）：
//     ① 内核映像上界 = __bss_end 的**物理**形式（VA - ML64_KERNEL_VA_BASE），向上取 2MB；
//     ② 堆底 = max(堆下限 112MB, ①) —— 内核长得再大也不会被自己的堆覆盖；
//     ③ 堆顶 = 堆底 + 48MB；页池 = [堆顶, E820 上界)。
//   为什么②要用 max 而不是"直接用 __bss_end"：堆下限之下是载荷暂存窗口(64..96MB)，
//   载荷是引导层写进去的、内核无权覆盖，所以堆绝不能落进那个窗口。
static void mem64_layout64(void) {
    const uint64_t bss_pa = (uint64_t)(uintptr_t)__bss_end - ML64_KERNEL_VA_BASE;
    // ★ 用字面量 0x200000 而不是 memlayout64.h 的 PAGE_SIZE_2M：mem_64.h 把 PAGE_SIZE_64/2M
    //   定义成宏，两个头一起进同一个编译单元会撞成"expected unqualified-id"（踩过）。
    uint64_t hb = align_up64(bss_pa, 0x200000ull);          // 2MiB 对齐（大页边界）
    if (hb < (uint64_t)ML64_HEAP_PHYS_FLOOR) hb = ML64_HEAP_PHYS_FLOOR;
    g_heap_base  = hb;
    g_heap_limit = hb + ML64_HEAP_BYTES;
    g_pool_base  = g_heap_limit;
}

void mem_init_64(const BootInfo* bi) {
    mem64_layout64();
    const uint64_t high = e820_mem_high64(bi);

    // ★ 布局断言（运行期）：四段不许有任何重叠。任一条不成立都是真事故，
    //   必须大声打出来 —— 静默继续跑只会变成"随机时刻的野写"。
    {
        const uint64_t bss_pa = (uint64_t)(uintptr_t)__bss_end - ML64_KERNEL_VA_BASE;
        const bool guard = (bss_pa <= (uint64_t)ML64_PAYLOAD_PHYS) &&
                           (g_heap_base >= (uint64_t)ML64_PAYLOAD_PHYS + ML64_PAYLOAD_MAX_BYTES) &&
                           (g_heap_base >= bss_pa) &&
                           (g_heap_limit > g_heap_base) &&
                           (g_pool_base == g_heap_limit);
        dbg64_line_begin64();
        dbg64_str("[MEM64] layout bss_end=");
        dbg64_hex64(bss_pa);
        dbg64_str(" payload=[");
        dbg64_hex64(ML64_PAYLOAD_PHYS);
        dbg64_str(",");
        dbg64_hex64((uint64_t)ML64_PAYLOAD_PHYS + ML64_PAYLOAD_MAX_BYTES);
        dbg64_str(") heap=[");
        dbg64_hex64(g_heap_base);
        dbg64_str(",");
        dbg64_hex64(g_heap_limit);
        dbg64_str(") pool=[");
        dbg64_hex64(g_pool_base);
        dbg64_str(",");
        dbg64_hex64(high);
        dbg64_str(guard ? ") guard=ok" : ") guard=FAIL");
        dbg64_nl();
        dbg64_line_end64();
    }

    if (high <= g_pool_base + MEM64_PAGE) {         // 内存小到没有可用页池
        g_pool_end = g_pool_base;
        g_page_total = 0;
        g_page_free = 0;
        dbg64_str("[MEM64] WARN pool empty (mem_high=");
        dbg64_hex64(high);
        dbg64_str(")");
        dbg64_nl();
        return;
    }

    g_pool_end = high;
    // ★★ 页池 = [pool_base, pool_end) 里**真正 usable** 的那些页。
    //   旧实现 `g_page_total = (g_pool_end - g_pool_base) / MEM64_PAGE` 把整段当连续 RAM，
    //   在 3GB 处有 PCI 空洞的机器上会把空洞页也算成"可用"——缺位图扩容时它恰好没跨到
    //   空洞，所以一直没出事；放开上限的同一版必须把它改成按段填充：
    //   先把整个跨度标成**已用**，再只把 E820 usable 段内的页清成空闲。
    const uint64_t span_pages = (g_pool_end - g_pool_base) / MEM64_PAGE;
    g_page_span  = span_pages;
    g_page_total = 0;
    for (uint64_t i = 0; i < MEM64_BITMAP_BYTES; i++) g_page_bitmap[i] = 0xFF;

    Mem64Range64 ranges[16];
    const uint32_t nr = e820_usable_ranges64(bi, ranges, 16, &g_ram_usable, &g_ram_above4g);
    if (nr == 0) {
        // E820 缺失（引导层没给内存图）：退回旧行为把整段当 RAM，但必须大声说清楚，
        // 因为这种机器上页池可能包含 MMIO 空洞 —— 宁可日志刺眼，也不要静默野写。
        dbg64_str("[MEM64] WARN E820 missing -> pool assumed contiguous");
        dbg64_nl();
        for (uint64_t i = 0; i < span_pages; i++) { bitmap_set64(i, false); g_page_total++; }
        g_ram_usable  = g_pool_end;
        g_ram_above4g = 0;
    } else {
        for (uint32_t r = 0; r < nr; r++) {
            uint64_t lo = ranges[r].base;
            uint64_t hi = ranges[r].end;
            if (lo < g_pool_base) lo = g_pool_base;
            if (hi > g_pool_end)  hi = g_pool_end;
            lo = (lo + MEM64_PAGE - 1ull) & ~(MEM64_PAGE - 1ull);   // 页对齐（向上取）
            hi = hi & ~(MEM64_PAGE - 1ull);                          // 页对齐（向下取）
            if (hi <= lo) continue;
            const uint64_t i0 = (lo - g_pool_base) / MEM64_PAGE;
            const uint64_t i1 = (hi - g_pool_base) / MEM64_PAGE;
            for (uint64_t i = i0; i < i1 && i < span_pages; i++) {
                if (bitmap_get64(i)) { bitmap_set64(i, false); g_page_total++; }
            }
        }
    }
    g_page_free  = g_page_total;   // bitmap 在 .bss，不占页池，所以可用页一开始全空闲
    g_pool_rover = 0;

    // 堆：[g_heap_base, g_heap_limit) 一次性交给空闲链表
    heap_self_init64();

    dbg64_str("[MEM64] pool=[");
    dbg64_hex64(g_pool_base);
    dbg64_str(",");
    dbg64_hex64(g_pool_end);
    dbg64_str(") pages=");
    dbg64_dec(g_page_total);
    dbg64_str("/");
    dbg64_dec(g_page_span);                 // 跨度（含 PCI 空洞等不可用页）
    dbg64_str(" free=");
    dbg64_dec(g_page_free);
    dbg64_str(" heap=[");
    dbg64_hex64(g_heap_base);
    dbg64_str(",");
    dbg64_hex64(g_heap_limit);
    dbg64_str(")");
    dbg64_nl();
    // ★ v0.11.0 用户明确要求"串口打印出总内存大小"：这里给三个口径，互相对得上账 ——
    //   ram total = E820 usable 总量（"这台机器到底有多少内存"，已验证与 dump_e820 一致）
    //   above4g   = 其中 4GB 以上的部分（可用，但内核没有直映，不进页池）
    //   pool      = 真正交给页分配器的容量（= 直映内的 usable 页）
    dbg64_str("[MEM64] ram total=");
    dbg64_dec(g_ram_usable / (1024ull * 1024ull));
    dbg64_str("MiB (=E820 usable, hole excluded)");
    dbg64_str("MiB above4g=");
    dbg64_dec(g_ram_above4g / (1024ull * 1024ull));
    dbg64_str("MiB pool=");
    dbg64_dec((g_page_total * MEM64_PAGE) / (1024ull * 1024ull));
    dbg64_str("MiB direct_limit=");
    dbg64_dec(MEM64_DIRECT_MAP_BYTES / (1024ull * 1024ull));
    dbg64_str("MiB");
    dbg64_nl();
}

void* page_alloc_64() {
    if (g_page_total == 0 || g_page_span == 0) return nullptr;
    const uint64_t _if = irq_save64();
    // ★ v0.11.0：扫描范围是**跨度**（g_page_span）而不是可用页数 —— 位图按地址索引，
    //   空洞页被标成占用，所以扫跨度才能覆盖到空洞之后的那部分可用页。
    for (uint64_t n = 0; n < g_page_span; n++) {
        const uint64_t idx = (g_pool_rover + n) % g_page_span;
        if (!bitmap_get64(idx)) {
            bitmap_set64(idx, true);
            g_page_free--;
            g_pool_rover = (idx + 1) % g_page_span;
            irq_restore64(_if);
            return (void*)(uintptr_t)(g_pool_base + idx * MEM64_PAGE);
        }
    }
    irq_restore64(_if);
    return nullptr;                                  // 页池耗尽
}

void page_free_64(void* p) {
    if (!p) return;
    const uint64_t addr = (uint64_t)(uintptr_t)p;
    if (addr < g_pool_base || addr >= g_pool_end) return;
    if (addr & (MEM64_PAGE - 1)) return;             // 未对齐的地址不是页首
    const uint64_t idx = (addr - g_pool_base) / MEM64_PAGE;
    const uint64_t _if = irq_save64();
    if (bitmap_get64(idx)) { bitmap_set64(idx, false); g_page_free++; }
    irq_restore64(_if);
}

// ★ v0.10.2（帧预算）：2 MiB 对齐 + 512 页连续的物理块。
//   为什么需要它：用户态 shm / 帧缓冲原来用 4 KiB 页映射，合成器一帧要跨几百个页
//   （实测 4 KiB = 2 µs、64 KiB = 2.1 ms、4 MiB 整屏 = 519~565 ms）。2 MiB 大页把一个
//   4 MiB 缓冲区压成 2 个页表项。硬件前提有两条：**物理地址 2 MiB 对齐**、**512 页连续**，
//   所以这里专门按对齐扫描位图，而不是拿 page_alloc_64 拼。
void* page_alloc_2mb_64() {
    const uint64_t PAGES = 0x200000ull / MEM64_PAGE;              // 512
    // ★ v0.11.0：跨度与可用页数都要够（空洞页在位图里是占用态，扫到会自然跳过）
    if (g_page_total < PAGES || g_page_span < PAGES) return nullptr;
    const uint64_t _if = irq_save64();
    const uint64_t pad_bytes = (0x200000ull - (g_pool_base & 0x1FFFFFull)) & 0x1FFFFFull;
    const uint64_t idx0 = pad_bytes / MEM64_PAGE;                  // 池内第一个 2 MiB 对齐页
    for (uint64_t idx = idx0; idx + PAGES <= g_page_span; idx += PAGES) {
        uint64_t k = 0;
        while (k < PAGES && !bitmap_get64(idx + k)) k++;
        if (k == PAGES) {
            for (uint64_t j = 0; j < PAGES; j++) bitmap_set64(idx + j, true);
            g_page_free -= PAGES;
            irq_restore64(_if);
            return (void*)(uintptr_t)(g_pool_base + idx * MEM64_PAGE);
        }
    }
    irq_restore64(_if);
    return nullptr;
}

void page_free_2mb_64(void* base) {
    if (!base) return;
    const uint64_t PAGES = 0x200000ull / MEM64_PAGE;
    const uint64_t addr = (uint64_t)(uintptr_t)base;
    if (addr & 0x1FFFFFull) return;                               // 不是 2 MiB 块首：不猜
    for (uint64_t j = 0; j < PAGES; j++)
        page_free_64((void*)(uintptr_t)(addr + j * MEM64_PAGE));
}

// ★ v0.11.0：取**地址最高**的一个空闲页。只给 mem_selftest 用。
//   它的意义不是"能分配"，而是"能分配出 2 GiB 以上的**真实**物理页"：
//   旧版页池被封顶在 2.16 GiB，池顶永远取不到 > 2 GiB 的页 ——
//   "分配成功"和"用上了物理内存的高半区"根本是两件事，别拿前者冒充后者。
static void* page_alloc_highest64() {
    if (g_page_total == 0 || g_page_span == 0) return nullptr;
    const uint64_t _if = irq_save64();
    for (uint64_t i = g_page_span; i-- > 0; ) {
        if (!bitmap_get64(i)) {
            bitmap_set64(i, true);
            g_page_free--;
            irq_restore64(_if);
            return (void*)(uintptr_t)(g_pool_base + i * MEM64_PAGE);
        }
    }
    irq_restore64(_if);
    return nullptr;
}


uint64_t page_count_total_64() { return g_page_total; }
uint64_t page_count_free_64()  { return g_page_free; }

// ==================== 内存归属记账 ====================
static int      g_mem_owner = MEM_OWNER_KERNEL_64;
static uint64_t g_owner_bytes64[MEM_OWNER_COUNT_64];

void mem_owner_set_64(int owner) {
    g_mem_owner = (owner >= 0 && owner < MEM_OWNER_COUNT_64) ? owner : MEM_OWNER_KERNEL_64;
}
int mem_owner_get_64() { return g_mem_owner; }
uint64_t mem_owner_bytes_64(int owner) {
    return (owner >= 0 && owner < MEM_OWNER_COUNT_64) ? g_owner_bytes64[owner] : 0;
}
const char* mem_owner_name_64(int owner) {
    static const char* names[MEM_OWNER_COUNT_64] = {
        "kernel", "gui", "terminal", "calc", "mines", "tmgr", "settings", "apps"
    };
    return (owner >= 0 && owner < MEM_OWNER_COUNT_64) ? names[owner] : "?";
}

// ==================== 内核堆 ====================
// 16 字节对齐的空闲/占用块头。magic 用来抓"野指针 kfree / 二次释放"，
// 32 位版没有这个字段，坏指针会静默把块头写烂，故障点离病因很远。
#define BLK_MAGIC_64   0x564D543634424C4Bull     // "VMT64BLK"
#define BLK_FREE_64    0x424C363446524545ull     // "FREE64BL" 空闲链表成员（与已分配块区分）
#define BLK_HEAD       32u                       // sizeof(Blk64)，保持 16 字节对齐

struct Blk64 {
    uint64_t size;      // 含块头
    uint64_t owner;
    uint64_t magic;
    Blk64*   next;      // 仅空闲块使用
};

static Blk64*   g_heap_head  = nullptr;
static uint64_t g_heap_used  = 0;
static uint64_t g_heap_total = 0;
static bool     g_heap_ready = false;
static uint32_t g_heap_alloc_count = 0;
static uint32_t g_heap_free_count  = 0;
static uint32_t g_heap_walk_steps  = 0;
// 自检期间静音 + 拒绝计数：
//   内存自检的第 7 项**故意**喂坏指针来验证守卫生效，那不是故障，不该在日志里
//   冒充错误（否则任何"日志里不许出现 kfree: bad"的验收都会假失败）。
//   静音的同时把拒绝次数记下来，反而把"守卫到底有没有拦"变成一条硬断言。
static bool     g_kfree_quiet    = false;
static uint32_t g_kfree_rejected = 0;

static inline uint64_t align16_64(uint64_t v) { return (v + 15ull) & ~15ull; }

void heap_self_init64() {                            // 由 mem_init_64 调用
    g_heap_head = (Blk64*)(uintptr_t)g_heap_base;
    g_heap_head->size   = g_heap_limit - g_heap_base;
    g_heap_head->owner  = MEM_OWNER_KERNEL_64;
    g_heap_head->magic  = BLK_FREE_64;       // 空闲链表成员：kfree 会拒绝释放它（防自环）
    g_heap_head->next   = nullptr;
    g_heap_total = g_heap_head->size;
    g_heap_used  = 0;
    g_heap_ready = true;
}

void* kmalloc_64(uint64_t size) {
    if (!g_heap_ready) heap_self_init64();
    if (size == 0) size = 1;
    const uint64_t need = align16_64(size) + BLK_HEAD;      // 数据 16 字节对齐

    const uint64_t _if = irq_save64();
    Blk64* prev = nullptr;
    Blk64* cur  = g_heap_head;
    g_heap_walk_steps = 0;
    while (cur) {
        g_heap_walk_steps++;
        if (cur->size >= need) {
            // 剩余够放一个块头 + 至少 64 字节才分裂，否则整块给出去（防碎片化）
            if (cur->size >= need + BLK_HEAD + 64) {
                Blk64* rest = (Blk64*)((uint8_t*)cur + need);
                rest->size  = cur->size - need;
                rest->owner = MEM_OWNER_KERNEL_64;
                rest->magic = BLK_FREE_64;
                rest->next  = cur->next;
                cur->size   = need;
                cur->next   = rest;
            }
            if (prev) prev->next = cur->next;
            else      g_heap_head = cur->next;
            cur->next  = nullptr;
            cur->owner = (uint64_t)g_mem_owner;
            cur->magic = BLK_MAGIC_64;
            g_owner_bytes64[g_mem_owner] += cur->size;
            g_heap_used += cur->size;
            g_heap_alloc_count++;
            irq_restore64(_if);
            return (void*)((uint8_t*)cur + BLK_HEAD);
        }
        prev = cur;
        cur  = cur->next;
    }
    irq_restore64(_if);
    dbg64_str("[MEM64] OOM: kmalloc(");
    dbg64_dec(size);
    dbg64_str(") failed, used=");
    dbg64_dec(g_heap_used / 1024);
    dbg64_str("KB / ");
    dbg64_dec(g_heap_total / 1024);
    dbg64_str("KB");
    dbg64_nl();
    return nullptr;
}

void kfree_64(void* p) {
    if (!p) return;
    const uint64_t addr = (uint64_t)(uintptr_t)p;
    // 越界/未对齐/块头 magic 不符 -> 拒绝释放并如实报告，而不是把堆写烂
    if (addr < g_heap_base + BLK_HEAD || addr >= g_heap_limit) {
        g_kfree_rejected++;
        if (!g_kfree_quiet) {
            dbg64_str("[MEM64] kfree: bad pointer ");
            dbg64_hex64(addr);
            dbg64_nl();
        }
        return;
    }
    Blk64* blk = (Blk64*)(uintptr_t)(addr - BLK_HEAD);
    if (blk->magic != BLK_MAGIC_64) {
        g_kfree_rejected++;
        if (!g_kfree_quiet) {
            dbg64_str("[MEM64] kfree: bad/double free at ");
            dbg64_hex64(addr);
            dbg64_str(" magic=");
            dbg64_hex64(blk->magic);
            dbg64_nl();
        }
        return;
    }

    const uint64_t _if = irq_save64();
    const uint64_t ow = blk->owner;
    if (ow < MEM_OWNER_COUNT_64) {
        if (g_owner_bytes64[ow] >= blk->size) g_owner_bytes64[ow] -= blk->size;
        else                                  g_owner_bytes64[ow]  = 0;
    }
    g_heap_used = (g_heap_used >= blk->size) ? (g_heap_used - blk->size) : 0;
    g_heap_free_count++;
    blk->magic = BLK_FREE_64;       // 二次释放/释放空闲块都会被上面的 magic 检查挡住

    // 按地址插回空闲链表（保证升序，便于合并）
    Blk64** pp = &g_heap_head;
    while (*pp && (uint64_t)(uintptr_t)(*pp) < (uint64_t)(uintptr_t)blk) pp = &(*pp)->next;
    blk->next = *pp;
    *pp = blk;

    // 一次遍历完成相邻合并（与前后都试）
    Blk64* cur = g_heap_head;
    while (cur && cur->next) {
        if ((uint8_t*)cur + cur->size == (uint8_t*)cur->next) {
            cur->size += cur->next->size;
            cur->next  = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
    irq_restore64(_if);
}

void* krealloc_64(void* p, uint64_t newsize) {
    if (!p) return kmalloc_64(newsize);
    Blk64* blk = (Blk64*)(uintptr_t)((uint64_t)(uintptr_t)p - BLK_HEAD);
    if (blk->magic != BLK_MAGIC_64) return kmalloc_64(newsize);
    const uint64_t oldsize = blk->size - BLK_HEAD;
    if (newsize <= oldsize) return p;                   // 缩小：原地返回
    void* n = kmalloc_64(newsize);
    if (!n) return nullptr;
    const uint8_t* s = (const uint8_t*)p;
    uint8_t* d = (uint8_t*)n;
    for (uint64_t i = 0; i < oldsize; i++) d[i] = s[i];
    kfree_64(p);
    return n;
}

uint64_t heap_used_64()  { return g_heap_used; }
uint64_t heap_total_64() { return g_heap_total; }

void heap_audit_64(const char* tag) {
    uint64_t sum = 0;
    for (int i = 0; i < MEM_OWNER_COUNT_64; i++) sum += g_owner_bytes64[i];
    dbg64_str("[HEAP64] ");
    dbg64_str(tag ? tag : "-");
    dbg64_str(" used=");
    dbg64_dec(g_heap_used / 1024);
    dbg64_str("KB alloc=");
    dbg64_dec(sum / 1024);
    dbg64_str("KB total=");
    dbg64_dec(g_heap_total / 1024);
    dbg64_str("KB nalloc=");
    dbg64_dec(g_heap_alloc_count);
    dbg64_str(" nfree=");
    dbg64_dec(g_heap_free_count);
    dbg64_str(" walkmax=");
    dbg64_dec(g_heap_walk_steps);
    dbg64_nl();
    for (int i = 0; i < MEM_OWNER_COUNT_64; i++) {
        if (g_owner_bytes64[i] == 0) continue;
        dbg64_str("[HEAP64]   owner=");
        dbg64_str(mem_owner_name_64(i));
        dbg64_str(" ");
        dbg64_dec(g_owner_bytes64[i] / 1024);
        dbg64_str("KB");
        dbg64_nl();
    }
    if (sum + 4096 < g_heap_used) {
        dbg64_str("[HEAP64] WARN accounting gap ");
        dbg64_dec((g_heap_used - sum) / 1024);
        dbg64_str("KB - possible leak");
        dbg64_nl();
    }
}

void mem_info_64(uint64_t* total_kb, uint64_t* free_kb, uint64_t* heap_kb) {
    // ★ v0.11.0：total 从"池跨度"改成"可用页的字节和" —— 跨度里含 PCI 空洞，
    //   拿跨度当容量会在 4GB 机器上虚报约 1 GiB（空洞那一段）。
    if (total_kb) *total_kb = (g_page_total * MEM64_PAGE) / 1024;
    if (free_kb)  *free_kb  = (g_page_free * MEM64_PAGE) / 1024;
    if (heap_kb)  *heap_kb  = g_heap_total / 1024;
}

// ★★ v0.11.0：口径从"页池上界地址"改成 **E820 usable 总量**。
//   旧口径（`return g_pool_end;`）有两个隐含前提，4GB 机器上两条都不成立：
//     ① 页池没被位图封顶 —— 旧版封在 2.16 GiB，于是 4GB 被报成 2.16 GB（用户看到的就是
//        "分配了 4GB 只认 2GB"）；
//     ② 内存连续 —— 4GB 机器在 3GB 处有 1 GiB PCI 空洞，"上界地址"根本代表不了可用量。
//   新口径 = 1MB 以上 E820 usable 段求和，与启动日志 dump_e820() 的
//   `[LM64] E820 usable_above_1M_kb=` **完全同口径**，两边可以直接对账。
uint64_t mem_total_ram_64()  { return g_ram_usable; }
uint64_t mem_pool_base_64()  { return g_pool_base; }
uint64_t mem_pool_end_64()   { return g_pool_end; }
// 池跨度页数（含不可用页）/ 可用页数（page_count_total_64）：区分"地址跨度"与"真可用"
uint64_t mem_pool_span_pages_64() { return g_page_span; }

void mem_dump_64() {
    dbg64_str("[MEM64] free list:");
    dbg64_nl();
    uint32_t n = 0;
    for (Blk64* cur = g_heap_head; cur && n < 32; cur = cur->next, n++) {
        dbg64_str("  blk ");
        dbg64_hex64((uint64_t)(uintptr_t)cur);
        dbg64_str(" size=");
        dbg64_dec(cur->size / 1024);
        dbg64_str("KB");
        dbg64_nl();
    }
}

void mem_validate_64() {
    uint64_t bytes = 0;
    bool ordered = true;
    uint64_t prev_end = 0;
    for (Blk64* cur = g_heap_head; cur; cur = cur->next) {
        const uint64_t a = (uint64_t)(uintptr_t)cur;
        if (cur->magic != BLK_FREE_64) { ordered = false; break; }
        if (a < prev_end) { ordered = false; break; }       // 应与前一块不重叠且升序
        prev_end = a + cur->size;
        bytes += cur->size;
    }
    dbg64_str("[MEM64] validate: free=");
    dbg64_dec(bytes / 1024);
    dbg64_str("KB + used=");
    dbg64_dec(g_heap_used / 1024);
    dbg64_str("KB vs total=");
    dbg64_dec(g_heap_total / 1024);
    dbg64_str("KB ordered=");
    dbg64_str(ordered ? "yes" : "NO");
    dbg64_nl();
}

void mem_stats_64() {
    dbg64_str("[MEM64] heap used=");
    dbg64_dec(g_heap_used / 1024);
    dbg64_str("KB total=");
    dbg64_dec(g_heap_total / 1024);
    dbg64_str("KB pages free=");
    dbg64_dec(g_page_free);
    dbg64_str("/");
    dbg64_dec(g_page_total);
    dbg64_nl();
}

// ==================== 自检 ====================
// 给启动期自动验收用：分配/释放/重分配/对齐/归属记账/合并 都要对，最后账面必须归零。
int mem_selftest_64() {
    int fails = 0;
    heap_audit_64("before-selftest");
    const uint64_t used0  = g_heap_used;
    const uint64_t alloc0 = g_owner_bytes64[MEM_OWNER_KERNEL_64];

    // 1) 基本分配：16 字节对齐、可写
    mem_owner_set_64(MEM_OWNER_GUI_64);
    void* a = kmalloc_64(100);
    if (!a) { fails |= 1; }
    else {
        if (((uint64_t)(uintptr_t)a & 15) != 0) fails |= 2;         // 未 16 字节对齐
        for (int i = 0; i < 100; i++) ((uint8_t*)a)[i] = (uint8_t)i;
        for (int i = 0; i < 100; i++) if (((uint8_t*)a)[i] != (uint8_t)i) { fails |= 4; break; }
    }
    // 2) 归属记账应落在 gui 上
    if (mem_owner_bytes_64(MEM_OWNER_GUI_64) == 0) fails |= 8;

    // 3) 多个块互不重叠
    void* b = kmalloc_64(4096);
    void* c = kmalloc_64(64);
    if (!b || !c) fails |= 16;
    else {
        const uint64_t ab = (uint64_t)(uintptr_t)b, ac = (uint64_t)(uintptr_t)c;
        const uint64_t aa = (uint64_t)(uintptr_t)a;
        if (ab == ac || ab == aa || ac == aa) fails |= 32;
        ((uint8_t*)b)[0] = 0xAB; ((uint8_t*)b)[4095] = 0xCD;
        ((uint8_t*)c)[63]  = 0xEF;
        if (((uint8_t*)b)[0] != 0xAB || ((uint8_t*)b)[4095] != 0xCD) fails |= 64;
        // 写 c 的尾部不能破坏 b 的内容（越界/重叠检查）
        if (((uint8_t*)b)[4095] != 0xCD) fails |= 128;
    }

    // 4) krealloc：内容保留
    void* d = krealloc_64(a, 4096);
    if (!d) fails |= 256;
    else for (int i = 0; i < 100; i++) if (((uint8_t*)d)[i] != (uint8_t)i) { fails |= 512; break; }

    // 5) 全部释放后：账面必须回到起点
    kfree_64(d); kfree_64(b); kfree_64(c);
    mem_owner_set_64(MEM_OWNER_KERNEL_64);
    if (g_heap_used != used0) fails |= 1024;
    if (g_owner_bytes64[MEM_OWNER_KERNEL_64] + g_owner_bytes64[MEM_OWNER_GUI_64] != alloc0) {
        // GUI 归属已清零，这里检查两者之和回到起点（alloc0 是 kernel 的）
        if (g_owner_bytes64[MEM_OWNER_GUI_64] != 0) fails |= 2048;
    }

    // 6) 合并：反复分配/释放同一尺寸，堆不应越用越碎（步数不增长）
    uint32_t walk_first = 0;
    for (int round = 0; round < 2; round++) {
        void* arr[32];
        for (int i = 0; i < 32; i++) arr[i] = kmalloc_64(256);
        for (int i = 0; i < 32; i++) kfree_64(arr[i]);
        if (round == 0) walk_first = g_heap_walk_steps;
        else if (g_heap_walk_steps > walk_first * 4 + 8) fails |= 4096;   // 明显碎片化
    }

    // 7) 坏指针防护：不该崩、不该改账面
    uint64_t used_before = g_heap_used;
    const uint32_t rej0 = g_kfree_rejected;
    g_kfree_quiet = true;                            // 故意喂坏指针：静音，只数拒绝次数
    kfree_64(nullptr);                               // 空指针：静默返回（不算拒绝）
    kfree_64((void*)0x10ull);                        // 越界 -> 必须被拒
    kfree_64((void*)(g_heap_base + BLK_HEAD));       // 空闲链表成员 -> 必须被拒（曾在此自环挂死）
    g_kfree_quiet = false;
    if (g_kfree_rejected - rej0 != 2) fails |= 32768;   // 守卫必须恰好拦下 2 次
    if (g_heap_used > used_before + 64) fails |= 8192;


    // 8) ★ v0.11.0 内存扩容的核心证据：页池**最高地址**的页必须真实可读写，
    //    且在大内存机器上必须落在 2 GiB 以上。
    //    旧版页池被封顶在 2.16 GiB（64 KiB 位图），4GB 机器上"分配一个页"永远只拿到
    //    低 2 GiB 的页 —— 分配成功完全不能证明用满了物理内存。所以这里不查账面，查**地址**。
    //    分档判据（512MB 机器行为完全不变）：
    //      * 任何机器：池顶页读写往返一致、释放后账面归零；
    //      * ram_usable >= 3 GiB：池顶页物理地址必须 > 0x80000000（2 GiB）。
    {
        void* hp = page_alloc_highest64();
        if (!hp) {
            fails |= 65536;                                   // 池顶取不到页 = 页池记账坏了
        } else {
            const uint64_t pa = (uint64_t)(uintptr_t)hp;
            volatile uint64_t* w = (volatile uint64_t*)(uintptr_t)pa;
            const uint64_t pat = 0xA5A5C0DE12345678ull;
            w[0] = pat; w[511] = ~pat;
            const bool rw_ok = (w[0] == pat) && (w[511] == ~pat);
            page_free_64(hp);
            if (!rw_ok) fails |= 131072;                      // 高地址页读写不通
            const bool big = (g_ram_usable >= 3ull * 1024ull * 1024ull * 1024ull);
            if (big && pa <= 0x80000000ull) fails |= 262144;   // 大机器上池顶必须 > 2 GiB
            dbg64_str("[MEM64] selftest high_page pa=0x");
            dbg64_hex64(pa);
            dbg64_str(" rw=");
            dbg64_str(rw_ok ? "1" : "0");
            dbg64_str(" gt2g=");
            dbg64_str(pa > 0x80000000ull ? "1" : "0");
            dbg64_str(" req=");
            dbg64_str(big ? "1" : "0");
            dbg64_nl();
        }
    }

    dbg64_str("[MEM64] selftest ");
    dbg64_str(fails == 0 ? "PASS" : "FAIL");
    dbg64_str(" mask=");
    dbg64_dec((uint64_t)fails);
    dbg64_str(" heap_used=");
    dbg64_dec(g_heap_used / 1024);
    dbg64_str("KB pages_free=");
    dbg64_dec(g_page_free);
    dbg64_nl();
    heap_audit_64("after-selftest");
    return fails;
}

// ==================== 编译器辅助例程 ====================
// clang 在 -ffreestanding 下依然会为结构体赋值/大数组清零发出这些调用，
// 必须用 C 链接的**无后缀**名字提供，否则链接失败。
extern "C" {

void* memcpy(void* dst, const void* src, unsigned long n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    // 先按 8 字节搬，减小大块拷贝的开销（桌面重绘时会频繁用到）
    while (n >= 8) { *(uint64_t*)d = *(const uint64_t*)s; d += 8; s += 8; n -= 8; }
    while (n--) *d++ = *s++;
    return dst;
}

void* memmove(void* dst, const void* src, unsigned long n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void* memset(void* dst, int c, unsigned long n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t v = (uint8_t)c;
    const uint64_t v8 = 0x0101010101010101ull * v;
    while (n >= 8) { *(uint64_t*)d = v8; d += 8; n -= 8; }
    while (n--) *d++ = v;
    return dst;
}

int memcmp(const void* a, const void* b, unsigned long n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    while (n--) { if (*x != *y) return (int)*x - (int)*y; x++; y++; }
    return 0;
}

unsigned long strlen(const char* s) { const char* p = s; while (*p) p++; return (unsigned long)(p - s); }

char* strcpy(char* d, const char* s) { char* r = d; while ((*d++ = *s++)) {} return r; }

char* strncpy(char* d, const char* s, unsigned long n) {
    unsigned long i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char* a, const char* b, unsigned long n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

char* strcat(char* d, const char* s) {
    char* r = d;
    while (*d) d++;
    while ((*d++ = *s++)) {}
    return r;
}

char* strchr(const char* s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char*)s;
        if (!*s) return nullptr;
    }
}

char* strstr(const char* hay, const char* needle) {
    if (!*needle) return (char*)hay;
    for (; *hay; hay++) {
        const char* h = hay;
        const char* n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char*)hay;
    }
    return nullptr;
}

}  // extern "C"

// _64 后缀版本（mem_64.h 的声明）：直接转调上面的实现，避免两份逻辑
void* memcpy_64(void* d, const void* s, uint64_t n)  { return memcpy(d, s, (unsigned long)n); }
void* memmove_64(void* d, const void* s, uint64_t n) { return memmove(d, s, (unsigned long)n); }
void* memset_64(void* d, int c, uint64_t n)          { return memset(d, c, (unsigned long)n); }
int   memcmp_64(const void* a, const void* b, uint64_t n) { return memcmp(a, b, (unsigned long)n); }
uint64_t strlen_64(const char* s)                    { return (uint64_t)strlen(s); }
char* strcpy_64(char* d, const char* s)              { return strcpy(d, s); }
char* strncpy_64(char* d, const char* s, uint64_t n) { return strncpy(d, s, (unsigned long)n); }
int   strcmp_64(const char* a, const char* b)        { return strcmp(a, b); }
int   strncmp_64(const char* a, const char* b, uint64_t n) { return strncmp(a, b, (unsigned long)n); }
uint64_t virt_to_phys_64(uint64_t virt_addr)         { return virt_addr; }   // 恒等映射
uint64_t phys_to_virt_64(uint64_t phys_addr)         { return phys_addr; }

// ==================== C++ new/delete ====================
// 桌面栈里的窗口状态是 C++ 对象，没有这个就编不过。
void* operator new(unsigned long size)             { return kmalloc_64(size); }
void* operator new[](unsigned long size)           { return kmalloc_64(size); }
void  operator delete(void* p) noexcept            { kfree_64(p); }
void  operator delete[](void* p) noexcept          { kfree_64(p); }
void  operator delete(void* p, unsigned long) noexcept   { kfree_64(p); }
void  operator delete[](void* p, unsigned long) noexcept { kfree_64(p); }
