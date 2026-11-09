// r3drv64.cpp - ★ v0.9.33：ring3 驱动框架内核侧实现（注册表 + 零拷贝共享环 + 监督重启 + 路由）
//
// 设计取舍（先说清"为什么不那么做"，这是本文件最值钱的部分）：
//   1) 为什么不把 ext4 写进内核：内核现在 3,166,544 / 15,728,640 B，还有 12.5 MB 余量 ——
//      **不是放不下**，是不该放。文件系统解析器是**最容易被畸形磁盘镜像打崩**的那类代码：
//      它要按盘上的字节做链式解引用、算偏移、跟整数溢出搏斗。把它留在 ring0 意味着
//      一张坏 U 盘能拖垮整机；放到 ring3，最坏结果是"那个服务进程死了"，
//      而本文件的监督器会把它拉起来（或如实放弃并让卷退回只读）。
//   2) 为什么 DMA 目标必须是**服务的共享页**而不是内核缓冲：内核缓冲那条路要多一次
//      `for (i..) dst[i]=src[i]`，64 MiB 就是 64 MiB 的 CPU 拷贝。而 AHCI 的 PRDT
//      允许"一帧一项"，所以只要内核知道共享对象的**物理帧号**（proc64_shm_view64 给），
//      就能让 DMA 引擎自己把数据放进服务已经映射好的页里 —— 那一次拷贝**根本不需要发生**。
//   3) 为什么环的完成是"就地"写 SQE.state：省一个 CQ 环、省一次内存序协商；
//      代价是同一槽在下一次提交前必须已被消费（服务自己按 `head - tail < SLOTS` 节流，
//      见 user/drivers/r3drv.h 的 zc_submit64）。
//   4) 为什么监督器在**内核**里而不是一个 ring3 看门狗进程：一个用户态进程无法可靠地
//      重启另一个已经崩了的进程（它连对方死没死都要靠内核告诉它）。所以"拉起"这件事
//      只能在 ring0 做；ring3 的 /usr/bin/drvwatch 是**观察者**（读 /proc/vimtu/drivers
//      证明状态机真的在动），不是执行者。这条边界在 docs/用户态驱动框架.md 里也写着。
//
// 串口打点（自动验收 grep 这些行，改格式要同步改 tests/r3drv64_test.py）：
//   [R3DRV] init conf=/etc/r3drv.conf autostart=<0|1> beat_ticks=<n> kbuf_pages=<n>
//   [R3DRV] conf <key>=<val> ...                            （只打读到的关键项）
//   [R3DRV] probe disk=<n> lba=<n> proto=<ext4|vimtufs2|fat32|none>
//   [R3DRV] route disk=<n> lba=<n> proto=<..> mode=driver slot=<n>
//   [R3DRV] route disk=<n> lba=<n> proto=<..> mode=read-only reason=<no-driver|failed>
//   [R3DRV] route disk=<n> lba=<n> proto=none mode=unknown-ro
//   [R3DRV] autostart skipped reason=<no-target-volume|service-missing|off|no-isolation>
//   [R3DRV] start name=<s> path=<p> proto=<..> disk=<n> lba=<n> pid=<n> slot=<n> ok|<FAILED reason=..>
//   [R3DRV] register name=<s> proto=<..> disk=<n> shm=<id> bytes=<n> pid=<n> slot=<n> mode=<new|update>
//   [R3DRV] beat slot=<n> pid=<n> beats=<n>
//   [R3DRV] kick slot=<n> n=<n> dma_bytes=<n> copy_bytes=<n> errs=<n>
//   [R3DRV] zerocopy disk=<n> lba=<n> secs=<n> bytes=<n> frames=<n> copied=0
//   [R3DRV] copy     disk=<n> lba=<n> secs=<n> bytes=<n> copied=<n>         （对照路径）
//   [R3DRV] service died name=<s> pid=<n> beats=<n> restarts=<n> reason=<exit|beat-timeout>
//   [R3DRV] service restart name=<s> attempt=<n> pid=<n> <ok|FAILED reason=..>
//   [R3DRV] service FAILED name=<s> reason=restart-limit state=read-only
//   [R3DRV] selftest PASS / [R3DRV] selftest FAIL mask=<n>
#include "r3drv64.h"
#include "debug64.h"
#include "memlayout64.h"    // ★ 必须排在 mem_64.h 之前（PAGE_SIZE_64 是宏）
#include "mem_64.h"
#include "x86_64.h"         // g_ticks64 / ms_to_ticks64
#include "ata64.h"          // ata64_identify / ata64_read / ATA64_AHCI_BASE
#include "ahci64.h"         // ahci64_read_frames64（零拷贝散列读）
#include "proc64.h"         // shm_view / proc 生命周期
#include "vfs64.h"          // 读 /etc/r3drv.conf

// ==================== 槽表 ====================
struct R3drvSlotT {
    uint32_t used;
    uint32_t proto;
    uint32_t disk;
    uint32_t state;            // R3DRV_ST_*
    int32_t  pid;
    int32_t  shm_id;
    uint64_t shm_bytes;
    uint64_t lba;              // 服务认领的卷起始 LBA（打点/路由用）
    uint64_t beats;
    uint64_t dma_bytes;
    uint64_t dma_ops;
    uint64_t copy_bytes;
    uint64_t last_beat;        // 最近一次心跳的 tick
    uint32_t restarts;
    uint32_t fails;
    uint64_t started_tick;     // 拉起时刻（等服务注册的兜底超时）
    char     name[R3DRV64_NAME_MAX];
    char     image[R3DRV64_PATH_MAX];
};

static R3drvSlotT g_slot[R3DRV64_MAX];
// ★ v0.9.34：每个槽的"死亡时刻"（PIT tick）。由 proc64 的退出漏斗**即时**打进
//   （r3drv64_on_die64），监督器只在下一拍做状态机推进 + 拉起。0 = 没有待处理的死亡。
static uint64_t   g_die_tick[R3DRV64_MAX];
static uint64_t   g_frames[SHM64_MAX_PAGES64];      // proc64_shm_view64 的帧号数组（4 KiB）
// 拷贝对照路径的内核暂存：**逐页记地址**，绝不假设连续。
// ★ 这里踩过一次真坑：最初只记首址 `g_kbuf`，然后往它后面一次读 64 KiB —— 而 page_alloc_64
//   出来的页**不保证连续**，那样会直接写坏页池里的别人的页。正确做法只有两条：
//   ① 按页读（每页一次 I/O）；② 先探测出"从首址起连续多少页"，用这个长度当上限。
//   下面用 ②（常态下页池是顺序发放的，一次能拿到 64 KiB 连续段，性能不掉）。
static uint8_t*   g_kbuf_pg[16];                    // 16 × 4 KiB = 64 KiB（= AHCI 单命令上限）
static uint32_t   g_kbuf_run = 0;                   // 从 g_kbuf_pg[0] 起**连续**的页数
static int        g_kbuf_pages = 0;
static uint32_t   g_beat_ticks = R3DRV64_BEAT_TICKS;
static uint32_t   g_restarts = 0;
static uint64_t   g_kicks = 0;
static uint32_t   g_kick_logs = 0;
static uint32_t   g_zc_logs = 0;
static uint32_t   g_cp_logs = 0;
static int        g_init = 0;
static int        g_autostart = 1;
static char       g_cfg_service[R3DRV64_PATH_MAX];
static char       g_cfg_watchdog[R3DRV64_PATH_MAX];
static uint32_t   g_cfg_proto = R3DRV_PROTO_EXT4;
static int        g_cfg_disk = -1;                 // -1 = auto
static int        g_route_logged[8];               // 每盘只打一次"unknown-ro"（避免刷屏）

