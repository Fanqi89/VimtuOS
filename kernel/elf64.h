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
//       [rsp] argc=1, argv[0]="/hello.elf", NULL, NULL(envp), auxv..., AT_NULL/0
//       auxv 至少含 AT_PAGESZ(6)/AT_PHDR(3)/AT_PHNUM(4)/AT_ENTRY(9)/AT_UID/EUID/GID/EGID(11..14)
//       rsp 16 字节对齐（进入 _start 时 rsp%16==0）。
//   * 进 ring3 走 user64_enter_at64（usermode64.cpp）：TSS.rsp0 + syscall 栈顶镜像切好、抬栈 iretq。
//   * 跑完（exit）回收：逐页解除映射 + page_free_64；中间页表页保留（理由同 usermode64.cpp）。
//
// 串口打点（自动验收 tests/elf64_test.py grep，格式勿改）：
//   [ELF64] install ok path=/hello.elf bytes=<n>   / install skipped (exists) /hello.elf size=<n>
//   [ELF64] load path=<p> entry=<hex> phnum=<n> segs=<n> size=<n>
//   [ELF64] enter ring3 entry=<hex> rsp=<hex>      [ELF64] back to kernel (ring0) rc=<n>
//   [ELF64] launch ok rc=<n> path=<p>              [ELF64] launch FAILED path=<p> reason=<r>
//   [ELF64] reject reason=<r> / [ELF64] reject segment va=<hex> reason=<r>
//   [ELF64] selftest PASS / [ELF64] selftest FAIL mask=<n>
//   [ELF64] selftest reject sample=<n> reason=<r>   （自检里的坏样本，故意与上面的 [ELF64] reject 区分）
#pragma once
#include <stdint.h>

// 单次读盘/解析的文件上限（缓冲在 elf64.cpp 的 .bss 里，不进内核镜像）。
// 注意：装载区的上界是 USER64_STACK_VA64（窗口低 64KiB），所以能装的映像远小于这个数。
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
