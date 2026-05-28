# Wanwan OKI ADPCM ROM editor

This folder contains a dependency-free browser editor for generating a replacement OKI MSM6653A-457 phrase ROM binary for Wanwan Aijou Monogatari.

Open `index.html` in a modern browser. Users can upload WAV, MP3, OGG, FLAC, M4A, or other audio formats supported by the browser's `decodeAudioData()` implementation. The editor mixes to mono, trims optional silence, applies a DC blocker, resamples to the selected OKI rate, encodes to the same 4-bit OKI ADPCM model used by CLoopy, and exports a 128-entry phrase-table ROM binary.

Only command slots 0x13 and 0x16 are named in the UI because they are grounded by save-state traces. Other command slots are shown as guesses, using assumptions from the earlier original-sample categories rather than the free replacement pack.


## Importing an existing binary ROM

Use **Import ROM .bin** to load an existing OKI phrase-table binary. The editor reads the 128-entry table at the start of the file, imports Wanwan command slots 0x01 through 0x16, decodes each phrase through the same OKI ADPCM decoder used for preview, and keeps the phrase bytes intact unless the slot is replaced with newly uploaded audio. After edits, **Export ROM .bin** rebuilds a compact phrase-table binary.
