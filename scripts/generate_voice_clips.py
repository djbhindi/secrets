"""Generate every reusable Cori speech clip before running the phone service."""

from pathlib import Path
import wave

from piper import PiperVoice

from voice import DEFAULT_MODEL, RATE, ROOT, render_16k, soften_low_mids


PHRASES = {
    "record_after_beep": "Leave your message after the beep.",
    "store_full": "The message store is full.",
    "here_message": "Here is your message.",
    "your_code": "Your message is saved. Your three digit code is",
    "repeat": "I repeat:",
    "you_dialed": "You dialed",
    "no_message": "There is no message for that code. Please try again.",
}
DIGITS = dict(zip("123456789", "one two three four five six seven eight nine".split()))


def main():
    voice = PiperVoice.load(str(DEFAULT_MODEL))
    destination = ROOT / "server" / "assets" / "voice"
    destination.mkdir(parents=True, exist_ok=True)
    phrases = dict(PHRASES)
    phrases.update({f"digit_{digit}": word for digit, word in DIGITS.items()})
    for name, words in phrases.items():
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
