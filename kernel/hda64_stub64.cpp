// kernel/hda64_stub64.cpp - **安装程序内核**的音频 ABI 弱兜底（构建修复，不是新功能）
//
// 为什么需要它
// ------------
// kernel/syscall64.cpp 是**两份内核共用**的一份源码：它带的音频系统调用
// （sc64_audio_play64 -> hda64_ready64 / hda64_play64）在系统内核里由 kernel/hda64.o 实现。
// 但安装程序内核（build64/kernel64.bin）那一遍**不编也不链** hda64.cpp ——
//   * 安装程序内核的对象集是 SRCS_CORE（见 build64.sh），不含 kernel/hda64.cpp；
//   * 而 hda64.cpp 依赖 config64/store64（把 ui.sound.volume 落到硬件），
//     这两者同样不在安装程序内核里，硬拉进来会连环缺符号。
// 结果是：安装程序内核链接期报
//   ld.lld: error: undefined symbol: hda64_ready64()
//   ld.lld: error: undefined symbol: hda64_play64(short const*, unsigned long)
// 构建在"链接两个内核"处 exit 1（ISO/硬盘映像都产不出来）。
//
// 做法（为什么是 weak）
// --------------------
// 这里用 `__attribute__((weak))` 提供两个**空实现**（没有音频控制器：ready=0，play=-1=ENODEV），
// 与 store64.h 里"ATA 弱链接约定"是同一个套路。weak 的语义正好让这条修复**自愈**：
//   * 现在：安装程序内核链不上真实现 -> 用弱兜底，构建通过；安装程序里本来就没有音频通路，
//     弱兜底只保证"符号存在且如实返回 ENODEV"（绝不假装出声）；
//   * 以后：哪天有人把 kernel/hda64.cpp（强定义）也加进安装程序内核的编译/链接，
//     强定义会**直接覆盖**弱兜底，不会出现重复符号错误，这个文件自动变成死代码（可删）。
//
// 不改的那条纪律：本文件**没有**任何新功能、没有策略（音量/静音/素材仍全在用户态 + hda64.cpp），
// 只是在安装程序内核里把这两个符号"补上并如实报无通路"。

#include <stddef.h>
#include <stdint.h>

__attribute__((weak)) int hda64_ready64() { return 0; }

__attribute__((weak)) int hda64_play64(const int16_t* pcm, size_t frames) {
    (void)pcm;
    (void)frames;
    return -1;              // 安装程序内核里没有 HDA 通路：如实失败，不假装出声
}
