"""wait-carrier: bounded wait for eth0 link before DHCP.

The external RTL8211F on this board links about five seconds after ifup, and
BusyBox udhcpc forked to the background with no lease when the carrier was
still down. These tests drive the real script with a stand-in sysfs tree.
"""
import os
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "board/x96max/overlay/etc/network/wait-carrier"


class WaitCarrier(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)
        self.carrier = self.base / "eth0"
        self.carrier.write_text("0")
        # Point the script's sysfs path at the stand-in file.
        text = SCRIPT.read_text().replace('"/sys/class/net/$iface/carrier"', '"%s"' % self.carrier)
        self.script = self.base / "wait-carrier"
        self.script.write_text(text)
        self.script.chmod(0o755)

    def tearDown(self):
        self.tmp.cleanup()

    def run_script(self, *args):
        start = time.monotonic()
        result = subprocess.run(["/bin/sh", str(self.script), *args],
                                capture_output=True, text=True)
        return result, time.monotonic() - start

    def test_returns_immediately_when_already_linked(self):
        self.carrier.write_text("1")
        result, elapsed = self.run_script("eth0", "10")
        self.assertEqual(result.returncode, 0)
        self.assertLess(elapsed, 1.0)

    def test_stops_waiting_once_carrier_appears(self):
        import threading
        threading.Timer(1.5, lambda: self.carrier.write_text("1")).start()
        result, elapsed = self.run_script("eth0", "10")
        self.assertEqual(result.returncode, 0)
        # One-second polling, so it must notice well before the 10 s bound.
        self.assertLess(elapsed, 4.0)
        self.assertGreaterEqual(elapsed, 1.0)

    def test_gives_up_at_the_bound_without_failing(self):
        result, elapsed = self.run_script("eth0", "2")
        self.assertEqual(result.returncode, 0,
                         "a missing link is DHCP's business, not this helper's failure")
        self.assertGreaterEqual(elapsed, 1.5)
        self.assertLess(elapsed, 4.0)

    def test_rejects_bad_usage(self):
        for args in ((), ("eth0", "abc"), ("eth0", "-1")):
            with self.subTest(args=args):
                result, _ = self.run_script(*args)
                self.assertEqual(result.returncode, 2)

    def test_missing_carrier_file_is_treated_as_no_link(self):
        self.carrier.unlink()
        result, elapsed = self.run_script("eth0", "1")
        self.assertEqual(result.returncode, 0)
        self.assertLess(elapsed, 3.0)


if __name__ == "__main__":
    unittest.main()
