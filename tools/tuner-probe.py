#!/usr/bin/env python3
"""Run the tuner reader by hand and keep its output on the SD card.

`pix-stream` dies the instant it opens the DT295: the whole box stops answering
on the console and on the network (round 154), so nothing survives in RAM. The
only durable place to leave evidence is the card's FAT partition, which the Mac
can read after the box is powered off.

  tuner-probe.py --run              # start the probe and let it hang
  tuner-probe.py --read /dev/disk10 # read the log off the card afterwards
"""
import argparse
import subprocess
import sys
from pathlib import Path

HOST = "192.168.99.56"
SSH = ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no",
       "-o", "ConnectTimeout=8", f"root@{HOST}"]

# Writes every line to the card as it is produced, so a hang keeps what came
# before it. Runs the capture under nohup so the ssh session can return first.
PROBE = r'''
set -u
LOG=/mnt/sd/tuner-probe.log
mkdir -p /mnt/sd
mount -t vfat /dev/mmcblk0p1 /mnt/sd 2>/dev/null || true
{
    echo "=== tuner-probe $(date) ==="
    echo "--- lsusb ---"
    lsusb 2>&1
    echo "--- dmesg tail ---"
    dmesg | tail -20
    echo "--- running pix-ts-legacy ---"
} >> "$LOG" 2>&1
sync

# Unbuffered, line by line, straight onto the card.
nohup sh -c 'exec pix-ts-legacy --model dt295 -s terrestrial -c 17 \
    -a /usr/share/pix-tuner -t 30 2>&1 | while IFS= read -r line; do
        echo "OUT: $line" >> /mnt/sd/tuner-probe.log
        sync
    done' >/dev/null 2>&1 &
echo "probe started"
'''

READ = r'''
set -u
mount -t vfat /dev/mmcblk0p1 /mnt/sd 2>/dev/null || true
cat /mnt/sd/tuner-probe.log 2>/dev/null || echo "(no tuner-probe.log)"
'''


def ssh(script, timeout=60):
    return subprocess.run(SSH + [script], capture_output=True, text=True, timeout=timeout)


def read_card(device):
    subprocess.run(["diskutil", "mount", device + "s1"], capture_output=True)
    info = subprocess.run(["diskutil", "info", "-plist", device + "s1"],
                          capture_output=True, text=True)
    import plistlib
    volume = plistlib.loads(info.stdout.encode()).get("MountPoint")
    if not volume:
        raise SystemExit("the card's FAT volume did not mount")
    log = Path(volume) / "tuner-probe.log"
    if not log.is_file():
        print(f"no tuner-probe.log on {volume}")
        return 1
    text = log.read_text(errors="replace")
    print(text)
    # The interesting part is where the output stops.
    lines = [line for line in text.splitlines() if line.strip()]
    if lines:
        print("\n=== last line before the hang ===")
        print("  " + lines[-1][:160])
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", action="store_true", help="start the probe on the running box")
    ap.add_argument("--read", metavar="DEVICE", help="read the log off this card")
    args = ap.parse_args()

    if args.read:
        return read_card(args.read)
    if not args.run:
        ap.error("give --run or --read DEVICE")

    result = ssh(PROBE)
    print(result.stdout.strip() or result.stderr.strip())
    print("\nThe box will hang. Power it off, then:")
    print(f"  {sys.executable} {Path(__file__).name} --read /dev/diskN")
    return 0


if __name__ == "__main__":
    sys.exit(main())
