// Single-pass feature extraction, station-overlay learning and review thumbnails.
//
// The input is the received fragmented MP4, read once from start to end. No
// seeking is used because the recorder writes fragments without a random-access
// index. Every output goes to the caller-supplied directory, which the service
// keeps on tmpfs.
//
// Outputs:
//   video.csv, audio.csv   same schema as the previous probe
//   thumbs.bin             downscaled frames for manual review
//   logo.json              persistent-overlay candidates with reference crops
//   ref-<candidate>-<k>.pgm  full-resolution crops for the overlay matcher
//   structure-<band>.pgm   agreement picture, diagnosis only
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "cm_overlay_template.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace {

void need(bool ok, const char* text) {
    if (!ok) throw std::runtime_error(text);
}

constexpr int kBlock = 8;
constexpr double kMinMagnitude = 2.0;   // mean |gradient| of an overlay pixel
constexpr double kMinStable = 1.5;      // mean signed gradient magnitude
constexpr double kMinAgreement = 0.55;  // stable / magnitude

struct Band {
    int y = 0, h = 0;
    std::vector<double> gx, gy, magnitude;
    long samples = 0;
    Band() = default;
    Band(int height, int width) : gx(size_t(width) * height, 0.0), gy(size_t(width) * height, 0.0),
                                  magnitude(size_t(width) * height, 0.0) {}
};

struct WindowFrame {
    double start = 0;
    bool stored = false;
    std::vector<uint8_t> top, bottom;  // first frame of the window, band rows
};

struct Candidate {
    int x = 0, y = 0, w = 0, h = 0;
    int band = 0;
    double score = 0, magnitude = 0;
    double consistency = 0;  // mean normalised match across sampling windows
    double worst_consistency = 0;
    int blocks = 0;
    std::vector<int> windows;
    std::vector<std::string> references;
};

