#!/usr/bin/env python3
"""Shows what follows each candidate boundary, so the right one can be picked.

When a proposed edge does not sit on a real transition, the replacement is one
of the chapter points nearby. Listing those points as numbers does not say which
one starts the commercial, so each candidate gets four frames: just before, just
after, and two and four seconds after.

    candidate_sheet.py --work WORK --out SHEET.png \
        --source slug=/path.mp4 --point slug=1032.63 [--point ...]

The tile label is the candidate time and the offset, so an answer is one number
per proposed edge, such as "3=1064.56".
"""
import argparse
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

FFMPEG = '/home/ku/work/TVerRec/bin/ffmpeg'
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
OFFSETS = (-0.5, 0.5, 2.0, 4.0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--source', action='append', required=True, help='slug=/path.mp4')
    parser.add_argument('--point', action='append', required=True, help='slug=seconds')
    parser.add_argument('--width', type=int, default=240)
    args = parser.parse_args()

    work = Path(args.work)
    sources = dict(item.split('=', 1) for item in args.source)
    points = []
    for item in args.point:
        slug, value = item.split('=', 1)
        points.append((slug, float(value)))
    for slug in {slug for slug, _ in points}:
        if slug not in sources:
            raise SystemExit(f'no --source for {slug}')

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix='cand-', dir=str(out.parent)))
    columns = len(OFFSETS)
    try:
        argv = [FFMPEG, '-hide_banner', '-loglevel', 'error']
        rows, tile = [], 0
        for number, (slug, point) in enumerate(points, start=1):
            origin = json.loads((work / slug / 'info.json').read_text())['video_origin_s']
            for order, offset in enumerate(OFFSETS):
                target = staging / f'c{number:02d}_{order}.png'
                label = f'{number} {point:.2f} {offset:+.1f}'
                vf = (f'scale={args.width}:-2,drawtext=fontfile={FONT}:text={label}'
                      f':x=4:y=4:fontsize=17:fontcolor=yellow:box=1:boxcolor=black@0.7')
                subprocess.run([FFMPEG, '-hide_banner', '-loglevel', 'error', '-ss',
                                f'{point + offset + origin:.3f}', '-i', sources[slug],
                                '-frames:v', '1', '-vf', vf, str(target)],
                               check=True, capture_output=True)
                argv += ['-i', str(target)]
            inputs = ''.join(f'[{tile + order}:v]' for order in range(columns))
            rows.append(f'{inputs}hstack=inputs={columns}[row{number}]')
            tile += columns
        stacked = ''.join(f'[row{n}]' for n in range(1, len(points) + 1))
        graph = ';'.join(rows + [f'{stacked}vstack=inputs={len(points)}[out]'])
        argv += ['-filter_complex', graph, '-map', '[out]', '-frames:v', '1', str(out)]
        subprocess.run(argv, check=True)
    finally:
        shutil.rmtree(staging, ignore_errors=True)

    for number, (slug, point) in enumerate(points, start=1):
        print('%-3d %-8s %8.2f' % (number, slug, point), flush=True)
    print('sheet:', out, flush=True)


if __name__ == '__main__':
    if not Path(FFMPEG).exists():
        FFMPEG = shutil.which('ffmpeg') or FFMPEG
    main()
