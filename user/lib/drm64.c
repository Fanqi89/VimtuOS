/* drm64.c - ★ v0.9.12：DRM/KMS + evdev 的用户态薄封装（Vimtu64 用户态 C 运行时）
 *
 * 四条调用全部走**既有号位**，不新增任何 syscall：
 *   drm64_open()      -> Linux 号段 2（open）：fd = open("/dev/dri/card0", O_RDONLY)
 *                        内核里它命中 fd64 的**设备特判**（不查盘、不建文件），返回一个 DRM fd。
 *   drm64_ioctl()     -> Linux 号段 16（ioctl）：参数顺序与 Linux 完全一致（fd, cmd, arg）。
 *                        内核按 fd 类型分派到 kernel/drm64.cpp 的 drm64_ioctl64（**既有 ioctl
 *                        分支不动**，只在 tty 之后加了一条设备分派）。
 *   evdev64_open(w)   -> Linux 号段 2：/dev/input/event0（w=0 键盘）/ event1（w=1 鼠标）。
 *   evdev64_read()    -> Linux 号段 0（read）：内核把事件队列里的原始事件转成
 *                        struct InputEvent64（24 B/条，与 Linux 的 input_event 同尺寸）。
 *
 * 返回值口径（**不翻译 errno，调用方如实打印**）：
 *   0 / 正数 = 内核返回值；负数 = 内核的 -errno（-22 = -EINVAL、-25 = -ENOTTY、
 *   -12 = -ENOMEM、-11 = -EAGAIN（事件队列空）、-38 = -ENOSYS（这条通道不存在））。
 *   之所以不像 user/lib/fb.c 那样映射成 errno：验收程序要**原样**打印内核给的负值
 *   （tests/drm64_test.py 直接断言 badcmd rc=-25）。
 */
#include <errno.h>

#include "drm64.h"
#include "vimtu64.h"

/* Linux 号段（syscall 指令）里本文件用到的几个号 —— 与 user/lib/vimtu64.h 的口径一致 */
#define DRM64_LX_READ64   0
#define DRM64_LX_OPEN64   2
#define DRM64_LX_CLOSE64  3
#define DRM64_LX_IOCTL64  16

/* 打开标志（Linux 子集；DRM 设备只需要"能 open"） */
#define DRM64_O_RDONLY 0

static const char* const DRM64_DEV_PATH64  = "/dev/dri/card0";
static const char* const EVDEV64_PATH0_64  = "/dev/input/event0";
static const char* const EVDEV64_PATH1_64  = "/dev/input/event1";

int drm64_open(void) {
    const long r = __v64_syscall(DRM64_LX_OPEN64, (long)(uintptr_t)DRM64_DEV_PATH64,
                                 DRM64_O_RDONLY, 0, 0, 0);
    if (r < 0) { errno = ENOENT; return (int)r; }     /* 负数原样返回（内核的 -errno） */
    return (int)r;
}

long drm64_ioctl(int fd, unsigned long cmd, void* arg) {
    return __v64_syscall(DRM64_LX_IOCTL64, (long)fd, (long)cmd, (long)(uintptr_t)arg, 0, 0);
}

int evdev64_open(int which) {
    const char* p = (which == 1) ? EVDEV64_PATH1_64 : EVDEV64_PATH0_64;
    const long r = __v64_syscall(DRM64_LX_OPEN64, (long)(uintptr_t)p, DRM64_O_RDONLY, 0, 0, 0);
    if (r < 0) { errno = ENOENT; return (int)r; }
    return (int)r;
}

long evdev64_read(int fd, void* buf, long len) {
    return __v64_syscall(DRM64_LX_READ64, (long)fd, (long)(uintptr_t)buf, len, 0, 0);
}

/* ★ v0.9.20：从 /dev/dri/card0 读事件（struct Drm64EventVblank，32 B/条）。
 * 队列空时内核如实返回 -EAGAIN（-11）—— 调用方应该先 poll(POLLIN) 再读。 */
long drm64_read(int fd, void* buf, long len) {
    return __v64_syscall(DRM64_LX_READ64, (long)fd, (long)(uintptr_t)buf, len, 0, 0);
}
