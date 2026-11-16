#!/usr/bin/env python3
"""Stop the X96Max autoboot and start Linux from the SD card.

The box boots Android from eMMC on its own. Linux only starts when the U-Boot
prompt is caught and the load commands are typed in, so this waits on the UART
for the prompt, sends the sequence from board/x96max/uboot-manual.txt one line
at a time (waiting for the prompt to come back after each), and then hands over
to the kernel log.

Nothing is saved to the environment: these settings live in RAM only.

Usage:
  uboot_boot_linux.py [--dev /dev/cu.usbserial-0001] [--wait 180] [--log FILE]
"""
import argparse
import os
import re
import select
import sys
import termios
import time
import tty

UBOOT_PROMPT = re.compile(r"(?:=>|[A-Za-z0-9_.\-]{3,})#\s*$")
# The box also brings up Android, whose root shell looks similar but has a
# path before the '#'. Rebooting from there goes back through U-Boot.
ANDROID_SHELL = re.compile(r"console:/\s*#\s*$")

# From board/x96max/uboot-manual.txt. The inspection commands (version,
# printenv, help) are left out; the rest is what the board needs.
SETUP = [
    "setenv sddev 0",
    "setenv kernel_addr_r 0x01080000",
    "setenv fdt_addr_r 0x08000000",
    "setenv dtb_mem_addr 0x08000000",
    "setenv fdtaddr 0x08000000",
    "mmc dev ${sddev}",
    "mmc rescan",
    "fatls mmc ${sddev}:1",
    "fatload mmc ${sddev}:1 ${kernel_addr_r} Image",
    "fatload mmc ${sddev}:1 ${fdt_addr_r} meson-g12a-x96-max-p.dtb",
    "setenv bootargs 'console=ttyAML0,115200n8 earlycon "
    "root=PARTUUID=96320001-02 rootfstype=ext4 rootwait rw'",
]
BOOT = "booti ${kernel_addr_r} - ${fdt_addr_r}"

# The loaders print these when they could not read the card. Continuing would
# start the kernel from whatever happens to be in RAM.
FAILURES = ("** Bad device", "Wrong Image Format", "ERROR: can't get", "Failed to load",
            "Not a valid FAT", "Unable to read file")


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
    def __init__(self, fd, log):
        self.fd = fd
        self.log = log
        self.tail = ""

    def pump(self, seconds):
        """Reads for a while and returns what arrived."""
        got = ""
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if not r:
                continue
            try:
                data = os.read(self.fd, 4096)
            except (BlockingIOError, OSError):
                continue
            if data:
                text = data.decode("utf-8", "replace")
                got += text
                self.tail = (self.tail + text)[-4000:]
                sys.stdout.write(text)
                sys.stdout.flush()
                if self.log:
                    self.log.write(data)
                    self.log.flush()
        return got


def catch_prompt(console, wait_seconds):
    """Spams Enter until the U-Boot prompt appears.

    If Android came up instead, reboot it so the box goes through U-Boot
    again. Returns the name of the prompt that was reached, or None.
    """
    start = time.time()
    last = 0.0
    rebooted = False
    while time.time() - start < wait_seconds:
        now = time.time()
        if ANDROID_SHELL.search(console.tail) and not rebooted:
            print("[boot] Android is up; rebooting it to reach U-Boot",
                  file=sys.stderr)
            os.write(console.fd, b"reboot\r")
            rebooted = True
            console.tail = ""
            console.pump(1.0)
            continue
        if now - last > 0.25:
            os.write(console.fd, b"\r")
            last = now
        console.pump(0.1)
        if UBOOT_PROMPT.search(console.tail):
            return "uboot"
    return None


def run(console, command, timeout=25.0):
    """Sends one line and waits for the prompt to come back."""
    os.write(console.fd, (command + "\r").encode())
    start = time.time()
    console.pump(0.3)
    while time.time() - start < timeout:
        console.pump(0.2)
        if UBOOT_PROMPT.search(console.tail):
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dev", default="/dev/cu.usbserial-0001")
    ap.add_argument("--wait", type=float, default=300.0,
                    help="seconds to keep pressing Enter while waiting for power")
    ap.add_argument("--log", default="/tmp/uboot-linux-boot.log")
    ap.add_argument("--trace", type=float, default=20.0,
                    help="seconds of kernel log to show after booti")
    args = ap.parse_args()

    log = open(args.log, "ab", buffering=0)
    fd = open_port(args.dev)
    console = Console(fd, log)
    try:
        print("[boot] waiting for the U-Boot prompt (up to %.0fs)" % args.wait,
              file=sys.stderr)
        if catch_prompt(console, args.wait) != "uboot":
            print("[boot] no prompt; the box did not reach U-Boot", file=sys.stderr)
            return 1
        print("[boot] prompt caught", file=sys.stderr)

        for command in SETUP:
            print("[boot] %s" % command, file=sys.stderr)
            if not run(console, command):
                print("[boot] no prompt after: %s" % command, file=sys.stderr)
                return 1
            for marker in FAILURES:
                if marker in console.tail:
                    print("[boot] the command failed (%s); stopping" % marker,
                          file=sys.stderr)
                    return 1

        print("[boot] %s" % BOOT, file=sys.stderr)
        os.write(fd, (BOOT + "\r").encode())
        console.pump(args.trace)
        print("[boot] done; kernel log in %s" % args.log, file=sys.stderr)
        return 0
    finally:
        log.close()
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
