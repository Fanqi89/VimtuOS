; proc64.asm - Vimtu64 多进程演示程序（静态 ELF64，ring3，用 syscall 指令与内核通信）
;
; 它验证批次 C 的进程语义（三条都要求有**真实**内核支持，不是打印假话）：
;   1) fork：父子各自有独立地址空间（同一个 VA 各自读到自己的值）
;   2) execve：子进程换成别的映像继续跑（子进程的 exit code 由父进程 wait4 收到）
;   3) kill(SIGTERM)：父进程终止另一个子进程，wait4 收到 143 退出码编码的 status
;
; 模式（argv[1]）：
;   无      -> 父进程模式（跑完整演示）
;   child2  -> 被 execve 拉起来的那个映像：打印一行后 exit(7)
;   killme  -> 被 execve 拉起后一直 nanosleep，等父进程 kill
;
; 号段（Linux x86_64，见 kernel/syscall64.cpp 的文件头大表）：
;   1 write / 9 mmap / 35 nanosleep / 39 getpid / 57 fork / 59 execve / 61 wait4 / 62 kill /
;   60 exit / 110 getppid
;
; 构建（见 build64.sh）：
;   nasm -f elf64 user/proc64.asm -o build64/proc64.o
;   ld.lld -m elf_x86_64 -T user/hello_elf64.ld -o build64/proc64.elf build64/proc64.o
;   objcopy -I binary -> 嵌进系统内核（proc64.cpp 幂等装成 VimtuFS2 的 /proc64.elf）
bits 64
default rel

%define NR_WRITE       1
%define NR_MMAP        9
%define NR_NANOSLEEP   35
%define NR_GETPID      39
%define NR_FORK        57
%define NR_EXECVE      59
%define NR_WAIT4       61
%define NR_KILL        62
%define NR_EXIT        60
%define NR_GETPPID     110

global _start

section .text
_start:
    ; ---- 解析 argv[1]（argc = [rsp]，argv[0] = [rsp+8]，argv[1] = [rsp+16]）----
    cmp     qword [rsp], 2
    jl      .parent                          ; argc < 2 -> 父进程模式
    mov     rdi, [rsp + 16]                  ; argv[1]
    lea     rsi, [m_child2]
    call    str_eq
    test    eax, eax
    jnz     .mode_child2
    mov     rdi, [rsp + 16]
    lea     rsi, [m_killme]
    call    str_eq
    test    eax, eax
    jnz     .mode_killme

