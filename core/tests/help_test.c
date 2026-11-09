/*
 * help_test -- the Help covers what the app has: every Preferences setting
 * and choice, every machine, every ROM, every default key and gamepad button,
 * the app's shortcuts (and the Mac's), the two ports, the debugger's tabs
 * and prompt commands, and every context a Help button opens. Every miss is
 * listed before it fails.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "colecohelp.h"
#include "colecosession.h"

#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } } while (0)

static const unsigned platforms[] = {
    COLECOHELP_GNOME, COLECOHELP_KDE, COLECOHELP_WINDOWS, COLECOHELP_MACOS
};
static const char *const platform_names[] = { "GNOME", "KDE", "Windows", "macOS" };

/* What the frontends' Help buttons and menus open. */
static const char *const contexts[] = {
    "main", "nobios", "controllers", "fujilog", "prefs/Machine", "prefs/Input", "prefs/ROMs",
    "debugger/cpu", "debugger/vdp", "debugger/trace", "debugger/fujinet",
};

/* The Preferences pages: what each one has, as the frontends label it. */
static const struct {
    const char *context;
    const char *labels[13];
} pages[] = {
    { "preferences", { "Super Game Module", "Palette", "Default (TMS9928A)", "Palette 2", "Palette 3", "Palette 4",
                       "Enable FujiNet", "Use external FujiNet", "Address:port", "Audio", NULL } },
    { "prefs/Input", { "Enable gamepads", "Swap controller buttons", "Gamepads", "Automatic", "Controller 1",
                       "Controller 2", "Reset Controls", "Open Controllers", "remembered between runs", NULL } },
    { "preferences", { "Import BIOS", "Import from a Folder", "ROM folder", "ColecoVision BIOS (OS7.rom)", NULL } },
};

/* The settings keys the pages write. */
static const char *const settings_keys[] = {
    "sgm", "palette", "swap_buttons", "enable_fujinet", "fujinet_external", "fujinet_external_addr",
    "enable_audio", "enable_gamepad", "tv_aspect", "smooth", "cart_path", "bindings", "pad_assign",
    "controllers_show_bindings",
};

/* The app's shortcuts, as a PC and as a Mac shows them. */
static const struct { const char *pc, *mac; } shortcuts[] = {
    { "Ctrl+,", "⌘," }, { "Ctrl+Shift+H", "⌘?" }, { "F9", "F9" }, { "F12", "F12" },
};

static int misses;

