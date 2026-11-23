/*
 * pix-stream-manager: 管理APIと画面。
 *
 * STBの登録・削除、録画の開始・停止、状態の表示をまとめて扱う。
 * 既存の pix-archive や miraview とは別のポートで動き、それらには触れない。
 *
 * 依存は cJSON のみ。HTTPは自前実装で、同時に1リクエストずつ処理する。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/select.h>

#include "cJSON.h"

#define MAX_NODES 16
#define MAX_BODY 8192

/* An STB seen on the network, whether or not it is registered. */
typedef struct {
    char instance[128];
    char display[96];
    char ip[32];
    int port;
    int channel;
    time_t last_seen;
} Discovered;

#define MAX_DISCOVERED 32
static Discovered g_found[MAX_DISCOVERED];
static int g_found_count;
static pthread_mutex_t g_found_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    char id[96];
    char name[128];
    char host[64];
    int port;
    char channel[32];  /* "17" (GR) or "BS15_0" (BS/CS) */
    char tuners[16];   /* "gr" or "gr,bs,cs" */
    int enabled;
    char codec[8];   /* "hevc" or "avc" */
    int width;
    int height;
    int bitrate;
    char rate_mode[8]; /* "vbr" or "cbr" */
    int gop_sec;       /* keyframe interval in seconds */
    pid_t pid;   /* recorder child, 0 when stopped */
    /* Last data progress seen on the STB, for the "stalled" state. */
    unsigned long long last_pushed;
    time_t last_pushed_at;
} Node;

static Node g_nodes[MAX_NODES];
static int g_node_count;
static char g_config_path[512];
static char g_recorder_path[512];
static char g_archive_url[256];
static char g_recordings_root[256];
static char g_tmpfs_dir[256];
static char g_state_dir[256];
static int g_bitrate;

/* 放送局名: /api/archive/services の GR/BS/CS チャンネル一覧をキャッシュする。 */
#define MAX_CHANNELS 128
typedef struct {
    char type[8];      /* "GR" / "BS" / "CS" */
    char channel[32];  /* "17" / "BS15_0" */
    char name[64];
} ChannelName;
static ChannelName g_channels[MAX_CHANNELS];
static int g_channel_count;
static time_t g_channels_at;


/* ---- 自前のmDNSリスナ ---- */

#define MDNS_PORT 5353
#define MDNS_GROUP "224.0.0.251"
#define SERVICE "_pixstb._tcp.local"

/* Reads a possibly compressed DNS name starting at *off. */
static int read_name(const uint8_t *buf, size_t len, size_t *off,
                     char *out, size_t out_size) {
    size_t pos = *off;
    size_t written = 0;
    int jumped = 0;
    size_t after = 0;

    for (int guard = 0; guard < 64; guard++) {
        if (pos >= len) return -1;
        uint8_t c = buf[pos];
        if (c == 0) {
            pos++;
            break;
        }
        if ((c & 0xc0) == 0xc0) {
            if (pos + 1 >= len) return -1;
            size_t target = ((size_t)(c & 0x3f) << 8) | buf[pos + 1];
            if (!jumped) after = pos + 2;
            jumped = 1;
            pos = target;
            continue;
        }
        size_t n = c;
        if (pos + 1 + n > len) return -1;
        if (written && written + 1 < out_size) out[written++] = '.';
        for (size_t i = 0; i < n && written + 1 < out_size; i++) {
            out[written++] = (char)buf[pos + 1 + i];
        }
        pos += 1 + n;
    }
    out[written] = '\0';
    *off = jumped ? after : pos;
    return 0;
}

static int starts_with_service(const char *name) {
    return strncmp(name, SERVICE, strlen(SERVICE)) == 0;
}

/* Records the contents of one announcement. */
static void store_discovery(const char *instance, const char *display,
                            const char *ip, int port, int channel) {
    if (!instance[0] || !ip[0]) return;
    pthread_mutex_lock(&g_found_lock);
    int idx = -1;
    for (int i = 0; i < g_found_count; i++) {
        if (strcmp(g_found[i].instance, instance) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        if (g_found_count >= MAX_DISCOVERED) {
            pthread_mutex_unlock(&g_found_lock);
            return;
        }
        idx = g_found_count++;
        memset(&g_found[idx], 0, sizeof(g_found[idx]));
        snprintf(g_found[idx].instance, sizeof(g_found[idx].instance), "%s", instance);
        fprintf(stderr, "manager: discovered %s at %s\n", instance, ip);
    }
    if (display && display[0]) {
        size_t n = strlen(display);
        if (n >= sizeof(g_found[idx].display)) n = sizeof(g_found[idx].display) - 1;
        memcpy(g_found[idx].display, display, n);
        g_found[idx].display[n] = '\0';
    }
    {
        size_t n = strlen(ip);
        if (n >= sizeof(g_found[idx].ip)) n = sizeof(g_found[idx].ip) - 1;
        memcpy(g_found[idx].ip, ip, n);
        g_found[idx].ip[n] = '\0';
    }
    if (port) g_found[idx].port = port;
    if (channel) g_found[idx].channel = channel;
    g_found[idx].last_seen = time(NULL);
    pthread_mutex_unlock(&g_found_lock);
}

/* Parses one mDNS packet and folds anything useful into the list. */
static void parse_mdns(const uint8_t *buf, size_t len) {
    if (len < 12) return;
    uint16_t qd = (uint16_t)((buf[4] << 8) | buf[5]);
    uint16_t an = (uint16_t)((buf[6] << 8) | buf[7]);

    size_t off = 12;
    char name[160];
    for (int i = 0; i < qd; i++) {
        if (read_name(buf, len, &off, name, sizeof(name)) != 0) return;
        if (off + 4 > len) return;
        off += 4;                       /* type + class */
    }

    char instance[160] = {0};
    char target[160] = {0};
    int have_our_ptr = 0;
    char ip[32] = {0};
    char display[96] = {0};
    int port = 0, channel = 0;

    for (int i = 0; i < an; i++) {
        if (read_name(buf, len, &off, name, sizeof(name)) != 0) return;
        if (off + 10 > len) return;
        uint16_t type = (uint16_t)((buf[off] << 8) | buf[off + 1]);
        off += 4;                       /* type + class */
        off += 4;                       /* ttl */
        uint16_t rdlen = (uint16_t)((buf[off] << 8) | buf[off + 1]);
        off += 2;
        if (off + rdlen > len) return;
        size_t rdstart = off;

        if (type == 12) {               /* PTR */
            size_t p = rdstart;
            char ptr[160];
            /*
             * The owner name is the service ("_pixstb._tcp.local") and the
             * record data is the instance, so the two must be checked for
             * different things.
             */
            if (starts_with_service(name)
                && read_name(buf, len, &p, ptr, sizeof(ptr)) == 0) {
                have_our_ptr = 1;
                const char *dot = strstr(ptr, "._pixstb");
                size_t n = dot ? (size_t)(dot - ptr) : strlen(ptr);
                if (n >= sizeof(instance)) n = sizeof(instance) - 1;
                memcpy(instance, ptr, n);
                instance[n] = '\0';
            }
        } else if (type == 33) {        /* SRV */
            if (rdlen >= 6) {
                port = (buf[rdstart + 4] << 8) | buf[rdstart + 5];
                size_t p = rdstart + 6;
                read_name(buf, len, &p, target, sizeof(target));
                if (!instance[0]) {
                    const char *dot = strstr(name, "._pixstb");
                    if (dot) {
                        size_t n = (size_t)(dot - name);
                        if (n >= sizeof(instance)) n = sizeof(instance) - 1;
                        memcpy(instance, name, n);
                        instance[n] = '\0';
                    }
                }
            }
        } else if (type == 16) {        /* TXT */
            size_t p = rdstart;
            while (p < rdstart + rdlen) {
                uint8_t l = buf[p++];
                if (l == 0 || p + l > rdstart + rdlen) break;
                char item[128];
                size_t cp = l < sizeof(item) - 1 ? l : sizeof(item) - 1;
                memcpy(item, buf + p, cp);
                item[cp] = '\0';
                p += l;
                if (strncmp(item, "name=", 5) == 0) {
                    size_t n = strlen(item + 5);
                    if (n >= sizeof(display)) n = sizeof(display) - 1;
                    memcpy(display, item + 5, n);
                    display[n] = '\0';
                } else if (strncmp(item, "ch=", 3) == 0) {
                    channel = atoi(item + 3);
                } else if (strncmp(item, "ip=", 3) == 0) {
                    size_t n = strlen(item + 3);
                    if (n >= sizeof(ip)) n = sizeof(ip) - 1;
                    memcpy(ip, item + 3, n);
                    ip[n] = '\0';
                }
            }
        } else if (type == 1 && rdlen == 4) {   /* A */
            if (strcmp(name, target) == 0 || !ip[0]) {
                snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                         buf[rdstart], buf[rdstart + 1],
                         buf[rdstart + 2], buf[rdstart + 3]);
            }
        }
        off = rdstart + rdlen;
    }

    /* Only records belonging to our own service are interesting. */
    if (have_our_ptr && instance[0] && ip[0]) {
        store_discovery(instance, display, ip, port, channel);
    }
}

