#include <Arduino.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <SD.h>
#include <SPI.h>
#include <driver/i2s.h>
#include <driver/adc.h>
#include <driver/rtc_io.h>
#include <math.h>
#include <soc/i2s_reg.h>
#include <soc/soc.h>
#include "prompt_pcm.h"
#include "voice_adpcm.h"
#include "phone_transport.h"

#ifndef PHONE_CRADLE_PIN
#define PHONE_CRADLE_PIN 32
#endif
#if PHONE_CRADLE_PIN != 4 && PHONE_CRADLE_PIN != 32
#error "PHONE_CRADLE_PIN must be GPIO4 or GPIO32"
#endif

// GPIO pins used for the DOIT ESP32 DEVKIT V1 microphone wiring.
constexpr int PIN_BCLK = 25;
constexpr int PIN_WS = 26;
constexpr int PIN_DATA = 33;
constexpr int PIN_SPEAKER_BCLK = 14;
constexpr int PIN_SPEAKER_WS = 27;
constexpr int PIN_SPEAKER_DATA = 22;
constexpr int PIN_CRADLE = PHONE_CRADLE_PIN;
constexpr int PIN_DIAL_RR = 5;
constexpr uint32_t DIAL_SETTLE_US = 250000;
// Settled readings after the current wiring: handset up ~1000, down ~4095.
// Ignore intermediate contact readings while the cradle is moving.
constexpr int CRADLE_UP_BELOW = 1800;
constexpr int CRADLE_DOWN_ABOVE = 3000;
constexpr unsigned long CRADLE_UP_SETTLE_MS = 100;
constexpr unsigned long CRADLE_DOWN_SETTLE_MS = 700;
constexpr uint32_t CRADLE_DIAL_GUARD_US = 800000;
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
constexpr i2s_port_t SPEAKER_PORT = I2S_NUM_1;
constexpr int SAMPLE_RATE = 32000;
constexpr int RECORD_RATE = SAMPLE_RATE / 2;
constexpr uint32_t HOST_RECORD_SAMPLES = RECORD_RATE * 10;
constexpr size_t FRAMES_PER_READ = 256;
constexpr size_t HOST_PLAY_FRAMES = 1024;
constexpr float PROMPT_GAIN = 0.08f;
constexpr float VOICE_GAIN = 0.08f;
constexpr char AUDIO_FILE[] = "/audio.pcm";

PhoneTransport phone_client;
constexpr unsigned long SERVER_RETRY_MS = 3000;
constexpr unsigned long SERVER_IDLE_TIMEOUT_MS = 20000;
unsigned long last_server_attempt_at = 0;
unsigned long last_server_contact_at = 0;

int32_t samples[FRAMES_PER_READ * 2]; // left, right, repeated
int16_t recording_chunk[FRAMES_PER_READ / 2];
uint8_t recording_file_chunk[4096];
int16_t host_play_chunk[FRAMES_PER_READ];
int16_t stereo_chunk[FRAMES_PER_READ * 2];
constexpr uint32_t prompt_samples =
    sizeof(PROMPT_MENU_PCM) / sizeof(PROMPT_MENU_PCM[0]);
constexpr uint32_t prompt_intro_samples =
    sizeof(PROMPT_INTRO_PCM) / sizeof(PROMPT_INTRO_PCM[0]);
int cradle_raw = HIGH;
int cradle_stable = HIGH;
unsigned long cradle_changed_at = 0;
unsigned long cradle_sampled_at = 0;
int cradle_adc = -1;
bool cradle_event_pending = false;
volatile uint32_t dial_edges = 0;
volatile uint32_t dial_last_edge_us = 0;
uint32_t dial_reported_edges = 0;
int dial_reported_level = HIGH;
uint32_t dial_pending_edges = 0;
uint32_t dial_turn_id = 0;
uint32_t dial_turn_at_lift = 0;
#if PHONE_CONNECTION_MODE == PHONE_CONNECTION_SD
uint8_t sd_dial_queue[8] = {};
uint8_t sd_dial_read = 0;
uint8_t sd_dial_write = 0;
#endif
char command_buffer[16] = {};
size_t command_length = 0;

void IRAM_ATTR onDialEdge() {
  const uint32_t now = micros();
  if (now - dial_last_edge_us < 3000) return; // Ignore contact bounce under 3 ms.
  dial_last_edge_us = now;
  ++dial_edges;
}

