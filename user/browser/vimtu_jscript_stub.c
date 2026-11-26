/* user/browser/vimtu_jscript_stub.c - "没有 JS 引擎"的 js_* 空实现
 *
 * 为什么会需要这个文件（先说清楚，免得被当成"前端乱补符号"）：
 *   上游 NetSurf 把 JS 后端分成两半：
 *     * content/handlers/javascript/duktape/dukky.c   —— 真实现，要 duktape（本构建没编）；
 *     * desktop/jscript.c                            —— NETSURF_USE_DUKTAPE=0 时的空实现（stub）。
 *   本仓库的 third_party/netsurf 里**根本没有 desktop/jscript.c / jscript.h**
 *   （抓源码时那只文件没下来：ls third_party/netsurf/desktop 里只有 bitmap/browser/…/version）。
 *   于是 libnetsurfcore.a 里 html.o/script.o/dom_event.o/textplain.o 引用的 11 个 js_*
 *   符号没有任何定义 —— 链接必然失败。这不是"我漏了某个表成员"，是核心侧缺一整个桩文件。
 *
 * 为什么不改 third_party / 不改构建脚本去补：
 *   任务纪律明确写了 third_party 只读（会被上游覆盖）、构建脚本判据不许动。
 *   而 user/browser 下的 .c 是"前端自己的一亩三分地"，把这个桩放在这里既不动别人的地盘，
 *   又能在交接单里一眼看见"JS 是故意的空实现"。
 *
 * 语义（与上游 jscript.c 的空实现一致，逐条对齐）：
 *   * js_initialise / js_finalise         —— 什么都不做；
 *   * js_newheap / js_newthread           —— 返回 NSERROR_NOT_IMPLEMENTED（没有引擎就没有 heap）；
 *   * js_exec / js_fire_event             —— 返回 false（= 这次执行/派发没有发生）；
 *   * 其余（destroy/close/cleanup/新元素）—— 空实现：上游在无引擎时的调用点不会传出有效指针，
 *                                            这里不接受也不假装处理。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utils/errors.h"
#include "javascript/js.h"

void js_initialise(void)
{
    /* 无 JS 引擎：什么都不做。页面里的 <script> 会被 html handler 跳过执行。 */
}

void js_finalise(void)
{
}

nserror js_newheap(int timeout, jsheap **heap)
{
    (void)timeout;
    if (heap != NULL) {
        *heap = NULL;
    }
    return NSERROR_NOT_IMPLEMENTED;
}

void js_destroyheap(jsheap *heap)
{
    (void)heap;
}

nserror js_newthread(jsheap *heap, void *win_priv, void *doc_priv, jsthread **thread)
{
    (void)heap;
    (void)win_priv;
    (void)doc_priv;
    if (thread != NULL) {
        *thread = NULL;
    }
    return NSERROR_NOT_IMPLEMENTED;
}

nserror js_closethread(jsthread *thread)
{
    (void)thread;
    return NSERROR_OK;
}

void js_destroythread(jsthread *thread)
{
    (void)thread;
}

bool js_exec(jsthread *thread, const uint8_t *txt, size_t txtlen, const char *name)
{
    (void)thread;
    (void)txt;
    (void)txtlen;
    (void)name;
    return false;
}

bool js_fire_event(jsthread *thread, const char *type, struct dom_document *doc, struct dom_node *target)
{
    (void)thread;
    (void)type;
    (void)doc;
    (void)target;
    return false;
}

bool js_dom_event_add_listener(jsthread *thread,
                              struct dom_document *document,
                              struct dom_node *node,
                              struct dom_string *event_type_dom,
                              void *js_funcval)
{
    (void)thread;
    (void)document;
    (void)node;
    (void)event_type_dom;
    (void)js_funcval;
    return false;
}

void js_handle_new_element(jsthread *thread, struct dom_element *node)
{
    (void)thread;
    (void)node;
}

void js_event_cleanup(jsthread *thread, struct dom_event *evt)
{
    (void)thread;
    (void)evt;
}
