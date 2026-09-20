// boot/efi/uefi64.c - UEFI 引导器主体（**平铺长模式二进制** UEFI64.BIN，链接在 0x800000）
//
// 由 BOOTX64.EFI（极小的 PE 桩）读进 0x800000 后跳进来执行。这样我们的引导逻辑完全不经过
// EDK2 的 PE 校验（实测它拒绝我们那个 ~8KB 的 PE，而 1.5KB 的小 PE 正常），但依然是
// **全 64 位**：UEFI 固件 → PE 桩 → 这份平铺代码 → 内核，没有任何 16/32 位代码。
//
// 它做的事（与 BIOS 路径的引导桩 + loader64 等价）：
//   1) 从 ESP 读 KERNEL64.BIN（4MB 区）→ 0x100000，SYSTEM.IMG（载荷）→ 0x04000000
//      （SYSTEM.IMG 是**安装介质**才有的：装好的盘上它不存在，此时跳过并如实打点 ——
//       系统内核不用载荷；安装介质上它必须在，否则安装程序拿不到载荷）
//   2) GOP 取线性帧缓冲参数 → 写 BootInfo（0x1000）
//   3) UEFI 内存映射 → E820 风格表（0x2000）
//   4) 写"介质描述符"（0x0F00，kind=2 = 载荷已在内存）
//   5) ExitBootServices（自己成为唯一主人）
//   6) 建恒等映射页表（0..512GB，1GB 大页；PML4=0x40000 / PDPT=0x41000，与 loader64 同址）
//   7) efi_enter_kernel(0x100000, 0x40000)：换 GDT/CR3 跳内核 entry64 → kmain64
//   8) 查 EFI 系统表的配置表（ACPI 2.0 GUID 优先，其次 1.0）-> RSDP 物理地址写物理 0x7800
//      （8 字节槽，地址约定见 kernel/memlayout64.h 的 ML64_RSDP_PTR_PHYS），供内核 ACPI 解析；
//      两个 GUID 都没命中就写 0（内核自行扫 legacy 窗口）。这一步打一个 'R' 标记表示
//      "RSDP 查找完成"；已有的打点序列（桩 S12345J / jump64 的 abBcd / 内核入口 EsK）不动。
//
// 串口：全程往 COM1(0x3F8) 打 "U:" 前缀的进度标记（QEMU/VMware 都能在串口日志里看到）。

#include "efi.h"

#define HIGH_KERNEL      0x00100000ULL
#define HIGH_PAYLOAD     0x04000000ULL
#define BOOT_INFO_ADDR   0x1000ULL
#define BOOT_MEM_MAP     0x2000ULL
#define MEDIUM_DESC_ADDR 0x0F00ULL
#define KERNEL_BYTES     (8000ULL * 512ULL)        // 4MB（KERNEL_SECTORS）
#define PAYLOAD_BYTES    (8073ULL * 512ULL)        // system.img 大小
// ★ 内核搬高半区（与 kernel/linker64.ld 的链接基址一致）：
//   虚拟 0xFFFFFFFF80000000 = 物理 0x100000（平铺装载地址），
//   即 VA = KERNEL_VA_BASE + (PA - HIGH_KERNEL)。
#define KERNEL_VA_BASE   0xFFFFFFFF80100000ULL
#define KERNEL_MAP_BYTES (48ULL * 1024 * 1024)      // 直映 PA 0..48MB（内核镜像+BSS 约 36MB）

// ★ RSDP 传递槽（物理地址，8 字节 u64）：UEFI 按规范通过 EFI 配置表交付 RSDP，不放
//   legacy EBDA/0xE0000 窗口。命中项的 VendorTable 写进这里；0 = 没找到。
//   地址必须与 kernel/memlayout64.h 的 ML64_RSDP_PTR_PHYS 一致。
#define RSDP_PTR_PHYS    0x7800ULL

