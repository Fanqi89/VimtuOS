# UEFI 地址空间实验报告（批次 D：运行期 CR3 + 就地挂载用户窗口）

一句话结论：**两条固件路径（QEMU+OVMF、VMware EFI）实测都是"方案 A 成功 + 方案 B 成功"**，
`mov cr3` 在 VMware EFI 下**没有**复现历史复位；实验构建仍然是**运行期**才把隔离打开，
**默认构建（不带实验宏）行为一字不变**。是否默认开启：**建议暂不默认开启**（理由见 §5）。

---

## 1. 背景（为什么会有这个实验）

* UEFI 路径（`boot/efi/uefi64.c`）**不替换固件的页表根**：内核跑在"固件当前活动的 PML4"上，
  只往里挂了一条高半区映射（`PML4[511]`）。EDK2 把这些页表页标成**只读**，所以
  `kernel/usermode64.cpp` 里"顺手把顶层打开 U/S"的写会立刻 `#PF`（实测 `err=3`、CR2 = 固件 PML4 页），
  于是 `user64_available64()` 如实返回 0、`kernel/proc64.cpp` 如实降级成
  `[PROC64] init mode=shared` + `[PROC64] cr3 isolation OFF`。
* 引导期往固件 PML4 里挂项时用的手法是**临时清 `CR0.WP`**（`boot/efi/uefi64.c` 里那段带注释的代码，
  实测"直接写会 `#PF err=0x3`，清 WP 后写入成功"）。本实验把这个手法做成可开关的**运行期**路径。
* 历史现象（批次 C 记录）：**VMware EFI 下运行期 `mov cr3` 会立刻复位**，所以当时干脆不在 UEFI 下换页表。
  本实验就是要把"到底行不行"变成实测事实，而不是沿用传闻。

## 2. 做法（`-DPROC64_UEFI_CR3_EXPERIMENT=1`，默认不编）

代码落点：`kernel/proc64.cpp` 末尾的实验段（`#if PROC64_UEFI_CR3_EXPERIMENT`）、
`kernel/usermode64.cpp` 的 WP-kludge（`user64_set_wp_kludge64`）+ 判定覆盖（`user64_force_available64`）、
调用点 `kernel/kernel64.cpp`（`gui64_run` 之前一行，实验构建才编）。

* **何时跑**：`proc64_uefi_cr3_experiment_start64()` 建一个**延迟 5 秒**的内核任务
  （`task_create64("cr3exp", ...)`）。位置在 `gui64_run()` 之前，但它先 `task_sleep64(5000)` ——
  跑的时候 `[GUI64] ready` 已经打出来了。这样即使 `mov cr3` 触发复位，串口里也已经留下
  "桌面起来了 + 实验走到哪一步"两段证据，不会把"实验失败"和"系统起不来"混在一起。
* **方案 A（就地挂载，不换 CR3）**：
  1. 取当前 CR3 → 固件活动 PML4；读 `PML4[0]` 拿到固件 PDPT；
  2. 自己分配一张 PD 页，**临时清 `CR0.WP`** → 写 `PML4[0] |= P/W/U`、`PDPT[4] = 新 PD | P/W/U`
     → 立刻恢复 `CR0.WP`；
  3. **读回校验**（读不会 `#PF`，所以这一步的失败是可判定的）；
  4. 打开 `usermode64` 的 WP-kludge（让 `u64_walk64` 的每次页表写都包在"清 WP→写→恢复"里），
     覆盖 `user64_available64()` 的缓存判定为 1；
  5. 用 `user64_run_blob64()` 跑一个**自带 ring3 探针**（`int 0x80`：`write(1,"PROC64-UEFI-EXP-RING3")`
     + `exit(2,0)`），退出码 0 才记 `stage=ok`。
