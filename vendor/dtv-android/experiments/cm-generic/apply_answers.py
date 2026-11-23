#!/usr/bin/env python3
"""Turns a line of answers into labels, and reports each detector's precision.

The reviewer only writes what a machine cannot decide:

    apply_answers.py review/ "1=cm 2=prog 3=? 4-6=cm rest=prog"

* ``cm``   - all the inside frames are commercials, so the proposal is right
* ``prog`` - the inside frames are programme, so the proposal is wrong
* ``mix``  - both appear, so there is a commercial here but the edges are off
* ``?``    - cannot tell; excluded from the numbers

``rest=prog`` or ``rest=cm`` covers every block not named. Blocks judged ``cm``
are written to ``labels/<slug>.json`` in the label format ``score_decisions.py``
reads, merged with whatever the file already holds.

The report is the answer to "may this detector be published": how many of the
blocks it proposed were real commercials.
"""
import argparse
import json
from pathlib import Path
import sys

VERDICTS = ('cm', 'prog', 'mix', '?')


def parse(text):
    answers, default = {}, None
    for token in text.split():
        if '=' not in token:
            raise SystemExit(f'not an answer: {token}')
        ids, verdict = token.split('=', 1)
        verdict = verdict.strip().lower()
        if verdict not in VERDICTS:
            raise SystemExit(f'unknown verdict: {verdict}')
        if ids == 'rest':
            default = verdict
            continue
        for part in ids.split(','):
            if '-' in part:
                lo, hi = part.split('-', 1)
                for number in range(int(lo), int(hi) + 1):
                    answers[number] = verdict
            else:
                answers[int(part)] = verdict
    return answers, default


def write_labels(blocks, root):
    """Appends the blocks judged commercial to each recording's label file."""
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    by_slug = {}
    for block in blocks:
        if block.get('verdict') != 'cm':
            continue
        by_slug.setdefault(block['slug'], []).append([block['start'], block['end']])
    written = []
    for slug, spans in by_slug.items():
        path = root / f'{slug}.json'
        label = json.loads(path.read_text()) if path.exists() else {}
        existing = [tuple(span) for span in label.get('cm') or []]
        for span in spans:
            if not any(abs(span[0] - other[0]) <= 2.0 and abs(span[1] - other[1]) <= 2.0
                       for other in existing):
                existing.append(tuple(span))
        label['cm'] = [list(span) for span in sorted(existing)]
        label.setdefault('programme', slug)
        label.setdefault('station', '')
        label['verified'] = label.get('verified') or '目視確認（ブロック単位）'
        label.setdefault('source', '目視確認（ブロック単位）')
        path.write_text(json.dumps(label, ensure_ascii=False, indent=2) + '\n')
        written.append((slug, len(spans)))
    return written


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('review')
    parser.add_argument('answers')
    parser.add_argument('--labels', default='labels')
    args = parser.parse_args()

    review = Path(args.review)
    payload = json.loads((review / 'blocks.json').read_text())
    blocks = payload['blocks']
    answers, default = parse(args.answers)
    for block in blocks:
        verdict = answers.get(block['id'])
        if verdict is None:
            verdict = default if default is not None else '?'
        block['verdict'] = verdict

    tally = {}
    for block in blocks:
        for detector in block['detector']:
            counts = tally.setdefault(
                detector, dict(proposed=0, cm=0, prog=0, mix=0, unknown=0))
            counts['proposed'] += 1
            counts[block['verdict'] if block['verdict'] != '?' else 'unknown'] += 1

    print(json.dumps(dict(labels_root=str(args.labels), blocks=len(blocks),
                          answered=sum(1 for b in blocks if b['verdict'] != '?'),
                          unknown=sum(1 for b in blocks if b['verdict'] == '?'),
                          mixed=sum(1 for b in blocks if b['verdict'] == 'mix')),
                     ensure_ascii=False))
    for detector, counts in sorted(tally.items()):
        judged = counts['cm'] + counts['prog'] + counts['mix']
        precision = counts['cm'] / judged if judged else None
        print('%-9s 提案 %3d / 正しい %3d / 誤り %3d / 境界ずれ %3d / 不明 %3d  精度 %s' % (
            detector, counts['proposed'], counts['cm'], counts['prog'], counts['mix'],
            counts['unknown'], '不明' if precision is None else '%.0f%%' % (100 * precision)))

    # A block judged ``mix`` holds a commercial, but the edges are not right, so it
    # cannot become a label yet. Keeping the list lets a later pass refine exactly
    # those boundaries instead of reviewing everything again.
    mixed = [dict(slug=b['slug'], start=b['start'], end=b['end'], detector=b['detector'])
             for b in blocks if b.get('verdict') == 'mix']
    (review / 'mixed.json').write_text(
        json.dumps(dict(blocks=mixed), ensure_ascii=False, indent=2) + '\n')

    written = write_labels(blocks, args.labels)
    for slug, count in written:
        print('labels/%s.json に %d 区間を記録' % (slug, count), flush=True)
    if not written:
        print('CM と判定された区間は無し', flush=True)

    (review / 'blocks.json').write_text(
        json.dumps(dict(blocks=blocks, sheets=payload.get('sheets') or []),
                   ensure_ascii=False, indent=2) + '\n')


if __name__ == '__main__':
    sys.exit(main())
