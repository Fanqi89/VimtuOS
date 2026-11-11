// r3drv_abi.h - ★ v0.9.33：ring3 驱动框架的 **ABI 单一定义**（内核 C++ 与用户态 C 共用这一份）
//
// 为什么单独一个文件并且**一个 #include 都没有**：
//   * 用户态是无 libc 的 freestanding（见 tools/vmtool_build_win.sh 的 -nostdinc），
//     拿不到 <stdint.h>；内核侧是 C++。两边要共享**逐字节一致**的结构布局，
//     唯一的干净做法是"只用与语言无关的标量类型 + 常量"，谁也不引别人的头。
//   * 布局一旦分叉（比如用户态把 lba 写成 u32），零拷贝环就会读出垃圾而没有任何编译错误。
//     所以本文件只允许出现：宏、以及"unsigned int / unsigned long long / char"组成的 POD 结构。
//
// ==================== 通道形态（一句话）====================
// 服务进程 shm_create 一块对象（≥ 8 KiB）→ shm_map 进自己地址空间 → r3drv_register 把
// 「名字 / 协议 / 盘号 / 对象 id」报给内核。之后：
//   内核 -> 服务：AHCI 的 DMA 直接把盘上的数据写进**这块对象的物理帧**（一帧一项 PRDT，
//                 全程 0 次 memcpy）；服务已经映射着它，落地即可读。
//   服务 -> 内核：在对象的环里写 SQE（lba/secs/目标偏移），调一次 r3drv_kick 当门铃，
//                 内核消费完把 SQE.state 就地改成 2（完成）/ 3（失败）。
// 所以"SQE 环"是控制面（每批一次 syscall），"对象的帧"是数据面（DMA 直达，零拷贝）。
//
// ==================== 对象内存布局 ====================
//   [0   , 56 )                  R3drvRing（环控制块；magic 必须由服务先写）
//   [64  , 64+32*R3DRV_SLOTS )   SQE 数组（就地完成）
//   [4096, 对象大小 )            数据区——**只有这一段允许当 DMA 目标**（obj_off 必须
//                                ≥ R3DRV_DATA_OFF 且 4096 对齐，因为帧号 = obj_off/4096）
#pragma once

// ---------- 管理面 ABI 版本 / 魔数 ----------
#define R3DRV_ABI_VER      1u
#define R3DRV_RING_MAGIC   0x51335652u   // 'RV3Q'（小端）；服务写进对象 +0，内核校验
#define R3DRV_SQ_OFF       64u           // SQE 数组偏移
#define R3DRV_SLOTS        8u            // 环深（必须是 2 的幂：索引用 & (SLOTS-1)）
#define R3DRV_DATA_OFF     4096u         // 数据区起点（页对齐 = 帧对齐）

// ---------- SQE.state ----------
#define R3DRV_SQ_FREE      0u            // 空槽
#define R3DRV_SQ_PENDING   1u            // 服务已提交，内核未处理
#define R3DRV_SQ_DONE      2u            // 内核已完成（bytes 有效）
#define R3DRV_SQ_FAILED    3u            // 内核已失败（bytes = 已完成的部分）

// ---------- SQE.flags ----------
// bit0 = 1：走"拷贝对照路径"（内核先 DMA 到自己的暂存页，再 memcpy 进对象帧）。
// 这条路径**只用于取证**：它证明零拷贝路径省掉的是真的那份 CPU 拷贝，而不是幻觉。
// 正常驱动一个字节都不该置这位。
#define R3DRV_F_COPY       1u

// ★ v0.9.40：bit1 = 1 -> **方向反过来**：把对象帧里的字节写到盘上（lba 起、secs 扇区），
//   DMA 从共享对象的帧流向盘。**只许追加**：bit0 的语义一个字都不改，老服务不置这一位
//   -> 行为与 v0.9.33 的只读通道完全一致。
// 如实边界：写路径同样要求 obj_off 4 KiB 对齐；内核**不做** FLUSH CACHE，也不做任何
//   卷级事务 —— 落盘顺序（数据 -> 元数据）由 ring3 服务自己负责。
#define R3DRV_F_WRITE      2u

// ---------- 协议（= 分区上的文件系统类型）----------
#define R3DRV_PROTO_NONE   0u
#define R3DRV_PROTO_EXT4   1u
#define R3DRV_PROTO_VIMTU  2u            // VimtuFS2（内核自带，不走 ring3）
#define R3DRV_PROTO_FAT32  3u
#define R3DRV_PROTO_MAX    4u

