//! tokens.rs - VimtuOS GUI 设计 Token 表（Design Tokens）—— **唯一真源**
//!
//! 为什么要放在 Rust 里：GUI 主体语言已经定为 Rust，设计 Token 是所有视觉参数
//! （圆角 / 模糊半径 / 透明度 / 阴影 / 动效时长 / 缓动曲线 / 字号档位）的**唯一真源**。
//! C/C++ 侧（渲染原语、外壳）不再各写一份常量，而是通过 `rust64_token_*64()`
//! 按名字查询（见 kernel/rust64.h）。
//!
//! 三条硬约束（决定了本文件的写法）：
//!   1. **no_std / 无 alloc**：只用 `static` 常量表 + 整数运算；
//!   2. **不引入浮点**：内核 C++ 侧用 `-mno-sse` 编译、引导链没有打开 CR4.OSFXSR，
//!      所以在 Rust 侧一律用**整数定点**表示小数：
//!        * 透明度 / 缓动控制点 / 比例 -> 千分比（permille，0..=1000，1000 = 1.0）；
//!        * 长度 / 时长 -> 整数（px / ms）。
//!      （设计稿里的 0.45 = 450‰、0.80 = 800‰、cubic-bezier(0.2,0,0,1) = 200/0/0/1000）
//!   3. **POD only**：导出到 C 的类型全是 `#[repr(C)]`，字符串用 `*const u8 + len`。
//!
//! 与"视觉线"的约定（数值一旦定下来只增不改）：
//!   圆角   窗口 14 / 卡片 12 / 按钮 9 / Dock 24 / 开始菜单 上 24 下 10
//!   模糊   背景 24 / 内容 12
//!   透明   背景材质 0.45 / 内容卡片 0.80 / 文字 1.0
//!   阴影   近 0 2 4 rgba(0,0,0,0.08)、远 0 12 32 rgba(0,0,0,0.12)；暗色更深 + 内高光
//!   动效   快 150ms / 普通 225ms / 大面板 330ms；缓动 cubic-bezier(0.2, 0, 0, 1)

/// Token 单位种类（`TokenDef.unit`）。用 u8 常量而不是 enum：静态表里布局最直白。
pub const UNIT_PX: u8 = 0; // 长度：`value`（i32）有效
pub const UNIT_MS: u8 = 1; // 时长：`value`（i32）有效
pub const UNIT_PERMILLE: u8 = 2; // 千分比：`permille`（u32）有效

/// 一条设计 Token。名字是**稳定接口**（C++ 侧按名字查），id 是数组下标（只增不改）。
#[repr(C)]
pub struct TokenDef {
    pub name: &'static str,
    pub unit: u8,
    pub value: i32,    // px / ms
    pub permille: u32, // 0..=1000
}

// ---- 名字（C++ / 测试都用这些字符串查，改名字等于改接口）----
pub const NAME_RADIUS_WINDOW: &str = "radius.window";
pub const NAME_RADIUS_CARD: &str = "radius.card";
pub const NAME_RADIUS_BUTTON: &str = "radius.button";
pub const NAME_RADIUS_DOCK: &str = "radius.dock";
pub const NAME_RADIUS_START_TOP: &str = "radius.start_menu_top";
pub const NAME_RADIUS_START_BOTTOM: &str = "radius.start_menu_bottom";
pub const NAME_BLUR_BG: &str = "blur.background";
pub const NAME_BLUR_CONTENT: &str = "blur.content";
pub const NAME_ALPHA_BG: &str = "alpha.backdrop_material";
pub const NAME_ALPHA_CARD: &str = "alpha.content_card";
pub const NAME_ALPHA_TEXT: &str = "alpha.text";
pub const NAME_MOTION_FAST: &str = "motion.fast_ms";
pub const NAME_MOTION_NORMAL: &str = "motion.normal_ms";
pub const NAME_MOTION_PANEL: &str = "motion.panel_ms";
pub const NAME_EASE_X1: &str = "ease.x1";
pub const NAME_EASE_Y1: &str = "ease.y1";
pub const NAME_EASE_X2: &str = "ease.x2";
pub const NAME_EASE_Y2: &str = "ease.y2";

