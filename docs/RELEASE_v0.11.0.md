> [← 返回主页](../README.md) · VimtuOS 文档体系

# VimtuOS v0.11.0 —— 内存扩容（4GB 不再被夹成 2GB）+ MicroPython 进终端

> 本版做完了交接单 §3 的前两件大事。第三件（桌面/文件高级交互）**本版未动**，
> 见 §6「本版没做的事」—— 不含糊、不假装。

---

## 1. 内存：4GB 被夹成 2GB 的根因是**两条**缺陷叠在一起

### 1.1 取证（先证明，不猜）

`-m 4096` 起机抓串口，改动前：

```
[LM64] E820[3] base=0000000000100000 len=00000000BFEE0000 type=1     ← 1MB..3.0GB 可用
[LM64] E820[6] base=0000000100000000 len=0000000040000000 type=1     ← 4GB..5GB 可用（above-4G remap）
[LM64] E820 usable_above_1M_kb=4193152                               ← 4.0 GiB，E820 报对了
[MEM64] pool=[000000000A000000,000000008A000000) pages=524288         ← 页池只有 2.0 GiB
```

E820 报 4.0 GiB、页池只有 2.0 GiB —— 被夹在 `kernel/mem64.cpp` 的两行上。

### 1.2 缺陷一：页位图只有 64 KiB → 页池上限 2 GiB

```
kernel/mem64.cpp:55  #define MEM64_BITMAP_BYTES (64u * 1024u)      // 64K*8 = 524288 页 = 2 GiB
kernel/mem64.cpp:57  #define MEM64_POOL_MAX (...)                  // 下限 + 位图位数*4KiB
kernel/mem64.cpp:97  if (highest > MEM64_POOL_MAX) highest = MEM64_POOL_MAX;   // ← 4GB 在这里被夹
```

### 1.3 缺陷二：页池被当成"一整段连续 RAM"（旧版只是**恰好**没出事）

旧实现 `g_page_total = (pool_end - pool_base) / PAGE`。4GB 机器的 E820 是
`[1MiB, 3.0GiB) usable` + `[3.0GiB, 4GiB) PCI 空洞(MMIO)` + `[4GiB, 5GiB) usable`。
中间 1 GiB 是**设备地址空间**。旧版因为封顶在 2.16 GiB 恰好没跨到空洞，所以这个缺陷一直没暴露；
**一旦只放大位图不修它，页池跨过 3GB 后会把 MMIO 空洞当 RAM 分配出去 —— 那不是分配失败，那是往设备寄存器里写数据。**

### 1.4 修法（三处，都在 `kernel/mem64.cpp`）

1. 位图 `64 KiB -> 256 KiB`（覆盖 2,097,152 页 = 8 GiB 地址跨度）。位图仍放 `.bss`，
   `__bss_end` 从 ~50.6 MB 涨到 ~50.8 MB，离 64 MiB 载荷窗口仍有 13 MB，
   `build64.sh` 的 nm 闸门（`__bss_end < 0x04000000`）仍然成立。
2. 新增 `e820_usable_ranges64()`：**按 E820 usable 段**填充页池 —— 先把整个跨度标成"已用"，
   再只把 usable 段内的页清成空闲。`g_page_span`（跨度，含空洞）与 `g_page_total`（真可用）自此分开，
   `page_alloc_64()` / `page_alloc_2mb_64()` 扫的是**跨度**（空洞页在位图里是占用态）。
3. 新增 `MEM64_DIRECT_MAP_BYTES = ML64_IDENTITY_BYTES`（4 GiB）：页池上界再被**直映边界**夹一次。
   4GB 机器第二段可用区在 4GB 以上，内核没有那段映射（`boot/loader64.asm` 的 PDPT 只有 4 项），
   划进页池就是第一次触碰即 `#PF`。这部分单独记账、如实打串口。

### 1.5 总内存口径改了（这就是"用户看到 2GB"的直接原因）

`mem_total_ram_64()` 原来 `return g_pool_end;`（"页池上界地址"）。这在 4GB 机器上错两次：
被位图封顶、且"上界地址"在有空洞的机器上代表不了可用量。
现在返回 **E820 usable 总量（≥1MB 段求和）**，与启动日志 `usable_above_1M_kb` 完全同口径，可逐 MiB 对账。

### 1.6 新增自检（这是本次修复的核心证据，不是"数字变大了"）

`mem_selftest_64()` 第 8 项：从页池里取**地址最高**的那个空闲页（`page_alloc_highest64()`），
写入 `0xA5A5C0DE12345678` / 取反、读回校验、释放，并把物理地址打进串口：

```
[MEM64] selftest high_page pa=0xBFFDF000 rw=1 gt2g=1 req=1
```

判据分档：任何机器要求读写往返一致；`ram_usable >= 3 GiB` 的机器**额外要求 `pa > 0x80000000`（2 GiB）**。
512 MB 机器上 `req=0`，行为一字不变。

### 1.7 实测（`tests/mem4g64_test.py`，新增，28/28 PASS）

