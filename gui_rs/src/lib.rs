//! lib.rs - VimtuOS GUI 的 Rust 侧 crate 根（`gui_rs`）
//!
//! 这是"Rust 正式接入内核"的第一个**真模块**（不是玩具示例）：
//!   * `tokens.rs` —— 设计 Token 表（圆角/模糊/透明度/阴影/动效/缓动/字号）唯一真源
//!   * `theme.rs`  —— 六个主题的配色表 + 配色计算（渐变插值、对比色、阴影参数）
//!   * `panic.rs`  —— `#[panic_handler]`：串口打点 + 内核蓝屏钩子 + `cli; hlt` 停机
//!
//! 构建（**只有系统内核链接本 crate 的产物**，安装介质不链）：
//!   bash gui_rs/build_rs.sh          # -> gui_rs/gui_rs.o（rustc 直接产出 ELF64 目标文件）
//!   具体命令见 build_rs.sh 里的 RUSTC_ARGS（--crate-type staticlib --emit obj、
//!   `-C panic=abort -C relocation-model=static -C code-model=kernel`）。
//!
//! C++ 侧接口约定（详见 kernel/rust64.h）：
//!   * 全部 `#[no_mangle] extern "C"`，**只传/只回 POD**（`#[repr(C)]` 结构、定长整数）；
//!   * 字符串 = `*const u8 + len`（UTF-8，**不带**隐含 NUL），绝不返回 Rust 引用/String；
//!   * 返回值约定：`0 = 成功`（少数查询接口直接回值，见各函数注释）；
//!   * 本 crate `#![no_std]`，**不使用 alloc / 不使用浮点**（内核 -mno-sse 环境，
//!     小数一律千分比整数，见 tokens.rs 顶部说明）。

#![no_std]
// 库对外接口里有些常量/函数目前只有 C++ 侧会用（或只留给下一步的渲染线），
// 这里统一放行 dead_code 警告，保持构建输出干净。
#![allow(dead_code)]

pub mod panic;
pub mod theme;
pub mod tokens;

use theme::{Rgba, Rust64Shadow, Rust64ThemeColors};

/// 链接证明标记：验收脚本用它在最终内核镜像里**按字节搜到 Rust 产物**
/// （`.rodata` 从 gui_rs.o 进内核 = 链接器没有把它扔掉）。改字符串等于改验收脚本。
pub const BUILD_TAG: &[u8] = b"RUST64-GUI-TOKENS-THEME-1";

/// 把 `src` 拷进 `out`（最多 `cap` 字节），返回**源长度**（不是拷贝长度）。
/// 安全约定：`out` 为 null 或 cap=0 时不写、只返回长度（调用方先问长度再分配）。
unsafe fn copy_out(src: &[u8], out: *mut u8, cap: usize) -> u32 {
    if !out.is_null() && cap > 0 {
        let n = if src.len() < cap { src.len() } else { cap };
        let mut i = 0usize;
        while i < n {
            core::ptr::write(out.add(i), src[i]);
            i += 1;
        }
    }
    src.len() as u32
}

// ==================== 0) 链接证明 ====================
#[no_mangle]
pub extern "C" fn rust64_build_tag64() -> *const u8 {
    BUILD_TAG.as_ptr()
}

#[no_mangle]
pub extern "C" fn rust64_build_tag_len64() -> u32 {
    BUILD_TAG.len() as u32
}

// ==================== 1) 启动自检 ====================
/// 启动期调用一次：跑完 Token 表 + 主题表 + 配色计算的自检。
/// 返回 0 = 全过；非 0 = 失败位图（位含义见 theme.rs 的 selftest 注释）。
#[no_mangle]
pub extern "C" fn rust64_tokens_init64() -> u32 {
    theme::selftest()
}

// ==================== 2) 设计 Token 查询 ====================
#[no_mangle]
pub extern "C" fn rust64_token_count64() -> u32 {
    tokens::count()
}

/// 按名字查 token id。名字是 UTF-8（不带 NUL）。找不到 -> -1。
#[no_mangle]
pub extern "C" fn rust64_token_lookup64(name: *const u8, len: usize) -> i32 {
    if name.is_null() {
        return tokens::LOOKUP_NOT_FOUND;
    }
    let bytes = unsafe { core::slice::from_raw_parts(name, len) };
    match tokens::find(bytes) {
        Some(id) => id as i32,
        None => tokens::LOOKUP_NOT_FOUND,
    }
}

