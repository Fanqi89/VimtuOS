// mem64.cpp - 64 位内存管理：物理页池 + 内核堆 + 归属记账 + 编译器辅助例程
//
// 为什么单独一个文件（而不是照抄 32 位 mem.cpp）：
//   64 位内核里同时存在三处**固定占用**，堆和页池必须一个都不碰：
//     0x0007C000                内核栈（entry64.asm: mov rsp, 0x7C000）
//     0x00100000..~0x02460000   内核映像 + .bss（含 fb 的 33MB 后备缓冲）
//     0x04000000..0x043F1200    安装载荷（仅"安装介质"内核用；系统内核不用）
//   32 位版把堆起点设为 ALIGN(_end)，把页 bitmap 也写在 _end 处，两者同址 ——
//   结果第一次 kmalloc 就把 FreeBlock 头写进了 bitmap，页分配器元数据被静默破坏。
//   64 位版直接把 bitmap 放进 .bss，从结构上消除这个缺陷：
//     * 堆   = [80MB, 128MB)  固定 48MB，落在载荷之上、页池之下，两个内核行为一致
//     * 页池 = [128MB, mem_high)  bitmap 在 .bss，不消耗任何页池地址
//
// 另一个必须放在这里的东西：**编译器辅助例程**。
//   64 位树里 memcpy/memset/memmove/memcmp 从来没被实现过（mem_64.h 只有 _64 后缀的
//   声明，且无实现）。现在能链接只是因为还没有代码触发编译器生成这些调用；一旦有
//   结构体赋值/数组清零，clang 就会发出外部引用，链接当场失败。所以在这里一并提供。

#include "mem_64.h"
#include "port.h"
#include "debug64.h"
#include "../bootinfo.h"


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
// ==================== 布局常量 ====================
#define MEM64_PAGE        4096ull
#define MEM64_HEAP_BASE   0x05000000ull      // 80MB
#define MEM64_HEAP_LIMIT  0x08000000ull      // 128MB
#define MEM64_POOL_BASE   0x08000000ull      // 128MB（= 堆上界，紧邻）

// bitmap 放 .bss：64KB 覆盖 64K*8 = 524288 页 = 2GB 页池上限。
// 512MB 的虚拟机实测只用 12KB，留足余量且永不与任何区域重叠。
#define MEM64_BITMAP_BYTES (64u * 1024u)
#define MEM64_POOL_MAX     (MEM64_POOL_BASE + (uint64_t)MEM64_BITMAP_BYTES * 8ull * MEM64_PAGE)

static uint8_t  g_page_bitmap[MEM64_BITMAP_BYTES];
static uint64_t g_pool_base   = MEM64_POOL_BASE;
static uint64_t g_pool_end    = 0;      // 由 E820 决定
static uint64_t g_page_total  = 0;
static uint64_t g_page_free   = 0;
static uint64_t g_pool_rover  = 0;      // 分配游标（避免每次都从 0 扫）

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
    if (highest > MEM64_POOL_MAX) highest = MEM64_POOL_MAX;
    return highest;
}

void mem_init_64(const BootInfo* bi) {
    g_pool_base = MEM64_POOL_BASE;
    const uint64_t high = e820_mem_high64(bi);

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

    g_pool_end  = high;
    g_page_total = (g_pool_end - g_pool_base) / MEM64_PAGE;
    if (g_page_total > (uint64_t)MEM64_BITMAP_BYTES * 8ull)
        g_page_total = (uint64_t)MEM64_BITMAP_BYTES * 8ull;

    for (uint64_t i = 0; i < MEM64_BITMAP_BYTES; i++) g_page_bitmap[i] = 0;
    g_page_free = g_page_total;   // bitmap 在 .bss，不占页池，所以全部页一开始都空闲
    g_pool_rover = 0;

    // 堆：固定 [80MB,128MB)，一次性交给空闲链表
    heap_self_init64();

    dbg64_str("[MEM64] pool=[");
    dbg64_hex64(g_pool_base);
    dbg64_str(",");
    dbg64_hex64(g_pool_end);
    dbg64_str(") pages=");
    dbg64_dec(g_page_total);
    dbg64_str(" free=");
    dbg64_dec(g_page_free);
    dbg64_str(" heap=[");
    dbg64_hex64(MEM64_HEAP_BASE);
    dbg64_str(",");
    dbg64_hex64(MEM64_HEAP_LIMIT);
    dbg64_str(")");
    dbg64_nl();
}

void* page_alloc_64() {
    if (g_page_total == 0) return nullptr;
    const uint64_t _if = irq_save64();
    for (uint64_t n = 0; n < g_page_total; n++) {
        const uint64_t idx = (g_pool_rover + n) % g_page_total;
        if (!bitmap_get64(idx)) {
            bitmap_set64(idx, true);
            g_page_free--;
            g_pool_rover = (idx + 1) % g_page_total;
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
    g_heap_head = (Blk64*)(uintptr_t)MEM64_HEAP_BASE;
    g_heap_head->size   = MEM64_HEAP_LIMIT - MEM64_HEAP_BASE;
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
    if (addr < MEM64_HEAP_BASE + BLK_HEAD || addr >= MEM64_HEAP_LIMIT) {
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
    if (total_kb) *total_kb = (g_pool_end > g_pool_base) ? (g_pool_end - g_pool_base) / 1024 : 0;
    if (free_kb)  *free_kb  = (g_page_free * MEM64_PAGE) / 1024;
    if (heap_kb)  *heap_kb  = g_heap_total / 1024;
}

uint64_t mem_total_ram_64()  { return g_pool_end; }     // E820 认定的可用内存上界
uint64_t mem_pool_base_64()  { return g_pool_base; }
uint64_t mem_pool_end_64()   { return g_pool_end; }

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
    kfree_64((void*)(MEM64_HEAP_BASE + BLK_HEAD));   // 空闲链表成员 -> 必须被拒（曾在此自环挂死）
    g_kfree_quiet = false;
    if (g_kfree_rejected - rej0 != 2) fails |= 32768;   // 守卫必须恰好拦下 2 次
    if (g_heap_used > used_before + 64) fails |= 8192;

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
