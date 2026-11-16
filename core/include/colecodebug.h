/*
 * colecodebug -- the toolkit-agnostic debugger engine for FujiNet Go
 * ColecoVision Desktop. Wraps adamcore's debug interface (adamcore_debug.h)
 * with pause/step/run-to execution control, breakpoints (execute, read,
 * write, access, port in/out, with condition expressions), an
 * instruction-history ring, a Z80 disassembler, OS7 symbol tables, VDP
 * visualizer decoders, a command line, and a trace of the cartridge's FujiBus
 * traffic. One engine exists per process (colecosession_debugger()); native
 * GTK/Qt/Cocoa/Win32 views sit on top.
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
    COLECODBG_STOP_WATCH,       /* a read/write/access/port breakpoint fired */
    COLECODBG_STOP_FRAME,       /* colecodebug_frame() reached the frame end */
} colecodebug_stop_reason;

/* ---- execution control --------------------------------------------------- */
void colecodebug_pause(colecodebug *d);   /* halts at the next instruction */
void colecodebug_resume(colecodebug *d);
/* True once the machine has actually STOPPED, not merely been asked to.
 * colecodebug_pause() returns as soon as the request is recorded and the
 * emulator thread may still be mid-frame, so a caller that reads registers
 * on the strength of the request gets a moving target. Poll this. */
int  colecodebug_is_paused(colecodebug *d);
/* Whether a pause has been requested, for a UI that wants its button to
 * change the moment it is clicked rather than when the machine catches up. */
int  colecodebug_pause_requested(colecodebug *d);
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
/* One record per executed instruction: the registers are the state BEFORE the
 * instruction at pc ran (a breakpoint or pause stop never records an
 * instruction that did not run). The ring stores raw registers and the four
 * bytes at pc; len is worked out when a record is read. A HALT that is
 * already halted folds into the previous record (repeat counts the extra
 * 4-cycle spins). */
typedef struct {
    uint16_t pc, sp, af, bc, de, hl;
    uint8_t len;
    uint8_t bytes[4];
    uint64_t cycles;
    /* added with the newer debugger; zero in a record from an older build */
    uint16_t ix, iy, af2, bc2, de2, hl2;
    uint8_t reg_i, reg_r, im;
    uint8_t iff;        /* bit 0 IFF1, bit 1 IFF2 */
    uint8_t halted;     /* the CPU was already in HALT when this record began */
    uint32_t repeat;    /* HALT spins folded into this record */
} colecotrace_entry;

#define COLECODEBUG_TRACE_SIZE 65536
/* The ring is allocated on the first enable. Trace and History share it: it
 * records while either asked for it. */
void colecodebug_trace_enable(colecodebug *dbg, int enable);
int  colecodebug_trace_enabled(colecodebug *dbg);
/* Copies up to max entries, newest first. Returns entries written. */
int  colecodebug_trace_read(colecodebug *dbg, colecotrace_entry *out, int max);
void colecodebug_trace_clear(colecodebug *dbg);

/* ---- FujiNet: the cartridge's FujiBus traffic, both ways -------------------
 * Always on from the session's start, debugger open or not: it is cheap, and
 * it is most useful about what happened before anyone thought to look. */
typedef struct {
    uint64_t serial;
    int      to_host;       /* 1: FujiNet -> ColecoVision, 0: the other way */
    int      device, command, length;   /* from the FujiBus header, -1 if short */
    int      nbytes;
    uint8_t  data[64];      /* the decoded packet's first bytes */
    char     text[96];      /* "ColecoVision -> FujiNet 70 F4 READ HOST SLOTS (6 bytes)" */
    uint32_t repeat;        /* identical exchanges folded into this entry ("x3") */
} coleco_fuji_packet;
/* The newest max packets, oldest first. Returns entries written. */
int  colecodebug_fujinet_trace(colecodebug *d, coleco_fuji_packet *out, int max);
void colecodebug_fujinet_trace_clear(colecodebug *d);
/* The cartridge's link, mailbox and image, as text lines. */
int  colecodebug_fujinet_state(colecodebug *d, char *dst, int dstsz);

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

