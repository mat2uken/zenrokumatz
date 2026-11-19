"""Offline checks for the unit-3 acceptance tool.

The tool talks to a box over ssh and to the NAS over HTTP; every test here
replaces those two boundaries with fixtures so the judgement logic (what
counts as PASS/FAIL) is verified without touching hardware.
"""
import importlib.util
import io
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("acceptance", ROOT / "tools/check-unit3-acceptance.py")
acceptance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(acceptance)


def run_quiet(fn, *args, **kwargs):
    out = io.StringIO()
    with redirect_stdout(out):
        result = fn(*args, **kwargs)
    return result, out.getvalue()


class BootStage(unittest.TestCase):
    def test_ram_root_without_swap_or_fault_passes(self):
        replies = {
            "cat /proc/cmdline": (0, "console=ttyAML0,115200n8 earlycon rdinit=/init\n", ""),
            "mount | sed": (0, "type rootfs (rw)\n", ""),
            "cat /proc/swaps | wc -l": (0, "1\n", ""),
            "dmesg | grep -c": (0, "0\n", ""),
            "ip -o link show eth0 | head -1": (0, "2: eth0: <UP,LOWER_UP> mtu 1500\n", ""),
            "ip -o -4 addr show eth0": (0, "2: eth0    inet 192.168.99.56/24\n", ""),
        }

        def fake_ssh(host, command, timeout=30):
            for prefix, reply in replies.items():
                if command.startswith(prefix):
                    return reply
            raise AssertionError("unexpected command: " + command)

        with mock.patch.object(acceptance, "ssh", fake_ssh):
            ok, text = run_quiet(acceptance.stage_boot, "192.168.99.56")
        self.assertTrue(ok, text)
        self.assertIn("boot:root-is-ram", text)

    def test_kernel_fault_fails_the_stage(self):
        def fake_ssh(host, command, timeout=30):
            if command.startswith("dmesg"):
                return 0, "1\n", ""
            return 0, "rdinit=/init\n" if "cmdline" in command else "rootfs\n", ""

        with mock.patch.object(acceptance, "ssh", fake_ssh):
            ok, text = run_quiet(acceptance.stage_boot, "192.168.99.56")
        self.assertFalse(ok)
        self.assertIn("FAIL   boot:no-kernel-fault", text)

    def test_swap_presence_fails_the_stage(self):
        def fake_ssh(host, command, timeout=30):
            if command.startswith("cat /proc/swaps"):
                return 0, "3\n", ""
            return 0, "rdinit=/init rootfs LOWER_UP inet\n", ""

        with mock.patch.object(acceptance, "ssh", fake_ssh):
            ok, text = run_quiet(acceptance.stage_boot, "192.168.99.56")
        self.assertFalse(ok)
        self.assertIn("FAIL   boot:no-swap", text)


class ReceiptStage(unittest.TestCase):
    def test_busy_tuner_is_reported_without_failing_the_tuners_identity(self):
        def fake_ssh(host, command, timeout=30):
            if "lsusb" in command:
                return 0, "1\n", ""
            if "ps | grep -c" in command:
                return 0, "3\n", ""
            if command.startswith("pix-ts-legacy"):
                self.assertIn("-s terrestrial", command)
                self.assertIn("-c 17", command)
                return 0, "rc=4\n", ""
            if "tables_ready" in command:
                return 0, "", ""
            if "locked" in command:
                return 0, "", ""
            if "eit_watch" in command:
                return 0, "", ""
            raise AssertionError("unexpected command: " + command)

        with mock.patch.object(acceptance, "ssh", fake_ssh):
            ok, text = run_quiet(acceptance.stage_receipt, "192.168.99.56", "terrestrial", 17, "")
        self.assertFalse(ok, "a failed capture must not count as PASS")
        self.assertIn("FAIL   receipt:tuner-idle", text)
        self.assertIn("stop the service", text)

    def test_satellite_capture_passes_the_slot_through(self):
        seen = {}

        def fake_ssh(host, command, timeout=30):
            if "lsusb" in command:
                return 0, "1\n", ""
            if "ps | grep -c" in command:
                return 0, "0\n", ""
            if command.startswith("pix-ts-legacy"):
                seen["command"] = command
                return 0, "rc=0\n", ""
            if "tables_ready" in command:
                return 0, '"tables_ready":1\n', ""
            if "locked" in command:
                return 0, '"locked":1\n', ""
            if "eit_watch" in command:
                return 0, "service=35864\n", ""
            raise AssertionError("unexpected command: " + command)

        with mock.patch.object(acceptance, "ssh", fake_ssh):
            ok, text = run_quiet(acceptance.stage_receipt, "192.168.99.123", "bs", 9, "--slot 0")
        self.assertTrue(ok, text)
        self.assertIn("-s bs", seen["command"])
        self.assertIn("--slot 0", seen["command"])


