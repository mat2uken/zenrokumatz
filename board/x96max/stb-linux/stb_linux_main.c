/*
 * stb_linux_main.c - Linux 版のライブ配信口
 *
 * dtv_android の c_src/stb_main.c と同じ構成（RamSpool + fmp4_writer +
 * stb_http）を使い、サンプルの供給元だけを差し替える。Android 版は
 * LiveTsStreamer.java が MediaCodec でデコード・エンコードした結果を
 * loopback TCP で送ってくるが、Linux 版は V4L2 mem2mem を直接叩いて
 * 同じサンプルを作る。ffmpeg などの外部ライブラリは使わない。
 *
 *   TS -> TsDemux -> V4L2 デコード -> V4L2 エンコード -> H.264 (Annex-B)
 *                                                          |
 *                          fmp4_writer (AVCC) -> RamSpool -> stb_http -> NAS
 *
 * 端末のストレージには書かない。TS は /dev/shm、映像はメモリ上だけ。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include "fmp4_writer.h"
#include "h264_avcc.h"
#include "ram_spool.h"
#include "stb_http.h"
#include "ts_demux.h"
#include "v4l2_m2m.h"

#define MAX_AU (4 * 1024 * 1024)

static volatile int g_stop;

static void on_sig(int sig)
{
    (void)sig;
    g_stop = 1;
}

typedef struct {
    uint8_t *data;
    size_t size;
    size_t cap;
} MemBuf;

static int mem_write_fn(void *user, const void *data, size_t size)
{
    MemBuf *b = user;
    if (b->size + size > b->cap) {
        size_t ncap = b->cap ? b->cap : 65536;
        while (ncap < b->size + size)
            ncap *= 2;
        uint8_t *p = realloc(b->data, ncap);
        if (!p)
            return -1;
        b->data = p;
        b->cap = ncap;
    }
    memcpy(b->data + b->size, data, size);
    b->size += size;
    return 0;
}

typedef struct {
    RamSpool *spool;
    Fmp4Writer *writer;
    MemBuf frag;
    uint32_t seq;
    H264ParamSets ps;
    int have_init;
    int width, height;
    int64_t pts_offset_us;
    int have_pts;
    uint64_t frames, fragments;
    uint8_t au[MAX_AU];
} Pipeline;

static void flush_fragment(Pipeline *p)
{
    if (!p->writer || fmp4_writer_fragment_samples(p->writer) == 0)
        return;
    if (fmp4_writer_flush_fragment(p->writer, p->seq) != 0) {
        fprintf(stderr, "[pipe] flush_fragment failed\n");
        return;
    }
    if (p->frag.size > 0) {
        if (ram_spool_push(p->spool, p->seq, p->frag.data, p->frag.size) == 0) {
            size_t used = 0, max_b = 0, count = 0;
            uint64_t pushed = 0, dropped = 0;
            ram_spool_stats(p->spool, &used, &max_b, &pushed, &dropped, &count);
            if ((p->fragments % 10) == 0)
                fprintf(stderr, "[pipe] fragment seq=%u (%.1f KB, spool %.1f/%.0f MB, frags=%zu)\n",
                        p->seq, p->frag.size / 1024.0,
                        used / 1048576.0, max_b / 1048576.0, count);
            p->fragments++;
        }
    }
    p->frag.size = 0;
    p->seq++;
}

/* SPS/PPS が揃った時点で初期化セグメントを作る */
static int init_session(Pipeline *p)
{
    if (p->have_init || !p->ps.have_sps || !p->ps.have_pps)
        return 0;

    MemBuf init = {0};
    Fmp4Writer *iw = fmp4_writer_open(&init, mem_write_fn);
    if (!iw)
        return -1;
    if (fmp4_writer_set_video_h264(iw, p->width, p->height,
                                   p->ps.sps, p->ps.sps_len,
                                   p->ps.pps, p->ps.pps_len) != 0) {
        fmp4_writer_close(iw);
        free(init.data);
        return -1;
    }
    fmp4_writer_write_init(iw);
    fmp4_writer_close(iw);

    ram_spool_set_init(p->spool, init.data, init.size);
    fprintf(stderr, "[pipe] init segment %zu bytes (%dx%d, sps=%zu pps=%zu)\n",
            init.size, p->width, p->height, p->ps.sps_len, p->ps.pps_len);
    free(init.data);

    if (p->writer)
        fmp4_writer_close(p->writer);
    p->writer = fmp4_writer_open(&p->frag, mem_write_fn);
    if (!p->writer)
        return -1;
    fmp4_writer_set_video_h264(p->writer, p->width, p->height,
                               p->ps.sps, p->ps.sps_len,
                               p->ps.pps, p->ps.pps_len);
    p->have_init = 1;
    return 0;
}

