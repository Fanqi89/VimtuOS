// ext4vol64.cpp - ★ v0.9.40：内核 -> ring3 服务的**反向请求通道**（ext4 只读挂载的底座）
//
// 这个文件只做三件事，做完就停（不做解析、不做缓存、不做挂载策略）：
//   1) **握手**：按协议找到活着的 ext4 服务，拿到它的共享对象帧号，确认 req.magic 已发布；
//   2) **有界 RPC**：把一条 R3drvReq 写进对象 [512,4096)，最后写 state=REQ，
//      然后**每圈让出 CPU**地等（task_yield64），到 150 ms 就返回 -ETIMEDOUT；
//   3) **自检**：把"列 /、取 /big.bin 属性、读 /big.bin 并算 CRC32"跑一遍并打点。
//
// 为什么内核不能忙等（写进注释，防止以后有人"优化"掉 yield）：
//   ring3 服务要跑，必须靠调度器把 CPU 交给它；如果内核在同一线程里死循环看 state，
//   服务永远拿不到时间片 -> state 永远是 REQ -> 双方互等到死。task_yield64() 会让出到
//   下一个 tick（sti; hlt），服务因此得以执行并把 state 改成 DONE。
//
// 为什么应答数据放数据区、而不是塞进 R3drvReq：
//   服务那边已经有"零拷贝把盘上扇区搬进数据区"的能力；把目录项/文件字节直接排在这块
//   已映射的页里，内核不需要第二套解析器，也不需要额外拷贝（这是混合内核该有的分工）。
#include "ext4vol64.h"

#include "r3drv64.h"
#include "debug64.h"
#include "memlayout64.h"   // PAGE_SIZE_64（必须先于 mem_64.h）
#include "mem_64.h"
#include "x86_64.h"        // ticks64 / ms_to_ticks64
#include "proc64.h"        // proc64_shm_view64
#include "vfs64.h"         // 读 /etc/r3drv.conf
#include "task64.h"        // task_yield64
#include "fs64.h"          // ★ 自检要走"文件管理器那条分派"（fs64_list64 / fs64_read64）

#define EXT4VOL_CONF_PATH "/etc/r3drv.conf"

static uint64_t g_ev_frames[SHM64_MAX_PAGES64];
static uint32_t g_ev_slot = 0xFFFFFFFFu;
static int32_t  g_ev_shm  = -1;
static uint64_t g_ev_seq  = 0;
static uint64_t g_ev_rpc  = 0;
static uint64_t g_ev_to   = 0;
static int      g_ev_logs = 0;

uint64_t ext4vol64_rpc_count64()    { return g_ev_rpc; }
uint64_t ext4vol64_rpc_timeouts64() { return g_ev_to; }

