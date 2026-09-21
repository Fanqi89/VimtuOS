// explorer64.h - 文件资源管理器 / "此电脑"（Win10 风格的文件管理器 UI + 交互）
//
// 为什么单独一个文件（而不是塞进 gui64.cpp）：
//   gui64.cpp 是**窗口外壳**（桌面/任务栏/开始菜单/窗口管理/消息循环），应用本体一律拆到
//   各自 .cpp（calc64/mines64/terminal64/...）。文件管理器有 1000+ 行 UI 状态机，塞进外壳会
//   把外壳撑成第二个 32 位 gui.cpp（4214 行）。所以这里跟其它应用同规格：外壳只在
//   app_mypc_open64() 里转调 explorer64_open64()，窗口回调全部由本文件实现。
//
// ==================== 窗口结构与导航模型（一个窗口里导航）====================
//   本实现选**单窗口导航**（不做"每个目录一个新窗口"）：桌面"我的电脑"图标 / 开始菜单
//   "我的电脑" / 终端等所有入口都进同一个窗口，已经开着就激活它（Win10 也是如此）。
//   窗口客户区从上到下分四条带：
//     [工具栏 26px]   文件 | 计算机 | 查看     （固定宽度按钮：文件[8..72] 计算机[76..140]
//                                              查看[144..208]；点击 = 可见反馈：按下变深色 +
//                                              hover 高亮；文件弹出小菜单，计算机回到"此电脑"，
//                                              查看在 图标视图 <-> 详细信息视图 之间切换）
//     [地址栏 28px]   后退[6..32] 前进[34..60] 上级[62..88] + **面包屑**（x>=96，每段可点击）
//     [主体]          左：导航窗格 150px（快速访问 / 此电脑 / 每个可浏览驱动器一个节点）
//                     右：内容区（此电脑 = 设备与驱动器卡片；目录 = 图标视图 / 详细信息视图）
//     [状态栏 22px]   左下 "N 个项目"（有选中时追加 "| 选中: 名称 大小"）
//
//   导航状态 = (mode, 盘符, 路径)：
//     mode 0 = 此电脑（驱动器卡片页）；mode 1 = 目录浏览（path 是 VimtuFS2 绝对路径）
//   历史栈 = 16 条的 (mode, letter, path) 记录 + 游标：进入/跳转 = push（并截断"前进"分支），
//   后退/前进 = 移动游标后重放。**"上级"** 走父目录（根目录的上级 = 此电脑）。
//
// ==================== 与 vfs64/drive64 的关系（★ 多卷：真的能进 D:/E:）====================
//   * 驱动器列表来自 drive64_scan64()（只读探测 + 给每个可浏览卷分配一个 vfs64 卷槽）；
//     目录内容来自 vfs64_list64（游标分页，作用于**当前卷**）。
//   * **双击盘符卡片 = drive64_activate_letter64(letter)**：把该盘对应的卷槽激活成"当前卷"，
//     然后列它的根目录 —— 所以 C:/D:/E: 点进去看到的都是各自盘上的真实内容；失败时如实打点
//     `[UI] explorer enter letter=D: slot=2 FAILED reason=...` 并在状态栏说明，不假装能进。
//   * 要进 D: 的目录（而不是只列根目录），进盘前会先激活卷；浏览中如果别的组件（终端 `vol`）
//     换了当前卷，exp_refresh_dir 会按界面盘符把卷同步回来（打点 [UI] explorer volume sync ...）。
//   * ESP / 未识别文件系统的条目显示为灰字 + "（不浏览）"，不响应双击。
//
// ==================== 打点（自动验收 grep；格式勿改）====================
//   [UI] explorer thispc drives=<n> browsable=<n>
//   [UI] explorer drive letter=C: fs=VimtuFS2 total_kb=<n> free_kb=<n>
//   [UI] explorer card idx=<i> letter=<C:|-> kind=<browsable|skip> name=<..>[ reason=<esp|no-fs>]
//   [UI] explorer nav path=<path> items=<n> view=icons|details
//   [UI] explorer enter name=<n> kind=<dir|vap|elf|text|file|bin|unknown>
//   [UI] explorer run name=<n> kind=<vap|elf> rc=<n>
//   [UI] explorer preview name=<n> bytes=<n>
//   [UI] explorer noassoc name=<n> kind=<..>
//   [UI] explorer back path=<p> / [UI] explorer forward path=<p> / [UI] explorer up path=<p>
//   [UI] explorer enter letter=<C:|D:> slot=<n> ok items=<n>    （★ 多卷：真的切到该盘并列出内容）
//   [UI] explorer view=icons rows=<n> / [UI] explorer view=details rows=<n>
//   [UI] explorer scroll top=<n> items=<n>      （滚动/翻页，有界）
//   [UI] explorer click x=<cx> y=<cy> sx=<sx> sy=<sy> hit=<none|card:<i>|item:<i>|btn:<id>|crumb:<k>|nav:<id>>
//       （x/y = 客户区坐标，sx/sy = 屏幕坐标：自动验收的鼠标闭环定位靠它）
//   [EXPL] selftest PASS / [EXPL] selftest FAIL mask=<n>
#pragma once
#include <stdint.h>

struct Window;
// 打开文件管理器窗口（已存在则激活并刷新）。打印 "[APP] mypc opened"（历史断言依赖，勿改）。
// 打开发管理器窗口（已存在则激活并刷新）。打印 "[APP] mypc opened"（历史断言依赖，勿改）。
void explorer64_open64();

// 关闭窗口（会话策略/自检用）。
void explorer64_reset64();

// 当前窗口（没有 = nullptr）；外壳每秒刷新时钟时用它做局部重绘。
Window* explorer64_window64();

// 纯逻辑自检：单位换算 / 路径与面包屑切分 / 滚动钳制 / 历史栈（不建窗口、不碰盘）。
int explorer64_selftest64();
