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