/* ===========================================================================
 * The newer family debugger: stops in words, breakpoints of every kind with
 * condition expressions, the command line, registers, memory map, machine and
 * sound state, search, symbols, trace and history. Same shape as the MSX and
 * Intellivision desktops' engines. Calls are safe from any thread; inspection
 * is meant for the stopped state and answers with whatever is current while
 * the machine runs.
 * ======================================================================== */

/* Bumped whenever the machine stopped/resumed, a command ran, or the
 * breakpoint list or symbols changed, so a window knows when to refresh. */
unsigned colecodebug_generation(colecodebug *dbg);

/* Run to the end of the frame in progress (the machine stops at the first
 * instruction of the next one). */
void colecodebug_frame(colecodebug *dbg);

/* ---- the stop, in words ----------------------------------------------------
 * "paused at $4010", "step at $4010", "frame at $0038", "stopped at $4013" (a
 * one-shot from Over/Out/Run to), "breakpoint: exec $4010",
 * "wrote $7000 = $12 (was $11) at $4A3C", "read $7000 = $12 at $4A3C",
 * "in $FC = $FF at $4A3C", "out $A1 = $12 at $4A3C". While running it says
 * "running" and *address is -1. A watch stop is reported at the instruction
 * AFTER the access (the machine stops at the next instruction boundary).
 * Returns the length. */
int colecodebug_stop_text(colecodebug *dbg, char *dst, int dstsz, int *address);

enum {
    COLECODEBUG_STOP_NONE = 0,
    COLECODEBUG_STOP_PAUSE,
    COLECODEBUG_STOP_STEP,
    COLECODEBUG_STOP_BREAKPOINT,   /* an EXEC entry */
    COLECODEBUG_STOP_WATCH_READ,
    COLECODEBUG_STOP_WATCH_WRITE,
    COLECODEBUG_STOP_WATCH_IN,
    COLECODEBUG_STOP_WATCH_OUT,
    COLECODEBUG_STOP_ONESHOT,      /* step over / out / run to */
    COLECODEBUG_STOP_FRAME
};

typedef struct {
    int      reason;        /* COLECODEBUG_STOP_* */
    int      bp_id;         /* the entry that fired, 0 if none */
    uint16_t addr;          /* the byte or port touched (watch), or the PC */
    int      value;         /* the byte moved, -1 when not a watch */
    int      old_value;     /* the byte a write replaced, -1 when not known */
    uint16_t pc;            /* the PC at the stop */
    int      from_pc;       /* the instruction that made the access, -1 unless a watch */
    uint64_t serial;        /* colecodebug_stop_serial at the stop */
} colecodebug_stop_info;
/* Returns 1 while the machine is stopped, 0 while it runs (out then holds
 * the last stop). */
int colecodebug_last_stop(colecodebug *dbg, colecodebug_stop_info *out);
/* Bumped by every stop (pause, breakpoint, step); frontends recentre their
 * views on the PC when it changes. */
uint64_t colecodebug_stop_serial(colecodebug *dbg);

/* Everything about the current stop read in one critical section, so a
 * poller never sees a stop with the old serial or the old reason: paused,
 * the serial, the stop info and its text agree. paused is 1 only once the
 * machine has really stopped (as colecodebug_is_paused). Returns paused. */
typedef struct {
    int      paused;
    uint64_t serial;
    colecodebug_stop_info stop;
    char     text[160];
    int      address;       /* the PC to show, -1 when running */
} colecodebug_snapshot;
int colecodebug_stop_snapshot(colecodebug *dbg, colecodebug_snapshot *out);

/* ---- the command line ------------------------------------------------------
 * One command per call: help, step [n], over, out, run, stop, frame,
 * runto <addr>, break <addr> [cond], bpr/bpw/bpa <addr>[-<end>] [cond]
 * (read / write / read-or-write), bpin/bpout <port>[-<end>] [cond],
 * delete <id|addr>, enable/disable <id>, clear, hits [clear], breaks,
 * print <expr>, regs, set <reg> <expr>, mem <addr> [len],
 * poke <addr> <val...>, disasm [addr] [n], find [hex] <bytes> |
 * find text <string>, trace [on|off|clear|n], watch [expr|lo-hi],
 * unwatch <n|all>, symbols [text], label <addr> <name>, labels <file>,
 * map, io, cart, sound, video [section], save <dis|mem|vram|regs> <file>.
 * Output text goes into dst; returns its length. */
