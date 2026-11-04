/*
 * AccentColor -- see the header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "AccentColor.h"

#include "colecosession.h"

NSColor *COLECOAccentColor(void)
{
    return [NSColor colorWithSRGBRed:((COLECOSESSION_ACCENT_RGB >> 16) & 0xff) / 255.0
                               green:((COLECOSESSION_ACCENT_RGB >> 8) & 0xff) / 255.0
                                blue:(COLECOSESSION_ACCENT_RGB & 0xff) / 255.0
                               alpha:1.0];
}

void COLECOSetButtonAccent(NSButton *button, BOOL on)
{
    if (!button)
        return;
    NSString *title = [button.title copy];
    if (on) {
        NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
        para.alignment = NSTextAlignmentCenter;
        button.bezelColor = COLECOAccentColor();
        button.attributedTitle = [[NSAttributedString alloc]
            initWithString:title
                attributes:@{
                    NSFontAttributeName : button.font ?: [NSFont systemFontOfSize:NSFont.systemFontSize],
                    NSForegroundColorAttributeName : NSColor.whiteColor,
                    NSParagraphStyleAttributeName : para
                }];
    } else {
        button.bezelColor = nil;
        button.title = title; /* drops the attributed title */
    }
}
