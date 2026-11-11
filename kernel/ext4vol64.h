// ext4vol64.h - ★ v0.9.40：内核侧的 **ext4 只读客户端**（通过 ring3 服务的反向请求通道）
//
// 定位（一句话）：内核不再自己解析 ext4 —— 它把"列目录/取属性/读文件"变成**一条有界 RPC**
// 发给 `/usr/bin/ext4svc`，服务在自己地址空间里解析盘上字节，把结果写回共享对象的数据区。
//
// 三条硬约束（都由 r3drv_abi.h 的 R3drvReq 注释定义）：
//   1) **不阻塞 GUI**：每次 RPC 有 150 ms 上限，超时即返回 -ETIMEDOUT，绝不无限等；
//   2) **等待期间必须让出 CPU**：内核若忙等，ring3 服务永远排不上来 -> 必然死锁。
//      所以每一圈都 task_yield64()（实测有效：服务在几百微秒内就把 state 改成 DONE）；
//   3) **不信任应答**：seq 必须原样回写（防串号），result 不许超过 data_secs 允许的字节数。
//
// 串口打点（自动验收按这些断言）：
//   [EXT4VOL] bind slot=<n> shm=<id> disk=<n> lba=<n> ok|FAILED reason=..
//   [EXT4VOL] rpc op=<stat|list|read> path=<p> result=<n> ms=<n> ok|FAILED err=<n>
//   [EXT4VOL] selftest list="/" count=<n> names=<a,b,c> ok|FAILED
//   [EXT4VOL] selftest stat="/big.bin" size=<n> type=<n> ok|FAILED
//   [EXT4VOL] selftest read="/big.bin" bytes=<n> crc=0x… ok|FAILED
//   [EXT4VOL] selftest timeout-probe=ok|FAILED（把上限压到 0 必须立刻超时，证明"不会卡死"）
#pragma once

#include <stdint.h>        // uint32_t/uint64_t（本头会被单独 include，不能指望调用方先引）
#include "r3drv_abi.h"

// 单次应答的数据区上限（一个 4 KiB 页 = 8 扇区）。LIST 64 条目录项、READ 4 KiB 都在一页内。
#define EXT4VOL_DATA_SECS   8u
#define EXT4VOL_MAX_ENTRIES 64
// RPC 上限：GUI 线程可忍受的最坏停顿（毫秒）。超时即报错，绝不重试。
#define EXT4VOL_RPC_MS      150u

// 是否已绑定到一个"活着且已发布请求块"的 ext4 服务（1 = 可以发 RPC）
int ext4vol64_ready64();

// 取路径属性：out_type = 1 文件 / 2 目录（对应 R3drvDirent64.type），out_size = 字节数
int ext4vol64_stat64(const char* path, uint32_t* out_type, uint32_t* out_size);

// 列目录：最多 max 条（≤ EXT4VOL_MAX_ENTRIES），*out_count = 实际条数，*out_more = 1 表示还有更多
int ext4vol64_list64(const char* path, R3drvDirent64* out, int max,
                     int* out_count, int* out_more);

// 读文件：从 off 读最多 len 字节到 buf；*out_got = 实际字节数（大文件按 4 KiB 分多次 RPC）
int ext4vol64_read64(const char* path, uint64_t off, void* buf, uint32_t len, uint32_t* out_got);

// 开机自检（只有 /etc/r3drv.conf 里 rpc=1 才跑；证明"内核真的通过 RPC 读到了 ext4 的字节"）
void ext4vol64_boot64();

// 单发 RPC 的统计（给 /proc 或诊断用；返回累计条数）
uint64_t ext4vol64_rpc_count64();
uint64_t ext4vol64_rpc_timeouts64();
