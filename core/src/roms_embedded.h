/*
 * The embedded BIOS table. Generated at build time: a real image when
 * WITH_COLECO_ROMS=ON, a NULL pointer otherwise -- which is what every
 * published build ships, and what core/tests/no_embedded_roms.py verifies.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef COLECO_ROMS_EMBEDDED_H
#define COLECO_ROMS_EMBEDDED_H

#include <stdint.h>

/* NULL when no BIOS was embedded. */
extern const uint8_t *const coleco_embedded_bios;

#endif
