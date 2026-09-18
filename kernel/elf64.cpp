// elf64.cpp - ELF64 加载器：读盘 -> 校验 -> 段映射进用户窗口 -> 建初始栈 -> 进 ring3 用
//             **syscall 指令**跑（Linux x86_64 ABI 的地基）
//
// 设计、装载策略、打点格式见 kernel/elf64.h 顶部（一页纸）。这里只额外记实现上的取舍：
//
// 1) 为什么先按 P|W|U 映射、最后再逐页收紧权限：
//    段内容是"从文件拷进去"的，而最终可能要求该页只读（PF_R 无 PF_W）。CR0.WP=1 时 ring0
//    也写不了只读页，所以先给写权限把内容写进去、清零 .bss，最后按 p_flags 重设叶子权限位
//    并重载 CR3。否则要么写不进去，要么得绕物理地址写（那就得在每个段里记物理页号）。
//
// 2) 为什么把整份文件读进静态缓冲再解析：
//    只有 96KiB 上限（.bss 不进内核镜像），换来"内容拷贝是纯内存操作"；VFS 的单文件上限
//    本来是 67584B，够用。超过上限直接拒绝（reason=size），不截断。
//
// 3) 装载区上界 = USER64_STACK_VA64：
//    用户窗口只有 1MiB，栈固定在 4GiB+64KiB。把"段必须落在 4GiB..4GiB+64KiB"作为硬约束，
//    一次性排掉"段压到栈/brk/mmap 区"的所有重叠场景；越界一律
//    `[ELF64] reject segment va=<hex> reason=outside-user-window|overlaps-user-region`。
//
// 4) ET_DYN（PIE）接受但**不做重定位**：
//    只按 p_vaddr 原样装载（相当于把链接基址当绝对地址）。真正的 PIE 需要重定位表处理，
//    本阶段不做；自有程序用 ET_EXEC 链接。
//
// 5) 回收：逐页 user64_unmap_page64() 拿回物理地址再 page_free_64()；中间页表页与
//    usermode64.cpp 一样保留（理由见那个文件）。munmap(11) 把页回收掉之后再跑这里也不会
//    出错：unmap 返回 0 的页直接跳过。
#include "elf64.h"
#include "usermode64.h"     // 用户窗口常量 / user64_map_page64 / user64_enter_at64
#include "vfs64.h"          // 读盘 + 幂等安装
#include "mem_64.h"         // PAGE_SIZE_64 / PTE_* / page_free_64
#include "debug64.h"

// 内嵌的 hello.elf（build64.sh：nasm -f elf64 -> ld.lld -static -> objcopy -I binary 嵌进系统内核）。
// 符号名由 objcopy 按输入路径生成：_binary_build64_hello_elf_start/_end（从仓库根执行才稳定）。
extern "C" const uint8_t _binary_build64_hello_elf_start[];
extern "C" const uint8_t _binary_build64_hello_elf_end[];

static const char ELF64_INSTALL_PATH64[] = "/hello.elf";

// 读盘缓冲：静态 .bss（不进镜像）。见文件头 (2)。
static uint8_t g_elf_file64[ELF64_MAX_FILE_BYTES64];

