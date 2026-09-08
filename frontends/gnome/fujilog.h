/*
 * The FujiNet console log window.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "colecosession.h"

G_BEGIN_DECLS

/* Shows (raising an existing one) the FujiNet console log window. */
void coleco_fujilog_show(GtkWindow *parent, colecosession *session);

G_END_DECLS