/* Lower case, every run of white space one space. */
static void
fold(const char *in, char *out, size_t outsz)
{
    size_t n = 0;
    int space = 0;
    for (; *in && n + 1 < outsz; in++) {
        unsigned char c = (unsigned char) *in;
        if (c == ' ' || c == '\n' || c == '\t') {
            space = 1;
            continue;
        }
        if (space && n)
            out[n++] = ' ';
        space = 0;
        out[n++] = (char) ((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    out[n] = 0;
}

static char section[1 << 18];
static char raw[1 << 18];

/* The folded text of a context's (or slug's) section on platform p; raw
 * keeps the licoleco. */
static const char *
section_text(int p, const char *context)
{
    int t, b;
    section[0] = 0;
    raw[0] = 0;
    if (colecohelp_find(context, &t, &b) == 0) {
        colecohelp_plain_text(platforms[p], t, b, raw, sizeof raw);
        fold(raw, section, sizeof section);
    }
    return section;
}

static void
require(int p, const char *context, const char *what, const char *text)
{
    char want[512];
    fold(text, want, sizeof want);
    if (!strstr(section_text(p, context), want)) {
        fprintf(stderr, "MISSING (%s) %s: %s \"%s\"\n", platform_names[p], context, what, text);
        misses++;
    }
}

/* A line of the section (a table row: cells joined by " | ") with every one
 * of these, folded, in that order. */
static void
require_row(int p, const char *context, const char *a, const char *b, const char *c)
{
    char line[1024], fa[256], fb[256], fc[256];
    const char *s, *e;
    int ok = 0;

    section_text(p, context);
    fold(a, fa, sizeof fa);
    fold(b ? b : "", fb, sizeof fb);
    fold(c ? c : "", fc, sizeof fc);
    for (s = raw; *s && !ok; s = *e ? e + 1 : e) {
        char *x, *y;
        size_t n;
        e = strchr(s, '\n');
        if (!e)
            e = s + strlen(s);
        n = (size_t) (e - s) < sizeof line - 1 ? (size_t) (e - s) : sizeof line - 1;
        memcpy(line, s, n);
        line[n] = 0;
        fold(line, line, sizeof line);
        x = strstr(line, fa);
        if (!x)
            continue;
        y = b && *fb ? strstr(x + strlen(fa), fb) : x + strlen(fa);
        if (!y)
            continue;
        if (c && *fc && !strstr(y + (b && *fb ? strlen(fb) : 0), fc))
            continue;
        ok = 1;
    }
    if (!ok) {
        fprintf(stderr, "MISSING (%s) %s: a row \"%s\" | \"%s\" | \"%s\"\n", platform_names[p], context,
                a, b ? b : "", c ? c : "");
        misses++;
    }
}

static void
check_prefs(int p)
{
    for (size_t k = 0; k < sizeof pages / sizeof *pages; k++)
        for (int i = 0; pages[k].labels[i]; i++)
            require(p, pages[k].context, "label", pages[k].labels[i]);

    for (size_t k = 0; k < sizeof settings_keys / sizeof *settings_keys; k++)
        require(p, "preferences", "setting", settings_keys[k]);
}

/* The default keys' rows: the table's first cell and the action it names. */
static const struct { const char *row; int act; } act_rows[] = {
    { "Up", COLECO_ACT_UP }, { "Down", COLECO_ACT_DOWN }, { "Left", COLECO_ACT_LEFT },
    { "Right", COLECO_ACT_RIGHT }, { "Left fire", COLECO_ACT_FIRE_L }, { "Right fire", COLECO_ACT_FIRE_R },
};

/* The gamepad table's rows: the button, the action it drives and what the
 * Help calls it. */
static const struct { const char *row; int button; int act; const char *does; } pad_rows[] = {
    { "South (A)", COLECO_PAD_BTN_SOUTH, COLECO_ACT_FIRE_L, "Left fire" },
    { "East (B)", COLECO_PAD_BTN_EAST, COLECO_ACT_FIRE_R, "Right fire" },
    { "West (X)", COLECO_PAD_BTN_WEST, COLECO_ACT_KEYPAD + 1, "Key 1" },
    { "North (Y)", COLECO_PAD_BTN_NORTH, COLECO_ACT_KEYPAD + 2, "Key 2" },
    { "Left shoulder", COLECO_PAD_BTN_LEFT_SHOULDER, COLECO_ACT_KEYPAD + 10, "Key *" },
    { "Right shoulder", COLECO_PAD_BTN_RIGHT_SHOULDER, COLECO_ACT_KEYPAD + 11, "Key #" },
};

static void
key_name(int target, char *dst, int dstsz)
{
    coleco_binding bind = colecosession_binding_get(NULL, target);
    colecosession_keysym_name(bind.keysym, dst, dstsz);
}

/* Every default key and gamepad button, the console's buttons and the
 * shortcuts. */
static void
check_controls(int plat)
{
    char name1[64], name2[64];

    CHECK(COLECO_TARGET_COUNT == 38, "the Help says 38 controls");
    require(plat, "changing-the-controls", "count", "38 in all");
    for (size_t idx = 0; idx < sizeof act_rows / sizeof *act_rows; idx++) {
        key_name(COLECO_TARGET_PORT(0, act_rows[idx].act), name1, sizeof name1);
        key_name(COLECO_TARGET_PORT(1, act_rows[idx].act), name2, sizeof name2);
        require_row(plat, "default-keys", act_rows[idx].row, name1, name2);
    }
    /* controller 1 has the digit row and the numeric keypad's * and /;
     * controller 2 has no keypad at all */
    for (int key = 0; key < COLECO_KEYPAD_KEYS; key++) {
        coleco_binding bind = colecosession_binding_get(NULL, COLECO_TARGET_PORT(0, COLECO_ACT_KEYPAD + key));
        CHECK(key > 9 || bind.keysym == (uint32_t) ('0' + key), "controller 1 key %d is the digit row", key);
        CHECK(colecosession_binding_get(NULL, COLECO_TARGET_PORT(1, COLECO_ACT_KEYPAD + key)).keysym == 0,
              "controller 2 has no keypad keys");
    }
    key_name(COLECO_TARGET_PORT(0, COLECO_ACT_KEYPAD + 0), name1, sizeof name1);
    key_name(COLECO_TARGET_PORT(0, COLECO_ACT_KEYPAD + 9), name2, sizeof name2);
    require_row(plat, "default-keys", "Keys 0 to 9", name1, name2);
    key_name(COLECO_TARGET_PORT(0, COLECO_ACT_KEYPAD + 10), name1, sizeof name1);
    require_row(plat, "default-keys", "Key *", name1, "none");
    key_name(COLECO_TARGET_PORT(0, COLECO_ACT_KEYPAD + 11), name1, sizeof name1);
    require_row(plat, "default-keys", "Key #", name1, "none");
    key_name(COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET), name1, sizeof name1);
    require_row(plat, "default-keys", coleco_target_short_name(COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET)), name1, NULL);
    key_name(COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET_CONFIG), name1, sizeof name1);
    require_row(plat, "default-keys", coleco_target_short_name(COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET_CONFIG)),
                name1, NULL);
    require(plat, "default-keys", "numeric keypad fallback", "numeric keypad");
    for (size_t idx = 0; idx < sizeof shortcuts / sizeof *shortcuts; idx++)
        require(plat, "default-keys", "shortcut", platforms[plat] == COLECOHELP_MACOS ? shortcuts[idx].mac : shortcuts[idx].pc);

    /* the gamepad's rows: the same default on both controllers */
    require_row(plat, "gamepads", "D-pad", "the stick", NULL);
    for (size_t idx = 0; idx < sizeof pad_rows / sizeof *pad_rows; idx++) {
        for (int port = 0; port < COLECO_PORTS; port++)
            CHECK(colecosession_binding_get(NULL, COLECO_TARGET_PORT(port, pad_rows[idx].act)).button
                  == pad_rows[idx].button, "%s drives %s on controller %d", pad_rows[idx].row, pad_rows[idx].does,
                  port + 1);
        require_row(plat, "gamepads", pad_rows[idx].row, pad_rows[idx].does, NULL);
    }
    require(plat, "gamepads", "remembered", "remembered between runs");
    require(plat, "gamepads", "assignment choices", "Controller 2");
    require(plat, "swapping-the-fire-buttons", "where", "Preferences ▸ Input");
}

