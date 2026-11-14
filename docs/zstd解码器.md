# zstd 解码器（v0.10.3）

> 为什么要有这一页：`.deb` 从 Ubuntu 21.10 / Debian 12 起默认用 `control.tar.zst` +
> `data.tar.zst`，而 VimtuOS 原来只认 gzip 与 xz —— 发行版**原样**的包连 ar 扫描这一关都过不去
> （`VS_E_FORMAT`）。v0.10.3 补上 zstd，同时把 `data.tar` 里普遍存在的**符号链接**落到实处。

## 1. 为什么自己写（而不是 vendored）

| 方案 | 结论 |
|---|---|
| zstd 官方 `zstddeclib`（decompress-only 单文件） | **塞不进**：几百 KB 源码；vpkg 的 PT_LOAD 装载区当时只有 64 KiB |
| 只挑官方源码的一部分 | 官方解码器强耦合 FSE/Huffman/xxhash 三块，拆不开 |
| **按 RFC 8878 自己写**（本版选择） | 只覆盖 `.deb` 需要的子集，约 900 行 / 一个文件 / 零依赖 |

落点：`user/store/vs_zstd.c`。它**一个头文件都不包含**（不用 `vs.h`、不用 libc），
所以同一份源码能被宿主 gcc 直接编译做交叉验证（见 §4）。

## 2. 支持的边界（如实）

**支持**：
* 多帧拼接、skippable 帧（`0x184D2A50..5F`）；
* 块类型 Raw / RLE / Compressed；
* 字面量：Raw / RLE / Huffman（1 流与 4 流）/ Treeless（复用上一块的树）；
* 序列：`Predefined_Mode` / `RLE_Mode` / `FSE_Compressed_Mode` / `Repeat_Mode` 四种模式；
* FSE 表的 tableLog 5..9（规范上限：LL/ML 9、OF 8、Huffman 权重 6）；
* repeat offset 的全部语义，含 `literals_length == 0` 的例外（`ov=3 → rep1 - 1`）；
* `Content_Checksum`（xxh64 低 32 位）**真验**；
* `Frame_Content_Size` 与实际产出对账。

**不支持**（都在 `*why` 里给出静态 ASCII 原因，由 `vs_pkg.c` 打成日志，不假装成功）：
* 字典帧（`Dictionary_ID != 0`）——`.deb` 不用字典；
* 跨帧状态复用（规范要求每帧重置，本实现照规范）。

**内存口径**（与 xz 那条路一致）：**不按 Window_Size 分配环形窗口**，而是把调用方给的输出缓冲
当历史窗口（`dst[0..out)`）。依据：匹配距离只能引用本帧已产出的字节，所以"产出 ≤ dst_cap"时
永不回绕。字面量必须**单独一块**（序列执行要"边取字面量边写输出"，就地解会互相覆盖）；
zstd 单块解压后 ≤ 128 KiB，所以 `lit` 给 128 KiB 就够。

工作区（约 14 KB 的 FSE/Huffman 解码表）一律走 `vs_mmap`：
本仓库全体用户态都编 `-fno-zero-initialized-in-bss`，静态表会进 `.data` 把映像撑出装载区。

## 3. 实现里踩过的四个真坑（按发现顺序）

| # | 症状 | 真因 | 修法 |
|---|---|---|---|
| 1 | 全部含序列的样本 `match-offset-out-of-range` / `sequences-underflow` | **数据位流以 stop bit 收尾**：最后一字节里只有「stop bit(1) + 其上 0 填充」是无效位，真数据从它下面开始。从第 0 位起读 = 整体错位 | `zs_bwd_init_stream()`：`pos = 8 - highbit(lastByte)`，lastByte==0 判坏流 |
| 2 | 同上（更根本） | **LL/ML 码本的 Baseline/位宽表用错了**（我凭记忆写了一套"小表"：ML code 43 → 83/3 位）。官方是 ML code 43 → **131/7 位** | 按官方 `zstd_compression_format.md` 重抄两张表（见 §5 证据） |
| 3 | 所有用压缩字面量的样本 `huffman-tree` | **Huffman 树头的两个分支写反了**：`<128` 是"FSE 压缩的权重，长度 = headerByte"，`>=128` 才是"4 位直存，`Number_of_Weights = headerByte - 127`" | 对调分支 |
| 4 | 同上 | **码字分配不是经典 canonical**：官方是"按 Weight 升序（= 码长降序）、同 Weight 按符号升序，逐个占用连续的 `2^(Weight-1)` 个表项" | 改成顺序切分填表 |
| 5（顺带） | 模式字节读反 | 官方位序是 `LL = bits 7-6`、`OF = bits 5-4`、`ML = bits 3-2`、`bits 1-0` 保留 | 修正三处 switch |

坑 2 的定位方式值得记下来：对 200 个字节全 0 的样本，**总长 = 字面量数 + 匹配长度**，
所以 ML 必然是 198；流里那 7 位 = `0b1000011 = 67`，`131 + 67 = 198` 逐位吻合。
旧表（code 43 → 83 + 3 位）**任何读法都得不到 198** —— 这就把"顺序问题"排除掉了。

## 4. 验收：宿主交叉验证（不动 QEMU）

`tools/vs_zstd_hosttest.c` 把 `vs_zstd.c` 直接 `#include` 进来，用宿主 gcc 编；
`tools/zstd_selftest.py` 用**真 zstd CLI** 造 344 个样本喂它：

```
43 组 (数据形态 × 尺寸) × 4 个压缩级别(1/3/9/19) × 2 个校验开关(--check/--no-check) = 344
数据形态：全零 / 重复文本 / 纯随机 / 混合（随机 + 小偏置）
尺寸：0, 1, 7, 200, 300, 1023, 1024, 4096, 17000, 300000, 1200000
```

判据（每条都能独立证伪）：

* **A** 解出长度 == 原文件长度；
* **B** 解出内容与原文件**逐字节一致**；
* **C** 负向对照：把压缩流截断 8 字节后必须**失败**（证明不是"解不动也返回 0"）。

实测（v0.10.3）：

```
真 zstd 样本：344 个（43 组 kind/size × 4 级 × 2 校验）
[ok] 344 / [FAIL] 0
负向对照：264 个样本被判住（小样本 <16 B 不参与）
===== RESULT: PASS =====
```

再跑：`py -3 -u tools/zstd_selftest.py`（退出码 0 = 全过；2 = 缺 zstd/gcc）。

## 5. 本机上留存的规范原文

`doc/zstd_compression_format.md`（zstd v1.5.5）是本版全部格式细节的依据，包括：
`Literals_Section_Header` 的 Size_Format 分档、`Huffman_Tree_Description`、
`Decoding Sequences` 的位序与状态顺序、`Repeat offsets`、
以及三张 `Default Distributions`。改解码器前请先读它的对应章节。

## 6. 和 `.deb` 链路的关系

`user/store/vs_pkg.c` 的 `deb_unzstd()` 是唯一调用点：

```
[VPKG] deb compression control=zst data=zst          ← 分派（gz/xz/zst 三条路）
[VPKG] deb zstd member=control.tar.zst in=… out=…    ← 成功
[VPKG] deb zstd member=data.tar.zst at=… reason=…    ← 失败（带偏移与原因）
```

失败返回 `VS_E_ZSTD`（错误码 15），由 `vs_err_name()` 显示为 `zstd`。
