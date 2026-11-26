#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/https64_test.py - ★ v0.12.4：真 HTTPS 打通验收（mbedTLS + 内核 socket 层）

这一项验的是**用户能感知的那一件事**：这台机器上能不能真的用 HTTPS 取回一个网页，
而且**错证书必须被拒**。它不是"库能编出来"的自证。

判据（全部来自串口 + 宿主侧独立证据，一条都不许用"看起来像"代替）：
  ① 熵：`[TLS] entropy hw-poll selftest=PASS … differ=1 len0_ok=1 nonzero=1`
     —— 证明 mbedTLS 的熵源真的接在我们自己的 CSPRNG（kernel/rand64.cpp）上；
  ② 时间：`[TIME64] clock_gettime realtime anchor=rtc epoch=<n>`，且 n 与宿主 time.time()
     相差 < 2 天 —— 证书有效期判定必须有真墙钟（这版从 CMOS RTC 取，SNTP 是下一步）；
  ③ CA：`[TLS] ca … parse_ret=0 certs>=80`；
  ④ DNS + TCP：`[TLS] pos dns host=example.com ip=<IPv4>` / `[TLS] pos tcp connected`；
  ⑤ 握手：`[TLS] pos handshake ok version=TLS1.2 suite=…` + `verify_flags=0x00000000`；
  ⑥ 取回：`[TLS] pos http status="HTTP/1.1 200` 且 `body_bytes>0`；
  ⑦ **负向对照**：一个自签证书的 https 服务（宿主上跑，客人从 10.0.2.2 连）
     **必须**握手失败并且失败原因是证书校验 —— 同时宿主的那个服务必须**真的收到过连接**
     （否则"失败"可能只是没连上，那不算数）；
  ⑧ 无 PANIC / 无 ring3 #PF，且跑完桌面照常起（`[GUI64] ready`）。

为什么全程不用键盘：QEMU 的 sendkey 在 1 fps 的 GUI 下会丢字、会吞长命令（v0.12.2 的两条下载
判据就是因为这个一直 UNVERIFIED）。这里改成"夹具卷放 /etc/tls_probe.on + /etc/tlsprobe.conf，
内核启动期自己跑探针"，验收只读串口。

用法：
  py -3 tests/https64_test.py                      # 自动造夹具盘 + 起本地 https + 跑 QEMU
  py -3 tests/https64_test.py --fixture X.img      # 用现成夹具盘（跳过造盘）
  py -3 tests/https64_test.py --keep               # 保留临时目录（默认保留，方便查串口日志）

