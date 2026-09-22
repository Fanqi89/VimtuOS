// rust64.h - Rust 模块（kernel/rust64.rs）的 C 链接声明
//
// 为什么要单独一个头：Rust 侧导出的是 `#[no_mangle] extern "C"` 符号，
//   C++ 这边必须在**文件作用域**用 `extern "C"` 声明（块作用域上的 linkage 说明符
//   clang 不接受：实测 "expected unqualified-id"）。集中放这里，调用点只管调用。
//
// 链接方式：build64.sh 里 `rustc --target x86_64-unknown-none --crate-type lib -O
//   -C panic=abort -C relocation-model=static --emit obj=$BUILD/os/rust64.o kernel/rust64.rs`，
//   然后把 rust64.o 加进**系统内核**的链接行（安装介质内核不链）。
#pragma once
#include <stdint.h>

extern "C" {
// 1 = 本模块已被链接进内核（用于自检"Rust 真的在里面"）。
uint32_t rust_present64(void);
// 自检：0 = 全过；非 0 = 失败位掩码（bit0 FNV-1a 向量、bit1 CRC32 向量、
//        bit2 空指针/零长边界、bit3 长数据可重复性）。
uint32_t rust_selftest64(void);
// 1 = Rust 侧发生过 panic（panic_handler 里置位；内核里 panic = 停机）。
uint32_t rust_panicked64(void);
// 纯计算接口（供内核其它模块按需使用）：
uint64_t rust_fnv1a64(const uint8_t* data, uintptr_t len);
uint32_t rust_crc32_64(const uint8_t* data, uintptr_t len);
}
