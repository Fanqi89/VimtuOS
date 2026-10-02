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
#   LBA 7497..8008   : ★ 本批：**图标包**（build/iconpack.bin，上限 512 扇区 = 256KB）——
#                      外置图标资源（真图标）就放这里：内核区尾部、内核二进制之后、数据分区之前。
#                      为什么不用文件系统：system.img 里没有 VimtuFS2 卷（卷在安装器建的数据分区上，
#                      起点 LBA 8009 与 store 的裸盘降级槽重叠，见 kernel/memlayout64.h 的 ★★ 段），
#                      而这段字节装到硬盘后原样保留（loader 就从 LBA 9 读内核）。
#   ★ 安装程序把这个映像写到目标盘后，还会在目标盘**盘尾**再建一个 48MB 的 FAT32 ESP
#     （EFI/BOOT/BOOTX64.EFI + UEFI64.BIN + KERNEL64.BIN）与盘尾备份 GPT ——
#     这样"装好的盘"在 UEFI 与 BIOS 下都能启动（见 kernel/fat64.cpp、kernel/part64.cpp）。
#     KERNEL64.BIN 的字节**直接读目标盘 LBA 9..8008**（不内嵌进安装程序内核，省它的 4MB 预算）。
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
# ★ 版本号**唯一真源**（发布时只改这一行）：编译期宏 VIMTUOS_VERSION_STR 进两份内核，
#   设置页"关于"（kernel/settings64.cpp）与串口打点都从这里取值（未定义时内核打 unknown）。
# ★ 本批（演示程序搬进系统卷）：内嵌的演示程序 blob（约 145 KB）全部搬出内核二进制 ——
#   交付 = 系统卷里的文件（tools/demo_pack_win.py 构建期写入 + 逐字节回读自检）；
#   空夹具盘由构建期"原始区"（build64/demo64_raw.bin，写进 system.img 的 LBA 7497 起）兜底。
#   内核二进制里只剩路径/偏移/长度表（kernel/demo64.h），本文件末尾有 64B 探针断言。
VIMTUOS_VERSION="0.4.2-beta19"
CXXFLAGS="$CXXFLAGS -DVIMTUOS_VERSION_STR=\"$VIMTUOS_VERSION\""
CXXFLAGS="$CXXFLAGS ${VIMTU_EXTRA_CXXFLAGS:-}"
#
# ★ A2：跑哪个 fbdemo —— 默认 **C 版**（user/apps/fbdemo.c，用我们自己的最小 libc 写），
#   VIMTU_USER_FBDEMO=asm 时切回 A1 的汇编版（user/fbdemo.asm）。两个版本的可观测行为
#   （串口打点 + 画的东西 + 越界/夹取判据）逐条一致，见 user/apps/fbdemo.c 顶部的对照表；
#   这里只是一个编译期宏，内核侧的选择写在 kernel/kernel64.cpp 的 VIMTU_USER_FBDEMO_ASM 分支里。
if [ "${VIMTU_USER_FBDEMO:-c}" = "asm" ]; then
    CXXFLAGS="$CXXFLAGS -DVIMTU_USER_FBDEMO_ASM=1"
else
    CXXFLAGS="$CXXFLAGS -DVIMTU_USER_FBDEMO_ASM=0"
fi
CXXFLAGS_INSTALLER="$CXXFLAGS -DVIMTU_INSTALLER_MEDIA=1 -DVIMTU_PAYLOAD_LBA=$PAYLOAD_LBA -DVIMTU_KBD_TRACE=1"

# 两套源文件清单：
#   核心 = 平台层 + 图形 + 输入 + 用户态/系统调用地基（安装介质与装好的系统都要；
#          usermode64/syscall64 进两份内核：x86_64.cpp 的 int 0x80 分发必须能链接，
#          "进 ring3 跑用户程序"只在系统内核里调用，见 kernel64.cpp 的 os_boot_path）
#   安装 = 核心 + ATA 驱动 + 分区/安装引擎 + 安装界面
#   ★ item 5a / item 6：ahci64.cpp（AHCI/SATA 驱动）+ nvme64.cpp（NVMe 驱动）+ hwui64.cpp
#     （屏幕硬件检查报告）也进 CORE —— 两份内核都要：安装程序要能看见 SATA/NVMe 盘，
#     系统内核的 VFS/store 也要能（驱动器号 8.. / 16.. 分派），
#     而硬件检查报告在安装介质与装好的系统里都是"没有串口时唯一的诊断画面"。
SRCS_CORE="kernel/kernel64.cpp kernel/console64.cpp kernel/x86_64.cpp kernel/fb.cpp kernel/font.cpp kernel/input.cpp kernel/input64.cpp kernel/mem64.cpp kernel/hwinfo64.cpp kernel/acpi64.cpp kernel/edid64.cpp kernel/display64.cpp kernel/fd64.cpp kernel/usermode64.cpp kernel/syscall64.cpp kernel/ahci64.cpp kernel/nvme64.cpp kernel/hwui64.cpp kernel/drive64.cpp kernel/fat64.cpp kernel/fs64.cpp kernel/sig64.cpp kernel/virtio_gpu64.cpp"
# ★ 本批（用户态设备映射）：kernel/pci64.cpp = PCI 配置空间 + BAR 解码（pci_map_bar(48) 的底层）。
#   进 **SRCS_CORE** 的理由：kernel/syscall64.cpp（也在 CORE 里）对它的是**强引用**，
#   两份内核都必须在链接行里能解析到它；它只有"配置空间读写 + BAR 解码"这一小块，
#   安装介质内核里不会被调用（那边没有 ring3），代价 ~1.5 KB。
SRCS_CORE="$SRCS_CORE kernel/pci64.cpp"
#
#  ★ 驱动线 3：kernel/virtio_gpu64.cpp（现代 virtio-gpu 2D：resource/scanout/TRANSFER_TO_HOST_2D）
#    进 CORE：**两份内核都要**（kernel/fb.cpp 的"上屏后端选择"引用它 —— 系统内核的桌面与
#    安装介质的向导画面都走同一条 fb 提交路径；没有 virtio-gpu 设备时它只打一行 not found，
#    软件 LFB 路径的行为**完全不变**）。
#  ★ 批次 N：console64.cpp = 开机滚屏引导控制台（启动日志环形缓冲 + 回放 + dmesg）。
#    进 CORE：安装介质与系统**两份内核都要**屏上跑一遍启动日志（安装介质走向导前、系统走桌面前）。
SRCS_INSTALLER="$SRCS_CORE kernel/part64.cpp kernel/setup64.cpp kernel/vfs64.cpp"
# 桌面外壳 + 应用：**只编进系统内核**（安装介质走向导，不带桌面，省 4MB 内核区空间）
#   注意：这几个文件依赖 kernel/gui64.cpp 提供的外壳实现，任何新增应用都要同时加进这里。
SRCS_DESKTOP="kernel/gui64.cpp kernel/explorer64.cpp kernel/calc64.cpp kernel/mines64.cpp \
 kernel/terminal64.cpp kernel/settings64.cpp kernel/taskmgr64.cpp"
# ★ 本批（Windows 11 现代外观）：theme64（设计 Token 唯一真源 + 主题表 + 减少动画开关）/
#   gfx64（圆角 + 双层阴影 + 毛玻璃缓存 + 渐变 + 壁纸 6 种适应模式）/ img64（PNG/BMP 解码：
#   壁纸/头像/开始图标优先从 VimtuFS2 读）。三者都是 gui64 的依赖，**只进系统内核**
#   （安装介质不带桌面，保持它的 4MB 预算不被这些新增代码吃掉）。
SRCS_DESKTOP="$SRCS_DESKTOP kernel/theme64.cpp kernel/gfx64.cpp kernel/img64.cpp"
#  ★ 本批（P1c：锁屏 / 登录 / 多用户骨架）：locklogin64（锁屏 + 登录界面 + 密码框 + 多用户选择）
#   与 userdb64（用户表 / 加盐哈希 / /etc/users.db 持久化 / 会话身份）依赖 theme64/gfx64/img64 +
#   config64/vfs64，**只进系统内核** —— 安装介质不链它们，所以安装向导不会出现锁屏。
SRCS_DESKTOP="$SRCS_DESKTOP kernel/locklogin64.cpp kernel/userdb64.cpp"
#  ★ 本批（P2：开始菜单 + 四个二级弹窗 + 设备插拔通知）：startmenu64（开始菜单/搜索/固定网格/状态区/电源菜单
#   + P2 UI 工具箱：混合圆角/亚克力/线性图标）与 panels64（通知/声音/网络/日历 + 通知列表 + 设备 toast）。
#   **只进系统内核**（安装介质没有桌面外壳，保持它的 4MB 预算不被这批代码吃掉）。
SRCS_DESKTOP="$SRCS_DESKTOP kernel/startmenu64.cpp kernel/panels64.cpp"
# ★ 本批（真图标）：kernel/icons64.cpp = 统一图标层（读系统镜像里的外置图标包 -> img64 解码 -> 缓存
#   -> 按主题 palette 着色上屏；取不到就回落既有程序化绘制）。**只进系统内核**（安装介质不带桌面）。
SRCS_DESKTOP="$SRCS_DESKTOP kernel/icons64.cpp"
#  ★ 本批（P5：桌面交互细节）：desktopops64（桌面右键菜单 / 玻璃选择框 / 回收站 / 桌面图标集合，
#   以及设置页那一处最小入口的两个后端）。**只进系统内核**（安装介质没有桌面外壳）。
SRCS_DESKTOP="$SRCS_DESKTOP kernel/desktopops64.cpp"
#   sysstate64 = 运行状态机 + 模块注册表 + 健康 + ring log（terminal 的 state/health/syslog）
#   config64   = 类型化配置 KV，落在 store64（VimtuFS2 的 /store.a|b，真落盘）
#   session64  = 会话/应用内容策略（关窗清状态、退出保存、启动恢复）
#   preload64  = 字形预光栅化 + 图标预缩放（batch A 后半；gui64 的图标缓存 + font 的 prewarm 原语）
#   update64   = 标记文件 -> 应用动作 -> store/ring log/自动重启（不是真"升级包"，见 update64.h）
SRCS_SYS="kernel/sysstate64.cpp kernel/config64.cpp kernel/session64.cpp kernel/panic64.cpp kernel/preload64.cpp kernel/update64.cpp"
# ★ 批次 C：kernel/proc64.cpp（进程/地址空间）只进系统内核 —— 它依赖 task64/elf64/vfs64。
SRCS_OS="$SRCS_CORE $SRCS_DESKTOP $SRCS_SYS kernel/task64.cpp kernel/vfs64.cpp kernel/store64.cpp kernel/ata64.cpp kernel/app64.cpp kernel/elf64.cpp kernel/proc64.cpp kernel/e1000_64.cpp kernel/net64.cpp kernel/apic64.cpp kernel/smp64.cpp kernel/usb64.cpp kernel/xhci64.cpp kernel/hda64.cpp kernel/wl64.cpp"
# ★ A5：kernel/wl64.cpp（Wayland 基础骨架：surface/commit/seat + 最小合成器）**只进系统内核** ——
#   它要用 proc64 的 shm 对象表（读共享缓冲的物理页）与 fb 的提交路径；安装介质内核不链它，
#   syscall64.cpp 对 15..21 号用弱引用（那里返回 -1 并打 [SYSCALL] deny，不假装成功）。
# ★ A4-5：kernel/sig64.cpp（信号投递）在 **SRCS_CORE** 里加（见上面那行）—— 两份内核都要链它：
#   syscall64.cpp 的 13/14/15 号是**强引用**它（安装介质内核没有进程表，sig64 内部对 proc64 的访问
#   全部是弱引用 + 判空 -> 那时它只做参数校验，如实不投递，绝不假装成功）。
# elf64.cpp = ELF64 加载器：**只进系统内核**（安装介质不需要它；它内嵌的 hello.elf 是系统程序）
# apic64.cpp = LAPIC + IOAPIC 接管中断路由：**只进系统内核**（安装链保持纯 8259 PIC，
#   避免影响安装介质内核的字节级断言；x86_64.cpp 对它的 EOI/掩码分派用 weak 引用，不链也不报错）

# ==================== ★ A2：可选目标 `--user <name>` ====================
# 只编一个用户态 C 程序（user/apps/<name>.c）然后退出 —— 不进整个内核构建（省几分钟）。
# 等价入口：bash user/build_user.sh <name>（同一个脚本；这里只是从 build64.sh 也能直接调）。
# 产出（固定在 build64/）：build64/user_<name>.elf（静态 ELF64）+ build64/user_<name>.bin（平铺 blob）。
if [ "${1:-}" = "--user" ]; then
    shift
    if [ -z "${1:-}" ]; then
        echo "用法：bash build64.sh --user <name>   （name = user/apps/<name>.c 的文件名主体）" >&2
        exit 2
    fi
    mkdir -p "$BUILD"
    bash user/build_user.sh "$1" "$BUILD"
    exit $?
fi

echo "==> 清理 $BUILD"
# ★ 占用容忍（本批实测踩到）：别的验收脚本可能在 build64/ 里留着一个正在被 QEMU/测试进程
#   占用的镜像（例如 gzip_test.img）。`set -e` 下 rm 的非零返回会**直接中断整个构建** ——
#   而构建产物全是按路径覆盖写的，残留一个测试镜像不影响内核/镜像产出。这里重试 + 如实警告，
#   绝不让"一个被占用的测试镜像"变成"整个构建失败"。
for _i in 1 2 3 4 5; do
    rm -rf "$BUILD" 2>/dev/null && break
    echo "    [warn] $BUILD 里有文件被占用（第 $_i 次），3 秒后重试"
    sleep 3
done
rm -rf "$BUILD" 2>/dev/null || echo "    [warn] $BUILD 未能完全清空（有文件被占用）；继续构建（产物按路径覆盖写）"
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

