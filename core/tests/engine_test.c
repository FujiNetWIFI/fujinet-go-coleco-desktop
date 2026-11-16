/*
 * engine_test -- the debugger engine against a real running machine, with no
 * BIOS: two tiny hand-assembled programs stand in for OS7 (the machine only
 * needs 8 KB of ROM at $0000), so this runs everywhere, including CI.
 *
 * Program A is a loop that touches everything the engine can watch:
 *
 *   0000  DI / LD SP,$7000 / LD B,0
 *   0006  INC B                 ; the loop top
 *   0007  LD A,B
 *   0008  LD ($6000),A          ; a write
 *   000B  LD A,($6001)          ; a read
 *   000E  CALL $0020            ; INC C / RET
 *   0011  IN A,($FC)            ; a port read
 *   0013  OUT ($44),A           ; a port write
 *   0015  JP $0006
 *
 * Program B writes the SN76489 (and, with an SGM, the AY) and halts.
 *
 * It covers the engine's contract and the latent bugs the port fixed: a
 * resume from a breakpoint re-fired the same breakpoint; a pause landed on a
 * frame boundary; the core was freed under a polling window; breakpoints and
 * hooks did not survive a session restart; stop state was read unlocked.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host.h"
#include "colecodebug.h"
#include "colecomemtools.h"

static int failures;

static void check(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static void nap(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint8_t g_loop_bios[8192];
static uint8_t g_halt_bios[8192];
static uint8_t g_vdp_bios[8192];

static void put(uint8_t *image, unsigned addr, const uint8_t *code, size_t len)
{
    memcpy(image + addr, code, len);
}

static void build_programs(void)
{
    static const uint8_t loop_main[] = {
        0xF3,                   /* 0000 DI */
        0x31, 0x00, 0x70,       /* 0001 LD SP,$7000 */
        0x06, 0x00,             /* 0004 LD B,0 */
        0x04,                   /* 0006 INC B */
        0x78,                   /* 0007 LD A,B */
        0x32, 0x00, 0x60,       /* 0008 LD ($6000),A */
        0x3A, 0x01, 0x60,       /* 000B LD A,($6001) */
        0xCD, 0x20, 0x00,       /* 000E CALL $0020 */
        0xDB, 0xFC,             /* 0011 IN A,($FC) */
        0xD3, 0x44,             /* 0013 OUT ($44),A */
        0xC3, 0x06, 0x00,       /* 0015 JP $0006 */
    };
    static const uint8_t loop_sub[] = { 0x0C, 0xC9 }; /* 0020 INC C / RET */
    static const uint8_t halt_main[] = {
        0xF3,                               /* 0000 DI */
        0x31, 0x00, 0x70,                   /* 0001 LD SP,$7000 */
        0x3E, 0x8E, 0xD3, 0xFF,             /* tone 1 period, low nibble E */
        0x3E, 0x04, 0xD3, 0xFF,             /* ... high bits 4: period $4E */
        0x3E, 0x90, 0xD3, 0xFF,             /* tone 1 attenuation 0 */
        0x3E, 0xBF, 0xD3, 0xFF,             /* tone 2 attenuation 15 */
        0x3E, 0xE5, 0xD3, 0xFF,             /* noise: white, rate 1 */
        0x3E, 0xFF, 0xD3, 0xFF,             /* noise attenuation 15 */
        0x3E, 0x00, 0xD3, 0x50,             /* AY register 0 ... */
        0x3E, 0x34, 0xD3, 0x51,             /* ... = $34 */
        0x3E, 0x01, 0xD3, 0x50,
        0x3E, 0x02, 0xD3, 0x51,             /* register 1 = 2: period $234 */
        0x3E, 0x07, 0xD3, 0x50,
        0x3E, 0xF8, 0xD3, 0x51,             /* mixer: tones on, noise off */
        0x3E, 0x08, 0xD3, 0x50,
        0x3E, 0x0C, 0xD3, 0x51,             /* level 12 */
        0x3E, 0x01, 0xD3, 0x53,             /* SGM RAM on */
        0x76,                               /* HALT */
        0x18, 0xFD,                         /* JR -3 (never reached) */
    };
    static const uint8_t vdp_main[] = {
        0xF3, 0x31, 0x00, 0x70,             /* DI / LD SP,$7000 */
        0x3E, 0x00, 0xD3, 0xA1,             /* 0004 VDP address low 0 */
        0x3E, 0x40, 0xD3, 0xA1,             /* 0008 high $40: write VRAM at 0 */
        0x3E, 0x55, 0xD3, 0xA0,             /* 000C data $55 */
        0xC3, 0x04, 0x00,                   /* 0010 JP $0004 */
    };
    put(g_vdp_bios, 0x0000, vdp_main, sizeof vdp_main);
    put(g_loop_bios, 0x0000, loop_main, sizeof loop_main);
    put(g_loop_bios, 0x0020, loop_sub, sizeof loop_sub);
    put(g_halt_bios, 0x0000, halt_main, sizeof halt_main);
}

static int start_machine(const uint8_t *image, int sgm)
{
    coleco_host_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.os7 = image;
    opts.fujinet_hostport = "";     /* no cartridge device */
    opts.sgm = sgm;
    opts.audio_rate = 44100;
    return coleco_host_start(&opts);
}

static int wait_stopped(colecodebug *dbg, int ms)
{
    int waited;
    for (waited = 0; waited < ms; waited += 5) {
        if (colecodebug_is_paused(dbg)) return 1;
        nap(5);
    }
    return 0;
}

/* Stopped at a stop newer than `serial`. */
static int wait_new_stop(colecodebug *dbg, uint64_t serial, int ms)
{
    int waited;
    for (waited = 0; waited < ms; waited += 5) {
        if (colecodebug_is_paused(dbg) && colecodebug_stop_serial(dbg) != serial) return 1;
        nap(5);
    }
    return 0;
}

