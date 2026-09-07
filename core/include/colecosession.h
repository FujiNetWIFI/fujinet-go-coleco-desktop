/*
 * colecosession -- the toolkit-agnostic desktop session for FujiNet Go
 * ColecoVision.
 *
 * Owns the adamcore emulator loop (its own paced thread), the FujiNet
 * cartridge and its link, the SDL audio and gamepad backends, the in-process
 * FujiNet runtime, the shared settings store, and the media/ROM path layout.
 * Frontends (GTK4, Qt6, AppKit, Win32) drive this API and do only windowing,
 * painting and event translation. A frontend that needs something which is
 * not one of those three things belongs here instead.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COLECOSESSION_H
#define COLECOSESSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COLECOSESSION_FB_WIDTH  256
#define COLECOSESSION_FB_HEIGHT 212

/* FujiNet's BoIP listener and its web admin UI. High ports of this app's own
 * so a standalone fujinet-pc, or a sibling FujiNet Go app, never collides:
 * ADAM uses 65216/65214, Apple II 1985/8000, CoCo 65504, MSX 65505/64003,
 * Intellivision 65503/64003, Astrocade 11500/11501, and pico/coleco's MAME
 * dev harness uses 9995.
 *
 * Note the direction, which is the opposite of the ADAM app's: FujiNet
 * LISTENS and the emulator's cartridge dials in, as on CoCo, MSX and
 * Astrocade. That decides startup ordering -- see colecosession_start. */
#define COLECOSESSION_BOIP_PORT  11502
#define COLECOSESSION_WEBUI_PORT 11503

/* The ColecoVision BIOS. One image, unlike the ADAM's three, and unlike the
 * ADAM app this one is not redistributed -- see COMPLIANCE.md. */
#define COLECOSESSION_BIOS_SIZE 0x2000

/* The host audio rate. adamcore resamples its PSG timeline to whatever it is
 * told, so this is a device preference rather than a machine property. */
#define COLECOSESSION_AUDIO_RATE 44100

typedef struct colecosession colecosession;
typedef struct colecodebug colecodebug;

/* All members optional (NULL = default).
 *  config_dir:  default $XDG_CONFIG_HOME/fujinet-go-coleco
 *  data_dir:    default $XDG_DATA_HOME/fujinet-go-coleco
 *  fujinet_lib: path to libfujinet.so/.dylib/.dll; default searches
 *               $FUJINET_LIB, the executable's directory, the install
 *               libdir, then tools/fujinet/work/out. "" disables FujiNet.
 *  fujinet_runtime_src: directory holding the pristine fnconfig.ini + data/
 *               + SD/ used to provision the user's runtime tree on first
 *               start (a macOS app passes its bundle's Resources/fujinet). */
typedef struct {
    const char *config_dir;
    const char *data_dir;
    const char *fujinet_lib;
    const char *fujinet_runtime_src;
} colecosession_paths;

colecosession *colecosession_new(const colecosession_paths *paths);
void colecosession_free(colecosession *s);

/* ---- settings (shared INI; one store for every frontend of this target) --- */
int         colecosession_get_int(colecosession *s, const char *key, int def);
void        colecosession_set_int(colecosession *s, const char *key, int value);
const char *colecosession_get_str(colecosession *s, const char *key,
                                  const char *def);
void        colecosession_set_str(colecosession *s, const char *key,
                                  const char *value);
void        colecosession_settings_flush(colecosession *s);

/* ---- lifecycle ------------------------------------------------------------ */
typedef struct {
    const char *cart_path;  /* cartridge image; NULL boots the CONFIG client */
    int sgm;                /* fit an Opcode Super Game Module */
    int palette;            /* 0..3 */
    int swap_buttons;       /* 0/1 */
    int enable_fujinet;     /* start the in-process FujiNet runtime */
    int enable_audio;       /* open the SDL audio device */
    int enable_gamepad;     /* start the SDL gamepad thread */
} colecosession_start_opts;

/* Fills opts from the settings store. */
void colecosession_default_opts(colecosession *s, colecosession_start_opts *opts);

/* Starts FujiNet (if enabled) and then the emulator.
 *
 * The order is not arbitrary and is the opposite of the ADAM app's: here
 * FujiNet listens and the cartridge dials in, so the listener has to exist
 * before the machine's first transaction or the client boots reporting no
 * link. start() brings FujiNet up first and waits briefly for the port.
 *
 * Returns 0, or -1 with colecosession_last_error() set. FujiNet failing to
 * start is NOT fatal: the machine boots with the mailbox reporting the link
 * down, which is far more useful than refusing to run. */
