// virtio_gpu64.h - virtio-gpu（2D）驱动对外接口（现代 virtio 1.0+ PCI）
//
// 定位与边界（先读，避免误期待；实现细节与踩坑见 virtio_gpu64.cpp 顶部）：
//   * **只做 2D 命令集**：GET_DISPLAY_INFO / RESOURCE_CREATE_2D / RESOURCE_UNREF /
//     RESOURCE_ATTACH_BACKING / RESOURCE_DETACH_BACKING / TRANSFER_TO_HOST_2D /
//     SET_SCANOUT / RESOURCE_FLUSH（+ 自检用的 TRANSFER_FROM_HOST_2D）。
//     **不做** 3D / virgl（VIRTIO_GPU_F_VIRGL 一概不协商）、不做 cursor 平面（queue 1 只报告、
//     不启用 —— 没有鼠标平面资源生命周期管理，见报告"没做到"）。
//   * framebuffer 作为**一个 resource**：宽高 = 设备给的显示分辨率（与 LFB 不一致时按 LFB 兜底并打点），
//     格式 = B8G8R8X8_UNORM（与内核后备缓冲 0xAARRGGBB / LFB 的 XRGB 逐字节一致），
//     backing = **后备缓冲的物理页**（每页一条 mem_entry）。
//   * 上屏路径：TRANSFER_TO_HOST_2D(脏矩形) + RESOURCE_FLUSH(脏矩形)；
//     队列**纯轮询 + 有界超时**（g_ticks64 计时 + 自旋上限双保险，绝不挂死）。
//   * 没有设备 / 初始化失败 / 运行期连续超时 -> **如实降级**：`vgpu64_blit64` 返回 0，
//     kernel/fb.cpp 原样走既有软件 LFB 路径（行为与改动前完全一致）；降级时会尝试
//     SET_SCANOUT(resource_id=0) 把显示交还给 legacy VGA framebuffer。
//
// 串口打点（自动验收按行 grep，格式别改；行锁保证不插行）：
//   [VGPU] not found (no virtio-gpu PCI device) -> backend=soft-lfb
//   [VGPU] pci 0:1.0 ven=0x1af4 dev=0x1050 sub=0x10
//   [VGPU] cap common=0x...(+0x... len=...) notify=... device=... isr=... mul=...
//   [VGPU] feature dev=0x00000000:00000000 ok=0x00000001:00000000 status=0xf
//   [VGPU] queue0 size=256 notify_off=0 desc=0x... avail=0x... used=0x...
//   [VGPU] queue1 size=64 (cursor; not enabled)
//   [VGPU] display info 1024x768 enabled=1 mode=b8g8r8x8 (lfb=1024x768 zoom=100)
//   [VGPU] res id=1 1024x768 fmt=b8g8r8x8 entries=192 backing=0x... bytes=3145728
//   [VGPU] cmd type=0x100 name=GET_DISPLAY_INFO used=1 timeouts=0
//   [VGPU] cmd timeout type=0x... name=TRANSFER_TO_HOST_2D used=0 timeouts=1
//   [VGPU] ready backend=virtio-gpu-2d
//   [VGPU] selftest stage=soft|device|erase x=.. y=.. w=.. h=.. hold_ms=..
//   [VGPU] selftest PASS dev_blits=.. soft_blits=.. hops=..
//   [VGPU] selftest skipped reason=no-device
//   [VGPU] selftest error reason=<...>
//   [VGPU] bench kind=full|region px=.. runs=.. soft_us=.. dev_us=.. ratio_x100=..
//   [VGPU] disable reason=<...> -> backend=soft-lfb
//   [VGPU] blit x=.. y=.. w=.. h=.. dev=1|0
//   [FB64] backend=virtio-gpu-2d  /  [FB64] backend=soft-lfb
#pragma once
#include <stdint.h>

