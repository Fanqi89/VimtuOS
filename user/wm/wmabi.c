/* wmabi.c - ★ B-wm：Ring 3 合成器 ABI（自有 ABI 22..26）的用户态包装
 *
 * 只做两件事：按 ABI 摆寄存器（rdi/rsi/rdx/r10 = 参数 1..4；rax = 返回）、把 64 位返回值截成 int。
 * **不发明任何语义** —— 语义/错误码的唯一定义点在 kernel/wl64.h 的 "B-wm" 段。 */
#include "wmabi.h"

int wl_composer_get(struct Wm64Rect* out_rect) {
    return (int)__v64_int80(V64_NR_WL_COMPOSER_GET, (long)(unsigned long)out_rect, 0, 0, 0);
}

int wl_surface_export(int idx, struct Wm64SurfaceInfo* out) {
    return (int)__v64_int80(V64_NR_WL_SURFACE_EXPORT, (long)idx, (long)(unsigned long)out, 0, 0);
}

int wl_surface_map(int surf, struct Wm64SurfaceMap* out) {
    return (int)__v64_int80(V64_NR_WL_SURFACE_MAP, (long)surf, (long)(unsigned long)out, 0, 0);
}

int wl_surface_ack(int surf) {
    return (int)__v64_int80(V64_NR_WL_SURFACE_ACK, (long)surf, 0, 0, 0);
}

int wl_seat_post(struct Wl64SeatEvent* ev) {
    return (int)__v64_int80(V64_NR_WL_SEAT_POST, (long)(unsigned long)ev, 0, 0, 0);
}
