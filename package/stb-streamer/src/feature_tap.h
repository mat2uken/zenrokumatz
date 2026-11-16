#ifndef FEATURE_TAP_H
#define FEATURE_TAP_H

/*
 * CM feature sideband.
 *
 * The first ffmpeg stage writes per-frame statistics to two text files on
 * tmpfs. This module turns complete lines into fixed size records and hands
 * them to the caller as "pixf" boxes. It never blocks the capture: missing
 * lines, unknown keys and over-long input are dropped and counted.
 */
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define FEATURE_TAP_SCHEMA 1
#define FEATURE_TAP_HEADER 44
#define FEATURE_TAP_RECORD 28
#define FEATURE_TAP_MAX_RECORDS 2000

typedef struct {
    uint8_t type;      /* 1 video, 2 audio */
    uint8_t flags;
    int64_t pts_us;
    uint32_t a;        /* video: frame index, audio: sample count */
    int32_t b;         /* video: mean luma x100, audio: RMS dB x100 */
    uint32_t c;        /* video: black fraction x1000 */
    uint32_t d;        /* video: scene difference x100 */
} FeatureRecord;

typedef struct {
    const char *video_path;
    const char *audio_path;
    /* blackdetect writes intervals to the ffmpeg log; we turn them into
     * type 3 records so the NAS can mark black frames without blackframe. */
    const char *black_path;
    size_t max_bytes;
    int fd_video;
    int fd_audio;
    int fd_black;
    off_t off_video;
    off_t off_audio;
    off_t off_black;
    char carry_video[1024];
    char carry_audio[1024];
    char carry_black[1024];
    size_t carry_len_video;
    size_t carry_len_audio;
    size_t carry_len_black;
    FeatureRecord pending_video;
    FeatureRecord pending_audio;
    int have_video;
    int have_audio;
    int got_mean;
    int got_scene;
    int got_black;
    int got_rms;
    int got_samples;
    uint32_t video_index;
    int64_t open_black_us;
    int have_open_black;
    /* ffmpeg restarts pts_time at zero; the tap follows the current run. */
    int pts_wrapped;
    /* Subtracted from every timestamp so a segment starts near zero. */
    int64_t pts_base;
    /*
     * Media time of the fragment the next batch belongs to. The tap times
     * frames on a fixed grid; the encoder drops frames, so that grid drifts
     * away from the file. Anchoring each batch to the fragment start keeps
     * the two timelines together.
     */
    int64_t anchor_us;
    int have_anchor;
    int64_t last_emitted_us;
    /* Size of the last applied anchor shift, for the diagnostic log. */
    int64_t last_shift_us;
    int64_t last_shift_report_us;
    int64_t last_seen_pts;
    /* Raw timestamps below this belong to the previous run and are ignored. */
    int64_t drop_below;
    int64_t last_video_pts;
    int64_t last_audio_pts;
    FeatureRecord *records;
    size_t count;
    size_t capacity;
    uint32_t generation;
    uint32_t dropped;
    int truncated;
    int started;
} FeatureTap;

/* Appends one complete box; returns 0 on success. */
typedef int (*FeatureAppend)(void *ctx, const void *data, size_t len);

void feature_tap_init(FeatureTap *tap, const char *video_path,
                      const char *audio_path, const char *black_path,
                      size_t max_bytes);

/* Reads whatever arrived since the last call. Never blocks. */
void feature_tap_poll(FeatureTap *tap);

/*
 * Serialises the records collected so far into one or more pixf boxes and
 * clears them. Returns 1 when boxes were written, 0 when there was nothing.
 */
int feature_tap_build_boxes(FeatureTap *tap, FeatureAppend append, void *ctx);

/* Drops collected records without writing them. */
void feature_tap_discard(FeatureTap *tap);
void feature_tap_rebase(FeatureTap *tap);

/* Anchors the next batch to the media time of the fragment it precedes. */
void feature_tap_set_anchor(FeatureTap *tap, int64_t anchor_us);

/* Records waiting to be sent. An anchor only makes sense when there are. */
size_t feature_tap_pending(const FeatureTap *tap);

/* Number of records or lines the tap could not use. */
uint32_t feature_tap_dropped(const FeatureTap *tap);

#endif /* FEATURE_TAP_H */
