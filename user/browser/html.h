/* html.h - VimtuOS Browser 的 HTML 子集解析器（纯用户态，零依赖）
 *
 * 设计取舍：这是**排版用的最小解析器**，不是通用 HTML 引擎。它把文档压成一串"块"
 * （段落/标题/列表项/分隔线），每块带一段已解码实体的 UTF-8 文本；渲染器只按顺序流式排版。
 * 为什么这样切：Ring3 程序的主装载区只有 64 KiB（kernel/elf64.cpp 的硬约束），DOM 树 + CSS
 * 层叠那种结构放不下；块列表是 16 B/块、文本进一块外部池，够用且能如实报"缓冲区满"。
 *
 * 调用约定：textpool/blocks 由调用方预算（浏览器走 mmap 拿），本模块**不 malloc**。
 */
#ifndef VIMTUOS_BROWSER_HTML_H
#define VIMTUOS_BROWSER_HTML_H

#include <stdint.h>

#define H_MAX_BLOCKS 768
#define H_MAX_TEXT   (56 * 1024)

enum {
    H_P = 0,      /* 段落 */
    H_H1, H_H2, H_H3,
    H_PRE,        /* 预排版（保留空白与换行） */
    H_LI,         /* 列表项（文本已带 "• " 前缀） */
    H_HR,         /* 水平分隔线（len = 0） */
    H_TITLE       /* 文档标题（不进正文流，单独给出） */
};

enum { H_ALIGN_LEFT = 0, H_ALIGN_CENTER, H_ALIGN_RIGHT };

struct HBlock {
    uint32_t off;        /* 文本在 textpool 里的字节偏移 */
    uint32_t len;        /* 字节数；可含 '\n'（来自 <br> 或 <pre>） */
    uint8_t  type;       /* H_* */
    uint8_t  align;
    uint8_t  bold;
    uint8_t  underline;
    uint16_t size;       /* 像素字号（建议值，渲染器可再调） */
    uint32_t color;      /* 0xRRGGBB；0 = 用主题默认色 */
    uint32_t bg;         /* 0 = 透明 */
};

struct HDoc {
    int  nblocks;
    int  title_off, title_len;
    int  text_used;
    uint32_t link_off, link_len;   /* 第一处 <a href> 的文本（演示"链接"颜色/下划线用） */
    char* textpool;
    struct HBlock* blocks;
};

/* 解析 src/len（UTF-8 的 HTML 文本）。
 *   返回 0 = 成功；-1 = blocks 满（已解析的部分仍可用）；-2 = textpool 满；-3 = 参数非法。
 *   解析完 doc 的 nblocks / title_off / title_len / text_used / link_off / link_len 都是真值（不猜不补）。 */
int html_parse(const char* src, int len, struct HDoc* doc, char* textpool, int textcap,
               struct HBlock* blocks, int maxblocks);

#endif /* VIMTUOS_BROWSER_HTML_H */