void pollDial() {
  const int level = digitalRead(PIN_DIAL_RR);
  noInterrupts();
  const uint32_t edges = dial_edges;
  const uint32_t last_edge_us = dial_last_edge_us;
  interrupts();
  if (cradle_stable != HIGH) {
    dial_reported_edges = edges;
    dial_pending_edges = 0;
    dial_reported_level = level;
    return;
  }
  if (edges != dial_reported_edges || level != dial_reported_level) {
    dial_pending_edges += edges - dial_reported_edges;
    dial_reported_edges = edges;
    dial_reported_level = level;
#if PHONE_CONNECTION_MODE != PHONE_CONNECTION_SD
    phone_client.printf("Dial RR: %s; edges: %lu\n",
                  level == LOW ? "GROUNDED" : "OPEN",
                  static_cast<unsigned long>(edges));
#endif
  }
  if (dial_pending_edges == 0 ||
      static_cast<uint32_t>(micros() - last_edge_us) < DIAL_SETTLE_US) return;

  const uint32_t turn_edges = dial_pending_edges;
  dial_pending_edges = 0;
  ++dial_turn_id;
#if PHONE_CONNECTION_MODE == PHONE_CONNECTION_SD
  const uint8_t next = (sd_dial_write + 1) % 8;
  if (next != sd_dial_read) {
    sd_dial_queue[sd_dial_write] = turn_edges;
    sd_dial_write = next;
  } else {
    PHONE_LOG_PRINTLN("Dial queue full; turn ignored");
  }
  PHONE_LOG_PRINTF("Dial: %lu edges; turn %lu\n",
                   static_cast<unsigned long>(turn_edges),
                   static_cast<unsigned long>(dial_turn_id));
#else
  if (turn_edges == 20) {
    phone_client.printf("TRIGGER (%lu edges; turn %lu)\n",
                  static_cast<unsigned long>(turn_edges),
                  static_cast<unsigned long>(dial_turn_id));
  } else if (turn_edges >= 2 && turn_edges <= 18 && turn_edges % 2 == 0) {
    phone_client.printf("DIALED %lu (%lu edges; turn %lu)\n",
                  static_cast<unsigned long>(turn_edges / 2),
                  static_cast<unsigned long>(turn_edges),
                  static_cast<unsigned long>(dial_turn_id));
  } else {
    phone_client.printf("Dial unrecognized (%lu edges; turn %lu)\n",
                  static_cast<unsigned long>(turn_edges),
                  static_cast<unsigned long>(dial_turn_id));
  }
#endif
}

void pollCradle(bool report = true) {
  if (report && cradle_event_pending) {
#if PHONE_CONNECTION_MODE == PHONE_CONNECTION_SD
    PHONE_LOG_PRINTF("Cradle %s; ADC: %d\n",
                  cradle_stable == LOW ? "DOWN" : "UP", cradle_adc);
#else
    phone_client.printf("Cradle %s; ADC: %d\n",
                  cradle_stable == LOW ? "DOWN" : "UP", cradle_adc);
#endif
    cradle_event_pending = false;
  }
  const unsigned long now = millis();
  if (now - cradle_sampled_at < 10) return;
  cradle_sampled_at = now;
  #if PHONE_CRADLE_PIN == 4 && PHONE_CONNECTION_MODE == PHONE_CONNECTION_SD
  int measured = -1;
  cradle_adc = adc2_get_raw(ADC2_CHANNEL_0, ADC_WIDTH_BIT_12,
                            &measured) == ESP_OK ? measured : -1;
  #elif PHONE_CRADLE_PIN == 4
  cradle_adc = digitalRead(PIN_CRADLE) == HIGH ? 4095 : 0;
  #else
  cradle_adc = adc1_get_raw(ADC1_CHANNEL_4);
  #endif
  if (cradle_adc < 0) return;
  int reading = cradle_raw;
  if (cradle_adc < CRADLE_UP_BELOW) reading = HIGH;
  if (cradle_adc > CRADLE_DOWN_ABOVE) reading = LOW;
  if (reading != cradle_raw) {
    cradle_raw = reading;
    cradle_changed_at = now;
  }
  const bool dial_recent =
      static_cast<uint32_t>(micros() - dial_last_edge_us) < CRADLE_DIAL_GUARD_US;
  const unsigned long settle_ms = reading == LOW ?
      CRADLE_DOWN_SETTLE_MS : CRADLE_UP_SETTLE_MS;
  if (reading != cradle_stable && now - cradle_changed_at >= settle_ms &&
      !(reading == LOW && dial_recent)) {
    cradle_stable = reading;
    if (reading == HIGH) dial_turn_at_lift = dial_turn_id;
    if (report) {
#if PHONE_CONNECTION_MODE == PHONE_CONNECTION_SD
      PHONE_LOG_PRINTF("Cradle %s; ADC: %d\n",
                    reading == LOW ? "DOWN" : "UP", cradle_adc);
#else
      phone_client.printf("Cradle %s; ADC: %d\n",
                    reading == LOW ? "DOWN" : "UP", cradle_adc);
#endif
    } else {
      cradle_event_pending = true;
    }
  }
}

bool handsetDown(bool report = true) {
  pollCradle(report);
  return cradle_stable == LOW;
}

void discardMicStartup() {
  for (int i = 0; i < 20; ++i) {
    size_t discarded = 0;
    i2s_read(I2S_PORT, samples, sizeof(samples), &discarded, portMAX_DELAY);
  }
}

bool writeNetworkBytes(const uint8_t *source, size_t count) {
  size_t sent = 0;
  while (sent < count && phone_client.connected()) {
    const size_t written = phone_client.write(source + sent, count - sent);
    if (written == 0) return false;
    sent += written;
  }
  return sent == count;
}

bool playBeep(bool ending = false);
bool playVoiceClip(uint8_t clip_id);

bool writeAudioFile(File &file, const uint8_t *data, size_t count) {
  size_t written = 0;
  int stalls = 0;
  while (written < count) {
    const size_t bytes = file.write(data + written, count - written);
    if (bytes == 0) {
      if (++stalls >= 3) return false;
      delay(1);
      continue;
    }
    written += bytes;
    stalls = 0;
  }
  return true;
}

