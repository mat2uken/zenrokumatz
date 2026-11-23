/*
 * pix-stream-recorder: NAS side recorder for the fMP4 streaming system.
 *
 * The NAS always initiates the TCP connection to the STB. One connection can
 * span several programs: the STB marks each program change with a "pixb" box
 * and this process splits the byte stream on those markers. Each program is
 * staged in tmpfs, turned into a normal faststart MP4, and published with the
 * JSON sidecar that the existing pix-archive already understands.
 *
 * Finalisation runs on a worker thread so the socket keeps draining while a
 * finished program is written out. That is what keeps the STB's RAM spool from
 * overflowing between programs.
 *
 * The only dependency is cJSON, which the NAS already links into pix-archive.
 */
#include "fmp4_to_mp4.h"
#include "cm_queue.h"

#include <cJSON.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    const char *stb_ip;
    int stb_port;
    const char *stream_id;
    const char *archive_url;
    const char *recordings_root;
    const char *tmpfs_dir;
    const char *state_dir;
    const char *channel;
    const char *codec;   /* encoder the STB should use: "hevc" or "avc" */
    const char *rate_mode; /* rate control: "vbr" or "cbr" */
    int gop_sec;           /* keyframe interval in seconds */
    int max_segment_sec;
    /* Publish gates: no storage for broadcast suspension or short fillers. */
    int min_publish_sec;
    int idle_discard_sec;
    /* Static pictures (colour bars, a frozen card) compress to a tiny bitrate. */
    int min_bitrate_kbps;
    int static_check_sec;
    int width;
    int height;
    int frame_rate_num;
    int frame_rate_den;
    int video_bitrate;
    int keep_staging;
    const char *cm_queue_dir;   /* fMP4 hand-off for the CM worker */
} Config;

/* One program being captured. */
typedef struct {
    int fd;
    char path[4096];
    int64_t wall_start;
    int64_t wall_end;
    uint32_t first_seq;
    uint32_t last_seq;
    uint32_t expect_seq;
    uint32_t gaps;
    int have_seq;
    int have_moof;

    int service_id;
    int event_id;
    int64_t prog_start_ms;
    int64_t prog_dur_ms;
    /* Programme name the STB decoded from the broadcast EIT (pixn box). */
    char name[256];
    /* Offset just past the last complete moof+mdat, so a trailing partial
     * fragment can be trimmed before the CM worker remuxes the file. */
    off_t complete_bytes;
} Segment;

typedef struct Job {
    Segment seg;
    struct Job *next;
} Job;

static Config g_cfg;

/* Saved init segment so every program file starts with its own ftyp and moov. */
static uint8_t *g_ftyp;
static size_t g_ftyp_len;
static uint8_t *g_moov;
static size_t g_moov_len;

static Job *g_head;
static Job *g_tail;
static pthread_mutex_t g_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_queue_cond = PTHREAD_COND_INITIALIZER;
static int g_worker_stop;

static volatile sig_atomic_t g_stop;

static void on_sig(int s) {
    (void)s;
    g_stop = 1;
}

/* ---- helpers ---- */

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd64(const uint8_t *p) {
    return ((uint64_t)rd32(p) << 32) | (uint64_t)rd32(p + 4);
}

static void write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (n == 0) return;
        p += n;
        len -= (size_t)n;
    }
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

/*
 * The STB answers with Transfer-Encoding: chunked, so the box stream is
 * interleaved with chunk framing. This reader hides the framing and hands the
 * caller a plain byte stream.
 */
typedef struct {
    int fd;
    size_t chunk_left;
} Body;

static int body_read(Body *b, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    while (n > 0) {
        if (b->chunk_left == 0) {
            char line[64];
            size_t k = 0;
            for (;;) {
                char c;
                ssize_t r = recv(b->fd, &c, 1, 0);
                if (r <= 0) return -1;
                if (c == '\n') {
                    if (k > 0 && line[k - 1] == '\r') k--;
                    line[k] = '\0';
                    break;
                }
                if (k + 1 < sizeof(line)) line[k++] = c;
            }
            if (k == 0) continue; /* trailer line between chunks */
            unsigned long sz = strtoul(line, NULL, 16);
            if (sz == 0) return -1; /* last chunk */
            b->chunk_left = (size_t)sz;
        }
        size_t want = n < b->chunk_left ? n : b->chunk_left;
        if (read_exact(b->fd, p, want) != 0) return -1;
        p += want;
        n -= want;
        b->chunk_left -= want;
        if (b->chunk_left == 0) {
            char crlf[2];
            if (read_exact(b->fd, crlf, 2) != 0) return -1;
        }
    }
    return 0;
}

static void make_dirs(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(tmp, 0755);
        *p = '/';
    }
    mkdir(tmp, 0755);
}

/* Keeps a UTF-8 string within max bytes without cutting a character. */
static void truncate_utf8(char *s, size_t max_bytes) {
    size_t n = strlen(s);
    if (n <= max_bytes) return;
    size_t cut = max_bytes;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
    s[cut] = '\0';
}

static void sanitize(char *s) {
    for (char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '/' || c == '\\' || c < 0x20) *p = '_';
    }
}

/* Reads the codec of the video track from the initialisation segment. */
static const char *detect_video_codec(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return "h264";
    uint8_t buf[65536];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    for (size_t i = 0; i + 4 <= n; i++) {
        if (memcmp(buf + i, "hvc1", 4) == 0 || memcmp(buf + i, "hev1", 4) == 0) return "hevc";
    }
    return "h264";
}

static void make_uuid(char *out) {
    unsigned char b[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, b, sizeof(b));
        (void)n;
        close(fd);
    } else {
        for (size_t i = 0; i < sizeof(b); i++) b[i] = (unsigned char)(rand() & 0xFF);
    }
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

/*
 * Keep one broadcast programme in one recording.
 *
 * The STB reconnects on its own schedule (PIX_CAPTURE_SECONDS, 30 minutes by
 * default) and the reader opens a fresh segment every time it reconnects.
 * Neither event means the programme changed, but this recorder used to start a
 * new recording folder each time, so a two hour programme arrived as four
 * folders with the same title.
 *
 * When the programme that is starting is already on disk for the same day,
 * reuse that folder and only advance the part number.
 */

/* The folder is "<HHMMSS>_<programme name>__<uuid>"; take the name back out. */
static int dir_program_name(const char *name, char *out, size_t cap) {
    const char *sep = strstr(name, "__");
    if (!sep) return 0;
    const char *under = strchr(name, '_');
    if (!under || under > sep) return 0;
    size_t n = (size_t)(sep - under - 1);
    if (n == 0 || n >= cap) return 0;
    memcpy(out, under + 1, n);
    out[n] = '\0';
    return 1;
}

/* Takes the trailing digits of "__pNN.json" as N, or 0 when it is not one. */
static int part_number(const char *fname) {
    const char *q = strstr(fname, "__p");
    if (!q) return 0;
    char *end = NULL;
    long part = strtol(q + 3, &end, 10);
    if (!end || strcmp(end, ".json") != 0) return 0;
    if (part <= 0 || part > 999) return 0;
    return (int)part;
}

