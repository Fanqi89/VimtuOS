// icons64.h - 统一"真图标"查询/绘制层（本批新增）：外置图标包 -> img64 解码 -> 缓存 -> 混色上屏
//
// ==================== 图标从哪来（外置，进不了内核）====================
//   * **首选：图标包（build/iconpack.bin）** —— 构建期由 tools/make_iconpack.py 用自研光栅器
//     （tools/svg2png.py，Bootstrap Icons MIT 的纯填充路径 SVG）预渲染成 16/24/32/48 px 的
//     PNG（白 + alpha 掩码），打包后由 build64.sh 用 dd 写进**系统镜像内核区（LBA 9..8008）
//     的尾部固定区间**（ICON64_PACK_LBA 起 ICON64_PACK_MAX_SECTORS 扇区）。
//     为什么放这儿而不是 VimtuFS2：system.img 的 8073 扇区里没有 VimtuFS2 卷（卷在安装器建的
//     数据分区上，起点 LBA 8009 == kernel/memlayout64.h 的 ML64_STORE_LBA，两者重叠是已知约束，
//     见 memlayout64.h 的 ★★ 段）；而这段内核区字节在"安装到硬盘后"原样保留（loader 就从 LBA 9
//     读内核），所以裸 system.img、安装出来的盘、测试夹具盘三种情形读到的是同一份包。
//     资源字节**完全在内核二进制之外**（内核体积不受图标数量影响）。
//   * **可选覆盖：VimtuFS2 文件** —— 若系统卷里存在 /icons/<system|apps>/<名字>@<尺寸>.png，
//     它优先于图标包（打点 src=vfs）。给"装好的盘上换图标"留一个口子。
//   * 两者都没有（或包解码/CRC 失败）-> **回落既有程序化绘制**（调用方拿到 -1 自己画），
//     绝不允许"缺文件把界面画空"。
//
// ==================== 图标是掩码，颜色跟随主题 Token ====================
//   包里的 PNG 一律是"白色形状 + alpha 覆盖率"（flags bit0=0）。绘制时用调用方给的 palette 颜色
//   重新着色，所以状态区图标会随主题（浅色主题深灰线 / 暗色主题浅色线）自动变 —— 不是硬编码色。
//
// ==================== 串口打点（自动验收 grep；两类各有上限防刷屏）====================
//   [ICON64] init pack lba=7497 drive=0 bytes=49176 entries=110 icons=30 ok=1 sha=ffaba02b
//   [ICON64] init pack absent reason=no-magic drives=1 last-drive=0
//   [ICON64] load kind=ethernet path=pack:system/ethernet@24.png size=24 src=pack ok=1
//   [ICON64] load kind=ethernet path=/icons/system/ethernet@24.png size=24 src=vfs ok=1
//   [ICON64] fallback kind=terminal reason=no-pack (programmatic draw kept)
//   [ICON64] fallback kind=bell reason=decode-fail
//   [ICON64] selftest PASS pack=1 draws=..
#pragma once
#include <stdint.h>

// ==================== 图标包（磁盘）布局常量 ====================
// 包起点：内核区**尾部固定区间**的起点（固定不随包大小漂移；尾部留 512 扇区 = 256KB 上限）
#define ICON64_PACK_MAX_SECTORS 512u
#define ICON64_PACK_MAGIC_LEN   8u
// 包体积上限（字节）：512 扇区
#define ICON64_PACK_MAX_BYTES   (ICON64_PACK_MAX_SECTORS * 512u)
// 单个 PNG 块上限（防坏包把内存吃光）
#define ICON64_BLOB_MAX_BYTES   (64u * 1024u)

// ==================== kind 编号（必须与 tools/make_iconpack.py 的 KINDS 一致）====================
// 系统图标（掩码，跟随主题颜色）
#define ICON64_K_GLOBE       1
#define ICON64_K_ETHERNET    2
#define ICON64_K_WIFI        3
#define ICON64_K_VOLUME      4
#define ICON64_K_VOLUME_MUTE 5
#define ICON64_K_BELL        6
#define ICON64_K_SEARCH      7
#define ICON64_K_CLOSE       8
#define ICON64_K_POWER       9
#define ICON64_K_GEAR        10
#define ICON64_K_LOCK        11
#define ICON64_K_REBOOT      12
#define ICON64_K_CHEV_L      13
#define ICON64_K_CHEV_R      14
#define ICON64_K_PLUS        15
#define ICON64_K_PERSON      16
#define ICON64_K_USB         17
#define ICON64_K_MONITOR     18
#define ICON64_K_CHECK       19
#define ICON64_K_BATTERY     20
// 应用图标（Dock/桌面/开始菜单网格；同样存掩码，颜色由 icons64_app_color64 给"品牌感"亮色）
#define ICON64_A_MYPC        64
#define ICON64_A_RECYCLE     65
#define ICON64_A_TERMINAL    66
#define ICON64_A_SETTINGS    67
#define ICON64_A_TMGR        68
#define ICON64_A_MINES       69
#define ICON64_A_CALC        70
#define ICON64_A_FILES       71
#define ICON64_A_MONITOR     72
#define ICON64_A_ABOUT       73

// ==================== 生命周期 ====================
// 启动期调一次（kernel64.cpp 的 os_boot_path 里、gui64_run 之前）：
//   读包 -> 校验 magic/version/总长/逐条目 CRC -> 预解码"界面必用"的 (kind,尺寸) -> 打点。
// 返回已加载的条目数（0 = 没有可用的包，之后一切绘制走程序化回落）。
int icons64_init64();
// 包是否可用（0/1）
int icons64_available64();
// 包里有多少个图标 kind（0 = 不可用）
int icons64_kind_count64();
// 导出成 gui64 用的 RGBA 字节流（RGB 已被 palette 着色 + alpha = 覆盖率；dw×dh 任意缩放）。
// 用途：Dock/桌面图标的**预缩放缓存**（gui64_preload_icons64 用它把真图标装进既有缓存，
// 绘制路径一行不用改）。返回 0 = 成功；-1 = 这个 kind 没有可用位图（调用方回落内置位图）。
int icons64_export_rgba64(int kind, int want, uint8_t* dst, int dw, int dh, uint32_t palette);
// 包里的图标名（用于打点/自检；未知 kind 返回 "unknown"）
const char* icons64_kind_name64(int kind);

// ==================== 查询 / 绘制 ====================
// 取句柄：>=1 = 可用（内部索引 + 1）；-1 = 这个 kind 不可用（调用方回落程序化绘制）
int icons64_handle64(int kind);
// 在 (x,y) 画 size×size 的图标，palette = 0xRRGGBB（掩码着色）。返回 0 = 画了；-1 = 没画（回落）
int icons64_draw64(int h, int x, int y, int size, uint32_t palette);
// 同上，但带整体不透明度 alpha（0..255；与 p2ui_icon64 的 a 参数同义）
int icons64_draw_a64(int h, int x, int y, int size, uint32_t palette, int alpha);
// 便捷：kind + 尺寸直接画（内部查句柄）
int icons64_draw_kind64(int kind, int x, int y, int size, uint32_t palette, int alpha);
// 应用图标的"品牌感"亮色（同一 kind 固定；在渐变底板上也够亮）
uint32_t icons64_app_color64(int kind);

// 自检（启动期；返回 0 = 通过）：包 absent 时只做"回落路径可用"的检查，不算失败。
int icons64_selftest64();
// 打点：[ICON64] dump ...（每条 kind 一行的加载/回落统计；预算内）
void icons64_dump64();
