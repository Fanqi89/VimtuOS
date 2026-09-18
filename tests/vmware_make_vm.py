#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""在 VMware 里搭一个「安装 VimtuOS 64 位」的测试虚拟机。

做法：
  * 新建独立目录（不动用户原有的 VimtuOS 64-bit.vmx）
  * 把安装介质 vimtu64-64.img 复制成 installer.img，并生成 VMDK 描述符（monolithicFlat）
  * 生成 16MB 空目标盘 target.img + target.vmdk
  * 写两份 .vmx：
      vimtu64-install-test.vmx   IDE0:0=安装介质  IDE0:1=目标盘（跑安装）
      vimtu64-installed-boot.vmx IDE0:0=目标盘（单独启动装好的系统）
    两份都是：BIOS 固件、guestOS=other-64（必须 64 位类型，否则 VMware 屏蔽 CPUID 长模式位）、
    serial0 -> 文件（看客人日志）、serial1 -> 命名管道（给客人送按键，走 COM2 无人值守通道）。
"""
import argparse
import os
import shutil
import sys

ROOT = r"C:\Users\fanqi\Desktop\VimtuOS\Vimtu64"
BASE = r"C:\Users\fanqi\Desktop\新建文件夹"
TESTDIR = os.path.join(BASE, "v64-install-test")

MEDIUM = os.path.join(ROOT, "vimtu64-64.img")
TARGET_SECTORS = 32768          # 16 MB
VNC_PORT = 5903
KEY_TCP_PORT = 4557          # COM2（串口按键通道）走 TCP，VMware 当服务端、我们连上去写按键


def vmdk_descriptor(path, flat_name, sectors, cid):
    txt = """# Disk DescriptorFile
version=1
encoding="UTF-8"
CID=%08x
parentCID=ffffffff
createType="monolithicFlat"

# Extent description
RW %d FLAT "%s" 0

# The Disk Data Base
#DDB

ddb.virtualHWVersion = "7"
ddb.geometry.cylinders = "%d"
ddb.geometry.heads = "16"
ddb.geometry.sectors = "63"
ddb.adapterType = "ide"
""" % (cid, sectors, flat_name, max(1, sectors // (16 * 63)))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(txt)


def disk_entry(slot, dev_type, file_name):
    return ("""ide%s.present = "TRUE"
ide%s.deviceType = "%s"
ide%s.fileName = "%s"
ide%s.startConnected = "TRUE"
""" % (slot, slot, dev_type, slot, file_name, slot))


def vmx_text(title, disks, serial_log, key_pipe=None, vnc_port=None, iso=None, uefi=False):
    txt = """#!/usr/bin/vmware
.encoding = "UTF-8"
config.version = "8"
virtualHW.version = "7"
displayName = "%s"
guestOS = "other-64"
msg.autoAnswer = "TRUE"
numvcpus = "1"
memsize = "512"
svga.vramSize = "134217728"
""" % title
    for slot, dev, fn in disks:
        txt += disk_entry(slot, dev, fn)
    if not any(s == "0:1" for s, _, _ in disks):
        txt += 'ide0:1.present = "FALSE"\n'
    if iso:
        txt += ('ide1:0.present = "TRUE"\n'
                'ide1:0.deviceType = "cdrom-image"\n'
                'ide1:0.fileName = "%s"\n'
                'ide1:0.startConnected = "TRUE"\n'
                'bios.bootOrder = "cdrom,hdd"\n' % iso.replace("\\", "\\\\"))
    else:
        txt += 'ide1:0.present = "FALSE"\n'
    txt += """floppy0.present = "FALSE"
ethernet0.present = "FALSE"
usb.present = "FALSE"
sound.present = "FALSE"
vmci0.present = "FALSE"
tools.syncTime = "FALSE"
tools.install.state = "none"
tools.upgrade.policy = "manual"
mks.enable3d = "FALSE"
isolation.tools.hgfs.disable = "TRUE"
firmware = "%s"
bios.bootDelay = "2000"
""" % ("efi" if uefi else "bios")
    if uefi:
        # UEFI 模式：不开 Secure Boot（我们自己签不了名），其余交给 VMware 的 EFI 固件
        txt += 'uefi.secureBoot = "FALSE"\n'
    txt += """serial0.present = "TRUE"
serial0.fileType = "file"
serial0.fileName = "%s"
serial0.tryNoRxLoss = "FALSE"
serial0.startConnected = "TRUE"
""" % serial_log.replace("\\", "\\\\")
    if key_pipe:
        # 注意：VMware 的 pipe 串口在本机建不起来（日志："Unable to create the server-side
        # instance of the ... named pipe"），两种写法都失败；改用 network(TCP) 模式：
        # VMware 当服务端监听，我们连上去写字节 -> 字节进客人的 COM2 接收缓冲。
        txt += """serial1.present = "TRUE"
