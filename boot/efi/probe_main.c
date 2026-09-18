// boot/efi/probe_main.c - 诊断用 EFI 应用：LoadImage 目标文件并把真实 EFI_STATUS 写进硬盘扇区
//
// 背景：UEFI Shell 只说 "is not an image"，不给状态码；串口/控制台输出会被固件终端驱动改写，
// 光盘上的 ESP 又是只读的（写文件会失败）。所以这里改用 **BlockIO 协议直接写目标硬盘的原始
// 扇区**（第 REPORT_LBA 个扇区开始），宿主机读那个偏移就能拿到干净的 ASCII 报告。
//
// 编译：clang --target=x86_64-unknown-windows → lld-link -subsystem:efi_application
#include "efi.h"

#define REPORT_LBA 200

// ---- EFI 协议（本探针额外需要的两个）----
typedef struct {
    u64 Revision; u64 MediaId; u8 RemovableMedia; u8 MediaPresent; u8 LogicalPartition; u8 ReadOnly;
    u8 WriteCaching; u32 BlockSize; u32 IoAlign; u64 LastBlock;
} EFI_BLOCK_IO_MEDIA;
typedef struct {
    u64 Revision; EFI_BLOCK_IO_MEDIA* Media; void* Reset; void* ReadBlocks; void* WriteBlocks; void* FlushBlocks;
} EFI_BLOCK_IO_PROTOCOL;

EFI_GUID gEfiBlockIoProtocolGuid = { 0x964E5B21, 0x6459, 0x11D2, { 0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B } };
EFI_GUID gEfiSimpleFileSystemProtocolGuid = { 0x964E5B22, 0x6459, 0x11D2, { 0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B } };
EFI_GUID gEfiLoadedImageProtocolGuid = { 0x5B1B31A1, 0x9562, 0x11D2, { 0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B } };

// ---- 报告缓冲（先攒在内存里，最后一次性写盘）----
static char g_rep[2048];
static int  g_len;
static void rep_str(const char* s) { while (*s && g_len < (int)sizeof(g_rep) - 1) g_rep[g_len++] = *s++; }
static void rep_hex(u64 v) {
    static const char* d = "0123456789ABCDEF";
    char b[3] = { 0, 0, 0 };
    rep_str("0x");
    int started = 0;
    for (int i = 15; i >= 0; i--) {
        int nib = (int)((v >> (i * 4)) & 0xF);
        if (nib || started || i == 0) { b[0] = d[nib]; rep_str(b); started = 1; }
    }
}
static void rep_num(u64 v) { char b[24]; int n = 0; if (!v) { rep_str("0"); return; } while (v) { b[n++] = (char)('0' + v % 10); v /= 10; } while (n--) { char c[2] = { b[n], 0 }; rep_str(c); } }

static EFI_BOOT_SERVICES* g_bs;
static EFI_BLOCK_IO_PROTOCOL* g_disk;
static EFI_FILE_PROTOCOL* g_root;

// 找一个可写的整盘（不是光盘、有介质、容量足够大）
static void find_disk(EFI_HANDLE image) {
    EFI_HANDLE* bufs = 0;
    u64 n = 0;
    // 注意参数顺序：(SearchType, Protocol, SearchKey, NoHandles, Buffer)
    EFI_STATUS s = ((EFI_STATUS(*)(u32, EFI_GUID*, void*, u64*, EFI_HANDLE**))g_bs->LocateHandleBuffer)(
        2 /*ByProtocol*/, &gEfiBlockIoProtocolGuid, 0, &n, &bufs);
    if (EFI_ERROR(s) || !bufs) { rep_str("REPORT: LocateHandleBuffer(BlockIo) 失败\n"); return; }
    for (u64 i = 0; i < n; i++) {
        if (bufs[i] == image) continue;
        EFI_BLOCK_IO_PROTOCOL* bio = 0;
        if (EFI_ERROR(((EFI_STATUS(*)(EFI_HANDLE, EFI_GUID*, void**))g_bs->HandleProtocol)(
                bufs[i], &gEfiBlockIoProtocolGuid, (void**)&bio)) || !bio || !bio->Media) continue;
        EFI_BLOCK_IO_MEDIA* m = bio->Media;
        if (!m->MediaPresent) continue;
        rep_str("REPORT: disk? last="); rep_num(m->LastBlock);
        rep_str(" blk="); rep_num(m->BlockSize);
        rep_str(" ro="); rep_num(m->ReadOnly);
        rep_str(" rm="); rep_num(m->RemovableMedia);
        rep_str("\n");
        if (m->ReadOnly && !g_disk) continue;                  // 只读的（光盘）不要
        if (m->ReadOnly) continue;
        if (m->LastBlock < 1000) continue;                     // 太小的不要
        g_disk = bio;
        rep_str("REPORT: 选中可写盘 last="); rep_num(m->LastBlock); rep_str("\n");
        break;
    }
}

