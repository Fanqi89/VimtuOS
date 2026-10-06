/* vs_xz.c - ★ .deb 的 xz 解压：xz **容器** + LZMA2 数据（解 control.tar.xz / data.tar.xz）
 *
 * 为什么要有它：Debian 的包体是 `control.tar.xz` + `data.tar.xz`（dpkg-deb 默认 xz），
 *   仓库原本只有 gzip/DEFLATE（user/store/vs_gzip.c），于是发行版的**原样**包一律被
 *   VS_E_XZ 整包拒绝。本文件把这条链路补齐（内核零改动：纯 ring3）。
 *
 * 分工（本文件只做"容器"，解码核是 vendored 的公共领域代码）：
 *   * LZMA 范围编码 / LZMA2 块层 = third_party/lzma 的 LzmaDec.c + Lzma2Dec.c
 *     （Igor Pavlov，public domain；来源/版本/sha256 见 third_party/lzma/README.vimtu64.md）；
 *   * xz 容器（流头 / 块头 / 过滤器链 / 块尾对齐与校验字段）= 本文件自己写，
 *     为的是把"到底哪一条不支持"写成**能分得开的**日志，而不是一句 unsupported。
 *
 * 字典缓冲的用法（★ 本实现与"通用 xz 解码器"最重要的一处不同，也是省下 8 MiB 的原因）：
 *   我们**不按流里声明的 dictSize 分配字典**，而是把**调用方给的输出缓冲**当字典
 *   （dicBufSize = outcap，走 Lzma2Dec_AllocateProbs + 手工挂 dic）。依据两条既有事实：
 *     ① LZMA2 的匹配距离只能引用**已经产出**的字节（LzmaDec.c 自己就校验
 *        `distance >= processedPos` → SZ_ERROR_DATA），所以"产出 ≤ outcap"时永不回绕；
 *     ② 本仓库 gzip 那条路**同口径**：一次解压的产物上限就是 4 MiB（vs_pkg.c 的 dbuf），
 *        超了如实失败——不按 8 MiB dict 分配不等于少解数据。
 *   代价（如实写在报告里）：某个包的 tar 解出来 > outcap 会被拒，与 gzip 路完全一样。
 *
 * 如实边界（都返回 VS_E_XZ / VS_E_FORMAT，并打出**具体是哪一条**）：
 *   * 只支持**单块**（single-block）xz 流：多块（`xz --block-size` 造出来的）拒绝；
 *   * 过滤器链只支持**单个 LZMA2**（id 0x21、properties 1 字节）：BCJ/Delta 等前置过滤器拒绝；
 *   * 校验字段：check=1（CRC32）与 check=4（CRC64，xz 的**默认**，也是 dpkg 实际用的）
 *     **真验**；check=0（none）不用验；check=10（SHA-256）**只跳过**并打 verify=skipped；
 *   * 保留的 check 类型 / 保留的块头标志位 / 保留的流标志位：拒绝；
 *   * 一次解压的产物上限 = 调用方给的 outcap；xz 的索引/流尾校验不做（解完数据段即停）；
 *   * **不做**多流拼接（xz 允许把多个流串起来；现代 xz 单文件只写一个流）。
 *
 * 打点（tests/linuxapp64_test.py 与 tests/vpkg64_test.py 按这些串 grep；格式勿改）：
 *   [VPKG] deb xz member=<名> in=<压缩字节> out=<解出字节> dict=<流里声明的字典> check=<名> blocks=<n>
 *   [VPKG] deb xz member=<名> at=<偏移> reason=<原因>                      ← 失败（带偏移）
 */
#include "vs.h"
#include "Lzma2Dec.h"

/* LZMA2 的 properties 字节 -> 声明的字典大小（Lzma2Dec.c 内部的宏，这里按同一公式复算）。
 * 只用于日志与报告："流里声明了多少"≠"我们分配了多少"（见文件头）。 */
static int lzma2_dict_of(unsigned int prop) {
    if (prop == 40u) return -1;                       /* 0xFFFFFFFF：保留值，按"最大"记 */
    if (prop > 40u) return -2;
    return (int)((2u | (prop & 1u)) << (prop / 2u + 11u));
}

/* ---- ISzAlloc 适配器：概率表走 vs_mmap（不占 .data：本仓库一律 -fno-zero-initialized-in-bss） ---- */
static void* xz_alloc(ISzAllocPtr p, size_t size) {
    (void)p;
    return vs_mmap((int)size);
}

