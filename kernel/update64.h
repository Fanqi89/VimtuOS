// update64.h - VimtuOS 64 位 update 子系统（标记文件 -> 应用动作 -> 自动重启）
//
// ★ 边界（必须先说清楚，别把它读成"真正的升级包"）：
//   这不是真正的"升级包"机制——没有内核替换、没有差分/签名/回滚，也没有从网络或介质读取
//   新版本。它只是把 32 位 `update` 命令那套**机制闭环**搬过来并做实：
//     终端 `update pending <ver>`（或外部写盘）写一个标记文件 /update.pending
//       -> 启动时（或终端 `update apply`）校验内容、把"已应用"记进 store64（update.applied=<ver>）
//          与 sysstate64 ring log、写 /update.done（VimtuFS2 没有 rename，用"写新文件 + 删旧文件"）
//       -> 自动软重启（sysstate64_soft_restart64：优雅停止 + 落盘 -> 硬复位链）。
//   真正的"升级包"（新内核二进制、差分、校验）不在本批次范围内。
//
// 与 32 位的对应关系：32 位把标记放在 /sys/update.pending，由 sysctl 状态机在 READY/运行中
//   轮询到就 sys_clear_update_pending() + 重启；64 位的 VimtuFS2 是**单层路径**（没有 /sys 目录），
//   所以标记文件落在根目录：/update.pending（内容为 "ver=<x.y.z>\n" 的 ASCII 文本）。
//
// 串口打点（自动验收 tests/preload_update_test.py grep，格式勿改）：
//   [UPDATE64] init version=<v> applied=<v|(none)>
//   [UPDATE64] no pending                           启动检查：没有标记文件
//   [UPDATE64] pending invalid ver=?                标记文件内容非法（保留文件，不应用）
//   [UPDATE64] stage pending ver=<v> path=/update.pending rc=0   终端 update pending <ver> 写入成功
//   [UPDATE64] apply pending ver=<v> at boot|manual 开始应用
//   [UPDATE64] applied ver=<v> from=<cur> store=0 flush=0 done=/update.done gen=<n>
//   [UPDATE64] restarting to finish update          应用完成 -> 软重启
#pragma once
#include <stdint.h>

#define UPDATE64_VERSION      "0.1.0"
#define UPDATE64_PENDING_PATH "/update.pending"
#define UPDATE64_DONE_PATH    "/update.done"
#define UPDATE64_STORE_KEY    "update.applied"

// 初始化：版本打点 + 向 sysstate64 注册 "update64" 模块。
// 位置要求：os_boot_path 里 sysstate64_start64() **之前**。
void update64_init64();

// 启动检查：没有 pending -> 打 [UPDATE64] no pending 并返回 0；
// 有且内容合法 -> 应用（store + ring log + /update.done + 删除 pending）后**自动软重启**（不返回）；
// 内容非法 -> 打点并返回 0（保留文件，供人工查看）。
int update64_check64();

// 读 pending 文件内容（存在且可读时返回 1，out 里是 NUL 结尾的原文；否则 0）
int update64_pending_exists64(char* out, int max);

// 手动应用一次（终端 `update apply`）：不重启，由调用方决定后续。
// 返回 1 = 应用成功（pending 已转成 /update.done 并被删掉）；0 = 没有/非法。
int update64_apply64();

// 终端 `update pending <ver>`：把 "ver=<ver>\n" 写进 /update.pending。返回 0 = 成功。
int update64_write_pending64(const char* ver);

const char* update64_version64();                       // 当前内核版本串
int update64_applied64(char* out, int max);             // 读 store 里的 update.applied；-1 = 没有
const char* update64_pending_path64();
const char* update64_done_path64();

// 终端 `update status` 的一行式报告（不分配、无 libc；写入 out，NUL 结尾）
void update64_report64(char* out, int maxlen);
