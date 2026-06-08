# CLoopy

This started as a C11-only port of LoopyMSE.
It's got a few improvements :
- Can run on the web
- Limited printing support
- Attempting to be more CPU accurate and follow the documented behavior more closely by using MAME's SH-1 
- A few VDP fixes (there may be a few regressions), including emulation for the Controller bit. (that was one big annoyance i had encountered initially)

The main reason why i even made this, is because i kept encountering annoying crashes and issues on real hardware in regards to homebrew software.

To be more accurate, this would need more intensive CPU tests + ROM/RAM wait states etc.. Right now, there is some but not really,
the speed of emulator != speed of console, but with my limited testing, it diverges less from real console than LoopyMSE would.

And also LoopyMSE would spam that terminal like crazy lol.

# License

GPLv3+ except for MAME's SH1 core that is licensed differently.
My changes to code in general fall under either license.
LoopyMSE was licensed under the GPLv3+, this is derived from it (mostly the VDP core as well as the sound core).

# TODO

- Complete ADPCM sound chip emulation used for Wan Wan Story, right now it's a mess.
- Swap the sound core for something else. I have ideas but for now this is good enough. I may revisit this given circumstances.
- VDP core may need some improvements. Perhaps a re-basing from scratch from MAME's would be nice.
- SDL 1.2 core for low end devices like OpenDingux with RGB565 output support (we can't go lower as Casio Loopy can display 512 colors in total, using the two layers, possibly more with raster tricks)
- MCP server like GearSystem has for AI agents

Some of the tests in Makefile may need to be updated / broken, that's to be expected.

## Wanwan MSM6653A-457 ADPCM replacement bank

Wanwan Aijou Monogatari uses an OKI/Casio MSM6653A-457 cartridge ADPCM chip with an internal sample mask ROM. The internal mask ROM is not bundled. 
The emulator therefore builds a freely licensed replacement phrase bank from `assets/wanwan_free_sounds/` by default:

```sh
make wanwan-oki-rom
```

`make`, `make headless`, `make sdl3`, and the WASM makefile all depend on that generated bank. The generated file is `wanwan_synthetic_msm6653a_457.bin`, and the embedded C bank is regenerated in `src/sound/wanwan_oki_bank.c`.

A user-supplied real/private MSM6653A-compatible binary takes precedence over the built-in replacement. Native builds accept it as an explicit command-line argument after the normal sound ROM:

```sh
./cloopy_sdl3 game.bin loopy_bios.bin loopy_sound.bin /path/to/msm6653a_457.bin
# or:
./cloopy_sdl3 game.bin loopy_bios.bin loopy_sound.bin --oki-rom /path/to/msm6653a_457.bin
```

The browser build has an optional “Wanwan OKI sample ROM” picker in the ROM manager. If no OKI sample ROM is provided, the freely licensed replacement bank is used. 

`loopy_sound.bin` is also optional in the browser build; omitting it silences the internal music chip but does not block cartridge ADPCM.

Note : This will be tweaked later as it is currently a mess, and the sound samples don't correspond to what's being done in game.