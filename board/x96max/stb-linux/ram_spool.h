#ifndef RAM_SPOOL_H
#define RAM_SPOOL_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

typedef struct {
    uint32_t seq;
    int is_init;        /* 初期化セグメント (ftyp+moov) の場合 1 */
    size_t size;
    uint8_t *data;
} SpoolFragment;

typedef struct {
    size_t max_bytes;     /* 最大許容メモリ (例: 200MB〜300MB) */
    size_t used_bytes;    /* 現在の蓄積バイト数 */
    uint64_t total_pushed;
    uint64_t total_dropped;
    uint32_t max_pushed;   /* これまでに発行した最大の断片連番 */

    /* キュー (循環リングバッファまたは動的配列) */
    SpoolFragment *fragments;
    size_t capacity;
    size_t head;
    size_t count;

    /* 初期化セグメントは破棄されないよう別枠保持 */
    uint8_t *init_data;
    size_t init_size;

    pthread_mutex_t lock;
    pthread_cond_t cond;
    int closed;
} RamSpool;

/* 作成・解放 */
RamSpool *ram_spool_create(size_t max_bytes);
void ram_spool_destroy(RamSpool *s);

/* 初期化セグメントの登録 (常に先頭配信用として固定保持) */
int ram_spool_set_init(RamSpool *s, const void *data, size_t size);

/* 新しい断片の投入 (上限超過時は最も古い断片を破棄) */
int ram_spool_push(RamSpool *s, uint32_t seq, const void *data, size_t size);

/* 指定連番 (after_seq) 以降の断片を取得するイテレータ関数 */
/* 戻り値: 1 取得成功 (data/size/seqにポインタ設定、コピーではないので要lockまたは即送出), 0 待機中/新データなし, -1 クローズ */
int ram_spool_get_after(RamSpool *s, uint32_t after_seq, SpoolFragment *out);

/* タイムアウト付き: 1=取得, 0=時間切れ, -1=クローズ */
int ram_spool_get_after_timeout(RamSpool *s, uint32_t after_seq,
                                SpoolFragment *out, int timeout_ms);

/* 断片をすべて捨てる (チャンネル切り替え時) */
void ram_spool_clear(RamSpool *s);

/** Drops fragments up to and including seq once no reader needs them. */
void ram_spool_drop_up_to(RamSpool *s, uint32_t seq);

/* スプール内の最大の断片連番 (0 なら空) */
uint32_t ram_spool_max_seq(RamSpool *s);

/* 統計情報 */
void ram_spool_stats(RamSpool *s, size_t *used_bytes, size_t *max_bytes,
                     uint64_t *pushed, uint64_t *dropped, size_t *frag_count);

#endif /* RAM_SPOOL_H */
