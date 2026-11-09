/*
 * The Win32 Controllers window: the two ColecoVision hand controllers drawn
 * as a player sees them -- a black case, a short stick on a disc ringed by
 * eight direction wedges, a fire button down either side, and the 12-key
 * keypad in the frame of the overlay-card slot -- from the session's layout
 * (colecosession_controls_layout), so every frontend shows the same
 * controllers. Under them the console's two buttons (Reset Console, Reset to
 * CONFIG), the gamepads with their assignments, and Map mode.
 *
 * Mouse: a press on a key or a fire button holds it; a press on the stick or
 * a wedge follows the pointer and holds the direction it points
 * (colecosession_stick_from_vector), nothing inside a quarter of the disc.
 * One pointer holds one control: dragging off a key lets go of it, dragging
 * onto another does not press it. Presses are the raw button-down and
 * button-up, never a click: the console samples its controls once a frame,
 * so a press present only for the instant of a click falls between frames.
 * The mouse is captured on press; letting go anywhere, losing the capture,
 * hiding or deactivating the window releases whatever the pointer holds
 * (colecosession_pointer_set(port, 0)). The console buttons fire on release
 * over the button, like a real one.
 *
 * What the console sees held -- keyboard, gamepads and this window together
 * (colecosession_buttons_held) -- is polled every 40 ms and painted in the
 * accent, so the window doubles as a gamepad tester.
 *
 * FIXED SIZE: laid out once for the controllers, three gamepad rows and the
 * Map row, in a caption-only tool window with no thick frame and no maximize
 * box, so neither Map mode nor a long gamepad name changes its size.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control to pick the target
 * (the two console buttons too), then press a host key OR a gamepad button
 * (Esc cancels). While armed the controls show what drives them, typing does
 * not reach the console, and rebinding steals the key from whatever held it.
 * Clear takes the picked target's key and button away. "Show bindings" shows
 * the same legends outside Map mode.
 *
 * The art is drawn at twice the size into a memory bitmap and shrunk with
 * HALFTONE (GDI has no anti-aliasing of its own); the text goes on after at
 * the window's own size.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include "controllers_window.h"

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../accent_button.h"
#include "../help/help_window.h"
#include "../key_forward.h"
#include "../win_common.h"

#define PANEL_CLASS   "FujiNetGoColecoControllers"
#define PANEL_STYLE   (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define PANEL_EXSTYLE WS_EX_TOOLWINDOW

/* Sizes at 96 dpi; SCL() scales them to the window's. */
#define UNIT96      34      /* pixels per layout unit (a keypad key's width) */
#define MARGIN      12
#define CASE_PAD    18
#define BRAND_H     16      /* the brand strip along the top of the case */
#define HEAD_H      22
#define NAME_H      22      /* a gamepad row: its name line ... */
#define SEG_H       24      /* ... and its Automatic / Controller 1 / 2 line */
#define ROW_GAP      6
#define PAD_ROWS     3      /* gamepad rows the window has room for */
#define BTN_H       28
#define MAP_W       56
#define CLEAR_W     56
#define DEFAULTS_W  74
#define SHOW_W     118
#define HELP_W      60

#define IDT_CAPTURE 1
#define IDT_HELD    2
#define IDT_PADS    3

#define PI_DEG 3.14159265358979323846

#define DIR_BITS ((1u << COLECO_ACT_UP) | (1u << COLECO_ACT_DOWN) | \
                  (1u << COLECO_ACT_LEFT) | (1u << COLECO_ACT_RIGHT))

/* The buttons below the controllers: the console's two, then the Map row. */
enum { SB_RESET, SB_CONFIG, SB_MAP, SB_CLEAR, SB_DEFAULTS, SB_SHOW, SB_HELP, SB_COUNT };

typedef struct {
    unsigned generation;
    int count, assign[PAD_ROWS], effective[PAD_ROWS];
} pads_state;

static HWND g_panel, g_parent;
static colecosession *g_session;
static int g_dpi = 96;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static int g_show_bindings;
static WPARAM g_swallow_vkey;   /* the key Map took: its repeats and release too */
static char g_hint[300];
static int g_hover_part = -1;   /* the part under the idle pointer: its bindings are the hint */
static int g_tracking;

/* The controllers' layout (units), and where it sits in the client. */
static const coleco_control *g_layout;
static int g_numlayout;
static float g_layw, g_layh;
static double g_unit, g_orgx, g_orgy;
static struct { double ctrx, ctry, rad; } g_disc[COLECO_PORTS];

/* What the pointer holds. */
static int g_mouse_part = -1;       /* a pressed key or fire button (layout index) */
static int g_mouse_port = -1;       /* a stick drag */
static int g_mouse_action = -1;     /* a pressed console button */
static unsigned g_pointer[COLECO_PORTS];    /* the bits given to colecosession_pointer_set */

/* What was last painted as held. */
static unsigned g_shown_held[COLECO_PORTS];

static pads_state g_pads_shown;
static int g_total_w, g_total_h;
static RECT g_case_rc, g_brand_rc, g_gp_rc, g_gp_head_rc, g_gp_row_rc[PAD_ROWS];
static RECT g_btn_rc[SB_COUNT], g_hint_rc;

static HFONT g_font, g_bold, g_legend, g_digit, g_tiny, g_brand;

static void refresh_held(void);
static void binding_label(int target, char *out, int outsz);

static int SCL(int val) { return coleco_scale(val, g_dpi); }

/* ---- fonts ------------------------------------------------------------------ */

static void free_fonts(void)
{
    HFONT *const slots[] = { &g_font, &g_bold, &g_legend, &g_digit, &g_tiny, &g_brand };
    size_t idx;
    for (idx = 0; idx < sizeof slots / sizeof slots[0]; idx++)
        if (*slots[idx]) { DeleteObject(*slots[idx]); *slots[idx] = NULL; }
}

static void make_fonts(void)
{
    free_fonts();
    g_font = coleco_make_font(g_dpi, 0, FW_NORMAL, NULL);
    g_bold = coleco_make_font(g_dpi, 0, FW_BOLD, NULL);
    g_legend = coleco_make_font(g_dpi, 11, FW_BOLD, NULL);
    g_digit = coleco_make_font(g_dpi, 15, FW_BOLD, NULL);
    g_tiny = coleco_make_font(g_dpi, 10, FW_NORMAL, NULL);
    g_brand = coleco_make_font(g_dpi, 11, FW_BOLD, NULL);
}

/* ---- text -------------------------------------------------------------------- */

/* UTF-8 (legends, key names, gamepad names) through DrawTextW. */
static int text_out(HDC hdc, const char *utf8, RECT *rect, UINT fmt)
{
    wchar_t wide[256];
    if (!utf8 || !*utf8) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)(sizeof wide / sizeof wide[0])) <= 0) return 0;
    return DrawTextW(hdc, wide, -1, rect, fmt | DT_NOPREFIX);
}

static void text_line(HDC hdc, const char *utf8, const RECT *rect, UINT fmt)
{
    RECT copy = *rect;
    text_out(hdc, utf8, &copy, fmt | DT_SINGLELINE | DT_VCENTER);
}

