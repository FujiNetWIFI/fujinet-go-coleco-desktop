/*
 * FujiNet trace (internal to the colecodebug engine): the cartridge's bytes,
 * both ways, SLIP-decoded into FujiBus packets. Pure -- no machine, no
 * threads -- so fujitrace_test drives it with synthetic bytes; the engine
 * owns one and holds a lock around every call.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COLECODEBUG_FUJITRACE_H
#define COLECODEBUG_FUJITRACE_H

#include <stddef.h>
#include <stdint.h>

#include "colecodebug.h"

#define FUJITRACE_MAX 512
#define FUJITRACE_FRAME_MAX 4096 /* longer frames keep their first 4K */

/* All zero is an empty trace. */
typedef struct {
    coleco_fuji_packet ring[FUJITRACE_MAX];
    uint32_t hash[FUJITRACE_MAX]; /* of every byte, for folding repeats */
    int head, count;              /* oldest at head */
    uint64_t serial;
    int pending;                  /* a repeated exchange, half seen */

    /* per direction (index = to_host): the frame being decoded */
    uint8_t acc[2][FUJITRACE_FRAME_MAX];
    int acclen[2];
    int esc[2];

    /* since the last clear, repeats included */
    uint32_t packets[2];
    uint64_t bytes[2];
} fujitrace;

/* dir 1 is ColecoVision -> FujiNet, the bytes actually sent; dir 0 is
 * FujiNet -> ColecoVision. Frames may arrive in any number of pieces. */
void fujitrace_feed(fujitrace *t, int dir, const uint8_t *buf, size_t len);
/* The newest max packets, oldest first. */
int  fujitrace_read(const fujitrace *t, coleco_fuji_packet *out, int max);
/* Drops the packets and counters; a frame half decoded stays. */
void fujitrace_clear(fujitrace *t);
/* "READ HOST SLOTS", or "" for an id the firmware does not name. */
const char *fujitrace_command_name(int id);

#endif /* COLECODEBUG_FUJITRACE_H */
