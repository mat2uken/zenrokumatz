/*
 * ecm_dump.c - TS から ECM セクションだけを取り出して書き出す
 *
 * B-CAS カードのサーバが同時に何本まで捌けるかを測るには、実際の放送から
 * 取った ECM を並べるのがいちばん近い。TS をそのまま持ってくるのは大きいので、
 * ここで ECM だけを抜き、内容ごとに 1 回だけ書き出す。
 *
 *   siano-ts ... -t 60 > /tmp/raw.ts
 *   ecm_dump --pid 2305 --pid 2306 < /tmp/raw.ts > /tmp/ecm.bin
 *
 * 出力は [2 バイト長][セクション本体] の繰り返し。長さは CRC を除いた、
 * B-CAS カードへ渡すバイト数に合わせている。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TS_PACKET 188
#define MAX_SECTION 4096
#define MAX_PIDS 8

static int g_pids[MAX_PIDS];
static int g_pid_count;
static uint8_t g_buf[MAX_PIDS][MAX_SECTION];
static size_t g_len[MAX_PIDS];
static int g_active[MAX_PIDS];
static uint8_t g_last[MAX_PIDS][MAX_SECTION];
static size_t g_last_len[MAX_PIDS];

/* 直前と同じ内容は書かない。ECM は同じ内容を繰り返し送出するため。 */
static void emit(int slot, const uint8_t *sec, size_t len)
{
    size_t body = len >= 4 ? len - 4 : len;   /* CRC を落とす */
    if (body == 0 || body > 0xffff)
        return;
    if (g_last_len[slot] == body && memcmp(g_last[slot], sec, body) == 0)
        return;
    uint8_t hdr[2] = { (uint8_t)(body >> 8), (uint8_t)body };
    fwrite(hdr, 1, 2, stdout);
    fwrite(sec, 1, body, stdout);
    memcpy(g_last[slot], sec, body);
    g_last_len[slot] = body;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pid") && i + 1 < argc && g_pid_count < MAX_PIDS)
            g_pids[g_pid_count++] = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: %s --pid N [--pid N ...]\n", argv[0]);
            return 2;
        }
    }
    if (g_pid_count == 0) {
        fprintf(stderr, "at least one --pid is required\n");
        return 2;
    }

    static uint8_t in[TS_PACKET * 64];
    size_t have = 0;
    for (;;) {
        ssize_t n = read(STDIN_FILENO, in + have, sizeof(in) - have);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;
        have += (size_t)n;
        size_t done = have - (have % TS_PACKET);
        for (size_t off = 0; off + TS_PACKET <= done; off += TS_PACKET) {
            const uint8_t *p = in + off;
            if (p[0] != 0x47) continue;
            int pid = ((p[1] & 0x1f) << 8) | p[2];
            int slot = -1;
            for (int i = 0; i < g_pid_count; i++)
                if (g_pids[i] == pid) { slot = i; break; }
            if (slot < 0) continue;

            int pusi = (p[1] & 0x40) != 0;
            int afc = (p[3] >> 4) & 0x3;
            size_t poff = 4;
            if (afc & 0x2) poff = 5 + p[4];
            if (!(afc & 0x1) || poff >= TS_PACKET) continue;
            const uint8_t *pl = p + poff;
            size_t plen = TS_PACKET - poff;

            if (pusi) {
                size_t ptr = pl[0];
                if (1 + ptr >= plen) { g_active[slot] = 0; continue; }
                g_len[slot] = plen - 1 - ptr;
                if (g_len[slot] > MAX_SECTION) g_len[slot] = MAX_SECTION;
                memcpy(g_buf[slot], pl + 1 + ptr, g_len[slot]);
                g_active[slot] = g_len[slot] >= 3;
            } else if (g_active[slot]) {
                if (g_len[slot] + plen > MAX_SECTION) { g_active[slot] = 0; continue; }
                memcpy(g_buf[slot] + g_len[slot], pl, plen);
                g_len[slot] += plen;
            } else {
                continue;
            }
            if (!g_active[slot] || g_len[slot] < 3) continue;
            size_t need = 3 + (((size_t)(g_buf[slot][1] & 0x0f) << 8) | g_buf[slot][2]);
            if (g_len[slot] < need) continue;
            if (g_buf[slot][0] == 0x82)          /* ECM (ARIB STD-B25) */
                emit(slot, g_buf[slot], need);
            g_active[slot] = 0;
        }
        memmove(in, in + done, have - done);
        have -= done;
    }
    return 0;
}
