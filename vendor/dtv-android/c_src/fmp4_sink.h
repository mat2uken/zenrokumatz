#ifndef FMP4_SINK_H
#define FMP4_SINK_H

#include <stddef.h>
#include <stdint.h>

typedef struct Fmp4Sink Fmp4Sink;

/**
 * Java プロセスから受け取ったHWエンコード済みサンプルを fMP4 断片へ変換し、
 * RAMスプールへ投入する受力口。
 *
 * 通信は 127.0.0.1 の loopback TCP で行い、eMMC への書き込みを発生させない。
 * loopback は端末内メモリ上だけで完結し、UNIX ソケットのような SELinux の
 * ファイル型制約を受けない。
 *
 * 受信プロトコル（固定長レコード、网络バイトオーダ）
 *
 *   [1 byte ] type        : 0=INIT, 1=VIDEO_SAMPLE, 2=AUDIO_SAMPLE, 3=FLUSH
 *   [1 byte ] flags       : bit0 = キーフレーム
 *   [2 bytes] reserved
 *   [8 bytes] pts_us      (int64)
 *   [4 bytes] duration_us (uint32)
 *   [4 bytes] data_size   (uint32)
 *   [N bytes ] data       : エンコーダー出力（Annex-B H.264/HEVC または AAC生）
 *
 * INIT の type=0 の场合、data_size=12 とし data 部長を次のように解釈する:
 *   [2 bytes] width, [2 bytes] height,
 *   [2 bytes] sps_len, [2 bytes] pps_len,
 *   [sps_len bytes] SPS, [pps_len bytes] PPS
 */

typedef struct {
    int port;                 /* loopback TCP ポート (127.0.0.1 のみ) */
    void *spool;              /* RamSpool* */
    size_t fragment_bytes;    /* これ以上積まったら断片をフラッシュ (0: 既定) */
} Fmp4SinkConfig;

Fmp4Sink *fmp4_sink_open(const Fmp4SinkConfig *cfg);

/**
 * 1回分だけ処理する。
 * 戻り値: 1 = データ受信, 0 = データなし, -1 = 切断, -2 = エラー
 */
int fmp4_sink_poll(Fmp4Sink *sink, int timeout_ms);

/**
 * Asks the sink to re-emit the current program marker in front of the next
 * fragment. Called when a new reader connects so it learns which program is on
 * air without waiting for the next real change.
 */
void fmp4_sink_request_boundary(Fmp4Sink *sink);

/**
 * Starts a new capture session: the fragment numbering and the program marker
 * are dropped so a reader can begin from sequence 0 on a new channel.
 */
void fmp4_sink_reset_session(Fmp4Sink *sink);

uint64_t fmp4_sink_frames(const Fmp4Sink *sink);
uint64_t fmp4_sink_fragments(const Fmp4Sink *sink);

void fmp4_sink_close(Fmp4Sink *sink);

#endif /* FMP4_SINK_H */
