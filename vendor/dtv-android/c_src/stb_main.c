#include "fmp4_writer.h"
#include "ram_spool.h"
#include "stb_http.h"
#include "fmp4_sink.h"
#include "mdns_announce.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <string.h>
#include <pthread.h>

static volatile int g_stop = 0;
static Fmp4Sink *g_sink;
static RamSpool *g_spool;
/* Set by start_mdns(), possibly from the retry thread. */
static MdnsAnnouncer *g_mdns;

static void on_stream_client(void *user) {
    (void)user;
    fmp4_sink_request_boundary(g_sink);
}

static void on_channel_change(void *user, const char *channel) {
    (void)user;
    /* A new channel means a new session: old fragments must not be served. */
    ram_spool_clear(g_spool);
    fmp4_sink_reset_session(g_sink);
    fprintf(stderr, "[STB] channel switched to %s\n", channel);
}
static void on_sig(int s) { (void)s; g_stop = 1; }

typedef struct {
    uint8_t *buf;
    size_t size;
    size_t cap;
} MemBuffer;

static int mem_write_fn(void *user, const void *data, size_t size) {
    MemBuffer *b = (MemBuffer *)user;
    if (b->size + size > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 65536;
        while (ncap < b->size + size) ncap *= 2;
        uint8_t *p = realloc(b->buf, ncap);
        if (!p) return -1;
        b->buf = p;
        b->cap = ncap;
    }
    memcpy(b->buf + b->size, data, size);
    b->size += size;
    return 0;
}

