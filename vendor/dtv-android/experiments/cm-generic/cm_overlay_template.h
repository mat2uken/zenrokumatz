// Gradient template for the station-overlay matcher, and the "is this a logo"
// test that goes with it.
//
// Two programs need this answer: cm-overlay, which measures a candidate over a
// whole recording, and cm-scan, which measures a cached mark inline and also
// learns new candidates. The learner used to keep the most temporally stable
// patch in each band, and sky, a studio wall or scan lines are perfectly stable
// and beat the real station mark. After uniform rows and columns are stripped
// such a patch holds no structure, so the matcher refuses it and the recording
// ends up with no usable mark at all. Deciding "is this a logo" in one place
// keeps the learner from proposing patches the matcher will refuse, and keeps
// the two from drifting apart.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// A luma patch that may become a reference for the matcher.
struct OverlayPatch {
    int w = 0, h = 0;
    std::vector<unsigned char> y;
};

// Builds the gradient template the matcher correlates against and returns the
// number of pixels that carry logo structure. `out` holds (gx, gy) per pixel.
inline int build_overlay_template(const std::vector<OverlayPatch>& patches, int w, int h,
                                  std::vector<double>& out, double& energy) {
    out.assign(size_t(w) * h * 2, 0.0);
    energy = 0.0;
    if (patches.empty()) return 0;
    auto value = [](const OverlayPatch& patch, int i, int j) {
        return double(patch.y[size_t(j) * patch.w + i]);
    };
    for (int j = 1; j < h - 1; ++j) {
        for (int i = 1; i < w - 1; ++i) {
            const int p = j * w + i;
            double gx0 = 0, gy0 = 0, gxsum = 0, gysum = 0;
            bool stable = true;
            for (const auto& patch : patches) {
                const double gx = value(patch, i + 1, j) - value(patch, i - 1, j);
                const double gy = value(patch, i, j + 1) - value(patch, i, j - 1);
                const double e = gx * gx + gy * gy;
                if (e < 36) { stable = false; break; }
                if (&patch == &patches.front()) {
                    gx0 = gx; gy0 = gy;
                } else {
                    const double base = gx0 * gx0 + gy0 * gy0;
                    const double agreement = (gx * gx0 + gy * gy0) / std::sqrt(base * e);
                    if (agreement < .8 || std::min(base, e) / std::max(base, e) < .25) {
                        stable = false;
                        break;
                    }
                }
                gxsum += std::clamp(gx, -20., 20.);
                gysum += std::clamp(gy, -20., 20.);
            }
            if (stable) {
                out[size_t(p) * 2] = gxsum / patches.size();
                out[size_t(p) * 2 + 1] = gysum / patches.size();
            }
        }
    }
    // Uniform full-width/full-height lines are reference background, not glyphs.
    std::vector<bool> remove(size_t(w) * h, false);
    for (int j = 1; j < h - 1; ++j) {
        int n = 0;
        for (int i = 1; i < w - 1; ++i) {
            const int p = j * w + i;
            n += std::abs(out[size_t(p) * 2 + 1]) >= 6 && std::abs(out[size_t(p) * 2]) < 3;
        }
        if (n > .70 * w)
            for (int y0 = std::max(0, j - 1); y0 <= std::min(h - 1, j + 1); ++y0)
                for (int i = 0; i < w; ++i) remove[size_t(y0) * w + i] = true;
    }
    for (int i = 1; i < w - 1; ++i) {
        int n = 0;
        for (int j = 1; j < h - 1; ++j) {
            const int p = j * w + i;
            n += std::abs(out[size_t(p) * 2]) >= 6 && std::abs(out[size_t(p) * 2 + 1]) < 3;
        }
        if (n > .70 * h)
            for (int x0 = std::max(0, i - 1); x0 <= std::min(w - 1, i + 1); ++x0)
                for (int j = 0; j < h; ++j) remove[size_t(j) * w + x0] = true;
    }
    int mask_count = 0;
    for (int p = 0; p < w * h; ++p) {
        if (remove[size_t(p)]) out[size_t(p) * 2] = out[size_t(p) * 2 + 1] = 0;
        const double e = out[size_t(p) * 2] * out[size_t(p) * 2] +
                         out[size_t(p) * 2 + 1] * out[size_t(p) * 2 + 1];
        energy += e;
        if (e > 0) ++mask_count;
    }
    return mask_count;
}

// The matcher needs this much structure before a patch means anything.
static constexpr int kMinLogoMaskPixels = 20;
