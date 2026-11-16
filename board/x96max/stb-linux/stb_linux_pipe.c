/*
 * stb_linux_pipe.c - fMP4 を標準入力から受け、dtv_android と同じ HTTP で配信する
 *
 * Android 版は MediaCodec が作ったサンプルを fmp4_sink が断片化し、RamSpool に
 * 積んで stb_http で配信する。Linux 版では ffmpeg が同じ fMP4（ftyp + moov +
 * moof/mdat の繰り返し）を吐けるので、ここでは断片の切り出しだけを行う。
 *
 *   ffmpeg ... -movflags +frag_keyframe+empty_moov+default_base_moof -f mp4 pipe:1
 *        | stb_linux_pipe --port 40774 --allowed-ip 192.168.99.80
 *
 * 端末のストレージには書かない。積む先は RamSpool（メモリ）だけ。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>

#include "mdns_announce.h"
#include "ram_spool.h"
#include "stb_http.h"

static volatile int g_stop;

/*
 * Program boundary. dtv_android writes a top level "pixb" box in front of the
 * fragment that starts a new program so the NAS recorder can close the
 * previous one. eit_watch supplies the program identity; this process turns a
 * change into the box.
 */
typedef struct {
    int service_id;
    int event_id;
    int64_t start_ms;
    int64_t duration_ms;
    int64_t wall_ms;
    int valid;
} ProgramInfo;

static ProgramInfo g_program;
static int g_have_program;
static time_t g_last_boundary_at;
static uint32_t g_last_events[4];
static int g_last_event_count;

static void read_program_file(const char *path, ProgramInfo *out)
{
    if (!path)
        return;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    ProgramInfo p = {0};
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "service=", 8)) p.service_id = atoi(line + 8);
        else if (!strncmp(line, "event=", 6)) p.event_id = atoi(line + 6);
        else if (!strncmp(line, "start=", 6)) p.start_ms = atoll(line + 6);
        else if (!strncmp(line, "duration=", 9)) p.duration_ms = atoll(line + 9);
        else if (!strncmp(line, "wall=", 5)) p.wall_ms = atoll(line + 5);
    }
    fclose(f);
    if (p.service_id > 0 && p.event_id > 0) {
        p.valid = 1;
        *out = p;
    }
}

/*
 * moof の中に映像トラックの traf があるか。ffmpe は音声だけの moof を
 * 出すことがあり、そこで番組を区切るとセグメントの音声だけが前倒しに
 * なり、A/V の長さがずれる。区切りは映像を含む moof に限る。
 */
static int moof_has_video(const uint8_t *body, size_t len)
{
    size_t off = 0;
    while (off + 8 <= len) {
        uint32_t size = ((uint32_t)body[off] << 24) | ((uint32_t)body[off+1] << 16) |
                        ((uint32_t)body[off+2] << 8) | body[off+3];
        if (size < 8 || off + size > len)
            break;
        if (memcmp(body + off + 4, "traf", 4) == 0) {
            const uint8_t *t = body + off + 8;
            size_t tlen = size - 8;
            size_t toff = 0;
            while (toff + 8 <= tlen) {
                uint32_t ts = ((uint32_t)t[toff] << 24) | ((uint32_t)t[toff+1] << 16) |
                              ((uint32_t)t[toff+2] << 8) | t[toff+3];
                if (ts < 8 || toff + ts > tlen)
                    break;
                if (memcmp(t + toff + 4, "tfhd", 4) == 0 && ts >= 12) {
                    uint32_t track = ((uint32_t)t[toff+8] << 24) | ((uint32_t)t[toff+9] << 16) |
                                     ((uint32_t)t[toff+10] << 8) | t[toff+11];
                    if (track == 1)     /* ffmpeg: 1=video, 2=audio */
                        return 1;
                }
                toff += ts;
            }
        }
        off += size;
    }
    return 0;
}

/* 44 バイトの pixb ボックス。レイアウトは dtv_android の emit_boundary_box と同じ。 */
static void build_boundary_box(const ProgramInfo *p, uint8_t box[44])
{
    memset(box, 0, 44);
    box[3] = 44;
    memcpy(box + 4, "pixb", 4);
    box[8] = 1;
    box[12] = (uint8_t)(p->service_id >> 8);
    box[13] = (uint8_t)p->service_id;
    box[14] = (uint8_t)(p->event_id >> 8);
    box[15] = (uint8_t)p->event_id;
    for (int i = 0; i < 8; i++) {
        box[20 + i] = (uint8_t)((uint64_t)p->start_ms >> (56 - 8 * i));
        box[28 + i] = (uint8_t)((uint64_t)p->duration_ms >> (56 - 8 * i));
        box[36 + i] = (uint8_t)((uint64_t)p->wall_ms >> (56 - 8 * i));
    }
}

