# `sdk/software-template/include/vimtu` —— 给第三方用的接口头

真源永远是仓库的 **`user/lib/*.h`**（以及内核侧 `kernel/syscall64.h`）；本目录只放**模板自己写的包装**，
免得你再抄一遍：

| 文件 | 内容 |
|---|---|
| `dir64.h` / `dir64.c` | Ring 3 **实时目录枚举**的自有 ABI 包装（`int 0x80` 50/51/52）：`vimtu_dir_open/read/close` + `struct VimtuDirent`。**自包含**（只依赖 `user/lib/vimtu64.h` 的 `__vint64` 原始桩与 `int 0x80`） |

```c
#include "dir64.h"                       /* -I sdk/software-template/include/vimtu */
struct VimtuDir* d = vimtu_dir_open("/", &err);        /* 必须是绝对路径 */
struct VimtuDirent e;
while (vimtu_dir_read(d, &e) == 1) {
    /* e.name / e.kind(VIMTU_DIR_KIND_FILE|DIR) / e.size / e.mtime */
}
vimtu_dir_close(d);                      /* 句柄是 fd 槽：上限 8，用完必须关 */
```

**如实边界**：相对路径要自己 `getcwd` 拼；句柄不关会 `-EMFILE`；权限由内核 vfs64 判（`-EACCES` 会打 `[PERM64] deny`）；
老卷/老内核的兜底是构建期快照 `/etc/vimtu.dirs`（**看不到本次开机新建的文件**）。
