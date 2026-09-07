/*
 * The clickable controller panel: both hand controllers side by side.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <windows.h>

#include "colecosession.h"

/* Shows the singleton panel (creating it on first call), or hides it if
 * already showing. */
void coleco_keypad_window_toggle(HWND parent, colecosession *session);
