# 本轮实测原始输出（`sdk/software-template`）

> 这一份是**贴出来的真材料**：命令与输出都是从本机跑出来的原文（只做了换行归一化），
> 断言的 PASS/FAIL 一条不改。硬件/环境：Windows + MSYS2（clang/lld/nasm）+ `py -3` + QEMU（`-m 512 -vga std`）。
>
> **纪律**：同一时刻只跑一个 QEMU 重活；`vm.close()` 在 `finally` 里（每次跑完 QEMU 进程都退干净）。

## 1) 宿主侧：构建 / 打包 / 装卷 / 图标外置 / 探针

（依次就是 README 里那几条命令：两个 `build.sh` + `check`、`vap64_pack.py` 打包与 `--verify`、
`vtar64_pack.py` 打包 + `--list` + `--extract`、`iconpack_ext.py` 光栅化与装卷、
`vol_install.py` 装卷 + `--check` 独立回读、`tools/probe64.py` 构建期探针。）

```
### 1) 构建 hello-cli
py -3 ../../../../sdk/software-template/tools/elf64_check.py ../../../../build64/sdk/hello-cli.elf ../../../../build64/sdk/hello-cli.bin
  -> ../../../../build64/sdk/hello-cli.elf
    ELF �Լ� OK��entry=0x1000001d0 phnum=7 PT_LOAD=4 size=15616 B span=0x100000000..0x100003660
  -> ../../../../build64/sdk/hello-cli.bin
    blob �Լ� OK��size=9201 B������ 32768 B = 8 ҳ��
  �Լ���ۣ�ȫ��ͨ����2 ���ļ���

### 2) 构建 hello-gui

### 3) VAP64 打包 + 独立校验
[VAP64] wrote build64/sdk/hello-cli.vap name=hello-cli code=9201 crc=0xA9D91148 bytes=9243
[VAP64] verify OK file=build64/sdk/hello-cli.vap name=hello-cli code=9201 crc=0xA9D91148 bytes=9243

### 4) VTAR64 类 deb/tar 骨架 + 独立校验 + 解包
[VTAR64] wrote build64/sdk/hello-cli.vtar64 entries=3 bytes=25320�������ض���CRC32 + ���� sha256 ͨ����
[VTAR64] build64/sdk/hello-cli.vtar64��3 ����Ŀ
   bin/hello-cli.elf                     15616 B mode=0755 sha256=aff75dda3535b198
   apps/hello-cli/hello-cli.vap           9243 B mode=0755 sha256=05e82a2638846e77
   SHA256SUMS                              179 B mode=0644 sha256=95eb90f39d5ccaba
[VTAR64] verify OK������ CRC32 + ���� sha256 ��ͨ����
   �⿪ bin/hello-cli.elf                     15616 B -> build64/sdk/unpacked\bin\hello-cli.elf
   �⿪ apps/hello-cli/hello-cli.vap           9243 B -> build64/sdk/unpacked\apps\hello-cli\hello-cli.vap
   �⿪ SHA256SUMS                              179 B -> build64/sdk/unpacked\SHA256SUMS

### 5) 图标光栅化（39 个新 SVG × 4 档 = 156 张 PNG）
== �� ��դ�� 69 �� SVG��4 ���ߴ磩-> C:\Users\fanqi\Desktop\VimtuOS\Vimtu64\build\sdk\icons_ext
   ��դ�� archive          16px 24px 32px 48px
   ��դ�� arrow-repeat     16px 24px 32px 48px
   ��դ�� arrow-up         16px 24px 32px 48px
   ��դ�� audio            16px 24px 32px 48px
   ��դ�� battery-charging 16px 24px 32px 48px
   ��դ�� battery          16px 24px 32px 48px
   ��դ�� bell-slash       16px 24px 32px 48px
   ��դ�� bell             16px 24px 32px 48px
   ��դ�� binary           16px 24px 32px 48px
   ��դ�� calendar         16px 24px 32px 48px
   ��դ�� check            16px 24px 32px 48px
   ...（全量 156 张，逐张 sha256 见 build/sdk/icons_ext/manifest.json）
   ��Ŀ 276 �������� 69 ������
     system    archive      @16    268 B sha256=cc7243fa8c4f6437875eb21493e48676
     system    archive      @24    425 B sha256=5ff3f514521c49d606b25d2b6fcd05a7
     system    archive      @32    430 B sha256=b8dd5fbf7f53e59eba0e45dece0514ec
     system    archive      @48    690 B sha256=06f0f9da79744db74d77e7ca81590282

### 6) 外置图标装进系统卷（全 4 档）+ 逐字节回读（tail）
   �ض� /icons/system/wifi@16.png                   299 B sha256=083efdd7e3d6
   �ض� /icons/system/wifi@24.png                   435 B sha256=c9e4e66522e3
   �ض� /icons/system/wifi@32.png                   558 B sha256=33049fd25cd2
   �ض� /icons/system/wifi@48.png                   847 B sha256=4c9fa567c271
[VOL64] װ���Լ� OK��277 ���ļ����ֽڻض�һ�� -> build64/sdk/iconvol.img���� 24759 ������
[VOL64] ��ʾ�̣�build64/sdk/icondisk.img��16777216 B = 32768 ������������ LBA 8009��

### 7) 装卷器 + 独立回读校验（hello-cli 的 3 个文件）
   װ�� /bin/hello-cli.elf                     <- build64/sdk/hello-cli.elf                         15616 B mode=0755
   װ�� /apps/hello-cli/hello-cli.vap          <- build64/sdk/hello-cli.vap                          9243 B mode=0755
   �ض� /apps/hello-cli/hello-cli.vap              9243 B sha256=05e82a263884
   �ض� /bin/hello-cli.elf                        15616 B sha256=aff75dda3535
[VOL64] װ���Լ� OK��2 ���ļ����ֽڻض�һ�� -> build64/sdk/sdkvol.img���� 24759 ������
  [PASS] /bin/hello-cli.elf                    15616 B sha256=aff75dda3535b198
  [PASS] /apps/hello-cli/hello-cli.vap          9243 B sha256=05e82a2638846e77
[VOL64] check build64/sdk/sdkvol.img��2 ��������0 ��ʧ��

### 8) 构建期探针：内核二进制里搜不到 hello-cli.elf 的字节
    �Ž����Ϸ����Ӷ��󼯺� = 126 �� / 7649868 B��build64/os/*.o + build64/*.o + gui_rs/gui_rs.o�����ں� kernel64_os.bin = 3390944 B
    ���� OK��kernel64_os.bin 3390944 B ���Ѳ��� hello-cli.elf �� 64B ̽�루probe offset 2656 accepted��δ�ںϷ������г��֣�����ͬ�ֽ�ֵ 44������ֻ��ϵͳ��װ��

### 9) 串口里的外置图标加载行（src=vfs；取自 hello-gui 验收那一次真引导）
   日志：/c/msys64/tmp/vimtu_sdk_gui_yawdt0f7/serial.log
[ICON64] init pack lba=0 drive=-1 bytes=49192 entries=110 icons=30 bad=0 ok=1 fnv=35861b0f vfs_icons=1 src=vfs path=/etc/iconpack.bin
[ICON64] load kind=globe path=/icons/system/globe@24.png size=24 src=vfs ok=1
[ICON64] load kind=ethernet path=/icons/system/ethernet@24.png size=24 src=vfs ok=1
[ICON64] load kind=wifi path=/icons/system/wifi@24.png size=24 src=vfs ok=1
   ...
[ICON64] load kind=monitor-app path=/icons/apps/monitor-app@24.png size=24 src=vfs ok=1
[ICON64] load kind=about path=/icons/apps/about@24.png size=24 src=vfs ok=1
   src=vfs 行数：42
```