* **方案 B（自带 PML4 + `mov cr3`）**：
  1. 新分配 PML4 页 + PDPT 页；把**固件 PML4 的 512 项整表复制**（内核高半区 `PML4[511]` 与
     固件留下的所有映射照抄），把 `PML4[0]` 指向的固件 PDPT 也整表复制（前 4GB 恒等映射），
     只把 `PDPT[4]` 清 0（用户窗口私有）；`PML4[0]` 改成指向我们自己的 PDPT 并打开 `P/W/U`；
  2. 打 `stage=build err=0`；
  3. 打 `stage=cr3 err=0` —— **这一行在 `mov cr3` 之前**，所以"VMware 若复位，这一行就是最后一行"；
  4. `mov cr3` → 读回 CR3 校验 → 打 `stage=enter err=0`（**能打这行 = 运行期 `mov cr3` 存活**）；
  5. 关掉 WP-kludge（自带 PML4 都是可写页，不再需要），再跑同一个 ring3 探针；成功则
     `stage=ok`、`g_isolate64 = 1`、`g_boot_cr364` 指向新 PML4，并打
     `[PROC64] cr3 isolation ON (uefi experiment B verified: per-process cr3 works here)`。
* **打点（严格照抄，测试按它 grep）**：
  ```
  [PROC64] uefi exp task started id=<n> (runs 5s after boot, after GUI ready)
  [PROC64] uefi exp begin (A: attach into firmware PML4, B: own PML4 + mov cr3)
  [PROC64] uefi exp A stage=<map|enter|ok|fail> err=<hex>          （16 位大写 hex）
  [PROC64] uefi exp B stage=<build|cr3|enter|ok|fail> err=<hex>
  [PROC64] uefi exp result=A|B|none mode=isolated|shared
  ```
* 复现命令：
  ```bash
  VIMTU_EXTRA_CXXFLAGS=-DPROC64_UEFI_CR3_EXPERIMENT=1 bash build64.sh
  cp build64/kernel64_os.bin build64/kernel64_os_cr3exp.bin
  py -3 tests/uefi_cr3_experiment_test.py            # QEMU+OVMF 与 VMware EFI 各跑一遍
  ```

## 3. 实测结果（两条固件路径，同一块夹具盘、同一个实验内核）

夹具：ESP（`make_esp.py`：`BOOTX64.EFI` + **实验内核** + `SYSTEM.IMG` + `UEFI64.BIN`）+ 主分区（VimtuFS2），
与 `tests/proc64_test.py --no-uefi` 的 UEFI 夹具同一套布局。

### 3.1 QEMU + OVMF（`C:\Program Files\qemu\share\edk2-x86_64-code.fd`，i440fx + 两个 pflash 卷）

关键串口原文（`build64/cr3exp` 夹具，`tests/uefi_cr3_experiment_test.py --qemu-only`）：

```
[USER64] user window NOT available (boot paging is firmware-owned, cr3=0x000000001F801000)
[PROC64] init mode=shared cr3=000000001F801000 proc_max=16 fork_max_pages=256
[PROC64] cr3 isolation OFF (boot paging is firmware-owned, check=3): shared address space mode -> ...
[PROC64] uefi exp task started id=208 (runs 5s after boot, after GUI ready)
[GUI64] ready
[PROC64] uefi exp begin (A: attach into firmware PML4, B: own PML4 + mov cr3)
[PROC64] uefi exp A stage=map err=0000000000000000
[PROC64] uefi exp A stage=enter err=0000000000000000
[USER64] enter ring3 entry=0000000100000000 rsp=0000000100013FF0
[USER64] back to kernel (ring0)
[USER64] demo done name=cr3expA rc=0
[PROC64] uefi exp A stage=ok err=0000000000000000
[PROC64] uefi exp B stage=build err=0000000000000000
[PROC64] uefi exp B stage=cr3 err=0000000000000000
[PROC64] uefi exp B stage=enter err=0000000000000000
[USER64] enter ring3 entry=0000000100000000 rsp=0000000100013FF0
[USER64] back to kernel (ring0)
[USER64] demo done name=cr3expB rc=0
[PROC64] uefi exp B stage=ok err=0000000000000000
[PROC64] cr3 isolation ON (uefi experiment B verified: per-process cr3 works here)
[PROC64] uefi exp result=B mode=isolated
```

