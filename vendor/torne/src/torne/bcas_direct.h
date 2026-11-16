/* SPDX-License-Identifier: Apache-2.0 */
#ifndef TORNE_BCAS_DIRECT_H
#define TORNE_BCAS_DIRECT_H
#include "card_client.h"
#include "b_cas_card.h"
typedef struct {
    B_CAS_CARD api;
    CardClient client;
    B_CAS_INIT_STATUS status;
    int64_t ids[16];
    int id_count;
    unsigned commands;
} DirectBcas;
int direct_bcas_init(DirectBcas *,const char *);
#endif