std::string only_name(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void write_pgm(const std::string& path, int w, int h, const uint8_t* data) {
    FILE* out = fopen(path.c_str(), "wb");
    need(out != nullptr, "open PGM for writing");
    fprintf(out, "P5\n%d %d\n255\n", w, h);
    need(fwrite(data, 1, size_t(w) * h, out) == size_t(w) * h, "write PGM body");
    need(fclose(out) == 0, "close PGM");
}

struct Features {
    std::string dir;
    FILE *video = nullptr, *audio = nullptr, *thumb = nullptr;
    std::vector<uint8_t> previous;
    double thumb_interval = 5, next_thumb = 0;
    long frames = 0, audio_frames = 0;

    explicit Features(const std::string& directory) : dir(directory) {
        video = fopen((dir + "/video.csv").c_str(), "w");
        audio = fopen((dir + "/audio.csv").c_str(), "w");
        thumb = fopen((dir + "/thumbs.bin").c_str(), "wb");
        need(video && audio && thumb, "output files");
        if (const char* interval = getenv("CM_THUMB_SECONDS")) {
            double value = atof(interval);
            if (value > 0) thumb_interval = value;
        }
        fprintf(video, "pts,mean,black_fraction,scene,logo_edge\n");
        fprintf(audio, "pts,samples,rms_db\n");
    }
    void video_frame(double pts, const std::vector<uint8_t>& y) {
        double sum = 0, diff = 0, edge = 0;
        int black = 0, edges = 0;
        for (size_t i = 0; i < y.size(); ++i) {
            sum += y[i];
            black += y[i] < 32;
            if (!previous.empty()) diff += std::abs(int(y[i]) - int(previous[i]));
        }
        // Diagnostic only: upper-right edge energy of the downscaled frame.
        for (int j = 5; j < 38; ++j)
            for (int i = 240; i < 315; ++i) {
                edge += std::abs(int(y[size_t(j) * 320 + i]) - int(y[size_t(j) * 320 + i - 1]));
                ++edges;
            }
        fprintf(video, "%.6f,%.6f,%.6f,%.6f,%.6f\n", pts, sum / y.size(),
                double(black) / y.size(), diff / y.size(), edge / edges);
        if (pts + 1e-5 >= next_thumb) {
            fwrite(&pts, sizeof(pts), 1, thumb);
            fwrite(y.data(), 1, y.size(), thumb);
            next_thumb = pts + thumb_interval;
        }
        previous = y;
        ++frames;
    }
    void audio_frame(double pts, const int16_t* pcm, int samples, int channels) {
        double energy = 0;
        for (int i = 0; i < samples * channels; ++i) {
            double x = pcm[i] / 32768.0;
            energy += x * x;
        }
        double db = 10 * log10(std::max(energy / std::max(1, samples * channels), 1e-12));
        fprintf(audio, "%.6f,%d,%.6f\n", pts, samples, db);
        ++audio_frames;
    }
    ~Features() {
        if (video) fclose(video);
        if (audio) fclose(audio);
        if (thumb) fclose(thumb);
    }
};

// Learns a persistent overlay from the whole-file pass.
class OverlayLearn {
  public:
    OverlayLearn(int width, int height, double duration, int window_count, double window_length, int references)
        : width_(width), height_(height), references_(references) {
        top_y_ = 0;
        top_h_ = std::max(24, int(height * 0.22));
        bottom_h_ = std::max(24, int(height * 0.18));
        bottom_y_ = height - bottom_h_;
        top_ = Band(top_h_, width);
        bottom_ = Band(bottom_h_, width);
        need(duration > 0, "duration required for overlay learning");
        int usable = std::max(2, std::min(window_count, std::max(2, int(duration / (window_length * 2)))));
        for (int i = 0; i < usable; ++i) {
            double t = duration * (i + 0.5) / usable - window_length / 2;
            WindowFrame frame;
            frame.start = std::max(0.0, std::min(duration - window_length, t));
            windows_.push_back(frame);
        }
        window_length_ = window_length;
    }

    int window_count() const { return int(windows_.size()); }

    // Called for every decoded frame; only frames inside a sampling window
    // contribute, so the cost stays proportional to the sampled time.
    void frame(double pts, const uint8_t* gray) {
        if (!base_known_) {
            base_ = pts;
            base_known_ = true;
        }
        int index = window_index(pts - base_);
        if (index < 0) return;
        accumulate(top_, top_y_, top_h_, gray);
        accumulate(bottom_, bottom_y_, bottom_h_, gray);
        auto& window = windows_[size_t(index)];
        if (!window.stored) {
            window.top.assign(gray, gray + size_t(top_h_) * width_);
            window.bottom.assign(gray + size_t(bottom_y_) * width_,
                                 gray + size_t(bottom_y_ + bottom_h_) * width_);
            window.stored = true;
        }
    }

    // Learns candidates and keeps them; references are exported afterwards so
    // that crops are written only for the few rectangles that survived.
    const std::vector<Candidate>& candidates() {
        std::vector<Candidate> found;
        collect(top_, 0, top_y_, top_h_, found);
        collect(bottom_, 1, bottom_y_, bottom_h_, found);
        std::sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) {
            return a.score > b.score;
        });
        for (auto& candidate : found) rank_windows(candidate);
        std::vector<Candidate> chosen;
        for (auto& candidate : found) {
            if (!measurable(candidate)) continue;
            bool overlaps = false;
            for (auto& kept : chosen) {
                const int pad = 8;
                if (candidate.x < kept.x + kept.w + pad && kept.x < candidate.x + candidate.w + pad &&
                    candidate.y < kept.y + kept.h + pad && kept.y < candidate.y + candidate.h + pad) {
                    overlaps = true;
                    break;
                }
            }
            if (!overlaps) chosen.push_back(candidate);
            if (chosen.size() >= 8) break;
        }
        chosen_ = std::move(chosen);
        return chosen_;
    }

    // True when the matcher will accept this candidate's reference crops.
    //
    // The learner used to keep the most temporally stable patch in each band.
    // Sky, a studio wall and scan lines are perfectly stable and beat the real
    // station mark, but after uniform rows and columns are stripped they carry
    // no structure, so cm-overlay refuses them and the recording ends up with
    // no usable mark at all. Asking the matcher's own question here keeps the
    // learner from proposing patches that can never be measured.
    bool measurable(const Candidate& candidate) const {
        const int band_y = candidate.band == 0 ? top_y_ : bottom_y_;
        const int local_y = candidate.y - band_y;
        std::vector<OverlayPatch> patches;
        for (size_t k = 0; k < candidate.windows.size(); ++k) {
            const auto& window = windows_[size_t(candidate.windows[k])];
            if (!window.stored) continue;
            const std::vector<uint8_t>& source = candidate.band == 0 ? window.top : window.bottom;
            OverlayPatch patch;
            patch.w = candidate.w;
            patch.h = candidate.h;
            patch.y.resize(size_t(candidate.w) * candidate.h);
            for (int j = 0; j < candidate.h; ++j)
                std::memcpy(&patch.y[size_t(j) * candidate.w],
                            &source[size_t(local_y + j) * width_ + candidate.x], candidate.w);
            patches.push_back(std::move(patch));
        }
        if (patches.size() < 2) return false;
        std::vector<double> scratch;
        double energy = 0;
        return build_overlay_template(patches, candidate.w, candidate.h, scratch, energy)
                   >= kMinLogoMaskPixels && energy > 0;
    }

    const std::vector<Candidate>& chosen_candidates() const { return chosen_; }

    // Exports the reference crops of every chosen candidate into `dir`.
    void export_references(const std::string& dir) {
        for (size_t index = 0; index < chosen_.size(); ++index) {
            Candidate& candidate = chosen_[index];
            const int band_y = candidate.band == 0 ? top_y_ : bottom_y_;
            const int band_h = candidate.band == 0 ? top_h_ : bottom_h_;
            const int local_y = candidate.y - band_y;
            need(local_y >= 0 && local_y + candidate.h <= band_h, "crop outside band");
            for (size_t k = 0; k < candidate.windows.size(); ++k) {
                const auto& window = windows_[size_t(candidate.windows[k])];
                if (!window.stored) continue;
                const std::vector<uint8_t>& source = candidate.band == 0 ? window.top : window.bottom;
                std::vector<uint8_t> crop(size_t(candidate.w) * candidate.h);
                for (int j = 0; j < candidate.h; ++j)
                    memcpy(&crop[size_t(j) * candidate.w],
                           &source[size_t(local_y + j) * width_ + candidate.x], candidate.w);
                char path[4096];
                snprintf(path, sizeof(path), "%s/ref-%02zu-%zu.pgm", dir.c_str(), index, k);
                write_pgm(path, candidate.w, candidate.h, crop.data());
                candidate.references.push_back(only_name(path));
            }
        }
    }

    void write_structure(const std::string& dir) const {
        write_structure_band(dir + "/structure-top.pgm", top_, top_h_);
        write_structure_band(dir + "/structure-bottom.pgm", bottom_, bottom_h_);
    }

  private:
    int window_index(double pts) const {
        // Windows are ordered; a binary search over starts is enough.
        int lo = 0, hi = int(windows_.size()) - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (pts < windows_[size_t(mid)].start) hi = mid - 1;
            else if (pts >= windows_[size_t(mid)].start + window_length_) lo = mid + 1;
            else return mid;
        }
        return -1;
    }

    void accumulate(Band& band, int y0, int h, const uint8_t* gray) {
        for (int j = 0; j < h; ++j) {
            const uint8_t* row = gray + size_t(y0 + j) * width_;
            const uint8_t* above = row - (j > 0 ? width_ : 0);
            const uint8_t* below = row + (j + 1 < h ? width_ : 0);
            size_t base = size_t(j) * width_;
            for (int i = 1; i + 1 < width_; ++i) {
                double gx = double(row[i + 1]) - double(row[i - 1]);
                double gy = double(below[i]) - double(above[i]);
                size_t p = base + i;
                band.gx[p] += gx;
                band.gy[p] += gy;
                band.magnitude[p] += std::abs(gx) + std::abs(gy);
            }
        }
        ++band.samples;
    }

    void collect(const Band& band, int band_index, int y0, int h, std::vector<Candidate>& out) const {
        if (band.samples <= 0) return;
        const int bw = width_ / kBlock, bh = h / kBlock;
        std::vector<double> block_strength(size_t(bw) * bh, 0.0);
        std::vector<int> active(size_t(bw) * bh, 0);
        for (int by = 0; by < bh; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                int hits = 0;
                double sum = 0;
                for (int j = by * kBlock; j < (by + 1) * kBlock; ++j) {
                    size_t base = size_t(j) * width_;
                    for (int i = bx * kBlock; i < (bx + 1) * kBlock; ++i) {
                        size_t p = base + i;
                        double mx = band.gx[p] / band.samples, my = band.gy[p] / band.samples;
                        double stable = std::abs(mx) + std::abs(my);
                        double magnitude = band.magnitude[p] / band.samples;
                        sum += stable;
                        if (magnitude >= kMinMagnitude && stable >= kMinStable &&
                            magnitude > 0 && stable / magnitude >= kMinAgreement)
                            ++hits;
                    }
                }
                size_t index = size_t(by) * bw + bx;
                block_strength[index] = sum / (kBlock * kBlock);
                // A broadcaster watermark is anchored in a frame corner; telops
                // and subtitles may be equally stable but are not corner-bound.
                const int global_x = bx * kBlock, global_y = y0 + by * kBlock;
                const bool corner_column = global_x < width_ * 20 / 100 || global_x + kBlock > width_ * 80 / 100;
                const bool corner_row = global_y < height_ * 13 / 100 || global_y + kBlock > height_ * 87 / 100;
                active[index] = (corner_column && corner_row && hits >= int(kBlock * kBlock * 0.35)) ? 1 : 0;
            }
        }
        // Integral picture of the block strength so the densest logo-sized
        // window can be found inside a component that also contains telops.
        const int iw = bw + 1;
        std::vector<double> integral(size_t(iw) * (bh + 1), 0.0);
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx)
                integral[size_t(by + 1) * iw + bx + 1] = block_strength[size_t(by) * bw + bx] +
                    integral[size_t(by) * iw + bx + 1] + integral[size_t(by + 1) * iw + bx] -
                    integral[size_t(by) * iw + bx];
        auto box_sum = [&](int x0, int y0b, int x1, int y1b) {
            return integral[size_t(y1b + 1) * iw + x1 + 1] - integral[size_t(y0b) * iw + x1 + 1] -
                   integral[size_t(y1b + 1) * iw + x0] + integral[size_t(y0b) * iw + x0];
        };
        // A station watermark is a small, compact mark, never a wide telop band.
        const int max_w = std::max(24, int(std::min<int>(width_ * 0.18, std::max<int>(64, width_ * 260 / 1920))));
        const int max_h = std::max(16, int(std::min<int>(h, std::max<int>(40, width_ * 110 / 1920))));
        const int wb = std::max(2, max_w / kBlock), hb = std::max(2, max_h / kBlock);

        std::vector<char> seen(active.size(), 0);
        std::vector<int> stack;
        for (int sy = 0; sy < bh; ++sy) {
            for (int sx = 0; sx < bw; ++sx) {
                size_t start = size_t(sy) * bw + sx;
                if (!active[start] || seen[start]) continue;
                int x0 = sx, x1 = sx, y0b = sy, y1b = sy;
                stack.assign(1, int(start));
                seen[start] = 1;
                while (!stack.empty()) {
                    int index = stack.back();
                    stack.pop_back();
                    int y = index / bw, x = index % bw;
                    x0 = std::min(x0, x); x1 = std::max(x1, x);
                    y0b = std::min(y0b, y); y1b = std::max(y1b, y);
                    const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
                    for (int k = 0; k < 4; ++k) {
                        int nx = x + dx[k], ny = y + dy[k];
                        if (nx < 0 || ny < 0 || nx >= bw || ny >= bh) continue;
                        size_t ni = size_t(ny) * bw + nx;
                        if (active[ni] && !seen[ni]) { seen[ni] = 1; stack.push_back(int(ni)); }
                    }
                }
                const int component_w = x1 - x0 + 1, component_h = y1b - y0b + 1;
                const int window_w = std::min(wb, component_w), window_h = std::min(hb, component_h);
                double best = -1;
                int best_x = x0, best_y = y0b;
                for (int wy = y0b; wy + window_h - 1 <= y1b; ++wy) {
                    for (int wx = x0; wx + window_w - 1 <= x1; ++wx) {
                        double sum = box_sum(wx, wy, wx + window_w - 1, wy + window_h - 1);
                        if (sum > best) { best = sum; best_x = wx; best_y = wy; }
                    }
                }
                Candidate candidate;
                candidate.band = band_index;
                candidate.x = best_x * kBlock;
                candidate.y = y0 + best_y * kBlock;
                candidate.w = window_w * kBlock;
                candidate.h = window_h * kBlock;
                candidate.blocks = window_w * window_h;
                candidate.magnitude = best / double(candidate.blocks);
                candidate.score = candidate.magnitude * std::sqrt(double(candidate.blocks));
                if (candidate.w < 24 || candidate.h < 12 || candidate.magnitude < 1.0) continue;
                // The matcher keeps a border for shifted comparisons.
                const int margin = 8;
                int left = std::max(candidate.x, margin), top = std::max(candidate.y, margin);
                int right = std::min(candidate.x + candidate.w, width_ - margin);
                int bottom = std::min(candidate.y + candidate.h, height_ - margin);
                if (right - left < 16 || bottom - top < 10) continue;
                candidate.x = left; candidate.y = top; candidate.w = right - left; candidate.h = bottom - top;
                out.push_back(candidate);
            }
        }
    }

    // Windows where the candidate's own structure is strongest are the best
    // reference crops; weak windows are more likely to be commercial breaks.
    void rank_windows(Candidate& candidate) {
        const Band& band = candidate.band == 0 ? top_ : bottom_;
        const int band_y = candidate.band == 0 ? top_y_ : bottom_y_;
        const int local_y = candidate.y - band_y;
        double template_energy = 0;
        for (int j = 0; j < candidate.h; ++j) {
            size_t base = size_t(local_y + j) * width_ + candidate.x;
            for (int i = 0; i < candidate.w; ++i) {
                double tx = band.gx[base + i] / band.samples, ty = band.gy[base + i] / band.samples;
                template_energy += tx * tx + ty * ty;
            }
        }
        std::vector<std::pair<double, int>> matches;
        for (size_t w = 0; w < windows_.size(); ++w) {
            const auto& window = windows_[w];
            if (!window.stored) continue;
            const std::vector<uint8_t>& source = candidate.band == 0 ? window.top : window.bottom;
            double dot = 0, energy = 0;
            for (int j = 0; j + 1 < candidate.h; ++j) {
                const uint8_t* row = &source[size_t(local_y + j) * width_ + candidate.x];
                const uint8_t* above = row - width_;
                const uint8_t* below = row + width_;
                size_t base = size_t(local_y + j) * width_ + candidate.x;
                for (int i = 1; i + 1 < candidate.w; ++i) {
                    double gx = double(row[i + 1]) - double(row[i - 1]);
                    double gy = double(below[i]) - double(above[i]);
                    double tx = band.gx[base + i] / band.samples, ty = band.gy[base + i] / band.samples;
                    dot += gx * tx + gy * ty;
                    energy += gx * gx + gy * gy;
                }
            }
            double cosine = (energy > 1e-9 && template_energy > 1e-9)
                                ? dot / std::sqrt(energy * template_energy) : 0.0;
            matches.emplace_back(cosine, int(w));
        }
        if (matches.empty()) return;
        std::sort(matches.begin(), matches.end(),
                  [](const std::pair<double, int>& a, const std::pair<double, int>& b) { return a.first > b.first; });
        double sum = 0;
        for (auto& match : matches) sum += match.first;
        candidate.consistency = sum / matches.size();
        // The weakest window separates a permanent mark from a changing telop.
        candidate.worst_consistency = matches.back().first;
        candidate.windows.clear();
        for (size_t k = 0; k < matches.size() && int(k) < references_; ++k)
            candidate.windows.push_back(matches[k].second);
    }

    void write_structure_band(const std::string& path, const Band& band, int h) const {
        std::vector<uint8_t> picture(size_t(width_) * h, 0);
        if (band.samples > 0) {
            for (size_t p = 0; p < picture.size(); ++p) {
                double mx = band.gx[p] / band.samples, my = band.gy[p] / band.samples;
                double stable = std::abs(mx) + std::abs(my);
                double magnitude = band.magnitude[p] / band.samples;
                double ratio = magnitude > 0 ? stable / magnitude : 0;
                double value = stable * 6 * std::max(0.0, ratio - 0.3);
                picture[p] = uint8_t(std::min(255.0, value));
            }
        }
        write_pgm(path, width_, h, picture.data());
    }

    int width_ = 0, height_ = 0;
    int top_y_ = 0, top_h_ = 0, bottom_y_ = 0, bottom_h_ = 0;
    int references_ = 4;
    double window_length_ = 8;
    Band top_, bottom_;
    std::vector<WindowFrame> windows_;
    std::vector<Candidate> chosen_;
    bool base_known_ = false;
    double base_ = 0;
};