void recordHostAudio() {
  uint16_t packet[FRAMES_PER_READ / 2 + 1] = {};
  if (SPIFFS.exists(AUDIO_FILE)) SPIFFS.remove(AUDIO_FILE);
  File recording = SPIFFS.open(AUDIO_FILE, FILE_WRITE);
  if (!recording) {
    const uint32_t rate = RECORD_RATE;
    packet[0] = UINT16_MAX;
    writeNetworkBytes(reinterpret_cast<const uint8_t *>("REC1"), 4);
    writeNetworkBytes(reinterpret_cast<const uint8_t *>(&rate), sizeof(rate));
    writeNetworkBytes(reinterpret_cast<const uint8_t *>(packet), sizeof(packet));
    return;
  }
  discardMicStartup();
  uint32_t captured = 0;
  uint16_t status = 0;
  size_t buffered = 0;
  const unsigned long capture_started_at = millis();
  while (captured < HOST_RECORD_SAMPLES) {
    if (handsetDown(false)) {
      break;
    }
    size_t bytes_read = 0;
    if (i2s_read(I2S_PORT, samples, sizeof(samples), &bytes_read,
                 portMAX_DELAY) != ESP_OK) {
      status = 65534; // I2S read.
      break;
    }
    const size_t frames = bytes_read / (2 * sizeof(int32_t));
    uint16_t count = 0;
    for (size_t frame = 0; frame + 1 < frames &&
                           captured + count < HOST_RECORD_SAMPLES; frame += 2) {
      const int32_t a = samples[frame * 2] >> 16;
      const int32_t b = samples[(frame + 1) * 2] >> 16;
      recording_chunk[count++] = static_cast<int16_t>((a + b) / 2);
    }
    if (count == 0) {
      status = 65533; // Empty I2S buffer.
      break;
    }
    const size_t bytes = count * sizeof(int16_t);
    if (buffered + bytes > sizeof(recording_file_chunk)) {
      if (!writeAudioFile(recording, recording_file_chunk, buffered)) {
        status = 65532; // Flash write.
        break;
      }
      buffered = 0;
    }
    memcpy(recording_file_chunk + buffered, recording_chunk, bytes);
    buffered += bytes;
    if (buffered == sizeof(recording_file_chunk) ||
        captured + count == HOST_RECORD_SAMPLES) {
      if (!writeAudioFile(recording, recording_file_chunk, buffered)) {
        status = 65532; // Flash write.
        break;
      }
      buffered = 0;
    }
    captured += count;
  }
  recording.close();
  PHONE_LOG_PRINTF("Captured %lu samples in %lu ms\n",
                static_cast<unsigned long>(captured),
                static_cast<unsigned long>(millis() - capture_started_at));
  if (captured != HOST_RECORD_SAMPLES && status == 0) {
    if (SPIFFS.exists(AUDIO_FILE)) SPIFFS.remove(AUDIO_FILE);
    pollCradle();
  }
  if (captured == HOST_RECORD_SAMPLES && !playBeep()) {
    if (SPIFFS.exists(AUDIO_FILE)) SPIFFS.remove(AUDIO_FILE);
    status = 0;
    captured = 0;
  }
  const uint32_t rate = RECORD_RATE;
  if (!writeNetworkBytes(reinterpret_cast<const uint8_t *>("REC1"), 4) ||
      !writeNetworkBytes(reinterpret_cast<const uint8_t *>(&rate), sizeof(rate))) return;
  if (captured == HOST_RECORD_SAMPLES && status == 0) {
    recording = SPIFFS.open(AUDIO_FILE, FILE_READ);
    if (!recording) status = 65531; // Flash reopen.
    while (status == 0 && recording.available()) {
      if (handsetDown()) {
        recording.close();
        if (SPIFFS.exists(AUDIO_FILE)) SPIFFS.remove(AUDIO_FILE);
        phone_client.stop();
        return;
      }
      const size_t bytes = recording.readBytes(
          reinterpret_cast<char *>(&packet[1]), sizeof(packet) - 2);
      if (bytes == 0 || bytes % 2 != 0) {
        status = 65530; // Flash read.
        break;
      }
      packet[0] = bytes / 2;
      if (!writeNetworkBytes(reinterpret_cast<const uint8_t *>(packet),
                             sizeof(packet))) {
        recording.close();
        if (SPIFFS.exists(AUDIO_FILE)) SPIFFS.remove(AUDIO_FILE);
        phone_client.stop();
        return;
      }
    }
    recording.close();
  }
  if (SPIFFS.exists(AUDIO_FILE)) SPIFFS.remove(AUDIO_FILE);
  PHONE_LOG_PRINTF("Temporary audio cleared; SPIFFS used: %u bytes\n",
                static_cast<unsigned>(SPIFFS.usedBytes()));
  packet[0] = status;
  if (status != 0) {
    memcpy(&packet[1], &captured, sizeof(captured));
    writeNetworkBytes(reinterpret_cast<const uint8_t *>(packet), sizeof(packet));
    return;
  }
  packet[0] = 0;
  writeNetworkBytes(reinterpret_cast<const uint8_t *>(packet), sizeof(packet));
}

