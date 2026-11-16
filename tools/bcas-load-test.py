#!/usr/bin/env python3
"""Measure a B-CAS card server under concurrent ECM load.

Reads the ECM records that ecm_dump produced from a real broadcast and replays
them from several simultaneous clients, which is the same thing cardrelay plus
torne-b25 do on each STB. The point is to find out how many streams one card
server can serve and where the latency starts to grow.

  python3 tools/bcas-load-test.py --host 192.168.99.10 --port 40790 \
      --token-file /tmp/tok.txt --ecm /tmp/ecm.bin --clients 8 \
      --interval 1.9 --duration 60

Frame layout (little endian): uint32 operation, uint32 size, int32 status,
uint8 data[512]. HELLO is operation 0 with data[0]=1 and the 32 byte token.
RESET is operation 1, TRANSMIT is operation 2 with the APDU in data.
"""
import argparse
import binascii
import socket
import statistics
import struct
import sys
import threading
import time

FRAME = 524


def make_frame(op, data=b""):
    if len(data) > 512:
        raise ValueError("payload too large")
    return struct.pack("<IIi", op, len(data), 0) + data + bytes(512 - len(data))


def recv_frame(sock):
    buf = b""
    while len(buf) < FRAME:
        chunk = sock.recv(FRAME - len(buf))
        if not chunk:
            return None
        buf += chunk
    op, size, status = struct.unpack("<IIi", buf[:12])
    return op, size, status, buf[12:]


def load_ecm(path):
    data = open(path, "rb").read()
    out, off = [], 0
    while off + 2 <= len(data):
        n = (data[off] << 8) | data[off + 1]
        off += 2
        if off + n > len(data):
            break
        out.append(data[off:off + n])
        off += n
    return out


def ecm_apdu(section):
    """90 34 00 00 <len> <body> 00, as arib25 sends it.

    arib25 hands the card the section body only: the eight byte long form
    section header and the trailing CRC are not part of the APDU.
    """
    if len(section) < 12:
        raise ValueError("ECM section too short")
    # The eight byte long form section header is not part of the APDU; the
    # CRC is. This is what a real card accepts as "purchased" (0x0800).
    body = section[8:]
    return b"\x90\x34\x00\x00" + bytes([len(body)]) + body + b"\x00"


class Client(threading.Thread):
    def __init__(self, index, host, port, token, ecms, interval, duration,
                 stagger=0.0):
        super().__init__(daemon=True)
        self.index = index
        self.host, self.port = host, port
        self.token = token
        self.ecms = ecms
        self.interval = interval
        self.duration = duration
        self.stagger = stagger
        self.latencies = []
        self.return_codes = {}
        self.errors = 0
        self.connected = False

    def run(self):
        try:
            sock = socket.create_connection((self.host, self.port), 5)
        except OSError:
            self.errors += 1
            return
        sock.settimeout(10)
        try:
            sock.sendall(make_frame(0, b"\x01" + self.token))
            reply = recv_frame(sock)
            if not reply or reply[2] != 0:
                self.errors += 1
                return
            self.connected = True
            for op, payload in ((1, b""), (2, b"\x90\x30")):
                sock.sendall(make_frame(op, payload))
                if not recv_frame(sock):
                    self.errors += 1
                    return

            if self.stagger > 0:
                # Real receivers do not fire their requests at the same instant;
                # without this the clients pile up and the latency shows the
                # pile-up rather than the server's steady state.
                time.sleep(self.stagger)
            deadline = time.monotonic() + self.duration
            i = 0
            while time.monotonic() < deadline:
                section = self.ecms[i % len(self.ecms)]
                i += 1
                started = time.monotonic()
                sock.sendall(make_frame(2, ecm_apdu(section)))
                reply = recv_frame(sock)
                elapsed = (time.monotonic() - started) * 1000.0
                if not reply:
                    self.errors += 1
                    break
                op, size, status, data = reply
                if status != 0 or len(data) < 6:
                    self.errors += 1
                else:
                    self.latencies.append(elapsed)
                    code = (data[4] << 8) | data[5]
                    self.return_codes[code] = self.return_codes.get(code, 0) + 1
                time.sleep(max(0.0, self.interval - (time.monotonic() - started)))
        finally:
            sock.close()


