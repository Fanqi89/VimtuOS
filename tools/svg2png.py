#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/svg2png.py - **自研**"仅填充路径"SVG 光栅器（构建期用；不参与内核）

为什么自研：本机探测结果 —— ImageMagick(magick) 缺失、rsvg-convert 缺失、inkscape 缺失、
cairosvg 未安装（`convert` 命中的是 Windows 自带的 NTFS convert.exe，不是 ImageMagick），
而仓库里能用的只有 **Python + Pillow**。所以按需求给出的兜底方案自己写一个"仅填充路径"的
光栅器，并把图标选择限制在**纯 fill 路径**的套件（Bootstrap Icons 全部是 `fill` 单/多 path，
无 stroke、无 filter、无 mask/use）。

覆盖的 SVG 特性（逐条实测，见 --report）：
  * 元素：<path>（主力）、<rect>/<circle>/<ellipse>/<polygon>/<polyline>（先归一化成 path）、
    <g>（继承 fill/fill-rule/opacity/transform）、<title>/<desc>（忽略）
  * path 命令：M m L l H h V v C c S s Q q T t A a Z z（含隐式 lineto、S/T 的反射控制点）
  * 圆弧 A/a：端点->圆心参数化后按角度采样（含 large-arc/sweep 四种组合）
  * 填充规则：**nonzero**（default；Bootstrap 用反向绕序挖洞）与 evenodd
  * fill / fill-rule / fill-opacity / opacity（属性与 style="" 两种写法）
  * transform：matrix/translate/scale/rotate/skewX/skewY（可嵌套在 <g> 上，按 SVG 顺序左乘）
  * 抗锯齿：4x4 超采样（--ss 可调）后取覆盖率
不覆盖（遇到就**明确报错/报警**，绝不静默画错）：stroke*（描边图标）、filter、mask、clip-path、
<use>、<text>、<image>、渐变/图案填充、CSS 类外部样式表（class="" 忽略）。

用法：
  py -3 tools/svg2png.py in.svg out.png --size 48            # 白字 + alpha 的 RGBA（掩码）
  py -3 tools/svg2png.py in.svg out.png --size 24 --color FF8800
  py -3 tools/svg2png.py in.svg out.png --size 24 --mask     # 只存 8 位 alpha（L 模式）
  py -3 tools/svg2png.py --report in.svg                     # 打印这个 SVG 用到的特性
