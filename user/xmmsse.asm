; xmmsse.asm - ★ A3 下半：**FPU/xmm 上下文切换**的 ring3 回归程序（两个进程同时用它）
;
; 干什么（为什么要这么写）：
;   每个进程把 **xmm0..xmm15 全部 16 个寄存器**初始化成"与 pid 绑定的模式"，
;   然后循环 ROUNDS 轮：每轮给 16 个寄存器各加 1（paddd），再 nanosleep 1ms
;   —— nanosleep 会真的让出 CPU，调度器把**另一个跑同一份程序的进程**切进来；
;   每 10 轮把 16 个寄存器逐个存到内存里核对（值必须等于 pattern + 已跑轮数）。
;   如果内核在任务切换处**不保存/恢复 xmm**，两个进程会互相污染 —— 核对必然失败，
;   程序打印 ok=0 并以退出码 1 结束（tests/dynlink64_test.py 断言必须是 ok=1 / code=0）。
;
; 约定（与 kernel/kernel64.cpp 的 fpu64_demo64 配套）：
;   * 跑法：内核建**两个真进程**（自己的 CR3/任务），同一个 ELF 跑两遍，各打一行
;     `[XMM] pid=<n> ok=1 rounds=300`；两个 pid 必须不同；
;   * 用 Linux syscall ABI（getpid 39 / nanosleep 35 / write 1 / exit 60）；
;   * 链接：nasm -f elf64 + ld.lld -T user/hello_elf64.ld（ET_EXEC，钉在 4GiB 装载区）。
;
; 与 .so/动态链接无关（纯 FPU 回归）；SSE 由内核进 ring3 前的 u64_fpu_enable64 打开
; （CR4.OSFXSR/OSXMMEXCPT；见 kernel/usermode64.cpp）。
bits 64
default rel

%define ROUNDS      300
%define VERIFY_EVERY 10

global _start

section .rodata align=16
one_vec:    times 4 dd 1                                   ; paddd 用的 16 字节对齐常量
one_f:      times 4 dd 0x3F800000                          ; 1.0f × 4（mulps 用：真浮点路径）
s_pre:      db "[XMM] pid="
s_pre_len:  equ $ - s_pre
s_mid:      db " ok="
s_mid_len:  equ $ - s_mid
s_post:     db " rounds=", 0x30 + (ROUNDS/100) % 10, 0x30 + (ROUNDS/10) % 10, 0x30 + ROUNDS % 10, 10
s_post_len: equ $ - s_post

section .bss
pidbuf:     resb 24
line:       resb 48                                 ; 拼整行用（一次 write；见 .report 段的说明）
out_buf:    resb 16
exp_val:    resd 1

section .text
_start:
    mov     eax, 39                                     ; getpid
    syscall
    mov     r12d, eax                                   ; r12d = pid（十进制打印 + pattern 都靠它）

    ; ---- 模式基值：base = pid * 0x101（每个进程不同；16 个寄存器再各自加 k*0x10001）----
    mov     r13d, r12d
    imul    r13d, r13d, 0x101

    ; ---- xmm0..xmm15 = replicate32(base + k*0x10001) ----
%assign K 0
%rep 16
    mov     eax, r13d
    add     eax, K * 0x10001
    movd    xmm %+ K, eax
    pshufd  xmm %+ K, xmm %+ K, 0
%assign K K+1
%endrep

    mov     r14, 0                                      ; 已跑轮数
    mov     r15, 1                                      ; ok = 1

.round:
    ; ---- 每轮：16 个 xmm 各 +1（SSE2 整数向量），xmm15 再乘一次 1.0f（真浮点，用 MXCSR）----
%assign K 0
%rep 16
    paddd   xmm %+ K, [one_vec]
%assign K K+1
%endrep
    mulps   xmm15, [one_f]

    ; ---- 让出 CPU（1ms）：另一个进程会在这段时间里用**它的** xmm ----
    mov     eax, 35                                     ; nanosleep(req, rem)
    sub     rsp, 16
    mov     qword [rsp], 0                              ; tv_sec = 0
    mov     qword [rsp + 8], 1000000                    ; tv_nsec = 1ms
    mov     rdi, rsp
    xor     esi, esi
    syscall
    add     rsp, 16

    inc     r14

    ; ---- 每 VERIFY_EVERY 轮核对一次：xmm_k 必须 == base + k*0x10001 + rounds ----
    mov     rax, r14
    xor     edx, edx
    mov     ecx, VERIFY_EVERY
    div     ecx
    test    edx, edx
    jnz     .next
    call    verify
    test    r15, r15
    jz      .report                                     ; 失败：立刻报告（不再跑完）

.next:
    cmp     r14, ROUNDS
    jb      .round

.report:
    ; ---- [XMM] pid=<pid> ok=<0|1> rounds=300\n ----
    ; ★ 先在内存里拼好整行，最后**一次** write(1, line, n)。
    ;   为什么必须一次：内核的 syscall 跟踪（[SYSCALL] insn nr=1 …）打在同一个串口上，
    ;   分成 5 次小 write 会让跟踪行插进这一行的中间，验收脚本就没法"逐字节"比对整行
    ;   （实测就是这个现象：`[XMM] pid=[SYSCALL] insn …`）。
    lea     rdi, [line]                     ; rdi = 写游标
    lea     rsi, [s_pre]
    mov     ecx, s_pre_len
    rep     movsb

    ; pid 十进制：先算到 pidbuf 尾部（倒着写），再正序拷进 line
    mov     eax, r12d
    lea     rsi, [pidbuf + 23]
    mov     byte [rsi], 0
    mov     ecx, 10
.pidloop:
    dec     rsi
    xor     edx, edx
    div     ecx
    add     dl, '0'
    mov     [rsi], dl
    test    eax, eax
    jnz     .pidloop
    lea     rcx, [pidbuf + 23]              ; 数字长度 = 尾部 - 首字符
    sub     rcx, rsi
    rep     movsb

    lea     rsi, [s_mid]
    mov     ecx, s_mid_len
    rep     movsb

    mov     al, '0'
    test    r15, r15
    jz      .ok0
    mov     al, '1'
.ok0:
    mov     [rdi], al
    inc     rdi

    lea     rsi, [s_post]
    mov     ecx, s_post_len
    rep     movsb

    lea     rsi, [line]                     ; 一次 write(1, line, rdi - line)
    mov     rdx, rdi
    sub     rdx, rsi
    mov     eax, 1
    mov     edi, 1
    syscall

    ; ---- exit(ok ? 0 : 1) ----
    mov     eax, 60
    xor     edi, edi
    test    r15, r15
    jnz     .exit
    mov     edi, 1
.exit:
    syscall
    hlt
; ---------------------------------------------------------------------------
; verify: 逐个把 xmm0..15 存到 out_buf 并与期望值比较；不等 -> r15 = 0
;   期望值 = base(=r13d) + k*0x10001 + rounds(=r14)
;   只用内存 + GPR（16 个 xmm 全在被测，不能拿它们当暂存）
; ---------------------------------------------------------------------------
verify:
%assign K 0
%rep 16
    mov     eax, r13d
    add     eax, K * 0x10001
    add     eax, r14d
    mov     [exp_val], eax
    movdqu  [out_buf], xmm %+ K
    mov     ecx, [out_buf]
    cmp     ecx, [exp_val]
    jne     .fail
    cmp     ecx, [out_buf + 4]
    jne     .fail
    cmp     ecx, [out_buf + 8]
    jne     .fail
    cmp     ecx, [out_buf + 12]
    jne     .fail
%assign K K+1
%endrep
    ret
.fail:
    xor     r15, r15
    ret
