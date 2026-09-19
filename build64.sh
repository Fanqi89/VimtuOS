#!/bin/bash
# build64.sh - VimtuOS 纯 64 位构建（安装介质 + 系统镜像载荷）
#
# 产出三样东西：
#   1) build64/kernel64.bin     安装介质的**安装程序**内核（-DVIMTU_INSTALLER_MEDIA）
#   2) build64/kernel64_os.bin  装进硬盘的**系统**内核（不带该宏 -> 走系统启动路径）
#   3) build64/system.img       系统镜像载荷（= 装进硬盘的完整磁盘映像，8073 扇区）
#      以及 vimtu64-64.img      安装介质：引导链 + 安装程序 + 载荷（system.img）
#
# 镜像布局（必须与 kernel/memlayout64.h、boot/loader64.asm 三方一致）：
#   LBA 0            : boot.bin        （512B MBR）
#   LBA 1..8         : loader64.bin    （实模式准备 + 进长模式 + 64 位 ATA 读内核）
#   LBA 9..8008      : kernel*.bin     （最多 4MB）
#   LBA 8009..8072   : 设置持久化保留区（64 扇区）
#   ---- 以上 8073 扇区就是"一份完整的系统磁盘映像"（system.img 的内容）----
#   LBA 8192         : 载荷头（magic "VIMTUPAY" + 载荷扇区数 + 载荷 LBA）
#   LBA 8193..16265  : 载荷本体（system.img 的副本）
set -e

cd "$(dirname "$0")"
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

CXX="clang++ --target=x86_64-elf"
LD=ld.lld
LLDLINK=lld-link
EFI_CC=clang                   # UEFI 应用用 C 编译（clang++ 会把 .c 当 C++）
PY="${PYTHON:-python}"
OBJCOPY=objcopy.exe
NASM=nasm

BUILD=build64                  # 64 位构建对象目录
RES=build                      # 资源中间产物目录（字体/图标/logo 由 _*.py 生成到这里）
IMG=vimtu64-64.img
KERNEL_LBA=9
KERNEL_SECTORS=8000            # 与 kernel/memlayout64.h、boot/loader64.asm 一致（4MB）
STORE_SECTORS=64
PAYLOAD_LBA=8192               # 载荷头所在 LBA（安装程序按这个值找载荷）
SYS_SECTORS=$((KERNEL_LBA + KERNEL_SECTORS + STORE_SECTORS))          # 8073 = 系统镜像大小
IMAGE_SECTORS=$((PAYLOAD_LBA + 1 + SYS_SECTORS))                     # 16266 = 安装介质大小

# 安装程序内核的编译标志（多两个宏：它要跑安装界面，并知道载荷在哪）
CXXFLAGS="-target x86_64-elf -ffreestanding -nostdlib -fno-stack-protector -fno-pic -fno-pie \
 -fno-builtin -fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-threadsafe-statics \
 -fno-asynchronous-unwind-tables -fno-unwind-tables \
 -mno-red-zone -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -mno-avx \
 -Wall -Wextra -std=c++17 -O2"
# ★ 一次性实测钩子（默认空，不影响任何既有构建）：
#   批次 C 要实测"UEFI（固件页表）下运行期 mov cr3 到底行不行"，那次构建需要
#   -DPROC64_UEFI_CR3_EXPERIMENT=1。用法：VIMTU_EXTRA_CXXFLAGS=-DPROC64_UEFI_CR3_EXPERIMENT=1 bash build64.sh
CXXFLAGS="$CXXFLAGS ${VIMTU_EXTRA_CXXFLAGS:-}"
CXXFLAGS_INSTALLER="$CXXFLAGS -DVIMTU_INSTALLER_MEDIA=1 -DVIMTU_PAYLOAD_LBA=$PAYLOAD_LBA -DVIMTU_KBD_TRACE=1"

