#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "network_config.h"

#define PHONE_CONNECTION_USB 0
#define PHONE_CONNECTION_SHARED_WIFI 1
#define PHONE_CONNECTION_ESP32_AP 2
#define PHONE_CONNECTION_SD 3

#ifndef PHONE_CONNECTION_MODE
#define PHONE_CONNECTION_MODE PHONE_CONNECTION_SHARED_WIFI
#endif

#if PHONE_CONNECTION_MODE != PHONE_CONNECTION_USB && \
    PHONE_CONNECTION_MODE != PHONE_CONNECTION_SHARED_WIFI && \
    PHONE_CONNECTION_MODE != PHONE_CONNECTION_ESP32_AP && \
    PHONE_CONNECTION_MODE != PHONE_CONNECTION_SD
#error "Invalid PHONE_CONNECTION_MODE"
#endif

// In USB mode Serial carries binary audio; diagnostic text must not enter it.
#if PHONE_CONNECTION_MODE == PHONE_CONNECTION_USB
#define PHONE_LOG_PRINTF(...) do {} while (0)
#define PHONE_LOG_PRINTLN(...) do {} while (0)
#else
#define PHONE_LOG_PRINTF(...) Serial.printf(__VA_ARGS__)
#define PHONE_LOG_PRINTLN(...) Serial.println(__VA_ARGS__)
#endif

#if PHONE_CONNECTION_MODE == PHONE_CONNECTION_SHARED_WIFI
#ifndef PHONE_SERVER_HOST
#error "Set PHONE_SERVER_HOST in network_config.h"
#endif
#elif PHONE_CONNECTION_MODE == PHONE_CONNECTION_ESP32_AP
#ifndef PHONE_AP_SSID
#error "Set PHONE_AP_SSID in network_config.h"
#endif
#ifndef PHONE_AP_PASSWORD
#error "Set PHONE_AP_PASSWORD in network_config.h"
#endif
#endif

class PhoneTransport : public Stream {
 public:
  static constexpr uint16_t PORT = 45679;

  void begin() {
#if PHONE_CONNECTION_MODE == PHONE_CONNECTION_USB
    // Serial is the protocol stream in this mode.
#elif PHONE_CONNECTION_MODE == PHONE_CONNECTION_ESP32_AP
    WiFi.mode(WIFI_AP);
    WiFi.setSleep(false);
    if (!WiFi.softAP(PHONE_AP_SSID, PHONE_AP_PASSWORD)) {
      Serial.println("Could not start phone Wi-Fi network");
      while (true) delay(1000);
    }
    listener.begin();
    Serial.printf("Phone network %s ready at %s:%u\n", PHONE_AP_SSID,
                  WiFi.softAPIP().toString().c_str(), PORT);
#else
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
      Serial.printf("Wi-Fi join failed (reason %u)\n",
                    static_cast<unsigned>(info.wifi_sta_disconnected.reason));
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.begin(PHONE_WIFI_SSID, PHONE_WIFI_PASSWORD);
    Serial.printf("Joining Wi-Fi: %s\n", PHONE_WIFI_SSID);
    unsigned long last_report = millis();
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      if (millis() - last_report >= 5000) {
        Serial.printf("Waiting for Wi-Fi; status %d\n", WiFi.status());
        last_report = millis();
      }
    }
    if (!MDNS.begin("secrets-phone")) {
      Serial.println("mDNS name unavailable; use the printed IP address");
    }
    Serial.printf("Phone Wi-Fi: %s at %s; laptop: %s:%u\n",
                  PHONE_WIFI_SSID, WiFi.localIP().toString().c_str(),
                  PHONE_SERVER_HOST, PORT);
#endif
  }

  bool ready() const {
#if PHONE_CONNECTION_MODE == PHONE_CONNECTION_USB
    return true;
#elif PHONE_CONNECTION_MODE == PHONE_CONNECTION_ESP32_AP
    return WiFi.getMode() == WIFI_AP;
#else
    return WiFi.status() == WL_CONNECTED;
#endif
  }

  bool connected() {
#if PHONE_CONNECTION_MODE == PHONE_CONNECTION_USB
    return true;
#else
    return client.connected();
#endif
  }

  bool acceptOrConnect() {
#if PHONE_CONNECTION_MODE == PHONE_CONNECTION_USB
    return true;
#elif PHONE_CONNECTION_MODE == PHONE_CONNECTION_ESP32_AP
    client = listener.available();
    if (!client) return false;
    Serial.printf("Laptop connected from %s\n",
                  client.remoteIP().toString().c_str());
#else
    IPAddress address;
    if (!address.fromString(PHONE_SERVER_HOST)) {
      address = MDNS.queryHost(PHONE_SERVER_HOST, 2000);
    }
    if (address == IPAddress()) {
      Serial.printf("Laptop %s not found; retrying\n", PHONE_SERVER_HOST);
      return false;
    }
    if (!client.connect(address, PORT, 2000)) {
      Serial.printf("Laptop %s:%u unavailable; retrying\n",
                    address.toString().c_str(), PORT);
      return false;
    }
    Serial.printf("Connected to laptop at %s:%u\n",
                  address.toString().c_str(), PORT);
#endif
#if PHONE_CONNECTION_MODE != PHONE_CONNECTION_USB
    client.setNoDelay(true);
    client.setTimeout(3000);
#endif
    return true;
  }

  void stop() {
#if PHONE_CONNECTION_MODE != PHONE_CONNECTION_USB
    client.stop();
#endif
  }

  int available() override { return underlying().available(); }
  int read() override { return underlying().read(); }
  int peek() override { return underlying().peek(); }
  void flush() override { underlying().flush(); }
  size_t write(uint8_t byte) override { return underlying().write(byte); }
  size_t write(const uint8_t *data, size_t count) override {
    return underlying().write(data, count);
  }
  int read(uint8_t *data, size_t count) {
#if PHONE_CONNECTION_MODE == PHONE_CONNECTION_USB
    return Serial.readBytes(data, count);
#else
    return client.read(data, count);
#endif
  }

 private:
  Stream &underlying() {
#if PHONE_CONNECTION_MODE == PHONE_CONNECTION_USB
    return Serial;
#else
    return client;
#endif
  }
#if PHONE_CONNECTION_MODE != PHONE_CONNECTION_USB
  WiFiClient client;
#endif
#if PHONE_CONNECTION_MODE == PHONE_CONNECTION_ESP32_AP
  WiFiServer listener{PORT};
#endif
};
