#!/usr/bin/env python3
"""Builds a review bundle: one picture per batch of candidate blocks.

A person should only answer the question a machine cannot: "is this block a
commercial?" Everything else - finding the candidates, cutting the frames,
writing the label - is done here.

Blocks come from every decision path of each recording (repeat ``--summary``):
overlapping proposals are merged, so a block that two paths both proposed is
reviewed once. Each row of a sheet holds five frames of one block: two seconds
outside each edge and three points inside it. The block number is burned in.

    review_blocks.py --out review/ --summary a/summary.json --source a.mp4 [...]

Writes ``review/blocks.json`` (what was shown) and ``review/sheet-NN.png``.
Answer with ``apply_answers.py``.
"""
import argparse
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

FFMPEG = '/home/ku/work/TVerRec/bin/ffmpeg'
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
OUTSIDE = 2.0


def blocks_from(summary, slug, detector):
    """One entry per cut a detector proposed."""
    out = []
    for start, end in (summary.get(detector) or {}).get('cuts') or []:
        out.append(dict(slug=slug, start=float(start), end=float(end), detector=[detector]))
    return out


def merge(blocks):
    """One entry per overlapping proposal, keeping every detector that saw it."""
    merged = []
    for block in sorted(blocks, key=lambda item: (item['slug'], item['start'], item['end'])):
        current = merged[-1] if merged else None
        if current and current['slug'] == block['slug'] \
                and block['start'] < current['end'] and current['start'] < block['end']:
            current['start'] = min(current['start'], block['start'])
            current['end'] = max(current['end'], block['end'])
            current['detector'] = sorted(set(current['detector']) | set(block['detector']))
            continue
        merged.append(dict(block))
    return merged


def sample_points(block):
    """Two seconds outside each edge, and three points inside the block."""
    start, end = block['start'], block['end']
    middle = (start + end) / 2.0
    return [max(0.0, start - OUTSIDE), start + OUTSIDE, middle,
            end - OUTSIDE, end + OUTSIDE]


def extract(source, points, labels, directory, prefix, width):
    paths = []
    for order, (point, label) in enumerate(zip(points, labels)):
        target = directory / f'{prefix}_{order}.png'
        vf = (f'scale={width}:-2,drawtext=fontfile={FONT}:text={label}'
              f':x=6:y=6:fontsize=20:fontcolor=yellow:box=1:boxcolor=black@0.65')
        subprocess.run([FFMPEG, '-hide_banner', '-loglevel', 'error', '-ss', f'{point:.3f}',
                        '-i', str(source), '-frames:v', '1', '-vf', vf, str(target)],
                       check=True, capture_output=True)
        paths.append(target)
    return paths


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True)
    parser.add_argument('--summary', action='append', required=True)
    parser.add_argument('--source', action='append', required=True)
    parser.add_argument('--per-sheet', type=int, default=10)
    parser.add_argument('--width', type=int, default=360)
    args = parser.parse_args()
    if len(args.summary) != len(args.source):
        raise SystemExit('--summary and --source must be given the same number of times')

    sources, proposals = {}, []
    for summary_path, source in zip(args.summary, args.source):
        summary = json.loads(Path(summary_path).read_text())
        slug = Path(summary_path).parent.name
        sources[slug] = source
        for detector in ('mark', 'scenario'):
            proposals.extend(blocks_from(summary, slug, detector))
    blocks = merge(proposals)
    if not blocks:
        raise SystemExit('no candidate blocks')

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='review-', dir=str(out)))
    sheets = []
    try:
        for offset in range(0, len(blocks), args.per_sheet):
            batch = blocks[offset:offset + args.per_sheet]
            for index, block in enumerate(batch, start=1):
                number = offset + index
                labels = [f'#{number} out', f'#{number} in',
                          f'#{number} in', f'#{number} in', f'#{number} out']
                extract(sources[block['slug']], sample_points(block), labels, work,
                        f'{number:03d}', args.width)
            sheet = out / f'sheet-{len(sheets) + 1:02d}.png'
            rows = []
            argv = [FFMPEG, '-hide_banner', '-loglevel', 'error']
            for index in range(1, len(batch) + 1):
                frames = [work / f'{index + offset:03d}_{order}.png' for order in range(5)]
                missing = [path.name for path in frames if not path.is_file()]
                if missing:
                    raise SystemExit(f'missing frames: {missing}')
                for path in frames:
                    argv += ['-i', str(path)]
                first = 5 * (index - 1)
                argv_inputs = ''.join(f'[{first + order}:v]' for order in range(5))
                rows.append(f'{argv_inputs}hstack=inputs=5[r{index}]')
            stacked = ''.join(f'[r{index}]' for index in range(1, len(batch) + 1))
            filter_complex = ';'.join(rows + [f'{stacked}vstack=inputs={len(batch)}[out]'])
            argv += ['-filter_complex', filter_complex, '-map', '[out]', '-frames:v', '1',
                     str(sheet)]
            subprocess.run(argv, check=True)
            for path in work.glob('*.png'):
                path.unlink()
            sheets.append(sheet.name)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    for number, block in enumerate(blocks, start=1):
        block['id'] = number
        block['sheet'] = sheets[(number - 1) // args.per_sheet]
    (out / 'blocks.json').write_text(
        json.dumps(dict(blocks=blocks, sheets=sheets), ensure_ascii=False, indent=2) + '\n')
    for block in blocks:
        print('%-4d %-24s %7.1f-%7.1f  提案=%s' % (
            block['id'], block['slug'], block['start'], block['end'],
            ','.join(block['detector'])), flush=True)
    print('sheets:', ', '.join(sheets), flush=True)


if __name__ == '__main__':
    if not Path(FFMPEG).exists():
        FFMPEG = shutil.which('ffmpeg') or FFMPEG
    main()
