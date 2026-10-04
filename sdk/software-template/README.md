# VimtuOS 软件开发模板（SDK）· `sdk/software-template/`

> 这是**给第三方开发者用的软件开发模板**：在 VimtuOS 上写一个程序、装进系统卷、跑起来、写验收脚本 ——
> 需要的最小骨架、可复制命令、以及**能力边界（哪些现在做不到）**都在这一份里。
>
> **纯用户态 / 纯文档**：本模板不改 `kernel/**`，也不要求你重建内核。
> 所有命令都在 **Windows + MSYS2** 环境里实测跑过；原始输出见 `sdk/software-template/EVIDENCE.md`。

## 0. 先看三条命令（最短路径）

```sh
# ① 构建命令行示例（产出 build64/sdk/hello-cli.elf + hello-cli.bin）
bash sdk/software-template/apps/hello-cli/build.sh

# ② 打成 VAP64 应用包（产出 build64/sdk/hello-cli.vap，含 CRC32 回读校验）
py -3 sdk/software-template/packaging/vap64_pack.py build64/sdk/hello-cli.bin build64/sdk/hello-cli.vap hello-cli

# ③ 装进系统卷 + 拼一块可引导盘（产出 build64/sdk/sdkdisk.img）
py -3 sdk/software-template/packaging/vol_install.py \
      --vol-in build64/shellvol.img --vol-out build64/sdk/sdkvol.img \
      --src build64/sdk/hello-cli.elf:/bin/hello-cli.elf:0755 \
      --system build64/system.img --disk build64/sdk/sdkdisk.img
```

```sh
# 图形程序（/bin/wm 合成器 + shm surface + user/lib/font64.h 画字）
bash sdk/software-template/apps/hello-gui/build.sh
py -3 sdk/software-template/apps/hello-gui/tests/hello_gui_test.py     # 真 QEMU：登录->开终端->elfrun->抓屏
```

## 1. 目录

```
sdk/software-template/
├── README.md                      # 本文件
├── EVIDENCE.md                    # 本轮**实跑**命令与原始输出（构建 / 打包 / 验收 / 图标加载）
├── apps/
│   ├── hello-cli/                 # 示例①：纯命令行（C + user/lib；含 Makefile/build.sh/tests/）
│   └── hello-gui/                 # 示例②：图形（/lib/wm 合成器 + shm surface + 字体栈）
├── include/vimtu/                 # 给第三方用的接口头（目录枚举 ABI 包装，自包含）
├── packaging/                     # VAP64 / 类 deb-tar 骨架 / 装卷器 / 校验
├── icons/                         # 应用图标规范 + 模板自带示例图标（自绘 CC0）
└── tools/                         # 构建自检、图标外置、QEMU 夹具、验收基座
```

## 2. VimtuOS 简介与**能力边界**（先读这三份文档的小节）

VimtuOS（仓库 `Vimtu64`）是一个**x86_64 长模式、自研内核 + Wayland 风格桌面**的小型操作系统：
自有 `int 0x80` ABI、VimtuFS2 卷格式、`/bin/wm` Ring 3 合成器、用户态 C 运行时（`user/lib`）。

写程序之前，请按顺序读这几节（**别只看标题**，里面的"如实边界"就是踩坑清单）：

