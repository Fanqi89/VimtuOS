/* mbedtls_vimtu_user_config.h - ★ v0.12.4：VimtuOS 的 mbedTLS **用户覆盖配置**
 *
 * 用法（tools/mbedtls_build_win.sh 里那条唯一正确的写法）：
 *   CFG_DEF='-DMBEDTLS_USER_CONFIG_FILE="mbedtls_vimtu_user_config.h"'
 *   $CC ... $CFG_DEF ...
 * 为什么不是 MBEDTLS_CONFIG_FILE：那个是**替换**整份默认配置（要抄 4000 行）；
 *   MBEDTLS_USER_CONFIG_FILE 在 build_info.h 里是"默认配置之后、check_config.h 之前"
 *   再 include 一次（见 include/mbedtls/build_info.h:129），所以这里只写**差异**。
 *
 * 踩过的坑（v0.12.3 实测）：`-DMBEDTLS_CONFIG_FILE='<mbedtls/mbedtls_config.h>'` 里的尖括号
 *   会被 bash 的引号吃掉 -> `error: expected "FILENAME" or <FILENAME>`。所以本次用
 *   **双引号**形式，并且用变量展开（bash 不对变量展开结果再做去引号，引号原样传给 clang）。
 */
#ifndef VIMTU_MBEDTLS_USER_CONFIG_H
#define VIMTU_MBEDTLS_USER_CONFIG_H

/* ==================== ① 熵：只信我们自己的 CSPRNG ====================
 * 默认的 mbedtls_entropy 会去开 /dev/urandom —— 本系统**没有这个设备**，于是
 *   mbedtls_entropy_func 会拿不到熵 -> 握手前的 DRBG 播种直接失败（"TLS 能连上但握手不起来"
 *   的典型现场之一）。这里两条一起改：
 *     MBEDTLS_NO_PLATFORM_ENTROPY  : 干脆不要 /dev/urandom 这条默认熵源；
 *     MBEDTLS_ENTROPY_HARDWARE_ALT : 由 user/lib/mbedtls_vimtu_port.c 的
 *                                    mbedtls_hardware_poll() 提供熵，它调 getrandom(318)
 *                                    -> kernel/rand64.cpp 的密码学级 CSPRNG（RDSEED/RDRAND
 *                                    或 SHA-256 计数器 DRBG）。 */
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_NO_PLATFORM_ENTROPY

/* ==================== ② 只做 TLS 客户端 ====================
 * 服务端（SSL_SRV）与 mbedtls 自带的 socket 封装（NET_C）都用不到：
 *   * NET_C 的 net_sockets.c 依赖 select/fcntl/getsockopt 的完整语义；我们直接用内核
 *     socket syscall 自写 send/recv 回调（mbedtls_ssl_set_bio），少一整层不确定性。
 *   * SSL_SRV 关掉后 libssl 小一截，也少一类"服务端专有"代码路径的链接风险。 */
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_NET_C

/* ==================== ③ 保留但明确下来（不要跟着上游默认值漂）====================
 * 证书链校验要这几项，它们本来是默认打开的，这里**显式**写出来，免得将来上游改默认值：
 *   MBEDTLS_X509_CRT_PARSE_C  PEM/DER 解析 + 链校验
 *   MBEDTLS_SSL_PROTO_TLS1_2  3.6 的默认协议（TLS 1.3 在 3.6 里默认是**关**的，
 *                             打开需要另一组 KEY_EXCHANGE_MODE 宏；本轮先只吃 1.2，
 *                             下一轮再开 1.3 并用 httpsprobe 的两个站点点名验证）
 *   MBEDTLS_HAVE_TIME          证书有效期判定需要 time()（见 kernel 侧 rtc_epoch64）。*/
#ifndef MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CRT_PARSE_C
#endif
#ifndef MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_2
#endif
#ifndef MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME
#endif

#endif /* VIMTU_MBEDTLS_USER_CONFIG_H */
