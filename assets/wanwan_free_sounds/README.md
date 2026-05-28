# Wanwan freely licensed replacement OKI assets

These files are replacement inputs for the synthetic MSM6653A-457 phrase ROM used by the emulator when a real Wanwan OKI sample ROM is not supplied. They are not dumps or reconstructions of the original Casio/OKI mask ROM.

All files in this folder are derived from the user's provided `free_sounds.zip`, which was curated from CC0/public-domain sound-effect sources for this purpose. The generated ROM should therefore be considered a freely licensed approximation, not an authentic game asset dump.

Source groups used:

- Freesound CC0 dog bark: `850822__sadiquecat__dog-bark-1.wav`
- User-supplied CC0 dog-bark extracts: `bark_one_shot.wav`, `alt_bark_one_shot.wav`, `alt_dog_single_bark.wav`
- OpenGameArt CC0 BookFlip SFX: `BookFlip2.wav`, `replacement/BookFlip8.wav`
- OpenGameArt CC0 100 SFX: `paper_01.ogg`, `paper_03.ogg`, `paper_04.ogg`, `machine_01.ogg`, `machine_02.ogg`, `weird_03.ogg`, `plop_01.ogg`, `explosion.ogg`
- OpenGameArt CC0 bang/firework SFX: `ex/bang_04.ogg`

`cmd16_two_bark_replacement.wav` is a composite made only from the CC0 `bark_one_shot.wav`, with two barks spaced to approximate the timing of the game event triggered by command `0x16`.
