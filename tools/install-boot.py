#!/usr/bin/env python3
"""Install RAM-boot files onto unit 3's SD card and boot them, without sudo.

The SD card cannot be reached from the Mac, and this U-Boot has neither fatwrite
nor working TFTP, so the only ways to replace a file are through a running
Android or a running RAM-boot Linux. Both are supported here:

  Android: adb push to /data/local/tmp, then su -c cp onto /mnt/media_rw/<vol>
  Linux  : base64 in chunks over ssh, then cp onto the mounted FAT partition

After installing, the box is rebooted into U-Boot and booted with the chosen
DTB, and the diagnostic lines are printed.

  install-boot.py --dtb meson-g12a-x96-max-p-gxl.dtb
  install-boot.py --dtb meson-g12a-x96-max-p-gxl.dtb --files Image
  install-boot.py --dtb ... --no-reboot        # install only
"""
import argparse
import base64
import hashlib
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "output/ramboot-unit3"
SERIAL = "/dev/cu.usbserial-01025FC4"
HOST = "192.168.99.56"


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=kw.pop("timeout", 60), **kw)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def reachable(port):
    import socket
    with socket.socket() as s:
        s.settimeout(2)
        return s.connect_ex((HOST, port)) == 0


def adb(*args, timeout=180):
    return run(["adb", "-s", f"{HOST}:5555", *args], timeout=timeout)


def install_android(names):
    """Copy files through a running Android onto its mounted SD volume."""
    run(["adb", "connect", f"{HOST}:5555"], timeout=30)
    volume = adb("shell", "su -c 'ls /mnt/media_rw/'").stdout.strip().splitlines()
    if not volume:
        raise SystemExit("Android is up but no SD volume is mounted under /mnt/media_rw")
    volume = volume[0].strip()
    print(f"android volume: {volume}")
    for name in names:
        local = OUT / name
        remote = f"/data/local/tmp/{name}.new"
        result = adb("push", str(local), remote)
        if result.returncode:
            raise SystemExit(f"push failed for {name}: {result.stderr.strip()}")
        target = f"/mnt/media_rw/{volume}/{name}"
        adb("shell", f"su -c 'cp {remote} {target}; sync'")
        got = adb("shell", f"su -c 'sha256sum {target}'").stdout.split()[0]
        if got != digest(local):
            raise SystemExit(f"{name}: hash mismatch on the card ({got[:16]})")
        adb("shell", f"su -c 'rm -f {remote}'")
        print(f"  {name}: installed and verified ({got[:16]})")


def install_linux(names):
    """Copy files through a running RAM-boot Linux onto its mounted card.

    sshd here has no sftp-server, so scp does not work. Instead the file is
    piped through ssh's stdin as base64 and decoded on the far side: one ssh
    call per file rather than one per chunk, which matters because the kernel
    image alone is 42 MB.
    """
    ssh = ["ssh", "-o", "BatchMode=yes", f"root@{HOST}"]
    mounted = run(ssh + ["mount | grep -c ' /mnt/sd '"]).stdout.strip()
    if mounted == "0":
        run(ssh + ["mkdir -p /mnt/sd && mount -t vfat /dev/mmcblk0p1 /mnt/sd"], timeout=30)
    print("linux mount: /mnt/sd")
    for name in names:
        local = OUT / name
        payload = base64.b64encode(local.read_bytes()).decode()
        remote = (f"base64 -d > /tmp/{name} && cp /tmp/{name} /mnt/sd/{name} && "
                  f"sync && sha256sum /mnt/sd/{name} && rm -f /tmp/{name}")
        result = subprocess.run(ssh + [remote], input=payload, text=True,
                                capture_output=True, timeout=300)
        if result.returncode:
            raise SystemExit(f"{name}: copy failed: {result.stderr.strip()[:200]}")
        got = result.stdout.split()[0] if result.stdout.split() else ""
        if got != digest(local):
            raise SystemExit(f"{name}: hash mismatch on the card ({got[:16]})")
        print(f"  {name}: installed and verified ({got[:16]})")


def boot(dtb, trace_seconds):
    sys.path.insert(0, str(ROOT / "tools"))
    import uboot_boot_linux as uboot

    fd = uboot.open_port(SERIAL)
    log = open(ROOT / "work" / "install-boot.log", "wb", buffering=0)
    console = uboot.Console(fd, log)
    console.tail = ""
    console.pump(1.0)
    prompt = uboot.catch_prompt(console, 200)
    print(f"prompt: {prompt}")
    if not prompt:
        os.close(fd)
        raise SystemExit("could not reach U-Boot")

    # Each load must be confirmed before the next one runs: booting stale memory
    # silently when a fatload failed is worse than stopping here.
    for command, wait, marker in (
        ("mmc dev 0", 6, "card in"),
        ("fatload mmc 0:1 0x01080000 Image", 30, "bytes read"),
        ("fatload mmc 0:1 0x08000000 " + dtb, 10, "bytes read"),
        ("fatload mmc 0:1 0x10000000 rootfs.cpio.uboot", 25, "bytes read"),
        ("setenv bootargs console=ttyAML0,115200n8 earlycon rdinit=/init", 4, ""),
    ):
        console.tail = ""
        os.write(fd, (command + "\r").encode())
        output = console.pump(wait)
        if marker and marker not in output and marker not in console.tail:
            os.close(fd)
            raise SystemExit(f"step failed: {command} (no '{marker}')")
    log.write(b"\n=== BOOTI ===\n")
    console.tail = ""
    os.write(fd, b"booti 0x01080000 0x10000000 0x08000000\r")
    console.pump(trace_seconds)
    os.close(fd)

    text = (ROOT / "work" / "install-boot.log").read_bytes().replace(b"\x00", b"").decode("utf-8", "replace")
    print("\n=== diagnostics ===")
    for line in text.splitlines():
        if "DBG " in line or "USB2 ports" in line or "USB3 ports" in line or "Internal error" in line:
            print("  " + line.strip()[:120])
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dtb", required=True, help="DTB file name inside the FAT partition")
    ap.add_argument("--files", nargs="*", default=["Image", "rootfs.cpio.uboot"],
                    help="files to install (default: Image and rootfs.cpio.uboot)")
    ap.add_argument("--no-reboot", action="store_true")
    ap.add_argument("--trace", type=float, default=60.0)
    args = ap.parse_args()

    names = list(args.files)
    for name in names + [args.dtb]:
        if not (OUT / name).is_file():
            raise SystemExit(f"missing {OUT / name}")

    if reachable(5555):
        print("installing through Android")
        install_android(names)
    elif reachable(22):
        print("installing through RAM-boot Linux")
        install_linux(names)
    else:
        raise SystemExit("neither Android (5555) nor Linux (22) is reachable; "
                         "power the box on and let it boot")

    if args.no_reboot:
        print("installed; not rebooting (--no-reboot)")
        return 0
    boot(args.dtb, args.trace)
    return 0


if __name__ == "__main__":
    sys.exit(main())