echo "==> 内嵌 UEFI 引导字节（安装完成时写进目标盘 ESP；只进**安装程序**内核）"
# 目的：安装程序在目标盘上建 ESP 时，要把 BOOTX64.EFI / UEFI64.BIN 原样写进 FAT32 卷。
# 符号名由 objcopy 按输入路径生成（从仓库根执行才稳定）：_binary_build64_BOOTX64_EFI_start 等。
# ★ 系统内核不需要这两个字节（装好的盘上 ESP 不再变动），别把它链进系统内核省体积。
$OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 "$BUILD/BOOTX64.EFI" "$BUILD/bootx64_efi.o"
$OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 "$BUILD/UEFI64.BIN" "$BUILD/uefi64_bin.o"
echo "    内嵌：BOOTX64.EFI=$(stat -c%s "$BUILD/BOOTX64.EFI") B  UEFI64.BIN=$(stat -c%s "$BUILD/UEFI64.BIN") B"
echo "==> 编译内核汇编"
$NASM -f elf64 kernel/entry64.asm    -o "$BUILD/entry64.o"
$NASM -f elf64 kernel/isr_stubs64.asm -o "$BUILD/isr_stubs64.o"
$NASM -f elf64 kernel/switch64.asm   -o "$BUILD/switch64.o"
# syscall 指令入口（LSTAR 指向它）：两份内核都要链（syscall64.cpp 在 SRCS_CORE 里引用它）
$NASM -f elf64 kernel/syscall_entry64.asm -o "$BUILD/syscall_entry64.o"

