#!/usr/bin/env python3
"""Segment-based break inference, modelled on join_logo_scp's 推測構成.

join_logo_scp searches for Japanese spot structure (whole 15 second units) and
labels every segment (programme / CM / sponsor / trailer / endcard). Its rules
are structural, so it keeps working when the station mark is missing.

This module builds the same picture for one recording:

* boundaries come from silences and scene changes,
* a block is a chain of spot boundaries whose total length is a whole number of
  15 second spots,
* the best non-overlapping set of blocks is chosen by weighted interval
  scheduling, so two hypotheses never overlap.

Three guards keep a chain of coincidences from becoming a cut. They are the
ones join_logo_scp applies on its logo-less path, where a 15 second coincidence
is far too common to be evidence on its own:

* a block is three spots or more; two are accepted only when the length is
  exactly 30 seconds (join_logo_scp's MgnCmDetect),
* a block whose silences do not sit on its own spot boundaries is refused, as a
  busy programme has speech gaps everywhere while a sold block is quiet only at
  its boundaries (its checkMuteMany),
* when the station mark was measured and is trustworthy, a block that begins
  where the mark is on screen starts inside programme material and is moved
  forward to the first second without the mark.

Nothing here decides on its own; the caller compares the chosen blocks with the
recording before anything is cut.

The feature rows are indexed once per run. They used to be re-scanned for every
candidate block, which cost minutes of pure list traversal on a 30 minute
programme.
"""
import bisect

UNIT = 15.0
TOLERANCE = 1.5
MIN_BLOCK = 30.0   # a single spot alone is too easy to confuse with programme
MAX_BLOCK = 300.0
MIN_PROGRAMME = 200.0
SCENE_CUT = 25.0
# A two spot block is only a break when its length is exact; anything shorter
# is one spot and is never a block.
MIN_UNITS = 3
EXACT_TOLERANCE = 0.25
# A block has to score at least this much to be cut. Expansion re-checks it:
# a unit added over the station mark turns a spot block into programme, and the
# expanded block used to be cut without asking again.
MIN_SCORE = 2.0
# A block that begins at the first frame cannot be checked: the recording
# started somewhere inside it, or just before it, so its true start is unknown.
# The mark path refuses those within 2 s; a block that starts a few seconds in
# is just as unverifiable, and on the labelled material such a block cut
# programme. JLS treats the first 12 s the same way (its LgCutFirst).
START_GUARD = 12.0
# How far forward a start may be moved onto the mark's disappearance.
ANCHOR_WINDOW = 30.0
ANCHOR_TOLERANCE = 1.2


class Track:
    """One measured column of the recording, indexed by time."""

    def __init__(self, rows, key):
        self.pts = [row['pts'] for row in rows]
        self.values = [row.get(key) for row in rows]

    def between(self, start, end):
        left = bisect.bisect_left(self.pts, start)
        right = bisect.bisect_left(self.pts, end)
        return self.values[left:right]


def _option(options, name, default):
    return default if not options else options.get(name, default)