static uint32_t r32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* 通常MP4 (pes_720p.mp4) をパースして、本物のH.264実波サンプルをfMP4逐次生成器へ流し込む */
static int feed_live_h264_from_mp4(const char *mp4_path, RamSpool *spool) {
    FILE *fp = fopen(mp4_path, "rb");
    if (!fp) return -1;

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *d = malloc((size_t)fsize);
    if (!d || fread(d, 1, (size_t)fsize, fp) != (size_t)fsize) {
        free(d); fclose(fp); return -1;
    }
    fclose(fp);

    /* avcC を探して SPS / PPS を抽出 */
    const uint8_t *sps = NULL, *pps = NULL;
    size_t sps_len = 0, pps_len = 0;
    uint32_t width = 1280, height = 720;

    for (size_t i = 0; i + 8 <= (size_t)fsize; i++) {
        if (memcmp(d + i + 4, "avcC", 4) == 0) {
            size_t p = i + 8;
            p += 5; /* configVersion, profile, compat, level, lengthSizeMinusOne */
            uint8_t num_sps = d[p++] & 0x1f;
            if (num_sps > 0) {
                sps_len = (size_t)((d[p] << 8) | d[p+1]); p += 2;
                sps = d + p; p += sps_len;
            }
            uint8_t num_pps = d[p++];
            if (num_pps > 0) {
                pps_len = (size_t)((d[p] << 8) | d[p+1]); p += 2;
                pps = d + p;
            }
            break;
        }
    }

    if (!sps || !pps) {
        free(d);
        return -1;
    }

    /* 1. 初期化セグメント (ftyp + moov[mvex]) の生成 */
    MemBuffer init_mem = {0};
    Fmp4Writer *iw = fmp4_writer_open(&init_mem, mem_write_fn);
    fmp4_writer_set_video_h264(iw, (int)width, (int)height, sps, sps_len, pps, pps_len);
    const uint8_t asc[] = { 0x11, 0x90 }; /* AAC 48kHz stereo */
    fmp4_writer_set_audio_aac(iw, 48000, 2, asc, sizeof(asc));
    fmp4_writer_write_init(iw);
    fmp4_writer_close(iw);

    ram_spool_set_init(spool, init_mem.buf, init_mem.size);
    fprintf(stderr, "[STB] Real H.264 init segment registered (%zu bytes, %dx%d)\n", init_mem.size, width, height);
    free(init_mem.buf);

    /* stsz (サンプルサイズ一覧) と stco/co64 (最初のチャンク位置) を探す */
    uint32_t sample_count = 0;
    const uint8_t *stsz_entries = NULL;
    uint64_t first_chunk_offset = 0;

    for (size_t i = 0; i + 8 <= (size_t)fsize; i++) {
        if (memcmp(d + i + 4, "stsz", 4) == 0) {
            sample_count = r32(d + i + 16);
            stsz_entries = d + i + 20;
        } else if (memcmp(d + i + 4, "co64", 4) == 0 && first_chunk_offset == 0) {
            first_chunk_offset = ((uint64_t)r32(d + i + 16) << 32) | r32(d + i + 20);
        } else if (memcmp(d + i + 4, "stco", 4) == 0 && first_chunk_offset == 0) {
            first_chunk_offset = r32(d + i + 16);
        }
    }

    if (!stsz_entries || first_chunk_offset == 0 || sample_count == 0) {
        free(d);
        return -1;
    }

    fprintf(stderr, "[STB] Found %u real video frames in live MP4 (first chunk at %llu)\n",
            sample_count, (unsigned long long)first_chunk_offset);

    /* サンプルを読み出し、fMP4断片として逐次組み立ててスプールへ push */
    uint32_t seq = 1;
    size_t sidx = 0;
    int64_t pts = 0;
    uint64_t cur_sample_offset = first_chunk_offset;

    while (sidx < sample_count && !g_stop) {
        MemBuffer frag_mem = {0};
        Fmp4Writer *fw = fmp4_writer_open(&frag_mem, mem_write_fn);
        fmp4_writer_set_video_h264(fw, (int)width, (int)height, sps, sps_len, pps, pps_len);
        fmp4_writer_set_audio_aac(fw, 48000, 2, asc, sizeof(asc));

        /* 1断片あたり約30フレームずつパック */
        size_t batch = 30;
        if (sidx + batch > sample_count) batch = sample_count - sidx;

        for (size_t k = 0; k < batch; k++) {
            size_t idx = sidx + k;
            uint32_t sz = r32(stsz_entries + idx * 4);

            if (cur_sample_offset + sz <= (size_t)fsize) {
                int is_key = (k == 0);
                /* AVCC 形式 (4バイト length-prefixed) をそのままダイレクト投入 (超高速・完全一致) */
                fmp4_writer_add_sample_avcc(fw, 0, d + cur_sample_offset, sz, pts, pts, 33333, is_key);
            }
            cur_sample_offset += sz;
            pts += 33333;
        }

        sidx += batch;
        fmp4_writer_flush_fragment(fw, seq);
        fmp4_writer_close(fw);

        if (frag_mem.size > 0) {
            ram_spool_push(spool, seq, frag_mem.buf, frag_mem.size);
            size_t used = 0, max_b = 0, count = 0;
            uint64_t pushed = 0, dropped = 0;
            ram_spool_stats(spool, &used, &max_b, &pushed, &dropped, &count);
            fprintf(stderr, "[STB] Real H.264 fragment seq=%u pushed (size=%zu bytes, spool used: %.2f MB / %zu MB)\n",
                    seq, frag_mem.size, (double)used / (1024 * 1024), max_b / (1024 * 1024));
            seq++;
        }
        free(frag_mem.buf);
    }

    free(d);
    return 0;
}

/* Picks the IPv4 address of the interface that carries the LAN. */
static int primary_ipv4(struct in_addr *out) {
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0) return -1;
    int found = -1;
    for (struct ifaddrs *ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
        if (strncmp(ifa->ifa_name, "eth", 3) == 0) {
            *out = sin->sin_addr;
            found = 0;
            break;
        }
        if (found != 0) {
            *out = sin->sin_addr;
            found = 0;
        }
    }
    freeifaddrs(list);
    return found;
}

