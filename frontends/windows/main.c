/*
 * FujiNet Go ColecoVision -- the Windows (Win32 + GDI) frontend.
 *
 * No toolkit: a plain window, a menu bar, and StretchDIBits. That is enough
 * for a 256x212 framebuffer, and it keeps the artifact a folder you copy
 * rather than a runtime hunt.
 *
 * Two Windows-specific things are load bearing:
 *
 *   DwmFlush() on a present thread is this platform's frame clock. There is
 *   no GdkFrameClock here, and a plain timer would beat against the panel.
 *   The thread does nothing but wait for the compositor and hand the tick to
 *   the session, which is what lets the emulator phase-lock.
 *
 *   WM_ACTIVATE releases every held key. Alt-tabbing away mid-jump and coming
 *   back to a character walking into a wall is the classic symptom of not
 *   doing this, and Windows is where it happens most, because the WM eats the
 *   key-up.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>
#include <dwmapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "colecosession.h"
#include "debugger/dbg_window.h"
#include "keypad/keypad_window.h"
#include "key_forward.h"
#include "resource.h"

#define WIN_CLASS "FujiNetGoColecoVision"

static colecosession *g_session;
static HWND g_hwnd;
static coleco_input_state g_input;
static uint16_t *g_fb;
static uint32_t *g_rgb;
static uint64_t g_serial;
static BITMAPINFO g_bmi;
static volatile LONG g_running = 1;
static HANDLE g_present_thread;
static int g_tv_aspect = 1, g_smooth = 0;

/* ---- frame conversion ----------------------------------------------------- */

static void convert_frame(void)
{
    const int n = COLECOSESSION_FB_WIDTH * COLECOSESSION_FB_HEIGHT;
    int i;
    for (i = 0; i < n; i++) {
        const uint16_t p = g_fb[i];
        unsigned r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
        /* Replicate the high bits rather than shifting left: a plain shift
         * maps full-scale 0x1F to 0xF8, so white comes out slightly grey. */
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        g_rgb[i] = (r << 16) | (g << 8) | b;
    }
}

/* ---- the present thread: this platform's frame clock ---------------------- */

static DWORD WINAPI present_thread(LPVOID arg)
{
    (void)arg;
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        LARGE_INTEGER t;
        /* Blocks until the compositor's next vblank. Falls through
         * immediately if the DWM is off, in which case the session's
         * wall-clock pacing takes over -- which is the whole reason
         * notify_vsync is advisory. */
        DwmFlush();
        QueryPerformanceCounter(&t);
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            colecosession_notify_vsync(
                g_session, (int64_t)(t.QuadPart * 1000000000LL / f.QuadPart));
        }
        if (colecosession_copy_frame(g_session, g_fb, &g_serial)) {
            convert_frame();
            InvalidateRect(g_hwnd, NULL, FALSE);
        }
    }
    return 0;
}

/* ---- painting ------------------------------------------------------------- */

static void paint(HDC dc)
{
    RECT rc;
    double want, w, h, sw, sh;

    GetClientRect(g_hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    /* 256x212 is not 4:3: the 192 active lines are, and the borders are part
     * of the same field on real hardware, so TV mode stretches the whole
     * buffer rather than letterboxing inside one. */
    want = g_tv_aspect ? (4.0 / 3.0)
                       : (double)COLECOSESSION_FB_WIDTH /
                         (double)COLECOSESSION_FB_HEIGHT;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }

    /* Black the letterbox before the blit, or the previous frame's edges stay
     * on screen when the window is resized. */
    {
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        FillRect(dc, &rc, black);
    }
    SetStretchBltMode(dc, g_smooth ? HALFTONE : COLORONCOLOR);
    StretchDIBits(dc,
                  (int)((w - sw) / 2), (int)((h - sh) / 2), (int)sw, (int)sh,
                  0, 0, COLECOSESSION_FB_WIDTH, COLECOSESSION_FB_HEIGHT,
                  g_rgb, &g_bmi, DIB_RGB_COLORS, SRCCOPY);
}

/* ---- input ---------------------------------------------------------------- */

static void push_input(void)
{
    colecosession_joystick_raw(g_session, 0, coleco_input_word(&g_input, 0));
    colecosession_joystick_raw(g_session, 1, coleco_input_word(&g_input, 1));
}

/* ---- menu ----------------------------------------------------------------- */

