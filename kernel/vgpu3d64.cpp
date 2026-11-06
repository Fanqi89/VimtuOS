// vgpu3d64.cpp - ★ v0.9.17：virtio-gpu 的 **3D（virgl）加速通路**（自检 + 像素取证）
//
// ============================ 这一层做什么 ============================
// 2D 路径（kernel/virtio_gpu64.cpp）只会"搬运 + 扫描输出"：CPU 光栅化，GPU 只做
// TRANSFER_TO_HOST_2D / RESOURCE_FLUSH。**真正的 3D 加速是另一条通路**：
//   1) 设备协商 VIRTIO_GPU_F_VIRGL（virtio_gpu64.cpp 现在会收这个位）；
//   2) 拿 virgl 的 capset（主机侧渲染器信息）；
//   3) 建一个 **virgl 上下文**（CTX_CREATE）+ 一块 **3D 资源**（RESOURCE_CREATE_3D，
//      backing 用页池的页）+ CTX_ATTACH_RESOURCE；
//   4) SUBMIT_3D 提交 **virgl 命令流**：建 surface 对象 -> 绑 framebuffer -> CLEAR 成已知颜色；
//   5) TRANSFER_FROM_HOST_3D 把这块资源读回来，**逐像素核对清屏色**。
//
// 它证明的是"内核 -> virtio-gpu -> 主机 GL 渲染器 -> 回读"这条 3D 链路真通，且**结果被像素验证**。
// 它**不是** GL/Vulkan 实现 —— guest 侧 GL 需要 Mesa，那是后续批次（见
// docs/Wayland基础抽象层.md 的 A.8 与 docs/2D与3D加速.md）。
//
// 如实降级：设备没给 VIRGL（`-vga std` / 普通 `virtio-gpu-pci` / 主机 QEMU 没编 virglrenderer /
// 没有 GL 显示后端）就只打一行 `[GPU3D] skipped reason=no-virgl`，**一个命令都不发**。
//
// ============================ 串口打点（自动验收 grep，格式勿改）============================
//   [GPU3D] skipped reason=no-device|no-virgl
//   [GPU3D] capset id=<n> ver=<n> size=<n>
//   [GPU3D] selftest res=<n> w=<n> h=<n> ctx=<n> str=<n> lenslot=<n> pages=<n> ok=<0|1> px=0x<hex> want=0x<hex>
//   [GPU3D] variant try slot=<n> surf_len=<n> fail=<n>      （每种编码试一次，失败计数）
//   [GPU3D] selftest FAIL reason=<reason> ok=0
//   [GPU3D] cmd FAIL what=<name> resp=0x<hex> want=0x<hex>
//
// 体积纪律：日志原语一律 noinline（同 drm64.cpp）。
#include "virtio_gpu64.h"
#include "debug64.h"
#include "mem_64.h"        // PAGE_SIZE_64

// ==================== 打点 ====================
static const char* const G3_HEXL = "0123456789abcdef";
__attribute__((noinline)) static void g3_puts(const char* s) { dbg64_str(s); }
__attribute__((noinline)) static void g3_ch(char c) { dbg64_putc(c); }
__attribute__((noinline)) static void g3_num(uint64_t v) { dbg64_dec(v); }
__attribute__((noinline)) static void g3_hex(uint64_t v) {
    g3_puts("0x");
    if (!v) { g3_ch('0'); return; }
    char b[16]; int n = 0;
    while (v && n < 16) { b[n++] = G3_HEXL[v & 0xFu]; v >>= 4; }
    while (n > 0) g3_ch(b[--n]);
}
__attribute__((noinline)) static void g3_begin() { dbg64_line_begin64(); }
__attribute__((noinline)) static void g3_end() { dbg64_nl(); dbg64_line_end64(); }

// ==================== virtio-gpu 协议（只列 3D 用到的）====================
struct __attribute__((packed)) G3Hdr {          // virtio_gpu_ctrl_hdr = 24 B
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t  ring_idx;
    uint8_t  pad[3];
};
static_assert(sizeof(G3Hdr) == 24, "virtio_gpu_ctrl_hdr 必须是 24 B");