/* Centred both ways, wrapped (a legend that is a long key name). */
static void text_block(HDC hdc, const char *utf8, const RECT *rect)
{
    RECT mid = *rect;
    int high;
    InflateRect(&mid, -SCL(1), 0);
    high = text_out(hdc, utf8, &mid, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
    mid.left = rect->left + SCL(1);
    mid.right = rect->right - SCL(1);
    mid.top = rect->top + ((rect->bottom - rect->top) - high) / 2;
    if (mid.top < rect->top) mid.top = rect->top;
    mid.bottom = rect->bottom;
    text_out(hdc, utf8, &mid, DT_CENTER | DT_WORDBREAK);
}

/* The colours live here and only here; the geometry is the core's. */
#define COL_ACCENT    COLECO_ACCENT_COLORREF
#define COL_ON_ACCENT RGB(0xff, 0xff, 0xff)               /* the accent is dark: white on it */
#define COL_CASE_TOP  RGB(0x3a, 0x3a, 0x3d)
#define COL_CASE_BOT  RGB(0x1f, 0x1f, 0x21)
#define COL_CASE_EDGE RGB(0x0b, 0x0b, 0x0c)
#define COL_BRAND_BAR RGB(0x15, 0x15, 0x17)
#define COL_BRAND_TXT RGB(0x94, 0x96, 0xf2)               /* the accent lightened 35% */
#define COL_BODY_TOP  RGB(0x2b, 0x2b, 0x2e)
#define COL_BODY_BOT  RGB(0x15, 0x15, 0x17)
#define COL_BODY_EDGE RGB(0x05, 0x05, 0x05)
#define COL_CAPTION   RGB(0xe4, 0xe0, 0xd6)
#define COL_PLATE     RGB(0x0d, 0x0d, 0x0e)
#define COL_PLATE_EDG RGB(0x18, 0x18, 0x1a)
#define COL_WEDGE     RGB(0x2a, 0x2a, 0x2e)
#define COL_WEDGE_EDG RGB(0x44, 0x44, 0x4a)
#define COL_GLYPH     RGB(0x9a, 0x96, 0x8c)
#define COL_CAP_DARK  RGB(0x1c, 0x1c, 0x1f)
#define COL_CAP_LIGHT RGB(0x55, 0x55, 0x5b)
#define COL_CAP_RIM   RGB(0x0a, 0x0a, 0x0b)
#define COL_FIRE_TOP  RGB(0xd9, 0x46, 0x1e)
#define COL_FIRE_SIDE RGB(0x7c, 0x24, 0x0f)
#define COL_CARD_TOP  RGB(0xb9, 0xbb, 0xbe)
#define COL_CARD_BOT  RGB(0x8e, 0x90, 0x93)
#define COL_CARD_SLOT RGB(0x0b, 0x0b, 0x0c)
#define COL_CARD_LIP  RGB(0x4a, 0x4a, 0x4e)
#define COL_KEY_TOP   RGB(0x1d, 0x1d, 0x20)
#define COL_KEY_SIDE  RGB(0x0a, 0x0a, 0x0b)
#define COL_KEY_EDGE  RGB(0x3c, 0x3c, 0x40)
#define COL_LEGEND    RGB(0xf2, 0xf2, 0xf2)
#define COL_DIM_TO    RGB(0x25, 0x25, 0x28)               /* unpicked parts fade toward the case */
#define NOFILL        ((COLORREF)0xFFFFFFFFu)

/* ---- art: GDI at a supersampled scale ------------------------------------------- */

/* One art pass draws the client rectangle with its top-left at (g_art_x,
 * g_art_y) into a bitmap g_art_k times the size and g_art_rows rows tall. */
static int g_art_k = 1, g_art_x, g_art_y, g_art_rows;

static int art_xpos(double val) { return (int)floor((val - g_art_x) * g_art_k + 0.5); }
static int art_ypos(double val) { return (int)floor((val - g_art_y) * g_art_k + 0.5); }
static int art_len(double val) { return (int)floor(val * g_art_k + 0.5); }

static COLORREF mix_color(COLORREF start, COLORREF finish, double frac)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    return RGB((int)(GetRValue(start) + (GetRValue(finish) - GetRValue(start)) * frac + 0.5),
               (int)(GetGValue(start) + (GetGValue(finish) - GetGValue(start)) * frac + 0.5),
               (int)(GetBValue(start) + (GetBValue(finish) - GetBValue(start)) * frac + 0.5));
}

/* An unpicked part in Map mode: 40% of its own colours over the case's. */
static COLORREF tone(COLORREF col, int dim)
{
    return dim ? mix_color(col, COL_DIM_TO, 0.6) : col;
}

static void poly(HDC hdc, const POINT *pnts, int num, COLORREF fill, COLORREF line, double width)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, width > 0 ? art_len(width) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, width > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Polygon(hdc, pnts, num);
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void ellipse(HDC hdc, double ctrx, double ctry, double radx, double rady, COLORREF fill, COLORREF line,
                    double width)
{
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, width > 0 ? art_len(width) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, width > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Ellipse(hdc, art_xpos(ctrx - radx), art_ypos(ctry - rady), art_xpos(ctrx + radx), art_ypos(ctry + rady));
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    DeleteObject(brush);
}

static void rrect(HDC hdc, double xpos, double ypos, double width, double height, double radius, COLORREF fill,
                  COLORREF line, double linew)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, linew > 0 ? art_len(linew) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, linew > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    RoundRect(hdc, art_xpos(xpos), art_ypos(ypos), art_xpos(xpos + width), art_ypos(ypos + height),
              art_len(radius * 2), art_len(radius * 2));
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

/* A rounded rectangle filled with a vertical gradient, then outlined. */
static void grad_rrect(HDC hdc, double xpos, double ypos, double width, double height, double radius,
                       COLORREF top, COLORREF bottom, COLORREF edge, double edgew)
{
    const int left = art_xpos(xpos), right = art_xpos(xpos + width);
    const int first = art_ypos(ypos), last = art_ypos(ypos + height);
    HRGN clip = CreateRoundRectRgn(left, first, right + 1, last + 1, art_len(radius * 2), art_len(radius * 2));
    int row;
    HGDIOBJ oldb;
    SelectClipRgn(hdc, clip);
    oldb = SelectObject(hdc, GetStockObject(DC_BRUSH));
    for (row = first < 0 ? 0 : first; row <= last && row < g_art_rows; row++) {
        SetDCBrushColor(hdc, mix_color(top, bottom, last > first ? (double)(row - first) / (last - first) : 0));
        PatBlt(hdc, left, row, right - left + 1, 1, PATCOPY);
    }
    SelectObject(hdc, oldb);
    SelectClipRgn(hdc, NULL);
    DeleteObject(clip);
    rrect(hdc, xpos, ypos, width, height, radius, NOFILL, edge, edgew);
}

/* The pixel rectangle of a layout part. */
static void part_box(const coleco_control *ctl, double *xpos, double *ypos, double *width, double *height)
{
    *xpos = g_orgx + (double)ctl->xpos * g_unit;
    *ypos = g_orgy + (double)ctl->ypos * g_unit;
    *width = (double)ctl->width * g_unit;
    *height = (double)ctl->height * g_unit;
}

static void part_rect(const coleco_control *ctl, RECT *rect)
{
    double xpos, ypos, width, height;
    part_box(ctl, &xpos, &ypos, &width, &height);
    SetRect(rect, (int)floor(xpos), (int)floor(ypos), (int)ceil(xpos + width), (int)ceil(ypos + height));
}

static int binding_mode(void)
{
    return g_map_state != -2 || g_show_bindings;
}

/* Whether a part is shown held: what the console sees, in the accent. In Map
 * mode the picked target is the one lit. */
static int part_lit(const coleco_control *ctl)
{
    if (g_map_state != -2)
        return g_map_state >= 0 && ctl->target == g_map_state;
    switch (ctl->kind) {
    case COLECO_CTL_KEY:
    case COLECO_CTL_FIRE: return (g_shown_held[ctl->port] & ctl->act_mask) != 0;
    case COLECO_CTL_DIRECTION: return (g_shown_held[ctl->port] & DIR_BITS) == ctl->act_mask;
    default: return 0;
    }
}

/* Map mode with a target picked: every other part is drawn at 40%. */
static int part_dim(const coleco_control *ctl)
{
    if (g_map_state < 0) return 0;
    if (ctl->kind != COLECO_CTL_KEY && ctl->kind != COLECO_CTL_FIRE &&
        ctl->kind != COLECO_CTL_DIRECTION && ctl->kind != COLECO_CTL_STICK) return 0;
    return ctl->target != g_map_state;
}

