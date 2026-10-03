/* font64.h - ★ Ring 3 用户态文字/字体栈（把"画字"从内核搬到用户态）
 *
 * 这一层解决的唯一问题：**用户态程序怎么在自家 shm 画布上画字**（ASCII + 常用汉字）。
 * 字库不编进程序：font_load() 从**系统卷**里用既有 open/read 读 TTF，解析后缓存；
 * 字形位图按 (面, 码点, 字号) 缓存（首次 vs 命中耗时由 font64_stats() 给出）。
 *
 * 度量规则**照抄内核**（kernel/font.cpp），逐条给出处：
 *   * 缩放：scaleFix = size_px * 1024 / unitsPerEm                （kernel/font.cpp:327）
 *   * 推进：adv_px   = (advance * scaleFix + 512) >> 10，下限 1    （kernel/font.cpp:336-338 / 813-815）
 *   * 行高：line = size_px + 4                                     （kernel/font.cpp:716）
 *   * 锚点：y = 升部顶端（基线 = y + (hhea.ascender * scaleFix >> 10)，
 *           与内核把光栅缓冲行 0 当 ascender 顶端的口径一致）      （kernel/font.cpp:617-625, 862-868）
 *   * 查询链：主面 -> 中文面 -> 兜底面 -> 缺字占位                  （kernel/font.cpp:787-803）
 * 单头文件：stb_truetype v1.26（public domain / MIT 双许可，原文见文件末尾）。
 *
 *   * 缺字占位推进一个 em                                          （kernel/font.cpp:808）
 *   * 中英 1:2 列网格（GRID12）：半角 = size_px/2、全角 = size_px —— 内核终端用
 *     "等宽面（Sarasa，ASCII 0.5em）+ 中文面（NotoSansSC，汉字 1.0em）" 得到的 8/16 @em16
 *     列网格，逐值相同（kernel/font.cpp 的 [FONT64] mono ascii=8 cjk=16 ratio=2 自检，
 *     以及 tests/fonts64_test.py 的断言）。
 *
 * 如实边界（**没做**的事，见各函数注释与报告）：
 *   * 不做 kerning/GPOS 字距调整、不做复杂脚本整形（阿拉伯/天城文/连字）、不做双向文本；
 *   * 不做亚像素定位（只有 0 或 +0.5 两种整体相位，font64_set_shift）；
 *   * 不做多字重合成（每个面就是一份 TTF；粗体要自己再 font64_add_face 一份字重文件）；
 *   * 不支持 CFF/OTTO 字库（font_load 会如实拒绝：内核 face_init 同样只吃 glyf）。
 */
#ifndef VIMTU64_FONT64_H
#define VIMTU64_FONT64_H

#include <stdint.h>

/* ---------------- 面（face）类型 ---------------- */
#define F64_KIND_AUTO   0xFF   /* font_load() 用法：按字库内容自动判定 */
#define F64_KIND_LATIN  0      /* 西文 UI（内核 face 0：Fonts/NotoSans-*.ttf） */
#define F64_KIND_CJK    1      /* 中文（内核 face 1：Fonts-open/NotoSansSC-*.ttf） */
#define F64_KIND_MONO   2      /* 终端等宽（内核 face 2：Fonts-open/sarasa-mono-sc-*.ttf） */
#define F64_KIND_FALLBK 3      /* 缺字兜底（内核 face 3：Fonts-open/unifont-*.ttf） */
#define F64_MAX_FACES   4

/* ---------------- 字距策略 ---------------- */
#define F64_SPACING_NATURAL 0  /* 每个面按自己的 hmtx 推进（= 内核公式；内核 UI 面就是这个） */
#define F64_SPACING_GRID12  1  /* 内核终端列网格：半角 em/2、全角 em（中英 1:2） */

/* ---------------- 画布（= 自家 shm 映射进来的像素缓冲）----------------
 * px 指向映射基址；pitch 固定 = w*4（XRGB8888，内存里低字节是蓝，与内核 fb 同口径）。 */
struct F64Canvas {
    uint32_t* px;
    int w, h;
};

/* ---------------- 不透明句柄 + 统计 ---------------- */
struct Font64;

struct F64FaceInfo {           /* 逐面信息（测试拿它与 fontTools 对账） */
    int         used;          /* 1 = 这一槽装好了一份字库 */
    int         kind;          /* F64_KIND_* */
    const char* path;          /* 从**系统卷**读的路径 */
    unsigned    bytes;         /* 实际读进来的字节数 */
    int         upem;          /* head.unitsPerEm */
    int         asc_fu;        /* hhea.ascender（字体单位） */
    int         desc_fu;       /* hhea.descender（字体单位，负） */
    int         nglyph;        /* maxp.numGlyphs */
    int         has_glyf;      /* 1 = 有 glyf 表（TrueType 轮廓；OTTO/CFF 会被拒） */
};

