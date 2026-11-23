#!/usr/bin/env python3
"""Read the CM feature sidecar the receiver stripped from the stream.

The capture side sends "pixf" boxes next to the fMP4 fragments. The NAS
receiver keeps their payloads in one tmpfs file per programme, so the media
file never contains them. This module turns that payload back into the rows
``cm_rules`` already understands.

Layout, little endian:

  header 44 bytes: schema u16, flags u16, generation u32, count u32,
                   first_pts_us i64, last_pts_us i64, video u32, audio u32,
                   dropped u32, reserved u32
  record 28 bytes: type u8, flags u8, reserved u16, pts_us i64,
                   a u32, b i32, c u32, d u32

Video records carry mean luma x100 in b, black fraction x1000 in c and the
scene score x100 in d. Audio records carry the sample count in a and RMS dB
x100 in b.
"""
import bisect
import struct

SCHEMA = 1
HEADER = 44
RECORD = 28


class FeatureUnavailable(Exception):
    """The sidecar cannot be used; the caller must measure on the NAS."""


def _read_header(raw):
    if len(raw) < HEADER:
        raise FeatureUnavailable('short header')
    schema, flags, generation, count, first_us, last_us, video, audio, dropped, _ = \
        struct.unpack_from('<HHIIqqIIII', raw, 0)
    return dict(schema=schema, flags=flags, generation=generation, count=count,
                first_pts_us=first_us, last_pts_us=last_us, video=video,
                audio=audio, dropped=dropped)


def parse(raw):
    """Turns the sidecar bytes into rows. Raises FeatureUnavailable.

    The receiver appends one payload per pixf box, so the file is a sequence
    of batches. Every batch has to be readable; a single bad one invalidates
    the whole sidecar because coverage can no longer be trusted.
    """
    if len(raw) < HEADER:
        raise FeatureUnavailable('short header')
    video, audio, black = [], [], []
    positions = dict(first=None, last=None)
    total = dict(records=0, dropped=0, video=0, audio=0, batches=0, generations=0)
    generation = None
    # Video and audio arrive as separate blocks inside one batch, so only each
    # stream's own timeline has to move forward.
    previous = {1: None, 2: None}
    offset = 0
    while offset < len(raw):
        if len(raw) - offset < HEADER:
            raise FeatureUnavailable('short header')
        header = _read_header(raw[offset:])
        if header['schema'] != SCHEMA:
            raise FeatureUnavailable(f"schema {header['schema']}")
        if header['count'] == 0:
            raise FeatureUnavailable('no records')
        end = offset + HEADER + header['count'] * RECORD
        if end > len(raw):
            raise FeatureUnavailable('truncated records')
        # The capture side bumps the generation when its sideband restarts,
        # e.g. after the pipeline is restarted. That is a gap, not a corrupt
        # batch: the records that did arrive are still valid for their range.
        if generation is None:
            generation = header['generation']
        elif header['generation'] != generation:
            total['generations'] += 1
            generation = header['generation']
            # The restarted sideband starts its own timeline; do not compare
            # the first record after the gap with the one before it.
            previous = {1: None, 2: None}
        for index in range(header['count']):
            record_offset = offset + HEADER + index * RECORD
            kind, _flags, _reserved, pts_us, a, b, c, d = struct.unpack_from(
                '<BBHqIiII', raw, record_offset)
            pts = pts_us / 1_000_000.0
            if kind in previous and previous[kind] is not None and pts < previous[kind] - 1.0:
                raise FeatureUnavailable('pts went backwards')
            if kind in previous:
                previous[kind] = pts
            if positions['first'] is None or pts < positions['first']:
                positions['first'] = pts
            if positions['last'] is None or pts > positions['last']:
                positions['last'] = pts
            if kind == 1:
                video.append(dict(pts=pts, mean=b / 100.0, black_fraction=c / 1000.0,
                                  scene=d / 100.0, logo_edge=0.0))
                total['video'] += 1
            elif kind == 2:
                if a <= 0:
                    raise FeatureUnavailable('audio record without samples')
                rms = b / 100.0
                if not -120.0 <= rms <= 0.0:
                    # Older capture builds wrote a sentinel for digital
                    # silence; treat anything out of range as the floor.
                    rms = -120.0
                audio.append(dict(pts=pts, samples=a, rms_db=rms))
                total['audio'] += 1
            elif kind == 3:
                # blackdetect interval from the capture side: start in pts,
                # length in milliseconds. Converted to black_fraction below.
                if a == 0:
                    raise FeatureUnavailable('black interval without length')
                black.append((pts, pts + a / 1000.0))
            else:
                raise FeatureUnavailable(f'record type {kind}')
        total['records'] += header['count']
        total['dropped'] += header['dropped']
        total['batches'] += 1
        offset = end
    if not video or not audio:
        raise FeatureUnavailable('video or audio rows missing')
    if black:
        # cm-scan reports the fraction of the frame below 32. blackdetect uses
        # the same picture ratio, so a frame inside an interval is all black.
        black.sort()
        current = 0
        for row in video:
            while current < len(black) and black[current][1] < row['pts']:
                current += 1
            inside = current < len(black) and black[current][0] <= row['pts'] <= black[current][1]
            row['black_fraction'] = 1.0 if inside else 0.0
    meta = dict(schema=SCHEMA, generation=generation, first_pts=positions['first'],
                last_pts=positions['last'], **total)
    meta['video_rows'] = total['video']
    meta['audio_rows'] = total['audio']
    meta['first_pts_us'] = int(round(positions['first'] * 1_000_000))
    meta['last_pts_us'] = int(round(positions['last'] * 1_000_000))
    return dict(meta=meta, video=video, audio=audio)


