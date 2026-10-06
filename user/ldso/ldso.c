/* ldso.c - ★ A3 下半：**VimtuOS 自己的动态链接器**（ring3，跑在主程序之前）
 *
 * 谁调用它、它做什么（与内核的分工见 kernel/elf64.h 的"动态链接"段）：
 *   内核装载带 PT_INTERP 的主程序时，会先把本文件映射到用户窗口顶部，auxv 给：
 *     AT_BASE  = 本 ld.so 的加载基址（= 链接基址 0 + 内核挑的基址）
 *     AT_ENTRY = 主程序入口      AT_PHDR = 主程序程序头表地址
 *   然后把控制权交给 _dlstart（user/ldso/ldso_start.S），rsp = 内核建好的初始栈。
 *   本文件从初始栈读 auxv，然后：
 *     1) **自定位**：用 AT_BASE 找到自己的 PT_DYNAMIC，应用自己的 R_X86_64_RELATIVE；
 *     2) 找主程序的 PT_DYNAMIC / PT_NEEDED，按 DT_NEEDED **递归加载 .so**
 *        （open/read + **整块** mmap(MAP_FIXED) 后逐段读入 / mprotect，段权限逐页分开：R|X / R|W）；
 *     3) 重定位：R_X86_64_RELATIVE（必须）、R_X86_64_GLOB_DAT / JUMP_SLOT（必须）、
 *        R_X86_64_64；符号解析顺序 = **主程序优先 -> 依赖加载顺序 -> 弱符号**；
 *     4) 调用各对象的 DT_INIT / DT_INIT_ARRAY（依赖先于依赖者，主程序最后）；
 *     5) 返回 AT_ENTRY，由 _dlstart 恢复初始栈并跳过去。
 *
 * 本阶段如实**不做**的（doc 里也列了）：lazy binding（PLT 立即绑定；不解析
 * DT_PLTGOT/DT_NEEDED 之外的动态项）、dlopen/dlsym、TLS 动态模型（PT_TLS/DT_TLSDESC）、
 * vDSO、LD_LIBRARY_PATH/LD_PRELOAD、rpath/runpath、版本符号（DT_VERSYM/VERNEED）、
 * 依赖环检测（按名字去重，环会打点放弃）。
 *
 * 为什么不用 libc：它是"程序之前"的程序 —— 只能自己实现最少的 syscall 包装与字符串函数。
 * 编译：tools/dynlink_build_win.sh（clang -fPIC -nostdlib + ld.lld -shared + user/ldso/ldso.ld）。
 *   * 所有符号都声明成 static（除 _dlstart/ldso_main/ldso_saved_sp 外），于是 -fPIC 下对
 *     它们都是 PC 相对访问 —— **自定位完成前不碰任何需要重定位的存储**；
 *   * 唯一需要重定位的是 .data/.got 里的指针（R_X86_64_RELATIVE），自定位函数只用参数。
 */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long      u64;
typedef long               i64;

/* ==================== ELF64 结构（与内核 kernel/elf64.cpp 同一份 ABI）==================== */
typedef struct { u8 e_ident[16]; u16 e_type, e_machine; u32 e_version;
                 u64 e_entry, e_phoff, e_shoff; u32 e_flags;
                 u16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } Ehdr;
typedef struct { u32 p_type, p_flags; u64 p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align; } Phdr;
typedef struct { i64 d_tag; u64 d_val; } Dyn;
typedef struct { u32 st_name; u8 st_info, st_other; u16 st_shndx; u64 st_value, st_size; } Sym;
typedef struct { u64 r_offset, r_info; i64 r_addend; } Rela;

#define PT_LOAD      1u
#define PT_DYNAMIC   2u
#define PT_PHDR      6u
#define DT_NULL      0
#define DT_NEEDED    1
#define DT_PLTRELSZ  2
#define DT_HASH      4
#define DT_STRTAB    5
#define DT_SYMTAB    6
#define DT_RELA      7
#define DT_RELASZ    8
#define DT_RELAENT   9
#define DT_INIT      12
#define DT_SONAME    14
#define DT_PLTREL    20
#define DT_JMPREL    23
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27
#define DT_GNU_HASH  0x6ffffef5
#define DT_FLAGS_1   0x6ffffffb

#define R_X86_64_64        1
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8

#define STB_WEAK   2
#define STT_GNU_IFUNC 10

/* 用户窗口常量（kernel/usermode64.h 的唯一定义点；这里只用到 mmap 区起点做下限校验） */
#define LD_MMAP_MIN_VA 0x0000000100090000UL