// Measures a known overlay ROI while the single decode pass runs.
//
// cm-overlay is a second, independent full decode of the same file. The ROI and
// its reference crops are known before the scan starts (they come from the
// marker cache), so the measurement can ride along with the pass that already
// decodes every frame. The arithmetic is the one cm-overlay performs: accumulate
// the luma rows of the ROI plus a border, and once per second correlate the
// one-second average against the reference gradient template.
class OverlayMeasure {
  public:
    static constexpr int kPad = 6;

    struct Image {
        int w = 0, h = 0;
        std::vector<unsigned char> y;
    };

    OverlayMeasure(const std::vector<Image>& images, int roi_x, int roi_y, int roi_w, int roi_h,
                   int width, int height, const std::string& output_path)
        : x_(roi_x), y_(roi_y), w_(roi_w), h_(roi_h), width_(width), height_(height) {
        need(x_ >= kPad && y_ >= kPad && x_ + w_ + kPad < width_ && y_ + h_ + kPad < height_,
             "ROI outside frame");
        need(!images.empty(), "missing references");
        accum_.resize(size_t(w_ + 2 * kPad) * (h_ + 2 * kPad));
        // A reference may be a full-frame picture (scaled to the frame) or an
        // exact-size crop of this ROI, as produced by the overlay learner.
        auto at = [&](const Image& image, int px, int py) {
            if (image.w == w_ && image.h == h_) {
                const int lx = std::clamp(px - x_, 0, w_ - 1), ly = std::clamp(py - y_, 0, h_ - 1);
                return double(image.y[size_t(ly) * w_ + lx]);
            }
            const int sx = std::min(image.w - 1, int((px + 0.5) * image.w / width_));
            const int sy = std::min(image.h - 1, int((py + 0.5) * image.h / height_));
            return double(image.y[size_t(sy) * image.w + sx]);
        };
        std::vector<OverlayPatch> patches;
        patches.reserve(images.size());
        for (const auto& image : images) {
            OverlayPatch patch;
            patch.w = w_;
            patch.h = h_;
            patch.y.resize(size_t(w_) * h_);
            for (int j = 0; j < h_; ++j)
                for (int i = 0; i < w_; ++i)
                    patch.y[size_t(j) * w_ + i] =
                        (unsigned char)std::clamp(at(image, x_ + i, y_ + j), 0.0, 255.0);
            patches.push_back(std::move(patch));
        }
        mask_count_ = build_overlay_template(patches, w_, h_, reference_, template_energy_);
        need(mask_count_ >= kMinLogoMaskPixels && template_energy_ > 0,
             "insufficient reference logo edges");
        output_ = fopen(output_path.c_str(), "w");
        need(output_ != nullptr, "output file");
        fprintf(output_, "bin_start,samples,correlation,peak_margin,gradient_ratio\n");
        fprintf(stderr, "ROI=%d,%d,%d,%d reference_mask_pixels=%d\n", x_, y_, w_, h_, mask_count_);
    }

