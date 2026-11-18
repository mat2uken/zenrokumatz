#!/usr/bin/env python3
"""Tests for cm_logos: the correlation and where the logo is found."""
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cm_logos


def brute_ncc(frame, template, mask):
    fh, fw = frame.shape
    th, tw = template.shape
    best = None
    weighted = template * mask
    count = mask.sum()
    mean_t = weighted.sum() / count
    var_t = (weighted * weighted).sum() - count * mean_t * mean_t
    for y in range(fh - th + 1):
        for x in range(fw - tw + 1):
            patch = frame[y:y + th, x:x + tw]
            sum_f = (patch * mask).sum()
            sum_f2 = (patch * patch * mask).sum()
            sum_ft = (patch * weighted).sum()
            variance = sum_f2 - sum_f * sum_f / count
            value = (sum_ft - sum_f * mean_t) / np.sqrt(max(variance * var_t, 1e-9))
            if best is None or value > best[0]:
                best = (float(value), x, y)
    return best


def test_correlation_matches_brute_force():
    rng = np.random.default_rng(7)
    frame = rng.normal(128, 30, (40, 60)).astype(np.float32)
    template = rng.normal(128, 40, (12, 9)).astype(np.float32)
    mask = (rng.random((12, 9)) > 0.3).astype(np.float32)
    mask[0, 0] = 1.0
    score = cm_logos.masked_ncc(frame, template, mask)
    width, height = template.shape[1], template.shape[0]
    brute = brute_ncc(frame, template, mask)
    row, col = np.unravel_index(int(np.argmax(score)), score.shape)
    assert score.shape == (frame.shape[0] - height + 1, frame.shape[1] - width + 1)
    assert abs(float(score[row, col]) - brute[0]) < 1e-4, (score[row, col], brute)
    assert (col, row) == (brute[1], brute[2])


