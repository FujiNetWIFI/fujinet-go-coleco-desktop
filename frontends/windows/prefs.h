/*
 * The Preferences window -- see prefs.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef COLECO_WIN_PREFS_H
#define COLECO_WIN_PREFS_H

#include "win_common.h"
#include "colecosession.h"

/* What the window asks of the main window. */
typedef struct {
    /* Import BIOS (from a folder when folder is set): the main window runs
     * the picker and restarts the session if the machine had none */
    void (*import_bios)(HWND owner, int folder);
    /* the window closed; dirty = a setting that needs a restart changed */
    void (*closed)(int dirty);
} coleco_prefs_hooks;

/* Shows the window, or raises it when it is open. */
void coleco_prefs_show(HWND parent, colecosession *session, const coleco_prefs_hooks *hooks);
/* Closing without applying anything (quitting): the settings are saved. */
void coleco_prefs_abandon(void);
/* An import made elsewhere: refresh what the window shows about the BIOS. */
void coleco_prefs_roms_changed(void);
/* The gamepad set changed. */
void coleco_prefs_gamepads_changed(void);
/* Tab and the arrow keys move between its controls; 1 if the message was used. */
int coleco_prefs_pretranslate(MSG *msg);

#endif
