// vfs64.h - VimtuFS2：Vimtu64 的极简但**真实**的磁盘文件系统
//           （v3 = 真正的目录树：多级路径 + inode 时间戳 + 类型判定）
//
// 为什么自研格式而不是 FAT16/32：
//   本阶段需求是"格式化 / 挂载 / 建多级目录 / 建文件 / 写 / 读 / 删 / 遍历 + 元数据自校验"，
//   不允许引入 libc / 堆 / 浮点。自研的 8 字段超级块 + 固定 inode 规则全部写在源码里，
//   验收时可以直接按扇区对偏移做检查，比猜 FAT 的 BPB/簇链语义更省事。
//
// ==================== 磁盘布局（相对**分区起始 LBA**；1 块 = 1 扇区 = 512B，块号从 0 开始）====================
//   块 0                : 超级块（magic "VIMTUFS2"，末尾 0xAA55，自带 CRC32）
//   块 1 .. 1+bmn-1     : 空闲块位图（每块 512B = 4096 个块位，1 = 已用）
//   块 bp .. bp+ibn-1   : inode 表（固定大小记录，见下；inode 0 = 根目录）
//   块 dp .. 总块数-1   : 数据区（文件内容 + 一级间接块）
//   其中 bmn = ceil(总块数/4096)、ibn = ceil(inode 数/每块 inode 数)、bp = 1+bmn、dp = bp+ibn；
//   这些数字都写在超级块里，挂载时**逐项重算校验**（不信任盘上数字，见 sb_verify）。
//
// ==================== 卷版本（**唯一定义点就是本节 + vfs64.cpp 的 LAY_V2/LAY_V3**）====================
//   版本字段：超级块偏移 8（u32）；inode 记录大小：超级块偏移 44（u32）。
//   * **v3（当前，vfs64_format 产出）= 本文档描述的格式**：
//       超级块：块 0，偏移见 vfs64.cpp 的 VFS_O_*（CRC32 覆盖 [0,60)，签名 0xAA55）
//       inode ：**128 B/个**（每块 4 个），布局见下
//       名字上限 31 B（足够喂满旧调用方的 [32] 缓冲）、inode 上限 512、单文件上限 67584 B
//       目录树：**多级路径**，目录 = "parent 字段相同的 inode 列表"（见"目录表示"）
//   * **v2（旧卷，仍能挂载）**：超级块布局相同（版本=2、inode=64 B/个、8 个/块、
//       名字上限 27 B、**没有 mtime/kind/nlink 字段**）。挂载后按"单层语义"工作
//       （v2 卷里所有条目的 parent 都是 0，所以多级路径照样能解析，只是建不出子目录树）。
//       不拒绝 v2：现有已安装的系统盘就是 v2（回归测试依赖它）；新格式化一律产出 v3。
//
// ---- v3 inode（128 B，全部小端；偏移为**唯一权威定义**，vfs64.cpp 有 static_assert）----
//   0   u8  type      0 = 空槽 / 1 = 普通文件 / 2 = 目录
//   1   u8  namelen   名字字节数 0..31（0 只用于根目录）
//   2   u16 rsvd      必须为 0
//   4   u32 size      文件字节数（目录恒为 0：目录没有数据块）
//   8   u32 d0        直接块 0
//   12  u32 d1        直接块 1
//   16  u32 d2        直接块 2
//   20  u32 d3        直接块 3
//   24  u32 ind       一级间接块（512B / 4B = 128 个块号）
//   28  u32 parent    父目录 inode 号（根目录 = 自己 = 0）
//   32  u32 mtime     **修改时间**，打包编码见下；0 = 未知（v2 卷/无 RTC）
//   36  u16 nlink     链接数：普通文件 = 1；目录 = 1 + 直接子目录数（只维护计数，**不提供链接原语**）
//   38  u8  kind      类型判定缓存（VFS64_KIND_*，写文件时按内容算好，供 UI 直接用）
//   39  u8  rsvd2     必须为 0
//   40  31B name      名字（ASCII 可打印，NUL 不写盘，长度看 namelen）
//   71  ..  123       保留区，必须全 0
//   124 u32 crc32     inode CRC32（覆盖 [0,124)）
//   **mtime 打包编码**：(年-2000)<<26 | 月<<22 | 日<<17 | 时<<12 | 分<<6 | 秒
//     （6+4+5+5+6+6 = 32 位；年 2000..2063、月 1..12、日 1..31、时 0..23、分/秒 0..59；
//      与 kernel/x86_64.cpp 的 rtc_get_date64（月 1..12、年已加 2000）口径一致）
//
// ---- 目录表示（取舍写清）----
//   名字**放在 inode 里**（不另设目录项结构、目录没有数据块）："目录" = 父 inode 号等于它的
//   那批 inode 的集合。取舍：
//     好处：少一层指针（目录数据块）→ 少一类越界与一类坏链；一个目录天然支持多个子项；
//           读写/删除路径完全复用文件那套块管理；格式小到一页纸能写清。
//     代价：条目总数上限 = inode 数（512）；列目录/查名字是 O(inode 数) 线性扫描
//           （vfs64.cpp 用一个"inode 所在扇区"缓存把扫描的读盘次数降到 1/4）；
//           **没有硬链接/符号链接**；目录不能有"第二个名字"。
//
// ---- 路径语义（v3 起；大小写敏感、ASCII）----
//   * 绝对路径 "/dir/sub/file"；相对路径 "dir/file" 等价于 "/dir/file"
//     （VFS 层没有"当前目录"概念 —— 终端/应用自己维护 cwd，见 kernel/fd64.h）。
//   * 分隔符 '/'：连续的 "//" 等价于一个；结尾的 '/' 忽略；"" 与 "/" 都是根目录。
//   * "." = 当前目录（跳过）；".." = 父目录（**根目录的父目录还是根目录**，POSIX 语义）。
//   * 单段长度 ≤ 该卷的名字上限（v3 = 31 B、v2 = 27 B）；整条路径 ≤ VFS64_PATH_MAX(128) B；
//     路径段数 ≤ VFS64_PATH_DEPTH_MAX(16)。
//   * 段内字符必须是可打印 ASCII（0x21..0x7E，'/' 除外）：不合法一律 -1 + 打点。
//   * 所有索引（inode 号 / 块号 / 段下标）**先范围校验再用**，越界一律打点 + 返回 -1，绝不越界读写。
//
// ---- 如实标注的限制 ----
//   * 单文件上限 **67584 B**（4 个直接块 + 1 个一级间接块 × 512B）—— 本轮**没有**加二级间接块，
//     所以格式升级没有提高文件上限（提高它需要新的块索引层，留待后续；见 fd64.h 的同口径说明）。
//   * 名字上限 31 B（v3）/ 27 B（v2）；inode 总数上限 512；目录深度上限 16 层；
//     无权限/属主、无硬链接、无符号链接、无稀疏文件、无日志/崩溃一致性（只有"先数据后 inode"的提交顺序）。
//   * 删除：文件可以删（vfs64_unlink64）；**空目录**可以删（vfs64_rmdir64）；非空目录必须自己先清空。
//   * 兼容 API vfs64_ls 的名字缓冲是 [32]，实现里把超过 31 B 的名字截断（v3 上限就是 31，所以
//     实际不会截断）；新代码请用 vfs64_list64 / opendir+readdir（报告完整名字 + 类型 + 时间）。
//
// ---- 使用顺序 ----
//   vfs64_format(drive, start_lba, sectors)   // 建 v3 卷（成功后新卷即处于已挂载状态）
//   vfs64_mount(drive, start_lba)             // 或者挂载已有卷（v2/v3 都认）
//   vfs64_list64 / read64 / write64 / mkdir64 / unlink64 / rmdir64 / stat64 / create64
//   vfs64_opendir64 / readdir64 / closedir64  // 游标式遍历（适合长列表，不一次读爆缓冲）
//   vfs64_tree_dump64("/", 32, 4)             // 串口打目录树（验收 grep；有界）
//   vfs64_dump64()                            // 串口打当前状态
//   vfs64_selftest64()                        // 64 扇区假盘自检 + 真盘只读探测
#pragma once
#include <stdint.h>

