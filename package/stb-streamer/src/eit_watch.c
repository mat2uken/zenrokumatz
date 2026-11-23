/*
 * eit_watch.c - TS を素通ししながら EIT を読み、現在番組と番組表を書き出す
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
 *
 * 現在番組は --state（既定 /dev/shm/pix/program.txt）に書く。ついでに
 * present/following と schedule 表（0x50–0x5f）から番組表を蓄積し、
 * --epg（既定 /dev/shm/pix/epg.json）に JSON で書き出す。stb_http が
 * /v1/epg で公開し、NAS が取得してカタログに供給する（mirakc は使わない）。
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

#include "arib_text.h"
#include "logo_cdt.h"
#include "logo_store.h"

#define TS_PACKET 188
#define EIT_PID 0x12
#define PROGRAM_NAME_MAX 256

/* 現在の番組。NAS へ伝える項目だけを持つ。 */
typedef struct {
    int service_id;
    int event_id;
    int64_t start_ms;
    int64_t duration_ms;
    int64_t wall_ms;
    char name[PROGRAM_NAME_MAX];
    /*
     * 「番組はまだ続いている」の根拠。EIT の segment_last_section_number が
     * 0x00 以外なら、後続セクションに続きがあるので番組は続いている。0x00
     * は「これで終わり」の番組、または見えていない後続があるだけの番組。
     */
    int continues;
    /* 0x00 が続く回数。これが伸びると後続が無い側へ 1 票入る。 */
    int stops;
    int valid;
} Program;

static int g_pmt_pid = -1;
static int g_service_id = -1;
static Program g_last;

/*
 * 地上波の局ロゴ (CDT) は、録画中のこの TS にそのまま乗っている。素通しする
 * ついでに拾っておけば、チューナーを止めずに /v1/logos を更新できる。
 * --logo-dir が無ければ何もしない。
 */
static cdt_collector *g_cdt;
static const char *g_logo_dir;
static unsigned g_logo_written;
static int g_logo_debug;
static unsigned g_logo_progress_next;

static void logo_debug(void *user, uint16_t pid, const uint8_t *section, size_t length)
{
    (void)user;
    if (!g_logo_debug || length < 4)
        return;
    char hex[80];
    size_t at = 0;
    for (size_t i = 0; i < length && i < 24 && at + 3 < sizeof(hex); i++)
        at += (size_t)snprintf(hex + at, sizeof(hex) - at, "%02x", section[i]);
    fprintf(stderr, "eit_watch: section pid=0x%04x tid=0x%02x len=%zu %s\n", pid, section[0],
            length, hex);
}

/* ---- 出力 ---- */

static void state_write(const char *path, const Program *p)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f,
            "service=%d\nevent=%d\nstart=%lld\nduration=%lld\nwall=%lld\n"
            "continues=%d\nstops=%d\nname=%s\n",
            p->service_id, p->event_id, (long long)p->start_ms,
            (long long)p->duration_ms, (long long)p->wall_ms,
            p->continues, p->stops, p->name);
    fclose(f);
    rename(tmp, path);
}

/*
 * 番組名は short_event_descriptor (0x4D) に入っている。先頭の3バイトが言語
 * コードで、その次の1バイトが名前の長さ。文字列は ARIB STD-B24 の8ビット
 * 符号なので arib_text.c で UTF-8 に直す。
 */
