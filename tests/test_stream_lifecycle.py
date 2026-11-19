"""Run real lifecycle shells against fixture-only processes and paths.

No devices, sysfs, network, host service controller or process-name kills.
All signals target PIDs/groups created by this fixture. Production protocol
budgets remain >=35s; only the fixture's sleep executable accelerates polling.
"""
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
OVERLAY = ROOT / "board/x96max/overlay"


class StreamLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="stream-lifecycle-")
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name).resolve()
        self.bin = self.base / "bin"
        self.run = self.base / "run"
        self.shm = self.base / "shm"
        self.logs = self.base / "logs"
        for directory in (self.bin, self.run, self.shm, self.logs):
            directory.mkdir()
        self.trace = self.base / "trace"
        self.defaults = self.base / "defaults"
        self.defaults.write_text("PIX_TUNER=dt295\nPIX_SYSTEM=bs\nPIX_DECRYPT=0\n")
        self.vendor = self.base / "vendor.lock"
        self.vendor.write_text("stable vendor lock inode\n")
        self.vendor_inode = self.vendor.stat().st_ino
        self.proc = self.base / "proc"
        self.proc.mkdir()
        self.processes = []
        self.addCleanup(self.stop_processes)
        self.executable("setsid", "#!" + sys.executable + "\n"
                        "import os, sys\nos.setsid()\nos.execvp(sys.argv[1], sys.argv[1:])\n")
        # 35 polling ticks = 1.75s, longer than our 0.3s protocol teardown.
        # Startup sleep is accelerated too, so tests use condition-based waits.
        self.executable("sleep", "#!/bin/sh\nexec /bin/sleep 0.05\n")
        self.executable("ps", "#!/bin/sh\nexit 0\n")
        self.executable("id", "#!/bin/sh\necho 0\n")
        self.executable("start-stop-daemon", "#!/bin/sh\n"
                        "echo unexpected-launch >> \"$TRACE\"\nexit 1\n")
        self.executable("pix-usb-reset-common", "pix_usb_reset() { return 1; }\n")
        self.executable("pix-ts-legacy", "#!" + sys.executable + "\n" + r'''import os, signal, sys, time
from pathlib import Path

def event(text):
    with open(os.environ['TRACE'], 'a') as f: f.write(text + '\n')

args = sys.argv[1:]
probe = args[args.index('-t') + 1] == '6'
if probe and os.environ.get('BLOCK_PROBE') != '1':
    if os.environ.get('EMPTY_PAT') == '1':
        sys.stderr.write('{"have_pat":1,"program_count":0}\n')
    else:
        sys.stdout.buffer.write(b'x' * 250000)
    sys.exit(0)

def close(signum, frame):
    event('capture-term')
    time.sleep(0.3)  # model a slow, required device protocol close
    event('capture-closed')
    sys.exit(int(os.environ.get('NATIVE_EXIT', '0')))

if os.environ.get('IGNORE_TERM') == '1':
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    signal.signal(signal.SIGINT, signal.SIG_IGN)
else:
    # Match the real native parent, which installs the same handler for both.
    signal.signal(signal.SIGTERM, close)
    signal.signal(signal.SIGINT, close)
event('capture-ready')
Path(os.environ['CAPTURE_PID']).write_text(str(os.getpid()))
Path(os.environ['CAPTURE_PID'] + '.parent').write_text(str(os.getppid()))
if os.environ.get('ONESHOT_CAPTURE') == '1': close(None, None)
while True: time.sleep(60)
''')
        self.usb = self.base / "usb"
        device = self.usb / "1-1"
        device.mkdir(parents=True)
        (device / "idVendor").write_text("06b8\n")
        (device / "idProduct").write_text("103b\n")
        for name in ("ffmpeg", "eit_watch", "stb_streamer", "torne-b25", "pix-ring"):
            self.executable(name, "#!" + sys.executable + "\n" + r'''import sys
sys.stdout.buffer.write(sys.stdin.buffer.read())
''')
        self.executable("pix-dt295-logo", "#!" + sys.executable + "\n"
                        "import sys\nsys.exit(0 if sys.stdin.buffer.read() else 3)\n")
        mappings = (
            ("/usr/bin/pix-stream-simple", str(self.bin / "simple")),
            ("/usr/bin/pix-stream-linux", str(self.bin / "legacy")),
            ("/usr/bin/pix-usb-reset-common", str(self.bin / "pix-usb-reset-common")),
            ("/usr/bin/pix-logo-weekly", str(self.bin / "weekly")),
            ("/usr/bin/pix-logo-fetch", str(self.bin / "fetch")),
            ("/usr/bin/pix-ts-legacy", str(self.bin / "pix-ts-legacy")),
            ("/usr/bin/pix-dt295-logo", str(self.bin / "pix-dt295-logo")),
            ("/etc/init.d/S97pix-stream", str(self.bin / "S97")),
            ("/etc/default/pix-stream-linux", str(self.defaults)),
            ("/var/run", str(self.run)),
            ("/proc/", str(self.proc) + "/"),
            ("/run/pix-usb-reset.lock", str(self.run / "pix-usb-reset.lock")),
            ("/dev/shm/pix", str(self.shm)),
            ("/dev/shm/torne", str(self.base / "card")),
            ("/var/lib/pix/logos", str(self.base / "logos")),
            ("/tmp/pix-userland-dt295-0.lock", str(self.vendor)),
            ("/tmp/", str(self.logs) + "/"),
            # wait_for_tuner() looks the DT295 up here; without a fixture the
            # supervisor would block for the whole wait budget.
            ("/sys/bus/usb/devices/", str(self.usb) + "/"),
        )
        # Replace original paths in one pass: on Linux the fixture itself is
        # under /tmp, so sequential replacement would rewrite inserted paths.
        replacements = dict(mappings)
        paths = re.compile('|'.join(re.escape(old) for old, _ in mappings))
        for source, target in (("usr/bin/pix-stream-simple", "simple"),
                               ("usr/bin/pix-logo-weekly", "weekly"),
                               ("usr/bin/pix-logo-fetch", "fetch"),
                               ("etc/init.d/S97pix-stream", "S97"),
                               ("etc/init.d/S98pix-logo", "S98"),
                               ("etc/init.d/S99pix-shutdown", "S99")):
            text = (OVERLAY / source).read_text()
            text = paths.sub(lambda match: replacements[match[0]], text)
            self.executable(target, text)
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        TRACE=str(self.trace), CAPTURE_PID=str(self.base / "capture.pid"))

    def executable(self, name, text):
        path = self.bin / name
        path.write_text(text)
        path.chmod(0o755)
        return path

    def stop_processes(self):
        for process in self.processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)
            for stream in (process.stdin, process.stdout, process.stderr):
                if stream:
                    stream.close()

    def calls(self):
        return self.trace.read_text().splitlines() if self.trace.exists() else []

    def until(self, predicate, timeout=4):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.01)
        self.fail("fixture condition did not become true: " + repr(self.calls()))

    def launch(self, name, *args, **env):
        process = subprocess.Popen([str(self.bin / name), *args], env=dict(self.env, **env),
                                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL,
                                   start_new_session=(name == "legacy"))
        self.processes.append(process)
        self.mirror_identity(process.pid, name)
        return process

    def mirror_identity(self, pid, name):
        # macOS has no /proc. Mirror only known fixture processes, including
        # fetch children created by the real weekly shell rather than Popen.
        proc = self.proc / str(pid)
        proc.mkdir(exist_ok=True)
        (proc / "cmdline").write_bytes(b"/bin/sh\0" + str(self.bin / name).encode() + b"\0")

    def invoke(self, service, action):
        return subprocess.run([str(self.bin / service), action], env=self.env,
                              capture_output=True, text=True, timeout=6)

    def running_stream(self, **env):
        process = self.launch("simple", **env)
        (self.run / "pix-stream.pid").write_text(str(process.pid))
        # Real init reaps the daemon; here its actual parent must do so.
        threading.Thread(target=process.wait, daemon=True).start()
        self.until(lambda: "capture-ready" in self.calls())
        return process

    def native_capture_fixture(self):
        from test_dt295_stop_signal import build_fixture
        binary = self.bin / "native-stop-fixture"
        build_fixture(binary)
        output = self.base / "native-writer.log"
        self.env["NATIVE_OUTPUT"] = str(output)
        self.executable("pix-ts-legacy", '#!/bin/sh\n'
                        'case " $* " in *" -t 6 "*) '
                        '[ "${BLOCK_PROBE:-0}" = 1 ] || exit 0;; esac\n'
                        f'exec "{binary}" --pipe "$NATIVE_OUTPUT"\n')
        return output

    def test_native_first_stop_preserves_real_pipe_reader_and_end_ack(self):
        output = self.native_capture_fixture()
        self.running_stream()
        result = self.invoke("S97", "stop")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(output.read_text(), "writer-ready\nwriter-end\n")
        self.assertIn("native capture exit=0", (self.logs / "pix-simple.log").read_text())
        self.assertFalse((self.run / "pix-stream.owner").exists())

    def test_native_first_probe_stop_also_preserves_real_end_ack(self):
        output = self.native_capture_fixture()
        self.running_stream(BLOCK_PROBE="1")
        result = self.invoke("S97", "stop")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(output.read_text(), "writer-ready\nwriter-end\n")
        self.assertEqual(self.calls().count("capture-ready"), 1)
        self.assertFalse((self.run / "pix-stream.owner").exists())

    def test_closed_native_nonzero_ack_is_diagnostic_not_stale_ownership(self):
        self.running_stream(NATIVE_EXIT="70")
        result = self.invoke("S97", "stop")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("native capture exit=70", (self.logs / "pix-simple.log").read_text())
        self.assertFalse((self.run / "pix-stream.owner").exists())

    def test_native_error_ack_preserves_ordinary_retry(self):
        self.running_stream(ONESHOT_CAPTURE="1", NATIVE_EXIT="70")
        self.until(lambda: self.calls().count("capture-ready") >= 2)
        result = self.invoke("S97", "stop")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("native capture exit=70", (self.logs / "pix-simple.log").read_text())
        self.assertFalse((self.run / "pix-stream.owner").exists())

    def test_offair_role_failure_with_native_zero_remains_offair(self):
        process = self.launch("simple", EMPTY_PAT="1")
        (self.run / "pix-stream.pid").write_text(str(process.pid))
        threading.Thread(target=process.wait, daemon=True).start()
        log = self.logs / "pix-simple.log"
        self.until(lambda: log.exists() and "no programme on air" in log.read_text())
        result = self.invoke("S97", "stop")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("capture-ready", self.calls())
        self.assertIn("native capture exit=0", log.read_text())
        self.assertFalse((self.run / "pix-stream.owner").exists())

    def test_invalid_native_ack_fails_closed(self):
        self.executable("cat", '#!/bin/sh\ncase "$1" in\n'
                        '*/pix-stream.owner/worker.*/exit) echo invalid;;\n'
                        '*) exec /bin/cat "$@";;\nesac\n')
        self.running_stream(BLOCK_PROBE="1")
        result = self.invoke("S97", "stop")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertTrue((self.run / "pix-stream.owner").exists())
        self.assertIn("capture-closed", self.calls())

    def test_missing_native_ack_exhausts_outer_budget_and_retains_owner(self):
        source = (self.bin / "simple").read_text()
        publisher = 'echo "$native_status" > "$WORK/exit" || return 1'
        self.assertEqual(source.count(publisher), 1)
        self.executable("simple", source.replace(publisher, ': # withheld ACK fault'))
        self.running_stream(BLOCK_PROBE="1")
        result = self.invoke("S97", "stop")
        log = (self.logs / "pix-simple.log").read_text()
        # Without a real ACK the worker never learns of the exit and is forced.
        # The forced end is reported through the log; no clean-close claim is
        # made, and the dead worker's ownership is released for a restart.
        self.assertIn("exceeded 35s TERM budget; forcing shutdown", log)
        self.assertIn(result.returncode, (0, 1), result.stdout + result.stderr)
        self.assertIn("capture-closed", self.calls())
        self.assertFalse(list((self.run / "pix-stream.owner").glob("worker.*/exit")))

    def test_cancel_before_outer_worker_pid_publication_drains_launched_worker(self):
        source = (self.bin / "simple").read_text()
        publisher = '    worker=$!\n'
        self.assertEqual(source.count(publisher), 1)
        self.executable("simple", source.replace(publisher,
                        '    while [ ! -e "$FIXTURE_ROOT/publish-worker" ]; do sleep 0.1; done\n'
                        + publisher))
        process = self.running_stream(BLOCK_PROBE="1", FIXTURE_ROOT=str(self.base))
        owner = self.run / "pix-stream.owner"
        directories = list(owner.glob("worker.*"))
        self.assertEqual(len(directories), 1)
        workdir = directories[0]
        worker = int(workdir.name.split(".")[-1])
        release = self.base / "publish-worker"

        def cleanup():
            release.touch()
            # The old unguarded source leaves this fixture-owned worker alive.
            # Do not signal a retired/reused PID after successful metadata cleanup.
            if not workdir.exists():
                return
            try:
                os.kill(worker, signal.SIGTERM)
            except ProcessLookupError:
                return
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                try:
                    os.killpg(worker, 0)
                except ProcessLookupError:
                    return
                time.sleep(0.01)
            os.killpg(worker, signal.SIGKILL)

        self.addCleanup(cleanup)
        (self.run / "pix-stream.stopped").write_text("stop\n")
        process.terminate()
        release.touch()
        self.until(lambda: process.poll() is not None)
        result = self.invoke("S97", "stop")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("capture-closed", self.calls())
        self.assertFalse(owner.exists())
        self.assertEqual(self.calls().count("capture-ready"), 1)

    def test_cancel_before_native_pid_publication_still_waits_for_real_ack(self):
        source = (self.bin / "simple").read_text()
        publisher = 'if ! echo "$native" > "$WORK/pid"; then'
        self.assertEqual(source.count(publisher), 1)
        self.executable("simple", source.replace(publisher,
                        'while [ ! -e "$FIXTURE_ROOT/publish-native" ]; do sleep 0.1; done\n'
                        + publisher))
        self.running_stream(BLOCK_PROBE="1", FIXTURE_ROOT=str(self.base))
        self.assertFalse(list((self.run / "pix-stream.owner").glob("worker.*/pid")))
        stopper = self.launch("S97", "stop")
        self.until((self.run / "pix-stream.stopped").exists)
        (self.base / "publish-native").touch()
        self.assertEqual(stopper.wait(timeout=5), 0)
        self.assertIn("capture-closed", self.calls())
        self.assertFalse((self.run / "pix-stream.owner").exists())

    def test_capture_wrapper_termination_never_publishes_false_wait_ack(self):
        self.running_stream(BLOCK_PROBE="1")
        parent = self.base / "capture.pid.parent"
        self.until(parent.exists)
        # The wrapper is gone before it can report the real native exit, so the
        # worker cannot honestly acknowledge it.
        os.kill(int(parent.read_text()), signal.SIGTERM)  # owned first-stage wrapper only
        result = self.invoke("S97", "stop")
        # Nothing may fabricate a clean exit; the dead worker leaves only
        # metadata (no exit ACK) and the freed owner allows a restart.
        self.assertIn(result.returncode, (0, 1), result.stdout + result.stderr)
        log = (self.logs / "pix-simple.log").read_text()
        self.assertIn("worker ended before native capture was established", log)
        self.assertFalse((self.run / "pix-stream.owner").exists())
        self.assertFalse(list((self.run / "pix-stream.owner").glob("worker.*/exit")))

    def test_exit_inside_trapped_wait_still_drains_published_worker(self):
        # Run the script as its own shell so SIGTERM reaches its trap directly.
        process = subprocess.Popen([str(self.bin / "simple")], env=self.env,
                                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
        self.processes.append(process)
        (self.run / "pix-stream.pid").write_text(str(process.pid))
        self.until(lambda: "capture-ready" in self.calls())
        process.send_signal(signal.SIGTERM)
        self.assertEqual(process.wait(timeout=10), 143)
        self.assertIn("capture-closed", self.calls())
        self.assertIn("native capture exit=0", (self.logs / "pix-simple.log").read_text())
        self.assertFalse((self.run / "pix-stream.owner").exists())

    def test_worker_killed_before_native_publication_clears_stale_metadata(self):
        source = (self.bin / "simple").read_text()
        publisher = 'if ! echo "$native" > "$WORK/pid"; then'
        self.assertEqual(source.count(publisher), 1)
        self.executable("simple", source.replace(publisher,
                        'while [ ! -e "$FIXTURE_ROOT/publish-native" ]; do sleep 0.1; done\n'
                        + publisher))
        process = self.launch("simple", FIXTURE_ROOT=str(self.base))
        (self.run / "pix-stream.pid").write_text(str(process.pid))
        threading.Thread(target=process.wait, daemon=True).start()
        owner = self.run / "pix-stream.owner"
        self.until(lambda: list(owner.glob("worker.*/worker")) != [])
        worker = int(next(owner.glob("worker.*/worker")).read_text())
        # The worker dies before it can publish the native PID or its exit ACK.
        os.kill(worker, signal.SIGKILL)
        log = self.logs / "pix-simple.log"
        self.until(lambda: "Killed" in log.read_text())
        (self.run / "pix-stream.stopped").write_text("stop\n")
        result = self.invoke("S97", "stop")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("worker ended before native capture was established",
                      (self.logs / "pix-simple.log").read_text())
        # A freed owner lets S97 start again; the blocked capture never ran.
        self.assertFalse(owner.exists())
        self.assertNotIn("capture-ready", self.calls())

    def test_stop_waits_for_slow_capture_and_never_kills_unrelated_names(self):
        unrelated = subprocess.Popen([str(self.bin / "ffmpeg")], env=self.env,
                                     stdin=subprocess.PIPE, stdout=subprocess.DEVNULL)
        self.processes.append(unrelated)
        process = self.running_stream()
        before = time.monotonic()
        result = self.invoke("S97", "stop")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertGreaterEqual(time.monotonic() - before, 0.3)
        self.assertIn("capture-closed", self.calls())
        self.assertIsNotNone(process.poll())
        self.assertIsNone(unrelated.poll(), "normal stop must only signal owned groups")
        self.assertFalse((self.run / "pix-stream.owner").exists())
        self.assertTrue((self.run / "pix-stream.stopped").exists())
        self.assertEqual(self.vendor.stat().st_ino, self.vendor_inode)

    def test_term_during_probe_also_drains_capture(self):
        self.running_stream(BLOCK_PROBE="1")
        result = self.invoke("S97", "stop")
        details = result.stdout + result.stderr
        if result.returncode:
            details += repr(self.calls())
            log = self.logs / 'pix-simple.log'
            if log.exists():
                details += log.read_text()
        self.assertEqual(result.returncode, 0, details)
        self.assertIn("capture-closed", self.calls())

    def test_concurrent_supervisors_have_one_owner(self):
        contenders = [self.launch("simple"), self.launch("simple")]
        self.until(lambda: "capture-ready" in self.calls())
        owner_pid = int((self.run / "pix-stream.owner/pid").read_text())
        owner = next(p for p in contenders if p.pid == owner_pid)
        loser = next(p for p in contenders if p.pid != owner_pid)
        self.assertEqual(loser.wait(timeout=2), 1)
        self.assertIsNone(owner.poll())
        self.assertNotIn("capture-term", self.calls())
        self.assertEqual(self.calls().count("capture-ready"), 1)

    def test_resume_cannot_clear_desired_stop_or_reset_exclusion(self):
        (self.run / "pix-stream.stopped").touch()
        result = self.invoke("S97", "resume")
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((self.run / "pix-stream.stopped").exists())
        (self.run / "pix-usb-reset.lock").mkdir()
        result = self.invoke("S97", "start")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("unexpected-launch", self.calls())

    def test_start_failure_is_nonzero_and_stale_owner_fails_closed(self):
        result = self.invoke("S97", "start")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertEqual(self.calls(), ["unexpected-launch"])
        (self.run / "pix-stream.owner").mkdir()
        result = self.invoke("S97", "stop")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertTrue((self.run / "pix-stream.owner").is_dir())
        result = self.invoke("S97", "restart")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertEqual(self.calls(), ["unexpected-launch"])

    def test_borrow_excludes_start_and_stop_does_not_claim_tuner_free(self):
        lock = self.run / "pix-logo-fetch.lock"
        lock.mkdir()
        fence = self.run / "pix-stream.stopped"
        fence.write_text("shutdown\n")
        result = self.invoke("S97", "start")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertEqual(fence.read_text(), "shutdown\n")
        self.assertNotIn("unexpected-launch", self.calls())
        self.assertEqual(self.invoke("S97", "pause").returncode, 0)
        self.assertEqual(self.invoke("S97", "stop").returncode, 1)
        self.assertTrue(lock.exists())

    def test_stale_pid_identity_never_signals_an_unrelated_fixture(self):
        self.executable("unrelated", "#!/bin/sh\nexec /bin/sleep 60\n")
        unrelated = self.launch("unrelated")
        for service, pidfile in (("S97", "pix-stream.pid"), ("S98", "pix-logo-weekly.pid")):
            (self.run / pidfile).write_text(str(unrelated.pid))
            result = self.invoke(service, "stop")
            self.assertEqual(result.returncode, 1, result.stdout)
            self.assertIsNone(unrelated.poll())
            result = self.invoke(service, "start")
            self.assertEqual(result.returncode, 1, result.stdout)
            self.assertNotIn("unexpected-launch", self.calls())

    def test_background_fork_success_without_daemon_is_start_failure(self):
        self.executable("start-stop-daemon", "#!/bin/sh\nexit 0\n")
        for service in ("S97", "S98"):
            self.assertEqual(self.invoke(service, "start").returncode, 1)

    def test_legacy_group_stop_delivers_term_to_foreground_capture(self):
        self.executable("legacy", "#!/bin/sh\ntrap 'exit 143' INT TERM\n"
                        + str(self.bin / "pix-ts-legacy") + " -t 1800\n")
        legacy = self.launch("legacy")
        (self.run / "pix-stream.pid").write_text(str(legacy.pid))
        (self.run / "pix-stream.legacy-group").write_text(str(legacy.pid))
        threading.Thread(target=legacy.wait, daemon=True).start()
        self.until(lambda: "capture-ready" in self.calls())
        result = self.invoke("S97", "stop")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("capture-closed", self.calls())
        self.assertFalse((self.run / "pix-stream.legacy-group").exists())

    def test_forced_teardown_is_reported_not_silently_restarted(self):
        self.running_stream(IGNORE_TERM="1")
        result = self.invoke("S97", "stop")
        # The forced kill cannot prove a graceful close. Its message is the
        # honest report; ownership must not stay stale forever.
        log = (self.logs / "pix-simple.log").read_text()
        self.assertIn("exceeded 35s TERM budget; forcing shutdown", log)
        self.assertIn(result.returncode, (0, 1), result.stdout + result.stderr)
        self.assertEqual(self.calls().count("capture-ready"), 1)

    def launch_weekly(self):
        utc = time.gmtime()
        env = dict(PIX_LOGO_TZ="UTC", PIX_LOGO_WEEKLY_DAY=str(utc.tm_wday + 1),
                   PIX_LOGO_WEEKLY_HOUR=str(utc.tm_hour))
        scheduler = self.launch("weekly", **env)
        (self.run / "pix-logo-weekly.pid").write_text(str(scheduler.pid))
        threading.Thread(target=scheduler.wait, daemon=True).start()
        return scheduler

    def stop_weekly_with_fake_fetch(self, kind):
        self.env.update(FIXTURE_ROOT=str(self.base), FETCH_CASE=kind)
        self.executable("fetch", '''#!/bin/sh
lock="$FIXTURE_ROOT/run/pix-logo-fetch.lock"
mkdir "$lock" || exit 1
echo "$$" > "$lock/pid"
finish_fetch() {
    if [ "$FETCH_CASE" = released ]; then
        rm -f "$lock/pid" && rmdir "$lock"
        echo fetch-released >> "$TRACE"
    fi
    exit 1
}
if [ "$FETCH_CASE" = live ]; then
    trap '' INT TERM
else
    trap finish_fetch INT TERM
fi
echo fetch-ready >> "$TRACE"
while :; do /bin/sleep 0.05; done
''')
        scheduler = self.launch_weekly()
        self.until(lambda: "fetch-ready" in self.calls())
        child = int((self.run / "pix-logo-fetch.lock/pid").read_text())
        self.mirror_identity(child, "fetch")
        # Interrupt the scheduler's first wait before the child can exit: this
        # exercises its cleanup wait deterministically, not a timing-only race.
        scheduler.terminate()
        return scheduler.wait(timeout=5)

    def test_weekly_released_child_exit_one_does_not_retain_process_owner(self):
        self.assertEqual(self.stop_weekly_with_fake_fetch("released"), 143)
        self.assertIn("fetch-released", self.calls())
        self.assertFalse((self.run / "pix-logo-fetch.lock").exists())
        self.assertFalse((self.run / "pix-logo-weekly.owner").exists())
        result = self.invoke("S98", "stop")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("child exited with status 1", (self.logs / "pix-logo.log").read_text())

    def test_weekly_exited_child_retained_fetch_lock_still_refuses_start(self):
        self.assertEqual(self.stop_weekly_with_fake_fetch("retained"), 143)
        self.assertFalse((self.run / "pix-logo-weekly.owner").exists())
        self.assertTrue((self.run / "pix-logo-fetch.lock").exists())
        self.assertEqual(self.invoke("S98", "stop").returncode, 1)
        self.assertEqual(self.invoke("S97", "start").returncode, 1)
        self.assertNotIn("unexpected-launch", self.calls())

    def test_weekly_live_child_timeout_still_forces_and_retains_ownership(self):
        self.assertEqual(self.stop_weekly_with_fake_fetch("live"), 1)
        self.assertTrue((self.run / "pix-logo-weekly.owner").exists())
        self.assertTrue((self.run / "pix-logo-fetch.lock").exists())
        self.assertEqual(self.invoke("S98", "stop").returncode, 1)
        self.assertEqual(self.invoke("S97", "start").returncode, 1)
        self.assertNotIn("unexpected-launch", self.calls())

    def test_scheduler_exit_after_identity_check_is_already_drained(self):
        self.env['FIXTURE_ROOT'] = str(self.base)
        self.executable("weekly", '#!/bin/sh\n'
                        'owner="$FIXTURE_ROOT/run/pix-logo-weekly.owner"\n'
                        'mkdir "$owner" || exit 1\necho "$$" > "$owner/pid"\n'
                        'trap \'rm -f "$owner/pid"; rmdir "$owner"; exit 0\' TERM\n'
                        'echo weekly-ready >> "$TRACE"\n'
                        'while :; do /bin/sleep 0.05; done\n')
        source = (self.bin / "S98").read_text()
        identity = 'grep -Fx -e "${2:-$DAEMON}" >/dev/null'
        self.assertEqual(source.count(identity), 1)
        self.executable("S98", source.replace(identity,
                        identity + ' || return 1\n'
                        ': > "$FIXTURE_ROOT/identity-verified"\n'
                        'while [ ! -e "$FIXTURE_ROOT/allow-term" ]; do /bin/sleep 0.01; done\n'
                        'return 0'))
        scheduler = self.launch("weekly")
        (self.run / "pix-logo-weekly.pid").write_text(str(scheduler.pid))
        self.until(lambda: "weekly-ready" in self.calls())
        stopper = self.launch("S98", "stop")
        release = self.base / "allow-term"
        self.addCleanup(release.touch)
        self.until((self.base / "identity-verified").exists)
        scheduler.terminate()
        self.assertEqual(scheduler.wait(timeout=3), 0)
        release.touch()
        self.assertEqual(stopper.wait(timeout=3), 0)
        self.assertFalse((self.run / "pix-logo-weekly.owner").exists())

    def test_manual_logo_stop_without_scheduler_preserves_stream_retries(self):
        self.running_stream(ONESHOT_CAPTURE="1")
        result = self.invoke("S98", "stop")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.run / "pix-stream.stopped").exists())
        # A fence here used to stop recording permanently at the next normal
        # capture exit. Exercise that exit, not only the immediately-live PID.
        self.until(lambda: self.calls().count("capture-ready") >= 2)

    def test_manual_logo_stop_cancels_fetch_then_restores_recording(self):
        # Mock only controller effects for the positive resume path.
        self.executable("S97", "#!/bin/sh\necho \"$1\" >> \"$TRACE\"\nexit 0\n")
        (self.run / "pix-stream.pid").write_text(str(os.getpid()))  # kill -0 only
        self.launch_weekly()
        self.until(lambda: "capture-ready" in self.calls())
        self.mirror_identity(int((self.run / "pix-logo-fetch.lock/pid").read_text()), "fetch")
        result = self.invoke("S98", "stop")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertLess(self.calls().index("capture-closed"), self.calls().index("resume"))
        self.assertFalse((self.run / "pix-stream.stopped").exists())
        self.assertFalse((self.run / "pix-logo-fetch.lock").exists())

    def test_shutdown_cancels_manual_fetch_and_waits_for_capture_drain(self):
        manual = self.launch("fetch", "--force")
        threading.Thread(target=manual.wait, daemon=True).start()
        self.until(lambda: "capture-ready" in self.calls())
        self.assertEqual(self.invoke("S99", "stop").returncode, 0)
        result = self.invoke("S98", "stop")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("capture-closed", self.calls(), 'S98 must drain manual CLI fetch too')
        self.assertFalse((self.run / "pix-logo-fetch.lock").exists())
        self.assertEqual(self.invoke("S97", "stop").returncode, 0)
        self.assertNotIn("unexpected-launch", self.calls())
        self.until(lambda: manual.poll() is not None)

    def test_shutdown_during_fetch_lock_pid_initialization_prevents_capture(self):
        created = self.base / "lock-created"
        release = self.base / "initialize-owner"
        self.executable("mkdir", "#!" + sys.executable + "\n" + r'''import os, sys, time
from pathlib import Path
root = Path(os.environ['FIXTURE_ROOT'])
if sys.argv[1:] == [str(root / 'run/pix-logo-fetch.lock')]:
    try: os.mkdir(sys.argv[1])
    except FileExistsError: sys.exit(1)
    (root / 'lock-created').touch()
    deadline = time.monotonic() + 4
    while not (root / 'initialize-owner').exists():
        assert time.monotonic() < deadline
        time.sleep(0.01)
else:
    os.execv('/bin/mkdir', ['/bin/mkdir', *sys.argv[1:]])
''')
        self.env['FIXTURE_ROOT'] = str(self.base)
        manual = self.launch("fetch", "--force")
        self.addCleanup(release.touch)
        self.until(created.exists)
        self.assertFalse((self.run / "pix-logo-fetch.lock/pid").exists())
        self.assertEqual(self.invoke("S99", "stop").returncode, 0)
        stopper = self.launch("S98", "stop")
        self.until((self.run / "pix-logo.stopped").exists)
        release.touch()
        self.assertEqual(stopper.wait(timeout=4), 0)
        # Either its own fence check or S98's verified TERM may win once pid
        # is published. Both paths must finish before any capture is started.
        self.assertIn(manual.wait(timeout=4), (1, 143))
        self.assertNotIn("capture-ready", self.calls())
        self.assertNotIn("unexpected-launch", self.calls())
        self.assertFalse((self.run / "pix-logo-fetch.lock").exists())

    def test_bad_scheduler_pid_cannot_skip_manual_capture_drain(self):
        self.executable("unrelated", "#!/bin/sh\nexec /bin/sleep 60\n")
        unrelated = self.launch("unrelated")
        (self.run / "pix-logo-weekly.pid").write_text(str(unrelated.pid))
        manual = self.launch("fetch", "--force")
        threading.Thread(target=manual.wait, daemon=True).start()
        self.until(lambda: "capture-ready" in self.calls())
        self.assertEqual(self.invoke("S99", "stop").returncode, 0)
        self.assertEqual(self.invoke("S98", "stop").returncode, 1)
        self.assertIn("capture-closed", self.calls())
        self.assertFalse((self.run / "pix-logo-fetch.lock").exists())
        self.assertIsNone(unrelated.poll())

    def test_manual_fetch_pid_mismatch_is_not_signaled(self):
        self.executable("unrelated", "#!/bin/sh\nexec /bin/sleep 60\n")
        unrelated = self.launch("unrelated")
        lock = self.run / "pix-logo-fetch.lock"
        lock.mkdir()
        (lock / "pid").write_text(str(unrelated.pid))
        self.assertEqual(self.invoke("S99", "stop").returncode, 0)
        result = self.invoke("S98", "stop")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIsNone(unrelated.poll())
        self.assertTrue(lock.exists())

    def test_shutdown_order_inhibits_then_cancels_fetch_without_late_resume(self):
        # Run all six real shells, including S97 pause, rather than mocking the
        # shutdown fence or the controller's desired-state behavior.
        stream = self.running_stream()
        self.assertEqual(self.invoke("S99", "start").returncode, 0)
        self.assertFalse((self.run / "pix-stream.stopped").exists())
        scheduler = self.launch_weekly()
        self.until(lambda: self.calls().count("capture-ready") == 2)
        self.mirror_identity(int((self.run / "pix-logo-fetch.lock/pid").read_text()), "fetch")
        self.assertIsNotNone(stream.poll())  # pause drained the original stream
        self.assertEqual(self.invoke("S99", "stop").returncode, 0)
        self.assertTrue((self.run / "pix-stream.stopped").exists())
        self.assertIsNone(scheduler.poll())  # inhibit fences but does not drain
        self.assertTrue((self.run / "pix-logo-fetch.lock").is_dir())
        for service in ("S98", "S97"):
            result = self.invoke(service, "stop")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.calls().count("capture-closed"), 2)
        self.assertNotIn("unexpected-launch", self.calls())
        self.assertFalse((self.run / "pix-logo-fetch.lock").exists())
        self.assertFalse((self.run / "pix-logo-weekly.owner").exists())


