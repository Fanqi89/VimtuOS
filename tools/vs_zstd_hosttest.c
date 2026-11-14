/* tools/vs_zstd_hosttest.c - vs_zstd.c 的**宿主自测**（不进系统卷、不进构建产物）
 *
 * 为什么要它：zstd 解码器是"格式对不对"的问题，只有拿**真 zstd 编出来的字节流**
 *   逐字节比对才算证据。把 vs_zstd.c 编到用户态 ELF 里再进 QEMU 试，一次迭代要
 *   1~15 分钟；而它是零依赖的 C，宿主 gcc 直接编就能在 1 秒内跑完几百个样本。
 *
 * 用法（宿主）：
 *   gcc -O2 -Wall -Wextra -o vs_zstd_hosttest.exe tools/vs_zstd_hosttest.c
 *   vs_zstd_hosttest.exe <a.zst> <a.orig> [<b.zst> <b.orig> ...]
 *   退出码：0 全部逐字节一致 / 1 有样本失败或错误路径没被判住 / 2 用法或环境
 *
 * 判据（每条都能独立证伪）：
 *   A. 解出来的长度 == 原始文件长度；
 *   B. 解出来的内容与原始文件**逐字节一致**；
 *   C. 把压缩流截断 8 字节后必须**失败**（负向对照：证明不是在"假装成功"）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user/store/vs_zstd.c"     /* 直接编进来：同一份源码，零改动 */

static unsigned char* slurp(const char* path, long* out_len) {
    FILE* f = fopen(path, "rb");
    unsigned char* p;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    p = (unsigned char*)malloc((size_t)n + 16);
    if (!p) { fclose(f); return NULL; }
    if (n > 0 && fread(p, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(p); return NULL; }
    fclose(f);
    *out_len = n;
    return p;
}

int main(int argc, char** argv) {
    int i;
    int bad = 0;
    void* work = malloc((size_t)VS_ZSTD_WORK_BYTES);
    unsigned char* lit = (unsigned char*)malloc(128 * 1024);
    if (argc < 3 || ((argc - 1) & 1)) {
        fprintf(stderr, "usage: %s <a.zst> <a.orig> [<b.zst> <b.orig> ...]\n", argv[0]);
        return 2;
    }
    if (!work || !lit) { fprintf(stderr, "out of memory\n"); return 2; }

    for (i = 1; i + 1 < argc; i += 2) {
        long zn = 0, on = 0;
        unsigned char* z = slurp(argv[i], &zn);
        unsigned char* orig = slurp(argv[i + 1], &on);
        unsigned char* dst;
        const char* why = "?";
        int at = 0, got = 0, rc;

        if (!z || !orig) { fprintf(stderr, "cannot read %s / %s\n", argv[i], argv[i + 1]); return 2; }
        dst = (unsigned char*)malloc((size_t)on + 64);
        if (!dst) { fprintf(stderr, "out of memory\n"); return 2; }

        rc = vs_zstd_decode(z, (int)zn, dst, (int)on, lit, 128 * 1024,
                            work, &got, &why, &at);
        if (rc != 0) {
            printf("  [FAIL] %-40s decode rc=%d reason=%s at=%d\n", argv[i], rc, why, at);
            bad = 1;
        } else if (got != (int)on) {
            printf("  [FAIL] %-40s size %d != %ld\n", argv[i], got, on);
            bad = 1;
        } else if (memcmp(dst, orig, (size_t)on) != 0) {
            long k = 0;
            while (k < on && dst[k] == orig[k]) k++;
            printf("  [FAIL] %-40s first diff at %ld (got %02x want %02x)\n",
                   argv[i], k, dst[k], orig[k]);
            bad = 1;
        } else {
            printf("  [ok]   %-40s %8ld B -> %8ld B (ratio %.2f)\n",
                   argv[i], zn, on, (double)zn / (double)(on ? on : 1));
        }

        /* 负向对照：截断必须失败（证明没有"解不动也返回 0"） */
        if (zn > 16) {
            const char* why2 = "?";
            int at2 = 0, got2 = 0;
            const int rc2 = vs_zstd_decode(z, (int)zn - 8, dst, (int)on, lit, 128 * 1024,
                                           work, &got2, &why2, &at2);
            if (rc2 == 0 && got2 == (int)on && memcmp(dst, orig, (size_t)on) == 0) {
                printf("  [FAIL] %-40s truncated-by-8 still decoded ok (no negative control)\n", argv[i]);
                bad = 1;
            } else {
                printf("  [ok]   %-40s truncated-by-8 rejected (%s)\n", argv[i], why2);
            }
        }
        free(z); free(orig); free(dst);
    }
    free(work); free(lit);
    printf("RESULT: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
