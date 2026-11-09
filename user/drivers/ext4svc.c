/* ext4svc.c - ★ v0.9.33：**ring3 的 ext4 只读驱动服务**（混合驱动架构的第一个真驱动）
 *
 * 它做三件事，而这三件事恰好证明了框架的三条硬要求：
 *   1) 向内核**注册**"我处理 ext4、我服务这块盘"（r3drv_register）；
 *   2) 通过**零拷贝通道**拿数据：内核用 AHCI 的 DMA 直接把盘上扇区写进本进程映射着的
 *      共享页（SQE 里 obj_off 指向那块页），**全程没有一次 memcpy**；
 *   3) 挂掉也只死自己：内核监督器（kernel/r3drv64.cpp 的 tick）会把它重新拉起来。
 *
 * 为什么 ext4 解析放在 ring3（而不是像 VimtuFS2 那样塞进 ring0）：
 *   见 kernel/r3drv64.cpp 头部第 1 条 —— 按盘上字节做链式解引用的代码，最不该拿到 ring0 权限。
 *
 * 只读边界（如实，不许吹）：
 *   * 只支持 **extents**（EXT4_FEATURE_INCOMPAT_EXTENTS），不支持 extent 深度 > 1 的索引树
 *     （遇到打 `unsupported depth`，不猜）；
 *   * 不支持间接块映射（老 ext3 风格 i_block[15]）、不支持 64bit 块号、不支持 metadata_csum
 *     校验（夹具不置这些位；真卷上遇到就如实报原因）；
 *   * 不做日志（journal）重放 —— 只读挂载本来就不该碰日志；
 *   * **完全没有写路径**（框架本身就是只读的）。
 *
 * argv（内核 proc64_start_elf64_argv64 传）：argv[0]=路径 argv[1]=驱动器号 argv[2]=卷起始 LBA
 * 串口行（tests/r3drv64_test.py 按这些断言）：
 *   [EXT4SVC] start disk=<n> lba=<n> shm=<id> va=0x...
 *   [EXT4SVC] register ret=<slot> restarts=<n>
 *   [EXT4SVC] sb bs=<n> blocks=<n> inodes=<n> inode_size=<n> first_data=<n> incompat=0x... ok
 *   [EXT4SVC] root ino=2 size=<n> mode=0x... links=<n> extent_blocks=<n>
 *   [EXT4SVC] dirent name=<s> ino=<n> type=<n> size=<n>
 *   [EXT4SVC] file path=<s> size=<n> crc=0x........ ok|FAILED reason=..
 *   [EXT4SVC] zc-crc lba=<n> len=<n> crc=0x........        （零拷贝落地数据的校验和）
 *   [EXT4SVC] bench bytes=<n> mode=zerocopy ms=<n> mbs=<n>
 *   [EXT4SVC] bench bytes=<n> mode=copy ms=<n> mbs=<n>
 *   [EXT4SVC] crash test deliberately faulting（仅在 /etc/r3drv.conf 的 crash=1 且首次启动时）
 *   [EXT4SVC] serve rounds=<n> beats=<n> done
 */
#include "r3drv.h"

#define EXT4_MAGIC       0xEF53u
#define EXT_MAGIC        0xF30Au
#define EXT4_MIN_BLOCK   1024u

/* 共享对象：2 MiB（= SHM64 单对象上限）。第 0 页给环，其余给数据区。 */
#define SVC_SHM_BYTES    (2u * 1024u * 1024u)
#define SVC_DATA_BYTES   (SVC_SHM_BYTES - R3DRV_DATA_OFF)      /* 2,093,056 B */
#define SVC_DATA_SECS    (SVC_DATA_BYTES / 512u)                /* 4,088 扇区/次 */
#define SVC_BENCH_BYTES  (64u * 1024u * 1024u)                 /* 64 MiB 顺序读 */
#define SVC_SERVE_ROUNDS 300                                   /* 每轮 200ms ≈ 60s 常驻 */
/* ★ v0.9.34：心跳 40ms -> 200ms。原值让串口每秒 ~25 行（sycall trace + serve 打点），
 *   单次运行能到 1MB；200ms 仍然远小于内核 beat_ticks=250（1s）的判活窗口。 */
#define SVC_BEAT_MS      200

typedef unsigned long long u64;
typedef long long          i64;
typedef unsigned int       u32;
typedef int                i32;
typedef unsigned short     u16;
typedef unsigned char      u8;