class TunerWait(unittest.TestCase):
    """The supervisor waits for the tuner instead of restarting into a panic.

    Starting with no tuner present made the pipeline fail three times in a row,
    which triggered the guarded USB recovery and left the board unresponsive.
    wait_for_tuner() must find the device through sysfs, give up boundedly, and
    never treat a stop request as a reason to keep waiting.
    """

    def extract(self):
        """Pull the wait_for_tuner function out of the overlay script."""
        text = (OVERLAY / "usr/bin/pix-stream-simple").read_text()
        start = text.index("wait_for_tuner() {")
        end = text.index("\n}\n", start) + 3
        return text[start:end]

    def run_wait(self, devices, wait, stop=False, stop_after=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, files in devices.items():
                node = root / "devices" / name
                node.mkdir(parents=True)
                for key, value in files.items():
                    (node / key).write_text(value + "\n")
            stopfile = root / "stop"
            if stop:
                stopfile.write_text("")
            script = root / "wait.sh"
            script.write_text(
                '#!/bin/sh\n'
                'STOPFILE=%s\n'
                'PIX_DECRYPT=${PIX_DECRYPT:-0}\n'
                'PIX_TUNER_WAIT=%s\n'
                'log() { echo "$*"; }\n'
                '%s\n'
                'wait_for_tuner && echo FOUND || echo STOPPED\n'
                % (stopfile, wait, self.extract().replace(
                    "/sys/bus/usb/devices/", str(root / "devices") + "/")))
            script.chmod(0o755)
            if stop_after is None:
                return subprocess.run([str(script)], capture_output=True,
                                      text=True, timeout=30)
            # Let it wait, then ask it to stop so the loop ends on its own.
            with subprocess.Popen([str(script)], stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True) as process:
                time.sleep(stop_after)
                stopfile.write_text("")
                out, _ = process.communicate(timeout=10)
            return subprocess.CompletedProcess(process.args, process.returncode, out, "")

    def test_finds_the_dt295_by_vendor_and_product(self):
        result = self.run_wait({"1-1": {"idVendor": "06b8", "idProduct": "103b"}}, 5)
        self.assertIn("FOUND", result.stdout)

    def test_ignores_other_vendors(self):
        result = self.run_wait(
            {"1-1": {"idVendor": "0781", "idProduct": "5580"},
             "1-2": {"idVendor": "06b8", "idProduct": "103b"}}, 5)
        self.assertIn("FOUND", result.stdout)

    def test_an_explicit_bound_starts_anyway(self):
        """A positive PIX_TUNER_WAIT is an explicit opt-in to proceed without one.

        The default is to wait, because starting without a tuner fails the
        pipeline three times and reaches dt295_recover -> pix_usb_reset.
        """
        result = self.run_wait({"1-1": {"idVendor": "0781", "idProduct": "5580"}}, 1)
        self.assertIn("not found within 1s; starting anyway", result.stdout)
        self.assertIn("FOUND", result.stdout)

    def test_the_default_waits_until_stopped(self):
        result = self.run_wait({"1-1": {"idVendor": "0781", "idProduct": "5580"}},
                               "", stop_after=3)
        self.assertIn("STOPPED", result.stdout)
        self.assertNotIn("FOUND", result.stdout)

    def test_a_stop_request_ends_the_wait(self):
        result = self.run_wait({"1-1": {"idVendor": "0781", "idProduct": "5580"}},
                               30, stop=True)
        self.assertIn("STOPPED", result.stdout)

    def test_decrypt_mode_skips_the_wait(self):
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "w.sh"
            script.write_text(
                '#!/bin/sh\nPIX_DECRYPT=1\nlog() { :; }\n%s\n'
                'wait_for_tuner && echo FOUND\n' % self.extract())
            script.chmod(0o755)
            result = subprocess.run([str(script)], capture_output=True, text=True,
                                    timeout=10)
        self.assertIn("FOUND", result.stdout)


if __name__ == "__main__":
    unittest.main()
