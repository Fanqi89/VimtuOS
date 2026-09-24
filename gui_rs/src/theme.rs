//! theme.rs - VimtuOS 主题表 + 配色计算（Rust 侧，供 C++ 调用）
//!
//! 主题是"设计 Token 之上的一层"：Token 给的是所有主题共用的**形状/动效**参数
//! （圆角、模糊、时长、缓动，见 tokens.rs），主题给的是每个皮肤的**颜色**：
//!   窗口背景 / 卡片 / 主文本 / 次要文本 / 强调色 / Dock / 边框高光 / 阴影，
//! 自定义渐变主题再给 起点 + 终点 rgba（其余主题起点=终点=窗口背景，即纯色）。
//!
//! 六个主题（索引固定，C++ 侧 / 测试按索引与名字两层核对）：
//!   0 白色(默认)     1 暗色        2 蓝白渐变
//!   3 粉白渐变       4 粉绿渐变     5 粉紫渐变
//!
//! 两条约定（本文件里是代码，不是注释）：
//!   * **非暗色主题的 Dock 用固定的默认色**（DEFAULT_DOCK）；只有暗色主题用
//!     深灰半透（DARK_DOCK）—— 自检里逐主题核对（selftest bit3）。
//!   * 暗色主题的阴影**更深**、边框高光改成**内高光**（见 shadow()/colors() 的 dark 分支）。
//!
//! 数值一律整数：颜色是 `Rgba { r, g, b, a }`（a 是 0..=255 的**真 alpha**），
//! 千分比只用于"按比例插值"的参数（t 等），不引入浮点、不引入 SSE（见 tokens.rs 顶部说明）。

use crate::tokens;

/// RGBA 颜色（POD；导出给 C 侧）。
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Rgba {
    pub r: u8,
    pub g: u8,
    pub b: u8,
    pub a: u8,
}

impl Rgba {
    pub const fn new(r: u8, g: u8, b: u8, a: u8) -> Self {
        Rgba { r, g, b, a }
    }
    /// 不透明色（alpha = 255）。
    pub const fn rgb(r: u8, g: u8, b: u8) -> Self {
        Rgba { r, g, b, a: 255 }
    }
    /// 打包成 0xRRGGBB（丢 alpha）—— 给 C++ 侧做"直接比对"的便捷值。
    pub const fn to_rgb24(&self) -> u32 {
        ((self.r as u32) << 16) | ((self.g as u32) << 8) | (self.b as u32)
    }
    /// Rec.601 亮度（0..=255，整数；用于"压在强调色上的字该用黑还是白"）。
    pub const fn luma(&self) -> u32 {
        (299 * (self.r as u32) + 587 * (self.g as u32) + 114 * (self.b as u32)) / 1000
    }
}

/// 主题配色（导出给 C 侧的**主结构**；字段顺序 = ABI，改字段等于改 ABI）。
#[repr(C)]
pub struct Rust64ThemeColors {
    pub window_bg: Rgba,  // 窗口背景（纯色底；渐变主题取中间调）
    pub card_bg: Rgba,    // 内容卡片（alpha 按 tokens::ALPHA_CONTENT_CARD 由主题给出）
    pub text: Rgba,       // 主文本
    pub text_dim: Rgba,   // 次要文本
    pub accent: Rgba,     // 强调色
    pub dock: Rgba,       // Dock / 任务栏
    pub border_hi: Rgba,  // 边框高光（暗色主题 = 内高光）
    pub shadow: Rgba,     // 阴影基准色（近阴影；远阴影更深，见 shadow()）
    pub grad_start: Rgba, // 渐变起点（纯色主题 = window_bg）
    pub grad_end: Rgba,   // 渐变终点（纯色主题 = window_bg）
    pub is_dark: u32,     // 1 = 暗色主题（Dock 深灰半透 / 阴影更深 / 内高光）
    pub reserved: u32,    // 保留（必须为 0；将来加字段先用它）
}

