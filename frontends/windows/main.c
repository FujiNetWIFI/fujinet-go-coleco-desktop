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
#include <commctrl.h>
#include <dwmapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "help/help_window.h"
#include "colecosession.h"
#include "debugger/dbg_window.h"
#include "controllers/controllers_window.h"
#include "import.h"
#include "key_forward.h"
#include "prefs.h"
#include "resource.h"

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

#define WIN_CLASS "FujiNetGoColecoVision"
#define APP_TITLE "FujiNet Go ColecoVision"

/* The main window's own timer: system actions from the gamepad thread, the
 * hot-plug notice, and the notice's expiry. */
#define IDT_MAIN 10
#define NOTICE_MS 4000

static colecosession *g_session;
static HWND g_hwnd;
static unsigned g_pad_generation;
static DWORD g_notice_until;            /* tick the title notice ends, 0 = none */
static int g_sysact_down[COLECO_SYSACT_COUNT];  /* a held Reset key fires once */
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

static int ask(HWND owner, const char *text, const char *title,
               const char *yes, const char *no, const char *help);
static int import_bios(HWND owner, int folder);

/* A start that failed: when the reason is the missing BIOS the message names
 * Import BIOS, and the answer is an offer of the import, not a dead end. */
static void report_start_failure(const char *title)
{
    char err[512];
    snprintf(err, sizeof err, "%s", colecosession_last_error(g_session));
    if (strstr(err, "Import BIOS") != NULL) {
        if (ask(g_hwnd, err, "BIOS needed", "Import BIOS...", "Not Now", "nobios"))
            import_bios(g_hwnd, 0);
        return;
    }
    MessageBox(g_hwnd, err, title, MB_ICONWARNING | MB_OK);
}

/* Stop, re-read the settings store, start. Everything the settings window
 * writes is read by colecosession_default_opts(), so this is the only way
 * any of it can take effect. */
static void restart_session(void)
{
    colecosession_start_opts o;
    colecosession_settings_flush(g_session);
    colecosession_default_opts(g_session, &o);
    colecosession_stop(g_session);
    if (colecosession_start(g_session, &o) != 0)
        report_start_failure("Could not start");
}

/* A short notice in the title bar (there is no status bar): a gamepad
 * hot-plug, a reset. */
static void notice(const char *text)
{
    char title[400];
    snprintf(title, sizeof title, APP_TITLE " - %s", text);
    coleco_set_text_utf8(g_hwnd, title);
    g_notice_until = GetTickCount() + NOTICE_MS;
    if (!g_notice_until) g_notice_until = 1;
}

static void notice_tick(void)
{
    if (!g_notice_until || (LONG)(GetTickCount() - g_notice_until) < 0) return;
    g_notice_until = 0;
    SetWindowTextA(g_hwnd, APP_TITLE);
}

/* Run a system action (a bound key, the menu, a gamepad button or the
 * Controllers window): the UI thread only. */
static void run_sysaction(int sysact)
{
    colecosession_sysaction(g_session, sysact);
    notice(sysact == COLECO_SYSACT_RESET ? "Console reset" : "Back to the FujiNet CONFIG client");
}

/* Gamepad hot-plug: the session bumps a generation on every add/remove and
 * keeps the last event as text; show it for a few seconds. */
static void poll_gamepads(void)
{
    const unsigned gen = colecosession_gamepad_generation(g_session);
    char text[160];
    if (gen == g_pad_generation) return;
    g_pad_generation = gen;
    if (colecosession_gamepad_last_event(g_session, text, sizeof text) > 0)
        notice(text);
    coleco_controllers_window_refresh();
    coleco_prefs_gamepads_changed();
}

/* ---- menu ----------------------------------------------------------------- */