// ---- 卷头/几何常量（与 vfs64.cpp 的偏移注释一一对应）----
#define VFS64_MAGIC            "VIMTUFS2"   // 8B；老占位超级块是 "VIMTUFS1"，故意区分
#define VFS64_VERSION          3u           // 新格式化产出
#define VFS64_VERSION_V2       2u           // 旧卷（仍可挂载）
#define VFS64_SECTOR_BYTES     512u
#define VFS64_BLOCK_BYTES      512u         // 1 块 = 1 扇区（不做块缓存，够简单）
#define VFS64_INODE_BYTES      128u         // v3 inode 记录大小
#define VFS64_INODES_PER_BLK   4u           // 512 / 128
#define VFS64_INODE_BYTES_V2   64u          // v2 inode 记录大小
#define VFS64_INODES_PER_BLK_V2 8u          // 512 / 64
#define VFS64_NAME_MAX         31u          // v3 名字上限（[32] 兼容缓冲够用）
#define VFS64_NAME_MAX_V2      27u          // v2 名字上限
#define VFS64_DIRECT_BLOCKS    4u
#define VFS64_INDIRECT_PTRS    128u         // 一级间接块 = 512B / 4B
#define VFS64_MAX_INODES       512u         // inode 总数上限（v3 格式化上限；v2 卷历史上最多 256）
#define VFS64_BITMAP_BLK_BITS  4096u        // 512B * 8
#define VFS64_MIN_BLOCKS       32u          // 格式化下限（16KB 分区）
#define VFS64_PATH_MAX         128u         // 整条路径字节上限
#define VFS64_PATH_DEPTH_MAX   16u          // 路径段数上限
#define VFS64_LS_NAME_BUF      32u          // 兼容 API vfs64_ls 的名字缓冲（旧调用方写死 32）
#define VFS64_DIRSTREAM_MAX    8u           // 同时打开的目录游标数
#define VFS64_MAX_FILE_BYTES   (VFS64_DIRECT_BLOCKS * VFS64_BLOCK_BYTES + \
                                VFS64_INDIRECT_PTRS * VFS64_BLOCK_BYTES)   // 67584