static void trim_spaces(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

/*
 * 番組名は short_event_descriptor に入っているが、present/following の
 * セクションによっては記述子が付かず、スケジュール表の同じイベントには
 * 付いていることがある。見えた名前をイベントIDで覚えておき、現在番組の
 * セクションに名前が無いときはそれを使う。
 */
#define NAME_CACHE_SLOTS 64
typedef struct {
    int event_id;
    char name[PROGRAM_NAME_MAX];
} NameCacheEntry;
static NameCacheEntry g_names[NAME_CACHE_SLOTS];
static unsigned g_name_next;

static void name_cache_put(int event_id, const char *name)
{
    if (event_id <= 0 || name[0] == '\0')
        return;
    for (int i = 0; i < NAME_CACHE_SLOTS; i++) {
        if (g_names[i].event_id == event_id) {
            snprintf(g_names[i].name, sizeof(g_names[i].name), "%s", name);
            return;
        }
    }
    NameCacheEntry *e = &g_names[g_name_next++ % NAME_CACHE_SLOTS];
    e->event_id = event_id;
    snprintf(e->name, sizeof(e->name), "%s", name);
}

static const char *name_cache_get(int event_id)
{
    for (int i = 0; i < NAME_CACHE_SLOTS; i++)
        if (g_names[i].event_id == event_id && g_names[i].name[0])
            return g_names[i].name;
    return "";
}

/* ---- 番組表（スケジュール）キャッシュ ---- */

/*
 * EIT schedule 表を event_id 単位で覚える。現在番組（program.txt）とは別に
 * 番組表全体を epg.json に書き出し、stb_http が /v1/epg で公開する。NAS の
 * ブリッジが取得してカタログとフォールバック命名に供給する。
 * ストレージは tmpfs のみで、中間ディスク書き込みはしない。
 */
#define EPG_MAX_EVENTS 2048
#define EPG_MIN_DURATION_MS 180000        /* 番宣など短い項目は番組として扱わない */
#define EPG_PAST_KEEP_MS (24LL * 3600 * 1000)   /* 過去は 24 時間分残す */
#define EPG_FLUSH_SEC 30

typedef struct {
    int event_id;
    int64_t start_ms;
    int64_t duration_ms;
    char name[PROGRAM_NAME_MAX];
    int valid;
} EpgEvent;

static EpgEvent g_epg[EPG_MAX_EVENTS];
static int g_epg_dirty;
static int64_t g_epg_last_flush;
static const char *g_epg_path;

static void epg_put(int event_id, int64_t start_ms, int64_t duration_ms, const char *name)
{
    if (event_id <= 0 || start_ms <= 0 || duration_ms < EPG_MIN_DURATION_MS)
        return;
    EpgEvent *slot = NULL;
    for (int i = 0; i < EPG_MAX_EVENTS; i++) {
        if (!g_epg[i].valid) {
            if (!slot)
                slot = &g_epg[i];
            continue;
        }
        if (g_epg[i].event_id == event_id) {
            slot = &g_epg[i];
            break;
        }
    }
    if (!slot) {
        /* 満杯: startAt が最も古いものを追い出す。 */
        slot = &g_epg[0];
        for (int i = 1; i < EPG_MAX_EVENTS; i++)
            if (g_epg[i].start_ms < slot->start_ms)
                slot = &g_epg[i];
    }

    int changed = 0;
    if (!slot->valid || slot->event_id != event_id) {
        memset(slot, 0, sizeof(*slot));
        slot->event_id = event_id;
        slot->start_ms = start_ms;
        slot->duration_ms = duration_ms;
        slot->valid = 1;
        changed = 1;
    } else {
        if (slot->start_ms != start_ms) {
            slot->start_ms = start_ms;
            changed = 1;
        }
        if (slot->duration_ms != duration_ms) {
            slot->duration_ms = duration_ms;
            changed = 1;
        }
    }
    if (name && name[0] && strcmp(name, slot->name) != 0) {
        snprintf(slot->name, sizeof(slot->name), "%s", name);
        changed = 1;
    }
    if (changed)
        g_epg_dirty = 1;
}

/* JSON 文字列へ出すための最低限のエスケープ。UTF-8 の複数バイトはそのまま。 */
static size_t json_escape(const char *src, char *dst, size_t cap)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && n + 8 < cap; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':  dst[n++] = '\\'; dst[n++] = '"'; break;
        case '\\': dst[n++] = '\\'; dst[n++] = '\\'; break;
        case '\b': dst[n++] = '\\'; dst[n++] = 'b'; break;
        case '\f': dst[n++] = '\\'; dst[n++] = 'f'; break;
        case '\n': dst[n++] = '\\'; dst[n++] = 'n'; break;
        case '\r': dst[n++] = '\\'; dst[n++] = 'r'; break;
        case '\t': dst[n++] = '\\'; dst[n++] = 't'; break;
        default:
            if (c < 0x20)
                n += (size_t)snprintf(dst + n, cap - n, "\\u%04x", c);
            else
                dst[n++] = (char)c;
            break;
        }
    }
    dst[n] = '\0';
    return n;
}

