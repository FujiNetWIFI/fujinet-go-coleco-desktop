/*
 * roms -- finding and importing the ColecoVision BIOS.
 *
 * One image, unlike the ADAM's three, and unlike fujinet-go-adam-desktop this
 * app does not redistribute it: OS-7 is copyrighted Coleco firmware (see
 * COMPLIANCE.md). A fresh install therefore has no BIOS at all, and the app's
 * job is to say so and offer an import rather than show a black screen and
 * let the user guess.
 *
 * "Import BIOS..." works the way fujinet-go-o2-desktop's and
 * fujinet-go-msdos-desktop's do. The input is a file, a zip (MAME's coleco.zip
 * works as it is) or a folder searched eight levels deep; a zip found in a
 * folder is opened too. A dump is recognised by size and CRC-32 whatever it
 * is called, and stored as OS7.rom. A file of exactly the BIOS's size whose
 * CRC is not in the table may be imported anyway (homebrew and regional
 * variants exist), but only when the user hands over that one file: a sweep
 * of a folder or a zip never guesses.
 *
 * WITH_COLECO_ROMS=ON embeds a developer's local copy; published builds are
 * built with it OFF and core/tests/no_embedded_roms.py checks that claim
 * against the shipped binaries.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "roms_embedded.h"
#include "session_internal.h"
#include "zipread.h"

/* The directory walk's depth limit: folders nested this many levels below the
 * one the user chose are searched; deeper ones are not. */
#define MAX_DEPTH 8
/* A zip inside a zip inside a folder is as far as anyone sensibly goes. */
#define MAX_ZIP_DEPTH 2
#define MAX_NESTED_ZIP (8u * 1024 * 1024)

/* The canonical filename in the user's ROM directory. Matched
 * case-insensitively on import so a file named os7.rom, OS7.ROM or
 * coleco.rom still lands correctly. */
#define BIOS_NAME "OS7.rom"

/* The two BIOS revisions in circulation. A single file of the right SIZE but
 * neither CRC is still accepted when the user names it -- homebrew and
 * regional variants exist -- but the mismatch is reported, because "the wrong
 * 8K file" is otherwise indistinguishable from "the emulator is broken". */
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

/* ---- the import ------------------------------------------------------------ */

static int is_known_crc(uint32_t crc)
{
    size_t i;

    for (i = 0; i < sizeof known_crc / sizeof *known_crc; i++)
        if (crc == known_crc[i])
            return 1;
    return 0;
}

typedef struct {
    struct colecosession *s;
    int probe;                  /* media_is_rom: look, do not write */
    int found;                  /* probe: something recognised */
    int guess_bios;             /* the user handed over one file: an 8K file of
                                   an unknown CRC may be the BIOS */
    int imported;               /* 1 once OS7.rom is written */
    int guessed;                /* it was taken on size alone */
    uint32_t guessed_crc;
    int write_failed;
    char write_error[COLECO_PATH_MAX + 48];
    /* the last candidate looked at, for "what is this file" */
    uint32_t last_size, last_crc;
    int last_valid;             /* last_crc was computed */
    int plain_file;             /* the chosen path is one file, not a zip or folder */
} import_ctx;

/* Written through a .part file and renamed, so a failed write leaves the old
 * ROM in place. */