// ---- 值（供本模块内部与其他 Rust 模块直接引用；C++ 侧走查询接口）----
pub const RADIUS_WINDOW_PX: i32 = 14;
pub const RADIUS_CARD_PX: i32 = 12;
pub const RADIUS_BUTTON_PX: i32 = 9;
pub const RADIUS_DOCK_PX: i32 = 24;
pub const RADIUS_START_TOP_PX: i32 = 24;
pub const RADIUS_START_BOTTOM_PX: i32 = 10;
pub const BLUR_BACKGROUND_PX: i32 = 24;
pub const BLUR_CONTENT_PX: i32 = 12;
pub const ALPHA_BACKDROP_MATERIAL: u32 = 450; // 0.45
pub const ALPHA_CONTENT_CARD: u32 = 800; // 0.80
pub const ALPHA_TEXT: u32 = 1000; // 1.00
pub const SHADOW_NEAR_DY_PX: i32 = 2;
pub const SHADOW_NEAR_BLUR_PX: i32 = 4;
pub const SHADOW_NEAR_ALPHA: u32 = 80; // rgba(0,0,0,0.08)
pub const SHADOW_FAR_DY_PX: i32 = 12;
pub const SHADOW_FAR_BLUR_PX: i32 = 32;
pub const SHADOW_FAR_ALPHA: u32 = 120; // rgba(0,0,0,0.12)
/// 暗色主题"更深"：近阴影 0.08 -> 0.18、远阴影 0.12 -> 0.28（模糊半径也各加一点）。
pub const SHADOW_DARK_NEAR_ALPHA: u32 = 180;
pub const SHADOW_DARK_FAR_ALPHA: u32 = 280;
pub const SHADOW_DARK_NEAR_BLUR_PX: i32 = 6;
pub const SHADOW_DARK_FAR_BLUR_PX: i32 = 36;
/// 暗色主题的"内高光"（卡片/窗口内侧上沿的一条亮线，见 theme.rs 的 border_hi）。
pub const INNER_HIGHLIGHT_LIGHT: u32 = 140; // 亮色主题：白色 55% 高光
pub const INNER_HIGHLIGHT_DARK: u32 = 60; // 暗色主题：内高光更克制（上方白色 24%）
pub const MOTION_FAST_MS: i32 = 150;
pub const MOTION_NORMAL_MS: i32 = 225;
pub const MOTION_PANEL_MS: i32 = 330;
/// cubic-bezier(0.2, 0, 0, 1) 的四个控制点（千分比）
pub const EASE_X1: u32 = 200;
pub const EASE_Y1: u32 = 0;
pub const EASE_X2: u32 = 0;
pub const EASE_Y2: u32 = 1000;

/// 字号档位（px）。当前内核只光栅化了一种字形尺寸（body = 14，见 gui64.cpp 的
/// `(TITLE_H - 14) / 2`），这些档位是给"多字号渲染"预留的设计约定。
pub const FONT_CAPTION_PX: i32 = 12;
pub const FONT_BODY_PX: i32 = 14;
pub const FONT_TITLE_PX: i32 = 18;
pub const FONT_HEADING_PX: i32 = 22;
pub const FONT_DISPLAY_PX: i32 = 28;

