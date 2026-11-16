/*
 * expr -- the debugger's expression evaluator: breakpoint conditions,
 * print, set, and every address argument the prompt takes.
 *
 *   numbers     $C000  0xC000  C000h  49152  %1010
 *   names       whatever the context resolves: Z80 registers and flags,
 *               labels, frame, cycles, vline, and value/addr/old for an access
 *   memory      [expr] the byte at that address, {expr} the 16-bit word
 *               (little-endian), both as the CPU sees them now
 *               and side-effect free
 *   operators   C's, with C's precedence: unary - ! ~, * / %, + -, << >>,
 *               < <= > >=, == !=, &, ^, |, &&, ||
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COLECO_EXPR_H
#define COLECO_EXPR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* A name to a value; return 1 if known. */
    int (*name)(void *user, const char *name, long *out);
    /* A byte of memory, without side effects. */
    uint8_t (*read_byte)(void *user, uint16_t addr);
    void *user;
} colecoexpr_ctx;

/* Evaluate; 0 and *out on success, -1 with a message otherwise. With a NULL
 * context, names evaluate to 0 and memory reads to 0 -- a syntax check. */
int colecoexpr_eval(const char *text, const colecoexpr_ctx *ctx, long *out, char *err, int errsz);

#ifdef __cplusplus
}
#endif

#endif /* COLECO_EXPR_H */