static void xz_free(ISzAllocPtr p, void* address) {
    (void)p;
    (void)address;                        /* vs_mmap 没有 free（本次运行期的如实边界，见 vs.h） */
}

static const ISzAlloc g_xz_alloc = { xz_alloc, xz_free };

/* 小端 u32（流头/块头的 CRC32 字段） */
static unsigned int rd_le32(const unsigned char* p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

/* xz 的可变长整数（LEB128，≤ 9 字节 / 63 位）。返回读掉的字节数（0 = 坏）。 */
static int rd_varint(const unsigned char* b, int n, int* off, unsigned long long* out) {
    unsigned long long v = 0;
    int shift = 0, used = 0;
    while (*off + used < n && used < 9) {
        const unsigned char c = b[*off + used];
        used++;
        v |= (unsigned long long)(c & 0x7Fu) << shift;
        if (!(c & 0x80u)) { *off += used; *out = v; return used; }
        shift += 7;
    }
    return 0;
}

static const char* check_name(int check) {
    if (check == 0) return "none";
    if (check == 1) return "crc32";
    if (check == 4) return "crc64";
    if (check == 10) return "sha256";
    return "reserved";
}

/* CRC-64/XZ（poly=0xC96C5795D7870F42 的反射形式，init = xorout = ~0）：xz 的 check=4。
 * 故意**不用查表**：256×8 B 的 .rodata 会吃掉 64 KiB 装载区里那点余量（见本文件头的体积说明），
 * 逐位实现对一个 256 KB 的 tar 也就毫秒级（客人里慢一些，但仍在噪声里）。 */
static unsigned long long crc64(const unsigned char* p, int n) {
    unsigned long long c = ~0ULL;
    for (int i = 0; i < n; i++) {
        c ^= (unsigned long long)p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xC96C5795D7870F42ULL & (unsigned long long)(-(long long)(c & 1ULL)));
    }
    return ~c;
}

/* ===========================================================================
 * vs_unxz：解一个 **单块** xz 流到 out（outcap 上限）。
 * 返回 VS_OK（*out_len = 解出的字节数）或 VS_E_XZ / VS_E_FORMAT（失败原因已打点）。
 * =========================================================================== */
