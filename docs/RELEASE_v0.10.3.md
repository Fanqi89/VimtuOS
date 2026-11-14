# RELEASE v0.10.3 — 本地包管理打通：`.deb` 的 zstd 与符号链接

发布日期：2026-11-15
上一版：v0.10.2（真机加固 + 桌面帧预算）

---

## 1. 一句话

**VimtuOS 现在能装 Ubuntu 21.10+ / Debian 12+ 默认格式（`data.tar.zst`）的 `.deb`，
并且把包里的符号链接建成真正的符号链接。** 这是"本地包管理"从 demo 走向可用的一步。

## 2. 三件事

### 2.1 zstd 解码器（`user/store/vs_zstd.c`，新文件）

`.deb` 的压缩壳从两条路变成三条：**gzip / xz / zstd**（bzip2 仍如实拒绝）。

* 自己按 RFC 8878 写，**900 行、零依赖**（不包含任何头文件，不用 `vs.h`/libc）。
  为什么不用 vendored：官方 decompress-only 单文件几百 KB，而 vpkg 当时的 PT_LOAD
  装载区只有 64 KiB。
* 覆盖面：多帧拼接 / skippable 帧 / Raw·RLE·Compressed 块 / 字面量 Raw·RLE·Huffman(1 流+4 流)·Treeless /
  序列四种模式（Predefined/RLE/FSE-Compressed/Repeat）/ FSE tableLog 5..9 /
  repeat offset 全语义（含 `literals_length==0` 的 `rep1-1` 例外）/ **xxh64 内容校验真验**。
* 不支持字典帧（`.deb` 不用），边界写在 `docs/zstd解码器.md` §2。

**验收（宿主交叉验证，不依赖 QEMU）**：`tools/zstd_selftest.py` 用**真 zstd CLI**
造 344 个样本（43 组形态×尺寸 × 4 个压缩级别 × 2 个校验开关），
判据 = 长度一致 + **逐字节一致** + 截断 8 字节必须失败。

```
真 zstd 样本：344 个
[ok] 344 / [FAIL] 0 ；负向对照 264 个被判住
===== RESULT: PASS =====
```

### 2.2 符号链接（`vs_pkg.c` 的 tar 落盘路径）

* tar 条目 `type=2` 不再整包拒绝，改为按内核 syscall 88 建**真链接**
  （`kernel/vfs64.cpp` 的 `vfs64_symlink64` 自 v0.7.0 就在，缺的一直是装包侧）；
* 目标字符串原样存（≤511 B），相对目标由内核按"链接所在目录 + 目标"解析；
* 记进已装文件清单 → 卸载时会被删掉；
* 重装/覆盖：先 `unlink` 再建（dpkg 语义）。

### 2.3 上限放宽（盘量决定，不是拍的）

| 项 | 旧 | 新 |
|---|---|---|
| 包体大小上限 | 8 MiB | **16 MiB** |
| `control.tar` 解出上限 | 256 KiB | **1 MiB** |
| `data.tar` 解出上限 | 4 MiB | **16 MiB** |

系统卷本身只有 ~12.6 MiB（`PART_MAIN 32584 + 24759`），所以这已经是"能塞进卷"的量级。

## 3. 顺带的结构性改动

**`/bin/vpkg` 换成"大映像"基址**（新文件 `user/store/vpkg64.ld`，基址 `4GiB+576KiB`）。

装了 zstd 之后 vpkg 的 PT_LOAD 从 61,851 B 涨到 74,547 B，越过老装载区上界
（`USER64_STACK_VA64 = 4GiB+64KiB`），构建期自检如实报红：

```
build64/store/vpkg.elf PT_LOAD 越出主程序装载区（va=0x10000ee80 msz=0x2ef0 <= 0x10000）
```

内核**早就支持**这条路（`kernel/elf64.cpp` 的 `e64_pick_main_layout64` 两条路，
`tcc/busybox/lua/make` 都是这么干的），所以没有改内核，只换了链接脚本。
`build_store.sh` 的自检按产物分档：store 与两个示例载荷仍必须落在 64 KiB 老区，
vpkg 必须落在 `[4GiB+576KiB, 4GiB+998MiB)` —— 判据没有放松，是换成了内核真实的边界。

## 4. 验收矩阵（本版实测）

| 项 | 结果 |
|---|---|
| `tools/zstd_selftest.py`（344 个真 zstd 样本） | **PASS**（344/344，负向对照 264） |
| `user/store/build_store.sh` 四个产物自检 | **EXIT=0**；vpkg.elf 80,112 B / memsz 74,547 B |
| 全量构建 `build64.sh` | 见 §6 |
| `tests/dpkg64_test.py` | **PASS（exit=0）**：A0 + A–I 全绿，见 §5 |
| `tests/vpkg64_test.py` | **61 PASS / 20 FAIL**（两次运行逐位相同，非偶发）—— 见 §8 |

## 5. 新增的验收判据（`tests/dpkg64_test.py`）

新客体 `/usr/share/dpkg/test-zstd-2.0.deb`（由 `tools/dpkg_pack_win.py` 用**真 zstd**现场生成）：
`control.tar.zst` + `data.tar.zst`，载荷 = 1 个文件 + **2 条符号链接**（相对 + 绝对）。

