#!/usr/bin/env python3
"""Continuously audit recordings, CM cuts and the recording services.

This is intentionally a sampler, not a second full decoder. For every new
recording it checks the container, decodes three short video windows, extracts
three frames into a contact sheet, and records the result. For every new CM
job it records the decision, validation and publication state, and (for a
small number of completed jobs) saves boundary frames for visual review.
Service, queue, disk and node health are checked on every run.

Results are append-only JSONL plus a summary.json under --audit-root. The
systemd timer runs it every ten minutes for as long as the NAS is up.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import urllib.request

SOURCE_ROOT = '/mnt/xblk/video/zenroku/recordings'
PUBLISH_ROOT = '/mnt/xblk/video/zenroku/recordings'
STATE_JOBS = '/home/ku/pix-cm/state/jobs'
AUDIT_ROOT = '/mnt/xblk/video/zenroku/audit'
FFMPEG = '/home/ku/work/TVerRec/bin/ffmpeg'
FFPROBE = '/home/ku/work/TVerRec/bin/ffprobe'
MANAGER = 'http://127.0.0.1:40776/api/nodes'
# A recorder that cannot get media still rewrites its staging stub, so a fresh
# mtime proves nothing. On 2026-11-23 a stalled BS11 node kept a 1212 byte stub
# with a two second old mtime for thirteen hours and this audit recorded
# node_progressing every ten minutes. A live capture writes about a megabyte a
# second, so a real staging file passes this floor within seconds.
STAGING_MIN_BYTES = 1 << 20
SERVICES = ['pix-stream-manager', 'pix-cm-worker@1', 'pix-cm-worker@2',
            'pix-cm-worker@3', 'pix-stb-guard']
TIMERS = ['pix-bs-deinterlace.timer']

PROFILES = {
    '3219535864': dict(name='テレビ静岡', codecs=('hevc', 'h264'), width=1440, height=1080, fps=29.97),
    '3219435856': dict(name='SBS1', codecs=('hevc', 'h264'), width=1440, height=1080, fps=29.97,
                       sizes=((1440, 1080), (1280, 736), (1280, 720))),
    # The NHK box records at 1440x1080 like the rest of the network; 1920 was
    # the old expectation and made every recording raise a size warning.
    '3219235840': dict(name='NHK', codecs=('h264',), width=1440, height=1080, fps=29.97,
                       sizes=((1440, 1080), (1920, 1080))),
    '3219435858': dict(name='SBS3', codecs=('hevc', 'h264'), width=1440, height=1080, fps=29.97,
                       sizes=((1440, 1080), (1280, 720))),
    '3219435857': dict(name='SBS2', codecs=('hevc', 'h264'), width=1440, height=1080, fps=29.97),
    '3219635872': dict(name='Daichi', codecs=('h264',), width=1440, height=1080, fps=29.97,
                       sizes=((1440, 1080), (1920, 1080))),
    # BS11 used to arrive field coded at 59.94 through the DT295 on-board
    # encoder. Since 2026-11-09 the box transcodes itself at 29.97, so both
    # shapes are accepted: the old recordings and the new ones.
    # The satellite reorganisation moved the BS11 feed to 1920x1080i around
    # 2026-11-22 (the source size now reaches the recorder because the STB
    # encoder carries no explicit scaler); 1440 and the old field-coded
    # shapes stay accepted for the recordings made before it.
    '400211': dict(name='BS11', codecs=('h264',), width=1920, height=1080, fps=29.97,
                   fps_alternatives=(59.94,),
                   sizes=((1920, 1080), (1440, 1080), (1280, 736))),
}
SERVICE_ID = re.compile(r'^(?:GR|BS)_(\d+)_')


def now_iso():
    return time.strftime('%Y-%m-%dT%H:%M:%S%z')


def log_event(log_path, severity, kind, **fields):
    line = json.dumps(dict(ts=now_iso(), severity=severity, kind=kind, **fields),
                      ensure_ascii=False)
    print(line, flush=True)
    try:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with log_path.open('a', encoding='utf-8') as handle:
            handle.write(line + '\n')
    except OSError:
        pass


def run(command, timeout=120):
    try:
        result = subprocess.run([str(item) for item in command], capture_output=True,
                                text=True, timeout=timeout)
        return result.returncode, result.stdout, result.stderr
    except (OSError, subprocess.TimeoutExpired) as error:
        return 255, '', str(error)


def load(path, default=None):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, ValueError):
        return default


def save(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + '\n')


def probe(path, ffprobe):
    code, out, err = run([ffprobe, '-v', 'error', '-show_format', '-show_streams',
                          '-of', 'json', path], timeout=60)
    if code:
        return None, err.strip()[-500:]
    try:
        return json.loads(out), None
    except ValueError as error:
        return None, str(error)


def profile_for(path):
    for part in Path(path).parts:
        match = SERVICE_ID.match(part)
        if match:
            return PROFILES.get(match.group(1))
    return None


def decode_sample(path, start, seconds, ffmpeg, timeout=120):
    code, _, err = run([ffmpeg, '-hide_banner', '-v', 'error', '-threads', '1',
                        '-ss', f'{start:.3f}', '-t', f'{seconds:.3f}', '-i', path,
                        '-map', '0:v:0', '-f', 'null', '-'], timeout=timeout)
    errors = [line for line in (err or '').splitlines() if line.strip()]
    return code, errors[:8], len(errors)


def extract_frame(path, at, out, ffmpeg):
    out.parent.mkdir(parents=True, exist_ok=True)
    code, _, err = run([ffmpeg, '-hide_banner', '-v', 'error', '-threads', '1',
                        '-ss', f'{at:.3f}', '-i', path, '-frames:v', '1',
                        '-vf', 'scale=480:-1', '-y', out], timeout=90)
    return code == 0 and out.is_file(), err.strip()[-300:]


def make_sheet(frames, out, ffmpeg):
    frames = [frame for frame in frames if frame and Path(frame).is_file()]
    if not frames:
        return None
    out.parent.mkdir(parents=True, exist_ok=True)
    if len(frames) == 1:
        shutil.copyfile(frames[0], out)
        return out
    command = [ffmpeg, '-hide_banner', '-v', 'error']
    for frame in frames:
        command += ['-i', str(frame)]
    command += ['-filter_complex', f'hstack=inputs={len(frames)}', '-y', str(out)]
    code, _, _ = run(command, timeout=60)
    return out if code == 0 and out.is_file() else None


def audit_recording(path, args, state, audit_root, log_path):
    stat = path.stat()
    key = str(path)
    old = (state.get('files') or {}).get(key) or {}
    if old.get('mtime') == stat.st_mtime and old.get('size') == stat.st_size:
        return None
    if time.time() - stat.st_mtime < args.min_age_minutes * 60:
        return None
    sidecar = path.with_suffix('.json')
    meta = load(sidecar, {}) or {}
    info, error = probe(path, args.ffprobe)
    if info is None:
        record = dict(path=key, error=f'ffprobe: {error}', mtime=stat.st_mtime,
                      size=stat.st_size, checked_at=now_iso())
        log_event(log_path, 'error', 'recording_unreadable', path=key, error=error)
        state.setdefault('files', {})[key] = dict(mtime=stat.st_mtime, size=stat.st_size,
                                                  checked_at=now_iso(), error=error)
        return record
    streams = info.get('streams') or []
    video = next((item for item in streams if item.get('codec_type') == 'video'), {})
    audio = next((item for item in streams if item.get('codec_type') == 'audio'), {})
    duration = float((info.get('format') or {}).get('duration') or 0)
    video_duration = float(video.get('duration') or 0)
    audio_duration = float(audio.get('duration') or 0)
    av_duration_delta = abs(video_duration - audio_duration)
    # Use the stream duration, not the container duration: a truncated tail
    # leaves the container longer than the video and made 59.94 fps look like
    # 41.67 fps on the BS11 233003 recording.
    video_span = video_duration or duration
    audio_span = audio_duration or duration
    format_gap = abs(duration - max(video_duration, audio_duration)) \
        if duration and (video_duration or audio_duration) else 0.0
    video_frames = int(video.get('nb_frames') or 0)
    audio_frames = int(audio.get('nb_frames') or 0)
    audio_fps = audio_frames / audio_span if audio_span else 0.0
    video_fps = video_frames / video_span if video_span else 0.0
    profile = profile_for(path)
    problems = []
    if duration <= 0:
        problems.append('zero_duration')
    if not video:
        problems.append('no_video')
    if not audio:
        problems.append('no_audio')
    if audio and audio_fps < 10:
        problems.append(f'audio_frames_low:{audio_fps:.2f}')
    if video_duration > 0 and audio_duration > 0 and av_duration_delta > 2.0:
        problems.append(f'av_duration_mismatch:{av_duration_delta:.1f}s')
    # A field-coded container can carry a duration derived from a wrapped PTS
    # (BS11 reported 26h for a 30min recording). Treat a container span far
    # beyond the stream span as a header artifact, not a recording gap.
    stream_span = max(video_duration, audio_duration)
    format_gap_actionable = format_gap > 2.0 and (
        stream_span <= 0 or duration <= stream_span * 8)
    if format_gap_actionable:
        problems.append(f'format_duration_gap:{format_gap:.1f}s')
    if profile:
        if video.get('codec_name') not in profile['codecs']:
            problems.append(f"codec:{video.get('codec_name')}")
        allowed_sizes = profile.get('sizes') or ((profile['width'], profile['height']),)
        actual_size = (int(video.get('width') or 0), int(video.get('height') or 0))
        if actual_size not in allowed_sizes:
            problems.append(f"size:{video.get('width')}x{video.get('height')}")
        # A missing stream section makes packets/duration look low. The A/V
        # mismatch is the useful signal; do not add a misleading fps error.
        # SBS1 and テレビ静岡 run at 25 fps on the two X96 boxes to keep their
        # encoders ahead of the tuner, so a 25 fps stream is intentional.
        near_25 = 24.0 <= video_fps <= 25.5
        if (av_duration_delta <= 2.0 and not near_25
                and min(abs(video_fps - candidate)
                        for candidate in (profile['fps'],) + tuple(profile.get('fps_alternatives') or ()))
                > max(2.0, profile['fps'] * 0.1)):
            problems.append(f'fps:{video_fps:.2f}')
    if meta.get('missingFragments'):
        problems.append(f"missing_fragments:{meta['missingFragments']}")
    if meta.get('state') not in (None, 'complete', 'partial'):
        problems.append(f"state:{meta.get('state')}")
    samples = []
    if duration > 0:
        points = [min(5.0, max(0.0, duration * 0.1))]
        if duration > 120:
            points += [max(1.0, duration * 0.5 - 10), max(1.0, duration - 20)]
        else:
            points += [max(1.0, duration * 0.8 - 5)]
        for point in sorted(set(round(value, 3) for value in points)):
            if point >= duration:
                continue
            seconds = min(20.0, max(5.0, duration - point))
            code, errors, count = decode_sample(path, point, seconds, args.ffmpeg)
            samples.append(dict(start=point, seconds=round(seconds, 3), code=code,
                                errors=count, first=errors))
            if code != 0 or count:
                problems.append(f'decode:{point:.1f}s:{count}')
    date = time.strftime('%Y/%m/%d', time.localtime(stat.st_mtime))
    digest = hashlib.sha1(key.encode()).hexdigest()[:12]
    channel = profile['name'] if profile else 'unknown'
    frame_dir = audit_root / 'frames' / date / channel
    frames = []
    if args.frames and duration > 0:
        for index, fraction in enumerate((0.1, 0.5, 0.9)):
            at = min(max(0.0, duration * fraction), max(0.0, duration - 0.5))
            frame = frame_dir / f'{digest}_{index}.png'
            ok, frame_error = extract_frame(path, at, frame, args.ffmpeg)
            if ok:
                frames.append(dict(at=round(at, 3), path=str(frame)))
            elif frame_error:
                problems.append(f'frame:{index}')
        sheet = make_sheet([item['path'] for item in frames], frame_dir / f'{digest}_sheet.png',
                           args.ffmpeg)
    else:
        sheet = None
    record = dict(path=key, channel=channel, duration=round(duration, 3),
                  av_duration_delta=round(av_duration_delta, 3),
                  format_gap=round(format_gap, 3),
                  video=dict(codec=video.get('codec_name'), profile=video.get('profile'),
                             width=video.get('width'), height=video.get('height'),
                             field_order=video.get('field_order'),
                             r_frame_rate=video.get('r_frame_rate'),
                             avg_frame_rate=video.get('avg_frame_rate'),
                             frames=video_frames, fps=round(video_fps, 3),
                             duration=round(video_duration, 3)),
                  audio=dict(codec=audio.get('codec_name'), sample_rate=audio.get('sample_rate'),
                             channels=audio.get('channels'), frames=audio_frames,
                             fps=round(audio_fps, 3),
                             duration=round(audio_duration, 3)),
                  state=meta.get('state'), missing=meta.get('missingFragments'),
                  stop=meta.get('stopReason'), samples=samples,
                  frames=frames, sheet=str(sheet) if sheet else None,
                  problems=problems, mtime=stat.st_mtime, size=stat.st_size,
                  checked_at=now_iso())
    with (audit_root / 'recordings.jsonl').open('a', encoding='utf-8') as handle:
        handle.write(json.dumps(record, ensure_ascii=False) + '\n')
    if problems:
        log_event(log_path, 'warn', 'recording_problem', path=key, problems=problems,
                  channel=channel)
    state.setdefault('files', {})[key] = dict(mtime=stat.st_mtime, size=stat.st_size,
                                              checked_at=now_iso(), problems=problems)
    return record


def job_duration(original):
    sidecar = Path(original).with_suffix('.json') if original else None
    if sidecar and sidecar.is_file():
        media = (load(sidecar, {}) or {}).get('media') or {}
        return float(media.get('durationMs') or 0) / 1000.0
    return 0.0


def audit_cm_job(result_path, args, state, audit_root, log_path, frame_budget):
    result = load(result_path, {}) or {}
    job_id = result_path.parent.name
    stat = result_path.stat()
    old = (state.get('cm_jobs') or {}).get(job_id) or {}
    if old.get('mtime') == stat.st_mtime:
        return None
    original = result.get('original') or ''
    channel = profile_for(original)['name'] if profile_for(original) else 'unknown'
    duration = job_duration(original)
    detection = result.get('detection') or {}
    cut = result.get('cut') or {}
    validation = result.get('validation') or {}
    output = result.get('output')
    problems = []
    if result.get('status') == 'completed':
        if output and not Path(output).exists():
            problems.append('published_output_missing')
        if validation.get('verified') is not True:
            problems.append('completed_without_validation')
        if validation.get('video_hash_mismatches'):
            problems.append('video_hash_mismatch')
        if validation.get('audio_hash_mismatches'):
            problems.append('audio_hash_mismatch')
    if result.get('status') == 'failed' and result.get('release_id'):
        problems.append('job_failed')
    frames = []
    if frame_budget[0] > 0 and result.get('status') == 'completed' and original and Path(original).is_file():
        cuts = (detection.get('cuts') or [])[:3]
        if cuts:
            date = time.strftime('%Y/%m/%d', time.localtime(stat.st_mtime))
            frame_dir = audit_root / 'cm_frames' / date
            for index, (start, end) in enumerate(cuts):
                points = [max(0.0, start - 2), start + 2, max(0.0, end - 2), end + 2]
                files = []
                for point_index, point in enumerate(points):
                    frame = frame_dir / f'{job_id}_cut{index}_{point_index}.png'
                    ok, _ = extract_frame(original, point, frame, args.ffmpeg)
                    if ok:
                        files.append(str(frame))
                sheet = make_sheet(files, frame_dir / f'{job_id}_cut{index}_sheet.png',
                                   args.ffmpeg)
                if sheet:
                    frames.append(dict(cut=[round(start, 3), round(end, 3)], sheet=str(sheet)))
            frame_budget[0] -= 1
    record = dict(job=job_id, channel=channel, status=result.get('status'),
                  reason=result.get('reason'), duration=round(duration, 3),
                  cuts=len(detection.get('cuts') or []), removed=cut.get('removed_seconds'),
                  confidence=detection.get('confidence'), verified=validation.get('verified'),
                  full_decode_ok=validation.get('full_decode_ok'),
                  video_hash_mismatches=validation.get('video_hash_mismatches'),
                  audio_hash_mismatches=validation.get('audio_hash_mismatches'),
                  audio_decode_warning=result.get('audio_decode_warning'),
                  field_coded=result.get('field_coded'), output=output,
                  original=original, frames=frames, problems=problems,
                  release_id=result.get('release_id'), mtime=stat.st_mtime,
                  checked_at=now_iso())
    with (audit_root / 'cm_jobs.jsonl').open('a', encoding='utf-8') as handle:
        handle.write(json.dumps(record, ensure_ascii=False) + '\n')
    if problems:
        log_event(log_path, 'warn', 'cm_job_problem', job=job_id, channel=channel,
                  problems=problems)
    state.setdefault('cm_jobs', {})[job_id] = dict(mtime=stat.st_mtime,
                                                   checked_at=now_iso(),
                                                   problems=problems)
    return record


def staging_progress(node_id):
    """Return the live staging file for a node's recorder.

    The recorder finalises the mp4 only at a program boundary, so pushed can
    stay at its old value for hours.  The staging file is written continuously;
    its size and mtime are the progress signal the audit should use.
    """
    host = str(node_id).split('-', 1)[0]
    code, out, _ = run(['ps', '-eo', 'pid,args'], timeout=10)
    if code != 0:
        return None
    for line in out.splitlines():
        if 'pix_stream_recorder' not in line or ('--stb-ip ' + host) not in line:
            continue
        parts = line.split(None, 1)
        if not parts:
            continue
        pid = parts[0]
        files = list(Path('/dev/shm/pix_stream').glob('staging_%s_*.fmp4' % pid))
        if not files:
            continue
        latest = max(files, key=lambda path: path.stat().st_mtime)
        stat = latest.stat()
        return dict(pid=int(pid), path=latest.name, bytes=stat.st_size,
                    mtime=round(stat.st_mtime),
                    age_sec=round(time.time() - stat.st_mtime, 1))
    return None


def service_health(args, log_path, repair):
    health = {}
    for service in SERVICES + TIMERS:
        code, out, _ = run(['systemctl', 'is-active', service], timeout=10)
        health[service] = out.strip() or 'unknown'
        if health[service] not in ('active', 'activating'):
            log_event(log_path, 'warn', 'service_not_active', service=service,
                      state=health[service])
            if repair and service in SERVICES:
                run(['sudo', '-n', 'systemctl', 'reset-failed', service], timeout=20)
                run(['sudo', '-n', 'systemctl', 'start', service], timeout=30)
                log_event(log_path, 'info', 'service_repair', service=service)
            elif repair and service in TIMERS:
                run(['sudo', '-n', 'systemctl', 'start', service], timeout=30)
                log_event(log_path, 'info', 'timer_repair', service=service)
    try:
        nodes = json.loads(urllib.request.urlopen(MANAGER, timeout=10).read().decode())
    except (OSError, ValueError) as error:
        nodes = []
        log_event(log_path, 'error', 'manager_unavailable', error=str(error))
    health['nodes'] = []
    for node in nodes:
        item = dict(id=node.get('id'), state=node.get('state'), pushed=node.get('pushed'),
                    error=node.get('stbError'),
                    dropped=(node.get('stb') or {}).get('totalDropped'),
                    spool=(node.get('stb') or {}).get('spoolUsedBytes'))
        item['staging'] = staging_progress(node.get('id'))
        health['nodes'].append(item)
    for node in health['nodes']:
        if node.get('state') != 'stalled':
            continue
        staging = node.get('staging') or {}
        progressing = (staging.get('bytes', 0) >= STAGING_MIN_BYTES
                       and staging.get('age_sec', 10 ** 9) <= 90)
        if progressing:
            log_event(log_path, 'info', 'node_progressing', id=node.get('id'),
                      pushed=node.get('pushed'), staging_bytes=staging.get('bytes'),
                      staging_age_sec=staging.get('age_sec'))
        else:
            log_event(log_path, 'warn', 'node_stalled', id=node.get('id'),
                      pushed=node.get('pushed'), spool=node.get('spool'),
                      staging_bytes=staging.get('bytes'),
                      staging_age_sec=staging.get('age_sec'))
    for node in health['nodes']:
        if node.get('state') not in ('error', 'stopped'):
            continue
        log_event(log_path, 'warn', 'node_error', id=node.get('id'),
                  state=node.get('state'), error=node.get('error'),
                  staging_bytes=(node.get('staging') or {}).get('bytes'))
    usage = shutil.disk_usage(SOURCE_ROOT)
    health['disk'] = dict(free=usage.free, total=usage.total,
                          free_percent=round(usage.free / usage.total * 100, 1))
    queue = Path('/dev/shm/pix_cm_queue')
    health['queue'] = dict(pending=len(list(queue.glob('*.json'))) if queue.is_dir() else 0,
                           failed=len(list((queue / 'failed').glob('*.json'))) if (queue / 'failed').is_dir() else 0)
    if health['disk']['free_percent'] < 10:
        log_event(log_path, 'error', 'disk_low', **health['disk'])
    if health['queue']['failed'] > 200:
        log_event(log_path, 'warn', 'cm_failed_backlog', **health['queue'])
    return health


EPG_DB = '/mnt/xblk/video/zenroku/recordings/_catalog/archive.sqlite'
EPG_STALE_HOURS = 6


def check_epg(log_path, now=None):
    """Warns when the programme guide stopped being collected.

    The archive fetches the guide from a tuner backend over HTTP. When that
    backend is down the guide freezes silently, every recording then falls back
    to a placeholder name, and the failure is only visible days later. The
    newest airing start is the direct measure of freshness.
    """
    import sqlite3
    import time
    if not os.path.exists(EPG_DB):
        log_event(log_path, 'warn', 'epg_database_missing', path=EPG_DB)
        return None
    try:
        connection = sqlite3.connect('file:%s?mode=ro' % EPG_DB, uri=True, timeout=10)
        row = connection.execute('select max(start_at) from airings').fetchone()
        connection.close()
    except Exception as error:
        log_event(log_path, 'warn', 'epg_query_failed', error=str(error)[-200:])
        return None
    newest = (row or [None])[0]
    if not newest:
        log_event(log_path, 'warn', 'epg_empty')
        return None
    age_hours = ((now or time.time()) - newest / 1000.0) / 3600.0
    if age_hours > EPG_STALE_HOURS:
        log_event(log_path, 'warn', 'epg_stale', newest_start_at=int(newest),
                  age_hours=round(age_hours, 1), database=EPG_DB)
    return dict(newest_start_at=int(newest), age_hours=round(age_hours, 1))


def summarize(audit_root, recordings, cm_jobs, health, log_path):
    summary = dict(updated_at=now_iso(), recordings_last_run=recordings,
                   cm_jobs_last_run=cm_jobs, health=health)
    save(audit_root / 'summary.json', summary)
    log_event(log_path, 'info', 'audit_summary', recordings=len(recordings),
              cm_jobs=len(cm_jobs), services=health)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', default=SOURCE_ROOT)
    parser.add_argument('--publish-root', default=PUBLISH_ROOT)
    parser.add_argument('--state-jobs', default=STATE_JOBS)
    parser.add_argument('--audit-root', default=AUDIT_ROOT)
    parser.add_argument('--ffmpeg', default=FFMPEG)
    parser.add_argument('--ffprobe', default=FFPROBE)
    parser.add_argument('--hours', type=float, default=2.0)
    parser.add_argument('--cm-hours', type=float, default=6.0)
    parser.add_argument('--limit', type=int, default=8)
    parser.add_argument('--cm-frame-jobs', type=int, default=2)
    parser.add_argument('--min-age-minutes', type=int, default=5)
    parser.add_argument('--frames', action='store_true', default=True)
    parser.add_argument('--no-frames', dest='frames', action='store_false')
    parser.add_argument('--repair', action='store_true')
    parser.add_argument('--once', action='store_true')
    args = parser.parse_args()
    source_root = Path(args.source_root)
    audit_root = Path(args.audit_root)
    audit_root.mkdir(parents=True, exist_ok=True)
    log_path = audit_root / 'audit.log'
    state_path = audit_root / 'state.json'
    state = load(state_path, {}) or {}
    cutoff = time.time() - args.hours * 3600
    recordings = []
    candidates = []
    for path in source_root.rglob('*__p01.mp4'):
        try:
            stat = path.stat()
        except OSError:
            continue
        if stat.st_mtime < cutoff:
            continue
        old = (state.get('files') or {}).get(str(path)) or {}
        if old.get('mtime') == stat.st_mtime and old.get('size') == stat.st_size:
            continue
        candidates.append((stat.st_mtime, path))
    eligible = [(mtime, path) for mtime, path in candidates
                if time.time() - mtime >= args.min_age_minutes * 60]
    for _, path in sorted(eligible)[:args.limit]:
        try:
            record = audit_recording(path, args, state, audit_root, log_path)
        except Exception as error:                       # noqa: BLE001 - keep the run alive
            log_event(log_path, 'error', 'recording_audit_exception', path=str(path),
                      error=str(error)[-500:])
            record = None
        if record:
            recordings.append(record)
    cm_jobs = []
    frame_budget = [args.cm_frame_jobs]
    cm_cutoff = time.time() - args.cm_hours * 3600
    for path in Path(args.state_jobs).glob('*/result.json'):
        try:
            stat = path.stat()
        except OSError:
            continue
        if stat.st_mtime < cm_cutoff:
            continue
        old = (state.get('cm_jobs') or {}).get(path.parent.name) or {}
        if old.get('mtime') == stat.st_mtime:
            continue
        try:
            record = audit_cm_job(path, args, state, audit_root, log_path, frame_budget)
        except Exception as error:                       # noqa: BLE001
            log_event(log_path, 'error', 'cm_audit_exception', job=path.parent.name,
                      error=str(error)[-500:])
            record = None
        if record:
            cm_jobs.append(record)
    health = service_health(args, log_path, args.repair)
    check_epg(log_path)
    summarize(audit_root, recordings, cm_jobs, health, log_path)
    save(state_path, state)
    return 0


if __name__ == '__main__':
    sys.exit(main())