static int pause_machine(colecodebug *dbg)
{
    colecodebug_pause(dbg);
    return wait_stopped(dbg, 3000);
}

static adamcore_z80_regs regs_now(colecodebug *dbg)
{
    adamcore_z80_regs regs;
    colecodebug_get_regs(dbg, &regs);
    return regs;
}

/* Lets the machine go and waits for the next stop. */
static int go_and_wait(colecodebug *dbg)
{
    const uint64_t serial = colecodebug_stop_serial(dbg);
    colecodebug_resume(dbg);
    return wait_new_stop(dbg, serial, 3000);
}

static colecodebug_snapshot snap_now(colecodebug *dbg)
{
    colecodebug_snapshot snap;
    colecodebug_stop_snapshot(dbg, &snap);
    return snap;
}

/* ---------------------------------------------------------------------------- */

static void test_expressions(colecodebug *dbg)
{
    long value = 0;
    char err[96];
    adamcore_z80_regs regs = regs_now(dbg);

    printf("expressions against a stopped machine:\n");
    check("a register", colecodebug_eval(dbg, "b + 1", &value, err, sizeof err) == 0 && value == regs.b + 1);
    check("a 16-bit register", colecodebug_eval(dbg, "sp", &value, err, sizeof err) == 0 && value == regs.sp);
    check("a byte read", colecodebug_eval(dbg, "[0]", &value, err, sizeof err) == 0 && value == 0xF3);
    check("a word read", colecodebug_eval(dbg, "{1}", &value, err, sizeof err) == 0 && value == 0x0031);
    check("a flag", colecodebug_eval(dbg, "zf", &value, err, sizeof err) == 0 && (value == 0 || value == 1));
    check("cycles", colecodebug_eval(dbg, "cycles", &value, err, sizeof err) == 0 && value > 0);
    check("frame", colecodebug_eval(dbg, "frame", &value, err, sizeof err) == 0 && value >= 0);
    check("an unknown name is refused", colecodebug_eval(dbg, "nosuch", &value, err, sizeof err) != 0 && err[0]);
    check("value/old/addr are unknown outside a watch stop",
          colecodebug_eval(dbg, "value", &value, err, sizeof err) != 0);
}

static void test_pause_and_breakpoints(colecodebug *dbg)
{
    int idx, mid_frame = 0, id;
    adamcore_z80_regs regs;
    uint64_t serial;
    colecodebug_breakpoint list[8];

    printf("pause lands on an instruction, not a frame boundary:\n");
    /* an unreachable breakpoint keeps the engine on the stepped path */
    id = colecodebug_breakpoint_add(dbg, COLECODEBUG_BP_EXEC, 0x3000, 0x3000, "", NULL, 0);
    check("a breakpoint can be added to a running machine", id > 0);
    /* a false condition and the trace keep the emulator thread busy for a good
     * part of each frame, so a pause has a chance to arrive mid-frame */
    colecodebug_breakpoint_add(dbg, COLECODEBUG_BP_EXEC, 0x0006, 0x0006, "b == 999", NULL, 0);
    colecodebug_trace_enable(dbg, 1);
    colecodebug_resume(dbg);
    nap(100);
    for (idx = 0; idx < 600 && (idx < 60 || mid_frame == 0); idx++) {
        int line = 0, tstate;
        if (!pause_machine(dbg)) break;
        {
            colecodebug_io io;
            colecodebug_io_get(dbg, &io);
            line = io.beam_line;
            tstate = io.beam_tstate;
        }
        if (line != 0 || tstate != 0) mid_frame++;
        colecodebug_resume(dbg);
        nap(3 + (idx * 7) % 13);
    }
    printf("      %d of %d pauses landed mid-frame\n", mid_frame, idx);
    check("pauses land mid-frame (a frame-boundary-only pause would give none)", mid_frame >= 1);
    colecodebug_trace_enable(dbg, 0);
    colecodebug_breakpoint_clear(dbg);
    colecodebug_resume(dbg);

    printf("a resume from a breakpoint does not fire it again:\n");
    id = colecodebug_breakpoint_add(dbg, COLECODEBUG_BP_EXEC, 0x0006, 0x0006, "", NULL, 0);
    check("break at the loop top", id > 0);
    check("it stops", wait_stopped(dbg, 3000));
    {
        colecodebug_snapshot snap = snap_now(dbg);
        check("the reason is a breakpoint", snap.stop.reason == COLECODEBUG_STOP_BREAKPOINT && snap.stop.bp_id == id);
        check("the text says so", !strcmp(snap.text, "breakpoint: exec $0006"));
        check("at $0006", snap.address == 0x0006 && regs_now(dbg).pc == 0x0006);
    }
    regs = regs_now(dbg);
    {
        int steady = 1;
        uint8_t last = regs.b;
        for (idx = 0; idx < 5; idx++) {
            if (!go_and_wait(dbg)) { steady = 0; break; }
            regs = regs_now(dbg);
            if (regs.pc != 0x0006 || regs.b != (uint8_t)(last + 1)) steady = 0;
            last = regs.b;
        }
        check("each resume runs exactly one loop (B +1) and stops at $0006 again", steady);
    }
    colecodebug_breakpoint_list(dbg, list, 8);
    check("every stop counted a hit", list[0].hits >= 6);
    check("the generation moved", colecodebug_generation(dbg) > 0);

    printf("conditions and enable/disable:\n");
    check("a condition is refused if it does not parse",
          !colecodebug_breakpoint_set_condition(dbg, id, "b ==", NULL, 0));
    check("a good condition is taken", colecodebug_breakpoint_set_condition(dbg, id, "b == 200", NULL, 0));
    colecodebug_breakpoint_clear_hits(dbg);
    check("it stops when the condition holds", go_and_wait(dbg));
    regs = regs_now(dbg);
    check("with B at 200", regs.b == 200 && regs.pc == 0x0006);
    colecodebug_breakpoint_list(dbg, list, 8);
    check("only the true evaluation is a hit", list[0].hits == 1);
    colecodebug_breakpoint_enable(dbg, id, 0);
    serial = colecodebug_stop_serial(dbg);
    colecodebug_resume(dbg);
    nap(150);
    check("a disabled breakpoint never stops it", !colecodebug_is_paused(dbg) && colecodebug_stop_serial(dbg) == serial);
    colecodebug_breakpoint_enable(dbg, id, 1);
    check("enabled again it does", wait_new_stop(dbg, serial, 3000));
    colecodebug_breakpoint_remove(dbg, id);
    check("removed, the table is empty", colecodebug_breakpoint_list(dbg, list, 8) == 0);
}

