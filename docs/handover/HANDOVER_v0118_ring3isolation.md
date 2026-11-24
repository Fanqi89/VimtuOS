# HANDOVER —— v0.11.8（ring3 真隔离第一层：硬件隔离位 + 地址空间审计 + 活进程探针）

- 日期：2026-11-24（本地）
- 版本串：`build64.sh` `VIMTUOS_VERSION="0.11.8"`（**未发版**；本轮先发了 v0.11.7，然后开工这一层）
- 基线：**v0.11.7 已发布**（tag `d740f4d0…` / Release `456771997` / 封存单 `HANDOVER_v0117_ready.md`）
- 内核：**3,538,208 B**（闸门 15,728,640 B）；构建 `EXIT=0`

> 用户本轮指令（原文）：
> 1) 先打包发布 v0.11.7；
> 2) 然后立刻开工把 **ring3 真隔离**这层补齐。

## 0. 先把 v0.11.7 发出去了（回读事实）

| 项 | 值 |
|---|---|
| tag | `v0.11.7` = `d740f4d0beb14698308c2dc3042c475add6c7c30` → commit `26516540` |
| Release | id `456771997`（draft=false / prerelease=false，附件 2 个） |
| `vimtu64-64.iso` | 93,458,432 B — `sha256:14498c264922ae4c59256c3d468cf0ff982d5694f91cfed5a5916298a75009ca` |
| `vimtu64-64.img` | 42,684,416 B — `sha256:55cb40373bbf086a18fc77c291fa0a7593619a5a58fdd6eafedddb991c6ff948` |
| 旧版 v0.11.5 | 两个附件的 `id/size/digest/updated_at` **逐条未动**（RESULT: PASS） |

发版脚本 `$PI_SCRATCH_DIR\rel_v0117.py`；封存单 `docs/handover/HANDOVER_v0117_ready.md`；索引与
`docs/发布流程.md` §9 已回写。

## 1. ring3 "真隔离"的**起点事实**（先把话说清楚）

本轮开工前，实测/读码得到的事实（不是猜）：

1. **每进程有自己的 CR3**（BIOS 路径 `proc64_init64` 探测通过 → `[PROC64] init mode=isolated`），
   但 `p64_build_as64` 把引导期 PML4 **512 项全复制**（`kernel/proc64.cpp:736`），
   于是 **PML4[511]（内核高半区）与 PML4[0]→PDPT[0..3]（前 4GB 恒等映射）在每个用户地址空间里都存在**；
   `proc64.cpp:2392` 甚至把它钉成自检断言 `pml4[511] == boot[511]`。→ **没有 KPTI**。
2. 这些内核项 **U/S=0**（`boot/loader64.asm` 里全是 `| 3`，无 bit2），所以 ring3 访问内核页会 #PF、被
   `sig64` 转成 SIGSEGV 杀掉该进程（不 PANIC 整机）—— 这一层**本来是对的**。
3. 但**硬件隔离位一个都没开**：CR0.WP=0（内核自己能写只读页）、CR4 只有 OSFXSR/OSXMMEXCPT（无 SMEP/SMAP）、
   无 PCID、无缺页按需分页。
4. ring3 进程启动是要**真的建新地址空间**的（`proc64_create64`），而"活进程的页表到底干不干净"从来没人查过。

## 2. 本批做了什么（`kernel/iso64.h/.cpp`，新增约 400 行，只进系统内核）

### 2.1 硬件隔离位（启动期一次性打开）

- **CR0.WP = 1**：内核（CPL0）也不能写只读页 —— 挡住"内核 bug 顺手改坏用户 .text / 页表 / 只读数据"。
- **CR4.SMEP = 1**（CPU 支持才开）：内核不能**执行**用户页 —— 挡住"把用户内存当代码跳进去"这一类提权跳板。
- **CR4.SMAP**：**本版明确不开**（开了之后内核每次读用户内存都要 STAC/CLSTAC，本内核里
  路径字符串/iovec/ELF 装载/终端邮箱等访问点太多，要单独一轮逐点包起来）。
  日志如实报 `smap_supported=<0|1> smap_on=0`，**不假装**。

### 2.2 地址空间审计（真证据，不是嘴说）

`iso64_audit64(cr3, pid, why)` 走四级页表，逐类**数**出 present 项：

| 类 | 含义 | 要求 |
|---|---|---|
| `khigh` | PML4[511] 链上的项（内核高半区） | 全部 **U/S=0** |
| `ident` | PML4[0]→PDPT[0..3] 链上的项（前 4GB 恒等映射/MMIO） | 全部 **U/S=0** |
| `uwin` | PDPT[4]（4GiB..5GiB 用户窗口）里的叶子页 | 全部 **U/S=1** |
| `outside` | PDPT[5..511]（窗口外的用户地址） | **必须全不存在**（出现即漏洞） |

任何一条不符 → `us_violation++`、`ok=0`。审计**只读页表、不改映射**（悄悄"修好"等于把证据擦掉）。

### 2.3 活进程探针（`iso64_probe64`）

