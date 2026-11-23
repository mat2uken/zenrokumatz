#include "fmp4_sink.h"
#include "fmp4_writer.h"
#include "ram_spool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <poll.h>

#define REC_TYPE_INIT     0
#define REC_TYPE_VIDEO    1
#define REC_TYPE_AUDIO    2
#define REC_TYPE_FLUSH    3
#define REC_TYPE_BOUNDARY 4

#define REC_HEADER_LEN 20
#define DEFAULT_FRAGMENT_BYTES (2 * 1024 * 1024)

typedef struct {
    uint8_t *data;
    size_t size;
    size_t cap;
} MemBuf;

static int mem_write_fn(void *user, const void *data, size_t size) {
    MemBuf *b = (MemBuf *)user;
    if (!b) return -1;
    if (b->size + size > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 65536;
        while (ncap < b->size + size) ncap *= 2;
        uint8_t *p = realloc(b->data, ncap);
        if (!p) return -1;
        b->data = p;
        b->cap = ncap;
    }
    memcpy(b->data + b->size, data, size);
    b->size += size;
    return 0;
}

struct Fmp4Sink {
    int port;
    int listen_fd;
    int client_fd;

    RamSpool *spool;
    size_t fragment_bytes;

    /* 現在構築中の断片（writer がこのバッファへ書き込む） */
    MemBuf frag;
    Fmp4Writer *writer;
    uint32_t seq;

    int have_init;
    int video_hevc;
    int width, height;
    uint8_t *sps, *pps;
    size_t sps_len, pps_len;

    /* Audio configuration reported by the Java side. */
    int has_audio;
    int sample_rate;
    int channels;
    uint8_t asc[64];
    size_t asc_len;

    /* Pending program boundary, emitted in front of the next fragment. */
    int boundary_pending;
    int bd_service_id;
    int bd_event_id;
    int64_t bd_start_ms;
    int64_t bd_duration_ms;
    int64_t bd_wallclock_ms;

    uint64_t frames;
    uint64_t fragments;

    /*
     * Timeline normalisation. The Java side may restart its codec, which
     * makes the encoder timestamps jump back to zero. The STB keeps one
     * monotonic timeline so the fragments stay ordered and a reader can
     * resume from a sequence number.
     */
    int64_t pts_offset_us;      /* shared, so audio and video stay in sync */
    int64_t next_pts_us[2];     /* per track: video=0, audio=1 */
    int have_pts;

    pthread_mutex_t lock;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t rd64(const uint8_t *p) {
    return ((uint64_t)rd32(p) << 32) | (uint64_t)rd32(p + 4);
}

static int read_exact(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

/* 現在の断片をスプールへ投入して次の断片を開始する */
static int flush_current_fragment(Fmp4Sink *st) {
    if (!st->writer || !st->spool) return 0;
    if (fmp4_writer_fragment_samples(st->writer) == 0) return 0;

    if (fmp4_writer_flush_fragment(st->writer, st->seq) != 0) {
        fprintf(stderr, "[SINK] flush_fragment failed\n");
        return -1;
    }

    if (st->frag.size > 0) {
        if (ram_spool_push(st->spool, st->seq, st->frag.data, st->frag.size) != 0) {
            fprintf(stderr, "[SINK] spool push failed\n");
        } else {
            size_t used = 0, max_b = 0, count = 0;
            uint64_t pushed = 0, dropped = 0;
            ram_spool_stats(st->spool, &used, &max_b, &pushed, &dropped, &count);
            fprintf(stderr, "[SINK] fragment seq=%u pushed (%.1f KB, spool %.1f/%.0f MB, frags=%zu)\n",
                    st->seq, st->frag.size / 1024.0,
                    used / (1024.0 * 1024.0), max_b / (1024.0 * 1024.0), count);
            st->fragments++;
        }
    }
    st->frag.size = 0;
    st->seq++;
    return 0;
}

Fmp4Sink *fmp4_sink_open(const Fmp4SinkConfig *cfg) {
    if (!cfg || cfg->port <= 0) return NULL;

    Fmp4Sink *st = calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->port = cfg->port;
    st->spool = (RamSpool *)cfg->spool;
    st->fragment_bytes = cfg->fragment_bytes ? cfg->fragment_bytes : DEFAULT_FRAGMENT_BYTES;
    st->client_fd = -1;
    pthread_mutex_init(&st->lock, NULL);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { free(st); return NULL; }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)st->port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 4) != 0) {
        close(fd);
        free(st);
        return NULL;
    }
    st->listen_fd = fd;
    return st;
}