"""
import argparse
import math
import os
import re
import sys
import xml.etree.ElementTree as ET

NS = "{http://www.w3.org/2000/svg}"
UNSUPPORTED_TAGS = ("use", "text", "image", "mask", "filter", "pattern", "tspan")
STROKE_KEYS = ("stroke", "stroke-width", "stroke-linecap", "stroke-linejoin", "stroke-dasharray")


# ==================== 变换矩阵（a b c d e f 列主序，与 SVG 一致）====================
def mat_mul(m1, m2):
    a1, b1, c1, d1, e1, f1 = m1
    a2, b2, c2, d2, e2, f2 = m2
    return (a1 * a2 + c1 * b2, b1 * a2 + d1 * b2,
            a1 * c2 + c1 * d2, b1 * c2 + d1 * d2,
            a1 * e2 + c1 * f2 + e1, b1 * e2 + d1 * f2 + f1)


def mat_apply(m, x, y):
    a, b, c, d, e, f = m
    return (a * x + c * y + e, b * x + d * y + f)


_NUM = re.compile(r"[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?")


def parse_transform(s):
    m = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)
    if not s:
        return m
    for fn, args in re.findall(r"(\w+)\s*\(([^)]*)\)", s):
        v = [float(x) for x in _NUM.findall(args)]
        if fn == "matrix" and len(v) == 6:
            t = tuple(v)
        elif fn == "translate":
            tx = v[0] if v else 0.0
            ty = v[1] if len(v) > 1 else 0.0
            t = (1, 0, 0, 1, tx, ty)
        elif fn == "scale":
            sx = v[0] if v else 1.0
            sy = v[1] if len(v) > 1 else sx
            t = (sx, 0, 0, sy, 0, 0)
        elif fn == "rotate":
            a = math.radians(v[0] if v else 0.0)
            ca, sa = math.cos(a), math.sin(a)
            t = (ca, sa, -sa, ca, 0, 0)
            if len(v) >= 3:                     # rotate(a cx cy) = T(c) R T(-c)
                t = mat_mul(mat_mul((1, 0, 0, 1, v[1], v[2]), t), (1, 0, 0, 1, -v[1], -v[2]))
        elif fn == "skewX":
            t = (1, 0, math.tan(math.radians(v[0] if v else 0.0)), 1, 0, 0)
        elif fn == "skewY":
            t = (1, math.tan(math.radians(v[0] if v else 0.0)), 0, 1, 0, 0)
        else:
            raise ValueError("不支持的 transform 函数：%s" % fn)
        m = mat_mul(m, t)
    return m


# ==================== path 数据 -> 轮廓（闭合点列）====================
def _arc_to_points(x0, y0, rx, ry, phi_deg, large, sweep, x1, y1, steps_hint=16):
    """SVG 椭圆弧：端点参数化 -> 中点参数化 -> 采样成折线（含退化情形按直线处理）"""
    if rx == 0 or ry == 0:
        return [(x1, y1)]
    phi = math.radians(phi_deg)
    cp, sp = math.cos(phi), math.sin(phi)
    dx2, dy2 = (x0 - x1) / 2.0, (y0 - y1) / 2.0
    x1p = cp * dx2 + sp * dy2
    y1p = -sp * dx2 + cp * dy2
    rx, ry = abs(rx), abs(ry)
    lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry)
    if lam > 1.0:                                # 半径过小：按规范等比放大
        s = math.sqrt(lam)
        rx *= s
        ry *= s
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    co = math.sqrt(max(0.0, num / den)) if den > 0 else 0.0
    if large == sweep:
        co = -co
    cxp = co * rx * y1p / ry
    cyp = -co * ry * x1p / rx
    cx = cp * cxp - sp * cyp + (x0 + x1) / 2.0
    cy = sp * cxp + cp * cyp + (y0 + y1) / 2.0

    def ang(ux, uy, vx, vy):
        d = (ux * vx + uy * vy) / (math.hypot(ux, uy) * math.hypot(vx, vy) + 1e-12)
        a = math.acos(max(-1.0, min(1.0, d)))
        return -a if (ux * vy - uy * vx) < 0 else a

    th1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dth = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dth > 0:
        dth -= 2 * math.pi
    elif sweep and dth < 0:
        dth += 2 * math.pi
    n = max(4, int(abs(dth) / (math.pi / 16)) + 1) if not steps_hint else \
        max(4, min(96, int(abs(dth) * max(rx, ry) / 1.2) + 4))
    out = []
    for i in range(1, n + 1):
        t = th1 + dth * i / n
        ex = cp * rx * math.cos(t) - sp * ry * math.sin(t) + cx
        ey = sp * rx * math.cos(t) + cp * ry * math.sin(t) + cy
        out.append((ex, ey))
    return out


def _flatten_cubic(p0, p1, p2, p3, out, tol=0.15):
    approx = math.dist(p0, p1) + math.dist(p1, p2) + math.dist(p2, p3)
    n = max(4, min(64, int(approx / tol) + 1))
    for i in range(1, n + 1):
        t = i / n
        mt = 1 - t
        x = mt * mt * mt * p0[0] + 3 * mt * mt * t * p1[0] + 3 * mt * t * t * p2[0] + t * t * t * p3[0]
        y = mt * mt * mt * p0[1] + 3 * mt * mt * t * p1[1] + 3 * mt * t * t * p2[1] + t * t * t * p3[1]
        out.append((x, y))


def _flatten_quad(p0, p1, p2, out, tol=0.15):
    approx = math.dist(p0, p1) + math.dist(p1, p2)
    n = max(3, min(64, int(approx / tol) + 1))
    for i in range(1, n + 1):
        t = i / n
        mt = 1 - t
        out.append((mt * mt * p0[0] + 2 * mt * t * p1[0] + t * t * p2[0],
                    mt * mt * p0[1] + 2 * mt * t * p1[1] + t * t * p2[1]))


def path_to_contours(d, m=(1, 0, 0, 1, 0, 0)):
    """返回 [contour, ...]，contour = [(x, y), ...]（已应用变换；未闭合的会隐式闭合）"""
    toks = re.findall(r"[MmLlHhVvCcSsQqTtAaZz]|[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?", d)
    i, n = 0, len(toks)
    cmd = None
    x = y = 0.0          # 当前点
    sx = sy = 0.0        # 子路径起点
    px = py = 0.0        # 上个控制点（S/T 反射用）
    prev_cmd = None
    contours, cur = [], []

    def flush():
        if len(cur) >= 2:
            contours.append(list(cur))
        cur.clear()

    def nf():
        nonlocal i
        v = float(toks[i])
        i += 1
        return v

    while i < n:
        t = toks[i]
        if re.match(r"[A-Za-z]", t):
            cmd = t
            i += 1
        nums = lambda: (nf(), nf())
        rel = cmd.islower()
        c = cmd.upper()
        if c == "M":
            ax, ay = nums()
            if rel:
                ax, ay = x + ax, y + ay
            flush()
            x, y = ax, ay
            sx, sy = x, y
            cur.append((x, y))
            cmd = "l" if rel else "L"       # 隐式 lineto
        elif c == "L":
            ax, ay = nums()
            if rel:
                ax, ay = x + ax, y + ay
            x, y = ax, ay
            cur.append((x, y))
        elif c == "H":
            ax = nf()
            x = x + ax if rel else ax
            cur.append((x, y))
        elif c == "V":
            ay = nf()
            y = y + ay if rel else ay
            cur.append((x, y))
        elif c in ("C", "S"):
            if c == "C":
                x1, y1 = nums()
                if rel:
                    x1, y1 = x + x1, y + y1
            else:
                x1, y1 = (2 * x - px, 2 * y - py) if prev_cmd in ("C", "S", "c", "s") else (x, y)
            x2, y2 = nums()
            ax, ay = nums()
            if rel:
                x2, y2 = x + x2, y + y2
                ax, ay = x + ax, y + ay
            _flatten_cubic((x, y), (x1, y1), (x2, y2), (ax, ay), cur)
            px, py = x2, y2
            x, y = ax, ay
        elif c in ("Q", "T"):
            if c == "Q":
                x1, y1 = nums()
                if rel:
                    x1, y1 = x + x1, y + y1
            else:
                x1, y1 = (2 * x - px, 2 * y - py) if prev_cmd in ("Q", "T", "q", "t") else (x, y)
            ax, ay = nums()
            if rel:
                ax, ay = x + ax, y + ay
            _flatten_quad((x, y), (x1, y1), (ax, ay), cur)
            px, py = x1, y1
            x, y = ax, ay
        elif c == "A":
            rx, ry = nums()
            rot = nf()
            la, sw = nf(), nf()
            ax, ay = nums()
            if rel:
                ax, ay = x + ax, y + ay
            pts = _arc_to_points(x, y, rx, ry, rot, int(la) != 0, int(sw) != 0, ax, ay)
            cur.extend(pts)
            x, y = ax, ay
        elif c == "Z":
            flush()
            x, y = sx, sy
            cur.append((x, y))
        else:
            raise ValueError("path 命令不支持：%s" % cmd)
        prev_cmd = cmd if c in ("C", "S", "Q", "T") else c
    flush()
    return [[mat_apply(m, p[0], p[1]) for p in cin] for cin in contours]


def _num(v, default=0.0):
    if v is None:
        return default
    v = v.strip()
    if v.endswith("%"):
        return float(v[:-1]) / 100.0
    return float(v)


def shape_to_paths(el):
    """把 rect/circle/ellipse/polygon/polyline/line 归一化成 path 的 d（返回 [d, ...]）"""
    tag = el.tag.replace(NS, "")
    g = el.attrib.get
    if tag == "path":
        return [g("d", "")]
    if tag == "rect":
        x, y = _num(g("x")), _num(g("y"))
        w, h = _num(g("width")), _num(g("height"))
        rx = _num(g("rx"), -1.0)
        ry = _num(g("ry"), -1.0)
        if rx < 0:
            rx = ry if ry >= 0 else 0.0
        if ry < 0:
            ry = rx
        rx, ry = min(rx, w / 2), min(ry, h / 2)
        if rx <= 0 or ry <= 0:
            return ["M%f %f L%f %f L%f %f L%f %f Z" % (x, y, x + w, y, x + w, y + h, x, y + h)]
        return ["M%f %f A%f %f 0 0 1 %f %f L%f %f A%f %f 0 0 1 %f %f L%f %f A%f %f 0 0 1 %f %f "
                "L%f %f A%f %f 0 0 1 %f %f Z"
                % (x + rx, y, rx, ry, x + w, y + ry, x + w, y + h - ry, rx, ry, x + w - rx, y + h,
                   x, y + h - ry, rx, ry, x, y + ry, x, y + ry, rx, ry, x + rx, y)]
    if tag == "circle":
        cx, cy, r = _num(g("cx")), _num(g("cy")), _num(g("r"))
        return ["M%f %f A%f %f 0 1 0 %f %f A%f %f 0 1 0 %f %f Z"
                % (cx - r, cy, r, r, cx + r, cy, r, r, cx - r, cy)]
    if tag == "ellipse":
        cx, cy = _num(g("cx")), _num(g("cy"))
        rx, ry = _num(g("rx")), _num(g("ry"))
        return ["M%f %f A%f %f 0 1 0 %f %f A%f %f 0 1 0 %f %f Z"
                % (cx - rx, cy, rx, ry, cx + rx, cy, rx, ry, cx - rx, cy)]
    if tag in ("polygon", "polyline"):
        nums = _NUM.findall(g("points", ""))
        pts = [(float(nums[i]), float(nums[i + 1])) for i in range(0, len(nums) - 1, 2)]
        if not pts:
            return []
        d = "M" + " L".join("%f %f" % p for p in pts) + (" Z" if tag == "polygon" else "")
        return [d]
    if tag == "line":
        return ["M%f %f L%f %f" % (_num(g("x1")), _num(g("y1")), _num(g("x2")), _num(g("y2")))]
    return []


# ==================== 光栅化 ====================
def rasterize(svg_path, size, ss=4, viewbox=None):
    """返回 (coverage, features)：coverage = [size*size] 浮点 0..1"""
    tree = ET.parse(svg_path)
    root = tree.getroot()
    vb = viewbox or root.attrib.get("viewBox", "0 0 16 16")
    vx, vy, vw, vh = [float(t) for t in re.split(r"[\s,]+", vb.strip()) if t]
    scale = size / max(vw, vh)

    cov = [0.0] * (size * size)
    features = set()

    def parse_paint(el):
        style = el.attrib.get("style", "")
        kv = {}
        for part in style.split(";"):
            if ":" in part:
                k, v = part.split(":", 1)
                kv[k.strip()] = v.strip()
        get = lambda k: kv.get(k, el.attrib.get(k))
        fill = get("fill")
        fr = get("fill-rule")
        fo = get("opacity")
        fop = get("fill-opacity")
        return fill, fr, fo, fop, kv

    def walk(el, ctm):
        tag = el.tag.replace(NS, "")
        if tag in UNSUPPORTED_TAGS:
            features.add("UNSUPPORTED:" + tag)
            raise ValueError("SVG 元素不支持（仅填充路径光栅器）：<%s>" % tag)
        if tag in ("title", "desc", "defs", "style"):
            if tag == "style":
                features.add("style-element")
            return
        m = mat_mul(ctm, parse_transform(el.attrib.get("transform")))
        if tag == "g":
            for ch in el:
                walk(ch, m)
            return
        fill, fr, op, fop, kv = parse_paint(el)
        if fill in ("none", None) and "fill" in el.attrib and el.attrib["fill"] == "none":
            return
        if fill == "none":
            return
        for k in STROKE_KEYS:
            if kv.get(k) not in (None, "none", "0"):
                features.add("STROKE:" + k)
                raise ValueError("SVG 用了描边（%s），本光栅器只做填充：%s" % (k, svg_path))
        if kv.get("clip-path") or el.attrib.get("clip-path"):
            raise ValueError("SVG 用了 clip-path，本光栅器不支持：%s" % svg_path)
        if fill and fill.startswith("url("):
            raise ValueError("SVG 用了渐变/图案填充，本光栅器不支持：%s" % svg_path)
        alpha = 1.0
        if op is not None:
            alpha *= _num(op, 1.0)
        if fop is not None:
            alpha *= _num(fop, 1.0)
        if alpha <= 0:
            return
        evenodd = (fr == "evenodd")
        features.add("fill-rule:evenodd" if evenodd else "fill-rule:nonzero")

        contours = []
        for d in shape_to_paths(el):
            if d.strip():
                contours.extend(path_to_contours(d, m))
        if not contours:
            return
        # 视图变换：SVG 用户坐标(0..16) -> 像素
        tx = lambda p: ((p[0] - vx) * scale, (p[1] - vy) * scale)
        contours = [[tx(p) for p in c] for c in contours]
        for c in contours:
            if len(c) >= 2 and c[0] != c[-1]:
                c.append(c[0])
        _accumulate(cov, contours, size, ss, evenodd, alpha)

    def walk_root():
        # 根 <svg> 的 viewBox 缩放由 tx 负责；transform 属性若存在也一并应用
        m = mat_mul((1, 0, 0, 1, 0, 0), parse_transform(root.attrib.get("transform")))
        for ch in root:
            walk(ch, m)

    walk_root()
    return cov, features


def _accumulate(cov, contours, size, ss, evenodd, alpha):
    """nonzero/evenodd 覆盖率累积：逐子行（y 超采样）求交 -> 按 x 超采样累加

    非零环绕规则：把一条子行上所有轮廓的交点按 x 排序，从左到右累加带符号方向（上穿 +1/下穿 -1），
    winding != 0 的区间算"内部"——Bootstrap 的"挖洞"（外圈顺时针 + 内圈逆时针）靠的就是这个。
    evenodd 规则：同样的交点，改成数穿越次数的奇偶。
    """
    H = size * ss
    NU = size * ss
    row = [0.0] * NU                     # 当前子行的 x 覆盖率累计（单位 = 子像素）
    for yy in range(H):
        sy = (yy + 0.5) / ss
        xs = []
        for c in contours:
            for k in range(len(c) - 1):
                x1, y1 = c[k]
                x2, y2 = c[k + 1]
                if y1 == y2:
                    continue
                if (y1 <= sy < y2) or (y2 <= sy < y1):
                    t = (sy - y1) / (y2 - y1)
                    xs.append((x1 + t * (x2 - x1), 1 if y2 > y1 else -1))
        if not xs:
            continue
        xs.sort(key=lambda p: p[0])
        for j in range(NU):
            row[j] = 0.0
        wind, inside, start = 0, False, 0.0
        for xx, d in xs:
            wind += -d if evenodd else d        # evenodd：只看奇偶 -> 用 +1 累加效果等价
            now_in = (wind != 0)
            if (not inside) and now_in:
                inside, start = True, xx
            elif inside and (not now_in):
                inside = False
                _span_add(row, start * ss, xx * ss, NU)
        if inside:                              # 收尾（理论上已被 Z 闭合，防御性处理）
            _span_add(row, start * ss, float(NU), NU)
        base = (yy // ss) * size
        w = 1.0 / (ss * ss)
        for j in range(NU):
            if row[j] > 0.0:
                px = j // ss
                add = row[j] * w * alpha
                if add > 1.0:
                    add = 1.0
                idx = base + px
                cov[idx] = 1.0 if cov[idx] + add > 1.0 else cov[idx] + add


def _span_add(row, x0, x1, nu):
    """把 [x0, x1)（子像素单位）的覆盖量加到 row[]（按像素桶边界裁剪）"""
    if x1 <= x0:
        return
    x0 = max(0.0, x0)
    x1 = min(float(nu), x1)
    for j in range(max(0, int(math.floor(x0))), min(nu, int(math.ceil(x1)))):
        lo = max(x0, float(j))
        hi = min(x1, float(j + 1))
        if hi > lo:
            row[j] += (hi - lo)


# ==================== 输出 ====================
def write_png(cov, size, out, color=(255, 255, 255), mask_only=False):
    """写 PNG：默认 RGBA（给定颜色 + 覆盖率 alpha）；--mask 输出 8 位 L 灰度"""
    from PIL import Image
    if mask_only:
        buf = bytes(max(0, min(255, int(round(c * 255)))) for c in cov)
        img = Image.frombytes("L", (size, size), buf)
    else:
        r, g, b = color
        buf = bytearray(size * size * 4)
        for i, c in enumerate(cov):
            a = max(0, min(255, int(round(c * 255))))
            buf[i * 4 + 0] = r
            buf[i * 4 + 1] = g
            buf[i * 4 + 2] = b
            buf[i * 4 + 3] = a
        img = Image.frombytes("RGBA", (size, size), bytes(buf))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    img.save(out, optimize=True)
    return os.path.getsize(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("svg", nargs="?")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--size", type=int, default=24)
    ap.add_argument("--color", default="FFFFFF", help="6 位十六进制（默认白：配运行时着色）")
    ap.add_argument("--mask", action="store_true", help="输出 8 位 alpha（L 模式）")
    ap.add_argument("--ss", type=int, default=4, help="超采样倍数（默认 4）")
    ap.add_argument("--viewbox", default=None)
    ap.add_argument("--report", action="store_true", help="只打印该 SVG 用到的特性")
    args = ap.parse_args()
    if not args.svg:
        ap.error("需要 in.svg")
    if args.report:
        cov, feats = rasterize(args.svg, args.size, args.ss, args.viewbox)
        ink = sum(1 for c in cov if c > 0.02)
        print("%s: features=%s ink_px=%d/%d" % (args.svg, sorted(feats), ink, args.size * args.size))
        return 0
    if not args.out:
        ap.error("需要 out.png")
    cov, _ = rasterize(args.svg, args.size, args.ss, args.viewbox)
    col = tuple(int(args.color[i:i + 2], 16) for i in (0, 2, 4))
    n = write_png(cov, args.size, args.out, col, args.mask)
    print("  %s @%dpx -> %s (%d B)" % (os.path.basename(args.svg), args.size, args.out, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
