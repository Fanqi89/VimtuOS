// app64.cpp - VAP64 可安装应用：安装（写进 VimtuFS2）+ 加载（从盘上读出来）+ 启动（进 ring3）
//
// 三件事，对应三个 API（见 app64.h 的完整布局/打点说明）：
//   app64_install_builtin64(drive, part_lba)  内嵌 hello.vap -> vfs64_write("/hello.vap")，幂等
//   app64_launch64(path)                      vfs64_read -> 校验 VAP64 头 -> user64_run_blob64
//   app64_selftest64()                        位掩码自检（头往返 / 拒绝坏包 / 名字边界 / 内嵌 blob）
//
// 本文件只进**系统内核**（build64.sh 的 SRCS_OS）；安装程序内核不需要它。
// 依赖：vfs64（文件系统，OS 内核已链接）、usermode64（进 ring3）、ata64（读 MBR 找主分区）、
//       part64.h（主分区起始 LBA 的常量定义，仅头文件，不引入 part64.cpp）。
#include "app64.h"
#include "vfs64.h"
#include "usermode64.h"
#include "part64.h"     // PART_MAIN_LBA：主分区起始 LBA 的唯一定义点（= 9 + 8000 = 8009）
#include "elf64.h"      // ELF64 加载器（app64_run_any64 按魔数分派到这里）
#include "ata64.h"      // 挂载前读 MBR，按安装器同一条规则找 type=0x07 分区
#include "debug64.h"
#include "panic64.h"      // ★ pause/unpause：注册表读写要真读盘，别让看门狗误判（与 terminal/fd64 同款）

// ★ 本批（演示程序搬进系统卷）：hello.vap 的字节**不在内核里** ——
//   交付 = 系统卷里的 /hello.vap（tools/demo_pack_win.py 构建期写入 + 逐字节回读自检）；
//   启动期的"幂等装卷"只在卷里没有时从构建期"原始区"取字节（空夹具盘的情形，见 kernel/demo64.h）。
#include "demo64.h"

static const uint8_t VAP64_MAGIC64[8] = { 'V', 'A', 'P', '6', '4', 0, 0, 0 };
static const char    VAP64_INSTALL_PATH64[] = "/hello.vap";

// 读盘缓冲：静态 .bss（不进镜像）。头+名字(≤24)+代码(≤32768) = ≤32824B，留到 64KB 富余。
static uint8_t g_vap_file64[VAP64_MAX_FILE_BYTES64];

// ==================== 小工具（不依赖 libc）====================
static uint32_t vap_rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void vap_wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}
static bool vap_bytes_eq(const uint8_t* a, const uint8_t* b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return false;
    return true;
}
static uint32_t vap_strlen(const char* s) {
    uint32_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}
// 标准 CRC-32（反射 0xEDB88320、初值/末异或 0xFFFFFFFF）——与 zlib.crc32 / vfs64 同口径，
// 自检里有 crc32("123456789") == 0xCBF43926 的写死断言防实现漂移。
static uint32_t vap_crc32(const uint8_t* p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}
static void vap_hex32(uint32_t v) {
    static const char* H = "0123456789ABCDEF";
    char b[9];
    for (int i = 7; i >= 0; i--) { b[i] = H[v & 0xFu]; v >>= 4; }
    b[8] = 0;
    dbg64_str(b);
}

// ==================== VAP64 头解析（安全问题都在这里挡住）====================
enum {
    VAP64_OK = 0,
    VAP64_ERR_ARG,       // 参数为空
    VAP64_ERR_MAGIC,     // magic 不对
    VAP64_ERR_VERSION,   // version/header_size 不对
    VAP64_ERR_NAME,      // 名字长度非法 / 无 NUL 结尾 / 名字里有 NUL
    VAP64_ERR_SIZE,      // 短于头 / 长度不一致 / entry_offset 不合法 / code_size 非法
    VAP64_ERR_CRC        // code_crc32 与实测不符
};
static const char* vap_reason_text(int rc) {
    switch (rc) {
        case VAP64_ERR_ARG:     return "arg";
        case VAP64_ERR_MAGIC:   return "magic";
        case VAP64_ERR_VERSION: return "version";
        case VAP64_ERR_NAME:    return "name";
        case VAP64_ERR_SIZE:    return "size";
        case VAP64_ERR_CRC:     return "crc";
        default:                return "?";
    }
}

struct Vap64FileInfo {
    uint32_t version, header_size, entry_offset, code_size, code_crc, name_len, file_bytes;
    const uint8_t* name;      // 指向文件内名字字段（已确认 NUL 结尾；只当字符串用，绝不当指针执行）
    const uint8_t* code;      // = 文件基址 + entry_offset（已与 name_len/总长交叉校验）
};

// 解析 + 校验。rc != VAP64_OK 时 *out 不保证有效（调用方只用返回值）。
static int vap64_parse(const uint8_t* p, uint32_t n, Vap64FileInfo* out) {
    if (!p || !out) return VAP64_ERR_ARG;
    if (n < VAP64_HEADER_SIZE64) return VAP64_ERR_SIZE;
    if (!vap_bytes_eq(p, VAP64_MAGIC64, 8)) return VAP64_ERR_MAGIC;

    const uint32_t version = vap_rd32(p + 8);
    const uint32_t hs      = vap_rd32(p + 12);
    const uint32_t eo      = vap_rd32(p + 16);
    const uint32_t cs      = vap_rd32(p + 20);
    const uint32_t crc     = vap_rd32(p + 24);
    const uint32_t nl      = vap_rd32(p + 28);

    if (version != VAP64_VERSION64 || hs != VAP64_HEADER_SIZE64) return VAP64_ERR_VERSION;
    if (nl == 0 || nl > VAP64_NAME_MAX64) return VAP64_ERR_NAME;
    if (n < hs + nl + 1u) return VAP64_ERR_SIZE;                 // 头+名字都放不下
    if (p[hs + nl - 1u] != 0) return VAP64_ERR_NAME;             // 名字必须 NUL 结尾
    for (uint32_t i = 0; i + 1u < nl; i++) {
        if (p[hs + i] == 0) return VAP64_ERR_NAME;               // 名字内部不能含 NUL
    }
    if (cs == 0 || cs > VAP64_MAX_CODE64) return VAP64_ERR_SIZE;
    if (eo != hs + nl) return VAP64_ERR_SIZE;                    // 代码入口 = 头+名字，绝不接受别的偏移
    if (n != hs + nl + cs) return VAP64_ERR_SIZE;                // 文件总长必须严丝合缝

    const uint8_t* code = p + eo;
    if (vap_crc32(code, cs) != crc) return VAP64_ERR_CRC;

    out->version     = version;
    out->header_size = hs;
    out->entry_offset= eo;
    out->code_size   = cs;
    out->code_crc    = crc;
    out->name_len    = nl;
    out->file_bytes  = n;
    out->name        = p + hs;
    out->code        = code;
    return VAP64_OK;
}