int  colecosession_start(colecosession *s, const colecosession_start_opts *opts);
void colecosession_stop(colecosession *s);
int  colecosession_is_running(const colecosession *s);
const char *colecosession_last_error(const colecosession *s);

/* ---- video ---------------------------------------------------------------
 * copy_frame copies the latest frame into dst (FB_WIDTH*HEIGHT uint16 RGB565)
 * iff its serial differs from *serial_inout, updates it, and returns 1;
 * returns 0 when unchanged, leaving dst alone. Pass 0 to force a copy (e.g.
 * the first paint after a window map). */
int  colecosession_copy_frame(colecosession *s, uint16_t *dst,
                              uint64_t *serial_inout);

/* Feed the UI's frame-clock ticks (CLOCK_MONOTONIC ns). While a steady ~60 Hz
 * stream arrives the emulator phase-locks one frame per tick; otherwise it
 * paces on the wall clock (59.92 Hz). A frontend with no frame clock simply
 * never calls this and loses nothing but the phase lock. */
void colecosession_notify_vsync(colecosession *s, int64_t frame_time_ns);

/* ---- input ----------------------------------------------------------------
 * The ColecoVision has no keyboard: all input is two hand controllers, each
 * with four directions, two fire buttons and a 12-key keypad. */

/* Controller word: active-low, idle 0x7F7F. keypad: 0..9, 10 = '*',
 * 11 = '#', negative = no key. */
uint16_t coleco_controller_encode(int up, int down, int left, int right,
                                  int fire_left, int fire_right, int keypad);

void colecosession_joystick_raw(colecosession *s, int port, uint16_t state);

/* Translate a desktop key event into a controller word for `port`, returning
 * 1 if the key is bound and writing the new state, else 0. keysym is an
 * X11/xkb keysym (== a GDK keyval; Qt and Win32 map through a small table),
 * so one table serves every frontend. `down` is press/release.
 *
 * A pure function over an explicit held-key set so it can be unit-tested on
 * its own, which is where the sibling ports' input bugs were actually
 * caught. */
typedef struct {
    uint8_t dir[2];      /* per port: bit0 up, 1 right, 2 down, 3 left */
    uint8_t fire[2];     /* per port: bit0 left fire, bit1 right fire */
    int8_t  keypad[2];   /* per port: 0..11, or -1 */
} coleco_input_state;

void coleco_input_reset(coleco_input_state *st);
/* Apply one control directly, the way the on-screen keypad window does. */
void coleco_input_apply(coleco_input_state *st, int port, int act, int down);
/* The system action a key is bound to, or -1. Frontends handle these
 * themselves; they are not controller state. */
int  coleco_input_key_sysaction(uint32_t keysym);
/* Returns 1 if the keysym is bound to something, 0 if it should be ignored. */
int  coleco_input_key(coleco_input_state *st, uint32_t keysym, int down);
uint16_t coleco_input_word(const coleco_input_state *st, int port);

void colecosession_reset(colecosession *s);

/* Restart the machine with a freshly created cartridge, which puts the
 * FujiNet CONFIG client back on screen after a network boot has swapped a
 * game in. On real hardware that takes a power cycle -- the cartridge edge
 * carries no reset line, so the RESET button cannot reach the RP2040, and
 * the firmware watchdogs back to CONFIG when console power goes away. This
 * is the emulated equivalent, and it is a different thing from
 * colecosession_reset(), which the cartridge deliberately does not see. */
void colecosession_reset_to_config(colecosession *s);

/* ---- remappable bindings --------------------------------------------------
 * Every control the keypad window shows can be rebound to a different
 * keyboard key. Targets are a flat index so the window can iterate them and
 * the settings store can name them.
 *
 * The default table is what input_test pins, so the pure mapping stays
 * testable: bindings start as the defaults and only diverge when a user
 * remaps something. */

#define COLECO_KEYPAD_KEYS 12   /* 0-9, then * (10) and # (11) */

typedef enum {
    COLECO_ACT_KEYPAD = 0,      /* + key 0..11 */
    COLECO_ACT_UP = COLECO_KEYPAD_KEYS,
    COLECO_ACT_DOWN,
    COLECO_ACT_LEFT,
    COLECO_ACT_RIGHT,
    COLECO_ACT_FIRE_L,
    COLECO_ACT_FIRE_R,
    COLECO_ACT_PER_PORT         /* how many actions one controller has */
} coleco_action;

