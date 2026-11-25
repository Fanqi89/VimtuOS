# HANDOVER —— v0.11.9（KPTI 双页表：用户地址空间里没有内核高半区）

- 日期：2026-11-25（本地）
- 版本串：`build64.sh` `VIMTUOS_VERSION="0.11.9"`（**已提交、未发版**）
- 基线：v0.11.8（`cca2c043`，ring3 隔离第一层：CR0.WP + CR4.SMEP + 审计 + 探针）
- 内核：**3,558,464 B**（闸门 15,728,640 B）；构建 `EXIT=0`
- 起因：用户指令"继续完成做 KPTI 的计划" —— v0.11.8 封存的 KPTI 本轮**正式开工并交付**

## 0. 一句话结论

**KPTI 做完了，而且是被数出来的**：用户地址空间里 `PML4[511] = 0`（内核高半区一个字节都摸不到），
进出内核的每条路径只经过固定 VA `0x0000000180000000`（6 GiB）那一小段"两张表都映射"的共享代码。
证据：`tests/iso64_test.py` **41/41 PASS**，其中
`[ISO64] audit pid=26 … khigh=0 … kind=user`（用户表里内核项**为零**）、
`probe … kpti_u=1 u_violation=0 ok=1`、`[KPTI64] selftest PASS mask=63`。

## 1. 设计（最终落地的形态，与 v0.11.8 封存时的草图差别很大，以本节为准）

### 1.1 两张 PML4 / 一个共享入口区

| 东西 | 位置 | 说明 |
|---|---|---|
| 内核表 | `Proc64::cr3`（= `pml4_phys`） | 与 v0.11.8 完全一样：PML4[511] 链 + 前 4GB 恒等映射 + 私有 PDPT[4] |
| 用户表 | `Proc64::pml4_user_phys`（**新增**） | 只有两处不同：**PML4[511] = 0**、PML4[255] 数据槽 |
| 两张表共用的私有 PDPT | `Proc64::pdpt_phys` | 所以 map/unmap 用户页在两张表里**同时生效**（内核访问用户内存不受影响） |
| 共享入口区 | `KPTI64_BASE = 0x0000000180000000`（**6 GiB**） | 挂在该私有 PDPT 的 **[6]**，每一张表都可见；U/S=0 |

`PML4[255]` 是"**非 present 数据槽**"：内核表里存用户表物理地址，用户表里存内核表物理地址。
蹦床靠它找到"另一张表"，于是**不需要**在每次任务切换时维护"当前进程用哪张表"的全局变量。
（`PML4[255]` 落在 0..127TB 的非规范区，两张表都没别的用处；非 present 项 CPU 不查保留位。）

### 1.2 共享入口区的页布局（**页号不写死**）

- 页 0..N-1：内核镜像里 `.text.kpti` 那一段（`kernel/kpti_blob64.asm`）——不额外占内存；
- 页 N：向量偏移表（256 × qword，0 = 该向量没有蹦床）；
- 页 N+1：IDT（运行期 `page_alloc_64` 一页）；
- 页 0 内含：入口/出口代码 + 变量 + **GDT 副本 + TSS 副本** + 入口栈（1792 B）+ 返回栈（512 B）。

N 由 `kpti64_init64()` 按 `kpti64_offtab64` 的**实际偏移**算（`img_pages = ceil((off+2048)/4096)`）。
**这一条踩过**：给蹦床 0 加 8 字节魔数之后偏移表从页 2 挪到页 1，写死 0x2000 的断言当场把 KPTI 关掉。

### 1.3 入口 / 出口路径（全部只经过共享区）

| 来源 | 入口 | 出口 |
|---|---|---|
| `int 0x80` / 异常 / IRQ | IDT 门 → 共享区蹦床（每向量一条）→ 判 `CS.RPL` | `isr_stubs64.asm` 的公共出口改成 `g_isr64_exit64` 指针 → 共享区 `kpti64_iret_frame64` |
| `syscall` 指令 | LSTAR = 共享区 `kpti64_syscall_entry64` | 同一段代码里的 `o64 sysret` 出口 |
| 从内核进 ring3 | `task_switch_iret64` 也是走 `g_isr64_exit64` 指针 | 同上（CS.RPL=3 → 搬帧 + 切 CR3 + iretq） |

