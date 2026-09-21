// store64.cpp - VimtuOS 64 位设置持久化 store：双槽 A/B + 世代号 + CRC32（整槽读写）
//
// ★ 载体（carrier）——槽的字节镜像"存在哪里"，与槽格式解耦：
//   1) VFS 文件（**首选，正常运行走这条**）：/store.a、/store.b，各 **16384B = 一个槽**，
//      位于安装程序创建的主分区（VimtuFS2 卷）里；整槽读 = vfs64_read，整槽写 = vfs64_write。
//      store64_init64 先探"卷挂没挂"（vfs64_stat("/")），挂上了就用这个载体。
//   2) 裸盘槽区（**仅降级路径**）：LBA 8009..8072（kernel/memlayout64.h 的 ML64_STORE_*），
//      64 扇区 = 32KB，对半切成 A = 前 32 扇区、B = 后 32 扇区。
//      ★★ 这块区域与安装程序创建的主分区起点（LBA 8009 = kernel/part64.h 的 PART_MAIN_LBA）
//         **完全重叠**：VimtuFS2 的超级块/位图/inode 表/数据块都写在这里。所以只有在
//         "没有任何可用卷"时才用裸盘槽，并且 init 必定打一条醒目 WARN；正常路径绝不碰这些扇区。
//   一次 flush 只走一条路径（VFS 或裸盘），绝不同时写两处。
//
// 槽内格式（全部小端；两种载体**逐字节一致**，改格式 = 同时改 VFS 文件与裸盘扇区）：
//   偏移 0   8B   magic "VSTORE64"
//   偏移 8   u32  结构版本（= 1）
//   偏移 12  u32  标志位（保留，写 0）
//   偏移 16  u64  世代号 generation（单调递增；每次成功 flush +1）
//   偏移 24  u32  payload 字节数（记录区长度，≤ 16320）
//   偏移 28  u32  payload 的 CRC32
//   偏移 32  u32  头部 CRC32：**只覆盖前 32 字节（偏移 0..31）**  <- 二选一里选"头部单独 CRC"
//   偏移 36  u32  KV 记录条数（≤ 64）
//   偏移 40..63  保留（写 0）
//   偏移 64 起   KV 记录串，每条：u8 klen(1..31) + u8 vlen(0..255) + key 字节 + value 字节
//
// 为什么头部 CRC 与 payload CRC 分开（而不是"头部+payload 一起算"）：
//   记录区本来就必须整段校验（payload CRC），头部再单独盯住 magic/版本/世代号/长度这些
//   元数据 —— 分开后"头部坏"与"记录坏"在日志里能区分（reason=header_crc / payload_crc），
//   排障更直接，代价只多 4 字节。两者都校验，不是二选一。
//
// 写入顺序（断电安全的关键）：
//   1) 在内存里构造整槽镜像：先写记录，再填 payload 长度/CRC，最后算头部 CRC
//   2) 提交目标槽（**载体相关，见 st_carrier_write_slot**）：
//      * VFS：vfs64_write 自身就是"先分配新块 -> 写完数据 -> 最后提交 inode"（vfs64.cpp:804），
//        中途断电只会泄漏新块，旧文件（旧世代号/旧 CRC）仍然可读；
//      * 裸盘：先 PIO 写数据扇区（1..31），最后写扇区 0（头部）——这一步成功才算提交。
//      两条路的提交点都带 CRC：断电只会让目标槽下次 init 判无效 -> 回退到另一槽（旧设置完好）。
//   3) 内存状态切到目标槽、generation+1，打印
//      [STORE64] flush via=vfs -> slot=B gen=<n> crc=0x<hex> ok
//
// ==================== ATA 弱链接约定（重要）====================
// 64 位树里有两个内核：
//   * 安装介质内核（build64.sh 的 SRCS_INSTALLER）**链接 kernel/ata64.cpp**（但**不编本文件**）
//   * 系统内核（build64.sh 的 SRCS_OS）链接 kernel/ata64.cpp + kernel/vfs64.cpp + 本文件
// 本文件对 ATA 用 __attribute__((weak)) 弱引用：**只有裸盘降级路径**用它，ATA 没链接进来时
// st_ata_linked() 为 false，裸盘路径直接判失败，绝不调用空指针；VFS 载体的磁盘 I/O 由
// vfs64.cpp 自己负责（它也有自己的弱引用），与本文件的弱引用无关。
// ★ 签名必须与 kernel/ata64.h **逐字一致**：ata64.h 没写 extern "C"，符号是 C++ 名字
//   （_Z10ata64_readijjPv / _Z11ata64_writeijjPKv）。这里 include ata64.h 之后再用
//   __attribute__((weak)) 重声明，保证与 ata64.cpp 的定义落在同一个符号上。
//   将来若给 ata64.h 加 extern "C"，必须同步改这里的声明，否则弱引用会"悄悄失配"
//   （表现为永远走"未链接"分支，但不报错、不崩）。
// ==============================================================

#include "store64.h"
#include "memlayout64.h"
#include "debug64.h"
#include "ata64.h"
#include "vfs64.h"     // 首选载体：VimtuFS2 的 /store.a、/store.b（vfs64_stat / vfs64_read / vfs64_write）

__attribute__((weak)) bool ata64_read (int drive, uint32_t lba, uint32_t count, void* buf);
__attribute__((weak)) bool ata64_write(int drive, uint32_t lba, uint32_t count, const void* buf);

// ==================== 布局常量 ====================
static const char     ST_MAGIC[8]     = { 'V','S','T','O','R','E','6','4' };
static const uint32_t ST_FORMAT_VER   = 1;                                        // 结构版本
static const uint32_t ST_HEADER_BYTES = 64;                                       // 头部固定 64B
static const uint32_t ST_SLOT_BYTES   = (ML64_STORE_SECTORS / 2u) * (uint32_t)ML64_SECTOR_BYTES; // 16384
static const uint32_t ST_SLOT_SECTORS = ST_SLOT_BYTES / (uint32_t)ML64_SECTOR_BYTES;             // 32
static const uint32_t ST_PAYLOAD_MAX  = ST_SLOT_BYTES - ST_HEADER_BYTES;          // 16320

// 载体（carrier）：槽的字节镜像"存在哪里"。**首选 VFS 文件**（VimtuFS2 卷里的 /store.a、/store.b，
// 各 = 一个槽 = 16384B）；裸盘槽区只做"没有可用卷"时的降级，并且会打 WARN（与数据分区重叠）。
static const char ST_VFS_A[]   = "/store.a";
static const char ST_VFS_B[]   = "/store.b";
static const char ST_VFS_TMP[] = "/st64.tmp";   // 自检临时文件（写完即删，绝不碰上面两个真槽）
enum { ST_CARRIER_NONE = 0, ST_CARRIER_RAW = 1, ST_CARRIER_VFS = 2 };
static_assert(ST_SLOT_BYTES == 16384u, "一个槽 = 16KB：VFS 载体上就是 /store.a|b 的文件大小");