/* Machine-wide actions, after both ports' controls. */
typedef enum {
    COLECO_SYSACT_RESET = 0,
    COLECO_SYSACT_RESET_CONFIG,
    COLECO_SYSACT_COUNT
} coleco_sysaction;

#define COLECO_TARGET_PORT(port, act) ((port) * COLECO_ACT_PER_PORT + (act))
#define COLECO_TARGET_SYSACT(sa)      (2 * COLECO_ACT_PER_PORT + (sa))
#define COLECO_TARGET_COUNT           (2 * COLECO_ACT_PER_PORT + COLECO_SYSACT_COUNT)

/* Human-readable, for the keypad window's Map row and for settings keys. */
const char *coleco_binding_label(int target);
const char *coleco_binding_key_name(int target);   /* "" when unbound */

/* Load from (or seed) the settings store. Called by colecosession_new. */
void coleco_bindings_load(colecosession *s);
/* Bind `target` to `keysym`, stealing it from whatever held it -- one key
 * cannot drive two controls, and silently ending up with a key that does two
 * things is worse than losing the old binding. keysym 0 unbinds. */
void coleco_binding_set(colecosession *s, int target, uint32_t keysym);
void coleco_bindings_reset_defaults(colecosession *s);

/* The keypad window presses controls directly, bypassing the key table.
 * `port` 0/1, `act` a coleco_action, `down` press/release. */
void colecosession_press(colecosession *s, int port, int act, int down);
void colecosession_sysaction(colecosession *s, int sysact);

/* ---- audio ---------------------------------------------------------------
 * Owned by the session (SDL) when opts.enable_audio was set. A frontend that
 * wants the device itself can pull mono S16 at the configured rate instead. */
int  colecosession_render_audio(colecosession *s, int16_t *out, int nsamples);

/* ---- gamepads (SDL, hotplugged; started by colecosession_start) ---------
 * Pads are assigned to ports in connection order. The keypad is deliberately
 * NOT mapped to face buttons: a ColecoVision keypad has twelve keys and a
 * gamepad has nowhere near twelve spare buttons, so guessing a subset would
 * make some games work and others fail invisibly. The on-screen keypad
 * window covers it, for both ports. */
int colecosession_gamepad_count(const colecosession *s);
/* Name of the pad on `port` into dst; returns length, or 0 if none. */
int colecosession_gamepad_name(const colecosession *s, int port, char *dst,
                               int dstsz);
/* The controller word the gamepad layer last pushed to `port` (idle 0x7F7F
 * when it has pushed nothing). For status displays, and for testing the pad
 * layer at all -- the words it pushes are otherwise invisible, since the
 * machine merges them with the keyboard's. */
uint16_t colecosession_gamepad_last_state(const colecosession *s, int port);

/* ---- FujiNet -------------------------------------------------------------*/
int         colecosession_fujinet_running(const colecosession *s);
const char *colecosession_fujinet_webui_url(const colecosession *s);
int         colecosession_fujinet_copy_log(colecosession *s, char *dst, int max);
/* Is the cartridge's link to FujiNet up, and is its mailbox decoding? */
int         colecosession_cart_link_up(const colecosession *s);
int         colecosession_cart_mailbox_live(const colecosession *s);

/* ---- ROMs and media ------------------------------------------------------
 * The BIOS is not redistributed, so a fresh install has none and the app has
 * to say so rather than showing a black screen. */
int  colecosession_bios_available(const colecosession *s);
/* Copies src into the ROM directory as OS7.rom after checking its size and
 * CRC32. Returns 0, or -1 with the error set. */
int  colecosession_import_bios(colecosession *s, const char *src_path);

/* Routes a dropped file: .rom/.col/.bin to the cartridge directory (the
 * returned path is usable as cart_path), disk images to the FujiNet SD
 * folder. Returns 0 and writes the destination into dest_out. */
int  colecosession_import_media(colecosession *s, const char *src_path,
                                char *dest_out, int dest_sz);
/* 1 if this path looks like a cartridge, so a frontend can decide whether an
 * import should also become the running cartridge. */
int  colecosession_media_is_cartridge(const char *path);

const char *colecosession_config_path(const colecosession *s);
const char *colecosession_data_path(const colecosession *s);
const char *colecosession_roms_path(const colecosession *s);
const char *colecosession_carts_path(const colecosession *s);
const char *colecosession_sd_path(const colecosession *s);

/* ---- debugger ------------------------------------------------------------*/
colecodebug *colecosession_debugger(colecosession *s);

#ifdef __cplusplus
}
#endif

#endif /* COLECOSESSION_H */
