; boot/efi/jump64.asm - 两个小工具：
;   efi_jump_far(entry, a0, a1, a2)  跳进平铺长模式引导器（UEFI64.BIN，链接到 8MB）
;   efi_enter_kernel(entry, cr3)     进内核前换 GDT/CR3 并跳到 0x100000
;
; 汇编：nasm -f win64（COFF）。Win64 ABI：rcx, rdx, r8, r9
bits 64
default rel

; ---------------------------------------------------------------------------
; 串口打点宏（COM1 0x3F8）。用途：UEFI 下"进内核即静默复位"（三重故障无日志）时，
; 把每一步打到串口才能把复位点夹到具体一条指令上。
;
; ★ 教训一：`out` 的端口必须在 **DX**，而 DX = **RDX 低 16 位**；RDX 在这里正是
;   **CR3 的值**（efi_enter_kernel 第二个参数）。所以打点要自己保存/恢复 RDX。
;   （踩过：直接把 RDX 低 16 位写成 0x3F8 -> mov cr3 加载未对齐垃圾 -> #GP -> 复位）
;
; ★ 教训二：等字节**真的发完**要用 LSR bit6（TEMT，移位寄存器也空），不是 bit5（THRE）。
;   bit5 只表示字节被搬进移位寄存器，115200bps 下还要 ~87µs 才上线；
;   复位发生在这段时间里，最后一个标记就丢了 —— 会得到**假的定位**。我们被骗过一次。
; ---------------------------------------------------------------------------
%macro SERMARK 1
    push rdx
    push rax
    mov dx, 0x3F8
%%w1:
    add dx, 5
    in al, dx                       ; LSR
    sub dx, 5
    test al, 0x40                   ; TEMT？
    jz %%w1
    mov al, %1
    out dx, al
%%w2:
    add dx, 5
    in al, dx
    sub dx, 5
    test al, 0x40                   ; 等这个字节也真的发完
    jz %%w2
    pop rax
    pop rdx
%endmacro

section .text
global efi_jump_far
global efi_enter_kernel

efi_jump_far:
    ; rcx = entry, rdx = a0, r8 = a1, r9 = a2
    mov rax, rcx
    mov rcx, rdx
    mov rdx, r8
    mov r8, r9
    sub rsp, 32                     ; 影子空间（Win64 ABI）
    call rax
    add rsp, 32
    cli
    hlt
    jmp $

; 进内核：rcx = 内核入口(0x100000)，rdx = 页表物理地址(CR3)
efi_enter_kernel:
    SERMARK 'a'                     ; 进入本函数
    cli
    cld
    lgdt [gdt_desc]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    SERMARK 'b'                     ; GDT/段寄存器就绪（此刻栈还是固件的）
    ; ★★ 这里**故意不切 CR3**，继续用固件留下的页表。原因（实测，别改回去）：
    ;   VMware EFI（无 VT-x、走二进制翻译 + 影子页表）对这条 `mov cr3` 的反应是**立刻复位**：
    ;     - 打点显示卡在 'b' 与 'B' 之间，即 `mov cr3, rdx` 本身；
    ;     - 传入的 CR3 = 0x806000 已 4KB 对齐（U:cr3=806000 low12=0），且表内容换表前
    ;       刚校验过（U:pt check ok），所以不是"表坏了"也不是"值不对"；
    ;     - OVMF 下同样的代码 `abcd` 全通、内核正常起来 —— 纯属 VMware EFI 的执行环境差异。
    ;   而不切表**没有副作用**：UEFI 固件本来就为整机 RAM 和 GOP 帧缓冲建了恒等映射，
    ;   内核全程按物理地址访问（页池/帧缓冲/载荷都是），所以够用。
    ;   若将来遇到"固件不恒等映射全部 RAM"的环境，需要把这一步挪进内核入口（0x100000 处）
    ;   再做，那时代码已在最终位置，避免在 8MB 处的搬运代码里切表。
    ; mov cr3, rdx                  ; ← 保留这一行作为记录：启用它会怎样，见上面的实测
    SERMARK 'B'                     ; 二分用标记：能看到 B 就说明走过了"本该换表"的位置
    mov rsp, 0x7C000                ; 换内核栈（第一次写 0x7C000 附近发生在打点里）
    xor rbp, rbp
    xor rdi, rdi
    SERMARK 'c'                     ; CR3 + 栈都就绪
    xor rsi, rsi
    ; ★ 必须用**远跳**把 CS 换成我们自己的 0x08（近跳 jmp rcx 只改 RIP！）
    ;   早先用 `jmp rcx` 时 CS 仍是固件的 0x38：内核照跑（同环、长模式），
    ;   但 PIT 中断一来，CPU 把 CS=0x38 压进中断帧，我们的桩在 iretq 要恢复它时
    ;   发现 GDT 里没有 index 7 的描述符 -> #GP(err=0x38)，表现就是"UEFI 下内核刚起来
    ;   就 PANIC cpu exception 13"（BIOS 路径走的是 loader64.asm 的远跳，所以一直正常）。
    ;   64 位模式下没有 ptr16:64 形式的远跳指令，标准做法是 push CS/RIP + retfq。
    push qword 0x08                 ; 目标 CS
    lea rax, [rel .cs_ok]
    push rax                        ; 目标 RIP（retfq 先弹 RIP 再弹 CS）
    retfq
.cs_ok:
    SERMARK 'd'                     ; 远跳成功：CS 已是 0x08
    jmp rcx                         ; 再近跳到内核入口（0x100000）

.hang:
    cli
    hlt
    jmp .hang

section .data
align 16
gdt_start:
    dq 0x0000000000000000
    dq 0x00AF9A000000FFFF            ; 0x08 64 位代码
    dq 0x00AF92000000FFFF            ; 0x10 数据
gdt_end:
gdt_desc:
    dw gdt_end - gdt_start - 1
    dq gdt_start
