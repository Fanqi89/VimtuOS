; kpti_blob64.asm - KPTI（内核页表隔离）的**共享入口区**：整段映射在固定 VA
;                   KPTI64_BASE = 0x0000000180000000（6 GiB），**内核表与用户表都映射它**，
;                   所以 ring3 里发生的 int/异常/IRQ/syscall 能在"还没切 CR3"的那几条指令里
;                   找到可执行的代码、可用的栈、以及 IDT/GDT/TSS 指向的数据。
;
; ============================ 为什么需要它 ============================
; KPTI 打开后，用户地址空间里**没有**内核高半区（PML4[511]，kernel/kpti64.cpp 把那条清 0）。
; 于是 CPU 在 ring3 里收到中断时：
;   1) 取 IDT 门  —— IDTR.base 指的本节里的 IDT 页（不在内核高半区）；
;   2) 读 TSS.rsp0 —— GDT/TSS 也在本节里（gdt/tss 描述符由 kernel/x86_64.cpp 搬到本节）；
;   3) 跳到门目标 —— 就是下面的 kpti64_tramp64_<向量>（内核高半区的 isrN_64 在这里不可取指）。
; 蹦床只做三件事：补 vec/err 两格、判断来源（CS.RPL）、然后
;   * 来自内核：直接 jmp isr64_common（CR3 本来就是内核表）；
;   * 来自用户：把帧搬到"本任务的真实内核栈"，先切到内核 CR3，再 jmp isr64_common。
; 出口对称：isr_stubs64.asm 的公共出口改跳 kpti64_iret_frame64（见那边的说明），
;   它在"要回 ring3"时把帧搬到本节里的返回栈、切回用户 CR3，再从本节 iretq。
;
; ============================ 本节 = 2 页（镜像内），另有 1 页运行时分配 ============================
;   页 0（6 GiB + 0）：代码 + 变量 + GDT/TSS 副本 + 入口栈 + 返回栈（都是**本节的相对布局**，
;                        所以 `default rel` 的 [label] 天然正确：整节被整段映射到固定 VA）；
;   页 1（0x140001000）：向量偏移表（kpti64_offtab64，256 个 qword，0 = 该向量没装门）；
;   页 2（0x140002000）：IDT（4 KiB，运行时从 g_idt 拷过来，见 kernel/x86_64.cpp）。
;
; ★ 坑：本节的链接地址是内核高半区（0xFFFFFFFF80100xxx），**不是** 6 GiB 那个共享 VA。
;   节内相对地址（[rel label]、相对 jmp）在重定位后仍成立；但 C 侧要拿"共享 VA"时必须用
;   KPTI64_BASE + (符号 - kpti64_blob64) 换算（kernel/kpti64.cpp 的 dav64()）。
; ==============================================================================
bits 64
default rel

%define FRAME_SIZE  0xD0          ; = PR64_SIZE（kernel/x86_64.h），帧布局唯一权威定义见 isr_stubs64.asm
%define FRAME_MARK  0x180         ; = SYSCALL64_INSM_FRAME_MARK64（kernel/syscall64.h）
%define SEL_KCODE   0x08
%define SEL_KDATA   0x10
%define SEL_UCODE   0x2B
%define SEL_UDATA   0x23

extern isr64_common                       ; isr_stubs64.asm（已 global）
extern syscall64_dispatch64
extern g_syscall64_exit_to_kernel64
extern user64_resume_tramp64

section .text.kpti align=4096

global kpti64_blob64
global kpti64_offtab64
extern isr64_common_call64                ; isr_stubs64.asm：帧已压好、只差 call 的那一点
global kptii_gdt64
global kptii_tss64
global kptii_entry_rsp64
global kptii_ret_rsp64
global kptii_kstack64
global kptii_user_cr364
global kptii_user_rsp64
global kptii_entry_stk64
global kptii_ret_stk64
global kpti64_syscall_entry64
global kpti64_user_entry64
global kpti64_iret_frame64
global kpti64_syscall_entry64

kpti64_blob64:
    ; ★ 8 字节魔数：一是"这段确实被映射到共享 VA"的标记（C 侧可据此核对别名），
    ;   二是保证 tramp[0] 的偏移 != 0 —— 偏移表里 0 表示"该向量没有蹦床"，
    ;   若蹦床 0 正好落在偏移 0，除零向量就会**悄悄退回内核高半区的桩**
    ;   （v0.11.9 实测过：tramp 计数 49 而不是 50）。
    dq 0x4B50544936344100

