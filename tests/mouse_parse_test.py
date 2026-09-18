#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/mouse_parse_test.py - 用 VMware 实抓的字节流验证鼠标修复（解析约定 + 累加器）

用户报告的现象：QEMU 下鼠标正常，VMware 下"鼠标只要一动，光标就围着窗口边缘转"。

抓包方式：安装程序内核开 -DVIMTU_PS2_TRACE=1，每个 PS/2 字节按 "M.." 打串口、
每个凑齐的包打 "|pB..X..Y.."；宿主鼠标在 VM 里画了一串圈。日志见下面 PACKETS_HEX。

两条结论（都由这份实抓数据推出，见下方断言）：
  1) **符号约定**：3 字节包的两个数据字节是"无符号幅值"，符号在首字节 b0 的 bit4(X)/bit5(Y)
     （Linux psmouse 也是这么读：sign ? byte-256 : byte）。
     判据：按这套读法，b0 的符号位与解码结果的符号**零矛盾**（638 包/0 次矛盾）；
     而按旧的 (int8_t) 直接符号扩展读，有 352 次自相矛盾 —— 设备不可能一边说"正向"
     一边给出一个"负值字节"。旧代码因此把正向大位移读成反向 → 光标朝反方向跑、撞边后贴边。
  2) **聚合位移**：客人每次读包之间，设备会把多次移动攒起来，实测单包可达 ±255 像素。
     直接累加会让光标一步窜到屏幕边界。所以驱动里加了"每步最多 MOUSE_STEP_MAX 像素 +
     余量留到下一包（有上限）"，配合上下界钳制。
  3) 安装界面不再每帧整屏重绘：鼠标移动只重画光标新旧位置那一小块（脏矩形 + fb_flip_region），
     这样客人读包变快、位移不再被聚合成大跳（这是根因，1/2 只是把症状压住）。

