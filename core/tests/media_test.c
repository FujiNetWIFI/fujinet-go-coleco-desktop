/*
 * media_test -- where a dropped file goes, and whether a real cartridge
 * actually runs once it gets there.
 *
 * The routing matters because getting it wrong is silent: a disk image
 * copied into the cartridge directory looks imported and then fails to boot,
 * with nothing to suggest the copy was the mistake.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include "colecosession.h"

static int failures;

static void check(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok " : "FAIL", what);
    if (!ok) failures++;
}

static void write_file(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(data, 1, n, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    colecosession_paths paths;
    colecosession *s;
    char tmp[512], cfg[600], data[600], cmd[700];
    char src[700], dest[1024];
    static uint8_t cart[0x8000];
    const char *romdir = argc > 1 ? argv[1] : NULL;

    snprintf(tmp, sizeof tmp, "/tmp/coleco-media-test-%d", (int)getpid());
    snprintf(cfg, sizeof cfg, "%s/config", tmp);
    snprintf(data, sizeof data, "%s/data", tmp);
    if (mkdir(tmp, 0700) != 0 && access(tmp, W_OK) != 0) return 1;

    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";

    s = colecosession_new(&paths);
    if (!s) { fprintf(stderr, "colecosession_new failed\n"); return 1; }

    printf("classification:\n");
    check(".col is a cartridge", colecosession_media_is_cartridge("/x/game.col"));
    check(".rom is a cartridge", colecosession_media_is_cartridge("/x/game.rom"));
    check(".COL uppercase too", colecosession_media_is_cartridge("/x/GAME.COL"));
    check(".bin is a cartridge here",
          colecosession_media_is_cartridge("/x/game.bin"));
    check(".dsk is not", !colecosession_media_is_cartridge("/x/disk.dsk"));
    check("no extension is not", !colecosession_media_is_cartridge("/x/game"));

    printf("routing:\n");
    /* A minimal but valid ColecoVision cartridge: the $AA55 header the BIOS
     * looks for, so this is a real image rather than noise. */
    memset(cart, 0xFF, sizeof cart);
    cart[0] = 0xAA;
    cart[1] = 0x55;
    snprintf(src, sizeof src, "%s/probe.col", tmp);
    write_file(src, cart, sizeof cart);

    check("a cartridge imports",
          colecosession_import_media(s, src, dest, sizeof dest) == 0);
    check("...into the cartridge directory",
          strstr(dest, colecosession_carts_path(s)) == dest);
    check("...keeping its name", strstr(dest, "probe.col") != NULL);
    check("...and is byte-identical", access(dest, R_OK) == 0);

    /* A disk image with no FujiNet runtime must FAIL with an explanation,
     * not land in the cartridge directory. */
    snprintf(src, sizeof src, "%s/probe.dsk", tmp);
    write_file(src, cart, 1024);
    check("a disk image with no FujiNet runtime is refused",
          colecosession_import_media(s, src, dest, sizeof dest) != 0);
    check("...and says why", colecosession_last_error(s)[0] != '\0');
    printf("      (\"%s\")\n", colecosession_last_error(s));

    snprintf(src, sizeof src, "%s/mystery.xyz", tmp);
    write_file(src, cart, 16);
    check("an unknown type is refused",
          colecosession_import_media(s, src, dest, sizeof dest) != 0);

    /* With a BIOS, run the imported cartridge for real -- through the
     * FujiNet cartridge device, which is what serves every image here. */
    if (romdir) {
        char biossrc[800];
        snprintf(biossrc, sizeof biossrc, "%s/OS7.rom", romdir);
        if (access(biossrc, R_OK) == 0) {
            printf("running the imported cartridge:\n");
            colecosession_import_bios(s, biossrc);
            snprintf(src, sizeof src, "%s/probe.col", tmp);
            colecosession_import_media(s, src, dest, sizeof dest);
            {
                colecosession_start_opts o;
                uint16_t *fb;
                uint64_t serial = 0;
                int tries = 0, got = 0;

                colecosession_default_opts(s, &o);
                o.cart_path = dest;
                o.enable_fujinet = 0;
                o.enable_audio = 0;
                o.enable_gamepad = 0;
                check("the session starts with the cartridge",
                      colecosession_start(s, &o) == 0);
                fb = malloc(sizeof(uint16_t) * COLECOSESSION_FB_WIDTH *
                            COLECOSESSION_FB_HEIGHT);
                while (tries++ < 200 && !got) {
                    struct timespec ts = { 0, 10000000 };
                    got = colecosession_copy_frame(s, fb, &serial);
                    if (!got) nanosleep(&ts, NULL);
                }
                check("a frame is produced", got);
                /* An unsigned image runs as a plain cartridge: the mailbox
                 * must be dead, which is the "load anything" path. */
                check("the mailbox is dead for an unsigned image",
                      !colecosession_cart_mailbox_live(s));
                free(fb);
                colecosession_stop(s);
            }
        }
    }

    colecosession_free(s);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp);
    if (system(cmd) != 0) { /* best effort */ }

    if (failures) {
        fprintf(stderr, "\nmedia_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("\nmedia_test: PASS\n");
    return 0;
}
