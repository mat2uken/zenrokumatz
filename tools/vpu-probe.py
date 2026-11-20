#!/usr/bin/env python3
"""Exercise the Amlogic HCodec encoder and leave the evidence on the SD card.

Touching the VPU on this board has wedged the box before (round 156), taking the
console and the network with it, so RAM loggers are useless. Every step is
appended to a file on the card's FAT partition and synced, which survives a
power cycle and can be read on the Mac afterwards.

  vpu-probe.py --run
  vpu-probe.py --read /dev/disk10
"""
import argparse
import subprocess
import sys
from pathlib import Path

HOST = "192.168.99.56"
SSH = ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no",
       "-o", "ConnectTimeout=8", f"root@{HOST}"]

# Each step logs what it is about to do before doing it, so a hang points at the
# exact command. The encoder is fed from a generated MPEG-2 clip rather than the
# tuner, so a wedged USB stack cannot be confused with a wedged VPU.
PROBE = r'''
set -u
LOG=/mnt/sd/vpu-probe.log
mkdir -p /mnt/sd
mount -t vfat /dev/mmcblk0p1 /mnt/sd 2>/dev/null || true

log() { echo "$*" >> "$LOG"; sync; }

: > "$LOG"
log "=== vpu-probe $(date) ==="

log "--- modules ---"
lsmod 2>/dev/null | grep -E "amvenc|meson_vdec|meson_canvas" >> "$LOG"
log "--- video devices ---"
for v in /sys/class/video4linux/video*; do
    [ -e "$v/name" ] || continue
    log "$(basename "$v"): $(cat "$v/name")"
done
log "--- dmesg (amvenc/canvas/vdec) ---"
dmesg | grep -iE "amvenc|canvas|meson_vdec|hcodec" | tail -25 >> "$LOG"

log "--- generating a 2s MPEG-2 test clip ---"
ffmpeg -hide_banner -v error -f lavfi -i testsrc2=size=320x180:rate=30:duration=2 \
    -c:v mpeg2video -b:v 3000k -f mpegts /tmp/vpu-in.ts >> "$LOG" 2>&1
log "clip: $(ls -l /tmp/vpu-in.ts 2>/dev/null | awk '{print $5}') bytes"

log "--- listing h264 encoders ---"
ffmpeg -hide_banner -encoders 2>/dev/null | grep -i h264 >> "$LOG"

log "--- encoding with h264_v4l2m2m (this is where it may die) ---"
timeout 30 ffmpeg -hide_banner -nostdin -v info -f mpegts -i /tmp/vpu-in.ts \
    -vf format=nv12 -c:v h264_v4l2m2m -b:v 2000k -g 30 \
    -f mpegts /tmp/vpu-out.ts >> "$LOG" 2>&1
log "encode exit: $?"
log "output: $(ls -l /tmp/vpu-out.ts 2>/dev/null | awk '{print $5}') bytes"

log "--- encoder still alive? ---"
lsmod 2>/dev/null | grep amvenc >> "$LOG"
log "=== done ==="
echo "probe finished"
'''

READ = r'''
mount -t vfat /dev/mmcblk0p1 /mnt/sd 2>/dev/null || true
cat /mnt/sd/vpu-probe.log 2>/dev/null || echo "(no vpu-probe.log)"
'''


def ssh(script, timeout=180):
    return subprocess.run(SSH + [script], capture_output=True, text=True, timeout=timeout)


def read_card(device):
    subprocess.run(["diskutil", "mount", device + "s1"], capture_output=True)
    info = subprocess.run(["diskutil", "info", "-plist", device + "s1"],
                          capture_output=True, text=True)
    import plistlib
    volume = plistlib.loads(info.stdout.encode()).get("MountPoint")
    if not volume:
        raise SystemExit("the card's FAT volume did not mount")
    log = Path(volume) / "vpu-probe.log"
    if not log.is_file():
        print(f"no vpu-probe.log on {volume}")
        return 1
    text = log.read_text(errors="replace")
    print(text)
    lines = [line for line in text.splitlines() if line.strip()]
    if lines:
        print("\n=== last line before the hang ===")
        print("  " + lines[-1][:160])
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", action="store_true", help="run the probe on the box")
    ap.add_argument("--read", metavar="DEVICE", help="read the log off this card")
    args = ap.parse_args()

    if args.read:
        return read_card(args.read)
    if not args.run:
        ap.error("give --run or --read DEVICE")

    result = ssh(PROBE)
    print(result.stdout.strip() or result.stderr.strip())
    print("\nIf the box stopped answering, power it off and run:")
    print(f"  {sys.executable} {Path(__file__).name} --read /dev/diskN")
    return 0


if __name__ == "__main__":
    sys.exit(main())