| 想了解 | 去哪看（小节名） |
|---|---|
| 整体到哪一步了、哪些是"已完成"、哪些是"未做" | `docs/项目状态总览.md` → **`## 1. 当前结论（截至最近一次核查）`**、**`## 3. 下一步：从"能跑自己的程序"到"能跑别人的程序"`** |
| 系统调用一页速览 + 两条入口（`int 0x80` 与 `syscall` 指令） | `docs/应用层与系统调用说明.md` → **`## 1. 一页速览`**、**`## 3. 入口一：int 0x80（Vimtu64 自有 ABI）`**、**`## 4. 入口二：syscall 指令（Linux x86_64 号段）`** |
| 用户内存布局（装载区/栈/brk/mmap 窗各自在哪、多大） | 同上 → **`## 2. 用户内存布局（kernel/usermode64.h，唯一定义点）`** |
| fd / 文件语义 / 管道 / fork / execve | 同上 → **`### 4.2 FD 层与文件语义（批次 D：每进程 fd 表 + 引用计数对象；kernel/fd64.h / kernel/fd64.cpp）`**、**`## 9. 进程与地址空间（批次 C：kernel/proc64.h / kernel/proc64.cpp）`** |
| 目录树与盘符（VimtuFS2 v3 + drive64） | 同上 → **`### 4.3 VimtuFS2 v3 目录树 与 盘符/驱动器枚举层`**、**`### 4.4 已知边界（别把没做的说成做了）`** |
| 可执行格式（VAP64 / ELF64）与"怎么编" | 同上 → **`## 5. 可执行格式一：VAP64（Vimtu64 自有）`**、**`## 6. 可执行格式二：ELF64（kernel/elf64.h + kernel/elf64.cpp）`**、**`## 7. 怎么编一个能在 VimtuOS 上跑的程序`** |
| 用户态 C 运行时（`user/lib` 到底有什么） | 同上 → **`## 用户态 C 运行时（A2）`**（一~六节） |
| 用户态绘图/事件接口（fb_map / fb_flip / input_poll） | 同上 → **`## 用户态绘图与事件接口（A1）`**（一~六节，含"本轮**没做**的"） |
| 图像解码现状（PNG/BMP/JPEG） | 同上 → **`## 批次 P1 附：开始按钮真图与 img64（PNG/BMP；JPEG 未支持）`**（历史：那一批还没有 JPEG）与 **`## 内核内置 JPEG 基线解码（kernel/img64.cpp，v0.4.3 / M38）`**（现状：**基线 JPEG 已支持**，渐进式/算术编码仍明确拒绝） |
| 什么算"能加进内核"、什么只能放用户态 | `docs/内核边界与架构规则.md` → **`## 1. 判据清单`**（含 `### 1.1 什么算「完善内核」…` / `### 1.2 什么算「新功能」（禁止加在内核）` / `### 1.3 边界情况的裁决流程`）+ **`## 3. 本批（busybox）用到的"缺口 → 裁决"实例`**（ring3 没有 `getdents(217)` / 没有 `statfs(137)` / symlink 缺失 三行就是典型样例） |

**能力边界一句话版**（写程序时按这个来，别按 Linux 的直觉来）：

* 有：自有 ABI（`write/read/open/close/exit/getpid/ticks/sleep_ms/fb_map/fb_flip/input_poll/shm_*` +
  `wl_*` 窗口 + **`dir_open/dir_read/dir_close`(50/51/52) 实时目录枚举**）、Linux 兼容号段子集
  （`open/lseek/stat/fstat/getcwd/unlink/fork/execve/wait4/mmap/pipe/ioctl`…）、静态 ELF64 与 VAP64 两种可执行格式、
  跨进程共享内存、Ring 3 合成器、用户态字体栈（`user/lib/font64.h`）。
* 没有（**当前事实**，逐条都有出处）：动态链接器只支持很窄的一种（`tools/dynlink_*`）；**没有** `/proc`、`/dev`、
  符号链接/硬链接、`statfs`、`getdents`；**没有**信号里的停止类、线程、socket 走 fd；
  **没有**包管理器/应用商店（见 §7）；`printf` **没有浮点**（见 §10）。

## 3. 开发环境（Windows + MSYS2）

| 需要 | 位置 / 用法 |
|---|---|
| MSYS2（提供 `bash` / `make` / `nasm` / `objcopy`） | `C:\msys64\`；调用方式固定为 `C:\msys64\usr\bin\bash.exe -l -c "…"`（`-l` 会带上 MSYS2 的登录环境） |
| clang + lld（交叉编译到裸机目标） | MSYS2 的 `C:\msys64\mingw64\bin\{clang.exe, ld.lld.exe}`；脚本里统一 `export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"` |
| nasm | `/c/msys64/usr/bin/nasm`（仓库的 `build64.sh` 用它编 32/64 位汇编演示） |
| Python | 一律 **`py -3`**（Windows 原生；`tests/*.py` 与 `tools/*.py` 都按这个口径写） |
| QEMU | `C:\Program Files\qemu\qemu-system-x86_64.exe`（验收脚本会自动找；也可 `--qemu` 指定） |
| 整个仓库构建（只在你需要内核/系统镜像时） | `C:\msys64\usr\bin\bash.exe -l -c "cd /c/Users/fanqi/Desktop/VimtuOS/Vimtu64 && bash build64.sh"` |

**纪律三条**（仓库一贯的做法，模板的脚本也照做）：

1. `build64.sh` **只调用、不修改**；本模板的所有产物都落在 `build64/sdk/`。
2. 同一时刻**只跑一个 QEMU 重活**（真引导的验收很吃 CPU/磁盘；多个并行会互相把超时打爆）。
3. clang 一律带 **`-Wall -Wextra`**，且要求**零告警**（模板的 Makefile 就是这套选项）。

## 4. 三类应用写法

### 4.1 C + `user/lib`（自研最小 libc，推荐起点）

