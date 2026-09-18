; syscall_entry64.asm - Vimtu64 的 syscall 指令入口（Linux 兼容路径）
;
; 谁跳到这里：LSTAR（IA32_LSTAR = 0xC0000082）指向 syscall64_insn_entry64，
;   由 kernel/syscall64.cpp 的 syscall64_init_msr64() 写入（同时开 EFER.SCE、
;   写 STAR = 0x1B<<48 | 0x08<<32、FMASK = 0x700）。用户侧只要一条 `syscall`。
;
; ============================ 为什么必须单独写这段汇编 ============================
; SYSCALL 与 int 0x80 有三处根本差别，靠 C 代码补不了：
;   1) SYSCALL **不换栈**：进入时 rsp 还是用户 rsp（中断门会由 CPU 用 TSS.rsp0 切栈）；
;   2) SYSCALL **不压任何栈**：只有 rcx=返回地址、r11=RFLAGS，没有 rip/cs/rflags/rsp/ss；
;   3) SYSRET 用 rcx→RIP、r11→RFLAGS，并且**拿当前 rsp 当用户 rsp**（同样不换栈）。
; 所以入口要自己换栈 + 自己压出一份完整帧；出口要自己把 rsp 换回用户栈再 sysretq。
;
; 内核栈从哪来：内核内存里的变量 g_syscall64_kstack64（kernel/syscall64.cpp 定义）。
;   它是 **TSS.rsp0 的镜像**：kernel/usermode64.cpp 每次进 ring3 前，在 tss_set_rsp0()
;   的同一处把它一起更新（一个 store，值必然一致）。
;   为什么用镜像变量而不是直接读 TSS.rsp0：TSS 结构体在 kernel/x86_64.cpp 里是
;   static，汇编要读它就得再加一个导出函数；而"进 ring3 前一定有正确值"这件事在
;   usermode64.cpp 里已经是一个点，镜像一份最省事、也最容易核对（改动只需看那一处）。
;   兜底初值 0x80000 与 tss_init64(0x80000)/usermode64 的兜底值一致。
;
; 帧布局：与 kernel/isr_stubs64.asm 的 0xD0 布局**逐字段一致**（那边是权威定义）。
;   cs=0x2B、ss=0x23、rip=rcx（CPU 存的返回地址）、rflags=r11（CPU 存的 RFLAGS）、
;   rsp=用户 rsp、err_code=0。int_no 槽填 0x180 = SYSCALL64_INSM_FRAME_MARK64：
;   分发器（syscall64_dispatch64）靠它区分"syscall 指令路径（Linux 号段）"与
;   "int 0x80 路径（自有 ABI，int_no=0x80）"。这两条路径的帧结构因此完全相同，
;   0xD0 布局与 pt_regs64 的那些断言继续成立。
;
; 中断（IF）：FMASK=0x700 已经清掉 TF/IF/DF，所以入口处中断是关的；分发期间**保持
;   IF=0**（与 int 0x80 的中断门行为一致，也避免被 PIT 抢占后复用同一段内核栈）。
;   回到用户态时由 sysretq 用 r11 里的用户 RFLAGS 恢复 IF=1（用户程序的 rflags=0x202）。
;
; 出口两条路：
;   A) 普通返回：sysretq（rcx→RIP、r11→RFLAGS、当前 rsp 就是用户 rsp）。
;      返回的 rax 是分发器写回帧里的 rax（负值 = -errno）。
;      用户 rsp 由帧的 0xC0 槽（`pop rsp`）换回去 —— 内核栈因此不会被泄给用户；
;      gs/fs 全程不动（本内核不用 swapgs，也没有 per-CPU gs，见 elf64/syscall64 说明）。
;   B) exit：分发器把 g_syscall64_exit_to_kernel64 置 1，这里丢掉整帧后直接
;      jmp user64_resume_tramp64 —— 那是内核里"从 ring3 回来"的唯一出口（复位内核栈与
;      callee-saved 后 ret 回 user64_run_blob64 / elf64_run64 的调用点）。
;      ★ 绝不能对那个蹦床用 sysretq：它的 CS 是 0x08（内核），而 sysretq 会按 STAR
;        强行把 CPL 设成 3，用户态执行内核代码/特权指令立刻 #GP。
;
; 与 kernel/x86_64.h 的对应：帧字段偏移必须与 struct pt_regs64 一致（0xD0 = PR64_SIZE）。
; ==============================================================================
bits 64

extern syscall64_dispatch64
extern user64_resume_tramp64                 ; usermode64.cpp 的 ring0 收尾蹦床
extern g_syscall64_kstack64                  ; 内核栈顶镜像（TSS.rsp0 的镜像）
extern g_syscall64_exit_to_kernel64          ; 1 = 本帧已 exit，别 sysret

%define FRAME_SIZE  0xD0                     ; = PR64_SIZE（kernel/x86_64.h）
%define FRAME_MARK  0x180                    ; = SYSCALL64_INSM_FRAME_MARK64（kernel/syscall64.h）
%define SEL_KCODE   0x08
%define SEL_KDATA   0x10
%define SEL_UCODE   0x2B                     ; STAR[63:48]=0x1B 推出：CS=+16、SS=+8
%define SEL_UDATA   0x23

