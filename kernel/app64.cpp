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

// 内嵌的 hello.vap（build64.sh：nasm -> tools/make_vap.py -> objcopy -I binary 嵌进内核）。
// 符号名由 objcopy 按输入路径生成：_binary_build64_hello_vap_start/_end（从仓库根执行才稳定）。
extern "C" const uint8_t _binary_build64_hello_vap_start[];
extern "C" const uint8_t _binary_build64_hello_vap_end[];

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

// ==================== 安装器（幂等）====================
int app64_install_builtin64(int drive, uint32_t part_lba) {
    const uint32_t bytes = (uint32_t)(_binary_build64_hello_vap_end - _binary_build64_hello_vap_start);
    Vap64FileInfo vi;
    if (vap64_parse(_binary_build64_hello_vap_start, bytes, &vi) != VAP64_OK) {
        dbg64_line_begin64();
        dbg64_str("[APP64] install FAILED reason=blob\n");
        dbg64_line_end64();
        return -1;
    }

    // 卷可能没挂载（例如有人不经过启动路径直接调用）：先探根目录，没有就按 (drive, part_lba) 挂一次。
    uint32_t t = 0, sz = 0;
    if (vfs64_stat("/", &t, &sz) != 0) {
        if (vfs64_mount(drive, part_lba) != 0) {
            dbg64_line_begin64();
            dbg64_str("[APP64] install FAILED reason=mount\n");
            dbg64_line_end64();
            return -1;
        }
    }

    // 幂等：已存在就跳过，绝不重复写（重启动安全）
    if (vfs64_stat(VAP64_INSTALL_PATH64, &t, &sz) == 0) {
        dbg64_line_begin64();
        dbg64_str("[APP64] install skipped (exists) /hello.vap size=");
        dbg64_dec(sz);
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }

    const int w = vfs64_write(VAP64_INSTALL_PATH64, _binary_build64_hello_vap_start, (int)bytes);
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

    // ---- bit5：内嵌 hello.vap 的头部自校验（名字/CRC/entry_offset/代码指针）----
    {
        const uint32_t blob_n = (uint32_t)(_binary_build64_hello_vap_end - _binary_build64_hello_vap_start);
        if (vap64_parse(_binary_build64_hello_vap_start, blob_n, &vi) != VAP64_OK) {
            fail |= 32;
        } else {
            if (!vap_bytes_eq(vi.name, (const uint8_t*)"hello", 6)) fail |= 32;
            if (vi.code_size == 0 || vi.entry_offset != VAP64_HEADER_SIZE64 + 6u) fail |= 32;
            if (vi.code != _binary_build64_hello_vap_start + vi.entry_offset) fail |= 32;
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