static void *mdns_listen_thread(void *arg) {
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return NULL;
    int one = 1;
    /*
     * SO_REUSEADDR only. With SO_REUSEPORT the kernel spreads arriving
     * datagrams across every socket in the group, so this listener would see
     * only a fraction of the announcements.
     */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MDNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "manager: cannot bind mDNS port: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }

    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(MDNS_GROUP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    fprintf(stderr, "manager: listening for %s announcements\n", SERVICE);

    for (;;) {
        uint8_t buf[2048];
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &flen);
        if (n <= 0) continue;
        parse_mdns(buf, (size_t)n);
    }
    return NULL;
}

static void start_discovery(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, mdns_listen_thread, NULL) != 0) {
        fprintf(stderr, "manager: cannot start discovery: %s\n", strerror(errno));
        return;
    }
    pthread_detach(t);
}

/* チャンネルは "17" (GR) か "BS15_0"/"CS..." (衛星)。数値も受け付ける。 */
static void channel_from_json(cJSON *v, char *out, size_t out_size, const char *fallback) {
    if (cJSON_IsString(v) && v->valuestring[0]) {
        snprintf(out, out_size, "%s", v->valuestring);
    } else if (cJSON_IsNumber(v)) {
        snprintf(out, out_size, "%d", (int)v->valuedouble);
    } else {
        snprintf(out, out_size, "%s", fallback);
    }
}

static int valid_channel(const char *ch) {
    if (!ch[0] || strlen(ch) >= 31) return 0;
    if (strspn(ch, "0123456789") == strlen(ch)) return 1;
    if ((strncmp(ch, "BS", 2) == 0 || strncmp(ch, "CS", 2) == 0) && ch[2]) {
        return strspn(ch + 2, "0123456789_") == strlen(ch + 2);
    }
    return 0;
}

static int valid_tuners(const char *t) {
    return !strcmp(t, "gr") || !strcmp(t, "gr,bs") || !strcmp(t, "gr,bs,cs");
}

