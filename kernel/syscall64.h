// syscall64.h - 两个系统调用入口：自有 ABI 的 int 0x80 + Linux ABI 的 syscall 指令
//
// ============================ 入口 1：int 0x80（Vimtu64 自有 ABI）============================
//   rax = 调用号；rdi / rsi / rdx = 参数 1/2/3；返回值放 rax（负数 = 错误）。
//   门：IDT[128] 装成 0xEE（P=1、DPL=3 中断门，见 kernel/x86_64.cpp 的 idt_build），
//       所以 ring3 直接 int 0x80 即可；内核侧入口 isr128_64 -> isr_handler64 ->
//       syscall64_dispatch64（本文件）。
//
// 调用号：
//   1 write(fd, buf, len)   fd=1：输出到 串口 + 屏幕。buf 是**用户态地址**，必须通过
//                           user64_range_ok64（用户窗口内、已映射为用户页），越界返回 -1。
//   2 exit(code)            结束 ring3：由 usermode64 改写系统调用帧回到内核（见其说明）
//   3 getpid()              当前任务 id（没有调度器的内核返回 0）
//   4 ticks()               PIT tick 数（250Hz）
//   5 sleep_ms(ms)          睡眠（走 task_sleep64；没有调度器时按 tick 忙等）
//   6 open(path)         批次 B 起**真实现**：走 kernel/fd64.cpp 的 FD 层（只读；单层路径 "/name"）；
//                         失败返回 -1（不做假成功）。
//   7 read(fd, buf, len) 批次 B 起**真实现**：fd >= 3 从 FD 层读（单次上限 4096B）并拷进用户 buf；
//                         buf 先过 user64_range_ok64；fd 非法/越界 -> -1。
//   8 close(fd)          批次 B 起**真实现**：释放 FD 层的槽；fd 非法 -> -1。
//   ★ 这条路径的**号段与既有行为其余部分不动**（app64/demo64/既有测试都靠它）。
//
//   ==================== A1：用户态绘图（ring3 自己画屏；内核只映射显存 + 提交区域）====================
//   9  fb_map(out_va**, out_info**)      把内核的**后备缓冲**（back buffer）按**用户可读写**页表
//                                        映射进**当前进程**的地址空间，返回用户态 VA + 几何。
//                                        rdi = 用户指针（写 8 字节 VA），rsi = 用户指针（写 32 字节
//                                        Fb64Info，布局见本文件下面）；返回 0 = 成功。
//                                        **幂等**：同一个地址空间重复调用返回同一个 VA（页表已存在
//                                        就直接复用，绝不重复分配/重复映射）。
//   10 fb_flip(x, y, w, h)               把用户刚画好的矩形从后备缓冲**提交到屏幕**（LFB）。
//                                        rdi/rsi/rdx/r10 = x/y/w/h（有符号）；返回 0 = 已提交
//                                        （含**夹取**后的部分提交）、1 = 完全越界被拒（打点 clip=reject，
//                                        什么都不提交、不崩）。
//   11 fb_present()                      整屏提交（缩放模式下的整帧路径），返回 0。
//   ==================== ★ A5 前置：每进程输入事件队列 + 共享内存缓冲 ====================
//   12 input_poll(Event *out, unsigned max, unsigned flags)
//                                        **每进程**事件队列取事件（自有 ABI，详见 kernel/input64.h）：
//                                        rdi=out（用户数组，40B/条）、rsi=max、rdx=flags；
//                                        max=0 -> 只查询\"有多少待取\"并返回条数；
//                                        max>0 -> 最多拷 max 条并返回条数；负数 = 错误码：
//                                          -1 EPERM  没有进程上下文（任务 0 / 共享模式）
//                                          -2 EFAULT out 指针非法
//                                          -4 ENOMEM max 超过队列容量（EV64_MAX_EVENTS64=32）
//                                          -5 EINVAL flags 有未知位
//                                        flags：bit0 申请键盘焦点、bit1 申请指针捕获、bit2 释放两者。
//                                        队列满**丢最旧**并计数打点（`[EV64] drop`），绝不静默。
//   13 shm_create(size)                  建一块**跨进程可共享**的内存对象，返回对象 id（>=1）；
//                                        负数 = 错误码（-1 EPERM 无进程上下文 / -3 EINVAL size 非法 /
//                                        -4 ENOMEM：对象表满、页池空、句柄表满）。上限 64 KiB/对象。
//   14 shm_map(id, offset, len, out_va)  把对象的 [offset,offset+len) 映射进**当前进程**
//                                        （用户可读写、不可执行），*out_va 写回用户态基址；
//                                        offset 必须 4KiB 对齐；返回 0 = 成功。
//                                        错误码：-1 EPERM（没有这个句柄）、-2 EFAULT（out_va 非法）、
//                                        -3 EINVAL（越界/非对齐/len=0）、-4 ENOMEM（映射窗满/页表页不足）。
//                                        句柄 fork 继承、execve 默认关闭、退出引用 -1 并在归零时回收页帧。
//
//   ==================== ★ A5：Wayland 基础骨架（surface / buffer / commit / seat = 号 15..21）====================
//   目标：把"客户端在用户态画进共享缓冲 -> 提交 -> 合成器显示"这条最小闭环按 Wayland 术语立起来
//   （接口/语义/错码/打点格式的唯一定义点 = kernel/wl64.h，实现 = kernel/wl64.cpp）。
//   15 wl_surface_create(w, h, format)          -> surface id (>=1)；fmt=0(XRGB8888)、单面 <= 16384 px
//   16 wl_surface_attach(surf, shm_id, offset)  -> 0；缓冲 = 一个 shm 对象 + 4 字节对齐的 offset
//   17 wl_surface_damage(surf, xy, wh)          -> 0；★ rsi=(x|y<<32)、rdx=(w|h<<32)（5 分量打包）
//   18 wl_surface_commit(surf)                  -> 0；**入提交队列**（异步：dispatch 里合成上屏）
//   19 wl_surface_destroy(surf)                 -> 0；归还 attach 的 shm 引用
//   20 wl_seat_get()                            -> seat id (1)；申请键盘焦点 + 指针捕获（复用 12 的 flags）
//   21 wl_display_dispatch(timeout_ms, out, max)-> 本次投递的 seat 事件条数；同一轮里合成提交队列
//       out = Wl64SeatEvent 数组（64B/条 = input_poll 的 40B 事件 + seat/surf/sx/sy/inside），max <= 32。
//   错误码：-1 EPERM（没有进程上下文 / surface 不属于本进程）、-2 EFAULT（out 指针）、
//           -3 ENOENT（没有这个 surface/shm 对象/句柄）、-4 EINVAL（尺寸/格式/offset/越界/max）、
//           -5 ENODEV（没有可用的显示/面板放不下）、-6 ENOSPC（surface 表满/引用计数）。
//   ★ 安装介质内核不链 kernel/wl64.cpp -> 这 7 号的弱引用为 0 -> 返回 -1 并打 [SYSCALL] deny
//     （**不假装成功**；它们不是 enosys 号，所以不会污染"不得出现 enosys"那些断言）。
//   打点：[WL64] init / surface create / attach / commit / blit / composite / seat event /
//         surface destroy / release / selftest / demo —— 见 kernel/wl64.h 的清单。
//   ==================== ★ B-wm：Ring 3 合成器（号 22..26）====================
//   目标：把"合成 + 上屏"从内核搬到用户态合成器进程（/bin/wm）—— 内核只留 surface 表、提交队列、
//   缓冲页映射、seat 路由/投递、fb_map/fb_flip。接口/语义/错误码的唯一定义点 = kernel/wl64.h。
//   22 wl_composer_get()            -> (seat id) | (gpu<<8)；注册当前进程为合成器 + 申请焦点/捕获
//   23 wl_surface_export(idx, out)  -> 0 = 填好一条 Wl64SurfaceInfo(64B)；1 = 该下标没有 surface
//   24 wl_surface_map(surf, out)    -> 0；把该 surface 的缓冲页映射进合成器（Wl64SurfaceMap(16B)）
//   25 wl_surface_ack(surf)         -> 0；合成器声明"这条合成完了"（清 pending/damage、content=1）
//   26 wl_seat_post(out_ev)         -> 投递到的 pid；合成器把事件投回目标 surface 的拥有者队列
//   22..26 只有注册过的合成器能调（其它进程 EPERM）；没有合成器时内核继续走 15..21 的老路
//   （dispatch 里合成、打 [WL64] composite），两条路互不影响。安装介质内核不链 wl64.cpp -> deny。
//   ==================== ★ 本批：用户态设备映射（号 48 pci_map_bar）====================
//   目标：让 ring3 的**驱动服务**能直接读自己那块设备的寄存器（用户态驱动的第一块地基）。
//   48 pci_map_bar(bdf, bar_index, out_va, out_len)
//      rdi = bdf        打包的 PCI 位置：bdf = (bus << 8) | (dev << 3) | (fn)（0..0xFFFF）
//      rsi = bar_index  BAR 序号 0..5（64 位 BAR 只填它的**低半**序号，高半自动一起读）
//      rdx = out_va     用户指针（8 字节）：写回映射到的**用户 VA**
//      r10 = out_len    用户指针（8 字节）：写回 **BAR 的实际大小**（字节）
//      -> 0 = 成功；< 0 = 错误码（见下）。**幂等**：同一个 (bdf, bar) 重复调用返回同一个 VA，
//         不重复分配页表页（内核打点里 `re=1`）。
//   语义要点（细节与风险见 docs/应用层与系统调用说明.md 末节"用户态设备映射与驱动服务骨架"）：
//     * 只映射 **MMIO**（memory BAR）：I/O 端口 BAR 直接用 -EINVAL 拒掉，绝不假装能映射；
//     * 长度取**写全 1 回读**探出来的真实大小（不盲信寄存器里的地址位）；64 位 BAR 读高 32 位；
//     * 页表项 = P|U|W|NX（用户可读写、不可执行）——与内核驱动读同一段物理地址的内存类型一致；
//     * 窗口：每进程 DEV64_SLOTS64=8 槽 × 256 KiB（见 kernel/proc64.h），**不是页池的页**，
//       进程退出/execve/fork/munmap 四条路径都跳过它（否则会往页池链表里插设备地址）；
//     * 权限：**只允许 root（euid == 0）**。非 root 返回 -EPERM 并打 `[PCIMAP] deny ... reason=not-root`。
//   错误码（自有 ABI 的负数风格；与 kernel/pci64.h 的 PCI64_* 同一张表）：
//     -1 = EPERM   非 root / 没有进程上下文
//     -2 = EFAULT  out_va / out_len 不是用户可写指针
//     -3 = EINVAL  bdf 编码非法 / bar_index > 5 / **该 BAR 是 I/O 端口不是 MMIO**
//     -4 = ENOMEM  映射窗满（8 槽）/ BAR 比单槽（256 KiB）还大 / 页表页不足（已回滚）
//     -5 = ENODEV  没有这个设备 / 该 BAR 未实现 / BAR 物理地址在恒等映射之外（>= 4 GiB）
//   ★ 号位为什么是 48：22..26 已被 Ring 3 合成器（另一条线）占用，48 与 1..26 都拉开距离。
//   ★ 打点（自动验收 tests/drvsvc64_test.py grep，格式勿改；失败行有上限防刷屏）：
//     [PCIMAP] map pid=<n> bdf=0x<hex> bar=<n> pa=0x<hex> len=<n> va=0x<hex> pages=<n> u=1 re=<0|1>
//     [PCIMAP] FAILED pid=<n> bdf=0x<hex> bar=<n> reason=<...> err=<n>
//     [PCIMAP] deny pid=<n> euid=<n> reason=<...> err=<n>
//     [PCIMAP] release pid=<n> slots=<n> freed=0 (mmio, not page-pool)      （进程退出路径）
//
//   错误码（负数，两个入口的 errno 风格一致，**不与 Linux 号段共用号**）：
//     -1 = EPERM  用户窗口/页表不可用（UEFI 固件只读页表，见 kernel/usermode64.cpp）
//     -2 = EFAULT 用户指针非法（没通过 user64_range_ok64）
//     -3 = ENODEV 没有帧缓冲（fb 未初始化 / 没有后备缓冲）
//     -4 = ENOMEM 页表页不足（映射失败）
//   打点（自动验收 tests/fbmap64_test.py grep，格式勿改）：
//     [FB64] map pid=<n> va=<hex> pa=<hex> w=<n> h=<n> pitch=<n> fmt=<n> pages=<n> re=<0|1> u=<0|1>
//     [FB64] flip pid=<n> x=<n> y=<n> w=<n> h=<n> clip=ok|clamped|reject
//     [FB64] present pid=<n> w=<n> h=<n>
//     [FB64] map FAILED pid=<n> reason=<...> err=<n>
//   ★ 越界一律"夹取 or 拒绝"，绝不让用户参数直接进内核的绘制路径（不越界、不崩）。
//   ==================== ★ 系统音效：音频 ABI（号 49 audio_play）====================
//   目标：让 ring3 **真的能出声** —— 系统音效素材（/usr/share/sounds/*.wav，构建期由
//   tools/sounds_gen.py 合成、tools/sounder_pack_win.py 装卷）在**用户态**解析成 PCM，再用这一号
//   把"一段内存 PCM"交给**已有的** HDA 驱动播放。内核只做校验 + 转发：
//   **不做混音、不选素材、不碰音量/静音/采样率/输出源**（那些策略全在用户态：/bin/sounder +
//   既有 audio 命令/声音面板；采样率只有 hda64 那一种格式，见下）。
//   49 audio_play(pcm_va, frames, format)
//     rdi = pcm_va  用户态缓冲（16 位立体声交织；frames*4 字节，**只读**）
//     rsi = frames  每声道采样数（1 .. SND64_MAX_FRAMES64 = 96000 = 2 秒，有界）
//     rdx = format  只认 SND64_FMT_48K16S2 = 0x11（48kHz/16bit/2ch —— 与 hda64 的流格式逐位一致）
//     -> 0 = 整段已送进控制器（按 <=8KiB 周期分块，阻塞到每块的流完成；驱动自己的等待有界，
//            绝不挂死）；< 0 = 错误码（常量见本文件下面的"系统音效"段）。
//   校验顺序 = 判错顺序（用户态可以按它写负例）：fmt -> frames -> 缓冲范围 -> 驱动 -> 忙。
//   ★ 打点（自动验收 tests/sounds64_test.py grep，格式勿改；两条各 64 行上限防刷屏）：
//     [SND64] play va=0x<hex> frames=<n> fmt=0x<hex> rc=<带符号整数>
//     [SND64] deny reason=<bad-fmt|bad-frames|bad-buf|no-driver|stream-busy|stream-timeout> err=<n>
//
// ============================ 入口 2：syscall 指令（Linux x86_64 ABI）============================
//   寄存器约定与 Linux 完全一致：rax = 调用号；rdi/rsi/rdx/r10/r8/r9 = 参数 1..6；
//   返回 rax（负值 = -errno）；rcx/r11 被 CPU 覆盖（Linux 也一样，属于易失）。
//   入口汇编在 kernel/syscall_entry64.asm（LSTAR 指向它）：SYSCALL 不换栈，所以入口
//   自己切到内核栈、按 isr_stubs64.asm 的 0xD0 布局压出一份完整用户帧，再调本文件的分发器。
//   帧结构与 int 0x80 **完全相同**，只有 int_no 槽不同：syscall 指令路径填
//   SYSCALL64_INSM_FRAME_MARK64，分发器靠它分流（两条路径的号段语义互不影响）。
//
//   号段映射（完整表 + 每条的实现状态见 kernel/syscall64.cpp 顶部的大注释表）。
//   能做到的**真做**，做不到的一律 -ENOSYS(-38) 并且每个号只打一次
//   `[SYSCALL] enosys nr=<n>`（避免刷屏），绝不假装成功。
//
// 串口打点策略（避免刷屏；自动验收 grep 用，格式勿改）：
//   [SYSCALL] nr=<n> rdi=<hex> rsi=<hex> rdx=<hex> ret=<hex>        int 0x80：只对 write(1)/exit(2)
//   [SYSCALL] insn nr=<n> rdi=<hex> rsi=<hex> rdx=<hex> ret=<hex>   syscall 指令：每个调用都打
//                                                                  （调用量小，证据链最完整）
//   [SYSCALL] deny nr=<n> arg=<ptr>                                 任何安全校验失败（两条路径共用）
//   [SYSCALL] enosys nr=<n>                                         未实现的号（每号只打一次）
//   [SYSCALL] msr init ...                                          MSR/STAR/LSTAR/FMASK 初始化留证
//   [SYSCALL] selftest PASS / [SYSCALL] selftest FAIL mask=<n>
#pragma once
#include <stdint.h>
#include "x86_64.h"

