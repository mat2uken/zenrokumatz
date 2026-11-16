/*
 * eit_watch.c - TS を素通ししながら EIT の present/following を読み、
 *               現在の番組をファイルに書き出す
 *
 * dtv_android の LiveTsStreamer.java は TS から EIT を解析して
 * service_id / event_id / 開始時刻 / 長さを取り出し、fMP4 のストリームに
 * pixb ボックスを差し込んで NAS に「番組が変わった」と伝えている。
 * Linux 側でも同じ情報が要るので、パイプの途中にこのツールを挟む。
 *
 *   siano-ts | torne-b25 | eit_watch --state /dev/shm/pix/program.txt | ffmpeg ...
 *
 * 標準入力のバイト列はそのまま標準出力へ流す。取りこぼすと映像が壊れるので、
 * 解析に失敗しても必ず全部書き出す。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TS_PACKET 188
#define EIT_PID 0x12
#define MAX_SECTION 4096

/* 現在の番組。NAS へ伝える項目だけを持つ。 */
typedef struct {
    int service_id;
    int event_id;
    int64_t start_ms;
    int64_t duration_ms;
    int64_t wall_ms;
    int valid;
} Program;

static int g_pmt_pid = -1;
static int g_service_id = -1;
static Program g_last;

/* ---- 出力 ---- */

static void state_write(const char *path, const Program *p)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "service=%d\nevent=%d\nstart=%lld\nduration=%lld\nwall=%lld\n",
            p->service_id, p->event_id, (long long)p->start_ms,
            (long long)p->duration_ms, (long long)p->wall_ms);
    fclose(f);
    rename(tmp, path);
}

/* ---- 時刻変換 ---- */

/* EIT の開始時刻は MJD(16bit) + BCD(hh mm ss)。JST 固定で扱う。 */
static int64_t mjd_bcd_to_ms(const uint8_t *p)
{
    int mjd = (p[0] << 8) | p[1];
    int hh = ((p[2] >> 4) & 0x0f) * 10 + (p[2] & 0x0f);
    int mm = ((p[3] >> 4) & 0x0f) * 10 + (p[3] & 0x0f);
    int ss = ((p[4] >> 4) & 0x0f) * 10 + (p[4] & 0x0f);
    if (mjd <= 0 || hh > 23 || mm > 59 || ss > 59)
        return -1;
    int64_t days = (int64_t)mjd - 40587;          /* MJD 40587 = 1970-01-01 */
    int64_t sec = days * 86400 + hh * 3600 + mm * 60 + ss - 9 * 3600;
    return sec * 1000;
}

static int64_t bcd_duration_ms(const uint8_t *p)
{
    int hh = ((p[0] >> 4) & 0x0f) * 10 + (p[0] & 0x0f);
    int mm = ((p[1] >> 4) & 0x0f) * 10 + (p[1] & 0x0f);
    int ss = ((p[2] >> 4) & 0x0f) * 10 + (p[2] & 0x0f);
    if (hh > 23 || mm > 59 || ss > 59)
        return -1;
    return (int64_t)(hh * 3600 + mm * 60 + ss) * 1000;
}

/* ---- PSI/EIT ---- */

/* PAT から最初の実プログラムの service_id と PMT PID を採る。 */
static void handle_pat(const uint8_t *sec, size_t len)
{
    if (g_service_id >= 0 || len < 12)
        return;
    size_t sec_len = ((size_t)(sec[1] & 0x0f) << 8) | sec[2];
    size_t end = 3 + sec_len - 4;                 /* CRC を除く */
    for (size_t i = 8; i + 4 <= len && i + 4 <= end; i += 4) {
        int prog = (sec[i] << 8) | sec[i + 1];
        int pid = ((sec[i + 2] & 0x1f) << 8) | sec[i + 3];
        if (prog != 0) {
            g_service_id = prog;
            g_pmt_pid = pid;
            return;
        }
    }
}

/*
 * EIT の present/following（table_id 0x4E）から番組を採る。
 * service_id が自分の番組と一致するものだけを見る。
 */
