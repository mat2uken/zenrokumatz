#!/usr/bin/env python3
"""Scores CM decisions against labelled material.

The decode comparison of cm-verify proves a cut followed the request; this
script answers the other question: was the request right. It reports the four
measures agreed for the review:

* how many programmes lost programme time, and how much,
* how much of the labelled commercial time was removed,
* how often no decision was made at all.

Labels live in ``labels/*.json`` and are produced from logo-based cuts whose
boundaries were checked against still frames. They are machine assisted, not a
human ground truth, so the numbers are a lower bound on what a person would
find.

Usage:
    score_decisions.py <work-directory-root> [--labels labels]

The work directory root must contain one directory per label, named after the
label file. Each directory holds the ``features`` and ``info.json`` written by
``evaluate_recordings.py``.
"""
import argparse
import json
from pathlib import Path
import sys

BUNDLE = Path('/home/ku/pix-cm/current')
# The modules beside this script win over the deployed release: a measurement
# must run the rules under test, not whatever the worker is currently using.
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.append(str(BUNDLE))
import cm_rules  # noqa: E402
import cm_scenario  # noqa: E402


def overlap(a, b, others):
    """Seconds of [a, b] that fall inside the labelled commercial intervals."""
    return sum(max(0.0, min(b, hi) - max(a, lo)) for lo, hi in others)


def score(cuts, labelled, duration):
    total_cm = sum(end - start for start, end in labelled)
    removed_cm = sum(overlap(a, b, labelled) for a, b in cuts)
    outside = sum((b - a) - overlap(a, b, labelled) for a, b in cuts)
    return dict(
        programmes=1,
        cm_seconds=round(total_cm, 1),
        removed_cm_seconds=round(removed_cm, 1),
        cm_removed_ratio=round(removed_cm / total_cm, 3) if total_cm else 0.0,
        programme_seconds_removed=round(outside, 1),
        programme_lost=outside > 0.5,
    )


def main(root, labels_dir):
    root = Path(root)
    totals = dict(programmes=0, with_decision=0, programme_lost=0,
                  cm_seconds=0.0, removed_cm_seconds=0.0, programme_seconds_removed=0.0)
    rows = []
    for path in sorted(Path(labels_dir).glob('*.json')):
        label = json.loads(path.read_text())
        work = root / path.stem
        info_path = work / 'info.json'
        if not info_path.exists():
            rows.append((path.stem, 'no data', None, None))
            continue
        info = json.loads(info_path.read_text())
        origin = info['video_origin_s']
        video = cm_rules.read_rows(work / 'features' / 'video.csv')
        audio = cm_rules.read_rows(work / 'features' / 'audio.csv')
        for row in video:
            row['pts'] -= origin
        for row in audio:
            row['pts'] -= origin
        # The cached station mark is measured first, so it must stay overlays[0]:
        # analyze() hands that one to the logo-less path.
        cached = work / 'features' / 'ov-cached.csv'
        overlays = [cm_rules.read_rows(path) for path in
                    ([cached] if cached.exists() else [])
                    + sorted(path for path in (work / 'features').glob('ov-*.csv')
                             if path.name != 'ov-cached.csv')]
        duration = info['duration_seconds']

        mark = cm_rules.analyze(video, audio, overlays, duration)
        scenario = cm_scenario.decide(video, audio, duration)
        loose = cm_scenario.decide(video, audio, duration, {'scenario_spot_steps': True})

        totals['programmes'] += 1
        totals['cm_seconds'] += sum(end - start for start, end in label['cm'])
        for name, cuts in (('mark', mark.get('cuts') or []), ('scenario', scenario['cuts']),
                           ('scenario-loose', loose['cuts'])):
            result = score(cuts, label['cm'], duration)
            if name == 'mark':
                totals['with_decision'] += 1
                totals['programme_lost'] += 1 if result['programme_lost'] else 0
                totals['removed_cm_seconds'] += result['removed_cm_seconds']
                totals['programme_seconds_removed'] += result['programme_seconds_removed']
            rows.append((path.stem, name, result, cuts))
    print(json.dumps(dict(summary={k: round(v, 1) for k, v in totals.items()}), ensure_ascii=False))
    for name, what, result, cuts in rows:
        if result is None:
            print(f'{name:26s} {what}')
            continue
        print('%-26s %-15s CM除去%5.1f%% 本編削除%6.1fs 区間%s' % (
            name, what, 100 * result['cm_removed_ratio'], result['programme_seconds_removed'],
            [[round(a, 1), round(b, 1)] for a, b in cuts]))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('root')
    parser.add_argument('--labels', default='labels')
    args = parser.parse_args()
    main(args.root, args.labels)
