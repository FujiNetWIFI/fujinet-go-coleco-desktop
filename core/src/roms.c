/*
 * roms -- finding and importing the ColecoVision BIOS.
 *
 * One image, unlike the ADAM's three, and unlike fujinet-go-adam-desktop this
 * app does not redistribute it: OS-7 is copyrighted Coleco firmware (see
 * COMPLIANCE.md). A fresh install therefore has no BIOS at all, and the app's
 * job is to say so and offer an import rather than show a black screen and
 * let the user guess.
 *
 * WITH_COLECO_ROMS=ON embeds a developer's local copy; published builds are
 * built with it OFF and core/tests/no_embedded_roms.py checks that claim
 * against the shipped binaries.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "roms_embedded.h"
#include "session_internal.h"

/* The canonical filename in the user's ROM directory. Matched
 * case-insensitively on import so a file named os7.rom, OS7.ROM or
 * coleco.rom still lands correctly. */
#define BIOS_NAME "OS7.rom"

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    static int built;
    uint32_t c = 0xFFFFFFFFu;
    size_t i;

    if (!built) {
        uint32_t k, j;
        for (k = 0; k < 256; k++) {
            uint32_t v = k;
            for (j = 0; j < 8; j++)
                v = (v & 1) ? (0xEDB88320u ^ (v >> 1)) : (v >> 1);
            table[k] = v;
        }
        built = 1;
    }
    for (i = 0; i < n; i++)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* The two BIOS revisions in circulation. A file that is the right SIZE but
 * neither CRC is still accepted -- homebrew and regional variants exist, and
 * refusing them would be more annoying than useful -- but the mismatch is
 * worth reporting, because "the wrong 8K file" is otherwise indistinguishable
 * from "the emulator is broken". */
static const uint32_t known_crc[] = {
    0x3AA93EF3u, /* the common ColecoVision BIOS */
    0x39BB16FCu, /* the "Dina / Telegames" variant */
};

static void bios_path(const struct colecosession *s, char *out, int n)
{
    snprintf(out, (size_t)n, "%s/%s", s->roms_dir, BIOS_NAME);
}

static uint8_t *read_exact(const char *path, size_t want)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    size_t got;

    if (!f) return NULL;
    buf = malloc(want);
    if (!buf) { fclose(f); return NULL; }
    got = fread(buf, 1, want, f);
    /* Reject a file that is merely longer: an 8K prefix of something else is
     * not a BIOS, and accepting it would boot to a hang with no explanation. */
    if (got != want || fgetc(f) != EOF) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return buf;
}

uint8_t *roms_load_bios(struct colecosession *s)
{
    char path[COLECO_PATH_MAX];
    uint8_t *buf;

    bios_path(s, path, sizeof path);
    buf = read_exact(path, COLECOSESSION_BIOS_SIZE);
    if (buf) return buf;

    if (coleco_embedded_bios) {
        buf = malloc(COLECOSESSION_BIOS_SIZE);
        if (!buf) return NULL;
        memcpy(buf, coleco_embedded_bios, COLECOSESSION_BIOS_SIZE);
        return buf;
    }

    session_set_error(s,
        "No ColecoVision BIOS. Put an 8192-byte OS7.rom in %s, or use "
        "\"Import BIOS...\". This application does not redistribute one; see "
        "COMPLIANCE.md.", s->roms_dir);
    return NULL;
}

int roms_any_available(const struct colecosession *s)
{
    char path[COLECO_PATH_MAX];
    uint8_t *buf;

    if (coleco_embedded_bios) return 1;
    bios_path(s, path, sizeof path);
    buf = read_exact(path, COLECOSESSION_BIOS_SIZE);
    if (!buf) return 0;
    free(buf);
    return 1;
}

/* Materialise an embedded BIOS into the ROM directory on first run, so a
 * developer build behaves like a configured install and the directory is
 * never mysteriously empty. */
void roms_provision_embedded(struct colecosession *s)
{
    char path[COLECO_PATH_MAX];
    FILE *f;

    if (!coleco_embedded_bios) return;
    bios_path(s, path, sizeof path);
    f = fopen(path, "rb");
    if (f) { fclose(f); return; }   /* the user's own copy wins */
    f = fopen(path, "wb");
    if (!f) return;
    fwrite(coleco_embedded_bios, 1, COLECOSESSION_BIOS_SIZE, f);
    fclose(f);
}

int colecosession_bios_available(const colecosession *s)
{
    return roms_any_available(s);
}

int colecosession_import_bios(colecosession *s, const char *src_path)
{
    uint8_t *buf;
    char dest[COLECO_PATH_MAX];
    uint32_t crc;
    FILE *f;
    size_t i;
    int known = 0;

    buf = read_exact(src_path, COLECOSESSION_BIOS_SIZE);
    if (!buf) {
        session_set_error(s,
            "%s is not a ColecoVision BIOS: it must be exactly %d bytes.",
            src_path, COLECOSESSION_BIOS_SIZE);
        return -1;
    }

    crc = crc32_of(buf, COLECOSESSION_BIOS_SIZE);
    for (i = 0; i < sizeof known_crc / sizeof *known_crc; i++)
        if (crc == known_crc[i]) { known = 1; break; }

    bios_path(s, dest, sizeof dest);
    f = fopen(dest, "wb");
    if (!f) {
        free(buf);
        session_set_error(s, "Cannot write %s", dest);
        return -1;
    }
    fwrite(buf, 1, COLECOSESSION_BIOS_SIZE, f);
    fclose(f);
    free(buf);

    if (!known) {
        /* Imported anyway -- see the note on known_crc -- but say so. */
        session_set_error(s,
            "Imported, but this is not a BIOS revision I recognise "
            "(CRC32 %08X). If the machine misbehaves, that is the first "
            "thing to suspect.", crc);
        return 1;
    }
    return 0;
}