static void build_menu(HWND hwnd)
{
    HMENU bar = CreateMenu();
    HMENU machine = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU fuji = CreatePopupMenu();
    HMENU help = CreatePopupMenu();

    AppendMenu(machine, MF_STRING, IDM_OPEN, "&Open Cartridge...");
    AppendMenu(machine, MF_STRING, IDM_EJECT, "&Eject Cartridge");
    AppendMenu(machine, MF_SEPARATOR, 0, NULL);
    AppendMenu(machine, MF_STRING, IDM_RESET, "&Reset Console");
    AppendMenu(machine, MF_STRING, IDM_RESET_CONFIG, "Reset to &CONFIG");
    AppendMenu(machine, MF_SEPARATOR, 0, NULL);
    AppendMenu(machine, MF_STRING, IDM_IMPORT_BIOS, "Import &BIOS...");
    AppendMenu(machine, MF_STRING, IDM_IMPORT_FOLDER,
               "Import BIOS from a &Folder...");
    AppendMenu(machine, MF_SEPARATOR, 0, NULL);
    AppendMenu(machine, MF_STRING, IDM_SETTINGS, "&Settings...");
    AppendMenu(machine, MF_SEPARATOR, 0, NULL);
    AppendMenu(machine, MF_STRING, IDM_EXIT, "E&xit");

    AppendMenu(view, MF_STRING, IDM_CONTROLLERS, "&Controllers\tF9");
    AppendMenu(view, MF_STRING, IDM_DEBUGGER, "&Debugger\tF12");
    AppendMenu(view, MF_SEPARATOR, 0, NULL);
    AppendMenu(view, MF_STRING | MF_CHECKED, IDM_TV_ASPECT, "&TV Aspect (4:3)");
    AppendMenu(view, MF_STRING, IDM_SMOOTH, "&Smooth Scaling");

    AppendMenu(fuji, MF_STRING, IDM_FUJINET_CONFIG, "&Configuration");
    AppendMenu(fuji, MF_STRING, IDM_FUJINET_LOG, "Console &Log");

    AppendMenu(bar, MF_POPUP, (UINT_PTR)machine, "&Machine");
    AppendMenu(bar, MF_POPUP, (UINT_PTR)view, "&View");
    AppendMenu(bar, MF_POPUP, (UINT_PTR)fuji, "&FujiNet");
    AppendMenu(help, MF_STRING, IDM_HELP, "FujiNet Go ColecoVision &Help\tCtrl+Shift+H");
    AppendMenu(help, MF_SEPARATOR, 0, NULL);
    AppendMenu(help, MF_STRING, IDM_ABOUT, "&About FujiNet Go ColecoVision");
    AppendMenu(bar, MF_POPUP, (UINT_PTR)help, "&Help");
    SetMenu(hwnd, bar);
}

/* A question with two buttons of our own words. Task dialogs need the
 * version 6 common controls (the manifest asks for them); looked up rather
 * than imported, so a process without them still gets a plain Yes/No. 1 for
 * the first button. */
static wchar_t *wide_dup(const char *text)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    if (!w) return NULL;
    if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, text, -1, w, n))
        w[0] = 0;
    return w;
}

/* The task dialog's Help button (a third button that does not close it). */
#define ASK_YES  1001
#define ASK_NO   1002
#define ASK_HELP 1003

static HRESULT CALLBACK ask_callback(HWND dlg, UINT note, WPARAM wp, LPARAM lp, LONG_PTR ref)
{
    (void)lp;
    if (note == TDN_BUTTON_CLICKED && wp == ASK_HELP) {
        coleco_help_show(dlg, (const char *)ref);
        return S_FALSE;
    }
    return S_OK;
}

static int ask(HWND owner, const char *text, const char *title,
               const char *yes, const char *no, const char *help)
{
    typedef HRESULT (WINAPI *task_fn)(const TASKDIALOGCONFIG *, int *, int *,
                                      BOOL *);
    HMODULE cc = LoadLibraryA("comctl32.dll");
    task_fn fn = cc ? (task_fn)(void (*)(void))GetProcAddress(
                          cc, "TaskDialogIndirect") : NULL;
    if (fn) {
        wchar_t *wt = wide_dup(text), *wc = wide_dup(title);
        wchar_t *wy = wide_dup(yes), *wn = wide_dup(no), *wh = wide_dup("Help");
        TASKDIALOG_BUTTON buttons[3];
        TASKDIALOGCONFIG cfg;
        int chosen = 0;
        HRESULT hr = E_FAIL;
        if (wt && wc && wy && wn && wh) {
            memset(&cfg, 0, sizeof cfg);
            buttons[0].nButtonID = ASK_YES;
            buttons[0].pszButtonText = wy;
            buttons[1].nButtonID = ASK_NO;
            buttons[1].pszButtonText = wn;
            buttons[2].nButtonID = ASK_HELP;
            buttons[2].pszButtonText = wh;
            cfg.cbSize = sizeof cfg;
            cfg.hwndParent = owner;
            cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
            cfg.pszWindowTitle = wc;
            cfg.pszMainIcon = TD_INFORMATION_ICON;
            cfg.pszContent = wt;
            cfg.cButtons = help ? 3 : 2;
            cfg.pButtons = buttons;
            cfg.nDefaultButton = ASK_YES;
            cfg.pfCallback = ask_callback;
            cfg.lpCallbackData = (LONG_PTR)help;
            hr = fn(&cfg, &chosen, NULL, NULL);
        }
        free(wt); free(wc); free(wy); free(wn); free(wh);
        if (SUCCEEDED(hr))
            return chosen == ASK_YES;
    }
    return MessageBoxA(owner, text, title,
                       MB_ICONINFORMATION | MB_YESNO) == IDYES;
}

