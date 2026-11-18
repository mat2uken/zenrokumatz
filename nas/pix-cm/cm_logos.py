#!/usr/bin/env python3
"""Station logos from the recorder, used to seed the CM marker cache.

The STB records the station's logo from the broadcast (BS-15's engineering
service for satellite, each channel's CDT for terrestrial) and serves it as
`GET /v1/logos` (index) and `GET /v1/logos/<service id>.png`. Learning the
mark from a recording is what fails first: short recordings, a mark that is
covered for most of a programme, or a service whose mark has never been
learned. The logo gives the shape for free, so all that is left is finding
where on screen it sits: correlate it against the corners of a few frames,
then crop the same region out of those frames and cache it exactly as the
learned marker would be.

Nothing here changes the decision rules; a seeded marker is measured by
cm-overlay like any other, and a marker that does not measure well is still
re-learned by the caller.
"""
import json
import subprocess
import urllib.request

import numpy as np

# Logo types are authored for different screen shapes. The recording may also
# be anamorphic (1440x1080 for a 16:9 picture), which changes the horizontal
# scale only, so both axes are varied.
# A logo authored at 36x24 can be shown at twice that on screen, and a 16:9
# picture stored as 1440x1080 stretches it by 1.333 across but not down.
SCALES = ((1.0, 1.0), (0.75, 1.0), (1.3333, 1.0), (1.5, 1.0), (1.75, 1.0),
          (2.0, 1.0), (2.4, 1.0), (0.667, 1.0), (0.9, 0.9), (1.1, 1.1),
          (1.25, 1.25), (1.4, 1.4), (1.6, 1.6), (1.9, 1.9), (2.2, 2.2))
# A match has to be this strong before it is believed at all.
MIN_SCORE = 0.55
# The cropped reference has to look like the logo as well.
MIN_CROP_SCORE = 0.50
# Frames are taken from the first minutes: the mark does not move, and this
# keeps the seek cheap on a fragmented MP4.
FRAME_WINDOW = 300
FRAME_COUNT = 5
# The best position has to repeat within this many pixels across frames.
POSITION_TOLERANCE = 8

BAND = 'top'


def marker_is_plausible(roi, info):
    """A station watermark is a few dozen pixels wide, never a few pixels."""
    x, y, w, h = roi
    min_w = max(24.0, info['width'] / 36.0)
    min_h = max(12.0, info['height'] / 54.0)
    if w < min_w or h < min_h:
        return False
    ratio = w / max(1.0, float(h))
    return 0.5 <= ratio <= 8.0


def service_id(metadata):
    service = metadata.get('service') or {}
    return service.get('id') or service.get('serviceId')


def _get(url, timeout):
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return response.read()


def fetch_logo(config, ident):
    """The logo for one service id, or None.

    `config['logo_source']['urls']` lists the recorders to ask; the first one
    that has the service wins. Nothing is raised: a recorder that is off or a
    service without a logo just means learning from the recording as before.
    """
    source = config.get('logo_source') or {}
    if not source.get('enabled'):
        return None
    timeout = float(source.get('timeout', 5))
    for base in source.get('urls') or []:
        try:
            index = json.loads(_get(base.rstrip('/') + '/v1/logos', timeout))
        except (OSError, ValueError):
            continue
        for entry in index.get('logos') or []:
            if entry.get('id') != ident:
                continue
            name = str(entry.get('file') or '')
            if not name or '/' in name or '..' in name:
                continue
            try:
                png = _get(base.rstrip('/') + '/v1/logos/' + name, timeout)
            except OSError:
                continue
            return dict(png=png, entry=entry, base=base)
    return None


def decode_png(png, width, height, ffmpeg):
    """RGBA pixels of the logo as (luma, alpha) float arrays."""
    result = subprocess.run([str(ffmpeg), '-v', 'error', '-i', '-', '-f', 'rawvideo',
                             '-pix_fmt', 'rgba', '-'], input=png, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, timeout=30)
    if result.returncode or len(result.stdout) != width * height * 4:
        return None, None
    rgba = np.frombuffer(result.stdout, dtype=np.uint8).reshape(height, width, 4).astype(np.float32)
    alpha = rgba[:, :, 3] / 255.0
    luma = 0.299 * rgba[:, :, 0] + 0.587 * rgba[:, :, 1] + 0.114 * rgba[:, :, 2]
    return luma, alpha


