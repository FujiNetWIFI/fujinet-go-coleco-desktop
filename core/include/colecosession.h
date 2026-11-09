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

/* The app's accent colour (the launcher icon's background): the TMS9918's
 * "dark blue", the pair of the ADAM app's light blue. Held and remap-target
 * buttons; text on it is white. */
#define COLECOSESSION_ACCENT_RGB 0x5455ED

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
    int external_fujinet;   /* 1: don't start it; dial another FujiNet
                             * instance at boip_connect instead */
    const char *boip_connect; /* "host:port" or "port" the cartridge dials
                               * (external only); NULL/"" = 127.0.0.1:11502 */
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

/* Ask to be told when a new frame is ready, for a frontend with no frame
 * clock of its own: cb(ctx) runs on the machine thread after each frame is
 * published (a frame identical to the previous one is not published again).
 * Keep it quick (post an event and return) and do not call into the session
 * from it. Passing NULL stops the calls, and returns only after any call in
 * flight has finished, so ctx may be freed right after. */
typedef void (*colecosession_frame_cb)(void *ctx);
void colecosession_set_frame_callback(colecosession *s,
                                      colecosession_frame_cb cb, void *ctx);
/* Frames the machine has run since it started, including the ones identical
 * to their predecessor (which are not published again): use it to ask "is the
 * machine running", not "is the picture changing". */
uint64_t colecosession_frame_count(colecosession *s);

/* Feed the UI's frame-clock ticks (CLOCK_MONOTONIC ns). While a steady ~60 Hz
 * stream arrives the emulator phase-locks one frame per tick; otherwise it
 * paces on the wall clock (59.92 Hz). A frontend with no frame clock simply
 * never calls this and loses nothing but the phase lock. */
void colecosession_notify_vsync(colecosession *s, int64_t frame_time_ns);

/* ---- input ----------------------------------------------------------------
 * The ColecoVision has no keyboard: all input is two hand controllers, each
 * with four directions, two fire buttons and a 12-key keypad.
 *
 * Every way of holding a control (the keyboard, the Controllers window's
 * mouse, each gamepad's buttons and its stick) is a separate source of the
 * session: letting go of one never releases another, and what the console
 * sees is the merge, handed to the machine after every change. Nothing here
 * needs the machine to be running. */

#define COLECO_PORTS       2    /* hand controllers: "Controller 1" and "2" */
#define COLECO_KEYPAD_KEYS 12   /* 0-9, then * (10) and # (11) */

/* Controller word: active-low, idle 0x7F7F. The high byte is the joystick
 * strobe (directions and the left fire), the low byte the keypad strobe (the
 * key and the right fire). keypad: 0..9, 10 = '*', 11 = '#', anything else =
 * no key. The word is NOT swapped for swap_buttons: the machine does that. */
uint16_t coleco_controller_encode(int upward, int down, int left, int right,
                                  int fire_left, int fire_right, int keypad);

typedef enum {
    COLECO_ACT_KEYPAD = 0,      /* + key 0..11 */
    COLECO_ACT_UP = COLECO_KEYPAD_KEYS,
    COLECO_ACT_DOWN,
    COLECO_ACT_LEFT,
    COLECO_ACT_RIGHT,
    COLECO_ACT_FIRE_L,          /* the left side button (joystick strobe) */
    COLECO_ACT_FIRE_R,          /* the right side button (keypad strobe) */
    COLECO_ACT_PER_PORT         /* how many actions one controller has */
} coleco_action;

/* Machine-wide actions, after both controllers' controls. They are the app's
 * to run (on the UI thread), not machine inputs. */
typedef enum {
    COLECO_SYSACT_RESET = 0,
    COLECO_SYSACT_RESET_CONFIG,
    COLECO_SYSACT_COUNT
} coleco_sysaction;

/* The flat target space the bindings and the window iterate: per controller
 * its 18 actions, then the system actions. 38 in all. */
