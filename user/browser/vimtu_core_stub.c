/* user/browser/vimtu_core_stub.c - 补三个核心侧缺失的小符号（save_pdf / JS scheme fetcher / fabs）
 *
 * 这三个符号是**核心构建留下的洞**，不是前端表成员，逐条说明为什么在这里补：
 *
 * ① nserror save_pdf(const char *path)
 *    desktop/save_pdf.c 需要 libharu（NETSURF_USE_HARU_PDF=0），所以 core 构建把它排除在外；
 *    但 desktop/gui_factory.c 的默认 pdf_password 回调仍然直接调 save_pdf()。
 *    本前端的 misc 表没提供 pdf_password（用的是默认实现），于是这个符号必须有定义。
 *    语义：没有 PDF 后端 -> 返回 NSERROR_NOT_IMPLEMENTED，绝不返回 OK 假装存过文件。
 *
 * ② nserror fetch_javascript_register(void)
 *    content/fetch.c 无条件调它；实现本该来自 content/handlers/javascript/fetcher.c，而
 *    handlers 组只编 css/html/image/text 四个子目录，javascript/ 不在其中（没有 duktape，
 *    本来也用不上 JS）。这里做"注册一个什么都不提供的 scheme"的等价物：直接返回 OK。
 *    后果是 javascript: URL 没有 fetcher（页面里最多是"取不到"），不会崩。
 *
 * ③ double fabs(double)
 *    desktop/frames.c 用了 fabs()，而 musl 的 libc.a 不含数学函数（在 libm.a 里），
 *    链接命令只带 libc.a。这里给一个 IEEE-754 二进制补零的 fabs：没有浮点异常、
 *    不需要 libm、与 libm 的 fabs 语义逐位相同（清符号位）。
 *    为什么不加 libm.a 到链接命令：判据/脚本不许动，而 fabs 就一行。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utils/errors.h"

nserror save_pdf(const char *path)
{
    (void)path;
    return NSERROR_NOT_IMPLEMENTED;
}

nserror fetch_javascript_register(void)
{
    /* 没有 JS 引擎：不注册任何 scheme。核心只检查返回值，OK 表示"注册阶段没有问题"。 */
    return NSERROR_OK;
}

double fabs(double x)
{
    union {
        double d;
        uint64_t u;
    } v;

    v.d = x;
    v.u &= 0x7FFFFFFFFFFFFFFFULL;    /* 清符号位 */
    return v.d;
}
