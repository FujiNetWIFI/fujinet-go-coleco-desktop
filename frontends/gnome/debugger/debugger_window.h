/*
 * ColecoDebuggerWindow -- the native debugger: a Z80 tab and a VDP tab over
 * the shared engine in core/debugger/.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "colecosession.h"

void coleco_debugger_window_toggle(GtkWindow *parent, colecosession *session);