// ==================== 小工具 ====================
static void r3_strcpy(char* d, uint32_t cap, const char* s) {
    if (!d || cap == 0) return;
    uint32_t i = 0;
    if (s) { while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; } }
    d[i] = 0;
}
static int r3_streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return (*a == 0 && *b == 0) ? 1 : 0;
}
static const char* r3_proto_name(uint32_t p) {
    switch (p) {
    case R3DRV_PROTO_EXT4:  return "ext4";
    case R3DRV_PROTO_VIMTU: return "vimtufs2";
    case R3DRV_PROTO_FAT32: return "fat32";
    default:                return "none";
    }
}
static uint32_t r3_proto_by_name(const char* s) {
    if (!s) return R3DRV_PROTO_NONE;
    if (r3_streq(s, "ext4"))     return R3DRV_PROTO_EXT4;
    if (r3_streq(s, "vimtufs2")) return R3DRV_PROTO_VIMTU;
    if (r3_streq(s, "fat32"))    return R3DRV_PROTO_FAT32;
    return R3DRV_PROTO_NONE;
}
// 十进制/十六进制小解析（配置与 argv 用；非法 -> 返回 def）
static int32_t r3_atoi(const char* s, int32_t def) {
    if (!s || !*s) return def;
    int32_t v = 0;
    int neg = 0, i = 0;
    if (s[0] == '-') { neg = 1; i = 1; }
    if (!s[i]) return def;
    for (; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return def;
        v = v * 10 + (s[i] - '0');
        if (v > 100000000) return def;
    }
    return neg ? -v : v;
}
static uint32_t r3_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ==================== 配置 /etc/r3drv.conf ====================
// 为什么要一个**文本配置文件**而不是编死的常量：
//   验收要能证明"没有驱动时系统退回只读安全态"和"驱动崩了会被拉起来"这两条，
//   就必须能在**不改内核**的前提下切换行为。文本文件还能被测试用**等长字节替换**改掉
//   （tests/r3drv64_test.py 就是这么干的：把 "autostart=1" 换成 "autostart=0"）。
static const char kR3drvDefaultConf[] =
    "# /etc/r3drv.conf - VimtuOS ring3 driver framework (v0.9.33)\n"
    "autostart=1\n"
    "service=/usr/bin/ext4svc\n"
    "watchdog=/usr/bin/drvwatch\n"
    "proto=ext4\n"
    "disk=auto\n"
    "beat_ticks=250\n"
    "bench=0\n"
    "crash=0\n";

static void r3_cfg_apply_line(const char* line, int len) {
    if (len <= 0 || line[0] == '#') return;
    char key[24], val[40];
    int i = 0, k = 0;
    while (i < len && line[i] != '=' && k < (int)sizeof(key) - 1) key[k++] = line[i++];
    key[k] = 0;
    if (i >= len || line[i] != '=') return;
    i++;
    int v = 0;
    while (i < len && line[i] != '\r' && line[i] != '\n' && v < (int)sizeof(val) - 1) val[v++] = line[i++];
    val[v] = 0;
    while (k > 0 && (key[k - 1] == ' ' || key[k - 1] == '\t')) key[--k] = 0;
    while (v > 0 && (val[v - 1] == ' ' || val[v - 1] == '\t')) val[--v] = 0;

    if (r3_streq(key, "autostart"))       g_autostart = r3_atoi(val, 1) ? 1 : 0;
    else if (r3_streq(key, "service"))    r3_strcpy(g_cfg_service, sizeof(g_cfg_service), val);
    else if (r3_streq(key, "watchdog"))   r3_strcpy(g_cfg_watchdog, sizeof(g_cfg_watchdog), val);
    else if (r3_streq(key, "proto"))      g_cfg_proto = r3_proto_by_name(val);
    else if (r3_streq(key, "disk"))       g_cfg_disk = r3_streq(val, "auto") ? -1 : (int)r3_atoi(val, -1);
    else if (r3_streq(key, "beat_ticks")) {
        const int32_t e = r3_atoi(val, (int32_t)g_beat_ticks);
        if (e >= 10 && e <= 5000) g_beat_ticks = (uint32_t)e;
    }
    // bench / crash 两项是给**服务自己**读的，内核不解释它们（保持单一职责）
}

static void r3_cfg_load64() {
    g_autostart = 1;
    r3_strcpy(g_cfg_service, sizeof(g_cfg_service), "/usr/bin/ext4svc");
    r3_strcpy(g_cfg_watchdog, sizeof(g_cfg_watchdog), "/usr/bin/drvwatch");
    g_cfg_proto = R3DRV_PROTO_EXT4;
    g_cfg_disk = -1;
    g_beat_ticks = R3DRV64_BEAT_TICKS;

    static char buf[1024];
    int n = vfs64_read64(R3DRV64_CONF_PATH, buf, (uint32_t)(sizeof(buf) - 1));
    const char* src = "vfs";
    if (n <= 0) {
        int len = 0;
        while (kR3drvDefaultConf[len]) len++;
        const int rc = vfs64_write64(R3DRV64_CONF_PATH, kR3drvDefaultConf, (uint32_t)len);
        if (rc < 0) {
            src = "default(seed-denied)";                  // 和 fontcfg64 同口径：写不进不是故障
        } else {
            n = vfs64_read64(R3DRV64_CONF_PATH, buf, (uint32_t)(sizeof(buf) - 1));
            src = (n > 0) ? "seeded" : "default(readback-empty)";
        }
    }
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    if (n < 0) n = 0;
    buf[n] = 0;

    const char* p = buf;
    while (*p) {
        const char* e = p;
        while (*e && *e != '\n') e++;
        r3_cfg_apply_line(p, (int)(e - p));
        if (!*e) break;
        p = e + 1;
    }
    dbg64_line_begin64();
    dbg64_str("[R3DRV] conf src=");        dbg64_str(src);
    dbg64_str(" autostart=");              dbg64_dec((uint64_t)g_autostart);
    dbg64_str(" service=");                dbg64_str(g_cfg_service);
    dbg64_str(" watchdog=");               dbg64_str(g_cfg_watchdog);
    dbg64_str(" proto=");                  dbg64_str(r3_proto_name(g_cfg_proto));
    dbg64_str(" disk=");                   if (g_cfg_disk < 0) dbg64_str("auto"); else dbg64_dec((uint64_t)g_cfg_disk);
    dbg64_str(" beat_ticks=");             dbg64_dec((uint64_t)g_beat_ticks);
    dbg64_nl();
    dbg64_line_end64();
}