/* Does the first part of this folder describe the programme we are starting? */
static int json_matches_program(const char *jpath, int event_id, int64_t start_ms,
                                const char *name) {
    FILE *f = fopen(jpath, "r");
    if (!f) return 0;
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return 0;
    int match = 0;
    cJSON *pg = cJSON_GetObjectItem(root, "program");
    if (cJSON_IsObject(pg)) {
        cJSON *ev = cJSON_GetObjectItem(pg, "eventId");
        cJSON *st = cJSON_GetObjectItem(pg, "startAt");
        cJSON *nm = cJSON_GetObjectItem(pg, "name");
        int ev_ok = cJSON_IsNumber(ev) && event_id > 0 && (int)ev->valuedouble == event_id;
        int st_ok = cJSON_IsNumber(st) && start_ms > 0 &&
                    (int64_t)st->valuedouble == start_ms;
        /*
         * Event id alone is enough: it identifies one broadcast programme.
         * Without it (the EIT did not arrive yet) fall back to the start time
         * together with the name so two different programmes that happen to
         * start at the same second cannot be joined.
         */
        if (ev_ok) match = 1;
        else if (st_ok && name && name[0] && cJSON_IsString(nm) &&
                 strcmp(nm->valuestring, name) == 0)
            match = 1;
    }
    cJSON_Delete(root);
    return match;
}

/*
 * Looks for a recording of the same programme on the same day and, when it
 * exists, hands back its folder and the highest part number in it. Returns 0
 * when this is a new programme.
 */
static int find_continuation(const char *base_dir, int event_id, int64_t start_ms,
                             const char *prog_name,
                             char *out_dir, size_t out_cap,
                             char *out_recording_id, size_t out_rid_cap) {
    DIR *d = opendir(base_dir);
    if (!d) return 0;

    struct dirent *e;
    int best_part = 0;
    char best_dir[4096] = "";
    char best_rid[37] = "";

    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;

        /* The STB sends the EIT title, which is exactly what named the folder. */
        char name[512];
        if (!dir_program_name(e->d_name, name, sizeof(name))) continue;
        if (strcmp(name, prog_name) != 0) continue;

        char dir[4096];
        if (snprintf(dir, sizeof(dir), "%s/%s", base_dir, e->d_name) >= (int)sizeof(dir))
            continue;

        DIR *dd = opendir(dir);
        if (!dd) continue;
        int max_part = 0, first_matches = 0;
        struct dirent *f;
        while ((f = readdir(dd)) != NULL) {
            int part = part_number(f->d_name);
            if (part <= 0) continue;
            if (part > max_part) max_part = part;
            if (part != 1) continue;
            char jpath[4096];
            if (snprintf(jpath, sizeof(jpath), "%s/%s", dir, f->d_name) >= (int)sizeof(jpath))
                continue;
            first_matches = json_matches_program(jpath, event_id, start_ms, prog_name);
        }
        closedir(dd);

        if (!first_matches || max_part <= 0) continue;
        if (max_part < best_part) continue;

        best_part = max_part;
        snprintf(best_dir, sizeof(best_dir), "%s", dir);
        best_rid[0] = '\0';
        /* Carry the recording id so every part belongs to one recording. */
        DIR *dr = opendir(dir);
        if (dr) {
            struct dirent *g;
            while ((g = readdir(dr)) != NULL) {
                if (part_number(g->d_name) != 1) continue;
                char jpath[4096];
                if (snprintf(jpath, sizeof(jpath), "%s/%s", dir, g->d_name) >=
                    (int)sizeof(jpath))
                    continue;
                FILE *jf = fopen(jpath, "r");
                if (!jf) continue;
                char buf[65536];
                size_t n = fread(buf, 1, sizeof(buf) - 1, jf);
                fclose(jf);
                buf[n] = '\0';
                cJSON *jr = cJSON_Parse(buf);
                if (jr) {
                    cJSON *rid = cJSON_GetObjectItem(jr, "recordingId");
                    if (cJSON_IsString(rid) && rid->valuestring[0])
                        snprintf(best_rid, sizeof(best_rid), "%s", rid->valuestring);
                    cJSON_Delete(jr);
                }
                break;
            }
            closedir(dr);
        }
    }
    closedir(d);

    if (best_part <= 0) return 0;
    snprintf(out_dir, out_cap, "%s", best_dir);
    if (out_recording_id && out_rid_cap)
        out_recording_id[0] = '\0';
    if (best_rid[0] && out_recording_id && out_rid_cap > strlen(best_rid))
        snprintf(out_recording_id, out_rid_cap, "%s", best_rid);
    return best_part;
}

/* ---- minimal HTTP GET for the archive API ---- */

static void split_host_port(const char *url, char *host, size_t host_cap, int *port) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t i = 0;
    while (*p && *p != ':' && *p != '/' && i + 1 < host_cap) host[i++] = *p++;
    host[i] = '\0';
    *port = 80;
    if (*p == ':') *port = atoi(p + 1);
}

static char *http_get(const char *host, int port, const char *path) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { close(fd); return NULL; }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return NULL; }

    char req[512];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
                      path, host, port);
    if (write(fd, req, (size_t)rl) != rl) { close(fd); return NULL; }

    size_t cap = 1 << 20, len = 0;
    char *buf = malloc(cap);
    if (!buf) { close(fd); return NULL; }
    for (;;) {
        if (len + 65536 > cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); close(fd); return NULL; }
            buf = nb;
            cap *= 2;
        }
        ssize_t n = recv(fd, buf + len, 65536, 0);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(fd);

    char *sep = memmem(buf, len, "\r\n\r\n", 4);
    if (!sep) { free(buf); return NULL; }
    size_t off = (size_t)(sep - buf) + 4;
    size_t blen = len - off;
    char *body = malloc(blen + 1);
    if (!body) { free(buf); return NULL; }
    memcpy(body, buf + off, blen);
    body[blen] = '\0';
    free(buf);
    return body;
}

static cJSON *fetch_array(const char *api_path) {
    char host[256];
    int port = 80;
    split_host_port(g_cfg.archive_url, host, sizeof(host), &port);
    char *body = http_get(host, port, api_path);
    if (!body) return NULL;
    cJSON *parsed = cJSON_Parse(body);
    free(body);
    if (!parsed) return NULL;
    if (!cJSON_IsArray(parsed)) {
        cJSON *items = cJSON_GetObjectItem(parsed, "items");
        if (cJSON_IsArray(items)) {
            cJSON_Delete(parsed);
            return items;
        }
    }
    return parsed;
}

/* ---- box scanning ---- */

/* Reads the sequence number out of the mfhd box of a movie fragment. */
static void scan_mfhd(const uint8_t *b, size_t len, Segment *seg) {
    size_t p = 0;
    while (p + 8 <= len) {
        uint32_t sz = rd32(b + p);
        if (sz < 8 || p + sz > len) break;
        if (memcmp(b + p + 4, "mfhd", 4) == 0 && sz >= 16) {
            uint32_t seq = rd32(b + p + 12);
            if (!seg->have_seq) {
                seg->first_seq = seq;
                seg->expect_seq = seq;
                seg->have_seq = 1;
            } else if (seq > seg->expect_seq) {
                seg->gaps += seq - seg->expect_seq;
            }
            if (seq > seg->last_seq) seg->last_seq = seq;
            seg->expect_seq = seq + 1;
        }
        p += sz;
    }
}