; ============================ 向量蹦床 ============================
; 帧布局（入口那一刻，rsp 指向最新压入的项）：
;   [rsp+0]  vec        （本节 push）
;   [rsp+8]  err_code   （CPU 压的，或本节补 0）
;   [rsp+16] rip        （CPU）
;   [rsp+24] cs         （CPU）  ← 用它判"从哪来的"
;   [rsp+32] rflags / [rsp+40] rsp / [rsp+48] ss（仅特权级切换时存在）
%macro KTRAMP 2
global kpti64_tramp64_%1
kpti64_tramp64_%1:
%if %2 == 0
    push qword 0                          ; 无错误码的向量：补一个 0（与 isr_stubs64 一致）
%endif
    push qword %1
    test byte [rsp + 24], 3               ; CS.RPL
    jnz kpti64_user_entry64
    ; ★ 这里**不能用 mov rax, isr64_common; jmp rax**：此刻通用寄存器还是被中断现场的
    ;   用户值（isr64_common 还没压它们），clobber rax 会把它恢复成"isr64_common 的地址"。
    ;   实测过：kmain 的 tick 比较寄存器被这样改成了 0xffffffff803a5f58，于是在 hlt 里死等。
    ;   走内存间接跳转 —— 不碰任何通用寄存器。
    jmp qword [kptii_isr64_common_ptr]
%endmacro

KTRAMP 0, 0      ; 除零
KTRAMP 1, 0
KTRAMP 2, 0      ; NMI
KTRAMP 3, 0
KTRAMP 4, 0
KTRAMP 5, 0
KTRAMP 6, 0
KTRAMP 7, 0
KTRAMP 8, 1      ; 双重故障（CPU 压错误码）
KTRAMP 9, 0
KTRAMP 10, 1
KTRAMP 11, 1
KTRAMP 12, 1
KTRAMP 13, 1     ; 通用保护
KTRAMP 14, 1     ; 缺页
KTRAMP 15, 0
KTRAMP 16, 0
KTRAMP 17, 1
KTRAMP 18, 0
KTRAMP 19, 0
KTRAMP 20, 0
KTRAMP 21, 1
KTRAMP 22, 0
KTRAMP 23, 0
KTRAMP 24, 0
KTRAMP 25, 0
KTRAMP 26, 0
KTRAMP 27, 0
KTRAMP 28, 0
KTRAMP 29, 0
KTRAMP 30, 1
KTRAMP 31, 0
KTRAMP 32, 0     ; IRQ0（PIT）
KTRAMP 33, 0
KTRAMP 34, 0
KTRAMP 35, 0
KTRAMP 36, 0
KTRAMP 37, 0
KTRAMP 38, 0
KTRAMP 39, 0
KTRAMP 40, 0
KTRAMP 41, 0
KTRAMP 42, 0
KTRAMP 43, 0
KTRAMP 44, 0
KTRAMP 45, 0
KTRAMP 46, 0
KTRAMP 47, 0
KTRAMP 128, 0    ; int 0x80（自有 ABI 的系统调用门）

; 0xFF：APIC 伪中断向量。语义与 apic64.cpp 的 apic64_spurious_stub64 完全一致 ——
;   伪中断没有错误码、也没有我们的帧，CPU 压了 5 格，直接 iretq 回去即可（SDM：不需要 EOI）。
;   从 ring3 进来时 CR3 仍是用户表，本节的 iretq 在这里可执行 ✓。
global kpti64_tramp64_255
kpti64_tramp64_255:
    iretq

; ============================ 用户态入口 ============================
; 进入时：CPL0、CR3 = 用户表、rsp = 本节入口栈顶（TSS.rsp0，见 kernel/x86_64.cpp）
; 帧（vec/err/rip/cs/rflags/rsp/ss）已在栈上，通用寄存器仍是**用户的值**。
;
; 关键约束（★ 实测踩过，别改回去）：
;   * 换栈/搬运前必须先把**全部**通用寄存器压进帧 —— 之前只 push rax/rbx 当暂存，
;     结果 rep movsq 用的 rsi/rdi/rcx 把用户的三个寄存器改掉了；
;   * 内核对等 CR3 从**当前表的 PML4[255]** 取（一个"非 present 的数据槽"，
;     见 kernel/proc64.cpp 的 p64_build_as64）。0 = 共享地址空间（引导期表 /
;     没有独立地址空间的 ring3 演示）→ 不切 CR3，其余照做；
;   * 压完帧之后再动 rax/rbx 是安全的：它们的用户值已经躺在帧里了。
kpti64_user_entry64:
    ; ---- 按 isr_stubs64 的**同一顺序**压通用寄存器（15 个）+ 4 个段占位槽 ----
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    push qword 0x10
    push qword 0x10
    push qword 0x10
    push qword 0x10
    ; rsp 现在指向帧起点（0xD0 布局：vec/err/rip/cs/rflags/rsp/ss 已由 CPU + 蹦床压好）
    mov rax, cr3
    and rax, ~0xFFF
    mov rbx, [rax + 255*8]                ; PML4[255] = 同一进程的内核表（0 = 共享）
    test rbx, rbx
    jz .same_space
    mov cr3, rbx
