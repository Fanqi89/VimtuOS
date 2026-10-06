/* Precomp.h - ★ 本仓库自己写的**极小替身**（不是 LZMA SDK 的文件，别与上游的 Precomp.h 混）
 *
 * 为什么要有它：SDK 里的 LzmaDec.c / Lzma2Dec.c 第一行就是 `#include "Precomp.h"`，
 *   而上游的 Precomp.h 会拉进 Compiler.h / CpuArch.h / 平台头（7-Zip 的构建树才有）。
 *   本仓库只用到 LzmaDec + Lzma2Dec 两个解码核，需要的公共类型全在 7zTypes.h 里，
 *   所以这里给一个只有一行 include 的替身，**不改上游 .c/.h 一个字节**。
 *
 * 上游对应文件：LZMA SDK 的 C/Precomp.h（本目录 README.vimtu64.md 记了来源与许可）。
 */
#ifndef VIMTU_LZMA_PRECOMP_H
#define VIMTU_LZMA_PRECOMP_H

#include "7zTypes.h"

#endif /* VIMTU_LZMA_PRECOMP_H */
