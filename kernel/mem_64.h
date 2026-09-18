// mem_64.h - 64位内存管理（物理页 + 堆）
#pragma once
#include <stdint.h>
#include <stddef.h>

// 启动信息结构（保持与32位兼容）
struct BootInfo;

// 物理页管理（4KB）
void mem_init_64(const struct BootInfo* bi);
void* page_alloc_64();
void page_free_64(void* p);
uint64_t page_count_total_64();
uint64_t page_count_free_64();

// 内核堆管理
void* kmalloc_64(uint64_t size);
void kfree_64(void* p);
void* krealloc_64(void* p, uint64_t newsize);
uint64_t heap_used_64();
uint64_t heap_total_64();

// 内存信息
void mem_info_64(uint64_t* total_kb, uint64_t* free_kb, uint64_t* heap_kb);

// 内存归属统计（扩展到64位）
#define MEM_OWNER_KERNEL_64   0
#define MEM_OWNER_GUI_64      1
#define MEM_OWNER_TERMINAL_64 2
#define MEM_OWNER_CALC_64     3
#define MEM_OWNER_MINES_64    4
#define MEM_OWNER_TMGR_64     5
#define MEM_OWNER_SETTINGS_64 6
#define MEM_OWNER_APPS_64     7
#define MEM_OWNER_COUNT_64    8

void mem_owner_set_64(int owner);
int mem_owner_get_64();
uint64_t mem_owner_bytes_64(int owner);
const char* mem_owner_name_64(int owner);
void heap_audit_64(const char* tag);

// 页面分配器配置
#define PAGE_SIZE_64      4096
#define PAGE_MASK_64     ~(PAGE_SIZE_64 - 1)
#define PAGE_ALIGN_64(x)  (((x) + PAGE_SIZE_64 - 1) & PAGE_MASK_64)

// 页表项标志
#define PTE_PRESENT_64    0x001
#define PTE_WRITE_64      0x002
#define PTE_USER_64       0x004
#define PTE_PWT_64        0x008  // Write-Through
#define PTE_PCD_64        0x010  // Cache Disable
#define PTE_ACCESSED_64   0x020
#define PTE_DIRTY_64      0x040
#define PTE_GLOBAL_64     0x100  // Global Page (PGE)
#define PTE_PAT_64        0x080  // Page Attribute Table
#define PTE_NX_64         0x8000000000000000  // No-Execute (NX)

// 页表结构
struct PageTableEntry_64 {
    uint64_t value;
};

// 页目录表项
struct PageDirectoryEntry_64 {
    uint64_t value;
};

// 页目录指针表项
struct PageDirectoryPointerEntry_64 {
    uint64_t value;
};

// 页映射层4项
struct PageMapLayer4Entry_64 {
    uint64_t value;
};

// 页表级别
enum PageTableLevel {
    PML4_LEVEL = 3,
    PDPT_LEVEL = 2,
    PD_LEVEL = 1,
    PT_LEVEL = 0
};

// 地址转换函数
static inline uint64_t page_align_64(uint64_t addr) {
    return addr & PAGE_MASK_64;
}

static inline uint64_t page_offset_64(uint64_t addr) {
    return addr & (PAGE_SIZE_64 - 1);
}

static inline uint64_t pte_index_64(uint64_t addr, enum PageTableLevel level) {
    return (addr >> (12 + (3 - level) * 9)) & 0x1FF;
}

// 页表操作
void pt_set_entry_64(struct PageTableEntry_64* pte, uint64_t phys_addr, uint64_t flags);
void pt_clear_entry_64(struct PageTableEntry_64* pte);
int pt_is_present_64(const struct PageTableEntry_64* pte);
uint64_t pt_get_phys_addr_64(const struct PageTableEntry_64* pte);

// 页目录操作
void pd_set_entry_64(struct PageDirectoryEntry_64* pde, uint64_t phys_addr, uint64_t flags);
void pd_clear_entry_64(struct PageDirectoryEntry_64* pde);
int pd_is_present_64(const struct PageDirectoryEntry_64* pde);
uint64_t pd_get_phys_addr_64(const struct PageDirectoryEntry_64* pde);

// 页目录指针表操作
void pdpt_set_entry_64(struct PageDirectoryPointerEntry_64* pdpte, uint64_t phys_addr, uint64_t flags);
void pdpt_clear_entry_64(struct PageDirectoryPointerEntry_64* pdpte);
int pdpt_is_present_64(const struct PageDirectoryPointerEntry_64* pdpte);
uint64_t pdpt_get_phys_addr_64(const struct PageDirectoryPointerEntry_64* pdpte);

// 页映射层4操作
void pml4_set_entry_64(struct PageMapLayer4Entry_64* pml4e, uint64_t phys_addr, uint64_t flags);
void pml4_clear_entry_64(struct PageMapLayer4Entry_64* pml4e);
int pml4_is_present_64(const struct PageMapLayer4Entry_64* pml4e);
uint64_t pml4_get_phys_addr_64(const struct PageMapLayer4Entry_64* pml4e);

