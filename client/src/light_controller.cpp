#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <driver/rmt.h>
#include <math.h>
#include "network_config.h"

#ifndef LIGHT_DATA_PIN
#define LIGHT_DATA_PIN 13
#endif
#ifndef LIGHT_PIXEL_COUNT
#define LIGHT_PIXEL_COUNT 50
#endif

constexpr uint16_t STATE_PORT = 45680;
constexpr rmt_channel_t LIGHT_CHANNEL = RMT_CHANNEL_0;
constexpr uint32_t FRAME_MS = 33;
constexpr uint32_t FADE_MS = 1200;
constexpr uint32_t DEMO_MS = 4000;
constexpr uint32_t WIFI_RETRY_MS = 5000;
constexpr uint32_t STATE_TIMEOUT_MS = 4000;
constexpr float TAU = 6.2831853f;

enum class PhoneState : uint8_t { Down, Up, Recording, Playback };
struct Rgb { uint8_t red, green, blue; };
struct Palette { Rgb base, warm, cool; };
constexpr Palette PALETTES[] = {
    {{180, 55, 0}, {225, 100, 8}, {150, 30, 3}},    // orange and amber
    {{200, 160, 20}, {225, 190, 30}, {175, 125, 8}}, // yellow and gold
    {{190, 0, 0}, {225, 30, 8}, {155, 0, 24}},       // red and crimson
    {{125, 25, 100}, {165, 40, 105}, {95, 15, 130}} // warm purple
};

PhoneState state = PhoneState::Down;
WiFiUDP udp;
bool demo = false;
bool udp_ready = false;
bool remote_state_seen = false;
bool transitioning = false;
uint32_t last_wifi_attempt = 0;
uint32_t last_packet_at = 0;
uint32_t last_demo_change = 0;
uint32_t fade_started_at = 0;
uint32_t last_frame_at = 0;
Rgb displayed[LIGHT_PIXEL_COUNT] = {};
Rgb fade_from[LIGHT_PIXEL_COUNT] = {};
rmt_item32_t pixels[LIGHT_PIXEL_COUNT * 24];

const char *stateName(PhoneState value) {
  switch (value) {
    case PhoneState::Down: return "down";
    case PhoneState::Up: return "up";
    case PhoneState::Recording: return "recording";
    case PhoneState::Playback: return "playback";
  }
  return "unknown";
}

Rgb blend(Rgb from, Rgb to, float amount) {
  return {static_cast<uint8_t>(roundf(from.red + (to.red - from.red) * amount)),
          static_cast<uint8_t>(roundf(from.green + (to.green - from.green) * amount)),
          static_cast<uint8_t>(roundf(from.blue + (to.blue - from.blue) * amount))};
}

Rgb colorAt(size_t pixel, uint32_t now) {
  const Palette &palette = PALETTES[static_cast<uint8_t>(state)];
  const float seconds = now / 1000.0f;
  const float position = static_cast<float>(pixel) / LIGHT_PIXEL_COUNT;
  const float wave = sinf(TAU * (position * 1.4f - seconds / 10.0f));
  const Rgb neighbor = wave >= 0 ? palette.warm : palette.cool;
  const Rgb mixed = blend(palette.base, neighbor, fabsf(wave) * 0.55f);
  const float breath = 0.86f + 0.11f * sinf(TAU * seconds / 4.8f);
  return {static_cast<uint8_t>(roundf(mixed.red * breath)),
          static_cast<uint8_t>(roundf(mixed.green * breath)),
          static_cast<uint8_t>(roundf(mixed.blue * breath))};
}

void writePixels() {
  size_t item = 0;
  for (const Rgb &color : displayed) {
    const uint8_t channels[] = {color.red, color.green, color.blue}; // This strip is RGB.
    for (uint8_t channel : channels) {
      for (int bit = 7; bit >= 0; --bit) {
        const bool one = channel & (1u << bit);
        pixels[item].level0 = 1;
        pixels[item].duration0 = one ? 8 : 4;
        pixels[item].level1 = 0;
        pixels[item].duration1 = one ? 5 : 9;
        ++item;
      }
    }
  }
  if (rmt_write_items(LIGHT_CHANNEL, pixels, item, true) != ESP_OK)
    Serial.println("LED transmission failed");
  delayMicroseconds(80);
}

