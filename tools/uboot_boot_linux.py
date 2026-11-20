#!/usr/bin/env python3
"""Stop X96Max autoboot and start disk Linux or full RAM-root Linux.

Catch the UART U-Boot prompt, load each file with confirmation, then hand over
to the kernel log. Disk boot uses SD; RAM boot uses unit1 SD/2GiB, unit2
USB/4GiB or unit3 SD/DT295. Units 1 and 2 can instead load their files by TFTP.
RAM boot also checks the firmware board identity before loading a unit-specific
DTB; unit 3 picks its DTB from that identity.

Nothing is saved to the environment: these settings live in RAM only.

Usage:
  uboot_boot_linux.py [--dev /dev/cu.usbserial-0001] [--wait 180] [--log FILE]
  uboot_boot_linux.py --dev /dev/cu.usbserial-01025FCC --ramboot [--unit 2] [--tftp-server IPv4]
"""
import argparse
import os
import re
import select
import sys
import termios
import time
import tty

UBOOT_PROMPT = re.compile(r"(?:=>|([A-Za-z0-9_.\-]{3,})#)\s*$")
# The box also brings up Android, whose root shell looks similar but has a
# path before the '#'. Rebooting from there goes back through U-Boot.
# Android's console shell prompt is "console:/ #" when rooted and
# "console:/ $" otherwise; both must be recognized, or the box is left
# running Android and never passes through U-Boot again.
ANDROID_SHELL = re.compile(r"console:/\s*[#$]\s*$")

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


RAMBOOT_BOARDS = {1: "g12a_u212_2g", 2: "g12a_u211_4g", 3: "g12a_u212_v1"}
# Unit 3 accepts either board revision: the ePHY board (u211) needs the
# external-PHY DTB and the internal-PHY board (u212) the standard one.
# This U-Boot names the board in its prompt (g12a_u212_v1#) and in aml_dt,
# and prints aml_dt as soc/platform/variant (sm1 ac213 4g). The DTBs are only
# built for the SoC-level identity (g12a/sm1), so an exact DTB match is not
# possible; the gate proves the unit is not the other two units' boards.
UNIT3_DTBS = {"u211": "meson-g12a-x96-max-p-ephy-4g.dtb",
              "u212": "meson-g12a-x96-max-p.dtb"}
RAMBOOT_BOARDS_ANY = {3: tuple(UNIT3_DTBS)}


def ramboot_board_matches(text, unit):
    expected = RAMBOOT_BOARDS.get(unit)
    if expected is None:
        return False
    accepted = RAMBOOT_BOARDS_ANY.get(unit)
    if accepted is None:
        return re.search(r"(?:^|\n)aml_dt=" + re.escape(expected) + r"\r?(?:\n|$)", text) is not None
    return any(re.search(re.escape(board), text) for board in accepted)