static void handle_eit(const uint8_t *sec, size_t len, int64_t wall_ms)
{
    if (g_service_id < 0 || len < 26)
        return;
    size_t sec_len = ((size_t)(sec[1] & 0x0f) << 8) | sec[2];
    size_t end = 3 + sec_len - 4;
    if (end > len)
        end = len;

    int service_id = (sec[3] << 8) | sec[4];
    if (service_id != g_service_id)
        return;

    /*
     * イベントはヘッダ 14 バイトの後ろから始まる。
     * table_id(1) section_length(2) service_id(2) version(1) section(1)
     * last_section(1) ts_id(2) network_id(2) segment_last(1) last_table_id(1)
     *
     * present/following には放送中と次の番組が並ぶ。どちらが先頭かは
     * 放送局によって揺れるので、順番ではなく開始時刻で選ぶ。
     */
    int best_event = 0;
    int64_t best_start = 0, best_dur = 0;
    int found_current = 0;
    size_t i = 14;
    while (i + 12 <= end) {
        int event_id = (sec[i] << 8) | sec[i + 1];
        int64_t ev_start = mjd_bcd_to_ms(sec + i + 2);
        int64_t ev_dur = bcd_duration_ms(sec + i + 7);
        size_t desc_len = ((size_t)(sec[i + 10] & 0x0f) << 8) | sec[i + 11];
        size_t next = i + 12 + desc_len;

        if (event_id != 0 && ev_start > 0 && ev_dur > 0) {
            /*
             * 放送中の番組は、その放送時間に現在時刻が入っているもの。
             * 時計が正しければこれで一意に決まる。present/following には
             * 「次の番組」も並ぶので、順番だけで決めてはいけない。
             */
            if (wall_ms > 0 && wall_ms >= ev_start &&
                wall_ms < ev_start + ev_dur) {
                best_event = event_id;
                best_start = ev_start;
                best_dur = ev_dur;
                found_current = 1;
                break;
            }
            /*
             * どれも現在時刻を含まない場合は、前へ進む方向でいちばん早い
             * ものを採る。時計がまだ合っていない起動直後でも動かすため。
             */
            if (!found_current) {
                int take = 0;
                if (!g_last.valid)
                    take = 1;
                else if (ev_start > g_last.start_ms)
                    take = 1;
                if (take && (best_event == 0 || ev_start < best_start)) {
                    best_event = event_id;
                    best_start = ev_start;
                    best_dur = ev_dur;
                }
            }
        }
        if (next <= i)
            break;
        i = next;
    }

    if (best_event != 0 &&
        (!g_last.valid || g_last.event_id != best_event)) {
        Program p = {
            .service_id = service_id,
            .event_id = best_event,
            .start_ms = best_start,
            .duration_ms = best_dur,
            .wall_ms = wall_ms,
            .valid = 1,
        };
        g_last = p;
    }
}

/* ---- 入力 ---- */

typedef struct {
    uint8_t buf[MAX_SECTION];
    size_t len;
    int active;
} SectionBuf;

static SectionBuf g_pat = {0}, g_eit = {0};

/* セクションを組み立てる。完了したら 1 を返す。 */
static int section_push(SectionBuf *s, const uint8_t *payload, size_t len,
                        int pusi, const uint8_t **out, size_t *out_len)
{
    if (pusi) {
        size_t ptr = payload[0];
        if (1 + ptr >= len) {
            s->len = 0;
            s->active = 0;
            return 0;
        }
        s->len = 0;
        memcpy(s->buf, payload + 1 + ptr, len - 1 - ptr);
        s->len = len - 1 - ptr;
        s->active = s->len >= 3;
    } else if (s->active && s->len + len <= sizeof(s->buf)) {
        memcpy(s->buf + s->len, payload, len);
        s->len += len;
    } else {
        s->active = 0;
        return 0;
    }
    if (!s->active || s->len < 3)
        return 0;

    size_t need = 3 + (((size_t)(s->buf[1] & 0x0f) << 8) | s->buf[2]);
    if (s->len < need)
        return 0;
    *out = s->buf;
    *out_len = need;
    s->active = 0;
    return 1;
}

int main(int argc, char **argv)
{
    const char *state_path = "/dev/shm/pix/program.txt";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--state") && i + 1 < argc)
            state_path = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--state PATH]\n", argv[0]);
            return 2;
        }
    }

    fprintf(stderr, "eit_watch: ready event-offset=14 state=%s\n", state_path);

    static uint8_t in[TS_PACKET * 128];
    size_t have = 0;

    while (1) {
        ssize_t n = read(STDIN_FILENO, in + have, sizeof(in) - have);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        have += (size_t)n;

        /* 188 バイト境界までを処理し、端数は次回へ回す */
        size_t done = have - (have % TS_PACKET);
        for (size_t off = 0; off + TS_PACKET <= done; off += TS_PACKET) {
            const uint8_t *p = in + off;
            if (p[0] != 0x47)
                continue;
            int pid = ((p[1] & 0x1f) << 8) | p[2];
            int pusi = (p[1] & 0x40) != 0;
            int afc = (p[3] >> 4) & 0x3;
            size_t poff = 4;
            if (afc & 0x2)
                poff = 5 + p[4];
            if (!(afc & 0x1) || poff >= TS_PACKET)
                continue;

            const uint8_t *payload = p + poff;
            size_t plen = TS_PACKET - poff;
            const uint8_t *sec = NULL;
            size_t slen = 0;

            if (pid == 0) {
                if (section_push(&g_pat, payload, plen, pusi, &sec, &slen) &&
                    sec[0] == 0x00)
                    handle_pat(sec, slen);
            } else if (pid == EIT_PID) {
                if (section_push(&g_eit, payload, plen, pusi, &sec, &slen) &&
                    sec[0] == 0x4e) {
                    struct timespec ts;
                    clock_gettime(CLOCK_REALTIME, &ts);
                    int64_t wall = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
                    Program before = g_last;
                    handle_eit(sec, slen, wall);
                    if (g_last.valid &&
                        (!before.valid || before.event_id != g_last.event_id)) {
                        state_write(state_path, &g_last);
                        fprintf(stderr, "eit_watch: service=%d event=%d start=%lld dur=%lld\n",
                                g_last.service_id, g_last.event_id,
                                (long long)g_last.start_ms,
                                (long long)g_last.duration_ms);
                    }
                }
            }
        }

        /* 解析し終わった分をそのまま下流へ流す */
        size_t wrote = 0;
        while (wrote < done) {
            ssize_t w = write(STDOUT_FILENO, in + wrote, done - wrote);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                return 1;
            }
            wrote += (size_t)w;
        }
        memmove(in, in + done, have - done);
        have -= done;
        if (have == sizeof(in))
            have = 0;                       /* 188 で割れないまま満杯: 捨てる */
    }
    return 0;
}
