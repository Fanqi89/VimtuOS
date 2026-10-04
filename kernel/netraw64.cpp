// netraw64.cpp - ★ P9：原始帧收发 ABI（自有 int 0x80 号 53 `net_raw`）的内核落点
//
// 语义 / 错误码 / 打点格式 / 宪法四条件对照的唯一说明在 kernel/netraw64.h（一页纸）与
// kernel/syscall64.h 的"net_raw（53）"段。本文件只有四件事：
//   ① 校验（op -> 长度 -> 缓冲区范围 -> 网卡在不在：顺序就是判错顺序）；
//   ② **转发**给 kernel/e1000_64.cpp **既有的** e1000_send64 / e1000_recv64（不碰任何寄存器、
//      不新增驱动接口、不做 DMA/描述符管理 —— 环的轮转仍由驱动自己维护）；
//   ③ 把驱动的返回值翻译成契约里的错误码（无帧 -> -EAGAIN；驱动没了 -> -ENODEV；超时 -> -EAGAIN）；
//   ④ 打 [NETRAW] 行（有上限防刷屏）。
// 内核**没有**协议解析：ARP 应答、IP 校验和、ICMP、DHCP、DNS、UDP/TCP 全在 ring3 的 /bin/netd。
//
// 恒等映射与上下文：前 4GB 恒等映射（kernel/mem64.cpp），而"用户缓冲的 VA"是**当前进程 CR3**
// 下的用户页 —— 系统调用是在该进程上下文里跑的，所以直接把用户 VA 交给驱动 memcpy 就行
// （与 dir_read / fb_map 写用户缓冲同一条路子）。本文件不切 CR3、不睡眠。
#include "netraw64.h"
#include "e1000_64.h"
#include "usermode64.h"     // user64_range_ok64（二次防御：调用点已校验，这里再确认一次）
#include "mem_64.h"         // memcpy_64
#include "debug64.h"
#include <stdint.h>

// ==================== 打点上限（防刷屏；单位 = 行）====================
static const int NETRAW64_LOG_MAX64  = 32;    // tx / rx 各 32 行
static const int NETRAW64_INFO_MAX64 = 8;     // mac / link 查询各 8 行
static const int NETRAW64_DENY_MAX64 = 32;    // 拒绝 32 行

static int g_nr64_tx_budget64   = NETRAW64_LOG_MAX64;
static int g_nr64_rx_budget64   = NETRAW64_LOG_MAX64;
static int g_nr64_info_budget64 = NETRAW64_INFO_MAX64;
static int g_nr64_deny_budget64 = NETRAW64_DENY_MAX64;
static int g_nr64_tx_capped64   = 0;          // "到了上限"只打一次
static int g_nr64_rx_capped64   = 0;

static uint64_t g_nr64_tx_frames64 = 0;
static uint64_t g_nr64_rx_frames64 = 0;
static uint64_t g_nr64_tx_bytes64  = 0;
static uint64_t g_nr64_rx_bytes64  = 0;
static uint64_t g_nr64_denied64    = 0;

static void nr64_put_mac64(const uint8_t* m) {
    static const char* H = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        if (i) dbg64_putc(':');
        dbg64_putc(H[(m[i] >> 4) & 0xF]);
        dbg64_putc(H[m[i] & 0xF]);
    }
}

static void nr64_tx_log64(uint64_t bytes, int ok, int64_t err) {
    if (g_nr64_tx_budget64 <= 0) {
        if (!g_nr64_tx_capped64) {                     // 只打一次"已到上限"
            g_nr64_tx_capped64 = 1;
            dbg64_line_begin64();
            dbg64_str("[NETRAW] log cap reached tx=");
            dbg64_dec((uint64_t)NETRAW64_LOG_MAX64);
            dbg64_str(" rx=");
            dbg64_dec((uint64_t)NETRAW64_LOG_MAX64);
            dbg64_nl();
            dbg64_line_end64();
        }
        return;
    }
    g_nr64_tx_budget64--;
    dbg64_line_begin64();
    dbg64_str("[NETRAW] tx bytes=");
    dbg64_dec(bytes);
    dbg64_str(" ok=");
    dbg64_dec((uint64_t)(ok ? 1u : 0u));
    dbg64_str(" err=");
    if (err < 0) { dbg64_putc('-'); dbg64_dec((uint64_t)(-err)); } else dbg64_dec((uint64_t)err);
    dbg64_str(" frames=");
    dbg64_dec(g_nr64_tx_frames64);
    dbg64_nl();
    dbg64_line_end64();
}