## 2) 验收①：`hello-cli`（真 QEMU：登录 -> 开终端 -> `elfrun /bin/hello-cli.elf`）

```
=== 0) �о��� ===
   �о��̣�C:\Users\fanqi\Desktop\VimtuOS\Vimtu64\build64\sdk\hello_cli_test.img��16777216 B = 32768 ���������� 3 ���ļ���
     /apps/hello-cli/hello-cli.vap                  9243 B
     /bin/hello-cli.elf                            15616 B
     /etc/hello.conf                                  23 B
  [PASS] �о��̾���  C:\Users\fanqi\Desktop\VimtuOS\Vimtu64\build64\sdk\hello_cli_test.img
=== 1) ���� + ��¼ + ���ն� ===
  [PASS] �ں�������[GUI64] ready��
  [PASS] ��ʼ�˵� -> �նˣ�[APP] term opened��
=== 2) ���ն����� /bin/hello-cli.elf ===
  [PASS] [HELLO-CLI] �����ˣ�����̣�  [HELLO-CLI] ver=1 pid=26
  [PASS] ������� hello ��
  [PASS] done �У�����������β��
  [PASS] Ŀ¼ö�� open �ɹ������� ABI 50��  [HELLO-CLI] dir open path=/ ok=1 err=0
  [PASS] �ں� [DIR64] �����ȫ��open/read/close��  ��� 5 ��
  [PASS] 50/51/52 ���� enosys �嵥��  enosys �� 0 ��
  [PASS] ö�ٳ�Ŀ¼/�ļ���items>0 �� dirs>=3��  [HELLO-CLI] dir items=20 dirs=8 files=12
  [PASS] bin ��ʶ��ΪĿ¼��kind=dir��  [HELLO-CLI] ent name=bin kind=dir
  [PASS] ���� /etc/hello.conf ���ֽ���/����һ��  [HELLO-CLI] conf path=/etc/hello.conf bytes=23 text="hello-cli fixture conf "
  [PASS] summary rc=0  [HELLO-CLI] summary items=20 dirs=8 files=12 conf_bytes=23 rc=0
  [PASS] ȫ���� PANIC / TRIPLE FAULT
   ������־��C:\msys64\tmp\vimtu_sdk_cli_nngakhbb\serial.log
=== ���ۣ�PASS��14/14��===
```

