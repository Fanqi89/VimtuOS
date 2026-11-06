# Wayland 基础抽象层（v0.9.12）

> 这份文档只回答一个问题：**跑 wlroots 需要的内核态抽象，本批做到了哪一步、还差什么。**
> 本批交付的是"内核门面 + 一个 ring3 验收程序"，**不含** libdrm / libinput / wlroots / libwayland /
> EGL / GBM —— 那些是下一批的用户态兼容层。

四条内核态抽象（Wayland 生态真正依赖的东西）逐条对照：

| # | 抽象 | 本批做了什么 | 现在能到什么程度 | 还差什么才能跑 wlroots |
|---|---|---|---|---|
| 1 | **DRM/KMS**（显示模式 / 帧缓冲 / 翻页 / vblank） | `kernel/drm64.h` + `kernel/drm64.cpp`：`/dev/dri/card0` 设备特判；`ioctl(16)` 认 11 个命令（VERSION / GET_CAP / GETRESOURCES / GETCRTC / GETCONNECTOR / CREATE·MAP·DESTROY_DUMB / PAGE_FLIP / WAIT_VBLANK / CRTC_GET_SEQUENCE）；dumb buffer = 内核页 + 挂进调用方页表；page_flip = 拷进后备缓冲 + GPU 传输或 CPU 写 LFB | ring3 能**自己枚举模式、自己画、自己翻上屏、自己等 vblank**；一条 CRTC、一个 connector（`HDMI-A-1`，always connected）、最多 4 块 dumb buffer | ① **多 CRTC / 多 connector / plane / 属性（properties）** 与 `drmModeSetCrtc` 级 API；② dumb buffer 的**真正 mmap 语义**（现在的 `MAP_DUMB.offset` 是用户 VA，不是文件偏移）；③ **原子提交（atomic modeset）** 与 page-flip 的**事件回投**（现在 `PAGE_FLIP` 是同步的、不回 `drm_event`）；④ GBM 要的 `gpu`/`render` 节点与 dmabuf（目前只有 card0，没有 DMA-BUF 导入导出） |
| 2 | **evdev**（原始输入事件） | `/dev/input/event0`（键盘）/`event1`（鼠标）：`read` 出 24 B `struct input_event`（与 Linux x86_64 同尺寸）；`EVIOCGNAME`/`EVIOCGVERSION`/`EVIOCGBIT(0)` | ring3 能 open + 读**原始事件**；键码口径 = 内核既有的键码表（可打印 ASCII / 控制字符 / `NAV_*`），鼠标移动是 `EV_REL`（REL_X/REL_Y）、按键与滚轮也走 `EV_KEY`/`EV_REL` | ① **Linux 键码表**（`KEY_A`.. 与 `BTN_LEFT=0x110`）：现在是"内核键码"，libinput 会认不出来；② `EVIOCGBIT(EV_KEY, len)` 的**逐键位图**（现在只给事件类型位图）、`EVIOCGABS`/`EVIOCGID`/`EVIOCGPHYS`；③ `EV_SYN/SYN_REPORT` 的**分帧语义**（现在每条事件独立返回，没有 report 边界）；④ 绝对坐标设备（触摸板/触摸屏）与 `libinput` 需要的 udev 元数据 |
| 3 | **缓冲共享与同步** | 既有的 `shm_create(13)/shm_map(14)`（每进程一份共享对象、页帧不属于任何进程）+ 本批的 dumb buffer（内核页挂进调用方页表）。`drmdemo` 走的就是"建 → 映射 → 直接写像素 → 翻上屏" | ring3 ↔ 内核共享一块**CPU 可写**的像素缓冲；`wl64`（A5 骨架）与 `wl_shm` 风格的东西已经在用同一套 shm 对象 | ① **DMA-BUF / dma-buf fd 传递**（GPU 侧共享，libwayland 的 `zwp_linux_dmabuf_v1` 必需）；② **显式同步**（`dma_fence`/`sync_file`/`EGL_KHR_fence_sync`）：现在只有 `WAIT_VBLANK` 一种时间点；③ 缓冲的**生命周期与所有权**（dumb buffer 目前与创建它的槽位绑定，进程异常退出不会自动回收 —— 有界 4 槽，见 `kernel/drm64.h`） |
| 4 | **事件通知**（fd 就绪/可 poll） | 既有的 `poll(7)/epoll(61 系列)` 在 FD 层（`fd64_ready64`）；evdev 的 `read` 在队列空时返回 `-EAGAIN`（非阻塞语义） | 事件循环的**最基础两种形态**都有：① `read` 返回 `-EAGAIN` 的循环；② `poll/epoll` 等 fd 就绪 | ① **DRM fd 的可 poll 性**（page-flip 完成/ vblank 事件应当是 `poll` 可等的 **fd 事件**，现在 `WAIT_VBLANK` 是**同步阻塞**的 ioctl）；② `eventfd`/`signalfd`/`timerfd`（wl_event_loop 的重活靠它们）；③ `EPOLLET`/`EPOLLONESHOT` 等边沿语义（现在只有水平触发） |

