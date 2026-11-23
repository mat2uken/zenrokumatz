#!/usr/bin/env python3
"""Tests for the spot-chain search in cm_scenario."""

import unittest

import cm_scenario


def _video_marks(times):
    """Picture cuts at the given times, enough to snap silences onto them."""
    return [dict(pts=t, mean=100.0, scene=30.0, black_fraction=0.0, logo_edge=0.0) for t in times]


def _audio(times, duration, block=(0.0, 120.0), step=0.1,
           programme_db=-26.0, block_db=-18.0, silence_db=-60.0):
    """Programme around a louder block, with short silences at the boundaries."""
    rows = []
    t = 0.0
    while t < duration:
        if any(abs(t - s) < 0.2 for s in times):
            level = silence_db
        elif block[0] <= t < block[1]:
            level = block_db
        else:
            level = programme_db
        rows.append(dict(pts=t, samples=1024, rms_db=level))
        t += step
    return rows


def _mark_states(duration, absent=()):
    """A mark state per second; ``absent`` is a list of (start, end) ranges."""
    states = {}
    for second in range(duration):
        inside = any(start <= second < end for start, end in absent)
        states[second] = 'absent' if inside else 'present'
    return states


class SpotStepTest(unittest.TestCase):

    def test_two_unit_jump_is_allowed(self):
        """A missing boundary one unit along must not end the chain."""
        # Silences every 15 s except one at 45 s, so the chain has to jump
        # from 30 s straight to 60 s.
        boundaries = [0.0, 15.0, 30.0, 60.0, 75.0, 90.0]
        marks = boundaries + [400.0]
        audio = _audio(boundaries, 500.0, block=(0.0, 90.0))
        options = dict(cm_scenario.__dict__.get('DEFAULTS', {}))
        result = cm_scenario.decide(_video_marks(marks), audio, 500.0, options)
        spans = [tuple(span) for span in result['cuts']]
        self.assertTrue(spans, 'the chain should still produce a block')
        longest = max(b - a for a, b in spans)
        self.assertGreaterEqual(longest, 60.0,
                                'the block should cross the missing boundary')

    def test_single_unit_chain_still_works(self):
        """The ordinary case, every boundary present, is unchanged."""
        boundaries = [0.0, 15.0, 30.0, 45.0, 60.0, 90.0]
        marks = boundaries + [400.0]
        audio = _audio(boundaries, 500.0, block=(0.0, 90.0))
        options = dict(cm_scenario.__dict__.get('DEFAULTS', {}))
        result = cm_scenario.decide(_video_marks(marks), audio, 500.0, options)
        self.assertIn('cuts', result)


class TwoSpotTest(unittest.TestCase):
    """Two spots are a break only when the length is exactly right."""

    def test_exactly_two_spots_is_kept(self):
        boundaries = [100.0, 130.0]
        audio = _audio(boundaries, 900.0, block=(100.0, 130.0))
        result = cm_scenario.decide(_video_marks(boundaries), audio, 900.0)
        self.assertEqual(len(result['cuts']), 1)
        self.assertAlmostEqual(result['cuts'][0][0], 100.0, places=2)
        self.assertAlmostEqual(result['cuts'][0][1], 130.0, places=2)

    def test_a_near_miss_is_refused(self):
        """31.4 s is close enough for the chain, not close enough to cut."""
        boundaries = [100.0, 131.4]
        audio = _audio(boundaries, 900.0, block=(100.0, 131.4))
        result = cm_scenario.decide(_video_marks(boundaries), audio, 900.0)
        self.assertEqual(result['cuts'], [])

    def test_a_three_spot_block_keeps_its_tolerance(self):
        """The exact-length rule applies to two spots only."""
        boundaries = [100.0, 115.0, 130.0, 146.4]
        audio = _audio(boundaries, 900.0, block=(100.0, 146.4))
        result = cm_scenario.decide(_video_marks(boundaries), audio, 900.0)
        self.assertEqual(len(result['cuts']), 1)


class MuteDensityTest(unittest.TestCase):
    """Silences that no spot boundary explains mean this is not a break."""

    def test_off_the_grid_cancels_the_block(self):
        marks = [60.0 + 7.0 * step for step in range(1, 13)]
        self.assertFalse(cm_scenario.mute_density_ok(60.0, 150.0, 6, marks))

    def test_on_the_grid_is_accepted(self):
        marks = [60.0 + 15.0 * step for step in range(1, 6)]
        self.assertTrue(cm_scenario.mute_density_ok(60.0, 150.0, 6, marks))

    def test_a_short_block_tolerates_a_few(self):
        self.assertTrue(cm_scenario.mute_density_ok(0.0, 45.0, 3, [20.0]))


