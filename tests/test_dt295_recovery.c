/*
 * test_dt295_recovery.c - host test for the DT295 firmware recovery paths.
 *
 * The device is replaced by a register/doorbell model, so every state the
 * tuner can be left in by a previous run (0x11 idle, 0xFFFF error, 0x02
 * streaming) can be reproduced without hardware. The firmware image bundle is
 * the real one from the vendored tree: the loader verifies its hashes, so a
 * fabricated bundle would test nothing.
 *
 * Build and run: scripts/test-dt295-host.sh recovery
 */
#define _POSIX_C_SOURCE 200809L
#include "dt295.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USB_TIMEOUT_TEST (-7)
#define MEM_SIZE (16u * 1024u * 1024u)

/* Hooks compiled into dt295.c with -DDT295_TESTING. */
int dt295_test_command(dt295 *d, uint8_t cmd, uint8_t sub, uint16_t val);
unsigned dt295_test_noack(const dt295 *d);
void dt295_test_force_session(dt295 *d, uint64_t now_ms);
uint64_t dt295_test_renewal_due(const dt295 *d);

typedef struct {
    uint8_t *mem;
    uint64_t now;
    unsigned recorded[64][4];
    unsigned commands;
    size_t uploaded;
    size_t ts_available;
    size_t ts_drained;
    int wedged;      /* the firmware stopped acking: nothing is answered */
    unsigned reject_cmd;
    unsigned reject_left;
    unsigned secure_calls;
} mock;

static uint16_t get16(const mock *m, uint32_t a) {
    return (uint16_t)((m->mem[a] << 8) | m->mem[a + 1]);
}
static void set16(mock *m, uint32_t a, uint16_t v) {
    m->mem[a] = (uint8_t)(v >> 8);
    m->mem[a + 1] = (uint8_t)v;
}
static void raise_bit(mock *m, uint16_t bit) {
    set16(m, 0x90074, (uint16_t)(get16(m, 0x90074) | bit));
}
static void set_state(mock *m, uint16_t state) {
    set16(m, 0x82008, state);
    raise_bit(m, 0x0004);
}
static void ack(mock *m, int ch, uint8_t cmd, uint8_t result) {
    set16(m, ch ? 0x80016 : 0x80014, (uint16_t)((cmd << 8) | result));
    raise_bit(m, ch ? 0x0002 : 0x0008);
}

/* A type-8 response that the driver accepts: the id echoed back and enough
 * words that a keepalive's 8-word challenge is not rejected. The body is not
 * meaningful; the keepalive MAC is computed by the driver either way. */
static void secure_response(mock *m) {
    uint8_t *p = m->mem + 0x83000;
    uint32_t id = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    memset(p, 0, 512);
    p[0] = (uint8_t)(id >> 24);
    p[1] = (uint8_t)(id >> 16);
    p[2] = (uint8_t)(id >> 8);
    p[3] = (uint8_t)id;
    p[11] = 8; /* words = 8 */
    m->secure_calls++;
}

static void doorbell(mock *m, int ch, uint8_t cmd, uint8_t sub, uint16_t par) {
    if (m->commands < 64) {
        m->recorded[m->commands][0] = (unsigned)ch;
        m->recorded[m->commands][1] = cmd;
        m->recorded[m->commands][2] = sub;
        m->recorded[m->commands][3] = par;
    }
    m->commands++;
    if (m->wedged)
        return;
    if (m->reject_cmd == cmd && m->reject_left) {
        m->reject_left--;
        ack(m, ch, cmd, 0x80);
        return;
    }
    switch (cmd) {
    case 1:
        set_state(m, 0x11);
        break;
    case 2:
        set_state(m, sub ? 0x12 : 0x10);
        break;
    case 3:
        set_state(m, 0x10);
        break;
    case 4:
        if (sub)
            set_state(m, 0x10);
        break;
    case 5:
        set_state(m, par == 2 ? 2 : par == 4 ? 4 : 1);
        break;
    case 8:
        secure_response(m);
        break;
    case 9:
        m->mem[0x823be] = 0;
        m->mem[0x823bf] = 0;
        raise_bit(m, 0x0100);
        break;
    case 0x0a:
        m->mem[0x822ac] = 0x10;
        break;
    default:
        break;
    }
    ack(m, ch, cmd, 0);
}