/* ---- segment handling ---- */

static void segment_open(Segment *seg) {
    memset(seg, 0, sizeof(*seg));
    seg->fd = -1;
    snprintf(seg->path, sizeof(seg->path), "%s/staging_%d_%" PRId64 ".fmp4",
             g_cfg.tmpfs_dir, (int)getpid(), now_ms());
    seg->fd = open(seg->path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    seg->wall_start = now_ms();
    if (seg->fd < 0) {
        fprintf(stderr, "cannot open staging %s: %s\n", seg->path, strerror(errno));
        return;
    }
    /* Every program file needs its own init segment. */
    if (g_ftyp_len) write_all(seg->fd, g_ftyp, g_ftyp_len);
    if (g_moov_len) write_all(seg->fd, g_moov, g_moov_len);
}

static void enqueue_job(Segment *seg) {
    if (!seg->have_moof || seg->fd < 0) {
        if (seg->fd >= 0) close(seg->fd);
        seg->fd = -1;
        return;
    }
    close(seg->fd);
    seg->fd = -1;
    seg->wall_end = now_ms();

    Job *job = malloc(sizeof(*job));
    if (!job) return;
    job->seg = *seg;
    job->next = NULL;

    pthread_mutex_lock(&g_queue_lock);
    if (g_tail) g_tail->next = job; else g_head = job;
    g_tail = job;
    pthread_cond_signal(&g_queue_cond);
    pthread_mutex_unlock(&g_queue_lock);
}

/* ---- publishing one program ---- */

static cJSON *find_service_by_id(int service_id) {
    cJSON *services = fetch_array("/api/archive/services");
    if (!services) return NULL;
    cJSON *found = NULL;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, services) {
        cJSON *sid = cJSON_GetObjectItem(item, "serviceId");
        if (!cJSON_IsNumber(sid)) continue;
        if ((int)sid->valuedouble != service_id) continue;
        found = cJSON_Duplicate(item, 1);
        break;
    }
    cJSON_Delete(services);
    return found;
}

/* Finds the service that carries the configured channel number. */
static cJSON *find_service_by_channel(int *out_service_id) {
    cJSON *services = fetch_array("/api/archive/services");
    if (!services) return NULL;
    /* "17" is terrestrial; "BS15_0"/"CS..." is a satellite transponder. */
    const char *want_type = "GR";
    if (strncmp(g_cfg.channel, "BS", 2) == 0) want_type = "BS";
    else if (strncmp(g_cfg.channel, "CS", 2) == 0) want_type = "CS";
    cJSON *found = NULL;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, services) {
        cJSON *chan = cJSON_GetObjectItem(item, "channel");
        if (!cJSON_IsObject(chan)) continue;
        cJSON *num = cJSON_GetObjectItem(chan, "channel");
        cJSON *type = cJSON_GetObjectItem(chan, "type");
        if (!cJSON_IsString(num) || !cJSON_IsString(type)) continue;
        if (strcmp(type->valuestring, want_type) != 0) continue;
        if (strcmp(num->valuestring, g_cfg.channel) != 0) continue;
        found = cJSON_Duplicate(item, 1);
        cJSON *sid = cJSON_GetObjectItem(item, "serviceId");
        if (out_service_id && cJSON_IsNumber(sid)) {
            *out_service_id = (int)sid->valuedouble;
        }
        break;
    }
    cJSON_Delete(services);
    return found;
}

/* Finds the program whose broadcast window contains the given wall clock. */
static cJSON *find_program_by_clock(int service_id, int64_t wall_ms) {
    if (wall_ms <= 0) return NULL;
    cJSON *programs = fetch_array("/api/archive/programs");
    if (!programs) return NULL;
    cJSON *found = NULL;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, programs) {
        cJSON *sid = cJSON_GetObjectItem(item, "serviceId");
        cJSON *st = cJSON_GetObjectItem(item, "startAt");
        cJSON *du = cJSON_GetObjectItem(item, "duration");
        if (!cJSON_IsNumber(st) || !cJSON_IsNumber(du)) continue;
        if (service_id > 0 && cJSON_IsNumber(sid) && (int)sid->valuedouble != service_id) continue;
        int64_t start = (int64_t)st->valuedouble;
        int64_t end = start + (int64_t)du->valuedouble;
        if (wall_ms >= start && wall_ms < end) {
            found = cJSON_Duplicate(item, 1);
            break;
        }
    }
    cJSON_Delete(programs);
    return found;
}

static cJSON *find_program_by_event(int service_id, int event_id,
                                    int64_t start_ms, int64_t dur_ms) {
    cJSON *programs = fetch_array("/api/archive/programs");
    if (!programs) return NULL;

    cJSON *exact = NULL;
    cJSON *near = NULL;
    int64_t near_delta = INT64_MAX;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, programs) {
        cJSON *eid = cJSON_GetObjectItem(item, "eventId");
        cJSON *sid = cJSON_GetObjectItem(item, "serviceId");
        cJSON *st = cJSON_GetObjectItem(item, "startAt");
        if (!cJSON_IsNumber(st)) continue;
        int64_t s = (int64_t)st->valuedouble;
        if (cJSON_IsNumber(eid) && cJSON_IsNumber(sid)
            && (int)eid->valuedouble == event_id
            && (int)sid->valuedouble == service_id) {
            exact = cJSON_Duplicate(item, 1);
            break;
        }
        /*
         * Fall back to the program whose start time is closest. It has to be
         * the same service: when the archive EPG is stale, matching by clock
         * alone picked a programme from another channel and the sidecar
         * described the wrong broadcast.
         */
        if (start_ms > 0 &&
            (service_id <= 0 || !cJSON_IsNumber(sid) ||
             (int)sid->valuedouble == service_id)) {
            int64_t d = s > start_ms ? s - start_ms : start_ms - s;
            if (d < near_delta) {
                near_delta = d;
                cJSON_Delete(near);
                near = cJSON_Duplicate(item, 1);
            }
        }
    }
    cJSON_Delete(programs);
    if (exact) { cJSON_Delete(near); return exact; }
    if (near && start_ms > 0 && near_delta < (dur_ms > 0 ? dur_ms : 7200000)) return near;
    cJSON_Delete(near);
    return NULL;
}