#define VFS64_SLOT_MAX         4u           // ★ 多卷：卷槽数（0 = 系统卷，另外最多 3 个数据卷 -> D:/E:/F:）

// inode 类型（磁盘字段 type）
#define VFS64_TYPE_FREE        0u
#define VFS64_TYPE_FILE        1u
#define VFS64_TYPE_DIR         2u

// 类型判定（磁盘字段 kind；供 UI 直接显示，判定规则见 vfs64.cpp 的 vfs64_kind_of_data64）
#define VFS64_KIND_NONE        0u   // 未知/未判定
#define VFS64_KIND_FILE        1u   // 普通文件（内容不像已知类型）
#define VFS64_KIND_DIR         2u   // 目录
#define VFS64_KIND_VAP         3u   // VAP64 应用（头 8B = "VAP64\0\0\0"）
#define VFS64_KIND_ELF         4u   // ELF64 程序（头 4B = 7F 'E' 'L' 'F'）
#define VFS64_KIND_TEXT        5u   // 文本（扩展名 .txt/.md/.cfg 或可打印字符启发式）
#define VFS64_KIND_BIN         6u   // 二进制
#define VFS64_KIND_MAX         6u

// ==================== 元数据（vfs64_stat64 / list64 / readdir64 共用）====================
struct Vfs64Info64 {
    uint32_t index;                       // inode 号
    uint32_t type;                        // VFS64_TYPE_*
    uint32_t size;                        // 字节数（目录 = 0）
    uint32_t mtime;                       // 打包时间戳（0 = 未知；v2 卷恒为 0）
    uint32_t parent;                      // 父目录 inode 号
    uint32_t nlink;                       // 链接数（v2 卷恒报 1）
    uint32_t kind;                        // VFS64_KIND_*
    char     name[VFS64_NAME_MAX + 1];    // NUL 结尾（根目录 = ""）
    uint32_t name_len;
};

// 目录项（遍历用；不带 parent/nlink —— 省栈省拷贝）
struct Vfs64Dirent64 {
    char     name[VFS64_NAME_MAX + 1];
    uint32_t index;
    uint32_t type;
    uint32_t size;
    uint32_t mtime;
    uint32_t kind;
};

// 解包后的时间（供 UI 显示；year 是真实年份，例如 2025）
struct Vfs64Time64 {
    uint16_t year;
    uint8_t  month, day, hour, minute, second;
};

// 卷信息（只读探测 / 盘符层用）
struct Vfs64VolInfo64 {
    uint32_t version;        // 2 / 3
    uint32_t blocks;         // 总块数（= 分区扇区数）
    uint32_t inodes;         // inode 总数
    uint32_t inode_bytes;    // 单个 inode 字节数（v2 = 64、v3 = 128）
    uint32_t bitmap_start;
    uint32_t bitmap_blocks;
    uint32_t data_start;
    uint32_t data_blocks;
    uint32_t free_blocks;    // 数据区空闲块数
};