static int mock_control(void *user, uint8_t rt, uint8_t req, uint16_t value, uint16_t index,
                        uint8_t *data, size_t length, unsigned timeout, size_t *actual) {
    mock *m = user;
    (void)timeout;
    *actual = 0;
    if (req == 0xbc) {
        uint32_t addr = ((uint32_t)(value >> 8) << 16) | index;
        if (addr + length > MEM_SIZE)
            return -1;
        if (rt == 0xc0) {
            memcpy(data, m->mem + addr, length);
        } else if (addr == 0x90074) {
            /* The status bits are write-1-to-clear. */
            for (size_t i = 0; i + 1 < length; i += 2)
                set16(m, 0x90074, (uint16_t)(get16(m, 0x90074) &
                                             ~((uint16_t)(data[i] << 8) | data[i + 1])));
        } else {
            memcpy(m->mem + addr, data, length);
        }
        *actual = length;
        return 0;
    }
    if (req == 0xb8 || req == 0xb9) {
        if (length != 4)
            return -1;
        doorbell(m, req == 0xb9, data[0], data[1], (uint16_t)((data[2] << 8) | data[3]));
        *actual = length;
        return 0;
    }
    return -1;
}

static int mock_bulk(void *user, uint8_t ep, uint8_t *data, size_t length, unsigned timeout,
                     size_t *actual) {
    mock *m = user;
    (void)timeout;
    *actual = 0;
    if (ep == 0x02) {
        m->uploaded += length;
        *actual = length;
        return 0;
    }
    if (ep == 0x81) {
        if (!m->ts_available) {
            m->now += 20;
            return USB_TIMEOUT_TEST;
        }
        size_t n = m->ts_available < length ? m->ts_available : length;
        memset(data, 0x47, n);
        m->ts_available -= n;
        m->ts_drained += n;
        *actual = n;
        return 0;
    }
    return -1;
}

static int mock_interrupt(void *user, uint8_t ep, uint8_t *data, size_t length, unsigned timeout,
                          size_t *actual) {
    mock *m = user;
    (void)ep;
    (void)data;
    (void)length;
    *actual = 0;
    m->now += timeout;
    return USB_TIMEOUT_TEST;
}

static uint64_t mock_now(void *user) {
    return ((mock *)user)->now;
}
static void mock_sleep(void *user, unsigned ms) {
    ((mock *)user)->now += ms;
}

static int failures;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            failures++;                                                                \
        }                                                                              \
    } while (0)

static void mock_init(mock *m) {
    memset(m, 0, sizeof(*m));
    m->mem = calloc(1, MEM_SIZE);
    if (!m->mem) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    m->now = 1000;
}
static void mock_free(mock *m) {
    free(m->mem);
    m->mem = NULL;
}
static dt295 *open_device(mock *m, dt295_assets *assets) {
    dt295_io io = {m, mock_control, mock_bulk, mock_interrupt, mock_now, mock_sleep};
    dt295 *d = NULL;
    char error[256] = {0};
    if (dt295_create(&d, &io, assets, error, sizeof(error))) {
        fprintf(stderr, "dt295_create: %s\n", error);
        exit(2);
    }
    return d;
}
static int has_command(const mock *m, int ch, uint8_t cmd, uint8_t sub) {
    for (unsigned i = 0; i < m->commands && i < 64; i++)
        if ((int)m->recorded[i][0] == ch && m->recorded[i][1] == cmd && m->recorded[i][2] == sub)
            return 1;
    return 0;
}

/* 0x11 (idle, left by a previous run): the idle image is reloaded so the
 * previous session cannot die in the middle of the next stream. */
