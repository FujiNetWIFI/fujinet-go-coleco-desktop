/*
 * colecodebug engine: execution control (pause / step into/over/out /
 * run-to / frame), the breakpoint table (execute, read, write, access and
 * port breakpoints with condition expressions), the instruction-history ring,
 * symbols, search, the stop in words, and the emulator-thread frame driver the
 * session switches to while the debugger is engaged. The command line is in
 * prompt.c, the machine getters in iostate.c, watches and range parsing in
 * memtools.c, the expression grammar in expr.c.
 *
 * Execution model. All core mutation happens on the emulator thread (the host
 * paced loop calls colecodebug_host_frame while the engine is engaged). UI
 * threads change the engine's tables under the engine mutex and raise a flag;
 * the emulator thread picks it up at the next instruction boundary (the exec
 * hook) or the next frame. While engaged the exec hook is always installed:
 * it is how a pause lands on the very next instruction, how a read/write/port
 * watch stops after the instruction that made the access, and how step-over,
 * step-out and run-to stop, surviving frame boundaries, interrupts and
 * recursion (the SP floor). Execute breakpoints ride adamcore's PC map and are
 * re-checked here (condition, hit count). Whatever resumes from a stop first
 * runs ONE instruction with every check off, so a breakpoint at the current PC
 * does not fire again.
 *
 * Lifetime. The engine keeps its own pointer to the core, valid only under its
 * mutex; the host tells it when a core is created and destroyed
 * (coleco_host_set_core_hooks), so a window polling during a session restart
 * can never touch freed memory, and the breakpoint table, hooks, watches and
 * trace survive the restart.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "../coleco/fujinet_cart.h"
#include "../coleco/host.h"
#include "colecodebug.h"
#include "colecovidtab.h"
#include "debugger_internal.h"
#include "expr.h"
#include "fuji_mailbox.h"
#include "fujitrace.h"
#include "symbols.h"
#include "z80dasm.h"

/* The engine is a singleton, like the machine it steps: adamcore is owned by
 * host.c, and there is exactly one of it per process (the FujiNet cartridge
 * makes that unavoidable anyway). It deliberately does NOT reach into the
 * session. */
static colecodebug *g_debugger;

#define TRACE_RING_LEN COLECODEBUG_TRACE_SIZE /* power of two */

/* What the exec hook has to look at, lock-free: while none is set the hook
 * is a load and a compare per instruction. */
enum {
    WORK_STOPREQ = 1u,  /* a pause was asked for */
    WORK_DIRTY   = 2u,  /* the tables changed: re-arm the core */
    WORK_PENDING = 4u,  /* a watch fired during the last instruction */
    WORK_TRANS   = 8u,  /* step-over / step-out / run-to is armed */
    WORK_TRACE   = 16u  /* the history ring is recording */
};

typedef struct {
    int id;
    int type;
    uint16_t start, end;
    int enabled;
    unsigned hits;
    char cond[128];
} bp_entry;

/* The raw ring record: registers as they were BEFORE the instruction, the
 * four bytes at pc. The instruction is decoded when a record is read. */
typedef struct {
    uint16_t pc, sp, af, bc, de, hl, ix, iy, af2, bc2, de2, hl2;
    uint8_t reg_i, reg_r, im, iff, halted;
    uint8_t bytes[4];
    uint64_t cycles;
    uint32_t repeat;
} ring_rec;

typedef struct {
    int active;
    colecodebug_stop_info info;
} pending_hit;

struct colecodebug {
    colecosession *session;
    pthread_mutex_t mtx;
    pthread_cond_t cv; /* wakes the parked emulator thread; also the waiters */

    adamcore *core;       /* the machine, valid only under mtx; NULL without one */
    adamcore *armed_core; /* the core the hooks were installed on (emulator thread) */

    int paused;        /* target run state -- the REQUEST */
    int at_break;      /* the emulator thread is really stopped, parked */
    int want_record;   /* a pause was requested and no stop has been recorded yet */
    int pending_step;  /* instructions to execute while paused */
    int skip_first;    /* the next run starts with one unchecked instruction */
    int frame_stop;    /* stop at the end of the frame in progress */
    int hook_skip;     /* hook calls to let pass unchecked (emulator thread) */

    atomic_uint work;  /* WORK_* */

    /* The stop the exec hook decided on, for the frame driver. */
    colecodebug_stop_info hook_stop;
    colecodebug_stop_reason hook_reason;

    /* A watch that fired, waiting for the instruction boundary. */
    pending_hit pend;

    /* Transient stop condition, evaluated in the exec hook. */
    struct {
        int active;
        colecodebug_stop_reason reason;
        uint16_t target_pc;
        int has_sp_floor;
        int sp_strict; /* stop only when SP > floor (step-out) */
        uint16_t sp_floor;
    } trans;

    /* The current stop, one critical section with paused. */
    colecodebug_stop_info stop;
    uint64_t serial;
    unsigned gen;

    int notify_flag; /* a stop callback is owed */
    colecodebug_stop_reason notify_reason;
    uint16_t notify_pc;
    void (*stop_cb)(void *ud, colecodebug_stop_reason reason, uint16_t pc);
    void *stop_ud;

    /* The breakpoint table. Survives session restarts; applied to each core. */
    bp_entry bps[COLECODEBUG_MAX_BREAKPOINTS];
    int bp_count;
    int next_id;
    /* Bus-hook interest, rebuilt whenever the table changes (emulator
     * thread, under mtx; read lock-free by the bus hook on the same thread). */
    uint8_t read_map[0x2000], write_map[0x2000], in_map[32], out_map[32];
    int bus_installed;
    uint16_t cur_pc; /* the instruction being executed (emulator thread) */

    /* Instruction history, written by the exec hook. */
    pthread_mutex_t trace_mtx;
    int trace_want, history_want;
    ring_rec *ring;
    uint64_t ring_total;

    sym_tables syms;
    char *user_syms;
    unsigned sym_gen;

    /* The FujiNet trace, fed by the cartridge on the emulator thread. Its own
     * lock, so the tap never waits on the pause machinery. */
    pthread_mutex_t fuji_mtx;
    fujitrace fuji;
};

static const char *(*g_video_text)(const char *section);

/* ---- small helpers -------------------------------------------------------------- */

void colecodebug_lock(colecodebug *dbg)
{
    pthread_mutex_lock(&dbg->mtx);
}

void colecodebug_unlock(colecodebug *dbg)
{
    pthread_mutex_unlock(&dbg->mtx);
}

adamcore *colecodebug_core_locked(colecodebug *dbg)
{
    return dbg ? dbg->core : NULL;
}

static void work_set(colecodebug *dbg, unsigned bits)
{
    atomic_fetch_or(&dbg->work, bits);
}

static void work_clear(colecodebug *dbg, unsigned bits)
{
    atomic_fetch_and(&dbg->work, ~bits);
}

static int is_watch_reason(int reason)
{
    return reason >= COLECODEBUG_STOP_WATCH_READ && reason <= COLECODEBUG_STOP_WATCH_OUT;
}

static void lower_copy(char *dst, int dstsz, const char *src)
{
    int idx = 0;
    while (src[idx] && idx + 1 < dstsz) {
        dst[idx] = (char)tolower((unsigned char)src[idx]);
        idx++;
    }
    dst[idx] = '\0';
}

/* ---- engagement ----------------------------------------------------------- */

static int enabled_bp_count_locked(const colecodebug *dbg)
{
    int idx, count = 0;
    for (idx = 0; idx < dbg->bp_count; idx++)
        if (dbg->bps[idx].enabled) count++;
    return count;
}

/* Engage only while the debugger actually needs the stepped path.
 * adamcore_debug_run is bit-identical to run_frame but costs more, so an
 * open-but-idle debugger window must not slow the machine down. */
static int needs_engaged_locked(const colecodebug *dbg)
{
    return dbg->paused || dbg->pending_step || dbg->trans.active || dbg->frame_stop ||
           dbg->trace_want || dbg->history_want || enabled_bp_count_locked(dbg) > 0 ||
           dbg->notify_flag;
}

/* Turning it ON is any thread's job; turning it off is the emulator thread's
 * (at the end of a frame, where the hooks come off too). */
static void update_engagement_locked(colecodebug *dbg)
{
    if (needs_engaged_locked(dbg))
        coleco_host_set_debug_engaged(1);
}

static void mark_dirty_locked(colecodebug *dbg)
{
    work_set(dbg, WORK_DIRTY);
    dbg->gen++;
    update_engagement_locked(dbg);
}

/* ---- arming the core ------------------------------------------------------ */

