// elf64.h - ELF64 加载器（把磁盘上的静态 ELF64 装进用户窗口、建初始栈、进 ring3）
//
// ============================ 设计（一页纸）============================
// 目标：VimtuOS 能"加载并运行 ELF64 可执行文件"，并且用 **syscall 指令**与内核通信
// （即 Linux x86_64 ABI 的地基）。这里只做 **静态 ELF64**（ET_EXEC/ET_DYN，无 PT_INTERP、
// 无重定位）：自有程序用 `ld.lld -nostdlib -static -e _start -Ttext=<用户窗口内地址>` 链接。
//
// 装载策略（逐条对应 elf64.cpp 的实现）：
//   * 校验：\x7fELF / class=2 / data=1(小端) / version=1 / machine=0x3E / type=ET_EXEC(2)|ET_DYN(3) /
//     e_phentsize=56 / 程序头表整段落在文件内（e_phoff + e_phnum*56 <= 文件长度）。
//   * 每个 PT_LOAD：
//       - 目标 [p_vaddr, p_vaddr+p_memsz) 必须完整落在**用户窗口**（usermode64.h 的 4GiB..4GiB+1MiB）
//         的 ELF 装载区（窗口低 64KiB，即 4GiB..USER64_STACK_VA64）；
//       - p_filesz 字节从**文件**拷到 p_vaddr，p_filesz..p_memsz 清零（.bss）；
//       - 权限按 p_flags 给：PF_R→P|U、PF_W→+W、PF_X→不加 NX（不可执行段加 PTE_NX_64）。
//         实现上先统一按 P|W|U 映射（否则只读段没法写内容），拷/清零完成后再逐页收紧。
//   * 用户栈：USER64_STACK_VA64 起 16KiB（4 页，P|U|W|NX），按 SysV ABI 压初始栈：
//       [rsp] argc=1, argv[0]="/hello.elf", NULL, NULL(envp), auxv…, AT_NULL/0
//       ★ A3：auxv 逐条给全（号与 Linux elf.h 一致，缺一条 musl 启动期就会读崩）：
//       AT_PAGESZ(6)/AT_PHDR(3)/AT_PHENT(4)/AT_PHNUM(5)/AT_BASE(7)/AT_ENTRY(9)/AT_HWCAP(16)/
//       AT_UID(11)/AT_EUID(12)/AT_GID(13)/AT_EGID(14)/AT_SECURE(23)/AT_RANDOM(25)（16 字节
//       随机放在初始栈上、指针数组上方 —— 见 elf64.cpp 的 auxv 段）。
//       （历史错误：AT_PHNUM 曾是 4，与 AT_PHENT 撞号；musl 会按 AT_PHENT 步进遍历程序头表，
//         号错了必崩 —— 已修，并在 elf64_selftest64 的 bit2 里逐条核对。）
//       rsp 16 字节对齐（进入 _start 时 rsp%16==0）。
//   * 进 ring3 走 user64_enter_at64（usermode64.cpp）：TSS.rsp0 + syscall 栈顶镜像切好、抬栈 iretq。
//     execve 路径走 proc64_start_elf64（kernel/proc64.cpp）：它有自己的 CR3/任务，见 proc64.h。
//   * 跑完（exit）回收：逐页解除映射 + page_free_64；中间页表页保留（理由同 usermode64.cpp）。
//
// 串口打点（自动验收 tests/elf64_test.py、tests/musl64_test.py grep，格式勿改）：
//   [ELF64] install ok path=<p> bytes=<n>   / install skipped (exists) <p> size=<n>
//   [ELF64] load path=<p> entry=<hex> phnum=<n> segs=<n> size=<n> [rsp=<hex>]  （execve 路径多 size/rsp/via=execve）
//   [ELF64] auxv phdr=<hex> phent=<n> phnum=<n> base=<hex> entry=<hex> random=<hex> secure=<n> pagesz=<n> rsp=<hex>
//   [ELF64] enter ring3 entry=<hex> rsp=<hex>      [ELF64] back to kernel (ring0) rc=<n>
//   [ELF64] launch ok rc=<n> path=<p>              [ELF64] launch FAILED path=<p> reason=<r>
//   [ELF64] reject reason=<r> / [ELF64] reject segment va=<hex> reason=<r>
//   [ELF64] selftest PASS / [ELF64] selftest FAIL mask=<n>
//   [ELF64] selftest reject sample=<n> reason=<r>   （自检里的坏样本，故意与上面的 [ELF64] reject 区分）
// ==================== ★ A3 下半：动态链接（PT_INTERP）====================
// 判据与分工（谁做什么，别混）：
//   * **内核**：只做"装载"—— 主程序有 PT_INTERP 时，先把**解释器**（我们自己写的 ld.so，
//     或 musl 的 ld-musl-x86_64.so.1）从 VimtuFS2 读出来、按它自己的链接地址映射进用户
//     窗口，再把主程序装到它自己的地址；auxv 如实给出：
//       AT_BASE  = 解释器的加载基址（没有解释器 = 0）
//       AT_ENTRY = **主程序**的入口
//       AT_PHDR  = **主程序**的程序头表在映像里的地址
//     控制权交给**解释器入口**（不是主程序入口）。
//   * **解释器（ring3）**：处理 PT_DYNAMIC（RELA/JMPREL/SYMTAB/STRTAB/HASH/NEEDED）、做重定位、
//     按 DT_NEEDED 打开 .so（用 mmap/mprotect 自己映射）、调用 DT_INIT/DT_INIT_ARRAY，
//     最后跳到 AT_ENTRY。内核不认识 DT_*，也不做任何重定位。
// 解释器从哪来（两条路都要能被内核装载）：
//   1) PT_INTERP 是**绝对路径**（Linux 惯例 "/lib/…"）—— 原样进 VFS 打开；
//   2) 不是绝对路径（例如 "ldvimtu.so"）—— 按 **/lib/<name>** 搜索（拼成绝对路径再打开）。
//   两条路都失败 = interp reject reason=vfs + 主程序拒绝（reason=interp）。
// 解释器映射到哪（策略，为什么要这样）：
//   解释器是 ET_DYN（p_vaddr 从 0 起、位置无关，靠 AT_BASE + R_X86_64_RELATIVE 自定位），
//   内核把它**钉在用户窗口顶部**：
//       interp_base = (4GiB + 1MiB) - align_up(解释器 span, 4KiB)
//   于是解释器自己就能算出"库区" = [USER64_MMAP_VA64, AT_BASE)（它要 mmap 的 .so 放在
//   自己下方，向上不越过 AT_BASE、向下不撞 brk/mmap 起点）。span 放不进
//   [USER64_MMAP_VA64, 窗口顶] 就拒绝（reason=interp-window）。
// 打点（自动验收 tests/dynlink64_test.py grep，格式勿改）：
//   [ELF64] interp path=<p> base=<hex> entry=<hex> span=<n>
//   [ELF64] interp reject path=<p> reason=<vfs|bad|window|arg|size|interp>
//   （静态路径的既有打点一字不改：musl64_test 的断言依赖它们。）
#pragma once
#include <stdint.h>