/* Instance name from the address: all four octets, so two boxes on the same
 * /24 do not announce the same name and hide each other. */
static void instance_name(struct in_addr ip, char *out, size_t size) {
    snprintf(out, size, "pixstb-%02x%02x%02x%02x",
             (unsigned)(ip.s_addr & 0xff),
             (unsigned)((ip.s_addr >> 8) & 0xff),
             (unsigned)((ip.s_addr >> 16) & 0xff),
             (unsigned)((ip.s_addr >> 24) & 0xff));
}

struct MdnsRetry {
    MdnsAnnounceConfig cfg;
    char host[64];
    char display[96];
};

/*
 * Keeps looking for an address and starts the announcer when one appears.
 *
 * The streamer is started from init at boot, before the network is up. Asking
 * for the address once used to disable discovery for the whole process: the box
 * streamed fine but never appeared in the NAS discovery list, which reads as
 * "no response" on the management page. Two seconds between attempts costs
 * nothing and recovers as soon as the interface comes up.
 */
static void *mdns_retry_start(void *arg) {
    struct MdnsRetry *retry = arg;
    for (int attempt = 0; attempt < 300; ++attempt) {
        struct in_addr ip;
        if (primary_ipv4(&ip) == 0) {
            instance_name(ip, retry->host, sizeof(retry->host));
            retry->cfg.addr = ip;
            g_mdns = mdns_announce_start(&retry->cfg);
            fprintf(stderr, "[MDNS] announcing as %s after %d s\n", retry->host,
                    attempt * 2);
            free(retry);
            return NULL;
        }
        sleep(2);
    }
    fprintf(stderr, "[MDNS] still no IPv4 address after 10 minutes; discovery stays off\n");
    free(retry);
    return NULL;
}

static void start_mdns(int port, const char *name, int channel) {
    struct MdnsRetry *retry = calloc(1, sizeof(*retry));
    if (!retry) return;
    retry->cfg.port = port;
    retry->cfg.host = retry->host;
    retry->cfg.display = retry->display;
    retry->cfg.channel = channel;
    snprintf(retry->display, sizeof(retry->display), "%s", name);

    struct in_addr ip;
    if (primary_ipv4(&ip) == 0) {
        instance_name(ip, retry->host, sizeof(retry->host));
        retry->cfg.addr = ip;
        g_mdns = mdns_announce_start(&retry->cfg);
        free(retry);
        return;
    }
    fprintf(stderr, "[MDNS] no IPv4 address yet, retrying in the background\n");
    pthread_t thread;
    if (pthread_create(&thread, NULL, mdns_retry_start, retry) != 0) {
        fprintf(stderr, "[MDNS] cannot start the retry thread\n");
        free(retry);
        return;
    }
    pthread_detach(thread);
}

