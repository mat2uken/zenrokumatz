#!/usr/bin/env python3
"""Sweeps the coverage floor over one recording.

Runs the same measurement the worker runs, then decides the recording at
several coverage floors so the smallest relaxation that still produces sane
breaks can be seen.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

BUNDLE = Path('/home/ku/pix-cm/current')
sys.path.insert(0, str(BUNDLE))
import cm_rules  # noqa: E402

THRESHOLDS = [0.85, 0.82, 0.80, 0.78, 0.75, 0.72, 0.70, 0.65, 0.60]


def run(argv, log, env=None):
    with Path(log).open('w') as handle:
        code = subprocess.run([str(item) for item in argv], stdout=handle,
                              stderr=subprocess.STDOUT, env=env).returncode
    if code:
        raise RuntimeError(f'{Path(argv[0]).name} exit={code}: {Path(log).read_text(errors="replace")[-600:]}')


def main(source, work):
    work = Path(work)
    work.mkdir(parents=True, exist_ok=True)
    empty = work / 'empty.tsv'
    empty.write_text('')
    run([BUNDLE / 'cm-cut', source, work / 'unused.mp4', empty, work / 'info.json'], work / 'inspect.log')
    info = json.loads((work / 'info.json').read_text())
    features = work / 'features'
    features.mkdir(exist_ok=True)
    run([BUNDLE / 'cm-scan', source, features, '4',
         '--duration', f"{info['duration_seconds']:.6f}"], work / 'scan.log')
    candidates = json.loads((features / 'logo.json').read_text()).get('candidates') or []
    overlays = []
    if candidates:
        env = dict(os.environ, CM_TIME_ORIGIN=str(info['video_origin_s']))
        argv = [BUNDLE / 'cm-overlay', source]
        for candidate in candidates:
            refs = ';'.join(str(features / name) for name in candidate['references'])
            argv += [refs, *map(str, candidate['roi']), str(features / f"ov-{candidate['index']:02d}.csv")]
        run(argv, work / 'overlay.log', env)
        for candidate in candidates:
            path = features / f"ov-{candidate['index']:02d}.csv"
            if path.exists():
                overlays.append(cm_rules.read_rows(path))
    video = cm_rules.read_rows(features / 'video.csv')
    audio = cm_rules.read_rows(features / 'audio.csv')
    origin = info['video_origin_s']
    for row in video:
        row['pts'] -= origin
    for row in audio:
        row['pts'] -= origin
    duration = info['duration_seconds']
    print(json.dumps(dict(label=source[-40:], duration=round(duration, 1), candidates=len(candidates),
                          width=info['width'], height=info['height']), ensure_ascii=False))
    for threshold in THRESHOLDS:
        options = dict(cm_rules.settings(), min_coverage=threshold)
        result = cm_rules.analyze(video, audio, overlays, duration, options)
        cuts = [[round(a, 1), round(b, 1)] for a, b in (result.get('cuts') or [])]
        print("  floor %.2f -> %-26s cuts=%-2d %s ratio=%.3f" % (
            threshold, result.get('reason'), len(cuts), cuts[:4],
            result.get('cut_ratio') or 0), flush=True)


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
