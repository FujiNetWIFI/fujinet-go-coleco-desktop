# FujiNet Go — ColecoVision

A self-contained ColecoVision with a built-in [FujiNet](https://fujinet.online/):
browse a network host from the console, boot images over the network, and let
the booted program keep talking to the network — all in one desktop app. The
newest member of the FujiNet Go desktop family (`fujinet-go-adam-desktop`,
`-apple2-`, `-coco-`, `-msx-`, `-intv-`, `-astrocade-`).

## Status

Complete. All four frontends build and run, FujiNet runs in-process, and every
CI platform is green. See `TODO` for the milestone-by-milestone log; nothing
there is marked done on "it compiles".

| | |
|---|---|
| **Emulator** | [adamcore](https://github.com/tschak909/adamcore), clean-room GPLv3. Z80 validated against Tom Harte's SingleStepTests and ZEXDOC/ZEXALL, TMS9928A VDP, SN76489, and the Opcode Super Game Module — which reaches **TEST COMPLETED!** on Óscar Toledo G.'s own `super_game_module_test.rom`. |
| **FujiNet** | The cartridge device compiles the RP2040 firmware's own protocol sources verbatim and dials a real `fujinet-pc`, built in-process as `libfujinet`. Verified on the wire: `dev=70 cmd=F4 seq=1 → err=0 reply=06 rxlen=256 "SD"`. |
| **Frontends** | GNOME (GTK4/libadwaita), KDE (Qt6 Widgets), macOS (AppKit), Windows (Win32/GDI) — each with the display, a two-controller keypad window, and a full Z80 + VDP debugger. |
| **Packaging** | Per-frontend DEB/RPM/TGZ, two Flatpaks, a Windows zip and a macOS bundle, all through GitHub Actions. |

## What it is

- **Every ColecoVision cartridge mapper** — MegaCart, Activision, X-in-1 and
  the Opcode SGC — modelled by `colmap_serve`, the same function the real
  cartridge firmware inlines, so the emulated cart and the hardware cannot
  disagree about a bank.
- **The Super Game Module**, fitted by default: an AY-3-8910 sharing one
  timeline with the SN76489, and the 24K RAM expansion. A machine without one
  is genuinely bare — 1K at `$6000-$7FFF` mirrored four times, open bus below.
- **A debugger in all four frontends** over one shared engine: CPU / VDP /
  Trace tabs, breakpoints by symbol, a trace ring, a live VRAM dump you can
  poke, and the full VDP decode — name table, pattern banks, sprites with
  their attribute table, and the palette, all rendered from live VRAM.
- **A clickable controller panel** showing both hand controllers, with every
  control remappable. Buttons are *held*, not pulsed: the machine samples the
  controller once a frame, so a value present for only an instant falls
  between frames and a polling game never sees it.

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

## How this was verified

Every claim above was checked by running the thing, not by compiling it:

- **The FujiNet link** — a `READ_HOST_SLOTS` transaction traced end to end,
  from the Z80's cartridge-read hotspots through `fujimail`, `fujibus`, SLIP
  and TCP into `libfujinet`'s own handler and back into the cartridge window,
  with both ends of the socket in one process.
- **The Super Game Module** — `super_game_module_test.rom` reaches *TEST
  COMPLETED!*: base 1K, SGM detection, the 24K, the lower-8K BIOS swap and the
  AY (peak 19704, 88% non-silent). With the SGM absent it correctly reports
  *NO SUPER GAME MODULE DETECTED* and the audio is pure silence.
- **Gamepads** — 547 state changes from a real pad, every controller word
  matching the value `input_test` pins, including all four diagonals (this pad
  reports its d-pad as axes, not buttons) and fire-plus-direction together.
- **Windows** — the whole app cross-builds with mingw-w64 and its tests pass
  under Wine, including `boot_smoke` at 54 fps. That number matters:
  `clock_nanosleep(TIMER_ABSTIME)` is a no-op under mingw, so without the
  fallback the emulator would free-run.
- **Flatpak** — builds in a sandbox with no network, so anything the manifest
  does not declare is simply absent. It is the strictest test of the
  clone-and-build promise here.

## Licence

GPL-3.0-or-later. See `COMPLIANCE.md` for per-component provenance.
