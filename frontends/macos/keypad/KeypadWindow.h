/*
 * The clickable controller panel: both hand controllers side by side.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "colecosession.h"

@interface ColecoKeypadWindow : NSWindowController
+ (void)toggleWithSession:(colecosession *)session;
+ (BOOL)isVisible;
@end
