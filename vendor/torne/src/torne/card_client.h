/* SPDX-License-Identifier: MIT */
#ifndef TORNE_CARD_CLIENT_H
#define TORNE_CARD_CLIENT_H
#include <stdint.h>
typedef struct { int fd; } CardClient;
int card_client_open(CardClient *,const char *);
int card_client_reset(CardClient *);
int card_client_transmit(CardClient *,const uint8_t *,unsigned,uint8_t *,unsigned *);
void card_client_close(CardClient *);
#endif