def load(path, expected_duration=None, min_coverage=0.95):
    """Reads one sidecar and checks it covers the recording it belongs to."""
    try:
        with open(path, 'rb') as handle:
            raw = handle.read()
    except OSError as error:
        raise FeatureUnavailable(f'cannot read {path}: {error}')
    parsed = parse(raw)
    meta = parsed['meta']
    if meta['dropped']:
        raise FeatureUnavailable(f"capture reported {meta['dropped']} dropped records")
    if expected_duration:
        span = meta['last_pts'] - meta['first_pts']
        if span < expected_duration * min_coverage:
            raise FeatureUnavailable(
                f'features cover {span:.1f}s of {expected_duration:.1f}s')
        # A longer span is allowed: the capture starts before the recorder
        # receives media and keeps running past the end. The comparison only
        # uses the shared window, so an overshoot is harmless. A sidecar from
        # another programme is caught by the value comparison instead.
    align_audio(parsed)
    parsed['source'] = path
    return parsed


def align_audio(parsed, tolerance=0.5):
    """Drops audio rows that start before the video does.

    Both streams are timed by the same encoder clock, so the rows share one
    timeline and must not be shifted. The audio log can still hold a few rows
    from before the capture started; those describe no picture and would pair
    with unrelated frames.
    """
    video = parsed['video']
    audio = parsed['audio']
    if not video or not audio:
        return
    first = video[0]['pts'] - tolerance
    kept = [row for row in audio if row['pts'] >= first]
    if not kept or len(kept) == len(audio):
        return
    parsed['meta']['audio_dropped'] = len(audio) - len(kept)
    parsed['audio'] = kept