static void bit_set(uint8_t *map, unsigned bit)
{
    map[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

static int bit_test(const uint8_t *map, unsigned bit)
{
    return (map[bit >> 3] >> (bit & 7)) & 1;
}

static int exec_hook(void *user, adamcore *core, uint16_t pc);
static void bus_hook(void *user, int kind, uint16_t addr, uint8_t val);

/* Puts the table into the core: the PC map, the bus interest, the hooks.
 * Emulator thread (or before it exists), mtx held. */
static void sync_core_locked(colecodebug *dbg, adamcore *core)
{
    int idx, want_bus = 0;
    unsigned addr;

    memset(dbg->read_map, 0, sizeof(dbg->read_map));
    memset(dbg->write_map, 0, sizeof(dbg->write_map));
    memset(dbg->in_map, 0, sizeof(dbg->in_map));
    memset(dbg->out_map, 0, sizeof(dbg->out_map));
    adamcore_bp_clear_all(core);
    for (idx = 0; idx < dbg->bp_count; idx++) {
        const bp_entry *entry = &dbg->bps[idx];
        if (!entry->enabled) continue;
        if (entry->type & COLECODEBUG_BP_EXEC) {
            for (addr = entry->start; addr <= entry->end; addr++)
                adamcore_bp_set(core, (uint16_t)addr);
        }
        if (entry->type & COLECODEBUG_BP_READ) {
            for (addr = entry->start; addr <= entry->end; addr++) bit_set(dbg->read_map, addr);
            want_bus = 1;
        }
        if (entry->type & COLECODEBUG_BP_WRITE) {
            for (addr = entry->start; addr <= entry->end; addr++) bit_set(dbg->write_map, addr);
            want_bus = 1;
        }
        if (entry->type & COLECODEBUG_BP_IN) {
            for (addr = entry->start; addr <= entry->end && addr <= 0xFF; addr++) bit_set(dbg->in_map, addr);
            want_bus = 1;
        }
        if (entry->type & COLECODEBUG_BP_OUT) {
            for (addr = entry->start; addr <= entry->end && addr <= 0xFF; addr++) bit_set(dbg->out_map, addr);
            want_bus = 1;
        }
    }
    if (dbg->armed_core != core) {
        dbg->armed_core = core;
        dbg->bus_installed = 0;
    }
    if (want_bus && !dbg->bus_installed) {
        adamcore_set_bus_hook(core, bus_hook, dbg);
        dbg->bus_installed = 1;
    } else if (!want_bus && dbg->bus_installed) {
        adamcore_set_bus_hook(core, NULL, NULL);
        dbg->bus_installed = 0;
    }
    adamcore_set_exec_hook(core, exec_hook, dbg);
    work_clear(dbg, WORK_DIRTY);
}

/* Takes everything back out of the core (emulator thread, mtx held). */
static void detach_core_locked(colecodebug *dbg, adamcore *core)
{
    adamcore_bp_clear_all(core);
    adamcore_set_exec_hook(core, NULL, NULL);
    if (dbg->bus_installed)
        adamcore_set_bus_hook(core, NULL, NULL);
    dbg->bus_installed = 0;
    dbg->armed_core = NULL;
    work_set(dbg, WORK_DIRTY);
}

/* ---- registers by name ------------------------------------------------------ */

enum {
    R_A, R_F, R_B, R_C, R_D, R_E, R_H, R_L,
    R_AF, R_BC, R_DE, R_HL, R_IX, R_IY, R_SP, R_PC,
    R_AF2, R_BC2, R_DE2, R_HL2,
    R_IXH, R_IXL, R_IYH, R_IYL,
    R_I, R_R, R_IM, R_IFF1, R_IFF2,
    R_FLAG_S, R_FLAG_Z, R_FLAG_Y, R_FLAG_H, R_FLAG_X, R_FLAG_P, R_FLAG_N, R_FLAG_C
};

static const struct {
    const char *name;
    int code;
} k_regs[] = {
    { "a", R_A }, { "f", R_F }, { "b", R_B }, { "c", R_C }, { "d", R_D }, { "e", R_E },
    { "h", R_H }, { "l", R_L },
    { "af", R_AF }, { "bc", R_BC }, { "de", R_DE }, { "hl", R_HL }, { "ix", R_IX },
    { "iy", R_IY }, { "sp", R_SP }, { "pc", R_PC },
    { "af'", R_AF2 }, { "bc'", R_BC2 }, { "de'", R_DE2 }, { "hl'", R_HL2 },
    { "ixh", R_IXH }, { "ixl", R_IXL }, { "iyh", R_IYH }, { "iyl", R_IYL },
    { "i", R_I }, { "r", R_R }, { "im", R_IM }, { "iff1", R_IFF1 }, { "iff2", R_IFF2 },
    { "sf", R_FLAG_S }, { "zf", R_FLAG_Z }, { "yf", R_FLAG_Y }, { "hf", R_FLAG_H },
    { "xf", R_FLAG_X }, { "pf", R_FLAG_P }, { "nf", R_FLAG_N }, { "cf", R_FLAG_C },
};
#define NUM_REGS ((int)(sizeof(k_regs) / sizeof(k_regs[0])))

const char *const colecodebug_reg_names[] = {
    "a", "f", "b", "c", "d", "e", "h", "l", "af", "bc", "de", "hl", "ix", "iy", "sp", "pc",
    "af'", "bc'", "de'", "hl'", "ixh", "ixl", "iyh", "iyl", "i", "r", "im", "iff1", "iff2",
    "sf", "zf", "yf", "hf", "xf", "pf", "nf", "cf", "frame", "cycles", "vline",
    "value", "addr", "old", "vdpaddr", "vdpsecond", NULL
};

static int reg_code(const char *name)
{
    char low[16];
    int idx;
    lower_copy(low, (int)sizeof(low), name);
    for (idx = 0; idx < NUM_REGS; idx++)
        if (!strcmp(low, k_regs[idx].name)) return k_regs[idx].code;
    return -1;
}

static unsigned flag_mask(int code)
{
    switch (code) {
    case R_FLAG_S: return 0x80;
    case R_FLAG_Z: return 0x40;
    case R_FLAG_Y: return 0x20;
    case R_FLAG_H: return 0x10;
    case R_FLAG_X: return 0x08;
    case R_FLAG_P: return 0x04;
    case R_FLAG_N: return 0x02;
    default:       return 0x01;
    }
}

static long reg_value(const adamcore_z80_regs *regs, int code)
{
    switch (code) {
    case R_A: return regs->a;
    case R_F: return regs->f;
    case R_B: return regs->b;
    case R_C: return regs->c;
    case R_D: return regs->d;
    case R_E: return regs->e;
    case R_H: return regs->h;
    case R_L: return regs->l;
    case R_AF: return (regs->a << 8) | regs->f;
    case R_BC: return (regs->b << 8) | regs->c;
    case R_DE: return (regs->d << 8) | regs->e;
    case R_HL: return (regs->h << 8) | regs->l;
    case R_IX: return regs->ix;
    case R_IY: return regs->iy;
    case R_SP: return regs->sp;
    case R_PC: return regs->pc;
    case R_AF2: return (regs->a2 << 8) | regs->f2;
    case R_BC2: return (regs->b2 << 8) | regs->c2;
    case R_DE2: return (regs->d2 << 8) | regs->e2;
    case R_HL2: return (regs->h2 << 8) | regs->l2;
    case R_IXH: return regs->ix >> 8;
    case R_IXL: return regs->ix & 0xFF;
    case R_IYH: return regs->iy >> 8;
    case R_IYL: return regs->iy & 0xFF;
    case R_I: return regs->i;
    case R_R: return regs->r;
    case R_IM: return regs->im;
    case R_IFF1: return regs->iff1;
    case R_IFF2: return regs->iff2;
    default: return (regs->f & flag_mask(code)) != 0;
    }
}

static void reg_store(adamcore_z80_regs *regs, int code, long value)
{
    const uint8_t byte = (uint8_t)value;
    const uint16_t word = (uint16_t)value;
    switch (code) {
    case R_A: regs->a = byte; break;
    case R_F: regs->f = byte; break;
    case R_B: regs->b = byte; break;
    case R_C: regs->c = byte; break;
    case R_D: regs->d = byte; break;
    case R_E: regs->e = byte; break;
    case R_H: regs->h = byte; break;
    case R_L: regs->l = byte; break;
    case R_AF: regs->a = (uint8_t)(word >> 8); regs->f = (uint8_t)word; break;
    case R_BC: regs->b = (uint8_t)(word >> 8); regs->c = (uint8_t)word; break;
    case R_DE: regs->d = (uint8_t)(word >> 8); regs->e = (uint8_t)word; break;
    case R_HL: regs->h = (uint8_t)(word >> 8); regs->l = (uint8_t)word; break;
    case R_IX: regs->ix = word; break;
    case R_IY: regs->iy = word; break;
    case R_SP: regs->sp = word; break;
    case R_PC: regs->pc = word; break;
    case R_AF2: regs->a2 = (uint8_t)(word >> 8); regs->f2 = (uint8_t)word; break;
    case R_BC2: regs->b2 = (uint8_t)(word >> 8); regs->c2 = (uint8_t)word; break;
    case R_DE2: regs->d2 = (uint8_t)(word >> 8); regs->e2 = (uint8_t)word; break;
    case R_HL2: regs->h2 = (uint8_t)(word >> 8); regs->l2 = (uint8_t)word; break;
    case R_IXH: regs->ix = (uint16_t)((regs->ix & 0x00FF) | (byte << 8)); break;
    case R_IXL: regs->ix = (uint16_t)((regs->ix & 0xFF00) | byte); break;
    case R_IYH: regs->iy = (uint16_t)((regs->iy & 0x00FF) | (byte << 8)); break;
    case R_IYL: regs->iy = (uint16_t)((regs->iy & 0xFF00) | byte); break;
    case R_I: regs->i = byte; break;
    case R_R: regs->r = byte; break;
    case R_IM: regs->im = (uint8_t)value; break;
    case R_IFF1: regs->iff1 = (uint8_t)value; break;
    case R_IFF2: regs->iff2 = (uint8_t)value; break;
    default:
        if (value) regs->f |= (uint8_t)flag_mask(code);
        else       regs->f &= (uint8_t)~flag_mask(code);
        break;
    }
}

/* ---- expressions ----------------------------------------------------------------- */

typedef struct {
    colecodebug *dbg;
    adamcore *core;
    int have_regs;
    adamcore_z80_regs regs;
    const colecodebug_stop_info *access; /* value/addr/old, or NULL */
    int trial;                           /* a syntax trial: access names read 0 */
} eval_ctx;

static uint8_t eval_read_byte(void *user, uint16_t addr)
{
    eval_ctx *ctx = user;
    return ctx->core ? adamcore_peek(ctx->core, addr) : 0xFF;
}

static int eval_name(void *user, const char *name, long *out)
{
    eval_ctx *ctx = user;
    char low[64];
    uint16_t found;
    int code;

    lower_copy(low, (int)sizeof(low), name);
    if (!strcmp(low, "value") || !strcmp(low, "addr") || !strcmp(low, "old")) {
        if (ctx->trial) {
            *out = 0;
            return 1;
        }
        if (!ctx->access || !is_watch_reason(ctx->access->reason)) return 0;
        if (!strcmp(low, "addr")) *out = ctx->access->addr;
        else if (!strcmp(low, "value")) *out = ctx->access->value < 0 ? 0 : ctx->access->value;
        else *out = ctx->access->old_value < 0
                        ? (ctx->access->value < 0 ? 0 : ctx->access->value)
                        : ctx->access->old_value;
        return 1;
    }
    code = reg_code(low);
    if (code >= 0 || !strcmp(low, "cycles")) {
        if (!ctx->have_regs) {
            if (ctx->core) adamcore_get_regs(ctx->core, &ctx->regs);
            else memset(&ctx->regs, 0, sizeof(ctx->regs));
            ctx->have_regs = 1;
        }
        *out = code >= 0 ? reg_value(&ctx->regs, code) : (long)ctx->regs.cycles;
        return 1;
    }
    if (!strcmp(low, "vdpaddr") || !strcmp(low, "vdpsecond")) {
        /* the VDP's host side as it is right now: inside the bus hook that is
         * before an OUT lands, which is what a Break-on-writes condition wants */
        uint8_t vregs[8], vstatus;
        uint16_t vaddr = 0;
        adamcore_vdp_latch latch;
        memset(&latch, 0, sizeof(latch));
        if (ctx->core) {
            adamcore_vdp_state(ctx->core, vregs, &vstatus, &vaddr);
            adamcore_vdp_latch_state(ctx->core, &latch);
        }
        *out = low[3] == 'a' ? vaddr : latch.latched;
        return 1;
    }
    if (!strcmp(low, "frame") || !strcmp(low, "vline")) {
        uint64_t frame = 0;
        int line = 0;
        if (ctx->core) adamcore_debug_beam(ctx->core, &line, &frame);
        *out = low[0] == 'f' ? (long)frame : line;
        return 1;
    }
    if (symtabs_find(&ctx->dbg->syms, name, &found)) {
        *out = found;
        return 1;
    }
    return 0;
}

/* mtx held. */
static int eval_locked(colecodebug *dbg, adamcore *core, const char *text,
                       const colecodebug_stop_info *access, int trial, long *out,
                       char *err, int errsz)
{
    eval_ctx ctx;
    colecoexpr_ctx expr;

    memset(&ctx, 0, sizeof(ctx));
    ctx.dbg = dbg;
    ctx.core = core;
    ctx.access = access;
    ctx.trial = trial;
    expr.name = eval_name;
    expr.read_byte = eval_read_byte;
    expr.user = &ctx;
    return colecoexpr_eval(text, &expr, out, err, errsz);
}

int colecodebug_eval(colecodebug *dbg, const char *text, long *out, char *err, int errsz)
{
    int result;
    long scratch;

    if (err && errsz > 0) err[0] = '\0';
    if (!dbg) {
        if (err && errsz > 0) snprintf(err, (size_t)errsz, "no debugger");
        return -1;
    }
    pthread_mutex_lock(&dbg->mtx);
    result = eval_locked(dbg, dbg->core, text,
                         dbg->paused && dbg->at_break ? &dbg->stop : NULL, 0,
                         out ? out : &scratch, err, errsz);
    pthread_mutex_unlock(&dbg->mtx);
    return result;
}

/* A condition at a hit: true when it holds -- or when it cannot be evaluated,
 * because a debugger that silently ignores a broken condition is worse than
 * one that stops. mtx held. */
static int condition_holds_locked(colecodebug *dbg, adamcore *core, const char *cond,
                                  const colecodebug_stop_info *access)
{
    long value = 1;
    char err[64];
    if (!cond[0]) return 1;
    if (eval_locked(dbg, core, cond, access, 0, &value, err, (int)sizeof(err)) != 0)
        return 1;
    return value != 0;
}

/* ---- lifecycle ------------------------------------------------------------ */

static void core_created(adamcore *core)
{
    colecodebug *dbg = g_debugger;
    if (!dbg || !core) return;
    pthread_mutex_lock(&dbg->mtx);
    dbg->core = core;
    dbg->armed_core = NULL;
    dbg->bus_installed = 0;
    dbg->pend.active = 0;
    dbg->skip_first = 0;
    /* Hooks, watches, breakpoints and trace go into the new machine. This
     * runs before the emulator thread exists, so the core is ours to touch. */
    sync_core_locked(dbg, core);
    if (dbg->trace_want || dbg->history_want) work_set(dbg, WORK_TRACE);
    update_engagement_locked(dbg);
    dbg->gen++;
    pthread_mutex_unlock(&dbg->mtx);
}

/* The host is about to join the emulator thread: wake it if it is parked. */
static void core_stopping(void)
{
    colecodebug *dbg = g_debugger;
    if (!dbg) return;
    pthread_mutex_lock(&dbg->mtx);
    pthread_cond_broadcast(&dbg->cv);
    pthread_mutex_unlock(&dbg->mtx);
}

/* The thread is gone and the core is about to be freed: nothing may reach it
 * again. A stop belongs to the machine that is going away, so the next one
 * starts running (unless nobody asked for a machine yet). */
static void core_destroying(adamcore *core)
{
    colecodebug *dbg = g_debugger;
    (void)core;
    if (!dbg) return;
    pthread_mutex_lock(&dbg->mtx);
    dbg->core = NULL;
    dbg->armed_core = NULL;
    dbg->bus_installed = 0;
    dbg->paused = 0;
    dbg->at_break = 0;
    dbg->want_record = 0;
    dbg->pending_step = 0;
    dbg->skip_first = 0;
    dbg->frame_stop = 0;
    dbg->hook_skip = 0;
    dbg->trans.active = 0;
    dbg->pend.active = 0;
    dbg->notify_flag = 0;
    work_clear(dbg, WORK_STOPREQ | WORK_PENDING | WORK_TRANS);
    work_set(dbg, WORK_DIRTY);
    dbg->gen++;
    pthread_cond_broadcast(&dbg->cv);
    pthread_mutex_unlock(&dbg->mtx);
}

/* Kept for the session's old call site: the host tells the engine now. */
void colecodebug_on_core_created(colecosession *sess)
{
    (void)sess;
    if (g_debugger && coleco_host_core() && !g_debugger->core)
        core_created(coleco_host_core());
}

/* ---- the instruction history ---------------------------------------------------- */

static void trace_record(colecodebug *dbg, adamcore *core, uint16_t pc)
{
    adamcore_z80_regs regs;
    ring_rec rec;
    ring_rec *slot;

    adamcore_get_regs(core, &regs);
    memset(&rec, 0, sizeof(rec));
    rec.pc = pc;
    rec.sp = regs.sp;
    rec.af = (uint16_t)((regs.a << 8) | regs.f);
    rec.bc = (uint16_t)((regs.b << 8) | regs.c);
    rec.de = (uint16_t)((regs.d << 8) | regs.e);
    rec.hl = (uint16_t)((regs.h << 8) | regs.l);
    rec.ix = regs.ix;
    rec.iy = regs.iy;
    rec.af2 = (uint16_t)((regs.a2 << 8) | regs.f2);
    rec.bc2 = (uint16_t)((regs.b2 << 8) | regs.c2);
    rec.de2 = (uint16_t)((regs.d2 << 8) | regs.e2);
    rec.hl2 = (uint16_t)((regs.h2 << 8) | regs.l2);
    rec.reg_i = regs.i;
    rec.reg_r = regs.r;
    rec.im = regs.im;
    rec.iff = (uint8_t)((regs.iff1 ? 1 : 0) | (regs.iff2 ? 2 : 0));
    rec.halted = regs.halted;
    rec.cycles = regs.cycles;
    adamcore_peek_block(core, pc, rec.bytes, 4);

    pthread_mutex_lock(&dbg->trace_mtx);
    if (dbg->ring) {
        /* A CPU already in HALT spins on the same pc every four cycles: one
         * record with a count, not a flood that scrolls everything away. */
        if (rec.halted && dbg->ring_total > 0) {
            ring_rec *last = &dbg->ring[(dbg->ring_total - 1) & (TRACE_RING_LEN - 1)];
            if (last->halted && last->pc == pc) {
                last->repeat++;
                pthread_mutex_unlock(&dbg->trace_mtx);
                return;
            }
        }
        slot = &dbg->ring[dbg->ring_total & (TRACE_RING_LEN - 1)];
        *slot = rec;
        dbg->ring_total++;
    }
    pthread_mutex_unlock(&dbg->trace_mtx);
}

static void ring_entry(const ring_rec *rec, colecotrace_entry *out)
{
    z80d_insn insn;
    memset(out, 0, sizeof(*out));
    out->pc = rec->pc;
    out->sp = rec->sp;
    out->af = rec->af;
    out->bc = rec->bc;
    out->de = rec->de;
    out->hl = rec->hl;
    out->ix = rec->ix;
    out->iy = rec->iy;
    out->af2 = rec->af2;
    out->bc2 = rec->bc2;
    out->de2 = rec->de2;
    out->hl2 = rec->hl2;
    out->reg_i = rec->reg_i;
    out->reg_r = rec->reg_r;
    out->im = rec->im;
    out->iff = rec->iff;
    out->halted = rec->halted;
    out->repeat = rec->repeat;
    out->cycles = rec->cycles;
    memcpy(out->bytes, rec->bytes, 4);
    out->len = (uint8_t)z80_disassemble(&insn, rec->pc, rec->bytes);
}

static void apply_trace_locked(colecodebug *dbg)
{
    const int want = dbg->trace_want || dbg->history_want;
    if (want) {
        pthread_mutex_lock(&dbg->trace_mtx);
        if (!dbg->ring)
            dbg->ring = calloc(TRACE_RING_LEN, sizeof(ring_rec));
        pthread_mutex_unlock(&dbg->trace_mtx);
        work_set(dbg, WORK_TRACE);
    } else {
        work_clear(dbg, WORK_TRACE);
    }
    update_engagement_locked(dbg);
    dbg->gen++;
}

/* ---- the exec hook ---------------------------------------------------------------- */

static int exec_hook(void *user, adamcore *core, uint16_t pc)
{
    colecodebug *dbg = user;
    unsigned work;

    dbg->cur_pc = pc;
    if (dbg->hook_skip > 0) {
        /* The unchecked instruction(s) after a stop: record, never stop. */
        dbg->hook_skip--;
        if (atomic_load(&dbg->work) & WORK_TRACE) trace_record(dbg, core, pc);
        return 0;
    }
    work = atomic_load_explicit(&dbg->work, memory_order_relaxed);
    if (!work) return 0;

    if (work & (WORK_STOPREQ | WORK_PENDING | WORK_TRANS | WORK_DIRTY)) {
        int stop = 0;
        pthread_mutex_lock(&dbg->mtx);
        if (atomic_load(&dbg->work) & WORK_DIRTY)
            sync_core_locked(dbg, core);
        if (dbg->pend.active) {
            dbg->hook_stop = dbg->pend.info;
            dbg->hook_reason = COLECODBG_STOP_WATCH;
            dbg->pend.active = 0;
            work_clear(dbg, WORK_PENDING);
            stop = 1;
        } else if (atomic_load(&dbg->work) & WORK_STOPREQ) {
            memset(&dbg->hook_stop, 0, sizeof(dbg->hook_stop));
            dbg->hook_stop.reason = COLECODEBUG_STOP_PAUSE;
            dbg->hook_stop.value = dbg->hook_stop.old_value = dbg->hook_stop.from_pc = -1;
            dbg->hook_reason = COLECODBG_STOP_PAUSE;
            stop = 1;
        } else if (dbg->trans.active && pc == dbg->trans.target_pc) {
            int reached = 1;
            if (dbg->trans.has_sp_floor) {
                adamcore_z80_regs regs;
                adamcore_get_regs(core, &regs);
                reached = dbg->trans.sp_strict ? regs.sp > dbg->trans.sp_floor
                                               : regs.sp >= dbg->trans.sp_floor;
            }
            if (reached) {
                memset(&dbg->hook_stop, 0, sizeof(dbg->hook_stop));
                dbg->hook_stop.reason = COLECODEBUG_STOP_ONESHOT;
                dbg->hook_stop.value = dbg->hook_stop.old_value = dbg->hook_stop.from_pc = -1;
                dbg->hook_reason = dbg->trans.reason;
                stop = 1;
            }
        }
        pthread_mutex_unlock(&dbg->mtx);
        /* A stop returns BEFORE the instruction runs: it must not be recorded
         * as if it had. */
        if (stop) return 1;
    }
    if (work & WORK_TRACE)
        trace_record(dbg, core, pc);
    return 0;
}

/* ---- the bus hook ----------------------------------------------------------------- */

static void bus_hook(void *user, int kind, uint16_t addr, uint8_t val)
{
    colecodebug *dbg = user;
    int idx, want_bit, reason;
    uint8_t port = (uint8_t)addr;

    switch (kind) {
    case ADAMCORE_BUS_MEM_R:
        if (!bit_test(dbg->read_map, addr)) return;
        want_bit = COLECODEBUG_BP_READ;
        reason = COLECODEBUG_STOP_WATCH_READ;
        break;
    case ADAMCORE_BUS_MEM_W:
        if (!bit_test(dbg->write_map, addr)) return;
        want_bit = COLECODEBUG_BP_WRITE;
        reason = COLECODEBUG_STOP_WATCH_WRITE;
        break;
    case ADAMCORE_BUS_IO_R:
        if (!bit_test(dbg->in_map, port)) return;
        want_bit = COLECODEBUG_BP_IN;
        reason = COLECODEBUG_STOP_WATCH_IN;
        break;
    case ADAMCORE_BUS_IO_W:
        if (!bit_test(dbg->out_map, port)) return;
        want_bit = COLECODEBUG_BP_OUT;
        reason = COLECODEBUG_STOP_WATCH_OUT;
        break;
    default:
        return;
    }

    pthread_mutex_lock(&dbg->mtx);
    {
        adamcore *core = dbg->armed_core;
        colecodebug_stop_info access;
        const unsigned point = (want_bit == COLECODEBUG_BP_IN || want_bit == COLECODEBUG_BP_OUT)
                                   ? port : addr;
        int fetch = 0;

        if (!core) {
            pthread_mutex_unlock(&dbg->mtx);
            return;
        }
        if (kind == ADAMCORE_BUS_MEM_R) {
            /* An opcode or operand fetch goes through the same hook, right
             * after the PC moved past the byte: addr == pc-1. Not a data read. */
            adamcore_z80_regs regs;
            adamcore_get_regs(core, &regs);
            fetch = addr == (uint16_t)(regs.pc - 1);
        }
        if (!fetch) {
            memset(&access, 0, sizeof(access));
            access.reason = reason;
            access.addr = (uint16_t)point;
            access.value = val;
            access.old_value = kind == ADAMCORE_BUS_MEM_W ? adamcore_peek(core, addr)
                               : (kind == ADAMCORE_BUS_IO_W ? -1 : val);
            access.from_pc = dbg->cur_pc;
            for (idx = 0; idx < dbg->bp_count; idx++) {
                bp_entry *entry = &dbg->bps[idx];
                if (!entry->enabled || !(entry->type & want_bit)) continue;
                if (point < entry->start || point > entry->end) continue;
                if (!condition_holds_locked(dbg, core, entry->cond, &access)) continue;
                entry->hits++;
                if (!dbg->pend.active) {
                    dbg->pend.active = 1;
                    dbg->pend.info = access;
                    dbg->pend.info.bp_id = entry->id;
                    work_set(dbg, WORK_PENDING);
                }
            }
        }
    }
    pthread_mutex_unlock(&dbg->mtx);
}

/* ---- recording a stop ------------------------------------------------------------- */

/* Makes the stop the machine is in: one critical section updates paused, the
 * stop info and its serial together. mtx held. */
static void enter_stop_locked(colecodebug *dbg, adamcore *core, const colecodebug_stop_info *info,
                              colecodebug_stop_reason cb_reason)
{
    adamcore_z80_regs regs;

    adamcore_get_regs(core, &regs);
    if (info) {
        dbg->stop = *info;
    } else {
        memset(&dbg->stop, 0, sizeof(dbg->stop));
        dbg->stop.value = dbg->stop.old_value = dbg->stop.from_pc = -1;
    }
    if (!is_watch_reason(dbg->stop.reason))
        dbg->stop.addr = regs.pc;
    dbg->stop.pc = regs.pc;
    dbg->stop.serial = ++dbg->serial;

    dbg->paused = 1;
    dbg->want_record = 0;
    dbg->frame_stop = 0;
    dbg->trans.active = 0;
    dbg->pend.active = 0;
    work_clear(dbg, WORK_STOPREQ | WORK_PENDING | WORK_TRANS);

    dbg->notify_flag = 1;
    dbg->notify_reason = cb_reason;
    dbg->notify_pc = regs.pc;
    dbg->gen++;
    update_engagement_locked(dbg);
}

static void notify_stop_unlocked(colecodebug *dbg, colecodebug_stop_reason reason, uint16_t pc)
{
    void (*callback)(void *, colecodebug_stop_reason, uint16_t) = dbg->stop_cb;
    void *user = dbg->stop_ud;
    if (callback)
        callback(user, reason, pc);
}

/* One instruction, exactly: debug_run closes pending scanlines before it
 * executes anything, and reports the frame end instead when that was the
 * last line. Try again until an instruction ran. */
static void run_one_instruction(adamcore *core, int *changed)
{
    int attempt;
    for (attempt = 0; attempt < 4; attempt++) {
        int chg = 0;
        adamcore_run_status status = adamcore_debug_run(core, 1, 0, &chg);
        if (chg) *changed = 1;
        if (status != ADAMCORE_RUN_FRAME_DONE) return;
    }
}

/* ---- emulator-thread frame driver ---------------------------------------- */

/* The exec breakpoints at pc whose conditions hold; the first one's id, or 0.
 * Counts a hit on each. mtx held. */
static int exec_breakpoint_hit_locked(colecodebug *dbg, adamcore *core, uint16_t pc)
{
    int idx, first = 0;
    for (idx = 0; idx < dbg->bp_count; idx++) {
        bp_entry *entry = &dbg->bps[idx];
        if (!entry->enabled || !(entry->type & COLECODEBUG_BP_EXEC)) continue;
        if (pc < entry->start || pc > entry->end) continue;
        if (!condition_holds_locked(dbg, core, entry->cond, NULL)) continue;
        entry->hits++;
        if (!first) first = entry->id;
    }
    return first;
}

static void disengage_locked(colecodebug *dbg, adamcore *core)
{
    if (dbg->paused || needs_engaged_locked(dbg)) return;
    detach_core_locked(dbg, core);
    coleco_host_set_debug_engaged(0);
}

/* One frame the stepped way. Installed into host.c as the frame function and
 * called from the paced loop while the debugger is engaged. */
int colecodebug_host_frame(void)
{
    colecodebug *dbg = g_debugger;
    adamcore *core = coleco_host_core();
    int changed = 0;

    if (!dbg || !core)
        return core ? adamcore_run_frame(core) : 0;

    pthread_mutex_lock(&dbg->mtx);
    if (dbg->core != core) {
        /* An engine made after the machine started. */
        dbg->core = core;
        dbg->armed_core = NULL;
    }
    if (dbg->armed_core != core || (atomic_load(&dbg->work) & WORK_DIRTY))
        sync_core_locked(dbg, core);
    if (dbg->trace_want || dbg->history_want) work_set(dbg, WORK_TRACE);

    for (;;) {
        if (coleco_host_stopping())
            break;

        if (dbg->paused) {
            if (dbg->want_record) {
                enter_stop_locked(dbg, core,
                                  &(colecodebug_stop_info){ .reason = COLECODEBUG_STOP_PAUSE,
                                                            .value = -1, .old_value = -1,
                                                            .from_pc = -1 },
                                  COLECODBG_STOP_PAUSE);
                continue;
            }
            if (dbg->notify_flag) {
                const colecodebug_stop_reason reason = dbg->notify_reason;
                const uint16_t pc = dbg->notify_pc;
                dbg->notify_flag = 0;
                pthread_mutex_unlock(&dbg->mtx);
                notify_stop_unlocked(dbg, reason, pc);
                pthread_mutex_lock(&dbg->mtx);
                continue; /* re-evaluate: the callback may have resumed */
            }
            if (dbg->pending_step > 0) {
                int left = dbg->pending_step;
                colecodebug_stop_info info;
                dbg->pending_step = 0;
                dbg->hook_skip = 0;
                while (left-- > 0) {
                    dbg->hook_skip = 1;
                    pthread_mutex_unlock(&dbg->mtx);
                    run_one_instruction(core, &changed);
                    pthread_mutex_lock(&dbg->mtx);
                    dbg->hook_skip = 0;
                    if (dbg->pend.active) break; /* a watch fired during the step */
                }
                if (dbg->pend.active) {
                    info = dbg->pend.info;
                    enter_stop_locked(dbg, core, &info, COLECODBG_STOP_WATCH);
                } else {
                    memset(&info, 0, sizeof(info));
                    info.reason = COLECODEBUG_STOP_STEP;
                    info.value = info.old_value = info.from_pc = -1;
                    enter_stop_locked(dbg, core, &info, COLECODBG_STOP_STEP);
                }
                continue;
            }
            coleco_host_set_audio_mute(1);
            /* The emulator thread is now genuinely stopped, as opposed to
             * merely having been asked to stop. is_paused reports THIS, not
             * the request: a caller that pauses and immediately reads
             * registers must not get them from a machine still finishing its
             * frame. */
            dbg->at_break = 1;
            pthread_cond_broadcast(&dbg->cv);
            pthread_cond_wait(&dbg->cv, &dbg->mtx);
            dbg->at_break = 0;
            continue;
        }

        coleco_host_set_audio_mute(0);

        /* Running with the debugger engaged. */
        if (atomic_load(&dbg->work) & WORK_DIRTY)
            sync_core_locked(dbg, core);

        if (dbg->skip_first) {
            /* Step off the instruction the machine stopped at, with every
             * check off: a breakpoint there must not fire again, and a run-to
             * the current PC must go all the way round. */
            dbg->hook_skip = 1;
            pthread_mutex_unlock(&dbg->mtx);
            run_one_instruction(core, &changed);
            pthread_mutex_lock(&dbg->mtx);
            dbg->hook_skip = 0;
            dbg->skip_first = 0;
            continue;
        }

        {
            adamcore_run_status status;
            int chg = 0;
            pthread_mutex_unlock(&dbg->mtx);
            status = adamcore_debug_run(core, 0, 1, &chg);
            pthread_mutex_lock(&dbg->mtx);
            changed |= chg;

            if (status == ADAMCORE_RUN_FRAME_DONE) {
                if (dbg->frame_stop && !dbg->paused) {
                    colecodebug_stop_info info;
                    memset(&info, 0, sizeof(info));
                    info.reason = COLECODEBUG_STOP_FRAME;
                    info.value = info.old_value = info.from_pc = -1;
                    enter_stop_locked(dbg, core, &info, COLECODBG_STOP_FRAME);
                    continue;
                }
                break; /* frame boundary: hand back to the paced loop */
            }

            if (status == ADAMCORE_RUN_BREAKPOINT) {
                adamcore_z80_regs regs;
                int hit_id;
                adamcore_get_regs(core, &regs);
                hit_id = exec_breakpoint_hit_locked(dbg, core, regs.pc);
                if (dbg->pend.active) {
                    /* a watch fired just before this boundary */
                    colecodebug_stop_info info = dbg->pend.info;
                    enter_stop_locked(dbg, core, &info, COLECODBG_STOP_WATCH);
                } else if (hit_id) {
                    colecodebug_stop_info info;
                    memset(&info, 0, sizeof(info));
                    info.reason = COLECODEBUG_STOP_BREAKPOINT;
                    info.bp_id = hit_id;
                    info.value = info.old_value = info.from_pc = -1;
                    enter_stop_locked(dbg, core, &info, COLECODBG_STOP_BREAKPOINT);
                } else {
                    dbg->skip_first = 1; /* a condition that is false: go on */
                }
                continue;
            }

            if (status == ADAMCORE_RUN_HOOK_STOP) {
                colecodebug_stop_info info = dbg->hook_stop;
                enter_stop_locked(dbg, core, &info, dbg->hook_reason);
                continue;
            }
        }
    }
    coleco_host_set_audio_mute(0);
    disengage_locked(dbg, core);
    pthread_mutex_unlock(&dbg->mtx);
    return changed;
}

void colecodebug_resume_for_stop(colecodebug *dbg)
{
    if (!dbg) return;
    pthread_mutex_lock(&dbg->mtx);
    pthread_cond_broadcast(&dbg->cv);
    pthread_mutex_unlock(&dbg->mtx);
}

/* ---- creation --------------------------------------------------------------- */

/* Guards creation only. A static mutex rather than one borrowed from the
 * session, because the engine has no session: it steps the core host.c
 * owns. */
static pthread_mutex_t g_create_mtx = PTHREAD_MUTEX_INITIALIZER;

/* The cartridge's trace tap (emulator thread). */
static void fuji_tap(void *user, int dir, const uint8_t *buf, size_t len)
{
    colecodebug *dbg = user;
    pthread_mutex_lock(&dbg->fuji_mtx);
    fujitrace_feed(&dbg->fuji, dir, buf, len);
    pthread_mutex_unlock(&dbg->fuji_mtx);
}

colecodebug *colecosession_debugger(colecosession *sess)
{
    (void)sess;
    pthread_mutex_lock(&g_create_mtx);
    if (!g_debugger) {
        colecodebug *dbg = calloc(1, sizeof(*dbg));
        if (dbg) {
            static const coleco_host_core_hooks hooks = { core_created, core_stopping,
                                                          core_destroying };
            pthread_mutex_init(&dbg->mtx, NULL);
            pthread_cond_init(&dbg->cv, NULL);
            pthread_mutex_init(&dbg->trace_mtx, NULL);
            dbg->next_id = 1;
            /* OS7 only: the ColecoVision has no EOS. */
            symtabs_load_text(&dbg->syms, colecodebug_builtin_os7_sym, "os7");
            /* The stepped frame path the paced loop calls while engaged. */
            coleco_host_set_frame_fn(colecodebug_host_frame);
            coleco_host_set_core_hooks(&hooks);
            /* The FujiNet trace is always on: colecosession_start creates
             * the engine before the machine for this. */
            pthread_mutex_init(&dbg->fuji_mtx, NULL);
            coleco_cart_set_trace(fuji_tap, dbg);
            g_debugger = dbg;
            /* the console's `video` command reads the Video tab's text */
            colecovid_tab_register_console(dbg);
            /* A machine that is already running: the emulator thread arms
             * it at its next frame. */
            if (coleco_host_core()) {
                dbg->core = coleco_host_core();
                work_set(dbg, WORK_DIRTY);
            }
        }
    }
    pthread_mutex_unlock(&g_create_mtx);
    return g_debugger;
}

/* ---- execution control ---------------------------------------------------- */

void colecodebug_pause(colecodebug *dbg)
{
    pthread_mutex_lock(&dbg->mtx);
    if (!dbg->paused) {
        dbg->paused = 1;
        dbg->want_record = 1;
        dbg->trans.active = 0;
        dbg->frame_stop = 0;
        work_clear(dbg, WORK_TRANS);
        /* The exec hook stops the machine at the NEXT INSTRUCTION, not at the
         * end of the frame. */
        work_set(dbg, WORK_STOPREQ);
        dbg->gen++;
        update_engagement_locked(dbg);
    }
    pthread_cond_broadcast(&dbg->cv);
    pthread_mutex_unlock(&dbg->mtx);
}

/* Lets the machine go from a stop. mtx held. */
static void release_locked(colecodebug *dbg)
{
    if (dbg->paused) {
        /* The first instruction after a stop runs unchecked, so a breakpoint
         * at the PC does not fire again. */
        dbg->skip_first = 1;
    }
    dbg->paused = 0;
    dbg->want_record = 0;
    work_clear(dbg, WORK_STOPREQ);
    dbg->gen++;
    update_engagement_locked(dbg);
    pthread_cond_broadcast(&dbg->cv);
}

void colecodebug_resume(colecodebug *dbg)
{
    pthread_mutex_lock(&dbg->mtx);
    dbg->trans.active = 0;
    dbg->frame_stop = 0;
    work_clear(dbg, WORK_TRANS);
    release_locked(dbg);
    pthread_mutex_unlock(&dbg->mtx);
}

/* Reports whether the machine has actually STOPPED, not whether a pause has
 * been requested. The distinction matters: colecodebug_pause() returns as
 * soon as the request is recorded, and the emulator thread may still be
 * mid-frame. A frontend that read registers on the strength of the request
 * would get a moving target.
 *
 * While the machine is stopped but not yet parked (the window between request
 * and hook), this returns 0 -- "not stopped yet" -- which is the honest answer
 * and the one that makes a poll-until-paused loop correct. */
int colecodebug_is_paused(colecodebug *dbg)
{
    int paused;
    pthread_mutex_lock(&dbg->mtx);
    paused = dbg->paused && dbg->at_break;
    pthread_mutex_unlock(&dbg->mtx);
    return paused;
}

/* Has a pause been REQUESTED? For a UI that wants its button to change the
 * moment it is clicked rather than when the machine catches up. */
int colecodebug_pause_requested(colecodebug *dbg)
{
    int paused;
    pthread_mutex_lock(&dbg->mtx);
    paused = dbg->paused;
    pthread_mutex_unlock(&dbg->mtx);
    return paused;
}

void colecodebug_step_into(colecodebug *dbg)
{
    pthread_mutex_lock(&dbg->mtx);
    if (dbg->paused) {
        dbg->pending_step = 1;
        update_engagement_locked(dbg);
        pthread_cond_broadcast(&dbg->cv);
    }
    pthread_mutex_unlock(&dbg->mtx);
}

/* Arms a one-shot stop and lets the machine go. mtx held. */
static void arm_oneshot_locked(colecodebug *dbg, colecodebug_stop_reason reason, uint16_t target,
                               int use_floor, int strict, uint16_t floor_sp)
{
    dbg->trans.active = 1;
    dbg->trans.reason = reason;
    dbg->trans.target_pc = target;
    dbg->trans.has_sp_floor = use_floor;
    dbg->trans.sp_strict = strict;
    dbg->trans.sp_floor = floor_sp;
    work_set(dbg, WORK_TRANS);
    release_locked(dbg);
}

void colecodebug_step_over(colecodebug *dbg)
{
    adamcore_z80_regs regs;
    uint8_t code[4];
    z80d_insn insn;

    pthread_mutex_lock(&dbg->mtx);
    if (!dbg->paused || !dbg->core) {
        pthread_mutex_unlock(&dbg->mtx);
        return;
    }
    adamcore_get_regs(dbg->core, &regs);
    adamcore_peek_block(dbg->core, regs.pc, code, 4);
    z80_disassemble(&insn, regs.pc, code);

    if (insn.flags & (COLECODASM_CALL | COLECODASM_HALT | COLECODASM_BLOCK)) {
        /* Run until control returns to the next instruction with the stack
         * back at (or above) this level -- survives recursion and any NMI
         * that fires while the callee runs. */
        arm_oneshot_locked(dbg, COLECODBG_STOP_STEP, (uint16_t)(regs.pc + insn.len), 1, 0, regs.sp);
    } else {
        dbg->pending_step = 1;
        update_engagement_locked(dbg);
        pthread_cond_broadcast(&dbg->cv);
    }
    pthread_mutex_unlock(&dbg->mtx);
}

void colecodebug_step_out(colecodebug *dbg)
{
    adamcore_z80_regs regs;
    uint8_t low_byte, high_byte;

    pthread_mutex_lock(&dbg->mtx);
    if (!dbg->paused || !dbg->core) {
        pthread_mutex_unlock(&dbg->mtx);
        return;
    }
    adamcore_get_regs(dbg->core, &regs);
    low_byte = adamcore_peek(dbg->core, regs.sp);
    high_byte = adamcore_peek(dbg->core, (uint16_t)(regs.sp + 1));
    arm_oneshot_locked(dbg, COLECODBG_STOP_STEP, (uint16_t)(low_byte | (high_byte << 8)), 1, 1, regs.sp);
    pthread_mutex_unlock(&dbg->mtx);
}

void colecodebug_run_to(colecodebug *dbg, uint16_t addr)
{
    pthread_mutex_lock(&dbg->mtx);
    arm_oneshot_locked(dbg, COLECODBG_STOP_RUNTO, addr, 0, 0, 0);
    pthread_mutex_unlock(&dbg->mtx);
}

void colecodebug_frame(colecodebug *dbg)
{
    pthread_mutex_lock(&dbg->mtx);
    dbg->trans.active = 0;
    work_clear(dbg, WORK_TRANS);
    dbg->frame_stop = 1;
    release_locked(dbg);
    pthread_mutex_unlock(&dbg->mtx);
}

void colecodebug_set_stop_callback(colecodebug *dbg,
                                   void (*callback)(void *, colecodebug_stop_reason, uint16_t),
                                   void *user)
{
    pthread_mutex_lock(&dbg->mtx);
    dbg->stop_cb = callback;
    dbg->stop_ud = user;
    pthread_mutex_unlock(&dbg->mtx);
}

unsigned colecodebug_generation(colecodebug *dbg)
{
    unsigned gen;
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    gen = dbg->gen;
    pthread_mutex_unlock(&dbg->mtx);
    return gen;
}

void colecodebug_touch(colecodebug *dbg)
{
    if (!dbg) return;
    pthread_mutex_lock(&dbg->mtx);
    dbg->gen++;
    pthread_mutex_unlock(&dbg->mtx);
}

/* ---- the stop, in words -------------------------------------------------------- */

static int format_stop(const colecodebug_stop_info *info, char *dst, int dstsz)
{
    char value[24] = "", was[24] = "";
    const unsigned pc = info->pc;

    if (info->value >= 0) snprintf(value, sizeof(value), " = $%02X", info->value);
    if (info->old_value >= 0 && info->reason == COLECODEBUG_STOP_WATCH_WRITE)
        snprintf(was, sizeof(was), " (was $%02X)", info->old_value);
    switch (info->reason) {
    case COLECODEBUG_STOP_PAUSE:       return snprintf(dst, (size_t)dstsz, "paused at $%04X", pc);
    case COLECODEBUG_STOP_STEP:        return snprintf(dst, (size_t)dstsz, "step at $%04X", pc);
    case COLECODEBUG_STOP_FRAME:       return snprintf(dst, (size_t)dstsz, "frame at $%04X", pc);
    case COLECODEBUG_STOP_BREAKPOINT:  return snprintf(dst, (size_t)dstsz, "breakpoint: exec $%04X", pc);
    case COLECODEBUG_STOP_WATCH_WRITE:
        return snprintf(dst, (size_t)dstsz, "wrote $%04X%s%s at $%04X", info->addr, value, was, pc);
    case COLECODEBUG_STOP_WATCH_READ:
        return snprintf(dst, (size_t)dstsz, "read $%04X%s at $%04X", info->addr, value, pc);
    case COLECODEBUG_STOP_WATCH_IN:
        return snprintf(dst, (size_t)dstsz, "in $%02X%s at $%04X", info->addr & 0xFF, value, pc);
    case COLECODEBUG_STOP_WATCH_OUT:
        return snprintf(dst, (size_t)dstsz, "out $%02X%s at $%04X", info->addr & 0xFF, value, pc);
    default:                           return snprintf(dst, (size_t)dstsz, "stopped at $%04X", pc);
    }
}

int colecodebug_stop_text(colecodebug *dbg, char *dst, int dstsz, int *address)
{
    int len;
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (address) *address = -1;
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    if (dbg->paused && dbg->at_break) {
        len = format_stop(&dbg->stop, dst, dstsz);
        if (address) *address = dbg->stop.pc;
    } else {
        len = snprintf(dst, (size_t)dstsz, "running");
    }
    pthread_mutex_unlock(&dbg->mtx);
    return len < dstsz ? len : dstsz - 1;
}

int colecodebug_last_stop(colecodebug *dbg, colecodebug_stop_info *out)
{
    int stopped;
    if (!dbg || !out) return 0;
    pthread_mutex_lock(&dbg->mtx);
    *out = dbg->stop;
    stopped = dbg->paused && dbg->at_break;
    pthread_mutex_unlock(&dbg->mtx);
    return stopped;
}

uint64_t colecodebug_stop_serial(colecodebug *dbg)
{
    uint64_t serial;
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    serial = dbg->serial;
    pthread_mutex_unlock(&dbg->mtx);
    return serial;
}

int colecodebug_stop_snapshot(colecodebug *dbg, colecodebug_snapshot *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    out->address = -1;
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    out->paused = dbg->paused && dbg->at_break;
    out->serial = dbg->serial;
    out->stop = dbg->stop;
    if (out->paused) {
        format_stop(&dbg->stop, out->text, (int)sizeof(out->text));
        out->address = dbg->stop.pc;
    } else {
        snprintf(out->text, sizeof(out->text), "running");
    }
    pthread_mutex_unlock(&dbg->mtx);
    return out->paused;
}

/* Waits (bounded) for the machine to stop at a stop newer than `serial`.
 * Returns 1 once stopped. For the command line. */
int colecodebug_wait_stop(colecodebug *dbg, uint64_t serial, int timeout_ms)
{
    struct timespec deadline;
    int stopped;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec++;
    }
    pthread_mutex_lock(&dbg->mtx);
    while (!(dbg->paused && dbg->at_break && dbg->serial != serial)) {
        if (pthread_cond_timedwait(&dbg->cv, &dbg->mtx, &deadline) != 0)
            break;
    }
    stopped = dbg->paused && dbg->at_break && dbg->serial != serial;
    pthread_mutex_unlock(&dbg->mtx);
    return stopped;
}

/* ---- the breakpoint table ---------------------------------------------------------- */

static bp_entry *bp_by_id_locked(colecodebug *dbg, int id)
{
    int idx;
    for (idx = 0; idx < dbg->bp_count; idx++)
        if (dbg->bps[idx].id == id) return &dbg->bps[idx];
    return NULL;
}

static bp_entry *bp_exec_at_locked(colecodebug *dbg, uint16_t addr)
{
    int idx;
    for (idx = 0; idx < dbg->bp_count; idx++)
        if (dbg->bps[idx].type == COLECODEBUG_BP_EXEC && dbg->bps[idx].start <= addr &&
            dbg->bps[idx].end >= addr)
            return &dbg->bps[idx];
    return NULL;
}

static void bp_remove_at_locked(colecodebug *dbg, int idx)
{
    memmove(&dbg->bps[idx], &dbg->bps[idx + 1],
            (size_t)(dbg->bp_count - idx - 1) * sizeof(dbg->bps[0]));
    dbg->bp_count--;
}

int colecodebug_breakpoint_check(colecodebug *dbg, uint16_t addr)
{
    int found;
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    found = bp_exec_at_locked(dbg, addr) != NULL;
    pthread_mutex_unlock(&dbg->mtx);
    return found;
}

int colecodebug_breakpoint_add(colecodebug *dbg, int type, uint16_t start, uint16_t end,
                               const char *condition, char *err, int errsz)
{
    bp_entry *entry;
    int id;

    if (err && errsz > 0) err[0] = '\0';
    if (!dbg) return -1;
    if (type != COLECODEBUG_BP_EXEC && type != COLECODEBUG_BP_READ &&
        type != COLECODEBUG_BP_WRITE && type != COLECODEBUG_BP_IN &&
        type != COLECODEBUG_BP_OUT && type != COLECODEBUG_BP_ACCESS) {
        if (err && errsz > 0) snprintf(err, (size_t)errsz, "unknown breakpoint type");
        return -1;
    }
    if (end < start) end = start;
    if ((type == COLECODEBUG_BP_IN || type == COLECODEBUG_BP_OUT) && end > 0xFF) {
        if (err && errsz > 0) snprintf(err, (size_t)errsz, "ports are $00-$FF");
        return -1;
    }
    pthread_mutex_lock(&dbg->mtx);
    if (condition && *condition) {
        /* Tried once now, so a typo is refused instead of sitting silent
         * (the access names read 0 in the trial). */
        long scratch;
        if (eval_locked(dbg, dbg->core, condition, NULL, 1, &scratch, err, errsz) != 0) {
            pthread_mutex_unlock(&dbg->mtx);
            return -1;
        }
    }
    if (dbg->bp_count >= COLECODEBUG_MAX_BREAKPOINTS) {
        pthread_mutex_unlock(&dbg->mtx);
        if (err && errsz > 0) snprintf(err, (size_t)errsz, "too many breakpoints");
        return -1;
    }
    entry = &dbg->bps[dbg->bp_count];
    memset(entry, 0, sizeof(*entry));
    entry->id = dbg->next_id++;
    entry->type = type;
    entry->start = start;
    entry->end = type == COLECODEBUG_BP_EXEC ? start : end;
    entry->enabled = 1;
    snprintf(entry->cond, sizeof(entry->cond), "%s", condition ? condition : "");
    id = entry->id;
    dbg->bp_count++;
    mark_dirty_locked(dbg);
    pthread_mutex_unlock(&dbg->mtx);
    return id;
}

int colecodebug_breakpoint_toggle(colecodebug *dbg, uint16_t addr)
{
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    {
        bp_entry *entry = bp_exec_at_locked(dbg, addr);
        if (entry) {
            bp_remove_at_locked(dbg, (int)(entry - dbg->bps));
            mark_dirty_locked(dbg);
            pthread_mutex_unlock(&dbg->mtx);
            return 0;
        }
    }
    pthread_mutex_unlock(&dbg->mtx);
    return colecodebug_breakpoint_add(dbg, COLECODEBUG_BP_EXEC, addr, addr, "", NULL, 0) >= 0;
}

void colecodebug_breakpoint_remove(colecodebug *dbg, int id)
{
    int idx;
    if (!dbg) return;
    pthread_mutex_lock(&dbg->mtx);
    for (idx = 0; idx < dbg->bp_count; idx++)
        if (dbg->bps[idx].id == id) {
            bp_remove_at_locked(dbg, idx);
            mark_dirty_locked(dbg);
            break;
        }
    pthread_mutex_unlock(&dbg->mtx);
}

void colecodebug_breakpoint_enable(colecodebug *dbg, int id, int enabled)
{
    bp_entry *entry;
    if (!dbg) return;
    pthread_mutex_lock(&dbg->mtx);
    entry = bp_by_id_locked(dbg, id);
    if (entry && entry->enabled != (enabled != 0)) {
        entry->enabled = enabled != 0;
        mark_dirty_locked(dbg);
    }
    pthread_mutex_unlock(&dbg->mtx);
}

int colecodebug_breakpoint_set_condition(colecodebug *dbg, int id, const char *condition,
                                         char *err, int errsz)
{
    bp_entry *entry;
    int ok = 0;

    if (err && errsz > 0) err[0] = '\0';
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    entry = bp_by_id_locked(dbg, id);
    if (!entry) {
        if (err && errsz > 0) snprintf(err, (size_t)errsz, "no such breakpoint");
    } else {
        long scratch;
        if (condition && *condition &&
            eval_locked(dbg, dbg->core, condition, NULL, 1, &scratch, err, errsz) != 0) {
            /* err holds the reason */
        } else {
            snprintf(entry->cond, sizeof(entry->cond), "%s", condition ? condition : "");
            dbg->gen++;
            ok = 1;
        }
    }
    pthread_mutex_unlock(&dbg->mtx);
    return ok;
}

int colecodebug_breakpoint_list(colecodebug *dbg, colecodebug_breakpoint *out, int max)
{
    int count, idx;
    if (!dbg || !out) return 0;
    pthread_mutex_lock(&dbg->mtx);
    count = dbg->bp_count < max ? dbg->bp_count : max;
    for (idx = 0; idx < count; idx++) {
        out[idx].id = dbg->bps[idx].id;
        out[idx].type = dbg->bps[idx].type;
        out[idx].start = dbg->bps[idx].start;
        out[idx].end = dbg->bps[idx].end;
        out[idx].enabled = dbg->bps[idx].enabled;
        out[idx].hits = dbg->bps[idx].hits;
        snprintf(out[idx].condition, sizeof(out[idx].condition), "%s", dbg->bps[idx].cond);
    }
    pthread_mutex_unlock(&dbg->mtx);
    return count;
}

void colecodebug_breakpoint_clear(colecodebug *dbg)
{
    if (!dbg) return;
    pthread_mutex_lock(&dbg->mtx);
    dbg->bp_count = 0;
    mark_dirty_locked(dbg);
    pthread_mutex_unlock(&dbg->mtx);
}

void colecodebug_breakpoint_clear_hits(colecodebug *dbg)
{
    int idx;
    if (!dbg) return;
    pthread_mutex_lock(&dbg->mtx);
    for (idx = 0; idx < dbg->bp_count; idx++) dbg->bps[idx].hits = 0;
    dbg->gen++;
    pthread_mutex_unlock(&dbg->mtx);
}

/* The older single-address API: execute breakpoints in the same table. */

void colecodebug_bp_set(colecodebug *dbg, uint16_t addr)
{
    if (!colecodebug_breakpoint_check(dbg, addr))
        colecodebug_breakpoint_add(dbg, COLECODEBUG_BP_EXEC, addr, addr, "", NULL, 0);
}

void colecodebug_bp_clear(colecodebug *dbg, uint16_t addr)
{
    int idx;
    pthread_mutex_lock(&dbg->mtx);
    for (idx = dbg->bp_count - 1; idx >= 0; idx--)
        if (dbg->bps[idx].type == COLECODEBUG_BP_EXEC && dbg->bps[idx].start == addr)
            bp_remove_at_locked(dbg, idx);
    mark_dirty_locked(dbg);
    pthread_mutex_unlock(&dbg->mtx);
}

void colecodebug_bp_toggle(colecodebug *dbg, uint16_t addr)
{
    colecodebug_breakpoint_toggle(dbg, addr);
}

void colecodebug_bp_clear_all(colecodebug *dbg)
{
    colecodebug_breakpoint_clear(dbg);
}

int colecodebug_bp_is_set(colecodebug *dbg, uint16_t addr)
{
    return colecodebug_breakpoint_check(dbg, addr);
}

int colecodebug_bp_list(colecodebug *dbg, uint16_t *out, int max)
{
    int count = 0, idx;
    pthread_mutex_lock(&dbg->mtx);
    for (idx = 0; idx < dbg->bp_count && count < max; idx++)
        if (dbg->bps[idx].type == COLECODEBUG_BP_EXEC) out[count++] = dbg->bps[idx].start;
    pthread_mutex_unlock(&dbg->mtx);
    return count;
}

/* ---- CPU / memory ---------------------------------------------------------- */

void colecodebug_get_regs(colecodebug *dbg, adamcore_z80_regs *out)
{
    pthread_mutex_lock(&dbg->mtx);
    if (dbg->core)
        adamcore_get_regs(dbg->core, out);
    else
        memset(out, 0, sizeof(*out));
    pthread_mutex_unlock(&dbg->mtx);
}

void colecodebug_set_regs(colecodebug *dbg, const adamcore_z80_regs *in)
{
    pthread_mutex_lock(&dbg->mtx);
    if (dbg->core)
        adamcore_set_regs(dbg->core, in);
    dbg->gen++;
    pthread_mutex_unlock(&dbg->mtx);
}

int colecodebug_regs_set(colecodebug *dbg, const adamcore_z80_regs *regs, char *err, int errsz)
{
    adamcore_z80_regs before;
    adamcore_z80_regs applied;

    if (err && errsz > 0) err[0] = '\0';
    if (!dbg || !regs) return 0;
    if (regs->im > 2) {
        if (err && errsz > 0) snprintf(err, (size_t)errsz, "IM is 0, 1 or 2");
        return 0;
    }
    if (regs->iff1 > 1 || regs->iff2 > 1 || regs->halted > 1) {
        if (err && errsz > 0) snprintf(err, (size_t)errsz, "IFF1, IFF2 and HALT are 0 or 1");
        return 0;
    }
    pthread_mutex_lock(&dbg->mtx);
    if (!dbg->core || !dbg->paused || !dbg->at_break) {
        pthread_mutex_unlock(&dbg->mtx);
        if (err && errsz > 0) snprintf(err, (size_t)errsz, "the machine is running (stop it first)");
        return 0;
    }
    adamcore_get_regs(dbg->core, &before);
    applied = *regs;
    applied.cycles = before.cycles;
    adamcore_set_regs(dbg->core, &applied);
    dbg->gen++;
    pthread_mutex_unlock(&dbg->mtx);
    if (err && errsz > 0 && applied.halted && applied.pc != before.pc)
        snprintf(err, (size_t)errsz,
                 "PC changed while the CPU is halted: it stays halted until HALT is cleared "
                 "or an interrupt arrives");
    return 1;
}

int colecodebug_register_set(colecodebug *dbg, const char *name, long value, char *err, int errsz)
{
    adamcore_z80_regs regs;
    const int code = name ? reg_code(name) : -1;

    if (err && errsz > 0) err[0] = '\0';
    if (code < 0) {
        if (err && errsz > 0) snprintf(err, (size_t)errsz, "unknown register '%s'", name ? name : "");
        return 0;
    }
    if (code == R_IM && (value < 0 || value > 2)) {
        if (err && errsz > 0) snprintf(err, (size_t)errsz, "IM is 0, 1 or 2");
        return 0;
    }
    if (code == R_IFF1 || code == R_IFF2)
        value = value != 0;
    colecodebug_get_regs(dbg, &regs);
    reg_store(&regs, code, value);
    return colecodebug_regs_set(dbg, &regs, err, errsz);
}

int colecodebug_read_mem(colecodebug *dbg, uint16_t addr, uint8_t *dst, int count)
{
    pthread_mutex_lock(&dbg->mtx);
    if (!dbg->core) {
        pthread_mutex_unlock(&dbg->mtx);
        memset(dst, 0, (size_t)count);
        return 0;
    }
    adamcore_peek_block(dbg->core, addr, dst, (uint32_t)count);
    pthread_mutex_unlock(&dbg->mtx);
    return count;
}

int colecodebug_write_mem(colecodebug *dbg, uint16_t addr, const uint8_t *src, int count)
{
    int idx;
    pthread_mutex_lock(&dbg->mtx);
    if (!dbg->core) {
        pthread_mutex_unlock(&dbg->mtx);
        return 0;
    }
    for (idx = 0; idx < count; idx++)
        adamcore_poke(dbg->core, (uint16_t)(addr + idx), src[idx]);
    dbg->gen++;
    pthread_mutex_unlock(&dbg->mtx);
    return count;
}

/* ---- disassembly ----------------------------------------------------------- */

int colecodebug_disassemble(colecodebug *dbg, uint16_t addr, int count,
                            colecodasm_line *out)
{
    int idx;
    uint16_t pc = addr;

    pthread_mutex_lock(&dbg->mtx);
    if (!dbg->core) {
        pthread_mutex_unlock(&dbg->mtx);
        return 0;
    }
    for (idx = 0; idx < count; idx++) {
        uint8_t code[4];
        z80d_insn insn;
        colecodasm_line *line = &out[idx];
        uint16_t off = 0;
        const char *sym;

        adamcore_peek_block(dbg->core, pc, code, 4);
        z80_disassemble(&insn, pc, code);
        line->addr = pc;
        line->len = insn.len;
        memcpy(line->bytes, insn.bytes, 4);
        memcpy(line->text, insn.text, sizeof(line->text));
        line->target = insn.target;
        line->flags = insn.flags;
        sym = symtabs_at(&dbg->syms, pc, &off);
        line->symbol = (sym && off == 0) ? sym : NULL;
        pc = (uint16_t)(pc + insn.len);
    }
    pthread_mutex_unlock(&dbg->mtx);
    return count;
}

static void fill_line_locked(colecodebug *dbg, colecodebug_line *line, uint16_t pc,
                             const uint8_t code[4], uint16_t cur_pc)
{
    z80d_insn insn;
    int idx;
    uint16_t off = 0;
    const char *sym;
    bp_entry *entry;

    z80_disassemble(&insn, pc, code);
    memset(line, 0, sizeof(*line));
    line->address = pc;
    line->length = insn.len;
    line->is_pc = pc == cur_pc;
    for (idx = 0; idx < insn.len && idx < 4; idx++)
        snprintf(line->bytes + idx * 3, sizeof(line->bytes) - (size_t)idx * 3, "%02X ", insn.bytes[idx]);
    snprintf(line->disasm, sizeof(line->disasm), "%s", insn.text);
    sym = symtabs_at(&dbg->syms, pc, &off);
    if (sym && off == 0) snprintf(line->label, sizeof(line->label), "%s", sym);
    entry = bp_exec_at_locked(dbg, pc);
    if (entry) {
        line->has_breakpoint = 1;
        line->breakpoint_enabled = entry->enabled;
    }
    if (insn.flags & (COLECODASM_JUMP | COLECODASM_CALL)) {
        sym = symtabs_at(&dbg->syms, insn.target, &off);
        if (sym && off == 0) snprintf(line->comment, sizeof(line->comment), "%s", sym);
    }
}

int colecodebug_disassemble_lines(colecodebug *dbg, uint16_t addr, colecodebug_line *out,
                                  int max, int *pc_line)
{
    int idx;
    uint16_t at = addr, cur_pc = 0;

    if (pc_line) *pc_line = -1;
    if (!dbg || !out || max <= 0) return 0;
    pthread_mutex_lock(&dbg->mtx);
    if (!dbg->core) {
        pthread_mutex_unlock(&dbg->mtx);
        return 0;
    }
    {
        adamcore_z80_regs regs;
        adamcore_get_regs(dbg->core, &regs);
        cur_pc = regs.pc;
    }
    for (idx = 0; idx < max; idx++) {
        uint8_t code[4];
        adamcore_peek_block(dbg->core, at, code, 4);
        fill_line_locked(dbg, &out[idx], at, code, cur_pc);
        if (out[idx].is_pc && pc_line) *pc_line = idx;
        at = (uint16_t)(at + out[idx].length);
    }
    pthread_mutex_unlock(&dbg->mtx);
    return max;
}

int colecodebug_row_address(colecodebug *dbg, uint16_t addr, int rows)
{
    uint8_t buf[512 + 8];
    int idx;

    if (!dbg || rows == 0) return addr;
    if (rows > 0) {
        uint16_t at = addr;
        for (idx = 0; idx < rows; idx++) {
            uint8_t code[4];
            z80d_insn insn;
            colecodebug_read_mem(dbg, at, code, 4);
            z80_disassemble(&insn, at, code);
            at = (uint16_t)(at + insn.len);
        }
        return at;
    } else {
        /* A best guess: decode forward from a little way back and take the
         * row boundary `rows` before addr on the path that lands on addr. */
        const int want = -rows;
        int back = want * 3 + 6, start;
        uint16_t from, positions[256];
        int count = 0, hit = -1;
        if (back > 500) back = 500;
        start = (int)addr - back;
        if (start < 0) start = 0;
        from = (uint16_t)start;
        colecodebug_read_mem(dbg, from, buf,
                             (int)(addr - from) + 8 > (int)sizeof(buf) ? (int)sizeof(buf)
                                                                       : (int)(addr - from) + 8);
        {
            uint16_t at = from;
            while ((int)at < (int)addr && count < 256) {
                z80d_insn insn;
                uint8_t code[4];
                int offset = at - from, k;
                for (k = 0; k < 4; k++) code[k] = (offset + k < (int)sizeof(buf)) ? buf[offset + k] : 0;
                z80_disassemble(&insn, at, code);
                positions[count++] = at;
                at = (uint16_t)(at + insn.len);
            }
            if (at == addr) hit = count - want;
        }
        if (hit >= 0 && hit < count) return positions[hit];
        return (addr + rows) & 0xFFFF;
    }
}

/* ---- trace and history ------------------------------------------------------- */

void colecodebug_trace_enable(colecodebug *dbg, int enable)
{
    pthread_mutex_lock(&dbg->mtx);
    dbg->trace_want = enable ? 1 : 0;
    apply_trace_locked(dbg);
    pthread_mutex_unlock(&dbg->mtx);
}

int colecodebug_trace_enabled(colecodebug *dbg)
{
    int on;
    pthread_mutex_lock(&dbg->mtx);
    on = dbg->trace_want;
    pthread_mutex_unlock(&dbg->mtx);
    return on;
}

void colecodebug_history_enable(colecodebug *dbg, int enable)
{
    pthread_mutex_lock(&dbg->mtx);
    dbg->history_want = enable ? 1 : 0;
    apply_trace_locked(dbg);
    pthread_mutex_unlock(&dbg->mtx);
}

int colecodebug_history_enabled(colecodebug *dbg)
{
    int on;
    pthread_mutex_lock(&dbg->mtx);
    on = dbg->history_want;
    pthread_mutex_unlock(&dbg->mtx);
    return on;
}

/* The newest `max` records, oldest first, into out. Returns how many. */
static int ring_newest(colecodebug *dbg, colecotrace_entry *out, int max)
{
    uint64_t held, take, idx;

    if (max <= 0) return 0;
    pthread_mutex_lock(&dbg->trace_mtx);
    if (!dbg->ring) {
        pthread_mutex_unlock(&dbg->trace_mtx);
        return 0;
    }
    held = dbg->ring_total < TRACE_RING_LEN ? dbg->ring_total : TRACE_RING_LEN;
    take = held < (uint64_t)max ? held : (uint64_t)max;
    for (idx = 0; idx < take; idx++)
        ring_entry(&dbg->ring[(dbg->ring_total - take + idx) & (TRACE_RING_LEN - 1)], &out[idx]);
    pthread_mutex_unlock(&dbg->trace_mtx);
    return (int)take;
}

int colecodebug_trace_read(colecodebug *dbg, colecotrace_entry *out, int max)
{
    int count, idx;
    count = ring_newest(dbg, out, max);
    for (idx = 0; idx < count / 2; idx++) {     /* newest first */
        colecotrace_entry swap = out[idx];
        out[idx] = out[count - 1 - idx];
        out[count - 1 - idx] = swap;
    }
    return count;
}

void colecodebug_trace_clear(colecodebug *dbg)
{
    pthread_mutex_lock(&dbg->trace_mtx);
    dbg->ring_total = 0;
    pthread_mutex_unlock(&dbg->trace_mtx);
    colecodebug_touch(dbg);
}

int colecodebug_trace_count(colecodebug *dbg)
{
    uint64_t held;
    pthread_mutex_lock(&dbg->trace_mtx);
    held = dbg->ring_total < TRACE_RING_LEN ? dbg->ring_total : TRACE_RING_LEN;
    pthread_mutex_unlock(&dbg->trace_mtx);
    return (int)held;
}

uint64_t colecodebug_trace_total(colecodebug *dbg)
{
    uint64_t total;
    pthread_mutex_lock(&dbg->trace_mtx);
    total = dbg->ring_total;
    pthread_mutex_unlock(&dbg->trace_mtx);
    return total;
}

int colecodebug_trace_format(const colecotrace_entry *rec, char *dst, int dstsz)
{
    z80d_insn insn;

    if (!rec || !dst || dstsz <= 0) return 0;
    z80_disassemble(&insn, rec->pc, rec->bytes);
    return snprintf(dst, (size_t)dstsz, "%04X  %-18s AF=%04X BC=%04X DE=%04X HL=%04X SP=%04X",
                    rec->pc, insn.text, rec->af, rec->bc, rec->de, rec->hl, rec->sp);
}

int colecodebug_trace_match(const colecotrace_entry *rec, int filter, uint16_t low, uint16_t high)
{
    z80d_insn insn;

    if (!rec) return 0;
    if (filter == COLECODEBUG_TRACE_RANGE) {
        if (low > high) { uint16_t swap = low; low = high; high = swap; }
        return rec->pc >= low && rec->pc <= high;
    }
    if (filter == COLECODEBUG_TRACE_BRANCHES) {
        z80_disassemble(&insn, rec->pc, rec->bytes);
        return (insn.flags & (COLECODASM_JUMP | COLECODASM_CALL | COLECODASM_RET)) != 0;
    }
    return 1;
}

void colecodebug_history_clear(colecodebug *dbg)
{
    colecodebug_trace_clear(dbg);
}

uint64_t colecodebug_history_count(colecodebug *dbg)
{
    return colecodebug_trace_total(dbg);
}

int colecodebug_history_copy(colecodebug *dbg, colecohist_entry *out, int max)
{
    return ring_newest(dbg, out, max);
}

int colecodebug_history_format(const colecohist_entry *rec, char *dst, int dstsz)
{
    z80d_insn insn;
    char flags[9];
    static const char k_names[] = "SZYHXPNC";
    int idx;
    int len;

    if (!rec || !dst || dstsz <= 0) return 0;
    z80_disassemble(&insn, rec->pc, rec->bytes);
    for (idx = 0; idx < 8; idx++)
        flags[idx] = ((rec->af >> (7 - idx)) & 1) ? k_names[idx] : '.';
    flags[8] = '\0';
    len = snprintf(dst, (size_t)dstsz,
                   "%04X  %-18s AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X SP=%04X  %s%s",
                   rec->pc, insn.text, rec->af, rec->bc, rec->de, rec->hl, rec->ix, rec->iy,
                   rec->sp, flags, rec->halted ? " HALT" : "");
    if (len > 0 && len < dstsz && rec->repeat)
        len += snprintf(dst + len, (size_t)(dstsz - len), " x%u", (unsigned)rec->repeat + 1);
    return len < dstsz ? len : dstsz - 1;
}

int colecodebug_history_match(const colecohist_entry *rec, int filter, uint16_t low, uint16_t high)
{
    return colecodebug_trace_match(rec, filter, low, high);
}

/* ---- FujiNet ---------------------------------------------------------------- */

int colecodebug_fujinet_trace(colecodebug *dbg, coleco_fuji_packet *out, int max)
{
    int count;
    pthread_mutex_lock(&dbg->fuji_mtx);
    count = fujitrace_read(&dbg->fuji, out, max);
    pthread_mutex_unlock(&dbg->fuji_mtx);
    return count;
}

void colecodebug_fujinet_trace_clear(colecodebug *dbg)
{
    pthread_mutex_lock(&dbg->fuji_mtx);
    fujitrace_clear(&dbg->fuji);
    pthread_mutex_unlock(&dbg->fuji_mtx);
}

static const char *boot_err_name(int err)
{
    switch (err) {
    case FN_BOOT_ERR_TOOBIG: return "too big";
    case FN_BOOT_ERR_TRUNCATED: return "truncated";
    case FN_BOOT_ERR_NOMAP: return "no mapper fits";
    case FN_BOOT_ERR_STOREBUSY: return "store busy";
    default: return "error";
    }
}

static const char *txn_err_name(int err)
{
    switch (err) {
    case FN_ERR_OK: return "ok";
    case FN_ERR_NOLINK: return "no link";
    case FN_ERR_TIMEOUT: return "timeout";
    case FN_ERR_BADFRAME: return "bad frame";
    case FN_ERR_TOOBIG: return "too big";
    default: return "?";
    }
}

/* Bounded append, as in vdpview.c: the cursor never passes max-1. */
static void appendf(char *out, int max, int *pos, const char *fmt, ...)
{
    va_list args;
    int wrote;

    if (*pos >= max - 1)
        return;
    va_start(args, fmt);
    wrote = vsnprintf(out + *pos, (size_t)(max - *pos), fmt, args);
    va_end(args);
    if (wrote < 0)
        return;
    *pos += (wrote >= max - *pos) ? (max - *pos - 1) : wrote;
}

int colecodebug_fujinet_state(colecodebug *dbg, char *dst, int dstsz)
{
    coleco_cart *cart = coleco_host_cart();
    coleco_cart_info info;
    uint32_t packets[2];
    uint64_t bytes[2];
    int pos = 0;

    if (dstsz <= 0)
        return 0;
    dst[0] = '\0';
    pthread_mutex_lock(&dbg->fuji_mtx);
    packets[0] = dbg->fuji.packets[0];
    packets[1] = dbg->fuji.packets[1];
    bytes[0] = dbg->fuji.bytes[0];
    bytes[1] = dbg->fuji.bytes[1];
    pthread_mutex_unlock(&dbg->fuji_mtx);

    if (!cart) {
        appendf(dst, dstsz, &pos, "No cartridge: the machine is off,\n"
                                  "or FujiNet is off.\n");
    } else {
        coleco_cart_get_info(cart, &info);
        appendf(dst, dstsz, &pos, "Link       %s, %s\n", info.hostport,
                info.link_up ? "up" : "down");
        appendf(dst, dstsz, &pos, "Serving    %s, %u bytes",
                colmap_kindname(info.kind), (unsigned)info.size);
        if (info.kind == COLMAP_MEGACART || info.kind == COLMAP_ACTIVISION)
            appendf(dst, dstsz, &pos, ", bank %u", info.bank);
        appendf(dst, dstsz, &pos, "\n");
        if (info.staged)
            appendf(dst, dstsz, &pos, "Staged     %s, %u bytes%s\n",
                    colmap_kindname(info.staged_kind), (unsigned)info.staged_size,
                    info.swap_armed ? ", swap armed" : "");
        else
            appendf(dst, dstsz, &pos, "Staged     nothing\n");
        if (!info.mailbox_live) {
            /* a plain ROM, or a game swapped in over the CONFIG client */
            appendf(dst, dstsz, &pos, "Mailbox    off (no FUJI claim)\n");
        } else {
            appendf(dst, dstsz, &pos, "Mailbox    live\n");
            appendf(dst, dstsz, &pos, "ACKSEQ     %02X\n", info.ackseq);
            appendf(dst, dstsz, &pos, "Status     %02X%s%s\n", info.status,
                    (info.status & FN_R_STATUS_LINK) ? " link" : "",
                    (info.status & FN_R_STATUS_BUSY) ? " busy" : "");
            appendf(dst, dstsz, &pos, "Last reply %s, %u bytes, %s\n",
                    info.reply_cmd == 0x06   ? "ACK"
                    : info.reply_cmd == 0x15 ? "NAK"
                                             : "none",
                    (unsigned)info.rxlen, txn_err_name(info.err));
            if (info.boot_state == FN_BOOT_IDLE)
                appendf(dst, dstsz, &pos, "Boot       idle\n");
            else if (info.boot_state == FN_BOOT_XFER)
                appendf(dst, dstsz, &pos, "Boot       receiving, %u%%\n",
                        info.boot_pct);
            else if (info.boot_state == FN_BOOT_READY)
                appendf(dst, dstsz, &pos, "Boot       image ready\n");
            else
                appendf(dst, dstsz, &pos, "Boot       failed (%s)\n",
                        boot_err_name(info.boot_err));
        }
    }
    appendf(dst, dstsz, &pos, "\nSince the trace was cleared\n");
    appendf(dst, dstsz, &pos, "Sent       %u packets, %llu bytes\n",
            (unsigned)packets[0], (unsigned long long)bytes[0]);
    appendf(dst, dstsz, &pos, "Received   %u packets, %llu bytes\n",
            (unsigned)packets[1], (unsigned long long)bytes[1]);
    return pos;
}

/* ---- symbols ---------------------------------------------------------------- */

int colecodebug_symbols_load(colecodebug *dbg, const char *path, const char *table)
{
    int result;
    pthread_mutex_lock(&dbg->mtx);
    result = symtabs_load_file(&dbg->syms, path, table);
    dbg->sym_gen++;
    dbg->gen++;
    pthread_mutex_unlock(&dbg->mtx);
    return result;
}

const char *colecodebug_symbol_at(colecodebug *dbg, uint16_t addr, uint16_t *offset)
{
    return symtabs_at(&dbg->syms, addr, offset);
}

int colecodebug_symbol_find(colecodebug *dbg, const char *name, uint16_t *addr)
{
    return symtabs_find(&dbg->syms, name, addr);
}

int colecodebug_load_symbols(colecodebug *dbg, const char *path, char *msg, int msgsz)
{
    int result;
    if (!dbg || !path) return -1;
    pthread_mutex_lock(&dbg->mtx);
    result = symtabs_load_file(&dbg->syms, path, "user");
    dbg->sym_gen++;
    dbg->gen++;
    pthread_mutex_unlock(&dbg->mtx);
    if (msg && msgsz > 0) {
        if (result < 0)
            snprintf(msg, (size_t)msgsz, "could not read %s", path);
        else
            snprintf(msg, (size_t)msgsz, "%d label%s from %s", result, result == 1 ? "" : "s", path);
    }
    return result;
}

int colecodebug_label_address(colecodebug *dbg, const char *label)
{
    uint16_t addr;
    if (!dbg || !label || !colecodebug_symbol_find(dbg, label, &addr)) return -1;
    return addr;
}

int colecodebug_address_label(colecodebug *dbg, uint16_t addr, char *dst, int dstsz)
{
    uint16_t off = 0;
    const char *sym;

    if (dst && dstsz > 0) dst[0] = '\0';
    sym = dbg ? symtabs_at(&dbg->syms, addr, &off) : NULL;
    if (!sym || off != 0) return 0;
    if (dst && dstsz > 0) snprintf(dst, (size_t)dstsz, "%s", sym);
    return (int)strlen(sym);
}

int colecodebug_set_label(colecodebug *dbg, uint16_t addr, const char *label)
{
    char line[96];
    size_t have, add;
    char *grown;

    if (!dbg || !label || !*label || strpbrk(label, " \t;\n")) return -1;
    snprintf(line, sizeof(line), "%04X %s\n", (unsigned)addr, label);
    pthread_mutex_lock(&dbg->mtx);
    have = dbg->user_syms ? strlen(dbg->user_syms) : 0;
    add = strlen(line);
    grown = realloc(dbg->user_syms, have + add + 1);
    if (!grown) {
        pthread_mutex_unlock(&dbg->mtx);
        return -1;
    }
    memcpy(grown + have, line, add + 1);
    dbg->user_syms = grown;
    symtabs_load_text(&dbg->syms, dbg->user_syms, "labels");
    dbg->sym_gen++;
    dbg->gen++;
    pthread_mutex_unlock(&dbg->mtx);
    return 0;
}

int colecodebug_symbols_count(colecodebug *dbg)
{
    int total = 0, idx;
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    for (idx = 0; idx < dbg->syms.count; idx++) total += dbg->syms.tables[idx].count;
    pthread_mutex_unlock(&dbg->mtx);
    return total;
}

/* mtx held. */
static const sym_entry *symbol_entry_locked(const colecodebug *dbg, int index)
{
    int idx;
    if (index < 0) return NULL;
    for (idx = 0; idx < dbg->syms.count; idx++) {
        const sym_table *table = &dbg->syms.tables[idx];
        if (index < table->count) return &table->entries[index];
        index -= table->count;
    }
    return NULL;
}

int colecodebug_symbols_get(colecodebug *dbg, int index, uint16_t *addr, char *name, int namesz)
{
    const sym_entry *entry;
    int found = 0;
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    entry = symbol_entry_locked(dbg, index);
    if (entry) {
        if (addr) *addr = entry->addr;
        if (name && namesz > 0) snprintf(name, (size_t)namesz, "%s", entry->name);
        found = 1;
    }
    pthread_mutex_unlock(&dbg->mtx);
    return found;
}

typedef struct { uint16_t addr; const char *name; int index; } sym_pick;

static int sym_pick_cmp(const void *left, const void *right)
{
    const sym_pick *lhs = left, *rhs = right;
    if (lhs->addr != rhs->addr) return lhs->addr < rhs->addr ? -1 : 1;
    return strcasecmp(lhs->name, rhs->name);
}

static int contains_nocase(const char *hay, const char *needle)
{
    const size_t len = strlen(needle);
    if (!len) return 1;
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, len)) return 1;
    return 0;
}