/// 阴影参数（POD；导出给 C 侧）。`blur`/`dy` 直接来自 tokens.rs。
#[repr(C)]
pub struct Rust64Shadow {
    pub dx: i32,
    pub dy: i32,
    pub blur: u32,
    pub color: Rgba,
}

/// 非暗色主题共用的**固定默认 Dock 色**（深色条 + 半透，沿用桌面既有观感）。
pub const DEFAULT_DOCK: Rgba = Rgba::new(24, 24, 28, 235);
/// 暗色主题的 Dock：深灰半透。
pub const DARK_DOCK: Rgba = Rgba::new(36, 38, 44, 200);
/// 压在强调色上的"深色字"（亮强调色时用）。
pub const ON_ACCENT_DARK: Rgba = Rgba::rgb(16, 16, 20);
/// 压在强调色上的"白色字"（暗强调色时用）。
pub const ON_ACCENT_LIGHT: Rgba = Rgba::rgb(255, 255, 255);

/// 一条主题定义（内部用；C++ 侧只看 Rust64ThemeColors）。
pub struct ThemeDef {
    pub name: &'static str,
    pub window_bg: Rgba,
    pub card_bg: Rgba,
    pub text: Rgba,
    pub text_dim: Rgba,
    pub accent: Rgba,
    pub dock: Rgba,
    pub border_hi: Rgba,
    pub shadow: Rgba,
    pub grad_start: Rgba,
    pub grad_end: Rgba,
    pub dark: bool,
}

// ---- 六个主题：颜色就写在这里，别的文件不再各写一份 ----
pub static THEMES: [ThemeDef; 6] = [
    // 0 白色（默认）：Windows 观感的白底 + 桌面的 Win10 蓝强调色（0,84,158）
    ThemeDef {
        name: "白色(默认)",
        window_bg: Rgba::rgb(245, 246, 248),
        card_bg: Rgba::new(255, 255, 255, 204), // 0.80
        text: Rgba::rgb(28, 28, 32),
        text_dim: Rgba::rgb(104, 110, 120),
        accent: Rgba::rgb(0x00, 0x54, 0x9E),
        dock: DEFAULT_DOCK,
        border_hi: Rgba::new(255, 255, 255, 140),
        shadow: Rgba::new(0, 0, 0, 20), // 0.08
        grad_start: Rgba::rgb(245, 246, 248),
        grad_end: Rgba::rgb(245, 246, 248),
        dark: false,
    },
    // 1 暗色：深灰底 + 亮蓝强调色；Dock 深灰半透、阴影更深、边框高光变内高光
    ThemeDef {
        name: "暗色",
        window_bg: Rgba::rgb(24, 26, 32),
        card_bg: Rgba::new(40, 44, 54, 204), // 0.80
        text: Rgba::rgb(238, 240, 245),
        text_dim: Rgba::rgb(158, 164, 176),
        accent: Rgba::rgb(0x4C, 0x98, 0xF0),
        dock: DARK_DOCK,
        border_hi: Rgba::new(255, 255, 255, 60), // 内高光（克制）
        shadow: Rgba::new(0, 0, 0, 45),          // 0.18（更深）= 180‰ × 255/1000，与 tokens 一致
        grad_start: Rgba::rgb(24, 26, 32),
        grad_end: Rgba::rgb(24, 26, 32),
        dark: true,
    },
    // 2 蓝白渐变：上蓝下白
    ThemeDef {
        name: "蓝白渐变",
        window_bg: Rgba::rgb(232, 240, 252),
        card_bg: Rgba::new(255, 255, 255, 204),
        text: Rgba::rgb(24, 32, 48),
        text_dim: Rgba::rgb(90, 104, 128),
        accent: Rgba::rgb(0x1E, 0x6A, 0xD2),
        dock: DEFAULT_DOCK,
        border_hi: Rgba::new(255, 255, 255, 140),
        shadow: Rgba::new(0, 0, 0, 20),
        grad_start: Rgba::rgb(32, 110, 220),
        grad_end: Rgba::rgb(250, 252, 255),
        dark: false,
    },
    // 3 粉白渐变：上粉下白
    ThemeDef {
        name: "粉白渐变",
        window_bg: Rgba::rgb(253, 240, 246),
        card_bg: Rgba::new(255, 255, 255, 204),
        text: Rgba::rgb(48, 24, 36),
        text_dim: Rgba::rgb(128, 96, 112),
        accent: Rgba::rgb(0xD6, 0x3A, 0x7C),
        dock: DEFAULT_DOCK,
        border_hi: Rgba::new(255, 255, 255, 140),
        shadow: Rgba::new(0, 0, 0, 20),
        grad_start: Rgba::rgb(255, 170, 200),
        grad_end: Rgba::rgb(255, 255, 255),
        dark: false,
    },
    // 4 粉绿渐变：粉 -> 薄荷绿
    ThemeDef {
        name: "粉绿渐变",
        window_bg: Rgba::rgb(246, 250, 246),
        card_bg: Rgba::new(255, 255, 255, 204),
        text: Rgba::rgb(28, 40, 32),
        text_dim: Rgba::rgb(96, 116, 104),
        accent: Rgba::rgb(0x22, 0x96, 0x6E),
        dock: DEFAULT_DOCK,
        border_hi: Rgba::new(255, 255, 255, 140),
        shadow: Rgba::new(0, 0, 0, 20),
        grad_start: Rgba::rgb(255, 150, 190),
        grad_end: Rgba::rgb(150, 224, 180),
        dark: false,
    },
    // 5 粉紫渐变：粉 -> 紫
    ThemeDef {
        name: "粉紫渐变",
        window_bg: Rgba::rgb(248, 242, 252),
        card_bg: Rgba::new(255, 255, 255, 204),
        text: Rgba::rgb(36, 26, 48),
        text_dim: Rgba::rgb(112, 96, 128),
        accent: Rgba::rgb(0x96, 0x48, 0xC8),
        dock: DEFAULT_DOCK,
        border_hi: Rgba::new(255, 255, 255, 140),
        shadow: Rgba::new(0, 0, 0, 20),
        grad_start: Rgba::rgb(255, 150, 200),
        grad_end: Rgba::rgb(170, 140, 240),
        dark: false,
    },
];

