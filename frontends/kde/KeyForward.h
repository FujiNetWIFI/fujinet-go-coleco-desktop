/*
 * Qt key events to X11/xkb keysyms.
 *
 * The session's input table is written in keysyms because a GDK keyval
 * already IS one, so the GNOME frontend needs no translation at all. Qt is
 * the frontend that has to map, and doing it here rather than teaching the
 * table a second vocabulary is what lets ONE tested table serve every
 * frontend.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QKeyEvent>
#include <cstdint>

/* Returns the keysym, or 0 when the key has no equivalent worth forwarding. */
uint32_t colecoKeysymFromQt(const QKeyEvent *e);