/* The BIOS changed under a console that had none: the session restarts to
 * pick it up -- the cartridge it runs again, or CONFIG. */
static void apply_new_roms(int was_missing)
{
    if (was_missing)
        restart_session();
    coleco_prefs_roms_changed();
}

/* Import BIOS from the menu, Settings or an alert: files or a folder.
 * Returns 1 when something was imported. */
static int import_bios(HWND owner, int folder)
{
    const int was_missing = !colecosession_bios_available(g_session);
    const int n = coleco_import_bios(owner, g_session, folder);
    if (n > 0) apply_new_roms(was_missing);
    return n > 0;
}

static void load_media(const char *path)
{
    char dest[1024];
    /* A BIOS dump is a ROM, not a cartridge, though it is a .rom or .bin. */
    if (colecosession_media_is_rom(path)) {
        const int was_missing = !colecosession_bios_available(g_session);
        const char *one[1];
        one[0] = path;
        if (coleco_import_roms(g_hwnd, g_session, one, 1) > 0)
            apply_new_roms(was_missing);
        return;
    }
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
    restart_session();
}

static void pick_cart(void)
{
    OPENFILENAME ofn;
    char path[MAX_PATH] = "";

    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = "ColecoVision cartridges\0*.rom;*.col;*.bin\0"
                      "All files\0*.*\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof path;
    ofn.lpstrTitle = "Open Cartridge";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileName(&ofn)) return;
    load_media(path);
}

/* ---- settings window ---------------------------------------------------------
 *
 * prefs.c. Every option it writes is read by colecosession_default_opts() at
 * session start, so all of them are applied by restarting the session when
 * the window closes; doing it per click would rebuild the machine under
 * someone still deciding. The display options (TV aspect, smooth scaling)
 * apply live and stay in the View menu.
 */

static void prefs_import_bios(HWND owner, int folder)
{
    import_bios(owner, folder);
}

static void prefs_closed(int dirty)
{
    if (!dirty) return;
    restart_session();
    coleco_controllers_window_refresh();
}

static void show_settings(void)
{
    static const coleco_prefs_hooks hooks = { prefs_import_bios, prefs_closed };
    coleco_prefs_show(g_hwnd, g_session, &hooks);
}

/* ---- FujiNet console log ---------------------------------------------------
 *
 * A live view of the in-process runtime's captured output. This project is a
 * protocol bring-up as much as an emulator -- the cartridge dials into the
 * FujiNet and every transaction is logged -- so being able to watch that
 * without leaving the app is the difference between "it does not work" and
 * knowing which command failed.
 */

static HWND g_log_window;
static HWND g_log_edit, g_log_help;
#define LOG_BAR 34