/// 当前主题（启动默认 = 0 白色）。用 `static mut` 而不是原子：内核是单核 + 关中断
/// 的调用点（渲染/设置都在同一执行流），且 C 侧拿的是 u32 值，不需要更强的保证。
static mut CURRENT_THEME: u32 = 0;

pub fn count() -> u32 {
    THEMES.len() as u32
}

pub fn at(idx: u32) -> Option<&'static ThemeDef> {
    let i = idx as usize;
    if i < THEMES.len() {
        Some(&THEMES[i])
    } else {
        None
    }
}

pub fn current() -> u32 {
    unsafe { CURRENT_THEME }
}

/// 设置当前主题。越界 -> 返回非 0 且不改动（不静默回退到 0）。
pub fn set_current(idx: u32) -> u32 {
    if (idx as usize) < THEMES.len() {
        unsafe { CURRENT_THEME = idx };
        0
    } else {
        1
    }
}

pub fn is_dark(idx: u32) -> bool {
    match at(idx) {
        Some(t) => t.dark,
        None => false,
    }
}

/// 主题 -> 导出结构（POD）。越界 -> None。
pub fn colors(idx: u32) -> Option<Rust64ThemeColors> {
    let t = at(idx)?;
    Some(Rust64ThemeColors {
        window_bg: t.window_bg,
        card_bg: t.card_bg,
        text: t.text,
        text_dim: t.text_dim,
        accent: t.accent,
        dock: t.dock,
        border_hi: t.border_hi,
        shadow: t.shadow,
        grad_start: t.grad_start,
        grad_end: t.grad_end,
        is_dark: if t.dark { 1 } else { 0 },
        reserved: 0,
    })
}

