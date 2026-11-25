# HANDOVER —— v0.12.0（SMAP 真开 + WP 泄漏根因修正 + KPTI/SMAP/WP 三项复核）

- 日期：2026-11-25（本地）
- 版本串：`build64.sh` `VIMTUOS_VERSION="0.12.0"`（唯一真源）
- 基线：v0.11.9（`d159d953`，KPTI 双页表；已发版 tag `v0.11.9` / Release `457607672`）
- 内核：**3,566,928 B**（闸门 15,728,640 B）；构建 `EXIT=0`
- 本轮指令：读 v0.11.9 交接单恢复现场 → 补完 **SMAP** 与 **WP** → 复核 KPTI/SMAP/WP 三项是否全部完成

## 0. 一句话结论

**三项现在都是"数出来的"，不是"跑起来了"：**

| 项 | 结论 | 硬判据（串口原串） |
|---|---|---|
| **KPTI** | 已完整（BIOS 路径） | `[KPTI64] init ok … tramp=50 kpti=1`；`[ISO64] audit pid=26 … khigh=0 … kind=user` |
| **SMAP** | **本轮打开并通过**（CPU 支持时） | `[ISO64] hw … smap_supported=1 smap_on=1`；`-cpu max` 全程 **`[SMAP64] violation` 0 条** |
| **WP** | **根因找到并修掉**（原来那条"泄漏"是误判） | `[ISO64] selftest PASS mask=63 … wp_leak=0`；`[ISO64] wp check at gui-entry wp=1` |

`tests/iso64_test.py` 扩到 **48 条，48/48 PASS**（原 41 条）。

## 1. §8.2「启动期 CR0.WP 泄漏」的真相 —— 是**误判**，根因是调用顺序

### 1.1 现象与 v0.11.8/v0.11.9 的处置

那两版每次启动都打：

```
[ISO64] wp leak detected at selftest (CR0.WP=0) -> reassert
[ISO64] wp reassert was=0 now=1 cr0_was=0000000080000011
[ISO64] selftest PASS mask=63 … wp_leak=1
```

并且**把 `wp_leak=1` 的那一位"补完再复判"成 PASS** —— 判据被自己抹掉了。

### 1.2 根因（一行代码就能看出来）

`kernel/kernel64.cpp` 里**自检排在初化之前**：

```
1263:  (void)iso64_selftest64();     // ← 先自检
…
1284:  (void)iso64_init64();         // ← 后开 WP（而且还套在 user64_available64() 分支里）
```

**决定性证据**：那次读到的 `cr0_was = 0x80000011` = `PG|ET|PE`，正是
`boot/loader64.asm` 打开分页时写的值（`or eax, (1<<31)|1`），**本来就没有 WP 位**。
也就是说：没有任何一条路径"把 WP 清掉了"；是自检跑在"还没开"的时刻，
自检里的 `iso64_reassert64()` 顺手把它补上，于是后面（含进桌面）一直是 1。

顺带发现第二个缺陷：**WP/SMEP 只在 `user64_available64()` 分支里才开** →
UEFI 启动路径整层没开。

### 1.3 修法（判据改成"掉了就算失败"）

1. `iso64_init64()` 提到 `iso64_selftest64()` **之前**，并且**无条件**执行（不再套在分支里）；
2. 删掉 `user64_available64()` 分支里那次重复初化（留注释说明搬到哪去了）；
3. `iso64_selftest64()`：检测到 WP=0 → 打日志 + `fails |= 1`（**泄漏本身就是 FAIL**），
   仍然调用 `iso64_reassert64()` 先保住防护（不掩盖，只兜底）；
4. `kernel64.cpp` 进图形阶段前那处 `iso64_reassert64()` **兜底改成"只查不补"** 的
   `iso64_check_wp64("gui-entry")`（新增），打 `[ISO64] wp check at gui-entry wp=<0|1> cr0=<hex>`；
5. ⚠ 顺带修掉一个**会误导的日志 bug**：`[ISO64] mode` 那行在 v0.12.0 早期改动里丢了
   ` reason=`（串口出现过 `smap=1user-space-has-no-kernel-half`），已补回。

### 1.4 判据（三次启动，全部实测通过）

