"""Run the rotary phone answering system, storing messages on this computer."""

import argparse
from array import array
from collections import deque
from itertools import product
from pathlib import Path
import re
import secrets
import socket
import struct
import sys
import time
import wave

RATE = 16000
RECORD_SECONDS = 10
MENU_REPEAT_SECONDS = 15
HEARTBEAT_SECONDS = 5
HEARTBEAT_TIMEOUT_SECONDS = 3
PLAY_CHUNK_BYTES = 1024 * 2
PHONE_PORT = 45679
ESP32_AP_HOST = "192.168.4.1"
DIGITS = "123456789"
DIALED = re.compile(r"^DIALED ([1-9]) \(\d+ edges; turn (\d+)\)$")
TRIGGER = re.compile(r"^TRIGGER \(20 edges; turn (\d+)\)$")
SERVER_DIR = Path(__file__).resolve().parent
VOICE_CLIPS = ("record_after_beep", "store_full", "here_message",
               "your_code", "repeat", "you_dialed", "no_message",
               *(f"digit_{digit}" for digit in DIGITS))


class Hangup(Exception):
    """The handset was placed on the cradle during an operation."""


class SocketPort:
    """Provide the small stream interface used by Phone over a TCP socket."""

    def __init__(self, connection):
        self.connection = connection
        self.buffer = bytearray()

    def _receive(self):
        try:
            chunk = self.connection.recv(4096)
        except socket.timeout:
            return False
        if not chunk:
            raise ConnectionError("ESP32 disconnected")
        self.buffer.extend(chunk)
        return True

    def read(self, count):
        if not self.buffer and not self._receive():
            return b""
        result = bytes(self.buffer[:count])
        del self.buffer[:count]
        return result

    def readline(self):
        while b"\n" not in self.buffer:
            if not self._receive():
                return b""
        end = self.buffer.index(10) + 1
        result = bytes(self.buffer[:end])
        del self.buffer[:end]
        return result

    def write(self, data):
        self.connection.sendall(data)
        return len(data)

    def flush(self):
        pass


def read_exact(port, count):
    result = bytearray()
    while len(result) < count:
        chunk = port.read(count - len(result))
        if not chunk:
            raise TimeoutError(f"Audio stream stopped after {len(result)} of {count} bytes")
        result.extend(chunk)
    return bytes(result)


def pcm_samples(raw):
    values = array("h")
    values.frombytes(raw)
    if sys.byteorder != "little":
        values.byteswap()
    return values


def pcm_bytes(values):
    if sys.byteorder == "little":
        return values.tobytes()
    copy = array("h", values)
    copy.byteswap()
    return copy.tobytes()


def save_wav(path, raw):
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(RATE)
        output.writeframes(raw)


def load_wav(path):
    with wave.open(str(path), "rb") as source:
        if (source.getnchannels(), source.getsampwidth(), source.getframerate()) != (1, 2, RATE):
            raise ValueError(f"Expected 16 kHz mono 16-bit WAV: {path}")
        return source.readframes(source.getnframes())


def listening_copy(raw):
    samples = pcm_samples(raw)
    settled = samples[int(RATE * 0.15):] or samples
    mean = round(sum(settled) / len(settled)) if settled else 0
    peak = max((abs(sample - mean) for sample in settled), default=0)
    gain = min(8.0, 7000 / peak) if peak else 1.0
    result = array("h", (max(-32768, min(32767, round((sample - mean) * gain)))
                         for sample in samples))
    return pcm_bytes(result), gain


