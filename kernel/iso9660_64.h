// iso9660_64.h - ISO9660（光盘/ISO 镜像）**只读**文件系统
//
// 为什么需要它（v0.9.26）：
//   到 v0.9.25 为止，本系统的"光驱支持"只到"能把载荷从光盘拷到硬盘"这一层：
//     * kernel/ata64.cpp 的 ATAPI PACKET + READ(10) 会读光盘（2048B/扇区）；
//     * 但内核里**没有任何 ISO9660 解析**（0 处 PVD/目录记录代码），
//       光盘插进去在文件管理器里看不见东西；
//     * 安装程序也不靠文件系统找载荷，而是靠引导桩回填的固定 LBA（MediumDesc）。
//   本文件补上"把光盘当文件系统读"这一段：PVD -> 根目录 -> 逐级目录记录 -> 文件内容。
//
// ==================== 只做 ISO9660 主力卷（不做的东西如实列在这里）====================
//   做：
//     * 主卷描述符（PVD，光盘 LBA 16，2048B/扇区）：卷标识 / 卷空间大小 / 根目录记录；
//     * 目录记录遍历（记录长 + 名字长度 + extent/size 的两个字节序副本都读并**交叉校验**）；
//     * 路径解析（逐级查找，名字比较**大小写不敏感**、"FOO.TXT;1" 的 ";1" 版本后缀忽略）；
//     * 文件按字节读（extent LBA × 2048 + 偏移，跨块自动分次读）。
//   不做（不是遗漏，是本批的边界）：
//     * Joliet（SVD）与 Rock Ridge（RR/PX/NM）：光盘上的**主卷名字仍是 8.3 大写**形式，
//       所以带长名字/RR 的盘也能按主名打开，只是名字长短与原盘不完全一致；
//     * 多段（multi-session）、UDF、El Torito 目录（引导目录不是文件系统的一部分）；
//     * 写入：光盘是只读介质，所有写操作由上层（fs64）直接 -EROFS，不进来。
//
// ==================== 打点（验收按行 grep）====================
//   [ISO9660] mount ok cd=<驱动器号> vol=<卷标识> blocks=<2048B 块数> bytes=<n> root=<extent>/<size>
//   [ISO9660] mount FAIL cd=<驱动器号> why=<no-pvd|bad-magic|bad-record|read-error>
//   [ISO9660] ls path=<路径> entries=<n>                        （每次列目录一行）
//   [ISO9660] selftest PASS / FAIL mask=<n>
//       mask：1=挂载失败 2=根目录不可列 4=根目录不空却列到 0 条 8=按路径找不到已知文件
//            16=读内容失败 32=PVD 字段自相矛盾（两个字节序副本不一致）
//   [ISO9660] read path=... off=... len=... got=...             （自检里读文件时一行）
#pragma once
#include <stdint.h>

#define ISO64_NAME_MAX   64u     // 名字缓冲（8.3 + 后缀足够；Joliet 长名不解析，如实不保证）
#define ISO64_VOLID_MAX  33u     // 卷标识（PVD 偏移 40 的 32 字节）
#define ISO64_EROFS      30      // -errno：只读（与 fs64 的 FS64_EROFS 同值）

// 一条目录项（上层 fs64 再转成自己的 Fs64Dirent64）
struct Iso64Dirent64 {
    char     name[ISO64_NAME_MAX];
    uint32_t extent;      // 起始**光盘块号**（每块 2048B）
    uint32_t size;        // 字节数（**目录恒为 0**，与 vfs64 口径一致）
    uint32_t size_raw;    // 原始字节数（目录也有它：路径解析/遍历要用真实长度）
    uint32_t is_dir;      // 1 = 目录
    uint32_t mtime;       // 打包时间（vfs64 口径）；ISO 的时间字段解析不了就给 0（如实）
};
struct Iso64Stat64 {
    uint32_t type;        // 1 = 普通文件 / 2 = 目录（与 VFS64_TYPE_* 同值）
    uint32_t size;
    uint32_t extent;
    uint32_t mtime;
};

// 挂载第 cd_idx 个光驱（0 = 第一个探测到的光驱，见 ata64_cd_drive64）。0 = 成功，-1 = 失败。
// 幂等：已经挂在同一个光驱上直接返回 0；换光驱会重新读 PVD（换盘后重挂）。
int iso966064_mount64(int cd_idx);
// 已挂载光驱的驱动器号（-1 = 没挂）。上层用它去读（同时用于 fs64 的卷表登记）。
int iso966064_cd_drive64();
// 已挂载卷的总容量（2048B 块数与字节数；未挂载时写 0）
int iso966064_volume64(uint32_t* out_blocks, uint32_t* out_bytes);
// 卷标识（PVD 偏移 40；未挂载时空串）
const char* iso966064_volid64();

// 列目录（游标分页，语义与 vfs64_list64 一致）：返回填充条数 / 0 = 结束 / -1 = 不是目录或读失败。
int iso966064_list64(const char* path, Iso64Dirent64* out, int max, uint32_t* cursor);
// 查属性：0 = 成功，-1 = 不存在。
int iso966064_stat64(const char* path, Iso64Stat64* out);
// 按字节读：0 = 成功（*out_got = 实际读到的字节数，读到文件尾会小于 len）；-1 = 失败 / 是目录(-21)。
int iso966064_read64(const char* path, uint32_t off, void* buf, uint32_t len, uint32_t* out_got);

// 自检：把已挂载的光盘读一遍（PVD 字段一致性 + 根目录 + 已知文件）；打 [ISO9660] selftest 行。
// 没挂载光盘时返回 0 但**不打 PASS**（打 mount 阶段的 FAIL 行由调用方负责）。
int iso966064_selftest64();
