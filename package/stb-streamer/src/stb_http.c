#include "stb_http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#define REQUEST_PATH "/dev/shm/pix/capture.req"
#define MAX_READERS 8

static int64_t now_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

struct StbHttpServer {
    StbHttpConfig cfg;
    int listen_fd;
    pthread_t thread;
    pthread_t monitor;
    volatile int running;

    pthread_mutex_t state_lock;
    int readers;              /* readers currently streaming */
    char channel[32];         /* channel being captured, "" = idle */
    int64_t last_reader_ms;   /* when the last reader left */

    /*
     * Encoder settings the reader asked for (0/empty = leave the box default).
     * They travel in the capture request so the capture agent can write them
     * into live.conf before the next session starts.
     */
    char req_codec[8];
    int req_width;
    int req_height;
    int req_bitrate;
    char req_mode[8];   /* rate control: "vbr" | "cbr" */
    int req_gop;        /* keyframe interval in seconds */

    /* Feature sideband version the reader asked for; 0 = no sideband. */
    int req_features;
    int feature_restart;   /* a new reader started; realign the sideband */
    int64_t feature_restart_ms;
};

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * Tells the capture agent what the reader wants. The file is replaced
 * atomically so the agent never reads a half written line.
 */
static void write_request(const char *state, const char *channel, const char *codec,
                          int width, int height, int bitrate,
                          const char *mode, int gop) {
    /* The directory must exist before the temporary file is opened. */
    mkdir("/dev/shm/pix", 0777);
    char tmp[80];
    snprintf(tmp, sizeof(tmp), "%s.tmp", REQUEST_PATH);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "[HTTP] cannot write the capture request\n");
        return;
    }
    if (channel && channel[0]) {
        /* "start <ch> [<codec> <w> <h> <bitrate>]" - the format is optional so
         * an older reader still works and the box keeps its own defaults. */
        if (codec && codec[0] && width > 0 && height > 0 && bitrate > 0) {
            if (mode && mode[0] && gop > 0) {
                fprintf(f, "%s %s %s %d %d %d %s %d\n", state, channel, codec,
                        width, height, bitrate, mode, gop);
            } else {
                fprintf(f, "%s %s %s %d %d %d\n", state, channel, codec, width, height, bitrate);
            }
        } else {
            fprintf(f, "%s %s\n", state, channel);
        }
    } else {
        fprintf(f, "%s\n", state);
    }
    fclose(f);
    if (rename(tmp, REQUEST_PATH) != 0) {
        fprintf(stderr, "[HTTP] cannot publish the capture request\n");
    }
}

/* Returns the value of ?<key>=... in a query string, or NULL. */
static const char *query_value(const char *query, const char *key) {
    size_t klen = strlen(key);
    const char *p = query;

    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            return p + klen + 1;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return NULL;
}

typedef struct {
    StbHttpServer *srv;
    int client_fd;
    struct sockaddr_in peer;
} ClientContext;

static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_http_chunk(int fd, const void *buf, size_t len) {
    if (!len) return 0;
    char hdr[32];
    int n = snprintf(hdr, sizeof(hdr), "%zx\r\n", len);
    if (n <= 0) return -1;
    if (write_all(fd, hdr, (size_t)n) != 0) return -1;
    if (write_all(fd, buf, len) != 0) return -1;
    return write_all(fd, "\r\n", 2);
}

/* Sends the initialisation segment once it is available. */
static int send_init_segment(int fd, RamSpool *spool, int *sent) {
    if (!spool || *sent) return 0;
    uint8_t *copy = NULL;
    size_t size = 0;

    pthread_mutex_lock(&spool->lock);
    if (spool->init_data && spool->init_size > 0) {
        size = spool->init_size;
        copy = malloc(size);
        if (copy) memcpy(copy, spool->init_data, size);
    }
    pthread_mutex_unlock(&spool->lock);

    if (!copy) return 0;   /* not produced yet */
    int rc = send_http_chunk(fd, copy, size);
    free(copy);
    if (rc != 0) return -1;
    *sent = 1;
    return 0;
}