echo "==> 准备资源（字体 / logo / 图标；生成脚本产物落在 build/，两份内核都要嵌）"
"$PY" _otf2ttf.py
# 顺序有讲究：face 1（中文）先建，_subset_fonts.py 才能按"前三个面的**产物**实际覆盖"算出
# 兜底面（face 3）的码点集合 —— 见 _subset_fonts.py 的 fallback_chars()。
"$PY" _subsetsimhei.py
"$PY" _subset_fonts.py
"$PY" _make_logo.py
"$PY" _make_icons.py
"$PY" _make_start_icon.py
cp build/font_bahnschrift.ttf build/font_simhei.ttf build/font_mono.ttf build/font_fallback.ttf "$BUILD/"
"$PY" tools/make_iconpack.py
# ★ 本批（开始按钮的**内置兜底**）：icon_start.bin（64x64 RGBA = 16,384 B）也搬走了，但"裸
#   system.img（没有 VimtuFS2 卷）"这条路径必须仍有一个**内置**兜底（tests/gui_modern64_test.py
#   的裸盘断言：`[DOCK64] start icon src=builtin:icon_start.bin … ok=1` + 该区域彩色像素 > 80）。
#   做法：构建期从 build/icon_start.bin 取 24x24 最近邻 mip（2,304 B）内嵌 —— Dock 显示 46px、
#   老接口显示 24px（START_ICON_DISP），24x24 的 mip 在两条路径上都够用，且只占 2.3 KB。
"$PY" - "build/icon_start.bin" "build/icon_start_mini.bin" <<'PYMINI'
import sys
src = open(sys.argv[1], "rb").read()
SW = SH = 64
assert len(src) == SW * SH * 4, "icon_start.bin 不是 64x64 RGBA（实际 %d B）" % len(src)
N = 24
# 最近邻采样写成 sx = x*SW//N（**覆盖整张源图**）—— 不能写 x*(SW//N)：64/24 = 2，那样只取到左上
# 48x48 的一块（等于裁图），mip 会偏心。颜色保持原样（不混色），与 gui64 的 scale_rgba64 同一条公式。
sx_of = [x * SW // N for x in range(N)]
sy_of = [y * SH // N for y in range(N)]
dst = bytearray(N * N * 4)
for y in range(N):
    for x in range(N):
        o = ((sy_of[y] * SW) + sx_of[x]) * 4
        dst[(y * N + x) * 4:(y * N + x) * 4 + 4] = src[o:o + 4]
assert sx_of[0] == 0 and sy_of[0] == 0 and sx_of[N - 1] >= SW - 4 and sy_of[N - 1] >= SH - 4
assert len(dst) == N * N * 4
# 自检：mip 的每个像素都逐字节来自源图（同一公式），尺寸钉死
for y in range(N):
    for x in range(N):
        o = ((sy_of[y] * SW) + sx_of[x]) * 4
        assert dst[(y * N + x) * 4:(y * N + x) * 4 + 4] == src[o:o + 4]
open(sys.argv[2], "wb").write(bytes(dst))
print("    开始按钮内置兜底 mip：64x64(%d B) -> %dx%d(%d B)" % (len(src), N, N, len(dst)))
PYMINI
cp build/icon_start_mini.bin "$BUILD/icon_start_mini.bin"
# ★ 本批（资源外置）：这 5 份 raw 资源**不再内嵌进内核** —— 它们的去处是"构建期写进系统卷"
#   （见本文件后面的 tools/assets_pack_win.py 那一步），内核侧从 /etc/logo.bin 等 5 个文件里读。
#   所以这里**不**再把它们 cp 进 $BUILD/ 做 objcopy（留在 build/ 给打包脚本用）。
# ★ 本批：把**真文件 logo/kaisi.png 的原始字节**也嵌进系统内核（objcopy，符号 _binary_kaisi_png_*）——
#   启动期由 gui64 幂等装进 VimtuFS2 系统卷（/logo/kaisi.png + /kaisi.png），开始按钮从盘上读真图。
#   只进系统内核：安装介质不跑桌面，不需要它（省它的 4MB 预算）。
cp logo/kaisi.png "$BUILD/kaisi.png"
echo "    嵌入 logo/kaisi.png = $(stat -c%s logo/kaisi.png) 字节"

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
# ★ 内核预算（A4-4）：四份**字体**改成"构建期 zlib -9 压缩 + 启动期内核解到 .bss"。
#   为什么不是"放卷里、启动期从卷读"：font_init() 在 kmain 的图形栈初始化里就要用，而系统卷要到
#   os_boot_path64() 才挂载；且验收夹具里有大量"无卷/空卷"盘（desktop64_test 直接引导 system.img；
#   musl64_test/dynlink64_test/ipc64_test/sh64_test 的卷里只有 shell）——卷里没有就没字会让这些
#   路径全变成豆腐块。压缩内嵌是行为等价的搬法（解出来就是同一份 TTF 字节，见 kernel/font.cpp）。
#   格式：font_<name>.z = 8B 未压缩长度（小端 u64）+ raw deflate（wbits=-15，无 zlib 头）。
#   实测：1,164,272 B -> 755,944 B（省 408,328 B；内核 4,037,328 -> 3,629,000 B）。
#   原始 .ttf 仍留在 build64/（fonts64_test 看 build/ 的产物；这里是同批拷贝），只作留档/许可核对。
"$PY" - "$BUILD" <<'PYFONTZ'
import os, struct, sys, zlib
b = sys.argv[1]
tot_raw = tot_pack = 0
for n in ("bahnschrift", "simhei", "mono", "fallback"):
    raw = open(os.path.join(b, "font_%s.ttf" % n), "rb").read()
    co = zlib.compressobj(9, zlib.DEFLATED, -15)          # raw deflate（无 zlib 头/adler32）
    z = co.compress(raw) + co.flush()
    packed = struct.pack("<Q", len(raw)) + z
    with open(os.path.join(b, "font_%s.z" % n), "wb") as f:
        f.write(packed)
    # 构建期自检：宿主 zlib 必须能把自己压出来的解回来（逐字节）
    back = zlib.decompressobj(-15).decompress(z)
    assert back == raw, "字体压缩往返自检失败：font_%s" % n
    tot_raw += len(raw); tot_pack += len(packed)
    print("    字体 %-12s %8d B -> %8d B（deflate -9 + 8B 长度头）" % (n, len(raw), len(packed)))
print("    字体合计：%d B -> %d B（省 %d B；内核里只留压缩形态，解压缓冲在 .bss）"
      % (tot_raw, tot_pack, tot_raw - tot_pack))
PYFONTZ
(cd "$BUILD" && $OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 font_bahnschrift.z font_bahnschrift_z.o)
(cd "$BUILD" && $OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 font_simhei.z font_simhei_z.o)
(cd "$BUILD" && $OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 font_mono.z font_mono_z.o)
(cd "$BUILD" && $OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 font_fallback.z font_fallback_z.o)
# ★ 本批（资源外置）：logo_rgba.bin / icon_mycomputer.bin / icon_recyclebin.bin / icon_terminal.bin /
#   icon_start.bin（16,384 B）**都不再内嵌**（objcopy 那几行已删）—— 它们由 tools/assets_pack_win.py
#   写进系统卷的 /etc/logo.bin、/etc/icon_*.bin，运行期从卷里读。这里只留**开始按钮的内置兜底 mip**
#   （24x24 = 2,304 B；裸 system.img 无卷时用，符号 _binary_icon_start_mini_bin_start/_end）。
(cd "$BUILD" && $OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 icon_start_mini.bin icon_start_mini.o)
(cd "$BUILD" && $OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 kaisi.png kaisi_png.o)
# OS 内核用同一批资源对象（直接复用）
cp "$BUILD"/font_*_z.o "$BUILD"/icon_start_mini.o "$BUILD/os/"
cp "$BUILD"/kaisi_png.o "$BUILD/os/"      # 只给系统内核（桌面用）
# ★ A4-2a：**图标包搬进 VimtuFS2 系统卷** —— 包字节不再放在"内核区尾部（LBA 7497..8008）"，
#   改成把 build/iconpack.bin 内嵌进**系统内核**（objcopy -> .rodata），启动期由 icons64 幂等
#   装进系统卷的 /icons/pack.bin，之后一切加载都从**卷**里读（[ICON64] load … src=vfs）。
#   为什么内嵌：安装器建出来的盘、测试夹具盘上"卷里的包"必须有个来源，而安装器侧不在本批可改范围；
#   内嵌的代价只有 49 KB，换来的却是内核区从 7,488 扇区放宽到 **8,000 扇区**（+262,144 B）。
cp "$RES/iconpack.bin" "$BUILD/iconpack.bin"
(cd "$BUILD" && $OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 iconpack.bin iconpack_bin.o)
cp "$BUILD/iconpack_bin.o" "$BUILD/os/"
echo "    内嵌图标包（系统内核）：$RES/iconpack.bin = $(stat -c%s "$RES/iconpack.bin") B -> 启动期装进系统卷 /icons/pack.bin"

echo "==> Rust 模块（gui_rs crate：设计 Token 表 + 主题配色计算；项目路线 C + C++ + Rust）"
# 为什么单独一段：Rust 用 `rustc --target x86_64-unknown-none` 编成**可直接被 ld.lld 链接**
#   的目标文件（gui_rs/gui_rs.o）。
#   ★ 工具链前置（本机已装）：rustup + stable-x86_64-pc-windows-gnu (rustc 1.98.1)，
#     且 `rustup target add x86_64-unknown-none`（装 no_std 的 core/rust-src）。
#   ★ 只进**系统内核**（安装介质不链 Rust 模块）；具体参数与"为什么用 rustup shim 的
#     绝对路径（MSYS2 自带 rustc 会 E0463: can't find crate for `core`）"见 gui_rs/build_rs.sh。
#   ★ 找不到 rustc / 没有那个目标 / 编译失败：build_rs.sh 会**明确报错并非 0 退出**，
#     set -e 让整个构建立刻失败（绝不静默跳过 —— 那样 Rust 模块会悄悄从内核里消失）。
bash gui_rs/build_rs.sh

echo "==> 用户态演示程序（真实 ring3 代码：nasm 平铺二进制 -> objcopy 嵌入内核）"
# user/demo64.asm 是**用户态**程序（ring3，用 int 0x80 与内核通信），不是内核代码：
#   ★ 本批：nasm 只出平铺二进制 build64/user_demo64.bin —— **不再 objcopy 进内核**；
#   交付 = 系统卷 /bin/demo64.bin（tools/demo_pack_win.py 构建期写入 + 逐字节回读自检），
#   空夹具盘由"原始区"兜底（见 kernel/demo64.h）。内核里一个字节都没有它的本体。
$NASM -f bin user/demo64.asm -o "$BUILD/user_demo64.bin"
echo "==> A1 用户态绘图演示（ring3 自己画屏：nasm -f bin -> 交付 = 系统卷 /bin/fbdemo64.bin）"
# user/fbdemo.asm 是 ring3 程序，走 int 0x80 自有 ABI 的 fb_map(9) / fb_flip(10) / fb_present(11)：
#   平铺二进制**不内嵌内核**：交付 = 系统卷 /bin/fbdemo64.bin（调用点见 kernel/kernel64.cpp
#   的 k64_run_raw_demo64；同时 /bin/fbdemo_c.bin 是默认的 C 版）。
$NASM -f bin user/fbdemo.asm -o "$BUILD/user_fbdemo64.bin"

echo "==> ★ A2：用户态 **C** 程序交叉编译（我们自己的最小 libc + 自有 ABI 包装；clang + lld）"
# 见 user/build_user.sh（编译/链接的全部细节与每条选项的理由都在那里）与
# docs/应用层与系统调用说明.md 的"用户态 C 运行时（A2）"节。这里做两件事：
#   1) 把 user/apps/*.c 编成 build64/user_<name>.elf（静态 ELF64）+ build64/user_<name>.bin（平铺 blob）；
#   2) ★ 本批：**不再 objcopy 进内核** —— 交付 = 系统卷 /bin/<name>_c.bin（demo_pack_win.py 写入），
#      空夹具盘由"原始区"兜底（见 kernel/demo64.h）。内核里一个字节都没有这些程序的本体。
# ★ 体积纪律：单条上限 8 页 = 32768B（见 user/build_user.sh 的 MAX_BLOB）。
# ★ 默认集：hello（hello world）、libctest（printf 子集 + malloc 压力）、fbdemo（C 版 A1 演示）；
#   VIMTU_USER_FBDEMO=asm 时 fbdemo 换回 A1 汇编版（user/fbdemo.asm）。
if [ "${VIMTU_USER_FBDEMO:-c}" = "asm" ]; then
    VIMTU_USER_APPS="${VIMTU_USER_APPS:-hello libctest}"
else
    VIMTU_USER_APPS="${VIMTU_USER_APPS:-hello libctest fbdemo}"
fi
for app in $VIMTU_USER_APPS; do
    bash user/build_user.sh "$app" "$BUILD"
    echo "    演示程序（卷交付）：user/apps/$app.c -> $BUILD/user_${app}.bin（$(stat -c%s "$BUILD/user_${app}.bin") B）"
done

echo "==> ★ A4-1：ring3 shell（**交付 = 系统卷里的 /bin/shell.bin**，不内嵌内核）"
# 交付方式（本批次的核心要求：内核只负责"把它装进用户态并跑起来"）：
#   1) user/shell/build_shell.sh 用 user/lib（自研最小 libc + 自有 ABI 包装）编出静态 ELF64
#      -> $BUILD/shell.elf / shell.bin；
#   2) tools/make_shellvol.py 把 shell.bin 写进一块 VimtuFS2 **v4** 卷（/bin/shell.bin +
#      /etc/sh64hello.txt + /tmp(0777)）-> $BUILD/shellvol.img（写完会逐字节回读自检）；
#   3) 系统镜像装完之后，同一条命令再把它放进演示/验收盘的主分区（LBA 8009）
#      -> $BUILD/sysdisk.img；内核二进制里**不含** shell 的字节（后面有一条断言）。
bash user/shell/build_shell.sh "$BUILD"
$PY tools/make_shellvol.py --shell "$BUILD/shell.bin" --vol "$BUILD/shellvol.img"

echo "==> ★ A3：musl 静态程序（third_party/musl 的 libc.a；编译/链接全在 tools/musl_build_win.sh 里）"
# 见 tools/musl_build_win.sh 与 docs/应用层与系统调用说明.md 的"musl（A3 第一步）"节：
#   * 该脚本用 **musl 自己的头文件 + musl 的 lib/libc.a + 自写 _start + tools/musl_hello64.ld**
#     链出 build64/musl_hello.elf（静态 ELF64，钉在用户窗口 4GiB 起、无 PT_INTERP/无重定位）；
#   * ★ 本批：**不再内嵌进内核** —— 交付 = 系统卷 /musl_hello.elf（tools/demo_pack_win.py
#     构建期写入 + 逐字节回读自检），空夹具盘由"原始区"兜底（见 kernel/demo64.h）。
#     启动期由 kernel64.cpp 的 musl64_install64() 保证它在卷里，再从盘上读出来、作为
#     **真进程**进 ring3 —— 见 musl64_demo64。
# ★ 体积记账（本批）：这份 blob 已**不在** kernel64_os.bin 里（构建输出里有精确值）。
bash tools/musl_build_win.sh "$BUILD/musl_hello.elf"
# ★ A3 下半（体积）：写卷前去掉符号表 —— 卷里那份只按程序头表装载，.symtab/.strtab/.shstrtab
#   一个字节都用不到（musl 静态 ELF 的符号表约 9 KB）；musl64_test 只断言 16384 < size < 65536，
#   tools/musl_build_win.sh 里那份自检照旧跑在 strip **之前**（它只解析 ELF 头/程序头）。
$OBJCOPY --strip-all "$BUILD/musl_hello.elf"
echo "    演示程序（卷交付）：$BUILD/musl_hello.elf = $(stat -c%s "$BUILD/musl_hello.elf") B（musl libc.a 静态链接；已 strip 符号表）"
echo "==> ★ A3 下半：动态链接（我们自己的 ld.so + libfoo.so + dynhello.elf；构建期自检见脚本）"
# 见 tools/dynlink_build_win.sh 与 docs/应用层与系统调用说明.md 的"A3 下半：动态链接"节：
#   * 三份产物都**不塞进**用户窗口的 64KiB 装载区，而是启动期由内核幂等装进 VimtuFS2：
#       /lib/ldvimtu.so（解释器） /lib/libfoo.so（共享库） /dynhello.elf（动态主程序）；
#   * ★ 本批：**不再内嵌进内核** —— 交付 = 系统卷里的 /lib/ldvimtu.so、/lib/libfoo.so、
#     /dynhello.elf（tools/demo_pack_win.py 构建期写入 + 逐字节回读自检）；空夹具盘由
#     "原始区"兜底（见 kernel/demo64.h）。
bash tools/dynlink_build_win.sh "$BUILD"
echo "    演示程序（卷交付）：$(stat -c%s "$BUILD/ldvimtu.so") + $(stat -c%s "$BUILD/libfoo.so") + $(stat -c%s "$BUILD/dynhello.elf") = $(( $(stat -c%s "$BUILD/ldvimtu.so") + $(stat -c%s "$BUILD/libfoo.so") + $(stat -c%s "$BUILD/dynhello.elf") )) B"

echo "==> ★ A4-2b：TinyCC（**交付 = 系统卷里的 /tcc + /bin/tcc + /lib/tcc.bin**，内核里一个字节都不加）"
# 为什么这样交付（内核区只剩 ~187 KB；tcc 是 280 KB 量级的编译器）：
#   1) tools/tcc_build_win.sh 手写构建（不走上游 configure/Makefile，理由见那个脚本的头部）：
#        * build64/tcc.bin  —— 真 tcc：静态 ELF64，**非 PIC、按 4GiB+0x90000 定址**（-mcmodel=large）
#        * build64/tcc      —— 装载驱动（< 64 KiB，走内核的主程序装载器）：它 mmap 到 4GiB+0x90000、
#                              把 tcc 的段搬进去、改 auxv、jmp 进 tcc（详见 user/apps/tcc/tccdrv.c）
#        * build64/tcc_stage/ —— /tcc 那棵树的离线镜像（libtcc1.a + musl 头 + crt1.o/crti.o/crtn.o/libc.a）
#   2) tools/tcc_pack_win.py 把上面这些东西 + A4-1 的 shell 一起离线写进**同一块** VimtuFS2 卷
#      （写完逐字节回读自检；tcc.bin 单文件 282 KB 要用卷格式的**二级间接**，写入器在 A4-1 那份上补了这一层）。
#   3) **内核二进制里搜不到 tcc 的字节**（后面有断言）—— 这正是"内核区只剩 187 KB"的纪律要求。
bash tools/tcc_build_win.sh "$BUILD"

echo "==> ★ A4-4a/A4-4c：Lua 5.4.7（**交付 = 系统卷里的 /bin/lua + /lib/lua.bin + /tcc/demo/*.lua**）"
# 交付方式与 tcc 同构（内核里一个字节都不加；内核区只剩 ~187 KB）：
#   * tools/lua_build_win.sh：
#       build64/lua.bin —— 真解释器：静态 musl、非 PIC、按 4GiB+0x90000 定址（-mcmodel=large）；
#       build64/lua     —— 装载驱动（< 64 KiB，走内核主程序装载器）：mmap 到 4GiB+0x90000、
#                          搬段、改 auxv、jmp 进 Lua（详见 user/lua/luadrv.c）
#   * tools/lua_pack_win.py 把上面两件 + user/lua/demo/*.lua 装进"tcc 那块卷"（逐字节回读自检）。
bash tools/lua_build_win.sh "$BUILD"

echo "==> ★ A4-4c：用户态 gzip/gunzip（**交付 = 系统卷里的 /bin/gzip + /bin/gunzip**）"
# tools/gzip_build_win.sh：自足（不引 user/lib / musl）的 < 64 KiB 静态 ELF64 —— 直接走内核的
# 主程序装载器（`run /bin/gzip ...`），deflate = fixed-Huffman + stored，inflate = 完整实现。
bash tools/gzip_build_win.sh "$BUILD"

echo "==> ★ A3 下半：FPU/xmm 上下文回归程序（user/xmmsse.asm；两个进程同时跑）"
# 见 user/xmmsse.asm 顶部说明：同一份 ELF 起两个真进程、各自核对 16 个 xmm 是否被对方污染。
# 链接脚本 user/xmmsse_elf64.ld（不是 hello_elf64.ld）：本程序只有 FPU 回归、没有段权限断言，
# 所以贴紧排布 + --strip-all —— 9456 B -> 约 2.3 KB。★ 本批：这份字节**不内嵌内核**，
# 交付 = 系统卷 /xmmsse.elf（tools/demo_pack_win.py 写入 + 逐字节回读自检；空夹具盘走"原始区"）。
# 安全性依据（装载器不做页同余检查、同页复用物理页）写在那个脚本的头部注释里。
$NASM -f elf64 user/xmmsse.asm -o "$BUILD/xmmsse.o"
$LD -m elf_x86_64 --strip-all -T user/xmmsse_elf64.ld -o "$BUILD/xmmsse.elf" "$BUILD/xmmsse.o"
echo "    演示程序（卷交付）：$BUILD/xmmsse.elf = $(stat -c%s "$BUILD/xmmsse.elf") B"
echo "==> 可安装应用示例（VAP64：nasm -> tools/make_vap.py；交付 = 系统卷 /hello.vap）"
# user/hello64.asm 是 ring3 程序；tools/make_vap.py 给它加 32B VAP64 头（含代码段 CRC32）；
# ★ 本批：整个 .vap **不内嵌内核**（app64.cpp 启动时保证它在 VimtuFS2 的 /hello.vap 里，
# 再从盘上读出来跑；空夹具盘回落到构建期"原始区"）。

echo "==> 多进程演示程序（批次 C：fork/execve/wait4/kill；交付 = 系统卷文件）"
# user/proc64.asm 是 ring3 程序，用 syscall 指令；链接脚本复用 user/hello_elf64.ld
# （把映像钉在用户窗口 4GiB 起、低于 USER64_STACK_VA64 —— elf64.cpp 会拒绝越界的 PT_LOAD）。
# proc64.cpp 保证它装成 VimtuFS2 的 /proc64.elf，再由 proc64_demo64() 以 init 进程跑起来。
$NASM -f elf64 user/proc64.asm -o "$BUILD/proc64.o"
$LD -m elf_x86_64 --strip-all -T user/hello_elf64.ld -o "$BUILD/proc64.elf" "$BUILD/proc64.o"
echo "==> pipe64：ring3 管道演示（批次 D：fork 后父子各持一端；交付 = 系统卷文件）"
# user/pipe64.asm 用 syscall 指令走 Linux ABI：pipe(22)/fork(57)/read(0)/write(1)/close(3)/
# nanosleep(35)/wait4(61)/exit(60)。proc64.cpp 保证它装成 /pipe64.elf，再由
# proc64_pipe_demo64() 当成一个真进程跑起来（fork 后子写父读）。
$NASM -f elf64 user/pipe64.asm -o "$BUILD/pipe64.o"
$LD -m elf_x86_64 --strip-all -T user/hello_elf64.ld -o "$BUILD/pipe64.elf" "$BUILD/pipe64.o"

echo "==> ★ A5 前置：/evshm.elf（用户态输入事件投递 + 共享内存缓冲演示；只嵌**系统内核**）"
# user/apps/evshm_demo.c 用自研 user/lib 编成**静态 ELF64**（链接脚本 user/apps/evshm_demo.ld：
# 4GiB 基址 + .text.start + .l* 段；无 PT_INTERP/PT_DYNAMIC/重定位），
# kernel/proc64.cpp 启动期幂等把它装进 VimtuFS2 的 /evshm.elf，再以**真进程**跑起来
# （演示要 fork：父子两个进程共享同一块 shm；blob 路径没有进程上下文，fork 会是 -ENOSYS）。
# 体积记账：这份 blob 只进**系统内核**（安装介质不链 proc64.cpp，也没有 ELF64 加载器）。
EVSHM_DIR="$BUILD/uapps/evshm"
mkdir -p "$EVSHM_DIR"
EVSHM_UCFLAGS="--target=x86_64-unknown-none-elf -nostdinc -I user/lib \
 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
 -mcmodel=large -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-avx \
 -fno-asynchronous-unwind-tables -fno-unwind-tables \
 -ffunction-sections -fdata-sections -std=c11 -O2 -Wall -Wextra"
EVSHM_ASFLAGS="--target=x86_64-unknown-none-elf -nostdinc -ffreestanding -fno-pic -fno-pie \
 -ffunction-sections -fdata-sections -Wall -Wextra"
EVSHM_OBJS=""
for src in syscall.c string.c stdlib.c stdio.c fb.c; do
    clang $EVSHM_UCFLAGS -c "user/lib/$src" -o "$EVSHM_DIR/lib_${src%.c}.o"
    EVSHM_OBJS="$EVSHM_OBJS $EVSHM_DIR/lib_${src%.c}.o"
done
for src in crt0.S syscall.S; do
    clang $EVSHM_ASFLAGS -c "user/lib/$src" -o "$EVSHM_DIR/lib_${src%.S}_asm.o"
    EVSHM_OBJS="$EVSHM_OBJS $EVSHM_DIR/lib_${src%.S}_asm.o"
done
clang $EVSHM_UCFLAGS -c user/apps/evshm_demo.c -o "$EVSHM_DIR/evshm_demo.o"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T user/apps/evshm_demo.ld \
    -o "$BUILD/evshm.elf" $EVSHM_OBJS "$EVSHM_DIR/evshm_demo.o"
# 自检（纯 Python 解析 ELF 头/程序头）：PT_LOAD 落在 4GiB..4GiB+64KiB、无 PT_INTERP/PT_DYNAMIC、
# 程序头表在首个 PT_LOAD 内、入口在某个段里 —— 这些是 kernel/elf64.cpp 的硬门槛，提前拦住。
$PY - "$BUILD/evshm.elf" <<'PYEVSHM'
import struct, sys
path = sys.argv[1]
d = open(path, "rb").read()
assert d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 1, "不是 ELF64 小端"
etype, machine = struct.unpack_from("<HH", d, 16)
assert etype == 2 and machine == 0x3E, "必须是 ET_EXEC / x86_64"
entry = struct.unpack_from("<Q", d, 24)[0]
phoff = struct.unpack_from("<Q", d, 32)[0]
phentsize, phnum = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
assert phentsize == 56 and 0 < phnum <= 16, "程序头表不合法"
LO, HI = 0x100000000, 0x100000000 + 0x10000
segs, nload, first_off, first_filesz = [], 0, None, None
for i in range(phnum):
    p = phoff + i * phentsize
    ptype = struct.unpack_from("<I", d, p)[0]
    poff, pva, _ppa, pfsz, pmsz, _al = struct.unpack_from("<QQQQQQ", d, p + 8)
    if ptype == 3:
        raise SystemExit("ERROR: 出现 PT_INTERP（内核只做静态装载）")
    if ptype == 2:
        raise SystemExit("ERROR: 出现 PT_DYNAMIC（静态链接不该有）")
    if ptype != 1:
        continue
    nload += 1
    if first_off is None:
        first_off, first_filesz = poff, pfsz
    assert pmsz >= pfsz and poff + pfsz <= len(d), "段文件范围越界"
    assert pva >= LO and pva + pmsz <= HI, "PT_LOAD 越出装载区（va=0x%x memsz=0x%x）" % (pva, pmsz)
    assert (pva + pmsz + 0xFFF) & ~0xFFF <= HI, "PT_LOAD 页对齐后压到用户栈区"
    segs.append((pva, pmsz))
assert nload and any(va <= entry < va + msz for va, msz in segs), "入口不在任何 PT_LOAD 内"
assert first_off == 0 and phoff + phnum * phentsize <= first_filesz, "程序头表不在首个 PT_LOAD 内"
assert len(d) <= 96 * 1024, "文件超过内核读盘缓冲 96 KiB"
print("    evshm.elf 自检 OK：entry=0x%x phnum=%d segs=%d size=%d B" % (entry, phnum, nload, len(d)))
PYEVSHM
echo "    演示程序（卷交付）：$BUILD/evshm.elf = $(stat -c%s "$BUILD/evshm.elf") B（静态 ELF64；proc64.cpp 保证它在系统卷 /evshm.elf 里）"

echo "==> ★ A5：/wlclient.elf（Wayland 基础骨架的 ring3 客户端；只嵌**系统内核**）"
# user/apps/wlclient.c 用自研 user/lib 编成**静态 ELF64**（链接脚本复用 user/apps/evshm_demo.ld：
# 4GiB 基址 + .text.start + .l* 段；无 PT_INTERP/PT_DYNAMIC/重定位），
# kernel/wl64.cpp 启动期幂等把它装进 VimtuFS2 的 /wlclient.elf，再以**真进程**跑起来。
# 源文件清单**不复制** EVSHM 那一份（只加 Wayland 包装 user/lib/wl.c）：多编出来的目标文件
# 不会进别人的链接行，也就不会挪动 evshm/sig64 的字节。
# 入口用 user/lib/crt0.S（argc=1、argv[0]="user64"）：本程序**不靠 argv**，full/short 模式由
# /etc/wl64_probe 探针决定（内核与客户端各自判一次同一个文件，见 kernel/wl64.h 与 wlclient.c）。
WL_DIR="$BUILD/uapps/wlclient"
mkdir -p "$WL_DIR"
WL_OBJS=""
for src in syscall.c string.c stdlib.c stdio.c wl.c; do
    clang $EVSHM_UCFLAGS -c "user/lib/$src" -o "$WL_DIR/lib_${src%.c}.o"
    WL_OBJS="$WL_OBJS $WL_DIR/lib_${src%.c}.o"
done
for src in crt0.S syscall.S; do
    clang $EVSHM_ASFLAGS -c "user/lib/$src" -o "$WL_DIR/lib_${src%.S}_asm.o"
    WL_OBJS="$WL_OBJS $WL_DIR/lib_${src%.S}_asm.o"
done
clang $EVSHM_UCFLAGS -c user/apps/wlclient.c -o "$WL_DIR/wlclient.o"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T user/apps/evshm_demo.ld \
    -o "$BUILD/wlclient.elf" $WL_OBJS "$WL_DIR/wlclient.o"
# 自检（纯 Python 解析 ELF 头/程序头，判据与 evshm.elf 完全相同：PT_LOAD 落在装载区、无
# PT_INTERP/PT_DYNAMIC、程序头表在首个 PT_LOAD 内、入口在某个段里）—— kernel/elf64.cpp 的硬门槛。
$PY - "$BUILD/wlclient.elf" <<'PYWL'
import struct, sys
path = sys.argv[1]
d = open(path, "rb").read()
assert d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 1, "不是 ELF64 小端"
etype, machine = struct.unpack_from("<HH", d, 16)
assert etype == 2 and machine == 0x3E, "必须是 ET_EXEC / x86_64"
entry = struct.unpack_from("<Q", d, 24)[0]
phoff = struct.unpack_from("<Q", d, 32)[0]
phentsize, phnum = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
assert phentsize == 56 and 0 < phnum <= 16, "程序头表不合法"
LO, HI = 0x100000000, 0x100000000 + 0x10000
segs, nload, first_off, first_filesz = [], 0, None, None
for i in range(phnum):
    p = phoff + i * phentsize
    ptype = struct.unpack_from("<I", d, p)[0]
    poff, pva, _ppa, pfsz, pmsz, _al = struct.unpack_from("<QQQQQQ", d, p + 8)
    if ptype == 3:
        raise SystemExit("ERROR: 出现 PT_INTERP（内核只做静态装载）")
    if ptype == 2:
        raise SystemExit("ERROR: 出现 PT_DYNAMIC（静态链接不该有）")
    if ptype != 1:
        continue
    nload += 1
    if first_off is None:
        first_off, first_filesz = poff, pfsz
    assert pmsz >= pfsz and poff + pfsz <= len(d), "段文件范围越界"
    assert pva >= LO and pva + pmsz <= HI, "PT_LOAD 越出装载区（va=0x%x memsz=0x%x）" % (pva, pmsz)
    assert (pva + pmsz + 0xFFF) & ~0xFFF <= HI, "PT_LOAD 页对齐后压到用户栈区"
    segs.append((pva, pmsz))
assert nload and any(va <= entry < va + msz for va, msz in segs), "入口不在任何 PT_LOAD 内"
assert first_off == 0 and phoff + phnum * phentsize <= first_filesz, "程序头表不在首个 PT_LOAD 内"
assert len(d) <= 96 * 1024, "文件超过内核读盘缓冲 96 KiB"
print("    wlclient.elf 自检 OK：entry=0x%x phnum=%d segs=%d size=%d B" % (entry, phnum, nload, len(d)))
PYWL
# ★ 体积纪律（与 shell/tcc/lua/gzip/edit 同一条）：/wlclient.elf **不内嵌进内核** —— 它是
#   "系统卷里的文件"，由验收夹具写进卷（tests/wl64_test.py 的夹具盘）；内核只在卷里按路径找它，
#   找不到就如实打一行 [WL64] demo skipped (no elf on vfs)。理由：system.img 的内核余量只剩
#   ~700 KB 的硬线（tests/a42a64_test.py 断言），而这个 blob 值 ~18 KB；更重要的是这本来就是
#   本项目对"可交付程序"的一贯交付方式（内核里搜不到它的字节 —— 见下面那条断言）。
echo "    /wlclient.elf = $(stat -c%s "$BUILD/wlclient.elf") B（**不内嵌**：由夹具写进系统卷 /wlclient.elf）"
# ★ B-wm：Ring 3 合成器 + 两个真客户端（user/wm/*.c）—— **不内嵌**，卷交付（同 /wlclient.elf）。
#   为什么放这里：三者共用同一批 user/lib 目标文件（WL_OBJS），也不进任何人的链接行。
#   交付路径（由验收夹具 / tools/wm_pack_win.py 写进系统卷）：/bin/wm.elf、/wmclock.elf、/wmpanel.elf。
#   ★ 内核**不会**把它们内嵌：kernel/wl64.cpp 只按路径在卷里找 /bin/wm.elf；找不到就打
#     [WL64] wm skipped (no elf on vfs)。下面还有一条"内核二进制里搜不到 wm.elf 字节"的断言。
WM_DIR="$BUILD/uapps/wm"
mkdir -p "$WM_DIR"
WM_OBJS=""
for src in syscall.c string.c stdlib.c stdio.c fb.c wl.c; do
    clang $EVSHM_UCFLAGS -c "user/lib/$src" -o "$WM_DIR/lib_${src%.c}.o"
    WM_OBJS="$WM_OBJS $WM_DIR/lib_${src%.c}.o"
done
for src in crt0.S syscall.S; do
    clang $EVSHM_ASFLAGS -c "user/lib/$src" -o "$WM_DIR/lib_${src%.S}_asm.o"
    WM_OBJS="$WM_OBJS $WM_DIR/lib_${src%.S}_asm.o"
done
clang $EVSHM_UCFLAGS -I user/wm -c user/wm/wmabi.c -o "$WM_DIR/wmabi.o"
WM_OBJS="$WM_OBJS $WM_DIR/wmabi.o"
for app in wm wmclock wmpanel; do
    clang $EVSHM_UCFLAGS -I user/wm -c "user/wm/$app.c" -o "$WM_DIR/$app.o"
    $LD -m elf_x86_64 -static --gc-sections -z noexecstack -T user/apps/evshm_demo.ld \
        -o "$BUILD/$app.elf" $WM_OBJS "$WM_DIR/$app.o"
    # 自检（判据与 evshm/wlclient 完全相同：静态 ET_EXEC、PT_LOAD 落在 4GiB..4GiB+64KiB、
    # 无 PT_INTERP/PT_DYNAMIC、程序头表在首个 PT_LOAD 内、入口在某个段里、文件 <= 96 KiB）
    "$PY" - "$BUILD/$app.elf" "$app" <<'PYWM'
import struct, sys
path, name = sys.argv[1], sys.argv[2]
d = open(path, "rb").read()
assert d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 1, "不是 ELF64 小端"
etype, machine = struct.unpack_from("<HH", d, 16)
assert etype == 2 and machine == 0x3E, "必须是 ET_EXEC / x86_64"
entry = struct.unpack_from("<Q", d, 24)[0]
phoff = struct.unpack_from("<Q", d, 32)[0]
phentsize, phnum = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
assert phentsize == 56 and 0 < phnum <= 16, "程序头表不合法"
LO, HI = 0x100000000, 0x100000000 + 0x10000
segs, nload, first_off, first_filesz = [], 0, None, None
for i in range(phnum):
    p = phoff + i * phentsize
    ptype = struct.unpack_from("<I", d, p)[0]
    poff, pva, _ppa, pfsz, pmsz, _al = struct.unpack_from("<QQQQQQ", d, p + 8)
    if ptype == 3:
        raise SystemExit("ERROR: %s 出现 PT_INTERP（内核只做静态装载）" % name)
    if ptype == 2:
        raise SystemExit("ERROR: %s 出现 PT_DYNAMIC（静态链接不该有）" % name)
    if ptype != 1:
        continue
    nload += 1
    if first_off is None:
        first_off, first_filesz = poff, pfsz
    assert pmsz >= pfsz and poff + pfsz <= len(d), "%s 段文件范围越界" % name
    assert pva >= LO and pva + pmsz <= HI, "%s PT_LOAD 越出装载区（va=0x%x memsz=0x%x）" % (name, pva, pmsz)
    assert (pva + pmsz + 0xFFF) & ~0xFFF <= HI, "%s PT_LOAD 页对齐后压到用户栈区" % name
    segs.append((pva, pmsz))
assert nload and any(va <= entry < va + msz for va, msz in segs), "%s 入口不在任何 PT_LOAD 内" % name
assert first_off == 0 and phoff + phnum * phentsize <= first_filesz, "%s 程序头表不在首个 PT_LOAD 内" % name
assert len(d) <= 96 * 1024, "%s 文件超过内核读盘缓冲 96 KiB" % name
print("    %s.elf 自检 OK：entry=0x%x phnum=%d segs=%d size=%d B" % (name, entry, phnum, nload, len(d)))
PYWM
    echo "    /bin/$app.elf <- build64/$app.elf = $(stat -c%s "$BUILD/$app.elf") B（**不内嵌**，卷交付）"
done

echo "==> ★ A4-5：信号投递演示程序（user/apps/sig64_demo.c -> build64/sig64.elf -> **卷交付**）"
# 交付方式与 /evshm.elf 完全同构（复用同一批 user/lib 目标文件 EVSHM_OBJS，只是多编一个 .c）：
# 静态 ELF64（链接脚本 user/apps/evshm_demo.ld）-> **不内嵌内核**；
# 交付 = 系统卷 /sig64.elf（tools/demo_pack_win.py 构建期写入 + 逐字节回读自检），
# 空夹具盘由"原始区"兜底；启动期由 kernel64.cpp 的 sig64_demo64() 保证它装进系统卷再以
# **真进程**跑（它要 fork + signal + wait4 + 用户态异常，blob 路径没有进程上下文）。
# 体积记账（本批）：这份 blob 已**不在** kernel64_os.bin 里。
SIG64_DIR="$BUILD/uapps/sig64"
mkdir -p "$SIG64_DIR"
clang $EVSHM_UCFLAGS -c user/apps/sig64_demo.c -o "$SIG64_DIR/sig64_demo.o"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T user/apps/evshm_demo.ld \
    -o "$BUILD/sig64.elf" $EVSHM_OBJS "$SIG64_DIR/sig64_demo.o"
echo "    演示程序（卷交付）：$BUILD/sig64.elf = $(stat -c%s "$BUILD/sig64.elf") B（静态 ELF64；kernel64.cpp 保证它在系统卷 /sig64.elf 里）"
$NASM -f bin user/hello64.asm -o "$BUILD/hello64.bin"
"$PY" tools/make_vap.py "$BUILD/hello64.bin" "$BUILD/hello.vap" hello
echo "    /hello.vap = $(stat -c%s "$BUILD/hello.vap") B（**不内嵌**：交付 = 系统卷 /hello.vap）"

echo "==> 真正的 ELF64 可执行程序（nasm -f elf64 -> ld.lld -T；交付 = 系统卷 /hello.elf）"
# user/hello_elf64.asm 是 ring3 程序，用 **syscall 指令**（Linux ABI）与内核通信；
# user/hello_elf64.ld 把映像钉在用户窗口（4GiB 起、低于 USER64_STACK_VA64）——
# elf64.cpp 会拒绝越出用户窗口的 PT_LOAD，所以链接地址不能随便放。
# ★ 本批：**不内嵌内核**（安装介质不带 ELF64 加载器，系统内核也不再带它的字节）；
# 空夹具盘由"原始区"兜底（见 kernel/demo64.h）。
$NASM -f elf64 user/hello_elf64.asm -o "$BUILD/hello_elf64.o"
$LD -m elf_x86_64 -T user/hello_elf64.ld -o "$BUILD/hello.elf" "$BUILD/hello_elf64.o"
echo "    /hello.elf = $(stat -c%s "$BUILD/hello.elf") B（**不内嵌**：交付 = 系统卷 /hello.elf）"
echo "==> spin64：长命用户程序（终端 proc run 用；任务管理器进程页 / proc64 kill 的验证目标）"
# user/spin64.asm 打印 pid 后每 1 秒 nanosleep，永不退出；★ 本批：**不内嵌内核** ——
# 交付 = 系统卷 /spin.elf（tools/demo_pack_win.py 写入）；空夹具盘由"原始区"兜底，
# terminal64.cpp 的 `proc run spin` 与启动期 slotreuse 演示共用这一份。
$NASM -f elf64 user/spin64.asm -o "$BUILD/spin64.o"
$LD -m elf_x86_64 --strip-all -T user/hello_elf64.ld -o "$BUILD/spin64.elf" "$BUILD/spin64.o"
echo "    /spin.elf = $(stat -c%s "$BUILD/spin64.elf") B（**不内嵌**：交付 = 系统卷 /spin.elf）"

echo "==> filedemo64：ring3 读文件演示（open /t.txt -> read -> write；交付 = 系统卷 /filedemo.elf）"
# user/filedemo64.asm 用 syscall 指令走 Linux ABI：open(2)/read(0)/write(1)/close(3)/exit(60)。
# terminal64.cpp 开终端时保证它在卷里（终端 `run filedemo` 即可跑；批次 B 的 syscall
# open/read/close 接真 FD 层的证据）。
$NASM -f elf64 user/filedemo64.asm -o "$BUILD/filedemo64.o"
$LD -m elf_x86_64 --strip-all -T user/hello_elf64.ld -o "$BUILD/filedemo64.elf" "$BUILD/filedemo64.o"
echo "    /filedemo.elf = $(stat -c%s "$BUILD/filedemo64.elf") B（**不内嵌**：交付 = 系统卷 /filedemo.elf）"

echo "==> AP 跳板（SMP：nasm 平铺二进制 -> objcopy 嵌入**系统内核**）"
# kernel/ap_trampoline64.asm 由 BSP 原字节拷到物理 0x8000，再由 SIPI（向量 0x08）拉起 AP：
#   nasm -f bin 直接出平铺二进制（按 [org 0x8000] 汇编，**不是**位置无关，见文件顶部说明）
#   -> objcopy 变 elf64 目标 -> 只链进系统内核（安装介质不跑 SMP，不链 smp64.o）。
#   符号名由 objcopy 按输入路径生成：_binary_build64_ap_trampoline64_bin_start/_end。
$NASM -f bin kernel/ap_trampoline64.asm -o "$BUILD/ap_trampoline64.bin"
$OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 "$BUILD/ap_trampoline64.bin" "$BUILD/ap_trampoline64.o"
cp "$BUILD/ap_trampoline64.o" "$BUILD/os/ap_trampoline64.o"
echo "    AP 跳板 = $(stat -c%s "$BUILD/ap_trampoline64.bin") 字节（按 0x8000 汇编）"
# ★ 本批（演示程序搬进系统卷）：不再有 user_fbdemo64.o —— 汇编版 fbdemo 的字节也走
#   /bin/fbdemo64.bin（构建期装卷；VIMTU_USER_FBDEMO=asm 时内核从"原始区"取它来跑）。
#   所以系统内核的链接行里这一项恒为空（保留变量名让链接行读起来还是同一套结构）。
ASM_FBDEMO_OBJ=""
echo "==> ★ 本批：演示程序 blob 打包（原始区 + 内核偏移表；内核里只有元数据，0 字节本体）"
# 顺序：所有演示 blob 都已编完（上面）-> 拼"原始区"（dd 进 system.img 的内核区尾部）+ 生成
# kernel/demo64.h 需要的偏移表 -> 编译 kernel/demo64.cpp。
# ★ 只有这一个目标文件需要 -I"$BUILD"（它 include 构建期生成的 demo64_blobtab.h）。
"$PY" tools/demo_pack_win.py --build "$BUILD" \
      --raw "$BUILD/demo64_raw.bin" --header "$BUILD/demo64_blobtab.h"
$CXX -c kernel/demo64.cpp -o "$BUILD/demo64.o" $CXXFLAGS -I"$BUILD"
echo "      kernel/demo64.cpp -> $BUILD/demo64.o（路径/偏移/长度表；blob 本体 0 字节）"

echo "==> 链接两个内核"
$LD -m elf_x86_64 -o "$BUILD/kernel64.elf"    kernel/linker64.ld "$BUILD"/kernel64.o "$BUILD"/console64.o "$BUILD"/x86_64.o \
    "$BUILD"/fb.o "$BUILD"/font.o "$BUILD"/input.o "$BUILD"/input64.o "$BUILD"/mem64.o "$BUILD"/ata64.o "$BUILD"/part64.o "$BUILD"/setup64.o \
    "$BUILD"/fat64.o "$BUILD"/fs64.o "$BUILD"/drive64.o \
    "$BUILD"/bootx64_efi.o "$BUILD"/uefi64_bin.o \
    "$BUILD"/hwinfo64.o "$BUILD"/acpi64.o "$BUILD"/edid64.o "$BUILD"/vfs64.o "$BUILD"/fd64.o "$BUILD"/usermode64.o "$BUILD"/syscall64.o "$BUILD"/sig64.o "$BUILD"/pci64.o \
    "$BUILD"/ahci64.o "$BUILD"/nvme64.o "$BUILD"/hwui64.o \
    "$BUILD"/virtio_gpu64.o \
    "$BUILD"/display64.o \
    "$BUILD"/entry64.o "$BUILD"/isr_stubs64.o "$BUILD"/switch64.o "$BUILD"/syscall_entry64.o \
    "$BUILD"/demo64.o "$BUILD"/font_*_z.o "$BUILD"/icon_start_mini.o
$OBJCOPY -O binary "$BUILD/kernel64.elf" "$BUILD/kernel64.bin"

$LD -m elf_x86_64 -o "$BUILD/kernel64_os.elf" kernel/linker64.ld "$BUILD/os"/kernel64.o "$BUILD/os"/console64.o "$BUILD/os"/x86_64.o \
    gui_rs/gui_rs.o \
    "$BUILD/os"/fb.o "$BUILD/os"/font.o "$BUILD/os"/input.o "$BUILD/os"/input64.o "$BUILD/os"/mem64.o \
    "$BUILD/os"/hwinfo64.o "$BUILD/os"/acpi64.o "$BUILD/os"/edid64.o "$BUILD/os"/vfs64.o "$BUILD/os"/store64.o "$BUILD/os"/ata64.o "$BUILD/os"/apic64.o "$BUILD/os"/display64.o "$BUILD/os"/fd64.o "$BUILD/os"/usermode64.o "$BUILD/os"/syscall64.o "$BUILD/os"/sig64.o "$BUILD/os"/pci64.o \
    "$BUILD/os"/fat64.o "$BUILD/os"/fs64.o "$BUILD/os"/ahci64.o "$BUILD/os"/nvme64.o "$BUILD/os"/hwui64.o "$BUILD/os"/drive64.o \
    "$BUILD/os"/virtio_gpu64.o \
    "$BUILD/os"/gui64.o "$BUILD/os"/calc64.o "$BUILD/os"/mines64.o \
    "$BUILD/os"/terminal64.o "$BUILD/os"/settings64.o "$BUILD/os"/taskmgr64.o "$BUILD/os"/explorer64.o \
    "$BUILD/os"/sysstate64.o "$BUILD/os"/config64.o "$BUILD/os"/session64.o "$BUILD/os"/panic64.o \
    "$BUILD/os"/theme64.o "$BUILD/os"/gfx64.o "$BUILD/os"/img64.o \
    "$BUILD/os"/locklogin64.o "$BUILD/os"/userdb64.o \
    "$BUILD/os"/wl64.o \
    "$BUILD/os"/startmenu64.o "$BUILD/os"/panels64.o "$BUILD/os"/desktopops64.o \
    "$BUILD/os"/icons64.o \
    "$BUILD/os"/preload64.o "$BUILD/os"/update64.o \
    "$BUILD"/entry64.o "$BUILD"/isr_stubs64.o "$BUILD"/switch64.o "$BUILD"/syscall_entry64.o "$BUILD/os"/task64.o \
    "$BUILD/os"/app64.o "$BUILD/os"/elf64.o "$BUILD/os"/proc64.o \
    "$BUILD/os"/e1000_64.o "$BUILD/os"/net64.o "$BUILD/os"/usb64.o "$BUILD/os"/xhci64.o "$BUILD/os"/hda64.o \
    "$BUILD/os"/smp64.o "$BUILD/os"/ap_trampoline64.o \
    "$BUILD"/demo64.o "$BUILD/os"/font_*_z.o "$BUILD/os"/icon_start_mini.o \
    "$BUILD/os"/kaisi_png.o "$BUILD/os"/iconpack_bin.o
$OBJCOPY -O binary "$BUILD/kernel64_os.elf" "$BUILD/kernel64_os.bin"

KBSZ=$(stat -c%s "$BUILD/kernel64.bin")
OSSZ=$(stat -c%s "$BUILD/kernel64_os.bin")
LDSZ=$(stat -c%s "$BUILD/loader64.bin")
echo "Build OK. 安装程序内核 = ${KBSZ} bytes, 系统内核 = ${OSSZ} bytes, loader = ${LDSZ} bytes"

if [ "$LDSZ" -gt 4096 ]; then echo "ERROR: loader64.bin > 4096" >&2; exit 1; fi
if [ "$KBSZ" -gt $((KERNEL_SECTORS * 512)) ]; then echo "ERROR: 安装程序内核超出内核区" >&2; exit 1; fi
if [ "$OSSZ" -gt $((KERNEL_SECTORS * 512)) ]; then echo "ERROR: 系统内核超出内核区" >&2; exit 1; fi

# ==================== ★ 可选目标：UEFI 运行期 CR3 实验内核 ====================
# 只在显式要求时编（默认构建的两个内核 / 安装介质 / ISO 字节都不受影响）：
#     bash build64.sh --cr3exp          或      VIMTU_BUILD_CR3EXP=1 bash build64.sh
# 产出 build64/kernel64_os_cr3exp.bin（tests/uefi_cr3_experiment_test.py 默认找这个名字），
# 否则该脚本没有实验内核会 SKIP 成 checks=0 的空 PASS（不是真跑过）。
# 做法：只把**引用该宏的 3 个文件**（kernel64.cpp / proc64.cpp / usermode64.cpp ——
# 见 kernel/*.cpp 里的 `#if defined(PROC64_UEFI_CR3_EXPERIMENT)`）用额外宏重编到
# build64/cr3exp/，其余目标文件复用本次构建的 build64/os/*.o 重新链接。
if [ "${VIMTU_BUILD_CR3EXP:-0}" = "1" ] || [ "$1" = "--cr3exp" ]; then
    echo "==> 可选目标：UEFI 运行期 CR3 实验内核（-DPROC64_UEFI_CR3_EXPERIMENT=1）"
    mkdir -p "$BUILD/cr3exp"
    CXXFLAGS_CR3EXP="$CXXFLAGS -DPROC64_UEFI_CR3_EXPERIMENT=1"
    for src in kernel/kernel64.cpp kernel/proc64.cpp kernel/usermode64.cpp; do
        base="$(basename "${src%.cpp}")"
        $CXX -c "$src" -o "$BUILD/cr3exp/$base.o" $CXXFLAGS_CR3EXP
        echo "      $src -> $BUILD/cr3exp/$base.o"
    done
    $LD -m elf_x86_64 -o "$BUILD/kernel64_os_cr3exp.elf" kernel/linker64.ld \
        "$BUILD/cr3exp/kernel64.o" "$BUILD/os"/console64.o "$BUILD/os"/x86_64.o \
        gui_rs/gui_rs.o \
        "$BUILD/os"/fb.o "$BUILD/os"/font.o "$BUILD/os"/input.o "$BUILD/os"/input64.o "$BUILD/os"/mem64.o \
        "$BUILD/os"/hwinfo64.o "$BUILD/os"/acpi64.o "$BUILD/os"/edid64.o "$BUILD/os"/vfs64.o "$BUILD/os"/store64.o "$BUILD/os"/ata64.o "$BUILD/os"/apic64.o "$BUILD/os"/display64.o "$BUILD/os"/fd64.o "$BUILD/cr3exp/usermode64.o" "$BUILD/os"/syscall64.o "$BUILD/os"/sig64.o "$BUILD/os"/pci64.o \
        "$BUILD/os"/fat64.o "$BUILD/os"/fs64.o "$BUILD/os"/ahci64.o "$BUILD/os"/nvme64.o "$BUILD/os"/hwui64.o "$BUILD/os"/drive64.o \
        "$BUILD/os"/virtio_gpu64.o \
        "$BUILD/os"/gui64.o "$BUILD/os"/calc64.o "$BUILD/os"/mines64.o \
        "$BUILD/os"/terminal64.o "$BUILD/os"/settings64.o "$BUILD/os"/taskmgr64.o "$BUILD/os"/explorer64.o \
        "$BUILD/os"/sysstate64.o "$BUILD/os"/config64.o "$BUILD/os"/session64.o "$BUILD/os"/panic64.o \
        "$BUILD/os"/theme64.o "$BUILD/os"/gfx64.o "$BUILD/os"/img64.o \
        "$BUILD/os"/locklogin64.o "$BUILD/os"/userdb64.o \
        "$BUILD/os"/startmenu64.o "$BUILD/os"/panels64.o "$BUILD/os"/desktopops64.o \
        "$BUILD/os"/icons64.o \
        "$BUILD/os"/preload64.o "$BUILD/os"/update64.o \
        "$BUILD"/entry64.o "$BUILD"/isr_stubs64.o "$BUILD"/switch64.o "$BUILD"/syscall_entry64.o "$BUILD/os"/task64.o \
        "$BUILD/os"/app64.o "$BUILD/os"/elf64.o "$BUILD/cr3exp/proc64.o" \
        "$BUILD/os"/wl64.o \
        "$BUILD/os"/e1000_64.o "$BUILD/os"/net64.o "$BUILD/os"/usb64.o "$BUILD/os"/xhci64.o "$BUILD/os"/hda64.o \
        "$BUILD/os"/smp64.o "$BUILD/os"/ap_trampoline64.o \
        "$BUILD"/demo64.o "$BUILD/os"/font_*_z.o "$BUILD/os"/icon_start_mini.o \
        "$BUILD/os"/kaisi_png.o "$BUILD/os"/iconpack_bin.o   # ★ 收口修：图标包也是内嵌对象（与系统内核链接行一致）
    $OBJCOPY -O binary "$BUILD/kernel64_os_cr3exp.elf" "$BUILD/kernel64_os_cr3exp.bin"
    echo "实验内核 OK. $BUILD/kernel64_os_cr3exp.bin = $(stat -c%s "$BUILD/kernel64_os_cr3exp.bin") bytes"
fi

echo "==> 组装系统镜像载荷 system.img（$SYS_SECTORS 扇区）"
dd if=/dev/zero of="$BUILD/system.img" bs=512 count="$SYS_SECTORS" status=none
dd if="$BUILD/boot.bin"        of="$BUILD/system.img" conv=notrunc status=none
dd if="$BUILD/loader64.bin"    of="$BUILD/system.img" seek=1 conv=notrunc status=none
dd if="$BUILD/kernel64_os.bin" of="$BUILD/system.img" seek="$KERNEL_LBA" conv=notrunc status=none

# ---- ★ 本批：演示程序 blob 的"原始区"（见 kernel/demo64.h / tools/demo_pack_win.py）----
#   写进内核区尾部（LBA 7497 起）—— loader 会把整个内核区（LBA 9..8008）平铺加载到物理
#   0x100000（BIOS 的 int13/atapi 与两条 UEFI 路径都是"读满 4MB"），所以内核按物理直映
#   就能读到它；内核二进制里只有路径/偏移/长度表（0 字节 blob 本体）。
#   两道断言：内核二进制末尾不许压到原始区起点；原始区不许越出内核区尾。
DEMO64_RAW_LBA=7497
DEMO64_RAW_BYTES=$(stat -c%s "$BUILD/demo64_raw.bin")
KERNEL_OS_SECTORS=$(( (OSSZ + 511) / 512 ))
DEMO64_RAW_SECTORS=$(( (DEMO64_RAW_BYTES + 511) / 512 ))
if [ "$(( KERNEL_LBA + KERNEL_OS_SECTORS ))" -gt "$DEMO64_RAW_LBA" ]; then
    echo "ERROR: 系统内核（$OSSZ B = $KERNEL_OS_SECTORS 扇区）压到演示程序原始区起点（LBA $DEMO64_RAW_LBA）" >&2
    exit 1
fi
if [ "$DEMO64_RAW_SECTORS" -gt "$(( KERNEL_LBA + KERNEL_SECTORS - DEMO64_RAW_LBA ))" ]; then
    echo "ERROR: 演示程序原始区 $DEMO64_RAW_BYTES B（$DEMO64_RAW_SECTORS 扇区）超出内核区尾部" >&2
    exit 1
fi
dd if="$BUILD/demo64_raw.bin" of="$BUILD/system.img" seek="$DEMO64_RAW_LBA" conv=notrunc status=none
DEMO64_RAW_PHYS=$((0x100000 + (DEMO64_RAW_LBA - KERNEL_LBA) * 512))
echo "    演示程序原始区：$DEMO64_RAW_BYTES B -> system.img LBA $DEMO64_RAW_LBA 起（内核区尾部；内核侧按物理 0x$(printf '%X' "$DEMO64_RAW_PHYS") 直映读）"

# ---- ★ A4-2a：图标包已**搬进 VimtuFS2 系统卷**（内核区尾部不再预留、也不再写入 dd）。----
#      改动前：内核区尾部 LBA 7497..8008 被图标包区间占住 -> 系统内核只能用 7,488 扇区
#              （= 3,833,856 B；实测剩 272 B 余量）。
#      改动后：这段区间**全部交还内核**（内核可用到 LBA 8008 = 8,000 扇区 = 4,096,000 B，
#              +262,144 B)；图标包字节改由系统内核内嵌、启动期装进卷的 /icons/pack.bin
#              （见上面 objcopy 段与 kernel/icons64.cpp 的 icpack 加载路径）。
#      两道断言仍然保留（把"内核不许越界"钉死，只是边界从 LBA 7497 放宽到 8008）：
#        ① 内嵌的包字节不能超过 icons64.h 的 ICON64_PACK_MAX_SECTORS 上限（卷里那份的大小上限）；
#        ② 系统内核不能长过内核区（LBA 9..8008）。
ICONPACK_MAX_SECTORS=512                       # = kernel/icons64.h 的 ICON64_PACK_MAX_SECTORS
ICONPACK_BYTES=$(stat -c%s "$RES/iconpack.bin")
ICONPACK_SECTORS=$(( (ICONPACK_BYTES + 511) / 512 ))
KERNEL_OS_SECTORS=$(( (OSSZ + 511) / 512 ))
KERNEL_AREA_BYTES=$(( KERNEL_SECTORS * 512 ))
if [ "$ICONPACK_SECTORS" -gt "$ICONPACK_MAX_SECTORS" ]; then
    echo "ERROR: 图标包 $ICONPACK_BYTES B（$ICONPACK_SECTORS 扇区）超过 icons64.h 的 ICON64_PACK_MAX_SECTORS=$ICONPACK_MAX_SECTORS" >&2
    exit 1
fi
if [ "$KERNEL_OS_SECTORS" -gt "$KERNEL_SECTORS" ]; then
    echo "ERROR: 系统内核（$OSSZ B = $KERNEL_OS_SECTORS 扇区）超出内核区 $KERNEL_SECTORS 扇区（$KERNEL_AREA_BYTES B）" >&2
    exit 1
fi
echo "    内核区（图标包搬走后）：LBA $KERNEL_LBA..$((KERNEL_LBA + KERNEL_SECTORS - 1)) = $KERNEL_SECTORS 扇区 = $KERNEL_AREA_BYTES B 全部可用；"
echo "                            系统内核 $OSSZ B / $KERNEL_OS_SECTORS 扇区（余 $((KERNEL_AREA_BYTES - OSSZ)) B；改动前上限 7,488 扇区 = $((7488 * 512)) B）"
echo "    图标包：$ICONPACK_BYTES B（$ICONPACK_SECTORS 扇区）**内嵌进系统内核** -> 启动期装进系统卷 /icons/pack.bin（运行期 src=vfs）"
KERNEL_FREE_BYTES=$((KERNEL_AREA_BYTES - OSSZ))
echo "    演示程序外置（本批）：18 份演示 blob（合计 $DEMO64_RAW_BYTES B 含对齐）搬出内核二进制；"
echo "                            交付 = 系统卷文件（tools/demo_pack_win.py 写入 + 逐字节回读自检）；"
echo "                            空夹具兜底 = 内核区尾部的演示程序原始区（LBA $DEMO64_RAW_LBA 起，内核里只有偏移表）；"
echo "                            系统内核 3,436,080 B -> $OSSZ B；余量 $KERNEL_FREE_BYTES B = $((KERNEL_FREE_BYTES / 1024)) KiB（本批目标 ≥ 750 KiB = 768,000 B）"

# ★ 本批的硬证据：18 份演示 blob 在**两份内核二进制**里都搜不到
#   （搬移是不是"真搬"就看这一条：哪天有人把 blob 塞回内核，这里必红）。
#   探针取"高熵 64B 窗口"（扫全图挑不同字节值最多的窗口）—— 纯零/同色填充的中段探针
#   会因为内核里本来就有大片同样字节而假命中（实测 hello.elf 的中段就是一片 0）；
#   小文件（<= 4 KiB）直接查**整份文件**是否在内核里（最硬的一条）。
"$PY" - <<'PYDEMO64'
import importlib.util, os, sys
spec = importlib.util.spec_from_file_location("demo_pack_win", "tools/demo_pack_win.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
kernels = [("kernel64.bin（安装程序内核）", open("build64/kernel64.bin", "rb").read()),
           ("kernel64_os.bin（系统内核）", open("build64/kernel64_os.bin", "rb").read())]
bad = 0
for path, name, _mode in m.BLOBS:
    b = open(os.path.join("build64", name), "rb").read()
    whole = len(b) <= 4096                       # 小文件：整份比对（比探针硬）
    if whole:
        probe, off, nv = b, 0, len(set(b))
    else:
        best_off, best_n = -1, -1
        for off in range(0, max(1, len(b) - 64), 32):
            win = b[off:off + 64]
            n = len(set(win))
            if n > best_n:
                best_n, best_off = n, off
        probe, off, nv = (b[best_off:best_off + 64], best_off, best_n) if best_off >= 0 else (b"", -1, -1)
    ok = False
    for kn, k in kernels:
        hit = (probe in k) if probe else False
        if whole:
            hit = hit or (b in k)
        if hit:
            sys.stderr.write("ERROR: %s 里搜得到 %s 的%s（偏移 %d）—— 演示程序没搬干净\n"
                             % (kn, name, "整份字节" if whole else "64B 高熵探针", off))
            bad = 1
    print("    断言 OK：%-22s %s（%s）在两份内核二进制里都搜不到"
          % (name, "整份 %d B" % len(b) if whole else "64B 高熵探针@%-6d 不同字节值 %d" % (off, nv),
             path))
raise SystemExit(bad)
PYDEMO64

echo "==> ★ A4-1：带 /bin/shell.bin 的演示盘 + \"内核里没有 shell 字节\"断言"
# system.img 已经装好 -> 把它 + MBR + 主分区（= 带 /bin/shell.bin 的 VimtuFS2 v4 卷）拼成
# 一块能直接启动的盘：build64/sysdisk.img（验收脚本 tests/sh64_test.py 也用它做夹具）。
# ★ A4-2b / A4-4：造盘 = **多步串起来**（每步都逐字节回读自检，谁都不重写别人那棵树）：
#   1) tools/tcc_pack_win.py：同一块卷里装进 /bin/tcc（驱动）、/lib/tcc.bin（tcc 本体）、
#      /tcc/**（libtcc1.a + 系统头 + crt/libc）、/tcc/demo/*.c、/hello（宿主版 tcc 产物）；
#   2) tools/lua_pack_win.py：在那块卷上再加 /bin/lua + /lib/lua.bin + /tcc/demo/*.lua（★ A4-4a）；
#   3) tools/gzip_pack_win.py：再加 /bin/gzip + /bin/gunzip + /tcc/demo/big1m.txt（★ A4-4c），
#      最后由它拼出完整演示盘 build64/sysdisk.img。
#   卷内容与 A4-1 完全兼容（/bin/shell.bin + /etc/sh64hello.txt + /tmp），shell 部分逐字节同前。
"$PY" tools/tcc_pack_win.py --shell "$BUILD/shell.bin" --bin-tcc "$BUILD/tcc" --tcc "$BUILD/tcc.bin" \
      --stage "$BUILD/tcc_stage" --hello "$BUILD/tcc_demo_hello" --demo-dir user/apps/tcc \
      --vol "$BUILD/tccvol.img"
"$PY" tools/lua_pack_win.py --vol-in "$BUILD/tccvol.img" --vol-out "$BUILD/luavol.img" \
      --drv "$BUILD/lua" --bin "$BUILD/lua.bin" --demo-dir user/lua/demo --probe "$BUILD/a44probe"
"$PY" tools/gzip_pack_win.py --vol-in "$BUILD/luavol.img" --vol-out "$BUILD/sysvol.img" \
      --gzip "$BUILD/gzip" --system "$BUILD/system.img" --disk "$BUILD/sysdisk.img"
# ★ A4-5：**第四步**——把 Ring 3 编辑器装进同一块卷（/bin/edit + 验收夹具 + 1 MiB 大文件），
#   并用编辑后的卷重拼演示盘（sysdisk.img 覆盖为含 /bin/edit 的那份；这一步之前的产物一个字节不动）。
bash user/apps/edit/build_edit.sh "$BUILD"
"$PY" tools/edit_pack_win.py --vol-in "$BUILD/sysvol.img" --vol-out "$BUILD/editvol.img" \
      --edit "$BUILD/edit" --system "$BUILD/system.img" --disk "$BUILD/sysdisk.img"
# ★ 本批（资源外置）：**第五步**——把内核里搬出来的 5 份 raw 资源写进同一块卷的 /etc：
#   /etc/logo.bin(144,000) / /etc/icon_mypc.bin(65,536) / /etc/icon_recycle.bin(65,536) /
#   /etc/icon_term.bin(65,536) / /etc/icon_start.bin(16,384) —— 合计 356,992 B（= 内核省下的字节）。
#   为什么放在最后一步：前四步的产物（shell/tcc/lua/gzip/edit）一个字节都不动，本步只"再加 5 个文件"。
#   盘内位置：全部落在**已存在**的 /etc 里（不新建根级目录 —— 理由见 kernel/icons64.h 的 ★ 段）。
#   ★ 二级间接：/etc/logo.bin 是 282 块 > 132 块，走 VimtuFS2 v4 的 dind（tcc_pack_win.py 已实现），
#   脚本用 tcc_pack_win.verify 把 5 个文件逐字节回读比对（直接块 / ind / dind 三级）。
"$PY" tools/assets_pack_win.py --vol-in "$BUILD/editvol.img" --vol-out "$BUILD/assetsvol.img" \
      --res build --system "$BUILD/system.img" --disk "$BUILD/sysdisk.img"

# ★ B5（本批）：**GNU make 与 tar**（交付 = 系统卷里的文件，内核里一个字节都不加）。
#   为什么这样交付（内核区只剩 ~700 KB）：make 是 300 KB 量级的构建工具，装不进 64 KiB 的
#   主程序装载窗口 —— 与 /bin/tcc、/bin/lua 同构：一个 < 64 KiB 的驱动 /bin/make
#   （user/make/makedrv.c）mmap 到 4GiB+0x90000、把 /lib/make.bin 的段搬进去再 jmp。
#   tar 只有 53 KB，直接当主程序装（不引驱动）。
#   容器里同时补一份 **/bin/sh**（= shell.bin 的字节）：GNU make 的 recipe 就是 `/bin/sh -c '<recipe>'`，
#   而本批刚给 ring3 shell 加了 `-c` 与 make 需要的语义子集（见 user/shell/main.c 的 B5 段）。
echo "==> ★ B5：GNU make（/bin/make + /lib/make.bin + /bin/sh）与 tar（/bin/tar）→ 系统卷"
bash tools/make_build_win.sh "$BUILD"
bash tools/tar_build_win.sh "$BUILD"
"$PY" tools/make_pack_win.py --vol-in "$BUILD/assetsvol.img" --vol-out "$BUILD/makevol.img" \
      --make "$BUILD/make" --bin "$BUILD/make.bin" --shell "$BUILD/shell.bin" \
      --demo-dir user/make/demo --system "$BUILD/system.img" --disk "$BUILD/sysdisk.img"
"$PY" tools/tar_pack_win.py --vol-in "$BUILD/makevol.img" --vol-out "$BUILD/tarvol.img" \
      --tar "$BUILD/tar" --system "$BUILD/system.img" --disk "$BUILD/sysdisk.img"
# ★ 本批（用户态设备映射）：卷链的**最后一步** —— 把 /bin/drvdemo 写进同一块系统卷。
#   读 tarvol.img（上一步的产物）-> 写 drvsvcvol.img + 重拼 sysdisk.img。
#   ★ 与另一条线（Ring 3 合成器）的合流说明：他们的 /bin/wm.elf 由 tools/wm_pack_win.py 写；
#     两个"最后一步"必须**串成一条链**（谁的步骤在后面，谁就读前一步的 --vol-in），
#     否则后跑的那一步会用自己的卷覆盖 sysdisk.img、把前一步的文件丢掉。
#     合并时的正确接法：把本步的 --vol-in 改成上一步的输出卷（或把本步整体后移）。
# ★ 注意执行顺序：drvdemo.elf 要到后面（本文件"Ring 3 驱动服务骨架"那一段）才编出来，
#   所以装卷这一步**放在那里**（见下面的 drvdemo_pack_win.py 调用），不能放在这里。
# 同一条体积纪律：**内核二进制里不能出现 make/tar 的字节**（工具只从系统卷装载）。
# 探针取每个文件中段的 64 字节（代码/数据混排的中段最稳）。
"$PY" - "$BUILD/kernel64_os.bin" "$BUILD/make.bin" "$BUILD/make" "$BUILD/tar" "$BUILD/shell.bin" <<'PYEOF5B'
import sys
k = open(sys.argv[1], "rb").read()
bad = 0
for p in sys.argv[2:]:
    b = open(p, "rb").read()
    mid = len(b) // 2
    probe = b[mid:mid + 64]
    if len(probe) < 64 or probe in k:
        sys.stderr.write("ERROR: system kernel contains %s bytes (delivery must be a volume file)\n" % p)
        bad = 1
    else:
        print("    断言 OK：系统内核 %d B 里搜不到 %s 的 64B 探针（偏移 %d）；它只从系统卷装载"
              % (len(k), p.rsplit("/", 1)[-1], mid))
raise SystemExit(bad)
PYEOF5B
# 断言（两条，都是"资源真的搬走了"的硬证据）：
#   ① **整份文件**的字节在内核里搜不到（最硬的一条：objcopy 一塞回去就必红）；
#   ② 再取一个**高熵探针**（扫全图挑"不同字节值最多"的 64B 窗口；纯色/透明区域的中段探针会因为
#      内核里本来就有大片同色数据而假命中 —— 实测 logo_rgba.bin 的中段就是一片透明），要求也不在内核里。
"$PY" - "$BUILD/kernel64_os.bin" build/logo_rgba.bin build/icon_mycomputer.bin \
      build/icon_recyclebin.bin build/icon_terminal.bin <<'PYASSET'
import sys
k = open(sys.argv[1], "rb").read()
bad = []
for p in sys.argv[2:]:
    b = open(p, "rb").read()
    name = p.rsplit("/", 1)[-1]
    if len(b) >= 64 and b in k:                       # ① 整份文件
        bad.append(p)
        sys.stderr.write("ERROR: 系统内核里含整份 %s（%d B）—— 资源没搬干净\n" % (name, len(b)))
        continue
    best_off, best_n = -1, -1                         # ② 高熵探针（32B 步长扫一遍）
    for off in range(0, max(1, len(b) - 64), 32):
        win = b[off:off + 64]
        n = len(set(win))
        if n > best_n:
            best_n, best_off = n, off
    probe = b[best_off:best_off + 64] if best_off >= 0 else b""
    if len(probe) == 64 and best_n >= 8 and probe in k:
        bad.append(p)
        sys.stderr.write("ERROR: 系统内核里含 %s 的 64B 高熵探针（偏移 %d，不同字节值 %d）\n"
                         % (name, best_off, best_n))
    else:
        print("    断言 OK：系统内核 %d B 里既没有 %s 整份字节，也没有它的 64B 高熵探针"
              "（偏移 %d，不同字节值 %d）" % (len(k), name, best_off, best_n))
raise SystemExit(1 if bad else 0)
PYASSET
# 同样的纪律单独钉一遍 icon_start.bin（它的"内置兜底"只剩 24x24 mip = 2,304 B，
#   64x64 原图必须只在卷里；否则这条搬移就是假的）。
"$PY" - "$BUILD/kernel64_os.bin" build/icon_start.bin build/icon_start_mini.bin <<'PYASSET2'
import sys
k = open(sys.argv[1], "rb").read()
full = open(sys.argv[2], "rb").read()
mini = open(sys.argv[3], "rb").read()
mid = len(full) // 2
bad = 0
# ① 整份原图不能在内核里（64x64 RGBA = 16,384 B）
if full in k:
    sys.stderr.write("ERROR: 系统内核里含整份 icon_start.bin(64x64, %d B)\n" % len(full))
    bad = 1
# ② 中段 64B 探针（仅当它不是"内核里到处都是的同色窗口"时才有判别力）
probe = full[mid:mid + 64]
if len(set(probe)) >= 8 and probe in k:
    sys.stderr.write("ERROR: 系统内核里仍有 icon_start.bin(64x64) 的 64B 探针（偏移 %d）\n" % mid)
    bad = 1
# ③ 内置的 24x24 mip 必须在（无卷盘的兜底就靠它）
if mini not in k:
    sys.stderr.write("ERROR: 系统内核里找不到 icon_start_mini.bin（%d B）\n" % len(mini))
    bad = 1
if not bad:
    print("    断言 OK：icon_start.bin 原图（%d B）不在内核里；内核里只有 24x24 mip（%d B）"
          % (len(full), len(mini)))
raise SystemExit(bad)
PYASSET2
# 断言：内核二进制里**不能**出现 shell.bin 的字节（交付方式必须是"系统卷里的文件"）。
# 探针取 shell 中段的 64 字节（ELF 头/入口附近的字节模式到处都是，中段最稳）。
"$PY" - "$BUILD/kernel64_os.bin" "$BUILD/shell.bin" <<'PYEOF'
import sys
k = open(sys.argv[1], "rb").read()
s = open(sys.argv[2], "rb").read()
mid = len(s) // 2
probe = s[mid:mid + 64]
if len(probe) < 64 or probe in k:
    sys.stderr.write("ERROR: system kernel contains shell.bin bytes (delivery must be a volume file)\n")
    raise SystemExit(1)
print("    断言 OK：系统内核 %d B 里搜不到 shell.bin 的 64B 探针（偏移 %d）；shell 只从系统卷装载" % (len(k), mid))
PYEOF

# ★ A4-2b 的同一条纪律：**内核二进制里不能出现 tcc 的字节**（tcc 只从系统卷 /lib/tcc.bin 装载）。
# 探针取 tcc.bin 中段的 64 字节（ELF 头/入口附近的模式到处都是，中段最稳）。
"$PY" - "$BUILD/kernel64_os.bin" "$BUILD/tcc.bin" <<'PYEOF2'
import sys
k = open(sys.argv[1], "rb").read()
t = open(sys.argv[2], "rb").read()
mid = len(t) // 2
probe = t[mid:mid + 64]
if len(probe) < 64 or probe in k:
    sys.stderr.write("ERROR: system kernel contains tcc.bin bytes (delivery must be a volume file)\n")
    raise SystemExit(1)
print("    断言 OK：系统内核 %d B 里搜不到 tcc.bin 的 64B 探针（偏移 %d）；tcc 只从系统卷装载" % (len(k), mid))
PYEOF2

# ★ A4-4 的同一条纪律：**内核二进制里不能出现 Lua 与 gzip 的字节**（两者都只从系统卷装载；
# 内核区只剩 ~187 KB，交付一律"卷里的文件"）。探针同样取中段 64 字节。
"$PY" - "$BUILD/kernel64_os.bin" "$BUILD/lua.bin" "$BUILD/gzip" <<'PYEOF3'
import sys
k = open(sys.argv[1], "rb").read()
bad = 0
for p in sys.argv[2:]:
    b = open(p, "rb").read()
    mid = len(b) // 2
    probe = b[mid:mid + 64]
    if len(probe) < 64 or probe in k:
        sys.stderr.write("ERROR: system kernel contains %s bytes (delivery must be a volume file)\n" % p)
        bad = 1
    else:
        print("    断言 OK：系统内核 %d B 里搜不到 %s 的 64B 探针（偏移 %d）；它只从系统卷装载"
              % (len(k), p.rsplit("/", 1)[-1], mid))
raise SystemExit(bad)
PYEOF3

# ★ A4-5 的同一条纪律：**内核二进制里不能出现编辑器 /bin/edit 的字节**（它只从系统卷装载）。
# 探针取 build64/edit 中段的 64 字节（ELF 头/入口附近到处都是，中段最稳）。
"$PY" - "$BUILD/kernel64_os.bin" "$BUILD/edit" <<'PYEOF4'
import sys
k = open(sys.argv[1], "rb").read()
e = open(sys.argv[2], "rb").read()
mid = len(e) // 2
probe = e[mid:mid + 64]
if len(probe) < 64 or probe in k:
    sys.stderr.write("ERROR: system kernel contains /bin/edit bytes (delivery must be a volume file)\n")
    raise SystemExit(1)
print("    断言 OK：系统内核 %d B 里搜不到 /bin/edit 的 64B 探针（偏移 %d）；编辑器只从系统卷装载" % (len(k), mid))
PYEOF4

# ★ A5 的同一条纪律：**内核二进制里不能出现 /wlclient.elf 的字节**（它只从系统卷装载）。
#   探针取 build64/wlclient.elf 中段的 64 字节（ELF 头/入口附近的字节模式到处都是，中段最稳）。
"$PY" - "$BUILD/kernel64_os.bin" "$BUILD/wlclient.elf" <<'PYEOF5'
import sys
k = open(sys.argv[1], "rb").read()
w = open(sys.argv[2], "rb").read()
mid = len(w) // 2
probe = w[mid:mid + 64]
if len(probe) < 64 or probe in k:
    sys.stderr.write("ERROR: system kernel contains /wlclient.elf bytes (delivery must be a volume file)\n")
    raise SystemExit(1)
print("    断言 OK：系统内核 %d B 里搜不到 /wlclient.elf 的 64B 探针（偏移 %d）；客户端只从系统卷装载" % (len(k), mid))
PYEOF5

echo "==> ★ A4-2a：ring3 系统调用探针（chdir/rename/rmdir/dup2/utime + execve 失败路径的真证据）"
# 为什么源码由构建脚本生成：本批只允许改 kernel/*、build64.sh、tests/a42a64_test.py、docs —— user/
#   由另一条线在改。这份**验收用**的 ring3 程序因此不落在 user/ 里：源码在构建目录里生成、编成
#   build64/a42a_sys.elf，然后由 tests/a42a64_test.py 装进测试夹具盘的卷里（/bin/a42a_sys.elf），
#   终端里用 `elfrun /bin/a42a_sys.elf` 跑（真进程 + 真系统调用）。
#   它用的是 **syscall 指令 + Linux 号段**（与 musl/shell 同一条 ABI），不是 int 0x80 自有号段。
cat > "$BUILD/a42a_sys.c" <<'A42A_CEOF'
/* a42a_sys.c - A4-2a ring3 系统调用探针（构建期生成；自包含：不 include 任何头、不依赖 libc） */
typedef long i64;
typedef unsigned long u64;

static i64 sc3(i64 n, i64 a, i64 b, i64 c) {
    i64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static i64 sc0(i64 n) { return sc3(n, 0, 0, 0); }
static i64 sc1(i64 n, i64 a) { return sc3(n, a, 0, 0); }
static i64 sc2(i64 n, i64 a, i64 b) { return sc3(n, a, b, 0); }

static void out_str(const char* s) {
    int n = 0;
    while (s[n]) n++;
    (void)sc3(1, 1, (i64)(u64)s, n);          /* write(1, ...) -> 内核控制台（串口 + 屏幕） */
}
static void out_num(i64 v) {
    char b[24];
    int n = 0;
    u64 x;
    if (v < 0) { b[n++] = '-'; x = (u64)(-v); } else { x = (u64)v; }
    char t[24];
    int k = 0;
    if (x == 0) t[k++] = '0';
    while (x) { t[k++] = (char)('0' + (int)(x % 10u)); x /= 10u; }
    while (k) b[n++] = t[--k];
    b[n] = 0;
    out_str(b);
}
static void ev(const char* tag, i64 v) { out_str("A42A "); out_str(tag); out_str("="); out_num(v); out_str("\n"); }

static char  g_cwd[64];
static long  g_tm[2] = { 1, 2 };              /* utimbuf：显式时间（本内核如实 -ENOSYS） */
static long  g_status;

__attribute__((section(".text.start"), noreturn)) void _start(void) {
    /* ① execve 失败路径：/nope.bin 不存在 -> 预检失败 -> -ENOENT，**本进程继续跑**（不再 #PF/PANIC） */
    const i64 ex = sc3(59, (i64)(u64)"/nope.bin", 0, 0);

    /* ② 打开重定向目标 + dup2(file -> 1)：把 stdout 换成文件（重定向的落地动作） */
    const i64 fo = sc3(2, (i64)(u64)"/tmp/a42a_out.txt", 0x241 /*O_WRONLY|O_CREAT|O_TRUNC*/, 0644);
    const i64 d2 = sc2(33, fo, 1);

    /* ③ fork：子进程继承 fd 1 = 文件（"把某个 fd 作为 1 交给子进程"的真路径） */
    const i64 pid = sc0(57);
    if (pid == 0) {
        static const char cs[] = "A42A-child-stdout-in-file\n";
        static const char ce[] = "A42A child stderr on console (fd2 unbound)\n";
        (void)sc3(1, 1, (i64)(u64)cs, (i64)(sizeof(cs) - 1u));   /* -> 文件（fd 1 已被 dup2） */
        (void)sc3(1, 2, (i64)(u64)ce, (i64)(sizeof(ce) - 1u));   /* -> 控制台（fd 2 空槽 = 老语义） */
        (void)sc1(60, 0);
        for (;;) {}
    }
    const i64 w4 = sc3(61, pid, (i64)(u64)&g_status, 0);         /* wait4(pid, &status, 0) */

    /* ④ 用最小控制台 tty 对象把 stdout 换回来：open("/dev/console") + dup2 -> 之后输出回串口/屏幕 */
    const i64 tty = sc3(2, (i64)(u64)"/dev/console", 2 /*O_RDWR*/, 0);
    const i64 d2t = sc2(33, tty, 1);

    ev("execve_nope", ex);
    ev("dup2_file_to_1", d2);
    ev("fork_pid", pid);
    ev("wait4", w4);
    ev("wait_status", g_status);
    ev("tty_fd", tty);
    ev("dup2_tty_to_1", d2t);
    ev("ioctl_tiocgwinsz_on_tty1", sc3(16, 1, 0x5413 /*TIOCGWINSZ*/, (i64)(u64)g_cwd));
    ev("ioctl_tcgets_on_tty1", sc3(16, 1, 0x5401 /*TCGETS*/, (i64)(u64)g_cwd));

    /* ⑤ chdir / getcwd（含相对路径 ".."：/tmp -> /） */
    ev("chdir_tmp", sc1(80, (i64)(u64)"/tmp"));
    (void)sc2(79, (i64)(u64)g_cwd, (i64)sizeof(g_cwd));
    out_str("A42A getcwd_in_tmp="); out_str(g_cwd); out_str("\n");
    ev("chdir_missing", sc1(80, (i64)(u64)"/a42a_nope"));
    ev("chdir_file", sc1(80, (i64)(u64)"/bin/shell.bin"));
    ev("chdir_rel_dotdot", sc1(80, (i64)(u64)".."));
    (void)sc2(79, (i64)(u64)g_cwd, (i64)sizeof(g_cwd));
    out_str("A42A getcwd_root="); out_str(g_cwd); out_str("\n");
    ev("getcwd_small_buf", sc2(79, (i64)(u64)g_cwd, 1));   /* 缓冲 1 字节 = 装不下 "/" + NUL -> ERANGE */

    /* ⑥ 相对路径的 open/stat（按 cwd 解析）：回到 /tmp 再建文件 */
    (void)sc1(80, (i64)(u64)"/tmp");
    ev("open_rel", sc3(2, (i64)(u64)"a42a_rel.txt", 0x241, 0644));
    ev("stat_rel", sc2(4, (i64)(u64)"a42a_rel.txt", (i64)(u64)g_cwd));

    /* ⑦ rename：同目录成功 / 源缺失 / 目标已存在 / 跨目录（如实 -ENOSYS） */
    (void)sc2(82, (i64)(u64)"a42a_rel.txt", (i64)(u64)"a42a_rel2.txt");
    ev("rename_rel_ok", sc2(82, (i64)(u64)"a42a_rel2.txt", (i64)(u64)"a42a_abs.txt"));
    ev("rename_missing", sc2(82, (i64)(u64)"/tmp/a42a_no.txt", (i64)(u64)"/tmp/a42a_x.txt"));
    ev("rename_exists", sc2(82, (i64)(u64)"/tmp/a42a_abs.txt", (i64)(u64)"/tmp/a42a_out.txt"));
    ev("rename_cross_dir", sc2(82, (i64)(u64)"/tmp/a42a_abs.txt", (i64)(u64)"/a42a_moved.txt"));

    /* ⑧ rmdir：成功 / 缺失 / 非空 / 目标是文件 / 根目录 */
    ev("mkdir_d", sc1(83, (i64)(u64)"/tmp/a42a_d"));
    ev("rmdir_ok", sc1(84, (i64)(u64)"/tmp/a42a_d"));
    ev("rmdir_missing", sc1(84, (i64)(u64)"/tmp/a42a_nodir"));
    ev("rmdir_root", sc1(84, (i64)(u64)"/"));
    ev("mkdir_ne", sc1(83, (i64)(u64)"/tmp/a42a_ne"));
    ev("mkdir_ne_sub", sc1(83, (i64)(u64)"/tmp/a42a_ne/sub"));
    ev("rmdir_nonempty", sc1(84, (i64)(u64)"/tmp/a42a_ne"));
    ev("rmdir_ne_sub", sc1(84, (i64)(u64)"/tmp/a42a_ne/sub"));
    ev("rmdir_ne_again", sc1(84, (i64)(u64)"/tmp/a42a_ne"));
    ev("rmdir_file", sc1(84, (i64)(u64)"/tmp/a42a_abs.txt"));

    /* ⑨ utime：NULL = 幂等成功；缺路径 = -ENOENT；显式时间 = -ENOSYS（如实） */
    ev("utime_null", sc2(132, (i64)(u64)"/tmp/a42a_abs.txt", 0));
    ev("utime_missing", sc2(132, (i64)(u64)"/tmp/a42a_nofile.txt", 0));
    ev("utime_explicit", sc2(132, (i64)(u64)"/tmp/a42a_abs.txt", (i64)(u64)g_tm));

    /* ⑩ dup2 / close 的失败路径（错误码必须准确） */
    ev("dup2_bad_old", sc2(33, 99, 1));
    ev("dup2_bad_new", sc2(33, tty, 40));
    ev("dup2_same", sc2(33, 1, 1));
    ev("close_stdin_unbound", sc1(3, 0));
    ev("close_tty1", sc1(3, 1));
    ev("write1_after_close", sc3(1, 1, (i64)(u64)"A42A back on console after close(1)\n", 34));
    out_str("A42A done=0\n");
    (void)sc1(60, 0);
    for (;;) {}
}
A42A_CEOF
clang --target=x86_64-unknown-none-elf -nostdinc -ffreestanding -nostdlib -fno-builtin \
  -fno-stack-protector -fno-pic -fno-pie -fno-zero-initialized-in-bss -mcmodel=large -mno-red-zone \
  -mno-sse -mno-sse2 -mno-mmx -mno-avx -fno-asynchronous-unwind-tables -fno-unwind-tables \
  -ffunction-sections -fdata-sections -std=c11 -O2 -Wall -Wextra \
  -c "$BUILD/a42a_sys.c" -o "$BUILD/a42a_sys.o"
$LD -m elf_x86_64 -T user/lib/user64.ld --gc-sections -o "$BUILD/a42a_sys.elf" "$BUILD/a42a_sys.o"
A42A_SZ=$(stat -c%s "$BUILD/a42a_sys.elf")
if [ "$A42A_SZ" -gt 65536 ]; then
    echo "ERROR: a42a_sys.elf $A42A_SZ B 超过 64KiB（用户窗口装载区上限）" >&2
    exit 1
fi
echo "    $BUILD/a42a_sys.elf = $A42A_SZ B（syscall 指令 + Linux 号段；由 tests/a42a64_test.py 装进夹具卷 /bin/a42a_sys.elf）"

echo "==> ★ 本批：Ring 3 驱动服务骨架 /bin/drvdemo（**不内嵌**内核，卷交付；pci_map_bar 的真实用户）"
# 交付方式与 /bin/shell.bin、/wlclient.elf 完全相同的一条纪律：**内核二进制里一个字节都不加**。
#   1) 源码 user/svc/drvdemo.c 自足（不 include 任何头、不链 user/lib）：另一条线在改 user/lib 与
#      user/shell，这份程序不依赖它们就不会被它们的中间态带崩；
#   2) 链接脚本复用 user/lib/user64.ld（4GiB 定址 + .text.start 第一 + .image_end 哨兵）——
#      与 A4-2a 的 ring3 探针同一套，kernel/elf64.cpp 的装载门槛逐条满足；
#   3) 产物只落 build64/drvdemo.elf，由 tools/drvdemo_pack_win.py 写进系统卷的 /bin/drvdemo
#      （验收夹具 tests/drvsvc64_test.py 也会自己写一份到它的夹具卷里）。
DRVDEMO_SZ=0
clang --target=x86_64-unknown-none-elf -nostdinc -ffreestanding -nostdlib -fno-builtin \
  -fno-stack-protector -fno-pic -fno-pie -fno-zero-initialized-in-bss -mcmodel=large -mno-red-zone \
  -mno-sse -mno-sse2 -mno-mmx -mno-avx -fno-asynchronous-unwind-tables -fno-unwind-tables \
  -ffunction-sections -fdata-sections -std=c11 -O2 -Wall -Wextra \
  -c user/svc/drvdemo.c -o "$BUILD/drvdemo.o"
$LD -m elf_x86_64 -T user/lib/user64.ld --gc-sections -o "$BUILD/drvdemo.elf" "$BUILD/drvdemo.o"
DRVDEMO_SZ=$(stat -c%s "$BUILD/drvdemo.elf")
if [ "$DRVDEMO_SZ" -gt 65536 ]; then
    echo "ERROR: drvdemo.elf $DRVDEMO_SZ B 超过 64KiB（用户窗口装载区上限）" >&2
    exit 1
fi
# 断言：内核二进制里**不能**出现 drvdemo 的字节（交付方式必须是"系统卷里的文件"）。
# 探针取中段的 64 字节（ELF 头/入口附近的字节模式到处都是，中段最稳）。
"$PY" - "$BUILD/kernel64_os.bin" "$BUILD/drvdemo.elf" <<'PYEOFDRV'
import sys
k = open(sys.argv[1], "rb").read()
d = open(sys.argv[2], "rb").read()
mid = len(d) // 2
probe = d[mid:mid + 64]
if len(probe) < 64 or probe in k:
    sys.stderr.write("ERROR: system kernel contains drvdemo.elf bytes (delivery must be a volume file)\n")
    raise SystemExit(1)
print("    断言 OK：系统内核 %d B 里搜不到 drvdemo.elf 的 64B 探针（偏移 %d）；它只从系统卷装载"
      % (len(k), mid))
PYEOFDRV
echo "    /bin/drvdemo = $DRVDEMO_SZ B（静态 ELF64；由 tools/drvdemo_pack_win.py 装进系统卷）"
if [ -f "$BUILD/drvdemo.elf" ] && [ -f "$BUILD/tarvol.img" ]; then
    "$PY" tools/drvdemo_pack_win.py --vol-in "$BUILD/tarvol.img" --vol-out "$BUILD/drvsvcvol.img" \
          --drvdemo "$BUILD/drvdemo.elf" --system "$BUILD/system.img" --disk "$BUILD/sysdisk.img"
fi

# ★ 本批（演示程序搬进系统卷）：卷链的**最后一步** —— 把 18 份演示程序写进同一块系统卷
#   （/hello.elf、/musl_hello.elf、/sig64.elf、/evshm.elf、/lib/ldvimtu.so、/lib/libfoo.so、
#    /dynhello.elf、/xmmsse.elf、/proc64.elf、/pipe64.elf、/spin.elf、/filedemo.elf、/hello.vap、
#    /bin/demo64.bin、/bin/hello_c.bin、/bin/libctest_c.bin、/bin/fbdemo_c.bin、/bin/fbdemo64.bin），
#   并用这卷重拼演示盘 build64/sysdisk.img（前面 drvdemo 那一步的产物一个字节不动：
#   读 drvsvcvol.img -> 写 demovol.img）。
#   工具写完**逐字节回读自检**；内核二进制里搜不到这些字节（上面那条 64B 探针断言）。
echo "==> ★ 本批：演示程序写进系统卷（18 份；内核里 0 字节）+ 重拼 sysdisk.img"
if [ -f "$BUILD/drvsvcvol.img" ]; then
    "$PY" tools/demo_pack_win.py --build "$BUILD" --vol-in "$BUILD/drvsvcvol.img" \
          --vol-out "$BUILD/demovol.img" --system "$BUILD/system.img" --disk "$BUILD/sysdisk.img"
else
    echo "ERROR: 缺少 $BUILD/drvsvcvol.img（卷链上一步没跑成）—— 演示程序没装进系统卷" >&2
    exit 1
fi

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

echo "==> 生成 UEFI 的 ESP 镜像（FAT32 48MB，含 BOOTX64.EFI + 内核 + 载荷）"
"$PY" tools/make_esp.py "$BUILD/esp.img" "$BUILD/BOOTX64.EFI" "$BUILD/kernel64.bin" "$BUILD/system.img" "$BUILD/UEFI64.BIN"

echo "==> 生成 64 位安装 ISO（vimtu64-64.iso：BIOS 光盘 + U 盘 hybrid + UEFI 三种引导）"
"$PY" tools/make_iso64.py
echo
echo "启动安装介质: qemu-system-x86_64 -drive format=raw,file=$IMG -drive format=raw,file=target.img -boot order=c -m 512 -vga std -serial stdio"
echo "端到端安装测试: python tests/install_flow_test.py"

# ★ B-wm 的同一条纪律：**内核二进制里不能出现 /bin/wm.elf 的字节**（它只从系统卷装载）。
#   探针取 build64/wm.elf 中段的 64 字节（与 /wlclient.elf 的判据完全相同）。
"$PY" - "$BUILD/kernel64_os.bin" "$BUILD/wm.elf" <<'PYEOF6'
import sys
k = open(sys.argv[1], "rb").read()
w = open(sys.argv[2], "rb").read()
mid = len(w) // 2
probe = w[mid:mid + 64]
if len(probe) < 64 or probe in k:
    sys.stderr.write("ERROR: system kernel contains /bin/wm.elf bytes (delivery must be a volume file)\n")
    raise SystemExit(1)
print("    断言 OK：系统内核 %d B 里搜不到 /bin/wm.elf 的 64B 探针（偏移 %d）；合成器只从系统卷装载" % (len(k), mid))
PYEOF6

