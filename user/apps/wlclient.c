/* wlclient.c - ★ A5：Wayland 基础骨架的 ring3 客户端
 *
 * 这条闭环（"客户端在用户态画进共享缓冲 -> 提交 -> 合成器显示"）就是本程序要跑通的东西：
 *   ① wl_seat_get()                        —— 拿座位（= 申请键盘焦点 + 指针捕获，复用 input_poll 的 flags）
 *   ② wl_surface_create() x2               —— A（160x96，**两块缓冲** = 两个 shm 对象 ping-pong）
 *                                             B（128x64，**一个对象里两块缓冲** = offset 0 / 32768）
 *   ③ shm_create() + shm_map()             —— 缓冲本体（用户态可读写的共享页）
 *   ④ 每帧：往当前缓冲里画（移动的彩色方块 + 一帧一个色带）-> wl_surface_damage -> wl_surface_attach
 *           -> wl_surface_commit -> wl_display_dispatch（合成 + 收 seat 事件）
 *   ⑤ seat：键/鼠标事件 -> 在事件路由到的那个 surface 上画小十字（鼠标）/ 换色带（按键）
 *   ⑥ wl_surface_destroy() x2 -> 退出（shm 引用归零 -> 页池回基线）
 *
 * 两种模式（与内核 kernel/wl64.cpp 的探针一致：**同一个文件**的存在性，两边各自判一次）：
 *   * short（没有 /etc/wl64_probe）：2 帧提交 + 立即销毁退出（~1 s）—— 给别的验收脚本用，别拖慢启动；
 *   * full （卷里有 /etc/wl64_probe）：本批验收用的完整 3 阶段：
 *       阶段 1：8 帧（每帧 ~700 ms；f%4==0 的帧用**整面** damage，其余帧只 damage 移动方块的新旧两块）；
 *       阶段 2：**写缓冲但不提交**（~2 s）-> 再**提交但不 dispatch**（~2 s）—— 两段屏幕都不许变；
 *       阶段 3：seat 监听（最长 ~45 s，收齐键 down/up + 移动 + 左键 down/up 就提前收尾）。
 *
 * 打点（tests/wl64_test.py 按这些串 grep；格式勿改）：
 *   [WLCLIENT] mode full=<0|1> probe=<0|1>
 *   [WLCLIENT] seat=<n>
 *   [WLCLIENT] surface a=<id> b=<id> awh=160x96 bwh=128x64
 *   [WLCLIENT] shm a0=<id> a1=<id> b=<id> amap=0x.. a1map=0x.. bmap=0x..
 *   [WLCLIENT] frame f=<n> full=<0|1> abuf=<0|1> bbuf=<0|1> acol=0x.. bcol=0x.. dmg=<x>,<y>,<w>,<h> sq=<x>,<y> cross=<surf>,<sx>,<sy>
 *   [WLCLIENT] nocommit f=<n> why=<no-commit|no-dispatch> acol=0x.. sq=<x>,<y>
 *   [WLCLIENT] seat n=<n> surf=<n> type=<n> code=0x.. x=<n> y=<n> sx=<n> sy=<n> inside=<0|1>
 *   [WLCLIENT] key code=0x.. surf=<n> band=<n> acol=0x.. bcol=0x..
 *   [WLCLIENT] cross surf=<n> sx=<n> sy=<n>
 *   [WLCLIENT] phase1 done frames=<n> ; [WLCLIENT] listen start ; [WLCLIENT] listen seen key=.. 
 *   [WLCLIENT] count key=<n> move=<n> down=<n> up=<n> wheel=<n>
 *   [WLCLIENT] destroy a=<ret> b=<ret>
 *   [WLCLIENT] done bytes=<n>
 * 键码：code 是既有键码（'a'=0x61 / Up=0xFD…，见 user/lib/vimtu64.h 的 V64_KEY_*）。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vimtu64.h"
#include "wl.h"

#define AW        160                 /* surface A 宽（160*96 = 15360 px <= 16384 px 上限） */
#define AH        96
#define BW        128                 /* surface B 宽（128*64 = 8192 px；一个对象放两块缓冲） */
#define BH        64
#define A_BYTES   (AW * AH * 4u)      /* 61440 B（15 页） */
#define B_BYTES   (BW * BH * 4u)      /* 32768 B（8 页）；B 的对象 = 两块 */
#define SQ        24                  /* 移动方块的边长 */
#define EV_MAX    32                  /* 一次 dispatch 最多收几条 seat 事件 */
#define LISTEN_MS 45000u              /* 阶段 3 的最长监听时间（收齐证据就提前走） */
#define PROBE     "/etc/wl64_probe"   /* 与 kernel/wl64.cpp 的 WL64_PROBE_PATH64 同一个文件 */

