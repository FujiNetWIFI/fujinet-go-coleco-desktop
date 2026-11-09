/*
 * AppKit key events to X11/xkb keysyms, by hardware key code through the
 * session's HID table: the same physical-key route the other frontends take,
 * so a binding names the key whatever Shift or the layout is doing and the
 * numeric keypad is Keypad n, not the cursor keys, whatever Num Lock says.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>
#include <stdint.h>

/* Returns the keysym, or 0 when the key has no equivalent worth forwarding. */
uint32_t ColecoKeysymFromEvent(NSEvent *event);

/* The keysym for a modifier flag change (flagsChanged), or 0. Sets *down. */
uint32_t ColecoKeysymFromFlagsChange(NSEvent *event, int *down);
