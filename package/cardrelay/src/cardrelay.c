/* cardrelay.c - bridge the local torne card socket to a remote bcasd.
 *
 * torne-b25 opens /dev/shm/torne/card.sock and speaks the 524-byte
 * TorneCardMessage protocol. This relay accepts that connection, opens a TCP
 * connection to bcasd, authenticates with a token, and forwards the frames in
 * both directions. It never stores key material.
 *
 * Config (default /data/local/tmp/pix-stream/bcas.conf):
 *   host=192.168.99.10
 *   port=40790
 *   token=<64 hex characters>
 *   socket=/dev/shm/torne/card.sock
 *   connect_timeout_ms=3000
 *   apdu_timeout_ms=5000
 *
 * Build (Android armv7a):
 *   $CC -std=gnu11 -O2 -Wall -Wextra -Werror -o cardrelay cardrelay.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define FRAME_SIZE 524
#define MAX_APDU 512
#define PATH_MAX_LEN 108

typedef struct {
    uint32_t operation;
    uint32_t size;
    int32_t status;
    uint8_t data[MAX_APDU];
} __attribute__((packed)) Frame;

static char g_host[128] = "192.168.99.10";
static int g_port = 40790;
static uint8_t g_token[32];
static int g_token_set;
static char g_socket[PATH_MAX_LEN] = "/dev/shm/torne/card.sock";
static int g_connect_ms = 3000;
static int g_apdu_ms = 5000;
static volatile sig_atomic_t g_stop;

static void log_msg(const char *fmt, ...)
{
    va_list ap;
    struct timespec ts;
    struct tm tm;
    char stamp[32];

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
    flockfile(stderr);
    fprintf(stderr, "%s ", stamp);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    funlockfile(stderr);
}

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_token(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64) return -1;
    for (int i = 0; i < 32; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) end--;
    *end = 0;
    return s;
}

static int load_token_file(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    int ok = -1;

    if (!f) {
        log_msg("cannot open token file %s: %s", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (!*p || *p == '#') continue;
        if (!parse_token(p, g_token)) {
            g_token_set = 1;
            ok = 0;
            break;
        }
    }
    fclose(f);
    if (ok) log_msg("no valid token in %s", path);
    return ok;
}

static int load_config(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];

    if (!f) {
        log_msg("cannot open config %s: %s", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        char *eq;
        if (!*p || *p == '#') continue;
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(p);
        char *val = trim(eq + 1);
        if (!strcmp(key, "host")) snprintf(g_host, sizeof(g_host), "%s", val);
        else if (!strcmp(key, "port")) g_port = atoi(val);
        else if (!strcmp(key, "socket")) snprintf(g_socket, sizeof(g_socket), "%s", val);
        else if (!strcmp(key, "connect_timeout_ms")) g_connect_ms = atoi(val);
        else if (!strcmp(key, "apdu_timeout_ms")) g_apdu_ms = atoi(val);
        else if (!strcmp(key, "token")) {
            if (parse_token(val, g_token)) {
                log_msg("invalid token in config");
                fclose(f);
                return -1;
            }
            g_token_set = 1;
        } else if (!strcmp(key, "token_file")) {
            if (load_token_file(val)) {
                fclose(f);
                return -1;
            }
        }
    }
    fclose(f);
    if (!g_token_set) {
        log_msg("no token configured");
        return -1;
    }
    if (g_port < 1 || g_port > 65535) return -1;
    if (g_connect_ms < 100 || g_connect_ms > 60000) return -1;
    if (g_apdu_ms < 100 || g_apdu_ms > 60000) return -1;
    return 0;
}

static int read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static void set_timeout(int fd, int ms)
{
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int connect_remote(void)
{
    struct addrinfo hints, *res = NULL, *ai;
    char port[16];
    int fd = -1;

    snprintf(port, sizeof(port), "%d", g_port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(g_host, port, &hints, &res)) {
        log_msg("cannot resolve %s", g_host);
        return -1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        int flags;
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
            if (errno != EINPROGRESS) {
                close(fd);
                fd = -1;
                continue;
            }
            struct pollfd p = { fd, POLLOUT, 0 };
            int r = poll(&p, 1, g_connect_ms);
            if (r <= 0) {
                close(fd);
                fd = -1;
                continue;
            }
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) || error) {
                close(fd);
                fd = -1;
                continue;
            }
        }
        fcntl(fd, F_SETFL, flags);
        break;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        log_msg("cannot connect to %s:%d", g_host, g_port);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    set_timeout(fd, g_apdu_ms);

    Frame hello, reply;
    memset(&hello, 0, sizeof(hello));
    hello.operation = 0;
    hello.size = 33;
    hello.data[0] = 1;
    memcpy(hello.data + 1, g_token, 32);
    if (write_full(fd, &hello, sizeof(hello)) || read_full(fd, &reply, sizeof(reply)) ||
        reply.operation != 0 || reply.status != 0) {
        /*
         * The server answers with a status code, so say which one. -2 means it
         * has no free client slot, -3 means the token is not in its list; a
         * silent close means the peer was rejected by its allow list.
         */
        if (reply.operation == 0 && reply.status == -2)
            log_msg("bcasd is full (no free client slot)");
        else if (reply.operation == 0 && reply.status == -3)
            log_msg("bcasd rejected the token; register it in the server's tokens file");
        else
            log_msg("bcasd authentication failed (status=%d)", reply.status);
        close(fd);
        return -1;
    }
    log_msg("connected to %s:%d", g_host, g_port);
    return fd;
}