    ~OverlayMeasure() {
        flush();
        if (output_) fclose(output_);
    }

    void frame(double pts, const uint8_t* luma, int linesize) {
        const int next = int(std::floor(pts));
        if (next != bin_) {
            flush();
            bin_ = next;
        }
        const int stride = w_ + 2 * kPad;
        for (int j = 0; j < h_ + 2 * kPad; ++j) {
            const uint8_t* row = luma + size_t(y_ - kPad + j) * linesize + (x_ - kPad);
            for (int i = 0; i < stride; ++i) accum_[size_t(j) * stride + i] += row[i];
        }
        ++count_;
    }

  private:
    double correlate(int dx, int dy, double* ratio = nullptr) const {
        double dot = 0, energy = 0;
        const int stride = w_ + 2 * kPad;
        for (int j = 1; j < h_ - 1; ++j) {
            for (int i = 1; i < w_ - 1; ++i) {
                const int p = j * w_ + i;
                const double tx = reference_[size_t(p) * 2], ty = reference_[size_t(p) * 2 + 1];
                if (tx == 0 && ty == 0) continue;
                const int k = (j + kPad + dy) * stride + i + kPad + dx;
                const double gx = (accum_[size_t(k + 1)] - accum_[size_t(k - 1)]) / count_;
                const double gy = (accum_[size_t(k + stride)] - accum_[size_t(k - stride)]) / count_;
                dot += gx * tx + gy * ty;
                energy += gx * gx + gy * gy;
            }
        }
        if (ratio) *ratio = std::sqrt(energy / template_energy_);
        return energy > 1e-9 ? dot / std::sqrt(energy * template_energy_) : 0;
    }

