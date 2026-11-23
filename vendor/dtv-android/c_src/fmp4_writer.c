#include "fmp4_writer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SCALE 1000000 /* タイムスケール: 1,000,000 (マイクロ秒単位) */

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
    int failed;
} Box;

typedef struct {
    int64_t pts_us;
    int64_t dts_us;
    uint32_t duration_us;
    uint32_t size;
    int is_key;
} SampleMeta;

typedef struct {
    SampleMeta *samples;
    size_t count;
    size_t capacity;
    uint8_t *payload;
    size_t payload_size;
    size_t payload_cap;
} FragTrack;

struct Fmp4Writer {
    void *user;
    Fmp4WriteFn write_fn;

    /* ビデオ設定 */
    int has_video;
    int is_hevc;
    int width, height;
    uint8_t *sps, *pps;
    size_t sps_size, pps_size;

    /* オーディオ設定 */
    int has_audio;
    int sample_rate, channels;
    uint8_t *asc;
    size_t asc_size;

    /* 断片蓄積用 */
    FragTrack tracks[2]; /* 0: video, 1: audio */
    int init_written;
};

/* ---- バイト構築ユーティリティ ---- */

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
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
    box_u32(b, 0); /* 後で埋めるサイズプレースホルダ */
    box_bytes(b, type, 4);
    return at;
}

static void box_end(Box *b, size_t at) {
    if (!b->failed && b->data) {
        put32(b->data + at, (uint32_t)(b->size - at));
    }
}

static void box_matrix(Box *b) {
    /* 単位行列 (1.0, 0, 0, 0, 1.0, 0, 0, 0, 16384) */
    for (int i = 0; i < 9; i++) {
        box_u32(b, (i == 0 || i == 4) ? 0x00010000 : (i == 8) ? 0x40000000 : 0);
    }
}



/* ---- 設定・初期化 ---- */

Fmp4Writer *fmp4_writer_open(void *user, Fmp4WriteFn write_fn) {
    if (!write_fn) return NULL;
    Fmp4Writer *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->user = user;
    w->write_fn = write_fn;
    return w;
}

int fmp4_writer_set_video_h264(Fmp4Writer *w, int width, int height,
                               const void *sps, size_t sps_len,
                               const void *pps, size_t pps_len) {
    if (!w || width <= 0 || height <= 0 || !sps || !pps || sps_len < 4 || pps_len < 1) return -1;
    free(w->sps); free(w->pps);
    w->sps = malloc(sps_len);
    w->pps = malloc(pps_len);
    if (!w->sps || !w->pps) { free(w->sps); free(w->pps); w->sps = w->pps = NULL; return -1; }
    memcpy(w->sps, sps, sps_len); w->sps_size = sps_len;
    memcpy(w->pps, pps, pps_len); w->pps_size = pps_len;
    w->width = width;
    w->height = height;
    w->is_hevc = 0;
    w->has_video = 1;
    return 0;
}

int fmp4_writer_set_video_hevc(Fmp4Writer *w, int width, int height,
                               const void *hvcc, size_t hvcc_len) {
    if (!w || width <= 0 || height <= 0 || !hvcc || hvcc_len < 8) return -1;
    free(w->sps);
    free(w->pps);
    w->sps = malloc(hvcc_len);
    if (!w->sps) return -1;
    memcpy(w->sps, hvcc, hvcc_len);
    w->sps_size = hvcc_len;
    w->pps = NULL;
    w->pps_size = 0;
    w->width = width;
    w->height = height;
    w->is_hevc = 1;
    w->has_video = 1;
    return 0;
}

int fmp4_writer_set_audio_aac(Fmp4Writer *w, int sample_rate, int channels,
                              const void *asc, size_t asc_len) {
    if (!w || sample_rate <= 0 || channels <= 0 || !asc || !asc_len || asc_len > 64) return -1;
    free(w->asc);
    w->asc = malloc(asc_len);
    if (!w->asc) return -1;
    memcpy(w->asc, asc, asc_len);
    w->asc_size = asc_len;
    w->sample_rate = sample_rate;
    w->channels = channels;
    w->has_audio = 1;
    return 0;
}

