; spin64.asm - 一个"一直活着"的 ELF64 用户程序（任务管理器进程页 / proc64 kill 的可验证目标）
;
; 为什么需要它：proc64 的启动期多进程演示跑完就退出了，桌面起来时进程表是空的；
;   "任务管理器进程页显示真进程并能 kill"需要一个运行期能创建的**长命**进程：
;   终端 `proc run spin` 会用 proc64_create64 + proc64_start_elf64 把它作为**真正的进程**
;   （独立 CR3 + 独立任务）跑起来；本程序打印 pid 后每 1 秒 nanosleep 一次，永不退出，
;   直到被 kill（SIGKILL/SIGTERM）。
;
; 用到的系统调用（Linux x86_64 ABI，见 kernel/syscall64.cpp 的映射表）：
;   1 write / 35 nanosleep / 39 getpid
;
; 构建（见 build64.sh）：nasm -f elf64 -> ld.lld -T user/hello_elf64.ld -> objcopy 嵌入系统内核；
;   链接地址必须落在用户窗口（4GiB 起、低于 USER64_STACK_VA64），与 hello/proc64 同一脚本。
;
; 串口/屏幕输出（tests/tmgr_proc_test.py 会 grep）：
;   spin64: alive pid=<n>   （之后每 1000ms 一次静默循环，不再打印）
bits 64
default rel

%define NR_WRITE     1
%define NR_GETPID    39
%define NR_NANOSLEEP 35

global _start

section .text
_start:
    ; ---- write(1, "spin64: alive pid=", ...) ----
    lea     rsi, [m_alive]
    mov     edx, m_alive_len
    call    write_stdout

    ; ---- getpid -> 十进制 ----
    mov     eax, NR_GETPID
    syscall
    call    print_dec
    call    newline

    ; ---- 初始化 timespec：tv_sec = 1, tv_nsec = 0 ----
    ; ★ 必须显式写：.bss 里是 0，而 nanosleep(0) 会**立刻返回** —— 那就变成 ring3 里疯狂
    ;   发 syscall 的忙循环（还刷满串口日志），不是"长命睡眠进程"。
    mov     qword [ts], 1
    mov     qword [ts + 8], 0

.loop:
    ; ---- nanosleep(&ts, NULL)：睡 1 秒（被 kill 前一直循环）----
    lea     rdi, [ts]
    xor     esi, esi
    mov     eax, NR_NANOSLEEP
    syscall
    jmp     .loop                            ; 永不退出：等被 kill

; ---------------------------------------------------------------------------
write_stdout:                                ; rsi = buf, rdx = len
    mov     eax, NR_WRITE
    mov     edi, 1
    syscall
    ret

newline:
    lea     rsi, [m_nl]
    mov     edx, 1
    jmp     write_stdout

print_dec:                                   ; rax -> stdout（无符号）
    sub     rsp, 32
    lea     r8, [rsp + 24]
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
    lea     rdx, [rsp + 24]
    sub     rdx, rsi
    mov     eax, NR_WRITE
    mov     edi, 1
    syscall
    add     rsp, 32
    ret

section .rodata
m_alive:     db "spin64: alive pid="
m_alive_len  equ $ - m_alive
m_nl:        db 10

section .bss
ts:     resq 2                              ; struct timespec