static cJSON *build_json(const Segment *seg, cJSON *service, cJSON *program,
                        const char *mp4_rel,
                        int64_t media_duration_ms, int64_t frame_count,
                        const char *video_codec, const char *recording_id_in) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "schemaVersion", 1);

    {
        char recording_id[37];
        char part_id[37];
        /* A later part of the same programme keeps the first part's id. */
        if (recording_id_in && recording_id_in[0])
            snprintf(recording_id, sizeof(recording_id), "%s", recording_id_in);
        else
            make_uuid(recording_id);
        make_uuid(part_id);
        cJSON_AddStringToObject(root, "recordingId", recording_id);
        cJSON_AddStringToObject(root, "partId", part_id);
    }

    cJSON *svc = cJSON_AddObjectToObject(root, "service");
    if (service) {
        const char *keys[] = {"id", "serviceId", "networkId", "type", "logoId",
                              "remoteControlKeyId", "name", "hasLogoData"};
        for (size_t k = 0; k < sizeof(keys) / sizeof(*keys); k++) {
            cJSON *v = cJSON_GetObjectItem(service, keys[k]);
            if (v) cJSON_AddItemToObject(svc, keys[k], cJSON_Duplicate(v, 1));
        }
        cJSON *chan = cJSON_AddObjectToObject(svc, "channel");
        cJSON_AddStringToObject(chan, "type", "GR");
        cJSON_AddStringToObject(chan, "channel", g_cfg.channel);
    } else {
        cJSON_AddNumberToObject(svc, "id", 0);
        cJSON_AddNumberToObject(svc, "serviceId", seg->service_id);
        cJSON_AddNumberToObject(svc, "networkId", 0);
        cJSON_AddNumberToObject(svc, "type", 1);
        cJSON_AddStringToObject(svc, "name", "GR");
        cJSON *chan = cJSON_AddObjectToObject(svc, "channel");
        cJSON_AddStringToObject(chan, "type", "GR");
        cJSON_AddStringToObject(chan, "channel", g_cfg.channel);
    }

    cJSON *prog = cJSON_AddObjectToObject(root, "program");
    int64_t prog_start = seg->prog_start_ms;
    int64_t prog_dur = seg->prog_dur_ms;
    if (program) {
        const char *keys[] = {"id", "eventId", "serviceId", "networkId", "startAt",
                              "duration", "isFree", "name", "description", "extended",
                              "video", "audio", "audios", "genres"};
        for (size_t k = 0; k < sizeof(keys) / sizeof(*keys); k++) {
            cJSON *v = cJSON_GetObjectItem(program, keys[k]);
            if (v) cJSON_AddItemToObject(prog, keys[k], cJSON_Duplicate(v, 1));
        }
        cJSON *st = cJSON_GetObjectItem(program, "startAt");
        cJSON *du = cJSON_GetObjectItem(program, "duration");
        if (cJSON_IsNumber(st)) prog_start = (int64_t)st->valuedouble;
        if (cJSON_IsNumber(du)) prog_dur = (int64_t)du->valuedouble;
    } else {
        cJSON_AddNumberToObject(prog, "id", 0);
        cJSON_AddNumberToObject(prog, "eventId", seg->event_id);
        cJSON_AddNumberToObject(prog, "startAt", prog_start);
        cJSON_AddNumberToObject(prog, "duration", prog_dur);
        cJSON_AddStringToObject(prog, "name", "GR");
    }
    /* The EIT title is the truth for the sidecar as well. */
    if (seg->name[0]) {
        cJSON_DeleteItemFromObject(prog, "name");
        cJSON_AddStringToObject(prog, "name", seg->name);
    }

    /* A gap in the fragment sequence means the STB dropped part of the program. */
    int partial = seg->gaps > 0;
    cJSON_AddStringToObject(root, "state", partial ? "partial" : "complete");
    cJSON_AddBoolToObject(root, "partialStart", partial ? 1 : 0);
    cJSON_AddBoolToObject(root, "partialEnd", 0);
    cJSON_AddStringToObject(root, "stopReason", "program-end");
    cJSON_AddNumberToObject(root, "missingFragments", (int)seg->gaps);

    cJSON *onair = cJSON_AddObjectToObject(root, "onAir");
    if (program) {
        cJSON *ev = cJSON_GetObjectItem(program, "eventId");
        if (ev) cJSON_AddItemToObject(onair, "eventId", cJSON_Duplicate(ev, 1));
    }
    cJSON_AddNumberToObject(onair, "startAt", prog_start);
    cJSON_AddNumberToObject(onair, "duration", prog_dur);

    cJSON *capture = cJSON_AddObjectToObject(root, "capture");
    cJSON_AddNumberToObject(capture, "startAt", seg->wall_start);
    cJSON_AddNumberToObject(capture, "endAt", seg->wall_end);
    int64_t offset = prog_start - seg->wall_start;
    cJSON_AddNumberToObject(capture, "programStartOffsetMs", offset > 0 ? offset : 0);
    cJSON_AddNumberToObject(capture, "missingHeadMs", offset > 0 ? offset : 0);

    cJSON *media = cJSON_AddObjectToObject(root, "media");
    cJSON_AddStringToObject(media, "relativePath", mp4_rel);
    cJSON_AddNumberToObject(media, "width", g_cfg.width);
    cJSON_AddNumberToObject(media, "height", g_cfg.height);
    cJSON_AddStringToObject(media, "videoCodec", video_codec);
    cJSON_AddStringToObject(media, "audioCodec", "aac");
    cJSON_AddNumberToObject(media, "audioSampleRate", 48000);
    cJSON_AddNumberToObject(media, "audioChannels", 2);
    cJSON_AddNumberToObject(media, "frameRateNumerator", g_cfg.frame_rate_num);
    cJSON_AddNumberToObject(media, "frameRateDenominator", g_cfg.frame_rate_den);
    cJSON_AddNumberToObject(media, "videoBitrate", g_cfg.video_bitrate);
    cJSON_AddNumberToObject(media, "durationMs", media_duration_ms);
    cJSON_AddNumberToObject(media, "frameCount", (int)frame_count);

    return root;
}