#define COLECO_TARGET_PORT(port, act) ((port) * COLECO_ACT_PER_PORT + (act))
#define COLECO_TARGET_SYSACT(sysact) (COLECO_PORTS * COLECO_ACT_PER_PORT + (sysact))
#define COLECO_TARGET_COUNT           (COLECO_PORTS * COLECO_ACT_PER_PORT + COLECO_SYSACT_COUNT)

/* Hold or release one target (a controller action: the keyboard source), the
 * way a key does; a system action acts on press only. A bound key goes
 * through colecosession_key instead. UI thread. */
void colecosession_press(colecosession *sess, int target, int down);
/* Let go of everything the keyboard holds (focus loss). A gamepad's holds are
 * left to the gamepad, the mouse's to colecosession_pointer_set. */
void colecosession_release_all(colecosession *sess);
/* The actions held on `port` as the console sees them: a bit per
 * coleco_action. Opposed directions cancel (neither is held) and only the
 * lowest held keypad key counts, because the machine reads one. Every source
 * counts, so a window lights exactly what the game gets. */
unsigned colecosession_buttons_held(colecosession *sess, int port);
/* The Controllers window's mouse hold on `port`: `mask` is a bit per
 * coleco_action (its own source; 0 lets go of that port). */
void colecosession_pointer_set(colecosession *sess, int port, unsigned mask);
/* The word last handed to the machine for `port` (idle 0x7F7F); for tests
 * and status. */
uint16_t colecosession_controller_word(colecosession *sess, int port);

/* Restarts the machine's CPU (the console RESET). The cartridge does not see
 * it. */
void colecosession_reset(colecosession *sess);

/* Restart the machine with a freshly created cartridge, which puts the
 * FujiNet CONFIG client back on screen after a network boot has swapped a
 * game in. On real hardware that takes a power cycle -- the cartridge edge
 * carries no reset line, so the RESET button cannot reach the RP2040, and
 * the firmware watchdogs back to CONFIG when console power goes away. This
 * is the emulated equivalent, and it is a different thing from
 * colecosession_reset(), which the cartridge deliberately does not see. The
 * gamepads and what is held stay as they are. */
void colecosession_reset_to_config(colecosession *sess);

/* Run a system action (COLECO_SYSACT_*). UI thread only: Reset to CONFIG
 * stops and restarts the session. */
void colecosession_sysaction(colecosession *sess, int sysact);
/* Cross-thread: the gamepad thread cannot call into a UI toolkit, so a
 * system action it resolves is posted and a frontend's timer takes it. Posts
 * of one action coalesce; take returns the lowest pending one. */
void colecosession_sysaction_post(colecosession *sess, int sysact);
int  colecosession_sysaction_take(colecosession *sess, int *sysact);

/* ---- the Controllers window's layout ---------------------------------------
 * One layout for every toolkit: two ColecoVision hand controllers side by
 * side, each with a stick (a disc ringed by eight direction wedges), a fire
 * button on either side and the 12-key keypad below, in the card slot's
 * frame. Units: a keypad key's width is 1.0. Geometry only -- colours are the
 * frontends'. Wedges are annular sectors, so their rectangles are bounding
 * boxes: paint one as the sector of the STICK disc whose direction is its
 * act_mask, between half the disc's radius and all of it, and hit-test with
 * colecosession_controls_hit (exact shapes). */
typedef enum {
    COLECO_CTL_KEY = 0,      /* a keypad key (0-9 * #) */
    COLECO_CTL_FIRE,         /* a side fire button */
    COLECO_CTL_STICK,        /* the stick's disc: drag to tilt */
    COLECO_CTL_DIRECTION,    /* one of the eight wedges round the stick */
    COLECO_CTL_OVERLAY,      /* the keypad's overlay-card frame (drawn, never pressed) */
    COLECO_CTL_CAPTION       /* text: "Controller 1" */
} coleco_control_kind;

