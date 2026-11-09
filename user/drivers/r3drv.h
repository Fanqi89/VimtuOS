/* r3drv.h - ring3 驱动服务的用户态客户端（薄封装，**不引任何 libc**）
 *
 * 为什么分成两个 ABI 入口（这点必须先搞清，否则调用号一定错）：
 *   * `int 0x80`  = VimtuOS **自有**号段（V64_NR_*）：write(1)/exit/shopen/read/ticks/sleep_ms
 *                   + shm_create(13) / shm_map(14)。同一个数字在两条路径上含义不同。
 *   * `syscall`   = **Linux x86_64** 号段：open(2)/read(0)/write(1)/close(3)/lseek(8)…
 *                   + 本批新增的私有号 1029..1035（驱动框架）。
 *   本文件把两者都包好：`v80_*` = int 0x80，`lx_*` = syscall。
 *
 * 通道用法（服务侧的推荐写法，见 ext4svc.c）：
 *     const int id = v80_shm_create(2u<<20);        // 建对象（2 MiB）
 *     unsigned long va = 0;
 *     v80_shm_map(id, 0, 2u<<20, &va);              // 映进自己，va 可读写
 *     ring = (struct R3drvRing*)va;  ...            // [0,64) 环控制块
 *     sq   = (struct R3drvSqe*)(va + R3DRV_SQ_OFF);  // SQE 数组（就地完成）
 *     data = (unsigned char*)(va + R3DRV_DATA_OFF);  // 数据区（DMA 直达这里，零拷贝）
 *     lx_reg("ext4svc", R3DRV_PROTO_EXT4, disk, id, 2u<<20);
 *     zc_submit64(q, lba, secs, off, 0) + zc_wait64(...)
 */
#ifndef R3DRV_USER_H
#define R3DRV_USER_H

#include "../../kernel/r3drv_abi.h"     /* 内核与用户态**共用**这一份（POD + 宏，零依赖） */

typedef unsigned long long u64;
typedef long long          i64;
typedef unsigned int       u32;
typedef int                i32;
typedef unsigned short     u16;
typedef unsigned char      u8;

/* ---------------- int 0x80：VimtuOS 自有号段 ---------------- */
#define V80_NR_WRITE       1
#define V80_NR_EXIT        2
#define V80_NR_GETPID      3
#define V80_NR_TICKS       4
#define V80_NR_SLEEP_MS    5
#define V80_NR_OPEN        6
#define V80_NR_READ        7
#define V80_NR_CLOSE       8
#define V80_NR_SHM_CREATE  13
#define V80_NR_SHM_MAP     14

/* ---------------- syscall：Linux 号段（只用最基础的几个）---------------- */
#define LX_NR_READ         0
#define LX_NR_WRITE        1
#define LX_NR_OPEN         2
#define LX_NR_CLOSE        3
#define LX_NR_LSEEK        8
#define LX_NR_EXIT_GROUP   231

static inline i64 v80_1(long nr, i64 a1) {
    i64 r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "D"(a1) : "rcx", "r11", "memory");
    return r;
}
static inline i64 v80_2(long nr, i64 a1, i64 a2) {
    i64 r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
    return r;
}
static inline i64 v80_4(long nr, i64 a1, i64 a2, i64 a3, i64 a4) {
    register i64 r10 __asm__("r10") = a4;
    i64 r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

static inline i64 lx_3(long nr, i64 a1, i64 a2, i64 a3) {
    i64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}
static inline i64 lx_4(long nr, i64 a1, i64 a2, i64 a3, i64 a4) {
    register i64 r10 __asm__("r10") = a4;
    i64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}
static inline i64 lx_5(long nr, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5) {
    register i64 r10 __asm__("r10") = a4;
    register i64 r8  __asm__("r8")  = a5;
    i64 r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return r;
}

/* ---------------- 串口/控制台输出（走 Linux write(1)，与 vmtool 同一条路）---------------- */
static inline unsigned long r3_slen(const char* s) {
    unsigned long n = 0;
    if (s) while (s[n]) n++;
    return n;
}
static inline void r3_puts(const char* s) {
    if (s && s[0]) (void)lx_3(LX_NR_WRITE, 1, (i64)(unsigned long)s, (i64)r3_slen(s));
}
static inline void r3_putu(u64 v) {
    char b[24];
    int k = 0;
    if (v == 0) b[k++] = '0';
    while (v > 0) { b[k++] = (char)('0' + (v % 10)); v /= 10; }
    char o[24];
    int j = 0;
    while (k > 0) o[j++] = b[--k];
    o[j] = 0;
    r3_puts(o);
}
static inline void r3_puthex(u64 v) {
    static const char hx[] = "0123456789abcdef";
    char o[20];
    int j = 0;
    o[j++] = '0';
    o[j++] = 'x';
    for (int i = 60; i >= 0; i -= 4) o[j++] = hx[(v >> i) & 0xF];
    o[j] = 0;
    r3_puts(o);
}
#define R3_LINE_BEGIN  static char g_line[256]; int g_n = 0; g_line[0] = 0;
/* 行内拼接（避免串口行被抢占打断成两条；内核侧同一套做法：dbg64_line_begin/end） */
#define R3_ADD(s)      do { const char* _s = (s); while (*_s && g_n < 250) g_line[g_n++] = *_s++; g_line[g_n] = 0; } while (0)
#define R3_ADDU(v)     do { char _b[24]; u64 _v = (u64)(v); int _k = 0; if (!_v) _b[_k++] = '0';      \
                            while (_v) { _b[_k++] = (char)('0' + (_v % 10)); _v /= 10; }                \
                            while (_k > 0 && g_n < 250) g_line[g_n++] = _b[--_k]; g_line[g_n] = 0; } while (0)