- **来自用户**：蹦床把**整帧**（15 个通用寄存器 + 4 个段槽 + vec/err + iret 帧 = 0xD0）压好，
  再切到内核表，把帧搬到"本任务真实内核栈"，然后跳 `isr64_common_call64`（省掉重复压栈）。
- **来自内核**：`jmp qword [kptii_isr64_common_ptr]`（**内存间接跳转**，见 §3 坑 1）。
- **出口**：帧搬到共享返回栈 → **先用寄存器切回用户表** → 在共享栈上 pop + `iretq`/`sysret`。
  为什么必须搬：`mov cr3, 内存` 不存在（CR 只能和通用寄存器互写），而 pop 完通用寄存器已经是
  用户值了，没有寄存器能腾出来放 CR3。
- **TSS.rsp0 恒为共享入口栈顶**（`tss_set_rsp0()` 里分流）；真实内核栈顶镜像到共享区的
  `kptii_kstack64`，只给"搬帧"用。

### 1.4 打开条件（三条全满足才启用，否则**逐指令**等于 v0.11.8）

1. 引导期页表是我们自己的（BIOS 路径：CR3 = 0x40000）。UEFI 固件表不启用
   （`reason=boot-cr3-not-own`；VMware EFI 下运行期 `mov cr3` 已知会复位，见 proc64.cpp）。
2. 不是安装介质内核（`reason=installer-media`：没有进程、不跑 ring3）。
3. 页池能分出 IDT+PD+PT 三页、偏移表页对齐、别名映射逐字节核对一致。

不满足时 **不映射、不改门、不改 TSS/LSTAR**，`g_isr64_exit64` 保持指向原位出口 → 行为不变。

## 2. 验收（实跑，串行）

| 脚本 | 结果 | 说明 |
|---|---|---|
| `tests/iso64_test.py`（扩到 **41 条**：+KPTI 建立/自检/门目标/用户表审计） | **41/41 PASS** | 含 `-cpu max` 证 SMEP |
| `tests/micropython64_test.py` | **PASS** | 关键回归：KPTI 下运行时 ELF 装载 + REPL + 脚本 argv 全通 |
| `tests/proc64_test.py` | **PASS 67/67** | fork/execve/wait4/kill + 多进程演示（双 CR3 每进程） |
| `tests/version64_test.py` | **PASS 8/8** | 版本串 0.11.9 |
| `tests/deskfolder64_test.py` | **64/74** | = v0.11.7 基线（10 条失败全是既有的 2 项 UNVERIFIED：拖进回收站手势没命中 + 圆角像素判据） |
| 构建 | `EXIT=0` | `kernel64_os.bin` **3,558,464 B** |

### 2.1 串口原证（默认 qemu64 CPU）

```
[KPTI64] init ok area=0000000180000000 blob_pa=000000000041B000 pd=000000000A001000 pt=000000000A002000 idt=000000000A000000 entry_rsp=0000000180000DB0 tramp=50 kpti=1
[KPTI64] mode kpti=1 entry=int80+irq+syscall rsp0=shared dual_cr3=1 (user table PML4[511]=0; kernel table full)
[KPTI64] selftest PASS mask=63 bits=0 kpti=1
[X64] idt[32] sel=0000000000000008 attr=000000000000008E idtr.limit=0000000000000FFF idtr.base=0000000180002000
[SYSCALL] msr init EFER.SCE=1 star=001B000800000000 lstar=00000001800004A3 fmask=0000000000000700
[ISO64] mode kpti=1 smap=0 reason=user-space-has-no-kernel-half (user table PML4[511]=0; entry trampolines at 6 GiB)
[ISO64] audit pid=- space=0000000000040000 khigh=514 ident=2052 uwin=0 outside=0 us_violation=0 why=selftest ok=1 entry=1 fbwin=0 kind=kern
[ISO64] probe pid=26 map=1 range=1 us_violation=0 pa=000000000CA63000 kpti_u=1 u_violation=0 ok=1
[ISO64] audit pid=26 space=000000000CA60000 khigh=0 ident=2052 uwin=1 outside=0 us_violation=0 why=probe-user ok=1 entry=1 fbwin=0 kind=user
```