typedef struct {
    int kind;                /* coleco_control_kind */
    const char *label;       /* "5", "*", "Left Fire", "Controller 1" */
    const char *legend;      /* NULL (the overlay card's legends are the game's) */
    int port;                /* 0 controller 1, 1 controller 2 */
    int target;              /* COLECO_TARGET_*, or -1 (stick, diagonals, overlay, caption) */
    unsigned act_mask;       /* the coleco_action bits it drives */
    float xpos, ypos, width, height;   /* layout units, from the top-left */
} coleco_control;

/* The layout; *count is its entry count and width/height (units) its extent,
 * so a toolkit can size its drawing area. Entries are in paint order. */
const coleco_control *colecosession_controls_layout(int *count, float *width,
                                                    float *height);
/* The topmost pressable control under a point (layout units), or -1. */
int colecosession_controls_hit(float xpos, float ypos);
/* The stick's direction bits (coleco_action UP/DOWN/LEFT/RIGHT) for a pointer
 * at (delx, dely) from the disc's centre (+x right, +y toward the player, any
 * unit) with `radius` the disc's reach: nothing inside a quarter of it, else
 * one of eight 45-degree sectors. */
unsigned colecosession_stick_from_vector(float delx, float dely, float radius);
/* The keycap of keypad action `act` (0..11): "0".."9", "*", "#"; else NULL. */
const char *colecosession_keypad_label(int act);

/* ---- keyboard translation ---------------------------------------------------
 * keysym is an X11/xkb keysym (== a GDK keyval); Qt, Win32 and AppKit map
 * their native key codes through the tables below, from the key's position
 * rather than the character it types, so one bindings table serves every
 * frontend. Returns 1 if the key drives a controller target (pressed or
 * released here), 0 if it should be ignored. System actions are not consumed
 * here: ask colecosession_key_sysaction first and run it yourself, on the
 * leading edge. Ctrl is a machine key (Left Fire), so a key typed with it
 * down is passed in as usual; an unbound one returns 0. Call for every press and release; auto-repeat is ignored. */
int  colecosession_key(colecosession *sess, uint32_t keysym, int down);
/* The system action a keysym is bound to (COLECO_SYSACT_*), or -1. */
int  colecosession_key_sysaction(colecosession *sess, uint32_t keysym);

/* Non-printing keys in the keysym space the frontends translate to. */
enum {
    COLECOSESSION_KEYSYM_NONE = 0,
    COLECOSESSION_KEYSYM_UP = 0xff52, COLECOSESSION_KEYSYM_DOWN = 0xff54,
    COLECOSESSION_KEYSYM_LEFT = 0xff51, COLECOSESSION_KEYSYM_RIGHT = 0xff53,
    COLECOSESSION_KEYSYM_ESCAPE = 0xff1b, COLECOSESSION_KEYSYM_RETURN = 0xff0d,
    COLECOSESSION_KEYSYM_BACKSPACE = 0xff08, COLECOSESSION_KEYSYM_TAB = 0xff09,
    COLECOSESSION_KEYSYM_SPACE = 0x20,
    COLECOSESSION_KEYSYM_F1 = 0xffbe, COLECOSESSION_KEYSYM_F2,
    COLECOSESSION_KEYSYM_F3, COLECOSESSION_KEYSYM_F4, COLECOSESSION_KEYSYM_F5,
    COLECOSESSION_KEYSYM_F6, COLECOSESSION_KEYSYM_F7, COLECOSESSION_KEYSYM_F8,
    COLECOSESSION_KEYSYM_F9, COLECOSESSION_KEYSYM_F10,
    COLECOSESSION_KEYSYM_F11, COLECOSESSION_KEYSYM_F12,
    COLECOSESSION_KEYSYM_LSHIFT = 0xffe1, COLECOSESSION_KEYSYM_RSHIFT = 0xffe2,
    COLECOSESSION_KEYSYM_LCTRL = 0xffe3, COLECOSESSION_KEYSYM_RCTRL = 0xffe4,
    COLECOSESSION_KEYSYM_LALT = 0xffe9, COLECOSESSION_KEYSYM_RALT = 0xffea,
    COLECOSESSION_KEYSYM_KP_0 = 0xffb0, COLECOSESSION_KEYSYM_KP_1,
    COLECOSESSION_KEYSYM_KP_2, COLECOSESSION_KEYSYM_KP_3,
    COLECOSESSION_KEYSYM_KP_4, COLECOSESSION_KEYSYM_KP_5,
    COLECOSESSION_KEYSYM_KP_6, COLECOSESSION_KEYSYM_KP_7,
    COLECOSESSION_KEYSYM_KP_8, COLECOSESSION_KEYSYM_KP_9,
    COLECOSESSION_KEYSYM_KP_ENTER = 0xff8d,
    COLECOSESSION_KEYSYM_KP_MULTIPLY = 0xffaa,
    COLECOSESSION_KEYSYM_KP_DIVIDE = 0xffaf,
    COLECOSESSION_KEYSYM_KP_PERIOD = 0xffae
};
/* The HID-usage keysym band: a key with no X11 keysym of its own (a media
 * key, an international key) is still bindable through this. */
