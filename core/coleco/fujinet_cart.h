/*
 * fujinet_cart -- the FujiNet cartridge, the one cart device this core has.
 *
 * A C transposition of the firmware's own MAME device model
 * (fujinet-firmware pico/coleco/emu/fujinet.cpp, BSD-3-Clause, Thomas
 * Cherryhomes): the ColecoVision's 32K cartridge window served from RAM,
 * with the read-hotspot mailbox of fuji_mailbox.h decoded on offsets
 * 0x7D00-0x7FFF and replies repainted into the window. The protocol itself
 * (fujimail.c), the wire codec (fujibus.c) and the image mapper (colmap.c)
 * are the cartridge firmware's own sources, staged verbatim into
 * fuji-generated/ by cmake/StageFujiProto.cmake; this file is only the port:
 * bytes go into the served window, frames go over a TCP socket to the
 * bundled fujinet-pc's BoIP listener.
 *
 * An image that does not carry the "FUJI" claim signature runs with the
 * mailbox dead -- a plain ROM, or a banked game on its own mapper (MegaCart,
 * X-in-1, Activision, Opcode SGC, all through colmap_serve) -- which is the
 * "load anything" path, and also how a network-booted image behaves after
 * the swap. With no image at all, the baked-in real CONFIG client
 * (fujiconfigrom.h, generated from the fujinet-config build) is served, the
 * same image the RP2040 cartridge bakes in.
 *
 * Single instance per process: fujimail's port interface is C function
 * pointers with no context argument. The constraint is real hardware's too
 * -- one cart slot, one cart.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COLECO_FUJINET_CART_H
#define COLECO_FUJINET_CART_H

#include <stdint.h>

#include "adamcore.h"
#include "colmap.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coleco_cart coleco_cart;

/* Creates the device and opens the link.
 *
 *  image/size   the cartridge to serve; NULL/0 serves the baked-in CONFIG
 *               client, which is what a real cartridge does with nothing
 *               staged.
 *  hostport     "host:port" for the FujiNet BoIP listener; NULL takes
 *               $FUJINET_TCP, then the built-in default.
 *
 * Returns NULL if a device already exists (see the single-instance note
 * above) or on allocation failure. Never fails merely because the link is
 * down: the cartridge is expected to outlive a FujiNet restart. */
coleco_cart *coleco_cart_create(const uint8_t *image, uint32_t size,
                                const char *hostport);
void coleco_cart_destroy(coleco_cart *c);

/* Install into a core with adamcore_set_cart_ops(core, coleco_cart_ops(), c). */
const adamcore_cart_ops *coleco_cart_ops(void);

/* Is the mailbox decoding hotspots? False for an image with no claim
 * signature, and after a swap to one. */
int coleco_cart_mailbox_live(const coleco_cart *c);

/* Is the TCP link to FujiNet up? */
int coleco_cart_link_up(const coleco_cart *c);

/* What the served image was mapped as, for a status display or debugger. */
colmap_kind_t coleco_cart_kind(const coleco_cart *c);

#ifdef __cplusplus
}
#endif

#endif /* COLECO_FUJINET_CART_H */
