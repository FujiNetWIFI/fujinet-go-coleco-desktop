/*
 * colecovidtab -- the Video tab's controller. All four frontends draw the same
 * tab from it, so what they own is widgets: a view and zoom chooser, overlay
 * toggles, a picture, the inspector and the sections. Modelled on the MSX and
 * Intellivision controllers (msxvidtab.h, intvvidtab.h), so a frontend that
 * has one ports the other by changing names:
 *
 *   - the machine is only COPIED: a finished frame (colecovidcap.h) is
 *     captured once per frame on the emulator thread, and a parked machine is
 *     read live. Decoding, scaling and overlays run on the caller's (UI)
 *     thread;
 *   - the picture is rebuilt only when something it shows changed, into one
 *     reused buffer;
 *   - hover and pin are state here, so a mouse move never re-renders the
 *     picture: draw the marks from colecovid_tab_marks() over the cached image;
 *   - pixels are 0xFFRRGGBB (opaque), whatever the toolkit does with alpha;
 *   - one table of overlays and one env-var parser, so the frontends cannot
 *     disagree.
 *
 * The TMS9918A draws every line from the registers as they stand when the
 * line is drawn. The per-line log of a captured frame (the registers as each
 * line finished) therefore says what each line was made from: the Decoded
 * views are drawn band by band (a split screen or a mode switch mid-frame
 * shows as bands), the Lines section lists the bands, and the Registers
 * section shows how often the registers changed down the frame. The log is
 * line-granular: a change in the middle of a line lands on that line.
 *
 * Views: Display is the last finished frame, exactly the picture of the main
 * window (256 x 212, the border included); Decoded redraws one source from
 * VRAM and the registers: the screen (the name table in Graphics I and II,
 * Text and Multicolor), the pattern sheet, the sprites or the palette; Beam
 * is the machine's frame buffer as it stands with the lines the beam has not
 * reached yet dimmed (stopped only); Values shows each name table entry as a
 * flat false colour of its value.
 *
 * Not decoded: the undefined mode combinations (M1+M2, ...) the core draws
 * as Text or Multicolor by priority; they are named as such. Multicolor has
 * no pattern sheet (its pattern table holds colours).
 *
 * UI thread only. Not thread-safe.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef COLECOVIDTAB_H
#define COLECOVIDTAB_H

#include <stdint.h>

#include "colecodebug.h"
#include "colecovidcap.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct colecovid_tab colecovid_tab;

enum colecovid_view {
    COLECOVID_V_DISPLAY = 0,    /* the last finished frame, 256 x 212 */
    COLECOVID_V_DECODED,        /* one source rebuilt from VRAM and the registers (see colecovid_source) */
    COLECOVID_V_BEAM,           /* the frame buffer as it stands, the lines below the beam dimmed (stopped) */
    COLECOVID_V_VALUES,         /* each name table entry as a flat false colour of its value */
    COLECOVID_V_COUNT
};

/* What the Decoded view redraws. */
enum colecovid_source {
    COLECOVID_SRC_SCREEN = 0,   /* the name table in its mode (256 x 192) */
    COLECOVID_SRC_PATTERNS,     /* the pattern generator table, 32 tiles a row (one bank in Graphics II) */
    COLECOVID_SRC_SPRITES,      /* the 32 sprites at native size, 8 x 4 */
    COLECOVID_SRC_PALETTE,      /* the 16 colours */
    COLECOVID_SRC_COUNT
};

/* Overlays: one table, bit per entry. */
enum {
    COLECOVID_OV_GRID    = 1u << 0, /* the tile grid (Display, Screen, Values, Beam) */
    COLECOVID_OV_SPRITES = 1u << 1, /* each sprite's box (Display, Beam, Screen) */
    COLECOVID_OV_BORDER  = 1u << 2, /* the border and blanking, tinted (Display, Beam) */
    COLECOVID_OV_BEAM    = 1u << 3, /* where the beam is (stopped) */
    COLECOVID_OV_HEAT    = 1u << 4, /* name table entries that changed lately */
    COLECOVID_OV_SPLITS  = 1u << 5, /* the lines where the registers changed (Display, Screen, Beam) */
    COLECOVID_OV_ALL     = 0x3Fu
};
typedef struct {
    unsigned bit;
    const char *name;           /* the env name: grid, sprites, ... */
    const char *label;          /* the checkbox text */
} colecovid_overlay_info;
const colecovid_overlay_info *colecovid_overlays(int *count);
const char *colecovid_view_name(int view);          /* "Display", ... */
const char *colecovid_view_key(int view);           /* "display", ... (env) */
const char *colecovid_source_name(int source);      /* "Screen", ... */
const char *colecovid_source_key(int source);       /* "screen", ... */

