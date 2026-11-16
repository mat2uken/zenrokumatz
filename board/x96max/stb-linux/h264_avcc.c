/*
 * h264_avcc.c - Annex-B から AVCC への変換
 */
#include "h264_avcc.h"

#include <string.h>

/* 次の開始コードを探す。戻り値は NAL 先頭、見つからなければ size */
static size_t find_start_code(const uint8_t *data, size_t from, size_t size,
                              size_t *sc_len)
{
    for (size_t i = from; i + 3 <= size; i++) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                *sc_len = 3;
                return i + 3;
            }
            if (i + 4 <= size && data[i + 2] == 0 && data[i + 3] == 1) {
                *sc_len = 4;
                return i + 4;
            }
        }
    }
    return size;
}

void h264_scan_param_sets(H264ParamSets *ps, const uint8_t *data, size_t size)
{
    size_t off = 0;
    while (off < size) {
        size_t sc_len = 0;
        size_t nal = find_start_code(data, off, size, &sc_len);
        if (nal >= size)
            break;
        size_t next_sc_len = 0;
        size_t next = find_start_code(data, nal, size, &next_sc_len);
        size_t nal_len = (next >= size ? size : next - next_sc_len) - nal;

        int type = data[nal] & 0x1f;
        if (type == 7 && !ps->have_sps && nal_len <= sizeof(ps->sps)) {
            memcpy(ps->sps, data + nal, nal_len);
            ps->sps_len = nal_len;
            ps->have_sps = 1;
        } else if (type == 8 && !ps->have_pps && nal_len <= sizeof(ps->pps)) {
            memcpy(ps->pps, data + nal, nal_len);
            ps->pps_len = nal_len;
            ps->have_pps = 1;
        }
        off = nal;
    }
}

int h264_is_keyframe(const uint8_t *data, size_t size)
{
    size_t off = 0;
    while (off < size) {
        size_t sc_len = 0;
        size_t nal = find_start_code(data, off, size, &sc_len);
        if (nal >= size)
            break;
        int type = data[nal] & 0x1f;
        if (type == 5)
            return 1;
        off = nal;
    }
    return 0;
}

int h264_annexb_to_avcc(const uint8_t *data, size_t size,
                        uint8_t *out, size_t cap)
{
    size_t written = 0;
    size_t off = 0;

    while (off < size) {
        size_t sc_len = 0;
        size_t nal = find_start_code(data, off, size, &sc_len);
        if (nal >= size)
            break;

        size_t next_sc_len = 0;
        size_t next = find_start_code(data, nal, size, &next_sc_len);
        size_t nal_end = (next >= size) ? size : next - next_sc_len;
        size_t nal_len = nal_end - nal;

        if (nal_len == 0) {
            off = nal;
            continue;
        }
        if (written + 4 + nal_len > cap)
            return -1;

        out[written + 0] = (uint8_t)(nal_len >> 24);
        out[written + 1] = (uint8_t)(nal_len >> 16);
        out[written + 2] = (uint8_t)(nal_len >> 8);
        out[written + 3] = (uint8_t)(nal_len);
        memcpy(out + written + 4, data + nal, nal_len);
        written += 4 + nal_len;

        off = nal;
    }
    return (int)written;
}
