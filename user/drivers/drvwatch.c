/* drvwatch.c - ★ v0.9.33：ring3 **看门狗观察进程**
 *
 * 先把边界说死（这条比代码重要）：
 *   **真正把崩掉的驱动重新拉起来的是内核**（kernel/r3drv64.cpp 的 r3drv64_tick64，
 *   跑在 kheart 内核线程里）。原因很朴素：一个 ring3 进程连"另一个进程还活着没有"
 *   都要靠内核告诉它，让它去"重启别人"是把可靠性倒着设计。
 *   所以本进程的职责是**观察与取证**：
 *     * 周期性读 /proc/vimtu/drivers（内核渲染的只读合成表）；
 *     * 把"服务在不在、重启过几次、零拷贝搬了多少字节、心跳到哪了"打进串口；
 *     * 发现状态异常（st != RUN）时明确报出来，让验收脚本能抓到。
 *   —— 这也正好证明那条合成节点是真数据：它的数字要与内核 [R3DRV] 打点对得上。
 *
 * 串口行（tests/r3drv64_test.py 断言）：
 *   [DRVWATCH] round=<n> slots=<n> restarts=<n> kicks=<n>
 *   [DRVWATCH] slot<n> name=<s> proto=<n> disk=<n> st=<n> pid=<n> beats=<n> rst=<n> dma=<n> cpy=<n>
 *   [DRVWATCH] ALERT slot=<n> state=<n>（非 RUN 才会打）
 *   [DRVWATCH] done rounds=<n>
 */
#include "r3drv.h"

#define WATCH_ROUNDS     60
#define WATCH_SLEEP_MS   1000

/* C 语言：kv_u64/kv_str 用到它，必须先声明（本文件只有这两个小函数依赖它） */
static unsigned long slen_(const char* s);
#define WATCH_PROC_PATH  "/proc/vimtu/drivers"

/* 从 "key=value" 形式的内核文本里取一个整数值（找不到 -> def）。
 * 为什么不引 sscanf：本进程是无 libc 的 freestanding。 */
static u64 kv_u64(const char* s, unsigned long n, const char* key, u64 def) {
    const unsigned long klen = slen_(key);
    for (unsigned long i = 0; i + klen < n; i++) {
        unsigned long j = 0;
        while (j < klen && s[i + j] == key[j]) j++;
        if (j != klen) continue;
        unsigned long p = i + klen;
        u64 v = 0;
        int any = 0;
        while (p < n && s[p] >= '0' && s[p] <= '9') { v = v * 10 + (u64)(s[p] - '0'); p++; any = 1; }
        return any ? v : def;
    }
    return def;
}
/* 取字符串值（写到 out，返回长度；找不到 -> 0） */
static unsigned long kv_str(const char* s, unsigned long n, const char* key, char* out, unsigned long cap) {
    const unsigned long klen = slen_(key);
    for (unsigned long i = 0; i + klen < n; i++) {
        unsigned long j = 0;
        while (j < klen && s[i + j] == key[j]) j++;
        if (j != klen) continue;
        unsigned long p = i + klen, k = 0;
        while (p < n && s[p] && s[p] != ' ' && s[p] != '\n' && k + 1 < cap) out[k++] = s[p++];
        out[k] = 0;
        return k;
    }
    if (cap) out[0] = 0;
    return 0;
}

