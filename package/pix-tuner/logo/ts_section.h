/*
 * ts_section.h - TS パケットからセクションを組み立てる
 *
 * 188 バイトのパケットを PID ごとに集め、セクションの区切りで丸ごと渡す。
 * 長いセクションは CRC を検算する。状態はすべて固定長で、ヒープを使わない。
 */
#ifndef PIX_TS_SECTION_H
#define PIX_TS_SECTION_H

#include <stddef.h>
#include <stdint.h>

#define TS_PACKET 188
#define TS_SECTION_MAX 4096
#define TS_DEMUX_PIDS 8

typedef void (*ts_section_cb)(void *ctx, uint16_t pid, const uint8_t *section, size_t length);

typedef struct {
    uint16_t pid;
    int used;
    uint32_t errors;
    size_t length; /* bytes assembled */
    size_t total;  /* 3 + section_length, once the header is in */
    uint8_t buf[TS_SECTION_MAX];
} ts_section_slot;

typedef struct {
    ts_section_slot slots[TS_DEMUX_PIDS];
    uint8_t partial[TS_PACKET];
    size_t partial_length;
    uint32_t crc_errors;
    uint32_t length_errors;
    uint32_t packets;
} ts_demux;

void ts_demux_init(ts_demux *d);

/* Register a PID to follow. Returns 0, or -1 when the table is full. */
int ts_demux_add(ts_demux *d, uint16_t pid);

/* Whether this PID is followed. */
int ts_demux_wants(const ts_demux *d, uint16_t pid);

/* Feed any number of bytes; a packet split between calls is kept. */
void ts_demux_feed(ts_demux *d, const uint8_t *data, size_t length, ts_section_cb cb, void *ctx);

/* CRC-32/MPEG-2 as used by MPEG-2 and ARIB sections. */
uint32_t ts_crc32_mpeg(const uint8_t *data, size_t length);

#endif /* PIX_TS_SECTION_H */
