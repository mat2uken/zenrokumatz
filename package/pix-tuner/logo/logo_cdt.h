/*
 * logo_cdt.h - 地上波の局ロゴ (CDT) を取り出す
 *
 * 地上波は各局の TS に CDT (Common Data Table, PID 0x29, table_id 0xC8) が
 * 乗っており、そこに自局のロゴ PNG が入る。どのロゴがどのサービスかは SDT の
 * logo_transmission_descriptor (0xCF) が示す。
 *
 * カルーセルと違って 1 周に 10 分ほどかかる (1 つの型が約 100 秒おき) ので、
 * 録画中の TS をそのまま見る使い方を想定している。チューナーは止めない。
 */
#ifndef PIX_LOGO_CDT_H
#define PIX_LOGO_CDT_H

#include <stddef.h>
#include <stdint.h>

#include "logo_carousel.h"
#include "ts_section.h"

#define CDT_MAX_LOGOS 64
#define CDT_MAX_MAP 64
#define CDT_MAX_MAP_SERVICES 24
#define CDT_MAX_SERVICES 256
#define CDT_PNG_MAX (64u * 1024u)
#define CDT_LOGO_TYPES 6
#define CDT_MAX_NAMES 64
#define CDT_NAME_MAX 64

typedef struct {
    uint16_t onid;
    uint16_t logo_id;
    uint16_t tsid;
    uint16_t service_id[CDT_MAX_MAP_SERVICES];
    unsigned service_count;
    int used;
} cdt_map_entry;

typedef struct {
    uint16_t onid;
    uint16_t logo_id;
    uint8_t logo_type;
    uint8_t *png;
    size_t png_length;
    int used;
} cdt_logo;

/* Diagnostic hook: every section the collector sees, before parsing. */
typedef void (*cdt_debug_fn)(void *user, uint16_t pid, const uint8_t *section, size_t length);

typedef struct {
    ts_demux demux;
    cdt_debug_fn debug;
    void *debug_user;
    cdt_map_entry map[CDT_MAX_MAP];
    unsigned map_count;
    cdt_logo logos[CDT_MAX_LOGOS];
    unsigned logo_count;
    /* the same (logo, type) came again: the CDTs have gone round once */
    int cycled;
    int have_first;
    uint16_t first_onid;
    uint16_t first_logo_id;
    uint8_t first_type;
    /* Derived at emit time. */
    logo_service services[CDT_MAX_SERVICES];
    unsigned service_count;
    logo_entry entries[CDT_MAX_LOGOS];
    unsigned entry_count;
    uint32_t crc_errors;
    uint32_t cdt_sections;
    uint32_t sdt_sections;
    /* Bumped whenever the stored logos actually change: the passive tap only
     * writes the files out then, not once per carousel round. */
    unsigned generation;
    struct {
        uint16_t service_id;
        char name[CDT_NAME_MAX];
    } names[CDT_MAX_NAMES];
    unsigned name_count;
} cdt_collector;

void cdt_collector_init(cdt_collector *c);
void cdt_collector_free(cdt_collector *c);
void cdt_collector_feed(cdt_collector *c, const uint8_t *data, size_t length);

/* Every logo the SDT names has all six types, or a CDT round has repeated. */
int cdt_collector_done(const cdt_collector *c);
/* At least one usable logo (a partial round is still worth writing). */
int cdt_collector_has_logos(const cdt_collector *c);
const char *cdt_collector_progress(const cdt_collector *c, char *buf, size_t cap);

/* Entries referencing the collector's own service pool and PNG storage. */
size_t cdt_collector_entries(cdt_collector *c, const logo_entry **out);

/* Total bytes of the PNGs held (for the memory budget). */
size_t cdt_collector_bytes(const cdt_collector *c);

/* The service's name from the SDT, or NULL. */
const char *cdt_collector_name(const cdt_collector *c, uint16_t service_id);

#endif /* PIX_LOGO_CDT_H */