/* ==================== 最小 syscall（Linux x86_64 ABI；内核 syscall64 就是这套号）==================== */
static i64 sys6(u64 nr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6) {
    register u64 r10 __asm__("r10") = a4;
    register u64 r8  __asm__("r8")  = a5;
    register u64 r9  __asm__("r9")  = a6;
    i64 ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}
static i64 ld_open(const char* p)        { return sys6(2, (u64)p, 0, 0, 0, 0, 0); }
static i64 ld_close(i64 fd)              { return sys6(3, (u64)fd, 0, 0, 0, 0, 0); }
static i64 ld_read(i64 fd, void* b, u64 n) { return sys6(0, (u64)fd, (u64)b, n, 0, 0, 0); }
static i64 ld_lseek(i64 fd, u64 off)     { return sys6(8, (u64)fd, off, 0 /*SEEK_SET*/, 0, 0, 0); }
static i64 ld_mmap_fixed(u64 va, u64 len) {
    /* prot 被本内核忽略（mmap 总是映射成 RW+U+NX），随后用 mprotect 收紧；
       flags = MAP_PRIVATE(2) | MAP_FIXED(0x10) | MAP_ANONYMOUS(0x20)。
       ★ 调用点必须**每个对象只调一次**（整块 span）：Linux 的 MAP_FIXED 语义是"先丢弃旧映射"，
       同一页映第二次会把上一次写进去的内容整页清掉（见 ld_load_so64 的回归说明）。 */
    return sys6(9, va, len, 3, 0x32, (u64)-1, 0);
}
static i64 ld_mprotect(u64 va, u64 len, u64 prot) { return sys6(10, va, len, prot, 0, 0, 0); }

/* ==================== 最小 print（fd 1 = 串口 + 屏幕；内核 syscall write(1) 原样转发）==================== */
static void ld_write(const char* s, u64 n) { if (n) sys6(1, 1, (u64)s, n, 0, 0, 0); }
static void ld_str(const char* s) { u64 n = 0; while (s[n]) n++; ld_write(s, n); }
static void ld_hex(u64 v) {
    static const char H[] = "0123456789abcdef";
    char b[19];
    b[0] = '0'; b[1] = 'x';
    for (u32 i = 0; i < 16; i++) b[2 + i] = H[(v >> (60 - 4 * i)) & 0xF];
    b[18] = 0;
    ld_write(b, 18);
}
static void ld_dec(u64 v) {
    char b[24];
    u32 i = 23;
    b[i] = 0;
    if (v == 0) { b[--i] = '0'; }
    while (v) { b[--i] = (char)('0' + (u32)(v % 10u)); v /= 10u; }
    ld_write(&b[i], (u64)(23 - i));
}
static void ld_pre(const char* tag) { ld_str("[LDSO] "); ld_str(tag); }
static void ld_endl(void) { ld_str("\n"); }
_Noreturn static void ld_fail(const char* why) {
    ld_pre("FAIL "); ld_str(why); ld_endl();
    sys6(60, 127, 0, 0, 0, 0, 0);            /* exit(127)：与 Linux ld.so 的失败码同口径 */
    for (;;) { }
}

/* ==================== 已加载对象（BFS 顺序；[0] = 主程序）==================== */
#define LD_MAX_OBJ 8
typedef struct {
    char  name[40];
    u64   base;
    const Dyn*  dyn;
    const Sym*  symtab;
    const char* strtab;
    u64   nsym;
    const Rela* rela;      u64 relasz;
    const Rela* jmprel;    u64 pltrelsz;
    u64   init;
    u64   init_array;      u64 init_arraysz;
    u64   soname;          /* DT_SONAME 在 strtab 里的偏移；0 = 没有 */
    u8    is_main;
} Obj64;

static Obj64 g_obj[LD_MAX_OBJ];
static int   g_nobj;
static u64   g_at_base;
static u64   g_alloc_top;             /* 库分配游标（向下：从 AT_BASE 往低走） */
static u64   g_rel_n[LD_MAX_OBJ];     /* 每对象的 RELATIVE 条数 */
static const char LD_SELF_TAG[];      /* 定义在文件末尾（自定位证据的字符串字面量） */
static const char* volatile g_self_tab[2];     /* 自定位证据（ldso 自己的 .data 指针数组） */

