/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pix-ring - 追記型ファイルの代わりに使う、大きさの決まったリングバッファ
 *
 * 復号器の出力を直接エンコーダへ繋ぐと、エンコーダの初期化中に復号器の
 * 書き込みが止まり、カードのやり取りごと止まって落ちる。かといって
 * 追記型のファイルに書くと、録画の長さだけ育って tmpfs を埋める。
 *
 * ここでは固定長の領域を回して使う。書き手は空きが無ければ待ち、
 * 読み手は書かれた分だけを読む。大きさは数十秒分あれば足りる。
 *
 *   pix-ring --path FILE --size MB --produce < TS > /dev/null
 *   pix-ring --path FILE --size MB --consume < /dev/null > TS
 *
 * 書き手と読み手は同じヘッダを共有する。書き込み位置を書くのは書き手
 * だけ、読み出し位置を書くのは読み手だけなので、ロックは要らない。
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HEADER_SIZE 4096
#define MAGIC "PIXRING1"
#define CHUNK (64 * 1024)

struct header {
    char magic[8];
    uint64_t size;      /* データ領域の大きさ */
    uint64_t write;     /* 書き込んだ累計バイト数 */
    uint64_t read;      /* 読み出した累計バイト数 */
    uint32_t closed;    /* 書き手が終わったら1 */
    uint32_t pad;
};

static void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void *map_ring(const char *path, uint64_t size, int create) {
    int flags = create ? (O_RDWR | O_CREAT) : O_RDWR;
    int fd = open(path, flags, 0600);
    if (fd < 0) {
        fprintf(stderr, "pix-ring: cannot open %s: %s\n", path, strerror(errno));
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return NULL;
    }
    if ((uint64_t)st.st_size < HEADER_SIZE + size) {
        if (!create) {
            fprintf(stderr, "pix-ring: %s is smaller than requested\n", path);
            close(fd);
            return NULL;
        }
        if (ftruncate(fd, (off_t)(HEADER_SIZE + size)) != 0) {
            fprintf(stderr, "pix-ring: cannot size %s: %s\n", path, strerror(errno));
            close(fd);
            return NULL;
        }
    }
    void *base = mmap(NULL, HEADER_SIZE + size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        fprintf(stderr, "pix-ring: cannot map %s: %s\n", path, strerror(errno));
        return NULL;
    }
    return base;
}

static int produce(const char *path, uint64_t size) {
    struct header *h = map_ring(path, size, 1);
    if (!h) return 1;
    memcpy(h->magic, MAGIC, sizeof(MAGIC));
    h->size = size;
    __atomic_store_n(&h->write, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&h->read, 0, __ATOMIC_RELEASE);
    h->closed = 0;
    uint8_t *data = (uint8_t *)h + HEADER_SIZE;
    uint8_t *buf = malloc(CHUNK);
    if (!buf) return 1;
    for (;;) {
        ssize_t n = read(STDIN_FILENO, buf, CHUNK);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        size_t left = (size_t)n;
        const uint8_t *p = buf;
        while (left > 0) {
            uint64_t w = __atomic_load_n(&h->write, __ATOMIC_ACQUIRE);
            uint64_t r = __atomic_load_n(&h->read, __ATOMIC_ACQUIRE);
            uint64_t used = w - r;
            if (used >= size) {
                /* 読み手が追いつくまで待つ。ここで待つのは、待たないと
                 * データを捨てることになり、復号の整合が崩れるため。 */
                sleep_ms(2);
                continue;
            }
            uint64_t free_bytes = size - used;
            size_t take = left;
            if ((uint64_t)take > free_bytes) take = (size_t)free_bytes;
            uint64_t off = w % size;
            size_t first = (size_t)(size - off);
            if (first > take) first = take;
            memcpy(data + off, p, first);
            if (take > first) memcpy(data, p + first, take - first);
            __atomic_store_n(&h->write, w + take, __ATOMIC_RELEASE);
            p += take;
            left -= take;
        }
    }
    free(buf);
    __atomic_store_n(&h->closed, 1, __ATOMIC_RELEASE);
    msync(h, HEADER_SIZE + size, MS_SYNC);
    munmap(h, HEADER_SIZE + size);
    return 0;
}

static int consume(const char *path, uint64_t size) {
    struct header *h = map_ring(path, size, 0);
    if (!h) return 1;
    if (memcmp(h->magic, MAGIC, sizeof(MAGIC)) != 0) {
        fprintf(stderr, "pix-ring: %s is not a ring\n", path);
        return 1;
    }
    uint8_t *data = (uint8_t *)h + HEADER_SIZE;
    uint8_t *buf = malloc(CHUNK);
    if (!buf) return 1;
    for (;;) {
        uint64_t w = __atomic_load_n(&h->write, __ATOMIC_ACQUIRE);
        uint64_t r = __atomic_load_n(&h->read, __ATOMIC_ACQUIRE);
        if (w == r) {
            if (__atomic_load_n(&h->closed, __ATOMIC_ACQUIRE)) break;
            sleep_ms(2);
            continue;
        }
        uint64_t avail = w - r;
        size_t take = avail > CHUNK ? CHUNK : (size_t)avail;
        uint64_t off = r % size;
        size_t first = (size_t)(size - off);
        if (first > take) first = take;
        memcpy(buf, data + off, first);
        if (take > first) memcpy(buf + first, data, take - first);
        size_t written = 0;
        while (written < take) {
            ssize_t n = write(STDOUT_FILENO, buf + written, take - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                free(buf);
                munmap(h, HEADER_SIZE + size);
                return 1;
            }
            written += (size_t)n;
        }
        __atomic_store_n(&h->read, r + take, __ATOMIC_RELEASE);
    }
    free(buf);
    munmap(h, HEADER_SIZE + size);
    return 0;
}

int main(int argc, char **argv) {
    const char *path = "/dev/shm/pix/live.ring";
    uint64_t size_mb = 16;
    int mode = 0; /* 1 produce, 2 consume */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--path") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) size_mb = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--produce")) mode = 1;
        else if (!strcmp(argv[i], "--consume")) mode = 2;
        else {
            fprintf(stderr, "usage: pix-ring --path FILE [--size MB] --produce|--consume\n");
            return 2;
        }
    }
    if (!mode || size_mb == 0) {
        fprintf(stderr, "usage: pix-ring --path FILE [--size MB] --produce|--consume\n");
        return 2;
    }
    uint64_t size = size_mb * 1024 * 1024;
    return mode == 1 ? produce(path, size) : consume(path, size);
}