/* The Controllers window's topic covers what the layout draws and every
 * control it has. */
static void
check_controllers(int plat)
{
    static const char *const words[] = {
        "Map", "Clear", "Defaults", "Show bindings", "Reset Console", "Reset to CONFIG", "Automatic",
        "Controller 1", "Controller 2", "accent colour", "fixed size", "controllers_show_bindings",
    };
    static const struct { int kind; const char *word; } kinds[] = {
        { COLECO_CTL_KEY, "keypad" }, { COLECO_CTL_FIRE, "fire button" }, { COLECO_CTL_STICK, "stick" },
        { COLECO_CTL_DIRECTION, "wedge" }, { COLECO_CTL_OVERLAY, "overlay" },
    };
    const coleco_control *layout;
    int count;
    float width, height;

    for (size_t idx = 0; idx < sizeof words / sizeof *words; idx++)
        require(plat, "controllers", "word", words[idx]);
    layout = colecosession_controls_layout(&count, &width, &height);
    for (size_t idx = 0; idx < sizeof kinds / sizeof *kinds; idx++) {
        int seen = 0;
        for (int ctl = 0; ctl < count && !seen; ctl++)
            seen = layout[ctl].kind == kinds[idx].kind;
        CHECK(seen, "the layout has no %s", kinds[idx].word);
        require(plat, "controllers", "part", kinds[idx].word);
    }
    require(plat, "controllers", "keypad keys", "1 to 9, *, 0 and #");
    require(plat, "changing-the-controls", "Done", "Done");
    require(plat, "changing-the-controls", "Escape", "Escape");
}