// 驱动摘要（只读；未初始化时全 0 / found=0）。给设置页"关于"、硬件检查报告与验收脚本用。
struct Vgpu64Info {
    int      found;              // 1 = PCI 上认到了 virtio-gpu（含只认到 legacy 的情况）
    int      ready;              // 1 = 到 DRIVER_OK 且 resource 已绑定（设备路径可用）
    int      legacy_only;        // 1 = 只有 legacy 接口（本批不实现 -> 如实降级 soft-lfb）
    uint8_t  bus, dev, fn;
    uint16_t vendor, device, subsys;
    uint64_t common_pa, common_len;      // COMMON_CFG 的 BAR 绝对地址 + 长度
    uint64_t notify_pa, notify_len;      // NOTIFY_CFG
    uint64_t device_pa, device_len;      // DEVICE_CFG
    uint64_t isr_pa, isr_len;            // ISR_CFG
    uint32_t notify_off_mul;             // notify_off_multiplier
    uint64_t bar_pa[6];                  // 6 个 BAR 的基址（0 = 不存在）
    uint32_t feat_dev_lo, feat_dev_hi;   // 设备声明的特性（lo = bits 0..31，hi = bits 32..63）
    uint32_t feat_ok_lo, feat_ok_hi;     // 我们协商接受的（只接受真能处理的位）
    uint8_t  device_status;              // 最后一次读到的 DEVICE_STATUS
    uint16_t queue0_size, queue1_size;
    uint16_t queue0_notify_off;
    uint64_t q0_desc_pa, q0_avail_pa, q0_used_pa;
    uint32_t res_id, res_format;
    int      disp_w, disp_h, disp_enabled;   // GET_DISPLAY_INFO 的原始结果
    int      lfb_w, lfb_h, lfb_zoom;         // 同一时刻的 LFB 几何（解释差异用）
    uint32_t backing_entries;                // ATTACH_BACKING 的页数
    uint64_t backing_bytes;
    uint64_t cmds, cmd_timeouts;             // 命令总数 / 超时数
    uint64_t transfers, flushes, bytes_to_host;
    uint64_t blits_dev, blits_soft;          // 区域提交走了设备/软件的次数（fb 侧每次提交都记）
    uint64_t selftest_dev_blits, selftest_soft_blits;
    const char* last_err;                    // 最近一次失败的阶段名（"none" = 没失败过）
    // ★ ⑭ 交换链运行时统计（0 = 没开交换链）
    uint32_t swap_frames;                    // 交换链块数
    uint32_t swap_presents;                  // 整帧提交次数（真的走了设备）
    uint32_t swap_flips;                     // SET_SCANOUT 翻页次数
};

// 初始化（幂等）：PCI 扫描 -> MEM/BUS MASTER -> capability -> 特性协商 -> DEVICE_STATUS ->
//   队列 0（/1 仅报告）-> GET_DISPLAY_INFO -> RESOURCE_CREATE_2D + ATTACH_BACKING（**不设 scanout**，
//   等 selftest 的第一段软件路径截图做完才 SET_SCANOUT，见 .cpp 里的两段式说明）。
//   没有设备 / 任何一步失败：只打点并保持 ready=0（软件路径照常，绝不挂死）。
void vgpu64_init64();

// 1 = 设备路径可用（ready && !backend_disabled）。**几何不匹配时会在内部处理**（重配或降级），
//   调用方（fb.cpp）不需要自己判。
int  vgpu64_ready64();
const Vgpu64Info* vgpu64_info64();

// "virtio-gpu-2d" / "soft-lfb"（"当前显示后端"的唯一真源；hwui64/settings64/验收脚本都取这里）
const char* vgpu64_backend_name64();

