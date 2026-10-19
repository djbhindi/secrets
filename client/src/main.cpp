#include <Arduino.h>
#include <driver/i2s.h>
#include <driver/adc.h>
#include <driver/rtc_io.h>
#include <math.h>
#include <soc/i2s_reg.h>
#include <soc/soc.h>
#include "prompt_pcm.h"

// GPIO pins used for the DOIT ESP32 DEVKIT V1 microphone wiring.
constexpr int PIN_BCLK = 25;
constexpr int PIN_WS = 26;
constexpr int PIN_DATA = 33;
constexpr int PIN_SPEAKER_BCLK = 14;
constexpr int PIN_SPEAKER_WS = 27;
constexpr int PIN_SPEAKER_DATA = 22;
constexpr int PIN_CRADLE = 4;
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
constexpr size_t HOST_PLAY_FRAMES = 128;
constexpr float PROMPT_GAIN = 0.08f;

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
    Serial.printf("Dial RR: %s; edges: %lu\n",
                  level == LOW ? "GROUNDED" : "OPEN",
                  static_cast<unsigned long>(edges));
  }
  if (dial_pending_edges == 0 ||
      static_cast<uint32_t>(micros() - last_edge_us) < DIAL_SETTLE_US) return;

  const uint32_t turn_edges = dial_pending_edges;
  dial_pending_edges = 0;
  ++dial_turn_id;
  if (turn_edges == 20) {
    Serial.printf("TRIGGER (%lu edges; turn %lu)\n",
                  static_cast<unsigned long>(turn_edges),
                  static_cast<unsigned long>(dial_turn_id));
  } else if (turn_edges >= 2 && turn_edges <= 18 && turn_edges % 2 == 0) {
    Serial.printf("DIALED %lu (%lu edges; turn %lu)\n",
                  static_cast<unsigned long>(turn_edges / 2),
                  static_cast<unsigned long>(turn_edges),
                  static_cast<unsigned long>(dial_turn_id));
  } else {
    Serial.printf("Dial unrecognized (%lu edges; turn %lu)\n",
                  static_cast<unsigned long>(turn_edges),
                  static_cast<unsigned long>(dial_turn_id));
  }
}

void pollCradle(bool report = true) {
  if (report && cradle_event_pending) {
    Serial.printf("Cradle %s; ADC: %d\n",
                  cradle_stable == LOW ? "DOWN" : "UP", cradle_adc);
    cradle_event_pending = false;
  }
  const unsigned long now = millis();
  if (now - cradle_sampled_at < 10) return;
  cradle_sampled_at = now;
  if (adc2_get_raw(ADC2_CHANNEL_0, ADC_WIDTH_BIT_12, &cradle_adc) != ESP_OK)
    return;
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
      Serial.printf("Cradle %s; ADC: %d\n",
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

void recordHostAudio() {
  discardMicStartup();
  const uint32_t rate = RECORD_RATE;
  Serial.write(reinterpret_cast<const uint8_t *>("REC1"), 4);
  Serial.write(reinterpret_cast<const uint8_t *>(&rate), sizeof(rate));

  uint16_t packet[FRAMES_PER_READ / 2 + 1] = {};
  uint32_t captured = 0;
  while (captured < HOST_RECORD_SAMPLES) {
    if (handsetDown(false)) {
      packet[0] = 0;
      Serial.write(reinterpret_cast<const uint8_t *>(packet), sizeof(packet));
      Serial.flush();
      pollCradle();
      return;
    }
    size_t bytes_read = 0;
    if (i2s_read(I2S_PORT, samples, sizeof(samples), &bytes_read,
                 portMAX_DELAY) != ESP_OK) {
      packet[0] = UINT16_MAX;
      Serial.write(reinterpret_cast<const uint8_t *>(packet), sizeof(packet));
      Serial.flush();
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
    Serial.write(reinterpret_cast<const uint8_t *>(packet), sizeof(packet));
    captured += count;
  }
  packet[0] = 0;
  Serial.write(reinterpret_cast<const uint8_t *>(packet), sizeof(packet));
  Serial.flush();
}

bool readSerialBytes(uint8_t *destination, size_t count) {
  size_t received = 0;
  while (received < count) {
    const size_t chunk = Serial.readBytes(destination + received,
                                          count - received);
    if (chunk == 0) return false;
    received += chunk;
  }
  return true;
}

void playHostAudio() {
  uint32_t sample_count = 0;
  if (!readSerialBytes(reinterpret_cast<uint8_t *>(&sample_count), 4) ||
      sample_count == 0 || sample_count > RECORD_RATE * 30) {
    Serial.println("PLAY ERROR");
    return;
  }
  uint32_t played = 0;
  while (played < sample_count) {
    pollDial();
    if (handsetDown()) {
      i2s_zero_dma_buffer(SPEAKER_PORT);
      Serial.println("ABORTED");
      return;
    }
    const size_t frames = min(static_cast<uint32_t>(HOST_PLAY_FRAMES),
                              sample_count - played);
    Serial.println("NEXT"); // Let the computer send only one small chunk.
    if (!readSerialBytes(reinterpret_cast<uint8_t *>(host_play_chunk),
                         frames * sizeof(int16_t))) {
      Serial.println("PLAY ERROR");
      return;
    }
    pollDial();
    if (handsetDown()) {
      i2s_zero_dma_buffer(SPEAKER_PORT);
      Serial.println("ABORTED");
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
      Serial.println("PLAY ERROR");
      return;
    }
    played += frames;
  }
  delay(50);
  i2s_zero_dma_buffer(SPEAKER_PORT);
  Serial.println(handsetDown() ? "ABORTED" : "PLAYED");
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
      Serial.println("SPEAKER ERROR");
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
  if (finished) Serial.println(include_intro ? "MENU PLAYED" : "MENU REPEATED");
  else if (dial_interrupted) Serial.println("MENU SKIPPED");
  else if (cradle_stable == LOW) Serial.println("ABORTED");
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
  adc2_config_channel_atten(ADC2_CHANNEL_0, ADC_ATTEN_DB_12);
  rtc_gpio_pulldown_dis(GPIO_NUM_4);
  rtc_gpio_pullup_en(GPIO_NUM_4);
  adc2_get_raw(ADC2_CHANNEL_0, ADC_WIDTH_BIT_12, &cradle_adc);
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
  Serial.printf("Audio memory: prompt=flash, message=RAM; free heap: %u\n",
                ESP.getFreeHeap());
}

bool pollSerialCommand() {
  bool consumed = false;
  while (Serial.available()) {
    consumed = true;
    const int incoming = Serial.read();
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
      Serial.println("HOST MODE");
    }
    if (command == 'o') playMenu(true);
    if (command == 'm') playMenu(false);
    if (command == 'R') recordHostAudio();
    if (command == 'P') playHostAudio();
    if (command == 'b') {
      Serial.println(playBeep() ? "BEEPED" : "ABORTED");
    }
    return true;
  }
  return consumed;
}

void loop() {
  pollDial();
  pollCradle();
  if (pollSerialCommand()) return;

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