/* Fallback used only when the CM queue cannot take the job. */
static int write_json(cJSON *root, const char *json_path) {
    char *text = cJSON_PrintUnformatted(root);
    int rc = -1;
    if (text) {
        int jfd = open(json_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (jfd >= 0) {
            write_all(jfd, text, strlen(text));
            write_all(jfd, "\n", 1);
            close(jfd);
            rc = 0;
        }
        free(text);
    }
    return rc;
}

static int publish(const Segment *seg) {
    /*
     * 放送休止（空PAT）や番組の切れ端は保存しない。番組が乗っていない
     * 区間は event_id が 0 のままで、短い区間は局名だけのファイルになる。
     * どちらも NAS の容量と CM ワーカーの時間を使うだけなので捨てる。
     */
    int64_t span_ms = seg->wall_end > seg->wall_start
                          ? seg->wall_end - seg->wall_start : 0;
    if (seg->event_id <= 0) {
        fprintf(stderr, "discarded: no programme on air (%s, %.1fs)\n",
                seg->path, span_ms / 1000.0);
        if (!g_cfg.keep_staging) unlink(seg->path);
        return 1;
    }
    if (span_ms > 0 && span_ms < (int64_t)g_cfg.min_publish_sec * 1000) {
        fprintf(stderr, "discarded: %.1fs is shorter than %ds (%s)\n",
                span_ms / 1000.0, g_cfg.min_publish_sec, seg->path);
        if (!g_cfg.keep_staging) unlink(seg->path);
        return 1;
    }
    /*
     * 放送休止のカラーバーや停止した静止画は、動きが無いので符号化後の
     * ビットレートが極端に低くなる（実測: 静止区間 237 kbps、通常番組
     * 4〜6 Mbps、静止の多い通販番組でも 600 kbps）。長時間かつ低レートなら
     * 番組として保存しない。
     */
    if (seg->complete_bytes > 0 && span_ms >= (int64_t)g_cfg.static_check_sec * 1000) {
        int64_t kbps = (int64_t)seg->complete_bytes * 8 / (span_ms / 1000) / 1000;
        if (kbps < g_cfg.min_bitrate_kbps) {
            fprintf(stderr, "discarded: static picture (%lld kbps over %.0fs, %s)\n",
                    (long long)kbps, span_ms / 1000.0, seg->path);
            if (!g_cfg.keep_staging) unlink(seg->path);
            return 1;
        }
    }

    time_t tstart = (time_t)(seg->wall_start / 1000);
    struct tm tmstart;
    localtime_r(&tstart, &tmstart);

    char day[16], hm[16], filedate[16];
    strftime(day, sizeof(day), "%Y/%m/%d", &tmstart);
    strftime(hm, sizeof(hm), "%H%M%S", &tmstart);
    strftime(filedate, sizeof(filedate), "%Y%m%d", &tmstart);

    int resolved_service = seg->service_id;
    cJSON *service = resolved_service > 0 ? find_service_by_id(resolved_service) : NULL;
    cJSON *program = NULL;
    if (resolved_service > 0) {
        program = find_program_by_event(resolved_service, seg->event_id,
                                        seg->prog_start_ms, seg->prog_dur_ms);
    }
    if (!program) {
        /*
         * The program marker only survives while the reader stays connected, so
         * a fresh connection may not have one. The channel and the wall clock
         * still identify the program from the archive EPG.
         */
        program = find_program_by_clock(resolved_service, seg->wall_start);
    }
    if (!service) {
        /* Several services can share a channel number, so prefer the one the
         * resolved program belongs to. */
        int from_program = 0;
        if (program) {
            cJSON *sid = cJSON_GetObjectItem(program, "serviceId");
            if (cJSON_IsNumber(sid)) from_program = (int)sid->valuedouble;
        }
        if (from_program > 0) {
            service = find_service_by_id(from_program);
            if (service) resolved_service = from_program;
        }
        if (!service) service = find_service_by_channel(&resolved_service);
    }

    char service_name[256] = "GR";
    char service_type[8] = "GR";
    int64_t service_row_id = 0;
    if (service) {
        cJSON *ch = cJSON_GetObjectItem(service, "channel");
        if (cJSON_IsObject(ch)) {
            cJSON *t = cJSON_GetObjectItem(ch, "type");
            if (cJSON_IsString(t) && t->valuestring[0])
                snprintf(service_type, sizeof(service_type), "%s", t->valuestring);
        }
        cJSON *n = cJSON_GetObjectItem(service, "name");
        if (cJSON_IsString(n)) snprintf(service_name, sizeof(service_name), "%s", n->valuestring);
        cJSON *id = cJSON_GetObjectItem(service, "id");
        if (cJSON_IsNumber(id)) service_row_id = (int64_t)id->valuedouble;
    }
    sanitize(service_name);

    /*
     * The name from the broadcast EIT needs no EPG service and is the title
     * that was actually on air, so it wins. The archive EPG and the channel
     * name are only fallbacks for boxes that do not send it.
     */
    char prog_name[512] = "";
    if (seg->name[0])
        snprintf(prog_name, sizeof(prog_name), "%s", seg->name);
    if (!prog_name[0] && program) {
        cJSON *n = cJSON_GetObjectItem(program, "name");
        if (cJSON_IsString(n)) snprintf(prog_name, sizeof(prog_name), "%s", n->valuestring);
    }
    if (!prog_name[0])
        snprintf(prog_name, sizeof(prog_name), "%s", service_name);
    sanitize(prog_name);
    /*
     * The directory is "<HHMMSS>_<name>__<uuid>". File systems cap a name at
     * 255 bytes, so a long EIT title has to be cut or the publish fails and
     * the recording is lost.
     */
    truncate_utf8(prog_name, 255 - (6 + 1 + 2 + 36));

    char recording_id[37];
    make_uuid(recording_id);

    /*
     * The folder belongs to the programme, not to the segment. A programme
     * that crosses midnight would otherwise put its part 2 into the next
     * day's folder, where find_continuation() cannot see part 1, and every
     * night's late programmes would split in two. File names below keep the
     * part's own capture time.
     */
    char folder_day[16], folder_hm[16];
    snprintf(folder_day, sizeof(folder_day), "%s", day);
    snprintf(folder_hm, sizeof(folder_hm), "%s", hm);
    {
        cJSON *st = program ? cJSON_GetObjectItem(program, "startAt") : NULL;
        if (cJSON_IsNumber(st) && st->valuedouble > 0) {
            time_t pt = (time_t)(st->valuedouble / 1000);
            struct tm ptm;
            localtime_r(&pt, &ptm);
            strftime(folder_day, sizeof(folder_day), "%Y/%m/%d", &ptm);
            strftime(folder_hm, sizeof(folder_hm), "%H%M%S", &ptm);
        }
    }

    /*
     * Reuse the folder of the same programme instead of starting another one.
     * A reconnect or the STB's periodic restart must not split a programme.
     */
    char service_dir[4096];
    snprintf(service_dir, sizeof(service_dir), "%s/%s/%s_%" PRId64 "_%s",
             g_cfg.recordings_root, folder_day, service_type, service_row_id, service_name);

    char dir[4096], base[1024], mp4_path[4096], json_path[4096], rel[4096];
    int part = 1;
    char prior_dir[4096];
    char prior_rid[37];
    int prior_part = find_continuation(service_dir, seg->event_id, seg->prog_start_ms,
                                       prog_name, prior_dir, sizeof(prior_dir),
                                       prior_rid, sizeof(prior_rid));
    if (prior_part > 0) {
        snprintf(dir, sizeof(dir), "%s", prior_dir);
        part = prior_part + 1;
        /* The whole programme keeps one recording id across its parts. */
        if (prior_rid[0]) snprintf(recording_id, sizeof(recording_id), "%s", prior_rid);
        fprintf(stderr, "continuing programme as part %d: %s\n", part, dir);
    } else {
        snprintf(dir, sizeof(dir), "%s/%s_%s__%s",
                 service_dir, folder_hm, prog_name, recording_id);
    }
    make_dirs(dir);
    snprintf(base, sizeof(base), "%s_%s_%s__p%02d", filedate, hm, prog_name, part);
    if (snprintf(mp4_path, sizeof(mp4_path), "%s/%s.mp4", dir, base) >= (int)sizeof(mp4_path)) {
        fprintf(stderr, "target path too long\n");
        cJSON_Delete(service);
        cJSON_Delete(program);
        return 1;
    }
    if (snprintf(json_path, sizeof(json_path), "%s/%s.json", dir, base)
        >= (int)sizeof(json_path)) {
        fprintf(stderr, "json path too long\n");
        cJSON_Delete(service);
        cJSON_Delete(program);
        return 1;
    }
    {
        /* The folder keeps the time and name of the first part. */
        const char *leaf = strrchr(dir, '/');
        leaf = leaf ? leaf + 1 : dir;
        snprintf(rel, sizeof(rel), "%s/%s_%" PRId64 "_%s/%s/%s.mp4",
                 day, service_type, service_row_id, service_name, leaf, base);
    }

    /*
     * Hand the fMP4 to the CM worker. It remuxes to MP4, measures the
     * programme, cuts the ad breaks and writes the sidecar, so the recorder no
     * longer converts the file itself. Handing over the whole fMP4 (not the
     * converted MP4) keeps the CM worker's packet-level validation possible.
     */
    if (seg->complete_bytes > 0) {
        if (truncate(seg->path, seg->complete_bytes) != 0)
            fprintf(stderr, "cannot trim staging to %lld: %s\n",
                    (long long)seg->complete_bytes, strerror(errno));
    }
    if (g_cfg.cm_queue_dir && g_cfg.cm_queue_dir[0]) {
        cJSON *root = build_json(seg, service, program, rel, 0, 0,
                                 detect_video_codec(seg->path), recording_id);
        if (root) {
            char original[8192];
            cJSON *rid = cJSON_GetObjectItem(root, "recordingId");
            /* The queue remembers results by this id, so it must be unique
             * per part: find_continuation() inherits the recordingId across
             * parts, and a reused id makes the service treat part 2 and
             * beyond as already-done, drop them, and delete their fMP4. */
            cJSON *pid = cJSON_GetObjectItem(root, "partId");
            const char *job_id = cJSON_IsString(pid) ? pid->valuestring
                                 : cJSON_IsString(rid) ? rid->valuestring
                                 : recording_id;
            /* job_id points into root; root is deleted before the log line. */
            char job_id_copy[64];
            snprintf(job_id_copy, sizeof(job_id_copy), "%s", job_id);
            if (snprintf(original, sizeof(original), "%s/%s", g_cfg.recordings_root, rel)
                >= (int)sizeof(original))
                original[0] = '\0';
            int rc = original[0]
                         ? cm_queue_submit(g_cfg.cm_queue_dir, job_id, seg->path, original,
                                           json_path, g_cfg.keep_staging, root)
                         : -1;
            cJSON_Delete(root);
            if (rc == 0) {
                fprintf(stderr, "cm fmp4 queued: %s producer=%s pid=%d input=%s\n",
                        job_id_copy, PIX_CM_PRODUCER, (int)getpid(), seg->path);
                cJSON_Delete(service);
                cJSON_Delete(program);
                return 0;
            }
            fprintf(stderr, "cm enqueue failed; converting locally instead\n");
        }
    }

    /* Fallback: no queue configured, or the queue rejected the job. */
    int64_t media_duration_ms = 0, samples = 0, video_samples = 0;
    if (fmp4_to_mp4_finalize_ex(seg->path, mp4_path, g_cfg.keep_staging ? 0 : 1,
                                &media_duration_ms, &samples, &video_samples) != 0) {
        fprintf(stderr, "finalize failed for %s\n", seg->path);
        if (!g_cfg.keep_staging) unlink(seg->path);
        cJSON_Delete(service);
        cJSON_Delete(program);
        return 1;
    }

    cJSON *root = build_json(seg, service, program, rel, media_duration_ms, video_samples,
                             detect_video_codec(mp4_path), recording_id);
    if (root) {
        write_json(root, json_path);
        cJSON_Delete(root);
    }

    fprintf(stderr, "published: %s (media %.1fs, %lld frames, gaps %u, %s)\n",
            rel, media_duration_ms / 1000.0, (long long)video_samples,
            seg->gaps, seg->gaps > 0 ? "partial" : "complete");

    cJSON_Delete(service);
    cJSON_Delete(program);
    return 0;
}

static void *worker_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_queue_lock);
        while (!g_head && !g_worker_stop) {
            pthread_cond_wait(&g_queue_cond, &g_queue_lock);
        }
        if (!g_head && g_worker_stop) {
            pthread_mutex_unlock(&g_queue_lock);
            break;
        }
        Job *job = g_head;
        g_head = job->next;
        if (!g_head) g_tail = NULL;
        pthread_mutex_unlock(&g_queue_lock);

        if (publish(&job->seg) != 0 && !g_cfg.keep_staging)
            unlink(job->seg.path);
        free(job);
    }
    return NULL;
}

