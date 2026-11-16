/*
 * debugger_internal -- what the prompt (prompt.c), the machine getters
 * (iostate.c) and the memory tools (memtools.c) share with the engine
 * (debugger.c). Not installed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COLECO_DEBUGGER_INTERNAL_H
#define COLECO_DEBUGGER_INTERNAL_H

#include "adamcore.h"
#include "colecodebug.h"
#include "symbols.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The engine's core, for the getters in iostate.c. The caller must hold the
 * engine lock (colecodebug_lock) for as long as it uses the pointer: the
 * session may destroy the machine at any moment otherwise. NULL without a
 * machine. */
adamcore *colecodebug_core_locked(colecodebug *dbg);
void colecodebug_lock(colecodebug *dbg);
void colecodebug_unlock(colecodebug *dbg);

/* Waits (bounded) for the machine to be stopped at a stop newer than `serial`
 * (colecodebug_stop_serial before the command). 1 once stopped. For the
 * command line. */
int colecodebug_wait_stop(colecodebug *dbg, uint64_t serial, int timeout_ms);

/* Marks something a window should redraw for (a command ran). */
void colecodebug_touch(colecodebug *dbg);

/* The symbol tables, for completion (the caller must not keep the pointer
 * across a load). */
const sym_tables *colecodebug_symtabs(colecodebug *dbg);

/* The Video tab's text hook, or NULL (see colecodebug_set_video_text). */
const char *(*colecodebug_video_text_hook(void))(const char *section);

#ifdef __cplusplus
}
#endif

#endif /* COLECO_DEBUGGER_INTERNAL_H */
