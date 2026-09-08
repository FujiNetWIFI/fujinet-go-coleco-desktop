/*
 * The application delegate: the window, the menu bar, and the session.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "colecosession.h"

/* NSWindowDelegate is declared, not cast to at the call site: the delegate
 * really does implement windowWillClose: (the Settings window applies its
 * machine options there), and declaring it is what lets the compiler check
 * that. */
@interface ColecoAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
- (instancetype)initWithSession:(colecosession *)session
                       cartPath:(const char *)cartPath;
@end