#define COLECOSESSION_KEYSYM_HID_BASE 0x10000u
#define COLECOSESSION_HID_USAGE_MAX   0xE7u
/* Native key codes for the platforms whose toolkits do not deliver
 * position-true keysyms: Windows scan code (set 1, with the E0 flag), Linux
 * evdev code (GTK/Qt keycode minus 8) and macOS virtual key code, each mapped
 * to a keysym; a key with no keysym of its own lands in the HID band
 * (0x10000 + usage) so it is still bindable. 0 when unknown. */
uint32_t colecosession_keysym_from_win_scancode(unsigned scancode, int extended);
uint32_t colecosession_keysym_from_evdev(unsigned code);
uint32_t colecosession_keysym_from_macos_keycode(unsigned keycode);
/* Name for a keysym ("F1", "Space", "A", "Keypad 5"); returns the length. */
int colecosession_keysym_name(uint32_t keysym, char *dst, int dstsz);

/* ---- remappable bindings ----------------------------------------------------
 * Every target can be driven by one keyboard key and one gamepad button.
 * Rebinding STEALS: a key drives exactly one target, because one keystroke
 * doing two things is worse than losing the old binding; a gamepad button is
 * unique within its controller (and the system actions). keysym 0 / button
 * NONE is "clear". Persisted in the settings store under "bindings" as only
 * the entries that differ from the defaults (the old per-target "bind.N" keys
 * of 0.9.0 are migrated once). Defaults: controller 1 has the digit row as
 * keypad 0-9, Keypad * and Keypad / as * and #, the arrows, Left Ctrl and
 * Left Alt as the fire buttons; controller 2 has W A S D, Left Shift and Tab;
 * F5 resets the console and F6 resets to CONFIG. Gamepad, both controllers:
 * D-pad steers (the left stick always does), South = left fire, East = right
 * fire, West = key 1, North = key 2, shoulders = * and #.
 *
 * Lookup: a keysym matches exactly (letters case-folded, Keypad Enter is
 * Return); only when nothing matches, Keypad 0-9 fall back to the digit-row
 * key of the same number, so the numeric keypad dials too. */