; ======================================================================
; 父进程模式
; ======================================================================
.parent:
    lea     rsi, [m_banner]
    mov     edx, m_banner_len
    call    write_stdout

    ; ---- getpid / getppid ----
    lea     rsi, [m_parent_pid]
    mov     edx, m_parent_pid_len
    call    write_stdout
    mov     eax, NR_GETPID
    syscall
    mov     [pid], eax
    call    print_sint
    lea     rsi, [m_ppid]
    mov     edx, m_ppid_len
    call    write_stdout
    mov     eax, NR_GETPPID
    syscall
    call    print_sint
    call    newline

    ; ---- mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) ----
    xor     edi, edi                         ; addr = 0
    mov     esi, 4096                        ; len
    mov     edx, 3                           ; PROT_READ|PROT_WRITE
    mov     r10d, 0x22                       ; MAP_PRIVATE|MAP_ANONYMOUS
    mov     r8, -1
    xor     r9d, r9d
    mov     eax, NR_MMAP
    syscall
    test    rax, rax
    js      .mmap_failed
    mov     [page], rax

    lea     rsi, [m_mmap]
    mov     edx, m_mmap_len
    call    write_stdout
    mov     rax, [page]
    call    print_hex
    call    newline

    ; 父进程往这块页写 0xAAAA...，往 .bss 的 marker 写 0x1111
    mov     rax, [page]
    mov     rcx, 0xAAAAAAAAAAAAAAAA
    mov     [rax], rcx
    mov     qword [marker], 0x1111

    ; ---- fork #1：子进程 execve("/hello.elf") ----
    mov     eax, NR_FORK
    syscall
    test    rax, rax
    js      .fork_failed
    jz      .child1

    ; 父进程：记子 pid、等它结束
    mov     [child1], eax
    lea     rsi, [m_fork1]
    mov     edx, m_fork1_len
    call    write_stdout
    mov     eax, [child1]
    call    print_sint
    call    newline

    mov     edi, -1                          ; wait4(-1, &status, 0, NULL)
    lea     rsi, [status]
    xor     edx, edx
    xor     r10d, r10d
    mov     eax, NR_WAIT4
    syscall
    call    print_wait4

    ; ---- 隔离证据：父进程那块页与 marker 仍是自己写的值 ----
    lea     rsi, [m_parent_va]
    mov     edx, m_parent_va_len
    call    write_stdout
    mov     rax, [page]
    call    print_hex
    lea     rsi, [m_value]
    mov     edx, m_value_len
    call    write_stdout
    mov     rax, [page]
    mov     rax, [rax]
    call    print_hex
    lea     rsi, [m_marker]
    mov     edx, m_marker_len
    call    write_stdout
    mov     rax, [marker]
    call    print_hex
    call    newline

    ; ---- fork #2：子进程 execve 自己（argv[1]=child2）-> exit(7) ----
    mov     eax, NR_FORK
    syscall
    test    rax, rax
    js      .fork_failed
    jz      .child2

    mov     [child2], eax
    lea     rsi, [m_fork2]
    mov     edx, m_fork2_len
    call    write_stdout
    mov     eax, [child2]
    call    print_sint
    call    newline

    mov     edi, -1
    lea     rsi, [status]
    xor     edx, edx
    xor     r10d, r10d
    mov     eax, NR_WAIT4
    syscall
    call    print_wait4

    ; ---- fork #3：子进程 execve 自己（argv[1]=killme）-> 父进程 kill(SIGTERM) ----
    mov     eax, NR_FORK
    syscall
    test    rax, rax
    js      .fork_failed
    jz      .child3

    mov     [child3], eax
    lea     rsi, [m_fork3]
    mov     edx, m_fork3_len
    call    write_stdout
    mov     eax, [child3]
    call    print_sint
    call    newline

    ; 给子进程一点时间真正跑起来（进 ring3、开始 nanosleep），再杀它
    call    sleep_80ms

    lea     rsi, [m_killing]
    mov     edx, m_killing_len
    call    write_stdout
    mov     edi, [child3]
    mov     esi, 15                          ; SIGTERM
    mov     eax, NR_KILL
    syscall
    lea     rsi, [m_kill_rc]
    mov     edx, m_kill_rc_len
    call    write_stdout
    call    print_sint
    call    newline

    mov     edi, -1
    lea     rsi, [status]
    xor     edx, edx
    xor     r10d, r10d
    mov     eax, NR_WAIT4
    syscall
    call    print_wait4

    lea     rsi, [m_done]
    mov     edx, m_done_len
    call    write_stdout
    xor     edi, edi
    mov     eax, NR_EXIT
    syscall
    jmp     hang

; ---- 子进程 1：写自己的值 -> 读回 -> execve("/hello.elf") ----
.child1:
    lea     rsi, [m_child1_pid]
    mov     edx, m_child1_pid_len
    call    write_stdout
    mov     eax, NR_GETPID
    syscall
    call    print_sint
    lea     rsi, [m_ppid]
    mov     edx, m_ppid_len
    call    write_stdout
    mov     eax, NR_GETPPID
    syscall
    call    print_sint
    call    newline

    ; 往**同一个 VA**写自己的值（父进程那块页的私有副本）
    mov     rax, [page]
    mov     rcx, 0xBBBBBBBBBBBBBBBB
    mov     [rax], rcx
    mov     qword [marker], 0x2222

    lea     rsi, [m_child1_va]
    mov     edx, m_child1_va_len
    call    write_stdout
    mov     rax, [page]
    call    print_hex
    lea     rsi, [m_value]
    mov     edx, m_value_len
    call    write_stdout
    mov     rax, [page]
    mov     rax, [rax]
    call    print_hex
    lea     rsi, [m_marker]
    mov     edx, m_marker_len
    call    write_stdout
    mov     rax, [marker]
    call    print_hex
    call    newline

    ; execve("/hello.elf", argv={"/hello.elf", NULL}, NULL)
    lea     rdi, [m_hello_path]
    lea     rsi, [argv_hello]
    xor     edx, edx
    mov     eax, NR_EXECVE
    syscall
    ; execve 成功不返回；真回来了就是失败
    lea     rsi, [m_exec_fail]
    mov     edx, m_exec_fail_len
    call    write_stdout
    call    print_sint
    call    newline
    mov     edi, 9
    mov     eax, NR_EXIT
    syscall
    jmp     hang