// 只在**真收到帧**时打：ring3 是轮询收包，无帧（-EAGAIN）是最常见的状态，逐次打会刷满串口。
static void nr64_rx_log64(uint64_t bytes) {
    if (g_nr64_rx_budget64 <= 0) {
        if (!g_nr64_rx_capped64) {
            g_nr64_rx_capped64 = 1;
            dbg64_line_begin64();
            dbg64_str("[NETRAW] log cap reached tx=");
            dbg64_dec((uint64_t)NETRAW64_LOG_MAX64);
            dbg64_str(" rx=");
            dbg64_dec((uint64_t)NETRAW64_LOG_MAX64);
            dbg64_nl();
            dbg64_line_end64();
        }
        return;
    }
    g_nr64_rx_budget64--;
    dbg64_line_begin64();
    dbg64_str("[NETRAW] rx bytes=");
    dbg64_dec(bytes);
    dbg64_str(" frames=");
    dbg64_dec(g_nr64_rx_frames64);
    dbg64_nl();
    dbg64_line_end64();
}

static void nr64_deny_log64(uint64_t op, const char* reason, int64_t err) {
    g_nr64_denied64++;
    if (g_nr64_deny_budget64 <= 0) return;
    g_nr64_deny_budget64--;
    dbg64_line_begin64();
    dbg64_str("[NETRAW] deny op=");
    dbg64_dec(op);
    dbg64_str(" reason=");
    dbg64_str(reason);
    dbg64_str(" err=");
    if (err < 0) { dbg64_putc('-'); dbg64_dec((uint64_t)(-err)); } else dbg64_dec((uint64_t)err);
    dbg64_nl();
    dbg64_line_end64();
}

static void nr64_info_log64(const uint8_t* mac, int link) {
    if (g_nr64_info_budget64 <= 0) return;
    g_nr64_info_budget64--;
    dbg64_line_begin64();
    dbg64_str("[NETRAW] mac=");
    nr64_put_mac64(mac);
    dbg64_str(" link=");
    dbg64_str(link ? "up" : "down");
    dbg64_nl();
    dbg64_line_end64();
}

// 网卡在不在（e1000_mac64 只在初始化成功后返回非空）
static int nr64_nic_ok64() { return e1000_mac64() != nullptr; }

// ==================== 四个落点 ====================
// tx：发一整帧（不含 FCS）。校验顺序：长度闸门 -> 用户缓冲范围 -> 网卡 -> 转发。
int64_t netraw64_tx64(uint64_t frame_va, uint64_t len) {
    if (len < (uint64_t)NETRAW64_TX_MIN) {                  // 连以太网头都不够：结构性非法
        nr64_deny_log64(NETRAW64_OP_TX, "short", NETRAW64_EINVAL);
        nr64_tx_log64(len, 0, NETRAW64_EINVAL);
        return NETRAW64_EINVAL;
    }
    if (len > (uint64_t)NETRAW64_MTU) {                     // 超过硬件口径（1514，不含 FCS）
        nr64_deny_log64(NETRAW64_OP_TX, "oversize", NETRAW64_EMSGSIZE);
        nr64_tx_log64(len, 0, NETRAW64_EMSGSIZE);
        return NETRAW64_EMSGSIZE;
    }
    if (!user64_range_ok64(frame_va, len)) {                // 越界/未映射/内核页：绝不交给 DMA
        nr64_deny_log64(NETRAW64_OP_TX, "bad-buf", NETRAW64_EFAULT);
        nr64_tx_log64(len, 0, NETRAW64_EFAULT);
        return NETRAW64_EFAULT;
    }
    if (!nr64_nic_ok64()) {
        nr64_deny_log64(NETRAW64_OP_TX, "no-nic", NETRAW64_ENODEV);
        nr64_tx_log64(len, 0, NETRAW64_ENODEV);
        return NETRAW64_ENODEV;
    }
    // e1000_send64 只在"参数不合法 / 驱动不可用 / 描述符有界等待超时"时返回 -1；
    // 前两种上面已经判过，剩下的是"TX 描述符忙" -> 契约里映射成 -EAGAIN（可重试）。
    const int rc = e1000_send64((const void*)(uintptr_t)frame_va, (int)len);
    if (rc != 0) {
        nr64_deny_log64(NETRAW64_OP_TX, "tx-timeout", NETRAW64_EAGAIN);
        nr64_tx_log64(len, 0, NETRAW64_EAGAIN);
        return NETRAW64_EAGAIN;
    }
    g_nr64_tx_frames64++;
    g_nr64_tx_bytes64 += len;
    nr64_tx_log64(len, 1, 0);
    return 0;
}