void renderFrame(uint32_t now) {
  const float progress = constrain(static_cast<float>(now - fade_started_at) / FADE_MS,
                                   0.0f, 1.0f);
  const float eased = progress * progress * (3.0f - 2.0f * progress);
  for (size_t i = 0; i < LIGHT_PIXEL_COUNT; ++i) {
    const Rgb target = colorAt(i, now);
    displayed[i] = transitioning ? blend(fade_from[i], target, eased) : target;
  }
  writePixels();
  if (transitioning && progress >= 1.0f) transitioning = false;
  last_frame_at = now;
}

void setState(PhoneState value) {
  if (state == value) return; // The phone repeats its state every second.
  memcpy(fade_from, displayed, sizeof(displayed));
  fade_started_at = millis();
  transitioning = true;
  state = value;
  Serial.printf("Light state: %s\n", stateName(value));
}

bool parseState(const char *text, PhoneState &result) {
  if (!strcmp(text, "down")) result = PhoneState::Down;
  else if (!strcmp(text, "up")) result = PhoneState::Up;
  else if (!strcmp(text, "recording")) result = PhoneState::Recording;
  else if (!strcmp(text, "playback")) result = PhoneState::Playback;
  else return false;
  return true;
}

void tryWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (last_wifi_attempt && millis() - last_wifi_attempt < WIFI_RETRY_MS) return;
  last_wifi_attempt = millis();
  WiFi.begin(PHONE_AP_SSID, PHONE_AP_PASSWORD);
  Serial.printf("Joining phone network %s\n", PHONE_AP_SSID);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  rmt_config_t config = RMT_DEFAULT_CONFIG_TX(static_cast<gpio_num_t>(LIGHT_DATA_PIN),
                                               LIGHT_CHANNEL);
  config.clk_div = 8; // 10 MHz RMT clock gives 800 kHz WS2811 timing.
  if (rmt_config(&config) != ESP_OK || rmt_driver_install(LIGHT_CHANNEL, 0, 0) != ESP_OK) {
    Serial.println("LED output setup failed");
    while (true) delay(1000);
  }
  renderFrame(millis()); // Orange immediately, even without Wi-Fi.
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  Serial.println("Default: breathing orange. Send 0-3 to test states, a to cycle.");
  tryWifi();
}

void loop() {
  if (Serial.available()) {
    const char command = Serial.read();
    PhoneState selected = state;
    bool valid = true;
    switch (command) {
      case '0': case 'd': selected = PhoneState::Down; break;
      case '1': case 'u': selected = PhoneState::Up; break;
      case '2': case 'r': selected = PhoneState::Recording; break;
      case '3': case 'p': selected = PhoneState::Playback; break;
      case 'a': demo = true; last_demo_change = millis(); valid = false; break;
      default: valid = false; break;
    }
    if (valid) { demo = false; remote_state_seen = false; setState(selected); }
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!udp_ready) {
      udp_ready = udp.begin(STATE_PORT);
      Serial.printf("Phone network joined at %s; state receiver %s\n",
                    WiFi.localIP().toString().c_str(),
                    udp_ready ? "ready" : "failed");
    }
    if (udp_ready) {
      const int packet_size = udp.parsePacket();
      if (packet_size > 0) {
        char text[24] = {};
        const int bytes = udp.read(text, sizeof(text) - 1);
        if (bytes > 0) {
          PhoneState incoming;
          if (parseState(text, incoming)) {
            demo = false;
            remote_state_seen = true;
            last_packet_at = millis();
            setState(incoming);
          }
        }
        while (udp.available()) udp.read();
      }
    }
  } else {
    if (udp_ready) { udp.stop(); udp_ready = false; }
    tryWifi();
  }

  if (remote_state_seen && millis() - last_packet_at >= STATE_TIMEOUT_MS) {
    remote_state_seen = false;
    setState(PhoneState::Down);
  }
  if (demo && millis() - last_demo_change >= DEMO_MS) {
    last_demo_change = millis();
    setState(static_cast<PhoneState>((static_cast<uint8_t>(state) + 1) % 4));
  }
  const uint32_t now = millis();
  if (now - last_frame_at >= FRAME_MS) renderFrame(now);
  delay(5);
}
