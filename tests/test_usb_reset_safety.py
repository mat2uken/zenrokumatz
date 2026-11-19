"""Fixture-only USB reset safety tests; never use host /sys, /proc or devices.

Run: python3 -m unittest discover -s tests -p 'test_usb_reset_safety.py' -v
The helper is sourced before fixture paths/functions are installed. Production
has no environment override or test-mode switch. Swap rdev lookup and USB
re-enumeration are mocked; topology parsing and reset control writes are real.
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "board/x96max/overlay/usr/bin"
HELPER = BIN / "pix-usb-reset-common"
GLUE = "ffe09000.usb"


class UsbResetSafetyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pix-usb-safety-")
        self.addCleanup(self.temp.cleanup)
        # macOS /var is a symlink to /private/var; sysfs ancestry is canonical.
        self.base = Path(self.temp.name).resolve()
        self.sys = self.base / "sys"
        self.proc = self.base / "proc"
        self.run = self.base / "run"
        self.run.mkdir()
        (self.proc / "self").mkdir(parents=True)
        (self.proc / "uptime").write_text("1000.00 0.00\n")
        (self.proc / "swaps").write_text("Filename\tType\tSize\tUsed\tPriority\n")
        self.controller = self.sys / "devices/platform" / GLUE
        self.controller.mkdir(parents=True)
        self.driver = self.sys / "bus/platform/drivers/dwc3-meson-g12a"
        self.driver.mkdir(parents=True)
        (self.sys / "bus/platform/devices").mkdir(parents=True)
        (self.sys / "dev/block").mkdir(parents=True)
        (self.driver / GLUE).symlink_to(self.controller)
        (self.sys / "bus/platform/devices" / GLUE).symlink_to(self.controller)
        for control in ("unbind", "bind"):
            (self.driver / control).write_text("")
        usb_disk = self.controller / "xhci/usb1/1-1/host0/target0/block/sda"
        self.disk = self.block("8:0", usb_disk)
        self.usb = self.block("8:2", usb_disk / "sda2", partition=True)
        mmc_disk = self.sys / "devices/platform/mmc/host0/block/mmcblk0"
        self.block("179:0", mmc_disk)
        self.mmc = self.block("179:2", mmc_disk / "mmcblk0p2", partition=True)
        self.mounts(self.mount("0:1", "rootfs", "rootfs"),
                    self.mount("0:2", "tmpfs", "tmpfs", "/run"))
        self.events = self.base / "events"

    def block(self, dev, node, partition=False):
        node.mkdir(parents=True, exist_ok=True)
        (node / "dev").write_text(dev + "\n")
        (node / "holders").mkdir()
        if partition:
            # Real Linux partitions have no slaves directory.
            (node / "partition").write_text("2\n")
        else:
            (node / "slaves").mkdir()
        (self.sys / "dev/block" / dev).symlink_to(node)
        return node

    @staticmethod
    def mount(dev, fs="ext4", source="/dev/root", target="/", root="/"):
        return f"24 1 {dev} {root} {target} rw - {fs} {source} rw\n"

    def mounts(self, *rows):
        (self.proc / "self/mountinfo").write_text("".join(rows))

    def setup_shell(self):
        q = shlex.quote
        return f"""
