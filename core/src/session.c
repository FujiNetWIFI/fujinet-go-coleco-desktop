/*
 * session.c -- the frontend contract. See core/include/colecosession.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "fujinet_cart.h"
#include "session_internal.h"

void session_set_error(struct colecosession *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_error, sizeof s->last_error, fmt, ap);
    va_end(ap);
}

/* ---- lifecycle ------------------------------------------------------------ */

colecosession *colecosession_new(const colecosession_paths *paths)
{
    struct colecosession *s = calloc(1, sizeof *s);
    if (!s) return NULL;

    pthread_mutex_init(&s->settings_mtx, NULL);
    pthread_mutex_init(&s->sysact_mtx, NULL);

    if (paths_init(s, paths ? paths->config_dir : NULL,
                   paths ? paths->data_dir : NULL) != 0) {
        colecosession_free(s);
        return NULL;
    }
    settings_init(s);
    coleco_bindings_load(s);
    coleco_input_reset(&s->panel_input);
    roms_provision_embedded(s);

    snprintf(s->boip_hostport, sizeof s->boip_hostport, "127.0.0.1:%d",
             COLECOSESSION_BOIP_PORT);
    snprintf(s->webui_url, sizeof s->webui_url, "http://127.0.0.1:%d/",
             COLECOSESSION_WEBUI_PORT);

    if (paths && paths->fujinet_lib)
        snprintf(s->fujinet_lib, sizeof s->fujinet_lib, "%s",
                 paths->fujinet_lib);
    return s;
}

void colecosession_free(colecosession *s)
{
    if (!s) return;
    colecosession_stop(s);
    colecosession_settings_flush(s);
    settings_free_all(s);
    free(s->bios);
    pthread_mutex_destroy(&s->settings_mtx);
    pthread_mutex_destroy(&s->sysact_mtx);
    free(s);
}

void colecosession_default_opts(colecosession *s, colecosession_start_opts *opts)
{
    memset(opts, 0, sizeof *opts);
    /* The Super Game Module defaults ON: it is a passive expansion, a
     * cartridge that never touches ports $50-$53 cannot tell it is there
     * (adamcore's psg_test pins that), and the titles that need it are
     * otherwise silently wrong. */
    opts->sgm = colecosession_get_int(s, "sgm", 1);
    opts->palette = colecosession_get_int(s, "palette", 0);
    opts->swap_buttons = colecosession_get_int(s, "swap_buttons", 0);
    opts->enable_fujinet = colecosession_get_int(s, "enable_fujinet", 1);
    opts->external_fujinet = colecosession_get_int(s, "fujinet_external", 0);
    opts->boip_connect = colecosession_get_str(s, "fujinet_external_addr", NULL);
    opts->enable_audio = colecosession_get_int(s, "enable_audio", 1);
    opts->enable_gamepad = colecosession_get_int(s, "enable_gamepad", 1);
    opts->cart_path = colecosession_get_str(s, "cart_path", NULL);
    if (opts->cart_path && !opts->cart_path[0])
        opts->cart_path = NULL;
}

static uint8_t *read_file(const char *path, uint32_t *size_out)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n <= 0 || n > 4 * 1024 * 1024) { fclose(f); return NULL; }
    rewind(f);
    buf = malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size_out = (uint32_t)n;
    return buf;
}

