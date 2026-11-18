/*
 * pix_dt295_logo.c - 局ロゴの受信
 *
 * BS/CS: 929 (BS-15) のカルーセルから。1 周に 20 分ほどかかる。
 *    pix-ts-legacy --model dt295 ... -s bs -c 15 --tsid 0x40f1 -t 120 \
 *      | pix-dt295-logo --out /var/lib/pix/logos --wait 120
 *
 * 地上波: 各局の TS の CDT から。ローカルの TS ファイルにも使える。
 *    pix-dt295-logo --cdt --out /var/lib/pix/logos --wait 600 < ch17.ts
 *
 * どちらも TS を読むだけでデバイスには触らない。ロゴがそろったら待たずに終わる。
 * 出力は 1 サービス 1 ファイルの PNG と index.json で、ファイル名は mirakc と
 * 同じサービス ID (original_network_id * 100000 + service_id)。NAS 側の CM カットは
 * service.id で局を引くので、そのまま対応する。
 */
#define _POSIX_C_SOURCE 200809L
#include "arib_text.h"
#include "logo_carousel.h"
#include "logo_cdt.h"
#include "logo_store.h"
#include "ts_section.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SID 929
#define DEFAULT_WAIT 120

static int verbose = 1;

static void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_line(const char *fmt, ...) {
    if (!verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void usage(FILE *f) {
    fprintf(f,
            "usage: pix-dt295-logo --out DIR [options] < ts\n"
            "\n"
            "  --out DIR        write <service id>.png, <service id>.json and index.json\n"
            "  --cdt            terrestrial: take the logos from each station's CDT\n"
            "                   (default is the BS/CS carousel of service 929)\n"
            "  --wait SEC       give up after this many seconds (default %d)\n"
            "  --sid N          engineering service's program number (default %d)\n"
            "  --system NAME    bs / cs / terrestrial, for the index (default: bs, or\n"
            "                   terrestrial with --cdt)\n"
            "  --if-hz HZ       tuning frequency, for the index\n"
            "  --tsid N         transport_stream_id, for the index\n"
            "  --keep-dummies   keep the transparent placeholders (size <= %d)\n"
            "  --quiet          no progress on stderr\n"
            "  -h, --help       this help\n"
            "\n"
            "Exit: 0 logos written or already current, 3 nothing received, 2 usage, 1 I/O.\n",
            DEFAULT_WAIT, DEFAULT_SID, LOGO_STORE_TRANSPARENT_MAX);
}

/* One PNG per service: the most preferred non-transparent logo type it has. */
static size_t build_outputs(const logo_entry *entries, size_t entry_count, int keep_dummies,
                            logo_output **outputs) {
    logo_output *result = NULL;
    size_t count = 0, capacity = 0;
    for (size_t i = 0; i < entry_count; i++) {
        const logo_entry *entry = &entries[i];
        int real = entry->png_length > LOGO_STORE_TRANSPARENT_MAX;
        if (!real && !keep_dummies)
            continue;
        unsigned rank = logo_type_rank(entry->logo_type);
        for (size_t s = 0; s < entry->service_count; s++) {
            uint32_t id = (uint32_t)entry->services[s].original_network_id * 100000u +
                          entry->services[s].service_id;
            logo_output *held = NULL;
            for (size_t k = 0; k < count; k++)
                if (result[k].id == id) {
                    held = &result[k];
                    break;
                }
            if (held) {
                int held_real = held->png_length > LOGO_STORE_TRANSPARENT_MAX;
                if (real && (!held_real || rank < logo_type_rank(held->logo_type))) {
                    held->png = entry->png;
                    held->png_length = entry->png_length;
                    held->logo_type = entry->logo_type;
                    held->logo_id = entry->logo_id;
                }
                continue;
            }
            if (count == capacity) {
                size_t grown = capacity ? capacity * 2 : 32;
                logo_output *bigger = realloc(result, grown * sizeof(*result));
                if (!bigger)
                    break;
                result = bigger;
                capacity = grown;
            }
            result[count++] = (logo_output){
                .id = id,
                .network_id = entry->services[s].original_network_id,
                .transport_stream_id = entry->services[s].transport_stream_id,
                .service_id = entry->services[s].service_id,
                .png = entry->png,
                .png_length = entry->png_length,
                .logo_type = entry->logo_type,
                .logo_id = entry->logo_id,
            };
        }
    }
    *outputs = result;
    return count;
}

static int compare_outputs(const void *a, const void *b) {
    const logo_output *x = a, *y = b;
    return x->id < y->id ? -1 : x->id > y->id;
}

static int read_stdin(void *collector, int cdt, unsigned wait_seconds) {
    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);
    uint8_t buffer[65536];
    time_t last_report = 0;
    int done = 0;
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        time_t elapsed = now.tv_sec - started.tv_sec;
        if ((unsigned)elapsed >= wait_seconds)
            break;
        struct pollfd fd = {STDIN_FILENO, POLLIN, 0};
        int ready = poll(&fd, 1, 200);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ready == 0)
            continue;
        ssize_t n = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        if (cdt)
            cdt_collector_feed(collector, buffer, (size_t)n);
        else
            logo_collector_feed(collector, buffer, (size_t)n);
        int complete = cdt ? cdt_collector_done(collector) : logo_collector_done(collector);
        if (complete) {
            done = 1;
            break;
        }
        if (verbose && elapsed >= last_report + 15) {
            char progress[192];
            last_report = elapsed;
            if (cdt)
                log_line("{\"event\":\"logo_progress\",\"s\":%ld,\"detail\":\"%s\"}", (long)elapsed,
                         cdt_collector_progress(collector, progress, sizeof(progress)));
            else
                log_line("{\"event\":\"logo_progress\",\"s\":%ld,\"detail\":\"%s\"}", (long)elapsed,
                         logo_collector_progress(collector, progress, sizeof(progress)));
        }
    }
    return done;
}

