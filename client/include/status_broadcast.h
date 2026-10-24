#pragma once

#include <WiFi.h>
#include <WiFiUdp.h>

#ifndef PHONE_STATUS_LIGHTS
#define PHONE_STATUS_LIGHTS 0
#endif

#if PHONE_CONNECTION_MODE == PHONE_CONNECTION_SD && PHONE_STATUS_LIGHTS
#if PHONE_CRADLE_PIN != 32
#error "Status Wi-Fi needs cradle sensing on GPIO32 (ADC1)"
#endif
#ifndef PHONE_AP_SSID
#error "Set PHONE_AP_SSID in network_config.h"
#endif
#ifndef PHONE_AP_PASSWORD
#error "Set PHONE_AP_PASSWORD in network_config.h"
#endif

enum class PhoneLightState : uint8_t { Down, Up, Recording, Playback };
WiFiUDP status_udp;
PhoneLightState status_state = PhoneLightState::Down;
unsigned long status_last_sent = 0;
bool status_network_ready = false;

const char *phoneLightName(PhoneLightState value) {
  switch (value) {
    case PhoneLightState::Down: return "down";
    case PhoneLightState::Up: return "up";
    case PhoneLightState::Recording: return "recording";
    case PhoneLightState::Playback: return "playback";
  }
  return "down";
}

void beginStatusBroadcast() {
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  status_network_ready = WiFi.softAP(PHONE_AP_SSID, PHONE_AP_PASSWORD);
  if (status_network_ready) {
    status_udp.begin(45681);
    Serial.printf("Phone status network %s ready at %s\n", PHONE_AP_SSID,
                  WiFi.softAPIP().toString().c_str());
  } else {
    Serial.println("Phone status Wi-Fi failed; SD phone stays available");
  }
}

void tickStatusBroadcast() {
  if (!status_network_ready || WiFi.softAPgetStationNum() == 0) return;
  if (status_last_sent && millis() - status_last_sent < 1000) return;
  // The default ESP32 softAP address is 192.168.4.1/24.
  if (status_udp.beginPacket(IPAddress(192, 168, 4, 255), 45680)) {
    status_udp.print(phoneLightName(status_state));
    status_udp.endPacket();
    status_last_sent = millis();
  }
}

void broadcastPhoneState(PhoneLightState value) {
  if (status_state != value) {
    status_state = value;
    status_last_sent = 0;
    Serial.printf("Phone state: %s\n", phoneLightName(value));
  }
  tickStatusBroadcast();
}
#else
enum class PhoneLightState : uint8_t { Down, Up, Recording, Playback };
void beginStatusBroadcast() {}
void tickStatusBroadcast() {}
void broadcastPhoneState(PhoneLightState) {}
#endif
