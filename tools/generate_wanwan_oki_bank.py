#!/usr/bin/env python3
"""Generate a freely licensed Wanwan MSM6653A-457 replacement ADPCM ROM.

The MSM6653A-457 internal mask ROM cannot be dumped through the Loopy cart.
This utility builds a replacement phrase table from independently licensed
replacement WAV assets under assets/wanwan_free_sounds by default. It is not
raw WAV playback: every supplied WAV is resampled, OKI-ADPCM encoded, packed
into the phrase ROM block format, and then the emulator decodes it through the
same OKI path used by the game.

Known command positions from save-state traces:
  command 0x13 -> one-bark/dog-effect slot seen in Wanwan .ls2
  command 0x16 -> two-bark/dog-effect slot seen in Wanwan .ls0

The audio in the default generated bank is an approximation made from CC0/
public-domain replacement assets. A user who has a private real or more accurate
MSM6653A-compatible binary can load it externally; the emulator will use that
instead of this built-in replacement.
"""
from __future__ import annotations

import argparse
import math
import struct
import wave
from pathlib import Path
from typing import Iterable

INDEX_SHIFT = [-1, -1, -1, -1, 2, 4, 6, 8]
NBL2BIT = [
    ( 1, 0, 0, 0), ( 1, 0, 0, 1), ( 1, 0, 1, 0), ( 1, 0, 1, 1),
    ( 1, 1, 0, 0), ( 1, 1, 0, 1), ( 1, 1, 1, 0), ( 1, 1, 1, 1),
    (-1, 0, 0, 0), (-1, 0, 0, 1), (-1, 0, 1, 0), (-1, 0, 1, 1),
    (-1, 1, 0, 0), (-1, 1, 0, 1), (-1, 1, 1, 0), (-1, 1, 1, 1),
]
DIFF_LOOKUP: list[list[int]] = []
for step in range(49):
    stepval = int(math.floor(16.0 * ((11.0 / 10.0) ** step)))
    row: list[int] = []
    for sign, b1, b2, b3 in NBL2BIT:
        row.append(sign * (stepval * b1 + (stepval // 2) * b2 + (stepval // 4) * b3 + stepval // 8))
    DIFF_LOOKUP.append(row)

TARGET_RATE = 8000
TABLE_COMMANDS = 128
GROUND_TARGET_PEAK = 1450
GUESS_TARGET_PEAK = 1050

# Known and guessed command layout using freely licensed replacement assets.
# 0x13 and 0x16 are known command positions from save-state traces; the sounds
# assigned here are CC0/public-domain approximations, not original mask-ROM data.
COMMAND_SAMPLE_NAMES: dict[int, str] = {
    0x01: "cmd01_dog_bark_short.wav",
    0x02: "cmd02_dog_bark_alt.wav",
    0x03: "cmd03_dog_bark_single.wav",
    0x04: "cmd04_dog_bark_cc0.wav",
    0x05: "cmd05_bush_rustle.wav",
    0x06: "cmd06_pageflip.wav",
    0x07: "cmd07_ship_machine.wav",
    0x08: "cmd08_explosion.wav",
    0x09: "cmd09_dog_bark_low.wav",
    0x0A: "cmd0A_dog_complain.wav",
    0x0B: "cmd0B_dog_disappointed.wav",
    0x0C: "cmd0C_dog_grunt.wav",
    0x0D: "cmd0D_bush_rustle_alt.wav",
    0x0E: "cmd0E_pageflip_alt.wav",
    0x0F: "cmd0F_ship_machine_alt.wav",
    0x10: "cmd10_bang.wav",
    0x11: "cmd11_dog_bark_yip.wav",
    0x12: "cmd12_dog_complain_alt.wav",
    0x13: "cmd13_dog_bark2_replacement.wav",
    0x14: "cmd14_rustle_alt.wav",
    0x15: "cmd15_dog_disappointed_alt.wav",
    0x16: "cmd16_two_bark_replacement.wav",
}
TRACE_KNOWN_COMMANDS = {0x13, 0x16}
# Small transformations only for duplicate guessed commands so the montage is
# easier to audit by ear; grounded commands are never pitch-shifted.
COMMAND_PITCH: dict[int, float] = {
    0x09: 0.88,
    0x0A: 1.12,
    0x0B: 0.92,
    0x0C: 1.08,
    0x0D: 0.82,
    0x0E: 1.18,
    0x0F: 0.86,
    0x10: 1.14,
    0x11: 1.24,
    0x12: 0.76,
    0x14: 1.28,
    0x15: 0.80,
}


def read_wav_mono(path: Path) -> tuple[int, list[float]]:
    with wave.open(str(path), "rb") as w:
        rate = w.getframerate()
        channels = w.getnchannels()
        width = w.getsampwidth()
        frames = w.getnframes()
        raw = w.readframes(frames)
    if width != 2:
        raise ValueError(f"{path}: expected 16-bit PCM WAV; got {width * 8}-bit")
    values = struct.unpack("<" + "h" * (len(raw) // 2), raw)
    if channels == 1:
        return rate, [float(v) for v in values]
    mono: list[float] = []
    for i in range(0, len(values), channels):
        mono.append(sum(values[i:i + channels]) / float(channels))
    return rate, mono


def moving_average_abs(data: list[float], radius: int) -> list[float]:
    if not data:
        return []
    radius = max(1, radius)
    out: list[float] = []
    acc = 0.0
    q: list[float] = []
    for v in data:
        q.append(abs(v))
        acc += abs(v)
        if len(q) > radius:
            acc -= q.pop(0)
        out.append(acc / float(len(q)))
    return out


def trim_active(rate: int, data: list[float], *, threshold_factor: float = 0.025, pad_ms: float = 12.0) -> list[float]:
    if not data:
        return data
    mean = sum(data) / float(len(data))
    centered = [v - mean for v in data]
    env = moving_average_abs(centered, max(1, int(rate * 0.006)))
    peak = max(env) if env else 0.0
    threshold = max(45.0, peak * threshold_factor)
    active = [i for i, v in enumerate(env) if v > threshold]
    if not active:
        return centered
    pad = int(rate * pad_ms / 1000.0)
    start = max(0, active[0] - pad)
    stop = min(len(centered), active[-1] + pad)
    return centered[start:stop]


def highpass_dc(data: list[float]) -> list[float]:
    if not data:
        return data
    # Lightweight one-pole DC blocker, adequate for the noisy line captures.
    out: list[float] = []
    prev_x = 0.0
    prev_y = 0.0
    r = 0.995
    for x in data:
        y = x - prev_x + r * prev_y
        out.append(y)
        prev_x = x
        prev_y = y
    return out


def resample_linear(data: list[float], src_rate: int, dst_rate: int) -> list[float]:
    if not data or src_rate == dst_rate:
        return list(data)
    out_len = max(1, int(round(len(data) * dst_rate / float(src_rate))))
    out: list[float] = []
    scale = src_rate / float(dst_rate)
    last = len(data) - 1
    for i in range(out_len):
        pos = i * scale
        j = int(pos)
        frac = pos - j
        if j >= last:
            out.append(float(data[last]))
        else:
            out.append(float(data[j]) * (1.0 - frac) + float(data[j + 1]) * frac)
    return out


def pitch_time(data: list[float], pitch: float) -> list[float]:
    if pitch == 1.0 or len(data) < 2:
        return list(data)
    new_len = max(8, int(round(len(data) / pitch)))
    out: list[float] = []
    last = len(data) - 1
    for i in range(new_len):
        pos = i * last / float(max(1, new_len - 1))
        j = int(pos)
        frac = pos - j
        if j >= last:
            out.append(float(data[last]))
        else:
            out.append(float(data[j]) * (1.0 - frac) + float(data[j + 1]) * frac)
    return out


def normalize_peak(data: list[float], target_peak: int) -> list[int]:
    if not data:
        return []
    mean = sum(data) / float(len(data))
    centered = [v - mean for v in data]
    peak = max(max(centered), -min(centered), 1.0)
    scale = float(target_peak) / peak
    fade = min(len(centered) // 8, 80)
    out: list[int] = []
    for i, v in enumerate(centered):
        mul = 1.0
        if fade > 1 and i < fade:
            mul = i / float(fade)
        elif fade > 1 and i >= len(centered) - fade:
            mul = (len(centered) - 1 - i) / float(fade)
        x = int(round(v * scale * max(0.0, min(1.0, mul))))
        out.append(max(-2048, min(2047, x)))
    return out


def preprocess(path: Path, *, target_peak: int, pitch: float, max_ms: float | None) -> list[int]:
    rate, data = read_wav_mono(path)
    data = trim_active(rate, data)
    data = highpass_dc(data)
    data = resample_linear(data, rate, TARGET_RATE)
    data = pitch_time(data, pitch)
    if max_ms is not None:
        data = data[:max(8, int(round(TARGET_RATE * max_ms / 1000.0)))]
    return normalize_peak(data, target_peak)


def clock(signal: int, step: int, nibble: int) -> tuple[int, int]:
    signal += DIFF_LOOKUP[step][nibble & 15]
    signal = max(-2048, min(2047, signal))
    step += INDEX_SHIFT[nibble & 7]
    step = max(0, min(48, step))
    return signal, step


def encode_pcm(pcm: list[int]) -> list[int]:
    signal = -2
    step = 0
    out: list[int] = []
    for target in pcm:
        best_nibble = 0
        best_error = 1 << 30
        best_signal = signal
        best_step = step
        for nibble in range(16):
            predicted_signal, predicted_step = clock(signal, step, nibble)
            error = abs(target - predicted_signal)
            if error < best_error:
                best_error = error
                best_nibble = nibble
                best_signal = predicted_signal
                best_step = predicted_step
        out.append(best_nibble)
        signal = best_signal
        step = best_step
    return out


def decode_nibbles(nibbles: Iterable[int]) -> list[int]:
    signal = -2
    step = 0
    out: list[int] = []
    for nibble in nibbles:
        signal, step = clock(signal, step, nibble)
        out.append(signal)
    return out


def pack_phrase(nibbles: list[int]) -> bytes:
    out = bytearray()
    index = 0
    while index < len(nibbles):
        count = min(254, len(nibbles) - index)
        if count & 1:
            count -= 1
        if count <= 0:
            break
        out.append(count // 2)
        for offset in range(0, count, 2):
            out.append(((nibbles[index + offset] & 15) << 4) | (nibbles[index + offset + 1] & 15))
        index += count
    out.append(0)
    return bytes(out)


def similarity_score(target: list[int], decoded: list[int]) -> float:
    n = min(len(target), len(decoded))
    if n <= 16:
        return 1e18
    t = target[:n]
    d = decoded[:n]
    # Remove DC and compare normalized shape. Penalize duration mismatch.
    mt = sum(t) / float(n)
    md = sum(d) / float(n)
    vt = math.sqrt(sum((x - mt) * (x - mt) for x in t) / float(n)) or 1.0
    vd = math.sqrt(sum((x - md) * (x - md) for x in d) / float(n)) or 1.0
    err = 0.0
    for a, b in zip(t, d):
        err += (((a - mt) / vt) - ((b - md) / vd)) ** 2
    err = math.sqrt(err / float(n))
    err += abs(len(target) - len(decoded)) / float(max(len(target), len(decoded), 1))
    return err


def best_encoded_phrase(path: Path, grounded: bool, base_pitch: float) -> tuple[bytes, int, int, float, int, float]:
    # The generator scores encode->decode against each candidate preprocessed
    # waveform and keeps the closest ADPCM representation.
    peaks = [GROUND_TARGET_PEAK] if grounded else [900, 1050, 1250]
    pitches = [1.0] if grounded else sorted(set([base_pitch, 1.0]))
    max_ms_values: list[float | None] = [None] if grounded else [520.0, None]
    best: tuple[float, bytes, int, int, float, int] | None = None
    for peak in peaks:
        for pitch in pitches:
            for max_ms in max_ms_values:
                pcm = preprocess(path, target_peak=peak, pitch=pitch, max_ms=max_ms)
                nibbles = encode_pcm(pcm)
                decoded = decode_nibbles(nibbles)
                score = similarity_score(pcm, decoded)
                phrase = pack_phrase(nibbles)
                if best is None or score < best[0]:
                    best = (score, phrase, len(pcm), peak, pitch, len(decoded))
    assert best is not None
    score, phrase, pcm_len, peak, pitch, decoded_len = best
    return phrase, pcm_len, len(phrase), score, peak, pitch


def c_array(data: bytes | bytearray, name: str, width: int = 12) -> str:
    lines = [f"const uint8_t {name}[] = {{"]
    for offset in range(0, len(data), width):
        chunk = data[offset:offset + width]
        suffix = "," if offset + width < len(data) else ""
        lines.append("    " + ", ".join(f"0x{byte:02X}" for byte in chunk) + suffix)
    lines.append("};")
    return "\n".join(lines)


def build_rom(sample_dir: Path, no_placeholders: bool) -> tuple[bytes, list[str]]:
    phrases: dict[int, bytes] = {}
    report: list[str] = []
    for command in range(1, 23):
        name = COMMAND_SAMPLE_NAMES.get(command)
        if not name:
            continue
        path = sample_dir / name
        if not path.exists():
            if no_placeholders:
                continue
            # Fall back to the clean dog bark rather than address-zero silence.
            path = sample_dir / "cmd01_dog_bark_short.wav"
        grounded = command in TRACE_KNOWN_COMMANDS
        pitch = 1.0 if grounded else COMMAND_PITCH.get(command, 1.0)
        phrase, pcm_len, phrase_len, score, peak, chosen_pitch = best_encoded_phrase(path, grounded, pitch)
        phrases[command] = phrase
        tag = "trace-known replacement" if grounded else "free replacement guess"
        line = (f"0x{command:02X}: {name} [{tag}], peak={peak}, pitch={chosen_pitch:.3f}, "
                f"pcm={pcm_len}, phrase={phrase_len}, score={score:.4f}")
        print(line)
        report.append(line)

    rom = bytearray(TABLE_COMMANDS * 4)
    pos = len(rom)
    for command in sorted(phrases):
        while pos & 0xF:
            rom.append(0)
            pos += 1
        start = pos
        rom.extend(phrases[command])
        pos += len(phrases[command])
        rom[command * 4 + 0] = (start >> 16) & 0xFF
        rom[command * 4 + 1] = (start >> 8) & 0xFF
        rom[command * 4 + 2] = start & 0xFF
        rom[command * 4 + 3] = 0
        print(f"command 0x{command:02X}: table start 0x{start:06X}")
    return bytes(rom), report


def write_bank(source_dir: Path, rom: bytes, report: list[str]) -> None:
    source_dir.mkdir(parents=True, exist_ok=True)
    report_text = "\n".join(" * " + line for line in report)
    c_text = f'''#include "sound/wanwan_oki_bank.h"

#include <stdint.h>

/*
 * Freely licensed replacement MSM6653A-457 phrase ROM for
 * Wanwan Aijou Monogatari.
 *
 * This is not a dump of the internal OKI mask ROM, and it is not derived
 * from captured commercial game audio. It is a compatibility replacement
 * built from CC0/public-domain sound effects under assets/wanwan_free_sounds.
 * A user-supplied external binary ROM takes precedence at runtime.
 *
 * Current generated mapping and encode/decode scoring:
{report_text}
 *
 * Table format used by this emulator: 128 entries at ROM offset 0, four
 * bytes per command, containing a 24-bit big-endian phrase start address
 * followed by one reserved byte. Phrases are [count][ADPCM bytes] blocks
 * terminated by a zero count, matching the OKIM6376-style block stream.
 */
'''
    c_text += c_array(rom, "wanwan_oki_rom")
    c_text += f"\n\nconst uint32_t wanwan_oki_rom_size = {len(rom)}u;\n"
    (source_dir / "wanwan_oki_bank.c").write_text(c_text)
    (source_dir / "wanwan_oki_bank.h").write_text('''#ifndef LOOPY_WANWAN_OKI_BANK_H
#define LOOPY_WANWAN_OKI_BANK_H

#include <stdint.h>

extern const uint8_t wanwan_oki_rom[];
extern const uint32_t wanwan_oki_rom_size;

#endif
''')

def write_order(path: Path, report: list[str]) -> None:
    lines = [
        "Wanwan freely licensed replacement MSM6653A-457 command order",
        "",
        "Known command positions from save-state traces:",
        "  0x13 -> cmd13_dog_bark2_replacement.wav (.ls2 replacement)",
        "  0x16 -> cmd16_two_bark_replacement.wav (.ls0 replacement)",
        "",
        "All default audio is CC0/public-domain replacement material, not original mask-ROM data.",
        "Full generated table:",
    ]
    lines.extend("  " + line for line in report)
    path.write_text("\n".join(lines) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sample-dir", default=Path("assets/wanwan_free_sounds"), type=Path,
                        help="directory containing freely licensed Wanwan replacement WAVs")
    parser.add_argument("--source-dir", default=Path("src/sound"), type=Path)
    parser.add_argument("--bin", default=Path("wanwan_synthetic_msm6653a_457.bin"), type=Path)
    parser.add_argument("--order", default=Path("wanwan_synthetic_msm6653a_457_order.txt"), type=Path)
    parser.add_argument("--no-placeholders", action="store_true", help="only emit commands with explicit mapped files")
    args = parser.parse_args()

    rom, report = build_rom(args.sample_dir, args.no_placeholders)
    args.bin.write_bytes(rom)
    write_bank(args.source_dir, rom, report)
    write_order(args.order, report)
    print(f"wrote {len(rom)} bytes to {args.bin}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
