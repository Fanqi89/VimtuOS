// userdb64.h - 多用户骨架（用户表 + 密码加盐哈希 + 持久化 + 会话身份）
//
// 本批（P1c：锁屏 / 登录 / 多用户骨架）里"用户"的真源。**权限位检查（uid/gid/mode）属 P4**：
//   本模块只提供 Linux 语义的**数据布局**与**可查的会话身份**，不做任何越权拦截（如实标注）。
//
// ==================== 数据布局（Linux 语义）====================
//   root  ：uid=0，主目录 /root，桌面 /root/Desktop，**在登录界面默认隐藏**（永不出现）
//   普通用户：uid >= 1000，主目录 /home/<用户名>，桌面 /home/<用户名>/Desktop，用户名由用户自己创建
//   头像可选："" = 内置（按 uid 选 3 个程序化头像之一）/ "builtin:<0..2>" / VimtuFS2 路径（PNG、BMP）
//   密码可选：没设密码 = 直接登录；设了密码 = 登录界面弹密码框
//
// ==================== 持久化（VimtuFS2 系统卷，跨重启不丢）====================
//   文件：/etc/users.db（系统卷槽 vfs64_system_slot64()；用户浏览 D:/E: 时也不会写错盘）
//   格式：**文本 + 版本号**（第一行就是版本，解析时逐字段校验，坏行整条跳过并打点）：
//     VIMTU-USERSDB 1
//     U|<name>|<uid>|<home>|<hash_hex>|<salt_hex>|<avatar>|<theme>|<wall>|<lock_mode>|<flags>
//   * hash_hex = SHA-256(salt || pw) 再叠加 (ITER-1) 轮 SHA-256(h || salt) 的十六进制（64 字符）
//   * salt_hex = 16 字节十六进制（32 字符）；**明文密码绝不落盘、也绝不进串口**
//   * theme/wall/lock_mode 的 -1（或空串）= 继承全局配置；flags：1=root、2=隐藏、4=系统
//   * 每次改动（useradd / passwd / userdel / 登录记录）立即整体重写该文件（vfs64 的写是"先数据后 inode"）
//
// ==================== 会话身份（★ P4 起：**权限位真的拦截**）====================
//   GUI 身份（登录界面的用户名/头像）与**会话身份**（euid）是两份：
//     * userdb64_login64()  设置 GUI 身份 = 该用户，会话身份也 = 该用户
//     * `su - root` / `sudo -i` / `su - <user>` 只改**会话身份**（GUI 的用户名/头像**不变**）
//     * `exit` 退回 GUI 用户
//     * 会话身份只在内存里：软重启/断电后回到"开机锁屏、只有普通用户"
//   ★ P4：每次身份变化都调 vfs64_set_cred64(uid, gid, euid, egid)（普通用户 gid = uid，root = 0）
//     并打一行 `[PERM64] cred uid=.. gid=.. euid=.. egid=.. user=.. via=login|su|exit`；
//     VFS 层按这份凭证做 Linux 三段判定（见 kernel/vfs64.h 的"★ P4 权限"）。
//   ★ 每个用户的主目录/桌面由本模块建成 **属主=该用户、mode=0700**（root 的 /root 也是 0700）。
//
// 串口打点（自动验收 grep；每类都有**上限**防刷屏，见 .cpp 的 log_budget）：
//   ★ 注意：[USER64] 这个前缀**早已被 ring3 那一批占用**（[USER64] map/enter/selftest/slotreuse…，
//     见 kernel/usermode64.cpp）。本模块的行一律带**独特关键字**（db / useradd / passwd / users /
//     login / desktop / su / exit / whoami / avatar / selftest userdb），互不歧义。
//   [USER64] userdb init path=/etc/users.db version=1 algo=sha256 iter=1000 users=3 normal=2 root=hidden
//   [USER64] userdb load line=<n> name=<..> uid=<n> home=<..>
//   [USER64] userdb save ok path=/etc/users.db bytes=<n> users=<n>
//   [USER64] userdb firstboot bootstrap user=vimtu uid=1000 (no password)
//   [USER64] useradd ok name=A uid=1001 home=/home/A desktop=/home/A/Desktop
//   [USER64] useradd FAIL name=A reason=exists
//   [USER64] userdel ok name=A uid=1001
//   [USER64] passwd ok user=A set=1 algo=sha256 iter=1000 salt=16B (plaintext never stored)
//   [USER64] passwd FAIL user=A reason=...
//   [USER64] users count=3 root=1 normal=2 list=root,A,B
//   [USER64] login ok name=A uid=1001 home=/home/A desktop=/home/A/Desktop gui=A
//   [USER64] login FAIL name=A reason=...
//   [USER64] login apply settings user=A theme=<n|inherit> wall=<p|-> lock_mode=<n|inherit>
//   [USER64] desktop list user=A dir=/home/A/Desktop entries=1 names=a.txt
//   [USER64] su ok from=vimtu to=root euid=0 gui=vimtu gui_unchanged=1 via=su-dash
//   [USER64] exit ok from=root to=vimtu euid=1000 gui=vimtu
//   [USER64] whoami user=vimtu euid=1000 gui=vimtu root_session=0
//   [USER64] avatar user=A src=builtin:1
//   [USER64] selftest userdb PASS mask=0
#pragma once
#include <stdint.h>