```
[ISO64] selftest PASS mask=63 wp=1 smep=… audited=1 violations=0 win=0 wp_leak=0 cr0=0000000080010011 cr4=…
[ISO64] wp check at gui-entry wp=1 cr0=0000000080010011
```
且日志里**没有**任何 `[ISO64] wp leak detected` 行。`iso64_test` 现在同时断言这三条
（`wp_leak=0` / `wp check … wp=1` / 无泄漏行）。

## 2. §8.1 SMAP：设计（最终落地的形态）

### 2.1 三层结构

| 层 | 位置 | 作用 |
|---|---|---|
| ① 进内核先 `CLAC` | `isr_handler64()` 开头 + `syscall64_dispatch64()` 开头（`ISO64_CLAC_ON_ENTRY()`） | 内核侧**永远从 AC=0 开始**；同时把开窗计数清零 |
| ② 显式开窗 `STAC/CLAC` | 内核里**确实要读写用户内存**的那些点（表 2.2） | 只有这些瞬间 SMAP 被放行 |
| ③ 内核态违规处置 | `iso64_smap_fault64()`，由 `isr_handler64()` 在**掉进 PANIC 之前**调用 | 绝不 PANIC 整机 |

**开窗是引用计数，不是布尔**（`g_smap_ac_depth`）。这条是踩出来的：
`e64_build_stack64` 开了一个大窗，里面调 `e64_fill_random64`（它自己也开窗）——
内层的 `CLAC` 把外层还开着的窗一起关掉，于是外层剩下的写用户页立刻 #PF
（实测 `[SMAP64] violation … e64_build_stack64+0x62`）。计数进内核时清零，跨 syscall 不累积。

**SMAP 没开（CPU 不支持 / 未置位）时 `STAC/CLAC` 是空操作**：两个包装函数先看 `g_smap`，
同一份代码在 `qemu64`（无 SMAP）与 `-cpu max`（有 SMAP）上都是对的。

### 2.2 开窗清单（本轮逐点加的，全部是"内核真的碰用户页"的地方）

| 文件 | 位置 | 干什么 |
|---|---|---|
| `syscall64.cpp` | `lx64_copy_to_user64` | read/getdents 等把内核数据写回用户 |
| `syscall64.cpp` | `lx64_copy_from_user64`（**新增**） | write 的用户缓冲 → 内核 bounce（`g_lx_writebuf64`） |
| `syscall64.cpp` | `lx64_user_str64` | 逐字节读用户字符串（**每字节一个窗**） |
| `syscall64.cpp` | `lx64_write64`（fd 1/2 与 fd≥3 两条） | 先 bounce，再输出/写盘 —— 输出侧**只碰内核内存** |
| `syscall64.cpp` | `syscall64_write64`（int 0x80 的 write） | 同上（bounce 后串口/屏幕） |
| `syscall64.cpp` | `lx64_getdents64` | 直接在用户缓冲里拼 dirent → 作用域守卫 `Iso64UserAccess64` |
| `syscall64.cpp` | `lx64_nanosleep64` | 读用户 timespec（16 B） |
| `syscall64.cpp` | `lx64_execve64` | 读用户 `argv[]` 里的指针 |
| `syscall64.cpp` | `lx64_tty_ioctl64`（TCSETS*） | 读用户 termios（36 B） |
| `syscall64.cpp` | `sc64_fb_map64` | 两个出参（VA + 几何）写回用户 |
| `sig64.cpp` | `sig64_copy_to_user64` | 信号帧/旧 sigaction 写回用户 |
| `sig64.cpp` | `sig64_sys_rt_sigaction64` / `sig64_sys_rt_sigprocmask64` / `sig64_sys_rt_sigreturn64` | 读/写用户 sigaction、sigset、信号帧 |
| `sig64.cpp` | `sig64_push_frame64` | 往用户栈上搭信号帧 + restorer 地址 |
| `elf64.cpp` | `e64_seg_map_union64`（blob / 读盘两个分支 + `.bss` 清零） | 段内容写用户页 |
| `elf64.cpp` | `e64_build_stack64` / `e64_build_stack_argv64` / `e64_fill_random64` | 初始栈（argv/auxv/AT_RANDOM） |
| `elf64.cpp` | `elf64_selftest64` | 自检**读回用户页里的映像/auxv/argv[0]** |
| `proc64.cpp` | `proc64_devmap64` / `proc64_shm_map64`（3 处） | 出参 VA 写回用户 |
| `wl64.cpp` | `wl64_composer_get64` | 面板 rect 写回用户 |

### 2.3 违规处置（产品行为）