.same_space:
    mov rsi, rsp                          ; 源：入口栈上的整帧（两张表都映射）
    mov rdi, [kptii_kstack64]             ; 目的：本任务真实内核栈顶
    sub rdi, FRAME_SIZE
    mov rsp, rdi
    mov rcx, FRAME_SIZE / 8
    rep movsq
    mov rdi, rsp                          ; rdi = 内核栈上的帧起点
    jmp qword [kptii_isr64_common_call_ptr]

; ============================ 出口（回 iret 帧）============================
; void kpti64_iret_frame64(uint64_t frame)   frame = 0xD0 布局的帧起点（内核栈或返回栈上）
;   * CS.RPL == 3 且本进程有用户表 → 帧搬到本节返回栈，切回用户 CR3，从本节 pop+iretq；
;   * 其余（回内核上下文 / 共享地址空间）→ 原地 pop+iretq（与 v0.11.8 完全一致）。
; 进入时通用寄存器可以随便用：它们的用户值都在帧里，最后 pop 回来。
; ★ 必须 cli：从"把帧搬到返回栈"到 iretq 之间，rsp 落在**共享区**里。此时若来一个中断，
;   调度器会把"帧指针在共享栈上"当成坏帧（task64 的 frameprobe 直接 halt —— 实测踩过：
;   [TASK64] frameprobe FAIL frame=0x180000E10）。iretq 用帧里的 rflags 恢复 IF，
;   所以这里关中断不丢任何状态。
kpti64_iret_frame64:
    cli
    test byte [rdi + 0xB0], 3             ; 帧里的 cs
    jz .plain
    mov rax, cr3
    and rax, ~0xFFF
    mov rbx, [rax + 255*8]                ; 本进程的用户表（0 = 共享地址空间）
    test rbx, rbx
    jz .plain
    mov rsi, rdi
    mov rdi, [kptii_ret_rsp64]
    sub rdi, FRAME_SIZE
    mov rsp, rdi
    mov rcx, FRAME_SIZE / 8
    rep movsq
    mov cr3, rbx                          ; ★ 切回用户表：下面只碰本节的数据（两张表都映射）
    jmp .tail
.plain:
    mov rsp, rdi
.tail:
    add rsp, 32                           ; 4 个段占位槽（长模式下不加载 ds/es/fs/gs）
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16                           ; int_no + err_code
    iretq

; ============================ syscall 指令入口 ============================
; LSTAR 指向这里（kernel/syscall64.cpp 的 MSR 设置；KPTI 打开时才指这里）。
; 与 kernel/syscall_entry64.asm 的帧布局**逐字段一致**，差别只有两处：
;   1) 入口栈用本节入口栈顶（[kptii_entry_rsp64]），不是内核内存里的 g_syscall64_kstack64；
;   2) 压完帧先把 0xD0 搬到内核栈并切到内核 CR3，再调分发器；出口在最后一刻切回用户 CR3。
kpti64_syscall_entry64:
    cli
    mov [kptii_user_rsp64], rsp           ; 用户 rsp（rsp 马上要换掉）
    mov rsp, [kptii_entry_rsp64]

    push qword SEL_UDATA                  ; 0xC8 ss
    push qword [kptii_user_rsp64]         ; 0xC0 rsp（用户）
    push r11                              ; 0xB8 rflags
    push qword SEL_UCODE                  ; 0xB0 cs
    push rcx                              ; 0xA8 rip
    push qword 0                          ; 0xA0 err_code
    push qword FRAME_MARK                 ; 0x98 int_no（标记 syscall 指令路径）
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    push qword SEL_KDATA                  ; 0x18 ds
    push qword SEL_KDATA                  ; 0x10 es
    push qword SEL_KDATA                  ; 0x08 fs
    push qword SEL_KDATA                  ; 0x00 gs

    ; ---- 切到内核地址空间，把帧搬到本任务内核栈 ----
    mov rax, cr3
    and rax, ~0xFFF
    mov rbx, [rax + 255*8]
    test rbx, rbx
    jz .same_space
    mov cr3, rbx
