/*
 * AppKit key events to X11/xkb keysyms.
 *
 * Same job as the Qt and Win32 frontends' equivalents: the session's input
 * table is written in keysyms because a GDK keyval already IS one, so every
 * other frontend maps into that one tested table rather than growing a
 * vocabulary of its own.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>
#include <stdint.h>

/* Returns the keysym, or 0 when the key has no equivalent worth forwarding. */
uint32_t ColecoKeysymFromEvent(NSEvent *event);
