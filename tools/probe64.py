#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""交付物"只能从系统卷装载"这条体积门禁的**共享探针选择器**（修假命中）。

假命中根因（实测，2026-XX 本批）
--------------------------------
旧门禁给每份交付物取一个 64 B 探针：小文件取整份，大文件取"全图不同字节值最多"的窗口
（`probe in kernel` 成立就 `exit 1`）。这类窗口**可能是两份二进制共享的公共数据表**：
实测 `build64/gzip` 偏移 21856 的 64 B 就是标准 **CRC-32 表**的一段，而内核链接进来的
`build64/os/vfs64.o`（偏移 121296）里有一模一样的 1037 B 连续段 —— 于是门禁对着**合法链接
假命中**、构建在 ISO 组装前 `exit 1`，`vimtu64-64.img` / `build64/a42a_sys.elf` 都产不出来。

判据（四层，缺一不可）
----------------------
⓪ **整份文件**在内核里（仅当整份文件字节足够"有判别力"）-> 真泄漏（最硬的一条）。
① 探针窗口必须**不在内核二进制里**（在内核里搜到 = 泄漏嫌疑）。
② 探针窗口必须**不在"合法链接的对象集合"里**（默认 `build64/os/*.o` + `build64/*.o` +
   `gui_rs/gui_rs.o`）—— 对象里的字节本来就是内核的一部分，出现在那里只能说明这是
   共享模式（CRC 表 / 对齐填充 / 公共字面量），**不是**交付物泄漏。
   候选窗口按"不同字节值多"降序、同分按偏移升序（完全确定性），第一个同时满足 ①② 的
   被接受，并在构建日志里打印 `probe offset N accepted（未在合法对象中出现）`。
③ **一个候选都找不到**（该文件的每个候选窗口都出现在合法对象里）-> 真泄漏：
   只有"有人把这份交付物编进 / 塞进内核"才会这样。
④ 该文件里任一**对齐的 1056 B 连续块**（块内不同字节值 >= 8）出现在内核里 -> 真泄漏。
   这条专抓"只搬了一部分"的泄漏：假命中源（CRC-32 表）只有 1037 B，凑不出一个完整的
   1056 B 块；而真塞进去的代码段必然含 ≥ 1056 B 的连续区。

为什么"②"不会放过真泄漏：真泄漏有两条路 —— 走对象链接进来的（③ / ④ 必红），
或者链接后被 objcopy 直接塞进镜像的（① 会让所有候选窗口都被判掉，最终 ③ 的"找不到候选"
正好也是这个状态）。两条路都被抓住。

用法（构建脚本调用）
-------------------
    python tools/probe64.py --kernel build64/kernel64_os.bin [--obj GLOB]... FILE...
    python tools/probe64.py --kernel ... --stats FILE...        # 额外打印判据统计（慢）
选项：
    --mode mid   沿用老的"取中段 64 B 当首选探针"（中段不满足 ①② 时自动退到通用候选）
