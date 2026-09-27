/* libctest.c - ★ A2：用户态 C 运行时的自证程序（printf 子集 + malloc 压力 + POSIX 契约）
 *
 * 为什么单独一个程序（而不是塞进 hello.c）：验收脚本 tests/userlib64_test.py 要按行断言
 * 这三块证据，独立程序让"哪一条断言对应哪一段代码"一目了然；内核里它只是一个 ~几 KB 的 blob。
 *
 * 覆盖：
 *   1) printf 子集：%s %c %d %u %x %X %% 与宽度（%5d / %-5d / %05d / %8s / %-8s）；
 *   2) malloc/free/calloc/realloc 压力：分配 -> 写**内容哨兵**（每块自己的模式）-> 交错释放 ->
 *      复用空闲链表再分配（大小故意不同，逼出"切块"路径）-> 逐个校验内容哨兵与堆哨兵
 *      （vimtu64_heap_check()：块头 magic + 竞技场尾部 guard + 空闲链表升序）；
 *   3) POSIX 契约：getcwd（走 Linux 兼容路径 = syscall 指令）必须成功拿到 "/"；
 *      chdir 内核**没有**实现 -> 必须 -1 + errno=ENOSYS（如实，不假装换目录成功）。
 *
 * ★ 刻意**不调用** lseek(fd=1) 之类的失败调用：内核会打 [SYSCALL] deny，而 tests/elf64_test.py
 *   把 "[SYSCALL] deny" 列为禁止串（每一次启动都会跑到本程序）—— 详见报告里的说明。 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NBLK 24

int main(void) {
    /* ================= 1) printf 子集 ================= */
    printf("[LIBC] printf s=[%s] d=[%d] u=[%u] x=[%x] X=[%X] c=[%c] pct=[%%]\n",
           "vimtu64", -42, 4294967295u, 0xdeadbeefu, 0xdeadbeefu, 'Z');
    printf("[LIBC] width [%5d][%-5d][%05d][%8s][%-8s|]\n", 7, 7, 7, "ab", "ab");

    /* ================= 2) malloc/free 压力 ================= */
    static unsigned char* blk[NBLK];
    static unsigned      sz[NBLK];
    unsigned bad = 0;

    for (int i = 0; i < NBLK; i++) {
        sz[i] = 24u + (unsigned)(i % 7) * 16u;               /* 24..120 字节 */
        blk[i] = (unsigned char*)malloc(sz[i]);
        if (!blk[i]) { bad |= 1u; continue; }
        for (unsigned k = 0; k < sz[i]; k++) blk[i][k] = (unsigned char)(0xA5u ^ (unsigned)(i * 31 + (int)k));
    }
    for (int i = 0; i < NBLK; i += 2) { free(blk[i]); blk[i] = 0; }        /* 交错释放：留洞 */

    for (int i = 0; i < NBLK; i += 2) {                                    /* 复用空闲链表（大小 +8）*/
        sz[i] += 8u;
        blk[i] = (unsigned char*)malloc(sz[i]);
        if (!blk[i]) { bad |= 2u; continue; }
        for (unsigned k = 0; k < sz[i]; k++) blk[i][k] = (unsigned char)(0xA5u ^ (unsigned)(i * 31 + (int)k));
    }
    for (int i = 0; i < NBLK; i++) {                                       /* 内容哨兵校验 */
        if (!blk[i]) { bad |= 4u; continue; }
        for (unsigned k = 0; k < sz[i]; k++) {
            if (blk[i][k] != (unsigned char)(0xA5u ^ (unsigned)(i * 31 + (int)k))) { bad |= 4u; break; }
        }
    }

    unsigned char* z = (unsigned char*)calloc(64, 1);                      /* calloc：必须全 0 */
    if (!z) {
        bad |= 8u;
    } else {
        for (int k = 0; k < 64; k++) if (z[k] != 0) { bad |= 8u; break; }
        free(z);
    }
    unsigned char* r = (unsigned char*)malloc(16);                         /* realloc：老内容必须搬过去 */
    if (!r) {
        bad |= 16u;
    } else {
        memset(r, 0x5A, 16);
        unsigned char* r2 = (unsigned char*)realloc(r, 128);
        if (!r2) { bad |= 16u; free(r); } else {
            for (int k = 0; k < 16; k++) if (r2[k] != 0x5A) { bad |= 16u; break; }
            free(r2);
        }
    }

    for (int i = 0; i < NBLK; i++) { free(blk[i]); blk[i] = 0; }          /* 全部归还 */
    unsigned used = 0, total = 0, live = 0;
    vimtu64_heap_stats(&used, &total, &live);
    printf("[LIBC] malloc stress blocks=%d bad=0x%x heap_check=%u used=%u total=%u live=%u\n",
           NBLK, bad, vimtu64_heap_check(), used, total, live);

    /* ================= 3) POSIX 契约（两条路径各一条证据）================= */
    char cwd[64];
    cwd[0] = 0;
    char* g = getcwd(cwd, sizeof(cwd));               /* Linux 兼容路径（syscall 指令，nr=79）*/
    errno = 0;
    const int cr = chdir("/");                        /* 内核没有 chdir -> 必须如实失败 */
    const int ce = errno;
    printf("[LIBC] posix getcwd=[%s] chdir=%d errno=%d\n", g ? cwd : "(fail)", cr, ce);

    printf("[LIBC] printf after stress ok\n");        /* 压力之后 printf 仍然正常（行缓冲没被踩坏）*/
    return 0;
}