; ---- 子进程 2：execve 自己（argv[1]=child2）-> exit(7) ----
.child2:
    lea     rdi, [m_self_path]
    lea     rsi, [argv_child2]
    xor     edx, edx
    mov     eax, NR_EXECVE
    syscall
    lea     rsi, [m_exec_fail]
    mov     edx, m_exec_fail_len
    call    write_stdout
    call    print_sint
    call    newline
    mov     edi, 9
    mov     eax, NR_EXIT
    syscall
    jmp     hang

; ---- 子进程 3：execve 自己（argv[1]=killme）-> 等被杀 ----
.child3:
    lea     rdi, [m_self_path]
    lea     rsi, [argv_killme]
    xor     edx, edx
    mov     eax, NR_EXECVE
    syscall
    lea     rsi, [m_exec_fail]
    mov     edx, m_exec_fail_len
    call    write_stdout
    call    print_sint
    call    newline
    mov     edi, 9
    mov     eax, NR_EXIT
    syscall
    jmp     hang

; ---- execve 起来的实例：child2 模式 ----
.mode_child2:
    lea     rsi, [m_child2_ok]
    mov     edx, m_child2_ok_len
    call    write_stdout
    mov     eax, NR_GETPID
    syscall
    call    print_sint
    call    newline
    mov     edi, 7                           ; 退出码 7（父进程 wait4 应看到 status=1792）
    mov     eax, NR_EXIT
    syscall
    jmp     hang

; ---- execve 起来的实例：killme 模式（一直睡，等父进程 kill）----
.mode_killme:
    lea     rsi, [m_child3_ok]
    mov     edx, m_child3_ok_len
    call    write_stdout
    mov     eax, NR_GETPID
    syscall
    call    print_sint
    call    newline
.loop:
    lea     rdi, [ts_200ms]
    xor     esi, esi
    mov     eax, NR_NANOSLEEP
    syscall
    jmp     .loop

.mmap_failed:
    lea     rsi, [m_mmap_fail]
    mov     edx, m_mmap_fail_len
    call    write_stdout
    call    print_sint
    call    newline
    mov     edi, 3
    mov     eax, NR_EXIT
    syscall
    jmp     hang

.fork_failed:
    lea     rsi, [m_fork_fail]
    mov     edx, m_fork_fail_len
    call    write_stdout
    call    print_sint
    call    newline
    mov     edi, 4
    mov     eax, NR_EXIT
    syscall
    jmp     hang

hang:
    jmp     hang

; ---------------------------------------------------------------------------
; 工具：write_stdout(rsi = buf, rdx = len)
; ---------------------------------------------------------------------------
write_stdout:
    mov     eax, NR_WRITE
    mov     edi, 1
    syscall
    ret

newline:
    lea     rsi, [m_nl]
    mov     edx, 1
    jmp     write_stdout

; print_wait4(rax = wait4 返回值, 打印 "proc64: wait4 pid=<n> status=<n>")
print_wait4:
    mov     [w4_rc], eax
    lea     rsi, [m_wait4]
    mov     edx, m_wait4_len
    call    write_stdout
    mov     eax, [w4_rc]
    call    print_sint
    lea     rsi, [m_status]
    mov     edx, m_status_len
    call    write_stdout
    mov     eax, [status]
    call    print_sint
    call    newline
    ret

; sleep_80ms：nanosleep(80ms)（链表式的 200ms 结构在下面的 .rodata 里）
sleep_80ms:
    lea     rdi, [ts_80ms]
    xor     esi, esi
    mov     eax, NR_NANOSLEEP
    syscall
    ret

; ---------------------------------------------------------------------------
; print_sint(rax)：带符号十进制（负 errno 打印成 -N）
; ---------------------------------------------------------------------------
print_sint:
    test    rax, rax
    jns     print_dec
    push    rax
    lea     rsi, [m_minus]
    mov     edx, 1
    call    write_stdout
    pop     rax
    neg     rax
    jmp     print_dec

; print_dec(rax)：无符号十进制
print_dec:
    sub     rsp, 48
    lea     r8, [rsp + 32]
    mov     rcx, 10
.loop:
    xor     edx, edx
    div     rcx
    add     dl, '0'
    dec     r8
    mov     [r8], dl
    test    rax, rax
    jnz     .loop
    mov     rsi, r8
    lea     rdx, [rsp + 32]
    sub     rdx, rsi
    mov     eax, NR_WRITE
    mov     edi, 1
    syscall
    add     rsp, 48
    ret

