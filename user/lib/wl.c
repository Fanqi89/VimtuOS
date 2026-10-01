/* wl.c - ★ A5：Wayland 基础骨架的用户态包装（int 0x80 自有 ABI 号 15..21）
 *
 * 只做三件事：按 ABI 摆寄存器、把 64 位返回值截成 int、把 damage 的 5 个分量打包成 2 个寄存器。
 * **不发明任何语义**（语义/错误码的唯一定义点是 kernel/wl64.h；这里与 user/lib/wl.h 的头注释一一对应）。 */
#include "wl.h"
#include "vimtu64.h"

/* 打包：(x, y) -> x | y<<32；(w, h) -> w | h<<32（内核按有符号 32 位解释两个半区，与 kernel/wl64.h 一致） */
static long pack2(int lo, int hi) {
    return (long)((unsigned)lo | ((unsigned long)(unsigned)hi << 32));
}

int wl_surface_create(int w, int h, int format) {
    return (int)__v64_int80(V64_NR_WL_SURFACE_CREATE, (long)w, (long)h, (long)format, 0);
}

int wl_surface_attach(int surf, int shm_id, int offset) {
    return (int)__v64_int80(V64_NR_WL_SURFACE_ATTACH, (long)surf, (long)shm_id, (long)offset, 0);
}

int wl_surface_damage(int surf, int x, int y, int w, int h) {
    return (int)__v64_int80(V64_NR_WL_SURFACE_DAMAGE, (long)surf, pack2(x, y), pack2(w, h), 0);
}

int wl_surface_commit(int surf) {
    return (int)__v64_int80(V64_NR_WL_SURFACE_COMMIT, (long)surf, 0, 0, 0);
}

int wl_surface_destroy(int surf) {
    return (int)__v64_int80(V64_NR_WL_SURFACE_DESTROY, (long)surf, 0, 0, 0);
}

int wl_seat_get(void) {
    return (int)__v64_int80(V64_NR_WL_SEAT_GET, 0, 0, 0, 0);
}

int wl_display_dispatch(unsigned timeout_ms, struct Wl64SeatEvent* out, unsigned max) {
    return (int)__v64_int80(V64_NR_WL_DISPATCH, (long)timeout_ms, (long)(unsigned long)out, (long)max, 0);
}