/* 開始時刻順に並べる。無効な項目は末尾へ送る（qsort は安定でないが順不同の並びは問題ない）。 */
static int epg_order(const void *a, const void *b)
{
    const EpgEvent *x = a, *y = b;
    if (x->valid != y->valid)
        return y->valid - x->valid;
    if (!x->valid)
        return 0;
    if (x->start_ms != y->start_ms)
        return x->start_ms < y->start_ms ? -1 : 1;
    return x->event_id - y->event_id;
}

/*
 * dirty な項目を 30 秒以下の間隔で epg.json へ書き出す。書く前に過去 24 時間
 * より昔に終わった番組を捨て、残りを開始時刻順にして間引く。呼び出し元は
 * データの流れを止めない（program.txt と同じ tmp ファイル + rename である）。
 * force が非 0 なら 30 秒の間隔を無視する（終了時）。
 */
static void epg_flush(int force)
{
    if (!g_epg_dirty || !g_epg_path)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (!force && now - g_epg_last_flush < EPG_FLUSH_SEC * 1000)
        return;
    g_epg_last_flush = now;
    g_epg_dirty = 0;

    qsort(g_epg, EPG_MAX_EVENTS, sizeof(g_epg[0]), epg_order);
    int64_t keep_from = now - EPG_PAST_KEEP_MS;
    size_t count = 0;
    for (int i = 0; i < EPG_MAX_EVENTS; i++) {
        EpgEvent *e = &g_epg[i];
        if (!e->valid)
            break;
        if (e->start_ms + e->duration_ms < keep_from) {
            memset(e, 0, sizeof(*e));
            continue;
        }
        if (count != (size_t)i)
            g_epg[count] = *e;
        count++;
    }
    for (size_t i = count; i < EPG_MAX_EVENTS; i++)
        memset(&g_epg[i], 0, sizeof(g_epg[i]));

    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_epg_path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "{\"serviceId\":%d,\"generatedAt\":%lld,\"events\":[",
            g_service_id > 0 ? g_service_id : 0, (long long)now);
    for (size_t i = 0; i < count; i++) {
        char esc[PROGRAM_NAME_MAX * 6 + 1];
        json_escape(g_epg[i].name, esc, sizeof(esc));
        fprintf(f, "%s{\"eventId\":%d,\"startAt\":%lld,\"duration\":%lld,\"name\":\"%s\"}",
                i ? "," : "", g_epg[i].event_id,
                (long long)g_epg[i].start_ms, (long long)g_epg[i].duration_ms, esc);
    }
    fprintf(f, "]}\n");
    if (fclose(f) != 0)
        return;
    rename(tmp, g_epg_path);
}

static void event_name(const uint8_t *sec, size_t off, size_t desc_len,
                       char *out, size_t cap)
{
    size_t end = off + desc_len;
    out[0] = '\0';
    for (size_t j = off; j + 2 <= end; ) {
        uint8_t tag = sec[j];
        uint8_t len = sec[j + 1];
        if (j + 2 + len > end)
            break;
        if (tag == 0x4d && len >= 5) {
            const uint8_t *b = sec + j + 2;
            size_t n = b[3];
            if (n > 0 && 4 + n <= len) {
                arib_text_to_utf8(b + 4, n, out, cap);
                trim_spaces(out);
                return;
            }
        }
        j += 2 + len;
    }
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
 * 再起動すると g_last が空になり、最初に一致したセクションで状態を
 * 上書きする。そのセクションに名前が無いと、名前が空のまま次の切替まで
 * 残ってしまう。前回の状態がまだ放送時間内なら、名前ごと引き継ぐ。
 */
static void load_state(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    Program p;
    memset(&p, 0, sizeof(p));
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (!strncmp(line, "service=", 8)) p.service_id = atoi(line + 8);
        else if (!strncmp(line, "event=", 6)) p.event_id = atoi(line + 6);
        else if (!strncmp(line, "start=", 6)) p.start_ms = atoll(line + 6);
        else if (!strncmp(line, "duration=", 9)) p.duration_ms = atoll(line + 9);
        else if (!strncmp(line, "wall=", 5)) p.wall_ms = atoll(line + 5);
        else if (!strncmp(line, "continues=", 10)) p.continues = atoi(line + 10);
        else if (!strncmp(line, "stops=", 6)) p.stops = atoi(line + 6);
        else if (!strncmp(line, "name=", 5)) snprintf(p.name, sizeof(p.name), "%s", line + 5);
    }
    fclose(f);
    if (p.service_id <= 0 || p.event_id <= 0 || p.start_ms <= 0 || p.duration_ms <= 0)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (now < p.start_ms || now >= p.start_ms + p.duration_ms)
        return;                       /* 前の番組のものは使わない */
    p.valid = 1;
    g_last = p;
}

