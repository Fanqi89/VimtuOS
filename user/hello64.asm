; hello64.asm - Vimtu64 "可安装应用"示例（VAP64 包里的代码段，真实 ring3）
;
; 构建方式（见 build64.sh）：
;   nasm -f bin user/hello64.asm -o build64/hello64.bin
;   python tools/make_vap.py build64/hello64.bin build64/hello.vap hello   ; 加 VAP64 头
;   objcopy -I binary -O elf64-x86-64 build64/hello.vap build64/hello_vap64.o  ; 嵌进内核
; 启动后由 kernel/app64.cpp 从 VimtuFS2 读出来 -> 校验 VAP64 头 -> user64_run_blob64 进 ring3。
;
; 平铺二进制、org 0：只用 RIP 相对寻址和栈，不依赖加载地址（代码被拷到用户代码页 4GiB）。
; 与内核的接口 = int 0x80（自有 ABI，见 kernel/syscall64.h）：
;   rax = 调用号，rdi/rsi/rdx = 参数，返回值在 rax
;   1 write(fd=1, buf, len)   2 exit(code)   4 ticks()
;
; 输出（自动验收 tests/app64_test.py 会 grep）：
;   hello from installed app (VAP64 on VimtuFS2)
;   ticks=<n>
bits 64
org 0

%define NR_WRITE 1
%define NR_EXIT  2
%define NR_TICKS 4

_start:
    ; ---- write(1, "hello from installed app (VAP64 on VimtuFS2)\n", len) ----
    lea     rsi, [rel msg_hello]
    mov     edx, msg_hello_len
    call    write_stdout

    ; ---- write(1, "ticks=", 6); write(1, 十进制(ticks()), ..) ----
    lea     rsi, [rel msg_ticks]
    mov     edx, msg_ticks_len
    call    write_stdout

    mov     eax, NR_TICKS
    int     0x80
    call    print_dec
    call    newline

    ; ---- exit(0)：内核把控制权交回 ring0（见 usermode64.cpp）----
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
;   缓冲区在**可写的用户栈**上（代码页是只读映射），十进制转换完全在栈上完成。
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
msg_hello:      db "hello from installed app (VAP64 on VimtuFS2)", 10
msg_hello_len   equ $ - msg_hello
msg_ticks:      db "ticks="
msg_ticks_len   equ $ - msg_ticks
msg_nl:         db 10