// ---------- 槽位状态 ----------
#define R3DRV_ST_FREE      0u            // 槽空
#define R3DRV_ST_DYING     1u            // 内核已拉起进程，等服务 register
#define R3DRV_ST_RUN       2u            // 服务已注册且心跳正常
#define R3DRV_ST_STALE     3u            // 心跳超时（内核准备重启）
#define R3DRV_ST_FAILED    4u            // 重启超过上限：放弃，卷退回只读安全态

// ---------- 路由结果（r3drv_route 返回值）----------
#define R3DRV_ROUTE_DRIVER 0             // 有活着的 ring3 驱动：走驱动
#define R3DRV_ROUTE_RO     1             // 认得出文件系统但没有驱动：只读安全态
#define R3DRV_ROUTE_UNKNOWN 2            // 认不出：当只读裸卷

// ---------- 私有 syscall 号（只许追加，不许改旧号）----------
#define R3DRV_NR_REGISTER  1029          // r3drv_register(name, proto, disk, shm_id, bytes)
#define R3DRV_NR_BEAT      1030          // r3drv_beat()
#define R3DRV_NR_KICK      1031          // r3drv_kick(shm_id)      -> 处理的 SQE 条数
#define R3DRV_NR_DMA       1032          // r3drv_dma(shm_id, obj_off, lba, secs)   单发零拷贝
#define R3DRV_NR_COPY      1033          // r3drv_copy(shm_id, obj_off, lba, secs)  单发对照
#define R3DRV_NR_STATUS    1034          // r3drv_status(slot, out R3drvInfo, len)
#define R3DRV_NR_ROUTE     1035          // r3drv_route(disk, lba)  -> R3DRV_ROUTE_*
#define R3DRV_NR_WRITE     1036          // ★ v0.9.40 r3drv_write(shm_id, obj_off, lba, secs) 单发零拷贝写

// ==================== ★ v0.9.40：反向请求/应答（内核 -> 服务）====================
// 为什么需要它：v0.9.33 的环只有"服务 -> 内核"一个方向（服务提交读/写请求）。
// 文件管理器要**挂载** ext4，就必须能问服务"这个目录里有什么""把这个文件读给我"——
// 也就是内核要能发起请求。于是追加这一块（**只追加**：旧偏移一个都没动）。
//
// 内存布局（对象内）：
//   [0    , 56)                  R3drvRing
//   [64   , 64+32*R3DRV_SLOTS)   SQE 数组（服务 -> 内核）
//   [512  , 4096)                ★ R3drvReq（内核 -> 服务；本批新增）
//   [4096 , 对象大小)             数据区（DMA 落点；应答数据也放这里）
//
// 握手与所有权（关键，写进注释防止以后乱用）：
//   * 服务启动后把 req.magic = R3DRV_REQ_MAGIC 写进块里 —— 这是"我准备好接请求"的判据；
//   * 内核发起请求：填 path/op/off/size/data_secs，seq+1，**最后**写 state = REQ；
//   * 服务处理完：写应答数据到数据区，填 result/err，把 state 改成 DONE/FAIL
//     （并把 seq 原样回写 —— 内核据此确认"这条应答是我要的那条"，避免串号）；
//   * 内核**有界等待**（默认 150 ms，每圈 task_yield64() 让 ring3 服务真的能跑），
//     超时返回 -ETIMEDOUT 并**不重试**（GUI 线程绝不能在这里卡死）。
#define R3DRV_REQ_OFF      512u
#define R3DRV_REQ_MAGIC    0x52335652u   // 'RV3R'（小端）；服务写进 req.magic
#define R3DRV_REQ_ST_IDLE  0u            // 空闲（服务处理完会回到 DONE，空闲只用于初值）
#define R3DRV_REQ_ST_REQ   1u            // 内核已投递
#define R3DRV_REQ_ST_DONE  2u            // 服务已完成（result/err 有效）
#define R3DRV_REQ_ST_FAIL  3u            // 服务处理失败（err 有效）

// ---------- 请求 op ----------
#define R3DRV_OP_STAT      1u            // 取 path 的属性 -> R3drvReq.aux/result = size、size = type
#define R3DRV_OP_LIST      2u            // 列 path 目录 -> 数据区里放 R3drvDirent64 数组，result = 条数
#define R3DRV_OP_READ      3u            // 读 path 的 [off, off+size) -> 数据区，result = 实际字节

