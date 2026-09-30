// kernel64.cpp - Vimtu64 纯 64 位（长模式）内核入口与 M0 自检
//
// 与 kernel/kernel.cpp 的关系：
//   kernel.cpp 是 32 位内核的入口（32 位构建专用，内含 32 位内联汇编与 32 位中断帧假设）。
//   本文件是 **64 位构建专用入口**，从零开始验证长模式引导链，不引用任何 32 位专用代码。
//   两者分别由 build.sh（32 位）与 build64.sh（64 位）编译，互不影响：
//   32 位构建不会看到这个文件，64 位构建也不编译 kernel.cpp。
//
// 阶段目标（M0）：证明"BIOS -> loader -> 长模式 -> 64 位 C++ 代码"这段链路成立，
//   并且能正确读到 loader 写的 BootInfo（VBE 模式、LFB 地址、E820 表）。
//   本阶段不建立 IDT、不开中断、不跑 GUI —— 那些是 M1 的工作，必须建立在 M0 全绿之上。
//
// 自动断言（由 tests/boot64_assert.py 读串口日志判定）：
//   [LM64] ENTERED LONG MODE   长模式成立
//   [LM64] BOOTINFO OK
//   [LM64] LFB ...             图形模式参数可读
#include "fb.h"
#include "font.h"
#include "input.h"
// ---- 硬件清单 / ACPI 平台表 / VimtuFS2 文件系统：两份内核都链接（只读探测 + 内存假盘自检）----
#include "hwinfo64.h"   // CPUID + PCI 只读枚举
#include "acpi64.h"     // RSDP -> RSDT/XSDT -> FADT/MADT 只读解析
#include "edid64.h"     // 显示器 EDID（引导层已落在 0x7600）只读解析：厂商/名字/首选时序/刷新率
#include "vfs64.h"      // 真文件系统 VimtuFS2（v3 目录树；安装程序格式化分区要用）
#include "drive64.h"    // 盘符/驱动器枚举层（C: = 系统卷；只读扫描，见 os_boot_path）
#include "fs64.h"       // ★ 批次 K：统一文件系统分派层（VimtuFS2 读写 / FAT32 只读）
#include "ata64.h"      // ATA：IRQ14 中断驱动等待（超时回退 PIO 轮询），系统内核也要初始化
#ifndef VIMTU_INSTALLER_MEDIA
#include "store64.h"    // 设置持久化 store：只有系统内核链接它（安装程序不链 store64.cpp）
#endif
#ifdef VIMTU_INSTALLER_MEDIA
#include "setup64.h"          // 只有安装介质的内核需要安装界面
#endif

#ifndef VIMTU_INSTALLER_MEDIA
#include "gui64.h"      // 64 位桌面外壳（只有"系统内核"链接它；安装程序内核走向导）
#endif
#include "task64.h"     // 调度器（同样只进系统内核：安装程序内核不链它）
#ifndef VIMTU_INSTALLER_MEDIA
#include "usermode64.h" // 用户态（ring3）：建用户页 + 进/出 ring3（只在系统内核路径里调用）
#include "display64.h"  // 运行期显示层：模式清单 + 0x3DA 实测刷新率 + EDID 对比（只进系统内核）
#include "fd64.h"       // 小 FD 层（终端文件命令 + ring3 open/read/close 的公共底座）
#include "syscall64.h"  // int 0x80 系统调用分发（两份内核都链接实现，安装程序不调用）
#include "app64.h"      // VAP64 可安装应用：安装器 + 启动器（只进系统内核，见 os_boot_path）
#include "elf64.h"      // ELF64 加载器（自有静态 ELF64 程序 + syscall 指令路径；只进系统内核）
#include "net64.h"      // 网络：e1000 驱动 + ARP/ICMP（只进系统内核；启动链里跑一次探测）
#include "proc64.h"      // 进程/地址空间（批次 C：每进程 CR3 + fork/execve/wait4；只进系统内核）
#include "usb64.h"      // USB 主机：UHCI + HID 引导键盘（只进系统内核；按键注入 PS/2 同一队列）
#include "apic64.h"    // LAPIC + IOAPIC 接管中断路由（只进系统内核；拿不到就留在 PIC）
#include "smp64.h"     // SMP：启动 AP（INIT-SIPI-SIPI + 低端跳板；只进系统内核）
// ---- 本轮移植的四个子系统（都只进系统内核，见 build64.sh 的 SRCS_OS）----
#include "config64.h"    // 系统配置：类型化 KV，落在 store64 上（真落盘）
#include "session64.h"   // 会话/应用内容策略（关窗清状态、退出保存、启动恢复）
#include "sysstate64.h"  // 运行状态机 + 模块注册表 + 健康报告 + ring log
#include "panic64.h"     // 蓝屏（BSOD）+ 看门狗（gui64 帧心跳）
// ---- ★ 本批（P1c）：多用户骨架（用户表/口令哈希/会话身份）----
//   必须在 vfs64 挂载系统卷 + config64_init64 之后 init（见 os_boot_path 的调用点）。
#include "userdb64.h"
// ---- 批次 A 后半的两个子系统（同样只进系统内核；os_boot_path 里注册 + 跑）----
#include "preload64.h"   // 字形/图标预热（进桌面之前跑一轮，带 rdtsc64 实测证据）
// ---- Intel HDA 声卡（kernel/hda64.cpp；只进系统内核，与 net64/usb64 同一批）----
//   class 0x0403 + BAR0 MMIO + CORB/RIRB + AFG->DAC->Pin + SD0 播放流；全程轮询。
//   没声卡只打一行 [HDA64] not found，系统照常启动（面板/设置页按 ready 如实降级）。
#include "hda64.h"
#include "update64.h"    // update 子系统（标记 -> 应用 -> store/重启；不是真"升级包"，见其头文件）
// ---- 文件资源管理器 / 此电脑（只进系统内核；外壳 gui64 的 app_mypc_open64 转调它）----
#include "explorer64.h"  // "此电脑"+盘内浏览：纯逻辑自检在启动期跑一次（[EXPL] selftest PASS）
// 用户态演示程序 blob：user/demo64.asm -> nasm 平铺二进制 -> objcopy 嵌入（见 build64.sh）。
// 符号名由 objcopy 按输入路径生成：_binary_build64_user_demo64_bin_start/_end。
extern "C" const uint8_t _binary_build64_user_demo64_bin_start[];
extern "C" const uint8_t _binary_build64_user_demo64_bin_end[];
// ★ A1：用户态绘图演示程序 blob（user/fbdemo.asm -> nasm -f bin -> objcopy 嵌入；见 build64.sh）。
//   符号名同样由 objcopy 按输入路径生成：_binary_build64_user_fbdemo64_bin_start/_end。
extern "C" const uint8_t _binary_build64_user_fbdemo64_bin_start[];
extern "C" const uint8_t _binary_build64_user_fbdemo64_bin_end[];
// ★ A2：**用 C 写**的用户程序 blob（user/apps/*.c -> user/build_user.sh 编成静态 ELF64 ->
//   objcopy 平铺 blob 嵌进内核；见 build64.sh 的"用户程序交叉编译"段）。
//   跑它们的入口是 user64_run_capp64()（代码页可写：C 程序有 .data/.bss，见 kernel/usermode64.cpp）。
//   符号名同样由 objcopy 按输入路径生成：_binary_build64_user_<name>_bin_start/_end。
extern "C" const uint8_t _binary_build64_user_hello_bin_start[];
extern "C" const uint8_t _binary_build64_user_hello_bin_end[];
extern "C" const uint8_t _binary_build64_user_libctest_bin_start[];
extern "C" const uint8_t _binary_build64_user_libctest_bin_end[];
extern "C" const uint8_t _binary_build64_user_fbdemo_bin_start[];
extern "C" const uint8_t _binary_build64_user_fbdemo_bin_end[];
// ★ A5 前置：/evshm.elf（user/apps/evshm_demo.c 编出的静态 ELF64，由 proc64.cpp 幂等装进
//   系统卷再以真进程跑）。符号名由 objcopy 按输入路径生成：_binary_build64_evshm_elf_start/_end。
extern "C" const uint8_t _binary_build64_evshm_elf_start[];
extern "C" const uint8_t _binary_build64_evshm_elf_end[];
// ★ A2：跑哪个 fbdemo —— 0（默认）= C 版（user/apps/fbdemo.c）；1 = A1 的汇编版（user/fbdemo.asm）。
//   由 build64.sh 按环境变量 VIMTU_USER_FBDEMO 定义（默认 c；=asm 时切回汇编版，二进制行为一致）。
#ifndef VIMTU_USER_FBDEMO_ASM
#define VIMTU_USER_FBDEMO_ASM 0
#endif
#endif

#include <stdint.h>
#include <stddef.h>

#include "../bootinfo.h"
#include "debug64.h"
#include "memlayout64.h"

#include "console64.h"   // ★ 批次 N：开机滚屏引导控制台（启动日志环形缓冲 + 回放 + dmesg）
#include "rust64.h"      // ★ 项目路线（C + C++ + Rust）：Rust 模块 gui_rs 的 C 链接声明
                         //   （设计 Token + 主题配色；只有**系统内核**链接它，见 build64.sh）
#include "hwui64.h"      // ★ item 5a：屏幕硬件检查报告（**两份内核都编**：安装介质与系统内核共用）
// ---- 平台层（64 位）：PIC/PIT/IDT/TSS/RTC，实现在 kernel/x86_64.cpp ----
#include "x86_64.h"
// ---- 图形栈与输入（与 32 位共用同一份源文件：只依赖 port.h / fb.h 等纯 IO 接口）----
#include "fb.h"
#include "font.h"
#include "input.h"
// linker64.ld 提供
// ---- 64 位内存管理：物理页池 + 内核堆 + 编译器辅助例程（kernel/mem64.cpp）----
#include "mem_64.h"
extern "C" char __bss_start[];
extern "C" char __bss_end[];

