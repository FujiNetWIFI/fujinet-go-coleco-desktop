/*
 * colecodebug -- the toolkit-agnostic debugger engine for FujiNet Go Adam
 * Desktop. Wraps adamcore's debug interface (adamcore_debug.h) with
 * pause/step/run-to execution control, breakpoint management, an
 * instruction-history ring, a Z80 disassembler, EOS/OS7 symbol tables, and
 * VDP visualizer decoders. One engine exists per session
 * (colecosession_debugger()); native GTK/Qt views sit on top.
 *
 * Threading: control calls (pause/resume/step/breakpoints) are safe from
 * the UI thread. The stop callback fires on the EMULATOR thread -- marshal
 * to the UI (g_idle_add / QMetaObject::invokeMethod). State reads are
 * consistent while paused.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COLECODEBUG_H
#define COLECODEBUG_H

#include <stdint.h>

#include "adamcore_debug.h"
#include "colecosession.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    COLECODBG_STOP_PAUSE = 0,   /* explicit pause request */
    COLECODBG_STOP_BREAKPOINT,  /* PC breakpoint hit (before execution) */
    COLECODBG_STOP_STEP,        /* step into/over/out completed */
    COLECODBG_STOP_RUNTO,       /* run-to-address reached */
} colecodebug_stop_reason;

/* ---- execution control --------------------------------------------------- */
void colecodebug_pause(colecodebug *d);   /* halts at the next instruction */
void colecodebug_resume(colecodebug *d);
int  colecodebug_is_paused(colecodebug *d);
void colecodebug_step_into(colecodebug *d);           /* while paused */
void colecodebug_step_over(colecodebug *d);           /* while paused */
void colecodebug_step_out(colecodebug *d);            /* while paused */
void colecodebug_run_to(colecodebug *d, uint16_t addr);

/* Fires on the emulator thread whenever execution stops (and also after
 * each completed step). */
void colecodebug_set_stop_callback(colecodebug *d,
                                 void (*cb)(void *ud,
                                            colecodebug_stop_reason reason,
                                            uint16_t pc),
                                 void *ud);

/* ---- breakpoints ---------------------------------------------------------- */
void colecodebug_bp_toggle(colecodebug *d, uint16_t addr);
void colecodebug_bp_set(colecodebug *d, uint16_t addr);
void colecodebug_bp_clear(colecodebug *d, uint16_t addr);
void colecodebug_bp_clear_all(colecodebug *d);
int  colecodebug_bp_is_set(colecodebug *d, uint16_t addr);
int  colecodebug_bp_list(colecodebug *d, uint16_t *out, int max);

/* ---- CPU / memory (consistent while paused) ------------------------------- */
void colecodebug_get_regs(colecodebug *d, adamcore_z80_regs *out);
void colecodebug_set_regs(colecodebug *d, const adamcore_z80_regs *in);
int  colecodebug_read_mem(colecodebug *d, uint16_t addr, uint8_t *dst, int n);
int  colecodebug_write_mem(colecodebug *d, uint16_t addr, const uint8_t *src,
                         int n);

/* ---- disassembly ---------------------------------------------------------- */
#define COLECODASM_JUMP     0x01
#define COLECODASM_CALL     0x02
#define COLECODASM_RET      0x04
#define COLECODASM_COND     0x08
#define COLECODASM_RELATIVE 0x10
#define COLECODASM_BLOCK    0x20 /* LDIR/CPIR/INIR/OTIR family */
#define COLECODASM_HALT     0x40

typedef struct {
    uint16_t addr;
    uint8_t len;      /* 1..4 */
    uint8_t bytes[4];
    char text[32];    /* "LD (IX+5),A" */
    uint16_t target;  /* jump/call destination when flags say so */
    uint8_t flags;    /* COLECODASM_* */
    const char *symbol; /* label at addr from the symbol tables, or NULL */
} colecodasm_line;

/* Disassembles count instructions starting at addr (reading through the
 * current memory map, side-effect free). Returns lines written. */
int colecodebug_disassemble(colecodebug *d, uint16_t addr, int count,
                          colecodasm_line *out);

/* ---- instruction history (trace ring) ------------------------------------- */
typedef struct {
    uint16_t pc, sp, af, bc, de, hl;
    uint8_t len;
    uint8_t bytes[4];
    uint64_t cycles;
} colecotrace_entry;

void colecodebug_trace_enable(colecodebug *d, int enable);
int  colecodebug_trace_enabled(colecodebug *d);
/* Copies up to max entries, newest first. Returns entries written. */
int  colecodebug_trace_read(colecodebug *d, colecotrace_entry *out, int max);
void colecodebug_trace_clear(colecodebug *d);

/* ---- symbols --------------------------------------------------------------
 * .sym format: "HHHH NAME [; comment]" per line, '#' comments. The built-in
 * EOS and OS7 tables (generated from the Drushel EOS-5 disassembly, the
 * eoslib jump-table names, and the os7lib listing) load automatically when
 * the engine is created. */
int colecodebug_symbols_load(colecodebug *d, const char *path, const char *table);
const char *colecodebug_symbol_at(colecodebug *d, uint16_t addr,
                                uint16_t *offset);
int colecodebug_symbol_find(colecodebug *d, const char *name, uint16_t *addr);

/* ---- VDP ------------------------------------------------------------------ */
typedef struct {
    uint8_t vram[0x4000];
    uint8_t regs[8];
    uint8_t status;
    uint16_t addr;
    uint16_t palette565[16];
} colecovdp_snapshot;

void colecodebug_vdp_snapshot(colecodebug *d, colecovdp_snapshot *out);