int main(int argc, char **argv) {
    int port = 40774;
    const char *allowed_ip = "192.168.99.80";
    size_t spool_mb = 256;
    const char *live_mp4 = "/dev/shm/pes_720p.mp4";
    int sink_port = 0;
    size_t fragment_mb = 2;
    /* Identity announced over mDNS. The defaults are the first unit's; a second
     * box overrides them so the NAS list does not show two identical rows. */
    int mdns_channel = 20;
    const char *mdns_name = "X96Max";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--allowed-ip") == 0 && i + 1 < argc) allowed_ip = argv[++i];
        else if (strcmp(argv[i], "--spool-mb") == 0 && i + 1 < argc) spool_mb = (size_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--input-mp4") == 0 && i + 1 < argc) live_mp4 = argv[++i];
        else if (strcmp(argv[i], "--sink-port") == 0 && i + 1 < argc) sink_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--fragment-mb") == 0 && i + 1 < argc) fragment_mb = (size_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--mdns-channel") == 0 && i + 1 < argc) mdns_channel = atoi(argv[++i]);
        else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) mdns_name = argv[++i];
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    fprintf(stderr, "=== STB Live Streamer (Torne Real H.264 -> fMP4 -> NAS) ===\n");
    fprintf(stderr, "Port: %d, Allowed IP: %s, Spool: %zu MB, Input: %s\n", port, allowed_ip, spool_mb, live_mp4);
    fprintf(stderr, "eMMC writes: 0 bytes (Pure RAM mode)\n");

    RamSpool *spool = ram_spool_create(spool_mb * 1024 * 1024);
    g_spool = spool;
    if (!spool) return 1;

    StbHttpConfig http_cfg = {
        .port = port,
        .allowed_ip = allowed_ip,
        .spool = spool,
        .stop_flag = &g_stop,
        .on_stream_client = on_stream_client,
        .on_stream_user = NULL,
        .on_channel_change = on_channel_change,
        .on_channel_user = NULL,
        .idle_timeout_sec = 30
    };

    StbHttpServer *srv = stb_http_start(&http_cfg);
    if (!srv) {
        ram_spool_destroy(spool);
        return 1;
    }
    fprintf(stderr, "HTTP Server listening on 0.0.0.0:%d (NAS pull ready)\n", port);

    /*
     * Tell the network where this box is so the NAS can find it without anyone
     * typing an address. The service is announced over multicast DNS.
     */
    start_mdns(port, mdns_name, mdns_channel);

    if (sink_port > 0) {
        /* 本番モード: Java (HWトランスコード) からソケットでサンプルを受け取り、
           fMP4断片へ変換してRAMスプールへ投入する */
        fprintf(stderr, "[STB] Sink mode: waiting for Java client on 127.0.0.1:%d\n", sink_port);

        Fmp4SinkConfig scfg = {
            .port = sink_port,
            .spool = spool,
            .fragment_bytes = fragment_mb * 1024 * 1024
        };
        Fmp4Sink *sink = fmp4_sink_open(&scfg);
        g_sink = sink;
        if (!sink) {
            fprintf(stderr, "Failed to open sink port %d\n", sink_port);
            mdns_announce_stop(g_mdns);
    stb_http_stop(srv);
            ram_spool_destroy(spool);
            return 1;
        }

        uint64_t last_frames = 0;
        while (!g_stop) {
            int rc = fmp4_sink_poll(sink, 200);
            if (rc < 0) {
                fprintf(stderr, "[STB] sink client gone, waiting for reconnect\n");
                usleep(500000);
            }
            uint64_t frames = fmp4_sink_frames(sink);
            uint64_t frags = fmp4_sink_fragments(sink);
            if (frames != last_frames) {
                last_frames = frames;
                size_t used = 0, max_b = 0, count = 0;
                uint64_t pushed = 0, dropped = 0;
                ram_spool_stats(spool, &used, &max_b, &pushed, &dropped, &count);
                fprintf(stderr, "[STB] frames=%llu frags=%llu spool=%.1f/%.0fMB dropped=%llu\n",
                        (unsigned long long)frames,
                        (unsigned long long)frags,
                        used / (1024.0 * 1024.0), max_b / (1024.0 * 1024.0),
                        (unsigned long long)dropped);
            }
        }
        fmp4_sink_close(sink);
    } else {
        /* オフライン検証モード: 生成済みMP4から実波H.264を取り込む */
        if (feed_live_h264_from_mp4(live_mp4, spool) != 0) {
            fprintf(stderr, "Failed to feed live H.264 from %s\n", live_mp4);
        }

        /* 配信ループ待機 */
        while (!g_stop) {
            usleep(100000);
        }
    }

    fprintf(stderr, "Shutting down STB Streamer...\n");
    mdns_announce_stop(g_mdns);
    stb_http_stop(srv);
    ram_spool_destroy(spool);
    fprintf(stderr, "STB Streamer cleanly stopped. eMMC writes: 0 bytes.\n");
    return 0;
}
