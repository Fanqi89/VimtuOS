// boot/efi/efi.h - UEFI 最小定义（自研，不用 gnu-efi）
//
// 只声明我们真正用到的东西：SystemTable / BootServices / GOP / SimpleFileSystem / FileProtocol /
// LoadedImage / 内存描述符。全部按 UEFI 2.x 规范的字段顺序排列（偏移必须完全一致）。
//
// 编译：clang --target=x86_64-unknown-windows（COFF），链接：lld-link -subsystem:efi_application
#pragma once
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t  i64;
typedef void*    EFI_HANDLE;
typedef u64      EFI_STATUS;
typedef void*    EFI_EVENT;
typedef u64      EFI_PHYSICAL_ADDRESS;
typedef u64      EFI_VIRTUAL_ADDRESS;
typedef u64      EFI_TPL;

#define EFIAPI __attribute__((ms_abi))
#define EFI_SUCCESS 0ULL
#define EFI_ERROR(s) ((i64)(s) < 0)

typedef struct { u32 a; u16 b, c; u8 d[8]; } EFI_GUID;

typedef struct {
    u64 Signature; u32 Revision; u32 HeaderSize; u32 CRC32; u32 Reserved;
} EFI_TABLE_HEADER;

// 内存描述符（UEFI 2.x，40 字节）
typedef struct {
    u32 Type; u32 Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    u64 NumberOfPages;
    u64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;
    // 0x18 起（Hdr 24 字节 + 一堆 RaiseTPL 等，必须逐个占位保证偏移正确）
    void* RaiseTPL; void* RestoreTPL;                 // 0x18 0x20
    void* AllocatePages; void* FreePages; void* GetMemoryMap; void* AllocatePool; void* FreePool;
    void* CreateEvent; void* SetTimer; void* WaitForEvent; void* SignalEvent; void* CloseEvent; void* CheckEvent;
    void* InstallProtocolInterface; void* ReinstallProtocolInterface; void* UninstallProtocolInterface;
    void* HandleProtocol; void* Reserved; void* RegisterProtocolNotify;
    void* LocateHandle; void* LocateDevicePath; void* InstallConfigurationTable;
    void* LoadImage; void* StartImage; void* Exit; void* UnloadImage; void* ExitBootServices;   // ExitBootServices = 0xE0
    void* GetNextMonotonicCount; void* Stall; void* SetWatchdogTimer;
    void* ConnectController; void* DisconnectController;
    void* OpenProtocol; void* CloseProtocol; void* OpenProtocolInformation;
    void* ProtocolsPerHandle; void* LocateHandleBuffer; void* LocateProtocol;
    void* InstallMultipleProtocolInterfaces; void* UninstallMultipleProtocolInterfaces;
    void* CalculateCrc32; void* CopyMem; void* SetMem; void* CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    u16* FirmwareVendor; u32 FirmwareRevision; EFI_HANDLE ConsoleInHandle;
    void* ConIn; EFI_HANDLE ConsoleOutHandle; void* ConOut;
    EFI_HANDLE StandardErrorHandle; void* StdErr; void* RuntimeServices; EFI_BOOT_SERVICES* BootServices;
    u64 NumberOfTableEntries; void* ConfigurationTable;
} EFI_SYSTEM_TABLE;

// ---- 图形输出协议（GOP）----
typedef struct { u32 RedMask, GreenMask, BlueMask, ReservedMask; } EFI_PIXEL_BITMASK;
typedef struct {
    u32 Version; u32 HorizontalResolution; u32 VerticalResolution;
    u32 PixelFormat;                        // 0=RGBx 1=BGRx 2=BitMask 3=BltOnly
    EFI_PIXEL_BITMASK PixelInformation;
    u32 PixelsPerScanLine;
} EFI_GOP_MODE_INFO;
typedef struct {
    u32 MaxMode; u32 Mode; EFI_GOP_MODE_INFO* Info; u64 SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase; u64 FrameBufferSize;
} EFI_GOP_MODE;
// ★ GOP 协议有**四个**成员：QueryMode / SetMode / **Blt** / Mode（偏移 0/8/16/24）。
//   早先这里漏了 Blt，于是 gop->Mode 实际读到的是 Blt 这个函数指针（偏移 16），
//   把它当 EFI_GOP_MODE 用 -> Mode->Info 拿到的是代码字节当指针 -> 读
//   mi->HorizontalResolution 时 #GP（固件转储：RIP=0x8010FB 处 mov 0x4(%rdi),%ecx，
//   RDI=0xCD894D5541C68945 正是几条指令的机器码）。补上 Blt 即可。
typedef struct {
    void* QueryMode; void* SetMode; void* Blt; EFI_GOP_MODE* Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

// ---- 简单文件系统 / 文件协议 ----
typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
    u64 Revision;
    void* Open; void* Close; void* Delete; void* Read; void* Write;
    void* GetPosition; void* SetPosition; void* GetInfo; void* SetInfo; void* Flush;
};
typedef struct {
    u64 Revision;
    void* OpenVolume;
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct {
    u32 Revision; EFI_HANDLE ParentHandle; void* SystemTable;
    EFI_HANDLE DeviceHandle; void* FilePath; void* Reserved;
    u32 LoadOptionsSize; void* LoadOptions;
    void* ImageBase; u64 ImageSize;
    u32 ImageCodeType; u32 ImageDataType; void* Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

// 提供这些 GUID 的地方（efi_main.c）也在这里声明
extern EFI_GUID gEfiGraphicsOutputProtocolGuid;
extern EFI_GUID gEfiSimpleFileSystemProtocolGuid;
extern EFI_GUID gEfiLoadedImageProtocolGuid;
