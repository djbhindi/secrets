#include <Arduino.h>

constexpr int DIAL_PIN = 5;
constexpr uint32_t BOUNCE_US = 3000;
constexpr uint32_t SETTLE_US = 250000;

volatile uint32_t dial_edges = 0;
volatile uint32_t last_edge_us = 0;
uint32_t reported_edges = 0;

void IRAM_ATTR onDialEdge() {
  const uint32_t now = micros();
  if (now - last_edge_us < BOUNCE_US) return;
  last_edge_us = now;
  ++dial_edges;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(DIAL_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(DIAL_PIN), onDialEdge, CHANGE);
  Serial.printf("Dial check ready; GPIO5=%d. Dial normally; cradle position is ignored.\n",
                digitalRead(DIAL_PIN));
}

void loop() {
  noInterrupts();
  const uint32_t edges = dial_edges;
  const uint32_t last = last_edge_us;
  interrupts();
  if (edges != reported_edges && static_cast<uint32_t>(micros() - last) >= SETTLE_US) {
    const uint32_t turn_edges = edges - reported_edges;
    reported_edges = edges;
    if (turn_edges == 20) {
      Serial.printf("Dial 0: %lu edges; GPIO5=%d\n",
                    static_cast<unsigned long>(turn_edges), digitalRead(DIAL_PIN));
    } else if (turn_edges >= 2 && turn_edges <= 18 && turn_edges % 2 == 0) {
      Serial.printf("Dial %lu: %lu edges; GPIO5=%d\n",
                    static_cast<unsigned long>(turn_edges / 2),
                    static_cast<unsigned long>(turn_edges), digitalRead(DIAL_PIN));
    } else {
      Serial.printf("Unrecognized dial: %lu edges; GPIO5=%d\n",
                    static_cast<unsigned long>(turn_edges), digitalRead(DIAL_PIN));
    }
  }
  delay(10);
}
