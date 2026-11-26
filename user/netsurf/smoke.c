/* user/netsurf/smoke.c - ★ v0.12.5：NetSurf 库栈的**链接+接口自证**程序（ring3）
 *
 * 它存在的唯一目的：证明"13 个依赖库 + expat + zlib"不是一个一个能编，而是**能一起链进
 * 一个真实 ring3 程序、并且公开 API 真的能调用**。判据分两步（见 tools/netsurf_linkgate_win.sh）：
 *
 *   ① **whole-archive 链接**：把 14 个 .a 全部整个塞进链接器（`--whole-archive`），
 *      任何库内部引用了我们没编的东西（例如 libsvgtiny 要 `dom_xml_parser_create`）都会在
 *      链接期炸出来 —— 这一步是"库能编 ≠ 能用"的硬门禁。
 *   ② 本程序真的**调用**核心三段：libwapcaplet 的字符串内化 → libdom 的 hubbub 绑定把一段 HTML
 *      解成 DOM 树 → 读出根元素名字。串口打印 `[SMOKE] ...`，任一段失败就打印 FAIL 并返回非 0。
 *
 * 为什么用 HTML 而不是 XML：HTML 路径覆盖 libhubbub（tokeniser/treebuilder）+ libparserutils
 * （输入流/字符集）+ libdom（文档/节点）+ libwapcaplet（节点名内化），一条链把四个库全串起来；
 * XML 路径要 expat，而 expat 的绑定（libdom/bindings/xml/expat_xmlparser.c）由 ① 保证能链上。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "libwapcaplet/libwapcaplet.h"
#include "dom/dom.h"
#include "dom/bindings/hubbub/parser.h"

/* 一段带 doctype/head/body/属性的最小 HTML —— 足够驱动 tokeniser 与 treebuilder */
static const char kHtml[] =
    "<!DOCTYPE html>"
    "<html><head><title>vimtu</title></head>"
    "<body class=\"b\"><p id=\"x\">hello</p><p>world</p></body></html>";

/* ==================== 启动（自写 _start；与 user/tls/httpsprobe.c 同一条纪律）====================
 * 本程序是**静态 musl** 链接，不能用 musl 的 crt1.o：它里面有 `lea _DYNAMIC(%rip),%rsi`，
 * 而我们的映像钉在 4GiB+576KiB、静态非 PIE，_DYNAMIC 无处解析（链接期就报
 * `cannot find entry symbol _start`，产物只剩 2 KB 空壳 —— 实测第一版就是这样）。
 * 所以自己写 _start：把内核摆好的 SysV 初始栈交给 musl 的 __libc_start_main。 */
extern int __libc_start_main(int (*main_fn)(int, char**, char**), int argc, char** argv,
                             void (*init_dummy)(void), void (*fini_dummy)(void),
                             void (*ldso_dummy)(void));
static int smoke_main(int argc, char** argv, char** envp);
int main(void);

__attribute__((naked, noreturn, used, section(".text.start")))
void _start(void)
{
    __asm__ volatile(
        "xor %rbp, %rbp\n\t"
        "mov %rsp, %rdi\n\t"
        "and $-16, %rsp\n\t"
        "call smoke_start_c\n\t"
        "hlt\n\t");
}

__attribute__((used, noinline))
static void smoke_start_c(uint64_t* sp)
{
    const int argc = (int)sp[0];
    char** argv = (char**)(sp + 1);
    __libc_start_main(smoke_main, argc, argv, 0, 0, 0);
    _exit(127);
}

static int smoke_main(int argc, char** argv, char** envp)
{
    (void)argc; (void)argv; (void)envp;
    return main();
}

int main(void)
{
    char root_name[64] = "(none)";
    int ok = 1;

    /* ---------------- ① libwapcaplet：字符串内化（幂等） ---------------- */
    lwc_string *a = NULL, *b = NULL;
    const lwc_error ea = lwc_intern_string("vimtu", 5, &a);
    const lwc_error eb = lwc_intern_string("vimtu", 5, &b);
    printf("[SMOKE] lwc intern a=%d b=%d same=%d data=%s\n",
           (int)ea, (int)eb, (a != NULL && a == b) ? 1 : 0,
           (a != NULL) ? lwc_string_data(a) : "(null)");
    if (ea != lwc_error_ok || a == NULL || a != b) ok = 0;

    /* ---------------- ② libdom + libhubbub + libparserutils：HTML -> DOM ---------------- */
    dom_hubbub_parser_params params;
    memset(&params, 0, sizeof params);
    params.enc = "UTF-8";
    params.fix_enc = true;
    params.enable_script = false;

    dom_hubbub_parser *parser = NULL;
    dom_document *doc = NULL;
    dom_hubbub_error herr = dom_hubbub_parser_create(&params, &parser, &doc);
    printf("[SMOKE] hubbub create=%d parser=%p doc=%p\n", (int)herr, (void *)parser, (void *)doc);
    if (herr != DOM_HUBBUB_OK || parser == NULL || doc == NULL) {
        ok = 0;
    } else {
        herr = dom_hubbub_parser_parse_chunk(parser, (const uint8_t *)kHtml,
                                            strlen(kHtml));
        printf("[SMOKE] hubbub parse_chunk=%d bytes=%u\n", (int)herr, (unsigned)strlen(kHtml));
        if (herr != DOM_HUBBUB_OK) ok = 0;
        (void)dom_hubbub_parser_completed(parser);

        dom_node *root = NULL;
        const dom_exception dre = dom_document_get_document_element(doc, &root);
        if (dre == DOM_NO_ERR && root != NULL) {
            dom_string *name = NULL;
            if (dom_node_get_node_name(root, &name) == DOM_NO_ERR && name != NULL) {
                snprintf(root_name, sizeof root_name, "%s", dom_string_data(name));
                dom_string_unref(name);
            }
            dom_node_unref(root);
        }
        printf("[SMOKE] root=%s\n", root_name);
        /* 解析出来的根必须是 html —— 这条把"树真的建出来了"和"只跑了词法"分开 */
        if (strcmp(root_name, "html") != 0 && strcmp(root_name, "HTML") != 0) ok = 0;

        dom_node_unref(doc);          /* create() 把文档所有权交给调用方 */
        dom_hubbub_parser_destroy(parser);
    }

    if (a != NULL) lwc_string_unref(a);
    if (b != NULL) lwc_string_unref(b);

    printf("[SMOKE] result=%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
