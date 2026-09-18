// font.h - TrueType 字体渲染（bahnschrift + ChaparralPro + simhei 中文；UTF-8）
#pragma once
#include <stdint.h>

void font_init();                    // 解析嵌入 TTF（无堆分配）
void font_select(int face);          // 0=bahnschrift(正文), 1=chaparral(标题), 2=simhei(中文)
int  font_face_count();              // 3
int  font_line_height();             // 行高（像素）
int  font_glyph_advance(char c);     // ASCII 字符推进宽度（像素）
int  font_glyph_advance_cp(uint32_t cp);  // Unicode 码点推进宽度
int  font_text_width(const char* s); // UTF-8 字符串总宽（像素）
uint32_t font_utf8_decode(const char* s, int* adv);  // 解码 UTF-8 首字符（0=非法）
bool font_draw_glyph(int x, int y, char c, uint32_t fg);   // ASCII，false=无字形
bool font_draw_glyph_cp(int x, int y, uint32_t cp, uint32_t fg);  // Unicode
void font_draw_text(int x, int y, const char* s, uint32_t fg);     // UTF-8

// ==================== 字形预加载（系统级预加载的一部分） ====================
// 界面/终端的文字平时是"首次绘制时才现场光栅化"，会带来首次打开窗口的卡顿。
// 启动阶段把常用字形提前光栅化进缓存（ASCII + 界面常用汉字），此后绘制只做位图混合。
// 返回值 = 本次**新光栅化**的字形个数（已缓存的不会重复计数）。
int font_prewarm_ascii();                  // 当前 face：ASCII 32..126
int font_prewarm_ascii_all();              // 全部 3 个 face 的 ASCII
int font_prewarm_text(const char* utf8);   // UTF-8 串里出现的所有字形（自动选 face）
int font_cache_used();                     // 已缓存的字形总数（诊断/UI）
int font_lru_capacity();                   // 每 face 的 CJK LRU 槽数（预加载自检用）
int font_cache_capacity();                 // 缓存容量上限