// ==================== 主分区起始 LBA（挂载参数）====================
uint32_t app64_main_part_lba64(int drive) {
    if (drive >= 0 && drive < 4) {
        static uint8_t sec[512];                                  // 512B 静态缓冲（不进镜像）
        if (ata64_read_sector(drive, 0, sec)) {
            for (int i = 0; i < 4; i++) {
                const uint8_t* e = sec + 446 + i * 16;
                if (e[4] != 0x07) continue;                       // 0x07 = 主数据分区（安装器写的就是它）
                const uint32_t start = vap_rd32(e + 8);
                if (start > 0) {
                    dbg64_line_begin64();
                    dbg64_str("[APP64] main partition drive=");
                    dbg64_dec((uint64_t)drive);
                    dbg64_str(" lba=");
                    dbg64_dec(start);
                    dbg64_str(" (from MBR type=0x07)\n");
                    dbg64_line_end64();
                    return start;
                }
            }
            dbg64_line_begin64();
            dbg64_str("[APP64] main partition: no type=0x07 entry, fallback PART_MAIN_LBA=");
            dbg64_dec(PART_MAIN_LBA);
            dbg64_nl();
            dbg64_line_end64();
        } else {
            dbg64_line_begin64();
            dbg64_str("[APP64] main partition: MBR read failed, fallback PART_MAIN_LBA=");
            dbg64_dec(PART_MAIN_LBA);
            dbg64_nl();
            dbg64_line_end64();
        }
    } else {
        dbg64_line_begin64();
        dbg64_str("[APP64] main partition: bad drive, fallback PART_MAIN_LBA=");
        dbg64_dec(PART_MAIN_LBA);
        dbg64_nl();
        dbg64_line_end64();
    }
    return PART_MAIN_LBA;
}