static void test_watchpoints(colecodebug *dbg)
{
    colecodebug_snapshot snap;
    adamcore_z80_regs regs;
    int id;
    char want[96];
    colecodebug_breakpoint list[8];

    printf("watchpoints stop at the next instruction:\n");
    id = colecodebug_breakpoint_add(dbg, COLECODEBUG_BP_WRITE, 0x6000, 0x6000, "", NULL, 0);
    check("bpw $6000", id > 0);
    check("it stops", go_and_wait(dbg));
    snap = snap_now(dbg);
    regs = regs_now(dbg);
    check("reason write", snap.stop.reason == COLECODEBUG_STOP_WATCH_WRITE && snap.stop.bp_id == id);
    check("the address is $6000", snap.stop.addr == 0x6000);
    check("stopped at the instruction after the store", regs.pc == 0x000B && snap.stop.pc == 0x000B);
    check("from_pc names the store", snap.stop.from_pc == 0x0008);
    check("value is what was stored", snap.stop.value == regs.a);
    snprintf(want, sizeof want, "wrote $6000 = $%02X (was $%02X) at $000B", snap.stop.value, snap.stop.old_value);
    check("the text", !strcmp(snap.text, want));
    printf("      %s\n", snap.text);
    {
        long value = 0;
        char err[64];
        check("value in an expression", colecodebug_eval(dbg, "value", &value, err, sizeof err) == 0 && value == snap.stop.value);
        check("old in an expression", colecodebug_eval(dbg, "old", &value, err, sizeof err) == 0 && value == snap.stop.old_value);
        check("addr in an expression", colecodebug_eval(dbg, "addr", &value, err, sizeof err) == 0 && value == 0x6000);
    }
    {
        const int first = snap.stop.value;
        check("the next stop is the next iteration", go_and_wait(dbg));
        snap = snap_now(dbg);
        check("old is the byte the last store left", snap.stop.old_value == first);
    }
    colecodebug_breakpoint_remove(dbg, id);

    id = colecodebug_breakpoint_add(dbg, COLECODEBUG_BP_WRITE, 0x6000, 0x6000, "value == 5 && old == 4", NULL, 0);
    check("a write condition in the grammar", id > 0);
    check("it stops on that store", go_and_wait(dbg));
    snap = snap_now(dbg);
    check("value 5 over 4", snap.stop.value == 5 && snap.stop.old_value == 4);
    colecodebug_breakpoint_remove(dbg, id);
    check("value is not an exec condition",
          colecodebug_breakpoint_add(dbg, COLECODEBUG_BP_EXEC, 0x0006, 0x0006, "nosuchname", NULL, 0) < 0);

    printf("an opcode fetch is not a read:\n");
    id = colecodebug_breakpoint_add(dbg, COLECODEBUG_BP_READ, 0x0000, 0x0021, "", NULL, 0);
    check("bpr over the whole program", id > 0);
    {
        const uint64_t serial = colecodebug_stop_serial(dbg);
        colecodebug_resume(dbg);
        nap(200);
        check("it never stops (fetches only)", !colecodebug_is_paused(dbg) && colecodebug_stop_serial(dbg) == serial);
        colecodebug_breakpoint_list(dbg, list, 8);
        check("and counts nothing", list[0].hits == 0);
        pause_machine(dbg);
    }
    colecodebug_breakpoint_remove(dbg, id);

    id = colecodebug_breakpoint_add(dbg, COLECODEBUG_BP_READ, 0x6001, 0x6001, "", NULL, 0);
    check("bpr $6001", id > 0);
    check("it stops", go_and_wait(dbg));
    snap = snap_now(dbg);
    regs = regs_now(dbg);
    check("reason read, after the load", snap.stop.reason == COLECODEBUG_STOP_WATCH_READ && regs.pc == 0x000E);
    check("value is what was read", snap.stop.value == regs.a && snap.stop.old_value == snap.stop.value);
    snprintf(want, sizeof want, "read $6001 = $%02X at $000E", snap.stop.value);
    check("the text", !strcmp(snap.text, want));
    colecodebug_breakpoint_remove(dbg, id);

    id = colecodebug_breakpoint_add(dbg, COLECODEBUG_BP_IN, 0xFC, 0xFC, "", NULL, 0);
    check("bpin $FC", id > 0);
    check("it stops", go_and_wait(dbg));
    snap = snap_now(dbg);
    regs = regs_now(dbg);
    check("reason in, at the OUT", snap.stop.reason == COLECODEBUG_STOP_WATCH_IN && regs.pc == 0x0013);
    check("value is the byte the IN got", snap.stop.value == regs.a && snap.stop.addr == 0xFC);
    snprintf(want, sizeof want, "in $FC = $%02X at $0013", snap.stop.value);
    check("the text", !strcmp(snap.text, want));
    colecodebug_breakpoint_remove(dbg, id);

    id = colecodebug_breakpoint_add(dbg, COLECODEBUG_BP_OUT, 0x40, 0x4F, "value == a", NULL, 0);
    check("bpout $40-$4F with a condition", id > 0);
    check("it stops", go_and_wait(dbg));
    snap = snap_now(dbg);
    regs = regs_now(dbg);
    check("reason out, at the JP", snap.stop.reason == COLECODEBUG_STOP_WATCH_OUT && regs.pc == 0x0015);
    check("port $44", snap.stop.addr == 0x44);
    colecodebug_breakpoint_remove(dbg, id);

    id = colecodebug_breakpoint_add(dbg, COLECODEBUG_BP_ACCESS, 0x6000, 0x6001, "", NULL, 0);
    check("bpa $6000-$6001", id > 0);
    check("it stops on the store", go_and_wait(dbg));
    check("reason write", snap_now(dbg).stop.reason == COLECODEBUG_STOP_WATCH_WRITE);
    check("then on the load", go_and_wait(dbg) && snap_now(dbg).stop.reason == COLECODEBUG_STOP_WATCH_READ);
    colecodebug_breakpoint_list(dbg, list, 8);
    check("one id, one hit count for both", list[0].hits >= 2 && list[0].type == COLECODEBUG_BP_ACCESS);
    colecodebug_breakpoint_remove(dbg, id);

    {
        char err[64];
        check("ports are 0-255", colecodebug_breakpoint_add(dbg, COLECODEBUG_BP_IN, 0, 0x100, "", err, sizeof err) < 0 && err[0]);
    }
}

