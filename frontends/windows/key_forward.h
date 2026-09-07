/*
 * Win32 virtual key codes to X11/xkb keysyms.
 *
 * Same job as the Qt frontend's KeyForward: the session's input table is
 * written in keysyms because a GDK keyval already IS one, so GNOME
 * translates nothing and every other frontend maps into that one tested
 * table rather than growing a vocabulary of its own.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdint.h>

static inline uint32_t coleco_keysym_from_vk(int vk)
{
    switch (vk) {
    case VK_UP:      return 0xFF52;
    case VK_DOWN:    return 0xFF54;
    case VK_LEFT:    return 0xFF51;
    case VK_RIGHT:   return 0xFF53;
    case VK_TAB:     return 0xFF09;
    case VK_ESCAPE:  return 0xFF1B;
    case VK_SHIFT:
    case VK_LSHIFT:  return 0xFFE1;
    case VK_CONTROL:
    case VK_LCONTROL: return 0xFFE3;
    case VK_MENU:
    case VK_LMENU:   return 0xFFE9;
    case VK_F5:      return 0xFFC2;
    case VK_F6:      return 0xFFC3;
    case VK_MULTIPLY: return '*';
    default: break;
    }
    /* Digits and letters: Win32 reports both as their ASCII code point, and
     * the binding table folds case, so these pass straight through. The
     * numeric keypad maps onto the top row there too. */
    if (vk >= '0' && vk <= '9') return (uint32_t)vk;
    if (vk >= 'A' && vk <= 'Z') return (uint32_t)vk;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return (uint32_t)('0' + (vk - VK_NUMPAD0));
    return 0;
}