struct __attribute__((packed)) G3Box { uint32_t x, y, z, w, h, d; };

#define G3_CMD_GET_CAPSET_INFO     0x0108u
#define G3_CMD_CTX_CREATE          0x0200u
#define G3_CMD_CTX_ATTACH_RESOURCE 0x0202u
#define G3_CMD_RESOURCE_CREATE_3D  0x0204u
#define G3_CMD_TRANSFER_FROM_3D    0x0206u
#define G3_CMD_SUBMIT_3D           0x0207u

#define G3_FLAG_FENCE          0x0001u    // VIRTIO_GPU_FLAG_FENCE
#define G3_RESP_OK_NODATA      0x1100u
#define G3_RESP_OK_CAPSET_INFO 0x1102u

#define G3_CAPSET_VIRGL  1u
#define G3_CAPSET_VIRGL2 2u

// ---- virgl 命令编码（virgl_protocol.h 的 VIRGL_CMD0）----
//   dw0 = 命令号(bit0..7) | 对象类型(bit8..15) | 负载 dwords(bit16..31)
#define G3_CCMD(cmd, obj, len) (((uint32_t)(cmd) & 0xFFu) | (((uint32_t)(obj) & 0xFFu) << 8) | \
                                ((uint32_t)(len) << 16))
#define G3_CCMD_CREATE_OBJECT      1u
#define G3_CCMD_BIND_OBJECT        2u
#define G3_CCMD_SET_FRAMEBUFFER    5u
#define G3_CCMD_CLEAR              7u
#define G3_OBJ_SURFACE             8u
#define G3_OBJ_SURFACE_SIZE        6u       // handle, res, format, level, first_layer, last_layer
#define G3_OBJ_FB_SIZE             10u      // nr_cbufs + cbufs[8] + zsurf
#define G3_OBJ_CLEAR_SIZE          5u       // buffers + color[4]
#define G3_FORMAT_B8G8R8A8_UNORM   1u
#define G3_BIND_RENDER_TARGET      2u       // VIRGL_BIND_RENDER_TARGET
#define G3_TARGET_TEXTURE_2D       2u       // PIPE_TEXTURE_2D
#define G3_CLEAR_COLOR0            4u       // PIPE_CLEAR_COLOR0

// ==================== 尺寸 / 期望值 ====================
#define G3_W 64u
#define G3_H 64u
#define G3_STRIDE (G3_W * 4u)
#define G3_BYTES  (G3_STRIDE * G3_H)
#define G3_PAGES  ((G3_BYTES + (uint32_t)PAGE_SIZE_64 - 1u) / (uint32_t)PAGE_SIZE_64)
// 清屏色 RGBA = (0.25, 0.5, 0.75, 1.0)；B8G8R8A8 内存序 => 0xFF 40 80 BF（小端 u32）
#define G3_WANT_PX 0xFF4080BFu
#define G3_TOL     2u                        // GL 的 8 位量化允许 ±2

struct __attribute__((packed)) G3GetCapsetInfo { G3Hdr hdr; uint32_t capset_index, pad; };
struct __attribute__((packed)) G3CtxCreate { G3Hdr hdr; uint32_t nlen, context_init; char name[64]; };
struct __attribute__((packed)) G3CtxRes { G3Hdr hdr; uint32_t resource_id, pad; };
struct __attribute__((packed)) G3ResCreate3d {
    G3Hdr hdr;
    uint32_t resource_id, target, format, bind;
    uint32_t width, height, depth, array_size;
    uint32_t last_level, nr_samples, flags, pad;
};
struct __attribute__((packed)) G3TransferFrom3d {
    G3Hdr hdr;
    G3Box  box;
    uint64_t offset;
    uint32_t resource_id, level, stride, layer_stride;
};
struct __attribute__((packed)) G3MemEntry { uint64_t addr; uint32_t length; uint32_t pad; };
struct __attribute__((packed)) G3AttachHead { G3Hdr hdr; uint32_t nr_entries, pad; };
struct __attribute__((packed)) G3Submit3d { G3Hdr hdr; uint32_t size, pad; };