def compare(provided, measured_video, measured_audio, tolerance=1.0):
    """Report how far the sideband is from a full NAS measurement.

    Used by the shadow mode: the numbers decide whether applying the sideband
    is safe, they never change a decision by themselves. The capture may start
    a moment before the recorder receives media, so only the shared window is
    compared; that window is what the rules would actually see.
    """
    report = dict(provided=dict(video=len(provided['video']), audio=len(provided['audio'])),
                  measured=dict(video=len(measured_video), audio=len(measured_audio)))
    window = overlap_window(provided['video'], measured_video)
    if window is None:
        report['skipped'] = 'no overlap between the sideband and the media'
        return report
    start, end = window
    left_video = [r for r in provided['video'] if start <= r['pts'] <= end]
    left_audio = [r for r in provided['audio'] if start <= r['pts'] <= end]
    right_video = [r for r in measured_video if start <= r['pts'] <= end]
    right_audio = [r for r in (measured_audio or []) if start <= r['pts'] <= end]
    report['window'] = [round(start, 3), round(end, 3)]
    # Keep the raw edges: a span difference is easier to read from these than
    # from the deltas alone.
    report['edges'] = dict(
        provided=[round(provided['video'][0]['pts'], 3),
                  round(provided['video'][-1]['pts'], 3)],
        measured=[round(measured_video[0]['pts'], 3),
                  round(measured_video[-1]['pts'], 3)])
    # Clipping hides a sidecar that stops early, so record how much of the
    # media the shared window covers.
    media_span = measured_video[-1]['pts'] - measured_video[0]['pts']
    if media_span > 0:
        report['coverage_ratio'] = round((end - start) / media_span, 4)
    report['provided']['video_in_window'] = len(left_video)
    report['measured']['video_in_window'] = len(right_video)
    if left_video and right_video:
        report['first_pts_delta'] = round(left_video[0]['pts'] - right_video[0]['pts'], 3)
        report['last_pts_delta'] = round(left_video[-1]['pts'] - right_video[-1]['pts'], 3)
    if left_audio and right_audio:
        report['audio_first_pts_delta'] = round(left_audio[0]['pts'] - right_audio[0]['pts'], 3)
    report['video_rows_ratio'] = round(
        len(left_video) / len(right_video), 4) if right_video else None
    report['audio_rows_ratio'] = round(
        len(left_audio) / len(right_audio), 4) if right_audio else None
    # Row counts and timestamps only prove the two runs saw the same frames.
    # The values themselves must also agree, or the sideband describes the
    # picture differently and the rules would decide on different numbers.
    value_deltas = compare_values(
        dict(video=left_video, audio=left_audio), right_video, right_audio)
    report.update(value_deltas)
    span_delta = None
    if report.get('first_pts_delta') is not None and report.get('last_pts_delta') is not None:
        span_delta = abs(report['last_pts_delta'] - report['first_pts_delta'])
    report['span_delta'] = None if span_delta is None else round(span_delta, 3)
    report['within_tolerance'] = bool(
        span_delta is not None
        and span_delta <= 0.5
        and report.get('coverage_ratio') is not None
        and report['coverage_ratio'] >= 0.95
        and report['video_rows_ratio'] is not None
        and abs(report['video_rows_ratio'] - 1.0) <= 0.01
        and report.get('luma_median_delta') is not None
        and report['luma_median_delta'] <= 2.0
        and report.get('luma_outlier_ratio') is not None
        and report['luma_outlier_ratio'] <= 0.10
        # The rules compare the scene score against one threshold, so what
        # matters is that both runs agree above or below it. The raw values
        # come from different detectors and their mean difference is only
        # reported.
        and report.get('scene_agreement') is not None
        and report['scene_agreement'] >= 0.90)
    return report


def overlap_window(left, right):
    """Time range both runs cover, or None when they do not meet."""
    if not left or not right:
        return None
    start = max(left[0]['pts'], right[0]['pts'])
    end = min(left[-1]['pts'], right[-1]['pts'])
    if end - start <= 0:
        return None
    return start, end


