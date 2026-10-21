#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <driver/i2s.h>
#include <driver/adc.h>
#include <driver/rtc_io.h>
#include <math.h>
#include <soc/i2s_reg.h>
#include <soc/soc.h>
#include "prompt_pcm.h"
#include "voice_adpcm.h"
#include "network_config.h"

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
// Settled readings on this phone: handset up ~150, down ~3900-4095.
// Ignore intermediate contact readings while the cradle is moving.
constexpr int CRADLE_UP_BELOW = 500;
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
constexpr uint16_t PHONE_PORT = 45679;

WiFiServer phone_server(PHONE_PORT);
WiFiClient phone_client;

int32_t samples[FRAMES_PER_READ * 2]; // left, right, repeated
int16_t recording_chunk[FRAMES_PER_READ / 2];
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
    phone_client.printf("Dial RR: %s; edges: %lu\n",
                  level == LOW ? "GROUNDED" : "OPEN",
                  static_cast<unsigned long>(edges));
  }
  if (dial_pending_edges == 0 ||
      static_cast<uint32_t>(micros() - last_edge_us) < DIAL_SETTLE_US) return;

  const uint32_t turn_edges = dial_pending_edges;
  dial_pending_edges = 0;
  ++dial_turn_id;
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
}

void pollCradle(bool report = true) {
  if (report && cradle_event_pending) {
    phone_client.printf("Cradle %s; ADC: %d\n",
                  cradle_stable == LOW ? "DOWN" : "UP", cradle_adc);
    cradle_event_pending = false;
  }
  const unsigned long now = millis();
  if (now - cradle_sampled_at < 10) return;
  cradle_sampled_at = now;
  #if PHONE_CRADLE_PIN == 4
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
      phone_client.printf("Cradle %s; ADC: %d\n",
                    reading == LOW ? "DOWN" : "UP", cradle_adc);
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

void recordHostAudio() {
  discardMicStartup();
  const uint32_t rate = RECORD_RATE;
  if (!writeNetworkBytes(reinterpret_cast<const uint8_t *>("REC1"), 4) ||
      !writeNetworkBytes(reinterpret_cast<const uint8_t *>(&rate), sizeof(rate))) return;

  uint16_t packet[FRAMES_PER_READ / 2 + 1] = {};
  uint32_t captured = 0;
  while (captured < HOST_RECORD_SAMPLES) {
    if (handsetDown(false)) {
      packet[0] = 0;
      writeNetworkBytes(reinterpret_cast<const uint8_t *>(packet), sizeof(packet));
      pollCradle();
      return;
    }
    size_t bytes_read = 0;
    if (i2s_read(I2S_PORT, samples, sizeof(samples), &bytes_read,
                 portMAX_DELAY) != ESP_OK) {
      packet[0] = UINT16_MAX;
      writeNetworkBytes(reinterpret_cast<const uint8_t *>(packet), sizeof(packet));
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
    packet[0] = count;
    memcpy(&packet[1], recording_chunk, count * sizeof(int16_t));
    if (!writeNetworkBytes(reinterpret_cast<const uint8_t *>(packet),
                           sizeof(packet))) return;
    captured += count;
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
    if (clip_ids[clip_number] >= VOICE_CLIP_COUNT) {
      phone_client.println("CLIPS ERROR");
      return;
    }
    const VoiceClip &clip = VOICE_CLIP_TABLE[clip_ids[clip_number]];
    int32_t predictor = 0;
    int index = 0;
    for (uint32_t played = 0; played < clip.samples;) {
      pollDial();
      if (handsetDown()) {
        i2s_zero_dma_buffer(SPEAKER_PORT);
        phone_client.println("ABORTED");
        return;
      }
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
          written != frames * 2 * sizeof(int16_t)) {
        phone_client.println("SPEAKER ERROR");
        return;
      }
      played += frames;
    }
  }
  delay(50);
  i2s_zero_dma_buffer(SPEAKER_PORT);
  phone_client.println(handsetDown() ? "ABORTED" : "CLIPS PLAYED");
}

void playHostAudio() {
  uint32_t sample_count = 0;
  if (!readNetworkBytes(reinterpret_cast<uint8_t *>(&sample_count), 4) ||
      sample_count == 0 || sample_count > RECORD_RATE * 30) {
    phone_client.println("PLAY ERROR");
    return;
  }
  uint32_t played = 0;
  phone_client.println("NEXT");
  while (played < sample_count) {
    pollDial();
    if (handsetDown()) {
      i2s_zero_dma_buffer(SPEAKER_PORT);
      phone_client.stop(); // Discard any prefetched audio on hangup.
      return;
    }
    const size_t frames = min(static_cast<uint32_t>(HOST_PLAY_FRAMES),
                              sample_count - played);
    if (!readNetworkBytes(reinterpret_cast<uint8_t *>(host_play_chunk),
                         frames * sizeof(int16_t))) {
      phone_client.println("PLAY ERROR");
      return;
    }
    // Ask for the next block while this one plays through the I2S DMA buffer.
    if (played + frames < sample_count) phone_client.println("NEXT");
    pollDial();
    if (handsetDown()) {
      i2s_zero_dma_buffer(SPEAKER_PORT);
      phone_client.stop();
      return;
    }
    for (size_t offset = 0; offset < frames; offset += FRAMES_PER_READ) {
      pollDial();
      if (handsetDown()) {
        i2s_zero_dma_buffer(SPEAKER_PORT);
        phone_client.stop();
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
        phone_client.println("PLAY ERROR");
        return;
      }
    }
    played += frames;
  }
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

bool playBeep() {
  constexpr uint32_t BEEP_SAMPLES = RECORD_RATE / 5; // 200 ms
  uint32_t played = 0;
  while (played < BEEP_SAMPLES) {
    if (handsetDown()) {
      i2s_zero_dma_buffer(SPEAKER_PORT);
      return false;
    }
    const size_t frames = min(static_cast<uint32_t>(FRAMES_PER_READ),
                              BEEP_SAMPLES - played);
    for (size_t frame = 0; frame < frames; ++frame) {
      const uint32_t index = played + frame;
      const uint32_t edge = min(index, BEEP_SAMPLES - 1 - index);
      const float fade = min(1.0f, edge / 160.0f);
      const int16_t value = static_cast<int16_t>(
          5000 * fade * sinf(2.0f * PI * 1000.0f * index / RECORD_RATE));
      stereo_chunk[frame * 2] = value;
      stereo_chunk[frame * 2 + 1] = value;
    }
    size_t written = 0;
    i2s_write(SPEAKER_PORT, stereo_chunk, frames * 2 * sizeof(int16_t),
              &written, portMAX_DELAY);
    played += frames;
  }
  delay(50);
  i2s_zero_dma_buffer(SPEAKER_PORT);
  return !handsetDown();
}

void setup() {
  Serial.begin(460800);
  delay(500);
  Serial.println("Phone client ready");
  pinMode(PIN_DIAL_RR, INPUT_PULLUP);
  dial_reported_level = digitalRead(PIN_DIAL_RR);
  attachInterrupt(digitalPinToInterrupt(PIN_DIAL_RR), onDialEdge, CHANGE);
  Serial.printf("Dial RR initial: %s\n",
                digitalRead(PIN_DIAL_RR) == LOW ? "GROUNDED" : "OPEN");
  pinMode(PIN_CRADLE, INPUT_PULLUP);
  #if PHONE_CRADLE_PIN == 32
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
  Serial.printf("Cradle initial: %s; ADC: %d\n",
                cradle_stable == LOW ? "DOWN" : "UP", cradle_adc);

  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = 0;
  config.dma_buf_count = 4;
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
    Serial.printf("I2S setup failed: %d\n", error);
    while (true) delay(1000);
  }

  // The SPH0645 changes DATA close to the edge on which the original ESP32
  // samples it. Delay the receive input in hardware so the sign bit is not
  // lost. Standard I2S already requests the one-bit MSB offset; assert it
  // explicitly here because both settings are required for this microphone.
  REG_SET_FIELD(I2S_TIMING_REG(I2S_PORT), I2S_RX_SD_IN_DELAY, 2);
  REG_SET_BIT(I2S_CONF_REG(I2S_PORT), I2S_RX_MSB_SHIFT);
  Serial.printf("I2S timing: input delay=%u, MSB shift=%u\n",
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
  speaker_config.dma_buf_count = 4;
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
    Serial.printf("Speaker I2S setup failed: %d\n", error);
    while (true) delay(1000);
  }
  i2s_zero_dma_buffer(SPEAKER_PORT);
  Serial.printf("Audio memory: prompt=flash, messages=streamed; free heap: %u\n",
                ESP.getFreeHeap());
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(PHONE_WIFI_SSID, PHONE_WIFI_PASSWORD);
  Serial.printf("Joining Wi-Fi: %s\n", PHONE_WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  if (!MDNS.begin("secrets-phone")) {
    Serial.println("mDNS name unavailable; use the printed IP address");
  }
  phone_server.begin();
  Serial.printf("Phone Wi-Fi: %s at %s:%u; free heap: %u\n",
                PHONE_WIFI_SSID, WiFi.localIP().toString().c_str(),
                PHONE_PORT, ESP.getFreeHeap());
}

bool pollNetworkCommand() {
  bool consumed = false;
  while (phone_client.available()) {
    consumed = true;
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
    if (command == 'R') recordHostAudio();
    if (command == 'P') playHostAudio();
    if (command == 'C') playVoiceClips();
    if (command == 'b') {
      phone_client.println(playBeep() ? "BEEPED" : "ABORTED");
    }
    return true;
  }
  return consumed;
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    phone_client.stop();
    delay(100);
    return;
  }
  if (!phone_client.connected()) {
    phone_client.stop();
    pollCradle(false);
    WiFiClient next_client = phone_server.available();
    if (next_client) {
      phone_client = next_client;
      phone_client.setNoDelay(true);
      phone_client.setTimeout(3000);
      command_length = 0;
      cradle_event_pending = false;
      noInterrupts();
      dial_reported_edges = dial_edges;
      interrupts();
      dial_pending_edges = 0;
      dial_turn_at_lift = dial_turn_id;
      Serial.printf("Laptop connected from %s\n",
                    phone_client.remoteIP().toString().c_str());
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
    Serial.printf("I2S read failed: %d, bytes: %u\n", error,
                  static_cast<unsigned>(bytes_read));
    delay(500);
    return;
  }

}