; print_hex(rax)：0x + 16 位十六进制
print_hex:
    sub     rsp, 32
    lea     r8, [rsp + 18]
    mov     rcx, rax
    mov     r9d, 16
.loop:
    dec     r8
    mov     rax, rcx
    and     rax, 0xF
    cmp     al, 10
    jb      .digit
    add     al, 'a' - 10
    jmp     .store
.digit:
    add     al, '0'
.store:
    mov     [r8], al
    shr     rcx, 4
    dec     r9d
    jnz     .loop
    mov     byte [r8 - 2], '0'
    mov     byte [r8 - 1], 'x'
    lea     rsi, [r8 - 2]
    mov     edx, 18
    mov     eax, NR_WRITE
    mov     edi, 1
    syscall
    add     rsp, 32
    ret

; str_eq(rdi, rsi) -> eax = 1 相等 / 0 不等（两个 NUL 结尾的字符串）
str_eq:
    xor     ecx, ecx
.loop:
    mov     al, [rdi + rcx]
    mov     dl, [rsi + rcx]
    cmp     al, dl
    jne     .no
    test    al, al
    jz      .yes
    inc     ecx
    cmp     ecx, 64
    jb      .loop
.no:
    xor     eax, eax
    ret
.yes:
    mov     eax, 1
    ret

section .rodata
m_nl:            db 10
m_minus:         db "-"
m_banner:        db "proc64: fork/execve/wait4/kill demo start", 10
m_banner_len     equ $ - m_banner
m_parent_pid:    db "proc64: parent pid="
m_parent_pid_len equ $ - m_parent_pid
m_ppid:          db " ppid="
m_ppid_len       equ $ - m_ppid
m_mmap:          db "proc64: mmap page va="
m_mmap_len       equ $ - m_mmap
m_mmap_fail:     db "proc64: mmap failed rc="
m_mmap_fail_len  equ $ - m_mmap_fail
m_fork1:         db "proc64: forked child1 pid="
m_fork1_len      equ $ - m_fork1
m_fork2:         db "proc64: forked child2 pid="
m_fork2_len      equ $ - m_fork2
m_fork3:         db "proc64: forked child3 pid="
m_fork3_len      equ $ - m_fork3
m_fork_fail:     db "proc64: fork failed rc="
m_fork_fail_len  equ $ - m_fork_fail
m_child1_pid:    db "proc64: child1 pid="
m_child1_pid_len equ $ - m_child1_pid
m_child1_va:     db "proc64: child1 same-va va="
m_child1_va_len  equ $ - m_child1_va
m_parent_va:     db "proc64: parent same-va va="
m_parent_va_len  equ $ - m_parent_va
m_value:         db " value="
m_value_len      equ $ - m_value
m_marker:        db " marker="
m_marker_len     equ $ - m_marker
m_wait4:         db "proc64: wait4 pid="
m_wait4_len      equ $ - m_wait4
m_status:        db " status="
m_status_len     equ $ - m_status
m_killing:       db "proc64: killing child3 with SIGTERM", 10
m_killing_len    equ $ - m_killing
m_kill_rc:       db "proc64: kill rc="
m_kill_rc_len    equ $ - m_kill_rc
m_exec_fail:     db "proc64: execve failed rc="
m_exec_fail_len  equ $ - m_exec_fail
m_child2_ok:     db "proc64: child2 exec ok pid="
m_child2_ok_len  equ $ - m_child2_ok
m_child3_ok:     db "proc64: child3 waiting for kill pid="
m_child3_ok_len  equ $ - m_child3_ok
m_done:          db "proc64: demo done exit(0)", 10
m_done_len       equ $ - m_done
m_hello_path:    db "/hello.elf", 0
m_self_path:     db "/proc64.elf", 0
m_child2:        db "child2", 0
m_killme:        db "killme", 0

align 8
argv_hello:      dq m_hello_path, 0
argv_child2:     dq m_self_path, m_child2, 0
argv_killme:     dq m_self_path, m_killme, 0
ts_80ms:         dq 0, 80000000            ; struct timespec { tv_sec, tv_nsec }
ts_200ms:        dq 0, 200000000

section .bss
pid:      resd 1
w4_rc:    resd 1
status:   resd 1
child1:   resd 1
child2:   resd 1
child3:   resd 1
page:     resq 1
marker:   resq 1
