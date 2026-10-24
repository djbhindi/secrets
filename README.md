# Wireless rotary phone message system

The ESP32 reads the phone's cradle and dial, captures an SPH0645LM4H microphone, and drives a MAX98357A amplifier. Select standalone SD card, USB serial, shared 2.4 GHz Wi-Fi, or an ESP32-hosted Wi-Fi network. The opening menu and reusable Cori speech clips are stored in ESP32 flash and play locally. In standalone mode, caller messages are saved as WAV files on the microSD card; no laptop service or Wi-Fi is needed.

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
| SD card reader | GND, VCC | GND, VIN (5V, for this reader with an onboard regulator) |
| SD card reader | MISO, MOSI, SCK, CS | GPIO19, GPIO23, GPIO18, GPIO13 |

Set `PHONE_CRADLE_PIN` in `client/include/network_config.h` to match the cradle wire. The example uses GPIO4, matching this phone's original wiring. That mode senses a digital high/low level because Wi-Fi prevents ADC2 readings on the original ESP32. GPIO32 is available for ADC1 sensing if the wire is moved. Verify both handset positions after flashing. The other cradle wire stays on GND. Connect the speaker to both amplifier output terminals, never to ESP32 GPIO or GND. All devices share a ground. GPIO5 is a startup strapping pin, so verify the board still boots with the dial connected.

To check the SD reader before changing the phone firmware, insert a FAT32-formatted card and run:

```sh
pio run -d client -e sd-card-check -t upload --upload-port /dev/cu.usbserial-0001
pio device monitor -p /dev/cu.usbserial-0001 -b 115200
```

Reset the ESP32 with the monitor open if the startup text was missed. `PASS` means the card mounted and a test file was written, read back, and removed. The check touches only `/rotary_sd_check.txt`; other files are left alone. This is a separate diagnostic firmware, so upload the normal `esp32doit-devkit-v1` environment afterward to return to phone operation.

## Configure and run

Copy `client/include/network_config.h.example` to `client/include/network_config.h`. Set `PHONE_CONNECTION_MODE` to `PHONE_CONNECTION_SD`, `PHONE_CONNECTION_USB`, `PHONE_CONNECTION_SHARED_WIFI`, or `PHONE_CONNECTION_ESP32_AP`. The real config file is ignored by Git. SD mode runs the whole phone flow on the ESP32 and does not start a transport.

In shared Wi-Fi mode, enter the Wi-Fi name and password used by the laptop. Set `PHONE_SERVER_HOST` to the laptop's local network name (without `.local`) or IP address. On a Mac, `scutil --get LocalHostName` prints the name. In ESP32 AP mode, choose a distinct network name and password (at least eight characters).

```sh
cp client/include/network_config.h.example client/include/network_config.h
# Edit PHONE_CONNECTION_MODE and the settings for that mode
pio run -d client -t upload
```

For SD mode, put a FAT32 card in the reader, flash the ESP32, and power it from USB or a battery. New messages are `/secrets_hotline/MSG###.WAV` files on the card. A temporary `/secrets_hotline/RECORD.TMP` file is removed after a complete recording, after an aborted recording, and on boot. To use a laptop transport instead, start the matching laptop mode. USB mode needs `pyserial` (`python3 -m pip install pyserial`):

```sh
python3 server/phone.py --connection usb --serial-port /dev/cu.usbserial-0001
python3 server/phone.py --connection shared-wifi
```

For ESP32 AP mode, join the laptop to `PHONE_AP_SSID`, then start:

```sh
python3 server/phone.py --connection esp32-ap
```

In shared mode, the laptop listens on TCP port `45679` and the ESP32 connects to it. In ESP32 AP mode, the ESP32 listens at `192.168.4.1:45679` and the laptop connects to it. In USB mode, the same audio commands and replies use serial at 460800 baud. The laptop service reconnects after a lost link. Wi-Fi modes need no Python packages beyond the standard library. For wireless use, keep the ESP32 powered from a wall adapter or power bank. A laptop using the ESP32 network generally needs another connection for internet access.

Put the handset down, then lift it. In SD mode, the opening prompt waits 1.5 seconds so there is time to raise the handset to your ear. The phone says “This is the Secrets Hotline” and gives the dial instructions. After 15 seconds of inactivity it repeats only the instructions. Starting to dial skips the opening pause or the rest of the menu. Dial **0** to record a message, or dial a three-digit key using **1–9** to hear one. In SD mode, dial **0** again to finish and save the recording; playback stops two seconds before the end to avoid dial noise, while the WAV file keeps the complete audio. The two-minute cap also finishes and saves the full recording and plays it in full. Hanging up during recording discards it. An ascending two-note chime starts recording; a descending chime ends it. A missing key can be retried without hanging up.

In SD mode, the phone considers three-digit keys made from 1–9 in a random order, skipping keys already saved on the card and the reserved keys **777**, **666**, and **911**. Each key is issued at most once during a power-on run. The laptop server uses its own shuffled key pool in transport modes. Copy the SD card WAV files or `server/data/` to keep saved messages.

To install an easter egg in SD mode, put a 16 kHz, mono, 16-bit PCM WAV with a 44-byte header on the card for each reserved code. A clip can be up to two minutes long. For example, after obtaining an audio clip, convert it with `ffmpeg`:

```sh
mkdir -p /Volumes/PHONE/secrets_hotline
ffmpeg -i clip.mp3 -ar 16000 -ac 1 -c:a pcm_s16le -bitexact /Volumes/PHONE/secrets_hotline/MSG777.WAV
```

Use `MSG777.WAV`, `MSG666.WAV`, and `MSG911.WAV` for those codes. The files belong in `/secrets_hotline/` on the card. Preloaded clips play at the volume encoded in the WAV; phone recordings retain automatic playback gain. With a reserved file absent, dialing that number gives the usual “no message” response. These SD files do not supply clips to laptop transport modes; that service uses its own `server/data/messages/` files.

The microphone produces 16 kHz, 16-bit mono PCM: 32 KB per second. A full two-minute SD recording is about 3.84 MB. The SD recording stores playback levels in the WAV file as it records, so playback does not need to scan the full message before the introduction. In USB mode, recording streams to the laptop as it is captured, and playback uses small chunks paced by the ESP32; neither operation writes the temporary flash file. In either Wi-Fi mode, the ESP32 writes one temporary SPIFFS file while recording, then uploads it. For playback it downloads the whole message into that file before speaking the introduction and playing it. The temporary file is deleted after each operation, after a hang-up or error, and on boot. The laptop saves each message as a WAV file. The reusable speech clips are embedded in firmware flash. Repeated Wi-Fi recordings contribute to flash wear; USB mode avoids that temporary flash write. The client reconnects when the Wi-Fi link or laptop service returns.

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
