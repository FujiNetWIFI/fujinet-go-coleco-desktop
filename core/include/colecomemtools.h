/*
 * Watches, range parsing, memory-search queries and breakpoint descriptions,
 * kept in the core so the debugger windows only lay them out.
 *
 * A watch is text the user typed (an address, a label, an expression, or a
 * range low-high / low..high) shown with its live value. It is stored as text and
 * resolved every time it is shown, so loading symbols later makes a label
 * start resolving.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef COLECOMEMTOOLS_H
#define COLECOMEMTOOLS_H

#include <stdint.h>

#include "colecodebug.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "low..high", "low-high" (plain numbers or labels on both sides), or one
 * expression (an address, label, label+3, [ptr]). A bare token of 1-4 hex
 * digits ("C000") is hex unless it names a label; "$C000" and expressions
 * work as in the console. Returns 1 and fills low and high (inclusive,
 * low <= high), 0 if the text means nothing. */
int colecosym_parse_range(colecodebug *dbg, const char *text, uint16_t *low, uint16_t *high);

/* ---- watches ---- */
#define COLECOWATCH_MAX 64
/* The process-wide list, like the symbol tables: it outlives the window. */
int  colecowatch_count(void);
/* Returns the new index, or -1 if empty / full. A watch that does not resolve
 * yet is kept (it shows "?"). */
int  colecowatch_add(const char *text);
void colecowatch_remove(int index);
void colecowatch_clear(void);
const char *colecowatch_text(int index);
unsigned colecowatch_generation(void);
/* "score  $C010  $42 (66)". While the machine runs the value is "-"; a range
 * shows its first 8 bytes. Returns the length. */
int  colecowatch_format(colecodebug *dbg, int index, char *dst, int dstsz);
/* The range a watch covers (for "show in Memory"). 1 if it resolves. */
int  colecowatch_range(colecodebug *dbg, int index, uint16_t *low, uint16_t *high);

/* ---- memory search ---- */
enum { COLECOSEARCH_HEX = 0, COLECOSEARCH_TEXT };
#define COLECOSEARCH_MAX 32
/* Hex: byte pairs, optional "$" / "0x" / spaces. Text: one byte per
 * character, case-insensitive. Fills pat, count, mask (0xFF) and flags for
 * colecodebug_search. Returns 1, or 0 for empty / malformed text. */
int colecosearch_parse(int kind, const char *text, uint8_t *pat, int *count, uint8_t *mask, int *flags);

/* ---- breakpoints ---- */
/* "Write  $C010 (score)  if value != old". Returns the length. */
int colecobp_describe(colecodebug *dbg, const colecodebug_breakpoint *bpt, char *dst, int dstsz);

/* ---- memory map ---- */
/* The name of the region of `map` (from colecodebug_memory_map) holding addr
 * ("Main RAM (slot 3-2)"), for labelling Memory rows. Fetch the map once per
 * refresh and look rows up in it. Returns the length, 0 and "" when no region
 * covers addr. */
int colecomem_region_name(const colecodebug_region *map, int count, uint16_t addr, char *dst, int dstsz);

#ifdef __cplusplus
}
#endif

#endif /* COLECOMEMTOOLS_H */
