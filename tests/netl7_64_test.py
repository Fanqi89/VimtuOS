#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/netl7_64_test.py - ★ v0.12.2：DNS 打通 + 大文件下载（应用层验收）

这一项验的是**用户能感知的两件事**，不是在验"能连上"：

  ① 域名解析（这是用户报的 Bug：`ping baidu.com` 报错）
     - 旧的三个根因（两个代码 + 一个环境）：
       a) 卷上**没有 /etc/resolv.conf**：musl 的 getaddrinfo 不内置默认 nameserver，直接失败；
       b) `poll()` 对 socket handle 一律 POLLNVAL（handle 不进 fd 表）→ musl 等不到 DNS 回包；
       c) QEMU 用户网络（slirp）**不转发公网 ICMP** —— 所以"ping 公网域名能变绿"在 slirp 下不可能，
          那一条如实记为环境限制，本脚本只断言"域名**解析**成功"（拿到 A 记录）。
  ② 大文件传输（这是"TCP 能不能扛住几兆到几十兆"的唯一硬判据）
     - 2 MiB 落到卷上 → 客人 `sha256sum` 与**宿主侧** sha256 逐字节一致（证明没有错位/重复/截断）；
     - 16 MiB 走 `-O /dev/null` → **宿主 HTTP 服务端**记录"这次真的发完了 16,777,216 B 且客户端读完",
       同时给出吞吐（这是宿主侧独立证据，不是客人自报）。

用法：py -3 tests/netl7_64_test.py [--img build64/system.img] [--port 5682] [--http-port 5899]
退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题

★ 需要宿主有**外网 DNS**（本脚本用 example.com 做解析判据）。没有外网时第 ① 组会 FAIL，
  这是环境问题而不是产品缺陷 —— 报告里会打印清楚。
