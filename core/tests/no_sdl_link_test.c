/*
 * no_sdl_link_test -- coleco_core must carry no SDL dependency. It links
 * coleco_core ALONE (not coleco_session, which is where SDL lives) and calls
 * entry points from both staged trees; if any SDL symbol had crept into the
 * core archive, the link would fail. The runtime body barely matters -- the
 * linker is the test.
 *
 * It also proves the two staged trees actually compile and link together,
 * which is the whole of what M0 claims.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "adamcore.h"
#include "colmap.h"
#include "fujibus.h"

int main(void)
{
    colmap_plan_t plan;
    unsigned char img[COLMAP_WINDOW];

    /* The wire codec's own self-test: known-good encodings of a
     * GET_ADAPTERCONFIG_EXTENDED request and the bare ACK/NAK replies. This
     * is the staged firmware source checking itself, byte for byte. */
    if (!fujibus_selftest()) {
        fprintf(stderr, "no_sdl_link_test: FAIL (fujibus selftest)\n");
        return 1;
    }

    /* A flat 32K image with no claim signature must map, and must not claim
     * the mailbox -- the "load anything" path. */
    memset(img, 0xFF, sizeof img);
    if (colmap_plan(img, sizeof img, COLMAP_KIND_AUTO, &plan) != COLMAP_OK) {
        fprintf(stderr, "no_sdl_link_test: FAIL (colmap_plan on a flat 32K)\n");
        return 1;
    }
    if (plan.mailbox_ok) {
        fprintf(stderr, "no_sdl_link_test: FAIL (unsigned image claimed the "
                        "mailbox)\n");
        return 1;
    }

    /* Touch adamcore so the linker pulls that archive in too, and prove a
     * ColecoVision can actually be instantiated. A zero-filled OS7 is enough
     * -- this is a link and lifecycle check, not a boot test (that is
     * boot_smoke, which needs a real BIOS and skips without one). The core
     * takes the ROM from memory when given it, so no file is touched, and
     * boip_listen_port 0 keeps it from opening a socket. */
    {
        static const uint8_t os7_blank[ADAMCORE_OS7_ROM_SIZE];
        adamcore_config cfg;
        adamcore *c;

        memset(&cfg, 0, sizeof cfg);
        cfg.os7_rom_data = os7_blank;
        cfg.start_machine = ADAMCORE_MACHINE_CV;
        cfg.audio_rate = 44100;
        cfg.boip_listen_port = 0;

        c = adamcore_create(&cfg);
        if (!c) {
            fprintf(stderr, "no_sdl_link_test: FAIL (adamcore_create)\n");
            return 1;
        }
        adamcore_run_frame(c);
        adamcore_destroy(c);
    }

    fprintf(stderr, "no_sdl_link_test: PASS (coleco_core links with no SDL)\n");
    return 0;
}