/* エンコーダーのアクセスユニットを fMP4 へ積む */
static void handle_encoded(Pipeline *p, const uint8_t *data, size_t size,
                           int64_t pts_us)
{
    h264_scan_param_sets(&p->ps, data, size);
    if (init_session(p) != 0 || !p->writer)
        return;

    int key = h264_is_keyframe(data, size);
    int n = h264_annexb_to_avcc(data, size, p->au, sizeof(p->au));
    if (n <= 0)
        return;

    if (key && fmp4_writer_fragment_samples(p->writer) > 0)
        flush_fragment(p);

    if (!p->have_pts) {
        p->pts_offset_us = -pts_us;
        p->have_pts = 1;
    }
    int64_t norm = pts_us + p->pts_offset_us;
    if (norm < 0)
        norm = 0;

    fmp4_writer_add_sample_avcc(p->writer, 0, p->au, (size_t)n,
                                norm, norm, 33333, key);
    p->frames++;
}

/* TS から来た映像 PES をデコードへ流し、出てきた絵をエンコードへ渡す */
typedef struct {
    Pipeline *p;
    V4l2M2m *dec;
    V4l2M2m *enc;
    int64_t pts_us;
} FrameCtx;

static int on_video_pes(void *user, const uint8_t *data, size_t size,
                        int64_t pts_us, int is_key)
{
    (void)is_key;
    FrameCtx *fc = user;
    Pipeline *p = fc->p;

    if (pts_us >= 0)
        fc->pts_us = pts_us;

    /* デコーダーへ 1 フレーム分を積む */
    if (v4l2_m2m_get_free_out(fc->dec) == NULL)
        return 0;
    if (v4l2_m2m_queue_out(fc->dec, data, size, fc->pts_us, 1) != 0)
        return 0;

    /* デコード済みの絵を取り出してエンコーダーへ渡す */
    V4l2Buffer dbuf;
    while (!g_stop && v4l2_m2m_dequeue_cap(fc->dec, &dbuf) == 1) {
        if (v4l2_m2m_get_free_out(fc->enc) != NULL)
            v4l2_m2m_queue_out(fc->enc, dbuf.addr, dbuf.bytesused, fc->pts_us, 1);
        v4l2_m2m_requeue_cap(fc->dec, &dbuf);
    }

    /* エンコード結果を回収 */
    V4l2Buffer ebuf;
    while (!g_stop && v4l2_m2m_dequeue_cap(fc->enc, &ebuf) == 1) {
        handle_encoded(p, ebuf.addr, ebuf.bytesused, fc->pts_us);
        v4l2_m2m_requeue_cap(fc->enc, &ebuf);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *input = "/dev/shm/pix/live.ts";
    const char *allowed_ip = NULL;
    const char *decoder = "/dev/video0";
    const char *encoder = "/dev/video1";
    int port = 40774;
    size_t spool_mb = 192;
    int width = 1440, height = 1080;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--allowed-ip") == 0 && i + 1 < argc) allowed_ip = argv[++i];
        else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) input = argv[++i];
        else if (strcmp(argv[i], "--decoder") == 0 && i + 1 < argc) decoder = argv[++i];
        else if (strcmp(argv[i], "--encoder") == 0 && i + 1 < argc) encoder = argv[++i];
        else if (strcmp(argv[i], "--spool-mb") == 0 && i + 1 < argc) spool_mb = (size_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--size") == 0 && i + 2 < argc) {
            width = atoi(argv[++i]);
            height = atoi(argv[++i]);
        }
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "=== STB Linux streamer (V4L2 m2m, no external libraries) ===\n");
    fprintf(stderr, "Input: %s, Decoder: %s, Encoder: %s, %dx%d\n",
            input, decoder, encoder, width, height);

    RamSpool *spool = ram_spool_create(spool_mb * 1024 * 1024);
    if (!spool)
        return 1;

    StbHttpConfig http_cfg = {
        .port = port,
        .allowed_ip = allowed_ip,
        .spool = spool,
        .stop_flag = &g_stop,
        .idle_timeout_sec = 0,
    };
    StbHttpServer *srv = stb_http_start(&http_cfg);
    if (!srv) {
        fprintf(stderr, "failed to start the HTTP server on port %d\n", port);
        ram_spool_destroy(spool);
        return 1;
    }
    fprintf(stderr, "HTTP server listening on 0.0.0.0:%d\n", port);

    Pipeline p = {0};
    p.spool = spool;
    p.width = width;
    p.height = height;

    V4l2M2mConfig dcfg = {
        .device = decoder,
        .pixfmt_out = V4L2_PIX_FMT_MPEG2,
        .pixfmt_cap = V4L2_PIX_FMT_NV12M,
        .width = width, .height = height,
        .num_out = 4, .num_cap = 8,
    };
    V4l2M2m *dec = v4l2_m2m_open(&dcfg);
    if (!dec) {
        fprintf(stderr, "failed to open the decoder %s\n", decoder);
        goto out_http;
    }

    V4l2M2mConfig ecfg = {
        .device = encoder,
        .pixfmt_out = V4L2_PIX_FMT_NV12M,
        .pixfmt_cap = V4L2_PIX_FMT_H264,
        .width = width, .height = height,
        .num_out = 4, .num_cap = 4,
    };
    V4l2M2m *enc = v4l2_m2m_open(&ecfg);
    if (!enc) {
        fprintf(stderr, "failed to open the encoder %s\n", encoder);
        v4l2_m2m_close(dec);
        goto out_http;
    }

    FrameCtx fc = { .p = &p, .dec = dec, .enc = enc, .pts_us = 0 };
    TsDemuxConfig tcfg = { .on_video = on_video_pes, .user = &fc, .want_audio = 0 };
    TsDemux *demux = ts_demux_open(&tcfg);
    if (!demux) {
        v4l2_m2m_close(enc);
        v4l2_m2m_close(dec);
        goto out_http;
    }

    int fd = open(input, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "failed to open the input %s: %s\n", input, strerror(errno));
        ts_demux_close(demux);
        v4l2_m2m_close(enc);
        v4l2_m2m_close(dec);
        goto out_http;
    }

    static uint8_t tsbuf[188 * 256];
    while (!g_stop) {
        ssize_t n = read(fd, tsbuf, sizeof(tsbuf));
        if (n <= 0) {
            if (n == 0) break;
            if (errno == EINTR) continue;
            break;
        }
        ts_demux_feed(demux, tsbuf, (size_t)n);
    }

    close(fd);
    ts_demux_close(demux);
    flush_fragment(&p);
    v4l2_m2m_drain(enc);
    v4l2_m2m_drain(dec);
    v4l2_m2m_close(enc);
    v4l2_m2m_close(dec);
    if (p.writer)
        fmp4_writer_close(p.writer);
    free(p.frag.data);
    fprintf(stderr, "[pipe] done: %llu frames, %u fragments\n",
            (unsigned long long)p.frames, p.seq);

out_http:
    stb_http_stop(srv);
    ram_spool_destroy(spool);
    return 0;
}
