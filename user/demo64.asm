; demo64.asm - VimtuOS 用户态演示程序（真实 ring3 代码）
;
; 构建方式（见 build64.sh）：nasm -f bin -> build64/user_demo64.bin -> objcopy 成
; elf64 目标文件嵌进内核，再由 kernel/usermode64.cpp 拷到用户代码页（VA 4GiB）执行。
; 平铺二进制、org 0：代码里只用 RIP 相对寻址和栈，不依赖加载地址。
;
; 与内核的接口 = int 0x80（自有 ABI，见 kernel/syscall64.h）：
;   rax = 调用号，rdi/rsi/rdx = 参数，返回值在 rax
;   1 write(fd=1, buf, len)   2 exit(code)   3 getpid()   4 ticks()
;
; 输出（自动验收 tests/user64_test.py 会 grep）：
;   hello from ring3 (VimtuOS user mode)
;   pid=<n>
;   ticks=<n>
bits 64
org 0

%define NR_WRITE  1
%define NR_EXIT   2
%define NR_GETPID 3
%define NR_TICKS  4

_start:
    ; ---- write(1, "hello from ring3 (VimtuOS user mode)\n", len) ----
    mov     eax, NR_WRITE
    mov     edi, 1
    lea     rsi, [rel msg_hello]
    mov     edx, msg_hello_len
    int     0x80

    ; ---- write(1, "pid=", 4); write(1, 十进制(getpid()), ..) ----
    lea     rsi, [rel msg_pid]
    mov     edx, msg_pid_len
    call    write_stdout

    mov     eax, NR_GETPID
    int     0x80
    call    print_dec
    call    newline

    ; ---- write(1, "ticks=", 6); write(1, 十进制(ticks()), ..) ----
    lea     rsi, [rel msg_ticks]
    mov     edx, msg_ticks_len
    call    write_stdout

    mov     eax, NR_TICKS
    int     0x80
    call    print_dec
    call    newline

    ; ---- exit(0)：内核会把控制权交回 ring0（见 usermode64.cpp）----
    mov     eax, NR_EXIT
    xor     edi, edi
    int     0x80

    ; exit 正常不返回；真回来了就原地停住（不该发生）
.hang:
    jmp     .hang

; ---------------------------------------------------------------------------
; write(1, rsi, rdx)：辅助子程序（调用方已把 buf/len 放进 rsi/rdx）
; ---------------------------------------------------------------------------
write_stdout:
    mov     eax, NR_WRITE
    mov     edi, 1
    int     0x80
    ret

newline:
    lea     rsi, [rel msg_nl]
    mov     edx, 1
    jmp     write_stdout

; ---------------------------------------------------------------------------
; print_dec(rax)：把 rax 以十进制写到 stdout
;   注意：缓冲区必须在**可写的用户栈**上 —— 代码页是只读映射（PTE 不带 W），
;   往 .data/numbuf 写会 #PF。这里 sub rsp 用栈上的 32 字节。
; ---------------------------------------------------------------------------
print_dec:
    sub     rsp, 48
    lea     r8, [rsp + 32]              ; r8 = 缓冲区末尾（写指针向前走）
    mov     rcx, 10
.loop:
    xor     edx, edx
    div     rcx                         ; rax = rax/10，rdx = 余数
    add     dl, '0'
    dec     r8
    mov     [r8], dl
    test    rax, rax
    jnz     .loop
    mov     rsi, r8
    lea     rdx, [rsp + 32]
    sub     rdx, rsi                    ; rdx = 数字长度
    mov     eax, NR_WRITE
    mov     edi, 1
    int     0x80
    add     rsp, 48
    ret

; ---- 只读数据（与代码同页，因此只可读可执行、不可写）----
msg_hello:      db "hello from ring3 (VimtuOS user mode)", 10
msg_hello_len   equ $ - msg_hello
msg_pid:        db "pid="
msg_pid_len     equ $ - msg_pid
msg_ticks:      db "ticks="
msg_ticks_len   equ $ - msg_ticks
msg_nl:         db 10