// ==================== 安装器（幂等；字节在系统卷/原始区，不在内核里）====================
int app64_install_builtin64(int drive, uint32_t part_lba) {
    uint32_t bytes = 0;
    const uint8_t* blob = demo64_blob_find64(VAP64_INSTALL_PATH64, &bytes);
    Vap64FileInfo vi;
    if (!blob || bytes == 0 || vap64_parse(blob, bytes, &vi) != VAP64_OK) {
        dbg64_line_begin64();
        dbg64_str("[APP64] install FAILED reason=blob\n");
        dbg64_line_end64();
        return -1;
    }

    // 卷可能没挂载（例如有人不经过启动路径直接调用）：先探**系统卷**根目录，没有就 vfs64_mount_system64
    // （挂/登记系统卷槽并激活它；已挂过就直接复用，绝不顶掉别的卷的槽）。
    // ★ 多卷（为什么不会写错卷）：安装目标是系统卷上的 /hello.vap —— 读写一律 *_on64(系统卷槽, …)：
    //   即使用户此刻正在浏览 D:，这次安装也只会落在 C: 上（on64 只在单次调用期间临时切卷，返回前切回）。
    uint32_t t = 0, sz = 0;
    {
        const int sys0 = vfs64_system_slot64();
        if (sys0 < 0 || vfs64_stat_on64(sys0, "/", &t, &sz) != 0) {
            if (vfs64_mount_system64(drive, part_lba) != 0) {
                dbg64_line_begin64();
                dbg64_str("[APP64] install FAILED reason=mount\n");
                dbg64_line_end64();
                return -1;
            }
        }
    }
    const int sys = vfs64_system_slot64();

    // 幂等：已存在就跳过，绝不重复写（重启动安全）
    if (vfs64_stat_on64(sys, VAP64_INSTALL_PATH64, &t, &sz) == 0) {
        dbg64_line_begin64();
        dbg64_str("[APP64] install skipped (exists) /hello.vap size=");
        dbg64_dec(sz);
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }

    const int w = vfs64_write_on64(sys, VAP64_INSTALL_PATH64, blob, (int)bytes);
    if (w != (int)bytes) {
        dbg64_line_begin64();
        dbg64_str("[APP64] install FAILED reason=write\n");
        dbg64_line_end64();
        return -1;
    }
    dbg64_line_begin64();
    dbg64_str("[APP64] install ok path=/hello.vap bytes=");
    dbg64_dec(bytes);
    dbg64_str(" crc=0x");
    vap_hex32(vi.code_crc);
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

// ==================== 启动器（从盘上读出来 -> ring3）====================
static int vap_launch_fail(const char* path, const char* reason) {
    dbg64_line_begin64();
    dbg64_str("[APP64] launch FAILED path=");
    dbg64_str(path ? path : "?");
    dbg64_str(" reason=");
    dbg64_str(reason);
    dbg64_nl();
    dbg64_line_end64();
    return -1;
}

int app64_launch64(const char* path) {
    if (!path || path[0] == 0) return vap_launch_fail(path, "arg");

    // 1) 先查属性拿"期望长度"：存在、是文件、不超过读缓冲（避免 vfs64_read 静默截断后误判）
    uint32_t type = 0, size = 0;
    if (vfs64_stat(path, &type, &size) != 0 || type != VFS64_TYPE_FILE) {
        return vap_launch_fail(path, "vfs");
    }
    if (size == 0 || size > VAP64_MAX_FILE_BYTES64) {
        return vap_launch_fail(path, "size");
    }

    // 2) 真的从文件系统读出来（不是内存里直接跑）
    const int n = vfs64_read(path, g_vap_file64, (int)sizeof(g_vap_file64));
    if (n != (int)size) return vap_launch_fail(path, "vfs");

    // 3) 校验 VAP64 头（magic/版本/名字边界/长度一致性/CRC32）
    Vap64FileInfo vi;
    const int prc = vap64_parse(g_vap_file64, (uint32_t)n, &vi);
    if (prc != VAP64_OK) return vap_launch_fail(path, vap_reason_text(prc));

    dbg64_line_begin64();
    dbg64_str("[APP64] load path=");
    dbg64_str(path);
    dbg64_str(" via=vfs64_read bytes=");
    dbg64_dec((uint64_t)n);
    dbg64_str(" code=");
    dbg64_dec(vi.code_size);
    dbg64_str(" name=");
    dbg64_str((const char*)vi.name);
    dbg64_str(" crc=0x");
    vap_hex32(vi.code_crc);
    dbg64_nl();
    dbg64_line_end64();

    // 4) 进 ring3 运行（代码段在用户页里，入口 = code 基址）
    const int urc = user64_run_blob64(vi.code, vi.code_size, (const char*)vi.name);
    if (urc != 0) return vap_launch_fail(path, "user64");

    dbg64_line_begin64();
    dbg64_str("[APP64] launch ok path=");
    dbg64_str(path);
    dbg64_str(" name=");
    dbg64_str((const char*)vi.name);
    dbg64_str(" rc=");
    dbg64_dec((uint64_t)urc);
    dbg64_nl();
    dbg64_line_end64();
    return urc;
}

// ==================== 按文件头魔数分派（终端 run）====================
int app64_run_any64(const char* path) {
    if (!path || path[0] == 0) return vap_launch_fail(path, "arg");
    uint8_t head[8];
    const int hn = vfs64_read(path, head, (int)sizeof(head));     // 只取头 8 字节判魔数
    if (hn <= 0) return vap_launch_fail(path, "vfs");

    if (elf64_is_elf64(head, (uint32_t)hn)) {
        dbg64_str("[APP64] dispatch path=");
        dbg64_str(path);
        dbg64_str(" magic=elf64 -> elf64_run64\n");
        return elf64_run64(path);
    }
    dbg64_str("[APP64] dispatch path=");
    dbg64_str(path);
    dbg64_str(" magic=vap64 -> app64_launch64\n");
    return app64_launch64(path);                                  // 头解析/CRC 在里面把关
}

// ==================== 自检 ====================
// 在内存里搭一个合法 VAP64（与 make_vap.py 同规则），供往返/负例用
static void vap_st_build(uint8_t* buf, uint32_t* out_n, const char* name,
                         const uint8_t* code, uint32_t code_n) {
    const uint32_t nl = vap_strlen(name) + 1u;                 // 含 NUL
    for (uint32_t i = 0; i < 8; i++) buf[i] = VAP64_MAGIC64[i];
    vap_wr32(buf + 8,  VAP64_VERSION64);
    vap_wr32(buf + 12, VAP64_HEADER_SIZE64);
    vap_wr32(buf + 16, VAP64_HEADER_SIZE64 + nl);              // entry_offset = 32 + name_len
    vap_wr32(buf + 20, code_n);
    vap_wr32(buf + 24, vap_crc32(code, code_n));
    vap_wr32(buf + 28, nl);
    uint32_t off = VAP64_HEADER_SIZE64;
    for (uint32_t i = 0; i < nl; i++) buf[off + i] = (uint8_t)name[i];   // name[i] 最后一个是 '\0'
    off += nl;
    for (uint32_t i = 0; i < code_n; i++) buf[off + i] = code[i];
    *out_n = off + code_n;
}

int app64_selftest64() {
    int fail = 0;
    static uint8_t buf[128];
    static const uint8_t code[8] = { 0xB8, 0x02, 0x00, 0x00, 0x00, 0xCD, 0x80, 0x00 };  // 自检数据
    uint32_t n = 0;
    Vap64FileInfo vi;

    // ---- bit0：CRC 与 zlib 同口径 + 头解析往返 ----
    if (vap_crc32((const uint8_t*)"123456789", 9) != 0xCBF43926u) fail |= 1;
    vap_st_build(buf, &n, "t", code, sizeof(code));
    if (vap64_parse(buf, n, &vi) != VAP64_OK) fail |= 1;
    else {
        if (vi.version != VAP64_VERSION64 || vi.header_size != VAP64_HEADER_SIZE64) fail |= 1;
        if (vi.name_len != 2 || vi.entry_offset != VAP64_HEADER_SIZE64 + 2u) fail |= 1;
        if (vi.code_size != (uint32_t)sizeof(code) || vi.code != buf + vi.entry_offset) fail |= 1;
        if (!vap_bytes_eq(vi.name, (const uint8_t*)"t", 2)) fail |= 1;
        if (vi.code_crc != vap_crc32(code, (uint32_t)sizeof(code))) fail |= 1;
        if (vi.file_bytes != n) fail |= 1;
    }

    // ---- bit1：坏 magic 被拒 ----
    vap_st_build(buf, &n, "t", code, sizeof(code));
    buf[0] = 'X';
    if (vap64_parse(buf, n, &vi) != VAP64_ERR_MAGIC) fail |= 2;

    // ---- bit2：坏 CRC 被拒（改代码段最后一个字节）----
    vap_st_build(buf, &n, "t", code, sizeof(code));
    buf[n - 1u] ^= 0xFFu;
    if (vap64_parse(buf, n, &vi) != VAP64_ERR_CRC) fail |= 4;

    // ---- bit3：长度不一致 / entry_offset 不一致被拒 ----
    vap_st_build(buf, &n, "t", code, sizeof(code));
    if (vap64_parse(buf, n - 1u, &vi) != VAP64_ERR_SIZE) fail |= 8;              // 少 1 字节
    vap_st_build(buf, &n, "t", code, sizeof(code));
    vap_wr32(buf + 16, VAP64_HEADER_SIZE64);                                     // entry_offset 不等于 32+name_len
    if (vap64_parse(buf, n, &vi) != VAP64_ERR_SIZE) fail |= 8;
    vap_st_build(buf, &n, "t", code, sizeof(code));
    vap_wr32(buf + 20, (uint32_t)sizeof(code) + 1u);                             // code_size 声明比实际大
    if (vap64_parse(buf, n, &vi) != VAP64_ERR_SIZE) fail |= 8;

    // ---- bit4：名字边界 ----
    vap_st_build(buf, &n, "t", code, sizeof(code));
    vap_wr32(buf + 28, 0u);                                                      // name_len = 0
    if (vap64_parse(buf, n, &vi) != VAP64_ERR_NAME) fail |= 16;
    vap_st_build(buf, &n, "t", code, sizeof(code));
    vap_wr32(buf + 28, VAP64_NAME_MAX64 + 1u);                                   // name_len > 24
    if (vap64_parse(buf, n, &vi) != VAP64_ERR_NAME) fail |= 16;
    vap_st_build(buf, &n, "t", code, sizeof(code));
    buf[VAP64_HEADER_SIZE64 + 2u - 1u] = 'x';                                    // 名字没 NUL 结尾
    if (vap64_parse(buf, n, &vi) != VAP64_ERR_NAME) fail |= 16;
    {
        char nm[VAP64_NAME_MAX64];
        for (uint32_t i = 0; i + 1u < VAP64_NAME_MAX64; i++) nm[i] = 'a';        // 23 字符 = 允许的最长
        nm[VAP64_NAME_MAX64 - 1u] = 0;
        vap_st_build(buf, &n, nm, code, sizeof(code));
        if (vap64_parse(buf, n, &vi) != VAP64_OK) fail |= 16;
    }

    // ---- bit5：交付给系统卷的 hello.vap 头部自校验（名字/CRC/entry_offset/代码指针）----
    //   ★ 本批：字节不在内核里了 —— 从构建期"原始区"取（见 kernel/demo64.h）；取不到就跳过
    //     这一位（构建期已有"探针/回读"两道断言）。
    {
        uint32_t blob_n = 0;
        const uint8_t* vblob = demo64_blob_find64(VAP64_INSTALL_PATH64, &blob_n);
        if (!vblob || blob_n == 0) {
            dbg64_line_begin64();
            dbg64_str("[APP64] selftest bit5 skipped (no hello.vap in blob region)\n");
            dbg64_line_end64();
        } else if (vap64_parse(vblob, blob_n, &vi) != VAP64_OK) {
            fail |= 32;
        } else {
            if (!vap_bytes_eq(vi.name, (const uint8_t*)"hello", 6)) fail |= 32;
            if (vi.code_size == 0 || vi.entry_offset != VAP64_HEADER_SIZE64 + 6u) fail |= 32;
            if (vi.code != vblob + vi.entry_offset) fail |= 32;
            if (vap_crc32(vi.code, vi.code_size) != vi.code_crc) fail |= 32;
        }
    }

    if (fail == 0) {
        dbg64_line_begin64();
        dbg64_str("[APP64] selftest PASS\n");
        dbg64_line_end64();
    } else {
        dbg64_line_begin64();
        dbg64_str("[APP64] selftest FAIL mask=");
        dbg64_dec((uint64_t)fail);
        dbg64_nl();
        dbg64_line_end64();
    }
    return fail;
}


// ============================================================================
// ★ 应用注册表（/etc/apps.json）—— 本文件新增（见 app64.h 顶部的格式说明）
//   谁写：① 构建期编译进内核的 kBuiltinApps64（内置应用）+ 启动期幂等落盘；
//         ② /bin/vpkg 安装时把用户应用**追加**一行（user/store/vs_pkg.c）；
//         ③ 桌面拖动结束时内核写回快捷方式位置（app64_registry_set_pos64）。
//   谁读：kernel/startmenu64.cpp（开始菜单全部应用列表 + 8 个固定格）与
//         kernel/desktopops64.cpp（桌面快捷方式）；都是"一处登记、两处读"。
// ============================================================================
#include "gui64.h"        // APP_ID_*（注册表里内置应用的 id 就是它）

struct AppRegRec64 {
    char name[APPREG64_NAME_MAX];       // 显示名（中文；空 -> 用 en）
    char en[APPREG64_NAME_MAX];
    char exec[APPREG64_EXEC_MAX];
    char icon[APPREG64_ICON_MAX];
    char alias[APPREG64_ALIAS_MAX];
    int  id;                            // APP_ID_* / -1
    int  builtin;
    int  desktop;
    int  x, y;
};

// 内置应用表：**构建期**编译进内核（唯一真源）。顺序 = 开始菜单固定网格 2x4 的顺序
// （前 8 条），与 P2 开始菜单原来的固定项**逐项一致**（终端/我的电脑/设置/计算器/扫雷/
// 任务管理器/系统监视器/关于），所以既有 startmenu64_test 的 tile/搜索断言不受影响。
// icon = kernel/icons64.h 的 kIcon64Names 名字（取不到就程序化绘制，绝不空）。
static const AppRegRec64 kBuiltinApps64[] = {
    { "终端",       "Terminal",       "builtin:term",     "terminal",    "term shell console",            APP_ID_TERM,     1, 0, 0, 0 },
    { "我的电脑",   "My Computer",    "builtin:mypc",     "mypc",        "mypc computer explorer files",  APP_ID_MYPC,     1, 0, 0, 0 },
    { "设置",       "Settings",       "builtin:settings", "settings",    "settings cfg control",          APP_ID_SETTINGS, 1, 0, 0, 0 },
    { "计算器",     "Calculator",     "builtin:calc",     "calc",        "calc calculator",               APP_ID_CALC,     1, 0, 0, 0 },
    { "扫雷",       "Minesweeper",    "builtin:mines",    "mines",       "mines mine game",               APP_ID_MINES,    1, 0, 0, 0 },
    { "任务管理器", "Task Manager",   "builtin:tmgr",     "tmgr",        "tmgr taskmgr process",          APP_ID_TMGR,     1, 0, 0, 0 },
    { "系统监视器", "System Monitor", "builtin:monitor",  "monitor-app", "monitor perf",                  APP_ID_MONITOR,  1, 0, 0, 0 },
    { "关于",       "About VimtuOS",  "builtin:about",    "about",       "about version",                 APP_ID_ABOUT,    1, 0, 0, 0 },
    { "回收站",     "Recycle Bin",    "builtin:recycle",  "recycle",     "recycle trash bin",             APP_ID_RECYCLE,  1, 0, 0, 0 },
    // 用户态程序（卷里的文件；不是内核窗口）——"内置软件"也有注册表条目，开始菜单必须能看到
    { "商店",       "App Store",      "/bin/store",       "plus",        "store shop vpkg appstore",      -1,              1, 0, 0, 0 },
    { "文本编辑器", "Text Editor",    "/bin/edit",        "files",       "edit editor text file tcc",     -1,              1, 0, 0, 0 },
    { "网络工具",   "Network Tools",  "/bin/netd",        "globe",       "netd network dhcp dns tcp",     -1,              1, 0, 0, 0 },
};
#define APPREG64_BUILTIN_N ((int)(sizeof(kBuiltinApps64) / sizeof(kBuiltinApps64[0])))

static AppRegRec64 g_apps64[APPREG64_MAX];
static int      g_apps_n = 0;
static bool     g_apps_tried = false;
static uint32_t g_apps_gen = 0;
static uint32_t g_apps_fnv = 0;
static bool     g_apps_dirty = false;      // 内存里改过（位置/桌面标志）-> 写盘
static uint8_t  g_apps_file[APPREG64_FILE_MAX];
static char     g_apps_out[APPREG64_FILE_MAX];
static int      g_apps_logged = 0;

// ---- 小工具 ----
static int ar_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static void ar_cpy(char* d, const char* s, int cap) {
    int i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static bool ar_eq(const char* a, const char* b) {
    int i = 0;
    if (!a || !b) return a == b;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}
static uint32_t ar_fnv(const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}
static void ar_put_dec(int v) { dbg64_dec((uint64_t)(v < 0 ? 0 : v)); }

// ---- JSON 行扫描器（只认 "key": 后面的引号串/整数；够了，理由见 app64.h）----
static const char* ar_find(const char* line, const char* key) {
    for (const char* p = line; p && *p; p++) {
        if (*p != '"') continue;
        const char* k = key;
        const char* q = p + 1;
        while (*k && *q == *k) { k++; q++; }
        if (*k || *q != '"') continue;
        const char* r = q + 1;
        while (*r == ' ' || *r == '\t') r++;
        if (*r != ':') continue;
        r++;
        while (*r == ' ' || *r == '\t') r++;
        return r;
    }
    return nullptr;
}
static int ar_str(const char* line, const char* key, char* out, int cap) {
    out[0] = 0;
    const char* p = ar_find(line, key);
    if (!p || *p != '"') return -1;
    p++;
    int n = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            const char e = *p++;
            if (e == 'n') c = '\n';
            else if (e == 't') c = '\t';
            else c = e;
        }
        if (n < cap - 1) out[n++] = c;
    }
    out[n] = 0;
    return 0;
}
static int ar_int(const char* line, const char* key, int def) {
    const char* p = ar_find(line, key);
    if (!p) return def;
    int v = 0, neg = 0, any = 0;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = 1; }
    if (!any) return def;
    return neg ? -v : v;
}

