VimtuOS ★ B5：GNU make 演示工程（3 个 C 文件 + Makefile）

用法（在 ring3 shell 里）：
    run /bin/make -C /make-demo all       # 编出 /make-demo/hello
    run /make-demo/hello                  # 跑它：vimtuos-make-demo: 6*7=42
    run /bin/make -C /make-demo clean     # 删掉产物

它演示：变量 + $(…) 展开 + $@、**隐式规则** %.o: %.c（dir.c 的 stat 补丁支撑）、
recipe 里的 `&&`（强制走 /bin/sh -c）。编译器是系统卷里的 /bin/tcc（TinyCC 闭环）。
