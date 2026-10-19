"""Embed a 16 kHz mono 16-bit WAV prompt in ESP32 firmware flash."""

from pathlib import Path
import struct
import wave


target = Path("client/src/prompt_pcm.h")
lines = [
    "#pragma once",
    "#include <Arduino.h>",
    "// Generated from 16 kHz mono PCM prompt WAV files.",
]
for source, symbol in (
    (Path("client/audio/prompt_intro.wav"), "PROMPT_INTRO_PCM"),
    (Path("client/audio/prompt_menu.wav"), "PROMPT_MENU_PCM"),
):
    with wave.open(str(source), "rb") as wav:
        if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (1, 2, 16000):
            raise ValueError(f"Prompt must be 16 kHz mono 16-bit PCM WAV: {source}")
        raw = wav.readframes(wav.getnframes())
    samples = struct.unpack(f"<{len(raw) // 2}h", raw)
    lines.append(f"const int16_t {symbol}[] PROGMEM = {{")
    for offset in range(0, len(samples), 16):
        lines.append("  " + ", ".join(map(str, samples[offset:offset + 16])) + ",")
    lines.append("};")
    print(f"Embedded {len(samples) / 16000:.2f} seconds from {source}")
target.write_text("\n".join(lines) + "\n")
print(f"Wrote {target}")
