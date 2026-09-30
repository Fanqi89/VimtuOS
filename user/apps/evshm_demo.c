/* evshm_demo.c - ★ A5 前置：用户态输入事件投递（自有 ABI 12）+ 共享内存缓冲（13/14）的 ring3 演示
 *
 * 交付判据（tests/ipc64_test.py 按这些串口打点断言）：
 *   [EVSHM] fb va=0x.. w=.. h=.. pitch=.. fmt=..        fb_map(9)：后备缓冲映射（提交矩形要用它）
 *   [EVSHM] shm_create size=65536 ret=<id>              shm_create(13)：跨进程可共享的对象 id
 *   [EVSHM] shm_map id=.. ret=0 va=0x..                 shm_map(14)：映射进本进程（用户可读写）
 *   [EVSHM] pattern bytes=.. csum=0x........            共享缓冲里的矩形 + 图案的校验和（父进程算的）
 *   [EVSHM] flip ret=0 x=.. y=.. w=.. h=..             fb_flip(10)：把共享缓冲里的矩形提交上屏
 *   [EVSHM] child shm_map ret=0 va=0x..                 子进程用**继承到的句柄**映射同一块内存
 *   [EVSHM] child csum=0x........ match=1               子进程逐字节核对（csum 与父进程相同）
 *   [EVSHM] child magic=0xCAFEBABE                      子进程写魔数（共享是双向的）
 *   [EVSHM] parent magic=0xCAFEBABE ok=1                父进程读回（不是\"各一份拷贝\"）
 *   [EVSHM] listen pend=<n>                             max=0 查询待取条数 + 申请焦点/指针捕获
 *   [EVSHM] ev t=.. code=0x.. x=.. y=.. dx=.. dy=.. btn=0x.. mods=0x.. t=..  每条事件的原文
 *   [EVSHM] hold ms=6000                                暂停 6 秒不取事件（让队列被灌满 -> 丢最旧）
 *   [EVSHM] key=1 mouse=3 wheel=0                       按类型计数（规格里给的形状）
 *   [EVSHM] done
 *
 * 为什么是\"真 ELF64 进程\"而不是平铺 blob：本演示要 fork(57)（父子两个进程共享同一块 shm），
 * 而 fork 要求当前任务有**进程上下文**（kernel/proc64.cpp 的 p64_current64）。blob 路径
 * （user64_run_capp64）没有进程上下文 -> fork 返回 -ENOSYS。所以本程序由 kernel/proc64.cpp
 * 幂等装进系统卷（/evshm.elf）再以真进程跑（与 /proc64.elf、/pipe64.elf 同一套交付方式）。
 *
 * 键码口径：code 是**既有键码**（可打印 ASCII / 控制字符 / Esc 0x1B / NAV_* 0xFB..0xFE /
 * KBD_KEY_* 0xF7..0xFA），见 user/lib/vimtu64.h 的 V64_KEY_*。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fb.h"
#include "vimtu64.h"

#define SHM_BYTES   65536u          /* 共享缓冲大小（= 内核 SHM64_MAX_PAGES64 * 4KiB） */
#define RECT_W      256u            /* 画在共享缓冲里的彩色矩形（也是 fb_flip 提交的区域） */
#define RECT_H      64u
#define RECT_X      100u            /* 提交到屏幕的位置 */
#define RECT_Y      120u
#define EV_BATCH    16u             /* 单次 input_poll 最多取几条（队列容量是 32） */
#define LISTEN_MS   40000u          /* 阶段 1 的最长等待（等测试注入键鼠，正常 1~2 秒就够） */
#define HOLD_MS     6000u           /* 阶段 2：故意不取事件，让内核队列被灌满（验证丢最旧） */

static struct Ev64Event g_ev[EV_BATCH];
static unsigned g_key_down, g_key_up, g_move, g_mouse_down, g_mouse_up, g_wheel;
static unsigned g_ev_lines;         /* [EVSHM] ev 行上限（防刷屏；超过只计数） */

/* ---------------- 小工具 ---------------- */
static unsigned csum32(const unsigned char* p, unsigned n) {
    unsigned s = 0x12345678u;
    for (unsigned i = 0; i < n; i++) s = (s ^ p[i]) * 16777619u;   /* FNV-1a 风格的 32 位校验和 */
    return s;
}

static const char* ev_name(unsigned t) {
    switch (t) {
    case V64_EV_KEY_DOWN:   return "KEY_DOWN";
    case V64_EV_KEY_UP:     return "KEY_UP";
    case V64_EV_MOUSE_MOVE: return "MOVE";
    case V64_EV_MOUSE_DOWN: return "MOUSE_DOWN";
    case V64_EV_MOUSE_UP:   return "MOUSE_UP";
    case V64_EV_WHEEL:      return "WHEEL";
    default:                return "?";
    }
}