static void test_boot_from_idle(dt295_assets *assets) {
    mock m;
    mock_init(&m);
    set16(&m, 0x82008, 0x0011);
    dt295 *d = open_device(&m, assets);
    char error[256] = {0};
    CHECK(dt295_boot(d, error, sizeof(error)) == 0);
    CHECK(dt295_state(d) == 0x0010);
    CHECK(has_command(&m, 0, 4, 0));
    CHECK(has_command(&m, 0, 4, 1));
    CHECK(m.uploaded > 0);
    CHECK(!has_command(&m, 0, 3, 0));
    CHECK(dt295_test_noack(d) == 0);
    if (failures)
        fprintf(stderr, "  boot-from-0x11: %s\n", error);
    dt295_destroy(d);
    mock_free(&m);
}

/* When the reload is refused, the suspend/resume pair still brings it back. */
static void test_boot_from_idle_falls_back(dt295_assets *assets) {
    mock m;
    mock_init(&m);
    set16(&m, 0x82008, 0x0011);
    m.reject_cmd = 4;
    m.reject_left = 1;
    dt295 *d = open_device(&m, assets);
    char error[256] = {0};
    CHECK(dt295_boot(d, error, sizeof(error)) == 0);
    CHECK(dt295_state(d) == 0x0010);
    CHECK(has_command(&m, 0, 2, 1));
    CHECK(has_command(&m, 0, 2, 0));
    CHECK(m.uploaded == 0);
    dt295_destroy(d);
    mock_free(&m);
}

/* 0xFFFF (faulted firmware): residual TS is read out, then command 3. */
static void test_boot_from_fault(dt295_assets *assets) {
    mock m;
    mock_init(&m);
    set16(&m, 0x82008, 0xFFFF);
    m.ts_available = 300000;
    dt295 *d = open_device(&m, assets);
    char error[256] = {0};
    CHECK(dt295_boot(d, error, sizeof(error)) == 0);
    CHECK(dt295_state(d) == 0x0010);
    CHECK(m.ts_drained == 300000);
    CHECK(has_command(&m, 0, 3, 0));
    if (failures)
        fprintf(stderr, "  boot-from-0xffff: %s\n", error);
    dt295_destroy(d);
    mock_free(&m);
}

/* 0x02 (streaming, killed mid-recording): the TS of the dead stream must not
 * reach the next stream. */
static void test_boot_from_stream(dt295_assets *assets) {
    mock m;
    mock_init(&m);
    set16(&m, 0x82008, 0x0002);
    m.ts_available = 65536;
    dt295 *d = open_device(&m, assets);
    char error[256] = {0};
    CHECK(dt295_boot(d, error, sizeof(error)) == 0);
    CHECK(dt295_state(d) == 0x0010);
    CHECK(m.ts_drained == 65536);
    CHECK(has_command(&m, 0, 3, 0));
    dt295_destroy(d);
    mock_free(&m);
}

/* 0x10: nothing to recover, no command must be sent. */
static void test_boot_from_booted(dt295_assets *assets) {
    mock m;
    mock_init(&m);
    set16(&m, 0x82008, 0x0010);
    dt295 *d = open_device(&m, assets);
    char error[256] = {0};
    CHECK(dt295_boot(d, error, sizeof(error)) == 0);
    CHECK(m.commands == 0);
    dt295_destroy(d);
    mock_free(&m);
}

/* A stale ACK bit from an earlier command must be cleared, not fatal: the
 * firmware blocks a new doorbell until the host drains it. */
static void test_stale_ack_is_drained(dt295_assets *assets) {
    mock m;
    mock_init(&m);
    set16(&m, 0x82008, 0x0011);
    raise_bit(&m, 0x0008);
    raise_bit(&m, 0x0004);
    dt295 *d = open_device(&m, assets);
    char error[256] = {0};
    CHECK(dt295_boot(d, error, sizeof(error)) == 0);
    CHECK(dt295_state(d) == 0x0010);
    dt295_destroy(d);
    mock_free(&m);
}

/* A wedged firmware answers no doorbell. Four timeouts must be enough to stop
 * waiting on it, or a tune turns into minutes of timeouts. */
