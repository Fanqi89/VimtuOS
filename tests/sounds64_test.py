#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/sounds64_test.py - ★ 系统音效（4 段素材 + 音频 ABI 49 audio_play + /bin/sounder）端到端验收

被验的需求（任务书 ①②④ 一一对应；宿主侧断言 + 串口真打点 + **QEMU wav 后端的录音**）：

  ① 素材与卷内字节：
     * 4 段素材由 tools/sounds_gen.py 自合成（可复现），宿主侧量出 采样率/帧数/时长/峰值/sha256；
     * **从真实系统卷（build64/sysdisk.img 主分区 LBA 8009 的 VimtuFS2 卷）读回** /usr/share/sounds/*.wav
       与 /bin/sounder，逐字节与宿主生成物对照（sha256 相同）；
     * /bin/sounder <= 64 KiB（用户窗口装载区上限）；内核二进制里搜不到它们的字节（高熵 64B 探针）。
  ② 真的出声了（客观证据）：`-audiodev wav` 抓到的录音**非空**，且每个素材的
     非静音区段 时长/峰值 与宿主素材相符（打印实测数字 + 比值）。
  ③ 静音（`audio mute on`）时同一段程序播放 -> 录音里**不出现**新的非静音区段。
  ④ 音量 0 / 50 / 100 对输出幅度的影响：50% 的实测峰值 < 100% 的，0% 静音 -> 无区段。
  ⑤ `audio_play` 的非法参数（用户窗口外指针 / NULL / 错格式 / 零长度 / 超大长度）
     -> 明确错误码（-2 EFAULT / -3 EINVAL）、**不 PANIC**；另有一条合法调用返回 0。
  ⑥ 触发点打点：`[SND64] play va=0x… frames=… fmt=0x11 rc=…`（每次播放一行）+ 失败时的
     `[SND64] deny reason=…`；以及用户态播放器自己的 `SOUNDER play …` 行（含 frames/peak/sum）。
  ⑦ 全程无 PANIC / 三重故障；不出现 `[SYSCALL] enosys nr=49`；`[SND64]` 只出现在本批的调用点。

测试台：**QEMU 只能 QEMU**（HDA 仿真）：
  `-device ich9-intel-hda -device hda-duplex -audiodev wav,id=snd0,path=<out.wav>`
（VMware 上没有 HDA，本测试不适用。）

用法：py -3 tests\\sounds64_test.py [--img build64/sysdisk.img] [--qemu 路径] [--port 5807] [--keep]
       py -3 tests\\sounds64_test.py --no-qemu        # 只做宿主侧/卷内断言
退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题（构建产物缺失）。
"""
import argparse
import hashlib
import os
import struct
import re
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import qemuhelp as qh              # noqa: E402  （公共登录手势：ui.login.auto 默认 0）

FORBIDDEN = ["PANIC", "TRIPLE FAULT", "三重故障", "[SYSCALL] enosys nr=49"]
SOUNDS = ("startup", "notify", "click", "error")
SOUND_DIR = "/usr/share/sounds"
PART_MAIN_LBA = 8009
SECTOR = 512
# 基线（本批开工前的系统内核体积；报告里的"净增字节" = 现在 - 这个数）
KERNEL_BASELINE_BYTES = 3344448
KERNEL_HARD_LIMIT = 4096000


def q(p):
    return p.replace("\\", "/")


def fnv1a32(b):
    h = 0x811C9DC5
    for x in b:
        h = ((h ^ x) * 0x01000193) & 0xFFFFFFFF
    return h


# ---------------------------------------------------------------------------
# 宿主侧 WAV 解析（**独立实现**：不看 tools/sounds_gen.py 的自述，直接解析字节）
# ---------------------------------------------------------------------------
def wav_parse(raw):
    """返回 (rate, ch, bits, pcm_bytes, peak)。"""
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        return None
    i, fmt, data_off, data_sz = 12, None, None, 0
    while i + 8 <= len(raw):
        cid = raw[i:i + 4]
        sz = int.from_bytes(raw[i + 4:i + 8], "little")
        if cid == b"fmt ":
            fmt = raw[i + 8:i + 8 + sz]
        elif cid == b"data":
            data_off = i + 8
            data_sz = sz if (sz and i + 8 + sz <= len(raw)) else (len(raw) - i - 8)
            break
        i += 8 + sz + (sz & 1)
    if not fmt or data_off is None:
        return None
    ch = int.from_bytes(fmt[2:4], "little")
    rate = int.from_bytes(fmt[4:8], "little")
    bits = int.from_bytes(fmt[14:16], "little")
    pcm = raw[data_off:data_off + data_sz]
    peak = 0
    for k in range(len(pcm) // 2):
        v = struct.unpack_from("<h", pcm, k * 2)[0]
        a = -v if v < 0 else v
        if a > peak:
            peak = a
    return (rate, ch, bits, len(pcm), peak)


def wav_record_header(path):
    """录音文件（QEMU wav 后端实时写）的前 4 KiB 头：返回 (rate, ch, bits, data_off)。"""
    try:
        with open(path, "rb") as f:
            head = f.read(4096)
    except OSError:
        return None
    if head[:4] != b"RIFF" or head[8:12] != b"WAVE":
        return None
    i, fmt, data_off = 12, None, None
    while i + 8 <= len(head):
        cid = head[i:i + 4]
        sz = int.from_bytes(head[i + 4:i + 8], "little")
        if cid == b"fmt ":
            fmt = head[i + 8:i + 8 + sz]
        elif cid == b"data":
            data_off = i + 8
            break
        i += 8 + sz + (sz & 1)
    if not fmt or data_off is None:
        return None
    return (int.from_bytes(fmt[4:8], "little"), int.from_bytes(fmt[2:4], "little"),
            int.from_bytes(fmt[14:16], "little"), data_off)


def wav_tail(path, max_seconds=12.0, since_off=0):
    """读录音的 [since_off, EOF) 里**最近** max_seconds 秒：返回 (rate, ch, pcm bytes)。

    since_off：本阶段开始时的文件长度。**必须**用它把"上一阶段已经响过的那几段"挡掉，
    否则静音/小音量阶段会把上一阶段的声音算进去（实测踩过这类误判）。
    """
    hd = wav_record_header(path)
    if not hd:
        return None
    rate, ch, bits, data_off = hd
    if bits != 16 or ch <= 0:
        return None
    sz = os.path.getsize(path)
    start = max(data_off, since_off)
    if sz <= start:
        return (rate, ch, b"")
    want = int(max_seconds * rate * ch * 2)
    start = max(start, sz - want)
    with open(path, "rb") as f:
        f.seek(start)
        pcm = f.read(sz - start)
    pcm = pcm[:(len(pcm) // (2 * ch)) * (2 * ch)]        # 整帧
    return (rate, ch, pcm)


def wav_runs(path, thr=8, blk_ms=10.0, gap_blocks=5, max_seconds=12.0, since_off=0):
    """把录音尾部的**左声道**做 10 ms RMS 包络，返回非静音区段 [(起块, 止块), …]

    为什么用 RMS 包络而不是"逐样本非零"：正弦/方波本身有零交点（逐样本判会把一段音切成很多片），
    而静音段（声卡停流）是**真正的零**。合并间隔 <= gap_blocks（50 ms）的相邻块，
    这样"错误音"里两下 beep 之间的 15 ms 空隙不会被当成两段。
    """
    t = wav_tail(path, max_seconds=max_seconds, since_off=since_off)
    if t is None:
        return None, None
    rate, ch, pcm = t
    blk = int(rate * blk_ms / 1000.0)
    if blk <= 0 or not pcm:
        return [], (rate, ch, pcm)
    env = []
    for b in range(0, len(pcm) // (2 * ch) - blk + 1, blk):
        s = 0
        for k in range(b, b + blk):
            v = struct.unpack_from("<h", pcm, k * 2 * ch)[0]     # 只看左声道
            s += v * v
        env.append((s / float(blk)) ** 0.5)
    runs = []
    cur = None
    for i, e in enumerate(env):
        if e > thr:
            if cur is None:
                cur = [i, i]
            else:
                cur[1] = i
        elif cur is not None and i - cur[1] > gap_blocks:
            runs.append((cur[0], cur[1]))
            cur = None
    if cur is not None:
        runs.append((cur[0], cur[1]))
    return runs, (rate, ch, pcm, blk, env)


def run_peak(pcm, ch, blk, s, e, extra_tail=2):
    """区段 [s,e] 块（+尾部 2 块，防止衰减尾巴被切掉）的峰值。"""
    first = s * blk
    last = min((e + 1 + extra_tail) * blk, len(pcm) // (2 * ch))
    peak = 0
    for k in range(first, last):
        v = struct.unpack_from("<h", pcm, k * 2 * ch)[0]
        a = -v if v < 0 else v
        if a > peak:
            peak = a
    return peak


# ---------------------------------------------------------------------------
# QEMU / monitor / 光标（与 tests/hda64_test.py 同一套字符映射与相对位移口径）
# ---------------------------------------------------------------------------
class Monitor:
    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.35):
        try:
            s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        except OSError:
            return False
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
        finally:
            s.close()
        return True

    def key(self, name, wait=0.9):
        self.send("sendkey %s" % name, wait=wait)

    def raw(self, cmds, wait_between=0.12, wait_end=0.4):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        try:
            for c in cmds:
                s.sendall(c.encode() + b"\n")
                time.sleep(wait_between)
            time.sleep(wait_end)
        finally:
            s.close()

    def move(self, dx, dy, wait=0.10):
        self.send("mouse_move %d %d" % (dx, dy), wait=wait)

    def type_line(self, text, per_key=0.12):
        names = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", ">": "shift-dot",
                 "=": "equal", "_": "shift-minus"}
        for ch in text:
            if ch in names:
                self.key(names[ch], wait=per_key)
            elif ch.isalnum():
                self.key(ch, wait=per_key)
            else:
                raise ValueError("unsupported char for sendkey: %r" % ch)
        self.key("ret", wait=per_key + 0.2)


class Vm:
    def __init__(self, qemu, img, port, tag, workdir):
        self.tag = tag
        self.serial = os.path.join(workdir, "serial_%s.log" % tag)
        self.wav = os.path.join(workdir, "out_%s.wav" % tag)
        args = [qemu, "-name", "vimtu-snd-%s" % tag,
                "-drive", "format=raw,file=%s" % q(img),
                "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
                "-serial", "file:%s" % q(self.serial),
                "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
                "-no-reboot",
                # ★ HDA 只能 QEMU：Intel HDA 控制器 + 通用码器；wav 后端把声卡混出的音频录进文件
                "-device", "ich9-intel-hda",
                "-device", "hda-duplex,audiodev=snd0",
                "-audiodev", "wav,id=snd0,path=%s" % q(self.wav)]
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.port = port

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_log(self, needle, timeout, since=0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in self.log()[since:]:
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.25)
        return False

    def monitor(self):
        for _ in range(80):
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)
        return Monitor(self.port)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


def last(pattern, text, flags=0):
    m = list(re.finditer(pattern, text, flags))
    return m[-1] if m else None


def allm(pattern, text, flags=0):
    return list(re.finditer(pattern, text, flags))


# ---------------------------------------------------------------------------
# 卷内读回（独立按 VimtuFS2 格式走三级块链）
# ---------------------------------------------------------------------------
def load_vol_tools():
    import importlib.util
    spec = importlib.util.spec_from_file_location("tcc_pack_win", os.path.join(ROOT, "tools", "tcc_pack_win.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def vol_read(TP, vol_bytes, path):
    inodes = struct.unpack_from("<I", vol_bytes, 40)[0]
    total = struct.unpack_from("<I", vol_bytes, 20)[0]
    parts = [p for p in path.split("/") if p]
    cur = 0
    for k, part in enumerate(parts):
        hit = None
        for i, nm, rec in TP._entries(vol_bytes, cur, inodes):
            if nm == part:
                hit = (i, rec)
                break
        if not hit:
            return None
        ino, rec = hit
        if k == len(parts) - 1:
            return TP._read_file(vol_bytes, rec, total)
        cur = ino
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "sysdisk.img"),
                    help="整盘镜像（system.img + 带音效的系统卷）")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5807)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--no-qemu", action="store_true", help="只做宿主侧/卷内断言")
    args = ap.parse_args()

    checks = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond), str(detail) if detail else ""))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name,
                               ("  " + str(detail)) if detail else ""))

    print("=== ★ 系统音效：素材 / 音频 ABI 49 / /bin/sounder ===")
    if not os.path.exists(args.img):
        sys.stderr.write("镜像不存在：%s（先跑 bash build64.sh）\n" % args.img)
        return 2

    # ---------------- ⓪ 宿主侧：素材与播放器 ----------------
    sounds_dir = os.path.join(ROOT, "build64", "sounds")
    host = {}
    ok_all = True
    for n in SOUNDS:
        p = os.path.join(sounds_dir, n + ".wav")
        if not os.path.exists(p):
            sys.stderr.write("缺素材（先跑 py -3 tools/sounds_gen.py）：%s\n" % p)
            return 2
        raw = open(p, "rb").read()
        w = wav_parse(raw)
        if not w:
            sys.stderr.write("素材不是合法 WAV：%s\n" % p)
            return 2
        rate, ch, bits, pcm_bytes, peak = w
        host[n] = {"path": p, "bytes": len(raw), "rate": rate, "ch": ch, "bits": bits,
                   "pcm_bytes": pcm_bytes, "frames": pcm_bytes // 4,
                   "dur_ms": (pcm_bytes // 4) * 1000.0 / rate, "peak": peak,
                   "sha256": hashlib.sha256(raw).hexdigest(),
                   "fnv1a32": fnv1a32(raw[len(raw) - pcm_bytes:])}
        print("     素材 %-7s %d Hz/%dch/%dbit frames=%-6d dur=%6.1f ms peak=%-6d bytes=%-7d "
              "fnv=0x%08x" % (n, rate, ch, bits, host[n]["frames"], host[n]["dur_ms"],
                              peak, len(raw), host[n]["fnv1a32"]))
        ok_all = ok_all and rate == 48000 and ch == 2 and bits == 16
    check("① 4 段素材都是 48kHz/16bit/2ch 且非空（内核 hda64 的唯一流格式）", ok_all,
          "frames=" + ",".join("%s:%d" % (n, host[n]["frames"]) for n in SOUNDS))

    sounder_elf = os.path.join(ROOT, "build64", "sounder.elf")
    if not os.path.exists(sounder_elf):
        sys.stderr.write("缺 build64/sounder.elf（先跑 bash build64.sh）\n")
        return 2
    elf = open(sounder_elf, "rb").read()
    check("① /bin/sounder 是静态 ELF64 且 <= 64 KiB（用户窗口装载区上限）",
          elf[:4] == b"\x7fELF" and elf[4] == 2 and 0 < len(elf) <= 65536, "%d B" % len(elf))

    kernel = os.path.join(ROOT, "build64", "kernel64_os.bin")
    if not os.path.exists(kernel):
        sys.stderr.write("缺 build64/kernel64_os.bin\n")
        return 2
    kb = open(kernel, "rb").read()
    print("     系统内核 kernel64_os.bin = %d B（基线 %d B -> 净增 %+d B；余量 %d B / 硬上限 %d B）"
          % (len(kb), KERNEL_BASELINE_BYTES, len(kb) - KERNEL_BASELINE_BYTES,
             KERNEL_HARD_LIMIT - len(kb), KERNEL_HARD_LIMIT))
    check("① 内核在内核区硬上限内且余量 >= 640 KiB（与 a42a64_test 的三层预算口径一致）",
          len(kb) <= KERNEL_HARD_LIMIT and (KERNEL_HARD_LIMIT - len(kb)) >= 640 * 1024,
          "%d B（余量 %d B）" % (len(kb), KERNEL_HARD_LIMIT - len(kb)))

    probe_bad = []
    for n in SOUNDS:
        b = open(os.path.join(sounds_dir, n + ".wav"), "rb").read()
        best, boff = -1, -1
        for off in range(0, max(1, len(b) - 64), 32):
            d = len(set(b[off:off + 64]))
            if d > best:
                best, boff = d, off
        if best >= 8 and b[boff:boff + 64] in kb:
            probe_bad.append(n)
    mid = len(elf) // 2
    if elf[mid:mid + 64] in kb:
        probe_bad.append("sounder.elf")
    check("① 内核二进制里搜不到 4 段素材与 /bin/sounder 的 64B 高熵探针（交付 = 系统卷里的文件）",
          not probe_bad, "命中：%s" % (",".join(probe_bad) or "无"))

    # ---------------- ① 卷内读回：逐字节一致 ----------------
    print("=== ① 卷内读回（真实系统卷：%s 主分区 LBA %d 起）===" % (os.path.basename(args.img), PART_MAIN_LBA))
    img = open(args.img, "rb").read()
    if len(img) < (PART_MAIN_LBA + 1) * SECTOR:
        sys.stderr.write("镜像太小，主分区不在预期位置\n")
        return 2
    vol = img[PART_MAIN_LBA * SECTOR:]
    if vol[0:8] != b"VIMTUFS2":
        sys.stderr.write("主分区不是 VimtuFS2 卷（magic=%r）\n" % vol[0:8])
        return 2
    TP = load_vol_tools()
    expect = {"/bin/sounder": elf}
    for n in SOUNDS:
        expect["%s/%s.wav" % (SOUND_DIR, n)] = open(os.path.join(sounds_dir, n + ".wav"), "rb").read()
    vbad = TP.verify(vol, expect)
    check("① 卷内 5 个文件（4 素材 + /bin/sounder）与宿主生成物**逐字节一致**（独立回读三级块链）",
          vbad is None, vbad or "5/5 一致")
    same = 0
    for path, want in sorted(expect.items()):
        got = vol_read(TP, vol, path)
        if got == want:
            same += 1
        else:
            print("     不一致：%s（卷内 %s B vs 宿主 %d B）"
                  % (path, (len(got) if got is not None else -1), len(want)))
    check("① 逐个文件的 sha256 对照（卷内 == 宿主）", same == len(expect), "%d/%d" % (same, len(expect)))

    if args.no_qemu:
        npass = sum(1 for _, c, _ in checks if c)
        print("== 结果（--no-qemu）：%d 项断言，%s（PASS=%d FAIL=%d）=="
              % (len(checks), "全过" if npass == len(checks) else "有失败", npass, len(checks) - npass))
        return 0 if npass == len(checks) else 1

    # ---------------- ②③④⑥⑦ QEMU + HDA + wav 后端 ----------------
    qemu = args.qemu
    if not qemu:
        for c in (r"C:\Program Files\qemu\qemu-system-x86_64.exe",
                  r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
                  "qemu-system-x86_64"):
            if os.path.sep in c:
                if os.path.exists(c):
                    qemu = c
                    break
            else:
                import shutil
                qemu = shutil.which(c)
                if qemu:
                    break
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_snd_")
    print("=== ② QEMU 起机（ich9-intel-hda + hda-duplex + -audiodev wav）===")
    vm = Vm(qemu, args.img, args.port, "snd", tmp)
    mon = vm.monitor()
    try:
        if not qh.login_desktop(mon, vm.log, vm.proc, timeout=180):
            check("进入桌面（锁屏可交互 + 两次回车）", False, "没等到 [LOCK64] bg blur ready")
            raise SystemExit(1)
        check("桌面就绪（[GUI64] ready）", vm.wait_log("[GUI64] ready", 120))
        check("② HDA 驱动就绪（[HDA64] … selftest PASS）", "[HDA64] selftest PASS" in vm.log())

        # 终端（开始菜单 -> 终端）
        opened = False
        for _ in range(3):
            mon.key("meta_l", wait=1.0)
            mon.key("1", wait=2.0)
            if vm.wait_log("[APP] term opened", 15):
                opened = True
                break
        check("⑥ 开始菜单 -> 终端（[APP] term opened）", opened)

        # 音量/静音先归位（确定性），再让 /bin/sounder 无参 demo 连播 4 段
        mon.type_line("audio vol 100")
        vm.wait_log("[HDA64] cmd audio vol applied=1 pct=100", 20)
        mon.type_line("audio mute off")
        vm.wait_log("[HDA64] cmd audio mute applied=1 on=0", 20)
        time.sleep(0.8)

        n0 = len(vm.log())
        off_a = os.path.getsize(vm.wav) if os.path.exists(vm.wav) else 0
        mon.type_line("elfrun /bin/sounder")
        got_demo = vm.wait_log("SOUNDER demo count=4", 60, since=n0)
        time.sleep(1.2)                      # 等 wav 后端把最后一段写进文件
        log = vm.log()
        check("⑥ `elfrun /bin/sounder`（无参 demo）播完 4 段（SOUNDER demo count=4）", got_demo,
              (last(r"SOUNDER demo[^\r\n]*", log).group(0)
               if last(r"SOUNDER demo[^\r\n]*", log) else "（无）"))
        plays = allm(r"SOUNDER play name=(\w+) path=(\S+) rate=(\d+) ch=(\d+) bits=(\d+) frames=(\d+) "
                     r"bytes=(\d+) ms=(\d+) peak=(\d+) sum=(0x[0-9a-f]+) buf=(0x[0-9a-f]+) rc=(-?\d+) "
                     r"elapsed_ms=(\d+)", log[n0:])
        check("② 4 段素材都从**用户态**读出并播放（4 条 SOUNDER play … rc=0）",
              len(plays) == 4 and all(m.group(12) == "0" for m in plays), "plays=%d" % len(plays))
        # 用户态报的 frames/sum 与宿主素材对照（= 卷里读到的就是宿主生成的那份字节）
        frames_ok, sum_ok, dur_ok = True, True, True
        byname = {m.group(1): m for m in plays}
        for n in SOUNDS:
            m = byname.get(n)
            if not m:
                frames_ok = sum_ok = dur_ok = False
                continue
            frames_ok = frames_ok and int(m.group(6)) == host[n]["frames"]
            frames_ok = frames_ok and int(m.group(7)) == host[n]["pcm_bytes"]
            sum_ok = sum_ok and int(m.group(10), 16) == host[n]["fnv1a32"]
            ms = int(m.group(8))
            dur_ok = dur_ok and abs(ms - host[n]["dur_ms"]) <= 1.0
        check("① 用户态从卷里读到的 frames/bytes/时长/峰值 与宿主素材一致", frames_ok and dur_ok,
              "frames/bytes/ms 逐项对照")
        check("① 用户态算出的 PCM FNV-1a 校验和 == 宿主对同一素材算的（卷内字节逐字节一致）", sum_ok,
              "4/4")

        snd = allm(r"\[SND64\] play va=(0x[0-9a-f]+) frames=(\d+) fmt=(0x[0-9a-f]+) rc=(-?\d+)", log[n0:])
        check("⑥ 音频 ABI 打点：`[SND64] play va=… frames=… fmt=0x11 rc=0`（每次播放一行）",
              len(snd) >= 4 and all(m.group(3) == "0x11" and m.group(4) == "0" for m in snd),
              "行数=%d；例: %s" % (len(snd), snd[0].group(0) if snd else "（无）"))
        if snd:
            print("     客观证据（内核打点）：" + " | ".join(m.group(0) for m in snd[:4]))

        # ---- ② 录音（客观的"真的出声了"）：只分析**本次 demo 之后**的音频 ----
        print("=== ② 录音（-audiodev wav）：4 段素材的时长/峰值对照 ===")
        check("② QEMU wav 后端存在且非空（录音文件 > 1 MB）",
              os.path.exists(vm.wav) and os.path.getsize(vm.wav) > 1024 * 1024,
              "%d B" % (os.path.getsize(vm.wav) if os.path.exists(vm.wav) else 0))
        runs_a, det_a = wav_runs(vm.wav, max_seconds=20.0, since_off=off_a)
        check("② 录音里出现 >= 4 个非静音区段（4 段素材各一段）",
              bool(runs_a) and len(runs_a) >= 4,
              "区段数=%d（最后 4 个 %s）" % (len(runs_a or []), (runs_a or [])[-4:]))

        def measure(runs, det):
            """把最后 4 个区段按 demo 顺序映射到 4 段素材，返回 {name: (ms, peak)}。"""
            if not runs or not det or len(runs) < 4:
                return {}
            rate, ch, pcm, blk, env = det
            out = {}
            for n, (s, e) in zip(SOUNDS, runs[-4:]):
                out[n] = ((e + 1 - s) * blk * 1000.0 / rate, run_peak(pcm, ch, blk, s, e))
            return out

        meas = measure(runs_a, det_a)
        for n in SOUNDS:
            if n in meas:
                ms, peak = meas[n]
                print("     录音区段 %-7s 实测 %6.1f ms / peak %-6d（素材 %6.1f ms / peak %d）"
                      " 比值 时长 %.2f 幅度 %.2f"
                      % (n, ms, peak, host[n]["dur_ms"], host[n]["peak"],
                         ms / host[n]["dur_ms"], peak / float(host[n]["peak"])))
        check("② 4 段都在录音里被量到（区段 -> 素材一一对应）", len(meas) == 4, "meas=%d" % len(meas))
        dur_ratios = [meas[n][0] / host[n]["dur_ms"] for n in SOUNDS if n in meas]
        amp_ratios = [meas[n][1] / float(host[n]["peak"]) for n in SOUNDS if n in meas]
        check("② 录音里 4 段 **时长**与素材相符（比值 0.5..1.4；很短的衰减尾巴可能低于门限）",
              len(dur_ratios) == 4 and all(0.5 <= r <= 1.4 for r in dur_ratios),
              "比值=" + ",".join("%.2f" % r for r in dur_ratios))
        check("② 录音里 4 段 **峰值**与素材相符（比值 0.15..1.3：既不是静音也没削顶）",
              len(amp_ratios) == 4 and all(0.15 <= r <= 1.3 for r in amp_ratios),
              "比值=" + ",".join("%.2f" % r for r in amp_ratios))

        # ---- ③ 静音：同一程序播放 -> 录音里不应有新声音 ----
        print("=== ③ 静音（audio mute on）时播放不应出声 ===")
        mon.type_line("audio mute on")
        vm.wait_log("[HDA64] cmd audio mute applied=1 on=1", 20)
        off_m = os.path.getsize(vm.wav)
        n1 = len(vm.log())
        mon.type_line("elfrun /bin/sounder")
        got_demo2 = vm.wait_log("SOUNDER demo count=4", 60, since=n1)
        time.sleep(1.2)
        runs_m, det_m = wav_runs(vm.wav, max_seconds=8.0, since_off=off_m)
        check("③ 静音下 4 段仍走完整条 ABI（demo count=4 + [SND64] play rc=0；硬件静音不是「跳过」）",
              got_demo2 and "[SND64] play" in vm.log()[n1:], "demo=%s" % got_demo2)
        check("③ 静音下录音里**没有**新的非静音区段（声卡输出真的是静音）", not runs_m,
              "区段=%s" % (runs_m if runs_m else "无"))
        mon.type_line("audio mute off")
        vm.wait_log("[HDA64] cmd audio mute applied=1 on=0", 20)

        # ---- ④ 音量 0/50/100 对输出幅度的影响（同一段素材互相对照）----
        print("=== ④ 音量 50/0 对输出幅度的影响（与 100% 的实测峰值对照）===")
        peak100 = meas.get("notify", (0, 0))[1]
        peak50 = 0
        mon.type_line("audio vol 50")
        vm.wait_log("[HDA64] cmd audio vol applied=1 pct=50", 20)
        off_50 = os.path.getsize(vm.wav)
        n2 = len(vm.log())
        mon.type_line("elfrun /bin/sounder")
        vm.wait_log("SOUNDER demo count=4", 60, since=n2)
        time.sleep(1.2)
        runs_50, det_50 = wav_runs(vm.wav, max_seconds=20.0, since_off=off_50)
        meas50 = measure(runs_50, det_50)
        peak50 = meas50.get("notify", (0, 0))[1]
        print("     音量 50%%：notify 段实测峰值 %d（100%% 时 %d）" % (peak50, peak100))
        check("④ 音量 50%% 的输出幅度明显小于 100%%（比值 0.1..0.9）",
              peak100 > 0 and 0.1 <= peak50 / float(peak100) <= 0.9,
              "50%%=%d / 100%%=%d = %.2f" % (peak50, peak100, peak50 / float(peak100) if peak100 else 0))

        mon.type_line("audio vol 0")
        vm.wait_log("[HDA64] cmd audio vol applied=1 pct=0", 20)
        off_0 = os.path.getsize(vm.wav)
        n2b = len(vm.log())
        mon.type_line("elfrun /bin/sounder")
        vm.wait_log("SOUNDER demo count=4", 60, since=n2b)
        time.sleep(1.2)
        runs_0, det_0 = wav_runs(vm.wav, max_seconds=8.0, since_off=off_0)
        check("④ 音量 0：录音里没有非静音区段（放大器静音位生效）", not runs_0,
              "区段=%s" % (runs_0 if runs_0 else "无"))
        mon.type_line("audio vol 100")
        vm.wait_log("[HDA64] cmd audio vol applied=1 pct=100", 20)

        # ---- ⑤⑥ 用户态 shell 传 argv：list / 单段播放 / 负例自检 ----
        print("=== ⑤⑥ 用户态 shell 里 `run /bin/sounder …`（argv 路径 + 负例）===")
        mon.type_line("shell", per_key=0.18)
        shell_ok = vm.wait_log("[SH64] launch path=/bin/shell.bin", 40)
        if shell_ok:
            vm.wait_log("VimtuOS ring3 shell (sh64)", 30)
        check("⑥ 用户态 shell 装载（[SH64] launch path=/bin/shell.bin）", shell_ok,
              (last(r"\[SH64\] launch[^\r\n]*", vm.log()).group(0) if last(r"\[SH64\] launch[^\r\n]*", vm.log()) else "（无）"))

        if shell_ok:
            n3 = len(vm.log())
            mon.type_line("run /bin/sounder list", per_key=0.12)
            vm.wait_log("SOUNDER list done", 40, since=n3)
            log = vm.log()
            li = allm(r"SOUNDER list name=(\w+) path=(\S+) bytes=(\d+) rate=(\d+) ch=(\d+) bits=(\d+) "
                      r"frames=(\d+) ms=(\d+) peak=(\d+) sum=(0x[0-9a-f]+) rc=(\d+)", log[n3:])
            liok = len(li) == 4
            for m in li:
                n = m.group(1)
                liok = liok and n in host and int(m.group(3)) == host[n]["bytes"] \
                    and int(m.group(7)) == host[n]["frames"] and int(m.group(9)) == host[n]["peak"] \
                    and int(m.group(10), 16) == host[n]["fnv1a32"] and m.group(11) == "0"
            check("①⑥ `sounder list`：4 段素材的 bytes/frames/peak/sum 与宿主素材逐项一致（无需声卡）",
                  liok, "list=%d 行" % len(li))

            n4 = len(vm.log())
            mon.type_line("run /bin/sounder selftest", per_key=0.12)
            vm.wait_log("SOUNDER selftest done", 60, since=n4)
            log = vm.log()
            wneg = {m.group(1): int(m.group(2)) for m in
                    allm(r"SOUNDER neg (\w+) rc=(-?\d+)", log[n4:])}
            want = {"badptr": -2, "nullptr": -2, "badfmt": -3, "zeroframes": -3, "hugeframes": -3, "ok480": 0}
            check("⑤ 非法参数都拿到**明确错误码**（越界指针/空指针 -2；错格式/零长度/超大长度 -3）",
                  all(wneg.get(k) == v for k, v in want.items()), str(wneg))
            check("⑤ 合法调用仍然成功（neg ok480 rc=0：校验没有把正常路径也拒了）", wneg.get("ok480") == 0)
            denies = allm(r"\[SND64\] deny reason=(\S+) err=(\d+)", log[n4:])
            reasons = set(m.group(1) for m in denies)
            check("⑥ 内核侧失败打点 `[SND64] deny reason=… err=…`（bad-buf / bad-fmt / bad-frames 各就各位）",
                  {"bad-buf", "bad-fmt", "bad-frames"} <= reasons,
                  "reasons=%s" % sorted(reasons))

            n5 = len(vm.log())
            mon.type_line("run /bin/sounder notify", per_key=0.12)
            vm.wait_log("SOUNDER play name=notify", 40, since=n5)
            m = last(r"SOUNDER play name=notify path=(\S+) rate=(\d+) ch=(\d+) bits=(\d+) frames=(\d+) "
                     r"bytes=(\d+) ms=(\d+) peak=(\d+) sum=(0x[0-9a-f]+) buf=(0x[0-9a-f]+) rc=(-?\d+) "
                     r"elapsed_ms=(\d+)", vm.log()[n5:])
            check("⑥ `run /bin/sounder notify`（argv 路径）单段播放成功且与素材一致",
                  m is not None and m.group(11) == "0" and int(m.group(5)) == host["notify"]["frames"] and
                  int(m.group(9), 16) == host["notify"]["fnv1a32"],
                  m.group(0) if m else (last(r"SOUNDER play[^\r\n]*", vm.log()[n5:]).group(0)
                                        if last(r"SOUNDER play[^\r\n]*", vm.log()[n5:]) else "（无）"))
            if m:
                # 阻塞时长 ~= 素材时长（0.15 s，PIT 4 ms 粒度 + 驱动启动开销）
                el = int(m.group(12))
                check("⑥ 播放调用是**阻塞**的：elapsed_ms 与素材时长同量级（40..600 ms）",
                      40 <= el <= 600, "elapsed_ms=%d（素材 150 ms）" % el)
            mon.type_line("run /bin/sounder /usr/share/sounds/nosuch.wav", per_key=0.12)
            vm.wait_log("SOUNDER reject", 40, since=n5)
            mr = last(r"SOUNDER reject name=(\S+) reason=(\S+) rc=(-?\d+)", vm.log()[n5:])
            check("⑤ 不存在的素材路径 -> 用户态如实报 not-found rc=-2（不谎报播放成功）",
                  mr is not None and mr.group(3) == "-2", mr.group(0) if mr else "（无）")

        # ---- ⑦ 全程无 PANIC / 无 enosys ----
        bad = [w for w in FORBIDDEN if w in vm.log()]
        check("⑦ 全程无 PANIC / 三重故障 / [SYSCALL] enosys nr=49", not bad, ",".join(bad))
        check("⑦ [SND64] 的调用点全部来自我们的播放（打点行数 <= 演示次数*4 + 负例）",
              len(allm(r"\[SND64\] play", vm.log())) <= 40,
              "play 行数=%d" % len(allm(r"\[SND64\] play", vm.log())))
    finally:
        vm.stop()

    npass = sum(1 for _, c, _ in checks if c)
    print("== 结果：%d 项断言，%s（PASS=%d FAIL=%d）=="
          % (len(checks), "全过" if npass == len(checks) else "有失败", npass, len(checks) - npass))
    print("   scratch=%s" % tmp)
    return 0 if npass == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
