"""Execute the real logo-fetch shell in an isolated fake service filesystem."""
import fcntl
import os
from pathlib import Path
import signal
import subprocess
import time
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
        self.executable("setsid", "#!" + sys.executable + "\n"
                        "import os, sys\nos.setsid()\nos.execvp(sys.argv[1], sys.argv[1:])\n")
        self.executable("sleep", "#!/bin/sh\nexec /bin/sleep 0.02\n")
        self.executable("ps", '#!/bin/sh\nif [ -f "$ACTIVE" ]; then echo "123 root pix-ts-legacy"; fi\n')
        self.executable("init", "#!" + sys.executable + "\n" + '''import os, signal, sys
from pathlib import Path
with open(os.environ['TRACE'], 'a') as f: f.write(sys.argv[1] + '\\n')
if sys.argv[1] == 'resume':
    if os.environ.get('CHECK_TERM_DEFAULT') == '1':
        assert signal.getsignal(signal.SIGTERM) == signal.SIG_DFL
    assert not (Path(os.environ['FIXTURE_ROOT']) / 'run/pix-logo-fetch.lock').exists()
    if os.environ.get('MOCK_START_FAIL') == '1': sys.exit(1)
if sys.argv[1] == 'pause':
    if os.environ.get('MOCK_STOP_FAIL') == '1': sys.exit(1)
    active = Path(os.environ['ACTIVE'])
    assert active.resolve() == Path(os.environ['FIXTURE_ROOT']) / 'active'
    if os.environ.get('MOCK_KEEP_CAPTURE') != '1' and active.exists(): active.unlink()
''')
        self.executable("capture", "#!" + sys.executable + "\n" + '''import fcntl, os, signal, sys, time
with open(os.environ['TRACE'], 'a') as f: f.write('capture\\n')
if os.environ.get('MOCK_SLOW_CAPTURE') == '1':
    def close(signum, frame):
        time.sleep(0.2)
        with open(os.environ['TRACE'], 'a') as f: f.write('closed\\n')
        sys.exit(0)
    signal.signal(signal.SIGTERM, close)
    signal.signal(signal.SIGINT, close)
    with open(os.environ['TRACE'], 'a') as f: f.write('ready\\n')
    while True: time.sleep(60)
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
        self.assertEqual(self.calls()[0], "pause")
        self.assertEqual(self.calls()[-1], "resume")
        self.assertCountEqual(self.calls()[1:-1], ["capture", "parser"])
        self.assertFalse(self.fetch_lock.exists())

    def test_successful_stop_is_followed_by_capture_then_restart(self):
        self.pipeline()
        result = self.invoke("--force")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls()[0], "pause")
        self.assertEqual(self.calls()[-1], "resume")
        self.assertCountEqual(self.calls()[1:-1], ["capture", "parser"])
        self.assertFalse(self.fetch_lock.exists())

    def test_stop_error_never_borrows_tuner(self):
        self.pipeline()
        result = self.invoke("--force", MOCK_STOP_FAIL="1")
        self.assertEqual(result.returncode, 1, result.stderr)
        # Desired-running restoration is inherited by every lock owner, even
        # when pause itself fails; the real S97 rejects any unsafe restart.
        self.assertEqual(self.calls(), ["pause", "resume"])
        self.assertFalse(self.fetch_lock.exists())

    def test_live_capture_after_deadline_never_borrows_tuner(self):
        self.pipeline()
        result = self.invoke("--force", MOCK_KEEP_CAPTURE="1")
        self.assertEqual(result.returncode, 1, result.stderr)
        # The tuner is never borrowed, and the recorder it stopped is given back.
        self.assertEqual(self.calls(), ["pause", "resume"])
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
        self.assertEqual(self.calls()[0], "pause")
        self.assertEqual(self.calls()[-1], "resume")
        self.assertFalse(self.fetch_lock.exists())

    def test_restored_service_does_not_inherit_ignored_term(self):
        result = self.invoke("--force", CHECK_TERM_DEFAULT="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls()[-1], "resume")

    def test_restore_failure_is_nonzero(self):
        self.pipeline()
        result = self.invoke("--force", MOCK_START_FAIL="1")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(self.calls()[-1], "resume")
        self.assertFalse(self.fetch_lock.exists())
        self.assertIn("resume returned nonzero; verify recording", result.stderr)

    def cancel_slow_fetch(self, shutdown=False):
        self.pipeline()
        process = subprocess.Popen(["sh", str(self.script), "--force"],
                                   env=dict(self.env, MOCK_SLOW_CAPTURE="1"),
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        def reap():
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=3)
        self.addCleanup(reap)
        deadline = time.monotonic() + 3
        while "ready" not in self.calls() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertIn("ready", self.calls())
        if shutdown:
            (self.run / "pix-stream.stopped").touch()
        process.send_signal(signal.SIGTERM)
        self.assertEqual(process.wait(timeout=3), 143)
        self.assertIn("closed", self.calls())
        self.assertFalse(self.fetch_lock.exists())
        self.assertEqual(self.vendor_lock.stat().st_ino, self.inode)
        return self.calls()

    def test_manual_cancel_waits_for_capture_then_restores(self):
        calls = self.cancel_slow_fetch()
        self.assertLess(calls.index("closed"), calls.index("resume"))

    def test_shutdown_cancel_drains_capture_without_restoring(self):
        calls = self.cancel_slow_fetch(shutdown=True)
        self.assertNotIn("resume", calls)
        self.assertNotIn("start", calls)

    def test_existing_stop_fence_prevents_even_borrow(self):
        self.pipeline()
        (self.run / "pix-stream.stopped").touch()
        result = self.invoke("--force")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(self.calls(), [])
        self.assertFalse(self.fetch_lock.exists())

    def test_successive_fetch_in_release_resume_gap_inherits_restoration(self):
        self.successive_fetch_gap()

    def test_successor_logo_stop_before_pause_still_restores(self):
        self.successive_fetch_gap(cancel="logo-stop")

    def test_successor_term_before_pause_still_restores(self):
        self.successive_fetch_gap(cancel="term")

    def successive_fetch_gap(self, cancel=None):
        self.pipeline()
        first_resume = self.base / "first-resume"
        second_capture = self.base / "second-capture"
        finish_second = self.base / "finish-second"
        pidfile = self.run / "pix-stream.pid"
        self.executable("init", "#!" + sys.executable + "\n" + r'''import os, sys, time
from pathlib import Path
root = Path(os.environ['FIXTURE_ROOT'])
tag, action = os.environ['FETCH_TAG'], sys.argv[1]
with open(os.environ['TRACE'], 'a') as f: f.write(tag + '-' + action + '\n')
if action == 'pause':
    for path in (root / 'run/pix-stream.pid', root / 'active'):
        if path.exists(): path.unlink()
if action == 'resume':
    if tag == 'first':
        (root / 'first-resume').touch()
        deadline = time.monotonic() + 3
        while not (root / 'second-capture').exists():
            assert time.monotonic() < deadline
            time.sleep(0.01)
    if (root / 'run/pix-logo-fetch.lock').exists(): sys.exit(1)
    (root / 'run/pix-stream.pid').write_text('restored\n')
''')
        self.executable("capture", "#!" + sys.executable + "\n" + r'''import os, sys, time
from pathlib import Path
root = Path(os.environ['FIXTURE_ROOT'])
with open(os.environ['TRACE'], 'a') as f: f.write(os.environ['FETCH_TAG'] + '-capture\n')
if os.environ['FETCH_TAG'] == 'second':
    (root / 'second-capture').touch()
    deadline = time.monotonic() + 5
    while not (root / 'finish-second').exists():
        assert time.monotonic() < deadline
        time.sleep(0.01)
sys.stdout.write('complete logo data\n')
''')
        if cancel:
            # Stop B after lock/pid publication but before the first fence
            # check and pause. A's resume must encounter that live lock.
            self.executable("mkdir", "#!" + sys.executable + "\n" + r'''import os, sys, time
from pathlib import Path
root = Path(os.environ['FIXTURE_ROOT'])
if os.environ['FETCH_TAG'] == 'second' and sys.argv[1:2] == ['-p']:
    (root / 'second-capture').touch()
    deadline = time.monotonic() + 5
    while not (root / 'finish-second').exists():
        assert time.monotonic() < deadline
        time.sleep(0.01)
os.execv('/bin/mkdir', ['/bin/mkdir', *sys.argv[1:]])
''')
        processes = []
        def cleanup():
            finish_second.touch()
            for process in processes:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=4)
        self.addCleanup(cleanup)
        def launch(tag):
            process = subprocess.Popen(["sh", str(self.script), "--force"],
                                       env=dict(self.env, FETCH_TAG=tag),
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            processes.append(process)
            return process
        def until_file(path):
            deadline = time.monotonic() + 3
            while not path.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(path.exists(), self.calls())
        first = launch("first")
        until_file(first_resume)
        self.assertFalse(pidfile.exists(), 'first pause removed the live PID')
        second = launch("second")
        until_file(second_capture)
        self.assertEqual(first.wait(timeout=3), 1, 'first resume must hit second borrow exclusion')
        self.assertTrue(self.fetch_lock.is_dir())
        self.assertFalse(pidfile.exists())
        if cancel == "logo-stop":
            (self.run / "pix-logo.stopped").touch()
        elif cancel == "term":
            second.send_signal(signal.SIGTERM)
        finish_second.touch()
        expected = 143 if cancel == "term" else (1 if cancel else 0)
        self.assertEqual(second.wait(timeout=3), expected)
        if cancel:
            self.assertNotIn("second-pause", self.calls())
            self.assertNotIn("second-capture", self.calls())
        else:
            self.assertIn("second-pause", self.calls(), 'absence of PID is not desired-stop intent')
        self.assertIn("second-resume", self.calls())
        self.assertEqual(pidfile.read_text(), "restored\n")
        self.assertFalse(self.fetch_lock.exists())

    def test_term_during_lock_acquisition_does_not_leave_stale_owner(self):
        self.cancel_during_lock_acquisition(signal.SIGTERM)

    def test_group_term_during_lock_acquisition_does_not_leave_stale_owner(self):
        self.cancel_during_lock_acquisition(signal.SIGTERM, group=True)

    def test_group_int_during_lock_acquisition_does_not_leave_stale_owner(self):
        self.cancel_during_lock_acquisition(signal.SIGINT, group=True)

    def cancel_during_lock_acquisition(self, signum, group=False):
        created = self.base / "acquired"
        proceed = self.base / "finish-mkdir"
        self.executable("mkdir", "#!" + sys.executable + "\n" + r'''import os, sys, time
from pathlib import Path
root = Path(os.environ['FIXTURE_ROOT'])
if sys.argv[1:] == [str(root / 'run/pix-logo-fetch.lock')]:
    os.mkdir(sys.argv[1])
    (root / 'acquired').touch()
    deadline = time.monotonic() + 4
    while not (root / 'finish-mkdir').exists():
        assert time.monotonic() < deadline
        time.sleep(0.01)
else:
    os.execv('/bin/mkdir', ['/bin/mkdir', *sys.argv[1:]])
''')
        process = subprocess.Popen(["sh", str(self.script), "--force"], env=self.env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   start_new_session=group)
        def cleanup():
            proceed.touch()
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=3)
        self.addCleanup(cleanup)
        deadline = time.monotonic() + 3
        while not created.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(created.exists())
        if group:
            os.killpg(process.pid, signum)  # this fixture's new session only
        else:
            process.send_signal(signum)
        proceed.touch()
        self.assertEqual(process.wait(timeout=3), 143)
        self.assertFalse(self.fetch_lock.exists())
        self.assertEqual(self.calls(), ["resume"])
        self.assertEqual(self.vendor_lock.stat().st_ino, self.inode)

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
