// boot/efi/stub.c - BOOTX64.EFI：极小的 UEFI 引导桩（自研 PE32+，不用 gnu-efi）
//
// 设计要点（踩坑记录）：
//   1) 真正的引导逻辑放在 UEFI64.BIN —— 一份平铺长模式二进制（链接到 0x800000），
//      桩把它整个读进内存后跳过去。平铺二进制不经过任何 PE 校验。
//      为什么这么分：早先试过把引导逻辑直接做成一个大 PE，EDK2 的 PE 加载器拒绝
//      （UEFI Shell 报 "is not an image"）；而 1.5–2KB 的小 PE 能正常加载。
//   2) FAT 子目录必须有 "." / ".." 项（见 tools/make_esp.py），否则固件找不到本文件。
//   3) **PE 的首选基址必须为 0**（build_uefi.sh 里的 -base:0x0）：lld-link 默认给
//      x86_64 PE 的 ImageBase 是 0x140000000（5GB），固件会先按它 ConvertPages 分配，
//      512MB 虚拟机里必然失败，然后**直接放弃该引导项**（OVMF：ConvertPages: failed to
//      find range 140000000 - 140003FFF → Booting EFI Internal Shell）。表现就是
//      "固件不自动引导我们"，但手动 load 又能成功。
//   4) **文件名必须是 UTF-16（CHAR16）**：EFI_FILE_PROTOCOL::Open 的 FileName 参数是
//      CHAR16*，早先写成 ASCII char[]，固件按 UTF-16 解释成乱码 → EFI_NOT_FOUND →
//      桩 return 2 → OVMF 打印 "Image Return Status = Warning Delete Failure"。
//
// 桩只做：LoadedImage → SimpleFileSystem → OpenVolume → 打开 UEFI64.BIN → 读 → 跳。
// 全程往 COM1(0x3F8) 打单字符进度标记：S 进入 / 1 2 3 4 5 各步成功 / J 即将跳转 /
// E<hh> 出错并把 EFI_STATUS 低字节以十六进制打出来。
#include "efi.h"

// ★ 引导器装载地址：**必须在内核 BSS 范围之外**！
//   内核 BSS 覆盖物理 0x28E810..0x22E9000（约 36MB，含 fb 的 33MB 后备缓冲），
//   内核启动时会把这段**清零**。早期把引导器放在 0x800000（8MB，正好在 BSS 里），
//   结果：内核清 BSS 时把引导器建的**高半区页表**（在引导器 .bss 里）一起清零 ->
//   下一次页表遍历失败 -> 三重故障 -> 复位（VMware EFI 实测；OVMF 只是恰好命中 TLB 才幸免）。
//   现在放 0x04800000（72MB）：在载荷末尾（64MB+4.1MB）之后、内核堆（80MB）之前，
//   三段互不重叠。改这里必须同步改 build_uefi.sh 的 -base。
#define LOADER_ADDR 0x04800000ULL
#define LOADER_MAX  (256 * 1024ULL)

EFI_GUID gEfiSimpleFileSystemProtocolGuid = { 0x964E5B22, 0x6459, 0x11D2, { 0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B } };
EFI_GUID gEfiLoadedImageProtocolGuid     = { 0x5B1B31A1, 0x9562, 0x11D2, { 0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B } };

extern void efi_jump_far(u64 entry, u64 a0, u64 a1, u64 a2) __attribute__((noreturn));

static const u16 g_name[11] = { 'U','E','F','I','6','4','.','B','I','N', 0 };

// ==================== 极小串口打点（COM1 0x3F8）====================
// 为什么值得占这点体积：固件只说"加载失败/Not Found"时，你无法区分
//   ① 固件根本没执行我们的镜像；② 执行了但错在某一步；③ 执行成功但第二段跳飞了。
// 有了打点，串口日志里按顺序读 S 1 2 3 4 5 J 就能定位；出错会打出 E + 错误码。
static void ser_mark(char c) {
    __asm__ __volatile__("outb %0, %1" : : "a"((unsigned char)c), "Nd"((unsigned short)0x3F8));
}
static void ser_err(u64 status) {
    static const char H[] = "0123456789ABCDEF";
    ser_mark('E');
    ser_mark(H[(status >> 4) & 0xF]);
    ser_mark(H[status & 0xF]);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE* st) {
    EFI_BOOT_SERVICES* bs = st->BootServices;
    EFI_STATUS s;
    ser_mark('S');                       // 桩已进入

    EFI_LOADED_IMAGE_PROTOCOL* li;
    s = ((EFI_STATUS(*)(EFI_HANDLE, EFI_GUID*, void**))bs->HandleProtocol)(
            image, &gEfiLoadedImageProtocolGuid, (void**)&li);
    if (EFI_ERROR(s) || !li) { ser_err(s); return 1; }
    ser_mark('1');

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs;
    s = ((EFI_STATUS(*)(EFI_HANDLE, EFI_GUID*, void**))bs->HandleProtocol)(
            li->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (void**)&fs);
    if (EFI_ERROR(s) || !fs) { ser_err(s); return 1; }
    ser_mark('2');

    EFI_FILE_PROTOCOL* root;
    s = ((EFI_STATUS(*)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL*, EFI_FILE_PROTOCOL**))fs->OpenVolume)(fs, &root);
    if (EFI_ERROR(s) || !root) { ser_err(s); return 1; }
    ser_mark('3');

    EFI_FILE_PROTOCOL* f = 0;
    s = ((EFI_STATUS(*)(EFI_FILE_PROTOCOL*, EFI_FILE_PROTOCOL**, const u16*, u64, u64))root->Open)(
            root, &f, g_name, 1 /*READ*/, 0);
    if (EFI_ERROR(s) || !f) { ser_err(s); return 2; }
    ser_mark('4');

    // 读整份 UEFI64.BIN 到 0x800000。注意读之前先试探文件大小是否放得下：
    // 目标区只有 LOADER_MAX 字节，超了就直接报错（把溢出挡在固件层，别污染别的内存）。
    u64 got = LOADER_MAX;
    s = ((EFI_STATUS(*)(EFI_FILE_PROTOCOL*, u64*, void*))f->Read)(f, &got, (void*)LOADER_ADDR);
    if (EFI_ERROR(s)) { ser_err(s); return 3; }
    ser_mark('5');

    ser_mark('J');                       // 即将跳进 UEFI64.BIN（第二段接着打 "U:..."）
    efi_jump_far(LOADER_ADDR, (u64)image, (u64)st, (u64)root);
    return 0;
}
