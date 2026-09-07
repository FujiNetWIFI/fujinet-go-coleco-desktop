/*
 * The main application window.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "colecosession.h"

#define COLECO_TYPE_WINDOW (coleco_window_get_type())
G_DECLARE_FINAL_TYPE(ColecoWindow, coleco_window, COLECO, WINDOW,
                     AdwApplicationWindow)

GtkWidget *coleco_window_new(AdwApplication *app, colecosession *session);