/* ---- 小工具（全部静态：-fPIC 下 PC 相对，无需重定位）---- */
static void ld_memzero(void* p, u64 n) { u8* d = (u8*)p; for (u64 i = 0; i < n; i++) d[i] = 0; }
static int  ld_streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    u64 i = 0;
    for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
    return a[i] == b[i] && a[i] == 0;
}
static void ld_strcpy_n(char* dst, const char* src, u32 cap) {
    u32 i = 0;
    for (; src && src[i] && i + 1u < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* ==================== 自定位（R_X86_64_RELATIVE）==================== */
/* 只用参数：调用它之前不能碰任何 .data/.got（见文件头说明）。返回应用的条数。 */
static u64 ld_apply_relative64(u64 base, const Dyn* dyn, u64* out_bytes) {
    const Rela* rela = 0;
    u64 relasz = 0, relaent = 24;
    for (const Dyn* d = dyn; d && d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_RELA)    rela = (const Rela*)(base + d->d_val);
        else if (d->d_tag == DT_RELASZ) relasz = d->d_val;
        else if (d->d_tag == DT_RELAENT) relaent = d->d_val;
    }
    if (!rela || !relasz || relaent < 24) { if (out_bytes) *out_bytes = 0; return 0; }
    u64 n = 0;
    for (u64 off = 0; off + 24 <= relasz; off += relaent) {
        const Rela* r = (const Rela*)((const u8*)rela + off);
        const u32 type = (u32)(r->r_info & 0xffffffffu);
        if (type == R_X86_64_RELATIVE) {
            *(u64*)(base + r->r_offset) = base + (u64)r->r_addend;
            n++;
        }
    }
    if (out_bytes) *out_bytes = relasz;
    return n;
}

/* ==================== 程序头辅助 ==================== */
static const Phdr* ld_phdr_at(const Ehdr* eh, u64 file_base, u32 i, u16 phentsize) {
    if (!eh || eh->e_phoff == 0) return 0;
    return (const Phdr*)((const u8*)(file_base + eh->e_phoff) + (u64)i * phentsize);
}
static const Dyn* ld_find_dynamic64(const Ehdr* eh, u64 mem_base) {
    if (!eh || eh->e_phoff == 0 || eh->e_phentsize != 56) return 0;
    for (u32 i = 0; i < eh->e_phnum; i++) {
        const Phdr* ph = ld_phdr_at(eh, mem_base, i, 56);
        if (ph && ph->p_type == PT_DYNAMIC) return (const Dyn*)(mem_base + ph->p_vaddr);
    }
    return 0;
}

/* ==================== 符号表规模（DT_HASH / DT_GNU_HASH 双实现）==================== */
// 参数 base：DT_HASH / DT_GNU_HASH 的 d_val 是**链接期地址**（对 .so 是 0 基），
// 必须加装载基址才能读 —— 少加就是"去读 0x324"那种 #PF（实测踩过）。
static u64 ld_sym_count64(const Dyn* dyn, u64 base, const char** which) {
    const u32* hash = 0;
    const u32* gnu  = 0;
    for (const Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_HASH) hash = (const u32*)(base + d->d_val);
        else if (d->d_tag == DT_GNU_HASH) gnu = (const u32*)(base + d->d_val);
    }
    if (hash) { if (which) *which = "sysv"; return hash[1]; }        /* [1] = nchain */
    if (gnu) {
        const u32 nbuckets  = gnu[0];
        const u32 symoffset = gnu[1];
        const u32 bloom_sz  = gnu[2];
        const u32* buckets  = gnu + 4 + bloom_sz * 2;
        const u32* chain    = buckets + nbuckets;
        u64 n = symoffset;
        for (u32 b = 0; b < nbuckets; b++) {
            u32 idx = buckets[b];
            if (idx == 0) continue;
            u64 k = (u64)idx - symoffset;
            for (;;) {
                const u32 v = chain[k];
                if ((u64)idx + 1 > n) n = (u64)idx + 1;
                k++;
                if (v & 1u) break;
                if (k > 100000u) break;                              /* 防御：坏表不死循环 */
            }
        }
        if (which) *which = "gnu";
        return n;
    }
    if (which) *which = "none";
    return 0;
}