// ==================== 初始化 / 自检 ====================
void r3drv64_init64() {
    if (g_init) return;
    g_init = 1;
    for (uint32_t i = 0; i < R3DRV64_MAX; i++) {
        g_slot[i].used = 0;
        g_slot[i].state = R3DRV_ST_FREE;
        g_slot[i].pid = -1;
        g_slot[i].shm_id = -1;
        g_slot[i].name[0] = 0;
        g_slot[i].image[0] = 0;
    }
    // 拷贝对照路径的暂存页：16 页 = 64 KiB = AHCI 单条命令的上限（128 扇区）。
    // 逐页记地址，然后探测"从首址起连续多少页"（g_kbuf_run），拷贝路径拿它当单次 I/O 上限。
    for (int i = 0; i < 16; i++) {
        uint8_t* pg = (uint8_t*)page_alloc_64();
        if (!pg) break;
        for (int k = 0; k < PAGE_SIZE_64; k++) pg[k] = 0;
        g_kbuf_pg[g_kbuf_pages++] = pg;
    }
    g_kbuf_run = 0;
    if (g_kbuf_pages > 0) {
        g_kbuf_run = 1;
        for (int i = 1; i < g_kbuf_pages; i++) {
            if (g_kbuf_pg[i] != g_kbuf_pg[i - 1] + PAGE_SIZE_64) break;   // 断链：连续段到此为止
            g_kbuf_run++;
        }
    }
    r3_cfg_load64();
    dbg64_line_begin64();
    dbg64_str("[R3DRV] init conf=");   dbg64_str(R3DRV64_CONF_PATH);
    dbg64_str(" autostart=");          dbg64_dec((uint64_t)g_autostart);
    dbg64_str(" beat_ticks=");         dbg64_dec((uint64_t)g_beat_ticks);
    dbg64_str(" kbuf_pages=");         dbg64_dec((uint64_t)g_kbuf_pages);
    dbg64_str(" slots=");              dbg64_dec((uint64_t)R3DRV64_MAX);
    dbg64_str(" abi=");                dbg64_dec((uint64_t)R3DRV_ABI_VER);
    dbg64_nl();
    dbg64_line_end64();
}

int r3drv64_selftest64() {
    uint32_t mask = 0;
    // bit0：ABI 布局。用户态与内核共用 r3drv_abi.h，任何一侧改动都要在这里立刻炸出来。
    if (sizeof(R3drvSqe) != 32) mask |= 1u;
    if (sizeof(R3drvRing) != 56) mask |= 2u;
    if (R3DRV_SQ_OFF + R3DRV_SLOTS * sizeof(R3drvSqe) > R3DRV_DATA_OFF) mask |= 4u;
    if (sizeof(g_frames) / sizeof(g_frames[0]) < 8) mask |= 8u;
    // bit1：槽表必须自洽（FREE 的槽不许挂着 pid/shm）
    for (uint32_t i = 0; i < R3DRV64_MAX; i++) {
        if (g_slot[i].state == R3DRV_ST_FREE && g_slot[i].used) mask |= 16u;
        if (g_slot[i].used && g_slot[i].name[0] == 0) mask |= 16u;
    }
    // bit2：拷贝对照路径必须有暂存页（否则"对照"实验做不了，但零拷贝照常工作）
    if (g_kbuf_pages < 16) mask |= 32u;
    // bit3：配置项范围
    if (g_beat_ticks < 10 || g_beat_ticks > 5000) mask |= 64u;

    dbg64_line_begin64();
    if (mask == 0) dbg64_str("[R3DRV] selftest PASS");
    else { dbg64_str("[R3DRV] selftest FAIL mask="); dbg64_dec((uint64_t)mask); }
    dbg64_str(" slots_used="); dbg64_dec((uint64_t)r3drv64_count64());
    dbg64_str(" restarts=");   dbg64_dec((uint64_t)g_restarts);
    dbg64_nl();
    dbg64_line_end64();
    return (int)mask;
}

// ==================== 探盘 / 路由 ====================
// 读一个扇区到静态暂存（PA==VA，所以 PIO/DMA 两条路都能直接用它）
static uint8_t g_sec[512] __attribute__((aligned(8)));
static uint8_t g_sec2[512] __attribute__((aligned(8)));

static int r3_rd64(uint32_t disk, uint32_t lba, uint32_t n, void* buf) {
    return ata64_read((int)disk, lba, n, buf) ? 1 : 0;
}

// 认文件系统（**只读**，一个字节都不写盘）
uint32_t r3drv64_probe_proto64(uint32_t disk, uint32_t lba) {
    // 1) VimtuFS2：卷首扇区就是 "VIMTUFS2"
    if (r3_rd64(disk, lba, 1, g_sec)) {
        static const char m[] = "VIMTUFS2";
        int ok = 1;
        for (int i = 0; i < 8; i++) if (g_sec[i] != (uint8_t)m[i]) { ok = 0; break; }
        if (ok) return R3DRV_PROTO_VIMTU;
        // 2) FAT32：BPB 偏移 82 处的 "FAT32   "
        if (g_sec[510] == 0x55 && g_sec[511] == 0xAA) {
            static const char f[] = "FAT32";
            int fok = 1;
            for (int i = 0; i < 5; i++) if (g_sec[82 + i] != (uint8_t)f[i]) { fok = 0; break; }
            if (fok) return R3DRV_PROTO_FAT32;
        }
    }
    // 3) ext4：超级块在卷内字节 1024（= 扇区 2 偏移 0），magic 0xEF53 在超级块 +0x38。
    if (r3_rd64(disk, lba + 2, 1, g_sec2)) {
        if (g_sec2[0x38] == 0x53 && g_sec2[0x39] == 0xEF) return R3DRV_PROTO_EXT4;
    }
    return R3DRV_PROTO_NONE;
}

static int r3_drive_present64(int drive, uint64_t* out_sectors) {
    DiskInfo di;
    if (!ata64_identify(drive, &di)) return 0;
    if (!di.present || di.atapi) return 0;               // ★ ATAPI/空槽不算盘（踩过：会读到垃圾）
    if (out_sectors) *out_sectors = di.sectors;
    return 1;
}

// 在一块盘的所有 MBR 分区项（以及整盘）上找某协议
static int r3_find_on_drive64(int drive, uint32_t want, uint32_t* out_lba) {
    if (r3_rd64((uint32_t)drive, 0, 1, g_sec) && g_sec[510] == 0x55 && g_sec[511] == 0xAA) {
        for (int p = 0; p < 4; p++) {
            const uint8_t* e = g_sec + 446 + p * 16;
            const uint32_t type = e[4];
            const uint32_t start = r3_le32(e + 8);
            const uint32_t secs = r3_le32(e + 12);
            if (type == 0 || start == 0 || secs == 0) continue;
            if (r3drv64_probe_proto64((uint32_t)drive, start) == want) {
                if (out_lba) *out_lba = start;
                return 1;
            }
        }
    }
    // 整盘当卷（没有分区表的 ext4 夹具很常见）
    if (r3drv64_probe_proto64((uint32_t)drive, 0) == want) {
        if (out_lba) *out_lba = 0;
        return 1;
    }
    return 0;
}

int r3drv64_find_volume64(uint32_t proto, uint32_t* out_disk, uint32_t* out_lba) {
    if (proto == R3DRV_PROTO_NONE) return 0;
    for (int d = 0; d < 4; d++) {                        // PATA 0..3
        if (!r3_drive_present64(d, nullptr)) continue;
        uint32_t lba = 0;
        if (r3_find_on_drive64(d, proto, &lba)) {
            if (out_disk) *out_disk = (uint32_t)d;
            if (out_lba) *out_lba = lba;
            return 1;
        }
    }
    for (int d = ATA64_AHCI_BASE; d < ATA64_AHCI_BASE + 8; d++) {   // AHCI 8..15
        if (!r3_drive_present64(d, nullptr)) continue;
        uint32_t lba = 0;
        if (r3_find_on_drive64(d, proto, &lba)) {
            if (out_disk) *out_disk = (uint32_t)d;
            if (out_lba) *out_lba = lba;
            return 1;
        }
    }
    return 0;
}