class MarkStartTest(unittest.TestCase):
    """A start inside the mark is programme; move it to the mark's absence."""

    def _boundaries(self):
        return [270.0, 300.0, 315.0, 330.0, 345.0, 360.0,
                375.0, 390.0, 405.0, 420.0]

    def test_a_start_inside_the_mark_moves_to_its_disappearance(self):
        boundaries = self._boundaries()
        audio = _audio(boundaries, 900.0, block=(300.0, 420.0))
        marks = boundaries + [700.0]
        logo = dict(states=_mark_states(900, [(300, 420)]), anchor=True)
        result = cm_scenario.decide(_video_marks(marks), audio, 900.0, None, logo)
        self.assertEqual(result['cuts'], [[300.0, 420.0]])
        self.assertEqual(result['anchored'], 1)

    def test_a_start_without_the_mark_is_left_alone(self):
        """Trailers and spots carry no mark; cutting them is the policy."""
        boundaries = self._boundaries()
        audio = _audio(boundaries, 900.0, block=(270.0, 420.0))
        marks = boundaries + [700.0]
        logo = dict(states=_mark_states(900, [(270, 420)]), anchor=True)
        result = cm_scenario.decide(_video_marks(marks), audio, 900.0, None, logo)
        self.assertEqual(result['cuts'], [[270.0, 420.0]])
        self.assertEqual(result['anchored'], 0)

    def test_no_mark_means_no_anchor(self):
        boundaries = self._boundaries()
        audio = _audio(boundaries, 900.0, block=(270.0, 420.0))
        marks = boundaries + [700.0]
        result = cm_scenario.decide(_video_marks(marks), audio, 900.0)
        self.assertEqual(result['cuts'], [[270.0, 420.0]])

    def test_a_dead_mark_adds_no_bonus(self):
        """Unknown states are not evidence, so the score stays as without one."""
        silence = [0.0, 15.0, 30.0]
        without = cm_scenario.block_score((0.0, 30.0), silence, None, 900.0)
        dead = cm_scenario.block_score(
            (0.0, 30.0), silence,
            dict(states={second: 'unknown' for second in range(900)}), 900.0)
        self.assertEqual(without, dead)

    def test_a_block_under_the_mark_is_refused(self):
        """A spot block does not carry the station mark, whatever its shape."""
        silence = [0.0, 15.0, 30.0]
        live = dict(states={second: 'present' for second in range(900)})
        self.assertIsNone(cm_scenario.block_score((0.0, 30.0), silence, live, 900.0))

    def test_a_mark_on_part_of_the_block_only_lowers_the_score(self):
        """Mostly covered is still scored, not refused."""
        silence = [0.0, 15.0, 30.0]
        mostly = {second: ('present' if second < 20 else 'absent') for second in range(900)}
        without = cm_scenario.block_score((0.0, 30.0), silence, None, 900.0)
        covered = cm_scenario.block_score((0.0, 30.0), silence, dict(states=mostly), 900.0)
        self.assertIsNotNone(covered)
        self.assertLess(covered, without)


class StartGuardTest(unittest.TestCase):
    """A block starting at the first frame cannot be verified."""

    def test_a_block_at_the_recording_start_is_not_cut(self):
        boundaries = [1.3, 31.4]
        audio = _audio(boundaries, 900.0, block=(1.3, 31.4))
        result = cm_scenario.decide(_video_marks(boundaries), audio, 900.0)
        self.assertEqual(result['cuts'], [])

    def test_a_block_a_few_seconds_in_is_still_unverifiable(self):
        """An 8 s start means the recording began inside the block."""
        boundaries = [8.1, 98.1]
        audio = _audio(boundaries, 900.0, block=(8.1, 98.1))
        result = cm_scenario.decide(_video_marks(boundaries), audio, 900.0)
        self.assertEqual(result['cuts'], [])

    def test_a_block_well_inside_the_recording_is_kept(self):
        boundaries = [100.0 + 15.0 * step for step in range(7)]
        audio = _audio(boundaries, 900.0, block=(100.0, 190.0))
        result = cm_scenario.decide(_video_marks(boundaries), audio, 900.0)
        self.assertEqual(len(result['cuts']), 1)
        self.assertAlmostEqual(result['cuts'][0][0], 100.0, places=2)


if __name__ == '__main__':
    unittest.main()