/* A direction vector from the held bits (+y toward the player, down). */
static void held_vector(unsigned held, double *delx, double *dely)
{
    double xval = (int)((held >> COLECO_ACT_RIGHT) & 1) - (int)((held >> COLECO_ACT_LEFT) & 1);
    double yval = (int)((held >> COLECO_ACT_DOWN) & 1) - (int)((held >> COLECO_ACT_UP) & 1);
    const double len = sqrt(xval * xval + yval * yval);
    if (len > 0) { xval /= len; yval /= len; }
    *delx = xval;
    *dely = yval;
}

/* One of the eight wedges: the sector of the stick disc between half its
 * radius and all of it, a little narrower than 45 degrees so they read as
 * separate. */
static void wedge(HDC hdc, double ctrx, double ctry, double rad, int sector, COLORREF fill, COLORREF line)
{
    POINT pnts[40];
    int num = 0, idx;
    const double start = (sector * 45.0 - 21.5) * PI_DEG / 180.0, span = 43.0 * PI_DEG / 180.0;
    for (idx = 0; idx <= 9; idx++) {
        const double ang = start + span * idx / 9.0;
        pnts[num].x = art_xpos(ctrx + rad * cos(ang));
        pnts[num++].y = art_ypos(ctry - rad * sin(ang));
    }
    for (idx = 9; idx >= 0; idx--) {
        const double ang = start + span * idx / 9.0;
        pnts[num].x = art_xpos(ctrx + rad * 0.52 * cos(ang));
        pnts[num++].y = art_ypos(ctry - rad * 0.52 * sin(ang));
    }
    poly(hdc, pnts, num, fill, line, 1.0);
}

/* The sector index of a wedge (0 = RIGHT, counter-clockwise) from its bits. */
static int wedge_sector(unsigned mask)
{
    static const unsigned sector_bits[8] = {
        1u << COLECO_ACT_RIGHT,
        (1u << COLECO_ACT_UP) | (1u << COLECO_ACT_RIGHT),
        1u << COLECO_ACT_UP,
        (1u << COLECO_ACT_UP) | (1u << COLECO_ACT_LEFT),
        1u << COLECO_ACT_LEFT,
        (1u << COLECO_ACT_DOWN) | (1u << COLECO_ACT_LEFT),
        1u << COLECO_ACT_DOWN,
        (1u << COLECO_ACT_DOWN) | (1u << COLECO_ACT_RIGHT),
    };
    int idx;
    for (idx = 0; idx < 8; idx++)
        if (sector_bits[idx] == mask) return idx;
    return 0;
}

/* An arrowhead at (ctrx, ctry) pointing along maths angle `ang` (degrees,
 * y up). */
static void arrow(HDC hdc, double ctrx, double ctry, double ang, double size, COLORREF col)
{
    POINT pnts[3];
    const double rad = ang * PI_DEG / 180.0, dirx = cos(rad), diry = -sin(rad);
    pnts[0].x = art_xpos(ctrx + dirx * size);
    pnts[0].y = art_ypos(ctry + diry * size);
    pnts[1].x = art_xpos(ctrx - dirx * size * 0.6 - diry * size * 0.8);
    pnts[1].y = art_ypos(ctry - diry * size * 0.6 + dirx * size * 0.8);
    pnts[2].x = art_xpos(ctrx - dirx * size * 0.6 + diry * size * 0.8);
    pnts[2].y = art_ypos(ctry - diry * size * 0.6 - dirx * size * 0.8);
    poly(hdc, pnts, 3, col, col, 0.5);
}

/* The stick's cap: a dark-rimmed disc lit from the upper left. */
static void cap_disc(HDC hdc, double ctrx, double ctry, double rad, double shadowx, double shadowy, int dim)
{
    const int steps = 9;
    int idx;
    ellipse(hdc, ctrx + shadowx, ctry + shadowy, rad + g_unit * 0.06, rad + g_unit * 0.06,
            tone(RGB(0x07, 0x07, 0x08), dim), tone(RGB(0x07, 0x07, 0x08), dim), 0);
    ellipse(hdc, ctrx, ctry, rad + g_unit * 0.06, rad + g_unit * 0.06, tone(COL_CAP_RIM, dim),
            tone(COL_CAP_RIM, dim), 0);
    for (idx = 0; idx < steps; idx++) {
        const double frac = (double)idx / (steps - 1);
        const COLORREF col = tone(mix_color(COL_CAP_DARK, COL_CAP_LIGHT, frac), dim);
        const double size = rad * (1.0 - 0.7 * frac);
        ellipse(hdc, ctrx - rad * 0.16 * frac, ctry - rad * 0.2 * frac, size, size, col, col, 0);
    }
}

/* One controller's stick: the plate, the eight wedges, the cap. */
static void art_stick(HDC hdc, int port, const coleco_control *stick, unsigned held)
{
    const double ctrx = g_disc[port].ctrx, ctry = g_disc[port].ctry, rad = g_disc[port].rad;
    const int mapping = binding_mode();
    const int stick_dim = part_dim(stick);
    double delx, dely;
    int idx;

    ellipse(hdc, ctrx, ctry, rad * 1.07, rad * 1.07, tone(COL_PLATE, stick_dim), tone(COL_PLATE_EDG, stick_dim), 1.0);
    for (idx = 0; idx < g_numlayout; idx++) {
        const coleco_control *ctl = &g_layout[idx];
        const int lit = part_lit(ctl), dim = part_dim(ctl);
        int sector;
        if (ctl->kind != COLECO_CTL_DIRECTION || ctl->port != port) continue;
        sector = wedge_sector(ctl->act_mask);
        wedge(hdc, ctrx, ctry, rad, sector, lit ? COL_ACCENT : tone(COL_WEDGE, dim),
              lit ? COL_ON_ACCENT : tone(COL_WEDGE_EDG, dim));
        if (!mapping)
            arrow(hdc, ctrx + rad * 0.76 * cos(sector * 45.0 * PI_DEG / 180.0),
                  ctry - rad * 0.76 * sin(sector * 45.0 * PI_DEG / 180.0), sector * 45.0, rad * 0.11,
                  lit ? COL_ON_ACCENT : tone(COL_GLYPH, dim));
    }
    held_vector(g_map_state == -2 ? held : 0, &delx, &dely);
    cap_disc(hdc, ctrx + delx * rad * 0.14, ctry + dely * rad * 0.14, rad * 0.46,
             rad * (0.04 + 0.06 * fabs(delx)) + delx * rad * 0.1, rad * 0.06 + dely * rad * 0.1, stick_dim);
}

