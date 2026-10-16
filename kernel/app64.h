// app64.h - VAP64：Vimtu64 自有"可安装应用"格式 + 安装器 / 启动器
//
// ============================ VAP64 文件布局（唯一定义点）============================
// （tools/make_vap.py 必须与本文件逐字段一致；改布局两边一起改）
//
//   偏移  长度  字段            语义
//   0     8B    magic           "VAP64\0\0\0"（8 字节，含 3 个 NUL）
//   8     4B    version         结构版本 = 1
//   12    4B    header_size     固定头字节数 = 32（不含名字）
//   16    4B    entry_offset    代码入口在**文件内**的绝对偏移 = header_size + name_len
//                               （名字紧跟头后；hello 是 6B 名字 -> 38。加载器不接受别的值）
//   20    4B    code_size       代码段字节数（1..32768；user64_run_blob64 的代码上限 8 页）
//   24    4B    code_crc32      zlib.crc32(代码段)（标准 CRC-32：反射 0xEDB88320、
//                               初值/末异或 0xFFFFFFFF，与 vfs64/make_vap.py 同口径）
//   28    4B    name_len        名字字节数，含结尾 NUL（1..24）
//   32    name_len 字节        名字（NUL 结尾，紧跟头后）
//   32+name_len  code_size 字节 代码段（平铺二进制，入口在段首）
//
//   文件总长 = 32 + name_len + code_size（加载器用这条等式做长度一致性校验）
//
// 为什么自己定义格式而不是 ELF/PE：本阶段的"可安装应用"只需要"装载一段 ring3 平铺
// 二进制并校验完整性"，32B 定长头 + 名字 + CRC32 就够了；不做重定位/导入表/多段。
//
// 安全约定（app64.cpp 里逐条执行）：
//   * 所有字段先做边界/一致性检查，再使用；盘上偏移绝不直接当函数指针跑；
//   * 代码段长度与"文件总长"严格相等，名字必须 NUL 结尾且长度合法；
//   * CRC 不匹配 / magic 不对 / 版本不对 / 长度不合 -> 一律拒绝并如实打点。
//
// 串口打点（自动验收 tests/app64_test.py grep，格式勿改）：
//   [APP64] install ok path=/hello.vap bytes=<n> crc=0x<hex>
//   [APP64] install skipped (exists) /hello.vap size=<n>
//   [APP64] install FAILED reason=<mount|write|blob>
//   [APP64] load path=<p> via=vfs64_read bytes=<n> code=<n> name=<n> crc=0x<hex>
//   [APP64] launch ok path=<p> name=<n> rc=<n>  / [APP64] launch FAILED path=<p> reason=<...>
//   [APP64] selftest PASS / [APP64] selftest FAIL mask=<n>
#pragma once
#include <stdint.h>

// ---- 头部常量（与 tools/make_vap.py 一致）----
#define VAP64_MAGIC_STR        "VAP64"      // 实际 8B 魔法见 app64.cpp 的 VAP64_MAGIC64
#define VAP64_VERSION64        1u
#define VAP64_HEADER_SIZE64    32u
#define VAP64_NAME_MAX64       24u          // 含结尾 NUL -> 名字最长 23 个可见字符
#define VAP64_MAX_CODE64       32768u       // ★ **用户窗口**上限（user64_run_blob64 的 8 页）—— 不要动
// ---- 三项上限的**归属**（批次 M 写清；别把三者混成一个数）----
//   1) **FS 侧（文件总长）**：VimtuFS2 单文件上限 **8 MiB**（批次 M：二级间接块；v2 卷 67584 B）——
//      盘上放得下多得多的字节，但 VAP64 的"代码段"另有上限（下一项）。
//   2) **用户窗口**：`VAP64_MAX_CODE64` = 32 KiB（代码段拷进用户页、只能占 8 页）—— 硬约束，不动。
//   3) **读盘缓冲**：`VAP64_MAX_FILE_BYTES64` = 64 KiB（app64.cpp 的 .bss 里）。
//      它比"32 KiB 代码 + 32B 头 + 24B 名字"大得多，所以**只要 code_size 合法，文件一定读得下**；
//      超过它一律 `reason=size` 拒绝（不截断）。刻意不抬到 8 MiB（缓冲白吃内存，真正卡住的是第 2 条）。
#define VAP64_MAX_FILE_BYTES64 65536u