int colecodebug_symbols_filter(colecodebug *dbg, const char *text, int *out, int max)
{
    sym_pick *pick;
    int total, count = 0, idx, base = 0, table_idx;

    if (!dbg || !out || max <= 0) return 0;
    pthread_mutex_lock(&dbg->mtx);
    total = 0;
    for (idx = 0; idx < dbg->syms.count; idx++) total += dbg->syms.tables[idx].count;
    pick = total > 0 ? malloc((size_t)total * sizeof(*pick)) : NULL;
    if (!pick) {
        pthread_mutex_unlock(&dbg->mtx);
        return 0;
    }
    for (table_idx = 0; table_idx < dbg->syms.count; table_idx++) {
        const sym_table *table = &dbg->syms.tables[table_idx];
        for (idx = 0; idx < table->count && count < total; idx++)
            if (contains_nocase(table->entries[idx].name, text ? text : "")) {
                pick[count].addr = table->entries[idx].addr;
                pick[count].name = table->entries[idx].name;
                pick[count].index = base + idx;
                count++;
            }
        base += table->count;
    }
    qsort(pick, (size_t)count, sizeof(*pick), sym_pick_cmp);
    pthread_mutex_unlock(&dbg->mtx);
    if (count > max) count = max;
    for (idx = 0; idx < count; idx++) out[idx] = pick[idx].index;
    free(pick);
    return count;
}

