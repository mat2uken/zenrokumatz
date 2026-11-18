/*
 * test_dt295_logo.c - host test for the station logo extractor.
 *
 * Builds a synthetic engineering-service TS (PAT -> PMT -> DSM-CC DII/DDB ->
 * logo module -> PNG) and checks the collector reassembles it. Passing a path
 * as argv[1] also writes the TS there, so the shell runner can feed the real
 * pix-dt295-logo binary with it.
 *
 * Build and run: scripts/test-dt295-host.sh logo
 */
#define _POSIX_C_SOURCE 200809L
#include "logo_carousel.h"
#include "logo_cdt.h"
#include "ts_section.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An isolated native variant compiles logo_carousel.c with
 * -Dcalloc=logo_test_calloc and this file with -DLOGO_TEST_ALLOC_FAILURE. */
#ifdef LOGO_TEST_ALLOC_FAILURE
static int fail_module_allocation;
void *logo_test_calloc(size_t count, size_t size) {
    if (fail_module_allocation) {
        fail_module_allocation = 0;
        return NULL;
    }
    return calloc(count, size);
}
#endif

static int failures;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            failures++;                                                                \
        }                                                                              \
    } while (0)

typedef struct {
    uint8_t bytes[1 << 20];
    size_t length;
} buffer;

static void build_png(uint8_t *png, size_t length, uint16_t width, uint16_t height);

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Long-form section: table_id, section_length, 5 header bytes, message, CRC. */
static size_t make_section(uint8_t *out, uint8_t table_id, const uint8_t *message,
                           size_t message_length) {
    size_t body = 5 + message_length + 4; /* after the length field */
    out[0] = table_id;
    out[1] = (uint8_t)(0xB0 | ((body >> 8) & 0x0F));
    out[2] = (uint8_t)(body & 0xFF);
    out[3] = 0;
    out[4] = 0; /* table_id_extension */
    out[5] = 0xC1; /* version 0, current_next 1 */
    out[6] = 0;    /* section_number */
    out[7] = 0;    /* last_section_number */
    memcpy(out + 8, message, message_length);
    size_t total = 8 + message_length + 4;
    uint32_t crc = ts_crc32_mpeg(out, total - 4);
    put32(out + total - 4, crc);
    return total;
}

