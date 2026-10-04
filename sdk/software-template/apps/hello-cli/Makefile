# sdk/software-template/apps/hello-cli/Makefile
#
# 纯命令行 VimtuOS 程序的模板构建：C + user/lib（自研最小 libc，自有无依赖 ABI int 0x80）。
# 编译/链接选项与仓库的 user/build_user.sh **同款**（每条理由见那个脚本的头部注释），
# 只多了两处模板自己的东西：
#   * -I sdk/software-template/include/vimtu （模板带的目录枚举包装 dir64.h/.c）
#   * 一次构建出**两个产物**：ELF64（走 `elfrun` / ELF64 装载器）与平铺 blob（走 VAP64 或 blob 装载器）
#
# 用法（在 MSYS2 bash 里；或用同目录的 build.sh 帮你设 PATH）：
#   bash sdk/software-template/apps/hello-cli/build.sh
#   bash sdk/software-template/apps/hello-cli/build.sh clean
#
# 产物：build64/sdk/hello-cli.elf（静态 ELF64，<= 64 KiB）
#       build64/sdk/hello-cli.bin（objcopy -O binary 的平铺 blob，<= 32768 B = 8 页）

# 本目录 -> 仓库根：sdk/software-template/apps/hello-cli 共四层
ROOT    := ../../../..
BUILD   := $(ROOT)/build64/sdk
OBJ     := $(BUILD)/obj-hello-cli
LIB     := $(ROOT)/user/lib
SDKINC  := $(ROOT)/sdk/software-template/include/vimtu
APP     := hello-cli
OUT_ELF := $(BUILD)/$(APP).elf
OUT_BIN := $(BUILD)/$(APP).bin

CC      := clang --target=x86_64-unknown-none-elf
LD      := ld.lld
OBJCOPY := objcopy.exe
# 只读我们自己的头（-nostdinc）：连 <stdint.h> 都用 user/lib 里那份，编译器的资源头一个都不碰。
# --target 已经在 $(CC) 里，不重复写（写两遍只是啰嗦）。
CFLAGS  := -nostdinc -I $(LIB) -I $(SDKINC) \
           -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
           -fno-zero-initialized-in-bss -mcmodel=large -mno-red-zone \
           -mno-sse -mno-sse2 -mno-mmx -mno-avx -fno-asynchronous-unwind-tables -fno-unwind-tables \
           -ffunction-sections -fdata-sections -std=c11 -O2 -Wall -Wextra
ASFLAGS := -nostdinc -ffreestanding -fno-pic -fno-pie \
           -ffunction-sections -fdata-sections -Wall -Wextra

LDFLAGS := -m elf_x86_64 -static --gc-sections -z noexecstack

# 运行时 + 应用源码（目录枚举包装是模板自己的那份）
CSRCS := $(LIB)/syscall.c $(LIB)/string.c $(LIB)/stdlib.c $(LIB)/stdio.c \
         $(SDKINC)/dir64.c $(CURDIR)/main.c
SSRCS := $(LIB)/crt0.S $(LIB)/syscall.S
# ★ 目标名带**源扩展名**（syscall.c.o / syscall.S.o）：user/lib 里 syscall.c 与 syscall.S 同名，
#   只用 basename 做目标名会把两份源码编到同一个 .o 上（实测就是 duplicate symbol）。
COBJS := $(patsubst %.c,$(OBJ)/%.c.o,$(notdir $(CSRCS)))
SOBJS := $(patsubst %.S,$(OBJ)/%.S.o,$(notdir $(SSRCS)))
OBJS  := $(COBJS) $(SOBJS)

.PHONY: all clean check
all: $(OUT_ELF) $(OUT_BIN)

# ---- 编译（每个 .c/.S 一条规则；同名文件不冲突：本模板的文件名都是唯一的）----
$(OBJ)/%.c.o: $(LIB)/%.c | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/%.c.o: $(SDKINC)/%.c | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/%.c.o: $(CURDIR)/%.c | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/%.S.o: $(LIB)/%.S | $(OBJ)
	$(CC) $(ASFLAGS) -c $< -o $@

$(OBJ):
	mkdir -p $(OBJ) $(BUILD)

# ---- 链接一：ELF64（evshm_demo.ld：ELF 头/程序头表落在第一个 PT_LOAD 内，内核才拿得到 AT_PHDR）----
$(OUT_ELF): $(OBJS) $(ROOT)/user/apps/evshm_demo.ld
	mkdir -p $(BUILD)
	$(LD) $(LDFLAGS) -T $(ROOT)/user/apps/evshm_demo.ld -o $@ $(OBJS)
	@echo "    $(APP).elf = $$(stat -c%s $@) B（静态 ELF64；elfrun /bin/$(APP).elf）"

# ---- 链接二：平铺 blob（user64.ld 的 .image_end 哨兵保证 .bss 零也算进长度）----
$(OUT_BIN): $(OBJS) $(LIB)/user64.ld
	mkdir -p $(BUILD)
	$(LD) $(LDFLAGS) -T $(LIB)/user64.ld -o $(BUILD)/$(APP).blob.elf $(OBJS)
	$(OBJCOPY) -O binary $(BUILD)/$(APP).blob.elf $@
	@echo "    $(APP).bin = $$(stat -c%s $@) B（blob 上限 32768 B = 8 页）"

# ---- 自检：PT_LOAD 必须落在 4GiB..4GiB+64KiB（内核 elf64.cpp 会拒绝越界），无 PT_INTERP/PT_DYNAMIC ----
check: all
	py -3 $(ROOT)/sdk/software-template/tools/elf64_check.py $(OUT_ELF) $(OUT_BIN)

clean:
	rm -rf $(OBJ) $(OUT_ELF) $(OUT_BIN) $(BUILD)/$(APP).blob.elf
