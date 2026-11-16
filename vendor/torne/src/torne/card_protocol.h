/* SPDX-License-Identifier: MIT */
#ifndef TORNE_CARD_PROTOCOL_H
#define TORNE_CARD_PROTOCOL_H
#include <stdint.h>
enum { TORNE_CARD_RESET=1,TORNE_CARD_TRANSMIT=2 };
typedef struct { uint32_t operation,size;int32_t status;uint8_t data[512]; } TorneCardMessage;
#endif
