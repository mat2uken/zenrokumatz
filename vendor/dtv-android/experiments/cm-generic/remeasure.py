#!/usr/bin/env python3
"""Re-run measurement and decision on an existing cm-scan directory."""
import json
from pathlib import Path
import subprocess
import sys

import cm_rules

BUNDLE = Path('/dev/shm/cm-build')


def main(directory):
    out = Path(directory)
    learned = json.loads((out / 'logo.json').read_text())
    candidates = learned.get('candidates') or []
    log = (out / 'measure.log').open('w')
    if candidates:
        argv = [str(BUNDLE / 'cm-overlay'), str(Path(sys.argv[2]))]
        for candidate in candidates:
            references = ';'.join(str(out / name) for name in candidate['references'])
            argv += [references, *map(str, candidate['roi']), str(out / f"ov-{candidate['index']:02d}.csv")]
        subprocess.run(argv, check=True, stdout=log, stderr=subprocess.STDOUT)
    video = cm_rules.read_rows(out / 'video.csv')
    audio = cm_rules.read_rows(out / 'audio.csv')
    origin = video[0]['pts']
    for row in video:
        row['pts'] -= origin
    for row in audio:
        row['pts'] -= origin
    duration = video[-1]['pts'] - video[0]['pts']
    overlays = []
    for candidate in candidates:
        path = out / f"ov-{candidate['index']:02d}.csv"
        if path.exists():
            overlays.append(cm_rules.read_rows(path))
    decision = cm_rules.analyze(video, audio, overlays, duration)
    decision['duration'] = round(duration, 2)
    decision['frame_size'] = [learned['width'], learned['height']]
    decision['audio_decode_failures'] = learned.get('audio_decode_failures')
    decision['candidates_geometry'] = [dict(index=c['index'], band=c['band'], roi=c['roi'],
                                            magnitude=round(c['magnitude'], 2),
                                            consistency=round(c['consistency'], 3)) for c in candidates]
    (out / 'decision.json').write_text(json.dumps(decision, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({k: decision.get(k) for k in
                      ('duration', 'mode', 'marker_index', 'reason', 'present_ratio', 'cut_ratio', 'cuts')},
                     ensure_ascii=False))


if __name__ == '__main__':
    main(sys.argv[1])
