/*
 * FujiNet Go ColecoVision -- the macOS (AppKit) frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#import "AppDelegate.h"

#include "colecosession.h"

int main(int argc, const char **argv)
{
    @autoreleasepool {
        colecosession *session = colecosession_new(NULL);
        if (!session) {
            NSLog(@"Could not create the session (unusable config or data "
                   "directories?)");
            return 1;
        }

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        ColecoAppDelegate *delegate = [[ColecoAppDelegate alloc]
            initWithSession:session
                   cartPath:(argc > 1 ? argv[1] : NULL)];
        [app setDelegate:delegate];
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
