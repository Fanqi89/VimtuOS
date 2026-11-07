"""tests/iso9660_64_test.py - 光驱驱动 + ISO9660 只读卷的端到端验收（真 QEMU + 真光盘）

背景（v0.9.26 之前的状态，已核实）：
  * kernel/ata64.cpp 早就有 ATAPI PACKET + READ(10)（PIO），但**只**被安装程序用来
    "把载荷从光盘拷到硬盘"，光驱没有驱动器号、没有 READ CAPACITY、不进任何文件系统；
  * kernel/ahci64.cpp 对 PxSIG = 0xEB140101 的端口只打 "kind=atapi（只识别）"，**不读**；
  * 内核里 **0 处** ISO9660 解析（没有 PVD / 目录记录代码）。所以光盘插进去，
    文件管理器里什么都看不到。

本批（v0.9.26）做了什么：
  * ata64 新增光驱段（ata64_cd_count64/drive64/model64/read64/capacity64）：
    驱动器号 0..3 = PATA ATAPI、32.. = AHCI ATAPI；**接口单位一律 2048B 光盘块**；
  * ahci64 新增 PACKET(0xA0) + 12 字节 CDB 的 DMA 读（ATA_CMD_PACKET / ACMD 区 / features bit0=DMA）；
  * 新文件 kernel/iso9660_64.{h,cpp}：PVD -> 根目录记录 -> 逐级路径解析 -> 按字节读文件；
    每个 32 位字段的 LE/BE **两份副本都读并交叉校验**（不一致 = 如实失败，不猜）；
  * fs64 新增 FS64_KIND_ISO9660 与只读卷号（FS64_VOL_ISO_BASE = 8），
    list/stat/read/read_range 走 ISO9660，写入口一律 -FS64_EROFS；
  * drive64 把光盘登记成一条盘符（fs=ISO9660、ro=1）；
  * kernel64 系统内核启动路径：探测光驱 -> READ CAPACITY -> 挂 ISO -> 列根目录 -> 自检。

本脚本的判据（**两侧独立**，不是自己证明自己）：
  A) 宿主侧用 Python 直接解析 vimtu64-64.iso 的 PVD/根目录；
  B) 客人侧串口报 blocks / root extent / 根目录条目数 / 文件前 N 字节长度；
  C) A 与 B **逐项相等**（blocks、root extent、root size、根目录条目数、名字集合、读取长度）。
  再加：盘符层登记（[DRV64] fs=ISO9660 ro=1）、自检 PASS、禁止行（PANIC/TRIPLE FAULT/mount FAIL/
  atapi read FAILED/selftest FAIL）。
  D) 换一台机器（q35 = AHCI 控制器）再跑一遍：光驱走 **AHCI ATAPI** 路径（bus=ahci），
     证明 SATA 光驱也能读（不只是 IDE 的老路径）。

用法（Windows 原生 Python）：
    py -3 tests\\iso9660_64_test.py [--qemu 路径] [--keep] [--only pata|ahci]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU / 构建产物缺失）
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import proc64_test as p64          # noqa: E402  夹具/端口/QEMU 路径
import qemuhelp as qh              # noqa: E402  Monitor（sendkey）

ISO = os.path.join(ROOT, "vimtu64-64.iso")

FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "[ISO9660] mount FAIL",
    "[ISO9660] selftest FAIL",
    "[AHCI64] atapi read FAILED",
    "[CD] ATAPI 读失败",
]


def q(path):
    return path.replace("\\", "/")


def free_port():
    import socket
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def slog(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_re(path, pattern, timeout, proc=None, since=0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        m = re.search(pattern, slog(path)[since:])
        if m:
            return m
        if proc is not None and proc.poll() is not None:
            break
        time.sleep(0.4)
    return None


# ==================== 宿主侧独立解析（ISO9660 的一手校验）====================
def iso_host(path):
    """解析 PVD 与根目录。返回 dict：blocks / root_ext / root_len / entries[(name, ext, size, is_dir)]"""
    with open(path, "rb") as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
        assert pvd[0] == 1 and pvd[1:6] == b"CD001", "LBA16 不是 ISO9660 主卷描述符"
        blocks_le = int.from_bytes(pvd[80:84], "little")
        blocks_be = int.from_bytes(pvd[84:88], "big")
        assert blocks_le == blocks_be, "PVD 的卷空间大小两个字节序副本不一致"
        rl = pvd[156]
        assert rl >= 34, "PVD 根目录记录长度不合法"
        root_ext = int.from_bytes(pvd[156 + 2:156 + 6], "little")
        root_len = int.from_bytes(pvd[156 + 10:156 + 14], "little")
        volid = pvd[40:72].decode("ascii", "replace").rstrip(" \x00")
        f.seek(root_ext * 2048)
        data = f.read(root_len)
    entries = []
    off = 0
    while off < len(data):
        ln = data[off]
        if ln == 0:
            off = (off // 2048 + 1) * 2048
            continue
        rec = data[off:off + ln]
        if len(rec) < 34:
            break
        nl = rec[32]
        name = rec[33:33 + nl]
        if not (nl == 1 and name[0] in (0, 1)):
            nm = name.split(b";")[0].decode("ascii", "replace")
            entries.append((nm,
                            int.from_bytes(rec[2:6], "little"),
                            int.from_bytes(rec[10:14], "little"),
                            (rec[25] & 0x02) != 0))
        off += ln
    return {"blocks": blocks_le, "root_ext": root_ext, "root_len": root_len,
            "volid": volid, "entries": entries}


# ==================== 一个用例：起 QEMU，等客人把话说完 ====================
def run_case(qemu, disk, serial, label, machine, expect_bus, checks, keep=False):
    port = free_port()
    argv = [qemu, "-name", "Vimtu64-%s" % label,
            "-drive", "format=raw,file=%s" % q(disk),
            "-cdrom", q(ISO),
            "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
            "-serial", "file:%s" % q(serial),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port, "-no-reboot"]
    if machine:
        argv[1:1] = ["-machine", machine]
    proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        host = iso_host(ISO)
        # ① 光驱枚举（PATA 或 AHCI）
        mcd = wait_re(serial, r"\[CD64\] cdrom idx=0 drive=(\d+) bus=(\w+) model=([^\n]*) atapi=1",
                      120, proc)
        checks.append(("%s：光驱被枚举（[CD64] cdrom ... atapi=1）" % label, mcd is not None,
                       mcd.group(0) if mcd else "（无 [CD64] cdrom 行）"))
        if mcd is None:
            return
        drv = mcd.group(1)
        checks.append(("%s：光驱走 %s 通路" % (label, expect_bus), mcd.group(2) == expect_bus,
                       "bus=%s" % mcd.group(2)))
        # ② READ CAPACITY：块数 == **整张光盘**的 2048B 块数（= ISO 文件大小 / 2048）。
        #    注意别拿 PVD 的"卷空间大小"来比：本仓库的 ISO 是 **El Torito 混合镜像**
        #    （xorriso 追加了 UEFI 的 FAT 附加分区），整盘块数 > ISO9660 卷声明的块数。
        #    两者都对，口径不同 —— 这一条钉的是"驱动读出来的物理容量"，③ 才钉"PVD 声称的卷大小"。
        iso_blocks = os.path.getsize(ISO) // 2048
        mcap = wait_re(serial, r"\[CD64\] capacity idx=0 drive=%s blocks=(\d+) bytes=(\d+) ok" % drv,
                       60, proc)
        checks.append(("%s：READ CAPACITY 成功且块数 == 整盘块数（%d）" % (label, iso_blocks),
                       mcap is not None and int(mcap.group(1)) == iso_blocks,
                       (mcap.group(0) if mcap else "（无 capacity 行）") +
                       " / ISO 文件 %d B = %d 块" % (os.path.getsize(ISO), iso_blocks)))
        # ③ ISO9660 挂载：卷标识 / 卷空间大小 / 根目录 extent 与长度都要与宿主一致
        #    （组号：1=vol 2=blocks 3=bytes 4=root_ext 5=root_size）
        mmo = wait_re(serial, r"\[ISO9660\] mount ok cd=%s vol=(\S+) blocks=(\d+) bytes=(\d+) "
                              r"root=(\d+)/(\d+)" % drv, 60, proc)
        ok3 = (mmo is not None
               and mmo.group(1) == host["volid"]
               and int(mmo.group(2)) == host["blocks"]
               and int(mmo.group(4)) == host["root_ext"]
               and int(mmo.group(5)) == host["root_len"])
        checks.append(("%s：PVD 解析（vol/blocks/root extent/size）与宿主逐项相等" % label, ok3,
                       (mmo.group(0) if mmo else "（无 mount ok 行）") +
                       " / 宿主 vol=%s blocks=%d root=%d/%d" % (host["volid"], host["blocks"],
                                                               host["root_ext"], host["root_len"])))
        # ④ 根目录条目数：客人 ls 的 entries 必须等于宿主 walker 的条目数
        mls = wait_re(serial, r"\[ISO9660\] ls path=/ entries=(\d+)", 60, proc)
        checks.append(("%s：根目录条目数 == 宿主（%d）" % (label, len(host["entries"])),
                       mls is not None and int(mls.group(1)) == len(host["entries"]),
                       (mls.group(0) if mls else "（无 ls 行）")))
        # ⑤ 客人列出来的名字必须都在宿主集合里（大小写不敏感），且至少 2 条
        names = re.findall(r"\[CD64\] root name=(\S+) size=(\d+) dir=(\d)", slog(serial))
        hset = set(n.upper() for n, _e, _s, _d in host["entries"])
        ok5 = len(names) >= 2 and all(n.upper() in hset for n, _s, _d in names)
        checks.append(("%s：客人报的根目录名字 ∈ 宿主集合（至少 2 条）" % label, ok5,
                       ", ".join("%s(%s)" % (n, d) for n, _s, d in names[:6]) if names else "（无 root name 行）"))
        # ⑥ 读文件：len 必须等于 min(16, 宿主 size)，got == len
        mrd = wait_re(serial, r"\[ISO9660\] read path=(\S+) off=0 len=(\d+) got=(\d+)", 60, proc)
        ok6 = False
        detail = "（无 read 行）"
        if mrd is not None:
            pth = mrd.group(1).lstrip("/")
            want = None
            for n, _e, s, d in host["entries"]:
                if n.upper() == pth.upper() and not d:
                    want = min(16, s)
                    break
            ok6 = (want is not None and int(mrd.group(2)) == want and int(mrd.group(3)) == want)
            detail = mrd.group(0) + (" / 宿主 size 推出 want=%s" % want)
        checks.append(("%s：按路径读文件（len/got 与宿主 size 自洽）" % label, ok6, detail))
        # ⑦ 自检 PASS + 盘符层登记（fs=ISO9660 ro=1）
        mst = wait_re(serial, r"\[ISO9660\] selftest PASS entries=(\d+) drive=%s" % drv, 60, proc)
        checks.append(("%s：内核自检 PASS" % label, mst is not None,
                       mst.group(0) if mst else "（无 selftest PASS 行）"))
        mfi = wait_re(serial, r"\[FS64\] mount iso vol=8 cd=%s volid=\S+ total_kb=(\d+) ro=1" % drv, 60, proc)
        checks.append(("%s：fs64 登记为只读 ISO 卷（ro=1）" % label, mfi is not None,
                       mfi.group(0) if mfi else "（无 [FS64] mount iso 行）"))
        mdr = wait_re(serial, r"\[DRV64\] letter=(\w): disk=%s part=0 fs=ISO9660 [^\n]*ro=1" % drv, 90, proc)
        checks.append(("%s：盘符层登记成一张只读光盘（[DRV64] fs=ISO9660 ro=1）" % label,
                       mdr is not None, mdr.group(0) if mdr else "（无 [DRV64] letter 行）"))
        # ⑧ 系统仍然起得来（挂载光盘没把启动链搞坏）。
        #   判据用 [GUI64] desktop init（桌面外壳初始化完、几何已定）而**不是** [GUI64] ready：
        #   ready 要等锁屏->登录那一整套手势走完（tests/wheel64_test.py 里是 login_desktop 驱动的），
        #   本脚本不注入键鼠，钉到 ready 会等成假红。
        checks.append(("%s：系统仍能起到桌面层（[GUI64] desktop init）" % label,
                       wait_re(serial, r"\[GUI64\] desktop init \d+x\d+", 240, proc) is not None, ""))
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=20)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--only", default=None, choices=("pata", "ahci"))
    args = ap.parse_args()

    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(ISO):
        sys.stderr.write("缺少 %s（先跑 bash build64.sh）\n" % ISO)
        return 2
    base = p64.prepare_fixture()
    if not base or not os.path.exists(base):
        sys.stderr.write("缺少构建产物或造夹具失败：%s\n" % p64.SYSTEM_IMG)
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_iso64_")
    disk = os.path.join(tmp, "iso.img")
    shutil.copyfile(base, disk)            # ★ 副本：绝不直接引导共享夹具

    checks = []
    try:
        if args.only != "ahci":
            print("=== A) 默认机器（i440fx：光驱在 PATA/ATAPI 上）===")
            run_case(qemu, disk, os.path.join(tmp, "pata.log"), "pata", None, "pata", checks)
        if args.only != "pata":
            print("=== B) q35（光驱在 AHCI 控制器上：走 PACKET + DMA）===")
            d2 = os.path.join(tmp, "iso_ahci.img")
            shutil.copyfile(base, d2)
            run_case(qemu, d2, os.path.join(tmp, "ahci.log"), "ahci", "q35", "ahci", checks)

        # 禁止行（两个日志一起看）
        for tag in ("pata", "ahci"):
            p = os.path.join(tmp, tag + ".log")
            txt = slog(p)
            if not txt:
                continue
            bad = [w for w in FORBIDDEN if w in txt]
            checks.append(("%s：没有禁止行（PANIC/挂载失败/读失败）" % tag, not bad,
                           "、".join(bad) if bad else ""))
    finally:
        if args.keep:
            print("日志保留：%s" % tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)

    print()
    passed = sum(1 for _n, c, _d in checks if c)
    for name, cond, detail in checks:
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))
    print("=== %d/%d PASS ===" % (passed, len(checks)))
    return 0 if passed == len(checks) and checks else 1


if __name__ == "__main__":
    sys.exit(main())
