#ifndef FMP4_WRITER_H
#define FMP4_WRITER_H

#include <stddef.h>
#include <stdint.h>

typedef struct Fmp4Writer Fmp4Writer;

/* コールバック型: 出力バイト列をストリームまたはバッファへ順次書き出す (追記のみ) */
typedef int (*Fmp4WriteFn)(void *user, const void *data, size_t size);

/* 作成・解放 */
Fmp4Writer *fmp4_writer_open(void *user, Fmp4WriteFn write_fn);
void fmp4_writer_close(Fmp4Writer *w);

/* コーデック設定 (init segment 出力前に呼ぶ) */
int fmp4_writer_set_video_h264(Fmp4Writer *w, int width, int height,
                               const void *sps, size_t sps_len,
                               const void *pps, size_t pps_len);

/* HEVC の初期化セグメント用設定 (HVCC はエンコーダーの csd-0) */
int fmp4_writer_set_video_hevc(Fmp4Writer *w, int width, int height,
                               const void *hvcc, size_t hvcc_len);

int fmp4_writer_set_audio_aac(Fmp4Writer *w, int sample_rate, int channels,
                              const void *asc, size_t asc_len);

/* 初期化セグメント (ftyp + moov[mvex]) を出力する (ファイル開始時に1度のみ) */
int fmp4_writer_write_init(Fmp4Writer *w);

int fmp4_writer_add_sample_avcc(Fmp4Writer *w, int is_audio,
                                const void *data, size_t size,
                                int64_t pts_us, int64_t dts_us,
                                uint32_t duration_us, int is_key);

/* 溜まったサンプルからメディア断片 (moof + mdat) を生成して出力し、断片バッファをクリアする */
int fmp4_writer_flush_fragment(Fmp4Writer *w, uint32_t seq);

/* 断片に溜まっているサンプルを捨てる (セッション切り替え時) */
void fmp4_writer_reset_samples(Fmp4Writer *w);

/* 現在の断片に溜まっているサンプル数 */
size_t fmp4_writer_fragment_samples(const Fmp4Writer *w);

#endif /* FMP4_WRITER_H */
