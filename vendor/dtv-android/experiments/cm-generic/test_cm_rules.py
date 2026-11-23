#!/usr/bin/env python3
"""Tests for the commercial inference rules. No media or network access."""
import unittest

import cm_rules


def bins(values, samples=30, start=0):
    return [dict(bin_start=row + start, samples=samples, correlation=correlation,
                 peak_margin=correlation * 0.8, gradient_ratio=1.0)
            for row, correlation in enumerate(values)]


def features(duration, chapters_at, scene=20.0, silent_around=None, step=1 / 30.0):
    """Builds video/audio rows with scene changes at the requested seconds."""
    video, audio = [], []
    silent = set()
    for entry in silent_around or chapters_at:
        for offset in range(-3, 4):
            silent.add(round(entry + offset * 0.1, 2))
    count = int(duration / step)
    for index in range(count):
        pts = index * step
        cut = any(abs(pts - point) < step for point in chapters_at)
        video.append(dict(pts=pts, mean=100.0, black_fraction=0.0, scene=scene if cut else 1.0, logo_edge=1.0))
    for index in range(int(duration * 20)):
        pts = index / 20.0
        quiet = any(abs(pts - point) < 0.35 for point in (silent_around or chapters_at))
        audio.append(dict(pts=pts, samples=2400, rms_db=-60.0 if quiet else -20.0))
    return video, audio


class ClassifyBinsTest(unittest.TestCase):
    def test_separates_two_clusters(self):
        values = [0.85] * 70 + [0.02] * 30
        states, clusters = cm_rules.classify_bins(bins(values), cm_rules.settings())
        self.assertEqual(states[0], 'present')
        self.assertEqual(states[80], 'absent')
        self.assertGreater(clusters['gap'], 0.5)

    def test_rejects_flat_series(self):
        states, clusters = cm_rules.classify_bins(bins([0.5] * 60), cm_rules.settings())
        self.assertEqual(states, {})
        self.assertIsNone(clusters)

    def test_sparse_bins_are_unknown(self):
        rows = bins([0.9] * 40 + [0.05] * 20)
        rows[3]['samples'] = 2
        states, _ = cm_rules.classify_bins(rows, cm_rules.settings())
        self.assertEqual(states[3], 'unknown')


class RunTest(unittest.TestCase):
    def test_bridges_unknown_but_not_present(self):
        states = {0: 'absent', 1: 'unknown', 2: 'absent', 3: 'present', 4: 'absent'}
        runs = cm_rules.bin_runs(states, 'absent', 1.0, 2.0)
        self.assertEqual(runs, [(0.0, 3.0), (4.0, 5.0)])

    def test_bridge_limit(self):
        states = {0: 'absent', 5: 'absent'}
        self.assertEqual(cm_rules.bin_runs(states, 'absent', 1.0, 2.0), [(0.0, 1.0), (5.0, 6.0)])


class StructureTest(unittest.TestCase):
    def test_accepts_one_minute_block(self):
        options = cm_rules.settings()
        kept, rejected = cm_rules.structure_breaks([(100.0, 160.0), (160.0, 220.0)], 900.0, options)
        self.assertEqual([item['interval'] for item in kept], [[100.0, 160.0], [160.0, 220.0]])
        self.assertEqual(rejected, [])

    def test_rejects_short_sponsor(self):
        kept, rejected = cm_rules.structure_breaks([(100.0, 105.0)], 900.0, cm_rules.settings())
        self.assertEqual(kept, [])
        self.assertEqual(rejected[0]['reason'], 'too_short')

    def test_keeps_end_card(self):
        options = cm_rules.settings(end_guard_seconds=90.0, end_card_seconds=12.0)
        kept, rejected = cm_rules.structure_breaks([(880.0, 890.0)], 900.0, options)
        self.assertEqual(kept, [])
        self.assertEqual(rejected[0]['reason'], 'end_card')

    def test_rejects_odd_length_below_long_block(self):
        kept, rejected = cm_rules.structure_breaks([(100.0, 118.0)], 900.0, cm_rules.settings())
        self.assertEqual(kept, [])
        self.assertEqual(rejected[0]['reason'], 'unusual_length')


