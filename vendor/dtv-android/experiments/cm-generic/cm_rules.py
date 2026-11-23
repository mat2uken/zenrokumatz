#!/usr/bin/env python3
"""Infer commercial intervals from measured programme features.

Nothing here decodes media or touches the filesystem. Input is the CSV already
produced by cm-scan and cm-overlay; output is a decision record.

The structure follows the principal Japanese open tool chain: chapter points
come from silence and scene changes, the station mark tells programme content
from a commercial break, and the break is only cut when its structure looks
like a break (several 15 second units, or one long unit).
"""

DEFAULTS = dict(
    # Chapter points: a scene change or black frame that coincides with silence.
    silence_db=-42.0,
    silence_min_seconds=0.15,
    silence_merge_seconds=0.10,
    scene_threshold=12.0,
    black_fraction=0.98,
    chapter_tolerance=0.35,
    # Overlay classification.
    bin_seconds=1.0,
    min_bin_samples=15,
    cluster_iterations=24,
    min_cluster_gap=0.25,
    unknown_band_ratio=0.12,
    min_present_ratio=0.50,
    max_present_ratio=0.995,
    # Break structure.
    unknown_gap_seconds=2.0,
    # A no-mark run whose length is a few seconds off a whole number of spots is
    # normally an over-extension. Retry it as the spot chain inside it, but only
    # for blocks the timing path also proposes: two independent detectors
    # agreeing is evidence neither can produce alone.
    trim_spot_blocks=True,
    min_break_seconds=12.0,
    max_break_seconds=300.0,
    unit_seconds=5.0,
    unit_tolerance=1.0,
    min_long_break_seconds=30.0,
    # A break shorter than two minutes is normally a whole number of 15 second
    # spots. A short block that does not fit means something else hid the mark.
    short_break_seconds=120.0,
    short_break_unit_seconds=15.0,
    short_break_tolerance=1.5,
    end_guard_seconds=90.0,
    end_card_seconds=12.0,
    edge_search_seconds=2.0,
    max_cut_ratio=0.45,
    min_coverage=0.78,
    # Timing-only fallback.
    timing_unit_seconds=15.0,
    timing_tolerance=1.5,
    timing_min_block_seconds=30.0,
    timing_min_silences=2,
    timing_min_show_segment=240.0,
    timing_max_cut_ratio=0.30,
    # How much the timing path may trust a measured mark. A mark on screen for
    # under 20 seconds cannot say where a break starts; JLS treats a mark seen
    # for less than that as no mark at all (WLogoSumMin). Its disappearance is
    # only meaningful for a mark that is on for most of the recording, because
    # a telop or a studio graphic also comes and goes.
    scenario_logo_min_present_seconds=20.0,
    scenario_logo_anchor_ratio=0.60,
    scenario_logo_anchor_window=30.0,
    scenario_logo_anchor_tolerance=1.2,
    # The timing path's own guards, taken from the rules JLS applies when no
    # mark is trusted: three spots or more, two only at an exact 30 seconds,
    # and no block whose silences no spot boundary explains.
    scenario_min_units=3,
    scenario_exact_tolerance=0.25,
    scenario_mute_extra_limit=8,
)


def settings(**overrides):
    merged = dict(DEFAULTS)
    merged.update(overrides)
    return merged


def read_rows(path):
    """Reads a CSV with a header line into a list of float dictionaries."""
    import csv
    from pathlib import Path
    with Path(path).open() as handle:
        return [{key: float(value) for key, value in row.items()} for row in csv.DictReader(handle)]


def median(values):
    if not values:
        return 0.0
    ordered = sorted(values)
    middle = len(ordered) // 2
    return ordered[middle] if len(ordered) % 2 else (ordered[middle - 1] + ordered[middle]) / 2.0


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round(fraction * (len(ordered) - 1)))))
    return ordered[index]


def silence_intervals(audio, options):
    """Contiguous quiet stretches, merging short interruptions."""
    intervals = []
    start = end = None
    for row in audio:
        if row['rms_db'] <= options['silence_db']:
            if start is None:
                start = row['pts']
            elif row['pts'] - end > options['silence_merge_seconds']:
                if end - start >= options['silence_min_seconds']:
                    intervals.append((start, end))
                start = row['pts']
            end = row['pts'] + row['samples'] / 48000.0
        elif start is not None:
            if end - start >= options['silence_min_seconds']:
                intervals.append((start, end))
            start = end = None
    if start is not None and end - start >= options['silence_min_seconds']:
        intervals.append((start, end))
    return intervals