int colecosession_start(colecosession *s, const colecosession_start_opts *opts)
{
    colecosession_start_opts local;
    coleco_host_opts hopts;
    uint8_t *cart = NULL;
    uint32_t cart_size = 0;
    int rc;

    if (s->running) return 0;
    s->last_error[0] = '\0';

    if (!opts) {
        colecosession_default_opts(s, &local);
        opts = &local;
    }

    free(s->bios);
    s->bios = roms_load_bios(s);
    if (!s->bios) return -1; /* roms_load_bios set the error */

    if (opts->cart_path && opts->cart_path[0]) {
        cart = read_file(opts->cart_path, &cart_size);
        if (!cart) {
            session_set_error(s, "Cannot read cartridge %s", opts->cart_path);
            return -1;
        }
    }

    /* FujiNet FIRST, and this is the whole reason the ordering is written
     * down rather than left to chance: on this target FujiNet listens and the
     * cartridge dials in (as on CoCo, MSX and Astrocade, and unlike the ADAM
     * app where the emulator listens). A cartridge that dials out before the
     * listener exists boots reporting no link, and nothing retries it until
     * the next transaction.
     *
     * A FujiNet that fails to start is NOT fatal. The machine boots with the
     * mailbox reporting the link down, the CONFIG client says so on screen,
     * and the user can still run cartridges -- far better than refusing. */
    s->fujinet_external = 0;
    snprintf(s->boip_hostport, sizeof s->boip_hostport, "127.0.0.1:%d",
             COLECOSESSION_BOIP_PORT);
    if (opts->external_fujinet) {
        /* Someone else's FujiNet: dial the address the settings name, and
         * start nothing. A bad address is an error, not a silent fall back
         * to the built-in one. */
        char host[64];
        int port;

        if (colecosession_parse_hostport(opts->boip_connect, host, sizeof host,
                                         &port) != 0) {
            session_set_error(s, "\"%s\" is not an address:port for the "
                              "external FujiNet (example: 127.0.0.1:%d)",
                              opts->boip_connect ? opts->boip_connect : "",
                              COLECOSESSION_BOIP_PORT);
            free(cart);
            return -1;
        }
        s->fujinet_external = 1;
        snprintf(s->boip_hostport, sizeof s->boip_hostport, "%s:%d", host,
                 port);
    } else if (opts->enable_fujinet) {
        if (fujinet_start(s) == 0)
            fujinet_wait_for_boip(s, 3000);
    }

    memset(&hopts, 0, sizeof hopts);
    hopts.os7 = s->bios;
    hopts.cart = cart;
    hopts.cart_size = cart_size;
    hopts.sgm = opts->sgm;
    hopts.palette = opts->palette;
    hopts.swap_buttons = opts->swap_buttons;
    hopts.audio_rate = COLECOSESSION_AUDIO_RATE;
    /* "" means no cartridge device at all -- a bare ColecoVision. */
    hopts.fujinet_hostport = (opts->enable_fujinet || s->fujinet_external)
                                 ? s->boip_hostport : "";

    /* The debugger's FujiNet trace records from the first transaction, open
     * or not: creating the engine installs its tap before the machine runs. */
    (void)colecosession_debugger(s);

    rc = coleco_host_start(&hopts);
    free(cart);
    if (rc != 0) {
        session_set_error(s, "%s", coleco_host_last_error());
        fujinet_stop(s);
        return -1;
    }

    if (opts->enable_gamepad && gamepad_start(s) != 0) {
        /* A gamepad is a convenience; the keyboard and the on-screen panel
         * still work. Note it and carry on. */
        fprintf(stderr, "coleco: gamepads unavailable (%s)\n", s->last_error);
        s->last_error[0] = '\0';
    }

    if (opts->enable_audio && audio_start(s) != 0) {
        /* Audio is a convenience, not the machine. Note it and carry on
         * rather than refusing to run on a box with no sound device. */
        fprintf(stderr, "coleco: audio unavailable (%s); continuing silent\n",
                s->last_error);
        s->last_error[0] = '\0';
    }

    s->running = 1;
    return 0;
}

void colecosession_stop(colecosession *s)
{
    if (!s->running) return;
    audio_stop(s);
    gamepad_stop(s);
    coleco_host_stop();
    fujinet_stop(s);
    s->fujinet_external = 0;
    s->running = 0;
}

int colecosession_is_running(const colecosession *s) { return s->running; }
const char *colecosession_last_error(const colecosession *s)
{
    return s->last_error;
}

/* ---- video / audio / input ------------------------------------------------ */

int colecosession_copy_frame(colecosession *s, uint16_t *dst,
                             uint64_t *serial_inout)
{
    (void)s;
    return coleco_host_frame_copy(dst, serial_inout);
}

void colecosession_notify_vsync(colecosession *s, int64_t frame_time_ns)
{
    (void)s;
    coleco_host_notify_vsync(frame_time_ns);
}

int colecosession_render_audio(colecosession *s, int16_t *out, int nsamples)
{
    (void)s;
    return coleco_host_render_audio(out, nsamples);
}

void colecosession_joystick_raw(colecosession *s, int port, uint16_t state)
{
    (void)s;
    coleco_host_joystick(port, state);
}

void colecosession_reset(colecosession *s)
{
    (void)s;
    coleco_host_reset();
}

void colecosession_reset_to_config(colecosession *s)
{
    colecosession_start_opts o;

    if (!s->running) return;
    /* A full restart with no cartridge image, so the device serves the
     * CONFIG client again. A plain reset cannot do this: the cartridge never
     * sees the console's reset line, which is the whole point of
     * adamcore_cart_ops' power-on-only reset -- after a network boot the
     * swapped-in game is what the cartridge is serving, and only rebuilding
     * the cartridge puts CONFIG back. On hardware this is a power cycle. */
    colecosession_default_opts(s, &o);
    o.cart_path = NULL;
    colecosession_stop(s);
    if (colecosession_start(s, &o) != 0)
        fprintf(stderr, "coleco: reset to CONFIG failed: %s\n", s->last_error);
}

