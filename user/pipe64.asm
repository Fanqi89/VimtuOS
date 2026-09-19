; pipe64.asm - ring3 管道演示：fork 后父子各持一端通信（批次 D）
;
; 号段（Linux x86_64，见 kernel/syscall64.cpp 的文件头大表）：
;   pipe(22) / read(0) / write(1) / close(3) / nanosleep(35) / fork(57) / wait4(61) / exit(60)
;
; 流程（内核侧 pid=.. 的打点在 kernel/proc64.cpp 的 pipe-demo 段）：
;   1) pipe(fds)：拿到读端/写端两个 fd（64 B 环形缓冲，非阻塞语义）；
;   2) fork()：
;        * 子进程：关掉读端 -> 往写端写 "PIPE-OK-FROM-CHILD" -> 打印写入字节数 -> exit(0)；
;        * 父进程：关掉写端 -> 轮询读读端（管道是**非阻塞**的：读空且写端开着返回 -EAGAIN，
;                  所以要 nanosleep 重试；写端全关后读返回 0 = EOF）-> 打印读到的原文
;                  -> wait4 收子进程 -> exit(0)。
;   验证据此有三层：用户态两行打印 + 内核 [FD64] pipe read n=.. data=.. + [PROC64] pipe-demo done。
;
; 构建（见 build64.sh）：nasm -f elf64 -> ld.lld -T user/hello_elf64.ld -> objcopy 嵌入系统内核，
; 运行时由 kernel/proc64.cpp 幂等装成 /pipe64.elf，再由 proc64_pipe_demo64() 当成一个真进程跑。
bits 64
default rel

%define NR_READ       0
%define NR_WRITE      1
%define NR_CLOSE      3
%define NR_PIPE       22
%define NR_NANOSLEEP  35
%define NR_FORK       57
%define NR_EXIT       60
%define NR_WAIT4      61

%define NR_GETPID     39

global _start

section .text
_start:
    ; ---- pipe(fds) ----
    lea     rdi, [fds]
    mov     eax, NR_PIPE
    syscall
    test    eax, eax
    js      .fail

    lea     rsi, [m_pipe]
    mov     edx, m_pipe_len
    call    write_stdout
    mov     eax, [fds]
    call    print_sint
    lea     rsi, [m_w]
    mov     edx, m_w_len
    call    write_stdout
    mov     eax, [fds + 4]
    call    print_sint
    call    newline

    ; ---- fork() ----
    mov     eax, NR_FORK
    syscall
    test    rax, rax
    js      .fail
    jz      .child

; ==================== 父进程：读端 ====================
.parent:
    lea     rsi, [m_parent]
    mov     edx, m_parent_len
    call    write_stdout
    mov     eax, NR_GETPID
    syscall
    call    print_sint
    call    newline

    ; 关掉自己的写端（子进程还持有一份，所以 writers 不会归零）
    mov     edi, [fds + 4]
    mov     eax, NR_CLOSE
    syscall

    mov     dword [tries], 0
.retry:
    mov     edi, [fds]                      ; 读端
    lea     rsi, [buf]
    mov     edx, 64
    mov     eax, NR_READ
    syscall
    test    eax, eax
    jg      .got                            ; > 0：收到数据
    jz      .eof                            ; 0：写端全关（EOF）
    ; 负值（-EAGAIN = -11）：管道非阻塞，睡 10ms 再试（上限 500 次 = 5 秒，防挂死）
    inc     dword [tries]
    cmp     dword [tries], 500
    ja      .fail
    lea     rdi, [ts10ms]
    xor     esi, esi
    mov     eax, NR_NANOSLEEP
    syscall
    jmp     .retry

.got:
    mov     [nread], eax
    lea     rsi, [m_read]
    mov     edx, m_read_len
    call    write_stdout
    mov     eax, [nread]
    call    print_sint
    lea     rsi, [m_data]
    mov     edx, m_data_len
    call    write_stdout
    mov     edx, [nread]                    ; 原文照打（收到的就是子进程写的字节）
    lea     rsi, [buf]
    mov     eax, NR_WRITE
    mov     edi, 1
    syscall
    call    newline
    jmp     .reap

.eof:
    lea     rsi, [m_eof]
    mov     edx, m_eof_len
    call    write_stdout

.reap:
    ; wait4(-1, &status, 0, NULL)：收子进程（非阻塞轮询由内核侧完成）
    mov     edi, -1
    lea     rsi, [status]
    xor     edx, edx
    xor     r10d, r10d
    mov     eax, NR_WAIT4
    syscall
    lea     rsi, [m_wait]
    mov     edx, m_wait_len
    call    write_stdout
    call    print_sint
    call    newline

    mov     edi, [fds]                      ; 关读端
    mov     eax, NR_CLOSE
    syscall

    lea     rsi, [m_done]
    mov     edx, m_done_len
    call    write_stdout
    xor     edi, edi
    mov     eax, NR_EXIT
    syscall
    jmp     hang

; ==================== 子进程：写端 ====================
.child:
    lea     rsi, [m_child]
    mov     edx, m_child_len
    call    write_stdout
    mov     eax, NR_GETPID
    syscall
    call    print_sint
    call    newline

    mov     edi, [fds]                      ; 关掉读端（自己只写）
    mov     eax, NR_CLOSE
    syscall

    mov     edi, [fds + 4]                  ; 写端
    lea     rsi, [m_msg]
    mov     edx, m_msg_len
    mov     eax, NR_WRITE
    syscall
    mov     [nwrote], eax

    lea     rsi, [m_wrote]
    mov     edx, m_wrote_len
    call    write_stdout
    mov     eax, [nwrote]
    call    print_sint
    call    newline

    xor     edi, edi
    mov     eax, NR_EXIT
    syscall
    jmp     hang

.fail:
    lea     rsi, [m_failed]
    mov     edx, m_failed_len
    call    write_stdout
    mov     edi, 3
    mov     eax, NR_EXIT
    syscall

hang:
    jmp     hang

; ---------------------------------------------------------------------------
; write_stdout(rsi = buf, rdx = len)
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

section .rodata
m_pipe:       db "pipe64: pipe r="
m_pipe_len    equ $ - m_pipe
m_w:          db " w="
m_w_len       equ $ - m_w
m_parent:     db "pipe64: parent pid="
m_parent_len  equ $ - m_parent
m_child:      db "pipe64: child pid="
m_child_len   equ $ - m_child
m_wrote:      db "pipe64: child wrote n="
m_wrote_len   equ $ - m_wrote
m_read:       db "pipe64: parent read n="
m_read_len    equ $ - m_read
m_data:       db " data="
m_data_len    equ $ - m_data
m_eof:        db "pipe64: EOF (writer closed, nothing received)", 10
m_eof_len     equ $ - m_eof
m_wait:       db "pipe64: wait4 rc="
m_wait_len    equ $ - m_wait
m_done:       db "pipe64: demo done exit(0)", 10
m_done_len    equ $ - m_done
m_failed:     db "pipe64: FAILED", 10
m_failed_len  equ $ - m_failed
m_msg:        db "PIPE-OK-FROM-CHILD"
m_msg_len     equ $ - m_msg
m_minus:      db "-"
m_nl:         db 10
; nanosleep 的 timespec {tv_sec=0, tv_nsec=10000000} = 10ms（非阻塞管道要轮询）
ts10ms:       dq 0, 10000000

section .bss
fds:     resd 2
nread:   resd 1
nwrote:  resd 1
tries:   resd 1
status:  resd 1
buf:     resb 64
