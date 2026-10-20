"""Encode the reusable 16 kHz speech clips as 4-bit IMA ADPCM in ESP32 flash."""

from pathlib import Path
import sys
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from server.phone import VOICE_CLIPS  # noqa: E402

STEP = (7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
        34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
        143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
        449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282,
        1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
        3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
        9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
        22385, 24623, 27086, 29794, 32767)
INDEX = (-1, -1, -1, -1, 2, 4, 6, 8)


def encode(samples):
    predictor = 0
    index = 0
    encoded = bytearray()
    for position, sample in enumerate(samples):
        step = STEP[index]
        difference = sample - predictor
        nibble = 8 if difference < 0 else 0
        difference = abs(difference)
        change = step >> 3
        for mask, amount in ((4, step), (2, step >> 1), (1, step >> 2)):
            if difference >= amount:
                nibble |= mask
                difference -= amount
                change += amount
        predictor += -change if nibble & 8 else change
        predictor = max(-32768, min(32767, predictor))
        index = max(0, min(88, index + INDEX[nibble & 7]))
        if position & 1:
            encoded[-1] |= nibble << 4
        else:
            encoded.append(nibble)
    return encoded


def main():
    lines = [
        "#pragma once",
        "#include <Arduino.h>",
        "// Generated from server/assets/voice/*.wav by scripts/build_voice_header.py.",
        "struct VoiceClip { const uint8_t *data; uint32_t samples; };",
    ]
    total = 0
    for number, name in enumerate(VOICE_CLIPS):
        source = ROOT / "server" / "assets" / "voice" / f"{name}.wav"
        with wave.open(str(source), "rb") as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (1, 2, 16000):
                raise ValueError(f"Expected 16 kHz mono 16-bit PCM: {source}")
            raw = wav.readframes(wav.getnframes())
        import array
        samples = array.array("h")
        samples.frombytes(raw)
        if sys.byteorder != "little":
            samples.byteswap()
        data = encode(samples)
        total += len(data)
        lines.append(f"// {number}: {name}")
        lines.append(f"const uint8_t VOICE_{number}[] PROGMEM = {{")
        for offset in range(0, len(data), 24):
            lines.append("  " + ", ".join(str(value) for value in data[offset:offset + 24]) + ",")
        lines.append("};")
    lines.append(f"constexpr uint8_t VOICE_CLIP_COUNT = {len(VOICE_CLIPS)};")
    lines.append("const VoiceClip VOICE_CLIP_TABLE[] = {")
    for number, name in enumerate(VOICE_CLIPS):
        with wave.open(str(ROOT / "server" / "assets" / "voice" / f"{name}.wav"), "rb") as wav:
            frames = wav.getnframes()
        lines.append(f"  {{VOICE_{number}, {frames}}}, // {name}")
    lines.append("};")
    target = ROOT / "client" / "src" / "voice_adpcm.h"
    target.write_text("\n".join(lines) + "\n")
    print(f"Embedded {len(VOICE_CLIPS)} clips in {total:,} compressed bytes: {target}")


if __name__ == "__main__":
    main()
