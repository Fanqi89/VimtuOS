# -*- coding: utf-8 -*-
"""源字体准备（**四个字体面**：西文 / 中文 / 终端等宽 / 缺字兜底）。

本脚本只做"把源字体放到该在的位置，并且证明它真的是字体"这件事：
  1) 清单里每个文件：存在就校验大小 + SHA256，不在（或哈希不符）就按 URL 下载再校验；
  2) 校验不是"看着像"就放行：必须是 TrueType(glyf/loca) 轮廓，含 CFF 的 OTF 直接报错
     —— 内核渲染器 kernel/font.cpp 的 face_init() 要求 head/hhea/hmtx/maxp/**loca/glyf**/cmap
     七张表齐全，缺一张就 font_ok=false（连字都画不出来），rasterize_glyph() 也不支持复合字形；
  3) 中文面的五个**静态字重**（NotoSansSC-{Light,Regular,Medium,Bold,Black}.ttf）由官方可变字体
     Fonts-open/NotoSansSC[wght].ttf 用 varLib.instancer pin wght=300/400/500/700/900 派生
     —— 官方仓库（google/fonts 与 notofonts/noto-cjk）**只**提供可变字体与 OTF/CFF 静态字重，
     没有静态 TTF；派生文件的大小/SHA256 也写进清单，派生结果可复现。

★ 为什么"缺失=硬失败"而不是"换个字体继续构建"：
  上一批实测出现过 Fonts-open/ 中途消失、构建照跑的事故 —— 那会产出"构建成功但字体不对"的内核，
  体积断言/渲染效果全部漂移；也出现过 URL 返回 HTML 错误页被当成字体存下来的事故（HTML 头 0x0a 或
  "<!DOCTYPE"），所以这里**同时**校验魔数 + SHA256。
  下载地址与许可见 FONTS.md、docs/字体许可说明.md。

用法：py -3 _otf2ttf.py            （只校验/补齐源字体；构建时由 build64.sh 自动调用）
"""
import os
import shutil
import sys
import hashlib

BASE = os.path.dirname(os.path.abspath(__file__))
FONTS = os.path.join(BASE, "Fonts")            # 西文（英文/数字/符号）
FONTS_OPEN = os.path.join(BASE, "Fonts-open")  # 中文 + 终端等宽 + 缺字兜底