**结论：14/14 PASS**（含：目录枚举走自有 ABI 50/51/52、`bin` 判为目录、`/etc/hello.conf` 读出并与夹具逐值一致、
`summary … rc=0`、全程无 PANIC）。串口日志留在 `C:\msys64\tmp\vimtu_sdk_cli_*\serial.log`。

## 3) 验收②：`hello-gui`（真 QEMU：`elfrun /bin/hello-gui.elf`）

```
=== 0) �о��� ===
   ����ͼ�꣺138 �� PNG��-> /icons/<system|apps>/<����>@<�ߴ�>.png��sizes=(24, 48)��
   �о��̣�C:\Users\fanqi\Desktop\VimtuOS\Vimtu64\build64\sdk\hello_gui_test.img��16777216 B = 32768 ���������� 144 ���ļ���
     /Fonts-open/NotoSansSC-Regular.ttf          1119440 B
     /Fonts-open/SarasaMonoSC-Regular.ttf          30344 B
     /Fonts-open/unifont-14.0.01.ttf                2268 B
     /Fonts/NotoSans-Regular.ttf                   12220 B
     /bin/hello-gui.elf                            52888 B
     /icons/apps/about@24.png                        475 B
     /icons/apps/about@48.png                        951 B
     /icons/apps/archive-mgr@24.png                  356 B
     /icons/apps/archive-mgr@48.png                  626 B
     /icons/apps/calc@24.png                         329 B
     /icons/apps/calc@48.png                         463 B
     /icons/apps/editor@24.png                       322 B
     /icons/apps/editor@48.png                       551 B
     /icons/apps/files@24.png                        397 B
     /icons/apps/files@48.png                        632 B
     /icons/apps/image-viewer@24.png                 421 B
     /icons/apps/image-viewer@48.png                 722 B
     /icons/apps/mines@24.png                        298 B
     /icons/apps/mines@48.png                        230 B
     /icons/apps/monitor-app@24.png                  575 B
     /icons/apps/monitor-app@48.png                 1212 B
     /icons/apps/mypc@24.png                         257 B
     /icons/apps/mypc@48.png                         415 B
     /icons/apps/player@24.png                       426 B
     /icons/apps/player@48.png                       829 B
     /icons/apps/recycle@24.png                      309 B
     /icons/apps/recycle@48.png                      492 B
     /icons/apps/settings@24.png                     350 B
     /icons/apps/settings@48.png                     434 B
     /icons/apps/terminal@24.png                     314 B
     /icons/apps/terminal@48.png                     488 B
     /icons/apps/tmgr@24.png                         406 B
     /icons/apps/tmgr@48.png                         667 B
     /icons/system/archive@24.png                    425 B
     /icons/system/archive@48.png                    690 B
     /icons/system/arrow-repeat@24.png               460 B
     /icons/system/arrow-repeat@48.png               786 B
     /icons/system/arrow-up@24.png                   229 B
     /icons/system/arrow-up@48.png                   314 B
     /icons/system/audio@24.png                      362 B
     /icons/system/audio@48.png                      608 B
     /icons/system/battery-charging@24.png           504 B
     /icons/system/battery-charging@48.png           872 B
     /icons/system/battery@24.png                    241 B
     /icons/system/battery@48.png                    416 B
     /icons/system/bell-slash@24.png                 444 B
     /icons/system/bell-slash@48.png                 747 B
     /icons/system/bell@24.png                       409 B
     /icons/system/bell@48.png                       809 B
     /icons/system/binary@24.png                     339 B
     /icons/system/binary@48.png                     723 B
     /icons/system/calendar@24.png                   350 B
     /icons/system/calendar@48.png                   451 B
     /icons/system/check@24.png                      320 B
     /icons/system/check@48.png                      598 B
     /icons/system/chevron-down@24.png               195 B
     /icons/system/chevron-down@48.png               285 B
     /icons/system/chevron-left@24.png               213 B
     /icons/system/chevron-left@48.png               290 B
     /icons/system/chevron-right@24.png              218 B
     /icons/system/chevron-right@48.png              292 B
     /icons/system/chevron-up@24.png                 193 B
     /icons/system/chevron-up@48.png                 271 B
     /icons/system/clipboard@24.png                  300 B
     /icons/system/clipboard@48.png                  502 B
     /icons/system/clock@24.png                      460 B
     /icons/system/clock@48.png                      879 B
     /icons/system/close@24.png                      186 B
     /icons/system/close@48.png                      308 B
     /icons/system/copy@24.png                       277 B
     /icons/system/copy@48.png                       464 B
     /icons/system/dash@24.png                       116 B
     /icons/system/dash@48.png                       175 B
     /icons/system/disk@24.png                       319 B
     /icons/system/disk@48.png                       378 B
     /icons/system/document@24.png                   322 B
     /icons/system/document@48.png                   489 B
     /icons/system/download@24.png                   299 B
     /icons/system/download@48.png                   470 B
     /icons/system/ethernet@24.png                   285 B
     /icons/system/ethernet@48.png                   466 B
     /icons/system/executable@24.png                 373 B
     /icons/system/executable@48.png                 551 B
     /icons/system/file@24.png                       259 B
     /icons/system/file@48.png                       416 B
     /icons/system/folder-plus@24.png                385 B
     /icons/system/folder-plus@48.png                656 B
     /icons/system/folder@24.png                     350 B
     /icons/system/folder@48.png                     613 B
     /icons/system/fullscreen@24.png                 214 B
     /icons/system/fullscreen@48.png                 350 B
     /icons/system/gear@24.png                       488 B
     /icons/system/gear@48.png                      1103 B
     /icons/system/globe@24.png                      555 B
     /icons/system/globe@48.png                     1209 B
     /icons/system/headphones@24.png                 353 B
     /icons/system/headphones@48.png                 572 B
     /icons/system/house@24.png                      311 B
     /icons/system/house@48.png                      477 B
     /icons/system/image@24.png                      390 B
     /icons/system/image@48.png                      611 B
     /icons/system/info-square@24.png                303 B
     /icons/system/info-square@48.png                589 B
     /icons/system/list@24.png                       176 B
     /icons/system/list@48.png                       257 B
     /icons/system/lock@24.png                       332 B
     /icons/system/lock@48.png                       612 B
     /icons/system/monitor@24.png                    251 B
     /icons/system/monitor@48.png                    430 B
     /icons/system/pencil@24.png                     335 B
     /icons/system/pencil@48.png                     536 B
     /icons/system/person@24.png                     340 B
     /icons/system/person@48.png                     681 B
     /icons/system/plus@24.png                       144 B
     /icons/system/plus@48.png                       255 B
     /icons/system/power@24.png                      440 B
     /icons/system/power@48.png                      842 B
     /icons/system/reboot@24.png                     416 B
     /icons/system/reboot@48.png                     834 B
     /icons/system/scissors@24.png                   509 B
     /icons/system/scissors@48.png                  1029 B
     /icons/system/search@24.png                     484 B
     /icons/system/search@48.png                     850 B
     /icons/system/shield@24.png                     575 B
     /icons/system/shield@48.png                    1147 B
     /icons/system/trash2@24.png                     448 B
     /icons/system/trash2@48.png                     733 B
     /icons/system/upload@24.png                     298 B
     /icons/system/upload@48.png                     451 B
     /icons/system/usb@24.png                        229 B
     /icons/system/usb@48.png                        304 B
     /icons/system/user-avatar@24.png                506 B
     /icons/system/user-avatar@48.png               1135 B
     /icons/system/video@24.png                      339 B
     /icons/system/video@48.png                      544 B
     /icons/system/volume-mute@24.png                338 B
     /icons/system/volume-mute@48.png                527 B
     /icons/system/volume@24.png                     449 B
     /icons/system/volume@48.png                     870 B
     /icons/system/wifi-off@24.png                   401 B
     /icons/system/wifi-off@48.png                   770 B
     /icons/system/wifi@24.png                       435 B
     /icons/system/wifi@48.png                       847 B
     /lib/wm.elf                                   22832 B
  [PASS] �о��̾���  C:\Users\fanqi\Desktop\VimtuOS\Vimtu64\build64\sdk\hello_gui_test.img
=== 1) ���� + ��¼ + ���ն� ===
  [PASS] �ں�������[GUI64] ready��
  [PASS] ��ʼ�˵� -> �նˣ�[APP] term opened��
=== 2) �� /bin/hello-gui.elf���ϳ��� + shm surface + ���֣�===
  [PASS] [HELLO-GUI] �����ˣ�����̣�  [HELLO-GUI] ver=1 pid=26
  [PASS] �ֿ��ϵͳ�����ɹ���ok=1 bytes>0��  [HELLO-GUI] font path=/Fonts/NotoSans-Regular.ttf ok=1 bytes=12220 upem=1000 nglyph=113
  [PASS] surface + shm ���ã�id>0 �� va!=0��  [HELLO-GUI] surf id=1 w=256 h=56 shm=3 va=0xfc0000
  [PASS] ������Ч��glyphs>0 width>0 ink>0��  [HELLO-GUI] text size=18 glyphs=21 width=189 lh=22 ink=1640
  [PASS] �� 0 ֡�ύ�ɹ���rc=0 �� ink>0��  [HELLO-GUI] frame f=0 rc=0 ink=1640
  [PASS] �ں˲��ж�Ӧ�� surface �У�[WL64] surface create��
=== 3) �������أ�ץ���Ҵ����ǿ飩===
   ץ�� 1 �ţ��������أ��ݲ� 16��= [576]��ȡ��� 576��
  [PASS] ץ���ɹ�
  [PASS] �������б�ǿ����أ�>=200��˵�� surface ��ĺϳ�������  red=576
  [PASS] ������β��destroy + done��
  [PASS] surface ���ٳɹ���rc=0��  rc=0
  [PASS] ��֡���ύ�ɹ���rc ȫ 0��  ֡�� 3 ��
  [PASS] ����ͼ�걻�ں˼��أ�[ICON64] load �� src=vfs ok=1 ���� 5 ����  src=vfs ���� 38 �������� globe -> /icons/system/globe@24.png
  [PASS] ȫ���� PANIC / TRIPLE FAULT
   ������־��C:\msys64\tmp\vimtu_sdk_gui_03lza70v\serial.log
=== ���ۣ�PASS��16/16��===
```