真源：`user/lib/`（`stdio.h` `stdlib.h` `string.h` `unistd.h` `fcntl.h` `fb.h` `wl.h` `font64.h` `vimtu64.h`）。
它**没有外部依赖**：连 `<stdint.h>` 都是仓库里那份，编译用 `-nostdinc`。

```sh
# 最小骨架（模板 hello-cli 就是这么编的；完整 Makefile 见 apps/hello-cli/Makefile）
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
CC="clang --target=x86_64-unknown-none-elf"
$CC -nostdinc -I user/lib -I sdk/software-template/include/vimtu \
    -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
    -fno-zero-initialized-in-bss -mcmodel=large -mno-red-zone \
    -mno-sse -mno-sse2 -mno-mmx -mno-avx \
    -ffunction-sections -fdata-sections -std=c11 -O2 -Wall -Wextra \
    -c myapp.c -o myapp.o
```

链接两条路（模板一次性出两个产物）：

```sh
# ① ELF64（走 `elfrun`，能 fork/execve/mmap，仓库的 fontdemo 就是这条）
ld.lld -m elf_x86_64 -static --gc-sections -z noexecstack \
       -T user/apps/evshm_demo.ld -o build64/sdk/myapp.elf myapp.o <user/lib 的 .o>
# ② 平铺 blob（走 VAP64 或内核的 blob 装载器）
ld.lld -m elf_x86_64 -static --gc-sections -z noexecstack -T user/lib/user64.ld -o myapp.blob.elf myapp.o <…>
objcopy.exe -O binary myapp.blob.elf build64/sdk/myapp.bin
```

`-mcmodel=large` **必须**：程序链接在 4 GiB（`USER64_CODE_VA64`），small 模型会给全局变量生成
`R_X86_64_32S` 绝对重定位，`lld` 直接报 out of range（`user/build_user.sh` 头部有实测记录）。
`font64.c` 是唯一例外：**它必须开 SSE**（stb_truetype 是浮点代码），见 §5.2 的 GUI 模板 Makefile。

### 4.2 C + musl 静态（大一点的程序：能用的 libc）

仓库已经能用 **musl** 编静态程序（`third_party/musl` + `tools/musl_build_win.sh`），
模板直接把这条线当"第三方 SDK"用：

```sh
# 现成入口：编一个 musl 静态程序（默认源 user/apps/muslhello.c）
C:\msys64\usr\bin\bash.exe -l -c "cd /c/Users/fanqi/Desktop/VimtuOS/Vimtu64 && bash tools/musl_build_win.sh build64/sdk/musl_hello.elf <你的源文件.c>"
```

脚本自己会做硬约束自检（`PT_LOAD` 必须落在 4 GiB..4 GiB+64 KiB、无 `PT_INTERP`/`PT_DYNAMIC`、
程序头表在第一个 `PT_LOAD` 内、入口在段内），与内核装载器 `kernel/elf64.cpp` 的判据逐条一致。

**如实边界**（`docs/应用层与系统调用说明.md` → `## musl（A3 第一步）：musl 静态程序真的在 ring3 跑起来了`、`## 10. 离 glibc 还差什么（如实清单）`）：
* musl 静态构建要 SSE，Ring 3 入口已开 `CR4.OSFXSR/OSXMMEXCPT`；但**没有 xmm 上下文切换**
  （`switch64.asm` 不保存 xmm）—— 多进程同时用 SSE 会互相污染，这是**已知边界**；
* `arch_prctl(ARCH_SET_FS)` 等少数调用走不到；`getdents`/`statfs` 不存在（见 §10）。

### 4.3 Rust（**现状：用户态 Rust 路线待定，如实写**）

* 仓库里目前只有**内核侧**的 Rust：`gui_rs/`（`Token`/主题/UI 常量），由 `build_uefi.sh`/`build64.sh`
  的既有步骤编进内核；终端有 `rust` 命令与 `docs/应用层与系统调用说明.md` →
  **`## 批次 P1：Rust 模块（gui_rs）与 rust 终端命令`** 一节。
* **用户态 Rust（ring3 的 `.elf`/blob）这条路现在没有打通、也没有验证过**：
  没有 `no_std` 的 ring3 target 配置、没有连着 `user/lib` 或自有 ABI 的 crate、没有验收脚本。
  **本模板不给"看起来能用"的假配方** —— 想要的话需要单独立项（target spec + 自有 ABI 的 syscall crate +
  链接脚本 + QEMU 验收），本文档不声称它可用。
* 想现在就写系统程序：用 §4.1 的 C，或 §4.2 的 musl 静态 C。

## 5. GUI 两路（都能用，**推荐第二路**）

### 5.1 路一：`fb_map` / `fb_flip` 直画（简单、但要自己管全屏坐标）