/* ---- receive loop ---- */

static void handle_boundary(Segment *seg, const uint8_t *bd) {
    /*
     * bd is the box payload, so it starts after the 8 byte box header.
     * Layout: version(1) flags(3) service(2) event(2) reserved(4) start(8)
     *         duration(8) wallclock(8)
     */
    int next_service = (bd[4] << 8) | bd[5];
    int next_event = (bd[6] << 8) | bd[7];
    int64_t start_ms = (int64_t)rd64(bd + 12);
    int64_t dur_ms = (int64_t)rd64(bd + 20);
    int64_t wall_ms = (int64_t)rd64(bd + 28);

    fprintf(stderr, "boundary: service=%d event=%d start=%" PRId64 " dur=%" PRId64 "\n",
            next_service, next_event, start_ms, dur_ms);

    /*
     * A reconnecting reader is told again which program is on air. Adopt that
     * when this segment has no program yet, and ignore it when it names the
     * program already being recorded, so a repeated marker does not split a
     * program into two files.
     */
    if (seg->have_moof && seg->service_id == next_service && seg->event_id == next_event) {
        return;
    }
    if (seg->service_id == 0 && seg->event_id == 0) {
        seg->service_id = next_service;
        seg->event_id = next_event;
        seg->prog_start_ms = start_ms;
        seg->prog_dur_ms = dur_ms;
        seg->name[0] = '\0';
        return;
    }

    /* Close the program that just ended and start the next one. */
    enqueue_job(seg);
    segment_open(seg);
    seg->service_id = next_service;
    seg->event_id = next_event;
    seg->prog_start_ms = start_ms;
    seg->prog_dur_ms = dur_ms;
    if (seg->fd >= 0 && wall_ms > 0) seg->wall_start = wall_ms;
}

/*
 * A reader that connects in the middle of a programme gets no boundary box
 * until the next one starts, so ask the STB which programme is on air now.
 * Without this the first file after every reconnect would fall back to the
 * channel name even though the box knows the title.
 */
static void seed_program_from_stb(Segment *seg) {
    char *body = http_get(g_cfg.stb_ip, g_cfg.stb_port, "/v1/program");
    if (!body) return;

    char name[sizeof(seg->name)];
    name[0] = '\0';
    int service = 0, event = 0;
    int64_t start = 0, dur = 0;
    for (char *line = body; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl++ = '\0';
        if (!strncmp(line, "service=", 8)) service = atoi(line + 8);
        else if (!strncmp(line, "event=", 6)) event = atoi(line + 6);
        else if (!strncmp(line, "start=", 6)) start = atoll(line + 6);
        else if (!strncmp(line, "duration=", 9)) dur = atoll(line + 9);
        else if (!strncmp(line, "name=", 5)) snprintf(name, sizeof(name), "%s", line + 5);
        line = nl;
    }
    free(body);

    if (service <= 0 || event <= 0) return;
    /*
     * The STB state file can lag behind the broadcast: eit_watch only rewrites
     * it when the present section arrives, so a fresh connection may read the
     * programme that just ended. Seeding from it would label this segment with
     * the previous title until the next boundary. Only trust it when the
     * advertised window still contains now.
     */
    int64_t now = now_ms();
    if (start <= 0 || dur <= 0 || now < start || now >= start + dur) {
        fprintf(stderr, "seeded from STB: ignored stale programme %s (window %lld..%lld)\n",
                name, (long long)start, (long long)(start + dur));
        return;
    }
    if (seg->service_id == 0) {
        seg->service_id = service;
        seg->event_id = event;
        seg->prog_start_ms = start;
        seg->prog_dur_ms = dur;
    }
    if (name[0] && !seg->name[0])
        snprintf(seg->name, sizeof(seg->name), "%s", name);
    fprintf(stderr, "seeded from STB: service=%d event=%d name=%s\n",
            seg->service_id, seg->event_id, seg->name);
}