def make_logo(width=128, height=56):
    """A station mark: letters on a bar, transparent elsewhere.

    The visible pixels must not be a single value: a flat patch has no
    variance, and a normalised correlation against it means nothing.
    """
    alpha = np.zeros((height, width), dtype=np.float32)
    alpha[8:height - 8, 6:width - 6] = 1.0
    alpha[20:32, 40:70] = 0.0
    gray = np.full((height, width), 235.0, dtype=np.float32)
    xx = np.arange(width)[None, :]
    yy = np.arange(height)[:, None]
    letters = 150.0 + 90.0 * (((xx // 9) + (yy // 7)) % 2)
    gray[8:height - 8, 6:width - 6] = letters[8:height - 8, 6:width - 6]
    gray[20:32, 40:70] = 20.0
    return gray, alpha


def place(frame, template, mask, x, y, sx, sy):
    height = int(round(template.shape[0] * sy))
    width = int(round(template.shape[1] * sx))
    scaled = cm_logos._resize(template, height, width)
    scaled_mask = cm_logos._resize(mask, height, width)
    region = frame[y:y + height, x:x + width]
    frame[y:y + height, x:x + width] = region * (1 - scaled_mask) + scaled * scaled_mask
    return width, height


def test_locate_finds_anamorphic_logo():
    rng = np.random.default_rng(3)
    template, alpha = make_logo()
    mask = (alpha > 0.5).astype(np.float32)
    frames = []
    for _ in range(4):
        frame = rng.normal(110, 25, (1080, 1440)).astype(np.float32)
        # A bright corner patch would fool a naive search; the logo is at the
        # position a 1440x1080 stream would show it (x scaled by 0.75).
        place(frame, template, mask, 1312, 24, 0.75, 1.0)
        frames.append(frame)
    roi = cm_logos.locate(frames, template, mask)
    assert roi is not None
    x, y, w, h, score = roi
    assert abs(x - 1312) <= 4, roi
    assert abs(y - 24) <= 4, roi
    assert abs(w - 96) <= 4 and abs(h - 56) <= 3, roi
    assert score >= cm_logos.MIN_SCORE


def test_locate_rejects_noise():
    rng = np.random.default_rng(11)
    template, alpha = make_logo()
    mask = (alpha > 0.5).astype(np.float32)
    frames = [rng.normal(110, 25, (1080, 1440)).astype(np.float32) for _ in range(3)]
    assert cm_logos.locate(frames, template, mask) is None


def test_marker_is_plausible():
    info = dict(width=1440, height=1080)
    assert cm_logos.marker_is_plausible([1312, 24, 96, 56], info)
    assert not cm_logos.marker_is_plausible([0, 0, 4, 3], info)
    assert not cm_logos.marker_is_plausible([0, 0, 96, 2], info)


def test_seed_marker_writes_a_cache(tmp=Path('/tmp/cm-logos-test')):
    """End to end with a fake recorder and a fake recording toolkit."""
    rng = np.random.default_rng(5)
    template, alpha = make_logo()
    mask = (alpha > 0.5).astype(np.float32)
    frame = rng.normal(110, 25, (1080, 1440)).astype(np.float32)
    w, h = place(frame, template, mask, 1312, 24, 0.75, 1.0)

    class FakeFfmpeg:
        """Stands in for ffmpeg: serves the logo's PNG and the frames."""

        def __init__(self):
            self.calls = []

        def __call__(self, png, width, height):
            rgba = np.zeros((height, width, 4), dtype=np.uint8)
            rgba[:, :, 0] = template
            rgba[:, :, 1] = template
            rgba[:, :, 2] = template
            rgba[:, :, 3] = (mask * 255).astype(np.uint8)
            return template.astype(np.float32), mask

    import shutil
    if tmp.exists():
        shutil.rmtree(tmp)
    (tmp / 'refs').mkdir(parents=True)

    original_decode = cm_logos.decode_png
    original_frames = cm_logos.frames_gray
    original_fetch = cm_logos.fetch_logo
    try:
        cm_logos.fetch_logo = lambda config, ident: dict(
            png=b'x', entry=dict(id=ident, file='1.png', width=128, height=56), base='stub')
        cm_logos.decode_png = lambda png, width, height, ffmpeg: (template, mask)
        cm_logos.frames_gray = lambda ffmpeg, source, width, height: [frame.copy() for _ in range(3)]
        ok = cm_logos.seed_marker(
            dict(logo_source=dict(enabled=True, urls=['http://stub'])),
            dict(service=dict(id=3219535864, name='テレビ静岡')),
            dict(width=1440, height=1080), Path('/dev/null'), tmp,
            tmp / 'marker.json', tmp / 'refs', Path('/usr/bin/ffmpeg'),
            log=lambda line: None)
        assert ok
        marker = json.loads((tmp / 'marker.json').read_text())
        assert marker['source'] == 'logo'
        assert abs(marker['roi'][0] - 1312) <= 4 and abs(marker['roi'][1] - 24) <= 4
        assert marker['roi'][2] == w and marker['roi'][3] == h
        assert len(marker['references']) >= 2
        for name in marker['references']:
            data = (tmp / 'refs' / name).read_bytes()
            assert data.startswith(b'P5\n')
            assert data.split(b'\n')[1] == f'{w} {h}'.encode()
    finally:
        cm_logos.fetch_logo = original_fetch
        cm_logos.decode_png = original_decode
        cm_logos.frames_gray = original_frames


def test_seed_marker_without_logo_is_false():
    assert not cm_logos.seed_marker({}, dict(service=dict(id=1)), dict(width=1440, height=1080),
                                    Path('/dev/null'), Path('/tmp'), Path('/tmp/m.json'),
                                    Path('/tmp/refs'), Path('/usr/bin/ffmpeg'),
                                    log=lambda line: None)


if __name__ == '__main__':
    test_correlation_matches_brute_force()
    test_locate_finds_anamorphic_logo()
    test_locate_rejects_noise()
    test_marker_is_plausible()
    test_seed_marker_writes_a_cache()
    test_seed_marker_without_logo_is_false()
    print('test_cm_logos: ok')