static void test_stepping(colecodebug *dbg)
{
    adamcore_z80_regs before, regs;
    uint64_t serial;
    colecodebug_snapshot snap;
    int id;

    printf("run to, step into, over, out, and frame:\n");
    /* get to a known place: the loop top */
    id = colecodebug_breakpoint_add(dbg, COLECODEBUG_BP_EXEC, 0x0006, 0x0006, "", NULL, 0);
    check("stop at the loop top", go_and_wait(dbg) && regs_now(dbg).pc == 0x0006);
    colecodebug_breakpoint_remove(dbg, id);

    before = regs_now(dbg);
    serial = colecodebug_stop_serial(dbg);
    colecodebug_run_to(dbg, 0x0006);
    check("run to the current PC goes all the way round", wait_new_stop(dbg, serial, 3000));
    regs = regs_now(dbg);
    check("...and stops there with the loop run once", regs.pc == 0x0006 && regs.b == (uint8_t)(before.b + 1));
    snap = snap_now(dbg);
    check("a one-shot reads 'stopped at'", snap.stop.reason == COLECODEBUG_STOP_ONESHOT && !strcmp(snap.text, "stopped at $0006"));

    serial = colecodebug_stop_serial(dbg);
    colecodebug_run_to(dbg, 0x000E);
    check("run to the CALL", wait_new_stop(dbg, serial, 3000) && regs_now(dbg).pc == 0x000E);

    before = regs_now(dbg);
    serial = colecodebug_stop_serial(dbg);
    colecodebug_step_over(dbg);
    check("step over a CALL", wait_new_stop(dbg, serial, 3000));
    regs = regs_now(dbg);
    check("...lands after it with the subroutine run", regs.pc == 0x0011 && regs.c == (uint8_t)(before.c + 1));
    check("...and the stack where it was", regs.sp == before.sp);

    serial = colecodebug_stop_serial(dbg);
    colecodebug_step_into(dbg);
    check("step into an IN", wait_new_stop(dbg, serial, 3000) && regs_now(dbg).pc == 0x0013);
    snap = snap_now(dbg);
    check("...reason step", snap.stop.reason == COLECODEBUG_STOP_STEP && !strcmp(snap.text, "step at $0013"));

    serial = colecodebug_stop_serial(dbg);
    colecodebug_run_to(dbg, 0x0006);
    check("back to the loop top", wait_new_stop(dbg, serial, 3000));
    serial = colecodebug_stop_serial(dbg);
    colecodebug_run_to(dbg, 0x000E);
    wait_new_stop(dbg, serial, 3000);
    serial = colecodebug_stop_serial(dbg);
    colecodebug_step_into(dbg);
    check("step into a CALL", wait_new_stop(dbg, serial, 3000) && regs_now(dbg).pc == 0x0020);

    before = regs_now(dbg);
    serial = colecodebug_stop_serial(dbg);
    colecodebug_step_out(dbg);
    check("step out of it", wait_new_stop(dbg, serial, 3000));
    regs = regs_now(dbg);
    check("...to the return address with the stack popped", regs.pc == 0x0011 && regs.sp == (uint16_t)(before.sp + 2));

    {
        colecodebug_io io, after;
        colecodebug_io_get(dbg, &io);
        serial = colecodebug_stop_serial(dbg);
        colecodebug_frame(dbg);
        check("frame", wait_new_stop(dbg, serial, 3000));
        colecodebug_io_get(dbg, &after);
        snap = snap_now(dbg);
        check("it ended the frame", after.frames == io.frames + 1 && after.beam_line == 0 && after.beam_tstate == 0);
        check("reason frame", snap.stop.reason == COLECODEBUG_STOP_FRAME && strstr(snap.text, "frame at $") == snap.text);
    }

    printf("a stop after the engine was told to step reports through the callback path:\n");
    {
        colecodebug_snapshot first = snap_now(dbg), second;
        colecodebug_step_into(dbg);
        wait_new_stop(dbg, first.serial, 3000);
        second = snap_now(dbg);
        check("the serial moves with every stop", second.serial > first.serial);
        check("a snapshot is one critical section (paused, serial, text agree)",
              second.paused && second.stop.serial == second.serial && second.address == second.stop.pc);
    }
}