/// 按下标取 token 名（UTF-8）：返回长度；`out` 可空/`cap=0` 表示"只问长度"。
#[no_mangle]
pub extern "C" fn rust64_token_name64(id: u32, out: *mut u8, cap: usize) -> u32 {
    match tokens::at(id) {
        Some(t) => unsafe { copy_out(t.name.as_bytes(), out, cap) },
        None => 0,
    }
}

/// px / ms 类 token 的值；越界或单位不符 -> -1。
#[no_mangle]
pub extern "C" fn rust64_token_px64(id: u32) -> i32 {
    tokens::px(id)
}

/// 千分比类 token 的值（0..=1000）；越界或单位不符 -> 0xFFFFFFFF。
#[no_mangle]
pub extern "C" fn rust64_token_permille64(id: u32) -> u32 {
    tokens::permille(id)
}

// ==================== 3) 主题表 / 当前主题 ====================
#[no_mangle]
pub extern "C" fn rust64_theme_count64() -> u32 {
    theme::count()
}

/// 主题名（UTF-8）：返回长度；`out` 可空/`cap=0` 只问长度；越界 -> 0。
#[no_mangle]
pub extern "C" fn rust64_theme_name64(idx: u32, out: *mut u8, out_cap: usize) -> u32 {
    match theme::name_bytes(idx) {
        Some(b) => unsafe { copy_out(b, out, out_cap) },
        None => 0,
    }
}

/// 主题配色 -> `out`。返回 0 = 成功；1 = idx 越界或 out 为空（不改动 out）。
#[no_mangle]
pub extern "C" fn rust64_theme_colors64(idx: u32, out: *mut Rust64ThemeColors) -> u32 {
    if out.is_null() {
        return 1;
    }
    match theme::colors(idx) {
        Some(c) => {
            unsafe { core::ptr::write(out, c) };
            0
        }
        None => 1,
    }
}

/// 阴影参数（近/远）-> `out`。返回 0 = 成功；1 = 越界或 out 为空。
#[no_mangle]
pub extern "C" fn rust64_theme_shadow64(idx: u32, far: u32, out: *mut Rust64Shadow) -> u32 {
    if out.is_null() {
        return 1;
    }
    match theme::shadow(idx, far != 0) {
        Some(s) => {
            unsafe { core::ptr::write(out, s) };
            0
        }
        None => 1,
    }
}

/// 设置当前主题。返回 0 = 成功；1 = idx 越界（不改动当前主题）。
#[no_mangle]
pub extern "C" fn rust64_theme_set64(idx: u32) -> u32 {
    theme::set_current(idx)
}

/// 当前主题下标。
#[no_mangle]
pub extern "C" fn rust64_theme_current64() -> u32 {
    theme::current()
}

/// 主题强调色，打包成 0xRRGGBB（便于 C++ 直接比对）；越界 -> 0xFFFFFFFF。
#[no_mangle]
pub extern "C" fn rust64_accent_rgb64(idx: u32) -> u32 {
    match theme::at(idx) {
        Some(t) => t.accent.to_rgb24(),
        None => 0xFFFF_FFFF,
    }
}

/// "压在强调色上"的文字色 0xRRGGBB（亮强调色 -> 深字，暗强调色 -> 白字）；越界 -> 0xFFFFFFFF。
#[no_mangle]
pub extern "C" fn rust64_accent_on_rgb64(idx: u32) -> u32 {
    match theme::on_accent(idx) {
        Some(c) => c.to_rgb24(),
        None => 0xFFFF_FFFF,
    }
}

/// 主题背景在 `t`（千分比，会被夹到 0..=1000）处的颜色 0xRRGGBB：
/// 渐变主题 = 起点->终点插值；纯色主题 = window_bg。越界 -> 0xFFFFFFFF。
#[no_mangle]
pub extern "C" fn rust64_theme_gradient64(idx: u32, t_permille: u32) -> u32 {
    match theme::gradient_at(idx, t_permille) {
        Some(c) => c.to_rgb24(),
        None => 0xFFFF_FFFF,
    }
}

/// 通用配色计算：两色按 `t`（千分比）混合，返回 0xRRGGBB（忽略 alpha）。
/// 渲染原语想做"强调色淡出""卡片叠色"时用这个，不用再各写一份插值。
#[no_mangle]
pub extern "C" fn rust64_blend_rgb64(
    r1: u32,
    g1: u32,
    b1: u32,
    r2: u32,
    g2: u32,
    b2: u32,
    t_permille: u32,
) -> u32 {
    let a = Rgba::new(r1 as u8, g1 as u8, b1 as u8, 255);
    let b = Rgba::new(r2 as u8, g2 as u8, b2 as u8, 255);
    theme::blend(a, b, t_permille).to_rgb24()
}
