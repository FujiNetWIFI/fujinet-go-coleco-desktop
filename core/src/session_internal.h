/*
 * colecosession's private state. Not installed; the session's own modules
 * (session, settings, paths, roms, input, bindings, gamepad_sdl) and the tests
 * that drive them without a machine include it.
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

    char boip_hostport[80];   /* "127.0.0.1:11502" -- handed to the cart */
    char last_error[256];

    /* ---- FujiNet runtime (fujinet_runtime.c) ---- */
    char fujinet_root[COLECO_PATH_MAX];    /* <data>/fujinet */
    char fujinet_config[COLECO_PATH_MAX];  /* .../fnconfig.ini */
    char fujinet_sd[COLECO_PATH_MAX];      /* .../SD */
    char fujinet_data[COLECO_PATH_MAX];    /* .../data */
    char fujinet_lib[COLECO_PATH_MAX];     /* resolved libfujinet path, "" until then */
    char webui_url[64];                   /* http://127.0.0.1:11501/ */
    int  fujinet_running;
    int  fujinet_external;                /* dialing a FujiNet that isn't ours */

    /* cross-thread system-action latch (see colecosession_sysaction_post) */
    pthread_mutex_t sysact_mtx;
    unsigned sysact_pending;

    /* the loaded BIOS, resolved at start from the ROM dir or the embedded
     * table; kept so a reset/reload need not re-read it */
    uint8_t *bios;            /* COLECO_BIOS_SIZE bytes, or NULL */

    /* What drives each controller action, one bit per source (SRC_*), so
     * letting go of one source never releases another's. input_mtx covers
     * these, held_keysym, host_word and the calls that hand the merged word
     * to the machine; it is a leaf lock. */
    pthread_mutex_t input_mtx;
    uint32_t pad_src[COLECO_PORTS][COLECO_ACT_PER_PORT];
    /* keys the keyboard currently holds, by target (the keysym it was pressed
     * as), so a release clears exactly what its press asserted even if the
     * binding changed meanwhile */
    uint32_t held_keysym[COLECO_TARGET_COUNT];
    uint16_t host_word[COLECO_PORTS];   /* what was last pushed to the machine */
    int keep_input;           /* a power cycle in progress: input_clear is skipped */

    void *audio;              /* audio_sdl.c state, NULL until started */
    void *gamepad;            /* gamepad_sdl.c state, NULL until started */
    int running;

    /* the pad generation, carried across gamepad_stop/start so a restart of
     * the session never replays an old toast */
    unsigned pad_gen_base;
    /* a pad thread has run before: its first scan finds the same pads again,
     * which is no hot-plug (no toast, no generation change) */
    int pad_restarted;
    /* the last gamepad hot-plug event, for a frontend toast */
    pthread_mutex_t pad_event_mtx;
    char pad_event[128];
};

void settings_init(struct colecosession *s);
void settings_free_all(struct colecosession *s);

/* mkdir -p; 0 on success or if it exists. */
int paths_mkdir_p(const char *path);
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

/* Who is holding an input down (the bits of pad_src): the keyboard, the
 * window's mouse, and for each gamepad (by the stable owner number its slot
 * was given, 0 to COLECO_MAX_PADS - 1) its buttons and its stick / hat
 * separately. So letting go of one never releases another, even two pads or
 * two buttons bound to the one key. */
#define COLECO_MAX_PADS 8
#define SRC_KEYBOARD 1u
#define SRC_PAD(owner)   (1u << (1 + (owner)))
#define SRC_STICK(owner) (1u << (1 + COLECO_MAX_PADS + (owner)))
#define SRC_POINTER  (1u << (1 + 2 * COLECO_MAX_PADS))

/* bindings.c: seeds the table (defaults, then the "bindings" setting, with
 * the 0.9.0 per-target "bind.N" keys migrated once). bindings_shutdown lets go
 * of the session before it is freed. */
void bindings_init(struct colecosession *sess);
void bindings_shutdown(struct colecosession *sess);
/* The targets a (case-folded) keysym is bound to, the Keypad fallback applied
 * when none is; returns how many were written to `out` (at most `max`). */
int  bindings_targets_for_key(uint32_t keysym, int *out, int max);
uint32_t bindings_fold_key(uint32_t keysym);

/* input.c */
void input_init(struct colecosession *sess);
/* Let go of every source and tell the machine (a start). */
void input_clear(struct colecosession *sess);
/* A power cycle made a new machine (idle 0x7F7F): hand it what is held. */
void input_after_power_cycle(struct colecosession *sess);
/* The keyboard asserts one target as the key `pressed` (ignoring a repeat) /
 * releases everything that key asserted, returning how many it let go. */
void input_key_hold(struct colecosession *sess, int target, uint32_t pressed);
int  input_key_release(struct colecosession *sess, uint32_t pressed);

/* audio_sdl.c */
int  audio_start(struct colecosession *s);
void audio_stop(struct colecosession *s);

/* gamepad_sdl.c */
int  gamepad_start(struct colecosession *sess);
void gamepad_stop(struct colecosession *sess);
/* Called by the gamepad thread for the pad that owns `owner`. press: a pad
 * button drives a controller target -- the pad's SRC_PAD source. stick: an
 * analog stick or hat drives a direction -- its SRC_STICK source. */
void session_gamepad_press(struct colecosession *sess, int owner, int target, int down);
void session_gamepad_stick(struct colecosession *sess, int owner, int port, int act, int down);
/* Called by the gamepad thread on a connect/disconnect. */
void session_gamepad_event(struct colecosession *sess, const char *text);

#endif /* COLECO_SESSION_INTERNAL_H */
