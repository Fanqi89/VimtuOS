// sysfs64.cpp - /sys 合成节点树（只读、无盘）。设计与树形见 sysfs64.h。
//
// ★ 强/弱符号：设备的真实信息在 drm64 里（evdev 的两路设备）。但**安装程序内核不链 drm64.o**
//   （它不需要 DRM，4 MB 预算也不允许），而 fs64 在两份内核里都有、都要引用本模块。
//   所以这里对 drm64 的五个查询函数用**弱引用 + 判空**：
//     * 系统内核（链了 drm64.o）-> 走真实数据；
//     * 安装内核（没链）-> 判空后树是空的（0 个 event），自检跳过设备相关项。
//   这和本工程既有做法一致（drm64_tick64 / vgpu3d_selftest64 / drm64_proc_release64 都是弱引用）。
//
// 自检位掩码（0 = 全过）：
//   bit 0  /sys 没被认成目录
//   bit 1  列 /sys 不是只有 class 一条
//   bit 2  /sys/nope 没有如实返回"不存在"
//   bit 3  读偏移越界没有读回 0 字节
//   ---- 以下只在"本内核有 evdev"时检查（安装内核会跳过并打 skipped）----
//   bit 4  /sys/class/input/event1/name 没被认成文件（或 size = 0）
//   bit 5  读 event1/name 的内容不是 "vimtu-mouse\n"
//   bit 6  列 /sys/class/input 不是 event0/event1 两条
//   bit 7  列 event1/capabilities 不是 ev/key/rel/abs 四条
//   bit 8  读 event1/capabilities/ev 不是 "7\n"（SYN|KEY|REL）
//   bit 9  读 event0/capabilities/ev 不是 "3\n"（SYN|KEY，键盘没有 REL）
#include "sysfs64.h"
#include "vfs64.h"      // VFS64_TYPE_DIR / VFS64_TYPE_FILE
#include "debug64.h"

// ---- 与 drm64.h 完全同形的**弱**声明（不能用 extern "C"：drm64.cpp 里是 C++ 链接）----
int         drm64_evdev_count64() __attribute__((weak));
const char* drm64_evdev_name64(int which) __attribute__((weak));
uint32_t    drm64_evdev_evbits64(int which) __attribute__((weak));
uint32_t    drm64_evdev_relbits64(int which) __attribute__((weak));
uint32_t    drm64_evdev_absbits64(int which) __attribute__((weak));

static int ev_cnt64() { return drm64_evdev_count64 ? drm64_evdev_count64() : 0; }
static const char* ev_name64(int w) {
    return drm64_evdev_name64 ? drm64_evdev_name64(w) : "unknown";
}
static uint32_t ev_bits64(int w) { return drm64_evdev_evbits64 ? drm64_evdev_evbits64(w) : 0u; }
static uint32_t ev_rel64(int w)  { return drm64_evdev_relbits64 ? drm64_evdev_relbits64(w) : 0u; }
static uint32_t ev_abs64(int w)  { return drm64_evdev_absbits64 ? drm64_evdev_absbits64(w) : 0u; }

static const char SYSFS64_EVDIR[] = "/sys/class/input/event";
#define SYSFS64_EVDIR_LEN (sizeof(SYSFS64_EVDIR) - 1)

// ==================== 最小字符串工具（无 libc）====================
static int s_eq(const char* a, const char* b) {
    for (;;) {
        if (*a != *b) return 0;
        if (!*a) return 1;
        a++; b++;
    }
}

static uint32_t s_put(char* out, uint32_t n, uint32_t cap, const char* s) {
    while (*s && n < cap) out[n++] = *s++;
    return n;
}

