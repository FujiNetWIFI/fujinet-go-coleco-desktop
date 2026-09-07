/*
 * colecosession's private state. Not installed; only session.c, settings.c,
 * paths.c and roms.c include it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COLECO_SESSION_INTERNAL_H
#define COLECO_SESSION_INTERNAL_H

#include <pthread.h>
#include <stdint.h>

#include "colecosession.h"

#define COLECO_PATH_MAX 1024

typedef struct setting_kv {
    char *key;
    char *val;
    struct setting_kv *next;
} setting_kv;

struct colecosession {
    char config_dir[COLECO_PATH_MAX];
    char data_dir[COLECO_PATH_MAX];
    char roms_dir[COLECO_PATH_MAX];
    char carts_dir[COLECO_PATH_MAX];
    char settings_file[COLECO_PATH_MAX];

    setting_kv *settings;
    pthread_mutex_t settings_mtx;
    int settings_dirty;

    char boip_hostport[64];   /* "127.0.0.1:11500" -- handed to the cart */
    char last_error[256];

    /* ---- FujiNet runtime (fujinet_runtime.c) ---- */
    char fujinet_root[COLECO_PATH_MAX];    /* <data>/fujinet */
    char fujinet_config[COLECO_PATH_MAX];  /* .../fnconfig.ini */
    char fujinet_sd[COLECO_PATH_MAX];      /* .../SD */
    char fujinet_data[COLECO_PATH_MAX];    /* .../data */
    char fujinet_lib[COLECO_PATH_MAX];     /* resolved libfujinet path, "" until then */
    char webui_url[64];                   /* http://127.0.0.1:11501/ */
    int  fujinet_running;

    /* cross-thread system-action latch (see colecosession_sysaction_post) */
    pthread_mutex_t sysact_mtx;
    unsigned sysact_pending;

    /* the loaded BIOS, resolved at start from the ROM dir or the embedded
     * table; kept so a reset/reload need not re-read it */
    uint8_t *bios;            /* COLECO_BIOS_SIZE bytes, or NULL */

    /* The on-screen keypad's own held-key set, kept separate from the
     * frontend's keyboard state so a button held with the mouse and a key
     * held on the keyboard cannot clear each other. */
    coleco_input_state panel_input;

    void *audio;              /* audio_sdl.c state, NULL until started */
    void *gamepad;            /* gamepad_sdl.c state, NULL until started */
    int running;
};

void settings_init(struct colecosession *s);
void settings_free_all(struct colecosession *s);

int paths_init(struct colecosession *s, const char *config_dir,
               const char *data_dir);
/* Locate libfujinet and provision the runtime tree (fnconfig.ini + data/ +
 * SD/) into <data>/fujinet on first run. Returns 0, or -1 if no runtime is
 * available (not fatal to the session -- see fujinet_start). */
int paths_provision_fujinet(struct colecosession *s);

void session_set_error(struct colecosession *s, const char *fmt, ...);

/* fujinet_runtime.c */
int  fujinet_start(struct colecosession *s);
void fujinet_stop(struct colecosession *s);
/* Block (up to timeout_ms) until the BoIP port accepts, so the emulator's
 * first dial-out finds the listener. Returns 0 once up, -1 on timeout. */
int  fujinet_wait_for_boip(struct colecosession *s, int timeout_ms);

/* roms.c -- BIOS resolution and import. */
/* Loads the BIOS into a freshly malloc'd COLECOSESSION_BIOS_SIZE buffer:
 * first from the ROM directory, else from the embedded table
 * (WITH_COLECO_ROMS). Returns the buffer (caller frees), or NULL with the
 * error set -- which is the normal state of a fresh install, since this app
 * does not redistribute a BIOS. */
uint8_t *roms_load_bios(struct colecosession *s);
/* 1 if any known BIOS is available (ROM dir or embedded). */
int roms_any_available(const struct colecosession *s);
/* Materialise any embedded BIOS into the ROM directory on first run. */
void roms_provision_embedded(struct colecosession *s);

/* bindings.c -- the reverse lookup the key handler uses. */
int coleco_binding_target_for_key(uint32_t keysym);

/* audio_sdl.c */
int  audio_start(struct colecosession *s);
void audio_stop(struct colecosession *s);

/* gamepad_sdl.c */
int  gamepad_start(struct colecosession *s);
void gamepad_stop(struct colecosession *s);

#endif /* COLECO_SESSION_INTERNAL_H */