// 头部字段偏移（小端）
static const uint32_t ST_O_MAGIC = 0;     // 8B  magic "VSTORE64"
static const uint32_t ST_O_VER   = 8;     // u32 结构版本
static const uint32_t ST_O_FLAGS = 12;    // u32 标志位（保留，0）
static const uint32_t ST_O_GEN   = 16;    // u64 世代号
static const uint32_t ST_O_PLEN  = 24;    // u32 payload 字节数
static const uint32_t ST_O_PCRC  = 28;    // u32 payload CRC32
static const uint32_t ST_O_HCRC  = 32;    // u32 头部 CRC32（只覆盖 [0,32)）
static const uint32_t ST_O_COUNT = 36;    // u32 KV 记录条数

static_assert(ML64_STORE_SECTORS % 2u == 0u, "store 保留区扇区数必须是偶数才能对半切 A/B");
static_assert(ST_HEADER_BYTES <= (uint32_t)ML64_SECTOR_BYTES,
              "头部必须整个落在扇区 0 内（提交点 = 只写扇区 0）");

// ==================== 内存状态（全 .bss 静态，无 new/delete/malloc）====================
struct KV64 {
    uint16_t klen;
    uint16_t vlen;
    char     key[STORE64_MAX_KEY_LEN + 1];    // 含 '\0'，便于 C 字符串比较
    char     val[STORE64_MAX_VAL_LEN + 1];
};

struct KVTable64 {
    KV64 kv[STORE64_MAX_KEYS];
    int  count;
};

static KVTable64 g_tab;                           // 当前（内存）KV 表
static KVTable64 g_st_tab;                        // 自检专用表（绝不碰 g_tab）
static uint8_t   g_slot_buf[2][ST_SLOT_BYTES];    // 两槽整槽缓冲：整槽 PIO 读入 / 构造后整槽写出
static int       g_active_slot = -1;              // -1 = 无（两槽都无效）
static uint64_t  g_gen         = 0;               // 当前活动槽的世代号
static int       g_drive       = -1;              // init 传入的 ATA 驱动器号

// 载体状态：槽存在哪里（init 时按"有没有卷"决定），以及降级警告是否已经打过
static int       g_carrier    = ST_CARRIER_NONE;
static bool      g_raw_warned = false;
// 惰性升级（init 早于 VFS 挂载）时暂存内存表：flush 前把用户的改动搬到 VFS 载体上，不丢键
static KVTable64 g_pending;

// ==================== 小工具（不依赖 libc）====================
static void st_zero(uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) p[i] = 0;
}
static void st_copy(void* dst, const void* src, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}
static int st_cmp(const void* a, const void* b, uint32_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (uint32_t i = 0; i < n; i++) if (x[i] != y[i]) return (x[i] < y[i]) ? -1 : 1;
    return 0;
}
static uint32_t st_len(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}
static int st_scmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t* p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
static void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static void wr64(uint8_t* p, uint64_t v) {
    wr32(p, (uint32_t)(v & 0xFFFFFFFFu));
    wr32(p + 4, (uint32_t)(v >> 32));
}

// 标准 CRC-32（反射多项式 0xEDB88320，初值/末异或都是 0xFFFFFFFF），与 zlib/PNG/zip 一致：
//   crc32("123456789") == 0xCBF43926（自检里写死断言，防实现漂移）
static uint32_t crc32_64(const uint8_t* p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}

// ==================== key / value 合法性 ====================
// key：1..31 字节可打印 ASCII（0x20..0x7E），不含 '='（dump 行是 key=value，'=' 会混淆）
static bool st_key_ok(const char* k) {
    if (!k) return false;
    const uint32_t n = st_len(k);
    if (n == 0 || n > STORE64_MAX_KEY_LEN) return false;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t c = (uint8_t)k[i];
        if (c < 0x20 || c > 0x7E || c == '=') return false;
    }
    return true;
}

// 严格 UTF-8：拒绝过长编码 / 代理区 / 超出 U+10FFFF / 截断序列
static bool st_utf8_ok(const char* s, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        const uint8_t c = (uint8_t)s[i];
        if (c < 0x80) { i++; continue; }
        uint32_t extra;
        uint32_t cp;
        if      ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1Fu; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07u; }
        else return false;                                   // 0x80..0xBF 起头 / 0xF8+ 非法
        if (i + extra >= n) return false;                    // 序列被截断
        for (uint32_t j = 1; j <= extra; j++) {
            const uint8_t cc = (uint8_t)s[i + j];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (uint32_t)(cc & 0x3F);
        }
        if (extra == 1 && cp < 0x80u)     return false;      // 过长编码
        if (extra == 2 && cp < 0x800u)    return false;
        if (extra == 3 && cp < 0x10000u)  return false;
        if (cp > 0x10FFFFu)               return false;
        if (cp >= 0xD800u && cp <= 0xDFFFu) return false;    // UTF-16 代理区
        i += extra + 1;
    }
    return true;
}

// value：0..255 字节合法 UTF-8，且不含 CR/LF（串口日志/dump 按行组织，换行会撕断行）
static bool st_val_ok(const char* v) {
    if (!v) return false;
    const uint32_t n = st_len(v);
    if (n > STORE64_MAX_VAL_LEN) return false;
    for (uint32_t i = 0; i < n; i++) if (v[i] == '\r' || v[i] == '\n') return false;
    return st_utf8_ok(v, n);
}

// ==================== 内存 KV 表操作 ====================
static int tab_find(const KVTable64* t, const char* key) {
    for (int i = 0; i < t->count; i++) if (st_scmp(t->kv[i].key, key) == 0) return i;
    return -1;
}

// 序列化后记录区总字节数（u8 klen + u8 vlen + key + value）
static uint32_t tab_payload(const KVTable64* t) {
    uint32_t n = 0;
    for (int i = 0; i < t->count; i++) n += 2u + (uint32_t)t->kv[i].klen + (uint32_t)t->kv[i].vlen;
    return n;
}

