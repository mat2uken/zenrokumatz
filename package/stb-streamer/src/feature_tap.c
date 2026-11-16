/*
 * feature_tap.c - turns ffmpeg metadata lines into pixf sideband boxes.
 *
 * The producer side is the first ffmpeg stage:
 *
 *   -vf "...,signalstats,blackframe=amount=0:threshold=32,scdet=threshold=10,
 *        metadata=mode=print:file=/dev/shm/pix/feat-v.log"
 *
 * and an audio-only null output using astats/ametadata. The text files are an
 * IPC channel on tmpfs; nothing here writes to flash.
 */
#define _GNU_SOURCE
#include "feature_tap.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    uint8_t *data;
    size_t size;
    size_t cap;
} MemBuf;

static int mem_append(MemBuf *b, const void *src, size_t n)
{
    if (b->size + n > b->cap) {
        size_t next = b->cap ? b->cap : 4096;
        while (next < b->size + n)
            next *= 2;
        uint8_t *grown = realloc(b->data, next);
        if (!grown)
            return -1;
        b->data = grown;
        b->cap = next;
    }
    memcpy(b->data + b->size, src, n);
    b->size += n;
    return 0;
}

static void put_u16(MemBuf *b, uint16_t v)
{
    uint8_t raw[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    mem_append(b, raw, sizeof(raw));
}

static void put_u32(MemBuf *b, uint32_t v)
{
    uint8_t raw[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    mem_append(b, raw, sizeof(raw));
}

static void put_u64(MemBuf *b, uint64_t v)
{
    uint8_t raw[8];
    for (int i = 0; i < 8; i++)
        raw[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    mem_append(b, raw, sizeof(raw));
}

void feature_tap_init(FeatureTap *tap, const char *video_path,
                      const char *audio_path, const char *black_path,
                      size_t max_bytes)
{
    memset(tap, 0, sizeof(*tap));
    tap->video_path = video_path;
    tap->audio_path = audio_path;
    tap->black_path = black_path;
    tap->max_bytes = max_bytes;
    tap->fd_video = -1;
    tap->fd_audio = -1;
    tap->fd_black = -1;
}

uint32_t feature_tap_dropped(const FeatureTap *tap)
{
    return tap->dropped;
}

static int64_t anchor_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

size_t feature_tap_pending(const FeatureTap *tap)
{
    return tap->count;
}

void feature_tap_set_anchor(FeatureTap *tap, int64_t anchor_us)
{
    tap->anchor_us = anchor_us;
    tap->have_anchor = 1;
}

/* Starts a new segment: drop what is buffered and rebase the timeline. */
void feature_tap_rebase(FeatureTap *tap)
{
    feature_tap_discard(tap);
    /*
     * The log still holds lines from before this point. They belong to media
     * that has already been sent, so drop them instead of letting them drag
     * the timeline backwards.
     */
    tap->drop_below = tap->last_seen_pts;
    tap->pts_base += tap->last_seen_pts;
    tap->last_seen_pts = 0;
}

void feature_tap_discard(FeatureTap *tap)
{
    tap->count = 0;
    tap->have_open_black = 0;
    tap->have_video = 0;
    tap->have_audio = 0;
    tap->got_mean = tap->got_scene = tap->got_black = 0;
    tap->got_rms = tap->got_samples = 0;
}

/* Reads a double after "<key>=" for a whole line; 1 on success. */
static int tap_value(const char *line, const char *key, double *out)
{
    size_t len = strlen(key);
    if (strncmp(line, key, len) != 0 || line[len] != '=')
        return 0;
    const char *text = line + len + 1;
    /* ffmpeg prints "-inf" for digital silence. strtod understands it, but the
     * value cannot be scaled to a fixed point record; clamp it to the floor. */
    if (!strncmp(text, "-inf", 4) || !strncmp(text, "-INF", 4) || !strncmp(text, "-Inf", 4)) {
        *out = -120.0;
        return 1;
    }
    if (!strncmp(text, "inf", 3) || !strncmp(text, "INF", 3) || !strncmp(text, "Inf", 3)) {
        *out = 0.0;
        return 1;
    }
    char *end = NULL;
    double value = strtod(text, &end);
    if (end == text)
        return 0;
    *out = value;
    return 1;
}

/* pts_time of a "frame:N pts:P pts_time:S" header line, in microseconds. */
static int tap_pts_us(const char *line, int64_t *out)
{
    const char *p = strstr(line, "pts_time:");
    if (!p)
        return 0;
    char *end = NULL;
    double seconds = strtod(p + 9, &end);
    if (end == p + 9)
        return 0;
    *out = (int64_t)(seconds * 1000000.0 + (seconds >= 0 ? 0.5 : -0.5));
    return 1;
}

/* A new ffmpeg run started: forget the previous run's records and carry. */
static void tap_reset_for_wrap(FeatureTap *tap)
{
    tap->count = 0;
    tap->have_video = 0;
    tap->have_audio = 0;
    tap->got_mean = tap->got_scene = tap->got_black = 0;
    tap->got_rms = tap->got_samples = 0;
    tap->carry_len_video = 0;
    tap->carry_len_audio = 0;
    tap->have_open_black = 0;
    tap->generation = (uint32_t)time(NULL);
}

static void tap_push(FeatureTap *tap, const FeatureRecord *record)
{
    if (tap->count == tap->capacity) {
        size_t next = tap->capacity ? tap->capacity * 2 : 256;
        FeatureRecord *grown = realloc(tap->records, next * sizeof(*grown));
        if (!grown) {
            tap->dropped++;
            return;
        }
        tap->records = grown;
        tap->capacity = next;
    }
    tap->records[tap->count++] = *record;
}

static void tap_finish(FeatureTap *tap, int audio)
{
    if (audio) {
        if (!tap->have_audio)
            return;
        tap->pending_audio.type = 2;
        tap->pending_audio.c = 0;
        tap->pending_audio.d = 0;
        if (tap->got_samples)
            tap_push(tap, &tap->pending_audio);
        else
            tap->dropped++;
        tap->have_audio = 0;
        tap->got_rms = tap->got_samples = 0;
    } else {
        if (!tap->have_video)
            return;
        tap->pending_video.type = 1;
        tap->pending_video.a = tap->video_index++;
        /*
         * Mean luma and the scene score are per frame. Black is not: the
         * frame-level filter (blackframe) is missing from this ffmpeg build,
         * so black intervals travel as type 3 records instead and the NAS
         * marks the frames inside them.
         */
        if (tap->got_mean && tap->got_scene)
            tap_push(tap, &tap->pending_video);
        else
            tap->dropped++;
        tap->have_video = 0;
        tap->got_mean = tap->got_scene = tap->got_black = 0;
    }
}

/* blackdetect interval line: "black_start:1.2 black_end:3.4 ...". */
static void tap_black_line(FeatureTap *tap, const char *line)
{
    const char *start = strstr(line, "black_start:");
    const char *end = strstr(line, "black_end:");
    if (start) {
        tap->open_black_us = (int64_t)(strtod(start + 12, NULL) * 1000000.0);
        tap->have_open_black = 1;
    }
    if (end && tap->have_open_black) {
        int64_t stop = (int64_t)(strtod(end + 10, NULL) * 1000000.0);
        int64_t length = stop - tap->open_black_us;
        if (length > 0) {
            FeatureRecord record;
            memset(&record, 0, sizeof(record));
            record.type = 3;
            record.pts_us = tap->open_black_us - tap->pts_base;
            record.a = (uint32_t)(length / 1000);
            tap_push(tap, &record);
        }
        tap->have_open_black = 0;
    }
}

static void tap_reset_for_wrap(FeatureTap *tap);

static void tap_line(FeatureTap *tap, int audio, char *line)
{
    int64_t pts = 0;
    double value = 0;
    /* ffmpeg prefixes the filter's log lines with "[Parsed_metadata_N @ ...] ". */
    if (line[0] == '[') {
        char *close = strchr(line, ']');
        if (close && close[1] == ' ')
            line = close + 2;
    }
    if (!strncmp(line, "frame:", 6)) {
        /* A restart of ffmpeg restarts pts_time at zero. */
        int64_t probe = 0;
        if (tap_pts_us(line, &probe)) {
            if ((audio ? tap->last_audio_pts : tap->last_video_pts) > probe + 1000000)
                tap_reset_for_wrap(tap);
            if (audio) tap->last_audio_pts = probe; else tap->last_video_pts = probe;
        }
        /*
         * Several print filters may each emit their own header for the same
         * frame (one key per filter). A header that repeats the open frame is
         * a continuation, not the next frame.
         */
        int continuing = 0;
        if (tap_pts_us(line, &pts)) {
            if (audio && tap->have_audio &&
                tap->pending_audio.pts_us == pts - tap->pts_base)
                continuing = 1;
            else if (!audio && tap->have_video &&
                     tap->pending_video.pts_us == pts - tap->pts_base)
                continuing = 1;
        }
        if (continuing)
            return;
        tap_finish(tap, audio);
        if (!tap_pts_us(line, &pts)) {
            tap->dropped++;
            return;
        }
        if (tap->drop_below > 0 && pts < tap->drop_below) {
            /* Before the realignment point: not part of this sidecar. */
            if (audio)
                tap->have_audio = 0;
            else
                tap->have_video = 0;
            return;
        }
        tap->last_seen_pts = pts;
        if (audio) {
            memset(&tap->pending_audio, 0, sizeof(tap->pending_audio));
            tap->pending_audio.pts_us = pts - tap->pts_base;
            tap->have_audio = 1;
        } else {
            memset(&tap->pending_video, 0, sizeof(tap->pending_video));
            tap->pending_video.pts_us = pts - tap->pts_base;
            tap->have_video = 1;
        }
        return;
    }
    if (!audio) {
        if (!tap->have_video)
            return;
        if (tap_value(line, "lavfi.signalstats.YAVG", &value)) {
            tap->pending_video.b = (int32_t)(value * 100.0 + 0.5);
            tap->got_mean = 1;
        } else if (tap_value(line, "lavfi.signalstats.YDIF", &value)) {
            tap->pending_video.d = (uint32_t)(value * 100.0 + 0.5);
            tap->got_scene = 1;
        } else if (tap_value(line, "lavfi.blackframe.pblack", &value)) {
            /* pblack is a percentage of the frame; the NAS wants 0..1000. */
            tap->pending_video.c = (uint32_t)(value * 10.0 + 0.5);
            tap->got_black = 1;
        }
    } else {
        if (!tap->have_audio)
            return;
        if (tap_value(line, "lavfi.astats.Overall.RMS_level", &value) ||
            tap_value(line, "lavfi.astats.1.RMS_level", &value)) {
            tap->pending_audio.b = (int32_t)(value * 100.0 + (value >= 0 ? 0.5 : -0.5));
            tap->got_rms = 1;
        } else if (tap_value(line, "lavfi.astats.Overall.Number_of_samples", &value)) {
            tap->pending_audio.a = (uint32_t)(value + 0.5);
            tap->got_samples = 1;
        }
    }
}

/* Splits buffered text on newlines; the unterminated tail stays in carry. */
static void tap_feed_black(FeatureTap *tap, const char *data, size_t len)
{
    char *carry = tap->carry_black;
    size_t *carry_len = &tap->carry_len_black;
    size_t used = *carry_len;
    while (len > 0) {
        size_t space = sizeof(tap->carry_black) - used - 1;
        size_t take = len < space ? len : space;
        if (take == 0) {
            used = 0;
            break;
        }
        memcpy(carry + used, data, take);
        used += take;
        data += take;
        len -= take;
        char *line = carry;
        char *nl;
        while ((nl = memchr(line, '\n', used - (size_t)(line - carry))) != NULL) {
            *nl = '\0';
            if (nl > line && nl[-1] == '\r')
                nl[-1] = '\0';
            tap_black_line(tap, line);
            line = nl + 1;
        }
        size_t rest = used - (size_t)(line - carry);
        if (rest && line != carry)
            memmove(carry, line, rest);
        used = rest;
    }
    *carry_len = used;
}

static void tap_feed(FeatureTap *tap, int audio, const char *data, size_t len)
{
    char *carry = audio ? tap->carry_audio : tap->carry_video;
    size_t *carry_len = audio ? &tap->carry_len_audio : &tap->carry_len_video;
    size_t used = *carry_len;
    while (len > 0) {
        size_t space = sizeof(tap->carry_video) - used - 1;
        size_t take = len < space ? len : space;
        if (take == 0) {
            /* A line longer than the buffer is not ours; drop it. */
            used = 0;
            tap->dropped++;
            break;
        }
        memcpy(carry + used, data, take);
        used += take;
        data += take;
        len -= take;
        char *line = carry;
        char *nl;
        while ((nl = memchr(line, '\n', used - (size_t)(line - carry))) != NULL) {
            *nl = '\0';
            if (nl > line && nl[-1] == '\r')
                nl[-1] = '\0';
            tap_line(tap, audio, line);
            line = nl + 1;
        }
        size_t rest = used - (size_t)(line - carry);
        if (rest && line != carry)
            memmove(carry, line, rest);
        used = rest;
    }
    *carry_len = used;
}

static void tap_poll_file(FeatureTap *tap, int audio)
{
    int *fdp = audio ? &tap->fd_audio : &tap->fd_video;
    off_t *offp = audio ? &tap->off_audio : &tap->off_video;
    const char *path = audio ? tap->audio_path : tap->video_path;
    if (!path || !path[0] || tap->truncated)
        return;
    if (*fdp < 0) {
        *fdp = open(path, O_RDONLY);
        if (*fdp < 0)
            return;
        /*
         * The pipeline truncates this log when it restarts. A new fd means a
         * new file, so read from the beginning: keeping the old offset would
         * start the sidecar part-way into the recording and shift every
         * timestamp against the media.
         */
        *offp = 0;
        if (audio) {
            tap->carry_len_audio = 0;
            tap->have_audio = 0;
        } else {
            tap->carry_len_video = 0;
            tap->have_video = 0;
        }
    }
    /*
     * The pipeline reopens the log with O_TRUNC on every restart, but the file
     * can also keep growing across a restart when the old reader still holds
     * it. Treat a timestamp that jumps back to zero as a new recording: reset
     * the offset, the carry and the record timeline.
     */
    struct stat st;
    if (fstat(*fdp, &st) == 0 && ((off_t)st.st_size < *offp || tap->pts_wrapped)) {
        *offp = 0;
        if (audio) {
            tap->carry_len_audio = 0;
            tap->have_audio = 0;
        } else {
            tap->carry_len_video = 0;
            tap->have_video = 0;
        }
    }
    char buf[32768];
    for (;;) {
        ssize_t n = pread(*fdp, buf, sizeof(buf), *offp);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        if (n == 0)
            return;
        *offp += n;
        if (tap->max_bytes && (size_t)*offp > tap->max_bytes) {
            tap->truncated = 1;
            fprintf(stderr, "feature_tap: feature file over the cap; "
                            "the sideband stops here\n");
            return;
        }
        tap_feed(tap, audio, buf, (size_t)n);
    }
}

static void tap_poll_black(FeatureTap *tap)
{
    const char *path = tap->black_path;
    if (!path || !path[0])
        return;
    if (tap->fd_black < 0) {
        tap->fd_black = open(path, O_RDONLY);
        if (tap->fd_black < 0)
            return;
    }
    char buf[16384];
    for (;;) {
        ssize_t n = pread(tap->fd_black, buf, sizeof(buf), tap->off_black);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        if (n == 0)
            return;
        tap->off_black += n;
        tap_feed_black(tap, buf, (size_t)n);
    }
}

void feature_tap_poll(FeatureTap *tap)
{
    if (!tap->started) {
        tap->started = 1;
        tap->generation = (uint32_t)time(NULL);
    }
    if (tap->max_bytes && (size_t)tap->off_video > tap->max_bytes)
        tap->truncated = 1;
    tap_poll_file(tap, 0);
    tap_poll_file(tap, 1);
    tap_poll_black(tap);
}

int feature_tap_build_boxes(FeatureTap *tap, FeatureAppend append, void *ctx)
{
    if (tap->count == 0)
        return 0;
    size_t done = 0;
    while (done < tap->count) {
        size_t take = tap->count - done;
        if (take > FEATURE_TAP_MAX_RECORDS)
            take = FEATURE_TAP_MAX_RECORDS;
        int64_t first = tap->records[done].pts_us;
        int64_t last = first;
        uint32_t video = 0;
        uint32_t audio = 0;
        for (size_t i = 0; i < take; i++) {
            const FeatureRecord *r = &tap->records[done + i];
            if (r->pts_us < first)
                first = r->pts_us;
            if (r->pts_us > last)
                last = r->pts_us;
            if (r->type == 1)
                video++;
            else
                audio++;
        }
        /*
         * Move the batch onto the media timeline when the caller knows where
         * the following fragment starts. A shift that would move the batch
         * backwards is refused: the parser requires each stream to advance.
         */
        if (tap->have_anchor && first != last) {
            int64_t shift = tap->anchor_us - first;
            int64_t shifted_first = first + shift;
            /*
             * Correct the drift by moving the whole timeline, not just this
             * batch: pts_base is what every later record is measured against,
             * so changing it keeps the order intact. A large shift means the
             * log is lagging the encoder; those batches keep their own time
             * rather than jump ahead of the records that follow.
             */
            if (shift > -3000000 && shift < 3000000 &&
                shifted_first >= tap->last_emitted_us) {
                tap->pts_base -= shift;
                first += shift;
                last += shift;
                for (size_t i = 0; i < take; i++)
                    tap->records[done + i].pts_us += shift;
                /*
                 * A frame still being collected keeps the timestamp it was
                 * given under the old base. Move it too, or it lands behind
                 * what has just been sent.
                 */
                if (tap->have_video)
                    tap->pending_video.pts_us += shift;
                if (tap->have_audio)
                    tap->pending_audio.pts_us += shift;
                if (tap->have_open_black)
                    tap->open_black_us += shift;
                tap->last_seen_pts -= shift;
                tap->last_shift_us = shift;
                int64_t since = anchor_now_us() - tap->last_shift_report_us;
                /* The first application is always reported: it proves the
                 * caller supplied a usable fragment time. */
                if (tap->last_shift_report_us == 0 ||
                    ((shift > 200000 || shift < -200000) && since > 60 * 1000000)) {
                    fprintf(stderr, "feature_tap: anchor shift %+.3f s\n",
                            (double)shift / 1e6);
                    tap->last_shift_report_us = anchor_now_us();
                }
            }
            tap->have_anchor = 0;
        }
        MemBuf box = {0};
        /* ISO BMFF box headers are big-endian even though the payload is not. */
        uint32_t size = (uint32_t)(8 + FEATURE_TAP_HEADER + take * FEATURE_TAP_RECORD);
        uint8_t head[8] = {
            (uint8_t)(size >> 24), (uint8_t)(size >> 16),
            (uint8_t)(size >> 8), (uint8_t)size,
            'p', 'i', 'x', 'f'
        };
        mem_append(&box, head, sizeof(head));
        put_u16(&box, FEATURE_TAP_SCHEMA);
        put_u16(&box, (uint16_t)(tap->truncated ? 1 : 0));
        put_u32(&box, tap->generation);
        put_u32(&box, (uint32_t)take);
        put_u64(&box, (uint64_t)first);
        put_u64(&box, (uint64_t)last);
        put_u32(&box, video);
        put_u32(&box, audio);
        put_u32(&box, tap->dropped);
        put_u32(&box, 0);
        for (size_t i = 0; i < take; i++) {
            const FeatureRecord *r = &tap->records[done + i];
            uint8_t head[4] = { r->type, r->flags, 0, 0 };
            mem_append(&box, head, sizeof(head));
            put_u64(&box, (uint64_t)r->pts_us);
            put_u32(&box, r->a);
            put_u32(&box, (uint32_t)r->b);
            put_u32(&box, r->c);
            put_u32(&box, r->d);
        }
        if (box.size != (size_t)(8 + FEATURE_TAP_HEADER + take * FEATURE_TAP_RECORD)) {
            free(box.data);
            tap->dropped += (uint32_t)take;
            return -1;
        }
        if (last > tap->last_emitted_us)
            tap->last_emitted_us = last;
        int rc = append(ctx, box.data, box.size);
        free(box.data);
        if (rc != 0) {
            tap->dropped += (uint32_t)take;
            tap->count = 0;
            return -1;
        }
        done += take;
    }
    tap->count = 0;
    return 1;
}
