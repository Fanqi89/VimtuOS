// sysstate64.h - 64 位系统状态机 + 模块注册表 + 健康报告 + ring log
//
// 与 32 位 legacy32/kernel/sysstate.cpp 的关系（移植语义，按 64 位现状裁剪）：
//   32 位：SYS_STOPPED/STARTING/READY/RUNNING/STOPPING 五态状态机，由 sysctl 任务按 tick 推进；
//          模块表（名字 + init/stop 钩子 + 超时 + 重试 + CRITICAL/PLATFORM 标志）；
//          健康度（UP/DEGRADED/DOWN，CRITICAL 模块坏 = DOWN）；48 条 ring log 供 `syslog` 查询；
//          停止阶段按**逆序**停模块，超时强制清理，然后关机/重启（硬复位链或软启动）。
//   64 位：保留同样的"状态机 + 模块表 + 健康 + ring log + 逆序优雅停止"骨架，但：
//     * 启动阶段不重做各子系统初始化（它们在 kernel64.cpp 的启动序列里已经初始化并自检过），
//       init 钩子的语义改成**复核**（读各模块的只读状态，判断"在不在、活不活"），
//       失败了就如实降级（MOD_DEGRADED / MOD_FAILED），不会把已经跑起来的系统推倒重来；
//     * RUNNING 的判定点 = 桌面首帧（gui64 主循环第一次迭代）——那之前是 STARTING；
//     * 软重启（restart --soft）= 优雅停止（停模块 + session/config 落盘 flush）-> 再走原有硬复位链
//       （gui64 的 sys_reboot64：8042 -> PCI 0xCF9 -> 三重故障），不假装能"原地重启内核"；
//     * 关机/重启都先调用 sysstate64_stop_all64()（逆序停模块）。
//
// 串口打点（自动验收 grep，格式勿改；每行都走 dbg64_line_begin64/end64 行锁）：
//   [SYS64] state=BOOT                                  冷启动进入 BOOT
//   [SYS64] module <name> registered                    模块注册（注册几个打几行）
//   [SYS64] state=STARTING                              开始启动阶段
//   [SYS64] module <name> init ok (try=0)               模块复核通过
//   [SYS64] module <name> DEGRADED (non-critical)       非关键模块不可用（如实降级）
//   [SYS64] state=RUNNING gen=0 modules=17              桌面首帧：系统对外可用
//   [SYS64] health ok modules=17 failed=0 unknown=2     健康报告（正常路径）
//   [SYS64] state=STOPPING reason=shutdown              进入停止阶段
//   [SYS64] module <name> stop ok                       逆序优雅停止
//   [SYS64] state=STOPPED stopped=17 failed=0           停止完成（含 session/config 落盘）
//   [SYS64] soft restart requested                      软重启（优雅停止 -> 硬复位链）
//   [SYS64] syslog lines=<n>                            终端 syslog：ring log 行数
#pragma once
#include <stdint.h>

// ---- 状态 ----
#define SYS64_STATE_STOPPED   0
#define SYS64_STATE_BOOT      1
#define SYS64_STATE_STARTING  2
#define SYS64_STATE_RUNNING   3
#define SYS64_STATE_STOPPING  4

// ---- 模块状态 ----
#define SYS64_MOD_STOPPED   0
#define SYS64_MOD_STARTING  1
#define SYS64_MOD_READY     2
#define SYS64_MOD_DEGRADED  3
#define SYS64_MOD_FAILED    4

// ---- 健康度 ----
#define SYS64_HEALTH_DOWN     0
#define SYS64_HEALTH_DEGRADED 1
#define SYS64_HEALTH_UP       2

// ---- ring log（固定条数循环日志，供 syslog 查询）----
#define SYS64_LOG_RING 64       // 条数
#define SYS64_LOG_LEN  120      // 单行字符上限（含 NUL）