static void log_refresh(void)
{
    /* Static: the ring is large and this runs once a second. */
    static char buf[128 * 1024];
    int n;
    DWORD first, last, lines;

    if (!g_log_edit) return;

    /* Follow the tail only while the reader is already at the bottom --
     * scrolling up to read something must not be yanked away a second
     * later. EM_GETFIRSTVISIBLELINE plus the visible height is the only way
     * an EDIT control will tell you that. */
    first = (DWORD)SendMessageA(g_log_edit, EM_GETFIRSTVISIBLELINE, 0, 0);
    lines = (DWORD)SendMessageA(g_log_edit, EM_GETLINECOUNT, 0, 0);
    {
        RECT rc;
        HDC dc = GetDC(g_log_edit);
        TEXTMETRICA tm;
        int visible = 1;
        GetClientRect(g_log_edit, &rc);
        if (dc) {
            HFONT of = (HFONT)SelectObject(
                dc, (HGDIOBJ)SendMessageA(g_log_edit, WM_GETFONT, 0, 0));
            if (GetTextMetricsA(dc, &tm) && tm.tmHeight > 0)
                visible = (rc.bottom - rc.top) / tm.tmHeight;
            SelectObject(dc, of);
            ReleaseDC(g_log_edit, dc);
        }
        last = first + (DWORD)(visible > 0 ? visible : 1);
    }

    n = colecosession_fujinet_copy_log(g_session, buf, sizeof buf);
    SetWindowTextA(g_log_edit, n > 0 ? buf : "(no FujiNet output yet)");

    if (last >= lines) {
        int len = GetWindowTextLengthA(g_log_edit);
        SendMessageA(g_log_edit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        SendMessageA(g_log_edit, EM_SCROLLCARET, 0, 0);
    }
}

static LRESULT CALLBACK log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (g_log_edit)
            MoveWindow(g_log_edit, 0, LOG_BAR, rc.right - rc.left,
                       rc.bottom - rc.top - LOG_BAR, TRUE);
        if (g_log_help) MoveWindow(g_log_help, rc.right - 84, 4, 76, LOG_BAR - 8, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_LOG_HELP) { coleco_help_for(hwnd); return 0; }
        break;
    case WM_TIMER:
        if (wp == IDT_LOG_REFRESH) log_refresh();
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_LOG_REFRESH);
        g_log_window = NULL;
        g_log_edit = NULL;
        g_log_help = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void show_fujinet_log(HINSTANCE inst)
{
    RECT rc;

    if (colecosession_fujinet_external(g_session)) {
        MessageBox(g_hwnd, "FujiNet is external: its console output is in "
                   "the process that is running it.", "FujiNet",
                   MB_ICONINFORMATION | MB_OK);
        return;
    }

    if (g_log_window) {
        SetForegroundWindow(g_log_window);
        return;
    }
    {
        static int registered;
        if (!registered) {
            WNDCLASSA wc;
            memset(&wc, 0, sizeof(wc));
            wc.lpfnWndProc = log_proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.lpszClassName = "ColecoFujiNetLogWindow";
            RegisterClassA(&wc);
            registered = 1;
        }
    }
    g_log_window = CreateWindowA("ColecoFujiNetLogWindow",
                                 "FujiNet Console Log", WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 860, 600, NULL,
                                 NULL, inst, NULL);
    GetClientRect(g_log_window, &rc);
    g_log_edit = CreateWindowExA(
        WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
            ES_AUTOVSCROLL | ES_READONLY,
        0, LOG_BAR, rc.right - rc.left, rc.bottom - rc.top - LOG_BAR, g_log_window,
        (HMENU)(INT_PTR)IDC_LOG_EDIT, inst, NULL);
    g_log_help = CreateWindowA("BUTTON", "Help", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10,
                               g_log_window, (HMENU)(INT_PTR)IDC_LOG_HELP, inst, NULL);
    SendMessageA(g_log_help, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    coleco_help_set_context(g_log_window, "fujilog", 1);
    /* A fixed-pitch font: the runtime's output is timestamped columns. */
    SendMessageA(g_log_edit, WM_SETFONT,
                 (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);

    SetTimer(g_log_window, IDT_LOG_REFRESH, 1000, NULL);
    SendMessageA(g_log_window, WM_SIZE, 0, 0);
    log_refresh();
    ShowWindow(g_log_window, SW_SHOW);
}

/* ---- window --------------------------------------------------------------- */

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
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

    /* WM_SYSKEY* as well: Windows reports Alt -- and every key pressed while
     * it is down -- as a system key. One that drives the console (a bound
     * key, Left Alt by default) is the console's; the rest (Alt+F4, the menu
     * mnemonics, Alt+Esc) stay Windows'. */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        uint32_t keysym;
        int sysact;
        if (coleco_controllers_map_key(msg, wparam, lparam)) return 0;
        if (wparam == VK_F9) {
            if (!(lparam & (1 << 30))) coleco_controllers_window_toggle(hwnd, g_session);
            return 0;
        }
        if (wparam == VK_F12) {
            if (!(lparam & (1 << 30))) coleco_debugger_show(hwnd, g_session);
            return 0;
        }
        /* Ctrl+Shift+H is the Help's, taken before the message reaches here */
        if (msg == WM_KEYDOWN && coleco_app_key(wparam)) return 0;
        if (msg == WM_SYSKEYDOWN) {
            /* the app's own actions never fire from a system key: Alt+Esc
             * is Windows' */
            if (coleco_forward_key(g_session, msg, wparam, lparam)) return 0;
            break;
        }
        keysym = coleco_keysym_from_msg(wparam, lparam);
        if (!keysym) break;
        /* The Reset keys first, on the leading edge only, whatever modifiers
         * are down (Left Ctrl is Fire). Then the console, through the bindings. */
        sysact = colecosession_key_sysaction(g_session, keysym);
        if (sysact >= 0) {
            if (!(lparam & (1 << 30)) && !g_sysact_down[sysact]) {
                g_sysact_down[sysact] = 1;
                run_sysaction(sysact);
            }
            return 0;
        }
        if (coleco_forward_key(g_session, msg, wparam, lparam)) return 0;
        break;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
        uint32_t keysym;
        int sysact;
        if (coleco_controllers_map_key(msg, wparam, lparam)) return 0;
        keysym = coleco_keysym_from_msg(wparam, lparam);
        if (!keysym) break;
        sysact = colecosession_key_sysaction(g_session, keysym);
        if (sysact >= 0) { g_sysact_down[sysact] = 0; return 0; }
        /* A bound Alt's release stops here, so it does not also put the menu
         * bar into keyboard mode. */
        if (coleco_forward_key(g_session, msg, wparam, lparam)) return 0;
        break;
    }
    case WM_ENTERMENULOOP:
        /* The menu takes the keyboard: whatever was held lets go now rather
         * than staying down until the key is pressed again. */
        colecosession_release_all(g_session);
        memset(g_sysact_down, 0, sizeof g_sysact_down);
        break;
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            /* Windows eats the key-up when focus leaves, so without this the
             * machine believes whatever was held is still down. */
            colecosession_release_all(g_session);
            memset(g_sysact_down, 0, sizeof g_sysact_down);
        }
        return 0;

    case WM_TIMER:
        if (wparam == IDT_MAIN) {
            int sysact;
            while (colecosession_sysaction_take(g_session, &sysact))
                run_sysaction(sysact);
            poll_gamepads();
            notice_tick();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDM_OPEN:
            pick_cart();
            return 0;
        case IDM_EJECT:
            colecosession_set_str(g_session, "cart_path", "");
            colecosession_settings_flush(g_session);
            colecosession_reset_to_config(g_session);
            return 0;
        case IDM_RESET: run_sysaction(COLECO_SYSACT_RESET); return 0;
        case IDM_RESET_CONFIG: run_sysaction(COLECO_SYSACT_RESET_CONFIG); return 0;
        case IDM_IMPORT_BIOS:
            import_bios(hwnd, 0);
            return 0;
        case IDM_IMPORT_FOLDER:
            import_bios(hwnd, 1);
            return 0;
        case IDM_CONTROLLERS:
            coleco_controllers_window_toggle(hwnd, g_session);
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
        case IDM_HELP: coleco_help_for(hwnd); return 0;
        case IDM_ABOUT: {
            /* MB_USERICON with the app's own icon, not a stock Windows one. */
            MSGBOXPARAMSA mb;
            memset(&mb, 0, sizeof(mb));
            mb.cbSize = sizeof(mb);
            mb.hwndOwner = hwnd;
            mb.hInstance = (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE);
            mb.lpszText = "FujiNet Go ColecoVision " COLECO_VERSION_STRING "\n"
                          "Self-contained ColecoVision with built-in FujiNet.\n"
                          "Copyright (C) 2026 Thomas Cherryhomes\n"
                          "GPL-3.0-or-later\n\n"
                          "https://fujinet.online/";
            mb.lpszCaption = "About FujiNet Go ColecoVision";
            mb.dwStyle = MB_OK | MB_USERICON;
            mb.lpszIcon = MAKEINTRESOURCEA(IDI_APPICON);
            MessageBoxIndirectA(&mb);
            return 0;
        }
        case IDM_SETTINGS:
            show_settings();
            return 0;
        case IDM_FUJINET_LOG:
            show_fujinet_log((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE));
            return 0;
        case IDM_FUJINET_CONFIG:
            if (colecosession_fujinet_external(g_session)) {
                MessageBox(hwnd, "FujiNet is external: use the web UI of the "
                           "instance this app dials.", "FujiNet",
                           MB_ICONINFORMATION | MB_OK);
                return 0;
            }
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
        HDROP drop = (HDROP)wparam;
        UINT i, count = DragQueryFile(drop, 0xFFFFFFFFu, NULL, 0);
        for (i = 0; i < count; i++)
            if (DragQueryFile(drop, i, path, sizeof path))
                load_media(path);
        DragFinish(drop);
        return 0;
    }

    case WM_DESTROY:
        coleco_prefs_abandon();
        KillTimer(hwnd, IDT_MAIN);
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProc(hwnd, msg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSEX wc;
    MSG msg;
    colecosession_start_opts opts;
    const int n = COLECOSESSION_FB_WIDTH * COLECOSESSION_FB_HEIGHT;

    (void)prev;

    /* The folder browser behind Import BIOS from a Folder wants COM. */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

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
        WS_EX_ACCEPTFILES, WIN_CLASS, APP_TITLE,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 848, 660,
        NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;
    coleco_help_set_context(g_hwnd, "main", 0);       /* F1 stays bindable in the window itself */
    build_menu(g_hwnd);
    CheckMenuItem(GetMenu(g_hwnd), IDM_TV_ASPECT,
                  MF_BYCOMMAND | (g_tv_aspect ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(GetMenu(g_hwnd), IDM_SMOOTH,
                  MF_BYCOMMAND | (g_smooth ? MF_CHECKED : MF_UNCHECKED));
    ShowWindow(g_hwnd, show);

    /* A BIOS named on the command line is imported, not run as a cartridge. */
    if (cmdline && *cmdline && colecosession_media_is_rom(cmdline)) {
        const char *one[1];
        one[0] = cmdline;
        coleco_import_roms(g_hwnd, g_session, one, 1);
        cmdline = NULL;
    }
    colecosession_default_opts(g_session, &opts);
    if (cmdline && *cmdline) opts.cart_path = cmdline;
    if (colecosession_start(g_session, &opts) != 0) {
        /* Show the window anyway: the usual reason to be here is a fresh
         * install with no BIOS, and the window is where the import lives --
         * so offer it. */
        report_start_failure("FujiNet Go ColecoVision");
    }

    /* The family's launch hooks: the way in when the app misbehaves before a
     * menu is reachable, and how a headless check can look at either panel. */
    g_pad_generation = colecosession_gamepad_generation(g_session);
    SetTimer(g_hwnd, IDT_MAIN, 50, NULL);
    {
        /* COLECO_OPEN_KEYPAD is the old name of COLECO_OPEN_CONTROLLERS */
        const char *env = getenv("COLECO_OPEN_CONTROLLERS");
        if (!env || !*env) env = getenv("COLECO_OPEN_KEYPAD");
        if (env && *env && *env != '0')
            coleco_controllers_window_toggle(g_hwnd, g_session);
        env = getenv("COLECO_OPEN_DEBUGGER");
        if (env && *env && *env != '0')
            coleco_debugger_show(g_hwnd, g_session);
        env = getenv("COLECO_OPEN_SETTINGS");
        if (env && *env && *env != '0')
            show_settings();
    }

    g_present_thread = CreateThread(NULL, 0, present_thread, NULL, 0, NULL);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        /* Before TranslateMessage, so the debugger's F5/F7/F8 accelerators
         * work even while one of its edit fields has the focus. */
        if (coleco_prefs_pretranslate(&msg))      /* first: it is a dialog's keys */
            continue;
        if (coleco_debugger_pretranslate(&msg))
            continue;
        if (coleco_help_pretranslate(&msg))
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