/* ==================== 解析工具（obj 的 .dynamic -> 各表指针）==================== */
static void ld_fill_obj64(Obj64* o) {
    o->rela = 0; o->relasz = 0; o->jmprel = 0; o->pltrelsz = 0;
    o->init = 0; o->init_array = 0; o->init_arraysz = 0;
    for (const Dyn* d = o->dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:  o->symtab   = (const Sym*)(o->base + d->d_val); break;
            case DT_STRTAB:  o->strtab   = (const char*)(o->base + d->d_val); break;
            case DT_RELA:    o->rela     = (const Rela*)(o->base + d->d_val); break;
            case DT_RELASZ:  o->relasz   = d->d_val; break;
            case DT_JMPREL:  o->jmprel   = (const Rela*)(o->base + d->d_val); break;
            // ★ 这里原来漏了 DT_PLTRELSZ（=2）：jmprel 有值、pltrelsz 一直是 0，于是
            //   ld_reloc_table64(o, jmprel, 0, …) 直接返回 —— **JUMP_SLOT 一条都没做**。
            //   症状（实测）：ld.so 自己的汇总行 `jump_slot=0`，主程序第一次走自己的 PLT
            //   （入口桩 call dh_main）就跳到未重定位的 GOT 槽 0 -> #PF rip=0 cr2=0。
            case DT_PLTRELSZ: o->pltrelsz = d->d_val; break;
            case DT_PLTREL:  /* 必须是 RELA(7)；不是就按 0 处理（后面的循环会跳过） */ break;
            case DT_INIT:    o->init     = o->base + d->d_val; break;
            case DT_INIT_ARRAY: o->init_array = o->base + d->d_val; break;
            case DT_INIT_ARRAYSZ: o->init_arraysz = d->d_val; break;
            case DT_SONAME:  o->soname   = d->d_val; break;
            default: break;
        }
    }
    if (!o->symtab || !o->strtab) ld_fail("obj: no symtab/strtab");
    o->nsym = ld_sym_count64(o->dyn, o->base, 0);
    if (o->nsym == 0) ld_fail("obj: no hash/gnu_hash (cannot size symtab)");
}

/* ==================== 符号解析（主程序优先 -> 依赖顺序 -> 弱符号）==================== */
static u64 ld_lookup64(const char* name, int* is_weak) {
    u64 weak_val = 0;
    int weak_hit = 0;
    for (int i = 0; i < g_nobj; i++) {
        const Obj64* o = &g_obj[i];
        for (u64 k = 1; k < o->nsym; k++) {
            const Sym* s = &o->symtab[k];
            if (s->st_shndx == 0) continue;                       /* 未定义：不是定义 */
            const char* n = o->strtab + s->st_name;
            if (!ld_streq(n, name)) continue;
            if ((s->st_info >> 4) == STB_WEAK) {                  /* 弱定义：先记下，继续找强的 */
                if (!weak_hit) { weak_val = o->base + s->st_value; weak_hit = 1; }
                continue;
            }
            if ((s->st_info & 0xf) == STT_GNU_IFUNC) {
                ld_pre("warn ifunc sym="); ld_str(name); ld_endl();   /* 不支持：如实打点后用其地址 */
            }
            if (is_weak) *is_weak = 0;
            return o->base + s->st_value;
        }
    }
    if (weak_hit) { if (is_weak) *is_weak = 1; return weak_val; }
    if (is_weak) *is_weak = 0;
    return 0;
}

/* ==================== 单个重定位表：GLOB_DAT / JUMP_SLOT / 64 ==================== */
static void ld_reloc_table64(Obj64* o, const Rela* tab, u64 sz, u64* n_glob, u64* n_jump, u64* n_abs) {
    if (!tab || !sz) return;
    for (u64 off = 0; off + 24 <= sz; off += 24) {
        const Rela* r = (const Rela*)((const u8*)tab + off);
        const u32 type = (u32)(r->r_info & 0xffffffffu);
        if (type == R_X86_64_RELATIVE) continue;                  /* 自定位那一步已做 */
        if (type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT && type != R_X86_64_64) continue;
        const u64 symidx = r->r_info >> 32;
        if (symidx == 0 || symidx >= o->nsym) ld_fail("reloc: bad symidx");
        const Sym* s = &o->symtab[symidx];
        const char* name = o->strtab + s->st_name;
        int weak = 0;
        u64 val = ld_lookup64(name, &weak);
        if (!val && !weak) {
            ld_pre("FAIL unresolved sym="); ld_str(name);
            ld_str(" obj="); ld_str(o->name); ld_endl();
            ld_fail("relocation");
        }
        *(u64*)(o->base + r->r_offset) = val + (u64)r->r_addend;
        if (type == R_X86_64_GLOB_DAT) {
            (*n_glob)++;
            ld_pre("resolve sym="); ld_str(name); ld_str(" kind=glob_dat val="); ld_hex(val);
            ld_str(" weak="); ld_dec((u64)(weak ? 1 : 0)); ld_endl();
        } else if (type == R_X86_64_JUMP_SLOT) {
            (*n_jump)++;
            ld_pre("resolve sym="); ld_str(name); ld_str(" kind=jump_slot val="); ld_hex(val); ld_endl();
        } else {
            (*n_abs)++;
            // ★ R_X86_64_64 也打点：④ 号断言（符号解析顺序）用的就是这条路径 —— .so 里
            //   "取同名函数地址"会生成 R_X86_64_64（不 -Bsymbolic 的共享库），ld.so 解析到
            //   哪一份，看这一行的 val 就知道（主程序优先 -> 落在 4GiB 装载区）。
            ld_pre("resolve sym="); ld_str(name); ld_str(" kind=abs64 val="); ld_hex(val);
            ld_str(" addend="); ld_dec((u64)r->r_addend); ld_endl();
        }
    }
}

