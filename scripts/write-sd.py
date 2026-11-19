#!/usr/bin/env python3
"""Write a generated image to an explicitly selected external macOS disk."""
import argparse
import hashlib
import plistlib
import re
import struct
import subprocess
import sys
from pathlib import Path


def check_disk(info, device, image_size):
    if not re.fullmatch(r"/dev/disk[0-9]+", device):
        raise ValueError("Specify a whole disk, for example /dev/disk4.")
    if info.get("DeviceNode") != device or info.get("WholeDisk") is not True:
        raise ValueError("The selected device is not a whole disk.")
    if info.get("Internal") is not False:
        raise ValueError("Internal disks are not accepted.")
    if info.get("VirtualOrPhysical") != "Physical":
        raise ValueError("Select a physical SD card or USB card reader.")
    if info.get("Writable") is not True:
        raise ValueError("The selected disk is not writable.")
    if info.get("TotalSize", 0) < image_size:
        raise ValueError("The image is larger than the selected disk.")


def hash_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(4 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def boot_partition(image):
    """Return the FAT range for this project's SD layouts.

    Two layouts are written by this repo: the disk build (FAT boot + ext4 root)
    and the RAM-root build (a single FAT partition holding kernel, DTB and
    initramfs). macOS mounts the FAT volume after a write and drops
    .fseventsd/.Spotlight-V100 into it, so read-back has to treat that range as
    "filesystem-managed" for either layout. Returning None here means the
    caller may not excuse any difference at all.
    """
    with image.open("rb") as source:
        mbr = source.read(512)
    if len(mbr) != 512 or mbr[510:] != b"\x55\xaa":
        return None
    start, length = struct.unpack_from("<II", mbr, 454)
    if start < 1 or length == 0 or mbr[450] != 0x0c:
        return None
    end = (start + length) * 512
    if any(mbr[478:510]):
        return None
    # RAM-root layout: exactly one FAT partition covering the image.
    if all(byte == 0 for byte in mbr[466:478]):
        return (start * 512, end) if end == image.stat().st_size else None
    root_start, root_length = struct.unpack_from("<II", mbr, 470)
    if (mbr[466] != 0x83
            or root_start != start + length or root_length == 0
            or (root_start + root_length) * 512 != image.stat().st_size
            or any(mbr[478:510])):
        return None
    return start * 512, (start + length) * 512


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, default=Path(__file__).resolve().parents[1] / "output/sdcard.img")
    parser.add_argument("--device", required=True, help="External whole disk from diskutil list external physical")
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--erase", action="store_true", help="Erase this disk, write the image, and verify it")
    action.add_argument("--verify-only", action="store_true", help="Verify an already written disk without rewriting it")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("Run this command on macOS.")
    if not re.fullmatch(r"/dev/disk[0-9]+", args.device):
        parser.error("Use /dev/diskN, not a partition or /dev/rdiskN.")
    image = args.image.resolve(strict=True)
    size = image.stat().st_size
    if size == 0:
        parser.error("The image is empty.")
    info = plistlib.loads(subprocess.check_output(["diskutil", "info", "-plist", args.device]))
    check_disk(info, args.device, size)
    expected = hash_file(image)
    manifest = {name: digest for digest, name in
                (line.split() for line in image.with_name("SHA256SUMS").read_text().splitlines())}
    if manifest.get(image.name) != expected:
        raise ValueError("Image SHA-256 does not match SHA256SUMS.")
    print(f"Image: {image} ({size} bytes)")
    print(f"Target: {args.device}, {info.get('MediaName')}, {info['TotalSize']} bytes", flush=True)
    if not args.erase and not args.verify_only:
        print("Dry run only. Add --erase to erase this disk and write the image.")
        return
    raw = args.device.replace("/dev/disk", "/dev/rdisk", 1)
    if args.erase:
        subprocess.run(["diskutil", "unmountDisk", args.device], check=True)
        subprocess.run(["sudo", "/bin/dd", f"if={image}", f"of={raw}", "bs=4m"], check=True)
        subprocess.run(["sync"], check=True)
    # macOS can mount the newly written FAT volume and add its metadata.
    subprocess.run(["diskutil", "unmountDisk", args.device], check=True)
    fat_range = boot_partition(image)
    count = (size + 4 * 1024 * 1024 - 1) // (4 * 1024 * 1024)
    reader = subprocess.Popen(["sudo", "/bin/dd", f"if={raw}", "bs=4m", f"count={count}"], stdout=subprocess.PIPE)
    digest = hashlib.sha256()
    remaining = size
    outside_fat_matches = True
    with reader.stdout, image.open("rb") as source:
        while remaining:
            chunk = reader.stdout.read(min(4 * 1024 * 1024, remaining))
            if not chunk:
                break
            reference = source.read(len(chunk))
            if fat_range is None:
                outside_fat_matches &= chunk == reference
            else:
                position = size - remaining
                prefix = max(0, min(len(chunk), fat_range[0] - position))
                suffix = max(0, min(len(chunk), fat_range[1] - position))
                outside_fat_matches &= (chunk[:prefix] == reference[:prefix]
                                        and chunk[suffix:] == reference[suffix:])
            digest.update(chunk)
            remaining -= len(chunk)
        # dd rounds up to a full block; do not hash bytes after the image.
        while reader.stdout.read(4 * 1024 * 1024):
            pass
    if reader.wait() or remaining:
        raise RuntimeError("SD read-back verification failed.")
    actual = digest.hexdigest()
    print(f"Read-back SHA-256: {actual}", flush=True)
    if actual != expected:
        if fat_range is None or not outside_fat_matches:
            raise RuntimeError("SD read-back verification failed: data outside the FAT partition differs.")
        # Do not ignore FAT differences unless the filesystem and every shipped
        # boot file pass. The RAM-root image carries Image, several DTBs and the
        # initramfs, so verify everything the manifest names rather than a fixed
        # pair that only matches the disk build.
        boot_files = tuple(sorted(name for name in manifest
                                  if name not in (image.name, "rootfs.ext4", "ramboot.img")))
        if not boot_files:
            raise ValueError("Boot file hashes are missing from SHA256SUMS.")
        if not ({"Image"} <= set(boot_files)):
            raise ValueError("SHA256SUMS names no boot kernel; refusing to accept FAT differences.")
        subprocess.run(["sudo", "/sbin/fsck_msdos", "-n", raw + "s1"], check=True)
        subprocess.run(["diskutil", "mount", "readOnly", "nobrowse", args.device + "s1"], check=True)
        try:
            volume = plistlib.loads(subprocess.check_output(["diskutil", "info", "-plist", args.device + "s1"]))
            mount = volume.get("MountPoint")
            if not mount:
                raise RuntimeError("The read-only boot volume was not mounted.")
            for name in boot_files:
                if hash_file(Path(mount) / name) != manifest[name]:
                    raise RuntimeError(f"SD boot file verification failed: {name}")
                print(f"Verified boot file: {name} SHA-256={manifest[name]}")
        finally:
            subprocess.run(["diskutil", "unmountDisk", args.device], check=True)
        print("Verified: MBR and rootfs match byte-for-byte; FAT is valid; boot file hashes match.")
    else:
        print(f"Verified SHA-256: {expected}")
    subprocess.run(["diskutil", "eject", args.device], check=True)
    print("SD verification completed; disk ejected.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