// 地址转换
uint64_t virt_to_phys_64(uint64_t virt_addr);
uint64_t phys_to_virt_64(uint64_t phys_addr);

// 内存映射
void map_page_64(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);
void unmap_page_64(uint64_t virt_addr);
int is_mapped_64(uint64_t virt_addr);

// 大页支持（2MB）
#define PAGE_SIZE_2MB_64  (2 * 1024 * 1024)
#define PTE_2MB_64        0x80  // 大页标志

void map_2mb_page_64(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);
void unmap_2mb_page_64(uint64_t virt_addr);

// 1GB大页支持（可选）
#define PAGE_SIZE_1GB_64  (1024 * 1024 * 1024)
#define PTE_1GB_64        0x100 // 1GB大页标志

void map_1gb_page_64(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);
void unmap_1gb_page_64(uint64_t virt_addr);

// 内存池
struct MemoryPool_64 {
    uint64_t base;
    uint64_t size;
    uint64_t used;
    uint64_t granularity;
};

void pool_init_64(struct MemoryPool_64* pool, uint64_t base, uint64_t size, uint64_t granularity);
void* pool_alloc_64(struct MemoryPool_64* pool, uint64_t size);
void pool_free_64(struct MemoryPool_64* pool, void* ptr);
int pool_contains_64(const struct MemoryPool_64* pool, void* ptr);

// 内存调试
void mem_dump_64();
void mem_validate_64();
void mem_stats_64();

// 内存屏障
#define mb_64()  __asm__ __volatile__("mfence" : : : "memory")
#define rmb_64() __asm__ __volatile__("lfence" : : : "memory")
#define wmb_64() __asm__ __volatile__("sfence" : : : "memory")

// 原子操作
static inline uint64_t atomic_read_64(volatile uint64_t* addr) {
    uint64_t val;
    __asm__ __volatile__("mov %0, %1" : "=r"(val) : "m"(*addr));
    return val;
}

static inline void atomic_write_64(volatile uint64_t* addr, uint64_t val) {
    __asm__ __volatile__("mov %0, %1" : "=r"(val) : "m"(*addr));
}

static inline uint64_t atomic_fetch_add_64(volatile uint64_t* addr, uint64_t val) {
    uint64_t result;
    __asm__ __volatile__("lock xaddq %0, %1" : "=r"(result), "=m"(*addr) : "0"(val) : "memory");
    return result;
}

static inline uint64_t atomic_fetch_sub_64(volatile uint64_t* addr, uint64_t val) {
    return atomic_fetch_add_64(addr, -val);
}

// 缓冲区管理
struct Buffer_64 {
    uint64_t size;
    uint64_t capacity;
    char* data;
    int owner;
};

struct Buffer_64* buffer_create_64(uint64_t initial_size, int owner);
void buffer_destroy_64(struct Buffer_64* buffer);
int buffer_resize_64(struct Buffer_64* buffer, uint64_t new_size);
int buffer_append_64(struct Buffer_64* buffer, const char* data, uint64_t size);
void buffer_clear_64(struct Buffer_64* buffer);

// 内存映射文件
struct MappedFile_64 {
    uint64_t size;
    void* data;
    int fd;
    int owner;
};

struct MappedFile_64* map_file_64(const char* path, int owner);
void unmap_file_64(struct MappedFile_64* mf);

// 内存保护
#define PROT_READ_64   0x1
#define PROT_WRITE_64  0x2
#define PROT_EXEC_64   0x4

int mprotect_64(void* addr, uint64_t len, int prot);
int munprotect_64(void* addr, uint64_t len);

// 内存复制
void* memcpy_64(void* dest, const void* src, uint64_t n);
void* memmove_64(void* dest, const void* src, uint64_t n);
void* memset_64(void* s, int c, uint64_t n);
int memcmp_64(const void* s1, const void* s2, uint64_t n);
uint64_t strlen_64(const char* s);
char* strcpy_64(char* dest, const char* src);
char* strncpy_64(char* dest, const char* src, uint64_t n);
int strcmp_64(const char* s1, const char* s2);
int strncmp_64(const char* s1, const char* s2, uint64_t n);
// ==================== 64 位实现补充声明（kernel/mem64.cpp）====================
// 布局（见 mem64.cpp 顶部说明）：
//   内核堆   = [80MB, 128MB)   固定 48MB，避开内核 .bss（到 ~36.4MB）与载荷（64MB）
//   物理页池 = [128MB, mem_high)  bitmap 在 .bss，不占页池
void heap_self_init64();                    // 由 mem_init_64 内部调用；也可单独重置堆

// 真实内存规模（系统监视器/任务管理器显示用，与 mem_info_64 的"页池"语义区分开）
uint64_t mem_total_ram_64();                // E820 认定的可用内存上界
uint64_t mem_pool_base_64();
uint64_t mem_pool_end_64();

// 启动期自检：分配/释放/重分配/对齐/归属记账/合并/坏指针防护。返回 0 = 全通过
// （返回的位图掩码指出失败项，便于定位；启动日志打印 "[MEM64] selftest PASS"）
int mem_selftest_64();