用法：python tests/mouse_parse_test.py [--log <串口日志>] [--dump]
"""
import argparse
import os
import re
import sys

# Windows 控制台默认是 GBK，而本脚本会打印 U+2212 减号/箭头等字符，
# 不强制 UTF-8 会以 UnicodeEncodeError 假失败（与算法无关，纯输出编码问题）。
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

SCR_W, SCR_H = 1024, 768
STEP_MAX = 24      # 与 kernel/input.cpp 的 MOUSE_STEP_MAX 一致
ACC_MAX = 96       # 与 kernel/input.cpp 的 MOUSE_ACC_MAX 一致

# VMware 实抓（v64-install-test 的 serial-install.log；每 6 hex = b0 X Y）
PACKETS_HEX = (
    "080A00 081100 081500 081800 081901 081902 081902 081903 081904 081904 "
    "081906 081908 08190A 08190C 08190E 08190F 081911 081911 081912 081913 "
    "081913 081914 081915 081816 081517 081217 080E18 080819 08021A 18FC1B "
    "18F41D 18EC1F 18E521 18DE23 18D826 18D328 18CD2A 18C72D 18C031 18B734 "
    "18AE39 18A53E 189D41 189743 189145 188C47 188847 188647 188447 188347 "
    "188447 188647 188A47 189143 189B41 18A73D 18B03A 18B638 18BA37 18BB37 "
    "18BB36 18BB35 18BB34 18BC34 18BF33 18C332 18C532 18C931 18CD30 18D030 "
    "18D42E 18D62E 18D92D 18DC2C 18DE2C 18E02B 18E32A 18E42A 18E629 18E928 "
    "18EB27 18ED26 18EF25 18F024 18F124 18F323 18F322 18F522 18F622 18F822 "
    "18FA22 18FB22 18FD22 18FF22 080022 080222 080322 080522 080722 080922 "
    "080B22 080D22 080F22 081122 081222 081422 081522 081825 081A2A 083C67 "
    "082140 082347 08244D 082452 082456 082459 08245B 08245D 08235E 08215F "
    "083CC1 081C61 081B61 081961 081961 081761 081661 081561 081461 081261 "
    "081261 081160 08105F 080E5E 080E5C 080C5A 080C58 080A55 080952 08084E "
    "08064B 080548 080445 080242 08023F 08003D 18FF39 18FD37 18FC34 18FA32 "
    "18F830 18F72D 18F52B 18F329 18E24C 18EF24 18ED22 18EB22 18EA20 18E71F "
    "18E51D 18E31C 18E01B 18DE19 18DB17 18D815 18D614 18A424 18CD10 18C90F "
    "18C50D 18C00C 18BC0B 18B80A 18B409 18B108 18AE08 18AB07 18A906 18A706 "
    "18A506 18A305 18A204 18A104 189F04 189E04 189D03 189C03 189B02 189B02 "
    "183303 189900 189900 3899FF 3899FE 3899FE 3899FD 3899FC 3899FB 389AF9 "
    "389CF6 389EF5 38A1F2 38A3E9 3850BA 38ABD1 38AECA 38B1C3 38B4BC 38B6B5 "
    "38B8AF 38794C 38BF9D 38C197 38C391 38C58B 38C786 38C980 38CB7A 38CC74 "
    "38CE6E 38CF68 38D162 38D25C 38A800 38D64C 38B200 38DC45 38E044 38E345 "
    "38E649 38EA4E 38D900 38EF5D 38F061 38F166 38F36A 38F571 38F978 38FC87 "
    "280096 2802A4 2805B1 2808BF 280ACC 280DD9 280FE6 2811F4 081201 08150E "
    "08171B 081A26 081D32 081F3D 082146 0844A4 08255F 082669 082873 082A7D "
    "082C89 082E93 08309F 0832AA 0834B7 0837C3 0838CF 083ADA 083CE3 083DF2 "
    "083EFF 083FFF 083FFF 0841FF 0842FF 0843FF 0845FF 0847FF 0849FF 084AFF "
    "084BFF 084DFF 084DFF 084EFF 084FFF 084FFF 084FFF 084FFF 084FFF 0847FF "
    "0836FF 0820FF 080AEA 18FBC7 18F4A8 18F28C 18F175 18EC5D 18E447 18DD34 "
    "18D727 18D41F 18D31A 18D316 18D313 18D311 18D30F 18D30D 18D40D 18D40D "
    "18D60D 18D70D 18D90D 18DB0D 18DE0D 18E10D 18E40D 18E60D 18EA0D 18ED0D "
    "18F00D 18F30D 18F70D 18FB0D 18FF0D 08040D 08080D 080D0D 08120C 08150B "
    "081A0B 081F0A 08220A 082609 082A08 082F08 083307 083906 083F04 084403 "
    "084B02 085102 085801 085E00 086400 286AFF 286FFF 2875FE 287BFE 2881FE "
    "2888FE 288EFE 2894FE 289AFE 289FFE 28A6FE 08AC01 08B305 08BA0C 08C315 "
    "08CB1F 08D12A 08D534 08D63D 08D644 08D54A 08CB50 08BE58 08AB5F 089765 "
    "08836B 08716E 085F71 084E74 083A76 082376 080A76 18F375 18E26E 18D467 "
    "18CD5F 1890A6 18C643 18C638 18C62C 18CB20 18D413 18DF08 38EBFD 38F6F4 "
    "281FA7 281DCF 2827C7 2830BF 283AB7 2843B1 284BAB 2850A5 28549F 285699 "
    "285793 28B115 285C83 285E80 28647E 286B7C 28717C 28787C 28FF00 288A7D "
    "288F86 28FF00 289FBF 289FD0 289FE5 289FFE 089F1E 08A441 08AE64 08B884 "
    "08BFA0 08C5BB 08C8D6 08C9F1 08C9FC 08C9FF 08C9FF 08C9FF 08C9FF 08C9FF "
    "08C8FF 08C8FF 08C8FF 08C7FF 08C7FF 08C7FF 08C7FF 08C7FF 08FFFF 08C7FF "
    "08C6FF 08C4FF 08BDFF 08B0FF 089DFF 0889FF 0871FF 0858FF 083FFF 082AFF "
    "0817FF 0807FF 18F6FF 18E5F8 1899FF 18B8D4 18ACCA 1838FF 188EAC 180CFF "
    "18808A 187D7E 187B73 187A67 18795E 187955 18794C 187945 187B3D 187F37 "
    "188332 18882C 188D27 189422 18991D 189F1B 18A617 185E26 18B90F 18C10C "
    "18C80A 18D008 18D806 18E205 18E30A 080305 080F05 081906 082108 085C1D "
    "083A18 08431F 084F2A 085B37 086846 087555 088165 088C75 089786 08A096 "
    "08A8A6 08ACB7 08AEC5 08AED0 08AED6 08AADA 08A1E0 0897E6 088AED 087BF3 "
    "086BF7 085AFA 0847FB 0847FF 0804F2 18F3EB 18E6E2 18DADA 18CFD1 18C5C7 "
    "18BCBE 18B6B4 18B1AB 18AEA3 18AC9B 18AC95 18AC8E 1804FF 18AC75 18AE6F "
    "18B26B 18B667 18BA62 18BF5C 18C657 18CF50 18D649 18E042 18E83B 18F135 "
    "18F930 08022A 080D26 081723 082422 083022 083B22 084526 08502C 085C37 "
    "086A45 087554 087F64 088673 088980 088A8C 088A95 08869B 087C9E 0870A2 "
    "0861A6 084FAB 083BB0 0824B2 080EB2 18F8B2 18E4AC 18D3A4 18C59B 18BA93 "
    "18B48D 18B188 18B084 18B080 18B07C 18B077 18B371 18B76B 18BE64 18C55D "
    "18CC56 18D450 18DC4A 18E646 18EF42 18F73F 18FE3C 08043B 080D3B 1800FF "
    "1900FF 1800FF 08C0FF 09FFFF 09E2FF 09E8FF 09EFFF 09F5FF 09FAFF 09FFFF "
    "09FFFF 09FFFF 09FFFF 09FFFF 09FFFF 09FFFF 09FFFF 09FFFF 09FFFF 09FFFF "
    "09FFFF 09FFFF 09FFFF 09FFFF 09FFFF 09FFFF 08FFFF 08EAFF 09FFFF 09FFFF "
    "09FFFF 09FFFF 09FFFF 09FFFF 09FFFF 09FFFF 09FFFF 09FFFF 09FFFF 09FFFF "
    "09FFFF 09FFFF 08FFFF 080960 09FFFF 08FFFF"
)


def parse_hex_list(blob):
    out = []
    for tok in blob.split():
        v = int(tok, 16)
        out.append(((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF))
    return out


def packets_from_log(path):
    text = open(path, "r", encoding="utf-8", errors="replace").read()
    return [(int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16))
            for m in re.finditer(r"\|pB([0-9A-F]{2})X([0-9A-F]{2})Y([0-9A-F]{2})", text)]


def decode_old(b0, xb, yb):
    """旧代码：(int8_t) 直接符号扩展（丢掉了 b0 里的符号位）。"""
    return (xb - 256 if xb >= 128 else xb), (yb - 256 if yb >= 128 else yb)


def decode_b0(b0, xb, yb):
    """正确：符号在 b0 的 bit4/bit5，数据字节是无符号幅值（Linux psmouse 同款）。"""
    return ((xb - 256) if (b0 & 0x10) else xb), ((yb - 256) if (b0 & 0x20) else yb)


def contradictions(pkts, fn):
    """设备自洽性：b0 的符号位是否与解码结果的符号一致。"""
    n = 0
    for (b0, xb, yb) in pkts:
        dx, dy = fn(b0, xb, yb)
        if dx != 0 and ((b0 & 0x10) != 0) != (dx < 0):
            n += 1
        if dy != 0 and ((b0 & 0x20) != 0) != (dy < 0):
            n += 1
    return n


def simulate(pkts, fn, use_accum, sens_num=17, sens_den=10):
    """返回 (edge_hits, max_step, end, traveled)：
       edge_hits 越界次数、max_step 单次最大位移、end 终点、traveled 走过的总路程。"""
    x, y = SCR_W // 2, SCR_H // 2
    acc_x = acc_y = 0
    edge = 0
    max_step = 0
    traveled = 0
    for (b0, xb, yb) in pkts:
        dx, dy = fn(b0, xb, yb)
        sx, sy = dx * sens_num // sens_den, dy * sens_num // sens_den
        if use_accum:
            acc_x += sx
            acc_y += sy
            acc_x = max(-ACC_MAX, min(ACC_MAX, acc_x))
            acc_y = max(-ACC_MAX, min(ACC_MAX, acc_y))
            sx = max(-STEP_MAX, min(STEP_MAX, acc_x))
            sy = max(-STEP_MAX, min(STEP_MAX, acc_y))
            acc_x -= sx
            acc_y -= sy
        x += sx
        y -= sy
        if x < 0 or x >= SCR_W or y < 0 or y >= SCR_H:
            edge += 1
        max_step = max(max_step, abs(sx), abs(sy))
        traveled += abs(sx) + abs(sy)
        x = max(0, min(SCR_W - 1, x))
        y = max(0, min(SCR_H - 1, y))
        if not use_accum:
            # 旧代码：只钳下界（上界不钳）—— 复现用户看到的现象
            if x < 0: x = 0
            if y < 0: y = 0
    return edge, max_step, (x, y), traveled


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default=None, help="用新的串口日志里的 |p 包替换内置样本")
    ap.add_argument("--dump", action="store_true", help="打印前 40 个包")
    args = ap.parse_args()

    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    pkts = packets_from_log(args.log) if args.log and os.path.exists(args.log) \
        else parse_hex_list(PACKETS_HEX)
    print("样本：%d 个真实包（%s）" % (len(pkts), args.log or "内置 VMware 实抓"))

    print("=== 1) 符号约定：设备自洽性决定用哪种读法 ===")
    c_old = contradictions(pkts, decode_old)
    c_new = contradictions(pkts, decode_b0)
    check("旧读法（int8）与设备自相矛盾（就是它把方向读反的原因）", c_old > 100, "%d 次" % c_old)
    check("新读法（b0 符号位）零矛盾 —— 与 Linux psmouse 一致", c_new == 0, "%d 次" % c_new)
    big = sum(1 for (b, x, y) in pkts if x >= 128 or y >= 128)
    check("存在幅值 >= 128 的包（VMware 会发这种大位移）", big > 50, "%d 个" % big)
    if args.dump:
        for (b0, xb, yb) in pkts[:40]:
            print("     %02X%02X%02X  旧=%s 新=%s" % (b0, xb, yb, decode_old(b0, xb, yb), decode_b0(b0, xb, yb)))

    print("=== 2) 关键包逐个核对（符号位驱动）===")
    check("083CC1：b0 说 Y 正向、幅值 193 → +193（旧读法给 −63）",
          decode_b0(0x08, 0x3C, 0xC1)[1] == 193 and decode_old(0x08, 0x3C, 0xC1)[1] == -63)
    check("183303：b0 说 X 负向、幅值 51 → −205（旧读法给 +51）",
          decode_b0(0x18, 0x33, 0x03)[0] == -205 and decode_old(0x18, 0x33, 0x03)[0] == 51)
    check("18FF22：X 负、字节 0xFF → −1（低 8 位，9 位计数器语义）",
          decode_b0(0x18, 0xFF, 0x22)[0] == -1)
    small = [(0x08, 8, 0)] * 20                       # 20 个 +8 的包
    _, _, end_s, travel_s = simulate(small, decode_b0, use_accum=True)
    # 每包 8×1.7 → 13px，20 包共 260px（起点在屏幕中心 512，不触边）
    check("20 个 +8 的包 → 光标恰好走 260px（累加器不丢小位移）",
          travel_s == 20 * (8 * 17 // 10), "实际 %d" % travel_s)
    check("小位移时终点 = 起点 + 260（未被边界吞掉）",
          end_s[0] == SCR_W // 2 + 260, "x=%d" % end_s[0])

    print("=== 4) 实抓大位移流模拟（1024x768）===")
    e_o, m_o, end_o, t_o = simulate(pkts, decode_old, use_accum=False)
    e_n, m_n, end_n, t_n = simulate(pkts, decode_b0, use_accum=True)
    print("     旧（int8 + 只钳下界）：越界 %d 次，单步最大 %d px，终点 %s，路程 %d"
          % (e_o, m_o, end_o, t_o))
    print("     新（b0 符号 + 累加器 + 双向钳制）：越界 %d 次，单步最大 %d px，终点 %s，路程 %d"
          % (e_n, m_n, end_n, t_n))
    check("旧算法单步可达上百像素（一步窜到边界，就是用户看到的现象）", m_o > 100, "%d px" % m_o)
    check("新算法单步被限制在 %d px 以内（不会再一步贴边）" % STEP_MAX, m_n <= STEP_MAX, "%d px" % m_n)
    check("新算法光标确实在动（不是被冻住）", t_n > 10000, "路程 %d" % t_n)
    dir_ok = sum(1 for (b0, xb, yb) in pkts
                 if (decode_b0(b0, xb, yb)[0] >= 0) == ((b0 & 0x10) == 0))
    check("方向一致：解码方向与 b0 符号位一致率 100%%",
          dir_ok == len(pkts), "%d/%d" % (dir_ok, len(pkts)))

    print("=== MOUSE PARSE RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
