/*
 * media -- routing a file the user dropped on the window to the right place.
 *
 * Two destinations, and which one a file wants is not a matter of taste:
 *
 *   Cartridges (.rom .col .bin) go to the cartridge directory and are loaded
 *   by the emulator itself, through the FujiNet cartridge device -- which
 *   means every ColecoVision mapper works, because colmap decides the
 *   layout, not us.
 *
 *   Disk and tape images go to the FujiNet SD folder, because FujiNet is
 *   what serves them. Copying one into the cartridge directory would look
 *   like it worked and then fail to boot.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "session_internal.h"

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    return slash ? slash + 1 : path;
}

static int ext_is(const char *path, const char *const *exts)
{
    const char *dot = strrchr(base_name(path), '.');
    int i;
    if (!dot) return 0;
    for (i = 0; exts[i]; i++) {
        const char *a = dot + 1, *b = exts[i];
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            if (ca != *b) break;
            a++; b++;
        }
        if (!*a && !*b) return 1;
    }
    return 0;
}

/* .bin is deliberately included and deliberately last: it is ambiguous (the
 * Astrocade and plenty of other systems use it too), but on a ColecoVision
 * app a dropped .bin is overwhelmingly a cartridge, and colmap rejects
 * anything it cannot map, so a wrong guess fails loudly at load rather than
 * quietly at boot. */
static const char *const cart_exts[] = { "rom", "col", "bin", NULL };
static const char *const disk_exts[] = { "dsk", "ddp", "img", "atr", "po",
                                         "do", "d64", "cas", NULL };

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out;
    char buf[16384];
    size_t n;

    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return -1;
        }
    }
    fclose(in);
    if (fclose(out) != 0) return -1;
    return 0;
}

int colecosession_import_media(colecosession *s, const char *src_path,
                               char *dest_out, int dest_sz)
{
    const char *name = base_name(src_path);
    const char *dir;

    if (!src_path || !*src_path) {
        session_set_error(s, "No file to import");
        return -1;
    }

    if (ext_is(src_path, cart_exts)) {
        dir = s->carts_dir;
    } else if (ext_is(src_path, disk_exts)) {
        /* The SD tree only exists once the FujiNet runtime has been
         * provisioned. Test the DIRECTORY, not just the path string: the
         * path is always computed, so a string check passes and the copy
         * then fails with "could not copy", which tells the user nothing
         * about the actual problem. */
        struct stat st;
        if (!s->fujinet_sd[0] ||
            stat(s->fujinet_sd, &st) != 0 || !S_ISDIR(st.st_mode)) {
            session_set_error(s,
                "%s is a disk image, which FujiNet serves -- but the FujiNet "
                "runtime is not available, so there is nowhere to put it.",
                name);
            return -1;
        }
        dir = s->fujinet_sd;
    } else {
        session_set_error(s,
            "Don't know what %s is. Cartridges are .rom, .col or .bin; disk "
            "images go to FujiNet's SD folder.", name);
        return -1;
    }

    snprintf(dest_out, (size_t)dest_sz, "%s/%s", dir, name);
    if (copy_file(src_path, dest_out) != 0) {
        session_set_error(s, "Could not copy %s to %s", name, dir);
        return -1;
    }
    return 0;
}

int colecosession_media_is_cartridge(const char *path)
{
    return path ? ext_is(path, cart_exts) : 0;
}