// ---------- 一条请求（512 B 上限内的 POD；内核 C++ 与用户态 C 共用）----------
struct R3drvReq {
    unsigned int       magic;       // +0   服务写 R3DRV_REQ_MAGIC
    unsigned int       state;       // +4   R3DRV_REQ_ST_*
    unsigned int       op;          // +8   R3DRV_OP_*
    unsigned int       err;         // +12  0 = 成功；否则为**正的**错误号（服务侧定义）
    unsigned long long seq;         // +16  内核每发一条 +1；服务原样回写
    unsigned long long off;         // +24  READ 的文件偏移
    unsigned long long size;        // +32  READ 的最大字节数
    unsigned int       result;      // +40  DONE 时的有效字节数 / 条数 / 大小
    unsigned int       aux;         // +44  STAT: 1=文件 2=目录；LIST: 溢出标志（1=还有更多）
    unsigned int       data_off;    // +48  应答数据落在对象内的偏移（内核填，>= R3DRV_DATA_OFF）
    unsigned int       data_secs;   // +52  内核允许的最大扇区数（服务不许越界）
    char               path[448];   // +56  NUL 结尾的**卷内绝对路径**（如 "/sub/data.bin"）
};

// ---------- 目录项应答格式（64 B/条，紧排在数据区）----------
// 为什么自带名字而不是让内核按 inode 查：服务是唯一的 ext4 解析者，内核不该再有一份解析器。
struct R3drvDirent64 {
    unsigned int  ino;              // +0   inode 号
    unsigned int  size;             // +4   文件字节数（目录 = 目录项字节数）
    unsigned char type;             // +8   1 = 普通文件，2 = 目录
    unsigned char namelen;          // +9   名字长度（1..51）
    unsigned char pad[2];           // +10
    char          name[52];         // +12  NUL 结尾（可能不满）
};

// ---------- 环控制块（56 B，必须落在对象 [0,64)）----------
struct R3drvRing {
    unsigned int       magic;       // +0  R3DRV_RING_MAGIC（服务写；内核只校验）
    unsigned int       slots;       // +4  R3DRV_SLOTS（服务写）
    unsigned int       head;        // +8  生产者位置（服务写：每提交一条 +1）
    unsigned int       tail;        // +12 消费者位置（内核写：处理到哪）
    unsigned long long kicks;       // +16 内核 kick 次数
    unsigned long long dma_bytes;   // +24 内核累计零拷贝字节
    unsigned long long copy_bytes;  // +32 内核累计对照路径字节
    unsigned int       errs;        // +40 内核累计失败条数
    unsigned int       ver;         // +44 R3DRV_ABI_VER
    unsigned int       pad[2];      // +48 对齐填充（保持 56 B）
};

// ---------- 一条请求（32 B）----------
struct R3drvSqe {
    unsigned long long lba;         // +0  绝对 LBA（512B 扇区）
    unsigned long long obj_off;     // +8  目标对象内字节偏移（≥ R3DRV_DATA_OFF，4096 对齐）
    unsigned int       secs;        // +16 扇区数
    unsigned int       flags;       // +20 R3DRV_F_*
    unsigned int       state;       // +24 R3DRV_SQ_*（内核就地完成）
    unsigned int       bytes;       // +28 完成时 = 实际落地字节数
};

// ---------- 槽位快照（r3drv_status 的 out 结构 / /proc/vimtu/drivers 的数据源）----------
struct R3drvInfo {
    unsigned int       slot;        // 槽号
    unsigned int       proto;       // R3DRV_PROTO_*
    unsigned int       disk;        // 统一驱动器号（0..3 = PATA，8.. = AHCI）
    unsigned int       state;       // R3DRV_ST_*
    int                pid;         // 服务进程 pid（-1 = 未知）
    int                shm_id;      // 共享对象 id（-1 = 没有）
    unsigned int       restarts;    // 内核已重启次数
    unsigned int       fails;       // kick/单发失败累计
    unsigned long long beats;       // 心跳累计
    unsigned long long dma_bytes;   // 零拷贝累计字节
    unsigned long long dma_ops;     // 零拷贝累计条数
    unsigned long long copy_bytes;  // 对照路径累计字节
    char               name[16];    // 服务名（NUL 结尾）
    char               image[32];   // 重启用的 ELF 路径（NUL 结尾）
};