static void bss_clear_64() {
    volatile uint64_t* p = (volatile uint64_t*)__bss_start;
    volatile uint64_t* e = (volatile uint64_t*)__bss_end;
    // ★ 进度打点：搬高半区之后，VMware EFI 下曾在"清 BSS"阶段复位（OVMF 正常）。
    //   BSS 有 36MB（含 fb 的 33MB 后备缓冲），每 8MB 打一个点，
    //   这样能区分"清到哪一片出问题"和"一开始就崩"。
    while (p < e) {
        *p++ = 0;
        if (((uint64_t)(uintptr_t)p & 0x7FFFFFULL) == 0) dbg64_putc('.');
    }
    dbg64_str(" |");
}

// 停机（IDT 尚未建立，只能 hlt + 空转）
[[noreturn]] static void halt_forever(const char* reason) {
    dbg64_str("[LM64] HALT: ");
    dbg64_str(reason);
    dbg64_nl();
    for (;;) { __asm__ volatile("cli; hlt"); }
}

static void dump_e820(const BootInfo* bi) {
    if (bi->mem_entries == 0 || bi->mem_map_addr == 0) {
        dbg64_str("[LM64] E820 NONE");
        dbg64_nl();
        return;
    }
    const E820Entry* e = (const E820Entry*)(uintptr_t)bi->mem_map_addr;
    uint32_t n = bi->mem_entries;
    if (n > 64) n = 64;

    uint64_t usable = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t base = ((uint64_t)e[i].base_high << 32) | e[i].base_low;
        uint64_t len  = ((uint64_t)e[i].len_high  << 32) | e[i].len_low;
        if (e[i].type == 1) {
            // 只累计 1MB 以上的可用区（低端 640KB 保留给引导链/BIOS 数据）
            uint64_t top = base + len;
            uint64_t lo = base > 0x100000 ? base : 0x100000;
            if (top > lo) usable += (top - lo);
        }
        if (i < 8) {
            dbg64_str("[LM64] E820[");
            dbg64_dec(i);
            dbg64_str("] base=");
            dbg64_hex64(base);
            dbg64_str(" len=");
            dbg64_hex64(len);
            dbg64_str(" type=");
            dbg64_dec(e[i].type);
            dbg64_str(" size_kb=");
            dbg64_dec(len / 1024);
            dbg64_nl();
        }
    }
    dbg64_str("[LM64] E820 usable_above_1M_kb=");
    dbg64_dec(usable / 1024);
    dbg64_nl();
}

static uint64_t read_rsp64() { uint64_t v; __asm__ volatile("mov %%rsp, %0" : "=r"(v)); return v; }
static uint64_t read_cs64()  { uint64_t v; __asm__ volatile("mov %%cs, %0"  : "=r"(v)); return v & 0xFFFF; }

// ==================== 已安装系统的启动路径 ====================
// 走到这里说明运行的不是安装介质，而是装到硬盘上的系统。64 位的桌面栈已经就位：
// 外壳在 kernel/gui64.cpp，应用在 calc64/mines64/terminal64/settings64/taskmgr64.cpp。
//   注意：串口那两行 "[OS] booted from installed disk" 与 "[OS] ready (idle)" 是自动验收
//   （tests/install_flow_test.py、tests/vmware_install_test.py）依赖的断言，勿删。
//   安装程序内核不带桌面（gui64.cpp 不进它的链接），所以整段用 #ifndef 包起来。
// ==================== 批次 B：ring3 槽位复用回归（缺陷根治的现场证明）====================
// 缺陷：usermode64.cpp 的 in_ring3 是**按任务槽位**的位图；ring3 进程被 kill（SIGKILL/SIGTERM）
//   时不会从 user64_enter64 正常返回，那一槽的位永远留着 —— 复用该槽的新任务进 ring3 会失败
//   （[USER64] enter FAILED reason=2）。本批次在 task64 的 kill/reap/exit/force-remove 路径清位。
// 这个自检就是回归证明：连续 rounds 轮"建 ring3 进程 -> 确认真进 ring3 -> kill(9) -> 收尸 ->
//   等任务槽回收 -> 下一轮必须复用**同一个槽**并再次成功进 ring3"。
// 为什么用 /spin.elf：它永不退出（nanosleep 死循环），所以 kill 时**必然**处在 ring3 里 ——
//   这正是会留下脏位的那个场景（普通会自己退出的程序走的是"正常返回 -> 自己清位"那条路）。
// 打点（自动验收 grep）：
//   [USER64] slotreuse round=<n> slot=<s> pid=<p> enter=ok
//   [USER64] slotreuse PASS rounds=<n> slot=<s>      /  FAIL round=<n> reason=<k>
#ifndef VIMTU_INSTALLER_MEDIA
extern "C" const uint8_t _binary_build64_spin64_elf_start[];
extern "C" const uint8_t _binary_build64_spin64_elf_end[];
// ★ A3：内嵌的 musl 静态程序（tools/musl_build_win.sh 编出 build64/musl_hello.elf，
//   build64.sh 里 objcopy -> build64/os/musl_hello_elf.o，符号名按输入路径生成）。
//   体积代价见 build64.sh 里那段注释与 docs 的 "musl（A3 第一步）" 节。
extern "C" const uint8_t _binary_build64_musl_hello_elf_start[];
extern "C" const uint8_t _binary_build64_musl_hello_elf_end[];

// 任务表快照查询（-1 = 该任务已不在表里 = 槽已回收）
static int k64_task_state_of64(uint32_t id) {
    for (int i = 0; i < TASK64_MAX; i++) {
        Task64Info in;
        if (task_info64(i, &in) == 0) continue;
        if (in.id == id) return (int)in.state;
    }
    return -1;
}
static int k64_proc_state_of64(int pid) {
    for (int i = 0; i < PROC64_MAX; i++) {
        Proc64Info in;
        if (proc64_info64(i, &in) == 0) continue;
        if ((int)in.pid == pid) return (int)in.state;
    }
    return -1;
}
static uint32_t k64_proc_task_id64(int pid) {
    for (int i = 0; i < PROC64_MAX; i++) {
        Proc64Info in;
        if (proc64_info64(i, &in) == 0) continue;
        if ((int)in.pid == pid) return in.task_id;
    }
    return 0;
}

static void ring3_slot_reuse_demo64(const char* path, int rounds) {
    // 幂等安装内嵌 /spin.elf（与终端 `proc run spin` 用的是同一个 blob）
    uint32_t ty = 0, sz = 0;
    if (vfs64_stat(path, &ty, &sz) != 0) {
        const int len = (int)(_binary_build64_spin64_elf_end - _binary_build64_spin64_elf_start);
        if (len <= 0 || vfs64_write(path, _binary_build64_spin64_elf_start, len) < 0) {
            dbg64_line_begin64();
            dbg64_str("[USER64] slotreuse skipped (cannot install /spin.elf)\n");
            dbg64_line_end64();
            return;
        }
    }

    int slot_expect = -1;
    int same_slot   = 1;
    for (int r = 1; r <= rounds; r++) {
        int reason = 0;
        const int pid = proc64_create64("slotchk", 0);
        if (pid < 0) { reason = 1; }                              // 建进程失败
        if (reason == 0 && proc64_start_elf64(pid, path) != 0) {  // 装载/建任务失败
            proc64_destroy64(pid);
            reason = 2;
        }
        uint32_t tid = 0;
        int slot = -1;
        if (reason == 0) {
            tid  = k64_proc_task_id64(pid);
            slot = task_slot_of_id64(tid);
            if (slot < 0) reason = 3;                             // 拿不到槽位
        }
        if (reason == 0) {
            // 有界等待"真的进了 ring3"：任务被调度过（RUNNING/SLEEP）且进程没有秒退。
            // 失败模式（reason=2 进不去 ring3）会让进程立刻 EXITED、任务变 DEAD —— 这里必现。
            task64_set_quiet64(tid, 1);                           // 回归自检的任务安静（原因见 task64.h）
            int reached = 0;
            for (int k = 0; k < 120; k++) {                       // 120 × 5ms = 600ms 上限
                task_sleep64(5);
                if (k64_proc_state_of64(pid) == (int)PROC64_EXITED) break;
                const int ts = k64_task_state_of64(tid);
                if (ts < 0 || ts == TASK64_DEAD) break;
                if (ts == TASK64_RUNNING || ts == TASK64_SLEEP) { reached = 1; break; }
            }
            if (!reached) {
                reason = 4;                                       // 没被调度起来（或已死）
            } else {
                // 再确认一次"稳定活着"（排除刚好采样在失败入口的微秒窗口里）
                task_sleep64(20);
                const int ts2 = k64_task_state_of64(tid);
                const int ps2 = k64_proc_state_of64(pid);
                if (ts2 < 0 || ts2 == TASK64_DEAD || ps2 == (int)PROC64_EXITED) reason = 5;
            }
        }

        if (reason != 0) {
            if (pid > 0) { (void)proc64_kill64(pid, 9); if (proc64_find64(pid)) proc64_destroy64(pid); }
            dbg64_line_begin64();
            dbg64_str("[USER64] slotreuse FAIL round=");
            dbg64_dec((uint64_t)r);
            dbg64_str(" reason=");
            dbg64_dec((uint64_t)reason);
            dbg64_nl();
            dbg64_line_end64();
            return;
        }

        if (slot_expect < 0) slot_expect = slot;
        else if (slot != slot_expect) same_slot = 0;
        dbg64_line_begin64();
        dbg64_str("[USER64] slotreuse round=");
        dbg64_dec((uint64_t)r);
        dbg64_str(" slot=");
        dbg64_dec((uint64_t)slot);
        dbg64_str(" pid=");
        dbg64_dec((uint64_t)pid);
        dbg64_str(" enter=ok\n");
        dbg64_line_end64();

        // 真 kill（SIGKILL=9）：进程变 EXITED、任务变 DEAD 并挂进回收队列。
        // 目标进程此刻在 ring3（spin 的 nanosleep 循环里）—— 正是会留下脏位的场景。
        (void)proc64_kill64(pid, 9);
        if (proc64_find64(pid)) proc64_destroy64(pid);
        // 有界等待任务槽真回收（下一轮才会复用它）；task_sleep64 内部会 task_yield64 -> task_drain_reap。
        for (int k = 0; k < 100; k++) {                            // 100 × 5ms = 500ms 上限
            task_sleep64(5);
            if (k64_task_state_of64(tid) < 0) break;               // 槽已 FREE（任务表里查不到）
        }
    }

    dbg64_line_begin64();
    if (same_slot) {
        dbg64_str("[USER64] slotreuse PASS rounds=");
        dbg64_dec((uint64_t)rounds);
        dbg64_str(" slot=");
        dbg64_dec((uint64_t)(slot_expect < 0 ? 0 : slot_expect));
        dbg64_str("\n");
    } else {
        dbg64_str("[USER64] slotreuse FAIL reason=6 (slot not reused)\n");
    }
    dbg64_line_end64();
}

