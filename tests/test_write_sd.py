import hashlib
import importlib.util
import io
import plistlib
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts' / 'write-sd.py'
spec = importlib.util.spec_from_file_location('write_sd', SCRIPT)
write_sd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(write_sd)


class WriteSdTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.image = Path(self.temp.name) / 'sdcard.img'
        self.data = b'linux sd image content'
        self.image.write_bytes(self.data)
        self.digest = hashlib.sha256(self.data).hexdigest()
        (self.image.parent / 'SHA256SUMS').write_text(f'{self.digest}  {self.image.name}\n')
        self.disk = {'DeviceNode': '/dev/disk9', 'WholeDisk': True, 'Internal': False,
                     'VirtualOrPhysical': 'Physical', 'Writable': True, 'TotalSize': 1024,
                     'MediaName': 'Test card'}

    def run_main(self, *flags, readback=None, reader_exit=0, fsck_error=False):
        argv = ['write-sd.py', '--image', str(self.image), '--device', '/dev/disk9', *flags]
        reader = Mock()
        reader.stdout = io.BytesIO(self.data if readback is None else readback)
        reader.wait.return_value = reader_exit
        def disk_info(command):
            if command[-1] == '/dev/disk9s1':
                return plistlib.dumps({'MountPoint': str(self.image.parent / 'mount')})
            return plistlib.dumps(self.disk)

        def run_command(command, **kwargs):
            if fsck_error and command[1:3] == ['/sbin/fsck_msdos', '-n']:
                raise subprocess.CalledProcessError(1, command)

        with patch.object(sys, 'platform', 'darwin'), patch.object(sys, 'argv', argv), \
             patch.object(write_sd.subprocess, 'check_output', side_effect=disk_info) as info, \
             patch.object(write_sd.subprocess, 'run', side_effect=run_command) as run, \
             patch.object(write_sd.subprocess, 'Popen', return_value=reader) as popen, \
             patch('sys.stdout', new_callable=io.StringIO):
            def wait_and_check_eject():
                self.assertNotIn(['diskutil', 'eject', '/dev/disk9'],
                                 [call.args[0] for call in run.call_args_list])
                return reader_exit
            reader.wait.side_effect = wait_and_check_eject
            try:
                write_sd.main()
            finally:
                self.calls = (info, run, popen)
        return run, popen

    def test_disk_checks(self):
        write_sd.check_disk(self.disk, '/dev/disk9', len(self.data))
        cases = [
            ({'Internal': True}, '/dev/disk9'),
            ({'WholeDisk': False}, '/dev/disk9s1'),
            ({'VirtualOrPhysical': 'Virtual'}, '/dev/disk9'),
            ({'TotalSize': len(self.data) - 1}, '/dev/disk9'),
        ]
        for change, device in cases:
            with self.subTest(change=change, device=device):
                with self.assertRaises(ValueError):
                    write_sd.check_disk({**self.disk, **change}, device, len(self.data))

    def test_dry_run_does_not_mutate_disk(self):
        run, popen = self.run_main()
        run.assert_not_called()
        popen.assert_not_called()
        self.calls[0].assert_called_once_with(['diskutil', 'info', '-plist', '/dev/disk9'])

    def test_bad_manifest_prevents_mutation(self):
        (self.image.parent / 'SHA256SUMS').write_text(f'{"0" * 64}  {self.image.name}\n')
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            self.run_main('--erase')
        self.calls[1].assert_not_called()
        self.calls[2].assert_not_called()

    def test_erase_ejects_after_successful_readback(self):
        run, popen = self.run_main('--erase', readback=self.data + b'padding')
        self.assertEqual([call.args[0] for call in run.call_args_list], [
            ['diskutil', 'unmountDisk', '/dev/disk9'],
            ['sudo', '/bin/dd', f'if={self.image.resolve()}', 'of=/dev/rdisk9', 'bs=4m'],
            ['sync'], ['diskutil', 'unmountDisk', '/dev/disk9'],
            ['diskutil', 'eject', '/dev/disk9'],
        ])
        popen.assert_called_once_with(['sudo', '/bin/dd', 'if=/dev/rdisk9', 'bs=4m', 'count=1'],
                                      stdout=write_sd.subprocess.PIPE)

    def test_erase_never_ejects_when_readback_fails(self):
        for readback, reader_exit in [(b'wrong content', 0), (self.data[:-1], 0), (self.data, 1)]:
            with self.subTest(readback=readback, reader_exit=reader_exit):
                with self.assertRaisesRegex(RuntimeError, 'read-back verification failed'):
                    self.run_main('--erase', readback=readback, reader_exit=reader_exit)
                self.assertNotIn(['diskutil', 'eject', '/dev/disk9'],
                                 [call.args[0] for call in self.calls[1].call_args_list])


    def make_partitioned_image(self):
        # MBR, 3-sector FAT, then 4-sector Linux rootfs.
        image = bytearray(8 * 512)
        image[450] = 0x0c
        struct.pack_into('<II', image, 454, 1, 3)
        image[466] = 0x83
        struct.pack_into('<II', image, 470, 4, 4)
        image[510:512] = b'\x55\xaa'
        self.data = bytes(image)
        self.image.write_bytes(self.data)
        self.disk['TotalSize'] = len(self.data)
        mount = self.image.parent / 'mount'
        mount.mkdir()
        manifest = [f'{hashlib.sha256(self.data).hexdigest()}  {self.image.name}']
        for name in ('Image', 'meson-g12a-x96-max-p.dtb'):
            contents = f'known {name}'.encode()
            (mount / name).write_bytes(contents)
            manifest.append(f'{hashlib.sha256(contents).hexdigest()}  {name}')
        (self.image.parent / 'SHA256SUMS').write_text('\n'.join(manifest) + '\n')
        changed = bytearray(self.data)
        changed[512 + 21] = 1
        return bytes(changed), mount

    def assert_no_eject(self):
        self.assertNotIn(['diskutil', 'eject', '/dev/disk9'],
                         [call.args[0] for call in self.calls[1].call_args_list])

    def test_verify_only_fat_metadata(self):
        changed, _ = self.make_partitioned_image()
        run, popen = self.run_main('--verify-only', readback=changed)
        self.assertEqual([call.args[0] for call in run.call_args_list], [
            ['diskutil', 'unmountDisk', '/dev/disk9'],
            ['sudo', '/sbin/fsck_msdos', '-n', '/dev/rdisk9s1'],
            ['diskutil', 'mount', 'readOnly', 'nobrowse', '/dev/disk9s1'],
            ['diskutil', 'unmountDisk', '/dev/disk9'],
            ['diskutil', 'eject', '/dev/disk9'],
        ])
        popen.assert_called_once_with(['sudo', '/bin/dd', 'if=/dev/rdisk9', 'bs=4m', 'count=1'],
                                      stdout=write_sd.subprocess.PIPE)
        self.assertFalse(any(arg.startswith('of=') for call in run.call_args_list
                             for arg in call.args[0]))

    def test_verify_only_mbr_or_rootfs_change_is_rejected(self):
        changed, _ = self.make_partitioned_image()
        for offset in (20, 4 * 512 + 30):
            with self.subTest(offset=offset):
                corrupt = bytearray(changed)
                corrupt[offset] = 1
                with self.assertRaisesRegex(RuntimeError, 'outside the FAT partition differs'):
                    self.run_main('--verify-only', readback=bytes(corrupt))
                self.assert_no_eject()

    def test_verify_only_boot_file_corruption_is_rejected(self):
        changed, mount = self.make_partitioned_image()
        (mount / 'Image').write_bytes(b'incorrect file')
        with self.assertRaisesRegex(RuntimeError, 'boot file verification failed: Image'):
            self.run_main('--verify-only', readback=changed)
        self.assert_no_eject()
        self.assertEqual(self.calls[1].call_args_list[-1].args[0],
                         ['diskutil', 'unmountDisk', '/dev/disk9'])

    def test_verify_only_fsck_failure_is_rejected(self):
        changed, _ = self.make_partitioned_image()
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_main('--verify-only', readback=changed, fsck_error=True)
        self.assert_no_eject()
        self.assertNotIn(['diskutil', 'mount', 'readOnly', 'nobrowse', '/dev/disk9s1'],
                         [call.args[0] for call in self.calls[1].call_args_list])


