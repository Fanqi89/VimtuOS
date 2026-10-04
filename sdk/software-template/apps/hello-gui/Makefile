# sdk/software-template/apps/hello-gui/Makefile
#
# 图形程序模板：/bin/wm（Ring 3 合成器）+ shm surface + user/lib/font64.h 画字。
# 选项与仓库里 fontdemo 那条线**同款**（build64.sh 的 EVSHM_* 段），差别只有两处：
#   * 本模板的源码目录不同（应用在 sdk/...，运行时仍用 user/lib）
#   * **font64.c 必须开 SSE**：stb_truetype 是浮点代码，x86_64 的 float 返回值走 xmm 寄存器，
#     -mno-sse 下 clang 直接报 "SSE register return with SSE disabled"；
#     ring3 入口已经开了 CR4.OSFXSR/OSXMMEXCPT（见 tools/musl_build_win.sh 的头部说明），可正常执行。
#
# 用法（Windows + MSYS2 bash）：
#   bash sdk/software-template/apps/hello-gui/build.sh
#   bash sdk/software-template/apps/hello-gui/build.sh clean
#
# 产物：build64/sdk/hello-gui.elf（静态 ELF64；由 `elfrun /bin/hello-gui.elf` 跑）

# 本目录 -> 仓库根：sdk/software-template/apps/hello-gui 共四层
ROOT    := ../../../..
BUILD   := $(ROOT)/build64/sdk
OBJ     := $(BUILD)/obj-hello-gui
LIB     := $(ROOT)/user/lib
SDKINC  := $(ROOT)/sdk/software-template/include/vimtu
APP     := hello-gui
OUT_ELF := $(BUILD)/$(APP).elf
OUT_BIN := $(BUILD)/$(APP).bin

CC      := clang --target=x86_64-unknown-none-elf
LD      := ld.lld
OBJCOPY := objcopy.exe

# 只读我们自己的头（-nostdinc）：连 <stdint.h> 都用 user/lib 里那份
CFLAGS  := -nostdinc -I $(LIB) -I $(SDKINC) \
           -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
           -mcmodel=large -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-avx \
           -fno-asynchronous-unwind-tables -fno-unwind-tables \
           -ffunction-sections -fdata-sections -std=c11 -O2 -Wall -Wextra

# 只给 font64.c（stb_truetype 浮点）用：开 SSE，并去掉上面那组 -mno-sse*
CFLAGS_SSE := $(filter-out -mno-sse -mno-sse2 -mno-mmx -mno-avx,$(CFLAGS)) -msse -msse2

ASFLAGS := -nostdinc -ffreestanding -fno-pic -fno-pie \
           -ffunction-sections -fdata-sections -Wall -Wextra

LDFLAGS := -m elf_x86_64 -static --gc-sections -z noexecstack
LDSCRIPT := $(ROOT)/user/apps/evshm_demo.ld

CSRCS := $(LIB)/syscall.c $(LIB)/string.c $(LIB)/stdlib.c $(LIB)/stdio.c $(LIB)/wl.c \
         $(CURDIR)/main.c
SSRCS := $(LIB)/crt0.S $(LIB)/syscall.S
# ★ 目标名带源扩展名：user/lib 里 syscall.c 与 syscall.S 同名，只用 basename 会撞车
COBJS := $(patsubst %.c,$(OBJ)/%.c.o,$(notdir $(CSRCS)))
SOBJS := $(patsubst %.S,$(OBJ)/%.S.o,$(notdir $(SSRCS)))
OBJS  := $(COBJS) $(SOBJS) $(OBJ)/font64.c.o

.PHONY: all clean check
all: $(OUT_ELF) $(OUT_BIN)

$(OBJ)/%.c.o: $(LIB)/%.c | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/%.c.o: $(CURDIR)/%.c | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/%.S.o: $(LIB)/%.S | $(OBJ)
	$(CC) $(ASFLAGS) -c $< -o $@

# 唯一一条开 SSE 的规则
$(OBJ)/font64.c.o: $(LIB)/font64.c | $(OBJ)
	$(CC) $(CFLAGS_SSE) -c $< -o $@

$(OBJ):
	mkdir -p $(OBJ) $(BUILD)

$(OUT_ELF): $(OBJS) $(LDSCRIPT)
	mkdir -p $(BUILD)
	$(LD) $(LDFLAGS) -T $(LDSCRIPT) -o $@ $(OBJS)
	@echo "    $(APP).elf = $$(stat -c%s $@) B（静态 ELF64 + 用户态字体栈；elfrun /bin/$(APP).elf）"

$(OUT_BIN): $(OBJS) $(LIB)/user64.ld
	mkdir -p $(BUILD)
	$(LD) $(LDFLAGS) -T $(LIB)/user64.ld -o $(BUILD)/$(APP).blob.elf $(OBJS)
	$(OBJCOPY) -O binary $(BUILD)/$(APP).blob.elf $@
	@echo "    $(APP).bin = $$(stat -c%s $@) B（blob 上限 32768 B = 8 页）"

check: all
	py -3 $(ROOT)/sdk/software-template/tools/elf64_check.py $(OUT_ELF)

clean:
	rm -rf $(OBJ) $(OUT_ELF) $(OUT_BIN) $(BUILD)/$(APP).blob.elf