// 高半区的 PDPT 与 PD（放在**我们自己的 .bss**里，任何其它代码都不会碰）。
// 注意：这里**不再**给整机建 0..512GB 的新页表 —— 见下面"只往固件页表挂一项"的说明。
static u64 g_pdpt_hi[512] __attribute__((aligned(4096)));
static u64 g_pd_hi[512]   __attribute__((aligned(4096)));
#define SERIAL_PORT      0x3F8

static inline void outb(u16 port, u8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline u8  inb(u16 port) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }

// GUID 定义（这份是独立的可执行体，需要自己定义）
EFI_GUID gEfiGraphicsOutputProtocolGuid = { 0x9042A9DE, 0x23DC, 0x4A38, { 0x96,0xFB,0x7A,0xDE,0xD0,0x80,0x51,0x6A } };
EFI_GUID gEfiSimpleFileSystemProtocolGuid = { 0x964E5B22, 0x6459, 0x11D2, { 0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B } };
EFI_GUID gEfiLoadedImageProtocolGuid     = { 0x5B1B31A1, 0x9562, 0x11D2, { 0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B } };

static void ser_init(void) {
    outb(SERIAL_PORT + 1, 0x00); outb(SERIAL_PORT + 3, 0x80);
    outb(SERIAL_PORT + 0, 0x01); outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x03); outb(SERIAL_PORT + 2, 0xC7); outb(SERIAL_PORT + 4, 0x0B);
}
static void ser_putc(char c) {
    for (int i = 0; i < 100000; i++) if (inb(SERIAL_PORT + 5) & 0x20) break;
    outb(SERIAL_PORT, (u8)c);
}
static void ser_str(const char* s) { while (*s) { if (*s == '\n') ser_putc('\r'); ser_putc(*s++); } }
static void ser_hex(u64 v) {
    static const char* d = "0123456789ABCDEF";
    char b[17];
    for (int i = 15; i >= 0; i--) { b[i] = d[v & 0xF]; v >>= 4; }
    b[16] = 0;
    const char* p = b; while (p[0] == '0' && p[1]) p++;
    ser_str(p);
}
static void ser_dec(u32 v) {
    char b[12]; int n = 0;
    if (!v) { ser_putc('0'); return; }
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n--) ser_putc(b[n]);
}
static void mem_zero(void* dst, u64 n) { u8* p = (u8*)dst; while (n--) *p++ = 0; }

// BootInfo（与 bootinfo.h 布局一致）
typedef struct {
    u32 magic; u32 lfb_addr; u16 width; u16 height; u8 bpp; u8 pad; u16 pitch;
    u32 mem_entries; u32 mem_map_addr; u32 kernel_size;
    u16 mode_num; u16 mode_count; u32 mode_list; u32 edid_addr; u16 edid_size; u16 edid_ok;
} __attribute__((packed)) BootInfo;

// EFI 配置表项（UEFI 2.x，24 字节）：EFI_GUID + 表指针。EDK2 下表指针就是物理地址。
typedef struct { EFI_GUID VendorGuid; void* VendorTable; } EFI_CONFIGURATION_TABLE;

// 按**内存字节序**比较 16 字节 GUID：ACPI 的 GUID 常量在这里直接按小端展开成 u8[16]，
// 不碰 EFI_GUID 的字段，避免主机字节序/字段序的坑。
static int guid_eq16(const u8* mem, const u8* ref16) {
    for (int i = 0; i < 16; i++) if (mem[i] != ref16[i]) return 0;
    return 1;
}

static u8 g_map_buf[8192] __attribute__((aligned(8)));

