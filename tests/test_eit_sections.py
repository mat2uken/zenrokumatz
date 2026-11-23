#!/usr/bin/python3
"""Isolated native SI/EIT and logo regressions; synthetic TS only, no devices.

The C fixture includes the production EIT main/dispatch and controls only its
clock. Build outputs, state and logos live in a unique temporary directory.
"""
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
LOGO = ROOT / "package/pix-tuner/logo"
EIT = ROOT / "package/stb-streamer/src"


def crc32_mpeg(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ (0x04C11DB7 if crc & 0x80000000 else 0)) & 0xFFFFFFFF
    return crc


def finish(data):
    data = bytearray(data)
    size = len(data) + 4 - 3
    data[1:3] = struct.pack(">H", 0xB000 | size)
    return bytes(data) + struct.pack(">I", crc32_mpeg(data))


def pat(sid):
    return finish(bytes.fromhex("0000000001c10000") + struct.pack(">HH", sid, 0xE100))


def eit(sid=35864, following=False, padding=0):
    data = bytearray.fromhex("4e00000000c100017dc37dc3014e")
    data[3:5] = struct.pack(">H", sid)
    data[6] = int(following)
    event = 1601 if following else 1599
    when = bytes.fromhex("efb2112400000600" if following else "efb2090000022400")
    # Real ARIB text: LS1 then NOW, which decodes to fullwidth ＮＯＷ.
    desc = bytes.fromhex("4d096a706e040e4e4f5700")
    while padding:
        n = min(padding, 200)
        desc += bytes([0xFE, n]) + bytes(n)
        padding -= n
    data += struct.pack(">H", event) + when + struct.pack(">H", len(desc)) + desc
    return finish(data)


