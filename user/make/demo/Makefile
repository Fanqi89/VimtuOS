# Makefile - ★ B5：**在 VimtuOS ring3 里用 GNU make 编一个 3 文件 C 工程**（演示 + 验收用）
#
#   run /bin/make -C /make-demo all     （或把这份工程解到别的目录再 -C 过去）
#   run /bin/make -C /make-demo clean
#
# 这份 Makefile 有意把本批的三条能力都用上：
#   ① 变量（CC/CFLAGS/OBJS/PROG）+ `$(…)` 展开 + `$@`/`$<` 自动变量；
#   ② **隐式规则**：main.o / util.o 没有显式规则，由 GNU make 内置的 `%.o: %.c` 推出
#      （它要求 make 能判断 main.c/util.c 存在 —— 这条走的是 dir.c 的目标平台补丁：stat 兜底）；
#   ③ recipe 里带 `&&`，**强制走 `/bin/sh -c '<recipe>'`**（本批给 ring3 shell 加的 -c 子集）。
#      不这么做的话 make 会走"直接 exec"的快路径，就看不到 sh -c 了。
#
# 注意（如实标注）：`make` 会把**隐式规则产出**的中间文件在结束时删掉（这是 GNU make 的标准行为，
#   不是缺陷）。这里用 `.SECONDARY` 留住 .o —— 否则 `make clean` 里那些 rm 会因为文件已不存在而失败。
CC      = /bin/tcc
CFLAGS  = -c
OBJS    = main.o util.o
PROG    = hello

.PHONY: all clean
.SECONDARY: $(OBJS)

all: $(PROG)
	@echo "make: all done -> $(PROG)"

$(PROG): $(OBJS)
	$(CC) $(OBJS) -o $@ && echo "make: linked $@ from $(OBJS)"

# main.o / util.o 故意不写规则：交给内置的隐式规则 %.o: %.c（= $(CC) $(CFLAGS) -c -o $@ $<）

clean:
	-rm $(PROG)
	-rm main.o
	-rm util.o