// ==================== 时间戳 / 类型判定（纯函数，UI 与工具都能直接用）====================
// 打包/解包 mtime（编码见文件头）；参数越界时返回 0（= 未知）。
uint32_t vfs64_pack_time64(int year, int month, int day, int hour, int minute, int second);
void     vfs64_unpack_time64(uint32_t packed, Vfs64Time64* out);
// 读 RTC 打包成 u32；RTC 值不合法（月 0/13、日 0…）时回退到固定值并打一次打点，**绝不返回 0**。
uint32_t vfs64_now64();
// 打包值 -> 字符串：ymd 至少 11 B（"YYYY-MM-DD"）、hms 至少 9 B（"HH:MM:SS"）；0 -> "0000-00-00"/"00:00:00"
void     vfs64_time_str64(uint32_t packed, char* ymd, int ymd_cap, char* hms, int hms_cap);
// 按内容 + 名字判定类型（写文件时就调用，结果存进 inode 的 kind 字段）
uint32_t vfs64_kind_of_data64(const void* data, uint32_t len, const char* name, uint32_t name_len);
// 只按类型 + 名字判定（读 v2 卷/没有内容时用；保守：只认扩展名）
uint32_t vfs64_kind_by_name64(uint32_t type, const char* name, uint32_t name_len);
// kind -> 稳定小写字符串（"dir"/"file"/"vap"/"elf"/"text"/"bin"/"unknown"）
const char* vfs64_kind_str64(uint32_t kind);

// ==================== 卷生命周期 ====================
// 格式化：写超级块 + 空闲块位图 + 清零 inode 区 + 根目录 inode（**v3**）。成功返回 0（并且卷已挂载），
// 失败返回 -1（已打印原因）。start_lba = 分区起始绝对 LBA；total_sectors = 分区扇区数。
// 打点：[VFS64] format ok blocks=<n> version=3 inode=<128> root=<绝对 LBA>
int  vfs64_format(int drive, uint32_t start_lba, uint32_t total_sectors);

// 挂载：读扇区 0，校验 magic / CRC32 / 版本（2 或 3）/ 几何自洽（位图与 inode 区必须严丝合缝地接在数据区前）。
// 成功打印 "[VFS64] mount ok blocks=<n> inodes=<n> free=<n> version=<v> inode=<n>B"，失败打印 reason=<...>。
int  vfs64_mount(int drive, uint32_t start_lba);

// 只读探测：读 (drive, start_lba) 的超级块并校验（含几何重算与空闲块统计），把几何填进 *out。
// **不改任何挂载状态**（drive64 枚举所有盘时用）。返回 0 = 是合法 VimtuFS2 卷；-1 = 不是/读不到。
int  vfs64_probe_volume64(int drive, uint32_t start_lba, Vfs64VolInfo64* out);

// 当前挂载卷的身份（drive + 起始 LBA）；未挂载返回 -1。out 参数都可传 nullptr。
int  vfs64_mounted_volume64(int* drive, uint32_t* start_lba, Vfs64VolInfo64* out);

// ==================== ★ 多卷（卷槽表 + 按盘符/槽切换）====================
// 模型（为什么这样设计见 vfs64.cpp 的"多卷"一节）：
//   * 卷槽表 g_vol[0..VFS64_SLOT_MAX-1]：每个槽独立保存一份卷几何（drive/起始 LBA/位图/inode/数据区/布局）。
//   * g_cur_slot = **当前卷**：所有旧 API（vfs64_stat64/read64/write64/ls/opendir…）都作用于当前卷 —— 
//     fd64/终端/explorer 不必改调用点（"当前卷"就是用户在文件管理器里点进去的那块盘）。
//   * 系统组件（store64/config64/update64/app64/elf64/proc64/sysstate64）**必须**用
//     vfs64_*_on64(vfs64_system_slot64(), ...)：写盘只在这一次调用期间临时切到系统卷槽，
//     调用返回前原样切回（不可重入计数 + LIFO 恢复）—— 用户在浏览 D: 时 3 秒自动落盘也不会写到 D:。
//   * inode 扇区缓存按 (slot, drive, lba) 三元组键控（见 vfs64.cpp），切卷绝不会读到上一个卷的字节。

// 挂系统卷：挂进 **0 号槽**并激活它，同时把它记成"系统卷槽"（store64 等固定写卷的依据）。返回 0/-1。
int  vfs64_mount_system64(int drive, uint32_t start_lba);