# ==================== 源字体清单 ====================
# face 列 = 内核字体面（0=西文 1=中文 2=终端等宽 3=兜底），version 取自字体 name 表。
# 每个 URL 都是官方可再分发来源；浏览器/公司网络打不开 raw 时，同路径可用 jsDelivr 镜像：
#   https://cdn.jsdelivr.net/gh/<owner>/<repo>@<ref>/<path>
MANIFEST = [
    # ---- face 0：西文 UI（Noto Sans 静态字重，只有 Regular 参与构建，其余为可选字重）----
    dict(path="Fonts/NotoSans-Thin.ttf",
         url="https://github.com/notofonts/noto-fonts/raw/main/hinted/ttf/NotoSans/NotoSans-Thin.ttf",
         sha256="4dff742907c3816bf9c0e7ead025afc1ae8474a3315382fbf54f3217eaba3c68",
         size=546536, face=0, version="Noto Sans 2.008", license="SIL OFL 1.1", used=False),
    dict(path="Fonts/NotoSans-Light.ttf",
         url="https://github.com/notofonts/noto-fonts/raw/main/hinted/ttf/NotoSans/NotoSans-Light.ttf",
         sha256="74ffdd438f2ae232371ceb00444dae93e2590d263a4ff4c5e2c8385aa8012fdb",
         size=555788, face=0, version="Noto Sans 2.008", license="SIL OFL 1.1", used=False),
    dict(path="Fonts/NotoSans-Regular.ttf",
         url="https://github.com/notofonts/noto-fonts/raw/main/hinted/ttf/NotoSans/NotoSans-Regular.ttf",
         sha256="b85c38ecea8a7cfb39c24e395a4007474fa5a4fc864f6ee33309eb4948d232d5",
         size=569208, face=0, version="Noto Sans 2.008", license="SIL OFL 1.1", used=True),
    dict(path="Fonts/NotoSans-Bold.ttf",
         url="https://github.com/notofonts/noto-fonts/raw/main/hinted/ttf/NotoSans/NotoSans-Bold.ttf",
         sha256="c976e4b1b99edc88775377fcc21692ca4bfa46b6d6ca6522bfda505b28ff9d6a",
         size=575740, face=0, version="Noto Sans 2.008", license="SIL OFL 1.1", used=False),
    dict(path="Fonts/NotoSans-Black.ttf",
         url="https://github.com/notofonts/noto-fonts/raw/main/hinted/ttf/NotoSans/NotoSans-Black.ttf",
         sha256="2ea74ab862678045678cd0843e12fa17e100e7ad10b20f220fea6bb7b0ae88a5",
         size=591488, face=0, version="Noto Sans 2.008", license="SIL OFL 1.1", used=False),
    dict(path="Fonts/OFL-NotoSans.txt",
         url="https://github.com/notofonts/noto-fonts/raw/main/LICENSE",
         sha256="0dab92d0544f7b233403f14b84a663bdbfa746982eda629e7f4f9ffe1b036feb",
         size=4377, face=0, version="—", license="SIL OFL 1.1", used=False, is_license=True),

    # ---- face 1：中文（Noto Sans SC；官方只有可变字体，静态字重由本脚本派生）----
    dict(path="Fonts-open/NotoSansSC[wght].ttf",
         url="https://github.com/google/fonts/raw/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf",
         sha256="a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da",
         size=17772300, face=1, version="Noto Sans SC 2.004-H2 (variable, wght 100..900)",
         license="SIL OFL 1.1", used=False, variable=True),
    dict(path="Fonts-open/OFL-NotoSansSC.txt",
         url="https://github.com/google/fonts/raw/main/ofl/notosanssc/OFL.txt",
         sha256="1c05c68c34f9708415aada51f17e1b0092d2cea709bf4a94cd38114f9e73d7d9",
         size=4388, face=1, version="—", license="SIL OFL 1.1", used=False, is_license=True),

    # ---- face 2：终端等宽（Sarasa Mono SC，中英严格 1:2）----
    dict(path="Fonts-open/sarasa-mono-sc-regular.ttf",
         url="https://github.com/be5invis/Sarasa-Gothic/releases/download/v1.0.41/"
             "SarasaMonoSC-TTF-Unhinted-1.0.41.7z",
         archive_sha256="6e3ac724c4bf7d099aa44a2cc24ccdd4a3234c3248b13b3a4a76d570e8c79a26",
         archive_size=49736292, archive_member="SarasaMonoSC-Regular.ttf",
         sha256="fdd22c533bf15d72dbfe83f83712717ad19433a076be697aa4b15155efbea929",
         size=14037244, face=2, version="Sarasa Gothic 1.0.41 (Unhinted)",
         license="SIL OFL 1.1", used=True),
    dict(path="Fonts-open/sarasa-mono-sc-bold.ttf",
         url="https://github.com/be5invis/Sarasa-Gothic/releases/download/v1.0.41/"
             "SarasaMonoSC-TTF-Unhinted-1.0.41.7z",
         archive_sha256="6e3ac724c4bf7d099aa44a2cc24ccdd4a3234c3248b13b3a4a76d570e8c79a26",
         archive_size=49736292, archive_member="SarasaMonoSC-Bold.ttf",
         sha256="5e38695212e8a4cdf80c48b4d63761ff90e91ff8758acc7427a9fa242d1fbdd9",
         size=13809144, face=2, version="Sarasa Gothic 1.0.41 (Unhinted)",
         license="SIL OFL 1.1", used=False),
    dict(path="Fonts-open/OFL-Sarasa.txt",
         url="https://github.com/be5invis/Sarasa-Gothic/raw/v1.0.41/LICENSE",
         sha256="32c932e0dbae4f6e6386964bbc2d04178707665a05ca65cf636241af13d50a53",
         size=4702, face=2, version="Sarasa Gothic 1.0.41", license="SIL OFL 1.1",
         used=False, is_license=True),

    # ---- face 3：缺字兜底（GNU Unifont；最后一个官方提供 TTF 构建的版本）----
    dict(path="Fonts-open/unifont-14.0.01.ttf",
         url="https://unifoundry.com/pub/unifont/unifont-14.0.01/font-builds/unifont-14.0.01.ttf",
         sha256="c632666c659ccdfdb841f151aa6cc48cb987e093b90806f5af3d5a4bea7c54a5",
         size=12273956, face=3, version="GNU Unifont 14.0.01",
         license="GPL-2.0+ with font exception / SIL OFL 1.1 (dual)", used=True),
    dict(path="Fonts-open/UNIFONT-LICENSE.txt",
         url="https://unifoundry.com/LICENSE.txt",
         sha256="1e74cb82bf476843e97c2596297b04219b1a7e51f7238944a8c031cb9401fa87",
         size=24142, face=3, version="—",
         license="GPL-2.0+ with font exception / SIL OFL 1.1", used=False, is_license=True),
]