static int write_rom(import_ctx *ctx, const uint8_t *data, uint32_t size)
{
    char path[COLECO_PATH_MAX], tmp[COLECO_PATH_MAX + 8];
    FILE *f;
    int ok;

    if (ctx->imported)
        return 1;               /* the first image in a sweep wins */
    paths_mkdir_p(ctx->s->roms_dir);
    bios_path(ctx->s, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.part", path);
    f = fopen(tmp, "wb");
    if (!f) {
        snprintf(ctx->write_error, sizeof ctx->write_error, "Cannot write %s", path);
        ctx->write_failed = 1;
        return 0;
    }
    ok = fwrite(data, 1, size, f) == size;
    if (fclose(f) != 0)
        ok = 0;
    if (ok) {
#if defined(_WIN32)
        remove(path);           /* rename() will not replace a file here */
#endif
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        remove(tmp);
        snprintf(ctx->write_error, sizeof ctx->write_error, "Cannot write %s", path);
        ctx->write_failed = 1;
        return 0;
    }
    ctx->imported = 1;
    return 1;
}

/* One candidate image. */
static void take(import_ctx *ctx, const uint8_t *data, uint32_t size)
{
    const uint32_t crc = zipread_crc32(data, size);

    ctx->last_size = size;
    ctx->last_crc = crc;
    ctx->last_valid = 1;
    if (size != COLECOSESSION_BIOS_SIZE)
        return;
    if (is_known_crc(crc)) {
        ctx->found = 1;
        if (!ctx->probe)
            write_rom(ctx, data, size);
        return;
    }
    if (ctx->guess_bios) {
        ctx->found = 1;
        if (!ctx->probe && write_rom(ctx, data, size)) {
            ctx->guessed = 1;
            ctx->guessed_crc = crc;
        }
    }
}

static int name_is_zip(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && (strcmp(name + n - 4, ".zip") == 0 || strcmp(name + n - 4, ".ZIP") == 0
                     || strcmp(name + n - 4, ".Zip") == 0);
}

static void import_zip(import_ctx *ctx, zipread *z, int zdepth)
{
    int i;

    for (i = 0; i < zipread_count(z) && !(ctx->probe && ctx->found); i++) {
        uint32_t size = zipread_size(z, i);
        uint8_t *data = NULL;
        const char *name = zipread_name(z, i);

        if (name_is_zip(name) && zdepth < MAX_ZIP_DEPTH && size <= MAX_NESTED_ZIP) {
            if (zipread_extract(z, i, &data, &size, MAX_NESTED_ZIP) == 0) {
                zipread *inner = zipread_open_memory(data, size);
                free(data);
                if (inner) {
                    import_zip(ctx, inner, zdepth + 1);
                    zipread_close(inner);
                }
            }
            continue;
        }
        if (size != COLECOSESSION_BIOS_SIZE)
            continue;
        if (zipread_extract(z, i, &data, &size, COLECOSESSION_BIOS_SIZE) == 0) {
            int guess = ctx->guess_bios;
            ctx->guess_bios = 0;     /* never from inside a zip */
            take(ctx, data, size);
            ctx->guess_bios = guess;
            free(data);
        }
    }
}

static void import_path(import_ctx *ctx, const char *path, int depth);

static void import_dir(import_ctx *ctx, const char *dir, int depth)
{
    DIR *d = opendir(dir);
    struct dirent *e;

    if (!d)
        return;
    while ((e = readdir(d)) != NULL && !(ctx->probe && ctx->found)) {
        char path[COLECO_PATH_MAX];

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        import_path(ctx, path, depth + 1);
    }
    closedir(d);
}

/* `depth` is how many folders below the chosen one this path is (0 for the
 * chosen path itself). */
static void import_path(import_ctx *ctx, const char *path, int depth)
{
    struct stat st;
    zipread *z;

    if (stat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        if (depth <= MAX_DEPTH && !ctx->probe)
            import_dir(ctx, path, depth);
        return;
    }
    if (!S_ISREG(st.st_mode))
        return;

    /* only a file the user named is ever taken on size alone */
    if (depth > 0)
        ctx->guess_bios = 0;

    z = zipread_open(path);
    if (z) {
        import_zip(ctx, z, 0);
        zipread_close(z);
        return;
    }
    if (depth == 0)
        ctx->plain_file = 1;
    ctx->last_size = (uint32_t)st.st_size;
    ctx->last_valid = 0;
    if ((uint32_t)st.st_size == COLECOSESSION_BIOS_SIZE) {
        uint8_t *data = read_exact(path, COLECOSESSION_BIOS_SIZE);
        if (data) {
            take(ctx, data, COLECOSESSION_BIOS_SIZE);
            free(data);
        }
    }
}

static const char *base_of(const char *path)
{
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    return slash ? slash + 1 : path;
}

#if defined(__GNUC__)
static void say(char *msg, int msgsz, int *len, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
#endif
static void say(char *msg, int msgsz, int *len, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (!msg || msgsz <= 0 || *len >= msgsz - 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(msg + *len, (size_t)(msgsz - *len), fmt, ap);
    va_end(ap);
    if (n > 0)
        *len += n < msgsz - *len ? n : msgsz - *len - 1;
}

static void fail(struct colecosession *s, char *msg, int msgsz, const char *text)
{
    session_set_error(s, "%s", text);
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, "%s", text);
}

static int roms_import_ex(struct colecosession *s, const char *path, char *msg, int msgsz,
                   int *guessed)
{
    import_ctx *ctx;
    struct stat st;
    int len = 0, rc;

    if (msg && msgsz > 0)
        msg[0] = '\0';
    if (guessed)
        *guessed = 0;
    if (!s || !path || !*path) {
        fail(s, msg, msgsz, "No file to import");
        return -1;
    }
    if (stat(path, &st) != 0) {
        char text[COLECO_PATH_MAX + 32];
        snprintf(text, sizeof text, "Cannot read %s", path);
        fail(s, msg, msgsz, text);
        return -1;
    }

    ctx = calloc(1, sizeof *ctx);
    if (!ctx) {
        fail(s, msg, msgsz, "Out of memory");
        return -1;
    }
    ctx->s = s;
    ctx->guess_bios = S_ISREG(st.st_mode);
    import_path(ctx, path, 0);

    if (ctx->write_failed) {
        fail(s, msg, msgsz, ctx->write_error);
        free(ctx);
        return -1;
    }

    if (ctx->imported) {
        say(msg, msgsz, &len, "Imported %s.", BIOS_NAME);
        if (ctx->guessed)
            say(msg, msgsz, &len,
                " That file's CRC-32 (%08X) is not one I know: it was taken as the "
                "BIOS because of its size. If the machine misbehaves, suspect it first.",
                ctx->guessed_crc);
        say(msg, msgsz, &len, " Takes effect at the next power cycle.");
    } else if (ctx->plain_file) {
        say(msg, msgsz, &len, "%s is not a ROM this app imports (%u bytes",
            base_of(path), ctx->last_size);
        if (ctx->last_valid)
            say(msg, msgsz, &len, ", CRC-32 %08X", ctx->last_crc);
        say(msg, msgsz, &len,
            "). The ColecoVision BIOS is 8,192 bytes: MAME's coleco.zip.");
    } else {
        say(msg, msgsz, &len, "No ColecoVision BIOS was found in %s.", path);
    }

    if (guessed)
        *guessed = ctx->guessed;
    rc = ctx->imported;
    free(ctx);
    return rc;
}

int colecosession_rom_import(colecosession *s, const char *path, char *msg, int msgsz)
{
    return roms_import_ex(s, path, msg, msgsz, NULL);
}

int colecosession_media_is_rom(const char *path)
{
    import_ctx *ctx;
    struct stat st;
    int found;

    if (!path || !*path)
        return 0;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    ctx = calloc(1, sizeof *ctx);
    if (!ctx)
        return 0;
    ctx->probe = 1;             /* guess_bios stays 0: an unknown 8K file is a cartridge */
    import_path(ctx, path, 0);
    found = ctx->found;
    free(ctx);
    return found;
}

/* The single-file entry point older callers and tests use. 0 = imported, 1 =
 * imported but the CRC is not one I know (the warning is in last_error), -1 =
 * not imported (the reason is in last_error). */
int colecosession_import_bios(colecosession *s, const char *src_path)
{
    char msg[512];
    int guessed = 0;
    int rc = roms_import_ex(s, src_path, msg, sizeof msg, &guessed);

    if (rc < 0)
        return -1;
    if (rc == 0) {
        session_set_error(s, "%s", msg);
        return -1;
    }
    if (guessed) {
        session_set_error(s, "%s", msg);
        return 1;
    }
    return 0;
}
