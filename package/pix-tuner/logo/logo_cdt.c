/*
 * logo_cdt.c - 地上波の局ロゴ (CDT) を取り出す
 *
 * CDT (table_id 0xC8) のロゴは data_type 0x01 で、
 *   section header 8 バイト
 *   original_network_id(2) data_type(1) 予約(2) ... 13 バイトから
 *   data_length(2 の下位12ビット) ぶんの領域
 *   logo_type(1) logo_id(2, 下位9ビット) logo_version(2) length(2) PNG
 * が並ぶ。どのサービスがどの logo_id を使うかは SDT の
 * logo_transmission_descriptor (0xCF, 先頭 1 バイトが 1 か 2 で CDT) が持つ。
 */
#define _POSIX_C_SOURCE 200809L
#include "logo_cdt.h"

#include "arib_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TID_SDT 0x42
#define TID_CDT 0xC8
#define DESC_LOGO_TRANSMISSION 0xCF
#define DATA_TYPE_LOGO 0x01
#define DESC_SERVICE 0x48
#define PID_SDT 0x0011
#define PID_CDT 0x0029
#define SECTION_HEADER 8

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

void cdt_collector_init(cdt_collector *c) {
    memset(c, 0, sizeof(*c));
    ts_demux_init(&c->demux);
    ts_demux_add(&c->demux, PID_SDT);
    ts_demux_add(&c->demux, PID_CDT);
}

void cdt_collector_free(cdt_collector *c) {
    for (unsigned i = 0; i < CDT_MAX_LOGOS; i++) {
        free(c->logos[i].png);
        c->logos[i].png = NULL;
    }
}

static cdt_map_entry *map_for(cdt_collector *c, uint16_t onid, uint16_t logo_id, int create) {
    for (unsigned i = 0; i < c->map_count; i++)
        if (c->map[i].used && c->map[i].onid == onid && c->map[i].logo_id == logo_id)
            return &c->map[i];
    if (!create || c->map_count >= CDT_MAX_MAP)
        return NULL;
    cdt_map_entry *entry = &c->map[c->map_count++];
    memset(entry, 0, sizeof(*entry));
    entry->used = 1;
    entry->onid = onid;
    entry->logo_id = logo_id;
    return entry;
}

/* SDT: service_id -> logo_id, for the services that point at the CDT. */
static void parse_sdt(cdt_collector *c, const uint8_t *sec, size_t length) {
    if (length < 15)
        return;
    size_t end = length - 4;
    uint16_t tsid = be16(sec + 3); /* table_id_extension */
    uint16_t onid = be16(sec + 8);
    size_t at = SECTION_HEADER + 3; /* after onid and the reserved byte */
    while (at + 5 <= end) {
        uint16_t sid = be16(sec + at);
        size_t loop = (size_t)(be16(sec + at + 3) & 0x0FFFu);
        at += 5;
        if (at + loop > end)
            return;
        size_t desc = at;
        at += loop;
        while (desc + 2 <= at) {
            uint8_t tag = sec[desc];
            size_t body = sec[desc + 1];
            if (desc + 2 + body > at)
                break;
            const uint8_t *d = sec + desc + 2;
            desc += 2 + body;
            if (tag == DESC_SERVICE && body >= 3) {
                size_t provider = d[1];
                if (2 + provider + 1 > body)
                    continue;
                size_t name = d[2 + provider];
                if (3 + provider + name > body)
                    continue;
                int known_name = 0;
                for (unsigned i = 0; i < c->name_count; i++)
                    if (c->names[i].service_id == sid)
                        known_name = 1;
                if (!known_name && c->name_count < CDT_MAX_NAMES) {
                    arib_text_to_utf8(d + 3 + provider, name, c->names[c->name_count].name,
                                      CDT_NAME_MAX);
                    c->names[c->name_count++].service_id = sid;
                }
                continue;
            }
            if (tag != DESC_LOGO_TRANSMISSION || body < 3)
                continue;
            if (d[0] != 1 && d[0] != 2)
                continue; /* 3 is a logo string, not a CDT */
            uint16_t logo_id = (uint16_t)(be16(d + 1) & 0x01FFu);
            cdt_map_entry *entry = map_for(c, onid, logo_id, 1);
            if (!entry)
                continue;
            if (!entry->tsid)
                entry->tsid = tsid;
            int known = 0;
            for (unsigned i = 0; i < entry->service_count; i++)
                if (entry->service_id[i] == sid)
                    known = 1;
            if (!known && entry->service_count < CDT_MAX_MAP_SERVICES) {
                entry->service_id[entry->service_count++] = sid;
                c->generation++;
            }
        }
    }
    c->sdt_sections++;
}

