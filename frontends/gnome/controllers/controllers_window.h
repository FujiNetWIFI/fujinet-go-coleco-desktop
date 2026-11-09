/*
 * The Controllers window -- both ColecoVision hand controllers drawn as a
 * player sees them: the stick's disc ringed by eight direction wedges, a fire
 * button down either side and the twelve-key keypad on its overlay-card
 * frame. Held controls light up in the accent whoever holds them, the pointer
 * works every control, and Map mode rebinds any of them to a key or a gamepad
 * button.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "colecosession.h"

/* Toggles visibility: shows the singleton window (creating it on first
 * call), or hides it if already showing. `parent` is the main window; it is
 * only used the first time, to set the transient-for relationship, and keys
 * typed in this window go through it. */
void coleco_controllers_toggle(GtkWindow *parent, colecosession *session);
/* Shows it (or raises it). */
void coleco_controllers_show(GtkWindow *parent, colecosession *session);
gboolean coleco_controllers_visible(void);
