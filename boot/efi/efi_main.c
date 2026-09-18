// boot/efi/efi_main.c - Vimtu64 的自研 UEFI 引导程序（BOOTX64.EFI）
//
// 目标：**完全 64 位**的引导路径，不用 16 位实模式、也不用 32 位保护模式（除 UEFI 固件自己）。
// 不依赖 gnu-efi：EFI 结构体在 boot/efi/efi.h 里自己声明，PE 头由 lld-link 生成。
//
// 它做的事（与 BIOS 路径的引导桩 + loader64 等价）：
//   1) 用 SimpleFileSystem 从自己的启动分区（ESP/FAT）读 KERNEL64.BIN → 0x100000
//      （读满 4MB，与 KERNEL_SECTORS 一致）与 SYSTEM.IMG（载荷）→ 0x04000000
//   2) 用 GOP 拿到线性帧缓冲参数 → 写 BootInfo（0x1000）
//   3) 综合 UEFI 内存映射 → 写 E820 风格表（0x2000，内核照旧打印/统计）
//   4) 写"介质描述符"（0x0F00，kind=2 = 载荷已在内存）——安装程序据此跳过介质扫描
//   5) ExitBootServices（自己成为唯一主人）
//   6) 建恒等映射页表（0..512GB，1GB 大页；PML4=0x40000 / PDPT=0x41000，与 loader64 同址）
//   7) 载入自己的 GDT（CS=0x08 代码 / 0x10 数据，与 entry64.asm 的期望一致），CR3 指向新页表，
//      然后 jmp 0x100000 —— 之后就是内核的 entry64 → kmain64
//
// 串口：全程往 COM1(0x3F8) 打 "U:" 前缀的进度标记，QEMU/VMware 都能在串口日志里看到。

#include "efi.h"

// ---- 与我们其它部分一致的约定（必须与 kernel/memlayout64.h、boot/cdiso.asm 保持一致）----
#define HIGH_KERNEL      0x00100000ULL
#define HIGH_PAYLOAD     0x04000000ULL
#define BOOT_INFO_ADDR   0x1000ULL
#define BOOT_MEM_MAP     0x2000ULL
#define MEDIUM_DESC_ADDR 0x0F00ULL
#define KERNEL_BYTES     (8000ULL * 512ULL)        // 4MB（KERNEL_SECTORS）
#define PAYLOAD_BYTES    (8073ULL * 512ULL)        // system.img 大小
#define PAGE_PML4        0x40000ULL
#define PAGE_PDPT        0x41000ULL
#define SERIAL_PORT      0x3F8

// ---- 串口（直接端口 IO，不依赖固件）----
static inline void outb(u16 port, u8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline u8  inb(u16 port) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }

static void ser_init(void) {
    outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x80);
    outb(SERIAL_PORT + 0, 0x01);          // 115200
    outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x03);
    outb(SERIAL_PORT + 2, 0xC7);
    outb(SERIAL_PORT + 4, 0x0B);
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
    // 去掉前导 0（保持短）
    const char* p = b; while (p[0] == '0' && p[1]) p++;
    ser_str(p);
}
static void ser_dec(u32 v) {
    char b[12]; int n = 0;
    if (!v) { ser_putc('0'); return; }
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n--) ser_putc(b[n]);
}

// ---- GUID ----
EFI_GUID gEfiGraphicsOutputProtocolGuid = { 0x9042A9DE, 0x23DC, 0x4A38, { 0x96,0xFB,0x7A,0xDE,0xD0,0x80,0x51,0x6A } };
EFI_GUID gEfiSimpleFileSystemProtocolGuid = { 0x964E5B22, 0x6459, 0x11D2, { 0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B } };
EFI_GUID gEfiLoadedImageProtocolGuid     = { 0x5B1B31A1, 0x9562, 0x11D2, { 0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B } };