    void flush() {
        if (!count_) return;
        double ratio;
        const double on = correlate(0, 0, &ratio);
        double other = -1;
        for (auto delta : std::vector<std::pair<int, int>>{{-3, 0}, {3, 0}, {0, -3}, {0, 3},
                                                           {-6, 0}, {6, 0}, {0, -6}, {0, 6}})
            other = std::max(other, correlate(delta.first, delta.second));
        fprintf(output_, "%d,%d,%.8f,%.8f,%.8f\n", bin_, count_, on, on - other, ratio);
        std::fill(accum_.begin(), accum_.end(), 0);
        count_ = 0;
    }

    int x_ = 0, y_ = 0, w_ = 0, h_ = 0, width_ = 0, height_ = 0;
    int bin_ = -1, count_ = 0, mask_count_ = 0;
    std::vector<double> reference_, accum_;
    double template_energy_ = 0;
    FILE* output_ = nullptr;
};

// NVDEC gate: GPU decoding is opportunistic and must never take the pipeline
// down. It runs only when the service enabled it (CM_GPU_DECODE), the card
// answers and is cool enough, and no other process holds the single-session
// lock -- four concurrent sessions are what dropped the card off the bus
// (Xid 79). Every refusal falls back to software decoding, which is always
// available and produces the reference output.
struct GpuGate {
    bool allowed = false;
    int fd = -1;
    GpuGate() = default;
    GpuGate(GpuGate&& other) noexcept : allowed(other.allowed), fd(other.fd) { other.fd = -1; }
    GpuGate& operator=(GpuGate&& other) noexcept {
        if (this != &other) {
            if (fd >= 0) close(fd);
            allowed = other.allowed;
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }
    GpuGate(const GpuGate&) = delete;
    GpuGate& operator=(const GpuGate&) = delete;
    ~GpuGate() {
        if (fd >= 0) close(fd);
    }
};

inline GpuGate acquire_gpu() {
    GpuGate gate;
    if (!std::getenv("CM_GPU_DECODE")) return gate;  // not enabled: no GPU contact at all
    gate.fd = open("/dev/shm/cm-gpu.lock", O_CREAT | O_RDWR, 0666);
    if (gate.fd < 0) {
        fprintf(stderr, "gpu gate: cannot open lock file: %s; using software\n", strerror(errno));
        gate.fd = -1;
        return gate;
    }
    if (flock(gate.fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "gpu gate: another decoder holds the GPU; using software\n");
        close(gate.fd);
        gate.fd = -1;
        return gate;
    }
    int worst = -1;
    if (FILE* probe = popen("nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader,nounits 2>/dev/null", "r")) {
        char line[32];
        while (fgets(line, sizeof(line), probe)) worst = std::max(worst, atoi(line));
        pclose(probe);
    }
    int limit = 85;
    if (const char* limit_text = std::getenv("CM_GPU_MAX_TEMP")) {
        const int value = atoi(limit_text);
        if (value > 0) limit = value;
    }
    if (worst < 0) {
        fprintf(stderr, "gpu gate: GPU does not answer; using software\n");
        close(gate.fd);
        gate.fd = -1;
        return gate;
    }
    if (worst >= limit) {
        fprintf(stderr, "gpu gate: GPU at %dC >= %dC; using software\n", worst, limit);
        close(gate.fd);
        gate.fd = -1;
        return gate;
    }
    gate.allowed = true;
    return gate;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 4) {
            fprintf(stderr,
                    "usage: cm-scan input.fmp4 output-directory threads --duration SECONDS "
                    "[--windows N] [--window-length S] [--references K]\n"
                    "       [--overlay-roi X,Y,W,H --overlay-references PGM;PGM --overlay-output CSV]\n");
            return 2;
        }
        const char* input = argv[1];
        const std::string dir = argv[2];
        const int threads = std::atoi(argv[3]);
        double duration = 0;
        int window_count = 16, references = 4;
        double window_length = 8.0;
        // Optional inline overlay measurement: the ROI and its reference crops
        // are known from the marker cache before the pass starts, so the
        // measurement rides along instead of decoding the file a second time.
        std::string overlay_references, overlay_output;
        int overlay_roi[4] = {0, 0, 0, 0};
        bool has_overlay = false;
        for (int i = 4; i < argc; ++i) {
            std::string flag = argv[i];
            need(i + 1 < argc, "missing option value");
            std::string value = argv[++i];
            if (flag == "--duration") duration = std::atof(value.c_str());
            else if (flag == "--windows") window_count = std::atoi(value.c_str());
            else if (flag == "--window-length") window_length = std::atof(value.c_str());
            else if (flag == "--references") references = std::atoi(value.c_str());
            else if (flag == "--overlay-roi") {
                // x,y,width,height in one argument keeps the call site short.
                char comma = 0;
                int parsed = sscanf(value.c_str(), "%d,%d,%d,%d%c", &overlay_roi[0], &overlay_roi[1],
                                    &overlay_roi[2], &overlay_roi[3], &comma);
                need(parsed == 4, "overlay ROI must be x,y,width,height");
                has_overlay = true;
            } else if (flag == "--overlay-references") overlay_references = value;
            else if (flag == "--overlay-output") overlay_output = value;
            else need(false, "unknown option");
        }
        if (has_overlay)
            need(!overlay_references.empty() && !overlay_output.empty(),
                 "overlay ROI needs references and an output");
        need(threads > 0 && threads <= 16, "thread count out of range");
        need(window_count >= 2 && window_count <= 64, "window count out of range");
        need(references >= 1 && references <= 8, "reference count out of range");