int vs_unxz(const char* tag, const char* member, const unsigned char* in, int n,
            unsigned char* out, int outcap, int* out_len) {
    *out_len = 0;
    if (!out || outcap <= 0) return VS_E_NOMEM;
    if (n < 12 || in[0] != 0xFD || in[1] != 0x37 || in[2] != 0x7A || in[3] != 0x58 ||
        in[4] != 0x5A || in[5] != 0x00) {
        vs_log(tag, "deb xz member=%s at=0 reason=not-an-xz-stream bytes=%d\n", member, n);
        return VS_E_XZ;
    }
    /* ---- 流头：magic(6) + 流标志(2) + 标志的 CRC32(4) ---- */
    const int check = in[7] & 0x0F;
    if (in[6] != 0 || (in[7] & 0xF0) != 0 ||
        (check != 0 && check != 1 && check != 4 && check != 10)) {
        vs_log(tag, "deb xz member=%s at=6 reason=stream-flags flags=%02x%02x\n",
               member, in[6], in[7]);
        return VS_E_XZ;
    }
    if (vs_crc32(in + 6, 2) != rd_le32(in + 8)) {
        vs_log(tag, "deb xz member=%s at=8 reason=stream-header-crc\n", member);
        return VS_E_FORMAT;
    }

    int off = 12;                                /* 流头之后 */
    int blocks = 0, produced_total = 0, dict = 0, check_skipped = 0;
    for (;;) {
        if (off > n) {
            vs_log(tag, "deb xz member=%s at=%d reason=truncated-stream\n", member, off);
            return VS_E_FORMAT;
        }
        if (off == n) break;                     /* 数据段到头（没写索引也认） */
        if (in[off] == 0x00) break;              /* 索引指示符 = 块到此为止 */
        if (blocks > 0) {
            vs_log(tag, "deb xz member=%s at=%d reason=multi-block-unsupported blocks=%d\n",
                   member, off, blocks);
            return VS_E_XZ;
        }
        /* ---- 块头：首字节 b -> 头长 (b<<2)+4（末尾 4 字节是头的 CRC32） ---- */
        const int block_off = off;
        const int hsize = ((int)in[block_off]) << 2;
        if (hsize <= 0 || block_off + hsize + 4 > n) {
            vs_log(tag, "deb xz member=%s at=%d reason=bad-block-header-size\n", member, block_off);
            return VS_E_FORMAT;
        }
        if (vs_crc32(in + block_off, hsize) != rd_le32(in + block_off + hsize)) {
            vs_log(tag, "deb xz member=%s at=%d reason=block-header-crc\n", member, block_off);
            return VS_E_FORMAT;
        }
        const int hflags = in[block_off + 1];
        const int nfilters = (hflags & 0x03) + 1;
        if (hflags & 0x3C) {
            vs_log(tag, "deb xz member=%s at=%d reason=block-flags-reserved flags=%02x\n",
                   member, block_off, hflags);
            return VS_E_XZ;
        }
        int p = block_off + 2;
        unsigned long long pack = ~0ULL, unpack = ~0ULL;
        if (hflags & 0x40) {
            if (!rd_varint(in, block_off + hsize, &p, &pack)) {
                vs_log(tag, "deb xz member=%s at=%d reason=bad-packed-size-varint\n", member, block_off);
                return VS_E_FORMAT;
            }
        }
        if (hflags & 0x80) {
            if (!rd_varint(in, block_off + hsize, &p, &unpack)) {
                vs_log(tag, "deb xz member=%s at=%d reason=bad-unpacked-size-varint\n", member, block_off);
                return VS_E_FORMAT;
            }
        }
        if (nfilters != 1) {
            vs_log(tag, "deb xz member=%s at=%d reason=filter-chain n=%d\n",
                   member, block_off, nfilters);
            return VS_E_XZ;
        }
        unsigned long long fid = 0, propsz = 0;
        if (!rd_varint(in, block_off + hsize, &p, &fid) ||
            !rd_varint(in, block_off + hsize, &p, &propsz)) {
            vs_log(tag, "deb xz member=%s at=%d reason=bad-filter-id\n", member, block_off);
            return VS_E_FORMAT;
        }
        if (fid != 0x21ULL) {
            vs_log(tag, "deb xz member=%s at=%d reason=filter-not-lzma2 id=%x\n",
                   member, block_off, (unsigned)fid);
            return VS_E_XZ;
        }
        if (propsz != 1 || p >= block_off + hsize) {
            vs_log(tag, "deb xz member=%s at=%d reason=lzma2-props-size n=%d\n",
                   member, block_off, (int)propsz);
            return VS_E_XZ;
        }
        const unsigned int prop = in[p++];
        if (prop > 40u) {
            vs_log(tag, "deb xz member=%s at=%d reason=lzma2-props-range prop=%d\n",
                   member, block_off, (int)prop);
            return VS_E_XZ;
        }
        dict = lzma2_dict_of(prop);
        for (int z = p; z < block_off + hsize; z++) {
            if (in[z]) {
                vs_log(tag, "deb xz member=%s at=%d reason=block-header-padding\n", member, z);
                return VS_E_FORMAT;
            }
        }

        /* ---- 压缩数据 ---- */
        const int cdata = block_off + hsize + 4;
        if (cdata > n) {
            vs_log(tag, "deb xz member=%s at=%d reason=truncated-block\n", member, cdata);
            return VS_E_FORMAT;
        }
        int avail = n - cdata;
        if (pack != ~0ULL) {
            if (pack == 0 || pack > (unsigned long long)avail) {
                vs_log(tag, "deb xz member=%s at=%d reason=packed-size-out-of-range pack=%d avail=%d\n",
                       member, cdata, (int)pack, avail);
                return VS_E_FORMAT;
            }
            avail = (int)pack;
        }

        CLzma2Dec s;
        Lzma2Dec_Construct(&s);
        const SRes ar = Lzma2Dec_AllocateProbs(&s, (Byte)prop, &g_xz_alloc);
        if (ar != SZ_OK) {
            vs_log(tag, "deb xz member=%s at=%d reason=lzma2-alloc-probs res=%d\n", member, cdata, (int)ar);
            return VS_E_NOMEM;
        }
        s.decoder.dic = out;                     /* 字典 = 调用方的输出缓冲（见文件头） */
        s.decoder.dicBufSize = (SizeT)outcap;
        Lzma2Dec_Init(&s);

        const int out_base = produced_total;      /* 本块的产物从这里开始（校验字段用） */
        int consumed = 0, finished = 0;
        while (!finished) {
            SizeT inLen = (SizeT)(avail - consumed);
            const SizeT before = s.decoder.dicPos;
            ELzmaStatus st = LZMA_STATUS_NOT_SPECIFIED;
            const SRes r = Lzma2Dec_DecodeToDic(&s, (SizeT)outcap, in + cdata + consumed,
                                                &inLen, LZMA_FINISH_ANY, &st);
            consumed += (int)inLen;
            if (r != SZ_OK) {
                const int why = (r == SZ_ERROR_OUTPUT_EOF) ? VS_E_NOMEM : VS_E_FORMAT;
                vs_log(tag, "deb xz member=%s at=%d reason=lzma2-decode res=%d consumed=%d out=%d cap=%d\n",
                       member, cdata, (int)r, consumed, (int)s.decoder.dicPos, outcap);
                return (r == SZ_ERROR_DATA || r == SZ_ERROR_OUTPUT_EOF) ? why : VS_E_XZ;
            }
            if (st == LZMA_STATUS_FINISHED_WITH_MARK) { finished = 1; break; }
            if (s.decoder.dicPos == before && inLen == 0) {
                if (s.decoder.dicPos >= (SizeT)outcap) {
                    vs_log(tag, "deb xz member=%s at=%d reason=output-over-cap cap=%d\n",
                           member, cdata, outcap);
                    return VS_E_NOMEM;
                }
                vs_log(tag, "deb xz member=%s at=%d reason=lzma2-stream-not-finished consumed=%d/%d\n",
                       member, cdata, consumed, avail);
                return VS_E_FORMAT;
            }
        }
        const int produced = (int)s.decoder.dicPos - out_base;
        if (unpack != ~0ULL && unpack != (unsigned long long)produced) {
            vs_log(tag, "deb xz member=%s at=%d reason=unpacked-size-mismatch head=%d real=%d\n",
                   member, cdata, (int)unpack, produced);
            return VS_E_FORMAT;
        }
        if (pack != ~0ULL && consumed != (int)pack) {
            vs_log(tag, "deb xz member=%s at=%d reason=packed-size-mismatch head=%d used=%d\n",
                   member, cdata, (int)pack, consumed);
            return VS_E_FORMAT;
        }
        /* ---- 块尾：4 字节对齐 + 校验字段 ---- */
        const int pad = (4 - (consumed & 3)) & 3;
        const int cfield = cdata + consumed + pad;
        const int csize = (check == 0) ? 0 : ((check == 1) ? 4 : ((check == 4) ? 8 : 32));
        if (cfield + csize > n) {
            vs_log(tag, "deb xz member=%s at=%d reason=truncated-block-check\n", member, cfield);
            return VS_E_FORMAT;
        }
        for (int z = cdata + consumed; z < cfield; z++) {
            if (in[z]) {
                vs_log(tag, "deb xz member=%s at=%d reason=block-padding\n", member, z);
                return VS_E_FORMAT;
            }
        }
        if (check == 1) {                        /* CRC32：解开多少就验多少（真验，不跳过） */
            const unsigned int want = rd_le32(in + cfield);
            const unsigned int got = vs_crc32(out + out_base, produced);
            if (want != got) {
                vs_log(tag, "deb xz member=%s at=%d reason=check-crc32 want=%08x got=%08x\n",
                       member, cfield, want, got);
                return VS_E_FORMAT;
            }
        } else if (check == 4) {                 /* CRC64：xz 的**默认**，dpkg 打的包用的就是它 */
            unsigned long long w64 = 0, g64 = 0;
            for (int k = 7; k >= 0; k--) w64 = (w64 << 8) | (unsigned long long)in[cfield + k];
            g64 = crc64(out + out_base, produced);
            if (w64 != g64) {
                vs_log(tag, "deb xz member=%s at=%d reason=check-crc64 want=%08x%08x got=%08x%08x\n",
                       member, cfield, (unsigned)(w64 >> 32), (unsigned)w64,
                       (unsigned)(g64 >> 32), (unsigned)g64);
                return VS_E_FORMAT;
            }
        } else if (check == 10) {
            check_skipped = 1;                   /* SHA-256 校验字段本仓库没实现：如实标注跳过 */
        }
        produced_total += produced;
        off = cfield + csize;
        blocks++;
    }

    if (blocks == 0) {
        vs_log(tag, "deb xz member=%s at=%d reason=no-block\n", member, off);
        return VS_E_FORMAT;
    }
    if (check_skipped)
        vs_log(tag, "deb xz member=%s check=%s verify=skipped (no sha256 in this build)\n",
               member, check_name(check));
    *out_len = produced_total;
    vs_log(tag, "deb xz member=%s in=%d out=%d dict=%d check=%s blocks=%d\n",
           member, n, produced_total, dict, check_name(check), blocks);
    return VS_OK;
}
