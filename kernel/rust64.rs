// rust64.rs - VimtuOS 的 Rust 内核模块（no_std）
//
// 为什么有这个文件：
//   项目的既定路线是 **C + C++ + Rust**。本文件是"Rust 参与实现"的第一个模块，
//   用来把 Rust 真正链进内核并跑起来（不是演示文件）。
//
// 构建（由 build64.sh 调用，产出可直接被 ld.lld 链接的 .o）：
//   rustc --target x86_64-unknown-none --crate-type lib -O -C panic=abort \
//         -C relocation-model=static --emit obj=kernel/rust64.o kernel/rust64.rs
//   目标 x86_64-unknown-none 由 `rustup target add x86_64-unknown-none` 提供。
//
// 约束（与内核其余部分一致）：
//   * no_std：没有 libc / 没有 Rust 标准库；
//   * 不引入 panic 展开：panic = abort，且提供 `#[panic_handler]`；
//   * 与 C/C++ 的接口一律 `#[no_mangle] extern "C"`，类型用定长整数；
//   * 不依赖任何分配器（当前模块只用栈与纯计算）。

#![no_std]

use core::panic::PanicInfo;

/// panic 处理器：内核里不允许"未处理 panic"，所以这里**直接停机**并把原因留在静态变量里，
/// 供 C++ 侧在串口上打出来（不打印、不分配、不递归）。
#[panic_handler]
fn rust_panic64(_info: &PanicInfo) -> ! {
    unsafe { RUST_PANIC64_FLAG = 1; }
    loop {
        core::hint::spin_loop();
    }
}

/// Rust 侧发生 panic 时置 1（C++ 侧读它并打点）。
#[no_mangle]
pub static mut RUST_PANIC64_FLAG: u32 = 0;

// ==================== 1) 纯计算：FNV-1a 64 位哈希 ====================
/// FNV-1a 64：内核里做快速校验用（例如页池/位图/日志缓冲的自洽检查）。
#[no_mangle]
pub extern "C" fn rust_fnv1a64(data: *const u8, len: usize) -> u64 {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    if data.is_null() {
        return h;
    }
    let bytes = unsafe { core::slice::from_raw_parts(data, len) };
    let mut i = 0usize;
    while i < bytes.len() {
        h ^= bytes[i] as u64;
        h = h.wrapping_mul(0x0000_0100_0000_01B3);
        i += 1;
    }
    h
}

// ==================== 2) 纯计算：CRC32（与 Python zlib.crc32 同口径）====================
/// 标准反射 CRC-32（多项式 0xEDB88320），与内核/工具链里使用的 CRC32 口径一致。
/// 用位运算实现（不提表，省 .rodata；内核里调用频率不高）。
#[no_mangle]
pub extern "C" fn rust_crc32_64(data: *const u8, len: usize) -> u32 {
    let mut crc: u32 = 0xFFFF_FFFF;
    if data.is_null() {
        return !crc;
    }
    let bytes = unsafe { core::slice::from_raw_parts(data, len) };
    let mut i = 0usize;
    while i < bytes.len() {
        crc ^= bytes[i] as u32;
        let mut k = 0;
        while k < 8 {
            crc = if crc & 1 != 0 { (crc >> 1) ^ 0xEDB8_8320 } else { crc >> 1 };
            k += 1;
        }
        i += 1;
    }
    !crc
}

// ==================== 3) 自检：让 C++ 侧一次调用就能判定"Rust 真的在跑"====================
/// 返回 0 = 全部通过；非 0 = 失败位掩码（与内核其它 selftest 的约定一致）。
///   bit0 FNV-1a 对已知向量的结果正确（"123456789" 的 FNV-1a 64 = 0xA51D69DF880BAE86）
///   bit1 CRC32 对已知向量的结果正确（"123456789" 的 CRC-32 = 0xCBF43926）
///   bit2 空指针/零长度边界不崩且返回初值
///   bit3 长数据（4 KiB）哈希可重复（同一输入两次结果相同）
#[no_mangle]
pub extern "C" fn rust_selftest64() -> u32 {
    let mut fail: u32 = 0;

    // 已知向量："123456789"
    let v = b"123456789";

    let fnv = rust_fnv1a64(v.as_ptr(), v.len());
    if fnv != 0xA51D_69DF_880B_AE86 {
        fail |= 1;
    }

    let crc = rust_crc32_64(v.as_ptr(), v.len());
    if crc != 0xCBF4_3926 {
        fail |= 2;
    }

    // 边界：空指针 / 零长度
    let f0 = rust_fnv1a64(core::ptr::null(), 0);
    let c0 = rust_crc32_64(core::ptr::null(), 0);
    if f0 != 0xcbf2_9ce4_8422_2325 || c0 != 0 {
        fail |= 4;
    }

    // 可重复性：4 KiB 缓冲（栈上，不分配）
    let mut buf = [0u8; 4096];
    let mut i = 0usize;
    while i < buf.len() {
        buf[i] = (i as u8).wrapping_mul(31).wrapping_add(7);
        i += 1;
    }
    let h1 = rust_fnv1a64(buf.as_ptr(), buf.len());
    let h2 = rust_fnv1a64(buf.as_ptr(), buf.len());
    if h1 != h2 {
        fail |= 8;
    }

    fail
}

/// 供 C++ 侧查询"Rust 侧是否 panic 过"。
#[no_mangle]
pub extern "C" fn rust_panicked64() -> u32 {
    unsafe { RUST_PANIC64_FLAG }
}

/// 1 = 本模块已被链接进内核（链接器会把 C++ 侧未定义引用解析到这里）。
#[no_mangle]
pub extern "C" fn rust_present64() -> u32 {
    1
}
