/* user/apps/sounder.c - ★ 系统音效：用户态播放器（/bin/sounder）
 *
 * 交付方式：**系统卷里的文件 /bin/sounder**（内核里不含它的字节；见 build64.sh 的那条探针断言）。
 * 它做的事情（任务书 ③ 的"用户态播放器"）：
 *   1) 从系统卷读 WAV（默认 /usr/share/sounds/<name>.wav，name = startup|notify|click|error），
 *      在**用户态**解析 RIFF 头（fmt/data），量出 采样率/声道/位深/帧数/峰值/PCM 校验和；
 *   2) 只把 **48kHz/16bit/2ch** 的 PCM 交给内核的音频 ABI（int 0x80 号 49 `audio_play`）——
 *      格式判断、缓冲分配、选哪段素材、播不播，全是本程序（用户态）的事；
 *   3) 播放是**阻塞**的：内核把它交给 hda64 的流（<=8KiB 分块、有界等待），返回时这一段已经
 *      送进控制器。本程序把这次调用**耗掉多少 ms** 也打出来（对照素材时长 = 真的出声了）。
 *
 * 用法（argv 由**用户态 shell** 的 `run /bin/sounder …` 传入；内核终端的 elfrun 不带参数，
 * 那时等价于 `sounder` 无参 = demo）：
 *   sounder                  demo：按 startup -> notify -> click -> error 依次播放（间隔 300 ms）
 *   sounder list             列出 4 段素材（路径/大小/格式/帧数/时长/峰值/校验和；不需要声卡）
 *   sounder <name>           播放 /usr/share/sounds/<name>.wav
 *   sounder /path/x.wav      播放任意路径
 *   sounder selftest         负例自检：坏指针/空指针/错格式/零长度/超大长度 + 一条合法调用
 *
 * 为什么自己实现一套最小 runtime（不 include 任何头、不链 user/lib）：与 /bin/drvdemo 同一条纪律
 * —— 另一条线在改 user/lib 与 user/shell，本文件不依赖它们就不会被它们的中间态带崩；交付形态就是
 * "卷里的一份静态 ELF"。
 *
 * ABI（与 kernel/syscall64.h 的"系统音效"一节逐字对应）：
 *   int 0x80 自有 ABI：rax=49 audio_play(rdi=pcm_va, rsi=frames, rdx=format)
 *     format 只认 0x11（48kHz/16bit/2ch）；frames 1..96000（2 秒）；pcm_va 必须是用户可读的
 *     frames*4 字节。返回 0 = 已送完；-2 EFAULT / -3 EINVAL / -4 EAGAIN / -5 ENODEV。
 *   另外用 int 0x80 的 1 write(1) / 2 exit / 4 ticks / 5 sleep_ms / 6 open / 7 read / 8 close，
 *   用 syscall 指令的 9 mmap（给 PCM 缓冲拿一块 192 KiB 的匿名区）。
 *
 * 打点（自动验收 tests/sounds64_test.py grep，格式勿改）：
 *   SOUNDER list name=<n> path=<p> bytes=<n> rate=<n> ch=<n> bits=<n> frames=<n> ms=<n> peak=<n> sum=0x<hex>
 *   SOUNDER list done ok=<n> fail=<n>
 *   SOUNDER play name=<n> path=<p> rate=<n> ch=<n> bits=<n> frames=<n> bytes=<n> ms=<n> peak=<n> sum=0x<hex> buf=0x<hex> rc=<signed> elapsed_ms=<n>
 *   SOUNDER reject name=<n> reason=<not-found|short|bad-riff|bad-fmt|too-long|no-mem> rc=<signed>
 *   SOUNDER demo count=<n> rc=<signed>
 *   SOUNDER neg <what> rc=<signed>
 *   SOUNDER selftest done neg=<n> ok=<n> rc=<signed>
 *
 * ★ 如实边界（不是"混音器"）：单流、单文件、阻塞播放；不做混音/重采样/循环/音量控制（音量/静音
 *   走既有的 audio 命令与声音面板 = 内核 hda64 的放大器，用户态不重复实现）；不支持 8/24/32-bit、
 *   非 48 kHz、单声道（本地就拒，不让内核看到不认识的格式）。
 */
typedef signed long    i64;
typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