        AVFormatContext* format = nullptr;
        need(avformat_open_input(&format, input, nullptr, nullptr) >= 0, "open input");
        need(avformat_find_stream_info(format, nullptr) >= 0, "read stream information");
        int video_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        int audio_index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        need(video_index >= 0 && audio_index >= 0, "video and audio streams required");
        if (duration <= 0 && format->duration > 0) duration = format->duration / double(AV_TIME_BASE);
        need(duration > 0, "unknown duration; pass --duration");
        const int width = format->streams[video_index]->codecpar->width;
        const int height = format->streams[video_index]->codecpar->height;
        need(width > 0 && height > 0, "unknown frame size");

        AVCodecContext* video_codec = nullptr;
        AVCodecContext* audio_codec = nullptr;
        // NVDEC offload: when the gate allows it and the *_cuvid decoder opens,
        // the GPU decodes and hands over NV12 luma in system memory, and the
        // CPU only feeds packets and reads the luma plane back. The software
        // decoder remains the fallback for every refusal reason.
        AVCodecID video_id = format->streams[video_index]->codecpar->codec_id;
        static const std::map<AVCodecID, const char*> kCuvid = {
            {AV_CODEC_ID_H264, "h264_cuvid"},   {AV_CODEC_ID_HEVC, "hevc_cuvid"},
            {AV_CODEC_ID_MPEG2VIDEO, "mpeg2_cuvid"}, {AV_CODEC_ID_VC1, "vc1_cuvid"},
        };
        auto found = kCuvid.find(video_id);
        // The gate is only claimed for codecs the GPU can actually decode, so
        // it never sits idle between other workers and the card.
        std::unique_ptr<GpuGate> gpu;
        if (found != kCuvid.end()) gpu.reset(new GpuGate(acquire_gpu()));
        if (found != kCuvid.end() && gpu->allowed) {
            const AVCodec* hw = avcodec_find_decoder_by_name(found->second);
            if (hw) {
                AVCodecContext* ctx = avcodec_alloc_context3(hw);
                if (ctx && avcodec_parameters_to_context(ctx, format->streams[video_index]->codecpar) >= 0 &&
                    avcodec_open2(ctx, hw, nullptr) >= 0) {
                    video_codec = ctx;
                    fprintf(stderr, "video decoder: %s (NVDEC)\n", found->second);
                } else {
                    avcodec_free_context(&ctx);
                }
            }
        }
        for (auto item : {std::make_pair(video_index, &video_codec), std::make_pair(audio_index, &audio_codec)}) {
            if (*item.second) continue;  // already opened (NVDEC video path)
            const AVCodec* codec = avcodec_find_decoder(format->streams[item.first]->codecpar->codec_id);
            need(codec != nullptr, "decoder unavailable");
            *item.second = avcodec_alloc_context3(codec);
            need(*item.second != nullptr, "allocate codec context");
            need(avcodec_parameters_to_context(*item.second, format->streams[item.first]->codecpar) >= 0,
                 "copy codec parameters");
            (*item.second)->thread_count = threads;
            need(avcodec_open2(*item.second, codec, nullptr) >= 0, "open decoder");
        }

