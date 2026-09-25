// settings64.h - VimtuOS 设置应用（★ P3：Windows 11 风格，左导航 240px + 右卡片）
//
// 本文件只导出"应用入口以外的接线点"：
//   * app_settings_open64() / app_settings_reset64() 的声明在 gui64.h（外壳的应用契约），此处不重复；
//   * settings64_boot_apply64()：**启动期**把设置页持久化的项生效（字体大小档 / 自定义渐变壁纸），
//     由 kernel/gui64.cpp 的 gui64_run() 在 theme64_init64() + 壁纸装载之后调一次
//     （字体字号必须在任何界面绘制之前生效；自定义渐变壁纸必须晚于外壳的壁纸装载决定）。
#pragma once

// 启动期应用设置页的持久化项（幂等；可重复调用）：
//   1) font_set_size64(cfg64_font_size64())  —— 字体大小档（14/16/18；默认 16 = 历史值）
//   2) cfg64_grad_on64() 且没配壁纸文件时，用 ui.grad.a/ui.grad.b 造一张渐变壁纸当桌面
//      （gfx64_wall_set_source64；不打点，gfx64 自己的 [GFX64] wall 行就是证据）
//   打点：[SET64] boot apply font=<px> grad=<0|1> wall=<builtin|file|gradient>
void settings64_boot_apply64();