static void handle_stream(ClientContext *ctx, const char *query, const char *channel) {
    int fd = ctx->client_fd;
    StbHttpServer *srv = ctx->srv;
    RamSpool *spool = srv->cfg.spool;

    /*
     * Ask for the requested channel. A different channel means a new capture
     * session: the spool and the fragment numbering restart so the reader
     * never receives fragments from the previous channel.
     */
    pthread_mutex_lock(&srv->state_lock);
    int switched = 0;
    if (channel[0] && strcmp(channel, srv->channel) != 0) {
        snprintf(srv->channel, sizeof(srv->channel), "%s", channel);
        switched = 1;
    }
    srv->readers++;
    /*
     * Feature sideband. Only "features=v1" enables it, and the capture side
     * reads this value before every fragment. An old reader never sends the
     * parameter, so the spool content stays exactly as before.
     */
    srv->req_features = 0;
    srv->feature_restart = 0;
    if (query) {
        const char *pf = query_value(query, "features");
        if (pf && pf[0] == 'v' && pf[1] == '1' &&
            (pf[2] == '\0' || pf[2] == '&'))
            srv->req_features = 1;
    }
    pthread_mutex_unlock(&srv->state_lock);

    if (switched) {
        fprintf(stderr, "[HTTP] switching to channel %s\n", channel);
        if (srv->cfg.on_channel_change) {
            srv->cfg.on_channel_change(srv->cfg.on_channel_user, channel);
        }
    }
    /*
     * Optional encoder settings from the reader, e.g.
     *   /v1/streams/17?after=42&codec=hevc&w=1440&h=1080&bitrate=5000000
     * They only take effect when the capture (re)starts, so a format change
     * also has to restart the session.
     */
    if (query) {
        const char *v = query_value(query, "codec");
        const char *pw = query_value(query, "w");
        const char *ph = query_value(query, "h");
        const char *pb = query_value(query, "bitrate");

        pthread_mutex_lock(&srv->state_lock);
        if (v) {
            size_t n = strcspn(v, "&");
            if (n >= sizeof(srv->req_codec)) n = sizeof(srv->req_codec) - 1;
            memcpy(srv->req_codec, v, n);
            srv->req_codec[n] = '\0';
        }
        if (pw) srv->req_width = atoi(pw);
        if (ph) srv->req_height = atoi(ph);
        if (pb) srv->req_bitrate = atoi(pb);
        {
            const char *pm = query_value(query, "mode");
            const char *pg = query_value(query, "gop");
            if (pm) {
                size_t n = strcspn(pm, "&");
                if (n >= sizeof(srv->req_mode)) n = sizeof(srv->req_mode) - 1;
                memcpy(srv->req_mode, pm, n);
                srv->req_mode[n] = '\0';
            }
            if (pg) srv->req_gop = atoi(pg);
        }
        pthread_mutex_unlock(&srv->state_lock);
    }
    write_request("start", channel[0] ? channel : srv->channel,
                  srv->req_codec, srv->req_width, srv->req_height, srv->req_bitrate,
                  srv->req_mode, srv->req_gop);

    /* after パラメータの取得 (?after=123) */
    uint32_t after_seq = 0;
    if (query) {
        const char *p = strstr(query, "after=");
        if (p) {
            after_seq = (uint32_t)strtoul(p + 6, NULL, 10);
        }
    }

    /* HTTP chunked レスポンスヘッダ */
    const char *resp_hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: video/mp4\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "\r\n";
    if (write_all(fd, resp_hdr, strlen(resp_hdr)) != 0) return;

    /* 1. 初期化セグメント (ftyp + moov) をまず chunk 送信 */
    int init_sent = 0;
    if (send_init_segment(fd, spool, &init_sent) != 0) {
        goto done;
    }

    /*
     * The STB may have restarted, which restarts the fragment numbering. A
     * reader that resumes from a sequence number the STB can no longer reach
     * would wait forever, so an unreachable request is served from the
     * oldest fragment that is still held.
     */
    uint32_t max_seq = ram_spool_max_seq(spool);
    if (after_seq > max_seq) {
        fprintf(stderr, "[HTTP] after_seq=%u is beyond the current max %u, restarting from the oldest fragment\n",
                after_seq, max_seq);
        after_seq = 0;
    }

    fprintf(stderr, "[HTTP] stream request after_seq=%u\n", after_seq);
    size_t used0 = 0, max0 = 0, cnt0 = 0;
    uint64_t pushed0 = 0, dropped0 = 0;
    ram_spool_stats(spool, &used0, &max0, &pushed0, &dropped0, &cnt0);
    fprintf(stderr, "[HTTP] spool has %zu fragments (%.1f/%.0f MB, dropped %llu)\n",
            cnt0, used0 / (1024.0 * 1024.0), max0 / (1024.0 * 1024.0),
            (unsigned long long)dropped0);

    /* Let the owner react to a fresh reader (for example re-announce the
     * program that is currently being recorded). */
    /*
     * The reader starts receiving from this moment. Anything the feature tap
     * collected before it belongs to media that was never sent, so ask the
     * owner to restart the sideband timeline here.
     */
    /*
     * Only the first reader of this process realigns. The streamer runs in
     * the same pipeline as the encoder, so its start is the start of the
     * media that will be sent; later readers are reconnects and must not
     * move the timeline backwards.
     */
    if (srv->feature_restart_ms == 0) {
        srv->feature_restart = 1;
        srv->feature_restart_ms = now_monotonic_ms();
    }
    if (ctx->srv->cfg.on_stream_client) {
        ctx->srv->cfg.on_stream_client(ctx->srv->cfg.on_stream_user);
    }

    /* 2. RAMスプールから指定 seq 以降の断片を順次 chunked 送信 */
    while (ctx->srv->running) {
        /*
         * A reader may connect before the capture has started, so the
         * initialisation segment can appear after this request began.
         */
        if (!init_sent) {
            if (send_init_segment(fd, spool, &init_sent) != 0) break;
        }

        SpoolFragment frag = {0};
        int rc = ram_spool_get_after_timeout(spool, after_seq, &frag, 1000);
        if (rc < 0) break; /* スプールクローズ */
        if (rc == 1) {
            int s_rc = send_http_chunk(fd, frag.data, frag.size);
            after_seq = frag.seq;
            free(frag.data);
            if (s_rc != 0) break;
            /*
             * With a single reader the fragments it has just been given are
             * dead weight: keeping them fills the spool and makes new
             * fragments get dropped, which shows up as gaps in a recording.
             */
            pthread_mutex_lock(&srv->state_lock);
            int only_reader = (srv->readers == 1);
            pthread_mutex_unlock(&srv->state_lock);
            if (only_reader) {
                ram_spool_drop_up_to(spool, frag.seq);
            }
        } else {
            /*
             * Nothing to send for a second. Readers that die without closing
             * the connection would otherwise keep the count up forever and the
             * capture would never go back to idle, so check the peer here.
             */
            char probe;
            ssize_t n = recv(fd, &probe, 1, MSG_DONTWAIT);
            if (n == 0) break;                        /* closed by the peer */
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
        }
    }

done:
    /* 終端チャンク (0\r\n\r\n) */
    write_all(fd, "0\r\n\r\n", 5);

    pthread_mutex_lock(&srv->state_lock);
    if (srv->readers > 0) srv->readers--;
    srv->last_reader_ms = now_ms();
    pthread_mutex_unlock(&srv->state_lock);
}