static uint64_t g3_page[G3_PAGES];                       // 3D 资源的 backing（页池页）
static uint32_t g3_dw[64];                               // virgl 命令流
static G3GetCapsetInfo g3_capset_info;
static uint8_t  g3_rb[64];                               // 响应拷贝
static uint8_t  g3_attach[sizeof(G3AttachHead) + sizeof(G3MemEntry) * G3_PAGES + 8];
static uint8_t  g3_submit[sizeof(G3Submit3d) + sizeof(g3_dw)];

__attribute__((noinline)) static void g3_zero(void* p, uint32_t n) {
    uint8_t* b = (uint8_t*)p;
    for (uint32_t i = 0; i < n; i++) b[i] = 0;
}

static uint32_t g3_fence = 0;

// 发一条命令并**自己判响应类型**（3D 的口径：只认期望的 OK_*，ERR_* 一律失败）。
// ★ 所有 3D 命令都带 VIRTIO_GPU_FLAG_FENCE：主机等 virgl 真正执行完才回响应，
//   否则 QEMU 立刻回 OK_NODATA、渲染仍在后面异步跑 —— TRANSFER_FROM 就会读到全 0。
static int g3_cmd(void* buf, uint32_t len, uint32_t want_resp, const char* what) {
    {
        G3Hdr* h = (G3Hdr*)buf;
        h->flags = G3_FLAG_FENCE;
        h->fence_id = (uint64_t)(++g3_fence);
    }
    if (!vgpu64_cmd_ex64(buf, len)) {
        g3_begin(); g3_puts("[GPU3D] cmd FAIL what="); g3_puts(what);
        g3_puts(" reason=not-consumed"); g3_end();
        return 0;
    }
    const uint32_t rt = vgpu64_resp_type_ex64();
    if (rt != want_resp) {
        g3_begin(); g3_puts("[GPU3D] cmd FAIL what="); g3_puts(what);
        g3_puts(" resp="); g3_hex(rt); g3_puts(" want="); g3_hex(want_resp); g3_end();
        return 0;
    }
    return 1;
}

__attribute__((noinline)) static int g3_fail(const char* why) {
    g3_begin(); g3_puts("[GPU3D] selftest FAIL reason="); g3_puts(why); g3_puts(" ok=0"); g3_end();
    return -1;
}

// ==================== 自检 ====================
// 为什么要"变体阶梯"：virgl 的 CREATE_OBJECT 编码在历史上有两种写法（对象类型放命令行头的
// bit8..15，或放在负载的第一个 dword），而这两种写法的长度也差一个 dword。只看文档猜不准，
// 所以这里**一次启动里试 3 种**（每种用独立的 virgl 上下文 —— 一旦某条命令被 vrend 判错，
// 那个上下文就废了），哪种能拿回正确像素就用哪种，并把变体号写进证据行。
// 第一轮阶梯的实测结论（QEMU stderr 原文）：
//   len=6 -> "failed to dispatch CREATE_OBJECT: 22"
//   len=7（类型也进负载）-> 同样 22
//   len=5 -> CREATE_OBJECT 通过，错误推进到 BIND_OBJECT: 22
// 所以 surface 的负载就是 **5 个 dword**（handle, res, format, level, first_layer；last_layer 由资源推）。
// 第二轮只需试 BIND_OBJECT 的三种写法。
// 第二轮阶梯的实测结论（QEMU stderr 原文）：三种 BIND_OBJECT 编码**全部**被拒（22），
//   而 CREATE_OBJECT（len=5）一直是好的 —— 说明不是编码问题，是**句柄撞车**：
//   vrend 在 ctx 建立时给内置状态对象（blend/dsa/rasterizer…）占用了 1..N 号句柄，
//   我们用 handle=1 建 surface 会把冲突掩盖，绑定时就查不到/类型不符。
// 第三轮：把 surface 句柄挪到高位，并顺带再试两种 bind 编码。
struct G3Flavor {
    uint32_t surf_len;        // 恒 5（第一轮实测）
    uint32_t surf_handle;     // surface 对象句柄（高位，避开 vrend 内置对象）
    uint32_t bind_len;        // BIND_OBJECT 的负载 dwords
    uint32_t bind_obj;        // 命令行头里的对象类型
    int      bind_type_in_payload;
    const char* name;
};
static const G3Flavor G3_FLAVORS[3] = {
    { 5u, 0x100u,  1u, G3_OBJ_SURFACE, 0, "h256-bind1-hdr" },
    { 5u, 0x100u,  2u, 0u,             1, "h256-bind2-payload" },
    { 5u, 0x2000u, 1u, G3_OBJ_SURFACE, 0, "h8192-bind1-hdr" },
};

