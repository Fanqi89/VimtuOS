/* user/browser/vimtu_fetcher_unit.c - 把 fetchers/vimtu.c 编进前端的"编译单元"
 *
 * 为什么要这个文件（保留它的理由，实测过一次反复）：
 *   user/browser/fetchers/vimtu.c（HTTP/1.1 + mbedTLS 的 fetcher）应该由
 *   tools/netsurf_core_build_win.sh 的 fetchers 组收进 libnetsurfcore.a —— 有时它确实进去了
 *   （实测 ar t 里出现过 _c_Users_..._fetchers_vimtu.o），但这条路径**不稳定**：
 *   脚本对 EXTRA 的处理是 `[ -f "$NS/$e" ] && FILES="$FILES $NS/$e"`，而 EXTRA 里给的是
 *   `$ROOT/user/browser/fetchers/vimtu.c`（已经是绝对路径），拼上 $NS/ 之后能否命中取决于
 *   当时 shell 的路径解析 —— 换句话说"fetcher 在不在归档里"变成了一件看运气的事。
 *   一旦不在，链接就报 `undefined symbol: fetch_vimtu_register`，浏览器根本没网络。
 *
 *   所以前端自己把这个 TU 带上（构建脚本本来就收 user/browser/*.c）：两种情况下都能链接：
 *     * 归档里没有 vimtu.o -> 由本文件提供定义；
 *     * 归档里也有 vimtu.o -> 显式 .o 已经定义了这些符号，归档成员不会被拉进来（无重复定义）。
 *
 * 纪律边界：
 *   * 不改 user/browser/fetchers/vimtu.c（任务明确"不要动它"）—— 这里只是 include；
 *   * 不改 tools/netsurf_core_build_win.sh（判据是硬要求）；
 *   * "" 形式的 include 按**本文件所在目录**解析，所以 "fetchers/vimtu.c" 就是
 *     user/browser/fetchers/vimtu.c，不依赖 -I（也就不动构建环境）。
 */
#include <stdarg.h>          /* vimtu.c 用了 va_start/va_end 却没自带 stdarg.h（它此前没被单独
                              * 用 -Wall 编过，这个漏 include 一直没暴露） */
#include "fetchers/vimtu.c"