static void build_menu(HWND hwnd)
{
    HMENU bar = CreateMenu();
    HMENU machine = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU fuji = CreatePopupMenu();

    AppendMenu(machine, MF_STRING, IDM_OPEN, "&Open Cartridge...");
    AppendMenu(machine, MF_STRING, IDM_EJECT, "&Eject Cartridge");
    AppendMenu(machine, MF_SEPARATOR, 0, NULL);
    AppendMenu(machine, MF_STRING, IDM_RESET, "&Reset Console");
    AppendMenu(machine, MF_STRING, IDM_RESET_CONFIG, "Reset to &CONFIG");
    AppendMenu(machine, MF_SEPARATOR, 0, NULL);
    AppendMenu(machine, MF_STRING, IDM_IMPORT_BIOS, "&Import BIOS...");
    AppendMenu(machine, MF_SEPARATOR, 0, NULL);
    AppendMenu(machine, MF_STRING, IDM_EXIT, "E&xit");

    AppendMenu(view, MF_STRING, IDM_KEYPAD, "&Controllers\tF9");
    AppendMenu(view, MF_STRING, IDM_DEBUGGER, "&Debugger\tF12");
    AppendMenu(view, MF_SEPARATOR, 0, NULL);
    AppendMenu(view, MF_STRING | MF_CHECKED, IDM_TV_ASPECT, "&TV Aspect (4:3)");
    AppendMenu(view, MF_STRING, IDM_SMOOTH, "&Smooth Scaling");

    AppendMenu(fuji, MF_STRING, IDM_FUJINET_CONFIG, "&Configuration");

    AppendMenu(bar, MF_POPUP, (UINT_PTR)machine, "&Machine");
    AppendMenu(bar, MF_POPUP, (UINT_PTR)view, "&View");
    AppendMenu(bar, MF_POPUP, (UINT_PTR)fuji, "&FujiNet");
    SetMenu(hwnd, bar);
}

static void load_media(const char *path)
{
    char dest[1024];
    if (colecosession_import_media(g_session, path, dest, sizeof dest) != 0) {
        MessageBox(g_hwnd, colecosession_last_error(g_session),
                   "Import failed", MB_ICONWARNING | MB_OK);
        return;
    }
    if (!colecosession_media_is_cartridge(dest)) {
        MessageBox(g_hwnd,
                   "Copied to FujiNet's SD folder. Mount it from the CONFIG "
                   "client.", "Imported", MB_ICONINFORMATION | MB_OK);
        return;
    }
    colecosession_set_str(g_session, "cart_path", dest);
    colecosession_settings_flush(g_session);
    {
        colecosession_start_opts o;
        colecosession_default_opts(g_session, &o);
        colecosession_stop(g_session);
        if (colecosession_start(g_session, &o) != 0)
            MessageBox(g_hwnd, colecosession_last_error(g_session),
                       "Could not start", MB_ICONWARNING | MB_OK);
    }
}

static void pick_file(const char *title, const char *filter, int is_bios)
{
    OPENFILENAME ofn;
    char path[MAX_PATH] = "";

    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof path;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileName(&ofn)) return;

    if (is_bios) {
        if (colecosession_import_bios(g_session, path) < 0)
            MessageBox(g_hwnd, colecosession_last_error(g_session),
                       "Import failed", MB_ICONWARNING | MB_OK);
        else
            MessageBox(g_hwnd, "BIOS imported. Restart to boot it.",
                       "Imported", MB_ICONINFORMATION | MB_OK);
    } else {
        load_media(path);
    }
}