// 把一个卷挂进指定槽（**不改变**当前卷）。失败时该槽保持调用前的状态。返回 0/-1。
// 打点：[VFS64] mount slot=<n> ok blocks=.. / mount slot=<n> FAILED reason=<..>
int  vfs64_mount_slot64(int slot, int drive, uint32_t start_lba);

// 激活某个槽（= 把"当前卷"切到这个槽）。0 = 成功；-1 = 槽号非法/该槽没挂载。
// 打点：[VFS64] activate slot=<n> drive=<d> start=<lba> blocks=<n> version=<v>
int  vfs64_activate_slot64(int slot);

// 当前卷槽号；没有挂载任何卷时返回 -1。
int  vfs64_current_slot64();

// 系统卷槽号（C:；固定写卷用）。从未挂载/格式化过系统卷时返回 -1。
int  vfs64_system_slot64();

// 槽占用查询：1 = 该槽已挂载一个卷；0 = 空槽/槽号非法。
int  vfs64_slot_used64(int slot);

// 找一个**空槽**（不挂载、不激活）；-1 = 卷表满（调用方必须如实拒绝，不能偷偷覆盖已有卷）。
int  vfs64_slot_alloc64();

// (drive, start_lba) 那个卷在哪个槽；-1 = 没挂载过。drive64 靠它做"重扫幂等"。
int  vfs64_slot_find64(int drive, uint32_t start_lba);

// 槽的卷信息（几何 + 剩余块数，**只读**，不切换当前卷、不碰挂载状态）。
// drive/start_lba 可传 nullptr。0 = 该槽有卷；-1 = 空槽/槽号非法。
// 串口打印卷槽表（终端 `vol` 与自动验收用，每槽一行：used/drive/start/blocks/version/free）。
void vfs64_slots_dump64();

int  vfs64_slot_info64(int slot, int* drive, uint32_t* start_lba, Vfs64VolInfo64* out);

// ---- 按槽操作（显式卷号；语义与同名旧 API 逐字一致，只是"当前卷"在调用期间临时换成 slot）----
// 这些是**系统组件固定写系统卷**的入口：调用返回前一定把当前卷切回去（见 .cpp 的守卫实现）。
int  vfs64_stat_on64(int slot, const char* path, uint32_t* type, uint32_t* size);
int  vfs64_read_on64(int slot, const char* path, void* buf, int max);
int  vfs64_write_on64(int slot, const char* path, const void* buf, int len);
int  vfs64_mkdir_on64(int slot, const char* path);
int  vfs64_create_on64(int slot, const char* path);
int  vfs64_unlink_on64(int slot, const char* path);
int  vfs64_rmdir_on64(int slot, const char* path);
int  vfs64_ls_on64(int slot, const char* path, char names[][VFS64_LS_NAME_BUF], int max, uint32_t* sizes);
int  vfs64_stat64_on64(int slot, const char* path, Vfs64Info64* out);
int  vfs64_list64_on64(int slot, const char* path, Vfs64Dirent64* out, int max, uint32_t* cursor);
int  vfs64_tree_dump64_on64(int slot, const char* path, int max_entries, int max_depth);

// ==================== v3 新 API（多级路径）====================
// 查属性（含 type/size/mtime/parent/nlink/kind/名字）。返回 0 = 找到；-1 = 不存在/非法/未挂载。
int  vfs64_stat64(const char* path, Vfs64Info64* out);

// 列目录（**路径版 + 游标分页**）：从 *cursor 开始最多读 max 条进 out[]，并把 *cursor 推进到下一个待读位置。
// 返回填充条数（0 = 枚举结束）；-1 = 路径非法/不是目录/未挂载。cursor 可传 nullptr（= 从 0 开始，只读一页）。
// 不排序（按 inode 号顺序 = 创建顺序），条目多时调用方自己分页，别一次要一大片。
int  vfs64_list64(const char* path, Vfs64Dirent64* out, int max, uint32_t* cursor);

// 目录游标（opendir/readdir/closedir；句柄是内部静态表下标 0..VFS64_DIRSTREAM_MAX-1）。
// readdir 每次只扫一个 inode（游标推进），所以长列表不会被一次性拷进大缓冲。
int  vfs64_opendir64(const char* path, int* out_handle);
int  vfs64_readdir64(int handle, Vfs64Dirent64* out);      // 1 = 一条；0 = 枚举结束；-1 = 错
int  vfs64_closedir64(int handle);