// 插入/覆盖；0 = 成功，-1 = key/value 非法 / 超过 64 条 / 槽容量放不下
static int tab_set(KVTable64* t, const char* key, const char* value) {
    if (!st_key_ok(key) || !st_val_ok(value)) return -1;
    const uint32_t kl = st_len(key);
    const uint32_t vl = st_len(value);
    const uint32_t need = 2u + kl + vl;
    int idx = tab_find(t, key);
    uint32_t total = tab_payload(t);
    if (idx < 0) {
        if (t->count >= STORE64_MAX_KEYS) return -1;                        // 超过 64 条
        idx = t->count;
        total += need;
    } else {
        const uint32_t old = 2u + (uint32_t)t->kv[idx].klen + (uint32_t)t->kv[idx].vlen;
        total = total - old + need;
    }
    if (total > ST_PAYLOAD_MAX) return -1;                                  // 槽放不下
    KV64* e = &t->kv[idx];
    st_copy(e->key, key, kl + 1);
    st_copy(e->val, value, vl + 1);
    e->klen = (uint16_t)kl;
    e->vlen = (uint16_t)vl;
    if (idx == t->count) t->count++;
    return 0;
}

// 删除；0 = 删掉，-1 = 键不存在（后面的记录整体前移，保持无空洞）
static int tab_remove(KVTable64* t, const char* key) {
    const int idx = tab_find(t, key);
    if (idx < 0) return -1;
    for (int i = idx; i + 1 < t->count; i++) t->kv[i] = t->kv[i + 1];
    t->count--;
    return 0;
}

// ==================== 槽镜像：构造 / 校验 / 解析 ====================
static uint32_t slot_lba(int s) {
    return (uint32_t)ML64_STORE_LBA + ((s == 1) ? ST_SLOT_SECTORS : 0u);
}

// 把内存表构造成一整个槽镜像（只动内存，不碰磁盘）。
// 成功返回 true 并回传 payload 长度与 CRC；失败 = 记录区放不下。
static bool slot_build(const KVTable64* t, uint64_t gen, uint8_t* buf,
                       uint32_t* out_plen, uint32_t* out_pcrc) {
    if (!buf || t->count < 0 || t->count > STORE64_MAX_KEYS) return false;
    st_zero(buf, ST_SLOT_BYTES);
    st_copy(buf + ST_O_MAGIC, ST_MAGIC, 8);
    wr32(buf + ST_O_VER, ST_FORMAT_VER);
    wr32(buf + ST_O_FLAGS, 0);
    wr64(buf + ST_O_GEN, gen);
    wr32(buf + ST_O_COUNT, (uint32_t)t->count);
    uint32_t off = ST_HEADER_BYTES;
    for (int i = 0; i < t->count; i++) {
        const KV64* e = &t->kv[i];
        if (off + 2u + (uint32_t)e->klen + (uint32_t)e->vlen > ST_SLOT_BYTES) return false;
        buf[off++] = (uint8_t)e->klen;
        buf[off++] = (uint8_t)e->vlen;
        st_copy(buf + off, e->key, e->klen);
        off += e->klen;
        st_copy(buf + off, e->val, e->vlen);
        off += e->vlen;
    }
    const uint32_t plen = off - ST_HEADER_BYTES;
    const uint32_t pcrc = crc32_64(buf + ST_HEADER_BYTES, plen);
    wr32(buf + ST_O_PLEN, plen);
    wr32(buf + ST_O_PCRC, pcrc);
    wr32(buf + ST_O_HCRC, crc32_64(buf, ST_O_HCRC));    // 头部 CRC 覆盖 [0,32)
    if (out_plen) *out_plen = plen;
    if (out_pcrc) *out_pcrc = pcrc;
    return true;
}

// 校验整槽镜像：0 = 有效；1=magic 2=版本 3=记录条数 4=payload 长度 5=头部CRC 6=payload CRC
static int slot_validate(const uint8_t* buf, uint64_t* out_gen, uint32_t* out_plen) {
    if (!buf) return 1;
    if (st_cmp(buf + ST_O_MAGIC, ST_MAGIC, 8) != 0) return 1;
    if (rd32(buf + ST_O_VER) != ST_FORMAT_VER) return 2;
    const uint32_t count = rd32(buf + ST_O_COUNT);
    if (count > STORE64_MAX_KEYS) return 3;
    const uint32_t plen = rd32(buf + ST_O_PLEN);
    if (plen > ST_PAYLOAD_MAX) return 4;
    if (rd32(buf + ST_O_HCRC) != crc32_64(buf, ST_O_HCRC)) return 5;
    if (rd32(buf + ST_O_PCRC) != crc32_64(buf + ST_HEADER_BYTES, plen)) return 6;
    if (out_gen)  *out_gen  = rd64(buf + ST_O_GEN);
    if (out_plen) *out_plen = plen;
    return 0;
}

static const char* slot_reason(int r) {
    switch (r) {
        case 0:  return "ok";
        case 1:  return "magic";
        case 2:  return "version";
        case 3:  return "count";
        case 4:  return "payload_len";
        case 5:  return "header_crc";
        case 6:  return "payload_crc";
        default: return "parse";
    }
}

// 把记录区解析进内存表（由 slot_validate 先放行；这里仍逐字节做边界检查）。
// 记录条数必须与头部一致；同键后者覆盖前者（本模块自己写出的槽不会有重复键）。
static bool slot_parse(const uint8_t* buf, uint32_t plen, KVTable64* t) {
    if (!buf || !t) return false;
    t->count = 0;
    uint32_t off = ST_HEADER_BYTES;
    const uint32_t end = ST_HEADER_BYTES + plen;
    while (off < end) {
        if (off + 2u > end) return false;
        const uint32_t kl = buf[off];
        const uint32_t vl = buf[off + 1];
        off += 2;
        if (kl == 0 || kl > STORE64_MAX_KEY_LEN) return false;
        if (off + kl + vl > end) return false;
        char kbuf[STORE64_MAX_KEY_LEN + 1];
        for (uint32_t i = 0; i < kl; i++) kbuf[i] = (char)buf[off + i];
        kbuf[kl] = 0;
        int idx = tab_find(t, kbuf);
        if (idx < 0) {
            if (t->count >= STORE64_MAX_KEYS) return false;
            idx = t->count;
        }
        KV64* e = &t->kv[idx];
        st_copy(e->key, kbuf, kl + 1);
        e->klen = (uint16_t)kl;
        off += kl;
        for (uint32_t i = 0; i < vl; i++) e->val[i] = (char)buf[off + i];
        e->val[vl] = 0;
        e->vlen = (uint16_t)vl;
        off += vl;
        if (idx == t->count) t->count++;
    }
    if (off != end) return false;
    return (uint32_t)t->count == rd32(buf + ST_O_COUNT);
}

