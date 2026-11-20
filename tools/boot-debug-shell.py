#!/usr/bin/env python3
"""Boot straight into a shell and drive commands over the serial console.

The unit's init scripts start pix-stream, which opens the DT295 and then wedges
the whole box (round 156/163). With rdinit=/bin/sh nothing runs, so the VPU can
be exercised without touching the tuner at all -- and without needing the SD
card removed from the unit.

  boot-debug-shell.py --cmd 'dmesg | grep amvenc' --cmd 'ls /dev/video*'

Each command is echoed, then the console is read until it goes quiet. Output is
also written to work/debug-shell.log so the last line before a hang survives.
"""
import argparse
import os
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import uboot_boot_linux as uboot  # noqa: E402

SERIAL = "/dev/cu.usbserial-01025FC4"
KERNEL_ADDR = "0x01080000"
INITRD_ADDR = "0x10000000"
DTB_ADDR = "0x08000000"
PROMPT = re.compile(r"(^|\n)[^\n]*[#\$] $")


def catch(fd, console, tries=250, pause=0.1):
    console.tail = ""
    for _ in range(tries):
        os.write(fd, b"\r")
        console.pump(pause)
        if uboot.UBOOT_PROMPT.search(console.tail.replace("\r", "")):
            return True
    return False


def run(console, fd, command, wait=6.0):
    console.tail = ""
    os.write(fd, (command + "\r").encode())
    console.pump(wait)
    return console.tail.replace("\r", "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dtb", default="meson-g12a-x96-max-p-sm1.dtb")
    ap.add_argument("--cmd", action="append", default=[],
                    help="command to run in the debug shell (repeatable)")
    ap.add_argument("--wait-boot", type=float, default=45.0)
    args = ap.parse_args()

    log_path = ROOT / "work" / "debug-shell.log"
    fd = uboot.open_port(SERIAL)
    log = open(log_path, "wb", buffering=0)
    console = uboot.Console(fd, log)
    console.pump(1.0)

    if not catch(fd, console):
        print("could not reach U-Boot", file=sys.stderr)
        return 1
    print("U-Boot prompt reached")

    console.tail = ""
    os.write(fd, b"usb start\r")
    console.pump(10.0)
    print("usb start:", "storage found" if "Storage Device" in console.tail else "no storage")

    for cmd, wait in (
        (f"fatload mmc 0:1 {KERNEL_ADDR} Image", 30.0),
        (f"fatload mmc 0:1 {DTB_ADDR} {args.dtb}", 10.0),
        (f"fatload mmc 0:1 {INITRD_ADDR} rootfs.cpio.uboot", 25.0),
    ):
        console.tail = ""
        os.write(fd, (cmd + "\r").encode())
        console.pump(wait)
        if "bytes read" not in console.tail:
            print("load failed:", cmd, file=sys.stderr)
            return 1
    print("kernel, dtb and initramfs loaded")

    # rdinit=/bin/sh skips /init, so no service touches the tuner or the VPU.
    console.tail = ""
    os.write(fd, (f"setenv bootargs console=ttyAML0,115200n8 earlycon "
                  f"rdinit=/bin/sh panic_on_oops=1 panic=8\r").encode())
    console.pump(3.0)
    os.write(fd, (f"booti {KERNEL_ADDR} {INITRD_ADDR} {DTB_ADDR}\r").encode())

    deadline = time.time() + args.wait_boot
    while time.time() < deadline:
        console.pump(2.0)
        if PROMPT.search(console.tail.replace("\r", "")):
            break
    print("shell reached" if PROMPT.search(console.tail.replace("\r", ""))
          else "shell prompt not seen")

    # Bring up just enough of /dev, /proc and /sys for modprobe and ffmpeg.
    for setup in ("/bin/mount -t proc proc /proc",
                  "/bin/mount -t sysfs sys /sys",
                  "/bin/mount -t devtmpfs dev /dev 2>/dev/null"):
        run(console, fd, setup, 2.0)

    for command in args.cmd:
        print(f"\n$ {command}")
        output = run(console, fd, command, 20.0)
        print(output.strip()[-2000:])

    os.close(fd)
    print(f"\nfull transcript: {log_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