最小骨架（完整可跑版本见 `user/apps/fbdemo.c`）：

```c
#include <stdio.h>
#include <unistd.h>
#include "fb.h"          /* user/lib/fb.h：fb_map / fb_flip / fb_present */
#include "vimtu64.h"

int main(void) {
    uint32_t* fb = 0;
    struct Fb64Info info;
    const int rc = fb_map(&fb, &info);          /* 自有 ABI 9：映射后备缓冲 */
    if (rc != 0) { printf("fb_map FAILED ret=%d\n", rc); return 1; }   /* 如实报错，不假装成功 */

    for (unsigned y = 100; y < 140; y++) {      /* 直接写像素：XRGB8888，pitch 是字节数 */
        uint32_t* row = (uint32_t*)((unsigned char*)fb + (unsigned long)y * info.pitch);
        for (unsigned x = 0; x < 200; x++) row[x] = 0xFFFF0000u;
    }
    printf("[MYAPP] frame flip=%d\n", fb_flip(0, 100, 200, 40));   /* 只提交这块区域 */
    fb_present();                                                  /* 整屏提交 */
    return 0;
}
```

* 越界：完全越界 `fb_flip` 返回 `1`（被拒），部分越界返回 `0`（夹取后提交）——判据见 `docs/…A1` → `### 四、错误码`；
* 这条路**没有窗口概念**：多个程序会互相覆盖；要"窗口/合成"用 5.2。

### 5.2 路二（**推荐**）：`/bin/wm` 合成器 + shm surface

流程（模板 `apps/hello-gui/main.c` 就是完整实现，~150 行）：

```c
#include <fcntl.h>
#include <unistd.h>
#include "vimtu64.h"     /* shm_create / shm_map / __v64_int80 / __v64_syscall */
#include "wl.h"          /* wl_surface_* / wl_seat_get / wl_display_dispatch */
#include "font64.h"      /* 用户态字体栈：font_load / font_draw_text … */

/* ① 拉起 Ring 3 合成器（内核启动期只认 /bin/wm.elf；桌面期由你自己 fork） */
static char* argv[2]; static char* envp[1];
argv[0] = (char*)"/lib/wm.elf"; argv[1] = 0; envp[0] = 0;
if (fork() == 0) {
    __v64_syscall(59 /*execve*/, (long)argv[0], (long)argv, (long)envp, 0, 0);
    __v64_int80(V64_NR_EXIT, 127, 0, 0, 0);
    for (;;) { }
}
vimtu64_sleep_ms(2500);                  /* 它要映射 33 MB 后备缓冲 + 标定 rdtsc（实测 1.5~2 s） */

/* ② shm 画布 + surface（单面 <= 16384 px、单 shm <= 64 KiB） */
const int shm  = shm_create(256 * 56 * 4);
const int surf = wl_surface_create(256, 56, V64_WL_FORMAT_XRGB8888);
void* va = 0; shm_map(shm, 0, 256 * 56 * 4, &va);
struct F64Canvas canvas = { (uint32_t*)va, 256, 56 };

/* ③ 画字（字库从系统卷读，不编进程序） */
struct Font64* f = font_load("/Fonts/NotoSans-Regular.ttf");
font64_add_face(f, "/Fonts-open/NotoSansSC-Regular.ttf", F64_KIND_CJK);
font64_set_spacing(f, F64_SPACING_GRID12);
font64_fill(&canvas, 0xFF101014u);
font_draw_text(f, &canvas, 8, 8, "VimtuOS 你好", 18, 0xFFFFFFFFu);

/* ④ 提交：attach + damage + commit，**然后必须 dispatch 一次**才上屏 */
wl_surface_attach(surf, shm, 0);
wl_surface_damage(surf, 0, 0, 256, 56);
wl_surface_commit(surf);
wl_display_dispatch(0, 0, 0);            /* 0 = 不取事件，只把提交合成上屏 */
```

文字这一路的账号口径（与内核同式，逐条对照见 `user/lib/font64.h` 头部）：
缩放 `size*1024/upem`、推进 `(advance*scale+512)>>10`（下限 1）、行高 `size+4`、
锚点 = 升部顶端、查询链 `主面 → 中文面 → 兜底面 → 缺字占位`；**不做** kerning/复杂脚本整形/亚像素定位。

## 6. 装进系统卷（**确切命令**）与"内核里不该出现你的程序字节"

### 6.1 仓库既有入口