/* ==================== 系统调用包装（与 /bin/drvdemo 同款：第 4 个参数走 r10）==================== */
static i64 sc4(i64 nr, i64 a, i64 b, i64 c, i64 d) {
    i64 r;
    __asm__ volatile("movq %[d], %%r10\n\t"
                     "int $0x80"
                     : "=a"(r)
                     : "a"(nr), "D"(a), "S"(b), "d"(c), [d] "r"(d)
                     : "r10", "rcx", "r11", "memory");
    return r;
}
static i64 sc0(i64 nr) { return sc4(nr, 0, 0, 0, 0); }
static i64 sc1(i64 nr, i64 a) { return sc4(nr, a, 0, 0, 0); }
static i64 sc3(i64 nr, i64 a, i64 b, i64 c) { return sc4(nr, a, b, c, 0); }

/* Linux ABI（syscall 指令）：给 PCM 缓冲要一块匿名内存（mmap 9） */
static i64 lx6(i64 nr, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5, i64 a6) {
    i64 r;
    register i64 r10 __asm__("r10") = a4;
    register i64 r8  __asm__("r8")  = a5;
    register i64 r9  __asm__("r9")  = a6;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

/* 主逻辑（定义在文件末；_start 的汇编直接 call 它，所以是外部链接 + 先声明） */
int snd_main(long argc, char** argv);

/* ==================== 常量（与 kernel/syscall64.h 的"系统音效"段一致）==================== */
#define SND_NR          49
#define SND_FMT_48K16S2 0x11
#define SND_MAX_FRAMES  96000u                 /* 2 秒上限（内核侧同一个上限） */
#define SND_E_EFAULT    (-2)                   /* 指针非法 */
#define SND_E_EINVAL    (-3)                   /* 格式/长度非法 */
#define SND_E_EAGAIN    (-4)                   /* 单流忙 / 流超时 */
#define SND_E_ENODEV    (-5)                   /* 没有声卡 */

#define SND_DIR      "/usr/share/sounds/"
#define SND_BUF_BYTES (192u * 1024u)           /* 最大素材 134,444 B + 余量 */
#define RD_CHUNK     4096                      /* 自有 ABI read(7) 的单次上限口径 */
#define GAP_MS       300                       /* demo 里两段之间的静音间隔 */

static const char* const g_names[4] = { "startup", "notify", "click", "error" };

/* ==================== 最小输出（自有 ABI write(1) -> 串口 + 屏幕）==================== */
static void out_str(const char* s) {
    i64 n = 0;
    while (s[n]) n++;
    if (n) (void)sc3(1, 1, (i64)(u64)s, n);
}
static void out_u64(u64 v) {
    char b[24];
    int n = 0, k = 0;
    char t[24];
    if (v == 0) b[n++] = '0';
    while (v) { t[k++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    while (k) b[n++] = t[--k];
    b[n] = 0;
    out_str(b);
}
static void out_hex(u64 v) {                   /* 0x + 小写十六进制（与内核打点同款；小值只打低 32 位） */
    static const char* H = "0123456789abcdef";
    char b[19];
    b[0] = '0';
    b[1] = 'x';
    for (int i = 0; i < 16; i++) b[2 + i] = H[(v >> ((15 - i) * 4)) & 0xF];
    b[18] = 0;
    if ((v >> 32) != 0) {
        out_str(b);
        return;
    }
    char c[11];
    c[0] = '0';
    c[1] = 'x';
    for (int i = 0; i < 8; i++) c[2 + i] = H[(v >> ((7 - i) * 4)) & 0xF];
    c[10] = 0;
    out_str(c);
}
static void out_signed(i64 v) {
    if (v < 0) { out_str("-"); out_u64((u64)(-v)); } else out_u64((u64)v);
}
static void out_0x32(u32 v) {                  /* sum=0x<8 位十六进制> */
    static const char* H = "0123456789abcdef";
    char hb[11];
    hb[0] = '0';
    hb[1] = 'x';
    for (int i = 0; i < 8; i++) hb[2 + i] = H[(v >> ((7 - i) * 4)) & 0xF];
    hb[10] = 0;
    out_str(hb);
}
static void kv(const char* k, u64 v) { out_str(k); out_u64(v); }
static void ks(const char* k, const char* s) { out_str(k); out_str(s); }

/* ==================== 小工具 ==================== */
static u32 rd16(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 rd32(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static int ceq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static u32 fnv1a32(const u8* p, u64 n) {
    u32 h = 0x811C9DC5u;
    for (u64 i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}
static void sleep_ms(i64 ms) { (void)sc1(5, ms); }

/* ==================== WAV 解析（用户态策略：只接受 48k/16bit/2ch）==================== */
typedef struct Wav {
    const u8* pcm;                 /* data 区起点 */
    u64       pcm_bytes;
    u32       rate, ch, bits;
    int       bad;                 /* 0 = OK；1 = 不是 RIFF/WAVE；2 = 头不完整；3 = 格式不支持 */
} Wav;

static void wav_parse(const u8* buf, u64 n, Wav* w) {
    w->pcm = 0;
    w->pcm_bytes = 0;
    w->rate = w->ch = w->bits = 0;
    w->bad = 0;
    if (n < 44) { w->bad = 2; return; }
    if (rd32(buf) != 0x46464952u /*'RIFF'*/ || rd32(buf + 8) != 0x45564157u /*'WAVE'*/) {
        w->bad = 1;
        return;
    }
    u64 i = 12;
    const u8* fmt = 0;
    u32 fmt_len = 0;
    while (i + 8 <= n) {
        const u32 id = rd32(buf + i);
        const u32 sz = rd32(buf + i + 4);
        const u64 body = i + 8;
        if (id == 0x20746D66u /*'fmt '*/ && body + sz <= n) { fmt = buf + body; fmt_len = sz; }
        if (id == 0x61746164u /*'data'*/) {                 /* data 的 sz 可能被写坏（实时写盘的文件） */
            w->pcm = buf + body;
            w->pcm_bytes = (body + sz <= n) ? sz : (n - body);
            break;
        }
        i = body + sz + (sz & 1u);
    }
    if (!fmt || fmt_len < 16 || !w->pcm) { w->bad = 2; return; }
    if (rd16(fmt) != 1u) { w->bad = 3; return; }            /* 1 = PCM */
    w->ch = rd16(fmt + 2);
    w->rate = rd32(fmt + 4);
    w->bits = rd16(fmt + 14);
    if (w->ch != 2u || w->rate != 48000u || w->bits != 16u) { w->bad = 3; return; }
    w->pcm_bytes &= ~3ull;                                  /* 只留整帧（4 字节/帧） */
}

static u32 pcm_peak(const u8* p, u64 n) {
    u32 peak = 0;
    for (u64 i = 0; i + 1 < n; i += 2) {
        i64 v = (i64)(short)((u32)p[i] | ((u32)p[i + 1] << 8));
        if (v < 0) v = -v;
        if ((u32)v > peak) peak = (u32)v;
    }
    return peak;
}

/* ==================== 读一个文件到缓冲（自有 ABI 6/7/8）==================== */
static i64 read_file(const char* path, u8* dst, u64 cap, u64* out_n) {
    const i64 fd = sc1(6, (i64)(u64)path);
    if (fd < 0) return -1;                                  /* 打开失败（不存在/不是文件） */
    u64 n = 0;
    for (;;) {
        if (n >= cap) break;                                /* 缓冲满：按"截断"处理，调用方按 cap 判 */
        u64 want = cap - n;
        if (want > RD_CHUNK) want = RD_CHUNK;
        const i64 got = sc3(7, fd, (i64)(u64)(dst + n), (i64)want);
        if (got < 0) { (void)sc1(8, fd); return -2; }
        if (got == 0) break;                                /* EOF */
        n += (u64)got;
    }
    (void)sc1(8, fd);
    *out_n = n;
    return 0;
}

/* ==================== 播放一段内存 PCM（唯一的内核调用点）==================== */
static i64 abi_play(const u8* pcm, u64 frames, u64 fmt) {
    return sc3(SND_NR, (i64)(u64)pcm, (i64)frames, (i64)fmt);
}

static void print_reject(const char* name, const char* reason, i64 rc) {
    out_str("SOUNDER reject name=");
    out_str(name);
    ks(" reason=", reason);
    out_str(" rc=");
    out_signed(rc);
    out_str("\n");
}

/* ==================== 播放一个 wav 文件 ==================== */
static i64 play_path(const char* name, const char* path, u8* buf) {
    u64 n = 0;
    const i64 rr = read_file(path, buf, SND_BUF_BYTES, &n);
    if (rr != 0 || n == 0) {
        print_reject(name, "not-found", SND_E_EFAULT);
        return SND_E_EFAULT;
    }
    Wav w;
    wav_parse(buf, n, &w);
    if (w.bad) {
        print_reject(name, (w.bad == 1) ? "bad-riff" : ((w.bad == 2) ? "short" : "bad-fmt"),
                     SND_E_EINVAL);
        return SND_E_EINVAL;
    }
    const u64 frames = w.pcm_bytes / 4u;
    if (frames == 0 || frames > (u64)SND_MAX_FRAMES) {
        print_reject(name, "too-long", SND_E_EINVAL);
        return SND_E_EINVAL;
    }
    const u32 peak = pcm_peak(w.pcm, w.pcm_bytes);
    const u32 sum  = fnv1a32(w.pcm, w.pcm_bytes);
    const u64 t0 = (u64)sc0(4);                             /* ticks()：250Hz PIT */
    const i64 rc = abi_play(w.pcm, frames, SND_FMT_48K16S2);
    const u64 t1 = (u64)sc0(4);
    out_str("play name=");
    out_str(name);
    ks(" path=", path);
    kv(" rate=", w.rate);
    kv(" ch=", w.ch);
    kv(" bits=", w.bits);
    kv(" frames=", frames);
    kv(" bytes=", w.pcm_bytes);
    kv(" ms=", frames * 1000u / (u64)w.rate);
    kv(" peak=", peak);
    out_str(" sum=");
    out_0x32(sum);
    out_str(" buf=");
    out_hex((u64)(u64)w.pcm);
    out_str(" rc=");
    out_signed(rc);
    kv(" elapsed_ms=", (t1 - t0) * 4u);
    out_str("\n");
    return rc;
}

static void wav_path_of(char* dst, u64 cap, const char* name) {
    u64 k = 0;
    const char* a = SND_DIR;
    while (a[k] && k < cap - 1) { dst[k] = a[k]; k++; }
    for (u64 i = 0; name[i] && k < cap - 6; i++) dst[k++] = name[i];
    const char* ext = ".wav";
    for (int i = 0; ext[i] && k < cap - 1; i++) dst[k++] = ext[i];
    dst[k] = 0;
}

/* ==================== 模式：list ==================== */
static void mode_list(u8* buf, i64* out_ok, i64* out_fail) {
    i64 ok = 0, fail = 0;
    for (int i = 0; i < 4; i++) {
        char path[64];
        wav_path_of(path, sizeof(path), g_names[i]);
        u64 n = 0;
        const i64 rr = read_file(path, buf, SND_BUF_BYTES, &n);
        if (rr != 0 || n == 0) {
            ks("SOUNDER list name=", g_names[i]);
            ks(" path=", path);
            out_str(" rc=");
            out_signed(SND_E_EFAULT);
            out_str("\n");
            fail++;
            continue;
        }
        Wav w;
        wav_parse(buf, n, &w);
        out_str("SOUNDER list name=");
        out_str(g_names[i]);
        ks(" path=", path);
        kv(" bytes=", n);
        kv(" rate=", w.rate);
        kv(" ch=", w.ch);
        kv(" bits=", w.bits);
        kv(" frames=", w.pcm_bytes / 4u);
        kv(" ms=", w.bad ? 0 : (w.pcm_bytes / 4u) * 1000u / (u64)w.rate);
        kv(" peak=", w.bad ? 0 : pcm_peak(w.pcm, w.pcm_bytes));
        out_str(" sum=");
        out_0x32(w.bad ? 0u : fnv1a32(w.pcm, w.pcm_bytes));
        kv(" rc=", (u64)(w.bad ? 1u : 0u));
        out_str("\n");
        if (w.bad) fail++;
        else ok++;
    }
    *out_ok = ok;
    *out_fail = fail;
}

/* ==================== 模式：selftest（负例 + 一条合法调用）==================== */
static i64 mode_selftest(u8* buf) {
    i64 neg = 0, ok = 0;
    struct { const char* what; i64 rc; } t[6];

    /* ① 用户窗口外的指针（0x1000 是内核镜像区）-> -EFAULT */
    t[0].what = "badptr";
    t[0].rc = abi_play((const u8*)(u64)0x1000, 480, SND_FMT_48K16S2);
    /* ② NULL 指针 -> -EFAULT */
    t[1].what = "nullptr";
    t[1].rc = abi_play((const u8*)0, 480, SND_FMT_48K16S2);
    /* ③ 格式不对（0x99）-> -EINVAL */
    t[2].what = "badfmt";
    t[2].rc = abi_play(buf, 4800, 0x99);
    /* ④ 零长度 -> -EINVAL */
    t[3].what = "zeroframes";
    t[3].rc = abi_play(buf, 0, SND_FMT_48K16S2);
    /* ⑤ 超大长度（> 2 秒上限）-> -EINVAL */
    t[4].what = "hugeframes";
    t[4].rc = abi_play(buf, 1000000, SND_FMT_48K16S2);
    /* ⑥ 合法：480 帧（10 ms）静音 -> 0 */
    for (u64 i = 0; i < 480u * 4u; i++) buf[i] = 0;
    t[5].what = "ok480";
    t[5].rc = abi_play(buf, 480, SND_FMT_48K16S2);

    for (int i = 0; i < 6; i++) {
        ks("SOUNDER neg ", t[i].what);
        out_str(" rc=");
        out_signed(t[i].rc);
        out_str("\n");
        if (i == 5) { if (t[i].rc == 0) ok++; }
        else neg++;
    }
    out_str("SOUNDER selftest done neg=");
    out_u64((u64)neg);
    out_str(" ok=");
    out_u64((u64)ok);
    out_str(" rc=");
    out_signed((t[5].rc == 0) ? 0 : t[5].rc);
    out_str("\n");
    return (t[5].rc == 0) ? 0 : t[5].rc;
}

/* ==================== 主逻辑（argv 由 _start 传进来）==================== */
int snd_main(long argc, char** argv) {                    /* 外部链接：_start 的汇编直接 call 它 */
    const i64 va = lx6(9 /*mmap*/, 0, SND_BUF_BYTES, 3 /*READ|WRITE*/, 0, 0, 0);
    if (va < 0) {
        print_reject("unknown", "no-mem", SND_E_EFAULT);
        return 1;
    }
    u8* buf = (u8*)(u64)va;

    const char* a1 = (argc > 1 && argv && argv[1]) ? argv[1] : 0;
    i64 rc = 0;

    if (a1 && ceq(a1, "list")) {
        i64 ok = 0, fail = 0;
        mode_list(buf, &ok, &fail);
        out_str("SOUNDER list done ok=");
        out_u64((u64)ok);
        out_str(" fail=");
        out_u64((u64)fail);
        out_str("\n");
        rc = (fail == 0) ? 0 : 1;
    } else if (a1 && ceq(a1, "selftest")) {
        rc = mode_selftest(buf);
    } else if (a1 && a1[0] == '/') {
        rc = play_path("path", a1, buf);
    } else if (a1) {
        char path[64];
        wav_path_of(path, sizeof(path), a1);
        rc = play_path(a1, path, buf);
    } else {
        /* 无参 = demo：4 段依次播放（间隔 GAP_MS），给内核终端 elfrun 一条"一次听全"的路 */
        i64 count = 0;
        for (int i = 0; i < 4; i++) {
            char path[64];
            wav_path_of(path, sizeof(path), g_names[i]);
            const i64 r = play_path(g_names[i], path, buf);
            if (r == 0) count++;
            if (i < 3) sleep_ms(GAP_MS);
        }
        out_str("SOUNDER demo count=");
        out_u64((u64)count);
        out_str(" rc=");
        out_signed((count == 4) ? 0 : -1);
        out_str("\n");
        rc = (count == 4) ? 0 : 1;
    }
    (void)sc1(2, (rc == 0) ? 0 : 1);                        /* exit(code)：0 = 全部成功 */
    return 0;
}

/* 入口：从初始栈取 argc/argv（内核 ELF64 装载器按 [argc][argv..][NULL][envp NULL] 建栈）。
 * 必须落在 .text.start（user64.ld 里排第一），且与 /bin/drvdemo 用同一套链接脚本。 */
__asm__(".section .text.start,\"ax\"\n"
        ".globl _start\n"
        ".type _start,@function\n"
        "_start:\n"
        "  movq (%rsp), %rdi\n"                 /* argc */
        "  leaq 8(%rsp), %rsi\n"                /* argv */
        "  andq $-16, %rsp\n"                   /* 调用前对齐 16（不依赖 fp） */
        "  call snd_main\n"
        "1: jmp 1b\n"                           /* snd_main 里 exit 不会返回；兜底不自旋出界 */
        ".previous\n");