/* Right-hand sections. */
enum { COLECOVID_S_PIXEL = 0, COLECOVID_S_REGISTERS, COLECOVID_S_COLOURS, COLECOVID_S_LINES,
       COLECOVID_S_TIMING, COLECOVID_S_VRAM, COLECOVID_S_COUNT };
const char *colecovid_section_name(int section);
const char *colecovid_section_key(int section);

/* dbg is the debugger the tab asks whether the machine is parked, adds write
 * breakpoints to and pokes VRAM through; NULL (tests) makes every Break on...
 * return -1 and the machine count as running. The tab asks the host to
 * capture frames until it is freed. */
colecovid_tab *colecovid_tab_new(colecodebug *dbg);
void           colecovid_tab_free(colecovid_tab *tab);

/* ---- what to show (they mark the picture stale) ------------------------------------ */
void     colecovid_tab_set_view(colecovid_tab *tab, int view);
int      colecovid_tab_view(const colecovid_tab *tab);
void     colecovid_tab_set_source(colecovid_tab *tab, int source);
int      colecovid_tab_source(const colecovid_tab *tab);
void     colecovid_tab_set_overlays(colecovid_tab *tab, unsigned ovl);
unsigned colecovid_tab_overlays(const colecovid_tab *tab);
/* 1, 2, 3, 4, 6, 8, 12, 16, or 0 for "fit the viewport" (the largest of
 * those that fits both ways) */
void     colecovid_tab_set_zoom(colecovid_tab *tab, int zoom);
int      colecovid_tab_zoom(const colecovid_tab *tab);
/* the room the picture has, for Fit; call when the widget resizes */
void     colecovid_tab_set_viewport(colecovid_tab *tab, int wide, int high);
/* the zoom steps the chooser offers (0 = Fit first) */
const int *colecovid_zooms(int *count);
/* The Patterns source in Graphics II shows one of three 256-tile banks. */
void     colecovid_tab_set_bank(colecovid_tab *tab, int bank);
int      colecovid_tab_bank(const colecovid_tab *tab);
/* The first byte of the VRAM section's hex dump (the VRAM pane); set by the
 * pane's address box, and by a pin on a pixel it is made from. */
void     colecovid_tab_set_vram_base(colecovid_tab *tab, uint32_t addr);
uint32_t colecovid_tab_vram_base(const colecovid_tab *tab);

/*
 * The scripted-capture variables, read once by colecovid_tab_new:
 *   COLECO_DEBUGGER_VIDEO_VIEW      display decoded beam values
 *   COLECO_DEBUGGER_VIDEO_SOURCE    screen patterns sprites palette   (the Decoded view's source)
 *   COLECO_DEBUGGER_VIDEO_OVERLAYS  grid,sprites,border,beam,heat,splits | all | none
 *   COLECO_DEBUGGER_VIDEO_ZOOM      1..16 or fit
 *   COLECO_DEBUGGER_VIDEO_SECTION   pixel registers colours lines timing vram   (names, never indexes)
 * Returns the starting section, COLECOVID_S_PIXEL when unset.
 */
int colecovid_tab_env_section(const colecovid_tab *tab);

/* ---- refresh ------------------------------------------------------------------------ */
#define COLECOVID_TAB_PICTURE 1u    /* repaint the picture (colecovid_tab_picture) */
#define COLECOVID_TAB_TABLES  2u    /* refill the status line and the sections */
#define COLECOVID_TAB_OFF     4u    /* no machine: show colecovid_tab_why() instead of a picture */