class SpotChainTest(unittest.TestCase):
    """An over-extended block still holds the spots it contains."""

    def test_walks_back_from_the_mark_returning(self):
        options = cm_rules.settings(trim_spot_blocks=True)
        chapters = [790.0, 832.1, 847.1, 862.1, 877.1]
        self.assertEqual(cm_rules.spot_chain(790.0, 877.1, chapters, 1014.6, options),
                         (832.1, 877.1))

    def test_takes_the_longer_side(self):
        options = cm_rules.settings(trim_spot_blocks=True)
        chapters = [100.0, 115.0, 130.0, 160.0]
        self.assertEqual(cm_rules.spot_chain(100.0, 160.0, chapters, 600.0, options),
                         (100.0, 130.0))

    def test_no_chain_without_spot_boundaries(self):
        options = cm_rules.settings(trim_spot_blocks=True)
        self.assertIsNone(cm_rules.spot_chain(100.0, 160.0, [100.0, 160.0], 600.0, options))

    def test_recover_leaves_other_refusals_alone(self):
        options = cm_rules.settings(trim_spot_blocks=True)
        rejected = [dict(interval=[100.0, 160.0], reason='too_short'),
                    dict(interval=[300.0, 387.0], reason='short_break_not_a_spot_multiple')]
        chapters = [300.0, 315.0, 330.0, 345.0, 387.0]
        recovered, kept = cm_rules.recover_spot_blocks(rejected, chapters, 600.0, options)
        self.assertEqual([item['interval'] for item in kept], [[100.0, 160.0]])
        self.assertEqual([item['interval'] for item in recovered], [(300.0, 345.0)])


class SnapTest(unittest.TestCase):
    def test_moves_edges_to_chapter_points(self):
        self.assertEqual(cm_rules.snap((10.4, 20.6), [10.0, 21.0], 2.0, 0.0, 100.0), (10.0, 21.0))

    def test_keeps_edge_without_candidate(self):
        self.assertEqual(cm_rules.snap((10.4, 20.6), [50.0], 2.0, 0.0, 100.0), (10.4, 20.6))


