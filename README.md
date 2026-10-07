# ti99-libretro

A [libretro](https://www.libretro.com/) core for the **Texas Instruments
TI-99/4A**, to give [retrods](https://github.com/) — and any other libretro
frontend — the one system in that collection with no core at all.

## Status

| | |
|---|---|
| upstream | `upstream/`, a submodule pinned at `80e74d7` (billzajac/ti99sim), unmodified |
| libretro core | **works** — `make libretro` builds `ti99_libretro.so`; it boots the console, renders all four VDP modes, takes keyboard and pad input, makes sound and supports save states |
| CI | builds the upstream core on 4 platforms, builds the core on Linux and smoke-tests it, attaches both to releases |

ti99sim is a clean split between the machine (`src/core/`) and a frontend
(`src/sdl/`). This is a second frontend, so `upstream/` is untouched: the port
is entirely `src/libretro.cpp` plus the objects the Makefile compiles out of
`upstream/src/core`.

## Building

ti99sim is a submodule, so fetch it first:

```sh
git clone --recursive <this repo>       # or, in an existing clone:
git submodule update --init

make            # ti99_libretro.so
make core       # compile upstream/src/core on its own (a compile check)
make bootcheck  # tools/bootcheck, the harness below
```

macOS has no `<endian.h>`, which ti99sim's `common.hpp` expects under
`__GNUC__`; `compat/endian.h` shims it and the Makefile only adds that include
path on Darwin, so the upstream tree stays pristine and other platforms are
unchanged.

## System files

Put the console dumps in the frontend's **system directory** (RetroArch:
`system/`):

| file | size | notes |
|---|---|---|
| `994aROM.bin` | 8192 | the console's two 4 KiB CPU ROMs. A raw dump concatenates the chips — one holds the even-address bytes, the other the odd-address bytes — so the core interleaves them back into one 8 KiB image |
| `994aGROM.bin` | 24576 | the three 8 KiB console GROM banks |

The core builds the console's ROM/GROM cartridge in memory from these two
files. Its CPU memory map is the console's plus the usual expansion:

| region | contents |
|---|---|
| `>0000`-`>1FFF` | console ROM (from `994aROM.bin`) |
| `>2000`-`>3FFF`, `>A000`-`>FFFF` | the 32 KiB RAM expansion |
| `>6000`-`>7FFF` | cartridge (see below) |
| `>8000`-`>8FFF` | scratch-pad RAM at `>8300` |

Without the scratch-pad RAM every register write is discarded and the CPU
executes scratch-pad garbage; the 32 KiB expansion is what the `-32KB`
cartridge images need.

Content can be:

* a ti99sim **`.ctg`** image, which may carry both a CPU ROM and GROMs;
* a raw **`.bin`** cartridge ROM, the format the FlashROM 99 and FinalGROM 99
  use. Images up to 32 KiB are mapped as 8 KiB banks at `>6000`-`>7FFF`,
  selected by the address written inside that window — ti99sim's bank-switch
  scheme, and FR99's;
* a raw **`.G`** GROM image, placed in the cartridge's GROM banks from GROM base
  3, above the console's own GROMs. **Unverified** — see below.

With no content the console boots to its title screen and "1 for TI BASIC";
with a cartridge its entries are added to that menu.

## Controls

The full TI keyboard is mapped through libretro's keyboard device (frontends
that expose it — RetroArch, and retrods' keyboard): letters and digits map
directly, <kbd>Shift</kbd> is the TI shift key, <kbd>Alt</kbd> is **FCTN**, and
the punctuation that needs it (`(`, `)`, `*`, `+`, `-`, …) comes from the usual
shifted pairs. The D-pad and buttons mirror the common TI keys for pad-only
frontends: D-pad is <kbd>FCTN</kbd>+<kbd>E/S/D/X</kbd>, A is Enter, B is space,
X is erase and Y is next-field.

## Save states

`retro_serialize`/`retro_unserialize` work. ti99sim's own snapshot is written to
a file under `$HOME` by name — neither usable by libretro nor a size the core
can report up front (its VRAM goes through an RLE codec) — so the state is
assembled here instead: the CPU and the PIC serialise to a stream, the VDP's
registers, sprite priority table and VRAM are written verbatim, the two 64 KiB
memories and the GROM pointer come from the console, and the sound chip carries
its own voice phases.

## Layout

| | |
|---|---|
| `src/libretro.cpp` | the whole port: VDP rendering, sound synthesis, input, content, save states |
| `upstream/` | ti99sim submodule at `80e74d7`, unmodified |
| `include/libretro.h` | the libretro API header (libretro team), vendored |
| `compat/endian.h` | macOS `<endian.h>` shim |
| `tools/bootcheck.cpp` | runs the core's own entry points and writes a PPM |
| `NOTICE` | licence and provenance |

## Checking it

`tools/bootcheck` drives `retro_init`/`retro_load_game`/`retro_run` directly —
"it links" is not "it boots".

```sh
TI99_SYS=<dir with the ROMs> ./tools/bootcheck "" 300        # to the title screen
TI99_SYS=... TI99_TYPE='##...@#1' ./tools/bootcheck "" 700   # key through to TI BASIC
TI99_SAVETEST=1 TI99_SYS=... ./tools/bootcheck "" 1250       # save-state round trip
```

`TI99_TYPE` types a script on the emulated keyboard (`@` is pad A, `#` a pause),
so a BASIC program can be entered and run; the harness prints the audio peak,
which is how sound is checked. `TI99_PPM=<file>` writes the last frame.

`TI99_SAVETEST` serialises at frame 300, again at 400 (the state must have
changed), reloads at 800 and checks that re-serialising gives the identical
image, that the machine then renders the same frames it did the first time, and
that 400 frames later it is byte-for-byte back at the frame-700 state.

## Not done

* **Disks (`.dsk`).** ti99sim's disk support is a `cDiskDevice` behind its own
  `ti-disk` DSR ROM, handed to it as a cartridge; that ROM is not in the tree and
  no TI disk image is available here to validate a read against, so only
  cartridges load.
* **`.G` GROM images are unverified.** The mapping (GROM base 3 upward) is the
  one ti99sim's own GROM cartridges use, but the single GROM image to hand is
  not picked up by the console's menu, so treat raw `.G` as experimental. GROM
  cartridges in `.ctg` form are unaffected.