static void on_sig(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* Picks the IPv4 address of the interface that carries the LAN. */
static int primary_ipv4(struct in_addr *out)
{
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0)
        return -1;
    int found = -1;
    for (struct ifaddrs *ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
            continue;
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

/*
 * Instance name from the address, in the same form the Android build uses so
 * the NAS shows one entry per box. Two boxes on the same /24 must not announce
 * the same name or they hide each other.
 */
static void instance_name(struct in_addr ip, char *out, size_t size)
{
    const uint8_t *b = (const uint8_t *)&ip.s_addr;
    snprintf(out, size, "pixstb-%02x%02x%02x%02x", b[0], b[1], b[2], b[3]);
}

/* 標準入力から len バイト読み切る。戻り値: 0=成功, -1=EOF/エラー */
static int read_full(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n < 0) {
            if (errno == EINTR) {
                if (g_stop)
                    return -1;
                continue;
            }
            return -1;
        }
        if (n == 0)
            return -1;
        got += (size_t)n;
    }
    return 0;
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd64(const uint8_t *p)
{
    return ((uint64_t)rd32(p) << 32) | (uint64_t)rd32(p + 4);
}

/*
 * ボックスをひとつ読む。ヘッダは caller が読み済みの前提で、
 * 本体だけを buf へ読む。size にはヘッダ込みの総長を返す。
 */
static int read_box_body(int fd, uint8_t *hdr, uint64_t *total_size,
                         uint8_t **body, size_t *body_len)
{
    uint64_t size = rd32(hdr);
    size_t hdr_len = 8;

    if (size == 1) {
        uint8_t ext[8];
        if (read_full(fd, ext, sizeof(ext)) != 0)
            return -1;
        size = rd64(ext);
        hdr_len = 16;
    } else if (size == 0) {
        /* ファイル末尾まで。ライブ用途では来ない想定。 */
        return -1;
    }
    if (size < hdr_len || size > (64u * 1024 * 1024))
        return -1;

    size_t n = (size_t)(size - hdr_len);
    uint8_t *p = malloc(n ? n : 1);
    if (!p)
        return -1;
    if (n && read_full(fd, p, n) != 0) {
        free(p);
        return -1;
    }
    *total_size = size;
    *body = p;
    *body_len = n;
    return 0;
}

/* 初期化セグメント（ftyp + moov）を組み立てるための可変バッファ */
typedef struct {
    uint8_t *data;
    size_t size;
    size_t cap;
} Buf;

static int buf_append(Buf *b, const void *src, size_t n)
{
    if (b->size + n > b->cap) {
        size_t ncap = b->cap ? b->cap : 65536;
        while (ncap < b->size + n)
            ncap *= 2;
        uint8_t *p = realloc(b->data, ncap);
        if (!p)
            return -1;
        b->data = p;
        b->cap = ncap;
    }
    memcpy(b->data + b->size, src, n);
    b->size += n;
    return 0;
}

int main(int argc, char **argv)
{
    int port = 40774;
    const char *allowed_ip = NULL;
    size_t spool_mb = 192;
    const char *channel = "";
    const char *display = "X96Max-P";
    const char *program_file = "/dev/shm/pix/program.txt";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--allowed-ip") == 0 && i + 1 < argc) {
            allowed_ip = argv[++i];
        } else if (strcmp(argv[i], "--spool-mb") == 0 && i + 1 < argc) {
            spool_mb = (size_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
            channel = argv[++i];
        } else if (strcmp(argv[i], "--display") == 0 && i + 1 < argc) {
            display = argv[++i];
        } else if (strcmp(argv[i], "--program-file") == 0 && i + 1 < argc) {
            program_file = argv[++i];
        } else {
            fprintf(stderr, "usage: %s [--port N] [--allowed-ip LIST] "
                            "[--spool-mb N] [--channel NAME] [--display NAME]\n",
                    argv[0]);
            return 2;
        }
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    RamSpool *spool = ram_spool_create(spool_mb * 1024 * 1024);
    if (!spool) {
        fprintf(stderr, "cannot create the spool\n");
        return 1;
    }

    StbHttpConfig cfg = {
        .port = port,
        .allowed_ip = allowed_ip,
        .spool = spool,
        .stop_flag = &g_stop,
        .idle_timeout_sec = 0,
    };
    StbHttpServer *srv = stb_http_start(&cfg);
    if (!srv) {
        fprintf(stderr, "cannot listen on port %d\n", port);
        ram_spool_destroy(spool);
        return 1;
    }
    fprintf(stderr, "stb_linux_pipe: HTTP on 0.0.0.0:%d, spool %zu MB, channel '%s'\n",
            port, spool_mb, channel);

    /*
     * Announce the _pixstb._tcp service the NAS manager listens for. The
     * network may still be coming up when init starts us, so retry for a
     * while before giving up; discovery is optional and the HTTP server keeps
     * working without it.
     */
    MdnsAnnouncer *mdns = NULL;
    char host[64];
    struct in_addr addr;
    for (int attempt = 0; attempt < 60 && !g_stop; attempt++) {
        if (primary_ipv4(&addr) == 0) {
            instance_name(addr, host, sizeof(host));
            MdnsAnnounceConfig acfg = {
                .port = port,
                .host = host,
                .display = display,
                .channel = atoi(channel),
                .addr = addr,
            };
            mdns = mdns_announce_start(&acfg);
            if (mdns) {
                char ip[32];
                inet_ntop(AF_INET, &addr, ip, sizeof(ip));
                fprintf(stderr, "stb_linux_pipe: announced %s as '%s' at %s:%d\n",
                        host, display, ip, port);
                break;
            }
        }
        sleep(2);
    }
    if (!mdns)
        fprintf(stderr, "stb_linux_pipe: no IPv4 address; mDNS stays off\n");

    Buf init = {0};
    Buf frag = {0};
    uint32_t seq = 0;
    int have_init = 0;

    uint8_t hdr[16];
    while (!g_stop) {
        if (read_full(STDIN_FILENO, hdr, 8) != 0)
            break;

        char type[5] = {0};
        memcpy(type, hdr + 4, 4);

        uint64_t total = 0;
        uint8_t *body = NULL;
        size_t body_len = 0;
        if (read_box_body(STDIN_FILENO, hdr, &total, &body, &body_len) != 0) {
            fprintf(stderr, "stb_linux_pipe: bad box '%s'\n", type);
            free(body);
            break;
        }

        /*
         * ftyp と moov は初期化セグメント、moof と mdat は断片。
         * 断片は moof で開始し、続く mdat までを 1 単位として積む。
         */
        if (strcmp(type, "ftyp") == 0 || strcmp(type, "moov") == 0) {
            if (frag.size > 0) {
                /* 途中で初期化セグメントが来たら、溜めていた断片は捨てる */
                frag.size = 0;
            }
            if (buf_append(&init, hdr, 8) != 0 ||
                (body_len && buf_append(&init, body, body_len) != 0)) {
                free(body);
                break;
            }
            if (strcmp(type, "moov") == 0 && init.size > 0 && !have_init) {
                if (ram_spool_set_init(spool, init.data, init.size) == 0) {
                    fprintf(stderr, "stb_linux_pipe: init segment %zu bytes\n",
                            init.size);
                    have_init = 1;
                }
            }
        } else if (strcmp(type, "moof") == 0) {
            if (have_init) {
                frag.size = 0;
                /*
                 * A new program has to be announced before the fragment that
                 * carries it. The recorder cuts on these boxes.
                 */
                ProgramInfo cur;
                memset(&cur, 0, sizeof(cur));
                read_program_file(program_file, &cur);
                /*
                 * EIT は present/following が揺れるので、短時間に同じ番組が
                 * 行ったり来たりすることがある。そのたびに区切ると数秒の
                 * セグメントが量産される。直近で区切った番組には戻らない。
                 */
                int recent = 0;
                if (g_have_program && cur.event_id == g_program.event_id)
                    recent = 1;
                for (int k = 0; k < g_last_event_count && !recent; k++)
                    if (g_last_events[k] == cur.event_id)
                        recent = 1;
                if (cur.valid && !recent &&
                    moof_has_video(body, body_len)) {
                    uint8_t box[44];
                    build_boundary_box(&cur, box);
                    if (buf_append(&frag, box, sizeof(box)) == 0) {
                        g_last_events[g_last_event_count % 4] = g_program.event_id;
                        if (g_have_program)
                            g_last_event_count++;
                        g_program = cur;
                        g_have_program = 1;
                        fprintf(stderr, "stb_linux_pipe: boundary service=%d event=%d\n",
                                cur.service_id, cur.event_id);
                    }
                }
                if (buf_append(&frag, hdr, 8) != 0 ||
                    (body_len && buf_append(&frag, body, body_len) != 0)) {
                    free(body);
                    break;
                }
            }
        } else if (strcmp(type, "mdat") == 0) {
            if (have_init && frag.size > 0) {
                if (buf_append(&frag, hdr, 8) != 0 ||
                    (body_len && buf_append(&frag, body, body_len) != 0)) {
                    free(body);
                    break;
                }
                if (ram_spool_push(spool, seq, frag.data, frag.size) == 0) {
                    seq++;
                    if ((seq % 20) == 0) {
                        size_t used = 0, maxb = 0, count = 0;
                        uint64_t pushed = 0, dropped = 0;
                        ram_spool_stats(spool, &used, &maxb, &pushed, &dropped,
                                        &count);
                        fprintf(stderr,
                                "stb_linux_pipe: %u fragments, spool %.1f/%.0f MB, "
                                "dropped=%llu\n",
                                seq, used / 1048576.0, maxb / 1048576.0,
                                (unsigned long long)dropped);
                    }
                }
                frag.size = 0;
            }
        }
        free(body);
    }

    fprintf(stderr, "stb_linux_pipe: input closed after %u fragments\n", seq);
    if (mdns)
        mdns_announce_stop(mdns);
    free(init.data);
    free(frag.data);
    stb_http_stop(srv);
    ram_spool_destroy(spool);
    return 0;
}
