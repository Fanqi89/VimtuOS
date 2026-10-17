# -*- coding: utf-8 -*-
"""tests/browser_http64_test.py —— v0.5.2：ring3 浏览器**真的从网络抓 HTML 并渲染**

判据（串口打点 + 屏幕像素）：
  1) URL 解析：`[BNET] url host=10.0.2.2 port=<p> path=/`
  2) 链路：`[BNET] tcp connect ip=10.0.2.2:<p> ok=1 ms=..`
  3) HTTP：`[BNET] http get status=200 bytes=<n> ms=..`（n 与宿主页字节数一致）
  4) 浏览器接住：`[BROWSER] http ok status=200 bytes=<n>` + `[BROWSER] load url=http://… blocks>=2`
  5) 屏幕像素：页面区出现**深色墨迹**（宿主页里那行 MARKER 文字被画出来了）
  6) 反向用例（不许假装成功）：抓一个没人监听的端口 -> `[BNET] FAIL reason=<...>` +
     `[BROWSER] load … rc=<负>`（显示内置错误页）
  7) 全程无 PANIC / TRIPLE FAULT

网络：QEMU 用户模式网络（`-netdev user,id=n0` + `-device e1000,netdev=n0`），
宿主 HTTP 服务器绑 0.0.0.0:<port>，客人用 slirp 的**宿主别名 10.0.2.2** 访问它。

用法：py -3 tests/browser_http64_test.py [--port 5675]
退出码：0 全过 / 1 有断言失败 / 2 环境问题（QEMU、镜像、端口）。
"""
import argparse
import http.server
import os
import re
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import textstress64_test as T                                    # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYS_DISK = os.path.join(ROOT, "build64", "sysdisk.img")
HOST_ALIAS = "10.0.2.2"          # QEMU slirp 的宿主别名
outdir = os.path.join(os.environ.get("PI_SCRATCH_DIR", ROOT), "browser_http")
os.makedirs(outdir, exist_ok=True)
checks = []
PAGE = """<html><head><title>HTTP MARKER PAGE</title></head><body>
<h1>网络验收页</h1>
<p>MARKER-ZQ7X9 这一行只存在于宿主 HTTP 服务器返回的页面上。</p>
<p>屏幕上能看到 MARKER 字样，就说明 ring3 浏览器真的从网络抓回了 HTML 并排版渲染了。</p>
</body></html>
"""
MARKER_BYTES = len(PAGE.encode("utf-8"))


def check(name, cond, detail=""):
    checks.append((name, bool(cond)))
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""), flush=True)


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = PAGE.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


