#!/usr/bin/env python3
"""Consume received fMP4 jobs; all intermediate files live on tmpfs.

The station mark is learned per recording, so no station or programme list is
needed. A commercial break is cut only when the overlay classification is
consistent and the break looks like a real break; the original MP4 is always
kept and validated packet by packet.
"""
import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time
import unicodedata

import cm_features
import cm_rules
# Imported here as well as inside cm_rules: a module missing from the release
# stops the worker at startup instead of failing one recording at a time.
import cm_scenario  # noqa: F401


def load(path):
    return json.loads(Path(path).read_text())


def save(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def require_tmpfs(path):
    # The staging file is removed when the recorder restarts, so a job that
    # was queued just before the restart can still reference a missing path.
    # findmnt -T fails on a missing path; probe the parent directory instead
    # so the job fails on its real validation, not on a transient cleanup.
    path = Path(path)
    probe = path if path.exists() else path.parent
    kind = subprocess.check_output(['findmnt', '-n', '-o', 'FSTYPE', '-T', str(probe)], text=True).strip()
    if kind != 'tmpfs':
        raise ValueError(f'tmpfs required: {path} ({kind})')


def command(argv, log, env=None):
    with Path(log).open('w') as handle:
        result = subprocess.run([str(item) for item in argv], stdout=handle, stderr=subprocess.STDOUT, env=env)
    if result.returncode:
        tail = Path(log).read_text(errors='replace')[-2000:]
        raise RuntimeError(f'{Path(argv[0]).name} exit={result.returncode}: {tail}')


def tool_env(config, extra=None):
    """Environment for the decode tools.

    The NVDEC gate in cm-scan / cm-overlay / cm-verify stays completely
    inert unless CM_GPU_DECODE is set, so the GPU is only ever touched when
    the configuration opted in ("decode_gpu": true). Everything else --
    the single-session lock, the health and temperature probe, the fallback
    to software decoding -- is enforced inside the tools themselves.
    """
    env = dict(os.environ)
    if extra:
        env.update(extra)
    if config.get('decode_gpu'):
        env['CM_GPU_DECODE'] = '1'
    return env


def final_copy(source, target):
    """Write once to the permanent filename. No disk staging or rename."""
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    with Path(source).open('rb') as src, target.open('xb') as dst:
        shutil.copyfileobj(src, dst, 8 * 1024 * 1024)
        dst.flush()
        os.fsync(dst.fileno())


def retain_test_input(job, config):
    if not config.get('retain_test_input'):
        return False
    until = config.get('retain_test_until')
    if until is None:
        return True
    start = job.get('metadata', {}).get('capture', {}).get('startAt')
    if start is None:
        start = job.get('queued_at', 0) * 1000
    return start < until * 1000


def inspect_source(bundle, source, stage):
    """Packet counts and duration without decoding."""
    empty = stage / 'empty.tsv'
    empty.write_text('')
    command([bundle / 'cm-cut', source, stage / 'unused.mp4', empty, stage / 'inspect.json'], stage / 'inspect.log')
    return load(stage / 'inspect.json')


def validate_remux(info, output_info):
    """Check that the remuxed MP4 carries the same content as the source fMP4.

    The container duration is not the same measure on both sides. With
    B-frames the MP4 muxer folds the reorder delay into the reported duration,
    so the same content reports a few frames more (measured: +0.083 s on a
    61-minute AVC program, +0.311 s on a 102-minute HEVC program) while the
    frame count, the audio packet count and the PTS span match exactly. Judge
    fidelity by those three and keep only a loose bound on the duration, so a
    faithful copy is not rejected as a corruption. A duration difference above
    one second is treated as a damaged tail, not as benign muxer jitter: the
    116-minute SBS1 recording that failed on 2026-11-05 also had 2052 decode
    errors in its last 20 seconds.
    """
    for field in ('video_frames', 'audio_packets', 'width', 'height', 'video_codec'):
        if info[field] != output_info[field]:
            raise ValueError(f'remux mismatch: {field}')
    if abs(info['span_seconds'] - output_info['span_seconds']) > 0.05:
        raise ValueError('remux mismatch: span')
    if abs(info['duration_seconds'] - output_info['duration_seconds']) > 1.0:
        raise ValueError('remux mismatch: duration')


def av_duration_mismatch(video_duration, audio_duration, tolerance=2.0):
    """True when one stream is materially shorter than the other.

    The packet-count remux check proves the MP4 carries the same bytes as the
    fMP4, but it cannot see that the capture itself lost the audio tail. A
    damaged DT295 recording on 2026-11-05 had 971 s of video and 752 s of
    audio; cutting it would publish a programme with no sound at the end.
    """
    return (video_duration > 0 and audio_duration > 0
            and abs(video_duration - audio_duration) > tolerance)


def filter_short_cuts(cuts, minimum=15.0, tolerance=None):
    """Split detected breaks into publishable and too-short ones.

    A 12-second logo-less stretch at a news studio transition or inside a
    shopping programme is not a commercial spot. The overlay detector accepted
    those on 2026-11-06 and published 12/24-second cuts of news and shopping
    content. Standard spots are 15 seconds or longer, so anything shorter is
    kept as original only.

    The floor uses the spot model the detector already applied: a short break
    is a whole number of 15 second spots within short_break_tolerance. Judging
    the floor without that tolerance threw away three real spots in six days
    (14.98, 14.45 and 14.01 s) purely because their edges had been snapped a
    fraction early, so the floor now allows the same tolerance.
    """
    if tolerance is None:
        tolerance = float(cm_rules.DEFAULTS['short_break_tolerance'])
    floor = max(0.0, float(minimum) - float(tolerance))
    kept = []
    rejected = []
    for start, end in cuts:
        if end - start >= floor:
            kept.append((start, end))
        else:
            rejected.append((start, end))
    return kept, rejected


NEWS_OR_SHOPPING = ('NEWS', 'ニュース', '報道', 'ディノス', 'テレショップ', 'ショップチャンネル')


def cut_minimum(title, config):
    """Minimum break length for this programme.

    A 15-second logo-less stretch in NEWS24 or a shopping programme is usually
    a studio transition or the programme itself, not a spot. The overlay
    detector published 15.4-second news and 14.9-second shopping cuts on
    2026-11-06. Use a 30-second floor for those programme types; normal
    programmes keep the configured 15-second floor.
    """
    normalized = unicodedata.normalize('NFKC', title or '')
    base = float(config.get('min_cut_seconds', 15.0))
    if any(word in normalized for word in NEWS_OR_SHOPPING):
        return max(base, 30.0)
    return base


def probe_stream_durations(path, ffprobe):
    """Return (video_duration, audio_duration) from an MP4, or None.

    This is a safety check, not a validation gate: a missing ffprobe or a
    stream without a duration must not stop the normal packet-count path.
    """
    try:
        result = subprocess.run(
            [str(ffprobe), '-v', 'error', '-show_entries', 'stream=codec_type,duration',
             '-of', 'json', str(path)], capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode:
        return None
    try:
        streams = json.loads(result.stdout).get('streams') or []
        video = next((item for item in streams if item.get('codec_type') == 'video'), {})
        audio = next((item for item in streams if item.get('codec_type') == 'audio'), {})
        return float(video.get('duration') or 0), float(audio.get('duration') or 0)
    except (TypeError, ValueError):
        return None


def audio_warning_is_acceptable(validation, cut_error, config):
    """Allow a cut whose only failed check is AAC decoding the source.

    The packet-level fallback has already proved that every video frame and
    audio packet in the cut is byte-identical to the source. Some broadcast
    AAC streams use features the reference decoder does not implement (gain
    control, PNS), so a full decode fails even though the cut is a faithful
    subset. The caller must opt in, and the packet comparison must be clean.
    """
    if not config.get('allow_audio_decode_warning', False):
        return False
    if not cut_error:
        return False
    if validation.get('verified') is not True:
        return False
    if validation.get('full_decode_ok') is not False:
        return False
    if validation.get('video_hash_mismatches') != 0:
        return False
    if validation.get('audio_hash_mismatches') != 0:
        return False
    if validation.get('video_max_pts_error_ms', 1e9) > 1.0:
        return False
    if validation.get('audio_max_pts_error_ms', 1e9) > 1.0:
        return False
    return True


def ffmpeg_copy(source, target, log, config):
    """Best-effort plain remux used when the normaliser rejects the fMP4.

    A damaged tail (for example a reboot during capture) makes cm-cut stop.
    ffmpeg can often still write the playable prefix, which is better than
    losing the only copy of the programme.
    """
    ffmpeg = config.get('ffmpeg') or shutil.which('ffmpeg')
    if not ffmpeg:
        return False
    try:
        command([ffmpeg, '-hide_banner', '-v', 'error', '-i', source,
                 '-c', 'copy', '-movflags', '+faststart', '-y', target], log)
    except RuntimeError:
        return False
    return Path(target).is_file() and Path(target).stat().st_size > 0


def preserve_source(config, job, source):
    """Copy an unconvertible fMP4 to persistent storage before tmpfs loses it."""
    root = Path(config['publish_root']) / '.recovery'
    root.mkdir(parents=True, exist_ok=True)
    target = root / (job['metadata']['recordingId'] + Path(source).suffix)
    if target.exists():
        return target
    try:
        with Path(source).open('rb') as src, target.open('xb') as dst:
            shutil.copyfileobj(src, dst, 8 * 1024 * 1024)
            dst.flush()
            os.fsync(dst.fileno())
    except OSError:
        return None
    return target


def save_original_file(job, metadata, original, result):
    """Publish the MP4 once, never overwriting a completed original."""
    target = Path(job['original_mp4'])
    sidecar = Path(job['original_json'])
    if sidecar.exists() and load(sidecar).get('recordingId') == metadata['recordingId']:
        if not target.is_file():
            raise ValueError('completed original is missing')
    else:
        final_copy(original, target)
        save(sidecar, metadata)
    result.update(original_saved=True, original=str(target))


def field_coded_stream(info, detection):
    """True when the container carries two access units per decoded picture.

    DT295's AVC copy is PAFF: the MP4 packet count is twice the number of
    pictures the scanner decodes. The cutter works in packets and produces a
    two-frame-short, hash-mismatched cut, so CM cutting is not attempted until
    a field-aware cutter exists.
    """
    pictures = (detection or {}).get('video_frames') or 0
    packets = info.get('video_frames') or 0
    return pictures > 0 and packets >= int(pictures * 1.8)


def marker_key(metadata, info):
    """One learned marker per service and picture size."""
    service = metadata.get('service') or {}
    ident = service.get('id') or service.get('serviceId')
    if not ident:
        import unicodedata
        ident = unicodedata.normalize('NFKC', service.get('name') or 'unknown').replace('/', '_')
    return f"{ident}_{int(info['width'])}x{int(info['height'])}"


def marker_files(config, metadata, info):
    root = Path(config.get('marker_dir', '/home/ku/pix-cm/markers'))
    key = marker_key(metadata, info)
    return root / f"{key}.json", root / key


def marker_stats_path(config, metadata, info):
    root = Path(config.get('marker_dir', '/home/ku/pix-cm/markers'))
    return root / f"{marker_key(metadata, info)}.stats.json"


def pgm_dimensions(header):
    """Width and height from an 8-bit binary PGM header, or None.

    cm-scan accepts only this shape ("reference requires 8-bit P5 PGM"), so a
    crop that carries anything else can never be measured.
    """
    if len(header) < 4 or header[0] != b'P5' or header[3] != b'255':
        return None
    try:
        return int(header[1]), int(header[2])
    except ValueError:
        return None


def pgm_size(path):
    """Width and height of a PGM on disk, or None when it cannot be read."""
    try:
        with Path(path).open('rb') as handle:
            return pgm_dimensions(handle.read(64).split())
    except OSError:
        return None


def store_references(refs_dir, blobs):
    """Writes reference crops under names that are never rewritten.

    A crop used to be named after the learner's candidate index (ref-00-0.pgm)
    or after the spare slot (alt-0.pgm). Both names are reused by the next
    learning run, so a later write could replace the pixels of a marker that
    was already cached while its roi stayed behind. The cache then described
    two geometries at once and cm-scan refused the whole recording with
    "insufficient reference logo edges". Naming a crop by its own content makes
    a stored pair immutable: the marker and the alternative can only share a
    file when they share the picture.
    """
    refs_dir = Path(refs_dir)
    refs_dir.mkdir(parents=True, exist_ok=True)
    names = []
    for index, blob in enumerate(blobs):
        name = f'ref-{hashlib.sha1(blob).hexdigest()[:10]}-{index}.pgm'
        target = refs_dir / name
        if not target.exists():
            target.write_bytes(blob)
        names.append(name)
    return names


def marker_pair_valid(marker, refs_dir, info=None):
    """True when the roi and every reference describe the same geometry.

    cm-scan reads a reference directly only when it is exactly the roi's size;
    every other picture is rescaled as if it were the whole frame. A crop taken
    from a different roi therefore lands on flat video, no edge survives, and
    the scan refuses the file before a single rule runs.
    """
    roi = marker.get('roi') or []
    if len(roi) != 4:
        return False
    x, y, w, h = (int(value) for value in roi)
    if w <= 0 or h <= 0 or x < 0 or y < 0:
        return False
    allowed = {(w, h)}
    frame = tuple(int(value) for value in marker.get('size') or [])
    if len(frame) != 2 and info:
        frame = (int(info['width']), int(info['height']))
    if len(frame) == 2:
        allowed.add(frame)
    names = marker.get('references') or []
    if not names:
        return False
    return all(pgm_size(Path(refs_dir) / name) in allowed for name in names)


MARKER_LOCKS = {}


@contextlib.contextmanager
def marker_lock(config, metadata, info):
    """Serialises the cache updates of the workers that share one key.

    Three workers pick jobs for the same service, so their read-modify-write of
    the marker and of its stats interleaves: two workers stored different
    alternatives for BS11 within the same minute and each reset the other's
    counters. The content names keep the pixels apart; this keeps the records
    of them from being lost. Re-entrant, because the helpers call each other
    while holding it.
    """
    key = marker_key(metadata, info)
    if key in MARKER_LOCKS:
        yield
        return
    root = Path(config.get('marker_dir', '/home/ku/pix-cm/markers'))
    root.mkdir(parents=True, exist_ok=True)
    with (root / f'{key}.lock').open('a+') as handle:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
        MARKER_LOCKS[key] = True
        try:
            yield
        finally:
            MARKER_LOCKS.pop(key, None)
            fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def load_marker_stats(config, metadata, info):
    """Per-service marker trust bookkeeping.

    Learned markers jump between whatever stable patch each programme happened
    to show (station clock, data broadcast, telops), so a marker that passed
    once can mislead every later programme on the service. The stats track how
    often the cached marker actually survived the rules, which is what decides
    whether a newly learned marker may replace it.
    """
    path = marker_stats_path(config, metadata, info)
    try:
        stats = load(path)
        if stats.get('version') == 1:
            return stats
    except (OSError, ValueError):
        pass
    return dict(version=1, current_roi=None, confirms=0, fails=0, alt=None)


def save_marker_stats(config, metadata, info, stats):
    stats['version'] = 1
    save(marker_stats_path(config, metadata, info), stats)


def confirm_marker(config, metadata, info):
    """The cached marker survived the rules on this recording."""
    stats = load_marker_stats(config, metadata, info)
    stats['confirms'] = int(stats.get('confirms') or 0) + 1
    stats['fails'] = 0
    save_marker_stats(config, metadata, info, stats)


def promote_alternative(config, metadata, info, reason):
    """Makes the stored alternative the cached marker when it is usable.

    The alternative is the other mark this service has already shown, so it is
    the cheapest way back from a cache that no longer describes the video. The
    crops are copied to their own content names so the new cache entry does not
    share a file with the slot the next alternative will be written to.
    """
    meta_path, refs_dir = marker_files(config, metadata, info)
    stats = load_marker_stats(config, metadata, info)
    alt = stats.get('alt') or {}
    names = alt.get('references') or []
    if not names or not all((refs_dir / name).is_file() for name in names):
        return None
    if not marker_pair_valid(alt, refs_dir, info):
        # An alternative that disagrees with itself is no better than the entry
        # it was meant to replace; drop it instead of promoting it forever.
        stats['alt'] = None
        stats['current_roi'] = None
        save_marker_stats(config, metadata, info, stats)
        print(json.dumps(dict(event='marker_alt_unusable', roi=alt.get('roi'))), flush=True)
        return None
    demoted = None
    try:
        existing = load(meta_path)
        if marker_pair_valid(existing, refs_dir, info):
            demoted = dict(roi=existing.get('roi'), references=existing['references'],
                           band=existing.get('band'))
    except (OSError, ValueError):
        demoted = None
    written = store_references(refs_dir, [(refs_dir / name).read_bytes() for name in names])
    save(meta_path, dict(roi=alt['roi'], references=written, band=alt.get('band'),
                         service=(metadata.get('service') or {}).get('name'),
                         size=[int(info['width']), int(info['height'])]))
    stats['current_roi'] = alt['roi']
    stats['confirms'] = 0
    stats['fails'] = 0
    stats['alt'] = demoted
    save_marker_stats(config, metadata, info, stats)
    print(json.dumps(dict(event='marker_promoted', reason=reason, roi=alt['roi'])), flush=True)
    return dict(roi=alt['roi'], references=written, band=alt.get('band'))


def fail_marker(config, metadata, info):
    """The cached marker did not fit this recording.

    A marker that keeps failing is demoted in favour of the stored alternative,
    so one programme's telop cannot keep poisoning the cache.
    """
    with marker_lock(config, metadata, info):
        stats = load_marker_stats(config, metadata, info)
        stats['fails'] = int(stats.get('fails') or 0) + 1
        # Persist the count before a promotion can reset it: the failure is
        # evidence about the marker that was used, not about the one that
        # replaces it.
        save_marker_stats(config, metadata, info, stats)
        if stats['fails'] >= 2 and (stats.get('alt') or {}).get('references'):
            promoted = promote_alternative(config, metadata, info, 'cached marker failed twice')
            return promoted is not None
        return False


def marker_survived(decision):
    """True when the cached marker was measured and reached a verdict.

    A programme with no commercial break is not evidence against the mark: the
    mark was on screen, the rules looked for an absent run and found none.
    Counting that as a failure demoted good caches on ad-free programmes, so
    the alternative and the current mark swapped for no reason. The recording is
    still re-learned when the mark found no break; only the trust counter is
    spared.
    """
    if decision.get('mode') == 'overlay':
        return True
    return any(entry.get('reason') == 'no_breaks'
               for entry in (decision.get('candidates') or []))


def load_marker(config, metadata, info):
    """Returns the cached marker, or None when there is nothing usable.

    A cache whose roi and reference crops disagree is rebuilt from its own
    alternative here, before it can be handed to cm-scan. Such a pair makes
    cm-scan abort with "insufficient reference logo edges" before any rule
    runs, and because nothing downstream ran, the failure never reached the
    trust counters: the same broken cache was reused for every later recording
    on the service and none of them could ever be analysed.
    """
    meta_path, refs_dir = marker_files(config, metadata, info)
    try:
        marker = load(meta_path)
    except (OSError, ValueError):
        marker = None
    if marker is None or not marker_pair_valid(marker, refs_dir, info):
        if marker is not None:
            print(json.dumps(dict(event='marker_cache_inconsistent', roi=marker.get('roi'),
                                  references=marker.get('references'))), flush=True)
        with marker_lock(config, metadata, info):
            marker = promote_alternative(config, metadata, info, 'inconsistent cache')
        if marker is None:
            return None
    marker['reference_paths'] = [str(refs_dir / name) for name in marker['references']]
    return marker


def marker_is_plausible(roi, info):
    """A station watermark is a few dozen pixels wide, never a few pixels."""
    x, y, w, h = roi
    min_w = max(24.0, info['width'] / 36.0)
    min_h = max(12.0, info['height'] / 54.0)
    if w < min_w or h < min_h:
        return False
    ratio = w / max(1.0, float(h))
    return 0.5 <= ratio <= 8.0


def save_marker(config, metadata, info, candidate, features):
    """Caches the marker that survived the decision, with its references.

    A marker only replaces the cached one outright when the cache is absent,
    describes the same roi, is unusable, or has already failed twice in a row.
    A newly learned marker from a cache that still works is stored as the
    alternative: it may be this programme's telop rather than the station mark,
    and one programme is not evidence enough to poison a working cache.
    """
    roi = candidate.get('roi') or [0, 0, 0, 0]
    if not marker_is_plausible(roi, info):
        # A flat corner patch can correlate with noise and win on a fragment.
        # Caching it would then mislead every later programme on this service.
        print(json.dumps(dict(event='marker_cache_skipped', roi=candidate.get('roi'),
                              reason='implausible roi')), flush=True)
        return
    meta_path, refs_dir = marker_files(config, metadata, info)
    blobs = []
    for name in candidate['references']:
        source = Path(features) / name
        if not source.is_file():
            return
        blobs.append(source.read_bytes())
    # Never cache a pair that cm-scan could not read back: the roi and its
    # crops must describe the same rectangle.
    if not all(pgm_dimensions(blob[:64].split()) == (int(roi[2]), int(roi[3]))
               for blob in blobs):
        print(json.dumps(dict(event='marker_cache_skipped', roi=roi,
                              reason='reference size mismatch')), flush=True)
        return
    with marker_lock(config, metadata, info):
        try:
            stats = load_marker_stats(config, metadata, info)
            existing = None
            try:
                existing = load(meta_path)
            except (OSError, ValueError):
                existing = None
            same_roi = existing is not None and existing.get('roi') == candidate.get('roi')
            usable = existing is not None and marker_pair_valid(existing, refs_dir, info)
            replace = (existing is None or same_roi or not usable
                       or int(stats.get('fails') or 0) >= 2)
            names = store_references(refs_dir, blobs)
            if not replace:
                # Store as the alternative under its own names, leaving the
                # working cache untouched.
                stats['alt'] = dict(roi=candidate.get('roi'), references=names,
                                    band=candidate.get('band'))
                save_marker_stats(config, metadata, info, stats)
                print(json.dumps(dict(event='marker_alt_stored', roi=candidate.get('roi'),
                                      existing=existing.get('roi'))), flush=True)
                return
            save(meta_path, dict(roi=candidate['roi'], references=names, band=candidate.get('band'),
                                 service=(metadata.get('service') or {}).get('name'),
                                 size=[int(info['width']), int(info['height'])]))
            if not same_roi:
                stats['current_roi'] = candidate['roi']
                stats['confirms'] = 0
                stats['fails'] = 0
                save_marker_stats(config, metadata, info, stats)
            prune_references(config, metadata, info, refs_dir)
        except OSError as error:
            print(json.dumps(dict(event='marker_cache_failed', error=str(error))), flush=True)


def prune_references(config, metadata, info, refs_dir):
    """Drops reference crops that no cache entry points at any more.

    Content names never repeat, so a service that keeps learning new marks
    would otherwise accumulate one small PGM set per programme forever. The
    files are kept for a while first: a job that is still running may have
    loaded the old pair and must be able to read it to the end.
    """
    days = float(config.get('marker_prune_days', 7) or 0)
    if days <= 0:
        return
    keep = set()
    meta_path, _ = marker_files(config, metadata, info)
    try:
        keep.update(load(meta_path).get('references') or [])
    except (OSError, ValueError):
        pass
    alt = (load_marker_stats(config, metadata, info).get('alt') or {}).get('references') or []
    keep.update(alt)
    limit = time.time() - days * 86400
    for path in Path(refs_dir).glob('*.pgm'):
        if path.name in keep:
            continue
        try:
            if path.stat().st_mtime < limit:
                path.unlink()
        except OSError:
            continue


def seed_marker_from_logo(config, metadata, info, source, stage):
    """Fill the marker cache from the station logo the recorder serves.

    Learning the mark is what fails on short or heavily covered recordings;
    the logo gives the shape, so only its position has to be found. Failures
    are logged and ignored: a recording must never depend on a logo.
    """
    if not (config.get('logo_source') or {}).get('enabled'):
        return False
    try:
        import cm_logos  # numpy is only needed here
        meta_path, refs_dir = marker_files(config, metadata, info)
        if meta_path.exists():
            return False
        return cm_logos.seed_marker(config, metadata, info, source, stage, meta_path, refs_dir,
                                    config.get('ffmpeg', 'ffmpeg'), store=store_references)
    except Exception as error:  # noqa: BLE001 - a logo is never worth failing a job
        print(json.dumps(dict(event='logo_seed_failed', error=str(error))), flush=True)
        return False


def scan_produced_frames(features):
    """True once the scan has written at least one video row.

    cm-scan opens video.csv before it decodes anything, so a table that holds
    nothing but its header means the pass stopped before the first frame: the
    inputs were refused, and running it again without them is cheap. A table
    with rows means the decode itself failed, which is not worth repeating.
    """
    try:
        with (Path(features) / 'video.csv').open(errors='replace') as handle:
            return sum(1 for _ in handle) > 1
    except OSError:
        return False


def known_candidates(config, metadata, info, features, skip_roi=None):
    """Station marks already known for this service, ready to be measured.

    The cached mark is measured inside the feature pass, but the alternative is
    only looked at after the trusted mark has failed. Measuring it in the same
    cm-overlay pass as the learned candidates costs no extra decode, and it lets
    the rules judge a real station mark before a telop that the learner happened
    to pick. The crops are copied into the feature directory so a candidate that
    wins can be cached by the usual path.
    """
    features = Path(features)
    _, refs_dir = marker_files(config, metadata, info)
    stats = load_marker_stats(config, metadata, info)
    alt = stats.get('alt') or {}
    roi = alt.get('roi')
    names = alt.get('references') or []
    if not roi or not names:
        return []
    if skip_roi and tuple(int(value) for value in roi) == tuple(int(value) for value in skip_roi):
        return []
    if not marker_pair_valid(alt, refs_dir, info):
        return []
    paths = [refs_dir / name for name in names]
    if not all(path.is_file() for path in paths):
        return []
    copied = []
    for index, path in enumerate(paths):
        name = f'known-0-{index}{path.suffix}'
        (features / name).write_bytes(path.read_bytes())
        copied.append(name)
    return [dict(roi=roi, band=alt.get('band'), references=copied,
                 paths=[features / name for name in copied],
                 magnitude=0.0, consistency=0.0, known=True)]


def learned_candidates(features):
    """The overlay candidates the scan proposed for this recording."""
    features = Path(features)
    learned = load(features / 'logo.json')
    candidates = []
    for candidate in learned.get('candidates') or []:
        paths = [features / name for name in candidate['references']]
        if not paths:
            continue
        candidates.append(dict(roi=candidate['roi'], band=candidate.get('band'),
                               references=list(candidate['references']), paths=paths,
                               magnitude=candidate.get('magnitude', 0.0),
                               consistency=candidate.get('consistency', 0.0)))
    return candidates


def overlay_pass(bundle, source, features, stage, env, log_name, candidates):
    """Measures every candidate in one cm-overlay pass, numbered to match."""
    features = Path(features)
    indexed = []
    argv = [bundle / 'cm-overlay', source]
    for candidate in candidates:
        if not candidate.get('paths'):
            continue
        index = len(indexed)
        argv += [';'.join(str(path) for path in candidate['paths']),
                 *map(str, candidate['roi']),
                 str(features / f'overlay-{index:02d}.csv')]
        indexed.append(dict(candidate, index=index))
    if not indexed:
        return indexed
    command(argv, stage / log_name, env)
    return indexed


def read_overlays(features, candidates):
    """Rows and the candidates that produced them, kept in step."""
    features = Path(features)
    rows, kept = [], []
    for candidate in candidates:
        path = features / f"overlay-{candidate['index']:02d}.csv"
        if path.exists():
            rows.append(cm_rules.read_rows(path))
            kept.append(candidate)
    return rows, kept


def measure(bundle, source, info, stage, config, metadata, rows=None):
    """Learns the overlay candidates and measures them over the whole file."""
    # The sideband attempt must not share a directory with the fallback: the
    # fallback writes video.csv and the learned overlay there.
    features = stage / ('features' if rows is None else 'features-sideband')
    features.mkdir()
    scan_argv = [bundle / 'cm-scan', source, features, str(config.get('threads', 4)),
                 '--duration', f"{info['duration_seconds']:.6f}",
                 '--windows', str(config.get('scan_windows', 16)),
                 '--references', str(config.get('scan_references', 3))]
    origin = info['video_origin_s']
    env = tool_env(config, {'CM_TIME_ORIGIN': str(origin)})
    if rows is None:
        seed_marker_from_logo(config, metadata, info, source, stage)
    cached = load_marker(config, metadata, info)
    candidates = []
    scan_done = False
    if rows is not None:
        if not cached:
            raise cm_features.FeatureUnavailable('no marker cache for the sideband path')
        argv = [bundle / 'cm-overlay', source,
                ';'.join(cached['reference_paths']), *map(str, cached['roi']),
                str(features / 'overlay-00.csv')]
        try:
            command(argv, stage / 'overlay.log', env)
        except RuntimeError as error:
            # A cache this tool refuses is a reason to measure the recording
            # the slow way, not to lose it: the caller falls back to the full
            # NAS decode when the sideband cannot be used.
            fail_marker(config, metadata, info)
            print(json.dumps(dict(event='marker_scan_failed', path='sideband',
                                  error=str(error)[:400])), flush=True)
            raise cm_features.FeatureUnavailable(
                'cached marker refused by cm-overlay: %s' % str(error)[:200])
        candidates = [dict(index=0, roi=cached['roi'], band=cached.get('band'),
                           references=cached['references'], magnitude=0.0, consistency=0.0, cached=True)]
    elif cached:
        # The cached marker is measured inside the feature pass: the ROI and its
        # reference crops are known before the first frame is decoded, so
        # cm-scan does the accumulation as it goes. That removes a second full
        # decode of the same file. The scan always runs, because either the
        # cached mark fits and is measured, or the learned candidates replace
        # it, and both outcomes need video.csv.
        inline_argv = [*scan_argv,
                       '--overlay-roi', ','.join(str(int(value)) for value in cached['roi']),
                       '--overlay-references', ';'.join(cached['reference_paths']),
                       '--overlay-output', str(features / 'overlay-00.csv')]
        try:
            command(inline_argv, stage / 'scan.log', env)
        except RuntimeError as error:
            # cm-scan refuses a marker it cannot read before the first frame is
            # decoded. That is the cache's fault, not the recording's, and it
            # used to end the job here: no rule ran, so the trust counter never
            # moved, the alternative was never promoted, and the same cache
            # failed every later recording on the service. Count the failure
            # and analyse this recording by learning its mark instead.
            fail_marker(config, metadata, info)
            print(json.dumps(dict(event='marker_scan_failed', error=str(error)[:400])), flush=True)
            if scan_produced_frames(features):
                raise
            cached = None
            (features / 'overlay-00.csv').unlink(missing_ok=True)
            command(scan_argv, stage / 'scan-nocache.log', tool_env(config))
            candidates = overlay_pass(bundle, source, features, stage, env,
                                      'overlay-nocache.log',
                                      learned_candidates(features)
                                      + known_candidates(config, metadata, info, features))
        else:
            scan_done = True
            candidates = [dict(index=0, roi=cached['roi'], band=cached.get('band'),
                               references=cached['references'], magnitude=0.0,
                               consistency=0.0, cached=True)]
    else:
        command(scan_argv, stage / 'scan.log', tool_env(config))
        scan_done = True
        candidates = overlay_pass(bundle, source, features, stage, env, 'overlay.log',
                                  learned_candidates(features)
                                  + known_candidates(config, metadata, info, features))
    overlays, candidates = read_overlays(features, candidates)
    if rows is not None:
        # The capture side measured the same frames it encoded. Keep the rows
        # in the same shape cm-scan writes so the rules stay untouched.
        video = [dict(row) for row in rows[0]]
        audio = [dict(row) for row in rows[1]]
        origin = info['video_origin_s']
        for row in video:
            row['pts'] -= origin
        for row in audio:
            row['pts'] -= origin
    else:
        video = cm_rules.read_rows(features / 'video.csv')
        audio = cm_rules.read_rows(features / 'audio.csv')
        # The encoder rebases the picture to zero but copies the sound with the
        # timestamps it inherited, so a recording taken hours into a capture
        # session carries sound tens of thousands of seconds away from the
        # picture. Line the sound up with the picture before anything else:
        # both streams cover the same wall time, so their first rows differ by
        # exactly the offset.
        if video and audio:
            shift = audio[0]['pts'] - video[0]['pts']
            if abs(shift) > 0.5:
                for row in audio:
                    row['pts'] -= shift
        # Programme-relative time, as expected by the cut planning step.
        origin = info['video_origin_s']
        for row in video:
            row['pts'] -= origin
        for row in audio:
            row['pts'] -= origin
    options = cm_rules.settings()
    excluded = gap_spans(video) + gap_spans(audio)
    allow_scenario = config.get('timing_mode') in ('cut', 'detect')
    decision = cm_rules.analyze(video, audio, overlays, info['duration_seconds'],
                                options, allow_timing=allow_scenario,
                                excluded=excluded)
    decision['excluded_spans'] = [[round(a, 3), round(b, 3)] for a, b in excluded]
    # A valid marker that simply found no break is still a usable cache entry.
    # A scenario/no-breaks result does not prove the cached marker was valid:
    # the marker may have been unmeasurable and the fallback may have found
    # nothing. In that case re-learn once instead of keeping a bad cache.
    cache_ok = decision.get('mode') == 'overlay'
    if rows is None and cached:
        # The cached marker was measured against this recording: record whether
        # it survived, which is what separates a durable station mark from one
        # programme's telop.
        if marker_survived(decision):
            confirm_marker(config, metadata, info)
        else:
            fail_marker(config, metadata, info)
    service_id = (metadata.get('service') or {}).get('serviceId') \
        or (metadata.get('service') or {}).get('id')
    allowed = config.get('feature_marker_free_services') or []
    if rows is not None and not cache_ok and allow_scenario \
            and config.get('feature_allow_scenario', True) \
            and service_id in allowed:
        # No usable marker for this recording. Judge from the sideband alone
        # with the timing and silence rules, which is how the stations without
        # a persistent mark are handled anyway. The cut planning, cutting and
        # verification steps are unchanged, so a wrong guess is still caught
        # before anything is published.
        # measure() has already shifted the rows by the media origin, so a row
        # at negative time describes content from before the first media frame.
        # Dropping those is what keeps the cut edges in place: rebasing on the
        # first row instead (which is negative) moved the whole judgement later
        # by that amount, and the cut edges moved inwards by the same 14 s.
        kept_video = [row for row in video if row['pts'] >= -0.5]
        kept_audio = [row for row in audio if row['pts'] >= -0.5]
        if len(kept_video) < 10:
            # Too little overlap to judge; keep everything and let the
            # plausibility check below reject an impossible answer.
            kept_video, kept_audio = video, audio
        without_marker_offsets = dict(
            sideband_first=round(video[0]['pts'], 3) if video else None,
            media_origin=round(info.get('video_origin_s') or 0.0, 3),
            dropped_rows=len(video) - len(kept_video))
        rebased_video = [dict(row) for row in kept_video]
        rebased_audio = [dict(row) for row in kept_audio]
        without_marker = cm_rules.analyze(rebased_video, rebased_audio, [],
                                          info['duration_seconds'], options,
                                          allow_timing=True,
                                          excluded=gap_spans(rebased_video)
                                          + gap_spans(rebased_audio))
        duration = info['duration_seconds']
        cuts = without_marker.get('cuts') or []
        plausible = (without_marker.get('mode') in ('scenario', 'none')
                     and cuts_within(cuts, duration))
        if not plausible:
            # A decision outside the recording means the rows and the media do
            # not share a timeline; do not act on it.
            raise cm_features.FeatureUnavailable(
                'marker-free decision unusable: cuts=%s duration=%.1f'
                % (cuts[:2], duration))
        if without_marker.get('mode') in ('scenario', 'none'):
            without_marker['excluded_spans'] = decision['excluded_spans']
            without_marker['feature_source'] = 'capture'
            without_marker['marker'] = None
            without_marker['feature_note'] = 'no usable marker'
            # Record why the marker was unavailable: a missing cache and an
            # unusable one need different follow-up.
            without_marker['marker_state'] = 'no_cache' if not cached else 'cache_unusable'
            without_marker['offsets'] = without_marker_offsets
            # The marker was available but did not reach a decision. Say which
            # candidate the rules refused and why, so the rate of these cases
            # can be followed per station.
            without_marker['marker_refusal'] = sorted({
                '%s:%s' % (c.get('index'), c.get('reason'))
                for c in (decision.get('candidates') or []) if c.get('reason')})
            without_marker['learned_candidates'] = [
                dict(index=c['index'], band=c.get('band'), roi=c.get('roi'),
                     magnitude=round(c.get('magnitude', 0), 4),
                     consistency=round(c.get('consistency', 0), 4))
                for c in candidates]
            without_marker['video_frames'] = len(video)
            without_marker['audio_frames'] = len(audio)
            without_marker['source_gap_ms'] = dict(
                video=round(max_gap_ms(video), 2), audio=round(max_gap_ms(audio), 2))
            without_marker['_rows'] = (video, audio)
            without_marker['_overlays'] = overlays
            without_marker['_skip_marker_save'] = True
            return without_marker
    if rows is not None and not cache_ok:
        # The cached marker did not describe this recording. The caller gets a
        # clean fallback: a full scan learns a new one. Report what the rules
        # saw so the mismatch can be read from the job instead of guessed.
        # analyze() reports each marker attempt under 'candidates'.
        reasons = sorted({str(e.get('reason')) for e in (decision.get('candidates') or [])
                          if e.get('reason')})
        # Record what the bin classifier saw, so a refusal can be read from
        # the job instead of guessed at.
        options = cm_rules.settings()
        adapted = cm_rules.adapt_bin_samples(video, options)
        usable = 0
        for overlay in overlays:
            usable = max(usable, sum(1 for row in overlay
                                     if row.get('samples', 0) >= adapted['min_bin_samples']))
        rows_csv = features / 'overlay-00.csv'
        tail = ''
        log = stage / 'overlay.log'
        if log.exists():
            tail = ' '.join(log.read_text(errors='replace').split()[-12:])
        raise cm_features.FeatureUnavailable(
            'marker did not fit the sideband path: mode=%s reasons=%s overlay_csv=%s '
            'min_bin_samples=%s usable_bins=%s overlays=%d marker_free_allowed=%s log=%s'
            % (decision.get('mode'), ','.join(reasons) or 'none',
               rows_csv.exists(), adapted['min_bin_samples'], usable,
               len(overlays), service_id in allowed, tail))
    if cached and not cache_ok:
        # The cached marker did not describe this recording (a station change or
        # a moved mark). Fall back to learning and replace the cache.
        #
        # The scan that produced video.csv also wrote logo.json and the
        # reference crops; both describe this same recording and were not
        # touched by the cached attempt. Running it a second time here would
        # decode the whole file again for a result already on disk.
        cached_roi = cached['roi']
        cached = None
        if not scan_done:
            command(scan_argv, stage / 'scan-no-cache.log', tool_env(config))
            scan_done = True
        # The learned candidates keep the first position: analyze() passes the
        # first overlay to cm_scenario, and moving the station mark in front of
        # it changed the logo-less decision as a side effect.
        measured = overlay_pass(bundle, source, features, stage, env, 'overlay-no-cache.log',
                                learned_candidates(features)
                                + known_candidates(config, metadata, info, features, cached_roi))
        overlays, candidates = read_overlays(features, measured)
        decision = cm_rules.analyze(video, audio, overlays, info['duration_seconds'],
                                    options, allow_timing=allow_scenario,
                                    excluded=excluded)
        decision['excluded_spans'] = [[round(a, 3), round(b, 3)] for a, b in excluded]
    if decision.get('mode') == 'overlay' and not decision.get('_skip_marker_save'):
        winner = decision.get('marker_index')
        if winner is None and candidates:
            # The rules can settle on a marker without naming its index (the
            # local-window branch); keep the first candidate in that case.
            winner = 0
        if winner is not None and winner < len(candidates):
            # Only keep a marker whose correlation separates cleanly. A cache
            # entry that barely passed still fails the sideband path later,
            # and a bad entry keeps being reused instead of relearned.
            separation = bin_separation(overlays[winner] if winner < len(overlays) else [],
                                        options)
            threshold = options['min_cluster_gap'] * 1.5
            if separation is not None and separation >= threshold:
                try:
                    save_marker(config, metadata, info, candidates[winner], features)
                    decision['marker_saved'] = dict(
                        winner=winner, roi=candidates[winner].get('roi'),
                        separation=round(separation, 3))
                except OSError as error:
                    decision['marker_saved'] = 'failed: %s' % error
            else:
                decision['marker_not_saved'] = (
                    'separation %s below %.2f' % (
                        'unknown' if separation is None else round(separation, 3),
                        threshold))
    decision['learned_candidates'] = [
        dict(index=c['index'], band=c.get('band'), roi=c.get('roi'),
             magnitude=round(c.get('magnitude', 0), 4), consistency=round(c.get('consistency', 0), 4))
        for c in candidates]
    decision['feature_source'] = 'capture' if rows is not None else 'nas'
    decision['video_frames'] = len(video)
    decision['audio_frames'] = len(audio)
    # Largest clock jump inside the recording; a large value means the capture
    # lost data even though the packet counts still match.
    decision['source_gap_ms'] = dict(video=round(max_gap_ms(video), 2), audio=round(max_gap_ms(audio), 2))
    # Internal only: the shadow mode compares these with the sideband. The
    # caller removes the key before the decision is written to result.json.
    decision.pop('_skip_marker_save', None)
    decision['_rows'] = (video, audio)
    decision['_overlays'] = overlays
    return decision


def cuts_within(cuts, duration):
    """True when every cut sits inside the recording and in order.

    A decision outside the recording means the rows and the media do not share
    a timeline; acting on it would cut the wrong place.
    """
    if duration is None or duration <= 0:
        return False
    return all(0.0 <= a < b <= duration + 1.0 for a, b in (cuts or []))


def bin_separation(overlay, options):
    """Gap between the two correlation clusters, or None when unusable.

    Mirrors the split the classifier makes, so a marker that would score as
    marginal is not written to the cache.
    """
    usable = sorted(row['correlation'] for row in overlay
                    if row.get('samples', 0) >= options['min_bin_samples'])
    if len(usable) < 10:
        return None
    total = sum(usable)
    left_sum = 0.0
    best_score, best_index = -1.0, None
    for index in range(1, len(usable)):
        left_sum += usable[index - 1]
        left_mean = left_sum / index
        right_mean = (total - left_sum) / (len(usable) - index)
        score = index * (len(usable) - index) * (left_mean - right_mean) ** 2
        if score > best_score:
            best_score, best_index = score, index
    if best_index is None:
        return None
    low = sum(usable[:best_index]) / best_index
    high = sum(usable[best_index:]) / (len(usable) - best_index)
    return high - low


def gap_spans(rows, factor=4.0, minimum=1.0):
    """Clock jumps inside a recording, in the same time base as the rows."""
    if len(rows) < 3:
        return []
    steps = sorted(rows[i]['pts'] - rows[i - 1]['pts'] for i in range(1, len(rows)))
    nominal = steps[len(steps) // 2]
    spans = []
    for i in range(1, len(rows)):
        jump = rows[i]['pts'] - rows[i - 1]['pts']
        if jump > max(nominal * factor, minimum):
            spans.append((rows[i - 1]['pts'], rows[i]['pts']))
    return spans


def max_gap_ms(rows):
    if len(rows) < 2:
        return 0.0
    return max((rows[index]['pts'] - rows[index - 1]['pts']) * 1000.0 for index in range(1, len(rows)))


def decision_agreement(sideband, measured, overlays, duration, config,
                       tolerance=0.5):
    """What the rules would cut with the sideband instead of the scan."""
    options = cm_rules.settings()
    excluded = []
    allow_timing = config.get('timing_mode') in ('cut', 'detect')
    try:
        capture = cm_rules.analyze(sideband['video'], sideband['audio'], overlays,
                                   duration, options, allow_timing=allow_timing,
                                   excluded=excluded)
        nas = cm_rules.analyze(measured[0] or [], measured[1] or [], overlays,
                               duration, options, allow_timing=allow_timing,
                               excluded=excluded)
    except (KeyError, TypeError, ValueError) as error:
        return dict(error=str(error))
    left = [tuple(round(v, 2) for v in span) for span in (nas.get('cuts') or [])]
    right = [tuple(round(v, 2) for v in span) for span in (capture.get('cuts') or [])]
    matched = sum(1 for span in left if span in right)
    return dict(nas_cuts=len(left), capture_cuts=len(right), matched=matched,
                mode=nas.get('mode'), capture_mode=capture.get('mode'),
                same=left == right)


def chapter_agreement(sideband, measured, tolerance=0.5):
    """How far the sideband moves the cut edges the rules would choose.

    Both runs use the same overlay measurement; only the video and audio rows
    differ. Chapter points are the edges the rules cut on, so agreement here
    means the substitution would not change the cut.
    """
    options = cm_rules.settings()
    try:
        left, _ = cm_rules.chapter_points(
            sideband['video'], sideband['audio'], options)
        right, _ = cm_rules.chapter_points(
            measured[0] or [], measured[1] or [], options)
    except (KeyError, TypeError, ValueError):
        return None
    if not right:
        return dict(nas=0, capture=len(left), matched=0, ratio=None)
    matched = 0
    for point in right:
        if any(abs(point - other) <= tolerance for other in left):
            matched += 1
    return dict(nas=len(right), capture=len(left), matched=matched,
                ratio=round(matched / len(right), 4))


def feature_rows(features, duration, config):
    """Returns (rows, reason) for the optional capture-side sidecar.

    rows is None whenever the sidecar must not be trusted; the caller then
    measures on the NAS exactly as before. This function never raises.
    """
    if config.get('feature_mode', 'off') not in ('shadow', 'apply'):
        return None, 'disabled'
    if not features:
        return None, 'no_features'
    path = features.get('path')
    if not path:
        return None, 'no_path'
    try:
        parsed = cm_features.load(path, expected_duration=duration)
    except cm_features.FeatureUnavailable as error:
        return None, str(error)
    return (parsed['video'], parsed['audio']), 'ok'


def process_job(job, config):
    if job.get('version') != 2 or job.get('input_kind') != 'fmp4' or job.get('retain_original') is not True:
        raise ValueError('expected fMP4 v2 job retaining original')
    source = Path(job['source']).resolve()
    require_tmpfs(source)
    scratch = Path(config['scratch_dir'])
    scratch.mkdir(parents=True, exist_ok=True)
    require_tmpfs(scratch)
    bundle = Path(config['bundle']).resolve()
    ffprobe = config.get('ffprobe') or str(Path(config.get('ffmpeg', 'ffmpeg')).with_name('ffprobe'))
    metadata = job['metadata']
    cm_enabled = config.get('enable_cm_cut') is True
    cut_mode = config.get('mode', 'cut') == 'cut'
    declared = metadata['media']
    fps_num = declared.get('frameRateNumerator', 0)
    fps_den = declared.get('frameRateDenominator', 0)
    fps_options = ['--expected-fps', str(fps_num), str(fps_den)] if fps_num > 0 and fps_den > 0 else []
    result = dict(source=str(source), producer=job.get('producer'), receiver_pid=job.get('receiver_pid'),
                  recording_id=metadata['recordingId'], retain_original=True, original_saved=False,
                  reason='cm_disabled' if not cm_enabled else 'not_detected',
                  status='original_only', release_id=config['release_id'])
    with tempfile.TemporaryDirectory(prefix='cm-', dir=scratch) as temp:
        stage = Path(temp)
        info = inspect_source(bundle, source, stage)
        cuts = []
        # A fragment that lost data, or is too short to contain a break, is not
        # worth two decoding passes: record the condition and keep the original.
        capture = metadata.get('capture', {})
        wall_ms = capture.get('endAt', 0) - capture.get('startAt', 0)
        shortfall_ms = round(wall_ms - info['duration_seconds'] * 1000)
        # A recording that lost data is still analysed: the decision drops only
        # the candidates that touch the missing part. Only a recording too short
        # to contain a break skips detection and saves CPU.
        too_short = info['duration_seconds'] < config.get('min_detect_seconds', 120.0)
        if cm_enabled and too_short:
            result['detection'] = dict(skipped='too_short',
                                       duration_seconds=round(info['duration_seconds'], 3))
            result['reason'] = 'too_short_to_detect'
            # Record the configured mode even when detection is skipped, so the
            # rollout can be followed from every job.
            result['feature_mode'] = config.get('feature_mode', 'off')
            result['feature_reason'] = 'recording_too_short'
            cm_enabled = False
        if cm_enabled:
            print(json.dumps(dict(event='detect_fmp4', source=str(source))), flush=True)
            feature_mode = config.get('feature_mode', 'off')
            rows, feature_reason = feature_rows(job.get('features'), info['duration_seconds'], config)
            result['feature_mode'] = feature_mode
            result['feature_reason'] = feature_reason
            try:
                if feature_mode == 'apply' and rows is not None:
                    try:
                        decision = measure(bundle, source, info, stage, config, metadata, rows=rows)
                    except cm_features.FeatureUnavailable as error:
                        result['feature_fallback'] = str(error)
                        decision = measure(bundle, source, info, stage, config, metadata)
                else:
                    decision = measure(bundle, source, info, stage, config, metadata)
                measured_rows = decision.pop('_rows', (None, None))
                if feature_mode == 'shadow':
                    # Judge with the NAS measurement, then record how far the
                    # sideband is from it. Shadow never changes the verdict.
                    # Both sides must use the same time origin: the measured
                    # rows were shifted to programme start, the sidecar was
                    # not, so shift the sidecar by the same amount first.
                    if rows is not None and measured_rows[0]:
                        # Both sides describe the same recording; line them up
                        # on their own first picture. The absolute offset is
                        # kept in the report as 'edges' instead.
                        origin = info['video_origin_s']
                        base_v = rows[0][0]['pts'] if rows[0] else 0.0
                        base_a = rows[1][0]['pts'] if rows[1] else 0.0
                        base_m = measured_rows[0][0]['pts'] if measured_rows[0] else 0.0
                        aligned = dict(
                            video=[dict(r, pts=r['pts'] - base_v) for r in rows[0]],
                            audio=[dict(r, pts=r['pts'] - base_a) for r in rows[1]])
                        measured_shifted = [
                            dict(r, pts=r['pts'] - base_m) for r in (measured_rows[0] or [])]
                        measured_audio_shifted = [
                            dict(r, pts=r['pts'] - base_m) for r in (measured_rows[1] or [])]
                        report = cm_features.compare(
                            aligned, measured_shifted, measured_audio_shifted)
                        # Keep the raw first timestamps: a span difference is
                        # easier to read from these than from the deltas.
                        report['offsets'] = dict(
                            capture_first=round(base_v, 3),
                            capture_audio_first=round(base_a, 3),
                            measured_first=round(base_m, 3),
                            media_origin=round(origin, 3))
                        # The rules cut on chapter points, so what decides the
                        # outcome is whether substituting the sideband moves
                        # any of them. The overlay part is the same NAS
                        # measurement in both runs and needs no comparison.
                        report['chapter_agreement'] = chapter_agreement(
                            aligned, (measured_shifted, measured_audio_shifted))
                        overlays = decision.pop('_overlays', None)
                        if overlays is not None:
                            report['decision_agreement'] = decision_agreement(
                                aligned, (measured_shifted, measured_audio_shifted),
                                overlays, info['duration_seconds'], config)
                        result['feature_shadow'] = report
                    elif feature_reason != 'ok':
                        result['feature_shadow'] = dict(skipped=feature_reason)
                result['detection'] = decision
                cuts = [tuple(item) for item in decision.get('cuts') or []]
                if cuts and decision.get('mode') == 'scenario' and not config.get('scenario_publish'):
                    # The spot-chain inference is still being measured on live
                    # material: record what it would cut, publish nothing yet.
                    result['scenario_preview'] = [[round(a, 3), round(b, 3)] for a, b in cuts]
                    cuts = []
                result['reason'] = decision.get('reason', 'detected')
                title = str(job.get('original_mp4') or '')
                if not title:
                    title = str((metadata.get('media') or {}).get('relativePath') or '')
                short_min = cut_minimum(title, config)
                result['min_cut_seconds'] = short_min
                kept, rejected = filter_short_cuts(cuts, short_min)
                if rejected:
                    result['short_cuts_rejected'] = [
                        [round(a, 3), round(b, 3)] for a, b in rejected]
                    cuts = kept
                    if not cuts:
                        result['reason'] = 'short_breaks_rejected'
            except Exception as error:
                result.update(reason='detection_failed', detection_error=str(error))
        # Detection precedes normal MP4 generation; failure still preserves original.
        print(json.dumps(dict(event='normalise_fmp4', source=str(source))), flush=True)
        original = stage / 'original.mp4'
        empty = stage / 'empty.tsv'
        try:
            command([bundle / 'cm-cut', source, original, empty, stage / 'original.json', '--copy-all'], stage / 'original.log')
        except RuntimeError as error:
            result['original_error'] = str(error)
            # A damaged tail (for example a reboot during capture) makes the
            # normaliser stop. Keep the playable prefix with a plain remux.
            if not ffmpeg_copy(source, original, stage / 'original-fallback.log', config):
                recovery = preserve_source(config, job, source)
                if recovery:
                    result['recovery_source'] = str(recovery)
                result.update(status='original_only', reason='original_unavailable',
                              original_saved=False)
                return result
            result['original_fallback'] = 'ffmpeg_copy'
            metadata['state'] = 'partial'
            metadata.setdefault('capture', {})['originalIssue'] = str(error)
            result['original_validation'] = dict(verified=False, mode='fallback_copy',
                                                  full_decode_ok=None, error=str(error))
            result['reason'] = 'original_partial'
            save_original_file(job, metadata, original, result)
            return result
        empty.write_text('')
        try:
            command([bundle / 'cm-cut', original, stage / 'unused-output.mp4', empty, stage / 'output-inspect.json'],
                    stage / 'output-inspect.log')
            output_info = load(stage / 'output-inspect.json')
            validate_remux(info, output_info)
            result['original_validation'] = dict(
                verified=True, mode='remux_packet_counts', full_decode_ok=None,
                source_video_frames=info['video_frames'], output_video_frames=output_info['video_frames'],
                source_audio_packets=info['audio_packets'], output_audio_packets=output_info['audio_packets'])
        except Exception as error:
            # The MP4 exists but its packet accounting differs. Do not delete
            # the only copy: publish it as partial and skip CM cutting.
            metadata['state'] = 'partial'
            metadata.setdefault('capture', {})['originalIssue'] = str(error)
            result['original_validation'] = dict(verified=False, mode='remux_packet_counts',
                                                  full_decode_ok=None, error=str(error))
            result['reason'] = 'original_validation_failed'
            save_original_file(job, metadata, original, result)
            return result
        validation = result['original_validation']
        audio_gap = validation.get('source_audio_max_gap_ms', 0)
        video_gap = validation.get('source_video_max_gap_ms', 0)
        if max(audio_gap, video_gap) > 1000:
            metadata['state'] = 'partial'
            metadata.setdefault('capture', {})['sourceTimestampGapMs'] = dict(audio=audio_gap, video=video_gap)
            result['source_timestamp_gap'] = True
            result['reason'] = 'source_timestamp_gap'
            cuts = []
        video_gap_count = validation.get('source_video_gap_count', 0)
        audio_gap_count = validation.get('source_audio_gap_count', 0)
        if video_gap_count or audio_gap_count:
            metadata['state'] = 'partial'
            metadata.setdefault('capture', {})['sourcePacketGaps'] = dict(video=video_gap_count, audio=audio_gap_count)
            result['source_packet_gaps'] = True
            result['reason'] = 'source_packet_gap'
            cuts = []
        capture = metadata.get('capture', {})
        if wall_ms > 0 and shortfall_ms > max(5000, wall_ms * 0.01):
            metadata['state'] = 'partial'
            capture['sourceDurationShortfallMs'] = shortfall_ms
            result['source_duration_shortfall'] = True
        durations = probe_stream_durations(original, ffprobe)
        if durations and av_duration_mismatch(*durations):
            video_duration, audio_duration = durations
            metadata['state'] = 'partial'
            capture['sourceAvDurationDeltaMs'] = round(abs(video_duration - audio_duration) * 1000)
            result['source_av_duration_mismatch'] = dict(
                video=round(video_duration, 3), audio=round(audio_duration, 3),
                delta=round(abs(video_duration - audio_duration), 3))
            result['reason'] = 'source_av_duration_mismatch'
            cuts = []
        media = metadata['media']
        # The container counts access units; a field-coded stream carries two
        # per picture, so the packet count is twice the frame count. Prefer the
        # number of pictures the scan actually decoded.
        detected_frames = (result.get('detection') or {}).get('video_frames')
        media.update(width=info['width'], height=info['height'], videoCodec=info['video_codec'],
                     durationMs=round(info['duration_seconds'] * 1000),
                     frameCount=detected_frames or info['video_frames'],
                     frameRateNumerator=info['frame_rate_num'], frameRateDenominator=info['frame_rate_den'])
        save_original_file(job, metadata, original, result)
        target = Path(result['original'])
        if field_coded_stream(info, result.get('detection')):
            result['field_coded'] = True
            result['reason'] = 'field_coded_unsupported'
            cuts = []
        if not cuts:
            return result
        if not cut_mode:
            result['reason'] = 'detect_only'
            result['planned_cuts'] = [[round(a, 3), round(b, 3)] for a, b in cuts]
            return result
        minimum = config.get('min_cut_confidence', 0.0)
        confidence = (result.get('detection') or {}).get('confidence', 0.0)
        if confidence < minimum:
            result['reason'] = 'low_confidence'
            return result
        try:
            requested = stage / 'requested.tsv'
            requested.write_text(''.join(f'{a:.9f} {b:.9f}\n' for a, b in cuts))
            command([bundle / 'cm-cut', source, stage / 'cut.mp4', requested, stage / 'cut.json'], stage / 'cut.log')
            cut = load(stage / 'cut.json')
            result['cut'] = cut
            if not cut['cuts']:
                result['reason'] = 'no_complete_safe_gop'
                return result
            actual = stage / 'actual.tsv'
            actual.write_text(''.join(f'{a:.9f} {b:.9f}\n' for a, b in cut['cuts']))
            try:
                command([bundle / 'cm-verify', source, stage / 'cut.mp4', actual, stage / 'validation.json', *fps_options],
                        stage / 'validation.log', tool_env(config))
            except RuntimeError as error:
                # Keep the verifier's own report even when the check fails: it
                # carries the frame and packet counts that explain why.
                report = stage / 'validation.json'
                if report.exists():
                    result['validation'] = load(report)
                # Some broadcast AAC streams exceed the reference decoder's
                # limits. The cut itself is still fully checked: every video
                # frame is decoded and compared, audio is compared packet by
                # packet. The undecodable audio is a property of the source.
                if not report.exists() or load(report).get('reason') != 'audio_decode_error':
                    raise
                result['cut_audio_decode_error'] = str(error)
                command([bundle / 'cm-verify', source, stage / 'cut.mp4', actual, report,
                         '--audio-packets-only', *fps_options], stage / 'validation-packets.log',
                        tool_env(config))
            result['validation'] = load(stage / 'validation.json')
            if result['validation'].get('verified') is not True:
                raise ValueError('cut validation failed')
            if config.get('require_full_decode', True) and result['validation'].get('full_decode_ok') is not True:
                # The audio fallback compares audio packets but never decodes
                # them. Allow publication only when the packet comparison is
                # clean and the caller has opted in to the known AAC decoder
                # limitation; video decode failures never take this path.
                if not audio_warning_is_acceptable(result['validation'],
                                                   result.get('cut_audio_decode_error'), config):
                    raise ValueError('cut validation skipped the full decode')
                result['audio_decode_warning'] = True
            relative = target.relative_to(Path(config['source_root']))
            public = Path(config['publish_root']) / relative.parent / (target.stem + '_CMカット.mp4')
            # This hidden file is the permanent CM output, retained after publication.
            stored = Path(config['publish_root']) / '.media' / (metadata['recordingId'] + '.mp4')
            if not stored.exists():
                final_copy(stage / 'cut.mp4', stored)
            reason = 'cut_and_verified_audio_warning' if result.get('audio_decode_warning') else 'cut_and_verified'
            result.update(output=str(public), stored_output=str(stored), status='completed', reason=reason)
            public.parent.mkdir(parents=True, exist_ok=True)
            save(public.with_suffix('.cm.json'), dict(result, programme_metadata=metadata))
            if public.is_symlink() or public.exists():
                public.unlink()
            public.symlink_to(os.path.relpath(stored, public.parent))
        except Exception as error:
            result.update(status='original_only', reason='cut_failed', cut_error=str(error))
        return result


def run_queue(config, once=False, stop=None):
    queue = Path(config['queue_dir'])
    queue.mkdir(parents=True, exist_ok=True)
    require_tmpfs(queue)
    locks = queue / '.locks'
    locks.mkdir(exist_ok=True)
    state = Path(config['state_dir'])
    state.mkdir(parents=True, exist_ok=True)
    # Several workers share this database; wait instead of failing when two
    # of them commit at the same time.
    db = sqlite3.connect(queue / 'jobs.sqlite3', timeout=30)
    db.execute('CREATE TABLE IF NOT EXISTS queue_jobs(id TEXT PRIMARY KEY,status TEXT,detail TEXT)')
    db.commit()
    count = 0
    try:
        while stop is None or not stop[0]:
            pending = []
            for path in queue.glob('*.json'):
                try:
                    pending.append((path.stat().st_mtime_ns, path))
                except FileNotFoundError:
                    continue
            for _, path in sorted(pending):
                if stop and stop[0]:
                    break
                with (locks / (path.stem + '.lock')).open('a') as lock:
                    try:
                        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    except BlockingIOError:
                        continue
                    if not path.exists():
                        continue
                    job = load(path)
                    row = db.execute('SELECT status,detail FROM queue_jobs WHERE id=?', (path.stem,)).fetchone()
                    if row and row[0] in ('completed', 'original_only'):
                        result = json.loads(row[1])
                    else:
                        db.execute('INSERT OR REPLACE INTO queue_jobs VALUES(?,?,?)', (path.stem, 'processing', '{}'))
                        db.commit()
                        try:
                            result = process_job(job, config)
                        except Exception as error:
                            result = dict(status='failed', reason=str(error), source=job.get('source'), original_saved=False)
                        db.execute('UPDATE queue_jobs SET status=?,detail=? WHERE id=?',
                                   (result['status'], json.dumps(result, ensure_ascii=False), path.stem))
                        db.commit()
                        save(state / 'jobs' / path.stem / 'result.json', result)
                        print(json.dumps(result, ensure_ascii=False), flush=True)
                        count += 1
                    # Commit permanent history before removing the received RAM input.
                    if (result.get('original_saved') or result.get('recovery_source')) \
                            and not job.get('keep_input') and not retain_test_input(job, config):
                        Path(job['source']).unlink(missing_ok=True)
                        feature_path = (job.get('features') or {}).get('path')
                        if feature_path:
                            Path(feature_path).unlink(missing_ok=True)
                    destination = queue / ('completed' if result.get('original_saved') else 'failed')
                    destination.mkdir(exist_ok=True)
                    path.rename(destination / path.name)
            if once:
                break
            for _ in range(config.get('poll_seconds', 10)):
                if stop and stop[0]:
                    break
                time.sleep(1)
    finally:
        db.close()
    return count


REQUIRED_BINARIES = ('cm-scan', 'cm-overlay', 'cm-cut', 'cm-verify')
REQUIRED_CONFIG = ('bundle', 'source_root', 'publish_root', 'queue_dir', 'state_dir')
MIN_FREE_BYTES = 2 * 1024 ** 3


def self_check(config):
    """Checks everything a run needs before the queue is touched.

    A release once shipped without cm_scenario.py. Nothing noticed until a
    recording reached the logo-less path, failed with "No module named" and was
    kept as original only. Failing at startup turns that into a visible
    deployment error instead of a silent loss of cuts.
    """
    problems = []
    bundle = Path(str(config.get('bundle') or ''))
    if not bundle.is_dir():
        problems.append(f'bundle directory missing: {bundle}')
    for name in REQUIRED_BINARIES:
        path = bundle / name
        if not path.is_file() or not os.access(path, os.X_OK):
            problems.append(f'missing or not executable: {path}')
    for key in REQUIRED_CONFIG:
        if not config.get(key):
            problems.append(f'missing setting: {key}')
    for module in ('cm_rules', 'cm_scenario'):
        if module not in sys.modules:
            problems.append(f'module not imported: {module}')
    try:
        decision = cm_scenario.decide([], [], 1.0)
        if 'cuts' not in decision:
            problems.append('logo-less path returned no decision')
    except Exception as error:                     # noqa: BLE001 - report, never crash
        problems.append(f'logo-less path failed on empty input: {error}')
    for key in ('queue_dir', 'state_dir'):
        directory = Path(str(config.get(key) or ''))
        try:
            directory.mkdir(parents=True, exist_ok=True)
            probe = directory / '.self-check'
            probe.write_text('ok')
            probe.unlink()
        except OSError as error:
            problems.append(f'{key} is not writable: {error}')
    scratch = Path(str(config.get('scratch_dir') or tempfile.gettempdir()))
    try:
        free = shutil.disk_usage(scratch).free
        if free < MIN_FREE_BYTES:
            problems.append(f'only {free // (1024 ** 2)}MB free on {scratch}')
    except OSError as error:
        problems.append(f'cannot measure free space on {scratch}: {error}')
    return problems


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--config', type=Path, required=True)
    sub = parser.add_subparsers(dest='command', required=True)
    queue = sub.add_parser('queue')
    queue.add_argument('--once', action='store_true')
    args = parser.parse_args()
    config = load(args.config)
    if config.get('retain_original') is not True:
        raise ValueError('original recordings must be retained')
    problems = self_check(config)
    if problems:
        print(json.dumps(dict(event='self_check_failed', problems=problems), ensure_ascii=False), flush=True)
        raise SystemExit(2)
    print(json.dumps(dict(event='self_check_ok', bundle=str(config.get('bundle'))), ensure_ascii=False), flush=True)
    if args.command == 'queue':
        stop = [False]
        signal.signal(signal.SIGTERM, lambda *_: stop.__setitem__(0, True))
        signal.signal(signal.SIGINT, lambda *_: stop.__setitem__(0, True))
        run_queue(config, args.once, stop)


if __name__ == '__main__':
    main()