static int r3_slot_for64(uint32_t proto, uint32_t disk) {
    for (uint32_t i = 0; i < R3DRV64_MAX; i++)
        if (g_slot[i].used && g_slot[i].proto == proto && g_slot[i].disk == disk &&
            g_slot[i].state == R3DRV_ST_RUN) return (int)i;
    return -1;
}

int r3drv64_route64(uint32_t disk, uint32_t lba, int* out_slot) {
    if (out_slot) *out_slot = -1;
    const uint32_t proto = r3drv64_probe_proto64(disk, lba);
    if (proto == R3DRV_PROTO_NONE) {
        if (disk < 8 && !g_route_logged[disk & 7]) {
            g_route_logged[disk & 7] = 1;
            dbg64_line_begin64();
            dbg64_str("[R3DRV] route disk="); dbg64_dec((uint64_t)disk);
            dbg64_str(" lba=");               dbg64_dec((uint64_t)lba);
            dbg64_str(" proto=none mode=unknown-ro");
            dbg64_nl();
            dbg64_line_end64();
        }
        return R3DRV_ROUTE_UNKNOWN;
    }
    const int si = r3_slot_for64(proto, disk);
    dbg64_line_begin64();
    dbg64_str("[R3DRV] route disk="); dbg64_dec((uint64_t)disk);
    dbg64_str(" lba=");               dbg64_dec((uint64_t)lba);
    dbg64_str(" proto=");             dbg64_str(r3_proto_name(proto));
    if (si >= 0) {
        dbg64_str(" mode=driver slot="); dbg64_dec((uint64_t)si);
        if (out_slot) *out_slot = si;
    } else {
        // 认得出文件系统但没有可用驱动 = **只读安全态**（不是"未知"，也不假装能写）
        dbg64_str(" mode=read-only reason=");
        int any = 0;
        for (uint32_t i = 0; i < R3DRV64_MAX; i++)
            if (g_slot[i].used && g_slot[i].proto == proto && g_slot[i].state == R3DRV_ST_FAILED) any = 1;
        dbg64_str(any ? "failed" : "no-driver");
    }
    dbg64_nl();
    dbg64_line_end64();
    return (si >= 0) ? R3DRV_ROUTE_DRIVER : R3DRV_ROUTE_RO;
}

// ==================== 零拷贝 / 对照路径的真正落地 ====================
// 把对象的帧号数组取出来（顺带核对页数与调方是否真的持有该句柄）。
static int r3_frames_of64(int32_t shm_id, uint32_t* out_pages, uint32_t* out_refs) {
    if (shm_id <= 0) return 0;
    uint32_t pages = 0, refs = 0;
    if (proc64_shm_view64((uint32_t)shm_id, g_frames, SHM64_MAX_PAGES64, &pages, &refs) != 0) return 0;
    if (out_pages) *out_pages = pages;
    if (out_refs)  *out_refs  = refs;
    return (pages > 0) ? 1 : 0;
}

// 一条请求的核心：把 [lba, lba+secs) 搬到对象内 [obj_off, obj_off+secs*512)。
// copy_path=0：DMA 直达对象帧（零拷贝）；copy_path=1：内核暂存 + memcpy（对照）。
static int r3_do_io64(const R3drvSlotT* s, uint64_t obj_off, uint64_t lba,
                      uint32_t secs, int copy_path, uint32_t* out_bytes) {
    if (out_bytes) *out_bytes = 0;
    if (secs == 0) return 0;
    if (obj_off < (uint64_t)R3DRV_DATA_OFF || (obj_off & 4095ull) != 0) return -3;   // EINVAL
    uint32_t pages = 0;
    if (!r3_frames_of64(s->shm_id, &pages, nullptr)) return -19;                     // ENODEV
    const uint32_t fi0 = (uint32_t)(obj_off / 4096ull);
    if (fi0 >= pages) return -3;
    const uint64_t need = (uint64_t)secs * 512ull;
    if (need > ((uint64_t)(pages - fi0) * 4096ull)) return -3;

    if (!copy_path && s->disk >= (uint32_t)ATA64_AHCI_BASE) {
        // ---- 零拷贝：一帧一项 PRDT，DMA 直接写进服务的页 ----
        uint64_t bytes = 0;
        if (!ahci64_read_frames64((int)s->disk - ATA64_AHCI_BASE, (uint32_t)lba, secs,
                                  &g_frames[fi0], pages - fi0, &bytes)) {
            if (out_bytes) *out_bytes = (uint32_t)bytes;
            return -5;                                                               // EIO
        }
        if (out_bytes) *out_bytes = (uint32_t)bytes;
        if (g_zc_logs < 4 || (g_zc_logs % 64) == 0) {
            g_zc_logs++;
            dbg64_line_begin64();
            dbg64_str("[R3DRV] zerocopy disk="); dbg64_dec((uint64_t)s->disk);
            dbg64_str(" lba=");                  dbg64_dec(lba);
            dbg64_str(" secs=");                 dbg64_dec((uint64_t)secs);
            dbg64_str(" bytes=");                dbg64_dec(bytes);
            dbg64_str(" frames=");               dbg64_dec((uint64_t)((secs + 7u) / 8u));
            dbg64_str(" copied=0");
            dbg64_nl();
            dbg64_line_end64();
        } else {
            g_zc_logs++;
        }
        return 0;
    }

    // ---- 对照 / 兜底：内核暂存 -> 逐帧 memcpy ----
    // 为什么要留这条路：① 取证（证明零拷贝省下的确实是那份拷贝）；
    //                  ② 非 AHCI 盘（PATA PIO）没有 DMA 目标地址，只能这么搬。
    uint32_t done_sec = 0;
    uint32_t copied = 0;
    if (g_kbuf_pages <= 0 || g_kbuf_run == 0) return -12;          // ENOMEM：没有暂存页就没有对照路径
    const uint32_t max_sec = g_kbuf_run * 8u;                      // 4 KiB/页 = 8 扇区
    while (done_sec < secs) {
        uint32_t n = secs - done_sec;
        if (n > max_sec) n = max_sec;                              // 只读"确实连续"的那一段
        if (!ata64_read((int)s->disk, (uint32_t)(lba + done_sec), n, g_kbuf_pg[0])) return -5;
        // 拷进对象帧：段首帧对齐（obj_off 已校验 4 KiB 对齐），所以帧内偏移恒为 0
        uint32_t left = n * 512u;
        uint32_t src_off = 0;
        uint32_t fi = fi0 + (done_sec / 8u);
        while (left > 0) {
            if (fi >= pages) return -5;
            uint32_t take = left > 4096u ? 4096u : left;
            uint8_t* dst = (uint8_t*)(uintptr_t)g_frames[fi];
            const uint8_t* src = g_kbuf_pg[0] + src_off;
            for (uint32_t k = 0; k < take; k++) dst[k] = src[k];
            copied += take;
            src_off += take;
            left -= take;
            fi++;
        }
        done_sec += n;
    }
    if (out_bytes) *out_bytes = copied;
    if (g_cp_logs < 4 || (g_cp_logs % 64) == 0) {
        dbg64_line_begin64();
        dbg64_str("[R3DRV] copy disk="); dbg64_dec((uint64_t)s->disk);
        dbg64_str(" lba=");              dbg64_dec(lba);
        dbg64_str(" secs=");             dbg64_dec((uint64_t)secs);
        dbg64_str(" bytes=");            dbg64_dec((uint64_t)copied);
        dbg64_str(" copied=");           dbg64_dec((uint64_t)copied);
        dbg64_nl();
        dbg64_line_end64();
    }
    g_cp_logs++;
    return 0;
}

