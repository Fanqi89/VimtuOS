; ============================================================================
; ap_trampoline64.asm - SMP：AP 的 16 位实模式跳板 + 64 位引导桩（平铺二进制）
; ============================================================================
; 由 BSP（kernel/smp64.cpp）拷到**物理 0x8000**，然后对每个 AP 发 INIT-SIPI-SIPI
; （SIPI 向量 = 0x08 -> CS=0x0800、IP=0 -> 物理 0x8000）把 AP 从这里拉起来。
;
; ★ 为什么是 0x8000（依据，见 kernel/memlayout64.h 与 boot/loader64.asm 的布局）：
;     0x0F00 介质描述符 / 0x1000 BootInfo / 0x2000 E820 / 0x7000 VBE /
;     0x7400 模式表 / 0x7600 EDID / 0x7800 RSDP 槽 / 0x7BFF loader 栈顶 /
;     0x9000 loader 自身 / 0x40000..0x48000 页表 / 0x100000 内核镜像。
;   => 0x8000..0x8FFF 这一页是唯一既**页对齐**、又在 64KB 实模式寻址范围内、
;      且不与上述任何既有布局冲突的低端页。boot/loader64.asm 注释里的
;      "SeaBIOS trampoline 区 0x8000-0x9000" 是 **BIOS POST 期间**的事：
;      内核已经在跑（os_boot_path）时 SeaBIOS 早已不再使用这一页 —— 这也正是
;      Linux 等系统放 AP 蹦床的经典地址。
;
; ★ 位置无关？**不是**。全部绝对引用都按 [org 0x8000] 汇编（含 GDTR 描述符、
;   共享块偏移、GDT 拷贝目标 0x8C00）。BSP 必须原字节拷到 0x8000；C++ 侧
;   （kernel/smp64.h 的 SMP64_* 常量与 smp64_selftest64 的"逐字节一致"检查）
;   保证这个约定不被悄悄破坏。
;
; 页内布局（偏移 = 相对 0x8000；必须与 kernel/smp64.h 的常量一致）：
;     0x000..   16 位实模式代码（含最后的 [bits 32]/[bits 64] 桩）
;     0x040     GDTR 描述符（6 字节：limit=63，base=0x8C00）—— 自检会读回核对
;     0x060     32 位保护模式段（smp64_pm32）
;     0x080     64 位段（smp64_lm64）
;     0x800     共享数据块（CR3/CR0/CR4/EFER/入口 RIP/本 AP 栈顶/GDT/GDTR/IDTR…）
;     0xC00     临时 GDT 拷贝目标（AP **自己**从共享块拷过来，见下面 rep movsb）
;     0xE00..   实模式栈（SS=0，SP=0x8F00，向下生长）
; ============================================================================
%define SMP64_TRAMPOLINE_PHYS   0x8000      ; SMP64_TRAMPOLINE_PHYS
%define SMP64_OFF_GDTR          0x40        ; SMP64_OFF_GDTR
%define SMP64_OFF_PM32          0x60        ; SMP64_OFF_PM32
%define SMP64_OFF_LM64          0xC0        ; SMP64_OFF_LM64
%define SMP64_OFF_SHARED        0x800       ; SMP64_OFF_SHARED
%define SMP64_OFF_GDT_COPY      0xC00       ; SMP64_OFF_GDT_COPY
%define SMP64_RM_STACK_TOP      0x8F00      ; SMP64_RM_STACK_TOP

; 共享块内偏移（相对 SMP64_OFF_SHARED；与 smp64.h 的 SMP64_S_* 一致）
%define S_MAGIC      0x00      ; u32 'SMPT'
%define S_INDEX      0x04      ; u32 AP 序号（1 起）
%define S_APICID     0x08      ; u32 本 AP 的 LAPIC ID
%define S_VECTOR     0x0C      ; u32 SIPI 向量
%define S_CR3        0x10      ; u64 内核 PML4 物理地址（BSP 当前 CR3）
%define S_CR0        0x18      ; u64 BSP 的 CR0（已或上 PE|PG）
%define S_CR4        0x20      ; u64 BSP 的 CR4（已或上 PAE）
%define S_EFER       0x28      ; u64 BSP 的 EFER（已或上 LME）
%define S_ENTRY      0x30      ; u64 64 位入口（内核高半区函数地址）
%define S_RSP        0x38      ; u64 本 AP 的内核栈顶（16 字节对齐）
%define S_FLAGPTR    0x40      ; u64 本 AP 的在线标志字节指针（内核高半区）
%define S_COUNTPTR   0x48      ; u64 在线原子计数指针（内核高半区）
%define S_GDT        0x50      ; u8[64] 临时 GDT（BSP 的 GDT + index3 = 32 位代码段）
%define S_IDTR       0x90      ; u8[10] BSP 的 IDTR（AP 也装上：出异常能看到原因）

; ★ 共享块各字段的**页内绝对地址** = 页基址 + 块偏移 + 字段偏移。
;   踩坑记录：早期漏了 +SMP64_TRAMPOLINE_PHYS，于是跳板去读 0x0820（低端垃圾）当 CR4、
;   并把 GDT 拷到 0x0C00 / 读 0x0850 当源 —— 症状是 AP 在远跳 0x18 时 #GP(0018) 三重故障复位。
;   自检（smp64_selftest64）现在会核对页内 GDTR 描述符 = 页基址 + SMP64_OFF_GDT_COPY。
%define P_BASE       SMP64_TRAMPOLINE_PHYS
%define P_SBLOCK     (P_BASE + SMP64_OFF_SHARED)
%define P_MAGIC      (P_SBLOCK + S_MAGIC)
%define P_INDEX      (P_SBLOCK + S_INDEX)
%define P_APICID     (P_SBLOCK + S_APICID)
%define P_VECTOR     (P_SBLOCK + S_VECTOR)
%define P_CR3        (P_SBLOCK + S_CR3)
%define P_CR0        (P_SBLOCK + S_CR0)
%define P_CR4        (P_SBLOCK + S_CR4)
%define P_EFER       (P_SBLOCK + S_EFER)
%define P_ENTRY      (P_SBLOCK + S_ENTRY)
%define P_RSP        (P_SBLOCK + S_RSP)
%define P_GDT        (P_SBLOCK + S_GDT)
%define P_IDTR       (P_SBLOCK + S_IDTR)
%define P_GDT_COPY   (P_BASE + SMP64_OFF_GDT_COPY)

