/*
 * timesync: 最小構成のSNTPクライアント。
 *
 * Androidの自動時刻合わせは間隔が長く、この機種では無効にされていた時期も
 * ある。起動時と運用中に確実に時刻を合わせるため、1回だけ同期して終了する
 * 小さな道具として用意し、ウォッチドッグから定期的に呼ぶ。
 *
 * rootで実行すると settimeofday で時刻を設定する。ずれが小さいときは何もしない。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <arpa/inet.h>

#define NTP_PORT 123
#define NTP_PACKET 48
#define NTP_UNIX_OFFSET 2208988800ULL

static const char *g_servers[] = {
    "ntp.nict.jp",
    "pool.ntp.org",
    "time.cloudflare.com",
    NULL
};

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t read_ntp_time(const uint8_t *data) {
    uint64_t secs = 0;
    for (int i = 0; i < 4; i++) secs = (secs << 8) | data[40 + i];
    return secs;
}

/* 1つのサーバーへ問い合わせ、受け取った時刻 (ms) と往復時間 (ms) を返す。 */
static int query(const char *server, int timeout_ms,
                 int64_t *out_time_ms, int64_t *out_rtt_ms) {
    char port[16];
    snprintf(port, sizeof(port), "%d", NTP_PORT);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(server, port, &hints, &res) != 0 || !res) {
        return -1;
    }

    uint8_t packet[NTP_PACKET];
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x23;   /* LI = 0, version 4, mode 3 (client) */

    /*
     * A UDP connect() succeeds even when the peer is unreachable, so every
     * address has to be tried until one actually answers. Otherwise an address
     * family the box cannot route simply burns the timeout.
     */
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, SOCK_DGRAM, 0);
        if (fd < 0) continue;

        struct timeval tv = { .tv_sec = timeout_ms / 1000,
                              .tv_usec = (timeout_ms % 1000) * 1000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            close(fd);
            continue;
        }

        int64_t t1 = now_ms();
        if (send(fd, packet, sizeof(packet), 0) != (ssize_t)sizeof(packet)) {
            close(fd);
            continue;
        }

        uint8_t reply[NTP_PACKET];
        ssize_t n = recv(fd, reply, sizeof(reply), 0);
        int64_t t4 = now_ms();
        close(fd);

        if (n < NTP_PACKET) continue;
        if ((reply[0] & 0x07) != 4) continue;   /* mode 4 = server */
        if (reply[1] == 0) continue;            /* stratum 0 = kiss of death */

        uint64_t ntp_secs = read_ntp_time(reply);
        if (ntp_secs < 1000000000ULL) continue;

        int64_t server_ms = (int64_t)((ntp_secs - NTP_UNIX_OFFSET) * 1000ULL);
        *out_time_ms = server_ms + (t4 - t1) / 2;
        *out_rtt_ms = t4 - t1;
        freeaddrinfo(res);
        return 0;
    }

    freeaddrinfo(res);
    return -1;
}

int main(int argc, char **argv) {
    int timeout_ms = 3000;
    int64_t threshold_ms = 1000;
    const char *only = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--server") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) timeout_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threshold") && i + 1 < argc) threshold_ms = atoi(argv[++i]);
    }

    const char *list[4];
    int count = 0;
    if (only) {
        list[count++] = only;
    } else {
        for (int i = 0; g_servers[i]; i++) list[count++] = g_servers[i];
    }

    for (int i = 0; i < count; i++) {
        int64_t target = 0, rtt = 0;
        if (query(list[i], timeout_ms, &target, &rtt) != 0) {
            fprintf(stderr, "timesync: %s unreachable\n", list[i]);
            continue;
        }
        int64_t current = now_ms();
        int64_t delta = target - current;
        if (delta < 0) delta = -delta;

        if (delta < threshold_ms) {
            printf("timesync: %s ok, offset %lldms (within %lldms)\n",
                   list[i], (long long)(target - current), (long long)threshold_ms);
            return 0;
        }

        struct timeval tv;
        tv.tv_sec = (time_t)(target / 1000);
        tv.tv_usec = (suseconds_t)((target % 1000) * 1000);
        if (settimeofday(&tv, NULL) != 0) {
            fprintf(stderr, "timesync: settimeofday failed: %s\n", strerror(errno));
            return 2;
        }
        printf("timesync: %s set clock, offset %lldms rtt %lldms\n",
               list[i], (long long)(target - current), (long long)rtt);
        return 0;
    }

    fprintf(stderr, "timesync: no server answered\n");
    return 1;
}
