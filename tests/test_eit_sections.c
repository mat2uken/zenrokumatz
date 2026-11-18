/* Synthetic host fixtures for the actual EIT dispatch and pass-through main.
 * No tuner, card, network, production state, or captured broadcast is used. */
#define _POSIX_C_SOURCE 200809L
#include <time.h>

static int fixture_clock_gettime(clockid_t clock_id, struct timespec *ts) {
    (void)clock_id;
    ts->tv_sec = 1794967250; /* 2026-11-18 11:00:50 JST, inside the fixture */
    ts->tv_nsec = 0;
    return 0;
}
#define clock_gettime fixture_clock_gettime
#define main eit_watch_stream_main
#include "../package/stb-streamer/src/eit_watch.c"
#undef main
#undef clock_gettime

static int failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; \
} } while (0)

static void be16(uint8_t *p, unsigned n) { p[0] = n >> 8; p[1] = n; }
static size_t finish(uint8_t *s, size_t n) {
    size_t length = n + 4 - 3;
    s[1] = 0xb0 | (length >> 8);
    s[2] = length;
    uint32_t crc = ts_crc32_mpeg(s, n);
    s[n++] = crc >> 24; s[n++] = crc >> 16;
    s[n++] = crc >> 8; s[n++] = crc;
    return n;
}
static size_t make_pat(uint8_t *s, unsigned sid) {
    memset(s, 0, 32);
    s[0] = 0; s[5] = 0xc1;
    be16(s + 8, sid); be16(s + 10, 0xe100);
    return finish(s, 12);
}
static size_t make_eit(uint8_t *s, unsigned sid, int following, size_t padding) {
    memset(s, 0, 4096);
    s[0] = 0x4e; be16(s + 3, sid); s[5] = 0xc1;
    s[6] = following; s[7] = 1;
    be16(s + 8, 32195); be16(s + 10, 32195); s[12] = 1; s[13] = 0x4e;
    be16(s + 14, following ? 1601 : 1599);
    const uint8_t present[] = {0xef, 0xb2, 0x09, 0x00, 0x00, 0x02, 0x24, 0x00};
    const uint8_t future[] = {0xef, 0xb2, 0x11, 0x24, 0x00, 0x00, 0x06, 0x00};
    memcpy(s + 16, following ? future : present, 8);
    size_t at = 26;
    /* LS1 + fullwidth alphanumerics: the real arib_text decoder is linked. */
    const uint8_t desc[] = {0x4d, 9, 'j', 'p', 'n', 4, 0x0e, 'N', 'O', 'W', 0};
    memcpy(s + at, desc, sizeof(desc)); at += sizeof(desc);
    while (padding) {
        size_t n = padding > 200 ? 200 : padding;
        s[at++] = 0xfe; s[at++] = n;
        memset(s + at, 0, n); at += n; padding -= n;
    }
    be16(s + 24, at - 26);
    return finish(s, at);
}
static void reset_program(void) {
    g_service_id = -1; g_pmt_pid = -1;
    memset(&g_last, 0, sizeof(g_last));
    memset(g_names, 0, sizeof(g_names)); g_name_next = 0;
}
static void init(ts_demux *d) {
    reset_program(); ts_demux_init(d);
    CHECK(ts_demux_add(d, 0) == 0);
    CHECK(ts_demux_add(d, EIT_PID) == 0);
}
static void packet_bytes(uint8_t *p, unsigned pid, int pusi, const uint8_t *b, size_t n) {
    CHECK(n <= 184);
    memset(p, 0xff, 188);
    p[0] = 0x47; p[1] = (pusi ? 0x40 : 0) | (pid >> 8); p[2] = pid;
    p[3] = 0x10;
    memcpy(p + 4, b, n);
}
static void feed(ts_demux *d, const uint8_t *p, const char *state) {
    /* Deliberately split TS packets between reads. */
    ts_demux_feed(d, p, 7, program_section, (void *)state);
    ts_demux_feed(d, p + 7, 181, program_section, (void *)state);
}
static void section(ts_demux *d, unsigned pid, const uint8_t *s, size_t n,
                    const char *state) {
    uint8_t p[188], payload[184]; unsigned cc = 0; int first = 1;
    while (n) {
        size_t room = first ? 183 : 184;
        size_t take = n < room ? n : room;
        if (first) payload[0] = 0;
        memcpy(payload + (first ? 1 : 0), s, take);
        packet_bytes(p, pid, first, payload, take + (first ? 1 : 0));
        p[3] |= cc++ & 15;
        feed(d, p, state); s += take; n -= take; first = 0;
    }
}
static void pat(ts_demux *d, unsigned sid, const char *state) {
    uint8_t s[32]; size_t n = make_pat(s, sid); section(d, 0, s, n, state);
    CHECK(g_service_id == (int)sid);
}
static void test_pointer_tail(const char *state) {
    ts_demux d; init(&d); pat(&d, 35864, state);
    uint8_t a[4096], b[4096], p[188], payload[184];
    size_t an = make_eit(a, 35864, 0, 210), bn = make_eit(b, 35864, 1, 0);
    CHECK(an > 183 && an - 183 + bn <= 183);
    payload[0] = 0; memcpy(payload + 1, a, 183);
    packet_bytes(p, EIT_PID, 1, payload, 184); feed(&d, p, state);
    CHECK(!g_last.valid);
    size_t tail = an - 183;
    payload[0] = tail; memcpy(payload + 1, a + 183, tail);
    memcpy(payload + 1 + tail, b, bn);
    packet_bytes(p, EIT_PID, 1, payload, 1 + tail + bn); p[3] |= 1;
    feed(&d, p, state);
    CHECK(g_last.valid && g_last.event_id == 1599);
    CHECK(strcmp(g_last.name, "ＮＯＷ") == 0);
    CHECK(d.slots[1].errors == 0);
}
static void test_packed_and_split(const char *state) {
    ts_demux d; init(&d); pat(&d, 35864, state);
    uint8_t a[4096], b[4096], p[188], payload[184];
    size_t an = make_eit(a, 35865, 0, 0), bn = make_eit(b, 35864, 0, 0);
    payload[0] = 0; memcpy(payload + 1, a, an); memcpy(payload + 1 + an, b, bn);
    packet_bytes(p, EIT_PID, 1, payload, 1 + an + bn); feed(&d, p, state);
    CHECK(g_last.valid && g_last.event_id == 1599);
    init(&d); pat(&d, 35864, state);
    /* Use adaptation padding to end a packet after only two bytes of header. */
    packet_bytes(p, EIT_PID, 1, payload, 1 + an + bn);
    size_t plen = 1 + an + 2, adaptation = 183 - plen;
    p[3] = 0x30; p[4] = adaptation;
    memset(p + 5, 0, adaptation);
    p[5 + adaptation] = 0;
    memcpy(p + 6 + adaptation, a, an);
    memcpy(p + 6 + adaptation + an, b, 2);
    feed(&d, p, state); CHECK(!g_last.valid);
    packet_bytes(p, EIT_PID, 0, b + 2, bn - 2); p[3] |= 1; feed(&d, p, state);
    CHECK(g_last.valid && strcmp(g_last.name, "ＮＯＷ") == 0);
}
static void test_normal_and_isolation(const char *state) {
    ts_demux d; uint8_t s[4096];
    init(&d); pat(&d, 211, state); /* BS-style ordinary pointer-zero layout */
    size_t n = make_eit(s, 211, 1, 0);
    section(&d, EIT_PID, s, n, state); CHECK(!g_last.valid);
    n = make_eit(s, 212, 0, 0);
    section(&d, EIT_PID, s, n, state); CHECK(!g_last.valid);
    n = make_eit(s, 211, 0, 700);
    section(&d, 0x27, s, n, state); CHECK(!g_last.valid);
    section(&d, EIT_PID, s, n, state);
    CHECK(g_last.valid && g_last.service_id == 211 && g_last.event_id == 1599);
    CHECK(strcmp(g_last.name, "ＮＯＷ") == 0);
    n = make_eit(s, 211, 1, 0);
    section(&d, EIT_PID, s, n, state);
    CHECK(g_last.event_id == 1599); /* following must never replace current */
}
static void test_rejection_and_reset(const char *state) {
    ts_demux d; uint8_t s[4096], p[188], payload[184];
    init(&d); pat(&d, 35864, state);
    size_t n = make_eit(s, 35864, 0, 0); s[n - 1] ^= 1;
    section(&d, EIT_PID, s, n, state); CHECK(!g_last.valid);
    CHECK(d.slots[1].errors == 1);
    n = make_eit(s, 35864, 0, 210);
    for (unsigned kind = 0; kind < 5; kind++) {
        init(&d); pat(&d, 35864, state);
        payload[0] = 0; memcpy(payload + 1, s, 183);
        packet_bytes(p, EIT_PID, 1, payload, 184); feed(&d, p, state);
        CHECK(d.slots[1].length == 183);
        if (kind == 0) { p[1] |= 0x80; }
        if (kind == 1) { p[3] |= 0x80; }
        if (kind == 2) { p[3] = 0x30; p[4] = 184; }
        if (kind == 3) { p[4] = 184; } /* out-of-packet pointer */
        if (kind == 4) { p[3] = 0; } /* invalid adaptation control */
        feed(&d, p, state); CHECK(d.slots[1].length == 0);
        packet_bytes(p, EIT_PID, 0, s + 183, n - 183); feed(&d, p, state);
        CHECK(!g_last.valid); /* a lost prefix cannot be revived */
        section(&d, EIT_PID, s, n, state); CHECK(g_last.valid);
    }
    init(&d); pat(&d, 35864, state);
    const uint8_t oversized[] = {0, 0x4e, 0xbf, 0xff};
    packet_bytes(p, EIT_PID, 1, oversized, sizeof(oversized)); feed(&d, p, state);
    CHECK(!g_last.valid && d.slots[1].errors == 1);
    section(&d, EIT_PID, s, n, state); CHECK(g_last.valid);
}
int main(int argc, char **argv) {
    if (argc != 3 || strcmp(argv[1], "--fixtures"))
        return eit_watch_stream_main(argc, argv);
    test_pointer_tail(argv[2]);
    test_packed_and_split(argv[2]);
    test_normal_and_isolation(argv[2]);
    test_rejection_and_reset(argv[2]);
    if (failures) return 1;
    puts("test_eit_sections: ok (pointer/packed/split/BS/isolation/CRC/reset)");
    return 0;
}