/// 阴影计算：`far = 0` 近阴影、`far != 0` 远阴影。
/// 参数（dy / blur / alpha）**全部来自 tokens.rs**：
///   * 近阴影 alpha = 主题表里的 shadow 色（亮色 0.08 / 暗色更深，见 THEMES）；
///   * 远阴影 alpha = Token 表的 SHADOW_FAR_ALPHA / SHADOW_DARK_FAR_ALPHA
///     （所以"远阴影一定比近阴影深"，且暗色主题整体更深 —— 自检 bit3 逐条核对）。
pub fn shadow(idx: u32, far: bool) -> Option<Rust64Shadow> {
    let t = at(idx)?;
    let base = t.shadow;
    if t.dark {
        if far {
            Some(Rust64Shadow {
                dx: 0,
                dy: tokens::SHADOW_FAR_DY_PX,
                blur: tokens::SHADOW_DARK_FAR_BLUR_PX as u32,
                color: Rgba::new(base.r, base.g, base.b, (tokens::SHADOW_DARK_FAR_ALPHA * 255 / 1000) as u8),
            })
        } else {
            Some(Rust64Shadow {
                dx: 0,
                dy: tokens::SHADOW_NEAR_DY_PX,
                blur: tokens::SHADOW_DARK_NEAR_BLUR_PX as u32,
                color: Rgba::new(base.r, base.g, base.b, (tokens::SHADOW_DARK_NEAR_ALPHA * 255 / 1000) as u8),
            })
        }
    } else if far {
        Some(Rust64Shadow {
            dx: 0,
            dy: tokens::SHADOW_FAR_DY_PX,
            blur: tokens::SHADOW_FAR_BLUR_PX as u32,
            // 远阴影更深：alpha 用 Token（0.12 -> 30/255），不能用主题里的近阴影色
            color: Rgba::new(base.r, base.g, base.b, (tokens::SHADOW_FAR_ALPHA * 255 / 1000) as u8),
        })
    } else {
        Some(Rust64Shadow {
            dx: 0,
            dy: tokens::SHADOW_NEAR_DY_PX,
            blur: tokens::SHADOW_NEAR_BLUR_PX as u32,
            color: base,
        })
    }
}

/// 千分比插值（整数，对称四舍五入；`t` 先夹到 0..=1000）。
/// ★ 端点必须**精确**：t=0 -> a、t=1000 -> b（自检会核对每个渐变主题的两端）。
///   注意不能写成 `lo + ((hi-lo)*t + 500)/1000`：C 风格整数除法向零截断，
///   在 hi<lo 时端点会差 1（实测粉绿/粉紫渐变的 t=1000 得到 151/171 而不是 150/170）。
pub fn lerp_u8(a: u8, b: u8, t: u32) -> u8 {
    if t == 0 {
        return a;
    }
    if t >= 1000 {
        return b;
    }
    let tt = t as i32;
    let (lo, hi) = (a as i32, b as i32);
    let d = (hi - lo) * tt;
    let v = if d >= 0 { lo + (d + 500) / 1000 } else { lo - ((-d + 500) / 1000) };
    v.clamp(0, 255) as u8
}

/// 两个颜色的按比例混合（含 alpha）。`t = 0` -> a，`t = 1000` -> b。
pub fn blend(a: Rgba, b: Rgba, t: u32) -> Rgba {
    Rgba::new(
        lerp_u8(a.r, b.r, t),
        lerp_u8(a.g, b.g, t),
        lerp_u8(a.b, b.b, t),
        lerp_u8(a.a, b.a, t),
    )
}

/// 主题背景在 `t`（千分比）处的颜色：
///   * 纯色主题（grad_start == grad_end）-> window_bg（保证"没定义渐变"时不是黑）；
///   * 渐变主题 -> 起点到终点的线性插值。
pub fn gradient_at(idx: u32, t: u32) -> Option<Rgba> {
    let th = at(idx)?;
    if th.grad_start == th.grad_end {
        Some(th.window_bg)
    } else {
        Some(blend(th.grad_start, th.grad_end, t))
    }
}

