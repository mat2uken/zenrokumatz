/*
 * ts_section.c - TS パケットからセクションを組み立てる
 *
 * ARIB のカルーセルも PSI も同じ形で来る: ペイロードの先頭に pointer_field、
 * その先にセクションが並ぶ。長いセクション (section_syntax_indicator=1) は
 * 末尾 4 バイトが CRC-32/MPEG-2 で、全体を計算すると 0 になる。
 */
#include "ts_section.h"

#include <string.h>

uint32_t ts_crc32_mpeg(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}

void ts_demux_init(ts_demux *d) {
    memset(d, 0, sizeof(*d));
}

int ts_demux_add(ts_demux *d, uint16_t pid) {
    for (unsigned i = 0; i < TS_DEMUX_PIDS; i++) {
        if (d->slots[i].used && d->slots[i].pid == pid)
            return 0;
        if (!d->slots[i].used) {
            d->slots[i].used = 1;
            d->slots[i].pid = pid;
            return 0;
        }
    }
    return -1;
}

int ts_demux_wants(const ts_demux *d, uint16_t pid) {
    for (unsigned i = 0; i < TS_DEMUX_PIDS; i++)
        if (d->slots[i].used && d->slots[i].pid == pid)
            return 1;
    return 0;
}

static ts_section_slot *slot_for(ts_demux *d, uint16_t pid) {
    for (unsigned i = 0; i < TS_DEMUX_PIDS; i++)
        if (d->slots[i].used && d->slots[i].pid == pid)
            return &d->slots[i];
    return NULL;
}

/* Append bytes to a slot, handing over every complete section. */
static void slot_push(ts_section_slot *s, const uint8_t *p, size_t n, ts_section_cb cb,
                      void *ctx, int only_open) {
    while (n) {
        /* 0xFF is the stuffing the multiplexer pads the packet with: a section
         * never starts with it. */
        if (s->length == 0 && *p == 0xFF)
            return;
        if (s->length < 3) {
            size_t want = 3 - s->length;
            if (want > n)
                want = n;
            memcpy(s->buf + s->length, p, want);
            s->length += want;
            p += want;
            n -= want;
            if (s->length < 3)
                return;
            uint32_t total = 3u + (uint32_t)(((s->buf[1] & 0x0Fu) << 8) | s->buf[2]);
            if (total < 4 || total > TS_SECTION_MAX) {
                s->errors++;
                s->length = 0;
                return;
            }
            s->total = total;
        }
        size_t want = s->total - s->length;
        if (want > n)
            want = n;
        memcpy(s->buf + s->length, p, want);
        s->length += want;
        p += want;
        n -= want;
        if (s->length < s->total)
            return;
        int long_section = (s->buf[1] & 0x80) != 0;
        if (!long_section || ts_crc32_mpeg(s->buf, s->total) == 0)
            cb(ctx, s->pid, s->buf, s->total);
        else
            s->errors++;
        s->length = 0;
        if (only_open)
            return; /* the rest of a pointer prefix is not a new section */
    }
}

static void packet(ts_demux *d, const uint8_t *p, ts_section_cb cb, void *ctx) {
    if (p[0] != 0x47)
        return;
    uint16_t pid = (uint16_t)(((p[1] & 0x1Fu) << 8) | p[2]);
    ts_section_slot *s = slot_for(d, pid);
    if (!s)
        return;
    /* A packet whose payload is flagged bad, or scrambled, is not a section
     * this decoder can trust. */
    if ((p[1] & 0x80) || (p[3] & 0xC0)) {
        s->length = 0;
        return;
    }
    int start_unit = (p[1] & 0x40) != 0;
    unsigned control = (p[3] >> 4) & 3u;
    if (control == 0) {
        s->length = 0;
        return;
    }
    if (control == 2)
        return; /* adaptation only: no section bytes were lost */
    size_t start = 4;
    if (control == 3) {
        size_t adaptation = p[4];
        if (5 + adaptation >= TS_PACKET) {
            s->length = 0;
            return;
        }
        start = 5 + adaptation;
    }
    if (start_unit) {
        /* pointer_field says how far the next section starts. */
        if (start >= TS_PACKET)
            return;
        size_t skip = p[start];
        start++;
        if (start + skip > TS_PACKET) {
            s->length = 0;
            return;
        }
        /* Bytes before the new section may finish the previous one. Deliver it
         * before resetting, but never parse pointer stuffing as new sections. */
        if (s->length && skip)
            slot_push(s, p + start, skip, cb, ctx, 1);
        s->length = 0;
        start += skip;
    } else if (!s->length) {
        return; /* cannot know where the section starts */
    }
    if (start >= TS_PACKET)
        return;
    d->packets++;
    slot_push(s, p + start, TS_PACKET - start, cb, ctx, 0);
}

void ts_demux_feed(ts_demux *d, const uint8_t *data, size_t length, ts_section_cb cb, void *ctx) {
    while (length) {
        if (d->partial_length) {
            size_t want = TS_PACKET - d->partial_length;
            if (want > length)
                want = length;
            memcpy(d->partial + d->partial_length, data, want);
            d->partial_length += want;
            data += want;
            length -= want;
            if (d->partial_length < TS_PACKET)
                return;
            d->partial_length = 0;
            packet(d, d->partial, cb, ctx);
        } else if (length < TS_PACKET) {
            memcpy(d->partial, data, length);
            d->partial_length = length;
            return;
        } else {
            packet(d, data, cb, ctx);
            data += TS_PACKET;
            length -= TS_PACKET;
        }
    }
}
