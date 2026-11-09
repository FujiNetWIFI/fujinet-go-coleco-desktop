/*
 * The Win32 Controllers window (F9): both ColecoVision hand controllers drawn
 * as a player sees them -- stick, side buttons and keypad -- with the console
 * buttons, the gamepads and their assignments, and Map mode, in a fixed-size
 * floating tool window. Hidden at start.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <windows.h>

#include "colecosession.h"

/* Show / hide (F9). Created on first use, hidden not destroyed after. */
void coleco_controllers_window_toggle(HWND parent, colecosession *session);
/* Show it (and raise it) if it is not showing. */
void coleco_controllers_window_show(HWND parent, colecosession *session);
int  coleco_controllers_window_visible(void);

/* The gamepad set, the bindings or the machine changed: refresh. */
void coleco_controllers_window_refresh(void);

/* Map mode is armed: the next host key is a binding, not typing. The main
 * window gives its keys here first; 1 if consumed. */
int coleco_controllers_map_key(UINT msg, WPARAM wparam, LPARAM lparam);