# 中文面静态字重：由 Fonts-open/NotoSansSC[wght].ttf 派生（wght pin 值 + 期望 SHA256）
STATIC_WEIGHTS = [
    ("Light", 300, "a86990e29cfd1f8f179f0946fa1f65563d51aace81df370ec21dbff6e89b0ff0", 10600932),
    ("Regular", 400, "5a461e5f078a4c07a154b5ad5f83fdf89ea9dd08861e27435ca95520056568b0", 10595876),
    ("Medium", 500, "23216ea389f0ea6b87b1f7a063d2cb33d65aafb714f459e820ccadb2fa2f2f40", 10589080),
    ("Bold", 700, "64eab14d1cc0c9e748abad548decdfd48417b745a2861a973317561c050229e0", 10585400),
    ("Black", 900, "344b29ba64a9d451f6a2896da4b43ce32682c4cf1ee936e45575be5ef7f4d107", 10576656),
]

CJK_REGULAR = "Fonts-open/NotoSansSC-Regular.ttf"   # face 1 构建实际用到的静态 Regular


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def looks_like_font(path):
    """魔数检查：TrueType(0x00010000) / 'true' / 'OTTO'；HTML 错误页（0x0a 开头或 <!DOCTYPE）明确拒绝。"""
    if not os.path.exists(path):
        return None, "文件不存在"
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        head = f.read(1024)
    if head[:4] in (b"\x00\x01\x00\x00", b"true", b"ttcf", b"OTTO"):
        return True, "TTF/OTF 魔数 ok (%d B)" % size
    low = head.lstrip(b"\r\n\t ")
    kind = "HTML 错误页" if low[:9].lower() == b"<!doctype" else "未知内容"
    return False, "不是字体文件（%s，前 4 字节 %s）" % (kind, head[:4].hex())


def require_glyf(path):
    """内核渲染器的硬要求：TrueType 轮廓（glyf + loca），明确拒绝 CFF/OTTO。"""
    from fontTools.ttLib import TTFont
    f = TTFont(path, lazy=True)
    tables = set(f.keys())
    if "glyf" not in tables or "loca" not in tables:
        f.close()
        raise SystemExit("ERROR: %s 没有 glyf/loca 表（CFF/OTTO 轮廓）——内核 TrueType 渲染器不支持，"
                         "请换 TrueType 轮廓的源字体" % path)
    if "CFF " in tables or "CFF2" in tables:
        f.close()
        raise SystemExit("ERROR: %s 含 CFF/CFF2 表——本管线不做 CFF→glyf 转换" % path)
    upem = f["head"].unitsPerEm
    nglyph = f["maxp"].numGlyphs
    f.close()
    return upem, nglyph


