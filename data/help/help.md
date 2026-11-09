This is the text of FujiNet Go ColecoVision's built-in Help (Help ▸ FujiNet Go
ColecoVision Help, or Ctrl+Shift+H; ⌘? on a Mac). The app shows each part only
to the frontend it is about; here you see them all. Every topic below is one
page in the app.

# Getting started

FujiNet Go ColecoVision is a ColecoVision with a FujiNet cartridge in its slot.
The console is adamcore's: the Z80, the TMS9928A video chip and the SN76489
sound chip, with the Opcode Super Game Module fitted. The FujiNet runs inside
the app, so CONFIG comes up at once and can load programs from the network.

1. **Import the BIOS.** The console will not start without its BIOS, and none
   comes with the app. **Import BIOS…** takes your own copy. See
   [BIOS and ROMs](#bios-and-roms).
2. **Pick a program in CONFIG.** Browse from the keypad: choose a host and an
   image on it, and FujiNet loads it into the cartridge. See
   [FujiNet](#fujinet).
3. **Or open a cartridge of your own.** **Open Cartridge…** takes a `.rom`,
   `.col` or `.bin` image. See [Cartridges](#cartridges).
4. **Play.** The keys, the gamepads and the Controllers window work both hand
   controllers. See [Controllers and keys](#controllers-and-keys).
5. **Change the machine.** [Preferences](#preferences) sets the Super Game
   Module, the palette, FujiNet and the sound.

## This Help

The list on the left has a topic for each part of the app; the search box
above it finds every section that has all the words you type. A **Help**
button in Preferences, the Debugger, the Controllers window and the FujiNet
console log opens the topic for what it shows, and
<kbd>Ctrl+Shift+H</kbd> does the same for the window you are in.

<!-- only: gnome kde windows -->

In Preferences, the Debugger and the console log, <kbd>F1</kbd> does too.

<!-- end -->

The same text is on GitHub, as
[data/help/help.md](https://github.com/FujiNetWIFI/fujinet-go-coleco-desktop/blob/main/data/help/help.md).

<!-- only: macos -->

On a Mac, the Help menu's search field also finds Help topics.

<!-- end -->

<!-- context: main -->
# The window

The console's picture fills the window. The menus have:

- **Open Cartridge…** and **Eject Cartridge**: see [Cartridges](#cartridges).
- **Reset Console** and **Reset to CONFIG**.
- **Import BIOS…** and **Import BIOS from a Folder…**: see
  [BIOS and ROMs](#bios-and-roms).
- **Controllers** (<kbd>F9</kbd>) and **Debugger** (<kbd>F12</kbd>).
- **TV Aspect (4:3)** and **Smooth Scaling**.
- **FujiNet Configuration** and **Console Log**.
- **Preferences** (<kbd>Ctrl+,</kbd>), **Help** and **About**.

<!-- only: gnome -->

On GNOME, everything is in the main menu (the button at the right of the
header bar).

<!-- end -->

<!-- only: kde windows -->

The menu bar has Machine, View, FujiNet and Help. The Machine menu has the
cartridge, reset and BIOS items, Preferences (Settings on Windows) and Quit
(Exit on Windows).

<!-- end -->

<!-- only: macos -->

On a Mac, Settings and Quit are in the app menu, and the cartridge, reset and
BIOS items are in the Machine menu. **Open Cartridge…** is ⌘O, **Reset
Console** ⌘R, **Controllers** ⌘K and **Debugger** ⌘D.

<!-- end -->

A **Needs BIOS** message (with an **Import BIOS…** button) appears when the
console has no BIOS to start with. See [Needing the BIOS](#needing-the-bios).
Other messages (a cartridge opened, the console reset, FujiNet restarted, a
gamepad plugged in or taken out) show for a few seconds.

## Reset and power

- **Reset Console** (<kbd>F5</kbd>) is the console's reset button: it restarts
  the cartridge in the slot.
- **Reset to CONFIG** (<kbd>F6</kbd>) is a power cycle that ejects an opened
  cartridge and boots FujiNet's CONFIG again.

## The picture

**TV Aspect (4:3)** shows the picture with a television's shape, or square.
**Smooth Scaling** blends the pixels when the picture is stretched, and leaves
them sharp when it is off.

# Controllers and keys

The ColecoVision has two hand controllers. Each has a short stick (eight
directions, from four switches), a fire button on each side of the case and a
keypad of 1 to 9, `*`, 0 and `#`. The **left fire** and the **right fire** are
two separate buttons, and a game may ask for either.

All of a controller's contacts can be held at once, from any mix of the
keyboard, a gamepad and the mouse: letting go of one never lets go of another.
Opposite directions held together cancel, and the console reads one keypad
key at a time, the lowest held.

## Default keys

Keys go by where they are on the keyboard, whatever your layout.

Controller 1 has the arrow keys and the digit row, because that is where a
one-player game looks. Controller 2 has no keypad: a keyboard has one set of
digits, and the Controllers window covers the second controller's.

| Does | Controller 1 | Controller 2 |
|---|---|---|
| Keys 0 to 9 | <kbd>0</kbd> to <kbd>9</kbd> | none |
| Key `*` | <kbd>Keypad</kbd> `*` | none |
| Key `#` | <kbd>Keypad /</kbd> | none |
| Up | <kbd>Up</kbd> | <kbd>W</kbd> |
| Down | <kbd>Down</kbd> | <kbd>S</kbd> |
| Left | <kbd>Left</kbd> | <kbd>A</kbd> |
| Right | <kbd>Right</kbd> | <kbd>D</kbd> |
| Left fire | <kbd>Left Ctrl</kbd> | <kbd>Left Shift</kbd> |
| Right fire | <kbd>Left Alt</kbd> | <kbd>Tab</kbd> |

The numeric keypad's digits dial too, with Num Lock on or off: where nothing
is bound to a numeric-keypad digit, it presses the digit-row key of the same
number.

| Console | Key |
|---|---|
| Reset Console | <kbd>F5</kbd> |
| Reset to CONFIG | <kbd>F6</kbd> |

A key does one thing: giving a key to a new job takes it from the old one.

The app keeps these keys for itself:

| Shortcut | Does |
|---|---|
| <kbd>Ctrl+,</kbd> | Preferences |
| <kbd>Ctrl+Shift+H</kbd> | Help |
| <kbd>F9</kbd> | Controllers |
| <kbd>F12</kbd> | Debugger |

<!-- only: gnome kde windows -->

<kbd>Ctrl</kbd> is a machine key too (Controller 1's Left fire), so steering with it held reaches the console; only the shortcuts above are the app's.

<!-- end -->

<!-- only: macos -->

Keys with ⌘ never reach the console.

<!-- end -->

<!-- only: linux -->

Key combinations with the Super (Windows) key are the desktop's.

<!-- end -->

## Swapping the fire buttons

Preferences ▸ Input ▸ **Swap controller buttons** swaps the left and right
fire buttons on both controllers, for games that expect the other way round.

## Gamepads

Gamepads are picked up as they are plugged in, and can come and go at any
time. The first drives Controller 1, the next Controller 2; a third drives
nothing until one is unplugged. A message says which controller a gamepad
took, and when one goes. Preferences ▸ Input and the Controllers window can
fix which is which. A gamepad's controller is remembered between runs: plug it
back in, or start the app again, and it gets its controller back.

| Gamepad | Does |
|---|---|
| D-pad, or left stick | The stick |
| South (A) | Left fire |
| East (B) | Right fire |
| West (X) | Key 1 |
| North (Y) | Key 2 |
| Left shoulder | Key `*` |
| Right shoulder | Key `#` |

The same buttons work on both controllers, for the gamepad that drives it.
Start, Back, Guide, the stick buttons and the triggers are free to be given
jobs. The left stick is an eight-way switch: it engages at half deflection
and lets go below a third. A gamepad with no standard layout (a plain USB
adapter) steers with its first stick or hat, and its buttons and hat
directions can be given jobs in the same way.

## Changing the controls

The controls are changed in the Controllers window (<kbd>F9</kbd>): click
**Map**, then click a control on the picture (38 in all: both controllers'
twelve keys, four directions and two fire buttons, and the two Reset
actions; a diagonal wedge is two directions at once, so it has no key of its
own), then press the keyboard key or the gamepad button to give it. The
control's old holder loses it, and the message names it. Map stays on, so
several controls can be given in a row; <kbd>Escape</kbd> steps back one
level and **Done** leaves Map.

- **Clear** takes the job off the key and the button of the control you
  picked.
- **Defaults** puts every control back (so does **Reset Controls** in
  Preferences ▸ Input).
- **Show bindings** writes each control's key on it.

<!-- only: macos -->

On a Mac a control can also be chosen from the list in the window, instead
of clicking it on the picture.

<!-- end -->

A key drives one control; a gamepad button drives one control of each
controller. <kbd>F9</kbd> and <kbd>F12</kbd> cannot be given away. The map is
kept in the settings, and only what differs from the defaults is written;
controls saved by earlier versions are carried over.

<!-- context: controllers -->
# Controllers window

The Controllers window (<kbd>F9</kbd>) draws both hand controllers as a player
sees them, Controller 1 on the left and Controller 2 on the right. It floats
on its own at a fixed size, stays hidden until you ask for it and hides
rather than closes. Each controller has:

- **The stick**: a short disc with a ring of eight direction wedges.
- **The fire buttons**, one on each side of the case.
- **The keypad**: twelve keys, 1 to 9, `*`, 0 and `#`, on a silver card with
  a slot at the top for a game's overlay (drawn only; nothing there is
  pressed).

The controls that are held light up in the accent colour, whoever holds them:
the keyboard, a gamepad or the mouse. Under the controllers are the console's
**Reset Console** and **Reset to CONFIG** buttons, then the row of **Map**,
**Clear**, **Defaults** and **Show bindings**, and a row for each gamepad.

| With the mouse | Does |
|---|---|
| Press a key or a fire button | Holds it for as long as the button is down |
| Press on a wedge | Holds that direction |
| Drag from the stick | Tilts it (eight ways, nothing near the middle) |
| Drag off a control | Lets it go |
| Release on a Reset button | Runs it |

The console reads its controllers once a frame, so a control is *held*, not
pulsed, as the plastic one is: a value present for an instant falls between
frames and a polling game would never see it. Switching to another window
lets go of everything the mouse holds. Keys typed in the Controllers window
go to the console as well.

Each gamepad has a row with its name, the controller it drives and a choice
of *Automatic*, *Controller 1* or *Controller 2*. The rows follow the
gamepads as they come and go. **Map**, **Clear**, **Defaults** and **Show
bindings** change the controls (see
[Changing the controls](#changing-the-controls)); once Map has a
control picked, the others are dimmed. Show bindings is remembered
(setting `controllers_show_bindings`).

# Machines

There is one machine: the ColecoVision, NTSC, with a Z80 and a TMS9928A.

- **The Super Game Module** is fitted by default: an AY-3-8910 sound chip that
  shares one timeline with the SN76489, and 24K of RAM. Cartridges that do not
  use it are unaffected. Without it the console is bare: 1K at `$6000` to
  `$7FFF` mirrored four times, and nothing below. Preferences ▸ Machine ▸
  **Super Game Module** turns it off.
- **The palette** picks which TMS9928A colour set the picture uses: *Default
  (TMS9928A)*, *Palette 2*, *Palette 3* or *Palette 4*.
- **The cartridge mappers** are every one the FujiNet cartridge serves:
  MegaCart, Activision, X-in-1 and the Opcode SGC.

# BIOS and ROMs

The ColecoVision's OS-7 BIOS is Coleco's copyrighted firmware, so none comes
with the app, and the console will not start without it. The BIOS is 8,192
bytes (CRC-32 `3AA93EF3`).

## Importing it

**Import BIOS…** takes your own copy: the single file (`OS7.rom`), a zip such
as MAME's `coleco.zip`, or a folder that holds either (**Import BIOS from a
Folder…**). It is in the Machine menu, on the ROMs page of Preferences, and in
the Needs BIOS message. The BIOS is recognised by its size and checksum,
whatever it is called. A file of the right size with an unknown checksum is
taken only when it is the one file given, with a warning. Dropping the file on
the window does the same. Imported files are copied into the app's `roms`
folder (see [Files and folders](#files-and-folders)). The new BIOS takes effect
at the next power cycle.

<!-- context: nobios -->
## Needing the BIOS

With no BIOS imported the console cannot start, and the app says so on its first
run. **Import BIOS…** opens the file chooser; once the BIOS is in, the console
starts, with the stored cartridge if there is one. **Not Now** leaves the
window as it is.

# Cartridges

A cartridge is a file: a `.rom`, `.col` or `.bin`. The cartridge in the slot is
always the FujiNet one; a program is loaded into it, from CONFIG or from a file.

## Open Cartridge

**Open Cartridge…** serves a local image from power-on in place of CONFIG. It is
remembered and starts again next time; **Eject Cartridge** or **Reset to
CONFIG** forgets it. Dropping a cartridge file on the window does the same, and
copies it into the `carts` folder; a BIOS dump is imported, and a disk image
goes to FujiNet's SD folder.

# FujiNet

The FujiNet is FujiNet's own firmware, running inside the app, and the
cartridge in the slot is its connection to the console. The cartridge's
protocol is the real cartridge firmware's own, so the emulated cartridge and the
hardware one cannot disagree: the console talks to a mailbox in the cartridge,
and the cartridge talks to FujiNet over a local socket (BoIP, FujiBus over
TCP). Unusually, FujiNet listens and the cartridge dials in, so the app starts
FujiNet first and waits for it.

## CONFIG

CONFIG is FujiNet's program for choosing what to run, driven from the keypad:

- **Hosts** are where programs come from: the SD folder and servers on the
  network.
- Pick a host and an image on it. FujiNet loads it into the cartridge and the
  console starts it.
- **Reset to CONFIG** brings CONFIG back.

## The FujiNet menu

- **Configuration** opens FujiNet's settings page in your browser, at
  `http://127.0.0.1:11503/`. Only this computer can reach it.
- **Console Log** shows what FujiNet prints. See
  [The console log](#the-console-log).

<!-- context: fujilog -->
## The console log

The FujiNet Console Log window shows FujiNet's own messages as they come,
following the end while you are at the bottom.

## Turning it off

Preferences ▸ FujiNet ▸ **Enable FujiNet** turns the FujiNet off. There is then
no network and no CONFIG client. Cartridges you open yourself are not affected.

## An external FujiNet

Preferences ▸ FujiNet ▸ **Use external FujiNet** skips the built-in FujiNet and
dials another FujiNet instance at the **Address:port** you give, for example
`127.0.0.1:11502`. Set that instance's BoIP listener to that address.
Configuration and Console Log belong to the built-in FujiNet, so they say the
FujiNet is external; use the other instance's own. The Debugger's FujiNet tab
still shows the traffic.

## Behind the scenes

- FujiNet's BoIP listener is on port 11502 and its web UI on 11503, on
  127.0.0.1. They are the app's own, so a standalone `fujinet-pc` or another
  FujiNet Go app on the same computer does not collide with it.
- FujiNet's settings are `fnconfig.ini` in the app's `fujinet` folder.

# Preferences

Preferences (<kbd>Ctrl+,</kbd>) set up the machine. The Machine and FujiNet
settings apply by restarting the session when Preferences closes; an opened
cartridge boots again. The Host settings apply at once, and the Input settings
when Preferences closes. The settings are one file, shared by the GNOME and
KDE apps on the computer.

<!-- only: macos -->

On a Mac this window is called **Settings** (in the app menu).

<!-- end -->

<!-- only: windows -->

On Windows this window is called **Settings** (in the Machine menu). It has a
tab for each group of settings below: Machine, Input and ROMs.

<!-- end -->

<!-- context: prefs/Machine -->
## Machine settings

- **Super Game Module**: fit the Opcode SGM, an AY-3-8910 and 24K of RAM
  (setting `sgm`).
- **Palette**: which TMS9928A colour set to render with: *Default (TMS9928A)*,
  *Palette 2*, *Palette 3* or *Palette 4* (setting `palette`).

## FujiNet settings

- **Enable FujiNet**: run the in-process FujiNet the cartridge dials into
  (setting `enable_fujinet`).
- **Use external FujiNet**: dial another instance instead (setting
  `fujinet_external`).
- **Address:port**: where to dial, for example `127.0.0.1:11502` (see
  [An external FujiNet](#an-external-fujinet); setting
  `fujinet_external_addr`).

## Host settings

- **Audio**: open the system audio device (setting `enable_audio`).

<!-- context: prefs/Input -->
## Input settings

- **Enable gamepads**: poll USB and Bluetooth gamepads (setting
  `enable_gamepad`).
- **Swap controller buttons**: exchange the left and right fire buttons on
  both controllers (setting `swap_buttons`).
- **Gamepads**: each one plugged in, and which controller it drives. A new
  gamepad drives Controller 1, the next Controller 2; choose *Controller 1* or
  *Controller 2* to fix it, or *Automatic*. The choice is remembered between
  runs (setting `pad_assign`), by the gamepad's id and name; two identical
  gamepads cannot be told apart, so the first to connect gets it.
- **Reset Controls**: put every key and gamepad button back to its default.
- **Open Controllers**: open the Controllers window, where the controls are
  changed (see [Changing the controls](#changing-the-controls)).

<!-- context: prefs/ROMs -->
## ROMs settings

- **Import BIOS…** and **Import from a Folder…**: add your own BIOS (see
  [Importing it](#importing-it)).
- **ROM folder**: where imported files are kept, with an **Open** button.
- **ColecoVision BIOS (OS7.rom)**: whether it is imported.

## The settings file

The settings file is `settings.ini` (see [Files and folders](#files-and-folders)).
Each setting above names its key. The others are `tv_aspect` and `smooth` (the
View menu), `cart_path` (the opened cartridge, empty for CONFIG), `bindings` (only the
controls that differ from the defaults), `pad_assign` (the gamepads kept on a
controller) and `controllers_show_bindings` (the Controllers window's Show
bindings).

# Debugger

The Debugger (<kbd>F12</kbd>) stops the console and shows what it is doing.
It has four tabs; closing it lets the console go on as it was.

| Button | Does |
|---|---|
| Pause or Continue (<kbd>F5</kbd>) | Stops before the next instruction, or runs on |
| Step Into (<kbd>F7</kbd>) | One instruction |
| Step Over (<kbd>F8</kbd>) | Over a call |
| Step Out (<kbd>Shift+F8</kbd>) | On until the routine returns |
| Go to addr / symbol | Moves the disassembly there |
| Trace | Records every instruction in a ring |
| Help | Opens the topic of the tab that shows |

<!-- context: debugger/cpu -->
## CPU

The Z80 disassembled at the PC, with labels and the breakpoints in the gutter;
click a line to toggle its breakpoint. Beside it are the registers (edit a value
and press Enter while stopped), the breakpoints (**Add** an address or a symbol;
**Clear all**) and a memory view (type an address or a symbol). Addresses take
`FC5D`, `$FC5D`, `0xFC5D` or a symbol name; the EOS and OS-7 symbols are known.

<!-- context: debugger/vdp -->
## VDP

The TMS9928A, decoded from its live video memory: the name table, the pattern
banks (choose Bank 0, 1 or 2), the sprites with their attribute table, the
palette, and the registers with the table addresses the beam is fetching from.
The VRAM view shows 16K a page at a time with ◀ and ▶; type
`addr [bytes...]`, for example `1800: 41 42 43`, and press Enter to write.
Addresses wrap at 16K.

<!-- context: debugger/trace -->
## Trace

With **Trace** on, the last instructions executed, with the registers at each:
`PC AF BC DE HL SP`. With it off the tab says so.

<!-- context: debugger/fujinet -->
## FujiNet packets

Every FujiBus packet on the cartridge's link, SLIP-decoded, both ways, with hex,
recorded from the moment the machine starts. **Clear** empties the list. Beside
it is the FujiNet cartridge's state.

# Files and folders

The app keeps its settings in one file, and everything else in its data
folder:

<!-- only: linux -->

On Linux, the settings are `~/.config/fujinet-go-coleco/settings.ini` and
the data is in `~/.local/share/fujinet-go-coleco` (or under
`$XDG_CONFIG_HOME` and `$XDG_DATA_HOME`). The GNOME and KDE apps share them.
As a Flatpak, they are under `~/.var/app/`, in the app's own folder.

<!-- end -->

<!-- only: windows -->

On Windows, the settings are `%APPDATA%\fujinet-go-coleco\settings.ini` and
the data is in `%LOCALAPPDATA%\fujinet-go-coleco`.

<!-- end -->

<!-- only: macos -->

On a Mac, the settings are `~/.config/fujinet-go-coleco/settings.ini` and
the data is in `~/.local/share/fujinet-go-coleco`.

<!-- end -->

In the data folder:

| Folder | Has |
|---|---|
| `roms` | the imported BIOS |
| `carts` | cartridges dropped on the window |
| `fujinet` | `fnconfig.ini`, the SD folder (`SD`) and FujiNet's data |

# Troubleshooting

- **The ColecoVision needs its BIOS**: none is imported. See
  [Needing the BIOS](#needing-the-bios).
- **Cannot Import BIOS**: the file is not a BIOS this app knows, by size and
  checksum. MAME's `coleco.zip` and the 8,192-byte `OS7.rom` work.
- **link down**: FujiNet is off, not started yet, or the external address
  is wrong. Check Preferences ▸ FujiNet.
- **Keys don't reach the console**: a dialog or another window is in front, or
  the key is one the app keeps (see
  [Controllers and keys](#controllers-and-keys)).
- **A game never sees the controller**: the controller is read once a frame; a
  control must be held, not tapped in an instant. The Controllers window holds
  a control as long as the button is down.
- **A gamepad does nothing**: Enable gamepads may be off in Preferences ▸
  Input, a third gamepad drives nothing, or its buttons may be given other
  jobs. The gamepad's row in the Controllers window shows which controller it
  drives.
- **A gamepad drives the wrong controller**: choose *Controller 1* or
  *Controller 2* for it in Preferences ▸ Input or the Controllers window; the
  choice is remembered.
- **No sound**: Audio may be off in Preferences, or the audio device could not
  be opened.
- **A port is taken**: another program is using 11502 or 11503. Close the other
  FujiNet, or point the app at it with **Use external FujiNet**.

# About and licences

FujiNet Go ColecoVision is free software under the GNU General Public License,
version 3 or later. Copyright © 2026 Thomas Cherryhomes.

- **adamcore** (the Z80, the TMS9928A, the SN76489 and the Super Game Module,
  GPL-3.0-or-later) is the emulator.
- **The FujiNet firmware** (GPL-3.0-or-later) runs inside the app, and the
  cartridge's own protocol sources are the firmware's (BSD-3-Clause).
- No BIOS or other copyrighted ROM is part of the app; you import your own.
  "ColecoVision" is a trademark of its owner, used to name the machine
  emulated; this project is not affiliated with it.

[fujinet.online](https://fujinet.online/) ·
[fujinet-go-coleco-desktop](https://github.com/FujiNetWIFI/fujinet-go-coleco-desktop)