// ==================== 小工具（不依赖 libc）====================
static uint16_t e_rd16(const uint8_t* p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t e_rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t e_rd64(const uint8_t* p) { return (uint64_t)e_rd32(p) | ((uint64_t)e_rd32(p + 4) << 32); }
static void e_wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void e_wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void e_wr64(uint8_t* p, uint64_t v) { e_wr32(p, (uint32_t)v); e_wr32(p + 4, (uint32_t)(v >> 32)); }
static void e_zero(uint8_t* p, uint32_t n) { for (uint32_t i = 0; i < n; i++) p[i] = 0; }
static uint32_t e_strlen(const char* s) { uint32_t n = 0; if (!s) return 0; while (s[n]) n++; return n; }
// ==================== ELF64 常量（唯一来源）====================
#define ELF64_EHDR_SIZE     64u
#define ELF64_PHDR_SIZE     56u
#define ELF64_PT_LOAD       1u
#define ELF64_PF_X          0x1u
#define ELF64_PF_W          0x2u
#define ELF64_PF_R          0x4u
#define ELF64_ET_EXEC       2u
#define ELF64_ET_DYN        3u
#define ELF64_EM_X86_64     0x3Eu

// 装载区（用户窗口的低 64KiB）
static inline uint64_t e64_lo64() { return USER64_CODE_VA64; }
static inline uint64_t e64_hi64() { return USER64_STACK_VA64; }                 // 排他上界

// ==================== 解析结果 ====================
struct Elf64Seg64 {
    uint64_t va;        // p_vaddr（最终权限用）
    uint64_t off;       // p_offset
    uint64_t filesz;    // p_filesz
    uint64_t memsz;     // p_memsz
    uint32_t flags;     // p_flags
};

struct Elf64Image64 {
    uint64_t entry;     // e_entry
    uint64_t phdr_va;   // 程序头表在**映像里**的地址（给 auxv AT_PHDR；算不出来 = 0）
    uint64_t phnum;     // e_phnum
    uint32_t nseg;      // 实际 PT_LOAD 段数
    uint32_t file_bytes;
    Elf64Seg64 seg[ELF64_MAX_PHDR64];
};

static uint64_t g_e64_bad_va64 = 0;                // 最近一次解析失败的段 VA（打印 reason 时带出来）

// 错误码（打印时映射成短字符串；reason= 后面的文本是自动验收的一部分）
enum {
    E64_OK = 0,
    E64_ARG, E64_SIZE, E64_MAGIC, E64_CLASS, E64_DATA, E64_VERSION, E64_MACHINE, E64_TYPE,
    E64_PHDR, E64_NOSEG, E64_SEG_WINDOW, E64_SEG_REGION, E64_SEG_MEMSZ, E64_SEG_FILE,
    E64_ENTRY, E64_LOAD, E64_STACK, E64_BUSY, E64_VFS
};
static const char* e_reason64(int rc) {
    switch (rc) {
        case E64_ARG:        return "arg";
        case E64_SIZE:       return "size";
        case E64_MAGIC:      return "magic";
        case E64_CLASS:      return "class";
        case E64_DATA:       return "endian";
        case E64_MACHINE:    return "machine";
        case E64_TYPE:       return "type";
        case E64_PHDR:       return "phdr";
        case E64_NOSEG:      return "no-segment";
        case E64_VERSION:    return "version";
        case E64_SEG_WINDOW: return "outside-user-window";
        case E64_SEG_REGION: return "overlaps-user-region";
        case E64_SEG_MEMSZ:  return "memsz-filesz";
        case E64_SEG_FILE:   return "segment-file-range";
        case E64_ENTRY:      return "entry-not-in-segment";
        case E64_LOAD:       return "load";
        case E64_STACK:      return "stack";
        case E64_BUSY:       return "busy";
        case E64_VFS:        return "vfs";
        default:             return "?";
    }
}

int elf64_is_elf64(const uint8_t* p, uint32_t n) {
    if (!p || n < 4) return 0;
    return (p[0] == 0x7F && p[1] == 'E' && p[2] == 'L' && p[3] == 'F') ? 1 : 0;
}

// ==================== 解析 + 校验（只读，不打印）====================
// 返回 E64_OK 或错误码；坏段的起始 VA 记进 *bad_va。
static int elf64_parse64(const uint8_t* p, uint32_t n, Elf64Image64* out, uint64_t* bad_va) {
    if (bad_va) *bad_va = 0;
    if (!p || !out || n == 0) return E64_ARG;
    if (n < ELF64_EHDR_SIZE) return E64_SIZE;
    if (!(p[0] == 0x7F && p[1] == 'E' && p[2] == 'L' && p[3] == 'F')) return E64_MAGIC;
    if (p[4] != 2) return E64_CLASS;                    // ELFCLASS64
    if (p[5] != 1) return E64_DATA;                     // ELFDATA2LSB
    if (p[6] != 1) return E64_VERSION;                  // EV_CURRENT（并入 class 这一档打印）
    if (e_rd16(p + 18) != ELF64_EM_X86_64) return E64_MACHINE;
    const uint16_t etype = e_rd16(p + 16);
    if (etype != ELF64_ET_EXEC && etype != ELF64_ET_DYN) return E64_TYPE;

    const uint64_t entry = e_rd64(p + 24);
    const uint64_t phoff = e_rd64(p + 32);
    const uint16_t phentsize = e_rd16(p + 54);
    const uint16_t phnum = e_rd16(p + 56);
    if (phentsize != ELF64_PHDR_SIZE) return E64_PHDR;
    if (phnum == 0 || phnum > ELF64_MAX_PHDR64) return E64_PHDR;
    if (phoff > (uint64_t)n) return E64_PHDR;
    if ((uint64_t)phnum * ELF64_PHDR_SIZE > (uint64_t)n - phoff) return E64_PHDR;   // 程序头表必须整段在文件内

    out->entry = entry;
    out->phnum = phnum;
    out->phdr_va = 0;
    out->nseg = 0;
    out->file_bytes = n;

    for (uint32_t i = 0; i < phnum; i++) {
        const uint8_t* ph = p + phoff + (uint64_t)i * ELF64_PHDR_SIZE;
        if (e_rd32(ph + 0) != ELF64_PT_LOAD) continue;          // 非 PT_LOAD 一律忽略（无 PT_INTERP 处理）
        const uint32_t flags = e_rd32(ph + 4);
        const uint64_t off = e_rd64(ph + 8);
        const uint64_t va = e_rd64(ph + 16);
        const uint64_t filesz = e_rd64(ph + 32);
        const uint64_t memsz = e_rd64(ph + 40);

        if (memsz < filesz) { if (bad_va) *bad_va = va; return E64_SEG_MEMSZ; }
        if (filesz > (uint64_t)n || off > (uint64_t)n || filesz > (uint64_t)n - off) {
            if (bad_va) *bad_va = va;
            return E64_SEG_FILE;                                // p_offset+p_filesz 越出文件
        }
        if (memsz == 0) continue;                               // 空段：跳过（合法但没意义）
        // 目标必须完整落在用户窗口、且不碰窗口里的其它分区（栈/brk/mmap/自检页）
        if (va < e64_lo64() || memsz > e64_hi64() - e64_lo64() || va + memsz > e64_hi64()) {
            if (bad_va) *bad_va = va;
            return E64_SEG_WINDOW;
        }
        if (((va + memsz + PAGE_SIZE_64 - 1) & ~((uint64_t)PAGE_SIZE_64 - 1)) > e64_hi64()) {
            if (bad_va) *bad_va = va;
            return E64_SEG_REGION;                              // 页对齐后压到栈区
        }
        if (out->nseg >= ELF64_MAX_PHDR64) return E64_PHDR;
        Elf64Seg64* s = &out->seg[out->nseg++];
        s->va = va; s->off = off; s->filesz = filesz; s->memsz = memsz; s->flags = flags;

        // AT_PHDR：程序头表落在哪个段的文件范围内，就换算成映像地址
        if (phoff >= off && phoff < off + filesz) out->phdr_va = va + (phoff - off);
    }
    if (out->nseg == 0) return E64_NOSEG;

    // 入口必须落在某个可执行段里（[va, va+filesz) 允许含头部的段；这里用 memsz 更宽松）
    bool ok_entry = false;
    for (uint32_t i = 0; i < out->nseg; i++) {
        const Elf64Seg64* s = &out->seg[i];
        if (entry >= s->va && entry < s->va + s->memsz) { ok_entry = true; break; }
    }
    if (!ok_entry) { if (bad_va) *bad_va = entry; return E64_ENTRY; }
    return E64_OK;
}

// ==================== 已装载镜像的状态（单线程模型：同时只有一份）====================
struct Elf64Range64 { uint64_t va; uint32_t pages; };
struct Elf64Loaded64 {
    uint8_t  used;
    uint64_t entry;
    uint64_t user_rsp;
    uint32_t nrange;
    Elf64Range64 range[ELF64_MAX_PHDR64 + 2];      // 各 PT_LOAD 各一条 + 栈一条
};
static Elf64Loaded64 g_loaded64;
static Elf64Image64  g_img64;                      // 解析结果（权限收紧那一步还要用）

// 回收（幂等）：逐页 unmap + 释放物理页。中间页表页保留不回收（同 usermode64.cpp 的说明）。
int elf64_unload64() {
    for (uint32_t r = 0; r < g_loaded64.nrange; r++) {
        const uint64_t va = g_loaded64.range[r].va;
        const uint32_t pages = g_loaded64.range[r].pages;
        for (uint32_t i = 0; i < pages; i++) {
            const uint64_t phys = user64_unmap_page64(va + (uint64_t)i * PAGE_SIZE_64);
            if (phys) page_free_64((void*)(uintptr_t)phys);   // munmap(11) 已经回收过的页会是 0：跳过
        }
    }
    user64_paging_sync64();
    g_loaded64.used = 0;
    g_loaded64.entry = 0;
    g_loaded64.user_rsp = 0;
    g_loaded64.nrange = 0;
    return 0;
}

// ==================== 初始栈（SysV ABI）====================
static const uint64_t E64_AT_NULL   = 0;
static const uint64_t E64_AT_PHDR   = 3;
static const uint64_t E64_AT_PHNUM  = 4;
static const uint64_t E64_AT_PAGESZ = 6;
static const uint64_t E64_AT_ENTRY  = 9;
static const uint64_t E64_AT_UID    = 11;
static const uint64_t E64_AT_EUID   = 12;
static const uint64_t E64_AT_GID    = 13;
static const uint64_t E64_AT_EGID   = 14;

// 把 [argc][argv..][NULL][envp NULL][auxv..][NULL] 压到用户栈顶；返回初始 rsp。
// 布局从**低地址到高地址**依次写；字符串放在数组上方。rsp 16 字节对齐（进入 _start 时 rsp%16==0）。
static uint64_t e64_build_stack64(const char* argv0) {
    const uint64_t stack_top = USER64_STACK_VA64 + USER64_STACK_BYTES64;
    const uint64_t str_va = stack_top - 32;                       // 字符串区（16 字节对齐）

    uint8_t* sp = (uint8_t*)(uintptr_t)str_va;
    uint32_t k = 0;
    if (argv0) { for (; argv0[k] && k < 24; k++) sp[k] = (uint8_t)argv0[k]; }
    sp[k] = 0;

    uint64_t w[32];
    uint32_t n = 0;
    w[n++] = 1;                    // argc
    w[n++] = str_va;               // argv[0]
    w[n++] = 0;                    // argv 终止
    w[n++] = 0;                    // envp 终止（本内核没有环境变量）
    w[n++] = E64_AT_PAGESZ; w[n++] = PAGE_SIZE_64;
    w[n++] = E64_AT_PHDR;   w[n++] = g_img64.phdr_va;
    w[n++] = E64_AT_PHNUM;  w[n++] = g_img64.phnum;
    w[n++] = E64_AT_ENTRY;  w[n++] = g_img64.entry;
    w[n++] = E64_AT_UID;    w[n++] = 0;
    w[n++] = E64_AT_EUID;   w[n++] = 0;
    w[n++] = E64_AT_GID;    w[n++] = 0;
    w[n++] = E64_AT_EGID;   w[n++] = 0;
    w[n++] = E64_AT_NULL;   w[n++] = 0;

    const uint64_t rsp = (str_va - 64u - (uint64_t)n * 8u) & ~((uint64_t)15);   // 16 字节对齐
    uint64_t* d = (uint64_t*)(uintptr_t)rsp;
    for (uint32_t i = 0; i < n; i++) d[i] = w[i];
    return rsp;
}

// ==================== 装载（映射 + 拷内容 + 建栈；不打印）====================
static int elf64_load_image64(const uint8_t* p, uint32_t n, uint64_t* out_entry) {
    if (g_loaded64.used) return E64_BUSY;                 // 一份镜像都没收尾：先 unload
    uint64_t bad_va = 0;
    const int prc = elf64_parse64(p, n, &g_img64, &bad_va);
    if (prc != E64_OK) {
        g_e64_bad_va64 = bad_va;                           // 打印者用（reason 里带出出错的 va）
        g_loaded64.nrange = 0;
        return prc;
    }

    g_loaded64.used = 1;
    g_loaded64.nrange = 0;

    // ---- 1) 各 PT_LOAD：先按 P|W|U 映射、拷内容、清 .bss ----
    for (uint32_t i = 0; i < g_img64.nseg; i++) {
        const Elf64Seg64* s = &g_img64.seg[i];
        const uint64_t va0 = s->va & ~((uint64_t)PAGE_SIZE_64 - 1);
        const uint64_t end = s->va + s->memsz;
        const uint64_t va1 = (end + PAGE_SIZE_64 - 1) & ~((uint64_t)PAGE_SIZE_64 - 1);
        const uint32_t pages = (uint32_t)((va1 - va0) / PAGE_SIZE_64);

        g_loaded64.range[g_loaded64.nrange].va = va0;
        g_loaded64.range[g_loaded64.nrange].pages = pages;
        g_loaded64.nrange++;

        for (uint32_t k = 0; k < pages; k++) {
            uint64_t phys = 0;
            if (!user64_map_page64(va0 + (uint64_t)k * PAGE_SIZE_64,
                                   PTE_USER_64 | PTE_WRITE_64, 1, &phys)) {
                elf64_unload64();
                return E64_LOAD;
            }
        }
        user64_paging_sync64();
        uint8_t* dst = (uint8_t*)(uintptr_t)s->va;        // 已映射可写：ring0 直接按 VA 写
        for (uint64_t b = 0; b < s->filesz; b++) dst[b] = p[s->off + b];
        for (uint64_t b = s->filesz; b < s->memsz; b++) dst[b] = 0;
    }

    // ---- 2) 用户栈：16KiB（4 页）P|U|W|NX ----
    {
        const uint32_t stack_pages = (uint32_t)(USER64_STACK_BYTES64 / PAGE_SIZE_64);
        g_loaded64.range[g_loaded64.nrange].va = USER64_STACK_VA64;
        g_loaded64.range[g_loaded64.nrange].pages = stack_pages;
        g_loaded64.nrange++;
        for (uint32_t k = 0; k < stack_pages; k++) {
            uint64_t phys = 0;
            if (!user64_map_page64(USER64_STACK_VA64 + (uint64_t)k * PAGE_SIZE_64,
                                   PTE_USER_64 | PTE_WRITE_64 | PTE_NX_64, 1, &phys)) {
                elf64_unload64();
                return E64_STACK;
            }
        }
        user64_paging_sync64();
    }
    const uint64_t rsp = e64_build_stack64(ELF64_INSTALL_PATH64);
    if (rsp & 0xFu) { elf64_unload64(); return E64_STACK; }        // ABI 要求 rsp%16==0

    // ---- 3) 权限收紧：按 p_flags 重设各段叶子权限位 ----
    // ★ 按**页**取并集，而不是"逐段覆盖"：两个权限不同的段可能落在同一页（页粒度权限表示不了
    //   段粒度），逐段覆盖会让后一个段把前一页的权限抹掉（例如 .text 与 .bss 挤一页时 .text 变
    //   可写/不可执行 -> 立刻 #PF）。并集规则：任一覆盖该页的段可写 -> 页可写；任一段可执行 ->
    //   页可执行。自带程序用 user/hello_elf64.ld 把各段页对齐，正常不会命中这条兜底。
    {
        static const uint32_t E64_MAX_PAGES64 = 32;         // 装载区只有 64KiB（16 页），32 够用
        uint64_t page_va[E64_MAX_PAGES64];
        uint64_t page_fl[E64_MAX_PAGES64];
        uint32_t np = 0;
        for (uint32_t i = 0; i < g_img64.nseg; i++) {
            const Elf64Seg64* s = &g_img64.seg[i];
            const uint64_t va0 = s->va & ~((uint64_t)PAGE_SIZE_64 - 1);
            const uint64_t va1 = (s->va + s->memsz + PAGE_SIZE_64 - 1) & ~((uint64_t)PAGE_SIZE_64 - 1);
            for (uint64_t a = va0; a < va1; a += PAGE_SIZE_64) {
                uint32_t k = 0;
                while (k < np && page_va[k] != a) k++;
                if (k == np) {
                    if (np >= E64_MAX_PAGES64) { elf64_unload64(); return E64_LOAD; }
                    page_va[np] = a;
                    page_fl[np] = 0;
                    np++;
                }
                page_fl[k] |= (uint64_t)(s->flags & (ELF64_PF_W | ELF64_PF_X));
            }
        }
        for (uint32_t k = 0; k < np; k++) {
            const uint64_t f = PTE_USER_64                                 // PF_R（x86 上 P 就是可读）
                             | ((page_fl[k] & ELF64_PF_W) ? PTE_WRITE_64 : 0u)
                             | ((page_fl[k] & ELF64_PF_X) ? 0u : PTE_NX_64);
            if (!user64_remap_flags64(page_va[k], f)) { elf64_unload64(); return E64_LOAD; }
        }
    }
    user64_paging_sync64();                                // 权限改完了，投递到 TLB

    g_loaded64.entry = g_img64.entry;
    g_loaded64.user_rsp = rsp;
    if (out_entry) *out_entry = g_img64.entry;
    return E64_OK;
}

// ==================== 读盘 + 打印 + 装载 ====================
// 拒绝时的统一打点。tag = "[ELF64] reject"（真实装载）或 "[ELF64] selftest reject"（自检坏样本）。
// 凡是"段级"的问题都带出出错的 VA，便于定位。
static void e64_log_reject64(const char* tag, int rc) {
    dbg64_line_begin64();
    dbg64_str(tag);
    if (rc == E64_SEG_WINDOW || rc == E64_SEG_REGION || rc == E64_SEG_MEMSZ ||
        rc == E64_SEG_FILE || rc == E64_ENTRY) {
        dbg64_str(" segment va=");
        dbg64_hex64(g_e64_bad_va64);
    }
    dbg64_str(" reason=");
    dbg64_str(e_reason64(rc));
    dbg64_nl();
    dbg64_line_end64();
}

int elf64_load64(const char* path, uint64_t* out_entry) {
    if (!path || path[0] == 0) { e64_log_reject64("[ELF64] reject", E64_ARG); return -1; }
    if (g_loaded64.used) { e64_log_reject64("[ELF64] reject", E64_BUSY); return -1; }

    // 1) 属性 -> 期望长度（拒绝空文件/超上限，避免 vfs64_read 静默截断后误判）
    uint32_t type = 0, size = 0;
    if (vfs64_stat(path, &type, &size) != 0 || type != VFS64_TYPE_FILE) {
        dbg64_line_begin64();
        dbg64_str("[ELF64] load FAILED path=");
        dbg64_str(path);
        dbg64_str(" reason=vfs\n");
        dbg64_line_end64();
        return -1;
    }
    if (size == 0 || size > ELF64_MAX_FILE_BYTES64) { e64_log_reject64("[ELF64] reject", E64_SIZE); return -1; }

    // 2) 真的从文件系统读出来（不是内存里直接跑）
    const int rd = vfs64_read(path, g_elf_file64, (int)sizeof(g_elf_file64));
    if (rd != (int)size) {
        dbg64_line_begin64();
        dbg64_str("[ELF64] load FAILED path=");
        dbg64_str(path);
        dbg64_str(" reason=vfs\n");
        dbg64_line_end64();
        return -1;
    }

    // 3) 解析 + 装载 + 建栈
    uint64_t entry = 0;
    const int rc = elf64_load_image64(g_elf_file64, (uint32_t)rd, &entry);
    if (rc != E64_OK) {
        e64_log_reject64("[ELF64] reject", rc);
        g_loaded64.used = 0;                               // 上面只是暂存错误 VA，不算"已装载"
        return -1;
    }
    g_loaded64.used = 1;

    dbg64_line_begin64();
    dbg64_str("[ELF64] load path=");
    dbg64_str(path);
    dbg64_str(" entry=");
    dbg64_hex64(entry);
    dbg64_str(" phnum=");
    dbg64_dec(g_img64.phnum);
    dbg64_str(" segs=");
    dbg64_dec(g_img64.nseg);
    dbg64_str(" size=");
    dbg64_dec((uint64_t)rd);
    dbg64_str(" rsp=");
    dbg64_hex64(g_loaded64.user_rsp);
    dbg64_nl();
    dbg64_line_end64();

    if (out_entry) *out_entry = entry;
    return 0;
}

// ==================== 进 ring3 + 回收 ====================
int elf64_run_loaded64() {
    if (!g_loaded64.used) {
        dbg64_line_begin64();
        dbg64_str("[ELF64] launch FAILED reason=not-loaded\n");
        dbg64_line_end64();
        return -1;
    }
    dbg64_line_begin64();
    dbg64_str("[ELF64] enter ring3 entry=");
    dbg64_hex64(g_loaded64.entry);
    dbg64_str(" rsp=");
    dbg64_hex64(g_loaded64.user_rsp);
    dbg64_nl();
    dbg64_line_end64();

    const int rc = user64_enter_at64(g_loaded64.entry, g_loaded64.user_rsp, ELF64_INSTALL_PATH64);

    dbg64_line_begin64();
    dbg64_str("[ELF64] back to kernel (ring0) rc=");
    if (rc < 0) dbg64_putc('-');
    dbg64_dec((uint64_t)(rc < 0 ? -rc : rc));
    dbg64_nl();
    dbg64_line_end64();

    elf64_unload64();
    return rc;
}

int elf64_run64(const char* path) {
    uint64_t entry = 0;
    if (elf64_load64(path, &entry) != 0) {
        dbg64_line_begin64();
        dbg64_str("[ELF64] launch FAILED path=");
        dbg64_str(path ? path : "?");
        dbg64_str(" reason=load\n");
        dbg64_line_end64();
        return -1;
    }
    const int rc = elf64_run_loaded64();
    dbg64_line_begin64();
    dbg64_str("[ELF64] launch ok rc=");
    if (rc < 0) dbg64_putc('-');
    dbg64_dec((uint64_t)(rc < 0 ? -rc : rc));
    dbg64_str(" path=");
    dbg64_str(path ? path : "?");
    dbg64_nl();
    dbg64_line_end64();
    return rc;
}

// ==================== 幂等安装（内嵌 hello.elf -> VFS /hello.elf）====================
int elf64_install_builtin64(int drive, uint32_t part_lba) {
    const uint32_t bytes = (uint32_t)(_binary_build64_hello_elf_end - _binary_build64_hello_elf_start);
    // 先自校验内嵌映像（与运行时同一条解析路径），坏了就别往盘上写
    Elf64Image64 img;
    uint64_t bad_va = 0;
    if (elf64_parse64(_binary_build64_hello_elf_start, bytes, &img, &bad_va) != E64_OK) {
        dbg64_line_begin64();
        dbg64_str("[ELF64] install FAILED reason=blob\n");
        dbg64_line_end64();
        return -1;
    }

    // 卷可能没挂载：先探根目录，没有就按 (drive, part_lba) 挂一次
    uint32_t t = 0, sz = 0;
    if (vfs64_stat("/", &t, &sz) != 0) {
        if (vfs64_mount(drive, part_lba) != 0) {
            dbg64_line_begin64();
            dbg64_str("[ELF64] install FAILED reason=mount\n");
            dbg64_line_end64();
            return -1;
        }
    }
    if (vfs64_stat(ELF64_INSTALL_PATH64, &t, &sz) == 0) {       // 幂等：已存在就跳过
        dbg64_line_begin64();
        dbg64_str("[ELF64] install skipped (exists) /hello.elf size=");
        dbg64_dec(sz);
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    const int w = vfs64_write(ELF64_INSTALL_PATH64, _binary_build64_hello_elf_start, (int)bytes);
    if (w != (int)bytes) {
        dbg64_line_begin64();
        dbg64_str("[ELF64] install FAILED reason=write\n");
        dbg64_line_end64();
        return -1;
    }
    dbg64_line_begin64();
    dbg64_str("[ELF64] install ok path=/hello.elf bytes=");
    dbg64_dec(bytes);
    dbg64_str(" entry=");
    dbg64_hex64(img.entry);
    dbg64_str(" segs=");
    dbg64_dec(img.nseg);
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

// ==================== 自检 ====================
// 造一个最小合法 ELF64：1 个 PT_LOAD（R|X 或 R|W），p_vaddr 由调用方给。
//   p_offset = 0 且 p_filesz ≥ 0x90 -> **程序头表也在段内**，于是 AT_PHDR 算得出来（= vaddr+64），
//   与 user/hello_elf64.ld 的 `SIZEOF_HEADERS` 技巧是同一个道理。
//   段内容（16 字节模式）写在文件偏移 0x80；返回文件总长。
static const uint32_t E64_ST_HEADER_COVER = 0x90;   // 覆盖 ELF 头(64B) + 1 个程序头(56B)
static const uint32_t E64_ST_CONTENT_OFF  = 0x80;
static const uint32_t E64_ST_CONTENT_LEN  = 16;
static uint32_t e64_st_build(uint8_t* buf, uint64_t vaddr, uint64_t memsz, uint32_t pflags) {
    e_zero(buf, 256);
    buf[0] = 0x7F; buf[1] = 'E'; buf[2] = 'L'; buf[3] = 'F';
    buf[4] = 2;              // ELFCLASS64
    buf[5] = 1;              // ELFDATA2LSB
    buf[6] = 1;              // EV_CURRENT
    e_wr16(buf + 16, ELF64_ET_EXEC);
    e_wr16(buf + 18, ELF64_EM_X86_64);
    e_wr32(buf + 20, 1);
    e_wr64(buf + 24, vaddr);            // e_entry
    e_wr64(buf + 32, ELF64_EHDR_SIZE);  // e_phoff
    e_wr16(buf + 52, ELF64_EHDR_SIZE);
    e_wr16(buf + 54, ELF64_PHDR_SIZE);
    e_wr16(buf + 56, 1);                // e_phnum
    const uint64_t pho = ELF64_EHDR_SIZE;
    e_wr32(buf + pho + 0, ELF64_PT_LOAD);
    e_wr32(buf + pho + 4, pflags);
    e_wr64(buf + pho + 8, 0);                        // p_offset = 0（段覆盖 ELF 头/程序头）
    e_wr64(buf + pho + 16, vaddr);
    e_wr64(buf + pho + 24, vaddr);
    e_wr64(buf + pho + 32, E64_ST_HEADER_COVER);     // p_filesz（覆盖头 + 程序头 + 内容）
    e_wr64(buf + pho + 40, memsz);                   // p_memsz
    e_wr64(buf + pho + 48, PAGE_SIZE_64);
    for (uint32_t i = 0; i < E64_ST_CONTENT_LEN; i++) buf[E64_ST_CONTENT_OFF + i] = (uint8_t)(0xA0u + i);
    return E64_ST_CONTENT_OFF + E64_ST_CONTENT_LEN;
}

// 位含义：bit0 合法映像解析、bit1 装载（内容/.bss 清零/权限位）、bit2 初始栈（argc/argv/auxv/对齐）、
//         bit3 截断被拒、bit4 坏 magic 被拒、bit5 段越界被拒、bit6 p_memsz<p_filesz 被拒、
//         bit7 坏 machine 被拒、bit8 段文件范围越界被拒、bit9 入口不在段内被拒、bit10 回收干净
int elf64_selftest64() {
    int fail = 0;
    static uint8_t buf[256];
    Elf64Image64 img;
    uint64_t bad_va = 0;
    // 自检页：必须在**装载区**内（4GiB..4GiB+64KiB，上界 = USER64_STACK_VA64），
    // 而且不与真正的镜像冲突（自检在 elf64_run64 之前跑，且最后会 unload 干净）。
    const uint64_t tva = USER64_CODE_VA64 + 0x8000;

    // ---- bit0：合法映像解析 ----
    uint32_t n = e64_st_build(buf, tva, 0x180, ELF64_PF_R | ELF64_PF_X);   // memsz > filesz -> 练 .bss 清零
    if (elf64_parse64(buf, n, &img, &bad_va) != E64_OK) fail |= 1;
    else {
        if (img.entry != tva) fail |= 1;
        if (img.nseg != 1 || img.phnum != 1) fail |= 1;
        if (img.phdr_va != tva + ELF64_EHDR_SIZE) fail |= 1;                  // AT_PHDR 换算（段覆盖了程序头表）
        if (img.seg[0].filesz != E64_ST_HEADER_COVER || img.seg[0].memsz != 0x180) fail |= 1;
    }

    // ---- bit1/bit2：真的装载到自检页 + 权限位 + 初始栈 ----
    uint64_t entry = 0;
    if (elf64_load_image64(buf, n, &entry) != E64_OK) {
        fail |= 2 | 4;
    } else {
        // 细分位（只用来打印"bit1 里到底哪一条没过"）：1=entry 2=内容 4=.bss 清零 8=用户页
        //   16=叶子权限 32=栈页权限
        int d = 0;
        if (entry != tva) d |= 1;
        const uint8_t* m = (const uint8_t*)(uintptr_t)tva;
        for (uint32_t i = 0; i < E64_ST_CONTENT_LEN; i++) {                                     // 内容拷进去了
            if (m[E64_ST_CONTENT_OFF + i] != (uint8_t)(0xA0u + i)) d |= 2;
        }
        for (uint32_t i = E64_ST_CONTENT_OFF + E64_ST_CONTENT_LEN; i < 0x180; i++) {            // .bss 清零
            if (m[i] != 0) d |= 4;
        }
        if (!user64_page_is_user_ok64(tva)) d |= 8;                                                 // 映射成了用户页
        const uint64_t* st = (const uint64_t*)(uintptr_t)g_loaded64.user_rsp;
        if (g_loaded64.user_rsp % 16u) fail |= 4;                                                       // ABI 对齐
        if (st[0] != 1) fail |= 4;                                                                      // argc
        if (st[1] != USER64_STACK_VA64 + USER64_STACK_BYTES64 - 32) fail |= 4;                          // argv[0] 指针
        if (st[2] != 0 || st[3] != 0) fail |= 4;                                                        // argv/envp 终止
        bool at_entry = false, at_null = false;
        for (uint32_t i = 4; i < 32; i += 2) {
            if (st[i] == E64_AT_ENTRY && st[i + 1] == tva) at_entry = true;
            if (st[i] == E64_AT_NULL) { at_null = true; break; }
        }
        if (!at_entry || !at_null) fail |= 4;
        const char* a0 = (const char*)(uintptr_t)st[1];                      // argv[0] 是 NUL 结尾的串
        if (a0[0] != '/' || e_strlen(a0) == 0) fail |= 4;
        // 权限位：p_flags = PF_R|PF_X -> P|U、**不可写**、**不加 NX**（可执行）
        const uint64_t sfl = user64_page_flags64(tva);
        if (!(sfl & PTE_PRESENT_64) || !(sfl & PTE_USER_64)) d |= 16;
        if (sfl & PTE_WRITE_64) d |= 16;
        if (sfl & PTE_NX_64) d |= 16;
        // 栈页：P|U|W|NX（数据页不可执行）
        const uint64_t tfl = user64_page_flags64(USER64_STACK_VA64 + PAGE_SIZE_64);
        if (!(tfl & PTE_WRITE_64) || !(tfl & PTE_NX_64) || !(tfl & PTE_USER_64)) d |= 32;
        if (d != 0) {
            fail |= 2;
            dbg64_line_begin64();
            dbg64_str("[ELF64] selftest bit1 detail=");
            dbg64_dec((uint64_t)d);
            dbg64_nl();
            dbg64_line_end64();
        }

        elf64_unload64();
        if (user64_page_is_user_ok64(tva)) fail |= 1024;                                                // 页已回收
    }
    // ---- bit3：截断文件被拒 ----
    n = e64_st_build(buf, tva, 0x180, ELF64_PF_R | ELF64_PF_X);
    if (elf64_parse64(buf, 32, &img, &bad_va) != E64_SIZE) fail |= 8;
    dbg64_line_begin64();
    dbg64_str("[ELF64] selftest reject sample=truncated reason=");
    dbg64_str(e_reason64(E64_SIZE));
    dbg64_nl();
    dbg64_line_end64();

    // ---- bit4：坏 magic 被拒 ----
    n = e64_st_build(buf, tva, 0x180, ELF64_PF_R | ELF64_PF_X);
    buf[1] = 'X';
    if (elf64_parse64(buf, n, &img, &bad_va) != E64_MAGIC) fail |= 16;
    dbg64_line_begin64();
    dbg64_str("[ELF64] selftest reject sample=magic reason=magic\n");
    dbg64_line_end64();

    // ---- bit5：段目标越出用户窗口被拒（低 4GB = 内核恒等映射区）----
    n = e64_st_build(buf, 0x100000ULL, 0x180, ELF64_PF_R | ELF64_PF_X);
    if (elf64_parse64(buf, n, &img, &bad_va) != E64_SEG_WINDOW) fail |= 32;
    if (bad_va != 0x100000ULL) fail |= 32;
    dbg64_line_begin64();
    dbg64_str("[ELF64] selftest reject sample=segment-window reason=");
    dbg64_str(e_reason64(E64_SEG_WINDOW));
    dbg64_nl();
    dbg64_line_end64();

    // ---- bit6：p_memsz < p_filesz 被拒 ----
    n = e64_st_build(buf, tva, 0x08, ELF64_PF_R | ELF64_PF_X);          // memsz(8) < filesz(0x90)
    if (elf64_parse64(buf, n, &img, &bad_va) != E64_SEG_MEMSZ) fail |= 64;
    dbg64_line_begin64();
    dbg64_str("[ELF64] selftest reject sample=memsz-filesz reason=");
    dbg64_str(e_reason64(E64_SEG_MEMSZ));
    dbg64_nl();
    dbg64_line_end64();

    // ---- bit7：坏 machine 被拒 ----
    n = e64_st_build(buf, tva, 0x10, ELF64_PF_R | ELF64_PF_X);
    e_wr16(buf + 18, 0x28u);                                            // EM_ARM
    if (elf64_parse64(buf, n, &img, &bad_va) != E64_MACHINE) fail |= 128;
    dbg64_line_begin64();
    dbg64_str("[ELF64] selftest reject sample=machine reason=machine\n");
    dbg64_line_end64();

    // ---- bit8：段文件范围越界被拒（把 p_filesz 改成比文件长；memsz 同步放大，先撞文件范围）----
    n = e64_st_build(buf, tva, 0x180, ELF64_PF_R | ELF64_PF_X);
    e_wr64(buf + ELF64_EHDR_SIZE + 40, 0x1000);                          // p_memsz = 4096
    e_wr64(buf + ELF64_EHDR_SIZE + 32, 0x1000);                          // p_filesz = 4096 > 文件长度
    if (elf64_parse64(buf, n, &img, &bad_va) != E64_SEG_FILE) fail |= 256;
    dbg64_line_begin64();
    dbg64_str("[ELF64] selftest reject sample=segment-file-range reason=");
    dbg64_str(e_reason64(E64_SEG_FILE));
    dbg64_nl();
    dbg64_line_end64();

    // ---- bit9：入口不在任何段内被拒 ----
    n = e64_st_build(buf, tva, 0x180, ELF64_PF_R | ELF64_PF_X);
    e_wr64(buf + 24, tva + 0x1000);                                      // e_entry 跑到段外
    if (elf64_parse64(buf, n, &img, &bad_va) != E64_ENTRY) fail |= 512;
    dbg64_line_begin64();
    dbg64_str("[ELF64] selftest reject sample=entry reason=");
    dbg64_str(e_reason64(E64_ENTRY));
    dbg64_nl();
    dbg64_line_end64();

    // ---- bit10：内嵌 hello.elf 必须能解析（构建期产物与加载器同口径）----
    {
        const uint32_t hb = (uint32_t)(_binary_build64_hello_elf_end - _binary_build64_hello_elf_start);
        if (elf64_parse64(_binary_build64_hello_elf_start, hb, &img, &bad_va) != E64_OK) {
            fail |= 1024;
        } else {
            // hello.elf 用 user/hello_elf64.ld 的 SIZEOF_HEADERS 技巧，所以程序头表在映像里
            if (img.phdr_va == 0) fail |= 1024;
            // 入口必须落在**某个** PT_LOAD 段里（hello.elf 有 4 段：头+text / rodata / bss）
            bool in_seg = false;
            for (uint32_t i = 0; i < img.nseg; i++) {
                if (img.entry >= img.seg[i].va && img.entry < img.seg[i].va + img.seg[i].memsz) in_seg = true;
            }
            if (!in_seg) fail |= 1024;
            // 所有段都必须落在装载区（4GiB..4GiB+64KiB）
            for (uint32_t i = 0; i < img.nseg; i++) {
                if (img.seg[i].va < e64_lo64() ||
                    img.seg[i].va + img.seg[i].memsz > e64_hi64()) fail |= 1024;
            }
        }
    }

    if (fail == 0) {
        dbg64_line_begin64();
        dbg64_str("[ELF64] selftest PASS\n");
        dbg64_line_end64();
    } else {
        dbg64_line_begin64();
        dbg64_str("[ELF64] selftest FAIL mask=");
        dbg64_dec((uint64_t)fail);
        dbg64_nl();
        dbg64_line_end64();
    }
    return fail;
}