```
[SMAP64] violation n=<k> pid=<n> cr2=<hex> rip=<hex> err=<hex> -> sig=11 (kernel touched user page without STAC)
```
- **有进程上下文**：只杀当前进程（`proc64_sig_die_current64(SIG64_SEGV, r)`），内核继续跑；
- **没有进程上下文**（内核任务 0 上走到的路径，例如自检自己读用户页）：
  **关掉 CR4.SMAP 让那条指令原地重试**，并打 `[SMAP64] smap degraded (no process context, CR4.SMAP=0 from now on)`。
  取舍理由：宁可降级并大声留痕，也不要变砖（这条路径本来就意味着"内核有 bug"）；
- **绝不 PANIC 整机**。

### 2.4 怎么把清单列全的（方法可复用）

SMAP 一旦打开，"漏了 STAC 的点"会立刻 #PF。于是加了一套**探针模式**（`ISO64_SMAP_PROBE`）：

- 违规时**不杀进程**，而是关掉 SMAP → iretq 后那条指令重试成功 → 按 PIT tick（约 0.26 s）**重新武装** SMAP；
- 于是一次启动就能收下一长串违规点（每条带 `rip`）；
- 用 `objdump -d build64/kernel64_os.elf` 把 `rip` 归位成 `符号+偏移` → 直接看源码那一行。

**收敛过程（同一台机器、`-cpu max`，每轮一次构建 + 一次启动）**：

| 轮次 | 违规行数 | 去重后的点 |
|---|---|---|
| 1 | 46 | 5（`lx64_nanosleep64` / `e64_build_stack64` / `syscall64_serial_linewise64` / `lx64_execve64` / `sig64_sys_rt_sigprocmask64`） |
| 2 | 17 | 3（`e64_build_stack64`（嵌套窗） / `sig64_sys_rt_sigreturn64` / `proc64_shm_map64`） |
| 3 | 5 | 4（`sig64_sys_rt_sigaction64` / `elf64_selftest64` / `sig64_push_frame64` / `sc64_fb_map64`） |
| 4 | 3 | 3（`elf64_selftest64` / `lx64_tty_ioctl64` / `wl64_composer_get64`） |
| 5 | 1 | 1（`elf64_selftest64+0xa5e` —— 自检读回用户页映像那一段） |
| **6（交付构建 probe=0）** | **0** | **0** |

交付构建里 `ISO64_SMAP_PROBE = 0`（探针模式**不进交付**）。

## 3. 踩坑（本批新增，都是真踩出来的）

1. **别用页错误码的 U/S 位去认 SMAP 违规**。QEMU 的 TCG 上实测给的是 `err=0x3`（`P|W`，U/S=0），
   而 Intel SDM 的描述会让人以为 U/S=1。按 SDM 写的判据在 QEMU 上一条都认不出来 →
   直接掉进 PANIC（第一轮就是这么停机的：`[PANIC] cpu exception 14 err=3 … rip=…e64_seg_map_union64`）。
   **正确判据**：CPL0 的 #PF + `AC=0` + `cr2` 确实落在"可访问的用户页"上（`user64_range_ok64(cr2,1)==1`），
   写访问再核一层"这一页本来就可写"（否则那是 `CR0.WP` 的活）。对照实验：`-cpu max` 加 `-smap`
   就正常起桌面 —— 证明那次停机确实是 SMAP 引起的。
2. **`STAC/CLAC` 必须可嵌套**（见 §2.1）。布尔标志会让内层 `CLAC` 关掉外层的窗。
3. **`iso64.cpp` 只在桌面内核里链接**（`build64.sh` 的 `SRCS_DESKTOP`），而 `x86_64.cpp`/`syscall64.cpp`
   是**共享**的 → 裸调用新符号会让基础内核/安装介质内核链接失败。修法沿用项目既有的 `weak` 口径：
   声明加 `__attribute__((weak))`，调用点全走带判空的宏 `ISO64_STAC64()/ISO64_CLAC64()/…`（没链接就是空操作）。
4. **`ELF` 装载的 `.bss` 清零与 `AT_RANDOM` 也是写用户页**（容易漏，因为看起来像"内核自己的循环"）。
5. **自检自己会读用户页**（`elf64_selftest64` 读回映像/auxv/argv[0]）—— 自检也要开窗，
   否则 `-cpu max` 下自检就是第一个 PANIC。

## 4. 验收（实跑，串行）

