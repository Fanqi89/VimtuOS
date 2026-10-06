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
//    主程序装载区固定在 4GiB..4GiB+64KiB，栈固定在 4GiB+64KiB。把"段必须落在
//    4GiB..4GiB+64KiB"作为硬约束，一次性排掉"段压到栈/brk/mmap 区"的所有重叠场景；
//    越界一律 `[ELF64] reject segment va=<hex> reason=outside-user-window|overlaps-user-region`。
//    ★ A4-2b-2 起用户窗口是 16 MiB（usermode64.h），但**主程序装载区仍是这 64 KiB**：
//    窗口放大只抬高 mmap 区（4GiB+0x90000 往上）的上界，装载区/栈语义一个字没变。
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
#include "x86_64.h"         // ★ A3：g_ticks64（AT_RANDOM 的随机种子）
#include "debug64.h"

// ★ 本批（演示程序搬进系统卷）：hello.elf 的字节**不在内核里** ——
//   交付 = 系统卷里的 /hello.elf（tools/demo_pack_win.py 构建期写入 + 逐字节回读自检）；
//   启动期的"幂等装卷"只在卷里没有时从构建期"原始区"取字节（空夹具盘的情形，见 kernel/demo64.h）。
#include "demo64.h"

static const char ELF64_INSTALL_PATH64[] = "/hello.elf";

// 读盘缓冲：见本文件 "★ 本批：按段/按块读盘" 段（g_e64_hdr64 / g_e64_ph64 / g_e64_chunk64）。

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
#define ELF64_PT_DYNAMIC    2u
#define ELF64_PT_INTERP     3u
#define ELF64_PF_X          0x1u
#define ELF64_PF_W          0x2u
#define ELF64_PF_R          0x4u
#define ELF64_ET_EXEC       2u
#define ELF64_ET_DYN        3u
#define ELF64_EM_X86_64     0x3Eu
// ★ A3 下半：PT_INTERP 的解释器路径缓冲（Linux 的 realpath 上限远大于此，演示/验收够用）
#define ELF64_INTERP_PATH_MAX64 96u

// ==================== ★ 本批：按段/按块读盘（不再整份文件进内核缓冲）====================
// 老实现的两条硬上限（都实测撞过）：
//   * 主程序 96 KiB 读盘缓冲（ELF64_MAX_FILE_BYTES64）-> 425 KB 的静态 musl 程序 reason=size；
//   * 解释器同一条缓冲 -> musl 的 ld-musl-x86_64.so.1(873,416 B)/glibc 的 ld-linux(215,000 B)
//     reason=interp-size。
// 本批改成"按段读"：ELF 头(64B) + 程序头表(<=896B) 进 .bss 小缓冲；每个 PT_LOAD 的 p_filesz
// 字节按 ELF64_READ_CHUNK64 块**从文件偏移**直接读进已经映射好的映像页。于是
//   * 可执行文件/解释器的大小上限 = **文件系统**上限（VFS64_MAX_FILE_BYTES = 8 MiB），
//     不再是 96 KiB；实测 ≥4 MiB 的映像与 ≥2 MiB 的解释器都能装（见 [ELF64] read 打点）。
//   * .bss 里只剩 1 个 16 KiB 分块缓冲 + 元数据缓冲（比原来的 2×96 KiB 还小）。
#define ELF64_READ_CHUNK64  16384u                                    // 单次读盘/拷贝分块
#define ELF64_PAGE_MAX64    4096u                                     // 权限并集表容量（页）
#define ELF64_LEGACY_BYTES64 (USER64_STACK_VA64 - USER64_CODE_VA64)    // 老装载区 = 4GiB..4GiB+64KiB
// 按页收紧权限用的并集表的容量（8 MiB 映像 = 2048 页 + 解释器；取 4096 页 = 64 KiB .bss）

static uint8_t g_e64_hdr64[64];                                       // ELF 头
static uint8_t g_e64_ph64[ELF64_MAX_PHDR64 * 56u];                    // 程序头表（最多 896 B）
static uint8_t g_e64_chunk64[ELF64_READ_CHUNK64];                     // 分块读盘/拷贝

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
// ★ A3 下半：解释器（PT_INTERP）的信息 —— 只有主程序的解析才填
//   interp[] = PT_INTERP 的路径（可能不是绝对路径 -> 装载时按 /lib 搜索）
// ★ 本批：解析只把 PT_INTERP 的**文件偏移/长度**记下来（interp_off/interp_len），字符串由
//   e64_interp_take64 从**盘上**（或内存 blob）读 —— 因为文件不再整份进缓冲。
struct Elf64Image64 {
    uint64_t entry;     // e_entry（已加 base）
    uint64_t phdr_va;   // 程序头表在**映像里**的地址（给 auxv AT_PHDR；算不出来 = 0）
    uint64_t phnum;     // e_phnum
    uint64_t span;      // max(p_vaddr + p_memsz)（含 base；解释器算基址要用）
    uint64_t min_va;    // min(页对齐后的 p_vaddr)（含 base）
    uint64_t base;      // 本次装载偏移（load bias；ET_EXEC = 0，PIE = 选的基址 - min_vaddr）
    uint64_t interp_off, interp_len;   // PT_INTERP 路径在**文件**里的位置
    uint32_t nseg;      // 实际 PT_LOAD 段数
    uint32_t file_bytes;
    uint16_t etype;     // ET_EXEC / ET_DYN
    uint8_t  has_interp;
    char     interp[ELF64_INTERP_PATH_MAX64];
    Elf64Seg64 seg[ELF64_MAX_PHDR64];
};

static uint64_t g_e64_bad_va64 = 0;                // 最近一次解析失败的段 VA（打印 reason 时带出来）

// 错误码（打印时映射成短字符串；reason= 后面的文本是自动验收的一部分）
enum {
    E64_OK = 0,
    E64_ARG, E64_SIZE, E64_MAGIC, E64_CLASS, E64_DATA, E64_VERSION, E64_MACHINE, E64_TYPE,
    E64_PHDR, E64_NOSEG, E64_SEG_WINDOW, E64_SEG_REGION, E64_SEG_MEMSZ, E64_SEG_FILE,
    E64_ENTRY, E64_LOAD, E64_STACK, E64_BUSY, E64_VFS,
    // ★ 本批：按段读盘相关（E64_READ = 段内容读盘失败；与 E64_SEG_FILE 区分开：后者是文件长度判据）
    E64_READ,
    // ★ A3 下半：解释器（PT_INTERP）相关
    E64_INTERP_ARG, E64_INTERP_SIZE, E64_INTERP_VFS, E64_INTERP_BAD,
    E64_INTERP_WINDOW, E64_INTERP_LOAD
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
        case E64_INTERP_ARG:    return "interp-arg";
        case E64_INTERP_SIZE:   return "interp-size";
        case E64_INTERP_VFS:    return "interp-vfs";
        case E64_INTERP_BAD:    return "interp-bad";
        case E64_INTERP_WINDOW: return "interp-window";
        case E64_INTERP_LOAD:   return "interp-load";
        case E64_READ:          return "read";
        default:             return "?";
    }
}

int elf64_is_elf64(const uint8_t* p, uint32_t n) {
    if (!p || n < 4) return 0;
    return (p[0] == 0x7F && p[1] == 'E' && p[2] == 'L' && p[3] == 'F') ? 1 : 0;
}

