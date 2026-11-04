/*
 * The app's accent colour for the Windows frontend: the TMS9918's dark blue
 * (COLECOSESSION_ACCENT_RGB, the launcher icon's colour), with WHITE text on
 * it. A push button drawn by the app itself (the keypad) fills its held or
 * remap-target state with coleco_accent_brush(); a real BUTTON control that
 * is a checkable push button handles NM_CUSTOMDRAW in its parent's WM_NOTIFY
 * and returns
 *   coleco_accent_customdraw((const NMCUSTOMDRAW *)lp, checked);
 * so an off button keeps the themed look and only the "on" paint is replaced.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef COLECO_WIN_ACCENT_BUTTON_H
#define COLECO_WIN_ACCENT_BUTTON_H

#include <windows.h>
#include <commctrl.h>

#include "colecosession.h"

#define COLECO_ACCENT_COLORREF                                            \
    RGB((COLECOSESSION_ACCENT_RGB >> 16) & 0xFF,                         \
        (COLECOSESSION_ACCENT_RGB >> 8) & 0xFF, COLECOSESSION_ACCENT_RGB & 0xFF)
#define COLECO_ACCENT_TEXT RGB(255, 255, 255)

/* The accent and a slightly lighter one for the hot (hovered) state; both
 * live for the process. */
static inline HBRUSH coleco_accent_brush(void)
{
    static HBRUSH brush;
    if (!brush)
        brush = CreateSolidBrush(COLECO_ACCENT_COLORREF);
    return brush;
}

static inline HBRUSH coleco_accent_hot_brush(void)
{
    static HBRUSH brush;
    if (!brush) {
        const COLORREF c = COLECO_ACCENT_COLORREF;
        const int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
        brush = CreateSolidBrush(RGB(r + (255 - r) / 5, g + (255 - g) / 5,
                                     b + (255 - b) / 5));
    }
    return brush;
}

/* Handles one NM_CUSTOMDRAW from a button. on = the button's toggle state;
 * the result is what the parent's WM_NOTIFY should return. */
static inline LRESULT coleco_accent_customdraw(const NMCUSTOMDRAW *cd, int on)
{
    char text[64];
    RECT r;

    if (cd->dwDrawStage != CDDS_PREPAINT || !on)
        return CDRF_DODEFAULT;
    r = cd->rc;
    FillRect(cd->hdc, &r,
             (cd->uItemState & CDIS_HOT) ? coleco_accent_hot_brush()
                                         : coleco_accent_brush());
    FrameRect(cd->hdc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
    SetBkMode(cd->hdc, TRANSPARENT);
    SetTextColor(cd->hdc, COLECO_ACCENT_TEXT);
    text[0] = '\0';
    GetWindowTextA(cd->hdr.hwndFrom, text, sizeof text);
    {
        /* the control's own font, not the DC's default */
        HFONT f = (HFONT)SendMessageA(cd->hdr.hwndFrom, WM_GETFONT, 0, 0);
        HGDIOBJ old = f ? SelectObject(cd->hdc, f) : NULL;
        DrawTextA(cd->hdc, text, -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (old) SelectObject(cd->hdc, old);
    }
    if (GetFocus() == cd->hdr.hwndFrom) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(cd->hdc, &r);
    }
    return CDRF_SKIPDEFAULT;
}

#endif