/* 色带（一帧一个）：0 = 蓝、1 = 橙、2..4 是按键切换出来的（见 key 分支） */
static const unsigned A_BANDS[5] = { 0x2A90E0u, 0xE0A020u, 0xC03050u, 0x30B070u, 0x8040D0u };
static const unsigned B_BANDS[5] = { 0x14A020u, 0x14A020u, 0x8020C0u, 0x2060C0u, 0xC08020u };
#define A_SQ      0xFFFFFFu           /* 移动方块：纯白 */
#define CROSS_COL 0x00FF40u           /* 鼠标小十字：绿色（与白色方块区分，测试按这个颜色断言） */

/* ---- 绘图（全部在**共享缓冲**里做，内核只管把它拷上屏）---- */
static inline unsigned px(unsigned rgb) { return 0xFF000000u | (rgb & 0xFFFFFFu); }

static void fill_band(unsigned char* buf, int w, int h, unsigned rgb) {
    const unsigned body = px(rgb);
    const unsigned head = px((rgb >> 1) & 0x7F7F7Fu);   /* 顶部 10 行稍暗（色带更看得出来） */
    for (int y = 0; y < h; y++) {
        unsigned* row = (unsigned*)(buf + (unsigned)y * (unsigned)w * 4u);
        const unsigned c = (y < 10) ? head : body;
        for (int x = 0; x < w; x++) row[x] = c;
    }
}

static void draw_square(unsigned char* buf, int w, int sx, int sy) {
    for (int y = 0; y < SQ; y++) {
        unsigned* row = (unsigned*)(buf + (unsigned)(sy + y) * (unsigned)w * 4u);
        for (int x = 0; x < SQ; x++) row[sx + x] = px(A_SQ);
    }
}

/* 小十字（9x9：中心 + 四个方向各 4 像素）——画在"事件路由到的那个 surface"上 */
static void draw_cross(unsigned char* buf, int w, int h, int cx, int cy) {
    const unsigned c = px(CROSS_COL);
    if (cx < 4 || cy < 4 || cx >= w - 4 || cy >= h - 4) return;
    for (int d = -4; d <= 4; d++) {
        unsigned* rv = (unsigned*)(buf + (unsigned)(cy + d) * (unsigned)w * 4u);
        rv[cx] = c;
        unsigned* rh = (unsigned*)(buf + (unsigned)cy * (unsigned)w * 4u);
        rh[cx + d] = c;
    }
}

/* ---- 一次提交（attach + damage + commit + dispatch）---- */
struct Surf {
    int id;
    int w, h;
    unsigned char* buf[2];      /* 两块缓冲的用户态 VA（ping-pong） */
    int shm[2];                 /* 两块缓冲所在的 shm 对象 id */
    unsigned int off[2];        /* 缓冲在对象里的字节偏移（B 是同一对象的 0 / 32768） */
};