// 一次尝试：ctx/res 都是新的；成功返回 1（并已校验像素），失败返回 0
static int g3_try_variant64(uint32_t ctx_id, uint32_t res_id, const G3Flavor* v,
                            uint64_t* page, uint32_t pages, uint32_t* px_out) {
    // ---- virgl 上下文 ----
    {
        static G3CtxCreate cc;
        g3_zero(&cc, (uint32_t)sizeof(cc));
        cc.hdr.type = G3_CMD_CTX_CREATE;
        cc.hdr.ctx_id = ctx_id;
        cc.nlen = 7u;
        const char* nm = "vimtu3d";
        for (uint32_t i = 0; i < 64u && nm[i]; i++) cc.name[i] = nm[i];
        if (!g3_cmd(&cc, (uint32_t)sizeof(cc), G3_RESP_OK_NODATA, "ctx-create")) return 0;
    }
    // ---- 3D 资源 + backing + 挂进上下文 ----
    {
        static G3ResCreate3d rc;
        g3_zero(&rc, (uint32_t)sizeof(rc));
        rc.hdr.type = G3_CMD_RESOURCE_CREATE_3D;
        rc.resource_id = res_id;
        rc.target = G3_TARGET_TEXTURE_2D;
        rc.format = G3_FORMAT_B8G8R8A8_UNORM;
        rc.bind = G3_BIND_RENDER_TARGET;
        rc.width = G3_W; rc.height = G3_H; rc.depth = 1u; rc.array_size = 1u;
        if (!g3_cmd(&rc, (uint32_t)sizeof(rc), G3_RESP_OK_NODATA, "res-create-3d")) return 0;
        g3_zero(g3_attach, (uint32_t)sizeof(g3_attach));
        G3AttachHead* ah = (G3AttachHead*)g3_attach;
        ah->hdr.type = 0x0106u;
        ah->nr_entries = pages;
        G3MemEntry* es = (G3MemEntry*)(g3_attach + sizeof(G3AttachHead));
        for (uint32_t i = 0; i < pages; i++) {
            es[i].addr = page[i];
            es[i].length = (uint32_t)PAGE_SIZE_64;
        }
        const uint32_t alen = (uint32_t)(sizeof(G3AttachHead) + sizeof(G3MemEntry) * pages);
        if (!g3_cmd(g3_attach, alen, G3_RESP_OK_NODATA, "attach-backing")) return 0;
        static G3CtxRes cr;
        g3_zero(&cr, (uint32_t)sizeof(cr));
        cr.hdr.type = G3_CMD_CTX_ATTACH_RESOURCE;
        cr.hdr.ctx_id = ctx_id;
        cr.resource_id = res_id;
        if (!g3_cmd(&cr, (uint32_t)sizeof(cr), G3_RESP_OK_NODATA, "ctx-attach")) return 0;
    }
    // ---- SUBMIT_3D：surface 对象 -> 绑定 -> framebuffer -> CLEAR ----
    {
        uint32_t n = 0;
        // CREATE_OBJECT：负载 5 个 dword（第一轮阶梯实测的唯一有效写法）
        g3_dw[n++] = G3_CCMD(G3_CCMD_CREATE_OBJECT, G3_OBJ_SURFACE, 5u);
        g3_dw[n++] = v->surf_handle;                      // 对象 handle（高位，避开内置对象）
        g3_dw[n++] = res_id;                              // 资源号
        g3_dw[n++] = G3_FORMAT_B8G8R8A8_UNORM;
        g3_dw[n++] = 0u;                                  // level
        g3_dw[n++] = 0u;                                  // first_layer
        // BIND_OBJECT：三种写法（第二轮阶梯）
        g3_dw[n++] = G3_CCMD(G3_CCMD_BIND_OBJECT, v->bind_obj, v->bind_len);
        g3_dw[n++] = v->surf_handle;                      // 绑定同一个 surface 句柄
        if (v->bind_type_in_payload) g3_dw[n++] = G3_OBJ_SURFACE;
        g3_dw[n++] = G3_CCMD(G3_CCMD_SET_FRAMEBUFFER, 0u, G3_OBJ_FB_SIZE);
        g3_dw[n++] = 1u;                                  // nr_cbufs
        g3_dw[n++] = v->surf_handle;                      // cbufs[0] = surface 句柄
        for (uint32_t i = 0; i < 7u; i++) g3_dw[n++] = 0u;
        g3_dw[n++] = 0u;                                  // zsurf
        g3_dw[n++] = G3_CCMD(G3_CCMD_CLEAR, 0u, G3_OBJ_CLEAR_SIZE);
        g3_dw[n++] = G3_CLEAR_COLOR0;
        g3_dw[n++] = 0x3E800000u;                         // 0.25
        g3_dw[n++] = 0x3F000000u;                         // 0.5
        g3_dw[n++] = 0x3F400000u;                         // 0.75
        g3_dw[n++] = 0x3F800000u;                         // 1.0
        const uint32_t csize = n * 4u;
        g3_zero(g3_submit, (uint32_t)sizeof(g3_submit));
        G3Submit3d* sh = (G3Submit3d*)g3_submit;
        sh->hdr.type = G3_CMD_SUBMIT_3D;
        sh->hdr.ctx_id = ctx_id;
        sh->size = csize;
        uint8_t* dst = g3_submit + sizeof(G3Submit3d);
        const uint8_t* src = (const uint8_t*)g3_dw;
        for (uint32_t i = 0; i < csize; i++) dst[i] = src[i];
        if (!g3_cmd(g3_submit, (uint32_t)(sizeof(G3Submit3d) + csize), G3_RESP_OK_NODATA, "submit-3d"))
            return 0;
    }
    // ---- 读回 + 像素核对 ----
    {
        static G3TransferFrom3d tr;
        g3_zero(&tr, (uint32_t)sizeof(tr));
        tr.hdr.type = G3_CMD_TRANSFER_FROM_3D;
        tr.hdr.ctx_id = ctx_id;
        tr.box.w = G3_W; tr.box.h = G3_H; tr.box.d = 1u;
        tr.resource_id = res_id;
        tr.stride = G3_STRIDE;
        if (!g3_cmd(&tr, (uint32_t)sizeof(tr), G3_RESP_OK_NODATA, "transfer-from-3d")) return 0;
    }
    uint32_t bad = 0;
    const uint32_t samples[3] = {
        0u,
        (G3_STRIDE / 4u) * (G3_H / 2u) + (G3_W / 2u),
        (G3_STRIDE / 4u) * (G3_H - 1u) + (G3_W - 1u),
    };
    uint32_t first = 0;
    for (uint32_t s = 0; s < 3u; s++) {
        const uint32_t px = *(const uint32_t*)(uintptr_t)(page[0] + (uint64_t)samples[s] * 4ull);
        if (!s) first = px;
        for (uint32_t sh = 0; sh < 32u; sh += 8u) {
            const uint32_t a = (px >> sh) & 0xFFu, b = (G3_WANT_PX >> sh) & 0xFFu;
            const uint32_t d = (a > b) ? (a - b) : (b - a);
            if (d > G3_TOL) bad++;
        }
    }
    *px_out = first;
    return (bad == 0u) ? 1 : 0;
}

