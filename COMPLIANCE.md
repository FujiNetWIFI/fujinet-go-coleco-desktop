# Compliance

Per-component provenance for `fujinet-go-coleco-desktop`, written before the
first public build, in the family tradition (see
`fujinet-go-adam-desktop/COMPLIANCE.md`, `fujinet-go-astrocade-desktop/COMPLIANCE.md`).

## What ships

| Component | Origin | Licence | How it enters the build |
|---|---|---|---|
| This application | this repository | GPL-3.0-or-later | — |
| **adamcore** | [`tschak909/adamcore`](https://github.com/tschak909/adamcore) | GPL-3.0-or-later | pinned in `cmake/Dependencies.cmake`, staged unmodified into `core/adamcore-generated/` by `cmake/StageAdamcore.cmake` |
| **`fujimail.c` / `fujibus.c` / `colmap.c`** | `fujinet-firmware`, `pico/coleco/firmware/` | BSD-3-Clause, © Thomas Cherryhomes | staged unmodified into `core/coleco/fuji-generated/` by `cmake/StageFujiProto.cmake` |
| **`fujitcp.c`** | `fujinet-firmware`, `pico/coleco/emu/` | BSD-3-Clause, © Thomas Cherryhomes | same |
| **`fujiconfigrom.h`** | `fujinet-firmware`, `pico/coleco/firmware/include/` | BSD-3-Clause, © Thomas Cherryhomes | same; the baked CONFIG client the cartridge serves with no image staged |
| **FujiNet firmware** (`libfujinet`) | [`FujiNetWIFI/fujinet-firmware`](https://github.com/FujiNetWIFI/fujinet-firmware) | GPL-3.0-or-later | built as a shared library by `tools/fujinet/build-fujinet-desktop.sh`, `dlopen`'d at run time |
| SDL3 | libsdl-org | Zlib | system package on Linux; linked statically on macOS/Windows |
| mbedTLS 3.6.x | Mbed-TLS | Apache-2.0 | system package where it is a usable 3.x, otherwise the pinned source |

The staged protocol sources are compiled **verbatim**. That is the point of
the arrangement: the emulated cartridge and the real RP2040 cartridge run the
same bytes, so they cannot disagree about the wire format. If you find
yourself editing anything under a `*-generated/` directory, edit the upstream
checkout and re-stage instead (`-DADAMCORE_RESTAGE=ON` / `-DFUJI_RESTAGE=ON`).

## System ROMs — not redistributed

The ColecoVision **OS-7 BIOS is copyrighted Coleco firmware and is not freely
licensed.** This repository does not contain it and does not distribute it.

- `WITH_COLECO_ROMS=OFF` is the **default**, and every published artifact is
  built that way. The application resolves `OS7.rom` from the user's ROM
  directory (`~/.local/share/fujinet-go-coleco/roms/`) at run time, with an
  import path in the UI, and says so plainly on first start rather than
  failing silently.
- `-DWITH_COLECO_ROMS=ON` embeds a local `tools/roms/OS7.rom` for development
  convenience. `.gitignore` refuses ROM images under `tools/roms/`, and
  `core/tests/no_embedded_roms.py` runs as a ctest against every built
  frontend binary to check the ROM-less claim on the artifacts a user actually
  downloads.

This differs deliberately from `fujinet-go-adam-desktop`, which embeds
EOS/OS7/SmartWriter on a maintainer determination that those lapsed into the
public domain. That determination is not relied on here; the ColecoVision app
takes the conservative route, which is also the route
`fujinet-go-astrocade-desktop` takes for the Bally BIOS and the model
`fujinet-go-msx-desktop` follows with C-BIOS.

## Deliberately not used

- **ADAMEm** (Marcel de Kogel) and **AdamEmSDL** — non-commercial licence,
  GPL-incompatible. No code from either is present. This includes its
  `z80dasm`: the debugger's Z80 disassembler is a fresh implementation from
  the Zilog manual, for exactly this reason.
- **Gearcoleco**, **ColEm**, **blueMSX**, **openMSX** — consulted as feature
  checklists only; no source taken.
- **MAME** — no source taken *into adamcore*. See the note below, which is the
  substantive point rather than a formality.

## Why `colmap.c` lives here and not in adamcore

adamcore is strictly clean-room: its `PROVENANCE.md` records that it was
written without consulting any existing ADAM/ColecoVision emulator's source,
with every table derived from datasheets and first principles.

`colmap.c` is not clean-room in that sense, and does not claim to be. It
models the established ColecoVision cartridge mappers (MegaCart, Activision,
X-in-1, the Opcode SGC) and its own comments state that it matches MAME's
handlers deliberately — *"We match MAME so the A/B test compares equal"* —
because being byte-compatible with MAME is what makes the firmware's soak test
meaningful.

That is emulator-behaviour-derived knowledge. Putting it inside adamcore would
make adamcore's clean-room statement false. It therefore stays on this side of
the `adamcore_cart_ops` boundary, staged from `fujinet-firmware` alongside the
rest of the protocol, where its BSD-3-Clause provenance is recorded and
correct. adamcore contributes the console; this repository contributes the
cartridge.

## Debug symbol tables

Symbol tables used by the debugger (names and addresses extracted from
published disassemblies) are tables of **facts** and contain no program code.
They are not derived from any emulator or from copyrighted firmware, and are
distributed here on that basis — the same position
`fujinet-go-adam-desktop/COMPLIANCE.md` records for its EOS/OS7 tables.

## Icon artwork

`data/icons/src/fujinet-go-coleco-foreground.png` is the shared FujiNet
launcher foreground, byte-identical to the art used by the sibling apps in the
family. The background colour is this app's own.
