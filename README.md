# CLoopy

This started as a C11-only port of LoopyMSE.
It's got a few improvements :
- Can run on the web
- libretro core, with savestates and rewind
- Limited printing support
- Attempting to be more CPU accurate and follow the documented behavior more closely by using MAME's SH-1 
- A few VDP fixes (there may be a few regressions), including emulation for the Controller bit. (that was one big annoyance i had encountered initially)

The main reason why i even made this, is because i kept encountering annoying crashes and issues on real hardware in regards to homebrew software.

To be more accurate, this would need more intensive CPU tests + ROM/RAM wait states etc.. Right now, there is some but not really,
the speed of emulator != speed of console, but with my limited testing, it diverges less from real console than LoopyMSE would.

And also LoopyMSE would spam that terminal like crazy lol.

# libretro core

```sh
make libretro          # -> cloopy_libretro.so (.dylib on macOS)
```

Put `loopy_bios.bin` in the frontend's system directory. `loopy_soundbios.bin`
(the sound BIOS) and `loopy_okirom.bin` (a real MSM6653A-457 dump for Wanwan)
are optional and go in the same place. Battery saves go through the frontend as
`.srm`; the core does not write `.sav` itself in this mode.

Savestates use the same in-memory state blob as the native frontends, so the
frontend's rewind works. `cloopy_libretro.info` is the core info file frontends
read.

## Idle Loop Skip

Loopy titles spend most of their CPU time spinning in the BIOS vblank wait loop
polling a VDP register. This option detects loops that provably cannot observe
anything changing before the next scheduler event and fast-forwards the CPU
through them, which is worth roughly 1.7-2x.

It is bit-for-bit identical to running the spin for real: the detector only
fires when a whole iteration wrote nothing, read nothing that can change on its
own, and left every register identical, and it then consumes a whole number of
iteration costs and lets the final iteration execute normally, so the slice ends
on exactly the cycle it otherwise would. Verified by capturing every frame with
the option on and off and comparing.

It defaults to retail cartridges only, detected from the Casio copyright string
in the cartridge header. Homebrew is what this emulator exists to test against
real hardware, so unrecognised images opt out rather than in. Note that a
rebuilt image which drops that string (a translation patch, for instance) reads
as unrecognised even though the underlying game is retail. The native builds
take `--idle-skip` / `--no-idle-skip` to force it either way, and the libretro
core has the same three-way option.

# Inspection, profiling and MCP

The headless build doubles as an analysis tool for the hardware itself. All of
it reads through a side-effect-free peek path, so sampling the machine never
changes what it does next -- MMIO registers are deliberately *not* readable this
way, because reading one for real can clear a status flag or move the VDP bus
latch.

```sh
make headless
./cloopy_headless game.bin loopy_bios.bin loopy_sound.bin --frames 600 \
    --bus-prof --cpu-prof --bios-trace
```

| Flag | What it does |
| --- | --- |
| `--list-regions` | Memory map with ranges and bus widths, incl. the BIOS-reserved splits |
| `--disasm ADDR[,COUNT]` | Disassemble SH-1 code, resolving PC-relative literals |
| `--dump REGION FILE` | Write a named region to disk |
| `--bus-prof` | Per-region memory traffic: reads/writes/fetches/bytes/wait cycles |
| `--bus-prof-per-frame` | The same, printed and reset every frame |
| `--cpu-prof [--cpu-prof-top N]` | Hottest PCs by cycles consumed, disassembled |
| `--bios-trace` | Discover BIOS entry points by tracing calls into BIOS ROM |
| `--mcp` | Serve MCP over stdio instead of running frames |

## Memory regions

Region names come from the Loopy hardware notes' CPU Map. Two splits are worth
knowing about, because the BIOS keeps live state in both and a homebrew linker
script that treats either as free will be corrupted by the next BIOS call:

- `0x09000000-0x090000FF` -- work RAM reserved for BIOS state (`wram_bios`);
  game code gets `0x09000100-0x0907FFFF` (`wram_game`).
- `0x0F0003F0-0x0F0003FF` -- the top 16 bytes of the 1KB on-chip RAM
  (`ocram_bios`); game code gets `0x0F000000-0x0F0003EF` (`ocram_game`).

## Disassembler

Strict SH-1: encodings the interpreter rejects as SH-2-only (`BRAF`, `BSRF`,
`MUL.L`, `MAC.L`, `DMULS.L`, `DMULU.L`, `DT`, `BT/S`, `BF/S`) disassemble as
`.WORD` rather than suggesting something this CPU would refuse to execute.
Verified against binutils `sh-elf-objdump -m sh -M sh1` across all 65536
encodings, with `make test-disasm` pinning the cases that are easy to regress.

## BIOS call tracing

