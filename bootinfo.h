// bootinfo.h - loader 传给内核的启动信息结构
#pragma once
#include <stdint.h>

// 布局与 boot/loader.asm 的 write_bootinfo 一致
struct BootInfo {
    uint32_t magic;        // 0x00 'AUR1'
    uint32_t lfb_addr;     // 0x04
    uint16_t width;        // 0x08
    uint16_t height;       // 0x0A
    uint8_t  bpp;          // 0x0C
    uint8_t  pad;          // 0x0D
    uint16_t pitch;        // 0x0E
    uint32_t mem_entries;  // 0x10
    uint32_t mem_map_addr; // 0x14
    uint32_t kernel_size;  // 0x18
    uint16_t mode_num;     // 0x1C 当前 VBE 模式号（loader 实际设置的模式）
    uint16_t mode_count;   // 0x1E 探测到的可用模式数（<= BOOT_MODE_MAX）
    uint32_t mode_list;    // 0x20 模式表地址（每项 8 字节，见 BootMode）
    uint32_t edid_addr;    // 0x24 EDID 缓冲地址（VBE/DDC 4F15 读到的 128 字节）
    uint16_t edid_size;    // 0x28 EDID 长度（通常 128）
    uint16_t edid_ok;      // 0x2A 1 = EDID 读取成功
    // ---- ★ v0.9.8：帧缓冲地址的**高 32 位**与来源（只在尾部追加，老字段与 magic 一个没动）----
    // 为什么必须加：UEFI 的 GOP 帧缓冲在**大显存卡**上常被固件放到 4GiB 以上（RTX 3080 这类
    //   "Above 4G Decoding" 机器尤其如此）。老结构只有 32 位的 lfb_addr，真机上会被截断成
    //   一个不存在的地址 —— 写进去就是 #PF/黑屏。拼法：pa = lfb_addr | (lfb_addr_hi << 32)。
    // 兼容性：BIOS loader（VBE）显式写 0（VBE LFB 一定在 4GiB 以内），行为与改动前完全一致。
    uint32_t lfb_addr_hi;  // 0x2C 帧缓冲物理地址高 32 位（0 = 4GiB 以内）
    uint8_t  fb_source;    // 0x30 0 = VBE(BIOS loader) / 1 = GOP(UEFI loader)
    uint8_t  fb_flags;     // 0x31 bit0 = 1 表示 lfb 物理地址 > 4GiB（信息位，不改变语义）
    uint16_t fb_rsvd;      // 0x32 保留（一律 0，给下一批留位）
} __attribute__((packed));

// 一个可用显示模式（loader 的 VBE 探测结果，真实值，非硬编码）
struct BootMode {
    uint16_t mode;         // VBE 模式号
    uint16_t width;        // 水平分辨率（像素）
    uint16_t height;       // 垂直分辨率（像素）
    uint8_t  bpp;          // 色深位
    uint8_t  pad;
} __attribute__((packed));
static const uint32_t BOOT_MODE_MAX = 16;
static const uint32_t BOOT_MODE_LIST = 0x7400;

// E820 条目（20 字节）
struct E820Entry {
    uint32_t base_low;
    uint32_t base_high;
    uint32_t len_low;
    uint32_t len_high;
    uint32_t type;         // 1 = usable RAM
} __attribute__((packed));

static const uint32_t BOOT_INFO_MAGIC = 0x41555231;  // 'AUR1'
static const uint32_t BOOT_INFO_ADDR  = 0x1000;
static const uint32_t BOOT_MEM_MAP    = 0x2000;
