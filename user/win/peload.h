/* peload.h - ★ v0.9.45（W2）：PE 装载器与 Windows 运行期桩之间的接口
 *
 * 为什么拆成两个 .c：
 *   `peload.c` 只管 **PE 装载**（解析 / 映射 / 重定位 / 权限 / 跳入口），
 *   `peload_crt.c` 只管 **Win32 + 老式 msvcrt 的桩实现**（TEB/PEB、kernel32、msvcrt）。
 *   两者通过本头文件的一个**桩表**和几个输出函数连接。
 *   好处：改 CRT 不用碰装载器，改装载器不用碰 CRT —— 而 W2 之后还会一直往桩表里加东西。
 */
#ifndef PELOAD_H
#define PELOAD_H

typedef unsigned long long u64;
typedef long long          i64;
typedef unsigned int       u32;
typedef int                i32;
typedef unsigned short     u16;
typedef unsigned char      u8;

/* ---- 私有系统调用号（与 kernel/syscall64.cpp 的 case 号一致）---- */
#define PL_NR_SET_GS  1037      /* set_gs_base(addr) -> 0 / -EINVAL  */
#define PL_NR_GET_GS  1038      /* get_gs_base(out_u64) -> 0 / -EFAULT */

/* ---- 输出（行级缓冲：内核每次 write 返回都会打一行 [SYSCALL]，所以一次 write 一整行）---- */
void pl_out(int fd, const char *s, u32 n);
void pl_outs(int fd, const char *s);
void pl_dec(int fd, u64 v);
void pl_hex(int fd, u64 v);
void pl_flush(void);

/* ---- 桩表条目 ----
 * is_data = 0：`addr` 是**函数地址**（IAT 槽里放它，PE 直接 call）。
 * is_data = 1：`addr` 是**数据对象地址**（IAT 槽里放它，PE 用 `mov rax,[__imp_x]` 取到的
 *              就是这个对象的地址）。两者**不能混** —— 把数据当函数填，PE 会跳到数据上执行；
 *              把函数当数据填，PE 会去读代码段里的"变量"。msvcrt 的 `_commode`/`_fmode`/
 *              `_acmdln`/`__initenv` 都是**数据**导入。 */
typedef struct {
    const char *dll;
    const char *name;
    void       *addr;
    int         is_data;
} PlStubEntry;

typedef struct {
    const PlStubEntry *tab;
    u32                n;
} PlStubTable;

/* peload_crt.c 提供 */
extern const PlStubTable g_pl_stubs;
/* 装载完成后、填 IAT 之前调用：建 TEB/PEB、设 GS.base、初始化 mini-CRT 的堆与 argv。 */
int  pl_crt_setup(u64 image_base, u32 image_size, const char *exe_path);
/* main 返回之后调用：跑 atexit 链（`exit` 桩也用同一条链）。 */
void pl_crt_run_exit_chain(void);

#endif /* PELOAD_H */