def scene_boundaries(video, options):
    return [row['pts'] for row in video
            if row['scene'] >= options['scene_threshold'] or row['black_fraction'] >= options['black_fraction']]


def chapter_points(video, audio, options):
    """Scene changes corroborated by silence; the strongest points in a break.

    A commercial break starts and ends on a picture cut, and the audio is
    normally quiet at that same instant. Requiring both keeps the list short
    enough to use as cut edges without inventing edges inside a programme.
    """
    silence = silence_intervals(audio, options)
    tolerance = options['chapter_tolerance']
    points = []
    for row in video:
        cuts = row['scene'] >= options['scene_threshold'] or row['black_fraction'] >= options['black_fraction']
        if not cuts:
            continue
        if any(start - tolerance <= row['pts'] <= end + tolerance for start, end in silence):
            if not points or row['pts'] - points[-1] > 0.2:
                points.append(row['pts'])
    return points, silence


def classify_bins(overlay, options):
    """Splits the per-second overlay measurements into present and absent.

    Thresholds are learned per recording: stations differ in how strongly the
    mark correlates and transcoding changes it further. Otsu splitting finds
    the boundary between the two populations even when recording starts and
    ends blur them; a band around the boundary stays unknown so that a fading
    mark never decides a cut on its own.
    """
    usable = [row for row in overlay if row['samples'] >= options['min_bin_samples']]
    if len(usable) < 10:
        return {}, None
    values = sorted(row['correlation'] for row in usable)
    total = sum(values)
    left_sum = 0.0
    best_score, best_index = -1.0, None
    for index in range(1, len(values)):
        left_sum += values[index - 1]
        left_count = index
        right_count = len(values) - index
        left_mean = left_sum / left_count
        right_mean = (total - left_sum) / right_count
        score = left_count * right_count * (left_mean - right_mean) ** 2
        if score > best_score:
            best_score, best_index = score, index
    if best_index is None:
        return {}, None
    low_centre = sum(values[:best_index]) / best_index
    high_centre = sum(values[best_index:]) / (len(values) - best_index)
    separation = high_centre - low_centre
    if separation < options['min_cluster_gap']:
        return {}, None
    boundary = (values[best_index - 1] + values[best_index]) / 2.0
    band = options['unknown_band_ratio'] * separation
    states = {}
    for row in overlay:
        bin_start = int(row['bin_start'])
        if row['samples'] < options['min_bin_samples']:
            states[bin_start] = 'unknown'
        elif row['correlation'] >= boundary + band:
            states[bin_start] = 'present'
        elif row['correlation'] <= boundary - band:
            states[bin_start] = 'absent'
        else:
            states[bin_start] = 'unknown'
    return states, dict(low=low_centre, high=high_centre, gap=separation,
                        boundary=boundary, band=band)


def bin_runs(states, wanted, bin_seconds, gap_seconds):
    """Runs of `wanted` bins, bridging short runs of another state only when the
    bridged bins are unknown."""
    runs = []
    for bin_start in sorted(states):
        if states[bin_start] != wanted:
            continue
        lo, hi = bin_start * bin_seconds, (bin_start + 1) * bin_seconds
        if runs and lo - runs[-1][1] <= gap_seconds + 1e-9:
            bridged = True
            for probe in range(int(round(runs[-1][1] / bin_seconds)), int(round(lo / bin_seconds))):
                if states.get(probe, 'unknown') != 'unknown':
                    bridged = False
                    break
            if bridged:
                runs[-1][1] = hi
                continue
        runs.append([lo, hi])
    return [tuple(run) for run in runs]


def snap(interval, chapters, distance, low_limit, high_limit):
    """Moves each edge onto the nearest chapter point inside the window."""
    start, end = interval
    near_start = [point for point in chapters if abs(point - start) <= distance]
    near_end = [point for point in chapters if abs(point - end) <= distance]
    if near_start:
        start = min(near_start, key=lambda point: abs(point - start))
    if near_end:
        end = min(near_end, key=lambda point: abs(point - end))
    start = max(low_limit, start)
    end = min(high_limit, end)
    return (start, end)


