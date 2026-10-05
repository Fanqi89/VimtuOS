// font.h - TrueType 字体渲染（**四个字体面**：西文 / 中文 / 终端等宽 / 缺字兜底；UTF-8）
#pragma once
#include <stdint.h>

// 面表（与 _subset_fonts.py / _subsetsimhei.py 的产物文件名一一对应）
enum {
    FONT_FACE_ASCII    = 0,   // 西文 UI     build/font_bahnschrift.ttf（Noto Sans 子集）
    FONT_FACE_CJK      = 1,   // 中文        build/font_simhei.ttf    （Noto Sans SC 子集）
    FONT_FACE_MONO     = 2,   // 终端等宽    build/font_mono.ttf       （Sarasa Mono SC 子集，中英 1:2）
    FONT_FACE_FALLBACK = 3,   // 缺字兜底    build/font_fallback.ttf   （Unifont 子集，只收前三个面没有的码点）
    FONT_FACE_COUNT    = 4
};

void font_init();                    // 解析嵌入 TTF（无堆分配）+ 打 [FONT64] faces= 行
void font_select(int face);          // 见上面的 FONT_FACE_*（历史调用方写 font_select(2)：
                                     // 现在是终端等宽面，汉字由查询链落到 FONT_FACE_CJK，不会出豆腐块）
// ★ P3（字体大小档）：设置应用把渲染字号档写进这里（em 高度像素，14/16/18）。
//   * 只改 scaleFix/ascFix 与全部字形缓存，**不改**光栅缓冲尺寸（FONT_PX），所以绝不越界；
//   * 取值被钳制在 12..18（>18 时字形会被 FONT_PX=20 的缓冲裁掉，如实不让选）；
//   * 默认 16 = 历史 FONT_SIZE_PX，不调用它时行为与之前逐像素一致。
void font_set_size64(int px);
int  font_get_size64();
int  font_face_count();              // 4
int  font_line_height();             // 行高（像素）
int  font_current_face();            // ★ 当前面序号（只读；console64 画完引导日志后恢复现场用）
int  font_glyph_advance(char c);     // ASCII 字符推进宽度（像素，取当前面）
int  font_glyph_advance_cp(uint32_t cp);  // Unicode 码点推进宽度（走查询链）
int  font_text_width(const char* s); // UTF-8 字符串总宽（像素）
uint32_t font_utf8_decode(const char* s, int* adv);  // 解码 UTF-8 首字符（0=非法）
bool font_draw_glyph(int x, int y, char c, uint32_t fg);   // ASCII，false=无字形
bool font_draw_glyph_cp(int x, int y, uint32_t cp, uint32_t fg);  // Unicode
void font_draw_text(int x, int y, const char* s, uint32_t fg);     // UTF-8

// ==================== 四面的启动自检 / 打点（kernel64.cpp 调；验收脚本按行断言）====================
//   [FONT64] faces=4 ascii=1 cjk=1 mono=1 fallback=1
//   [FONT64] mono ascii=8 cjk=16 ratio=2            （等宽面 ASCII 宽 = 中文面汉字宽 / 2）
//   [FONT64] fallback hit cp=0x2229 face=3          （查询链落到兜底面；每个码点只打一次）
//   [FONT64] glyph miss  cp=0x<hex>                 （四个面都没有；每个码点只打一次）
//   [FONT64] selftest PASS mask=0x1f                （FAIL 时 mask 指出哪几项没过）
void font_selftest();
uint32_t font_fallback_hits();       // 查询链落到兜底面的次数（诊断）
uint32_t font_glyph_miss_count();    // 四个面都画不出来的码点数（诊断；正常应为 0）

// ==================== ★ 本批（内核预算）：中文面（face 1）外置装载 ====================
// 系统内核**不再内嵌**中文面（font_simhei_z = 733,584 B，唯一的大块字体）：它搬进"原始区" blob
// （tools/demo_pack_win.py 的 RAW_EXTRA；夹具盘都有原始区、卷里的 /Fonts-open/*.ttf 只在全量演示盘里）。
// 什么时候调（顺序有讲究，见 kernel64.cpp 的两个调用点）：
//   ① ata64_init64() 之后立刻一次 —— 那一行之后才开始画中文（早于此屏幕还只有 ASCII）；
//      ★ 这个点跑在**盘符扫描之前**：AHCI-only 机器上 drive64 给不出"系统盘"，所以这一点的取值
//      在"系统盘 / drive 0"之外还包括**按 ata64 枚举逐盘有界探测原始区**（与图标包同源；判据 =
//      blob 的长度头 + 完整解压 + TTF cmap 真有汉字；有界 / 失败如实打点 / 绝不 PANIC / 失败不永久缓存）；
//      ★ 如实边界：更早的那屏"屏幕硬件检查报告"（kmain 里约 5 秒）仍只有 ASCII 面，其汉字是缺字占位；
//   ② 盘符扫描/store64_init64（vfs 起来）之后再一次 —— 早试失败在这里重试（幂等）；
//   ③ font_external_give_up64()：两个点都试完还是取不到就"如实认输"（中文渲染成缺字占位）——
//      **不是失败**：系统照常启动，[FONT64] faces=… cjk=0 + 与中文面相依的三项标 cjk_ext_na=1。
// 返回：0 = 中文面就绪（或本来就在内核里）；1 = 这次没到手（可以再调）；2 = 已确认取不到（不再重试）。
int  font_external_load64();
void font_external_give_up64();

// ==================== 字形预加载（系统级预加载的一部分） ====================
// 界面/终端的文字平时是"首次绘制时才现场光栅化"，会带来首次打开窗口的卡顿。
// 启动阶段把常用字形提前光栅化进缓存（ASCII + 界面常用汉字），此后绘制只做位图混合。
// 返回值 = 本次**新光栅化**的字形个数（已缓存的不会重复计数）。
int font_prewarm_ascii();                  // 当前 face：ASCII 32..126
int font_prewarm_ascii_all();              // 全部 4 个 face 的 ASCII
int font_prewarm_text(const char* utf8);   // UTF-8 串里出现的所有字形（自动走查询链选 face）
int font_cache_used();                     // 已缓存的字形总数（诊断/UI）
int font_lru_capacity();                   // 每 face 的 CJK LRU 槽数（预加载自检用）
int font_cache_capacity();                 // 缓存容量上限