bool readNetworkBytes(uint8_t *destination, size_t count) {
  size_t received = 0;
  unsigned long last_progress = millis();
  while (received < count) {
    const size_t request = min(static_cast<size_t>(1024), count - received);
    const int chunk = phone_client.read(destination + received,
                                        request);
    if (chunk > 0) {
      received += chunk;
      last_progress = millis();
    } else {
      if (!phone_client.connected() || millis() - last_progress > 3000) return false;
      delay(1);
    }
  }
  return true;
}

#if PHONE_CONNECTION_MODE == PHONE_CONNECTION_USB
void recordUsbAudio() {
  discardMicStartup();
  const uint32_t rate = RECORD_RATE;
  phone_client.write(reinterpret_cast<const uint8_t *>("REC1"), 4);
  phone_client.write(reinterpret_cast<const uint8_t *>(&rate), sizeof(rate));
  uint16_t packet[FRAMES_PER_READ / 2 + 1] = {};
  uint32_t captured = 0;
  while (captured < HOST_RECORD_SAMPLES) {
    if (handsetDown(false)) {
      packet[0] = 0;
      phone_client.write(reinterpret_cast<const uint8_t *>(packet), sizeof(packet));
      phone_client.flush();
      pollCradle();
      return;
    }
    size_t bytes_read = 0;
    if (i2s_read(I2S_PORT, samples, sizeof(samples), &bytes_read,
                 portMAX_DELAY) != ESP_OK) {
      packet[0] = UINT16_MAX;
      phone_client.write(reinterpret_cast<const uint8_t *>(packet), sizeof(packet));
      phone_client.flush();
      return;
    }
    const size_t frames = bytes_read / (2 * sizeof(int32_t));
    uint16_t count = 0;
    for (size_t frame = 0; frame + 1 < frames &&
                           captured + count < HOST_RECORD_SAMPLES; frame += 2) {
      const int32_t a = samples[frame * 2] >> 16;
      const int32_t b = samples[(frame + 1) * 2] >> 16;
      recording_chunk[count++] = static_cast<int16_t>((a + b) / 2);
    }
    if (count == 0) {
      packet[0] = UINT16_MAX;
      phone_client.write(reinterpret_cast<const uint8_t *>(packet), sizeof(packet));
      phone_client.flush();
      return;
    }
    packet[0] = count;
    memcpy(&packet[1], recording_chunk, count * sizeof(int16_t));
    phone_client.write(reinterpret_cast<const uint8_t *>(packet), sizeof(packet));
    captured += count;
  }
  playBeep();
  packet[0] = 0;
  phone_client.write(reinterpret_cast<const uint8_t *>(packet), sizeof(packet));
  phone_client.flush();
}

void playUsbAudio() {
  uint32_t sample_count = 0;
  uint8_t intro_clip = 255;
  if (!readNetworkBytes(reinterpret_cast<uint8_t *>(&sample_count), 4) ||
      !readNetworkBytes(&intro_clip, 1) ||
      sample_count == 0 || sample_count > RECORD_RATE * 30 ||
      (intro_clip != 255 && intro_clip >= VOICE_CLIP_COUNT)) {
    phone_client.println("PLAY ERROR");
    return;
  }
  if (intro_clip != 255 && !playVoiceClip(intro_clip)) {
    phone_client.println(handsetDown() ? "ABORTED" : "SPEAKER ERROR");
    return;
  }
  uint32_t played = 0;
  while (played < sample_count) {
    if (handsetDown()) {
      i2s_zero_dma_buffer(SPEAKER_PORT);
      phone_client.println("ABORTED");
      return;
    }
    const size_t frames = min(static_cast<uint32_t>(FRAMES_PER_READ / 2),
                              sample_count - played);
    phone_client.println("NEXT");
    if (!readNetworkBytes(reinterpret_cast<uint8_t *>(host_play_chunk),
                          frames * sizeof(int16_t))) {
      phone_client.println("PLAY ERROR");
      return;
    }
    for (size_t frame = 0; frame < frames; ++frame) {
      stereo_chunk[frame * 2] = host_play_chunk[frame];
      stereo_chunk[frame * 2 + 1] = host_play_chunk[frame];
    }
    size_t written = 0;
    if (i2s_write(SPEAKER_PORT, stereo_chunk, frames * 2 * sizeof(int16_t),
                  &written, portMAX_DELAY) != ESP_OK ||
        written != frames * 2 * sizeof(int16_t)) {
      phone_client.println("PLAY ERROR");
      return;
    }
    played += frames;
  }
  delay(50);
  i2s_zero_dma_buffer(SPEAKER_PORT);
  phone_client.println(handsetDown() ? "ABORTED" : "PLAYED");
}
#endif

// IMA ADPCM state resets for each prerecorded clip.
constexpr int16_t ADPCM_STEP[] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282,
    1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767};
constexpr int8_t ADPCM_INDEX[] = {-1, -1, -1, -1, 2, 4, 6, 8};

int16_t decodeVoiceSample(const VoiceClip &clip, uint32_t position,
                          int32_t &predictor, int &index) {
  const uint8_t packed = clip.data[position / 2];
  const uint8_t nibble = position & 1 ? packed >> 4 : packed & 0x0f;
  const int step = ADPCM_STEP[index];
  int change = step >> 3;
  if (nibble & 4) change += step;
  if (nibble & 2) change += step >> 1;
  if (nibble & 1) change += step >> 2;
  predictor += (nibble & 8) ? -change : change;
  predictor = constrain(predictor, -32768, 32767);
  index = constrain(index + ADPCM_INDEX[nibble & 7], 0, 88);
  const uint32_t edge = min(position, clip.samples - position - 1);
  const float fade = min(1.0f, edge / 160.0f);
  return static_cast<int16_t>(predictor * VOICE_GAIN * fade);
}