为什么需要它：实测**终端 shell 不是独立 ring3 进程**（`iso` 里 `procs=0`），拿外部 app 当判据靠不住。
探针自己造对象：`proc64_create64("iso_probe")` → 切到它的 CR3 → `user64_map_page64` 映一页用户页
→ 审计（此时 `uwin>=1` 才有意义）→ 切回内核 CR3 → `proc64_destroy64`。

### 2.4 自检（6 位）+ 终端命令 `iso`

- 自检 6 位：① CR0.WP ② CR4.SMEP（CPU 支持则必须开）③ 内核地址被用户指针闸门拒绝
  ④ 用户窗口地址被接受（无进程时如实 `win=0` 跳过，由探针补验）⑤⑥ 内核 + 全进程地址空间审计零违规。
- 终端 `iso`：打 `[ISO64] status …` + 审一遍所有活着的地址空间 + 跑探针 + 再自检一次。
  **用户自己也能敲 `iso` 验**，不用信我的结论。

### 2.5 ★ 真发现：启动期 CR0.WP 会被某个路径清掉（中间那次构建实测到）

**实测经过（如实）**：把 CR0.WP 打开后的**第一次**构建，启动自检红在第 ① 位（`mask=54`），
而同一份构建的 `[ISO64] hw` 行明明 `wp=1`；到桌面后终端里 `iso` 又报 `wp=1`。
⇒ 结论只能是：**启动期有某条路径把 WP 清掉、之后才恢复**（`fb.cpp` 里"临时清 WP 写页表"的
`fb64_cr0_set64(cr0_nw)` 是主要嫌疑，但本轮**没有逐点锁定**，不猜结论）。
**最终构建没有再复现**（boot 自检 `mask=63`、`wp_leak=0`）——这只能说明"这次启动顺序没踩到"，
**不等于问题已定位**。

处理方式（不隐瞒、也不留隐患）：
- `iso64_selftest64()` **先按真实值判**；掉了就打 `[ISO64] wp leak detected at selftest (CR0.WP=0) -> reassert`，
- 然后 `iso64_reassert64()` 补上并**复判**，`mask` 反映修复后的稳态，同时把 `wp_leak=<0|1>` 打进自检行；
- `kernel64.cpp` 在 `gui64_run(bi)` **之前**再补一次（保证图形阶段一定在）；
- 第 6 节的坑清单里记了一条：改 `fb.cpp` 的临时代码时**必须成对** `fb64_cr0_set64(cr0_nw)` / `fb64_cr0_set64(cr0)`。
- **下一轮要做的事**：把这个泄漏点定位到具体早退路径（`fb.cpp` 多处），改成成对 enter/leave，
  然后删掉这条兜底 reassert。

## 3. 验收（实跑）

| 脚本 | 结果 | 说明 |
|---|---|---|
| `tests/iso64_test.py`（**新增**，6 阶段 32 条判据） | **32/32 PASS** | 见下面原文 |
| `tests/micropython64_test.py` | **PASS** | **关键回归**：WP=1 下运行时 ELF 装载（`/bin/mp`）照常工作，无 PANIC |
| 构建 | `EXIT=0` | `kernel64_os.bin` **3,538,208 B** |

### 3.1 串口原文（默认 qemu64 CPU）

```
[ISO64] hw wp=1 smep=0 smep_supported=0 smap_supported=0 smap_on=0 cr0=0000000080010011 cr4=0000000000000620 ok=1
[ISO64] mode kpti=0 smap=0 reason=user-space-still-maps-kernel (PML4[511] shared by design; KPTI needs a trampoline + dual tables, next stage)
[ISO64] selftest PASS mask=63
[ISO64] audit pid=- space=0000000000040000 khigh=514 ident=2052 uwin=0 outside=0 us_violation=0 why=selftest ok=1
[ISO64] status wp=1 smep=0 smap=0(supported=0) kpti=0 audited=1
[ISO64] audit all spaces=1 procs=0 slots=0 violations=0 ok=1
[ISO64] probe pid=26 map=1 range=1 us_violation=0 pa=000000000CA46000 ok=1
[ISO64] audit pid=26 space=000000000CA42000 khigh=514 ident=2052 uwin=1 outside=0 us_violation=0 why=probe ok=1
[ISO64] selftest PASS mask=63 wp=1 smep=0 audited=4 violations=0 win=0 wp_leak=0 cr0=0000000080010011 cr4=0000000000000620
```

### 3.2 `-cpu max`（带 SMEP/SMAP 位的 CPU）—— SMEP **真的被打开**

```
[ISO64] hw wp=1 smep=1 smep_supported=1 smap_supported=1 smap_on=0 … ok=1
[ISO64] selftest PASS mask=63
```
→ 默认 `qemu64` 没有 SMEP 位（如实报 `smep_supported=0`）；换 `-cpu max,la57=off,umip=off` 后
**SMEP 真的置位**且自检仍全绿；SMAP 仍如实关闭。

### 3.3 判据要点（32 条里最硬的几条）