// ---- 系统卷（有挂载的卷才读写）----
static int ar_sys_slot64() {
    const int s = vfs64_system_slot64();
    if (s < 0 || !vfs64_slot_used64(s)) return -1;
    return s;
}

// ---- 序列化（一行一条；字段顺序固定，见 app64.h）----
static void ar_put_esc(char* b, int cap, int* o, const char* s) {
    if (*o < cap - 1) b[(*o)++] = '"';
    for (int i = 0; s && s[i] && *o < cap - 2; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { b[(*o)++] = '\\'; b[(*o)++] = (char)c; }
        else if (c < 0x20) { b[(*o)++] = ' '; }
        else b[(*o)++] = (char)c;
    }
    if (*o < cap - 1) b[(*o)++] = '"';
}
static void ar_put_num(char* b, int cap, int* o, int v) {
    char t[16];
    int n = 0;
    const bool neg = v < 0;
    unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    if (!u) t[n++] = '0';
    while (u) { t[n++] = (char)('0' + (u % 10u)); u /= 10u; }
    if (neg && *o < cap - 1) b[(*o)++] = '-';
    while (n > 0 && *o < cap - 1) b[(*o)++] = t[--n];
}
static void ar_put_kv(char* b, int cap, int* o, const char* k, const char* v, bool first) {
    if (!first && *o < cap - 1) b[(*o)++] = ',';
    ar_put_esc(b, cap, o, k);
    if (*o < cap - 1) b[(*o)++] = ':';
    ar_put_esc(b, cap, o, v ? v : "");
}
static void ar_put_kvi(char* b, int cap, int* o, const char* k, int v) {
    if (*o < cap - 1) b[(*o)++] = ',';
    ar_put_esc(b, cap, o, k);
    if (*o < cap - 1) b[(*o)++] = ':';
    ar_put_num(b, cap, o, v);
}
// 把内存里的整张表序列化成 JSONL（返回字节数；0 = 装不下，如实失败不写半截）
static int ar_serialize64() {
    int o = 0;
    const int cap = (int)sizeof(g_apps_out);
    for (int i = 0; i < g_apps_n; i++) {
        const AppRegRec64& a = g_apps64[i];
        if (o > cap - 512) return 0;
        g_apps_out[o++] = '{';
        ar_put_kv(g_apps_out, cap, &o, "name", a.name, true);
        ar_put_kv(g_apps_out, cap, &o, "en", a.en, false);
        ar_put_kv(g_apps_out, cap, &o, "exec", a.exec, false);
        ar_put_kv(g_apps_out, cap, &o, "icon", a.icon, false);
        ar_put_kv(g_apps_out, cap, &o, "alias", a.alias, false);
        ar_put_kvi(g_apps_out, cap, &o, "id", a.id);
        ar_put_kvi(g_apps_out, cap, &o, "builtin", a.builtin);
        ar_put_kvi(g_apps_out, cap, &o, "desktop", a.desktop);
        ar_put_kvi(g_apps_out, cap, &o, "x", a.x);
        ar_put_kvi(g_apps_out, cap, &o, "y", a.y);
        if (o < cap - 2) { g_apps_out[o++] = '}'; g_apps_out[o++] = '\n'; }
    }
    if (o < cap - 1) g_apps_out[o] = 0;
    return o;
}