（ring3 探针自己写的 `PROC64-UEFI-EXP-RING3` 也在串口里出现过 —— A/B 各一次。）

**结论：A 与 B 都成功；运行期 `mov cr3` 在 OVMF 下可用；桌面照常起；无 PANIC/三重故障。**

### 3.2 VMware EFI（UEFI 固件，独立 vmx：`C:\Users\fanqi\Desktop\新建文件夹\v64-uefi-test\vimtu64-cr3exp.vmx`）

同一块夹具盘，串口 `serial-cr3exp.log`（`tests/uefi_cr3_experiment_test.py --vmware-only`）：

```
[USER64] user window NOT available (boot paging is firmware-owned, cr3=0x000000001FF97000)
[PROC64] init mode=shared cr3=000000001FF97000 proc_max=16 fork_max_pages=256
[PROC64] cr3 isolation OFF (boot paging is firmware-owned, check=3): shared address space mode -> ...
[PROC64] uefi exp task started id=208 (runs 5s after boot, after GUI ready)
[GUI64] ready
[PROC64] uefi exp begin (A: attach into firmware PML4, B: own PML4 + mov cr3)
[PROC64] uefi exp A stage=map err=0000000000000000
[PROC64] uefi exp A stage=enter err=0000000000000000
[USER64] enter ring3 entry=0000000100000000 rsp=0000000100013FF0
PROC64-UEFI-EXP-RING3
[USER64] demo done name=cr3expA rc=0
[PROC64] uefi exp A stage=ok err=0000000000000000
[PROC64] uefi exp B stage=build err=0000000000000000
[PROC64] uefi exp B stage=cr3 err=0000000000000000
[PROC64] uefi exp B stage=enter err=0000000000000000
[USER64] enter ring3 entry=0000000100000000 rsp=0000000100013FF0
PROC64-UEFI-EXP-RING3
[USER64] demo done name=cr3expB rc=0
[PROC64] uefi exp B stage=ok err=0000000000000000
[PROC64] cr3 isolation ON (uefi experiment B verified: per-process cr3 works here)
[PROC64] uefi exp result=B mode=isolated
```

**结论：VMware EFI 下 `mov cr3` 也没有复位 —— `stage=enter` 打出来了，随后 ring3 探针打印出
`PROC64-UEFI-EXP-RING3` 并正常返回（`rc=0`）。**

### 3.3 与历史现象的关系（如实说明，不硬解释）

* 历史记录是"**引导期**用自己新建的页表（1GB 大页 / 与固件不同的构造）执行 `mov cr3` 后立刻复位"，
  当时的应对就是**放弃换页表**、改成往固件 PML4 里挂一项（也就是现在 UEFI 路径的做法）。
* 本次实测的是"**运行期**把 CR3 换到**一份复制出来的 PML4**（4KB 页表链、原样复制固件的全部项）"，
  两条固件路径都存活。两者不是同一个动作（时机、页表内容、映射粒度都不同），
  所以**不能**据此反推"当年那次复位的根因是 VMware 的固件行为还是我们的页表构造"，
  本报告只陈述两次实测各自的事实。
* 没做到的部分：本批次**没有**验证"UEFI 下打开隔离之后，`fork`/`execve`/`wait4` 整条多进程链也能跑"
  （实验是在桌面起来之后才翻转的，启动期的 ring3 演示按 `mode=shared` 的既有逻辑跳过了）。
  要把它做成"UEFI 也能多进程"，需要把 A/B 提前到启动期演示之前，并重跑全套 ring3/UEFI 回归 —— 见 §5。

## 4. "清 CR0.WP 挂页表项"这个手法的安全性（必须写清）