// ---------------- CRC32（IEEE，与 zlib.crc32 / ext4svc 的 crc32 同值）----------------
static uint32_t s_crc_tab[256];
static int s_crc_ready = 0;
static void crc_init() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_crc_tab[i] = c;
    }
    s_crc_ready = 1;
}
static uint32_t crc32_of(uint32_t crc, const uint8_t* p, uint32_t n) {
    if (!s_crc_ready) crc_init();
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) c = s_crc_tab[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ---------------- 绑定：找服务 + 取帧 + 确认已发布 ----------------
// 返回 0 = 可发 RPC；-19 = 没有服务；-6 = 服务还没发布请求块；-12 = 取不到帧
static int ev_bind() {
    uint32_t slot = 0, disk = 0;
    int32_t  shm = -1;
    uint64_t lba = 0;
    if (!r3drv64_lookup64(R3DRV_PROTO_EXT4, &slot, &shm, &disk, &lba)) {
        g_ev_slot = 0xFFFFFFFFu; g_ev_shm = -1;
        return -19;
    }
    uint32_t pages = 0;
    // ★ 注意返回值语义：proc64_shm_view64 **成功返回 0**（不是 1）。
    //   第一版这里写成了 `if (!...(...)) return -12;` —— 成功被当成失败，ready 永远是 0。
    if (proc64_shm_view64((uint32_t)shm, g_ev_frames, SHM64_MAX_PAGES64, &pages, nullptr) != 0)
        return -12;
    if (pages < 2) return -12;                     // 至少要 1 页环 + 1 页数据区
    g_ev_slot = slot;
    g_ev_shm  = shm;
    return 0;
}

static volatile R3drvReq* req_ptr() {
    // 与 r3drv64.cpp 同一条约定：共享对象的帧在内核里可以直接按 PA 访问（PA==VA 恒等映射）
    return (volatile R3drvReq*)(uintptr_t)(g_ev_frames[0] + (uint64_t)R3DRV_REQ_OFF);
}
static uint8_t* data_ptr() {
    return (uint8_t*)(uintptr_t)g_ev_frames[R3DRV_DATA_OFF / 4096u];
}

int ext4vol64_ready64() {
    if (g_ev_shm <= 0 || g_ev_slot == 0xFFFFFFFFu) {
        if (ev_bind() != 0) return 0;
    }
    volatile R3drvReq* rq = req_ptr();
    return (rq->magic == R3DRV_REQ_MAGIC) ? 1 : 0;
}

// 有界等队列空闲：上一条请求若还挂在 REQ（例如上次内核超时了，服务稍后才消费），
// 这里等它被服务取走，最多 ms 毫秒；超时返回 -11（EAGAIN）而不是把后面的请求全卡死。
static int ev_wait_idle(uint32_t ms) {
    volatile R3drvReq* rq = req_ptr();
    if (rq->magic != R3DRV_REQ_MAGIC) return -6;
    if (rq->state != R3DRV_REQ_ST_REQ) return 0;
    const uint32_t dl = ticks64() + ms_to_ticks64(ms ? ms : 1u);
    while ((int32_t)(ticks64() - dl) < 0) {
        if (rq->state != R3DRV_REQ_ST_REQ) return 0;
        task_yield64();
    }
    return -11;
}

// ---------------- 单发 RPC（核心）----------------
// timeout_ms = 0 表示"只投递不等待"（自检里用它证明"不会卡死"）
static int ev_rpc(uint32_t op, const char* path, uint64_t off, uint64_t size,
                  uint32_t timeout_ms, uint32_t* out_result, uint32_t* out_aux, uint32_t* out_err) {
    if (out_result) *out_result = 0;
    if (out_aux)    *out_aux = 0;
    if (out_err)    *out_err = 0;

    int rc = ev_bind();
    if (rc != 0) return rc;
    volatile R3drvReq* rq = req_ptr();
    if (rq->magic != R3DRV_REQ_MAGIC) return -6;              // 服务还没准备好
    {                                                          // 上一条还没被取走：有界等它
        const int idle = ev_wait_idle(500u);
        if (idle != 0) return idle;
    }

    // 填请求（**最后**写 state，服务只认 state==REQ）
    rq->op        = op;
    rq->err       = 0;
    rq->off       = off;
    rq->size      = size;
    rq->result    = 0;
    rq->aux       = 0;
    rq->data_off  = R3DRV_DATA_OFF;
    rq->data_secs = EXT4VOL_DATA_SECS;
    {
        uint32_t i = 0;
        if (path) {
            while (path[i] && i + 1u < (uint32_t)sizeof(rq->path)) { rq->path[i] = path[i]; i++; }
        }
        rq->path[i] = 0;
    }
    const uint64_t seq = ++g_ev_seq;
    rq->seq   = seq;
    __sync_synchronize();
    rq->state = R3DRV_REQ_ST_REQ;
    g_ev_rpc++;

    const uint32_t t0 = ticks64();
    const uint32_t deadline = t0 + ms_to_ticks64(timeout_ms ? timeout_ms : 1u);
    uint32_t spins = 0;
    for (;;) {
        const uint32_t st = rq->state;
        if (st == R3DRV_REQ_ST_DONE || st == R3DRV_REQ_ST_FAIL) {
            if (rq->seq != seq) return -5;                    // 串号：不是我要的那条应答
            const uint32_t ms = (ticks64() - t0) * 4u;
            if (out_result) *out_result = rq->result;
            if (out_aux)    *out_aux    = rq->aux;
            if (out_err)    *out_err    = rq->err;
            if (g_ev_logs < 8 || (g_ev_rpc % 32u) == 0) {
                g_ev_logs++;
                dbg64_line_begin64();
                dbg64_str("[EXT4VOL] rpc op=");
                dbg64_str(op == R3DRV_OP_STAT ? "stat" : op == R3DRV_OP_LIST ? "list" : "read");
                dbg64_str(" path=");
                dbg64_str(path ? path : "?");
                dbg64_str(" result=");
                dbg64_dec((uint64_t)rq->result);
                dbg64_str(" ms=");
                dbg64_dec((uint64_t)ms);
                dbg64_str(st == R3DRV_REQ_ST_DONE ? " ok" : " FAILED err=");
                if (st != R3DRV_REQ_ST_DONE) dbg64_dec((uint64_t)rq->err);
                dbg64_nl();
                dbg64_line_end64();
            }
            return (st == R3DRV_REQ_ST_DONE) ? 0 : -5;
        }
        if (timeout_ms == 0) return -110;                     // 只投递不等待
        if ((int32_t)(ticks64() - deadline) >= 0) {
            g_ev_to++;
            dbg64_line_begin64();
            dbg64_str("[EXT4VOL] rpc TIMEOUT path=");
            dbg64_str(path ? path : "?");
            dbg64_str(" state=");
            dbg64_dec((uint64_t)rq->state);
            dbg64_str(" spins=");
            dbg64_dec((uint64_t)spins);
            dbg64_nl();
            dbg64_line_end64();
            return -110;
        }
        task_yield64();                                       // ★ 必须让出：否则服务永远跑不到
        if (++spins > 400000u) return -110;                   // 兜底（yield 计数也封顶）
    }
}

// ---------------- 对外三件套 ----------------
int ext4vol64_stat64(const char* path, uint32_t* out_type, uint32_t* out_size) {
    uint32_t res = 0, aux = 0, err = 0;
    const int rc = ev_rpc(R3DRV_OP_STAT, path, 0, 0, EXT4VOL_RPC_MS, &res, &aux, &err);
    if (rc != 0) return rc;
    if (out_type) *out_type = aux;                 // aux = 1 文件 / 2 目录
    if (out_size) *out_size = res;                 // result = 字节数
    return 0;
}

int ext4vol64_list64(const char* path, R3drvDirent64* out, int max,
                     int* out_count, int* out_more) {
    if (out_count) *out_count = 0;
    if (out_more)  *out_more = 0;
    if (!out || max <= 0) return -22;
    if (max > EXT4VOL_MAX_ENTRIES) max = EXT4VOL_MAX_ENTRIES;

    uint32_t res = 0, aux = 0, err = 0;
    const int rc = ev_rpc(R3DRV_OP_LIST, path, 0, 0, EXT4VOL_RPC_MS, &res, &aux, &err);
    if (rc != 0) return rc;
    const uint32_t cap = (EXT4VOL_DATA_SECS * 512u) / (uint32_t)sizeof(R3drvDirent64);
    uint32_t n = res;
    if (n > cap) n = cap;                          // 不信任服务自报：按 data_secs 再封一次顶
    if ((int)n > max) { n = (uint32_t)max; if (out_more) *out_more = 1; }
    const uint8_t* src = data_ptr();
    for (uint32_t i = 0; i < n; i++) {
        const R3drvDirent64* e = (const R3drvDirent64*)(src + i * sizeof(R3drvDirent64));
        out[i] = *e;
        out[i].name[sizeof(out[i].name) - 1] = 0;  // 强制 NUL 结尾（服务写的也要防一手）
    }
    if (out_count) *out_count = (int)n;
    if (out_more && aux != 0) *out_more = 1;
    return 0;
}

int ext4vol64_read64(const char* path, uint64_t off, void* buf, uint32_t len, uint32_t* out_got) {
    if (out_got) *out_got = 0;
    if (!buf) return -22;
    uint8_t* dst = (uint8_t*)buf;
    uint32_t done = 0;
    const uint32_t chunk = EXT4VOL_DATA_SECS * 512u;      // 一次最多一页
    while (done < len) {
        uint32_t n = len - done;
        if (n > chunk) n = chunk;
        uint32_t res = 0, aux = 0, err = 0;
        const int rc = ev_rpc(R3DRV_OP_READ, path, off + done, n, EXT4VOL_RPC_MS, &res, &aux, &err);
        if (rc != 0) return rc;
        if (res == 0) break;                              // EOF
        if (res > n) res = n;                             // 不信任自报
        const uint8_t* src = data_ptr();
        for (uint32_t i = 0; i < res; i++) dst[done + i] = src[i];
        done += res;
        if (res < n) break;                               // 到文件尾
    }
    if (out_got) *out_got = done;
    return 0;
}

// ---------------- 开机自检（配置 rpc=1 才跑）----------------
static int ev_conf_rpc() {
    static char buf[512];
    const int n = vfs64_read64(EXT4VOL_CONF_PATH, buf, (int)sizeof(buf) - 1);
    if (n <= 0) return 0;
    buf[n] = 0;
    for (int i = 0; i + 4 < n; i++) {
        if (buf[i] == 'r' && buf[i + 1] == 'p' && buf[i + 2] == 'c' && buf[i + 3] == '=') {
            return (buf[i + 4] == '1') ? 1 : 0;
        }
    }
    return 0;
}

// ★ 诊断（只在自检 not-ready 时打一次）：把"到底哪一步断了"摊开
static void ev_diag64(void) {
    uint32_t slot = 0, disk = 0, pages = 0;
    int32_t shm = -1;
    uint64_t lba = 0;
    const int f = r3drv64_lookup64(R3DRV_PROTO_EXT4, &slot, &shm, &disk, &lba);
    int v = -1;
    if (f && shm > 0)
        v = proc64_shm_view64((uint32_t)shm, g_ev_frames, SHM64_MAX_PAGES64, &pages, nullptr);
    dbg64_line_begin64();
    dbg64_str("[EXT4VOL] diag lookup=");  dbg64_dec((uint64_t)f);
    dbg64_str(" slot=");                  dbg64_dec((uint64_t)slot);
    dbg64_str(" shm=");                   dbg64_dec((uint64_t)(shm < 0 ? 0 : shm));
    dbg64_str(" view=");                  dbg64_dec((uint64_t)v);
    dbg64_str(" pages=");                 dbg64_dec((uint64_t)pages);
    if (v == 0 && pages > 0) {
        const uint64_t pa = g_ev_frames[0];
        dbg64_str(" frame0=0x");          dbg64_hex64(pa);
        dbg64_str(" ringmagic=0x");       dbg64_hex64((uint64_t)*(volatile uint32_t*)(uintptr_t)pa);
        dbg64_str(" reqmagic=0x");
        dbg64_hex64((uint64_t)*(volatile uint32_t*)(uintptr_t)(pa + (uint64_t)R3DRV_REQ_OFF));
        dbg64_str(" reqstate=");
        dbg64_dec((uint64_t)*(volatile uint32_t*)(uintptr_t)(pa + (uint64_t)R3DRV_REQ_OFF + 4u));
    }
    dbg64_nl();
    dbg64_line_end64();
}

// 自检用：对 EAGAIN(-11) 做有限重试（服务刚进循环时可能还差一两拍）
static int ev_list_retry(const char* path, R3drvDirent64* out, int max, int* n, int* more) {
    int rc = -11;
    for (int i = 0; i < 6; i++) {
        rc = ext4vol64_list64(path, out, max, n, more);
        if (rc != -11) return rc;
        task_sleep64(10);
    }
    return rc;
}
static int ev_stat_retry(const char* path, uint32_t* type, uint32_t* size) {
    int rc = -11;
    for (int i = 0; i < 6; i++) {
        rc = ext4vol64_stat64(path, type, size);
        if (rc != -11) return rc;
        task_sleep64(10);
    }
    return rc;
}

void ext4vol64_selftest64() {
    // 1) 等服务发布请求块（有界：最多 3000 ms；期间每圈都让出 CPU）
    const uint32_t dl = ticks64() + ms_to_ticks64(15000u);   // 15 s：服务首次解析可能较久（仍然有界）
    int ready = 0;
    while ((int32_t)(ticks64() - dl) < 0) {
        if (ext4vol64_ready64()) { ready = 1; break; }
        task_yield64();
    }
    {
        dbg64_line_begin64();
        dbg64_str("[EXT4VOL] bind slot="); dbg64_dec(g_ev_slot == 0xFFFFFFFFu ? 0 : (uint64_t)g_ev_slot);
        dbg64_str(" shm=");                dbg64_dec((uint64_t)(g_ev_shm < 0 ? 0 : g_ev_shm));
        dbg64_str(ready ? " ok" : " FAILED reason=not-ready");
        dbg64_nl();
        dbg64_line_end64();
    }
    if (!ready) { ev_diag64(); return; }

    // 2) 列根目录
    {
        static R3drvDirent64 ents[EXT4VOL_MAX_ENTRIES];
        int n = 0, more = 0;
        const int rc = ev_list_retry("/", ents, EXT4VOL_MAX_ENTRIES, &n, &more);
        dbg64_line_begin64();
        dbg64_str("[EXT4VOL] selftest list=\"/\" count="); dbg64_dec((uint64_t)(rc == 0 ? n : 0));
        dbg64_str(" names=");
        for (int i = 0; i < n && i < 6; i++) {
            if (i) dbg64_str(",");
            dbg64_str(ents[i].name);
        }
        dbg64_str(rc == 0 ? " ok" : " FAILED");
        if (rc != 0) { dbg64_str(" err="); dbg64_dec((uint64_t)(-rc)); }
        dbg64_nl();
        dbg64_line_end64();
    }

    // 3) 取 /big.bin 属性
    {
        uint32_t type = 0, size = 0;
        const int rc = ev_stat_retry("/big.bin", &type, &size);
        dbg64_line_begin64();
        dbg64_str("[EXT4VOL] selftest stat=\"/big.bin\" size="); dbg64_dec((uint64_t)size);
        dbg64_str(" type="); dbg64_dec((uint64_t)type);
        dbg64_str(rc == 0 ? " ok" : " FAILED");
        if (rc != 0) { dbg64_str(" err="); dbg64_dec((uint64_t)(-rc)); }
        dbg64_nl();
        dbg64_line_end64();
    }

    // 4) 读 /big.bin 全文并算 CRC32（宿主持有同一份夹具，会独立算一遍比对）
    {
        static uint8_t tmp[4096];
        uint32_t off = 0, crc = 0, total = 0;
        int ok = 1;
        for (;;) {
            uint32_t got = 0;
            const int rc = ext4vol64_read64("/big.bin", off, tmp, (uint32_t)sizeof(tmp), &got);
            if (rc != 0) { ok = 0; break; }
            if (got == 0) break;
            crc = crc32_of(crc, tmp, got);
            off += got;
            total += got;
            if (off > (4u * 1024u * 1024u)) { ok = 0; break; }   // 防跑飞
            if (got < (uint32_t)sizeof(tmp)) break;
        }
        dbg64_line_begin64();
        dbg64_str("[EXT4VOL] selftest read=\"/big.bin\" bytes="); dbg64_dec((uint64_t)total);
        dbg64_str(" crc=0x"); dbg64_hex64((uint64_t)crc);
        dbg64_str(ok && total == 100000u ? " ok" : " FAILED");
        dbg64_nl();
        dbg64_line_end64();
    }


    // 6) ★ 走 **fs64 的分派**（文件管理器用的就是这条路径，不是 ext4vol64 的私有接口）
    {
        static Fs64Dirent64 ents[8];
        uint32_t cursor = 0;
        const int n = fs64_list64(FS64_VOL_EXT4_BASE, "/", ents, 8, &cursor);
        dbg64_line_begin64();
        dbg64_str("[EXT4VOL] selftest fs64-list count="); dbg64_dec((uint64_t)(n < 0 ? 0u : (uint64_t)n));
        dbg64_str(" names=");
        for (int i = 0; i < n && i < 6; i++) {
            if (i) dbg64_str(",");
            dbg64_str(ents[i].name);
        }
        dbg64_str(n >= 3 ? " ok" : " FAILED");
        dbg64_nl();
        dbg64_line_end64();
    }

    // 7) ★ 同一条分派上的**读文件**（hello.txt 是 13 B 文本）
    {
        static uint8_t tbuf[128];
        const int got = fs64_read64(FS64_VOL_EXT4_BASE, "/hello.txt", tbuf, (int)sizeof(tbuf) - 1);
        if (got > 0) tbuf[got] = 0;
        dbg64_line_begin64();
        dbg64_str("[EXT4VOL] selftest fs64-read path=/hello.txt bytes=");
        dbg64_dec((uint64_t)(got < 0 ? 0 : got));
        dbg64_str(" text=");
        if (got > 0) {
            for (int i = 0; i < got && i < 32; i++) {
                const char c = (char)tbuf[i];
                if (c == '\n') { dbg64_str("\\n"); continue; }
                if (c < 32 || c > 126) { dbg64_str("."); continue; }
                char one[2]; one[0] = c; one[1] = 0; dbg64_str(one);
            }
        }
        dbg64_str(got > 0 ? " ok" : " FAILED");
        dbg64_nl();
        dbg64_line_end64();
    }

    // 5) 超时探针：timeout=0 时**必须**立刻返回 -ETIMEDOUT（证明"不会把 GUI 卡死"）
    {
        uint32_t res = 0, aux = 0, err = 0;
        (void)ev_wait_idle(500u);          // 先让队列空下来，探针才有意义
        const int rc = ev_rpc(R3DRV_OP_STAT, "/big.bin", 0, 0, 0, &res, &aux, &err);
        dbg64_line_begin64();
        dbg64_str("[EXT4VOL] selftest timeout-probe=");
        dbg64_str(rc == -110 ? "ok" : "FAILED");
        dbg64_nl();
        dbg64_line_end64();
    }
}

void ext4vol64_boot64() {
    if (!ev_conf_rpc()) return;              // 默认关：绝大多数夹具行为一个字节不变
    dbg64_line_begin64();
    dbg64_str("[EXT4VOL] boot rpc=1 selftest begin\n");
    dbg64_line_end64();
    ext4vol64_selftest64();
}
