# `sdk/software-template/apps/hello-gui` —— 示例②：图形程序（合成器 + shm surface）

* `main.c` —— 源码：fork+execve 拉起 `/lib/wm.elf`（Ring 3 合成器）→ `shm_create/shm_map` 画布 →
  `wl_surface_create/attach/damage/commit` + `wl_display_dispatch` → `user/lib/font64.h` 画字 →
  `wl_surface_destroy` 收尾（**完整流程 ~150 行，可以直接当模板抄**）
* `Makefile` / `build.sh` —— 构建（`font64.c` 是**唯一开 SSE** 的编译单元，见 Makefile 注释）
* `tests/hello_gui_test.py` —— 端到端验收（真 QEMU + 抓屏像素 + `[ICON64] … src=vfs` 外置图标证据）

```sh
bash sdk/software-template/apps/hello-gui/build.sh          # 产物 build64/sdk/hello-gui.elf
py -3 sdk/software-template/apps/hello-gui/tests/hello_gui_test.py
```

**如实边界**（源码注释里也写了）：

* `build64/sdk/hello-gui.bin`（平铺 blob）会**超过 8 页上限**（约 44.5 KB > 32768 B）——
  因为用户态字体栈比 blob 装载器允许的体积大；**GUI 程序走 ELF64（`elfrun`）这条**，blob 那条只作留档参考；
* 不处理 seat 输入事件、不做多缓冲 ping-pong（模板只演示"画 + 提交上屏"）；
* 单 surface ≤ 16384 px、单 shm ≤ 64 KiB（内核上限）。