static void test_registers(colecodebug *dbg)
{
    adamcore_z80_regs regs, back;
    char err[200];
    long value;

    printf("registers:\n");
    regs = regs_now(dbg);
    regs.b = 0x42;
    regs.ix = 0x1234;
    regs.a2 = 0x11;
    regs.f2 = 0x22;
    regs.iff1 = 1;
    regs.iff2 = 0;
    regs.im = 2;
    check("regs_set while stopped", colecodebug_regs_set(dbg, &regs, err, sizeof err) == 1);
    back = regs_now(dbg);
    check("main, index, shadow, IFF and IM took", back.b == 0x42 && back.ix == 0x1234 && back.a2 == 0x11 &&
                                                        back.f2 == 0x22 && back.iff1 == 1 && back.im == 2);
    regs.im = 3;
    check("IM above 2 is refused", colecodebug_regs_set(dbg, &regs, err, sizeof err) == 0 && strstr(err, "IM"));
    regs.im = 2;
    regs.iff1 = 2;
    check("IFF is 0 or 1", colecodebug_regs_set(dbg, &regs, err, sizeof err) == 0);
    check("a register by name", colecodebug_register_set(dbg, "bc'", 0xBEEF, err, sizeof err) == 1 &&
                                    regs_now(dbg).b2 == 0xBE && regs_now(dbg).c2 == 0xEF);
    check("a flag by name", colecodebug_register_set(dbg, "zf", 1, err, sizeof err) == 1 && (regs_now(dbg).f & 0x40));
    check("...and cleared", colecodebug_register_set(dbg, "zf", 0, err, sizeof err) == 1 && !(regs_now(dbg).f & 0x40));
    check("a half register", colecodebug_register_set(dbg, "ixl", 0x99, err, sizeof err) == 1 && regs_now(dbg).ix == 0x1299);
    check("an unknown register", colecodebug_register_set(dbg, "xyz", 1, err, sizeof err) == 0 && strstr(err, "unknown"));
    check("eval sees it", colecodebug_eval(dbg, "bc'", &value, err, sizeof err) == 0 && value == 0xBEEF);
    check("IM through register_set is range-checked", colecodebug_register_set(dbg, "im", 3, err, sizeof err) == 0);

    printf("while running:\n");
    colecodebug_resume(dbg);
    nap(30);
    back = regs_now(dbg);
    check("regs_set is refused", colecodebug_regs_set(dbg, &back, err, sizeof err) == 0 && strstr(err, "running"));
    check("search is refused", colecodebug_search(dbg, (const uint8_t *)"\xF3", 1, 0xFF, 0, 0, 0xFFFF,
                                                  (uint16_t[4]){ 0 }, 4) == -1);
    pause_machine(dbg);
}

static void test_trace(colecodebug *dbg)
{
    colecotrace_entry rows[40];
    colecohist_entry hist[40];
    char text[160];
    int count, idx, id;
    uint8_t loop_b;

    printf("trace and history (state before each instruction):\n");
    colecodebug_trace_enable(dbg, 1);
    colecodebug_history_enable(dbg, 1);
    check("both asked for", colecodebug_trace_enabled(dbg) && colecodebug_history_enabled(dbg));
    colecodebug_trace_clear(dbg);
    id = colecodebug_breakpoint_add(dbg, COLECODEBUG_BP_EXEC, 0x0006, 0x0006, "", NULL, 0);
    check("run to a breakpoint", go_and_wait(dbg) && regs_now(dbg).pc == 0x0006);
    colecodebug_trace_clear(dbg);
    check("and once round the loop", go_and_wait(dbg) && regs_now(dbg).pc == 0x0006);
    loop_b = regs_now(dbg).b;
    count = colecodebug_trace_read(dbg, rows, 40);
    check("records exist", count >= 10);
    check("the stop at $0006 did not record an instruction that never ran", rows[0].pc == 0x0015);
    {
        static const uint16_t k_expect[] = { 0x0015, 0x0013, 0x0011, 0x0021, 0x0020, 0x000E, 0x000B, 0x0008, 0x0007, 0x0006 };
        int same = 1;
        for (idx = 0; idx < 10; idx++)
            if (rows[idx].pc != k_expect[idx]) same = 0;
        check("newest first: the loop, in reverse", same);
    }
    check("the registers are the state BEFORE the instruction (INC B)",
          (rows[9].bc >> 8) == (uint8_t)(loop_b - 1) && (rows[8].bc >> 8) == loop_b);
    check("records carry their bytes and a length", rows[0].len == 3 && rows[0].bytes[0] == 0xC3 && rows[2].len == 2);
    check("cycles increase", rows[0].cycles > rows[9].cycles);
    check("sp is recorded", rows[5].sp == 0x7000 && rows[3].sp == 0x6FFE);
    colecodebug_trace_format(&rows[0], text, sizeof text);
    check("a trace line reads like the disassembly", strstr(text, "JP $0006") != NULL && strncmp(text, "0015", 4) == 0);
    printf("      %s\n", text);
    check("count and total", colecodebug_trace_count(dbg) >= 10 && colecodebug_trace_total(dbg) >= 10);
    check("branch filter keeps JP/CALL/RET",
          colecodebug_trace_match(&rows[0], COLECODEBUG_TRACE_BRANCHES, 0, 0) &&
          !colecodebug_trace_match(&rows[9], COLECODEBUG_TRACE_BRANCHES, 0, 0) &&
          colecodebug_trace_match(&rows[3], COLECODEBUG_TRACE_BRANCHES, 0, 0));
    check("range filter", colecodebug_trace_match(&rows[4], COLECODEBUG_TRACE_RANGE, 0x20, 0x2F) &&
                          !colecodebug_trace_match(&rows[0], COLECODEBUG_TRACE_RANGE, 0x20, 0x2F));

    count = colecodebug_history_copy(dbg, hist, 40);
    check("history is the same ring, oldest first", count >= 10 && count <= 40 && hist[count - 1].pc == 0x0015 && hist[count - 2].pc == 0x0013);
    colecodebug_history_format(&hist[count - 1], text, sizeof text);
    check("a history line has the flags", strstr(text, "JP $0006") != NULL && strstr(text, "SP=7000") != NULL);
    check("history match", colecodebug_history_match(&hist[count - 1], COLECODEBUG_TRACE_BRANCHES, 0, 0));

    colecodebug_trace_clear(dbg);
    check("clear", colecodebug_trace_count(dbg) == 0 && colecodebug_trace_total(dbg) == 0);
    colecodebug_breakpoint_remove(dbg, id);
    colecodebug_trace_enable(dbg, 0);
    colecodebug_history_enable(dbg, 0);
    check("both off", !colecodebug_trace_enabled(dbg) && !colecodebug_history_enabled(dbg));

    printf("a single step is recorded:\n");
    colecodebug_trace_enable(dbg, 1);
    colecodebug_trace_clear(dbg);
    {
        const uint16_t pc = regs_now(dbg).pc;
        const uint64_t serial = colecodebug_stop_serial(dbg);
        colecodebug_step_into(dbg);
        wait_new_stop(dbg, serial, 3000);
        count = colecodebug_trace_read(dbg, rows, 4);
        check("the stepped instruction is in the ring", count >= 1 && rows[0].pc == pc);
    }
    colecodebug_trace_enable(dbg, 0);
}