typedef enum {
    COLECO_PAD_BTN_NONE = -1,
    COLECO_PAD_BTN_SOUTH = 0, COLECO_PAD_BTN_EAST, COLECO_PAD_BTN_WEST,
    COLECO_PAD_BTN_NORTH, COLECO_PAD_BTN_BACK, COLECO_PAD_BTN_GUIDE,
    COLECO_PAD_BTN_START, COLECO_PAD_BTN_LEFT_STICK, COLECO_PAD_BTN_RIGHT_STICK,
    COLECO_PAD_BTN_LEFT_SHOULDER, COLECO_PAD_BTN_RIGHT_SHOULDER,
    COLECO_PAD_BTN_DPAD_UP, COLECO_PAD_BTN_DPAD_DOWN, COLECO_PAD_BTN_DPAD_LEFT,
    COLECO_PAD_BTN_DPAD_RIGHT,
    COLECO_PAD_BTN_LEFT_TRIGGER, COLECO_PAD_BTN_RIGHT_TRIGGER,
    COLECO_PAD_BTN_COUNT,
    /* Raw joystick bands for devices SDL has no gamepad mapping for (a plain
     * HID adapter): button index, and hat directions (hat*8 + dir). */
    COLECO_PAD_BTN_RAW_BASE = 64, COLECO_PAD_BTN_RAW_LAST = 127,
    COLECO_PAD_HAT_BASE = 192, COLECO_PAD_HAT_LAST = 255
} coleco_pad_button;
#define COLECO_PAD_HAT_DIRS 8
#define COLECO_PAD_BTN_IS_NAMED(btn) ((btn) >= 0 && (btn) < COLECO_PAD_BTN_COUNT)
#define COLECO_PAD_BTN_IS_RAW(btn)   ((btn) >= COLECO_PAD_BTN_RAW_BASE && (btn) <= COLECO_PAD_BTN_RAW_LAST)
#define COLECO_PAD_BTN_IS_HAT(btn)   ((btn) >= COLECO_PAD_HAT_BASE && (btn) <= COLECO_PAD_HAT_LAST)

typedef struct {
    uint32_t keysym;     /* 0 = no key */
    int      button;     /* coleco_pad_button, NONE = no gamepad button */
} coleco_binding;

const char *coleco_target_name(int target);         /* "Controller 1: Keypad 5" */
const char *coleco_target_short_name(int target);   /* "Keypad 5", "Up", "Reset Console" */
const char *coleco_pad_button_name(int button);     /* "A", "Button 3", "Hat 0 Up-Right" */
coleco_binding colecosession_binding_get(colecosession *sess, int target);
/* Bind; the previous holder(s) of the key/button, if any, are described into
 * `stolen` (may be NULL), comma-separated. keysym 0 / button NONE unbinds. */
void colecosession_binding_set_key(colecosession *sess, int target,
                                   uint32_t keysym, char *stolen, int stolensz);
void colecosession_binding_set_button(colecosession *sess, int target,
                                      int button, char *stolen, int stolensz);
/* Every binding back to its default. */
void colecosession_bindings_reset(colecosession *sess);
/* The target a keysym drives (the Keypad fallback applies), or -1; and the
 * target a gamepad button drives on `port`, or -1. */
int colecosession_target_for_key(colecosession *sess, uint32_t keysym);
int colecosession_target_for_button(colecosession *sess, int port, int button);

/* ---- audio ---------------------------------------------------------------
 * Owned by the session (SDL) when opts.enable_audio was set. A frontend that
 * wants the device itself can pull mono S16 at the configured rate instead. */
int  colecosession_render_audio(colecosession *s, int16_t *out, int nsamples);

/* ---- gamepads (SDL, hotplugged; started by colecosession_start) --------------
 * USB pads, mapped or not (a plain HID adapter is opened as a raw joystick),
 * drive the hand controllers: the first pad connected drives Controller 1, the
 * second Controller 2, unless a pad is assigned explicitly. An explicit
 * assignment is remembered (setting "pad_assign": a pad's GUID, else its name)
 * and applies again when the pad is replugged or the app restarted. Every
 * button goes through the bindings (colecosession_target_for_button); the left
 * stick, a raw joystick's first hat and the D-pad's buttons steer; the two
 * triggers are bindable buttons. Pad `idx` below is its slot in the
 * connection order, 0 .. count - 1. */