/*
 * EIT（present/following とスケジュール表）から現在の番組を採る。
 * service_id が自分の番組と一致するものだけを見る。
 * 返り値は状態が変わったかどうか（呼び出し側が state_write する）。
 */
static int handle_eit(const uint8_t *sec, size_t len, int64_t wall_ms)
{
    if (g_service_id < 0 || len < 26)
        return 0;
    size_t sec_len = ((size_t)(sec[1] & 0x0f) << 8) | sec[2];
    size_t end = 3 + sec_len - 4;
    if (end > len)
        end = len;

    int service_id = (sec[3] << 8) | sec[4];
    if (service_id != g_service_id)
        return 0;

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
    size_t best_desc = 0, best_desc_len = 0;
    int found_current = 0;
    /*
     * 番組の記述が複数セクションに分かれるのが長い番組の普通の形である。
     * 最終セクション番号と現在のセクション番号を読み、分割の有無を
     * 「番組が続いているか」の根拠にする。
     */
    int segment_last = sec[10];
    int section_number = sec[6];
    size_t i = 14;
    while (i + 12 <= end) {
        int event_id = (sec[i] << 8) | sec[i + 1];
        int64_t ev_start = mjd_bcd_to_ms(sec + i + 2);
        int64_t ev_dur = bcd_duration_ms(sec + i + 7);
        size_t desc_len = ((size_t)(sec[i + 10] & 0x0f) << 8) | sec[i + 11];
        size_t next = i + 12 + desc_len;
        {
            /* どのイベントの名前も覚えておく（後で使うことがある）。 */
            char seen[PROGRAM_NAME_MAX] = "";
            if (i + 12 + desc_len <= sec_len + 3)
                event_name(sec, i + 12, desc_len, seen, sizeof(seen));
            if (seen[0])
                name_cache_put(event_id, seen);
            /* 番組表の項目も覚える。3 分未満の項目は番組として扱わない。 */
            epg_put(event_id, ev_start, ev_dur, seen);
        }

        /*
         * present/following には番宣や穴埋めの数秒〜数十秒の項目が混ざる。
         * それを放送中として採ると番組境界が細かく揺れるので、番組として
         * 扱うのは3分以上あるものだけにする。
         */
        if (event_id != 0 && ev_start > 0 && ev_dur >= 180000) {
            /*
             * 放送中の番組は、その放送時間に現在時刻が入っているもの。
             *
             * present と following は別セクションで届く。following の
             * セクションには「次の番組」しか入っていないので、時刻で
             * 判定すれば一致しない。以前は「今より後に始まる一番早い
             * イベント」を採るフォールバックがあり、following セクションが
             * 届くたびに次番組へ飛んでいた（名前が1番組ずれ、区切りも
             * 1番組早くなる）。時刻が一致しないものは採らない。
             */
            if (wall_ms > 0 && wall_ms >= ev_start &&
                wall_ms < ev_start + ev_dur) {
                if (!found_current) {
                    best_event = event_id;
                    best_start = ev_start;
                    best_dur = ev_dur;
                    best_desc = i + 12;
                    best_desc_len = desc_len;
                    found_current = 1;
                }
                /*
                 * 走査は続ける。同じセクションに後続イベントの名前も
                 * 載っているので、名前キャッシュに拾わせる。
                 */
            }
            /*
             * 時刻が一致しないイベントは採らない。以前は起動直後に限り
             * 最初に見えたイベントを仮採用していたが、present セクションが
             * 稀にしか届かない放送では following（次の番組）を拾ったまま
             * 次の切替まで残ってしまう。スケジュール表からも現在番組を
             * 引けるので、暫定採用はしない。
             */
        }
        if (next <= i)
            break;
        i = next;
    }

    if (best_event == 0)
        return 0;

    char name[PROGRAM_NAME_MAX] = "";
    if (best_desc > 0 && best_desc + best_desc_len <= sec_len + 3)
        event_name(sec, best_desc, best_desc_len, name, sizeof(name));
    if (name[0] == '\0')
        snprintf(name, sizeof(name), "%s", name_cache_get(best_event));

    if (!g_last.valid || g_last.event_id != best_event) {
        Program p = {
            .service_id = service_id,
            .event_id = best_event,
            .start_ms = best_start,
            .duration_ms = best_dur,
            .wall_ms = wall_ms,
            .continues = segment_last != 0 || section_number != 0,
            .valid = 1,
        };
        /* 番組名は放送波の記述子から採る（外部 EPG に依存しない）。 */
        snprintf(p.name, sizeof(p.name), "%s", name);
        g_last = p;
        return 1;
    }

    /*
     * continues は 0x00 以外のセクションを見た証拠を保持し、0x00 だけが
     * 続いた回数で打ち消す。長い番組は 0x00 のセクションでも番組は続く。
     */
    if (segment_last != 0 || section_number != 0) {
        g_last.continues = 1;
        g_last.stops = 0;
    } else {
        g_last.stops++;
    }

    /*
     * 同じイベントで名前だけ後から届くことがある（セクションによっては
     * short_event_descriptor が無い）。空のまま固定せず、名前が入った
     * 時点で更新する。
     */
    if (name[0] && strcmp(name, g_last.name) != 0) {
        snprintf(g_last.name, sizeof(g_last.name), "%s", name);
        g_last.wall_ms = wall_ms;
        return 1;
    }
    return 0;
}