static void art_controller(HDC hdc, int port)
{
    const unsigned held = g_shown_held[port];
    const double unit = g_unit;
    int idx;

    /* the case of the hand controller, behind its parts (the fire buttons
     * straddle its edges) */
    for (idx = 0; idx < g_numlayout; idx++) {
        const coleco_control *ctl = &g_layout[idx];
        double xpos, ypos, width, height;
        if (ctl->kind != COLECO_CTL_CAPTION || ctl->port != port) continue;
        part_box(ctl, &xpos, &ypos, &width, &height);
        grad_rrect(hdc, xpos + unit * 0.3, ypos + height + unit * 0.04, width - unit * 0.6,
                   ((double)g_layh - ctl->ypos - ctl->height) * unit - unit * 0.1, unit * 0.42, COL_BODY_TOP,
                   COL_BODY_BOT, COL_BODY_EDGE, 1.5);
    }
    for (idx = 0; idx < g_numlayout; idx++) {
        const coleco_control *ctl = &g_layout[idx];
        double xpos, ypos, width, height;
        const int lit = part_lit(ctl), dim = part_dim(ctl);
        if (ctl->port != port) continue;
        part_box(ctl, &xpos, &ypos, &width, &height);
        switch (ctl->kind) {
        case COLECO_CTL_OVERLAY:
            /* the overlay-card slot: a silver faceplate with the slot across its top */
            grad_rrect(hdc, xpos, ypos, width, height, unit * 0.16, COL_CARD_TOP, COL_CARD_BOT, COL_CARD_SLOT, 1.0);
            rrect(hdc, xpos + unit * 0.3, ypos + unit * 0.1, width - unit * 0.6, unit * 0.1, unit * 0.04,
                  COL_CARD_SLOT, COL_CARD_SLOT, 0);
            rrect(hdc, xpos + unit * 0.3, ypos + unit * 0.2, width - unit * 0.6, unit * 0.04, 0, COL_CARD_LIP,
                  COL_CARD_LIP, 0);
            break;
        case COLECO_CTL_STICK:
            art_stick(hdc, port, ctl, held);
            break;
        case COLECO_CTL_FIRE: {
            const double side = SCL(3);
            rrect(hdc, xpos, ypos + side, width, height - side, unit * 0.22,
                  lit ? COL_ACCENT : tone(COL_FIRE_SIDE, dim), lit ? COL_ON_ACCENT : tone(COL_FIRE_SIDE, dim),
                  lit ? 2.0 : 0);
            rrect(hdc, xpos, ypos, width, height - side, unit * 0.22, lit ? COL_ACCENT : tone(COL_FIRE_TOP, dim),
                  lit ? COL_ON_ACCENT : tone(COL_FIRE_SIDE, dim), lit ? 2.0 : 1.0);
            break;
        }
        case COLECO_CTL_KEY: {
            const double side = SCL(3);
            rrect(hdc, xpos, ypos + side, width, height - side, unit * 0.12, tone(COL_KEY_SIDE, dim),
                  tone(COL_KEY_SIDE, dim), 0);
            rrect(hdc, xpos, ypos, width, height - side, unit * 0.12, lit ? COL_ACCENT : tone(COL_KEY_TOP, dim),
                  lit ? COL_ON_ACCENT : tone(COL_KEY_EDGE, dim), lit ? 2.0 : 1.0);
            break;
        }
        default:
            break;
        }
    }
}

/* The case, its brand strip, and both controllers. */
static void art_case(HDC hdc)
{
    grad_rrect(hdc, g_case_rc.left, g_case_rc.top, g_case_rc.right - g_case_rc.left,
               g_case_rc.bottom - g_case_rc.top, SCL(16), COL_CASE_TOP, COL_CASE_BOT, COL_CASE_EDGE, 1.5);
    rrect(hdc, g_brand_rc.left, g_brand_rc.top, g_brand_rc.right - g_brand_rc.left,
          g_brand_rc.bottom - g_brand_rc.top, SCL(5), COL_BRAND_BAR, COL_CASE_EDGE, 1.0);
    rrect(hdc, g_brand_rc.left + SCL(3), g_brand_rc.bottom - SCL(3), g_brand_rc.right - g_brand_rc.left - SCL(6),
          SCL(2), 0, COL_ACCENT, COL_ACCENT, 0);
    art_controller(hdc, 0);
    art_controller(hdc, 1);
}

/* In Map mode and with Show bindings a target shows what drives it. */
static void binding_label(int target, char *out, int outsz)
{
    const coleco_binding bind = colecosession_binding_get(g_session, target);
    out[0] = '\0';
    if (bind.keysym) {
        colecosession_keysym_name(bind.keysym, out, outsz);
        /* the keycaps are small: "Keypad 5" reads "KP 5" */
        if (strncmp(out, "Keypad ", 7) == 0) {
            char shortname[48];
            snprintf(shortname, sizeof shortname, "KP %s", out + 7);
            snprintf(out, (size_t)outsz, "%s", shortname);
        }
    }
    if (!out[0]) snprintf(out, (size_t)outsz, "%s", "\xe2\x80\x94");
}

/* The labels and numerals over the art, at the window's own size. */
static void text_parts(HDC hdc)
{
    int idx;
    RECT brand = g_brand_rc;
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, g_brand);
    SetTextColor(hdc, COL_BRAND_TXT);
    brand.bottom -= SCL(3);
    text_line(hdc, "COLECOVISION", &brand, DT_CENTER);
    for (idx = 0; idx < g_numlayout; idx++) {
        const coleco_control *ctl = &g_layout[idx];
        RECT rect;
        const int lit = part_lit(ctl), dim = part_dim(ctl);
        const COLORREF ink = lit ? COL_ON_ACCENT : tone(COL_LEGEND, dim);
        char legend[96];
        part_rect(ctl, &rect);
        switch (ctl->kind) {
        case COLECO_CTL_CAPTION:
            SelectObject(hdc, g_bold);
            SetTextColor(hdc, COL_CAPTION);
            text_line(hdc, ctl->label, &rect, DT_CENTER);
            break;
        case COLECO_CTL_KEY: {
            RECT cap = rect;
            cap.bottom -= SCL(3);
            SetTextColor(hdc, ink);
            if (binding_mode()) {
                /* the digit above, what drives it below */
                RECT upper = cap, lower = cap;
                upper.bottom = cap.top + (cap.bottom - cap.top) * 11 / 20;
                lower.top = upper.bottom - SCL(1);
                SelectObject(hdc, g_legend);
                text_line(hdc, ctl->label, &upper, DT_CENTER);
                binding_label(ctl->target, legend, sizeof legend);
                SelectObject(hdc, g_tiny);
                text_line(hdc, legend, &lower, DT_CENTER | DT_END_ELLIPSIS);
            } else {
                SelectObject(hdc, g_digit);
                text_line(hdc, ctl->label, &cap, DT_CENTER);
            }
            break;
        }
        case COLECO_CTL_FIRE:
            if (binding_mode()) {
                RECT cap = rect;
                cap.bottom -= SCL(3);
                binding_label(ctl->target, legend, sizeof legend);
                SelectObject(hdc, g_tiny);
                SetTextColor(hdc, COL_ON_ACCENT);
                text_block(hdc, legend, &cap);
            }
            break;
        case COLECO_CTL_DIRECTION:
            if (binding_mode() && ctl->target >= 0) {
                /* a wedge's box is its bounding box: the legend sits at the middle of the sector */
                const double ang = wedge_sector(ctl->act_mask) * 45.0 * PI_DEG / 180.0;
                const double rad = g_disc[ctl->port].rad * 0.76;
                const int midx = (int)(g_disc[ctl->port].ctrx + rad * cos(ang));
                const int midy = (int)(g_disc[ctl->port].ctry - rad * sin(ang));
                const int half = (int)(g_unit * 0.5);
                RECT box;
                binding_label(ctl->target, legend, sizeof legend);
                SelectObject(hdc, g_tiny);
                SetTextColor(hdc, lit ? COL_ON_ACCENT : tone(COL_LEGEND, dim));
                SetRect(&box, midx - half, midy - SCL(6), midx + half, midy + SCL(6));
                text_line(hdc, legend, &box, DT_CENTER | DT_END_ELLIPSIS);
            }
            break;
        default:
            break;
        }
    }
}

/* ---- the strip: console buttons, gamepads, Map ------------------------------------- */

