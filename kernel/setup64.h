// setup64.h - Vimtu64 图形化安装程序（Win10 安装程序同款流程）
//
// 流程（对齐 Windows 10 安装程序，去掉"输入产品密钥"那一步）：
//   1) 语言选择      2) 现在安装      3) 许可条款
//   4) 安装类型      5) 磁盘与分区    6) 安装进度（进度条 + 百分比）  7) 完成并自动重启
//
// 运行环境：64 位内核早期（fb + font + input 已就绪，无任务系统）。
//          主循环是单线程事件循环，靠 PIT 中断（250Hz）驱动 hlt 唤醒。
#pragma once
#include "../bootinfo.h"

// 进入安装程序主循环（不返回；安装完成会重启机器）
[[noreturn]] void setup64_run(const BootInfo* bi);