// ==================== 解析 + 校验（只读，不打印）====================
// 返回 E64_OK 或错误码；坏段的起始 VA 记进 *bad_va。
// ★ A3 下半：参数化 — base 是**加在 p_vaddr 上的装载偏移**（主程序 = 0；解释器 = 内核挑的
//   基址），lo/hi 是本映像各段必须完整落进去的**映像地址区间**（含 base）。PT_LOAD 的
//   p_vaddr 先与 base 相加得到映像 VA，窗口检查按"p_vaddr 不越出 [lo-base, hi-base)"做，
//   这样既不会溢出、也把"段跨窗口边界"挡在解析期。
// ★ 本批：入参改成"元数据三段"—— ehdr（ELF 头）+ phtab（已读进来的程序头表）+ file_len（文件
//   真实长度，来自 stat；各段的 p_offset/p_filesz 边界都按它判）。调用方保证 phtab 至少有
//   e_phnum * 56 字节可用（blob 路径直接指向文件内容；读盘路径由 e64_read_meta64 读进 g_e64_ph64）。
static int e64_parse_core64(const uint8_t* ehdr, const uint8_t* phtab, uint64_t file_len,
                            uint64_t base, uint64_t lo, uint64_t hi,
                            Elf64Image64* out, uint64_t* bad_va) {
    if (bad_va) *bad_va = 0;
    if (!ehdr || !phtab || !out || file_len == 0) return E64_ARG;
    if (file_len < ELF64_EHDR_SIZE) return E64_SIZE;
    if (!(ehdr[0] == 0x7F && ehdr[1] == 'E' && ehdr[2] == 'L' && ehdr[3] == 'F')) return E64_MAGIC;
    if (ehdr[4] != 2) return E64_CLASS;                 // ELFCLASS64
    if (ehdr[5] != 1) return E64_DATA;                  // ELFDATA2LSB
    if (ehdr[6] != 1) return E64_VERSION;               // EV_CURRENT（并入 class 这一档打印）
    if (e_rd16(ehdr + 18) != ELF64_EM_X86_64) return E64_MACHINE;
    const uint16_t etype = e_rd16(ehdr + 16);
    if (etype != ELF64_ET_EXEC && etype != ELF64_ET_DYN) return E64_TYPE;
    // 区间自洽性（调用方给的）：lo < hi 且 base 不超过区间上界（否则下面 hi-base 会下溢）。
    //   ★ 修过的一个真缺陷：这里原来写的是 `hi - base < lo` —— 那是把 base 当 0 才成立的
    //   判据。解释器装载时 base 是**真实基址**（窗口顶部往下 span）、lo/hi 是**绝对**区间
    //   （mmap 区..窗口顶），于是 hi-base（只剩 0x5000 字节）< lo（0x100090000）恒真 ->
    //   解释器解析必被拒（打点 [ELF64] interp reject reason=bad）。主程序那条路 base=0，
    //   所以两者都只是"恰好"没暴露。
    if (hi <= lo || base > hi) return E64_SEG_WINDOW;           // 区间自洽性（调用方给的）
    const uint64_t entry = e_rd64(ehdr + 24) + base;
    const uint64_t phoff = e_rd64(ehdr + 32);
    const uint16_t phentsize = e_rd16(ehdr + 54);
    const uint16_t phnum = e_rd16(ehdr + 56);
    if (phentsize != ELF64_PHDR_SIZE) return E64_PHDR;
    if (phnum == 0 || phnum > ELF64_MAX_PHDR64) return E64_PHDR;
    if (phoff > file_len) return E64_PHDR;
    if ((uint64_t)phnum * ELF64_PHDR_SIZE > file_len - phoff) return E64_PHDR;   // 程序头表必须整段在文件内

    out->entry = entry;
    out->phnum = phnum;
    out->phdr_va = 0;
    out->nseg = 0;
    out->span = 0;
    out->min_va = 0;
    out->base = base;
    out->file_bytes = (uint32_t)file_len;
    out->etype = etype;
    out->has_interp = 0;
    out->interp_off = 0;
    out->interp_len = 0;
    out->interp[0] = 0;

    for (uint32_t i = 0; i < phnum; i++) {
        const uint8_t* ph = phtab + (uint64_t)i * ELF64_PHDR_SIZE;
        const uint32_t ptype = e_rd32(ph + 0);
        // ★ A3 下半 / ★ 本批：PT_INTERP —— 只把**文件里的位置**记下来（字符串由 e64_interp_take64
        //   从盘上读）；装载时按 /lib 搜索。has_interp=1 也是"解释器不许再有解释器"的判据。
        if (ptype == ELF64_PT_INTERP) {
            const uint64_t ioff = e_rd64(ph + 8);
            const uint64_t ilen = e_rd64(ph + 32);
            if (ioff >= file_len || ilen == 0 || ilen > ELF64_INTERP_PATH_MAX64) return E64_INTERP_BAD;
            if (ilen > file_len - ioff) return E64_INTERP_BAD;
            out->interp_off = ioff;
            out->interp_len = ilen;
            out->has_interp = 1;
            continue;
        }
        if (ptype != ELF64_PT_LOAD) continue;                   // 其它非 PT_LOAD 一律忽略
        const uint32_t flags = e_rd32(ph + 4);
        const uint64_t off = e_rd64(ph + 8);
        const uint64_t pv = e_rd64(ph + 16);
        const uint64_t filesz = e_rd64(ph + 32);
        const uint64_t memsz = e_rd64(ph + 40);
        if (pv > hi - base) { if (bad_va) *bad_va = base + pv; return E64_SEG_WINDOW; }
        const uint64_t va = base + pv;

        if (memsz < filesz) { if (bad_va) *bad_va = va; return E64_SEG_MEMSZ; }
        if (filesz > file_len || off > file_len || filesz > file_len - off) {
            if (bad_va) *bad_va = va;
            return E64_SEG_FILE;                                // p_offset+p_filesz 越出文件
        }
        if (memsz == 0) continue;                               // 空段：跳过（合法但没意义）
        // 目标必须完整落在本次给它的映像区间、且不碰区间里的其它分区（栈/brk/mmap/自检页）
        if (va < lo || memsz > hi - lo || va + memsz > hi) {
            if (bad_va) *bad_va = va;
            return E64_SEG_WINDOW;
        }
        if (((va + memsz + PAGE_SIZE_64 - 1) & ~((uint64_t)PAGE_SIZE_64 - 1)) > hi) {
            if (bad_va) *bad_va = va;
            return E64_SEG_REGION;                              // 页对齐后压到区间上界外
        }
        if (out->nseg >= ELF64_MAX_PHDR64) return E64_PHDR;
        Elf64Seg64* s = &out->seg[out->nseg++];
        s->va = va; s->off = off; s->filesz = filesz; s->memsz = memsz; s->flags = flags;
        if (va + memsz > out->span) out->span = va + memsz;
        const uint64_t va0 = va & ~((uint64_t)PAGE_SIZE_64 - 1);
        if (out->min_va == 0 || va0 < out->min_va) out->min_va = va0;

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

// ==================== 元数据读盘 + 装载布局（★ 本批新增）====================
// 读一段文件到缓冲（off/len 都是**文件偏移**；必须整段读到，少一个字节就算失败）。
static int e64_read_at64(const char* path, uint64_t off, uint8_t* buf, uint32_t len) {
    if (len == 0) return 0;
    uint32_t got = 0;
    if (vfs64_read_at64(path, (uint32_t)off, buf, len, &got) != 0) return -1;
    return (got == len) ? 0 : -1;
}

// 只读 ELF 头 + 程序头表（都在文件头部，最多 64+896 字节）-> 校验基本字段。
// 成功返回 E64_OK 并填 *out_phnum（程序头表已读进 g_e64_ph64[0..phnum*56)）。
static int e64_read_meta64(const char* path, uint64_t file_len, uint16_t* out_phnum) {
    if (file_len < ELF64_EHDR_SIZE) return E64_SIZE;
    if (e64_read_at64(path, 0, g_e64_hdr64, ELF64_EHDR_SIZE) != 0) return E64_VFS;
    if (!(g_e64_hdr64[0] == 0x7F && g_e64_hdr64[1] == 'E' &&
          g_e64_hdr64[2] == 'L' && g_e64_hdr64[3] == 'F')) return E64_MAGIC;
    const uint64_t phoff = e_rd64(g_e64_hdr64 + 32);
    const uint16_t phentsize = e_rd16(g_e64_hdr64 + 54);
    const uint16_t phnum = e_rd16(g_e64_hdr64 + 56);
    if (phentsize != ELF64_PHDR_SIZE) return E64_PHDR;
    if (phnum == 0 || phnum > ELF64_MAX_PHDR64) return E64_PHDR;
    if (phoff > file_len || (uint64_t)phnum * ELF64_PHDR_SIZE > file_len - phoff) return E64_PHDR;
    if (e64_read_at64(path, phoff, g_e64_ph64, (uint32_t)phnum * ELF64_PHDR_SIZE) != 0) return E64_VFS;
    *out_phnum = phnum;
    return E64_OK;
}

// PT_INTERP 的路径串：从盘上（blob == nullptr）或内存 blob 里取出来，写进 img->interp[]。
static int e64_interp_take64(const char* path, const uint8_t* blob, uint64_t blob_len, Elf64Image64* img) {
    if (!img->has_interp) { img->interp[0] = 0; return E64_OK; }
    if (img->interp_len == 0 || img->interp_len > ELF64_INTERP_PATH_MAX64) return E64_INTERP_BAD;
    uint8_t tmp[ELF64_INTERP_PATH_MAX64];
    if (blob) {
        if (img->interp_off + img->interp_len > blob_len) return E64_INTERP_BAD;
        for (uint64_t i = 0; i < img->interp_len; i++) tmp[i] = blob[img->interp_off + i];
    } else if (path) {
        if (e64_read_at64(path, img->interp_off, tmp, (uint32_t)img->interp_len) != 0) return E64_INTERP_BAD;
    } else {
        return E64_INTERP_BAD;
    }
    uint32_t k = 0;
    for (; k + 1u < (uint32_t)img->interp_len && tmp[k] != 0; k++) img->interp[k] = (char)tmp[k];
    img->interp[k] = 0;
    return (k == 0) ? E64_INTERP_BAD : E64_OK;
}

// blob（内存里的合法映像）解析：elf64_blob_ok64 / 自检 / 安装期自校验 / VAP64 嗅探都走它。
// base = 0、映像区间 = 老装载区（4GiB..4GiB+64KiB）—— 与历史行为一致（自检逐字段断言过）。
static int elf64_parse64(const uint8_t* p, uint32_t n, Elf64Image64* out, uint64_t* bad_va) {
    if (bad_va) *bad_va = 0;
    if (!p || !out || n == 0) return E64_ARG;
    if (n < ELF64_EHDR_SIZE) return E64_SIZE;
    const uint64_t phoff = e_rd64(p + 32);
    const uint16_t phentsize = e_rd16(p + 54);
    const uint16_t phnum = e_rd16(p + 56);
    if (phentsize != ELF64_PHDR_SIZE || phnum == 0 || phnum > ELF64_MAX_PHDR64) return E64_PHDR;
    if (phoff > (uint64_t)n || (uint64_t)phnum * ELF64_PHDR_SIZE > (uint64_t)n - phoff) return E64_PHDR;
    const int rc = e64_parse_core64(p, p + phoff, (uint64_t)n, 0, e64_lo64(), e64_hi64(), out, bad_va);
    if (rc != E64_OK) return rc;
    return e64_interp_take64(nullptr, p, (uint64_t)n, out);
}

// 量映像的地址布局（只读程序头表）：min（页对齐）/ span（页对齐）/ 类型。
//   span = align_up(max(p_vaddr+p_memsz)) - min
static int e64_probe_layout64(const uint16_t phnum, uint64_t* out_min, uint64_t* out_span) {
    uint64_t mn = ~0ULL, mx = 0;
    for (uint32_t i = 0; i < phnum; i++) {
        const uint8_t* ph = g_e64_ph64 + (uint64_t)i * ELF64_PHDR_SIZE;
        if (e_rd32(ph + 0) != ELF64_PT_LOAD) continue;
        const uint64_t pv = e_rd64(ph + 16);
        const uint64_t memsz = e_rd64(ph + 40);
        if (memsz == 0) continue;
        const uint64_t a = pv & ~((uint64_t)PAGE_SIZE_64 - 1);
        if (a < mn) mn = a;
        if (pv + memsz > mx) mx = pv + memsz;
    }
    if (mn == ~0ULL || mx <= mn) return E64_NOSEG;
    *out_min = mn;
    *out_span = ((mx - mn) + PAGE_SIZE_64 - 1) & ~((uint64_t)PAGE_SIZE_64 - 1);
    return E64_OK;
}

// 主程序装载布局（★ 本批核心）：按**程序头布局 + load bias** 定 (base, 映像区间 [lo, hi))。
//   mode: 0 = 老装载区（ET_EXEC 钉在 4GiB / PIE 小映像钉在 4GiB），1 = ET_EXEC 原样（大映像），
//         2 = PIE 大映像（首适配空洞）。返回 E64_OK 或错误码（*mode 里带着落点供打点）。
static int e64_pick_main_layout64(const uint16_t phnum, uint64_t* out_base, uint64_t* out_lo,
                                 uint64_t* out_hi, uint32_t* out_mode) {
    uint64_t minv = 0, span = 0;
    const int prc = e64_probe_layout64(phnum, &minv, &span);
    if (prc != E64_OK) return prc;
    const uint16_t etype = e_rd16(g_e64_hdr64 + 16);

    // ---- ET_EXEC（非 PIE）：bias = 0，按它自己的 p_vaddr 原样装载 ----
    //   老程序（段落在 4GiB..4GiB+64KiB）判据与历史**逐字节相同**；大映像（例如 4GiB+8MiB 的
    //   425 KB 静态程序）允许落在窗口里的"程序映像窗"（mmap 起点之上、DEV/SHM 窗之下）。
    if (etype == ELF64_ET_EXEC) {
        *out_base = 0;
        if (minv >= USER64_CODE_VA64 && minv + span <= USER64_STACK_VA64) {
            *out_lo = e64_lo64();
            *out_hi = e64_hi64();
            *out_mode = 0;
            return E64_OK;
        }
        *out_lo = USER64_CODE_VA64;                     // 含老装载区/固定分区：冲突由 region 判据挡
        *out_hi = USER64_LOADER_BIG_TOP_VA64;
        *out_mode = 1;
        return E64_OK;
    }

    // ---- ET_DYN（PIE）：小映像钉在老装载区（地址确定、无 ASLR）；大映像首适配 ----
    if (span <= ELF64_LEGACY_BYTES64) {
        *out_base = USER64_CODE_VA64 - minv;             // minv 通常是 0 -> base = 4GiB
        *out_lo = e64_lo64();
        *out_hi = e64_hi64();
        *out_mode = 0;
        return E64_OK;
    }
    const uint64_t at = user64_find_free64(USER64_MMAP_VA64, span);
    if (at == 0) {
        *out_base = 0;
        *out_lo = e64_lo64();
        *out_hi = e64_hi64();
        *out_mode = 0;
        return E64_SEG_WINDOW;
    }
    *out_base = at - minv;
    *out_lo = at;
    *out_hi = USER64_LOADER_BIG_TOP_VA64;
    *out_mode = 2;
    return E64_OK;
}

// 映像区间与**固定分区**冲突检查（栈/brk/邮箱/自检页在 [USER64_STACK_VA64, USER64_MMAP_VA64)；
// DEV/SHM 窗在 [USER64_LOADER_BIG_TOP_VA64, 窗口顶)）。老装载区（4GiB..+64KiB）不算冲突。
static int e64_region_conflict64(uint64_t va0, uint64_t va1) {
    if (va0 < USER64_MMAP_VA64 && va1 > USER64_STACK_VA64) return 1;
    if (va0 < USER64_CODE_VA64 + USER64_WINDOW_BYTES64 && va1 > USER64_LOADER_BIG_TOP_VA64) return 1;
    return 0;
}

// ==================== 已装载镜像的状态（单线程模型：同时只有一份）====================
struct Elf64Range64 { uint64_t va; uint32_t pages; };
struct Elf64Loaded64 {
    uint8_t  used;
    uint64_t entry;            // **执行入口**（有解释器 = 解释器入口；静态 = 主程序入口）
    uint64_t user_rsp;
    uint64_t interp_base;      // ★ A3 下半：AT_BASE（0 = 没有解释器）
    uint32_t nrange;
    // ★ A3 下半：各 PT_LOAD 一条 + 栈一条；解释器也是映像（≤16 段），所以上限要 ×2 + 1
    Elf64Range64 range[ELF64_MAX_PHDR64 * 2 + 2];
};
static Elf64Loaded64 g_loaded64;
static Elf64Image64  g_img64;                      // 主程序解析结果（权限收紧/auxv 还要用）
// ★ A3 下半：本次装载的 AT_BASE（0 = 静态）。初始栈与 auxv 打点都读它。
static uint64_t g_e64_at_base64 = 0;
// ★ 本批：解释器**不再需要独立的整份文件缓冲**（按段分块读，g_e64_chunk64 就是缓冲）。
// 权限收紧的按页并集表（★ 本批放大到 ELF64_PAGE_MAX64 页：8 MiB 映像 = 2048 页 + 解释器）
static uint64_t g_e64_page_va64[ELF64_PAGE_MAX64];
static uint64_t g_e64_page_fl64[ELF64_PAGE_MAX64];
// 最近一次主程序装载的布局证据（打点用）：mode / base / span
static uint32_t g_e64_lmode64 = 0;
static uint64_t g_e64_lbase64 = 0, g_e64_lspan64 = 0;

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
    g_loaded64.interp_base = 0;
    g_loaded64.nrange = 0;
    g_e64_at_base64 = 0;
    return 0;
}

// ==================== 初始栈（SysV ABI）====================
// ★ A3：auxv 的号必须与 Linux/elf.h 逐条一致（这里踩过一个真错：AT_PHNUM 曾是 4，
//   但 4 是 AT_PHENT —— musl 的 __init_libc/static_init_tls 会按 AT_PHENT 步进遍历
//   程序头表找 PT_TLS，号错了就是"按 6 字节步进读结构体"，必崩）。逐条清单：
//     AT_NULL(0) / AT_PHDR(3) / AT_PHENT(4) / AT_PHNUM(5) / AT_PAGESZ(6) / AT_BASE(7) /
//     AT_ENTRY(9) / AT_UID(11) / AT_EUID(12) / AT_GID(13) / AT_EGID(14) / AT_HWCAP(16) /
//     AT_SECURE(23) / AT_RANDOM(25)
//   为什么每条都要（musl 1.2.5 的启动路径，src/env/__libc_start_main.c + __init_tls.c +
//   __stack_chk_fail.c + malloc/mallocng/glue.h）：
//     AT_PHDR/AT_PHENT/AT_PHNUM：遍历程序头表（找 PT_TLS / PT_GNU_STACK / PT_DYNAMIC）
//     AT_PAGESZ：  libc.page_size（malloc/mallocng 的元数据分页全靠它，0 会直接崩）
//     AT_BASE：    静态程序 = 0（没有动态链接器）；如实给 0
//     AT_ENTRY：   与 e_entry 一致（部分启动代码用它；本内核给真值）
//     AT_UID/EUID/GID/EGID + AT_SECURE=0：__init_libc 只在"uid 与 euid 不同或 secure"时
//                  才去走 poll()+open("/dev/null") 的降权路径（本内核没有 poll -> 会真崩），
//                  所以这四条**必须**给"相等 + secure=0"（本内核没有权限模型，如实全 0）
//     AT_HWCAP：   本内核不暴露 CPUID hwcap -> 如实 0（musl 只存不用）
//     AT_RANDOM：  16 字节随机（__init_ssp 取 8 字节做 canary；mallocng 的 get_random_secret
//                  取 [8..16)）—— 必须是**可读的用户地址**，否则一取就 #PF。这里放在初始栈上。
static const uint64_t E64_AT_NULL   = 0;
static const uint64_t E64_AT_PHDR   = 3;
static const uint64_t E64_AT_PHENT  = 4;
static const uint64_t E64_AT_PHNUM  = 5;
static const uint64_t E64_AT_PAGESZ = 6;
static const uint64_t E64_AT_BASE   = 7;
static const uint64_t E64_AT_ENTRY  = 9;
static const uint64_t E64_AT_UID    = 11;
static const uint64_t E64_AT_EUID   = 12;
static const uint64_t E64_AT_GID    = 13;
static const uint64_t E64_AT_EGID   = 14;
static const uint64_t E64_AT_HWCAP  = 16;
static const uint64_t E64_AT_SECURE = 23;
static const uint64_t E64_AT_RANDOM = 25;
// AT_RANDOM 的地址（打印证据用；0 = 本次装载没给）
static uint64_t g_e64_rand_va64 = 0;

// 16 字节"够用的随机"：rdtsc + PIT tick + xorshift。**不是密码学安全随机**（与
// syscall64.cpp 的 getrandom(318) 同一口径，如实标注）：AT_RANDOM 只用来做
// stack canary / malloc 的 secret，本内核按"每次不同、分布还行"给。
static uint64_t g_e64_rng64 = 0x9E3779B97F4A7C15ULL;
static uint64_t e64_rand64() {
    uint32_t lo = 0, hi = 0;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t x = g_e64_rng64;
    x ^= ((uint64_t)hi << 32) | lo;
    x ^= g_ticks64 * 0xD6E8FEB86659FD93ULL;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    g_e64_rng64 = x;
    return x;
}
static void e64_fill_random64(uint64_t va) {
    uint8_t* p = (uint8_t*)(uintptr_t)va;                 // 栈页已映射（调用点保证）
    for (uint32_t i = 0; i < 16; i += 8) {
        const uint64_t r = e64_rand64();
        for (uint32_t k = 0; k < 8; k++) p[i + k] = (uint8_t)(r >> (8 * k));
    }
    g_e64_rand_va64 = va;
}

// 打印 auxv 证据（自动验收 tests/musl64_test.py grep `[ELF64] auxv`）。格式勿改：
//   [ELF64] auxv phdr=<hex> phent=<n> phnum=<n> base=<hex> entry=<hex> random=<hex> secure=<n> pagesz=<n>
static void e64_log_auxv64(uint64_t rsp) {
    dbg64_line_begin64();
    dbg64_str("[ELF64] auxv phdr=");
    dbg64_hex64(g_img64.phdr_va);
    dbg64_str(" phent=");
    dbg64_dec(ELF64_PHDR_SIZE);
    dbg64_str(" phnum=");
    dbg64_dec(g_img64.phnum);
    dbg64_str(" base=");
    dbg64_hex64(g_e64_at_base64);                          // ★ A3 下半：解释器基址（静态 = 0）
    dbg64_str(" entry=");
    dbg64_hex64(g_img64.entry);
    dbg64_str(" random=");
    dbg64_hex64(g_e64_rand_va64);
    dbg64_str(" secure=");
    dbg64_dec(0);
    dbg64_str(" pagesz=");
    dbg64_dec(PAGE_SIZE_64);
    dbg64_str(" rsp=");
    dbg64_hex64(rsp);
    dbg64_nl();
    dbg64_line_end64();
}
// 把 [argc][argv..][NULL][envp NULL][auxv..][NULL] 压到用户栈顶；返回初始 rsp。
// 布局从**低地址到高地址**依次写；字符串放在数组上方。rsp 16 字节对齐（进入 _start 时 rsp%16==0）。
static uint64_t e64_build_stack64(const char* argv0) {
    const uint64_t stack_top = USER64_STACK_VA64 + USER64_STACK_BYTES64;
    const uint64_t str_va = stack_top - 32;                       // 字符串区（16 字节对齐）

    uint8_t* sp = (uint8_t*)(uintptr_t)str_va;
    uint32_t k = 0;
    if (argv0) { for (; argv0[k] && k < 24; k++) sp[k] = (uint8_t)argv0[k]; }
    sp[k] = 0;

    uint64_t w[64];
    uint32_t n = 0;
    w[n++] = 1;                    // argc
    w[n++] = str_va;               // argv[0]
    w[n++] = 0;                    // argv 终止
    w[n++] = 0;                    // envp 终止（本内核没有环境变量）
    // ★ A3：AT_RANDOM 的 16 字节放在 [str_va-32, str_va-16) —— 这一段是**恒空**的：
    //   字符串在 [str_va, …)，而指针数组从 (str_va-64-n*8) 往下排，所以 str_va 下方
    //   紧挨着的 64 字节（n 多大都成立）不会被任何东西用掉。放这里的好处：它在 rsp
    //   **上方**，进程自己的函数调用（往 rsp 下面长）永远不会踩掉它。
    e64_fill_random64(str_va - 32);
    w[n++] = E64_AT_PAGESZ; w[n++] = PAGE_SIZE_64;
    w[n++] = E64_AT_PHDR;   w[n++] = g_img64.phdr_va;
    w[n++] = E64_AT_PHENT;  w[n++] = ELF64_PHDR_SIZE;      // ★ A3：musl 按它步进遍历程序头表
    w[n++] = E64_AT_PHNUM;  w[n++] = g_img64.phnum;
    w[n++] = E64_AT_BASE;   w[n++] = g_e64_at_base64;      // ★ A3 下半：解释器基址（静态 = 0）
    w[n++] = E64_AT_ENTRY;  w[n++] = g_img64.entry;
    w[n++] = E64_AT_HWCAP;  w[n++] = 0;                    // 本内核不暴露 hwcap（如实 0）
    w[n++] = E64_AT_UID;    w[n++] = 0;
    w[n++] = E64_AT_EUID;   w[n++] = 0;
    w[n++] = E64_AT_GID;    w[n++] = 0;
    w[n++] = E64_AT_EGID;   w[n++] = 0;
    w[n++] = E64_AT_RANDOM; w[n++] = str_va - 32;          // 上面那 16 字节随机
    w[n++] = E64_AT_SECURE; w[n++] = 0;                    // 没有权限模型 -> 非 secure 模式
    w[n++] = E64_AT_NULL;   w[n++] = 0;

    const uint64_t rsp = (str_va - 64u - (uint64_t)n * 8u) & ~((uint64_t)15);   // 16 字节对齐
    uint64_t* d = (uint64_t*)(uintptr_t)rsp;
    for (uint32_t i = 0; i < n; i++) d[i] = w[i];
    return rsp;
}

// ==================== 初始栈（execve 版：argv 由调用方给）====================
// 与 e64_build_stack64 的区别：字符串从栈顶向下排（不再固定 stack_top-32），复数个 argv 依次
// 摆放；指针数组与 auxv 在字符串下方。**不动** e64_build_stack64 的布局 —— 它的
// st[1] == stack_top-32 被 elf64_selftest64 逐字段断言。
static uint64_t e64_build_stack_argv64(const char* const* argv, uint32_t argc) {
    const uint64_t stack_top = USER64_STACK_VA64 + USER64_STACK_BYTES64;
    if (argc > 16) argc = 16;

    uint64_t argp[16];
    uint64_t ent_va = 0;                                   // argv[0] = 可执行文件路径（给 AT_ENTRY 之外的日志/AT_EXECFN 留用）
    uint64_t sp_va = stack_top;
    for (uint32_t i = 0; i < argc; i++) {
        const char* s = (argv && argv[i]) ? argv[i] : "";
        uint32_t len = e_strlen(s) + 1;
        if (len > 128) len = 128;                          // 单个参数的硬上限（演示/验收够用）
        sp_va = (sp_va - len) & ~((uint64_t)15);           // 每个字符串 16 字节对齐，便于日志直读
        uint8_t* d = (uint8_t*)(uintptr_t)sp_va;
        uint32_t k = 0;
        for (; k + 1u < len && s[k]; k++) d[k] = (uint8_t)s[k];
        d[k] = 0;
        argp[i] = sp_va;
        if (i == 0) ent_va = sp_va;
    }

    // ★ A3：AT_RANDOM 的 16 字节放在 [sp_va-16, sp_va) —— 字符串在 [sp_va, stack_top)，
    //   指针数组在 rsp..(rsp+8m) 且 rsp ≤ sp_va-128，所以这一段同样恒空且在 rsp 上方。
    const uint64_t rand_va = sp_va - 16;
    e64_fill_random64(rand_va);

    uint64_t w[64];
    uint32_t m = 0;
    w[m++] = argc;                                         // argc
    for (uint32_t i = 0; i < argc; i++) w[m++] = argp[i];  // argv[0..argc-1]
    w[m++] = 0;                                            // argv 终止
    w[m++] = 0;                                            // envp 终止（本内核没有环境变量）
    w[m++] = E64_AT_PAGESZ; w[m++] = PAGE_SIZE_64;
    w[m++] = E64_AT_PHDR;   w[m++] = g_img64.phdr_va;      // 程序头表在映像里的地址（加载器算好的）
    w[m++] = E64_AT_PHENT;  w[m++] = ELF64_PHDR_SIZE;      // ★ A3：musl 按它步进遍历程序头表
    w[m++] = E64_AT_PHNUM;  w[m++] = g_img64.phnum;
    w[m++] = E64_AT_BASE;   w[m++] = g_e64_at_base64;      // ★ A3 下半：解释器基址（静态 = 0）
    w[m++] = E64_AT_ENTRY;  w[m++] = g_img64.entry;
    w[m++] = E64_AT_HWCAP;  w[m++] = 0;                    // 本内核不暴露 hwcap（如实 0）
    w[m++] = E64_AT_UID;    w[m++] = 0;
    w[m++] = E64_AT_EUID;   w[m++] = 0;
    w[m++] = E64_AT_GID;    w[m++] = 0;
    w[m++] = E64_AT_EGID;   w[m++] = 0;
    w[m++] = E64_AT_RANDOM; w[m++] = rand_va;              // 上面那 16 字节随机
    w[m++] = E64_AT_SECURE; w[m++] = 0;                    // 没有权限模型 -> 非 secure 模式
    w[m++] = E64_AT_NULL;   w[m++] = 0;
    (void)ent_va;                                          // （AT_EXECFN 本轮不给：musl 只在 argv[0] 为空时才用）

    const uint64_t rsp = (sp_va - 128u - (uint64_t)m * 8u) & ~((uint64_t)15);   // 16 字节对齐
    uint64_t* d = (uint64_t*)(uintptr_t)rsp;
    for (uint32_t i = 0; i < m; i++) d[i] = w[i];
    return rsp;
}

// ==================== 映像映射（映射段 + 分块拷内容 + 清 .bss + 权限收紧）====================
// 为什么抽出来：主程序与**解释器**都走同一条 ELF64 段装载规则（只有一份实现才不会漂）。
// 记账（range）统一记进 g_loaded64：解释器与主程序都要被 unload 回收。
// ★ 本批：内容来源 **按需**—— blob != nullptr 时从内存 blob 拷（自检/安装期自校验；
//   blob_len = blob 长度），否则从**盘上按 p_offset + 分块**读（g_e64_chunk64，16 KiB/次）。
//   所以映像大小不再受"读盘缓冲"限制，只受文件系统（8 MiB）与窗口布局限制。
static int e64_seg_map_union64(const Elf64Image64* img, const char* path,
                               const uint8_t* blob, uint64_t blob_len) {
    // ---- 0) 冲突检查（一次，整个映像）：只对**不在老装载区**的映像做 ----
    //   老装载区（4GiB..4GiB+64KiB）的映像按历史行为直接覆盖（自有程序/演示都这么进来），
    //   而 mmap 区之上的映像必须落在真正空闲的地址上 —— 否则会"复用"解释器/别的映像
    //   已经映射的物理页（user64_map_page64 对已映射页只改权限位）。
    if (img->min_va >= USER64_MMAP_VA64) {
        const uint64_t a0 = img->min_va & ~((uint64_t)PAGE_SIZE_64 - 1);
        const uint64_t a1 = (img->span + PAGE_SIZE_64 - 1) & ~((uint64_t)PAGE_SIZE_64 - 1);
        for (uint64_t a = a0; a < a1; a += PAGE_SIZE_64) {
            if (a < USER64_MMAP_VA64) continue;
            if (user64_page_is_user_ok64(a)) return E64_SEG_REGION;
        }
    }
    // ---- 1) 各 PT_LOAD：先按 P|W|U 映射、拷内容、清 .bss ----
    for (uint32_t i = 0; i < img->nseg; i++) {
        const Elf64Seg64* s = &img->seg[i];
        const uint64_t va0 = s->va & ~((uint64_t)PAGE_SIZE_64 - 1);
        const uint64_t end = s->va + s->memsz;
        const uint64_t va1 = (end + PAGE_SIZE_64 - 1) & ~((uint64_t)PAGE_SIZE_64 - 1);
        const uint32_t pages = (uint32_t)((va1 - va0) / PAGE_SIZE_64);
        if (g_loaded64.nrange >= (uint32_t)(ELF64_MAX_PHDR64 * 2 + 2)) return E64_LOAD;

        // 注意：**不做**逐段的"这一页是否已映射"检查 —— 同一映像的相邻段经常共享一页
        // （stathello 的第 1/2 段就共用 4GiB 那页），逐段查会把合法映像误判成冲突。
        // 冲突检查在函数入口对**整个映像**做一次（那时本映像一个页都还没映射）。

        g_loaded64.range[g_loaded64.nrange].va = va0;
        g_loaded64.range[g_loaded64.nrange].pages = pages;
        g_loaded64.nrange++;

        for (uint32_t k = 0; k < pages; k++) {
            uint64_t phys = 0;
            if (!user64_map_page64(va0 + (uint64_t)k * PAGE_SIZE_64,
                                   PTE_USER_64 | PTE_WRITE_64, 1, &phys)) {
                return E64_LOAD;
            }
        }
        user64_paging_sync64();
        uint8_t* dst = (uint8_t*)(uintptr_t)s->va;        // 已映射可写：ring0 直接按 VA 写
        // ---- 内容：分块（blob 或盘）----
        uint64_t done = 0;
        while (done < s->filesz) {
            uint64_t left = s->filesz - done;
            uint32_t n = (uint32_t)((left > ELF64_READ_CHUNK64) ? ELF64_READ_CHUNK64 : left);
            if (blob) {
                if (s->off + done + n > blob_len) return E64_SEG_FILE;
                for (uint32_t b = 0; b < n; b++) dst[done + b] = blob[s->off + done + b];
            } else {
                if (!path) return E64_READ;
                if (e64_read_at64(path, s->off + done, g_e64_chunk64, n) != 0) return E64_READ;
                for (uint32_t b = 0; b < n; b++) dst[done + b] = g_e64_chunk64[b];
            }
            done += n;
        }
        for (uint64_t b = s->filesz; b < s->memsz; b++) dst[b] = 0;      // .bss 清零
    }

    // ---- 2) 权限收紧：按 p_flags 重设各段叶子权限位 ----
    // ★ 按**页**取并集，而不是"逐段覆盖"：两个权限不同的段可能落在同一页（页粒度权限表示不了
    //   段粒度），逐段覆盖会让后一个段把前一页的权限抹掉（例如 .text 与 .bss 挤一页时 .text 变
    //   可写/不可执行 -> 立刻 #PF）。并集规则：任一覆盖该页的段可写 -> 页可写；任一段可执行 ->
    //   页可执行。自带程序用 user/hello_elf64.ld 把各段页对齐，正常不会命中这条兜底。
    //   ★ A3 下半：表搬到 .bss 的 g_e64_page_*（★ 本批放大到 ELF64_PAGE_MAX64 项）—— 解释器/大映像可能上千页。
    {
        uint32_t np = 0;
        for (uint32_t i = 0; i < img->nseg; i++) {
            const Elf64Seg64* s = &img->seg[i];
            const uint64_t va0 = s->va & ~((uint64_t)PAGE_SIZE_64 - 1);
            const uint64_t va1 = (s->va + s->memsz + PAGE_SIZE_64 - 1) & ~((uint64_t)PAGE_SIZE_64 - 1);
            for (uint64_t a = va0; a < va1; a += PAGE_SIZE_64) {
                uint32_t k = 0;
                while (k < np && g_e64_page_va64[k] != a) k++;
                if (k == np) {
                    if (np >= ELF64_PAGE_MAX64) return E64_LOAD;
                    g_e64_page_va64[np] = a;
                    g_e64_page_fl64[np] = 0;
                    np++;
                }
                g_e64_page_fl64[k] |= (uint64_t)(s->flags & (ELF64_PF_W | ELF64_PF_X));
            }
        }
        for (uint32_t k = 0; k < np; k++) {
            const uint64_t f = PTE_USER_64                                 // PF_R（x86 上 P 就是可读）
                             | ((g_e64_page_fl64[k] & ELF64_PF_W) ? PTE_WRITE_64 : 0u)
                             | ((g_e64_page_fl64[k] & ELF64_PF_X) ? 0u : PTE_NX_64);
            if (!user64_remap_flags64(g_e64_page_va64[k], f)) return E64_LOAD;
        }
    }
    user64_paging_sync64();                                // 权限改完了，投递到 TLB
    return E64_OK;
}

// ==================== ★ A3 下半：解释器装载（PT_INTERP）====================
// 步骤：路径归一化（非绝对路径 -> /lib/<name>）-> stat -> 读元数据（头+程序头表）-> 量 span
//       -> 选基址 -> 解析（base 生效）-> 分块映射 + 权限收紧 -> 打点。
// ★ 本批两处改动：
//   1) **不再整份读盘**：解释器文件只按段读（823 KB 的 ld-musl / 215 KB 的 ld-linux 都能进来，
//      老实现是 96 KiB 读盘缓冲 -> `[ELF64] exec reject reason=interp-size`）。
//   2) 基址策略分两档（**老档一字不改**，保证 dynlink64_test/tcc 的解释器地址不动）：
//        a) span ≤ (USER64_INTERP_TOP_VA64 - USER64_MMAP_VA64)：base = 4GiB+1MiB - span（老行为）；
//        b) span 更大（musl 的 762 KiB 就是这种）：base = USER64_LOADER_BIG_TOP_VA64 - span，
//           即"窗口里 DEV/SHM 窗下沿往下 span 页"—— 那一段在老 mmap 区之上，不压任何固定分区。
static int e64_load_interp64(const char* in_path, uint64_t* out_base, uint64_t* out_entry) {
    char path[ELF64_INTERP_PATH_MAX64 + 8];
    uint32_t k = 0;
    if (!in_path || !in_path[0]) return E64_INTERP_ARG;
    if (in_path[0] != '/') {                                // 相对名字：按 /lib 搜索
        static const char LIBDIR64[] = "/lib/";
        for (uint32_t i = 0; LIBDIR64[i] && k + 1 < (uint32_t)sizeof(path); i++) path[k++] = LIBDIR64[i];
    }
    for (uint32_t i = 0; in_path[i] && k + 1 < (uint32_t)sizeof(path); i++) path[k++] = in_path[i];
    path[k] = 0;

    uint32_t type = 0, size = 0;
    if (vfs64_stat(path, &type, &size) != 0 || type != VFS64_TYPE_FILE) {
        dbg64_line_begin64();
        dbg64_str("[ELF64] interp reject path=");
        dbg64_str(path);
        dbg64_str(" reason=vfs\n");
        dbg64_line_end64();
        return E64_INTERP_VFS;
    }
    if (size == 0 || size > ELF64_MAX_FILE_BYTES64) return E64_INTERP_SIZE;

    // 元数据（ELF 头 + 程序头表）—— 解释器的程序头表会覆盖 g_e64_ph64，所以主程序的
    // **各段已经解析进 g_img64** 之后再调本函数（调用顺序在主程序解析之后）。
    uint16_t phnum = 0;
    {
        const int mrc = e64_read_meta64(path, size, &phnum);
        if (mrc != E64_OK) {
            dbg64_line_begin64();
            dbg64_str("[ELF64] interp reject path=");
            dbg64_str(path);
            dbg64_str(" reason=");
            dbg64_str(e_reason64(mrc));
            dbg64_nl();
            dbg64_line_end64();
            return (mrc == E64_SIZE) ? E64_INTERP_SIZE : E64_INTERP_BAD;
        }
    }
    if (e_rd16(g_e64_hdr64 + 16) == ELF64_ET_EXEC) {         // 解释器必须是 PIE（ET_DYN）
        dbg64_line_begin64();
        dbg64_str("[ELF64] interp reject path=");
        dbg64_str(path);
        dbg64_str(" reason=bad\n");
        dbg64_line_end64();
        return E64_INTERP_BAD;
    }

    uint64_t minv = 0, span = 0;
    int rc = e64_probe_layout64(phnum, &minv, &span);
    if (rc != E64_OK || span == 0) {
        dbg64_line_begin64();
        dbg64_str("[ELF64] interp reject path=");
        dbg64_str(path);
        dbg64_str(" reason=bad\n");
        dbg64_line_end64();
        return E64_INTERP_BAD;
    }

    // ---- 基址（两档，见函数头说明）----
    uint64_t top = USER64_INTERP_TOP_VA64;
    uint64_t base = 0;
    if (span <= (USER64_INTERP_TOP_VA64 - USER64_MMAP_VA64)) {
        base = USER64_INTERP_TOP_VA64 - span;                // 老档：与放大前逐字节相同
    } else {
        top = USER64_LOADER_BIG_TOP_VA64;
        if (span > (top - USER64_MMAP_VA64)) {
            dbg64_line_begin64();
            dbg64_str("[ELF64] interp reject path=");
            dbg64_str(path);
            dbg64_str(" reason=window\n");
            dbg64_line_end64();
            return E64_INTERP_WINDOW;
        }
        base = top - span;
        if (base < USER64_MMAP_VA64) {
            dbg64_line_begin64();
            dbg64_str("[ELF64] interp reject path=");
            dbg64_str(path);
            dbg64_str(" reason=window\n");
            dbg64_line_end64();
            return E64_INTERP_WINDOW;
        }
    }

    // 解析（映像区间 = [base, top)；段 VA = base + p_vaddr）
    Elf64Image64 img;
    uint64_t bad = 0;
    rc = e64_parse_core64(g_e64_hdr64, g_e64_ph64, size, base - minv, base, top, &img, &bad);
    if (rc != E64_OK || img.has_interp) {
        dbg64_line_begin64();
        dbg64_str("[ELF64] interp reject path=");
        dbg64_str(path);
        dbg64_str(img.has_interp ? " reason=interp\n" : " reason=bad\n");
        dbg64_line_end64();
        return img.has_interp ? E64_INTERP_ARG : E64_INTERP_BAD;
    }

    // 分块映射 + 拷内容（从盘上按 p_offset 读）
    const int mrc = e64_seg_map_union64(&img, path, nullptr, 0);
    if (mrc != E64_OK) {
        dbg64_line_begin64();
        dbg64_str("[ELF64] interp reject path=");
        dbg64_str(path);
        dbg64_str(" reason=");
        dbg64_str(e_reason64(mrc));
        dbg64_nl();
        dbg64_line_end64();
        return (mrc == E64_SEG_REGION) ? E64_INTERP_WINDOW : E64_INTERP_LOAD;
    }

    // 证据行（tests/dynlink64_test.py grep）：解释器是谁、落在哪、入口在哪、占了多大
    //   ★ 本批在行尾追加 size= 与 mode=（老字段顺序/格式一字不改，追加不影响既有 grep）。
    dbg64_line_begin64();
    dbg64_str("[ELF64] interp path=");
    dbg64_str(path);
    dbg64_str(" base=");
    dbg64_hex64(base);
    dbg64_str(" entry=");
    dbg64_hex64(img.entry);
    dbg64_str(" span=");
    dbg64_dec(span);
    dbg64_str(" size=");
    dbg64_dec(size);
    dbg64_str(" top=");
    dbg64_hex64(top);
    dbg64_nl();
    dbg64_line_end64();

    if (out_base)  *out_base  = base;
    if (out_entry) *out_entry = img.entry;
    return E64_OK;
}

// ==================== 装载（读元数据/定布局/映射/建栈；不打印）====================
// 两种入口：
//   * blob != nullptr  —— 内存里的一份合法映像（自检/安装期自校验/VAP64 嗅探）：base = 0、
//     映像区间 = 老装载区（与历史行为逐字节一致）。
//   * path != nullptr  —— 从文件系统读：stat 定长度 -> 读头+程序头表 -> **按程序头布局 + load bias**
//     定 (base, lo, hi) -> 解析 -> 分块映射。文件长度存进 g_e64_filelen64（打点用）。
static uint64_t g_e64_filelen64 = 0;
static int elf64_load_image64(const char* path, const uint8_t* blob, uint32_t blob_len,
                              uint64_t* out_entry) {
    if (g_loaded64.used) return E64_BUSY;                 // 一份镜像都没收尾：先 unload
    uint64_t bad_va = 0;
    int prc = E64_OK;
    if (blob) {
        g_e64_filelen64 = blob_len;
        g_e64_lmode64 = 0;
        g_e64_lbase64 = 0;
        g_e64_lspan64 = 0;
        prc = elf64_parse64(blob, blob_len, &g_img64, &bad_va);
    } else {
        uint32_t type = 0, size = 0;
        if (!path || vfs64_stat(path, &type, &size) != 0 || type != VFS64_TYPE_FILE) return E64_VFS;
        if (size == 0 || size > ELF64_MAX_FILE_BYTES64) return E64_SIZE;
        g_e64_filelen64 = size;
        uint16_t phnum = 0;
        prc = e64_read_meta64(path, size, &phnum);        // -> g_e64_hdr64 / g_e64_ph64
        if (prc == E64_OK) {
            uint64_t base = 0, lo = 0, hi = 0, span = 0, minv = 0;
            uint32_t mode = 0;
            (void)e64_probe_layout64(phnum, &minv, &span);
            prc = e64_pick_main_layout64(phnum, &base, &lo, &hi, &mode);
            g_e64_lmode64 = mode;
            g_e64_lbase64 = base;
            if (prc == E64_OK) {
                prc = e64_parse_core64(g_e64_hdr64, g_e64_ph64, size, base, lo, hi, &g_img64, &bad_va);
                // 大映像（不在老装载区里）先判固定分区冲突：栈/brk/邮箱/自检页、DEV/SHM 窗
                //   注意 img->span 是**绝对末地址**（max p_vaddr+p_memsz），不是长度。
                if (prc == E64_OK && mode != 0 &&
                    e64_region_conflict64(g_img64.min_va, g_img64.span)) {
                    bad_va = g_img64.min_va;
                    prc = E64_SEG_REGION;
                }
                if (prc == E64_OK) prc = e64_interp_take64(path, nullptr, 0, &g_img64);
            }
        }
    }
    if (prc != E64_OK) {
        g_e64_bad_va64 = bad_va;                           // 打印者用（reason 里带出出错的 va）
        g_loaded64.nrange = 0;
        return prc;
    }

    g_loaded64.used = 1;
    g_loaded64.nrange = 0;
    g_loaded64.interp_base = 0;
    g_e64_at_base64 = 0;
    uint64_t run_entry = g_img64.entry;                    // 执行入口（有解释器时会被换掉）

    // ---- 0) 解释器优先（有 PT_INTERP 时它要先能被跑起来，才能去重定位主程序）----
    if (g_img64.has_interp) {
        uint64_t ibase = 0, ientry = 0;
        const int irc = e64_load_interp64(g_img64.interp, &ibase, &ientry);
        if (irc != E64_OK) { g_loaded64.used = 0; g_loaded64.nrange = 0; return irc; }
        g_loaded64.interp_base = ibase;
        g_e64_at_base64 = ibase;
        run_entry = ientry;
    }

    // ---- 1) 主程序各 PT_LOAD + 权限收紧（blob 或按段读盘）----
    {
        const int mrc = e64_seg_map_union64(&g_img64, path, blob, (uint64_t)blob_len);
        if (mrc != E64_OK) { elf64_unload64(); return mrc; }
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
    e64_log_auxv64(rsp);                                           // ★ A3：auxv 证据（grep 用）

    g_loaded64.entry = run_entry;
    g_loaded64.user_rsp = rsp;
    if (out_entry) *out_entry = run_entry;
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

// ==================== execve 复用入口（批次 C）====================
void elf64_forget64() {
    g_loaded64.used    = 0;
    g_loaded64.entry   = 0;
    g_loaded64.user_rsp = 0;
    g_loaded64.interp_base = 0;
    g_loaded64.nrange  = 0;
    g_e64_at_base64    = 0;
}

// ★ A3 下半：最近一次装载的解释器基址（0 = 静态/无解释器）。见 elf64.h。
uint64_t elf64_interp_base64() { return g_loaded64.interp_base; }

int elf64_blob_ok64(const uint8_t* p, uint32_t n) {
    Elf64Image64 img;
    uint64_t bad_va = 0;
    return elf64_parse64(p, n, &img, &bad_va) == E64_OK ? 1 : 0;
}

// 读盘 -> 装载到**当前**地址空间 -> 按 argv 建初始栈。调用方负责：先清掉这个进程的旧映像、
// 保证当前 CR3 是该进程的、装载成功后自己管生命周期（本函数不留记账）。
// ★ 本批：整份读盘那三步（stat 判长度 -> vfs64_read 进 g_elf_file64）删掉 —— 交给
//   elf64_load_image64(path, nullptr, 0, …)：它按段分块读、并按程序头布局选 load bias。
int elf64_load_for_exec64(const char* path, const char* const* argv, uint32_t argc,
                          uint64_t* out_entry, uint64_t* out_rsp) {
    if (!path || path[0] == 0) { e64_log_reject64("[ELF64] exec reject", E64_ARG); return -1; }
    g_loaded64.used   = 0;                        // 旧映像由调用方释放；这里只允许重新装载
    g_loaded64.nrange = 0;
    uint64_t entry = 0;
    const int rc = elf64_load_image64(path, nullptr, 0, &entry);
    if (rc != E64_OK) {
        e64_log_reject64("[ELF64] exec reject", rc);
        g_loaded64.used = 0;
        return -1;
    }
    const uint64_t rsp = e64_build_stack_argv64(argv, argc);
    // ★ A3：execve 路径的打点 —— 与 elf64_load64 的 "[ELF64] load …" 同一格式（这里是
    //   **真**从 VFS 读出来 + 解析 + 装载 + 建初始栈/auxv 的证据），自动验收 grep 用。
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
    dbg64_dec(g_e64_filelen64);
    dbg64_str(" rsp=");
    dbg64_hex64(rsp);
    dbg64_str(" via=execve");
    dbg64_str(" bias=");
    dbg64_hex64(g_e64_lbase64);
    dbg64_str(" span=");
    dbg64_dec(g_e64_lspan64);
    dbg64_str(" layout=");
    dbg64_dec(g_e64_lmode64);
    dbg64_str(" chunk=");
    dbg64_dec(ELF64_READ_CHUNK64);
    dbg64_nl();
    dbg64_line_end64();
    e64_log_auxv64(rsp);
    if (out_entry) *out_entry = entry;
    if (out_rsp)   *out_rsp   = rsp;
    g_loaded64.used = 0;                          // 记账交回调用方（单份全局，见 elf64.h 说明）
    return 0;
}

int elf64_load64(const char* path, uint64_t* out_entry) {
    if (!path || path[0] == 0) { e64_log_reject64("[ELF64] reject", E64_ARG); return -1; }
    if (g_loaded64.used) { e64_log_reject64("[ELF64] reject", E64_BUSY); return -1; }

    // 解析 + 装载 + 建栈（★ 本批：内部按段分块读盘 + 按程序头布局选 load bias）
    uint64_t entry = 0;
    const int rc = elf64_load_image64(path, nullptr, 0, &entry);
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
    dbg64_dec(g_e64_filelen64);
    dbg64_str(" rsp=");
    dbg64_hex64(g_loaded64.user_rsp);
    dbg64_str(" bias=");
    dbg64_hex64(g_e64_lbase64);
    dbg64_str(" span=");
    dbg64_dec(g_e64_lspan64);
    dbg64_str(" layout=");
    dbg64_dec(g_e64_lmode64);
    dbg64_str(" chunk=");
    dbg64_dec(ELF64_READ_CHUNK64);
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

// ==================== 幂等安装（hello.elf -> VFS /hello.elf；字节在卷/原始区）====================
int elf64_install_builtin64(int drive, uint32_t part_lba) {
    uint32_t bytes = 0;
    const uint8_t* blob = demo64_blob_find64(ELF64_INSTALL_PATH64, &bytes);
    // 先自校验映像（与运行时同一条解析路径），坏了就别往盘上写
    Elf64Image64 img;
    uint64_t bad_va = 0;
    if (!blob || bytes == 0 || elf64_parse64(blob, bytes, &img, &bad_va) != E64_OK) {
        dbg64_line_begin64();
        dbg64_str("[ELF64] install FAILED reason=blob\n");
        dbg64_line_end64();
        return -1;
    }

    // 卷可能没挂载：先探**系统卷**根目录，没有就 vfs64_mount_system64（挂/登记系统卷槽，不改别的槽）。
    // ★ 多卷（为什么不会写错卷）：/hello.elf 是系统卷文件 —— 探/写都用 *_on64(系统卷槽, …)：
    //   用户浏览 D: 时这里也只会写 C:（on64 单次调用内临时切卷，返回前切回）。
    uint32_t t = 0, sz = 0;
    {
        const int sys0 = vfs64_system_slot64();
        if (sys0 < 0 || vfs64_stat_on64(sys0, "/", &t, &sz) != 0) {
            if (vfs64_mount_system64(drive, part_lba) != 0) {
                dbg64_line_begin64();
                dbg64_str("[ELF64] install FAILED reason=mount\n");
                dbg64_line_end64();
                return -1;
            }
        }
    }
    const int sys = vfs64_system_slot64();
    if (vfs64_stat_on64(sys, ELF64_INSTALL_PATH64, &t, &sz) == 0) {       // 幂等：已存在就跳过
        dbg64_line_begin64();
        dbg64_str("[ELF64] install skipped (exists) /hello.elf size=");
        dbg64_dec(sz);
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    const int w = vfs64_write_on64(sys, ELF64_INSTALL_PATH64, blob, (int)bytes);
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
    // ★ 本批：读盘语义的自述打点（自动验收/诊断用；格式：[ELF64] read mode=… 各字段一行）。
    //   它把"上限归属"钉成可核对的数字：max_file = 拒绝门限（= FS 上限 8 MiB）、chunk = 分块大小、
    //   page_max = 权限并集表容量（页）；"读盘不再整份进缓冲"这条因此可被脚本核对。
    dbg64_line_begin64();
    dbg64_str("[ELF64] read mode=segmented chunk=");
    dbg64_dec(ELF64_READ_CHUNK64);
    dbg64_str(" max_file=");
    dbg64_dec(ELF64_MAX_FILE_BYTES64);
    dbg64_str(" phdr_max=");
    dbg64_dec(ELF64_MAX_PHDR64);
    dbg64_str(" page_max=");
    dbg64_dec(ELF64_PAGE_MAX64);
    dbg64_str(" legacy_bytes=");
    dbg64_dec(ELF64_LEGACY_BYTES64);
    dbg64_str(" loader_top=");
    dbg64_hex64(USER64_LOADER_BIG_TOP_VA64);
    dbg64_nl();
    dbg64_line_end64();
    //   而本自检的 bit1/bit2/bit10 会**真的装载**一份映像到用户窗口 —— 那一步在 UEFI 下必然 #PF。
    //   所以这里如实跳过并打一行说明（不假装 PASS，也不把内核打死）。
    if (!user64_available64()) {
        dbg64_line_begin64();
        dbg64_str("[ELF64] selftest skipped (user window unavailable: firmware page tables are read-only)\n");
        dbg64_line_end64();
        return 0;
    }
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
    if (elf64_load_image64(nullptr, buf, n, &entry) != E64_OK) {
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
        // ★ A3：auxv 逐条核对（不止 AT_ENTRY/AT_NULL）—— 号错/缺项在这里立刻暴露。
        //   AT_PHENT 必须是 56、AT_PHNUM 与解析结果一致、AT_RANDOM 必须是可读的 16 字节
        //   （这里真的逐字节读一遍）、AT_SECURE 必须 0、AT_UID==AT_EUID（musl 才不会走降权路径）。
        bool at_entry = false, at_null = false, at_phent = false, at_phnum = false;
        bool at_random = false, at_secure = false, at_uid_eq = false;
        uint64_t at_uid = 1, at_euid = 2;
        for (uint32_t i = 4; i < 64; i += 2) {
            if (st[i] == E64_AT_ENTRY && st[i + 1] == tva) at_entry = true;
            if (st[i] == E64_AT_PHENT && st[i + 1] == ELF64_PHDR_SIZE) at_phent = true;
            if (st[i] == E64_AT_PHNUM && st[i + 1] == img.phnum) at_phnum = true;
            if (st[i] == E64_AT_RANDOM) {
                const uint8_t* rp = (const uint8_t*)(uintptr_t)st[i + 1];
                uint32_t acc = 0;                                        // 逐字节读（不可读这里就 #PF -> 自检失败）
                for (uint32_t k = 0; k < 16; k++) acc |= rp[k];
                at_random = user64_page_is_user_ok64(st[i + 1]) && (acc != 0);
            }
            if (st[i] == E64_AT_SECURE && st[i + 1] == 0) at_secure = true;
            if (st[i] == E64_AT_UID)  at_uid  = st[i + 1];
            if (st[i] == E64_AT_EUID) at_euid = st[i + 1];
            if (st[i] == E64_AT_NULL) { at_null = true; break; }
        }
        if (at_uid == at_euid) at_uid_eq = true;
        if (!at_entry || !at_null || !at_phent || !at_phnum || !at_random || !at_secure || !at_uid_eq) fail |= 4;
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

    // ---- bit10：交付给系统卷的 hello.elf 必须能解析（构建期产物与加载器同口径）----
    //   ★ 本批：字节不在内核里了 —— 从构建期"原始区"取（见 kernel/demo64.h）；取不到就跳过
    //     这一位（构建期已有"探针/回读"两道断言，不是运行期静默放过）。
    {
        uint32_t hb = 0;
        const uint8_t* hblob = demo64_blob_find64(ELF64_INSTALL_PATH64, &hb);
        if (!hblob || hb == 0) {
            dbg64_line_begin64();
            dbg64_str("[ELF64] selftest bit10 skipped (no hello.elf in blob region)\n");
            dbg64_line_end64();
        } else if (elf64_parse64(hblob, hb, &img, &bad_va) != E64_OK) {
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
