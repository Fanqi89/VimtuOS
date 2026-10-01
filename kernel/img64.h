// img64.h - 图像解码层（本批新增）：PNG（完整：inflate + 全部滤波器 + 色型 0/2/3/4/6）/ BMP（未压缩）
//
// 用途（对应需求"图标/壁纸/头像优先从 VimtuFS2 读，内核只留少量兜底"）：
//   * 桌面壁纸：优先读 ui.wall.path 指定的 VimtuFS2 文件，读不到就用内置兜底壁纸（gfx64 的程序化壁纸）；
//   * 开始按钮（Dock 最左）：优先读 VimtuFS2 的 /logo/kaisi.png、/kaisi.png，读不到再读系统卷里的
//     /etc/icon_start.bin（64x64 RGBA，构建期由 _make_start_icon.py 生成、打包脚本写进卷），
//     最后才是内核里**只剩 24x24 mip（2,304 B）**的 icon_start_mini.bin 兜底（见下面的"资源外置"段）；
//   * 头像：路径建在 config64（ui.avatar.path），本批只把解码/加载能力备好（锁屏下一波用）。
//
// 像素格式：解码结果统一 0xAARRGGBB（A=255 表示不透明）；壁纸铺图时会丢掉 alpha（0x00FFFFFF 掩码）。
//
// 串口打点（自动验收 grep；带防刷屏上限）：
//   [IMG64] selftest PASS png=4x4 rc=0 bytes=112 fmt=png real=158x158
//   [IMG64] selftest real ok=1 bytes=19810 158x158 px=24964 fnv=00000000EAD69FF1 rc=0 fmt=png
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

// ==================== ★ 本批（资源外置）：raw 图标/logo 从**系统卷**读，内核里不再内嵌 ====================
// 背景：内核里曾经内嵌 5 份 raw RGBA 资源（合计 356,992 B）——logo_rgba.bin 144,000 B（240x150）、
//   icon_mycomputer/recyclebin/terminal.bin 各 65,536 B（128x128）、icon_start.bin 16,384 B（64x64）。
//   它们由**构建期**写进系统卷（tools/assets_pack_win.py）：/etc/logo.bin、/etc/icon_mypc.bin、
//   /etc/icon_recycle.bin、/etc/icon_term.bin、/etc/icon_start.bin（都落在已存在的 /etc 里，
//   不新建根级目录）。内核侧只剩"怎么从卷里取"，取不到就回落调用方的内置/程序化绘制。
//
// 语义（★ 每个取用点都要打点，验收 grep 用）：
//   * 成功：*out = kmalloc_64 的缓冲（长度**恰好** want_bytes，调用方 kfree_64），返回 want_bytes；
//           打点  [IMG64] asset path=/etc/logo.bin want=144000 bytes=144000 src=vfs ok=1
//   * 失败：*out = nullptr，返回 < 0，并如实打一条 skip（reason=no-volume / not-found / size /
//           read-failed / oom）：
//            [IMG64] asset skip path=/etc/logo.bin want=144000 reason=no-volume src=builtin ok=0
//            [IMG64] asset skip path=/etc/icon_term.bin want=65536 reason=not-found src=builtin ok=0
// 为什么按**长度**校验而不是内容：卷里那份就是构建期资源文件的逐字节副本（打包脚本用
//   tcc_pack_win.verify 回读比对过），长度对上就是同一份；内容再校验一遍只会多花时间。
// 打点上限：每个路径只打一次成功行（g_asset_log 计数），失败行也按路径去重（避免每帧刷屏）。
int img64_load_asset64(const char* path, uint32_t want_bytes, uint8_t** out);
void img64_free64(Img64* img);
const char* img64_last_err64();
// 最近一次成功解码的格式名（"png" / "bmp"）
const char* img64_last_fmt64();

// 缩放（最近邻；src 是 AARRGGBB）：dst 需要 dw*dh*4 字节
void img64_scale64(const Img64* src, uint32_t* dst, int dw, int dh);

// 自检（启动期调用；返回 0 = 通过）：
//   * 内嵌 4x4 PNG 逐像素比对 + 缩放用例；
//   * ★ 真文件 logo/kaisi.png（内核内嵌原始字节，158x158 RGBA）解码 -> 尺寸 + 整块像素缓冲 FNV-1a 32
//     指纹比对（宿主侧 PIL/纯 Python 同一指纹）——专门挡住 inflate 的"动态块之后收尾固定块"这类退化。
int img64_selftest64();