// ==================== 注册 / 心跳 / 门铃 ====================
int64_t r3drv64_register64(const char* name, uint32_t proto, uint32_t disk,
                           int32_t shm_id, uint64_t bytes) {
    if (!g_init) r3drv64_init64();
    const int pid = proc64_current_pid64();
    if (pid <= 0) return -13;                                    // 没有进程上下文
    if (proto == R3DRV_PROTO_NONE || proto >= R3DRV_PROTO_MAX) return -3;
    if (!name || !name[0]) return -3;
    if (bytes < (uint64_t)R3DRV_DATA_OFF + 4096ull) return -3;   // 至少要 1 页数据区
    if (!proc64_shm_has_handle_id64(shm_id)) return -1;          // ★ 不是自己的对象就别认领

    int idx = -1;
    for (uint32_t i = 0; i < R3DRV64_MAX; i++) {
        if (g_slot[i].used && r3_streq(g_slot[i].name, name)) { idx = (int)i; break; }
    }
    // ★ mode 的语义：**"这个槽本来就在服务"才算 update**。
    //   不能用"槽是否已 used"判断 —— start_service 会预先占槽（为的是记下 ELF 路径好重启），
    //   于是首次注册永远会被误标成 update（实测踩过）。判据改成"注册前它是不是 RUN"。
    const int mode_update = (idx >= 0 && g_slot[idx].state == R3DRV_ST_RUN);
    if (idx < 0) {
        for (uint32_t i = 0; i < R3DRV64_MAX; i++) if (!g_slot[i].used) { idx = (int)i; break; }
        if (idx < 0) return -12;                                  // 槽满
        R3drvSlotT& s = g_slot[idx];
        s.used = 1;
        s.restarts = 0;
        s.fails = 0;
        s.beats = 0;
        s.dma_bytes = 0;
        s.dma_ops = 0;
        s.copy_bytes = 0;
        s.image[0] = 0;              // ★ 只有"全新槽"才清空 image；被 start_service 预占的槽必须留着
        r3_strcpy(s.name, sizeof(s.name), name);
    }
    R3drvSlotT& s = g_slot[idx];
    s.proto = proto;
    s.disk = disk;
    s.shm_id = shm_id;
    s.shm_bytes = bytes;
    s.pid = pid;
    s.state = R3DRV_ST_RUN;
    s.last_beat = g_ticks64;
    s.started_tick = g_ticks64;
    g_die_tick[idx] = 0;                 // 新实例认领：上一轮的死亡账已结清


    dbg64_line_begin64();
    dbg64_str("[R3DRV] register name="); dbg64_str(s.name);
    dbg64_str(" proto=");                dbg64_str(r3_proto_name(proto));
    dbg64_str(" disk=");                 dbg64_dec((uint64_t)disk);
    dbg64_str(" shm=");                  dbg64_dec((uint64_t)(shm_id < 0 ? 0 : shm_id));
    dbg64_str(" bytes=");                dbg64_dec(bytes);
    dbg64_str(" pid=");                  dbg64_dec((uint64_t)pid);
    dbg64_str(" slot=");                 dbg64_dec((uint64_t)idx);
    dbg64_str(" mode=");                 dbg64_str(mode_update ? "update" : "new");
    dbg64_nl();
    dbg64_line_end64();
    // ★ 路由状态迁移的第二端：注册之前 autostart 已经把"认得出 fs 但没有驱动 -> 只读安全态"
    //   记进了日志（mode=read-only）；这一刻驱动活了，于是补一条 mode=driver。
    //   两条日志放在同一个卷上是"按分区类型路由"最直接的证据 —— 不改任何挂载状态，纯状态机。
    dbg64_line_begin64();
    dbg64_str("[R3DRV] route disk="); dbg64_dec((uint64_t)disk);
    dbg64_str(" lba=");               dbg64_dec(s.lba);
    dbg64_str(" proto=");             dbg64_str(r3_proto_name(proto));
    dbg64_str(" mode=driver slot=");  dbg64_dec((uint64_t)idx);
    dbg64_nl();
    dbg64_line_end64();
    return (int64_t)idx;
}

int64_t r3drv64_beat64() {
    const int pid = proc64_current_pid64();
    if (pid <= 0) return -13;
    for (uint32_t i = 0; i < R3DRV64_MAX; i++) {
        R3drvSlotT& s = g_slot[i];
        if (!s.used || s.pid != pid) continue;
        s.beats++;
        s.last_beat = g_ticks64;
        if (s.state == R3DRV_ST_STALE || s.state == R3DRV_ST_DYING) s.state = R3DRV_ST_RUN;
        if ((s.beats & 0x3Fu) == 1u) {                 // 每 64 拍打一行（不刷屏）
            dbg64_line_begin64();
            dbg64_str("[R3DRV] beat slot="); dbg64_dec((uint64_t)i);
            dbg64_str(" pid=");              dbg64_dec((uint64_t)pid);
            dbg64_str(" beats=");            dbg64_dec(s.beats);
            dbg64_str(" restarts=");         dbg64_dec((uint64_t)s.restarts);
            dbg64_nl();
            dbg64_line_end64();
        }
        return 0;
    }
    return -19;                                        // 没有注册过的心跳
}

// 服务进程"还活着吗"——★ 不能只用 proc64_find64：
//   进程崩溃/正常退出后**槽还在**（PROC64_EXITED = 僵尸，等父进程 wait4 收），
//   所以 find64 仍然返回 1。只看它就会把"已经死了的僵尸"当活的，只能等心跳超时才反应过来。
//   这里再查一次状态：EXITED 就是死了。服务是由内核拉起的（ppid=0，没人 wait4），
//   所以这个僵尸不会自己消失 —— 只能由监督器收尸（proc64_destroy64）。
static int r3_proc_alive64(int pid) {
    if (pid <= 0) return 0;
    if (!proc64_find64(pid)) return 0;
    Proc64Info info;
    for (int i = 0; i < PROC64_MAX; i++) {
        if (proc64_info64(i, &info) != 1) continue;
        if ((int)info.pid != pid) continue;
        return (info.state == (uint32_t)PROC64_EXITED) ? 0 : 1;
    }
    return 1;                                   // 查不到就按"活着"处理（宁可多活一拍，不乱杀）
}

static R3drvSlotT* r3_slot_by_shm64(int32_t shm_id) {
    for (uint32_t i = 0; i < R3DRV64_MAX; i++)
        if (g_slot[i].used && g_slot[i].shm_id == shm_id && g_slot[i].state == R3DRV_ST_RUN)
            return &g_slot[i];
    return nullptr;
}