struct F64Stats {
    unsigned           miss_n, hit_n;      /* 字形缓存：首次光栅化 / 命中 次数 */
    unsigned long long miss_tsc, hit_tsc;  /* 对应的 rdtsc 周期累计（font64_tsc_per_ms() 换算成 µs） */
    unsigned           arena_peak, arena_fail;   /* stb 光栅化临时区的水位 / 分配失败次数（如实暴露） */
    unsigned           glyph_ok, glyph_empty, glyph_fail;  /* 位图非空 / 空白字形 / 失败 */
};

/* ---------------- 加载（从系统卷读，不把字库编进程序）---------------- */
/* 读一份 TTF 并解析：kind = F64_KIND_AUTO 时按内容判定（有 U+4E00 = 中文面）。
 * 返回句柄（失败返回 0）；faces[0] 失败时句柄仍然返回，用 font64_face_info() 如实查看
 * （具体原因打在串口 [FONT64U] 行里，绝不假装加载成功）。 */
struct Font64* font_load(const char* path);
/* 追加一个面（多面栈：西文 + 中文 + 等宽 + 兜底）。成功返回 0，失败返回负错误码。 */
int  font64_add_face(struct Font64* f, const char* path, int kind);
int  font64_face_count(struct Font64* f);
int  font64_face_info(struct Font64* f, int idx, struct F64FaceInfo* out);
int  font64_set_primary(struct Font64* f, int kind);     /* 主面（内核的 font_select） */
int  font64_set_spacing(struct Font64* f, int mode);     /* F64_SPACING_* */
int  font64_set_shift(struct Font64* f, int half_px);    /* 0 = 整数相位（默认）/ 1 = +0.5 */
int  font64_shift(struct Font64* f);

/* ---------------- 度量 ---------------- */
int  font64_line_height(struct Font64* f, int size_px);              /* size_px + 4 */
int  font64_advance(struct Font64* f, uint32_t cp, int size_px);     /* 笔位推进（含 1:2 策略） */
int  font64_face_of(struct Font64* f, uint32_t cp);                  /* 码点落在第几个面；-1 = 缺字 */
int  font64_text_width(struct Font64* f, const char* utf8, int size_px);
int  font64_is_wide(uint32_t cp);                                    /* 全角（East Asian W/F） */
/* 笔位序列：out[i] = 第 i 个字形绘制前的笔位 x（不含字形自身先导/悬挂）；
 * 返回字形个数（含空格），-1 = 容量不够。给验收脚本对账 1:2 推进用。 */
int  font64_pen_series(struct Font64* f, const char* utf8, int size_px, int* out, int cap);
int  font64_utf8_decode(const char* s, int* adv);                    /* 与内核 utf8_decode 同口径 */

/* ---------------- 绘制 ---------------- */
/* 往画布刷底色（整块）。 */
void font64_fill(struct F64Canvas* c, uint32_t color);
/* 单像素混合（覆盖率 + 颜色 alpha）——给"alpha 混合定点证据"用：已知 cov/颜色时结果可逐步复算。 */
void font64_blend_px(struct F64Canvas* c, int x, int y, uint32_t color, unsigned cov);
/* 往画布画一段 UTF-8 文本：y = 锚点行（升部顶端，与内核 font_draw_text 同口径），
 * color = 0xAARRGGBB（alpha 与字形覆盖率相乘后混合；alpha=0xFF 时不混合直接写）。
 * '\n' = 换行（回到起始 x，y += size_px + 4）；非法 UTF-8 字节跳过（与内核一致）。
 * 返回本次实际画出的字形个数（缺字会被算成 1 个，并打 [FONT64U] miss 行去重）。 */
int  font_draw_text(struct Font64* f, struct F64Canvas* c, int x, int y,
                    const char* utf8, int size_px, uint32_t color);

/* ---------------- 墨迹统计（给测试的像素口径）----------------
 * bg = 画布底色；tol = 判定为"有墨"的通道差阈值；统计 (x0,y0,w,h) 矩形内的墨迹像素数，
 * bbox 写回墨迹包围盒（[x,y,w,h]，无墨时全 0）。返回墨迹像素数。 */
int  font64_ink_stats(const struct F64Canvas* c, uint32_t bg, int x0, int y0, int w, int h,
                      int tol, int* bbox);

/* ---------------- 缓存 / 计时 ---------------- */
void               font64_cache_reset(struct Font64* f);   /* 清字形缓存（下一次全是 miss） */
struct F64Stats    font64_stats(struct Font64* f);
unsigned long long font64_rdtsc(void);
/* rdtsc 标定：用 PIT ticks()（250Hz）量一段真实睡眠，得出 tsc/ms。返回 0 = 标定失败。 */
unsigned long long font64_calibrate_tsc_per_ms(unsigned sleep_ms);

#endif /* VIMTU64_FONT64_H */
