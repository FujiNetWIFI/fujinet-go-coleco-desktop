/*
 * debug_test -- the debugger engine against a real running machine.
 *
 * The interesting property is not that a breakpoint fires; it is that
 * engaging the debugger does not change what the machine does. adamcore's
 * own debug_step test pins the two frame loops bit-identical at the core
 * level; this checks the layer above it -- that pausing, stepping and
 * breaking drive the SAME core the frontend is watching, and that letting go
 * puts the machine back on the fast path.
 *
 * SKIPs (ctest 77) with no BIOS, like boot_smoke.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
/* MSVCRT spells these with a leading underscore. */
#define access _access
#define getpid _getpid
#define R_OK 4
#define W_OK 2
#else
#include <unistd.h>
#endif

#include "colecodebug.h"
#include "colecosession.h"

#define SKIP 77

static int failures;

static void check(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok " : "FAIL", what);
    if (!ok) failures++;
}

static void nap(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
    colecosession_paths paths;
    colecosession_start_opts opts;
    colecosession *s;
    colecodebug *d;
    char tmp[512], cfg[600], data[600], cmd[700], bios[800];
    adamcore_z80_regs r1, r2;
    int i;

    snprintf(bios, sizeof bios, "%s/OS7.rom", argc > 1 ? argv[1] : "tools/roms");
    if (access(bios, R_OK) != 0) {
        fprintf(stderr, "debug_test: no OS7.rom -- skipping\n");
        return SKIP;
    }

    snprintf(tmp, sizeof tmp, "/tmp/coleco-debug-test-%d", (int)getpid());
    snprintf(cfg, sizeof cfg, "%s/config", tmp);
    snprintf(data, sizeof data, "%s/data", tmp);
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";

    s = colecosession_new(&paths);
    if (!s) return 1;
    colecosession_import_bios(s, bios);

    colecosession_default_opts(s, &opts);
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    opts.cart_path = NULL;
    if (colecosession_start(s, &opts) != 0) {
        fprintf(stderr, "start failed: %s\n", colecosession_last_error(s));
        return 1;
    }
    nap(100);

    d = colecosession_debugger(s);
    check("the engine is created on demand", d != NULL);
    if (!d) return 1;

    printf("pause and resume:\n");
    check("it starts running", !colecodebug_is_paused(d));
    colecodebug_pause(d);
    for (i = 0; i < 300 && !colecodebug_is_paused(d); i++) nap(10);
    check("pause takes hold", colecodebug_is_paused(d));

    colecodebug_get_regs(d, &r1);
    nap(80);
    colecodebug_get_regs(d, &r2);
    check("a paused machine really is stopped (PC unchanged)",
          r1.pc == r2.pc && r1.cycles == r2.cycles);

    printf("stepping:\n");
    colecodebug_step_into(d);
    for (i = 0; i < 300; i++) {
        colecodebug_get_regs(d, &r2);
        if (r2.cycles != r1.cycles) break;
        nap(10);
    }
    check("a step advances the machine", r2.cycles != r1.cycles);
    check("...and it is paused again afterwards", colecodebug_is_paused(d));

    printf("memory and disassembly:\n");
    {
        uint8_t buf[16], again[16];
        colecodasm_line lines[4];
        int n;

        check("memory reads", colecodebug_read_mem(d, 0x0000, buf, 16) == 16);
        check("...repeatably (a read must not perturb anything)",
              colecodebug_read_mem(d, 0x0000, again, 16) == 16 &&
              memcmp(buf, again, 16) == 0);

        n = colecodebug_disassemble(d, 0x0000, 4, lines);
        check("the BIOS disassembles", n == 4);
        check("...to non-empty text", lines[0].text[0] != '\0');
        check("...with sane lengths",
              lines[0].len >= 1 && lines[0].len <= 4);
        printf("      %04X  %s\n", lines[0].addr, lines[0].text);
        printf("      %04X  %s\n", lines[1].addr, lines[1].text);
    }

    printf("symbols:\n");
    {
        uint16_t addr = 0;
        check("the built-in OS7 table loaded",
              colecodebug_symbol_find(d, "MODE_1", &addr) ||
              colecodebug_symbol_find(d, "START", &addr) ||
              colecodebug_symbol_find(d, "PUT_VRAM", &addr));
    }

    printf("breakpoints:\n");
    {
        uint16_t list[8];
        colecodebug_bp_set(d, 0x1234);
        check("a breakpoint is remembered", colecodebug_bp_is_set(d, 0x1234));
        check("...and listed", colecodebug_bp_list(d, list, 8) == 1 &&
                               list[0] == 0x1234);
        colecodebug_bp_clear(d, 0x1234);
        check("...and cleared", !colecodebug_bp_is_set(d, 0x1234));
    }

    printf("letting go:\n");
    colecodebug_resume(d);
    for (i = 0; i < 300 && colecodebug_is_paused(d); i++) nap(10);
    check("resume takes hold", !colecodebug_is_paused(d));
    {
        /* And the machine is genuinely running again, on the fast path. */
        uint16_t *fb = malloc(sizeof(uint16_t) * COLECOSESSION_FB_WIDTH *
                              COLECOSESSION_FB_HEIGHT);
        uint64_t serial = 0;
        int frames = 0;
        /* Deliberately a low bar over a long window. The claim is "the
         * machine is running again", not "it hits 60 fps" -- boot_smoke owns
         * the rate. A loaded CI box can be slow, and a rate assertion here
         * would fail for a reason that has nothing to do with the
         * debugger. */
        for (i = 0; i < 200 && frames < 5; i++) {
            if (colecosession_copy_frame(s, fb, &serial)) frames++;
            nap(10);
        }
        check("frames flow again after resume", frames >= 5);
        printf("      %d frames after resume\n", frames);
        free(fb);
    }

    colecosession_stop(s);
    colecosession_free(s);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp);
    if (system(cmd) != 0) { /* best effort */ }

    if (failures) {
        fprintf(stderr, "\ndebug_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("\ndebug_test: PASS\n");
    return 0;
}