// 门铃：把服务在共享环里排好的 SQE 全部消费掉。
// 只信任**自己的**对象 + magic 对得上；其余一律如实拒（绝不猜服务想干什么）。
int64_t r3drv64_kick64(int32_t shm_id) {
    if (!g_init) r3drv64_init64();
    R3drvSlotT* s = r3_slot_by_shm64(shm_id);
    if (!s) return -19;
    if (!proc64_shm_has_handle_id64(shm_id)) return -1;
    uint32_t pages = 0;
    if (!r3_frames_of64(shm_id, &pages, nullptr)) return -19;
    if (pages == 0) return -19;
    // 环控制块在对象第 0 帧；内核用恒等映射（PA==VA）直接读写它（与 ahci64 的 DMA 池同源约定）
    R3drvRing* ring = (R3drvRing*)(uintptr_t)g_frames[0];
    if (ring->magic != R3DRV_RING_MAGIC) return -3;
    if (ring->ver != R3DRV_ABI_VER)      return -3;

    uint64_t dma_b = 0, copy_b = 0;
    uint32_t n = 0;
    uint32_t errs = 0;
    // 就地完成的 SQE：索引 = 生产/消费位置的模
    volatile R3drvSqe* sq = (volatile R3drvSqe*)(uintptr_t)(g_frames[0] + R3DRV_SQ_OFF);
    while (ring->tail != ring->head && n < R3DRV_SLOTS) {
        const uint32_t pos = ring->tail & (R3DRV_SLOTS - 1u);
        volatile R3drvSqe* q = &sq[pos];
        if (q->state != R3DRV_SQ_PENDING) { ring->tail++; continue; }
        uint32_t bytes = 0;
        const int rc = r3_do_io64(s, q->obj_off, q->lba, q->secs,
                                  (q->flags & R3DRV_F_COPY) ? 1 : 0, &bytes);
        q->bytes = bytes;
        q->state = (rc == 0) ? R3DRV_SQ_DONE : R3DRV_SQ_FAILED;
        if (rc == 0) {
            if (q->flags & R3DRV_F_COPY) { copy_b += bytes; s->copy_bytes += bytes; }
            else                         { dma_b  += bytes; s->dma_bytes  += bytes; s->dma_ops++; }
        } else {
            errs++;
            s->fails++;
        }
        ring->tail++;
        n++;
    }
    ring->kicks++;
    ring->dma_bytes  += dma_b;
    ring->copy_bytes += copy_b;
    ring->errs       += errs;
    g_kicks++;
    if (g_kick_logs < R3DRV64_KICK_LOG_MAX || (g_kicks % 64) == 0) {
        g_kick_logs++;
        dbg64_line_begin64();
        dbg64_str("[R3DRV] kick slot=");  dbg64_dec((uint64_t)(s - g_slot));
        dbg64_str(" n=");                 dbg64_dec((uint64_t)n);
        dbg64_str(" dma_bytes=");         dbg64_dec(dma_b);
        dbg64_str(" copy_bytes=");        dbg64_dec(copy_b);
        dbg64_str(" errs=");              dbg64_dec((uint64_t)errs);
        dbg64_nl();
        dbg64_line_end64();
    }
    if (errs) return -(int64_t)errs;
    return (int64_t)n;
}

int64_t r3drv64_dma_read64(int32_t shm_id, uint64_t obj_off, uint64_t lba, uint32_t secs) {
    if (!g_init) r3drv64_init64();
    R3drvSlotT* s = r3_slot_by_shm64(shm_id);
    if (!s) return -19;
    uint32_t bytes = 0;
    const int rc = r3_do_io64(s, obj_off, lba, secs, 0, &bytes);
    if (rc != 0) { s->fails++; return rc; }
    s->dma_bytes += bytes;
    s->dma_ops++;
    return (int64_t)bytes;
}

int64_t r3drv64_copy_read64(int32_t shm_id, uint64_t obj_off, uint64_t lba, uint32_t secs) {
    if (!g_init) r3drv64_init64();
    R3drvSlotT* s = r3_slot_by_shm64(shm_id);
    if (!s) return -19;
    uint32_t bytes = 0;
    const int rc = r3_do_io64(s, obj_off, lba, secs, 1, &bytes);
    if (rc != 0) { s->fails++; return rc; }
    s->copy_bytes += bytes;
    return (int64_t)bytes;
}

int r3drv64_info64(uint32_t slot, R3drvInfo* out) {
    if (!out || slot >= R3DRV64_MAX) return 0;
    const R3drvSlotT& s = g_slot[slot];
    if (!s.used) return 0;
    out->slot = slot;
    out->proto = s.proto;
    out->disk = s.disk;
    out->state = s.state;
    out->pid = s.pid;
    out->shm_id = s.shm_id;
    out->restarts = s.restarts;
    out->fails = s.fails;
    out->beats = s.beats;
    out->dma_bytes = s.dma_bytes;
    out->dma_ops = s.dma_ops;
    out->copy_bytes = s.copy_bytes;
    r3_strcpy(out->name, sizeof(out->name), s.name);
    r3_strcpy(out->image, sizeof(out->image), s.image);
    return 1;
}

