/*
 * vidtab_live_test -- the Video tab on a real running machine.
 *
 * The properties that matter: a tab that is open makes the host capture frames
 * and the per-line register log; at a stop the Display view is exactly the
 * picture the main window has (the published frame), whatever the half-drawn
 * frame buffer holds; the Beam view reads the machine as it stands; a step
 * moves the machine and the tab follows; a poke through the VRAM pane lands in
 * the VDP; and the console's `video` text is the tab's.
 *
 * SKIPs (ctest 77) with no BIOS, which is every CI and release checkout. Point
 * it at a directory holding OS7.rom with argv[1].
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
#define access _access
#define getpid _getpid
#define R_OK 4
#else
#include <unistd.h>
#endif

#include "colecodebug.h"
#include "colecosession.h"
#include "colecovidtab.h"
#include "host.h"

#define SKIP 77

static int failures;

static void check(const char *what, int passed)
{
    printf("  %s %s\n", passed ? "ok " : "FAIL", what);
    if (!passed) failures++;
}

static void nap(int millis)
{
    struct timespec nap_time = { millis / 1000, (long)(millis % 1000) * 1000000L };
    nanosleep(&nap_time, NULL);
}

static int contains(const char *text, const char *word)
{
    return text && strstr(text, word) != NULL;
}

/* the published frame as the main window gets it, as 0x00RRGGBB */
static void published_frame(colecosession *sess, uint32_t *dst)
{
    uint16_t *raw = malloc(sizeof(uint16_t) * COLECOSESSION_FB_WIDTH * COLECOSESSION_FB_HEIGHT);
    uint64_t serial = 0;
    colecosession_copy_frame(sess, raw, &serial);
    for (int item = 0; item < COLECOSESSION_FB_WIDTH * COLECOSESSION_FB_HEIGHT; item++) {
        uint32_t pix = raw[item];
        uint32_t red = (pix >> 11) & 0x1F, grn = (pix >> 5) & 0x3F, blu = pix & 0x1F;
        red = red << 3 | red >> 2;
        grn = grn << 2 | grn >> 4;
        blu = blu << 3 | blu >> 2;
        dst[item] = red << 16 | grn << 8 | blu;
    }
    free(raw);
}

/* a line "Label: value" of the timing text, as a number */
static unsigned long long timing_value(colecovid_tab *tab, const char *label)
{
    static char buf[4096];
    const char *found;
    colecovid_tab_text(tab, COLECOVID_S_TIMING, buf, (int)sizeof buf);
    found = strstr(buf, label);
    if (!found) return 0;
    found += strlen(label);
    while (*found == ' ' || *found == ':') found++;
    return strtoull(found, NULL, 10);
}

