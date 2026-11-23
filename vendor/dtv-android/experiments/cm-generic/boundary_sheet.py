#!/usr/bin/env python3
"""Pins a proposed edge to the frame where the picture actually changes.

A review sheet answers "is this block a commercial" at a two second grain, which
leaves the edge itself loose. This script takes the edges that survived that
review and lays out one row per edge at a fine grain, so the exact frame can be
read off the picture.

    boundary_sheet.py --work WORK --out SHEET.png \
        --source slug=/path/recording.mp4 [--source ...] \
        --edge slug=562.6 [--edge ...] [--steps 0.25] [--span 1.0]

Each tile carries its row number and its offset from the proposed edge, so an
answer is one number per edge, such as "1=+0.25" or "1=0".
"""
import argparse
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

FFMPEG = '/home/ku/work/TVerRec/bin/ffmpeg'
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'


def offsets_for(steps, span):
    """Symmetric offsets covering +-span, always including zero."""
    count = int(round(span / steps))
    return [round(index * steps, 3) for index in range(-count, count + 1)]


def extract(source, times, labels, directory, prefix, width):
    paths = []
    for order, (point, label) in enumerate(zip(times, labels)):
        target = directory / f'{prefix}_{order}.png'
        vf = (f'scale={width}:-2,drawtext=fontfile={FONT}:text={label}'
              f':x=6:y=6:fontsize=20:fontcolor=yellow:box=1:boxcolor=black@0.7')
        subprocess.run([FFMPEG, '-hide_banner', '-loglevel', 'error', '-ss', f'{point:.3f}',
                        '-i', str(source), '-frames:v', '1', '-vf', vf, str(target)],
                       check=True, capture_output=True)
        paths.append(target)
    return paths


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', required=True, help='directory holding <slug>/info.json')
    parser.add_argument('--out', required=True)
    parser.add_argument('--source', action='append', required=True, help='slug=/path.mp4')
    parser.add_argument('--edge', action='append', required=True, help='slug=seconds')
    parser.add_argument('--steps', type=float, default=0.25)
    parser.add_argument('--span', type=float, default=1.0)
    parser.add_argument('--width', type=int, default=260)
    args = parser.parse_args()

    work = Path(args.work)
    sources = dict(item.split('=', 1) for item in args.source)
    edges = []
    for item in args.edge:
        slug, value = item.split('=', 1)
        edges.append((slug, float(value)))
    for slug in {slug for slug, _ in edges}:
        if slug not in sources:
            raise SystemExit(f'no --source for {slug}')

    offsets = offsets_for(args.steps, args.span)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix='edges-', dir=str(out.parent)))
    columns = len(offsets)
    try:
        argv = [FFMPEG, '-hide_banner', '-loglevel', 'error']
        rows = []
        tile = 0
        for number, (slug, edge) in enumerate(edges, start=1):
            origin = json.loads((work / slug / 'info.json').read_text())['video_origin_s']
            times = [edge + offset + origin for offset in offsets]
            labels = [f'{number} {offset:+.2f}' for offset in offsets]
            extract(sources[slug], times, labels, staging, f'r{number:02d}', args.width)
            for order in range(columns):
                argv += ['-i', str(staging / f'r{number:02d}_{order}.png')]
            inputs = ''.join(f'[{tile + order}:v]' for order in range(columns))
            rows.append(f'{inputs}hstack=inputs={columns}[row{number}]')
            tile += columns
        if len(edges) == 1:
            graph = ';'.join(rows).replace('[row1]', '[out]')
        else:
            stacked = ''.join(f'[row{number}]' for number in range(1, len(edges) + 1))
            graph = ';'.join(rows + [f'{stacked}vstack=inputs={len(edges)}[out]'])
        argv += ['-filter_complex', graph, '-map', '[out]', '-frames:v', '1', str(out)]
        subprocess.run(argv, check=True)
    finally:
        shutil.rmtree(staging, ignore_errors=True)

    for number, (slug, edge) in enumerate(edges, start=1):
        print('%-3d %-8s %8.2f  オフセット %s' % (number, slug, edge,
              ' '.join('%+.2f' % o for o in offsets)), flush=True)
    print('sheet:', out, flush=True)


if __name__ == '__main__':
    if not Path(FFMPEG).exists():
        FFMPEG = shutil.which('ffmpeg') or FFMPEG
    main()
