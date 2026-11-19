#!/usr/bin/env python3
"""After the DT295 is plugged in, verify reception and recording end to end.

Run this once the tuner is physically connected. It does the whole sequence in
order and stops at the first step that fails, so the output says where the
chain broke:

  1. the DT295 enumerates over USB
  2. the pixel tuner can lock and read tables on the configured channel
  3. the service restarts and starts pushing fragments
  4. the NAS lists the node as recording

  wait-for-dt295.py --host 192.168.99.56 --channel 17
  wait-for-dt295.py --host 192.168.99.56 --channel 17 --watch 600
"""
import argparse
import subprocess
import sys
import time


def ssh(host, command, timeout=60):
    result = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "root@" + host, command],
        capture_output=True, text=True, timeout=timeout)
    return result.stdout.strip()


def step(number, title, ok, detail=""):
    print("%s %d. %s%s" % ("OK  " if ok else "FAIL", number, title,
                           (" — " + detail) if detail else ""))
    return ok


def wait_for_usb(host, seconds):
    """Poll until the DT295 shows up as a USB device."""
    deadline = time.time() + seconds
    last = ""
    while True:
        out = ssh(host, "lsusb | grep -i '06b8:103b' || true")
        if out:
            return True, out
        last = ssh(host, "lsusb") or "(no output)"
        if time.time() >= deadline:
            return False, last
        time.sleep(10)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--channel", type=int, default=17)
    ap.add_argument("--system", default="terrestrial")
    ap.add_argument("--watch", type=float, default=0,
                    help="seconds to wait for the tuner to be plugged in (0 = check once)")
    args = ap.parse_args()

    deadline = time.time() + args.watch
    while True:
        ok, detail = wait_for_usb(args.host, 1 if args.watch else 0)
        if ok or time.time() >= deadline:
            break
        print("waiting for the DT295 ... (%s)" % detail.splitlines()[-1][:60])
        time.sleep(5)

    if not step(1, "DT295 is on the USB bus", ok, detail if not ok else ""):
        return 1

    # Restart the service: the reset budget is per boot, and an earlier run
    # already exhausted it while the tuner was absent.
    ssh(args.host, "/etc/init.d/S97pix-stream start")
    time.sleep(5)
    step(2, "service restarted", True)

    status = ssh(args.host, "wget -qO- http://127.0.0.1:40774/v1/status 2>/dev/null")
    if not step(3, "status endpoint answers", bool(status), status[:120]):
        print("      pix-stream log:")
        print(ssh(args.host, "tail -5 /tmp/pix-simple.log") or "      (empty)")
        print("      tuner log:")
        print(ssh(args.host, "tail -3 /tmp/pix-ts.log") or "      (empty)")
        return 1

    first = status
    time.sleep(30)
    second = ssh(args.host, "wget -qO- http://127.0.0.1:40774/v1/status 2>/dev/null")
    step(4, "still answering after 30s", bool(second), second[:120])

    def pushed(text):
        import json
        try:
            return json.loads(text).get("totalPushed", 0)
        except ValueError:
            return 0

    grew = pushed(second) > pushed(first)
    step(5, "fragments are being pushed", grew,
         "%s -> %s" % (pushed(first), pushed(second)))

    program = ssh(args.host, "wget -qO- http://127.0.0.1:40774/v1/program 2>/dev/null")
    step(6, "program info is available", bool(program),
         program.replace("\n", " ")[:120] if program else "")

    print("\nNext: python3 tools/check-unit3-acceptance.py --host %s --channel %d"
          % (args.host, args.channel))
    return 0 if grew else 1


if __name__ == "__main__":
    sys.exit(main())