/* ---- 初期化セグメント (ftyp + moov) 構築 ---- */

static void write_sample_description(Box *b, Fmp4Writer *w, int is_audio) {
    size_t stsd = box_begin(b, "stsd");
    box_u32(b, 0); /* version/flags */
    box_u32(b, 1); /* entry_count = 1 */

    if (is_audio) {
        size_t mp4a = box_begin(b, "mp4a");
        box_zero(b, 6); /* reserved */
        box_u16(b, 1);  /* data_reference_index */
        box_zero(b, 8); /* reserved */
        box_u16(b, (uint16_t)w->channels);
        box_u16(b, 16); /* sample size */
        box_zero(b, 4); /* reserved */
        box_u32(b, (uint32_t)w->sample_rate << 16);

        size_t esds = box_begin(b, "esds");
        box_u32(b, 0); /* version/flags */
        box_u8(b, 3); box_u8(b, (uint8_t)(23 + w->asc_size)); box_u16(b, 2); box_u8(b, 0);
        box_u8(b, 4); box_u8(b, (uint8_t)(15 + w->asc_size)); box_u8(b, 0x40); box_u8(b, 0x15);
        box_zero(b, 3); box_u32(b, 0); box_u32(b, 0);
        box_u8(b, 5); box_u8(b, (uint8_t)w->asc_size);
        box_bytes(b, w->asc, w->asc_size);
        box_u8(b, 6); box_u8(b, 1); box_u8(b, 2);
        box_end(b, esds);

        box_end(b, mp4a);
    } else {
        /*
         * hvc1 carries the HEVC parameter sets in hvcC only, which is what the
         * hardware encoder hands over; avc1 keeps SPS and PPS in avcC.
         */
        size_t entry = box_begin(b, w->is_hevc ? "hvc1" : "avc1");
        box_zero(b, 6); /* reserved */
        box_u16(b, 1);  /* data_reference_index */
        box_zero(b, 16);
        box_u16(b, (uint16_t)w->width);
        box_u16(b, (uint16_t)w->height);
        box_u32(b, 0x00480000); /* 72 dpi */
        box_u32(b, 0x00480000);
        box_u32(b, 0);
        box_u16(b, 1); /* frame_count */
        box_zero(b, 32); /* compressorname */
        box_u16(b, 0x0018); /* depth: 24 */
        box_u16(b, 0xffff);

        if (w->is_hevc) {
            /* The encoder already provides a HEVCDecoderConfigurationRecord. */
            size_t hvcc = box_begin(b, "hvcC");
            box_bytes(b, w->sps, w->sps_size);
            box_end(b, hvcc);
        } else {
            size_t avcc = box_begin(b, "avcC");
            box_u8(b, 1); /* configurationVersion */
            box_u8(b, w->sps[1]); /* profile */
            box_u8(b, w->sps[2]); /* profile_compat */
            box_u8(b, w->sps[3]); /* level */
            box_u8(b, 0xff);      /* 6 bits reserved + 2 bits NALULengthSizeMinusOne (3 = 4B) */
            box_u8(b, 0xe1);      /* 3 bits reserved + 5 bits numOfSequenceParameterSets (1) */
            box_u16(b, (uint16_t)w->sps_size);
            box_bytes(b, w->sps, w->sps_size);
            box_u8(b, 1);         /* numOfPictureParameterSets */
            box_u16(b, (uint16_t)w->pps_size);
            box_bytes(b, w->pps, w->pps_size);
            box_end(b, avcc);
        }

        /* pasp (アスペクト比 1:1) */
        size_t pasp = box_begin(b, "pasp");
        box_u32(b, 1); box_u32(b, 1);
        box_end(b, pasp);

        box_end(b, entry);
    }
    box_end(b, stsd);
}