// ==================== 载体层：VFS 文件（首选） / 裸盘槽区（仅降级）====================
static bool st_ata_linked() {
    return (ata64_read != nullptr) && (ata64_write != nullptr);
}
static const char* st_slot_name(int s) {
    if (s == 0) return "A";
    if (s == 1) return "B";
    return "none";
}
static const char* st_carrier_name(int c) {
    if (c == ST_CARRIER_VFS) return "vfs";
    if (c == ST_CARRIER_RAW) return "raw";
    return "none";
}
static const char* st_slot_path(int s) {
    return (s == 1) ? ST_VFS_B : ST_VFS_A;
}
// 载体尺寸契约：一个槽在两种载体上都是 ST_SLOT_BYTES（VFS 文件大小 / 裸盘 32 扇区）——
// 尺寸不符一律当无效槽（例如别人往 /store.a 里塞了别的东西）。
static bool st_slot_size_ok(uint32_t bytes) {
    return bytes == ST_SLOT_BYTES;
}
// VFS 可用性探测：**系统卷**挂载着时 vfs64_stat_on64(系统卷槽, "/") 成功（与 kernel/app64.cpp
// 用同一条判断："根目录 stat 成功 = VimtuFS2 卷就在那儿"）。没挂载时 on64 自己会打一行 not mounted。
// ★ 多卷（为什么不会写错卷）：store64 的 /store.a、/store.b 是**系统卷上的文件**，所以这里以及下面
//   每一个读写都用 vfs64_*_on64(vfs64_system_slot64(), …)：只在这一次调用期间把"当前卷"临时切成
//   系统卷槽，函数返回前原样切回（vfs64.cpp 的 Vfs64SlotGuard，LIFO）。用户正在浏览 D: 时，
//   3 秒自动落盘/flush 仍然只写 C:（系统卷），D: 的位图/inode 一个字节都不会被碰到。
static bool st_vfs_available() {
    uint32_t type = 0;
    uint32_t size = 0;
    return vfs64_stat_on64(vfs64_system_slot64(), "/", &type, &size) == 0;
}
// 裸盘槽区的重叠警告（只打一次）：这块区域就是安装程序创建的数据分区本身。
static void st_warn_raw_overlap() {
    if (g_raw_warned) return;
    g_raw_warned = true;
    dbg64_line_begin64();
    dbg64_str("[STORE64] WARN raw slot area LBA ");
    dbg64_dec((uint64_t)ML64_STORE_LBA);
    dbg64_str(" overlaps the data partition; use VFS-backed store");
    dbg64_nl();
    dbg64_line_end64();
}
// 读一个槽（整槽进 buf）。0 = 拿到完整一槽；非 0 = 无效，*why 给出原因（日志里区分 absent/size/read）
static int st_carrier_read_slot(int s, uint8_t* buf, const char** why) {
    if (g_carrier == ST_CARRIER_VFS) {
        uint32_t type = 0;
        uint32_t size = 0;
        const int sys = vfs64_system_slot64();                     // ★ 固定系统卷
        if (vfs64_stat_on64(sys, st_slot_path(s), &type, &size) != 0) { *why = "absent";   return 1; }  // 文件不存在 = 无效槽
        if (type != VFS64_TYPE_FILE)                        { *why = "not_file"; return 1; }
        if (!st_slot_size_ok(size))                         { *why = "size";     return 1; }
        const int n = vfs64_read_on64(sys, st_slot_path(s), buf, (int)ST_SLOT_BYTES);
        if (n != (int)ST_SLOT_BYTES)                        { *why = "read";     return 1; }
        return 0;
    }
    if (g_carrier == ST_CARRIER_RAW) {
        if (!st_ata_linked()) { *why = "ata not linked"; return 1; }
        if (g_drive < 0)      { *why = "no drive";       return 1; }
        if (!ata64_read(g_drive, slot_lba(s), ST_SLOT_SECTORS, buf)) { *why = "read"; return 1; }
        return 0;
    }
    *why = "no carrier";
    return 1;
}
// 提交一个槽（整槽）。断电安全：VFS 靠 vfs64_write 的"最后提交 inode"，裸盘靠
// "先写数据扇区（1..31），最后写头部扇区（0，带 CRC）"。失败返回 false 且旧槽不受影响。
static bool st_carrier_write_slot(int s, const uint8_t* buf, const char** why) {
    if (g_carrier == ST_CARRIER_VFS) {
        // ★ 固定系统卷：只写 C: 上的 /store.<x>（见 st_vfs_available 上方说明）
        const int n = vfs64_write_on64(vfs64_system_slot64(), st_slot_path(s), buf, (int)ST_SLOT_BYTES);
        if (n != (int)ST_SLOT_BYTES) { *why = "vfs64_write"; return false; }
        return true;
    }
    if (g_carrier == ST_CARRIER_RAW) {
        // 1) 先写数据扇区（1..31）。payload ≤ 448B（多数设置场景）时它整个落在扇区 0 里，
        //    这一步只动"记录区在扇区 1 之后的部分"；旧槽要么完好、要么被 CRC 检出无效
        if (!ata64_write(g_drive, slot_lba(s) + 1u, ST_SLOT_SECTORS - 1u,
                         buf + ML64_SECTOR_BYTES)) {
            *why = "write data";
            return false;
        }
        // 2) 最后写头部扇区（0）——带 CRC，这一步成功才算提交
        if (!ata64_write(g_drive, slot_lba(s), 1u, buf)) {
            *why = "write header";
            return false;
        }
        return true;
    }
    *why = "no carrier";
    return false;
}
// 从当前载体读两槽 -> 校验 -> 选"有效且世代号更大"的活动槽 -> 解析进 g_tab。
// 载体差异（VFS 文件 / 裸盘扇区）全部藏在 st_carrier_read_slot 里，这里是纯格式逻辑。
static void st_load_slots() {
    g_active_slot = -1;
    g_gen = 0;
    g_tab.count = 0;
    uint64_t gen[2] = { 0, 0 };
    int  rc[2] = { -1, -1 };                     // -1 = 读失败；否则 slot_validate 原因码
    bool rd[2] = { false, false };
    for (int s = 0; s < 2; s++) {
        const char* why = "?";
        rd[s] = (st_carrier_read_slot(s, g_slot_buf[s], &why) == 0);
        if (rd[s]) rc[s] = slot_validate(g_slot_buf[s], &gen[s], nullptr);
        dbg64_line_begin64();
        dbg64_str("[STORE64] slot=");
        dbg64_str(st_slot_name(s));
        if (!rd[s]) {
            dbg64_str(" invalid reason=");
            dbg64_str(why);
        } else if (rc[s] != 0) {
            dbg64_str(" invalid reason=");
            dbg64_str(slot_reason(rc[s]));
        } else {
            dbg64_str(" ok gen=");
            dbg64_dec(gen[s]);
        }
        dbg64_nl();
        dbg64_line_end64();
    }

    // 两槽都有效 -> 选世代号更大的；只有一个有效 -> 用它；否则回退
    int pick = -1;
    int alt  = -1;
    if (rc[0] == 0 && rc[1] == 0) { pick = (gen[0] >= gen[1]) ? 0 : 1; alt = 1 - pick; }
    else if (rc[0] == 0) pick = 0;
    else if (rc[1] == 0) pick = 1;

    if (pick >= 0) {
        const uint32_t plen = rd32(g_slot_buf[pick] + ST_O_PLEN);
        if (slot_parse(g_slot_buf[pick], plen, &g_tab)) {
            g_active_slot = pick;
            g_gen = gen[pick];
        } else {
            dbg64_line_begin64();
            dbg64_str("[STORE64] slot=");
            dbg64_str(st_slot_name(pick));
            dbg64_str(" invalid reason=parse");
            dbg64_nl();
            dbg64_line_end64();
            if (alt >= 0) {
                const uint32_t plen2 = rd32(g_slot_buf[alt] + ST_O_PLEN);
                if (slot_parse(g_slot_buf[alt], plen2, &g_tab)) {
                    g_active_slot = alt;
                    g_gen = gen[alt];
                } else {
                    dbg64_line_begin64();
                    dbg64_str("[STORE64] slot=");
                    dbg64_str(st_slot_name(alt));
                    dbg64_str(" invalid reason=parse");
                    dbg64_nl();
                    dbg64_line_end64();
                }
            }
        }
    }

    // 两槽都无效（含读到失败/CRC 失败）-> 空 store，全默认值
    if (g_active_slot < 0) {
        g_gen = 0;
        g_tab.count = 0;
        dbg64_line_begin64();
        dbg64_str("[STORE64] both slots invalid -> defaults");
        dbg64_nl();
        dbg64_line_end64();
    }
}
static void st_log_init_line(const char* via) {
    dbg64_line_begin64();
    dbg64_str("[STORE64] init via=");
    dbg64_str(via);
    dbg64_str(" slot=");
    dbg64_str(st_slot_name(g_active_slot));
    dbg64_str(" gen=");
    dbg64_dec(g_gen);
    dbg64_str(" keys=");
    dbg64_dec((uint64_t)g_tab.count);
    dbg64_nl();
    dbg64_line_end64();
}
// 表拷贝（显式逐条拷：大结构体赋值在 -mno-sse 下会退化成 memcpy 调用，这里不想依赖它）
static void tab_copy(KVTable64* dst, const KVTable64* src) {
    dst->count = src->count;
    for (int i = 0; i < src->count; i++) {
        dst->kv[i].klen = src->kv[i].klen;
        dst->kv[i].vlen = src->kv[i].vlen;
        st_copy(dst->kv[i].key, src->kv[i].key, (uint32_t)src->kv[i].klen + 1u);
        st_copy(dst->kv[i].val, src->kv[i].val, (uint32_t)src->kv[i].vlen + 1u);
    }
}
// 惰性升级：init 时卷还没挂上（载体 = 裸盘），flush 时卷已可用 —— 把内存里的改动搬到 VFS
// 载体上：1) 暂存内存表 2) 从 VFS 读回两槽（继承盘上的世代号/活动槽）3) 暂存的键逐条覆盖回去
// （同 key 时内存永远比盘新）。之后本次 flush 只写 VFS —— 一次 flush 依然只有一条写路径。
static void st_upgrade_carrier_to_vfs() {
    if (g_carrier == ST_CARRIER_VFS) return;
    tab_copy(&g_pending, &g_tab);
    g_carrier = ST_CARRIER_VFS;
    st_load_slots();
    for (int i = 0; i < g_pending.count; i++)
        (void)tab_set(&g_tab, g_pending.kv[i].key, g_pending.kv[i].val);
    g_pending.count = 0;
    dbg64_line_begin64();
    dbg64_str("[STORE64] carrier upgrade via=vfs (vfs mounted after init) slot=");
    dbg64_str(st_slot_name(g_active_slot));
    dbg64_str(" gen=");
    dbg64_dec(g_gen);
    dbg64_str(" keys=");
    dbg64_dec((uint64_t)g_tab.count);
    dbg64_nl();
    dbg64_line_end64();
}
// 8 位大写十六进制（CRC32 打印用，与 dbg64_hex64 的大写风格一致）
static void st_hex8(uint32_t v) {
    static const char* H = "0123456789ABCDEF";
    dbg64_str("0x");
    for (int i = 7; i >= 0; i--) dbg64_putc(H[(v >> (i * 4)) & 0xF]);
}
static int st_flush_fail(const char* via, const char* why) {
    dbg64_line_begin64();
    dbg64_str("[STORE64] flush FAILED via=");
    dbg64_str(via ? via : "?");
    dbg64_str(" reason=");
    dbg64_str(why);
    dbg64_nl();
    dbg64_line_end64();
    return -1;
}