bool playVoiceClip(uint8_t clip_id) {
  if (clip_id >= VOICE_CLIP_COUNT) return false;
  const VoiceClip &clip = VOICE_CLIP_TABLE[clip_id];
  int32_t predictor = 0;
  int index = 0;
  for (uint32_t played = 0; played < clip.samples;) {
    pollDial();
    if (handsetDown()) return false;
    const size_t frames = min(static_cast<uint32_t>(FRAMES_PER_READ),
                              clip.samples - played);
    for (size_t frame = 0; frame < frames; ++frame) {
      const int16_t value = decodeVoiceSample(clip, played + frame,
                                               predictor, index);
      stereo_chunk[frame * 2] = value;
      stereo_chunk[frame * 2 + 1] = value;
    }
    size_t written = 0;
    if (i2s_write(SPEAKER_PORT, stereo_chunk, frames * 2 * sizeof(int16_t),
                  &written, portMAX_DELAY) != ESP_OK ||
        written != frames * 2 * sizeof(int16_t)) return false;
    played += frames;
  }
  return true;
}

void playVoiceClips() {
  uint8_t count = 0;
  if (!readNetworkBytes(&count, 1) || count == 0 || count > 16) {
    phone_client.println("CLIPS ERROR");
    return;
  }
  uint8_t clip_ids[16] = {};
  if (!readNetworkBytes(clip_ids, count)) {
    phone_client.println("CLIPS ERROR");
    return;
  }
  for (uint8_t clip_number = 0; clip_number < count; ++clip_number) {
    if (!playVoiceClip(clip_ids[clip_number])) {
      i2s_zero_dma_buffer(SPEAKER_PORT);
      phone_client.println(handsetDown() ? "ABORTED" : "SPEAKER ERROR");
      return;
    }
  }
  delay(50);
  i2s_zero_dma_buffer(SPEAKER_PORT);
  phone_client.println(handsetDown() ? "ABORTED" : "CLIPS PLAYED");
}

void playHostAudio() {
  uint32_t sample_count = 0;
  uint8_t intro_clip = 255;
  if (!readNetworkBytes(reinterpret_cast<uint8_t *>(&sample_count), 4) ||
      !readNetworkBytes(&intro_clip, 1) ||
      sample_count == 0 || sample_count > RECORD_RATE * 30 ||
      (intro_clip != 255 && intro_clip >= VOICE_CLIP_COUNT)) {
    phone_client.println("PLAY ERROR");
    phone_client.stop();
    return;
  }
  if (SPIFFS.exists(AUDIO_FILE)) SPIFFS.remove(AUDIO_FILE);
  File audio = SPIFFS.open(AUDIO_FILE, FILE_WRITE);
  if (!audio) {
    phone_client.println("PLAY ERROR");
    phone_client.stop();
    return;
  }
  size_t remaining = sample_count * sizeof(int16_t);
  while (remaining > 0) {
    if (handsetDown(false)) {
      audio.close();
      if (SPIFFS.exists(AUDIO_FILE)) SPIFFS.remove(AUDIO_FILE);
      phone_client.stop();
      return;
    }
    const size_t bytes = min(remaining, sizeof(host_play_chunk));
    if (!readNetworkBytes(reinterpret_cast<uint8_t *>(host_play_chunk), bytes) ||
        !writeAudioFile(audio, reinterpret_cast<const uint8_t *>(host_play_chunk), bytes)) {
      audio.close();
      if (SPIFFS.exists(AUDIO_FILE)) SPIFFS.remove(AUDIO_FILE);
      phone_client.stop();
      return;
    }
    remaining -= bytes;
  }
  audio.close();
  audio = SPIFFS.open(AUDIO_FILE, FILE_READ);
  if (!audio) {
    phone_client.println("PLAY ERROR");
    if (SPIFFS.exists(AUDIO_FILE)) SPIFFS.remove(AUDIO_FILE);
    return;
  }
  if (intro_clip != 255 && !playVoiceClip(intro_clip)) {
    audio.close();
    if (SPIFFS.exists(AUDIO_FILE)) SPIFFS.remove(AUDIO_FILE);
    i2s_zero_dma_buffer(SPEAKER_PORT);
    phone_client.println(handsetDown() ? "ABORTED" : "SPEAKER ERROR");
    return;
  }
  uint32_t played = 0;
  while (played < sample_count) {
    pollDial();
    if (handsetDown()) {
      audio.close();
      if (SPIFFS.exists(AUDIO_FILE)) SPIFFS.remove(AUDIO_FILE);
      i2s_zero_dma_buffer(SPEAKER_PORT);
      phone_client.println("ABORTED");
      return;
    }
    const size_t frames = min(static_cast<uint32_t>(HOST_PLAY_FRAMES),
                              sample_count - played);
    if (audio.readBytes(reinterpret_cast<char *>(host_play_chunk),
                        frames * sizeof(int16_t)) != frames * sizeof(int16_t)) {
      audio.close();
      if (SPIFFS.exists(AUDIO_FILE)) SPIFFS.remove(AUDIO_FILE);
      phone_client.println("PLAY ERROR");
      return;
    }
    pollDial();
    if (handsetDown()) {
      audio.close();
      if (SPIFFS.exists(AUDIO_FILE)) SPIFFS.remove(AUDIO_FILE);
      i2s_zero_dma_buffer(SPEAKER_PORT);
      phone_client.println("ABORTED");
      return;
    }
    for (size_t offset = 0; offset < frames; offset += FRAMES_PER_READ) {
      pollDial();
      if (handsetDown()) {
        audio.close();
        if (SPIFFS.exists(AUDIO_FILE)) SPIFFS.remove(AUDIO_FILE);
        i2s_zero_dma_buffer(SPEAKER_PORT);
        phone_client.println("ABORTED");
        return;
      }
      const size_t block = min(FRAMES_PER_READ, frames - offset);
      for (size_t frame = 0; frame < block; ++frame) {
        const int16_t value = host_play_chunk[offset + frame];
        stereo_chunk[frame * 2] = value;
        stereo_chunk[frame * 2 + 1] = value;
      }
      size_t written = 0;
      if (i2s_write(SPEAKER_PORT, stereo_chunk, block * 2 * sizeof(int16_t),
                    &written, portMAX_DELAY) != ESP_OK ||
          written != block * 2 * sizeof(int16_t)) {
        audio.close();
        if (SPIFFS.exists(AUDIO_FILE)) SPIFFS.remove(AUDIO_FILE);
        phone_client.println("PLAY ERROR");
        return;
      }
    }
    played += frames;
  }
  audio.close();
  if (SPIFFS.exists(AUDIO_FILE)) SPIFFS.remove(AUDIO_FILE);
  PHONE_LOG_PRINTF("Temporary audio cleared; SPIFFS used: %u bytes\n",
                static_cast<unsigned>(SPIFFS.usedBytes()));
  delay(80);
  i2s_zero_dma_buffer(SPEAKER_PORT);
  phone_client.println(handsetDown() ? "ABORTED" : "PLAYED");
}