[bits 16]
[org SMP64_TRAMPOLINE_PHYS]

smp64_trampoline_start:
    cli                             ; AP 全程关中断：本阶段不接任何中断
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, SMP64_RM_STACK_TOP      ; 自己的实模式栈（页内 0x8E00..0x8F00，见布局）
    cld

    ; 8259 全掩码：AP 这条路径不接 PIC 中断（BSP 已经是 LAPIC+IOAPIC 模式，
    ; 这里只是把 AP 本地的 PIC 屏蔽位也钉死，免得将来谁把 IMR 放开）
    mov al, 0xFF
    out 0x21, al
    out 0xA1, al

    ; A20：fast gate（端口 0x92 bit1）。QEMU/SeaBIOS/UEFI 下 A20 本来就开着，
    ; 这里只是"若需要"的兜底；只置 bit1，不动 bit0（bit0 是 fast reset）。
    in al, 0x92
    or al, 0x02
    out 0x92, al

    ; ---- 把临时 GDT 从共享块拷到固定地址 0x8C00（AP 看不到高半区，只能看低端物理内存）
    ;      这也是"AP 真的在跑我们这段代码"的早期证据。
    mov si, P_GDT
    mov di, P_GDT_COPY
    mov cx, 64
    rep movsb

    lgdt [smp64_gdtr]

    ; ---- 进保护模式（16 位）：0x18 = 临时 32 位代码段（D=1/L=0）。
    ;   注意 **不能**在这里直接跳 0x08 —— 0x08 是 64 位代码段（L=1），
    ;   IA-32e 还没打开（EFER.LME=0/CR0.PG=0）时跳 L=1 的描述符会 #GP。
    mov eax, cr0
    or eax, 1                       ; CR0.PE = 1
    mov cr0, eax
    jmp 0x18:smp64_pm32             ; 远跳 -> 32 位保护模式（同时把 CS 换成 GDT 项）

; ---- 以下固定偏移必须与 smp64.h 一致（SMP64_OFF_GDTR）----
    times (SMP64_OFF_GDTR - ($ - $$)) db 0xCC
smp64_gdtr:
    dw (8 * 8 - 1)                  ; limit = 63（8 项）
    dd P_GDT_COPY                   ; base  = 0x8C00（AP 刚拷过去的临时 GDT）

; ---- 32 位保护模式（SMP64_OFF_PM32）----
    times (SMP64_OFF_PM32 - ($ - $$)) db 0xCC
[bits 32]
smp64_pm32:
    mov ax, 0x10                    ; 内核数据段（与 BSP 同一个选择子）
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov esp, SMP64_RM_STACK_TOP     ; 32 位阶段不用栈；指页内安全区（非 call，仅保险）

    ; ---- CR4（BSP 的值，已含 PAE）-> CR3（内核 PML4 物理地址）----
    mov eax, [P_CR4]
    mov cr4, eax
    mov eax, [P_CR3]
    mov cr3, eax

    ; ---- EFER：整值照抄 BSP 的（含 LME/NXE/SCE 等），只保证 LME=1 ----
    mov ecx, 0xC0000080
    mov eax, [P_EFER]
    mov edx, [P_EFER + 4]
    wrmsr

    ; ---- CR0（BSP 的值 | PE | PG）-> 打开分页 = 进 IA-32e 兼容模式 ----
    mov eax, [P_CR0]
    or eax, 0x80000001              ; PG | PE
    mov cr0, eax

    jmp 0x08:smp64_lm64             ; 远跳进 **64 位代码段**（0x08，与 BSP 一致）

; ---- 64 位入口桩（SMP64_OFF_LM64）----
    times (SMP64_OFF_LM64 - ($ - $$)) db 0xCC
[bits 64]
smp64_lm64:
    ; ★ 用**自己的**栈：共享块里的 per-AP 栈顶（BSP 用 page_alloc_64 分配的 16KB）
    mov eax, P_RSP
    mov rsp, [rax]
    and rsp, ~0xF                   ; 16 字节对齐
    sub rsp, 8                      ; 让 C 函数看到 RSP%16==8（模拟 call 压的返回地址）
    mov qword [rsp], 0
    ; BSP 的 IDT 也装上：AP 万一出异常，走同一套 PANIC 路径（留证据，不是三重故障）
    mov eax, P_IDTR
    lidt [rax]
    ; 进内核（高半区地址，paging 已开、CR3 与 BSP 相同 -> 映射成立）
    mov eax, P_ENTRY
    mov rax, [rax]
    jmp rax                         ; -> smp64_ap_entry64()（不再返回）

smp64_trampoline_end:
    ; ★ 不与共享块重叠：编译期先兜一层（C++ 侧 smp64_prepare64 还会再查一次长度，
    ;   因为 nasm 的 %if 对"同一趟里定义的标签"只能做保守估计）。
%if (smp64_trampoline_end - $$) > SMP64_OFF_SHARED
    %error "AP trampoline code overlaps the shared data block (SMP64_OFF_SHARED)"
%endif
