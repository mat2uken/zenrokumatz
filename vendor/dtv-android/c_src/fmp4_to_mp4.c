#include "fmp4_to_mp4.h"
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define SCALE 1000000

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
    int failed;
} Box;

typedef struct {
    int is_audio;
    uint32_t size;
    uint32_t duration_us;
    int64_t pts_us;
    int64_t dts_us;
    int is_key;
    uint64_t in_offset;  /* 入力ファイル内の実データオフセット */
    uint64_t out_offset; /* 出力ファイル内の実データオフセット */
} SampleEntry;

/* ---- バイト操作ヘルパ ---- */

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}

static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t get64(const uint8_t *p) {
    return ((uint64_t)get32(p) << 32) | (uint64_t)get32(p + 4);
}

static void box_bytes(Box *b, const void *p, size_t n) {
    if (b->failed || !n) return;
    if (b->size + n > b->capacity) {
        size_t cap = b->capacity ? b->capacity * 2 : 1024;
        while (cap < b->size + n) cap *= 2;
        uint8_t *d = realloc(b->data, cap);
        if (!d) { b->failed = 1; return; }
        b->data = d;
        b->capacity = cap;
    }
    memcpy(b->data + b->size, p, n);
    b->size += n;
}

static void box_zero(Box *b, size_t n) {
    static const uint8_t zeros[64] = {0};
    while (n) {
        size_t c = n > sizeof(zeros) ? sizeof(zeros) : n;
        box_bytes(b, zeros, c);
        n -= c;
    }
}

static void box_u8(Box *b, uint8_t v) { box_bytes(b, &v, 1); }
static void box_u16(Box *b, uint16_t v) { uint8_t p[2]; put16(p, v); box_bytes(b, p, 2); }
static void box_u32(Box *b, uint32_t v) { uint8_t p[4]; put32(p, v); box_bytes(b, p, 4); }
static void box_u64(Box *b, uint64_t v) { uint8_t p[8]; put64(p, v); box_bytes(b, p, 8); }

static size_t box_begin(Box *b, const char type[4]) {
    size_t at = b->size;
    box_u32(b, 0);
    box_bytes(b, type, 4);
    return at;
}

static void box_end(Box *b, size_t at) {
    if (!b->failed && b->data) {
        put32(b->data + at, (uint32_t)(b->size - at));
    }
}

static void box_matrix(Box *b) {
    for (int i = 0; i < 9; i++) {
        box_u32(b, (i == 0 || i == 4) ? 0x00010000 : (i == 8) ? 0x40000000 : 0);
    }
}

/* ---- 再構成処理 ---- */

typedef struct {
    uint8_t *stsd_box;
    size_t stsd_size;
    int width, height;
    int sample_rate, channels;
} TrackConfig;

int fmp4_to_mp4_finalize(const char *in_fmp4_path, const char *out_mp4_path, int remove_input) {
    return fmp4_to_mp4_finalize_ex(in_fmp4_path, out_mp4_path, remove_input, NULL, NULL, NULL);
}