退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题（没 QEMU / 没外网 / 没 openssl / 构件缺失）
"""
import argparse
import os
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import http.server
import socketserver

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# 控制台是 GBK 时，直接 print 子进程的中文输出会 UnicodeEncodeError（实测）。这里把
# 本进程的 stdout/stderr 钉成 utf-8 + replace：宁可显示成乱码，也不许因为"打日志"把验收炸掉。
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:                                   # noqa: BLE001
        pass
BUILD = os.path.join(ROOT, "build64")

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]
OPENSSL_CANDIDATES = [
    r"C:\msys64\usr\bin\openssl.exe",
    "openssl",
]

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, bool(ok), detail))
    print("  %s %s%s" % ("PASS" if ok else "FAIL", name, ("  [" + detail + "]") if detail else ""))
    return ok


def find_exe(cands, explicit=None):
    if explicit and os.path.exists(explicit):
        return explicit
    for c in cands:
        if os.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            f = shutil.which(c)
            if f:
                return f
    return None


def q(p):
    return p.replace("\\", "/")


def py_exe():
    if shutil.which("py"):
        return ["py", "-3"]
    return [sys.executable]


# ==================== 宿主侧：自签证书的 https 服务（负向对照）====================
class NegHandler(http.server.BaseHTTPRequestHandler):
    hits = 0

    def do_GET(self):                                   # noqa: N802 - 标准名
        NegHandler.hits += 1
        body = b"self-signed: this must NOT be trusted\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):                          # 静音
        pass


class NegServer(threading.Thread):
    conns = 0            # 宿主侧收到的 TCP 连接数（独立于客人自报）
    tls_aborts = 0       # 其中在 TLS 握手阶段被客人中止的次数

    def __init__(self, port, certfile, keyfile):
        super().__init__(daemon=True)
        self.port, self.certfile, self.keyfile = port, certfile, keyfile
        self.httpd = None
        self.error = None

    def run(self):
        try:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(self.certfile, self.keyfile)

            class S(socketserver.ThreadingTCPServer):
                allow_reuse_address = True
                daemon_threads = True

                # ★ 宿主侧独立证据：**每一个 TCP 连接**都记一次（在 TLS 握手之前）。
                #   为什么不能只数 HTTP GET：负向对照里客人握手中途就因证书校验失败而中止，
                #   根本走不到 GET（第一版只数 GET -> hits=0 -> 假红，实际连接是通的）。
                def get_request(self):
                    NegServer.conns += 1
                    try:
                        return super().get_request()
                    except Exception:                   # noqa: BLE001
                        NegServer.tls_aborts += 1       # 客人在握手中途走了（正是我们要的）
                        raise

            self.httpd = S(("0.0.0.0", self.port), NegHandler)
            self.httpd.socket = ctx.wrap_socket(self.httpd.socket, server_side=True)
            self.httpd.serve_forever(poll_interval=0.2)
        except Exception as e:                          # noqa: BLE001
            self.error = "%s: %s" % (type(e).__name__, e)

    def stop(self):
        if self.httpd:
            try:
                self.httpd.shutdown()
                self.httpd.server_close()
            except Exception:                           # noqa: BLE001
                pass


def make_selfsigned(openssl, workdir):
    cert = os.path.join(workdir, "neg_cert.pem")
    key = os.path.join(workdir, "neg_key.pem")
    cmd = [openssl, "req", "-x509", "-newkey", "rsa:2048", "-sha256", "-days", "2",
           "-nodes", "-keyout", key, "-out", cert,
           "-subj", "/C=XX/O=VimtuOS-SelfSigned-Host/CN=10.0.2.2"]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0 or not os.path.exists(cert):
        print(r.stdout.decode("utf-8", "replace"))
        return None, None
    return cert, key


# ==================== 造夹具盘 ====================
def build_fixture(workdir, neg_port):
    vol = os.path.join(BUILD, "tlsvol.img")
    probe = os.path.join(BUILD, "httpsprobe.elf")
    system = os.path.join(BUILD, "system.img")
    for p in (vol, probe, system):
        if not os.path.exists(p):
            return None, "缺少构件：%s（先跑 bash build64.sh）" % p
    conf = os.path.join(workdir, "tlsprobe.conf")
    with open(conf, "w", encoding="utf-8", newline="\n") as f:
        f.write("host=example.com\n")
        f.write("port=443\n")
        f.write("path=/\n")
        f.write("ca=/etc/ssl/certs/ca.pem\n")
        f.write("negaddr=10.0.2.2\n")               # QEMU slirp：10.0.2.2 = 宿主
        f.write("negport=%d\n" % neg_port)
    fixture = os.path.join(workdir, "tls_fixture.img")
    cmd = py_exe() + [os.path.join(ROOT, "tools", "tls_pack_win.py"),
                      "--fixture-img", fixture,
                      "--from-vol", vol,
                      "--probe", probe,
                      "--conf", conf,
                      "--on",
                      "--system", system]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = r.stdout.decode("utf-8", "replace")
    print(out.rstrip())
    if r.returncode != 0 or not os.path.exists(fixture):
        return None, "夹具盘生成失败（tls_pack_win.py rc=%d）" % r.returncode
    return fixture, None


# ==================== 跑 QEMU ====================
def run_vm(qemu, img, serial, timeout):
    if os.path.exists(serial):
        os.remove(serial)
    proc = subprocess.Popen([
        qemu, "-name", "vimtu-https64",
        "-drive", "format=raw,file=%s" % q(img),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none", "-serial", "file:%s" % q(serial),
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        "-no-reboot",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def read():
        try:
            with open(serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    t0 = time.time()
    done = False
    while time.time() - t0 < timeout:
        log = read()
        if "===== probe done" in log or "[TLS64] probe done" in log:
            done = True
            break
        if proc.poll() is not None:
            break
        time.sleep(0.5)
    # 探针跑完 -> 内核接着起桌面。**等 GUI64 ready 再杀**：否则"桌面照常起"这条判据
    # 会因为我们自己杀太早而假红（探针在 net64_init64 之后、gui64_run 之前跑）。
    if done:
        t1 = time.time()
        while time.time() - t1 < 240:
            # ★ 等的是"桌面栈起来"，不是"登录完成"：`[GUI64] ready` 要等用户登录（本验收**不发
            #   任何键盘事件**，所以永远不会出现）。真正该等的是 desktop init + 锁屏上屏。
            if "[LOCK64] lock screen shown" in read():
                break
            if proc.poll() is not None:
                break
            time.sleep(0.5)
    time.sleep(1.0)
    if proc.poll() is None:
        proc.kill()
    try:
        proc.wait(timeout=10)
    except Exception:                                   # noqa: BLE001
        pass
    return read(), done, time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--fixture", default=None, help="现成夹具盘（跳过造盘）")
    ap.add_argument("--timeout", type=int, default=420, help="等探针跑完的最长秒数")
    ap.add_argument("--neg-port", type=int, default=8443)
    ap.add_argument("--workdir", default=None)
    ap.add_argument("--keep", action="store_true", help="保留临时目录（默认就保留，便于查日志）")
    args = ap.parse_args()

    qemu = find_exe(QEMU_CANDIDATES, args.qemu)
    if not qemu:
        print("环境问题：找不到 qemu-system-x86_64")
        return 2
    openssl = find_exe(OPENSSL_CANDIDATES)
    if not openssl:
        print("环境问题：找不到 openssl（负向对照要一份自签证书）")
        return 2

    workdir = args.workdir or tempfile.mkdtemp(prefix="vimtu64_https_")
    print("== VimtuOS HTTPS 探针验收 ==")
    print("   workdir = %s" % workdir)

    cert, key = make_selfsigned(openssl, workdir)
    if not cert:
        print("环境问题：自签证书生成失败")
        return 2
    srv = NegServer(args.neg_port, cert, key)
    srv.start()
    time.sleep(1.0)
    if srv.error:
        print("环境问题：本地 https 服务起不来（%s）" % srv.error)
        return 2
    print("   负向对照服务：https://0.0.0.0:%d（自签，CN=10.0.2.2）" % args.neg_port)

    fixture = args.fixture
    if fixture:
        print("   夹具盘（现成）= %s" % fixture)
    else:
        fixture, err = build_fixture(workdir, args.neg_port)
        if err:
            print(err)
            srv.stop()
            return 2

    serial = os.path.join(workdir, "https_serial.log")
    print("   启动 QEMU（串口 -> %s），等探针跑完 …" % serial)
    log, done, secs = run_vm(qemu, fixture, serial, args.timeout)
    srv.stop()
    print("   QEMU 退出，用时 %.1f s，串口 %d 字节，探针 %s"
          % (secs, len(log), "跑完" if done else "**没跑完（超时）**"))

    with open(os.path.join(workdir, "https_serial_copy.log"), "w", encoding="utf-8") as f:
        f.write(log)

    # ==================== 断言 ====================
    print("\n-- 判据 --")
    host_now = time.time()

    check("内核启动期识别触发开关（[TLS64] probe on）",
          "[TLS64] probe on flag=/etc/tls_probe.on" in log)
    check("探针进程正常返回（[TLS64] probe done pid-exit rc=0 code=0）",
          "[TLS64] probe done pid-exit rc=0 code=0" in log,
          (log.split("[TLS64] probe done")[-1][:28] if "[TLS64] probe done" in log else "缺"))

    m = None
    import re
    m = re.search(r"\[TIME64\] clock_gettime realtime anchor=rtc epoch=(\d+)", log)
    ok_time = False
    detail = "缺"
    if m:
        epoch = int(m.group(1))
        delta = abs(host_now - epoch)
        ok_time = delta < 2 * 86400
        detail = "epoch=%d 与宿主差 %.0f s" % (epoch, host_now - epoch)
    check("CLOCK_REALTIME 是**真墙钟**（RTC 锚点，与宿主差 < 2 天）", ok_time, detail)

    m = re.search(r"\[TLS\] entropy hw-poll selftest=(\w+) .*?differ=(\d) len0_ok=(\d) nonzero=(\d)", log)
    check("mbedTLS 熵源接在我们自己的 CSPRNG 上（自检 PASS 且两次输出不同）",
          m is not None and m.group(1) == "PASS" and m.group(2) == "1"
          and m.group(3) == "1" and m.group(4) == "1",
          (m.group(0)[:72] if m else "缺"))

    m = re.search(r"\[TLS\] drbg seeded random=([0-9a-f]+)", log)
    check("CTR_DRBG 播种成功且输出非全 0",
          m is not None and set(m.group(1)) != {"0"}, (m.group(1) if m else "缺"))

    m = re.search(r"\[TLS\] ca path=(\S+) bytes=(\d+) parse_ret=(-?\d+) certs=(\d+)", log)
    check("CA 包解析成功且份数合理（>= 80）",
          m is not None and m.group(3) == "0" and int(m.group(4)) >= 80,
          (m.group(0)[:80] if m else "缺"))

    # ★ 宿主侧独立判据：客人报的 FNV-1a64 必须与宿主对 build64/ca.pem 算的一致。
    #   这条把"客人从 VFS 读到的 CA 包内容是否正确"钉死（大文件走二级间接）。
    ca_host = os.path.join(BUILD, "ca.pem")
    m = re.search(r"\[TLS\] ca fnv1a=([0-9a-f]{16})", log)
    if os.path.exists(ca_host):
        h = 0xCBF29CE484222325
        with open(ca_host, "rb") as f:
            for b in f.read():
                h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
        want = "%016x" % h
        check("客人读到的 CA 包与宿主逐字节一致（FNV-1a64 相同）",
              m is not None and m.group(1).lower() == want,
              ("guest=%s host=%s" % (m.group(1) if m else "缺", want)))
    else:
        check("客人读到的 CA 包与宿主逐字节一致（FNV-1a64 相同）", False, "缺 build64/ca.pem")

    m = re.search(r"\[TLS\] pos dns host=(\S+) ip=(\d+\.\d+\.\d+\.\d+)", log)
    check("域名解析出 IPv4（musl getaddrinfo -> 内核 UDP socket）", m is not None,
          (m.group(0) if m else "缺（外网 DNS 不通时这一条是环境问题）"))

    check("TCP 连接真站点成功", "[TLS] pos tcp connected" in log)

    m = re.search(r"\[TLS\] pos handshake ok version=(\S+) suite=(\S+)", log)
    check("TLS 握手成功（并报出版本与套件）", m is not None,
          (m.group(0)[:96] if m else "缺"))

    m = re.search(r"\[TLS\] pos cert subject=(.+?) issuer=(.+?)", log) or \
        re.search(r"\[TLS\] pos cert subject=(.+)$", log, re.M)
    check("拿到对端证书主体（CN 里含 example.com）",
          m is not None and "example.com" in m.group(1).lower(), (m.group(1).strip() if m else "缺"))

    m = re.search(r"\[TLS\] pos cert valid (\S+) \.\. (\S+) verify_flags=0x(\w+)", log)
    check("证书有效期判定通过（verify_flags=0）",
          m is not None and int(m.group(3), 16) == 0,
          (m.group(0)[:96] if m else "缺"))

    m = re.search(r'\[TLS\] pos http status="(HTTP/1\.[01] \d{3}[^"]*)" body_bytes=(\d+)', log)
    check("HTTP 取回成功（状态行是 HTTP/1.x 2xx/3xx 且正文 > 0 字节）",
          m is not None and int(m.group(2)) > 0
          and (m.group(1).startswith("HTTP/1.1 2") or m.group(1).startswith("HTTP/1.1 3")
               or m.group(1).startswith("HTTP/1.0 2")),
          (m.group(0)[:96] if m else "缺"))

    check("正面结果 = pass", "[TLS] RESULT positive=pass" in log)

    # ---- 负向对照 ----
    check("负向对照真的连到了宿主的自签服务（宿主侧计数 > 0）",
          NegServer.conns > 0, "宿主侧 TCP 连接数=%d（TLS 握手中途被中止=%d）"
          % (NegServer.conns, NegServer.tls_aborts))
    check("错证书站点连接成功（说明失败发生在校验、不是没连上）",
          "[TLS] neg tcp connected" in log)
    m = re.search(r"\[TLS\] neg handshake FAILED ret=-0x(\w+)", log)
    check("错证书握手**必须失败**，且错误码是证书校验（-0x2700）",
          m is not None and m.group(1).upper() == "2700",
          ("ret=-0x%s" % m.group(1)) if m else "缺")
    check("错证书的 verify_flags 非 0（如实指出哪里不对）",
          re.search(r"\[TLS\] neg handshake FAILED .*verify_flags=0x0*[1-9a-fA-F]", log) is not None)
    check("负向结果 = pass", "[TLS] RESULT negative=pass" in log)

    check("总结行 = 正面 pass + 负向 pass",
          "===== probe done positive=pass negative=pass =====" in log)
    # 只看**探针开始之后**的日志：启动期有一批"故意触发异常"的自检演示
    # （sig64 会真的打 [SIG64] fault pid=… #PF，那是被测行为，不是故障）。
    seg = log.split("[TLS64] probe on")[-1] if "[TLS64] probe on" in log else log
    check("探针跑完之后没有 PANIC / 没有 ring3 致命异常",
          ("PANIC" not in log) and ("#PF" not in seg)
          and (re.search(r"\[SIG64\] fault", seg) is None))
    check("探针跑完之后桌面栈照常起来（[GUI64] desktop init + [LOCK64] lock screen shown）",
          ("[GUI64] desktop init" in seg) and ("[LOCK64] lock screen shown" in seg))

    # ==================== 汇总 ====================
    bad = [n for n, ok, _ in RESULTS if not ok]
    print("\n-- 汇总 --")
    print("   %d/%d 通过" % (len(RESULTS) - len(bad), len(RESULTS)))
    if bad:
        for n in bad:
            print("   FAIL: %s" % n)
        print("RESULT: FAIL")
        print("   串口日志：%s" % os.path.join(workdir, "https_serial_copy.log"))
        return 1
    print("RESULT: PASS")
    print("   串口日志：%s" % os.path.join(workdir, "https_serial_copy.log"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