section .bss
global g_syscall64_user_rsp64
g_syscall64_user_rsp64: resq 1               ; 换栈前临时存用户 rsp（窗口期 IF=0，不会重入）

section .text
global syscall64_insn_entry64

syscall64_insn_entry64:
    cli                                      ; 冗余但明确：FMASK 已清 IF，分发期间保持关中断
    mov     [rel g_syscall64_user_rsp64], rsp ; ★ 唯一必须先落内存的值（下面 rsp 要换掉）
    mov     rsp, [rel g_syscall64_kstack64]   ; 换到内核栈顶（= TSS.rsp0 的镜像）

    ; ---- 按 0xD0 布局压帧：push 朝低地址走，所以从**最高地址字段**开始压 ----
    push    qword SEL_UDATA                  ; 0xC8 ss
    push    qword [rel g_syscall64_user_rsp64] ; 0xC0 rsp（用户 rsp）
    push    r11                              ; 0xB8 rflags（CPU 在 r11 里放着）
    push    qword SEL_UCODE                  ; 0xB0 cs（iretq 回去时用得上，本例只给 sysret 用）
    push    rcx                              ; 0xA8 rip（CPU 在 rcx 里放着）
    push    qword 0                          ; 0xA0 err_code
    push    qword FRAME_MARK                 ; 0x98 int_no（= 0x180，标记"syscall 指令路径"）
    push    rax                              ; 0x90 rax = 系统调用号
    push    rcx                              ; 0x88 rcx（已被 CPU 覆盖成返回地址，与 Linux 一致）
    push    rdx                              ; 0x80 rdx = 参数 3
    push    rbx                              ; 0x78
    push    rbp                              ; 0x70
    push    rsi                              ; 0x68 rsi = 参数 2
    push    rdi                              ; 0x60 rdi = 参数 1
    push    r8                               ; 0x58（第 5 个参数寄存器，本内核未用）
    push    r9                               ; 0x50
    push    r10                              ; 0x48（第 4 个参数寄存器，本内核未用）
    push    r11                              ; 0x40（= 用户 RFLAGS，与 Linux 一样属于易失值）
    push    r12                              ; 0x38
    push    r13                              ; 0x30
    push    r14                              ; 0x28
    push    r15                              ; 0x20
    push    qword SEL_KDATA                  ; 0x18 ds 占位（出口整块跳过，不加载段寄存器）
    push    qword SEL_KDATA                  ; 0x10 es
    push    qword SEL_KDATA                  ; 0x08 fs
    push    qword SEL_KDATA                  ; 0x00 gs

    ; 帧指针 = 最低地址（gs 槽），与 isr_stubs64.asm 把 rdi=rsp 交给 isr_handler64 一致
    mov     qword [rel g_syscall64_exit_to_kernel64], 0
    mov     rdi, rsp
    call    syscall64_dispatch64

    ; ---- 出口 A：exit 走内核蹦床（不回用户态）----
    cmp     qword [rel g_syscall64_exit_to_kernel64], 0
    jne     .to_kernel

    ; ---- 出口 B：严格逆序还原，最后 sysretq ----
    add     rsp, 32                          ; 4 个段占位槽（长模式不加载 ds/es/fs/gs）
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11                              ; 用户 r11（易失，sysret 会覆盖成 rflags）
    pop     r10
    pop     r9
    pop     r8
    pop     rdi
    pop     rsi
    pop     rbp
    pop     rbx
    pop     rdx
    pop     rcx                              ; 用户 rcx（易失，下面覆盖成 RIP）
    pop     rax                              ; ★ 返回值：分发器写回的帧 rax
    add     rsp, 16                          ; int_no + err_code
    pop     rcx                              ; rcx = 用户 RIP      （sysretq 用）
    add     rsp, 8                           ; 跳过 cs（sysret 按 STAR 设 0x2B）
    pop     r11                              ; r11 = 用户 RFLAGS   （sysretq 用）
    pop     rsp                              ; ★ rsp = 用户 rsp：最后一步换回用户栈
    ; ★ 必须写 `o64 sysret`（= REX.W + 0F 07 = GAS 的 sysretq）：
    ;   NASM 的裸 `sysret` 汇编成 **sysretl（0F 07，32 位形式）**，而 32 位形式的 SYSRET 用
    ;   CS = STAR[63:48]（= 0x1B，GDT 保留槽！）+ RIP = ECX（把 64 位地址截成 32 位），
    ;   结果第一次返回用户态就 #GP err=0x18（0x1B & ~7），现场里 rip=0x1A8 / cs=0x1B。
    ;   64 位形式才是 CS = STAR[63:48]+16 = 0x2B、RIP = RCX。
    o64 sysret                               ; REX.W + 0F 07：rcx->RIP、r11->RFLAGS、CS/SS 由 STAR 推出
.to_kernel:
    add     rsp, FRAME_SIZE                  ; 整帧丢掉（不再回用户态）
    jmp     user64_resume_tramp64            ; 复位内核栈 + callee-saved 后 ret 回调用点