def unit_alignment(duration, options):
    """How closely a break length matches a whole number of sold spots."""
    unit = options['unit_seconds']
    units = max(1, round(duration / unit))
    return abs(duration - units * unit)


def structure_breaks(runs, duration, options):
    """Keeps breaks that look like a real commercial block.

    Short and isolated no-mark segments are normally sponsor credits or an end
    card, both of which are programme material and must stay.
    """
    kept, rejected = [], []
    for lo, hi in runs:
        length = hi - lo
        late = duration - hi <= options['end_guard_seconds']
        if late and length <= options['end_card_seconds']:
            rejected.append(dict(interval=[lo, hi], reason='end_card'))
            continue
        if length < options['min_break_seconds']:
            rejected.append(dict(interval=[lo, hi], reason='too_short'))
            continue
        if length > options['max_break_seconds']:
            rejected.append(dict(interval=[lo, hi], reason='too_long'))
            continue
        aligned = unit_alignment(length, options) <= options['unit_tolerance']
        if not aligned and length < options['min_long_break_seconds']:
            rejected.append(dict(interval=[lo, hi], reason='unusual_length'))
            continue
        if length < options['short_break_seconds']:
            spot_units = max(1, round(length / options['short_break_unit_seconds']))
            if abs(length - spot_units * options['short_break_unit_seconds']) > options['short_break_tolerance']:
                rejected.append(dict(interval=[lo, hi], reason='short_break_not_a_spot_multiple'))
                continue
        kept.append(dict(interval=[lo, hi], length=length, aligned=aligned))
    return kept, rejected


def overlaps(interval, spans):
    lo, hi = interval
    return any(lo < end and start < hi for start, end in spans)


def measurable_windows(states, options, minimum=60.0):
    """Stretches of the recording where the marker could be classified.

    A programme whose marker is only measurable for part of its length is not
    useless: the measurable part can still be judged on its own. Unknown bins
    are tolerated for a moment, because a single unclear second does not make
    the neighbourhood unusable.
    """
    gap = options['unknown_gap_seconds']
    windows = []
    start = None
    last = None
    for second in sorted(states):
        if states[second] in ('present', 'absent'):
            if start is None:
                start = second
            elif last is not None and second - last > gap:
                windows.append((start, last + options['bin_seconds']))
                start = second
            last = second
        elif start is not None and last is not None and second - last > gap:
            windows.append((start, last + options['bin_seconds']))
            start = None
    if start is not None and last is not None:
        windows.append((start, last + options['bin_seconds']))
    return [(a, b) for a, b in windows if b - a >= minimum]


def anchored_edges(start, end, chapters, options=None):
    """True when a transition explains both edges of a candidate break.

    Measured on four labelled programmes: real edges sit within 2.7 seconds of a
    transition, while the false break (mark hidden by a studio graphic) had its
    edges 42 and 58 seconds away.
    """
    tolerance = 3.0 if not options else options.get('anchor_tolerance', 3.0)
    near_start = any(abs(point - start) <= tolerance for point in chapters)
    near_end = any(abs(point - end) <= tolerance for point in chapters)
    return near_start and near_end


def spot_chain(lo, hi, chapters, duration, options):
    """The spot-aligned block inside a no-mark run that was refused whole.

    A run a few seconds off a whole number of spots is usually an over-extension:
    the mark stayed hidden past the end of the block. The block itself is still
    visible, because every boundary between two spots is a picture cut with
    silence - the same chapter points the cut edges use. Walk in spot steps from
    the end of the run (the mark returns there) and from its start, and keep the
    longer chain.
    """
    unit = options['short_break_unit_seconds']
    tolerance = options['short_break_tolerance']

    def on_boundary(point):
        return any(abs(candidate - point) <= tolerance for candidate in chapters)

    best = None
    for anchor, direction in ((hi, -1), (lo, 1)):
        edge, units = anchor, 0
        while True:
            step = edge + direction * unit
            if step < lo - tolerance or step > hi + tolerance:
                break
            if step < 0 or step > duration:
                break
            if not on_boundary(step):
                break
            edge = step
            units += 1
        if not units:
            continue
        start, end = min(anchor, edge), max(anchor, edge)
        if end - start < options['min_break_seconds']:
            continue
        if best is None or end - start > best[1] - best[0]:
            best = (start, end)
    return best