static void test_wedged_firmware_fails_fast(dt295_assets *assets) {
    mock m;
    mock_init(&m);
    set16(&m, 0x82008, 0x0011);
    m.wedged = 1;
    dt295 *d = open_device(&m, assets);
    char error[256] = {0};
    CHECK(dt295_boot(d, error, sizeof(error)) != 0);
    /* The recovery path itself has used its attempts; the next command is the
     * one that trips the limit and the one after must not wait at all. */
    CHECK(dt295_test_noack(d) >= 3);
    CHECK(dt295_test_command(d, 1, 0, 0) != 0);
    CHECK(dt295_test_noack(d) >= 4);
    unsigned before = m.commands;
    uint64_t start = m.now;
    CHECK(dt295_test_command(d, 1, 0, 0) != 0);
    CHECK(m.commands == before);
    CHECK(m.now - start < 1000);
    dt295_destroy(d);
    mock_free(&m);
}

/* A rejected command is not a stall: it must not count towards the limit. */
static void test_rejected_command_is_not_a_stall(dt295_assets *assets) {
    mock m;
    mock_init(&m);
    set16(&m, 0x82008, 0xFFFF);
    m.reject_cmd = 3;
    m.reject_left = 1;
    dt295 *d = open_device(&m, assets);
    char error[256] = {0};
    /* The first command 3 is refused; the recovery retries once and succeeds. */
    CHECK(dt295_boot(d, error, sizeof(error)) == 0);
    CHECK(dt295_test_noack(d) == 0);
    CHECK(dt295_state(d) == 0x0010);
    dt295_destroy(d);
    mock_free(&m);
}

/* Keepalive cadence: the firmware gives up about 60 s after the last one, so a
 * renewal must be due well inside that window. */
static void test_keepalive_interval(dt295_assets *assets) {
    mock m;
    mock_init(&m);
    dt295 *d = open_device(&m, assets);
    dt295_test_force_session(d, m.now);
    int renewed = 0;
    char error[256] = {0};
    CHECK(dt295_renew_if_due(d, &renewed, error, sizeof(error)) == 0);
    CHECK(renewed == 1);
    uint64_t due = dt295_test_renewal_due(d);
    CHECK(due > m.now);
    CHECK(due - m.now <= 15000);
    CHECK(m.secure_calls == 2);
    renewed = 0;
    CHECK(dt295_renew_if_due(d, &renewed, error, sizeof(error)) == 0);
    CHECK(renewed == 0);
    dt295_destroy(d);
    mock_free(&m);
}

/* A renewal that fails once (a command racing a state transition) delays the
 * next attempt instead of killing the session. */
static void test_keepalive_retries(dt295_assets *assets) {
    mock m;
    mock_init(&m);
    dt295 *d = open_device(&m, assets);
    dt295_test_force_session(d, m.now);
    m.reject_cmd = 8;
    m.reject_left = 2;
    int renewed = 0;
    char error[256] = {0};
    CHECK(dt295_renew_if_due(d, &renewed, error, sizeof(error)) == 0);
    CHECK(renewed == 0);
    uint64_t due = dt295_test_renewal_due(d);
    CHECK(due > m.now);
    CHECK(due - m.now <= 3000);
    m.now = due;
    m.reject_left = 0;
    renewed = 0;
    CHECK(dt295_renew_if_due(d, &renewed, error, sizeof(error)) == 0);
    CHECK(renewed == 1);
    dt295_destroy(d);
    mock_free(&m);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <dt295.assets>\n", argv[0]);
        return 2;
    }
    dt295_assets *assets = NULL;
    char error[512] = {0};
    if (dt295_assets_load(argv[1], &assets, error, sizeof(error))) {
        fprintf(stderr, "assets: %s\n", error);
        return 2;
    }
    test_boot_from_idle(assets);
    test_boot_from_idle_falls_back(assets);
    test_boot_from_fault(assets);
    test_boot_from_stream(assets);
    test_boot_from_booted(assets);
    test_stale_ack_is_drained(assets);
    test_wedged_firmware_fails_fast(assets);
    test_rejected_command_is_not_a_stall(assets);
    test_keepalive_interval(assets);
    test_keepalive_retries(assets);
    dt295_assets_free(assets);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_dt295_recovery: ok\n");
    return 0;
}
