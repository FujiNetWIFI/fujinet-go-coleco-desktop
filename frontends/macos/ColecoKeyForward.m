/*
 * ColecoKeyForward -- see the header.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ColecoKeyForward.h"

#import <Carbon/Carbon.h>   /* kVK_* virtual key codes */

#include "colecosession.h"

uint32_t ColecoKeysymFromEvent(NSEvent *event)
{
    const uint32_t keysym = colecosession_keysym_from_macos_keycode([event keyCode]);
    if (keysym) return keysym;

    switch ([event keyCode]) {
    case kVK_UpArrow:    return COLECOSESSION_KEYSYM_UP;
    case kVK_DownArrow:  return COLECOSESSION_KEYSYM_DOWN;
    case kVK_LeftArrow:  return COLECOSESSION_KEYSYM_LEFT;
    case kVK_RightArrow: return COLECOSESSION_KEYSYM_RIGHT;
    case kVK_Tab:        return COLECOSESSION_KEYSYM_TAB;
    case kVK_Escape:     return COLECOSESSION_KEYSYM_ESCAPE;
    case kVK_Return:     return COLECOSESSION_KEYSYM_RETURN;
    case kVK_Space:      return COLECOSESSION_KEYSYM_SPACE;
    case kVK_Delete:     return COLECOSESSION_KEYSYM_BACKSPACE;
    default: break;
    }

    /* Anything the table does not know: the character typed. */
    NSString *chars = [event charactersIgnoringModifiers];
    if ([chars length] == 1) {
        const unichar chr = [chars characterAtIndex:0];
        if (chr >= 0x20 && chr <= 0x7e) {
            if (chr >= 'A' && chr <= 'Z') return (uint32_t)(chr - 'A' + 'a');
            return (uint32_t)chr;
        }
        if (chr >= NSF1FunctionKey && chr <= NSF12FunctionKey)
            return COLECOSESSION_KEYSYM_F1 + (uint32_t)(chr - NSF1FunctionKey);
    }
    return 0;
}

/* The device-dependent bits of an event's modifier flags, one per side (IOKit's
 * NX_DEVICE*KEYMASK): the pair's own flag stays set while either side is held,
 * so a release of one with the other down would read as a press. */
#define COLECO_DEV_LCTL   0x00000001UL
#define COLECO_DEV_LSHIFT 0x00000002UL
#define COLECO_DEV_RSHIFT 0x00000004UL
#define COLECO_DEV_LALT   0x00000020UL
#define COLECO_DEV_RALT   0x00000040UL
#define COLECO_DEV_RCTL   0x00002000UL
#define COLECO_DEV_ANY    (COLECO_DEV_LCTL | COLECO_DEV_LSHIFT | COLECO_DEV_RSHIFT | \
                           COLECO_DEV_LALT | COLECO_DEV_RALT | COLECO_DEV_RCTL)

/* Whether a modifier key is down: its group flag is set and, when the event
 * carries the per-side bits (a synthetic one may not), its own bit is. */
static int modifierDown(NSUInteger flags, NSUInteger group, NSUInteger side)
{
    if (!(flags & group)) return 0;
    if (!(flags & COLECO_DEV_ANY)) return 1;
    return (flags & side) != 0;
}

/* Modifier keys arrive as flagsChanged, not keyDown/keyUp. The key code says
 * which key; its flag says whether it went down. Both fire buttons are
 * modifiers in the default bindings (Left Ctrl, Left Alt = Option). */
uint32_t ColecoKeysymFromFlagsChange(NSEvent *event, int *down)
{
    const NSUInteger flags = (NSUInteger)[event modifierFlags];
    switch ([event keyCode]) {
    case kVK_Shift:
        *down = modifierDown(flags, NSEventModifierFlagShift, COLECO_DEV_LSHIFT);
        return COLECOSESSION_KEYSYM_LSHIFT;
    case kVK_RightShift:
        *down = modifierDown(flags, NSEventModifierFlagShift, COLECO_DEV_RSHIFT);
        return COLECOSESSION_KEYSYM_RSHIFT;
    case kVK_Control:
        *down = modifierDown(flags, NSEventModifierFlagControl, COLECO_DEV_LCTL);
        return COLECOSESSION_KEYSYM_LCTRL;
    case kVK_RightControl:
        *down = modifierDown(flags, NSEventModifierFlagControl, COLECO_DEV_RCTL);
        return COLECOSESSION_KEYSYM_RCTRL;
    /* Option is where a Mac keyboard puts the key a PC calls Alt, which is
     * the second fire button in the default bindings. */
    case kVK_Option:
        *down = modifierDown(flags, NSEventModifierFlagOption, COLECO_DEV_LALT);
        return COLECOSESSION_KEYSYM_LALT;
    case kVK_RightOption:
        *down = modifierDown(flags, NSEventModifierFlagOption, COLECO_DEV_RALT);
        return COLECOSESSION_KEYSYM_RALT;
    default:
        *down = 0;
        return 0;
    }
}
