/*
 * audio_sdl.c -- SDL3 audio device pulling the emulator's samples. SDL routes
 * through PipeWire/Pulse on Linux and ports unchanged to the Mac/Windows
 * frontends. Only the audio subsystem is initialized here -- never SDL video,
 * which would fight the GTK/Qt display stack. Mono (the ColecoVision sound chip
 * is a single mixed channel). Modelled on the Intv port's audio_sdl.c.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <stdlib.h>

#include "host.h"
#include "session_internal.h"

typedef struct {
    SDL_AudioStream *stream;
} audio_state;

/* Pull model: the device asks for more and we hand back exactly that much.
 * There is no ring between here and the emulator -- adamcore synthesizes on
 * the caller's thread from its own timestamped PSG write queue and is
 * explicitly audio-thread safe, so this pulls straight through. That is why
 * the sibling ports' ring-trimming logic has no counterpart here. */
static void audio_cb(void *ud, SDL_AudioStream *stream, int additional_amount,
                     int total_amount)
{
    struct colecosession *s = ud;
    int16_t buf[2048];
    (void)total_amount;

    while (additional_amount > 0) {
        int want_bytes = additional_amount > (int)sizeof(buf)
                             ? (int)sizeof(buf) : additional_amount;
        int want_samples = want_bytes / 2;
        int n = colecosession_render_audio(s, buf, want_samples);
        /* pad any shortfall with silence -- an underrun should play as a gap,
         * not a buzz from stale/uninitialised buffer contents */
        if (n < want_samples)
            SDL_memset(buf + n, 0,
                       (size_t)(want_samples - n) * sizeof buf[0]);
        SDL_PutAudioStreamData(stream, buf, want_samples * 2);
        additional_amount -= want_samples * 2;
    }
}

int audio_start(struct colecosession *s)
{
    audio_state *a;
    SDL_AudioSpec spec;

    if (s->audio)
        return 0;
    /* The app owns its signals; SDL must not intercept SIGINT/SIGTERM. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        session_set_error(s, "SDL audio init failed: %s", SDL_GetError());
        return -1;
    }

    a = calloc(1, sizeof *a);
    if (!a)
        return -1;

    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq = COLECOSESSION_AUDIO_RATE;
    a->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                          &spec, audio_cb, s);
    if (!a->stream) {
        session_set_error(s, "SDL audio device open failed: %s", SDL_GetError());
        free(a);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return -1;
    }
    SDL_ResumeAudioStreamDevice(a->stream);
    s->audio = a;
    return 0;
}

void audio_stop(struct colecosession *s)
{
    audio_state *a = s->audio;
    if (!a)
        return;
    s->audio = NULL;
    SDL_DestroyAudioStream(a->stream);  /* also closes the bound device */
    free(a);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