/// Token 表本体（id = 下标；**只增不改**，C++ 侧不要缓存数值以外的假设）。
pub static TOKENS: [TokenDef; 33] = [
    TokenDef { name: NAME_RADIUS_WINDOW, unit: UNIT_PX, value: RADIUS_WINDOW_PX, permille: 0 },
    TokenDef { name: NAME_RADIUS_CARD, unit: UNIT_PX, value: RADIUS_CARD_PX, permille: 0 },
    TokenDef { name: NAME_RADIUS_BUTTON, unit: UNIT_PX, value: RADIUS_BUTTON_PX, permille: 0 },
    TokenDef { name: NAME_RADIUS_DOCK, unit: UNIT_PX, value: RADIUS_DOCK_PX, permille: 0 },
    TokenDef { name: NAME_RADIUS_START_TOP, unit: UNIT_PX, value: RADIUS_START_TOP_PX, permille: 0 },
    TokenDef { name: NAME_RADIUS_START_BOTTOM, unit: UNIT_PX, value: RADIUS_START_BOTTOM_PX, permille: 0 },
    TokenDef { name: NAME_BLUR_BG, unit: UNIT_PX, value: BLUR_BACKGROUND_PX, permille: 0 },
    TokenDef { name: NAME_BLUR_CONTENT, unit: UNIT_PX, value: BLUR_CONTENT_PX, permille: 0 },
    TokenDef { name: NAME_ALPHA_BG, unit: UNIT_PERMILLE, value: 0, permille: ALPHA_BACKDROP_MATERIAL },
    TokenDef { name: NAME_ALPHA_CARD, unit: UNIT_PERMILLE, value: 0, permille: ALPHA_CONTENT_CARD },
    TokenDef { name: NAME_ALPHA_TEXT, unit: UNIT_PERMILLE, value: 0, permille: ALPHA_TEXT },
    TokenDef { name: "shadow.near.offset_y", unit: UNIT_PX, value: SHADOW_NEAR_DY_PX, permille: 0 },
    TokenDef { name: "shadow.near.blur", unit: UNIT_PX, value: SHADOW_NEAR_BLUR_PX, permille: 0 },
    TokenDef { name: "shadow.near.alpha", unit: UNIT_PERMILLE, value: 0, permille: SHADOW_NEAR_ALPHA },
    TokenDef { name: "shadow.far.offset_y", unit: UNIT_PX, value: SHADOW_FAR_DY_PX, permille: 0 },
    TokenDef { name: "shadow.far.blur", unit: UNIT_PX, value: SHADOW_FAR_BLUR_PX, permille: 0 },
    TokenDef { name: "shadow.far.alpha", unit: UNIT_PERMILLE, value: 0, permille: SHADOW_FAR_ALPHA },
    TokenDef { name: "shadow.dark.near.alpha", unit: UNIT_PERMILLE, value: 0, permille: SHADOW_DARK_NEAR_ALPHA },
    TokenDef { name: "shadow.dark.far.alpha", unit: UNIT_PERMILLE, value: 0, permille: SHADOW_DARK_FAR_ALPHA },
    TokenDef { name: "shadow.dark.near.blur", unit: UNIT_PX, value: SHADOW_DARK_NEAR_BLUR_PX, permille: 0 },
    TokenDef { name: "shadow.dark.far.blur", unit: UNIT_PX, value: SHADOW_DARK_FAR_BLUR_PX, permille: 0 },
    TokenDef { name: NAME_MOTION_FAST, unit: UNIT_MS, value: MOTION_FAST_MS, permille: 0 },
    TokenDef { name: NAME_MOTION_NORMAL, unit: UNIT_MS, value: MOTION_NORMAL_MS, permille: 0 },
    TokenDef { name: NAME_MOTION_PANEL, unit: UNIT_MS, value: MOTION_PANEL_MS, permille: 0 },
    TokenDef { name: NAME_EASE_X1, unit: UNIT_PERMILLE, value: 0, permille: EASE_X1 },
    TokenDef { name: NAME_EASE_Y1, unit: UNIT_PERMILLE, value: 0, permille: EASE_Y1 },
    TokenDef { name: NAME_EASE_X2, unit: UNIT_PERMILLE, value: 0, permille: EASE_X2 },
    TokenDef { name: NAME_EASE_Y2, unit: UNIT_PERMILLE, value: 0, permille: EASE_Y2 },
    TokenDef { name: "font.caption_px", unit: UNIT_PX, value: FONT_CAPTION_PX, permille: 0 },
    TokenDef { name: "font.body_px", unit: UNIT_PX, value: FONT_BODY_PX, permille: 0 },
    TokenDef { name: "font.title_px", unit: UNIT_PX, value: FONT_TITLE_PX, permille: 0 },
    TokenDef { name: "font.heading_px", unit: UNIT_PX, value: FONT_HEADING_PX, permille: 0 },
    TokenDef { name: "font.display_px", unit: UNIT_PX, value: FONT_DISPLAY_PX, permille: 0 },
];

/// 表长度（C++ 侧不用猜，直接问）。
pub fn count() -> u32 {
    TOKENS.len() as u32
}

/// 按下标取（越界 -> None）。
pub fn at(id: u32) -> Option<&'static TokenDef> {
    let i = id as usize;
    if i < TOKENS.len() {
        Some(&TOKENS[i])
    } else {
        None
    }
}

/// 按名字查 id（线性扫描：33 条，够快；查不到的返回 None）。
pub fn find(name: &[u8]) -> Option<u32> {
    let mut i = 0usize;
    while i < TOKENS.len() {
        let n = TOKENS[i].name.as_bytes();
        if n.len() == name.len() {
            let mut j = 0usize;
            let mut same = true;
            while j < n.len() {
                if n[j] != name[j] {
                    same = false;
                    break;
                }
                j += 1;
            }
            if same {
                return Some(i as u32);
            }
        }
        i += 1;
    }
    None
}

/// 按名字（`&str`）查 id —— 给 Rust 侧调用方用的便捷包装。
pub fn find_str(name: &str) -> Option<u32> {
    find(name.as_bytes())
}

/// px / ms 类 token 的取值。单位不符或越界 -> -1（明确的错误值，不静默给 0）。
pub fn px(id: u32) -> i32 {
    match at(id) {
        Some(t) if t.unit == UNIT_PX || t.unit == UNIT_MS => t.value,
        _ => -1,
    }
}

/// 千分比类 token 的取值。单位不符或越界 -> u32::MAX。
pub fn permille(id: u32) -> u32 {
    match at(id) {
        Some(t) if t.unit == UNIT_PERMILLE => t.permille,
        _ => u32::MAX,
    }
}

/// 名字查找失败时的返回值（与 px()/permille() 的错误值区分开）。
pub const LOOKUP_NOT_FOUND: i32 = -1;