static void load_config(void) {
    FILE *f = fopen(g_config_path, "rb");
    if (!f) {
        fprintf(stderr, "manager: no registry at %s, starting empty\n", g_config_path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 1024 * 1024) { fclose(f); return; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;
    cJSON *nodes = cJSON_GetObjectItem(root, "nodes");
    cJSON *item = NULL;
    g_node_count = 0;
    cJSON_ArrayForEach(item, nodes) {
        if (g_node_count >= MAX_NODES) break;
        Node *nd = &g_nodes[g_node_count];
        memset(nd, 0, sizeof(*nd));
        cJSON *v;
        v = cJSON_GetObjectItem(item, "id");
        if (cJSON_IsString(v)) snprintf(nd->id, sizeof(nd->id), "%s", v->valuestring);
        v = cJSON_GetObjectItem(item, "name");
        if (cJSON_IsString(v)) snprintf(nd->name, sizeof(nd->name), "%s", v->valuestring);
        v = cJSON_GetObjectItem(item, "host");
        if (cJSON_IsString(v)) snprintf(nd->host, sizeof(nd->host), "%s", v->valuestring);
        v = cJSON_GetObjectItem(item, "port");
        nd->port = cJSON_IsNumber(v) ? (int)v->valuedouble : 40774;
        v = cJSON_GetObjectItem(item, "channel");
        channel_from_json(v, nd->channel, sizeof(nd->channel), "20");
        v = cJSON_GetObjectItem(item, "tuners");
        snprintf(nd->tuners, sizeof(nd->tuners), "%s",
                 (cJSON_IsString(v) && valid_tuners(v->valuestring)) ? v->valuestring : "gr");
        v = cJSON_GetObjectItem(item, "enabled");
        nd->enabled = cJSON_IsTrue(v) ? 1 : 0;
        /* Encoder settings: older registries predate them, so default. */
        snprintf(nd->codec, sizeof(nd->codec), "hevc");
        v = cJSON_GetObjectItem(item, "codec");
        if (cJSON_IsString(v) && v->valuestring[0])
            snprintf(nd->codec, sizeof(nd->codec), "%s", v->valuestring);
        nd->width = 1920;
        nd->height = 1080;
        nd->bitrate = 5000000;
        snprintf(nd->rate_mode, sizeof(nd->rate_mode), "vbr");
        nd->gop_sec = 5;
        v = cJSON_GetObjectItem(item, "rateMode");
        if (cJSON_IsString(v) && v->valuestring[0])
            snprintf(nd->rate_mode, sizeof(nd->rate_mode), "%s", v->valuestring);
        v = cJSON_GetObjectItem(item, "gopSec");
        if (cJSON_IsNumber(v)) nd->gop_sec = (int)v->valuedouble;
        v = cJSON_GetObjectItem(item, "width");
        if (cJSON_IsNumber(v)) nd->width = (int)v->valuedouble;
        v = cJSON_GetObjectItem(item, "height");
        if (cJSON_IsNumber(v)) nd->height = (int)v->valuedouble;
        v = cJSON_GetObjectItem(item, "bitrate");
        if (cJSON_IsNumber(v)) nd->bitrate = (int)v->valuedouble;
        if (nd->id[0] && nd->host[0]) g_node_count++;
    }
    cJSON_Delete(root);
    fprintf(stderr, "manager: loaded %d node(s)\n", g_node_count);
}

static void save_config(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON *nodes = cJSON_AddArrayToObject(root, "nodes");
    for (int i = 0; i < g_node_count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", g_nodes[i].id);
        cJSON_AddStringToObject(o, "name", g_nodes[i].name);
        cJSON_AddStringToObject(o, "host", g_nodes[i].host);
        cJSON_AddNumberToObject(o, "port", g_nodes[i].port);
        cJSON_AddStringToObject(o, "channel", g_nodes[i].channel);
        cJSON_AddStringToObject(o, "tuners", g_nodes[i].tuners);
        cJSON_AddBoolToObject(o, "enabled", g_nodes[i].enabled);
        cJSON_AddStringToObject(o, "codec", g_nodes[i].codec);
        cJSON_AddNumberToObject(o, "width", g_nodes[i].width);
        cJSON_AddNumberToObject(o, "height", g_nodes[i].height);
        cJSON_AddNumberToObject(o, "bitrate", g_nodes[i].bitrate);
        cJSON_AddStringToObject(o, "rateMode", g_nodes[i].rate_mode);
        cJSON_AddNumberToObject(o, "gopSec", g_nodes[i].gop_sec);
        cJSON_AddItemToArray(nodes, o);
    }
    char *text = cJSON_Print(root);
    if (text) {
        char tmp[600];
        snprintf(tmp, sizeof(tmp), "%s.tmp", g_config_path);
        FILE *f = fopen(tmp, "w");
        if (f) {
            fputs(text, f);
            fclose(f);
            rename(tmp, g_config_path);
        }
        free(text);
    }
    cJSON_Delete(root);
}

static Node *find_node(const char *id) {
    for (int i = 0; i < g_node_count; i++) {
        if (strcmp(g_nodes[i].id, id) == 0) return &g_nodes[i];
    }
    return NULL;
}

/* A node is one recording on one STB endpoint. The channel is a setting of
 * that recording, not part of the box identity: the same host:port with a
 * different channel is still the same tuner. */
static Node *find_node_by_endpoint(const char *host, int port) {
    for (int i = 0; i < g_node_count; i++) {
        if (g_nodes[i].port == port && strcmp(g_nodes[i].host, host) == 0) {
            return &g_nodes[i];
        }
    }
    return NULL;
}

static int node_running(const Node *nd) {
    if (nd->pid <= 0) return 0;
    int status = 0;
    pid_t r = waitpid(nd->pid, &status, WNOHANG);
    if (r == nd->pid || (r < 0 && errno == ECHILD)) return 0;
    return 1;
}

static void start_node(Node *nd) {
    if (node_running(nd)) return;
    /*
     * One recorder per STB endpoint. Two recorders on the same tuner ask for
     * different channels, and the STB switches under the first reader on every
     * request, so neither one records. Registration rejects a second node for
     * the same host:port; this is the safety net for a hand-edited registry.
     */
    for (int i = 0; i < g_node_count; i++) {
        Node *other = &g_nodes[i];
        if (other == nd) continue;
        if (other->port == nd->port && strcmp(other->host, nd->host) == 0
            && node_running(other)) {
            fprintf(stderr, "manager: node %s not started, %s already records from %s:%d\n",
                    nd->id, other->id, nd->host, nd->port);
            return;
        }
    }
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "manager: fork failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        char port[16], bitrate[16], width[16], height[16], gop[16];
        snprintf(port, sizeof(port), "%d", nd->port);
        snprintf(bitrate, sizeof(bitrate), "%d", nd->bitrate > 0 ? nd->bitrate : g_bitrate);
        snprintf(width, sizeof(width), "%d", nd->width);
        snprintf(height, sizeof(height), "%d", nd->height);
        snprintf(gop, sizeof(gop), "%d", nd->gop_sec > 0 ? nd->gop_sec : 5);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        execl(g_recorder_path, g_recorder_path,
              "--stb-ip", nd->host,
              "--port", port,
              "--stream-id", nd->channel,
              "--archive-url", g_archive_url,
              "--recordings-root", g_recordings_root,
              "--tmpfs-dir", g_tmpfs_dir,
              /*
               * Bound the segment length even when the broadcaster's event
               * information stalls: a multi-hour file delays every downstream
               * step and can crowd the tmpfs. Programmes are usually under an
               * hour, so this only splits unusual slots.
               */
              "--max-segment-sec", "7200",
              "--state-dir", g_state_dir,
              "--channel", nd->channel,
              "--codec", nd->codec,
              "--width", width,
              "--height", height,
              "--bitrate", bitrate,
              "--rate-mode", nd->rate_mode,
              "--gop", gop,
              (char *)NULL);
        _exit(127);
    }
    nd->pid = pid;
    fprintf(stderr, "manager: started node %s (pid %d)\n", nd->id, (int)pid);
}

static void stop_node(Node *nd) {
    if (!node_running(nd)) {
        nd->pid = 0;
        return;
    }
    kill(nd->pid, SIGTERM);
    for (int i = 0; i < 20; i++) {
        if (!node_running(nd)) break;
        usleep(100 * 1000);
    }
    if (node_running(nd)) kill(nd->pid, SIGKILL);
    nd->pid = 0;
    fprintf(stderr, "manager: stopped node %s\n", nd->id);
}

/* 1秒で諦める短いHTTP取得。管理画面の表示を遅らせないため。 */
static int http_get(const char *host, int port, const char *path,
                    char *out, size_t out_size) {
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    char req[256];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                      path, host);
    if (send(fd, req, (size_t)rl, MSG_NOSIGNAL) != rl) {
        close(fd);
        return -1;
    }
    size_t total = 0;
    while (total + 1 < out_size) {
        ssize_t n = recv(fd, out + total, out_size - 1 - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
    }
    out[total] = '\0';
    close(fd);

    char *body = strstr(out, "\r\n\r\n");
    if (body) {
        body += 4;
        memmove(out, body, strlen(body) + 1);
    }
    return 0;
}

/* ---- 放送局名のキャッシュ ---- */

/* "http://host:port" を host と port に分ける。 */
static void split_host_port(const char *url, char *host, size_t host_size, int *port) {
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - p) : strlen(p);
    const char *colon = memchr(p, ':', n);
    size_t hn = colon ? (size_t)(colon - p) : n;
    if (hn >= host_size) hn = host_size - 1;
    memcpy(host, p, hn);
    host[hn] = '\0';
    *port = colon ? atoi(colon + 1) : 80;
}

/*
 * アーカイブのサービス一覧から GR の ch -> 局名 を取り込む。BS は ch が
 * "BS15_0" のような文字列で、ノードの ch (整数) とは対応しないため扱わない。
 */
static void refresh_channels(void) {
    char host[128];
    int port = 80;
    split_host_port(g_archive_url, host, sizeof(host), &port);

    size_t cap = 65536;
    char *body = malloc(cap);
    if (!body) return;
    g_channels_at = time(NULL);
    if (http_get(host, port, "/api/archive/services", body, cap) != 0) {
        free(body);
        return;
    }
    cJSON *arr = cJSON_Parse(body);
    free(body);
    if (!arr) return;

    int n = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (n >= MAX_CHANNELS) break;
        cJSON *ch = cJSON_GetObjectItem(item, "channel");
        if (!cJSON_IsObject(ch)) continue;
        cJSON *type = cJSON_GetObjectItem(ch, "type");
        cJSON *num = cJSON_GetObjectItem(ch, "channel");
        cJSON *name = cJSON_GetObjectItem(item, "name");
        if (!cJSON_IsString(type) || !cJSON_IsString(num) || !cJSON_IsString(name)) continue;
        /* 地上波・BS・CS のみ。BS4K は DT-295 では受信できない。 */
        if (strcmp(type->valuestring, "GR") && strcmp(type->valuestring, "BS")
            && strcmp(type->valuestring, "CS")) continue;
        if (!num->valuestring[0] || strlen(num->valuestring) >= sizeof(g_channels[0].channel)) continue;
        int seen = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(g_channels[i].channel, num->valuestring) == 0) { seen = 1; break; }
        }
        if (seen) continue;
        snprintf(g_channels[n].type, sizeof(g_channels[n].type), "%s", type->valuestring);
        snprintf(g_channels[n].channel, sizeof(g_channels[n].channel), "%s", num->valuestring);
        snprintf(g_channels[n].name, sizeof(g_channels[n].name), "%s", name->valuestring);
        n++;
    }
    cJSON_Delete(arr);
    g_channel_count = n;
}

/* 取り込みは10分に1回まで。失敗したときは30秒あけて再試行する。 */
static void ensure_channels(void) {
    time_t now = time(NULL);
    if (g_channel_count > 0 && now - g_channels_at < 600) return;
    if (g_channels_at && now - g_channels_at < 30) return;
    refresh_channels();
}

static const char *channel_name(const char *channel) {
    for (int i = 0; i < g_channel_count; i++) {
        if (strcmp(g_channels[i].channel, channel) == 0) return g_channels[i].name;
    }
    return NULL;
}