/* 收到的事件按类型计数 + 打印原文（code/x/y/dx/dy/buttons/mods/时间戳） */
static void account(const struct Ev64Event* e) {
    switch (e->type) {
    case V64_EV_KEY_DOWN:   g_key_down++;   break;
    case V64_EV_KEY_UP:     g_key_up++;     break;
    case V64_EV_MOUSE_MOVE: g_move++;       break;
    case V64_EV_MOUSE_DOWN: g_mouse_down++; break;
    case V64_EV_MOUSE_UP:   g_mouse_up++;   break;
    case V64_EV_WHEEL:      g_wheel++;      break;
    default: break;
    }
    if (g_ev_lines < 96u) {
        g_ev_lines++;
        printf("[EVSHM] ev t=%u(%s) code=0x%x x=%d y=%d dx=%d dy=%d btn=0x%x mods=0x%x t=%lu\n",
               e->type, ev_name(e->type), e->code, e->x, e->y, e->dx, e->dy,
               e->buttons, e->mods, (unsigned long)e->t_ms);
    }
}

/* 取一批事件并计数；返回取到的条数（<= 0 = 没有/出错） */
static int pump(unsigned flags) {
    const int n = poll_event(g_ev, EV_BATCH, flags);
    if (n < 0) {
        printf("[EVSHM] input_poll FAILED ret=%d\n", n);
        return n;
    }
    for (int i = 0; i < n; i++) account(&g_ev[i]);
    return n;
}

