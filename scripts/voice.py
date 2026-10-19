"""Render local Piper speech as 16 kHz mono PCM for the phone speaker."""

from pathlib import Path
import subprocess
import tempfile
import wave

import numpy as np


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MODEL = ROOT / "voices/en_GB-cori-medium.onnx"
RATE = 16000
LEAD_SILENCE_MS = 200
TAIL_SILENCE_MS = 80


def render_16k(text, voice, lead_silence_ms=LEAD_SILENCE_MS):
    with tempfile.TemporaryDirectory(prefix="rotary-piper-") as directory:
        native = Path(directory) / "native.wav"
        converted = Path(directory) / "phone.wav"
        with wave.open(str(native), "wb") as output:
            voice.synthesize_wav(text, output)
        subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI16@16000",
                        str(native), str(converted)], check=True)
        with wave.open(str(converted), "rb") as source:
            if (source.getnchannels(), source.getsampwidth(), source.getframerate()) != (1, 2, 16000):
                raise ValueError("Piper conversion did not produce 16 kHz mono PCM")
            raw = source.readframes(source.getnframes())
        if not raw:
            raise RuntimeError("Piper generated no audio")
        lead = bytes(RATE * 2 * lead_silence_ms // 1000)
        tail = bytes(RATE * 2 * TAIL_SILENCE_MS // 1000)
        return lead + raw + tail


def soften_low_mids(raw):
    """Reduce the 200–500 Hz excess heard on short phone-speaker phrases."""
    samples = np.frombuffer(raw, dtype="<i2").astype(np.float64)
    frequencies = np.fft.rfftfreq(len(samples), 1 / RATE)
    # About -3.5 dB at 300 Hz, tapering to negligible change above 1 kHz.
    response = 1 - 0.34 / (1 + (frequencies / 650) ** 4)
    adjusted = np.fft.irfft(np.fft.rfft(samples) * response, n=len(samples))
    return np.clip(np.rint(adjusted), -32768, 32767).astype("<i2").tobytes()