/* 返回 commit 之后 dispatch 收到的事件条数（< 0 = 错误码） */
static int submit(struct Surf* s, int which, unsigned acol, int sqx, int sqy,
                  int has_cross, int cx, int cy, int full, int nrect,
                  const int* dx, const int* dy, const int* dw, const int* dh) {
    /* 画（在共享缓冲里）*/
    fill_band(s->buf[which], s->w, s->h, acol);
    if (sqx >= 0) draw_square(s->buf[which], s->w, sqx, sqy);
    if (has_cross) draw_cross(s->buf[which], s->w, s->h, cx, cy);

    int rc = wl_surface_attach(s->id, s->shm[which], (int)s->off[which]);
    if (rc != 0) return rc;
    if (full) {
        rc = wl_surface_damage(s->id, 0, 0, s->w, s->h);        /* damage = 整面 */
        if (rc != 0) return rc;
    } else {
        for (int i = 0; i < nrect; i++) {
            rc = wl_surface_damage(s->id, dx[i], dy[i], dw[i], dh[i]);
            if (rc != 0) return rc;
        }
    }
    rc = wl_surface_commit(s->id);
    if (rc != 0) return rc;
    struct Wl64SeatEvent ev[EV_MAX];
    return wl_display_dispatch(0, ev, EV_MAX);                      /* 0 = 不等待（拿到就合成）*/
}

/* ---- 探针：/etc/wl64_probe 存在 = 完整演示模式（与内核同一个判据）----
 * 走 Linux 兼容路径的 access(21)（自有 ABI 的 open(6) 只支持单层路径）。 */
static int probe_exists(void) {
    return __v64_syscall(21 /*access*/, (long)(unsigned long)PROBE, 0, 0, 0, 0) == 0 ? 1 : 0;
}

