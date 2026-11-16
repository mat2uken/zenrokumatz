#!/usr/bin/env python3
"""Switch the X96Max_P boot target between Linux (SD card) and Android (eMMC).

The box keeps both boot recipes in the U-Boot environment:

  linux_boot        loads Image and the DTB from the SD card and runs booti
  bootcmd_android   the stock Android boot ("ddr_auto_fast_boot_check ...;run storeboot")
  bootcmd           whichever of the two is run at power on

This only rewrites `bootcmd` and saves the environment, so the other side is
never lost. Run it with the box at the U-Boot prompt (hold Enter while it
powers on), or let it reboot the board first with --reboot-from-linux.

Usage:
  uboot_set_boot.py --target android [--reboot-from-linux]
  uboot_set_boot.py --target linux
  uboot_set_boot.py --dump FILE          # back up the whole environment
"""
import argparse
import os
import re
import select
import sys
import termios
import time
import tty

PROMPT = re.compile(r"[A-Za-z0-9_.\-]{3,}#\s*$")

TARGETS = {
    "linux": "run linux_boot",
    # Two attempts: a cold boot sometimes runs the first fatload before the
    # stick is enumerated, and the retry costs nothing when the first works.
    "linux-usb": "run linux_boot_usb",
    "android": "run bootcmd_android",
}

# The two boot recipes. Defining them again is harmless and lets a fresh board
# (or one whose environment was reset) be set up by this tool alone.
LINUX_BOOT = (
    "setenv sddev 0; "
    "setenv kernel_addr_r 0x01080000; "
    "setenv fdt_addr_r 0x08000000; "
    "setenv dtb_mem_addr 0x08000000; "
    "setenv fdtaddr 0x08000000; "
    "mmc dev ${sddev}; "
    "mmc rescan; "
    "fatload mmc ${sddev}:1 ${kernel_addr_r} Image; "
    "fatload mmc ${sddev}:1 ${fdt_addr_r} meson-g12a-x96-max-p.dtb; "
    "setenv bootargs console=ttyAML0,115200n8 earlycon "
    "root=PARTUUID=96320001-02 rootfstype=ext4 rootwait rw; "
    "booti ${kernel_addr_r} - ${fdt_addr_r}"
)
# Boards whose SD slot does not work boot the same image from a USB stick.
# The external PHY DTB is the one the u211 board needs; an internal PHY board
# ignores it because the file simply is not there.
# The USB stick is the boot medium on boards without a working SD slot. The
# three second pause after usb start is what makes a cold boot reliable: the
# first fatload sometimes runs before the stick has been enumerated.
LINUX_BOOT_USB = (
    "setenv kernel_addr_r 0x01080000; "
    "setenv fdt_addr_r 0x08000000; "
    "setenv dtb_mem_addr 0x08000000; "
    "setenv fdtaddr 0x08000000; "
    "setenv bootargs console=ttyAML0,115200n8 earlycon "
    "root=PARTUUID=96320001-02 rootfstype=ext4 rootwait rw; "
    "for t in 1 2 3 4 5; do "
    "usb start; "
    "sleep 3; "
    "if fatload usb 0:1 ${kernel_addr_r} Image; then "
    "if fatload usb 0:1 ${fdt_addr_r} meson-g12a-x96-max-p-ephy.dtb; then "
    "booti ${kernel_addr_r} - ${fdt_addr_r}; "
    "fi; fi; "
    "echo usb boot attempt ${t} failed; "
    "usb reset; "
    "sleep 2; "
    "done"
)
ANDROID_BOOT = "ddr_auto_fast_boot_check 6 0 0 50;run storeboot"

RECIPES = [
    "setenv bootcmd_android '" + ANDROID_BOOT + "'",
    "setenv linux_boot '" + LINUX_BOOT + "'",
    "setenv linux_boot_usb '" + LINUX_BOOT_USB + "'",
]


def open_port(dev):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 1
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


class Console:
    def __init__(self, fd):
        self.fd = fd
        self.buffer = ""

    def read_for(self, seconds, quiet=False):
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.15)
            if not r:
                continue
            try:
                data = os.read(self.fd, 16384)
            except (BlockingIOError, OSError):
                continue
            if data:
                text = data.decode("utf-8", "replace")
                self.buffer = (self.buffer + text)[-65536:]
                if not quiet:
                    sys.stdout.write(text)
                    sys.stdout.flush()

    def send(self, line):
        self.buffer = ""
        os.write(self.fd, (line + "\r").encode())

    def command(self, line, timeout=25.0):
        """Sends one line and waits for the U-Boot prompt to come back."""
        self.send(line)
        start = time.time()
        self.read_for(0.4, quiet=True)
        while time.time() - start < timeout:
            self.read_for(0.2, quiet=True)
            if PROMPT.search(self.buffer):
                return self.buffer
        return ""

    def wait_for_prompt(self, seconds):
        """Sends one Enter at a time and waits for the prompt.

        Repeated Enters make this U-Boot rerun the previous command, so the
        key is sent once and then given a second to answer before trying again.
        """
        start = time.time()
        while time.time() - start < seconds:
            os.write(self.fd, b"\r")
            self.buffer = ""
            self.read_for(1.0, quiet=True)
            if PROMPT.search(self.buffer):
                return True
            time.sleep(1.0)
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dev", default="/dev/cu.usbserial-0001")
    ap.add_argument("--target", choices=sorted(TARGETS))
    ap.add_argument("--dump", default="",
                    help="write the full printenv output to this file and exit")
    ap.add_argument("--reboot-from-linux", action="store_true",
                    help="reboot the board over SSH first (the box must be running Linux)")
    ap.add_argument("--host", default="root@192.168.99.109")
    args = ap.parse_args()
    if not args.target and not args.dump:
        ap.error("give --target or --dump")

    if args.reboot_from_linux:
        print("[boot] rebooting %s" % args.host, file=sys.stderr)
        os.system("ssh -o BatchMode=yes -o UpdateHostKeys=no -o ConnectTimeout=8 "
                  "%s 'sync; reboot' >/dev/null 2>&1" % args.host)
        time.sleep(3)

    fd = open_port(args.dev)
    console = Console(fd)
    try:
        if not console.wait_for_prompt(90):
            print("[boot] no U-Boot prompt; power-cycle the box and retry",
                  file=sys.stderr)
            return 1
        print("[boot] at the U-Boot prompt", file=sys.stderr)

        if args.dump:
            out = console.command("printenv")
            if not out:
                print("[boot] printenv did not return", file=sys.stderr)
                return 1
            with open(args.dump, "w") as handle:
                handle.write(out)
            print("[boot] environment written to %s" % args.dump, file=sys.stderr)
            return 0

        for line in RECIPES:
            if not console.command(line):
                print("[boot] could not define the boot recipes", file=sys.stderr)
                return 1

        wanted = TARGETS[args.target]
        if not console.command("setenv bootcmd '%s'" % wanted):
            print("[boot] setenv failed", file=sys.stderr)
            return 1
        check = console.command("printenv bootcmd")
        if wanted not in check:
            print("[boot] bootcmd does not read back as %r; not saving" % wanted,
                  file=sys.stderr)
            return 1
        saved = console.command("saveenv")
        if "done" not in saved and "OK" not in saved:
            print("[boot] saveenv did not confirm; check the console", file=sys.stderr)
            return 1
        print("[boot] boot target is now %s (%s)" % (args.target, wanted),
              file=sys.stderr)
        return 0
    finally:
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
