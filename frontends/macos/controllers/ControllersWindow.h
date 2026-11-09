/*
 * The Controllers panel (F9 / View menu): the ColecoVision's two hand
 * controllers drawn from the session's layout (colecosession_controls_*), the
 * console strip, the gamepads with their assignments, and Map mode for
 * rebinding. Hidden at start; a fixed-size floating NSPanel.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "colecosession.h"

/* The keys the app itself owns (F9, F12, a console action's key): called with
 * the keysym of one typed with the panel key; returns YES when it took it. */
typedef BOOL (^ColecoAppKeyHandler)(uint32_t keysym);

@interface ColecoControllersWindow : NSObject
- (instancetype)initWithSession:(colecosession *)session appKeys:(ColecoAppKeyHandler)appKeys;
/* Runs a console action the strip's buttons fire (COLECO_SYSACT_*); the app
 * sets it so the action also shows its toast. Without one the session runs it. */
@property (nonatomic, copy) void (^sysactionHandler)(int sysact);
/* Show, or hide when showing. */
- (void)toggle;
- (void)show;
- (void)hide;
- (BOOL)isVisible;
/* The settings changed (Preferences): pads and bindings are read again. */
- (void)refresh;
@end