| 脚本 | 结果 | 说明 |
|---|---|---|
| `tests/iso64_test.py`（**48 条**，原 41） | **48/48 PASS** | 新增：WP 无泄漏判据 3 条、`-cpu max` 下 `smap_on=1`、**SMAP 零违规**、`iso` 里 `smap_violations=0`、`-cpu max` 不得 PANIC |
| `tests/version64_test.py` | **PASS 8/8** | 版本串 0.12.0；旧串 v0.11.9 在内核里 0 次 |
| `tests/micropython64_test.py` | **PASS** | REPL/脚本/argv（用户态面最大的回归）；首跑有 1 条与 MPDRV argc 打点相关的**flaky**（同镜像重跑 PASS，判据未放宽） |
| `tests/deskfolder64_test.py` | **64/74**（= v0.11.7 基线） | 10 条失败全是既有 2 项 UNVERIFIED 的连锁：①"图标拖进回收站"手势未命中 → 回收站/恢复/scan 那一串 ②窗口圆角**像素**判据。与 v0.11.7 基线逐条一致，**没有新增失败**。 |
| 构建 | `EXIT=0` | `kernel64_os.bin` **3,566,928 B** |

关键串口原证（`-cpu max,la57=off,umip=off`，交付构建 probe=0）：

```
[ISO64] hw wp=1 smep=1 smep_supported=1 smap_supported=1 smap_on=1 cr0=0000000080010011 cr4=0000000000300620 ok=1
[ISO64] mode kpti=1 smap=1 reason=user-space-has-no-kernel-half (user table PML4[511]=0; entry trampolines at 6 GiB)
[ISO64] selftest PASS mask=63 wp=1 smep=1 audited=1 violations=0 win=0 wp_leak=0 cr0=0000000080010011 cr4=0000000000300620
[ISO64] wp check at gui-entry wp=1 cr0=0000000080010011
[GUI64] ready
（全日志 `[SMAP64] violation` 0 条）
```

默认 CPU（`qemu64`，无 SMEP/SMAP）如实降级：`smap_supported=0 smap_on=0 smap=0`，
其余判据照旧全绿。

## 5. 三项复核（对上表 §0 的展开）

### 5.1 KPTI —— 已完成，本轮**没动实现**，只被回归覆盖

- 实现与 v0.11.9 逐指令相同（`kernel/kpti_blob64.asm` + `kernel/kpti64.cpp` + 双 PML4）。
- 本轮唯一相关的"顺带影响"：`iso64_init64()` 提前到 `iso64_selftest64()` 之前后，
  `[ISO64] mode kpti=1` 仍在、`audit … khigh=0 … kind=user` 仍在（48/48 里都有）。
- **仍然缺的一块（设计性缺口，未变）**：UEFI 路径 `reason=boot-cr3-not-own` → KPTI 整层不启用。
  详见 v0.11.9 交接单 §8.3（本轮没碰）。**这一条要明确：KPTI 在 UEFI 启动下不存在。**

### 5.2 SMAP —— 本轮完成

- `iso64_init64()` 里 CPUID 支持就置 `CR4.SMAP(1<<21)` 并**回读确认**（`g_smap`）。
- 全内核用户访存点显式开窗（§2.2 的 18 处），进内核统一 `CLAC`。
- 判据：`-cpu max` 全启动（含 ring3 ELF 装载、proc64 多进程演示、GUI 到桌面）
  **零 `[SMAP64] violation`**；`iso` 里 `smap_violations=0`。
- **如实边界**：本轮的验证面 = QEMU `-cpu max` 上的"启动 → 全部自检/演示 → 桌面" +
  `micropython/deskfolder` 回归。窗口粒度是**函数级**（不是 Linux 那种单指令级），
  所以"窗口里调了别的内核函数"仍然会被一起放行 —— 这是精度上的取舍，如实记在这里。

### 5.3 WP —— 本轮完成（根因是误判，见 §1）

- 判据：`wp_leak=0` + `wp check at gui-entry wp=1` + 无 `wp leak detected` 行，三次启动一致。
- 仍然保留 `iso64_reassert64()` 这个函数本身（`iso` 侧/将来真掉时救人一命用），
  但**启动期不再拿它当兜底**。

## 6. 本批**没做**的（如实点名）