static unsigned long slen_(const char* s) {
    unsigned long n = 0;
    if (s) while (s[n]) n++;
    return n;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    static char buf[512];
    u32 rounds = 0;
    for (u32 r = 0; r < WATCH_ROUNDS; r++) {
        const i64 fd = lx_3(LX_NR_OPEN, (i64)(unsigned long)WATCH_PROC_PATH, 0, 0);
        if (fd < 0) {
            R3_LINE_BEGIN R3_ADD("[DRVWATCH] OPEN FAILED path="); R3_ADD(WATCH_PROC_PATH);
            R3_ADD("\n"); R3_FLUSH();
            return 1;
        }
        const i64 n = lx_3(LX_NR_READ, fd, (i64)(unsigned long)buf, (i64)(sizeof(buf) - 1));
        (void)lx_3(LX_NR_CLOSE, fd, 0, 0);
        if (n <= 0) {
            R3_LINE_BEGIN R3_ADD("[DRVWATCH] READ empty\n"); R3_FLUSH();
        } else {
            buf[n] = 0;
            const unsigned long len = (unsigned long)n;
            const u64 slots    = kv_u64(buf, len, "slots=", 0);
            const u64 restarts = kv_u64(buf, len, "restarts=", 0);
            const u64 kicks    = kv_u64(buf, len, "kicks=", 0);
            R3_LINE_BEGIN
            R3_ADD("[DRVWATCH] round=");   R3_ADDU((u64)r);
            R3_ADD(" slots=");             R3_ADDU(slots);
            R3_ADD(" restarts=");          R3_ADDU(restarts);
            R3_ADD(" kicks=");             R3_ADDU(kicks);
            R3_ADD("\n");                  R3_FLUSH();
            /* 逐个槽：内核每行以 "s<idx> name=..." 开头 */
            for (unsigned long i = 0; i + 2 < len; i++) {
                if (buf[i] != 's') continue;
                if (i != 0 && buf[i - 1] != '\n') continue;
                if (buf[i + 1] < '0' || buf[i + 1] > '9') continue;
                /* 该行范围 */
                unsigned long e = i;
                while (e < len && buf[e] != '\n') e++;
                const char* line = buf + i;
                const unsigned long ll = e - i;
                char name[24];
                kv_str(line, ll, "name=", name, sizeof(name));
                const u64 st  = kv_u64(line, ll, "st=", 0xFF);
                const u64 pid = kv_u64(line, ll, "pid=", 0);
                const u64 bt  = kv_u64(line, ll, "beats=", 0);
                const u64 rst = kv_u64(line, ll, "rst=", 0);
                const u64 dma = kv_u64(line, ll, "dma=", 0);
                const u64 cpy = kv_u64(line, ll, "cpy=", 0);
                const u64 disk = kv_u64(line, ll, "disk=", 0);
                const u64 proto = kv_u64(line, ll, "proto=", 0);
                R3_LINE_BEGIN
                R3_ADD("[DRVWATCH] slot");     R3_ADDU((u64)(buf[i + 1] - '0'));
                R3_ADD(" name=");              R3_ADD(name);
                R3_ADD(" proto=");             R3_ADDU(proto);
                R3_ADD(" disk=");              R3_ADDU(disk);
                R3_ADD(" st=");                R3_ADDU(st);
                R3_ADD(" pid=");               R3_ADDU(pid);
                R3_ADD(" beats=");             R3_ADDU(bt);
                R3_ADD(" rst=");               R3_ADDU(rst);
                R3_ADD(" dma=");               R3_ADDU(dma);
                R3_ADD(" cpy=");               R3_ADDU(cpy);
                R3_ADD("\n");                  R3_FLUSH();
                if (st != R3DRV_ST_RUN) {      /* 2 = RUN；其余都要报出来 */
                    R3_LINE_BEGIN
                    R3_ADD("[DRVWATCH] ALERT slot="); R3_ADDU((u64)(buf[i + 1] - '0'));
                    R3_ADD(" state=");                R3_ADDU(st);
                    R3_ADD("\n");                     R3_FLUSH();
                }
                i = e;
            }
        }
        rounds++;
        (void)v80_2(V80_NR_SLEEP_MS, WATCH_SLEEP_MS, 0);
    }
    R3_LINE_BEGIN R3_ADD("[DRVWATCH] done rounds="); R3_ADDU((u64)rounds); R3_ADD("\n"); R3_FLUSH();
    return 0;
}
