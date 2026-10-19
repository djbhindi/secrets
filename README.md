# Rotary phone message system

The ESP32 client reads the phone's cradle and dial, records from an SPH0645LM4H microphone, and plays audio through a MAX98357A amplifier. A laptop server stores ten-second messages and plays prerecorded Cori voice clips. The ESP32 and laptop communicate over **USB serial at 460800 baud**. This version does not use Wi-Fi; the laptop must remain connected by USB while the phone is in use.

## Layout

- `client/`: PlatformIO firmware, including the opening menu audio in flash.
- `server/`: Python service and its bundled prerecorded speech clips. Recordings are created in `server/data/messages/`, which Git ignores.
- `scripts/`: Optional offline tools for regenerating the Cori speech clips and firmware menu.
- `tests/`: Server behavior tests.

The laptop only needs Python and `pyserial` at runtime. Piper and the Cori model are needed only if you choose to regenerate the speech assets.

## Wiring (DOIT ESP32 DEVKIT V1)

| Device | Signal | ESP32 |
| --- | --- | --- |
| SPH0645LM4H | 3v, GND | 3V3, GND |
| SPH0645LM4H | BCLK, LRCL, DOUT | GPIO25, GPIO26, GPIO33 |
| SPH0645LM4H | SEL | GND (left channel) |
| MAX98357A | VIN, GND | 5V/VIN, GND |
| MAX98357A | BCLK, LRC, DIN | GPIO14, GPIO27, GPIO22 |
| Phone cradle | F/A terminals | GND, GPIO4 |
| Rotary dial | F, RR | GND, GPIO5 |

Connect the speaker to both amplifier output terminals, never to an ESP32 GPIO or GND. The microphone and amplifier share ESP32 ground. GPIO5 is an ESP32 startup strapping pin, so verify the board boots with the dial attached.

## Install and run

From the repository root, install the laptop runtime and flash the client:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r server/requirements.txt
pio run -d client -t upload
.venv/bin/python server/phone.py
```

The server detects the ESP32 if it is the only USB serial device. With multiple USB serial devices, pass `--port /dev/cu.usbserial-0001` (or the corresponding port on your laptop). Close any serial monitor before starting the server; only one program can hold the port. On Windows, use `.venv\\Scripts\\python.exe` in place of `.venv/bin/python`.

If you created `.venv` with `uv`, use `uv pip install --python .venv/bin/python -r server/requirements.txt` because those environments may not include `pip`.

Put the handset down, then lift it. The phone says “This is the Secrets Hotline” and gives the dial instructions. If you do nothing for 15 seconds, it repeats only the instructions. Starting to dial skips the rest of the menu. Dial **0** to record a ten-second message, or dial a three-digit key using **1–9** to hear a saved message. A beep marks the start and end of recording and follows message playback. Hanging up aborts the current action; the next lift starts over. A missing key can be retried without hanging up.

At startup the server shuffles all 729 three-digit keys made from 1–9, excluding any keys whose message files already exist. It issues keys from that shuffled list, so no key is issued twice during one server run, even if a recording is interrupted. Restarting the server reshuffles the remaining keys; existing message files are still excluded. The server does not keep a separate history of deleted messages. The `server/data/` directory is the only runtime state to copy when moving messages to another laptop.

## Change the voice or menu

The server plays WAV clips in `server/assets/voice/` and does not synthesize speech during a call. To regenerate those clips, install `scripts/requirements.txt`, download the Cori model into `voices/`, then run:

```sh
.venv/bin/python -m pip install -r scripts/requirements.txt
.venv/bin/python -m piper.download_voices --data-dir voices en_GB-cori-medium
.venv/bin/python scripts/generate_voice_clips.py
```

To change the embedded opening menu, edit `scripts/generate_menu_prompt.py`, then run:

```sh
.venv/bin/python scripts/generate_menu_prompt.py
.venv/bin/python scripts/build_prompt_header.py
pio run -d client -t upload
```

The SPH0645 has a receive timing quirk on the original ESP32. The client sets the I2S input delay and MSB alignment in firmware; shifting the captured samples afterward would not fix a misread sign bit.

Run the server tests with `.venv/bin/python -m unittest discover -s tests`.