# 两套源文件清单：
#   核心 = 平台层 + 图形 + 输入 + 用户态/系统调用地基（安装介质与装好的系统都要；
#          usermode64/syscall64 进两份内核：x86_64.cpp 的 int 0x80 分发必须能链接，
#          "进 ring3 跑用户程序"只在系统内核里调用，见 kernel64.cpp 的 os_boot_path）
#   安装 = 核心 + ATA 驱动 + 分区/安装引擎 + 安装界面
SRCS_CORE="kernel/kernel64.cpp kernel/x86_64.cpp kernel/fb.cpp kernel/font.cpp kernel/input.cpp kernel/mem64.cpp kernel/hwinfo64.cpp kernel/acpi64.cpp kernel/edid64.cpp kernel/display64.cpp kernel/fd64.cpp kernel/usermode64.cpp kernel/syscall64.cpp"
SRCS_INSTALLER="$SRCS_CORE kernel/ata64.cpp kernel/part64.cpp kernel/setup64.cpp kernel/vfs64.cpp"
# 桌面外壳 + 应用：**只编进系统内核**（安装介质走向导，不带桌面，省 4MB 内核区空间）
#   注意：这几个文件依赖 kernel/gui64.cpp 提供的外壳实现，任何新增应用都要同时加进这里。
SRCS_DESKTOP="kernel/gui64.cpp kernel/calc64.cpp kernel/mines64.cpp \
 kernel/terminal64.cpp kernel/settings64.cpp kernel/taskmgr64.cpp"
# 这四个子系统 + 批次 A 后半的两个：**只进系统内核**（安装介质不链它们；只有系统内核有 gui64/store64/app64）
#   sysstate64 = 运行状态机 + 模块注册表 + 健康 + ring log（terminal 的 state/health/syslog）
#   config64   = 类型化配置 KV，落在 store64（VimtuFS2 的 /store.a|b，真落盘）
#   session64  = 会话/应用内容策略（关窗清状态、退出保存、启动恢复）
#   preload64  = 字形预光栅化 + 图标预缩放（batch A 后半；gui64 的图标缓存 + font 的 prewarm 原语）
#   update64   = 标记文件 -> 应用动作 -> store/ring log/自动重启（不是真"升级包"，见 update64.h）
SRCS_SYS="kernel/sysstate64.cpp kernel/config64.cpp kernel/session64.cpp kernel/panic64.cpp kernel/preload64.cpp kernel/update64.cpp"
# ★ 批次 C：kernel/proc64.cpp（进程/地址空间）只进系统内核 —— 它依赖 task64/elf64/vfs64。
SRCS_OS="$SRCS_CORE $SRCS_DESKTOP $SRCS_SYS kernel/task64.cpp kernel/vfs64.cpp kernel/store64.cpp kernel/ata64.cpp kernel/app64.cpp kernel/elf64.cpp kernel/proc64.cpp kernel/e1000_64.cpp kernel/net64.cpp kernel/apic64.cpp kernel/smp64.cpp kernel/usb64.cpp"
# elf64.cpp = ELF64 加载器：**只进系统内核**（安装介质不需要它；它内嵌的 hello.elf 是系统程序）
# apic64.cpp = LAPIC + IOAPIC 接管中断路由：**只进系统内核**（安装链保持纯 8259 PIC，
#   避免影响安装介质内核的字节级断言；x86_64.cpp 对它的 EOI/掩码分派用 weak 引用，不链也不报错）

echo "==> 清理 $BUILD"
rm -rf "$BUILD"
mkdir -p "$BUILD" "$BUILD/os" "$RES"   # $RES：资源脚本（_*.py）会往这里写字体/图标，必须先建好

echo "==> 编译引导扇区（MBR）与 64 位 loader"
$NASM -f bin boot/boot.asm -o "$BUILD/boot.bin"
$NASM -f bin -O2 -i boot/ boot/loader64.asm -o "$BUILD/loader64.bin"
$NASM -f bin -O2 boot/cdiso.asm    -o "$BUILD/cdiso.bin"
$NASM -f bin -O2 boot/hybrid_mbr.asm -o "$BUILD/hybrid_mbr.bin"   # 写进 ISO 第 0 扇区（U 盘/硬盘引导）
echo "==> 编译 UEFI 引导（两段式：极小 PE 桩 + 平铺长模式引导器）"
# 见 build_uefi.sh 的说明：EDK2 的 PE 加载器会拒绝我们那个 ~8KB 的 PE（头/段/重定位/选项都合规，
# 但 UEFI Shell 报 "is not an image"；同工具链的 1.5–2.5KB 小 PE 能正常加载）。所以：
#   BOOTX64.EFI  = 只做 EFI 交互的极小 PE 桩（自研 PE32+，不用 gnu-efi）
#   UEFI64.BIN   = 我们的引导逻辑，平铺长模式二进制（链接在 0x800000，绕过所有 PE 校验）
bash build_uefi.sh "$BUILD"

