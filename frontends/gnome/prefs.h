/*
 * Preferences dialog for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "colecosession.h"

G_BEGIN_DECLS

typedef struct _ColecoWindow ColecoWindow;

/* Shows the preferences dialog. Every option here is a MACHINE option --
 * they are read by colecosession_default_opts() when the session starts --
 * so the dialog restarts the session on close if any of them changed. The
 * display options (TV aspect, smooth scaling) are live toggles and stay in
 * the View menu where they can be flipped while watching the picture. */
void coleco_prefs_show(ColecoWindow *parent, colecosession *session,
                       void (*restart)(ColecoWindow *parent));

G_END_DECLS
