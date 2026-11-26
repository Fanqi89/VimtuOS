# -*- coding: utf-8 -*-
"""把 fetch_vimtu 接进 NetSurf 核心 + 把 about/file fetcher 也编上。

两件事都是幂等的（第二次跑不会重复插入）：
  ① third_party/netsurf/content/fetch.c：插入 extern 声明 + fetch_vimtu_register() 调用；
  ② tools/netsurf_core_build_win.sh：fetchers 组加 about/file 子目录与我们的 vimtu.c，
     并把 mbedTLS 的头和用户配置接进 INC_FLAGS / DEFS。
"""
import io, os, sys

ROOT = r"C:\Users\fanqi\Desktop\VimtuOS\Vimtu64"
NS = os.path.join(ROOT, "third_party", "netsurf")

# ---------------- ① content/fetch.c ----------------
p = os.path.join(NS, "content", "fetch.c")
s = io.open(p, encoding="utf-8", errors="surrogateescape").read()

MARK = "/* --- vimtu frontend: 用我们自己的 socket + mbedTLS 顶替 libcurl --- */"
if MARK in s:
    print("[patch] fetch.c 已打过补丁，跳过")
else:
    anchor = "/* exported interface documented in content/fetch.h */\nnserror fetcher_init(void)\n{"
    assert s.count(anchor) == 1, "fetcher_init 锚点数量=%d" % s.count(anchor)
    s = s.replace(anchor, MARK + "\nextern nserror fetch_vimtu_register(void);\n\n" + anchor)

    call_anchor = "\tret = fetch_data_register();\n"
    assert s.count(call_anchor) == 1, "fetch_data_register 锚点数量=%d" % s.count(call_anchor)
    s = s.replace(call_anchor,
                  "\tret = fetch_vimtu_register();\n"
                  "\tif (ret != NSERROR_OK) {\n"
                  "\t\treturn ret;\n"
                  "\t}\n\n" + call_anchor)
    io.open(p, "w", encoding="utf-8", newline="\n", errors="surrogateescape").write(s)
    print("[patch] content/fetch.c 已插入 fetch_vimtu_register()")

# ---------------- ② tools/netsurf_core_build_win.sh ----------------
sp = os.path.join(ROOT, "tools", "netsurf_core_build_win.sh")
t = io.open(sp, encoding="utf-8").read()

old_fetchers = ('    fetchers)   RECURSE=1; SRC_DIRS=""\n'
                '                EXTRA="content/fetchers/data.c content/fetchers/resource.c"\n'
                '                # curl.c: WITH_CURL 关，不编（我们的 vimtu.c 顶替它）\n'
                '                EXCLUDE="" ;;\n')
new_fetchers = ('    fetchers)   RECURSE=1\n'
                '                # about/ 与 file/ 在子目录里（fetch.c 无条件调它们的 register）\n'
                '                SRC_DIRS="content/fetchers/about content/fetchers/file"\n'
                '                # curl.c: WITH_CURL 关，不编（我们的 vimtu.c 顶替它）\n'
                '                EXTRA="content/fetchers/data.c content/fetchers/resource.c $ROOT/user/browser/fetchers/vimtu.c"\n'
                '                EXCLUDE="" ;;\n')
if new_fetchers not in t:
    assert t.count(old_fetchers) == 1, "fetchers 组锚点数量=%d" % t.count(old_fetchers)
    t = t.replace(old_fetchers, new_fetchers)

# mbedTLS 头 + 用户配置（vimtu.c 需要）
old_inc = 'INC_FLAGS="$INC_FLAGS -I$ROOT/third_party/expat/expat/lib -I$ROOT/third_party/zlib"\n'
new_inc = ('# mbedTLS（user/browser/fetchers/vimtu.c 用它做 TLS）\n'
           'MBED="${MBEDTLS_DIR:-$ROOT/third_party/mbedtls}"\n'
           'cp "$ROOT/user/lib/mbedtls_vimtu_user_config.h" "$GEN/mbedtls_vimtu_user_config.h"\n'
           + old_inc +
           'INC_FLAGS="$INC_FLAGS -I$MBED/include -I$GEN"\n')
if "mbedtls_vimtu_user_config.h" not in t:
    assert t.count(old_inc) == 1, "INC_FLAGS 锚点数量=%d" % t.count(old_inc)
    t = t.replace(old_inc, new_inc)

old_defs = 'DEFS="-Dsmall -Dnsvimtu -include $GEN/nscfg.h"\n'
new_defs = ('DEFS="-Dsmall -Dnsvimtu -DMBEDTLS_USER_CONFIG_FILE=\\"mbedtls_vimtu_user_config.h\\" '
            '-include $GEN/nscfg.h"\n')
if "-DMBEDTLS_USER_CONFIG_FILE" not in t:
    assert t.count(old_defs) == 1, "DEFS 锚点数量=%d" % t.count(old_defs)
    t = t.replace(old_defs, new_defs)

io.open(sp, "w", encoding="utf-8", newline="\n").write(t)
print("[patch] netsurf_core_build_win.sh 已接入 vimtu.c + mbedTLS 头")
print("done")
