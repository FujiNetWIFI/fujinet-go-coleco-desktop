/*
 * The Controllers window (F9): the two ColecoVision hand controllers drawn as
 * a player sees them -- stick, side fire buttons and 12-key keypad -- with the
 * gamepads and their assignments, and Map mode for rebinding any control.
 * Hidden at start; a fixed-size floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

#include "colecosession.h"

/* Shows the window, or hides it when it is showing. */
void colecoControllersToggle(QWidget *parent, colecosession *session);
/* Shows (and raises) the window; leaves it showing if it already is. */
void colecoControllersShow(QWidget *parent, colecosession *session);
bool colecoControllersVisible();
/* Preferences closed or the session restarted: redraw. */
void colecoControllersRefresh();
