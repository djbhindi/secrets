# Wireless rotary phone message system

The ESP32 reads the phone's cradle and dial, captures an SPH0645LM4H microphone, and drives a MAX98357A amplifier. It joins the same 2.4 GHz Wi-Fi network as the laptop. The opening menu and all reusable Cori speech clips are stored in ESP32 flash and play locally. The laptop stores callers' messages and streams those recordings when requested. USB is needed to flash the ESP32 and can then be replaced with a USB wall adapter or power bank for power; commands and caller audio travel over Wi-Fi TCP.

## Layout

- `client/`: PlatformIO ESP32 firmware. The opening menu and reusable speech clips are embedded in flash.
- `server/`: Laptop program and source WAV clips used to build the firmware. Messages are saved to `server/data/messages/`, which Git ignores.
- `scripts/`: Optional tools to regenerate the voice and menu assets.
- `tests/`: Laptop program tests.

## Wiring (DOIT ESP32 DEVKIT V1)

| Device | Signal | ESP32 |
| --- | --- | --- |
| SPH0645LM4H | 3v, GND | 3V3, GND |
| SPH0645LM4H | BCLK, LRCL, DOUT | GPIO25, GPIO26, GPIO33 |
| SPH0645LM4H | SEL | GND (left channel) |
| MAX98357A | VIN, GND | 5V/VIN, GND |
| MAX98357A | BCLK, LRC, DIN | GPIO14, GPIO27, GPIO22 |
| Phone cradle | F/A terminals | GND, GPIO4 or GPIO32 |
| Rotary dial | F, RR | GND, GPIO5 |

Set `PHONE_CRADLE_PIN` in `client/include/network_config.h` to match the cradle wire. The example uses GPIO4, matching this phone's original wiring. That mode senses a digital high/low level because Wi-Fi prevents ADC2 readings on the original ESP32. GPIO32 is available for ADC1 sensing if the wire is moved. Verify both handset positions after flashing. The other cradle wire stays on GND. Connect the speaker to both amplifier output terminals, never to ESP32 GPIO or GND. All devices share a ground. GPIO5 is a startup strapping pin, so verify the board still boots with the dial connected.

## Configure and run

Copy `client/include/network_config.h.example` to `client/include/network_config.h` and enter the Wi-Fi network name and password used by the laptop. Set `PHONE_SERVER_HOST` to the laptop's local network name (without `.local`) or its IP address. On a Mac, `scutil --get LocalHostName` prints the name. The real config file is ignored by Git.

```sh
cp client/include/network_config.h.example client/include/network_config.h
# Edit PHONE_WIFI_SSID, PHONE_WIFI_PASSWORD, and PHONE_SERVER_HOST
pio run -d client -t upload
```

Leave the laptop on the same Wi-Fi network and start its program:

```sh
python3 server/phone.py
```

The laptop listens on TCP port `45679`. The ESP32 looks up the laptop and connects on boot, then reconnects if the link drops. A dead call session times out and closes while the laptop keeps listening for a new connection. If local name resolution is unavailable, set `PHONE_SERVER_HOST` to the laptop's current IP address and flash again. The laptop needs no Python packages beyond the standard library to run the server. Keep the ESP32 powered from a wall adapter or power bank when unplugging it from the laptop.

Put the handset down, then lift it. The phone says “This is the Secrets Hotline” and gives the dial instructions. After 15 seconds of inactivity it repeats only the instructions. Starting to dial skips the rest of the menu. Dial **0** to record a ten-second message, or dial a three-digit key using **1–9** to hear one. A beep marks the beginning and end of recording and follows playback. Hanging up aborts the current action. A missing key can be retried without hanging up.

At server startup, all 729 three-digit keys made from 1–9 are shuffled, excluding keys whose message files already exist. Each key is issued at most once during that server run. Restarting the server reshuffles the available keys. Copy `server/data/` to another laptop if you want to keep saved messages.

The microphone produces 16 kHz, 16-bit mono PCM: 32 KB per second. A ten-second caller message is about 320 KB. Audio is sent in small TCP chunks, so the ESP32 never stores a complete caller recording. The laptop saves each message as a WAV file. The 16 reusable speech clips are compressed to about 165 KB in firmware flash; their playback needs no audio transfer from the laptop, though the laptop still chooses which clip to play. The current firmware occupies about 1.17 MB of its 1.31 MB application partition, leaving insufficient room for a collection of caller messages. The client reconnects when the Wi-Fi link or laptop service returns.

## Change the voice or menu

The firmware plays compressed speech clips built from WAV files in `server/assets/voice/`; the laptop never synthesizes speech during a call. To regenerate those clips, install the optional Piper dependency and download the Cori model:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r scripts/requirements.txt
.venv/bin/python -m piper.download_voices --data-dir voices en_GB-cori-medium
.venv/bin/python scripts/generate_voice_clips.py
python3 scripts/build_voice_header.py
pio run -d client -t upload
```

To change the embedded opening menu, edit `scripts/generate_menu_prompt.py`, then run:

```sh
.venv/bin/python scripts/generate_menu_prompt.py
.venv/bin/python scripts/build_prompt_header.py
pio run -d client -t upload
```

The SPH0645 has a receive timing quirk on the original ESP32. The firmware sets the I2S input delay and MSB alignment; shifting captured samples afterward would not fix a misread sign bit.

Run laptop tests with `python3 -m unittest discover -s tests`.
