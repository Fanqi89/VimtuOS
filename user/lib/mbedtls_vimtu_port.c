/* mbedtls_vimtu_port.c - ★ v0.12.4：mbedTLS 的 VimtuOS 移植层（熵源 + 自检）
 *
 * 这个文件是整个"能不能真跑 TLS"的地基，只有一件事：**给 mbedTLS 提供密码学级熵**。
 *
 * 为什么必须自己做（不是可选项）：
 *   mbedTLS 默认的熵源是 mbedtls_platform_entropy_poll()，它开的是 /dev/urandom。
 *   VimtuOS 没有 /dev/urandom（没有 devtmpfs 那一套），于是
 *     mbedtls_entropy_func() -> 拿不到熵 -> mbedtls_ctr_drbg_seed() 失败
 *     -> 握手在发出 ClientHello 之前就退出（现场现象是"连得上、握不上"，很容易被误当成协议实现不对）。
 *   所以 user/lib/mbedtls_vimtu_user_config.h 里同时打开
 *     MBEDTLS_ENTROPY_HARDWARE_ALT（把熵源换成下面这个函数）
 *     MBEDTLS_NO_PLATFORM_ENTROPY（彻底不要 /dev/urandom 那条路）。
 *
 * 熵从哪来：getrandom(318) -> kernel/syscall64.cpp 的 lx64_getrandom64()
 *   -> kernel/rand64.cpp 的 rand64_bytes64()（v0.12.3 落地）：
 *     * 有 RDSEED/RDRAND 就直取（每 1 KiB 用 RDSEED 重播一次）；
 *     * 没有就 SHA-256 计数器 DRBG（出块即 rekey，丢掉第一块），种子来自 TSC 抖动 / RDRAND。
 *   本文件**不自己做任何混洗/推导**：上游 mbedTLS 的 entropy.c 已经会把硬件熵喂进 CTR_DRBG，
 *   在这里再造一层只会让"熵到底从哪来"更难查。
 *
 * 契约（照 include/mbedtls/entropy_poll.h 的说明，一条都不许放宽）：
 *   * 返回 0 = 成功；`*olen` = 实际写进 output 的字节数；
 *   * len == 0 时：**必须** `*olen = 0` 且返回 0（上游 entropy_gather 会用它探测上限）；
 *   * 失败时返回非 0（负数或 MBEDTLS_ERR_ENTROPY_SOURCE_FAILED），并**不许**把 output 说成有效；
 *   * 不许返回"全 0"当作成功（那等于把密钥变成常量）—— 这里额外做一次全 0 检查。
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/random.h>          /* musl：getrandom() */

#include "mbedtls/entropy.h"      /* MBEDTLS_ERR_ENTROPY_SOURCE_FAILED（3.6 里在公共头，不在 entropy_poll.h） */

/* mbedtls_hardware_poll 的声明：3.6 把它放在**私有**头 library/entropy_poll.h（entropy.c 自己 include），
 * 公共头里只有配置说明。我们是它的**实现方**，所以在这里给出与私有头逐字一致的原型。 */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);

/* Linux 的 getrandom(2) 单次上限是 256 字节（内核 SYSCALL_DEFINE3 的 EINVAL 阈值）；
 * musl 的 getrandom() 会自己按 256 拆分，但这里**不依赖**它：我们显式按 256 分段，
 * 这样"内核侧上限是多少"这件事不会变成一个隐藏假设（本内核 v0.12.3 已把上限放到 4 KiB，
 * 但用户态代码不该踩着这个数字写）。 */
#define VIMTU_HWPOLL_CHUNK 256u

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    if (olen == NULL) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    *olen = 0;
    if (len == 0) return 0;                       /* 契约：len==0 -> *olen=0 且返回 0 */
    if (output == NULL) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;

    size_t got = 0;
    while (got < len) {
        size_t want = len - got;
        if (want > VIMTU_HWPOLL_CHUNK) want = VIMTU_HWPOLL_CHUNK;
        const ssize_t r = getrandom(output + got, want, 0);
        if (r <= 0) {
            /* EINTR 重试一次就够：本系统的 getrandom 不会阻塞（没有熵池耗尽的概念） */
            if (r < 0 && errno == EINTR) continue;
            *olen = 0;
            return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        }
        got += (size_t)r;
    }

    /* 全 0 检查：CSPRNG 有"永不返回全 0"的承诺（kernel/rand64.cpp），这里做一次交叉验证。
     * 注意它**不是**随机性检验，只挡"设备/系统调用被桩化"这种整块 0 事故。 */
    unsigned char acc = 0;
    for (size_t i = 0; i < got; i++) acc |= output[i];
    if (acc == 0 && got >= 8) { *olen = 0; return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED; }

    *olen = got;
    return 0;
}

/* ==================== 自检（给 httpsprobe 用；同时在启动路径里便宜可查）====================
 * 判据（三条，都写进串口）：
 *   ① 两次 32 字节输出**必须不同**（不是"看起来随机"，是"两次调用没有返回同一块"）；
 *   ② len=0 -> *olen=0 且返回 0（上游契约；写错会让 entropy_gather 卡死或死循环）；
 *   ③ 32 字节里至少有一个非 0 字节。
 * 返回位掩码：bit0=①失败，bit1=②失败，bit2=③失败；0 = 全过。 */
unsigned vimtu_mbedtls_hwpoll_selftest(unsigned char* out_a, unsigned char* out_b, size_t n)
{
    unsigned fails = 0;
    size_t oa = 0, ob = 0, oz = 99;

    if (mbedtls_hardware_poll(NULL, out_a, n, &oa) != 0 || oa != n) fails |= 1u;
    if (mbedtls_hardware_poll(NULL, out_b, n, &ob) != 0 || ob != n) fails |= 1u;
    if (fails == 0 && memcmp(out_a, out_b, n) == 0) fails |= 1u;      /* ① */

    oz = 99;
    if (mbedtls_hardware_poll(NULL, out_a, 0, &oz) != 0 || oz != 0) fails |= 2u;   /* ② */

    unsigned char acc = 0;
    for (size_t i = 0; i < n; i++) acc |= out_b[i];
    if (acc == 0) fails |= 4u;                                        /* ③ */
    return fails;
}