. {q(str(HELPER))} || exit 90
_PIX_USB_SYS={q(str(self.sys))}
_PIX_USB_PROC={q(str(self.proc))}
_PIX_USB_RUN={q(str(self.run))}
sleep() {{ :; }}
_pix_usb_enumerated() {{ :; }}
stop_pipeline() {{ echo stop >> {q(str(self.events))}; }}
"""

    def shell(self, body="pix_usb_reset stop_pipeline", prefix="", env=None):
        return subprocess.run(["/bin/sh", "-c", self.setup_shell() + prefix + "\n" + body],
                              text=True, capture_output=True, timeout=15, env=env)

    def assert_refused(self, result):
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.driver / "unbind").read_text(), "")
        self.assertEqual((self.driver / "bind").read_text(), "")
        self.assertFalse(self.events.exists(), "preflight refusal stopped the pipeline")
        self.assertFalse((self.run / "pix-usb-reset.state").exists())

    def assert_reset(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.driver / "unbind").read_text(), GLUE + "\n")
        self.assertEqual((self.driver / "bind").read_text(), GLUE + "\n")
        self.assertEqual((self.run / "pix-usb-reset.state").read_text(), "1 1000\n")
        self.assertFalse((self.run / "pix-usb-reset.lock").exists())

    def test_usb_root_dev_root_alias_refuses_before_stop(self):
        self.mounts(self.mount("8:2", source="/dev/root"))
        self.assert_refused(self.shell())

    def test_non_usb_partition_root_allows_reset(self):
        self.mounts(self.mount("179:2", source="/dev/root"))
        self.assert_reset(self.shell())

    def test_initramfs_without_block_mounts_allows_reset(self):
        self.assert_reset(self.shell())
        self.assertEqual(self.events.read_text(), "stop\n")

    def test_ram_root_marker_never_overrides_mounted_usb(self):
        (self.run / "pix-ram-root").write_text("1\n")
        self.mounts(self.mount("8:2"))
        self.assert_refused(self.shell())

    def test_ram_root_with_usb_data_mount_refuses(self):
        self.mounts(self.mount("0:1", "rootfs", "rootfs"),
                    self.mount("8:2", target="/media/data"))
        self.assert_refused(self.shell())

    def test_usb_bind_mount_resolves_by_device_number(self):
        self.mounts(self.mount("8:2", source="/unhelpful/alias", target="/data", root="/subdir"))
        self.assert_refused(self.shell())

    def test_whole_usb_disk_mount_refuses(self):
        self.mounts(self.mount("8:0"))
        self.assert_refused(self.shell())

    def test_mount_in_another_process_namespace_refuses(self):
        (self.proc / "123").mkdir()
        (self.proc / "123/mountinfo").write_text(self.mount("8:2", target="/private"))
        self.assert_refused(self.shell())

    def test_missing_block_mapping_refuses(self):
        self.mounts(self.mount("65:99"))
        self.assert_refused(self.shell())

    def test_mismatched_device_number_refuses(self):
        (self.mmc / "dev").write_text("179:3\n")
        self.mounts(self.mount("179:2"))
        self.assert_refused(self.shell())

    def test_unknown_physical_ancestry_refuses(self):
        self.block("65:1", self.sys / "outside/block/sdz")
        self.mounts(self.mount("65:1"))
        self.assert_refused(self.shell())

    def test_virtual_mapper_mount_refuses(self):
        self.block("253:0", self.sys / "devices/virtual/block/dm-0")
        self.mounts(self.mount("253:0", source="/dev/mapper/root"))
        self.assert_refused(self.shell())

    def test_usb_holders_refuse_even_unmounted(self):
        (self.usb / "holders/dm-0").symlink_to(self.sys / "devices/virtual/block/dm-0")
        self.assert_refused(self.shell())

    def test_parent_disk_holders_refuse_on_non_usb_partition(self):
        (self.mmc.parent / "holders/dm-0").symlink_to(self.sys / "devices/virtual/block/dm-0")
        self.mounts(self.mount("179:2"))
        self.assert_refused(self.shell())

    def test_unknown_holders_directory_refuses(self):
        # Remove only this verified, empty fixture directory.
        (self.usb / "holders").rmdir()
        self.assert_refused(self.shell())

    def test_usb_slaves_refuse_even_unmounted(self):
        (self.disk / "slaves/other").symlink_to(self.mmc)
        self.assert_refused(self.shell())

    def test_unknown_nodev_backing_refuses(self):
        self.mounts(self.mount("0:40", "overlay", "overlay"))
        self.assert_refused(self.shell())

    def test_missing_or_empty_or_malformed_mountinfo_refuses(self):
        path = self.proc / "self/mountinfo"
        for contents in ("", "bad line\n", "24 1 garbage / / rw - ext4 /dev/root rw\n"):
            with self.subTest(contents=contents):
                path.write_text(contents)
                self.assert_refused(self.shell())
        path.unlink()
        self.assert_refused(self.shell())

    def test_missing_controller_mapping_refuses(self):
        (self.driver / GLUE).unlink()
        self.assert_refused(self.shell())

    def test_usb_active_swap_refuses(self):
        (self.proc / "swaps").write_text("Filename Type Size Used Priority\n/dev/sda2 partition 4096 0 -2\n")
        self.assert_refused(self.shell(prefix='_pix_usb_swap_id() { echo 8:2; }'))

    def test_non_usb_active_swap_allows(self):
        (self.proc / "swaps").write_text("Filename Type Size Used Priority\n/dev/mmcblk0p2 partition 4096 0 -2\n")
        self.assert_reset(self.shell(prefix='_pix_usb_swap_id() { echo 179:2; }'))

    def test_swap_unknown_mapping_refuses(self):
        (self.proc / "swaps").write_text("Filename Type Size Used Priority\n/dev/missing partition 4096 0 -2\n")
        self.assert_refused(self.shell(prefix='_pix_usb_swap_id() { echo 65:99; }'))

    def test_swap_file_unresolved_backing_refuses(self):
        swapfile = self.base / "swapfile"
        swapfile.write_text("")
        (self.proc / "swaps").write_text(f"Filename Type Size Used Priority\n{swapfile} file 4096 0 -2\n")
        self.assert_refused(self.shell())

    def test_missing_swaps_refuses(self):
        (self.proc / "swaps").unlink()
        self.assert_refused(self.shell())

    def test_recheck_after_stop_catches_new_mount(self):
        body = f"""
