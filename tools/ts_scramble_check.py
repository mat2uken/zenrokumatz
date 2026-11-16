#!/usr/bin/env python3
"""Report how many TS packets are scrambled and list the busiest PIDs."""
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/live.ts"
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 3000

with open(path, "rb") as f:
    data = f.read(188 * limit)

total = scrambled = 0
pids = {}
for i in range(0, len(data) - 187, 188):
    if data[i] != 0x47:
        continue
    total += 1
    pid = ((data[i + 1] & 0x1F) << 8) | data[i + 2]
    tsc = (data[i + 3] >> 6) & 0x3
    pids[pid] = pids.get(pid, 0) + 1
    if tsc != 0:
        scrambled += 1

pct = 100.0 * scrambled / total if total else 0.0
print("packets=%d scrambled=%d (%.1f%%)" % (total, scrambled, pct))
print("top PIDs:", sorted(pids.items(), key=lambda kv: -kv[1])[:8])