static void handle_status(ClientContext *ctx) {
    int fd = ctx->client_fd;
    RamSpool *spool = ctx->srv->cfg.spool;

    size_t used = 0, max_b = 0, count = 0;
    uint64_t pushed = 0, dropped = 0;
    if (spool) {
        ram_spool_stats(spool, &used, &max_b, &pushed, &dropped, &count);
    }

    pthread_mutex_lock(&ctx->srv->state_lock);
    char channel[32];
    snprintf(channel, sizeof(channel), "%s", ctx->srv->channel);
    int readers = ctx->srv->readers;
    pthread_mutex_unlock(&ctx->srv->state_lock);

    char body[512];
    int blen = snprintf(body, sizeof(body),
        "{\"status\":\"ok\",\"channel\":\"%s\",\"capturing\":%s,\"readers\":%d,"
        "\"spoolUsedBytes\":%zu,\"spoolMaxBytes\":%zu,"
        "\"fragmentsInSpool\":%zu,\"totalPushed\":%llu,\"totalDropped\":%llu}\n",
        channel, channel[0] ? "true" : "false", readers,
        used, max_b, count, (unsigned long long)pushed, (unsigned long long)dropped);

    char resp[1024];
    int hlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s", blen, body);

    write_all(fd, resp, (size_t)hlen);
}