// 写盘（整表重写；卷不可写/装不下时如实打点，绝不改内存语义）
static int ar_write64(const char* why) {
    const int n = ar_serialize64();
    if (n <= 0) {
        dbg64_line_begin64();
        dbg64_str("[APPREG64] write FAILED reason=too-big entries=");
        ar_put_dec(g_apps_n);
        dbg64_nl();
        dbg64_line_end64();
        return -1;
    }
    const int sys = ar_sys_slot64();
    if (sys < 0) {
        dbg64_line_begin64();
        dbg64_str("[APPREG64] write skip reason=no-volume entries=");
        ar_put_dec(g_apps_n);
        dbg64_str(" (memory-only; next boot re-seeds from the kernel builtin table)\n");
        dbg64_line_end64();
        return -1;
    }
    panic64_watchdog_pause64();                            // 要写盘：临时停表（与 terminal/fd64 同款）
    const int wr = vfs64_write_on64(sys, APPREG64_PATH, g_apps_out, n);
    panic64_watchdog_unpause64();
    dbg64_line_begin64();
    dbg64_str("[APPREG64] write path=");
    dbg64_str(APPREG64_PATH);
    dbg64_str(" entries=");
    ar_put_dec(g_apps_n);
    dbg64_str(" bytes=");
    ar_put_dec(n);
    dbg64_str(" written=");
    ar_put_dec(wr);
    dbg64_str(" ok=");
    ar_put_dec(wr == n ? 1 : 0);
    dbg64_str(" why=");
    dbg64_str(why ? why : "-");
    dbg64_nl();
    dbg64_line_end64();
    g_apps_dirty = false;
    return wr == n ? 0 : -1;
}

