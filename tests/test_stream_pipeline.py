#!/usr/bin/python3
"""Execute the legacy launcher's shell seams with byte/argv-recording mocks.

No launcher startup, device, card socket, sysfs, network, or real codec is used.
The production defaults, feature setup, tuner source, common output chain and
run_pipeline body are evaluated by /bin/sh; only external boundaries are fake.
"""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / "board/x96max/overlay/usr/bin/pix-stream-linux"
MOCK = r'''#!/usr/bin/python3
import json
import os
from pathlib import Path
import sys

command = Path(sys.argv[0]).name
args = sys.argv[1:]
stage = command
if command == "ffmpeg":
    if "h264_v4l2m2m" in args:
        stage = "encode"
    elif "mp4" in args:
        stage = "fragment"
    else:
        stage = "resync"
if command in ("siano-ts", "torne-c", "pix-ts-legacy"):
    data = ""
    output = command + "\n"
elif command == "tail":
    data = Path(args[-1]).read_text()
    output = data
else:
    data = sys.stdin.read()
    output = data + stage + "\n"
if command == "eit_watch":
    Path(args[args.index("--state") + 1]).write_text("fixture programme\n")
record = json.dumps(dict(command=command, stage=stage, args=args, input=data)) + "\n"
fd = os.open(os.environ["PIPELINE_TRACE"], os.O_CREAT | os.O_WRONLY | os.O_APPEND, 0o600)
os.write(fd, record.encode())
os.close(fd)
if command != "stb_streamer":
    sys.stdout.write(output)
'''


def values(args, flag):
    return [args[i + 1] for i, arg in enumerate(args[:-1]) if arg == flag]


class StreamPipelineTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="pix-stream-pipeline-")
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.runtime = self.base / "shm"
        (self.runtime / "pix").mkdir(parents=True)
        self.logs = self.base / "logs"
        self.logs.mkdir()
        self.trace = self.base / "trace.jsonl"
        fakebin = self.base / "bin"
        fakebin.mkdir()
        for command in ("siano-ts", "torne-c", "pix-ts-legacy", "torne-b25",
                        "eit_watch", "ffmpeg", "stb_streamer", "tail"):
            mock = fakebin / command
            mock.write_text(MOCK)
            mock.chmod(0o700)
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith("PIX_")}
        self.env.update(
            PATH=str(fakebin) + ":/usr/bin:/bin",
            PIPELINE_TRACE=str(self.trace),
            PIX_CHANNEL="17", PIX_TUNER="pxs1ud", PIX_TUNER_DEV="9:7",
            PIX_FIRMWARE=str(self.base / "fixture-firmware"),
            PIX_CARD_SOCKET=str(self.base / "fixture-card.sock"),
            PIX_DT295_ASSETS=str(self.base / "fixture-assets"),
            PIX_START_DELAY="0", PIX_LOG_DIR=str(self.logs),
            PIX_LOGO_DIR=str(self.base / "logos"), PIX_PORT="40774",
            PIX_ALLOWED_IP="192.0.2.8,192.0.2.9", PIX_SPOOL_MB="192",
            PIX_DISPLAY="unit fixture", PIX_FEATURE_MAX_MB="384",
            PIX_LIVE_FILE=str(self.runtime / "pix/live.ts"),
        )
        source = LAUNCHER.read_text()
        # Exclude startup/cleanup/lock ownership entirely. Keep real shell
        # function bodies and feature setup, not hand-written replicas.
        self.defaults = source[source.index(': "${PIX_CHANNEL:='):
                               source.index('\nlog() {')]
        self.device_setup = source[source.index('DEV="${PIX_TUNER_DEV:-}"'):
                                   source.index('if [ "$PIX_TUNER" = "pxs1ud" ] && [ -z "$DEV" ]; then')]
        self.features = source[source.index('if [ "$PIX_DEINTERLACE" = "1" ]; then'):
                               source.index('\nlog "tuner=')]
        self.pipeline = source[source.index('ts_source() {'):
                               source.index('\nfeature_failures=0')]
        self.retry_loop = source[source.index('feature_failures=0'):]

    def run_pipeline(self, tuner="pxs1ud", features=True, retry=False, **settings):
        env = dict(self.env, PIX_TUNER=tuner, PIX_FEATURES="1" if features else "0")
        env.update(settings)
        # All runtime absolute paths live inside this temporary fixture. The
        # watchdog exits at its first sleep, so it cannot poll or signal the host.
        # The only waiting is bounded producer startup for the DT295 file seam.
        prefix = '''
log() { :; }
kill_stale() { :; }
ps() { :; }
kill() { :; }
date() { echo 1000; }
sleep() {
    case "$1" in
        10) exit 0 ;;
        0) : ;;
        *) /bin/sleep 0.02 ;;
    esac
}
'''
        script = prefix + self.defaults + self.device_setup + self.features + self.pipeline
        script += self.retry_loop if retry else '\nrun_pipeline\n'
        script = script.replace('/dev/shm/', str(self.runtime) + '/')
        result = subprocess.run(["/bin/sh", "-c", script], env=env, text=True,
                                capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertTrue(self.trace.exists(), "pipeline invoked no mock stages")
        return [json.loads(line) for line in self.trace.read_text().splitlines()]

    def stage(self, events, stage):
        matches = [event for event in events if event["stage"] == stage]
        self.assertEqual(len(matches), 1, (stage, events))
        return matches[0]

    def assert_output_chain(self, events, source, features, buffered=False, descrambled=False):
        suffix = [] if descrambled else (["resync", "torne-b25"] if buffered else ["torne-b25"])
        clear = "\n".join([source] + suffix) + "\n"
        eit = self.stage(events, "eit_watch")
        self.assertEqual(eit["input"], clear)
        state = self.runtime / "pix/program.txt"
        self.assertEqual(eit["args"], ["--state", str(state), "--logo-dir", self.env["PIX_LOGO_DIR"]])
        self.assertEqual(state.read_text(), "fixture programme\n")

        encode = self.stage(events, "encode")
        args = encode["args"]
        self.assertEqual(encode["input"], clear + "eit_watch\n")
        self.assertEqual(values(args, "-probesize"), ["2000000" if buffered else "400000"])
        self.assertEqual(values(args, "-analyzeduration"), ["3000000" if buffered else "500000"])
        # Check maps/codecs per output, rather than allowing a duplicate raw
        # video map to hide behind a correct [main] feature-graph map.
        main = args[:args.index("pipe:1")]
        self.assertEqual(values(main, "-c:v"), ["mpeg2_v4l2m2m", "h264_v4l2m2m"])
        self.assertEqual(values(args, "-c:v"),
                         ["mpeg2_v4l2m2m", "h264_v4l2m2m"] + (["rawvideo"] if features else []))
        self.assertEqual(values(main, "-map"), ["[main]" if features else "0:v:0", "0:a:0"])
        self.assertEqual(values(main, "-c:a"), ["copy"])
        self.assertIn("-copyts", main)
        self.assertIn("-start_at_zero", main)
        self.assertEqual(values(main, "-b:v"), ["6000k"])
        self.assertEqual(values(main, "-g"), ["60"])
        if features:
            self.assertEqual(len(values(args, "-filter_complex")), 1)
            graph = values(args, "-filter_complex")[0]
            self.assertIn("split=2[main][an]", graph)
            self.assertIn("lavfi.signalstats.YDIF", graph)
            self.assertNotIn("-vf", args)
            self.assertEqual(values(args, "-map"), ["[main]", "0:a:0", "[aux]", "0:a:0"])
            self.assertEqual(values(args, "-c:a"), ["copy", "pcm_s16le"])
            self.assertEqual(values(args, "-f"), ["mpegts", "mpegts", "null", "null"])
        else:
            self.assertNotIn("-filter_complex", args)
            self.assertEqual(len(values(args, "-vf")), 1)
            self.assertEqual(values(args, "-map"), ["0:v:0", "0:a:0"])
            self.assertNotIn("null", args)

        fragment = self.stage(events, "fragment")
        self.assertEqual(fragment["input"], clear + "eit_watch\nencode\n")
        self.assertEqual(values(fragment["args"], "-map"), ["0:v", "0:a"])
        self.assertEqual(values(fragment["args"], "-c"), ["copy"])
        self.assertEqual(values(fragment["args"], "-bsf:a"), ["aac_adtstoasc"])
        self.assertEqual(values(fragment["args"], "-movflags"),
                         ["+frag_keyframe+empty_moov+default_base_moof"])
        streamer = self.stage(events, "stb_streamer")
        self.assertEqual(streamer["input"], clear + "eit_watch\nencode\nfragment\n")
        args = streamer["args"]
        for flag, expected in (("--port", "40774"), ("--allowed-ip", self.env["PIX_ALLOWED_IP"]),
                               ("--spool-mb", "192"), ("--display", "unit fixture"),
                               ("--program-file", str(state)), ("--logo-dir", self.env["PIX_LOGO_DIR"])):
            self.assertEqual(values(args, flag), [expected])
        feature_flags = {"--feature-file": str(self.logs / "pix-ff1.log"),
                         "--feature-audio-file": str(self.runtime / "pix/feat-a.log"),
                         "--feature-black-file": str(self.logs / "pix-ff1.log"),
                         "--feature-max-mb": "384"}
        for flag, expected in feature_flags.items():
            self.assertEqual(values(args, flag), [expected] if features else [])

    def assert_direct(self, tuner, features, channel):
        events = self.run_pipeline(tuner=tuner, features=features, PIX_CHANNEL=channel)
        source = "siano-ts" if tuner == "pxs1ud" else "torne-c"
        producer = self.stage(events, source)
        expected = (["--device", "9:7", "--firmware", self.env["PIX_FIRMWARE"], "-c", channel]
                    if tuner == "pxs1ud" else ["--channel", channel])
        self.assertEqual(producer["args"], expected)
        self.assertEqual(self.stage(events, "torne-b25")["args"],
                         ["--card-socket", self.env["PIX_CARD_SOCKET"]])
        self.assertFalse(any(event["command"] in ("pix-ts-legacy", "tail") for event in events))
        self.assertFalse((self.runtime / "pix/live.ts").exists())
        self.assert_output_chain(events, source, features)
        self.assertEqual(values(self.stage(events, "stb_streamer")["args"], "--channel"), [channel])

    def test_px_s1ud_features_use_direct_siano_bcas_eit_chain(self):
        # PIX_CAPTURE_SECONDS is deliberately absent, as on unit1.
        self.assertNotIn("PIX_CAPTURE_SECONDS", self.env)
        self.assert_direct("pxs1ud", True, "17")

    def test_px_s1ud_without_features_maps_video_and_audio(self):
        self.assert_direct("pxs1ud", False, "28")

    def test_torne_features_use_direct_torne_bcas_eit_chain(self):
        self.assert_direct("torne", True, "42")

    def test_torne_without_features_maps_video_and_audio(self):
        self.assert_direct("torne", False, "17")

    def test_dt295_manual_buffer_uses_canonical_satellite_arguments_and_eit(self):
        events = self.run_pipeline(tuner="dt295", features=False, PIX_SYSTEM="bs", PIX_CHANNEL="9",
                                   PIX_SAT_SLOT="0", PIX_SAT_TSID="16432", PIX_PIDS="0 17 18 257",
                                   PIX_CAPTURE_SECONDS="1800")
        args = self.stage(events, "pix-ts-legacy")["args"]
        self.assertEqual(args, ["--model", "dt295", "--assets", self.env["PIX_DT295_ASSETS"],
                                "-s", "bs", "-c", "9", "--slot", "0", "--tsid", "16432",
                                "-p", "0", "-p", "17", "-p", "18", "-p", "257", "-t", "1800"])
        self.stage(events, "tail")
        self.assertEqual(self.stage(events, "torne-b25")["args"],
                         ["--card-socket", self.env["PIX_CARD_SOCKET"],
                          "--trace", str(self.runtime / "pix/b25.trace")])
        self.assert_output_chain(events, "pix-ts-legacy", False, buffered=True)

    def test_dt295_manual_buffer_features_share_same_output_chain(self):
        events = self.run_pipeline(tuner="dt295", PIX_CAPTURE_SECONDS="1800")
        self.assertEqual(values(self.stage(events, "pix-ts-legacy")["args"], "-c"), ["T17"])
        self.assert_output_chain(events, "pix-ts-legacy", True, buffered=True)

    def test_dt295_known_clear_build_preserves_source_descramble_choice(self):
        events = self.run_pipeline(tuner="dt295", features=False, PIX_DT295_DESCRAMBLED="1")
        self.assertFalse(any(event["stage"] in ("resync", "torne-b25") for event in events))
        self.assert_output_chain(events, "pix-ts-legacy", False, buffered=True, descrambled=True)

    def test_feature_failure_retry_restores_raw_video_map_not_audio_only(self):
        events = self.run_pipeline(retry=True, PIX_FEATURE_FAILURES="1", PIX_MAX_SHORT_RUNS="2")
        encodes = [event for event in events if event["stage"] == "encode"]
        self.assertEqual(len(encodes), 2)
        first, second = [event["args"] for event in encodes]
        self.assertEqual(values(first[:first.index("pipe:1")], "-map"), ["[main]", "0:a:0"])
        self.assertEqual(values(second, "-map"), ["0:v:0", "0:a:0"])
        self.assertNotIn("-filter_complex", second)
        streamers = [event["args"] for event in events if event["stage"] == "stb_streamer"]
        self.assertEqual(len(streamers), 2)
        self.assertIn("--feature-file", streamers[0])
        self.assertNotIn("--feature-file", streamers[1])
        self.assertEqual(len([event for event in events if event["stage"] == "eit_watch"]), 2)
        self.assertFalse((self.runtime / "pix/live.ts").exists())

    def test_stale_eit_stage_is_included_in_pipeline_cleanup(self):
        source = LAUNCHER.read_text()
        cleanup = source[source.index('kill_stale() {'):source.index('\ncleanup() {')]
        events = self.base / "signals.txt"
        # Signal recording is a shell function: never signal any host process.
        prefix = '''
log() { :; }
sleep() { :; }
ps() { printf 'PID USER COMMAND\\n4242 root eit_watch --state fixture\\n'; }
'''
        prefix += 'kill() { printf "%s\\n" "$*" >> ' + shlex.quote(str(events)) + '; }\n'
        result = subprocess.run(["/bin/sh", "-c", prefix + cleanup + '\nkill_stale\n'],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(events.read_text().splitlines(), ["-TERM 4242", "-9 4242"])

    def test_launcher_remains_posix_shell_syntax(self):
        result = subprocess.run(["/bin/sh", "-n", str(LAUNCHER)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


class SimpleLauncherFeatures(unittest.TestCase):
    """pix-stream-simple must be able to carry the CM feature sideband.

    The old launcher builds the statistics and passes --feature-* to
    stb_streamer; the simple launcher used by units 2 and 3 did not, so a unit
    with PIX_FEATURES=1 silently recorded without any CM information. These
    checks pin the arguments without running ffmpeg.
    """

    SIMPLE = ROOT / "board/x96max/overlay/usr/bin/pix-stream-simple"

    def text(self):
        return self.SIMPLE.read_text()

    def test_feature_defaults_are_off(self):
        body = self.text()
        self.assertIn('PIX_FEATURES:=0', body)
        self.assertIn('PIX_FEATURE_MAX_MB:=384', body)

    def test_the_streamer_forwards_the_feature_arguments(self):
        body = self.text()
        streamer = body[body.index("streamer() {"):]
        streamer = streamer[:streamer.index("\n}\n")]
        self.assertIn("$FEAT_ARGS", streamer)

    def test_the_feature_flags_match_what_stb_streamer_accepts(self):
        body = self.text()
        helper = body[body.index("encode_with_features() {"):]
        helper = helper[:helper.index("\n}\n")]
        helpers = {flag for flag in
                   ("--feature-file", "--feature-audio-file",
                    "--feature-black-file", "--feature-max-mb")
                   if flag in helper}
        self.assertEqual(helpers, {"--feature-file", "--feature-audio-file",
                                   "--feature-black-file", "--feature-max-mb"})

    def test_the_recording_path_stays_without_filters_unless_enabled(self):
        body = self.text()
        encode = body[body.index("encode() {"):]
        encode = encode[:encode.index("\n}\n")]
        # The plain branch must still copy audio and keep the broadcast clock.
        self.assertIn('-c:a copy -copyts -start_at_zero', encode)
        self.assertIn('if [ "$PIX_FEATURES" = "1" ]', encode)

    def test_the_analysis_uses_a_downscaled_copy(self):
        body = self.text()
        helper = body[body.index("encode_with_features() {"):]
        helper = helper[:helper.index("\n}\n")]
        self.assertIn("scale=320:180", helper)
        self.assertIn("signalstats", helper)
        # Picture and sound must keep the broadcast clock.
        self.assertIn("split=2", helper)


if __name__ == "__main__":
    unittest.main()