```sh
# 只编一个用户态程序（等价于 user/build_user.sh <name>；源必须是 user/apps/<name>.c）
C:\msys64\usr\bin\bash.exe -l -c "cd /c/Users/fanqi/Desktop/VimtuOS/Vimtu64 && bash build64.sh --user <name>"

# 装卷：本仓库每个交付物都有自己的 *_pack_win.py（同一个卷链，--vol-in 接上一步的 --vol-out）
py -3 tools/demo_pack_win.py --build build64 --vol-in build64/drvsvcvol.img \
      --vol-out build64/demovol.img --system build64/system.img --disk build64/sysdisk.img
```

卷链的顺序（`build64.sh` 里）：`make_shellvol → tcc → lua → gzip → edit → assets → make → tar → drvdemo →
demo → busybox → sounder`，最终盘的路径是 **`build64/sysdisk.img`**。

### 6.2 模板的做法（不依赖任何既有 `*_pack_win.py`）

```sh
py -3 sdk/software-template/packaging/vol_install.py \
      --vol-in  build64/shellvol.img \
      --vol-out build64/sdk/sdkvol.img \
      --src build64/sdk/hello-cli.elf:/bin/hello-cli.elf:0755 \
      --src build64/sdk/hello-cli.vap:/apps/hello-cli/hello-cli.vap:0755 \
      --system  build64/system.img \
      --disk    build64/sdk/sdkdisk.img
```

它做三件事并且**每一步都自证**：写卷（复用 `tools/tcc_pack_win.VolumeEdit`）→ 逐字节回读（独立解析卷格式）
→ 拼盘（`tools/make_shellvol.build_disk`，MBR + 主分区 @ LBA 8009）。

### 6.3 "内核里不该出现你的程序字节" —— 构建期断言

本仓库的硬纪律：**可交付程序不内嵌进内核**，只从系统卷装载；构建期用
`tools/probe64.py` 在内核二进制里**搜**这些字节，搜到就**构建失败**：

```sh
py -3 tools/probe64.py --kernel build64/kernel64_os.bin --mode mid build64/sdk/hello-cli.elf
# 判据（见 tools/probe64.py 头部注释）：⓪整份文件搜不到 ①探针窗口搜不到 ③无候选即泄漏 ④任一 1056 B 连续块搜不到
```

`build64.sh` 末尾对 `/bin/fontdemo.elf` + 四份字库 + `/bin/wm.elf` 都有这条断言 —— 你的程序想守住同一条纪律，
在装卷前把上面那条命令跑一遍即可（本模板的产物**不在**内核里，因为根本没人 objcopy 它）。

## 7. 打包与安装

### 7.1 VAP64（自有可安装应用格式）

字段（唯一定义点 `kernel/app64.h`；打包器 `tools/make_vap.py`，模板 `packaging/vap64_pack.py` 直接复用它）：

| 偏移 | 大小 | 字段 | 含义 / 约束 |
|---|---|---|---|
| 0 | 8 | `magic` | `"VAP64\0\0\0"` |
| 8 | 4 | `version` | 1 |
| 12 | 4 | `header_size` | 32 |
| 16 | 4 | `entry_offset` | 代码入口在**文件内**的绝对偏移 = 32 + `name_len` |
| 20 | 4 | `code_size` | 1..32768（用户代码页 8 页上限） |
| 24 | 4 | `code_crc32` | `zlib.crc32(代码段)`（与内核同一口径） |
| 28 | 4 | `name_len` | 含结尾 NUL，1..24 |
| 32 | n | `name` | NUL 结尾；可见 ASCII，不含 `/` `\` |
| 32+n | m | `code` | 平铺代码段，入口在段首 |

```sh
py -3 sdk/software-template/packaging/vap64_pack.py build64/sdk/hello-cli.bin build64/sdk/hello-cli.vap hello-cli
py -3 sdk/software-template/packaging/vap64_pack.py --verify build64/sdk/hello-cli.vap   # 独立解析器复验（不信打包器自检）
```

### 7.2 "类 deb / tar"骨架：`VTAR64`（**简单包骨架**）

先如实说清：**VimtuOS 没有 dpkg/apt/仓库/依赖解析，也没有"应用商店"**（这点在
`docs/应用层与系统调用说明.md` → `## 附 A：update / preload / proc 终端命令` 里也写明了：
`update` 只是"标记 -> 应用 -> 重启"的机制闭环，**不是真正的升级包**）。
本模板给的是**自证的归档骨架**：把"程序 + 图标 + 元数据 + 校验和"打成一个文件，便于分发/核对/解包。

格式（见 `packaging/vtar64_pack.py` 头部逐字段说明）：
`magic "VTAR64\1\0"` + `entry_count` + `total_bytes` + 16B 保留 + 条目表（每条 64B：名字/偏移/长度/模式/CRC32）
+ 载荷 + 尾部 `"VTAREND\1"` + **整包 sha256**；包内还塞一份 `SHA256SUMS`（类 deb/tar 惯例）。