bool playPcmBuffer(const int16_t *pcm, uint32_t sample_count, float gain,
                   bool *dial_interrupted = nullptr) {
  if (dial_interrupted) *dial_interrupted = false;
  uint32_t played = 0;
  while (played < sample_count) {
    if (handsetDown()) {
      i2s_zero_dma_buffer(SPEAKER_PORT);
      return false;
    }
    if (dial_interrupted) {
      pollDial();
      if (dial_pending_edges != 0 || dial_turn_id != dial_turn_at_lift) {
        *dial_interrupted = true;
        i2s_zero_dma_buffer(SPEAKER_PORT);
        return false;
      }
    }
    const size_t frames = min(static_cast<uint32_t>(FRAMES_PER_READ),
                              sample_count - played);
    for (size_t frame = 0; frame < frames; ++frame) {
      const uint32_t index = played + frame;
      const uint32_t edge = min(index, sample_count - 1 - index);
      const float fade = min(1.0f, edge / 160.0f);
      const int32_t scaled = static_cast<int32_t>(pcm[index] * gain * fade);
      const int16_t value = constrain(scaled, -32768, 32767);
      stereo_chunk[frame * 2] = value;
      stereo_chunk[frame * 2 + 1] = value;
    }
    size_t written = 0;
    if (i2s_write(SPEAKER_PORT, stereo_chunk, frames * 2 * sizeof(int16_t),
                  &written, portMAX_DELAY) != ESP_OK ||
        written != frames * 2 * sizeof(int16_t)) {
      phone_client.println("SPEAKER ERROR");
      return false;
    }
    played += frames;
  }
  delay(50); // Let the last I2S buffer reach the speaker.
  i2s_zero_dma_buffer(SPEAKER_PORT);
  return !handsetDown();
}

void playMenu(bool include_intro) {
  bool dial_interrupted = false;
  bool finished = true;
  if (include_intro) {
    finished = playPcmBuffer(PROMPT_INTRO_PCM, prompt_intro_samples,
                             PROMPT_GAIN, &dial_interrupted);
  }
  if (finished) {
    finished = playPcmBuffer(PROMPT_MENU_PCM, prompt_samples,
                             PROMPT_GAIN, &dial_interrupted);
  }
  if (finished) phone_client.println(include_intro ? "MENU PLAYED" : "MENU REPEATED");
  else if (dial_interrupted) phone_client.println("MENU SKIPPED");
  else if (cradle_stable == LOW) phone_client.println("ABORTED");
}

