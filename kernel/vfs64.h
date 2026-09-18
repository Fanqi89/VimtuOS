// vfs64.h - VimtuFS2：Vimtu64 的极简但**真实**的磁盘文件系统（第一阶段：单层目录）
//
// 为什么自研格式而不是 FAT16/32：
//   本阶段的需求只有"格式化 / 挂载 / 建目录 / 写 / 读 / 删 / 列举 + 元数据自校验"，
//   不允许引入 libc / 堆 / 浮点。自研的 8 字段超级块 + 64B inode 规则全部写在源码里，
//   验收时可以直接按扇区对偏移做检查，比猜 FAT 的 BPB/簇链语义更省事。
//
// ---- 磁盘布局（相对**分区起始 LBA**；1 块 = 1 扇区 = 512B，块号从 0 开始）----
//   块 0                : 超级块（magic "VIMTUFS2"，末尾 0xAA55，自带 CRC32）
//   块 1 .. 1+bmn-1     : 空闲块位图（每块 512B = 4096 个块位，1 = 已用）
//   块 bp .. bp+ibn-1   : inode 表（固定 64B/个，inode 0 = 根目录）
//   块 dp .. 总块数-1   : 数据区（文件内容 + 一级间接块）
//   其中 bmn = ceil(总块数/4096)、ibn = ceil(inode 数/8)、bp = 1+bmn、dp = bp+ibn；
//   这些数字都写在超级块里，挂载时**逐项重算校验**（不信任盘上数字，见 sb_verify）。
//   inode 0 的所在扇区（= bp）就是"根目录 LBA"，format 打点里的 root= 是它的绝对 LBA。
//
// ---- inode（64B，见 vfs64.cpp 的 VFS_I_* 偏移注释）----
//   类型 / 名字长度 / 保留 / 大小 / 4 个直接块 / 1 个间接块 / 父目录 inode / 名字 / CRC32。
//   名字（≤27B ASCII）**直接放在 inode 里**：一个目录 = "父 inode 号相同的 inode 列表"。
//   取舍：省掉目录数据块与目录项结构（少一层指针、少一类越界），代价是条目数上限 =
//   inode 数（本阶段 256）、ls 是 O(inode 数) 线性扫描、名字上限 27B、没有硬链接。
//
// ---- 路径语义（阶段一，故意很小）----
//   只支持**单层**路径："/name" 或 "name"；"/"（或 ""）表示根目录。
//   多级路径、'.'/'..'、子目录里的文件都留到下一阶段（出现 '/' 一律返回 -1 并打点）。
//
// ---- 使用顺序 ----
//   vfs64_format(drive, start_lba, sectors)   // 建卷（成功后新卷即处于已挂载状态）
//   vfs64_mount(drive, start_lba)             // 或者挂载已有卷
//   vfs64_ls / read / write / mkdir / unlink / stat
//   vfs64_dump64()                            // 串口打当前状态（验收 grep 用）
//   vfs64_selftest64()                        // 64 扇区假盘自检 + 真盘只读探测
#pragma once
#include <stdint.h>

// ---- 卷头/几何常量（与 vfs64.cpp 的偏移注释一一对应）----
#define VFS64_MAGIC            "VIMTUFS2"   // 8B；老占位超级块是 "VIMTUFS1"，故意区分
#define VFS64_VERSION          2u
#define VFS64_SECTOR_BYTES     512u
#define VFS64_BLOCK_BYTES      512u         // 1 块 = 1 扇区（不做块缓存，够简单）
#define VFS64_INODE_BYTES      64u
#define VFS64_INODES_PER_BLK   8u
#define VFS64_NAME_MAX         27u          // inode 名字字段 28B（留 1B 兜底 '\0'）
#define VFS64_DIRECT_BLOCKS    4u
#define VFS64_INDIRECT_PTRS    128u         // 一级间接块 = 512B / 4B
#define VFS64_MAX_INODES       256u
#define VFS64_BITMAP_BLK_BITS  4096u        // 512B * 8
#define VFS64_MIN_BLOCKS       32u          // 格式化下限（16KB 分区）
#define VFS64_MAX_FILE_BYTES   (VFS64_DIRECT_BLOCKS * VFS64_BLOCK_BYTES + \
                                VFS64_INDIRECT_PTRS * VFS64_BLOCK_BYTES)   // 67584

// inode 类型
#define VFS64_TYPE_FREE        0u
#define VFS64_TYPE_FILE        1u
#define VFS64_TYPE_DIR         2u

// ---- API ----
// 格式化：写超级块 + 空闲块位图 + 清零 inode 区 + 根目录 inode。成功返回 0（并且卷已挂载），
// 失败返回 -1（已打印原因）。start_lba = 分区起始绝对 LBA；total_sectors = 分区扇区数。
int  vfs64_format(int drive, uint32_t start_lba, uint32_t total_sectors);

// 挂载：读扇区 0，校验 magic / CRC32 / 几何自洽（位图与 inode 区必须严丝合缝地接在数据区前）。
// 成功打印 "[VFS64] mount ok blocks=<n> inodes=<n> free=<n>"，失败打印 reason=<...>。
int  vfs64_mount(int drive, uint32_t start_lba);

// 列举目录。names 是调用方给的 [][32] 二维数组（最多填 max 条，名字 NUL 结尾），
// sizes[i] 是该条目字节数（目录为 0）。返回**实际填充的条目数**，路径非法/非目录返回 -1。
int  vfs64_ls(const char* path, char names[][32], int max, uint32_t* sizes);

// 读文件：最多把 max 字节拷进 buf，返回实际读到的字节数；不存在/是目录/越界返回 -1。
int  vfs64_read(const char* path, void* buf, int max);

// 写文件：不存在则创建，存在则**整体覆盖**（先分配新块并写完数据，提交新 inode 之后再释放旧块，
// 中途失败只可能泄漏块，绝不会让 inode 指向已释放的块）。返回写入字节数或 -1。
int  vfs64_write(const char* path, const void* buf, int len);

// 删除普通文件（目录必须先删空；本阶段不支持删目录，返回 -1）。
int  vfs64_unlink(const char* path);

// 建目录（父目录固定为根，阶段一）。
int  vfs64_mkdir(const char* path);

// 查属性：*type 取 VFS64_TYPE_FILE / VFS64_TYPE_DIR，*size 取字节数（目录为 0）。
// 两个输出指针都可以传 nullptr。返回 0 = 找到，-1 = 不存在/非法。
int  vfs64_stat(const char* path, uint32_t* type, uint32_t* size);

// 自检：64 扇区内存假盘上跑 格式化→写→读回比对→ls→stat→覆盖→unlink→越界/坏结构拒绝→
//       空间耗尽与回收；然后对真盘做**只读**探测（绝不在自检里格式化真盘）。
// 返回 0 = 全过，否则位掩码（bit0 格式化 / bit1 读写 / bit2 ls / bit3 stat / bit4 覆盖+删除 /
// bit5 边界与损坏拒绝 / bit6 空间耗尽与回收 / bit7 真盘探测），并打印 selftest PASS|FAIL mask=<n>。
int  vfs64_selftest64();

// 串口打印当前挂载状态 + 根目录条目（供自动验收 grep）。
void vfs64_dump64();