class NetVm:
    """带 e1000 用户模式网络的 QEMU（复用 tests/textstress64_test 的 Monitor/常量）。"""

    def __init__(self, qemu, img, port, tag):
        self.tmp = os.path.join(tempfile.gettempdir(), "vimtu_bhttp_%d" % port)
        os.makedirs(self.tmp, exist_ok=True)
        self.serial = os.path.join(self.tmp, tag + "_serial.log")
        if os.path.exists(self.serial):
            os.remove(self.serial)
        self.proc = subprocess.Popen([
            qemu, "-name", "vimtu-bhttp",
            "-drive", "format=raw,file=%s" % img.replace("\\", "/"),
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none", "-serial", "file:%s" % self.serial.replace("\\", "/"),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
            "-netdev", "user,id=n0",
            "-device", "e1000,netdev=n0",
            "-no-reboot",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.port = port

    def monitor(self):
        for _ in range(120):
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                return T.Monitor(self.port)
            except OSError:
                time.sleep(0.25)
        return None

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
            time.sleep(0.3)
        return False

    def close(self):
        try:
            self.proc.terminate()
            self.proc.wait(timeout=10)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass


KEYMAP = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", ":": "shift-semicolon"}


def type_line(mon, s, per=0.07):
    for ch in s:
        mon.key(KEYMAP.get(ch, ch), wait=per)


def start_http_server():
    srv = socketserver.TCPServer(("0.0.0.0", 0), Handler)
    srv.allow_reuse_address = True
    port = srv.server_address[1]
    th = threading.Thread(target=srv.serve_forever, daemon=True)
    th.start()
    return srv, port


def dark_pixels(a, x0, y0, x1, y1, step=2):
    dark = 0
    total = 0
    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            px = a[y][x]
            total += 1
            if int(px[0]) < 120 and int(px[1]) < 120 and int(px[2]) < 120:
                dark += 1
    return dark, total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=SYS_DISK)
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5675)
    args = ap.parse_args()

    qemu = T.find_qemu(args.qemu) if hasattr(T, "find_qemu") else args.qemu
    if not qemu or not os.path.exists(args.img):
        print("跳过：qemu 或镜像不可用（%s）" % args.img)
        return 2

    srv, hport = start_http_server()
    print("=== browser_http64_test：宿主 HTTP 0.0.0.0:%d（%d B）-> 客人 %s:%d ==="
          % (hport, MARKER_BYTES, HOST_ALIAS, hport))
    vm = NetVm(qemu, args.img, args.port, "bhttp")
    try:
        mon = vm.monitor()
        if not mon:
            check("连上 monitor", False)
            return 1
        if not vm.wait_log(T.LOCK_INTERACTIVE, 240):
            check("等锁屏", False)
            return 1
        mon.key("ret", 1.4)
        mon.key("ret", 1.4)
        if not vm.wait_log(T.DESKTOP_READY, 180):
            check("进桌面", False)
            return 1
        time.sleep(2.0)
        mon.key("meta_l", 1.2)
        mon.key("1", 2.0)
        check("开场终端（[APP] term opened）", vm.wait_log("[APP] term opened", 25))
        time.sleep(1.2)

        n0 = len(vm.log())
        type_line(mon, "shell")
        mon.key("ret", wait=2.0)
        got_sh = vm.wait_log("[SH64] launch", 60, since=n0)
        check("起 ring3 shell", got_sh)
        if not got_sh:
            print("\n=== RESULT: UNVERIFIED ===（ring3 shell 起不来）")
            return 2
        time.sleep(2.5)

        url = "http://%s:%d/" % (HOST_ALIAS, hport)
        n1 = len(vm.log())
        type_line(mon, "run /bin/browser " + url)
        mon.key("ret", wait=2.0)
        ok_get = vm.wait_log("[BNET] http get", 90, since=n1)
        # 时序：net.c 的 [BNET] http get 在 net_http_get 返回**之前**打，浏览器紧接着才有
        # [BROWSER] http ok / load —— 命中就取切片会截在中间（实测过：页面已画出来但打点没抓到）。
        vm.wait_log("[BROWSER] load", 40, since=n1)
        time.sleep(1.0)
        log = vm.log()[n1:]
        m_url = re.search(r"\[BNET\] url host=(\S+) port=(\d+) path=(\S+)", log)
        check("URL 解析（[BNET] url host=10.0.2.2 port=… path=/）",
              bool(m_url and m_url.group(1) == HOST_ALIAS and m_url.group(2) == str(hport)),
              m_url.group(0) if m_url else "（缺）")
        m_tcp = re.search(r"\[BNET\] tcp connect ip=(\S+):(\d+) ok=(\d+)", log)
        check("TCP 连接成功（[BNET] tcp connect … ok=1）",
              bool(m_tcp and m_tcp.group(3) == "1" and m_tcp.group(1) == HOST_ALIAS),
              m_tcp.group(0) if m_tcp else "（缺）")
        m_get = re.search(r"\[BNET\] http get status=(\d+) bytes=(\d+)", log)
        check("HTTP 200 且字节数为真（与宿主页一致）",
              bool(m_get and m_get.group(1) == "200" and int(m_get.group(2)) == MARKER_BYTES),
              (m_get.group(0) + "（宿主页 %d B）" % MARKER_BYTES) if m_get else "（缺）")
        m_ok = re.search(r"\[BROWSER\] http ok status=(\d+) bytes=(\d+)", log)
        check("浏览器接住响应（[BROWSER] http ok status=200）",
              bool(m_ok and m_ok.group(1) == "200"), m_ok.group(0) if m_ok else "（缺）")
        m_load = re.search(r"\[BROWSER\] load url=(\S+) bytes=(\d+) rc=(-?\d+) blocks=(\d+)", log)
        check("解析成块（blocks>=2、rc=0）",
              bool(m_load and int(m_load.group(4)) >= 2 and m_load.group(3) == "0"),
              m_load.group(0) if m_load else "（缺）")

        # 屏幕：窗口位置 + 页面区墨迹
        m_pos = None
        for mm in re.finditer(r"\[WL64\] surface create id=(\d+) w=(\d+) h=(\d+)[^\r\n]*pos=(-?\d+),(-?\d+)", vm.log()):
            if mm.group(2) == "896" and mm.group(3) == "580":
                m_pos = mm
        if not m_pos:
            check("拿到窗口位置", False)
            return 1
        wx, wy = int(m_pos.group(4)), int(m_pos.group(5))
        time.sleep(2.0)
        f = mon.shot(os.path.join(outdir, "http_page.ppm"))
        a = T.frame_arr(f)
        dark, total = dark_pixels(a, wx + 10, wy + 60, wx + 886, wy + 556)
        print("  [info] 页面区暗像素 %d/%d = %.2f%%" % (dark, total, 100.0 * dark / max(1, total)))
        check("页面区有正文墨迹（网络页被真的画出来了，暗像素 >= 0.2%）",
              dark * 500 >= total, "%d/%d" % (dark, total))

        # ---- 反向用例：没人监听的端口必须失败且显示错误页（不许假装成功）----
        n2 = len(vm.log())
        got_fail = False
        for _attempt in range(3):
            type_line(mon, "run /bin/browser http://%s:1/" % HOST_ALIAS)
            mon.key("ret", wait=2.0)
            got_fail = vm.wait_log("[BROWSER] http FAIL", 45, since=n2)
            if got_fail:
                break
            print("  [info] 反向 run 第 %d 次没被 shell 接住，重发" % (_attempt + 1))
            time.sleep(1.5)
        # 读**完整串口**而不是切片：实测切片（vm.log()[n2:]）在本夹具下会取到空串
        # （QEMU -serial file: 偶发重开）；完整日志里 FAIL 只可能来自这次反向运行 ——
        # 第一次运行上面已经断言过 status=200 成功。
        all_log = vm.log()
        try:
            with open(os.path.join(outdir, "http_serial.log"), "w", encoding="utf-8", errors="replace") as _f:
                _f.write(all_log)
        except OSError:
            pass
        has_bnet_fail = ("[BNET] FAIL reason=" in all_log) or ("reason=connect step=" in all_log)
        m_fail = re.search(r"\[BNET\] FAIL reason=(\w+) step=(\d+)", all_log)
        _dbg = " | ".join(l.strip()[:70] for l in all_log.splitlines() if "[BNET]" in l)[:300]
        check("反向：不可达端口如实失败（[BNET] FAIL reason=.. step=..）",
              bool(got_fail and has_bnet_fail),
              (m_fail.group(0) if m_fail else
               ("[BNET] FAIL 片段存在" if has_bnet_fail else ("（缺）串口 [BNET] 片段: " + _dbg))))
        got_err = got_fail or ("[BROWSER] http FAIL" in vm.log())
        check("反向：浏览器显示错误页并记 rc（[BROWSER] http FAIL rc=..）", got_err,
              (re.search(r"\[BROWSER\] http FAIL[^\r\n]*", vm.log()).group(0) if got_err else "（缺）"))

        for bad in ("PANIC", "TRIPLE FAULT"):
            check("全程不得出现 %s" % bad, bad not in vm.log())
    finally:
        try:
            vm.close()
        except Exception:
            pass
        try:
            srv.shutdown()
        except Exception:
            pass

    bad = [n for n, ok in checks if not ok]
    print("\n=== RESULT: %s ===  checks=%d ok=%d"
          % ("FAIL" if bad else "PASS", len(checks), len(checks) - len(bad)))
    for n in bad:
        print("  FAIL:", n)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
