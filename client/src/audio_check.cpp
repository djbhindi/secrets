#include <Arduino.h>
#include <driver/i2s.h>
#include <driver/adc.h>
#include <driver/rtc_io.h>
#include <math.h>
#include <soc/i2s_reg.h>
#include <soc/soc.h>

constexpr i2s_port_t MIC_PORT = I2S_NUM_0;
constexpr i2s_port_t SPEAKER_PORT = I2S_NUM_1;
constexpr uint32_t MIC_RATE = 32000;
constexpr uint32_t SPEAKER_RATE = 16000;
int32_t mic_samples[512];
int16_t speaker_samples[512];

void playTestTone() {
  constexpr float notes[] = {440.0f, 880.0f, 1320.0f};
  constexpr uint32_t lengths[] = {5600, 5600, 5600};
  for (unsigned note = 0; note < 3; ++note) {
    Serial.printf("Test tone: %u Hz\n", static_cast<unsigned>(notes[note]));
    for (uint32_t played = 0; played < lengths[note];) {
      const size_t frames = min(static_cast<uint32_t>(256), lengths[note] - played);
      for (size_t i = 0; i < frames; ++i) {
        const uint32_t position = played + i;
        const float fade = min(1.0f, min(position / 400.0f,
             (lengths[note] - position) / 800.0f));
        const int16_t sample = 5000 * fade *
            sinf(2.0f * PI * notes[note] * position / SPEAKER_RATE);
        speaker_samples[i * 2] = sample;
        speaker_samples[i * 2 + 1] = sample;
      }
      size_t written = 0;
      i2s_write(SPEAKER_PORT, speaker_samples, frames * 4, &written,
                portMAX_DELAY);
      played += frames;
    }
    delay(20);
  }
  i2s_zero_dma_buffer(SPEAKER_PORT);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Audio diagnostic starting");
  pinMode(4, INPUT_PULLUP);
  adc2_config_channel_atten(ADC2_CHANNEL_0, ADC_ATTEN_DB_12);
  rtc_gpio_pulldown_dis(GPIO_NUM_4);
  rtc_gpio_pullup_en(GPIO_NUM_4);
  i2s_config_t mic = {};
  mic.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  mic.sample_rate = MIC_RATE;
  mic.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  mic.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  mic.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  mic.dma_buf_count = 16;
  mic.dma_buf_len = 256;
  i2s_pin_config_t mic_pins = {};
  mic_pins.bck_io_num = 25;
  mic_pins.ws_io_num = 26;
  mic_pins.data_out_num = I2S_PIN_NO_CHANGE;
  mic_pins.data_in_num = 33;
  esp_err_t error = i2s_driver_install(MIC_PORT, &mic, 0, nullptr);
  if (error == ESP_OK) error = i2s_set_pin(MIC_PORT, &mic_pins);
  if (error != ESP_OK) {
    Serial.printf("Mic setup failed: %d\n", error);
    return;
  }
  REG_SET_FIELD(I2S_TIMING_REG(MIC_PORT), I2S_RX_SD_IN_DELAY, 2);
  REG_SET_BIT(I2S_CONF_REG(MIC_PORT), I2S_RX_MSB_SHIFT);

  i2s_config_t speaker = {};
  speaker.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  speaker.sample_rate = SPEAKER_RATE;
  speaker.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  speaker.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  speaker.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  speaker.dma_buf_count = 12;
  speaker.dma_buf_len = 256;
  speaker.tx_desc_auto_clear = true;
  i2s_pin_config_t speaker_pins = {};
  speaker_pins.bck_io_num = 14;
  speaker_pins.ws_io_num = 27;
  speaker_pins.data_out_num = 22;
  speaker_pins.data_in_num = I2S_PIN_NO_CHANGE;
  error = i2s_driver_install(SPEAKER_PORT, &speaker, 0, nullptr);
  if (error == ESP_OK) error = i2s_set_pin(SPEAKER_PORT, &speaker_pins);
  if (error != ESP_OK) {
    Serial.printf("Speaker setup failed: %d\n", error);
    return;
  }
  Serial.println("Mic and speaker I2S configured; tone repeats every 5 seconds");
}

void loop() {
  static unsigned long last_report = 0;
  static unsigned long last_tone = 0;
  static uint64_t left_square = 0;
  static uint64_t right_square = 0;
  static int64_t left_sum = 0;
  static int64_t right_sum = 0;
  static uint32_t count = 0;
  static int32_t left_peak = 0;
  static int32_t right_peak = 0;
  size_t read = 0;
  if (i2s_read(MIC_PORT, mic_samples, sizeof(mic_samples), &read,
               pdMS_TO_TICKS(100)) == ESP_OK) {
    const size_t words = read / sizeof(int32_t);
    for (size_t i = 0; i + 1 < words; i += 2) {
      const int32_t left = mic_samples[i] >> 16;
      const int32_t right = mic_samples[i + 1] >> 16;
      left_square += static_cast<int64_t>(left) * left;
      right_square += static_cast<int64_t>(right) * right;
      left_sum += left;
      right_sum += right;
      left_peak = max(left_peak, abs(left));
      right_peak = max(right_peak, abs(right));
      ++count;
    }
  }
  const unsigned long now = millis();
  if (now - last_report >= 500 && count) {
    const double left_mean = static_cast<double>(left_sum) / count;
    const double right_mean = static_cast<double>(right_sum) / count;
    const double left_variance = max(0.0, static_cast<double>(left_square) / count - left_mean * left_mean);
    const double right_variance = max(0.0, static_cast<double>(right_square) / count - right_mean * right_mean);
    int cradle_adc = -1;
    const esp_err_t cradle_error = adc2_get_raw(ADC2_CHANNEL_0,
                                                 ADC_WIDTH_BIT_12,
                                                 &cradle_adc);
    Serial.printf("GPIO4=%d ADC=%d status=%d | mic L audio=%lu peak=%ld | R audio=%lu peak=%ld\n",
        digitalRead(4),
        cradle_adc,
        cradle_error,
        static_cast<unsigned long>(sqrt(left_variance)),
        static_cast<long>(left_peak),
        static_cast<unsigned long>(sqrt(right_variance)),
        static_cast<long>(right_peak));
    left_square = right_square = 0;
    left_sum = right_sum = 0;
    left_peak = right_peak = 0;
    count = 0;
    last_report = now;
  }
  if (now - last_tone >= 5000) {
    Serial.println("Playing test chime now");
    playTestTone();
    last_tone = millis();
  }
}