static void write_init_track(Box *b, Fmp4Writer *w, int is_audio) {
    size_t trak = box_begin(b, "trak");

    /* tkhd */
    size_t tkhd = box_begin(b, "tkhd");
    box_u32(b, 0x00000007); /* version=0, flags=enabled|in_movie|in_preview */
    box_zero(b, 8);         /* creation_time, modification_time */
    box_u32(b, is_audio ? 2 : 1); /* track_ID */
    box_u32(b, 0);          /* reserved */
    box_u32(b, 0);          /* duration (fMP4では0) */
    box_zero(b, 8);         /* reserved */
    box_u16(b, 0);          /* layer */
    box_u16(b, 0);          /* alternate_group */
    box_u16(b, is_audio ? 0x0100 : 0); /* volume */
    box_u16(b, 0);          /* reserved */
    box_matrix(b);
    box_u32(b, is_audio ? 0 : (uint32_t)w->width << 16);
    box_u32(b, is_audio ? 0 : (uint32_t)w->height << 16);
    box_end(b, tkhd);

    /* mdia */
    size_t mdia = box_begin(b, "mdia");

    /* mdhd */
    size_t mdhd = box_begin(b, "mdhd");
    box_u32(b, 0);          /* version=0, flags=0 */
    box_zero(b, 8);         /* times */
    box_u32(b, SCALE);      /* timescale = 1000000 */
    box_u32(b, 0);          /* duration = 0 */
    box_u16(b, 0x55c4);     /* language = und */
    box_u16(b, 0);          /* pre_defined */
    box_end(b, mdhd);

    /* hdlr */
    size_t hdlr = box_begin(b, "hdlr");
    box_u32(b, 0);
    box_u32(b, 0);
    box_bytes(b, is_audio ? "soun" : "vide", 4);
    box_zero(b, 12);
    box_bytes(b, is_audio ? "SoundHandler" : "VideoHandler", is_audio ? 12 : 12);
    box_u8(b, 0);
    box_end(b, hdlr);

    /* minf */
    size_t minf = box_begin(b, "minf");
    if (is_audio) {
        size_t smhd = box_begin(b, "smhd");
        box_u32(b, 0); box_zero(b, 4);
        box_end(b, smhd);
    } else {
        size_t vmhd = box_begin(b, "vmhd");
        box_u32(b, 1); box_zero(b, 8);
        box_end(b, vmhd);
    }

    /* dinf -> dref -> url */
    size_t dinf = box_begin(b, "dinf");
    size_t dref = box_begin(b, "dref");
    box_u32(b, 0); box_u32(b, 1);
    size_t url = box_begin(b, "url ");
    box_u32(b, 1); /* flags=1 (same file) */
    box_end(b, url);
    box_end(b, dref);
    box_end(b, dinf);

    /* stbl: fMP4の初期化セグメントでは空のテーブル群が必須 (W3C MSE仕様) */
    size_t stbl = box_begin(b, "stbl");
    write_sample_description(b, w, is_audio);

    size_t stts = box_begin(b, "stts");
    box_u32(b, 0); box_u32(b, 0); /* entry_count = 0 */
    box_end(b, stts);

    size_t stsc = box_begin(b, "stsc");
    box_u32(b, 0); box_u32(b, 0); /* entry_count = 0 */
    box_end(b, stsc);

    size_t stsz = box_begin(b, "stsz");
    box_u32(b, 0); box_u32(b, 0); box_u32(b, 0); /* sample_size=0, sample_count=0 */
    box_end(b, stsz);

    size_t stco = box_begin(b, "stco");
    box_u32(b, 0); box_u32(b, 0); /* entry_count = 0 */
    box_end(b, stco);

    box_end(b, stbl);
    box_end(b, minf);
    box_end(b, mdia);
    box_end(b, trak);
}

