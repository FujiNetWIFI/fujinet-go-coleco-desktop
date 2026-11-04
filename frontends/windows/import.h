/*
 * File and folder choosers for the BIOS -- see import.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef COLECO_WIN_IMPORT_H
#define COLECO_WIN_IMPORT_H

#include <windows.h>

#include "colecosession.h"

/* Asks for BIOS files (several, zips included) or a folder, imports each,
 * and says in one message box what came of it. Returns how many ROM items
 * were imported (0: nothing recognised), or -1 when the chooser was
 * cancelled. */
int coleco_import_bios(HWND owner, colecosession *session, int folder);

/* Imports dropped or named files and folders the same way, one message box
 * for the lot. Returns how many ROM items came in. */
int coleco_import_roms(HWND owner, colecosession *session,
                       const char *const *paths, int n);

#endif
