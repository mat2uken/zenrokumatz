/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TORNE_CARD_SERVER_H
#define TORNE_CARD_SERVER_H
#include "torne_core.h"
typedef struct CardServer CardServer;
CardServer *card_server_open(struct torne_bridge *,const char *);
int card_server_start(CardServer *);
void card_server_wait_idle(CardServer *,unsigned);
unsigned card_server_commands(CardServer *);
void card_server_close(CardServer *);
#endif