def frames_gray(ffmpeg, source, width, height, window=FRAME_WINDOW, count=FRAME_COUNT):
    """A few greyscale frames from the first minutes of the recording."""
    step = max(1.0, window / max(1, count))
    argv = [str(ffmpeg), '-v', 'error', '-t', str(window), '-i', str(source),
            '-vf', f'fps=1/{step:.3f}', '-pix_fmt', 'gray', '-f', 'rawvideo', '-']
    result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            timeout=600)
    if result.returncode:
        return []
    size = width * height
    data = np.frombuffer(result.stdout, dtype=np.uint8)
    frames = []
    for offset in range(0, len(data) - size + 1, size):
        frames.append(data[offset:offset + size].reshape(height, width).astype(np.float32))
    return frames


def _correlate(frame, kernel):
    """Valid-region correlation: out[y,x] = sum frame[y+u, x+v] * kernel[u,v]."""
    fh, fw = frame.shape
    kh, kw = kernel.shape
    if fh < kh or fw < kw:
        return None
    shape = (fh + kh - 1, fw + kw - 1)
    spectrum = np.fft.rfft2(frame, s=shape) * np.fft.rfft2(kernel[::-1, ::-1], s=shape)
    full = np.fft.irfft2(spectrum, s=shape)
    return full[kh - 1:fh, kw - 1:fw]


def masked_ncc(frame, template, mask):
    """Masked normalised cross-correlation of the template over the frame."""
    template = template * mask
    count = float(mask.sum())
    if count < 16:
        return None
    sum_ft = _correlate(frame, template)
    sum_f = _correlate(frame, mask)
    sum_f2 = _correlate(frame * frame, mask)
    if sum_ft is None:
        return None
    mean_t = float(template.sum()) / count
    var_t = float((template * template).sum()) - count * mean_t * mean_t
    if var_t <= 1e-6:
        return None
    numerator = sum_ft - sum_f * mean_t
    variance = sum_f2 - sum_f * sum_f / count
    denominator = np.sqrt(np.maximum(variance * var_t, 1e-9))
    score = numerator / denominator
    score[variance <= 1e-6] = -1.0
    return score