unsigned colecodebug_symbols_generation(colecodebug *dbg)
{
    unsigned gen;
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    gen = dbg->sym_gen;
    pthread_mutex_unlock(&dbg->mtx);
    return gen;
}

void colecodebug_clear_symbols(colecodebug *dbg)
{
    if (!dbg) return;
    pthread_mutex_lock(&dbg->mtx);
    symtabs_free(&dbg->syms);
    free(dbg->user_syms);
    dbg->user_syms = NULL;
    symtabs_load_text(&dbg->syms, colecodebug_builtin_os7_sym, "os7");
    dbg->sym_gen++;
    dbg->gen++;
    pthread_mutex_unlock(&dbg->mtx);
}

const sym_tables *colecodebug_symtabs(colecodebug *dbg)
{
    return dbg ? &dbg->syms : NULL;
}

/* ---- search ------------------------------------------------------------------------ */

static int match_at(const uint8_t *mem, unsigned addr, const uint8_t *pat, int count, uint8_t mask,
                    int flags)
{
    int idx;
    for (idx = 0; idx < count; idx++) {
        uint8_t have = mem[addr + (unsigned)idx];
        uint8_t want = pat[idx];
        if (flags & COLECODEBUG_SEARCH_NOCASE) {
            if (have >= 'a' && have <= 'z') have = (uint8_t)(have - 32);
            if (want >= 'a' && want <= 'z') want = (uint8_t)(want - 32);
        }
        if ((have & mask) != (want & mask)) return 0;
    }
    return 1;
}

