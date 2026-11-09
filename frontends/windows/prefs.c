/*
 * The Preferences window: a tab per page -- Machine, Input, ROMs -- over the
 * same settings keys as the other frontends' Preferences, so a machine
 * configured in one comes up the same in another.
 *
 * Every machine and host option waits for the window to close, which hands
 * the main window one "dirty" flag so it restarts the session once for the
 * lot. The gamepad assignments apply at once.
 *
 * Laid out in 96-dpi units through coleco_scale() and rebuilt on
 * WM_DPICHANGED; each page is a set of children of the window shown or
 * hidden under a tab strip, so no page can paint over another.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include <commctrl.h>

#include "controllers/controllers_window.h"
#include "help/help_window.h"
#include "resource.h"

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

#define PREFS_CLASS "FujiNetGoColecoPrefs"
#define PREFS_STYLE (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define MAX_ITEMS 64

/* Layout, in 96-dpi units. */
#define PREFS_W 520         /* client width */
#define MARGIN 14
#define PAGE_H 296          /* the tallest page: Input */

enum { PG_MACHINE, PG_INPUT, PG_ROMS, PG_COUNT };

enum {
    IDC_TABS = 100, IDC_SGM, IDC_PALETTE, IDC_FUJINET, IDC_EXTERNAL, IDC_EXT_ADDR, IDC_AUDIO,
    IDC_GAMEPAD, IDC_SWAP, IDC_PAD_LIST, IDC_PAD_PORT, IDC_RESET_CONTROLS, IDC_OPEN_CONTROLLERS,
    IDC_ROM_PATH, IDC_IMPORT, IDC_IMPORT_FOLDER, IDC_ROM_STATE, IDC_PREFS_HELP, IDC_CLOSE
};

/* The Help contexts, by page. */
static const char *const k_contexts[PG_COUNT] = { "prefs/Machine", "prefs/Input", "prefs/ROMs" };

#define IDT_PADS 1

typedef struct {
    HWND ctrl;
    int page;               /* PG_*, or -1: always shown */
    int xpos, ypos, width, height;      /* page-relative, 96 dpi */
    int bold;               /* a heading */
} item;

static HWND g_win, g_tabs, g_close, g_help;
static HWND g_pad_list, g_pad_port, g_ext_addr, g_input_note, g_rom_state;
static item g_items[MAX_ITEMS];
static int g_numitems;
static HFONT g_font, g_bold;
static int g_dpi = 96;
static colecosession *g_session;
static HWND g_parent;
static coleco_prefs_hooks g_hooks;
static int g_dirty;
static int g_page;
static unsigned g_pad_seen;

static int SCL(int val) { return coleco_scale(val, g_dpi); }

/* ---- building -------------------------------------------------------------- */

static HWND add(const wchar_t *wclass, const char *utf8, DWORD style, int page, int ident,
                int xpos, int ypos, int width, int height)
{
    wchar_t *text = coleco_wide_dup(utf8 ? utf8 : "");
    HWND ctrl;
    if (g_numitems >= MAX_ITEMS) { free(text); return NULL; }
    ctrl = CreateWindowExW(0, wclass, text ? text : L"", WS_CHILD | style, 0, 0, 10, 10, g_win,
                           (HMENU)(INT_PTR)ident, GetModuleHandleA(NULL), NULL);
    free(text);
    g_items[g_numitems].ctrl = ctrl;
    g_items[g_numitems].page = page;
    g_items[g_numitems].xpos = xpos;
    g_items[g_numitems].ypos = ypos;
    g_items[g_numitems].width = width;
    g_items[g_numitems].height = height;
    g_items[g_numitems].bold = 0;
    g_numitems++;
    return ctrl;
}

static HWND label(int page, const char *text, int xpos, int ypos, int width, int height)
{
    return add(L"STATIC", text, SS_LEFT | SS_NOPREFIX, page, 0, xpos, ypos, width, height);
}

static HWND heading(int page, const char *text, int ypos)
{
    HWND ctrl = label(page, text, MARGIN, ypos, PREFS_W - 2 * MARGIN, 18);
    if (ctrl) g_items[g_numitems - 1].bold = 1;
    return ctrl;
}