serial1.fileType = "network"
serial1.fileName = "tcp://127.0.0.1:%d"
serial1.mode = "server"
serial1.startConnected = "TRUE"
""" % KEY_TCP_PORT
    if vnc_port:
        txt += """RemoteDisplay.vnc.enabled = "TRUE"
RemoteDisplay.vnc.port = "%d"
""" % vnc_port
    return txt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep-target", action="store_true", help="不重建目标盘（保留上次安装结果）")
    ap.add_argument("--iso", action="store_true",
                    help="用 ISO 当安装介质（虚拟光驱引导）而不是裸盘")
    ap.add_argument("--uefi", action="store_true",
                    help="用 UEFI 固件（firmware=efi）而不是 BIOS；目录换成 v64-uefi-test")
    args = ap.parse_args()
    if args.uefi:
        # UEFI 测试用独立目录，避免覆盖 BIOS 那套 VM/目标盘
        global TESTDIR
        TESTDIR = os.path.join(BASE, "v64-uefi-test")

    if not os.path.exists(MEDIUM):
        print("缺少安装介质 %s（先跑 build64.sh）" % MEDIUM)
        return 2
    os.makedirs(TESTDIR, exist_ok=True)

    # 1) 安装介质副本（避免 VMware 直接碰构建产物）
    ins_img = os.path.join(TESTDIR, "installer.img")
    src_size = os.path.getsize(MEDIUM)
    # ★ 每次都必须复制：镜像总大小恒定（16266 扇区），只比大小会漏掉内核改动，
    #   结果 VMware 里跑的还是旧内核（踩过：串口按键通道没生效就是这个原因）。
    shutil.copyfile(MEDIUM, ins_img)
    ins_sectors = src_size // 512
    print("[vm] installer.img = %d bytes (%d 扇区)" % (src_size, ins_sectors))

    # 2) 目标盘
    tgt_img = os.path.join(TESTDIR, "target.img")
    if (not args.keep_target) or (not os.path.exists(tgt_img)):
        with open(tgt_img, "wb") as f:
            f.write(b"\0" * (TARGET_SECTORS * 512))
    print("[vm] target.img = %d 扇区" % TARGET_SECTORS)

    # 3) VMDK 描述符
    vmdk_descriptor(os.path.join(TESTDIR, "installer.vmdk"), "installer.img", ins_sectors, 0x11111111)
    vmdk_descriptor(os.path.join(TESTDIR, "target.vmdk"), "target.img", TARGET_SECTORS, 0x22222222)

    # 4) 安装 VM
    install_serial = os.path.join(TESTDIR, "serial-install.log")
    boot_serial = os.path.join(TESTDIR, "serial-installed-boot.log")
    for p in (install_serial, boot_serial):
        if os.path.exists(p):
            os.remove(p)
    vmx1 = os.path.join(TESTDIR, "vimtu64-install-test.vmx")
    if args.iso:
        # ISO 模式：只挂目标盘 + 把 ISO 当虚拟光驱，从光驱引导
        with open(vmx1, "w", encoding="utf-8", newline="\n") as f:
            f.write(vmx_text("VimtuOS 64 位安装测试(ISO%s)" % ("/UEFI" if args.uefi else ""),
                             [("0:0", "disk", "target.vmdk")],
                             install_serial, key_pipe=True, vnc_port=VNC_PORT,
                             iso=os.path.join(ROOT, "vimtu64-64.iso"),
                             uefi=args.uefi))
    else:
        with open(vmx1, "w", encoding="utf-8", newline="\n") as f:
            f.write(vmx_text("VimtuOS 64 位安装测试%s" % ("(UEFI)" if args.uefi else ""),
                             [("0:0", "disk", "installer.vmdk"), ("0:1", "disk", "target.vmdk")],
                             install_serial, key_pipe=True, vnc_port=VNC_PORT,
                             uefi=args.uefi))

    # 5) "只挂装好的盘" 的 VM（验证装完真的能开机）
    vmx2 = os.path.join(TESTDIR, "vimtu64-installed-boot.vmx")
    with open(vmx2, "w", encoding="utf-8", newline="\n") as f:
        f.write(vmx_text("VimtuOS 64 位已安装盘启动测试%s" % ("(UEFI)" if args.uefi else ""),
                         [("0:0", "disk", "target.vmdk")],
                         boot_serial, key_pipe=None, vnc_port=None, uefi=args.uefi))

    print("[vm] 安装 VM   = %s" % vmx1)
    print("[vm] 启动 VM   = %s" % vmx2)
    print("[vm] 串口输出  = %s" % install_serial)
    print("[vm] 按键通道  = COM2 <- tcp://127.0.0.1:%d（VMware 当服务端）" % KEY_TCP_PORT)
    print("[vm] VNC(只读) = 127.0.0.1:%d" % VNC_PORT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