int colecodebug_command(colecodebug *dbg, const char *command, char *dst, int dstsz);
/* Completions for the end of a line (commands, registers, labels), one per
 * line. Returns the count. */
int colecodebug_completions(colecodebug *dbg, const char *prefix, char *dst, int dstsz);

/* ---- expressions --------------------------------------------------------------
 * The one grammar for conditions, print, set, watches and every address the
 * prompt takes: numbers ($C000 0xC000 C000h 49152 %1010), the Z80 registers
 * (a f b c d e h l af bc de hl ix iy sp pc i r im iff1 iff2 af' bc' de' hl'
 * ixh ixl iyh iyl), the flags (cf nf pf xf hf yf zf sf), labels, [addr] the
 * byte and {addr} the word at addr (as the CPU sees them, no side effects),
 * frame (frames completed), cycles (Z80 clocks), vline (the VDP line in the
 * frame) and, for an access, value, addr and old; C's operators with C's
 * precedence. In a breakpoint condition `value`/`addr`/`old` name the byte the
 * access moved (old = the byte a write replaced; for a read the value itself).
 * 0 and *out, or -1 and a message. Registers and [addr] read the machine as it
 * is now; with the machine stopped at a watch, value/addr/old name that
 * access. */
int colecodebug_eval(colecodebug *dbg, const char *text, long *out, char *err, int errsz);
/* Register names the grammar knows, NULL-terminated, for completion. */
extern const char *const colecodebug_reg_names[];

/* ---- registers, in bulk ----------------------------------------------------------
 * Writes every register of `regs` (the main and shadow sets, ix iy sp pc, i r,
 * im, iff1/iff2 and halted; cycles is the machine's timebase and is ignored).
 * Only while stopped: returns 1, or 0 with a reason in err (running, IM above
 * 2, a flag that is not 0 or 1, no machine). On success err may hold a NOTE
 * for the user: "PC changed while the CPU is halted: it stays halted until
 * HALT is cleared or an interrupt arrives". */
int colecodebug_regs_set(colecodebug *dbg, const adamcore_z80_regs *regs, char *err, int errsz);
/* One register or flag by the names of the expression grammar (a..l, af..hl,
 * ix iy sp pc, shadows, i r im iff1 iff2, sf zf yf hf xf pf nf cf, ixh...).
 * Same rules and notes as colecodebug_regs_set. */
int colecodebug_register_set(colecodebug *dbg, const char *name, long value, char *err, int errsz);

/* ---- breakpoints, all kinds ----------------------------------------------------------
 * Execute breakpoints ride adamcore's PC map; the others ride its bus hook and
 * stop at the next instruction boundary (an opcode fetch never counts as a
 * READ: it is dropped when addr == pc-1). The legacy colecodebug_bp_* calls
 * above are execute breakpoints with no condition and no hit count in this
 * same table. */
typedef enum {
    COLECODEBUG_BP_EXEC = 1, COLECODEBUG_BP_READ = 2, COLECODEBUG_BP_WRITE = 4,
    COLECODEBUG_BP_IN = 8, COLECODEBUG_BP_OUT = 16,
    /* Read and write as one entry ("Access"): READ | WRITE, one id, one hit count. */
    COLECODEBUG_BP_ACCESS = 6
} colecodebug_bp_type;
#define COLECODEBUG_MAX_BREAKPOINTS 128
typedef struct {
    int id;
    int type;                /* colecodebug_bp_type */
    uint16_t start, end;     /* a port range for IN/OUT (0-255) */
    int enabled;
    unsigned hits;
    char condition[128];     /* "" for none: an expression (see colecodebug_eval) */
} colecodebug_breakpoint;
/* Execute breakpoint at addr: added if absent, removed if present. Returns 1
 * when one is now set. */