stop_pipeline() {{
    echo stop >> {shlex.quote(str(self.events))}
    printf '%s\\n' '24 1 8:2 / /media rw - ext4 /dev/sda2 rw' > "$_PIX_USB_PROC/self/mountinfo"
}}
pix_usb_reset stop_pipeline
"""
        result = self.shell(body)
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.events.read_text(), "stop\n")
        self.assertEqual((self.driver / "unbind").read_text(), "")
        self.assertFalse((self.run / "pix-usb-reset.state").exists())

    def test_pipeline_stop_error_prevents_unbind(self):
        self.assert_refused(self.shell(prefix='stop_pipeline() { return 1; }'))

    def test_unbind_failure_never_attempts_bind(self):
        result = self.shell(prefix=f"""
_pix_usb_write_control() {{ echo "$1" >> {shlex.quote(str(self.events))}; return 1; }}
""")
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.events.read_text(), "stop\nunbind\n")
        self.assertEqual((self.run / "pix-usb-reset.state").read_text(), "1 1000\n")

    def test_bind_failure_is_not_retried(self):
        result = self.shell(prefix=f"""
_pix_usb_write_control() {{
    echo "$1" >> {shlex.quote(str(self.events))}
    [ "$1" != bind ]
}}
""")
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.events.read_text(), "stop\nunbind\nbind\n")

    def test_term_immediately_after_unbind_still_rebinds_once(self):
        result = self.shell(prefix=f"""
_pix_usb_write_control() {{
    echo "$1" >> {shlex.quote(str(self.events))}
    if [ "$1" = unbind ]; then
        # Only this fixture helper's subshell receives TERM, never its group.
        {shlex.quote(sys.executable)} -c 'import os, signal; os.kill(os.getppid(), signal.SIGTERM)'
    fi
}}
""")
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.events.read_text(), "stop\nunbind\nbind\n")
        self.assertFalse((self.run / "pix-usb-reset.lock").exists())

    def test_interrupted_unbound_delay_still_rebinds_once(self):
        result = self.shell(prefix=f"""
_pix_usb_write_control() {{ echo "$1" >> {shlex.quote(str(self.events))}; }}
sleep() {{ return 1; }}
""")
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.events.read_text(), "stop\nunbind\nbind\n")
        self.assertFalse((self.run / "pix-usb-reset.lock").exists())

    def test_cancel_before_unbind_never_touches_controller(self):
        result = self.shell(prefix=f"""
stop_pipeline() {{ {shlex.quote(sys.executable)} -c 'import os, signal; os.kill(os.getppid(), signal.SIGTERM)'; }}
_pix_usb_write_control() {{ echo "$1" >> {shlex.quote(str(self.events))}; }}
""")
        self.assert_refused(result)

    def test_cancel_after_callback_never_repeats_preflight(self):
        result = self.shell(prefix=f"""
_pix_usb_guard() {{ echo guard >> {shlex.quote(str(self.events))}; }}
stop_pipeline() {{ {shlex.quote(sys.executable)} -c 'import os, signal; os.kill(os.getppid(), signal.SIGTERM)'; }}
_pix_usb_write_control() {{ echo "$1" >> {shlex.quote(str(self.events))}; }}
""")
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.events.read_text(), 'guard\n')
        self.assertFalse((self.run / 'pix-usb-reset.lock').exists())

    def test_cancel_before_bind_write_still_completes_one_bind(self):
        result = self.shell(prefix=f"""
_pix_usb_write_control() {{
    if [ "$1" = bind ]; then
        {shlex.quote(sys.executable)} -c 'import os, signal; os.kill(os.getppid(), signal.SIGTERM)'
    fi
    echo "$1" >> {shlex.quote(str(self.events))}
}}
""")
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.events.read_text(), 'stop\nunbind\nbind\n')
        self.assertFalse((self.run / 'pix-usb-reset.lock').exists())

    def test_second_cancel_cannot_interrupt_compensating_bind(self):
        result = self.shell(prefix=f"""