static void ar_seed64() {
    for (int i = 0; i < APPREG64_BUILTIN_N && i < APPREG64_MAX; i++) {
        g_apps64[i] = kBuiltinApps64[i];
    }
    g_apps_n = APPREG64_BUILTIN_N;
}

// 读一行（行尾的 \r 去掉）；返回行长
static int ar_line64(const char* p, int n, char* out, int cap) {
    int i = 0;
    while (i < n && p[i] != '\n' && p[i] != '\r' && i < cap - 1) { out[i] = p[i]; i++; }
    out[i] = 0;
    return i;
}

// 解析注册表文件。**内存表先=内置表**，文件里：
//   * builtin=1 的行 -> 只取"可覆盖字段"（desktop/x/y；名字/exec/图标以内核表为准，
//     vpkg 不会写 builtin 行）；
//   * builtin=0 的行 -> 追加（按 exec 去重）。
// 返回文件行数（0 = 空/无文件），*ok 写 1 = 解析成功。
static int ar_parse64(const uint8_t* buf, int n, int* ok) {
    if (ok) *ok = 1;
    ar_seed64();
    int i = 0, lines = 0;
    char line[APPREG64_FILE_MAX];
    while (i < n) {
        const int used = ar_line64((const char*)buf + i, n - i, line, (int)sizeof(line));
        i += used;
        while (i < n && (buf[i] == '\n' || buf[i] == '\r')) i++;
        if (line[0] != '{') continue;                    // 空行/注释：跳过
        lines++;
        char name[APPREG64_NAME_MAX], en[APPREG64_NAME_MAX], exec[APPREG64_EXEC_MAX];
        char icon[APPREG64_ICON_MAX], alias[APPREG64_ALIAS_MAX];
        ar_str(line, "name", name, (int)sizeof(name));
        ar_str(line, "en", en, (int)sizeof(en));
        ar_str(line, "exec", exec, (int)sizeof(exec));
        ar_str(line, "icon", icon, (int)sizeof(icon));
        ar_str(line, "alias", alias, (int)sizeof(alias));
        const int id = ar_int(line, "id", -1);
        const int builtin = ar_int(line, "builtin", 0);
        const int desktop = ar_int(line, "desktop", 0);
        const int x = ar_int(line, "x", 0);
        const int y = ar_int(line, "y", 0);
        if (!exec[0]) { if (ok) *ok = 0; continue; }     // 没有 exec 的行：不认
        if (builtin) {
            int hit = -1;
            for (int k = 0; k < g_apps_n; k++) {
                if (g_apps64[k].builtin && (g_apps64[k].id == id || ar_eq(g_apps64[k].exec, exec))) { hit = k; break; }
            }
            if (hit >= 0) {                              // 只接受"可覆盖字段"
                g_apps64[hit].desktop = desktop ? 1 : 0;
                g_apps64[hit].x = x;
                g_apps64[hit].y = y;
            }
            continue;
        }
        if (g_apps_n >= APPREG64_MAX) { if (ok) *ok = 0; break; }
        AppRegRec64& a = g_apps64[g_apps_n];
        ar_cpy(a.name, name[0] ? name : en, APPREG64_NAME_MAX);
        ar_cpy(a.en, en[0] ? en : name, APPREG64_NAME_MAX);
        ar_cpy(a.exec, exec, APPREG64_EXEC_MAX);
        ar_cpy(a.icon, icon, APPREG64_ICON_MAX);
        ar_cpy(a.alias, alias, APPREG64_ALIAS_MAX);
        a.id = -1;
        a.builtin = 0;
        a.desktop = desktop ? 1 : 0;
        a.x = x; a.y = y;
        g_apps_n++;
    }
    return lines;
}

