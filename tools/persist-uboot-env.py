#!/usr/bin/env python3
"""Persist the RAM-boot U-Boot environment so a power cycle boots Linux.

Reaches the U-Boot prompt (rebooting Android out of the way if needed), sends
the setenv lines from an env file, saves the environment to eMMC and boots once
through the new path.

  persist-uboot-env.py output/ramboot-unit3/ramboot-unit3-uboot.txt

Lines in the env file: setenv/comment/run/echo are sent verbatim. The final
bootcmd+saveenv lines must be present in the file; nothing is guessed here.
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


def run(console, fd, command, wait):
    console.tail = ""
    os.write(fd, (command + "\r").encode())
    deadline = time.time() + wait
    while time.time() < deadline:
        console.pump(1.0)
        if re.search(r"(^|\n)[^\n]*[#>][>]? $", console.tail.replace("\r", "")):
            break
    return console.tail.replace("\r", "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("envfile")
    ap.add_argument("--boot", action="store_true",
                    help="run the final 'run linux_boot_ram' line after saveenv")
    ap.add_argument("--wait", type=float, default=90.0,
                    help="seconds to watch the boot after the last command")
    args = ap.parse_args()

    lines = [ln.strip() for ln in Path(args.envfile).read_text().splitlines()]
    lines = [ln for ln in lines if ln and not ln.startswith("#")]

    log_path = ROOT / "work" / "persist-uboot.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log = open(log_path, "ab", buffering=0)
    fd = uboot.open_port(SERIAL)
    console = uboot.Console(fd, log)
    console.pump(1.0)

    if not uboot.catch_prompt(console, 180.0):
        print("could not reach U-Boot", file=sys.stderr)
        return 1
    print("U-Boot prompt reached")

    for ln in lines:
        out = run(console, fd, ln, 12.0 if not ln.startswith("run ") else args.wait)
        tail = out.strip()[-300:]
        print(f"$ {ln}\n  {tail}\n")
        if ln.startswith("saveenv"):
            ok = "OK" in out or "saved" in out.lower()
            print("saveenv:", "ok" if ok else "CHECK OUTPUT")
    os.close(fd)
    print(f"full transcript: {log_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
