/*
 * ts_demux.c - 最小限の MPEG-2 TS 分離器
 *
 * PAT から PMT の PID を、PMT から映像 (0x02 MPEG-2) と音声 (0x0F/0x11 AAC) の
 * PID を取る。PES ヘッダを外してペイロードをコールバックへ渡す。
 * 継続性カウンタの欠落は捨てずに繋ぐ（dtv_android の TsDemuxer と同じ扱い）。
 */
#include "ts_demux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TS_PACKET 188
#define MAX_PES (1024 * 1024)

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
    int active;
} PesBuf;

struct TsDemux {
    TsDemuxConfig cfg;
    int pmt_pid;
    int video_pid;
    int audio_pid;
    int have_pat;
    PesBuf video;
    PesBuf audio;
};

static int push(PesBuf *p, const uint8_t *data, size_t size)
{
    if (p->len + size > p->cap) {
        size_t ncap = p->cap ? p->cap * 2 : 65536;
        while (ncap < p->len + size)
            ncap *= 2;
        if (ncap > MAX_PES)
            ncap = MAX_PES;
        if (p->len + size > ncap)
            return -1;
        uint8_t *nb = realloc(p->buf, ncap);
        if (!nb)
            return -1;
        p->buf = nb;
        p->cap = ncap;
    }
    memcpy(p->buf + p->len, data, size);
    p->len += size;
    return 0;
}

/* PES ヘッダから PTS を取り出す（33 bit） */
static int64_t parse_pts(const uint8_t *p)
{
    return ((int64_t)(p[0] & 0x0e) << 29) |
           ((int64_t)(p[1]) << 22) |
           ((int64_t)(p[2] & 0xfe) << 14) |
           ((int64_t)(p[3]) << 7) |
           ((int64_t)(p[4]) >> 1);
}

/* PES を完成させてコールバックへ渡す */
static void flush_pes(TsDemux *d, PesBuf *p, int is_video)
{
    if (!p->active || p->len == 0) {
        p->len = 0;
        return;
    }

    const uint8_t *data = p->buf;
    size_t size = p->len;
    int64_t pts = -1;
    int key = 1;

    if (size >= 9 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01) {
        size_t pes_hdr = 9 + (size_t)data[8];
        if (pes_hdr <= size) {
            uint8_t flags = data[7];
            if ((flags & 0x80) && pes_hdr >= 14)
                pts = parse_pts(data + 9);
            data += pes_hdr;
            size -= pes_hdr;
        }
    }
    if (size == 0) {
        p->len = 0;
        return;
    }

    TsPayloadFn fn = is_video ? d->cfg.on_video : d->cfg.on_audio;
    if (fn)
        fn(d->cfg.user, data, size, pts, key);
    p->len = 0;
}

static void handle_psi(TsDemux *d, const uint8_t *sec, size_t len)
{
    if (len < 8)
        return;
    int table_id = sec[0];
    size_t sec_len = ((size_t)(sec[1] & 0x0f) << 8) | sec[2];

    if (table_id == 0x00 && !d->have_pat) {
        /* PAT: program 0 は NIT。最初の実プログラムの PMT PID を採る */
        for (size_t i = 8; i + 4 <= len && i + 4 <= sec_len + 3; i += 4) {
            int prog = (sec[i] << 8) | sec[i + 1];
            int pid = ((sec[i + 2] & 0x1f) << 8) | sec[i + 3];
            if (prog != 0) {
                d->pmt_pid = pid;
                d->have_pat = 1;
                break;
            }
        }
    } else if (table_id == 0x02) {
        /* PMT: program_info_length の後ろに ES ループ */
        size_t pi_len = ((size_t)(sec[10] & 0x0f) << 8) | sec[11];
        size_t i = 12 + pi_len;
        size_t end = 3 + sec_len - 4; /* CRC を除く */
        while (i + 5 <= len && i + 5 <= end) {
            int stream_type = sec[i];
            int pid = ((sec[i + 1] & 0x1f) << 8) | sec[i + 2];
            size_t es_len = ((size_t)(sec[i + 3] & 0x0f) << 8) | sec[i + 4];
            if (stream_type == 0x02 && d->video_pid < 0)
                d->video_pid = pid;
            else if ((stream_type == 0x0f || stream_type == 0x11) && d->audio_pid < 0)
                d->audio_pid = pid;
            i += 5 + es_len;
        }
    }
}

TsDemux *ts_demux_open(const TsDemuxConfig *cfg)
{
    TsDemux *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->cfg = *cfg;
    d->pmt_pid = -1;
    d->video_pid = -1;
    d->audio_pid = -1;
    return d;
}

void ts_demux_close(TsDemux *d)
{
    if (!d)
        return;
    free(d->video.buf);
    free(d->audio.buf);
    free(d);
}

int ts_demux_video_pid(const TsDemux *d) { return d ? d->video_pid : -1; }
int ts_demux_audio_pid(const TsDemux *d) { return d ? d->audio_pid : -1; }

int ts_demux_feed(TsDemux *d, const uint8_t *data, size_t size)
{
    size_t off = 0;
    int processed = 0;

    while (off + TS_PACKET <= size) {
        const uint8_t *p = data + off;
        off += TS_PACKET;
        processed += TS_PACKET;

        if (p[0] != 0x47)
            continue;

        int pid = ((p[1] & 0x1f) << 8) | p[2];
        int pusi = (p[1] & 0x40) != 0;
        int afc = (p[3] >> 4) & 0x3;
        int cc = p[3] & 0x0f;
        (void)cc;

        size_t payload_off = 4;
        if (afc & 0x2) {
            size_t af_len = p[4];
            payload_off = 5 + af_len;
        }
        if (!(afc & 0x1) || payload_off >= TS_PACKET)
            continue;

        const uint8_t *payload = p + payload_off;
        size_t payload_len = TS_PACKET - payload_off;

        if (pid == 0 || pid == d->pmt_pid) {
            if (!pusi)
                continue;
            size_t ptr = payload[0];
            if (1 + ptr < payload_len)
                handle_psi(d, payload + 1 + ptr, payload_len - 1 - ptr);
            continue;
        }

        if (pid == d->video_pid) {
            if (pusi) {
                flush_pes(d, &d->video, 1);
                d->video.active = 1;
            }
            if (d->video.active)
                push(&d->video, payload, payload_len);
        } else if (pid == d->audio_pid && d->cfg.want_audio) {
            if (pusi) {
                flush_pes(d, &d->audio, 0);
                d->audio.active = 1;
            }
            if (d->audio.active)
                push(&d->audio, payload, payload_len);
        }
    }
    return processed;
}
