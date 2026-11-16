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