判据里最硬的一条：**`audit … khigh=0 … kind=user`** —— 用户表里内核高半区项数为 **0**（数出来的）。

## 3. 踩过的坑（本批新增，都是真踩出来的，别重犯）

1. **蹦床不能用 `mov rax, isr64_common; jmp rax`**。蹦床运行时通用寄存器还是被中断现场的值
   （`isr64_common` 还没压它们），clobber rax 会把它们**当成帧内容**保存/恢复。
   实测症状：kmain 的 tick 比较寄存器被改成 `0xffffffff803a5f58`，tick 循环在 `hlt` 里死等
   （`info registers` 抓到的现场：IRQ0 计数 3877、`g_ticks64` 不动）。
   处理：`jmp qword [kptii_isr64_common_ptr]`（地址槽在共享区，数据段里 `dq isr64_common`）。
2. **共享区的代码只能用"节内符号"的 RIP 相对寻址**。跨节符号（`g_syscall64_exit_to_kernel64`、
   `user64_resume_tramp64`、`syscall64_dispatch64`）必须用 `mov reg, symbol`（imm64）绝对寻址。
   踩法：`mov qword [g_syscall64_exit_to_kernel64], 0` 的 RIP 位移是**按链接地址（内核高半区）**
   算的，同一段代码搬到 6 GiB 跑时算成 `0x142C4D4FC` → `#PF err=2 cr2=0x142C4D500`。
   自查办法：`readelf -r kpti_blob64.o` 里**只允许** `R_X86_64_64`，出现 `PC32` 就是踩了。
3. **ring3 入口路径必须先压全 15 个通用寄存器再动任何寄存器**（`rep movsq` 会毁 rsi/rdi/rcx）。
   早期版本只 push rax/rbx 当暂存 → 用户 rsi/rdi/rcx 被改。
4. **回 ring3 的那一段必须 `cli`**。从"帧搬到共享返回栈"到 `iretq` 之间 rsp 落在共享区里，
   若被 PIT 打断，调度器会把"帧指针在共享栈上"当成坏帧：
   `[TASK64] frameprobe FAIL frame=0x180000E10 … -> halt`（整机停在 xmm 演示中途）。
   `iretq`/`sysret` 都会用帧里的 rflags/r11 恢复 IF，所以关中断不丢状态。
5. **蹦床 0 的偏移不能是 0**。偏移表里 0 = "该向量没有蹦床"，而蹦床 0 恰好曾落在偏移 0
   （`tramp=49`）→ 除零门悄悄退回内核高半区的桩（KPTI 下就是 #PF）。
   处理：节首放 8 字节魔数 `dq 0x4B50544936344100`。
6. **5 GiB 不能用**：`USER64_FB_VA64 = 0x140000000` 是 framebuffer/DRM/wl-surface 窗口
   （v0.9 起就占着 PDPT[5]）。KPTI 共享区改到 **6 GiB（PDPT[6]）**。
7. `memlayout64.h` 与 `mem_64.h` 都定义 `PAGE_SIZE_64`（一个 static const、一个宏），
   同文件包含**只有顺序对**才行 → kpti64.cpp 两个都不 include，直接用两个常量并注明真源。
8. 审计要认得两处"窗口外"的**已知**映射：PDPT[5] = fb 窗（要求 U/S=1）、PDPT[6] = 共享入口区
   （要求 U/S=0 且 PD 物理地址 == `kpti64_pd_phys64()`）。日志末尾追加 `entry=` / `fbwin=` / `kind=`。

## 4. 本批**没做**的（如实点名）

1. **UEFI 路径没有 KPTI**：固件页表只读、`mov cr3` 在 VMware EFI 下会复位，
   `reason=boot-cr3-not-own`。BIOS 路径（QEMU 全程用它）已完整覆盖。
2. **SMAP** 仍未开（`smap_on=0`）：需要把内核读用户内存的点逐条 `STAC/CLSTAC`。
3. **启动期 WP 泄漏**（v0.11.8 发现）仍在：本轮串口每次都能看到
   `[ISO64] wp leak detected at selftest (CR0.WP=0) -> reassert`（自检里检测+修复+留痕，
   `mask=63`，`wp_leak=` 如实）。**根因未定位**（`fb.cpp` 的 `fb64_cr0_set64` 早退仍是主嫌）。