// ==================== ★ A3：musl 静态程序（真进程 + ring3）====================
// 怎么做（与 /hello.elf 那条"内嵌 blob -> 装进 VimtuFS2 -> 从盘上读出来跑"完全同一套路，
// 但**跑法**升级成"真进程"）：
//   1) 把内嵌的 musl ELF（objcopy 出的 _binary_build64_musl_hello_elf_*，由
//      tools/musl_build_win.sh 产出）幂等装成 VimtuFS2 的 /musl_hello.elf；
//   2) 用 proc64_create64 + proc64_start_elf64 建**真进程**（自己的 CR3/任务），由
//      elf64_load_for_exec64 从**文件系统**读出来装载（打点 [ELF64] load … via=execve）；
//   3) 有界等待它退出（Proc64Info.state == PROC64_EXITED），收尸 + 打点。
// 为什么必须走进程而不是 user64_run_capp64 那条"平铺 blob"路：
//   musl 启动期要 arch_prctl(ARCH_SET_FS) 真写 IA32_FS_BASE（%fs 上的 TLS：errno 就在里面），
//   而 syscall64 的共享模式（没有进程上下文）只**记账**不写 MSR —— 走了也起不来（会 #PF）。
//   进程路径由 proc64_set_fs_base64 真写 MSR，且任务切换会保存/恢复（见 proc64.h 第 3 条）。
// 如实边界：UEFI（共享地址空间）下建不了进程 -> 只打一行跳过，绝不假装跑过。
// 打点（tests/musl64_test.py grep）：
//   [ELF64] install ok path=/musl_hello.elf bytes=<n>   / install skipped (exists) /musl_hello.elf size=<n>
//   [MUSL64] launch path=<p> pid=<n>                    （已建进程并挂上）
//   [MUSL64] done pid=<n> exited=<0|1> code=<n> ticks=<n>
//   [MUSL64] skipped (user window unavailable) / skipped (shared address space mode) / FAILED reason=<r>
static const char MUSL64_PATH64[] = "/musl_hello.elf";
static const char MUSL64_NAME64[] = "muslhello";