/* ---- 入力 ---- */

/* PAT/EIT とロゴは同じセクション再構成を使う。1パケットに複数あっても
 * すべて呼ばれ、次の PUSI の pointer_field に入った前セクションの末尾も届く。 */
static void program_section(void *ctx, uint16_t pid, const uint8_t *sec, size_t slen)
{
    const char *state_path = ctx;
    if (pid == 0 && sec[0] == 0x00) {
        handle_pat(sec, slen);
    } else if (pid == EIT_PID &&
               (sec[0] == 0x4e || (sec[0] >= 0x50 && sec[0] <= 0x5f))) {
        /* present/following とスケジュール表の両方から、時刻が一致する
         * 現在番組だけを採る。選択と名前の方針は handle_eit に集約する。 */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        int64_t wall = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        int changed = handle_eit(sec, slen, wall);
        if (changed && g_last.valid) {
            state_write(state_path, &g_last);
            fprintf(stderr,
                    "eit_watch: service=%d event=%d start=%lld dur=%lld "
                    "continues=%d stops=%d name=%s\n",
                    g_last.service_id, g_last.event_id,
                    (long long)g_last.start_ms,
                    (long long)g_last.duration_ms,
                    g_last.continues, g_last.stops, g_last.name);
        }
    }
}

/*
 * 取り込んだ CDT をファイルに出す。内容が変わったときだけ書き、SD カードを
 * 毎分書き換えないようにする (generation はロゴの中身が変わった回数)。
 */
