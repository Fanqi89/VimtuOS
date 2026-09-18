/* setup64_keys.h - 安装程序的"串口按键通道"（无人值守/自动化安装）
 *
 * 为什么要这条通道：
 *   * QEMU 的自动验收靠 monitor `sendkey`，VMware 想发键只能抢占它的窗口，而
 *     VMware Workstation 自带的 VNC 服务器是**只读**的（实测发 KeyEvent 客人收不到），
 *     真机更是没法脚本点按。走串口后，QEMU / VMware / 真机三类环境用同一条路径。
 *   * 真机可用：另一台机器接一根串口线就能驱动安装全过程（类似 Windows 的
 *     unattend.xml），也方便"装到一半看日志"。
 *
 * 约定：COM2(0x2F8) 收到的字节**原样**当按键 —— 回车 0x0D/0x0A、Esc 0x1B、
 *       字母/数字即字符；方向键用向导内部码 0xFD(上)/0xFE(下)/0xFB(左)/0xFC(右)。
 *       投递路径与真实键盘完全一致（同一个 handle_key），所以不需要两套逻辑。
 *
 * 注意：本文件在 setup64.cpp 的 handle_key 定义之后被 #include（不是编译单元），
 *       这样它能直接调用那个 static 函数；无需任何宏开关（setup64.cpp 只会被编进
 *       安装程序内核，不会进系统内核）。
 */
#pragma once

static void com2_init() {
    outb64(0x2F9, 0x00);   /* 关中断：这里轮询，不用 COM2 中断 */
    outb64(0x2FB, 0x80);   /* DLAB = 1 */
    outb64(0x2F8, 0x01);   /* 除数低字节 = 1 -> 115200 */
    outb64(0x2F9, 0x00);
    outb64(0x2FB, 0x03);   /* 8 位 / 无校验 / 1 停止位 */
    outb64(0x2FA, 0xC7);   /* FIFO 使能 + 清空 */
    outb64(0x2FC, 0x0B);   /* DTR / RTS */
    dbg64_str("[SETUP] 串口按键通道 COM2 就绪");
    dbg64_nl();
}

static void com2_poll_keys() {
    for (int guard = 0; guard < 64; guard++) {
        const uint8_t lsr = inb64(0x2FD);
        if (!(lsr & 0x01)) return;                            /* 没有收到数据 */
        if (lsr & 0x0E) { (void)inb64(0x2F8); continue; }      /* 帧/校验/溢出错误：读掉丢弃 */
        const uint8_t b = inb64(0x2F8);
        dbg64_str("[SETUP] com2key=0x");
        dbg64_hex64(b);
        dbg64_nl();
        handle_key(b);                                         /* 与真实键盘同一条路径 */
    }
}
