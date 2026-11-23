#!/usr/bin/env python3
"""Builds a contact sheet of frames around the cut boundaries of one recording.

Labelling a recording needs a person to look at the frames where a decision put
an edge, plus the frames just inside and just outside each block. This script
extracts exactly those frames, burns the timestamp into each tile, and tiles
them into one PNG, so one look answers "is this a commercial block".

    make_frame_sheet.py <source.mp4> <out.png> [options]

Times come from a summary written by evaluate_recordings.py (``--summary``), or
from an explicit ``--times 100,200``. Every candidate edge is checked at three
points: the edge itself, two seconds inside the block, and two seconds outside.

The timestamp burned into a tile is programme-relative - the same time base the
labels use - so a label can be written straight from the sheet.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

FFMPEG = '/home/ku/work/TVerRec/bin/ffmpeg'
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
INSIDE = 2.0


def candidate_times(summary, duration, limit=24):
    """Edges of every decision, plus two seconds inside and outside each block."""
    times = set()
    for side in ('mark', 'scenario'):
        for start, end in (summary.get(side) or {}).get('cuts') or []:
            for point in (start, end, start + INSIDE, end - INSIDE):
                if 0.5 <= point <= duration - 0.5:
                    times.add(round(point, 1))
    ordered = sorted(times)
    merged = []
    for point in ordered:
        if not merged or point - merged[-1] >= 1.0:
            merged.append(point)
    if len(merged) <= limit:
        return merged
    step = len(merged) / float(limit)
    return [merged[int(index * step)] for index in range(limit)]


def extract(source, times, directory, width):
    paths = []
    for index, point in enumerate(times, start=1):
        target = directory / f'f{index:02d}.png'
        label = f'{point:.1f}s'
        vf = (f'scale={width}:-2,drawtext=fontfile={FONT}:text={label}'
              f':x=6:y=6:fontsize=22:fontcolor=yellow:box=1:boxcolor=black@0.65')
        subprocess.run([FFMPEG, '-hide_banner', '-loglevel', 'error', '-ss', f'{point:.3f}',
                        '-i', str(source), '-frames:v', '1', '-vf', vf, str(target)],
                       check=True)
        paths.append(target)
    return paths


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source')
    parser.add_argument('output')
    parser.add_argument('--summary', help='summary.json from evaluate_recordings.py')
    parser.add_argument('--times', help='comma separated seconds, programme-relative')
    parser.add_argument('--origin', type=float, default=0.0,
                        help='info.json video_origin_s; added back for the seek')
    parser.add_argument('--duration', type=float, default=0.0)
    parser.add_argument('--cols', type=int, default=4)
    parser.add_argument('--width', type=int, default=480)
    args = parser.parse_args()

    if args.times:
        times = [float(value) for value in args.times.split(',')]
    elif args.summary:
        summary = json.loads(Path(args.summary).read_text())
        duration = args.duration or summary.get('duration') or 0.0
        times = candidate_times(summary, float(duration))
    else:
        raise SystemExit('need --summary or --times')
    if not times:
        raise SystemExit('no candidate boundaries')

    work = Path(tempfile.mkdtemp(prefix='sheet-', dir=str(Path(args.output).parent)))
    try:
        # The seek is in media time; the labels are programme-relative.
        media = [point + args.origin for point in times]
        extract(args.source, media, work, args.width)
        rows = (len(times) + args.cols - 1) // args.cols
        subprocess.run([FFMPEG, '-hide_banner', '-loglevel', 'error', '-framerate', '1',
                        '-i', str(work / 'f%02d.png'), '-vf',
                        f'tile={args.cols}x{rows}:padding=4:margin=4', '-frames:v', '1',
                        str(args.output)], check=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print(json.dumps(dict(output=str(args.output), tiles=len(times),
                          order=[[index + 1, round(point, 1)]
                                 for index, point in enumerate(times)]),
                     ensure_ascii=False), flush=True)


if __name__ == '__main__':
    if not Path(FFMPEG).exists():
        FFMPEG = shutil.which('ffmpeg') or FFMPEG
    main()
