/* demo_tiny.c - ★ A4-2b：给 **ring3 里的 tcc** 用的最小演示源码（`-c` 只编不链）
 *
 * 为什么故意不 #include 任何头：用户窗口只有 1 MiB（见 README 的内存账），tcc 的堆只有
 *   ~150 KiB 量级；带 musl 头的那份（demo_headers.c）内存需求高得多。这一份只用**语言本身**，
 *   用来把"tcc 真的在 ring3 里生成了一个 ELF64 目标文件"这件事测清楚。
 *
 * 验收脚本（tests/tcc64_test.py）会断言：
 *   run /bin/tcc -c /tcc/demo/demo_tiny.c -o /tmp/demo_tiny.o
 * 产出的 /tmp/demo_tiny.o 开头是 7f 45 4c 46 02 01 01 00（ELF64 小端）+ e_type=1(ET_REL)
 *   + e_machine=0x3e(x86_64)，并且符号表里有 tcc_demo_add / tcc_demo_mul（cat 出来的证据）。
 */
int tcc_demo_add(int a, int b) { return a + b; }
int tcc_demo_mul(int a, int b) { return a * b; }

int g_tcc_demo_counter = 7;

int tcc_demo_loop(int n) {
    int s = 0;
    for (int i = 0; i < n; i++) s += i;
    return s + g_tcc_demo_counter;
}