static void test_machine_getters(colecodebug *dbg)
{
    colecodebug_io io;
    colecodebug_cart cart;
    colecodebug_region map[16];
    colecodebug_sound sound;
    char text[3000];
    int count, idx, gapfree = 1;

    printf("controllers, VDP, CPU and the map:\n");
    /* up + left fire on port 1, key 5 */
    {
        uint16_t word = 0x7F7F;
        word = (uint16_t)(word & ~0x0100 & ~0x4000);
        word = (uint16_t)((word & 0xFFF0) | 5);
        coleco_host_joystick(0, word);
    }
    check("the engine reads the machine", colecodebug_io_get(dbg, &io) && io.valid);
    check("up and the left fire", io.pad[0].up && io.pad[0].fire_left && !io.pad[0].down && !io.pad[0].fire_right);
    check("key 5", io.pad[0].keypad == 5 && !strcmp(io.pad[0].keypad_name, "5"));
    check("what the console reads in each half", io.pad[0].joystick_byte == 0xBE && io.pad[0].keypad_byte == 0xF3);
    check("the idle pad has no key", io.pad[1].keypad == -1 && io.pad[1].joystick_byte == 0xFF);
    check("the strobe is a defined half", io.joy_mode == 0 || io.joy_mode == 1);
    check("VDP registers are readable", io.vdp_regs[1] == io.vdp_regs[1]);
    check("beam and frames", io.beam_line >= 0 && io.beam_line < 262 && io.frames > 0);
    check("an ADAM-less machine: no SGM", !io.sgm);
    colecodebug_io_state(dbg, text, sizeof text);
    check("the text form", strstr(text, "Controllers") && strstr(text, "key 5") && strstr(text, "VDP") &&
                           strstr(text, "Super Game Module   not fitted"));
    coleco_host_joystick(0, 0x7F7F);

    count = colecodebug_memory_map(dbg, map, 16);
    check("a memory map", count >= 4);
    for (idx = 0; idx < count; idx++) {
        const unsigned expect = idx == 0 ? 0 : (unsigned)map[idx - 1].high + 1;
        if (map[idx].low != expect || map[idx].high < map[idx].low) gapfree = 0;
    }
    check("it starts at $0000 and ends at $FFFF with no gaps", gapfree && map[0].low == 0 && map[count - 1].high == 0xFFFF);
    check("OS7, open bus, the 1K RAM and its mirror, the cartridge window",
          !strcmp(map[0].name, "OS7 BIOS ROM") && map[0].high == 0x1FFF && !map[0].writable &&
          strstr(map[1].name, "open bus") && !map[1].readable && map[2].low == 0x6000 &&
          strstr(map[2].name, "1K") && map[2].high == 0x63FF && strstr(map[3].name, "mirror") &&
          map[3].high == 0x7FFF && strstr(map[4].name, "Cartridge"));
    for (idx = 0; idx < count; idx++)
        printf("      $%04X-$%04X  %s\n", map[idx].low, map[idx].high, map[idx].name);

    colecodebug_cart_info(dbg, &cart);
    check("cart info", cart.valid && !strcmp(cart.machine, "ColecoVision") && !cart.sgm && !cart.cartridge && cart.bank == -1);
    colecodebug_cart_summary(dbg, text, sizeof text);
    check("cart summary", strstr(text, "ColecoVision") != NULL);

    check("sound getters", colecodebug_sound_get(dbg, &sound) && sound.sn.present && !sound.ay.present);
    check("the SN76489 is at its reset values: every channel muted", sound.sn.atten[0] == 15 && sound.sn.level[0] == 0);
}

