#!/usr/bin/env python3
"""Read USB registers from U-Boot's monitor, before any driver touches them.

Linux cannot answer this question: without the USB nodes the gate clock is
disabled and the whole region reads as a Bus error, and with them enabled
dwc3_meson_g12a_probe writes USB_R1 and faults. U-Boot still has the clocks up,
so "md" is the only place the registers can be inspected.

  uboot-md.py --address 0xffe09084 --count 4
  uboot-md.py --address 0xffe09000 --count 8 --reboot-if-android
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--address", required=True, help="first address, e.g. 0xffe09084")
    ap.add_argument("--count", type=int, default=1, help="number of words (default 1)")
    ap.add_argument("--wait", type=float, default=180.0)
    args = ap.parse_args()

    fd = uboot.open_port(SERIAL)
    log = open(ROOT / "work" / "uboot-md.log", "wb", buffering=0)
    console = uboot.Console(fd, log)
    console.tail = ""
    console.pump(1.0)
    prompt = uboot.catch_prompt(console, args.wait)
    print(f"prompt: {prompt}")
    if not prompt:
        os.close(fd)
        raise SystemExit("could not reach U-Boot")

    # Stop any pending autoboot before issuing the read.
    os.write(fd, b"\x03")
    console.pump(0.5)
    console.tail = ""
    os.write(fd, ("md %s %d\r" % (args.address, args.count)).encode())
    console.pump(4.0)
    text = console.tail.replace("\r", "")
    os.close(fd)

    print("\n=== md output ===")
    for line in text.splitlines():
        if re.search(r"^[0-9a-f]{8}:", line) or "Bus error" in line or "data abort" in line:
            print("  " + line.strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