if __name__ == '__main__':
    unittest.main()


class RamRootLayout(WriteSdTests):
    """The RAM-root image is one FAT partition, the disk image is FAT + ext4.

    macOS mounts the FAT volume after a write and adds .fseventsd and
    .Spotlight-V100, so read-back differs inside that range. Verification must
    excuse that for either layout, but only after the FAT and every shipped boot
    file check out. Getting this wrong failed a correct write for the RAM image.
    """

    def make_ramroot_image(self, extra_dtbs=()):
        # MBR, one 0x0c partition covering the whole image, like ramboot.img.
        sectors = 8
        image = bytearray(sectors * 512)
        image[450] = 0x0c
        struct.pack_into('<II', image, 454, 1, sectors - 1)
        image[510:512] = b'\x55\xaa'
        self.data = bytes(image)
        self.image.write_bytes(self.data)
        self.disk['TotalSize'] = len(self.data)
        mount = self.image.parent / 'mount'
        mount.mkdir()
        manifest = [f'{hashlib.sha256(self.data).hexdigest()}  {self.image.name}']
        names = ('Image', 'meson-g12a-x96-max-p.dtb', 'rootfs.cpio.uboot', *extra_dtbs)
        for name in names:
            contents = f'known {name}'.encode()
            (mount / name).write_bytes(contents)
            manifest.append(f'{hashlib.sha256(contents).hexdigest()}  {name}')
        (self.image.parent / 'SHA256SUMS').write_text('\n'.join(manifest) + '\n')
        changed = bytearray(self.data)
        changed[512 + 21] = 1
        return bytes(changed), mount

    def test_single_fat_partition_is_recognized(self):
        self.make_ramroot_image()
        self.assertIsNotNone(write_sd.boot_partition(self.image))

    def test_fat_metadata_on_single_partition_image_is_accepted(self):
        changed, mount = self.make_ramroot_image(
            extra_dtbs=('meson-g12a-x96-max-p-nousb.dtb', 'meson-g12a-x96-max-p-usb.dtb'))
        (mount / '.fseventsd').mkdir()
        self.run_main('--verify-only', readback=changed)
        self.assertEqual([call.args[0] for call in self.calls[1].call_args_list], [
            ['diskutil', 'unmountDisk', '/dev/disk9'],
            ['sudo', '/sbin/fsck_msdos', '-n', '/dev/rdisk9s1'],
            ['diskutil', 'mount', 'readOnly', 'nobrowse', '/dev/disk9s1'],
            ['diskutil', 'unmountDisk', '/dev/disk9'],
            ['diskutil', 'eject', '/dev/disk9'],
        ])

    def test_every_manifest_boot_file_is_verified_not_only_two(self):
        changed, mount = self.make_ramroot_image(
            extra_dtbs=('meson-g12a-x96-max-p-usb.dtb',))
        (mount / 'meson-g12a-x96-max-p-usb.dtb').write_bytes(b'corrupted')
        with self.assertRaisesRegex(RuntimeError,
                                    'boot file verification failed: meson-g12a-x96-max-p-usb.dtb'):
            self.run_main('--verify-only', readback=changed)
        self.assert_no_eject()

    def test_changing_bytes_outside_a_single_partition_is_rejected(self):
        changed, _ = self.make_ramroot_image()
        corrupt = bytearray(changed)
        corrupt[4] = 1  # MBR area, before the partition
        with self.assertRaisesRegex(RuntimeError, 'outside the FAT partition differs'):
            self.run_main('--verify-only', readback=bytes(corrupt))
        self.assert_no_eject()

    def test_image_without_an_Image_entry_is_rejected(self):
        changed, _ = self.make_ramroot_image()
        sums = self.image.parent / 'SHA256SUMS'
        sums.write_text('\n'.join(
            line for line in sums.read_text().splitlines() if not line.endswith('  Image')) + '\n')
        with self.assertRaisesRegex(ValueError, 'names no boot kernel'):
            self.run_main('--verify-only', readback=changed)