4. **KPTI 的性能账没量**：每个 IRQ / syscall 各两次 `mov cr3`（无 PCID，每 tick 2 次）；
   TCG 下实测启动到桌面与 v0.11.8 同量级（串口 200 KB/90 s），但没有做受控对比基准。
5. `iso` 命令**还没接进设置页**；`iso64_test` 还没纳入发版门禁。

## 5. 本批改动的文件

| 文件 | 要点 |
|---|---|
| `kernel/kpti_blob64.asm` | **新增**（约 440 行）：蹦床 50 条 + 用户态入口 + 出口 + syscall 入口/出口 + GDT/TSS/栈副本 + 偏移表 |
| `kernel/kpti64.h/.cpp` | **新增**：共享区建立（动态页号）、门重定向、`PML4[255]` 配对、6 位自检、报告 |
| `kernel/x86_64.cpp/.h` | `g_idtp`/`g_tss_active` 间接层；`idt_set_gate64` 自动指向蹦床；`x86_64_kpti_enable64()`（GDT/TSS/IDT 搬家 + 重载 + 门重定向）；`tss_set_rsp0` 语义分流；`x86_init64()` 末尾调 `kpti64_init64()` |
| `kernel/isr_stubs64.asm` | `isr64_common` / `isr64_common_call64` 对外；出口改走 `g_isr64_exit64`（默认原位出口 `isr64_exit_plain64`）；`task_switch_iret64` 也走它 |
| `kernel/proc64.cpp/.h` | 双 PML4（`pml4_user_phys`）、`kpti64_link_pair64`、`p64_cr3_is64` 认两张表、`proc64_user_cr364()`、布局自检加 KPTI 项 |
| `kernel/syscall64.cpp` | `syscall64_lstar_target64()`（LSTAR 指向谁的唯一真源）+ `syscall64_kpti_set_lstar64()`；自检断言跟着改 |
| `kernel/iso64.cpp/.h` | 审计分"内核表/用户表"两种判据（`khigh` 该有/该没有）、认 PDPT[5] fb 窗与 PDPT[6] 入口区、新增 `iso64_audit_user64()`；mode/status 行如实报 `kpti=`；probe 同时审用户表 |
| `build64.sh` | 0.11.8→0.11.9；`SRCS_CORE` 加 `kpti64.cpp`；nasm `kpti_blob64.asm`；三条链接线各加 `kpti64.o` 与 `kpti_blob64.o` |
| `tests/iso64_test.py` | +KPTI 建立行/蹦床数/自检/入口区审计/用户表 `khigh=0`/probe `u_violation` 等判据（共 41 条） |

## 6. 下一轮建议（按价值排序）

1. **启动期 WP 泄漏定位**（`fb.cpp` 的 `fb64_cr0_set64` 早退路径改成成对 enter/leave），
   然后撤掉 `iso64_reassert64()` 兜底；自检里的 leak 检测保留（它是判据）。
2. **KPTI 性能受控基准**：同一镜像 v0.11.8 vs v0.11.9 的 `[UI] perf … redraw_us` 与启动 tick 数对比。
3. `iso` 命令接进设置页（"安全"页），`iso64_test` 纳入发版门禁。
4. **SMAP**（逐点 STAC/CLSTAC，每批跑 micropython/deskfolder/desktopops）。
5. UEFI 路径的 KPTI（要先把 `mov cr3` 复位问题查清）。

## 7. 现场

- HEAD = 本提交；`kernel64_os.bin` **3,558,464 B**；`VIMTUOS_VERSION="0.11.9"`
- 产物：`build64/sysdisk.img`(38,489,600) / `vimtu64-64.iso`(93,458,432) / `vimtu64-64.img`
- 打点总表：`[KPTI64] init ok|init skipped|mode|selftest|status` + `[ISO64] hw|mode|selftest|audit|audit all|probe|status`
- 未发版：`tag v0.11.9` + Release + 附件回读 + 旧版 Release 不动 —— 按 `docs/发布流程.md` 走