/*
 * GET /v1/program - the programme eit_watch currently sees, verbatim.
 * A reader that connects in the middle of a programme has not received a
 * boundary box yet; this lets it name the segment it is about to build.
 */
static void handle_program(ClientContext *ctx) {
    int fd = ctx->client_fd;
    const char *path = ctx->srv->cfg.program_path;
    char body[768];
    size_t len = 0;

    body[0] = '\0';
    if (path) {
        FILE *f = fopen(path, "r");
        if (f) {
            len = fread(body, 1, sizeof(body) - 1, f);
            fclose(f);
        }
    }
    body[len] = '\0';

    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n", len);
    write_all(fd, hdr, (size_t)hlen);
    if (len > 0)
        write_all(fd, body, len);
}

int stb_http_take_feature_restart(StbHttpServer *srv) {    int pending = srv->feature_restart;
    srv->feature_restart = 0;
    return pending;
}

int stb_http_feature_version(StbHttpServer *srv) {
    if (!srv) return 0;
    pthread_mutex_lock(&srv->state_lock);
    int version = srv->req_features;
    pthread_mutex_unlock(&srv->state_lock);
    return version;
}

static void *client_thread(void *arg) {
    ClientContext *ctx = (ClientContext *)arg;
    int fd = ctx->client_fd;

    /* TCP_NODELAY 設定 (低遅延送信) */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /* A reader that stops reading must not block this thread forever. */
    struct timeval snd_timeout = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_timeout, sizeof(snd_timeout));

    /* リクエスト行読み取り (最大2048バイト) */
    char buf[2048];
    size_t total = 0;
    while (total + 1 < sizeof(buf)) {
        ssize_t n = recv(fd, buf + total, 1, 0);
        if (n <= 0) break;
        if (buf[total] == '\n') {
            total++;
            break;
        }
        total++;
    }
    buf[total] = '\0';

    char method[16] = {0}, uri[1024] = {0}, proto[16] = {0};
    if (sscanf(buf, "%15s %1023s %15s", method, uri, proto) == 3) {
        char *query = strchr(uri, '?');
        if (query) *query++ = '\0';

        if (strcmp(method, "GET") == 0 && strncmp(uri, "/v1/streams/", 12) == 0) {
            char channel[32];
            snprintf(channel, sizeof(channel), "%s", uri + 12);
            handle_stream(ctx, query, channel);
        } else if (strcmp(method, "GET") == 0 && strcmp(uri, "/v1/status") == 0) {
            handle_status(ctx);
        } else if (strcmp(method, "GET") == 0 && strcmp(uri, "/v1/program") == 0) {
            handle_program(ctx);
        } else if (strcmp(method, "POST") == 0 && strcmp(uri, "/v1/stop") == 0) {
            if (ctx->srv->cfg.stop_flag) {
                *ctx->srv->cfg.stop_flag = 1;
            }
            const char *ok = "HTTP/1.1 200 OK\r\nContent-Length: 15\r\nConnection: close\r\n\r\n{\"stopped\":true}";
            write_all(fd, ok, strlen(ok));
        } else {
            const char *nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            write_all(fd, nf, strlen(nf));
        }
    }

    close(fd);
    free(ctx);
    return NULL;
}