/* Take the machine's state, update the trackers, and say what to redraw.
 * force: redraw everything. */
unsigned colecovid_tab_refresh(colecovid_tab *tab, int force);

/* For tests: use this capture instead of the machine. stopped: the machine
 * counts as parked, so the Beam view works (cap->live says the VDP part is a
 * parked machine's, with cap->beam_line the beam). */
void colecovid_tab_feed(colecovid_tab *tab, const colecovid_capture *cap, int stopped);

/* ---- the picture ------------------------------------------------------------------- */
/*
 * The rendered picture, 0xFFRRGGBB, valid until the next refresh or set_*. NULL
 * when this view cannot be drawn: colecovid_tab_why() says why, and the tab
 * should show that text.
 */
const uint32_t *colecovid_tab_picture(colecovid_tab *tab, int *wide, int *high);
const char     *colecovid_tab_why(const colecovid_tab *tab);
int             colecovid_tab_effective_zoom(const colecovid_tab *tab);

/* ---- the tables -------------------------------------------------------------------- */
const char *colecovid_tab_status(const colecovid_tab *tab);     /* two lines, '\n' between */

/* VDP registers R0-R7 and the status register. */
typedef struct {
    const char *name;           /* "R0", ..., "ST" */
    char value[8];              /* "05" */
    char meaning[200];
    int age;                    /* refreshes since it last changed (COLECOVID_REG_FADE+: not recent) */
    int writes;                 /* lines of the captured frame where it changed */
    int first_line;             /* the first such line, -1 when none */
} colecovid_reg;
#define COLECOVID_REG_FADE 12
int colecovid_tab_regs(const colecovid_tab *tab, const colecovid_reg **out);

/* The 16 colours (the TMS9918A's palette is fixed). */
typedef struct {
    int index;                  /* 0..15 */
    const char *name;           /* "Medium green (2)" */
    uint32_t rgb;               /* 0xFFRRGGBB */
    long uses;                  /* pixels of the finished frame with exactly this colour */
    char role[48];              /* "backdrop (R7)", "" when unused by the registers */
    int inspected;              /* the inspected pixel has it */
} colecovid_swatch;
int colecovid_tab_swatches(const colecovid_tab *tab, const colecovid_swatch **out);

/* The Lines table: one entry for each band of display lines drawn from the
 * same register values (a program that never changes them in the frame has
 * one). */
typedef struct {
    int first, last;            /* display lines, inclusive (0 = the first line of the screen) */
    int abs_first;              /* the same in absolute frame lines (equal: the display starts the frame) */
    char mode[24];              /* "Graphics II" */
    unsigned name_table, pattern_table, colour_table, sprite_attr, sprite_pattern;   /* VRAM addresses */
    int backdrop;               /* the colour index of the backdrop */
    int writes;                 /* registers that changed on its first line */
    char text[256];             /* ready-made line */
} colecovid_band;
int colecovid_tab_lines(const colecovid_tab *tab, const colecovid_band **out);

/* the timing section, as ready-made "label\tvalue" lines */
int  colecovid_tab_timing(const colecovid_tab *tab, const char *const **lines);
/* where the beam is in the whole frame, 0..1000 (0 when unknown) */
int  colecovid_tab_beam_permille(const colecovid_tab *tab);

/* A section as plain text (the console's `video [section]` shares it). */
int  colecovid_tab_text(colecovid_tab *tab, int section, char *dst, int dstsz);
/* A section by name or prefix ("pixel", "reg", ...); -1 if none. */
int  colecovid_section_from_name(const char *name);
/* The console's `video`: attaches to the machine for the call, no tab needed.
 * section NULL or "" is the status and the registers. Returns the length. */
int  colecovid_tab_text_for(colecodebug *dbg, const char *section, char *dst, int dstsz);

/* Installs colecovid_tab_text_for as the console's `video [section]` text
 * (colecodebug_set_video_text) for dbg. Call once the engine exists. */
void colecovid_tab_register_console(colecodebug *dbg);

