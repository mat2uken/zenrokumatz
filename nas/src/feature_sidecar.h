#ifndef FEATURE_SIDECAR_H
#define FEATURE_SIDECAR_H

/*
 * Parsing of the pixf payload the capture side sends.
 *
 * The receiver uses it to validate a batch before writing it to the tmpfs
 * sidecar; the CM worker later turns the same bytes into rows. Keeping the
 * layout in one portable place lets the host tests check it without the NAS.
 */
#include <stddef.h>
#include <stdint.h>

#define FEATURE_SIDECAR_SCHEMA 1
#define FEATURE_SIDECAR_HEADER 44
#define FEATURE_SIDECAR_RECORD 28

typedef struct {
    uint16_t schema;
    uint16_t flags;
    uint32_t generation;
    uint32_t count;
    int64_t first_pts_us;
    int64_t last_pts_us;
    uint32_t video;
    uint32_t audio;
    uint32_t dropped;
} FeatureBatch;

/*
 * Returns the number of payload bytes this batch occupies (header plus
 * records), or -1 when the batch must not be used.
 */
int feature_batch_parse(const uint8_t *payload, size_t length, FeatureBatch *out);

#endif /* FEATURE_SIDECAR_H */