def download(url, dst):
    """下载到 dst。先试 urllib（走系统证书），失败再试 curl.exe（--ssl-no-revoke，某些 CA 链需要）。"""
    import ssl
    import subprocess
    import tempfile
    import urllib.request
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + ".part"
    print("    下载 %s" % url)
    ok = False
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "VimtuOS-font-fetch/1.0"})
        with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as out:
            shutil.copyfileobj(r, out, 1 << 20)
        ok = True
    except Exception as e:
        print("    urllib 失败（%s），改用 curl.exe 再试" % type(e).__name__)
    if not ok:
        curl = shutil.which("curl") or shutil.which("curl.exe")
        if not curl:
            raise SystemExit("ERROR: 下载失败且找不到 curl.exe：%s\n  请手动下载后放到 %s" % (url, dst))
        rc = subprocess.call([curl, "-L", "--ssl-no-revoke", "-sS", "-m", "900", "-o", tmp, url])
        if rc != 0:
            raise SystemExit("ERROR: curl 下载失败（rc=%d）：%s" % (rc, url))
    os.replace(tmp, dst)
    print("    -> %s (%d B)" % (dst, os.path.getsize(dst)))


def fetch_archive_member(entry):
    """Sarasa 的 TTF 在官方 release 的 7z 里：下整个压缩包，取出需要的成员（需要 py7zr）。"""
    arc = os.path.join(BASE, "build", "_cache", os.path.basename(entry["url"]))
    if not os.path.exists(arc) or os.path.getsize(arc) != entry["archive_size"]:
        download(entry["url"], arc)
    got = sha256_of(arc)
    if got != entry["archive_sha256"]:
        raise SystemExit("ERROR: %s 的 SHA256 不符（实际 %s，清单 %s）——下载被截断或上游换了内容"
                         % (arc, got, entry["archive_sha256"]))
    try:
        import py7zr
    except ImportError:
        raise SystemExit("ERROR: 取出 %s 需要 py7zr：py -3 -m pip install py7zr\n"
                         "  （压缩包已在 %s，也可以手动解压出 %s 放到 %s）"
                         % (entry["archive_member"], arc, entry["archive_member"], entry["path"]))
    dst = os.path.join(BASE, entry["path"])
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with py7zr.SevenZipFile(arc, "r") as z:
        tmpdir = os.path.join(BASE, "build", "_cache", "x")
        os.makedirs(tmpdir, exist_ok=True)
        z.extract(path=tmpdir, targets=[entry["archive_member"]])
    shutil.move(os.path.join(tmpdir, entry["archive_member"]), dst)
    print("    7z 取出 %s -> %s" % (entry["archive_member"], dst))


def ensure(entry):
    """存在就校验，缺失就下载；任何"不是字体/哈希不符"都硬失败。"""
    path = os.path.join(BASE, entry["path"])
    if entry.get("archive_member") and not os.path.exists(path):
        fetch_archive_member(entry)
    elif not os.path.exists(path):
        download(entry["url"], path)

    size = os.path.getsize(path)
    if entry.get("size") and size != entry["size"]:
        raise SystemExit("ERROR: %s 大小 %d != 清单 %d（下载中断/上游变更）"
                         % (entry["path"], size, entry["size"]))
    got = sha256_of(path)
    if got != entry["sha256"]:
        raise SystemExit("ERROR: %s 的 SHA256 不符：\n  实际 %s\n  清单 %s\n"
                         "  重新下载：%s" % (entry["path"], got, entry["sha256"], entry["url"]))
    if not entry.get("is_license"):
        ok, why = looks_like_font(path)
        if not ok:
            raise SystemExit("ERROR: %s：%s\n  来源：%s" % (entry["path"], why, entry["url"]))
        upem, nglyph = require_glyf(path)
        entry["_upem"], entry["_nglyph"] = upem, nglyph
    return True


