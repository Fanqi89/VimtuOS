# musl 1.2.5（Vimtu64 的 A2 实验性第三方目录）

## 这是什么 / 为什么在这里

`docs/应用层与系统调用说明.md` 的"用户态 C 运行时（A2）"一节里有一条**可行性实测**结论：
"musl 能不能在 **Windows 宿主**上交叉构建（x86_64），产出可用的 `libc.a`？"
本目录就是那次实测的**原始工作树**（保留下来便于复现/继续）：

* 源码：musl 1.2.5（上游 <https://musl.libc.org/releases/musl-1.2.5.tar.gz>，原样解包；
  许可见 **`COPYRIGHT`**（MIT，未被修改））。
* 我们加的**只有两样**（其余文件与上游逐字节一致）：
  * `config.mak` —— 手写的构建配置（**绕过 `configure`**，理由见文件头：`configure` 要
    "编译并运行"测试程序，Windows 跑不了 x86_64 Linux ELF）；
  * `tools/ar_win_chunk.py`、`tools/musl_archive_win.py` —— 两个**Windows 宿主专用**的
    归档助手（理由见各自文件头：命令行长度上限 / `ar` 的 r 模式大小写不敏感）。
* 已构建产物（保留着，可直接拿去链接）：`lib/libc.a`（1345 个成员）、
  `obj/include/bits/{alltypes.h,syscall.h}`（生成的位宽/系统调用号头文件）。

## 复现（Windows + MSYS2）

```bash
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
cd third_party/musl
make -j6 obj/crt/crt1.o          # 只编 crt1.o（单文件，命令行短，不受 Windows 限制）
make -j6 lib/libc.a || true      # ★ 最后一条归档命令在 Windows 上必定失败（Argument list too long），
                                 #   但**目标文件在这一步全部编完了**
py -3 tools/musl_archive_win.py  # 用脚本自己分块归档 -> lib/libc.a
```

链接一个静态程序（musl 的 `crt1.o` + 我们的 `hello.c`，编译用 musl 自己的头文件）：

```bash
clang --target=x86_64-linux-gnu -nostdinc \
      -isystem include -isystem arch/x86_64 -isystem obj/include \
      -O2 -c hello.c -o hello.o
ld.lld -m elf_x86_64 -static -o hello_musl.elf obj/crt/crt1.o hello.o lib/libc.a -z noexecstack
```

## 实测结论（详见 docs 里的 A2 节）

| 项目 | 结果 |
| --- | --- |
| 全部源码在 Windows 宿主上用 clang 22.1.8 编成 x86_64 目标文件 | ✅ 通过（无 error，只有上游 header 的几条 `-Wshift-op-parentheses` 警告） |
| 归档出 `lib/libc.a` | ✅ 2,306,000 B / 1345 个成员 |
| 静态链接 `hello.c`（musl `crt1.o`，默认基址 0x200000） | ✅ 27,120 B 的静态 ELF64 EXEC |
| 静态链接到**我们内核的用户窗口**（4GiB，自写 `_start`） | ✅ 30,128 B，段全部落在 `0x100000000..0x1000055e0`（内核对 ELF 段的检查区间内） |
| 用 musl 的 `crt1.o` 链到 4GiB | ❌ `relocation R_X86_64_PC32 out of range … references '_DYNAMIC'`（crt1.o 不开 PIC）——改用自写 `_start` 后通过 |
| **在 Vimtu64 内核里跑起来** | ❌ **没做**（不是"做过没成功"）：需要把 ELF 装进 VimtuFS2 + 给内核加一条启动路径，而本轮 A2 的范围明确要求"不为了它改内核"。风险点已写在 docs：内核的 ELF 初始栈是否带 auxv、musl 启动期要的号段子集（brk/mmap/arch_prctl/set_tid_address…）等 |

## 边界（不要把实验说成产品）

本目录**不参与** `build64.sh` 的构建，也不进内核镜像（内核只用 `user/lib/` 那份自研最小 libc）。
它是"musl 可行性实测"的证据树：证明"Windows 宿主上交叉构建 musl 静态库 + 链接静态 ELF"这条路是通的，
以及卡点分别在哪儿（宿主命令行长度、`ar` 的大小写行为、musl `crt1.o` 的非 PIC `_DYNAMIC` 引用）。
