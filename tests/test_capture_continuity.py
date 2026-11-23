#!/usr/bin/python3
"""The stage-1 contract: capture is not cut down every 1800 seconds.

Covers three properties the design depends on
(docs/dt295-external-descramble-design.md, stage 1):

  1. capture_seconds normalises the -t value so ``-`` (unbounded) reaches
     pix-ts-legacy, and an empty or zero setting can no longer turn into the
     ``-t ""`` / ``-t 0`` usage error that starts no pipeline at all.
  2. the DT295 USB recovery in the supervisor loop is gated on the tuner, not
     on PIX_DECRYPT, so enabling external descrambling does not silently
     remove the only automatic way back from a broken firmware state.
  3. unit 2's defaults stop cutting the capture and stop filtering PIDs.

The shell under test is the real launcher text, sliced the same way the other
launcher tests slice it; only external commands are replaced.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SIMPLE = ROOT / "board/x96max/overlay/usr/bin/pix-stream-simple"
UNIT2 = ROOT / "board/x96max/ramboot-overlay/etc/default/pix-stream-linux"


def extract(source, start, end):
    """Slice from `start` up to the first `end` found after it.

    `source.index(end)` alone would match inside the opening line when the
    end marker is "}\n": the "} {" of "name() {" is not a body terminator.
    """
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


class CaptureSecondsTests(unittest.TestCase):
    """The -t value handed to pix-ts-legacy."""

    def setUp(self):
        source = SIMPLE.read_text()
        self.fn = extract(source, "capture_seconds() {", "\n}\n") + "\n}\n"

    def capture_seconds(self, value):
        env = {k: v for k, v in os.environ.items() if not k.startswith("PIX_")}
        if value is not None:
            env["PIX_CAPTURE_SECONDS"] = value
        script = self.fn + "capture_seconds\n"
        result = subprocess.run(["/bin/sh", "-c", script], env=env, text=True,
                                capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.strip()

    def test_dash_stays_unbounded(self):
        self.assertEqual(self.capture_seconds("-"), "-")

    def test_finite_seconds_are_preserved(self):
        for value in ("1800", "300", "86400"):
            with self.subTest(value=value):
                self.assertEqual(self.capture_seconds(value), value)

    def test_empty_and_zero_become_unbounded_not_a_usage_error(self):
        # pix-ts-legacy's number() rejects "" and 0 (main.c:168-178, 401-406).
        # Passing either through starts no pipeline, so both must normalise.
        for value in ("", "0"):
            with self.subTest(value=value):
                self.assertEqual(self.capture_seconds(value), "-")

    def test_unset_defaults_to_unbounded(self):
        self.assertEqual(self.capture_seconds(None), "-")

    def test_leading_zero_is_not_rewritten(self):
        # strtoul(...,0) parses 011 as octal; the launcher must not "fix" it
        # silently. The defaults file forbids it instead (documented there).
        self.assertEqual(self.capture_seconds("011"), "011")


class LauncherPassesNormalisedSecondsTests(unittest.TestCase):
    """Both capture paths must use the same normalised value."""

    def setUp(self):
        self.source = SIMPLE.read_text()

    def test_no_path_passes_the_raw_setting(self):
        # A raw "$PIX_CAPTURE_SECONDS" would reintroduce -t "" / -t 0.
        offenders = [line for line in self.source.splitlines()
                     if '-t' in line and 'PIX_CAPTURE_SECONDS' in line
                     and 'capture_seconds' not in line]
        self.assertEqual(offenders, [])

    def test_every_capture_passes_through_capture_seconds(self):
        # run_buffered now goes through raw_ts_source, which picks the vendor
        # path (bounded by capture_seconds) or the CFW path (unbounded by
        # design). A raw setting anywhere is caught by the test above.
        direct = extract(self.source, "run_direct() {", "\n}\n")
        self.assertIn('"$(capture_seconds)"', direct)
        raw = extract(self.source, "raw_ts_source() {", "\n}\n")
        self.assertIn('"$(capture_seconds)"', raw)
        # The CFW branch must not pass a -t at all: pix-cfw-ts streams until
        # stopped, and handing it a timeout would reintroduce the cut.
        cfw_branch = raw[:raw.index(";;") + 2]
        self.assertIn("dt295-cfw", cfw_branch)
        # "-t" as a flag, not the "-t" inside the tuner name "dt295-cfw".
        self.assertNotIn("-t ", cfw_branch)
        self.assertNotIn('"-t"', cfw_branch)

    def test_direct_path_keeps_the_original_output_chain(self):
        # Only the -t value may change on the direct path. Programme metadata,
        # features and the streamer chain must stay exactly as they were, or
        # programme names, CM sideband and the NAS output format shift too.
        body = extract(self.source, "run_direct() {", "\n}\n")
        self.assertIn("-t \"$(capture_seconds)\"", body)
        self.assertIn("eit_watch --state /dev/shm/pix/program.txt", body)
        self.assertIn('--logo-dir "$PIX_LOGO_DIR"', body)
        self.assertIn("| encode | fragment | streamer", body)


class Dt295RecoveryIsNotGatedOnDecryptTests(unittest.TestCase):
    """PIX_DECRYPT must not disable DT295 USB recovery."""

    def setUp(self):
        self.source = SIMPLE.read_text()
        self.loop = self.source[self.source.index("while [ ! -e \"$STOPFILE\" ]"):]

    def test_recovery_calls_are_not_guarded_by_pix_decrypt(self):
        for line in self.loop.splitlines():
            if "dt295_recover" not in line:
                continue
            self.assertNotIn("PIX_DECRYPT", line,
                             "DT295 recovery must not depend on PIX_DECRYPT: " + line)

    def test_recovery_is_gated_on_the_tuner(self):
        # The dt295 branch must stay reachable for dt295 only. The guard sits
        # on the enclosing if/elif, so track the condition that opens the block
        # each dt295_recover call belongs to.
        guarded = 0
        condition = None
        for line in self.loop.splitlines():
            stripped = line.strip()
            if stripped.startswith(("if ", "elif ")) and stripped.endswith("then"):
                condition = stripped
            if "dt295_recover ||" in stripped:
                self.assertIsNotNone(condition, "dt295_recover outside a guarded block")
                self.assertIn('"$PIX_TUNER" = "dt295"', condition,
                              "DT295 recovery is not gated on the tuner: " + condition)
                guarded += 1
        self.assertEqual(guarded, 2, "expected both recovery branches")

    def test_external_descrambling_still_reaches_recovery(self):
        # Drive the supervisor loop's recovery block itself, with PIX_DECRYPT=1
        # set, and require the guarded reset to run. Stubbing the whole worker
        # pipeline would measure teardown instead of the gate being tested.
        # The loop contains nested if/fi and while/done pairs, so slice to the
        # outer terminator: the last line that is exactly "done".
        loop_at = self.source.index('while [ ! -e "$STOPFILE" ]; do')
        tail = self.source.index('\ndone\n', loop_at) + len('\ndone\n')
        block = self.source[loop_at:tail]
        with tempfile.TemporaryDirectory(prefix="pix-recovery-") as tmp:
            base = Path(tmp)
            events = base / "events"
            log = base / "pix-ts.log"
            log.write_text("firmware boot requires cold state 0 or unauthenticated state 16\n")
            script = block.replace("/tmp/pix-ts.log", shlex.quote(str(log)))
            prefix = f"""
