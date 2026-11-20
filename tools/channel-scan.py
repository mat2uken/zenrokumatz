#!/usr/bin/env python3
"""Identify which station a physical channel really carries.

Unit 3 is configured for PIX_CHANNEL=17, described as SBS. The NAS labels the
recordings from that channel テレビ静岡, and on the 日本平 transmitter SBS is
physical 15 while テレビ静岡 is 17 (NHK静岡's published table). The service ID
recorded earlier therefore identifies the station but the channel number in the
config may be the wrong one.

This captures a few seconds from a channel and reads the service name out of the
SDT with ffprobe, which is the ground truth.

  channel-scan.py --run --channel 15 --channel 17
  channel-scan.py --read /dev/disk10
"""
import argparse
import subprocess
import sys
from pathlib import Path

HOST = "192.168.99.56"
SSH = ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no",
       "-o", "ConnectTimeout=8", f"root@{HOST}"]

# One channel per capture, each written to the card so a wedge keeps the ones
# that already finished. --channel 0 is not used; pix-ts takes 13..62.
PROBE = r'''
set -u
LOG=/mnt/sd/channel-scan.log
mkdir -p /mnt/sd
mount -t vfat /dev/mmcblk0p1 /mnt/sd 2>/dev/null || true
: > "$LOG"
echo "=== channel-scan $(date) ===" >> "$LOG"
sync

for ch in __CHANNELS__; do
    echo "--- physical channel $ch ---" >> "$LOG"
    sync
    timeout 25 pix-ts-legacy --model dt295 -s terrestrial -c "$ch" \
        -a /usr/share/pix-tuner -t 8 > "/mnt/sd/ch$ch.ts" 2>"/mnt/sd/ch$ch.err"
    echo "capture exit: $? ($(ls -l "/mnt/sd/ch$ch.ts" 2>/dev/null | awk '{print $5}') bytes)" >> "$LOG"
    ffprobe -v error -f mpegts -show_entries program=program_id:program_tags=service_name \
        -of default=noprint_wrappers=1 "/mnt/sd/ch$ch.ts" >> "$LOG" 2>&1
    sync
    rm -f "/mnt/sd/ch$ch.ts"
done

echo "=== done ===" >> "$LOG"
echo "scan finished"
'''

READ = r'''
mount -t vfat /dev/mmcblk0p1 /mnt/sd 2>/dev/null || true
cat /mnt/sd/channel-scan.log 2>/dev/null || echo "(no channel-scan.log)"
'''


def ssh(script, timeout=400):
    return subprocess.run(SSH + [script], capture_output=True, text=True, timeout=timeout)


def read_card(device):
    subprocess.run(["diskutil", "mount", device + "s1"], capture_output=True)
    info = subprocess.run(["diskutil", "info", "-plist", device + "s1"],
                          capture_output=True, text=True)
    import plistlib
    volume = plistlib.loads(info.stdout.encode()).get("MountPoint")
    if not volume:
        raise SystemExit("the card's FAT volume did not mount")
    log = Path(volume) / "channel-scan.log"
    if not log.is_file():
        print(f"no channel-scan.log on {volume}")
        return 1
    print(log.read_text(errors="replace"))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", action="store_true")
    ap.add_argument("--channel", type=int, action="append", default=[],
                    help="physical channel to capture (repeatable)")
    ap.add_argument("--read", metavar="DEVICE")
    args = ap.parse_args()

    if args.read:
        return read_card(args.read)
    if not args.run:
        ap.error("give --run or --read DEVICE")
    channels = args.channel or [15, 17]

    # pix-stream must not be running: it holds the tuner and wedges the box.
    result = ssh(PROBE.replace("__CHANNELS__", " ".join(str(c) for c in channels)))
    print(result.stdout.strip() or result.stderr.strip())
    print("\nRead the result on the box with `cat /mnt/sd/channel-scan.log`,")
    print("or after a power cut:")
    print(f"  {sys.executable} {Path(__file__).name} --read /dev/diskN")
    return 0


if __name__ == "__main__":
    sys.exit(main())
