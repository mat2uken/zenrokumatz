#ifndef FMP4_TO_MP4_H
#define FMP4_TO_MP4_H

#include <stddef.h>
#include <stdint.h>

/**
 * tmpfs 上の fMP4 ファイルを走査し、先頭に moov を持つ通常 MP4 (faststart) へ再構成して
 * 出力先ファイル (最終保存先) へ直接書き出す。
 * 
 * 映像・音声の mdat ペイロードは再エンコードせずストリームコピーする。
 * 成功した場合、in_fmp4_path を unlink して tmpfs を即時解放する。
 *
 * @param in_fmp4_path   入力fMP4パス (例: /dev/shm/rec_staging/prog1.fmp4)
 * @param out_mp4_path   出力通常MP4パス (例: /mnt/xblk/video/pix-smb400/prog1.mp4)
 * @param remove_input   変換成功時に入力ファイルを unlink するか (1: 削除, 0: 残す)
 * @return 0: 成功, -1: 失敗
 */
int fmp4_to_mp4_finalize(const char *in_fmp4_path, const char *out_mp4_path, int remove_input);

/**
 * Same as fmp4_to_mp4_finalize, but also reports what the produced file holds.
 * out_duration_ms  : メディア実長 (ミリ秒)
 * out_samples      : 映像・音声のサンプル総数
 * out_video_samples: 映像サンプル数
 */
int fmp4_to_mp4_finalize_ex(const char *in_fmp4_path, const char *out_mp4_path,
                            int remove_input,
                            int64_t *out_duration_ms, int64_t *out_samples,
                            int64_t *out_video_samples);

#endif /* FMP4_TO_MP4_H */