## CPU + GPU 混合渲染的分工（本批的实现口径）

一句话：**CPU 做光栅化，GPU 只做传输与扫描输出。**

```
ring3 客户端
  │  ① ioctl CREATE_DUMB / MAP_DUMB
  │  ② 直接往 MAP_DUMB 给的 VA 里写像素（纯 CPU；没有任何 GPU 参与）
  ▼
dumb buffer（内核 page_alloc 的物理页，同一批页也挂在调用方页表里）
  │  ③ ioctl PAGE_FLIP(handle)
  ▼
kernel/drm64.cpp  d64_present_dumb64()
  │  ④ 逐行拷进**内核后备缓冲**（fb_surface64()，既有的绘制目标）
  ├─ ⑤a  vgpu64_blit64(0,0,w,h) 成功  → path=gpu
  │        （virtio-gpu 2D：TRANSFER_TO_HOST_2D 把后备缓冲搬进 device resource，
  │          RESOURCE_FLUSH 让设备把该区域刷到扫描输出）
  └─ ⑤b  它返回 0（无设备 / 运行期降级 / 传输超时）→ fb_soft_flip_region64(0,0,w,h) → path=cpu
           （CPU 逐行写 LFB；缩放模式走整帧放大路径）
```

* 这两条都是**既有函数**：本批没有写一行新的"上屏"代码，也没有碰任何厂商 GPU 寄存器
  （Intel/AMD/NVIDIA 的路径仍然是"只读探测"，见 `docs/开发历程.md` 的 M52/M54/M55）。
* `path=gpu|cpu` 不是猜的：它就是 `vgpu64_blit64()` 的返回值。
* vblank：`WAIT_VBLANK` 复用 `fb.cpp` 的 VGA 回扫探针（`0x3DA` bit3），不可信时按 16 ms 软兜底并
  如实打印 `mode=soft`；序列号在每次"等到/兜底"时 +1（单调递增，验收脚本按它判）。
* 为什么**不**把 dumb buffer 直接交给设备扫描输出（真 KMS 的做法）：virtio-gpu 的 scanout resource 在
  `vgpu64_init64()` 里一次性绑到后备缓冲的物理页上（`RESOURCE_ATTACH_BACKING`），换 resource 需要重配
  + 重新 `SET_SCANOUT`，那是"交换链/翻页"那一层的事（既有 `fb_swap_*` / `vgpu64_present64` 已经在做）。
  本批的 dumb buffer 是**客户端自己的缓冲**，所以走"拷进后备缓冲 → 传输"这条最不容易踩坏既有路径的路。

## 下一批（用户态兼容层）的计划与各自的前置

按依赖顺序，**前一项不做完，后一项就只能打桩**：

1. **libdrm 子集**（`drmModeGetResources/GetConnector/GetCrtc`、`drmModeCreateDumbBuffer`、
   `drmModeMapDumbBuffer`、`drmModePageFlip`、`drmWaitVBlank`）
   前置：本批的 ioctl 已经够用；真正的工作是 ① 把 `MAP_DUMB.offset` 接到用户态 `mmap`（需要"设备 mmap"
   或一个 `drm_gem` fd 语义）；② ioctl 号与结构体做成与真实 `drm.h` **逐字节一致**（本批明确说了不兼容）；
   ③ `GETRESOURCES` 的两次调用（先问数量、再填）路径与 `count` 语义按 libdrm 的习惯对齐。