/* INIT レコード: 初期化セグメントを生成してスプールへ登録し、writer を用意する */
static int handle_init(Fmp4Sink *st, int fd) {
    uint8_t cfgbuf[16];
    if (read_exact(fd, cfgbuf, sizeof(cfgbuf)) != 0) return -1;

    int width = rd16(cfgbuf);
    int height = rd16(cfgbuf + 2);
    size_t sps_len = rd16(cfgbuf + 4);
    size_t pps_len = rd16(cfgbuf + 6);
    int sample_rate = rd16(cfgbuf + 8);
    int channels = cfgbuf[10];
    size_t asc_len = cfgbuf[11];
    int is_hevc = cfgbuf[12];

    /* HEVC delivers the whole configuration record in csd-0, so pps is empty. */
    if (sps_len == 0 || sps_len > 4096 || pps_len > 4096) return -1;
    if (!is_hevc && pps_len == 0) return -1;
    if (asc_len > sizeof(st->asc)) return -1;

    uint8_t *sps = malloc(sps_len);
    uint8_t *pps = malloc(pps_len ? pps_len : 1);
    if (!sps || !pps) { free(sps); free(pps); return -1; }
    if (read_exact(fd, sps, sps_len) != 0) { free(sps); free(pps); return -1; }
    if (pps_len && read_exact(fd, pps, pps_len) != 0) { free(sps); free(pps); return -1; }
    if (asc_len > 0 && read_exact(fd, st->asc, asc_len) != 0) {
        free(sps); free(pps);
        return -1;
    }

    st->video_hevc = is_hevc;
    st->has_audio = (asc_len > 0 && sample_rate > 0 && channels > 0);
    st->sample_rate = sample_rate;
    st->channels = channels;
    st->asc_len = asc_len;

    /* 初期化セグメント (ftyp + moov[mvex]) の生成 */
    MemBuf init = {0};
    Fmp4Writer *iw = fmp4_writer_open(&init, mem_write_fn);
    if (!iw) { free(sps); free(pps); return -1; }
    int vrc = is_hevc
            ? fmp4_writer_set_video_hevc(iw, width, height, sps, sps_len)
            : fmp4_writer_set_video_h264(iw, width, height, sps, sps_len, pps, pps_len);
    if (vrc != 0) {
        fmp4_writer_close(iw); free(init.data); free(sps); free(pps);
        return -1;
    }
    if (st->has_audio) {
        fmp4_writer_set_audio_aac(iw, sample_rate, channels, st->asc, asc_len);
    }
    fmp4_writer_write_init(iw);
    fmp4_writer_close(iw);

    if (st->spool) {
        ram_spool_set_init(st->spool, init.data, init.size);
    }
    fprintf(stderr, "[SINK] Init segment registered (%zu bytes, %dx%d, %s, cfg=%zu, audio=%s)\n",
            init.size, width, height, is_hevc ? "hevc" : "avc",
            sps_len, st->has_audio ? "aac" : "none");
    if (st->has_audio) {
        fprintf(stderr, "[SINK] Audio track: %d Hz, %d ch, ASC %zu bytes\n",
                sample_rate, channels, asc_len);
    }
    free(init.data);

    free(st->sps); free(st->pps);
    st->sps = sps; st->sps_len = sps_len;
    st->pps = pps; st->pps_len = pps_len;
    st->width = width; st->height = height;
    st->have_init = 1;
    /* The fragment sequence must stay monotonic for the lifetime of this
     * process, otherwise a reader that resumes by sequence number would
     * either miss fragments or replay old ones. */
    st->frag.size = 0;
    st->have_pts = 0;

    /* 以降のサンプルを受ける writer（断片バッファへ直接書く） */
    if (st->writer) fmp4_writer_close(st->writer);
    st->writer = fmp4_writer_open(&st->frag, mem_write_fn);
    if (!st->writer) return -1;
    if (is_hevc) {
        fmp4_writer_set_video_hevc(st->writer, width, height, sps, sps_len);
    } else {
        fmp4_writer_set_video_h264(st->writer, width, height, sps, sps_len, pps, pps_len);
    }
    if (st->has_audio) {
        fmp4_writer_set_audio_aac(st->writer, sample_rate, channels, st->asc, asc_len);
    }
    return 0;
}