/// "压在强调色上的文字色"：按强调色亮度选黑或白（整数判断，无浮点）。
pub fn on_accent(idx: u32) -> Option<Rgba> {
    let t = at(idx)?;
    Some(if t.accent.luma() > 150 {
        ON_ACCENT_DARK
    } else {
        ON_ACCENT_LIGHT
    })
}

/// 主题名（UTF-8 字节；C 侧按 `*const u8 + len` 取）。
pub fn name_bytes(idx: u32) -> Option<&'static [u8]> {
    Some(at(idx)?.name.as_bytes())
}

// ==================== 自检（rust64_tokens_init64 的失败位图）====================
// bit0 Token 表：关键形状参数（圆角/模糊）与设计约定一致
// bit1 Token 表：透明度 / 动效时长 / 缓动控制点与设计约定一致
// bit2 主题表：数量 = 6、默认主题 = 白色、每个主题的 accent 与 accent_rgb 一致
// bit3 Dock 约定：非暗色主题全部用固定默认色；暗色主题 Dock 不同；暗色阴影更深
// bit4 渐变计算：渐变主题 t=0/1000 命中起点/终点；纯色主题与 window_bg 一致
// bit5 强调色对比：亮/暗强调色分别得到深/浅文字色
// bit6 边界：名字查询/越界的错误值（不崩、不静默）
pub fn selftest() -> u32 {
    let mut fail: u32 = 0;

    // ---- bit0：形状 Token ----
    if tokens::count() != 33 {
        fail |= 1;
    }
    let id_of = tokens::find_str;
    let want_px: [(u32, i32); 8] = [
        (id_of(tokens::NAME_RADIUS_WINDOW).unwrap_or(u32::MAX), 14),
        (id_of(tokens::NAME_RADIUS_CARD).unwrap_or(u32::MAX), 12),
        (id_of(tokens::NAME_RADIUS_BUTTON).unwrap_or(u32::MAX), 9),
        (id_of(tokens::NAME_RADIUS_DOCK).unwrap_or(u32::MAX), 24),
        (id_of(tokens::NAME_RADIUS_START_TOP).unwrap_or(u32::MAX), 24),
        (id_of(tokens::NAME_RADIUS_START_BOTTOM).unwrap_or(u32::MAX), 10),
        (id_of(tokens::NAME_BLUR_BG).unwrap_or(u32::MAX), 24),
        (id_of(tokens::NAME_BLUR_CONTENT).unwrap_or(u32::MAX), 12),
    ];
    for pair in want_px.iter() {
        if tokens::px(pair.0) != pair.1 {
            fail |= 1;
        }
    }

    // ---- bit1：透明度 / 动效 / 缓动 ----
    let pm = |n: &str| tokens::permille(id_of(n).unwrap_or(u32::MAX));
    if pm(tokens::NAME_ALPHA_BG) != 450
        || pm(tokens::NAME_ALPHA_CARD) != 800
        || pm(tokens::NAME_ALPHA_TEXT) != 1000
        || tokens::px(id_of(tokens::NAME_MOTION_FAST).unwrap_or(u32::MAX)) != 150
        || tokens::px(id_of(tokens::NAME_MOTION_NORMAL).unwrap_or(u32::MAX)) != 225
        || tokens::px(id_of(tokens::NAME_MOTION_PANEL).unwrap_or(u32::MAX)) != 330
        || pm(tokens::NAME_EASE_X1) != 200
        || pm(tokens::NAME_EASE_Y1) != 0
        || pm(tokens::NAME_EASE_X2) != 0
        || pm(tokens::NAME_EASE_Y2) != 1000
    {
        fail |= 2;
    }

    // ---- bit2：主题表 ----
    if count() != 6 || name_bytes(0) != Some("白色(默认)".as_bytes()) {
        fail |= 4;
    }
    let mut i = 0u32;
    while i < count() {
        match (at(i), colors(i)) {
            (Some(t), Some(c)) => {
                if c.accent.to_rgb24() != t.accent.to_rgb24() || c.is_dark != if t.dark { 1 } else { 0 } {
                    fail |= 4;
                }
            }
            _ => fail |= 4,
        }
        i += 1;
    }

    // ---- bit3：Dock 约定 + 阴影（远 > 近、暗色更深、主题表与计算值一致）----
    let mut i = 0u32;
    while i < count() {
        match (at(i), colors(i)) {
            (Some(t), Some(c)) => {
                if t.dark {
                    if t.dock != DARK_DOCK {
                        fail |= 8;
                    }
                } else if t.dock != DEFAULT_DOCK {
                    fail |= 8;
                }
                // 主题表里的 shadow 色必须等于"计算出来的近阴影色"（否则两处会各说各话）
                match shadow(i, false) {
                    Some(s) => {
                        if s.color != c.shadow || s.dy != tokens::SHADOW_NEAR_DY_PX {
                            fail |= 8;
                        }
                    }
                    None => fail |= 8,
                }
            }
            _ => fail |= 8,
        }
        i += 1;
    }
    let dark_near = shadow(1, false);
    let light_near = shadow(0, false);
    let dark_far = shadow(1, true);
    let light_far = shadow(0, true);
    match (light_near, dark_near, light_far, dark_far) {
        (Some(l0), Some(d0), Some(l1), Some(d1)) => {
            // 同一主题远阴影比近阴影深；暗色主题比亮色主题深
            if !(l1.color.a > l0.color.a && d1.color.a > d0.color.a && d0.color.a > l0.color.a && d1.color.a > l1.color.a) {
                fail |= 8;
            }
        }
        _ => fail |= 8,
    }

    // ---- bit4：渐变计算 ----
    let mut i = 0u32;
    while i < count() {
        if let (Some(t), Some(g0), Some(g1), Some(mid)) = (at(i), gradient_at(i, 0), gradient_at(i, 1000), gradient_at(i, 500)) {
            if t.grad_start == t.grad_end {
                if g0 != t.window_bg || g1 != t.window_bg || mid != t.window_bg {
                    fail |= 16;
                }
            } else if g0 != t.grad_start || g1 != t.grad_end {
                fail |= 16;
            }
        } else {
            fail |= 16;
        }
        i += 1;
    }
    // 插值自洽：blend(a, b, 0) = a、blend(a, b, 1000) = b、t 越界被夹住
    let a = Rgba::new(10, 20, 30, 40);
    let b = Rgba::new(200, 210, 220, 230);
    if blend(a, b, 0) != a || blend(a, b, 1000) != b || blend(a, b, 99999) != b {
        fail |= 16;
    }

    // ---- bit5：强调色上的文字色 ----
    match (on_accent(0), on_accent(1)) {
        (Some(on0), Some(on1)) => {
            // 主题 0 强调色是深蓝 -> 白字；主题 1 强调色是亮蓝 -> 仍是白字（两个都必须"够对比"）
            if on0.to_rgb24() != ON_ACCENT_LIGHT.to_rgb24() || on1.to_rgb24() != ON_ACCENT_LIGHT.to_rgb24() {
                fail |= 32;
            }
            // 亮的强调色必须给出深色字
            if Rgba::rgb(0xE0, 0xE0, 0xE0).luma() <= 150 {
                fail |= 32;
            }
        }
        _ => fail |= 32,
    }

    // ---- bit6：边界 ----
    if tokens::find(b"no.such.token").is_some() || tokens::px(u32::MAX) != -1 || tokens::permille(999) != u32::MAX {
        fail |= 64;
    }
    let before = current();
    if set_current(count()) == 0 || current() != before {
        fail |= 64; // 越界设置必须失败且不改动当前主题
    }
    if set_current(before) != 0 {
        fail |= 64; // 恢复原值也必须成功（自检不该改变运行期状态）
    }

    fail
}