* **F**：`dpkg -i` 该包 → run ok + 进程 exit 0，且串口必须同时出现
  `deb compression control=zst data=zst`、`deb zstd member=data.tar.zst`、
  `deb stats files=3 dirs=4 links=2`、两条 `install link path=… target=…`；
* **G**：`cat /usr/bin/vimtu-zstd-link`（**相对**链接）→ 终端读到 >0 字节；
* **H**：`cat /usr/share/vimtu-zstd-test/abs-link`（**绝对**链接）→ 终端读到 >0 字节；
* **I**：`dpkg -l` 同时列出两个包。

旧的 A–E 一条没删。

## 6. 未做 / 如实说明

* **维护者脚本**（`preinst/postinst/prerm/postrm`）仍然整包拒绝（`VS_E_SCRIPTS`）。
  要真支持需要 shell + fork/exec + 事务顺序，是下一大块。
* **`/var/lib/dpkg/status`** 仍是自有格式（`/var/lib/vpkg/installed.json`），
  真 `dpkg` 工具链读不了我们的库（我们的 `dpkg` 是同一个 vpkg 字节的 `argv[0]` 前端）。
* **依赖**仍只解析一层、不认版本区间；`Pre-Depends/Provides/Conflicts/Breaks` 不解析。
* **落盘权限**一律 0644（内核 `O_CREAT` 不吃 mode），可执行靠"ELF 加载器只要 r 权限"。
* **bzip2** 仍不支持。
* 大页机制保持**默认关闭**（v0.10.2 的结论：判据未达标，不再追求纯 CPU 的 60 fps）。

## 7. 产物

```
build64/kernel64_os.bin      （见 §6 实测字节数）
build64/store/vpkg.elf       80,112 B   PT_LOAD memsz 74,547 B（大映像窗口）
vimtu64-64.iso               真机安装介质（含 UEFI 修复）
vimtu64-64.img               演示盘
```

真机首测要抓的原始行见 `docs/真机启动加固.md` §2。

## 8. 本轮已知红项（如实，且**不属于本轮改动**）

`tests/vpkg64_test.py`（应用商店 + 包管理器端到端）本次 **61 PASS / 20 FAIL**，
两次运行逐位相同（非偶发）。根因已定位到两类，**都不在本轮改动的代码路径上**：

**(a) 2 条 `⑩` 红 = 测试侧没走 `/bin -> /usr/bin` 别名（产品侧字节是对的）**

* 客户机里 `run /bin/hello-cli` 是 PASS（`[HELLO-CLI] done` + `summary rc=0`）；
* 宿主侧却读 `/bin/hello-cli` 失败 —— 因为卷上**没有 `bin` 这个目录**，
  文件真身在 `/usr/bin/hello-cli`。现场实测（宿主直接解析 fixture 卷）：

```
/bin/hello-cli            -> None
/usr/bin/hello-cli        -> 11520 B b'\x7fELF'      ← 与 .vap64 的 payload 段逐字节一致
/usr/share/hello-cli/hello-cli.vap64 -> 11562 B b'VAP6'
```

即判据本意（"payload 原样落在卷上"）是**成立的**，只是测试读错了路径。

**(b) 18 条 GUI 红 = 商店窗口的精确配色/点击命中探测**

* 商店窗口**起来了、渲染了**（`[STORE] render f=0 sel=0 row0=hello-cli row1=gui-demo
  row2=vpkg-debdemo state0=installed` 全 PASS，窗口里亮像素 `bright>=300` PASS）；
* 但 `count_color(CO_OK)`（绿图标）与 `count_color(CO_ROWSEL)`（选中行底色）都探到 **0 px**，
  随后按同一原点算出的点击也全部打空（`[STORE] click` 一行都没出现）。

归属判断（可核对的证据，不是猜）：

* 本轮唯一进入 GUI 通路的改动是 `vs_io.c` 的 `vs_err_name` 多一条 `case VS_E_ZSTD: return "zstd";`
  （+4 B 字符串）与 `vs_pkg.c` 里被 `--gc-sections` 掉的 tar 代码 —— **不参与渲染**；
* 商店的画布由 `user/store/store.c` 画（最近改动 `cf9ec01`，早于 v0.10.0），
  上屏由合成器 `user/wm/wm.c` 拷贝（**最近改动正是 v0.10.2 的 `9b4f86f`**，
  那次把混色改成 `(t*257+257)>>16` 并改了帧节拍/输入批次重画）；
* `store.c` 里选中行底色是 `fill_rect(..., (sel ? CO_ROWSEL : CO_ROW) & 0xFFFFFFu)` ——
  **颜色常量的 alpha 字节被抹成 0**，这类像素在合成器里走哪条混色分支、是否被判为透明，
  正好落在 v0.10.2 改过的那段代码上。

结论：**未证明它是绿的，也未证明是本轮改红的**；它最可能是 v0.10.2 合成器改动的遗留，
属于下一轮"GUI 夹具与合成器对齐"的活。本轮**不**因为它阻塞发版（它不在本轮交付的
`.deb`/zstd/symlink 路径上），但明确登记为红项。