/**
 * Writes the program boundary box in front of the next fragment.
 * The box is a top level box, so players ignore it and the recorder can cut
 * the stream on program changes without breaking the MP4 structure.
 */
static void emit_boundary_box(Fmp4Sink *st) {
    uint8_t box[44];
    memset(box, 0, sizeof(box));
    box[0] = 0; box[1] = 0; box[2] = 0; box[3] = 44;
    memcpy(box + 4, "pixb", 4);
    box[8] = 1; /* version */
    box[12] = (uint8_t)(st->bd_service_id >> 8);
    box[13] = (uint8_t)st->bd_service_id;
    box[14] = (uint8_t)(st->bd_event_id >> 8);
    box[15] = (uint8_t)st->bd_event_id;
    /* 16..19 reserved */
    for (int i = 0; i < 8; i++) {
        box[20 + i] = (uint8_t)((uint64_t)st->bd_start_ms >> (56 - 8 * i));
        box[28 + i] = (uint8_t)((uint64_t)st->bd_duration_ms >> (56 - 8 * i));
        box[36 + i] = (uint8_t)((uint64_t)st->bd_wallclock_ms >> (56 - 8 * i));
    }
    mem_write_fn(&st->frag, box, sizeof(box));
    fprintf(stderr, "[SINK] boundary box written service=%d event=%d start=%lld dur=%lld\n",
            st->bd_service_id, st->bd_event_id,
            (long long)st->bd_start_ms, (long long)st->bd_duration_ms);
    st->boundary_pending = 0;
}