int main(void) {
    struct Surf A, B;
    struct Wl64SeatEvent ev[EV_MAX];
    int rc;

    memset(&A, 0, sizeof(A));
    memset(&B, 0, sizeof(B));

    /* 探针文件存在 = 完整演示模式；两边（内核 / 本程序）用的是**同一个判据** */
    const int full = probe_exists();
    printf("[WLCLIENT] mode full=%d probe=%d\n", full, full);

    /* ---- ① seat ---- */
    const int seat = wl_seat_get();
    printf("[WLCLIENT] seat=%d\n", seat);
    if (seat <= 0) { printf("[WLCLIENT] FAILED seat ret=%d\n", seat); return 1; }

    /* ---- ② surface ---- */
    A.id = wl_surface_create(AW, AH, V64_WL_FORMAT_XRGB8888);
    B.id = wl_surface_create(BW, BH, V64_WL_FORMAT_XRGB8888);
    printf("[WLCLIENT] surface a=%d b=%d awh=%dx%d bwh=%dx%d\n", A.id, B.id, AW, AH, BW, BH);
    if (A.id <= 0 || B.id <= 0) { printf("[WLCLIENT] FAILED surface a=%d b=%d\n", A.id, B.id); return 1; }
    A.w = AW; A.h = AH;
    B.w = BW; B.h = BH;

    /* ---- ③ 缓冲：A = 两个对象（各 15 页）；B = 一个对象，两块缓冲（off 0 / 32768）---- */
    A.shm[0] = shm_create(A_BYTES);
    A.shm[1] = shm_create(A_BYTES);
    B.shm[0] = shm_create(B_BYTES * 2u);
    B.shm[1] = B.shm[0];      /* ★ B 的两块缓冲在**同一个对象**里（off = 0 / 32768）*/
    printf("[WLCLIENT] shm a0=%d a1=%d b=%d\n", A.shm[0], A.shm[1], B.shm[0]);
    if (A.shm[0] <= 0 || A.shm[1] <= 0 || B.shm[0] <= 0) { printf("[WLCLIENT] FAILED shm\n"); return 1; }

    for (int i = 0; i < 2; i++) {
        void* va = 0;
        rc = shm_map(A.shm[i], 0, A_BYTES, &va);
        if (rc != 0 || !va) { printf("[WLCLIENT] FAILED shm_map a%d ret=%d\n", i, rc); return 1; }
        A.buf[i] = (unsigned char*)va;
        A.off[i] = 0;
    }
    {
        void* va = 0;
        rc = shm_map(B.shm[0], 0, B_BYTES * 2u, &va);
        if (rc != 0 || !va) { printf("[WLCLIENT] FAILED shm_map b ret=%d\n", rc); return 1; }
        B.buf[0] = (unsigned char*)va;
        B.buf[1] = (unsigned char*)va + B_BYTES;
        B.off[0] = 0;
        B.off[1] = B_BYTES;                       /* 32768，4KiB 对齐 */
    }
    printf("[WLCLIENT] amap=0x%x a1map=0x%x bmap=0x%x\n",
           (unsigned)(unsigned long)A.buf[0], (unsigned)(unsigned long)A.buf[1],
           (unsigned)(unsigned long)B.buf[0]);

    /* ---- 内部状态：上一帧的方块位置（damage 要覆盖"新旧两块"）---- */
    int prev_sqx = -1, prev_sqy = -1;
    int cross_surf = 0, cross_x = -1, cross_y = -1;   /* 最近一次鼠标事件路由到的 surface 与局部坐标 */
    int band = 0;                                    /* 当前色带（按键换） */
    unsigned long nkey = 0, nmove = 0, ndown = 0, nup = 0, nwheel = 0;
    int f = 0;
    int rc_last = 0;
    unsigned long total_bytes = 0;

    /* ==================== 阶段 1：帧序列（short 模式只有 2 帧）==================== */
    const int frames = full ? 8 : 2;
    for (; f < frames; f++) {
        const int afull = (f % 4 == 0);                     /* 每 4 帧一次整面 damage（换色带）*/
        const int band1 = (f / 4) % 2;                      /* 阶段 1 的色带：只在整面帧切换 */
        const int sqx = 8 + (f % 3) * 24;                   /* 方块在左上角 3x2 网格里移动 */
        const int sqy = 8 + (f % 2) * 24;
        const int abuf = f % 2;                             /* ping-pong 两块缓冲 */
        const int bbuf = f % 2;                             /* B：同一对象的两块 */
        const unsigned acol = A_BANDS[band1];
        const unsigned bcol = B_BANDS[band1];               /* B 的前两条色带相同：阶段 1 里 B 静止 */
        int dx[2], dy[2], dw[2], dh[2];
        int nrect = 0;
        if (!afull && prev_sqx >= 0) {                      /* 局部 damage = 旧方块 + 新方块 */
            dx[nrect] = prev_sqx; dy[nrect] = prev_sqy; dw[nrect] = SQ; dh[nrect] = SQ; nrect++;
            dx[nrect] = sqx;      dy[nrect] = sqy;      dw[nrect] = SQ; dh[nrect] = SQ; nrect++;
        }
        /* damage 包围盒（客户端自己算的，与内核的并集口径一致；打点给测试对账）*/
        int bx = 0, by = 0, bw = AW, bh = AH;
        if (!afull && nrect) {
            bx = sqx < prev_sqx ? sqx : prev_sqx;
            by = sqy < prev_sqy ? sqy : prev_sqy;
            const int ex = (sqx > prev_sqx ? sqx : prev_sqx) + SQ;
            const int ey = (sqy > prev_sqy ? sqy : prev_sqy) + SQ;
            bw = ex - bx; bh = ey - by;
        }
        rc = submit(&A, abuf, acol, sqx, sqy, 0, 0, 0, afull, nrect, dx, dy, dw, dh);
        if (rc < 0) { printf("[WLCLIENT] FAILED submit A ret=%d\n", rc); return 1; }
        rc_last = rc;
        /* B：**damage 为空 = 整面**（这一条语义由本调用验证；B 的内容只在按键时才变）*/
        rc = submit(&B, bbuf, bcol, -1, 0, 0, 0, 0, 0, 0, dx, dy, dw, dh);
        if (rc < 0) { printf("[WLCLIENT] FAILED submit B ret=%d\n", rc); return 1; }
        printf("[WLCLIENT] frame f=%d full=%d abuf=%d bbuf=%d acol=0x%x bcol=0x%x "
               "dmg=%d,%d,%d,%d sq=%d,%d cross=%d,%d,%d\n",
               f, afull, abuf, bbuf, acol, bcol, bx, by, bw, bh, sqx, sqy, cross_surf, cross_x, cross_y);
        prev_sqx = sqx; prev_sqy = sqy;
        total_bytes += (unsigned long)A_BYTES + B_BYTES;
        vimtu64_sleep_ms(full ? 700u : 120u);
    }
    printf("[WLCLIENT] phase1 done frames=%d\n", f);

    if (full) {
        /* ==================== 阶段 2a：写缓冲但**不提交**（屏幕必须不变）====================
         * 注意"写进当前已 attach 的那块缓冲"就等于在改客户端自己的内容：内核没收到 commit，
         * 后备缓冲/屏幕一个像素都不动（这就是"未提交不显示"）。 */
        const unsigned acol2 = 0x202020u;
        const int sqx2 = 120, sqy2 = 60;                    /* 故意挪到一个显眼的新位置 */
        const int abuf = f % 2;
        fill_band(A.buf[abuf], AW, AH, acol2);
        draw_square(A.buf[abuf], AW, sqx2, sqy2);
        rc = wl_surface_damage(A.id, 0, 0, AW, AH);         /* damage 也算上了，但**没有 commit** */
        printf("[WLCLIENT] nocommit f=%d why=no-commit acol=0x%x sq=%d,%d dmgret=%d\n",
               f, acol2, sqx2, sqy2, rc);
        vimtu64_sleep_ms(2000);

        /* ==================== 阶段 2b：提交了但**没 dispatch**（屏幕也必须不变）====================
         * commit 只是入队；合成发生在本进程下一次 wl_display_dispatch（异步提交模型）。 */
        rc = wl_surface_attach(A.id, A.shm[abuf], (int)A.off[abuf]);
        rc |= wl_surface_damage(A.id, 0, 0, AW, AH);
        rc |= wl_surface_commit(A.id);
        printf("[WLCLIENT] nocommit f=%d why=no-dispatch acol=0x%x sq=%d,%d dmgret=%d\n",
               f, acol2, sqx2, sqy2, rc);
        vimtu64_sleep_ms(2000);

        /* 收尾这一帧：真的 dispatch 一次（这时它才上屏，色带/方块变成本阶段画的那份）*/
        rc = wl_display_dispatch(0, ev, EV_MAX);
        printf("[WLCLIENT] frame f=%d full=1 abuf=%d bbuf=0 acol=0x%x bcol=0x%x "
               "dmg=0,0,%d,%d sq=%d,%d cross=%d,%d,%d flush=%d\n",
               f, abuf, acol2, B_BANDS[band], AW, AH, sqx2, sqy2, cross_surf, cross_x, cross_y, rc);
        prev_sqx = sqx2; prev_sqy = sqy2;
        f++;

        /* ==================== 阶段 3：seat 监听（键/鼠标）==================== */
        printf("[WLCLIENT] listen start\n");
        const unsigned long t0 = vimtu64_ticks();
        while ((vimtu64_ticks() - t0) < 250UL * (LISTEN_MS / 1000u)) {
            const int n = wl_display_dispatch(50, ev, EV_MAX);
            if (n < 0) { printf("[WLCLIENT] FAILED dispatch ret=%d\n", n); break; }
            int need_frame = 0;
            for (int i = 0; i < n; i++) {
                const struct Wl64SeatEvent* e = &ev[i];
                printf("[WLCLIENT] seat n=%d surf=%u type=%u code=0x%x x=%d y=%d sx=%d sy=%d inside=%u\n",
                       n, e->surf, e->ev.type, e->ev.code, e->ev.x, e->ev.y, e->sx, e->sy, e->inside);
                switch (e->ev.type) {
                case V64_EV_KEY_DOWN:
                    nkey++;
                    band = (int)(nkey % 3u) + 2;            /* 按键换色带（2/3/4 轮着来）*/
                    printf("[WLCLIENT] key code=0x%x surf=%u band=%d acol=0x%x bcol=0x%x\n",
                           e->ev.code, e->surf, band, A_BANDS[band], B_BANDS[band]);
                    need_frame = 1;
                    break;
                case V64_EV_MOUSE_MOVE:
                    nmove++;
                    if (e->surf && e->inside) {             /* 路由到自己的 surface 才画十字 */
                        cross_surf = (int)e->surf; cross_x = e->sx; cross_y = e->sy;
                        printf("[WLCLIENT] cross surf=%d sx=%d sy=%d\n", cross_surf, cross_x, cross_y);
                        need_frame = 1;
                    }
                    break;
                case V64_EV_MOUSE_DOWN: ndown++; break;
                case V64_EV_MOUSE_UP:   nup++;   break;
                case V64_EV_WHEEL:      nwheel++; break;
                default: break;
                }
            }
            if (need_frame) {
                /* 事件处理完立刻提交一帧：十字/色带随下一次 dispatch 上屏 */
                const int abuf = f % 2, bbuf = f % 2;
                int dx[1], dy[1], dw[1], dh[1];
                const unsigned acol = A_BANDS[band], bcol = B_BANDS[band];
                const int has_cross_a = (cross_surf == A.id);
                const int has_cross_b = (cross_surf == B.id);
                /* 按"事件属于哪个 surface"分别画十字：A 用整面 damage（简单可靠），B 走空 damage */
                rc = submit(&A, abuf, acol, prev_sqx, prev_sqy, has_cross_a, cross_x, cross_y,
                            1, 0, dx, dy, dw, dh);
                if (rc < 0) { printf("[WLCLIENT] FAILED submit A key ret=%d\n", rc); break; }
                rc = submit(&B, bbuf, bcol, -1, 0, has_cross_b, cross_x, cross_y,
                            0, 0, dx, dy, dw, dh);
                if (rc < 0) { printf("[WLCLIENT] FAILED submit B key ret=%d\n", rc); break; }
                printf("[WLCLIENT] frame f=%d full=1 abuf=%d bbuf=%d acol=0x%x bcol=0x%x "
                       "dmg=0,0,%d,%d sq=%d,%d cross=%d,%d,%d\n",
                       f, abuf, bbuf, acol, bcol, AW, AH, prev_sqx, prev_sqy,
                       cross_surf, cross_x, cross_y);
                f++;
            }
            /* 收齐"一次按键 + 一次移动 + 左键按下抬起"就走（省时间；收不齐就等满 LISTEN_MS）*/
            if (nkey >= 1 && nmove >= 1 && ndown >= 1 && nup >= 1) break;
            vimtu64_sleep_ms(20);
        }
        printf("[WLCLIENT] listen seen key=%lu move=%lu down=%lu up=%lu wheel=%lu\n",
               nkey, nmove, ndown, nup, nwheel);
    }

    /* ==================== 收尾：销毁 surface（内核把 shm 引用还回去）+ 退出 ==================== */
    const int da = wl_surface_destroy(A.id);
    const int db = wl_surface_destroy(B.id);
    printf("[WLCLIENT] destroy a=%d b=%d\n", da, db);
    printf("[WLCLIENT] count key=%lu move=%lu down=%lu up=%lu wheel=%lu\n", nkey, nmove, ndown, nup, nwheel);
    printf("[WLCLIENT] done bytes=%lu frames=%d dispatch_rc=%d\n", total_bytes, f, rc_last);
    return 0;
}