/* The VRAM pane's poke line: a hex address, then optionally ':' or '=' and
 * hex bytes (colecovdp_parse_poke). The bytes go through colecodebug_vdp_write
 * (the address counter and read-ahead are not touched); with only an address
 * the pane goes to it. Returns the bytes written (0 for a go-to), or -1 with
 * the reason in err. The picture is rebuilt on the next refresh. */
int  colecovid_tab_poke(colecovid_tab *tab, const char *line, char *err, int errsz);

/* ---- hover, pin ---------------------------------------------------------------------- */
enum { COLECOVID_PK_NONE = 0, COLECOVID_PK_BORDER, COLECOVID_PK_TILE,
       COLECOVID_PK_SPRITE, COLECOVID_PK_PATTERN, COLECOVID_PK_PALETTE, COLECOVID_PK_VALUE };
/* What Break on writes watches for each target. */
enum { COLECOVID_BK_VRAM = 0, COLECOVID_BK_REGISTER };

typedef struct {
    int valid;
    int picx, picy;             /* picture pixel (base picture, before the zoom) */
    int kind;                   /* COLECOVID_PK_* */
    int markx, marky, markw, markh;     /* the object under the pointer, in picture pixels */
    int vdp_x, vdp_y;           /* the VDP pixel (256 wide, the display's lines), -1 when the view has none */
    int line;                   /* the display line (= the absolute frame line), -1 when none */
    int colour;                 /* palette index of the pixel, -1 if none */
    int cell;                   /* name table entry under it, -1 */
    int name;                   /* the tile it shows, -1 */
    int sprite;                 /* the front sprite covering it, -1 */
    unsigned sprite_mask;       /* every sprite box that holds it, bit per sprite */
    unsigned vram_addr;         /* the VRAM address the pixel is made from (name entry, sprite byte, ...), 0xFFFFFFFF if none */
    int nbreak;                 /* write breakpoint targets (up to 3 ranges) */
    unsigned break_lo[3], break_hi[3];
    int break_kind[3];          /* COLECOVID_BK_*: lo..hi is a VRAM range, or a register number */
    uint32_t rgb;               /* 0xFFRRGGBB */
    char text[640];             /* ready-made inspector text */
} colecovid_pixel;

/* x, y in picture pixels at the current zoom; -1,-1 when the pointer left.
 * Returns 1 when the inspector changed. */
int  colecovid_tab_hover(colecovid_tab *tab, int posx, int posy);
void colecovid_tab_pin(colecovid_tab *tab, int posx, int posy);     /* a click; the same cell unpins */
void colecovid_tab_unpin(colecovid_tab *tab);
/* the pinned pixel if any, else the hovered one, else NULL */
const colecovid_pixel *colecovid_tab_pixel(const colecovid_tab *tab);
/* rectangles x,y,w,h in picture pixels (zoom applied) for the marks drawn over
 * the picture; bit0 of the result: hover valid, bit1: pin valid */
int  colecovid_tab_marks(const colecovid_tab *tab, int hover[4], int pin[4]);
/*
 * "Break on writes": breakpoints on what the inspected pixel is made from.
 * What the engine can watch is the CPU's port writes (the VDP is not on the
 * bus the hook sees), so:
 *   - VRAM (a name table entry, a pattern or colour byte, a sprite attribute
 *     or pattern): an OUT to the VDP data port ($A0-$BE, bit 0 clear) whose
 *     VRAM address counter is in the range. The engine reads the counter
 *     (adamcore_vdp_state) inside the hook, before the write lands;
 *   - a register (the backdrop, the mode bits): an OUT to the control port
 *     ($A1-$BF, bit 0 set) that is the SECOND byte of a pair (the latch
 *     state, adamcore_vdp_latch_state) with the value $80 + the register.
 * The palette is fixed, so it has no target. -1 when the pixel has no such
 * target, without a debugger or while the engine lacks the call. Not added
 * twice. Returns the first breakpoint's id, or -1. */
int  colecovid_tab_break_on_pixel(colecovid_tab *tab);
/* 1 when "Break on writes" has something to break on now */
int  colecovid_tab_can_break(const colecovid_tab *tab);

#ifdef __cplusplus
}
#endif

#endif