/* The CPU's 64K as mapped now; NULL unless stopped. */
static uint8_t *snapshot_64k(colecodebug *dbg)
{
    uint8_t *mem;
    if (!colecodebug_is_paused(dbg)) return NULL;
    mem = malloc(0x10000);
    if (!mem) return NULL;
    if (colecodebug_read_mem(dbg, 0, mem, 0x10000) != 0x10000) {
        free(mem);
        return NULL;
    }
    return mem;
}

static int search_buf(const uint8_t *mem, const uint8_t *pat, int count, uint8_t mask, int flags,
                      unsigned from, unsigned to, uint16_t *out, int max)
{
    unsigned addr;
    int found = 0;
    for (addr = from; addr <= to && addr + (unsigned)count - 1 <= 0xFFFF && found < max; addr++)
        if (match_at(mem, addr, pat, count, mask, flags)) out[found++] = (uint16_t)addr;
    return found;
}

int colecodebug_search(colecodebug *dbg, const uint8_t *pat, int count, uint8_t mask, int flags,
                       uint16_t from, uint16_t to, uint16_t *out, int max)
{
    uint8_t *mem;
    int found;

    if (!dbg || !pat || count <= 0 || !out || max <= 0) return -1;
    mem = snapshot_64k(dbg);
    if (!mem) return -1;
    if (from > to) { uint16_t swap = from; from = to; to = swap; }
    found = search_buf(mem, pat, count, mask, flags, from, to, out, max);
    free(mem);
    return found;
}