echo "==> 编译内核汇编"
$NASM -f elf64 kernel/entry64.asm    -o "$BUILD/entry64.o"
$NASM -f elf64 kernel/isr_stubs64.asm -o "$BUILD/isr_stubs64.o"
$NASM -f elf64 kernel/switch64.asm   -o "$BUILD/switch64.o"
# syscall 指令入口（LSTAR 指向它）：两份内核都要链（syscall64.cpp 在 SRCS_CORE 里引用它）
$NASM -f elf64 kernel/syscall_entry64.asm -o "$BUILD/syscall_entry64.o"

echo "==> 准备资源（字体 / logo / 图标；生成脚本产物落在 build/，两份内核都要嵌）"
"$PY" _otf2ttf.py
"$PY" _subset_fonts.py
"$PY" _subsetsimhei.py
"$PY" _make_logo.py
"$PY" _make_icons.py
"$PY" _make_start_icon.py
cp build/font_bahnschrift.ttf build/font_chaparral.ttf build/font_simhei.ttf "$BUILD/"
cp build/logo_rgba.bin build/icon_mycomputer.bin build/icon_recyclebin.bin \
   build/icon_terminal.bin build/icon_start.bin "$BUILD/"

echo "==> 编译 64 位内核对象（两套：OS 与 安装程序）"
# --- 系统内核（装进硬盘后运行）---
echo "    [OS 内核]"
for src in $SRCS_OS; do
    base="$(basename "${src%.cpp}")"
    $CXX -c "$src" -o "$BUILD/os/$base.o" $CXXFLAGS
    echo "      $src -> $BUILD/os/$base.o"
done
# --- 安装介质内核（跑安装程序）---
echo "    [安装程序内核]"
for src in $SRCS_CORE kernel/ata64.cpp kernel/part64.cpp kernel/setup64.cpp kernel/vfs64.cpp; do
    base="$(basename "${src%.cpp}")"
    $CXX -c "$src" -o "$BUILD/$base.o" $CXXFLAGS_INSTALLER
    echo "      $src -> $BUILD/$base.o"
done

echo "==> 资源对象（objcopy -> elf64，两份内核共用同一批）"
(cd "$BUILD" && $OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 font_bahnschrift.ttf font_bahnschrift.o)
(cd "$BUILD" && $OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 font_chaparral.ttf font_chaparral.o)
(cd "$BUILD" && $OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 font_simhei.ttf font_simhei.o)
(cd "$BUILD" && $OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 logo_rgba.bin logo_rgba.o)
(cd "$BUILD" && $OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 icon_mycomputer.bin icon_mycomputer.o)
(cd "$BUILD" && $OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 icon_recyclebin.bin icon_recyclebin.o)
(cd "$BUILD" && $OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 icon_terminal.bin icon_terminal.o)
(cd "$BUILD" && $OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 icon_start.bin icon_start.o)
# OS 内核用同一批资源对象（直接复用）
cp "$BUILD"/font_*.o "$BUILD"/logo_rgba.o "$BUILD"/icon_*.o "$BUILD/os/"

echo "==> 用户态演示程序（真实 ring3 代码：nasm 平铺二进制 -> objcopy 嵌入内核）"
# user/demo64.asm 是**用户态**程序（ring3，用 int 0x80 与内核通信），不是内核代码：
#   nasm 出平铺二进制 -> objcopy 变 elf64 目标文件 -> 链进两份内核（成本很低）。
#   符号名由 objcopy 的输入路径决定，usermode64.cpp 里按 _binary_build64_user_demo64_bin_* 引用。
$NASM -f bin user/demo64.asm -o "$BUILD/user_demo64.bin"
$OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 "$BUILD/user_demo64.bin" "$BUILD/user_demo64.o"
cp "$BUILD/user_demo64.o" "$BUILD/os/user_demo64.o"
echo "==> 可安装应用示例（VAP64：nasm -> tools/make_vap.py -> objcopy 嵌入系统内核）"
# user/hello64.asm 是 ring3 程序；tools/make_vap.py 给它加 32B VAP64 头（含代码段 CRC32）；
# objcopy 把整个 .vap 嵌进内核，app64.cpp 启动时把它装进 VimtuFS2 的 /hello.vap，再从盘上读出来跑。
# 符号名由 objcopy 按输入路径生成：_binary_build64_hello_vap_start/_end（从仓库根执行才稳定）。