int fmp4_sink_poll(Fmp4Sink *sink, int timeout_ms) {
    if (!sink) return -2;

    if (sink->client_fd < 0) {
        struct pollfd pfd = { .fd = sink->listen_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr > 0) {
            int cfd = accept(sink->listen_fd, NULL, NULL);
            if (cfd >= 0) {
                sink->client_fd = cfd;
                fprintf(stderr, "[SINK] Java client connected\n");

            }
        }
        return 0;
    }

    struct pollfd pfd = { .fd = sink->client_fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) return 0;

    /* Let channel requests proceed while the Java client is idle. Only this
     * thread accepts or closes client_fd; the lock protects sample state. */
    pthread_mutex_lock(&sink->lock);

    uint8_t hdr[REC_HEADER_LEN];
    if (read_exact(sink->client_fd, hdr, sizeof(hdr)) != 0) {
        fprintf(stderr, "[SINK] Java client disconnected\n");
        close(sink->client_fd);
        sink->client_fd = -1;
        if (sink->writer) { fmp4_writer_close(sink->writer); sink->writer = NULL; }
        sink->have_init = 0;
        pthread_mutex_unlock(&sink->lock);
        return -1;
    }

    uint8_t type = hdr[0];
    uint8_t flags = hdr[1];
    /* composition offset in milliseconds (pts - dts); 0 for senders that
       keep dts == pts, which is every stream the VPU encoder produces. */
    int16_t cto_ms = (int16_t)((uint16_t)((hdr[2] << 8) | hdr[3]));
    int64_t pts_us = (int64_t)rd64(hdr + 4);
    uint32_t dur_us = rd32(hdr + 12);
    uint32_t data_size = rd32(hdr + 16);

    if (type == REC_TYPE_INIT) {
        int rc = handle_init(sink, sink->client_fd);
        pthread_mutex_unlock(&sink->lock);
        return rc == 0 ? 1 : -2;
    }

    if (type == REC_TYPE_FLUSH) {
        flush_current_fragment(sink);
        pthread_mutex_unlock(&sink->lock);
        return 1;
    }

    if (type == REC_TYPE_BOUNDARY) {
        uint8_t bd[16];
        if (read_exact(sink->client_fd, bd, sizeof(bd)) != 0) {
            close(sink->client_fd);
            sink->client_fd = -1;
            pthread_mutex_unlock(&sink->lock);
            return -1;
        }
        sink->bd_service_id = rd16(bd);
        sink->bd_event_id = rd16(bd + 2);
        sink->bd_start_ms = (int64_t)(((uint64_t)rd32(bd + 8) << 32) | rd32(bd + 12));
        sink->bd_duration_ms = (int64_t)dur_us;
        sink->bd_wallclock_ms = pts_us;
        sink->boundary_pending = 1;
        fprintf(stderr, "[SINK] boundary requested service=%d event=%d\n",
                sink->bd_service_id, sink->bd_event_id);
        /* If no fragment is in progress the box can go out immediately. */
        if (sink->writer && fmp4_writer_fragment_samples(sink->writer) == 0) {
            emit_boundary_box(sink);
        }
        pthread_mutex_unlock(&sink->lock);
        return 1;
    }

    if (data_size > 32 * 1024 * 1024) { pthread_mutex_unlock(&sink->lock); return -2; }

    uint8_t *data = malloc(data_size ? data_size : 1);
    if (!data) { pthread_mutex_unlock(&sink->lock); return -2; }
    if (data_size && read_exact(sink->client_fd, data, data_size) != 0) {
        free(data);
        close(sink->client_fd);
        sink->client_fd = -1;
        pthread_mutex_unlock(&sink->lock);
        return -1;
    }

    if (sink->writer) {
        int is_audio = (type == REC_TYPE_AUDIO);

        /*
         * Every fragment must start on a key frame so that it can be decoded
         * on its own. When a key frame arrives and the current fragment
         * already holds samples, close the fragment first and start the new
         * one with this key frame.
         */
        if (!is_audio && (flags & 1)) {
            if (fmp4_writer_fragment_samples(sink->writer) > 0) {
                flush_current_fragment(sink);
            }
            /* A program change takes effect at the next key frame so that a
             * fragment never straddles two programs and every fragment still
             * starts on a key frame. */
            if (sink->boundary_pending) {
                emit_boundary_box(sink);
            }
        }

        /* Keep the timeline monotonic across codec restarts. The offset is
         * shared between the tracks so that audio and video stay in sync;
         * only the "next expected" value is tracked per track. */
        int tk = is_audio ? 1 : 0;
        if (!sink->have_pts) {
            sink->pts_offset_us = -pts_us;
            sink->have_pts = 1;
        }
        int64_t norm_pts = pts_us + sink->pts_offset_us;
        if (norm_pts + 1000000 < sink->next_pts_us[tk]) {
            /* The encoder restarted and the timestamps jumped backwards.
             * Shift the whole timeline forward so nothing goes back in time. */
            int64_t bump = sink->next_pts_us[tk] - norm_pts;
            sink->pts_offset_us += bump;
            norm_pts += bump;
        } else if (norm_pts < sink->next_pts_us[tk]) {
            norm_pts = sink->next_pts_us[tk];
        }

        /*
         * The hardware encoder emits 4-byte length prefixed NAL units (AVCC),
         * and the AAC frames are raw access units. Both go in verbatim; running
         * them through the Annex-B converter would corrupt any payload that
         * happens to contain a start code.
         */
        /*
         * The client sends decode time; the presentation time is decode time
         * plus the composition offset carried in the record header. Audio and
         * the VPU encoder path both send cto == 0.
         */
        int64_t dts_us = norm_pts;
        int64_t pts_us_out = is_audio ? norm_pts : norm_pts + (int64_t)cto_ms * 1000;
        if (pts_us_out < dts_us) pts_us_out = dts_us;
        int rc = fmp4_writer_add_sample_avcc(sink->writer, is_audio, data, data_size,
                                             pts_us_out, dts_us, dur_us, (flags & 1) ? 1 : 0);
        if (rc != 0) {
            fprintf(stderr, "[SINK] add_sample failed rc=%d\n", rc);
        }
        sink->next_pts_us[tk] = norm_pts + (dur_us > 0 ? (int64_t)dur_us : 33333);
        pts_us = norm_pts;
    }

    sink->frames++;
    free(data);

    /*
     * Fragments end only on a key frame boundary (handled above), so that
     * every fragment can be decoded on its own. A size based cut would split
     * a group of pictures and leave a fragment that starts mid-GOP.
     */
    (void)sink->fragment_bytes;

    pthread_mutex_unlock(&sink->lock);
    return 1;
}