.same_space:
    mov rsi, rsp
    mov rdi, [kptii_kstack64]
    sub rdi, FRAME_SIZE
    mov rsp, rdi
    mov rcx, FRAME_SIZE / 8
    rep movsq

    ; ★ g_syscall64_exit_to_kernel64 在**内核 .bss**（别的节）：绝不能写
    ;   `mov qword [g_syscall64_exit_to_kernel64], 0` —— 那样 NASM 发出的是"相对
    ;   内核高半区"的 RIP 位移，同一段代码搬到共享 VA 跑时位移就指到别处
    ;   （实测 CR2=0x142C4D500 的 #PF）。本节内符号才可用 [rel]；跨节必须
    ;   用 imm64 取绝对地址再间接访存。
    mov rax, g_syscall64_exit_to_kernel64
    mov qword [rax], 0
    mov rdi, rsp
    mov rax, syscall64_dispatch64
    call rax

    mov rax, g_syscall64_exit_to_kernel64
    cmp qword [rax], 0
    jne .to_kernel

    ; ---- 出口：把帧搬到本节返回栈，再切回用户 CR3，从本节 pop + sysret ----
    ; 为什么必须搬（而不是像 v0.11.8 那样在内核栈上 pop 完再切）：
    ;   `mov cr3, 内存` 不存在（CR 只能和通用寄存器互写），而 pop 完所有通用寄存器
    ;   都已经是用户值了 —— 没有任何一个寄存器能腾出来放 CR3。所以：先切 CR3（此时
    ;   通用寄存器可以随便用，用户值都在帧里），再在**两张表都映射**的本节返回栈上 pop。
    ; ★ cli：这一段 rsp 在共享区里，被打断就会被调度器当成坏帧（frameprobe）。
    ;   sysretq 用 r11 里的用户 rflags 恢复 IF，所以不丢任何状态。
    cli
    mov rax, cr3
    and rax, ~0xFFF
    mov rbx, [rax + 255*8]
    test rbx, rbx
    jnz .have_ucr3
    mov rbx, rax                          ; 共享地址空间：切回自己（等价于不切）
.have_ucr3:
    mov rsi, rsp
    mov rdi, [kptii_ret_rsp64]
    sub rdi, FRAME_SIZE
    mov rsp, rdi
    mov rcx, FRAME_SIZE / 8
    rep movsq
    mov cr3, rbx                          ; ★ 切回用户表：下面只碰本节数据与本节栈

    add rsp, 32
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16
    pop rcx                               ; rcx = 用户 RIP   （sysretq 用）
    add rsp, 8                            ; 跳过 cs（sysret 按 STAR 推出 0x2B）
    pop r11                               ; r11 = 用户 RFLAGS
    pop rsp                               ; ★ rsp = 用户 rsp（最后一步换回用户栈）
    o64 sysret                            ; REX.W + 0F 07：rcx->RIP、r11->RFLAGS

.to_kernel:
    add rsp, FRAME_SIZE                   ; 整帧丢掉（不再回用户态）
    mov rax, user64_resume_tramp64        ; 内核里"从 ring3 回来"的唯一出口
    jmp rax

; ============================ 数据 + 栈（页 0 内）============================
; ★ 全是**本节内的相对布局**：汇编期就定好，整节映射到固定 VA 后自洽。
align 8
global kptii_gdt64
kptii_gdt64:     times 64  db 0           ; GDT 副本（8 项；TSS 描述符由 x86_64.cpp 重写）
global kptii_tss64
kptii_tss64:     times 112 db 0           ; TSS 副本（104 字节，对齐留量）
kptii_user_cr364: dq 0                    ; 出口要切回的用户 CR3
kptii_kstack64:  dq 0                     ; 本任务真实内核栈顶（tss_set_rsp0 的入参镜像）
kptii_entry_rsp64: dq 0                   ; 入口栈顶（= TSS.rsp0；初始化时由 C 填）