def silences(audio, options=None):
    """Silence runs with the shape a spot boundary has.

    Japanese spot material carries about half a second of silence before and
    after each commercial, so length and depth are kept with the position.
    """
    floor = _option(options, 'scenario_silence_db', -55.0)
    shortest = _option(options, 'scenario_silence_seconds', 0.15)
    runs = []
    start = None
    previous = None
    for row in audio:
        if row['rms_db'] < floor:
            if start is None:
                start = row['pts']
        elif start is not None:
            runs.append((start, row['pts']))
            start = None
        previous = row['pts']
    if start is not None and previous is not None:
        runs.append((start, previous))

    track = Track(audio, 'rms_db')
    shaped = []
    for start, end in runs:
        if end - start < shortest:
            continue
        inside = track.between(start, end)
        before = track.between(start - 1.2, start - 0.2)
        after = track.between(end + 0.2, end + 1.2)
        shaped.append(dict(start=round(start, 3), end=round(end, 3),
                           duration=round(end - start, 3),
                           depth=round(min(inside), 1) if inside else floor,
                           before=round(sorted(before)[len(before) // 2], 1) if before else None,
                           after=round(sorted(after)[len(after) // 2], 1) if after else None))
    return shaped


def snap(run, video, options=None):
    """Places a boundary on the picture cut inside the silence.

    Cutting at the middle of a silence puts the edit up to half a second away
    from the picture change. When the decoder saw a scene change inside the run,
    that instant is the better edge.
    """
    floor = _option(options, 'scenario_scene', SCENE_CUT)
    inside = [row['pts'] for row in video
              if run['start'] <= row['pts'] <= run['end'] and (row.get('scene') or 0.0) >= floor]
    if inside:
        middle = (run['start'] + run['end']) / 2.0
        return min(inside, key=lambda t: abs(t - middle))
    return (run['start'] + run['end']) / 2.0


def block_score(block, silence_marks, logo, duration, options=None):
    """How much a candidate block looks like a sold block of spots.

    ``logo`` is the evidence the caller prepared for this recording, or None
    when no mark could be trusted. A mark that is mostly absent inside the
    block supports it; one that stays on screen contradicts it. With no
    trustworthy mark the term is skipped rather than awarded: a cached mark
    that measures nothing still produces a state for every second, and counting
    it as "absent" made every candidate pass the score floor.
    """
    start, end = block
    length = end - start
    if not (MIN_BLOCK <= length <= MAX_BLOCK):
        return None
    units = round(length / UNIT)
    if units < 1 or abs(length - units * UNIT) > TOLERANCE:
        return None
    left = bisect.bisect_left(silence_marks, start)
    right = bisect.bisect_left(silence_marks, end)
    start_silent = left < len(silence_marks) and abs(silence_marks[left] - start) <= 1.2
    end_silent = right < len(silence_marks) and abs(silence_marks[right] - end) <= 1.2
    inner = sum(1 for t in silence_marks if start + 1.0 < t < end - 1.0)
    score = 0.0
    score += 2.0 if (start_silent and end_silent) else (-1.0 if not start_silent else 0.0)
    score += 1.0 if units >= 2 else -0.5
    score += 0.5 * min(inner, 4)
    score += 0.3 * units
    if logo is not None:
        states = logo.get('states') or {}
        known = [states[second] for second in range(int(start), int(end) + 1)
                 if states.get(second) in ('present', 'absent')]
        if known:
            present = sum(1 for state in known if state == 'present') / len(known)
            # A sold block does not carry the station mark. When the mark is on
            # screen for essentially the whole block, the block is programme and
            # no amount of spot-shaped silence makes it a break: measured on the
            # confirmed labels, a 43.9 s block inside the mark scored 2.9 purely
            # from length and silences and was cut as programme.
            if (len(known) >= 10
                    and present >= _option(options, 'scenario_logo_veto_ratio', 0.90)):
                return None
            score += 2.0 if present <= 0.25 else (-3.0 if present >= 0.6 else 0.0)
    if start < 5.0 or end > duration - 5.0:
        score -= 1.0
    return score


def mute_density_ok(start, end, units, silence_marks, options=None):
    """True when the silences inside a block sit on its own spot boundaries.

    A sold block is quiet at the 15 second boundaries between its spots and loud
    in between. Programme material has speech gaps everywhere, so a chain of
    coincidences inside a programme carries silences that no boundary explains.
    join_logo_scp cancels its logo-less guess for exactly this reason.
    """
    limit = min(int(_option(options, 'scenario_mute_extra_limit', 8)),
                int(round(units)) + 1)
    left = bisect.bisect_left(silence_marks, start + 1.0)
    right = bisect.bisect_left(silence_marks, end - 1.0)
    extra = 0
    span = end - start
    for moment in silence_marks[left:right]:
        offset = moment - start
        nearest = round(offset / UNIT) * UNIT
        if not 0.0 < nearest < span or abs(offset - nearest) > TOLERANCE:
            extra += 1
            if extra > limit:
                return False
    return True


def choose(blocks, duration, min_score=MIN_SCORE, min_programme=MIN_PROGRAMME):
    """Weighted interval scheduling over the scored blocks."""
    usable = [b for b in blocks if b['score'] >= min_score]
    usable.sort(key=lambda b: b['end'])
    best = []
    for index, item in enumerate(usable):
        best.append(dict(item, previous=None, total=item['score']))
        for earlier in range(index):
            other = usable[earlier]
            if other['end'] + min_programme > item['start']:
                continue
            if best[earlier]['total'] + item['score'] > best[index]['total']:
                best[index] = dict(item, previous=earlier, total=best[earlier]['total'] + item['score'])
    if not best:
        return []
    cursor = max(range(len(best)), key=lambda i: best[i]['total'])
    chosen = []
    while cursor is not None:
        chosen.append(usable[cursor])
        cursor = best[cursor]['previous']
    chosen.reverse()
    return chosen


def block_loudness(audio, start, end, pad=120.0):
    """Median level inside the block against the programme around it."""
    inside = sorted(audio.between(start, end))
    outside = sorted(audio.between(start - pad, start) + audio.between(end, end + pad))
    if not inside or not outside:
        return None
    return inside[len(inside) // 2] - outside[len(outside) // 2]


def cuts_per_unit(video, start, end):
    rows = video.between(start, end)
    return sum(1 for value in rows if (value or 0.0) >= SCENE_CUT)


def expand(block, edges, video, duration, options=None):
    """Grows a core block over neighbouring spot units.

    A sold unit is cut about seven to ten times per 15 seconds, while a
    programme unit between two quiet moments is cut once or twice. The bar is
    relative to the core block's own density: animation and action are cut
    often in the programme too.
    """
    floor = _option(options, 'scenario_expand_min_cuts', 5)
    ratio = _option(options, 'scenario_expand_ratio', 0.5)
    limit = _option(options, 'scenario_max_block', MAX_BLOCK)
    left, right = block['start'], block['end']
    core_units = max(1.0, (block['end'] - block['start']) / UNIT)
    core_rate = cuts_per_unit(video, block['start'], block['end']) / core_units
    need = max(floor, ratio * core_rate)

    def silent(moment):
        index = bisect.bisect_left(edges, moment)
        return index < len(edges) and abs(edges[index] - moment) <= 1.2

    while right - left + UNIT <= limit:
        # The new edge has to be a spot boundary itself, or the block would end
        # somewhere the cut cannot be verified.
        if not silent(right + UNIT) or cuts_per_unit(video, right, right + UNIT) < need:
            break
        right += UNIT
    while right - left + UNIT <= limit:
        if not silent(left - UNIT) or cuts_per_unit(video, left - UNIT, left) < need:
            break
        left -= UNIT
    return dict(block, start=left, end=right,
                expanded=round(right - left - (block['end'] - block['start']), 3))


def _start_has_mark(states, start):
    """True when the two seconds around a block start are mark-present."""
    inside = [states.get(second) for second in (int(start), int(start) + 1)]
    return 'present' in inside and 'absent' not in inside


def _snap_to_edge(moment, edges, tolerance):
    """The picture cut nearest to ``moment``, when one is close enough."""
    index = bisect.bisect_left(edges, moment)
    best = None
    for probe in (index - 1, index):
        if 0 <= probe < len(edges):
            gap = abs(edges[probe] - moment)
            if gap <= tolerance and (best is None or gap < best[0]):
                best = (gap, edges[probe])
    return best[1] if best else moment


def anchor_start(block, logo, edges, options=None):
    """Moves a start that falls inside the mark onto the mark's disappearance.

    A station mark is on screen during the programme and, in this market, often
    on the trailers that run before the spots. When the block begins where the
    mark is present it begins inside programme material, and the first second
    without the mark is where the sold block actually starts.

    The rule only moves a start later, and only when the moved block is still a
    whole number of spots: a wrong anchor gives programme back instead of
    cutting more, so it cannot make the result unsafe.
    """
    if not logo or not logo.get('anchor'):
        return block
    states = logo.get('states') or {}
    start, end = block['start'], block['end']
    if not _start_has_mark(states, start):
        return block
    window = _option(options, 'scenario_logo_anchor_window', ANCHOR_WINDOW)
    tolerance = _option(options, 'scenario_logo_anchor_tolerance', ANCHOR_TOLERANCE)
    limit = min(end - MIN_BLOCK, start + window)
    moment = None
    for second in range(int(start), int(limit) + 1):
        if states.get(second) == 'absent':
            moment = float(second)
            break
    if moment is None or moment - start < 1.0:
        return block
    moved = _snap_to_edge(moment, edges, tolerance)
    length = end - moved
    units = round(length / UNIT)
    if not (MIN_BLOCK <= length <= _option(options, 'scenario_max_block', MAX_BLOCK)):
        return block
    if units < 1 or abs(length - units * UNIT) > TOLERANCE:
        return block
    return dict(block, start=moved, anchored=round(moved - start, 3))


def decide(video, audio, duration, options=None, logo=None):
    """Returns the chosen blocks for one recording.

    ``logo`` is None or the evidence ``cm_rules.scenario_logo()`` prepared: a
    dict with ``states`` (second -> present/absent/unknown) and ``anchor``.
    """
    runs = silences(audio, options)
    silence_marks = sorted((run['start'] + run['end']) / 2.0 for run in runs)
    # Silence boundaries snapped to the picture cut, used to place blocks.
    edges = sorted(snap(run, video, options) for run in runs)
    gate = _option(options, 'scenario_min_delta_db', 1.0)
    min_units = int(_option(options, 'scenario_min_units', MIN_UNITS))
    exact = _option(options, 'scenario_exact_tolerance', EXACT_TOLERANCE)
    # A spot chain can skip a boundary: the silences at 15 s spacing do not
    # always survive transcoding, and requiring every single unit stopped the
    # block early (a 4-minute commercial was cut to 2 minutes 45). Letting the
    # chain jump two units recovered that block in testing without adding a
    # false cut; allowing three or four did add one, so the default stays at
    # two. scenario_spot_steps keeps the wider search for experiments.
    steps = (1, 2, 3, 4) if _option(options, 'scenario_spot_steps', False) else (1, 2)
    video_track = Track(video, 'scene')
    audio_track = Track(audio, 'rms_db')

    start_guard = _option(options, 'scenario_start_guard_seconds', START_GUARD)
    candidates = []
    for start in edges:
        if start > duration - MIN_BLOCK:
            continue
        if start < start_guard:
            continue
        end = start
        units = 0
        while True:
            best = None
            for step in steps:
                target = end + step * UNIT
                if target - start > MAX_BLOCK:
                    break
                index = bisect.bisect_left(edges, target - TOLERANCE)
                while index < len(edges) and edges[index] <= target + TOLERANCE:
                    if edges[index] > end:
                        offset = abs(edges[index] - target)
                        if best is None or offset < abs(best[1] - target):
                            best = (step, edges[index])
                    index += 1
            if best is None:
                break
            step, end = best
            units += step
            if end - start < MIN_BLOCK:
                continue
            # A two spot block is only a break when it is exactly two spots.
            # Anything this short that is merely near a multiple is the shape a
            # programme makes when a few silences happen to line up.
            if units < min_units and abs((end - start) - units * UNIT) > exact:
                continue
            if not mute_density_ok(start, end, units, silence_marks, options):
                continue
            score = block_score((start, end), silence_marks, logo, duration, options)
            if score is None:
                continue
            loudness = block_loudness(audio_track, start, end)
            if loudness is not None and loudness < gate:
                continue
            # Positions stay unrounded: rounding to milliseconds moved a cut
            # count at the block edge and changed whether expansion continued.
            candidates.append(dict(start=start, end=end,
                                   score=round(score, 3), units=units, delta_db=loudness))

    # Keep one hypothesis per start: a sub-block never wins merely because it
    # leaves more room.
    best_by_start = {}
    for item in candidates:
        key = round(item['start'], 1)
        kept = best_by_start.get(key)
        if kept is None or (item['score'], item['end']) > (kept['score'], kept['end']):
            best_by_start[key] = item
    chosen = choose(list(best_by_start.values()), duration)
    # Expansion follows the snapped boundaries, not the silence middles:
    # the unit it adds must end on a picture cut like the core does.
    grown = []
    for item in chosen:
        bigger = expand(item, edges, video_track, duration, options)
        if bigger['end'] != item['end'] or bigger['start'] != item['start']:
            # The unit it added has to stand on its own: a block that was a
            # spot block before the expansion must still be one after it.
            rescored = block_score((bigger['start'], bigger['end']),
                                   silence_marks, logo, duration, options)
            if rescored is None or rescored < _option(options, 'scenario_min_score', MIN_SCORE):
                bigger = item
        grown.append(bigger)
    chosen = grown
    # The anchor runs last: it may only shrink a block, so an expansion that
    # was justified by the wider block is not invalidated.
    chosen = [anchor_start(item, logo, edges, options) for item in chosen]
    return dict(silences=len(runs), candidates=len(candidates),
                cuts=[[round(c['start'], 3), round(c['end'], 3)] for c in chosen],
                scores=[c['score'] for c in chosen],
                anchored=sum(1 for c in chosen if c.get('anchored')),
                logo=bool(logo))
