/*
 * mdns_announce: 自前の最小mDNSアナウンサー。
 *
 * STBが自分から「ここにいる」と定期的に知らせるためのもの。ライブラリは使わず、
 * DNSメッセージを直接組み立てて 224.0.0.251:5353 へ送る。
 *
 * 通知する内容:
 *   PTR  _pixstb._tcp.local      -> <host>._pixstb._tcp.local
 *   SRV  <host>._pixstb._tcp.local -> HTTPポートとホスト名
 *   TXT  <host>._pixstb._tcp.local -> 表示名・チャンネルなど
 *   A    <host>.local            -> IPv4アドレス
 *
 * 問い合わせが来たときにも同じ内容を返すので、NASを後から起動しても即座に見つかる。
 */

#include "mdns_announce.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <net/if.h>

#define MDNS_ADDR "224.0.0.251"
#define MDNS_PORT 5353
#define SERVICE "_pixstb._tcp.local"
#define MAX_PACKET 1400
#define ANNOUNCE_INTERVAL_SEC 30

struct MdnsAnnouncer {
    pthread_t thread;
    volatile int running;
    int fd;
    int port;                      /* 通知するHTTPポート */
    char host[64];                 /* インスタンス名 */
    char display[96];              /* 画面や管理画面に出す名前 */
    int channel;
    struct in_addr addr;           /* 自分のIPv4 */
};

/* ---- DNS書き込みヘルパ ---- */

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} Out;

static void put8(Out *o, uint8_t v) {
    if (o->len < o->cap) o->buf[o->len++] = v;
}

static void put16(Out *o, uint16_t v) {
    put8(o, (uint8_t)(v >> 8));
    put8(o, (uint8_t)(v & 0xff));
}

static void put32(Out *o, uint32_t v) {
    put16(o, (uint16_t)(v >> 16));
    put16(o, (uint16_t)(v & 0xffff));
}

static void put_bytes(Out *o, const void *p, size_t n) {
    if (o->len + n > o->cap) n = o->cap > o->len ? o->cap - o->len : 0;
    if (n) memcpy(o->buf + o->len, p, n);
    o->len += n;
}

/* "a.b.local" を長さ付きラベルで書く */
static void put_name(Out *o, const char *name) {
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t n = dot ? (size_t)(dot - p) : strlen(p);
        if (n == 0) break;
        if (n > 63) n = 63;
        put8(o, (uint8_t)n);
        put_bytes(o, p, n);
        if (!dot) break;
        p = dot + 1;
    }
    put8(o, 0);
}

static size_t put_record_header(Out *o, const char *name, uint16_t type,
                                uint32_t ttl) {
    put_name(o, name);
    put16(o, type);
    put16(o, 0x8001);   /* class IN, cache-flush bit */
    put32(o, ttl);
    size_t len_at = o->len;
    put16(o, 0);        /* rdlength は後で埋める */
    return len_at;
}

static void put_record_end(Out *o, size_t len_at) {
    size_t rdl = o->len - len_at - 2;
    if (len_at + 1 < o->cap) {
        o->buf[len_at] = (uint8_t)(rdl >> 8);
        o->buf[len_at + 1] = (uint8_t)(rdl & 0xff);
    }
}

static void put_txt_entry(Out *o, const char *key, const char *value) {
    char item[128];
    int n = snprintf(item, sizeof(item), "%s=%s", key, value);
    if (n <= 0 || n >= (int)sizeof(item)) return;
    if (n > 255) n = 255;
    put8(o, (uint8_t)n);
    put_bytes(o, item, (size_t)n);
}

/* 通知パケット全体を組み立てる */
static size_t build_packet(MdnsAnnouncer *a, uint8_t *buf, size_t cap, int is_response) {
    Out o = { buf, 0, cap };
    char instance[128];
    char hostname[128];
    char addr_str[32];

    snprintf(instance, sizeof(instance), "%s.%s", a->host, SERVICE);
    snprintf(hostname, sizeof(hostname), "%s.local", a->host);
    snprintf(addr_str, sizeof(addr_str), "%s", inet_ntoa(a->addr));

    put16(&o, 0);                      /* ID */
    put16(&o, is_response ? 0x8400 : 0x0000);
    put16(&o, 0);                      /* QDCOUNT */
    put16(&o, 4);                      /* ANCOUNT: PTR, SRV, TXT, A */
    put16(&o, 0);
    put16(&o, 0);

    /* PTR */
    size_t ptr = put_record_header(&o, SERVICE, 12, 120);
    put_name(&o, instance);
    put_record_end(&o, ptr);

    /* SRV */
    size_t srv = put_record_header(&o, instance, 33, 120);
    put16(&o, 0);                      /* priority */
    put16(&o, 0);                      /* weight */
    put16(&o, (uint16_t)a->port);
    put_name(&o, hostname);
    put_record_end(&o, srv);

    /* TXT */
    size_t txt = put_record_header(&o, instance, 16, 120);
    put_txt_entry(&o, "name", a->display);
    char ch[16];
    snprintf(ch, sizeof(ch), "%d", a->channel);
    put_txt_entry(&o, "ch", ch);
    put_txt_entry(&o, "ip", addr_str);
    put_txt_entry(&o, "ver", "1");
    put_record_end(&o, txt);

    /* A */
    size_t arec = put_record_header(&o, hostname, 1, 120);
    put_bytes(&o, &a->addr, 4);
    put_record_end(&o, arec);

    return o.len;
}