int colecodebug_search_next(colecodebug *dbg, const uint8_t *pat, int count, uint8_t mask,
                            int flags, uint16_t from, int backward, uint16_t *found)
{
    uint8_t *mem;
    uint16_t hit = 0;
    int result = 0;

    if (!dbg || !pat || count <= 0 || !found) return -1;
    mem = snapshot_64k(dbg);
    if (!mem) return -1;
    if (!backward) {
        /* after `from` to the end, then 0..from (which includes `from`) */
        if ((unsigned)from + 1 <= 0xFFFF &&
            search_buf(mem, pat, count, mask, flags, (unsigned)from + 1, 0xFFFF, &hit, 1))
            result = 1;
        else if (search_buf(mem, pat, count, mask, flags, 0, from, &hit, 1))
            result = 1;
    } else {
        /* the nearest match before `from`, else the last one from `from` up */
        int pass;
        for (pass = 0; pass < 2 && !result; pass++) {
            const int start = pass == 0 ? (int)from - 1 : 0xFFFF;
            const int stop = pass == 0 ? 0 : (int)from;
            int addr;
            for (addr = start; addr >= stop; addr--)
                if (addr + count - 1 <= 0xFFFF && match_at(mem, (unsigned)addr, pat, count, mask, flags)) {
                    hit = (uint16_t)addr;
                    result = 1;
                    break;
                }
        }
    }
    free(mem);
    if (result) *found = hit;
    return result;
}

