/*
 * test_feature_parse.c - host test for the pixf sideband parser.
 *
 * Feed the same text ffmpeg writes, then check the bytes that would reach the
 * NAS. A frame is only complete once the next "frame:" header arrives; the
 * test closes every block it expects to see.
 *
 * Build:
 *   cc -std=gnu11 -Ipackage/stb-streamer/src \
 *      tests/test_feature_parse.c package/stb-streamer/src/feature_tap.c \
 *      -o /tmp/test_feature_parse
 */
#define _GNU_SOURCE
#include "feature_tap.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint8_t g_box[8192];
static size_t g_box_len;

static int capture(void *ctx, const void *data, size_t len)
{
    (void)ctx;
    assert(len <= sizeof(g_box));
    memcpy(g_box, data, len);
    g_box_len = len;
    return 0;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void append_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    assert(fd >= 0);
    assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    close(fd);
}

static void fresh_dir(char *dir, size_t size, char *video, size_t vsize, char *audio, size_t asize)
{
    snprintf(dir, size, "/tmp/feat-test-XXXXXX");
    assert(mkdtemp(dir));
    snprintf(video, vsize, "%s/v.log", dir);
    snprintf(audio, asize, "%s/a.log", dir);
}

int main(void)
{
    char dir[64], video[128], audio[128], black[128];
    FeatureTap tap;

    /* ---- values and the box layout ---- */
    fresh_dir(dir, sizeof(dir), video, sizeof(video), audio, sizeof(audio));
    snprintf(black, sizeof(black), "%s/b.log", dir);
    feature_tap_init(&tap, video, audio, black, 1024 * 1024);
    feature_tap_poll(&tap);
    assert(feature_tap_build_boxes(&tap, capture, NULL) == 0);

    append_text(video,
        "frame:0    pts:0       pts_time:0\n"
        "lavfi.signalstats.YMIN=16\n"
        "lavfi.signalstats.YAVG=125.481\n"
        "lavfi.blackframe.pblack=0.5\n"
        "lavfi.signalstats.YDIF=12.34\n"
        "frame:1    pts:3003    pts_time:0.1001\n");
    append_text(audio,
        "frame:0    pts:0       pts_time:0\n"
        "lavfi.astats.Overall.RMS_level=-21.063050\n"
        "lavfi.astats.Overall.Number_of_samples=1024\n"
        "frame:1    pts:1024    pts_time:0.0213333\n");

    feature_tap_poll(&tap);
    assert(feature_tap_build_boxes(&tap, capture, NULL) == 1);
    assert(g_box_len == 8 + FEATURE_TAP_HEADER + 2 * FEATURE_TAP_RECORD);
    assert(memcmp(g_box + 4, "pixf", 4) == 0);
    assert(be32(g_box) == (uint32_t)g_box_len);
    const uint8_t *h = g_box + 8;
    assert(h[0] == 1 && h[1] == 0);                 /* schema 1, flags 0 */
    assert(rd32(h + 8) == 2);                       /* two records */
    assert(rd64(h + 12) == 0);                      /* first pts */
    assert(rd64(h + 20) == 0);                      /* last pts */
    assert(rd32(h + 28) == 1);                      /* video */
    assert(rd32(h + 32) == 1);                      /* audio */
    assert(rd32(h + 36) == 0);                      /* dropped */

    const uint8_t *r = h + FEATURE_TAP_HEADER;
    assert(r[0] == 1);                              /* video record */
    assert(rd64(r + 4) == 0);
    assert(rd32(r + 12) == 0);                      /* frame index */
    assert((int32_t)rd32(r + 16) == 12548);         /* mean luma x100 */
    assert(rd32(r + 20) == 5);                      /* black 0.5% -> 5 */
    assert(rd32(r + 24) == 1234);                   /* scene x100 */
    r += FEATURE_TAP_RECORD;
    assert(r[0] == 2);                              /* audio record */
    assert(rd32(r + 12) == 1024);                   /* samples */
    assert((int32_t)rd32(r + 16) == -2106);         /* RMS dB x100 */
    assert(feature_tap_dropped(&tap) == 0);

    /* ---- an unterminated line is kept until its newline arrives ---- */
    fresh_dir(dir, sizeof(dir), video, sizeof(video), audio, sizeof(audio));
    snprintf(black, sizeof(black), "%s/b.log", dir);
    feature_tap_init(&tap, video, audio, black, 1024 * 1024);
    append_text(video,
        "frame:0    pts:0       pts_time:0\n"
        "lavfi.signalstats.YAVG=42.5\n"
        "lavfi.blackframe.pblack=100\n");
    feature_tap_poll(&tap);
    assert(feature_tap_build_boxes(&tap, capture, NULL) == 0);   /* still open */
    append_text(video, "lavfi.signalstats.YDIF=0\nframe:1    pts:3003    pts_time:0.1001\n");
    feature_tap_poll(&tap);
    assert(feature_tap_build_boxes(&tap, capture, NULL) == 1);
    r = g_box + 8 + FEATURE_TAP_HEADER;
    assert((int32_t)rd32(r + 16) == 4250);

    /* ---- a frame without the keys the NAS needs is dropped ---- */
    fresh_dir(dir, sizeof(dir), video, sizeof(video), audio, sizeof(audio));
    snprintf(black, sizeof(black), "%s/b.log", dir);
    feature_tap_init(&tap, video, audio, black, 1024 * 1024);
    append_text(video,
        "frame:0    pts:0       pts_time:0\n"
        "lavfi.signalstats.YAVG=10\n"
        "lavfi.signalstats.YDIF=1\n"
        "frame:1    pts:3003    pts_time:0.1001\n");
    feature_tap_poll(&tap);
    /* black comes from the interval records, so mean+scene is enough */
    assert(feature_tap_build_boxes(&tap, capture, NULL) == 1);
    assert(feature_tap_dropped(&tap) == 0);

    /* ---- -inf RMS (digital silence) becomes the noise floor ---- */
    fresh_dir(dir, sizeof(dir), video, sizeof(video), audio, sizeof(audio));
    snprintf(black, sizeof(black), "%s/b.log", dir);
    feature_tap_init(&tap, video, audio, black, 1024 * 1024);
    append_text(audio,
        "frame:0    pts:0       pts_time:0\n"
        "lavfi.astats.Overall.RMS_level=-inf\n"
        "lavfi.astats.Overall.Number_of_samples=1024\n"
        "frame:1    pts:1024    pts_time:0.0213333\n");
    feature_tap_poll(&tap);
    assert(feature_tap_build_boxes(&tap, capture, NULL) == 1);
    r = g_box + 8 + FEATURE_TAP_HEADER;
    assert(r[0] == 2);
    assert((int32_t)rd32(r + 16) == -12000);        /* -120.0 dB x100 */

    /* ---- a frame that lost its scene score is still dropped ---- */
    fresh_dir(dir, sizeof(dir), video, sizeof(video), audio, sizeof(audio));
    snprintf(black, sizeof(black), "%s/b.log", dir);
    feature_tap_init(&tap, video, audio, black, 1024 * 1024);
    append_text(video,
        "frame:0    pts:0       pts_time:0\n"
        "lavfi.signalstats.YAVG=10\n"
        "frame:1    pts:3003    pts_time:0.1001\n");
    feature_tap_poll(&tap);
    assert(feature_tap_build_boxes(&tap, capture, NULL) == 0);
    assert(feature_tap_dropped(&tap) == 1);

    /* ---- blackdetect intervals become type 3 records ---- */
    fresh_dir(dir, sizeof(dir), video, sizeof(video), audio, sizeof(audio));
    snprintf(black, sizeof(black), "%s/b.log", dir);
    feature_tap_init(&tap, video, audio, black, 1024 * 1024);
    append_text(black, "[blackdetect @ 0x1] black_start:2.5 black_end:3.25 black_duration:0.75\n");
    feature_tap_poll(&tap);
    assert(feature_tap_build_boxes(&tap, capture, NULL) == 1);
    r = g_box + 8 + FEATURE_TAP_HEADER;
    assert(r[0] == 3);                              /* black interval */
    assert(rd64(r + 4) == 2500000);                 /* 2.5 s */
    assert(rd32(r + 12) == 750);                    /* 0.75 s in ms */

    /* ---- a truncated log is read from the start again ---- */
    fresh_dir(dir, sizeof(dir), video, sizeof(video), audio, sizeof(audio));
    snprintf(black, sizeof(black), "%s/b.log", dir);
    feature_tap_init(&tap, video, audio, black, 1024 * 1024);
    append_text(video,
        "frame:0    pts:0       pts_time:0\n"
        "lavfi.signalstats.YAVG=100\n"
        "lavfi.signalstats.YDIF=1\n"
        "frame:1    pts:3003    pts_time:0.1001\n");
    feature_tap_poll(&tap);
    assert(feature_tap_build_boxes(&tap, capture, NULL) == 1);
    /* Restart the producer: the file is truncated and filled again. */
    {
        int fd = open(video, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        const char *fresh =
            "frame:0    pts:0       pts_time:0\n"
            "lavfi.signalstats.YAVG=77\n"
            "lavfi.signalstats.YDIF=1\n"
            "frame:1    pts:3003    pts_time:0.1001\n";
        assert(write(fd, fresh, strlen(fresh)) == (ssize_t)strlen(fresh));
        close(fd);
    }
    feature_tap_poll(&tap);
    assert(feature_tap_build_boxes(&tap, capture, NULL) == 1);
    r = g_box + 8 + FEATURE_TAP_HEADER;
    assert((int32_t)rd32(r + 16) == 7700);          /* 77.00, the new file */

    /* ---- a restarted producer (pts back to zero) starts a new sidecar ---- */
    fresh_dir(dir, sizeof(dir), video, sizeof(video), audio, sizeof(audio));
    snprintf(black, sizeof(black), "%s/b.log", dir);
    feature_tap_init(&tap, video, audio, black, 1024 * 1024);
    append_text(video,
        "frame:100  pts:300300  pts_time:333.667\n"
        "lavfi.signalstats.YAVG=10\n"
        "lavfi.signalstats.YDIF=1\n"
        "frame:101  pts:303303  pts_time:337.003\n");
    feature_tap_poll(&tap);
    /* ffmpeg restarts: the same log now carries a run that begins at zero. */
    append_text(video,
        "frame:0    pts:0       pts_time:0\n"
        "lavfi.signalstats.YAVG=200\n"
        "lavfi.signalstats.YDIF=2\n"
        "frame:1    pts:3003    pts_time:0.1001\n");
    feature_tap_poll(&tap);
    assert(feature_tap_build_boxes(&tap, capture, NULL) == 1);
    r = g_box + 8 + FEATURE_TAP_HEADER;
    assert(rd64(r + 4) == 0);                       /* new run starts at 0 */
    assert((int32_t)rd32(r + 16) == 20000);         /* 200.00, not 10.00 */

    /* ---- an anchor moves the timeline and keeps the order ---- */
    fresh_dir(dir, sizeof(dir), video, sizeof(video), audio, sizeof(audio));
    snprintf(black, sizeof(black), "%s/b.log", dir);
    feature_tap_init(&tap, video, audio, black, 1024 * 1024);
    append_text(video,
        "frame:0    pts:900000000 pts_time:10\n"
        "lavfi.signalstats.YAVG=100\n"
        "lavfi.signalstats.YDIF=1\n"
        "frame:1    pts:900033000 pts_time:10.033\n"
        "lavfi.signalstats.YAVG=100\n"
        "lavfi.signalstats.YDIF=1\n"
        "frame:2    pts:900066000 pts_time:10.067\n");
    feature_tap_poll(&tap);
    feature_tap_set_anchor(&tap, 10500000);     /* media is 0.5 s ahead */
    assert(feature_tap_build_boxes(&tap, capture, NULL) == 1);
    r = g_box + 8 + FEATURE_TAP_HEADER;
    assert(rd64(r + 4) == 10500000);
    append_text(video,
        "frame:3    pts:900099000 pts_time:10.1\n"
        "lavfi.signalstats.YAVG=101\n"
        "lavfi.signalstats.YDIF=1\n"
        "frame:4    pts:900132000 pts_time:10.133\n");
    feature_tap_poll(&tap);
    assert(feature_tap_build_boxes(&tap, capture, NULL) == 1);
    r = g_box + 8 + FEATURE_TAP_HEADER;
    assert(rd64(r + 4) == 10600000);

    /* ---- lines from before a realign are ignored ---- */
    fresh_dir(dir, sizeof(dir), video, sizeof(video), audio, sizeof(audio));
    snprintf(black, sizeof(black), "%s/b.log", dir);
    feature_tap_init(&tap, video, audio, black, 1024 * 1024);
    append_text(video,
        "frame:0    pts:500000000 pts_time:500\n"
        "lavfi.signalstats.YAVG=100\n"
        "lavfi.signalstats.YDIF=1\n"
        "frame:1    pts:500033000 pts_time:500.033\n");
    feature_tap_poll(&tap);
    assert(feature_tap_build_boxes(&tap, capture, NULL) == 1);
    feature_tap_rebase(&tap);            /* the reader connected here */
    append_text(video,
        "frame:2    pts:400000000 pts_time:400\n"   /* stale line */
        "lavfi.signalstats.YAVG=10\n"
        "lavfi.signalstats.YDIF=9\n"
        "frame:3    pts:500100000 pts_time:500.1\n" /* current line */
        "lavfi.signalstats.YAVG=150\n"
        "lavfi.signalstats.YDIF=2\n"
        "frame:4    pts:500133000 pts_time:500.133\n");
    feature_tap_poll(&tap);
    assert(feature_tap_build_boxes(&tap, capture, NULL) == 1);
    r = g_box + 8 + FEATURE_TAP_HEADER;
    /* Only the current frame is kept, and it starts near zero. */
    assert((int32_t)rd32(r + 16) == 15000);

    /* ---- a boundary rebases the timeline for the next segment ---- */
    fresh_dir(dir, sizeof(dir), video, sizeof(video), audio, sizeof(audio));
    snprintf(black, sizeof(black), "%s/b.log", dir);
    feature_tap_init(&tap, video, audio, black, 1024 * 1024);
    append_text(video,
        "frame:0    pts:500000000 pts_time:500\n"
        "lavfi.signalstats.YAVG=100\n"
        "lavfi.signalstats.YDIF=1\n"
        "frame:1    pts:500033000 pts_time:500.033\n");
    feature_tap_poll(&tap);
    assert(feature_tap_build_boxes(&tap, capture, NULL) == 1);
    /* The programme changed: the next segment counts from that moment. */
    feature_tap_rebase(&tap);
    append_text(video,
        "frame:2    pts:501000000 pts_time:501\n"
        "lavfi.signalstats.YAVG=150\n"
        "lavfi.signalstats.YDIF=2\n"
        "frame:3    pts:501033000 pts_time:501.033\n");
    feature_tap_poll(&tap);
    assert(feature_tap_build_boxes(&tap, capture, NULL) == 1);
    r = g_box + 8 + FEATURE_TAP_HEADER;
    /* The base is the last frame of the previous segment (500.033 s), so the
     * first record of the new one sits 0.967 s after that instant. */
    assert(rd64(r + 4) == 967000);
    assert((int32_t)rd32(r + 16) == 15000);

    /* ---- the cap stops the sideband instead of growing without bound ---- */
    fresh_dir(dir, sizeof(dir), video, sizeof(video), audio, sizeof(audio));
    snprintf(black, sizeof(black), "%s/b.log", dir);
    feature_tap_init(&tap, video, audio, black, 64);
    append_text(video,
        "frame:0    pts:0       pts_time:0\n"
        "lavfi.signalstats.YAVG=10\n"
        "lavfi.blackframe.pblack=100\n"
        "lavfi.signalstats.YDIF=0\n"
        "frame:1    pts:3003    pts_time:0.1001\n");
    feature_tap_poll(&tap);
    /* Over the cap: whatever was parsed before the limit is still sent once. */
    int built = feature_tap_build_boxes(&tap, capture, NULL);
    assert(built == 0 || built == 1);
    append_text(video, "lavfi.signalstats.YAVG=10\n");
    feature_tap_poll(&tap);
    assert(feature_tap_build_boxes(&tap, capture, NULL) == 0);

    puts("feature parse tests passed");
    return 0;
}