class AnalyzeTest(unittest.TestCase):
    def build(self, journey):
        overlay = bins([journey[index] for index in range(300)])
        video, audio = features(300.0, [150.0, 210.0])
        return video, audio, [overlay]

    def test_cuts_measured_break(self):
        journey = [0.86] * 150 + [0.02] * 60 + [0.86] * 90
        video, audio, overlays = self.build(journey)
        result = cm_rules.analyze(video, audio, overlays, 300.0)
        self.assertEqual(result['mode'], 'overlay')
        self.assertEqual(len(result['cuts']), 1)
        start, end = result['cuts'][0]
        self.assertLess(abs(start - 150.0), 3.0)
        self.assertLess(abs(end - 210.0), 3.0)
        self.assertGreater(result['confidence'], 0.5)

    def test_no_cut_when_mark_always_present(self):
        video, audio, overlays = self.build([0.86] * 300)
        result = cm_rules.analyze(video, audio, overlays, 300.0)
        self.assertEqual(result['cuts'], [])
        self.assertIn(result['reason'], ('marker_not_present_enough', 'no_usable_marker'))

    def test_no_cut_when_mostly_absent(self):
        video, audio, overlays = self.build([0.02] * 300)
        result = cm_rules.analyze(video, audio, overlays, 300.0)
        self.assertEqual(result['cuts'], [])

    def test_ignores_tiny_breaks(self):
        journey = [0.86] * 150 + [0.02] * 4 + [0.86] * 146
        video, audio, overlays = self.build(journey)
        result = cm_rules.analyze(video, audio, overlays, 300.0)
        self.assertEqual(result['cuts'], [])

    def test_refuses_when_break_covers_most_of_recording(self):
        journey = [0.86] * 20 + [0.02] * 280
        video, audio, overlays = self.build(journey)
        result = cm_rules.analyze(video, audio, overlays, 300.0)
        self.assertEqual(result['cuts'], [])
        self.assertIn(result['reason'], ('marker_not_present_enough', 'too_much_removed', 'no_usable_marker'))

    def test_picks_the_marker_that_works(self):
        good = bins([0.86] * 150 + [0.02] * 60 + [0.86] * 90)
        flat = bins([0.5] * 300)
        video, audio = features(300.0, [150.0, 210.0])
        result = cm_rules.analyze(video, audio, [flat, good], 300.0)
        self.assertEqual(result['mode'], 'overlay')
        self.assertEqual(result['marker_index'], 1)

    def _spot_features(self, duration, block, louder=True):
        """Silences every 15 s across a block, programme elsewhere."""
        import math
        lo, hi = block
        video, audio = [], []
        step = 1 / 30.0
        for index in range(int(duration / step)):
            pts = index * step
            video.append(dict(pts=pts, mean=100.0, black_fraction=0.0, scene=1.0, logo_edge=1.0))
        quiet = [lo + 15.0 * k for k in range(int(round((hi - lo) / 15.0)) + 1)]
        for index in range(int(duration * 20)):
            pts = index / 20.0
            is_quiet = any(abs(pts - t) < 0.35 for t in quiet)
            inside = lo <= pts < hi
            level = -20.0
            if is_quiet:
                level = -120.0
            elif inside and louder:
                level = -18.0
            audio.append(dict(pts=pts, samples=2400, rms_db=level))
        return video, audio

    def test_scenario_cuts_a_spot_chain(self):
        video, audio = self._spot_features(900.0, (300.0, 420.0))
        result = cm_rules.analyze(video, audio, [], 900.0, allow_timing=True)
        self.assertEqual(result['mode'], 'scenario')
        self.assertEqual(len(result['cuts']), 1)
        start, end = result['cuts'][0]
        self.assertLess(abs(start - 300.0), 2.0)
        self.assertLess(abs(end - 420.0), 2.0)

    def test_scenario_expands_over_a_dense_spot_unit(self):
        video, audio = self._spot_features(900.0, (300.0, 420.0))
        # The unit before the core is cut like a commercial (8 cuts), so it is
        # part of the same break and must be removed as well.
        for row in video:
            if 285.0 <= row['pts'] < 300.0:
                row['scene'] = 60.0 if int(row['pts'] * 2) % 2 else 1.0
        for row in audio:
            if abs(row['pts'] - 285.0) < 0.35:
                row['rms_db'] = -120.0
        result = cm_rules.analyze(video, audio, [], 900.0, allow_timing=True)
        self.assertEqual(len(result['cuts']), 1)
        self.assertLess(abs(result['cuts'][0][0] - 285.0), 2.0)

    def test_scenario_does_not_expand_into_calm_programme(self):
        video, audio = self._spot_features(900.0, (300.0, 420.0))
        for row in video:
            if 285.0 <= row['pts'] < 300.0:
                row['scene'] = 1.0
        result = cm_rules.analyze(video, audio, [], 900.0, allow_timing=True)
        self.assertEqual(len(result['cuts']), 1)
        self.assertLess(abs(result['cuts'][0][0] - 300.0), 2.0)

    def test_scenario_needs_the_silence_cadence(self):
        video, audio = self._spot_features(900.0, (300.0, 420.0))
        for row in audio:
            if row['rms_db'] < -100:
                row['rms_db'] = -20.0
        result = cm_rules.analyze(video, audio, [], 900.0, allow_timing=True)
        self.assertEqual(result['cuts'], [])

    def test_scenario_needs_a_louder_block(self):
        video, audio = self._spot_features(900.0, (300.0, 420.0), louder=False)
        result = cm_rules.analyze(video, audio, [], 900.0, allow_timing=True)
        self.assertEqual(result['cuts'], [])

    def test_scenario_respects_gaps(self):
        video, audio = self._spot_features(900.0, (300.0, 420.0))
        result = cm_rules.analyze(video, audio, [], 900.0, allow_timing=True,
                                  excluded=[(350.0, 360.0)])
        self.assertEqual(result['cuts'], [])

    def _scene_cuts(self, video, boundaries):
        for row in video:
            if any(abs(row['pts'] - point) < 0.02 for point in boundaries):
                row['scene'] = 60.0

    def test_corroborated_block_is_cut_at_the_spots(self):
        """An over-extended no-mark stretch is cut where the timing path agrees."""
        video, audio = self._spot_features(900.0, (300.0, 420.0))
        self._scene_cuts(video, [330.0, 345.0, 360.0, 375.0, 390.0, 405.0, 420.0])
        overlay = bins([0.86] * 333 + [0.02] * 87 + [0.86] * 480)
        options = cm_rules.settings(trim_spot_blocks=True)
        agreed = cm_rules.corroborated_blocks(video, audio, [overlay], 900.0, options,
                                              [(300.0, 420.0)], ())
        self.assertEqual(len(agreed), 1)
        (start, end), score = agreed[0]
        self.assertLess(abs(start - 345.0), 3.0)
        self.assertLess(abs(end - 420.0), 3.0)
        self.assertGreater(score, 0.5)

    def test_block_without_agreement_is_dropped(self):
        video, audio = self._spot_features(900.0, (300.0, 420.0))
        self._scene_cuts(video, [330.0, 345.0, 360.0, 375.0, 390.0, 405.0, 420.0])
        overlay = bins([0.86] * 333 + [0.02] * 87 + [0.86] * 480)
        options = cm_rules.settings(trim_spot_blocks=True)
        self.assertEqual(cm_rules.corroborated_blocks(video, audio, [overlay], 900.0, options,
                                                      [(700.0, 800.0)], ()), [])

    def test_the_same_span_from_two_candidates_is_returned_once(self):
        """Duplicates reached the decision and inflated cut_ratio threefold."""
        video, audio = self._spot_features(900.0, (300.0, 420.0))
        self._scene_cuts(video, [330.0, 345.0, 360.0, 375.0, 390.0, 405.0, 420.0])
        overlay = bins([0.86] * 333 + [0.02] * 87 + [0.86] * 480)
        options = cm_rules.settings(trim_spot_blocks=True)
        agreed = cm_rules.corroborated_blocks(video, audio, [overlay, overlay], 900.0, options,
                                              [(300.0, 420.0)], ())
        self.assertEqual(len(agreed), 1)

    def test_trim_never_decides_without_the_timing_path(self):
        video, audio = self._spot_features(900.0, (300.0, 420.0))
        self._scene_cuts(video, [330.0, 345.0, 360.0, 375.0, 390.0, 405.0, 420.0])
        overlay = bins([0.86] * 333 + [0.02] * 87 + [0.86] * 480)
        options = cm_rules.settings(trim_spot_blocks=True)
        result = cm_rules.analyze(video, audio, [overlay], 900.0, options, allow_timing=False)
        self.assertEqual(result['cuts'], [])

    def test_coverage_floor_accepts_partial_mark(self):
        options = dict(cm_rules.settings())
        self.assertLessEqual(options['min_coverage'], 0.80)

    def test_timing_mode_is_opt_in(self):
        video, audio = features(300.0, [100.0, 130.0, 160.0])
        off = cm_rules.analyze(video, audio, [], 300.0)
        self.assertEqual(off['cuts'], [])
        self.assertEqual(off['mode'], 'none')