2. **libinput 子集**（`libinput_udev_create_context` 太重，先做 `libinput_path_create_context`）
   前置：① evdev 的**键码表换成 Linux `KEY_*` 值**；② `EVIOCGBIT(EV_KEY)` 逐键位图 + `EVIOCGID`；
   ③ `SYN_REPORT` 分帧；④ `poll()` 对 evdev fd 的可等性（`read` 空队列 → `-EAGAIN` 已经有）。
3. **libwayland（wire 协议）**
   前置：① `SCM_RIGHTS` 传 fd（Unix socket 的 fd 传递）—— 现在 `sock64/unix` 只传字节；② `eventfd`
   与 `poll` 的边沿语义；③ `wl_shm` 需要 `shm_open`/`ftruncate`/`mmap` 三件事（本内核有 shm 对象，
   但没有 `mmap` 文件的语义）。
4. **wlroots**（合成器本体）
   前置：① 上一批已经有的 `wl_surface`/`commit`/`seat`（`kernel/wl64.cpp`，A5）与本批的 DRM/evdev 门面
   **接线**：合成器要 `drmModePageFlip` + `drmWaitVBlank` + `poll(evdev)` 三件事同时成立；
   ② **EGL/GLES 或至少 pixman 软件渲染**（wlroots 允许纯软件后端，所以先做 pixman）；③ GBM（要 DMA-BUF）
   只有在"要 GPU 直接渲染"时才成为硬前置。
5. **EGL / GBM**
   前置：DMA-BUF + `drm` render 节点 + 一个能分配 GPU 可用缓冲的分配器。本内核目前**没有**
   GPU 显存分配器（Intel 那边只读探测 GTT，AMD/NVIDIA 明确 `no-touch`），所以这一项排最后。

## 本批如实保留的边界（别把没做的说成做了）

* **一次只有一个 CRTC**、一个 connector（`HDMI-A-1`，恒 `connected`）：`GETRESOURCES` 里
  `count_crtcs=1`；没有 plane 枚举（`DRM64_MAX_PLANE64=2` 只是上限常量，ioctl 里没有 plane 命令）。
* **没有属性（properties）**：`count_props=0`、`count_encoders=1`。
* **`PAGE_FLIP` 是同步的**：ioctl 返回时已经等过一次 vblank，**不回** `drm_event`；`user_data` 只进日志。
* **dumb buffer 与槽位绑定**：进程异常退出且没 `DESTROY_DUMB`，那块页会一直占着（有界：4 槽 × ≤8 MiB）。
  真 KMS 的 fd 生命周期管理（close(fd) 自动回收 GEM handle）是下一批。
* **没有文件 mmap**：`MAP_DUMB.offset` 是本内核的**用户 VA**，不是 Linux 的文件偏移。
* **尺寸上限**：单块 ≤ 8 MiB、单行 ≤ 8192 B、高 ≤ 4096；屏幕几何由 `fb_phys_*` 如实给出，
  超出上限一律 `-EINVAL`（不静默裁剪）。
* **evdev 只有两台设备**（键盘/鼠标），键码是内核口径、鼠标按键用掩码（1/2/4）而不是 Linux 的
  `BTN_LEFT(0x110)`；没有触摸/摇杆/绝对坐标设备。

## 证据（怎么自己验一遍）

```
py -3 tests\drm64_test.py            # 串口断言 + 头文件逐值比对 + 屏上非黑比例
```

需要的最小打点（全部在 `kernel/drm64.h` 的文件头里有格式表）：

```
[DRM] init crtc=1 connectors=1 fbs=4 dumb_max=4 vblank=<hw|soft>
[DRM] selftest dumb=ok flip=ok vblank=<n> mode=<hw|soft> ok=1
[DRM] verify bars=8 ok=1
[DRM] present id=<n> path=gpu|cpu bytes=<n> seq=<n>
[DRM] map handle=<n> va=0x<hex> pages=<n> phys=0x<hex> u=1
[DRMDEMO] ... （ring3 验收程序的 14 行，见 user/apps/drmdemo.c 文件头）
```

---

## 附录 A（v0.9.13）：用户态兼容层的进度与"剩下多远"的精确差值

### A.1 已经有的