int  colecodebug_breakpoint_toggle(colecodebug *dbg, uint16_t addr);
/* 1 when an execute breakpoint is at addr (enabled or not). */
int  colecodebug_breakpoint_check(colecodebug *dbg, uint16_t addr);
/* Returns the new id, or -1 with the reason in err (a condition that is not
 * an expression, a bad range, too many). */
int  colecodebug_breakpoint_add(colecodebug *dbg, int type, uint16_t start, uint16_t end,
                                const char *condition, char *err, int errsz);
void colecodebug_breakpoint_remove(colecodebug *dbg, int id);
void colecodebug_breakpoint_enable(colecodebug *dbg, int id, int enabled);
/* Replaces the condition of an entry; "" removes it. 1, or 0 with err. */
int  colecodebug_breakpoint_set_condition(colecodebug *dbg, int id, const char *condition,
                                          char *err, int errsz);
int  colecodebug_breakpoint_list(colecodebug *dbg, colecodebug_breakpoint *out, int max);
void colecodebug_breakpoint_clear(colecodebug *dbg);
void colecodebug_breakpoint_clear_hits(colecodebug *dbg);

/* ---- disassembly rows (the Disassembly tab) -------------------------------------------- */
typedef struct {
    uint16_t address;
    int is_pc;
    int has_breakpoint;      /* an execute breakpoint here, enabled or not */
    int breakpoint_enabled;
    int length;
    char bytes[16];          /* "CD 35 12 " */
    char label[48];
    char disasm[64];
    char comment[64];        /* a jump/call target's label */
} colecodebug_line;
/* Up to max lines starting at addr. Returns the count; *pc_line is the index
 * of the PC's line in out, -1 when not among them. */
int colecodebug_disassemble_lines(colecodebug *dbg, uint16_t addr, colecodebug_line *out,
                                  int max, int *pc_line);
/* The address `rows` disassembly rows before (negative) or after addr -- for
 * scrolling, and for "Follow PC". Backwards is a best guess. */
int colecodebug_row_address(colecodebug *dbg, uint16_t addr, int rows);

/* ---- symbols, for a Symbols tab and the labels --------------------------------------------
 * The built-in OS7 names, loaded files and user labels as one list. */
int  colecodebug_symbols_count(colecodebug *dbg);
/* Entry `index` (0 .. count-1). Returns 1, or 0 past the end. */
int  colecodebug_symbols_get(colecodebug *dbg, int index, uint16_t *addr, char *name, int namesz);
/* Indices of the entries whose name contains `text` (case-insensitive; empty
 * matches all), ordered by address then name. Returns the count written. */
int  colecodebug_symbols_filter(colecodebug *dbg, const char *text, int *out, int max);
/* Bumped when symbols are loaded, labelled or cleared. */
unsigned colecodebug_symbols_generation(colecodebug *dbg);
/* Drops every loaded file and user label; the built-ins stay. */
void colecodebug_clear_symbols(colecodebug *dbg);
/* Loads a .sym file; the message says what happened. Returns labels or -1. */
int  colecodebug_load_symbols(colecodebug *dbg, const char *path, char *msg, int msgsz);
int  colecodebug_label_address(colecodebug *dbg, const char *label);          /* -1 if none */
int  colecodebug_address_label(colecodebug *dbg, uint16_t addr, char *dst, int dstsz);
int  colecodebug_set_label(colecodebug *dbg, uint16_t addr, const char *label);

/* ---- search and the memory map ----------------------------------------------------------------- */
#define COLECODEBUG_SEARCH_NOCASE 1
/* Finds up to max start addresses in from..to of the CPU's 64K as mapped now,
 * where (mem & mask) == (pat & mask) for all count bytes. Returns the count,
 * or -1 while the machine is running or on an error. */
int  colecodebug_search(colecodebug *dbg, const uint8_t *pat, int count, uint8_t mask, int flags,
                        uint16_t from, uint16_t to, uint16_t *out, int max);
/* The next match after `from` (before it if backward), wrapping once.
 * Returns 1 and *found, 0 for none, -1 while running. */
int  colecodebug_search_next(colecodebug *dbg, const uint8_t *pat, int count, uint8_t mask,
                             int flags, uint16_t from, int backward, uint16_t *found);