class Phone:
    def __init__(self, port, message_dir):
        self.port = port
        self.message_dir = message_dir
        self.message_dir.mkdir(parents=True, exist_ok=True)
        self.available_keys = ["".join(parts) for parts in product(DIGITS, repeat=3)
                               if not (self.message_dir / ("".join(parts) + ".wav")).exists()]
        secrets.SystemRandom().shuffle(self.available_keys)
        self.pending = deque()
        self.offhook = False
        self.entered = ""
        self.last_turn_id = 0
        self.last_logged_line = ""
        self.menu_repeat_at = None
        self.serial_pacing = False

    def command(self, letter):
        self.port.write(b"RPH1:" + letter.encode("ascii") + b"\n")

    def line(self):
        raw = self.port.readline()
        if not raw:
            return ""
        line = raw.decode("utf-8", errors="replace").strip()
        if (line and line != self.last_logged_line and line != "NEXT"
                and not line.startswith("Dial RR:")):
            print(line, flush=True)
        if line and line != "NEXT" and not line.startswith("Dial RR:"):
            self.last_logged_line = line
        return line

    def wait_for(self, expected, seconds=20):
        expected_lines = (expected,) if isinstance(expected, str) else tuple(expected)
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            line = self.line()
            if line in expected_lines:
                return line
            if line == "ABORTED":
                raise Hangup
            if line.startswith(("Cradle ", "DIALED ", "TRIGGER")):
                if not self.pending or self.pending[-1] != line:
                    self.pending.append(line)
            if line.endswith("ERROR"):
                raise RuntimeError(line)
        raise TimeoutError(f"ESP32 did not respond with {expected}")

    def speak_clips(self, *names):
        if not 0 < len(names) <= 16:
            raise ValueError("Expected 1 to 16 speech clips")
        identifiers = bytes(VOICE_CLIPS.index(name) for name in names)
        self.command("C")
        self.port.write(bytes((len(identifiers),)) + identifiers)
        self.wait_for("CLIPS PLAYED", seconds=30)

    def play(self, raw, intro_clip=None):
        count = len(raw) // 2
        if not 0 < count <= RATE * 30:
            raise ValueError("Playback must be between 0 and 30 seconds")
        intro_id = 255 if intro_clip is None else VOICE_CLIPS.index(intro_clip)
        self.command("P")
        self.port.write(struct.pack("<IB", count, intro_id))
        if self.serial_pacing:
            for offset in range(0, len(raw), 256):
                self.wait_for("NEXT", seconds=10)
                self.port.write(raw[offset:offset + 256])
        else:
            for offset in range(0, len(raw), PLAY_CHUNK_BYTES):
                self.port.write(raw[offset:offset + PLAY_CHUNK_BYTES])
        self.wait_for("PLAYED", seconds=max(20, count / RATE + 10))

    def beep(self):
        self.command("b")
        self.port.flush()
        self.wait_for("BEEPED")

    def opening_prompt(self):
        self.command("o")
        self.port.flush()
        return self.wait_for(("MENU PLAYED", "MENU SKIPPED"), seconds=15)

    def repeat_menu_if_due(self):
        if not self.offhook or self.menu_repeat_at is None:
            return
        if time.monotonic() < self.menu_repeat_at:
            return
        self.command("m")
        self.port.flush()
        result = self.wait_for(("MENU REPEATED", "MENU SKIPPED"), seconds=15)
        self.menu_repeat_at = (time.monotonic() + MENU_REPEAT_SECONDS
                               if result == "MENU REPEATED" else None)

    def record(self):
        self.command("R")
        self.port.flush()
        print("Recording for ten seconds on the ESP32...", flush=True)
        deadline = time.monotonic() + RECORD_SECONDS + 15
        marker = bytearray()
        while bytes(marker) != b"REC1":
            if time.monotonic() > deadline:
                raise TimeoutError("ESP32 did not begin recording")
            byte = self.port.read(1)
            if byte:
                marker.extend(byte)
                marker = marker[-4:]
        rate = struct.unpack("<I", read_exact(self.port, 4))[0]
        if rate != RATE:
            raise ValueError(f"Unexpected recording rate: {rate} Hz")
        print("Transferring recorded audio...", flush=True)
        raw = bytearray()
        while True:
            packet = read_exact(self.port, 258)
            count = struct.unpack_from("<H", packet)[0]
            if count == 0:
                if len(raw) != RATE * RECORD_SECONDS * 2:
                    raise Hangup
                return bytes(raw)
            capture_errors = {65535: "flash open", 65534: "I2S read",
                              65533: "empty I2S buffer", 65532: "flash write",
                              65531: "flash reopen", 65530: "flash read"}
            if count in capture_errors:
                captured = struct.unpack_from("<I", packet, 2)[0]
                raise RuntimeError(
                    f"ESP32 capture failed: {capture_errors[count]} "
                    f"after {captured} samples")
            if count > 128 or len(raw) + count * 2 > RATE * RECORD_SECONDS * 2:
                raise ValueError(f"Invalid recording chunk: {count} samples after {len(raw) // 2} samples")
            raw.extend(packet[2:2 + count * 2])

    def available_key(self):
        return self.available_keys.pop() if self.available_keys else None

    def leave_message(self):
        key = self.available_key()
        if key is None:
            self.speak_clips("store_full")
            return
        self.speak_clips("record_after_beep")
        self.beep()
        raw = self.record()
        destination = self.message_dir / f"{key}.wav"
        temporary = self.message_dir / f".{key}.wav.tmp"
        save_wav(temporary, raw)
        temporary.replace(destination)
        print(f"Saved message {key}: {destination.resolve()}", flush=True)
        digits = tuple(f"digit_{digit}" for digit in key)
        self.speak_clips("your_code", *digits, "repeat", *digits)

    def fetch_message(self, key):
        path = self.message_dir / f"{key}.wav"
        if not path.exists():
            self.speak_clips("you_dialed", *(f"digit_{digit}" for digit in key),
                             "no_message")
            return
        print(f"Playing message {key}: {path.resolve()}", flush=True)
        raw, gain = listening_copy(load_wav(path))
        print(f"Playback gain: {gain:.1f} times", flush=True)
        self.play(raw, intro_clip="here_message")
        self.beep()

    def handle(self, line):
        if line.startswith("Cradle DOWN"):
            if not self.offhook:
                return
            self.offhook = False
            self.entered = ""
            self.menu_repeat_at = None
            return
        if line.startswith("Cradle UP"):
            if self.offhook:
                return
            self.offhook = True
            self.entered = ""
            result = self.opening_prompt()
            self.menu_repeat_at = (time.monotonic() + MENU_REPEAT_SECONDS
                                   if result == "MENU PLAYED" else None)
            return
        if not self.offhook:
            return
        if line.startswith("Dial unrecognized"):
            self.menu_repeat_at = None
            return
        trigger = TRIGGER.match(line)
        if trigger and self.accept_turn(int(trigger.group(1))):
            self.menu_repeat_at = None
            self.entered = ""
            self.leave_message()
            return
        match = DIALED.match(line)
        if match and self.accept_turn(int(match.group(2))):
            self.menu_repeat_at = None
            digit = match.group(1)
            self.entered += digit
            print(f"Key entered: {self.entered}", flush=True)
            self.speak_clips(f"digit_{digit}")
            if len(self.entered) == 3:
                key, self.entered = self.entered, ""
                self.fetch_message(key)

    def accept_turn(self, turn_id):
        if turn_id <= self.last_turn_id:
            return False
        self.last_turn_id = turn_id
        return True

    def run(self):
        self.pending.clear()
        self.offhook = False
        self.entered = ""
        self.menu_repeat_at = None
        self.last_turn_id = 0
        self.command("H")
        self.port.flush()
        self.wait_for("HOST MODE", seconds=8)
        self.pending.clear()
        print("Phone ready. Put the handset down, then lift it to begin.", flush=True)
        last_contact = time.monotonic()
        while True:
            line = self.pending.popleft() if self.pending else self.line()
            if line:
                try:
                    self.handle(line)
                except Hangup:
                    print("Call ended. Waiting for the next lift.", flush=True)
                    self.offhook = False
                    self.entered = ""
                    self.menu_repeat_at = None
                last_contact = time.monotonic()
            try:
                self.repeat_menu_if_due()
            except Hangup:
                print("Call ended. Waiting for the next lift.", flush=True)
                self.offhook = False
                self.entered = ""
                self.menu_repeat_at = None
            if not line and time.monotonic() - last_contact >= HEARTBEAT_SECONDS:
                self.command("H")
                self.wait_for("HOST MODE", seconds=HEARTBEAT_TIMEOUT_SECONDS)
                last_contact = time.monotonic()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--connection", choices=("usb", "shared-wifi", "esp32-ap"),
                        default="shared-wifi", help="ESP32 transport")
    parser.add_argument("--serial-port", default="/dev/cu.usbserial-0001",
                        help="USB serial device in USB mode")
    parser.add_argument("--bind", default="0.0.0.0",
                        help="Laptop address to listen on")
    parser.add_argument("--port", type=int, default=PHONE_PORT,
                        help="Laptop TCP port")
    parser.add_argument("--messages", type=Path,
                        default=SERVER_DIR / "data" / "messages")
    args = parser.parse_args()
    phone = Phone(None, args.messages)
    if args.connection == "usb":
        try:
            import serial
        except ImportError as error:
            raise SystemExit("USB mode needs pyserial: python3 -m pip install pyserial") from error
        print(f"Opening ESP32 on {args.serial_port}", flush=True)
        phone.serial_pacing = True
        while True:
            try:
                with serial.Serial(args.serial_port, 460800, timeout=2,
                                   write_timeout=3) as port:
                    run_phone(phone, port)
            except (serial.SerialException, OSError) as error:
                print(f"USB unavailable: {error}. Retrying...", flush=True)
            time.sleep(2)
        return
    if args.connection == "esp32-ap":
        print(f"Connecting to ESP32 at {ESP32_AP_HOST}:{args.port}", flush=True)
        while True:
            try:
                connection = socket.create_connection((ESP32_AP_HOST, args.port),
                                                      timeout=3)
            except OSError:
                time.sleep(3)
                continue
            run_session(phone, connection)
            time.sleep(1)
        return
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.bind, args.port))
        listener.listen(1)
        print(f"Waiting for ESP32 on {args.bind}:{args.port}", flush=True)
        while True:
            connection, address = listener.accept()
            print(f"Phone connected from {address[0]}:{address[1]}", flush=True)
            run_session(phone, connection)


def run_session(phone, connection):
    with connection:
        connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        connection.settimeout(2)
        run_phone(phone, SocketPort(connection))


def run_phone(phone, port):
    phone.port = port
    try:
        phone.run()
    except (ConnectionError, TimeoutError, OSError,
            RuntimeError, ValueError) as error:
        print(f"Phone session ended: {error}. Reconnecting...", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nPhone stopped.")
