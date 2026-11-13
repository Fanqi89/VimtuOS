/* peload_stubs.S - ★ v0.9.44（W1）/ v0.9.45（W2）：IAT 桩的 **Win64 -> SysV 跳板**
 *
 * ============================ 为什么必须有这个文件 ============================
 * PE 里的代码是 **Windows x64 调用约定**，我们的 C 是 **SysV**。两者有两处**不一致**，
 * 而且两处都会以"看起来很正常"的方式失败：
 *
 * 【陷阱 1：参数寄存器不同】
 *   Win64：第 1..4 个整数参数走 `rcx, rdx, r8, r9`，调用者还要在栈上留 32 字节影子空间。
 *   SysV ：参数走 `rdi, rsi, rdx, rcx, r8, r9`，没有影子空间。
 *   → 所以 `ExitProcess(7)` 的 7 在 `rcx`，SysV 的桩却去读 `rdi`（垃圾）：
 *     退出码变随机，而进程**确实退出了**。最难查的一类假绿。
 *
 * 【陷阱 2：被调用者保存寄存器（callee-saved）的集合不同】★ W2 实测踩到
 *   Win64 **非易失**：`rbx, rbp, rdi, rsi, r12..r15`（+ xmm6..15）
 *   SysV  **非易失**：`rbx, rbp,        r12..r15`
 *   → **`rdi` 与 `rsi` 在 Win64 里是"我调完必须和调之前一样"，在 SysV 里却是随便用的**。
 *     于是跳板把 `%rsi` 当临时寄存器搬参数时，就把 PE 的 `rsi` 踩了。
 *
 *   现场（W2 第一次跑 cui.exe，exit code 139）：
 *       mingw `__tmainCRTStartup` 的 argv 复制循环：
 *           mov 0x4cce(%rip),%r15     ; r15 = &argv
 *           mov $1,%esi               ; esi = 下标 i = 1
 *       .L: mov -0x8(%r15,%rsi,8),%rcx ; argv[i-1]
 *           call strlen               ; ← **我们的桩把 esi 改了**
 *           ...
 *           call malloc
 *           mov %rax,-0x8(%r13,%rsi,8); ← 用被踩坏的 rsi 当下标
 *       => [SIG64] fault #PF cr2=0x10109BFF8 rip=…137F（`cr2 - heap = 0x800FF8`，
 *          反解出来 rsi = 0x100200 —— 正是 strlen 之后 rdx 的残留）。
 *
 *   修法：每条跳板都 **push %rsi / push %rdi … pop %rdi / pop %rsi**。
 *   `rbx/rbp/r12..r15` 不用管 —— 那两个 ABI 都要求保留，SysV 编译的 C 会自己保。
 *   （xmm6..15 同理不同，但本文件与 C 都 `-mno-sse`，不碰它们。）
 *
 * ============================ 栈对齐怎么算 ============================
 * 进桩时 rsp % 16 == 8（`call` 刚压过返回地址）。
 * 两次 push 共 16 字节 -> rsp % 16 仍是 8；再 `subq $0x28`（40 % 16 = 8）-> rsp % 16 == 0，
 * 满足 SysV "call 前 16 对齐"。那 40 字节顺带就是 Win64 要求的 32 字节影子空间 + 8 字节填充。
 *
 * ============================ 两类特殊跳板 ============================
 * 1) `__getmainargs` 有 **5 个参数** —— 第 5 个在栈上：**入口时 [rsp+0x28]**
 *    （= 调用者 rsp+0x20，跳过 32 字节影子空间）。通用宏只搬 4 个寄存器，所以它单独写，
 *    而且必须在 push 之前读那个栈参数。
 * 2) `fprintf` 是**可变参数** —— 必须按 Win64 的 va_list 结构体
 *    `{ gp_offset, fp_offset, overflow_arg_area, reg_save_area }` 现场打包。
 */

    .section .text.peload_stubs,"ax",@progbits

/* 通用跳板：Win64 rcx/rdx/r8/r9 -> SysV rdi/rsi/rdx/rcx，再 call pl_do_<fn>
 * 第一个参数是**导入名**（拼出 pl_stub_<name>，msvcrt 的导入名本身带前导下划线），
 * 第二个是**C 实现名后缀**（拼出 pl_do_<fn>，去掉了导入名的前导下划线）。 */
    .macro W2S name, fn
    .globl pl_stub_\name
    .type  pl_stub_\name, @function
