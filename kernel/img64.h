// img64.h - 图像解码层（本批新增）：PNG（完整：inflate + 全部滤波器 + 色型 0/2/3/4/6）/ BMP（未压缩）
//
// 用途（对应需求"图标/壁纸/头像优先从 VimtuFS2 读，内核只留少量兜底"）：
//   * 桌面壁纸：优先读 ui.wall.path 指定的 VimtuFS2 文件，读不到就用内置兜底壁纸（gfx64 的程序化壁纸）；
//   * 开始按钮（Dock 最左）：优先读 VimtuFS2 的 /logo/kaisi.png、/kaisi.png，读不到就用内核里
//     已嵌的 icon_start.bin（就是 logo/kaisi.png 的 RGBA，构建期由 _make_start_icon.py 生成）；
//   * 头像：路径建在 config64（ui.avatar.path），本批只把解码/加载能力备好（锁屏下一波用）。
//
// 像素格式：解码结果统一 0xAARRGGBB（A=255 表示不透明）；壁纸铺图时会丢掉 alpha（0x00FFFFFF 掩码）。
//
// 串口打点（自动验收 grep；带防刷屏上限）：
//   [IMG64] selftest PASS png=4x4 rc=0
//   [IMG64] decode png 4x4 ct=6 bd=8 bytes=112 -> px=16
//   [IMG64] load vfs:/wallpaper.png size=... png=1400x1000
//   [IMG64] load skip path=/wallpaper.png reason=not-found
//   [IMG64] unsupported format (jpeg not implemented in this batch) bytes=...
#pragma once
#include <stdint.h>

struct Img64 {
    uint32_t* px;      // 0xAARRGGBB（w*h）
    int w, h;
    int owned;         // 1 = px 由 img64 分配（img64_free64 释放）
};

// 从内存解码（自动识别 PNG / BMP）。返回 0 = 成功；-1 参数错；-2 格式不支持；-3 数据损坏；-4 内存不足
int img64_decode64(const void* data, int len, Img64* out);
// 从 VimtuFS2 读文件并解码（系统卷）：返回 0 = 成功；-1 = 打不开/读失败
int img64_load_vfs64(const char* path, Img64* out);
// 把内核内嵌的字节**幂等装进 VimtuFS2 系统卷**（与 /hello.vap、/hello.elf 的既有做法一致）：
//   * 卷没挂上 -> 打点 [IMG64] install skip ... reason=no-volume，返回 -1（调用方走内置兜底）
//   * 已存在且大小一致 -> 打点 reason=exists，返回 0（不重写，省一次 20KB 写盘）
//   * 否则建父目录（一层，够 /logo/kaisi.png 用）+ 写文件，打点 [IMG64] install path=.. bytes=.. ok=1
// 目的：让"开始按钮用真文件 logo/kaisi.png"这条需求在**任何有 VimtuFS2 系统卷的盘**上都成立
//（安装器装出来的盘、测试夹具盘），而裸 system.img（没有卷）走内核内置兜底，行为与之前一致。
int img64_install_blob64(const char* path, const uint8_t* data, uint32_t len, const char* src);
void img64_free64(Img64* img);
const char* img64_last_err64();
// 最近一次成功解码的格式名（"png" / "bmp"）
const char* img64_last_fmt64();

// 缩放（最近邻；src 是 AARRGGBB）：dst 需要 dw*dh*4 字节
void img64_scale64(const Img64* src, uint32_t* dst, int dw, int dh);

// 自检（启动期调用；解内核内嵌的 4x4 测试 PNG 并逐像素比对）：返回 0 = 通过
int img64_selftest64();
