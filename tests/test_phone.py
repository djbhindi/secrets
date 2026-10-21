import io
from pathlib import Path
import socket
import struct
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from server.phone import Hangup, Phone, RATE, SocketPort, VOICE_CLIPS


class FakePort:
    def __init__(self, incoming):
        self.incoming = io.BytesIO(incoming)
        self.sent = bytearray()

    def read(self, count):
        return self.incoming.read(count)

    def readline(self):
        return self.incoming.readline()

    def write(self, data):
        self.sent.extend(data)
        return len(data)

    def flush(self):
        pass


class PhoneTests(unittest.TestCase):
    def test_tcp_playback_streams_bounded_chunks(self):
        left, right = socket.socketpair()
        received = []

        def device():
            try:
                self.assertEqual(right.recv(7), b"RPH1:P\n")
                count = struct.unpack("<I", right.recv(4))[0]
                self.assertEqual(count, 2049)
                for frames in (1024, 1024, 1):
                    right.sendall(b"NEXT\n")
                    chunk = bytearray()
                    while len(chunk) < frames * 2:
                        chunk.extend(right.recv(frames * 2 - len(chunk)))
                    received.append(bytes(chunk))
                right.sendall(b"PLAYED\n")
            finally:
                right.close()

        worker = threading.Thread(target=device)
        worker.start()
        try:
            left.settimeout(1)
            with tempfile.TemporaryDirectory() as directory:
                phone = Phone(SocketPort(left), Path(directory))
                raw = bytes((index % 256 for index in range(2049 * 2)))
                phone.play(raw)
                self.assertEqual(b"".join(received), raw)
        finally:
            left.close()
            worker.join(timeout=2)

    def test_tcp_stream_keeps_lines_and_audio_in_order(self):
        left, right = socket.socketpair()
        try:
            left.settimeout(0.1)
            port = SocketPort(left)
            right.sendall(b"HOST MODE\n\x00\x01\x02")
            self.assertEqual(port.readline(), b"HOST MODE\n")
            self.assertEqual(port.read(3), b"\x00\x01\x02")
            port.write(b"RPH1:P\n")
            self.assertEqual(right.recv(7), b"RPH1:P\n")
            right.close()
            with self.assertRaises(ConnectionError):
                port.read(1)
        finally:
            left.close()
            right.close()

    def test_codes_are_unique_during_a_server_run(self):
        with tempfile.TemporaryDirectory() as directory:
            phone = Phone(None, Path(directory))
            codes = [phone.available_key() for _ in range(729)]
            self.assertEqual(len(set(codes)), 729)
            self.assertTrue(all(len(code) == 3 and "0" not in code for code in codes))
            self.assertIsNone(phone.available_key())

    def test_code_announcement_plays_prerecorded_clips_on_esp32(self):
        with tempfile.TemporaryDirectory() as directory:
            port = FakePort(b"CLIPS PLAYED\n")
            phone = Phone(port, Path(directory))
            names = ("you_dialed", "digit_7", "digit_8", "digit_1", "no_message")
            phone.speak_clips(*names)
            self.assertEqual(port.sent, b"RPH1:C\n" + bytes((len(names),))
                             + bytes(VOICE_CLIPS.index(name) for name in names))

    def test_recording_uses_chunk_end_marker(self):
        raw = bytes(RATE * 10 * 2)
        chunks = (struct.pack("<H", count) + raw[offset:offset + count * 2]
                  for offset in range(0, len(raw), 256)
                  for count in [min(128, (len(raw) - offset) // 2)])
        incoming = (b"REC1" + struct.pack("<I", RATE) + b"".join(chunks)
                    + bytes(258))
        with tempfile.TemporaryDirectory() as directory:
            port = FakePort(incoming)
            phone = Phone(port, Path(directory))
            self.assertEqual(phone.record(), raw)
            self.assertEqual(port.sent, b"RPH1:R\n")

    def test_hangup_discards_incomplete_recording(self):
        incoming = (b"REC1" + struct.pack("<I", RATE)
                    + struct.pack("<H", 128) + bytes(256)
                    + bytes(258))
        with tempfile.TemporaryDirectory() as directory:
            phone = Phone(FakePort(incoming), Path(directory))
            with self.assertRaises(Hangup):
                phone.record()
            self.assertEqual(list(Path(directory).glob("*.wav")), [])

    def test_completed_recording_and_playback_end_with_beeps(self):
        with tempfile.TemporaryDirectory() as directory:
            phone = Phone(None, Path(directory))
            beeps = []
            spoken = []
            phone.speak_clips = lambda *names: spoken.append(names)
            phone.record = lambda: bytes(RATE * 10 * 2)
            phone.play = lambda raw: None
            phone.beep = lambda: beeps.append(True)
            with patch.object(phone, "available_key", return_value="919"):
                phone.leave_message()
            self.assertEqual(spoken[0], ("record_after_beep",))
            self.assertEqual(spoken[1], ("your_code", "digit_9", "digit_1", "digit_9",
                                         "repeat", "digit_9", "digit_1", "digit_9"))
            files = list(Path(directory).glob("*.wav"))
            self.assertEqual(len(files), 1)
            self.assertEqual(files[0].stem, "919")
            self.assertEqual(len(beeps), 2)  # Start and end of recording.
            phone.fetch_message(files[0].stem)
            self.assertEqual(len(beeps), 3)

    def test_repeated_events_do_not_repeat_actions(self):
        with tempfile.TemporaryDirectory() as directory:
            phone = Phone(None, Path(directory))
            actions = []
            phone.opening_prompt = lambda: actions.append("menu")
            phone.leave_message = lambda: actions.append("record")
            phone.fetch_message = lambda key: actions.append(key)
            phone.speak_clips = lambda *names: actions.append(names[0])
            for _ in range(3):
                phone.handle("Cradle UP; ADC: 150")
                phone.handle("TRIGGER (20 edges; turn 1)")
            for turn, edges, digit in [(2, 6, 3), (3, 10, 5), (4, 16, 8)]:
                for _ in range(3):
                    phone.handle(f"DIALED {digit} ({edges} edges; turn {turn})")
            phone.handle("Cradle DOWN; ADC: 4000")
            phone.handle("Cradle UP; ADC: 150")
            self.assertEqual(actions, ["menu", "record", "digit_3", "digit_5",
                                       "digit_8", "358", "menu"])

    def test_missing_message_reads_back_all_digits(self):
        with tempfile.TemporaryDirectory() as directory:
            phone = Phone(None, Path(directory))
            spoken = []
            phone.speak_clips = lambda *names: spoken.append(names)
            phone.fetch_message("781")
            self.assertEqual(spoken, [
                ("you_dialed", "digit_7", "digit_8", "digit_1", "no_message")
            ])

    def test_dial_can_skip_opening_prompt(self):
        with tempfile.TemporaryDirectory() as directory:
            port = FakePort(b"MENU SKIPPED\nDIALED 1 (2 edges; turn 1)\n")
            phone = Phone(port, Path(directory))
            phone.opening_prompt()
            self.assertEqual(port.sent, b"RPH1:o\n")
            self.assertEqual(phone.line(), "DIALED 1 (2 edges; turn 1)")

    def test_idle_menu_repeats_and_dial_cancels_reminders(self):
        with tempfile.TemporaryDirectory() as directory:
            port = FakePort(b"MENU PLAYED\nMENU REPEATED\n")
            phone = Phone(port, Path(directory))
            phone.speak_clips = lambda *names: None
            phone.handle("Cradle UP; ADC: 150")
            self.assertGreater(phone.menu_repeat_at, time.monotonic())
            phone.menu_repeat_at = time.monotonic() - 1
            phone.repeat_menu_if_due()
            self.assertEqual(port.sent, b"RPH1:o\nRPH1:m\n")
            self.assertGreater(phone.menu_repeat_at, time.monotonic())
            phone.handle("DIALED 1 (2 edges; turn 1)")
            self.assertIsNone(phone.menu_repeat_at)

    def test_missing_key_allows_another_code_without_hangup(self):
        with tempfile.TemporaryDirectory() as directory:
            phone = Phone(None, Path(directory))
            phone.offhook = True
            spoken = []
            phone.speak_clips = lambda *names: spoken.append(names)
            for turn, digit in enumerate("999111", start=1):
                phone.handle(f"DIALED {digit} ({int(digit) * 2} edges; turn {turn})")
            self.assertEqual(phone.entered, "")
            self.assertTrue(phone.offhook)
            self.assertIn(("you_dialed", "digit_9", "digit_9", "digit_9", "no_message"), spoken)
            self.assertIn(("you_dialed", "digit_1", "digit_1", "digit_1", "no_message"), spoken)


if __name__ == "__main__":
    unittest.main()