* `user/lib/libdrm_compat.h` —— **libdrm 风格的封装**（`libdrm_open` / `libdrm_mode_get_resources` /
  `libdrm_mode_get_connector` / `libdrm_mode_create_dumb` / `libdrm_mode_map_dumb` /
  `libdrm_mode_page_flip` / `libdrm_wait_vblank` / `libdrm_mode_destroy_dumb` / `libdrm_bad_cmd`），
  全部 `static inline` 跑在 `kernel/drm64.h` 那套 ioctl 上；**编译已通过**（被 `/bin/drmdemo` 的
  构建包含，产出 14,040 B 不变 = 没有把演示改坏）。
  定位写得很清楚：**今天叫"用法兼容"，不是"二进制兼容"** —— 真 libdrm 本体仍不能用它。
* `user/lib/drm64.{h,c}` —— 4 个薄封装（open / ioctl / evdev open / evdev read）。
* `kernel/drm64.cpp` 的 11 个 ioctl + evdev 门面（v0.9.12 已验收 32/32）。

### A.2 做到"真 libdrm 能编能跑"还差多少（逐个 struct 的实测差值）

我们的实现与 Linux `drm.h` 的差异**只在 4 处 struct 尺寸**（其余 7 个已经一致）。按
`DRM64_IOWR(nr,type) = (3<<30)|(sizeof(type)<<16)|('d'<<8)|nr` 编码：

| ioctl | 我们现在 | Linux `drm.h` | 差 | 要做的事 |
| --- | --- | --- | --- | --- |
| `DRM_IOCTL_VERSION` | sizeof=60 → `0xC03C6400` | sizeof=**64** → `0xC0406400` | **+4** | ★ 订正：上一版本附录写成 56，**错的**。Linux `struct drm_version` = `int major,minor,patch(12) + pad(4) + size_t name_len + char* name + size_t date_len + char* date + size_t desc_len + char* desc` = 12+4+8*6 = **64**。改法：去掉 `packed`、按这个顺序重排字段，`name/date/desc` 改成用户指针 + 长度（内核侧先 `user64_range_ok64` 再拷出） |
| `MODE_GETCRTC` | 108 → `0xC06C64A1` | **104** → `0xC06864A1` | -4 | `drm_mode_modeinfo` 的 `hskew/vscan` 位置与 `clock` 类型对齐 |
| `WAIT_VBLANK` | 40 → `0xC028643A` | **24** → `0xC018643A` | -16 | 改成 `u64 type; u64 sequence; union{u64 tval; struct{u32 sec,usec;} }`（去掉我们多出来的字段） |
| `CRTC_GET_SEQUENCE` | 40 → `0xC028643B` | **24** → `0xC018643B` | -16 | 改成 `u32 crtc_id; u32 active; u64 sequence; u64 ns` |
| `GET_CAP` / `GETRESOURCES` / `GETCONNECTOR` / `CREATE_DUMB` / `MAP_DUMB` / `DESTROY_DUMB` / `PAGE_FLIP` | 16 / 64 / 80 / 32 / 16 / 4 / 24 | **相同** | 0 | 只需核对字段名 |

> 一句话：**再有一批就能把"用法兼容"变成"二进制兼容"**，全部集中在 `kernel/drm64.h` +
> `user/lib/drm64.h` 两个文件与那 4 个 struct；改完 `tests/drm64_test.py` 的双头文件逐值比对
> 会立刻报出任何漂移（它本来就是干这个的）。

### A.3 还完全没做的（按"跑 wlroots"的依赖顺序）

1. **`MODE_ADDFB2` / `MODE_SETCRTC` / `MODE_ATOMIC` + `MODE_GETPLANE` 的属性系统** —— wlroots 的
   DRM 后端走 atomic commit；我们现在只有 `PAGE_FLIP` + dumb buffer，**没有 fb 对象与属性**。
2. **GBM + EGL/GLES**（Mesa）—— 没有它，wlroots 的渲染器只能用 pixman（软件），那就退化成
   "能用但没有 GPU 加速"；而 EGL 需要一整套 `eglGetPlatformDisplay`/`EGLImage`/`dma-buf` 导入。
