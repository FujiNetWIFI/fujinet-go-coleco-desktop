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

G_END_DECLS