int main(void) {
    uint32_t*      fb = 0;
    struct Fb64Info info;

    /* ---- (1) fb_map：拿到后备缓冲的用户 VA（提交矩形的必经之路） ---- */
    const int frc = fb_map(&fb, &info);
    if (frc != 0) {
        printf("[EVSHM] fb_map FAILED ret=%d\n", frc);
        return 1;
    }
    printf("[EVSHM] fb va=0x%lx w=%u h=%u pitch=%u fmt=%u\n",
           (unsigned long)info.va, info.width, info.height, info.pitch, info.format);

    /* ---- (2) shm_create + shm_map：一块跨进程可共享的内存 ---- */
    const int id = shm_create(SHM_BYTES);
    printf("[EVSHM] shm_create size=%u ret=%d\n", SHM_BYTES, id);
    if (id <= 0) { printf("[EVSHM] shm FAILED (create ret=%d)\n", id); return 1; }

    void* vp = 0;
    const int mrc = shm_map(id, 0, SHM_BYTES, &vp);
    printf("[EVSHM] shm_map id=%d ret=%d va=0x%lx\n", id, mrc, (unsigned long)vp);
    if (mrc != 0 || !vp) { printf("[EVSHM] shm FAILED (map ret=%d)\n", mrc); return 1; }
    unsigned char* shm = (unsigned char*)vp;

    /* ---- (3) 往共享缓冲里画一块彩色矩形（+ 逐像素图案，方便逐字节核对） ---- */
    for (unsigned y = 0; y < RECT_H; y++) {
        unsigned* row = (unsigned*)(shm + (unsigned long)y * RECT_W * 4u);
        for (unsigned x = 0; x < RECT_W; x++) {
            /* 0xAARRGGBB：不透明；颜色随 (x,y) 变化（图案 = 逐字节可核对的内容） */
            row[x] = 0xFF000000u | ((x & 0xFFu) << 16) | ((y & 0xFFu) << 8) | ((x ^ y) & 0xFFu);
        }
    }
    const unsigned csum = csum32(shm, SHM_BYTES);
    printf("[EVSHM] pattern bytes=%u csum=0x%08x\n", SHM_BYTES, csum);

    /* ---- (4) 把共享缓冲里的矩形拷进后备缓冲并提交（buffer -> commit 的最小闭环） ---- */
    for (unsigned y = 0; y < RECT_H; y++) {
        unsigned* dst = (unsigned*)((unsigned char*)fb + (unsigned long)(RECT_Y + y) * info.pitch);
        memcpy(dst + RECT_X, shm + (unsigned long)y * RECT_W * 4u, RECT_W * 4u);
    }
    const int flip = fb_flip((int)RECT_X, (int)RECT_Y, (int)RECT_W, (int)RECT_H);
    printf("[EVSHM] flip ret=%d x=%u y=%u w=%u h=%u\n", flip, RECT_X, RECT_Y, RECT_W, RECT_H);

    /* ---- (5) fork：第二个进程用继承到的句柄映射**同一块**内存（Wayland 的 buffer 语义） ---- */
    const long cpid = __v64_syscall(57 /*fork*/, 0, 0, 0, 0, 0);
    if (cpid == 0) {
        /* 子进程：句柄是 fork 继承来的（引用 +1），页帧是同一批 -> 逐字节应当一致 */
        void* cvp = 0;
        const int crc2 = shm_map(id, 0, SHM_BYTES, &cvp);
        printf("[EVSHM] child shm_map id=%d ret=%d va=0x%lx\n", id, crc2, (unsigned long)cvp);
        if (crc2 == 0 && cvp) {
            unsigned char* cb = (unsigned char*)cvp;
            const unsigned cc = csum32(cb, SHM_BYTES);
            printf("[EVSHM] child csum=0x%08x match=%d\n", cc, cc == csum);
            ((unsigned*)cb)[16] = 0xCAFEBABEu;                 /* 往共享页里写魔数（父进程稍后读回） */
            printf("[EVSHM] child magic=0xCAFEBABE\n");
        } else {
            printf("[EVSHM] child shm_map FAILED ret=%d\n", crc2);
        }
        _exit(0);
    }

    /* 父进程：等子进程写完（本内核**没有跨进程同步原语** —— 这里只有 sleep 这一种顺序手段，
       如实标注：a42a64/fd64 那套也一样，没有 futex/信号量） */
    vimtu64_sleep_ms(600);
    {
        const unsigned magic = ((unsigned*)shm)[16];
        printf("[EVSHM] parent magic=0x%08x ok=%d\n", magic, magic == 0xCAFEBABEu ? 1 : 0);
    }
    {
        long status = 0;
        const long w4 = __v64_syscall(61 /*wait4*/, cpid, (long)(uintptr_t)&status, 0, 0, 0);
        printf("[EVSHM] wait4 child=%ld ret=%ld status=%ld\n", cpid, w4, status);
    }

    /* ---- (6) input_poll：申请键盘焦点 + 指针捕获，然后循环收事件 ---- */
    printf("[EVSHM] listen pend=%d\n", poll_event(0, 0, 0));            /* max=0：只查询待取条数 */
    (void)poll_event(0, 0, V64_EV_FLAG_FOCUS | V64_EV_FLAG_CAPTURE);   /* 只申请焦点/捕获 */
    printf("[EVSHM] listen pid=%d flags=0x%x\n", getpid(),
           (unsigned)(V64_EV_FLAG_FOCUS | V64_EV_FLAG_CAPTURE));

    {
        const unsigned long t0 = vimtu64_ticks();
        while ((vimtu64_ticks() - t0) < 250UL * (LISTEN_MS / 1000u)) {
            (void)pump(0);
            /* 关键证据到齐就走（正常 1~2 秒）：一次按键（down+up）+ 一次移动 + 左键按下与抬起。
               为什么要等 up：抬起的包比按下晚到（宿主/设备各一次事件），若这里见到 down 就走，
               抬起事件会落进下面 hold 的 6 秒窗口，被灌包时的"丢最旧"挤掉 —— 那就少了"按钮抬起"
               这条证据（实测踩过：MOUSE_UP 被 burst 挤掉）。 */
            if (g_key_down && g_key_up && g_move && g_mouse_down && g_mouse_up) break;
            vimtu64_sleep_ms(20);
        }
        printf("[EVSHM] listen-seen key=%u keyup=%u move=%u down=%u up=%u wheel=%u\n",
               g_key_down, g_key_up, g_move, g_mouse_down, g_mouse_up, g_wheel);
    }

    /* ---- (7) 阶段 2：故意 6 秒不取事件 —— 队列会被灌满，内核必须\"丢最旧 + 计数打点\" ---- */
    printf("[EVSHM] hold ms=%u (no poll: kernel queue must drop oldest and count)\n", HOLD_MS);
    vimtu64_sleep_ms(HOLD_MS);
    printf("[EVSHM] hold-done\n");
    for (int i = 0; i < 20; i++) {                     /* 把积压的事件取干净（bounded） */
        const int n = pump(0);
        if (n <= 0) break;
        vimtu64_sleep_ms(30);
    }

    /* ---- (8) 收尾：释放焦点/捕获 + 按类型计数 ---- */
    (void)poll_event(0, 0, V64_EV_FLAG_RELEASE);
    printf("[EVSHM] count key=%u keyup=%u move=%u down=%u up=%u wheel=%u\n",
           g_key_down, g_key_up, g_move, g_mouse_down, g_mouse_up, g_wheel);
    printf("[EVSHM] key=%u mouse=%u wheel=%u\n",                       /* 验收脚本的计数口径 */
           g_key_down, g_move + g_mouse_down + g_mouse_up, g_wheel);
    printf("[EVSHM] done\n");
    return 0;
}