3. **`dma-buf` 与 fence**：本批用"内核把同一批物理页挂进调用方页表"的等价实现（跨进程共享还没有），
   wlroots 的 buffer 交接需要**可跨进程传递**的 handle（`PRIME_HANDLE_TO_FD`）。
4. **libinput 精简适配**：evdev 门面已能用（`/dev/input/event0|1` + `EVIOCGBIT`），但 libinput 还要
   `udev`（设备枚举、`/sys`）、`EPOLLIN|EPOLLOUT` 语义（我们有 epoll）、以及 `libevdev` 那一层的
   `EVIOCGRAB`/`EVIOCSABS` 等 ioctl。
5. **`done` 语义**：`dumb buffer` 的 fd 生命周期（进程异常退出未 DESTROY 会占页）—— 需要在
   `proc64` 的进程回收里加一个 hook（本批未做，风险点在"动 proc64 会牵动全部进程测试"）。
6. **真双缓冲交替**：演示目前是"同一帧缓冲连续两次 flip + 两次 wait_vblank"，两块 dumb 交替还没做。

### A.4.1 ★ 下一批的完整动手清单（ABI 逐字节对齐）

四个 struct 必须**一次改完、一次构建**（改一半会让 `kernel/drm64.cpp` 编译不过）：

| 文件 | 改什么 |
| --- | --- |
| `kernel/drm64.h` | 四个 struct 重排 + 去 `packed`（`drm_version` 64）；`Drm64ModeMode` 68（`clock` 在前、`name[32]` 在后）；`Drm64ModeCrtc` 104；`Drm64WaitVblank` 24（`type/sequence/tval_sec/tval_usec`）；`Drm64CrtcSeq` 24（`crtc_id/active/sequence/sequence_ns`）；每个都加一条 `static_assert` |
| `kernel/drm64.cpp` | `d64_ioctl_version64()` 改成指针版（三个 `len`+`ptr` 对，先 `user64_range_ok64` 再拷）；`wait_vblank` 的 `request→type`、`flags` 去掉（改 `tval_sec/usec`）；`crtc_seq` 的 `flags→active`、`ns→sequence_ns`；`getconnector` 里写 `mode.name` 的代码不变（字段名没改） |
| `user/lib/drm64.h` | 与 `kernel/drm64.h` 逐值一致（双头文件比对会报漂移） |
| `user/apps/drmdemo.c` | `version` 段改传缓冲区 + 长度；`wv.request` → `wv.type = 1`（两处） |
| `user/lib/libdrm_compat.h` | `libdrm_wait_vblank()` 里 `wv.request` → `wv.type` |
| `tests/drm64_test.py` | 把硬编码的 11 个 ioctl 号改成 Linux 值（见下表），并加一条"与 Linux 参考值逐个相等"的断言 |

**Linux 参考值（`_IOWR('d',nr,size)` = `(3<<30)|(size<<16)|(0x64<<8)|nr`）：**

```
VERSION            0xC0406400   (64)
GET_CAP            0xC010640C   (16)
MODE_GETRESOURCES  0xC04064A0   (64)
MODE_GETCRTC       0xC06864A1   (104)
MODE_GETCONNECTOR  0xC05064A7   (80)
MODE_CREATE_DUMB   0xC02064B2   (32)
MODE_MAP_DUMB      0xC01064B3   (16)
MODE_DESTROY_DUMB  0xC00464B4   (4)
MODE_PAGE_FLIP     0xC01864B0   (24)
WAIT_VBLANK        0xC018643A   (24)
CRTC_GET_SEQUENCE  0xC018643B   (24)
```

改完后的验收：`drm64_test` 双头文件逐值比对 + Linux 参考值断言全绿，`/bin/drmdemo` 输出不变（
`version name=vimtu-drm`、`wait_vblank seq=N`）。

### A.4 建议的下一批顺序

1. A.2 的 4 个 struct 对齐（小、可测、解锁 libdrm 本体）；
2. `ADDFB2` + `SETCRTC` + `GETPLANE`（解锁 wlroots 的**非 atomic** 路径）；
3. `proc64` 的 fd 回收 hook（解 A.3.5）；
4. `dma-buf` 跨进程（解 A.3.3）；
5. atomic commit（解 A.3.1）→ 到这一步 wlroots 的 DRM 后端才真正有戏。