def ramboot_setup(server=None, ipaddr=None, unit=2, usb=False):
    """Full RAM boot; unit 1 SD/2GiB, unit 2 USB/4GiB, unit 3 SD/DT295. Never saveenv."""
    if unit not in RAMBOOT_BOARDS:
        raise ValueError("RAM boot unit must be 1, 2 or 3")
    dtb = "meson-g12a-x96-max-p.dtb" if unit == 1 else "meson-g12a-x96-max-p-ephy-4g.dtb"
    setup = [
        "printenv aml_dt",
        "setenv kernel_addr_r 0x01080000", "setenv fdt_addr_r 0x08000000",
        "setenv dtb_mem_addr 0x08000000", "setenv fdtaddr 0x08000000",
        "setenv ramdisk_addr_r 0x10000000",
    ]
    if server:
        # Network boot works for every unit and needs no local medium, which is
        # what makes it the usable path when the SD card cannot be reached.
        import ipaddress
        server = str(ipaddress.IPv4Address(server))
        default_ipaddr = {1: "192.168.99.109", 2: "192.168.99.123", 3: "192.168.99.56"}[unit]
        ipaddr = str(ipaddress.IPv4Address(ipaddr or default_ipaddr))
        setup += [f"setenv ipaddr {ipaddr}", f"setenv serverip {server}"]
        load = "tftpboot"
        if unit == 3:
            # Unit 3's DTB is chosen by the board name in the U-Boot prompt, as
            # it is for local media, since this U-Boot's aml_dt is not a DTB name.
            setup += [f"setenv ramboot_dtb {next(iter(UNIT3_DTBS.values()))}"]
            for board, name in UNIT3_DTBS.items():
                setup += [f"if test \"${{board#*{board}}}\" != \"${{board}}\"; then setenv ramboot_dtb {name}; fi"]
            setup += ["printenv ramboot_dtb"]
            dtb = "${ramboot_dtb}"
    elif usb:
        # Load from a USB stick instead of the SD card. U-Boot on this board has
        # no fatwrite and its TFTP stack cannot transmit, so a stick is the only
        # way to swap the kernel and initramfs without removing the card.
        setup += ["usb start", "sleep 3"]
        load = "fatload usb 0:1"
        if unit == 3:
            setup += [f"setenv ramboot_dtb {next(iter(UNIT3_DTBS.values()))}"]
            for board, name in UNIT3_DTBS.items():
                setup += [f"if test \"${{board#*{board}}}\" != \"${{board}}\"; then setenv ramboot_dtb {name}; fi"]
            setup += ["printenv ramboot_dtb"]
            dtb = "${ramboot_dtb}"
    elif unit == 3:
        # The U-Boot prompt names the board (g12a_u212_v1#, g12a_u211_v1#).
        # The SD card must be powered before any FAT access is attempted, so
        # init the slot first. This U-Boot has no "board" variable, so the
        # caller sets it from the prompt before these commands run; without a
        # value the internal-PHY DTB (the u212 board) is the default.
        setup += ["printenv aml_dt", "setenv sddev 0", "mmc dev ${sddev}", "mmc rescan"]
        setup += [f"setenv ramboot_dtb {next(iter(UNIT3_DTBS.values()))}"]
        # Choose the DTB by substring, not by equality: this U-Boot's board
        # name is "g12a_u212_v1", so an exact "u212" comparison never matches.
        for board, name in UNIT3_DTBS.items():
            setup += [f"if test \"${{board#*{board}}}\" != \"${{board}}\"; then setenv ramboot_dtb {name}; fi"]
        setup += ["printenv board", "printenv ramboot_dtb"]
        load = "fatload mmc ${sddev}:1"
        dtb = "${ramboot_dtb}"
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
        "setenv bootargs 'console=ttyAML0,115200n8 earlycon rdinit=/init panic_on_oops=1 panic=8'",
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
            # A shorter select timeout drains the kernel tty buffer more often.
            # At 0.1s the boot log of this kernel overflowed it and whole lines
            # were lost, which made the abort's call trace unreadable.
            r, _, _ = select.select([self.fd], [], [], 0.02)
            if not r:
                continue
            try:
                data = os.read(self.fd, 65536)
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
        found = UBOOT_PROMPT.search(console.tail)
        if found:
            # This U-Boot uses "<board>#" (g12a_u212_v1#) as its prompt, so the
            # prompt is the most reliable board identity on this unit.
            console.board = found.group(1) or ""
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
    ap.add_argument("--unit", type=int, choices=(1, 2, 3), default=2,
                    help="RAM boot unit: 1=SD/2GiB, 2=USB/4GiB, 3=SD/DT295 (default 2)")
    ap.add_argument("--tftp-server", help="load RAM-boot files from this IPv4 address instead of local media")
    ap.add_argument("--usb", action="store_true",
                    help="load RAM-boot files from a FAT USB stick instead of the SD card")
    ap.add_argument("--ipaddr", help="temporary TFTP IPv4 (default: unit1 .109, unit2 .123)")
    ap.add_argument("--dtb", help="load this DTB name instead of the unit's default "
                                  "(for example the unit-3 diagnostic meson-g12a-x96-max-p-nousb.dtb)")
    args = ap.parse_args()
    if args.dtb and not args.ramboot:
        ap.error("--dtb requires --ramboot")
    if args.usb and not args.ramboot:
        ap.error("--usb requires --ramboot")
    if args.tftp_server and args.usb:
        ap.error("--tftp-server and --usb are mutually exclusive")
    if args.tftp_server and not args.ramboot:
        ap.error("--tftp-server requires --ramboot")
    try:
        setup, boot = ramboot_setup(args.tftp_server, args.ipaddr, args.unit, args.usb) if args.ramboot else (SETUP, BOOT)
    except ValueError as exc:
        ap.error(str(exc))
    if args.dtb:
        # Replace the unit's own DTB selection with one explicit file name.
        # The boot still requires this load's byte count, so a wrong name stops
        # before booti instead of booting stale memory.
        setup = [command for command in setup
                 if not command.startswith(("setenv ramboot_dtb", "if test",
                                            "printenv board", "printenv ramboot_dtb"))]
        position = next(i for i, command in enumerate(setup) if command.startswith("fatload ")
                        and "${ramboot_dtb}" in command)
        setup.insert(position, "setenv ramboot_dtb %s" % args.dtb)
        print("[boot] DTB override: %s" % args.dtb, file=sys.stderr)

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
        if getattr(console, "board", ""):
            # The prompt itself (g12a_u212_v1#) carries the board identity.
            # U-Boot has no such variable, so define it before the setup that
            # selects the DTB by board name.
            print("[boot] board from prompt: %s" % console.board, file=sys.stderr)
            if args.ramboot and args.unit == 3:
                setup.insert(0, "setenv board %s" % console.board)

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
