/*
 * The Win32 controller panel: both hand controllers side by side, a 3x4
 * keypad, a four-way stick and two fire buttons each, plus a System row and a
 * Map row.
 *
 * Buttons are driven by WM_LBUTTONDOWN/WM_LBUTTONUP on the panel window
 * rather than by BN_CLICKED, for the reason every frontend in this family
 * shares: the emulator samples the controller register once per frame, so a
 * value present only for the instant of a click falls between frames and a
 * polling game -- which is how essentially every ColecoVision title reads its
 * skill select -- never sees it. A button here is HELD.
 *
 * The mouse is captured on press and released on button-up wherever that
 * happens, so dragging off a button cannot strand the machine with a key
 * held forever.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "keypad_window.h"

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include <stdio.h>
#include <string.h>

#include "../key_forward.h"

#define PAD_CLASS "FujiNetGoColecoKeypad"

#define BTN_W  46
#define BTN_H  30
#define GAP     4
#define WIDE_W 150

typedef struct {
    RECT rc;
    int target;
    char face[16];
} pad_button;

static HWND g_panel;
static colecosession *g_session;
static pad_button g_btn[COLECO_TARGET_COUNT];
static int g_nbtn;
static int g_held = -1;          /* target under the captured mouse, or -1 */
static int g_map_state = -2;     /* -2 idle, -1 armed, >=0 awaiting a key */
static RECT g_map_rc, g_defaults_rc;
static coleco_input_state g_keys;

static void add_button(const char *face, int target, int x, int y, int w)
{
    pad_button *b;
    if (g_nbtn >= COLECO_TARGET_COUNT) return;
    b = &g_btn[g_nbtn++];
    SetRect(&b->rc, x, y, x + w, y + BTN_H);
    b->target = target;
    snprintf(b->face, sizeof b->face, "%s", face);
}

static const char *const kFace[COLECO_KEYPAD_KEYS] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "#"
};
/* The physical 3x4 layout: 1-9 in reading order, then * 0 #. */
static const int kOrder[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11 };

static int build_controller(int port, int x0, int y0)
{
    const int colw = BTN_W + GAP;
    int y = y0, i;

    for (i = 0; i < 12; i++)
        add_button(kFace[kOrder[i]],
                   COLECO_TARGET_PORT(port, COLECO_ACT_KEYPAD + kOrder[i]),
                   x0 + (i % 3) * colw, y + (i / 3) * (BTN_H + GAP), BTN_W);
    y += 4 * (BTN_H + GAP) + GAP;

    add_button("^", COLECO_TARGET_PORT(port, COLECO_ACT_UP),
               x0 + colw, y, BTN_W);
    add_button("<", COLECO_TARGET_PORT(port, COLECO_ACT_LEFT),
               x0, y + BTN_H + GAP, BTN_W);
    add_button(">", COLECO_TARGET_PORT(port, COLECO_ACT_RIGHT),
               x0 + 2 * colw, y + BTN_H + GAP, BTN_W);
    add_button("v", COLECO_TARGET_PORT(port, COLECO_ACT_DOWN),
               x0 + colw, y + 2 * (BTN_H + GAP), BTN_W);
    y += 3 * (BTN_H + GAP) + GAP;

    add_button("Fire L", COLECO_TARGET_PORT(port, COLECO_ACT_FIRE_L),
               x0, y, BTN_W + colw / 2);
    add_button("Fire R", COLECO_TARGET_PORT(port, COLECO_ACT_FIRE_R),
               x0 + colw + colw / 2, y, BTN_W + colw / 2);
    return y + BTN_H + GAP;
}

static void layout(void)
{
    const int colw = BTN_W + GAP;
    const int panelw = 3 * colw;
    int y;

    g_nbtn = 0;
    y = build_controller(0, GAP, GAP + 18);
    build_controller(1, GAP + panelw + GAP * 3, GAP + 18);

    y += GAP;
    add_button("Reset Console",
               COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET), GAP, y, WIDE_W);
    add_button("Reset to CONFIG",
               COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET_CONFIG),
               GAP + WIDE_W + GAP, y, WIDE_W);
    y += BTN_H + GAP;

    SetRect(&g_map_rc, GAP, y, GAP + 64, y + BTN_H);
    SetRect(&g_defaults_rc, GAP + 68, y, GAP + 68 + 80, y + BTN_H);
}

static void press_target(int target, int down)
{
    if (target >= COLECO_TARGET_SYSACT(0)) {
        /* System actions fire on release, like a real button: pressing and
         * dragging off must not reset the console. */
        if (!down)
            colecosession_sysaction(g_session, target - COLECO_TARGET_SYSACT(0));
        return;
    }
    colecosession_press(g_session, target / COLECO_ACT_PER_PORT,
                        target % COLECO_ACT_PER_PORT, down);
}

static int hit(int x, int y)
{
    POINT p = { x, y };
    int i;
    for (i = 0; i < g_nbtn; i++)
        if (PtInRect(&g_btn[i].rc, p)) return i;
    return -1;
}