static void ar_log_app64(int i, const char* tag) {
    const AppRegRec64& a = g_apps64[i];
    dbg64_line_begin64();
    dbg64_str(tag);
    dbg64_str(" idx=");
    ar_put_dec(i);
    dbg64_str(" app=");
    ar_put_dec(a.id);                       // 内置 = APP_ID_*；用户应用 = -1
    dbg64_str(" name=");
    dbg64_str(a.name[0] ? a.name : a.en);
    dbg64_str(" en=");
    dbg64_str(a.en);
    dbg64_str(" exec=");
    dbg64_str(a.exec);
    dbg64_str(" icon=");
    dbg64_str(a.icon[0] ? a.icon : "-");
    dbg64_str(" builtin=");
    ar_put_dec(a.builtin);
    dbg64_str(" desktop=");
    ar_put_dec(a.desktop);
    dbg64_str(" pos=");
    ar_put_dec(a.x); dbg64_str(","); ar_put_dec(a.y);
    dbg64_nl();
    dbg64_line_end64();
}

static void ar_log_all64(const char* tag) {
    for (int i = 0; i < g_apps_n; i++) ar_log_app64(i, tag);
}

// 真正读一次卷里的文件；返回 1 = 文件存在且解析过（内存已重建）
static int ar_reload64(int force_log) {
    int lines = 0, ok = 1;
    const int sys = ar_sys_slot64();
    if (sys >= 0) {
        panic64_watchdog_pause64();                        // 要读盘：临时停表（与 terminal/fd64 同款）
        const int got = vfs64_read_on64(sys, APPREG64_PATH, g_apps_file, (int)sizeof(g_apps_file));
        panic64_watchdog_unpause64();
        if (got > 0) {
            g_apps_fnv = ar_fnv(g_apps_file, (uint32_t)got);
            lines = ar_parse64(g_apps_file, got, &ok);
        } else {
            g_apps_fnv = 0;                       // 没有文件：内存 = 内置表
            ar_seed64();
        }
    } else {
        g_apps_fnv = 0;
        ar_seed64();
    }
    if (force_log || !g_apps_logged) {
        g_apps_logged = 1;
        int nd = 0;
        for (int i = 0; i < g_apps_n; i++) if (g_apps64[i].desktop) nd++;
        dbg64_line_begin64();
        dbg64_str("[APPREG64] init path=");
        dbg64_str(APPREG64_PATH);
        dbg64_str(" src=");
        dbg64_str(lines > 0 ? "vfs" : "builtin");
        dbg64_str(" lines=");
        ar_put_dec(lines);
        dbg64_str(" ok=");
        ar_put_dec(ok);
        dbg64_str(" entries=");
        ar_put_dec(g_apps_n);
        dbg64_str(" builtin=");
        ar_put_dec(APPREG64_BUILTIN_N);
        dbg64_str(" desktop=");
        ar_put_dec(nd);
        dbg64_nl();
        dbg64_line_end64();
        ar_log_all64("[APPREG64] app");
    }
    return lines > 0 ? 1 : 0;
}

int app64_registry_init64() {
    if (g_apps_tried) return g_apps_n;
    g_apps_tried = true;
    const int had = ar_reload64(1);
    // 幂等落盘：卷里没有（或空的）-> 用内置表建一份（"构建期写进该表"的落盘动作）
    if (!had) (void)ar_write64("seed-builtin");
    return g_apps_n;
}

int app64_registry_refresh64() {
    if (!g_apps_tried) return app64_registry_init64();
    if (g_apps_dirty) return 0;                 // 自己有未落盘的改动：先别用盘上版本盖掉
    const int sys = ar_sys_slot64();
    if (sys < 0) return 0;
    // ★ 下面这几步要**真读盘**（stat + 整文件读 -> VFS -> ATA）：临时停表 GUI 看门狗
    //   （与 terminal64 的 cmd_ls / fd64 的 Fd64WdtGuard64 同款）。实测：一旦 ATA 走
    //   "IRQ14 超时 -> 回退轮询"，每次事务 ≈160 ms，一次注册表读就能把 5 秒心跳预算吃光
    //   -> [WD64] watchdog fire -> [PANIC64] stop=WATCHDOG_TIMEOUT（source=gui64-frame）。
    //   调用方（桌面帧 desktopops64_tick64 / 开始菜单）本来就在心跳路径上，长 I/O 不是"卡死"。
    int full = 0;
    uint32_t fnv = 0;
    {
        panic64_watchdog_pause64();
        uint32_t type = 0, size = 0;
        if (vfs64_stat_on64(sys, APPREG64_PATH, &type, &size) == 0 &&
            size > 0 && size <= (uint32_t)sizeof(g_apps_file)) {
            full = vfs64_read_on64(sys, APPREG64_PATH, g_apps_file, (int)sizeof(g_apps_file));
            if (full > 0) fnv = ar_fnv(g_apps_file, (uint32_t)full);
        }
        panic64_watchdog_unpause64();
    }
    if (full <= 0) return 0;
    if (fnv == g_apps_fnv) return 0;
    int lines = 0, ok = 1;
    lines = ar_parse64(g_apps_file, full, &ok);
    g_apps_fnv = fnv;
    g_apps_gen++;
    dbg64_line_begin64();
    dbg64_str("[APPREG64] reload src=vfs lines=");
    ar_put_dec(lines);
    dbg64_str(" ok=");
    ar_put_dec(ok);
    dbg64_str(" entries=");
    ar_put_dec(g_apps_n);
    dbg64_str(" gen=");
    ar_put_dec((int)g_apps_gen);
    dbg64_str(" (user install/remove observed)");
    dbg64_nl();
    dbg64_line_end64();
    ar_log_all64("[APPREG64] app");
    return 1;
}