1. **UEFI 路径没有 KPTI**（同 v0.11.9 §8.3，未动）。
2. **SMAP 的窗口是函数级**，不是单指令级；且只在 CPU 支持时生效（默认 `qemu64` 验不到）。
3. **没有 PCID**：KPTI 每次 `mov cr3` 都刷 TLB，性能账仍未量。
4. **缺页按需分页 / COW** 仍没有。
5. 共享入口区 `.text.kpti` 三页在共享 VA 上仍是 **RW**（可拆 RO+RW，未做）。
6. `iso` 命令仍没接进设置页；`iso64_test` 仍没纳入发版门禁脚本。

## 7. 改动文件

| 文件 | 要点 |
|---|---|
| `kernel/iso64.h` | 新增 `iso64_stac64/clac64/smap_on64/smap_violations64/smap_fault64/clac_on_entry64/tick64/check_wp64`；全部 `weak` 声明 + 带判空的 `ISO64_*` 宏 + `Iso64UserAccess64` 作用域守卫 |
| `kernel/iso64.cpp` | SMAP 置位与回读、开窗引用计数、内核态违规处置（杀进程/降级）、探针模式（`ISO64_SMAP_PROBE`，交付=0）、`wp check`、selftest 泄漏判据改 `fails \|= 1`、`mode` 行补回 ` reason=` |
| `kernel/kernel64.cpp` | `iso64_init64()` 提到自检之前且无条件执行；删分支里的重复初化；`reassert` 兜底 → `iso64_check_wp64("gui-entry")` |
| `kernel/x86_64.cpp` | `isr_handler64()` 开头 `ISO64_CLAC_ON_ENTRY()`；PIT 里 `ISO64_TICK64()`；PANIC 之前 `ISO64_SMAP_FAULT(r)` |
| `kernel/syscall64.cpp` | `lx64_copy_from_user64` + `g_lx_writebuf64` bounce；write（int 0x80 与 fd 层各一条）走 bounce；getdents 作用域守卫；8 处开窗（见 §2.2） |
| `kernel/sig64.cpp` | 4 处开窗（sigaction/procmask/sigreturn/push_frame）+ `sig64_copy_to_user64` |
| `kernel/elf64.cpp` | 装载/初始栈/自检共 6 处开窗 |
| `kernel/proc64.cpp` | devmap/shm_map 的 4 处出参写回开窗 |
| `kernel/wl64.cpp` | composer_get 的 rect 写回开窗 |
| `build64.sh` | 0.11.9 → **0.12.0** |
| `tests/iso64_test.py` | 41 → **48 条**：WP 三条判据、`smap_on` 与 CPU 支持一致、`-cpu max` 下 SMAP 零违规、`iso` 里 `smap_violations=0`、`-cpu max` 不得 PANIC |
| `tests/fs_tree_test.py` / `tests/micropython64_test.py` | 新增 `VIMTU_CPU` 环境变量支持（默认行为一个字节不变；设了才追加 `-cpu`）—— 这是"在带 SMAP 的 CPU 模型上跑回归"的夹具入口 |

## 8. 下轮建议（按价值排序）

1. **UEFI 路径的 KPTI**（v0.11.9 §8.3 的三步：自建 PML4 → 双平台验 `mov cr3` → 存活才纳入）。
2. **打开 PCID**（KPTI 的 TLB 冲刷是现在最大的性能代价），顺带把 §6.3 的性能账量出来。
3. `iso` 命令接进设置页「安全」页；`iso64_test` 纳入发版门禁。
4. SMAP 窗口细化为"单条访存"（现在函数级），并把 `drm64/wl64` 的共享内存访问也逐点核一遍
   —— 本轮它们没违规，是因为那条路径上的用户访问都走 `user64_map_page*` 之后的**内核侧**
   访问（同一张私有 PDPT），不是"用户指针"。
5. v0.11.7 遗留的两条 UNVERIFIED（拖进回收站手势、窗口圆角像素判据）。

## 9. 现场

- HEAD（本提交）+ 文档提交；`VIMTUOS_VERSION="0.12.0"`；`kernel64_os.bin` **3,566,928 B**
- 打点总表：`[ISO64] hw|mode|selftest|audit|probe|wp check|status` + `[SMAP64] violation|smap degraded`
- 复现方法：`VIMTU_CPU=max,la57=off,umip=off` + `py -3 tests/iso64_test.py`
  （或 `$PI_SCRATCH_DIR/harvest_smap.py` 直接抓一次启动的全部 SMAP 违规并归位到符号）
