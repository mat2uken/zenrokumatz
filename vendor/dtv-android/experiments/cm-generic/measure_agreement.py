#!/usr/bin/env python3
"""Measures the logo-less (scenario) decision against the marker decision.

Any proposal to cut more recordings has to be judged by whether it agrees with
the path that publishes today, not by how many extra cuts it produces. This
tool produces both decisions from one scan of the same file, so the two paths
see exactly the same rows, and reports where they agree.

    measure_agreement.py <source.mp4> [<source.mp4> ...] [--marker-dir DIR]
                         [--threads N] [--json]

Each argument is one recording. The service is read from the recording's folder
name (``GR_3219535864_テレビ静岡``), as the recorder names it. A cut edge counts
as the same boundary when the two paths put it within TOL seconds.

Nothing is cut and nothing is published.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile

BUNDLE = Path(os.environ.get('CM_BUNDLE', Path(__file__).resolve().parent))
if str(BUNDLE) not in sys.path:
    sys.path.insert(0, str(BUNDLE))
import cm_rules        # noqa: E402
import cm_scenario     # noqa: E402
import cm_service      # noqa: E402

TOL = 1.5
SPOT = 15.0


def service_from_path(source):
    """Service id and name from the recorder's ``<KIND>_<id>_<name>`` folder.

    Only the directories are read: the file name starts with a clock and a part
    number, which would otherwise be taken for the service id.
    """
    for part in reversed(Path(source).parts[:-1]):
        fields = part.split('_', 2)
        if len(fields) == 3 and fields[1].isdigit() and not part.startswith('20'):
            return int(fields[1]), fields[2]
    raise ValueError(f'not a recording path: {source}')


def compare(scenario_cuts, mark_cuts, tolerance=TOL):
    """Matches scenario cuts to marker cuts, edge by edge."""
    matched, used = [], set()
    for start, end in scenario_cuts:
        hit = None
        for index, (other_start, other_end) in enumerate(mark_cuts):
            if abs(start - other_start) <= tolerance and abs(end - other_end) <= tolerance:
                hit = index
                used.add(index)
                break
        matched.append(hit)
    return dict(agree=sum(1 for hit in matched if hit is not None),
                extra=sum(1 for hit in matched if hit is None),
                missed=sum(1 for index in range(len(mark_cuts)) if index not in used))


def shape(cuts):
    """Widths of the cuts and the distance of each from a whole spot."""
    widths = [round(end - start, 2) for start, end in cuts]
    remainder = [round(min(width % SPOT, SPOT - width % SPOT), 2) for width in widths]
    return dict(count=len(widths), widths=widths, spot_remainder=remainder)


def measure(source, marker_dir, threads=4):
    """Both decisions for one recording."""
    service_id, name = service_from_path(source)
    config = dict(bundle=str(BUNDLE), marker_dir=str(marker_dir), threads=int(threads),
                  scan_windows=16, scan_references=3, timing_mode='detect',
                  ffmpeg=os.environ.get('CM_FFMPEG', 'ffmpeg'))
    metadata = dict(service=dict(id=service_id, name=name))
    stage = Path(tempfile.mkdtemp(prefix='agree-', dir=os.environ.get('CM_STAGE')))
    try:
        info = cm_service.inspect_source(BUNDLE, source, stage)
        features = stage / 'features'
        features.mkdir()
        env = cm_service.tool_env(config, dict(CM_TIME_ORIGIN=str(info['video_origin_s'])))
        cached = cm_service.load_marker(config, metadata, info)
        argv = [BUNDLE / 'cm-scan', source, features, str(int(threads)),
                '--duration', f"{info['duration_seconds']:.6f}",
                '--windows', str(config['scan_windows']),
                '--references', str(config['scan_references'])]
        if cached:
            argv += ['--overlay-roi', ','.join(str(int(value)) for value in cached['roi']),
                     '--overlay-references', ';'.join(cached['reference_paths']),
                     '--overlay-output', str(features / 'overlay-00.csv')]
        cm_service.command(argv, stage / 'scan.log', env)
        overlays = []
        if (features / 'overlay-00.csv').exists():
            overlays.append(cm_rules.read_rows(features / 'overlay-00.csv'))
        video = cm_rules.read_rows(features / 'video.csv')
        audio = cm_rules.read_rows(features / 'audio.csv')
        origin = info['video_origin_s']
        for row in video:
            row['pts'] -= origin
        for row in audio:
            row['pts'] -= origin
        if video and audio:
            shift = audio[0]['pts'] - video[0]['pts']
            if abs(shift) > 0.5:
                for row in audio:
                    row['pts'] -= shift
        duration = info['duration_seconds']
        options = cm_rules.settings()
        mark = cm_rules.analyze(video, audio, overlays, duration, options)
        # The timing path is told the mark analyze() would trust, not merely the
        # first candidate: that is what the production path does.
        logo = cm_rules.scenario_logo(overlays, options)
        scenario = cm_scenario.decide(video, audio, duration, options, logo)
        mark_cuts = [tuple(cut) for cut in (mark.get('cuts') or [])]
        scenario_cuts = [tuple(cut) for cut in (scenario.get('cuts') or [])]
        record = dict(source=os.path.basename(source), service=f'{service_id}_{name}',
                      minutes=round(duration / 60.0, 1), marker=cached['roi'] if cached else None,
                      mark=dict(mode=mark.get('mode'), reason=mark.get('reason'),
                                cuts=[[round(a, 1), round(b, 1)] for a, b in mark_cuts],
                                present_ratio=round(mark.get('present_ratio') or 0, 3),
                                confidence=round(mark.get('confidence') or 0, 3)),
                      scenario=dict(cuts=[[round(a, 1), round(b, 1)] for a, b in scenario_cuts],
                                    shape=shape(scenario_cuts)))
        if mark.get('mode') == 'overlay' and mark_cuts:
            record['agreement'] = compare(scenario_cuts, mark_cuts)
            record['mark_shape'] = shape(mark_cuts)
        return record
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def summarise(records):
    """How the scenario path compares with the marker path, over the sample."""
    pairs = [record for record in records if record.get('agreement')]
    totals = dict(compared=len(pairs),
                  mark_cuts=sum(len(record['mark']['cuts']) for record in pairs),
                  agree=sum(record['agreement']['agree'] for record in pairs),
                  extra=sum(record['agreement']['extra'] for record in pairs),
                  missed=sum(record['agreement']['missed'] for record in pairs))
    totals['clean'] = sum(1 for record in pairs if not record['agreement']['extra']
                          and not record['agreement']['missed'])
    return totals


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sources', nargs='+')
    parser.add_argument('--marker-dir', default=os.environ.get('CM_MARKER_DIR',
                                                              '/home/ku/pix-cm/markers'))
    parser.add_argument('--threads', default='4')
    args = parser.parse_args()
    records = [measure(source, args.marker_dir, args.threads) for source in args.sources]
    for record in records:
        print(json.dumps(record, ensure_ascii=False), flush=True)
    summary = summarise(records)
    print(json.dumps(dict(event='agreement_summary', **summary), ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
