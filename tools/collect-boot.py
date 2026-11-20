#!/usr/bin/env python3
"""Boot unit 3 with usb start and collect whatever the box managed to report.

The board stops answering on both the serial console and the network after
"usb start", so a plain boot leaves nothing to inspect. This runs the boot,
waits, then pulls the network state from the places it can survive:

  1. /dev/shm/netdiag.txt over ssh (works once the network is up)
  2. netdiag.txt on the SD card's FAT partition (works when the console and the
     network are both dead: mount the card on the Mac and read it)

  collect-boot.py --dtb meson-g12a-x96-max-p-ephy-4g.dtb
  collect-boot.py --dtb ... --from-card /dev/disk10   # read the card instead
"""
import argparse
import plistlib
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOST = "192.168.99.56"


def reachable(port):
    import socket
    with socket.socket() as probe:
        probe.settimeout(2)
        return probe.connect_ex((HOST, port)) == 0


def ssh(command, timeout=15):
    result = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", f"root@{HOST}", command],
        capture_output=True, text=True, timeout=timeout)
    return result.stdout.strip()


def read_from_card(device, keep=True):
    """Mount the card's FAT partition and read netdiag.txt off it."""
    subprocess.run(["diskutil", "mount", device + "s1"], capture_output=True)
    info = subprocess.run(["diskutil", "info", "-plist", device + "s1"],
                          capture_output=True, text=True)
    volume = plistlib.loads(info.stdout.encode()).get("MountPoint")
    if not volume:
        raise SystemExit("the card's FAT volume did not mount")
    target = Path(volume) / "netdiag.txt"
    if not target.is_file():
        print(f"no netdiag.txt on the card ({volume})")
        return None
    text = target.read_text(errors="replace")
    print(text)
    if not keep:
        subprocess.run(["diskutil", "unmountDisk", device], capture_output=True)
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dtb", default="meson-g12a-x96-max-p-ephy-4g.dtb")
    ap.add_argument("--files", nargs="*", default=["Image"])
    ap.add_argument("--trace", type=float, default=70.0)
    ap.add_argument("--wait", type=float, default=90.0,
                    help="seconds to wait after boot before collecting")
    ap.add_argument("--from-card", metavar="DEVICE",
                    help="skip booting; just read netdiag.txt off this card")
    args = ap.parse_args()

    if args.from_card:
        return 0 if read_from_card(args.from_card) else 1

    installer = ROOT / "tools/install-boot.py"
    print("=== booting with usb start ===")
    subprocess.run([sys.executable, str(installer), "--dtb", args.dtb,
                    "--files", *args.files, "--usb-start",
                    "--trace", str(args.trace)])

    print(f"\n=== waiting {args.wait:.0f}s for the box to settle ===")
    deadline = time.time() + args.wait
    while time.time() < deadline:
        if reachable(22):
            print("ssh is up")
            break
        time.sleep(5)

    if reachable(22):
        print("\n=== netdiag.txt over ssh ===")
        print(ssh("cat /dev/shm/netdiag.txt") or "(empty)")
        print("\n=== lsusb ===")
        print(ssh("lsusb") or "(empty)")
        return 0

    print("\nssh did not come up; read the card with:")
    print(f"  {sys.executable} {Path(__file__).name} --from-card /dev/diskN")
    return 1


if __name__ == "__main__":
    sys.exit(main())