| 项 | 改动前 | 改动后 |
|---|---|---|
| `-m 4096` `[MEM64] ram total` | ≈ 2208 MiB（池上界） | **4094 MiB**（= E820 usable 4094.9 MiB，逐 MiB 对账） |
| `-m 4096` 页池 | `[0x0A000000,0x8A000000) pages=524288`（2048 MiB） | `[0x0A000000,0x100000000) pages=745440/1007616`（**2911 MiB**） |
| 池顶页物理地址 | 永远 < 2 GiB | **0xBFFDF000（3071.9 MiB）**，读写往返一致 |
| 3GB 处 PCI 空洞 | 未覆盖（因封顶恰好没跨到） | 跨度−可用 = **262176 页精确值**（1 GiB MMIO + 128 KiB E820 reserved），不当 RAM |
| 4GB 以上 remap 段 | 未记账 | 如实报 `above4g=1024 MiB`（可用但无直映，不进页池） |
| `-m 512` 页池 | `pages=90080` / `pool_end=0x1FFE0000` | **逐数字相同**（跨度 == 可用页数，无空洞） |

> 判据加强记录：交接单写"分配并读写一个 phys > 0x90000000 的页"，本测试取的是**池顶页**
> （地址最高的可用页），比"随便一个高地址页"更强 —— 池顶页正确说明位图末尾也正确。
> 另外额外要求 `ram total` 与 E820 逐 MiB 对账，防"把常量写大"这种假绿。

---

## 2. MicroPython：终端里 `>>>` 真的能算

### 2.1 交付物（全在系统卷，内核映像里**一个字节都不加**）

```
/bin/mp                 17,112 B   装载驱动 < 64 KiB，走内核主程序装载器（与 /bin/lua 同构）
/lib/micropython.bin   267,904 B   真解释器，静态 ELF64、非 PIC、钉在 4GiB+0x90000、链 vendored musl
```

为什么要驱动：内核 ELF 装载器只装"主程序"，其 `PT_LOAD` 必须完整落在用户窗口**低 64 KiB**
（`kernel/elf64.cpp` 的 `e64_parse_ex64` 会拒掉越界段）。267 KiB 的解释器直接 `run` 只会被拒。
驱动做的事与 `/bin/lua` 完全同构：读 `/lib/micropython.bin` 的程序头 → `mmap` 申请 span →
逐段搬 → 改写 auxv（AT_PHDR/AT_ENTRY…）→ 换 512 KiB 栈 → `jmp e_entry`。

构建链（`tools/micropython_build_win.sh`，7 步，**不用 upstream Makefile、不用 mpy-cross**）：
`makeversionhdr` → `makeqstrdefs.py pp`（`-DNO_QSTR`）→ split/cat → 两段 sed + `makeqstrdata.py`/
`makemoduledefs.py`/`make_root_pointers.py` → 编译 132 个 `py/*.c` + 端口层 → `ld.lld -T port/mp64.ld`
→ `tools/micropython_check_elf.py` 做构建期硬断言。两次干净构建 sha256 一致。

### 2.2 ★ 真正卡住它的不是编译，是 **stdin 根本不存在**

改动前 `kernel/fd64.cpp`：

```cpp
if (of->kind == FD64_KIND_TTY) return 0;   // ★ A4-2a：控制台 tty 没有输入流（如实 EOF）
```

`/dev/console` 的 read **无条件返回 0（EOF）**。于是把 MicroPython 装进去的第一次实测是：

```
MicroPython 8901bf220e on 2026-11-19; vimtu64-mp with x86_64
>>> [SYSCALL] insn nr=0 rdi=0 ... ret=0000000000000000
[SYSCALL] insn nr=231                     ← exit_group，一闪就没了
```

`>>> ` 打出来了、然后就退出。修法（三处）：

1. **内核侧 stdin 通道**（`kernel/terminal64.cpp`）：`TerminalState` 加每终端 512 B 的 `sinb` 环 +
   `r3_pid` 前台登记；`term_key()` 在**最前面**（在 ESC 关窗之前）把按键投进环 ——
   对交互程序来说 `0x1B` 是普通字节，不能顺手关窗；**内核不回显**（前台程序自己回显，两边都回显会变双字符）。
2. **`fd64` 的 tty read 三态**：`>0` 字节数 / `0` 有通道但暂无按键（`task_sleep_ms64` 让出等待）/
   `-1` 本终端没有前台 r3 → **保持旧语义 EOF**（弱引用，安装介质内核符号为 0 → 行为不变）。
   `cat` 这类程序必须立刻看到 EOF，不能因为"有个 stdin 通道"就假装有输入。
3. **子进程 fd 0/1/2 必须显式绑 tty**：内核建新进程的 fd 表**不继承任何槽**。旧路径下 stdout 只是靠
   "fd 层回落内核表"碰巧能打（内核表的 1 是控制台），stdin 没有这条退路。现在装载前
   `fd64_open64("/dev/tty")` 并 `fd64_dup_into64(ct, tfd, 0/1/2)`。
   并且"登记前台"提到 `proc64_start_elf64` **之前**（否则进程可能先跑到 `read(0)`，那时还没登记 → 判成 EOF）。

