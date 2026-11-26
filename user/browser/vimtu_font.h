/* user/browser/vimtu_font.h - 前端唯一的字体度量与字形来源（单宽位图字模）
 *
 * 为什么要有这一层，而不是直接用 user/lib/font64.c + stb_truetype：
 *   ① stb_truetype.h 与本构建的 musl 头（-nostdinc + musl/obj/include）在
 *      assert/ctype 这几处口径不一致，实测编不过；而且要连带 20 多万行的第三方头，
 *      编译时间和出错面都不划算；
 *   ② 本前端只需要"能把字画出来 + 量得准"两件事，真值（advance）必须是**同一个函数**
 *      算出来的，否则 layout 量出来的宽度和 plotter 画出来的位置会漂 —— 文字重叠/裂缝
 *      这类 bug 一旦出现极难定位。所以度量和绘制共用 vimtu_font_advance()。
 *
 * 字模来源：user/wm/wm_ttf.h（构建期用 tools/wm_titlefont_win.py 从 build64/font_mono.ttf
 *   预光栅化的 16px em、ASCII 32..126、4bit/px 覆盖率的单宽字模）。直接复用而不是自己画
 *   ASCII 方块字：它已经是**真字形**（带抗锯齿），页面上看到的是"字"而不是色块。
 *   em = 16 行，基线在第 15 行（wm.c 的 wm_text_ttf 就是按 y = em 顶 画的）。
 */
#ifndef VIMTU_FONT_H
#define VIMTU_FONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIMTU_FONT_EM       16   /* em 框高（像素，scale=1） */
#define VIMTU_FONT_BASELINE 15   /* em 顶 -> 基线（像素，scale=1） */
#define VIMTU_FONT_ADV      8    /* 单宽 advance（像素，scale=1） */

struct plot_font_style;

/* 一个字形：位图宽高 + 相对笔位的偏移 + advance，alpha 是 4bit/px（2px/字节，高半字节在左） */
struct vimtu_glyph {
    int w;
    int h;
    int bx;
    int by;
    int adv;
    const unsigned char *alpha;
};

/* 取字形；ASCII 之外返回 false（调用方画"缺字块"，不假装有字形） */
bool vimtu_font_glyph(uint32_t ucs4, struct vimtu_glyph *out);

/* 从 plot_font_style 推出整数倍率（1..3）：字号 pt -> 像素 -> 16px em 的整数倍 */
int vimtu_font_scale(const struct plot_font_style *fstyle);

/* 单个字符的 advance（像素）—— layout->width 与 plot->text 必须都用它 */
int vimtu_font_advance(const struct plot_font_style *fstyle);

/* 行高（像素）：只给前端自己排版用（页面里的行高由 NetSurf 的 CSS layout 决定） */
int vimtu_font_line_height(const struct plot_font_style *fstyle);

/* 解一个 UTF-8 字符：返回下一个字符的偏移；非法序列按 U+FFFD 前进 1 字节 */
size_t vimtu_font_utf8_next(const char *s, size_t len, size_t off, uint32_t *ucs4);

#endif /* VIMTU_FONT_H */