```sh
py -3 sdk/software-template/packaging/vtar64_pack.py -o build64/sdk/hello-cli.vtar64 \
      --add 0755:bin/hello-cli.elf=build64/sdk/hello-cli.elf \
      --add 0755:apps/hello-cli/hello-cli.vap=build64/sdk/hello-cli.vap
py -3 sdk/software-template/packaging/vtar64_pack.py --list   build64/sdk/hello-cli.vtar64
py -3 sdk/software-template/packaging/vtar64_pack.py --verify build64/sdk/hello-cli.vtar64   # 逐条 CRC32 + 整包 sha256
py -3 sdk/software-template/packaging/vtar64_pack.py --extract build64/sdk/hello-cli.vtar64 --dest build64/sdk/unpacked
```

**与真 deb 的差距（如实）**：没有 `control` 的 `Depends/Conflicts`、没有 `preinst/postinst`、
没有签名（VimtuFS2 没有这个字段）、没有版本升级/回滚策略、没有多用户安装记录。

### 7.3 安装位置（约定）与权限

| 内容 | 卷内路径 | 模式 |
|---|---|---|
| 可执行程序（ELF64） | `/bin/<名字>.elf` | `0755` |
| 应用目录（包/资源） | `/apps/<名字>/…` | `0755` / `0644` |
| 库 | `/lib/<名字>.elf`、`.so` | `0755` |
| 配置/清单 | `/etc/…` | `0644` |
| 图标 | `/icons/{system,apps}/<名字>@<尺寸>.png` | `0644` |
| 字库 | `/Fonts/`、`/Fonts-open/` | `0644` |

权限自批次 P4 起**真拦截**（卷 v4 有 `uid/gid/mode`，见 `docs/应用层与系统调用说明.md` →
`## 批次 P4：权限（卷 v4）+ 身份 / 权限系统调用真值`）；**v3/v2 旧卷没有权限字段 → 不拦截**。

## 8. 图标规范（`gui_rs/assets/`）

真源结构（本仓库已落地）：

```
gui_rs/assets/
├── icons/
│   ├── system/   *.svg      # 系统/状态/导航/文件类型/文件操作图标（白 + alpha 掩码，运行期按主题着色）
│   ├── apps/     *.svg      # 应用图标（掩码 + 应用调色板亮色）
│   ├── LICENSE              # Bootstrap Icons 的 MIT 许可**原文**
│   └── SOURCES.md           # 逐文件上游 URL + sha256（可复现）
├── images/
│   ├── wallpapers/  *.png   # 壁纸（CC0，自绘样例）
│   └── avatars/     *.png   # 头像（CC0，自绘样例）
└── fonts/                   # 图标字体（Material Icons, Apache-2.0；当前内核**没有**图标字体渲染能力）
```

规矩：

1. **可以用 SVG**（本仓库自研光栅器 `tools/svg2png.py` 只支持**纯 fill 路径**：`stroke`/`filter`/`mask`/`<use>`/`<text>`/渐变
   一律**明确报错**，不会静默画错）；运行期内核只解 **PNG / BMP / JPEG**（JPEG 是批次 v0.4.3 才补上的**基线**解码：
   SOF0/SOF1、4:4:4/4:2:2/4:2:0、RSTn、EXIF Orientation，**渐进式/算术编码明确拒绝**）。
2. **尺寸建议 16 / 24 / 32 / 48 px**（内核自带图标包就是这四档：系统图标按 24 取、应用图标按 48 取；只缩小不放大的策略）。
3. **许可只收 MIT / ISC / Apache-2.0 / CC0**，且**落盘许可原文** + 在 `SOURCES.md` 记 `URL + sha256`。
   本仓库禁用：GPL/AGPL 图标集（如 Papirus）、需要商标授权的品牌 logo、来源不明的"免费素材包"。
4. **图标外置，不进内核**：系统卷里 `/icons/system/<名字>@<尺寸>.png`（或 `/icons/apps/…`）**优先于**内核里的图标包，
   打点变成 `[ICON64] load kind=<名字> path=/icons/… src=vfs ok=1`。用它装新图标：

```sh
# 把 gui_rs/assets/icons 下的 SVG 光栅化成 16/24/32/48 PNG，并按内核的查找路径装进卷
py -3 sdk/software-template/tools/iconpack_ext.py \
      --vol-in build64/shellvol.img --vol-out build64/sdk/iconvol.img \
      --system build64/system.img --disk build64/sdk/icondisk.img
# 只光栅化（看像素证据，不写卷）：py -3 sdk/software-template/tools/iconpack_ext.py --only folder --no-vol
```