def start_bytes(start_ms):
    """EIT start time: MJD(16bit) + BCD hh mm ss, in JST."""
    local = start_ms + 9 * 3600 * 1000
    days, rem = divmod(local, 86400000)
    hh, rem = divmod(rem, 3600000)
    mm, rem = divmod(rem, 60000)
    return struct.pack(">H", days + 40587) + bytes(
        int("%02d" % v, 16) for v in (hh, mm, rem // 1000))


def duration_bytes(duration_ms):
    """EIT duration: 3-byte BCD hh mm ss."""
    seconds = duration_ms // 1000
    hh, rem = divmod(seconds, 3600)
    mm, ss = divmod(rem, 60)
    return bytes(int("%02d" % v, 16) for v in (hh, mm, ss))


def name_desc(text):
    """short_event_descriptor with the LS1 single-byte shift, like eit() above."""
    body = b"\x0e" + text.encode("ascii")
    content = b"jpn" + bytes([len(body)]) + body
    return bytes([0x4D, len(content)]) + content


def schedule_eit(sid=35864, table=0x50, sec_num=0, last_sec=1, events=()):
    """EIT schedule section: [(event_id, start_ms, duration_ms, name|None), ...]"""
    data = bytearray([table, 0, 0])  # table_id + section_length placeholder
    data += struct.pack(">H", sid)
    data += bytes([0xC1, sec_num, last_sec])
    data += bytes.fromhex("7dc37dc3")  # same tsid/onid as the present/following fixtures
    data += bytes([last_sec, table])
    for event_id, start_ms, duration_ms, text in events:
        desc = name_desc(text) if text else b""
        data += struct.pack(">H", event_id) + start_bytes(start_ms) + duration_bytes(duration_ms)
        data += struct.pack(">H", len(desc)) + desc
    return finish(bytes(data))


def packet(pid, data, pusi=False, cc=0):
    assert len(data) <= 184
    return bytes([0x47, (pid >> 8) | (0x40 if pusi else 0), pid & 255,
                  0x10 | (cc & 15)]) + data + b"\xff" * (184 - len(data))


def section(pid, data):
    result = packet(pid, b"\x00" + data[:183], pusi=True)
    data = data[183:]
    cc = 1
    while data:
        result += packet(pid, data[:184], cc=cc)
        data = data[184:]
        cc += 1
    return result


class EitSectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="eit-sections-")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.base = Path(cls.tmp.name)
        cls.cc = shlex.split(os.environ.get("CC", "cc"))
        cls.common = ["-std=c11", "-O1", "-Wall", "-Wextra",
                      "-Wno-unused-parameter", "-D_POSIX_C_SOURCE=200809L",
                      "-I" + str(LOGO), "-I" + str(EIT)]
        cls.deps = [str(LOGO / name) for name in
                    ("logo_cdt.c", "logo_store.c", "ts_section.c")]
        cls.deps += [str(EIT / "arib_text.c")]
        cls.fixture = cls.base / "test_eit_sections"
        cls.native = cls.base / "eit_watch"
        cls.logo = cls.base / "test_dt295_logo"
        cls.logo_cli = cls.base / "pix-dt295-logo"
        for source, output, deps in (
            (ROOT / "tests/test_eit_sections.c", cls.fixture, cls.deps),
            (EIT / "eit_watch.c", cls.native, cls.deps),
            (ROOT / "tests/test_dt295_logo.c", cls.logo,
             cls.deps + [str(LOGO / "logo_carousel.c")]),
            (LOGO / "pix_dt295_logo.c", cls.logo_cli,
             cls.deps + [str(LOGO / "logo_carousel.c")]),
        ):
            result = subprocess.run(cls.cc + cls.common + [str(source)] + deps
                                    + ["-o", str(output)], capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)

    def setUp(self):
        self.run_tmp = tempfile.TemporaryDirectory(dir=self.base, prefix="case-")
        self.addCleanup(self.run_tmp.cleanup)
        self.case = Path(self.run_tmp.name)
        self.state = self.case / "program.txt"
        self.epg = self.case / "epg.json"

    def stream(self, data, logos=False, native=False):
        cmd = [str(self.native if native else self.fixture), "--state", str(self.state)]
        if logos:
            cmd += ["--logo-dir", str(self.case / "logos")]
        if native:
            # The schedule cache only writes through the production main's own
            # --epg argument; fixtures disable it in the C fixture above.
            cmd += ["--epg", str(self.epg)]
        result = subprocess.run(cmd, input=data, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(result.stdout, data, "TS pass-through changed bytes")
        return result

    def epg_events(self):
        """Parses the schedule file the production main wrote."""
        self.assertTrue(self.epg.exists(), "epg.json was not written")
        return json.loads(self.epg.read_text())

    def test_callback_synthetic_cases(self):
        result = subprocess.run([str(self.fixture), "--fixtures", str(self.state)],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("test_eit_sections: ok", result.stdout)

    def test_cold_present_tail_before_following_and_stdout_identity(self):
        current, future = eit(padding=210), eit(following=True)
        tail = current[183:]
        data = section(0, pat(35864))
        data += packet(0x12, b"\x00" + current[:183], pusi=True)
        data += packet(0x12, bytes([len(tail)]) + tail + future, pusi=True, cc=1)
        # Exercise multiple main read batches while the logo tap is enabled.
        data += packet(0x1FFF, b"\xff" * 184) * 300
        self.assertFalse(self.state.exists())
        self.stream(data, logos=True)
        state = dict(line.split("=", 1) for line in self.state.read_text().splitlines())
        # continues/stops let the streamer keep one segment for this
        # programme even when the station reports another starting event.
        self.assertEqual(state, dict(service="35864", event="1599", start="1794960000000",
                                     duration="8640000", wall="1794967250000",
                                     continues="1", stops="0", name="ＮＯＷ"))

    def test_back_to_back_wrong_service_then_current(self):
        data = section(0, pat(35864))
        data += packet(0x12, b"\x00" + eit(35865) + eit(), pusi=True)
        self.stream(data)
        self.assertIn("name=ＮＯＷ\n", self.state.read_text())

    def test_following_and_wrong_pid_do_not_seed_cold_state(self):
        data = section(0, pat(35864)) + section(0x12, eit(following=True))
        data += section(0x27, eit()) + section(0x12, eit(35865))
        self.stream(data)
        self.assertFalse(self.state.exists())

    def test_crc_invalid_section_rejected_and_recovered(self):
        bad = bytearray(eit())
        bad[-1] ^= 1
        prefix = section(0, pat(35864)) + section(0x12, bytes(bad))
        self.stream(prefix)
        self.assertFalse(self.state.exists())
        self.stream(prefix + section(0x12, eit()))
        self.assertIn("name=ＮＯＷ\n", self.state.read_text())

    def test_bs_pointer_zero_mult_packet_current_still_selected(self):
        self.stream(section(0, pat(211)) + section(0x12, eit(211, padding=700))
                    + section(0x12, eit(211, following=True)))
        state = self.state.read_text()
        self.assertIn("service=211\n", state)
        self.assertIn("event=1599\n", state)
        self.assertIn("name=ＮＯＷ\n", state)

    def test_native_production_main_pass_through(self):
        self.stream(section(0, pat(35864)) + section(0x12, eit())
                    + packet(0x1FFF, b"\xff" * 184) * 300, native=True)

    # ---- schedule cache (/v1/epg に供給する epg.json) ----

    def test_schedule_events_cached_and_short_items_filtered(self):
        now_ms = int(time.time() * 1000) // 1000 * 1000  # EIT 開始時刻は秒分解能
        long_event = (5001, now_ms + 6 * 3600 * 1000, 3600000, "FUTURE")
        short_event = (5002, now_ms + 7 * 3600 * 1000, 120000, "PROMO")
        data = section(0, pat(35864))
        data += section(0x12, schedule_eit(events=[long_event, short_event]))
        self.assertFalse(self.epg.exists())
        self.stream(data, native=True)
        body = self.epg_events()
        self.assertEqual(body["serviceId"], 35864)
        self.assertLess(abs(body["generatedAt"] - now_ms), 60000)
        self.assertEqual(len(body["events"]), 1, "3分未満の番宣を番組として扱わない")
        event = body["events"][0]
        self.assertEqual(event, dict(eventId=5001, startAt=now_ms + 6 * 3600 * 1000,
                                     duration=3600000, name="ＦＵＴＵＲＥ"))

    def test_past_events_pruned_and_output_sorted(self):
        now_ms = int(time.time() * 1000) // 1000 * 1000  # EIT 開始時刻は秒分解能
        stale = (5100, now_ms - 30 * 3600 * 1000, 1800000, "OLD")
        future = (5101, now_ms + 6 * 3600 * 1000, 1800000, "NEW")
        data = section(0, pat(35864))
        data += section(0x12, schedule_eit(events=[future, stale]))
        self.stream(data, native=True)
        body = self.epg_events()
        self.assertEqual([e["eventId"] for e in body["events"]], [5101],
                         "24時間より昔に終わった番組を捨てる")

    def test_schedule_name_arrives_after_first_section(self):
        now_ms = int(time.time() * 1000) // 1000 * 1000  # EIT 開始時刻は秒分解能
        data = section(0, pat(35864))
        data += section(0x12, schedule_eit(events=[(5200, now_ms + 6 * 3600 * 1000, 1800000, None)]))
        data += section(0x12, schedule_eit(table=0x51, sec_num=1,
                                           events=[(5200, now_ms + 6 * 3600 * 1000, 1800000, "NAMED")]))
        self.stream(data, native=True)
        event = self.epg_events()["events"][0]
        self.assertEqual(event["name"], "ＮＡＭＥＤ")

    def test_schedule_wrong_service_and_crc_rejected(self):
        now_ms = int(time.time() * 1000) // 1000 * 1000  # EIT 開始時刻は秒分解能
        data = section(0, pat(35864))
        data += section(0x12, schedule_eit(sid=35865, events=[(5300, now_ms + 6 * 3600 * 1000, 1800000, "X")]))
        bad = bytearray(schedule_eit(events=[(5301, now_ms + 6 * 3600 * 1000, 1800000, "Y")]))
        bad[-1] ^= 1
        data += section(0x12, bytes(bad))
        self.stream(data, native=True)
        self.assertFalse(self.epg.exists(), "他局とCRC不正のセクションは採らない")

    def test_shared_logo_collector_regressions(self):
        sample, dummy = self.case / "logo.ts", self.case / "dummy.ts"
        result = subprocess.run([str(self.logo), str(sample), str(dummy)],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("test_dt295_logo: ok", result.stdout)
        out = self.case / "logo-out"
        result = subprocess.run([str(self.logo_cli), "--out", str(out), "--wait", "5",
                                 "--system", "bs", "--tsid", "0x40f1"],
                                input=sample.read_bytes(), capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        for name in ("400211.png", "400212.png", "index.json"):
            self.assertGreater((out / name).stat().st_size, 0)
        self.assertIn('"id": 400211', (out / "index.json").read_text())
        empty = self.case / "logo-dummy-out"
        result = subprocess.run([str(self.logo_cli), "--out", str(empty), "--wait", "5",
                                 "--quiet"], input=dummy.read_bytes(),
                                capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 3, result.stderr.decode())
        self.assertEqual(list(empty.glob("*.png")), [])


if __name__ == "__main__":
    unittest.main()