// ============================ API ============================

// 主分区起始 LBA 解析（挂载 VimtuFS2 用）：
//   先按**安装器写盘的同一条规则**读 drive 的 MBR：找第 2 个标准分区项（type 0x07 = 主数据分区），
//   用它的起始 LBA；MBR 读不到 / 没有 0x07 项时退回 kernel/part64.h 的常量
//   PART_MAIN_LBA = PART_BOOT_LBA(9) + PART_BOOT_SECS(8000) = 8009 —— part_create_standard()
//   写进目标盘 MBR 的正是这个值，系统镜像的 store 保留区（ML64_STORE_LBA = 8009）也在同一位置。
uint32_t app64_main_part_lba64(int drive);

// 安装器：把内嵌的 hello.vap（build64/hello.vap，objcopy 嵌进内核）装进文件系统的 "/hello.vap"。
//   幂等：已存在就跳过（打印 install skipped (exists)），绝不重复写；
//   卷未挂载时按 (drive, part_lba) 尝试挂载一次（参数的意义）。
// 返回 0 = 成功/已存在；-1 = 失败（已打印 reason=<mount|write|blob>）。
int app64_install_builtin64(int drive, uint32_t part_lba);

// 启动器：vfs64_read(path) -> 校验 VAP64 头（magic/版本/长度一致性/CRC32）-> user64_run_blob64
//   把代码段拷进用户页、进 ring3 运行。返回 user64_run_blob64 的返回值（0 = 完整跑完）。
//   失败打印 [APP64] launch FAILED path=<path> reason=<arg|vfs|size|magic|version|name|crc|user64>。
int app64_launch64(const char* path);

// 按**文件头魔数**自动分派（终端 `run` 用）：
//   前 8 字节是 "VAP64\0\0\0" -> app64_launch64（VAP64 平铺代码段，程序里用 int 0x80）
//   前 4 字节是 "\x7fELF"    -> elf64_run64（kernel/elf64.cpp 的 ELF64 加载器，程序里用 syscall 指令）
// 两者都不像 -> 打印 [APP64] launch FAILED path=<p> reason=format 并返回 -1。
// 返回值与所走那条路径一致（0 = 跑完并正常退出）。
int app64_run_any64(const char* path);

// 自检（位掩码，0 = 全过；打印 [APP64] selftest PASS / FAIL mask=<n>）：
//   bit0 头解析往返（含 CRC 与 zlib 同口径断言 crc32("123456789")==0xCBF43926）
//   bit1 坏 magic 被拒        bit2 坏 CRC 被拒
//   bit3 长度不一致/entry_offset 不一致被拒
//   bit4 名字边界（0 / >24 / 无 NUL 拒绝；23 字符可接受）
//   bit5 内嵌 hello.vap 的头部自校验（magic/版本/名字/CRC/entry_offset）
int app64_selftest64();