static void serve_one(int local_fd)
{
    int remote_fd = connect_remote();
    if (remote_fd < 0) return;
    set_timeout(local_fd, g_apdu_ms);

    for (;;) {
        struct pollfd p[2] = {
            { local_fd, POLLIN, 0 },
            { remote_fd, POLLIN, 0 },
        };
        int r = poll(p, 2, 500);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            if (g_stop) break;
            continue;
        }
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            Frame f;
            if (read_full(local_fd, &f, sizeof(f)) || f.size > MAX_APDU) break;
            if (write_full(remote_fd, &f, sizeof(f))) break;
        }
        if (p[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            Frame f;
            if (read_full(remote_fd, &f, sizeof(f)) || f.size > MAX_APDU) break;
            if (write_full(local_fd, &f, sizeof(f))) break;
        }
    }
    close(remote_fd);
    log_msg("remote connection closed");
}

static int listen_local(void)
{
    struct sockaddr_un addr;
    int fd;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(g_socket) >= sizeof(addr.sun_path)) {
        close(fd);
        return -1;
    }
    strcpy(addr.sun_path, g_socket);
    unlink(g_socket);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        close(fd);
        return -1;
    }
    chmod(g_socket, 0600);
    if (listen(fd, 1)) {
        close(fd);
        unlink(g_socket);
        return -1;
    }
    return fd;
}

static void on_stop(int sig)
{
    (void)sig;
    g_stop = 1;
}

int main(int argc, char **argv)
{
    const char *config = "/data/local/tmp/pix-stream/bcas.conf";
    struct sigaction sa;
    int listen_fd;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) config = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--config PATH]\n", argv[0]);
            return 2;
        }
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (load_config(config)) return 1;
    listen_fd = listen_local();
    if (listen_fd < 0) {
        log_msg("cannot listen on %s: %s", g_socket, strerror(errno));
        return 1;
    }
    log_msg("cardrelay listening on %s -> %s:%d", g_socket, g_host, g_port);

    while (!g_stop) {
        struct pollfd p = { listen_fd, POLLIN, 0 };
        int r = poll(&p, 1, 500);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) continue;
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) continue;
        log_msg("local client connected");
        serve_one(fd);
        close(fd);
        log_msg("local client disconnected");
    }

    close(listen_fd);
    unlink(g_socket);
    log_msg("shutting down");
    return 0;
}