typedef struct {
    uint16_t low, high;     /* inclusive */
    int      readable, writable;
    char     name[64];      /* "OS7 BIOS ROM", "Console RAM (1K)", "Cartridge (MegaCart, bank 3)" */
} colecodebug_region;
/* Ordered and gap-free from $0000 to $FFFF as the machine is mapped now: OS7
 * (or SGM RAM over it), SGM RAM or open bus at $2000-$5FFF, the 1K console RAM
 * and its mirrors at $6000-$7FFF (or SGM RAM), and the cartridge window
 * $8000-$FFFF with its mapper kind and live bank. Returns how many (up to
 * max; at most 10). */
int  colecodebug_memory_map(colecodebug *dbg, colecodebug_region *out, int max);

/* Save with the path a native file picker supplied. kind: "dis" (the
 * disassembly of $0000-$FFFF as mapped now), "mem" (the 64K the CPU sees),
 * "vram" (the VDP's 16K), "regs" (CPU registers as text). Message into msg;
 * returns 0 or -1. */
int  colecodebug_save(colecodebug *dbg, const char *kind, const char *path, char *msg, int msgsz);

/* ---- trace and history ------------------------------------------------------------------------
 * Trace (newest first, live) and History (what ran before the machine stopped,
 * oldest first) read the ring described at colecotrace_entry. Both are safe
 * while running. */
/* Entries held. */
int  colecodebug_trace_count(colecodebug *dbg);
/* Records pushed since the last clear: a tab polls this to see new lines. */
uint64_t colecodebug_trace_total(colecodebug *dbg);
/* "0006  IN A,($DC)  AF=.... BC=.... DE=.... HL=.... SP=....". Returns the length. */
int  colecodebug_trace_format(const colecotrace_entry *rec, char *dst, int dstsz);
enum { COLECODEBUG_TRACE_ALL = 0, COLECODEBUG_TRACE_BRANCHES, COLECODEBUG_TRACE_RANGE };
/* 1 if the entry passes the filter (BRANCHES: jumps, calls, returns;
 * RANGE: pc in low..high). */
int  colecodebug_trace_match(const colecotrace_entry *rec, int filter, uint16_t low, uint16_t high);
typedef colecotrace_entry colecohist_entry;
void colecodebug_history_enable(colecodebug *dbg, int on);
int  colecodebug_history_enabled(colecodebug *dbg);
void colecodebug_history_clear(colecodebug *dbg);
uint64_t colecodebug_history_count(colecodebug *dbg);
/* Up to max of the newest entries, oldest first; returns how many. */
int  colecodebug_history_copy(colecodebug *dbg, colecohist_entry *out, int max);
/* "50CD  CALL $50D5   AF=.... BC=.... DE=.... HL=.... IX=.... IY=.... SP=....  SZ.H.PNC". */
int  colecodebug_history_format(const colecohist_entry *rec, char *dst, int dstsz);
int  colecodebug_history_match(const colecohist_entry *rec, int filter, uint16_t low, uint16_t high);

/* ---- the machine: Cart & I/O ---------------------------------------------------------------------
 * Read from adamcore's accessors (the mirrors the emulator thread keeps); valid
 * while stopped, a stale value while running is only a stale display. */
