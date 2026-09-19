; filedemo64.asm - ring3 文件演示：打开 /t.txt、读一行、打印出来（真 VimtuFS2，不再是 ramfs）
;
; 为什么需要它：批次 B 把 syscall 的 open(2)/read(0)/close(3) 接到了 kernel/fd64.cpp 的 FD 层
;   （底层 vfs64 -> VimtuFS2）。这个程序是终端之外的**第二类用户**：ring3 程序走同一条 FD 层，
;   证明"文件不只是内核/终端能碰"。它用 Linux x86_64 ABI（syscall 指令）：
;     open(2) / read(0) / write(1) / close(3) / exit(60)
;
; 用法（终端里）：先 `write /t.txt hello`，再 `run filedemo`（开终端时会幂等安装 /filedemo.elf）。
; 输出（自动验收 tests/fs_term_test.py grep）：
;   filedemo: open /t.txt fd=<n>
;   filedemo: read=<n> content=<文件内容>
;   filedemo: done (exit 0)
; 内核侧同一时刻会打 [FD64] open path=/t.txt fd=<n> / [FD64] read fd=<n> n=<n> / [FD64] close fd=<n>。
;
; 构建（见 build64.sh）：nasm -f elf64 -> ld.lld -T user/hello_elf64.ld -> objcopy 嵌入系统内核。
; ★ 链接地址必须落在用户窗口（4GiB..4GiB+64KiB），与 hello.elf / spin.elf 同一份链接脚本。
bits 64
default rel

%define NR_READ   0
%define NR_WRITE  1
%define NR_CLOSE  3
%define NR_EXIT   60
%define NR_OPEN   2

global _start

section .text
_start:
    ; ---- open("/t.txt", O_RDONLY=0, mode=0) ----
    lea     rdi, [m_path]
    xor     esi, esi
    xor     edx, edx
    mov     eax, NR_OPEN
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
    js      .no_file                         ; 打开失败（文件不在 / 没卷）：如实打印并退出

    ; ---- read(fd, buf, 128) ----
    mov     edi, [fd]
    lea     rsi, [buf]
    mov     edx, 128
    mov     eax, NR_READ
    syscall
    mov     [nread], eax

    lea     rsi, [m_read]
    mov     edx, m_read_len
    call    write_stdout
    mov     eax, [nread]
    call    print_sint

    lea     rsi, [m_content]
    mov     edx, m_content_len
    call    write_stdout
    mov     eax, [nread]
    test    eax, eax
    jle     .after_read
    mov     edx, eax                         ; 打印读到的原文
    lea     rsi, [buf]
    mov     eax, NR_WRITE
    mov     edi, 1
    syscall
.after_read:
    call    newline

    ; ---- close(fd) ----
    mov     edi, [fd]
    mov     eax, NR_CLOSE
    syscall

.no_file:
    lea     rsi, [m_done]
    mov     edx, m_done_len
    call    write_stdout

    xor     edi, edi
    mov     eax, NR_EXIT
    syscall

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
m_path:       db "/t.txt", 0
m_open:       db "filedemo: open /t.txt fd="
m_open_len    equ $ - m_open
m_read:       db "filedemo: read="
m_read_len    equ $ - m_read
m_content:    db " content="
m_content_len equ $ - m_content
m_done:       db "filedemo: done (exit 0)", 10
m_done_len    equ $ - m_done
m_minus:      db "-"
m_nl:         db 10

section .bss
fd:     resd 1
nread:  resd 1
buf:    resb 128
