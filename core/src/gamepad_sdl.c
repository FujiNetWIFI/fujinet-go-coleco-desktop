/*
 * gamepad_sdl -- SDL3 gamepads as ColecoVision hand controllers.
 *
 * A polling thread rather than SDL's event queue: SDL_PollEvent must be
 * called from the thread that initialised video, and this app's video belongs
 * to GTK/Qt/AppKit/Win32, not SDL. Only the gamepad subsystem is initialised
 * here -- never SDL video, which would fight the frontend's display stack.
 *
 * Pads are assigned to ports in connection order: first pad to controller 1,
 * second to controller 2. That is what a two-player session wants and needs
 * no configuration to get right.
 *
 * The keypad is deliberately NOT mapped to face buttons. A ColecoVision
 * keypad has twelve keys and a gamepad does not have twelve spare buttons;
 * guessing a subset would make some games work and others fail for reasons
 * the user cannot see. The on-screen keypad window covers it, and covers it
 * for both ports.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session_internal.h"

#define MAX_PADS 2
/* Analog sticks are read as a direction, since the machine only has one.
 * A third of full deflection: low enough that a worn stick still registers,
 * high enough that resting drift does not. */
#define STICK_THRESHOLD 10000

typedef struct {
    SDL_Thread *thread;
    SDL_AtomicInt stop;
    struct colecosession *session;
    SDL_Gamepad *pad[MAX_PADS];
    SDL_JoystickID id[MAX_PADS];
    uint16_t last[MAX_PADS];
} gamepad_state;

static void close_all(gamepad_state *g)
{
    int i;
    for (i = 0; i < MAX_PADS; i++) {
        if (g->pad[i]) {
            SDL_CloseGamepad(g->pad[i]);
            g->pad[i] = NULL;
            g->id[i] = 0;
        }
    }
}

/* Rescan on every poll: SDL_GetGamepads is cheap, and hotplug then needs no
 * event handling of its own. A pad that disappears leaves its port idle
 * rather than stuck holding whatever it last reported. */
static void rescan(gamepad_state *g)
{
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    int slot, i;

    if (!ids) return;

    for (slot = 0; slot < MAX_PADS; slot++) {
        int still = 0;
        for (i = 0; i < count; i++)
            if (g->id[slot] && ids[i] == g->id[slot]) { still = 1; break; }
        if (g->pad[slot] && !still) {
            SDL_CloseGamepad(g->pad[slot]);
            g->pad[slot] = NULL;
            g->id[slot] = 0;
            /* Release the port, or the last direction stays held forever. */
            colecosession_joystick_raw(g->session, slot, 0x7F7F);
            g->last[slot] = 0x7F7F;
        }
    }

    slot = 0;
    for (i = 0; i < count && slot < MAX_PADS; i++) {
        int taken = 0, k;
        for (k = 0; k < MAX_PADS; k++)
            if (g->id[k] == ids[i]) taken = 1;
        if (taken) continue;
        while (slot < MAX_PADS && g->pad[slot]) slot++;
        if (slot >= MAX_PADS) break;
        g->pad[slot] = SDL_OpenGamepad(ids[i]);
        if (g->pad[slot]) g->id[slot] = ids[i];
    }
    SDL_free(ids);
}

static uint16_t read_pad(SDL_Gamepad *p)
{
    int up, down, left, right, fire_l, fire_r;
    Sint16 ax, ay;

    ax = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX);
    ay = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY);

    up    = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_UP) ||
            ay < -STICK_THRESHOLD;
    down  = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_DOWN) ||
            ay > STICK_THRESHOLD;
    left  = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_LEFT) ||
            ax < -STICK_THRESHOLD;
    right = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ||
            ax > STICK_THRESHOLD;

    /* Both diagonals of the face cluster map to each fire button, so it does
     * not matter which one a player reaches for. */
    fire_l = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_SOUTH) ||
             SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_WEST);
    fire_r = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_EAST) ||
             SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_NORTH);

    return coleco_controller_encode(up, down, left, right, fire_l, fire_r, -1);
}

static int SDLCALL poll_thread(void *ud)
{
    gamepad_state *g = ud;
    int rescan_tick = 0;

    while (!SDL_GetAtomicInt(&g->stop)) {
        int i;
        /* Rescan a few times a second, not every poll: enumerating is cheap
         * but not free, and a pad plugged in is not a 4 ms-latency event. */
        if (++rescan_tick >= 60) {
            rescan_tick = 0;
            rescan(g);
        }
        SDL_UpdateGamepads();
        for (i = 0; i < MAX_PADS; i++) {
            uint16_t w;
            if (!g->pad[i]) continue;
            w = read_pad(g->pad[i]);
            /* Only push on change: the keyboard also writes these ports, and
             * a pad re-asserting idle 60 times a second would stamp on a key
             * being held. */
            if (w != g->last[i]) {
                g->last[i] = w;
                colecosession_joystick_raw(g->session, i, w);
            }
        }
        SDL_Delay(4);
    }
    return 0;
}

int gamepad_start(struct colecosession *s)
{
    gamepad_state *g;
    int i;

    if (s->gamepad) return 0;
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        session_set_error(s, "SDL gamepad init failed: %s", SDL_GetError());
        return -1;
    }
    g = calloc(1, sizeof *g);
    if (!g) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return -1;
    }
    g->session = s;
    for (i = 0; i < MAX_PADS; i++) g->last[i] = 0x7F7F;
    SDL_SetAtomicInt(&g->stop, 0);
    rescan(g);

    g->thread = SDL_CreateThread(poll_thread, "coleco-gamepad", g);
    if (!g->thread) {
        session_set_error(s, "could not start the gamepad thread: %s",
                          SDL_GetError());
        close_all(g);
        free(g);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return -1;
    }
    s->gamepad = g;
    return 0;
}

void gamepad_stop(struct colecosession *s)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    s->gamepad = NULL;
    SDL_SetAtomicInt(&g->stop, 1);
    SDL_WaitThread(g->thread, NULL);
    close_all(g);
    free(g);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

int colecosession_gamepad_count(const colecosession *s)
{
    gamepad_state *g = s->gamepad;
    int i, n = 0;
    if (!g) return 0;
    for (i = 0; i < MAX_PADS; i++)
        if (g->pad[i]) n++;
    return n;
}

int colecosession_gamepad_name(const colecosession *s, int port, char *dst,
                               int dstsz)
{
    gamepad_state *g = s->gamepad;
    const char *name;

    if (!g || port < 0 || port >= MAX_PADS || !g->pad[port]) {
        if (dstsz > 0) dst[0] = '\0';
        return 0;
    }
    name = SDL_GetGamepadName(g->pad[port]);
    if (!name) name = "Gamepad";
    snprintf(dst, (size_t)dstsz, "%s", name);
    return (int)strlen(dst);
}
