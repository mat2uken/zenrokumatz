"""Check production graceful signals against the real vendor fork-writer seam.

No USB or firmware is involved: these tests prove writer survival and END/ACK,
not that a real DT295 reached idle state 16. Binaries/output stay in temp dirs.
"""
import json
import os
from pathlib import Path
import re
import selectors
import shlex
import signal
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / "vendor/pix-userland"
OVERLAY = ROOT / "board/x96max/overlay/usr/bin"


def build_fixture(binary):
    command = shlex.split(os.environ.get("HOSTCC", "cc"))
    command += ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                "-I" + str(VENDOR / "include"),
                str(ROOT / "tests/dt295_stop_signal_fixture.c"),
                str(VENDOR / "src/ts_output.c"), "-o", str(binary)]
    subprocess.run(command, check=True, capture_output=True, text=True)


class Dt295StopSignalTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="dt295-stop-native-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.binary = Path(cls.build.name) / "stop-signal-fixture"
        build_fixture(cls.binary)

    def exercise(self, signum, group, piped=False):
        with tempfile.TemporaryDirectory(prefix="dt295-stop-output-") as directory:
            output = Path(directory) / "writer.log"
            command = [str(self.binary)] + (["--pipe"] if piped else []) + [str(output)]
            process = subprocess.Popen(command, text=True,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       start_new_session=True)
            try:
                with selectors.DefaultSelector() as ready:
                    ready.register(process.stdout, selectors.EVENT_READ)
                    self.assertTrue(ready.select(timeout=5), "native writer readiness timeout")
                announcement = json.loads(process.stdout.readline())
                self.assertTrue(announcement["ready"])
                self.assertEqual(announcement["parent"], process.pid)
                self.assertNotEqual(announcement["writer"], process.pid)
                # Only this fixture's newly-created private session is signaled.
                if group:
                    os.killpg(process.pid, signum)
                else:
                    process.send_signal(signum)
                stdout, stderr = process.communicate(timeout=5)
                self.assertEqual(stderr, "")
                result = json.loads(stdout)
                self.assertEqual(result["signal"], signum)
                return process.returncode, result, output.read_text()
            finally:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=3)
                process.stdout.close()
                process.stderr.close()

    def production_signal(self, name, variable):
        source = (OVERLAY / name).read_text()
        match = re.search(r'kill -(INT|TERM) -"\$' + variable + '"', source)
        self.assertIsNotNone(match, "missing production graceful group signal")
        return getattr(signal, "SIG" + match[1])

    def assert_writer_closed(self, result):
        status, details, text = result
        self.assertEqual(status, 0, details)
        self.assertEqual(details["error"], 0)
        self.assertEqual(details["destroy"], 0)
        self.assertEqual(details["pending"], 0)
        self.assertEqual(details["child_status"], 0)
        self.assertEqual(text, "writer-ready\nwriter-end\n")

    def test_group_int_still_breaks_real_pipe_reader_before_native_finish(self):
        status, details, text = self.exercise(signal.SIGINT, group=True, piped=True)
        self.assertEqual(status, 1)
        self.assertNotEqual(details["error"], 0)
        self.assertEqual(details["reader_term"], signal.SIGINT)
        self.assertNotIn("writer-end", text)

    def test_parent_only_term_keeps_pipe_reader_until_native_end_ack(self):
        self.assert_writer_closed(self.exercise(signal.SIGTERM, group=False, piped=True))

    def test_logo_graceful_signal_preserves_native_writer_end_ack(self):
        self.assert_writer_closed(self.exercise(
            self.production_signal("pix-logo-fetch", "pipeline"), group=True))

    def test_group_term_is_native_writer_abort_not_graceful_close(self):
        status, details, text = self.exercise(signal.SIGTERM, group=True)
        self.assertEqual(status, 1)
        self.assertNotEqual(details["error"], 0)
        self.assertEqual(details["destroy"], 0)
        self.assertEqual(details["child_term"], signal.SIGTERM)
        self.assertEqual(text, "writer-ready\n")

    def test_parent_only_term_preserves_native_writer_end_ack(self):
        self.assert_writer_closed(self.exercise(signal.SIGTERM, group=False))


if __name__ == "__main__":
    unittest.main()