static int write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void send_response(int fd, int code, const char *type,
                          const char *body, size_t len) {
    char head[512];
    const char *reason = code == 200 ? "OK" : (code == 201 ? "Created"
                             : (code == 404 ? "Not Found"
                             : (code == 409 ? "Conflict" : "Bad Request")));
    int hl = snprintf(head, sizeof(head),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n"
                      "Cache-Control: no-store\r\n"
                      "\r\n",
                      code, reason, type, len);
    write_all(fd, head, (size_t)hl);
    if (len) write_all(fd, body, len);
}

static cJSON *node_json(Node *nd) {
    cJSON *o = cJSON_CreateObject();
    int recording = node_running(nd) != 0;
    time_t now = time(NULL);
    cJSON_AddStringToObject(o, "id", nd->id);
    cJSON_AddStringToObject(o, "name", nd->name);
    cJSON_AddStringToObject(o, "host", nd->host);
    cJSON_AddNumberToObject(o, "port", nd->port);
    cJSON_AddStringToObject(o, "channel", nd->channel);
    cJSON_AddStringToObject(o, "tuners", nd->tuners);
    cJSON_AddBoolToObject(o, "enabled", nd->enabled != 0);
    cJSON_AddBoolToObject(o, "recording", recording != 0);
    cJSON_AddStringToObject(o, "codec", nd->codec);
    cJSON_AddNumberToObject(o, "width", nd->width);
    cJSON_AddNumberToObject(o, "height", nd->height);
    cJSON_AddNumberToObject(o, "bitrate", nd->bitrate);
    cJSON_AddStringToObject(o, "rateMode", nd->rate_mode);
    cJSON_AddNumberToObject(o, "gopSec", nd->gop_sec);
    {
        const char *cname = channel_name(nd->channel);
        if (cname) cJSON_AddStringToObject(o, "channelName", cname);
    }

    char raw[1024];
    char path[128];
    const char *state = recording ? "waiting" : "stopped";
    snprintf(path, sizeof(path), "/v1/status");
    if (http_get(nd->host, nd->port, path, raw, sizeof(raw)) == 0) {
        cJSON *st = cJSON_Parse(raw);
        if (st) {
            cJSON *ch = cJSON_GetObjectItem(st, "channel");
            cJSON *cap = cJSON_GetObjectItem(st, "capturing");
            cJSON *pushed = cJSON_GetObjectItem(st, "totalPushed");
            char live[32] = "";
            if (cJSON_IsString(ch)) snprintf(live, sizeof(live), "%s", ch->valuestring);
            else if (cJSON_IsNumber(ch)) snprintf(live, sizeof(live), "%d", (int)ch->valuedouble);
            int capturing = cJSON_IsTrue(cap) ? 1 : 0;
            cJSON_AddItemToObject(o, "stb", st);
            cJSON_AddStringToObject(o, "liveChannel", live);
            cJSON_AddBoolToObject(o, "capturing", capturing != 0);
            if (cJSON_IsNumber(pushed)) {
                unsigned long long p = (unsigned long long)pushed->valuedouble;
                if (p != nd->last_pushed || nd->last_pushed_at == 0) {
                    nd->last_pushed = p;
                    nd->last_pushed_at = now;
                }
                cJSON_AddNumberToObject(o, "pushed", (double)p);
            }
            /*
             * Report what is actually happening, not what was configured: a
             * recorder can be alive while the STB still serves another
             * channel, and the STB can claim to capture while no fragment
             * reaches the spool. Both used to look healthy on this page.
             */
            if (!recording) state = "stopped";
            else if (!capturing) state = "waiting";
            else if (strcmp(live, nd->channel) != 0) state = "mismatch";
            else if (nd->last_pushed_at && now - nd->last_pushed_at > 30) state = "stalled";
            else state = "recording";
        } else {
            cJSON_AddStringToObject(o, "stbError", "unparsable status");
            state = "error";
        }
    } else {
        cJSON_AddStringToObject(o, "stbError", "unreachable");
        state = "error";
    }
    cJSON_AddStringToObject(o, "state", state);
    return o;
}