static uint32_t s_dec(char* out, uint32_t n, uint32_t cap, uint32_t v) {
    char rev[12];
    int rn = 0;
    if (v == 0) rev[rn++] = '0';
    while (v && rn < 11) { rev[rn++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    while (rn > 0 && n < cap) out[n++] = rev[--rn];
    return n;
}

// 十六进制（小写、无 0x 前缀）—— 与 Linux sysfs 的 capabilities/* 写法一致
static uint32_t s_hex(char* out, uint32_t n, uint32_t cap, uint32_t v) {
    static const char DIG[] = "0123456789abcdef";
    int shift = 28;
    int started = 0;
    for (; shift >= 0; shift -= 4) {
        const uint32_t d = (v >> (uint32_t)shift) & 0xFu;
        if (d || started || shift == 0) { started = 1; if (n < cap) out[n++] = DIG[d]; }
    }
    return n;
}

// ==================== 路径解析 ====================
// "/sys/class/input/event<0|1>" 前缀命中时返回 event 号，*rest 指向数字之后（"" 或 "/name"…）
static int s_ev64(const char* p, const char** rest) {
    uint32_t i = 0;
    for (; i < SYSFS64_EVDIR_LEN; i++) if (p[i] != SYSFS64_EVDIR[i]) return -1;
    if (p[i] != '0' && p[i] != '1') return -1;
    *rest = p + i + 1;
    return (int)(p[i] - '0');
}

// 1 = 目录；2 = 文件（内容写进 out，长度写进 *len）；0 = 节点不存在；-1 = 不归 sysfs 管
static int s_compose64(const char* p, char* out, uint32_t cap, uint32_t* len) {
    *len = 0;
    if (!p || p[0] != '/') return -1;
    if (p[1] != 's' || p[2] != 'y' || p[3] != 's') return -1;
    if (p[4] == 0) return 1;                                  // /sys
    if (p[4] != '/') return -1;
    if (s_eq(p + 5, "class")) return 1;                        // /sys/class
    if (s_eq(p + 5, "class/input")) return 1;                  // /sys/class/input
    const char* rest = nullptr;
    const int w = s_ev64(p, &rest);
    if (w < 0) return 0;
    if (w >= ev_cnt64()) return 0;                              // 只有真实存在的设备
    if (*rest == 0) return 1;                                   // /sys/class/input/eventN
    if (s_eq(rest, "/capabilities")) return 1;
    if (s_eq(rest, "/name")) {
        *len = s_put(out, 0, cap, ev_name64(w));
        *len = s_put(out, *len, cap, "\n");
        return 2;
    }
    if (s_eq(rest, "/subsystem")) { *len = s_put(out, 0, cap, "input\n"); return 2; }
    if (s_eq(rest, "/dev")) {
        *len = s_put(out, 0, cap, "13:");
        *len = s_dec(out, *len, cap, 64u + (uint32_t)w);        // Linux input 主设备号 13
        *len = s_put(out, *len, cap, "\n");
        return 2;
    }
    if (s_eq(rest, "/uevent")) {
        *len = s_put(out, 0, cap, "MAJOR=13\nMINOR=");
        *len = s_dec(out, *len, cap, 64u + (uint32_t)w);
        *len = s_put(out, *len, cap, "\nDEVNAME=input/event");
        *len = s_dec(out, *len, cap, (uint32_t)w);
        *len = s_put(out, *len, cap, "\n");
        return 2;
    }
    if (s_eq(rest, "/capabilities/ev")) {
        *len = s_hex(out, 0, cap, ev_bits64(w));
        *len = s_put(out, *len, cap, "\n");
        return 2;
    }
    if (s_eq(rest, "/capabilities/key")) {
        // **如实**：本内核没有维护键位图（EVIOCGBIT(EV_KEY) 也回 0），这里同样给 0
        *len = s_put(out, 0, cap, "0\n");
        return 2;
    }
    if (s_eq(rest, "/capabilities/rel")) {
        *len = s_hex(out, 0, cap, ev_rel64(w));
        *len = s_put(out, *len, cap, "\n");
        return 2;
    }
    if (s_eq(rest, "/capabilities/abs")) {
        *len = s_hex(out, 0, cap, ev_abs64(w));
        *len = s_put(out, *len, cap, "\n");
        return 2;
    }
    return 0;
}

// ==================== 对外 ====================
int sysfs64_match64(const char* path) {
    if (!path || !path[0]) return -1;
    if (path[0] != '/' || path[1] != 's' || path[2] != 'y' || path[3] != 's') return 0;
    return (path[4] == 0 || path[4] == '/') ? 1 : 0;
}

int sysfs64_stat64(const char* path, uint32_t* type, uint32_t* size) {
    if (sysfs64_match64(path) != 1) return 0;
    char b[SYSFS64_BUF];
    uint32_t len = 0;
    const int r = s_compose64(path, b, SYSFS64_BUF, &len);
    if (r <= 0) return -1;                                     // 不存在（不归 sysfs 的情况上面已排除）
    if (type) *type = (r == 1) ? VFS64_TYPE_DIR : VFS64_TYPE_FILE;
    if (size) *size = (r == 1) ? 0u : len;
    return 1;
}

int sysfs64_read64(const char* path, uint32_t off, void* buf, uint32_t len, uint32_t* out_got) {
    if (out_got) *out_got = 0;
    if (sysfs64_match64(path) != 1) return 0;
    char b[SYSFS64_BUF];
    uint32_t n = 0;
    const int r = s_compose64(path, b, SYSFS64_BUF, &n);
    if (r <= 0) return -1;
    if (r == 1) return -2;                                     // 目录不能 read（调用方按 EISDIR）
    if (!buf || !out_got || off >= n) return 1;                // 越界 = 读 0 字节
    uint32_t k = n - off;
    if (k > len) k = len;
    for (uint32_t i = 0; i < k; i++) ((uint8_t*)buf)[i] = (uint8_t)b[off + i];
    *out_got = k;
    return 1;
}

// 往 names 里塞一个名字（截断到 31 B + NUL）
static void s_name(char names[][32], int i, const char* s) {
    int k = 0;
    for (; s[k] && k < 31; k++) names[i][k] = s[k];
    names[i][k] = 0;
}

int sysfs64_list64(const char* path, char names[][32], int max, uint32_t* sizes) {
    if (sysfs64_match64(path) != 1) return -2;
    if (!names || max <= 0) return -1;
    int n = 0;
    if (s_eq(path, "/sys")) {
        if (n < max) { s_name(names, n, "class"); if (sizes) sizes[n] = 0u; n++; }
        return n;
    }
    if (s_eq(path, "/sys/class")) {
        if (n < max) { s_name(names, n, "input"); if (sizes) sizes[n] = 0u; n++; }
        return n;
    }
    if (s_eq(path, "/sys/class/input")) {
        const int cnt = ev_cnt64();
        for (int w = 0; w < cnt && n < max; w++) {
            char nm[16];
            uint32_t k = s_put(nm, 0, sizeof(nm) - 1, "event");
            k = s_dec(nm, k, sizeof(nm) - 1, (uint32_t)w);
            nm[k] = 0;
            s_name(names, n, nm);
            if (sizes) sizes[n] = 0u;
            n++;
        }
        return n;
    }
    const char* rest = nullptr;
    const int w = s_ev64(path, &rest);
    if (w < 0 || w >= ev_cnt64()) return -1;
    if (*rest == 0) {
        static const char* F[] = { "name", "dev", "subsystem", "uevent", "capabilities" };
        for (int i = 0; i < 5 && n < max; i++) {
            s_name(names, n, F[i]);
            if (sizes) sizes[n] = 0u;
            n++;
        }
        return n;
    }
    if (s_eq(rest, "/capabilities")) {
        static const char* C[] = { "ev", "key", "rel", "abs" };
        for (int i = 0; i < 4 && n < max; i++) {
            s_name(names, n, C[i]);
            if (sizes) sizes[n] = 0u;
            n++;
        }
        return n;
    }
    return -1;                                                 // 文件不是目录
}

// ==================== 自检 ====================
int sysfs64_selftest64() {
    int mask = 0;
    uint32_t ty = 0, sz = 0, got = 0;
    char b[SYSFS64_BUF];
    char names[8][32];

    // ---- 与设备无关的部分：任何内核（含安装程序内核）都必须对 ----
    if (sysfs64_stat64("/sys", &ty, &sz) != 1 || ty != VFS64_TYPE_DIR) mask |= 1 << 0;
    const int n0 = sysfs64_list64("/sys", names, 8, nullptr);
    if (n0 != 1 || !s_eq(names[0], "class")) mask |= 1 << 1;
    if (sysfs64_stat64("/sys/nope", &ty, &sz) != -1) mask |= 1 << 2;
    for (uint32_t i = 0; i < SYSFS64_BUF; i++) b[i] = 0;
    if (sysfs64_read64("/sys/class/input", 999u, b, 4u, &got) == 1 && got != 0u) mask |= 1 << 3;

    const int devs = ev_cnt64();
    const int n1 = sysfs64_list64("/sys/class/input", names, 8, nullptr);
    dbg64_str("[SYSFS64] tree entries=");
    dbg64_dec((uint64_t)(n1 < 0 ? 0 : n1));
    dbg64_str(" ev0=");
    dbg64_str(devs > 0 ? ev_name64(0) : "none");
    dbg64_str(" ev1=");
    dbg64_str(devs > 1 ? ev_name64(1) : "none");
    dbg64_str(" devs=");
    dbg64_dec((uint64_t)devs);
    dbg64_nl();

    if (devs <= 0) {
        // 安装程序内核：不链 drm64.cpp，没有 evdev —— 如实跳过设备相关的检查（不是"通过"）
        dbg64_str("[SYSFS64] selftest skipped (no evdev in this kernel) mask=");
        dbg64_dec((uint64_t)mask);
        dbg64_nl();
        return mask;
    }

    // ---- 设备相关 ----
    if (sysfs64_stat64("/sys/class/input/event1/name", &ty, &sz) != 1 ||
        ty != VFS64_TYPE_FILE || sz == 0u) mask |= 1 << 4;
    for (uint32_t i = 0; i < SYSFS64_BUF; i++) b[i] = 0;
    got = 0;
    if (sysfs64_read64("/sys/class/input/event1/name", 0, b, SYSFS64_BUF - 1, &got) != 1 ||
        !s_eq(b, "vimtu-mouse\n")) mask |= 1 << 5;
    if (n1 != 2 || !s_eq(names[0], "event0") || !s_eq(names[1], "event1")) mask |= 1 << 6;
    const int n2 = sysfs64_list64("/sys/class/input/event1/capabilities", names, 8, nullptr);
    if (n2 != 4) mask |= 1 << 7;
    for (uint32_t i = 0; i < SYSFS64_BUF; i++) b[i] = 0;
    got = 0;
    if (sysfs64_read64("/sys/class/input/event1/capabilities/ev", 0, b, SYSFS64_BUF - 1, &got) != 1 ||
        !s_eq(b, "7\n")) mask |= 1 << 8;
    for (uint32_t i = 0; i < SYSFS64_BUF; i++) b[i] = 0;
    got = 0;
    if (sysfs64_read64("/sys/class/input/event0/capabilities/ev", 0, b, SYSFS64_BUF - 1, &got) != 1 ||
        !s_eq(b, "3\n")) mask |= 1 << 9;
    got = 0;
    if (sysfs64_read64("/sys/class/input/event1/name", 999u, b, 8u, &got) != 1 || got != 0u) mask |= 1 << 3;

    if (mask == 0) {
        dbg64_str("[SYSFS64] selftest PASS");
        dbg64_nl();
    } else {
        dbg64_str("[SYSFS64] selftest FAIL mask=");
        dbg64_dec((uint64_t)mask);
        dbg64_nl();
    }
    return mask;
}
