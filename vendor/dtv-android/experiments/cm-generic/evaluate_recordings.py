#!/usr/bin/env python3
"""Offline evaluation of the CM rules against saved recordings.

Two decisions are produced for every recording and kept apart:

* mark     - the production path: station mark detection plus structure rules.
* scenario - the logo-less path, called explicitly with no marker at all, so
             the two can be compared on their own.

The decode comparison of cm-verify is not part of this script: it proves the cut
followed the request, not that the request was right. Judging a decision needs
labelled material.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

BUNDLE = Path('/home/ku/pix-cm/current')
MARKERS = Path('/home/ku/pix-cm/markers')
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.append(str(BUNDLE))
import cm_rules  # noqa: E402
import cm_scenario  # noqa: E402
import cm_service  # noqa: E402


def service_of(source):
    """Service id and name from a recording path: GR_3219535864_テレビ静岡/..."""
    for part in Path(source).parts:
        fields = part.split('_')
        if len(fields) >= 2 and fields[0] in ('GR', 'BS'):
            return fields[1], (fields[2] if len(fields) > 2 else '')
    return None, None


def cached_marker(source, info):
    """The station mark the worker would use for this recording, if any.

    Measuring only the candidates cm-scan learned answers a different question
    than production asks: a service whose mark is cached but whose learner
    picked a telop elsewhere then looks like a service with no mark at all.

    This reads the cache instead of calling the worker's loader, which repairs an
    inconsistent cache. A measurement must not change the state it measures.
    """
    ident, name = service_of(source)
    if not ident:
        return None
    metadata = dict(service=dict(id=ident, name=name))
    config = dict(marker_dir=str(MARKERS))
    path, refs_dir = cm_service.marker_files(config, metadata, info)
    try:
        marker = json.loads(Path(path).read_text())
    except (OSError, ValueError):
        return None
    if not cm_service.marker_pair_valid(marker, refs_dir, info):
        return None
    marker['reference_paths'] = [str(refs_dir / name) for name in marker['references']]
    return marker


def run(argv, log, env=None):
    with Path(log).open('w') as handle:
        code = subprocess.run([str(item) for item in argv], stdout=handle,
                              stderr=subprocess.STDOUT, env=env).returncode
    if code:
        raise RuntimeError(f'{Path(argv[0]).name} exit={code}: {Path(log).read_text(errors="replace")[-800:]}')


def evaluate(source, work, label):
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
    learned = json.loads((features / 'logo.json').read_text())
    candidates = learned.get('candidates') or []
    env = dict(os.environ, CM_TIME_ORIGIN=str(info['video_origin_s']))
    # Production measures the cached station mark first, then the marks it
    # learned; analyze() hands overlays[0] to the logo-less path. Keeping the
    # cached mark first here means the two paths see what they see in the worker.
    passes = []
    cached = cached_marker(source, info)
    if cached:
        passes.append((cached['roi'], ';'.join(cached['reference_paths']),
                       features / 'ov-cached.csv'))
    for candidate in candidates:
        refs = ';'.join(str(features / name) for name in candidate['references'])
        passes.append((candidate['roi'], refs, features / f"ov-{candidate['index']:02d}.csv"))
    if passes:
        argv = [BUNDLE / 'cm-overlay', source]
        for roi, refs, output in passes:
            argv += [refs, *map(str, roi), str(output)]
        run(argv, work / 'overlay.log', env)
    overlays = []
    for _, _, output in passes:
        if output.exists() and len(output.read_text().splitlines()) > 1:
            overlays.append(cm_rules.read_rows(output))
    video = cm_rules.read_rows(features / 'video.csv')
    audio = cm_rules.read_rows(features / 'audio.csv')
    origin = info['video_origin_s']
    for row in video:
        row['pts'] -= origin
    for row in audio:
        row['pts'] -= origin
    duration = info['duration_seconds']
    options = cm_rules.settings()
    excluded = []
    summary = dict(label=label, duration=round(duration, 1), width=info['width'], height=info['height'],
                   codec=info['video_codec'], frames=info['video_frames'], candidates=len(candidates))
    mark = cm_rules.analyze(video, audio, overlays, duration, options)
    summary['mark'] = dict(mode=mark.get('mode'), reason=mark.get('reason'),
                           cuts=[[round(a, 1), round(b, 1)] for a, b in (mark.get('cuts') or [])],
                           present=round(mark.get('present_ratio') or 0, 3),
                           cut_ratio=round(mark.get('cut_ratio') or 0, 3))
    # The logo-less path is measured on its own: no overlay is passed, and the
    # decision function is called directly instead of going through analyze(),
    # which would return the mark result whenever the mark succeeded.
    scenario = cm_scenario.decide(video, audio, duration)
    summary['scenario'] = dict(candidates=scenario.get('candidates'),
                               cuts=[[round(a, 1), round(b, 1)] for a, b in scenario['cuts']],
                               scores=scenario.get('scores'))
    (work / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(summary, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    evaluate(sys.argv[1], sys.argv[2], sys.argv[3])