static void report_flush(void) {
    if (!g_disk || !g_disk->Media) return;
    u32 bs = g_disk->Media->BlockSize ? g_disk->Media->BlockSize : 512;
    static char buf[4096];
    for (u32 i = 0; i < sizeof(buf); i++) buf[i] = 0;
    for (int i = 0; i < g_len && i < (int)sizeof(buf) - 1; i++) buf[i] = g_rep[i];
    u32 nblk = (u32)(sizeof(buf) / bs);
    if (nblk == 0) nblk = 1;
    EFI_STATUS s = ((EFI_STATUS(*)(EFI_BLOCK_IO_PROTOCOL*, u32, u64, u32, void*))g_disk->WriteBlocks)(
        g_disk, (u32)(g_disk->Media->MediaId), REPORT_LBA, nblk, buf);
    if (EFI_ERROR(s)) {
        // 失败就再报告一次（下次读盘能看到）
        rep_str("REPORT: WriteBlocks 失败 status=");
        rep_hex(s);
        rep_str("\n");
    }
}

static u64 read_whole(EFI_FILE_PROTOCOL* root, const char* name, u8* buf, u64 cap) {
    char nm[64];
    for (int i = 0; i < 63; i++) nm[i] = name[i] ? name[i] : 0;
    EFI_FILE_PROTOCOL* f = 0;
    EFI_STATUS s = ((EFI_STATUS(*)(EFI_FILE_PROTOCOL*, EFI_FILE_PROTOCOL**, char*, u64, u64))root->Open)(
        root, &f, nm, 1, 0);
    if (EFI_ERROR(s) || !f) return 0;
    u64 got = cap;
    s = ((EFI_STATUS(*)(EFI_FILE_PROTOCOL*, u64*, void*))f->Read)(f, &got, buf);
    ((EFI_STATUS(*)(EFI_FILE_PROTOCOL*))f->Close)(f);
    if (EFI_ERROR(s)) return 0;
    return got;
}

static u8 g_buf[64 * 1024];

static void try_load(EFI_HANDLE me, const char* name) {
    rep_str("REPORT: try "); rep_str(name);
    u64 n = read_whole(g_root, name, g_buf, sizeof(g_buf));
    rep_str(" size="); rep_num(n);
    if (n == 0) { rep_str(" (read fail)\n"); return; }
    EFI_HANDLE out = 0;
    EFI_STATUS s = ((EFI_STATUS(*)(u8, EFI_HANDLE, void*, void*, u64, EFI_HANDLE*))g_bs->LoadImage)(
        0, me, 0, g_buf, n, &out);
    rep_str(" LoadImage="); rep_hex(s);
    rep_str(EFI_ERROR(s) ? " FAIL\n" : " OK\n");
}

EFI_STATUS EFIAPI probe_main(EFI_HANDLE image, EFI_SYSTEM_TABLE* st) {
    g_bs = st->BootServices;
    rep_str("==== Vimtu64 PROBE (LoadImage 诊断) ====\n");

    EFI_LOADED_IMAGE_PROTOCOL* li = 0;
    EFI_STATUS s = ((EFI_STATUS(*)(EFI_HANDLE, EFI_GUID*, void**))g_bs->HandleProtocol)(
        image, &gEfiLoadedImageProtocolGuid, (void**)&li);
    if (EFI_ERROR(s) || !li) { rep_str("ERR: no LoadedImage\n"); report_flush(); return 1; }
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = 0;
    s = ((EFI_STATUS(*)(EFI_HANDLE, EFI_GUID*, void**))g_bs->HandleProtocol)(
        li->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (void**)&fs);
    if (EFI_ERROR(s) || !fs) { rep_str("ERR: no SimpleFS\n"); report_flush(); return 1; }
    s = ((EFI_STATUS(*)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL*, EFI_FILE_PROTOCOL**))fs->OpenVolume)(fs, &g_root);
    if (EFI_ERROR(s) || !g_root) { rep_str("ERR: no volume\n"); report_flush(); return 1; }
    rep_str("ESP 挂载 OK\n");

    find_disk(image);
    try_load(image, "TESTA.EFI");
    try_load(image, "TESTB.EFI");
    try_load(image, "PROBE.EFI");
    try_load(image, "BOOTX64.EFI");
    rep_str("==== 结束 ====\n");
    report_flush();
    return 0;
}
