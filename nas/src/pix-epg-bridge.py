#!/usr/bin/env python3
"""pix-epg-bridge - STB の eit_watch 番組表を mirakc 互換 API で pix-archive に供給する。

Linux STB 3 台の GET /v1/epg（eit_watch が蓄積した番組表 JSON）を定期取得し、
旧 mirakc（192.168.99.40:40772。2026-11-21 から停止）と同じ API 形状で
pix-archive（--epg）に渡す。recorder と manager は /api/archive/* だけを見るため、
この置き換えの影響はカタログとフォールバック命名に限られる。

局の対応表は配備構成である。どの unit がどの局を受けるかはイメージの defaults
が決める（board/x96max/ramboot-unit{1,2,3}-overlay）。STB が死んでも前回の
データを提供し続ける。収集の停止は監査の epg_stale が捉える。

GET /v1/epg の形式（eit_watch が書く）:
  {"serviceId":35864,"generatedAt":1795278600000,
   "events":[{"eventId":1714,"startAt":...,"duration":...,"name":"..."}]}

mirakc 形への変換規則:
  service id  = networkId * 100000 + serviceId
  program id  = networkId * 10^10 + serviceId * 10^5 + eventId
（pix-archive の resolve() と同じ合成規則。archive.sqlite に保存されていた
mirakc 生 JSON で確認済み。）
"""
import argparse
import json
import sys
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VERSION = "pix-epg-bridge-20261122"

# 局の対応表。archive.sqlite の services に残っていた mirakc の値（networkId、
# 名前、チャンネル）を確認済みの値で静的に持つ。
STBS = [
    dict(host="192.168.99.109", port=40774, serviceId=35864, networkId=32195,
         name="テレビ静岡", channel={"type": "GR", "channel": "17"}),
    dict(host="192.168.99.123", port=40774, serviceId=211, networkId=4,
         name="ＢＳ１１イレブン", channel={"type": "BS", "channel": "BS09_0"}),
    dict(host="192.168.99.56", port=40774, serviceId=35856, networkId=32194,
         name="ＳＢＳ１", channel={"type": "GR", "channel": "15"}),
]


def log(event, **fields):
    print(json.dumps(dict(event=event, ts=time.strftime("%Y-%m-%dT%H:%M:%S"),
                          **fields), ensure_ascii=False), flush=True)


def service_list():
    """mirakc /api/services 形状の局一覧。"""
    services = []
    for stb in STBS:
        services.append({
            "id": stb["networkId"] * 100000 + stb["serviceId"],
            "serviceId": stb["serviceId"],
            "networkId": stb["networkId"],
            "type": 1,
            "logoId": -1,
            "remoteControlKeyId": 0,
            "name": stb["name"],
            "channel": dict(stb["channel"]),
            "hasLogoData": False,
        })
    return services


def convert_event(stb, event):
    """eit_watch の 1 イベントを mirakc /api/programs 形状に変換する。"""
    event_id = int(event["eventId"])
    start_at = int(event["startAt"])
    duration = int(event["duration"])
    if event_id <= 0 or start_at <= 0 or duration <= 0:
        return None
    return {
        "id": stb["networkId"] * 10**10 + stb["serviceId"] * 10**5 + event_id,
        "eventId": event_id,
        "serviceId": stb["serviceId"],
        "networkId": stb["networkId"],
        "startAt": start_at,
        "duration": duration,
        "isFree": True,
        "name": event.get("name") or "",
    }


class State:
    """各局の最後の成功データ。STB が死んでも前回分を提供し続ける。

    キーは serviceId（host ではなく。同一ホストで複数局を運ぶ可能性を
    局の対応表が持っていても混同しないため）。
    """

    def __init__(self):
        self.lock = threading.Lock()
        self.data = {}  # serviceId -> {"generatedAt":..., "fetchedAt":..., "events":[...]}

    def put(self, key, body):
        with self.lock:
            self.data[key] = body

    def snapshot(self):
        with self.lock:
            return dict(self.data)


def fetch_stb(stb, timeout):
    """1 台の GET /v1/epg を取得して検証する。失敗や形式不正は None。"""
    url = "http://%s:%d/v1/epg" % (stb["host"], stb["port"])
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            body = json.loads(response.read().decode("utf-8"))
    except Exception as error:  # network, timeout, JSON の壊れ
        log("fetch_failed", host=stb["host"], error=str(error)[:200])
        return None
    if body.get("serviceId") != stb["serviceId"]:
        log("service_mismatch", host=stb["host"], expected=stb["serviceId"],
            got=body.get("serviceId"))
        return None
    events = [event for event in (convert_event(stb, e) for e in body.get("events") or [])
              if event]
    return {"generatedAt": int(body.get("generatedAt") or 0), "events": events}


def poll_once(state, timeout):
    """3 台を取得して state を更新する。bridge の 1 周期。"""
    for stb in STBS:
        result = fetch_stb(stb, timeout)
        if result is None:
            continue
        state.put(stb["serviceId"], {"generatedAt": result["generatedAt"],
                                     "fetchedAt": round(time.time(), 3),
                                     "events": result["events"]})
        log("collected", host=stb["host"], serviceId=stb["serviceId"],
            events=len(result["events"]), generatedAt=result["generatedAt"])


def poller(state, interval, timeout):
    while True:
        poll_once(state, timeout)
        time.sleep(interval)


def programs(state):
    """mirakc /api/programs 形状の番組表（3 局分を結合し startAt 順）。"""
    merged = []
    for stb, data in ((stb, state.snapshot().get(stb["serviceId"])) for stb in STBS):
        if data:
            merged.extend(data["events"])
    merged.sort(key=lambda event: (event["startAt"], event["serviceId"]))
    return merged


class Handler(BaseHTTPRequestHandler):
    state = None

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/api/services":
            body = service_list()
        elif path == "/api/programs":
            body = programs(self.state)
        elif path == "/api/tuners":
            body = []
        elif path == "/api/version":
            body = {"version": VERSION}
        elif path == "/health":
            snapshot = self.state.snapshot()
            body = {"version": VERSION, "stbs": [
                {"host": stb["host"], "serviceId": stb["serviceId"],
                 "events": len(snapshot[stb["serviceId"]]["events"])
                 if stb["serviceId"] in snapshot else 0,
                 "fetchedAt": snapshot[stb["serviceId"]]["fetchedAt"]
                 if stb["serviceId"] in snapshot else None}
                for stb in STBS]}
        else:
            self.send_error(404)
            return
        payload = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt, *args):  # access log は不要（collected が要約する）
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=40775)
    parser.add_argument("--bind", default="127.0.0.1",
                        help="待ち受けアドレス。pix-archive だけが消費するので既定はローカル")
    parser.add_argument("--interval", type=int, default=60,
                        help="STB の /v1/epg 取得間隔（秒）")
    parser.add_argument("--timeout", type=int, default=10,
                        help="STB 1 台あたりの HTTP タイムアウト（秒）")
    args = parser.parse_args()

    state = State()
    Handler.state = state
    thread = threading.Thread(target=poller, args=(state, args.interval, args.timeout),
                              daemon=True)
    thread.start()
    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    log("listening", bind=args.bind, port=args.port, interval=args.interval,
        stbs=[stb["host"] for stb in STBS])
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