static void test_search_and_memtools(colecodebug *dbg)
{
    uint16_t hits[8], found = 0;
    static const uint8_t pat[] = { 0xF3, 0x31, 0x00, 0x70 };
    static const uint8_t jp[] = { 0xC3, 0x06, 0x00 };
    char row[160];
    int count;

    printf("search, watches and symbols against the live machine:\n");
    count = colecodebug_search(dbg, pat, 4, 0xFF, 0, 0, 0xFFFF, hits, 8);
    check("find the program", count == 1 && hits[0] == 0x0000);
    check("find with a mask", colecodebug_search(dbg, jp, 1, 0xFF, 0, 0, 0x2000, hits, 8) == 1 && hits[0] == 0x0015);
    check("search next wraps", colecodebug_search_next(dbg, jp, 3, 0xFF, 0, 0x0015, 0, &found) == 1 && found == 0x0015);
    check("search backward", colecodebug_search_next(dbg, jp, 3, 0xFF, 0, 0x0016, 1, &found) == 1 && found == 0x0015);
    check("not found", colecodebug_search(dbg, (const uint8_t *)"\x12\x34\x56\x78\x9A", 5, 0xFF, 0, 0, 0xFFFF, hits, 8) == 0);

    colecowatch_clear();
    colecowatch_add("$0000");
    colecowatch_add("$0001-$0004");
    colecowatch_format(dbg, 0, row, sizeof row);
    check("a watch shows its value while stopped", strstr(row, "$F3 (243)") != NULL);
    colecowatch_format(dbg, 1, row, sizeof row);
    check("a range shows its first bytes", strstr(row, "31 00 70") != NULL);
    colecowatch_clear();

    {
        char out[600];
        colecodebug_command(dbg, "mem 0 8", out, sizeof out);
        check("mem", strstr(out, "0000: F3 31 00 70") != NULL);
        colecodebug_command(dbg, "disasm 0 3", out, sizeof out);
        check("disasm", strstr(out, "DI") != NULL && strstr(out, "LD SP,$7000") != NULL);
        colecodebug_command(dbg, "find F3 31", out, sizeof out);
        check("find", strstr(out, "$0000") != NULL);
        colecodebug_command(dbg, "poke $6100 $12 $34", out, sizeof out);
        colecodebug_command(dbg, "mem $6100 2", out, sizeof out);
        check("poke then mem", strstr(out, "6100: 12 34") != NULL);
        colecodebug_command(dbg, "map", out, sizeof out);
        check("map", strstr(out, "OS7 BIOS ROM") != NULL);
        colecodebug_command(dbg, "sound", out, sizeof out);
        check("sound", strstr(out, "SN76489") != NULL);
        colecodebug_command(dbg, "step 3", out, sizeof out);
        check("step n from the prompt", strstr(out, "$") != NULL && colecodebug_is_paused(dbg));
        colecodebug_command(dbg, "set b 7", out, sizeof out);
        check("set from the prompt", strstr(out, "AF=") != NULL && regs_now(dbg).b == 7);
        colecodebug_command(dbg, "runto $0020", out, sizeof out);
        check("runto from the prompt waits for the stop", strstr(out, "$0020") != NULL);
        colecodebug_command(dbg, "over", out, sizeof out);
        check("over from the prompt", strstr(out, "stopped at") != NULL || strstr(out, "step at") != NULL);
        colecodebug_command(dbg, "frame", out, sizeof out);
        check("frame from the prompt", strstr(out, "frame at") != NULL);
    }
}

typedef struct {
    colecodebug *dbg;
    atomic_int quit;
    atomic_uint polls;
} poller;

static void *poll_thread(void *arg)
{
    poller *ctx = arg;
    colecodebug_snapshot snap;
    colecodebug_region map[16];
    colecodebug_breakpoint list[8];
    colecodebug_line lines[8];
    adamcore_z80_regs regs;
    colecodebug_io io;
    colecodebug_sound sound;
    uint8_t bytes[64];
    char text[128];
    long value;

    while (!atomic_load(&ctx->quit)) {
        colecodebug_stop_snapshot(ctx->dbg, &snap);
        colecodebug_get_regs(ctx->dbg, &regs);
        colecodebug_read_mem(ctx->dbg, 0x6000, bytes, sizeof bytes);
        colecodebug_memory_map(ctx->dbg, map, 16);
        colecodebug_breakpoint_list(ctx->dbg, list, 8);
        colecodebug_disassemble_lines(ctx->dbg, 0, lines, 8, NULL);
        colecodebug_io_get(ctx->dbg, &io);
        colecodebug_sound_get(ctx->dbg, &sound);
        colecodebug_eval(ctx->dbg, "[hl] + sp", &value, text, sizeof text);
        colecodebug_vdp_read(ctx->dbg, 0, bytes, sizeof bytes);
        colecodebug_stop_text(ctx->dbg, text, sizeof text, NULL);
        atomic_fetch_add(&ctx->polls, 1);
    }
    return NULL;
}

static void test_session_restart(colecodebug *dbg)
{
    poller ctx;
    pthread_t thread;
    colecodebug_breakpoint list[8];
    int round, id, ok = 1;
    unsigned before_polls;

    printf("breakpoints, hooks and trace survive a session restart; polling never touches a freed core:\n");
    memset(&ctx, 0, sizeof ctx);
    ctx.dbg = dbg;
    id = colecodebug_breakpoint_add(dbg, COLECODEBUG_BP_EXEC, 0x0006, 0x0006, "", NULL, 0);
    colecodebug_breakpoint_add(dbg, COLECODEBUG_BP_WRITE, 0x6000, 0x6000, "value == 9", NULL, 0);
    colecodebug_trace_enable(dbg, 1);
    colecodebug_resume(dbg);
    pthread_create(&thread, NULL, poll_thread, &ctx);

    for (round = 0; round < 6; round++) {
        if (round % 2 == 1) {
            /* stopped (parked) when the machine is torn down: must not hang */
            if (!pause_machine(dbg)) ok = 0;
        } else {
            nap(30);
        }
        coleco_host_stop();
        if (start_machine(g_loop_bios, 0) != 0) { ok = 0; break; }
        /* the new machine has nothing paused and runs to the breakpoint */
        if (!wait_stopped(dbg, 3000)) { ok = 0; break; }
        {
            const colecodebug_snapshot snap = snap_now(dbg);
            if (snap.stop.reason != COLECODEBUG_STOP_BREAKPOINT || snap.stop.pc != 0x0006) ok = 0;
        }
    }
    check("six restarts, half of them from a parked machine, each ends at the breakpoint", ok);
    check("the table came through", colecodebug_breakpoint_list(dbg, list, 8) == 2 && list[0].id == id);
    check("the trace is still on", colecodebug_trace_enabled(dbg));
    {
        colecotrace_entry rows[4];
        colecodebug_resume(dbg);
        wait_new_stop(dbg, colecodebug_stop_serial(dbg), 3000);
        check("and recording in the new machine", colecodebug_trace_read(dbg, rows, 4) >= 1);
    }
    before_polls = atomic_load(&ctx.polls);
    nap(50);
    check("the poller kept running through it all", atomic_load(&ctx.polls) > before_polls);
    atomic_store(&ctx.quit, 1);
    pthread_join(thread, NULL);
    colecodebug_trace_enable(dbg, 0);
    colecodebug_breakpoint_clear(dbg);
    colecodebug_resume(dbg);
}