// ---- 简单内存操作 ----
static void mem_zero(void* dst, u64 n) { u8* p = (u8*)dst; while (n--) *p++ = 0; }
static void mem_copy(void* dst, const void* src, u64 n) {
    u8* d = (u8*)dst; const u8* s = (const u8*)src;
    while (n--) *d++ = *s++;
}

// ---- BootInfo（与 bootinfo.h 布局一致）----
typedef struct {
    u32 magic; u32 lfb_addr; u16 width; u16 height; u8 bpp; u8 pad; u16 pitch;
    u32 mem_entries; u32 mem_map_addr; u32 kernel_size;
    u16 mode_num; u16 mode_count; u32 mode_list; u32 edid_addr; u16 edid_size; u16 edid_ok;
} __attribute__((packed)) BootInfo;

// 内存映射缓冲：UEFI 的内存描述符每项 40 字节，几百项也就几 KB，8KB 足够。
// ★ 曾经用 256KB 静态数组：它会让 PE 里出现一个"VirtualSize 远大于文件内容"的 .data 段，
//   EDK2 的 PE 加载器会拒绝这种镜像（实测 UEFI Shell 报 "is not an image"）。
//   换小之后固件就能正常 LoadImage 了。
#define MAP_BUF_BYTES 8192
static u8 g_map_buf[MAP_BUF_BYTES] __attribute__((aligned(8)));
static char g_path[64];

extern void efi_enter_kernel(u64 entry, u64 cr3, u64 gdt_desc_ptr) __attribute__((noreturn));