/* The two ports, in the FujiNet topic. */
static void
check_ports(int p)
{
    char port[16];

    snprintf(port, sizeof port, "%d", COLECOSESSION_BOIP_PORT);
    require(p, "fujinet", "BoIP port", port);
    snprintf(port, sizeof port, "%d", COLECOSESSION_WEBUI_PORT);
    require(p, "fujinet", "web UI port", port);
    require(p, "an-external-fujinet", "default address", "127.0.0.1:11502");
}

/* The debugger's buttons and its tabs. */
static void
check_debugger(int p)
{
    static const char *const words[] = { "Pause or Continue", "Step Into", "Step Over", "Step Out", "Trace",
                                         "four tabs", "Clear all", "Bank 0" };
    for (size_t k = 0; k < sizeof words / sizeof *words; k++)
        require(p, "debugger", "button", words[k]);
}

int
main(void)
{
    colecohelp_hit hits[16];
    int t, b;

    CHECK(colecohelp_topic_count() > 0, "no topics");

    for (size_t k = 0; k < sizeof contexts / sizeof *contexts; k++)
        if (colecohelp_find(contexts[k], &t, &b) != 0) {
            fprintf(stderr, "MISSING context %s\n", contexts[k]);
            misses++;
        }
    for (int plat = 0; plat < 4; plat++) {
        check_prefs(plat);
        check_controls(plat);
        check_controllers(plat);
        check_ports(plat);
        check_debugger(plat);
    }

    /* lookup and search */
    CHECK(colecohelp_find("#preferences", &t, &b) == 0 && b == -1, "#preferences");
    CHECK(colecohelp_find("#the-settings-file", &t, &b) == 0 && b >= 0, "#the-settings-file");
    CHECK(colecohelp_search(COLECOHELP_GNOME, "cartridge", hits, 16) > 0, "search cartridge");
    CHECK(colecohelp_search(COLECOHELP_GNOME, "zzqxj", hits, 16) == 0, "search nonsense");
    int n = colecohelp_search(COLECOHELP_KDE, "sprites", hits, 16);
    CHECK(n > 0 && colecohelp_find("debugger/vdp", &t, &b) == 0
          && hits[0].topic == t && hits[0].block == b, "sprites is the first hit");
    for (int i = 1; i < n; i++)
        CHECK(hits[i - 1].score >= hits[i].score, "hits best first");

    /* the Mac sees its own keys */
    {
        char text[4096];
        CHECK(colecohelp_find("default-keys", &t, &b) == 0, "default-keys");
        colecohelp_plain_text(COLECOHELP_MACOS, t, b, text, sizeof text);
        CHECK(strstr(text, "⌘,") != NULL, "the Mac's Preferences key");
        colecohelp_plain_text(COLECOHELP_GNOME, t, b, text, sizeof text);
        CHECK(strstr(text, "Ctrl+,") != NULL && !strstr(text, "⌘,"), "the PC's");
    }

    CHECK(misses == 0, "%d missing from data/help/help.md", misses);
    printf("help ok: %d topics\n", colecohelp_topic_count());
    return 0;
}
