/*
 * End-to-end test of the gamepad backend using an SDL3 virtual gamepad:
 * verifies hotplug pickup, the button/axis mapping, the encoded controller
 * words the backend pushes toward the core, and hot-unplug -- with no
 * hardware and no human.
 *
 * core/tests/gamepad_probe.c covers the same ground but needs someone to sit
 * there pressing buttons, so it is deliberately not a ctest. This is the half
 * that can run in CI, which is the half that catches a regression.
 *
 * The expected words come straight from coleco_controller_encode(): idle is
 * $7F7F and every control is ACTIVE LOW, so pressing something clears a bit.
 *   up $0100  right $0200  down $0400  left $0800  fireL $4000  fireR $0040
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "session_internal.h"

static int failures;
static int vport;   /* the port the virtual pad landed on */

static int wait_for(colecosession *s, uint16_t want, const char *what)
{
    /* Device sync runs at ~4Hz, state polls at ~125Hz. */
    int i;
    for (i = 0; i < 100; i++) {
        if (colecosession_gamepad_last_state(s, vport) == want)
            return 1;
        usleep(20 * 1000);
    }
    fprintf(stderr, "FAIL %s: last_state %04X want %04X\n", what,
            colecosession_gamepad_last_state(s, vport), want);
    failures++;
    return 0;
}

int main(void)
{
    char tmpdir[512];
    colecosession_paths paths;
    colecosession *s;
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID vid;
    SDL_Joystick *vj;
    int i, baseline;

    snprintf(tmpdir, sizeof tmpdir, "/tmp/coleco-gamepad-test-%d", (int)getpid());
    memset(&paths, 0, sizeof paths);
    paths.config_dir = tmpdir;
    paths.data_dir = tmpdir;
    paths.fujinet_lib = "";   /* no FujiNet: this is about the pad only */

    s = colecosession_new(&paths);
    if (!s) return 1;
    if (gamepad_start(s) != 0) {
        fprintf(stderr, "gamepad_map: SDL gamepad init failed (headless "
                        "CI?); skipping\n");
        return 77;
    }

    /* Real controllers may already be attached. Let the backend's first
     * device scan (about 4Hz) find them before taking the baseline, so the
     * virtual pad is identified by the slot it ADDS rather than assumed to
     * be port 0. */
    usleep(700 * 1000);
    baseline = colecosession_gamepad_count(s);

    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "colecotest virtual pad";
    vid = SDL_AttachVirtualJoystick(&desc);
    if (!vid) {
        fprintf(stderr, "gamepad_map: virtual joystick unavailable: %s; "
                        "skipping\n", SDL_GetError());
        gamepad_stop(s);
        return 77;
    }
    vj = SDL_OpenJoystick(vid);
    if (!vj) return 1;

    /* Wait for the backend's device sync to open it. */
    for (i = 0; i < 100 && colecosession_gamepad_count(s) <= baseline; i++)
        usleep(20 * 1000);

    vport = -1;
    for (i = 0; i < colecosession_gamepad_count(s); i++) {
        char name[64];
        colecosession_gamepad_name(s, i, name, sizeof name);
        printf("gamepad_map: pad %d = \"%s\"\n", i, name);
        if (strcmp(name, "colecotest virtual pad") == 0)
            vport = i;
    }
    if (vport < 0) {
        fprintf(stderr, "gamepad_map: backend never saw the virtual pad\n");
        gamepad_stop(s);
        colecosession_free(s);
        return 1;
    }
    if (vport >= 2) {
        /* Only two controller ports exist; a machine with two real pads
         * already plugged in leaves nowhere for the virtual one. */
        fprintf(stderr, "gamepad_map: virtual pad landed on port %d (real "
                        "pads attached?); skipping\n", vport);
        gamepad_stop(s);
        colecosession_free(s);
        return 77;
    }

    /* South (A) and West (X) both mean left fire, so it does not matter
     * which of the two a player reaches for. */
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
    wait_for(s, 0x3F7F, "south=fireL");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, false);
    wait_for(s, 0x7F7F, "release");

    /* East (B) and North (Y) both mean right fire. */
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, true);
    wait_for(s, 0x7F3F, "east=fireR");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, false);
    wait_for(s, 0x7F7F, "release");

    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_NORTH, true);
    wait_for(s, 0x7F3F, "north=fireR too");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_NORTH, false);
    wait_for(s, 0x7F7F, "release");

    /* D-pad. */
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    wait_for(s, 0x7E7F, "dpad up");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
    wait_for(s, 0x777F, "dpad left");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, false);
    wait_for(s, 0x7F7F, "release");

    /* The analogue stick past the direction threshold, plus a fire button.
     * This is the path that matters most in practice: the pad used during
     * bring-up reported its d-pad AS AXES, so a mapping that only handled
     * the hat buttons would have looked fine and driven nothing. */
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 20000);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, 20000);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_WEST, true);
    wait_for(s, 0x397F, "stick right+down + west=fireL");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, 0);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_WEST, false);
    wait_for(s, 0x7F7F, "release all");

    /* Hot-unplug: the count returns to the baseline, and the port is
     * RELEASED rather than left holding whatever was last pressed. */
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    wait_for(s, 0x7D7F, "dpad right before unplug");
    SDL_CloseJoystick(vj);
    SDL_DetachVirtualJoystick(vid);
    for (i = 0; i < 100 && colecosession_gamepad_count(s) != baseline; i++)
        usleep(20 * 1000);
    if (colecosession_gamepad_count(s) != baseline) {
        fprintf(stderr, "gamepad_map: backend kept the detached pad "
                        "(count %d, baseline %d)\n",
                colecosession_gamepad_count(s), baseline);
        failures++;
    }
    if (colecosession_gamepad_last_state(s, vport) != 0x7F7F) {
        fprintf(stderr, "gamepad_map: port %d still held %04X after unplug "
                        "-- the last direction would stick forever\n",
                vport, colecosession_gamepad_last_state(s, vport));
        failures++;
    }

    gamepad_stop(s);
    colecosession_free(s);

    if (failures) {
        fprintf(stderr, "gamepad_map: %d failure(s)\n", failures);
        return 1;
    }
    printf("gamepad_map: mapping, axes, hotplug and unplug-release ok\n");
    return 0;
}