extern void efi_enter_kernel(u64 entry, u64 cr3) __attribute__((noreturn));
// 把文件读满 want 字节到物理地址 dst（读不到的尾部清零）。
// required=1：打不开/读失败就当错误（返回 0 且打点）；required=0：文件不存在属正常（返回 0，打一行提示）。
static int load_file(EFI_FILE_PROTOCOL* root, const char* path, u64 dst, u64 want_bytes, int required) {
    // ★ 文件名必须是 UTF-16（CHAR16）！EFI_FILE_PROTOCOL::Open 的 FileName 参数是 CHAR16*，
    //   早先这里直接传 ASCII 的 char[]，固件按 UTF-16 解释成乱码 -> EFI_NOT_FOUND
    //   （串口上表现为 "U:open fail KERNEL64.BIN"）。桩 boot/efi/stub.c 里踩过同一个坑，
    //   这里一并修掉：就地做 ASCII -> UTF-16 的展开。
    u16 nm[64];
    int n = 0;
    for (; n < 63 && path[n]; n++) nm[n] = (u16)(unsigned char)path[n];
    nm[n] = 0;
    EFI_FILE_PROTOCOL* f = 0;
    EFI_STATUS s = ((EFI_STATUS(*)(EFI_FILE_PROTOCOL*, EFI_FILE_PROTOCOL**, const u16*, u64, u64))root->Open)(
        root, &f, nm, 1 /*READ*/, 0);
    if (EFI_ERROR(s) || !f) {
        if (required) { ser_str("U:open fail "); ser_str(path); ser_str("\n"); }
        else          { ser_str("U:missing "); ser_str(path); ser_str(" (optional, skipped)\n"); }
        return 0;
    }
    u64 off = 0;
    while (off < want_bytes) {
        u64 chunk = want_bytes - off;
        if (chunk > 0x100000) chunk = 0x100000;
        u64 got = chunk;
        s = ((EFI_STATUS(*)(EFI_FILE_PROTOCOL*, u64*, void*))f->Read)(f, &got, (void*)(dst + off));
        if (EFI_ERROR(s)) { ser_str("U:read err\n"); return 0; }
        if (got == 0) break;
        off += got;
        if (got < chunk) break;
    }
    if (off < want_bytes) mem_zero((void*)(dst + off), want_bytes - off);
    ((EFI_STATUS(*)(EFI_FILE_PROTOCOL*))f->Close)(f);
    ser_str("U:loaded "); ser_str(path); ser_str(" bytes="); ser_dec((u32)off); ser_str("\n");
    return 1;
}