static const char *PAGE =
"<!doctype html><meta charset=\"utf-8\"><title>録画管理</title>\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<style>\n"
"*{box-sizing:border-box}\n"
"body{font-family:system-ui,sans-serif;margin:0;background:#f6f7f9;color:#1b1d21}\n"
"main{max-width:1080px;margin:0 auto;padding:24px}\n"
"h1{font-size:21px;margin:0 0 4px}\n"
"h2{font-size:15px;margin:26px 0 10px;color:#4a4f57}\n"
".muted{color:#8a8f98;font-size:13px}\n"
"table{border-collapse:collapse;width:100%;background:#fff}\n"
".table-wrap{overflow-x:auto;background:#fff;border-radius:8px;box-shadow:0 1px 2px #0002}\n"
"th,td{padding:10px 12px;border-bottom:1px solid #e6e8ec;text-align:left;font-size:14px;vertical-align:middle}\n"
"th{background:#eef0f3;font-weight:600;color:#4a4f57;white-space:nowrap}\n"
"tr:last-child td{border-bottom:0}\n"
"tr.sel td{background:#eef2ff}\n"
"td.ops{white-space:nowrap}\n"
"td.ch{white-space:nowrap}\n"
".ch-name{margin-left:6px;color:#4a4f57;font-size:13px}\n"
"button{padding:5px 11px;border:1px solid #c8ccd4;background:#fff;border-radius:5px;cursor:pointer;font-size:13px;font-family:inherit}\n"
"button+button{margin-left:6px}\n"
"button:hover{background:#eef2ff}\n"
"button.primary{background:#2f5bea;border-color:#2f5bea;color:#fff}\n"
"button.primary:hover{background:#2449c4}\n"
"button.danger{color:#b3261e}\n"
"button:disabled{opacity:.5;cursor:default}\n"
".on{color:#146c2e;font-weight:600}.off{color:#8a8f98}.warn{color:#a15c00;font-weight:600}\n"
".card{background:#fff;padding:18px;border-radius:8px;box-shadow:0 1px 2px #0002;margin-top:14px}\n"
".grid{display:grid;grid-template-columns:repeat(3,1fr);gap:14px}\n"
"@media(max-width:720px){.grid{grid-template-columns:1fr 1fr}}\n"
"label{display:flex;flex-direction:column;gap:5px;font-size:13px;color:#4a4f57}\n"
"input,select{padding:7px 9px;border:1px solid #c8ccd4;border-radius:6px;font-size:14px;background:#fff;font-family:inherit}\n"
"input:focus,select:focus{outline:2px solid #b9c8ff;border-color:#2f5bea}\n"
".hint{font-size:12px;color:#8a8f98;margin:12px 0 0}\n"
".actions{margin-top:16px;display:flex;gap:8px}\n"
".hidden{display:none}\n"
"details.card summary{cursor:pointer;font-weight:600;font-size:14px}\n"
"details.card form{margin-top:16px}\n"
"</style>\n"
"<main>\n"
"<h1>STB録画管理</h1>\n"
"<div id=\"summary\" class=\"muted\">読み込み中…</div>\n"
"<div class=\"table-wrap\"><table>\n"
"<thead><tr><th>名前</th><th>STB</th><th>ch</th><th>形式</th><th>状態</th><th>操作</th></tr></thead>\n"
"<tbody id=\"rows\"><tr><td colspan=6>読み込み中…</td></tr></tbody>\n"
"</table></div>\n"
"\n"
"<section id=\"editor\" class=\"card hidden\">\n"
"<h2 id=\"editor-title\" style=\"margin-top:0\">録画設定</h2>\n"
"<div id=\"editor-sub\" class=\"muted\"></div>\n"
"<form id=\"settings\">\n"
"<input type=\"hidden\" name=\"id\">\n"
"<div class=\"grid\">\n"
"<label>チャンネル<select name=\"channel\" id=\"ch-select\" required></select></label>\n"
"<label>受信<select name=\"tuners\"><option value=\"gr\">地上波のみ</option><option value=\"gr,bs,cs\">地上波＋BS/CS</option></select></label>\n"
"<label>コーデック<select name=\"codec\"><option value=\"hevc\">HEVC (H.265)</option><option value=\"avc\">H.264</option></select></label>\n"
"<label>解像度<select name=\"res\"><option value=\"1920x1080\">1920x1080</option><option value=\"1440x1080\">1440x1080（放送ネイティブ）</option><option value=\"1280x720\">1280x720</option></select></label>\n"
"<label>ビットレート（bps）<input name=\"bitrate\" type=\"number\" step=\"100000\" min=\"200000\" max=\"20000000\"></label>\n"
"<label>レート制御<select name=\"mode\"><option value=\"vbr\">VBR（画質優先）</option><option value=\"cbr\">CBR（固定）</option></select></label>\n"
"<label>GOP（秒）<input name=\"gop\" type=\"number\" min=\"1\" max=\"30\"></label>\n"
"</div>\n"
"<p class=\"hint\">チャンネルを変えると、録画はそのチャンネルで再開します。形式の変更は数秒で反映されます。</p>\n"
"<div class=\"actions\"><button class=\"primary\" id=\"apply\">適用</button><button type=\"button\" id=\"editor-close\">閉じる</button></div>\n"
"</form>\n"
"</section>\n"
"\n"
"<h2>ネットワーク上のSTB（自動検出）</h2>\n"
"<div class=\"table-wrap\"><table>\n"
"<thead><tr><th>名前</th><th>アドレス</th><th>申告ch</th><th>状態</th><th>操作</th></tr></thead>\n"
"<tbody id=\"found\"><tr><td colspan=5>検索中…</td></tr></tbody>\n"
"</table></div>\n"
"\n"
"<details class=\"card\">\n"
"<summary>STBを追加</summary>\n"
"<form id=\"add\">\n"
"<div class=\"grid\">\n"
"<label>名前<input name=\"name\" placeholder=\"X96Max\" required></label>\n"
"<label>アドレス<input name=\"host\" placeholder=\"192.168.99.57\" required></label>\n"
"<label>ポート<input name=\"port\" type=\"number\" value=\"40774\"></label>\n"
"<label>チャンネル<select name=\"channel\" id=\"add-channel\"></select></label>\n"
"<label>受信<select name=\"tuners\"><option value=\"gr\">地上波のみ</option><option value=\"gr,bs,cs\">地上波＋BS/CS</option></select></label>\n"
"</div>\n"
"<div class=\"actions\"><button class=\"primary\">追加</button></div>\n"
"</form>\n"
"</details>\n"
"</main>\n"
"<script>\n"
"let allNodes=[],editingId=null,channels=[],chNames={};\n"
"const esc=s=>String(s).replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]));\n"
"const chNameOf=ch=>chNames[ch]||'';\n"
"const TYPE_LABEL={GR:'地上波',BS:'BS',CS:'CS'};\n"
"const TYPE_ORDER={GR:0,BS:1,CS:2};\n"
"function channelOptions(selected,tuners){\n"
" const types=String(tuners||'gr').split(',');\n"
" let html='';\n"
" types.forEach(t=>{\n"
"  const list=channels.filter(c=>String(c.type).toLowerCase()===t);\n"
"  if(!list.length)return;\n"
"  html+='<optgroup label=\"'+(TYPE_LABEL[String(t).toUpperCase()]||t)+'\">';\n"
"  list.forEach(c=>{html+='<option value=\"'+esc(c.channel)+'\">'+esc(c.channel)+' '+esc(c.name)+'</option>';});\n"
"  html+='</optgroup>';\n"
" });\n"
" if(selected&&!channels.some(c=>c.channel===selected))html+='<option value=\"'+esc(selected)+'\">'+esc(selected)+'</option>';\n"
" return html;\n"
"}\n"
"async function loadChannels(){\n"
" const r=await fetch('/api/channels');channels=await r.json();\n"
" channels.sort((a,b)=>((TYPE_ORDER[a.type]??9)-(TYPE_ORDER[b.type]??9))||String(a.channel).localeCompare(String(b.channel),'ja',{numeric:true}));\n"
" chNames={};channels.forEach(c=>chNames[c.channel]=c.name);\n"
" const add=document.getElementById('add-channel');\n"
" if(add&&!add.options.length){add.innerHTML=channelOptions('17','gr,bs,cs');add.value=channels.some(c=>c.channel==='17')?'17':(channels[0]?channels[0].channel:'17');}\n"
"}\n"
"const fmtOf=n=>(n.codec||'hevc').toUpperCase()+' '+n.width+'x'+n.height+' '+((n.bitrate||0)/1000000).toFixed(1)+'Mbps '+(n.rateMode||'vbr').toUpperCase()+' gop'+n.gopSec+'s';\n"
"function stateTxt(n){\n"
" if(n.stbError)return '<span class=off>応答なし</span>';\n"
" if(n.state==='recording')return '<span class=on>録画中 ch'+esc(n.liveChannel)+'</span>';\n"
" if(n.state==='mismatch')return '<span class=warn>ch不一致（実ch'+esc(n.liveChannel)+'）</span>';\n"
" if(n.state==='stalled')return '<span class=warn>データ停止（ch'+esc(n.liveChannel)+'）</span>';\n"
" if(n.state==='waiting')return '<span class=off>STB待機</span>';\n"
" return '<span class=off>停止</span>';\n"
"}\n"
"function renderRows(){\n"
" const tb=document.getElementById('rows');\n"
" if(!allNodes.length){tb.innerHTML='<tr><td colspan=6>登録されたSTBはありません</td></tr>';return;}\n"
" tb.innerHTML=allNodes.map(n=>{\n"
"  const toggle=n.enabled?'<button data-act=\"toggle\">停止</button>':'<button data-act=\"toggle\" class=\"primary\">開始</button>';\n"
"  const cname=chNameOf(n.channel);\n"
"  return '<tr data-id=\"'+esc(n.id)+'\" data-enabled=\"'+(n.enabled?1:0)+'\"'+(n.id===editingId?' class=\"sel\"':'')+'>'\n"
"   +'<td>'+esc(n.name)+'</td><td>'+esc(n.host)+':'+n.port+'</td><td class=\"ch\">'+esc(n.channel)+(cname?'<span class=\"ch-name\">'+esc(cname)+'</span>':'')+'</td><td>'+fmtOf(n)+'</td><td>'+stateTxt(n)+'</td>'\n"
"   +'<td class=\"ops\"><button data-act=\"edit\">設定</button>'+toggle+'<button data-act=\"del\" class=\"danger\">削除</button></td></tr>';\n"
" }).join('');\n"
"}\n"
"async function load(){\n"
" const r=await fetch('/api/nodes');allNodes=await r.json();\n"
" if(editingId&&!allNodes.some(n=>n.id===editingId))closeEditor();\n"
" const rec=allNodes.filter(n=>n.state==='recording').length;\n"
" document.getElementById('summary').textContent=allNodes.length?rec+' / '+allNodes.length+' 台を録画中':'登録されたSTBはありません';\n"
" renderRows();\n"
"}\n"
"function openEditor(id){\n"
" const n=allNodes.find(x=>x.id===id);if(!n)return;\n"
" editingId=id;\n"
" const f=document.getElementById('settings');\n"
" f.id.value=id;\n"
" f.tuners.value=n.tuners||'gr';\n"
" const sel=document.getElementById('ch-select');\n"
" sel.innerHTML=channelOptions(n.channel,f.tuners.value);\n"
" sel.value=n.channel;\n"
" f.codec.value=n.codec||'hevc';\n"
" f.res.value=(n.width||1920)+'x'+(n.height||1080);\n"
" if(!f.res.value)f.res.value='1920x1080';\n"
" f.bitrate.value=n.bitrate||5000000;f.mode.value=n.rateMode||'vbr';f.gop.value=n.gopSec||5;\n"
" const cname=chNameOf(n.channel);\n"
" document.getElementById('editor-title').textContent='録画設定 — '+n.name;\n"
" document.getElementById('editor-sub').textContent=n.host+':'+n.port+' / 現在 '+n.channel+(cname?' '+cname:'')+' / '+fmtOf(n);\n"
" document.getElementById('editor').classList.remove('hidden');\n"
" renderRows();\n"
" sel.focus();\n"
" document.getElementById('editor').scrollIntoView({block:'nearest'});\n"
"}\n"
"function closeEditor(){\n"
" editingId=null;\n"
" document.getElementById('editor').classList.add('hidden');\n"
" renderRows();\n"
"}\n"
"document.getElementById('editor-close').onclick=closeEditor;\n"
"document.querySelector('#settings [name=tuners]').addEventListener('change',e=>{\n"
" const sel=document.getElementById('ch-select');\n"
" const cur=sel.value;\n"
" sel.innerHTML=channelOptions(cur,e.target.value);\n"
" if([].some.call(sel.options,o=>o.value===cur))sel.value=cur;\n"
"});\n"
"document.getElementById('rows').addEventListener('click',e=>{\n"
" const b=e.target.closest('button');if(!b)return;\n"
" const tr=b.closest('tr');if(!tr)return;\n"
" const id=tr.dataset.id,act=b.dataset.act;\n"
" if(act==='edit')openEditor(id);\n"
" else if(act==='toggle')toggleNode(id,tr.dataset.enabled==='1');\n"
" else if(act==='del')delNode(id);\n"
"});\n"
"async function toggleNode(id,enabled){\n"
" const r=await fetch('/api/nodes/'+id+'/'+(enabled?'stop':'start'),{method:'POST'});\n"
" if(!r.ok){alert(await r.text());return;}\n"
" load();\n"
"}\n"
"async function delNode(id){\n"
" const n=allNodes.find(x=>x.id===id);\n"
" if(!confirm((n?n.name+'（'+n.host+'）':'')+'を削除しますか？'))return;\n"
" const r=await fetch('/api/nodes/'+id,{method:'DELETE'});\n"
" if(!r.ok){alert(await r.text());return;}\n"
" if(editingId===id)closeEditor();\n"
" load();\n"
"}\n"
"document.getElementById('settings').onsubmit=async e=>{\n"
" e.preventDefault();\n"
" const b=document.getElementById('apply');if(b.disabled)return;\n"
" const f=new FormData(e.target),wh=f.get('res').split('x');\n"
" b.disabled=true;\n"
" try{\n"
"  const r=await fetch('/api/nodes/'+f.get('id')+'/settings',{method:'POST',headers:{'Content-Type':'application/json'},\n"
"   body:JSON.stringify({channel:f.get('channel'),tuners:f.get('tuners'),codec:f.get('codec'),width:+wh[0],height:+wh[1],\n"
"    bitrate:+f.get('bitrate'),rateMode:f.get('mode'),gopSec:+f.get('gop')})});\n"
"  if(!r.ok){alert(await r.text());return;}\n"
"  closeEditor();\n"
"  load();\n"
" }finally{b.disabled=false;}\n"
"};\n"
"document.getElementById('add').onsubmit=async e=>{\n"
" e.preventDefault();const f=new FormData(e.target);\n"
" const r=await fetch('/api/nodes',{method:'POST',headers:{'Content-Type':'application/json'},\n"
"  body:JSON.stringify({name:f.get('name'),host:f.get('host'),port:+f.get('port'),channel:f.get('channel'),tuners:f.get('tuners'),enabled:true})});\n"
" if(!r.ok){alert(await r.text());return;}\n"
" e.target.reset();load();\n"
"};\n"
"async function loadFound(){\n"
" const r=await fetch('/api/discovered');const ds=await r.json();\n"
" const tb=document.getElementById('found');\n"
" tb.innerHTML=ds.length?ds.map(d=>{\n"
"  const nm=d.name||d.instance;\n"
"  const st=d.online?'<span class=on>オンライン</span>':'<span class=off>応答なし</span>';\n"
"  const act=d.known?('<span class=off>登録済み '+esc(d.knownChannel)+'</span>')\n"
"   :'<button data-host=\"'+esc(d.host)+'\" data-port=\"'+d.port+'\" data-ch=\"'+esc(d.channel)+'\" data-name=\"'+esc(nm)+'\">追加</button>';\n"
"  const cname=chNameOf(d.channel);\n"
"  return '<tr><td>'+esc(nm)+'</td><td>'+esc(d.host)+':'+d.port+'</td><td class=\"ch\">'+esc(d.channel)+(cname?'<span class=\"ch-name\">'+esc(cname)+'</span>':'')+'</td><td>'+st+'</td><td>'+act+'</td></tr>';\n"
" }).join(''):'<tr><td colspan=5>見つかりません。STBの電源とネットワークを確認してください</td></tr>';\n"
"}\n"
"document.getElementById('found').addEventListener('click',async e=>{\n"
" const b=e.target.closest('button');if(!b)return;\n"
" const r=await fetch('/api/nodes',{method:'POST',headers:{'Content-Type':'application/json'},\n"
"  body:JSON.stringify({name:b.dataset.name,host:b.dataset.host,port:+b.dataset.port,channel:b.dataset.ch,tuners:'gr',enabled:true})});\n"
" if(!r.ok){alert(await r.text());return;}\n"
" load();loadFound();\n"
"});\n"
"load();loadFound();loadChannels();setInterval(()=>{load();loadFound();},3000);setInterval(loadChannels,60000);\n"
"</script>\n";