echo "==> 多进程演示程序（批次 C：fork/execve/wait4/kill；只嵌进系统内核）"
# user/proc64.asm 是 ring3 程序，用 syscall 指令；链接脚本复用 user/hello_elf64.ld
# （把映像钉在用户窗口 4GiB 起、低于 USER64_STACK_VA64 —— elf64.cpp 会拒绝越界的 PT_LOAD）。
# proc64.cpp 幂等把它装成 VimtuFS2 的 /proc64.elf，再由 proc64_demo64() 以 init 进程跑起来。
# 符号名由 objcopy 按输入路径生成：_binary_build64_proc64_elf_start/_end。
$NASM -f elf64 user/proc64.asm -o "$BUILD/proc64.o"
$LD -m elf_x86_64 -T user/hello_elf64.ld -o "$BUILD/proc64.elf" "$BUILD/proc64.o"
$OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 "$BUILD/proc64.elf" "$BUILD/proc64_elf.o"
cp "$BUILD/proc64_elf.o" "$BUILD/os/"
$NASM -f bin user/hello64.asm -o "$BUILD/hello64.bin"
"$PY" tools/make_vap.py "$BUILD/hello64.bin" "$BUILD/hello.vap" hello
$OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 "$BUILD/hello.vap" "$BUILD/hello_vap64.o"
cp "$BUILD/hello_vap64.o" "$BUILD/os/hello_vap64.o"

echo "==> 真正的 ELF64 可执行程序（nasm -f elf64 -> ld.lld -T -> objcopy 嵌入系统内核）"
# user/hello_elf64.asm 是 ring3 程序，用 **syscall 指令**（Linux ABI）与内核通信；
# user/hello_elf64.ld 把映像钉在用户窗口（4GiB 起、低于 USER64_STACK_VA64）——
# elf64.cpp 会拒绝越出用户窗口的 PT_LOAD，所以链接地址不能随便放。
# 只嵌进**系统内核**：安装介质不带 ELF64 加载器（SRCS_OS 才有 kernel/elf64.cpp）。
# 符号名由 objcopy 按输入路径生成：_binary_build64_hello_elf_start/_end（从仓库根执行才稳定）。
$NASM -f elf64 user/hello_elf64.asm -o "$BUILD/hello_elf64.o"
$LD -m elf_x86_64 -T user/hello_elf64.ld -o "$BUILD/hello.elf" "$BUILD/hello_elf64.o"
$OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 "$BUILD/hello.elf" "$BUILD/hello_elf64_elf.o"
cp "$BUILD/hello_elf64_elf.o" "$BUILD/os/"

echo "==> spin64：长命用户程序（终端 proc run 用；任务管理器进程页 / proc64 kill 的验证目标）"
# user/spin64.asm 打印 pid 后每 1 秒 nanosleep，永不退出；只嵌进**系统内核**（终端在系统内核里）。
# 符号名：_binary_build64_spin64_elf_start/_end（terminal64.cpp 的 `proc run spin` 用它装到 /spin.elf）。
$NASM -f elf64 user/spin64.asm -o "$BUILD/spin64.o"
$LD -m elf_x86_64 -T user/hello_elf64.ld -o "$BUILD/spin64.elf" "$BUILD/spin64.o"
$OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 "$BUILD/spin64.elf" "$BUILD/spin64_elf.o"
cp "$BUILD/spin64_elf.o" "$BUILD/os/"

echo "==> filedemo64：ring3 读文件演示（open /t.txt -> read -> write；只嵌**系统内核**）"
# user/filedemo64.asm 用 syscall 指令走 Linux ABI：open(2)/read(0)/write(1)/close(3)/exit(60)。
# 符号名：_binary_build64_filedemo64_elf_start/_end（terminal64.cpp 开终端时幂等装到 /filedemo.elf，
# 终端 `run filedemo` 即可跑；批次 B 的 syscall open/read/close 接真 FD 层的证据）。
$NASM -f elf64 user/filedemo64.asm -o "$BUILD/filedemo64.o"
$LD -m elf_x86_64 -T user/hello_elf64.ld -o "$BUILD/filedemo64.elf" "$BUILD/filedemo64.o"
$OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 "$BUILD/filedemo64.elf" "$BUILD/filedemo64_elf.o"
cp "$BUILD/filedemo64_elf.o" "$BUILD/os/"

