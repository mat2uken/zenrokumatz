#!/usr/bin/env python3
"""Tests for the remux validation used before an original MP4 is saved.

The accepted values are measurements from real recordings. The MP4 muxer folds
the B-frame reorder delay into the container duration, so the same content
reports a few frames more than the source fMP4; the old 0.05 s duration check
rejected those faithful copies and the recording was thrown away.
"""
import json
import struct
import tempfile
import unittest
from pathlib import Path

import cm_features
import cm_service


def report(**overrides):
    """cm-cut report of the BS11 AVC recording that failed on 2026-11-05."""
    base = dict(video_frames=219600, audio_packets=171745, width=1280, height=736,
                video_codec='h264', duration_seconds=3663.66, span_seconds=3664.00388)
    base.update(overrides)
    return base


def pgm(width, height, fill=0):
    """A binary PGM of exactly this size, as the overlay learner writes them."""
    header = f'P5\n{width} {height}\n255\n'.encode('ascii')
    return header + bytes([fill]) * (width * height)


class ValidateRemuxTest(unittest.TestCase):

    def test_accepts_measured_avc_remeasure(self):
        # 0.083 s longer duration, identical span: a faithful copy.
        cm_service.validate_remux(report(), report(duration_seconds=3663.743317,
                                                   span_seconds=3664.003875))

    def test_accepts_measured_hevc_remeasure(self):
        # 102-minute HEVC programme, 0.311 s longer duration.
        source = report(video_frames=184490, audio_packets=288568, width=1440, height=1080,
                        video_codec='hevc', duration_seconds=6155.805992,
                        span_seconds=6155.805992)
        cm_service.validate_remux(source, dict(source, duration_seconds=6156.116466))

    def test_rejects_lost_video_frame(self):
        with self.assertRaisesRegex(ValueError, 'remux mismatch: video_frames'):
            cm_service.validate_remux(report(), report(video_frames=219599))

    def test_rejects_lost_audio_packet(self):
        with self.assertRaisesRegex(ValueError, 'remux mismatch: audio_packets'):
            cm_service.validate_remux(report(), report(audio_packets=171744))

    def test_rejects_shifted_presentation_span(self):
        with self.assertRaisesRegex(ValueError, 'remux mismatch: span'):
            cm_service.validate_remux(report(), report(span_seconds=3666.00388))

    def test_rejects_gross_duration_error(self):
        with self.assertRaisesRegex(ValueError, 'remux mismatch: duration'):
            cm_service.validate_remux(report(), report(duration_seconds=3668.66))


class AvDurationTest(unittest.TestCase):
    """A capture that lost its audio tail must not be cut."""

    def test_accepts_normal_stream_durations(self):
        self.assertFalse(cm_service.av_duration_mismatch(7002.68, 7001.69))

    def test_rejects_audio_tail_loss(self):
        self.assertTrue(cm_service.av_duration_mismatch(971.47, 752.61))

    def test_rejects_short_audio_dropout(self):
        self.assertTrue(cm_service.av_duration_mismatch(347.93, 338.26))

    def test_missing_duration_is_not_a_mismatch(self):
        self.assertFalse(cm_service.av_duration_mismatch(0, 752.61))


class CutMinimumTest(unittest.TestCase):
    """News and shopping programmes need a higher short-break floor."""

    def test_news_title_gets_30s(self):
        self.assertEqual(cm_service.cut_minimum('034400_ＮＥＷＳ２４', {}), 30.0)

    def test_news_word_gets_30s(self):
        self.assertEqual(cm_service.cut_minimum('ニュース7', {}), 30.0)

    def test_shopping_title_gets_30s(self):
        self.assertEqual(cm_service.cut_minimum('022002_ディノスＴＨＥストア', {}), 30.0)

    def test_normal_programme_keeps_15s(self):
        self.assertEqual(cm_service.cut_minimum('アニメ第5話', {}), 15.0)