★ **如实边界**：内核只**主动请求**它自己那张 `kIcon64Names` 表里的 30 个 kind（系统 20 + 应用 10，见
`kernel/icons64.cpp:47-63`）。你新起的名字（例如 `folder`、`copy`、`paste`）**文件装进卷了、但内核不会主动去读**，
要等内核侧登记对应的 kind（本模板按纪律**不改 `kernel/**`**）。所以：**同名覆盖**走 `src=vfs` 立刻生效，
**新名字**目前只能作为"应用自己的资源"（应用自己 `open`/`img_decode` 或将来登记）。

## 9. 调试与验收

### 9.1 串口怎么看

```sh
# 交互看：QEMU 把串口接到当前终端
qemu-system-x86_64 -drive format=raw,file=build64/sysdisk.img -boot order=c -m 512 -vga std -serial stdio
# 脚本看：写文件 + telnet monitor 注入键盘/抓屏（仓库 tests/*.py 的统一姿势）
qemu-system-x86_64 … -serial file:build64/serial.log -monitor telnet:127.0.0.1:5677,server,nowait -display none
```

内核的打点就是**唯一的调试真相**：`[SYSCALL]`（每次系统调用的编号/参数/返回值）、`[PROC64]`、
`[WL64]`、`[PERM64]`、`[ELF64] reject reason=…`、`[DIR64]`、`[ICON64]`…… 写验收脚本就是 grep 这些串。
`[SYSCALL]` 追踪会把一行输出拆开 —— 所以**一行只准一次 `printf`**（换行即 flush）。

### 9.2 每个应用自带 `tests/<名字>_test.py`（**不许削弱断言**）

模板两个应用都给了一份可直接跑的验收脚本（真 QEMU + 真串口 + 真像素）：

```sh
py -3 sdk/software-template/apps/hello-cli/tests/hello_cli_test.py
py -3 sdk/software-template/apps/hello-gui/tests/hello_gui_test.py
```

最小断言模板（照抄就能用；基座在 `tools/sdk_qemu.py`）：

```python
import os, re, sys
SDK = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(SDK, "tools"))
import sdk_qemu as sq

ok = True
def check(name, cond, detail=""):
    global ok
    ok = ok and bool(cond)
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

img = sq.build_fixture("build64/sdk/my_test.img",       # ① 造夹具盘（system.img + 卷）
                       [("build64/sdk/myapp.elf", "/bin/myapp.elf", 0o755), (host, vpath, 0o644)])
qemu = sq.find_qemu(); port = sq.free_port(); tmp, serial = sq.temp_serial()
vm = sq.QemuVm(qemu, img, port, serial, name="vimtu-sdk-myapp")
try:
    mon = sq.Monitor(port)
    check("[GUI64] ready", sq.login_desktop(mon, vm))                    # ② 登录进桌面
    check("终端打开", sq.open_terminal(mon, vm))
    sq.type_line(mon, "elfrun /bin/myapp.elf")                            # ③ 打字跑你的程序
    m = vm.wait_re(r"\[MYAPP\] ver=1 pid=(\d+)", 60)
    check("程序起来了", m is not None, m.group(0) if m else "没等到")
    log = vm.log()
    check("关键打点齐全", re.search(r"\[MYAPP\] summary .* rc=0", log) is not None)
    check("无 PANIC", "PANIC" not in log and "TRIPLE FAULT" not in log)
    p = os.path.join(tmp, "shot.ppm")
    if mon.shot(p):                                                       # ④ 上屏像素
        w, h, px = sq.read_ppm(p)
        check("画面上有我的颜色", sq.count_color(px, w, h, (255, 0, 0), tol=16) >= 200)
finally:
    vm.close()                                                            # ⑤ **别把 VM 挂着**
print("结论：%s" % ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
```

纪律：

* **不许削弱断言**：拿不到证据就 FAIL（模板里的失败信息都印出"没等到"或实测值，不做"看起来对"的结论）；
* **同一时刻只跑一个 QEMU 重活**（本仓库的约定；并行会把超时打爆）；
* `vm.close()` 一定在 `finally` 里（脚本崩了也不能留 QEMU 进程）；
* 截图用 monitor `screendump`（PPM），判据用**像素计数/形状 IoU**，别用"人眼看图"。

## 10. FAQ / 限制（当前事实，逐条给证据口径）

