/*
 * power_cycle_test -- what survives a power cycle. Reset to CONFIG restarts
 * the machine (and the cartridge, FujiNet and audio) but not the user's hands:
 * a key held across it stays held and the new machine is handed it, the
 * Controllers window's mouse hold too, and the gamepad layer is NOT restarted
 * (so no pad "reconnects", no toast replays and the generation holds). A plain
 * stop and start, on the other hand, begins with nothing held.
 *
 * Needs a real BIOS (argv[1]/OS7.rom): SKIPs (77) without one, as on CI,
 * where this repository's checkouts have none.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "colecosession.h"
#include "test_tmpdir.h"

static int failures;
static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

int main(int argc, char **argv)
{
    char cfg[512], data[512], src[700], text[96];
    colecosession_paths paths;
    colecosession *sess;
    colecosession_start_opts opts;
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID vid;
    unsigned gen;
    int tries, pads;

    if (argc < 2) return 77;
    snprintf(src, sizeof src, "%s/OS7.rom", argv[1]);
    if (access(src, R_OK) != 0) {
        printf("SKIP: no OS7.rom in %s\n", argv[1]);
        return 77;
    }
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) return 77;

    test_tmpdir(cfg, sizeof cfg, "pccfg");
    test_tmpdir(data, sizeof data, "pcdat");
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";
    sess = colecosession_new(&paths);
    if (!sess) return 1;
    colecosession_set_int(sess, "enable_fujinet", 0);
    colecosession_set_int(sess, "enable_audio", 0);
    colecosession_set_int(sess, "enable_gamepad", 1);
    colecosession_set_str(sess, "cart_path", "");
    if (colecosession_import_bios(sess, src) < 0) { printf("FAIL: import\n"); return 1; }

    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "colecotest cycle pad";
    vid = SDL_AttachVirtualJoystick(&desc);

    colecosession_default_opts(sess, &opts);
    check(colecosession_start(sess, &opts) == 0, "the session starts");
    for (tries = 0; tries < 250 && vid && colecosession_gamepad_count(sess) == 0; tries++) {
        struct timespec stamp = { 0, 20000000 };
        nanosleep(&stamp, NULL);
    }
    pads = colecosession_gamepad_count(sess);
    gen = colecosession_gamepad_generation(sess);
    colecosession_gamepad_last_event(sess, text, sizeof text);

    colecosession_key(sess, COLECOSESSION_KEYSYM_UP, 1);
    colecosession_pointer_set(sess, 1, 1u << (COLECO_ACT_KEYPAD + 4));
    check(colecosession_controller_word(sess, 0) == 0x7E7F && colecosession_controller_word(sess, 1) == 0x7F74,
          "a key and a mouse hold are handed to the machine");

    colecosession_reset_to_config(sess);
    check(colecosession_is_running(sess), "Reset to CONFIG restarts the machine");
    check(colecosession_buttons_held(sess, 0) == (1u << COLECO_ACT_UP)
          && colecosession_buttons_held(sess, 1) == (1u << (COLECO_ACT_KEYPAD + 4)),
          "what was held across it is still held");
    check(colecosession_controller_word(sess, 0) == 0x7E7F && colecosession_controller_word(sess, 1) == 0x7F74,
          "and the new machine was handed it");
    check(colecosession_gamepad_count(sess) == pads && colecosession_gamepad_generation(sess) == gen,
          "the gamepad layer was not restarted: same pads, same generation");
    {
        char again[96];
        colecosession_gamepad_last_event(sess, again, sizeof again);
        check(strcmp(again, text) == 0, "and no new toast");
    }
    colecosession_key(sess, COLECOSESSION_KEYSYM_UP, 0);
    colecosession_pointer_set(sess, 1, 0);
    check(colecosession_controller_word(sess, 0) == 0x7F7F && colecosession_controller_word(sess, 1) == 0x7F7F,
          "letting go after the cycle works");

    /* a plain stop and start begins with nothing held, and carries the pad
     * generation on */
    colecosession_key(sess, COLECOSESSION_KEYSYM_UP, 1);
    colecosession_stop(sess);
    check(!colecosession_is_running(sess), "stopped");
    check(colecosession_gamepad_generation(sess) == gen, "the generation is kept while stopped");
    colecosession_default_opts(sess, &opts);
    check(colecosession_start(sess, &opts) == 0, "started again");
    check(colecosession_buttons_held(sess, 0) == 0 && colecosession_controller_word(sess, 0) == 0x7F7F,
          "a new start has nothing held");
    check(colecosession_gamepad_generation(sess) >= gen, "and the generation never goes back");

    colecosession_free(sess);
    if (vid) SDL_DetachVirtualJoystick(vid);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
