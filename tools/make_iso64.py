#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/make_iso64.py - 生成 64 位安装 ISO（BIOS 光盘 + U 盘 hybrid + UEFI 三种引导）

产物：vimtu64-64.iso
  ISO9660 里的文件：
    CDISO.BIN     BIOS 光盘引导桩（boot/cdiso.asm，BIOS 加载到 0x7C00 执行）
    LOADER64.BIN  loader64（引导桩读进 0x9000）
    KERNEL64.BIN  安装程序内核（补零到 4MB）
    SYSTEM.IMG    系统镜像（= build64/system.img，8073 × 512B；装到目标盘上的东西）

三种引导路径（同一个 ISO）：
  1) BIOS 光盘：El Torito no-emul，引导镜像 = CDISO.BIN（我们自己写的桩）
  2) BIOS U 盘 ：ISO 第 0 扇区 = 自研 hybrid MBR（boot/hybrid_mbr.asm），把 CDISO.BIN 读进 0x7C00
  3) UEFI      ：El Torito no-emul（platform 0xEF）+ 一张 GPT + FAT32 的 ESP 附加分区（48MB）
                ESP 内含 EFI/BOOT/BOOTX64.EFI（自研 PE32+）+ UEFI64.BIN + KERNEL64.BIN + SYSTEM.IMG