class StreamStage(unittest.TestCase):
    def test_growing_push_with_no_drops_passes(self):
        replies = [{"capturing": True, "totalPushed": 10, "totalDropped": 0},
                   {"capturing": True, "totalPushed": 25, "totalDropped": 0}]
        with mock.patch.object(acceptance, "http_json", side_effect=replies), \
                mock.patch.object(acceptance.time, "sleep"):
            ok, text = run_quiet(acceptance.stage_stream, "192.168.99.56")
        self.assertTrue(ok, text)

    def test_stalled_push_fails(self):
        same = {"capturing": True, "totalPushed": 10, "totalDropped": 0}
        with mock.patch.object(acceptance, "http_json", side_effect=[same, same]), \
                mock.patch.object(acceptance.time, "sleep"):
            ok, text = run_quiet(acceptance.stage_stream, "192.168.99.56")
        self.assertFalse(ok)
        self.assertIn("FAIL   stream:pushed-grows", text)

    def test_dropped_fragments_fail(self):
        replies = [{"capturing": True, "totalPushed": 10, "totalDropped": 0},
                   {"capturing": True, "totalPushed": 40, "totalDropped": 2}]
        with mock.patch.object(acceptance, "http_json", side_effect=replies), \
                mock.patch.object(acceptance.time, "sleep"):
            ok, text = run_quiet(acceptance.stage_stream, "192.168.99.56")
        self.assertFalse(ok)
        self.assertIn("FAIL   stream:no-drop", text)


class NasStage(unittest.TestCase):
    def test_registered_recording_node_passes(self):
        nodes = [{"id": "192.168.99.56-17", "host": "192.168.99.56", "channel": "17",
                  "channelName": "ＳＢＳ１", "state": "recording"}]
        with mock.patch.object(acceptance, "http_json", return_value=nodes):
            ok, text = run_quiet(acceptance.stage_nas, "192.168.99.56", 17)
        self.assertTrue(ok, text)

    def test_same_ip_under_another_channel_is_flagged_as_stale(self):
        nodes = [{"id": "192.168.99.56-17", "host": "192.168.99.56", "channel": "17",
                  "state": "recording"},
                 {"id": "192.168.99.56-15", "host": "192.168.99.56", "channel": "15",
                  "state": "stopped"}]
        with mock.patch.object(acceptance, "http_json", return_value=nodes):
            ok, text = run_quiet(acceptance.stage_nas, "192.168.99.56", 17)
        self.assertFalse(ok)
        self.assertIn("FAIL   nas:no-stale-node", text)

    def test_unreachable_nas_fails_closed(self):
        with mock.patch.object(acceptance, "http_json", return_value=None):
            ok, text = run_quiet(acceptance.stage_nas, "192.168.99.56", 17)
        self.assertFalse(ok)
        self.assertIn("FAIL   nas:reachable", text)


if __name__ == "__main__":
    unittest.main()
