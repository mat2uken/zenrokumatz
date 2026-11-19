#!/usr/bin/env python3
"""Check a X96Max_P unit against the DT295 recording acceptance stages.

Run from the work machine. Each stage prints PASS/FAIL with the evidence it
used, so "equivalent to the second unit" is a measured claim, not an opinion.

  check-unit3-acceptance.py --host 192.168.99.56 --channel 17 --stage receipt
  check-unit3-acceptance.py --host 192.168.99.56 --stage stream

Stages: boot, receipt, stream, nas
"""
import argparse
import json
import subprocess
import sys
import time
import urllib.error
import urllib.request

NAS = "http://192.168.99.80:40776"
SERVICE_SBS = "35864"


def ssh(host, command, timeout=30):
    result = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "root@" + host, command],
        capture_output=True, text=True, timeout=timeout)
    return result.returncode, result.stdout, result.stderr


def http_json(url, timeout=8):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            return json.load(response)
    except (urllib.error.URLError, OSError, ValueError):
        return None


def report(name, ok, evidence):
    print("%-6s %-28s %s" % ("PASS" if ok else "FAIL", name, evidence))
    return ok


def stage_boot(host):
    ok = True
    rc, out, err = ssh(host, "cat /proc/cmdline")
    ok &= report("boot:cmdline", rc == 0 and "rdinit=/init" in out, out.strip() or err.strip())
    rc, out, _ = ssh(host, "mount | sed -n 's/.* on \\/ \\(.*\\).*/\\1/p'")
    ok &= report("boot:root-is-ram", "rootfs" in out, out.strip())
    rc, out, _ = ssh(host, "cat /proc/swaps | wc -l")
    ok &= report("boot:no-swap", out.strip() == "1", "swaps lines=%s" % out.strip())
    rc, out, _ = ssh(host, "dmesg | grep -c 'Unable to handle kernel' || true")
    ok &= report("boot:no-kernel-fault", out.strip() == "0", "fault count=%s" % out.strip())
    rc, out, _ = ssh(host, "ip -o link show eth0 | head -1")
    ok &= report("boot:eth0-carrier", "LOWER_UP" in out, out.strip()[:80])
    rc, out, _ = ssh(host, "ip -o -4 addr show eth0")
    ok &= report("boot:has-address", "inet " in out, out.strip()[:80])
    return ok


def stage_receipt(host, system, channel, extra=""):
    ok = True
    rc, out, _ = ssh(host, "lsusb | grep -c 'Pixela Corp. PIX-DT295' || true")
    ok &= report("receipt:dt295-present", out.strip() == "1", "matches=%s" % out.strip())
    # The DT295 has one tuner: a standalone capture only works when the live
    # pipeline is not using it. Report that state instead of blaming the tuner.
    rc, out, _ = ssh(host, "ps | grep -c '[p]ix-ts-legacy' || true")
    busy = out.strip() not in ("", "0")
    report("receipt:tuner-idle", not busy,
           "pix-ts-legacy running=%s (stop the service before a standalone capture)"
           % out.strip())
    command = ("pix-ts-legacy --model dt295 --assets /usr/share/pix-tuner "
               "-s %s -c %d %s -t 5 -o /tmp/accept.ts 2>/tmp/accept.err; echo rc=$?"
               % (system, channel, extra))
    rc, out, _ = ssh(host, command, timeout=90)
    ok &= report("receipt:capture-rc", "rc=0" in out, out.strip()[-40:])
    rc, out, _ = ssh(host, "grep -o '\"locked\":[01]' /tmp/accept.err | tail -1")
    ok &= report("receipt:locked", "1" in out, out.strip())
    rc, out, _ = ssh(host, "grep -o '\"tables_ready\":[01]' /tmp/accept.err | tail -1")
    ok &= report("receipt:tables-ready", "1" in out, out.strip())
    rc, out, _ = ssh(host, "eit_watch --state /tmp/accept-eit.json < /tmp/accept.ts >/dev/null 2>&1; "
                          "grep -o 'service=[0-9]*' /tmp/accept-eit.json | head -1")
    ok &= report("receipt:service-sbs", SERVICE_SBS in out, out.strip())
    return ok


def stage_stream(host):
    ok = True
    first = http_json("http://%s:40774/v1/status" % host)
    ok &= report("stream:status", bool(first), json.dumps(first) if first else "unreachable")
    if not first:
        return False
    ok &= report("stream:capturing", first.get("capturing") is True, str(first.get("capturing")))
    time.sleep(30)
    second = http_json("http://%s:40774/v1/status" % host)
    if not second:
        return report("stream:still-up", False, "unreachable after 30s")
    grew = second.get("totalPushed", 0) > first.get("totalPushed", 0)
    ok &= report("stream:pushed-grows", grew,
                 "%s -> %s" % (first.get("totalPushed"), second.get("totalPushed")))
    ok &= report("stream:no-drop", second.get("totalDropped") == 0,
                 "dropped=%s" % second.get("totalDropped"))
    return ok


def stage_nas(host, channel):
    nodes = http_json(NAS + "/api/nodes")
    if nodes is None:
        return report("nas:reachable", False, "NAS unreachable")
    ok = report("nas:reachable", True, "%d nodes" % len(nodes))
    stale = [n for n in nodes if n.get("host") == host]
    mine = [n for n in stale if str(n.get("channel")) == str(channel)]
    ok &= report("nas:node-registered", bool(mine),
                 "%s" % [(n.get("id"), n.get("channelName"), n.get("state")) for n in mine])
    others = [n for n in stale if str(n.get("channel")) != str(channel)]
    if others:
        ok &= report("nas:no-stale-node", False,
                     "same IP under other channels: %s" % [n.get("id") for n in others])
    if mine:
        ok &= report("nas:recording", any(n.get("state") == "recording" for n in mine),
                     str([n.get("state") for n in mine]))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--channel", type=int, default=17)
    ap.add_argument("--system", default="terrestrial", choices=("terrestrial", "bs", "cs"))
    ap.add_argument("--slot", default="", help="satellite slot for bs/cs, e.g. 0")
    ap.add_argument("--stage", choices=("boot", "receipt", "stream", "nas", "all"), default="all")
    args = ap.parse_args()
    stages = {
        "boot": lambda: stage_boot(args.host),
        "receipt": lambda: stage_receipt(
            args.host, args.system, args.channel,
            ("--slot %s" % args.slot) if args.slot else ""),
        "stream": lambda: stage_stream(args.host),
        "nas": lambda: stage_nas(args.host, args.channel),
    }
    wanted = stages if args.stage == "all" else {args.stage: stages[args.stage]}
    results = {name: check() for name, check in wanted.items()}
    print("\n%s" % ("ALL SELECTED STAGES PASS" if all(results.values()) else "SOME STAGES FAILED"))
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