void fmp4_sink_request_boundary(Fmp4Sink *sink) {
    if (!sink) return;
    pthread_mutex_lock(&sink->lock);
    if (sink->bd_service_id != 0) {
        sink->boundary_pending = 1;
        fprintf(stderr, "[SINK] re-announcing program service=%d event=%d\n",
                sink->bd_service_id, sink->bd_event_id);
    }
    pthread_mutex_unlock(&sink->lock);
}

void fmp4_sink_reset_session(Fmp4Sink *sink) {
    if (!sink) return;
    pthread_mutex_lock(&sink->lock);
    /*
     * The writer is kept: the Java side only sends the initialisation segment
     * once per session, so closing the writer here would leave the sink unable
     * to build fragments until the next session.
     */
    if (sink->writer) fmp4_writer_reset_samples(sink->writer);
    sink->frag.size = 0;
    /*
     * Readers ask for "the fragments after N" and a fresh reader sends N=0,
     * so the first fragment of a session has to be numbered 1. Starting at 0
     * made every session lose its opening fragment: two seconds of video and
     * the audio that started with it.
     */
    sink->seq = 1;
    sink->have_pts = 0;
    sink->next_pts_us[0] = 0;
    sink->next_pts_us[1] = 0;
    sink->pts_offset_us = 0;
    sink->boundary_pending = 0;
    sink->bd_service_id = 0;
    sink->bd_event_id = 0;
    sink->frames = 0;
    sink->fragments = 0;
    fprintf(stderr, "[SINK] capture session reset\n");
    pthread_mutex_unlock(&sink->lock);
}

uint64_t fmp4_sink_frames(const Fmp4Sink *sink) { return sink ? sink->frames : 0; }
uint64_t fmp4_sink_fragments(const Fmp4Sink *sink) { return sink ? sink->fragments : 0; }

void fmp4_sink_close(Fmp4Sink *sink) {
    if (!sink) return;
    if (sink->client_fd >= 0) close(sink->client_fd);
    if (sink->listen_fd >= 0) {
        close(sink->listen_fd);
    }
    if (sink->writer) fmp4_writer_close(sink->writer);
    free(sink->sps);
    free(sink->pps);
    free(sink->frag.data);
    pthread_mutex_destroy(&sink->lock);
    free(sink);
}
