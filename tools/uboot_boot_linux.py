#!/usr/bin/env python3
"""Stop X96Max autoboot and start disk Linux or full RAM-root Linux.

Catch the UART U-Boot prompt, load each file with confirmation, then hand over
to the kernel log. Disk boot uses SD; RAM boot uses unit1 SD/2GiB or unit2
USB/4GiB. Either RAM profile can instead load its files by TFTP. RAM boot also
checks the firmware board identity before loading a unit-specific DTB.

Nothing is saved to the environment: these settings live in RAM only.

Usage:
  uboot_boot_linux.py [--dev /dev/cu.usbserial-0001] [--wait 180] [--log FILE]
  uboot_boot_linux.py --dev /dev/cu.usbserial-01025FCC --ramboot [--tftp-server IPv4]
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
            "Not a valid FAT", "Unable to read file", "No partition table",
            "File not found", "Bad Linux ARM64 Image magic", "TFTP error",
            "Retry count exceeded", "ERROR: reserving fdt memory region failed",
            "Wrong Ramdisk Image Format", "Ramdisk image is corrupt or invalid")


RAMBOOT_BOARDS = {1: "g12a_u212_2g", 2: "g12a_u211_4g"}


def ramboot_board_matches(text, unit):
    expected = RAMBOOT_BOARDS.get(unit)
    return expected is not None and re.search(
        r"(?:^|\n)aml_dt=" + re.escape(expected) + r"\r?(?:\n|$)", text) is not None


def ramboot_setup(server=None, ipaddr=None, unit=2):
    """Full RAM boot; unit 1 uses SD/2GiB, unit 2 USB/4GiB. Never saveenv."""
    if unit not in RAMBOOT_BOARDS:
        raise ValueError("RAM boot unit must be 1 or 2")
    dtb = "meson-g12a-x96-max-p.dtb" if unit == 1 else "meson-g12a-x96-max-p-ephy-4g.dtb"
    setup = [
        "printenv aml_dt",
        "setenv kernel_addr_r 0x01080000", "setenv fdt_addr_r 0x08000000",
        "setenv dtb_mem_addr 0x08000000", "setenv fdtaddr 0x08000000",
        "setenv ramdisk_addr_r 0x10000000",
    ]
    if server:
        import ipaddress
        server = str(ipaddress.IPv4Address(server))
        ipaddr = str(ipaddress.IPv4Address(ipaddr or ("192.168.99.109" if unit == 1 else "192.168.99.123")))
        setup += [f"setenv ipaddr {ipaddr}", f"setenv serverip {server}"]
        load = "tftpboot"
    elif unit == 1:
        setup += ["setenv sddev 0", "mmc dev ${sddev}", "mmc rescan"]
        load = "fatload mmc ${sddev}:1"
    else:
        # Match the saved unit-2 recipe's cold-enumeration settling interval.
        setup += ["usb start", "sleep 3"]
        load = "fatload usb 0:1"
    setup += [
        f"{load} ${{kernel_addr_r}} Image",
        f"{load} ${{fdt_addr_r}} {dtb}",
        f"{load} ${{ramdisk_addr_r}} rootfs.cpio.uboot",
        "setenv bootargs 'console=ttyAML0,115200n8 earlycon rdinit=/init'",
    ]
    return setup, "booti ${kernel_addr_r} ${ramdisk_addr_r} ${fdt_addr_r}"


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
    # A prompt left by the preceding command is not completion evidence.
    console.tail = ""
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
    ap.add_argument("--ramboot", action="store_true", help="full RAM-root boot from the selected unit's medium")
    ap.add_argument("--unit", type=int, choices=(1, 2), default=2,
                    help="RAM boot unit: 1=SD/2GiB, 2=USB/4GiB (default2)")
    ap.add_argument("--tftp-server", help="load RAM-boot files from this IPv4 address instead of local media")
    ap.add_argument("--ipaddr", help="temporary TFTP IPv4 (default: unit1 .109, unit2 .123)")
    args = ap.parse_args()
    if args.tftp_server and not args.ramboot:
        ap.error("--tftp-server requires --ramboot")
    try:
        setup, boot = ramboot_setup(args.tftp_server, args.ipaddr, args.unit) if args.ramboot else (SETUP, BOOT)
    except ValueError as exc:
        ap.error(str(exc))

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

        for command in setup:
            print("[boot] %s" % command, file=sys.stderr)
            if not run(console, command, timeout=180.0 if command.startswith(("tftpboot ", "fatload ")) else 25.0):
                print("[boot] no prompt after: %s" % command, file=sys.stderr)
                return 1
            for marker in FAILURES:
                if marker in console.tail:
                    print("[boot] the command failed (%s); stopping" % marker,
                          file=sys.stderr)
                    return 1

            if args.ramboot and command == "printenv aml_dt" and not ramboot_board_matches(console.tail, args.unit):
                print("[boot] board does not match selected unit; stopping before loads", file=sys.stderr)
                return 1

            if command.startswith(("fatload ", "tftpboot ")):
                counts = re.findall(r"(?:(\d+) bytes read|Bytes transferred = (\d+))", console.tail)
                if not counts or not any(int(a or b) > 0 for a, b in counts):
                    print("[boot] load has no positive byte-count confirmation; stopping", file=sys.stderr)
                    return 1

        print("[boot] %s" % boot, file=sys.stderr)
        console.tail = ""
        os.write(fd, (boot + "\r").encode())
        console.pump(args.trace)
        if "Kernel panic" in console.tail or UBOOT_PROMPT.search(console.tail):
            print("[boot] boot failed; inspect the log", file=sys.stderr)
            return 1
        print("[boot] boot command sent; verify login/services in %s" % args.log, file=sys.stderr)
        return 0
    finally:
        log.close()
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
