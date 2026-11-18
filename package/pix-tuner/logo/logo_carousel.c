/*
 * logo_carousel.c - 929 のカルーセルから局ロゴを取り出す
 *
 * 形式の根拠:
 *   DSM-CC のヘッダは 12 バイト
 *   (protocolDiscriminator, dsmccType, messageId, transactionId,
 *    reserved, adaptationLength, messageLength)。
 *   DII (table_id 0x3B, messageId 0x1002) の本体は
 *   downloadId(4) blockSize(2) windowSize(1) ackPeriod(1)
 *   tCDownloadWindow(4) tCDownloadScenario(4) compatibilityDescriptor
 *   numberOfModules(2) モジュール列。
 *   DDB (0x3C, 0x1003) の本体は moduleId(2) moduleVersion(1) reserved(1)
 *   blockNumber(2) blockData。
 *   モジュール名はモジュール情報の descriptor tag 0x02 (moduleName)。
 *
 * 方針: DII を見る前に届いた DDB は捨てる。カルーセルは繰り返し送られるので、
 * 次の周でそろう。そのかわり、モジュールの状態を常に 1 つの版に保てる。
 */
#include "logo_carousel.h"

#include "arib_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TID_PAT 0x00
#define TID_PMT 0x02
#define TID_DII 0x3B
#define TID_DDB 0x3C
#define MSG_DII 0x1002
#define MSG_DDB 0x1003
#define STREAM_TYPE_CAROUSEL 0x0D
#define DESC_STREAM_ID 0x52
#define DESC_MODULE_NAME 0x02
#define TAG_BS 0x79
#define TAG_CS 0x7A
#define DSMCC_HEADER 12
#define SECTION_HEADER 8

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

unsigned logo_type_rank(uint8_t logo_type) {
    static const uint8_t order[6] = {5, 2, 4, 1, 3, 0};
    for (unsigned i = 0; i < 6; i++)
        if (order[i] == logo_type)
            return i;
    return 6;
}

/* Descriptors: tag(1) length(1) body(length). */
static int descriptor(const uint8_t *p, size_t n, uint8_t tag, const uint8_t **body,
                      size_t *body_length) {
    size_t at = 0;
    while (at + 2 <= n) {
        uint8_t t = p[at];
        size_t len = p[at + 1];
        if (at + 2 + len > n)
            return 0;
        if (t == tag) {
            *body = p + at + 2;
            *body_length = len;
            return 1;
        }
        at += 2 + len;
    }
    return 0;
}

static int is_logo_module(const char *name) {
    return strcmp(name, "LOGO-00") == 0 || strcmp(name, "LOGO-01") == 0 ||
           strcmp(name, "LOGO-02") == 0 || strcmp(name, "LOGO-03") == 0 ||
           strcmp(name, "LOGO-04") == 0 || strcmp(name, "LOGO-05") == 0 ||
           strcmp(name, "CS_LOGO-00") == 0 || strcmp(name, "CS_LOGO-01") == 0 ||
           strcmp(name, "CS_LOGO-02") == 0 || strcmp(name, "CS_LOGO-03") == 0 ||
           strcmp(name, "CS_LOGO-04") == 0 || strcmp(name, "CS_LOGO-05") == 0;
}

void logo_collector_init(logo_collector *c, uint16_t sid) {
    memset(c, 0, sizeof(*c));
    c->sid = sid;
    ts_demux_init(&c->demux);
    ts_demux_add(&c->demux, 0x0000); /* PAT */
    ts_demux_add(&c->demux, 0x0011); /* SDT: the service names */
}

void logo_collector_free(logo_collector *c) {
    for (unsigned i = 0; i < LOGO_MAX_MODULES; i++) {
        free(c->modules[i].body);
        c->modules[i].body = NULL;
    }
}

static logo_carousel *carousel_for(logo_collector *c, uint16_t pid) {
    for (unsigned i = 0; i < c->carousel_count; i++)
        if (c->carousels[i].pid == pid)
            return &c->carousels[i];
    return NULL;
}

static logo_module *module_find(logo_collector *c, uint32_t download_id, uint16_t id) {
    for (unsigned i = 0; i < c->module_count; i++)
        if (c->modules[i].used && c->modules[i].download_id == download_id &&
            c->modules[i].id == id)
            return &c->modules[i];
    return NULL;
}

/* Drop borrowed entries before releasing their module. Compact both derived
 * tables in place so surviving entries keep their completion order. */