static void handle_add(int fd, const char *body) {
    cJSON *o = cJSON_Parse(body);
    if (!o) {
        send_response(fd, 400, "text/plain", "bad json\n", 9);
        return;
    }
    if (g_node_count >= MAX_NODES) {
        cJSON_Delete(o);
        send_response(fd, 400, "text/plain", "too many nodes\n", 15);
        return;
    }
    Node *nd = &g_nodes[g_node_count];
    memset(nd, 0, sizeof(*nd));
    cJSON *v = cJSON_GetObjectItem(o, "name");
    snprintf(nd->name, sizeof(nd->name), "%s", cJSON_IsString(v) ? v->valuestring : "STB");
    v = cJSON_GetObjectItem(o, "host");
    if (!cJSON_IsString(v) || !v->valuestring[0]) {
        cJSON_Delete(o);
        send_response(fd, 400, "text/plain", "host is required\n", 17);
        return;
    }
    snprintf(nd->host, sizeof(nd->host), "%s", v->valuestring);
    v = cJSON_GetObjectItem(o, "port");
    nd->port = cJSON_IsNumber(v) ? (int)v->valuedouble : 40774;
    v = cJSON_GetObjectItem(o, "channel");
    channel_from_json(v, nd->channel, sizeof(nd->channel), "20");
    if (!valid_channel(nd->channel)) {
        cJSON_Delete(o);
        send_response(fd, 400, "text/plain", "invalid channel\n", 16);
        return;
    }
    v = cJSON_GetObjectItem(o, "tuners");
    snprintf(nd->tuners, sizeof(nd->tuners), "%s",
             (cJSON_IsString(v) && valid_tuners(v->valuestring)) ? v->valuestring : "gr");
    v = cJSON_GetObjectItem(o, "enabled");
    nd->enabled = cJSON_IsTrue(v) ? 1 : 0;

    snprintf(nd->codec, sizeof(nd->codec), "hevc");
    v = cJSON_GetObjectItem(o, "codec");
    if (cJSON_IsString(v) && v->valuestring[0])
        snprintf(nd->codec, sizeof(nd->codec), "%s", v->valuestring);
    nd->width = 1920;
    nd->height = 1080;
    nd->bitrate = 5000000;
    snprintf(nd->rate_mode, sizeof(nd->rate_mode), "vbr");
    nd->gop_sec = 5;
    v = cJSON_GetObjectItem(o, "rateMode");
    if (cJSON_IsString(v) && v->valuestring[0])
        snprintf(nd->rate_mode, sizeof(nd->rate_mode), "%s", v->valuestring);
    v = cJSON_GetObjectItem(o, "gopSec");
    if (cJSON_IsNumber(v)) nd->gop_sec = (int)v->valuedouble;
    v = cJSON_GetObjectItem(o, "width");
    if (cJSON_IsNumber(v)) nd->width = (int)v->valuedouble;
    v = cJSON_GetObjectItem(o, "height");
    if (cJSON_IsNumber(v)) nd->height = (int)v->valuedouble;
    v = cJSON_GetObjectItem(o, "bitrate");
    if (cJSON_IsNumber(v)) nd->bitrate = (int)v->valuedouble;

    /* ID はホストとチャンネルから作る。 */
    {
        char new_id[96];
        snprintf(new_id, sizeof(new_id), "%s-%s", nd->host, nd->channel);
        memcpy(nd->id, new_id, sizeof(nd->id));
        nd->id[sizeof(nd->id) - 1] = '\0';
    }
    /*
     * 同じ STB は 1 ノードだけ。チャンネルが違っても同じチューナーなので、
     * 二重登録すると 2 本のレコーダーが ch を奪い合って両方録れなくなる。
     */
    {
        Node *existing = find_node_by_endpoint(nd->host, nd->port);
        if (existing) {
            char msg[192];
            snprintf(msg, sizeof(msg), "already registered as %s (ch %s)\n",
                     existing->id, existing->channel);
            cJSON_Delete(o);
            send_response(fd, 409, "text/plain", msg, strlen(msg));
            fprintf(stderr, "manager: rejected %s, %s already uses %s:%d\n",
                    nd->id, existing->id, nd->host, nd->port);
            return;
        }
    }
    g_node_count++;
    cJSON_Delete(o);
    save_config();
    if (nd->enabled) start_node(nd);
    send_response(fd, 201, "application/json", "{\"ok\":true}\n", 12);
    fprintf(stderr, "manager: added node %s\n", nd->id);
}