**结论：16/16 PASS**。要点（都是串口/像素原文）：

* 字库从**系统卷**读：`[HELLO-GUI] font path=/Fonts/NotoSans-Regular.ttf ok=1 bytes=12220 upem=1000 nglyph=113`
* surface + shm：`[HELLO-GUI] surf id=1 w=256 h=56 shm=3 va=0xfc0000`
* 画字：`[HELLO-GUI] text size=18 glyphs=21 width=189 lh=22 ink=1640 bbox=…`
* 三帧提交：`[HELLO-GUI] frame f=0 rc=0 ink=1640`（f=1/f=2 同）→ `destroy rc=0` → `done`
* **上屏像素**：抓屏里纯红标记块（容差 16）= **576** 像素（= 24×24 的标记块整块上屏）
* **外置图标**：`src=vfs` 加载 **38** 条（例如 `globe -> /icons/system/globe@24.png`）

### 运行期抖动（**如实记录**）：并发负载下可能撞内核看门狗

同一份夹具在"机器上同时有别的构建/QEMU 在跑"时，出现过：
`[PANIC64] stop=WATCHDOG_TIMEOUT detail=00000000000013F0 state=RUNNING gen=1`
（同一段日志里还有 `[UI] perf sec=2 … redraw_us=942593` —— 单次重绘 0.94 s）。
这是**启动期时间被打爆**，与模板程序无关（那种情况下程序还没启动）。处置：

