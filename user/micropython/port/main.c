/* main.c - ★ v0.11.0：VimtuOS 上 MicroPython 的入口（ports/minimal/main.c 的裁剪版）
 *
 * 只做：GC 起来 -> mp_init -> pyexec_friendly_repl() -> mp_deinit。
 * 没有文件系统、没有 frozen 模块，所以 mp_lexer_new_from_file / mp_import_stat
 * 都是“明确失败”的桩（与 minimal 端口同口径）。
 *
 * 入口走 musl 的 __libc_start_main（见 port/mp_start.S / mp_start.c），
 * 理由是 lua 那次实测的结论：musl 的 crt1.o 里 `lea _DYNAMIC(%rip),%rsi`
 * 在 -static 下无处解析，链接期直接报 R_X86_64_PC32 out of range。
 */

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "py/builtin.h"
#include "py/compile.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/repl.h"
#include "py/runtime.h"
#include "py/stackctrl.h"

#include "shared/runtime/pyexec.h"

#if MICROPY_ENABLE_GC
// 静态堆：4 MiB 的 .bss（见 mpconfigport.h 的大小说明）。
// 放在 .bss 里意味着驱动的“p_memsz > p_filesz 尾部清零”会把它清零，且不占文件体积。
static char heap[MICROPY_HEAP_SIZE];
#endif

static char *stack_top;
int main(int argc, char **argv) {
    int stack_dummy;
    stack_top = (char *)&stack_dummy;
    mp_stack_set_top(stack_top);
    mp_stack_set_limit(MICROPY_STACK_LIMIT_BYTES); // MICROPY_STACK_CHECK=0 时是空操作

    #if MICROPY_ENABLE_GC
    gc_init(heap, heap + sizeof(heap));
    #endif

    mp_init();

    #if MICROPY_ENABLE_COMPILER
    // ★ v0.11.6：`mp <脚本>` = 跑一个文件（与上游 `micropython foo.py` 同语义）。
    //   argv 由内核终端经 proc64_start_elf64_argv64 摆好（argv[0] = /bin/mp，argv[1] = 脚本路径）。
    //   走 shared/runtime/pyexec.c 的 pyexec_file()：它自己管 nlr（脚本抛异常只打到 stderr，
    //   不会把解释器打死），源文件由本端口的 mp_lexer_new_from_file() 用 fd 2/0/8 读进来。
    //   没有参数 = 老的 REPL（一条字节都不变）。
    if (argc > 1 && argv != NULL && argv[1] != NULL && argv[1][0] != 0) {
        const int rc = pyexec_file(argv[1]);
        mp_deinit();
        return rc ? 0 : 1;
    }
    pyexec_friendly_repl();
    #endif

    mp_deinit();
    return 0;
}

#if MICROPY_ENABLE_GC
void gc_collect(void) {
    // 保守扫描：setjmp 先把 callee-saved 寄存器落到本帧，再从 &dummy 扫到栈顶。
    // （gc.c 的 gc_collect_start() 已经自动扫过 mp_state_ctx 的根指针区与 pystack，
    //   这里只需要补 C 栈这一段。）
    gc_collect_start();
    jmp_buf env;
    setjmp(env);
    void *dummy = (void *)&env;
    char *sp = (char *)&dummy;
    if (sp < stack_top) {
        gc_collect_root((void **)sp, (size_t)(stack_top - sp) / sizeof(mp_uint_t));
    }
    gc_collect_end();
}
#endif

// ==================== 源文件读取（★ v0.11.6：真文件系统，不再是桩）====================
// pyexec_file() 走的正是这条：mp_lexer_new_from_file(qstr) -> 本函数。
//   实现方式：musl 的 fopen/fread（-> 内核 open/read/lseek/close，都是已实现的系统调用），
//   读进一块**GC 管理的**缓冲（m_new），把 free_len 交给 lexer —— mp_reader_mem_close() 会在
//   lexer 析构时用 m_del(char, buf, free_len) 把它还回堆，不会漏。
// 边界（如实）：整文件读进内存（没有流式 lexer）—— 脚本按"用户自己写的小工具"定位，
//   1 MiB 以内没问题；再大就如实报错（EIO），不假装能跑。
#define MP_SRC_MAX_BYTES (1024 * 1024)
mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
    const char *path = qstr_str(filename);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        mp_raise_OSError(MP_ENOENT);
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); mp_raise_OSError(MP_EIO); }
    const long n = ftell(f);
    if (n < 0 || n > MP_SRC_MAX_BYTES) { fclose(f); mp_raise_OSError(MP_EIO); }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); mp_raise_OSError(MP_EIO); }
    char *buf = m_new(char, (size_t)n + 1);
    const size_t got = (n > 0) ? fread(buf, 1, (size_t)n, f) : 0;
    fclose(f);
    if (got != (size_t)n) {
        m_del(char, buf, (size_t)n + 1);
        mp_raise_OSError(MP_EIO);
    }
    buf[n] = 0;
    return mp_lexer_new_from_str_len(filename, buf, (size_t)n, (size_t)n + 1);
}

// open() 直接 ENOENT。
//   为什么必须在端口侧给：MICROPY_VFS=0 时 py/builtin.h:63 把 mp_builtin_open_obj
//   声明成“由端口提供”（unix 端口由 extmod/vfs 提供），而 py/modbuiltins.c:728 与
//   py/modio.c:214 都会引用它 —— upstream 的 minimal 端口能链得过只是因为它的
//   ROM 级别是 MINIMUM（MICROPY_PY_IO=0，那两处不在表里）。
mp_obj_t mp_builtin_open(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    (void)n_args;
    (void)args;
    (void)kwargs;
    mp_raise_OSError(MP_ENOENT);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, mp_builtin_open);

mp_import_stat_t mp_import_stat(const char *path) {
    (void)path;
    return MP_IMPORT_STAT_NO_EXIST;
}

void nlr_jump_fail(void *val) {
    (void)val;
    for (;;) {
    }
}

void MP_NORETURN __fatal_error(const char *msg) {
    (void)msg;
    for (;;) {
    }
}
