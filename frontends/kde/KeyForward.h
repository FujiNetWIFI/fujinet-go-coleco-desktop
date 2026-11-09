/*
 * Qt key events to X11/xkb keysyms.
 *
 * The session's bindings are keyed by the PHYSICAL key -- the unshifted
 * US-layout symbol of the hardware key -- so that a modifier cannot change
 * what a key means (with NumLock off the numeric keypad still reports
 * Keypad 5, not End's cursor symbol). The route is the hardware scan code
 * (evdev on Linux) through the session's HID table, with a Qt::Key fallback
 * for anything the table has no usage for.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QKeyEvent>
#include <cstdint>

/* Returns the keysym, or 0 when the key has no equivalent worth forwarding. */
uint32_t colecoKeysymFromQt(const QKeyEvent *evt);

/* The app's own keys: F9 (Controllers) and F12 (Debugger). They are never
 * sent to the console and cannot be bound. */
bool colecoIsAppKey(const QKeyEvent *evt);
