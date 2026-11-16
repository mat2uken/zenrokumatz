/*
 * v4l2_m2m.h - 最小限の V4L2 mem2mem ラッパー
 *
 * Android 版は MediaCodec がデコードとエンコードを担っていたが、Linux では
 * 同じ役割を V4L2 mem2mem デバイスが持つ。ここでは dtv_android の
 * LiveTsStreamer が Java 側で行っていたことを C で置き換えるために必要な
 * ioctl だけをまとめる。外部ライブラリは使わず、カーネルの UAPI だけを見る。
 */
#ifndef V4L2_M2M_H
#define V4L2_M2M_H

#include <stddef.h>
#include <stdint.h>

typedef struct V4l2M2m V4l2M2m;

/* 出力（圧縮側）と入力（生画像側）のバッファ */
typedef struct {
    void *addr;          /* データ先頭（mmap 結果にオフセットを足したもの） */
    void *map_base;      /* munmap に渡す元のポインタ */
    size_t map_len;
    size_t length;
    int index;
    size_t bytesused;
} V4l2Buffer;

typedef struct {
    const char *device;  /* /dev/videoN */
    uint32_t pixfmt_out; /* 出力側 (MPEG2 等) */
    uint32_t pixfmt_cap; /* 入力側 (NV12M 等) */
    int width;
    int height;
    int num_out;         /* 出力側バッファ数 */
    int num_cap;         /* 入力側バッファ数 */
} V4l2M2mConfig;

/* 開いてフォーマットを設定し、バッファを確保して STREAMON まで行う */
V4l2M2m *v4l2_m2m_open(const V4l2M2mConfig *cfg);
void v4l2_m2m_close(V4l2M2m *m);

/* 出力側バッファを1つ取り出してデータを書き、キューへ入れる */
int v4l2_m2m_queue_out(V4l2M2m *m, const void *data, size_t size,
                       int64_t pts_us, int keyframe);

/* エンコード済みデータを1つ取り出す。戻り値: 1=取得, 0=まだ無い, -1=エラー */
int v4l2_m2m_dequeue_cap(V4l2M2m *m, V4l2Buffer *buf);

/* 使い終わった入力バッファを戻す */
int v4l2_m2m_requeue_cap(V4l2M2m *m, V4l2Buffer *buf);

/* 出力側の空きバッファを取り出す（書き込み用）。無ければ NULL */
V4l2Buffer *v4l2_m2m_get_free_out(V4l2M2m *m);

/* STREAMOFF まで含めた停止（ドレイン付き） */
int v4l2_m2m_drain(V4l2M2m *m);

#endif /* V4L2_M2M_H */
