/*
 * The emulator display: a GtkDrawingArea that pulls frames from the session
 * on the compositor's own frame clock.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "colecosession.h"

#define COLECO_TYPE_DISPLAY (coleco_display_get_type())
G_DECLARE_FINAL_TYPE(ColecoDisplay, coleco_display, COLECO, DISPLAY, GtkWidget)

GtkWidget *coleco_display_new(colecosession *session);
/* Square pixels (1:1) or the 4:3 a television actually showed. */
void coleco_display_set_tv_aspect(ColecoDisplay *self, gboolean tv);
void coleco_display_set_smooth(ColecoDisplay *self, gboolean smooth);