static void paint_panel(HDC dc)
{
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HGDIOBJ old = SelectObject(dc, font);
    RECT client;
    int i;

    GetClientRect(dc ? WindowFromDC(dc) : g_panel, &client);
    FillRect(dc, &client, (HBRUSH)(COLOR_BTNFACE + 1));
    SetBkMode(dc, TRANSPARENT);

    /* Headings, centred over each controller's controls. */
    {
        const int colw = BTN_W + GAP, panelw = 3 * colw;
        RECT h1, h2;
        SetRect(&h1, GAP, GAP, GAP + panelw, GAP + 16);
        SetRect(&h2, GAP + panelw + GAP * 3, GAP,
                GAP + panelw + GAP * 3 + panelw, GAP + 16);
        DrawText(dc, "Controller 1", -1, &h1, DT_CENTER | DT_SINGLELINE);
        DrawText(dc, "Controller 2", -1, &h2, DT_CENTER | DT_SINGLELINE);
    }

    for (i = 0; i < g_nbtn; i++) {
        UINT state = DFCS_BUTTONPUSH;
        char label[24];
        if (i == g_held) state |= DFCS_PUSHED;
        DrawFrameControl(dc, &g_btn[i].rc, DFC_BUTTON, state);
        if (g_map_state != -2) {
            const char *k = coleco_binding_key_name(g_btn[i].target);
            snprintf(label, sizeof label, "%s", *k ? k : "-");
        } else {
            snprintf(label, sizeof label, "%s", g_btn[i].face);
        }
        DrawText(dc, label, -1, &g_btn[i].rc,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    DrawFrameControl(dc, &g_map_rc, DFC_BUTTON, DFCS_BUTTONPUSH);
    DrawText(dc, g_map_state == -2 ? "Map" : "Cancel", -1, &g_map_rc,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawFrameControl(dc, &g_defaults_rc, DFC_BUTTON, DFCS_BUTTONPUSH);
    DrawText(dc, "Defaults", -1, &g_defaults_rc,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (g_map_state != -2) {
        RECT hint;
        char msg[96];
        SetRect(&hint, g_defaults_rc.right + GAP * 2, g_map_rc.top,
                client.right - GAP, g_map_rc.bottom);
        if (g_map_state == -1)
            snprintf(msg, sizeof msg, "Click a control to remap");
        else
            snprintf(msg, sizeof msg, "Press a key for %s",
                     coleco_binding_label(g_map_state));
        DrawText(dc, msg, -1, &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, old);
}

static LRESULT CALLBACK pad_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint_panel(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        POINT p = { x, y };
        int i = hit(x, y);

        if (PtInRect(&g_map_rc, p)) {
            g_map_state = (g_map_state == -2) ? -1 : -2;
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        if (PtInRect(&g_defaults_rc, p)) {
            coleco_bindings_reset_defaults(g_session);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        if (i < 0) return 0;

        if (g_map_state == -1) {
            g_map_state = g_btn[i].target;
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        if (g_map_state >= 0) return 0;

        g_held = i;
        SetCapture(hwnd);
        press_target(g_btn[i].target, 1);
        InvalidateRect(hwnd, &g_btn[i].rc, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        /* Release wherever the mouse ended up: capture means this arrives
         * even if the pointer left the button, which is what stops a key
         * being held forever after a drag. */
        if (g_held >= 0) {
            RECT r = g_btn[g_held].rc;
            press_target(g_btn[g_held].target, 0);
            g_held = -1;
            ReleaseCapture();
            InvalidateRect(hwnd, &r, FALSE);
        }
        return 0;

    case WM_KEYDOWN: {
        const uint32_t ks = coleco_keysym_from_vk((int)wp);
        if (g_map_state >= 0) {
            if (ks) {
                coleco_binding_set(g_session, g_map_state, ks);
                g_map_state = -1;   /* stay armed: remapping several in a row
                                     * is the normal case */
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        }
        if (g_map_state == -1) return 0;
        if (!ks) break;
        {
            const int sa = coleco_input_key_sysaction(ks);
            if (sa >= 0) { colecosession_sysaction(g_session, sa); return 0; }
        }
        if (coleco_input_key(&g_keys, ks, 1)) {
            colecosession_joystick_raw(g_session, 0, coleco_input_word(&g_keys, 0));
            colecosession_joystick_raw(g_session, 1, coleco_input_word(&g_keys, 1));
            return 0;
        }
        break;
    }
    case WM_KEYUP: {
        const uint32_t ks = coleco_keysym_from_vk((int)wp);
        if (g_map_state != -2) return 0;
        if (ks && coleco_input_key(&g_keys, ks, 0)) {
            colecosession_joystick_raw(g_session, 0, coleco_input_word(&g_keys, 0));
            colecosession_joystick_raw(g_session, 1, coleco_input_word(&g_keys, 1));
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        /* Hide, do not destroy: a remap in progress and the window's position
         * both survive closing it. */
        g_map_state = -2;
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    default: break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

void coleco_keypad_window_toggle(HWND parent, colecosession *session)
{
    g_session = session;

    if (!g_panel) {
        WNDCLASSEX wc;
        RECT want;
        const int colw = BTN_W + GAP, panelw = 3 * colw;

        coleco_input_reset(&g_keys);
        layout();

        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = pad_proc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = PAD_CLASS;
        RegisterClassEx(&wc);

        SetRect(&want, 0, 0,
                GAP + panelw + GAP * 3 + panelw + GAP,
                g_defaults_rc.bottom + GAP);
        /* A tool window with a thin border and no resize grip: the panel
         * sizes to its controls and there is nothing to gain by stretching
         * it. WS_EX_TOOLWINDOW also keeps it off the taskbar. */
        AdjustWindowRectEx(&want, WS_CAPTION | WS_SYSMENU, FALSE,
                           WS_EX_TOOLWINDOW);
        g_panel = CreateWindowEx(
            WS_EX_TOOLWINDOW, PAD_CLASS, "Controllers",
            WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
            want.right - want.left, want.bottom - want.top,
            parent, NULL, wc.hInstance, NULL);
        if (!g_panel) return;
    }

    if (IsWindowVisible(g_panel)) {
        g_map_state = -2;
        ShowWindow(g_panel, SW_HIDE);
    } else {
        ShowWindow(g_panel, SW_SHOW);
        SetForegroundWindow(g_panel);
    }
}