/* ---- files ------------------------------------------------------------------ */

int colecodebug_save(colecodebug *dbg, const char *kind, const char *path, char *msg, int msgsz)
{
    FILE *file;
    int result = 0;

    if (msg && msgsz > 0) msg[0] = '\0';
    if (!dbg || !kind || !path) return -1;
    file = fopen(path, kind[0] == 'd' || kind[0] == 'r' ? "w" : "wb");
    if (!file) {
        if (msg && msgsz > 0) snprintf(msg, (size_t)msgsz, "could not write %s", path);
        return -1;
    }
    if (!strcmp(kind, "dis")) {
        uint16_t at = 0;
        int rows = 0;
        while (rows < 0x10000) {
            colecodebug_line lines[256];
            int got = colecodebug_disassemble_lines(dbg, at, lines, 256, NULL), idx;
            if (got <= 0) break;
            for (idx = 0; idx < got; idx++) {
                uint32_t next = (uint32_t)lines[idx].address + (uint32_t)lines[idx].length;
                if (lines[idx].label[0]) fprintf(file, "%s:\n", lines[idx].label);
                fprintf(file, "  %04X  %-12s %s\n", lines[idx].address, lines[idx].bytes, lines[idx].disasm);
                rows++;
                if (next > 0xFFFF) { rows = 0x10000; break; }
                at = (uint16_t)next;
            }
        }
    } else if (!strcmp(kind, "mem")) {
        static uint8_t buf[0x10000];
        colecodebug_read_mem(dbg, 0, buf, 0x10000);
        fwrite(buf, 1, sizeof(buf), file);
    } else if (!strcmp(kind, "vram")) {
        static uint8_t buf[0x4000];
        colecodebug_vdp_read(dbg, 0, buf, 0x4000);
        fwrite(buf, 1, sizeof(buf), file);
    } else if (!strcmp(kind, "regs")) {
        char text[1024];
        colecodebug_command(dbg, "regs", text, sizeof(text));
        fputs(text, file);
    } else {
        result = -1;
    }
    fclose(file);
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, result == 0 ? "saved %s" : "could not save %s", path);
    return result;
}