static void parse_cdt(cdt_collector *c, const uint8_t *sec, size_t length) {
    if (length < 20)
        return;
    if ((sec[1] & 0x80) == 0 || (sec[5] & 0x01) == 0)
        return; /* not a long section, or the next version */
    uint16_t onid = be16(sec + 8);
    if (sec[10] != DATA_TYPE_LOGO)
        return;
    size_t start = 13 + (size_t)(be16(sec + 11) & 0x0FFFu);
    size_t end = length - 4;
    if (start + 7 > end)
        return;
    const uint8_t *m = sec + start;
    uint8_t logo_type = m[0];
    uint16_t logo_id = (uint16_t)(be16(m + 1) & 0x01FFu);
    size_t png_length = be16(m + 5);
    if (logo_type > 5 || !png_length || png_length > CDT_PNG_MAX || start + 7 + png_length > end)
        return;

    cdt_logo *logo = NULL;
    for (unsigned i = 0; i < c->logo_count; i++)
        if (c->logos[i].used && c->logos[i].onid == onid && c->logos[i].logo_id == logo_id &&
            c->logos[i].logo_type == logo_type)
            logo = &c->logos[i];
    if (!logo) {
        if (c->logo_count >= CDT_MAX_LOGOS)
            return;
        logo = &c->logos[c->logo_count++];
        memset(logo, 0, sizeof(*logo));
        logo->used = 1;
        logo->onid = onid;
        logo->logo_id = logo_id;
        logo->logo_type = logo_type;
    }
    int same = logo->png && logo->png_length == png_length &&
               memcmp(logo->png, m + 7, png_length) == 0;
    if (!same) {
        uint8_t *copy = malloc(png_length);
        if (!copy)
            return;
        memcpy(copy, m + 7, png_length);
        free(logo->png);
        logo->png = copy;
        logo->png_length = png_length;
        c->generation++;
    }

    if (!c->have_first) {
        c->have_first = 1;
        c->first_onid = onid;
        c->first_logo_id = logo_id;
        c->first_type = logo_type;
    } else if (c->first_onid == onid && c->first_logo_id == logo_id &&
               c->first_type == logo_type) {
        c->cycled = 1;
    }
    c->cdt_sections++;
}

static void on_section(void *ctx, uint16_t pid, const uint8_t *section, size_t length) {
    cdt_collector *c = ctx;
    uint8_t table_id = section[0];
    if (c->debug)
        c->debug(c->debug_user, pid, section, length);
    if (pid == PID_SDT && table_id == TID_SDT)
        parse_sdt(c, section, length);
    else if (pid == PID_CDT && table_id == TID_CDT)
        parse_cdt(c, section, length);
}

void cdt_collector_feed(cdt_collector *c, const uint8_t *data, size_t length) {
    ts_demux_feed(&c->demux, data, length, on_section, c);
    c->crc_errors = 0;
    for (unsigned i = 0; i < TS_DEMUX_PIDS; i++)
        c->crc_errors += c->demux.slots[i].errors;
}

int cdt_collector_has_logos(const cdt_collector *c) {
    return c->logo_count > 0;
}

/* How many of the six types the (onid, logo_id) has. */
static unsigned type_count(const cdt_collector *c, uint16_t onid, uint16_t logo_id) {
    unsigned count = 0;
    for (unsigned i = 0; i < c->logo_count; i++)
        if (c->logos[i].used && c->logos[i].onid == onid && c->logos[i].logo_id == logo_id)
            count++;
    return count;
}

int cdt_collector_done(const cdt_collector *c) {
    if (c->cycled)
        return 1;
    if (!c->map_count)
        return 0;
    for (unsigned i = 0; i < c->map_count; i++) {
        if (!c->map[i].used)
            continue;
        if (type_count(c, c->map[i].onid, c->map[i].logo_id) < CDT_LOGO_TYPES)
            return 0;
    }
    return c->logo_count > 0;
}

const char *cdt_collector_progress(const cdt_collector *c, char *buf, size_t cap) {
    snprintf(buf, cap, "CDT logos %u (types %u services), SDT %u, CDT %u%s", c->logo_count,
             c->map_count, c->sdt_sections, c->cdt_sections, c->cycled ? ", round done" : "");
    return buf;
}

const char *cdt_collector_name(const cdt_collector *c, uint16_t service_id) {
    for (unsigned i = 0; i < c->name_count; i++)
        if (c->names[i].service_id == service_id)
            return c->names[i].name;
    return NULL;
}

size_t cdt_collector_bytes(const cdt_collector *c) {
    size_t total = 0;
    for (unsigned i = 0; i < c->logo_count; i++)
        total += c->logos[i].png_length;
    return total;
}

size_t cdt_collector_entries(cdt_collector *c, const logo_entry **out) {
    c->service_count = 0;
    c->entry_count = 0;
    for (unsigned i = 0; i < c->logo_count && c->entry_count < CDT_MAX_LOGOS; i++) {
        cdt_logo *logo = &c->logos[i];
        if (!logo->used || !logo->png_length)
            continue;
        cdt_map_entry *map = map_for(c, logo->onid, logo->logo_id, 0);
        if (!map || !map->service_count)
            continue; /* the SDT has not named a service for it yet */
        if (c->service_count + map->service_count > CDT_MAX_SERVICES)
            continue;
        size_t first = c->service_count;
        for (unsigned s = 0; s < map->service_count; s++) {
            c->services[c->service_count++] = (logo_service){
                .original_network_id = logo->onid,
                .transport_stream_id = map->tsid,
                .service_id = map->service_id[s],
            };
        }
        c->entries[c->entry_count++] = (logo_entry){
            .logo_id = logo->logo_id,
            .logo_type = logo->logo_type,
            .services = &c->services[first],
            .service_count = map->service_count,
            .png = logo->png,
            .png_length = logo->png_length,
        };
    }
    *out = c->entries;
    return c->entry_count;
}
