# CLoopy

This started as a C11-only port of LoopyMSE.
It's got a few improvements :
- Can run on the web
- Limited printing support
- Attempting to be more CPU accurate and follow the documented behavior more closely by using MAME's SH-1 + a few improvements on top of it
- A few VDP fixes (there may be a few regressions), including emulation for the Controller bit. (that was one big annoyance i had encountered initially)

The main reason why i even made this, is because i kept encountering annoying crashes and issues on real hardware in regards to homebrew software.
I have since discovered... interesting issues, like the EXTS.W extension sign bug in Mame's core.
To be more accurate, this would need more intensive CPU tests + ROM/RAM wait states etc.. Right now, there is some but not really,
the speed of emulator != speed of console, but with my limited testing, it diverges less from real console than LoopyMSE would.

And also LoopyMSE would spam that terminal like crazy lol.

# License

GPLv3+ except for MAME's SH1 core that is licensed differently.
My changes to code in general fall under either license.
LoopyMSE was licensed under the GPLv3+, this is derived from it (mostly the VDP core as well as the sound core).

# TODO

- Add ADPCM sound chip used for Wan Wan Story.
- Swap the sound core for something else. I have ideas but for now this is good enough. I may revisit this given circumstances.
- VDP core may need some improvements. Perhaps a re-basing from scratch from MAME's would be nice.
- SDL 1.2 core for low end devices like OpenDingux with RGB565 output support (we can't go lower as Casio Loopy can display 512 colors in total, using the two layers, possibly more with raster tricks)
- MCP server like GearSystem has for AI agents

Some of the tests in Makefile may need to be updated / broken, that's to be expected.