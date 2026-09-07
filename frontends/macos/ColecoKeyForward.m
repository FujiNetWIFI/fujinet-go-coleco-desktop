/*
 * ColecoKeyForward -- see the header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ColecoKeyForward.h"

#import <Carbon/Carbon.h>   /* kVK_* virtual key codes */

uint32_t ColecoKeysymFromEvent(NSEvent *event)
{
    switch ([event keyCode]) {
    case kVK_UpArrow:    return 0xFF52;
    case kVK_DownArrow:  return 0xFF54;
    case kVK_LeftArrow:  return 0xFF51;
    case kVK_RightArrow: return 0xFF53;
    case kVK_Tab:        return 0xFF09;
    case kVK_Escape:     return 0xFF1B;
    case kVK_Shift:
    case kVK_RightShift: return 0xFFE1;
    case kVK_Control:
    case kVK_RightControl: return 0xFFE3;
    /* Option is where a Mac keyboard puts the key a PC calls Alt, which is
     * the second fire button in the default bindings. */
    case kVK_Option:
    case kVK_RightOption: return 0xFFE9;
    case kVK_F5:         return 0xFFC2;
    case kVK_F6:         return 0xFFC3;
    default: break;
    }

    /* Digits, letters, * and # come from the characters, not the key code:
     * the key code is layout-independent, and a French or Dvorak keyboard
     * should bind what the user actually typed. The binding table folds case
     * and folds the numeric keypad onto the top row, so no extra work here. */
    NSString *chars = [event charactersIgnoringModifiers];
    if ([chars length] == 1) {
        const unichar c = [chars characterAtIndex:0];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || c == '*' || c == '#')
            return (uint32_t)c;
    }
    return 0;
}