int fmp4_writer_write_init(Fmp4Writer *w) {
    if (!w || !w->write_fn || w->init_written) return -1;
    if (!w->has_video) return -1;

    Box b = {0};

    /* 1. ftyp */
    size_t ftyp = box_begin(&b, "ftyp");
    box_bytes(&b, "iso6", 4);    /* major_brand */
    box_u32(&b, 1);              /* minor_version */
    box_bytes(&b, "isom", 4);    /* compatible_brands */
    box_bytes(&b, "iso6", 4);
    box_bytes(&b, w->is_hevc ? "hvc1" : "avc1", 4);
    box_bytes(&b, "mp41", 4);
    box_end(&b, ftyp);

    /* 2. moov */
    size_t moov = box_begin(&b, "moov");

    /* mvhd */
    size_t mvhd = box_begin(&b, "mvhd");
    box_u32(&b, 0);       /* version=0, flags=0 */
    box_zero(&b, 8);      /* creation, modification times */
    box_u32(&b, SCALE);   /* timescale = 1,000,000 */
    box_u32(&b, 0);       /* duration = 0 (fMP4) */
    box_u32(&b, 0x00010000); /* rate = 1.0 */
    box_u16(&b, 0x0100);     /* volume = 1.0 */
    box_zero(&b, 10);
    box_matrix(&b);
    box_zero(&b, 24);
    box_u32(&b, w->has_audio ? 3 : 2); /* next_track_ID */
    box_end(&b, mvhd);

    /* traks */
    write_init_track(&b, w, 0);
    if (w->has_audio) {
        write_init_track(&b, w, 1);
    }

    /* mvex (fMP4必須ボックス: 各トラックのtrexを含む) */
    size_t mvex = box_begin(&b, "mvex");
    /* video trex */
    size_t trex1 = box_begin(&b, "trex");
    box_u32(&b, 0); /* version/flags */
    box_u32(&b, 1); /* track_ID = 1 */
    box_u32(&b, 1); /* default_sample_description_index = 1 */
    box_u32(&b, 40000); /* default_sample_duration (25fps=40ms, 任意値) */
    box_u32(&b, 0);     /* default_sample_size = 0 */
    box_u32(&b, 0x01010000); /* default_sample_flags: 非同期サンプル既定 */
    box_end(&b, trex1);

    if (w->has_audio) {
        size_t trex2 = box_begin(&b, "trex");
        box_u32(&b, 0); /* version/flags */
        box_u32(&b, 2); /* track_ID = 2 */
        box_u32(&b, 1); /* default_sample_description_index = 1 */
        box_u32(&b, (uint32_t)(1024ULL * 1000000 / (w->sample_rate ? w->sample_rate : 48000)));
        box_u32(&b, 0);
        box_u32(&b, 0x02000000); /* 音声はすべて同期サンプル扱い */
        box_end(&b, trex2);
    }
    box_end(&b, mvex);

    box_end(&b, moov);

    if (b.failed) {
        free(b.data);
        return -1;
    }

    int rc = w->write_fn(w->user, b.data, b.size);
    free(b.data);
    if (rc == 0) w->init_written = 1;
    return rc;
}

/* ---- 断片 (moof + mdat) 構築 ---- */

static size_t calculate_moof_size(Fmp4Writer *w) {
    size_t s = 8; /* moof header */
    s += 16;      /* mfhd */

    /* video traf */
    if (w->tracks[0].count > 0) {
        s += 8;  /* traf header */
        s += 16; /* tfhd (flags=0x020000 default-base-is-moof) */
        s += 20; /* tfdt v1 (uint64 decode_time) */
        s += 20 + w->tracks[0].count * 16; /* trun (v1, flags=0xf01: dur, size, flags, cto) */
    }

    /* audio traf */
    if (w->has_audio && w->tracks[1].count > 0) {
        s += 8;  /* traf header */
        s += 16; /* tfhd */
        s += 20; /* tfdt v1 */
        s += 20 + w->tracks[1].count * 12; /* trun (v1, flags=0x701: dur, size, flags) */
    }
    return s;
}

