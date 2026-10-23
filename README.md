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

For SD mode, put a FAT32 card in the reader, flash the ESP32, and power it from USB or a battery. Messages are `/MSG###.WAV` files on the card. A temporary `/RECORD.TMP` file is removed after a complete recording, after an aborted recording, and on boot. To use a laptop transport instead, start the matching laptop mode. USB mode needs `pyserial` (`python3 -m pip install pyserial`):

```sh
python3 server/phone.py --connection usb --serial-port /dev/cu.usbserial-0001
python3 server/phone.py --connection shared-wifi
```

For ESP32 AP mode, join the laptop to `PHONE_AP_SSID`, then start:

```sh
python3 server/phone.py --connection esp32-ap
```

In shared mode, the laptop listens on TCP port `45679` and the ESP32 connects to it. In ESP32 AP mode, the ESP32 listens at `192.168.4.1:45679` and the laptop connects to it. In USB mode, the same audio commands and replies use serial at 460800 baud. The laptop service reconnects after a lost link. Wi-Fi modes need no Python packages beyond the standard library. For wireless use, keep the ESP32 powered from a wall adapter or power bank. A laptop using the ESP32 network generally needs another connection for internet access.

Put the handset down, then lift it. The phone says “This is the Secrets Hotline” and gives the dial instructions. After 15 seconds of inactivity it repeats only the instructions. Starting to dial skips the rest of the menu. Dial **0** to record a ten-second message, or dial a three-digit key using **1–9** to hear one. A beep marks the beginning and end of recording and follows playback. Hanging up aborts the current action. A missing key can be retried without hanging up.

In SD mode, all 729 three-digit keys made from 1–9 are considered in a random order, skipping keys already saved on the card. Each key is issued at most once during a power-on run. The laptop server uses its own shuffled key pool in transport modes. Copy the SD card WAV files or `server/data/` to keep saved messages.

The microphone produces 16 kHz, 16-bit mono PCM: 32 KB per second. A ten-second caller message is about 320 KB. In USB mode, recording streams to the laptop as it is captured, and playback uses small chunks paced by the ESP32; neither operation writes the temporary flash file. In either Wi-Fi mode, the ESP32 writes one temporary SPIFFS file while recording, then uploads it. For playback it downloads the whole message into that file before speaking the introduction and playing it. The temporary file is deleted after each operation, after a hang-up or error, and on boot. The laptop saves each message as a WAV file. The reusable speech clips are embedded in firmware flash. Repeated Wi-Fi recordings contribute to flash wear; USB mode avoids that temporary flash write. The client reconnects when the Wi-Fi link or laptop service returns.

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