pl_stub_\name:
    pushq   %rsi                        /* ★ 陷阱 2：Win64 里 rsi/rdi 是 callee-saved */
    pushq   %rdi
    movq    %rcx, %rdi
    movq    %rdx, %rsi
    movq    %r8,  %rdx
    movq    %r9,  %rcx
    subq    $0x28, %rsp
    call    pl_do_\fn
    addq    $0x28, %rsp
    popq    %rdi
    popq    %rsi
    ret
    .size pl_stub_\name, .-pl_stub_\name
    .endm

/* ---- kernel32 ---- */
    W2S ExitProcess,                ExitProcess
    W2S GetProcAddress,             GetProcAddress
    W2S LoadLibraryA,               LoadLibraryA
    W2S GetLastError,               GetLastError
    W2S SetUnhandledExceptionFilter, SetUnhandledExceptionFilter
    W2S Sleep,                      Sleep
    W2S VirtualProtect,             VirtualProtect
    W2S VirtualQuery,               VirtualQuery
    W2S GetStartupInfoA,            GetStartupInfoA
    W2S IsDBCSLeadByte,             IsDBCSLeadByte

/* ---- msvcrt（老式 1998 版 ABI）---- */
    W2S _XcptFilter,                XcptFilter
    W2S __iob_func,                 iob_func
    W2S __set_app_type,             set_app_type
    W2S __setusermatherr,           setusermatherr
    W2S _amsg_exit,                 amsg_exit
    W2S _cexit,                     cexit
    W2S _exit,                      _exit
    W2S _initterm,                  initterm
    W2S atexit,                     atexit
    W2S abort,                      abort
    W2S exit,                       exit
    W2S fflush,                     fflush
    W2S malloc,                     malloc
    W2S memcpy,                     memcpy
    W2S puts,                       puts
    W2S setvbuf,                    setvbuf
    W2S strlen,                     strlen
    W2S vfprintf,                   vfprintf

/* ---- 特殊 1：__getmainargs 的第 5 个参数在栈上 ---- */
    .globl pl_stub___getmainargs
    .type  pl_stub___getmainargs, @function
pl_stub___getmainargs:
    movq    %r8, %rax                   /* 先把 envp（第 3 个）暂存，r8 马上要装第 5 个 */
    movq    0x28(%rsp), %r8             /* ★ 第 5 个：STARTUPINFO*（必须在改 rsp 之前读） */
    pushq   %rsi
    pushq   %rdi
    movq    %rcx, %rdi                  /* argc*      */
    movq    %rdx, %rsi                  /* argv***    */
    movq    %rax, %rdx                  /* envp***    */
    movq    %r9,  %rcx                  /* dowildcard */
    subq    $0x28, %rsp
    call    pl_do_getmainargs
    addq    $0x28, %rsp
    popq    %rdi
    popq    %rsi
    ret
    .size pl_stub___getmainargs, .-pl_stub___getmainargs

/* ---- 特殊 2：fprintf 的可变参数 -> Win64 va_list -> pl_do_fprintf ---- */
    .globl pl_stub_fprintf
    .type  pl_stub_fprintf, @function
pl_stub_fprintf:
    pushq   %rsi                        /* ★ 陷阱 2 */
    pushq   %rdi
    subq    $0xD8, %rsp                 /* 入口 8 -> 两次 push 后仍 8 -> 减去 0xD8(216) -> 0，16 对齐 */
    /* 布局：影子空间 [rsp+0x00..0x1F]（留给 call）；
     *       reg_save_area [rsp+0x20..0x3F] 存 4 个整数寄存器；
     *       va_list [rsp+0x40..0x57]。 */
    movq    %rcx, 0x20(%rsp)
    movq    %rdx, 0x28(%rsp)
    movq    %r8,  0x30(%rsp)
    movq    %r9,  0x38(%rsp)
    movl    $16,   0x40(%rsp)           /* gp_offset：2 个具名参数已用掉（槽 0、1），下一个是 +16 */
    movl    $48,   0x44(%rsp)           /* fp_offset：W2 不处理浮点参数 */
    leaq    0xF0(%rsp), %rax            /* overflow_arg_area = 原始 rsp + 8（两次 push 16 + 0xD8） */
    movq    %rax,  0x48(%rsp)
    leaq    0x20(%rsp), %rax            /* reg_save_area */
    movq    %rax,  0x50(%rsp)

    movq    0x20(%rsp), %rdi            /* stream */
    movq    0x28(%rsp), %rsi            /* fmt */
    leaq    0x40(%rsp), %rdx            /* &va */
    call    pl_do_fprintf
    addq    $0xD8, %rsp
    popq    %rdi
    popq    %rsi
    ret
    .size pl_stub_fprintf, .-pl_stub_fprintf

    .section .note.GNU-stack,"",@progbits