def _resize(image, height, width):
    """Nearest-neighbour resize (the logo is only a few dozen pixels wide)."""
    rows = (np.arange(height) * image.shape[0] // height).clip(0, image.shape[0] - 1)
    cols = (np.arange(width) * image.shape[1] // width).clip(0, image.shape[1] - 1)
    return image[rows][:, cols]


def _band(frame, corner, width, height):
    """A corner region: corner is 'tl', 'tr', 'bl' or 'br'."""
    fh, fw = frame.shape
    height = min(height, fh)
    width = min(width, fw)
    y = 0 if corner[0] == 't' else fh - height
    x = 0 if corner[1] == 'l' else fw - width
    return x, y, frame[y:y + height, x:x + width]


def _corners():
    return ('tl', 'tr', 'bl', 'br')


def locate(frames, template, mask):
    """Where the logo sits: (x, y, w, h, score) or None.

    The same corner and scale must win in at least half the frames, and the
    positions must agree within POSITION_TOLERANCE.
    """
    votes = []
    for frame in frames:
        fh, fw = frame.shape
        band_width = int(max(2.2 * template.shape[1], 0.30 * fw))
        band_height = int(max(2.2 * template.shape[0], 0.12 * fh))
        best = None
        for corner in _corners():
            bx, by, patch = _band(frame, corner, band_width, band_height)
            for sx, sy in SCALES:
                height = max(6, int(round(template.shape[0] * sy)))
                width = max(6, int(round(template.shape[1] * sx)))
                if height > patch.shape[0] or width > patch.shape[1]:
                    continue
                scaled = _resize(template, height, width)
                scaled_mask = _resize(mask, height, width)
                score = masked_ncc(patch, scaled, scaled_mask)
                if score is None:
                    continue
                row, col = np.unravel_index(int(np.argmax(score)), score.shape)
                value = float(score[row, col])
                if best is None or value > best[0]:
                    best = (value, corner, (sx, sy), bx + col, by + row, width, height)
        if best:
            votes.append(best)
    if not votes:
        return None
    # Majority corner and scale, then the median position.
    keys = {}
    for vote in votes:
        keys.setdefault((vote[1], vote[2]), []).append(vote)
    corner, scale = max(keys, key=lambda k: len(keys[k]))
    chosen = keys[(corner, scale)]
    if len(chosen) * 2 < len(votes):
        return None
    scores = [vote[0] for vote in chosen]
    if sorted(scores)[len(scores) // 2] < MIN_SCORE:
        return None
    xs = [vote[3] for vote in chosen]
    ys = [vote[4] for vote in chosen]
    if max(xs) - min(xs) > POSITION_TOLERANCE or max(ys) - min(ys) > POSITION_TOLERANCE:
        return None
    width, height = chosen[0][5], chosen[0][6]
    return (int(np.median(xs)), int(np.median(ys)), width, height, float(np.median(scores)))


def write_pgm(path, patch):
    data = np.clip(patch, 0, 255).astype(np.uint8)
    header = f'P5\n{data.shape[1]} {data.shape[0]}\n255\n'.encode('ascii')
    path.write_bytes(header + data.tobytes())


def crop_score(patch, template, mask, width, height):
    """How much a crop looks like the logo, at that crop's size."""
    if patch.shape != (height, width):
        return -1.0
    scaled = _resize(template, height, width)
    scaled_mask = _resize(mask, height, width)
    score = masked_ncc(patch, scaled, scaled_mask)
    if score is None or score.shape != (1, 1):
        return -1.0
    return float(score[0, 0])


def seed_marker(config, metadata, info, source, stage, meta_path, refs_dir, ffmpeg,
                log=print):
    """Write a marker cache entry from the station logo; False when not possible."""
    ident = service_id(metadata)
    if not ident:
        return False
    found = fetch_logo(config, ident)
    if not found:
        return False
    entry = found['entry']
    width = int(entry.get('width') or 0)
    height = int(entry.get('height') or 0)
    if width < 8 or height < 8:
        log(json.dumps(dict(event='logo_seed_skipped', reason='logo too small', id=ident)))
        return False
    template, alpha = decode_png(found['png'], width, height, ffmpeg)
    if template is None:
        log(json.dumps(dict(event='logo_seed_skipped', reason='logo decode failed', id=ident)))
        return False
    coverage = float((alpha > 0.5).mean())
    if coverage < 0.02:
        # A fully transparent logo is what some stations put in the carousel:
        # it carries no mark, so there is nothing to look for on screen.
        log(json.dumps(dict(event='logo_seed_skipped', reason='logo is blank', id=ident)))
        return False
    mask = np.ones_like(alpha) if coverage >= 0.98 else (alpha > 0.5).astype(np.float32)
    if float(template[mask > 0].std()) < 2.0:
        log(json.dumps(dict(event='logo_seed_skipped', reason='logo is flat', id=ident)))
        return False
    frames = frames_gray(ffmpeg, source, info['width'], info['height'])
    if len(frames) < 2:
        log(json.dumps(dict(event='logo_seed_skipped', reason='no frames', id=ident)))
        return False
    roi = locate(frames, template, mask)
    if not roi:
        log(json.dumps(dict(event='logo_seed_skipped', reason='logo not found', id=ident)))
        return False
    x, y, w, h, score = roi
    if not marker_is_plausible([x, y, w, h], info):
        log(json.dumps(dict(event='logo_seed_skipped', reason='implausible roi', id=ident)))
        return False
    references = []
    for index in range(min(3, len(frames))):
        patch = frames[index][y:y + h, x:x + w]
        if patch.shape != (h, w):
            continue
        if crop_score(patch, template, mask, w, h) < MIN_CROP_SCORE:
            continue
        name = f'ref-00-{index}.pgm'
        refs_dir.mkdir(parents=True, exist_ok=True)
        write_pgm(refs_dir / name, patch)
        references.append(name)
    if not references:
        log(json.dumps(dict(event='logo_seed_skipped', reason='no usable crops', id=ident)))
        return False
    meta_path.parent.mkdir(parents=True, exist_ok=True)
    meta_path.write_text(json.dumps(dict(roi=[x, y, w, h], references=references, band=BAND,
                                         service=(metadata.get('service') or {}).get('name'),
                                         size=[int(info['width']), int(info['height'])],
                                         source='logo', logo_bytes=len(found['png'])),
                                    ensure_ascii=False, indent=2) + '\n')
    log(json.dumps(dict(event='logo_seed', id=ident, roi=[x, y, w, h], score=round(score, 3),
                        references=len(references), base=found['base'],
                        logo=[width, height], stage=str(stage))))
    return True
