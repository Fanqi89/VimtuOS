/* user/browser/vimtu_jscript_stub.c - 无 JS 引擎的 js_* 实现（直接吃上游官方桩）
 *
 * 【这一版为什么改成 #include 上游文件 —— 记一次真实事故】
 *   上一版是**手写**的 11 个 js_* 桩，语义写错了两个：
 *     我写：js_newheap() -> *heap=NULL; return NSERROR_NOT_IMPLEMENTED;
 *     上游：js_newheap() -> *heap=NULL; return NSERROR_OK;
 *     上游：js_newthread() -> *thread=NULL; return NSERROR_NOT_IMPLEMENTED;
 *   （也就是说 NOT_IMPLEMENTED 属于 newthread，不属于 newheap —— 我把两个搞反了。）
 *   后果：desktop/browser_window.c:3184 的
 *     err = js_newheap(nsoption_int(script_timeout), &bw->jsheap);
 *     if (err != NSERROR_OK) return err;
 *   在 browser_window_create() 的第一步就把它变成返回值 -> 浏览器启动即死：
 *     [BROWSER] fatal browser_window_create err=31     (31 = NSERROR_NOT_IMPLEMENTED)
 *   自测时看不到，因为那一步只在真机上跑（netsurf_init 之前没有任何 JS 调用）。
 *
 *   结论不再是"再手写一遍、这次小心点"，而是**不要手写**：上游本来就有一份官方空实现
 *   content/handlers/javascript/none/none.c，语义由上游维护。core 构建没收它（handlers 组
 *   只编 css/html/image/text 四个子目录，javascript/ 不在其中），所以由本文件把它带进链接。
 *   这样"无 JS"的语义与上游逐字一致，以后不会再因为某个 return 值写反而整机启动失败。
 *
 * 注意 none.c 里 js_initialise() 会顺带 nsoption_set_bool(enable_javascript, false)：
 *   这正是我们想要的效果（HTML 里 <script> 直接不执行），且它发生在 netsurf_init 内部。
 */
#include "../../third_party/netsurf/content/handlers/javascript/none/none.c"
