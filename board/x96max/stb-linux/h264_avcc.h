/*
 * h264_avcc.h - Annex-B から AVCC への変換
 *
 * meson の H.264 エンコーダー（/dev/video1）は SPS/PPS をインラインで持つ
 * Annex-B を出す。一方 fmp4_writer は AVCC（4 バイト長前置）をそのまま格納し、
 * SPS/PPS は別に受け取る。dtv_android では MediaCodec が AVCC を返していたので
 * この変換が無かったが、Linux では必要になる。
 */
#ifndef H264_AVCC_H
#define H264_AVCC_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t sps[256];
    size_t sps_len;
    uint8_t pps[256];
    size_t pps_len;
    int have_sps;
    int have_pps;
} H264ParamSets;

/* Annex-B を走査して SPS/PPS を取り出す（初回のみで良い） */
void h264_scan_param_sets(H264ParamSets *ps, const uint8_t *data, size_t size);

/*
 * Annex-B のアクセスユニットを AVCC へ変換する。
 * out は呼び出し側が用意したバッファ、cap は容量。
 * 戻り値は書き込んだバイト数、負値はエラー。
 */
int h264_annexb_to_avcc(const uint8_t *data, size_t size,
                        uint8_t *out, size_t cap);

/* アクセスユニットに IDR スライスが含まれるか */
int h264_is_keyframe(const uint8_t *data, size_t size);

#endif /* H264_AVCC_H */