* **它做了什么**：`CR0.WP(bit16)` = 1 时，ring0 写"只读页"也会 `#PF`；清 0 后 ring0 就能写只读页。
  EDK2 把固件页表页标成只读，所以"往固件 PML4/PDPT 里挂项"必须走这个窗口。
* **窗口有多长**：`kernel/usermode64.cpp` 的 `u64_wp_enter64/u64_wp_leave64`（实验专用）与
  `kernel/proc64.cpp` 的 `p64_wp_off64/p64_wp_on64` 都只包**一条 8 字节的页表项写**，
  写完立刻恢复原来的 CR0（保存的是**完整 CR0**，不是只翻 WP 位，避免把别的位改坏）。
  默认构建里这些函数是**空操作**（`#else` 分支），零开销、零语义变化。
* **残余风险（如实写）**：窗口内**没有关中断**（没有 `cli`）。如果恰好有中断在这个窗口里发生，
  ISR 也运行在 `WP=0` 下 —— 本内核的 ISR/异常路径只写可写内存与自己的数据结构，
  实测（两条固件路径各一整轮、每次页表写都走这条路）没有出现异常；但如果将来有人往
  "只读页"上写东西，这个窗口会把那种笔误从"立刻 #PF"变成"静默写坏"。
  要收得更紧，做法是 `cli; 清 WP; 写; 恢复; sti`（或在窗口里只用一条 `mov` 指令），
  本批次为了与 `boot/efi/uefi64.c` 的既有手法保持完全一致，**没有**加 `cli`。
* **另外两条边界**：① 窗口只存在于**实验构建**里（`user64_set_wp_kludge64(1)` 只在方案 A 里开，
  方案 B 一开始就把它关掉，因为那时页表已经是我们的可写页）；② 我们**不改固件页表页的权限位**，
  只是临时绕过 `CR0.WP` —— 复位/关机后固件看到的页表与它自己留下的一致。

## 5. 结论与建议（默认开不开）

1. **默认构建保持关闭**（宏不开 + 即使开了实验宏，也要等运行期 A/B 都通过才把 `mode` 从
   `shared` 改成 `isolated`）。理由：
   * 本批次只验证了"能切 CR3 + 能进 ring3"，**没有**验证 UEFI 下的 `fork/execve/wait4/pipe`
     整条多进程链（那是 `mode=isolated` 真正的收益面）；
   * 隔离一旦打开，`proc64` 会为每个进程新建 PML4（从 `g_boot_cr364` 复制），
     这些表在 UEFI 下是否与固件留下的所有映射（ACPI/RT 服务窗口、LFB 直映、IOMMU 相关）
     完全兼容，本批次没有做全覆盖回归；
   * OVMF 与 VMware EFI 都通过，但**真机 UEFI**（尤其是 Secure Boot/不同固件实现）没有验证。
2. **如果将来要默认开启**，建议的最小路径：
   * 只走**方案 B**（自带 PML4）—— 它不依赖"写固件页表"，也不需要 WP-kludge：
     把 A/B 实验段抽成 `proc64_uefi_try_isolate64()`，在 `proc64_init64()` 里
     **桌面启动之前**调用一次（此时还没有 ring3 演示/demo 依赖 `mode`）；
   * 通过后把 `g_boot_cr364` 更新为自带 PML4，并让 `task64_kernel_cr364()` 也能切到它
     （现在的 `mm_cr3 == 0` 仍然回到固件 PML4 —— 这一条要一起改，否则进程/内核任务会在两套
     页表之间来回切）；
   * 重跑：`tests/proc64_test.py`（含 UEFI 阶段）、`tests/fd64_test.py`（ring3 pipe/fork）、
     `tests/elf64_test.py`、`tests/tmgr_proc_test.py` 全绿之后再默认开。
3. **无论开不开，这份实验代码都可以留着**：默认不编、不占体积、不影响任何既有断言；
   要复现"UEFI 下到底能不能换页表"时它是唯一的一手证据来源。