static void send_packet(MdnsAnnouncer *a, int is_response) {
    uint8_t buf[MAX_PACKET];
    size_t n = build_packet(a, buf, sizeof(buf), is_response);
    if (n == 0) return;

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(MDNS_PORT);

    inet_pton(AF_INET, MDNS_ADDR, &to.sin_addr);
    if (sendto(a->fd, buf, n, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
        fprintf(stderr, "[MDNS] multicast send failed: %s\n", strerror(errno));
    }

    /*
     * Switches with IGMP snooping do not always forward multicast to every
     * port, so the same announcement is also broadcast. Listening on 5353
     * receives both forms.
     */
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    if (sendto(a->fd, buf, n, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
        fprintf(stderr, "[MDNS] broadcast send failed: %s\n", strerror(errno));
    }
}

/* 問い合わせに "_pixstb" が含まれていれば応答する */
static int looks_like_query(const uint8_t *buf, size_t n) {
    if (n < 12) return 0;
    if (buf[2] & 0x80) return 0;            /* QR=1 は応答 */
    uint16_t qd = (uint16_t)((buf[4] << 8) | buf[5]);
    if (qd == 0) return 0;

    /* 名前部分を素朴に走査して "_pixstb" を探す */
    for (size_t i = 12; i + 8 <= n; i++) {
        if (memcmp(buf + i, "_pixstb", 7) == 0) return 1;
    }
    return 0;
}

static void *mdns_thread(void *arg) {
    MdnsAnnouncer *a = (MdnsAnnouncer *)arg;
    time_t last_announce = 0;

    while (a->running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(a->fd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int r = select(a->fd + 1, &rfds, NULL, NULL, &tv);

        if (r > 0 && FD_ISSET(a->fd, &rfds)) {
            uint8_t pkt[1500];
            struct sockaddr_in from;
            socklen_t flen = sizeof(from);
            ssize_t n = recvfrom(a->fd, pkt, sizeof(pkt), 0,
                                 (struct sockaddr *)&from, &flen);
            if (n > 0 && looks_like_query(pkt, (size_t)n)) {
                send_packet(a, 1);
            }
        }

        time_t now = time(NULL);
        if (now - last_announce >= ANNOUNCE_INTERVAL_SEC) {
            last_announce = now;
            send_packet(a, 1);
            fprintf(stderr, "[MDNS] announced %s.%s port %d at %s\n",
                    a->host, SERVICE, a->port, inet_ntoa(a->addr));
        }
    }
    return NULL;
}

MdnsAnnouncer *mdns_announce_start(const MdnsAnnounceConfig *cfg) {
    if (!cfg || !cfg->port || !cfg->host || cfg->addr.s_addr == 0) return NULL;

    MdnsAnnouncer *a = calloc(1, sizeof(*a));
    if (!a) return NULL;

    a->port = cfg->port;
    a->channel = cfg->channel;
    snprintf(a->host, sizeof(a->host), "%s", cfg->host);
    snprintf(a->display, sizeof(a->display), "%s",
             cfg->display ? cfg->display : cfg->host);
    a->addr = cfg->addr;

    a->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (a->fd < 0) {
        fprintf(stderr, "[MDNS] socket failed: %s\n", strerror(errno));
        free(a);
        return NULL;
    }
    int one = 1;
    setsockopt(a->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(a->fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MDNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(a->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        /* Another responder already owns the port: announcements still work. */
        fprintf(stderr, "[MDNS] bind 5353 failed (%s), announcing only\n",
                strerror(errno));
        addr.sin_port = 0;
        bind(a->fd, (struct sockaddr *)&addr, sizeof(addr));
    }

    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(MDNS_ADDR);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(a->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    unsigned char ttl = 255;
    setsockopt(a->fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(a->fd, IPPROTO_IP, IP_MULTICAST_LOOP, &one, sizeof(one));
    setsockopt(a->fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

    a->running = 1;
    if (pthread_create(&a->thread, NULL, mdns_thread, a) != 0) {
        close(a->fd);
        free(a);
        return NULL;
    }

    fprintf(stderr, "[MDNS] announcing %s.local as %s\n",
            a->host, inet_ntoa(a->addr));
    return a;
}

void mdns_announce_stop(MdnsAnnouncer *a) {
    if (!a) return;
    a->running = 0;
    pthread_join(a->thread, NULL);
    close(a->fd);
    free(a);
}