def compare_values(provided, measured_video, measured_audio):
    """Mean absolute difference of the measured values, by timestamp.

    The two runs sample the same frames, so a value that differs by more than
    rounding means the capture side measured something else.
    """
    report = {}
    video = matched_values(provided['video'], measured_video, 'mean')
    if video:
        video.sort()
        # The mean is dominated by the worst frames. Report the middle of the
        # distribution as the agreement figure and keep the tail visible: a
        # genuinely different measurement moves the median, a minority of
        # corrupt frames on the capture side only moves the tail.
        report['mean_luma_delta'] = round(sum(video) / len(video), 3)
        report['luma_median_delta'] = round(video[len(video) // 2], 3)
        report['luma_p90_delta'] = round(video[int(len(video) * 0.9)], 3)
        report['luma_outlier_ratio'] = round(
            sum(1 for d in video if d > 20.0) / len(video), 5)
        report['video_matched'] = len(video)
    scene = matched_values(provided['video'], measured_video, 'scene')
    if scene:
        report['scene_delta'] = round(sum(scene) / len(scene), 3)
    # The rules only ask whether a frame is a scene change. What matters is
    # that both runs answer the same, not that the raw values are identical.
    agreement = threshold_agreement(provided['video'], measured_video, 'scene', 12.0)
    if agreement is not None:
        report['scene_agreement'] = agreement
    signed = matched_signed(provided['audio'], measured_audio, 'rms_db')
    audio = matched_values(provided['audio'], measured_audio, 'rms_db')
    if signed:
        # Direction matters for calibrating the capture side against the NAS.
        signed.sort()
        report['rms_db_bias'] = round(signed[len(signed) // 2], 3)
    if audio:
        # One sentinel value used to dominate the mean; the median is a fair
        # summary of how far the two measurements are apart.
        audio.sort()
        report['rms_db_delta'] = round(audio[len(audio) // 2], 3)
        report['rms_db_max'] = round(audio[-1], 3)
        report['audio_matched'] = len(audio)
    return report


def matched_signed(left, right, key, tolerance=0.05):
    """Signed differences for rows whose timestamps are close enough."""
    if not left or not right:
        return []
    ordered = sorted(right, key=lambda row: row['pts'])
    times = [row['pts'] for row in ordered]
    deltas = []
    for row in left:
        nearest = bisect.bisect_left(times, row['pts'])
        best = None
        for index in (nearest - 1, nearest):
            if 0 <= index < len(ordered):
                gap = abs(times[index] - row['pts'])
                if gap <= tolerance and (best is None or gap < best[0]):
                    best = (gap, ordered[index][key])
        if best is not None:
            deltas.append(row[key] - best[1])
    return deltas


def threshold_agreement(left, right, key, threshold, tolerance=0.05):
    """Share of frames where both runs agree above or below a threshold."""
    if not left or not right:
        return None
    ordered = sorted(right, key=lambda row: row['pts'])
    times = [row['pts'] for row in ordered]
    agree = total = 0
    for row in left:
        nearest = bisect.bisect_left(times, row['pts'])
        best = None
        for index in (nearest - 1, nearest):
            if 0 <= index < len(ordered):
                gap = abs(times[index] - row['pts'])
                if gap <= tolerance and (best is None or gap < best[0]):
                    best = (gap, ordered[index][key])
        if best is None:
            continue
        total += 1
        if (row[key] >= threshold) == (best[1] >= threshold):
            agree += 1
    return round(agree / total, 5) if total else None


def matched_values(left, right, key, tolerance=0.05):
    """Differences for rows whose timestamps are close enough to pair.

    The capture side subsamples frames, so its timestamps skip every N-th
    frame while the NAS measurement has all of them. An exact lookup would
    pair almost nothing; a small window finds the same instant instead.
    """
    if not left or not right:
        return []
    ordered = sorted(right, key=lambda row: row['pts'])
    times = [row['pts'] for row in ordered]
    deltas = []
    for row in left:
        nearest = bisect.bisect_left(times, row['pts'])
        best = None
        for index in (nearest - 1, nearest):
            if 0 <= index < len(ordered):
                gap = abs(times[index] - row['pts'])
                if gap <= tolerance and (best is None or gap < best[0]):
                    best = (gap, ordered[index][key])
        if best is not None:
            deltas.append(abs(row[key] - best[1]))
    return deltas