static void logos_flush(void)
{
    if (!g_cdt || g_cdt->generation == g_logo_written)
        return;
    if (!cdt_collector_has_logos(g_cdt))
        return;
    const logo_entry *entries = NULL;
    size_t count = cdt_collector_entries(g_cdt, &entries);
    if (!count)
        return;
    logo_output outputs[CDT_MAX_LOGOS * 2];
    size_t n = 0;
    for (size_t i = 0; i < count && n < sizeof(outputs) / sizeof(outputs[0]); i++) {
        if (!entries[i].png_length || entries[i].png_length <= LOGO_STORE_TRANSPARENT_MAX)
            continue;
        for (size_t s = 0; s < entries[i].service_count; s++) {
            outputs[n++] = (logo_output){
                .id = (uint32_t)entries[i].services[s].original_network_id * 100000u +
                      entries[i].services[s].service_id,
                .network_id = entries[i].services[s].original_network_id,
                .transport_stream_id = entries[i].services[s].transport_stream_id,
                .service_id = entries[i].services[s].service_id,
                .name = cdt_collector_name(g_cdt, entries[i].services[s].service_id),
                .png = entries[i].png,
                .png_length = entries[i].png_length,
                .logo_type = entries[i].logo_type,
                .logo_id = entries[i].logo_id,
            };
        }
    }
    if (!n)
        return;
    int changed = logo_store_write(g_logo_dir, outputs, n, "terrestrial", 0, 0);
    if (changed > 0) {
        fprintf(stderr, "eit_watch: logos written (%zu service(s), %d change(s))\n", n, changed);
        g_logo_written = g_cdt->generation;
    } else if (changed == 0) {
        g_logo_written = g_cdt->generation;
    }
}

int main(int argc, char **argv)
{
    const char *state_path = "/dev/shm/pix/program.txt";
    g_epg_path = "/dev/shm/pix/epg.json";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--state") && i + 1 < argc)
            state_path = argv[++i];
        else if (!strcmp(argv[i], "--epg") && i + 1 < argc)
            g_epg_path = argv[++i];
        else if (!strcmp(argv[i], "--logo-dir") && i + 1 < argc)
            g_logo_dir = argv[++i];
        else if (!strcmp(argv[i], "--logo-debug"))
            g_logo_debug = 1;
        else {
            fprintf(stderr, "usage: %s [--state PATH] [--epg PATH] [--logo-dir DIR] [--logo-debug]\n", argv[0]);
            return 2;
        }
    }

    if (g_logo_dir && g_logo_dir[0]) {
        g_cdt = calloc(1, sizeof(*g_cdt));
        if (!g_cdt) {
            fprintf(stderr, "eit_watch: out of memory for the logo collector\n");
            return 1;
        }
        cdt_collector_init(g_cdt);
        g_cdt->debug = logo_debug;
        if (logo_store_mkdirs(g_logo_dir))
            fprintf(stderr, "eit_watch: cannot create %s: %s\n", g_logo_dir, strerror(errno));
    }

    ts_demux program_demux;
    ts_demux_init(&program_demux);
    ts_demux_add(&program_demux, 0);
    ts_demux_add(&program_demux, EIT_PID);

    load_state(state_path);
    fprintf(stderr, "eit_watch: ready event-offset=14 state=%s epg=%s (previous=%s)\n",
            state_path, g_epg_path, g_last.valid ? g_last.name : "-");

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

        /*
         * 下流を待たせないよう、まず書いてから解析する。解析を先にすると、
         * 復号器の出力が詰まってカードのやり取りまで巻き添えになる。
         */
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

        ts_demux_feed(&program_demux, in, done, program_section, (void *)state_path);

        epg_flush(0);

        if (g_cdt) {
            cdt_collector_feed(g_cdt, in, done);
            static time_t last_flush;
            time_t now = time(NULL);
            if (now - last_flush >= 30) {
                last_flush = now;
                logos_flush();
                if (now >= (time_t)g_logo_progress_next) {
                    char progress[192];
                    g_logo_progress_next = (unsigned)now + 300;
                    fprintf(stderr, "eit_watch: %s\n",
                            cdt_collector_progress(g_cdt, progress, sizeof(progress)));
                }
            }
        }

        memmove(in, in + done, have - done);
        have -= done;
        if (have == sizeof(in))
            have = 0;                       /* 188 で割れないまま満杯: 捨てる */
    }
    logos_flush();
    epg_flush(1);
    if (g_cdt) {
        cdt_collector_free(g_cdt);
        free(g_cdt);
    }
    return 0;
}