        Features features(dir);
        OverlayLearn learn(width, height, duration, window_count, window_length, references);

        SwsContext* sws = nullptr;
        SwrContext* swr = nullptr;
        AVFrame* frame = av_frame_alloc();
        AVPacket* packet = av_packet_alloc();
        need(frame && packet, "allocate frame");
        long audio_packets_failed = 0, video_packets_failed = 0, video_packets = 0;
        long audio_decode_errors = 0, video_decode_errors = 0;
        std::vector<uint8_t> small(320 * 180);
        std::vector<uint8_t> full(size_t(width) * height);

        std::unique_ptr<OverlayMeasure> overlay;
        if (has_overlay) {
            std::vector<OverlayMeasure::Image> images;
            std::stringstream paths(overlay_references);
            std::string path;
            while (std::getline(paths, path, ';')) {
                OverlayMeasure::Image image;
                std::ifstream file(path, std::ios::binary);
                std::string magic;
                int maximum = 0;
                file >> magic >> image.w >> image.h >> maximum;
                need(magic == "P5" && maximum == 255, "reference requires 8-bit P5 PGM");
                file.get();
                image.y.resize(size_t(image.w) * image.h);
                file.read(reinterpret_cast<char*>(image.y.data()), std::streamsize(image.y.size()));
                need(bool(file), "reference image truncated");
                images.push_back(std::move(image));
            }
            overlay.reset(new OverlayMeasure(images, overlay_roi[0], overlay_roi[1], overlay_roi[2],
                                             overlay_roi[3], width, height, overlay_output));
        }