/* ---- window --------------------------------------------------------------- */

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;  /* paint() fills it; erasing first would flicker */

    case WM_KEYDOWN: {
        uint32_t ks;
        int sa;
        if (wp == VK_F9) {
            coleco_keypad_window_toggle(hwnd, g_session);
            return 0;
        }
        if (wp == VK_F12) {
            coleco_debugger_show(hwnd, g_session);
            return 0;
        }
        if (lp & (1 << 30)) return 0;  /* auto-repeat: the key is already held */
        ks = coleco_keysym_from_vk((int)wp);
        if (!ks) break;
        sa = coleco_input_key_sysaction(ks);
        if (sa >= 0) { colecosession_sysaction(g_session, sa); return 0; }
        if (coleco_input_key(&g_input, ks, 1)) { push_input(); return 0; }
        break;
    }
    case WM_KEYUP: {
        uint32_t ks = coleco_keysym_from_vk((int)wp);
        if (ks && coleco_input_key(&g_input, ks, 0)) { push_input(); return 0; }
        break;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            /* Windows eats the key-up when focus leaves, so without this the
             * machine believes whatever was held is still down. */
            coleco_input_reset(&g_input);
            push_input();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN:
            pick_file("Open Cartridge",
                      "ColecoVision cartridges\0*.rom;*.col;*.bin\0"
                      "All files\0*.*\0\0", 0);
            return 0;
        case IDM_EJECT:
            colecosession_set_str(g_session, "cart_path", "");
            colecosession_settings_flush(g_session);
            colecosession_reset_to_config(g_session);
            return 0;
        case IDM_RESET: colecosession_reset(g_session); return 0;
        case IDM_RESET_CONFIG: colecosession_reset_to_config(g_session); return 0;
        case IDM_IMPORT_BIOS:
            pick_file("Import ColecoVision BIOS (OS7.rom)",
                      "BIOS images\0*.rom;*.bin\0All files\0*.*\0\0", 1);
            return 0;
        case IDM_KEYPAD:
            coleco_keypad_window_toggle(hwnd, g_session);
            return 0;
        case IDM_DEBUGGER:
            coleco_debugger_show(hwnd, g_session);
            return 0;
        case IDM_TV_ASPECT: {
            HMENU m = GetMenu(hwnd);
            g_tv_aspect = !g_tv_aspect;
            CheckMenuItem(m, IDM_TV_ASPECT,
                          MF_BYCOMMAND | (g_tv_aspect ? MF_CHECKED : MF_UNCHECKED));
            colecosession_set_int(g_session, "tv_aspect", g_tv_aspect);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        case IDM_SMOOTH: {
            HMENU m = GetMenu(hwnd);
            g_smooth = !g_smooth;
            CheckMenuItem(m, IDM_SMOOTH,
                          MF_BYCOMMAND | (g_smooth ? MF_CHECKED : MF_UNCHECKED));
            colecosession_set_int(g_session, "smooth", g_smooth);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        case IDM_FUJINET_CONFIG:
            if (!colecosession_fujinet_running(g_session)) {
                MessageBox(hwnd, "FujiNet is not running.", "FujiNet",
                           MB_ICONINFORMATION | MB_OK);
                return 0;
            }
            ShellExecute(hwnd, "open",
                         colecosession_fujinet_webui_url(g_session),
                         NULL, NULL, SW_SHOWNORMAL);
            return 0;
        case IDM_EXIT:
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return 0;
        default: break;
        }
        break;

    case WM_DROPFILES: {
        char path[MAX_PATH];
        HDROP drop = (HDROP)wp;
        if (DragQueryFile(drop, 0, path, sizeof path))
            load_media(path);
        DragFinish(drop);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSEX wc;
    MSG msg;
    colecosession_start_opts opts;
    const int n = COLECOSESSION_FB_WIDTH * COLECOSESSION_FB_HEIGHT;

    (void)prev;

    g_session = colecosession_new(NULL);
    if (!g_session) {
        MessageBox(NULL, "Could not create the session (unusable config or "
                         "data directories?)", "FujiNet Go ColecoVision",
                   MB_ICONERROR | MB_OK);
        return 1;
    }

    g_fb = calloc((size_t)n, sizeof *g_fb);
    g_rgb = calloc((size_t)n, sizeof *g_rgb);
    if (!g_fb || !g_rgb) return 1;

    memset(&g_bmi, 0, sizeof g_bmi);
    g_bmi.bmiHeader.biSize = sizeof g_bmi.bmiHeader;
    g_bmi.bmiHeader.biWidth = COLECOSESSION_FB_WIDTH;
    /* Negative height: a top-down DIB, so row 0 is the top of the picture and
     * the framebuffer needs no flip. */
    g_bmi.bmiHeader.biHeight = -COLECOSESSION_FB_HEIGHT;
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;

    coleco_input_reset(&g_input);
    g_tv_aspect = colecosession_get_int(g_session, "tv_aspect", 1);
    g_smooth = colecosession_get_int(g_session, "smooth", 0);

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WIN_CLASS;
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassEx(&wc);

    g_hwnd = CreateWindowEx(
        WS_EX_ACCEPTFILES, WIN_CLASS, "FujiNet Go ColecoVision",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 848, 660,
        NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;
    build_menu(g_hwnd);
    CheckMenuItem(GetMenu(g_hwnd), IDM_TV_ASPECT,
                  MF_BYCOMMAND | (g_tv_aspect ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(GetMenu(g_hwnd), IDM_SMOOTH,
                  MF_BYCOMMAND | (g_smooth ? MF_CHECKED : MF_UNCHECKED));
    ShowWindow(g_hwnd, show);

    colecosession_default_opts(g_session, &opts);
    if (cmdline && *cmdline) opts.cart_path = cmdline;
    if (colecosession_start(g_session, &opts) != 0) {
        /* Show the window anyway: the usual reason to be here is a fresh
         * install with no BIOS, and the window is where the import lives. */
        MessageBox(g_hwnd, colecosession_last_error(g_session),
                   "FujiNet Go ColecoVision", MB_ICONWARNING | MB_OK);
    }

    /* The family's launch hooks: the way in when the app misbehaves before a
     * menu is reachable, and how a headless check can look at either panel. */
    {
        const char *env = getenv("COLECO_OPEN_KEYPAD");
        if (env && *env && *env != '0')
            coleco_keypad_window_toggle(g_hwnd, g_session);
        env = getenv("COLECO_OPEN_DEBUGGER");
        if (env && *env && *env != '0')
            coleco_debugger_show(g_hwnd, g_session);
    }

    g_present_thread = CreateThread(NULL, 0, present_thread, NULL, 0, NULL);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        /* Before TranslateMessage, so the debugger's F5/F7/F8 accelerators
         * work even while one of its edit fields has the focus. */
        if (coleco_debugger_pretranslate(&msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    InterlockedExchange(&g_running, 0);
    if (g_present_thread) {
        WaitForSingleObject(g_present_thread, 2000);
        CloseHandle(g_present_thread);
    }
    colecosession_stop(g_session);
    colecosession_free(g_session);
    free(g_fb);
    free(g_rgb);
    return (int)msg.wParam;
}