bool playBeep(bool ending) {
  constexpr float start_frequencies[] = {523.25f, 659.25f};
  constexpr float end_frequencies[] = {659.25f, 523.25f};
  const float *frequencies = ending ? end_frequencies : start_frequencies;
  constexpr uint32_t lengths[] = {RECORD_RATE * 18 / 100,
                                  RECORD_RATE * 22 / 100};
  constexpr int amplitudes[] = {3200, 2800};
  for (unsigned note = 0; note < 2; ++note) {
    for (uint32_t played = 0; played < lengths[note];) {
      if (handsetDown()) {
        i2s_zero_dma_buffer(SPEAKER_PORT);
        return false;
      }
      const size_t frames = min(static_cast<uint32_t>(FRAMES_PER_READ),
                                lengths[note] - played);
      for (size_t frame = 0; frame < frames; ++frame) {
        const uint32_t index = played + frame;
        const float attack = min(1.0f, index / (RECORD_RATE * .025f));
        const float release = min(1.0f,
            (lengths[note] - index) /
            (RECORD_RATE * (note == 0 ? .08f : .12f)));
        const int16_t value = static_cast<int16_t>(amplitudes[note] *
            min(attack, release) *
            sinf(2.0f * PI * frequencies[note] * index / RECORD_RATE));
        stereo_chunk[frame * 2] = value;
        stereo_chunk[frame * 2 + 1] = value;
      }
      size_t written = 0;
      if (i2s_write(SPEAKER_PORT, stereo_chunk, frames * 2 * sizeof(int16_t),
                    &written, portMAX_DELAY) != ESP_OK ||
          written != frames * 2 * sizeof(int16_t)) return false;
      played += frames;
    }
    if (note == 0) {
      memset(stereo_chunk, 0, sizeof(stereo_chunk));
      for (uint32_t silent = 0; silent < RECORD_RATE / 50;
           silent += FRAMES_PER_READ) {
        const size_t frames = min(static_cast<uint32_t>(FRAMES_PER_READ),
                                  RECORD_RATE / 50 - silent);
        size_t written = 0;
        if (i2s_write(SPEAKER_PORT, stereo_chunk, frames * 4,
                      &written, portMAX_DELAY) != ESP_OK ||
            written != frames * 4) return false;
      }
    }
  }
  delay(50);
  i2s_zero_dma_buffer(SPEAKER_PORT);
  return !handsetDown();
}

#if PHONE_CONNECTION_MODE == PHONE_CONNECTION_SD
#include "sd_phone.h"
#endif

void setup() {
  Serial.begin(460800);
  delay(500);
  PHONE_LOG_PRINTLN("Phone client ready");
  pinMode(PIN_DIAL_RR, INPUT_PULLUP);
  dial_reported_level = digitalRead(PIN_DIAL_RR);
  attachInterrupt(digitalPinToInterrupt(PIN_DIAL_RR), onDialEdge, CHANGE);
  PHONE_LOG_PRINTF("Dial RR initial: %s\n",
                digitalRead(PIN_DIAL_RR) == LOW ? "GROUNDED" : "OPEN");
  pinMode(PIN_CRADLE, INPUT_PULLUP);
  #if PHONE_CRADLE_PIN == 4 && PHONE_CONNECTION_MODE == PHONE_CONNECTION_SD
  adc2_config_channel_atten(ADC2_CHANNEL_0, ADC_ATTEN_DB_12);
  rtc_gpio_pulldown_dis(GPIO_NUM_4);
  rtc_gpio_pullup_en(GPIO_NUM_4);
  int measured = -1;
  cradle_adc = adc2_get_raw(ADC2_CHANNEL_0, ADC_WIDTH_BIT_12,
                            &measured) == ESP_OK ? measured : 4095;
  #elif PHONE_CRADLE_PIN == 32
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_4, ADC_ATTEN_DB_12);
  rtc_gpio_pulldown_dis(GPIO_NUM_32);
  rtc_gpio_pullup_en(GPIO_NUM_32);
  cradle_adc = adc1_get_raw(ADC1_CHANNEL_4);
  #else
  cradle_adc = digitalRead(PIN_CRADLE) == HIGH ? 4095 : 0;
  #endif
  cradle_raw = cradle_stable = cradle_adc < CRADLE_UP_BELOW ? HIGH : LOW;
  cradle_changed_at = cradle_sampled_at = millis();
  PHONE_LOG_PRINTF("Cradle initial: %s; ADC: %d\n",
                cradle_stable == LOW ? "DOWN" : "UP", cradle_adc);

  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = 0;
  config.dma_buf_count = 16;
  config.dma_buf_len = FRAMES_PER_READ;
  config.use_apll = false;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = PIN_BCLK;
  pins.ws_io_num = PIN_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = PIN_DATA;

  esp_err_t error = i2s_driver_install(I2S_PORT, &config, 0, nullptr);
  if (error == ESP_OK) error = i2s_set_pin(I2S_PORT, &pins);
  if (error != ESP_OK) {
    PHONE_LOG_PRINTF("I2S setup failed: %d\n", error);
    while (true) delay(1000);
  }

  // The SPH0645 changes DATA close to the edge on which the original ESP32
  // samples it. Delay the receive input in hardware so the sign bit is not
  // lost. Standard I2S already requests the one-bit MSB offset; assert it
  // explicitly here because both settings are required for this microphone.
  REG_SET_FIELD(I2S_TIMING_REG(I2S_PORT), I2S_RX_SD_IN_DELAY, 2);
  REG_SET_BIT(I2S_CONF_REG(I2S_PORT), I2S_RX_MSB_SHIFT);
  PHONE_LOG_PRINTF("I2S timing: input delay=%u, MSB shift=%u\n",
                static_cast<unsigned>(REG_GET_FIELD(I2S_TIMING_REG(I2S_PORT),
                                                     I2S_RX_SD_IN_DELAY)),
                !!(REG_READ(I2S_CONF_REG(I2S_PORT)) & I2S_RX_MSB_SHIFT));

  i2s_config_t speaker_config = {};
  speaker_config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  speaker_config.sample_rate = RECORD_RATE;
  speaker_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  speaker_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  speaker_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  speaker_config.intr_alloc_flags = 0;
  speaker_config.dma_buf_count =
      PHONE_CONNECTION_MODE == PHONE_CONNECTION_SD ? 12 : 4;
  speaker_config.dma_buf_len = FRAMES_PER_READ;
  speaker_config.use_apll = false;
  speaker_config.tx_desc_auto_clear = true;

  i2s_pin_config_t speaker_pins = {};
  speaker_pins.bck_io_num = PIN_SPEAKER_BCLK;
  speaker_pins.ws_io_num = PIN_SPEAKER_WS;
  speaker_pins.data_out_num = PIN_SPEAKER_DATA;
  speaker_pins.data_in_num = I2S_PIN_NO_CHANGE;

  error = i2s_driver_install(SPEAKER_PORT, &speaker_config, 0, nullptr);
  if (error == ESP_OK) error = i2s_set_pin(SPEAKER_PORT, &speaker_pins);
  if (error != ESP_OK) {
    PHONE_LOG_PRINTF("Speaker I2S setup failed: %d\n", error);
    while (true) delay(1000);
  }
  i2s_zero_dma_buffer(SPEAKER_PORT);