static void handle_request(int fd) {
    char buf[MAX_BODY + 4096];
    size_t total = 0;
    /* ヘッダ終端まで読む */
    while (total + 1 < sizeof(buf)) {
        ssize_t n = recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    if (total == 0) return;

    char method[16] = {0}, uri[256] = {0}, proto[16] = {0};
    if (sscanf(buf, "%15s %255s %15s", method, uri, proto) != 3) return;

    /* Content-Length 分の本文を読む */
    static char body[MAX_BODY];
    body[0] = '\0';
    char *sep = strstr(buf, "\r\n\r\n");
    if (sep) {
        size_t have = total - (size_t)(sep + 4 - buf);
        int want = 0;
        char *cl = strcasestr(buf, "content-length:");
        if (cl) want = atoi(cl + 15);
        if (want > 0 && (size_t)want < sizeof(body)) {
            size_t copied = have < (size_t)want ? have : (size_t)want;
            memcpy(body, sep + 4, copied);
            while (copied < (size_t)want) {
                ssize_t n = recv(fd, body + copied, (size_t)want - copied, 0);
                if (n <= 0) break;
                copied += (size_t)n;
            }
            body[copied] = '\0';
        }
    }

    if (strcmp(method, "GET") == 0 && strcmp(uri, "/") == 0) {
        send_response(fd, 200, "text/html; charset=utf-8", PAGE, strlen(PAGE));
        return;
    }
    /*
     * The Linux STB has no RTC and no NTP client, so its clock starts at the
     * epoch and recordings land in 1970/01/01. It asks for the time here at
     * boot instead of keeping a second time source in step.
     */
    if (strcmp(method, "GET") == 0 && strcmp(uri, "/api/time") == 0) {
        char stamp[32];
        snprintf(stamp, sizeof(stamp), "%lld", (long long)time(NULL));
        send_response(fd, 200, "text/plain", stamp, strlen(stamp));
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(uri, "/api/nodes") == 0) {
        ensure_channels();
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < g_node_count; i++) {
            cJSON_AddItemToArray(arr, node_json(&g_nodes[i]));
        }
        char *text = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        if (text) {
            send_response(fd, 200, "application/json", text, strlen(text));
            free(text);
        }
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(uri, "/api/channels") == 0) {
        ensure_channels();
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < g_channel_count; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "type", g_channels[i].type);
            cJSON_AddStringToObject(o, "channel", g_channels[i].channel);
            cJSON_AddStringToObject(o, "name", g_channels[i].name);
            cJSON_AddItemToArray(arr, o);
        }
        char *text = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        if (text) {
            send_response(fd, 200, "application/json", text, strlen(text));
            free(text);
        }
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(uri, "/api/discovered") == 0) {
        cJSON *arr = cJSON_CreateArray();
        time_t now = time(NULL);
        pthread_mutex_lock(&g_found_lock);
        for (int i = 0; i < g_found_count; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "instance", g_found[i].instance);
            cJSON_AddStringToObject(o, "name", g_found[i].display);
            cJSON_AddStringToObject(o, "host", g_found[i].ip);
            cJSON_AddNumberToObject(o, "port", g_found[i].port ? g_found[i].port : 40774);
            cJSON_AddNumberToObject(o, "channel", g_found[i].channel ? g_found[i].channel : 20);
            cJSON_AddNumberToObject(o, "ageSec", (double)(now - g_found[i].last_seen));
            cJSON_AddBoolToObject(o, "online", (now - g_found[i].last_seen) < 90);
            /*
             * Match by endpoint, not by the announced channel: the STB
             * announces a fixed value that says nothing about the tuner's
             * current state, and using it here used to offer "追加" for a box
             * that was already registered.
             */
            Node *reg = find_node_by_endpoint(g_found[i].ip,
                                              g_found[i].port ? g_found[i].port : 40774);
            cJSON_AddBoolToObject(o, "known", reg != NULL);
            if (reg) {
                cJSON_AddStringToObject(o, "knownId", reg->id);
                cJSON_AddStringToObject(o, "knownChannel", reg->channel);
            }
            cJSON_AddItemToArray(arr, o);
        }
        pthread_mutex_unlock(&g_found_lock);
        char *text = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        if (text) {
            send_response(fd, 200, "application/json", text, strlen(text));
            free(text);
        }
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(uri, "/api/nodes") == 0) {
        handle_add(fd, body);
        return;
    }
    if (strncmp(uri, "/api/nodes/", 11) == 0) {
        char id[64] = {0};
        char action[32] = {0};
        /* /api/nodes/<id> または /api/nodes/<id>/<action> */
        const char *p = uri + 11;
        const char *slash = strchr(p, '/');
        if (slash) {
            size_t n = (size_t)(slash - p);
            if (n >= sizeof(id)) n = sizeof(id) - 1;
            memcpy(id, p, n);
            id[n] = '\0';
            snprintf(action, sizeof(action), "%s", slash + 1);
        } else {
            size_t n = strlen(p);
            if (n >= sizeof(id)) n = sizeof(id) - 1;
            memcpy(id, p, n);
            id[n] = '\0';
        }

        Node *nd = find_node(id);
        if (!nd) {
            send_response(fd, 404, "text/plain", "not found\n", 10);
            return;
        }
        if (strcmp(method, "DELETE") == 0 && !action[0]) {
            stop_node(nd);
            int idx = (int)(nd - g_nodes);
            for (int i = idx; i + 1 < g_node_count; i++) g_nodes[i] = g_nodes[i + 1];
            g_node_count--;
            save_config();
            send_response(fd, 200, "application/json", "{\"ok\":true}\n", 12);
            fprintf(stderr, "manager: removed node %s\n", id);
            return;
        }
        /*
         * 録画設定 (チャンネルとエンコーダ)。変更したらレコーダーを再起動し、
         * STB に新しい設定でキャプチャさせる。チャンネルが変わると ID も
         * 作り直す (ID は host-channel)。
         */
        if (strcmp(method, "POST") == 0 && strcmp(action, "settings") == 0) {
            cJSON *o = cJSON_Parse(body);
            if (!o) {
                send_response(fd, 400, "text/plain", "bad json\n", 9);
                return;
            }
            int changed = 0;
            int channel_changed = 0;
            cJSON *fv = cJSON_GetObjectItem(o, "channel");
            if (cJSON_IsString(fv) || cJSON_IsNumber(fv)) {
                char ch[32];
                channel_from_json(fv, ch, sizeof(ch), nd->channel);
                if (!valid_channel(ch)) {
                    cJSON_Delete(o);
                    send_response(fd, 400, "text/plain", "invalid channel\n", 16);
                    return;
                }
                if (strcmp(ch, nd->channel) != 0) {
                    snprintf(nd->channel, sizeof(nd->channel), "%s", ch);
                    channel_changed = 1;
                    changed = 1;
                }
            }
            fv = cJSON_GetObjectItem(o, "tuners");
            if (cJSON_IsString(fv) && valid_tuners(fv->valuestring)) {
                if (strcmp(fv->valuestring, nd->tuners) != 0) {
                    snprintf(nd->tuners, sizeof(nd->tuners), "%s", fv->valuestring);
                    changed = 1;
                }
            }
            fv = cJSON_GetObjectItem(o, "codec");
            if (cJSON_IsString(fv) && (!strcmp(fv->valuestring, "hevc") || !strcmp(fv->valuestring, "avc"))) {
                snprintf(nd->codec, sizeof(nd->codec), "%s", fv->valuestring);
                changed = 1;
            }
            fv = cJSON_GetObjectItem(o, "width");
            if (cJSON_IsNumber(fv) && (int)fv->valuedouble >= 320 && (int)fv->valuedouble <= 1920) {
                nd->width = (int)fv->valuedouble;
                changed = 1;
            }
            fv = cJSON_GetObjectItem(o, "height");
            if (cJSON_IsNumber(fv) && (int)fv->valuedouble >= 240 && (int)fv->valuedouble <= 1080) {
                nd->height = (int)fv->valuedouble;
                changed = 1;
            }
            fv = cJSON_GetObjectItem(o, "bitrate");
            if (cJSON_IsNumber(fv) && (int)fv->valuedouble >= 200000 && (int)fv->valuedouble <= 20000000) {
                nd->bitrate = (int)fv->valuedouble;
                changed = 1;
            }
            fv = cJSON_GetObjectItem(o, "rateMode");
            if (cJSON_IsString(fv) && (!strcmp(fv->valuestring, "vbr") || !strcmp(fv->valuestring, "cbr"))) {
                snprintf(nd->rate_mode, sizeof(nd->rate_mode), "%s", fv->valuestring);
                changed = 1;
            }
            fv = cJSON_GetObjectItem(o, "gopSec");
            if (cJSON_IsNumber(fv) && (int)fv->valuedouble >= 1 && (int)fv->valuedouble <= 30) {
                nd->gop_sec = (int)fv->valuedouble;
                changed = 1;
            }
            cJSON_Delete(o);
            if (changed) {
                if (channel_changed) {
                    char new_id[96];
                    snprintf(new_id, sizeof(new_id), "%s-%s", nd->host, nd->channel);
                    memcpy(nd->id, new_id, sizeof(nd->id));
                    nd->id[sizeof(nd->id) - 1] = '\0';
                }
                save_config();
                if (nd->enabled) {
                    fprintf(stderr, "manager: node %s -> ch%s %s %dx%d %d bps\n",
                            nd->id, nd->channel, nd->codec, nd->width, nd->height, nd->bitrate);
                    stop_node(nd);
                    start_node(nd);
                }
            }
            char out[160];
            snprintf(out, sizeof(out), "{\"ok\":true,\"id\":\"%s\"}\n", nd->id);
            send_response(fd, 200, "application/json", out, strlen(out));
            return;
        }
        if (strcmp(method, "POST") == 0 && strcmp(action, "start") == 0) {
            nd->enabled = 1;
            start_node(nd);
            save_config();
            send_response(fd, 200, "application/json", "{\"ok\":true}\n", 12);
            return;
        }
        if (strcmp(method, "POST") == 0 && strcmp(action, "stop") == 0) {
            nd->enabled = 0;
            stop_node(nd);
            save_config();
            send_response(fd, 200, "application/json", "{\"ok\":true}\n", 12);
            return;
        }
    }
    send_response(fd, 404, "text/plain", "not found\n", 10);
}