int main(int argc, char **argv) {
    g_cfg.stb_ip = "192.168.99.57";
    g_cfg.stb_port = 40774;
    g_cfg.stream_id = "live";
    g_cfg.archive_url = "http://192.168.99.80:40773";
    g_cfg.recordings_root = "/mnt/xblk/video/pix-smb400";
    g_cfg.tmpfs_dir = "/dev/shm/pix_stream";
    g_cfg.state_dir = "/home/ku/pix-stream-20261101";
    g_cfg.channel = "20";
    g_cfg.codec = "hevc";
    g_cfg.rate_mode = "vbr";
    g_cfg.gop_sec = 5;
    g_cfg.max_segment_sec = 7200;
    g_cfg.min_publish_sec = 180;
    g_cfg.idle_discard_sec = 600;
    g_cfg.min_bitrate_kbps = 350;
    g_cfg.static_check_sec = 300;
    g_cfg.width = 1280;
    g_cfg.height = 720;
    g_cfg.frame_rate_num = 30000;
    g_cfg.frame_rate_den = 1001;
    g_cfg.video_bitrate = 4000000;
    g_cfg.keep_staging = 0;
    g_cfg.cm_queue_dir = "/dev/shm/pix_cm_queue";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--stb-ip") && i + 1 < argc) g_cfg.stb_ip = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) g_cfg.stb_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stream-id") && i + 1 < argc) g_cfg.stream_id = argv[++i];
        else if (!strcmp(argv[i], "--archive-url") && i + 1 < argc) g_cfg.archive_url = argv[++i];
        else if (!strcmp(argv[i], "--recordings-root") && i + 1 < argc) g_cfg.recordings_root = argv[++i];
        else if (!strcmp(argv[i], "--tmpfs-dir") && i + 1 < argc) g_cfg.tmpfs_dir = argv[++i];
        else if (!strcmp(argv[i], "--state-dir") && i + 1 < argc) g_cfg.state_dir = argv[++i];
        else if (!strcmp(argv[i], "--channel") && i + 1 < argc) g_cfg.channel = argv[++i];
        else if (!strcmp(argv[i], "--codec") && i + 1 < argc) g_cfg.codec = argv[++i];
        else if (!strcmp(argv[i], "--rate-mode") && i + 1 < argc) g_cfg.rate_mode = argv[++i];
        else if (!strcmp(argv[i], "--gop") && i + 1 < argc) g_cfg.gop_sec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-segment-sec") && i + 1 < argc) g_cfg.max_segment_sec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--min-publish-sec") && i + 1 < argc) g_cfg.min_publish_sec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--idle-discard-sec") && i + 1 < argc) g_cfg.idle_discard_sec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--min-bitrate-kbps") && i + 1 < argc) g_cfg.min_bitrate_kbps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--static-check-sec") && i + 1 < argc) g_cfg.static_check_sec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) g_cfg.width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) g_cfg.height = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc) g_cfg.video_bitrate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--keep-staging")) g_cfg.keep_staging = 1;
        else if (!strcmp(argv[i], "--cm-queue-dir") && i + 1 < argc) g_cfg.cm_queue_dir = argv[++i];
    }

    if (g_cfg.cm_queue_dir && g_cfg.cm_queue_dir[0]) {
        /* The worker reads the fMP4 from tmpfs; a disk path would be copied
         * needlessly and could outlive the recording. */
        if (cm_require_tmpfs_directory(g_cfg.cm_queue_dir) != 0) {
            fprintf(stderr, "cm queue %s is not a usable tmpfs directory\n",
                    g_cfg.cm_queue_dir);
            return 1;
        }
        fprintf(stderr, "cm integration: producer=%s queue=%s\n",
                PIX_CM_PRODUCER, g_cfg.cm_queue_dir);
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    make_dirs(g_cfg.tmpfs_dir);

    /*
     * Do not reuse the previous connection's init segment: the codec may have
     * changed between runs, and mixing two initialisation segments makes the
     * finalizer describe the file with the wrong one.
     */
    free(g_ftyp);
    g_ftyp = NULL;
    g_ftyp_len = 0;
    free(g_moov);
    g_moov = NULL;
    g_moov_len = 0;

    pthread_t worker;
    if (pthread_create(&worker, NULL, worker_main, NULL) != 0) {
        fprintf(stderr, "cannot start the finalize worker\n");
        return 1;
    }

    /* Resume from the last fragment this recorder already stored. */
    uint32_t after_seq = 0;
    {
        char seq_path[4096];
        snprintf(seq_path, sizeof(seq_path), "%s/last_seq_%s_%d_%s",
                 g_cfg.state_dir, g_cfg.stb_ip, g_cfg.stb_port, g_cfg.channel);
        FILE *sf = fopen(seq_path, "r");
        if (sf) {
            unsigned v = 0;
            if (fscanf(sf, "%u", &v) == 1) after_seq = v;
            fclose(sf);
        }
    }

    fprintf(stderr, "pix-stream-recorder: stb=%s:%d channel=%s codec=%s %dx%d %dbps after=%u\n",
            g_cfg.stb_ip, g_cfg.stb_port, g_cfg.channel, g_cfg.codec,
            g_cfg.width, g_cfg.height, g_cfg.video_bitrate, after_seq);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_cfg.stb_port);
    if (inet_pton(AF_INET, g_cfg.stb_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "bad stb ip\n");
        return 1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("connect");
        return 1;
    }
    struct timeval rcvto = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof(rcvto));

    /*
     * The encoder settings travel with the request so the STB capturer can
     * write them into live.conf before it starts a session. That is what makes
     * the format selectable from the management page.
     */
    char req[512];
    int rl = snprintf(req, sizeof(req),
                      "GET /v1/streams/%s?after=%u&codec=%s&w=%d&h=%d&bitrate=%d&mode=%s&gop=%d HTTP/1.1\r\n"
                      "Host: %s:%d\r\n"
                      "User-Agent: pix-stream-recorder/2.0\r\nConnection: close\r\n\r\n",
                      g_cfg.stream_id, after_seq,
                      g_cfg.codec, g_cfg.width, g_cfg.height, g_cfg.video_bitrate,
                      g_cfg.rate_mode, g_cfg.gop_sec,
                      g_cfg.stb_ip, g_cfg.stb_port);
    write_all(fd, req, (size_t)rl);

    /* Skip the response headers. */
    {
        char line[512];
        size_t n = 0;
        int ok = 0;
        for (;;) {
            char c;
            ssize_t r = recv(fd, &c, 1, 0);
            if (r <= 0) break;
            if (c == '\n') {
                if (n > 0 && line[n - 1] == '\r') n--;
                line[n] = '\0';
                if (!ok) {
                    if (!strstr(line, " 200 ")) {
                        fprintf(stderr, "unexpected response: %s\n", line);
                        close(fd);
                        return 1;
                    }
                    ok = 1;
                } else if (n == 0) {
                    break;
                }
                n = 0;
            } else if (n + 1 < sizeof(line)) {
                line[n++] = c;
            }
        }
        if (!ok) { close(fd); return 1; }
    }

    Body body = { .fd = fd, .chunk_left = 0 };
    Segment seg;
    segment_open(&seg);
    seed_program_from_stb(&seg);

    int64_t deadline = now_ms() + (int64_t)g_cfg.max_segment_sec * 1000;
    uint8_t hdr[8];
    uint64_t total = 0;
    int stop = 0;

    while (!stop && !g_stop && now_ms() < deadline) {
        if (body_read(&body, hdr, 8) != 0) break;
        uint64_t size = rd32(hdr);
        char type[5];
        memcpy(type, hdr + 4, 4);
        type[4] = '\0';
        size_t hdr_len = 8;
        if (size == 1) {
            uint8_t ext[8];
            if (body_read(&body, ext, 8) != 0) break;
            size = rd64(ext);
            hdr_len = 16;
        } else if (size == 0) {
            break;
        }
        if (size < hdr_len) break;
        uint64_t payload = size - hdr_len;

        if (!strcmp(type, "pixb")) {
            uint8_t bd[36];
            if (body_read(&body, bd, 36) != 0) break;
            handle_boundary(&seg, bd);
            continue;
        }

        /*
         * The STB follows pixb with pixn, which carries the UTF-8 programme
         * name it decoded from the EIT. It belongs to the segment pixb just
         * opened, so keep it out of the media file and remember it.
         */
        if (!strcmp(type, "pixn")) {
            size_t want = payload < sizeof(seg.name) - 1
                              ? (size_t)payload : sizeof(seg.name) - 1;
            if (want > 0 && body_read(&body, seg.name, want) != 0) break;
            seg.name[want] = '\0';
            for (uint64_t left = payload - want; left > 0; ) {
                uint8_t junk[512];
                size_t n = left > sizeof(junk) ? sizeof(junk) : (size_t)left;
                if (body_read(&body, junk, n) != 0) { stop = 1; break; }
                left -= n;
            }
            continue;
        }

        /* The init segment is reused for every program file. */
        if (!strcmp(type, "ftyp") || !strcmp(type, "moov")) {
            if (payload <= (1u << 20)) {
                uint8_t *b = malloc(payload + hdr_len);
                if (!b) break;
                memcpy(b, hdr, hdr_len);
                if (body_read(&body, b + hdr_len, payload) != 0) { free(b); break; }
                uint8_t **slot = !strcmp(type, "ftyp") ? &g_ftyp : &g_moov;
                size_t *slot_len = !strcmp(type, "ftyp") ? &g_ftyp_len : &g_moov_len;
                free(*slot);
                *slot = b;
                *slot_len = payload + hdr_len;
                /* The first program file is already open, so the init segment
                 * has to be written as it arrives. Later files get their own
                 * copy from segment_open(). */
                if (seg.fd >= 0 && !seg.have_moof) {
                    write_all(seg.fd, b, payload + hdr_len);
                }
                total += size;
                continue;
            }
        }

        if (seg.fd < 0) {
            /* No current program yet: keep draining so the STB spool never fills. */
            uint8_t buf[65536];
            uint64_t left = payload;
            while (left > 0) {
                size_t want = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
                if (body_read(&body, buf, want) != 0) { stop = 1; break; }
                left -= want;
            }
            continue;
        }

        write_all(seg.fd, hdr, hdr_len);

        if (!strcmp(type, "moof")) {
            if (payload > (1u << 20)) break;
            uint8_t *b = malloc(payload);
            if (!b || body_read(&body, b, payload) != 0) { free(b); break; }
            scan_mfhd(b, payload, &seg);
            write_all(seg.fd, b, payload);
            free(b);
            seg.have_moof = 1;
        } else {
            uint8_t buf[65536];
            uint64_t left = payload;
            while (left > 0) {
                size_t want = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
                if (body_read(&body, buf, want) != 0) { stop = 1; break; }
                write_all(seg.fd, buf, want);
                left -= want;
            }
            if (left == 0)
                seg.complete_bytes = lseek(seg.fd, 0, SEEK_CUR);
        }
        total += size;

        /*
         * 放送休止に入ると番組境界が来なくなり、1本のセグメントが延々と
         * 育つ。番組が乗っていないまま idle_discard_sec を過ぎたら打ち切る
         * （publish 側で event_id 0 として捨てる）。番組の放送時間を過ぎて
         * 境界が来ない場合も、末尾の余分を持ち越さないよう打ち切る。
         */
        int64_t now = now_ms();
        if (seg.event_id <= 0 && g_cfg.idle_discard_sec > 0 &&
            now - seg.wall_start > (int64_t)g_cfg.idle_discard_sec * 1000) {
            fprintf(stderr, "no programme for %ds; closing the session\n",
                    g_cfg.idle_discard_sec);
            break;
        }
        if (seg.event_id > 0 && seg.prog_start_ms > 0 && seg.prog_dur_ms > 0 &&
            now > seg.prog_start_ms + seg.prog_dur_ms + 120000) {
            fprintf(stderr, "programme window ended without a boundary; closing\n");
            break;
        }
        if (seg.complete_bytes > 0 &&
            now - seg.wall_start >= (int64_t)g_cfg.static_check_sec * 1000) {
            int64_t kbps = (int64_t)seg.complete_bytes * 8 /
                           ((now - seg.wall_start) / 1000) / 1000;
            if (kbps < g_cfg.min_bitrate_kbps) {
                fprintf(stderr, "static picture (%lld kbps); closing the session\n",
                        (long long)kbps);
                break;
            }
        }
    }

    close(fd);

    /* Remember the last fragment before finalising, so a crash cannot lose the
     * resume point. */
    if (seg.have_seq) {
        char seq_path[4096];
        snprintf(seq_path, sizeof(seq_path), "%s/last_seq_%s_%d_%s",
                 g_cfg.state_dir, g_cfg.stb_ip, g_cfg.stb_port, g_cfg.channel);
        FILE *sf = fopen(seq_path, "w");
        if (sf) {
            fprintf(sf, "%u\n", seg.last_seq);
            fclose(sf);
        }
    }

    enqueue_job(&seg);

    pthread_mutex_lock(&g_queue_lock);
    g_worker_stop = 1;
    pthread_cond_signal(&g_queue_cond);
    pthread_mutex_unlock(&g_queue_lock);
    pthread_join(worker, NULL);

    fprintf(stderr, "receive finished: %" PRIu64 " bytes\n", total);
    free(g_ftyp);
    free(g_moov);
    return 0;
}