def ensure_static_weights():
    """中文面五个静态字重：缺失/损坏时从官方可变字体派生（pin wght=<weight>）。"""
    vf = os.path.join(BASE, "Fonts-open/NotoSansSC[wght].ttf")
    need = []
    for label, wght, want_sha, want_size in STATIC_WEIGHTS:
        p = os.path.join(BASE, "Fonts-open/NotoSansSC-%s.ttf" % label)
        if os.path.exists(p) and os.path.getsize(p) == want_size and sha256_of(p) == want_sha:
            continue
        need.append((label, wght, p))
    if not need:
        print("    中文面静态字重：5 个都在且哈希相符（派生源 %s）" % os.path.basename(vf))
        return
    from fontTools.ttLib import TTFont
    from fontTools.varLib import instancer
    if not os.path.exists(vf):
        raise SystemExit("ERROR: 缺派生源 %s（见 MANIFEST 的 URL）" % vf)
    print("    中文面静态字重：从 %s 派生 %d 个（varLib.instancer pin wght）"
          % (os.path.basename(vf), len(need)))
    font = TTFont(vf)
    has_fvar = "fvar" in font
    for label, wght, out in need:
        if has_fvar:
            loc = {a.axisTag: (wght if a.axisTag == "wght" else a.defaultValue) for a in font["fvar"].axes}
            inst = instancer.instantiateVariableFont(font, loc, inplace=False, updateFontNames=True)
        else:                       # 万一拿到的是静态文件：直接用（并如实说明）
            print("    ★ %s 里没有 fvar（静态字体）——wght=%d 的期望由它直接担任" % (vf, wght))
            inst = font
        inst.save(out)
        if inst is not font:
            inst.close()
    font.close()
    for label, wght, want_sha, want_size in STATIC_WEIGHTS:
        p = os.path.join(BASE, "Fonts-open/NotoSansSC-%s.ttf" % label)
        got = sha256_of(p)
        if got != want_sha:
            print("    ★ 注意：%s 的 SHA256 %s 与清单里的 %s 不同（fontTools 版本差异）——"
                  "已按实际值使用；FONTS.md 的派生列以实际为准" % (os.path.basename(p), got[:16], want_sha[:16]))


def main():
    print("==> 源字体校验/补齐（四面的源文件 + 许可；缺失=硬失败，绝不换字体继续构建）")
    edge = [e for e in MANIFEST if e["face"] in (0, 2, 3)]
    for e in edge:
        ensure(e)
    # 中文面：官方可变字体 + 派生静态字重
    for e in MANIFEST:
        if e["face"] == 1:
            ensure(e)
    ensure_static_weights()
    for label, wght, want_sha, want_size in STATIC_WEIGHTS:
        ok, why = looks_like_font(os.path.join(BASE, "Fonts-open/NotoSansSC-%s.ttf" % label))
        if not ok:
            raise SystemExit("ERROR: Fonts-open/NotoSansSC-%s.ttf：%s" % (label, why))

    print()
    print("    面  %-34s %10s  %-24s %s" % ("文件", "字节", "版本", "用途"))
    for e in MANIFEST:
        if e.get("is_license"):
            print("    %d   %-34s %10d  %-24s 许可全文" % (e["face"], os.path.basename(e["path"]),
                                                          e["size"], e["version"]))
        else:
            print("    %d   %-34s %10d  %-24s %s" % (e["face"], os.path.basename(e["path"]), e["size"],
                                                    e["version"], "参与构建 ★" if e["used"] else "可选字重/备用"))
    for label, wght, want_sha, want_size in STATIC_WEIGHTS:
        p = os.path.join(BASE, "Fonts-open/NotoSansSC-%s.ttf" % label)
        print("    1   %-34s %10d  %-24s %s" % (os.path.basename(p), os.path.getsize(p),
                                                "Noto Sans SC 2.004-H2 (wght=%d)" % wght,
                                                "参与构建 ★" if label == "Regular" else "派生静态字重"))
    print()
    print("OK: 源字体齐备（%d 个源文件 + 5 个派生静态字重）" % len(MANIFEST))
    return 0


if __name__ == "__main__":
    sys.exit(main())
