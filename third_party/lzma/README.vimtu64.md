# third_party/lzma —— .deb 的 xz/LZMA2 解码核（vendored，公共领域）

## 这是什么

`/bin/vpkg` 要能装**发行版原样的** `.deb`，而 Debian 的包体是
`control.tar.xz` + `data.tar.xz`（xz 容器 + LZMA2 + LZMA 范围编码）。
仓库里原本只有 gzip/DEFLATE（`user/store/vs_gzip.c`），所以这里 vendor 一份
**公共领域**的 LZMA 解码核，再由 `user/store/vs_xz.c` 接上 xz **容器**解析。

## 来源与版本（可核对）

* 上游：LZMA SDK（Igor Pavlov，7-Zip 的 C 源码）。
* 抓取地址：`https://raw.githubusercontent.com/jljusten/LZMA-SDK/master/C/<file>`
  （`jljusten/LZMA-SDK` 是 7-Zip 官方 C 源码的公开镜像；每份文件头自带版本日期）。
* 抓取时间：2026-02（本仓库这一批"deb 链路补齐"）。
* 文件头里的上游日期：`7zTypes.h` 2017-07-17、`LzmaDec.h` 2018-04-21、
  `LzmaDec.c` 2018-02-28、`Lzma2Dec.h`/`Lzma2Dec.c` 2018-02-19。

入库后的 sha256（**行尾已按本仓库 `.gitattributes`（`* text=auto eol=lf`）规范成 LF**，
所以与上游 raw 的 CRLF 版本 sha256 不同；内容一个字节没改，只换了行尾）：

| 文件 | 字节 | sha256 |
|---|---|---|
| `7zTypes.h`  | 8805  | `915197bfd119b107355f5ff62745d29c7b5b01987e6c10c668a91eae840ccab8` |
| `LzmaDec.h`  | 6902  | `4d92fb5278e566199f9ee6b92938557ec2172e2e366ec620bf175158f08369f0` |
| `LzmaDec.c`  | 31846 | `51277f5b76080cce1ef3a2d973c63c4b9f4942337015459d4b8812162e53ae09` |
| `Lzma2Dec.h` | 3711  | `e3d61890f89b2d0d7f451d4b650a535e49c5de4be7ad9ae1e88e152a4570fe21` |
| `Lzma2Dec.c` | 12670 | `1e8745fea9a231227da0afbcb1736a00abacdcaa2e1fa2959d6ae3ac1f06dc47` |

## 许可

**公共领域**（每个文件头都写着 `Igor Pavlov : Public domain`）——见同目录 `LICENSE` 与
仓库根部 `LICENSE`。没有 copyleft、没有署名义务、不需要随二进制分发许可文本。

## 本仓库怎么用它（没有改上游一个字节）

* 上游 `LzmaDec.c` / `Lzma2Dec.c` 的第一行是 `#include "Precomp.h"`；本目录的
  `Precomp.h` **是本仓库写的替身**（只有一行 `#include "7zTypes.h"`），
  用来替掉 7-Zip 构建树里那份会拉进 `Compiler.h`/`CpuArch.h`/平台头的版本。
  除此之外上游文件**原样**。
* 调用方只有一处：`user/store/vs_xz.c`（xz 容器 → 过滤器链 → LZMA2 块 → 字典接口），
  由 `user/store/build_store.sh` 编进 `/bin/vpkg`（GUI 版 `/bin/store` **不编**，
  deb 走 `/bin/vpkg` 子进程，见 `user/store/store.c` 的说明）。
* 只用了两个入口：`Lzma2Dec_AllocateProbs`（**只要概率表**，字典缓冲由我们给，
  避免 SDK 自己按流里声明的 8 MiB dict 去分配）+ `Lzma2Dec_DecodeToDic`。
  没有用 `Lzma2Dec_Allocate`（它会自建按 dictSize 的字典缓冲）、也没有用 BCJ/Delta
  过滤器（见 `vs_xz.c` 顶部"如实边界"）。
* 概率表（每次约 20 KiB）与字典缓冲都走 `vs_mmap`（`ISzAlloc` 适配器在 `vs_xz.c` 里），
  不占 `.data`（本仓库一律 `-fno-zero-initialized-in-bss`，静态大表会顶爆 64 KiB 装载区）。