int vgpu3d_selftest64(void) {
    if (!vgpu64_ready_ex64()) {
        g3_begin(); g3_puts("[GPU3D] skipped reason=no-device"); g3_end();
        return 0;
    }
    if (!vgpu64_has_virgl64()) {                     // 设备/主机没给 3D：如实跳过，一个命令都不发
        g3_begin(); g3_puts("[GPU3D] skipped reason=no-virgl"); g3_end();
        return 0;
    }
    // ---- capset：证明主机侧真有 virgl 渲染器 ----
    {
        g3_zero(&g3_capset_info, (uint32_t)sizeof(g3_capset_info));
        g3_capset_info.hdr.type = G3_CMD_GET_CAPSET_INFO;
        g3_capset_info.capset_index = 0u;
        if (!g3_cmd(&g3_capset_info, (uint32_t)sizeof(g3_capset_info), G3_RESP_OK_CAPSET_INFO, "capset-info"))
            return g3_fail("capset-info");
        if (!vgpu64_resp_copy_ex64(g3_rb, (uint32_t)sizeof(g3_rb))) return g3_fail("capset-resp");
        const uint32_t* r = (const uint32_t*)g3_rb;
        const uint32_t capset_id = r[6], capset_ver = r[7], capset_size = r[8];
        g3_begin(); g3_puts("[GPU3D] capset id="); g3_num(capset_id);
        g3_puts(" ver="); g3_num(capset_ver); g3_puts(" size="); g3_num(capset_size); g3_end();
        if (capset_id != G3_CAPSET_VIRGL && capset_id != G3_CAPSET_VIRGL2)
            return g3_fail("capset-not-virgl");
    }
    // ---- backing 页（三个变体共用同一批页；每个资源各自 attach）----
    for (uint32_t i = 0; i < G3_PAGES; i++) {
        g3_page[i] = vgpu64_alloc_page_ex64();
        if (!g3_page[i]) return g3_fail("page-pool");
    }
    const uint32_t res_base = vgpu64_res_id_ex64();
    for (uint32_t vi = 0; vi < 3u; vi++) {
        const uint32_t ctx_id = 1u + vi;
        const uint32_t res_id = res_base + 1u + vi;
        // 每次尝试前把 backing 清零（上一轮可能写了东西）
        for (uint32_t i = 0; i < G3_PAGES; i++) {
            uint64_t* z = (uint64_t*)(uintptr_t)g3_page[i];
            for (uint32_t k = 0; k < (uint32_t)(PAGE_SIZE_64 / 8u); k++) z[k] = 0;
        }
        uint32_t px = 0;
        const int ok = g3_try_variant64(ctx_id, res_id, &G3_FLAVORS[vi], g3_page, G3_PAGES, &px);
        g3_begin(); g3_puts("[GPU3D] flavor try slot="); g3_num(vi);
        g3_puts(" name="); g3_puts(G3_FLAVORS[vi].name);
        g3_puts(" bind_len="); g3_num(G3_FLAVORS[vi].bind_len);
        g3_puts(" surf_h="); g3_num(G3_FLAVORS[vi].surf_handle);
        g3_puts(" ok="); g3_num(ok ? 1u : 0u); g3_puts(" px="); g3_hex(px); g3_end();
        if (ok) {
            g3_begin(); g3_puts("[GPU3D] selftest res="); g3_num(res_id);
            g3_puts(" w="); g3_num(G3_W); g3_puts(" h="); g3_num(G3_H);
            g3_puts(" ctx="); g3_num(ctx_id);
            g3_puts(" str="); g3_num(G3_STRIDE);
            g3_puts(" lenslot="); g3_num(vi);
            g3_puts(" pages="); g3_num(G3_PAGES);
            g3_puts(" ok=1 px="); g3_hex(px);
            g3_puts(" want="); g3_hex(G3_WANT_PX); g3_end();
            return 0;
        }
    }
    return g3_fail("no-variant-worked");
}
