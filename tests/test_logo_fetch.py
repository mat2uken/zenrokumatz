"""Execute the real logo-fetch shell in an isolated fake service filesystem."""
import fcntl
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "board/x96max/overlay/usr/bin/pix-logo-fetch"


class LogoFetchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="logo-fetch-")
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name).resolve()
        self.bin = self.base / "bin"
        self.bin.mkdir()
        self.run = self.base / "run"
        self.run.mkdir()
        self.trace = self.base / "trace"
        self.vendor_lock = self.base / "vendor.lock"
        self.vendor_lock.write_text("original vendor lock inode\n")
        self.inode = self.vendor_lock.stat().st_ino
        self.active = self.base / "active"
        self.fetch_lock = self.run / "pix-logo-fetch.lock"
        defaults = self.base / "defaults"
        defaults.write_text("PIX_TUNER=dt295\nPIX_SYSTEM=bs\n")
        self.executable("id", "#!/bin/sh\necho 0\n")
        # The capture is put in its own session; the host stub keeps the
        # fixture's PATH closed to the machine's real programs.
        real_setsid = "/usr/bin/setsid" if Path("/usr/bin/setsid").exists() else ""
        self.executable("setsid", "#!/bin/sh\n"
                        + (f'exec "{real_setsid}" "$@"\n' if real_setsid else 'exec "$@"\n'))
        self.executable("sleep", "#!/bin/sh\nexit 0\n")
        self.executable("ps", '#!/bin/sh\nif [ -f "$ACTIVE" ]; then echo "123 root pix-ts-legacy"; fi\n')
        self.executable("init", "#!" + sys.executable + "\n" + '''import os, sys
from pathlib import Path
with open(os.environ['TRACE'], 'a') as f: f.write(sys.argv[1] + '\\n')
if sys.argv[1] == 'stop':
    if os.environ.get('MOCK_STOP_FAIL') == '1': sys.exit(1)
    active = Path(os.environ['ACTIVE'])
    assert active.resolve() == Path(os.environ['FIXTURE_ROOT']) / 'active'
    if os.environ.get('MOCK_KEEP_CAPTURE') != '1' and active.exists(): active.unlink()
''')
        self.executable("capture", "#!" + sys.executable + "\n" + '''import fcntl, os, sys
with open(os.environ['TRACE'], 'a') as f: f.write('capture\\n')
with open(os.environ['VENDOR_LOCK'], 'a+') as lock:
    try: fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError: sys.exit(8)
    sys.stdout.write('complete logo data\\n')
''')
        self.executable("parser", "#!" + sys.executable + "\n" + '''import os, sys
with open(os.environ['TRACE'], 'a') as f: f.write('parser\\n')
sys.exit(0 if sys.stdin.buffer.read() else 3)
''')
        text = SOURCE.read_text()
        for old, new in (
            ("/etc/default/pix-stream-linux", str(defaults)),
            ("/etc/init.d/S97pix-stream", str(self.bin / "init")),
            ("/usr/bin/pix-dt295-logo", str(self.bin / "parser")),
            ("/usr/bin/pix-ts-legacy", str(self.bin / "capture")),
            ("/var/run", str(self.run)),
            ("/dev/shm/pix", str(self.base / "shm")),
            ("/tmp/pix-userland-dt295-0.lock", str(self.vendor_lock)),
        ):
            text = text.replace(old, new)
        self.script = self.base / "fetch"
        self.script.write_text(text)
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        TRACE=str(self.trace), ACTIVE=str(self.active),
                        VENDOR_LOCK=str(self.vendor_lock), FIXTURE_ROOT=str(self.base),
                        PIX_LOGO_DIR=str(self.base / "logos"),
                        PIX_LOGO_LOG=str(self.base / "fetch.log"))

    def executable(self, name, text):
        path = self.bin / name
        path.write_text(text)
        path.chmod(0o755)

    def pipeline(self):
        # kill -0 only: never signal the test runner.
        (self.run / "pix-stream.pid").write_text(str(os.getpid()))
        self.active.touch()

    def invoke(self, *args, **env):
        result = subprocess.run(["sh", str(self.script), *args], env=dict(self.env, **env),
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(self.vendor_lock.stat().st_ino, self.inode,
                         "vendor fcntl lock inode must never be replaced")
        return result

    def calls(self):
        return self.trace.read_text().splitlines() if self.trace.exists() else []

    def test_normal_fetch_releases_only_its_own_lock(self):
        result = self.invoke("--force")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertCountEqual(self.calls(), ["capture", "parser"])
        self.assertFalse(self.fetch_lock.exists())

    def test_successful_stop_is_followed_by_capture_then_restart(self):
        self.pipeline()
        result = self.invoke("--force")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls()[0], "stop")
        self.assertEqual(self.calls()[-1], "start")
        self.assertCountEqual(self.calls()[1:-1], ["capture", "parser"])
        self.assertFalse(self.fetch_lock.exists())

    def test_stop_error_never_borrows_tuner(self):
        self.pipeline()
        result = self.invoke("--force", MOCK_STOP_FAIL="1")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(self.calls(), ["stop"])
        self.assertFalse(self.fetch_lock.exists())

    def test_live_capture_after_deadline_never_borrows_tuner(self):
        self.pipeline()
        result = self.invoke("--force", MOCK_KEEP_CAPTURE="1")
        self.assertEqual(result.returncode, 1, result.stderr)
        # The tuner is never borrowed, and the recorder it stopped is given back.
        self.assertEqual(self.calls(), ["stop", "start"])
        self.assertFalse(self.fetch_lock.exists())

    def test_busy_fetch_lock_prevents_even_pipeline_stop(self):
        self.pipeline()
        self.fetch_lock.mkdir()
        result = self.invoke("--force")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(self.calls(), [])
        self.assertTrue(self.fetch_lock.is_dir(), "another owner's lock must remain")

    def test_kernel_vendor_lock_remains_effective_without_process_name_match(self):
        with self.vendor_lock.open("a+") as lock:
            fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.invoke("--force")
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertNotIn("start", self.calls())
        self.assertFalse(self.fetch_lock.exists())

    def test_force_is_required_before_mutation(self):
        self.pipeline()
        result = self.invoke()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(self.calls(), [])
        self.assertFalse(self.fetch_lock.exists())

    def test_dry_run_never_interrupts_recording(self):
        self.pipeline()
        result = self.invoke("--dry-run")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls(), [])
        self.assertFalse(self.fetch_lock.exists())


if __name__ == "__main__":
    unittest.main()