class UpdateBootFiles(unittest.TestCase):
    """--update-boot replaces boot files on a card without sudo.

    Writing a whole image needs root; replacing files inside the FAT volume
    does not. The copying itself was verified against a real card (the Image
    hash matched afterwards). These tests cover the safety rails only: without
    a kernel in the manifest, or without a boot-only FAT image, it must refuse.
    """

    def two_partition_image(self, base):
        image = base / "sdcard.img"
        data = bytearray(8 * 512)
        data[450] = 0x0C
        struct.pack_into("<II", data, 454, 1, 3)
        data[466] = 0x83
        struct.pack_into("<II", data, 470, 4, 4)
        data[510:512] = b"\x55\xaa"
        image.write_bytes(bytes(data))
        manifest = [f"{hashlib.sha256(bytes(data)).hexdigest()}  sdcard.img"]
        for name in ("Image", "x.dtb"):
            content = name.encode()
            (base / name).write_bytes(content)
            manifest.append(f"{hashlib.sha256(content).hexdigest()}  {name}")
        (base / "SHA256SUMS").write_text("\n".join(manifest) + "\n")
        return image, {n: hashlib.sha256(c).hexdigest()
                       for n, c in (("Image", b"Image"), ("x.dtb", b"x.dtb"))}

    def test_refuses_without_a_kernel_in_the_manifest(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            image, digests = self.two_partition_image(base)
            manifest = {n: x for n, x in digests.items() if n != "Image"}
            with self.assertRaisesRegex(ValueError, "boot-only FAT volume"):
                write_sd.update_boot_files("/dev/disk99", image, manifest,
                                           write_sd.hash_file(image))

    def test_refuses_a_two_partition_image(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            image, digests = self.two_partition_image(base)
            with self.assertRaisesRegex(ValueError, "boot-only FAT volume"):
                write_sd.update_boot_files("/dev/disk99", image, digests,
                                           write_sd.hash_file(image))

    def test_selection_is_validated_after_the_fat_layout(self):
        """--only cannot rescue an image that is not a boot-only FAT image.

        The FAT range is checked before the selection, so a two-partition image
        is refused with the layout error regardless of --only. This documents
        that ordering rather than pretending names are checked first.
        """
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            image, digests = self.two_partition_image(base)
            with self.assertRaisesRegex(ValueError, "boot-only FAT volume"):
                write_sd.update_boot_files("/dev/disk99", image, digests,
                                           write_sd.hash_file(image),
                                           only=["rootfs.cpio.uboot"])

    def test_refuses_an_empty_boot_list(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            image = base / "sdcard.img"
            image.write_bytes(b"\0" * 1024)
            with self.assertRaisesRegex(ValueError, "boot-only FAT volume"):
                write_sd.update_boot_files("/dev/disk99", image, {},
                                           write_sd.hash_file(image))