static void module_clear(logo_collector *c, logo_module *m) {
    if (m->body) {
        uintptr_t start = (uintptr_t)m->body;
        unsigned entries = 0, services = 0;
        for (unsigned i = 0; i < c->entry_count; i++) {
            logo_entry entry = c->entries[i];
            uintptr_t png = (uintptr_t)entry.png;
            /* Integer ranges avoid ordering pointers from different allocations. */
            if (png >= start && png - start < m->size)
                continue;
            memmove(&c->services[services], entry.services,
                    entry.service_count * sizeof(*entry.services));
            entry.services = &c->services[services];
            services += (unsigned)entry.service_count;
            c->entries[entries++] = entry;
        }
        c->entry_count = entries;
        c->service_count = services;
    }
    free(m->body);
    memset(m, 0, sizeof(*m));
}

/* Register (or restart) a module of the current download. */
static logo_module *module_register(logo_collector *c, uint32_t download_id, uint16_t id,
                                    const char *name, uint32_t size, uint8_t version,
                                    uint16_t block_size) {
    logo_module *m = module_find(c, download_id, id);
    if (m && m->body && m->size == size && m->version == version &&
        m->block_size == block_size && strcmp(m->name, name) == 0)
        return m;
    if (!m) {
        if (c->module_count >= LOGO_MAX_MODULES)
            return NULL;
        m = &c->modules[c->module_count++];
        memset(m, 0, sizeof(*m));
    }
    module_clear(c, m);
    uint8_t *body = calloc(1, size ? size : 1);
    if (!body)
        return NULL;
    m->used = 1;
    m->download_id = download_id;
    m->id = id;
    m->version = version;
    m->block_size = block_size;
    m->size = size;
    m->blocks_total = (uint16_t)((size + block_size - 1) / block_size);
    m->body = body;
    snprintf(m->name, sizeof(m->name), "%s", name);
    return m;
}

/* Parse the logos of a complete module. */
static void parse_logo_module(logo_collector *c, const uint8_t *body, size_t length) {
    if (length < 3)
        return;
    uint8_t logo_type = body[0];
    if (logo_type > 5)
        return;
    unsigned count = be16(body + 1);
    size_t pos = 3;
    for (unsigned i = 0; i < count && pos + 3 <= length; i++) {
        uint16_t logo_id = (uint16_t)(be16(body + pos) & 0x01FFu);
        unsigned services = body[pos + 2];
        pos += 3;
        if (services && pos + (size_t)services * 6 > length)
            return;
        size_t first = c->service_count;
        if (first + services > LOGO_MAX_SERVICES)
            return;
        for (unsigned s = 0; s < services; s++) {
            const uint8_t *t = body + pos + (size_t)s * 6;
            c->services[c->service_count++] =
                (logo_service){be16(t), be16(t + 2), be16(t + 4)};
        }
        pos += (size_t)services * 6;
        if (pos + 2 > length)
            return;
        size_t png_length = be16(body + pos);
        pos += 2;
        if (pos + png_length > length)
            return;
        const uint8_t *png = body + pos;
        pos += png_length;
        if (!services || !png_length)
            continue;
        if (c->entry_count >= LOGO_MAX_ENTRIES)
            return;
        c->entries[c->entry_count++] = (logo_entry){
            .logo_id = logo_id,
            .logo_type = logo_type,
            .services = &c->services[first],
            .service_count = services,
            .png = png,
            .png_length = png_length,
        };
    }
}

static void parse_dii(logo_collector *c, uint16_t pid, const uint8_t *sec, size_t length) {
    if (length < SECTION_HEADER + DSMCC_HEADER + 4)
        return;
    if (!carousel_for(c, pid))
        return;
    const uint8_t *m = sec + SECTION_HEADER;
    size_t end = length - 4; /* the section's CRC is last */
    if (be16(m + 2) != MSG_DII)
        return;
    size_t payload = SECTION_HEADER + DSMCC_HEADER + m[9];
    if (payload + 16 + 2 > end)
        return;
    const uint8_t *p = sec + payload;
    uint32_t download_id = be32(p);
    uint16_t block_size = be16(p + 4);
    size_t at = 16;
    size_t compatibility = be16(p + at);
    at += 2 + compatibility;
    if (payload + at + 2 > end)
        return;
    unsigned count = be16(p + at);
    at += 2;
    if (!block_size)
        return;
    struct {
        uint16_t id;
        uint32_t size;
        uint8_t version;
        char name[16];
        int logo;
    } listed[LOGO_MAX_MODULES];
    unsigned n = 0, logos = 0;
    for (unsigned i = 0; i < count && n < LOGO_MAX_MODULES; i++) {
        if (payload + at + 8 > end)
            break;
        uint16_t id = be16(p + at);
        uint32_t size = be32(p + at + 2);
        uint8_t version = p[at + 6];
        size_t info_length = p[at + 7];
        if (payload + at + 8 + info_length > end)
            break;
        const uint8_t *info = p + at + 8;
        const uint8_t *name = NULL;
        size_t name_length = 0;
        at += 8 + info_length;
        if (!descriptor(info, info_length, DESC_MODULE_NAME, &name, &name_length))
            continue;
        if (!name_length || name_length >= sizeof(listed[0].name))
            continue;
        listed[n].id = id;
        listed[n].size = size;
        listed[n].version = version;
        memcpy(listed[n].name, name, name_length);
        listed[n].name[name_length] = '\0';
        listed[n].logo = is_logo_module(listed[n].name);
        logos += (unsigned)listed[n].logo;
        n++;
    }
    if (!logos)
        return;
    /* A new download replaces the previous one: its partial modules would
     * never complete. */
    for (unsigned i = 0; i < c->module_count; i++) {
        logo_module *m2 = &c->modules[i];
        if (m2->used && m2->download_id != download_id) {
            module_clear(c, m2);
        }
    }
    for (unsigned i = 0; i < n; i++) {
        if (!listed[i].logo || !listed[i].size || listed[i].size > LOGO_MAX_MODULE_BYTES)
            continue;
        unsigned total = (unsigned)((listed[i].size + block_size - 1) / block_size);
        if (!total || total > LOGO_MAX_BLOCKS)
            continue;
        (void)module_register(c, download_id, listed[i].id, listed[i].name, listed[i].size,
                              listed[i].version, block_size);
    }
}

