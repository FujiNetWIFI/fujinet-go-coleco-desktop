/*
 * The app's accent colour for the macOS frontend (COLECOSESSION_ACCENT_RGB,
 * the icon's background): held pad buttons, the armed Map button and the
 * remap target. It is the TMS9918's dark blue, so text on it is white.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

NSColor *COLECOAccentColor(void);

/* Toggled-on look for a push button: accent bezel and a white title. Off
 * restores the plain bezel and title. Call again after setting a new title
 * on a button that is on (a plain title drops the attributed one). */
void COLECOSetButtonAccent(NSButton *button, BOOL on);