// ==================== 拉起 / 监督 ====================
int r3drv64_start_service64(const char* path, const char* name, uint32_t proto,
                            uint32_t disk, uint32_t lba, int* out_slot, int* out_pid) {
    if (out_slot) *out_slot = -1;
    if (out_pid) *out_pid = -1;
    if (!g_init) r3drv64_init64();
    if (!path || !path[0]) return 0;
    if (!proc64_isolate64()) {                       // 共享地址空间模式：fork/execve 不可用
        dbg64_line_begin64();
        dbg64_str("[R3DRV] start FAILED reason=no-isolation path="); dbg64_str(path);
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    // 先占槽（重启用得着 image/proto/disk/lba），进程跑起来再由 register 补 pid/shm
    int idx = -1;
    for (uint32_t i = 0; i < R3DRV64_MAX; i++) if (g_slot[i].used && r3_streq(g_slot[i].name, name)) { idx = (int)i; break; }
    if (idx < 0) {
        for (uint32_t i = 0; i < R3DRV64_MAX; i++) if (!g_slot[i].used) { idx = (int)i; break; }
    }
    if (idx < 0) {
        dbg64_line_begin64();
        dbg64_str("[R3DRV] start FAILED reason=no-slot name="); dbg64_str(name);
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    R3drvSlotT& s = g_slot[idx];
    const uint32_t keep_restarts = s.used ? s.restarts : 0;
    s.used = 1;
    s.state = R3DRV_ST_DYING;
    s.proto = proto;
    s.disk = disk;
    s.lba = lba;
    s.shm_id = -1;                                   // 旧对象随旧进程一起释放了
    s.pid = -1;
    s.restarts = keep_restarts;
    s.fails = 0;
    s.beats = 0;
    s.dma_bytes = 0;
    s.dma_ops = 0;
    s.copy_bytes = 0;
    s.last_beat = g_ticks64;
    s.started_tick = g_ticks64;
    r3_strcpy(s.name, sizeof(s.name), name);
    r3_strcpy(s.image, sizeof(s.image), path);

    const int pid = proc64_create64(name, 0);
    if (pid <= 0) {
        s.state = R3DRV_ST_FAILED;
        dbg64_line_begin64();
        dbg64_str("[R3DRV] start name="); dbg64_str(name);
        dbg64_str(" path=");              dbg64_str(path);
        dbg64_str(" pid=0 slot=");        dbg64_dec((uint64_t)idx);
        dbg64_str(" FAILED reason=create");
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    // ★ 立刻把 pid 写进槽：`proc64_start_elf64_argv64` 要读 ELF（磁盘 I/O）且**可被抢占**，
    //   而监督器跑在另一个线程（kheart）。若这中间槽里还是 pid=-1 + state=DYING，
    //   监督器就会把"还没启动完"当成"进程没了"→ 凭空重启一次（实测过：日志里出现
    //   `service died pid=0 beats=0 restarts=0 reason=exit` 紧跟一次失败的 restart）。
    s.pid = pid;
    // argv：path + "<disk>" + "<lba>"（十进制）。服务靠它知道自己该服务哪个卷。
    char a_disk[12], a_lba[12];
    {
        int v = (int)disk, i = 0;
        char tmp[12]; int t = 0;
        if (v == 0) tmp[t++] = '0';
        while (v > 0 && t < 11) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
        while (t > 0) a_disk[i++] = tmp[--t];
        a_disk[i] = 0;
        unsigned int u = (unsigned int)lba; i = 0; t = 0;
        if (u == 0) tmp[t++] = '0';
        while (u > 0 && t < 11) { tmp[t++] = (char)('0' + (u % 10)); u /= 10; }
        while (t > 0) a_lba[i++] = tmp[--t];
        a_lba[i] = 0;
    }
    const char* argv[3];
    argv[0] = path; argv[1] = a_disk; argv[2] = a_lba;
    const int rc = proc64_start_elf64_argv64(pid, path, argv, 3);
    if (rc != 0) {
        (void)proc64_kill64(pid, 9);
        if (proc64_find64(pid)) proc64_destroy64(pid);
        s.state = R3DRV_ST_FAILED;
        s.pid = -1;
        dbg64_line_begin64();
        dbg64_str("[R3DRV] start name="); dbg64_str(name);
        dbg64_str(" path=");              dbg64_str(path);
        dbg64_str(" pid=");               dbg64_dec((uint64_t)pid);
        dbg64_str(" slot=");              dbg64_dec((uint64_t)idx);
        dbg64_str(" FAILED reason=elf");
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    s.pid = pid;
    dbg64_line_begin64();
    dbg64_str("[R3DRV] start name="); dbg64_str(name);
    dbg64_str(" path=");              dbg64_str(path);
    dbg64_str(" proto=");             dbg64_str(r3_proto_name(proto));
    dbg64_str(" disk=");              dbg64_dec((uint64_t)disk);
    dbg64_str(" lba=");               dbg64_dec((uint64_t)lba);
    dbg64_str(" pid=");               dbg64_dec((uint64_t)pid);
    dbg64_str(" slot=");              dbg64_dec((uint64_t)idx);
    dbg64_str(" ok");
    dbg64_nl();
    dbg64_line_end64();
    if (out_slot) *out_slot = idx;
    if (out_pid) *out_pid = pid;
    return 1;
}

// ==================== 死亡即时通知（proc64 退出漏斗回调）====================
// ★ v0.9.34：原来监督器只能靠"下一拍轮询"发现服务没了 —— 那要等心跳线程的一个周期（1 s），
//   于是"崩了多久才被拉起"这件事既不可观测也不可控。现在进程一进入 EXITED 就由
//   p64_exit64 直接叫到这里（**同一拍**），只认领槽位 + 打时间戳；真正拉起仍在心跳线程里做
//   （拉起要读 ELF = 磁盘 I/O，不能出现在退出路径上）。
//   g_die_tick 存 `tick+1`：0 表示"没有待处理的死亡"。
extern "C" void r3drv64_on_die64(int pid) {
    if (!g_init || pid <= 0) return;
    for (uint32_t i = 0; i < R3DRV64_MAX; i++) {
        const R3drvSlotT& s = g_slot[i];
        if (s.used && s.pid == pid && g_die_tick[i] == 0u) {
            g_die_tick[i] = g_ticks64 + 1u;
            return;
        }
    }
}

// 监督：只推进状态机，不等待、不 sleep（GUI 帧循环里调也安全 —— 它不做磁盘 I/O，
// 只有**拉起**那一瞬才会读 ELF；所以调用点选在内核心跳循环而不是帧循环）。
void r3drv64_tick64() {
    if (!g_init) return;
    for (uint32_t i = 0; i < R3DRV64_MAX; i++) {
        R3drvSlotT& s = g_slot[i];
        if (!s.used) continue;
        if (s.state == R3DRV_ST_FAILED) continue;

        const int alive = r3_proc_alive64(s.pid);        // ★ 僵尸算死（见 r3_proc_alive64 的说明）
        const char* why = nullptr;
        // ★ 状态机必须按"槽处在哪个阶段"分别判，不能一律 `!alive -> died`：
        //   * DYING = 正在拉起（start_service 里 pid 立刻写好了，但 ELF 还没装完）。
        //     这一段**只有两种情况算事故**：进程建了又没了（exit）、或迟迟不注册（超时）。
        //     pid 还没分配时（理论上只在 create 前后几微秒）什么都不做 —— 让超时兜底。
        if (s.state == R3DRV_ST_DYING) {
            if (s.pid > 0 && !alive) {
                why = "exit";
            } else if ((g_ticks64 - s.started_tick) > (uint64_t)(g_beat_ticks * 5u)) {
                why = "register-timeout";            // 拉起后迟迟不注册：当失败处理
            }
        } else if (!alive) {
            why = "exit";                            // RUN/STALE 阶段进程没了 = 真死
        } else if ((g_ticks64 - s.last_beat) > (uint64_t)g_beat_ticks) {
            why = "beat-timeout";
        }
        if (!why) continue;

        // 死亡时刻：优先用 proc64 退出漏斗即时打的时间戳（= 真异常/退出发生的那一拍），
        // 没有就用"这一拍发现"当时间戳（beat-timeout 等）。PIT 250Hz -> 1 tick = 4ms。
        if (!g_die_tick[i]) g_die_tick[i] = g_ticks64 + 1u;      // +1：0 保留为"无待处理"
        const uint64_t died_at = g_die_tick[i] - 1u;
        const uint64_t lat_ms = (g_ticks64 > died_at ? (g_ticks64 - died_at) : 0u) * 4u;
        dbg64_line_begin64();
        dbg64_str("[R3DRV] service died name="); dbg64_str(s.name);
        dbg64_str(" pid=");                      dbg64_dec((uint64_t)(s.pid < 0 ? 0 : s.pid));
        dbg64_str(" beats=");                    dbg64_dec(s.beats);
        dbg64_str(" restarts=");                 dbg64_dec((uint64_t)s.restarts);
        dbg64_str(" reason=");                   dbg64_str(why);
        dbg64_str(" lat_ms=");                   dbg64_dec(lat_ms);
        dbg64_nl();
        dbg64_line_end64();

        // 收尸：把进程槽/地址空间收回来，否则重启几次就把 PROC64_MAX 耗干
        if (s.pid > 0) {
            if (proc64_find64(s.pid)) proc64_destroy64(s.pid);
            s.pid = -1;
        }
        s.shm_id = -1;
        s.state = R3DRV_ST_STALE;

        if (s.restarts >= R3DRV64_RESTART_MAX || !s.image[0]) {
            s.state = R3DRV_ST_FAILED;
            dbg64_line_begin64();
            dbg64_str("[R3DRV] service FAILED name="); dbg64_str(s.name);
            dbg64_str(" reason=");                     dbg64_str(s.image[0] ? "restart-limit" : "no-image");
            dbg64_str(" state=read-only");
            dbg64_nl();
            dbg64_line_end64();
            continue;
        }
        s.restarts++;
        g_restarts++;
        int slot2 = -1, pid2 = -1;
        char img[R3DRV64_PATH_MAX];
        r3_strcpy(img, sizeof(img), s.image);
        char nm[R3DRV64_NAME_MAX];
        r3_strcpy(nm, sizeof(nm), s.name);
        const uint32_t proto = s.proto, disk = s.disk;
        const uint32_t lba = (uint32_t)s.lba;
        // start_service 会重置 pid/shm/state，并把 restarts 保住
        const int ok = r3drv64_start_service64(img, nm, proto, disk, lba, &slot2, &pid2);
        const uint64_t now_t = g_ticks64;
        const uint64_t lat2_ms = (now_t > died_at ? (now_t - died_at) : 0u) * 4u;
        dbg64_line_begin64();
        dbg64_str("[R3DRV] service restart name="); dbg64_str(nm);
        dbg64_str(" attempt=");                    dbg64_dec(s.restarts);
        dbg64_str(" pid=");                        dbg64_dec((uint64_t)(pid2 < 0 ? 0 : pid2));
        dbg64_str(ok ? " ok" : " FAILED reason=start");
        dbg64_str(" lat_ms=");                     dbg64_dec(lat2_ms);
        dbg64_nl();
        dbg64_line_end64();
        g_die_tick[i] = 0;                        // 这一轮的死亡已处理完
        if (ok && slot2 >= 0) g_slot[slot2].restarts = s.restarts;
    }
}

// ==================== 开机自启 ====================
int r3drv64_autostart64() {
    if (!g_init) r3drv64_init64();
    if (!g_autostart) {
        dbg64_line_begin64();
        dbg64_str("[R3DRV] autostart skipped reason=off");
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    uint32_t disk = 0, lba = 0;
    int found = 0;
    if (g_cfg_disk >= 0) {
        // 指定盘：只探它（分区 + 整盘）
        if (r3_drive_present64(g_cfg_disk, nullptr) &&
            r3_find_on_drive64(g_cfg_disk, g_cfg_proto, &lba)) {
            disk = (uint32_t)g_cfg_disk;
            found = 1;
        }
    } else {
        found = r3drv64_find_volume64(g_cfg_proto, &disk, &lba);
    }
    if (!found) {
        dbg64_line_begin64();
        dbg64_str("[R3DRV] autostart skipped reason=no-target-volume proto=");
        dbg64_str(r3_proto_name(g_cfg_proto));
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    dbg64_line_begin64();
    dbg64_str("[R3DRV] probe disk="); dbg64_dec((uint64_t)disk);
    dbg64_str(" lba=");              dbg64_dec((uint64_t)lba);
    dbg64_str(" proto=");            dbg64_str(r3_proto_name(g_cfg_proto));
    dbg64_nl();
    dbg64_line_end64();

    // 先把"还没驱动"的只读安全态记进日志，再起服务 —— 测试要能看到状态迁移的两端
    (void)r3drv64_route64(disk, lba, nullptr);

    int slot = -1, pid = -1;
    const int ok = r3drv64_start_service64(g_cfg_service, "ext4svc", g_cfg_proto,
                                          disk, lba, &slot, &pid);
    if (!ok) {
        dbg64_line_begin64();
        dbg64_str("[R3DRV] autostart skipped reason=service-missing path=");
        dbg64_str(g_cfg_service);
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    // 看门狗（观察者）：服务起了才起它，免得无谓占进程槽
    if (g_cfg_watchdog[0]) {
        const int wpid = proc64_create64("drvwatch", 0);
        if (wpid > 0) {
            const char* wargv[2];
            wargv[0] = g_cfg_watchdog;
            wargv[1] = nullptr;
            const int wrc = proc64_start_elf64_argv64(wpid, g_cfg_watchdog, wargv, 1);
            dbg64_line_begin64();
            dbg64_str("[R3DRV] watchdog path="); dbg64_str(g_cfg_watchdog);
            dbg64_str(" pid=");                  dbg64_dec((uint64_t)wpid);
            dbg64_str(wrc == 0 ? " ok" : " FAILED reason=elf");
            dbg64_nl();
            dbg64_line_end64();
            if (wrc != 0) {
                (void)proc64_kill64(wpid, 9);
                if (proc64_find64(wpid)) proc64_destroy64(wpid);
            }
        }
    }
    return 1;
}

// ==================== /proc/vimtu/drivers ====================
static int r3_put64(char* t, int n, int cap, const char* s) {
    if (!s) return n;
    while (*s && n + 1 < cap) t[n++] = *s++;
    if (n < cap) t[n] = 0;
    return n;
}
static int r3_putu64(char* t, int n, int cap, unsigned long long v) {
    char tmp[24];
    int k = 0;
    if (v == 0) tmp[k++] = '0';
    while (v > 0 && k < 23) { tmp[k++] = (char)('0' + (v % 10)); v /= 10; }
    while (k > 0 && n + 1 < cap) t[n++] = tmp[--k];
    if (n < cap) t[n] = 0;
    return n;
}

int r3drv64_render64(char* out, uint32_t cap) {
    if (!out || cap < 32) return -1;
    int n = 0;
    out[0] = 0;
    n = r3_put64(out, n, (int)cap, "ver=");
    n = r3_putu64(out, n, (int)cap, (unsigned long long)R3DRV_ABI_VER);
    n = r3_put64(out, n, (int)cap, " slots=");
    n = r3_putu64(out, n, (int)cap, (unsigned long long)r3drv64_count64());
    n = r3_put64(out, n, (int)cap, " restarts=");
    n = r3_putu64(out, n, (int)cap, (unsigned long long)g_restarts);
    n = r3_put64(out, n, (int)cap, " kicks=");
    n = r3_putu64(out, n, (int)cap, g_kicks);
    n = r3_put64(out, n, (int)cap, "\n");
    // 一行一个槽，最多 4 个（LX64_PROC_MAX = 512 B 的合成缓冲，超了会被截断）
    for (uint32_t i = 0; i < R3DRV64_MAX; i++) {
        if (!g_slot[i].used) continue;
        if ((uint32_t)n + 96u >= cap) break;
        const R3drvSlotT& s = g_slot[i];
        n = r3_put64(out, n, (int)cap, "s");
        n = r3_putu64(out, n, (int)cap, i);
        n = r3_put64(out, n, (int)cap, " name=");
        n = r3_put64(out, n, (int)cap, s.name);
        n = r3_put64(out, n, (int)cap, " proto=");
        n = r3_putu64(out, n, (int)cap, s.proto);
        n = r3_put64(out, n, (int)cap, " disk=");
        n = r3_putu64(out, n, (int)cap, s.disk);
        n = r3_put64(out, n, (int)cap, " st=");
        n = r3_putu64(out, n, (int)cap, s.state);
        n = r3_put64(out, n, (int)cap, " pid=");
        n = r3_putu64(out, n, (int)cap, (unsigned long long)(s.pid < 0 ? 0 : s.pid));
        n = r3_put64(out, n, (int)cap, " shm=");
        n = r3_putu64(out, n, (int)cap, (unsigned long long)(s.shm_id < 0 ? 0 : s.shm_id));
        n = r3_put64(out, n, (int)cap, " beats=");
        n = r3_putu64(out, n, (int)cap, s.beats);
        n = r3_put64(out, n, (int)cap, " rst=");
        n = r3_putu64(out, n, (int)cap, s.restarts);
        n = r3_put64(out, n, (int)cap, " dma=");
        n = r3_putu64(out, n, (int)cap, s.dma_bytes);
        n = r3_put64(out, n, (int)cap, " cpy=");
        n = r3_putu64(out, n, (int)cap, s.copy_bytes);
        n = r3_put64(out, n, (int)cap, "\n");
    }
    return n;
}

uint32_t r3drv64_count64() {
    uint32_t c = 0;
    for (uint32_t i = 0; i < R3DRV64_MAX; i++) if (g_slot[i].used) c++;
    return c;
}
uint32_t r3drv64_restarts64() { return g_restarts; }