        auto drain = [&](AVCodecContext* codec, int index) {
            int rc;
            while ((rc = avcodec_receive_frame(codec, frame)) >= 0) {
                if (frame->best_effort_timestamp == AV_NOPTS_VALUE) {
                    if (index == video_index) need(false, "frame without timestamp");
                    av_frame_unref(frame);
                    continue;
                }
                double pts = frame->best_effort_timestamp * av_q2d(format->streams[index]->time_base);
                if (index == video_index) {
                    // Both yuv420p (software) and NV12 (cuvid) keep 8-bit luma
                    // in data[0] with a linesize stride, which is all the
                    // feature extraction and the overlay matcher read.
                    need(frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_NV12,
                         "prototype requires 8-bit yuv420p or nv12 luma");
                    sws = sws_getCachedContext(sws, frame->width, frame->height, AV_PIX_FMT_GRAY8, 320, 180,
                                               AV_PIX_FMT_GRAY8, SWS_POINT, nullptr, nullptr, nullptr);
                    need(sws != nullptr, "scale context");
                    uint8_t* dest[] = {small.data()};
                    int strides[] = {320};
                    need(sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dest, strides) == 180,
                         "scale frame");
                    features.video_frame(pts, small);
                    sws = sws_getCachedContext(sws, frame->width, frame->height, AV_PIX_FMT_GRAY8, width, height,
                                               AV_PIX_FMT_GRAY8, SWS_POINT, nullptr, nullptr, nullptr);
                    need(sws != nullptr, "scale context");
                    uint8_t* full_dest[] = {full.data()};
                    int full_strides[] = {width};
                    need(sws_scale(sws, frame->data, frame->linesize, 0, frame->height, full_dest, full_strides) == height,
                         "scale full frame");
                    learn.frame(pts, full.data());
                    if (overlay) overlay->frame(pts, frame->data[0], frame->linesize[0]);
                } else {
                    if (!swr) {
                        need(swr_alloc_set_opts2(&swr, &frame->ch_layout, AV_SAMPLE_FMT_S16, frame->sample_rate,
                                                 &frame->ch_layout, (AVSampleFormat)frame->format,
                                                 frame->sample_rate, 0, nullptr) >= 0, "resample config");
                        need(swr_init(swr) >= 0, "resample init");
                    }
                    std::vector<int16_t> pcm((frame->nb_samples + 256) * frame->ch_layout.nb_channels);
                    uint8_t* dest[] = {(uint8_t*)pcm.data()};
                    int got = swr_convert(swr, dest, frame->nb_samples + 256,
                                          (const uint8_t**)frame->extended_data, frame->nb_samples);
                    need(got >= 0, "resample");
                    features.audio_frame(pts, pcm.data(), got, frame->ch_layout.nb_channels);
                }
                av_frame_unref(frame);
            }
            // Decoder errors are counted, not fatal: a broken packet or an AAC
            // stream beyond the decoder's limits must not stop detection.
            if (rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) {
                if (index == audio_index) ++audio_decode_errors;
                else ++video_decode_errors;
            }
        };
        int rc;
        while ((rc = av_read_frame(format, packet)) >= 0) {
            if (packet->stream_index == video_index || packet->stream_index == audio_index) {
                if (packet->stream_index == video_index) ++video_packets;
                AVCodecContext* codec = packet->stream_index == video_index ? video_codec : audio_codec;
                // A damaged packet must not throw away the whole measurement:
                // the recording is usually usable except for the tail.
                if (avcodec_send_packet(codec, packet) < 0) {
                    if (packet->stream_index == audio_index) ++audio_packets_failed;
                    else ++video_packets_failed;
                    av_packet_unref(packet);
                    continue;
                }
                drain(codec, packet->stream_index);
            }
            av_packet_unref(packet);
        }
        need(rc == AVERROR_EOF, "read frame");
        need(avcodec_send_packet(video_codec, nullptr) >= 0, "flush video");
        drain(video_codec, video_index);
        need(avcodec_send_packet(audio_codec, nullptr) >= 0, "flush audio");
        drain(audio_codec, audio_index);

        // Damage is tolerated, but a recording that decodes to nothing has to be
        // reported as a decode failure, not as "no marker found".
        if (features.frames == 0 || (video_packets > 0 && video_packets_failed * 2 > video_packets)) {
            fprintf(stderr, "video could not be decoded: %ld of %ld packets rejected, %ld frames\n",
                    video_packets_failed, video_packets, features.frames);
            return 1;
        }
        overlay.reset();
        learn.candidates();
        learn.write_structure(dir);
        std::string json = "{\n  \"version\": 1,\n  \"width\": " + std::to_string(width) +
                           ",\n  \"height\": " + std::to_string(height) +
                           ",\n  \"duration_seconds\": " + std::to_string(duration) +
                           ",\n  \"windows\": " + std::to_string(learn.window_count()) +
                           ",\n  \"video_frames\": " + std::to_string(features.frames) +
                           ",\n  \"audio_decode_failures\": " + std::to_string(audio_packets_failed) +
                           ",\n  \"video_decode_failures\": " + std::to_string(video_packets_failed) +
                           ",\n  \"video_decode_errors\": " + std::to_string(video_decode_errors) +
                           ",\n  \"candidates\": [";
        learn.export_references(dir);
        const std::vector<Candidate>& candidates = learn.chosen_candidates();
        for (size_t i = 0; i < candidates.size(); ++i) {
            const Candidate& candidate = candidates[i];
            json += i ? ",\n    " : "\n    ";
            json += "{\"index\": " + std::to_string(i);
            json += ", \"band\": \"" + std::string(candidate.band == 0 ? "top" : "bottom") + "\"";
            json += ", \"roi\": [" + std::to_string(candidate.x) + ", " + std::to_string(candidate.y) + ", " +
                    std::to_string(candidate.w) + ", " + std::to_string(candidate.h) + "]";
            json += ", \"score\": " + std::to_string(candidate.score);
            json += ", \"magnitude\": " + std::to_string(candidate.magnitude);
            json += ", \"consistency\": " + std::to_string(candidate.consistency);
            json += ", \"worst_consistency\": " + std::to_string(candidate.worst_consistency);
            json += ", \"blocks\": " + std::to_string(candidate.blocks);
            json += ", \"windows\": [";
            for (size_t k = 0; k < candidate.windows.size(); ++k)
                json += (k ? ", " : "") + std::to_string(candidate.windows[k]);
            json += "], \"references\": [";
            for (size_t k = 0; k < candidate.references.size(); ++k)
                json += (k ? ", " : "") + ("\"" + candidate.references[k] + "\"");
            json += "]}";
        }
        json += "\n  ]\n}\n";
        std::string json_path = dir + "/logo.json";
        FILE* out = fopen(json_path.c_str(), "wb");
        need(out != nullptr, "write logo.json");
        need(fwrite(json.data(), 1, json.size(), out) == json.size(), "write logo.json");
        need(fclose(out) == 0, "close logo.json");

        sws_freeContext(sws);
        swr_free(&swr);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&video_codec);
        avcodec_free_context(&audio_codec);
        avformat_close_input(&format);
        fprintf(stderr, "video_frames=%ld audio_frames=%ld candidates=%zu windows=%d\n",
                features.frames, features.audio_frames, candidates.size(), learn.window_count());
        return 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        return 1;
    }
}