int  colecosession_gamepad_count(colecosession *sess);
int  colecosession_gamepad_name(colecosession *sess, int idx, char *dst, int dstsz);
/* port: 0 or 1 for that controller, -1 for automatic. */
void colecosession_gamepad_assign(colecosession *sess, int idx, int port);
/* The explicit port (0, 1), or -1 for automatic. */
int  colecosession_gamepad_assignment(colecosession *sess, int idx);
/* The controller the pad drives right now (0, 1), or -1 for none. */
int  colecosession_gamepad_effective_port(colecosession *sess, int idx);
/* Bumped on every pad added or removed (and carried across a restart of the
 * session), so a frontend can refresh its lists and toasts cheaply. */
unsigned colecosession_gamepad_generation(colecosession *sess);
/* The last hot-plug event as text ("Connected: 8BitDo SN30 Pro (controller 1)",
 * "Disconnected: ..."), for a toast; returns the length, 0 if none yet. */
int  colecosession_gamepad_last_event(colecosession *sess, char *dst, int dstsz);
/* Map mode: after begin(), the next gamepad button pressed on any pad is
 * reported by poll() (returns 1 and the coleco_pad_button, a raw or hat
 * number included) instead of acting. cancel() disarms. */
void colecosession_gamepad_capture_begin(colecosession *sess);
void colecosession_gamepad_capture_cancel(colecosession *sess);
int  colecosession_gamepad_capture_poll(colecosession *sess, int *button);

/* ---- FujiNet -------------------------------------------------------------*/
/* Parses the "Use external FujiNet" address field: "host:port" (host a name
 * or an IPv4 literal) or a bare "port". Empty text means the default,
 * 127.0.0.1:11502. Returns 0 and fills host/port, or -1 when the text is not
 * an address:port (nothing is written then). */
int         colecosession_parse_hostport(const char *text, char *host,
                                         int hostsz, int *port);
int         colecosession_fujinet_running(const colecosession *s);
/* 1 when the session was started to dial an external FujiNet. */
int         colecosession_fujinet_external(const colecosession *s);
/* "host:port" the cartridge dials (the running session's, or the default
 * 127.0.0.1:11502 before the first start). */
const char *colecosession_boip_endpoint(const colecosession *s);
const char *colecosession_fujinet_webui_url(const colecosession *s);
int         colecosession_fujinet_copy_log(colecosession *s, char *dst, int max);
/* Is the cartridge's link to FujiNet up, and is its mailbox decoding? */
int         colecosession_cart_link_up(const colecosession *s);
int         colecosession_cart_mailbox_live(const colecosession *s);

/* ---- ROMs and media ------------------------------------------------------
 * The BIOS is not redistributed, so a fresh install has none and the app has
 * to say so rather than showing a black screen. */
int  colecosession_bios_available(const colecosession *s);
/* Import BIOS, the way the Odyssey 2 / MS-DOS desktops do it: `path` is a
 * file, a zip (MAME's coleco.zip works) or a folder (searched 8 levels deep,
 * zips inside it too). The image is recognised by size and CRC-32 whatever it
 * is called and stored as OS7.rom. Returns how many items it imported (0 =
 * recognised nothing), or -1 with the error in `msg` and last_error; `msg`
 * always says what happened. A file of the BIOS's size with an unknown CRC is
 * taken only when it is the one file handed over, and the message warns about
 * it. Takes effect at the next power cycle. */
int  colecosession_rom_import(colecosession *s, const char *path, char *msg,
                              int msgsz);
/* 1 if `path` is a ROM this app imports (a dropped file or zip), by size and
 * known CRC (a zip: by any of its entries). Never guesses. */
int  colecosession_media_is_rom(const char *path);
/* A single file. Returns 0 imported, 1 imported but the CRC is not one I
 * know (the warning is in last_error), -1 not imported (reason in
 * last_error). */
int  colecosession_import_bios(colecosession *s, const char *src_path);

/* Routes a dropped file: a BIOS dump (colecosession_media_is_rom) to the ROM
 * directory, .rom/.col/.bin to the cartridge directory (the
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