class ScenarioLogoTest(unittest.TestCase):
    """What the timing path is told about the measured mark."""

    def test_a_dead_mark_is_not_offered(self):
        self.assertIsNone(cm_rules.scenario_logo([bins([0.5] * 300)],
                                                 cm_rules.settings()))

    def test_a_live_mark_is_chosen_over_a_dead_one(self):
        dead = bins([0.5] * 300)
        live = bins([0.86] * 240 + [0.02] * 60)
        logo = cm_rules.scenario_logo([dead, live], cm_rules.settings())
        self.assertEqual(logo['index'], 1)
        self.assertTrue(logo['usable'])
        self.assertTrue(logo['anchor'])

    def test_a_briefly_seen_mark_scores_but_does_not_anchor(self):
        """25 present seconds is evidence; it is not proof the mark is absent."""
        live = bins([0.86] * 25 + [0.02] * 575)
        logo = cm_rules.scenario_logo([live], cm_rules.settings())
        self.assertTrue(logo['usable'])
        self.assertFalse(logo['anchor'])

    def test_analyze_reports_the_mark_it_gave_the_timing_path(self):
        video, audio = features(900.0, [700.0])
        overlay = bins([0.86] * 500 + [0.02] * 120 + [0.86] * 280)
        result = cm_rules.analyze(video, audio, [overlay], 900.0, allow_timing=True)
        self.assertEqual(result['mode'], 'scenario')
        self.assertEqual(result['logo_index'], 0)
        self.assertTrue(result['logo_anchor'])

    def test_analyze_reports_no_mark_when_none_survives(self):
        video, audio = features(900.0, [700.0])
        result = cm_rules.analyze(video, audio, [bins([0.5] * 900)], 900.0,
                                  allow_timing=True)
        self.assertIsNone(result['logo_index'])


if __name__ == '__main__':
    unittest.main()
