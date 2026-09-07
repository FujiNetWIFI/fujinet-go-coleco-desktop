/*
 * ColecoKeypadWindow -- the clickable controller panel, showing both hand
 * controllers side by side so a mouse alone can drive every button the
 * machine has.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "colecosession.h"

/* Toggles visibility: shows the singleton window (creating it on first
 * call), or hides it if already showing. `parent` is only used the first
 * time, to set the transient-for relationship. */
void coleco_keypad_window_toggle(GtkWindow *parent, colecosession *session);
gboolean coleco_keypad_window_is_visible(void);