int fmp4_writer_flush_fragment(Fmp4Writer *w, uint32_t seq) {
    if (!w || !w->write_fn) return -1;
    if (w->tracks[0].count == 0) return 0; /* 出力対象なし */

    /* 1. moof サイズを事前計算 (1パスで data_offset を正確に埋めるため) */
    size_t moof_size = calculate_moof_size(w);

    Box b = {0};
    size_t moof = box_begin(&b, "moof");

    /* mfhd */
    size_t mfhd = box_begin(&b, "mfhd");
    box_u32(&b, 0);   /* version=0, flags=0 */
    box_u32(&b, seq); /* sequence_number */
    box_end(&b, mfhd);

    /* video traf */
    size_t v_offset = moof_size + 8; /* mdat ヘッダ(8B)分を加算 */
    {
        FragTrack *vt = &w->tracks[0];
        size_t traf = box_begin(&b, "traf");

        /* tfhd: default-base-is-moof (0x020000) */
        size_t tfhd = box_begin(&b, "tfhd");
        box_u32(&b, 0x00020000);
        box_u32(&b, 1); /* track_ID = 1 */
        box_end(&b, tfhd);

        /* tfdt: version=1 (uint64) */
        size_t tfdt = box_begin(&b, "tfdt");
        box_u32(&b, 0x01000000);
        box_u64(&b, (uint64_t)vt->samples[0].dts_us);
        box_end(&b, tfdt);

        /* trun: version=1, flags = data-offset | duration | size | flags | cto */
        size_t trun = box_begin(&b, "trun");
        box_u32(&b, 0x01000f01); /* version=1, flags=0x000f01 */
        box_u32(&b, (uint32_t)vt->count);
        box_u32(&b, (uint32_t)v_offset); /* data_offset */

        for (size_t i = 0; i < vt->count; i++) {
            SampleMeta *sm = &vt->samples[i];
            box_u32(&b, sm->duration_us);
            box_u32(&b, sm->size);
            /* sample_flags: 同期フレーム=0x02000000, 非同期=0x01010000 */
            uint32_t flags = sm->is_key ? 0x02000000 : 0x01010000;
            box_u32(&b, flags);
            /* composition_time_offset: pts - dts */
            int32_t cto = (int32_t)(sm->pts_us - sm->dts_us);
            box_u32(&b, (uint32_t)cto);
        }
        box_end(&b, trun);
        box_end(&b, traf);
    }

    /* audio traf */
    size_t a_offset = v_offset + w->tracks[0].payload_size;
    if (w->has_audio && w->tracks[1].count > 0) {
        FragTrack *at = &w->tracks[1];
        size_t traf = box_begin(&b, "traf");

        size_t tfhd = box_begin(&b, "tfhd");
        box_u32(&b, 0x00020000);
        box_u32(&b, 2); /* track_ID = 2 */
        box_end(&b, tfhd);

        size_t tfdt = box_begin(&b, "tfdt");
        box_u32(&b, 0x01000000);
        box_u64(&b, (uint64_t)at->samples[0].dts_us);
        box_end(&b, tfdt);

        size_t trun = box_begin(&b, "trun");
        box_u32(&b, 0x01000701); /* version=1, flags=0x000701 (ctoなし) */
        box_u32(&b, (uint32_t)at->count);
        box_u32(&b, (uint32_t)a_offset);

        for (size_t i = 0; i < at->count; i++) {
            SampleMeta *sm = &at->samples[i];
            box_u32(&b, sm->duration_us);
            box_u32(&b, sm->size);
            box_u32(&b, 0x02000000); /* 音声は全サンプル同期扱い */
        }
        box_end(&b, trun);
        box_end(&b, traf);
    }

    box_end(&b, moof);

    if (b.failed || b.size != moof_size) {
        /* 万一事前計算と実サイズが違った場合はエラー */
        free(b.data);
        return -1;
    }

    /* 2. moof を送信 */
    if (w->write_fn(w->user, b.data, b.size) != 0) {
        free(b.data);
        return -1;
    }
    free(b.data);

    /* 3. mdat ヘッダを送信 */
    size_t total_payload = w->tracks[0].payload_size + (w->has_audio ? w->tracks[1].payload_size : 0);
    uint8_t mdat_hdr[8];
    put32(mdat_hdr, (uint32_t)(8 + total_payload));
    memcpy(mdat_hdr + 4, "mdat", 4);
    if (w->write_fn(w->user, mdat_hdr, 8) != 0) return -1;

    /* 4. video ペイロードを送信 */
    if (w->tracks[0].payload_size > 0) {
        if (w->write_fn(w->user, w->tracks[0].payload, w->tracks[0].payload_size) != 0) return -1;
    }

    /* 5. audio ペイロードを送信 */
    if (w->has_audio && w->tracks[1].payload_size > 0) {
        if (w->write_fn(w->user, w->tracks[1].payload, w->tracks[1].payload_size) != 0) return -1;
    }

    /* 6. 断片バッファをクリアして次回に備える */
    for (int k = 0; k < 2; k++) {
        w->tracks[k].count = 0;
        w->tracks[k].payload_size = 0;
    }

    return 0;
}