// ---- 快路径 API ----
// 区域上屏：TRANSFER_TO_HOST_2D(脏矩形) + RESOURCE_FLUSH(脏矩形)。
//   返回 1 = **真的走了设备**（已上屏）；0 = 设备不可用/几何不匹配/超时（调用方走软件路径）。
//   坐标会被夹到当前渲染分辨率内；w/h <= 0 直接返回 0。
int  vgpu64_blit64(int x, int y, int w, int h);
// 区域填充：**2D 命令集没有 fill 原语**（只有 TRANSFER/FLUSH）—— CPU 填后备缓冲后由 blit 上屏。
//   返回 1 = 设备做的填充（本批恒为 0，如实），0 = CPU 填充（打点 [VGPU] fill ... dev=0）。
int  vgpu64_fill64(int x, int y, int w, int h, uint32_t color);
// 缓冲区/屏幕内拷贝（合成器搬窗口用）：**2D 命令集没有 copy 原语** —— CPU 行拷贝（memmove 语义）
//   后由 blit 上屏。返回 1 = 设备做的拷贝（本批恒为 0，如实）。
int  vgpu64_copy64(int dx, int dy, int w, int h, int sx, int sy);
// 软件路径提交计数（fb.cpp 每次走 CPU 路径时告知；用于"走了哪条路"的统计与验收打点）。
void vgpu64_note_soft_blit64(int x, int y, int w, int h);

// 自检（幂等；没有设备时打一行 skipped 并返回 0）：
//   ① 软件路径截图段（scanout 还没设，显示的是 legacy VGA framebuffer）
//   ② 设 scanout + 同一块图案走设备路径（TRANSFER+FLUSH）—— 两段的屏幕各保持 hold_ms，
//      验收脚本在这两段里各抓一帧 -> **逐字节比较**（设备路径 vs 软件路径的像素等价证据）。
//   ③ 第三段：用设备路径把该区域改成另一个颜色 -> 验收脚本比较"变化区域 == 请求区域"。
//   返回 0 = PASS 或 skipped（打点区分）；非 0 = 失败（位掩码，见 .cpp）。
int  vgpu64_selftest64();

// 基准（rdtsc 实测，多轮取中位数）：整屏 flip 与若干区域大小在**设备路径 vs 软件路径**下的耗时。
//   只在设备可用时跑（返回值 = 跑过的用例数；0 = 没跑）。打点见 [VGPU] bench ...
int  vgpu64_bench64();

// 分辨率变化后的重配（RESOURCE_UNREF -> CREATE_2D -> ATTACH -> SET_SCANOUT -> 整屏 TRANSFER）。
//   失败（或几何不支持）时**降级**：尝试把 scanout 交还 legacy VGA framebuffer + ready=0。

// ==================== ★ 用户计划 ⑭：VSync + 交换链（GPU 整帧翻页）+ 故障注入 ====================
// 语义：
//   * 交换链 = **每块一个 resource**（第 0 块 = 既有 primary resource，绑后备缓冲起始页；第 k 块绑
//     后备缓冲第 k 块首址起的页）。整帧提交 = TRANSFER_TO_HOST_2D(offset=0 整帧) + RESOURCE_FLUSH
//     + SET_SCANOUT 翻页 —— 绘制永远不写"正在被扫描出的那一块内存"，因此屏幕上任一时刻只能是
//     完整旧帧或完整新帧（宿主侧 screendump 抓不到中间态）。
//   * 返回值一律**如实**：设备不可用/几何不匹配/任何一步失败 -> 0，调用方（kernel/fb.cpp）原样
//     走软件路径并**继续出帧**（有界超时 + 有限次打点，绝不 PANIC）。
int  vgpu64_swap_init64(int frames);                 // 建交换链；返回可用块数（>=2 才成立）
int  vgpu64_swap_frames64();                         // 当前交换链块数
int  vgpu64_present64(int slot, int w, int h);       // 整帧提交第 slot 块；1 = 走了设备
// 故障注入（只由 fb.cpp 按 fw_cfg 配置调用；缺省全关 -> 既有行为一个字节不变）
//   kind：0=none 1=timeout（命令强制超时失败）2=illegal（响应类型非法）3=gone（设备消失）
//   after：第几条命令之后开始注入；注入次数有界（最多 8 条），命中后连续失败 -> 既有降级路径
void vgpu64_set_inject64(int kind, uint32_t after);
int  vgpu64_reconfigure64(int w, int h, int zoom);