// ==================== 对外 API ====================
// 初始化：先探测"有没有卷"，据此选载体，再从载体读两槽、选**有效且世代号更大**的活动槽。
//   载体 1) VFS（首选）：根目录能 stat 到 = VimtuFS2 卷已挂载 -> 槽就是文件 /store.a、/store.b
//   载体 2) 裸盘槽区（仅降级）：没有可用的卷时才用；它与数据分区重叠，所以会打 WARN
void store64_init64(int ata_drive) {
    g_drive = ata_drive;
    g_active_slot = -1;
    g_gen = 0;
    g_tab.count = 0;
    g_carrier = ST_CARRIER_NONE;

    // 首选：VFS 已挂载（卷可用）-> 设置落在文件系统里的 /store.a、/store.b
    if (st_vfs_available()) {
        g_carrier = ST_CARRIER_VFS;
        st_load_slots();
        st_log_init_line("vfs");
        return;
    }

    // 降级：没有可用的卷 -> 裸盘槽区（LBA 8009..8072 与数据分区**完全重叠**，必须大声报警）
    if (!st_ata_linked()) {
        // 与旧版一致：两种载体都不可用 -> 空 store（全默认值），绝不调用空指针
        dbg64_line_begin64();
        dbg64_str("[STORE64] no vfs volume and ata not linked -> weak refs are null, store is memory-only");
        dbg64_nl();
        dbg64_line_end64();
        dbg64_line_begin64();
        dbg64_str("[STORE64] both slots invalid -> defaults");
        dbg64_nl();
        dbg64_line_end64();
        st_log_init_line("none");
        return;
    }
    g_carrier = ST_CARRIER_RAW;
    st_warn_raw_overlap();
    st_load_slots();
    st_log_init_line("raw");
}

