// rust64.h - VimtuOS GUI 的 Rust 侧（gui_rs crate）C 链接声明
//
// 为什么要单独一个头：Rust 侧导出的是 `#[no_mangle] extern "C"` 符号，
//   C++ 这边必须在**文件作用域**用 `extern "C"` 声明（块作用域上的 linkage 说明符
//   clang 不接受：实测 "expected unqualified-id"）。集中放这里，调用点只管调用。
//
// 链接方式：build64.sh -> `bash gui_rs/build_rs.sh`（rustc 用 rustup shim 的绝对路径，
//   目标 x86_64-unknown-none，`--crate-type staticlib --emit obj` + `-C panic=abort
//   -C relocation-model=static -C code-model=kernel`），产出 gui_rs/gui_rs.o，
//   然后**只加进系统内核**的链接行（安装介质内核不链它 —— 见 build64.sh）。
//
// 接口约定（与 gui_rs/src/lib.rs 一一对应）：
//   * 只传/只回 POD（`#[repr(C)]` 结构 + 定长整数）；字符串 = `*const u8 + len`（UTF-8，
//     不带隐含 NUL）；绝不返回 Rust 引用 / String / 泛型；
//   * 返回值：0 = 成功（`rust64_theme_set64`/`rust64_theme_colors64`/...）；
//     查询类接口直接回值（名字类回长度、越界回 0；accent 回 0xRRGGBB，越界回 0xFFFFFFFF）；
//   * Rust 侧 no_std、无 alloc、无浮点（小数一律千分比整数，见 gui_rs/src/tokens.rs）。
//
// 串口打点（自动验收 grep，格式勿改）：
//   [RUST64] tokens ok themes=7 accent=#RRGGBB selftest PASS
//     （失败时后缀是 `selftest FAIL mask=N`；由 kernel64.cpp 的系统内核启动路径打出）
#pragma once
#include <stdint.h>

/// 颜色（与 gui_rs/src/theme.rs 的 `Rgba` 完全同布局）。
struct Rust64Rgba {
    uint8_t r, g, b, a;
};

/// 主题配色（与 gui_rs/src/theme.rs 的 `Rust64ThemeColors` 完全同布局；
/// ★ 字段顺序 = ABI，加字段只能用 reserved，不能插在中间）。
struct Rust64ThemeColors {
    Rust64Rgba window_bg;   // 窗口背景
    Rust64Rgba card_bg;     // 内容卡片
    Rust64Rgba text;        // 主文本
    Rust64Rgba text_dim;    // 次要文本
    Rust64Rgba accent;      // 强调色
    Rust64Rgba dock;        // Dock / 任务栏（非暗色主题 = 固定默认色）
    Rust64Rgba border_hi;   // 边框高光（暗色主题 = 内高光）
    Rust64Rgba shadow;      // 阴影基准色（近阴影；远阴影见 rust64_theme_shadow64）
    Rust64Rgba grad_start;  // 渐变起点（纯色主题 = window_bg）
    Rust64Rgba grad_end;    // 渐变终点
    uint32_t is_dark;       // 1 = 暗色主题
    uint32_t reserved;      // 必须为 0
};

/// 阴影参数（与 gui_rs/src/theme.rs 的 `Rust64Shadow` 完全同布局）。
struct Rust64Shadow {
    int32_t dx, dy;
    uint32_t blur;
    Rust64Rgba color;
};

extern "C" {
// ---- 链接证明 / 自检 ----
const uint8_t* rust64_build_tag64(void);            // 产物标记（验收脚本按字节搜它证明链接进去了）
uint32_t       rust64_build_tag_len64(void);
uint32_t       rust64_tokens_init64(void);           // 启动期调用一次；0 = 自检全过，非 0 = 失败位图

// ---- 设计 Token（唯一真源；名字是稳定接口）----
uint32_t rust64_token_count64(void);
int32_t  rust64_token_lookup64(const uint8_t* name, uintptr_t len);   // 找不到 -> -1
uint32_t rust64_token_name64(uint32_t id, uint8_t* out, uintptr_t out_cap);  // 返回长度（out 可空）
int32_t  rust64_token_px64(uint32_t id);             // px / ms 值；越界或单位不符 -> -1
uint32_t rust64_token_permille64(uint32_t id);       // 千分比值；越界或单位不符 -> 0xFFFFFFFF

// ---- 主题表 / 当前主题 ----
uint32_t rust64_theme_count64(void);
uint32_t rust64_theme_name64(uint32_t idx, uint8_t* out, uintptr_t out_cap); // 返回长度；越界 -> 0
uint32_t rust64_theme_colors64(uint32_t idx, Rust64ThemeColors* out);        // 0 = 成功
uint32_t rust64_theme_shadow64(uint32_t idx, uint32_t far, Rust64Shadow* out); // far!=0 = 远阴影
uint32_t rust64_theme_set64(uint32_t idx);           // 0 = 成功；越界非 0 且不改动
uint32_t rust64_theme_current64(void);
uint32_t rust64_accent_rgb64(uint32_t idx);          // 0xRRGGBB；越界 -> 0xFFFFFFFF
uint32_t rust64_accent_on_rgb64(uint32_t idx);       // 压在强调色上的文字色
uint32_t rust64_theme_gradient64(uint32_t idx, uint32_t t_permille);  // 背景在 t 处的颜色
uint32_t rust64_blend_rgb64(uint32_t r1, uint32_t g1, uint32_t b1,
                            uint32_t r2, uint32_t g2, uint32_t b2, uint32_t t_permille);

// ---- panic 钩子（Rust 侧 panic 时回调；系统内核指向 rust64_panic_hook64）----
typedef void (*Rust64PanicHook)(const uint8_t* msg, uint32_t len);
void     rust64_panic_hook_set64(Rust64PanicHook hook);
uint32_t rust64_panicked64(void);                    // 1 = Rust 侧 panic 过
}
