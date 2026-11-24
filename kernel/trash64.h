// trash64.h - ★ v0.11.6：**文件回收站**（删除的第二条真实语义）
//
// 需求原文（用户 2026-11-25）：
//   "补齐回收站与删除机制（右键删除后能进回收站，防止误删）"。
//
// ==================== 设计（为什么是"同卷改名 + 元数据文件"）====================
//   1) 回收站**就落在同一个卷上**：`/Recycle`（每个卷各自一份）。
//      为什么不集中放在 C:：跨卷"移动"在 VimtuFS2 上不成立（那叫 copy + delete），
//      而回收站的语义是"原封不动地暂存"，只有同卷改名（fs64_rename_to64）能保证
//      "一个字节都没动、可原样搬回"，也不需要空闲空间翻倍。
//   2) 条目名 = 原 basename（撞名时退化成 `stem (2)ext`），内容原封不动。
//   3) 原路径写在 `/Recycle/.meta/<条目名>.txt`（一行 `orig=<完整路径>` + 一行 `type=file|dir`）。
//      为什么每条一个元数据文件而不是一个总索引：总索引要"读-改-写"整块文件，
//      中途掉电/失败会连带毁掉别的条目；一条一个文件时，任何一条失败只影响它自己，
//      而且 put 阶段失败可以**把条目改名搬回去**（绝不静默丢数据）。
//   4) `.meta` 是点目录：explorer 的列表会跳过（与"隐藏文件"同一口径），
//      所以用户在回收站里只会看到被删的那几项。
//
// ==================== 边界（如实写清）====================
//   * 只做**同卷**回收/还原；跨卷删除不搬到别的卷（都在被删文件自己的卷上）。
//   * 还原时**目标已存在 -> 拒绝**（why=exists），绝不覆盖；原父目录没了 -> why=noparent。
//   * 永久删除非空目录是**有界递归**（深度 <= TRASH64_MAX_DEPTH、条目 <= TRASH64_MAX_NODES）；
//     超界 -> 如实返回 why=budget 并**保留未删部分**，不假装删干净。
//   * 只读卷（FAT32/ISO9660）上的删除本来就被 fs64 拦（-FS64_EROFS），这里不重复实现。
//   * 不记录"删除时间"（内核没有 RTC 通道给这层用；属性面板给的是文件自己的 mtime）——
//     宁可少一个字段，也不写一个假时间。
//
// ==================== 打点（自动验收据）====================
//   [TRASH64] init vol=<n> dir=/Recycle exists=<0|1> created=<0|1> items=<n>
//   [TRASH64] put name=<条目名> orig=<原路径> type=file|dir vol=<n> rc=0|1 why=.. meta=0|1
//   [TRASH64] restore name=<条目名> to=<原路径> rc=0|1 why=..
//   [TRASH64] purge name=<条目名> type=file|dir files=<n> dirs=<n> rc=0|1 why=..
//   [TRASH64] empty n=<n> files=<n> dirs=<n> rc=0|1 why=..
//   [TRASH64] count vol=<n> items=<n> meta=<n>
//   [TRASH64] selftest PASS|FAIL mask=<n>
#pragma once
#include <stdint.h>
#include "fs64.h"      // FS64_NAME_MAX / VFS64_PATH_MAX / VFS64_TYPE_*

#define TRASH64_DIR       "/Recycle"          // 回收站目录（每卷一份）
#define TRASH64_META_DIR  "/Recycle/.meta"    // 元数据目录（点开头 = 隐藏，explorer 跳过）
#define TRASH64_META_EXT  ".txt"              // 元数据文件后缀
#define TRASH64_NAME_MAX  (FS64_NAME_MAX)     // 条目名缓冲（名字上限同 FS64）
#define TRASH64_MAX_DEPTH 4                   // 永久删除的递归深度上限（含根）
#define TRASH64_MAX_NODES 96                  // 一次永久删除最多处理的条目数
#define TRASH64_LIST_MAX  64                  // 一次列出的条目数上限
#define TRASH64_RO_NAME   ".meta"             // 点目录名（列表时跳过）

struct Trash64Ent64 {
    char     name[TRASH64_NAME_MAX];   // 回收站里的条目名（= 删掉时的 basename，撞名带 (n)）
    char     orig[VFS64_PATH_MAX];     // 原始完整路径（同卷）
    uint32_t type;                     // VFS64_TYPE_FILE / VFS64_TYPE_DIR
    uint32_t size;                     // 字节数（目录 = 0）
    uint32_t mtime;                    // 条目自己的 mtime（不是删除时间，见文件头）
    uint32_t meta_ok;                  // 1 = 读到了元数据（0 = 孤立条目，头文件说明里那种）
};

// 幂等初始化：确保 <vol>/Recycle 与 /Recycle/.meta 存在（vol = -1 表示当前卷）。
// 返回 0 = 目录在位（新建或已存在）；-1 = 建不出来（只读卷/权限）。
// ★ 启动期兜底（ring0 = root）：确保 <vol>/Recycle 与 /Recycle/.meta 存在且 0777。
//   普通用户在 "/" 上建不了目录，所以**不能**等到第一次删除才 mkdir（那会 -EACCES）。
int trash64_boot_ensure64(int vol);

int trash64_init64(int vol);

// 把 vol 上的 path 移进回收站。*out_name 收条目名（可传 nullptr）。
// 返回 0 = 成功；负数 = 失败（-1 参数/stat/rename 失败、-FS64_EROFS 只读卷）。
int trash64_put64(int vol, const char* path, char* out_name, int cap);

// 列出回收站条目（跳过 .meta）。返回填充条数（>=0）；-1 = 目录不可读。
int trash64_list64(int vol, Trash64Ent64* out, int max);

// 回收站条目数（>=0）；-1 = 不可读。
int trash64_count64(int vol);

// 还原：把条目搬回它的原路径。0 = 成功；-1 失败（why 给原因，可传 nullptr）
int trash64_restore64(int vol, const char* name, const char** why);

// 永久删除单个条目（目录走有界递归）。0 = 成功。
// *files/*dirs 回填实际删掉的条目数（可传 nullptr）。失败 why = "budget" / "io" / "nometa"。
int trash64_purge64(int vol, const char* name, int* files, int* dirs, const char** why);

// 清空回收站（逐条 purge）。返回删掉的条目数（>=0）；*fail 回填失败条数。
int trash64_empty64(int vol, int* fail);

// 取条目的原路径（给属性面板/状态栏用）。0 = 读到了（out 已填）；-1 = 没有元数据。
int trash64_orig_of64(int vol, const char* name, char* out, int cap);

// 自检（0 = 全过）：在 /tmp 造样本文件往返一遍（put -> list -> restore -> purge）。
int trash64_selftest64();