#define USERDB64_MAX_USERS   16      // 用户上限（inode 上限 512，足够）
#define USERDB64_NAME_MAX    16      // 用户名（含 NUL；Linux 语义：1..15 个可打印 ASCII）
#define USERDB64_HASH_MAX    65      // SHA-256 十六进制 + NUL
#define USERDB64_SALT_MAX    33      // 16 字节十六进制 + NUL
#define USERDB64_DB_MAX      (32 * 1024)   // /etc/users.db 文本上限（16 个用户 × 约 300B 远远够）
#define USERDB64_PATH_MAX    128     // 与 VFS64_PATH_MAX 一致

// flags
#define USERDB64_F_ROOT      1u      // uid=0 的 root 账户
#define USERDB64_F_HIDDEN    2u      // 不在登录界面出现（root 默认带这一位）
#define USERDB64_F_SYSTEM    4u      // 系统账户（保留）

// 一个用户（内存态；路径都是 VimtuFS2 绝对路径）
struct User64Entry {
    char     name[USERDB64_NAME_MAX];
    uint32_t uid;
    char     home[USERDB64_PATH_MAX];
    char     desktop[USERDB64_PATH_MAX];
    char     hash[USERDB64_HASH_MAX];     // 空串 = 没设密码
    char     salt[USERDB64_SALT_MAX];
    char     avatar[USERDB64_PATH_MAX];   // "" / "builtin:N" / VimtuFS2 路径
    int      theme;                       // -1 = 继承全局
    char     wall[USERDB64_PATH_MAX];     // "" = 继承全局
    int      lock_mode;                   // -1 = 继承全局
    uint32_t flags;
    int      hidden;                      // 1 = 不在登录界面（= flags & HIDDEN）
};

// ---- 生命周期 ----
// 初始化：**必须在 vfs64 挂载系统卷 + config64_init64 之后**调用。
//   建 /etc、/home、/root 与每个用户的主目录/桌面；读 /etc/users.db（没有就首建）；
//   保证 root 存在（隐藏）；若表里**一个普通用户都没有**则建一个引导用户（默认 vimtu，无密码），
//   否则开机会没有任何可登录的账户（首启可用性）。最后打 [USER64] userdb init 行。
void userdb64_init64();

// ---- 查询 ----
int  userdb64_count64();                              // 全部用户（含 root）
const User64Entry* userdb64_at64(int i);              // 越界 nullptr
int  userdb64_find64(const char* name);               // 下标；-1 = 没有
const User64Entry* userdb64_find_root64();            // root 条目（永不为空；init 后）
int  userdb64_normal_count64();                       // 普通（非 root 非隐藏）用户数
// 登录界面用：第 k 个"可见用户"（跳过 root/隐藏）的名字；越界 nullptr
const User64Entry* userdb64_visible64(int k);
const char* userdb64_name_for_uid64(uint32_t uid);

// ---- 账号管理（终端 useradd/userdel/passwd/users 用；成功 0 / 失败 -1）----
int  userdb64_add64(const char* name, uint32_t* uid_out);      // 建用户 + 主目录 + 桌面 + 落盘
int  userdb64_del64(const char* name);                         // 删用户记录（**不删主目录**，如实打点）
// 设密码：pw 为 nullptr/空串 = 清除密码（直接登录）。**只存盐 + SHA-256 迭代哈希**
int  userdb64_set_password64(int idx, const char* pw);
// 校验：没设密码 -> 1（无需密码）；有密码 -> 明文比对哈希
int  userdb64_check_password64(int idx, const char* pw);
int  userdb64_save64();                                        // 写 /etc/users.db（立即落盘）
int  userdb64_selftest64();                                    // 纯逻辑自检（哈希向量/解析/会话）

// ---- 登录 / 会话身份 ----
// 登录：设 GUI 身份 + 会话身份 = 该用户；枚举该用户桌面（[USER64] desktop list）；
//       记住"上次登录用户"（config64 的 ui.login.last，便于锁屏默认选中）；返回 0/-1。
int  userdb64_login64(const char* name);
const User64Entry* userdb64_gui_user64();             // GUI 身份（su 不改；未登录 nullptr）
const User64Entry* userdb64_session_user64();         // 会话身份（su/sudo/exit 改）
int  userdb64_euid64();                               // 会话 euid；未登录 -1
const char* userdb64_session_name64();                // 会话用户名；未登录 "-"
// 切成指定用户（su/sudo 用；GUI 身份不变）。成功 0；-1 = 未登录/没有该用户。
// 语义：root 可切任何人；普通用户也可切（口令校验在终端层：目标有口令时先输对）。
int  userdb64_su64(const char* name, const char* via);
int  userdb64_root_session64();                       // 1 = 当前会话身份是 root
// 切成 root（via = "su-dash" / "su-root" / "sudo-i"）；返回 0 = 成功（= userdb64_su64("root", via)）
int  userdb64_session_root64(const char* via);
// 退回 GUI 用户（exit）；返回 0 = 有 root 会话可退，1 = 本来就不是 root（调用方如实提示）
int  userdb64_session_exit64();
const char* userdb64_last_user64();                   // 上次登录用户名（config64 ui.login.last；没有 ""）

// ---- 桌面文件列表（该用户 Desktop 目录的真实枚举；登录时刷新）----
int  userdb64_desktop_count64();                      // 条目数（最多 32，超出只记数）
const char* userdb64_desktop_name64(int i);           // 名字；越界 ""
uint32_t userdb64_desktop_size64(int i);              // 字节数（目录 = 0）
const char* userdb64_desktop_dir64();                 // 目录路径（未登录 ""）
int  userdb64_desktop_total64();                      // 目录里的真实条目数（可能 > 缓存的 32 条）

// ---- 报告（终端 users / whoami 用；返回写入长度）----
int  userdb64_report64(char* out, int max);