#define R3_FLUSH()     do { (void)lx_3(LX_NR_WRITE, 1, (i64)(unsigned long)g_line, (i64)g_n); } while (0)

/* ---------------- 内核侧接口 ---------------- */
static inline i64 lx_reg(const char* name, u32 proto, u32 disk, int shm_id, u64 bytes) {
    return lx_5(R3DRV_NR_REGISTER, (i64)(unsigned long)name, (i64)proto, (i64)disk, (i64)shm_id, (i64)bytes);
}
static inline i64 lx_beat(void)      { return lx_3(R3DRV_NR_BEAT, 0, 0, 0); }
static inline i64 lx_kick(int id)    { return lx_3(R3DRV_NR_KICK, (i64)id, 0, 0); }
static inline i64 lx_dma(int id, u64 off, u64 lba, u32 secs) {
    return lx_4(R3DRV_NR_DMA, (i64)id, (i64)off, (i64)lba, (i64)secs);
}
static inline i64 lx_copy(int id, u64 off, u64 lba, u32 secs) {
    return lx_4(R3DRV_NR_COPY, (i64)id, (i64)off, (i64)lba, (i64)secs);
}
static inline i64 lx_status(u32 slot, struct R3drvInfo* out) {
    return lx_3(R3DRV_NR_STATUS, (i64)slot, (i64)(unsigned long)out, (i64)sizeof(struct R3drvInfo));
}
static inline i64 lx_route(u32 disk, u32 lba) {
    return lx_3(R3DRV_NR_ROUTE, (i64)disk, (i64)lba, 0);
}

/* ---------------- 环的两条原语（服务侧唯一该用的提交/等待姿势）---------------- */
static inline struct R3drvRing* r3_ring_of(unsigned long va) {
    return (struct R3drvRing*)va;
}
static inline struct R3drvSqe* r3_sq_of(unsigned long va) {
    return (struct R3drvSqe*)(va + R3DRV_SQ_OFF);
}
static inline void r3_ring_init(unsigned long va) {
    struct R3drvRing* r = r3_ring_of(va);
    r->slots = R3DRV_SLOTS;
    r->head = 0;
    r->tail = 0;
    r->kicks = 0;
    r->dma_bytes = 0;
    r->copy_bytes = 0;
    r->errs = 0;
    r->ver = R3DRV_ABI_VER;
    r->magic = R3DRV_RING_MAGIC;          /* ★ 最后写 magic：内核以它做"环已就绪"的判据 */
}
/* 提交一条请求（不 kick）。返回 SQE 下标。 */
static inline u32 r3_submit(unsigned long va, u64 lba, u32 secs, u64 obj_off, u32 flags) {
    struct R3drvRing* r = r3_ring_of(va);
    struct R3drvSqe*  q = r3_sq_of(va);
    const u32 pos = r->head & (R3DRV_SLOTS - 1u);
    q[pos].lba = lba;
    q[pos].obj_off = obj_off;
    q[pos].secs = secs;
    q[pos].flags = flags;
    q[pos].bytes = 0;
    q[pos].state = R3DRV_SQ_PENDING;      /* ★ 最后写 state：内核只认 PENDING */
    r->head++;
    return pos;
}
/* 等某条 SQE 完成（内核**就地**改 state）。返回落地字节数，<0 = 失败。
 * 为什么必须在这里 beat：内核的监督器按心跳判活，服务在长循环里不 beat 会被当挂掉重拉。 */
static inline i64 r3_wait(unsigned long va, u32 pos) {
    struct R3drvSqe* q = r3_sq_of(va);
    for (u32 spins = 0; spins < 4000000u; spins++) {
        const u32 st = q[pos].state;
        if (st == R3DRV_SQ_DONE)   return (i64)q[pos].bytes;
        if (st == R3DRV_SQ_FAILED) return -(i64)(q[pos].bytes ? q[pos].bytes : 1);
        if ((spins & 0xFFFFu) == 0) (void)lx_beat();
    }
    return -2;
}

#endif /* R3DRV_USER_H */