// rx：取一帧（**非阻塞**）。>0 = 已拷贝的字节数；-4 EAGAIN = 暂时没有帧。
int64_t netraw64_rx64(uint64_t buf_va, uint64_t cap) {
    if (cap < (uint64_t)NETRAW64_RX_MIN) {
        nr64_deny_log64(NETRAW64_OP_RX, "short", NETRAW64_EINVAL);
        return NETRAW64_EINVAL;
    }
    if (cap > (uint64_t)NETRAW64_RX_MAX) {                  // 超过 e1000 单个 RX 缓冲口径
        nr64_deny_log64(NETRAW64_OP_RX, "oversize", NETRAW64_EMSGSIZE);
        return NETRAW64_EMSGSIZE;
    }
    if (!user64_range_ok64(buf_va, cap)) {
        nr64_deny_log64(NETRAW64_OP_RX, "bad-buf", NETRAW64_EFAULT);
        return NETRAW64_EFAULT;
    }
    if (!nr64_nic_ok64()) {
        nr64_deny_log64(NETRAW64_OP_RX, "no-nic", NETRAW64_ENODEV);
        return NETRAW64_ENODEV;
    }
    const int got = e1000_recv64((void*)(uintptr_t)buf_va, (int)cap);
    if (got < 0) {                                          // 驱动不可用
        nr64_deny_log64(NETRAW64_OP_RX, "no-nic", NETRAW64_ENODEV);
        return NETRAW64_ENODEV;
    }
    if (got == 0) return NETRAW64_EAGAIN;                   // 没有新帧：不是错误，不打点（见文件头）
    g_nr64_rx_frames64++;
    g_nr64_rx_bytes64 += (uint64_t)got;
    nr64_rx_log64((uint64_t)got);
    return (int64_t)got;
}

// mac：把 6 字节 MAC 写进用户缓冲（谁是"自己"这个问题由 ring3 决定用途，内核只报硬件事实）。
int64_t netraw64_mac64(uint64_t mac_va) {
    const uint8_t* m = e1000_mac64();
    if (!m) {
        nr64_deny_log64(NETRAW64_OP_MAC, "no-nic", NETRAW64_ENODEV);
        return NETRAW64_ENODEV;
    }
    if (!user64_range_ok64(mac_va, 6)) {
        nr64_deny_log64(NETRAW64_OP_MAC, "bad-buf", NETRAW64_EFAULT);
        return NETRAW64_EFAULT;
    }
    memcpy_64((void*)(uintptr_t)mac_va, m, 6);
    nr64_info_log64(m, e1000_link64());
    return 0;
}

// link：1 = 链路 up / 0 = down / -5 ENODEV = 没有网卡。
int64_t netraw64_link64() {
    const uint8_t* m = e1000_mac64();
    if (!m) {
        nr64_deny_log64(NETRAW64_OP_LINK, "no-nic", NETRAW64_ENODEV);
        return NETRAW64_ENODEV;
    }
    const int up = e1000_link64();
    nr64_info_log64(m, up);
    return (int64_t)(up ? 1 : 0);
}

// ==================== 统计（诊断用，不是 ABI）====================
uint64_t netraw64_tx_frames64() { return g_nr64_tx_frames64; }
uint64_t netraw64_rx_frames64() { return g_nr64_rx_frames64; }
uint64_t netraw64_tx_bytes64()  { return g_nr64_tx_bytes64; }
uint64_t netraw64_rx_bytes64()  { return g_nr64_rx_bytes64; }
uint64_t netraw64_denied64()    { return g_nr64_denied64; }