// 启动期调用：装 int 0x80 门 + 配 syscall 指令的 MSR（SCE/STAR/LSTAR/FMASK）+ 自检。幂等。
void syscall64_init64();
int  syscall64_selftest64();                     // 位掩码自检，0 = 全过

// 中断分发入口：isr_handler64 在 no == 128 时调用（x86_64.cpp 里是 weak 引用）
extern "C" void syscall64_dispatch64(pt_regs64* r);


// ==================== A1：用户态绘图接口（fb_map/fb_flip/fb_present）====================
// 结构体布局是**内核与用户程序之间的 ABI**：改字段必须同时改 docs/应用层与系统调用说明.md
// 的"用户态绘图与事件接口（A1）"一节与 user/fbdemo.asm 的读法（它按固定偏移读）。
struct Fb64Info {
    uint32_t width;      // +0  后备缓冲宽（像素；= 渲染分辨率，见 fb_width()）
    uint32_t height;     // +4  高
    uint32_t pitch;      // +8  每行字节数（= width*4）
    uint32_t format;     // +12 像素格式：0 = XRGB8888（0x00RRGGBB，内存里低字节是蓝）
    uint64_t size;       // +16 后备缓冲总字节数（= height*pitch）
    uint64_t va;         // +24 用户态可读写的映射基址（与 out_va 相同）
};
static const uint32_t SYSCALL64_FB_INFO_SIZE64 = 32;
static const uint32_t SYSCALL64_FB_FMT_XRGB8888 = 0;

