/*
 * logo_carousel.h - BS/CS の局ロゴ (エンジニアリングサービス 929) を取り出す
 *
 * 929 の PMT は component_tag 0x79 (BS) と 0x7A (CS) のカルーセルを持ち、
 * その中の LOGO-0x / CS_LOGO-0x というモジュールが局ロゴの PNG を運ぶ。
 * ここでは TS からそのモジュールを組み立て、ロゴ本体を解釈するところまでを
 * 持つ。ファイル出力は呼び出し側 (pix_dt295_logo.c) の仕事。
 */
#ifndef PIX_LOGO_CAROUSEL_H
#define PIX_LOGO_CAROUSEL_H

#include <stddef.h>
#include <stdint.h>

#include "ts_section.h"

#define LOGO_MAX_CAROUSELS 4
#define LOGO_MAX_MODULES 16
#define LOGO_MAX_MODULE_BYTES (256u * 1024u)
#define LOGO_MAX_BLOCKS 4096
#define LOGO_MAX_SERVICES 1024
#define LOGO_MAX_ENTRIES 256
#define LOGO_MAX_NAMES 64
#define LOGO_NAME_MAX 64

/* TVTest ignores a logo of this size or less: it is a transparent placeholder. */
#define LOGO_TRANSPARENT_MAX 93

typedef struct {
    uint16_t original_network_id;
    uint16_t transport_stream_id;
    uint16_t service_id;
} logo_service;

typedef struct {
    uint16_t logo_id;
    uint8_t logo_type;
    const logo_service *services;
    size_t service_count;
    const uint8_t *png;
    size_t png_length;
} logo_entry;

typedef struct {
    uint16_t pid;
    uint8_t tag;
    int used;
} logo_carousel;

typedef struct {
    int used;
    uint32_t download_id;
    uint16_t id;
    uint8_t version;
    uint16_t block_size;
    uint32_t size;
    char name[16];
    uint16_t blocks_total;
    uint16_t blocks_have;
    uint8_t *body;
    uint32_t seen[LOGO_MAX_BLOCKS / 32];
    int complete;
    int parsed;
} logo_module;

typedef struct {
    uint16_t sid;
    uint16_t pmt_pid;
    int have_pmt;
    uint16_t pmt_generation;
    logo_carousel carousels[LOGO_MAX_CAROUSELS];
    unsigned carousel_count;
    logo_module modules[LOGO_MAX_MODULES];
    unsigned module_count;
    unsigned pats;
    ts_demux demux;
    logo_service services[LOGO_MAX_SERVICES];
    unsigned service_count;
    logo_entry entries[LOGO_MAX_ENTRIES];
    unsigned entry_count;
    uint32_t crc_errors;
    uint32_t unsupported;
    struct {
        uint16_t service_id;
        char name[LOGO_NAME_MAX];
    } names[LOGO_MAX_NAMES];
    unsigned name_count;
} logo_collector;

void logo_collector_init(logo_collector *c, uint16_t sid);
void logo_collector_free(logo_collector *c);

/* Feed TS bytes of any length. */
void logo_collector_feed(logo_collector *c, const uint8_t *data, size_t length);

/* Every logo module of every carousel is in (and none was seen yet: false). */
int logo_collector_done(const logo_collector *c);

/* A short human readable progress line, for the log. */
const char *logo_collector_progress(const logo_collector *c, char *buf, size_t cap);

/* Entries in the order they were completed. */
size_t logo_collector_entries(const logo_collector *c, const logo_entry **out);

/* 0x05:5, 0x02:2, 0x04:4, 0x01:1, 0x03:3, 0x00:0 - the reference order for
 * which logo to keep when a service has several. Larger is worse. */
unsigned logo_type_rank(uint8_t logo_type);

/* The service's name from the SDT, or NULL when it has not been seen. */
const char *logo_collector_name(const logo_collector *c, uint16_t service_id);

#endif /* PIX_LOGO_CAROUSEL_H */
