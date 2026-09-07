# FujiNet Go — ColecoVision

A self-contained ColecoVision with a built-in [FujiNet](https://fujinet.online/):
browse a network host from the console, boot images over the network, and let
the booted program keep talking to the network — all in one desktop app. The
newest member of the FujiNet Go desktop family (`fujinet-go-adam-desktop`,
`-apple2-`, `-coco-`, `-msx-`, `-intv-`, `-astrocade-`).

## Status

Early. See `TODO` for the milestone-by-milestone log; nothing there is marked
done on "it compiles".

- **M0 — scaffold** ✅ builds and tests on Linux.
- **M1 — adamcore** — in progress. The ColecoVision-side emulator work (a
  cartridge-device vtable, the Opcode Super Game Module, a corrected
  stock-console memory map) lands upstream in
  [adamcore](https://github.com/tschak909/adamcore).

## What it is

- **Emulator core** — [adamcore](https://github.com/tschak909/adamcore), the
  clean-room GPLv3 Coleco ADAM / ColecoVision core: Z80 (validated against Tom
  Harte's SingleStepTests and ZEXDOC/ZEXALL), TMS9928A VDP, SN76489 PSG, and
  the ColecoVision I/O map. Staged into `core/adamcore-generated/` and
  compiled with this project's flags.
- **Opcode Super Game Module** — AY-3-8910 and the 24K RAM expansion, modelled
  as permanently attached (with a settings toggle for a bare console).
- **FujiNet** — the cartridge device compiles the firmware's own protocol
  sources verbatim (identical-by-construction with the real RP2040 cartridge)
  and dials a real `fujinet-pc` built in-process as `libfujinet`. Every
  ColecoVision mapper — MegaCart, Activision, X-in-1 and the Opcode SGC — is
  modelled by `colmap_serve`, the same function the cartridge itself inlines.
- **Frontends** — GNOME (GTK4/libadwaita), KDE (Qt6 Widgets), macOS (AppKit)
  and Windows (Win32/GDI), each with the emulator display, a clickable
  two-controller keypad window, a Z80 + VDP + SGM + cart-device debugger,
  gamepad support and ROM import.

## Building

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Both dependencies (adamcore and fujinet-firmware) are provided automatically —
a plain `git clone` with no `--recurse-submodules` is enough. To develop
against working checkouts instead:

```sh
cmake -B build -DADAMCORE_SRC=~/Workspace/adamcore \
               -DFUJINET_SRC=~/Workspace/fujinet-firmware
cmake --build build
```

Add `-DADAMCORE_RESTAGE=ON` / `-DFUJI_RESTAGE=ON` to pick up uncommitted edits
in those checkouts. `-DFRONTEND=none` builds just the core and its tests.

> The `fujinet-firmware` pin is on branch **`colecovision-bringup`**, not
> master: `pico/coleco` has not merged yet. The configure fails with a message
> naming the branch if the pin does not carry it.

### The BIOS

The ColecoVision OS-7 BIOS is copyrighted Coleco firmware and is **not**
redistributed here. Published builds use `-DWITH_COLECO_ROMS=OFF` (the
default) and import the BIOS from the user's ROM directory at run time. For
local development, put `OS7.rom` in `tools/roms/` and configure with
`-DWITH_COLECO_ROMS=ON`. See `COMPLIANCE.md`.

## Ports

FujiNet's BoIP (bus-over-IP) listener is on **11502** and its web admin UI on
**11503** — high ports of this app's own, so a standalone `fujinet-pc` or a
sibling FujiNet Go app on the same machine never collides with it.

Unusually for the family, **FujiNet listens and the emulator's cartridge dials
in** (as on CoCo, MSX and Astrocade). The session therefore starts FujiNet
first and waits for its listener before starting the emulator.

> If the link behaves strangely — a metronomic ~5 s connect/disconnect loop
> especially — check for a stray standalone `fujinet-pc` holding the port
> before suspecting the protocol: `ps aux | grep fujinet` and `ss -tnp`.

## How the FujiNet cartridge works

The 30-pin ColecoVision cartridge connector carries A0–A14, D0–D7 and four
pre-decoded chip selects — no `/RD`, no `/WR`, no clock, no reset. **The
cartridge cannot tell a read from a write.** So both directions of the mailbox
ride the read path: console → cart is a read inside a hotspot page where the
low address byte *is* the payload, and cart → console is bytes the cart paints
into the 32K window it already serves.

The full specification is
`fujinet-firmware/pico/coleco/firmware/include/fuji_mailbox.h`, the single
source of truth shared by the RP2040 firmware, the MAME device, the Z80 client
and this app.

## Licence

GPL-3.0-or-later. See `COMPLIANCE.md` for per-component provenance.