// 错误码（见本文件头部的 A1 段）：负数，返回值放 rax。
static const int64_t SYSCALL64_FB_EPERM64  = -1;   // 用户窗口不可用（UEFI 固件页表）
static const int64_t SYSCALL64_FB_EFAULT64 = -2;   // 用户指针非法
static const int64_t SYSCALL64_FB_ENODEV64 = -3;   // 没有帧缓冲/后备缓冲
static const int64_t SYSCALL64_FB_ENOMEM64 = -4;   // 映射失败（页表页不足）
// fb_flip 的返回码：0 = 已提交（可能被夹取）、1 = 完全越界被拒（打点 clip=reject）
static const int64_t SYSCALL64_FB_FLIPPED64 = 0;
static const int64_t SYSCALL64_FB_REJECT64  = 1;
// ==================== ★ 本批：用户态设备映射（自有 ABI 48 pci_map_bar）====================
// 语义/权限/打点格式的唯一说明见本文件上面那一段。这里只放**内核与用户程序共用的常量**：
// 号位（用户程序按它取号）与错误码（负数，返回值放 rax）。
static const uint64_t SYSCALL64_PCIMAP_NR64 = 48;
static const int64_t  PCIMAP64_EPERM64  = -1;   // 非 root / 没有进程上下文
static const int64_t  PCIMAP64_EFAULT64 = -2;   // out 指针非法
static const int64_t  PCIMAP64_EINVAL64 = -3;   // bdf/bar_index 非法，或该 BAR 是 I/O 端口
static const int64_t  PCIMAP64_ENOMEM64 = -4;   // 窗满 / BAR 比单槽大 / 页表页不足
static const int64_t  PCIMAP64_ENODEV64 = -5;   // 没有这个设备 / BAR 未实现 / 在恒等映射之外
// pci_map_bar 的"复用"返回约定：rax = 0 = 新建映射；与内核对齐，用户拿到的一直是 0
// （`re=1` 只是内核打点里的信息，不改变用户可见返回值）。
// ==================== ★ 系统音效：音频 ABI（自有 ABI 49 audio_play）====================
// 语义/校验顺序/打点格式的唯一说明见本文件上面那一段。这里只放**内核与用户程序共用的常量**
// （号位 + 格式字 + 上限 + 错误码；用户程序按同一份约定取号/判错）。
static const uint64_t SYSCALL64_SND_NR64 = 49;
static const uint32_t SND64_FMT_48K16S2  = 0x11u;    // 48kHz/16bit/2ch（= hda64 的 Set Converter Format 值）
static const uint64_t SND64_MAX_FRAMES64 = 96000u;   // 2 秒上限（48000 帧/秒）——一次调用有界阻塞的依据
static const int64_t  SND64_EFAULT64 = -2;   // pcm_va 不是用户可读的 [va, va+frames*4)
static const int64_t  SND64_EINVAL64 = -3;   // fmt != 0x11 / frames == 0 / frames > 上限
static const int64_t  SND64_EAGAIN64 = -4;   // 单流驱动正忙（另一段流在跑）/ 流超时（驱动如实报错）
static const int64_t  SND64_ENODEV64 = -5;   // 没有 HDA 控制器 / 通路没建立（hda64 not ready）
// ==================== syscall 指令路径（给汇编入口 / usermode64 用）====================
// 帧标记：syscall 指令路径的 int_no 槽填这个值（int 0x80 是 0x80）。改它必须同步
// kernel/syscall_entry64.asm 的 %define FRAME_MARK。
#define SYSCALL64_INSM_FRAME_MARK64 0x180ULL


