/*
 * logo_store.c - 局ロゴの保存 (PNG + サイドカー + index.json)
 *
 * index.json は <id>.json を読み直して組み立てる。JSON のパーサは要らない:
 * 自分で書いたサイドカーを、そのまま並べるだけ。
 */
#define _POSIX_C_SOURCE 200809L
#include "logo_store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SIDECAR_MAX 4096
#define INDEX_MAX (512u * 1024u)
#define INDEX_ENTRIES 1024

int logo_store_mkdirs(const char *path) {
    char buffer[PATH_MAX];
    if (!path || strlen(path) >= sizeof(buffer)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    snprintf(buffer, sizeof(buffer), "%s", path);
    for (char *p = buffer + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buffer, 0755) && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(buffer, 0755) && errno != EEXIST)
        return -1;
    return 0;
}

static void json_escape(char *out, size_t cap, const char *in) {
    size_t at = 0;
    for (; *in && at + 7 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            out[at++] = '\\';
            out[at++] = (char)c;
        } else if (c < 0x20) {
            at += (size_t)snprintf(out + at, cap - at, "\\u%04x", c);
        } else {
            out[at++] = (char)c;
        }
    }
    out[at] = '\0';
}

/* Write only when the bytes differ, so a repeat run leaves the card alone. */
static int write_changed(const char *path, const void *data, size_t length) {
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        char buffer[4096];
        size_t at = 0;
        int same = 1;
        while (same) {
            ssize_t n = read(fd, buffer, sizeof(buffer));
            if (n < 0) {
                same = 0;
                break;
            }
            if (n == 0)
                break;
            if (at + (size_t)n > length || memcmp(buffer, (const char *)data + at, (size_t)n)) {
                same = 0;
                break;
            }
            at += (size_t)n;
        }
        close(fd);
        if (same && at == length)
            return 0;
    }
    char temporary[PATH_MAX];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp%ld", path, (long)getpid()) >=
        (int)sizeof(temporary)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    const char *p = data;
    size_t left = length;
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n <= 0) {
            close(fd);
            unlink(temporary);
            return -1;
        }
        p += n;
        left -= (size_t)n;
    }
    if (close(fd) || rename(temporary, path)) {
        unlink(temporary);
        return -1;
    }
    return 1;
}

static int png_dimensions(const uint8_t *png, size_t length, unsigned *width, unsigned *height) {
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (length < 24 || memcmp(png, signature, 8) || memcmp(png + 12, "IHDR", 4))
        return -1;
    uint32_t w = ((uint32_t)png[16] << 24) | ((uint32_t)png[17] << 16) |
                 ((uint32_t)png[18] << 8) | png[19];
    uint32_t h = ((uint32_t)png[20] << 24) | ((uint32_t)png[21] << 16) |
                 ((uint32_t)png[22] << 8) | png[23];
    *width = w > 0xFFFF ? 0 : w;
    *height = h > 0xFFFF ? 0 : h;
    return 0;
}

static void timestamp(char *out, size_t cap) {
    struct tm local;
    time_t now = time(NULL);
    localtime_r(&now, &local);
    strftime(out, cap, "%Y-%m-%dT%H:%M:%S%z", &local);
}