echo "==> AP 跳板（SMP：nasm 平铺二进制 -> objcopy 嵌入**系统内核**）"
# kernel/ap_trampoline64.asm 由 BSP 原字节拷到物理 0x8000，再由 SIPI（向量 0x08）拉起 AP：
#   nasm -f bin 直接出平铺二进制（按 [org 0x8000] 汇编，**不是**位置无关，见文件顶部说明）
#   -> objcopy 变 elf64 目标 -> 只链进系统内核（安装介质不跑 SMP，不链 smp64.o）。
#   符号名由 objcopy 按输入路径生成：_binary_build64_ap_trampoline64_bin_start/_end。
$NASM -f bin kernel/ap_trampoline64.asm -o "$BUILD/ap_trampoline64.bin"
$OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 "$BUILD/ap_trampoline64.bin" "$BUILD/ap_trampoline64.o"
cp "$BUILD/ap_trampoline64.o" "$BUILD/os/ap_trampoline64.o"
echo "    AP 跳板 = $(stat -c%s "$BUILD/ap_trampoline64.bin") 字节（按 0x8000 汇编）"

echo "==> 链接两个内核"
$LD -m elf_x86_64 -o "$BUILD/kernel64.elf"    kernel/linker64.ld "$BUILD"/kernel64.o "$BUILD"/x86_64.o \
    "$BUILD"/fb.o "$BUILD"/font.o "$BUILD"/input.o "$BUILD"/mem64.o "$BUILD"/ata64.o "$BUILD"/part64.o "$BUILD"/setup64.o \
    "$BUILD"/hwinfo64.o "$BUILD"/acpi64.o "$BUILD"/edid64.o "$BUILD"/vfs64.o "$BUILD"/fd64.o "$BUILD"/usermode64.o "$BUILD"/syscall64.o \
    "$BUILD"/entry64.o "$BUILD"/isr_stubs64.o "$BUILD"/switch64.o "$BUILD"/syscall_entry64.o \
    "$BUILD"/user_demo64.o "$BUILD"/font_*.o "$BUILD"/logo_rgba.o "$BUILD"/icon_*.o
$OBJCOPY -O binary "$BUILD/kernel64.elf" "$BUILD/kernel64.bin"

$LD -m elf_x86_64 -o "$BUILD/kernel64_os.elf" kernel/linker64.ld "$BUILD/os"/kernel64.o "$BUILD/os"/x86_64.o \
    "$BUILD/os"/fb.o "$BUILD/os"/font.o "$BUILD/os"/input.o "$BUILD/os"/mem64.o \
    "$BUILD/os"/hwinfo64.o "$BUILD/os"/acpi64.o "$BUILD/os"/edid64.o "$BUILD/os"/vfs64.o "$BUILD/os"/store64.o "$BUILD/os"/ata64.o "$BUILD/os"/apic64.o "$BUILD/os"/display64.o "$BUILD/os"/fd64.o "$BUILD/os"/usermode64.o "$BUILD/os"/syscall64.o \
    "$BUILD/os"/gui64.o "$BUILD/os"/calc64.o "$BUILD/os"/mines64.o \
    "$BUILD/os"/terminal64.o "$BUILD/os"/settings64.o "$BUILD/os"/taskmgr64.o \
    "$BUILD/os"/sysstate64.o "$BUILD/os"/config64.o "$BUILD/os"/session64.o "$BUILD/os"/panic64.o \
    "$BUILD/os"/preload64.o "$BUILD/os"/update64.o \
    "$BUILD"/entry64.o "$BUILD"/isr_stubs64.o "$BUILD"/switch64.o "$BUILD"/syscall_entry64.o "$BUILD/os"/task64.o \
    "$BUILD/os"/app64.o "$BUILD/os"/elf64.o "$BUILD/os"/proc64.o \
    "$BUILD/os"/hello_elf64_elf.o "$BUILD/os"/proc64_elf.o "$BUILD/os"/spin64_elf.o "$BUILD/os"/filedemo64_elf.o \
    "$BUILD/os"/e1000_64.o "$BUILD/os"/net64.o "$BUILD/os"/usb64.o \
    "$BUILD/os"/smp64.o "$BUILD/os"/ap_trampoline64.o \
    "$BUILD/os"/hello_vap64.o \
    "$BUILD/os"/user_demo64.o "$BUILD/os"/font_*.o "$BUILD/os"/logo_rgba.o "$BUILD/os"/icon_*.o