static int musl64_install64(const char* path) {
    uint32_t ty = 0, sz = 0;
    if (vfs64_stat(path, &ty, &sz) == 0) {
        dbg64_line_begin64();
        dbg64_str("[ELF64] install skipped (exists) ");
        dbg64_str(path);
        dbg64_str(" size=");
        dbg64_dec(sz);
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    const int len = (int)(_binary_build64_musl_hello_elf_end - _binary_build64_musl_hello_elf_start);
    if (len <= 0) {
        dbg64_line_begin64();
        dbg64_str("[MUSL64] FAILED reason=blob-empty\n");
        dbg64_line_end64();
        return -1;
    }
    if (vfs64_write(path, _binary_build64_musl_hello_elf_start, len) != len) {
        dbg64_line_begin64();
        dbg64_str("[MUSL64] FAILED reason=install path=");
        dbg64_str(path);
        dbg64_nl();
        dbg64_line_end64();
        return -1;
    }
    dbg64_line_begin64();
    dbg64_str("[ELF64] install ok path=");
    dbg64_str(path);
    dbg64_str(" bytes=");
    dbg64_dec((uint64_t)len);
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

static void musl64_demo64(const char* path) {
    const char* p = (path && path[0] == '/') ? path : MUSL64_PATH64;
    if (!user64_available64()) {
        dbg64_line_begin64();
        dbg64_str("[MUSL64] skipped (user window unavailable on this boot path)\n");
        dbg64_line_end64();
        return;
    }
    if (musl64_install64(p) != 0) return;
    if (!proc64_isolate64()) {                          // UEFI/固件页表：共享地址空间模式
        dbg64_line_begin64();
        dbg64_str("[MUSL64] skipped (shared address space mode: arch_prctl would not write IA32_FS_BASE)\n");
        dbg64_line_end64();
        return;
    }

    const int pid = proc64_create64(MUSL64_NAME64, 0);
    if (pid <= 0) {
        dbg64_line_begin64();
        dbg64_str("[MUSL64] FAILED reason=create\n");
        dbg64_line_end64();
        return;
    }
    if (proc64_start_elf64(pid, p) != 0) {
        if (proc64_find64(pid)) proc64_destroy64(pid);
        dbg64_line_begin64();
        dbg64_str("[MUSL64] FAILED reason=start path=");
        dbg64_str(p);
        dbg64_nl();
        dbg64_line_end64();
        return;
    }
    dbg64_line_begin64();
    dbg64_str("[MUSL64] launch path=");
    dbg64_str(p);
    dbg64_str(" pid=");
    dbg64_dec((uint64_t)pid);
    dbg64_nl();
    dbg64_line_end64();

    // 有界等待（10 秒上限）：musl 程序自己会 exit(7)；超时就 SIGKILL 并如实打点。
    const uint64_t t0 = g_ticks64;
    int exited = 0;
    while ((g_ticks64 - t0) < (uint64_t)PIT_HZ_64 * 10u) {
        if (k64_proc_state_of64(pid) == (int)PROC64_EXITED) { exited = 1; break; }
        if (k64_proc_state_of64(pid) < 0) break;         // 进程没了（理论上不该）
        task_sleep64(2);
    }
    int code = -1;
    for (int i = 0; i < PROC64_MAX; i++) {
        Proc64Info in;
        if (proc64_info64(i, &in) == 0) continue;
        if ((int)in.pid == pid) { code = in.exit_code; break; }
    }
    if (!exited) {
        (void)proc64_kill64(pid, 9);
        dbg64_line_begin64();
        dbg64_str("[MUSL64] TIMEOUT pid=");
        dbg64_dec((uint64_t)pid);
        dbg64_str(" -> SIGKILL\n");
        dbg64_line_end64();
    }
    if (proc64_find64(pid)) {
        Proc64Info in;
        for (int i = 0; i < PROC64_MAX; i++) {
            if (proc64_info64(i, &in) == 0) continue;
            if ((int)in.pid == pid) { code = in.exit_code; break; }
        }
        proc64_destroy64(pid);
    }
    dbg64_line_begin64();
    dbg64_str("[MUSL64] done pid=");
    dbg64_dec((uint64_t)pid);
    dbg64_str(" exited=");
    dbg64_dec((uint64_t)(exited ? 1 : 0));
    dbg64_str(" code=");
    if (code < 0) dbg64_putc('-');
    dbg64_dec((uint64_t)(code < 0 ? -code : code));
    dbg64_str(" ticks=");
    dbg64_dec(g_ticks64 - t0);
    dbg64_nl();
    dbg64_line_end64();
}

// ==================== ★ A3 下半：动态链接（PT_INTERP）+ FPU/xmm 上下文回归 ====================
// 四份内嵌产物（build64.sh 里 objcopy 成 .o；符号名按输入路径生成）：
//   ldvimtu.so   我们的动态链接器（装到 /lib/ldvimtu.so）
//   libfoo.so    演示共享库（装到 /lib/libfoo.so）
//   dynhello.elf 动态主程序（装到 /dynhello.elf；PT_INTERP = /lib/ldvimtu.so）
//   xmmsse.elf   FPU/xmm 回归程序（装到 /xmmsse.elf；两个进程同时跑）
extern "C" const uint8_t _binary_build64_ldvimtu_so_start[];
extern "C" const uint8_t _binary_build64_ldvimtu_so_end[];
extern "C" const uint8_t _binary_build64_libfoo_so_start[];
extern "C" const uint8_t _binary_build64_libfoo_so_end[];
extern "C" const uint8_t _binary_build64_dynhello_elf_start[];
extern "C" const uint8_t _binary_build64_dynhello_elf_end[];
extern "C" const uint8_t _binary_build64_xmmsse_elf_start[];
extern "C" const uint8_t _binary_build64_xmmsse_elf_end[];

static const char DYNLINK_MAIN64[]  = "/dynhello.elf";
static const char DYNLINK_LIBDIR64[] = "/lib";
static const char DYNLINK_INTERP64[] = "/lib/ldvimtu.so";
static const char DYNLINK_LIB64[]   = "/lib/libfoo.so";
static const char XMM64_PATH64[]    = "/xmmsse.elf";

// 幂等安装一份内嵌 blob：已存在就 skipped (exists)；目录不存在先建（/lib）
static int dynlink64_install_file64(const char* path, const uint8_t* p, const uint8_t* e, const char* what) {
    const int len = (int)(e - p);
    if (len <= 0) {
        dbg64_line_begin64();
        dbg64_str("[DYNLINK] FAILED reason=blob-empty file=");
        dbg64_str(what);
        dbg64_str("\n");
        dbg64_line_end64();
        return -1;
    }
    uint32_t ty = 0, sz = 0;
    if (vfs64_stat(path, &ty, &sz) == 0) {
        dbg64_line_begin64();
        dbg64_str("[ELF64] install skipped (exists) ");
        dbg64_str(path);
        dbg64_str(" size=");
        dbg64_dec(sz);
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    if (vfs64_write(path, p, len) != len) {
        dbg64_line_begin64();
        dbg64_str("[DYNLINK] FAILED reason=write path=");
        dbg64_str(path);
        dbg64_str("\n");
        dbg64_line_end64();
        return -1;
    }
    dbg64_line_begin64();
    dbg64_str("[ELF64] install ok path=");
    dbg64_str(path);
    dbg64_str(" bytes=");
    dbg64_dec((uint64_t)len);
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

// 三份动态链接产物（/lib 目录 + 两个文件 + 主程序）幂等安装到 VimtuFS2
static int dynlink64_install64() {
    (void)vfs64_mkdir64(DYNLINK_LIBDIR64);        // 已存在 -> 返回非 0，无所谓
    int rc = 0;
    rc |= dynlink64_install_file64(DYNLINK_INTERP64, _binary_build64_ldvimtu_so_start, _binary_build64_ldvimtu_so_end, "ldvimtu.so");
    rc |= dynlink64_install_file64(DYNLINK_LIB64, _binary_build64_libfoo_so_start, _binary_build64_libfoo_so_end, "libfoo.so");
    rc |= dynlink64_install_file64(DYNLINK_MAIN64, _binary_build64_dynhello_elf_start, _binary_build64_dynhello_elf_end, "dynhello.elf");
    return rc;
}

// 等一个进程退出（有界）：返回 1 = 已退出（*out_code 有效），0 = 超时
static int dynlink64_wait64(int pid, uint32_t max_ticks, int* out_code) {
    const uint64_t t0 = g_ticks64;
    while (g_ticks64 - t0 < (uint64_t)max_ticks) {
        const int st = k64_proc_state_of64(pid);
        if (st == (int)PROC64_EXITED) {
            for (int i = 0; i < PROC64_MAX; i++) {
                Proc64Info in;
                if (proc64_info64(i, &in) == 0) continue;
                if ((int)in.pid == pid) { if (out_code) *out_code = in.exit_code; break; }
            }
            return 1;
        }
        if (st < 0) break;                        // 进程没了（理论上不该）
        task_sleep64(2);
    }
    return 0;
}

// 启动期演示：跑 /dynhello.elf（真进程；PT_INTERP -> 内核先装 /lib/ldvimtu.so）
// 打点（tests/dynlink64_test.py grep）：
//   [DYNLINK] launch path=/dynhello.elf pid=<n>
//   [DYNLINK] done pid=<n> exited=<0|1> code=<n> ticks=<n>
//   [DYNLINK] PASS  /  FAILED reason=<create|start|timeout|code>  /  skipped (…)
static void dynlink64_demo64(const char* path) {
    const char* p = (path && path[0] == '/') ? path : DYNLINK_MAIN64;
    if (!user64_available64()) {
        dbg64_line_begin64();
        dbg64_str("[DYNLINK] skipped (user window unavailable on this boot path)\n");
        dbg64_line_end64();
        return;
    }
    if (dynlink64_install64() != 0) return;
    if (!proc64_isolate64()) {
        dbg64_line_begin64();
        dbg64_str("[DYNLINK] skipped (shared address space mode)\n");
        dbg64_line_end64();
        return;
    }
    const int pid = proc64_create64("dynhello", 0);
    if (pid <= 0) {
        dbg64_line_begin64();
        dbg64_str("[DYNLINK] FAILED reason=create\n");
        dbg64_line_end64();
        return;
    }
    if (proc64_start_elf64(pid, p) != 0) {
        if (proc64_find64(pid)) proc64_destroy64(pid);
        dbg64_line_begin64();
        dbg64_str("[DYNLINK] FAILED reason=start path=");
        dbg64_str(p);
        dbg64_nl();
        dbg64_line_end64();
        return;
    }
    dbg64_line_begin64();
    dbg64_str("[DYNLINK] launch path=");
    dbg64_str(p);
    dbg64_str(" pid=");
    dbg64_dec((uint64_t)pid);
    dbg64_nl();
    dbg64_line_end64();

    const uint64_t t0 = g_ticks64;
    int code = -1;
    const int exited = dynlink64_wait64(pid, (uint32_t)PIT_HZ_64 * 10u, &code);
    if (!exited) (void)proc64_kill64(pid, 9);
    if (proc64_find64(pid)) proc64_destroy64(pid);
    dbg64_line_begin64();
    dbg64_str("[DYNLINK] done pid=");
    dbg64_dec((uint64_t)pid);
    dbg64_str(" exited=");
    dbg64_dec((uint64_t)(exited ? 1 : 0));
    dbg64_str(" code=");
    if (code < 0) dbg64_putc('-');
    dbg64_dec((uint64_t)(code < 0 ? -code : code));
    dbg64_str(" ticks=");
    dbg64_dec(g_ticks64 - t0);
    dbg64_nl();
    dbg64_line_end64();
    dbg64_line_begin64();
    if (exited && code == 0) {
        dbg64_str("[DYNLINK] PASS\n");
    } else {
        dbg64_str("[DYNLINK] FAILED reason=");
        dbg64_str(exited ? "code" : "timeout");
        dbg64_nl();
    }
    dbg64_line_end64();
}

// FPU/xmm 回归：同一个 ELF（/xmmsse.elf）起**两个**真进程，各自把 xmm0..15 初始化成
// pid 相关的模式、循环 300 轮（每轮 +1 并 nanosleep 让出 CPU），每 10 轮核对一次自己的
// 16 个 xmm 是否还是"自己的值"。两个进程交替运行 —— 内核若在任务切换处不保存/恢复
// xmm+MXCSR，双方立刻互相污染，程序会打印 ok=0 并以退出码 1 结束。
// 打点（tests/dynlink64_test.py grep）：
//   [TASK64] fpu demo spawn A=<pidA> B=<pidB>
//   [TASK64] fpu demo A=<codeA> B=<codeB> PASS|FAIL
static void fpu64_demo64(const char* path) {
    const char* p = (path && path[0] == '/') ? path : XMM64_PATH64;
    if (!user64_available64()) {
        dbg64_line_begin64();
        dbg64_str("[TASK64] fpu demo skipped (user window unavailable)\n");
        dbg64_line_end64();
        return;
    }
    if (dynlink64_install_file64(p, _binary_build64_xmmsse_elf_start, _binary_build64_xmmsse_elf_end, "xmmsse.elf") != 0) return;
    if (!proc64_isolate64()) {
        dbg64_line_begin64();
        dbg64_str("[TASK64] fpu demo skipped (shared address space mode)\n");
        dbg64_line_end64();
        return;
    }

    int pids[2] = { -1, -1 };
    int spawned = 0;
    for (int k = 0; k < 2; k++) {
        const int pid = proc64_create64(k ? "xmmB" : "xmmA", 0);
        if (pid <= 0) break;
        if (proc64_start_elf64(pid, p) != 0) {
            if (proc64_find64(pid)) proc64_destroy64(pid);
            break;
        }
        pids[k] = pid;
        spawned++;
    }
    if (spawned < 2) {
        for (int k = 0; k < 2; k++) if (pids[k] > 0 && proc64_find64(pids[k])) proc64_destroy64(pids[k]);
        dbg64_line_begin64();
        dbg64_str("[TASK64] fpu demo spawn FAILED (cannot create 2 procs)\n");
        dbg64_line_end64();
        return;
    }
    dbg64_line_begin64();
    dbg64_str("[TASK64] fpu demo spawn A=");
    dbg64_dec((uint64_t)pids[0]);
    dbg64_str(" B=");
    dbg64_dec((uint64_t)pids[1]);
    dbg64_nl();
    dbg64_line_end64();

    int codes[2] = { -1, -1 };
    int exited[2] = { 0, 0 };
    for (int k = 0; k < 2; k++) exited[k] = dynlink64_wait64(pids[k], (uint32_t)PIT_HZ_64 * 15u, &codes[k]);
    for (int k = 0; k < 2; k++) {
        if (!exited[k]) (void)proc64_kill64(pids[k], 9);
        if (proc64_find64(pids[k])) proc64_destroy64(pids[k]);
    }
    const int pass = (exited[0] && exited[1] && codes[0] == 0 && codes[1] == 0);
    dbg64_line_begin64();
    dbg64_str("[TASK64] fpu demo A=");
    dbg64_dec((uint64_t)(uint32_t)codes[0]);
    dbg64_str(" B=");
    dbg64_dec((uint64_t)(uint32_t)codes[1]);
    dbg64_str(pass ? " PASS\n" : " FAIL\n");
    dbg64_line_end64();
}

#endif

#ifndef VIMTU_INSTALLER_MEDIA
// ==================== Rust 模块（gui_rs crate）：设计 Token + 主题配色 ====================
// 位置与理由：
//   1) 只在**系统内核**里链接 gui_rs.o（安装介质内核不链，见 build64.sh），所以本段必须
//      待在 `#ifndef VIMTU_INSTALLER_MEDIA` 里 —— 安装程序内核编不到这些调用；
//   2) Rust 侧导出的是 `#[no_mangle] extern "C"` 符号，C++ 必须在**文件作用域**声明
//      （块作用域上的 linkage 说明符 clang 不接受：实测 "expected unqualified-id"）；
//      声明都集中在 kernel/rust64.h，这里只放钩子与启动打点。
// 打点格式（自动验收 grep，勿改）：[RUST64] tokens ok themes=N accent=#RRGGBB selftest PASS
extern "C" void rust64_panic_hook64(const uint8_t* msg, uint32_t len) {
    // panic64_bsod64 不返回；万一它返回了，Rust 侧自己还有 `cli; hlt` 兜底。
    (void)len;
    dbg64_str("[RUST64] panic hook: ");
    if (msg) dbg64_str((const char*)msg);
    dbg64_nl();
    panic64_bsod64("RUST64 panic", (uint64_t)(uintptr_t)msg);
}

// 小工具：打 6 位大写十六进制（accent 色值；dbg64_hex64 是固定 16 位宽，不适合这里）
static void rust64_print_hex6_64(uint32_t v) {
    static const char* H = "0123456789ABCDEF";
    char buf[7];
    for (int i = 5; i >= 0; i--) { buf[i] = H[v & 0xF]; v >>= 4; }
    buf[6] = 0;
    dbg64_str(buf);
}

// 启动期调用一次：装 panic 钩子 + 跑 Rust 侧自检 + 打点。
// 注意"不改运行期状态"：rust64_tokens_init64() 的自检内部会保存/恢复当前主题。
static void rust64_boot_init64() {
    rust64_panic_hook_set64(&rust64_panic_hook64);   // Rust panic -> [RUST64] + 内核蓝屏
    const uint32_t st  = rust64_tokens_init64();     // 0 = Token/主题/配色计算自检全过
    const uint32_t cur = rust64_theme_current64();
    const uint32_t acc = rust64_accent_rgb64(cur);
    dbg64_line_begin64();
    dbg64_str("[RUST64] tokens ok themes=");
    dbg64_dec((uint64_t)rust64_theme_count64());
    dbg64_str(" accent=#");
    rust64_print_hex6_64(acc);
    dbg64_str(" selftest ");
    if (st == 0 && rust64_panicked64() == 0) {
        dbg64_str("PASS");
    } else {
        dbg64_str("FAIL mask=");
        dbg64_dec((uint64_t)st);
        dbg64_str(" panicked=");
        dbg64_dec((uint64_t)rust64_panicked64());
    }
    // 主题名（UTF-8，名字是稳定接口）：把 "当前主题 + 名字" 一起留在启动日志里
    dbg64_str(" theme=");
    dbg64_dec((uint64_t)cur);
    dbg64_str(" name=");
    {
        uint8_t name[32];
        const uint32_t n = rust64_theme_name64(cur, name, sizeof(name));
        for (uint32_t i = 0; i < n && i < (uint32_t)sizeof(name); i++) dbg64_putc((char)name[i]);
    }
    dbg64_nl();
    dbg64_line_end64();
}

[[noreturn]] static void os_boot_path(const BootInfo* bi) {
    dbg64_str("[OS] booted from installed disk (system kernel, no installer)");
    dbg64_nl();
    // ---- 中断路由：优先 LAPIC + IOAPIC 接管（APIC）；拿不到就留在 8259 PIC。
    // 位置理由（为什么放在 os_boot_path 的第一件事）：
    //   1) 它在 kmain64 的 M0 门（"[LM64] PIT IRQ OK"）之后 —— 先证明 8259 路径本身是通的，
    //      再切换；万一 APIC 有 bug，串口日志能区分"本来就不通"和"切换切坏了"。
    //   2) 它在 ata64_init64()/task_start64() 之前 —— 键盘、鼠标、ATA 的 IRQ 注册与自检
    //      全部发生在 APIC 已接管之后，这些设备自己的 selftest 就顺带验收了 IOAPIC 路由
    //      （[ATA64] irq14 selftest PASS 要求 IRQ14 真的到达）。
    //   3) 只在系统内核调用：apic64.cpp 不进安装介质的链接（build64.sh），安装链保持纯 PIC。
    // 拿不到 ACPI/LAPIC/IOAPIC 或自检失败时，apic64_init64() 内部**自动保持/退回 PIC**
    // 并打印 unavailable 行，系统照常启动、桌面照常起来（硬要求）。
    (void)apic64_init64();
    // ---- SMP：启动 AP（多核第一阶段：只要求"AP 真的起来并且不捣乱"）----
    // 位置：apic64_init64() **之后**（ICR/LAPIC 已经可用、EOI 走 LAPIC）、task_start64()
    //       **之前**（调度器仍是单核；AP 起来后只 cli+hlt 停住，不参与调度）。
    // 只在 APIC 模式下做：还在 PIC 回退模式时直接跳过并说明（PIC 没有 ICR，也没有 SIPI）。
    // 只在系统内核调用：smp64.cpp/smp64.o/ap_trampoline64.o 都不进安装介质的链接。
    // 任何前置条件不满足（拿不到 MADT 的 APIC ID、跳板页没映射、自检失败…）都会在
    // smp64_init64() 内部优雅跳过 —— 绝不挂在启动里、绝不变砖。
    if (g_irq_mode64 == IRQ_MODE64_APIC) {
        (void)smp64_init64();
    } else {
        dbg64_line_begin64();
        dbg64_str("[SMP] skipped (irq mode = pic)");
        dbg64_nl();
        dbg64_line_end64();
    }
    // ---- ATA：注册/打开 IRQ14，之后读写走中断驱动等待 + 超时回退轮询（只读自检不写盘）----
    ata64_init64();
    // ---- 运行期显示层：模式清单（0x7400）+ 0x3DA 实测刷新率 + 与 EDID 首选时序对比 + 自检 ----
    // 位置：fb_init() 已经在 kmain 里跑过（本函数只读 fb 的物理分辨率兜底），g_ticks64 已在走；
    // 放在 task_start64() 之前 —— 探测本身不需要调度器，跑完再上线。
    // 边界：运行期 DDC 再探测**不做**（理由见 display64.h），打点里如实写 skipped。
    display64_init64();
    (void)display64_selftest64();
    // 调度器上线：任务 0 = 本流程（桌面消息循环），另建 kheart/kwork/ksum 三个内核线程。
    // 必须放在 gui64_run 之前 —— 之后不再返回。
    task_start64();
    // ---- 用户态（ring3）+ int 0x80：启动期跑一次真实用户程序（M3 起点）----
    // 位置有讲究：必须在 task_start64() 之后（调度器在线：TSS.rsp0 由它维护，用户程序
    // 也能被 PIT 抢占），且在 gui64_run() 之前（那之后不再返回）；跑完必须还能进桌面。
    syscall64_init64();                     // 装/确认 int 0x80 门（DPL=3）+ 自检
    // ★ 批次 C：先问"用户窗口到底能不能用" —— UEFI 路径下引导期页表属于固件且只读，
    //   用户窗口（要往 PML4[0]/PDPTE 里打开 U/S）根本建不起来（实测那一写就是 #PF err=3、
    //   cr2=固件 PML4）。所以那种情况下**整段 ring3 演示跳过**并只打一行说明，绝不假装成功。
    if (user64_available64()) {
        (void)user64_selftest64();              // 用户页/帧/选择子自检 -> [USER64] selftest PASS
        {
            const uint64_t blob_sz = (uint64_t)(_binary_build64_user_demo64_bin_end -
                                                _binary_build64_user_demo64_bin_start);
            (void)user64_run_blob64(_binary_build64_user_demo64_bin_start, (uint32_t)blob_sz, "demo64");
        }
        // ---- ★ A2：**C 写的**用户程序（我们自己的最小 libc + 自有 ABI 包装）----
        //   位置：紧跟 demo64、在 fbdemo 之前 —— 两个程序只打串口证据、不碰屏幕，所以不会挤掉
        //   下面 fbdemo 的像素证据窗口（tests/fbmap64_test.py 按 [FBDEMO] 同步抓屏）。
        //   入口用 user64_run_capp64：C 程序有**可写**的 .data/.bss（errno / printf 行缓冲 /
        //   malloc 竞技场），必须把代码页映射成可写（见 kernel/usermode64.cpp 的说明）。
        {
            const uint64_t hello_sz = (uint64_t)(_binary_build64_user_hello_bin_end -
                                                 _binary_build64_user_hello_bin_start);
            (void)user64_run_capp64(_binary_build64_user_hello_bin_start, (uint32_t)hello_sz, "hello_c");
        }
        {
            const uint64_t libc_sz = (uint64_t)(_binary_build64_user_libctest_bin_end -
                                                _binary_build64_user_libctest_bin_start);
            (void)user64_run_capp64(_binary_build64_user_libctest_bin_start, (uint32_t)libc_sz, "libctest_c");
        }
        // ---- ★ A1/A2：用户态绘图演示（ring3 自己把画面画到屏幕上；内核只映射显存 + 提交区域）----
        //   位置：紧跟 demo64（同一个"用户窗口可用"分支）；顺序上在 app64/elf64/proc64 演示之前，
        //   这样启动期的像素证据（tests/fbmap64_test.py）落在屏幕还没被别的阶段重画的时候。
        //   演示期间内核侧绘制/提交关闭（fb_kernel_paint64(0)，由 user64_run_fbdemo64 切换）：
        //   屏幕上那一条色带**只**由用户程序画 —— 这就是"这确实是用户程序在画"的证据。
        //   ★ A2 起默认跑 **C 版**（user/apps/fbdemo.c，构建期编成 blob；见 build64.sh 的
        //     VIMTU_USER_FBDEMO 开关）；VIMTU_USER_FBDEMO_ASM=1 的构建改跑 A1 的汇编版
        //     （user/fbdemo.asm）。两版的可观测行为逐条一致（对照表见 user/apps/fbdemo.c 顶部）。
        {
#if VIMTU_USER_FBDEMO_ASM
            const uint64_t fb_blob_sz = (uint64_t)(_binary_build64_user_fbdemo64_bin_end -
                                                  _binary_build64_user_fbdemo64_bin_start);
            (void)user64_run_fbdemo64(_binary_build64_user_fbdemo64_bin_start, (uint32_t)fb_blob_sz);
#else
            const uint64_t fb_blob_sz = (uint64_t)(_binary_build64_user_fbdemo_bin_end -
                                                  _binary_build64_user_fbdemo_bin_start);
            // C 版走同一个"关内核绘制 -> ring3 画屏 -> 恢复"的入口（user64_run_fbdemo_capp64 与
            // user64_run_fbdemo64 的差别只有 blob 的代码页可写性，见 usermode64.cpp）。
            (void)user64_run_fbdemo_capp64(_binary_build64_user_fbdemo_bin_start, (uint32_t)fb_blob_sz);
#endif
        }
    } else {
        dbg64_line_begin64();
        dbg64_str("[USER64] ring3 demos skipped (user window unavailable on this boot path)\n");
        dbg64_line_end64();
    }
    // ---- 用户态演示跑完，接着跑"从文件系统装出来"的应用（下面这段）----
    // ---- 可安装应用（VAP64）：挂载 VimtuFS2 -> 自检 -> 幂等安装内嵌 hello.vap -> 从盘上读出来跑一次 ----
    // 挂载参数 (drive, LBA) 的依据：
    //   drive = 0：primary master，与 store64_init64(0)/task 路径用的是同一块系统盘；
    //   LBA   = kernel/part64.h 定义的主分区起始 —— PART_MAIN_LBA = PART_BOOT_LBA(9) + PART_BOOT_SECS(8000)
    //           = 8009。安装引擎 part_create_standard()/part_install_step() 写进目标盘 MBR 的第 2 个
    //           分区项（type 0x07）起点就是这个值 —— 也就是说 kernel/memlayout64.h 的 store 裸盘
    //           保留区（ML64_STORE_LBA = 8009）与主分区起点**完全重叠**。所以下面的 store 初始化
    //           必须排在挂载之后：它先探测这个卷，优先把槽放进文件系统（/store.a、/store.b），
    //           没有卷才降级到裸盘槽区并打 WARN（见 kernel/store64.cpp 的载体说明）。
    //   app64_main_part_lba64() 先按同一条规则读 MBR 找 0x07 项（真盘换了布局也能跟上），
    //   读不到才退回上述常量。挂载失败/读不到时优雅降级：只打原因，install/launch 不跑、不崩，
    //   桌面起来后终端里仍可手动 `run`（VFS 若随后可用则命令可再试）。
    {
        const int app_drive = 0;
        const uint32_t app_lba = app64_main_part_lba64(app_drive);
        // ★ 多卷：系统卷挂进 **0 号槽**并登记成"系统卷槽"（store64/config64/update64 固定写卷的依据），
        //   同时激活它 —— 之后 drive64_scan64 会给其余可浏览卷分配 1..3 号槽，D:/E: 就能真点进去。
        if (vfs64_mount_system64(app_drive, app_lba) == 0) {
            // ---- 盘符/驱动器枚举 + 目录树打印（"此电脑"的数据来源；只读，不写盘）----
            // 位置讲究：必须在**挂载成功之后** —— C: 的判定依据就是"当前真正挂载的那个卷"
            // （见 kernel/drive64.h 的盘符规则）；再早调用只能退化成"按 MBR 0x07 猜"。
            //   drive64_scan64     ：枚举所有 PATA/AHCI/NVMe 盘 -> MBR 分区 -> 文件系统识别
            //   drive64_dump64     ：打印盘符表（自动验收 grep）
            //   drive64_selftest64 ：盘符唯一/容量自洽/幂等（[DRV64] selftest PASS）
            //   vfs64_tree_dump64  ：有界打印目录树（多级路径 + 类型/大小/mtime 的实测证据）
            (void)fat64_selftest64();                            // ★ 批次 K：写入器 + **新的只读读取器**离线自检
            (void)drive64_scan64();                              // （FAT32 分区的探测/挂载就在这一步）
            (void)fs64_selftest64();                             // ★ 批次 K：统一卷表 + FAT 只读语义自检
            vfs64_slots_dump64();                                // ★ 多卷：卷槽表（每槽 drive/起始 LBA/容量）
            drive64_dump64();
            (void)drive64_selftest64();
            (void)vfs64_tree_dump64("/", 32, 4);
            (void)explorer64_selftest64();                       // 文件管理器纯逻辑自检（单位换算/路径/滚动/历史栈）
            (void)app64_selftest64();
            (void)app64_install_builtin64(app_drive, app_lba);   // 幂等：已装过则 skipped (exists)
            (void)elf64_selftest64();                            // 合法映像/坏样本自检（坏样本带 selftest 前缀）
            (void)elf64_install_builtin64(app_drive, app_lba);   // 幂等：已装过则 skipped (exists)
            // ---- ring3 相关的一切都必须在"用户窗口可用"时才跑 ----
            // UEFI 路径下用户窗口建不起来（固件页表只读），VAP64/ELF64 启动器与多进程演示
            // 都会在第一次映射用户页时失败；所以这里统一跳过并留一行说明（os_boot_path 上面
            // 已经把 demo64 也一起跳过了）。安装/自检这两步只碰 VFS，照常做。
            if (user64_available64()) {
                (void)app64_launch64("/hello.vap");              // 从文件系统读出 -> 校验 VAP64 -> ring3
                (void)elf64_run64("/hello.elf");
                // ---- 批次 C：多进程演示（进程级地址空间 + fork/execve/wait4/kill）----
                // 位置：在既有 ring3 演示之后、gui64_run 之前（那之后不再返回）；跑完必须还能进桌面。
                // 顺序有讲究：
                //   1) proc64_init64()：探测"引导期页表是不是我们自己的" -> 决定隔离模式（串口打点）；
                //   2) proc64_selftest64()：进程表/地址空间布局自检（PML4[511] 共享 + PDPT[4] 私有）；
                //   3) 幂等把内嵌 /proc64.elf 装进 VimtuFS2（供 fork 出的子进程 execve 自己）；
                //   4) proc64_demo64()：建 init 进程（自己的 CR3 + 自己的任务）跑完整演示，
                //      内核这侧只做有界等待 + 收尾。UEFI（固件页表）下会打一行诚实跳过。
                proc64_init64();
                (void)proc64_selftest64();
                (void)proc64_install_builtin64(app_drive, app_lba);
                (void)proc64_demo64("/proc64.elf");
                // ---- 批次 D：ring3 pipe 演示（fork 后父子各持一端通信）----
                // 为什么单开一个程序：proc64_test 断言了上面那次 fork 的 pages=8，往 /proc64.elf 里
                // 加代码会挪动页数。这里跑的 /pipe64.elf 只做 pipe(22) + fork(57) + 读/写 + wait4(61)。
                (void)proc64_pipe_demo64("/pipe64.elf");
                // ---- ★ A5 前置：输入事件投递（自有 ABI 12）+ 共享内存缓冲（13/14）----
                // 事件层的结构体/队列自检先跑一次（布局 40B、满则丢最旧、焦点/捕获路由表）。
                // 位置：pipe 演示之后、slot 复用演示之前 —— 这两个演示都不碰输入/共享内存，
                // 而下面那段 fbdemo 的像素断言要求\"带外没有别的绘制\"，本演示只画一个矩形
                // （写在 fbdemo 之前，抓帧对比的两帧里都已经包含它，不影响那条断言）。
                {
                    const int evf = ev64_selftest64();
                    dbg64_line_begin64();
                    if (evf == 0) {
                        dbg64_str("[EV64] selftest PASS (event ABI 40B / ring drop-oldest / focus+capture)\n");
                    } else {
                        dbg64_str("[EV64] selftest FAIL mask=");
                        dbg64_dec((uint64_t)evf);
                        dbg64_nl();
                    }
                    dbg64_line_end64();
                }
                (void)proc64_evshm_install64(app_drive, app_lba);   // 幂等：把 /evshm.elf 装进系统卷
                (void)proc64_evshm_demo64("/evshm.elf");            // shm_create/map + fb_flip + input_poll
                // ---- 批次 B：缺陷回归 —— kill 掉 ring3 进程后复用同一任务槽，必须还能进 ring3 ----
                // 位置在 proc64 演示之后、gui64_run 之前（跑完必须还能进桌面）；只走 BIOS 路径
                // （UEFI 下用户窗口不可用，上面这一整块已经被 user64_available64() 挡在外面）。
                ring3_slot_reuse_demo64("/spin.elf", 4);
                // ---- ★ A3：musl 静态程序（真进程 + ring3；"移植 musl libc"的第一块里程碑）----
                // 位置：整段 ring3 演示的**最后**一个（跑完还要能进桌面）。为什么排最后：
                //   proc64 的 fork/pipe/slotreuse 演示都对进程表/日志有断言（tests/proc64_test.py、
                //   tmgr_proc_test.py 等），musl 那段要多建/多收一个进程 —— 排最后最不容易搅动它们。
                musl64_demo64(MUSL64_PATH64);
                // ---- ★ A3 下半：动态链接（PT_INTERP）+ FPU/xmm 上下文切换回归 ----
                // 位置：同样排在整段 ring3 演示的最后（跑完还要能进桌面）。两者都会多建/多收
                // 进程（动态链接 1 个、FPU 回归 2 个），排最后最不容易搅动前面那些脚本的断言。
                //   dynlink64_demo64：/dynhello.elf —— 内核先装 /lib/ldvimtu.so（解释器），
                //     再由 ld.so 加载 /lib/libfoo.so、做三类重定位、调 DT_INIT/DT_INIT_ARRAY；
                //   fpu64_demo64：/xmmsse.elf 起两个进程交替跑 SSE（xmm0..15 + MXCSR 回归）。
                dynlink64_demo64(DYNLINK_MAIN64);
                fpu64_demo64(XMM64_PATH64);
            } else {
                proc64_init64();                                 // 仍然打点：mode=shared（如实）
                (void)proc64_demo64("/proc64.elf");              // 只打一行 "demo skipped (shared address space mode)"
                dbg64_line_begin64();
                dbg64_str("[APP64] ring3 launches skipped (user window unavailable on this boot path)\n");
                dbg64_line_end64();
            }
            // ---- 批次 B：FD 层自检（路径规范化 + 目录句柄）；终端文件命令与 ring3
            //      open/read/close 都走这一层（kernel/fd64.cpp）。放在 VFS 挂载成功之后。----
            (void)fd64_selftest64();
        } else {
            // ★ 批次 K：系统卷挂不上（例如"装好但主分区还没格式化"的盘）也**照常枚举盘符** ——
            //   FAT32 只读卷（ESP / U 盘）不依赖 VimtuFS2，文件管理器和终端仍然要能进去看。
            //   （C: 的判定退化成"第一个可浏览卷"；没有可浏览卷时照旧不给盘符。）
            dbg64_str("[APP64] boot: vfs64 mount failed -> install/launch skipped (terminal 'run' can retry)\n");
            dbg64_line_begin64();
            dbg64_str("[APP64] boot: drive scan without system volume (FAT32 read-only volumes still browsable)\n");
            dbg64_line_end64();
            (void)fat64_selftest64();                 // ★ 批次 K：只读读取器自检（写入器 + 内存卷往返 + LFN）
            (void)drive64_scan64();
            drive64_dump64();
            (void)drive64_selftest64();
            (void)fs64_selftest64();
            (void)explorer64_selftest64();
        }
    }
    // ---- 设置持久化 store：**必须放在 VFS 挂载之后** ----
    // 槽优先放在文件系统里（VimtuFS2 的 /store.a、/store.b，各 16KB = 一个槽）；
    // store64_init64 先探"卷挂没挂"（vfs64_stat("/")），挂上了就走 VFS 载体。放在挂载之前的话
    // 它探不到卷，只能降级到裸盘槽区 LBA 8009..8072（ML64_STORE_LBA，正好是主分区起点，
    // 会把 VimtuFS2 超级块/inode 覆盖掉）——那是本模块只留给"无卷"的兜底路径。
    // 这里只 init + 自检 + dump —— **不要 flush**：flush 会写盘且不可重入，启动早期不做。
    // 0 = primary master，与上面 app64/vfs64 挂载用的是同一块系统盘。
    store64_init64(0);
    {
        const int sst = store64_selftest64();
        if (sst) {
            dbg64_str("[STORE64] selftest FAIL mask=");
            dbg64_dec((uint64_t)sst);
            dbg64_nl();
        }
    }
    store64_dump64();
    // ---- 系统配置 / 会话策略 / 状态机 / 看门狗（本轮移植：config64 + session64 + sysstate64 + panic64）----
    // 顺序有讲究：
    //   1) config64_init64()：**必须在 store64_init64() 之后**（它读 store64 里 "cfg." 前缀的键）；
    //   2) session64_init64()：读会话策略（config64）并登记应用表；
    //   3) sysstate64：BOOT -> 注册模块（模块的 init 钩子只做**只读复核**，不重做上面的初始化）
    //      -> STARTING；RUNNING 由 gui64 的桌面首帧推进（见 kernel/gui64.cpp 主循环）；
    //   4) panic64_init64()：建看门狗任务（需要 task_start64() 已经跑过），桌面首帧时武装。
    config64_init64();
    session64_init64();
    // ★ 本批（P1c）：多用户骨架（/etc/users.db + /home/<用户> + root 隐藏记录 + 引导用户）。
    //   位置讲究：必须在 vfs64 挂载系统卷之后（写 /etc/users.db）、config64_init64() 之后
    //   （读 ui.login.last），且在任何"要登录"之前的启动阶段；它自己会打 [USER64] userdb 行。
    userdb64_init64();
    sysstate64_begin64();
    sys64_register_builtin64();
    // ★ 批次 A 后半：preload64 / update64 也要进模块表 —— 必须在 sysstate64_start64() **之前**
    //   注册（这样 STARTING 阶段会复核它们；注册在 STARTING 之后的话模块状态会停在 STOPPED/健康 DOWN）。
    //   它们的 init 钩子只做"模块已编入"的只读复核，真正的预热/检查在下面（gui64_run 之前）。
    preload64_init64();
    update64_init64();
    (void)sysstate64_start64();
    panic64_init64();
    // ---- 网络：e1000（轮询收发）+ ARP/ICMP 一次性探测 ----
    // 位置：store 之后、gui64_run 之前（那之后不再返回）；安装程序内核不链本模块（见 build64.sh）。
    // 说明：桌面消息循环没有全局 tick 钩子（不改 gui64.cpp 的实现结构），所以 net64_poll64()
    //   的定时轮询没挂进 gui64_run；桌面起来后收包靠终端 `ping` 命令内部的有界 poll 循环。
    //   无网卡/ARP/ICMP 超时只打 no link 行并返回负值，系统照常启动、自检不算失败。
    (void)net64_init64();
    // ---- USB 主机（UHCI）：枚举 HID 引导键盘（只做引导键盘；EHCI/xHCI 未做）----
    // 位置：net64 之后、gui64_run 之前（那之后不再返回）；找不到主控/没插设备/枚举失败都
    //   只打点并返回负值，系统照常启动、桌面照常工作（自检按 skipped 处理，不算失败）。
    // 运行期轮询由 kusb 内核线程负责（见 kernel/task64.cpp），不占用这里的执行流。
    (void)usb64_init64();
    // ---- ★ 批次 O：USB 存储（U 盘）接进来之后**重扫盘符表** ----
    // 为什么必须重扫：drive64_scan64() 上面（vfs64 挂载成功那条路径）已经跑过一次，那时 USB
    //   存储还没枚举（usb64_init64 排在 net64 之后）。重扫会把 U 盘上的 FAT32/VimtuFS2 卷按现有
    //   规则识别 -> 只读挂载 -> 分配盘符（D:/E:…），容量/可用/只读标记全部由现有代码算出来。
    // 幂等性：drive64_scan64() 对同一块盘/同一个卷复用已有的 vfs64 槽与 fs64 卷号（见其注释），
    //   所以重扫不会动 C: 的盘符、也不会把卷表撑爆；**没插 U 盘时整段跳过**（行为与改动前一致）。
    if (usb64_msc_count64() > 0) {
        dbg64_line_begin64();
        dbg64_str("[USBST] storage attached -> rescan drive letters (usb drives=");
        dbg64_dec((uint64_t)usb64_msc_count64());
        dbg64_str(")\n");
        dbg64_line_end64();
        (void)drive64_scan64();
        drive64_dump64();
        (void)drive64_selftest64();
    }
    // 运行期轮询由 kusb 内核线程负责（见 kernel/task64.cpp），不占用这里的执行流。
    // （上面那行 usb64_init64() 的注释见下：找不到主控/没插设备只打点，系统照常启动。）
    // ---- Intel HDA 声卡（kernel/hda64.cpp）：初始化 + 一次可验证的输出证据 ----
    // 位置：usb 之后、gui64_run 之前（那之后不再返回）；没声卡只打一行 [HDA64] not found，
    //   面板/设置页按 hda64_ready64() 如实降级（driver absent；内存态），系统照常启动。
    // 自检会送一段已知 PCM（120ms 1kHz 方波）并断言控制器侧 LPIB/BCIS 前进（[HDA64] selftest PASS）。
    hda64_init64();
    (void)hda64_selftest64();
    // ---- 批次 A 后半：update 标记检查 + preload 预热（都在进桌面之前）----
    // 顺序：update 先查（有 pending 会应用 -> store/ring log -> /update.done -> 自动软重启，不返回）；
    //       没有 pending 就照常往下走，然后 preload 预热字形/图标（打点带 rdtsc64 实测证据）。
    (void)update64_check64();
    (void)preload64_run64();
    // ---- UEFI 运行期 CR3 实验（编译期开关，默认关；见 docs/UEFI地址空间实验报告.md）----
    // 位置：gui64_run 之前建一个**延迟 5 秒**的内核任务 —— 它跑的时候 [GUI64] ready 已经打出来了，
    //   所以即使 VMware EFI 下 mov cr3 真的触发复位，"桌面起来了"这条证据也已经留在串口里。
    // 默认构建（没有 -DPROC64_UEFI_CR3_EXPERIMENT=1）这两行不编进去，行为与之前完全一致。
#if defined(PROC64_UEFI_CR3_EXPERIMENT) && (PROC64_UEFI_CR3_EXPERIMENT == 1)
    (void)proc64_uefi_cr3_experiment_start64();
#endif
    // ==================== Rust 模块自检（项目路线：C + C++ + Rust）====================
    // 位置：所有启动期工作之后、开机滚屏之前 —— 这样 [RUST64] 行会进 boot console 的
    //   回放缓冲（屏上也能看到），并且**在进桌面之前**就证明"Rust 真的在跑"。
    rust64_boot_init64();
    // ==================== 开机滚屏引导控制台（批次 N）====================
    // 位置：所有启动期工作之后、gui64_run 之前 —— 也就是"进桌面之前"屏上跑一遍这次启动真的
    //   跑出来的内核日志（回放缓冲 + 实时追加），停 CON64_STAY_MS 或按任意键，然后进桌面。
    // 开关：config64 的 boot.verbose（默认 1；终端 `boot verbose on|off` 持久化到 store64）。
    //   关掉时只打 [CON64] verbose=0 skipped，缓冲/dmesg 照常。
    (void)con64_boot_screen64(config64_get_bool64("boot.verbose", 1));
    gui64_run(bi);          // 不返回：进入桌面消息循环
}
#endif

extern "C" [[noreturn]] void kmain64(void* arg0, void* arg1) {
    (void)arg0; (void)arg1;

    // 1) 串口先就绪，保证后面每条日志都能看到
    dbg64_serial_init();
    // 1b) 开机滚屏引导控制台：**从第一条日志起**就镜像进环形缓冲（这台机器/这次启动的完整启动日志）。
    //     位置讲究：必须在 bss_clear_64() **之前** —— 缓冲本体在 .data（section(".data.con64")），
    //     清 .bss 不会碰它；而 dbg64 的 sink 指针在 .bss，所以清完还要 con64_rehook64() 重挂。
    con64_init64();

    // 2) 确认我们真的在 64 位长模式下（读 IA32_EFER，LMA 位必须为 1）
    uint32_t efer_lo, efer_hi;
    __asm__ volatile("rdmsr" : "=a"(efer_lo), "=d"(efer_hi) : "c"(0xC0000080));
    dbg64_str("[LM64] ENTERED LONG MODE efer=");
    dbg64_hex64(((uint64_t)efer_hi << 32) | efer_lo);
    dbg64_nl();
    if (!(efer_lo & (1u << 10))) {   // EFER.LMA
        halt_forever("EFER.LMA not set (not in long mode)");
    }

    // 3) 清 .bss（CPU 复位不清 RAM，硬重启后必须自己清）
    dbg64_str("[LM64] bss ");
    dbg64_hex64((uint64_t)(uintptr_t)__bss_start);
    dbg64_str("..");
    dbg64_hex64((uint64_t)(uintptr_t)__bss_end);
    dbg64_nl();
    bss_clear_64();
    con64_rehook64();       // ★ 清 .bss 把 sink 指针清零了：重新挂上（缓冲内容还在，见 console64.h 顶部说明）
    dbg64_str("[LM64] BSS CLEARED");
    dbg64_nl();

    // 4) 读 BootInfo（loader 写在 0x1000）
    BootInfo* bi = (BootInfo*)(uintptr_t)BOOT_INFO_ADDR;
    if (bi->magic != BOOT_INFO_MAGIC) {
        dbg64_str("[LM64] BOOTINFO BAD magic=");
        dbg64_hex64(bi->magic);
        dbg64_nl();
        halt_forever("bad bootinfo magic");
    }
    dbg64_str("[LM64] BOOTINFO OK magic=");
    dbg64_hex64(bi->magic);
    dbg64_nl();

    // 5) 图形模式参数（M1 的 fb 模块要靠它建帧缓冲）
    dbg64_str("[LM64] LFB addr=");
    dbg64_hex64(bi->lfb_addr);
    dbg64_str(" size=");
    dbg64_dec(bi->width); dbg64_str("x"); dbg64_dec(bi->height);
    dbg64_str(" bpp="); dbg64_dec(bi->bpp);
    dbg64_str(" pitch="); dbg64_dec(bi->pitch);
    dbg64_str(" mode=0x"); dbg64_hex64(bi->mode_num);
    dbg64_str(" modes="); dbg64_dec(bi->mode_count);
    dbg64_str(" edid="); dbg64_dec(bi->edid_ok);
    dbg64_nl();

    // 6) 内存地图
    dump_e820(bi);


    // 6b) 64 位内存管理：页池 + 内核堆。
    //     位置讲究：必须在 E820 之后（页池上界来自 E820）、在任何 kmalloc_64 之前。
    //     布局避让三处固定占用（内核 .bss 到 ~36.4MB、安装载荷 64~68MB、内核栈 0x7C000），
    //     详见 kernel/mem64.cpp 顶部注释。自检失败会打印失败掩码，供自动验收定位。
    mem_init_64(bi);
    {
        const int mst = mem_selftest_64();
        if (mst != 0) {
            dbg64_str("[MEM64] selftest FAILED mask=");
            dbg64_dec((uint64_t)mst);
            dbg64_nl();
        }
    }
    // 6c-0) 硬件清单 / ACPI 平台表 / 文件系统自检（两份内核都跑得到）：
    //   hwinfo64：CPUID + PCI 枚举，只往 0xCF8/0xCFC 读设备配置，不写任何设备寄存器；
    //   acpi64  ：只读固件放好的 RSDP/RSDT/XSDT 表，不切 ACPI 模式、不接管中断；
    //   vfs64   ：自检在 64 扇区内**内存假盘**上跑（格式化/写/读/删），对真盘只做只读探测，不写盘。
    //   edid64  ：只读引导层已经读进 0x7600 的 EDID（BootInfo.edid_ok 说了算），
    //             不碰 fb 状态、不写 0x3DA/CRTC —— 设置页/任务管理器的刷新率来自这里。
    //             ★ 两份内核都跑（安装介质也打这两行日志，便于同一份验收脚本核对）。
    hwinfo_init64();  (void)hwinfo_selftest64();
    acpi_init64();    (void)acpi_selftest64();
    edid64_init64();  (void)edid64_selftest64();   // 只读：EDID 解析 + 自检（合成样本离线自证）
    (void)vfs64_selftest64();
    // 6c) 任务系统：把当前执行流登记为任务 0（内核主流程 -> 最终进入桌面消息循环）。
    //     必须在 mem_init_64 之后（任务栈来自内核堆），且在进入桌面之前。
    //     ★ 安装程序内核不链接 task64.cpp，所以整段用宏包起来。
#ifndef VIMTU_INSTALLER_MEDIA
    task_init64();
#endif

    // 7) 64 位能力自检：指针宽度、RIP 相对寻址、CR3 可读
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    dbg64_str("[LM64] ptr_bits=");
    dbg64_dec(sizeof(void*) * 8);
    dbg64_str(" cr3=");
    dbg64_hex64(cr3);
    dbg64_str(" cr4=");
    {
        uint64_t cr4; __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        dbg64_hex64(cr4);
    }
    dbg64_nl();

    // 8) 内核镜像规模（用于核对 loader 的 KERNEL_SECTORS 是否够）
    // ★ 用**虚拟**基址算镜像大小：内核已搬高半区（链接在 0xFFFFFFFF80000000），
    //   而 __bss_end 是高半区地址；用物理基址 ML64_KERNEL_BASE 会得到天文数字。
    uint64_t kb = ((uint64_t)(uintptr_t)__bss_end - ML64_KERNEL_VA_BASE + 1023) / 1024;
    dbg64_str("[LM64] kernel_image_kb=");
    dbg64_dec(kb);
    dbg64_str(" limit_kb=");
    dbg64_dec(ML64_KERNEL_MAX_BYTES / 1024);
    dbg64_nl();


    // 9) 平台层：PIC 重映射 + TSS + IDT + PIT，开中断实测时钟
    //    这是 M1 的第一块地基 —— 中断不走通，后面的任务系统/GUI 都无从谈起。
    dbg64_str("[LM64] x86_init64 ...");
    dbg64_nl();
    x86_init64();
    __asm__ volatile("sti");            // 开中断（IDT 已就绪）

    dbg64_str("[LM64] pre-sti rsp=");
    dbg64_hex64(read_rsp64());
    dbg64_str(" cs=");
    dbg64_hex64(read_cs64());
    dbg64_str(" ticks0=");
    dbg64_dec((uint64_t)g_ticks64);
    dbg64_nl();

    // 用 hlt 等 12 个 tick（约 48ms）：能等到就说明 PIT 中断真的进来了。
    // 带一个防死循环计数器：中断没到就打印 TIMEOUT 而不是永远卡住（便于自动验收定位）。
    uint64_t t0 = g_ticks64;
    uint64_t loop_guard = 0;
    while (g_ticks64 < t0 + 12) {
        if (++loop_guard > 2000000000ULL) {
            dbg64_str("[LM64] PIT IRQ TIMEOUT ticks=");
            dbg64_dec((uint64_t)g_ticks64);
            dbg64_str(" guard=");
            dbg64_dec(loop_guard);
            dbg64_nl();
            break;
        }
        __asm__ volatile("hlt");
    }
    uint64_t t1 = g_ticks64;
    dbg64_str("[LM64] PIT IRQ OK ticks=");
    dbg64_dec(t1 - t0);
    dbg64_str(" irq_total=");
    dbg64_dec(g_irq_total64);
    dbg64_str(" rtc=");
    {
        int hh = 0, mm = 0, ss = 0;
        rtc_get_time64(&hh, &mm, &ss);
        dbg64_dec(hh); dbg64_str(":"); dbg64_dec(mm); dbg64_str(":"); dbg64_dec(ss);
    }
    dbg64_nl();
    dbg64_str("[LM64] M0 PASS");
    dbg64_nl();

    // ==================== 图形栈 + 输入（安装程序的前置条件）====================
    // 安装程序只需要 framebuffer + TrueType 字体 + 键鼠三样；完整的桌面 GUI
    // （gui.cpp / task.cpp / mem.cpp）等 M1 收尾时再接，避免一次引入太多变量。
    fb_init(bi);
    dbg64_str("[G64] fb render=");
    dbg64_dec((uint64_t)fb_width()); dbg64_str("x"); dbg64_dec((uint64_t)fb_height());
    dbg64_str(" phys=");
    dbg64_dec((uint64_t)fb_phys_width()); dbg64_str("x"); dbg64_dec((uint64_t)fb_phys_height());
    dbg64_str(" zoom="); dbg64_dec((uint64_t)fb_get_zoom());
    dbg64_nl();

    font_init();
    dbg64_str("[G64] font faces=");
    dbg64_dec((uint64_t)font_face_count());
    dbg64_str(" line_h=");
    dbg64_dec((uint64_t)font_line_height());
    dbg64_nl();
    font_selftest();     // 四个字体面：加载状态 / 中英 1:2 / 查询链 / 兜底命中 —— [FONT64] 打点（验收脚本按行断言）

    // ★ 踩坑记录（M2：安装程序按键全乱："回车"变成 Esc）：
    //   这段初始化曾经被复制成两份。第二次 kbd_init() 读 8042 命令字节时，
    //   缓冲里还留着 mouse_init() 发 F6/F4 得到的 ACK(0xFA)，于是命令字节被
    //   当成 0xFA|0x01 = 0xFB 写回去：bit6（扫描码集 2 -> 集 1 翻译）被清掉，
    //   键盘从此送来集 2 码，而解码表是集 1 -> 按键全乱。
    //   现在只初始化一次，且 input.cpp 里读写命令字节都会先排空缓冲 + 强制 bit6=1。
    kbd_init();
    mouse_init();
    pic_unmask64(1);                 // IRQ1：键盘
    pic_unmask64(12);                // IRQ12：鼠标（自动级联开放 IRQ2）
    dbg64_str("[G64] input ready (kbd irq1 + mouse irq12)");
    dbg64_nl();

    // ==================== 屏幕硬件检查报告（真机可用性套件 item 5a）====================
    // 为什么放在这里：图形 + 字体 + 键鼠都已经就绪（fb_init/font_init/kbd_init/mouse_init 在上面），
    // 而安装程序/桌面还没开始跑 —— 真机上没有串口，这是用户唯一能在"还没进系统"时看到的诊断画面。
    // 位置的三点理由：
    //   1) 必须在输入就绪之后（任意键跳过要能收到键；kbd_drain 会清掉进页前的按键）；
    //   2) 必须在 setup64_run()/os_boot_path() 之前（那之后不再返回）；
    //   3) 报告内部会确保 ahci64_init64()/display64_init64() 已跑（都是幂等只读探测），
    //      这样"存储/显示"区块在安装程序枚举磁盘之前就有真值。
    // 展示时长 5 秒（规格要求"约 5 秒"），任意键提前结束；打点见 hwui64_show64。
    (void)hwui64_selftest64();
    (void)hwui64_show64(5000);

#ifdef VIMTU_INSTALLER_MEDIA
    // 安装介质**不显示开机滚屏引导控制台**（有意为之）：
    //   向导一开机就在等按键，滚屏会吞掉注入/按下的按键 —— 实测会让 install_flow / esp_install
    //   这类"回车×N 进向导"的脚本失败。滚屏只用于"进入已安装系统"那条路径（见 os_boot_path()）；
    //   启动日志本体照旧进环形缓冲，装好后终端 `dmesg` 仍可看。
    // ==================== 安装介质：进入安装程序（不返回）====================
    // 流程：语言 → 现在安装 → 许可 → 安装类型 → 磁盘与分区 → 安装进度 → 完成/自动重启。
    dbg64_str("[G64] entering setup wizard");
    dbg64_nl();
    setup64_run(bi);
#else
    // ==================== 已安装的系统：系统启动路径 ====================
    // 走到这里说明运行的不是安装介质，而是装到硬盘上的系统（安装程序写进硬盘的
    // 那份内核不带 VIMTU_INSTALLER_MEDIA 宏）。
    os_boot_path(bi);
#endif
}