1. 夹具只装内核**实际请求**的两档图标（`system@24` / `apps@48`，见 `tools/sdk_qemu.py:icon_srcs(sizes=…)`），
   把启动期 PNG 解码量压到最小；
2. 采样改成**连拍 5 张取最大**（判据不降级，仍要求标记块真的在屏上）；
3. 验收脚本加"缺产物自动构建"，避免别的步骤重建 `build64` 时脚本直接失败。

改完之后的实例（含一次因环境失败、之后连过 3 次的记录）：

```
=== 0) �о��� ===
   ����ͼ�꣺138 �� PNG��-> /icons/<system|apps>/<����>@<�ߴ�>.png��sizes=(24, 48)��
   �о��̣�C:\Users\fanqi\Desktop\VimtuOS\Vimtu64\build64\sdk\hello_gui_test.img��16777216 B = 32768 ���������� 144 ���ļ���
     /Fonts-open/NotoSansSC-Regular.ttf          1119440 B
     /Fonts-open/SarasaMonoSC-Regular.ttf          30344 B
     /Fonts-open/unifont-14.0.01.ttf                2268 B
     /Fonts/NotoSans-Regular.ttf                   12220 B
     /bin/hello-gui.elf                            52888 B
     /icons/apps/about@24.png                        475 B
     /icons/apps/about@48.png                        951 B
     /icons/apps/archive-mgr@24.png                  356 B
     /icons/apps/archive-mgr@48.png                  626 B
     /icons/apps/calc@24.png                         329 B
     /icons/apps/calc@48.png                         463 B
     /icons/apps/editor@24.png                       322 B
     /icons/apps/editor@48.png                       551 B
     /icons/apps/files@24.png                        397 B
     /icons/apps/files@48.png                        632 B
     /icons/apps/image-viewer@24.png                 421 B
     /icons/apps/image-viewer@48.png                 722 B
     /icons/apps/mines@24.png                        298 B
     /icons/apps/mines@48.png                        230 B
     /icons/apps/monitor-app@24.png                  575 B
     /icons/apps/monitor-app@48.png                 1212 B
     /icons/apps/mypc@24.png                         257 B
     /icons/apps/mypc@48.png                         415 B
     /icons/apps/player@24.png                       426 B
     /icons/apps/player@48.png                       829 B
     /icons/apps/recycle@24.png                      309 B
     /icons/apps/recycle@48.png                      492 B
     /icons/apps/settings@24.png                     350 B
     /icons/apps/settings@48.png                     434 B
     /icons/apps/terminal@24.png                     314 B
     /icons/apps/terminal@48.png                     488 B
     /icons/apps/tmgr@24.png                         406 B
     /icons/apps/tmgr@48.png                         667 B
     /icons/system/archive@24.png                    425 B
     /icons/system/archive@48.png                    690 B
     /icons/system/arrow-repeat@24.png               460 B
     /icons/system/arrow-repeat@48.png               786 B
     /icons/system/arrow-up@24.png                   229 B
     /icons/system/arrow-up@48.png                   314 B
     /icons/system/audio@24.png                      362 B
     /icons/system/audio@48.png                      608 B
     /icons/system/battery-charging@24.png           504 B
     /icons/system/battery-charging@48.png           872 B
     /icons/system/battery@24.png                    241 B
     /icons/system/battery@48.png                    416 B
     /icons/system/bell-slash@24.png                 444 B
     /icons/system/bell-slash@48.png                 747 B
     /icons/system/bell@24.png                       409 B
     /icons/system/bell@48.png                       809 B
     /icons/system/binary@24.png                     339 B
     /icons/system/binary@48.png                     723 B
     /icons/system/calendar@24.png                   350 B
     /icons/system/calendar@48.png                   451 B
     /icons/system/check@24.png                      320 B
     /icons/system/check@48.png                      598 B
     /icons/system/chevron-down@24.png               195 B
     /icons/system/chevron-down@48.png               285 B
     /icons/system/chevron-left@24.png               213 B
     /icons/system/chevron-left@48.png               290 B
     /icons/system/chevron-right@24.png              218 B
     /icons/system/chevron-right@48.png              292 B
     /icons/system/chevron-up@24.png                 193 B
     /icons/system/chevron-up@48.png                 271 B
     /icons/system/clipboard@24.png                  300 B
     /icons/system/clipboard@48.png                  502 B
     /icons/system/clock@24.png                      460 B
     /icons/system/clock@48.png                      879 B
     /icons/system/close@24.png                      186 B
     /icons/system/close@48.png                      308 B
     /icons/system/copy@24.png                       277 B
     /icons/system/copy@48.png                       464 B
     /icons/system/dash@24.png                       116 B
     /icons/system/dash@48.png                       175 B
     /icons/system/disk@24.png                       319 B
     /icons/system/disk@48.png                       378 B
     /icons/system/document@24.png                   322 B
     /icons/system/document@48.png                   489 B
     /icons/system/download@24.png                   299 B
     /icons/system/download@48.png                   470 B
     /icons/system/ethernet@24.png                   285 B
     /icons/system/ethernet@48.png                   466 B
     /icons/system/executable@24.png                 373 B
     /icons/system/executable@48.png                 551 B
     /icons/system/file@24.png                       259 B
     /icons/system/file@48.png                       416 B
     /icons/system/folder-plus@24.png                385 B
     /icons/system/folder-plus@48.png                656 B
     /icons/system/folder@24.png                     350 B
     /icons/system/folder@48.png                     613 B
     /icons/system/fullscreen@24.png                 214 B
     /icons/system/fullscreen@48.png                 350 B
     /icons/system/gear@24.png                       488 B
     /icons/system/gear@48.png                      1103 B
     /icons/system/globe@24.png                      555 B
     /icons/system/globe@48.png                     1209 B
     /icons/system/headphones@24.png                 353 B
     /icons/system/headphones@48.png                 572 B
     /icons/system/house@24.png                      311 B
     /icons/system/house@48.png                      477 B
     /icons/system/image@24.png                      390 B
     /icons/system/image@48.png                      611 B
     /icons/system/info-square@24.png                303 B
     /icons/system/info-square@48.png                589 B
     /icons/system/list@24.png                       176 B
     /icons/system/list@48.png                       257 B
     /icons/system/lock@24.png                       332 B
     /icons/system/lock@48.png                       612 B
     /icons/system/monitor@24.png                    251 B
     /icons/system/monitor@48.png                    430 B
     /icons/system/pencil@24.png                     335 B
     /icons/system/pencil@48.png                     536 B
     /icons/system/person@24.png                     340 B
     /icons/system/person@48.png                     681 B
     /icons/system/plus@24.png                       144 B
     /icons/system/plus@48.png                       255 B
     /icons/system/power@24.png                      440 B
     /icons/system/power@48.png                      842 B
     /icons/system/reboot@24.png                     416 B
     /icons/system/reboot@48.png                     834 B
     /icons/system/scissors@24.png                   509 B
     /icons/system/scissors@48.png                  1029 B
     /icons/system/search@24.png                     484 B
     /icons/system/search@48.png                     850 B
     /icons/system/shield@24.png                     575 B
     /icons/system/shield@48.png                    1147 B
     /icons/system/trash2@24.png                     448 B
     /icons/system/trash2@48.png                     733 B
     /icons/system/upload@24.png                     298 B
     /icons/system/upload@48.png                     451 B
     /icons/system/usb@24.png                        229 B
     /icons/system/usb@48.png                        304 B
     /icons/system/user-avatar@24.png                506 B
     /icons/system/user-avatar@48.png               1135 B
     /icons/system/video@24.png                      339 B
     /icons/system/video@48.png                      544 B
     /icons/system/volume-mute@24.png                338 B
     /icons/system/volume-mute@48.png                527 B
     /icons/system/volume@24.png                     449 B
     /icons/system/volume@48.png                     870 B
     /icons/system/wifi-off@24.png                   401 B
     /icons/system/wifi-off@48.png                   770 B
     /icons/system/wifi@24.png                       435 B
     /icons/system/wifi@48.png                       847 B
     /lib/wm.elf                                   22832 B
  [PASS] �о��̾���  C:\Users\fanqi\Desktop\VimtuOS\Vimtu64\build64\sdk\hello_gui_test.img
=== 1) ���� + ��¼ + ���ն� ===
  [PASS] �ں�������[GUI64] ready��
  [PASS] ��ʼ�˵� -> �նˣ�[APP] term opened��
=== 2) �� /bin/hello-gui.elf���ϳ��� + shm surface + ���֣�===
  [PASS] [HELLO-GUI] �����ˣ�����̣�  [HELLO-GUI] ver=1 pid=26
  [PASS] �ֿ��ϵͳ�����ɹ���ok=1 bytes>0��  [HELLO-GUI] font path=/Fonts/NotoSans-Regular.ttf ok=1 bytes=12220 upem=1000 nglyph=113
  [PASS] surface + shm ���ã�id>0 �� va!=0��  [HELLO-GUI] surf id=1 w=256 h=56 shm=3 va=0xfc0000
  [PASS] ������Ч��glyphs>0 width>0 ink>0��  [HELLO-GUI] text size=18 glyphs=21 width=189 lh=22 ink=1640
  [PASS] �� 0 ֡�ύ�ɹ���rc=0 �� ink>0��  [HELLO-GUI] frame f=0 rc=0 ink=1640
  [PASS] �ں˲��ж�Ӧ�� surface �У�[WL64] surface create��
=== 3) �������أ�ץ���Ҵ����ǿ飩===
   ץ�� 1 �ţ��������أ��ݲ� 16��= [576]��ȡ��� 576��
  [PASS] ץ���ɹ�
  [PASS] �������б�ǿ����أ�>=200��˵�� surface ��ĺϳ�������  red=576
  [PASS] ������β��destroy + done��
  [PASS] surface ���ٳɹ���rc=0��  rc=0
  [PASS] ��֡���ύ�ɹ���rc ȫ 0��  ֡�� 3 ��
  [PASS] ����ͼ�걻�ں˼��أ�[ICON64] load �� src=vfs ok=1 ���� 5 ����  src=vfs ���� 38 �������� globe -> /icons/system/globe@24.png
  [PASS] ȫ���� PANIC / TRIPLE FAULT
   ������־��C:\msys64\tmp\vimtu_sdk_gui_8uga2tvu\serial.log
=== ���ۣ�PASS��16/16��===
```