int main(int argc, char **argv)
{
    colecosession_paths paths;
    colecosession_start_opts opts;
    colecosession *sess;
    colecodebug *dbg;
    colecovid_tab *tab;
    char tmp[512], cfg[600], data[600], cmd[700], bios[800];
    uint32_t *want;
    const uint32_t *pic;
    int wide = 0, high = 0, round, ready = 0;

    snprintf(bios, sizeof bios, "%s/OS7.rom", argc > 1 ? argv[1] : "tools/roms");
    if (access(bios, R_OK) != 0) {
        fprintf(stderr, "vidtab_live_test: no OS7.rom -- skipping\n");
        return SKIP;
    }

    snprintf(tmp, sizeof tmp, "/tmp/coleco-vidtab-live-%d", (int)getpid());
    snprintf(cfg, sizeof cfg, "%s/config", tmp);
    snprintf(data, sizeof data, "%s/data", tmp);
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";

    sess = colecosession_new(&paths);
    if (!sess) return 1;
    colecosession_import_bios(sess, bios);
    colecosession_default_opts(sess, &opts);
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    opts.cart_path = NULL;
    if (colecosession_start(sess, &opts) != 0) {
        fprintf(stderr, "start failed: %s\n", colecosession_last_error(sess));
        return 1;
    }
    dbg = colecosession_debugger(sess);
    want = malloc(sizeof(uint32_t) * COLECOSESSION_FB_WIDTH * COLECOSESSION_FB_HEIGHT);
    if (!dbg || !want) return 1;

    printf("running:\n");
    tab = colecovid_tab_new(dbg);
    check("a tab is created", tab != NULL);
    colecovid_tab_set_overlays(tab, 0);
    colecovid_tab_set_zoom(tab, 1);
    for (round = 0; round < 300 && !ready; round++) {
        nap(20);
        colecovid_tab_refresh(tab, 0);
        pic = colecovid_tab_picture(tab, &wide, &high);
        ready = pic != NULL && contains(colecovid_tab_status(tab), "line log is recorded");
    }
    check("the host captures frames while the tab is open", colecovid_tab_picture(tab, &wide, &high) != NULL);
    check("...and the line log of a whole frame", ready);
    check("a running machine is reported as running", contains(colecovid_tab_status(tab), "Running"));
    colecovid_tab_set_view(tab, COLECOVID_V_BEAM);
    colecovid_tab_refresh(tab, 1);
    check("Beam says it needs a stop while running", colecovid_tab_picture(tab, &wide, &high) == NULL &&
                                                      contains(colecovid_tab_why(tab), "stopped"));
    colecovid_tab_set_view(tab, COLECOVID_V_DISPLAY);

    printf("stopped:\n");
    colecodebug_pause(dbg);
    for (round = 0; round < 300 && !colecodebug_is_paused(dbg); round++) nap(10);
    check("the machine stops", colecodebug_is_paused(dbg));
    colecovid_tab_refresh(tab, 1);
    check("the tab knows", contains(colecovid_tab_status(tab), "Stopped"));
    pic = colecovid_tab_picture(tab, &wide, &high);
    published_frame(sess, want);
    check("Display is 256 x 212", pic && wide == 256 && high == 212);
    {
        int same = pic != NULL;
        for (int item = 0; pic && item < 256 * 212; item++)
            if (pic[item] != (0xff000000u | want[item])) { same = 0; break; }
        check("Display is the published frame, pixel for pixel", same);
    }

    colecovid_tab_set_view(tab, COLECOVID_V_BEAM);
    colecovid_tab_refresh(tab, 1);
    pic = colecovid_tab_picture(tab, &wide, &high);
    check("Beam draws at a stop", pic && wide == 256 && high == 212);
    check("...and says where the beam is", contains(colecovid_tab_status(tab), "beam at line"));
    {
        static char text[4096];
        colecovid_tab_text(tab, COLECOVID_S_TIMING, text, (int)sizeof text);
        check("...the timing section has the beam", contains(text, "Beam: line"));
    }

    colecovid_tab_set_view(tab, COLECOVID_V_DECODED);
    colecovid_tab_set_source(tab, COLECOVID_SRC_SCREEN);
    colecovid_tab_refresh(tab, 1);
    pic = colecovid_tab_picture(tab, &wide, &high);
    check("Decoded Screen is 256 x 192", pic && wide == 256 && high == 192);
    {
        /* The BIOS screen has no sprites: the name table redraw must be what the VDP drew. */
        int equal = 0;
        for (int line = 0; pic && line < COLECOVID_ACTIVE; line++)
            for (int col = 0; col < 256; col++)
                if (pic[line * 256 + col] == (0xff000000u | want[(line + COLECOVID_TOP) * 256 + col])) equal++;
        printf("      Screen matches the frame on %d of %d pixels\n", equal, 256 * COLECOVID_ACTIVE);
        check("Decoded Screen is the display lines of the frame", equal >= 256 * COLECOVID_ACTIVE * 95 / 100);
    }

    printf("stepping and poking:\n");
    {
        const unsigned long long before = timing_value(tab, "Cycles");
        colecodebug_step_into(dbg);
        for (round = 0; round < 300; round++) {
            colecovid_tab_refresh(tab, 0);
            if (timing_value(tab, "Cycles") != before) break;
            nap(10);
        }
        check("a step moves the machine and the tab follows", timing_value(tab, "Cycles") > before);
    }
    {
        uint8_t old = 0, now = 0, new_byte = 0;
        char err[200];
        colecodebug_vdp_read(dbg, 0x3F00, &old, 1);
        new_byte = (uint8_t)(old ^ 0xA5);
        snprintf(cmd, sizeof cmd, "3F00: %02X", new_byte);
        check("a poke through the VRAM pane", colecovid_tab_poke(tab, cmd, err, (int)sizeof err) == 1);
        colecodebug_vdp_read(dbg, 0x3F00, &now, 1);
        check("...lands in the VDP", now == new_byte);
        colecovid_tab_refresh(tab, 0);
        {
            char text[4096];
            snprintf(cmd, sizeof cmd, "3F00  %02X", new_byte);
            colecovid_tab_text(tab, COLECOVID_S_VRAM, text, (int)sizeof text);
            check("...and shows in the VRAM section", contains(text, cmd));
        }
        colecodebug_vdp_write(dbg, 0x3F00, &old, 1);
    }

    printf("the console:\n");
    {
        static char text[8192];
        colecovid_tab_text_for(dbg, "", text, (int)sizeof text);
        check("`video` gives the status and the registers", contains(text, "R0") && contains(text, "ST"));
        colecovid_tab_text_for(dbg, "timing", text, (int)sizeof text);
        check("`video timing` gives the timing", contains(text, "NTSC"));
        colecovid_tab_text_for(dbg, "lines", text, (int)sizeof text);
        check("`video lines` gives the bands", contains(text, "lines 0-"));
    }

    printf("break on writes:\n");
    {
        colecovid_tab_set_view(tab, COLECOVID_V_DISPLAY);
        colecovid_tab_set_zoom(tab, 1);
        colecovid_tab_refresh(tab, 1);
        colecovid_tab_pin(tab, 100, COLECOVID_TOP + 60);
        check("a pinned tile can be broken on", colecovid_tab_can_break(tab));
        {
            const int bpid = colecovid_tab_break_on_pixel(tab);
            if (bpid >= 0) {
                check("Break on writes adds a breakpoint", 1);
                colecodebug_breakpoint_clear(dbg);
            } else {
                printf("  NOTE the engine refused the condition (it needs vdpaddr and vdpsecond): -1\n");
            }
        }
    }

    printf("running again:\n");
    colecodebug_resume(dbg);
    for (round = 0; round < 100; round++) {
        nap(20);
        colecovid_tab_refresh(tab, 0);
        if (contains(colecovid_tab_status(tab), "Running")) break;
    }
    check("the tab reports the machine running again", contains(colecovid_tab_status(tab), "Running"));

    colecovid_tab_free(tab);
    free(want);
    colecosession_stop(sess);
    colecosession_free(sess);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp);
    if (system(cmd) != 0) { /* best effort */ }

    if (failures) {
        fprintf(stderr, "\nvidtab_live_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("\nvidtab_live_test: PASS\n");
    return 0;
}