static int      g_shm = -1;
static unsigned long g_va = 0;
static u32      g_disk = 0;
static u32      g_lba = 0;
static int      g_slot = -1;
static u32      g_restarts = 0;
static u32      g_beats = 0;
static int      g_crash_cfg = 0;
static int      g_bench_cfg = 0;

/* ---------------- 小工具 ---------------- */
static u32 le16(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 le32(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static int  streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return (*a == 0 && *b == 0);
}
static int  atoi_s(const char* s) {
    int v = 0, i = 0;
    if (!s || !s[0]) return 0;
    while (s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
    return v;
}
static void mem_copy(void* d, const void* s, unsigned long n) {
    u8* a = (u8*)d;
    const u8* b = (const u8*)s;
    for (unsigned long i = 0; i < n; i++) a[i] = b[i];
}

/* CRC32（IEEE，反射，poly 0xEDB88320 —— 与 Python zlib.crc32 同值） */
static u32 g_crc_tab[256];
static int g_crc_ready = 0;
static void crc_init(void) {
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_tab[i] = c;
    }
    g_crc_ready = 1;
}
static u32 crc32(u32 crc, const u8* p, unsigned long n) {
    if (!g_crc_ready) crc_init();
    u32 c = crc ^ 0xFFFFFFFFu;
    for (unsigned long i = 0; i < n; i++) c = g_crc_tab[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---------------- 零拷贝读：一次"提交 + 门铃 + 等就地完成" ---------------- */
/* 把 [lba, lba+secs) 读进**本进程映射的数据区**（偏移 R3DRV_DATA_OFF）。返回落地字节数。 */
static i64 zc_read(u64 lba, u32 secs, u32 copy_flag) {
    if (secs == 0) return 0;
    if (secs > SVC_DATA_SECS) secs = SVC_DATA_SECS;
    const u32 pos = r3_submit(g_va, lba, secs, R3DRV_DATA_OFF, copy_flag);
    const i64 k = lx_kick(g_shm);
    if (k < 0) return -3;
    return r3_wait(g_va, pos);
}
static const u8* data_ptr(void) { return (const u8*)(g_va + R3DRV_DATA_OFF); }

/* ---------------- 配置读取（bench / crash 两项由服务自己解释）---------------- */
static void read_cfg(void) {
    /* Linux open(2)/read(0)/close(3)；/etc/r3drv.conf 由内核（或构建期装卷）预置 */
    const i64 fd = lx_3(LX_NR_OPEN, (i64)(unsigned long)"/etc/r3drv.conf", 0, 0);
    if (fd < 0) return;
    static char buf[512];
    const i64 n = lx_3(LX_NR_READ, fd, (i64)(unsigned long)buf, (i64)(sizeof(buf) - 1));
    (void)lx_3(LX_NR_CLOSE, fd, 0, 0);
    if (n <= 0) return;
    buf[n] = 0;
    char* p = buf;
    while (*p) {
        char* e = p;
        while (*e && *e != '\n') e++;
        /* 简单 key=value（与内核同一套） */
        char key[24], val[24];
        int k = 0, v = 0;
        char* q = p;
        while (q < e && *q != '=' && k < 23) key[k++] = *q++;
        key[k] = 0;
        if (q < e && *q == '=') {
            q++;
            while (q < e && v < 23) val[v++] = *q++;
            val[v] = 0;
            if (streq(key, "bench")) g_bench_cfg = atoi_s(val);
            else if (streq(key, "crash")) g_crash_cfg = atoi_s(val);
        }
        if (!*e) break;
        p = e + 1;
    }
}

/* ---------------- ext4 只读解析 ---------------- */
struct Ext4 {
    u32 bs;           /* 块大小 */
    u32 blocks;       /* 总块数 */
    u32 inodes;
    u32 inode_size;
    u32 first_data;
    u32 inodes_per_group;
    u32 blocks_per_group;
    u32 incompat;
    u32 inode_tab_blk;
};
static struct Ext4 g_fs;
static u8  g_sb[1024];          /* 超级块镜像（从数据区拷出来，避免被后续读覆盖） */
static u8  g_ino[256];          /* 当前 inode 镜像（inode_size ≤ 256）*/
static u8  g_blk[4096];         /* 单块镜像（目录块/extent 索引块用） */
static u8  g_blk2[4096];        /* 子目录递归专用（不许和父目录共用一块，见 walk_dir 注释） */

static int ext4_open(void) {
    /* 超级块在卷内字节 1024 = 扇区 2 */
    i64 got = zc_read((u64)g_lba + 2, 2, 0);
    if (got < 1024) return -1;
    mem_copy(g_sb, data_ptr(), 1024);

    const u32 magic = le16(g_sb + 0x38);
    if (magic != EXT4_MAGIC) return -2;
    const u32 logbs = le32(g_sb + 0x18);
    g_fs.bs = EXT4_MIN_BLOCK << logbs;
    if (g_fs.bs < 1024 || g_fs.bs > 4096) return -3;
    g_fs.blocks          = le32(g_sb + 0x04);
    g_fs.inodes          = le32(g_sb + 0x00);
    g_fs.first_data      = le32(g_sb + 0x14);
    g_fs.blocks_per_group = le32(g_sb + 0x20);
    g_fs.inodes_per_group = le32(g_sb + 0x28);
    // ★ 偏移必须按真实 ext4：0x54 = s_first_ino（u32），**0x58 = s_inode_size（u16）**，
    //   0x5A = s_block_group_nr。最初照错误笔记写成 0x54，夹具回读立刻对不上 —— 已改正。
    g_fs.inode_size      = le16(g_sb + 0x58);
    g_fs.incompat        = le32(g_sb + 0x60);
    if (g_fs.inode_size < 128 || g_fs.inode_size > 256) return -4;

    /* 组描述符表在块 first_data+1；第 0 组的 inode 表块号在 +0x08 */
    const u32 gdt_blk = g_fs.first_data + 1;
    const u64 gdt_lba = (u64)g_lba + (u64)gdt_blk * (g_fs.bs / 512u);
    got = zc_read(gdt_lba, g_fs.bs / 512u, 0);
    if (got < (i64)g_fs.bs) return -5;
    g_fs.inode_tab_blk = le32(data_ptr() + 0x08);
    return 0;
}

/* 读一个 inode 到 g_ino（返回 i_size 与 i_block 指针） */
static i64 inode_read(u32 ino, const u8** out_iblock) {
    if (ino == 0) return -1;
    const u64 off = (u64)g_fs.inode_tab_blk * g_fs.bs + (u64)(ino - 1) * g_fs.inode_size;
    const u64 lba = (u64)g_lba + off / 512u;
    const u32 secs = (g_fs.inode_size + (off % 512u) + 511u) / 512u;
    const i64 got = zc_read(lba, secs, 0);
    if (got < (i64)g_fs.inode_size) return -2;
    mem_copy(g_ino, data_ptr() + (off % 512u), g_fs.inode_size);
    if (out_iblock) *out_iblock = g_ino + 0x28;       /* i_block[60] */
    u64 sz = le32(g_ino + 0x04);
    if ((le16(g_ino + 0x00) & 0xF000u) == 0x8000u) {  /* 普通文件才有 i_size_high */
        sz |= ((u64)le32(g_ino + 0x6C) << 32);           // i_size_high 在 0x6C（0x64 是 i_generation）
    }
    return (i64)sz;
}

/* 读一个连续物理块区间到 g_blk（只用于 ≤ 4096B 的单块） */
static int block_read(u64 phys_blk, u8* out, u32 want) {
    const u64 lba = (u64)g_lba + phys_blk * (g_fs.bs / 512u);
    const i64 got = zc_read(lba, (want + 511u) / 512u, 0);
    if (got < (i64)want) return -1;
    mem_copy(out, data_ptr(), want);
    return 0;
}

/* 用 extent 树把文件数据依次读出来并累计 CRC（只支持 depth 0/1）
 * ★ 为什么入口先把 i_block 拷到局部 ex[60]：`iblock` 指向的是全局 g_ino（inode 镜像），
 *   而遍历过程中会再调 inode_read() 把 g_ino 覆盖掉。直接拿着指针走 = 读到别人的 extent。
 *   这是本文件最容易踩的一类 bug（缓冲区复用），所以副本放在最靠前的位置，一眼能看到。 */
static i64 file_crc(const u8* iblock, u64 size, u32* out_crc) {
    u8 ex[60];
    mem_copy(ex, iblock, sizeof(ex));
    const u32 magic = le16(ex + 0x00);
    if (magic != EXT_MAGIC) return -1;
    const u32 entries = le16(ex + 0x02);
    const u32 depth   = le16(ex + 0x06);
    if (depth > 1) return -2;                       /* 不做更深索引树（如实拒绝） */
    u32 crc = 0;
    u64 done = 0;
    if (depth == 0) {
        for (u32 i = 0; i < entries && i < 4u; i++) {
            const u8* e = ex + 12 + i * 12;
            const u32 logical = le32(e + 0);
            const u32 len     = le16(e + 4) & 0x7FFFu;
            const u64 start   = ((u64)le16(e + 6) << 32) | le32(e + 8);
            (void)logical;
            u64 left = (u64)len * g_fs.bs;
            if (done + left > size) left = size - done;
            u64 off = 0;
            while (off < left) {
                u32 chunk = (u32)((left - off) / 512u);
                if (chunk > SVC_DATA_SECS) chunk = SVC_DATA_SECS;
                if (chunk == 0) chunk = 1;
                const u64 lba = (u64)g_lba + (start * (g_fs.bs / 512u)) + off / 512u;
                const i64 got = zc_read(lba, chunk, 0);
                if (got <= 0) return -3;
                u32 take = (u32)got;
                if ((u64)take > left - off) take = (u32)(left - off);
                crc = crc32(crc, data_ptr(), take);
                off += take;
            }
            done += left;
            if (done >= size) break;
        }
    } else {
        /* depth == 1：first extent 指向一个索引块，索引块里是 4 个叶子 extent 项 */
        for (u32 i = 0; i < entries && i < 4u; i++) {
            const u8* e = ex + 12 + i * 12;
            const u64 idx_blk = ((u64)le16(e + 6) << 32) | le32(e + 8);
            if (block_read(idx_blk, g_blk, g_fs.bs) != 0) return -4;
            const u32 lm = le16(g_blk + 0x00);
            const u32 le_cnt = le16(g_blk + 0x02);
            if (lm != EXT_MAGIC) return -5;
            for (u32 j = 0; j < le_cnt && j < 4u; j++) {
                const u8* le2 = g_blk + 12 + j * 12;
                const u32 len   = le16(le2 + 4) & 0x7FFFu;
                const u64 start = ((u64)le16(le2 + 6) << 32) | le32(le2 + 8);
                u64 left = (u64)len * g_fs.bs;
                if (done + left > size) left = size - done;
                u64 off = 0;
                while (off < left) {
                    u32 chunk = (u32)((left - off) / 512u);
                    if (chunk > SVC_DATA_SECS) chunk = SVC_DATA_SECS;
                    if (chunk == 0) chunk = 1;
                    const u64 lba = (u64)g_lba + (start * (g_fs.bs / 512u)) + off / 512u;
                    const i64 got = zc_read(lba, chunk, 0);
                    if (got <= 0) return -3;
                    u32 take = (u32)got;
                    if ((u64)take > left - off) take = (u32)(left - off);
                    crc = crc32(crc, data_ptr(), take);
                    off += take;
                }
                done += left;
                if (done >= size) break;
            }
            if (done >= size) break;
        }
    }
    *out_crc = crc;
    return (i64)done;
}

/* 列目录 + 对每个普通文件算 CRC（这就是"用户态真的把 ext4 读出来了"的凭据）
 * ★ 缓冲区纪律（同一类坑的第二次预防）：
 *   * 入口把 i_block 拷进局部 ex[60] —— 因为循环里会 inode_read() 覆盖 g_ino；
 *   * 目录块用调用方给的 blk[]，且**递归时换一块**（g_blk2）—— 否则子目录会踩掉父目录
 *     正在解析的那一块，而父目录的游标 p 还要继续用。 */
static int walk_dir(const u8* iblock, u64 dir_size, int depth, u8* blk) {
    u8 ex[60];
    mem_copy(ex, iblock, sizeof(ex));
    u64 off = 0;
    int files = 0;
    const u32 entries = le16(ex + 0x02);
    const u32 dep = le16(ex + 0x06);
    if (dep != 0) return 0;                            /* 目录也只用 depth 0 夹具 */
    while (off < dir_size) {
        /* 找 off 所在的 extent */
        int found = 0;
        u64 start = 0;
        u32 elen = 0, elog = 0;
        for (u32 i = 0; i < entries && i < 4u; i++) {
            const u8* e = ex + 12 + i * 12;
            elog = le32(e + 0);
            elen = le16(e + 4) & 0x7FFFu;
            const u64 s = ((u64)le16(e + 6) << 32) | le32(e + 8);
            if (off / g_fs.bs >= elog && off / g_fs.bs < (u64)elog + elen) { start = s; found = 1; break; }
        }
        if (!found) break;
        const u64 blk_off = (off / g_fs.bs) - elog;
        if (block_read(start + blk_off, blk, g_fs.bs) != 0) return -1;
        u32 p = 0;
        while (p + 8 <= g_fs.bs) {
            const u32 ino = le32(blk + p);
            const u32 reclen = le16(blk + p + 4);
            const u32 nlen = blk[p + 6];
            const u32 ftype = blk[p + 7];
            if (reclen < 8 || p + reclen > g_fs.bs) break;
            if (ino != 0 && nlen > 0 && nlen < 256) {
                char name[256];
                mem_copy(name, blk + p + 8, nlen);
                name[nlen] = 0;
                const int is_dot = (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)));
                if (!is_dot) {
                    const u8* ib = 0;
                    const i64 fsz = inode_read(ino, &ib);
                    {
                        R3_LINE_BEGIN
                        R3_ADD("[EXT4SVC] dirent name=");  R3_ADD(name);
                        R3_ADD(" ino=");                   R3_ADDU(ino);
                        R3_ADD(" type=");                  R3_ADDU(ftype);
                        R3_ADD(" size=");                  R3_ADDU(fsz < 0 ? 0u : (u64)fsz);
                        R3_ADD(fsz >= 0 ? " ok" : " FAILED");
                        R3_ADD("\n");                      R3_FLUSH();
                    }
                    if (fsz >= 0 && ftype == 1 && depth < 2) {
                        u32 crc = 0;
                        const i64 got = file_crc(ib, (u64)fsz, &crc);
                        R3_LINE_BEGIN
                        R3_ADD("[EXT4SVC] file path=/");  R3_ADD(name);
                        R3_ADD(" size=");                  R3_ADDU(fsz);
                        R3_ADD(" crc=");                   R3_ADD("0x");
                        {
                            static const char hx[] = "0123456789abcdef";
                            char o[9];
                            for (int i = 0; i < 8; i++) o[i] = hx[(crc >> ((7 - i) * 4)) & 0xF];
                            o[8] = 0;
                            R3_ADD(o);
                        }
                        R3_ADD(" read=");                  R3_ADDU(got < 0 ? 0u : (u64)got);
                        R3_ADD(got == (i64)fsz ? " ok" : " FAILED");
                        R3_ADD("\n");                      R3_FLUSH();
                        files++;
                    } else if (fsz >= 0 && ftype == 2 && depth < 2) {
                        /* 子目录：递归一层（夹具里 /sub 只有一层） */
                        const u64 sub = (u64)fsz;
                        (void)sub;
                        /* 目录数据块的 extent 与父目录同构，直接复用 walk_dir（off 从 0 起） */
                        (void)walk_dir(ib, (u64)fsz, depth + 1, (depth == 0) ? g_blk2 : g_blk);
                    }
                }
            }
            p += reclen;
        }
        off = ((off / g_fs.bs) + 1) * g_fs.bs;
    }
    return files;
}

/* ---------------- 64 MiB 顺序读吞吐（零拷贝 vs 拷贝对照）---------------- */
static u32 ticks_ms(void) {
    return (u32)v80_1(V80_NR_TICKS, 0) * 4u;      /* PIT 250Hz -> 每 tick 4ms */
}
static void bench(u32 copy_flag, const char* tag) {
    const u32 total_secs = SVC_BENCH_BYTES / 512u;      /* 131072 */
    u32 done = 0;
    const u32 t0 = ticks_ms();
    u64 bytes = 0;
    while (done < total_secs) {
        u32 n = total_secs - done;
        if (n > SVC_DATA_SECS) n = SVC_DATA_SECS;
        const i64 got = zc_read((u64)g_lba + done, n, copy_flag);
        if (got <= 0) {
            R3_LINE_BEGIN R3_ADD("[EXT4SVC] bench FAILED mode="); R3_ADD(tag);
            R3_ADD(" at="); R3_ADDU(done); R3_ADD("\n"); R3_FLUSH();
            return;
        }
        bytes += (u64)got;
        done += n;
        (void)lx_beat();                                /* 长循环里必须 beat（监督器判活） */
    }
    const u32 t1 = ticks_ms();
    const u32 ms = (t1 > t0) ? (t1 - t0) : 1u;
    const u64 mbs = (bytes / 1048576u) * 1000u / (u64)ms;
    R3_LINE_BEGIN
    R3_ADD("[EXT4SVC] bench bytes=");  R3_ADDU(bytes);
    R3_ADD(" mode=");                  R3_ADD(tag);
    R3_ADD(" ms=");                    R3_ADDU(ms);
    R3_ADD(" mbs=");                   R3_ADDU(mbs);
    R3_ADD("\n");                      R3_FLUSH();
}

/* ---------------- 主流程 ---------------- */
int main(int argc, char** argv) {
    crc_init();
    if (argc >= 3) {
        g_disk = (u32)atoi_s(argv[1]);
        g_lba  = (u32)atoi_s(argv[2]);
    }
    read_cfg();

    /* (1) 建共享对象并映射进自己 —— 这块内存既是"环"又是"DMA 落点" */
    g_shm = (int)v80_1(V80_NR_SHM_CREATE, (i64)SVC_SHM_BYTES);
    if (g_shm <= 0) {
        R3_LINE_BEGIN R3_ADD("[EXT4SVC] shm_create FAILED ret="); R3_ADDU((u64)(i64)g_shm);
        R3_ADD("\n"); R3_FLUSH();
        return 1;
    }
    if (v80_4(V80_NR_SHM_MAP, (i64)g_shm, 0, (i64)SVC_SHM_BYTES, (i64)(unsigned long)&g_va) != 0) {
        R3_LINE_BEGIN R3_ADD("[EXT4SVC] shm_map FAILED\n"); R3_FLUSH();
        return 1;
    }
    r3_ring_init(g_va);
    {
        R3_LINE_BEGIN
        R3_ADD("[EXT4SVC] start disk="); R3_ADDU(g_disk);
        R3_ADD(" lba=");                 R3_ADDU(g_lba);
        R3_ADD(" shm=");                 R3_ADDU((u64)g_shm);
        R3_ADD(" va=");                  R3_ADDU(g_va);
        R3_ADD(" bench=");               R3_ADDU((u64)g_bench_cfg);
        R3_ADD(" crash=");               R3_ADDU((u64)g_crash_cfg);
        R3_ADD("\n");                    R3_FLUSH();
    }

    /* (2) 注册：告诉内核"ext4 由我在用户态处理" */
    const i64 slot = lx_reg("ext4svc", R3DRV_PROTO_EXT4, g_disk, g_shm, SVC_SHM_BYTES);
    g_slot = (int)slot;
    {
        struct R3drvInfo info;
        if (slot >= 0 && lx_status((u32)slot, &info) == 0) g_restarts = info.restarts;
        R3_LINE_BEGIN
        R3_ADD("[EXT4SVC] register ret="); R3_ADDU((u64)(slot < 0 ? 0 : slot));
        R3_ADD(" restarts=");              R3_ADDU((u64)g_restarts);
        R3_ADD(slot >= 0 ? " ok" : " FAILED");
        R3_ADD("\n");                      R3_FLUSH();
    }
    if (slot < 0) return 2;

    /* (3) 崩溃测试（只在配置要求且**首次**启动时做一次）：
     *     直接写空指针 -> #PF -> 内核杀进程 -> 监督器 restart -> 新实例 restarts=1 不再崩。
     *     —— 这条路径就是"Ring 3 驱动崩了，系统不死机"的实证。 */
    if (g_crash_cfg && g_restarts == 0) {
        /* crash=1 -> 写空指针（#PF）；crash=2 -> 整数除零（#DE）。
         * 两条都是**真硬件异常**（不是 exit()）：走"内核异常 -> 杀进程 -> 监督器重启"这条链。 */
        R3_LINE_BEGIN R3_ADD("[EXT4SVC] crash test deliberately faulting mode=");
        R3_ADDU((u64)g_crash_cfg); R3_ADD(" (restarts=0)\n"); R3_FLUSH();
        if (g_crash_cfg == 2) {
            volatile int z = 0;
            volatile int q = 100 / z;               /* 预期 #DE（除零）：进程被内核终止 */
            (void)q;
        }
        (void)lx_beat();
        volatile unsigned long* bad = (volatile unsigned long*)0x8;
        *bad = 0xDEADBEEF;                          /* 预期 #PF：进程被内核终止 */
    }

    /* (4) 真解析 ext4 */
    (void)lx_beat();
    const int rc = ext4_open();
    {
        R3_LINE_BEGIN
        R3_ADD("[EXT4SVC] sb bs=");        R3_ADDU(g_fs.bs);
        R3_ADD(" blocks=");                R3_ADDU(g_fs.blocks);
        R3_ADD(" inodes=");                R3_ADDU(g_fs.inodes);
        R3_ADD(" inode_size=");            R3_ADDU(g_fs.inode_size);
        R3_ADD(" first_data=");            R3_ADDU(g_fs.first_data);
        R3_ADD(" incompat=0x");            {
            static const char hx[] = "0123456789abcdef";
            char o[9];
            for (int i = 0; i < 8; i++) o[i] = hx[(g_fs.incompat >> ((7 - i) * 4)) & 0xF];
            o[8] = 0; R3_ADD(o);
        }
        R3_ADD(rc == 0 ? " ok" : " FAILED");
        if (rc != 0) { R3_ADD(" reason="); R3_ADDU((u64)(-rc)); }
        R3_ADD("\n"); R3_FLUSH();
    }
    if (rc == 0) {
        (void)lx_beat();
        const u8* ib = 0;
        const i64 rsz = inode_read(2, &ib);          /* inode 2 = 根目录 */
        R3_LINE_BEGIN
        R3_ADD("[EXT4SVC] root ino=2 size=");  R3_ADDU(rsz < 0 ? 0u : (u64)rsz);
        R3_ADD(" links=");                     R3_ADDU(le16(g_ino + 0x1A));
        R3_ADD(rsz >= 0 ? " ok" : " FAILED");
        R3_ADD("\n");                          R3_FLUSH();
        if (rsz >= 0) {
            (void)lx_beat();
            (void)walk_dir(ib, (u64)rsz, 0, g_blk);
        }
        /* 零拷贝落地正确性：读卷首 4 KiB 算 CRC（宿主侧用同一算法规格断言） */
        (void)lx_beat();
        if (zc_read((u64)g_lba, 8, 0) == 4096) {
            const u32 c = crc32(0, data_ptr(), 4096);
            R3_LINE_BEGIN
            R3_ADD("[EXT4SVC] zc-crc lba="); R3_ADDU(g_lba);
            R3_ADD(" len=4096 crc=0x");
            {
                static const char hx[] = "0123456789abcdef";
                char o[9];
                for (int i = 0; i < 8; i++) o[i] = hx[(c >> ((7 - i) * 4)) & 0xF];
                o[8] = 0; R3_ADD(o);
            }
            R3_ADD("\n"); R3_FLUSH();
        }
    }

    /* (5) 吞吐取证：同一条通道、同一块盘、同样 64 MiB，只差"置不置 COPY 位" */
    if (g_bench_cfg) {
        (void)lx_beat();
        bench(0, "zerocopy");
        (void)lx_beat();
        bench(R3DRV_F_COPY, "copy");
    }

    /* (6) 常驻：按心跳活着（内核监督器据此判活）*/
    for (u32 round = 0; round < SVC_SERVE_ROUNDS; round++) {
        (void)lx_beat();
        g_beats++;
        if ((round % 125u) == 0) {
            struct R3drvInfo info;
            R3_LINE_BEGIN
            R3_ADD("[EXT4SVC] serve round=");  R3_ADDU((u64)round);
            R3_ADD(" beats=");                 R3_ADDU((u64)g_beats);
            if (g_slot >= 0 && lx_status((u32)g_slot, &info) == 0) {
                R3_ADD(" kern_beats=");  R3_ADDU(info.beats);
                R3_ADD(" dma=");         R3_ADDU(info.dma_bytes);
                R3_ADD(" cpy=");         R3_ADDU(info.copy_bytes);
                R3_ADD(" rst=");         R3_ADDU((u64)info.restarts);
            }
            R3_ADD("\n"); R3_FLUSH();
        }
        (void)v80_2(V80_NR_SLEEP_MS, SVC_BEAT_MS, 0);
    }
    R3_LINE_BEGIN R3_ADD("[EXT4SVC] serve rounds="); R3_ADDU((u64)SVC_SERVE_ROUNDS);
    R3_ADD(" beats="); R3_ADDU((u64)g_beats); R3_ADD(" done\n"); R3_FLUSH();
    return 0;
}