$OBJCOPY -O binary "$BUILD/kernel64_os.elf" "$BUILD/kernel64_os.bin"

KBSZ=$(stat -c%s "$BUILD/kernel64.bin")
OSSZ=$(stat -c%s "$BUILD/kernel64_os.bin")
LDSZ=$(stat -c%s "$BUILD/loader64.bin")
echo "Build OK. 安装程序内核 = ${KBSZ} bytes, 系统内核 = ${OSSZ} bytes, loader = ${LDSZ} bytes"

if [ "$LDSZ" -gt 4096 ]; then echo "ERROR: loader64.bin > 4096" >&2; exit 1; fi
if [ "$KBSZ" -gt $((KERNEL_SECTORS * 512)) ]; then echo "ERROR: 安装程序内核超出内核区" >&2; exit 1; fi
if [ "$OSSZ" -gt $((KERNEL_SECTORS * 512)) ]; then echo "ERROR: 系统内核超出内核区" >&2; exit 1; fi

echo "==> 组装系统镜像载荷 system.img（$SYS_SECTORS 扇区）"
dd if=/dev/zero of="$BUILD/system.img" bs=512 count="$SYS_SECTORS" status=none
dd if="$BUILD/boot.bin"        of="$BUILD/system.img" conv=notrunc status=none
dd if="$BUILD/loader64.bin"    of="$BUILD/system.img" seek=1 conv=notrunc status=none
dd if="$BUILD/kernel64_os.bin" of="$BUILD/system.img" seek="$KERNEL_LBA" conv=notrunc status=none

echo "==> 生成载荷头（magic VIMTUPAY + 扇区数 + 载荷 LBA）"
"$PY" - "$BUILD/payload_hdr.bin" "$SYS_SECTORS" "$((PAYLOAD_LBA + 1))" <<'PYEOF'
import struct, sys
out, sectors, lba = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
buf = bytearray(512)
buf[0:8] = b"VIMTUPAY"
struct.pack_into("<III", buf, 8, sectors, lba, 0)
open(out, "wb").write(bytes(buf))
print("    payload_hdr: sectors=%d lba=%d -> %s" % (sectors, lba, out))
PYEOF

echo "==> 组装安装介质 $IMG（$IMAGE_SECTORS 扇区 = $((IMAGE_SECTORS / 2048)) MB）"
dd if=/dev/zero of="$IMG" bs=512 count="$IMAGE_SECTORS" status=none
dd if="$BUILD/boot.bin"        of="$IMG" conv=notrunc status=none
dd if="$BUILD/loader64.bin"    of="$IMG" seek=1 conv=notrunc status=none
dd if="$BUILD/kernel64.bin"    of="$IMG" seek="$KERNEL_LBA" conv=notrunc status=none
dd if="$BUILD/payload_hdr.bin" of="$IMG" seek="$PAYLOAD_LBA" conv=notrunc status=none
dd if="$BUILD/system.img"      of="$IMG" seek="$((PAYLOAD_LBA + 1))" conv=notrunc status=none

echo "Image  OK. $IMG = $(stat -c%s "$IMG") bytes"
echo "          载荷：LBA $PAYLOAD_LBA(头) + $((PAYLOAD_LBA + 1))..$((PAYLOAD_LBA + SYS_SECTORS))（$SYS_SECTORS 扇区）"

echo "==> 生成 UEFI 的 ESP 镜像（FAT16，含 BOOTX64.EFI + 内核 + 载荷）"
"$PY" tools/make_esp.py "$BUILD/esp.img" "$BUILD/BOOTX64.EFI" "$BUILD/kernel64.bin" "$BUILD/system.img" "$BUILD/UEFI64.BIN"

echo "==> 生成 64 位安装 ISO（vimtu64-64.iso：BIOS 光盘 + U 盘 hybrid + UEFI 三种引导）"
"$PY" tools/make_iso64.py
echo
echo "启动安装介质: qemu-system-x86_64 -drive format=raw,file=$IMG -drive format=raw,file=target.img -boot order=c -m 512 -vga std -serial stdio"
echo "端到端安装测试: python tests/install_flow_test.py"