#if PHONE_CONNECTION_MODE == PHONE_CONNECTION_SD
  setupSdPhone();
#else
  if (!SPIFFS.begin(true)) {
    PHONE_LOG_PRINTLN("Temporary audio storage unavailable");
    while (true) delay(1000);
  }
  if (SPIFFS.exists(AUDIO_FILE)) SPIFFS.remove(AUDIO_FILE); // Drop an interrupted transfer from an earlier boot.
  PHONE_LOG_PRINTF("Temporary audio storage: %u bytes free\n",
                static_cast<unsigned>(SPIFFS.totalBytes() - SPIFFS.usedBytes()));
  PHONE_LOG_PRINTF("Audio memory: prompt=flash, messages=temporary flash; free heap: %u\n",
                ESP.getFreeHeap());
  phone_client.begin();
  PHONE_LOG_PRINTF("Free heap after transport setup: %u\n", ESP.getFreeHeap());
#endif
}

bool pollNetworkCommand() {
  bool consumed = false;
  while (phone_client.available()) {
    consumed = true;
    last_server_contact_at = millis();
    const int incoming = phone_client.read();
    if (incoming != '\n') {
      if (incoming >= 32 && incoming <= 126 &&
          command_length < sizeof(command_buffer) - 1) {
        command_buffer[command_length++] = static_cast<char>(incoming);
      } else {
        command_length = 0;
      }
      continue;
    }
    command_buffer[command_length] = '\0';
    command_length = 0;
    if (strncmp(command_buffer, "RPH1:", 5) != 0 ||
        command_buffer[5] == '\0' || command_buffer[6] != '\0') continue;
    const char command = command_buffer[5];
    if (command == 'H') {
      phone_client.println("HOST MODE");
    }
    if (command == 'o') playMenu(true);
    if (command == 'm') playMenu(false);
    if (command == 'R') {
#if PHONE_CONNECTION_MODE == PHONE_CONNECTION_USB
      recordUsbAudio();
#else
      recordHostAudio();
#endif
    }
    if (command == 'P') {
#if PHONE_CONNECTION_MODE == PHONE_CONNECTION_USB
      playUsbAudio();
#else
      playHostAudio();
#endif
    }
    if (command == 'C') playVoiceClips();
    if (command == 'b') {
      phone_client.println(playBeep() ? "BEEPED" : "ABORTED");
    }
    return true;
  }
  return consumed;
}

void loop() {
#if PHONE_CONNECTION_MODE == PHONE_CONNECTION_SD
  loopSdPhone();
  return;
#endif
  if (!phone_client.ready()) {
    phone_client.stop();
    delay(100);
    return;
  }
  if (PHONE_CONNECTION_MODE != PHONE_CONNECTION_USB &&
      phone_client.connected() &&
      millis() - last_server_contact_at > SERVER_IDLE_TIMEOUT_MS) {
    PHONE_LOG_PRINTLN("Laptop timed out; reconnecting");
    phone_client.stop();
  }
  if (!phone_client.connected()) {
    phone_client.stop();
    pollCradle(false);
    const unsigned long now = millis();
    if (last_server_attempt_at == 0 ||
        now - last_server_attempt_at >= SERVER_RETRY_MS) {
      last_server_attempt_at = now;
      if (phone_client.acceptOrConnect()) {
        last_server_contact_at = millis();
        command_length = 0;
        noInterrupts();
        dial_reported_edges = dial_edges;
        interrupts();
        dial_pending_edges = 0;
        dial_turn_at_lift = dial_turn_id;
      }
    }
    delay(10);
    return;
  }
  pollDial();
  pollCradle();
  if (pollNetworkCommand()) return;

  size_t bytes_read = 0;
  esp_err_t error = i2s_read(I2S_PORT, samples, sizeof(samples),
                             &bytes_read, pdMS_TO_TICKS(1000));
  if (error != ESP_OK || bytes_read == 0) {
    PHONE_LOG_PRINTF("I2S read failed: %d, bytes: %u\n", error,
                  static_cast<unsigned>(bytes_read));
    delay(500);
    return;
  }

}