// 建目录（**多级**：父目录必须已存在，否则 -1 + 打点）
int  vfs64_mkdir64(const char* path);

// 建空文件（父目录必须存在；已存在且是文件 = 返回 0 幂等；是目录 = -1）
int  vfs64_create64(const char* path);

// 写文件（不存在则创建，存在则**整体覆盖**）：父目录必须存在。语义与旧 vfs64_write 完全一致
// （先分配新块 + 写完数据 + 提交新 inode，最后才释放旧块 —— 中途失败只泄漏块，绝不指向已释放的块）。
// 顺带写入 mtime 与 kind。返回写入字节数或 -1。
int  vfs64_write64(const char* path, const void* buf, int len);

// 读文件：最多把 max 字节拷进 buf，返回实际读到的字节数；不存在/是目录/越界返回 -1。
int  vfs64_read64(const char* path, void* buf, int max);

// 删除**普通文件**（目录一律拒绝并打点：用 vfs64_rmdir64）。
int  vfs64_unlink64(const char* path);

// 删除**空目录**（非空/根目录/不存在一律 -1 + 打点；成功后父目录 nlink-1）。
int  vfs64_rmdir64(const char* path);

// 目录树串口打印（有界）：从 path 开始（递归 ≤ max_depth 层、全树 ≤ max_entries 条），每行
//   [VFS64] tree <完整路径> type=<dir|file> size=<n> mtime=0x<hex> ymd=<YYYY-MM-DD> hms=<HH:MM:SS> kind=<str> idx=<inode>
// 返回打印的条目数；越界/不存在返回 -1（并打点）。供启动自检与自动验收 grep。
int  vfs64_tree_dump64(const char* path, int max_entries, int max_depth);

// ==================== 兼容 API（旧单层调用方一行都不用改：内部转调新实现）====================
// 列举目录：names 是调用方给的 [][32] 二维数组（最多填 max 条，名字 NUL 结尾，**超过 31 B 截断**），
// sizes[i] 是该条目字节数（目录为 0）。返回实际填充条数，路径非法/非目录返回 -1。
int  vfs64_ls(const char* path, char names[][VFS64_LS_NAME_BUF], int max, uint32_t* sizes);
int  vfs64_read(const char* path, void* buf, int max);      // = vfs64_read64
int  vfs64_write(const char* path, const void* buf, int len); // = vfs64_write64
int  vfs64_unlink(const char* path);                        // = vfs64_unlink64（目录拒绝）
int  vfs64_mkdir(const char* path);                         // = vfs64_mkdir64（现在支持多级）
// 查属性：*type 取 VFS64_TYPE_FILE / VFS64_TYPE_DIR，*size 取字节数（目录为 0）。两个指针都可传 nullptr。
int  vfs64_stat(const char* path, uint32_t* type, uint32_t* size);

// 自检（位掩码，0 = 全过；打印 [VFS64] selftest PASS|FAIL mask=<n>）：
//   bit0(1)    假盘格式化 + 挂载（v3）
//   bit1(2)    写 3000B（跨直接+间接块）读回逐字节比对
//   bit2(4)    根目录列举
//   bit3(8)    stat（类型/大小/不存在）
//   bit4(16)   覆盖写 + unlink
//   bit5(32)   边界与损坏拒绝（越界块号 / 坏 magic / 坏 CRC / 非法入参 / 超长名字）
//   bit6(64)   空间耗尽与删除后回收
//   bit7(128)  真盘**只读**探测（绝不在自检里格式化真盘）
//   bit8(256)  **多级目录树**：mkdir /efi、/apps、/apps/demo、父目录不存在被拒、在 /apps/demo 里写文件、
//              遍历 / 与 /apps/demo、stat64 的类型/大小/parent
//   bit9(512)  **路径语义**："." 与 ".."、/../.. = 根、超长段被拒、超深路径被拒、坏路径不崩
//   bit10(1024) **mtime 与类型判定**：写入后 mtime 非 0 且字段在合法范围；.txt -> TEXT、VAP64 头 -> VAP、
//              ELF 头 -> ELF、二进制 -> BIN
//   bit11(2048) **代际/几何 + 重挂载持久化**：超级块版本=3、inode=128B、几何自洽；重新 mount 后子目录文件仍在
int  vfs64_selftest64();

// 串口打印当前挂载状态 + 根目录条目（供自动验收 grep）。
void vfs64_dump64();