static void test_vdp_write_break(colecodebug *dbg)
{
    colecodebug_snapshot snap;
    int id;

    printf("a Break-on-writes condition on the VDP (vdpaddr, vdpsecond):\n");
    coleco_host_stop();
    colecodebug_breakpoint_clear(dbg);
    check("start the VDP program", start_machine(g_vdp_bios, 0) == 0);
    id = colecodebug_breakpoint_add(dbg, COLECODEBUG_BP_OUT, 0xA0, 0xBF,
                                    "(addr&1)==0&&vdpaddr>=0&&vdpaddr<=0x10", NULL, 0);
    check("the condition is accepted", id > 0);
    check("it stops", wait_stopped(dbg, 3000));
    snap = snap_now(dbg);
    check("on the data port with $55", snap.stop.reason == COLECODEBUG_STOP_WATCH_OUT &&
                                       snap.stop.addr == 0xA0 && snap.stop.value == 0x55);
    colecodebug_breakpoint_remove(dbg, id);
    id = colecodebug_breakpoint_add(dbg, COLECODEBUG_BP_OUT, 0xA0, 0xBF,
                                    "(addr&1)==1&&vdpsecond&&value==0x40", NULL, 0);
    check("a register-pair condition", id > 0);
    check("it stops on the second control byte", go_and_wait(dbg));
    snap = snap_now(dbg);
    check("at $A1 with $40", snap.stop.addr == 0xA1 && snap.stop.value == 0x40);
    colecodebug_breakpoint_remove(dbg, id);
}

static void test_halt_and_sound(colecodebug *dbg)
{
    colecodebug_sound sound;
    colecodebug_io io;
    colecodebug_region map[16];
    colecotrace_entry rows[200];
    char err[200];
    int count, idx, covered = 0;

    printf("a halted machine with the Super Game Module:\n");
    coleco_host_stop();
    colecodebug_trace_enable(dbg, 1);
    check("start program B", start_machine(g_halt_bios, 1) == 0);
    nap(250);
    check("pause a halted machine", pause_machine(dbg));
    check("the CPU is halted", regs_now(dbg).halted == 1);

    count = colecodebug_trace_read(dbg, rows, 200);
    check("a HALT flood collapses into a handful of records", count > 5 && count < 100);
    check("...with a spin count on the last", rows[0].halted == 1 && rows[0].repeat > 1000);
    check("...after the HALT itself ran", rows[1].bytes[0] == 0x76 && rows[1].halted == 0);

    check("sound getters", colecodebug_sound_get(dbg, &sound) == 1);
    check("tone 1 period $4E", sound.sn.period[0] == 0x4E);
    check("...and its frequency", sound.sn.freq[0] > 1434.0 && sound.sn.freq[0] < 1436.0);
    check("tone 1 loudest, tone 2 and noise muted", sound.sn.atten[0] == 0 && sound.sn.level[0] == 15 &&
                                                    sound.sn.atten[1] == 15 && sound.sn.level[1] == 0 &&
                                                    sound.sn.atten[3] == 15);
    check("noise: white, rate 1", sound.sn.noise_white && sound.sn.noise_rate == 1);
    check("AY present with the SGM", sound.ay.present);
    check("AY period $234, tone on, level 12", sound.ay.period[0] == 0x234 && sound.ay.tone_on[0] &&
                                               !sound.ay.noise_on[0] && sound.ay.level[0] == 12);
    check("AY frequency", sound.ay.freq[0] > 190.0 && sound.ay.freq[0] < 200.0);

    colecodebug_io_get(dbg, &io);
    check("the SGM is fitted and its RAM is on", io.sgm && io.sgm_ram_en && !io.sgm_bios_off);
    check("the AY register file", io.ay_regs[0] == 0x34 && io.ay_regs[1] == 0x02 && io.ay_regs[7] == 0xF8);

    count = colecodebug_memory_map(dbg, map, 16);
    for (idx = 0; idx < count; idx++)
        if (map[idx].low == 0x2000 && map[idx].high == 0x7FFF && !strcmp(map[idx].name, "SGM RAM")) covered = 1;
    check("SGM RAM is one region over $2000-$7FFF", covered);

    check("a PC edit while halted warns", colecodebug_register_set(dbg, "pc", 0x0010, err, sizeof err) == 1 &&
                                              strstr(err, "halted") != NULL);
    check("...and the CPU stays halted", regs_now(dbg).halted == 1);
    check("clearing HALT is allowed", colecodebug_register_set(dbg, "pc", 0x0010, err, sizeof err) == 1);
    {
        adamcore_z80_regs regs = regs_now(dbg);
        regs.halted = 0;
        check("regs_set clears halted", colecodebug_regs_set(dbg, &regs, err, sizeof err) == 1 && regs_now(dbg).halted == 0);
    }
    colecodebug_trace_enable(dbg, 0);
}

int main(void)
{
    colecodebug *dbg;

    build_programs();
    dbg = colecosession_debugger(NULL);
    check("the engine is created on demand", dbg != NULL);
    if (!dbg) return 1;
    if (start_machine(g_loop_bios, 0) != 0) {
        fprintf(stderr, "engine_test: could not start: %s\n", coleco_host_last_error());
        return 1;
    }
    nap(100);
    check("it starts running", !colecodebug_is_paused(dbg));
    check("pause", pause_machine(dbg));

    test_expressions(dbg);
    test_pause_and_breakpoints(dbg);
    test_watchpoints(dbg);
    test_stepping(dbg);
    test_registers(dbg);
    test_trace(dbg);
    test_machine_getters(dbg);
    test_search_and_memtools(dbg);
    test_session_restart(dbg);
    test_vdp_write_break(dbg);
    test_halt_and_sound(dbg);

    coleco_host_stop();
    if (failures) {
        fprintf(stderr, "\nengine_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("\nengine_test: PASS\n");
    return 0;
}
