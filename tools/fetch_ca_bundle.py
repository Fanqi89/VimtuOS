#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/fetch_ca_bundle.py - ★ v0.12.4：抓一份 CA 根证书包（PEM）给 /etc/ssl/certs/ca.pem

为什么需要：mbedTLS 校验服务端证书链时必须有**信任根**。没有 CA 包的话，任何真站点都会
在 `mbedtls_x509_crt_verify` 阶段失败（BAD_CERTIFICATE），而"能连上但验不过"最容易被
误读成"TLS 实现不对"。所以构建期抓一份 Mozilla 根证书集（curl.se 维护的同名文件）。

为什么**不进仓**（与 mbedTLS/NetSurf 源码同一条纪律）：230 KB 二进制在仓里没意义，且会随上游
变化；脚本按构建期现拉，缓存进 `third_party/ca/ca.pem`（.gitignore 已排除）。

判据（脚本自己打）：
  [CABUNDLE] ok=1 bytes=<n> certs=<n> sha256=<前16位> -> <路径>
  失败时 exit 2（调用方可以选择"不写 CA 包继续构建"，但那样 HTTPS 判据必须报 UNTRUSTED）。

用法：
  py -3 tools/fetch_ca_bundle.py               # -> third_party/ca/ca.pem（缓存）+ build64/ca.pem
  py -3 tools/fetch_ca_bundle.py --out X.pem   # 只写这个路径
"""
import argparse
import hashlib
import os
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# 两个来源（同一个文件的两个镜像）；第一个失败就退第二个。
SOURCES = [
    "https://curl.se/ca/cacert.pem",
    "https://raw.githubusercontent.com/bagder/ca-bundle/master/ca-bundle.crt",
]
MIN_CERTS = 80          # 低于这个数就当成"抓到了半个页面/错误页"，不接受
MIN_BYTES = 100 * 1024


def try_fetch(url, tries=4):
    last = None
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "VimtuOS-fetch-ca/0.12.4"})
            with urllib.request.urlopen(req, timeout=30) as r:
                data = r.read()
            if len(data) >= MIN_BYTES:
                return data
            last = "太短：%d B" % len(data)
        except Exception as e:                      # noqa: BLE001 - 网络错误一律重试
            last = "%s: %s" % (type(e).__name__, e)
        time.sleep(2 * (i + 1))
    sys.stderr.write("  抓取失败 %s（%s）\n" % (url, last))
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None, help="输出路径（默认 build64/ca.pem，同时缓存进 third_party/ca/）")
    ap.add_argument("--cache", default=os.path.join(ROOT, "third_party", "ca", "ca.pem"),
                    help="缓存路径（已有且合格就直接用，不再联网）")
    ap.add_argument("--force", action="store_true", help="忽略缓存重新抓")
    args = ap.parse_args()

    out = args.out or os.path.join(ROOT, "build64", "ca.pem")
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)

    data = None
    if args.cache and os.path.exists(args.cache) and not args.force:
        with open(args.cache, "rb") as f:
            cached = f.read()
        if len(cached) >= MIN_BYTES and cached.count(b"-----BEGIN CERTIFICATE-----") >= MIN_CERTS:
            data = cached
            print("    缓存命中：%s（%d B）" % (args.cache, len(data)))

    if data is None:
        for url in SOURCES:
            data = try_fetch(url)
            if data is not None:
                print("    抓到：%s" % url)
                break
        if data is None:
            sys.stderr.write("[CABUNDLE] FAIL 所有来源都抓不到（离线？）\n")
            return 2
        if args.cache:
            os.makedirs(os.path.dirname(args.cache) or ".", exist_ok=True)
            with open(args.cache, "wb") as f:
                f.write(data)

    n = data.count(b"-----BEGIN CERTIFICATE-----")
    if n < MIN_CERTS:
        sys.stderr.write("[CABUNDLE] FAIL 证书数只有 %d（< %d）—— 抓到的不是完整包\n" % (n, MIN_CERTS))
        return 2
    with open(out, "wb") as f:
        f.write(data)
    sha = hashlib.sha256(data).hexdigest()
    print("[CABUNDLE] ok=1 bytes=%d certs=%d sha256=%s -> %s" % (len(data), n, sha[:16], out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
