/*
 * Debugger window for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "colecosession.h"

G_BEGIN_DECLS

/* Shows (creating on first use) the debugger window for the session. */
void coleco_debugger_show(GtkWindow *parent, colecosession *session);

/* Shows it if hidden or absent, closes it if showing: the main window's F12
 * and menu item. */
void coleco_debugger_toggle(GtkWindow *parent, colecosession *session);

G_END_DECLS