"""
import argparse
import hashlib
import http.server
import os
import re
import shutil
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
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


def keys_for(text):
    out = []
    for ch in text:
        if ch == " ":
            out.append("spc")
        elif ch == "-":
            out.append("minus")
        elif ch == ".":
            out.append("dot")
        elif ch == "/":
            out.append("slash")
        elif ch == ":":
            out.append("shift-semicolon")
        elif ch == "_":
            out.append("shift-minus")
        else:
            out.append(ch)
    return out


class Monitor(object):
    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.4):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
        finally:
            s.close()

    def key(self, name, wait=0.9):
        self.send("sendkey %s" % name, wait=wait)

    def type_keys(self, names, wait=0.09):
        for k in names:
            self.key(k, wait=wait)

    def type_line(self, text, settle=1.2, per_key=0.22):
        """先等上一命令的输出停下来再逐键输入。
        实测踩过：紧接着上一条命令滚屏时输入，按键会被吞掉/错位
        （`run /bin/busybox wget …` 变成 `un /bin/busybox wget`）。"""
        time.sleep(settle)
        self.type_keys(keys_for(text), wait=per_key)
        time.sleep(0.5)
        self.key("ret", wait=0.5)


class Vm(object):
    def __init__(self, qemu, img, port, name="vimtu-netl7", workdir=None):
        self.qemu, self.img, self.port = qemu, img, port
        self.tmp = workdir or tempfile.mkdtemp(prefix="vimtu64_l7_")
        self.serial = os.path.join(self.tmp, name + "_serial.log")
        if os.path.exists(self.serial):
            os.remove(self.serial)
        self.proc = subprocess.Popen([
            qemu, "-name", name,
            "-drive", "format=raw,file=%s" % q(img),
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none", "-serial", "file:%s" % q(self.serial),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
            "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
            "-no-reboot",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def wait_monitor(self):
        for _ in range(80):
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)
        return Monitor(self.port)

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
                return needle in self.log()[since:]
            time.sleep(0.3)
        return False

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


# ---------------- 宿主 HTTP 服务（带"真的发完了多少字节"的记录）----------------
class SrvState(object):
    def __init__(self):
        self.lock = threading.Lock()
        self.hits = {}          # path -> dict(bytes=, seconds=, done=)

    def add(self, path, nbytes, seconds, done):
        with self.lock:
            self.hits[path] = {"bytes": nbytes, "seconds": seconds, "done": done}

    def get(self, path):
        with self.lock:
            return self.hits.get(path)


def make_handler(rootdir, state):
    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def log_message(self, *a):
            pass

        def do_GET(self):
            path = self.path.split("?")[0]
            f = os.path.join(rootdir, path.lstrip("/"))
            if not os.path.isfile(f):
                self.send_response(404)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            data = open(f, "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            t0 = time.time()
            sent = 0
            done = False
            try:
                # 分块写，模拟真实服务器节奏；写完才算"客户端读完了"
                view = memoryview(data)
                while sent < len(data):
                    n = min(16384, len(data) - sent)
                    self.wfile.write(view[sent:sent + n])
                    sent += n
                self.wfile.flush()
                done = sent == len(data)
            except Exception:
                done = False
            state.add(path, sent, time.time() - t0, done)

    return H


FAILS = []


def check(name, ok, detail=""):
    print("[%s] %s%s" % ("PASS" if ok else "FAIL", name,
                         ("  |  " + str(detail)) if detail else ""))
    if not ok:
        FAILS.append(name)
    return ok


def lcg_bytes(n, seed=0x1234567):
    out = bytearray(n)
    x = seed
    for i in range(n):
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        out[i] = (x >> 16) & 0xFF
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "system.img"))
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5682)
    ap.add_argument("--http-port", type=int, default=5899)
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu:
        print("环境问题：找不到 qemu-system-x86_64")
        return 2
    if not os.path.exists(args.img):
        print("环境问题：找不到系统镜像 %s（先跑 bash build64.sh）" % args.img)
        return 2

    # ---- 准备夹具 ----
    srvdir = tempfile.mkdtemp(prefix="vimtu64_http_")
    small = lcg_bytes(2 * 1024 * 1024)
    big = lcg_bytes(16 * 1024 * 1024)
    open(os.path.join(srvdir, "b2.bin"), "wb").write(small)
    open(os.path.join(srvdir, "b16.bin"), "wb").write(big)
    sha_small = hashlib.sha256(small).hexdigest()
    sha_big = hashlib.sha256(big).hexdigest()
    del small, big
    print("=== 0) 宿主夹具：b2.bin = 2 MiB sha256=%s… / b16.bin = 16 MiB sha256=%s…"
          % (sha_small[:16], sha_big[:16]))

    state = SrvState()
    handler = make_handler(srvdir, state)
    socketserver.TCPServer.allow_reuse_address = True
    srv = socketserver.ThreadingTCPServer(("127.0.0.1", args.http_port), handler)
    srv.daemon_threads = True
    th = threading.Thread(target=srv.serve_forever, daemon=True)
    th.start()
    print("    宿主 HTTP 服务：http://127.0.0.1:%d/（客人视角 http://10.0.2.2:%d/）"
          % (args.http_port, args.http_port))

    vm = Vm(qemu, args.img, args.port)
    print("    serial = %s" % vm.serial)
    try:
        mon = vm.wait_monitor()
        if not vm.wait_log("[LOCK64] bg blur ready", 240):
            print("环境问题：等不到锁屏就绪")
            return 2
        mon.key("ret", wait=1.0)
        mon.key("ret", wait=1.5)
        if not vm.wait_log("[GUI64] ready", 180):
            print("环境问题：进不了桌面")
            return 2
        check("内核网络已启动（[NET64] 行）", vm.wait_log("[NET64]", 30))

        print("=== 1) 打开终端 -> 进 Ring 3 shell ===")
        mon.key("meta_l", wait=1.2)
        mon.key("1", wait=1.8)
        base = len(vm.log())
        mon.type_keys(keys_for("shell"))
        mon.key("ret", wait=1.2)
        if not check("进入 Ring 3 shell（[ELF64] load path=/bin/shell.bin）",
                     vm.wait_log("[ELF64] load path=/bin/shell.bin", 40, since=base)):
            return 1

        # ---------------- ① DNS ----------------
        print("=== 2) 域名解析：run /bin/busybox nslookup example.com ===")
        base = len(vm.log())
        mon.type_line("run /bin/busybox nslookup example.com")
        got = vm.wait_log("Name:", 90, since=base)
        seg = vm.log()[base:]
        ns_addrs = [a for a in re.findall(r"Address:\\s*(\\d{1,3}(?:\\.\\d{1,3}){3})", seg)
                    if a != "10.0.2.3"]
        check("① 解析过程读了 /etc/resolv.conf 里的 10.0.2.3",
              "10.0.2.3" in seg)
        # 解析成功的**权威判据**放在 ping 那一步：nslookup 只要参数被输入法吞掉就会跑成无参版
        # （实测就踩过：`nslookup: can't resolve '(null)'`），不能拿它当唯一判据。

        # ---------------- ①/② 命令序列（带"输入被吞就重打"的重试；判据不变）----------------
        def run_cmd(cmd, expect, timeout=90, tries=3):
            """输入一条命令并等它真的产生 expect 证据；输入被吞（实测偶发）就重打一遍。
            注意：**判据没有放宽** —— 仍然是"必须出现 expect 那一行才算过"。"""
            base = len(vm.log())
            for i in range(tries):
                mon.type_line(cmd, settle=1.5)
                if vm.wait_log(expect, timeout, since=base):
                    return True, base
                base = len(vm.log())
                print("    （第 %d 次输入没产生证据，重打：%s）" % (i + 1, cmd[:48]))
            return False, base

        print("=== 3) ping 域名（解析通过；slirp 下公网 ICMP 无回复 = 环境限制，如实记录）===")
        ok_ping, base = run_cmd("run /bin/busybox ping -c 1 example.com", "packets transmitted", 120)
        seg = vm.log()[base:]
        m_ping = re.search(r"PING example\.com \((\d{1,3}(?:\.\d{1,3}){3})\)", seg)
        ping_ip = m_ping.group(1) if m_ping else None
        check("① 域名真解析出 IPv4（ping 打印 PING example.com (<ip>) 或 nslookup 拿到 A 记录）",
              bool(ping_ip) or len(ns_addrs) >= 1,
              ("解析到 %s" % (ping_ip or ns_addrs[0])) if (ping_ip or ns_addrs)
              else "未拿到 A 记录（宿主外网 DNS 不可用？）")
        check("ping 没有报 'bad address'（旧版本在这里失败）",
              "bad address" not in seg)

        print("=== 4) 2 MiB 下载到卷上 -> 客人 sha256 与宿主逐字节一致 ===")
        url2 = "http://10.0.2.2:%d/b2.bin" % args.http_port
        base = len(vm.log())
        for i in range(3):
            mon.type_line("run /bin/busybox wget -O /tmp/d.bin " + url2, settle=1.5)
            t0 = time.time()
            while time.time() - t0 < 240:
                if state.get("/b2.bin") is not None:
                    break
                if vm.proc.poll() is not None:
                    break
                time.sleep(1.0)
            if state.get("/b2.bin") is not None:
                break
            print("    （第 %d 次 wget 没到服务端，重打）" % (i + 1))
        hit2 = state.get("/b2.bin")
        check("宿主侧确认 2 MiB 全部发出且客户端读完（done=True）",
              bool(hit2 and hit2["done"]),
              ("bytes=%d seconds=%.2f" % (hit2["bytes"], hit2["seconds"])) if hit2 else "服务端没记录到请求")
        time.sleep(2.0)
        got_sha = None
        t0 = time.time()
        mon.type_line("run /bin/busybox sha256sum /tmp/d.bin", settle=1.5)
        while time.time() - t0 < 90:
            for m in re.finditer(r"\b([0-9a-f]{64})\b", vm.log()[base:]):
                got_sha = m.group(1)
                break
            if got_sha:
                break
            time.sleep(0.5)
        check("客人 sha256 与宿主 sha256 逐字节一致（这才是'传对了'）",
              got_sha == sha_small,
              ("guest=%s host=%s" % (got_sha[:16], sha_small[:16])) if got_sha else "客人没打出 sha256")

        print("=== 5) 16 MiB 下载到 /dev/null（卷空间不够，靠宿主侧字节数取证）===")
        url16 = "http://10.0.2.2:%d/b16.bin" % args.http_port
        for i in range(3):
            mon.type_line("run /bin/busybox wget -O /dev/null " + url16, settle=1.5)
            deadline = time.time() + 600
            while time.time() < deadline:
                if state.get("/b16.bin") is not None:
                    break
                if vm.proc.poll() is not None:
                    break
                time.sleep(2.0)
            if state.get("/b16.bin") is not None:
                break
            print("    （第 %d 次 16 MiB wget 没到服务端，重打）" % (i + 1))
        hit_big = state.get("/b16.bin")
        check("宿主侧确认 16,777,216 B 全部发出且客户端读完（done=True）",
              bool(hit_big and hit_big["done"] and hit_big["bytes"] == 16 * 1024 * 1024),
              ("bytes=%d seconds=%.1f 吞吐=%.2f MB/s"
               % (hit_big["bytes"], hit_big["seconds"], hit_big["bytes"] / 1048576.0 / max(0.001, hit_big["seconds"])))
              if hit_big else "服务端没记录到请求（超时）")

        print("=== 6) 内核侧证据（v0.12.2 硬化判据）===")
        allog = vm.log()
        check("新 socket 层自检 PASS（含窗口/就绪位断言）",
              "[SOCK64] selftest PASS" in allog)
        m_est = re.search(r"\[SOCK64\] estab peer=\S+:\d+ win=(\d+)", allog)
        check("握手时报出**真窗口**（win= 大于旧实现的写死 4096）",
              m_est is not None and int(m_est.group(1)) > 4096,
              m_est.group(0) if m_est else "未出现（没有 TCP 连接？）")
        m_fin = re.search(r"\[SOCK64\] fin rx=(\d+) dup=(\d+) ooo=(\d+) badsum=(\d+) retx=(\d+)", allog)
        check("FIN 汇总行存在（rx/dup/ooo/badsum/retx 五项判据）", m_fin is not None,
              m_fin.group(0) if m_fin else "未出现")
        if m_fin:
            check("没有坏校验和段（badsum=0）", int(m_fin.group(4)) == 0, m_fin.group(4))
        check("全程无 PANIC", "PANIC" not in allog)
    finally:
        vm.close()
        srv.shutdown()
        srv.server_close()

    print("")
    if FAILS:
        print("RESULT: FAIL —— %d 项：%s" % (len(FAILS), "; ".join(FAILS)))
        return 1
    print("RESULT: PASS —— DNS 解析通过 + 2 MiB 逐字节一致 + 16 MiB 宿主侧全量确认")
    return 0


if __name__ == "__main__":
    sys.exit(main())