PIX_TUNER=dt295
PIX_DECRYPT=1
PIX_CAPTURE_SECONDS=-
STOPFILE={shlex.quote(str(base / 'never.stopped'))}
fast_fail=0
feature_failures=0
cleanup_failed=0
FEAT_ARGS=
log() {{ :; }}
wait_for_tuner() {{ return 0; }}
run_worker() {{ return 1; }}
probe() {{ return 1; }}
pause_retry() {{ :; }}
date() {{ echo 1000; }}
dt295_recover() {{ echo recovered >> {shlex.quote(str(events))}; exit 0; }}
"""
            result = subprocess.run(["/bin/sh", "-c", prefix + script], text=True,
                                    capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(events.exists(), "DT295 recovery did not run under PIX_DECRYPT=1")
            self.assertIn("recovered", events.read_text())
    def test_other_tuners_never_trigger_dt295_usb_recovery(self):
        # The gate narrowed the trigger from "not PIX_DECRYPT" to "is dt295".
        # A pxs1ud unit must not unbind/bind the USB controller: that recovery
        # is DT295-specific and has left the board unresponsive before.
        loop_at = self.source.index('while [ ! -e "$STOPFILE" ]; do')
        tail = self.source.index('\ndone\n', loop_at) + len('\ndone\n')
        block = self.source[loop_at:tail]
        with tempfile.TemporaryDirectory(prefix="pix-norecover-") as tmp:
            base = Path(tmp)
            events = base / "events"
            log = base / "pix-ts.log"
            log.write_text("firmware boot requires cold state 0 or unauthenticated state 16\n")
            script = block.replace("/tmp/pix-ts.log", shlex.quote(str(log)))
            prefix = f"""
