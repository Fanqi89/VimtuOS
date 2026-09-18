; hello_elf64.asm - Vimtu64 上第一个**真正的 ELF64 可执行程序**（自有，不依赖 libc）
;
; 与 hello64.asm（VAP64 平铺二进制，用 int 0x80）的区别：
;   * 这是 ELF64 可执行文件（ET_EXEC，ld.lld 静态链接），由 kernel/elf64.cpp 加载；
;   * 入口是 _start，用 **syscall 指令**与内核通信（Linux x86_64 ABI：rax=号，
;     rdi/rsi/rdx/r10 = 参数，返回 rax；rcx/r11 被 CPU 覆盖）；
;   * 用到的号段：1 write / 39 getpid / 257 openat / 5 fstat / 0 read / 3 close /
;     228 clock_gettime / 60 exit（全部在 kernel/syscall64.cpp 的映射表里）。
;
; 构建（见 build64.sh）：
;   nasm -f elf64 user/hello_elf64.asm -o build64/hello_elf64.o
;   ld.lld -nostdlib -static -e _start -Ttext=0x0000000100020000 -o build64/hello.elf build64/hello_elf64.o
;   objcopy -I binary -> 嵌进系统内核（安装程序内核不需要）
; ★ 链接地址必须落在**用户窗口**（4GiB..4GiB+1MiB）且低于 USER64_STACK_VA64（4GiB+64KiB）：
;   elf64.cpp 会把"段越界"当作拒绝理由（[ELF64] reject segment va=... reason=outside-user-window）。
;
; 输出（自动验收 tests/elf64_test.py 会 grep）：
;   hello from ELF64 (syscall insn)
;   pid=<n> / open /hello.elf fd=<n> / read=<n> / sec=<n> / done (exit 60)
bits 64
default rel

%define NR_READ           0
%define NR_WRITE          1
%define NR_CLOSE          3
%define NR_FSTAT          5
%define NR_GETPID         39
%define NR_EXIT           60
%define NR_CLOCK_GETTIME  228
%define NR_OPENAT         257

global _start

section .text
_start:
    ; ---- write(1, "hello from ELF64 (syscall insn)\n", 32) ----
    mov     eax, NR_WRITE
    mov     edi, 1
    lea     rsi, [m_hello]
    mov     edx, m_hello_len
    syscall                                  ; rax = 32（写出的字节数）

    ; ---- write("pid="); write(十进制(getpid())) ----
    lea     rsi, [m_pid]
    mov     edx, m_pid_len
    call    write_stdout
    mov     eax, NR_GETPID
    syscall
    call    print_dec
    call    newline

    ; ---- openat(AT_FDCWD, "/hello.elf", O_RDONLY, 0) ：把自己的映像从 VFS 打开 ----
    mov     rdi, -100                        ; AT_FDCWD
    lea     rsi, [m_self]
    xor     edx, edx                         ; O_RDONLY
    xor     r10d, r10d                       ; mode
    mov     eax, NR_OPENAT
    syscall
    mov     [fd], eax

    lea     rsi, [m_open]
    mov     edx, m_open_len
    call    write_stdout
    mov     eax, [fd]
    call    print_sint
    call    newline

    mov     eax, [fd]
    test    eax, eax
    js      .no_file                         ; 打开失败就跳过这段（不是致命错误）

    ; ---- fstat(fd, &st)：只证明"能填 struct stat"（不解析内容）----
    mov     edi, [fd]
    lea     rsi, [st]
    mov     eax, NR_FSTAT
    syscall

    ; ---- read(fd, buf, 16)：读文件开头 16 字节 ----
    mov     edi, [fd]
    lea     rsi, [buf]
    mov     edx, 16
    mov     eax, NR_READ
    syscall
    mov     [nread], eax
    lea     rsi, [m_read]
    mov     edx, m_read_len
    call    write_stdout
    mov     eax, [nread]
    call    print_sint
    call    newline

    ; ---- close(fd) ----
    mov     edi, [fd]
    mov     eax, NR_CLOSE
    syscall

.no_file:
    ; ---- clock_gettime(CLOCK_MONOTONIC=1, &ts)：tv_sec 来自内核 PIT tick ----
    mov     edi, 1
    lea     rsi, [ts]
    mov     eax, NR_CLOCK_GETTIME
    syscall
    lea     rsi, [m_sec]
    mov     edx, m_sec_len
    call    write_stdout
    mov     eax, [ts]                        ; tv_sec（低 32 位足够演示）
    call    print_dec
    call    newline

    lea     rsi, [m_done]
    mov     edx, m_done_len
    call    write_stdout

    ; ---- exit(0)：内核把控制权交回 ring0（syscall 入口的出口 B）----
    xor     edi, edi
    mov     eax, NR_EXIT
    syscall

    ; exit 正常不返回；真回来了就原地停住
.hang:
    jmp     .hang

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
; print_dec(rax)：无符号十进制 -> stdout（缓冲区在栈上，用户栈可写）
; ---------------------------------------------------------------------------
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

; ---------------------------------------------------------------------------
; print_sint(rax)：带符号十进制（负 errno 会打印成 -N，便于日志直读）
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

; ---- 只读数据（.rodata，PF_R|PF_X 或 PF_R 段）----
section .rodata
m_hello:     db "hello from ELF64 (syscall insn)", 10
m_hello_len  equ $ - m_hello
m_pid:       db "pid="
m_pid_len    equ $ - m_pid
m_open:      db "open /hello.elf fd="
m_open_len   equ $ - m_open
m_read:      db "read="
m_read_len   equ $ - m_read
m_sec:       db "sec="
m_sec_len    equ $ - m_sec
m_done:      db "done (exit 60)", 10
m_done_len   equ $ - m_done
m_self:      db "/hello.elf", 0
m_minus:     db "-"
m_nl:        db 10

; ---- 可写数据：.bss 会变成"p_memsz > p_filesz"的段，正好验证加载器的清零 ----
section .bss
fd:     resd 1
nread:  resd 1
st:     resb 144        ; struct stat（x86_64 = 144 字节）
buf:    resb 16
ts:     resq 2          ; struct timespec