int main(int argc, char **argv) {
    const char *out_dir = NULL, *system = NULL;
    unsigned wait_seconds = DEFAULT_WAIT;
    unsigned sid = DEFAULT_SID;
    unsigned long if_hz = 0, tsid = 0;
    int keep_dummies = 0, cdt = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *value = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--out") && value) {
            out_dir = value;
            i++;
        } else if (!strcmp(a, "--wait") && value) {
            wait_seconds = (unsigned)strtoul(value, NULL, 10);
            i++;
        } else if (!strcmp(a, "--sid") && value) {
            sid = (unsigned)strtoul(value, NULL, 10);
            i++;
        } else if (!strcmp(a, "--system") && value) {
            system = value;
            i++;
        } else if (!strcmp(a, "--if-hz") && value) {
            if_hz = strtoul(value, NULL, 10);
            i++;
        } else if (!strcmp(a, "--tsid") && value) {
            tsid = strtoul(value, NULL, 0);
            i++;
        } else if (!strcmp(a, "--keep-dummies")) {
            keep_dummies = 1;
        } else if (!strcmp(a, "--cdt")) {
            cdt = 1;
        } else if (!strcmp(a, "--quiet")) {
            verbose = 0;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "%s: unknown option %s\n", argv[0], a);
            usage(stderr);
            return 2;
        }
    }
    if (!out_dir) {
        usage(stderr);
        return 2;
    }
    if (!system)
        system = cdt ? "terrestrial" : "bs";

    logo_collector *bs = NULL;
    cdt_collector *terrestrial = NULL;
    void *collector;
    if (cdt) {
        terrestrial = calloc(1, sizeof(*terrestrial));
        if (!terrestrial) {
            fprintf(stderr, "%s: out of memory\n", argv[0]);
            return 1;
        }
        cdt_collector_init(terrestrial);
        collector = terrestrial;
    } else {
        bs = calloc(1, sizeof(*bs));
        if (!bs) {
            fprintf(stderr, "%s: out of memory\n", argv[0]);
            return 1;
        }
        logo_collector_init(bs, (uint16_t)sid);
        collector = bs;
    }

    int done = read_stdin(collector, cdt, wait_seconds);

    const logo_entry *entries = NULL;
    size_t entry_count = cdt ? cdt_collector_entries(terrestrial, &entries)
                             : logo_collector_entries(bs, &entries);
    char progress[192];
    log_line("{\"event\":\"logo_end\",\"done\":%d,\"entries\":%zu,\"detail\":\"%s\"}", done,
             entry_count, cdt ? cdt_collector_progress(terrestrial, progress, sizeof(progress))
                              : logo_collector_progress(bs, progress, sizeof(progress)));

    logo_output *outputs = NULL;
    size_t output_count = build_outputs(entries, entry_count, keep_dummies, &outputs);
    /* Attach the service names from the SDT (best effort; the CDT path has no
     * names of its own yet). */
    if (!cdt)
        for (size_t i = 0; i < output_count; i++)
            outputs[i].name = logo_collector_name(bs, outputs[i].service_id);
    qsort(outputs, output_count, sizeof(*outputs), compare_outputs);

    int rc;
    if (output_count) {
        int changed = logo_store_write(out_dir, outputs, output_count, system, if_hz, tsid);
        if (changed < 0) {
            fprintf(stderr, "%s: cannot write %s: %s\n", argv[0], out_dir, strerror(errno));
            rc = 1;
        } else {
            log_line("{\"event\":\"logo_summary\",\"logos\":%zu,\"changed\":%d}", output_count,
                     changed);
            rc = 0;
        }
    } else {
        log_line("{\"event\":\"logo_summary\",\"logos\":0,\"changed\":0}");
        rc = 3;
    }

    free(outputs);
    if (terrestrial) {
        cdt_collector_free(terrestrial);
        free(terrestrial);
    }
    if (bs) {
        logo_collector_free(bs);
        free(bs);
    }
    return rc;
}