static void *server_thread(void *arg) {
    StbHttpServer *srv = (StbHttpServer *)arg;

    while (srv->running) {
        struct sockaddr_in peer;
        socklen_t len = sizeof(peer);
        int cfd = accept(srv->listen_fd, (struct sockaddr *)&peer, &len);
        if (cfd < 0) {
            if (!srv->running) break;
            if (errno == EINTR) continue;
            usleep(10000);
            continue;
        }

        /*
         * 接続元IP検査 (セキュリティ)。許可リストはカンマ区切りで複数書ける:
         * NAS は 192.168.99.80 と 192.168.99.52 の 2 つのアドレスを持ち、
         * 経路の選択次第でどちらから来るか変わる。片方だけ許可していると、
         * 経路が切り替わった瞬間に全録画が 403 で止まる（2026-11-05 に発生）。
         */
        char peer_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
        int allowed = strcmp(peer_ip, "127.0.0.1") == 0;
        const char *list = srv->cfg.allowed_ip;
        while (!allowed && list && *list) {
            const char *comma = strchr(list, ',');
            size_t n = comma ? (size_t)(comma - list) : strlen(list);
            while (n > 0 && (*list == ' ' || *list == '\t')) { list++; n--; }
            while (n > 0 && (list[n - 1] == ' ' || list[n - 1] == '\t')) n--;
            if (n > 0 && strlen(peer_ip) == n && strncmp(list, peer_ip, n) == 0) allowed = 1;
            if (!comma) break;
            list = comma + 1;
        }

        if (!allowed) {
            const char *forbidden = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            write_all(cfd, forbidden, strlen(forbidden));
            close(cfd);
            continue;
        }

        ClientContext *ctx = malloc(sizeof(*ctx));
        if (!ctx) { close(cfd); continue; }
        ctx->srv = srv;
        ctx->client_fd = cfd;
        ctx->peer = peer;

        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 128 * 1024); /* スタックを128KBに制限 (省メモリ) */

        if (pthread_create(&th, &attr, client_thread, ctx) != 0) {
            close(cfd);
            free(ctx);
        }
        pthread_attr_destroy(&attr);
    }
    return NULL;
}


/* Stops the capture once nobody has been reading for a while. */
static void *monitor_thread(void *arg) {
    StbHttpServer *srv = (StbHttpServer *)arg;
    int64_t timeout = (int64_t)srv->cfg.idle_timeout_sec * 1000;
    if (timeout <= 0) return NULL;

    while (srv->running) {
        for (int i = 0; i < 10 && srv->running; i++) usleep(100 * 1000);

        pthread_mutex_lock(&srv->state_lock);
        int idle = (srv->readers == 0 && srv->channel[0]
                    && now_ms() - srv->last_reader_ms >= timeout);
        if (idle) srv->channel[0] = '\0';
        pthread_mutex_unlock(&srv->state_lock);

        if (idle) {
            fprintf(stderr, "[HTTP] no reader for %ds, stopping the capture\n",
                    srv->cfg.idle_timeout_sec);
            write_request("stop", 0, NULL, 0, 0, 0, NULL, 0);
        }
    }
    return NULL;
}

StbHttpServer *stb_http_start(const StbHttpConfig *cfg) {
    if (!cfg || cfg->port <= 0) return NULL;

    StbHttpServer *srv = calloc(1, sizeof(*srv));
    if (!srv) return NULL;
    srv->cfg = *cfg;
    srv->running = 1;
    pthread_mutex_init(&srv->state_lock, NULL);
    srv->last_reader_ms = now_ms();

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { free(srv); return NULL; }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)cfg->port),
        .sin_addr.s_addr = INADDR_ANY
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 8) != 0) {
        close(fd);
        free(srv);
        return NULL;
    }

    srv->listen_fd = fd;
    if (pthread_create(&srv->thread, NULL, server_thread, srv) != 0) {
        close(fd);
        free(srv);
        return NULL;
    }
    if (cfg->idle_timeout_sec > 0) {
        pthread_create(&srv->monitor, NULL, monitor_thread, srv);
    }

    return srv;
}

void stb_http_stop(StbHttpServer *srv) {
    if (!srv) return;
    srv->running = 0;
    if (srv->listen_fd >= 0) {
        shutdown(srv->listen_fd, SHUT_RDWR);
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }
    pthread_join(srv->thread, NULL);
    if (srv->cfg.idle_timeout_sec > 0) {
        pthread_join(srv->monitor, NULL);
    }
    pthread_mutex_destroy(&srv->state_lock);
    free(srv);
}
