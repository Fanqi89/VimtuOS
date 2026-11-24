// deskdir64.h - ★ v0.11.7：**桌面 = 真实文件夹**（<home>/Desktop）的唯一读写入口
//
// 为什么要有这个模块（用户原话，`桌面图标真实文件.txt`）：
//   "咱们的'桌面（Desktop）'绝对不能只是一个读取 .links 配置的独立列表，
//     它必须直接对应底层文件系统里的一个真实文件夹（比如 /home/vimtu/Desktop/）。"
//   → 桌面图标 = 那个目录里的条目（真源 = 列目录）；
//   → 拖到桌面 = **真移动**（rename，不是登记路径）；
//   → 终端里 touch 一个新文件，桌面必须实时出现图标。
//
// 设计口径（改之前先读）：
//   1) **目录**：GUI 用户的 desktop 字段（userdb64_gui_user64()->desktop）。
//      root = /root/Desktop，普通用户 = /home/<用户名>/Desktop；未登录/没有用户表时
//      退化成 /home/vimtu/Desktop（与 settings64 的默认家目录同一个口径）。
//      目录本体由 userdb64 在**建用户/启动期**以 root 建好并 chmod 0700（普通用户在 / 上建不了目录）。
//   2) **本模块只认系统卷（VimtuFS2）**：桌面永远在系统卷上；别的卷（FAT/ISO/ext4r）上的东西
//      拖过来要走"复制 + 删除源"（跨卷 rename 在文件系统层面不成立）。
//   3) **一帧内不做无谓 I/O**：本模块只在"外部要求（init / 世代号变化 / 用户动作）"时读盘，
//      绝不自己轮询（轮询的触发条件在外壳：vfs64_mut_gen64() 的整数比较，见 desktopops64_tick64）。
//   4) 名字以 '.' 开头的条目不列（与 Linux 桌面习惯一致）；越界（DESKDIR_MAX）只记数不打穿内存。
//
// 串口打点（验收 grep 用；每类都有上限/只在变化时打）：
//   [DESKDIR] init dir=/home/vimtu/Desktop vol=0 exists=1 entries=0 mode=dir
//   [DESKDIR] scan why=fs dir=/home/vimtu/Desktop entries=1 added=1 removed=0 hidden=0 names=a.txt
//   [DESKDIR] move in why=explorer-drag from=/docs/a.txt to=/home/vimtu/Desktop/a.txt
//             vol=0 rc=0 via=rename name=a.txt
//   [DESKDIR] move in why=.. from=.. to=.. vol=0 rc=-1 why=readonly|crossvol-dir|exists|rc name=..
//   [DESKDIR] move out name=a.txt to=/docs/a.txt vol=0 rc=0 via=rename
//   [DESKDIR] rename name=a.txt to=b.txt rc=0
//   [DESKDIR] trash name=a.txt dir=/home/vimtu/Desktop rc=0 entry=a.txt
//   [DESKDIR] selftest PASS|FAIL mask=<n>
#pragma once
#include <stdint.h>

#define DESKDIR_MAX       24      // 一次列出的条目上限（桌面项总数上限由 desktopops64 管）
#define DESKDIR_NAME_MAX  64      // 单条名字上限（= VimtuFS2 名字上限量级）

// ---- 生命周期 ----
int  deskdir64_init64(const char* why);          // 记住目录 + 首次扫描（gui64_run 里，桌面项集合之前）
const char* deskdir64_dir64();                    // 当前桌面目录（未 init 时 = 退化默认值，永不为空）
int  deskdir64_slot64();                          // 系统卷的 VFS 槽（<0 = 没挂上）
int  deskdir64_vol64();                           // 系统卷的 fs64 卷号（<0 = 未知）
int  deskdir64_ensure64(const char* why);         // 目录不存在就建（mkdirs；失败如实打 rc=）
int  deskdir64_scan64(const char* why);           // 重新列目录；1 = 条目集合有变化（增/删/换类型）
// ★ 换用户（登录/切换）后重新绑定目录并重列：desktopops64 在 tick 里发现 GUI 用户变了就调它
int  deskdir64_rebind64(const char* why);         // 1 = 目录换了（调用方必须重列桌面项）

// ---- 条目访问 ----
int  deskdir64_count64();
const char* deskdir64_name64(int i);
int  deskdir64_is_dir64(int i);
uint32_t deskdir64_size64(int i);
int  deskdir64_path64(int i, char* out, int cap);            // 目录 + '/' + 名字
int  deskdir64_find64(const char* name);                     // 名字 -> 下标（-1 = 没有）
int  deskdir64_name_of64(const char* path, char* out, int cap);   // 路径最后一段

// ---- 真移动 / 真删除（桌面与卷内其它目录之间）----
//   move_in：把 (src_vol, src_path) **搬进**桌面目录（同卷 rename；跨卷单文件 = 复制 + 删源）。
//     out_name 回填落点名字（撞名会自动退化成 stem(2).ext，绝不覆盖已有条目）。
//     返回 1 = 成功（此时目录内容已变，调用方应重列）。
int  deskdir64_move_in64(int src_vol, const char* src_path, const char* why, char* out_name, int cap);
//   move_out：把桌面上的 name **搬出去**到 (dst_vol, dst_path)。
int  deskdir64_move_out64(const char* name, int dst_vol, const char* dst_path, const char* why);
//   rename：桌面内的同名改名（只改最后一段；不跨目录）。
int  deskdir64_rename64(const char* old_name, const char* new_name);
//   trash：桌面条目 -> /Recycle（**同卷改名**，走 trash64_put64，不复制、不漏数据）。
int  deskdir64_trash64(const char* name, const char* why, char* out_name, int cap);

// ---- 工具 ----
int  deskdir64_unique_name64(const char* name, char* out, int cap);   // 撞名 -> stem(2).ext
int  deskdir64_selftest64();
