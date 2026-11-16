#!/usr/bin/env python3
"""Walk a fragmented MP4 and report its fragment structure.

Runs on the machine that holds the file, so a slow network link does not have
to carry the whole recording. Only box headers are read, and the file is
seeked over, so a 1.5 GB recording costs a few thousand small reads.

  python3 mp4_fragment_stats.py /tmp/long-30min.mp4 [--head MB] [--tail-frags N]

With --head it also writes <file>.head.mp4 (the init segment plus that many
megabytes), and with --tail-frags it writes <file>.tail.mp4 (the init segment
plus the last N fragments). Both are small enough to copy off a slow link and
still decode, because every fragment starts on a key frame.
"""
import argparse
import os
import struct
import sys

CONTAINERS = {b"moov", b"trak", b"mdia", b"minf", b"stbl", b"mvex",
              b"moof", b"traf", b"mfra"}


def read_box_header(f, off, end):
    """Return (type, body_offset, box_end) or None when the chain is broken."""
    f.seek(off)
    head = f.read(8)
    if len(head) < 8:
        return None
    size = struct.unpack(">I", head[:4])[0]
    typ = head[4:8]
    body = off + 8
    if size == 1:
        ext = f.read(8)
        if len(ext) < 8:
            return None
        size = struct.unpack(">Q", ext)[0]
        body = off + 16
    elif size == 0:
        size = end - off
    if size < 8 or off + size > end:
        return None
    return typ, body, off + size


def walk(f, start, end, want=None):
    """Yield (type, body_offset, box_end) for boxes in [start, end)."""
    off = start
    while off < end:
        hdr = read_box_header(f, off, end)
        if not hdr:
            return
        typ, body, box_end = hdr
        yield typ, body, box_end
        if typ in CONTAINERS:
            yield from walk(f, body, box_end, want)
        off = box_end


def traf_info(f, body, box_end):
    """Return (track_id, base_media_decode_time, sample_count) for one traf."""
    track_id = None
    tfdt = None
    samples = 0
    f.seek(body)
    while True:
        head = f.read(8)
        if len(head) < 8:
            break
        size = struct.unpack(">I", head[:4])[0]
        typ = head[4:8]
        if size < 8:
            break
        payload = f.read(size - 8)
        if typ == b"tfhd" and len(payload) >= 8:
            track_id = struct.unpack(">I", payload[4:8])[0]
        elif typ == b"tfdt" and len(payload) >= 8:
            version = payload[0]
            if version == 1 and len(payload) >= 12:
                tfdt = struct.unpack(">Q", payload[4:12])[0]
            else:
                tfdt = struct.unpack(">I", payload[4:8])[0]
        elif typ == b"trun" and len(payload) >= 8:
            # version/flags are the first four bytes, then sample_count
            samples += struct.unpack(">I", payload[4:8])[0]
        if f.tell() >= box_end:
            break
    return track_id, tfdt, samples


def mvhd_timescale(f, body, box_end):
    f.seek(body)
    payload = f.read(min(box_end - body, 32))
    if len(payload) < 20:
        return 0
    if payload[0] == 1:
        return struct.unpack(">I", payload[20:24])[0] if len(payload) >= 24 else 0
    return struct.unpack(">I", payload[12:16])[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--head", type=int, default=0, help="head sample size in MB")
    ap.add_argument("--tail-frags", type=int, default=0,
                    help="number of trailing fragments to keep")
    ap.add_argument("--head-frags", type=int, default=0,
                    help="number of leading fragments to keep")
    ap.add_argument("--tail-skip", type=int, default=0,
                    help="drop this many fragments at the end (a recording cut "
                         "mid-fragment ends inside an mdat)")
    args = ap.parse_args()

    path = args.path
    end = os.path.getsize(path)
    frags = []
    init_end = 0
    mdat_bytes = 0
    moof_count = 0
    timescale = 0
    per_track = {}
    max_tfdt = {}

    with open(path, "rb") as f:
        for typ, body, box_end in walk(f, 0, end):
            if typ in (b"ftyp", b"moov"):
                init_end = max(init_end, box_end)
                for t2, b2, e2 in walk(f, body, box_end):
                    if t2 == b"mvhd":
                        timescale = mvhd_timescale(f, b2, e2)
            elif typ == b"moof":
                moof_count += 1
                for t2, b2, e2 in walk(f, body, box_end):
                    if t2 == b"traf":
                        tid, tfdt, n = traf_info(f, b2, e2)
                        if tid is not None:
                            per_track[tid] = per_track.get(tid, 0) + n
                            if tfdt is not None:
                                max_tfdt[tid] = max(max_tfdt.get(tid, 0), tfdt)
                frags.append(body - 8)  # moof start offset
            elif typ == b"mdat":
                mdat_bytes += box_end - body

    print("file: %s" % path)
    print("size: %d bytes (%.1f MB)" % (end, end / 1048576.0))
    print("init segment: 0..%d bytes" % init_end)
    print("fragments (moof): %d" % moof_count)
    for tid in sorted(per_track):
        last = max_tfdt.get(tid, 0)
        # track 1 is the video track and track 2 the audio track in the files
        # this pipeline produces. Report the duration each one implies so the
        # two can be compared against the wall clock of the recording.
        if tid == 1:
            implied = per_track[tid] / (30000.0 / 1001.0)
            note = "  -> %.1f s at 29.97 fps" % implied
        elif tid == 2:
            implied = per_track[tid] * 1024.0 / 48000.0
            note = "  -> %.1f s of AAC at 48 kHz" % implied
        else:
            note = ""
        print("track %d: %d samples, last decode time %d%s" % (
            tid, per_track[tid], last, note))
    print("mdat payload: %.1f MB" % (mdat_bytes / 1048576.0))
    if moof_count:
        print("average fragment payload: %.1f KB" % (
            mdat_bytes / moof_count / 1024.0))

    if args.head:
        n = args.head * 1024 * 1024
        out = path + ".head.mp4"
        with open(path, "rb") as f, open(out, "wb") as o:
            f.seek(0)
            o.write(f.read(n))
        print("wrote %s (%d bytes)" % (out, os.path.getsize(out)))

    if args.head_frags and len(frags) > args.head_frags + 1:
        stop = frags[args.head_frags]
        out = path + ".headfrags.mp4"
        with open(path, "rb") as f, open(out, "wb") as o:
            f.seek(0)
            o.write(f.read(init_end))
            f.seek(init_end)
            o.write(f.read(stop - init_end))
        print("wrote %s (%d bytes, %d fragments)" % (
            out, os.path.getsize(out), args.head_frags))

    if args.tail_frags and len(frags) > args.tail_frags + args.tail_skip:
        start = frags[-(args.tail_frags + args.tail_skip)]
        stop = end if not args.tail_skip else frags[-args.tail_skip]
        out = path + ".tail.mp4"
        with open(path, "rb") as f, open(out, "wb") as o:
            f.seek(0)
            o.write(f.read(init_end))
            f.seek(start)
            o.write(f.read(stop - start))
        print("wrote %s (%d bytes, from fragment %d)" % (
            out, os.path.getsize(out), len(frags) - args.tail_frags))
    return 0


if __name__ == "__main__":
    sys.exit(main())