def recover_spot_blocks(rejected, chapters, duration, options):
    """Retries the refusals that look like an over-extended block."""
    recovered, kept = [], []
    for item in rejected:
        if item.get('reason') not in ('short_break_not_a_spot_multiple', 'unusual_length'):
            kept.append(item)
            continue
        found = spot_chain(item['interval'][0], item['interval'][1], chapters, duration, options)
        if found:
            recovered.append(dict(interval=found, length=found[1] - found[0],
                                  aligned=True, trimmed_from=item['interval']))
        else:
            kept.append(item)
    return recovered, kept


def corroborated_blocks(video, audio, overlays, duration, options, timing_cuts, excluded,
                        classified=None, points=None):
    """Trimmed blocks that the timing path also proposes, with their confidence.

    Each candidate reports the same span when several of them measure the same
    interval, so the spans are keyed and kept once. Duplicates used to travel
    into the decision: the cut list then showed one interval three times, and
    cut_ratio counted it three times, which could refuse a break that was well
    under the limit.
    """
    agreed = {}
    for index, overlay in enumerate(overlays):
        result = overlay_decision(video, audio, overlay, duration, options, excluded,
                                  None if classified is None else classified[index], points)
        if not result or not result.get('cuts'):
            continue
        gap = (result.get('clusters') or {}).get('gap') or 0.0
        score = min(1.0, gap / 0.6)
        for start, end in result['cuts']:
            if any(start < other_end and other_start < end for other_start, other_end in timing_cuts):
                key = (round(start, 3), round(end, 3))
                if key not in agreed or score > agreed[key][1]:
                    agreed[key] = ((start, end), score)
    return [agreed[key] for key in sorted(agreed)]


def local_breaks(states, chapters, duration, options):
    """Breaks found only inside stretches where the marker was measurable."""
    cuts = []
    for start, end in measurable_windows(states, options):
        part = {key: value for key, value in states.items() if start <= key * options['bin_seconds'] < end}
        if not part:
            continue
        raw = bin_runs(part, 'absent', options['bin_seconds'], options['unknown_gap_seconds'])
        snapped = [snap(run, chapters, options['edge_search_seconds'], start, end) for run in raw]
        breaks, _ = structure_breaks([run for run in snapped if run[1] > run[0]], end - start, options)
        for item in breaks:
            lo, hi = item['interval']
            if lo >= start and hi <= end and lo - start >= 2.0:
                cuts.append((lo, hi))
    return cuts