/* Live VRAM access, addresses wrapping at 16K. Reads are side-effect free;
 * writes go straight into the VDP's RAM array without touching the address
 * register or the read-ahead latch, so poking VRAM cannot desynchronize a
 * transfer the running program is in the middle of. Both are safe from the
 * UI thread (a write racing the beam just shows up on the next line, the
 * same as one from the CPU). */
int colecodebug_vdp_read(colecodebug *d, uint16_t addr, uint8_t *dst, int n);
int colecodebug_vdp_write(colecodebug *d, uint16_t addr, const uint8_t *src,
                        int n);

/* Parses a "poke line": a hex address, then optionally a ':' or '=' and any
 * number of whitespace/comma-separated hex bytes ("1800", "$1800: 41 42",
 * "1800 = 4142FF"). Tokens longer than two digits are taken as a run of
 * bytes so a pasted hex dump works. Returns the byte count (0 when only an
 * address was given, i.e. "go to"), or -1 if the text does not parse. */
int colecovdp_parse_poke(const char *text, uint16_t *addr, uint8_t *bytes,
                       int max);

/* ---- VDP tables / registers ----------------------------------------------- */

typedef enum {
    COLECOVDP_MODE_GRAPHICS1 = 0,
    COLECOVDP_MODE_GRAPHICS2,
    COLECOVDP_MODE_MULTICOLOR,
    COLECOVDP_MODE_TEXT,
    COLECOVDP_MODE_INVALID, /* an M1/M2/M3 combination the VDP does not define */
} colecovdp_mode;

typedef struct {
    uint16_t base;  /* VRAM address the mode fetches this table from */
    uint16_t size;  /* bytes the mode actually fetches (0 = mode unused) */
    uint16_t mask;  /* Graphics II AND-mask over the table index (else 0) */
} colecovdp_table;

typedef struct {
    colecovdp_mode mode;
    const char *mode_name;
    colecovdp_table name, color, pattern, sprite_attr, sprite_pattern;
    int display_on;        /* R1 BLANK */
    int irq_on;            /* R1 IE */
    int vram_16k;          /* R1 M/S: 16K vs 4K RAM */
    int ext_video;         /* R0 EXTVID */
    int sprites_16x16;     /* R1 SIZE */
    int sprites_magnified; /* R1 MAG */
    uint8_t backdrop;      /* R7 low nibble */
    uint8_t text_fg;       /* R7 high nibble (text mode only) */
} colecovdp_tables;

/* Decodes the register set into the addresses the beam is actually using.
 * Tables a mode does not fetch come back with size 0. */
void colecovdp_get_tables(const colecovdp_snapshot *s, colecovdp_tables *out);

/* "Name table", "Sprite attributes", ... for R0-R7. */
const char *colecovdp_register_name(int reg);

/* One line of decoded detail for register reg (0-7): the bit fields by
 * name for R0/R1/R7, the resolved address plus extent (and Graphics II
 * index mask) for the table registers. Returns the length written. */
int colecovdp_describe_register(const colecovdp_snapshot *s, int reg, char *out,
                              int max);

/* Same for the read-only status register: interrupt flag, 5th-sprite and
 * coincidence flags, and the reported sprite number. */
int colecovdp_describe_status(const colecovdp_snapshot *s, char *out, int max);

/* TMS9918A color names, "Transparent" through "White". */
const char *colecovdp_color_name(int index);

/* The whole decode as monospace text -- current mode, the table addresses,
 * then one decoded line per register plus status. Every frontend shows the
 * identical block; 2K is comfortably enough. Returns the length written. */
int colecovdp_format_state(const colecovdp_snapshot *s, char *out, int max);

/* A classic hex dump of VRAM: rows of 16 bytes, "ADDR  xx .. xx  ascii",
 * starting at base (wrapping at 16K). Returns the length written. */
int colecovdp_format_hex(const colecovdp_snapshot *s, uint16_t base, int rows,
                       char *out, int max);

/* Decoders render RGBA8888 into caller-provided buffers; both toolkits just
 * wrap the result in a texture/QImage. Sizes are fixed:
 *   nametable: 256x192   patterns: 256x64 (one 32x8-tile bank)
 *   sprites:   128x64 (32 cells of 16x16 in an 8x4 grid)
 *   palette:   COLECOVDP_PAL_W x COLECOVDP_PAL_H */
void colecovdp_render_nametable(const colecovdp_snapshot *s, uint8_t *rgba);
void colecovdp_render_patterns(const colecovdp_snapshot *s, int bank,
                             uint8_t *rgba);

typedef struct {
    int y, x;        /* SAT position (y is the raw SAT value) */
    int pattern;
    int color;
    int early_clock; /* EC bit */
} colecovdp_sprite;

void colecovdp_render_sprites(const colecovdp_snapshot *s, uint8_t *rgba,
                            colecovdp_sprite info[32]);

/* The palette as 16 separated swatches in an 8x2 grid, each labeled with
 * its hex index and outlined against the gutter so adjacent entries never
 * read as one block. Entry 0 (transparent) is drawn as a checkerboard. */
#define COLECOVDP_PAL_CELL 40
#define COLECOVDP_PAL_COLS 8
#define COLECOVDP_PAL_ROWS 2
#define COLECOVDP_PAL_W (COLECOVDP_PAL_CELL * COLECOVDP_PAL_COLS)
#define COLECOVDP_PAL_H (COLECOVDP_PAL_CELL * COLECOVDP_PAL_ROWS)
void colecovdp_render_palette(const colecovdp_snapshot *s, uint8_t *rgba);

#ifdef __cplusplus
}
#endif

#endif /* COLECODEBUG_H */
