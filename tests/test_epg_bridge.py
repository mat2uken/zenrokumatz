#!/usr/bin/env python3
"""EPG 供給の置き換え（mirakc 廃止）の回帰。

- pix-epg-bridge: eit_watch の /v1/epg を mirakc 互換 JSON に変換する変換規則と
  HTTP 応答。合成 id の規則は archive.sqlite に残っていた mirakc 生 JSON で
  確認した値（id = networkId*10^10 + serviceId*10^5 + eventId）を固定する。
- 正規ソースの参照ゼロ: pix-stb-guard.py と pix_archive.c から mirakc /
  192.168.99.40 / adb の参照が再び増えないことを grep で固定する。
- 監査: pix-stb-health への結合（TIMERS と status.json 読み込み）が戻らないこと。
- STB 側の /v1/epg 経路の入口が消えていないこと（ソースレベルの固定）。

ネットワークはループバックのみ。実 STB・実 NAS・実放送は使わない。
"""
import importlib.util
import json
import re
import socket
import subprocess
import sys
import threading
import time
import unittest
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NAS_SRC = ROOT / "nas/src"
EIT_SRC = ROOT / "package/stb-streamer/src"


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class FakeStbHandler(BaseHTTPRequestHandler):
    """eit_watch の /v1/epg 応答を模倣する実 STB の代役。"""

    body = b""

    def do_GET(self):
        self.send_response(200 if self.body else 404)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(self.body)))
        self.end_headers()
        self.wfile.write(self.body)

    def log_message(self, fmt, *args):
        pass


class BridgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bridge = load_module("pix_epg_bridge", NAS_SRC / "pix-epg-bridge.py")
        cls.original_stbs = list(cls.bridge.STBS)
        cls.setUpFakeServers()

    @classmethod
    def tearDownClass(cls):
        cls.bridge.STBS[:] = cls.original_stbs

    def setUp(self):
        # 局の対応表をループバックへ差し替える（module 変数の退避は class 単位）。
        cls = type(self)
        cls.bridge.STBS[:] = [
            dict(host="127.0.0.1", port=cls.port, serviceId=35864, networkId=32195,
                 name="テレビ静岡", channel={"type": "GR", "channel": "17"}),
            dict(host="127.0.0.1", port=cls.port, serviceId=211, networkId=4,
                 name="ＢＳ１１イレブン", channel={"type": "BS", "channel": "BS09_0"}),
            dict(host="127.0.0.1", port=cls.dead_port, serviceId=35856, networkId=32194,
                 name="ＳＢＳ１", channel={"type": "GR", "channel": "15"}),
        ]
        cls.state = cls.bridge.State()
        now_ms = int(time.time() * 1000) // 1000 * 1000
        FakeStbHandler.body = json.dumps({
            "serviceId": 35864, "generatedAt": now_ms,
            "events": [
                {"eventId": 1714, "startAt": now_ms + 3600000,
                 "duration": 1800000, "name": "うまレボ！"},
                {"eventId": 1715, "startAt": now_ms + 7200000,
                 "duration": 1800000, "name": "ニュース"},
            ]}).encode("utf-8")

    @classmethod
    def setUpFakeServers(cls):
        cls.port = free_port()
        cls.dead_port = free_port()  # nobody listens: SBS は取得失敗になる
        cls.stb_server = ThreadingHTTPServer(("127.0.0.1", cls.port), FakeStbHandler)
        cls.stb_thread = threading.Thread(target=cls.stb_server.serve_forever, daemon=True)
        cls.stb_thread.start()
        cls.addClassCleanup(cls.stb_server.shutdown)
        cls.addClassCleanup(cls.stb_server.server_close)

    def test_service_list_uses_mirakc_composite_ids(self):
        services = self.bridge.service_list()
        by_name = {service["name"]: service for service in services}
        self.assertEqual(by_name["テレビ静岡"]["id"], 3219535864)
        self.assertEqual(by_name["ＢＳ１１イレブン"]["id"], 400211)
        self.assertEqual(by_name["ＳＢＳ１"]["id"], 3219435856)
        self.assertEqual(by_name["テレビ静岡"]["channel"],
                         {"type": "GR", "channel": "17"})

    def test_program_id_is_network_service_event_composite(self):
        event = {"eventId": 36582, "startAt": 1794839400000, "duration": 1800000,
                 "name": "ロメリア戦記"}
        program = self.bridge.convert_event(self.bridge.STBS[1], event)
        # BS11: networkId 4, serviceId 211 → 40021136582（mirakc 実績値と同じ合成）
        self.assertEqual(program["id"], 40021136582)
        self.assertEqual(program["serviceId"], 211)
        self.assertEqual(program["isFree"], True)

    def test_programs_merge_and_sort_across_stbs(self):
        self.bridge.poll_once(self.state, timeout=5)
        programs = self.bridge.programs(self.state)
        self.assertEqual(len(programs), 2, "死んでいる局は前回データ（無ければ 0 件）")
        starts = [program["startAt"] for program in programs]
        self.assertEqual(starts, sorted(starts))
        self.assertEqual(programs[0]["serviceId"], 35864)

    def test_http_api_serves_mirakc_shapes(self):
        self.bridge.poll_once(self.state, timeout=5)
        port = free_port()
        handler = type("BoundHandler", (self.bridge.Handler,), {})
        handler.state = self.state
        server = ThreadingHTTPServer(("127.0.0.1", port), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.shutdown)
        with urllib.request.urlopen(
                f"http://127.0.0.1:{port}/api/services", timeout=5) as response:
            services = json.loads(response.read().decode("utf-8"))
        self.assertEqual([service["id"] for service in services],
                         [3219535864, 400211, 3219435856])
        with urllib.request.urlopen(
                f"http://127.0.0.1:{port}/api/programs", timeout=5) as response:
            programs = json.loads(response.read().decode("utf-8"))
        self.assertEqual(len(programs), 2)
        with urllib.request.urlopen(
                f"http://127.0.0.1:{port}/api/tuners", timeout=5) as response:
            self.assertEqual(json.loads(response.read().decode("utf-8")), [])

    def test_failure_keeps_previous_data(self):
        self.bridge.poll_once(self.state, timeout=5)
        FakeStbHandler.body = b""  # /v1/epg が 404 を返す（epg.json 未作成）
        self.bridge.poll_once(self.state, timeout=5)
        programs = self.bridge.programs(self.state)
        self.assertEqual(len(programs), 2, "取得失敗でも前回データを提供し続ける")