sleep() {{ return 1; }}
_pix_usb_write_control() {{
    if [ "$1" = bind ]; then
        echo bind-start >> {shlex.quote(str(self.events))}
        [ -d {shlex.quote(str(self.run / 'pix-usb-reset.lock'))} ] || return 1
        {shlex.quote(sys.executable)} -c 'import os, signal; os.kill(os.getppid(), signal.SIGTERM)'
    fi
    echo "$1" >> {shlex.quote(str(self.events))}
}}
""")
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.events.read_text(), 'stop\nunbind\nbind-start\nbind\n')
        self.assertFalse((self.run / 'pix-usb-reset.lock').exists())

    def test_enumeration_failure_stops(self):
        result = self.shell(prefix='_pix_usb_enumerated() { return 1; }')
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.driver / "bind").read_text(), GLUE + "\n")

    def test_lock_prevents_pipeline_interruption(self):
        (self.run / "pix-usb-reset.lock").mkdir()
        self.assert_refused(self.shell())

    def test_rate_limit_prevents_pipeline_interruption(self):
        (self.run / "pix-usb-reset.state").write_text("1 900\n")
        result = self.shell()
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.events.exists())
        self.assertEqual((self.driver / "unbind").read_text(), "")

    def test_three_attempt_boot_budget_bounds_repeated_recovery(self):
        result = self.shell("""
pix_usb_reset || exit 11
printf '1300.00 0.00\\n' > "$_PIX_USB_PROC/uptime"
pix_usb_reset || exit 12
printf '1600.00 0.00\\n' > "$_PIX_USB_PROC/uptime"
pix_usb_reset || exit 13
printf '1900.00 0.00\\n' > "$_PIX_USB_PROC/uptime"
pix_usb_reset stop_pipeline
""")
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.run / "pix-usb-reset.state").read_text(), "3 1600\n")
        self.assertFalse(self.events.exists())

    def test_bad_state_or_uptime_refuses(self):
        state = self.run / "pix-usb-reset.state"
        for contents in ("oops\n", "1 900 extra\n", "1 2000\n", "4 0\n"):
            with self.subTest(contents=contents):
                state.write_text(contents)
                result = self.shell()
                self.assertNotEqual(result.returncode, 0, result.stderr)
                self.assertFalse(self.events.exists())
                self.assertEqual((self.driver / "unbind").read_text(), "")
        state.unlink()
        (self.proc / "uptime").write_text("unreadable\n")
        self.assert_refused(self.shell())

    def test_production_paths_cannot_be_overridden_by_environment(self):
        env = dict(os.environ, _PIX_USB_SYS=str(self.sys), _PIX_USB_PROC=str(self.proc),
                   _PIX_USB_RUN=str(self.run), _PIX_USB_GLUE="bypass")
        # Source only; never invoke the helper with production paths on the host.
        script = f'. {shlex.quote(str(HELPER))}; printf "%s\\n" "$_PIX_USB_SYS $_PIX_USB_PROC $_PIX_USB_RUN $_PIX_USB_GLUE"'
        result = subprocess.run(["/bin/sh", "-c", script], env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "/sys /proc /run ffe09000.usb\n")

    def test_manual_script_refusal_does_not_stop_pipeline(self):
        self.mounts(self.mount("8:2"))
        self.assert_refused(self.run_manual())

    def test_manual_script_safe_reset_stops_then_restarts(self):
        self.assert_reset(self.run_manual())
        self.assertEqual(self.events.read_text(), "stop\nstart\n")

    def test_manual_stop_failure_does_not_reset_or_restart(self):
        result = self.run_manual(stop_status=1)
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.events.read_text(), "stop\n")
        self.assertEqual((self.driver / "unbind").read_text(), "")
        self.assertFalse((self.run / "pix-usb-reset.state").exists())

    def test_manual_reset_preserves_vendor_lock_inode(self):
        lock = self.run / 'tuner.lock'
        lock.write_text('the fcntl inode must survive\n')
        inode = lock.stat().st_ino
        self.assert_reset(self.run_manual())
        self.assertEqual(lock.stat().st_ino, inode)
        self.assertEqual(lock.read_text(), 'the fcntl inode must survive\n')

    def run_manual(self, stop_status=0):
        # Replace only external boundary paths/commands, preserving script flow.
        script = (BIN / "pix-dt295-reset").read_text().replace(
            '. /usr/bin/pix-usb-reset-common || exit 1', self.setup_shell())
        script = script.replace('/etc/init.d/S97pix-stream', shlex.quote(str(self.base / "service")))
        script = script.replace('/var/run/pix-stream.pid', str(self.run / "pix-stream.pid"))
        script = script.replace('/tmp/pix-userland-dt295-0.lock', str(self.run / "tuner.lock"))
        service = self.base / "service"
        service.write_text(f'#!/bin/sh\necho "$1" >> {shlex.quote(str(self.events))}\n'
                           f'[ "$1" != stop ] || exit {stop_status}\n')
        service.chmod(0o700)
        prefix = 'id() { echo 0; }; ps() { :; };\n'
        return subprocess.run(["/bin/sh", "-c", prefix + script], text=True,
                              capture_output=True, timeout=15)

    def test_auto_cleanup_precedes_shared_guard_and_failure_is_returned(self):
        script = (BIN / "pix-stream-simple").read_text()
        function = script.split('dt295_recover() {', 1)[1].split('\n}', 1)[0]
        function = 'dt295_recover() {' + function + '\n}\n'
        prefix = f"""
