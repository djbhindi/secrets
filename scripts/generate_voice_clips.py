"""Generate every reusable Cori speech clip before running the phone service."""

from pathlib import Path
import wave

import numpy as np
from piper import PiperVoice

from voice import DEFAULT_MODEL, RATE, ROOT, render_16k, soften_low_mids


PHRASES = {
    "record_after_beep": "Record a message up to two minutes long after the beep, or dial zero when you're done.",
    "store_full": "The message store is full.",
    "here_message": "Here is your message.",
    "your_code": "Your message is saved. Your three digit code is",
    "repeat": "I repeat:",
    "you_dialed": "You dialed",
    "no_message": "There is no message for that code. Please try again.",
}
DIGITS = dict(zip("123456789", "one two three four five six seven eight nine".split()))


def contextual_eight():
    """Use the accepted Cori pronunciation from 'The number eight'."""
    source = ROOT / "server" / "assets" / "voice_sources" / "digit_8_context.wav"
    with wave.open(str(source), "rb") as wav:
        if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (1, 2, RATE):
            raise ValueError(f"Expected 16 kHz mono 16-bit PCM: {source}")
        samples = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").copy()
    # 'The number' ends at 0.58 s; 'eight' ends around 0.94 s.
    selected = samples[int(0.585 * RATE):int(1.25 * RATE)]
    fade = min(int(0.01 * RATE), len(selected))
    selected[:fade] = np.rint(selected[:fade] * np.linspace(0, 1, fade)).astype("<i2")
    return selected.tobytes()


def main():
    voice = PiperVoice.load(str(DEFAULT_MODEL))
    destination = ROOT / "server" / "assets" / "voice"
    destination.mkdir(parents=True, exist_ok=True)
    phrases = dict(PHRASES)
    phrases.update({f"digit_{digit}": word for digit, word in DIGITS.items()})
    for name, words in phrases.items():
        if name == "digit_8":
            raw = contextual_eight()
        else:
            raw = render_16k(words, voice, lead_silence_ms=0)
            if name.startswith("digit_"):
                raw = soften_low_mids(raw)
        with wave.open(str(destination / f"{name}.wav"), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(RATE)
            output.writeframes(raw)
        print(f"{name}: {len(raw) / (RATE * 2):.2f} s")


if __name__ == "__main__":
    main()
