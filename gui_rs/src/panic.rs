//! panic.rs - Rust 侧的 panic 处理（内核里 panic = 打印 + 停机，**不静默自旋**）
//!
//! 语义（与 kernel/panic64.h 的约定一致）：
//!   1. 先把一行 `[RUST64] PANIC ...` 打到 COM1 + debugcon（什么时候都能用）；
//!   2. 如果 C++ 侧在启动期装了钩子（`rust64_panic_hook_set64`，系统内核里指向
//!      `rust64_panic_hook64` -> panic64_bsod64），就走内核统一的蓝屏路径
//!      （蓝屏画屏 + `[PANIC64] stop=...` 串口行）；
//!   3. 钩子没装（或钩子返回了）就 `cli; hlt` 停机 —— 绝不停在静默死循环里。
//!
//! 为什么用"钩子"而不是直接 `extern "C" { fn panic64_bsod64(...) }`：
//!   Rust 目标文件只在**系统内核**里链接，而内核里 `panic64.cpp` 也用 weak 引用的
//!   方式处理"可能没链进来"的情况；用函数指针登记把链接期依赖变成运行期依赖，
//!   Rust 侧就永远不会有未定义符号（安装介质将来若链 Rust 也不会因此链接失败）。
//!
//! ★ 内核不链接 core 的 rlib，所以本文件里**故意不用会生成 panic 的写法**：
//!   任何 `slice[i]` 索引一旦 LLVM 消不掉边界检查，就会留下未定义符号
//!   `core::panicking::panic_bounds_check`（实测在 serial_dec 里出现过，已改裸指针）。

use core::panic::PanicInfo;

/// panic 现场：文件/行号/列号（`PanicInfo::location()` 只有带上调用位置信息时才有值，
/// 拿不到就写 0）。
struct Loc {
    file: &'static str,
    line: u32,
    col: u32,
}

fn location_of(info: &PanicInfo) -> Loc {
    match info.location() {
        Some(l) => Loc { file: l.file(), line: l.line(), col: l.column() },
        None => Loc { file: "?", line: 0, col: 0 },
    }
}

/// 串口 COM1(0x3F8) 写一个字节（内核 debug64.h 的 dbg64_putc 同一条通道），
/// 同时镜像到 QEMU 的 debugcon(0x402)。
#[inline(always)]
fn serial_putc(c: u8) {
    unsafe {
        // 0x3FD = LSR，bit5 = THR empty（等不到就直接写，绝不在 panic 路径上死等）
        let mut lsr: u8;
        core::arch::asm!("in al, dx", in("dx") 0x3FDu16, out("al") lsr, options(nomem, nostack));
        let mut spins = 0u32;
        while (lsr & 0x20) == 0 && spins < 100_000 {
            core::arch::asm!("in al, dx", in("dx") 0x3FDu16, out("al") lsr, options(nomem, nostack));
            spins += 1;
        }
        core::arch::asm!("out dx, al", in("dx") 0x3F8u16, in("al") c, options(nomem, nostack));
        core::arch::asm!("out dx, al", in("dx") 0x402u16, in("al") c, options(nomem, nostack));
    }
}

fn serial_str(s: &[u8]) {
    let mut i = 0usize;
    while i < s.len() {
        serial_putc(unsafe { *s.as_ptr().add(i) });
        i += 1;
    }
}

fn serial_dec(mut v: u32) {
    // 10 字节足够 u32（最多 10 位）。用裸指针读写而不是 `buf[n]` 索引：
    // 避免为"不可能发生的越界"生成 core 的 panic_bounds_check 引用。
    let mut buf = [0u8; 10];
    let p = buf.as_mut_ptr();
    let mut n = 0usize;
    if v == 0 {
        unsafe { core::ptr::write(p, b'0') };
        n = 1;
    }
    while v > 0 && n < 10 {
        let d = (v % 10) as u8;
        v /= 10;
        unsafe { core::ptr::write(p.add(n), b'0' + d) };
        n += 1;
    }
    while n > 0 {
        n -= 1;
        serial_putc(unsafe { core::ptr::read(p.add(n)) });
    }
}

/// 停机（cli + hlt 循环）：不再返回，也不静默自旋。
fn halt_forever() -> ! {
    loop {
        unsafe {
            core::arch::asm!("cli", "hlt", options(nomem, nostack));
        }
    }
}

/// C++ 侧登记的 panic 钩子（收到 NUL 结尾的 UTF-8 消息 + 长度；允许返回）。
pub type PanicHook = unsafe extern "C" fn(*const u8, u32);

static mut PANIC_HOOK: Option<PanicHook> = None;
/// 1 = Rust 侧已经 panic 过（C++ 侧读 `rust64_panicked64()`）。
static mut PANICKED_FLAG: u32 = 0;

/// 登记内核的 panic 钩子（系统内核在启动期调用；传空则退回"串口 + 停机"）。
#[no_mangle]
pub extern "C" fn rust64_panic_hook_set64(hook: Option<PanicHook>) {
    unsafe { PANIC_HOOK = hook };
}

/// 1 = Rust 侧发生过 panic。
#[no_mangle]
pub extern "C" fn rust64_panicked64() -> u32 {
    unsafe { PANICKED_FLAG }
}

#[panic_handler]
fn rust64_panic_handler(info: &PanicInfo) -> ! {
    // 不静默：先置标志、再打串口。用 IN_PANIC 防止"panic 里 panic"无限递归。
    static mut IN_PANIC: u32 = 0;
    unsafe {
        PANICKED_FLAG = 1;
        if IN_PANIC != 0 {
            // 已经在 panic 处理里又 panic：只停机，别递归打印
            halt_forever();
        }
        IN_PANIC = 1;
    }

    let loc = location_of(info);
    serial_str(b"\r\n[RUST64] PANIC at ");
    serial_str(loc.file.as_bytes());
    serial_putc(b':');
    serial_dec(loc.line);
    serial_putc(b':');
    serial_dec(loc.col);
    serial_str(b" (rust panic_handler; no unwinding, kernel halts)\r\n");
    serial_str(b"[RUST64] panicked=1\r\n");

    // 钩子：走内核统一蓝屏（panic64_bsod64 不返回；万一返回了下面还有停机兜底）。
    // 消息用固定串 —— 避免在 panic 路径上做任何可能再次 panic 的格式化。
    let msg: &[u8] = b"rust64 panic (see COM1 for file:line)\0";
    let hook = unsafe { PANIC_HOOK };
    if let Some(f) = hook {
        unsafe { f(msg.as_ptr(), (msg.len() - 1) as u32) };
    }

    halt_forever();
}