static HWND checkbox(int page, int ident, const char *text, int xpos, int ypos, int width, int checked)
{
    HWND ctrl = add(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, page, ident, xpos, ypos, width, 22);
    if (ctrl) SendMessageW(ctrl, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    return ctrl;
}

static HWND button(int page, int ident, const char *text, int xpos, int ypos, int width)
{
    return add(L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, page, ident, xpos, ypos, width, 28);
}

static HWND combo(int page, int ident, int xpos, int ypos, int width)
{
    return add(L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, page, ident, xpos, ypos, width, 220);
}

static void combo_add(HWND ctrl, const char *text)
{
    wchar_t *wide = coleco_wide_dup(text);
    if (wide) { SendMessageW(ctrl, CB_ADDSTRING, 0, (LPARAM)wide); free(wide); }
}

static void set_text(HWND ctrl, const char *utf8)
{
    wchar_t current[512], *wide = coleco_wide_dup(utf8);
    if (!wide) return;
    GetWindowTextW(ctrl, current, 512);
    if (wcscmp(current, wide) != 0) SetWindowTextW(ctrl, wide);
    free(wide);
}

/* ---- the machine page ---------------------------------------------------------- */

static void build_machine(void)
{
    /* Same order as every other frontend: the index is what goes into the
     * shared settings store. */
    static const char *const palette_names[] = {
        "Default (TMS9928A)", "Palette 2", "Palette 3", "Palette 4"
    };
    HWND ctrl;
    int ypos = 6, idx;

    heading(PG_MACHINE, "Machine", ypos); ypos += 24;
    checkbox(PG_MACHINE, IDC_SGM, "Super Game Module (Opcode SGM)", MARGIN, ypos, 340,
             colecosession_get_int(g_session, "sgm", 1));
    ypos += 30;
    label(PG_MACHINE, "Palette:", MARGIN, ypos + 3, 70, 18);
    ctrl = combo(PG_MACHINE, IDC_PALETTE, MARGIN + 76, ypos, 250);
    for (idx = 0; idx < (int)(sizeof palette_names / sizeof palette_names[0]); idx++)
        combo_add(ctrl, palette_names[idx]);
    SendMessageA(ctrl, CB_SETCURSEL, (WPARAM)colecosession_get_int(g_session, "palette", 0), 0);
    ypos += 40;

    heading(PG_MACHINE, "FujiNet", ypos); ypos += 24;
    checkbox(PG_MACHINE, IDC_FUJINET, "Enable FujiNet", MARGIN, ypos, 340,
             colecosession_get_int(g_session, "enable_fujinet", 1));
    ypos += 28;
    checkbox(PG_MACHINE, IDC_EXTERNAL, "Use external FujiNet", MARGIN, ypos, 340,
             colecosession_get_int(g_session, "fujinet_external", 0));
    ypos += 30;
    label(PG_MACHINE, "Address:port:", MARGIN, ypos + 3, 90, 18);
    g_ext_addr = add(L"EDIT", colecosession_get_str(g_session, "fujinet_external_addr", ""),
                     ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER, PG_MACHINE, IDC_EXT_ADDR, MARGIN + 96, ypos, 240, 24);
    if (g_ext_addr) {
        SendMessageW(g_ext_addr, EM_SETCUEBANNER, FALSE, (LPARAM)L"127.0.0.1:11502");
        EnableWindow(g_ext_addr, colecosession_get_int(g_session, "fujinet_external", 0) != 0);
    }
    ypos += 40;

    heading(PG_MACHINE, "Host", ypos); ypos += 24;
    checkbox(PG_MACHINE, IDC_AUDIO, "Audio", MARGIN, ypos, 340, colecosession_get_int(g_session, "enable_audio", 1));
}

/* ---- input ------------------------------------------------------------------------ */

static void refresh_pad_list(void)
{
    int idx, num, sel;
    if (!g_pad_list) return;
    sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
    SendMessageA(g_pad_list, LB_RESETCONTENT, 0, 0);
    num = colecosession_gamepad_count(g_session);
    if (num == 0)
        SendMessageA(g_pad_list, LB_ADDSTRING, 0, (LPARAM)"(no gamepads connected - plug one in)");
    for (idx = 0; idx < num; idx++) {
        char name[128], line[220];
        wchar_t wide[220];
        const int effective = colecosession_gamepad_effective_port(g_session, idx);
        colecosession_gamepad_name(g_session, idx, name, sizeof name);
        if (effective >= 0)
            snprintf(line, sizeof line, "%s  [controller %d%s]", name, effective + 1,
                     colecosession_gamepad_assignment(g_session, idx) < 0 ? ", automatic" : "");
        else
            snprintf(line, sizeof line, "%s  [unused]", name);
        MultiByteToWideChar(CP_UTF8, 0, line, -1, wide, 220);
        SendMessageW(g_pad_list, LB_ADDSTRING, 0, (LPARAM)wide);
    }
    if (sel >= 0 && sel < num) {
        SendMessageA(g_pad_list, LB_SETCURSEL, (WPARAM)sel, 0);
        SendMessageA(g_pad_port, CB_SETCURSEL, (WPARAM)(colecosession_gamepad_assignment(g_session, sel) + 1), 0);
    }
    g_pad_seen = colecosession_gamepad_generation(g_session);
}

static void build_input(void)
{
    int ypos = 6;
    heading(PG_INPUT, "Input", ypos); ypos += 24;
    checkbox(PG_INPUT, IDC_GAMEPAD, "Enable gamepads", MARGIN, ypos, 340,
             colecosession_get_int(g_session, "enable_gamepad", 1));
    ypos += 28;
    checkbox(PG_INPUT, IDC_SWAP, "Swap controller buttons", MARGIN, ypos, 340,
             colecosession_get_int(g_session, "swap_buttons", 0));
    ypos += 36;
    label(PG_INPUT, "Gamepads (select one, then choose the controller it drives):", MARGIN, ypos,
          PREFS_W - 2 * MARGIN, 18);
    ypos += 24;
    g_pad_list = add(L"LISTBOX", NULL, WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP | WS_BORDER, PG_INPUT,
                     IDC_PAD_LIST, MARGIN, ypos, 340, 84);
    g_pad_port = combo(PG_INPUT, IDC_PAD_PORT, MARGIN + 350, ypos, 142);
    combo_add(g_pad_port, "Automatic");
    combo_add(g_pad_port, "Controller 1");
    combo_add(g_pad_port, "Controller 2");
    SendMessageA(g_pad_port, CB_SETCURSEL, 0, 0);
    ypos += 96;
    g_input_note = label(PG_INPUT, "Keys and gamepad buttons are re-mapped in the Controllers window (F9).",
                         MARGIN, ypos, PREFS_W - 2 * MARGIN, 36);
    ypos += 44;
    button(PG_INPUT, IDC_RESET_CONTROLS, "Reset Controls", MARGIN, ypos, 130);
    button(PG_INPUT, IDC_OPEN_CONTROLLERS, "Open Controllers", MARGIN + 140, ypos, 150);
}

/* ---- the ROMs page ---------------------------------------------------------------- */

static void refresh_roms(void)
{
    if (!g_rom_state) return;
    set_text(g_rom_state, colecosession_bios_available(g_session) ? "Imported" : "Not imported");
}

static void build_roms(void)
{
    char path[1100];
    char *chr;
    int ypos = 6;
    heading(PG_ROMS, "ROMs", ypos); ypos += 24;
    label(PG_ROMS, "The BIOS is not part of the app. Import your own OS7.rom from a file, a zip (MAME's "
                   "coleco.zip works) or a folder; it is recognised by size and checksum, whatever it is "
                   "named. Kept in:", MARGIN, ypos, PREFS_W - 2 * MARGIN, 52);
    ypos += 58;
    snprintf(path, sizeof path, "%s", colecosession_roms_path(g_session));
    for (chr = path; *chr; chr++)
        if (*chr == '/') *chr = '\\';
    add(L"STATIC", path, SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, PG_ROMS, IDC_ROM_PATH, MARGIN, ypos,
        PREFS_W - 2 * MARGIN, 18);
    ypos += 30;
    button(PG_ROMS, IDC_IMPORT, "Import BIOS...", MARGIN, ypos, 130);
    button(PG_ROMS, IDC_IMPORT_FOLDER, "Import from a Folder...", MARGIN + 140, ypos, 180);
    ypos += 42;
    label(PG_ROMS, "ColecoVision BIOS (OS7.rom)", MARGIN, ypos, 280, 18);
    g_rom_state = add(L"STATIC", "", SS_RIGHT | SS_NOPREFIX, PG_ROMS, IDC_ROM_STATE, PREFS_W - MARGIN - 140, ypos,
                      140, 18);
    refresh_roms();
}

/* ---- layout ------------------------------------------------------------------------ */

static void show_page(int page)
{
    int idx;
    g_page = page;
    for (idx = 0; idx < g_numitems; idx++)
        ShowWindow(g_items[idx].ctrl, g_items[idx].page < 0 || g_items[idx].page == page ? SW_SHOW : SW_HIDE);
    /* Help, F1 and Ctrl+Shift+H open the page's own */
    coleco_help_set_context(g_win, k_contexts[page], 1);
}

/* Places everything at the current DPI and sizes the window to its
 * contents. */
static void layout(void)
{
    RECT want;
    int idx, top;
    top = coleco_place_tab_strip(g_tabs, 0, 0, SCL(PREFS_W)) + SCL(8);
    for (idx = 0; idx < g_numitems; idx++) {
        const item *entry = &g_items[idx];
        if (!entry->ctrl) continue;
        MoveWindow(entry->ctrl, SCL(entry->xpos), top + SCL(entry->ypos), SCL(entry->width), SCL(entry->height), TRUE);
    }
    MoveWindow(g_help, SCL(MARGIN), top + SCL(PAGE_H) + SCL(6), SCL(100), SCL(28), TRUE);
    MoveWindow(g_close, SCL(PREFS_W) - SCL(MARGIN) - SCL(100), top + SCL(PAGE_H) + SCL(6), SCL(100), SCL(28), TRUE);
    SetRect(&want, 0, 0, SCL(PREFS_W), top + SCL(PAGE_H) + SCL(6) + SCL(28) + SCL(MARGIN));
    {
        const SIZE outer = coleco_outer_size(g_win, want.right, want.bottom, FALSE);
        SetWindowPos(g_win, NULL, 0, 0, outer.cx, outer.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* The window's fonts at the current DPI: plain on every child, bold on the
 * headings. */
static void make_fonts(void)
{
    int idx;
    if (g_font) DeleteObject(g_font);
    if (g_bold) DeleteObject(g_bold);
    g_font = coleco_make_font(g_dpi, 0, FW_NORMAL, NULL);
    g_bold = coleco_make_font(g_dpi, 0, FW_BOLD, NULL);
    coleco_set_font_all(g_win, g_font);
    for (idx = 0; idx < g_numitems; idx++)
        if (g_items[idx].bold) SendMessageW(g_items[idx].ctrl, WM_SETFONT, (WPARAM)g_bold, TRUE);
}

/* ---- actions -------------------------------------------------------------------------- */

static void setting_from_checkbox(int ident, const char *key, int def)
{
    const int checked = SendDlgItemMessageA(g_win, ident, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (colecosession_get_int(g_session, key, def) != checked) {
        colecosession_set_int(g_session, key, checked);
        g_dirty = 1;
    }
}

/* The external address is saved, and only if it is one; an invalid one with
 * the option on keeps the window open and says so when interactive. */
static int commit_address(int interactive)
{
    char addr[96], host[64];
    const char *old;
    int port;
    char *first = addr, *last;

    GetWindowTextA(g_ext_addr, addr, sizeof addr);
    while (*first == ' ' || *first == '\t') first++;
    memmove(addr, first, strlen(first) + 1);
    last = addr + strlen(addr);
    while (last > addr && (last[-1] == ' ' || last[-1] == '\t')) *--last = '\0';

    if (SendDlgItemMessageA(g_win, IDC_EXTERNAL, BM_GETCHECK, 0, 0) == BST_CHECKED &&
        colecosession_parse_hostport(addr, host, sizeof host, &port) != 0) {
        if (!interactive) return 1;
        coleco_message(g_win, "That is not an address:port.\n\nExample: 127.0.0.1:11502", "External FujiNet",
                       MB_ICONWARNING | MB_OK);
        TabCtrl_SetCurSel(g_tabs, PG_MACHINE);
        show_page(PG_MACHINE);
        SetFocus(g_ext_addr);
        return 0;
    }
    old = colecosession_get_str(g_session, "fujinet_external_addr", "");
    if (colecosession_parse_hostport(addr, host, sizeof host, &port) == 0 && strcmp(old, addr) != 0) {
        colecosession_set_str(g_session, "fujinet_external_addr", addr);
        g_dirty = 1;
    }
    return 1;
}

static void on_command(int ident, int code)
{
    switch (ident) {
    case IDC_PALETTE:
        if (code == CBN_SELCHANGE) {
            const int sel = (int)SendDlgItemMessageA(g_win, IDC_PALETTE, CB_GETCURSEL, 0, 0);
            if (colecosession_get_int(g_session, "palette", 0) != sel) {
                colecosession_set_int(g_session, "palette", sel);
                g_dirty = 1;
            }
        }
        break;
    case IDC_SGM: setting_from_checkbox(ident, "sgm", 1); break;
    case IDC_FUJINET: setting_from_checkbox(ident, "enable_fujinet", 1); break;
    case IDC_EXTERNAL:
        setting_from_checkbox(ident, "fujinet_external", 0);
        EnableWindow(g_ext_addr, SendDlgItemMessageA(g_win, IDC_EXTERNAL, BM_GETCHECK, 0, 0) == BST_CHECKED);
        break;
    case IDC_AUDIO: setting_from_checkbox(ident, "enable_audio", 1); break;
    case IDC_GAMEPAD: setting_from_checkbox(ident, "enable_gamepad", 1); break;
    case IDC_SWAP: setting_from_checkbox(ident, "swap_buttons", 0); break;
    case IDC_PAD_LIST:
        if (code == LBN_SELCHANGE) {
            const int sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < colecosession_gamepad_count(g_session))
                SendMessageA(g_pad_port, CB_SETCURSEL, (WPARAM)(colecosession_gamepad_assignment(g_session, sel) + 1), 0);
        }
        break;
    case IDC_PAD_PORT:
        if (code == CBN_SELCHANGE) {
            const int sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
            const int choice = (int)SendMessageA(g_pad_port, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < colecosession_gamepad_count(g_session))
                colecosession_gamepad_assign(g_session, sel, choice - 1);
            refresh_pad_list();
            coleco_controllers_window_refresh();
        }
        break;
    case IDC_RESET_CONTROLS:
        colecosession_bindings_reset(g_session);
        coleco_controllers_window_refresh();
        set_text(g_input_note, "Every key and gamepad button is back to its default.");
        break;
    case IDC_OPEN_CONTROLLERS:
        coleco_controllers_window_show(g_parent, g_session);
        break;
    case IDC_IMPORT:
    case IDC_IMPORT_FOLDER:
        if (g_hooks.import_bios) g_hooks.import_bios(g_win, ident == IDC_IMPORT_FOLDER);
        refresh_roms();
        break;
    default: break;
    }
}

/* ---- the window ----------------------------------------------------------------------- */

static LRESULT CALLBACK prefs_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wparam) == IDC_PREFS_HELP) { coleco_help_for(g_win); return 0; }
        if (LOWORD(wparam) == IDC_CLOSE || LOWORD(wparam) == IDCANCEL) {
            PostMessageA(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (LOWORD(wparam) != IDOK) on_command(LOWORD(wparam), HIWORD(wparam));
        return 0;
    case WM_NOTIFY:
        if (((LPNMHDR)lparam)->hwndFrom == g_tabs && ((LPNMHDR)lparam)->code == (UINT)TCN_SELCHANGE) {
            show_page(TabCtrl_GetCurSel(g_tabs));
            return 0;
        }
        break;
    case WM_TIMER:
        if (wparam == IDT_PADS && colecosession_gamepad_generation(g_session) != g_pad_seen)
            refresh_pad_list();
        return 0;
    case WM_DPICHANGED:
        g_dpi = HIWORD(wparam);
        make_fonts();
        layout();
        return 0;
    case WM_CLOSE:
        if (!commit_address(1)) return 0;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
        const int dirty = g_dirty;
        KillTimer(hwnd, IDT_PADS);
        g_win = NULL;
        g_tabs = g_close = g_help = g_pad_list = g_pad_port = g_ext_addr = g_input_note = g_rom_state = NULL;
        g_numitems = 0;
        g_dirty = 0;
        if (g_font) DeleteObject(g_font);
        if (g_bold) DeleteObject(g_bold);
        g_font = g_bold = NULL;
        if (g_hooks.closed) g_hooks.closed(dirty);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void coleco_prefs_show(HWND parent, colecosession *session, const coleco_prefs_hooks *hooks)
{
    static int registered;
    static const char *const names[PG_COUNT] = { "Machine", "Input", "ROMs" };
    HINSTANCE inst = GetModuleHandleA(NULL);
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_TAB_CLASSES | ICC_BAR_CLASSES };
    int idx;

    if (g_win) {
        SetForegroundWindow(g_win);
        return;
    }
    g_session = session;
    g_parent = parent;
    g_hooks = *hooks;
    InitCommonControlsEx(&icc);
    if (!registered) {
        WNDCLASSEXW wcls;
        memset(&wcls, 0, sizeof wcls);
        wcls.cbSize = sizeof wcls;
        wcls.lpfnWndProc = prefs_proc;
        wcls.hInstance = inst;
        wcls.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wcls.lpszClassName = L"" PREFS_CLASS;
        wcls.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
        registered = RegisterClassExW(&wcls) != 0;
    }
    g_dpi = coleco_window_dpi(parent);
    g_numitems = 0;
    g_dirty = 0;
    g_win = CreateWindowExW(WS_EX_CONTROLPARENT, L"" PREFS_CLASS, L"Settings", PREFS_STYLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, parent, NULL, inst, NULL);
    if (!g_win) return;

    g_tabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS,
                             0, 0, 10, 10, g_win, (HMENU)(INT_PTR)IDC_TABS, inst, NULL);
    for (idx = 0; idx < PG_COUNT; idx++) {
        TCITEMW tab;
        wchar_t *wide = coleco_wide_dup(names[idx]);
        memset(&tab, 0, sizeof tab);
        tab.mask = TCIF_TEXT;
        tab.pszText = wide;
        SendMessageW(g_tabs, TCM_INSERTITEMW, (WPARAM)idx, (LPARAM)&tab);
        free(wide);
    }
    build_machine();
    build_input();
    build_roms();
    label(-1, "Applied by restarting the session when this window closes.", MARGIN + 110, PAGE_H + 6,
          PREFS_W - 2 * MARGIN - 220, 30);
    g_help = CreateWindowExW(0, L"BUTTON", L"Help", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                             0, 0, 10, 10, g_win, (HMENU)(INT_PTR)IDC_PREFS_HELP, inst, NULL);
    g_close = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              0, 0, 10, 10, g_win, (HMENU)(INT_PTR)IDC_CLOSE, inst, NULL);
    /* the tab strip first in creation order, last in z-order: with
     * WS_CLIPSIBLINGS it never paints over a page */
    SetWindowPos(g_tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    make_fonts();
    refresh_pad_list();
    TabCtrl_SetCurSel(g_tabs, PG_MACHINE);
    show_page(PG_MACHINE);
    layout();
    SetTimer(g_win, IDT_PADS, 1000, NULL);
    ShowWindow(g_win, SW_SHOW);
}

void coleco_prefs_abandon(void)
{
    if (!g_win) return;
    g_hooks.closed = NULL;
    commit_address(0);
    DestroyWindow(g_win);
}

void coleco_prefs_roms_changed(void)
{
    if (g_win) refresh_roms();
}

void coleco_prefs_gamepads_changed(void)
{
    if (g_win) refresh_pad_list();
}

int coleco_prefs_pretranslate(MSG *msg)
{
    if (!g_win || !msg->hwnd || (msg->hwnd != g_win && !IsChild(g_win, msg->hwnd))) return 0;
    /* F1 and the Ctrl shortcuts (Ctrl+Shift+H, Help) are the app's, not a dialog's */
    if ((msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN) &&
        (msg->wParam == VK_F1 || GetKeyState(VK_CONTROL) < 0))
        return 0;
    return IsDialogMessageW(g_win, msg) ? 1 : 0;
}