static void draw_face(HDC hdc, const RECT *bounds, int pushed, int accent, int inactive)
{
    RECT rect = *bounds;
    if (accent) {
        FillRect(hdc, &rect, coleco_accent_brush());
        FrameRect(hdc, &rect, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(hdc, COLECO_ACCENT_TEXT);
    } else {
        DrawFrameControl(hdc, &rect, DFC_BUTTON,
                         DFCS_BUTTONPUSH | (pushed ? DFCS_PUSHED : 0) | (inactive ? DFCS_INACTIVE : 0));
        SetTextColor(hdc, GetSysColor(inactive ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
    }
}

static const char *strip_label(int btn)
{
    switch (btn) {
    case SB_RESET: return "Reset Console";
    case SB_CONFIG: return "Reset to CONFIG";
    case SB_MAP: return g_map_state == -2 ? "Map" : "Done";
    case SB_CLEAR: return "Clear";
    case SB_DEFAULTS: return "Defaults";
    case SB_SHOW: return "Show bindings";
    default: return "Help";
    }
}

static int strip_target(int btn)
{
    return btn == SB_RESET ? COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET)
         : btn == SB_CONFIG ? COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET_CONFIG) : -1;
}

static void segment_rect(int row, int seg, RECT *rect)
{
    const RECT *rowrc = &g_gp_row_rc[row];
    const int gap = SCL(4), width = (rowrc->right - rowrc->left - 2 * gap) / 3;
    const int left = rowrc->left + seg * (width + gap);
    SetRect(rect, left, rowrc->top + SCL(NAME_H), left + width, rowrc->top + SCL(NAME_H) + SCL(SEG_H) - 1);
}

/* One block per gamepad: its name and the controller it drives now over the
 * Automatic / Controller 1 / Controller 2 choice, the current one in the
 * accent. */
static void paint_gamepads(HDC hdc)
{
    static const char *const segs[3] = { "Automatic", "Controller 1", "Controller 2" };
    const int num = colecosession_gamepad_count(g_session);
    int idx, seg;

    SelectObject(hdc, g_bold);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    text_line(hdc, "Gamepads", &g_gp_head_rc, DT_LEFT);
    SelectObject(hdc, g_font);
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    if (num == 0) {
        text_line(hdc, "No gamepads connected - plug one in", &g_gp_row_rc[0], DT_LEFT);
        return;
    }
    if (num > PAD_ROWS) {
        char more[48];
        snprintf(more, sizeof more, "%d more not shown", num - PAD_ROWS);
        text_line(hdc, more, &g_gp_head_rc, DT_RIGHT);
    }
    for (idx = 0; idx < num && idx < PAD_ROWS; idx++) {
        const int assign = colecosession_gamepad_assignment(g_session, idx);
        const int effective = colecosession_gamepad_effective_port(g_session, idx);
        const int chosen = assign < 0 || assign >= COLECO_PORTS ? 0 : assign + 1;
        char name[128], line[32];
        RECT name_rc, eff_rc;

        name_rc = g_gp_row_rc[idx];
        name_rc.bottom = name_rc.top + SCL(NAME_H);
        eff_rc = name_rc;
        name_rc.right -= SCL(130);
        eff_rc.left = name_rc.right;
        colecosession_gamepad_name(g_session, idx, name, sizeof name);
        SelectObject(hdc, g_font);
        SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
        text_line(hdc, name, &name_rc, DT_LEFT | DT_END_ELLIPSIS);
        if (effective >= 0) snprintf(line, sizeof line, "drives controller %d", effective + 1);
        else snprintf(line, sizeof line, "drives nothing");
        SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
        text_line(hdc, line, &eff_rc, DT_RIGHT);
        for (seg = 0; seg < 3; seg++) {
            RECT rect;
            segment_rect(idx, seg, &rect);
            draw_face(hdc, &rect, 0, seg == chosen, 0);
            text_line(hdc, segs[seg], &rect, DT_CENTER);
        }
    }
}

/* What the hint line says about the part under an idle pointer. */
static void hover_text(int part, char *out, int outsz)
{
    const coleco_control *ctl;
    coleco_binding bind;
    char key[48];
    out[0] = '\0';
    if (part < 0 || part >= g_numlayout) return;
    ctl = &g_layout[part];
    if (ctl->target < 0) return;
    bind = colecosession_binding_get(g_session, ctl->target);
    key[0] = '\0';
    if (bind.keysym) colecosession_keysym_name(bind.keysym, key, sizeof key);
    snprintf(out, (size_t)outsz, "%s   Key: %s   Gamepad: %s", coleco_target_name(ctl->target),
             key[0] ? key : "none",
             bind.button != COLECO_PAD_BTN_NONE ? coleco_pad_button_name(bind.button) : "none");
}

static void paint_strip(HDC hdc)
{
    int btn;
    char hover[200];
    SelectObject(hdc, g_font);
    for (btn = 0; btn < SB_COUNT; btn++) {
        const int armed = (btn == SB_MAP && g_map_state != -2) || (btn == SB_SHOW && g_show_bindings) ||
                          (g_map_state >= 0 && strip_target(btn) == g_map_state);
        const int inactive = btn == SB_CLEAR && g_map_state < 0;
        draw_face(hdc, &g_btn_rc[btn], btn == g_mouse_action, armed, inactive);
        text_line(hdc, strip_label(btn), &g_btn_rc[btn], DT_CENTER);
    }
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    hover_text(g_map_state >= 0 ? -1 : g_hover_part, hover, sizeof hover);
    text_line(hdc, hover[0] ? hover : g_hint, &g_hint_rc, DT_LEFT | DT_END_ELLIPSIS);
}

/* The whole window: the controllers' art supersampled, its text, the strips.
 * Only what the update region needs is drawn. */
static void paint_rect(HDC hdc, const RECT *inval)
{
    RECT art;
    const int scale = 2;
    HDC mem;
    HBITMAP bmp;
    void *bits = NULL;
    BITMAPINFO bmi;
    HGDIOBJ old;

    FillRect(hdc, inval, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(hdc, TRANSPARENT);

    /* the art: the intersection with the case, at 2x */
    IntersectRect(&art, inval, &g_case_rc);
    if (!IsRectEmpty(&art)) {
        const int width = art.right - art.left, height = art.bottom - art.top;
        memset(&bmi, 0, sizeof bmi);
        bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
        bmi.bmiHeader.biWidth = width * scale;
        bmi.bmiHeader.biHeight = -height * scale;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        mem = CreateCompatibleDC(hdc);
        bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bmp) {
            HBRUSH face = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
            RECT all = { 0, 0, width * scale, height * scale };
            old = SelectObject(mem, bmp);
            FillRect(mem, &all, face);
            DeleteObject(face);
            SetBkMode(mem, TRANSPARENT);
            g_art_k = scale;
            g_art_x = art.left;
            g_art_y = art.top;
            g_art_rows = height * scale;
            art_case(mem);
            g_art_k = 1;
            SetStretchBltMode(hdc, HALFTONE);
            SetBrushOrgEx(hdc, 0, 0, NULL);
            StretchBlt(hdc, art.left, art.top, width, height, mem, 0, 0, width * scale, height * scale, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
        }
        DeleteDC(mem);
        SelectObject(hdc, g_font);
        text_parts(hdc);
    }
    paint_gamepads(hdc);
    paint_strip(hdc);
}

/* ---- pressing ----------------------------------------------------------------- */

static void invalidate_part(int part)
{
    RECT rect;
    if (!g_panel || part < 0 || part >= g_numlayout) return;
    part_rect(&g_layout[part], &rect);
    InflateRect(&rect, SCL(14), SCL(14));
    InvalidateRect(g_panel, &rect, FALSE);
}

static void invalidate_controller(int port)
{
    RECT rect;
    int idx;
    if (!g_panel) return;
    for (idx = 0; idx < g_numlayout; idx++)
        if (g_layout[idx].kind == COLECO_CTL_CAPTION && g_layout[idx].port == port) {
            part_rect(&g_layout[idx], &rect);
            rect.bottom = (int)(g_orgy + g_layh * g_unit) + (int)g_unit / 4;
            InvalidateRect(g_panel, &rect, FALSE);
        }
}

static void set_pointer(int port, unsigned mask)
{
    if (port < 0 || port >= COLECO_PORTS || g_pointer[port] == mask) return;
    g_pointer[port] = mask;
    colecosession_pointer_set(g_session, port, mask);
}

/* Lets go of what the pointer holds in this window (only that: the main
 * window releases its own keys). A console button fires on release inside
 * it, like any button. */
static void mouse_release(int fire_action)
{
    const int part = g_mouse_part, action = g_mouse_action, port = g_mouse_port;
    g_mouse_part = -1;
    g_mouse_port = -1;
    g_mouse_action = -1;
    set_pointer(0, 0);
    set_pointer(1, 0);
    if (part >= 0) invalidate_part(part);
    if (action >= 0) {
        if (fire_action)
            colecosession_sysaction_post(g_session, action == SB_RESET ? COLECO_SYSACT_RESET
                                                                        : COLECO_SYSACT_RESET_CONFIG);
        if (g_panel) InvalidateRect(g_panel, &g_btn_rc[action], FALSE);
    }
    if (port >= 0) invalidate_controller(port);
}

static void set_hint(const char *text)
{
    snprintf(g_hint, sizeof g_hint, "%s", text);
    g_hover_part = -1;
    if (g_panel) InvalidateRect(g_panel, &g_hint_rc, FALSE);
}

#define IDLE_HINT "Press a key or a button to hold it, drag the stick to tilt it. Map rebinds."

static void set_map_state(int state)
{
    g_map_state = state;
    if (state < 0) {
        if (g_panel) KillTimer(g_panel, IDT_CAPTURE);
        colecosession_gamepad_capture_cancel(g_session);
    }
    if (state == -2) {
        set_hint(IDLE_HINT);
    } else if (state == -1) {
        set_hint("Click a control to remap. Esc finishes.");
    } else {
        char msg[200];
        mouse_release(0);
        snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s. Esc cancels.", coleco_target_name(state));
        set_hint(msg);
        colecosession_gamepad_capture_begin(g_session);
        if (g_panel) SetTimer(g_panel, IDT_CAPTURE, 50, NULL);
    }
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* After a rebind: what it is now, and whom it was taken from. */
static void bound_hint(const char *what, const char *stolen)
{
    char msg[260];
    if (stolen[0]) snprintf(msg, sizeof msg, "Bound %s (it was %s's)", what, stolen);
    else snprintf(msg, sizeof msg, "Bound %s", what);
    set_hint(msg);
}

/* ---- layout --------------------------------------------------------------------- */

/* The case with the controllers in it, the console buttons, the gamepad
 * rows, the Map row: the window is this size always. */
static void layout_static(void)
{
    const int left = SCL(MARGIN);
    int ypos = SCL(MARGIN), idx, xpos, content_w, half;
    static const int widths[SB_COUNT] = { 0, 0, MAP_W, CLEAR_W, DEFAULTS_W, SHOW_W, HELP_W };

    g_layout = colecosession_controls_layout(&g_numlayout, &g_layw, &g_layh);
    g_unit = (double)UNIT96 * g_dpi / 96.0;
    content_w = (int)ceil((double)g_layw * g_unit) + 2 * SCL(CASE_PAD);
    g_total_w = 2 * left + content_w;

    SetRect(&g_case_rc, left, ypos, left + content_w, 0);
    SetRect(&g_brand_rc, g_case_rc.left + SCL(10), g_case_rc.top + SCL(10), g_case_rc.right - SCL(10),
            g_case_rc.top + SCL(10) + SCL(BRAND_H));
    g_orgx = g_case_rc.left + SCL(CASE_PAD);
    g_orgy = g_brand_rc.bottom + SCL(12);
    g_case_rc.bottom = (int)ceil(g_orgy + (double)g_layh * g_unit) + SCL(CASE_PAD);
    ypos = g_case_rc.bottom + SCL(10);

    for (idx = 0; idx < g_numlayout; idx++) {
        const coleco_control *ctl = &g_layout[idx];
        if (ctl->kind == COLECO_CTL_STICK && ctl->port >= 0 && ctl->port < COLECO_PORTS) {
            g_disc[ctl->port].ctrx = g_orgx + ((double)ctl->xpos + ctl->width / 2) * g_unit;
            g_disc[ctl->port].ctry = g_orgy + ((double)ctl->ypos + ctl->height / 2) * g_unit;
            g_disc[ctl->port].rad = (double)ctl->width / 2 * g_unit;
        }
    }

    /* the console's two buttons share the case's width */
    half = (content_w - SCL(12)) / 2;
    SetRect(&g_btn_rc[SB_RESET], left, ypos, left + half, ypos + SCL(BTN_H + 4));
    SetRect(&g_btn_rc[SB_CONFIG], left + content_w - half, ypos, left + content_w, ypos + SCL(BTN_H + 4));
    ypos += SCL(BTN_H + 4) + SCL(12);

    SetRect(&g_gp_head_rc, left, ypos, left + content_w, ypos + SCL(HEAD_H));
    ypos += SCL(HEAD_H);
    for (idx = 0; idx < PAD_ROWS; idx++) {
        SetRect(&g_gp_row_rc[idx], left, ypos, left + content_w, ypos + SCL(NAME_H + SEG_H));
        ypos += SCL(NAME_H + SEG_H + ROW_GAP);
    }
    SetRect(&g_gp_rc, left, g_gp_head_rc.top, left + content_w, ypos);
    ypos += SCL(4);

    xpos = left;
    for (idx = SB_MAP; idx < SB_COUNT; idx++) {
        if (idx == SB_HELP) xpos = left + content_w - SCL(HELP_W);
        SetRect(&g_btn_rc[idx], xpos, ypos, xpos + SCL(widths[idx]), ypos + SCL(BTN_H));
        xpos += SCL(widths[idx]) + SCL(6);
    }
    ypos += SCL(BTN_H) + SCL(6);
    SetRect(&g_hint_rc, left, ypos, left + content_w, ypos + SCL(20));
    g_total_h = ypos + SCL(20) + SCL(MARGIN);
}

static void rebuild(void)
{
    mouse_release(0);
    if (g_panel && GetCapture() == g_panel) ReleaseCapture();
    make_fonts();
    layout_static();
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* ---- polling -------------------------------------------------------------------------- */

/* What the console sees held, lit. */
static void refresh_held(void)
{
    int port;
    if (!g_panel || g_map_state != -2) return;
    for (port = 0; port < COLECO_PORTS; port++) {
        const unsigned held = colecosession_buttons_held(g_session, port);
        if (held != g_shown_held[port]) {
            g_shown_held[port] = held;
            invalidate_controller(port);
        }
    }
}

/* Hot-plugged pads and auto-assignment changes. */
static void refresh_pads(void)
{
    pads_state now;
    int idx;
    memset(&now, 0, sizeof now);
    now.generation = colecosession_gamepad_generation(g_session);
    now.count = colecosession_gamepad_count(g_session);
    for (idx = 0; idx < PAD_ROWS && idx < now.count; idx++) {
        now.assign[idx] = colecosession_gamepad_assignment(g_session, idx);
        now.effective[idx] = colecosession_gamepad_effective_port(g_session, idx);
    }
    if (memcmp(&now, &g_pads_shown, sizeof now) != 0) {
        g_pads_shown = now;
        InvalidateRect(g_panel, &g_gp_rc, FALSE);
    }
}

/* ---- keys ------------------------------------------------------------------------------ */

static void hide_panel(void)
{
    set_map_state(-2);
    mouse_release(0);
    if (GetCapture() == g_panel) ReleaseCapture();
    g_swallow_vkey = 0;
    ShowWindow(g_panel, SW_HIDE);
}

/* A key while Map is armed: 1 if Map took it. Waiting for a target, a key is
 * not typing; waiting for a binding, the key is the binding (not F9 or F12,
 * which are the app's). */
int coleco_controllers_map_key(UINT msg, WPARAM wparam, LPARAM lparam)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;

    if (!g_panel) return 0;
    if (g_swallow_vkey && wparam == g_swallow_vkey) {
        if (!down) g_swallow_vkey = 0;
        return 1;
    }
    if (g_map_state == -2) return 0;
    /* Map is the panel's: with the game window active its keys (releases
     * included) are the console's, not Map's. */
    if (GetActiveWindow() != g_panel) return 0;
    if (wparam == VK_F9 || wparam == VK_F12) return 0;
    if (msg == WM_SYSKEYDOWN && wparam == VK_F4) return 0;
    if (!down || (lparam & (1 << 30))) return 1;
    if (wparam == VK_ESCAPE) {
        set_map_state(g_map_state >= 0 ? -1 : -2);
        g_swallow_vkey = wparam;
        return 1;
    }
    if (g_map_state >= 0) {
        char stolen[128], name[32];
        const int target = g_map_state;
        const uint32_t keysym = coleco_keysym_from_msg(wparam, lparam);
        if (!keysym) return 1;          /* nothing to call it: keep waiting */
        stolen[0] = '\0';
        colecosession_binding_set_key(g_session, target, keysym, stolen, sizeof stolen);
        colecosession_keysym_name(keysym, name, sizeof name);
        g_swallow_vkey = wparam;
        set_map_state(-1);              /* stay armed: remapping several in a row is normal */
        bound_hint(name, stolen);
    }
    return 1;
}

/* A key with the window focused: 1 if taken. */
static int panel_key(UINT msg, WPARAM wparam, LPARAM lparam)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    uint32_t keysym;
    int sysact;

    if (coleco_controllers_map_key(msg, wparam, lparam)) return 1;
    if (wparam == VK_F9) {
        if (down && !(lparam & (1 << 30))) hide_panel();
        return 1;
    }
    if (coleco_app_key(wparam)) {
        /* the main window's shortcuts, as if it had the focus */
        if (g_parent && down) SendMessageA(g_parent, msg, wparam, lparam);
        return 1;
    }
    keysym = coleco_keysym_from_msg(wparam, lparam);
    if (down && msg == WM_KEYDOWN && keysym && (sysact = colecosession_key_sysaction(g_session, keysym)) >= 0) {
        /* the main window runs these */
        if (!(lparam & (1 << 30))) colecosession_sysaction_post(g_session, sysact);
        return 1;
    }
    return coleco_forward_key(g_session, msg, wparam, lparam);
}

/* Exactly the laid-out client size, whatever the frame is at this DPI. */
static void fit_client(void)
{
    RECT wnd, client;
    GetWindowRect(g_panel, &wnd);
    GetClientRect(g_panel, &client);
    if (client.right == g_total_w && client.bottom == g_total_h) return;
    SetWindowPos(g_panel, NULL, 0, 0, (wnd.right - wnd.left) - client.right + g_total_w,
                 (wnd.bottom - wnd.top) - client.bottom + g_total_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The window's outer size for the laid-out client. */
static void window_size(HWND hwnd, SIZE *dims)
{
    RECT wnd, client;
    if (hwnd && GetWindowRect(hwnd, &wnd) && GetClientRect(hwnd, &client) && client.right > 0) {
        dims->cx = (wnd.right - wnd.left) - client.right + g_total_w;
        dims->cy = (wnd.bottom - wnd.top) - client.bottom + g_total_h;
    } else {
        SetRect(&wnd, 0, 0, g_total_w, g_total_h);
        AdjustWindowRectEx(&wnd, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
        dims->cx = wnd.right - wnd.left;
        dims->cy = wnd.bottom - wnd.top;
    }
}

static void rescale(int dpi)
{
    g_dpi = dpi;
    rebuild();
}

/* ---- the mouse ------------------------------------------------------------------------ */

/* The layout part under a client point, or -1. */
static int part_at(POINT pnt)
{
    if (!g_layout) return -1;
    return colecosession_controls_hit((float)((pnt.x - g_orgx) / g_unit), (float)((pnt.y - g_orgy) / g_unit));
}

/* A stick drag: where the pointer is on the disc decides what is held, out
 * over the wedges and beyond them while dragging, with nothing near the
 * middle. */
static void stick_follow(int port, POINT pnt)
{
    const double delx = pnt.x - g_disc[port].ctrx, dely = pnt.y - g_disc[port].ctry;
    set_pointer(port, colecosession_stick_from_vector((float)delx, (float)dely, (float)g_disc[port].rad));
    invalidate_controller(port);
    refresh_held();
}

static void mouse_down(HWND hwnd, POINT pnt)
{
    int part, row, seg, btn;
    SetFocus(hwnd);
    if (g_hover_part >= 0) {
        g_hover_part = -1;
        InvalidateRect(hwnd, &g_hint_rc, FALSE);
    }

    for (btn = 0; btn < SB_COUNT; btn++) {
        if (!PtInRect(&g_btn_rc[btn], pnt)) continue;
        switch (btn) {
        case SB_MAP: set_map_state(g_map_state == -2 ? -1 : -2); return;
        case SB_CLEAR:
            if (g_map_state >= 0) {
                char stolen[16];
                colecosession_binding_set_key(g_session, g_map_state, 0, stolen, sizeof stolen);
                colecosession_binding_set_button(g_session, g_map_state, COLECO_PAD_BTN_NONE, stolen, sizeof stolen);
                set_hint("Cleared. Press a key or a gamepad button to bind it again");
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return;
        case SB_DEFAULTS:
            colecosession_bindings_reset(g_session);
            set_hint("Every key and button is back to its default");
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        case SB_SHOW:
            g_show_bindings = !g_show_bindings;
            colecosession_set_int(g_session, "controllers_show_bindings", g_show_bindings);
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        case SB_HELP: coleco_help_show(hwnd, "controllers"); return;
        default:
            if (g_map_state != -2) {                 /* armed: picks the action */
                set_map_state(strip_target(btn));
            } else {
                mouse_release(0);
                g_mouse_action = btn;
                SetCapture(hwnd);
                InvalidateRect(hwnd, &g_btn_rc[btn], FALSE);
            }
            return;
        }
    }
    for (row = 0; row < PAD_ROWS && row < colecosession_gamepad_count(g_session); row++)
        for (seg = 0; seg < 3; seg++) {
            RECT rect;
            segment_rect(row, seg, &rect);
            if (PtInRect(&rect, pnt)) {
                /* 0 = automatic (connection order), 1-2 = that controller */
                colecosession_gamepad_assign(g_session, row, seg - 1);
                refresh_pads();
                InvalidateRect(hwnd, &g_gp_rc, FALSE);
                return;
            }
        }

    part = part_at(pnt);
    if (part < 0) return;
    {
        const coleco_control *ctl = &g_layout[part];
        if (g_map_state != -2) {
            /* armed: picks (or changes) the target */
            if (ctl->target >= 0 && ctl->kind != COLECO_CTL_STICK) set_map_state(ctl->target);
            return;
        }
        mouse_release(0);
        switch (ctl->kind) {
        case COLECO_CTL_STICK:
        case COLECO_CTL_DIRECTION:
            g_mouse_port = ctl->port;
            stick_follow(ctl->port, pnt);
            break;
        case COLECO_CTL_KEY:
        case COLECO_CTL_FIRE:
            g_mouse_part = part;
            set_pointer(ctl->port, ctl->act_mask);
            break;
        default:
            return;
        }
        invalidate_part(part);
        SetCapture(hwnd);
        refresh_held();
    }
}

static void mouse_move(HWND hwnd, POINT pnt)
{
    if (g_mouse_port >= 0) {
        stick_follow(g_mouse_port, pnt);
        return;
    }
    if (g_mouse_part >= 0) {
        /* dragged off what it holds: let go, as a button does */
        if (g_mouse_part >= g_numlayout || part_at(pnt) != g_mouse_part) {
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
            refresh_held();
        }
        return;
    }
    if (g_mouse_action >= 0) {
        if (!PtInRect(&g_btn_rc[g_mouse_action], pnt)) {
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        return;
    }
    {   /* idle: the hint line says what drives the part under the pointer */
        const int part = part_at(pnt);
        if (!g_tracking) {
            TRACKMOUSEEVENT track;
            memset(&track, 0, sizeof track);
            track.cbSize = sizeof track;
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            g_tracking = TrackMouseEvent(&track) != 0;
        }
        if (part != g_hover_part) {
            g_hover_part = part;
            InvalidateRect(hwnd, &g_hint_rc, FALSE);
        }
    }
}

static void mouse_up(HWND hwnd)
{
    if (g_mouse_part >= 0 || g_mouse_port >= 0 || g_mouse_action >= 0) {
        mouse_release(1);
        refresh_held();
    }
    if (GetCapture() == hwnd) ReleaseCapture();
}

/* ---- the window ----------------------------------------------------------------------- */

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC hdc = BeginPaint(hwnd, &paint);
        /* Off-screen first: the held highlights repaint at 25 Hz. */
        RECT inval = paint.rcPaint;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp, oldfont;
        const int width = inval.right - inval.left, height = inval.bottom - inval.top;
        if (width <= 0 || height <= 0) { EndPaint(hwnd, &paint); return 0; }
        mem = CreateCompatibleDC(hdc);
        bmp = CreateCompatibleBitmap(hdc, width, height);
        oldbmp = SelectObject(mem, bmp);
        oldfont = SelectObject(mem, g_font);
        SetViewportOrgEx(mem, -inval.left, -inval.top, NULL);
        paint_rect(mem, &inval);
        SetViewportOrgEx(mem, 0, 0, NULL);
        BitBlt(hdc, inval.left, inval.top, width, height, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldfont);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_down(hwnd, pnt);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_move(hwnd, pnt);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = 0;
        if (g_hover_part >= 0) {
            g_hover_part = -1;
            InvalidateRect(hwnd, &g_hint_rc, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        /* released wherever the mouse ended up: capture means this arrives
         * even if the pointer left the part */
        mouse_up(hwnd);
        return 0;
    case WM_CAPTURECHANGED:
        /* lost the mouse some other way (Alt+Tab mid-press): let go */
        if ((HWND)lparam != hwnd) {
            mouse_release(0);
            refresh_held();
        }
        return 0;

    case WM_TIMER:
        if (wparam == IDT_CAPTURE) {
            int button;
            if (g_map_state < 0) {
                KillTimer(hwnd, IDT_CAPTURE);
            } else if (colecosession_gamepad_capture_poll(g_session, &button)) {
                char stolen[128];
                const int target = g_map_state;
                stolen[0] = '\0';
                colecosession_binding_set_button(g_session, target, button, stolen, sizeof stolen);
                set_map_state(-1);   /* stay armed: remapping several in a row is normal */
                bound_hint(coleco_pad_button_name(button), stolen);
            }
        } else if (wparam == IDT_HELD) {
            refresh_held();
        } else if (wparam == IDT_PADS) {
            refresh_pads();
        }
        return 0;

    /* WM_SYSKEY* too: a bound Alt is the console's */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        if (panel_key(msg, wparam, lparam)) return 0;
        break;
    case WM_SYSCHAR:
        /* Alt+letter: the main window's menu mnemonics, as when it has the focus */
        if (wparam != ' ' && g_parent) { SendMessageA(g_parent, msg, wparam, lparam); return 0; }
        break;

    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            const HWND other = (HWND)lparam;
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
            g_swallow_vkey = 0;     /* its release goes elsewhere */
            /* Leaving the app: nothing typed here stays held (within it, the
             * window that gets the key-up lets go). */
            if (!other || GetWindowThreadProcessId(other, NULL) != GetCurrentThreadId())
                colecosession_release_all(g_session);
            refresh_held();
        }
        break;                      /* DefWindowProc gives the window the focus */
    case WM_SHOWWINDOW:
        if (wparam) {
            refresh_held();
            SetTimer(hwnd, IDT_HELD, 40, NULL);
            SetTimer(hwnd, IDT_PADS, 250, NULL);
        } else {
            KillTimer(hwnd, IDT_HELD);
            KillTimer(hwnd, IDT_PADS);
        }
        break;
    case WM_GETMINMAXINFO:
        /* Fixed: the size limits say so too, so neither maximizing nor a
         * compositor (Wine passes them on) makes it any other size. */
        if (g_total_w) {
            MINMAXINFO *limits = (MINMAXINFO *)lparam;
            SIZE dims;
            window_size(hwnd, &dims);
            limits->ptMinTrackSize.x = limits->ptMaxTrackSize.x = limits->ptMaxSize.x = dims.cx;
            limits->ptMinTrackSize.y = limits->ptMaxTrackSize.y = limits->ptMaxSize.y = dims.cy;
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        const RECT *suggested = (const RECT *)lparam;
        rescale(HIWORD(wparam));
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        fit_client();
        return 0;
    }
    case WM_CLOSE:
        /* Hide, do not destroy: a remap and the position survive closing it. */
        hide_panel();
        return 0;
    case WM_DESTROY:
        /* with the main window: the next toggle starts afresh */
        mouse_release(0);
        set_map_state(-2);
        g_panel = NULL;
        free_fonts();
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static int create_panel(void)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    RECT want;

    {   /* "controllers_show_bindings"; the older spellings are read when it is absent */
        int saved = colecosession_get_int(g_session, "controllers_show_bindings", -1);
        if (saved < 0) saved = colecosession_get_int(g_session, "controllers_bindings", -1);
        if (saved < 0) saved = colecosession_get_int(g_session, "controllers_key_names", -1);
        if (saved < 0) saved = colecosession_get_int(g_session, "show_bindings", -1);
        g_show_bindings = saved > 0;
    }

    if (!registered) {
        WNDCLASSEXA wcls;
        memset(&wcls, 0, sizeof wcls);
        wcls.cbSize = sizeof wcls;
        wcls.lpfnWndProc = panel_proc;      /* no CS_DBLCLKS: fast clicks stay press/release pairs */
        wcls.hInstance = inst;
        wcls.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wcls.lpszClassName = PANEL_CLASS;
        registered = RegisterClassExA(&wcls) != 0;
    }
    g_dpi = coleco_window_dpi(g_parent);
    make_fonts();
    layout_static();

    SetRect(&want, 0, 0, g_total_w, g_total_h);
    /* A tool window with a caption and no thick frame or maximize box: it is
     * exactly the size of its contents, and stays off the taskbar. */
    AdjustWindowRectEx(&want, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
    g_panel = CreateWindowExA(PANEL_EXSTYLE, PANEL_CLASS, "Controllers", PANEL_STYLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left, want.bottom - want.top,
                              g_parent, NULL, inst, NULL);
    if (!g_panel) return 0;
    coleco_help_set_context(g_panel, "controllers", 0);    /* F1 is bindable here */
    rescale(coleco_window_dpi(g_panel));    /* placed on another monitor than the main window */
    fit_client();
    memset(&g_pads_shown, 0, sizeof g_pads_shown);
    memset(g_shown_held, 0, sizeof g_shown_held);
    g_hover_part = -1;
    set_map_state(-2);
    return 1;
}

static void show_panel(void)
{
    refresh_pads();
    ShowWindow(g_panel, SW_SHOW);
    SetForegroundWindow(g_panel);
}

void coleco_controllers_window_show(HWND parent, colecosession *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_panel && !create_panel())
        return;
    if (!IsWindowVisible(g_panel))
        show_panel();
    else
        SetForegroundWindow(g_panel);
}

void coleco_controllers_window_toggle(HWND parent, colecosession *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_panel && !create_panel())
        return;
    if (IsWindowVisible(g_panel))
        hide_panel();
    else
        show_panel();
}

int coleco_controllers_window_visible(void)
{
    return g_panel && IsWindowVisible(g_panel);
}

void coleco_controllers_window_refresh(void)
{
    if (!g_panel)
        return;
    if (IsWindowVisible(g_panel)) {
        refresh_pads();
        InvalidateRect(g_panel, NULL, FALSE);
    }
}