// LSTAR 的目标（kernel/syscall_entry64.asm）。syscall64_init_msr64() 把它写进 LSTAR。
extern "C" void syscall64_insn_entry64();

// 入口出口开关：1 = 本帧的 exit 已把控制权交回内核（放行时不要 sysretq，走 ring0 蹦床）。
// 由分发器的 exit 分支置位，入口汇编读它。
extern "C" uint64_t g_syscall64_exit_to_kernel64;
// 内核栈顶（**当前任务**的 SYSCALL 入口专用栈）。SYSCALL 不换栈，入口靠这个值切栈。
// 批次 C 起它是**每任务一份**的：
//   * 有调度器时由 task64.cpp 的 task_apply_ctx64() 在每次任务切换时更新
//     （= 该任务内核栈顶下方 4KB；见 task64.cpp 里"为什么不能共用一块"的说明）；
//   * 任务 0 / 没有调度器（安装介质内核）时等于下面那个静态专用栈顶，值不变。
// usermode64.cpp 每次进 ring3 前也会把它对齐一次（与 tss_set_rsp0 同一处）。
extern "C" uint64_t g_syscall64_kstack64;
// 静态专用栈顶（.bss 里那块 16KiB；任务 0 与无调度器场景用它）—— task64/ usermode64 用它兜底
uint64_t syscall64_static_kstack_top64();