// ---- 读整个文件到物理地址 ----
static int load_file(EFI_FILE_PROTOCOL* root, const char* path, u64 dst, u64 want_bytes) {
    mem_zero(g_path, sizeof(g_path));
    for (int i = 0; path[i] && i < 60; i++) g_path[i] = path[i];     // 已经是 ASCII
    EFI_FILE_PROTOCOL* f = 0;
    EFI_STATUS s = ((EFI_STATUS(*)(EFI_FILE_PROTOCOL*, EFI_FILE_PROTOCOL**, char*, u64, u64))
                    root->Open)(root, &f, g_path, 1 /*READ*/, 0);
    if (EFI_ERROR(s) || !f) { ser_str("U:open fail "); ser_str(path); ser_str(" st="); ser_hex(s); ser_str("\n"); return 0; }
    u64 off = 0;
    while (off < want_bytes) {
        u64 chunk = want_bytes - off;
        if (chunk > 0x100000) chunk = 0x100000;                       // 每次最多 1MB
        u64 got = chunk;
        s = ((EFI_STATUS(*)(EFI_FILE_PROTOCOL*, u64*, void*))f->Read)(f, &got, (void*)(dst + off));
        if (EFI_ERROR(s)) { ser_str("U:read err "); ser_hex(s); ser_str("\n"); return 0; }
        if (got == 0) break;                                          // 文件读完了
        off += got;
        if (got < chunk) break;
    }
    // 关键：文件比 want_bytes 短时**必须把剩余部分清零** —— 那块内存由固件分配过，可能残留垃圾；
    // 内核镜像区若带脏字节，加载进来的静态数据就是坏的（BIOS 路径靠"读满固定扇区数"天然避免）。
    if (off < want_bytes) mem_zero((void*)(dst + off), want_bytes - off);
    ((EFI_STATUS(*)(EFI_FILE_PROTOCOL*))f->Close)(f);
    ser_str("U:loaded "); ser_str(path); ser_str(" bytes="); ser_dec((u32)off); ser_str("\n");
    return 1;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE* st) {
    ser_init();
    ser_str("\nU:==== Vimtu64 UEFI boot (hand-written BOOTX64.EFI, no gnu-efi) ====\n");

    EFI_BOOT_SERVICES* bs = st->BootServices;

    // ---- 1) 自己的启动卷（ESP）根目录 ----
    EFI_LOADED_IMAGE_PROTOCOL* li = 0;
    EFI_STATUS s = ((EFI_STATUS(*)(EFI_HANDLE, EFI_GUID*, void**))bs->HandleProtocol)(
        image, &gEfiLoadedImageProtocolGuid, (void**)&li);
    if (EFI_ERROR(s) || !li) { ser_str("U:no loadedimage\n"); return 1; }
    ser_str("U:loadedimage ok dev="); ser_hex((u64)li->DeviceHandle); ser_str("\n");

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = 0;
    s = ((EFI_STATUS(*)(EFI_HANDLE, EFI_GUID*, void**))bs->HandleProtocol)(
        li->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (void**)&fs);
    if (EFI_ERROR(s) || !fs) { ser_str("U:no simplefs\n"); return 1; }
    EFI_FILE_PROTOCOL* root = 0;
    s = ((EFI_STATUS(*)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL*, EFI_FILE_PROTOCOL**))fs->OpenVolume)(fs, &root);
    if (EFI_ERROR(s) || !root) { ser_str("U:no volume\n"); return 1; }
    ser_str("U:esp mounted\n");

    // ---- 2) 内核（4MB 区）与载荷 ----
    if (!load_file(root, "KERNEL64.BIN", HIGH_KERNEL, KERNEL_BYTES)) return 1;
    if (!load_file(root, "SYSTEM.IMG", HIGH_PAYLOAD, PAYLOAD_BYTES)) return 1;

    // ---- 3) GOP：线性帧缓冲 ----
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = 0;
    s = ((EFI_STATUS(*)(EFI_GUID*, void*, void**))bs->LocateProtocol)(
        &gEfiGraphicsOutputProtocolGuid, 0, (void**)&gop);
    if (EFI_ERROR(s) || !gop || !gop->Mode || !gop->Mode->Info) { ser_str("U:no GOP\n"); return 1; }
    EFI_GOP_MODE_INFO* mi = gop->Mode->Info;
    ser_str("U:gop "); ser_dec(mi->HorizontalResolution); ser_str("x"); ser_dec(mi->VerticalResolution);
    ser_str(" fmt="); ser_dec(mi->PixelFormat);
    ser_str(" lfb="); ser_hex(gop->Mode->FrameBufferBase);
    ser_str(" pitch_px="); ser_dec(mi->PixelsPerScanLine); ser_str("\n");
    if (mi->PixelFormat == 3 /*BltOnly*/) { ser_str("U:GOP not linear\n"); return 1; }
    // GOP 的 RGBx/BGRx 都是 32bpp；bitmask 模式也按 32bpp 处理（我们按 32bpp 写，颜色通道顺序见 fb.cpp）
    u32 bpp = 32;
    u32 pitch = mi->PixelsPerScanLine * 4;

    // ---- 4) BootInfo + 内存映射 + 介质描述符 ----
    BootInfo* bi = (BootInfo*)BOOT_INFO_ADDR;
    mem_zero(bi, sizeof(*bi));
    bi->magic       = 0x41555231;                 // 'AUR1'
    bi->lfb_addr    = (u32)gop->Mode->FrameBufferBase;
    bi->width       = (u16)mi->HorizontalResolution;
    bi->height      = (u16)mi->VerticalResolution;
    bi->bpp         = (u8)bpp;
    bi->pitch       = (u16)pitch;
    bi->kernel_size = (u32)KERNEL_BYTES;
    bi->mode_num    = (u16)gop->Mode->Mode;
    bi->mode_count  = 0;
    bi->mode_list   = 0x7400;
    bi->edid_addr   = 0x7600;
    bi->edid_size   = 128;
    bi->edid_ok     = 0;
    u64 map_size = sizeof(g_map_buf), map_key = 0, desc_size = 0;
    u32 desc_ver = 0;

    // UEFI 内存映射 → E820 风格（type 1 = 可用内存；其余归为 2 = 保留）
    // 先拿一次大小，再拿一次内容。Key 要留到 ExitBootServices。
    s = ((EFI_STATUS(*)(u64*, void*, u64*, u64*, u32*))bs->GetMemoryMap)(
        &map_size, g_map_buf, &map_key, &desc_size, &desc_ver);
    if (s == 5 /*BUFFER_TOO_SMALL*/) {
        map_size = sizeof(g_map_buf);
        s = ((EFI_STATUS(*)(u64*, void*, u64*, u64*, u32*))bs->GetMemoryMap)(
            &map_size, g_map_buf, &map_key, &desc_size, &desc_ver);
    }
    if (EFI_ERROR(s)) { ser_str("U:memmap fail\n"); return 1; }
    {
        u32 n = (u32)(map_size / desc_size);
        u32 out = 0;
        u8* e820 = (u8*)BOOT_MEM_MAP;
        for (u32 i = 0; i < n && out < 64; i++) {
            EFI_MEMORY_DESCRIPTOR* d = (EFI_MEMORY_DESCRIPTOR*)(g_map_buf + i * desc_size);
            u64 base = d->PhysicalStart;
            u64 len  = d->NumberOfPages * 4096ULL;
            if (!len) continue;
            u32 type = (d->Type == 7 /*EfiConventionalMemory*/) ? 1u : 2u;
            u8* p = e820 + out * 20;
            *(u32*)(p + 0)  = (u32)base;
            *(u32*)(p + 4)  = (u32)(base >> 32);
            *(u32*)(p + 8)  = (u32)len;
            *(u32*)(p + 12) = (u32)(len >> 32);
            *(u32*)(p + 16) = type;
            out++;
        }
        bi->mem_entries  = out;
        bi->mem_map_addr = (u32)BOOT_MEM_MAP;
        ser_str("U:e820 entries="); ser_dec(out); ser_str("\n");
    }

    // 介质描述符（0x0F00）：kind=2 = 载荷已在内存，安装程序不必碰介质
    {
        u8* md = (u8*)MEDIUM_DESC_ADDR;
        mem_zero(md, 32);
        *(u32*)(md + 0)  = 0x444D4D56;            // 'VMMD'
        md[4] = 2;                                // kind = RAM
        md[5] = 0;                                // drive（UEFI 没有 BIOS 盘号）
        *(u32*)(md + 8)  = (u32)HIGH_KERNEL;      // kernel 物理地址
        *(u32*)(md + 12) = (u32)HIGH_PAYLOAD;     // 载荷物理地址
        *(u32*)(md + 16) = (u32)(PAYLOAD_BYTES / 512);
        *(u32*)(md + 20) = 0;
    }

    ser_str("U:bootinfo lfb="); ser_hex(bi->lfb_addr);
    ser_str(" res="); ser_dec(bi->width); ser_str("x"); ser_dec(bi->height);
    ser_str(" pitch="); ser_dec(bi->pitch); ser_str("\n");

    // ---- 5) 页表：0..512GB 恒等映射（1GB 大页）----
    // PML4[0] -> PDPT；PDPT[0..511] = 1GB 大页（PS=1，flags=0x83 = 存在+可写+大页）
    {
        u64* pml4 = (u64*)PAGE_PML4;
        u64* pdpt = (u64*)PAGE_PDPT;
        mem_zero(pml4, 4096);
        pml4[0] = PAGE_PDPT | 3;
        for (int i = 0; i < 512; i++) pdpt[i] = ((u64)i * 0x40000000ULL) | 0x83;
    }
    ser_str("U:paging 0..512GB identity\n");

    // ---- 6) ExitBootServices（之后固件不再可用）----
    // 注意：ExitBootServices 可能会改变内存映射，所以必须用"最新一次"的 map_key；
    // 这里循环重试，直到成功（规范推荐做法）。
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
    ser_str("U:ExitBootServices ok -> enter kernel (long mode, 100% 64-bit path)\n");

    // ---- 7) 载入 GDT + 换 CR3 + 跳内核 ----
    efi_enter_kernel(HIGH_KERNEL, PAGE_PML4, 0);
    for (;;) { __asm__ volatile("cli; hlt"); }
}