EFI_STATUS uefi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE* st, EFI_FILE_PROTOCOL* root) {
    ser_init();
    ser_str("\nU:==== Vimtu64 UEFI 引导（自研 BOOTX64.EFI + 平铺长模式引导器）====\n");
    EFI_BOOT_SERVICES* bs = st->BootServices;

    // KERNEL64.BIN 必须有（它是"这次要跑的内核"：安装介质上=安装程序内核，装好的盘上=系统内核）；
    // SYSTEM.IMG（安装载荷）只有安装介质才有 —— 装好的盘上没有它，此时跳过（系统内核不用载荷）。
    if (!load_file(root, "KERNEL64.BIN", HIGH_KERNEL, KERNEL_BYTES, 1)) return 1;
    const int have_payload = load_file(root, "SYSTEM.IMG", HIGH_PAYLOAD, PAYLOAD_BYTES, 0);

    // GOP
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = 0;
    EFI_STATUS s = ((EFI_STATUS(*)(EFI_GUID*, void*, void**))bs->LocateProtocol)(
        &gEfiGraphicsOutputProtocolGuid, 0, (void**)&gop);
    if (EFI_ERROR(s) || !gop || !gop->Mode || !gop->Mode->Info) { ser_str("U:no GOP\n"); return 1; }
    EFI_GOP_MODE_INFO* mi = gop->Mode->Info;
    ser_str("U:gop "); ser_dec(mi->HorizontalResolution); ser_str("x"); ser_dec(mi->VerticalResolution);
    ser_str(" fmt="); ser_dec(mi->PixelFormat);
    ser_str(" lfb="); ser_hex(gop->Mode->FrameBufferBase);
    ser_str(" pitch_px="); ser_dec(mi->PixelsPerScanLine); ser_str("\n");
    if (mi->PixelFormat == 3) { ser_str("U:GOP not linear\n"); return 1; }

    BootInfo* bi = (BootInfo*)BOOT_INFO_ADDR;
    mem_zero(bi, sizeof(*bi));
    bi->magic       = 0x41555231;                 // 'AUR1'
    bi->lfb_addr    = (u32)gop->Mode->FrameBufferBase;
    bi->width       = (u16)mi->HorizontalResolution;
    bi->height      = (u16)mi->VerticalResolution;
    bi->bpp         = 32;
    bi->pitch       = (u16)(mi->PixelsPerScanLine * 4);
    bi->kernel_size = (u32)KERNEL_BYTES;
    bi->mode_num    = (u16)gop->Mode->Mode;
    bi->mode_list   = 0x7400;
    bi->edid_addr   = 0x7600;
    bi->edid_size   = 128;

    // UEFI 内存映射 -> E820 风格
    u64 map_size = sizeof(g_map_buf), map_key = 0, desc_size = 0;
    u32 desc_ver = 0;
    s = ((EFI_STATUS(*)(u64*, void*, u64*, u64*, u32*))bs->GetMemoryMap)(
        &map_size, g_map_buf, &map_key, &desc_size, &desc_ver);
    if (s == 5) {
        map_size = sizeof(g_map_buf);
        s = ((EFI_STATUS(*)(u64*, void*, u64*, u64*, u32*))bs->GetMemoryMap)(
            &map_size, g_map_buf, &map_key, &desc_size, &desc_ver);
    }
    if (EFI_ERROR(s)) { ser_str("U:memmap fail\n"); return 1; }
    {
        u32 n = (u32)(map_size / desc_size), out = 0;
        u8* e820 = (u8*)BOOT_MEM_MAP;
        for (u32 i = 0; i < n && out < 64; i++) {
            EFI_MEMORY_DESCRIPTOR* d = (EFI_MEMORY_DESCRIPTOR*)(g_map_buf + i * desc_size);
            u64 base = d->PhysicalStart, len = d->NumberOfPages * 4096ULL;
            if (!len) continue;
            u32 type = (d->Type == 7) ? 1u : 2u;
            u8* p = e820 + out * 20;
            *(u32*)(p + 0) = (u32)base;   *(u32*)(p + 4) = (u32)(base >> 32);
            *(u32*)(p + 8) = (u32)len;    *(u32*)(p + 12) = (u32)(len >> 32);
            *(u32*)(p + 16) = type;
            out++;
        }
        bi->mem_entries = out;
        bi->mem_map_addr = (u32)BOOT_MEM_MAP;
        ser_str("U:e820 entries="); ser_dec(out); ser_str("\n");
    }

    // 介质描述符（kind=2 = 载荷已在内存；装好的盘上没有载荷 -> kind=0 = "无载荷"，
    // 内核的 part_read_medium_desc 只认 CD/RAM，读到 0 就当没有描述符，行为与 BIOS 裸盘路径一致）
    {
        u8* md = (u8*)MEDIUM_DESC_ADDR;
        mem_zero(md, 32);
        *(u32*)(md + 0)  = 0x444D4D56;            // 'VMMD'
        md[4] = have_payload ? 2 : 0;
        if (have_payload) {
            *(u32*)(md + 8)  = (u32)HIGH_KERNEL;
            *(u32*)(md + 12) = (u32)HIGH_PAYLOAD;
            *(u32*)(md + 16) = (u32)(PAYLOAD_BYTES / 512);
        } else {
            ser_str("U:medium desc kind=0 (no payload: installed disk)\n");
        }
    }
    ser_str("U:bootinfo lfb="); ser_hex(bi->lfb_addr);
    ser_str(" res="); ser_dec(bi->width); ser_str("x"); ser_dec(bi->height);
    ser_str(" pitch="); ser_dec(bi->pitch); ser_str("\n");

    // ---- 页表：为"内核搬高半区"建映射 ----
    // ★ 关键决策（实测驱动）：**不替换固件的页表，只往它当前活动的 PML4 里加一项**。
    //   原因：VMware EFI 下 `mov cr3` 会立刻复位（同一份代码在 OVMF 下正常），
    //   而"新增一个原本未映射的 PML4 项"是立即生效的、也不需要刷 TLB —— 完全绕开 mov cr3。
    //   用 **2MB 大页**（不用 1GB）：BIOS 引导路径长期用 2MB 页且在 VMware 下验证过；
    //   1GB 页在 VMware 的二进制翻译/影子页表下的行为没有把握。
    //   映射内容：**VA = 0xFFFFFFFF80000000 + PA** 的线性直映，覆盖 PA 0..48MB
    //   （内核镜像 + BSS 约 36MB）。因为是直映，这里从 PA 0 开始填；内核在 PA 0x100000，
    //   自然落在 VA 0xFFFFFFFF80100000 —— 正好是它的链接地址（见 linker64.ld）。
    {
        u64 cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        u64* pml4 = (u64*)(unsigned long)(cr3 & ~0xFFFULL);   // 固件**当前活动**的 PML4
        mem_zero(g_pdpt_hi, 4096);
        mem_zero(g_pd_hi, 4096);
        g_pdpt_hi[510] = (u64)(unsigned long)g_pd_hi | 3;     // VA 0xFFFFFFFF80000000 -> PDPT[510]
        {
            const u32 pages = (u32)(KERNEL_MAP_BYTES / 0x200000ULL);   // 24
            for (u32 i = 0; i < pages; i++) {
                g_pd_hi[i] = ((u64)i * 0x200000ULL) | 0x83;   // P|RW|PS，从 PA 0 起（直映）
            }
            ser_str("U:direct map VA 0xFFFFFFFF80000000+PA, bytes=");
            ser_dec(pages);
            ser_str("\n");
        }
        ser_str("U:pml4[511] before="); ser_hex(pml4[511]); ser_str("\n");
        // ★ 固件把自己的页表页标成**只读**（EDK2 的页表保护）：直接写 PML4[511] 会 #PF
        //   （实测：ExceptionData=0x03 = P|W，CR2 正好等于 PML4[511] 的地址）。
        //   所以临时清 CR0.WP（Supervisor Write Protect）再写，写完立刻恢复。
        //   只**新增**一项、不改动已有映射，因此不需要刷 TLB。
        {
            u64 cr0;
            __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
            __asm__ volatile("mov %0, %%cr0" ::"r"(cr0 & ~0x10000ULL));   // WP=0
            pml4[511] = (u64)(unsigned long)g_pdpt_hi | 3;               // ★ 挂进固件页表
            __asm__ volatile("mov %0, %%cr0" ::"r"(cr0));                // 恢复 WP
        }
    }

    // ---- 诊断：把**固件留下的** CPU 状态打出来 ----
    // 为什么必须看这几个寄存器：我们按 4 级页表（PML4，1GB 大页）建表，然后 `mov cr3`。
    // 如果固件开了 5 级分页（CR4.LA57，bit12）或 PCIDE（bit17）等，我们的表会被按
    // 别的格式解释 —— 后果是 `mov cr3` 之后**取指就失败**：无 IDT -> 三重故障 -> 复位，
    // 且**没有任何输出**（VMware EFI 实测就是"进内核即静默复位"，打点停在 'b' 之后就没了）。
    // 有这三行，VMware 与 OVMF 的差异一眼可见。
    {
        u64 cr4v, cr3v, efer;
        u32 lo, hi;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4v));
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3v));
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
        efer = ((u64)hi << 32) | lo;
        ser_str("U:fw cr4="); ser_hex(cr4v);
        ser_str(" efer="); ser_hex(efer);
        ser_str(" cr3="); ser_hex(cr3v);
        ser_str(" cr0=");
        { u64 cr0v; __asm__ volatile("mov %%cr0, %0" : "=r"(cr0v)); ser_hex(cr0v); }
        ser_str("\n");
    }

    // ---- ACPI RSDP：EFI 配置表 -> 物理 0x7800（8 字节 u64，约定见 kernel/memlayout64.h）----
    // UEFI 固件按规范通过 EFI_SYSTEM_TABLE.ConfigurationTable 交付 RSDP（ACPI 2.0 GUID
    // 优先，其次 ACPI 1.0），它不在 legacy EBDA / 0xE0000 窗口里 —— 这正是 UEFI 下内核
    // acpi64 找不到 RSDP 的根因。配置表"有效"的保证到 ExitBootServices 为止，所以在它之前读。
    {
        static const u8 guid_acpi20[16] = {
            0x71,0xE8,0x68,0x88, 0xF1,0xE4, 0xD3,0x11, 0xBC,0x22, 0x00,0x80,0xC7,0x3C,0x88,0x81
        };
        static const u8 guid_acpi10[16] = {
            0x30,0x2D,0x9D,0xEB, 0x88,0x2D, 0xD3,0x11, 0x9A,0x16, 0x00,0x90,0x27,0x3F,0xC1,0x4D
        };
        u64 rsdp = 0;
        const u64 n = st->NumberOfTableEntries;
        const EFI_CONFIGURATION_TABLE* ct = (const EFI_CONFIGURATION_TABLE*)st->ConfigurationTable;
        if (ct) {
            for (int pass = 0; pass < 2 && rsdp == 0; pass++) {
                const u8* want = (pass == 0) ? guid_acpi20 : guid_acpi10;   // 先 2.0，找不到再 1.0
                for (u64 i = 0; i < n; i++) {
                    if (guid_eq16((const u8*)&ct[i].VendorGuid, want)) { rsdp = (u64)ct[i].VendorTable; break; }
                }
            }
        }
        *(volatile u64*)(u64)RSDP_PTR_PHYS = rsdp;   // 0 = 两个 GUID 都没命中，内核自行扫 legacy 窗口
        ser_str("R\n");                              // 打点：RSDP 查找完成
    }
    // ExitBootServices
    for (int attempt = 0; attempt < 4; attempt++) {
        map_size = sizeof(g_map_buf);
        s = ((EFI_STATUS(*)(u64*, void*, u64*, u64*, u32*))bs->GetMemoryMap)(
            &map_size, g_map_buf, &map_key, &desc_size, &desc_ver);
        if (EFI_ERROR(s)) { ser_str("U:memmap2 fail\n"); return 1; }
        s = ((EFI_STATUS(*)(EFI_HANDLE, u64))bs->ExitBootServices)(image, map_key);
        if (!EFI_ERROR(s)) break;
        ser_str("U:ExitBootServices retry\n");
    }
    if (EFI_ERROR(s)) { ser_str("U:ExitBootServices FAILED\n"); return 1; }
    ser_str("U:ExitBootServices ok -> 进内核（全 64 位）\n");

    // ---- 最后一道保险：确认挂进固件页表的高半区映射还在 ----
    // 把"页表被写坏 / 表项被覆盖"这种故障从"静默复位"变成一行明确的日志。
    {
        u64 cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        u64* pml4 = (u64*)(unsigned long)(cr3 & ~0xFFFULL);
        u64 ent = pml4[511];
        u32 bad = 0;
        if (!(ent & 1) || (ent & ~0xFFFULL) != (u64)(unsigned long)g_pdpt_hi) bad++;
        for (u32 i = 0; i < (u32)(KERNEL_MAP_BYTES / 0x200000ULL); i++) {
            if (g_pd_hi[i] != (((u64)i * 0x200000ULL) | 0x83)) { bad++; break; }
        }
        ser_str(bad ? "U:high half check BAD\n" : "U:high half check ok\n");
    }
    ser_str("U:entry="); ser_hex(KERNEL_VA_BASE); ser_str("\n");

    // ★ 准备进内核。注意第二参数已不再使用（**不换 CR3**：VMware EFI 下 mov cr3 会复位；
    //   我们改为往固件页表里挂高半区映射，见上面那段注释）。
    efi_enter_kernel(KERNEL_VA_BASE, 0);
    for (;;) { __asm__ volatile("cli; hlt"); }
}
