/* SPDX-License-Identifier: MIT */
#ifndef TORNE_TS_STATS_H
#define TORNE_TS_STATS_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef struct {
    int16_t cc[8192];
    uint64_t packets,tei,cc_errors,duplicates,scrambled;
    const char *stage;
    FILE *trace;
} TsStats;
void ts_stats_init(TsStats *,const char *);
void ts_stats_feed(TsStats *,const uint8_t *,size_t);
void ts_stats_print(const TsStats *);
#endif
