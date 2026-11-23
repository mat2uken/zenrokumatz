#include "fmp4_to_mp4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static volatile int g_stop = 0;
static void on_sig(int s) {
    (void)s;
    g_stop = 1;
}

static int read_line(int fd, char *buf, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '\n') {
            if (n > 0 && buf[n - 1] == '\r') n--;
            buf[n] = '\0';
            return 0;
        }
        buf[n++] = c;
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "Usage: %s <stb_ip> <port> <stream_id> <tmpfs_dir> <out_mp4_path> [--max-frags N]\n", argv[0]);
        return 1;
    }

    const char *stb_ip = argv[1];
    int port = atoi(argv[2]);
    const char *stream_id = argv[3];
    const char *tmpfs_dir = argv[4];
    const char *out_mp4_path = argv[5];

    int max_frags = 0;
    for (int i = 6; i < argc; i++) {
        if (strcmp(argv[i], "--max-frags") == 0 && i + 1 < argc) {
            max_frags = atoi(argv[++i]);
        }
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    printf("NAS Stream Collector starting...\n");
    printf("Target STB: %s:%d (stream: %s)\n", stb_ip, port, stream_id);
    printf("Tmpfs Staging Dir: %s\n", tmpfs_dir);
    printf("Final Destination: %s\n", out_mp4_path);

    /* 1. STB へ TCP 接続 (NAS起点接続) */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port)
    };
    if (inet_pton(AF_INET, stb_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid STB IP: %s\n", stb_ip);
        close(fd);
        return 1;
    }

    printf("Connecting to STB %s:%d...\n", stb_ip, port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("connect");
        close(fd);
        return 1;
    }
    printf("Connected to STB!\n");

    /* 2. GET /v1/streams/<stream_id> 送信 */
    char req[512];
    int qlen = snprintf(req, sizeof(req),
        "GET /v1/streams/%s?after=0 HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: pix-nas-collector/1.0\r\n"
        "Connection: close\r\n"
        "\r\n", stream_id, stb_ip, port);
    if (send(fd, req, (size_t)qlen, 0) != qlen) {
        perror("send");
        close(fd);
        return 1;
    }

    /* 3. HTTP ステータス行とヘッダの確認 */
    char line[1024];
    if (read_line(fd, line, sizeof(line)) != 0 || !strstr(line, " 200 ")) {
        fprintf(stderr, "Unexpected HTTP response: %s\n", line);
        close(fd);
        return 1;
    }

    int is_chunked = 0;
    while (read_line(fd, line, sizeof(line)) == 0) {
        if (line[0] == '\0') break;
        if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 && strstr(line, "chunked")) {
            is_chunked = 1;
        }
    }

    if (!is_chunked) {
        fprintf(stderr, "Error: Server response is not Transfer-Encoding: chunked\n");
        close(fd);
        return 1;
    }

    /* 4. tmpfs 上の一時 fMP4 ファイルを作成 */
    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s/staging_%d.fmp4", tmpfs_dir, (int)getpid());
    FILE *ftmp = fopen(tmp_path, "wb");
    if (!ftmp) {
        perror("fopen tmpfs");
        close(fd);
        return 1;
    }
    printf("Writing incoming fMP4 stream to tmpfs: %s\n", tmp_path);

    /* 5. chunked 本文を受信して tmpfs へ書き込み */
    uint64_t total_bytes = 0;
    char chunk_hdr[64];
    int frags_received = 0;

    while (!g_stop && read_line(fd, chunk_hdr, sizeof(chunk_hdr)) == 0) {
        char *end = NULL;
        unsigned long chunk_sz = strtoul(chunk_hdr, &end, 16);
        if (end == chunk_hdr) break;
        if (chunk_sz == 0) {
            printf("Received terminating chunk (0).\n");
            break;
        }

        /* chunk データを読み取り */
        size_t left = chunk_sz;
        char buf[8192];
        while (left > 0 && !g_stop) {
            size_t want = left > sizeof(buf) ? sizeof(buf) : left;
            ssize_t r = recv(fd, buf, want, 0);
            if (r <= 0) break;
            fwrite(buf, 1, (size_t)r, ftmp);
            left -= (size_t)r;
            total_bytes += (size_t)r;
        }

        /* chunk 末尾の CRLF を読み飛ばす */
        char crlf[2];
        recv(fd, crlf, 2, MSG_WAITALL);

        frags_received++;
        if (max_frags > 0 && frags_received >= max_frags) {
            printf("Reached target fragment count: %d\n", frags_received);
            break;
        }
    }

    fclose(ftmp);
    close(fd);
    printf("Stream receive loop ended. Total bytes: %llu\n", (unsigned long long)total_bytes);

    /* 6. 通常 MP4 への再構成 (finalize) と tmpfs 即時解放 */
    printf("Finalizing fMP4 -> Normal MP4 (faststart) to %s...\n", out_mp4_path);
    if (fmp4_to_mp4_finalize(tmp_path, out_mp4_path, 1 /* remove_input: tmpfsを自動unlink */) != 0) {
        fprintf(stderr, "Finalization failed!\n");
        return 1;
    }

    printf("SUCCESS: Normal MP4 created at %s, tmpfs file unlinked!\n", out_mp4_path);
    return 0;
}