/* One section into 188-byte packets on a PID. */
static void push_section(buffer *out, uint16_t pid, const uint8_t *section, size_t length) {
    size_t at = 0;
    unsigned cc = 0;
    int first = 1;
    while (at < length) {
        uint8_t packet[TS_PACKET];
        memset(packet, 0xFF, sizeof(packet));
        packet[0] = 0x47;
        packet[1] = (uint8_t)((first ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
        packet[2] = (uint8_t)(pid & 0xFF);
        packet[3] = (uint8_t)(0x10 | (cc & 0x0F));
        size_t room = TS_PACKET - 4 - (size_t)(first ? 1 : 0);
        size_t n = length - at;
        if (n > room)
            n = room;
        size_t payload = 4;
        if (first) {
            packet[payload++] = 0; /* pointer_field */
            first = 0;
        }
        memcpy(packet + payload, section + at, n);
        at += n;
        cc++;
        memcpy(out->bytes + out->length, packet, TS_PACKET);
        out->length += TS_PACKET;
    }
}

/* DSM-CC common header: 12 bytes, no adaptation. */
static size_t dsmcc_header(uint8_t *m, uint16_t message_id, uint32_t transaction, size_t body) {
    m[0] = 0x11;
    m[1] = 0x03;
    put16(m + 2, message_id);
    put32(m + 4, transaction);
    m[8] = 0xFF; /* reserved */
    m[9] = 0;    /* adaptationLength */
    put16(m + 10, (uint16_t)body);
    return 12;
}

static uint8_t logo_body[4096];
static size_t logo_body_length;

/* Two sections in one packet, the second split after two bytes: a section
 * does not have to start on a packet boundary. */
static void collect_section(void *ctx, uint16_t pid, const uint8_t *section, size_t length) {
    (void)section;
    int *seen = ctx;
    (void)pid;
    if (length)
        seen[0]++;
    if (length >= 1 && section[0] == 0x70)
        seen[1]++;
    if (length >= 1 && section[0] == 0x71)
        seen[2]++;
}

static void test_section_packing(void) {
    ts_demux demux;
    ts_demux_init(&demux);
    CHECK(ts_demux_add(&demux, 0x0100) == 0);

    /* Short sections (section_syntax_indicator 0): 3 header bytes, then 5
     * payload bytes named by the section_length field. */
    uint8_t first[8] = {0x70, 0x30, 0x05, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    uint8_t second[8] = {0x71, 0x30, 0x05, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t packet[TS_PACKET];
    memset(packet, 0xFF, sizeof(packet));
    packet[0] = 0x47;
    packet[1] = (uint8_t)(0x40 | 0x01); /* start unit, PID 0x0100 */
    packet[2] = 0x00;
    packet[3] = 0x10;
    packet[4] = 0x00; /* pointer_field */
    memcpy(packet + 5, first, sizeof(first));
    memcpy(packet + 5 + sizeof(first), second, 2); /* the rest follows */

    uint8_t tail[TS_PACKET];
    memset(tail, 0xFF, sizeof(tail));
    tail[0] = 0x47;
    tail[1] = 0x01; /* continuation of PID 0x0100 */
    tail[2] = 0x00;
    tail[3] = 0x11; /* payload, continuity counter 1 */
    memcpy(tail + 4, second + 2, sizeof(second) - 2);

    int seen[3] = {0, 0, 0};
    ts_demux_feed(&demux, packet, sizeof(packet), collect_section, seen);
    ts_demux_feed(&demux, tail, sizeof(tail), collect_section, seen);
    CHECK(seen[0] == 2);
    CHECK(seen[1] == 1);
    CHECK(seen[2] == 1);
    CHECK(demux.slots[0].errors == 0);
}

/* A nonzero pointer carries the old section's tail before the new section.
 * Prefix bytes beyond that tail are not additional sections. */
static void test_section_pointer_tail(void) {
    ts_demux demux;
    ts_demux_init(&demux);
    CHECK(ts_demux_add(&demux, 0x0100) == 0);
    uint8_t body[220] = {0}, first[256], second[32];
    size_t first_n = make_section(first, 0x70, body, sizeof(body));
    size_t second_n = make_section(second, 0x71, body, 4);
    uint8_t packets[2 * TS_PACKET];
    memset(packets, 0xFF, sizeof(packets));
    uint8_t *p = packets;
    p[0] = 0x47; p[1] = 0x41; p[2] = 0; p[3] = 0x10; p[4] = 0;
    memcpy(p + 5, first, 183);
    p += TS_PACKET;
    p[0] = 0x47; p[1] = 0x41; p[2] = 0; p[3] = 0x11;
    size_t tail = first_n - 183;
    const uint8_t not_a_section[] = {0x70, 0x30, 5, 1, 2, 3, 4, 5};
    p[4] = (uint8_t)(tail + sizeof(not_a_section));
    memcpy(p + 5, first + 183, tail);
    memcpy(p + 5 + tail, not_a_section, sizeof(not_a_section));
    memcpy(p + 5 + p[4], second, second_n);
    int seen[3] = {0, 0, 0};
    ts_demux_feed(&demux, packets, 189, collect_section, seen);
    CHECK(seen[0] == 0);
    ts_demux_feed(&demux, packets + 189, sizeof(packets) - 189, collect_section, seen);
    CHECK(seen[0] == 2 && seen[1] == 1 && seen[2] == 1);
    CHECK(demux.slots[0].errors == 0);
}

static void build_logo_body(uint8_t logo_type, const uint8_t *png, size_t png_length) {
    size_t at = 0;
    logo_body[at++] = logo_type;
    put16(logo_body + at, 1); /* one logo */
    at += 2;
    put16(logo_body + at, 1); /* logo_id */
    at += 2;
    logo_body[at++] = 2; /* two services */
    /* BS-11 (network 4, tsid 16625, service 211) and BS-12 (212) */
    const uint16_t triples[2][3] = {{4, 16625, 211}, {4, 16625, 212}};
    for (unsigned i = 0; i < 2; i++)
        for (unsigned k = 0; k < 3; k++) {
            put16(logo_body + at, triples[i][k]);
            at += 2;
        }
    put16(logo_body + at, (uint16_t)png_length);
    at += 2;
    memcpy(logo_body + at, png, png_length);
    at += png_length;
    logo_body_length = at;
}

/* SDT with a logo_transmission_descriptor, and the CDT section it names. */
static void build_sdt(buffer *out, uint16_t tsid, uint16_t onid, uint16_t sid, uint16_t logo_id) {
    uint8_t message[128];
    size_t at = 0;
    put16(message + at, onid);
    at += 2;
    message[at++] = 0xFF; /* reserved */
    put16(message + at, sid);
    at += 2;
    message[at++] = 0xFC; /* EIT flags */
    size_t loop_at = at;
    at += 2;
    size_t desc_at = at;
    message[at++] = 0xCF; /* logo_transmission_descriptor */
    message[at++] = 3;
    message[at++] = 1; /* transmission type 1 = CDT */
    put16(message + at, logo_id);
    at += 2;
    message[at++] = 0x48; /* service_descriptor (name) */
    message[at++] = 5;
    message[at++] = 0x01; /* service type */
    message[at++] = 0;    /* provider name length */
    message[at++] = 2;    /* service name length */
    message[at++] = 0xC1; /* katakana "ア" (GL/GR passthrough) */
    message[at++] = 0xC2;
    put16(message + loop_at, (uint16_t)(0xF000 | (at - desc_at)));
    uint8_t section[256];
    size_t length = make_section(section, 0x42, message, at);
    put16(section + 3, tsid);
    section[5] = 0xC1;
    uint32_t crc = ts_crc32_mpeg(section, length - 4);
    put32(section + length - 4, crc);
    push_section(out, 0x0011, section, length);
}

static void build_cdt(buffer *out, uint16_t onid, uint16_t logo_id, uint8_t logo_type,
                      const uint8_t *png, size_t png_length) {
    uint8_t message[512];
    size_t at = 0;
    put16(message + at, onid);
    at += 2;
    message[at++] = 0x01; /* data_type: logo */
    put16(message + at, 0); /* reserved + data_length */
    at += 2;
    message[at++] = logo_type;
    put16(message + at, logo_id);
    at += 2;
    put16(message + at, 0); /* logo_version */
    at += 2;
    put16(message + at, (uint16_t)png_length);
    at += 2;
    memcpy(message + at, png, png_length);
    at += png_length;
    uint8_t section[1024];
    size_t length = make_section(section, 0xC8, message, at);
    section[5] = 0xC1; /* current_next = 1 */
    uint32_t crc = ts_crc32_mpeg(section, length - 4);
    put32(section + length - 4, crc);
    push_section(out, 0x0029, section, length);
}

static void test_cdt(void) {
    static buffer ts;
    uint8_t png[180];
    build_png(png, sizeof(png), 72, 40);
    build_sdt(&ts, 0x1001, 0x0006, 35864, 3);
    build_cdt(&ts, 0x0006, 3, 5, png, sizeof(png));

    static cdt_collector collector;
    cdt_collector_init(&collector);
    size_t at = 0;
    while (at < ts.length) {
        size_t n = ts.length - at;
        if (n > 500)
            n = 500;
        cdt_collector_feed(&collector, ts.bytes + at, n);
        at += n;
    }
    CHECK(cdt_collector_has_logos(&collector));
    CHECK(collector.generation > 0);
    const logo_entry *entries = NULL;
    size_t count = cdt_collector_entries(&collector, &entries);
    CHECK(count == 1);
    if (count == 1) {
        CHECK(entries[0].logo_type == 5);
        CHECK(entries[0].service_count == 1);
        CHECK(entries[0].services[0].service_id == 35864);
        CHECK(entries[0].services[0].transport_stream_id == 0x1001);
        CHECK(entries[0].png_length == sizeof(png));
        CHECK(memcmp(entries[0].png, png, sizeof(png)) == 0);
    }
    const char *name = cdt_collector_name(&collector, 35864);
    CHECK(name != NULL && name[0] != '\0');

    /* The same logo again must not count as a change (no file rewrite). */
    unsigned generation = collector.generation;
    at = 0;
    while (at < ts.length) {
        size_t n = ts.length - at;
        if (n > 500)
            n = 500;
        cdt_collector_feed(&collector, ts.bytes + at, n);
        at += n;
    }
    CHECK(collector.generation == generation);
    CHECK(collector.cycled);
    CHECK(cdt_collector_done(&collector));
    cdt_collector_free(&collector);
}

static void build_program_map(buffer *out, uint16_t pmt_pid, uint16_t es_pid, uint8_t tag) {
    uint8_t message[128];
    size_t at = 0;
    put16(message + at, 0x0040); /* PCR PID (unused here) */
    at += 2;
    put16(message + at, 0xF000); /* program_info_length 0 */
    at += 2;
    message[at++] = 0x0D; /* stream_type: data carousel */
    put16(message + at, (uint16_t)(0xE000 | es_pid));
    at += 2;
    put16(message + at, 0xF000 | 3); /* ES info length */
    at += 2;
    message[at++] = 0x52; /* stream identifier descriptor */
    message[at++] = 1;
    message[at++] = tag;
    uint8_t section[256];
    size_t length = make_section(section, 0x02, message, at);
    put16(section + 3, 929); /* table_id_extension = program number */
    section[5] = 0xC1;
    uint32_t crc = ts_crc32_mpeg(section, length - 4);
    put32(section + length - 4, crc);
    push_section(out, pmt_pid, section, length);
}

static void build_program_association(buffer *out, uint16_t pmt_pid) {
    uint8_t message[16];
    size_t at = 0;
    put16(message + at, 929); /* program number */
    at += 2;
    put16(message + at, (uint16_t)(0xE000 | pmt_pid));
    at += 2;
    uint8_t section[64];
    size_t length = make_section(section, 0x00, message, at);
    put16(section + 3, 0x0001); /* transport_stream_id */
    section[5] = 0xC1;
    uint32_t crc = ts_crc32_mpeg(section, length - 4);
    put32(section + length - 4, crc);
    push_section(out, 0x0000, section, length);
}

static void build_dii_version(buffer *out, uint16_t pid, uint32_t download_id, uint16_t module_id,
                              const char *name, size_t module_size, uint16_t block_size,
                              uint8_t version) {
    uint8_t message[256];
    size_t at = dsmcc_header(message, 0x1002, download_id, 0);
    put32(message + at, download_id);
    at += 4;
    put16(message + at, block_size);
    at += 2;
    message[at++] = 0; /* windowSize */
    message[at++] = 0; /* ackPeriod */
    put32(message + at, 0);
    at += 4;
    put32(message + at, 0);
    at += 4;
    put16(message + at, 0); /* compatibilityDescriptorLength */
    at += 2;
    put16(message + at, 1); /* numberOfModules */
    at += 2;
    put16(message + at, module_id);
    at += 2;
    put32(message + at, (uint32_t)module_size);
    at += 4;
    message[at++] = version; /* moduleVersion */
    size_t info_at = at;
    at++;
    message[at++] = 0x02; /* moduleName descriptor */
    message[at++] = (uint8_t)strlen(name);
    memcpy(message + at, name, strlen(name));
    at += strlen(name);
    message[info_at] = (uint8_t)(at - info_at - 1);
    put16(message + 10, (uint16_t)(at - 12)); /* messageLength */
    uint8_t section[512];
    size_t length = make_section(section, 0x3B, message, at);
    push_section(out, pid, section, length);
}

static void build_dii(buffer *out, uint16_t pid, uint32_t download_id, uint16_t module_id,
                      const char *name, size_t module_size, uint16_t block_size) {
    build_dii_version(out, pid, download_id, module_id, name, module_size, block_size, 0);
}

static void build_ddb_version(buffer *out, uint16_t pid, uint32_t download_id, uint16_t module_id,
                              uint16_t block, uint16_t block_size, const uint8_t *body,
                              size_t body_length, uint8_t version) {
    uint8_t message[2048];
    size_t data = body_length - (size_t)block * block_size;
    if (data > block_size)
        data = block_size;
    size_t at = dsmcc_header(message, 0x1003, download_id, 0);
    put16(message + at, module_id);
    at += 2;
    message[at++] = version; /* moduleVersion */
    message[at++] = 0xFF; /* reserved */
    put16(message + at, block);
    at += 2;
    memcpy(message + at, body + (size_t)block * block_size, data);
    at += data;
    put16(message + 10, (uint16_t)(at - 12));
    uint8_t section[4096];
    size_t length = make_section(section, 0x3C, message, at);
    push_section(out, pid, section, length);
}

static void build_ddb(buffer *out, uint16_t pid, uint32_t download_id, uint16_t module_id,
                      uint16_t block, uint16_t block_size, const uint8_t *body,
                      size_t body_length) {
    build_ddb_version(out, pid, download_id, module_id, block, block_size, body, body_length, 0);
}

static void build_png(uint8_t *png, size_t length, uint16_t width, uint16_t height) {
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    memset(png, 0x5A, length);
    memcpy(png, signature, 8);
    put32(png + 8, 13); /* IHDR length */
    memcpy(png + 12, "IHDR", 4);
    put32(png + 16, width);
    put32(png + 20, height);
}

static void check_carousel_entries(const logo_collector *c, const unsigned *ids, size_t count,
                                   const uint8_t pngs[3][200]) {
    const logo_entry *entries = NULL;
    size_t actual = logo_collector_entries(c, &entries);
    CHECK(actual == count);
    CHECK(c->service_count == count * 2);
    if (actual != count)
        return;
    for (size_t i = 0; i < count; i++) {
        const logo_entry *e = &entries[i];
        unsigned id = ids[i];
        CHECK(e->logo_id == id);
        CHECK(e->service_count == 2);
        CHECK(e->services == &c->services[i * 2]);
        if (e->service_count == 2) {
            for (unsigned s = 0; s < 2; s++) {
                CHECK(e->services[s].original_network_id == 4);
                CHECK(e->services[s].transport_stream_id == 16625);
                CHECK(e->services[s].service_id == 211 + id * 10 + s);
            }
        }
        CHECK(e->png_length == sizeof(pngs[0]));
        if (e->png_length == sizeof(pngs[0]))
            CHECK(memcmp(e->png, pngs[id - 1], sizeof(pngs[0])) == 0);
    }
}

static void test_module_replacement(void) {
    enum { VERSION, SIZE, BLOCK_SIZE, NAME, DOWNLOAD_ID, ALLOC_FAILURE };
    unsigned cases = ALLOC_FAILURE;
#ifdef LOGO_TEST_ALLOC_FAILURE
    cases++;
#endif
    for (unsigned change = 0; change < cases; change++) {
        static buffer ts, batch;
        static logo_collector c, coalesced;
        uint8_t pngs[3][200], bodies[3][256];
        size_t lengths[3];
        const uint16_t pmt_pid = 0x01F0, pid = 0x0123, block_size = 64;
        const uint32_t download_id = 7;
        ts.length = 0;
        logo_collector_init(&c, 929);
        build_program_association(&ts, pmt_pid);
        build_program_map(&ts, pmt_pid, pid, 0x79);
        for (unsigned i = 0; i < 3; i++) {
            build_png(pngs[i], sizeof(pngs[i]), 96, 56);
            pngs[i][199] = (uint8_t)(0xA0 + i);
            build_logo_body(5, pngs[i], sizeof(pngs[i]));
            put16(logo_body + 3, (uint16_t)(i + 1));
            put16(logo_body + 10, (uint16_t)(211 + (i + 1) * 10));
            put16(logo_body + 16, (uint16_t)(212 + (i + 1) * 10));
            lengths[i] = logo_body_length;
            memcpy(bodies[i], logo_body, lengths[i]);
            build_dii(&ts, pid, download_id, (uint16_t)(i + 1), "LOGO-00", lengths[i],
                      block_size);
        }
        /* Completion order differs from registration order, with the victim
         * between two survivors. Removing it must compact services as well. */
        const unsigned completed[] = {3, 1, 2}, surviving[] = {3, 2}, replaced[] = {3, 2, 1};
        for (unsigned i = 0; i < 3; i++) {
            unsigned id = completed[i];
            for (size_t b = 0; b * block_size < lengths[id - 1]; b++)
                build_ddb(&ts, pid, download_id, (uint16_t)id, (uint16_t)b, block_size,
                          bodies[id - 1], lengths[id - 1]);
        }
        batch.length = ts.length;
        memcpy(batch.bytes, ts.bytes, ts.length);
        logo_collector_feed(&c, ts.bytes, ts.length);
        CHECK(logo_collector_done(&c));
        check_carousel_entries(&c, completed, 3, pngs);

        /* Unchanged DII and repeated DDB do not duplicate either derived table. */
        ts.length = 0;
        for (unsigned repeat = 0; repeat < 3; repeat++) {
            build_dii(&ts, pid, download_id, 1, "LOGO-00", lengths[0], block_size);
            build_ddb(&ts, pid, download_id, 1, 0, block_size, bodies[0], lengths[0]);
        }
        logo_collector_feed(&c, ts.bytes, ts.length);
        check_carousel_entries(&c, completed, 3, pngs);

        uint32_t next_download = change == DOWNLOAD_ID ? download_id + 1 : download_id;
        uint8_t next_version = change == VERSION ? 1 : 0;
        uint16_t next_block = change == BLOCK_SIZE ? block_size + 8 : block_size;
        const char *next_name = change == NAME ? "LOGO-01" : "LOGO-00";
        size_t next_length = lengths[0] + (change == SIZE || change == ALLOC_FAILURE ? 7 : 0);
        memset(bodies[0] + lengths[0], 0, next_length - lengths[0]);
        pngs[0][199] = 0xF0;
        bodies[0][lengths[0] - 1] = pngs[0][199];
        ts.length = 0;
        build_dii_version(&ts, pid, next_download, 1, next_name, next_length, next_block,
                          next_version);
#ifdef LOGO_TEST_ALLOC_FAILURE
        if (change == ALLOC_FAILURE)
            fail_module_allocation = 1;
#endif
        logo_collector_feed(&c, ts.bytes, ts.length);
        size_t survivors = change == DOWNLOAD_ID ? 0 : 2;
        check_carousel_entries(&c, surviving, survivors, pngs);
        /* Also consume only after one feed containing both completion and
         * replacement, as with the CLI's large read batches. */
        memcpy(batch.bytes + batch.length, ts.bytes, ts.length);
        batch.length += ts.length;
        logo_collector_init(&coalesced, 929);
        logo_collector_feed(&coalesced, batch.bytes, batch.length);
        check_carousel_entries(&coalesced, surviving, survivors, pngs);
        logo_collector_free(&coalesced);
#ifdef LOGO_TEST_ALLOC_FAILURE
        if (change == ALLOC_FAILURE) {
            CHECK(fail_module_allocation == 0);
            /* Retry after the failed allocation: no stale entries may reappear. */
            logo_collector_feed(&c, ts.bytes, ts.length);
            check_carousel_entries(&c, surviving, survivors, pngs);
        }
#endif
        CHECK(!logo_collector_done(&c));
        ts.length = 0;
        for (size_t b = 0; b * next_block < next_length; b++)
            build_ddb_version(&ts, pid, next_download, 1, (uint16_t)b, next_block, bodies[0],
                              next_length, next_version);
        logo_collector_feed(&c, ts.bytes, ts.length);
        CHECK(logo_collector_done(&c));
        check_carousel_entries(&c, change == DOWNLOAD_ID ? &replaced[2] : replaced,
                               survivors + 1, pngs);
        /* Repeated replacement metadata must not append already-complete logos. */
        ts.length = 0;
        build_dii_version(&ts, pid, next_download, 1, next_name, next_length, next_block,
                          next_version);
        logo_collector_feed(&c, ts.bytes, ts.length);
        check_carousel_entries(&c, change == DOWNLOAD_ID ? &replaced[2] : replaced,
                               survivors + 1, pngs);
        CHECK(c.crc_errors == 0);
        logo_collector_free(&c);
    }
    printf("test_dt295_logo: %u module replacement cases ok\n", cases);
}

int main(int argc, char **argv) {
    static buffer ts;
    const uint16_t pmt_pid = 0x01F0, es_pid = 0x0123;
    const uint32_t download_id = 7;
    const uint16_t block_size = 64;
    uint8_t png[200];

    test_section_packing();
    test_section_pointer_tail();
    test_cdt();
    test_module_replacement();
    build_png(png, sizeof(png), 96, 56);
    build_logo_body(5, png, sizeof(png));
    build_program_association(&ts, pmt_pid);
    build_program_map(&ts, pmt_pid, es_pid, 0x79);
    build_dii(&ts, es_pid, download_id, 1, "LOGO-00", logo_body_length, block_size);
    size_t blocks = (logo_body_length + block_size - 1) / block_size;
    for (size_t block = 0; block < blocks; block++)
        build_ddb(&ts, es_pid, download_id, 1, (uint16_t)block, block_size, logo_body,
                  logo_body_length);

    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        if (!f) {
            fprintf(stderr, "cannot write %s\n", argv[1]);
            return 2;
        }
        fwrite(ts.bytes, 1, ts.length, f);
        fclose(f);
    }
    /* A second TS whose only logo is a transparent placeholder: the binary must
     * write nothing and say so with exit code 3. */
    if (argc > 2) {
        static buffer dummy;
        uint8_t tiny[40];
        build_png(tiny, sizeof(tiny), 16, 8);
        build_logo_body(0, tiny, sizeof(tiny));
        build_program_association(&dummy, pmt_pid);
        build_program_map(&dummy, pmt_pid, es_pid, 0x79);
        build_dii(&dummy, es_pid, download_id, 1, "LOGO-00", logo_body_length, block_size);
        size_t dummy_blocks = (logo_body_length + block_size - 1) / block_size;
        for (size_t block = 0; block < dummy_blocks; block++)
            build_ddb(&dummy, es_pid, download_id, 1, (uint16_t)block, block_size, logo_body,
                      logo_body_length);
        FILE *f = fopen(argv[2], "wb");
        if (!f) {
            fprintf(stderr, "cannot write %s\n", argv[2]);
            return 2;
        }
        fwrite(dummy.bytes, 1, dummy.length, f);
        fclose(f);
    }

    static logo_collector collector;
    logo_collector_init(&collector, 929);
    /* Feed in awkward pieces: the packet boundary must not matter. */
    size_t at = 0;
    while (at < ts.length) {
        size_t n = ts.length - at;
        if (n > 1000)
            n = 1000;
        logo_collector_feed(&collector, ts.bytes + at, n);
        at += n;
    }
    char progress[128];
    CHECK(logo_collector_done(&collector));
    CHECK(collector.pats > 0);
    CHECK(collector.carousel_count == 1);
    CHECK(collector.crc_errors == 0);

    const logo_entry *entries = NULL;
    size_t count = logo_collector_entries(&collector, &entries);
    CHECK(count == 1);
    if (count == 1) {
        CHECK(entries[0].logo_type == 5);
        CHECK(entries[0].service_count == 2);
        CHECK(entries[0].png_length == sizeof(png));
        CHECK(memcmp(entries[0].png, png, sizeof(png)) == 0);
        CHECK(logo_type_rank(5) == 0);
        CHECK(logo_type_rank(0) == 5);
    }

    /* A new module version starts the module over: the old blocks must not be
     * mixed with it. */
    logo_collector latest;
    logo_collector_init(&latest, 929);
    at = 0;
    while (at < ts.length) {
        size_t n = ts.length - at;
        if (n > 1000)
            n = 1000;
        logo_collector_feed(&latest, ts.bytes + at, n);
        at += n;
    }
    CHECK(logo_collector_done(&latest));
    /* A second DII with the same name but a different size restarts the module. */
    static buffer changed;
    build_dii(&changed, es_pid, download_id, 1, "LOGO-00", logo_body_length + block_size,
              block_size);
    logo_collector_feed(&latest, changed.bytes, changed.length);
    CHECK(!logo_collector_done(&latest));

    /* Truncate the TS at the last DDB: the module must stay incomplete. */
    logo_collector partial;
    logo_collector_init(&partial, 929);
    size_t partial_length = ts.length / 2;
    logo_collector_feed(&partial, ts.bytes, partial_length);
    CHECK(!logo_collector_done(&partial));
    const char *text = logo_collector_progress(&partial, progress, sizeof(progress));
    CHECK(text == progress && progress[0] != '\0');

    logo_collector_free(&collector);
    logo_collector_free(&latest);
    logo_collector_free(&partial);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_dt295_logo: ok\n");
    return 0;
}