class ShortCutTest(unittest.TestCase):
    """A 12-second transition is not a commercial spot."""

    def test_rejects_12s_break(self):
        kept, rejected = cm_service.filter_short_cuts([(100.0, 112.0)])
        self.assertEqual(kept, [])
        self.assertEqual(rejected, [(100.0, 112.0)])

    def test_rejects_15s_news_break(self):
        kept, rejected = cm_service.filter_short_cuts(
            [(430.596, 446.0)], cm_service.cut_minimum('NEWS24', {}))
        self.assertEqual(kept, [])
        self.assertEqual(rejected, [(430.596, 446.0)])

    def test_keeps_15s_break(self):
        kept, rejected = cm_service.filter_short_cuts([(100.0, 115.0)])
        self.assertEqual(kept, [(100.0, 115.0)])
        self.assertEqual(rejected, [])

    def test_keeps_spot_snapped_a_fraction_short(self):
        """The three real spots the floor threw away on 2026-11-17..21."""
        for start, end in ((502.768933667, 517.750566667),   # 14.98 s, anime
                           (1264.5633, 1279.011066333),      # 14.45 s, drama
                           (2421.085333, 2435.099332)):      # 14.01 s, variety
            kept, rejected = cm_service.filter_short_cuts([(start, end)])
            self.assertEqual(kept, [(start, end)])
            self.assertEqual(rejected, [])

    def test_rejects_a_break_short_of_the_spot_tolerance(self):
        # 13.0 s is below 15 s minus the detector's 1.5 s spot tolerance.
        kept, rejected = cm_service.filter_short_cuts([(100.0, 113.0)])
        self.assertEqual(kept, [])
        self.assertEqual(rejected, [(100.0, 113.0)])

    def test_news_floor_keeps_a_snapped_two_spot_block(self):
        news = cm_service.cut_minimum('NEWS24', {})
        kept, _ = cm_service.filter_short_cuts([(400.0, 428.8)], news)
        self.assertEqual(kept, [(400.0, 428.8)])

    def test_news_floor_still_rejects_one_spot(self):
        news = cm_service.cut_minimum('ショップチャンネル', {})
        kept, rejected = cm_service.filter_short_cuts([(410.91, 426.259)], news)
        self.assertEqual(kept, [])
        self.assertEqual(rejected, [(410.91, 426.259)])

    def test_keeps_long_break(self):
        kept, rejected = cm_service.filter_short_cuts([(100.0, 396.0)])
        self.assertEqual(kept, [(100.0, 396.0)])
        self.assertEqual(rejected, [])

    def test_splits_mixed_breaks(self):
        kept, rejected = cm_service.filter_short_cuts(
            [(10.0, 22.0), (30.0, 45.0), (60.0, 360.0)])
        self.assertEqual(kept, [(30.0, 45.0), (60.0, 360.0)])
        self.assertEqual(rejected, [(10.0, 22.0)])


class FieldCodedTest(unittest.TestCase):
    """PAFF recordings carry two packets per decoded picture."""

    def test_detects_field_coded(self):
        self.assertTrue(cm_service.field_coded_stream(
            dict(video_frames=219600), dict(video_frames=109800)))

    def test_accepts_progressive(self):
        self.assertFalse(cm_service.field_coded_stream(
            dict(video_frames=54150), dict(video_frames=54150)))

    def test_missing_detection_is_not_field_coded(self):
        self.assertFalse(cm_service.field_coded_stream(dict(video_frames=100), {}))


class AudioWarningTest(unittest.TestCase):
    """AAC decoder limits must not discard a packet-faithful cut."""

    def validation(self, **overrides):
        base = dict(verified=True, full_decode_ok=False, video_hash_mismatches=0,
                    audio_hash_mismatches=0, video_max_pts_error_ms=0.0,
                    audio_max_pts_error_ms=0.01)
        base.update(overrides)
        return base

    def test_accepts_clean_packet_comparison_when_enabled(self):
        self.assertTrue(cm_service.audio_warning_is_acceptable(
            self.validation(), 'aac decode error', {'allow_audio_decode_warning': True}))

    def test_rejects_when_not_enabled(self):
        self.assertFalse(cm_service.audio_warning_is_acceptable(
            self.validation(), 'aac decode error', {}))

    def test_rejects_video_hash_mismatch(self):
        self.assertFalse(cm_service.audio_warning_is_acceptable(
            self.validation(video_hash_mismatches=1), 'aac decode error',
            {'allow_audio_decode_warning': True}))

    def test_rejects_audio_hash_mismatch(self):
        self.assertFalse(cm_service.audio_warning_is_acceptable(
            self.validation(audio_hash_mismatches=1), 'aac decode error',
            {'allow_audio_decode_warning': True}))

    def test_rejects_missing_cut_error(self):
        self.assertFalse(cm_service.audio_warning_is_acceptable(
            self.validation(), None, {'allow_audio_decode_warning': True}))

    def test_rejects_full_decode_success(self):
        self.assertFalse(cm_service.audio_warning_is_acceptable(
            self.validation(full_decode_ok=True), 'aac decode error',
            {'allow_audio_decode_warning': True}))