uint32_t app64_registry_gen64() { return g_apps_gen; }
int app64_registry_count64() { if (!g_apps_tried) (void)app64_registry_init64(); return g_apps_n; }

static const AppRegRec64* ar_at64(int i) {
    if (!g_apps_tried) (void)app64_registry_init64();
    if (i < 0 || i >= g_apps_n) return nullptr;
    return &g_apps64[i];
}
const char* app64_reg_name64(int i)  { const AppRegRec64* a = ar_at64(i); return a ? (a->name[0] ? a->name : a->en) : ""; }
const char* app64_reg_en64(int i)    { const AppRegRec64* a = ar_at64(i); return a ? a->en : ""; }
const char* app64_reg_exec64(int i)  { const AppRegRec64* a = ar_at64(i); return a ? a->exec : ""; }
const char* app64_reg_icon64(int i)  { const AppRegRec64* a = ar_at64(i); return a ? a->icon : ""; }
const char* app64_reg_alias64(int i) { const AppRegRec64* a = ar_at64(i); return a ? a->alias : ""; }
int app64_reg_id64(int i)      { const AppRegRec64* a = ar_at64(i); return a ? a->id : -1; }
int app64_reg_builtin64(int i) { const AppRegRec64* a = ar_at64(i); return a ? a->builtin : 0; }
int app64_reg_desktop64(int i) { const AppRegRec64* a = ar_at64(i); return a ? a->desktop : 0; }
void app64_reg_pos64(int i, int* x, int* y) {
    const AppRegRec64* a = ar_at64(i);
    if (x) *x = a ? a->x : 0;
    if (y) *y = a ? a->y : 0;
}
int app64_reg_find_exec64(const char* exec) {
    if (!exec || !exec[0]) return -1;
    if (!g_apps_tried) (void)app64_registry_init64();
    for (int i = 0; i < g_apps_n; i++) if (ar_eq(g_apps64[i].exec, exec)) return i;
    return -1;
}
int app64_reg_set_desktop64(int i, int on) {
    AppRegRec64* a = (AppRegRec64*)ar_at64(i);
    if (!a) return -1;
    const int v = on ? 1 : 0;
    if (a->desktop == v) return 0;
    a->desktop = v;
    g_apps_gen++;
    g_apps_dirty = true;
    dbg64_line_begin64();
    dbg64_str("[APPREG64] desktop idx=");
    ar_put_dec(i);
    dbg64_str(" name=");
    dbg64_str(a->name[0] ? a->name : a->en);
    dbg64_str(" on=");
    ar_put_dec(v);
    dbg64_nl();
    dbg64_line_end64();
    return ar_write64(on ? "shortcut-add" : "shortcut-remove");
}
int app64_reg_set_pos64(int i, int x, int y) {
    AppRegRec64* a = (AppRegRec64*)ar_at64(i);
    if (!a) return -1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (a->x == x && a->y == y) return 0;
    a->x = x; a->y = y;
    g_apps_gen++;
    g_apps_dirty = true;
    dbg64_line_begin64();
    dbg64_str("[APPREG64] pos idx=");
    ar_put_dec(i);
    dbg64_str(" name=");
    dbg64_str(a->name[0] ? a->name : a->en);
    dbg64_str(" to=");
    ar_put_dec(x); dbg64_str(","); ar_put_dec(y);
    dbg64_str(" persisted=1");
    dbg64_nl();
    dbg64_line_end64();
    return ar_write64("icon-move");
}

// 纯逻辑自检：内置表非空、字段齐全、exec 唯一、内置 id 与顺序符合开始菜单固定网格
int app64_reg_selftest64() {
    int fail = 0;
    if (APPREG64_BUILTIN_N < 8) fail |= 1;
    if (kBuiltinApps64[0].id != APP_ID_TERM) fail |= 2;            // 固定网格第 0 格 = 终端
    if (!ar_eq(kBuiltinApps64[1].exec, "builtin:mypc")) fail |= 4;
    for (int i = 0; i < APPREG64_BUILTIN_N; i++) {
        if (!kBuiltinApps64[i].en[0] || !kBuiltinApps64[i].exec[0] || !kBuiltinApps64[i].icon[0]) fail |= 8;
        for (int k = i + 1; k < APPREG64_BUILTIN_N; k++)
            if (ar_eq(kBuiltinApps64[i].exec, kBuiltinApps64[k].exec)) fail |= 16;
    }
    if (ar_len(kBuiltinApps64[0].name) == 0) fail |= 32;
    dbg64_line_begin64();
    dbg64_str("[APPREG64] selftest ");
    dbg64_str(fail == 0 ? "PASS" : "FAIL");
    dbg64_str(" mask=");
    ar_put_dec(fail);
    dbg64_str(" builtin=");
    ar_put_dec(APPREG64_BUILTIN_N);
    dbg64_nl();
    dbg64_line_end64();
    return fail;
}