- `CR0.WP=1`（内核不能写只读页）
- `selftest PASS mask=63`（6 位全绿，含两个地址空间审计）
- `audit … khigh=514 ident=2052 outside=0 us_violation=0`（**数出来的**：内核高半区 514 项、恒等映射 2052 项，
  全部 U/S=0；窗口外 0 项）
- `probe pid=26 map=1 range=1 us_violation=0 ok=1` + `audit pid=26 … uwin=1 … us_violation=0`
  （真进程、真有用户页、用户窗口里 1 页是 U/S=1、内核项一个都不可被用户访问）
- `-cpu max` 下 `smep=1`（CPU 支持时真的打开）
- 全程无 PANIC / TRIPLE FAULT / OOM

## 4. 本批**没做**的（下一层，如实点名）

1. **KPTI（内核页表隔离 / 双页表）** —— 本版**没有**。现在用户地址空间里仍映射着内核高半区（U/S=0）。
   要做的事（设计已想清楚，下一轮实施）：
   - 用户 CR3 不再复制 PML4[511]；另建**内核 CR3**（完整映射）；
   - 入口蹦床：`int 0x80` / `syscall` / 中断入口必须**在切 CR3 之前**只用到"两张表都映射"的那一小段代码
     （固定物理页 + 每 CPU 栈），所以要把 `syscall_entry64` / `isr_stubs64` 的入口片段单独放进
     一个"共享段"（同一物理页映射到两个地址空间的固定 VA），IDT 与 TSS/IST 也要在这张共享段里；
   - 进入内核后立刻 `mov cr3, kernel_cr3`，返回 ring3 前切回 user_cr3；
   - 验收判据（先写死）：`[ISO64] kpti=1` + `audit pid=N khigh=0`（用户地址空间里**一个内核项都没有**）
     + `probe` 仍 `ok=1` + `micropython64_test`/`deskfolder64_test` 全绿。
2. **SMAP** —— 需要把内核里所有"读用户内存"的点（`user64_range_ok64` 之后的那段访存、路径字符串拷贝、
   iovec、终端邮箱、ELF 装载）用 `STAC/CLSTAC` 成对包起来；工作量比 SMEP 大一个量级。
3. **启动期 WP 泄漏点的精确定位**（见 §2.5）。
4. 缺页按需分页 / COW：仍然没有（`proc64.h` §5 已写明取舍）。

## 5. 本批改动的文件

| 文件 | 要点 |
|---|---|
| `kernel/iso64.h/.cpp` | **新增**：CR0.WP + CR4.SMEP、四级页表审计（khigh/ident/uwin/outside/us_violation）、6 位自检、活进程探针、`iso64_reassert64` |
| `kernel/kernel64.cpp` | 启动期 `iso64_init64()` / `iso64_selftest64()`；`gui64_run` 之前 `iso64_reassert64()` |
| `kernel/terminal64.cpp` | 新命令 `iso` / `isolation`（状态 + 全地址空间审计 + 探针 + 复检） |
| `build64.sh` | 0.11.7→0.11.8；`SRCS_DESKTOP` 加 `iso64.cpp`；两条链接线加 `iso64.o` |
| `tests/iso64_test.py` | **新增**：6 阶段 32 条（含 `-cpu max` 证 SMEP） |
| `docs/handover/HANDOVER_v0117_ready.md` | v0.11.7 封存单（发布回读事实） |

## 6. 不要踩的坑（本批新增）

1. **`HwInfo64` 的 CPU 字段在 `->cpu` 里**（`hw_info64()->cpu.smep`），不是顶层。
2. **默认 `qemu64` 没有 SMEP/SMAP 位**：要验 SMEP 必须 `-cpu max`（本测试用 `max,la57=off,umip=off`
   避免 5 级页表/UMIP 干扰）。
3. **本机终端 shell 不是独立进程**（`proc64_count64()==0`）——"活进程审计"必须自己造进程（探针），
   别指望外部 app。
4. **`iso64_audit64` 走的是"物理地址直接解引用"**（前 4GB 恒等映射常驻）；给别的地址空间审计时
   传它的 CR3 物理地址即可，不需要切 CR3。
5. **WP 打开后，ELF 运行时装载必须"先写后设只读"**：本轮 `micropython64_test` 已验证现有装载顺序是安全的；
   以后改 `elf64.cpp` 的映射顺序要重跑它。
6. 自检里"用户窗口可用"这一位**没有进程时会假红**（窗口是空的，闸门当然拒）——
   本版改成 `win=<0|1>` 如实记录 + 探针补验，别再把"没进程"当成"隔离坏了"。

## 7. 现场 / 下一轮

- 内核 **3,538,208 B**；`iso64_test` **32/32**；`micropython64_test` **PASS**
- 打点总表：`[ISO64] hw|mode|selftest|audit|audit all|probe|status|wp reassert|wp leak`
- 下一轮（按优先级）：
  1. **KPTI 双页表**（§4.1 的设计，判据先写死再实现）；
  2. 定位并修掉启动期 WP 泄漏（然后删掉兜底 reassert）；
  3. SMAP（STAC/CLSTAC 逐点包裹）；
  4. 把 `iso` 命令接进设置页（用户可见）+ `iso64_test` 纳入发版门禁。