/* ---- the Video tab's console text ------------------------------------------------- */

void colecodebug_set_video_text(const char *(*func)(const char *section))
{
    g_video_text = func;
}

const char *(*colecodebug_video_text_hook(void))(const char *section)
{
    return g_video_text;
}

/* ---- VDP -------------------------------------------------------------------- */

void colecodebug_vdp_snapshot(colecodebug *dbg, colecovdp_snapshot *out)
{
    pthread_mutex_lock(&dbg->mtx);
    if (!dbg->core) {
        pthread_mutex_unlock(&dbg->mtx);
        memset(out, 0, sizeof(*out));
        return;
    }
    memcpy(out->vram, adamcore_vdp_vram(dbg->core), sizeof(out->vram));
    adamcore_vdp_state(dbg->core, out->regs, &out->status, &out->addr);
    memcpy(out->palette565, adamcore_palette565(dbg->core), sizeof(out->palette565));
    pthread_mutex_unlock(&dbg->mtx);
}

int colecodebug_vdp_read(colecodebug *dbg, uint16_t addr, uint8_t *dst, int count)
{
    const uint8_t *vram;
    int idx;

    pthread_mutex_lock(&dbg->mtx);
    if (!dbg->core) {
        pthread_mutex_unlock(&dbg->mtx);
        memset(dst, 0, (size_t)count);
        return 0;
    }
    vram = adamcore_vdp_vram(dbg->core);
    for (idx = 0; idx < count; idx++)
        dst[idx] = vram[(addr + idx) & 0x3FFF];
    pthread_mutex_unlock(&dbg->mtx);
    return count;
}

int colecodebug_vdp_write(colecodebug *dbg, uint16_t addr, const uint8_t *src, int count)
{
    uint8_t *vram;
    int idx;

    pthread_mutex_lock(&dbg->mtx);
    if (!dbg->core) {
        pthread_mutex_unlock(&dbg->mtx);
        return 0;
    }
    /* adamcore hands out VRAM read-only and has no poke entry point; the
     * cast is deliberate rather than a patch to the staged tree (which
     * cmake/StageAdamcore.cmake regenerates from upstream). Storing into
     * the array is the whole of a VRAM write -- the address register and
     * read-ahead latch belong to the port protocol, not to the RAM, so
     * leaving them alone is what keeps a transfer in flight intact. */
    vram = (uint8_t *)adamcore_vdp_vram(dbg->core);
    for (idx = 0; idx < count; idx++)
        vram[(addr + idx) & 0x3FFF] = src[idx];
    pthread_mutex_unlock(&dbg->mtx);
    return count;
}
