"""Generate the ESP32's embedded opening prompt with the local Piper voice."""

from pathlib import Path
import wave

from piper import PiperVoice

from voice import DEFAULT_MODEL, ROOT, render_16k, soften_low_mids


INTRO = "This is the Secrets Hotline."
INSTRUCTIONS = ("Dial zero to leave a message. "
                "To hear a message, dial your three digit code.")


voice = PiperVoice.load(str(DEFAULT_MODEL))
parts = {
    "prompt_intro.wav": render_16k(INTRO, voice),
    "prompt_menu.wav": soften_low_mids(render_16k(INSTRUCTIONS, voice)),
}
for filename, raw in parts.items():
    output_path = ROOT / "client" / "audio" / filename
    with wave.open(str(output_path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(16000)
        output.writeframes(raw)
    print(f"Generated {len(raw) / 32000:.2f} seconds in {output_path}")