PIX_TUNER=pxs1ud
PIX_DECRYPT=0
PIX_CAPTURE_SECONDS=-
STOPFILE={shlex.quote(str(base / 'stop'))}
fast_fail=9
feature_failures=0
cleanup_failed=0
FEAT_ARGS=
log() {{ :; }}
wait_for_tuner() {{ return 0; }}
# The loop is endless by design when recovery never fires, so stop it here.
# The probe branch must see success, or the loop takes the broadcast-suspension
# path and waits 5 minutes instead of reaching the recovery block.
run_worker() {{ PASSES=$((${{PASSES:-0}} + 1)); [ "$PASSES" -lt 3 ] && return 0; exit 0; }}
pause_retry() {{ :; }}
date() {{ echo 1000; }}
dt295_recover() {{ echo recovered >> {shlex.quote(str(events))}; :; }}
"""
            result = subprocess.run(["/bin/sh", "-c", prefix + script], text=True,
                                    capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertFalse(events.exists(),
                             "a non-dt295 unit must never run the DT295 USB reset")


class Unit2DefaultsTests(unittest.TestCase):
    """Unit 2's deployed defaults."""

    def setUp(self):
        self.text = UNIT2.read_text()
        self.values = dict(line.split("=", 1) for line in self.text.splitlines()
                           if re.match(r"^PIX_[A-Z0-9_]+=", line))

    def test_capture_is_not_cut_periodically(self):
        self.assertEqual(self.values["PIX_CAPTURE_SECONDS"], "-")

    def test_pid_filter_is_not_set(self):
        # ARIB keys are per ECM; filtering can drop an ECM and break
        # descrambling (and, with the card path, the card session).
        self.assertNotIn("PIX_PIDS", self.values)
        self.assertNotIn("PIX_PIDS=", self.text)

    def test_satellite_slot_is_kept(self):
        self.assertEqual(self.values["PIX_SAT_SLOT"], "0")

    def test_tuner_mapping_is_unchanged(self):
        # Unit 2 runs the CFW path (round: 2026-11-20): the tuner hands over
        # the raw stream and torne-b25 descrambles with the external B-CAS.
        # The satellite channel moved with the 2026-11 BS repack; the CFW
        # channel is "transponder/TSID", not the stock channel number.
        self.assertEqual(self.values["PIX_TUNER"], "dt295-cfw")
        self.assertEqual(self.values["PIX_SYSTEM"], "bs")
        self.assertEqual(self.values["PIX_CHANNEL"], "9")
        self.assertEqual(self.values["PIX_CFW_CHANNEL"], "BS9/0x4090")

    def test_defaults_file_is_shell_readable(self):
        result = subprocess.run(
            ["/bin/sh", "-c",
             f'. {shlex.quote(str(UNIT2))}; printf "%s" "$PIX_CAPTURE_SECONDS"'],
            text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "-")


if __name__ == "__main__":
    unittest.main()