class SourceReferenceTests(unittest.TestCase):
    """収録基準 4 の回帰: 正規ソースから mirakc / .40 / adb 参照をなくす。"""

    PATTERN = re.compile(r"192\.168\.99\.40|mirakc|\badb\b")

    def test_guard_has_no_legacy_references(self):
        text = (NAS_SRC / "pix-stb-guard.py").read_text()
        self.assertEqual(self.PATTERN.findall(text), [])
        self.assertIn("SSH_HOSTS", text)

    def test_guard_self_test_passes(self):
        result = subprocess.run([sys.executable, str(NAS_SRC / "pix-stb-guard.py"),
                                 "--self-test"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("self-test ok", result.stdout)

    def test_archive_source_renamed_to_epg(self):
        text = (NAS_SRC / "pix_archive.c").read_text()
        self.assertEqual(self.PATTERN.findall(text), [])
        self.assertIn('"--epg"', text)
        self.assertIn("http://127.0.0.1:40775", text)
        self.assertNotIn('"--mirakc"', text)

    def test_audit_decoupled_from_stb_health(self):
        text = (NAS_SRC / "pix-recording-audit.py").read_text()
        self.assertNotIn("pix-stb-health", text)
        self.assertEqual(self.PATTERN.findall(text), [])
        import ast
        ast.parse(text)

    def test_stb_sources_expose_the_epg_endpoint(self):
        pipe = (EIT_SRC / "stb_linux_pipe.c").read_text()
        http = (EIT_SRC / "stb_http.c").read_text()
        watch = (EIT_SRC / "eit_watch.c").read_text()
        self.assertIn('"/dev/shm/pix/epg.json"', pipe)
        self.assertIn('"/v1/epg"', http)
        self.assertIn("epg_flush", watch)


if __name__ == "__main__":
    unittest.main()