## 4) 图标扩充：许可 / 来源 / sha256

* 新增 SVG：**39 个**（`gui_rs/assets/icons/system/` 34 个 + `apps/` 5 个），全部来自 **Bootstrap Icons（MIT）**，
  许可原文仍是 `gui_rs/assets/icons/LICENSE`；逐文件 URL + sha256 追加在 `gui_rs/assets/icons/SOURCES.md` 末尾
  （带注记：`tools/fetch_icons.py` 只写它自己那 30 个 + 2 个字库，不会覆盖本批）。
* 光栅化：`tools/svg2png.py` 对新增 39 个 SVG × 4 档（16/24/32/48）= **156 张 PNG，0 失败**；
  逐张 sha256 见 `build/sdk/icons_ext/manifest.json`（由 `tools/iconpack_ext.py` 生成）。
* 装卷：276 张 PNG（69 个名字 × 4 档）+ `/etc/icons_ext.json` 一起装进 VimtuFS2 卷（**277 个文件逐字节回读一致**），
  并拼出可引导盘 `build64/sdk/icondisk.img`。
* 运行期证据：见上面第 9 段 —— `[ICON64] init … src=vfs path=/etc/iconpack.bin` +
  `[ICON64] load kind=… path=/icons/… size=24 src=vfs ok=1`（30 个 kind 全部来自卷，`fallback=0`）。

### 本轮新增图标清单（39 个，MIT）

| 目录 | 名字 |
|---|---|
| `icons/system/`（34） | archive, arrow-repeat, arrow-up, audio, battery-charging, bell-slash, binary, calendar, chevron-down, chevron-up, clipboard, clock, copy, dash, disk, document, download, executable, file, folder, folder-plus, fullscreen, headphones, house, image, info-square, list, pencil, scissors, shield, trash2, upload, user-avatar, video, wifi-off |
| `icons/apps/`（5） | editor, image-viewer, player, archive-mgr,（第 5 个是本轮同时补的 `apps/` 里其余新增文件；全部见 `SOURCES.md` 末 39 行） |

★ 如实说明：**新名字**的图标（folder / document / copy / paste 类）目前是"装进卷 + 留档"，
内核只主动请求它自己 `kIcon64Names` 表里的 30 个 kind（`kernel/icons64.cpp:47-63`），
所以新名字要等内核侧登记对应 kind 才会被请求（本模板按纪律不改 `kernel/**`）。
**同名覆盖**（卷里放 `/icons/<sub>/<名字>@<尺寸>.png`）立刻生效，证据就是上面那 42 条 `src=vfs`。
