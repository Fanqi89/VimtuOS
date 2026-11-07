// sysfs64.h - /sys 的**合成节点树**（v0.9.24）：只读、无盘、零磁盘 IO
//
// 为什么要有它（用户需求链）：Wayland 生态（libinput/udev）靠 `/sys/class/input/eventN/` 认识设备：
//   `name`（设备名）、`dev`（major:minor）、`capabilities/{ev,key,rel,abs}`（能力位图）。
//   在这之前本内核只有 `/dev/input/event0|1`，**没有 /sys 这一层** —— VFS 里没有任何目录能表示
//   "运行时合成、不在盘上"的节点，所以 libinput 那类上层拿不到设备属性。
//
// 设计（有意做小）：
//   1) **不改 VFS 的磁盘格式**，也不占 inode：`fs64_*` 在最前面判一次路径，命中就走本模块。
//      所以它在"没有卷"的机器上也能工作（`resolve_vol()` 失败之前就返回）。
//   2) 树只有本系统真实存在的设备（evdev 两路：event0 键盘 / event1 鼠标），
//      **内容全部来自 drm64 里 evdev 的同一份状态**（与 EVIOCGNAME/EVIOCGBIT/EVIOCGABS 同源），
//      不另存一份、不许漂移。
//   3) 写操作一律拒绝（只读）：`fs64_write*` 对 /sys 返回 -EROFS。
//   4) 没有硬链接/符号链接（本 VFS 的 link 只有磁盘节点）：`subsystem` 用普通文件给出 "input"，
//      这是**如实降级**，不是假装有 symlink。
//
// 树（每个 eventN 下 8 个文件）：
//   /sys/class/input/eventN/name                      "vimtu-keyboard\n" / "vimtu-mouse\n"
//   /sys/class/input/eventN/dev                       "13:64\n" / "13:65\n"（Linux input 主设备号 13）
//   /sys/class/input/eventN/subsystem                 "input\n"
//   /sys/class/input/eventN/uevent                    "MAJOR=13\nMINOR=<n>\nDEVNAME=input/event<N>\n"
//   /sys/class/input/eventN/capabilities/ev           "3\n"（EV_SYN|EV_KEY）/ "7\n"（+EV_REL）
//   /sys/class/input/eventN/capabilities/rel          "0\n" / "1\n"
//   /sys/class/input/eventN/capabilities/abs          "0\n" / "3\n"（ABS_X|ABS_Y）
//   /sys/class/input/eventN/capabilities/key          "0\n"（**如实**：本内核不维护键位图）
//
// 串口打点（自动验收 grep）：
//   [SYSFS64] tree entries=<n> ev0=<name> ev1=<name>
//   [SYSFS64] selftest PASS   /   [SYSFS64] selftest FAIL mask=<n>
#pragma once
#include <stdint.h>

#define SYSFS64_BUF        96u    // 单节点内容的静态缓冲区（够放 uevent 那几行）
#define SYSFS64_PATH_MAX   48u    // 本模块认的最长路径（超过一律"不归我管"）

// 1 = 这个路径归 sysfs 管（/sys、/sys/...）；0 = 不归（走真实卷）；-1 = path 空
int sysfs64_match64(const char* path);

// 1 = 命中（*type 是 VFS64_TYPE_DIR/FILE，*size 是内容字节数）；0 = 不是 sysfs 路径；
// -1 = 是 sysfs 路径但节点不存在（调用方按 ENOENT 处理）
int sysfs64_stat64(const char* path, uint32_t* type, uint32_t* size);

// 按偏移读一段（off 越界 = 读 0 字节）。返回 1 = 命中，0 = 不归 sysfs，-1 = 节点不存在
int sysfs64_read64(const char* path, uint32_t off, void* buf, uint32_t len, uint32_t* out_got);

// 列目录（名字 + 大小；目录项大小恒 0）。返回条数；-1 = 不是目录/不存在；-2 = 不归 sysfs
int sysfs64_list64(const char* path, char names[][32], int max, uint32_t* sizes);

// 自检：返回 0 = 全过，非 0 = 失败位掩码（位含义见 .cpp 顶部）
int sysfs64_selftest64();