退出码：0 = 全部 OK；1 = 至少一条真泄漏（stderr 有 ERROR 行）。
"""

import glob
import os
import sys

WIN = 64                      # 探针窗口长度
STRIDE = 32                   # 候选窗口步长（与旧门禁一致）
MIN_DISTINCT = 8              # 窗口不同字节值下限（低于此的窗口没有判别力：内核里到处都是零/同色）
BLOCK = 1056                  # ④ 的连续块长度（> 实测假命中源 1037 B 的 CRC-32 表）
BLOCK_MIN_DISTINCT = 8
WHOLE_MIN_DISTINCT = 16       # ⓪ 整份比对的判别力下限

_DEFAULT_OBJ_PATTERNS = ("os/*.o", "*.o")


def default_objects(build_dir):
    """内核链接时真正喂给 ld 的目标文件集合（见 build64.sh 的两条 $LD 命令）。"""
    out = []
    for pat in _DEFAULT_OBJ_PATTERNS:
        out += glob.glob(os.path.join(build_dir, pat))
    out += glob.glob(os.path.join(os.path.dirname(os.path.abspath(build_dir)), "gui_rs", "gui_rs.o"))
    return sorted(set(out))


class Gate(object):
    """一份内核二进制 + 它的"合法链接对象集合"上的门禁。"""

    _announced = False

    def __init__(self, kernel_path, obj_paths=None, verbose=True):
        self.kernel_path = kernel_path
        self.k = open(kernel_path, "rb").read()
        if obj_paths is None:
            obj_paths = default_objects(os.path.dirname(os.path.abspath(kernel_path)))
        self.obj_paths = list(obj_paths)
        self.obj = b"".join(open(p, "rb").read() for p in self.obj_paths)
        self.bad = 0
        if verbose and not Gate._announced:
            Gate._announced = True
            print("    门禁：合法链接对象集合 = %d 份 / %d B（%s + gui_rs/gui_rs.o）；内核 %s = %d B"
                  % (len(self.obj_paths), len(self.obj),
                     os.path.basename(os.path.dirname(os.path.abspath(kernel_path))) + "/os/*.o + " +
                     os.path.basename(os.path.dirname(os.path.abspath(kernel_path))) + "/*.o",
                     os.path.basename(kernel_path), len(self.k)))

    # ---- 内部：候选窗口排序（确定性） --------------------------------------
    @staticmethod
    def _offsets(b):
        return list(range(0, max(1, len(b) - WIN), STRIDE))

    @staticmethod
    def _ranked(b):
        offs = Gate._offsets(b)
        return sorted(((len(set(b[o:o + WIN])), o) for o in offs), key=lambda t: (-t[0], t[1]))

    @staticmethod
    def _mid_off(b):
        return len(b) // 2

    # ---- 主判据 -----------------------------------------------------------
    def check(self, path, mode="he", stats=False):
        """检查一份交付物；返回 True = 通过。"""
        b = open(path, "rb").read()
        name = os.path.basename(path)
        kn = os.path.basename(self.kernel_path)

        # ⓪ 整份文件
        if len(set(b)) >= WHOLE_MIN_DISTINCT and b in self.k:
            sys.stderr.write("ERROR: system kernel contains %s bytes (delivery must be a volume file)"
                             " —— 整份 %d B 都在 %s 里（最硬的一条：资源没搬干净）\n"
                             % (path, len(b), kn))
            self.bad += 1
            return False

        # ④ 连续块
        if len(b) >= BLOCK:
            for o in range(0, len(b) - BLOCK + 1, BLOCK):
                seg = b[o:o + BLOCK]
                if len(set(seg)) < BLOCK_MIN_DISTINCT:
                    continue
                if seg in self.k:
                    sys.stderr.write("ERROR: system kernel contains %s bytes (delivery must be a volume file)"
                                     " —— %d B 连续块（文件偏移 %d）整体出现在 %s 里\n"
                                     % (path, BLOCK, o, kn))
                    self.bad += 1
                    return False

        # ①②③ 探针
        ranked = self._ranked(b)
        if mode == "mid":
            mo = self._mid_off(b)
            midn = len(set(b[mo:mo + WIN]))
            # 中段窗口只在"有判别力"时才当首选（低熵中段——例如整片零——没有判别力，
            # 与旧门禁的"零字节假命中"是同一类问题）
            ranked = ([(midn, mo)] if midn >= MIN_DISTINCT else []) + [t for t in ranked if t[1] != mo]
        accepted = None
        skipped_obj = 0
        skipped_kernel = []
        for n, o in ranked:
            if n < MIN_DISTINCT:
                break
            w = b[o:o + WIN]
            if len(w) < WIN:
                break
            if w in self.obj:                      # ② 共享模式（合法对象里也有）-> 换窗口
                skipped_obj += 1
                continue
            if w in self.k:                        # ① 内核里有 -> 记一笔，继续找可接受的窗口
                skipped_kernel.append(o)
                continue
            accepted = (o, n)
            break

        if accepted is None:
            sys.stderr.write("ERROR: system kernel contains %s bytes (delivery must be a volume file)"
                             " —— 找不到可接受的探针：%d 个候选窗口都出现在合法链接的对象里%s"
                             "（只有\"把这份交付物编进/塞进内核\"才会这样）\n"
                             % (path, skipped_obj,
                                ("，另 %d 个候选只在内核里而不在对象里（偏移 %s）"
                                 % (len(skipped_kernel), ",".join(str(x) for x in skipped_kernel[:4])))
                                if skipped_kernel else ""))
            self.bad += 1
            return False

        o, n = accepted
        print("    断言 OK：%s %d B 里搜不到 %s 的 64B 探针（probe offset %d accepted（未在合法对象中出现），"
              "不同字节值 %d%s）；它只从系统卷装载"
              % (kn, len(self.k), name, o, n,
                 ("，跳过 %d 个共享窗口" % (skipped_obj + len(skipped_kernel)))
                 if (skipped_obj or skipped_kernel) else ""))
        if stats:
            acc = shared = ink = 0
            for oo in self._offsets(b):
                w = b[oo:oo + WIN]
                if len(w) < WIN:
                    continue
                if w in self.obj:
                    shared += 1
                elif w in self.k:
                    ink += 1
                else:
                    acc += 1
            print("        统计 %s：候选 %d，accepted %d，共享(在对象里) %d，内核独有 %d"
                  % (name, acc + shared + ink, acc, shared, ink))
        return True


def main(argv):
    kernel = None
    objs = []
    mode = "he"
    stats = False
    files = []
    files = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--kernel":
            i += 1
            kernel = argv[i]
        elif a == "--obj":
            i += 1
            objs.append(argv[i])
        elif a == "--mode":
            i += 1
            mode = argv[i]
        elif a == "--stats":
            stats = True
        elif a in ("-h", "--help"):
            sys.stdout.write(__doc__)
            return 0
        else:
            files.append(a)
        i += 1
    if not kernel or not files:
        sys.stderr.write("usage: probe64.py --kernel KERNEL.bin [--obj GLOB|PATH]... [--mode he|mid]"
                         " [--stats] FILE...\n")
        return 2
    if objs:
        paths = []
        for o in objs:
            paths += glob.glob(o)
        gate = Gate(kernel, sorted(set(paths)))
    else:
        gate = Gate(kernel)
    for f in files:
        gate.check(f, mode=mode, stats=stats)
    return 1 if gate.bad else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