static void parse_ddb(logo_collector *c, uint16_t pid, const uint8_t *sec, size_t length) {
    if (length < SECTION_HEADER + DSMCC_HEADER + 6 + 4)
        return;
    if (!carousel_for(c, pid))
        return;
    const uint8_t *m = sec + SECTION_HEADER;
    size_t end = length - 4;
    if (be16(m + 2) != MSG_DDB)
        return;
    uint32_t download_id = be32(m + 4);
    size_t payload = SECTION_HEADER + DSMCC_HEADER + m[9];
    if (payload + 6 > end)
        return;
    const uint8_t *p = sec + payload;
    uint16_t id = be16(p);
    uint8_t version = p[2];
    uint16_t block = be16(p + 4);
    const uint8_t *data = p + 6;
    size_t data_length = end - payload - 6;
    logo_module *m2 = module_find(c, download_id, id);
    if (!m2 || !m2->body || m2->version != version || !m2->block_size)
        return;
    if (block >= m2->blocks_total)
        return;
    if (m2->seen[block / 32] & (1u << (block % 32)))
        return;
    size_t offset = (size_t)block * m2->block_size;
    if (offset >= m2->size)
        return;
    size_t copy = m2->size - offset;
    if (copy > data_length)
        copy = data_length;
    memcpy(m2->body + offset, data, copy);
    m2->seen[block / 32] |= 1u << (block % 32);
    m2->blocks_have++;
    if (m2->blocks_have == m2->blocks_total && !m2->parsed) {
        m2->parsed = 1;
        m2->complete = 1;
        parse_logo_module(c, m2->body, m2->size);
    }
}


/* SDT (table_id 0x42): service_id -> service name, for the index. */
static void parse_sdt(logo_collector *c, const uint8_t *sec, size_t length) {
    if (length < 15)
        return;
    size_t end = length - 4;
    size_t at = 11; /* after original_network_id and the reserved bits */
    while (at + 5 <= end) {
        uint16_t service_id = be16(sec + at);
        size_t loop = (size_t)(be16(sec + at + 3) & 0x0FFFu);
        at += 5;
        if (at + loop > end)
            return;
        size_t desc = at;
        at += loop;
        while (desc + 2 <= at) {
            uint8_t engine_tag = sec[desc];
            size_t body = sec[desc + 1];
            if (desc + 2 + body > at)
                break;
            const uint8_t *d = sec + desc + 2;
            desc += 2 + body;
            if (engine_tag != 0x48 || body < 3)
                continue;
            size_t provider = d[1];
            if (2 + provider + 1 > body)
                break;
            size_t name = d[2 + provider];
            if (3 + provider + name > body)
                break;
            for (unsigned i = 0; i < c->name_count; i++)
                if (c->names[i].service_id == service_id)
                    goto next_service;
            if (c->name_count >= LOGO_MAX_NAMES)
                goto next_service;
            arib_text_to_utf8(d + 3 + provider, name, c->names[c->name_count].name,
                              LOGO_NAME_MAX);
            c->names[c->name_count++].service_id = service_id;
        }
    next_service:;
    }
}

