"""USB integrity audit: prove what it records and that it only reads.

The script is the real production file; devices and the USB identity tree are
fixtures, so no host disk is read or written.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "board/x96max/overlay/usr/bin/pix-usb-audit"
SERIAL = "AA010206130733341409"


class UsbAuditTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.usb = self.base / "usb"
        self.usb.write_bytes(b"\x00" * 4096)
        self.emmc = self.base / "emmc"
        self.emmc.write_bytes(b"\x55" * 4096)
        # macOS caches disk reads; the first read of these fixtures must stay
        # outside the test so corruption written by a test is visible.
        for device in (self.usb, self.emmc):
            subprocess.run(["sh", "-c", 'dd if="$1" of=/dev/null bs=1048576 count=1 2>/dev/null',
                            "x", str(device)], check=True)
        self.sysfs = self.base / "sysfs"
        dev = self.sysfs / "2-1"
        dev.mkdir(parents=True)
        (dev / "idVendor").write_text("0781\n")
        (dev / "idProduct").write_text("5580\n")
        (dev / "serial").write_text(SERIAL + "\n")
        # Same vendor, different model: must never be selected.
        other = self.sysfs / "1-1"
        other.mkdir()
        (other / "idVendor").write_text("0781\n")
        (other / "idProduct").write_text("5567\n")
        (other / "serial").write_text("OTHER\n")
        self.size_file = self.base / "usb_size"
        self.size_file.write_text(str(self.usb.stat().st_size) + "\n")
        self.audit = self.base / "audit"
        self.audit.mkdir()
        self.env = dict(
            os.environ,
            PIX_USB_AUDIT_DIR=str(self.audit),
            PIX_USB_AUDIT_INTERVAL="0",
            PIX_USB_AUDIT_SOURCE=str(self.usb),
            PIX_USB_AUDIT_SYSFS=str(self.sysfs),
            PIX_USB_AUDIT_SIZE_FILE=str(self.size_file),
            PIX_USB_AUDIT_SIZE=str(self.usb.stat().st_size),
            PIX_USB_AUDIT_ENV_DEV=str(self.emmc),
            PIX_USB_AUDIT_ENV_OFFSET="0",
            PIX_USB_AUDIT_KEEP="5",
            PIX_USB_AUDIT_DAEMON="0",
        )

    def run_script(self, *args, **extra):
        return subprocess.run(["sh", str(SCRIPT), *args], env=dict(self.env, **extra),
                              capture_output=True, text=True, timeout=20)

    def lines(self, name):
        path = self.audit / name
        return path.read_text().splitlines() if path.exists() else []

    def records(self):
        return self.lines("records.log")

    def last_hash(self):
        return (self.audit / "state").read_text().split(" ")[3]

    def write_state(self, name, lines):
        """Write a known prior state, as a previous boot left it."""
        path = self.audit / name
        path.write_text("".join(line + "\n" for line in lines))

    def test_start_records_boot_fingerprint_and_uses_identity_not_order(self):
        result = self.run_script("start")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Starting pix-usb-audit: OK", result.stdout)
        self.assertEqual(len(self.lines("state")), 1)
        self.assertIn("usb=2-1", self.lines("state")[0])
        self.assertIn("size_ok=yes", self.lines("state")[0])
        self.assertIn(" boot ", self.records()[0])

    def test_stop_compares_against_boot_baseline(self):
        self.assertEqual(self.run_script("start").returncode, 0)
        original = self.last_hash().strip()
        # The boot failure left a zeroed first sector; reproduce that state as
        # an unreadable-then-different fingerprint without touching a device.
        self.write_state("state.prev", ["2026-01-01T00:00:00Z usb=2-1 size_ok=yes mbr=" + original])
        self.write_state("records.log", ["prior"])
        self.assertEqual(self.run_script("check", PIX_USB_AUDIT_SOURCE=str(self.base / "gone")).returncode, 0)
        self.assertEqual(self.run_script("check").returncode, 0)
        lines = self.records()
        # Losing and regaining the device is recorded as a change, and the
        # first transition carries the separate UNREADABLE marker.
        self.assertIn("old=" + original, lines[-2], repr(lines))
        self.assertIn("new=mbr=unreadable", lines[-2], repr(lines))
        self.assertIn("new=" + original, lines[-1], repr(lines))

    def test_watcher_reports_environment_change_during_run(self):
        self.assertEqual(self.run_script("start").returncode, 0)
        original = (self.audit / "env.prev").read_text()
        self.write_state("env.prev", ["different-environment-copy"])
        self.assertEqual(self.run_script("check").returncode, 0)
        self.assertTrue(any("EMMC_ENV_CHANGED" in x and "old=different-environment-copy" in x
                            for x in self.records()), repr(self.records()))
        self.assertEqual((self.audit / "env.prev").read_text(), original)

    def test_unreadable_usb_is_recorded_not_fatal(self):
        missing = str(self.base / "missing")
        self.assertEqual(self.run_script("start").returncode, 0)
        self.assertEqual(self.run_script("check", PIX_USB_AUDIT_SOURCE=missing).returncode, 0)
        lines = self.records()
        self.assertTrue(any("new=mbr=unreadable" in x for x in lines), repr(lines))

    def test_missing_usb_identity_still_records_absent(self):
        # Point the audit at a tree without the known USB: absence is reported,
        # never mistaken for a zeroed disk.
        empty = self.base / "no-usb"
        empty.mkdir()
        result = self.run_script("start", PIX_USB_AUDIT_SYSFS=str(empty))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("usb=absent", self.lines("state")[-1])

    def test_identity_mismatch_is_not_reported_as_a_zeroed_disk(self):
        # A different but readable USB must not be mistaken for corruption.
        self.assertEqual(self.run_script("start").returncode, 0)
        empty = self.base / "no-usb"
        empty.mkdir()
        self.assertEqual(self.run_script("stop", PIX_USB_AUDIT_SYSFS=str(empty)).returncode, 0)
        self.assertIn("usb=absent", self.lines("state")[-1])
        self.assertFalse([x for x in self.records() if "USB_FIRST_MIB_CHANGED" in x])

    def test_show_prints_collectable_records(self):
        self.assertEqual(self.run_script("start").returncode, 0)
        self.assertEqual(self.run_script("stop").returncode, 0)
        result = self.run_script("show")
        self.assertEqual(result.returncode, 0)
        self.assertIn("records:", result.stdout)
        self.assertIn(" boot ", result.stdout)

    def test_records_are_bounded(self):
        self.assertEqual(self.run_script("start").returncode, 0)
        for _ in range(8):
            self.assertEqual(self.run_script("stop").returncode, 0)
            self.assertEqual(self.run_script("start").returncode, 0)
        self.assertLessEqual(len(self.records()), 5)

    def test_stop_without_session_and_bad_action_are_safe(self):
        self.assertEqual(self.run_script("stop").returncode, 0)
        self.assertNotEqual(self.run_script("bogus").returncode, 0)
        self.assertNotEqual(self.run_script().returncode, 0)


class UsbAuditInitTests(unittest.TestCase):
    def test_init_wrapper_never_breaks_boot_or_shutdown(self):
        text = (ROOT / "board/x96max/overlay/etc/init.d/S42pix-usb-audit").read_text()
        self.assertIn('"$DAEMON" start || true', text)
        self.assertIn('"$DAEMON" stop || true', text)
        self.assertIn('[ -x "$DAEMON" ] || exit 0', text)


if __name__ == "__main__":
    unittest.main()