/* ---- サンプル追加 ---- */

static int track_append_payload(FragTrack *t, const void *p, size_t n) {
    if (t->payload_size + n > t->payload_cap) {
        size_t cap = t->payload_cap ? t->payload_cap * 2 : 65536;
        while (cap < t->payload_size + n) cap *= 2;
        uint8_t *d = realloc(t->payload, cap);
        if (!d) return -1;
        t->payload = d;
        t->payload_cap = cap;
    }
    memcpy(t->payload + t->payload_size, p, n);
    t->payload_size += n;
    return 0;
}

void fmp4_writer_reset_samples(Fmp4Writer *w) {
    if (!w) return;
    for (int k = 0; k < 2; k++) {
        w->tracks[k].count = 0;
        w->tracks[k].payload_size = 0;
    }
}

size_t fmp4_writer_fragment_samples(const Fmp4Writer *w) {
    return w ? (w->tracks[0].count + w->tracks[1].count) : 0;
}

void fmp4_writer_close(Fmp4Writer *w) {
    if (!w) return;
    free(w->sps);
    free(w->pps);
    free(w->asc);
    for (int k = 0; k < 2; k++) {
        free(w->tracks[k].samples);
        free(w->tracks[k].payload);
    }
    free(w);
}

int fmp4_writer_add_sample_avcc(Fmp4Writer *w, int is_audio,
                                const void *data, size_t size,
                                int64_t pts_us, int64_t dts_us,
                                uint32_t duration_us, int is_key) {
    if (!w || !data || !size) return -1;
    int idx = is_audio ? 1 : 0;
    FragTrack *t = &w->tracks[idx];

    if (t->count == t->capacity) {
        size_t cap = t->capacity ? t->capacity * 2 : 256;
        SampleMeta *sm = realloc(t->samples, cap * sizeof(SampleMeta));
        if (!sm) return -1;
        t->samples = sm;
        t->capacity = cap;
    }

    /* AVCC 形式 (4バイト length-prefixed) をそのままダイレクト格納 */
    if (track_append_payload(t, data, size) != 0) return -1;

    if (t->count > 0 && dts_us > t->samples[t->count - 1].dts_us) {
        t->samples[t->count - 1].duration_us = (uint32_t)(dts_us - t->samples[t->count - 1].dts_us);
    }

    SampleMeta *meta = &t->samples[t->count++];
    meta->pts_us = pts_us;
    meta->dts_us = dts_us;
    meta->duration_us = duration_us ? duration_us : (is_audio ? 21333 : 40000);
    meta->size = (uint32_t)size;
    meta->is_key = is_key;
    return 0;
}