STOPFILE={shlex.quote(str(self.base / 'service.stopped'))}
log() {{ :; }}
cleanup_children() {{ echo cleanup >> {shlex.quote(str(self.events))}; }}
rm() {{ echo remove-tuner-lock >> {shlex.quote(str(self.events))}; }}
pix_usb_reset() {{ echo guarded-reset >> {shlex.quote(str(self.events))}; return 1; }}
"""
        result = self.shell(function + '\ndt295_recover', prefix=prefix)
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.events.read_text(), "cleanup\nguarded-reset\n")
        self.assertNotIn('rm -f /tmp/pix-userland-dt295-0.lock', script)
        self.assertNotIn('usb1-port1', script)
        self.assertNotRegex(script, r'>[^\n]*(?:/unbind|/bind)')

    def test_zero_output_probe_reaches_recovery_not_suspension(self):
        self.run_auto_loop(usb_root=False)

    def test_auto_usb_root_exits_without_reset_storm(self):
        self.mounts(self.mount("8:2"))
        self.run_auto_loop(usb_root=True)

    def run_auto_loop(self, usb_root):
        # Run the actual shell loop with fake pipeline programs. Every absolute
        # runtime path is redirected into the fixture, including logs/locks.
        script = (BIN / "pix-stream-simple").read_text()
        script = script.replace('. /etc/default/pix-stream-linux 2>/dev/null || true', ':')
        for path in ('/tmp/', '/dev/shm/', '/var/run/'):
            script = script.replace(path, str(self.base) + '/')
        script = script.replace('. /usr/bin/pix-usb-reset-common || exit 1', self.setup_shell())
        fakebin = self.base / 'bin'
        fakebin.mkdir()
        tuner = fakebin / 'pix-ts-legacy'
        tuner.write_text("#!/bin/sh\necho 'firmware could not be reset; a cold power cycle is required' >&2\nexit 1\n")
        tuner.chmod(0o700)
        # Exercise the real supervisor -> worker re-exec; macOS lacks setsid.
        setsid = fakebin / 'setsid'
        setsid.write_text(f'#!{sys.executable}\nimport os, sys\nos.setsid()\nos.execv(sys.argv[1], sys.argv[1:])\n')
        setsid.chmod(0o700)
        prefix = f'PATH={shlex.quote(str(fakebin))}:$PATH\n' + """
ps() { :; }
sleep() { :; }
ffmpeg() { while IFS= read -r line; do :; done; }
eit_watch() { while IFS= read -r line; do :; done; }
stb_streamer() { while IFS= read -r line; do :; done; }
date() { echo 1000; }
PIX_TUNER_WAIT=1
"""
        # setup_shell defines sleep too; make any mistaken 5-minute suspension
        # fail deterministically instead of hanging the test or touching hardware.
        script = script.replace('sleep() { :; }', 'sleep() { [ "$1" != 300 ] || exit 87; }')
        launcher = self.base / 'pix-stream-simple-fixture'
        launcher.write_text('#!/bin/sh\n' + prefix + script)
        launcher.chmod(0o700)
        result = subprocess.run([str(launcher)], text=True,
                                capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        log = (self.base / 'pix-simple.log').read_text()
        self.assertNotIn('broadcast suspension', log)
        self.assertIn('manual intervention required', log)
        if usb_root:
            self.assertEqual((self.driver / 'unbind').read_text(), '')
            self.assertFalse((self.run / 'pix-usb-reset.state').exists())
        else:
            self.assertEqual((self.driver / 'unbind').read_text(), GLUE + '\n')
            self.assertEqual((self.run / 'pix-usb-reset.state').read_text(), '1 1000\n')
            self.assertIn('cooling down', log)


if __name__ == '__main__':
    unittest.main()