def overlay_decision(video, audio, overlay, duration, options, excluded=(), classified=None,
                     points=None):
    chapters, silence = points if points is not None else chapter_points(video, audio, options)
    states, clusters = classified if classified is not None else classify_bins(overlay, options)
    if not states:
        return None
    total = len(states)
    present = sum(1 for state in states.values() if state == 'present')
    absent = sum(1 for state in states.values() if state == 'absent')
    unknown = total - present - absent
    coverage = (total - unknown) / total if total else 0.0
    ratio = present / max(1, present + absent)
    if ratio < options['min_present_ratio'] or ratio > options['max_present_ratio']:
        return dict(marker=None, reason='marker_not_present_enough', coverage=coverage,
                    present_ratio=ratio, clusters=clusters)
    if coverage < options['min_coverage'] and options.get('allow_local_windows', True):
        local = local_breaks(states, chapters, duration, options)
        if local:
            removed = sum(b - a for a, b in local)
            if removed / duration <= options['max_cut_ratio']:
                return dict(marker='absent_runs', reason='local_breaks', cuts=local,
                            coverage=coverage, present_ratio=ratio, local_windows=True,
                            cut_ratio=removed / duration, confidence=0.0, clusters=clusters,
                            chapters=len(chapters), silence_intervals=len(silence))
        return dict(marker=None, reason='low_coverage', coverage=coverage,
                    present_ratio=ratio, clusters=clusters)
    raw = bin_runs(states, 'absent', options['bin_seconds'], options['unknown_gap_seconds'])
    snapped = [snap(run, chapters, options['edge_search_seconds'], 0.0, duration) for run in raw]
    breaks, rejected = structure_breaks([run for run in snapped if run[1] > run[0]], duration, options)
    if options.get('trim_spot_blocks'):
        recovered, rejected = recover_spot_blocks(rejected, chapters, duration, options)
        breaks.extend(recovered)
    # Both edges of a real break sit on a transition: the picture and the sound
    # change when a spot starts or ends. A run whose edges no transition
    # explains is far more likely to be the mark hidden behind a graphic, so it
    # is refused instead of cutting programme.
    anchored = []
    for item in breaks:
        start, end = item['interval']
        if anchored_edges(start, end, chapters, options):
            anchored.append(item)
        else:
            rejected.append(dict(interval=item['interval'], reason='edge_not_on_a_transition'))
    breaks = anchored
    # A break that starts at the first frame cannot be checked: the recording
    # began somewhere inside it, so its true start is unknown.
    kept = []
    for item in breaks:
        if item['interval'][0] < 2.0:
            rejected.append(dict(interval=item['interval'], reason='boundary_at_recording_start'))
        else:
            kept.append(item)
    breaks = kept
    if excluded:
        # A cut that touches a gap in the recording cannot be verified against
        # the source, so that break is left alone and the rest is still cut.
        kept = []
        for item in breaks:
            if overlaps(item['interval'], excluded):
                rejected.append(dict(interval=item['interval'], reason='source_gap'))
            else:
                kept.append(item)
        breaks = kept
    removed = sum(item['length'] for item in breaks)
    cut_ratio = removed / duration if duration else 1.0
    if cut_ratio > options['max_cut_ratio']:
        return dict(marker='absent_runs', reason='too_much_removed', coverage=coverage,
                    present_ratio=ratio, cut_ratio=cut_ratio, clusters=clusters,
                    rejected=rejected)
    confidence = min(1.0, coverage) * min(1.0, clusters['gap'] / 0.6 if clusters else 0.0)
    return dict(marker='absent_runs', reason='breaks_found' if breaks else 'no_breaks',
                cuts=[item['interval'] for item in breaks], breaks=breaks, rejected=rejected,
                coverage=coverage, present_ratio=ratio, cut_ratio=cut_ratio,
                confidence=round(confidence, 4), clusters=clusters,
                chapters=len(chapters), silence_intervals=len(silence))


def timing_decision(video, audio, duration, options, excluded=()):
    """Fallback used when no station mark can be trusted.

    Without a mark the only usable structure is timing, which is much weaker
    evidence. The rules therefore demand a break that looks like a sold block:
    whole 15 second units, several spot boundaries (silence) inside it, enough
    programme either side, and a small share of the recording overall.
    """
    chapters, silence = chapter_points(video, audio, options)
    # A segment is the interval between consecutive chapter points. Only those
    # that measure a whole number of 15 second spots can be a commercial, and
    # because the endpoints are consecutive no programme scene change hides
    # inside. Merging arbitrary pairs instead chains the whole recording.
    spans = []
    for start, end in zip(chapters, chapters[1:]):
        length = end - start
        if length > options['max_break_seconds']:
            continue
        units = max(1, round(length / options['timing_unit_seconds']))
        if abs(length - units * options['timing_unit_seconds']) > options['timing_tolerance']:
            continue
        spans.append([start, end])
    merged = []
    for start, end in sorted(spans):
        if merged and abs(start - merged[-1][1]) <= 0.25:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])

    def inner_silences(block):
        lo, hi = block
        return sum(1 for start, end in silence if start > lo and end < hi)

    breaks, rejected = [], []
    for block in merged:
        length = block[1] - block[0]
        if length < options['timing_min_block_seconds']:
            rejected.append(dict(interval=block, reason='too_short'))
            continue
        if any(block[0] < end and start < block[1] for start, end in excluded):
            rejected.append(dict(interval=block, reason='source_gap'))
            continue
        # A block of spots is separated by short silences at every spot edge.
        if inner_silences(block) < options['timing_min_silences']:
            rejected.append(dict(interval=block, reason='too_few_spot_boundaries'))
            continue
        breaks.append(dict(interval=block, length=length))

    # Programme must be long on both sides: a burst of timing coincidences in
    # one area is far more likely to be programme than several breaks.
    guarded = []
    for item in breaks:
        lo, hi = item['interval']
        before = lo
        after = duration - hi
        if 0 < before < options['timing_min_show_segment'] or 0 < after < options['timing_min_show_segment']:
            rejected.append(dict(interval=item['interval'], reason='programme_segment_too_short'))
            continue
        guarded.append(item)

    removed = sum(item['length'] for item in guarded)
    cut_ratio = removed / duration if duration else 1.0
    if cut_ratio > options['timing_max_cut_ratio']:
        return dict(marker=None, reason='too_much_removed', cuts=[], cut_ratio=cut_ratio,
                    rejected=rejected, chapters=len(chapters))
    return dict(marker=None, reason='timing_breaks' if guarded else 'no_breaks',
                cuts=[item['interval'] for item in guarded], breaks=guarded, rejected=rejected,
                cut_ratio=cut_ratio, confidence=0.0, chapters=len(chapters),
                silence_intervals=len(silence))