int fmp4_to_mp4_finalize_ex(const char *in_fmp4_path, const char *out_mp4_path,
                            int remove_input,
                            int64_t *out_duration_ms, int64_t *out_samples,
                            int64_t *out_video_samples) {
    if (!in_fmp4_path || !out_mp4_path) return -1;

    if (out_duration_ms) *out_duration_ms = 0;
    if (out_samples) *out_samples = 0;
    if (out_video_samples) *out_video_samples = 0;

    FILE *fin = fopen(in_fmp4_path, "rb");
    if (!fin) return -1;

    TrackConfig configs[2] = {0}; /* 0: video, 1: audio */
    SampleEntry *samples[2] = {NULL, NULL};
    size_t sample_counts[2] = {0, 0};
    size_t sample_caps[2] = {0, 0};

    uint8_t hdr[8];
    uint64_t cur_pos = 0;

    /*
     * Integrity check. A connection can end in the middle of a fragment, which
     * leaves a moof whose mdat is short. Those samples would point past the end
     * of real data, so the group is dropped instead of being published.
     */
    size_t moof_mark[2] = {0, 0};
    uint64_t moof_expected = 0;
    int moof_pending = 0;

    /* 1. 入力 fMP4 のトップレベルボックスを走査 */
    while (fread(hdr, 1, 8, fin) == 8) {
        uint64_t size = get32(hdr);
        char type[5] = {0};
        memcpy(type, hdr + 4, 4);
        uint64_t hdr_len = 8;
        if (size == 1) {
            uint8_t ext[8];
            if (fread(ext, 1, 8, fin) != 8) break;
            size = get64(ext);
            hdr_len = 16;
        } else if (size == 0) {
            break;
        }

        uint64_t box_start = cur_pos;
        uint64_t box_payload_len = size - hdr_len;

        if (strcmp(type, "moov") == 0) {
            /* moov 内の stsd を読み取り */
            uint8_t *moov_buf = malloc((size_t)box_payload_len);
            if (!moov_buf || fread(moov_buf, 1, (size_t)box_payload_len, fin) != box_payload_len) {
                free(moov_buf);
                break;
            }
            /* 簡易走査: 'stsd' を探す */
            for (size_t i = 0; i + 8 <= (size_t)box_payload_len; i++) {
                if (memcmp(moov_buf + i + 4, "stsd", 4) == 0) {
                    uint32_t stsd_len = get32(moov_buf + i);
                    if (i + stsd_len <= (size_t)box_payload_len) {
                        /*
                         * stsd layout: size(4) type(4) version+flags(4) entry_count(4)
                         * then the first sample entry: size(4) format(4).
                         * The format code is therefore at offset 20 from the box start.
                         */
                        int is_aud = (i + 24 <= (size_t)box_payload_len)
                                && (memcmp(moov_buf + i + 20, "mp4a", 4) == 0);
                        TrackConfig *cfg = &configs[is_aud ? 1 : 0];
                        if (!cfg->stsd_box) {
                            cfg->stsd_box = malloc(stsd_len);
                            if (cfg->stsd_box) {
                                memcpy(cfg->stsd_box, moov_buf + i, stsd_len);
                                cfg->stsd_size = stsd_len;
                            }
                        }
                    }
                }
                /* tkhd から解像度取得 */
                if (memcmp(moov_buf + i + 4, "tkhd", 4) == 0) {
                    uint32_t tkhd_len = get32(moov_buf + i);
                    if (i + tkhd_len <= (size_t)box_payload_len && tkhd_len >= 84) {
                        uint32_t w = get32(moov_buf + i + tkhd_len - 8) >> 16;
                        uint32_t h = get32(moov_buf + i + tkhd_len - 4) >> 16;
                        if (w > 0 && h > 0) {
                            configs[0].width = (int)w;
                            configs[0].height = (int)h;
                        }
                    }
                }
            }
            free(moov_buf);
        } else if (strcmp(type, "moof") == 0) {
            if (moof_pending) {
                /* The previous fragment never got its mdat: roll it back. */
                sample_counts[0] = moof_mark[0];
                sample_counts[1] = moof_mark[1];
                fprintf(stderr, "dropped fragment with a missing mdat\n");
            }
            moof_mark[0] = sample_counts[0];
            moof_mark[1] = sample_counts[1];
            moof_expected = 0;
            moof_pending = 1;
            /* moof のパース */
            uint8_t *moof_buf = malloc((size_t)box_payload_len);
            if (!moof_buf || fread(moof_buf, 1, (size_t)box_payload_len, fin) != box_payload_len) {
                free(moof_buf);
                break;
            }

            /* 各 traf を走査 */
            size_t p = 0;
            while (p + 8 <= (size_t)box_payload_len) {
                uint32_t sub_len = get32(moof_buf + p);
                if (sub_len < 8 || p + sub_len > (size_t)box_payload_len) break;
                if (memcmp(moof_buf + p + 4, "traf", 4) == 0) {
                    uint32_t track_id = 1;
                    uint64_t base_dts = 0;
                    uint32_t data_offset = 0;

                    size_t tp = p + 8;
                    size_t traf_end = p + sub_len;

                    while (tp + 8 <= traf_end) {
                        uint32_t tlen = get32(moof_buf + tp);
                        if (tlen < 8 || tp + tlen > traf_end) break;
                        const char *ttype = (const char *)moof_buf + tp + 4;

                        if (memcmp(ttype, "tfhd", 4) == 0 && tlen >= 16) {
                            track_id = get32(moof_buf + tp + 12);
                        } else if (memcmp(ttype, "tfdt", 4) == 0 && tlen >= 20) {
                            uint8_t v = moof_buf[tp + 8];
                            if (v == 1) base_dts = get64(moof_buf + tp + 12);
                            else base_dts = get32(moof_buf + tp + 12);
                        } else if (memcmp(ttype, "trun", 4) == 0 && tlen >= 16) {
                            uint8_t v = moof_buf[tp + 8];
                            uint32_t flags = (get32(moof_buf + tp + 8) & 0x00ffffff);
                            uint32_t count = get32(moof_buf + tp + 12);

                            size_t ep = tp + 16;
                            if (flags & 0x000001) { /* data-offset */
                                data_offset = get32(moof_buf + ep);
                                ep += 4;
                            }
                            if (flags & 0x000004) ep += 4; /* first-sample-flags */

                            int is_aud = (track_id == 2);
                            uint64_t cur_dts = base_dts;
                            uint64_t sample_in_offset = box_start + data_offset;

                            for (uint32_t sidx = 0; sidx < count; sidx++) {
                                uint32_t dur = 40000, sz = 0, sflags = 0;
                                int32_t cto = 0;
                                if (flags & 0x000100) { dur = get32(moof_buf + ep); ep += 4; }
                                if (flags & 0x000200) { sz = get32(moof_buf + ep); ep += 4; }
                                if (flags & 0x000400) { sflags = get32(moof_buf + ep); ep += 4; }
                                if (flags & 0x000800) {
                                    if (v == 1) cto = (int32_t)get32(moof_buf + ep);
                                    else cto = (int32_t)(uint32_t)get32(moof_buf + ep);
                                    ep += 4;
                                }

                                int idx = is_aud ? 1 : 0;
                                if (sample_counts[idx] == sample_caps[idx]) {
                                    size_t cap = sample_caps[idx] ? sample_caps[idx] * 2 : 1024;
                                    SampleEntry *se = realloc(samples[idx], cap * sizeof(SampleEntry));
                                    if (!se) break;
                                    samples[idx] = se;
                                    sample_caps[idx] = cap;
                                }

                                SampleEntry *entry = &samples[idx][sample_counts[idx]++];
                                entry->is_audio = is_aud;
                                entry->size = sz;
                                moof_expected += sz;
                                entry->duration_us = dur;
                                entry->dts_us = (int64_t)cur_dts;
                                entry->pts_us = (int64_t)cur_dts + cto;
                                entry->is_key = (sflags & 0x02000000) != 0;
                                entry->in_offset = sample_in_offset;

                                cur_dts += dur;
                                sample_in_offset += sz;
                            }
                        }
                        tp += tlen;
                    }
                }
                p += sub_len;
            }
            free(moof_buf);
        } else {
            if (moof_pending && strcmp(type, "mdat") == 0) {
                if (box_payload_len < moof_expected) {
                    sample_counts[0] = moof_mark[0];
                    sample_counts[1] = moof_mark[1];
                    fprintf(stderr,
                            "dropped fragment: mdat holds %" PRIu64 " bytes, samples need %" PRIu64 "\n",
                            box_payload_len, moof_expected);
                }
                moof_pending = 0;
            }
            /* 他のボックス (mdat 等) は読み飛ばす */
            fseek(fin, (long)box_payload_len, SEEK_CUR);
        }
        cur_pos = box_start + size;
    }

    if (moof_pending) {
        sample_counts[0] = moof_mark[0];
        sample_counts[1] = moof_mark[1];
        fprintf(stderr, "dropped trailing fragment without an mdat\n");
    }

    /*
     * Refuse to build a file whose tracks cannot be described. Without this the
     * writer would emit an empty moov next to a full mdat, which plays as a
     * broken recording.
     */
    for (int k = 0; k < 2; k++) {
        if (sample_counts[k] > 0 && !configs[k].stsd_box) {
            fprintf(stderr, "refusing to finalize: track %d has %zu samples but no stsd\n",
                    k, sample_counts[k]);
            fclose(fin);
            for (int j = 0; j < 2; j++) { free(configs[j].stsd_box); free(samples[j]); }
            return -1;
        }
    }
    if (sample_counts[0] == 0) {
        fclose(fin);
        for (int k = 0; k < 2; k++) { free(configs[k].stsd_box); free(samples[k]); }
        return -1;
    }

    /*
     * The fragments carry absolute timestamps from the encoder session, so a
     * segment that starts later would carry a non-zero start time and a
     * duration that includes the gap before it. Rebase every timestamp to
     * zero so the file stands on its own.
     */
    int64_t base_pts = INT64_MAX;
    for (int k = 0; k < 2; k++) {
        if (sample_counts[k] == 0) continue;
        for (size_t i = 0; i < sample_counts[k]; i++) {
            if (samples[k][i].pts_us < base_pts) base_pts = samples[k][i].pts_us;
            if (samples[k][i].dts_us < base_pts) base_pts = samples[k][i].dts_us;
        }
    }
    if (base_pts == INT64_MAX) base_pts = 0;
    for (int k = 0; k < 2; k++) {
        for (size_t i = 0; i < sample_counts[k]; i++) {
            samples[k][i].pts_us -= base_pts;
            samples[k][i].dts_us -= base_pts;
        }
    }

    FILE *fout = fopen(out_mp4_path, "wb");
    if (!fout) {
        fclose(fin);
        for (int k = 0; k < 2; k++) { free(configs[k].stsd_box); free(samples[k]); }
        return -1;
    }

    /* ftyp: the compatible brands must name the codec the file really uses. */
    uint8_t ftyp[] = {
        0,0,0,32, 'f','t','y','p', 'i','s','o','m', 0,0,2,0,
        'i','s','o','m','i','s','o','2','a','v','c','1','m','p','4','1'
    };
    {
        int hevc = 0;
        for (int k = 0; k < 2 && !hevc; k++) {
            if (sample_counts[k] > 0 && configs[k].stsd_box
                && configs[k].stsd_size >= 24
                && (memcmp(configs[k].stsd_box + 20, "hvc1", 4) == 0
                    || memcmp(configs[k].stsd_box + 20, "hev1", 4) == 0)) {
                hevc = 1;
            }
        }
        if (hevc) {
            memcpy(ftyp + 8, "hvc1", 4);   /* major_brand */
            memcpy(ftyp + 24, "hvc1", 4);  /* compatible brand */
        }
    }
    fwrite(ftyp, 1, sizeof(ftyp), fout);

    /* moov の計算・構築 */
    uint64_t max_duration = 0;
    for (int k = 0; k < 2; k++) {
        if (sample_counts[k] > 0) {
            SampleEntry *last = &samples[k][sample_counts[k] - 1];
            uint64_t d = (uint64_t)(last->pts_us + last->duration_us);
            if (d > max_duration) max_duration = d;
        }
    }

    /* 2パス構築: 1パス目で moov_size を確定し、2パス目で co64 に正確な offset を入れる */
    size_t moov_size = 0;
    uint8_t *moov_data = NULL;

    for (int pass = 0; pass < 2; pass++) {
        Box b = {0};
        size_t moov = box_begin(&b, "moov");

        /* mvhd */
        size_t mvhd = box_begin(&b, "mvhd");
        box_u32(&b, 0x01000000); /* version 1 (64bit) */
        box_zero(&b, 16);
        box_u32(&b, SCALE);
        box_u64(&b, max_duration);
        box_u32(&b, 0x00010000);
        box_u16(&b, 0x0100);
        box_zero(&b, 10);
        box_matrix(&b);
        box_zero(&b, 24);
        box_u32(&b, sample_counts[1] > 0 ? 3 : 2);
        box_end(&b, mvhd);

        /* 各トラック */
        for (int k = 0; k < 2; k++) {
            if (sample_counts[k] == 0) continue;
            if (!configs[k].stsd_box) {
                fprintf(stderr, "track %d has samples but no stsd, skipping\n", k);
                continue;
            }
            int is_aud = k;
            size_t count = sample_counts[k];
            SampleEntry *arr = samples[k];

            size_t trak = box_begin(&b, "trak");
            size_t tkhd = box_begin(&b, "tkhd");
            box_u32(&b, 0x01000007);
            box_zero(&b, 16);
            box_u32(&b, is_aud ? 2 : 1);
            box_u32(&b, 0);
            box_u64(&b, max_duration);
            box_zero(&b, 8);
            box_u16(&b, 0); box_u16(&b, 0);
            box_u16(&b, is_aud ? 0x0100 : 0);
            box_u16(&b, 0);
            box_matrix(&b);
            box_u32(&b, is_aud ? 0 : (uint32_t)configs[0].width << 16);
            box_u32(&b, is_aud ? 0 : (uint32_t)configs[0].height << 16);
            box_end(&b, tkhd);

            size_t mdia = box_begin(&b, "mdia");
            size_t mdhd = box_begin(&b, "mdhd");
            box_u32(&b, 0x01000000);
            box_zero(&b, 16);
            box_u32(&b, SCALE);
            box_u64(&b, max_duration);
            box_u16(&b, 0x55c4); box_u16(&b, 0);
            box_end(&b, mdhd);

            size_t hdlr = box_begin(&b, "hdlr");
            box_u32(&b, 0); box_u32(&b, 0);
            box_bytes(&b, is_aud ? "soun" : "vide", 4);
            box_zero(&b, 12);
            box_bytes(&b, is_aud ? "SoundHandler" : "VideoHandler", 12);
            box_u8(&b, 0);
            box_end(&b, hdlr);

            size_t minf = box_begin(&b, "minf");
            if (is_aud) {
                size_t smhd = box_begin(&b, "smhd");
                box_u32(&b, 0); box_zero(&b, 4);
                box_end(&b, smhd);
            } else {
                size_t vmhd = box_begin(&b, "vmhd");
                box_u32(&b, 1); box_zero(&b, 8);
                box_end(&b, vmhd);
            }

            size_t dinf = box_begin(&b, "dinf");
            size_t dref = box_begin(&b, "dref");
            box_u32(&b, 0); box_u32(&b, 1);
            size_t url = box_begin(&b, "url ");
            box_u32(&b, 1);
            box_end(&b, url);
            box_end(&b, dref);
            box_end(&b, dinf);

            size_t stbl = box_begin(&b, "stbl");
            if (configs[k].stsd_box) {
                box_bytes(&b, configs[k].stsd_box, configs[k].stsd_size);
            }

            /* stts (ランレングス圧縮) */
            size_t stts = box_begin(&b, "stts");
            box_u32(&b, 0);
            size_t stts_run_at = b.size;
            box_u32(&b, 0);
            uint32_t stts_runs = 0;
            for (size_t i = 0; i < count;) {
                size_t j = i + 1;
                while (j < count && arr[j].duration_us == arr[i].duration_us) j++;
                box_u32(&b, (uint32_t)(j - i));
                box_u32(&b, arr[i].duration_us);
                stts_runs++;
                i = j;
            }
            put32(b.data + stts_run_at, stts_runs);
            box_end(&b, stts);

            /* ctts (CTSオフセット、ビデオのみ) */
            if (!is_aud) {
                size_t ctts = box_begin(&b, "ctts");
                box_u32(&b, 0x01000000); /* version 1 (signed int32) */
                size_t ctts_run_at = b.size;
                box_u32(&b, 0);
                uint32_t ctts_runs = 0;
                for (size_t i = 0; i < count;) {
                    int32_t cto = (int32_t)(arr[i].pts_us - arr[i].dts_us);
                    size_t j = i + 1;
                    while (j < count && (int32_t)(arr[j].pts_us - arr[j].dts_us) == cto) j++;
                    box_u32(&b, (uint32_t)(j - i));
                    box_u32(&b, (uint32_t)cto);
                    ctts_runs++;
                    i = j;
                }
                put32(b.data + ctts_run_at, ctts_runs);
                box_end(&b, ctts);
            }

            /* stsc (1サンプル/チャンク固定) */
            size_t stsc = box_begin(&b, "stsc");
            box_u32(&b, 0); box_u32(&b, 1);
            box_u32(&b, 1); box_u32(&b, 1); box_u32(&b, 1);
            box_end(&b, stsc);

            /* stsz (各サンプルサイズ) */
            size_t stsz = box_begin(&b, "stsz");
            box_u32(&b, 0); box_u32(&b, 0);
            box_u32(&b, (uint32_t)count);
            for (size_t i = 0; i < count; i++) {
                box_u32(&b, arr[i].size);
            }
            box_end(&b, stsz);

            /* co64 (64bit チャンクオフセット) */
            size_t co64 = box_begin(&b, "co64");
            box_u32(&b, 0);
            box_u32(&b, (uint32_t)count);
            for (size_t i = 0; i < count; i++) {
                box_u64(&b, arr[i].out_offset);
            }
            box_end(&b, co64);

            /* stss (キーフレーム一覧、ビデオのみ) */
            if (!is_aud) {
                size_t stss = box_begin(&b, "stss");
                box_u32(&b, 0);
                size_t stss_count_at = b.size;
                box_u32(&b, 0);
                uint32_t n_keys = 0;
                for (size_t i = 0; i < count; i++) {
                    if (arr[i].is_key) {
                        box_u32(&b, (uint32_t)(i + 1)); /* 1-based index */
                        n_keys++;
                    }
                }
                put32(b.data + stss_count_at, n_keys);
                box_end(&b, stss);
            }

            box_end(&b, stbl);
            box_end(&b, minf);
            box_end(&b, mdia);
            box_end(&b, trak);
        }

        box_end(&b, moov);

        if (pass == 0) {
            /* 1パス目でサイズ確定 -> out_offset を全サンプルに設定 */
            moov_size = b.size;
            uint64_t cur_out = sizeof(ftyp) + moov_size + 16; /* mdat ヘッダ(16B largesize) */

            /* 映像・音声を時間順に交互に配置 */
            size_t vp = 0, ap = 0;
            while (vp < sample_counts[0] || ap < sample_counts[1]) {
                int pick = 0;
                if (vp < sample_counts[0] && ap < sample_counts[1]) {
                    pick = (samples[0][vp].dts_us <= samples[1][ap].dts_us) ? 0 : 1;
                } else if (vp < sample_counts[0]) {
                    pick = 0;
                } else {
                    pick = 1;
                }

                if (pick == 0) {
                    samples[0][vp].out_offset = cur_out;
                    cur_out += samples[0][vp].size;
                    vp++;
                } else {
                    samples[1][ap].out_offset = cur_out;
                    cur_out += samples[1][ap].size;
                    ap++;
                }
            }
            free(b.data);
        } else {
            moov_data = b.data;
        }
    }

    /* 3. ftyp と moov の書き込み */
    fwrite(moov_data, 1, moov_size, fout);
    free(moov_data);

    /* 4. mdat ヘッダの書き込み (64bit largesize 形式) */
    uint64_t total_payload = 0;
    for (int k = 0; k < 2; k++) {
        for (size_t i = 0; i < sample_counts[k]; i++) {
            total_payload += samples[k][i].size;
        }
    }

    uint8_t mdat_hdr[16];
    put32(mdat_hdr, 1); /* largesize */
    memcpy(mdat_hdr + 4, "mdat", 4);
    put64(mdat_hdr + 8, 16 + total_payload);
    fwrite(mdat_hdr, 1, 16, fout);

    /* 5. ペイロードのストリームコピー */
    uint8_t copy_buf[65536];
    size_t vp = 0, ap = 0;
    while (vp < sample_counts[0] || ap < sample_counts[1]) {
        int pick = 0;
        if (vp < sample_counts[0] && ap < sample_counts[1]) {
            pick = (samples[0][vp].dts_us <= samples[1][ap].dts_us) ? 0 : 1;
        } else if (vp < sample_counts[0]) {
            pick = 0;
        } else {
            pick = 1;
        }

        SampleEntry *cur = (pick == 0) ? &samples[0][vp++] : &samples[1][ap++];

        fseek(fin, (long)cur->in_offset, SEEK_SET);
        size_t left = cur->size;
        while (left > 0) {
            size_t chunk = left > sizeof(copy_buf) ? sizeof(copy_buf) : left;
            if (fread(copy_buf, 1, chunk, fin) != chunk) break;
            fwrite(copy_buf, 1, chunk, fout);
            left -= chunk;
        }
    }

    fclose(fin);
    fclose(fout);

    if (out_duration_ms) *out_duration_ms = (int64_t)(max_duration / 1000);
    if (out_video_samples) *out_video_samples = (int64_t)sample_counts[0];
    if (out_samples) {
        *out_samples = (int64_t)(sample_counts[0] + sample_counts[1]);
    }

    /* 6. 不要になった一時ファイルを即座に unlink */
    if (remove_input) {
        unlink(in_fmp4_path);
    }

    /* メモリ解放 */
    for (int k = 0; k < 2; k++) {
        free(configs[k].stsd_box);
        free(samples[k]);
    }

    return 0;
}