// 模块描述：名字 + 可选钩子（init 复核 / stop 优雅停止 / health 只读健康）+ 标志 + 停止超时。
//   init   返回 0 = 就绪；<0 = 不可用（非关键 -> MOD_DEGRADED，关键 -> MOD_FAILED）
//   stop   返回 0 = 干净停止；-3 = 还没停完（会等到 timeout_ticks，超时强制清理）；其它 <0 = 失败
//   health 返回 SYS64_MODH_UP / _DEGRADED / _DOWN / _UNKNOWN（-1 = 没有可查询的状态 API）
#define SYS64_MODF_CRITICAL 1u   // 关键模块：坏了 = 系统健康 DOWN
#define SYS64_MODF_PLATFORM 2u   // 平台层（软重启时保留；64 位软重启走硬复位链，保留只为报告语义）

// 模块健康钩子的 4 态返回值
#define SYS64_MODH_DOWN     0
#define SYS64_MODH_DEGRADED 1
#define SYS64_MODH_UP       2
#define SYS64_MODH_UNKNOWN  (-1)  // 该子系统没有可查询的状态 API：如实记 unknown，不算失败

struct Sys64Module {
    const char* name;
    int  (*init)();
    int  (*stop)(uint32_t timeout_ticks);
    int  (*health)();
    uint8_t  flags;
    uint8_t  pad[3];
    uint32_t timeout_ticks;      // 停止超时（tick；250Hz -> 1000tick = 4 秒）
};

// ==================== 状态机 ====================
void        sysstate64_begin64();                   // 冷启动：state=BOOT（模块注册之前调用）
int         sysstate64_register64(const Sys64Module* m);   // 注册一个模块（返回下标）
void        sys64_register_builtin64();             // 注册平台/子系统模块（kernel64.cpp 调）
int         sysstate64_start64();                   // STARTING 阶段：依次复核模块（0 = 全部就绪/降级）
int         sysstate64_tick64();                    // GUI 主循环每帧调用；返回 1 = 本帧刚进 RUNNING
int         sysstate64_stop_all64(const char* reason); // 逆序优雅停止（返回已停止模块数）
void        sysstate64_soft_restart64();            // 软重启：优雅停止 + 落盘 -> 硬复位链（不返回）

int         sysstate64_state64();
const char* sysstate64_state_text64();
uint32_t    sysstate64_generation64();
int         sysstate64_module_count64();
int         sysstate64_module_failed_count64();
const char* sysstate64_module_name64(int i);
int         sysstate64_module_state64(int i);
const char* sysstate64_module_state_text64(int st);
int         sysstate64_module_health64(int i);      // 4 态：见 SYS64_MODH_*

// 健康度
int         sysstate64_health64();                  // SYS64_HEALTH_*
const char* sysstate64_health_text64();
int         sysstate64_health_report64(int log_to_serial, char* out, int maxlen);

// ring log
void        sys64_log64(const char* line);
void        sys64_logf64(const char* fmt, ...);     // 支持 %s %d %u %x %c %%
int         sys64_log_count64();                    // 当前 ring 里的行数（<= SYS64_LOG_RING）
const char* sys64_log_line64(int i);                // i = 0 最新
// 终端 `syslog` 用：打印 "[SYS64] syslog lines=<n>" + 逐行（最多 max 行）
int         sysstate64_dump_syslog64(int max);

// 报告（终端 state / health 用）
int         sysstate64_report64(char* out, int maxlen);

// ==================== VimtuFS2 卷使用情况 ====================
// gui64 的"我的电脑"窗口与终端 `disk` 命令共用这一份数据：
//   读一次分区超级块（总块数/位图位置）+ 块位图（空闲块）+ vfs64_ls("/")（文件数/字节数），
//   结果**缓存**（谁也不会每次重绘都去读盘）。返回 0 = 数据可信。
//   没有分区/没有卷/读盘失败时如实返回 -1 并把各字段清零（调用方打 "no volume"）。
struct Fs64Info {
    int      ok;             // 1 = 数据可信
    uint32_t total_blocks;   // 卷总块数（1 块 = 512B）
    uint32_t free_blocks;    // 空闲块数（按块位图统计，只统计数据区）
    uint32_t files;          // 根目录文件数
    uint32_t used_bytes;     // 根目录文件字节合计
    uint32_t pb_lba;         // 主分区起始 LBA（超级块所在位置）
    uint32_t inodes;         // inode 数（超级块字段）
};
int sysstate64_fsinfo64(Fs64Info* out);