另外修掉一个只有真实交互才暴露的坑：`term_key` 的 r3 分支原来只放行 `>=0x20 || \n || \b || 0x7F`，
**把 `0x04`（Ctrl-D）过滤掉了** —— 实测症状是"按 Ctrl-D 终端纹丝不动、MicroPython 永远退不出来"。
现在除已单独处理的 Ctrl-C 之外全部入环。

### 2.3 实测（`tests/micropython64_test.py`，新增，13/13 PASS）

真实按键 → 真实输出（全部来自串口原文，剥掉内核打点行后）：

```
>>> 1 + 1 <CR>              ->  2
>>> print("Hello VimtuOS!") ->  Hello VimtuOS!
>>> for i in range(3):      ->  0 / 7 / 14
...     print(i*7)
... <CR>
Ctrl-D                      ->  [TERM] ring3 front exit pid=26，键盘回到内核 shell（[TERM] cmd df ok）
```

判据加强记录：
* 交接单只要求 `1+1 -> 2`。本测试额外要求 `print("Hello VimtuOS!")` 与带**缩进块**的 `for` 循环 ——
  `1+1` 只走整数加法一条路径，证明不了 `range`/缩进块/字符串字面量这些 REPL 基础设施。
* 额外从**盘上**用"文件前 64 B + 后 64 B + 全长"三要素重新验一遍卷里的两个文件（不信 packer 的自述；
  只搜 ELF 头会让任何别的 ELF 假绿）。

---

## 3. 回归（本版实测）

| 测试 | 结果 |
|---|---|
| `mem4g64_test`（新增） | **28/28 PASS** |
| `micropython64_test`（新增） | **13/13 PASS** |
| `sh64_test` | **59/59 PASS**（v0.10.10 是 58/59，那条串口行原子性 flake 本版未命中） |
| `bootlog64_test` | **32/32 PASS** |
| `term_render64_test` | **16/16 PASS**（首跑 13/16 是截图时序 flake，重跑逐位相同两轮通过） |
| `sysstate64_test` / `taskmgr64_test` | **PASS / 31/31 PASS**（内存口径改动后的直接消费方） |

---

## 4. 构建期硬闸门（本版新增/维持）

```
[LM64] kernel_image_kb / limit_kb     内核仍 <= 15 MiB（本版 3,364,064 B）
__bss_end < 0x04000000                位图放大 192 KiB 后仍然成立（~50.8 MB）
micropython_check_elf.py              驱动 < 64 KiB + 首段 off=0/va=4GiB+0x90000 + 无 INTERP/DYNAMIC
内核二进制里搜不到 mp / micropython 字节   体积纪律（只从卷装载）
```

---

## 5. 产物

```
kernel64_os.bin    3,364,064 B
kernel64.bin(安装器) 1,899,136 B
system.img/sysdisk.img  38,456,320 B
build64/mp            17,112 B   -> 卷 /bin/mp
build64/micropython.bin 267,904 B -> 卷 /lib/micropython.bin
vimtu64-64.iso    93,425,664 B  sha256 164F085F259729941E3C1E8179ECB2406A9F255B560B21E1FC1EB69B11FB1C3E
vimtu64-64.img    42,651,136 B  sha256 CD4661F72A144A9529DF020D123992CDD1BA7D993ED2C8964714717690CBEEF9
```

---

## 6. 本版没做的事（如实）

1. **桌面/文件高级交互（交接单 §3.3）本版未动**。现状仍是：右键菜单已有 6+4 项、
   「新建文件夹/重命名/删除/复制/剪切/粘贴」已实现，真正缺的是**「新建文件」**与
   **explorer→桌面 跨层拖拽**。下一版做，并配 `tests/fileops64_test.py`。
2. **4GB 以上那 1 GiB 没有进页池**。`above4g=1024 MiB` 如实记账但不可用 ——
   内核恒等映射只有 0..4GB（`ML64_IDENTITY_BYTES`），要吃它必须扩引导层映射，
   两条引导链（BIOS/UEFI）都要动、且 UEFI 侧还得让固件页表的低端映射一起覆盖，
   属于独立风险项。本版选择"如实报出来"，而不是假装用上。
3. **`-m 8192` 未实测**。位图跨度够 8 GiB，但直映边界仍夹在 4 GiB，所以 8GB 机器上
   页池同样是 2911 MiB、`above4g` 会是 4 GiB 左右 —— 未取得实测证据前不写"支持 8GB"。
4. **MicroPython 的 float 变体**（`FLOAT_IMPL=none`，216,176 B）构建可通过但**未进正式构建**，
   与 `/lib/lua.bin` 保持同一口径（默认带 float）。
5. **旧的三条未绿**（`esp_install_test` 的 PATA 时序 flake、`perm64_test` 的夹具漂移）本版未复跑，
   不声称已修。