class SaveOriginalFileTest(unittest.TestCase):
    """The original must be published once, never overwritten or lost."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.stage = self.root / 'stage.mp4'
        self.stage.write_bytes(b'first')
        self.job = dict(original_mp4=str(self.root / 'library' / 'rec.mp4'),
                        original_json=str(self.root / 'library' / 'rec.json'))
        self.metadata = dict(recordingId='rid')
        self.result = {}

    def tearDown(self):
        self.temp.cleanup()

    def test_copies_once_and_writes_sidecar(self):
        cm_service.save_original_file(self.job, self.metadata, self.stage, self.result)
        self.assertEqual(Path(self.job['original_mp4']).read_bytes(), b'first')
        self.assertEqual(cm_service.load(self.job['original_json'])['recordingId'], 'rid')
        self.assertTrue(self.result['original_saved'])

    def test_does_not_overwrite_completed_original(self):
        cm_service.save_original_file(self.job, self.metadata, self.stage, {})
        self.stage.write_bytes(b'second')
        cm_service.save_original_file(self.job, self.metadata, self.stage, {})
        self.assertEqual(Path(self.job['original_mp4']).read_bytes(), b'first')

    def test_missing_target_with_sidecar_raises(self):
        cm_service.save(self.job['original_json'], {'recordingId': 'rid'})
        with self.assertRaisesRegex(ValueError, 'completed original is missing'):
            cm_service.save_original_file(self.job, self.metadata, self.stage, {})


if __name__ == '__main__':
    unittest.main()


class FeatureRowsTest(unittest.TestCase):
    """The optional capture sideband must never be trusted blindly."""

    def sidecar(self, records=2, dropped=0):
        header = struct.pack('<HHIIqqIIII', 1, 0, 5, records, 0, 100000, 1, 1, dropped, 0)
        video = struct.pack('<BBHqIiII', 1, 0, 0, 0, 0, 10000, 100, 200)
        audio = struct.pack('<BBHqIiII', 2, 0, 0, 0, 1024, -2000, 0, 0)
        handle = tempfile.NamedTemporaryFile(delete=False, suffix='.bin')
        handle.write(header + video + audio)
        handle.close()
        self.addCleanup(lambda: Path(handle.name).unlink(missing_ok=True))
        return handle.name

    def test_disabled_mode_ignores_the_sidecar(self):
        rows, reason = cm_service.feature_rows({'path': self.sidecar()}, 100.0, {})
        self.assertIsNone(rows)
        self.assertEqual(reason, 'disabled')

    def test_missing_metadata_is_not_an_error(self):
        rows, reason = cm_service.feature_rows(None, 100.0, {'feature_mode': 'apply'})
        self.assertIsNone(rows)
        self.assertEqual(reason, 'no_features')

    def test_missing_path_is_refused(self):
        rows, reason = cm_service.feature_rows({'schema': 1}, 100.0, {'feature_mode': 'apply'})
        self.assertIsNone(rows)
        self.assertEqual(reason, 'no_path')

    def test_unreadable_sidecar_returns_a_reason(self):
        rows, reason = cm_service.feature_rows({'path': '/nonexistent/feat.bin'}, 100.0,
                                               {'feature_mode': 'apply'})
        self.assertIsNone(rows)
        self.assertIn('cannot read', reason)

    def test_sidecar_with_dropped_records_is_refused(self):
        rows, reason = cm_service.feature_rows({'path': self.sidecar(dropped=2)}, 0.0,
                                               {'feature_mode': 'apply'})
        self.assertIsNone(rows)
        self.assertIn('dropped', reason)

    def test_valid_sidecar_returns_rows(self):
        rows, reason = cm_service.feature_rows({'path': self.sidecar()}, 0.0,
                                               {'feature_mode': 'apply'})
        self.assertEqual(reason, 'ok')
        self.assertEqual(len(rows[0]), 1)
        self.assertEqual(len(rows[1]), 1)
        self.assertAlmostEqual(rows[0][0]['mean'], 100.0)
        self.assertAlmostEqual(rows[1][0]['rms_db'], -20.0)


class ChapterAgreementTest(unittest.TestCase):
    """The sideband must not move the edges the rules cut on."""

    def _rows(self, points):
        return [dict(pts=p, mean=100.0, black_fraction=0.0, scene=20.0)
                for p in points]

    def test_same_edges_agree(self):
        video = self._rows([10.0, 30.0, 60.0])
        audio = [dict(pts=t, rms_db=-60.0, samples=1024) for t in (10.0, 30.0, 60.0)]
        result = cm_service.chapter_agreement(
            dict(video=video, audio=audio), (video, audio))
        self.assertEqual(result['nas'], result['capture'])
        self.assertEqual(result['matched'], result['nas'])

    def test_moved_edges_are_reported(self):
        side = self._rows([10.5, 30.5, 60.5])
        meas = self._rows([10.0, 30.0, 60.0])
        audio = [dict(pts=t, rms_db=-60.0, samples=1024) for t in (10.0, 30.0, 60.0)]
        result = cm_service.chapter_agreement(
            dict(video=side, audio=audio), (meas, audio))
        # 0.5 s is the tolerance, so these still pair.
        self.assertEqual(result['matched'], result['nas'])


class DecisionAgreementTest(unittest.TestCase):
    """Substituting the sideband must be judged by the cuts it produces."""

    def _config(self):
        return dict(timing_mode='detect')

    def test_no_breaks_on_both_sides_is_the_same(self):
        video = [dict(pts=p / 10.0, mean=100.0, black_fraction=0.0, scene=0.0)
                 for p in range(300)]
        audio = [dict(pts=p / 10.0, rms_db=-30.0, samples=1024) for p in range(300)]
        result = cm_service.decision_agreement(
            dict(video=video, audio=audio), (video, audio), [], 30.0, self._config())
        self.assertTrue(result['same'])
        self.assertEqual(result['nas_cuts'], result['capture_cuts'])

    def test_without_overlay_neither_side_cuts(self):
        # No marker measurement means no cuts on either side, so the
        # comparison cannot separate them. Recorded here so the limitation is
        # visible: the worker always has overlay rows.
        nas_video = [dict(pts=p / 10.0, mean=100.0, black_fraction=0.0, scene=0.0)
                     for p in range(300)]
        cap_video = [dict(row) for row in nas_video]
        for row in cap_video:
            if abs(row['pts'] - 15.0) < 0.01:
                row['scene'] = 30.0
        audio = [dict(pts=p / 10.0, rms_db=-70.0, samples=1024) for p in range(300)]
        result = cm_service.decision_agreement(
            dict(video=cap_video, audio=audio), (nas_video, audio), [], 30.0,
            self._config())
        self.assertEqual(result['nas_cuts'], 0)
        self.assertEqual(result['capture_cuts'], 0)
        self.assertTrue(result['same'])


class DecisionAgreementComparisonTest(unittest.TestCase):
    """The comparison itself, without depending on the rule thresholds."""

    def test_different_cut_lists_are_reported(self):
        calls = {'n': 0}

        def fake_analyze(video, audio, overlays, duration, options=None,
                         allow_timing=False, excluded=()):
            calls['n'] += 1
            # The first call uses the sideband rows, the second the measurement.
            if calls['n'] == 1:
                return dict(mode='overlay', cuts=[[10.0, 20.0]])
            return dict(mode='overlay', cuts=[])

        original = cm_service.cm_rules.analyze
        cm_service.cm_rules.analyze = fake_analyze
        try:
            result = cm_service.decision_agreement(
                dict(video=[], audio=[]), ([], []), [], 30.0, dict(timing_mode='detect'))
        finally:
            cm_service.cm_rules.analyze = original
        self.assertEqual(result['capture_cuts'], 1)
        self.assertEqual(result['nas_cuts'], 0)
        self.assertFalse(result['same'])

    def test_identical_cut_lists_are_the_same(self):
        def fake_analyze(video, audio, overlays, duration, options=None,
                         allow_timing=False, excluded=()):
            return dict(mode='overlay', cuts=[[10.0, 20.0]])

        original = cm_service.cm_rules.analyze
        cm_service.cm_rules.analyze = fake_analyze
        try:
            result = cm_service.decision_agreement(
                dict(video=[], audio=[]), ([], []), [], 30.0, dict(timing_mode='detect'))
        finally:
            cm_service.cm_rules.analyze = original
        self.assertTrue(result['same'])
        self.assertEqual(result['matched'], 1)


class BinSeparationTest(unittest.TestCase):
    """A marker must separate the two correlation clusters to be cached."""

    def _rows(self, values, samples=30):
        return [dict(bin_start=i, samples=samples, correlation=v,
                     peak_margin=v * 0.8, gradient_ratio=1.0)
                for i, v in enumerate(values)]

    def test_clear_split_is_reported(self):
        values = [0.05] * 20 + [0.85] * 20
        gap = cm_service.bin_separation(self._rows(values), cm_service.cm_rules.settings())
        self.assertGreater(gap, 0.5)

    def test_flat_values_are_rejected(self):
        values = [0.02 + 0.001 * (i % 5) for i in range(40)]
        gap = cm_service.bin_separation(self._rows(values), cm_service.cm_rules.settings())
        self.assertLess(gap, 0.25)

    def test_too_few_bins_is_unknown(self):
        self.assertIsNone(cm_service.bin_separation(self._rows([0.1, 0.9]), cm_service.cm_rules.settings()))


class MarkerFreeFallbackTest(unittest.TestCase):
    """When no marker can decide, the sideband alone may still decide."""

    def _video(self, cuts_at):
        rows = []
        for i in range(30 * 40):
            t = i / 30.0
            scene = 20.0 if any(abs(t - c) < 0.02 for c in cuts_at) else 0.0
            rows.append(dict(pts=t, mean=100.0, black_fraction=0.0, scene=scene))
        return rows

    def test_marker_free_decision_is_used_when_no_marker_fits(self):
        video = self._video([10.0, 25.0])
        audio = [dict(pts=i / 46.875, rms_db=-70.0, samples=1024) for i in range(int(40 * 46.875))]
        overlays = [cm_service.cm_rules.bins([0.1] * 40)] if hasattr(cm_service.cm_rules, 'bins') else []
        config = dict(timing_mode='detect', feature_allow_scenario=True)
        decision = cm_service.cm_rules.analyze(video, audio, overlays, 40.0,
                                                cm_service.cm_rules.settings(),
                                                allow_timing=True)
        # The plain rules path can decide without a marker; the worker only has
        # to accept that decision when the marker cannot.
        self.assertIn(decision.get('mode'), ('scenario', 'none', 'overlay'))


class CutsWithinTest(unittest.TestCase):
    """A decision must lie inside the recording it claims to describe."""

    def test_normal_cuts_are_accepted(self):
        self.assertTrue(cm_service.cuts_within([(10.0, 40.0), (100.0, 130.0)], 600.0))

    def test_no_cuts_is_accepted(self):
        self.assertTrue(cm_service.cuts_within([], 600.0))

    def test_negative_cut_is_rejected(self):
        self.assertFalse(cm_service.cuts_within([(-8065.2, -8005.3)], 1063.0))

    def test_cut_past_the_end_is_rejected(self):
        self.assertFalse(cm_service.cuts_within([(900.0, 1200.0)], 1063.0))

    def test_reversed_cut_is_rejected(self):
        self.assertFalse(cm_service.cuts_within([(40.0, 10.0)], 600.0))

    def test_zero_duration_is_rejected(self):
        self.assertFalse(cm_service.cuts_within([(1.0, 2.0)], 0.0))


class MarkerFreeAllowedTest(unittest.TestCase):
    """The marker-free path must stay limited to services verified to agree."""

    def test_allowlist_is_read_from_the_config(self):
        config = dict(feature_marker_free_services=[35864])
        allowed = config.get('feature_marker_free_services') or []
        self.assertIn(35864, allowed)
        self.assertNotIn(35840, allowed)

    def test_empty_allowlist_disables_the_path(self):
        config = {}
        allowed = config.get('feature_marker_free_services') or []
        self.assertEqual(allowed, [])


class MarkerSurvivedTest(unittest.TestCase):
    """An ad-free programme must not demote a working marker."""

    def test_a_cut_survives(self):
        self.assertTrue(cm_service.marker_survived(dict(mode='overlay', candidates=[])))

    def test_a_measurable_no_break_survives(self):
        decision = dict(mode='scenario', candidates=[dict(index=0, reason='no_breaks')])
        self.assertTrue(cm_service.marker_survived(decision))

    def test_a_mark_that_could_not_be_measured_fails(self):
        decision = dict(mode='scenario',
                        candidates=[dict(index=0, reason='marker_not_present_enough')])
        self.assertFalse(cm_service.marker_survived(decision))

    def test_unclassifiable_bins_fail(self):
        decision = dict(mode='none', candidates=[dict(index=0, reason='not_enough_bins')])
        self.assertFalse(cm_service.marker_survived(decision))


class MarkerTrustTest(unittest.TestCase):
    """A learned marker may not poison a cache that still works."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.config = {'marker_dir': self.temp.name}
        self.metadata = {'service': {'id': 3219535864, 'serviceId': 35864,
                                     'name': 'テレビ静岡'}}
        self.info = {'width': 1440, 'height': 1080}
        self.features = Path(self.temp.name) / 'features'
        self.features.mkdir()

    def candidate(self, roi, fill=0):
        """A learned candidate whose crops are exactly the roi's size.

        cm-scan reads a reference directly only at the roi's size, so a crop of
        any other size is a broken pair, not a small one.
        """
        names = []
        for index in range(2):
            name = f'ref-{roi[2]}x{roi[3]}-{index}.pgm'
            (self.features / name).write_bytes(pgm(roi[2], roi[3], fill))
            names.append(name)
        return dict(roi=roi, references=names, band='top')

    def corrupt(self, roi, alt_roi=None):
        """Writes the state the pipeline was found in on 2026-11-22.

        The marker names the crops that belong to an older alternative while
        its roi comes from a later candidate: the sizes disagree, and cm-scan
        aborted every recording on the service before a single rule ran.
        """
        marker, refs_dir = cm_service.marker_files(self.config, self.metadata, self.info)
        refs_dir.mkdir(parents=True, exist_ok=True)
        size = (alt_roi[2], alt_roi[3]) if alt_roi else (roi[2] // 2, roi[3] // 2)
        names = []
        for index in range(2):
            name = f'alt-{index}.pgm'
            (refs_dir / name).write_bytes(pgm(size[0], size[1]))
            names.append(name)
        cm_service.save(marker, dict(roi=roi, references=names, band='top', size=[1440, 1080]))
        alt = None
        if alt_roi is not None:
            for index in range(2):
                (refs_dir / f'alt-{index}.pgm').write_bytes(pgm(alt_roi[2], alt_roi[3]))
            alt = dict(roi=alt_roi, references=names, band='top')
        cm_service.save_marker_stats(self.config, self.metadata, self.info,
                                     dict(version=1, current_roi=roi, confirms=0, fails=0, alt=alt))
        return marker, refs_dir

    def test_save_on_empty_creates_cache_and_stats(self):
        cm_service.save_marker(self.config, self.metadata, self.info,
                               self.candidate([32, 48, 48, 48]), self.features)
        marker, _ = cm_service.marker_files(self.config, self.metadata, self.info)
        self.assertTrue(marker.exists())
        stats = cm_service.load_marker_stats(self.config, self.metadata, self.info)
        self.assertEqual(stats['current_roi'], [32, 48, 48, 48])
        self.assertEqual(stats['fails'], 0)

    def test_confirm_resets_fails(self):
        cm_service.save_marker(self.config, self.metadata, self.info,
                               self.candidate([32, 48, 48, 48]), self.features)
        cm_service.fail_marker(self.config, self.metadata, self.info)
        cm_service.confirm_marker(self.config, self.metadata, self.info)
        stats = cm_service.load_marker_stats(self.config, self.metadata, self.info)
        self.assertEqual(stats['confirms'], 1)
        self.assertEqual(stats['fails'], 0)

    def test_working_cache_keeps_new_marker_as_alternative(self):
        cm_service.save_marker(self.config, self.metadata, self.info,
                               self.candidate([32, 48, 48, 48]), self.features)
        cm_service.confirm_marker(self.config, self.metadata, self.info)
        cm_service.save_marker(self.config, self.metadata, self.info,
                               self.candidate([1152, 48, 192, 80]), self.features)
        marker, _ = cm_service.marker_files(self.config, self.metadata, self.info)
        self.assertEqual(json.loads(marker.read_text())['roi'], [32, 48, 48, 48])
        stats = cm_service.load_marker_stats(self.config, self.metadata, self.info)
        self.assertEqual(stats['alt']['roi'], [1152, 48, 192, 80])
        alt_refs = stats['alt']['references']
        _, refs_dir = cm_service.marker_files(self.config, self.metadata, self.info)
        for name in alt_refs:
            self.assertTrue((refs_dir / name).exists())

    def test_two_failures_promote_alternative(self):
        cm_service.save_marker(self.config, self.metadata, self.info,
                               self.candidate([32, 48, 48, 48]), self.features)
        cm_service.save_marker(self.config, self.metadata, self.info,
                               self.candidate([1152, 48, 192, 80]), self.features)
        cm_service.fail_marker(self.config, self.metadata, self.info)
        promoted = cm_service.fail_marker(self.config, self.metadata, self.info)
        self.assertTrue(promoted)
        marker, _ = cm_service.marker_files(self.config, self.metadata, self.info)
        self.assertEqual(json.loads(marker.read_text())['roi'], [1152, 48, 192, 80])
        stats = cm_service.load_marker_stats(self.config, self.metadata, self.info)
        self.assertEqual(stats['current_roi'], [1152, 48, 192, 80])
        self.assertEqual(stats['fails'], 0)

    def test_failed_cache_is_replaced_by_learn(self):
        cm_service.save_marker(self.config, self.metadata, self.info,
                               self.candidate([32, 48, 48, 48]), self.features)
        cm_service.fail_marker(self.config, self.metadata, self.info)
        cm_service.fail_marker(self.config, self.metadata, self.info)
        cm_service.save_marker(self.config, self.metadata, self.info,
                               self.candidate([88, 24, 192, 80]), self.features)
        marker, _ = cm_service.marker_files(self.config, self.metadata, self.info)
        self.assertEqual(json.loads(marker.read_text())['roi'], [88, 24, 192, 80])

    def test_same_roi_save_refreshes_without_resetting_confirm_count(self):
        cm_service.save_marker(self.config, self.metadata, self.info,
                               self.candidate([32, 48, 48, 48]), self.features)
        cm_service.confirm_marker(self.config, self.metadata, self.info)
        cm_service.save_marker(self.config, self.metadata, self.info,
                               self.candidate([32, 48, 48, 48]), self.features)
        stats = cm_service.load_marker_stats(self.config, self.metadata, self.info)
        self.assertEqual(stats['confirms'], 1)
        self.assertEqual(stats['current_roi'], [32, 48, 48, 48])

    def test_load_marker_returns_working_cache(self):
        cm_service.save_marker(self.config, self.metadata, self.info,
                               self.candidate([32, 48, 48, 48]), self.features)
        marker = cm_service.load_marker(self.config, self.metadata, self.info)
        self.assertIsNotNone(marker)
        self.assertEqual(marker['roi'], [32, 48, 48, 48])

    def test_inconsistent_cache_is_repaired_from_its_alternative(self):
        """The exact 2026-11-22 state: roi from one marker, crops from another."""
        roi, alt_roi = [56, 974, 192, 80], [1328, 24, 72, 48]
        marker, refs_dir = self.corrupt(roi, alt_roi)
        repaired = cm_service.load_marker(self.config, self.metadata, self.info)
        self.assertIsNotNone(repaired)
        self.assertEqual(repaired['roi'], alt_roi)
        # The repair has to reach the disk: the next job must not read the pair
        # that was just rejected.
        saved = json.loads(marker.read_text())
        self.assertEqual(saved['roi'], alt_roi)
        for name in saved['references']:
            self.assertEqual(cm_service.pgm_size(refs_dir / name), (72, 48))
        stats = cm_service.load_marker_stats(self.config, self.metadata, self.info)
        self.assertEqual(stats['current_roi'], alt_roi)
        self.assertIsNone(stats['alt'])

    def test_inconsistent_cache_without_alternative_is_relearned(self):
        self.corrupt([56, 24, 56, 64])
        self.assertIsNone(cm_service.load_marker(self.config, self.metadata, self.info))

    def test_missing_reference_crop_is_not_a_marker(self):
        cm_service.save_marker(self.config, self.metadata, self.info,
                               self.candidate([32, 48, 48, 48]), self.features)
        marker, refs_dir = cm_service.marker_files(self.config, self.metadata, self.info)
        for name in json.loads(marker.read_text())['references']:
            (refs_dir / name).unlink()
        self.assertIsNone(cm_service.load_marker(self.config, self.metadata, self.info))

    def test_alternative_cannot_overwrite_a_promoted_marker(self):
        """Regression: the promoted marker used to read the slot it shared."""
        first, second, third = [32, 48, 48, 48], [1152, 48, 192, 80], [1184, 48, 192, 80]
        for roi, fill in ((first, 1), (second, 2)):
            cm_service.save_marker(self.config, self.metadata, self.info,
                                   self.candidate(roi, fill), self.features)
        cm_service.fail_marker(self.config, self.metadata, self.info)
        self.assertTrue(cm_service.fail_marker(self.config, self.metadata, self.info))
        marker, refs_dir = cm_service.marker_files(self.config, self.metadata, self.info)
        self.assertEqual(json.loads(marker.read_text())['roi'], second)
        # A third marker is only an alternative now and must leave the crops of
        # the promoted marker exactly as they were.
        cm_service.save_marker(self.config, self.metadata, self.info,
                               self.candidate(third, 3), self.features)
        saved = json.loads(marker.read_text())
        self.assertEqual(saved['roi'], second)
        for name in saved['references']:
            self.assertEqual(cm_service.pgm_size(refs_dir / name), (192, 80))
        self.assertEqual(cm_service.load_marker(self.config, self.metadata, self.info)['roi'],
                         second)
        stats = cm_service.load_marker_stats(self.config, self.metadata, self.info)
        self.assertEqual(stats['alt']['roi'], third)

    def test_mismatched_candidate_is_not_cached(self):
        candidate = self.candidate([32, 48, 48, 48])
        (self.features / candidate['references'][0]).write_bytes(pgm(2, 2))
        cm_service.save_marker(self.config, self.metadata, self.info, candidate, self.features)
        marker, _ = cm_service.marker_files(self.config, self.metadata, self.info)
        self.assertFalse(marker.exists())

    def alt_setup(self):
        """A working cache plus the alternative a later programme produced."""
        first, second = [32, 48, 48, 48], [1152, 48, 192, 80]
        cm_service.save_marker(self.config, self.metadata, self.info,
                               self.candidate(first, 1), self.features)
        cm_service.confirm_marker(self.config, self.metadata, self.info)
        cm_service.save_marker(self.config, self.metadata, self.info,
                               self.candidate(second, 2), self.features)
        return first, second

    def test_known_alternative_is_offered_as_a_candidate(self):
        """The stored station mark is measured, not only the learned telops."""
        _, second = self.alt_setup()
        known = cm_service.known_candidates(self.config, self.metadata, self.info, self.features)
        self.assertEqual(len(known), 1)
        self.assertEqual(known[0]['roi'], second)
        self.assertTrue(known[0]['known'])
        for path in known[0]['paths']:
            self.assertTrue(path.is_file())
            self.assertEqual(cm_service.pgm_size(path), (192, 80))

    def test_known_candidates_skip_the_mark_already_measured(self):
        first, second = self.alt_setup()
        skipped = cm_service.known_candidates(self.config, self.metadata, self.info,
                                              self.features, skip_roi=second)
        self.assertEqual(skipped, [])
        kept = cm_service.known_candidates(self.config, self.metadata, self.info,
                                           self.features, skip_roi=first)
        self.assertEqual([entry['roi'] for entry in kept], [second])

    def test_known_candidates_ignore_an_unusable_alternative(self):
        self.alt_setup()
        _, refs_dir = cm_service.marker_files(self.config, self.metadata, self.info)
        stats = cm_service.load_marker_stats(self.config, self.metadata, self.info)
        for name in stats['alt']['references']:
            (refs_dir / name).write_bytes(pgm(2, 2))
        self.assertEqual(cm_service.known_candidates(self.config, self.metadata, self.info,
                                                     self.features), [])

    def test_read_overlays_keeps_candidates_in_step(self):
        features = Path(self.temp.name) / 'overlays'
        features.mkdir()
        (features / 'overlay-00.csv').write_text('bin_start,samples,correlation\n0,30,0.5\n')
        (features / 'overlay-02.csv').write_text('bin_start,samples,correlation\n0,30,0.1\n')
        rows, kept = cm_service.read_overlays(features, [dict(index=i) for i in range(3)])
        self.assertEqual([entry['index'] for entry in kept], [0, 2])
        self.assertEqual(len(rows), 2)

    def test_scan_that_stopped_before_the_first_frame_may_be_retried(self):
        """cm-scan opens video.csv before it decodes anything it can refuse."""
        features = Path(self.temp.name) / 'scan'
        features.mkdir()
        (features / 'video.csv').write_text('pts,mean,black_fraction,scene,logo_edge\n')
        self.assertFalse(cm_service.scan_produced_frames(features))
        (features / 'video.csv').write_text(
            'pts,mean,black_fraction,scene,logo_edge\n0.033,120,0.1,0.2,0\n')
        self.assertTrue(cm_service.scan_produced_frames(features))
        self.assertFalse(cm_service.scan_produced_frames(Path(self.temp.name) / 'absent'))
