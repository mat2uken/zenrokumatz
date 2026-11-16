#include "ram_spool.h"
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <stdio.h>

RamSpool *ram_spool_create(size_t max_bytes) {
    RamSpool *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->max_bytes = max_bytes ? max_bytes : (256 * 1024 * 1024); /* 既定 256MB */
    s->capacity = 64;
    s->fragments = calloc(s->capacity, sizeof(SpoolFragment));
    if (!s->fragments) { free(s); return NULL; }
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    return s;
}

void ram_spool_destroy(RamSpool *s) {
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    s->closed = 1;
    pthread_cond_broadcast(&s->cond);
    for (size_t i = 0; i < s->count; i++) {
        size_t idx = (s->head + i) % s->capacity;
        free(s->fragments[idx].data);
    }
    free(s->fragments);
    free(s->init_data);
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->cond);
    free(s);
}

int ram_spool_set_init(RamSpool *s, const void *data, size_t size) {
    if (!s || !data || !size) return -1;
    pthread_mutex_lock(&s->lock);
    free(s->init_data);
    s->init_data = malloc(size);
    if (!s->init_data) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    memcpy(s->init_data, data, size);
    s->init_size = size;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return 0;
}

static void drop_oldest_locked(RamSpool *s) {
    if (s->count == 0) return;
    SpoolFragment *f = &s->fragments[s->head];
    if (f->data) {
        s->used_bytes -= f->size;
        free(f->data);
        f->data = NULL;
    }
    s->head = (s->head + 1) % s->capacity;
    s->count--;
    s->total_dropped++;
}

int ram_spool_push(RamSpool *s, uint32_t seq, const void *data, size_t size) {
    if (!s || !data || !size) return -1;

    uint8_t *copy = malloc(size);
    if (!copy) return -1;
    memcpy(copy, data, size);

    pthread_mutex_lock(&s->lock);
    if (s->closed) {
        pthread_mutex_unlock(&s->lock);
        free(copy);
        return -1;
    }

    /* 容量上限を超える場合、古い断片から破棄 */
    while (s->count > 0 && (s->used_bytes + size > s->max_bytes)) {
        drop_oldest_locked(s);
    }

    /* 配列の拡張が必要な場合 */
    if (s->count == s->capacity) {
        size_t new_cap = s->capacity * 2;
        SpoolFragment *new_arr = calloc(new_cap, sizeof(SpoolFragment));
        if (!new_arr) {
            /* 拡張できなければ古いものを捨てる */
            drop_oldest_locked(s);
        } else {
            for (size_t i = 0; i < s->count; i++) {
                new_arr[i] = s->fragments[(s->head + i) % s->capacity];
            }
            free(s->fragments);
            s->fragments = new_arr;
            s->head = 0;
            s->capacity = new_cap;
        }
    }

    size_t tail = (s->head + s->count) % s->capacity;
    s->fragments[tail].seq = seq;
    s->fragments[tail].is_init = 0;
    s->fragments[tail].size = size;
    s->fragments[tail].data = copy;
    s->count++;
    s->used_bytes += size;
    s->total_pushed++;
    if (seq > s->max_pushed) s->max_pushed = seq;

    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return 0;
}

int ram_spool_get_after_timeout(RamSpool *s, uint32_t after_seq,
                                SpoolFragment *out, int timeout_ms) {
    if (!s || !out) return -1;

    struct timespec limit;
    clock_gettime(CLOCK_REALTIME, &limit);
    limit.tv_sec += timeout_ms / 1000;
    limit.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (limit.tv_nsec >= 1000000000L) {
        limit.tv_sec += 1;
        limit.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&s->lock);
    while (!s->closed) {
        for (size_t i = 0; i < s->count; i++) {
            size_t idx = (s->head + i) % s->capacity;
            SpoolFragment *f = &s->fragments[idx];
            if (f->seq > after_seq) {
                out->seq = f->seq;
                out->is_init = 0;
                out->size = f->size;
                out->data = malloc(f->size);
                if (!out->data) {
                    pthread_mutex_unlock(&s->lock);
                    return -1;
                }
                memcpy(out->data, f->data, f->size);
                pthread_mutex_unlock(&s->lock);
                return 1;
            }
        }
        /* Nothing new yet: wake up regularly so the caller can check the peer. */
        int rc = pthread_cond_timedwait(&s->cond, &s->lock, &limit);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&s->lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&s->lock);
    return -1;
}

int ram_spool_get_after(RamSpool *s, uint32_t after_seq, SpoolFragment *out) {
    if (!s || !out) return -1;

    pthread_mutex_lock(&s->lock);
    while (!s->closed) {
        /* after_seq より大きい最小の seq を探す */
        for (size_t i = 0; i < s->count; i++) {
            size_t idx = (s->head + i) % s->capacity;
            SpoolFragment *f = &s->fragments[idx];
            if (f->seq > after_seq) {
                out->seq = f->seq;
                out->is_init = 0;
                out->size = f->size;
                /* 呼び出し側で安全に送信できるようコピーを返す */
                out->data = malloc(f->size);
                if (!out->data) {
                    pthread_mutex_unlock(&s->lock);
                    return -1;
                }
                memcpy(out->data, f->data, f->size);
                pthread_mutex_unlock(&s->lock);
                return 1;
            }
        }
        /* まだ該当連番がない場合は待機 */
        pthread_cond_wait(&s->cond, &s->lock);
    }
    pthread_mutex_unlock(&s->lock);
    return -1; /* クローズ */
}

void ram_spool_drop_up_to(RamSpool *s, uint32_t seq) {
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    while (s->count > 0) {
        SpoolFragment *f = &s->fragments[s->head];
        if (f->seq > seq) break;
        s->used_bytes -= f->size;
        free(f->data);
        f->data = NULL;
        s->head = (s->head + 1) % s->capacity;
        s->count--;
    }
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

void ram_spool_clear(RamSpool *s) {
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    for (size_t i = 0; i < s->count; i++) {
        size_t idx = (s->head + i) % s->capacity;
        free(s->fragments[idx].data);
        s->fragments[idx].data = NULL;
    }
    s->head = 0;
    s->count = 0;
    s->used_bytes = 0;
    s->max_pushed = 0;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

uint32_t ram_spool_max_seq(RamSpool *s) {
    if (!s) return 0;
    pthread_mutex_lock(&s->lock);
    /* The highest sequence ever produced, not just the ones still held: a
     * reader asks to resume from that position even after old fragments were
     * released. */
    uint32_t max_seq = s->max_pushed;
    for (size_t i = 0; i < s->count; i++) {
        size_t idx = (s->head + i) % s->capacity;
        if (s->fragments[idx].seq > max_seq) max_seq = s->fragments[idx].seq;
    }
    pthread_mutex_unlock(&s->lock);
    return max_seq;
}

void ram_spool_stats(RamSpool *s, size_t *used_bytes, size_t *max_bytes,
                     uint64_t *pushed, uint64_t *dropped, size_t *frag_count) {
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    if (used_bytes) *used_bytes = s->used_bytes;
    if (max_bytes) *max_bytes = s->max_bytes;
    if (pushed) *pushed = s->total_pushed;
    if (dropped) *dropped = s->total_dropped;
    if (frag_count) *frag_count = s->count;
    pthread_mutex_unlock(&s->lock);
}