// ---- 三项上限的**归属**（批次 M 写清；不要把三者混成一个数）----
//   1) **FS 侧（文件总长）**：VimtuFS2 单文件上限 = **8 MiB**（`VFS64_MAX_FILE_BYTES`，
//      v3 卷 = 4 直接块 + 一级间接 + 二级间接；v2 旧卷仍是 67584 B）—— 盘上能放多大的 ELF。
//   2) **用户窗口**：装载区 = 用户窗口低 64 KiB（usermode64.h 的 4GiB..USER64_STACK_VA64），
//      所以**能真正装进去跑**的映像远小于 8 MiB —— 这是本节唯一会让人"以为文件上限就是它"的数字：
//      实际可用 ≈ 64 KiB 减去栈/对齐，**与文件系统无关**。
//   3) **读盘缓冲**：`ELF64_MAX_FILE_BYTES64`（96 KiB，.bss 里）是一次读盘/解析的硬上限；
//      它大于用户窗口、小于 FS 上限 —— 大于它的文件直接 `reason=size` 拒绝（不截断）。
//      ★ 刻意**不**把它抬到 8 MiB：缓冲在 .bss 里，抬上去只会白吃内核内存，
//      而真正卡住装载的是第 2 条（用户窗口）。
#define ELF64_MAX_FILE_BYTES64 (96u * 1024u)
#define ELF64_MAX_PHDR64       16u