typedef struct {
    int      valid;                 /* a machine is running */
    /* the controllers, as the pads sent them */
    uint8_t  joy_mode;              /* the strobe: 1 = joystick half ($C0), 0 = keypad half ($80) */
    struct {
        uint16_t raw;               /* the word given to adamcore_set_joystick */
        uint8_t  joystick_byte;     /* what the console reads in the joystick half (active low) */
        uint8_t  keypad_byte;       /* ... in the keypad half */
        int      up, right, down, left, fire_left, fire_right;   /* 1 = pressed */
        int      keypad;            /* 0-9, 10 '*', 11 '#', -1 none */
        char     keypad_name[4];    /* "", "0".."9", "*", "#" */
    } pad[2];
    /* the VDP's host side */
    uint8_t  vdp_regs[8], vdp_status;
    uint16_t vdp_addr;
    uint8_t  vdp_latch, vdp_latched, vdp_read_ahead, vdp_int_line;
    /* the CPU's side */
    uint8_t  cpu_q, ei_pending, irq_pending, nmi_pending;
    uint64_t nmi_count;
    int      beam_line;             /* scanline 0..261 of the frame in progress */
    int      beam_tstate;           /* T-states into it */
    uint64_t frames;                /* frames completed */
    /* memory control */
    uint8_t  mem_ctrl, net_ctrl;
    int      game_mode;
    /* the Opcode Super Game Module */
    int      sgm;                   /* fitted */
    int      sgm_ram_en, sgm_bios_off;
    uint8_t  ay_addr, ay_regs[16];
} colecodebug_io;
int  colecodebug_io_get(colecodebug *dbg, colecodebug_io *out);
/* The same as text lines for the Cart & I/O tab and the `io` command. Returns the length. */
int  colecodebug_io_state(colecodebug *dbg, char *dst, int dstsz);

typedef struct {
    int      valid;                 /* a machine is running */
    char     machine[32];           /* "ColecoVision" */
    int      sgm;                   /* an Opcode Super Game Module is fitted */
    int      cartridge;             /* the FujiNet cartridge device is installed */
    int      fujinet_link;          /* its socket is connected */
    int      mailbox_live;
    char     kind[24];              /* the mapper serving the window: "flat", "MegaCart", ... */
    unsigned size;                  /* bytes served */
    int      bank;                  /* MegaCart / Activision live bank, -1 otherwise */
    char     hostport[128];
} colecodebug_cart;
void colecodebug_cart_info(colecodebug *dbg, colecodebug_cart *out);
/* One line: "ColecoVision, FujiNet cartridge (MegaCart 131072 bytes, bank 3, link up), SGM". */
int  colecodebug_cart_summary(colecodebug *dbg, char *dst, int dstsz);

/* ---- Sound: the SN76489, and the AY-3-8910 of a Super Game Module -------------------------------------
 * From adamcore's emulator-thread mirrors (the live chips are ~50 ms behind on the
 * audio thread). Valid while stopped. */
typedef struct {
    int      present;               /* always 1 with a machine */
    uint8_t  latched_reg;           /* 0..7: tone0 period, tone0 atten, ... noise atten */
    int      period[3];             /* 10 bits; 0 counts as 1024 */
    double   freq[3];               /* 3579545 / (32 * period) */
    int      atten[4];              /* tones 0-2 then noise: 0 loudest, 15 muted, 2 dB a step */
    int      level[4];              /* 15 - atten: 0 silent .. 15 loudest */
    uint8_t  noise_ctrl;
    int      noise_white;           /* bit 2: white noise, else periodic */
    int      noise_rate;            /* bits 0-1: 0..2 fixed rates, 3 follows tone 2 */
    double   noise_hz;
} colecodebug_sn;

typedef struct {
    int      present;               /* the SGM is fitted */
    uint8_t  addr;                  /* the latched register */
    uint8_t  reg[16];               /* R7 mixer is active-low */
    int      period[3];             /* 12 bits */
    double   freq[3];               /* 111860.8 / period */
    int      tone_on[3], noise_on[3];
    int      level[3];              /* 0-15 */
    int      envelope[3];           /* the voice follows the envelope */
    int      noise_period;          /* 5 bits */
    int      env_period;            /* 16 bits */
    int      env_shape;             /* R13, low 4 bits */
} colecodebug_ay;

typedef struct {
    colecodebug_sn sn;
    colecodebug_ay ay;
} colecodebug_sound;
/* Returns 1 with a machine, 0 without. */
int  colecodebug_sound_get(colecodebug *dbg, colecodebug_sound *out);

/* ---- the Video tab's text for the console `video` command -------------------------------------------
 * func gets a section name (or "") and returns text that stays valid until its
 * next call, NULL when there is no frame yet. Set by whoever owns the Video
 * controller; until then `video` says there is no video state. */
void colecodebug_set_video_text(const char *(*func)(const char *section));

#ifdef __cplusplus
}
#endif

#endif /* COLECODEBUG_H */
