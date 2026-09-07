/*
 * The emulator display.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "colecosession.h"

@interface ColecoDisplayView : NSView
- (instancetype)initWithSession:(colecosession *)session;
- (void)setTvAspect:(BOOL)tv;
- (void)setSmooth:(BOOL)smooth;
- (void)stop;
@end