def summarise(latencies):
    if not latencies:
        return "no samples"
    latencies = sorted(latencies)
    def pct(p):
        idx = min(len(latencies) - 1, int(len(latencies) * p))
        return latencies[idx]
    return ("n=%d p50=%.1f p95=%.1f max=%.1f mean=%.1f ms" % (
        len(latencies), pct(0.50), pct(0.95), latencies[-1],
        statistics.fmean(latencies)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.99.10")
    ap.add_argument("--port", type=int, default=40790)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--ecm", required=True)
    ap.add_argument("--clients", type=int, default=8)
    ap.add_argument("--interval", type=float, default=1.9,
                    help="seconds between requests per client")
    ap.add_argument("--duration", type=float, default=60)
    ap.add_argument("--only-purchased", type=int, default=0,
                    help="keep only ECM sections of this byte length")
    ap.add_argument("--json", help="write this run's numbers here and stay quiet")
    ap.add_argument("--offset", type=int, default=0,
                    help="shift the ECM share so parallel runs do not collide")
    ap.add_argument("--stagger", type=float, default=-1.0,
                    help="seconds to wait before the first request "
                         "(default: spread clients inside the interval)")
    args = ap.parse_args()

    token = binascii.unhexlify(open(args.token_file).read().strip())
    ecms = load_ecm(args.ecm)
    if args.only_purchased:
        # A B-CAS card answers 0x0800 for a service it can unlock; anything
        # else is not worth replaying.
        ecms = [e for e in ecms if len(e) == args.only_purchased]
    if not ecms:
        print("no ECM records in", args.ecm)
        return 1

    # Give each client its own share so the server cannot answer from cache.
    per = max(1, len(ecms) // args.clients)
    clients = []
    for i in range(args.clients):
        start = (args.offset + i * per) % max(1, len(ecms))
        share = [ecms[(start + k) % len(ecms)] for k in range(per)]
        clients.append(Client(i, args.host, args.port, token, share,
                              args.interval, args.duration,
                              stagger=(args.stagger if args.stagger >= 0
                                       else (args.interval / args.clients) * i)))

    rate = args.clients / args.interval
    if not args.json:
        print("%d clients, %.2f s apart each -> about %.1f ECM requests/s for %.0f s"
              % (args.clients, args.interval, rate, args.duration))

    started = time.monotonic()
    for c in clients:
        c.start()
    for c in clients:
        c.join()
    wall = time.monotonic() - started

    if args.json:
        import json
        json.dump({
            "connected": sum(1 for c in clients if c.connected),
            "clients": args.clients,
            "ok": sum(len(c.latencies) for c in clients),
            "failed": sum(c.errors for c in clients),
            "latencies_ms": [round(x, 2) for c in clients for x in c.latencies],
            "return_codes": {"0x%04x" % k: v for c in clients
                             for k, v in c.return_codes.items()},
            "wall_s": round(wall, 2),
        }, open(args.json, "w"))
        return 0

    total = sum(len(c.latencies) for c in clients)
    failed = sum(c.errors for c in clients)
    connected = sum(1 for c in clients if c.connected)
    all_lat = [x for c in clients for x in c.latencies]
    codes = {}
    for c in clients:
        for k, v in c.return_codes.items():
            codes[k] = codes.get(k, 0) + v

    print("clients connected: %d/%d" % (connected, args.clients))
    print("requests: %d ok, %d failed in %.1f s (%.1f req/s)"
          % (total, failed, wall, total / wall if wall else 0))
    print("latency: %s" % summarise(all_lat))
    if codes:
        print("card return codes: %s" % {
            "0x%04x" % k: v for k, v in sorted(codes.items())})
    for c in clients:
        print("  client %d: %s%s" % (c.index, summarise(c.latencies),
                                     "" if c.connected else " (not connected)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
