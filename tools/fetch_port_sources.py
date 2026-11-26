"""把 mbedTLS 与 NetSurf 全家桶的**源码**拉到 third_party/ 下（tarball，不用 git clone）。
判据：解包后关键文件存在 + 打一行 "OK name bytes files"；失败要打印 HTTP 状态，不许静默。"""
import io
import os
import sys
import tarfile
import time
import urllib.error
import urllib.request

ROOT = r"C:\Users\fanqi\Desktop\VimtuOS\Vimtu64"
DEST = os.path.join(ROOT, "third_party")

# (repo, ref, 本地目录名, 关键文件（解包后必须存在，用来证明"真的拿到了"）)
TARGETS = [
    ("Mbed-TLS/mbedtls", "v3.6.2", "mbedtls",
     ["library/ssl_tls.c", "include/mbedtls/build_info.h", "library/bignum.c"]),
    ("netsurf-browser/netsurf", "170d89462442", "netsurf",
     ["content", "desktop", "include"]),
    ("netsurf-browser/libwapcaplet", "74e49210e9c5", "netsurf/libwapcaplet", ["src", "include"]),
    ("netsurf-browser/libparserutils", "c9971265064b", "netsurf/libparserutils", ["src", "include"]),
    ("netsurf-browser/libhubbub", "68989e60c786", "netsurf/libhubbub", ["src", "include"]),
    ("netsurf-browser/libcss", "0c4f51901a15", "netsurf/libcss", ["src", "include"]),
    ("netsurf-browser/libdom", "e4150b65e4e7", "netsurf/libdom", ["src", "include"]),
    ("netsurf-browser/libnsutils", "4f0b88241304", "netsurf/libnsutils", ["src", "include"]),
    ("netsurf-browser/libnspsl", "fb372b56cc83", "netsurf/libnspsl", ["src", "include"]),
    ("netsurf-browser/libnslog", "6c3445f5e78c", "netsurf/libnslog", ["src", "include"]),
    ("netsurf-browser/libnsgif", "b9e3b0aadce3", "netsurf/libnsgif", ["src", "include"]),
    ("netsurf-browser/libnsbmp", "3406e7e0bfb5", "netsurf/libnsbmp", ["src", "include"]),
    ("netsurf-browser/libsvgtiny", "848424e7328e", "netsurf/libsvgtiny", ["src", "include"]),
    ("netsurf-browser/librosprite", "5a529e37891b", "netsurf/librosprite", ["src", "include"]),
    ("libexpat/libexpat", "R_2_6_4", "expat", ["expat/lib/xmlparse.c", "expat/lib/expat.h"]),
    ("madler/zlib", "v1.3.1", "zlib", ["deflate.c", "zlib.h"]),
]


def fetch(url, tries=5):
    last = None
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "vimtuos-port"})
            with urllib.request.urlopen(req, timeout=600) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            last = "HTTP %d" % e.code
            if e.code in (404, 403):
                break
        except Exception as e:
            last = repr(e)
        time.sleep(3 * (i + 1))
    raise RuntimeError("%s failed: %s" % (url, last))


def extract(data, dest, prefix_hint):
    os.makedirs(dest, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tf:
        names = tf.getnames()
        # tarball 顶层一般是 <repo>-<ref>/，剥掉
        top = names[0].split("/")[0]
        for m in tf.getmembers():
            parts = m.name.split("/", 1)
            if len(parts) < 2 or not parts[1]:
                continue
            m.name = parts[1]
            # Windows 上符号链接/特殊设备会直接抛 OSError(22)：跳过它们，
            # 只解普通文件与目录（这些库的源码不需要链接）。
            if not (m.isfile() or m.isdir()):
                continue
            try:
                tf.extract(m, dest)
            except (OSError, IOError):
                pass
    return top, len(names)


def main():
    os.makedirs(DEST, exist_ok=True)
    fails = []
    for repo, ref, sub, keys in TARGETS:
        url = "https://codeload.github.com/%s/tar.gz/refs/%s/%s" % (
            repo, "tags" if ref.startswith(("v", "R_")) else "heads", ref)
        dest = os.path.join(DEST, sub.replace("/", os.sep))
        if all(os.path.exists(os.path.join(dest, k.replace("/", os.sep))) for k in keys):
            print("SKIP %-28s 已就位" % sub)
            continue
        t0 = time.time()
        try:
            data = fetch(url)
        except RuntimeError as e:
            print("FAIL %-28s %s" % (sub, e))
            fails.append(sub)
            continue
        try:
            top, n = extract(data, dest, repo)
        except Exception as e:
            print("FAIL %-28s 解包失败 %r" % (sub, e))
            fails.append(sub)
            continue
        missing = [k for k in keys if not os.path.exists(os.path.join(dest, k.replace("/", os.sep)))]
        if missing:
            print("FAIL %-28s 缺关键文件 %s" % (sub, missing))
            fails.append(sub)
        else:
            print("OK   %-28s %s KB  members=%d  %.1fs  top=%s"
                  % (sub, format(len(data) // 1024, ","), n, time.time() - t0, top))
    print("\n===== %s =====" % ("ALL OK" if not fails else "FAILED: " + ", ".join(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