def adapt_bin_samples(video, options):
    """Scales the per-bin sample floor to the recording's frame rate.

    Broadcast recordings are not always 30 frames per second: an encoder that
    skips duplicate frames leaves some seconds with far fewer pictures, and a
    fixed floor then marks a third of the programme as unmeasurable. The floor
    is therefore tied to the observed median.
    """
    counts = {}
    for row in video:
        second = int(row['pts'])
        counts[second] = counts.get(second, 0) + 1
    if not counts:
        return options
    values = sorted(counts.values())
    median = values[len(values) // 2]
    floor = max(5, min(options['min_bin_samples'], int(median * 0.5)))
    return dict(options, min_bin_samples=floor)


def logo_evidence(states, clusters, options):
    """How much a measured mark can say about this recording.

    ``usable`` is the floor for using the mark as a score term: the two
    populations of bins have to separate at all, and the mark has to be on
    screen for more than a moment. ``anchor`` is the stricter floor for using
    the mark's disappearance as a cut edge: only a mark that is on for most of
    the recording makes its absence mean a break.
    """
    if not states:
        return None
    present = sum(1 for state in states.values() if state == 'present')
    absent = sum(1 for state in states.values() if state == 'absent')
    known = present + absent
    if not known:
        return None
    bin_seconds = options['bin_seconds']
    ratio = present / known
    gap = (clusters or {}).get('gap') or 0.0
    usable = (gap >= options['min_cluster_gap']
              and present * bin_seconds >= options['scenario_logo_min_present_seconds'])
    return dict(present_seconds=round(present * bin_seconds, 3),
                present_ratio=round(ratio, 4),
                coverage=round(known / len(states), 4),
                cluster_gap=round(gap, 4),
                usable=usable,
                anchor=usable and ratio >= options['scenario_logo_anchor_ratio'])


def scenario_logo(overlays, options, classified=None):
    """The candidate the timing path may trust, as compact evidence.

    analyze() has already split every candidate into present and absent bins,
    so ranking them costs no extra pass over the recording. Only a candidate
    that measures as a real, mostly-present mark is returned. Passing the
    first candidate unconditionally handed the timing path a cached mark that
    measures nothing, and "the mark is absent" then held for every second of
    the recording.
    """
    if not overlays:
        return None
    pairs = classified if classified is not None else [
        classify_bins(overlay, options) for overlay in overlays]
    best, best_score = None, 0.0
    for index, (states, clusters) in enumerate(pairs):
        evidence = logo_evidence(states, clusters, options)
        if not evidence or not evidence['usable']:
            continue
        score = evidence['cluster_gap'] * evidence['present_ratio']
        if score > best_score:
            best = dict(evidence, index=index, states=states)
            best_score = score
    return best


def analyze(video, audio, overlays, duration, options=None, allow_timing=False, excluded=()):
    """Chooses the best marker and returns the decision record."""
    options = adapt_bin_samples(video, options or settings())
    # The trim pass only runs once the plain pass has found nothing, and then
    # only for blocks the timing path also proposes, so the loop below keeps it
    # off: an unconfirmed trim must never become the chosen marker on its own.
    plain = dict(options, trim_spot_blocks=False) if options.get('trim_spot_blocks') else options
    # One pass over the recording answers what every candidate needs. Both of
    # these used to be rebuilt once per candidate, and the trim pass rebuilt
    # them again, so four candidates read the whole feature table eight times.
    points = chapter_points(video, audio, options)
    classified = [classify_bins(overlay, options) for overlay in overlays]
    best = None
    evaluations = []
    details = []
    for index, overlay in enumerate(overlays):
        result = overlay_decision(video, audio, overlay, duration, plain, excluded,
                                  classified[index], points)
        if result is None:
            evaluations.append(dict(index=index, reason='not_enough_bins'))
            continue
        details.append(result)
        evaluations.append(dict(index=index, reason=result['reason'], coverage=result.get('coverage'),
                                present_ratio=result.get('present_ratio'), cut_ratio=result.get('cut_ratio'),
                                clusters=result.get('clusters')))
        score = result.get('confidence', 0.0) * (1.0 if result.get('cuts') else 0.1)
        if result.get('cuts') and (best is None or score > best[0]):
            best = (score, index, result)
    # A no-mark stretch the structure refused whole may still hold a block the
    # timing path also proposes. Two independent detectors agreeing is evidence
    # neither can produce alone, so that block is cut; the rest of the refused
    # stretch stays untouched. Only runs rejected for their length are retried,
    # and only when there are any, so the timing path is not run for nothing.
    corroborated = []
    if options.get('trim_spot_blocks') and allow_timing and overlays:
        retryable = any(item.get('reason') in ('short_break_not_a_spot_multiple', 'unusual_length')
                        for result in details for item in (result.get('rejected') or []))
        if retryable:
            import cm_scenario
            logo = scenario_logo(overlays, options, classified)
            timing = [tuple(item) for item in
                      cm_scenario.decide(video, audio, duration, options, logo)['cuts']]
            if timing:
                corroborated = corroborated_blocks(video, audio, overlays, duration, options,
                                                   timing, excluded, classified, points)
    if best is not None:
        _, index, result = best
        result = dict(result, mode='overlay', marker_index=index, candidates=evaluations)
        cuts = [tuple(cut) for cut in result['cuts']]
        added = []
        for span, score in corroborated:
            start, end = span
            if any(start < other_end and other_start < end for other_start, other_end in cuts):
                continue
            if not duration:
                continue
            if (sum(b - a for a, b in cuts) + (end - start)) / duration > options['max_cut_ratio']:
                continue
            cuts.append(span)
            added.append([round(start, 3), round(end, 3)])
        if added:
            cuts.sort()
            removed = sum(b - a for a, b in cuts)
            result = dict(result, cuts=cuts, cut_ratio=removed / duration if duration else 1.0,
                          corroborated=added)
        if result['coverage'] < options['min_coverage'] and not result.get('local_windows'):
            return dict(result, cuts=[], reason='low_coverage')
        return result
    if corroborated:
        kept = [span for span, _ in corroborated]
        return dict(mode='overlay', reason='breaks_found', cuts=kept,
                    cut_ratio=sum(b - a for a, b in kept) / duration if duration else 1.0,
                    confidence=round(max(score for _, score in corroborated), 4),
                    corroborated=[[round(a, 3), round(b, 3)] for a, b in kept],
                    candidates=evaluations)
    if allow_timing:
        # join_logo_scp style inference: 15 second spot chains anchored on
        # silences, filtered by the block being louder than the programme.
        import cm_scenario
        logo = scenario_logo(overlays, options, classified)
        scenario = cm_scenario.decide(video, audio, duration, options, logo)
        cuts = [tuple(item) for item in scenario['cuts']]

        if excluded:
            cuts = [item for item in cuts
                    if not any(item[0] < end and start < item[1] for start, end in excluded)]
        return dict(mode='scenario', reason='timing_breaks' if cuts else 'no_breaks',
                    cuts=cuts, cut_ratio=sum(b - a for a, b in cuts) / duration if duration else 0.0,
                    confidence=0.0, candidates=evaluations,
                    logo_index=(logo or {}).get('index'),
                    logo_present=(logo or {}).get('present_ratio'),
                    logo_anchor=bool((logo or {}).get('anchor')),
                    anchored=scenario.get('anchored'),
                    scenario_candidates=scenario.get('candidates'), scores=scenario.get('scores'))
    detailed = [item for item in evaluations if item.get('coverage') is not None]
    reason = 'no_usable_marker'
    detail = None
    if detailed:
        best_detail = max(detailed, key=lambda item: item.get('coverage') or 0)
        reason = best_detail.get('reason') or reason
        # Keep the closest candidate's own account of why it was refused.
        detail = max((item for item in details if item.get('coverage') is not None),
                     key=lambda item: item.get('coverage') or 0, default=None)
    return dict(mode='none', reason=reason, cuts=[], candidates=evaluations,
                rejected=(detail or {}).get('rejected'),
                coverage=(detail or {}).get('coverage'))