int main(int argc, char **argv) {
    int port = 40776;
    snprintf(g_config_path, sizeof(g_config_path), "/home/ku/pix-stream-20261101/nodes.json");
    snprintf(g_recorder_path, sizeof(g_recorder_path), "/home/ku/pix-stream-20261101/pix_stream_recorder");
    snprintf(g_archive_url, sizeof(g_archive_url), "http://192.168.99.80:40773");
    snprintf(g_recordings_root, sizeof(g_recordings_root), "/mnt/xblk/video/pix-smb400");
    snprintf(g_tmpfs_dir, sizeof(g_tmpfs_dir), "/dev/shm/pix_stream");
    snprintf(g_state_dir, sizeof(g_state_dir), "/home/ku/pix-stream-20261101");
    g_bitrate = 5000000;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) snprintf(g_config_path, sizeof(g_config_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--recorder") && i + 1 < argc) snprintf(g_recorder_path, sizeof(g_recorder_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--archive-url") && i + 1 < argc) snprintf(g_archive_url, sizeof(g_archive_url), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--recordings-root") && i + 1 < argc) snprintf(g_recordings_root, sizeof(g_recordings_root), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--tmpfs-dir") && i + 1 < argc) snprintf(g_tmpfs_dir, sizeof(g_tmpfs_dir), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--state-dir") && i + 1 < argc) snprintf(g_state_dir, sizeof(g_state_dir), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc) g_bitrate = atoi(argv[++i]);
    }

    signal(SIGPIPE, SIG_IGN);
    /*
     * SIGCHLD must not be ignored: ignoring it makes the kernel reap the
     * children, after which waitpid can no longer tell a running recorder from
     * a finished one and every node looks stopped.
     */

    load_config();
    start_discovery();

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        return 1;
    }
    listen(listen_fd, 16);
    /* The loop also supervises the recorders, so accept must not block forever. */
    struct timeval poll_tv = { .tv_sec = 0, .tv_usec = 500 * 1000 };
    setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &poll_tv, sizeof(poll_tv));
    fprintf(stderr, "manager: listening on 0.0.0.0:%d, registry %s\n", port, g_config_path);

    /* Start the nodes that were enabled when the manager last stopped. */
    for (int i = 0; i < g_node_count; i++) {
        if (g_nodes[i].enabled) start_node(&g_nodes[i]);
    }

    time_t next_check = 0;
    for (;;) {
        /*
         * Bring an enabled recorder back after it stopped on its own, for
         * example because the STB rebooted underneath it. This is what makes
         * the registration survive a power cycle on either side.
         */
        time_t now = time(NULL);
        if (now >= next_check) {
            next_check = now + 10;
            for (int i = 0; i < g_node_count; i++) {
                if (g_nodes[i].enabled && !node_running(&g_nodes[i])) {
                    fprintf(stderr, "manager: node %s stopped, restarting\n", g_nodes[i].id);
                    start_node(&g_nodes[i]);
                }
            }
        }

        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(200 * 1000);
                continue;
            }
            break;
        }
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        handle_request(fd);
        close(fd);
    }
    close(listen_fd);
    return 0;
}
