#!/usr/bin/env python3
"""Run commands on the already-running serial shell (no reboot).

The boot script always goes through U-Boot, which reboots the unit and wipes
/tmp. When the box already sits at a shell prompt (rdinit=/bin/sh), use this to
send commands over the same console instead.

  serial-run.py 'ls /tmp/*.ts'
  serial-run.py --wait 30 'pix-ts-legacy ... > /tmp/c.ts'
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
PROMPT = re.compile(r"(^|\n)[^\n]*[#\$] $")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", nargs="+", help="command to run on the live shell")
    ap.add_argument("--wait", type=float, default=6.0,
                    help="seconds to read the console after each command")
    ap.add_argument("--log", default=str(ROOT / "work" / "serial-run.log"))
    args = ap.parse_args()

    log_path = Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    fd = uboot.open_port(SERIAL)
    log = open(log_path, "ab", buffering=0)
    console = uboot.Console(fd, log)
    console.pump(1.0)

    for command in args.command:
        console.tail = ""
        os.write(fd, (command + "\r").encode())
        # Poll until the prompt reappears or the wait budget is spent.
        deadline = time.time() + max(args.wait, 2.0)
        while time.time() < deadline:
            console.pump(1.0)
            if PROMPT.search(console.tail.replace("\r", "")):
                break
        out = console.tail.replace("\r", "").strip()
        print(f"$ {command}")
        print(out[:4000])
        print()

    os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