/* ==================== 加载一个 .so（open/read/mmap/mprotect）==================== */
static int ld_load_so64(const char* name) {
    char path[64];
    u32 k = 0;
    if (name[0] != '/') {
        ld_strcpy_n(path, "/lib/", sizeof(path));
        while (path[k]) k++;
    }
    for (u32 i = 0; name[i] && k + 1u < (u32)sizeof(path); i++) path[k++] = name[i];
    path[k] = 0;

    i64 fd = ld_open(path);
    if (fd < 0) {                                              /* /lib/<name> 没找到 -> 试根目录 */
        k = 0;
        path[k++] = '/';
        for (u32 i = 0; name[i] && k + 1u < (u32)sizeof(path); i++) path[k++] = name[i];
        path[k] = 0;
        fd = ld_open(path);
        if (fd < 0) { ld_pre("FAIL open name="); ld_str(name); ld_endl(); return -1; }
    }

    /* 头 1KB 读进**栈缓冲**：注意 e_phoff 是**文件内偏移**，所以下面 ld_phdr_at 的第二参必须
     * 是这块缓冲自己的地址（不是 0）—— 传 0 会去读 VA 0x40（实测就是这个 #PF：
     * rip… err=5 cr2=0x40）。in-memory 映像（AT_BASE 那份自己）才传真实装载基址。 */
    u8 hdr[1024];
    ld_lseek(fd, 0);
    i64 rd = ld_read(fd, hdr, sizeof(hdr));
    if (rd < 64) { ld_close(fd); ld_fail("so: short read"); }
    const Ehdr* eh = (const Ehdr*)hdr;
    if (!(eh->e_ident[0] == 0x7f && eh->e_ident[1] == 'E' && eh->e_ident[2] == 'L' && eh->e_ident[3] == 'F'))
        ld_fail("so: bad magic");
    if (eh->e_phentsize != 56 || eh->e_phnum == 0 || eh->e_phnum > 16) ld_fail("so: phdr");
    if ((u64)64 + (u64)eh->e_phnum * 56u > (u64)rd) ld_fail("so: phdr not in first KB");

    /* span = max(p_vaddr + p_memsz) */
    u64 span = 0;
    for (u32 i = 0; i < eh->e_phnum; i++) {
        const Phdr* ph = ld_phdr_at(eh, (u64)(unsigned long)(hdr), i, 56);
        if (ph && ph->p_type == PT_LOAD && ph->p_vaddr + ph->p_memsz > span) span = ph->p_vaddr + ph->p_memsz;
    }
    if (span == 0) ld_fail("so: no PT_LOAD");
    span = (span + 0xFFF) & ~0xFFFUL;
    if (g_alloc_top < LD_MMAP_MIN_VA + span + 0x2000UL) ld_fail("so: no address space");
    const u64 base = (g_alloc_top - span) & ~0xFFFUL;          /* 向下分配（顶部是 ld.so 自己） */
    g_alloc_top = base - 0x1000UL;                             /* 留一个保护页 */

    if (g_nobj >= LD_MAX_OBJ) ld_fail("so: too many objects");
    Obj64* o = &g_obj[g_nobj];
    ld_memzero(o, sizeof(Obj64));
    ld_strcpy_n(o->name, name, sizeof(o->name));
    o->base = base;

    /* ---- 整对象**一次** MAP_FIXED（base .. base+span），再逐段读文件 + 清 .bss ----
     * ★ 回归修复（2026-10-06，对应内核 0b68742 把 mmap(9) 的 MAP_FIXED 改成**真 Linux 语义**
     *   "先丢弃旧映射再映"）：原来这里是**逐段**各调一次 MAP_FIXED —— 而多个段的 p_vaddr
     *   经常落在**同一页**（链接器只保证 p_offset ≡ p_vaddr (mod 4096)，不保证段独占页）。
     *   旧内核的 MAP_FIXED 遇到"这一页已经映射好"就复用同一物理页（内容不动），所以逐段
     *   "映射一页 -> 读这一段进去" 的写法看起来能用；一旦 MAP_FIXED 按 Linux 语义真丢弃旧页，
     *   同一页的第二次/第三次 MAP_FIXED 就把**刚读进去**的前几个段整页清成 0。
     *   实测（libfoo.so，span=2 页）：页 0 被段 2/3 各再 MAP_FIXED 一次 -> 丢掉 ELF 头/.dynsym/.
     *   dynstr；页 1 被段 5（p_filesz=0, p_memsz=8）再 MAP_FIXED 一次 -> 丢掉整个 .dynamic
     *   （va 0x1030）。于是 ld_fill_obj64 看不到 DT_SYMTAB/DT_STRTAB：
     *     [LDSO] FAIL obj: no symtab/strtab -> exit 127（[DYNLINK] FAILED reason=code）
     *   修法：**整块映一次**（也是 musl/glibc 的 ld.so 的做法 —— 先整块 map，再逐段读进来），
     *   段内偏移照旧按 p_offset 读；整块映射天然覆盖段间空洞（段间空洞按 Linux 是 0 填，这里
     *   是没写过的新页，也是 0）。不打内核补丁、不改内核的 mmap 语义。 */
    if (ld_mmap_fixed(base, span) != (i64)base) ld_fail("so: mmap");
    for (u32 i = 0; i < eh->e_phnum; i++) {
        const Phdr* ph = ld_phdr_at(eh, (u64)(unsigned long)(hdr), i, 56);
        if (!ph || ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        if (ph->p_filesz) {
            ld_lseek(fd, ph->p_offset);
            const i64 n = ld_read(fd, (void*)(base + ph->p_vaddr), ph->p_filesz);
            if (n != (i64)ph->p_filesz) ld_fail("so: read segment");
        }
        if (ph->p_memsz > ph->p_filesz)
            ld_memzero((void*)(base + ph->p_vaddr + ph->p_filesz), ph->p_memsz - ph->p_filesz);
    }
    /* ---- 权限收紧：**逐页取并集**（与内核 e64_seg_map_union64 同一口径）----
     * 为什么不能"逐段直接 mprotect"：两个权限不同的段可能落在同一页（链接器只保证
     * p_offset ≡ p_vaddr (mod 4096)，不保证每个段独占页）—— 后设的那一段会把前一页的
     * 权限抹掉：若 RW 段最后设置，与该页共享的代码页会丢掉 PROT_EXEC（一执行就 #PF）；
     * 反过来 R|X 段最后设置，数据页会丢掉可写。所以：任一覆盖该页的段可写 -> 可写；
     * 任一段可执行 -> 可执行。 */
    {
        const u64 obj_end = base + span;                        /* span 已页对齐 */
        for (u64 a = base; a < obj_end; a += 0x1000UL) {
            u64 fl = 1 /*PROT_READ*/;
            for (u32 i = 0; i < eh->e_phnum; i++) {
                const Phdr* ph = ld_phdr_at(eh, (u64)(unsigned long)(hdr), i, 56);
                if (!ph || ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
                const u64 s0 = base + (ph->p_vaddr & ~0xFFFUL);
                const u64 s1 = base + ((ph->p_vaddr + ph->p_memsz + 0xFFFUL) & ~0xFFFUL);
                if (a < s0 || a >= s1) continue;
                if (ph->p_flags & 2u) fl |= 2u;                 /* PF_W -> PROT_WRITE */
                if (ph->p_flags & 1u) fl |= 4u;                 /* PF_X -> PROT_EXEC */
            }
            if (ld_mprotect(a, 0x1000UL, fl) != 0) ld_fail("so: mprotect");
        }
    }

    /* ---- .dynamic ---- */
    u64 dyn_va = 0;
    for (u32 i = 0; i < eh->e_phnum; i++) {
        const Phdr* ph = ld_phdr_at(eh, (u64)(unsigned long)(hdr), i, 56);
        if (ph && ph->p_type == PT_DYNAMIC) dyn_va = base + ph->p_vaddr;
    }
    if (!dyn_va) ld_fail("so: no PT_DYNAMIC");
    o->dyn = (const Dyn*)dyn_va;
    ld_fill_obj64(o);
    g_nobj++;

    /* 自己的 RELATIVE 先做（此时 base 已定；符号解析后面统一做） */
    u64 bytes = 0;
    g_rel_n[g_nobj - 1] = ld_apply_relative64(base, o->dyn, &bytes);
    ld_close(fd);

    ld_pre("load name="); ld_str(name);
    ld_str(" base="); ld_hex(base);
    ld_str(" span="); ld_dec(span);
    ld_str(" rela="); ld_dec(bytes);
    ld_str(" relative="); ld_dec(g_rel_n[g_nobj - 1]);
    ld_endl();
    return 0;
}

/* ==================== DT_NEEDED（按名字去重；依赖顺序 = BFS 加载顺序）==================== */
static int ld_obj_by_name64(const char* n) {
    for (int i = 0; i < g_nobj; i++) {
        if (ld_streq(g_obj[i].name, n)) return i;
        if (g_obj[i].soname && ld_streq(g_obj[i].strtab + g_obj[i].soname, n)) return i;
    }
    return -1;
}
static void ld_load_needed64(void) {
    for (int i = 0; i < g_nobj; i++) {
        const Obj64* o = &g_obj[i];
        for (const Dyn* d = o->dyn; d->d_tag != DT_NULL; d++) {
            if (d->d_tag != DT_NEEDED) continue;
            const char* n = o->strtab + d->d_val;
            if (ld_obj_by_name64(n) >= 0) {                    /* 已加载（含"解释器自己"这种自引用） */
                continue;
            }
            ld_pre("need name="); ld_str(n); ld_str(" from="); ld_str(o->name); ld_endl();
            if (ld_load_so64(n) != 0) ld_fail("needed");
        }
    }
}

/* ==================== 初始化（DT_INIT / DT_INIT_ARRAY；依赖先于依赖者）==================== */
static void ld_call_init64(Obj64* o) {
    if (o->init_array && o->init_arraysz) {
        const u64 n = o->init_arraysz / 8u;
        for (u64 i = 0; i < n; i++) {
            const u64 fn = *(const u64*)(o->init_array + i * 8u);
            if (fn) ((void (*)(void))fn)();
        }
        ld_pre("init obj="); ld_str(o->name); ld_str(" init_array="); ld_dec(n); ld_endl();
    }
    if (o->init) {
        ((void (*)(void))o->init)();
        ld_pre("init obj="); ld_str(o->name); ld_str(" DT_INIT=1"); ld_endl();
    }
}

/* ==================== 入口 ==================== */
u64 ldso_main(u64* sp) {
    /* ---- 1) 初始栈：argc/argv/envp/auxv（只碰栈上的数据）---- */
    const u64 argc = sp[0];
    u64* p = sp + 1 + argc + 1;
    while (*p) p++;                                            /* envp */
    p++;
    u64 at_phdr = 0, at_phnum = 0, at_phent = 56, at_base = 0, at_entry = 0, at_pagesz = 4096;
    for (; p[0] != 0; p += 2) {
        switch (p[0]) {
            case 3: at_phdr  = p[1]; break;
            case 4: at_phent = p[1]; break;
            case 5: at_phnum = p[1]; break;
            case 6: at_pagesz = p[1]; break;
            case 7: at_base  = p[1]; break;
            case 9: at_entry = p[1]; break;
            default: break;
        }
    }
    if (!at_base || !at_phdr || !at_phnum) ld_fail("auxv (AT_BASE/AT_PHDR/AT_PHNUM)");
    (void)at_pagesz;

    /* ---- 2) 自定位：自己的 ELF 头就在 AT_BASE（第一个 PT_LOAD 从文件偏移 0 / VA 0 起）----
     * 这一步必须在**碰任何 .data 数据之前**做完：g_self_tab 的初值是"0 + 偏移"，
     * 只有 ld_apply_relative64 把它加上真基址之后才是有效指针。 */
    const Ehdr* self_eh = (const Ehdr*)at_base;
    if (self_eh->e_phentsize != 56) ld_fail("self: phdr");
    const Dyn* self_dyn = ld_find_dynamic64(self_eh, at_base);
    if (!self_dyn) ld_fail("self: PT_DYNAMIC");
    u64 self_bytes = 0;
    const u64 self_rel = ld_apply_relative64(at_base, self_dyn, &self_bytes);

    g_at_base   = at_base;
    g_alloc_top = at_base;                                     /* 库从 ld.so 下方开始分配 */

    ld_pre("start base="); ld_hex(at_base);
    ld_str(" entry="); ld_hex(at_entry);
    ld_str(" self_rela="); ld_dec(self_bytes);
    ld_str(" self_relative="); ld_dec(self_rel);
    ld_endl();

    /* 自定位证据：`g_self_tab[]` 是 .data 里的两个指针（链接期 = "0 + 偏移"），需要用
     * R_X86_64_RELATIVE 在装载期修正；元素 1 还带 +5 的加数（顺带验证 r_addend）。
     * 为什么用数组而不是单个 static 指针：单个 static const 指针会被 clang 常量折叠掉
     * （连存储都不生成 -> 既没有 .data 也没有重定位）。 */
    if (g_self_tab[0] && ld_streq(g_self_tab[0], LD_SELF_TAG) && g_self_tab[1] == LD_SELF_TAG + 5) {
        ld_pre("self tag=ok value="); ld_str(g_self_tab[0]); ld_endl();
    } else {
        ld_fail("self: relative reloc (g_self_tab)");
    }

    /* ---- 3) 主程序：PT_DYNAMIC + 基址（内核按 p_vaddr 原样装载 -> 基址 = AT_PHDR - PT_PHDR.vaddr）---- */
    Obj64* m = &g_obj[0];
    ld_memzero(m, sizeof(Obj64));
    m->is_main = 1;
    m->base = 0;
    ld_strcpy_n(m->name, "<main>", sizeof(m->name));
    /* 两遍扫程序头：PT_PHDR 先定基址，PT_DYNAMIC 用 base 换算（顺序无关） */
    for (u64 i = 0; i < at_phnum; i++) {
        const Phdr* ph = (const Phdr*)((const u8*)at_phdr + i * at_phent);
        if (ph->p_type == PT_PHDR) m->base = at_phdr - (ph->p_vaddr);
    }
    for (u64 i = 0; i < at_phnum; i++) {
        const Phdr* ph = (const Phdr*)((const u8*)at_phdr + i * at_phent);
        if (ph->p_type == PT_DYNAMIC) m->dyn = (const Dyn*)(m->base + ph->p_vaddr);
    }
    if (!m->dyn) ld_fail("main: no PT_DYNAMIC (静态程序不该由 ld.so 启动)");
    ld_fill_obj64(m);
    g_nobj = 1;
    {
        u64 bytes = 0;
        g_rel_n[0] = ld_apply_relative64(m->base, m->dyn, &bytes);
        ld_pre("main base="); ld_hex(m->base);
        ld_str(" rela="); ld_dec(bytes);
        ld_str(" relative="); ld_dec(g_rel_n[0]);
        ld_str(" syms="); ld_dec(m->nsym);
        ld_endl();
    }

    /* ---- 4) DT_NEEDED：先加载（每个 .so 自己的 RELATIVE 在加载时做完）---- */
    ld_load_needed64();

    /* ---- 5) 重定位：GLOB_DAT / JUMP_SLOT / 64（符号解析顺序 = 主程序 -> 依赖 -> 弱符号）---- */
    u64 n_glob = 0, n_jump = 0, n_abs = 0;
    for (int i = 0; i < g_nobj; i++) {
        Obj64* o = &g_obj[i];
        ld_reloc_table64(o, o->rela, o->relasz, &n_glob, &n_jump, &n_abs);
        ld_reloc_table64(o, o->jmprel, o->pltrelsz, &n_glob, &n_jump, &n_abs);
    }
    ld_pre("reloc done relative=");
    { u64 t = 0; for (int i = 0; i < g_nobj; i++) t += g_rel_n[i]; ld_dec(t); }
    ld_str(" glob_dat="); ld_dec(n_glob);
    ld_str(" jump_slot="); ld_dec(n_jump);
    ld_str(" abs64="); ld_dec(n_abs);
    ld_str(" objs="); ld_dec((u64)g_nobj);
    ld_endl();

    /* ---- 6) 初始化：依赖（加载顺序）先，主程序最后 ---- */
    for (int i = 1; i < g_nobj; i++) ld_call_init64(&g_obj[i]);
    ld_call_init64(&g_obj[0]);

    /* ---- 7) 交给主程序入口（_dlstart 会把 rsp 恢复成内核给的初始栈）---- */
    const u64 entry = at_entry;                 /* 内核给的 AT_ENTRY = 主程序入口（不是 ld.so 的） */
    ld_pre("jump main entry="); ld_hex(entry);
    ld_str(" argc="); ld_dec(argc);
    ld_endl();
    return entry;
}

/* ==================== .data：自定位要修的指针 + 汇编桩要的初始 rsp ====================
 * g_self_tab[] 的初值是地址常量 -> 链接器在 .data 里写的是"链接基址 0 + 偏移"，
 * 并生成 R_X86_64_RELATIVE；自定位（ld_apply_relative64）把它们改成真地址后才可读。
 * 这正是"ld.so 自己也要被重定位"的证据（打点 [LDSO] self tag=ok …）。
 * 用数组而不是单个 static const 指针：后者会被 clang 常量折叠掉（没有存储就没有重定位）。 */
static const char LD_SELF_TAG[] = "ldso-self";
static const char* volatile g_self_tab[2] = { LD_SELF_TAG, LD_SELF_TAG + 5 };
u64 ldso_saved_sp __attribute__((visibility("hidden"))) = 0;