为什么要回填 LBA：ISO9660 里文件落在哪个 LBA 只有 xorriso 生成之后才知道，而引导桩/MBR
必须知道。生成后解析 ISO9660 目录拿到 LBA，再改 ISO 字节里对应的补丁点
（魔数 + dword；ISO9660 没有校验和，直接改是安全的）。
"""
import os
import shutil
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, "build64")
STAGE = os.path.join(BUILD, "iso-stage")
ISO = os.path.join(ROOT, "vimtu64-64.iso")

XORRISO_CANDIDATES = [
    r"C:\msys64\usr\bin\xorriso.exe",
    r"C:\msys64\mingw64\bin\xorriso.exe",
    "xorriso",
]

KERNEL_CD_BYTES = 8000 * 512          # loader 固定读 4MB（KERNEL_SECTORS 个 512B 扇区）
PAYLOAD_SECTORS = 8073                # 系统镜像扇区数（512B 单位）
BOOT_LOAD_SECTORS = 4                 # El Torito 引导镜像占几个 512B 扇区（桩 1536B → 3，取 4）

ESP_IMG = os.path.join(BUILD, "esp.img")     # tools/make_esp.py 生成的 FAT32 ESP（48MB）
ESP_PARTNO = 2                               # GPT 里第 2 分区 = ESP


def find_xorriso():
    for c in XORRISO_CANDIDATES:
        if os.path.sep in c:
            if os.path.exists(c):
                return c
        else:
            f = shutil.which(c)
            if f:
                return f
    return None


def msys(p):
    """Windows 路径 -> MSYS 路径（xorriso 是 MSYS2 程序，只认 /c/... 形式）"""
    p = p.replace("\\", "/")
    if len(p) > 1 and p[1] == ":":
        p = "/" + p[0].lower() + p[2:]
    return p


def iso_dir_records(img, lba, size):
    """解析一个目录 extent，返回 [(name, lba, size)]。"""
    data = img[lba * 2048: lba * 2048 + size]
    out = []
    off = 0
    while off < len(data):
        ln = data[off]
        if ln == 0:
            off = (off // 2048 + 1) * 2048
            continue
        rec = data[off:off + ln]
        if len(rec) < 33:
            break
        ext_lba = struct.unpack_from("<I", rec, 2)[0]
        ext_len = struct.unpack_from("<I", rec, 10)[0]
        name_len = rec[32]
        name = rec[33:33 + name_len].decode("ascii", "replace")
        out.append((name, ext_lba, ext_len))
        off += ln
    return out


def iso_find_file(img, want):
    pvd = img[16 * 2048: 17 * 2048]
    if pvd[0] != 1 or pvd[1:6] != b"CD001":
        raise RuntimeError("ISO9660 主卷描述符不对")
    root = pvd[156:156 + 34]
    root_lba = struct.unpack_from("<I", root, 2)[0]
    root_len = struct.unpack_from("<I", root, 10)[0]
    for name, lba, size in iso_dir_records(img, root_lba, root_len):
        base = name.split(";")[0].upper()
        if base == want.upper():
            return lba, size
    raise RuntimeError("ISO 里找不到文件 %s" % want)


def patch_dword(img, magic, value, what):
    hits = 0
    i = 0
    while True:
        i = img.find(magic, i)
        if i < 0:
            break
        struct.pack_into("<I", img, i + len(magic), value)
        hits += 1
        i += len(magic) + 4
    if hits == 0:
        raise RuntimeError("补丁点 %s 没找到（%s）" % (magic.decode(), what))
    print("    补丁 %-4s = %-10d (%s)，命中 %d 处" % (magic.decode(), value, what, hits))
    return hits


def find_esp_from_eltorito(img):
    """从 El Torito 引导目录里读 UEFI 项（platform 0xEF）的 LBA 与扇区数。

    返回 (512B 单位的 LBA, 扇区数) 或 None。这是"ESP 在 ISO 里落在哪"的权威来源：
    xorriso 的 -append_partition 会把它附加在镜像末尾，并把 El Torito 的 UEFI 项指过去。

    ★ 扇区数可能是 0：El Torito 节条目里的 Sector Count 只有 **16 位**（512B 单位
      -> 最多 32MB）。我们的 ESP 是 48MB 的真 FAT32（FAT32 要求簇数 >= 65525，
      SPC=1 时数据区就 >= 32MB，卷下限约 33.5MB），塞不进这个 16 位字段，
      xorriso 于是写 0 —— 按 El Torito 规范 0 表示"整张引导映像"。
      调用方遇到 0 必须改用 **真实镜像字节数**（见 main：这么算出来的分区大小才是对的，
      否则 MBR/GPT 的 ESP 项会写成 0 扇区）。
    """
    d = img[17 * 2048: 18 * 2048]
    if d[0] != 0 or d[1:6] != b"CD001" or d[6] != 1:
        return None
    cat = struct.unpack_from("<I", d, 71)[0] * 2048
    off = 64                                    # 跳过校验项(32B) + 默认项(32B)
    for _ in range(4):
        hdr = img[cat + off: cat + off + 32]
        if hdr[0] in (0x90, 0x91):              # 节头（0x91 = 最后一项）
            ent = img[cat + off + 32: cat + off + 64]
            lba2048 = struct.unpack_from("<I", ent, 8)[0]
            nsec = struct.unpack_from("<H", ent, 6)[0]
            return lba2048 * 4, nsec
        off += 32
    return None


def patch_eltorito_sector_count(img, value):
    """把 UEFI El Torito 项（platform 0xEF）的 Sector Count 改成非 0 值（原地改 ISO 字节）。

    为什么必须改：该字段只有 **16 位**（512B 单位 -> 上限 65535 = 32MB），而我们的 ESP 是
    48MB 的真 FAT32（FAT32 要求簇数 >= 65525，SPC=1 时数据区就 >= 32MB）。xorriso 遇到
    装不下的镜像会写 0；实测 EDK2/OVMF 的 CD 引导路径把 0 当成"没有引导映像"：
        BdsDxe: failed to load Boot0001 "UEFI QEMU DVD-ROM ...": Not Found
    然后掉进 UEFI Shell —— 光盘的 UEFI 引导整个失效。写成 0xFFFF（字段能表达的最大值，
    32MB 窗口）后 OVMF 正常从 ESP 起我们的 BOOTX64.EFI（实测到安装向导第一屏）。
    ESP 里的全部文件都在前 ~8.3MB 内，32MB 的窗口足够；**ESP 的真实大小写在 GPT/MBR 里**
    （98304 扇区），磁盘形态（U 盘/硬盘）走的是那条路，不受这个 16 位字段限制。
    """
    d = img[17 * 2048: 18 * 2048]
    if d[0] != 0 or d[1:6] != b"CD001" or d[6] != 1:
        return 0
    cat = struct.unpack_from("<I", d, 71)[0] * 2048
    off = 64
    for _ in range(4):
        hdr = img[cat + off: cat + off + 32]
        if hdr[0] in (0x90, 0x91):
            struct.pack_into("<H", img, cat + off + 32 + 6, value)
            print("    补丁 El Torito(0xEF) Sector Count = %d（字段 16 位，写不下 48MB 的 ESP）"
                  % value)
            return 1
        off += 32
    return 0


def write_gpt(img, esp_lba512, esp_sectors):
    """自己写一张标准 GPT（xorriso 1.5.8 的 -isohybrid-gpt-basdat 实测没有生成 GPT）。

    布局：
        LBA 0          MBR（由 build_hybrid_mbr 写：0x17 ISO 区 + 0xEF ESP 项）
        LBA 1          GPT 头
        LBA 2..33      分区项数组（128 × 128B = 16KB）
        LBA 34..       可用区
        末尾 33 扇区    备份 GPT（项数组 + 头）
    分区：
        1) ISO9660（Basic Data）  LBA 0 .. esp-1
        2) EFI 系统分区（ESP）     LBA esp .. esp+n-1
    """
    total = len(img) // 512
    entries_lba, n_entries, ent_size = 2, 128, 128
    first_usable = entries_lba + (n_entries * ent_size) // 512        # 34
    last_usable = total - 34

    def guid(s):
        """GUID 是 5 段：前 3 段小端，第 4 段 2 字节，第 5 段 6 字节。"""
        a, b, c, d, e = s.split("-")
        return (struct.pack("<IHH", int(a, 16), int(b, 16), int(c, 16)) +
                bytes.fromhex(d) + bytes.fromhex(e))

    TYPE_BASIC = guid("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7")
    TYPE_ESP   = guid("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")
    DISK_GUID  = guid("56494D54-4F53-0001-0000-000000000001")          # 'VIMT…'
    P1_GUID    = guid("56494D54-4F53-0002-0000-000000000002")
    P2_GUID    = guid("56494D54-4F53-0003-0000-000000000003")

    ents = bytearray(n_entries * ent_size)

    def put_ent(i, type_guid, uniq, first, last, name):
        o = i * ent_size
        ents[o:o + 16] = type_guid
        ents[o + 16:o + 32] = uniq
        struct.pack_into("<Q", ents, o + 32, first)
        struct.pack_into("<Q", ents, o + 40, last)
        struct.pack_into("<Q", ents, o + 48, 0)                        # 属性
        nm = name.encode("utf-16-le")[:72]
        ents[o + 56:o + 56 + len(nm)] = nm

    put_ent(0, TYPE_BASIC, P1_GUID, 0, esp_lba512 - 1, "VIMTU64 ISO")
    put_ent(1, TYPE_ESP, P2_GUID, esp_lba512, esp_lba512 + esp_sectors - 1, "VIMTU64 ESP")
    ent_crc = zlib.crc32(bytes(ents)) & 0xFFFFFFFF

    def header(cur_lba, alt_lba, ent_lba):
        h = bytearray(512)
        h[0:8] = b"EFI PART"
        struct.pack_into("<I", h, 8, 0x00010000)                       # 版本 1.0
        struct.pack_into("<I", h, 12, 92)                              # 头大小
        struct.pack_into("<Q", h, 24, cur_lba)                         # 本头所在 LBA
        struct.pack_into("<Q", h, 32, alt_lba)                         # 备份头 LBA
        struct.pack_into("<Q", h, 40, first_usable)
        struct.pack_into("<Q", h, 48, last_usable)
        h[56:72] = DISK_GUID
        struct.pack_into("<Q", h, 72, ent_lba)                         # 项数组所在 LBA
        struct.pack_into("<I", h, 80, n_entries)
        struct.pack_into("<I", h, 84, ent_size)
        struct.pack_into("<I", h, 88, ent_crc)
        struct.pack_into("<I", h, 16, zlib.crc32(bytes(h[:92])) & 0xFFFFFFFF)
        return bytes(h)

    img[1 * 512: 2 * 512] = header(1, total - 1, entries_lba)
    img[entries_lba * 512: entries_lba * 512 + len(ents)] = ents
    bak_ent_lba = total - 33
    img[bak_ent_lba * 512: bak_ent_lba * 512 + len(ents)] = ents
    img[(total - 1) * 512: total * 512] = header(total - 1, 1, bak_ent_lba)
    return (0, esp_lba512 - 1), (esp_lba512, esp_lba512 + esp_sectors - 1)


def build_hybrid_mbr(mbr_path, stub_lba512, stub_bytes, image_bytes, esp_lba512, esp_sectors):
    """把 hybrid_mbr.bin（我们自己的 512 字节 MBR 代码）加工成可写入 ISO 第 0 扇区的字节。

    分区表规则：
      * 有 ESP（UEFI 双模式）：两项
            项1: 0x17（隐藏 ISO9660）覆盖 ISO 区，活动
            项2: 0xEF（EFI 系统分区）指向附加的 ESP
        为什么必须有 0xEF 项：一部分 UEFI 固件在"只有 MBR、没有 GPT"的 U 盘上就是靠
        找 MBR 里的 0xEF 分区来定位 ESP 的（UEFI 规范允许 ESP 位于 MBR 分区）。
        支持 GPT 的固件会优先读 GPT（write_gpt 里同样写好 ISO + ESP），两条路都通。
      * 没有 ESP（纯 BIOS 单模式）：一项 —— 活动 + 0x17 + 起始 LBA 0 + 覆盖整张镜像
        （isohybrid 的经典做法；真正执行的是 MBR 代码，分区只是让 fdisk/BIOS 满意）
    """
    with open(mbr_path, "rb") as f:
        mbr = bytearray(f.read())
    if len(mbr) != 512:
        raise RuntimeError("hybrid_mbr.bin 不是 512 字节：%d" % len(mbr))
    for magic, value in ((b"VMHY", stub_lba512), (b"VMHZ", stub_bytes)):
        i = mbr.find(magic)
        if i < 0:
            raise RuntimeError("MBR 里找不到补丁点 %s" % magic.decode())
        struct.pack_into("<I", mbr, i + 4, value)

    if esp_lba512:
        e1 = bytearray(16)
        e1[0] = 0x80
        e1[1:4] = b"\x00\x01\x00"
        e1[4] = 0x17
        e1[5:8] = b"\xFF\xFF\xFF"
        struct.pack_into("<I", e1, 8, 0)
        struct.pack_into("<I", e1, 12, esp_lba512)
        mbr[446:462] = e1
        e2 = bytearray(16)
        e2[1:4] = b"\x00\x01\x00"
        e2[4] = 0xEF
        e2[5:8] = b"\xFF\xFF\xFF"
        struct.pack_into("<I", e2, 8, esp_lba512)
        struct.pack_into("<I", e2, 12, esp_sectors)
        mbr[462:478] = e2
    else:
        e = bytearray(16)
        e[0] = 0x80
        e[1:4] = b"\x00\x01\x00"
        e[4] = 0x17
        e[5:8] = b"\x00\x01\x00"
        struct.pack_into("<I", e, 8, 0)
        struct.pack_into("<I", e, 12, image_bytes // 512)
        mbr[446:462] = e
    mbr[510] = 0x55
    mbr[511] = 0xAA
    return bytes(mbr)


def main():
    xo = find_xorriso()
    if not xo:
        print("找不到 xorriso（MSYS2: pacman -S xorriso）")
        return 2
    for f in ("cdiso.bin", "loader64.bin", "kernel64.bin", "system.img", "hybrid_mbr.bin"):
        if not os.path.exists(os.path.join(BUILD, f)):
            print("缺少 build64/%s（先跑 build64.sh 的汇编/内核部分）" % f)
            return 2

    # ---- 暂存目录 ----
    if os.path.exists(STAGE):
        shutil.rmtree(STAGE)
    os.makedirs(STAGE)
    shutil.copyfile(os.path.join(BUILD, "cdiso.bin"), os.path.join(STAGE, "CDISO.BIN"))
    shutil.copyfile(os.path.join(BUILD, "loader64.bin"), os.path.join(STAGE, "LOADER64.BIN"))

    kpath = os.path.join(BUILD, "kernel64.bin")
    ksize = os.path.getsize(kpath)
    with open(os.path.join(STAGE, "KERNEL64.BIN"), "wb") as out, open(kpath, "rb") as f:
        shutil.copyfileobj(f, out)
        pad = KERNEL_CD_BYTES - ksize
        if pad > 0:
            out.write(b"\0" * pad)
    shutil.copyfile(os.path.join(BUILD, "system.img"), os.path.join(STAGE, "SYSTEM.IMG"))

    loader_size = os.path.getsize(os.path.join(BUILD, "loader64.bin"))
    stub_size = os.path.getsize(os.path.join(BUILD, "cdiso.bin"))
    payload_bytes = os.path.getsize(os.path.join(STAGE, "SYSTEM.IMG"))
    print("  ISO 暂存：CDISO=%d B, LOADER64=%d B, KERNEL64=%d B(补零后), SYSTEM.IMG=%d B"
          % (stub_size, loader_size, KERNEL_CD_BYTES, payload_bytes))

    has_esp = os.path.exists(ESP_IMG)
    esp_bytes = os.path.getsize(ESP_IMG) if has_esp else 0

    # ---- xorriso 打包（BIOS El Torito + UEFI El Torito + 附加 ESP 分区）----
    args = [xo, "-as", "mkisofs", "-o", msys(ISO),
            "-b", "CDISO.BIN", "-no-emul-boot", "-boot-load-size", str(BOOT_LOAD_SECTORS),
            "-V", "VIMTU64", "-quiet"]
    if has_esp:
        args += ["-eltorito-alt-boot",
                 "-e", "--interval:appended_partition_%d:all::" % ESP_PARTNO,
                 "-no-emul-boot",
                 "-append_partition", str(ESP_PARTNO), "0xef", msys(ESP_IMG)]
        print("  UEFI：El Torito(0xEF) + 附加 ESP 分区（%d B = %.1f MB）"
              % (esp_bytes, esp_bytes / 1048576.0))
    else:
        print("  警告：没有 build64/esp.img —— 只做 BIOS 单模式 ISO")
    args += [msys(STAGE)]
    r = subprocess.run(args, capture_output=True)
    if r.returncode != 0:
        print("xorriso 失败：%s" % r.stderr.decode("utf-8", "replace")[-700:])
        return 1

    # ---- 回填 LBA、写 MBR / GPT ----
    with open(ISO, "r+b") as f:
        img = bytearray(f.read())
        ll, ls = iso_find_file(img, "LOADER64.BIN")
        kl, ks = iso_find_file(img, "KERNEL64.BIN")
        sl, ss = iso_find_file(img, "SYSTEM.IMG")
        cl, cs = iso_find_file(img, "CDISO.BIN")
        print("  ISO9660：CDISO@%d(%d) LOADER64@%d(%d) KERNEL64@%d(%d) SYSTEM.IMG@%d(%d)"
              % (cl, cs, ll, ls, kl, ks, sl, ss))

        # 光盘路径（2048B 单位）
        patch_dword(img, b"VMLD", ll, "光盘：loader 起始扇区")
        patch_dword(img, b"VMLB", loader_size, "loader 字节数")
        patch_dword(img, b"VMKN", kl, "光盘：内核起始扇区")
        patch_dword(img, b"VMSY", sl, "光盘：载荷起始扇区")
        patch_dword(img, b"VMSS", PAYLOAD_SECTORS, "载荷扇区数")
        # 硬盘/U 盘路径（512B 单位 = 2048B 单位 × 4）
        patch_dword(img, b"VML9", ll * 4, "硬盘：loader 起始扇区")
        patch_dword(img, b"VMK9", kl * 4, "硬盘：内核起始扇区")
        patch_dword(img, b"VMKB", KERNEL_CD_BYTES, "内核字节数")
        patch_dword(img, b"VMS9", sl * 4, "硬盘：载荷起始扇区")
        patch_dword(img, b"VMYB", payload_bytes, "载荷字节数")

        # ESP（UEFI）：从 El Torito 目录里读它在镜像中的真实位置
        esp_lba = esp_sectors = None
        if has_esp:
            found = find_esp_from_eltorito(img)
            if not found:
                print("    警告：El Torito 里没有 UEFI 项 —— 不写 GPT/ESP 分区表项")
            else:
                esp_lba, nsec = found
                # ★ 48MB 的 FAT32 ESP 塞不进 El Torito 的 16 位 Sector Count（上限 32MB），
                #   xorriso 会写 0（规范语义："整张引导映像"）；分区表（MBR/GPT）里必须写
                #   **真实大小**，否则 0xEF 项会是 0 扇区 -> 固件看不到 ESP。
                esp_sectors = nsec if nsec else (esp_bytes + 511) // 512
                print("  ESP：LBA %d..%d（%d 扇区 = %.1f MB）%s"
                      % (esp_lba, esp_lba + esp_sectors - 1, esp_sectors,
                         esp_sectors * 512 / 1048576.0,
                         "" if nsec else "（El Torito 字段为 0：>32MB 装不下 16 位 Sector Count，"
                                         "按真实镜像字节数写 MBR/GPT）"))
                if nsec == 0:
                    # ★ 关键：El Torito 的 Sector Count 为 0 时 EDK2/OVMF 认为"没有引导映像"，
                    #   光盘的 UEFI 引导会失效（实测掉进 UEFI Shell）。补成字段能表达的最大值。
                    patch_eltorito_sector_count(img, 0xFFFF)

        # 第 0 扇区 = hybrid MBR（BIOS U 盘/硬盘引导；光盘引导走 El Torito，与此扇区无关）
        mbr = build_hybrid_mbr(os.path.join(BUILD, "hybrid_mbr.bin"),
                               cl * 4, stub_size, len(img), esp_lba, esp_sectors)
        img[0:512] = mbr
        print("  hybrid MBR：桩@%d(512B 单位) 字节=%d，分区表=%s"
              % (cl * 4, stub_size,
                 "0x17 ISO 区(活动) + 0xEF ESP" if esp_lba
                 else "0x17 活动 覆盖 %d 扇区" % (len(img) // 512)))

        # GPT（UEFI 在"GPT 盘"上找 ESP；同时 MBR 里的 0xEF 项兜底）
        if esp_lba:
            p1, p2 = write_gpt(img, esp_lba, esp_sectors)
            print("  GPT：已写入（分区1 ISO9660 LBA %d..%d；分区2 ESP LBA %d..%d）"
                  % (p1[0], p1[1], p2[0], p2[1]))
            print("    校验：LBA1 = %r，备份头在 LBA %d"
                  % (bytes(img[512:520]), len(img) // 512 - 1))

        f.seek(0)
        f.write(img)

    print("  ISO 完成：%s = %d 字节（%d 个 2048B 扇区）"
          % (ISO, os.path.getsize(ISO), os.path.getsize(ISO) // 2048))
    return 0


if __name__ == "__main__":
    sys.exit(main())
