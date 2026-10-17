#pragma once
// ==================== 最小 fontconfig（v0.4.8）====================
// 用户要求"引入 fontconfig 来管理字体"。本内核没有动态库可链，所以做**等价的最小实现**：
//   * 配置文件 = 系统卷里的 `/etc/fonts.conf`（纯文本 `key = value`，人可读可改）；
//   * 内核启动时读它并按里面的策略设置字体渲染参数；文件不存在就**幂等种一份默认**（与
//     app64 种 /etc/apps.json 同一套做法，失败只打点、绝不 PANIC）；
//   * 角色 -> 字体面的映射同样由它决定，`fontcfg64_role_face64()` 给调用方查。
//
// ★ 如实边界（别当成真 fontconfig）：
//   - 不支持 XDG 的 fonts.conf 语法（没有 <match>/<test>/<edit> 规则引擎、没有别名回退链的递归匹配）；
//   - 不做字体枚举/扫描：字体面是**编译期内嵌的 4 个**（ASCII/CJK/MONO/FALLBACK），这里只能选、不能加载新字体；
//   - 没有 fc-cache / 缓存文件；没有可变字体、没有字体 fallback 的 per-language 规则。
int  fontcfg64_init64();                 // 读 /etc/fonts.conf（缺了就写默认）；返回 0 = ok，<0 = 文件层不可用
const char* fontcfg64_path64();          // "/etc/fonts.conf"
int  fontcfg64_stem64();                 // UI 笔画加厚百分比（100 = 不加厚，默认 120）
int  fontcfg64_aa64();                   // UI 是否抗锯齿（1 = 开）
int  fontcfg64_role_face64(const char* role);   // "ui"/"mono"/"cjk"/"fallback" -> face 索引，未知 -> -1
const char* fontcfg64_role_family64(const char* role);  // 配置里写的族名（打点/验收用）
