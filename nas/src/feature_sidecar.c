#include "feature_sidecar.h"

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64le(const uint8_t *p)
{
    return (uint64_t)rd32le(p) | ((uint64_t)rd32le(p + 4) << 32);
}

int feature_batch_parse(const uint8_t *payload, size_t length, FeatureBatch *out)
{
    if (!payload || !out || length < FEATURE_SIDECAR_HEADER)
        return -1;
    FeatureBatch batch;
    batch.schema = (uint16_t)(payload[0] | ((uint16_t)payload[1] << 8));
    batch.flags = (uint16_t)(payload[2] | ((uint16_t)payload[3] << 8));
    batch.generation = rd32le(payload + 4);
    batch.count = rd32le(payload + 8);
    batch.first_pts_us = (int64_t)rd64le(payload + 12);
    batch.last_pts_us = (int64_t)rd64le(payload + 20);
    batch.video = rd32le(payload + 28);
    batch.audio = rd32le(payload + 32);
    batch.dropped = rd32le(payload + 36);
    if (batch.schema != FEATURE_SIDECAR_SCHEMA)
        return -1;
    if (batch.count == 0)
        return -1;
    size_t needed = FEATURE_SIDECAR_HEADER + (size_t)batch.count * FEATURE_SIDECAR_RECORD;
    if (needed > length)
        return -1;
    *out = batch;
    return (int)needed;
}