/* The sidecar: the same data as the index entry, minus the schema. */
static int write_sidecar(const char *dir, const logo_output *logo, const char *system,
                         const char *now) {
    char name[256] = "";
    char escaped[1024] = "";
    char sidecar[SIDECAR_MAX];
    unsigned width = 0, height = 0;
    char path[PATH_MAX];

    png_dimensions(logo->png, logo->png_length, &width, &height);
    if (logo->name) {
        snprintf(name, sizeof(name), "%s", logo->name);
        json_escape(escaped, sizeof(escaped), name);
    }
    int length = snprintf(sidecar, sizeof(sidecar),
                          "{\"id\": %u, \"network_id\": %u, \"transport_stream_id\": %u, "
                          "\"service_id\": %u, \"name\": \"%s\", \"file\": \"%u.png\", "
                          "\"bytes\": %zu, \"width\": %u, \"height\": %u, \"logo_type\": %u, "
                          "\"logo_id\": %u, \"system\": \"%s\", \"collected_at\": \"%s\"}\n",
                          logo->id, logo->network_id, logo->transport_stream_id,
                          logo->service_id, logo->name ? escaped : "", logo->id,
                          logo->png_length, width, height, logo->logo_type, logo->logo_id,
                          system, now);
    if (length < 0 || (size_t)length >= sizeof(sidecar)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (snprintf(path, sizeof(path), "%s/%u.json", dir, logo->id) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return write_changed(path, sidecar, (size_t)length);
}

static int compare_ids(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* index.json は <id>.json を ID 順に並べたもの。 */
static int write_index(const char *dir, const char *system, unsigned long if_hz,
                       unsigned long tsid) {
    static uint32_t ids[INDEX_ENTRIES];
    size_t count = 0;
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
            continue;
        size_t length = strlen(entry->d_name);
        if (length < 6 || strcmp(entry->d_name + length - 5, ".json") != 0)
            continue;
        if (count >= INDEX_ENTRIES)
            break;
        ids[count++] = (uint32_t)strtoul(entry->d_name, NULL, 10);
    }
    closedir(d);
    qsort(ids, count, sizeof(ids[0]), compare_ids);

    char *index = malloc(INDEX_MAX);
    if (!index)
        return -1;
    char now[64];
    timestamp(now, sizeof(now));
    size_t at = 0;
    at += (size_t)snprintf(index + at, INDEX_MAX - at,
                           "{\n  \"schema\": \"pix-dt295-logo/1\",\n"
                           "  \"updated_at\": \"%s\",\n"
                           "  \"source\": {\"system\": \"%s\", \"if_hz\": %lu, \"tsid\": %lu},\n"
                           "  \"logos\": [",
                           now, system, if_hz, tsid);
    size_t written_entries = 0;
    for (size_t i = 0; i < count && at + 8192 < INDEX_MAX; i++) {
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%u.json", dir, ids[i]) >= (int)sizeof(path))
            continue;
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        char body[SIDECAR_MAX];
        size_t length = fread(body, 1, sizeof(body) - 1, f);
        fclose(f);
        if (!length)
            continue;
        body[length] = '\0';
        char *newline = strchr(body, '\n');
        if (newline)
            *newline = '\0';
        length = strlen(body);
        if (at + length + 16 >= INDEX_MAX)
            break;
        if (written_entries++)
            index[at++] = ',';
        memcpy(index + at, "\n    ", 5);
        at += 5;
        memcpy(index + at, body, length);
        at += length;
        index[at] = '\0';
    }
    at += (size_t)snprintf(index + at, INDEX_MAX - at, "\n  ]\n}\n");
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/index.json", dir) >= (int)sizeof(path)) {
        free(index);
        errno = ENAMETOOLONG;
        return -1;
    }
    int changed = write_changed(path, index, at);
    free(index);
    return changed;
}

int logo_store_write(const char *dir, const logo_output *logos, size_t count, const char *system,
                     unsigned long if_hz, unsigned long tsid) {
    if (!dir) {
        errno = EINVAL;
        return -1;
    }
    if (logo_store_mkdirs(dir))
        return -1;
    char now[64];
    timestamp(now, sizeof(now));
    int changed = 0;
    for (size_t i = 0; i < count; i++) {
        const logo_output *logo = &logos[i];
        if (!logo->png || !logo->png_length)
            continue;
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%u.png", dir, logo->id) >= (int)sizeof(path))
            continue;
        int png_changed = write_changed(path, logo->png, logo->png_length);
        if (png_changed < 0)
            return -1;
        int sidecar_changed = write_sidecar(dir, logo, system, now);
        if (sidecar_changed < 0)
            return -1;
        changed += png_changed + sidecar_changed;
    }
    int index_changed = write_index(dir, system, if_hz, tsid);
    if (index_changed < 0)
        return -1;
    return changed + index_changed;
}