/* The on-screen keypad window presses controls directly rather than
 * synthesising keystrokes -- a button on screen is a button, and routing it
 * through the key table would mean it stopped working the moment someone
 * remapped the key it was pretending to be. */
void colecosession_press(colecosession *s, int port, int act, int down)
{
    coleco_input_apply(&s->panel_input, port, act, down);
    colecosession_joystick_raw(s, port,
                               coleco_input_word(&s->panel_input, port));
}

void colecosession_sysaction(colecosession *s, int sysact)
{
    switch (sysact) {
    case COLECO_SYSACT_RESET: colecosession_reset(s); break;
    case COLECO_SYSACT_RESET_CONFIG: colecosession_reset_to_config(s); break;
    default: break;
    }
}

/* ---- FujiNet -------------------------------------------------------------- */

int colecosession_fujinet_running(const colecosession *s)
{
    return s->fujinet_running;
}

const char *colecosession_fujinet_webui_url(const colecosession *s)
{
    return s->fujinet_external ? "" : s->webui_url;
}

int colecosession_parse_hostport(const char *text, char *host, int hostsz,
                              int *port)
{
    char buf[80], *colon, *end;
    const char *name = "127.0.0.1";
    long num = COLECOSESSION_BOIP_PORT;
    size_t len, i;

    if (!text) text = "";
    while (*text == ' ' || *text == '\t') text++;
    len = strlen(text);
    while (len && (text[len - 1] == ' ' || text[len - 1] == '\t' ||
                   text[len - 1] == '\n' || text[len - 1] == '\r'))
        len--;
    if (len >= sizeof buf) return -1;
    memcpy(buf, text, len);
    buf[len] = '\0';

    if (len) {
        colon = strrchr(buf, ':');
        if (colon) {
            *colon = '\0';
            name = buf;
            if (!*name) return -1;
            text = colon + 1;
        } else {
            text = buf;
        }
        if (!*text) return -1;
        num = strtol(text, &end, 10);
        if (*end || num < 1 || num > 65535) return -1;
        if (colon) {
            /* A host name or an IPv4 literal: letters, digits, '-', '_' and
             * '.'; a name of only digits and dots must be a whole address. */
            int digits_only = 1, octet[4], used = 0, k;
            for (i = 0; name[i]; i++) {
                char ch = name[i];
                int alnum = (ch >= '0' && ch <= '9') ||
                            (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
                if (!alnum && ch != '-' && ch != '_' && ch != '.') return -1;
                if (!(ch >= '0' && ch <= '9') && ch != '.') digits_only = 0;
            }
            if (name[0] == '.' || name[i - 1] == '.' || strstr(name, "..")) return -1;
            if (digits_only) {
                if (sscanf(name, "%d.%d.%d.%d%n", &octet[0], &octet[1],
                           &octet[2], &octet[3], &used) != 4 || name[used])
                    return -1;
                for (k = 0; k < 4; k++)
                    if (octet[k] < 0 || octet[k] > 255) return -1;
            }
        }
    }
    if ((int)strlen(name) >= hostsz) return -1;
    memcpy(host, name, strlen(name) + 1);
    *port = (int)num;
    return 0;
}

int colecosession_fujinet_external(const colecosession *s)
{
    return s->fujinet_external;
}

const char *colecosession_boip_endpoint(const colecosession *s)
{
    return s->boip_hostport;
}

int colecosession_cart_link_up(const colecosession *s)
{
    (void)s;
    return coleco_cart_link_up(coleco_host_cart());
}

int colecosession_cart_mailbox_live(const colecosession *s)
{
    (void)s;
    return coleco_cart_mailbox_live(coleco_host_cart());
}

/* ---- paths ---------------------------------------------------------------- */

const char *colecosession_config_path(const colecosession *s)
{
    return s->config_dir;
}
const char *colecosession_data_path(const colecosession *s)
{
    return s->data_dir;
}
const char *colecosession_roms_path(const colecosession *s)
{
    return s->roms_dir;
}
const char *colecosession_carts_path(const colecosession *s)
{
    return s->carts_dir;
}
const char *colecosession_sd_path(const colecosession *s)
{
    return s->fujinet_sd;
}
