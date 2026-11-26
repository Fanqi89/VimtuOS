/* user/browser/vimtu_http_parameter.c - 把上游 utils/http/parameter.c 编进前端的编译单元
 *
 * 为什么这一个文件只 include 一个 .c：utils/http 下的 .c 各自定义了**同名但不同内容**的
 * 私有类型/静态函数（例如 http_directive、count、parse_max_age 在好几个文件里都有），
 * 合成一个 TU 会立刻 redefinition。所以一个上游 .c = 一个前端 TU，代价是 7 个小文件，
 * 换来的是"不需要改构建脚本、不需要动 third_party"。
 *
 * 背景（为什么会有这一组文件）：tools/netsurf_core_build_win.sh 的 utils 组是 RECURSE=0，
 * 只收 utils 下的 .c，而 HTTP 头解析住在 utils/http/ 子目录里 —— libnetsurfcore.a 因此缺
 * http_parse_content_type / http_content_type_destroy / http_cache_control_* /
 * http_strict_transport_security_* / http_parameter_list_find_item 等符号，链接必失败。
 * 详见 vimtu_http_content_type.c 头部的完整说明。
 */
#include "../../third_party/netsurf/utils/http/parameter.c"
