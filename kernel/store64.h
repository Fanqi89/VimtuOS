// store64.h - VimtuOS 64 位"设置持久化 store"（双槽 A/B + 世代号 + CRC32）
//
// 槽的**载体**（存在哪里）由 store64_init64 按"有没有可用卷"决定，槽格式与载体无关：
//   1) VFS 文件（**首选，正常运行**）：VimtuFS2 卷里的 /store.a、/store.b，各 16KB = 一个槽
//      （整槽 = vfs64_read / vfs64_write 的 16384 字节）。init 用 vfs64_stat("/") 探"卷挂没挂"。
//   2) 裸盘槽区（**仅降级路径**）：kernel/memlayout64.h 的 ML64_STORE_LBA(8009) 起 64 扇区 = 32KB，
//      对半切成两个槽：
//        槽 A = LBA 8009..8040（32 扇区 / 16KB）
//        槽 B = LBA 8041..8072（32 扇区 / 16KB）
//      ★ 这块区域与安装程序创建的主分区起点（LBA 8009）**完全重叠**，只在"没有可用卷"时使用，
//        init 必定打印 [STORE64] WARN raw slot area LBA 8009 overlaps the data partition;
//        use VFS-backed store。
//   每槽 = 头部（64B，含 magic/版本/世代号/payload 长度/payload CRC/头部 CRC）+ 一串 KV 记录。
//   一次 flush 只走一条路径（VFS 或裸盘），绝不同时写两处。
//
// 断电安全（写入策略）：
//   永远写"非活动槽"。VFS 载体靠 vfs64_write 自身的"写数据 -> 最后提交 inode"；
//   裸盘载体先写数据扇区（1..31）、最后写头部扇区（0，带 CRC）——头部写成功才算提交。
//   中途断电只会让目标槽 CRC 对不上 -> 下次 init 判它无效，回退到另一槽（旧设置完好）。
//   两槽都无效 -> 当作空 store（全默认值）。读写都是"整槽进静态缓冲 -> 在内存里解析/构造"。
//
// 内存/重入约定：
//   整槽缓冲与 KV 表全部是 .bss 静态数组，无 new/delete/malloc，无 libc 依赖；
//   flush 是唯一的写盘路径，调用方保证不在中断里调用（本模块自身不可重入）。
//
// ★ ATA 弱链接约定（重要，见 store64.cpp 顶部详注）：
//   裸盘降级路径对 ata64_read/ata64_write 用 __attribute__((weak)) 引用；VFS 载体不需要它
//   （磁盘 I/O 在 vfs64.cpp 里）。ATA 没链接进来时裸盘路径直接判失败（st_ata_linked()==false）：
//   flush 返回 -1 并打印 [STORE64] flush FAILED via=raw reason=ata not linked，绝不调用空指针。
#pragma once
#include <stdint.h>

#define STORE64_MAX_KEYS     64    // 单槽最多 KV 记录条数
#define STORE64_MAX_KEY_LEN  31    // key 最长字节数（可打印 ASCII，不含 '='）
#define STORE64_MAX_VAL_LEN  255   // value 最长字节数（UTF-8）

// 初始化：探测载体（VFS 优先）-> 读两槽 -> 选**有效且世代号更大**的槽作为活动槽；
// 两槽都无效 -> 空 store（全默认值）。打印 [STORE64] init via=vfs|raw|none slot=A|B|none gen=N keys=N。
// ata_drive 同 ata64.h：0=primary master, 1=primary slave, 2=secondary master, 3=secondary slave
// （只对裸盘降级路径有意义）。★ 想让 init 走 VFS：必须先 vfs64_mount（见 kernel/kernel64.cpp）。
void store64_init64(int ata_drive);

// 读键：返回 value 的**完整字节长度**（out 太小则截断，但仍返回完整长度并以 '\0' 结尾）；
// 键不存在 / key==nullptr / out==nullptr / out_max<=0 -> 返回 -1。
int store64_get64(const char* key, char* out, int out_max);

// 写内存（**不落盘**）：0 = 成功；-1 = key/value 非法（空 key、key>31B、value>255B、
// 非 UTF-8、含 '=' 的 key、含 CR/LF 的 value）、超过 64 条、或槽容量放不下。
int store64_set64(const char* key, const char* value);

// 落盘到**非活动槽**（载体相关：VFS 用 vfs64_write 整槽写 /store.<other>；裸盘先数据后头部的
// 断电安全顺序）+ 切换活动槽 + generation+1。
// 成功 0，并打印 [STORE64] flush via=vfs -> slot=B gen=<n> crc=0x<hex> ok（裸盘为 via=raw）；
// 失败返回 -1 并打印 [STORE64] flush FAILED via=<vfs|raw> reason=<...>（**不切槽**，旧槽不受影响）。
int store64_flush64();

// 位掩码自检（在内存里做，0 = 全过；需要真载体的部分在不可用时只打印跳过、不算失败）：
//   bit0(1) CRC32 已知向量   bit1(2) 插入/查找      bit2(4) 覆盖+截断读
//   bit3(8) 删除             bit4(16) 非法输入被拒  bit5(32) 超过 64 条被拒
//   bit6(64) 槽容量上限      bit7(128) 槽构造->解析往返
//   bit8(256) 坏槽检测(magic/头部CRC/payloadCRC)
//   bit9(512) 真载体探测：有卷 -> vfs64_write + vfs64_read 的整槽 16KB 往返（临时文件 /st64.tmp）；
//             没有卷 -> 裸盘只读探测
//   bit10(1024) 槽格式序列化/反序列化往返（**载体无关**，含尺寸契约与逐字节一致性）
int store64_selftest64();

// 串口打印全部 KV（每键一行 [STORE64] dump <key>=<value>，供自动验收 grep）
void store64_dump64();

// 当前活动槽的世代号（两槽都无效时为 0；每次成功 flush +1）
uint64_t store64_generation64();

// ---- 只读快照（终端 store 命令用；不改任何状态）----
int         store64_key_count64();      // 当前内存 KV 条数
const char* store64_slot_name64();      // 活动槽名："A" / "B" / "none"
const char* store64_carrier64();        // 当前载体："vfs" / "raw" / "none"
// 第 i 条记录（0 起）：key/value 拷进调用方缓冲（NUL 结尾）。返回 0 = 有值，-1 = 越界/参数错。
int         store64_entry64(int i, char* key_out, int key_max, char* val_out, int val_max);
