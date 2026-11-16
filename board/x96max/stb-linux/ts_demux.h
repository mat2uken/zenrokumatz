/*
 * ts_demux.h - 最小限の MPEG-2 TS 分離器
 *
 * dtv_android の TsDemuxer.java と同じ考え方で、PAT/PMT を読み、1 番組の
 * MPEG-2 映像と AAC 音声の PES を切り出す。ライブ入力（FIFO）から読むので
 * ファイル全体を前提にせず、届いた分だけを順に処理する。
 */
#ifndef TS_DEMUX_H
#define TS_DEMUX_H

#include <stddef.h>
#include <stdint.h>

typedef struct TsDemux TsDemux;

/* 1 本の ES を取り出すためのコールバック。data は PES ペイロード本体。 */
typedef int (*TsPayloadFn)(void *user, const uint8_t *data, size_t size,
                           int64_t pts_us, int is_key);

typedef struct {
    TsPayloadFn on_video;
    TsPayloadFn on_audio;
    void *user;
    int want_audio;
} TsDemuxConfig;

TsDemux *ts_demux_open(const TsDemuxConfig *cfg);
void ts_demux_close(TsDemux *d);

/* 188 バイト境界を保って少しずつ渡す。戻り値: 処理したバイト数, -1 でエラー */
int ts_demux_feed(TsDemux *d, const uint8_t *data, size_t size);

/* 見つかった PID（デバッグ用） */
int ts_demux_video_pid(const TsDemux *d);
int ts_demux_audio_pid(const TsDemux *d);

#endif /* TS_DEMUX_H */