; ============================ 蹦床用的地址槽 ============================
; 为什么要有它们：蹦床在压通用寄存器**之前**运行，任何寄存器都不能当暂存用
; （踩过：mov rax,isr64_common 把被中断现场的 rax 改成了那个地址，kmain 的 tick 比较当场废掉）。
; 所以走"内存间接跳转" —— 目标地址从这两个槽里读，不碰任何通用寄存器。
align 8
global kptii_isr64_common_ptr
kptii_isr64_common_ptr:      dq isr64_common
global kptii_isr64_common_call_ptr
kptii_isr64_common_call_ptr: dq isr64_common_call64
kptii_ret_rsp64:   dq 0                   ; 返回栈顶（回 ring3 时帧搬到这里）
kptii_user_rsp64:  dq 0                   ; syscall 入口临时存用户 rsp

align 16
kptii_entry_stk64: times 1792 db 0        ; 入口栈（向下增长，顶在 0x700）
kptii_ret_stk64:   times 512  db 0        ; 返回栈（向下增长，顶在 0x900）

; ============================ 向量偏移表（页 1）============================
; 每个 qword = 该向量蹦床相对 kpti64_blob64 的偏移；0 = 该向量没有蹦床。
; 顺序严格按向量号 0..255（C 侧 kpti64_tramp64(vec) 直接查）。
align 4096
kpti64_offtab64:
    dq kpti64_tramp64_0   - kpti64_blob64
    dq kpti64_tramp64_1   - kpti64_blob64
    dq kpti64_tramp64_2   - kpti64_blob64
    dq kpti64_tramp64_3   - kpti64_blob64
    dq kpti64_tramp64_4   - kpti64_blob64
    dq kpti64_tramp64_5   - kpti64_blob64
    dq kpti64_tramp64_6   - kpti64_blob64
    dq kpti64_tramp64_7   - kpti64_blob64
    dq kpti64_tramp64_8   - kpti64_blob64
    dq kpti64_tramp64_9   - kpti64_blob64
    dq kpti64_tramp64_10  - kpti64_blob64
    dq kpti64_tramp64_11  - kpti64_blob64
    dq kpti64_tramp64_12  - kpti64_blob64
    dq kpti64_tramp64_13  - kpti64_blob64
    dq kpti64_tramp64_14  - kpti64_blob64
    dq kpti64_tramp64_15  - kpti64_blob64
    dq kpti64_tramp64_16  - kpti64_blob64
    dq kpti64_tramp64_17  - kpti64_blob64
    dq kpti64_tramp64_18  - kpti64_blob64
    dq kpti64_tramp64_19  - kpti64_blob64
    dq kpti64_tramp64_20  - kpti64_blob64
    dq kpti64_tramp64_21  - kpti64_blob64
    dq kpti64_tramp64_22  - kpti64_blob64
    dq kpti64_tramp64_23  - kpti64_blob64
    dq kpti64_tramp64_24  - kpti64_blob64
    dq kpti64_tramp64_25  - kpti64_blob64
    dq kpti64_tramp64_26  - kpti64_blob64
    dq kpti64_tramp64_27  - kpti64_blob64
    dq kpti64_tramp64_28  - kpti64_blob64
    dq kpti64_tramp64_29  - kpti64_blob64
    dq kpti64_tramp64_30  - kpti64_blob64
    dq kpti64_tramp64_31  - kpti64_blob64
    dq kpti64_tramp64_32  - kpti64_blob64
    dq kpti64_tramp64_33  - kpti64_blob64
    dq kpti64_tramp64_34  - kpti64_blob64
    dq kpti64_tramp64_35  - kpti64_blob64
    dq kpti64_tramp64_36  - kpti64_blob64
    dq kpti64_tramp64_37  - kpti64_blob64
    dq kpti64_tramp64_38  - kpti64_blob64
    dq kpti64_tramp64_39  - kpti64_blob64
    dq kpti64_tramp64_40  - kpti64_blob64
    dq kpti64_tramp64_41  - kpti64_blob64
    dq kpti64_tramp64_42  - kpti64_blob64
    dq kpti64_tramp64_43  - kpti64_blob64
    dq kpti64_tramp64_44  - kpti64_blob64
    dq kpti64_tramp64_45  - kpti64_blob64
    dq kpti64_tramp64_46  - kpti64_blob64
    dq kpti64_tramp64_47  - kpti64_blob64
    times 80 dq 0                          ; 48..127：没有门
    dq kpti64_tramp64_128 - kpti64_blob64
    times 126 dq 0                         ; 129..254
    dq kpti64_tramp64_255 - kpti64_blob64