| 问题 | 现状（**如实**） |
|---|---|
| 有符号链接吗？ | **没有**。没有 `symlink`/`link`，`lstat` 与 `stat` 同一实现（`docs/应用层与系统调用说明.md` → `### 4.2` 的表：`lstat`）、`### 4.4 已知边界` 也写了"没有符号链接/硬链接"。多入口程序靠**包装程序**（如 `/bin/<applet>` execve 到 `/bin/busybox`）。 |
| 怎么列目录？相对路径行不行？ | **`dir_open`(50) / `dir_read`(51) / `dir_close`(52)** —— Ring 3 实时枚举，返回名字/类型/大小/mtime（`kernel/syscall64.h` 的"目录枚举 ABI"一节）。**路径必须是绝对路径**；句柄是 fd 表里的槽（上限 8，用完必须 close，否则 EMFILE）。模板的 `include/vimtu/dir64.h` + `dir64.c` 是现成包装。 |
| 老卷/老路径怎么办？ | 兜底 = 构建期快照 **`/etc/vimtu.dirs`**（每行 `<目录>` TAB `<名字>`）。**它是上次构建的快照：本次开机新建的文件看不到**（`user/busybox/vimtu_dirent.c` 与 `dir64_test` 的⑥就是这条对照）。 |
| 管道会阻塞吗？ | **不会阻塞 —— 默认就是非阻塞**：写满 → 短写，完全没空间 `-EAGAIN`；读空且写端还开着 → `-EAGAIN`；写端全关 → 读返回 0（EOF）。管道缓冲只有 **64 B**。**没有** `select/poll/阻塞语义`（`docs/应用层与系统调用说明.md` → `### 4.2 FD 层与文件语义` 的 `pipe(22)` 行）。 |
| 有 `/proc` 吗？ | **没有**。也没 `/dev`。设备在 fd 层没有节点；想知道进程/任务用**内核**的 `proc`/`ps` 或终端 `proc list`（`docs/应用层与系统调用说明.md` → `## 附 A` 的 `A.3 proc`）。 |
| `df` 能用吗？ | **`statfs(137)` 目前不存在**：`df` 会**明确失败**（非 0 退出，不是崩）。缺口在 `docs/内核边界与架构规则.md` 的缺口表（"ring3 没有 statfs(137)"那行）与 `docs/应用层与系统调用说明.md` 的对应说明里都标了 **GAP**。所以：卷容量现在只能靠**内核侧**终端 `df`（内核自己看超级块）。 |
| `printf` 支持哪些格式？ | `%s %c %d %i %u %x %X %p %%` + 长度修饰 `l/ll/z` + `-`/`0` 宽度；**没有 `%f/%e/%g`、没有精度、没有星号宽度**（遇到按字面输出，绝不假装算过 —— `user/lib/stdio.h` 头部）。要浮点自己定点化。 |
| 单次读写上限？ | `write(1,…)` 单次 **1024 B**（`write()` 包装会自动分块）；`read` 单次 **4096 B**（`### 4.2` 的表）。 |
| 单文件多大？ | **≤ 8 MiB（8,388,608 B）**（VimtuFS2 批次 M：4 直接块 + 一级间接 + **二级间接**）；v2 旧卷仍是 67,584 B。名字 ≤31 B、整条路径 ≤128 B、最多 16 段。 |
| 内存限额？ | 用户窗口 4 GiB..4 GiB+64 KiB（ELF `PT_LOAD` 只能落这里）；brk 区 **64 KiB**；`mmap` 是 bump 分配器（窗 4 GiB+0x90000..窗口末）；**单 shm 对象 ≤ 64 KiB**、**单 surface ≤ 16384 px**；blob 装载器 ≤ **32768 B（8 页）**；ELF 文件 ≤ 96 KiB（内核读盘缓冲）。 |
| 我能用多少 CPU/线程？ | 没有线程；有 `fork`（**整页物理复制，不做 COW**）、`execve`、`wait4`（5 秒有界）、信号子集（`SIGKILL` 立刻终止；停止类不实现）。 |
| 有应用商店/包管理器吗？ | **没有**。见 §7.2（`update` 只是机制闭环，不是升级包）。分发 = 文件 + 校验和（VAP64 / VTAR64）。 |
| 我能改内核吗？ | 按 `docs/内核边界与架构规则.md` 的判据：用户态**必需**且内核只是"参数校验 + 转发"的 ABI 补齐可以加；其余一律用户态解决。**本模板一行内核都没改。** |

## 11. 本轮实测（原始输出）

`EVIDENCE.md` 里是这份模板自己跑出来的原始输出：两个程序的构建、VAP64/VTAR64 打包与独立校验、装卷回读、
图标光栅化与 `[ICON64] … src=vfs` 加载证据、以及两份 `tests/*_test.py` 的逐条 PASS/FAIL。
**报告里的数字都从那里来**，不在这里复述。
