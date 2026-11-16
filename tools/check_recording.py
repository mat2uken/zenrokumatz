#!/usr/bin/env python3
"""Summarise a recording pulled from the STB.

Reports container duration, stream parameters, frame counts and how many
decode errors ffmpeg sees for the video and the audio separately. The two are
checked apart because a lost TS packet usually damages one and not the other.

  tools/check_recording.py work/nas-check/long-30min.mp4
"""
import json
import subprocess
import sys


def run(args):
    return subprocess.run(args, capture_output=True, text=True)


def probe(path):
    r = run(["ffprobe", "-hide_banner", "-v", "error", "-print_format", "json",
             "-show_format", "-show_streams", path])
    if r.returncode != 0:
        print("ffprobe failed:", r.stderr.strip()[:400])
        return None
    return json.loads(r.stdout)


def count_errors(path, extra):
    r = run(["ffmpeg", "-hide_banner", "-nostdin", "-v", "error", "-i", path]
            + extra + ["-f", "null", "-"])
    lines = [ln for ln in r.stderr.splitlines() if ln.strip()]
    return len(lines), lines[:3]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    info = probe(path)
    if not info:
        return 1

    fmt = info.get("format", {})
    print("file: %s" % path)
    print("duration: %s s   size: %s bytes   bitrate: %s" % (
        fmt.get("duration", "?"), fmt.get("size", "?"), fmt.get("bit_rate", "?")))

    for st in info.get("streams", []):
        if st.get("codec_type") not in ("video", "audio"):
            continue
        print("  #%s %s %s %sx%s" % (st.get("index"), st.get("codec_type"),
                                     st.get("codec_name"),
                                     st.get("width", "-"), st.get("height", "-")))

    v_errors, v_sample = count_errors(path, ["-map", "0:v"])
    a_errors, a_sample = count_errors(path, ["-map", "0:a"])
    print("video decode errors: %d" % v_errors)
    for ln in v_sample:
        print("   ", ln[:120])
    print("audio decode errors: %d" % a_errors)
    for ln in a_sample:
        print("   ", ln[:120])
    return 0


if __name__ == "__main__":
    sys.exit(main())