int store64_get64(const char* key, char* out, int out_max) {
    if (!key || !out || out_max <= 0) return -1;
    const int idx = tab_find(&g_tab, key);
    if (idx < 0) return -1;
    const KV64* e = &g_tab.kv[idx];
    const int n = (int)e->vlen;                 // 返回完整长度，即使被截断
    int c = (n < out_max - 1) ? n : (out_max - 1);
    for (int i = 0; i < c; i++) out[i] = e->val[i];
    out[c] = 0;
    return n;
}

int store64_set64(const char* key, const char* value) {
    if (tab_set(&g_tab, key, value) != 0) {
        // 失败行故意用 "set64" 前缀：自动验收统计 "[STORE64] set " 时不会把失败计入成功数
        dbg64_line_begin64();
        dbg64_str("[STORE64] set64 rejected key=");
        dbg64_str(key ? key : "(null)");
        dbg64_str(" reason=");
        if (!st_key_ok(key))                       dbg64_str("bad key");
        else if (!st_val_ok(value))                dbg64_str("bad value");
        else if (tab_find(&g_tab, key) < 0 && g_tab.count >= STORE64_MAX_KEYS) dbg64_str("too many keys");
        else                                        dbg64_str("payload full");
        dbg64_nl();
        dbg64_line_end64();
        return -1;
    }
    dbg64_line_begin64();
    dbg64_str("[STORE64] set ");
    dbg64_str(key);
    dbg64_putc('=');
    dbg64_str(value);
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

// 落盘：**一次 flush 只走一条路径** —— 有卷走 VFS（/store.<other>），没有卷才走裸盘槽区。
// 成功 = 目标槽提交完成 -> 切活动槽 + generation+1；失败 = 不切槽（旧槽完好）。
int store64_flush64() {
    // init 时还没有卷（走了裸盘），flush 时卷已可用 -> 先把内存里的改动搬到 VFS 载体上
    if (g_carrier != ST_CARRIER_VFS && st_vfs_available()) st_upgrade_carrier_to_vfs();

    // 永远写非活动槽；还没有活动槽（两槽都无效）时先写 A
    const int tgt = (g_active_slot == 0) ? 1 : 0;
    uint32_t plen = 0;
    uint32_t pcrc = 0;
    if (!slot_build(&g_tab, g_gen + 1, g_slot_buf[tgt], &plen, &pcrc))
        return st_flush_fail(st_carrier_name(g_carrier), "payload too large");

    if (g_carrier == ST_CARRIER_VFS) {
        // VFS 载体：整槽（16KB）写进 /store.<other>。vfs64_write 自己就是"分配新块 -> 写数据 ->
        // 最后提交 inode"，中途断电旧文件（旧世代号/旧 CRC）仍完好；失败**不**切槽。
        const char* why = "?";
        if (!st_carrier_write_slot(tgt, g_slot_buf[tgt], &why)) return st_flush_fail("vfs", why);
        g_active_slot = tgt;
        g_gen++;
        dbg64_line_begin64();
        dbg64_str("[STORE64] flush via=vfs -> slot=");
        dbg64_str(st_slot_name(g_active_slot));
        dbg64_str(" gen=");
        dbg64_dec(g_gen);
        dbg64_str(" crc=");
        st_hex8(pcrc);
        dbg64_str(" ok");
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }

    // ---- 裸盘降级路径（没有可用卷）：断电安全语义与旧版一致 ----
    if (!st_ata_linked()) return st_flush_fail("raw", "ata not linked");
    if (g_drive < 0)      return st_flush_fail("raw", "no drive (init not called)");
    g_carrier = ST_CARRIER_RAW;
    st_warn_raw_overlap();
    const char* why = "?";
    if (!st_carrier_write_slot(tgt, g_slot_buf[tgt], &why)) return st_flush_fail("raw", why);
    g_active_slot = tgt;
    g_gen++;
    dbg64_line_begin64();
    dbg64_str("[STORE64] flush via=raw -> slot=");
    dbg64_str(st_slot_name(g_active_slot));
    dbg64_str(" gen=");
    dbg64_dec(g_gen);
    dbg64_str(" crc=");
    st_hex8(pcrc);
    dbg64_str(" ok (raw fallback: this area overlaps the data partition)");
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

void store64_dump64() {
    dbg64_line_begin64();
    dbg64_str("[STORE64] kv count=");
    dbg64_dec((uint64_t)g_tab.count);
    dbg64_str(" gen=");
    dbg64_dec(g_gen);
    dbg64_str(" slot=");
    dbg64_str(st_slot_name(g_active_slot));
    dbg64_nl();
    dbg64_line_end64();
    for (int i = 0; i < g_tab.count; i++) {
        dbg64_line_begin64();
        dbg64_str("[STORE64] dump ");
        dbg64_str(g_tab.kv[i].key);
        dbg64_putc('=');
        dbg64_str(g_tab.kv[i].val);
        dbg64_nl();
        dbg64_line_end64();
    }
}

uint64_t store64_generation64() {
    return g_gen;
}

// ==================== 只读快照（终端 store 命令用；不改任何状态）====================
int store64_key_count64() {
    return g_tab.count;
}
const char* store64_slot_name64() {
    return st_slot_name(g_active_slot);
}
const char* store64_carrier64() {
    return st_carrier_name(g_carrier);
}
// 第 i 条记录（0 起）：key/value 拷进调用方缓冲（都 NUL 结尾）。返回 0 = 有值，-1 = 越界/参数错。
int store64_entry64(int i, char* key_out, int key_max, char* val_out, int val_max) {
    if (i < 0 || i >= g_tab.count) return -1;
    if (!key_out || !val_out || key_max <= 0 || val_max <= 0) return -1;
    const KV64* e = &g_tab.kv[i];
    int kc = (int)e->klen;
    if (kc > key_max - 1) kc = key_max - 1;
    int vc = (int)e->vlen;
    if (vc > val_max - 1) vc = val_max - 1;
    for (int j = 0; j < kc; j++) key_out[j] = e->key[j];
    key_out[kc] = 0;
    for (int j = 0; j < vc; j++) val_out[j] = e->val[j];
    val_out[vc] = 0;
    return 0;
}

// ==================== 自检（内存为主，真盘部分可跳过）====================
// 位掩码见 store64.h；返回 0 = 全过。
int store64_selftest64() {
    int fails = 0;
    KVTable64* t = &g_st_tab;                  // 自检一律用 g_st_tab，不动 g_tab（应用数据）
    t->count = 0;

    // bit0: CRC32 已知向量（"123456789" 是 CRC-32 的标准测试串）
    if (crc32_64((const uint8_t*)"123456789", 9) != 0xCBF43926u) fails |= 1;
    if (crc32_64((const uint8_t*)"", 0)          != 0x00000000u) fails |= 1;
    if (crc32_64((const uint8_t*)"a", 1)         != 0xE8B7BE43u) fails |= 1;

    // bit1: 插入 + 查找（含缺失键）
    if (tab_set(t, "lang", "zh")  != 0) fails |= 2;
    if (tab_set(t, "zoom", "125") != 0) fails |= 2;
    {
        const int i = tab_find(t, "lang");
        if (i < 0 || st_scmp(t->kv[i].val, "zh") != 0) fails |= 2;
    }
    if (t->count != 2 || tab_find(t, "nope") != -1) fails |= 2;

    // bit2: 覆盖（长度变化，条数不变）
    if (tab_set(t, "lang", "english") != 0) fails |= 4;
    {
        const int i = tab_find(t, "lang");
        if (i < 0 || t->kv[i].vlen != 7 || st_scmp(t->kv[i].val, "english") != 0) fails |= 4;
    }
    if (t->count != 2) fails |= 4;
    if (tab_set(t, "lang", "zh") != 0) fails |= 4;

    // bit3: 删除（后面的记录前移，其它键不受影响）
    if (tab_set(t, "tmp", "1") != 0) fails |= 8;
    if (tab_remove(t, "tmp") != 0) fails |= 8;
    if (tab_find(t, "tmp") != -1 || t->count != 2) fails |= 8;
    if (tab_find(t, "zoom") != 1) fails |= 8;

    // bit4: 非法输入必须被拒（空 key / 32B key / 含 '=' 的 key / 256B value / 坏 UTF-8）
    {
        char k32[40];
        for (int j = 0; j < 32; j++) k32[j] = 'k';
        k32[32] = 0;
        char v256[300];
        for (int j = 0; j < 256; j++) v256[j] = 'v';
        v256[256] = 0;
        if (tab_set(t, "", "x") == 0)            fails |= 16;
        if (tab_set(t, k32, "x") == 0)           fails |= 16;
        if (tab_set(t, "a=b", "x") == 0)         fails |= 16;
        if (tab_set(t, "big", v256) == 0)        fails |= 16;
        if (tab_set(t, "bad", "\xC3\x28") == 0)  fails |= 16;   // 非法 UTF-8 续字节
        if (t->count != 2)                       fails |= 16;
    }

    // bit5: 超过 64 条被拒（第 64 条成功，第 65 条必须失败）
    t->count = 0;
    {
        int added = 0;
        char kn[8];
        for (int j = 0; j < STORE64_MAX_KEYS; j++) {
            kn[0] = 'k';
            kn[1] = (char)('0' + (j / 10));
            kn[2] = (char)('0' + (j % 10));
            kn[3] = 0;
            if (tab_set(t, kn, "v") != 0) break;
            added++;
        }
        if (added != STORE64_MAX_KEYS) fails |= 32;
        if (tab_set(t, "k64", "v") == 0) fails |= 32;
        if (t->count != STORE64_MAX_KEYS) fails |= 32;
        if (tab_find(t, "k00") < 0) fails |= 32;
    }

    // bit6: 槽容量上限（31B key + 255B value = 288B/条，64 条装不进 16320B -> 必须提前被拒）
    t->count = 0;
    {
        char k31[32];
        char v255[256];
        for (int j = 0; j < 31; j++) k31[j] = 'k';
        k31[31] = 0;
        for (int j = 0; j < 255; j++) v255[j] = 'A';
        v255[255] = 0;
        int nfit = 0;
        bool rejected = false;
        for (int j = 0; j < STORE64_MAX_KEYS; j++) {
            k31[29] = (char)('0' + (j / 10));       // 尾 2 位保证 key 唯一
            k31[30] = (char)('0' + (j % 10));
            if (tab_set(t, k31, v255) != 0) { rejected = true; break; }
            nfit++;
        }
        if (!rejected || nfit >= STORE64_MAX_KEYS || nfit < 1) fails |= 64;
        if (tab_payload(t) > ST_PAYLOAD_MAX) fails |= 64;
        if (t->count != nfit) fails |= 64;
    }

    // bit7: 槽构造 -> 校验 -> 解析 往返（键值/条数/世代完全一致）
    t->count = 0;
    bool built = false;
    {
        if (tab_set(t, "lang", "zh") != 0)       fails |= 128;
        if (tab_set(t, "res", "1280x800") != 0)  fails |= 128;
        if (tab_set(t, "theme", "dark") != 0)    fails |= 128;
        uint32_t plen = 0;
        uint32_t pcrc = 0;
        built = slot_build(t, 7, g_slot_buf[0], &plen, &pcrc);
        if (!built) fails |= 128;
        else {
            uint64_t rgen = 0;
            uint32_t rlen = 0;
            if (slot_validate(g_slot_buf[0], &rgen, &rlen) != 0) fails |= 128;
            else if (rgen != 7 || rlen != plen) fails |= 128;
            else if (!slot_parse(g_slot_buf[0], rlen, t)) fails |= 128;
            else {
                const int li = tab_find(t, "lang");
                const int ri = tab_find(t, "res");
                if (t->count != 3) fails |= 128;
                if (li < 0 || st_scmp(t->kv[li].val, "zh") != 0) fails |= 128;
                if (ri < 0 || st_scmp(t->kv[ri].val, "1280x800") != 0) fails |= 128;
            }
        }
    }

    // bit8: 坏槽检测（改 payload -> payload_crc；改头部 -> header_crc；改 magic/版本 -> 对应原因）
    if (built) {
        uint8_t* b = g_slot_buf[0];
        b[ST_HEADER_BYTES + 1] ^= 0xFF;
        if (slot_validate(b, nullptr, nullptr) != 6) fails |= 256;
        b[ST_HEADER_BYTES + 1] ^= 0xFF;
        b[ST_O_GEN] ^= 0xFF;
        if (slot_validate(b, nullptr, nullptr) != 5) fails |= 256;
        b[ST_O_GEN] ^= 0xFF;
        b[ST_O_MAGIC] ^= 0x01;
        if (slot_validate(b, nullptr, nullptr) != 1) fails |= 256;
        b[ST_O_MAGIC] ^= 0x01;
        b[ST_O_VER] ^= 0x01;
        if (slot_validate(b, nullptr, nullptr) != 2) fails |= 256;
        b[ST_O_VER] ^= 0x01;
        if (slot_validate(b, nullptr, nullptr) != 0) fails |= 256;   // 还原后必须又有效
    }

    // bit9: 真载体探测。有卷（VFS 已挂载）就验证 vfs64_write + vfs64_read 的**整槽 16KB 往返** ——
    //        用独立的临时文件 /st64.tmp，写完即删，**绝不碰** /store.a、/store.b 这两个真槽；
    //        没有卷才退回裸盘**只读**探测（ATA 未链接 / 没 init 时跳过，不算失败）。
    if (st_vfs_available()) {
        for (uint32_t i = 0; i < ST_SLOT_BYTES; i++) g_slot_buf[0][i] = (uint8_t)(0xA5u ^ (uint8_t)(i * 7u));
        const int sys = vfs64_system_slot64();                   // ★ 临时文件也钉在系统卷上
        const int wn = vfs64_write_on64(sys, ST_VFS_TMP, g_slot_buf[0], (int)ST_SLOT_BYTES);
        int rn = -1;
        if (wn == (int)ST_SLOT_BYTES) rn = vfs64_read_on64(sys, ST_VFS_TMP, g_slot_buf[1], (int)ST_SLOT_BYTES);
        const bool same = (rn == (int)ST_SLOT_BYTES) &&
                          (st_cmp(g_slot_buf[0], g_slot_buf[1], ST_SLOT_BYTES) == 0);
        if (!same) fails |= 512;
        if (vfs64_unlink_on64(vfs64_system_slot64(), ST_VFS_TMP) != 0) {
            dbg64_line_begin64();
            dbg64_str("[STORE64] selftest vfs probe WARN: temp file not removed");   // 只警告：多余文件不影响正确性
            dbg64_nl();
            dbg64_line_end64();
        }
        dbg64_line_begin64();
        dbg64_str("[STORE64] selftest vfs probe ");
        dbg64_str(same ? "ok" : "FAIL");
        dbg64_str(" slot_bytes=");
        dbg64_dec((uint64_t)ST_SLOT_BYTES);
        dbg64_str(" write=");
        dbg64_dec((uint64_t)(wn < 0 ? 0 : wn));
        dbg64_str(" read=");
        dbg64_dec((uint64_t)(rn < 0 ? 0 : rn));
        dbg64_nl();
        dbg64_line_end64();
    } else if (!st_ata_linked() || g_drive < 0) {
        dbg64_line_begin64();
        dbg64_str("[STORE64] selftest carrier probe skipped (no vfs volume, ata not linked or no drive)");
        dbg64_nl();
        dbg64_line_end64();
    } else {
        const bool a = ata64_read(g_drive, slot_lba(0), ST_SLOT_SECTORS, g_slot_buf[0]);
        const bool b = ata64_read(g_drive, slot_lba(1), ST_SLOT_SECTORS, g_slot_buf[1]);
        if (!a && !b) fails |= 512;
        dbg64_line_begin64();
        dbg64_str("[STORE64] selftest disk probe ");
        dbg64_str((a || b) ? "ok" : "FAIL");
        dbg64_str(" slotA=");
        dbg64_str(a ? "rd" : "x");
        dbg64_str(" slotB=");
        dbg64_str(b ? "rd" : "x");
        dbg64_nl();
        dbg64_line_end64();
    }

    // bit10: 槽格式序列化/反序列化往返（**载体无关**）。载体（VFS 文件 /store.a|b 或裸盘 32 扇区）
    //        只负责搬运 ST_SLOT_BYTES 字节，所以"内存表 -> 槽镜像 -> 尺寸契约 -> 校验 -> 解析回表"
    //        这条往返在两种载体上必须完全一致，并且不含任何未初始化字节。
    {
        KVTable64* w = &g_st_tab;
        w->count = 0;
        if (tab_set(w, "theme", "dark") != 0)                    fails |= 1024;
        if (tab_set(w, "lang", "\xE4\xB8\xAD\xE6\x96\x87") != 0) fails |= 1024;   // "中文"：多字节也要逐字节一致
        if (tab_set(w, "n", "42") != 0)                          fails |= 1024;
        uint32_t plen = 0;
        uint32_t pcrc = 0;
        if (!slot_build(w, 9, g_slot_buf[0], &plen, &pcrc)) fails |= 1024;
        // 载体尺寸契约：必须正好一个槽；多一个 / 少一个字节都要拒（VFS 文件大小 / 裸盘扇区数）
        if (!st_slot_size_ok(ST_SLOT_BYTES))        fails |= 1024;
        if (st_slot_size_ok(ST_SLOT_BYTES - 1u))    fails |= 1024;
        if (st_slot_size_ok(ST_SLOT_BYTES + 1u))    fails |= 1024;
        st_copy(g_slot_buf[1], g_slot_buf[0], ST_SLOT_BYTES);     // 搬运 = 载体真正做的事（整槽字节进、出）
        uint64_t rgen = 0;
        uint32_t rlen = 0;
        if (slot_validate(g_slot_buf[1], &rgen, &rlen) != 0)      fails |= 1024;
        else if (rgen != 9 || rlen != plen)                       fails |= 1024;
        else if (rd32(g_slot_buf[1] + ST_O_COUNT) != 3u)          fails |= 1024;
        else if (pcrc != crc32_64(g_slot_buf[1] + ST_HEADER_BYTES, rlen)) fails |= 1024;
        else if (!slot_parse(g_slot_buf[1], rlen, w))             fails |= 1024;
        else {
            const int ti = tab_find(w, "theme");
            const int li = tab_find(w, "lang");
            const int ni = tab_find(w, "n");
            if (w->count != 3)                                    fails |= 1024;
            if (ti < 0 || st_scmp(w->kv[ti].val, "dark") != 0)    fails |= 1024;
            if (li < 0 || st_scmp(w->kv[li].val, "\xE4\xB8\xAD\xE6\x96\x87") != 0) fails |= 1024;
            if (ni < 0 || st_scmp(w->kv[ni].val, "42") != 0)      fails |= 1024;
            // 同一个表再构造一次必须逐字节一致（没有未初始化的填充字节）
            uint32_t plen2 = 0;
            uint32_t pcrc2 = 0;
            if (!slot_build(w, 9, g_slot_buf[0], &plen2, &pcrc2)) fails |= 1024;
            else if (plen2 != plen || pcrc2 != pcrc)              fails |= 1024;
            else if (st_cmp(g_slot_buf[0], g_slot_buf[1], ST_SLOT_BYTES) != 0) fails |= 1024;
        }
    }

    dbg64_line_begin64();
    dbg64_str("[STORE64] selftest ");
    if (fails == 0) {
        dbg64_str("PASS");
        dbg64_nl();
    } else {
        dbg64_str("FAIL mask=");
        dbg64_dec((uint64_t)fails);
        dbg64_nl();
    }
    dbg64_line_end64();
    return fails;
}
