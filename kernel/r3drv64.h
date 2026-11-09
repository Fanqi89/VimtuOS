// r3drv64.h - ★ v0.9.33：**ring3 驱动框架**（内核侧）
//
// ==================== 一句话 ====================
// 把"文件系统协议解析"从 ring0 搬到 ring3，但把"总线/DMA"留在 ring0：
//   * ring0 只干它非干不可的三件事：**探盘**、**按 PRDT 发 DMA**、**监督服务进程生死**；
//   * ring3 驱动服务拿到的数据是 DMA **直接落进它自己映射的共享页**（0 次 memcpy），
//     它在用户态解析 ext4，崩溃了也只死一个进程。
//
// ==================== 与既有设施的关系（为什么不新造轮子）====================
//   * 共享内存：直接用 proc64_shm_create64 / proc64_shm_map64 / proc64_shm_view64
//     （v0.5 起就有的 A5 前缀设施）。**view 能把物理帧号交给内核**，这正是零拷贝的钥匙。
//   * DMA：ahci64_read_frames64（本批新增）"一帧一项 PRDT"，不经过任何内核缓冲。
//   * 进程生命周期：proc64_create64 / proc64_start_elf64_argv64 / proc64_find64。
//   * 配置：/etc/r3drv.conf（key=value；缺就按内置默认，幂等种一份，与 fontcfg64 同一套做法）。
//
// ==================== 诚实边界（写在这里，别在文档里吹）====================
//   * 零拷贝**只在 AHCI 上成立**（PATA 的 PIO 没有 DMA 可达的目标地址；NVMe/USB 尚未接）。
//     非 AHCI 盘会自动退到"拷贝对照路径"，日志里如实写 reason=not-ahci。
//   * 环的完成是**就地**（SQE.state），没有独立 CQ 环、没有批量中断/事件队列；
//     一次 kick 消费 ≤ R3DRV_SLOTS 条，超出的留给下一次 kick。
//   * 没有任何"驱动隔离/权限"：能 register 的进程就能对该盘发任意 LBA 的读请求。
//     写路径**完全没做**（只读框架）——这是有意为之：只读驱动不会弄坏用户数据。
//   * 重启上限 DRV64_RESTART_MAX 次后置 FAILED，卷退回只读安全态（不静默、不假装健康）。
// ★ v0.9.34：进程退出漏斗（proc64 的 p64_exit64）**同一拍**调这里，把死亡时刻打进槽。
//   只标记不拉起（拉起要读 ELF = 磁盘 I/O，必须在心跳线程里做）。pid 不匹配 = 什么都不做。
extern "C" void r3drv64_on_die64(int pid);
#pragma once
#include <stdint.h>
#include "r3drv_abi.h"

static const uint32_t R3DRV64_MAX          = 8;      // 槽位数（同时最多 8 个 ring3 驱动）
static const uint32_t R3DRV64_NAME_MAX     = 16;
static const uint32_t R3DRV64_PATH_MAX     = 32;
static const uint32_t R3DRV64_RESTART_MAX  = 3;      // 同一槽最多重启几次
static const uint32_t R3DRV64_BEAT_TICKS   = 250;    // 心跳超时（PIT 250Hz -> 1s）；可用配置覆盖
static const uint32_t R3DRV64_KICK_LOG_MAX = 4;      // 前几条 kick 打点，之后按 64 条采样
static const char     R3DRV64_CONF_PATH[]  = "/etc/r3drv.conf";

// 幂等初始化：分配"拷贝对照"用的暂存页（16 页 = 64 KiB）+ 读配置 + 打一行 [R3DRV] init。
void r3drv64_init64();

// 位掩码自检（0 = 全过）：ABI 布局、槽表空、配置可读性、帧号数组容量。
int  r3drv64_selftest64();

// ==================== 服务侧（由 syscall64 转发，ring3 才能进来）====================
// 注册：name 已由 syscall 层拷进内核缓冲（NUL 结尾）。
//   proto/disk = 本服务认领的协议与盘；shm_id = 调方自己的共享对象（必须真的持有句柄）。
//   返回槽号（>=0）或负错误码；同名重复注册 = **更新**（重启后新进程重新认领同一槽）。
int64_t r3drv64_register64(const char* name, uint32_t proto, uint32_t disk,
                           int32_t shm_id, uint64_t bytes);
int64_t r3drv64_beat64();                                     // 心跳：0 = ok
int64_t r3drv64_kick64(int32_t shm_id);                       // 门铃：消费 SQ 环，返回处理条数
int64_t r3drv64_dma_read64(int32_t shm_id, uint64_t obj_off, uint64_t lba, uint32_t secs);
int64_t r3drv64_copy_read64(int32_t shm_id, uint64_t obj_off, uint64_t lba, uint32_t secs);
int     r3drv64_info64(uint32_t slot, R3drvInfo* out);         // 1 = 已填；0 = 槽空/越界

// ==================== 内核侧 ====================
// 按分区类型路由：探该 (disk, lba) 上的文件系统，找有没有活着的驱动。
//   返回 R3DRV_ROUTE_*；out_slot 可空。**不改任何挂载状态**，纯查询（可反复调）。
int  r3drv64_route64(uint32_t disk, uint32_t lba, int* out_slot);
// 探测某个 (disk, lba) 上的协议（R3DRV_PROTO_*；认不出 = NONE）。**只读，不写盘**。
uint32_t r3drv64_probe_proto64(uint32_t disk, uint32_t lba);
// 在真实存在的盘上找第一个装着某协议的卷（跳过 ATAPI/空槽）：1 = 找到（out_* 已填）。
int  r3drv64_find_volume64(uint32_t proto, uint32_t* out_disk, uint32_t* out_lba);
// 拉起一个驱动服务（记下 ELF 路径，死了才重启得起来）：1 = 成功（out_slot/out_pid 已填）。
int  r3drv64_start_service64(const char* path, const char* name, uint32_t proto,
                             uint32_t disk, uint32_t lba, int* out_slot, int* out_pid);
// 开机自启（读 /etc/r3drv.conf；没有目标卷就只打一行跳过）：1 = 起了服务，0 = 没起。
int  r3drv64_autostart64();

// 监督 tick（内核主循环/心跳里调）：心跳超时或进程消失 -> 记一笔并重启，超上限 -> FAILED。
// 幂等且**绝不阻塞**（不等待、不 sleep，只做状态机推进 + 拉起进程）。
void r3drv64_tick64();

// /proc/vimtu/drivers 的渲染（只读合成文本）：返回写入字节数；cap 不够就截断。
int  r3drv64_render64(char* out, uint32_t cap);

// 统计（自检/日志/测试用）
uint32_t r3drv64_count64();          // 已占用的槽数
uint32_t r3drv64_restarts64();       // 全局累计重启次数