// ---- 装载 / 运行 ----
// 读盘 -> 解析校验 -> 段映射进用户窗口 -> 建初始栈 -> 打印 [ELF64] load ...
// 成功返回 0（*out_entry = e_entry，镜像**留在**用户窗口里，必须接着 elf64_run_loaded64()
// 或 elf64_unload64() 收尾）；失败返回 -1（已打印 [ELF64] reject ...）。
int  elf64_load64(const char* path, uint64_t* out_entry);
// 进 ring3 跑**已装载**的镜像并回收；返回用户 exit 的退出码（0 = 正常退出），-1 = 失败/没装载。
int  elf64_run_loaded64();
// 只回收（装载后不想跑就调它）。幂等。
int  elf64_unload64();
// = load + run_loaded（启动路径与终端 elfrun 用这个）
int  elf64_run64(const char* path);

// 幂等安装：把内嵌的 hello.elf（build64/hello.elf，objcopy 嵌进系统内核）装成 VFS 的
// "/hello.elf"。已存在就跳过。返回 0 = 成功/已存在，-1 = 失败（已打印 reason=<mount|write|blob>）。
int  elf64_install_builtin64(int drive, uint32_t part_lba);

// 魔数嗅探（给 app64_run_any64 按文件头自动分派 VAP64 / ELF64 用）：1 = 像 ELF64。
int  elf64_is_elf64(const uint8_t* p, uint32_t n);

// 自检（位掩码，0 = 全过）：合法 ELF 的解析/装载/权限位/初始栈 + 若干坏样本必须被拒。
int  elf64_selftest64();

// ==================== execve 复用（批次 C：进程换映像）====================
// 为什么单独开一个入口（而不是直接调 elf64_load64）：execve 有三处不同：
//   1) 装载目标是**当前进程已经在用的**地址空间（自己的 CR3 已经装载）——调用方先把旧映像
//      整个用户区释放掉，然后调本函数；所以这里不再检查 g_loaded64.used（那份记账是单份的）；
//   2) 初始栈要按调用方给的 argv 建（elf64_load64 固定 argc=1 且字符串位置被自检断言钉住，
//      所以另写一份布局，见 elf64.cpp 的 e64_build_stack_argv64）；
//   3) 装载完成后**不**保留"已装载"记账（调用方用自己的进程记账管理生命周期）。
// 成功返回 0，*out_entry / *out_rsp 有效（映像留在窗口里，调用方负责把它跑起来/回收）；
// 失败返回 -1（已打印 `[ELF64] exec reject ...`，**不改**任何调用方状态）。
int elf64_load_for_exec64(const char* path, const char* const* argv, uint32_t argc,
                          uint64_t* out_entry, uint64_t* out_rsp);
// 清掉"已装载"记账（多进程/execve 场景：那份记账是全局单份的，跨进程只对同一 VA 布局有意义）
void elf64_forget64();
// 一段内存里的映像是否可解析（proc64 内置 /proc64.elf 的自检用；1 = 可解析）
int elf64_blob_ok64(const uint8_t* p, uint32_t n);

// ★ A3 下半：最近一次装载的解释器基址（AT_BASE 的那份值；0 = 没有解释器/静态装载）。
// 用途：日志与自检对照（elf64.cpp 的 [ELF64] interp 行与 auxv 行打印同一个值）。
uint64_t elf64_interp_base64();