// ==================== ★ 应用注册表（/etc/apps.json，单一真源）====================
// "有哪些应用、叫什么、执行什么、图标是什么"只有**一处**登记 —— 这个文件：
//   * **内置应用**的条目在**构建期**编译进内核（下面的 kBuiltinApps64，见 app64.cpp）；
//     启动期 / 首次读注册表时**幂等**写进 /etc/apps.json（文件不存在/卷里没有时只留在内存，
//     保证裸 system.img 上开始菜单仍列全部内置应用）。
//   * **用户安装的应用**由 /bin/vpkg 在安装时**追加**一条（从包 manifest 取
//     display/exec/icon；见 user/store/vs_pkg.c 的 registry_append）。写盘 = VimtuFS2 真落盘，
//     重启后 vpkg 的条目与桌面快捷方式位置都还在。
//   * 内核读：开始菜单（kernel/startmenu64.cpp）与桌面（kernel/desktopops64.cpp）都从
//     app64_registry_* 取；桌面上快捷方式的位置也写回这里（app64_registry_set_pos64）。
//
// 文件格式（UTF-8，一行一条 JSON 对象；内核侧解析器是"找键 -> 取引号串/整数"的小扫描器，
// 不引第三方库；字段顺序由写方固定，读方按 key 找，所以顺序不敏感）：
//   {"name":"终端","en":"Terminal","exec":"builtin:term","icon":"terminal",
//    "alias":"term shell console","id":3,"builtin":1,"desktop":0,"x":0,"y":0}
//   name    = 显示名（可空 -> 用 en）
//   en      = 英文名（开始菜单/搜索用）
//   exec    = "builtin:<窗口应用短名>" 或卷内可执行文件路径（ELF / .vap64 都行，
//             按**文件头魔数**分派，见 gui64 的 app_launch_exec64）
//   icon    = 系统图标名（kIcon64Names 里的名字，见 kernel/icons64.h）或 "/..." 卷内 PNG 路径；
//             取不到 -> 程序化兜底（绝不空）
//   alias   = 搜索别名（空格分隔）
//   id      = 内置窗口应用 id（APP_ID_*）；用户应用写 -1
//   builtin = 1 = 构建期内置；0 = 用户安装
//   desktop = 1 = 桌面上自动建了快捷方式（用户安装默认 1；内置默认 0，不往桌面塞）
//   x/y     = 桌面快捷方式位置（网格吸附后的左上角；0,0 = 还没定，由桌面模块挑第一个空格）
#define APPREG64_PATH        "/etc/apps.json"
#define APPREG64_MAX         24
#define APPREG64_NAME_MAX    32
#define APPREG64_EXEC_MAX    96
#define APPREG64_ICON_MAX    96
#define APPREG64_ALIAS_MAX   48
#define APPREG64_FILE_MAX    8192

// 启动期/首次使用时确保注册表就绪（内置表进内存 + 卷里有文件就解析/幂等写出）。
// 返回注册表里的应用条数（>= 内置条数；没有任何卷时 = 内置条数）。
int  app64_registry_init64();
// ★ 变化通知（**不要**在桌面帧里轮询读盘：vfs64 不可重入 + 长 I/O 会冻住鼠标，
//   两条都在 tests/vpkg64_test.py 里实测踩到过，说明见 kernel/app64.cpp 的函数头）：
//   * 外部改动（/bin/vpkg 装/卸）由 kernel/fd64.cpp 的 fd64_close64 在**写者自己的上下文**里
//     调 app64_registry_on_close64(path, writable) 触发重读；
//   * 调用方（桌面帧 / 开始菜单）只比较内存里的 app64_registry_gen64()，变了才重建视图。
// app64_registry_refresh64() 保留但**不再读盘**（只为兼容旧调用点，现在恒返回 0）。
int  app64_registry_refresh64();
// fd64 close 挂钩（weak 链接：安装介质内核不链 app64.cpp）。path 是刚关闭的文件路径。
void app64_registry_on_close64(const char* path, int writable);
uint32_t app64_registry_gen64();          // 每次内容变化 +1（调用方用它做去重）
int  app64_registry_count64();
const char* app64_reg_name64(int i);
const char* app64_reg_en64(int i);
const char* app64_reg_exec64(int i);
const char* app64_reg_icon64(int i);
const char* app64_reg_alias64(int i);
int  app64_reg_id64(int i);               // APP_ID_*；用户应用 = -1
int  app64_reg_builtin64(int i);
int  app64_reg_desktop64(int i);
void app64_reg_pos64(int i, int* x, int* y);
// 桌面快捷方式：改 desktop 标志 / 写回位置（整表重写 /etc/apps.json，跨重启持久化）
int  app64_reg_set_desktop64(int i, int on);
int  app64_reg_set_pos64(int i, int x, int y);
// 按 exec 找（-1 = 没有）
int  app64_reg_find_exec64(const char* exec);
int  app64_reg_selftest64();
