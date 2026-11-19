#!/usr/bin/env python3
"""Observe exactly what U-Boot's loady answers, byte for byte.

The batch YMODEM in U-Boot does not match the textbook sequence the first
attempt assumed, so instead of guessing, this records every byte the receiver
sends between our blocks. Use it to pin down the ACK/'C' pattern, then encode
that pattern in loady_send.py.

  probe_ymodem.py --dev /dev/cu.usbserial-01025FC4 --file X.dtb --address 0x08000000
"""
import argparse
import os
import sys
import termios
import time

import importlib.util


def load_sender():
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location("loady", os.path.join(here, "loady_send.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def drain(fd, seconds):
    """Collect every byte arriving within the window, with timestamps."""
    end = time.time() + seconds
    seen = bytearray()
    marks = []
    while time.time() < end:
        import select
        ready, _, _ = select.select([fd], [], [], 0.05)
        if not ready:
            continue
        try:
            data = os.read(fd, 4096)
        except OSError:
            continue
        if data:
            seen.extend(data)
            marks.append((round(time.time() - end + seconds, 3), data[:16]))
    return bytes(seen), marks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dev", required=True)
    ap.add_argument("--file", required=True)
    ap.add_argument("--address", default="0x08000000")
    ap.add_argument("--blocks", type=int, default=3)
    args = ap.parse_args()

    sender = load_sender()
    data = open(args.file, "rb").read()
    fd = sender.open_port(args.dev, 115200)
    try:
        os.write(fd, bytes([0x1b]))
        time.sleep(0.3)
        os.write(fd, b"\r")
        got, _ = drain(fd, 1.5)
        print("prompt tail: %r" % got[-80:])
        if b"#" not in got:
            print("not at a U-Boot prompt; aborting")
            return 1

        os.write(fd, ("loady %s\r" % args.address).encode())
        got, marks = drain(fd, 4.0)
        print("banner: %r" % got[-160:])
        print("banner marks: %r" % marks[:12])

        header = ("%s\x00%d 0 0\x00" % (os.path.basename(args.file), len(data))).encode()
        header += b"\x00" * (128 - len(header))
        os.write(fd, sender.make_packet(0, header))
        got, marks = drain(fd, 3.0)
        print("after header: %r marks=%r" % (got[:40], marks[:8]))

        for seq in range(1, args.blocks + 1):
            sent = (seq - 1) * 1024
            chunk = data[sent:sent + 1024]
            if len(chunk) < 1024:
                chunk += b"\x1a" * (1024 - len(chunk))
            os.write(fd, sender.make_packet(seq, chunk))
            got, marks = drain(fd, 3.0)
            print("after block %d: %r marks=%r" % (seq, got[:40], marks[:8]))
        return 0
    finally:
        try:
            sender.set_baud(fd, 115200)
        except Exception:
            pass
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