static void parse_pat(logo_collector *c, const uint8_t *sec, size_t length) {
    if (length < 12)
        return;
    c->pats++;
    size_t end = length - 4;
    for (size_t at = 8; at + 4 <= end; at += 4) {
        uint16_t program = be16(sec + at);
        uint16_t pid = (uint16_t)(be16(sec + at + 2) & 0x1FFFu);
        if (program && program == c->sid) {
            if (c->pmt_pid != pid) {
                c->pmt_pid = pid;
                c->have_pmt = 0;
                c->pmt_generation++;
                ts_demux_add(&c->demux, pid);
            }
            return;
        }
    }
}

static void parse_pmt(logo_collector *c, const uint8_t *sec, size_t length, uint16_t pid) {
    if (length < 16 || pid != c->pmt_pid)
        return;
    if (be16(sec + 3) != c->sid)
        return;
    size_t end = length - 4;
    size_t info_length = (size_t)(be16(sec + 10) & 0x0FFFu);
    size_t at = 12 + info_length;
    int added = 0;
    while (at + 5 <= end) {
        uint8_t stream_type = sec[at];
        uint16_t es_pid = (uint16_t)(be16(sec + at + 1) & 0x1FFFu);
        size_t es_info = (size_t)(be16(sec + at + 3) & 0x0FFFu);
        if (at + 5 + es_info > end)
            break;
        const uint8_t *desc = sec + at + 5;
        const uint8_t *tag = NULL;
        size_t tag_length = 0;
        if (stream_type == STREAM_TYPE_CAROUSEL &&
            descriptor(desc, es_info, DESC_STREAM_ID, &tag, &tag_length) && tag_length == 1 &&
            (tag[0] == TAG_BS || tag[0] == TAG_CS) && !carousel_for(c, es_pid) &&
            c->carousel_count < LOGO_MAX_CAROUSELS && ts_demux_add(&c->demux, es_pid) == 0) {
            c->carousels[c->carousel_count++] =
                (logo_carousel){.pid = es_pid, .tag = tag[0], .used = 1};
            added = 1;
        }
        at += 5 + es_info;
    }
    if (added || !c->have_pmt) {
        c->have_pmt = 1;
        c->pmt_generation++;
    }
}

static void on_section(void *ctx, uint16_t pid, const uint8_t *section, size_t length) {
    logo_collector *c = ctx;
    uint8_t table_id = section[0];
    if (pid == 0x0000 && table_id == TID_PAT)
        parse_pat(c, section, length);
    else if (pid == 0x0011 && table_id == 0x42)
        parse_sdt(c, section, length);
    else if (c->pmt_pid && pid == c->pmt_pid && table_id == TID_PMT)
        parse_pmt(c, section, length, pid);
    else if (table_id == TID_DII)
        parse_dii(c, pid, section, length);
    else if (table_id == TID_DDB)
        parse_ddb(c, pid, section, length);
}

void logo_collector_feed(logo_collector *c, const uint8_t *data, size_t length) {
    ts_demux_feed(&c->demux, data, length, on_section, c);
    c->crc_errors = 0;
    for (unsigned i = 0; i < TS_DEMUX_PIDS; i++)
        c->crc_errors += c->demux.slots[i].errors;
}

static void module_counts(const logo_collector *c, unsigned *total, unsigned *complete) {
    *total = *complete = 0;
    for (unsigned i = 0; i < c->module_count; i++) {
        const logo_module *m = &c->modules[i];
        if (!m->used || !is_logo_module(m->name))
            continue;
        (*total)++;
        if (m->complete)
            (*complete)++;
    }
}

int logo_collector_done(const logo_collector *c) {
    if (!c->have_pmt || c->carousel_count == 0)
        return 0;
    unsigned total = 0, complete = 0;
    module_counts(c, &total, &complete);
    return total > 0 && complete == total;
}

const char *logo_collector_progress(const logo_collector *c, char *buf, size_t cap) {
    if (!c->have_pmt) {
        snprintf(buf, cap, "waiting for service %u (PAT %u)", (unsigned)c->sid, c->pats);
        return buf;
    }
    if (!c->carousel_count) {
        snprintf(buf, cap, "no logo carousel in the PMT");
        return buf;
    }
    unsigned total = 0, complete = 0;
    module_counts(c, &total, &complete);
    snprintf(buf, cap, "logo modules %u/%u in %u carousel(s)", complete, total,
             c->carousel_count);
    return buf;
}

const char *logo_collector_name(const logo_collector *c, uint16_t service_id) {
    for (unsigned i = 0; i < c->name_count; i++)
        if (c->names[i].service_id == service_id)
            return c->names[i].name;
    return NULL;
}

size_t logo_collector_entries(const logo_collector *c, const logo_entry **out) {
    *out = c->entries;
    return c->entry_count;
}