The hardware notes name BIOS routines (`bios_vsync`, `bios_dma`, `bios_vdpMode`,
`bios_playBgm`, ...) but publish no entry addresses and no call ABI -- they are
deferred to a document that does not exist yet, and the vsync helper is
explicitly "not yet known how to call it". Only four entries are known at all,
from this emulator's own printer hooks.

So `--bios-trace` does not look entries up, it observes them: it records calls
and tail jumps from outside the BIOS ROM into it, by target address, with the
caller and the R4-R7 arguments as they stood on entry. Returns are excluded --
an interrupt taken while the BIOS holds the CPU comes back through `RTE` and
lands in the middle of BIOS code, which is a resumption, not a call.

Run a game and read the table back to learn the real entry points.

# MCP server

```sh
./cloopy_headless game.bin loopy_bios.bin loopy_sound.bin --mcp
```

Speaks MCP over stdio as newline-delimited JSON-RPC 2.0, with no external
dependencies. The session is persistent and stateful, which is the point: a
client can run frames, inspect, snapshot, run further, restore and compare --
enough to bisect a timing bug rather than just take one reading.

stdout is the transport, so the server takes it over at startup and redirects
ordinary `printf` to stderr; nothing else can corrupt the stream.

Tools: `loopy_run_frames`, `loopy_cpu_state`, `loopy_list_regions`,
`loopy_read_memory`, `loopy_dump_region`, `loopy_disassemble`,
`loopy_bus_profile`, `loopy_cpu_profile`, `loopy_bios_trace`,
`loopy_save_state`, `loopy_load_state`, `loopy_set_input`, `loopy_screenshot`.

Snapshots live in memory (8 slots) rather than on disk, so parking a point in
the timeline costs less than the frames themselves.

Example client config:

```json
{
  "mcpServers": {
    "cloopy": {
      "command": "/path/to/cloopy_headless",
      "args": ["/path/to/game.bin", "/path/to/loopy_bios.bin",
               "/path/to/loopy_sound.bin", "--mcp"]
    }
  }
}
```

# License

GPLv3+ except for MAME's SH1 core that is licensed differently.
My changes to code in general fall under either license.
LoopyMSE was licensed under the GPLv3+, this is derived from it (mostly the VDP core as well as the sound core).

`src/libretro/libretro.h` is from libretro-common, MIT, Copyright (C) 2010-2023 The RetroArch team.
The idle-loop skip is adapted from Gloopy (GPLv3, a LoopyMSE fork), reworked for this tree's SH-1 core.

# TODO

- Complete ADPCM sound chip emulation used for Wan Wan Story, right now it's a mess.
- Swap the sound core for something else. I have ideas but for now this is good enough. I may revisit this given circumstances.
- VDP core may need some improvements. Perhaps a re-basing from scratch from MAME's would be nice.
- SDL 1.2 core for low end devices like OpenDingux with RGB565 output support (we can't go lower as Casio Loopy can display 512 colors in total, using the two layers, possibly more with raster tricks)
- MCP server like GearSystem has for AI agents

Some of the tests in Makefile may need to be updated / broken, that's to be expected.

## Wanwan MSM6653A-457 ADPCM replacement bank

Wanwan Aijou Monogatari uses an OKI/Casio MSM6653A-457 cartridge ADPCM chip with an internal sample mask ROM. The internal mask ROM is not bundled.

A real dump of that mask ROM is expected soon, so the freely licensed replacement phrase bank is **no longer regenerated by default**. `make`, `make headless`, `make sdl3`, and the WASM makefile all link the checked-in bank in `src/sound/wanwan_oki_bank.c` and never invoke the generator.

To rebuild the replacement bank from `assets/wanwan_free_sounds/`, opt in explicitly:

```sh
make WANWAN_OKI_ROM_BUILD=1 wanwan-oki-rom
```

That regenerates `wanwan_synthetic_msm6653a_457.bin` and the embedded C bank in `src/sound/wanwan_oki_bank.c`. Passing `WANWAN_OKI_ROM_BUILD=1` to any other target restores the old behaviour of building the bank as part of the normal build.

A user-supplied real/private MSM6653A-compatible binary takes precedence over the built-in replacement. Native builds accept it as an explicit command-line argument after the normal sound ROM:

```sh
./cloopy_sdl3 game.bin loopy_bios.bin loopy_sound.bin /path/to/msm6653a_457.bin
# or:
./cloopy_sdl3 game.bin loopy_bios.bin loopy_sound.bin --oki-rom /path/to/msm6653a_457.bin
```

The browser build has an optional “Wanwan OKI sample ROM” picker in the ROM manager. If no OKI sample ROM is provided, the freely licensed replacement bank is used. 

`loopy_sound.bin` is also optional in the browser build; omitting it silences the internal music chip but does not block cartridge ADPCM.

Note : This will be tweaked later as it is currently a mess, and the sound samples don't correspond to what's being done in game.