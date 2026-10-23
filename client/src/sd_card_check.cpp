#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

constexpr int SD_SCK = 18;
constexpr int SD_MISO = 19;
constexpr int SD_MOSI = 23;
constexpr int SD_CS = 13;
constexpr char TEST_FILE[] = "/rotary_sd_check.txt";
constexpr char TEST_DATA[] = "Rotary SD card read/write check\n";

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("SD card check starting");

  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI, 1000000)) {
    Serial.println("FAIL: SD card mount failed. Check wiring and FAT32 formatting.");
    return;
  }
  if (SD.cardType() == CARD_NONE) {
    Serial.println("FAIL: No SD card detected.");
    return;
  }
  Serial.printf("Card size: %llu MB\n", SD.cardSize() / (1024ULL * 1024ULL));

  // Leave all card data except the diagnostic's own file untouched.
  if (SD.exists(TEST_FILE) && !SD.remove(TEST_FILE)) {
    Serial.println("FAIL: Could not remove the previous test file.");
    return;
  }
  File file = SD.open(TEST_FILE, FILE_WRITE);
  if (!file) {
    Serial.println("FAIL: Could not create test file.");
    return;
  }
  const size_t written = file.write(
      reinterpret_cast<const uint8_t *>(TEST_DATA), sizeof(TEST_DATA) - 1);
  file.close();
  if (written != sizeof(TEST_DATA) - 1) {
    Serial.println("FAIL: Could not write complete test data.");
    return;
  }

  file = SD.open(TEST_FILE, FILE_READ);
  if (!file) {
    Serial.println("FAIL: Could not reopen test file.");
    return;
  }
  bool matches = file.size() == sizeof(TEST_DATA) - 1;
  for (size_t i = 0; matches && i < sizeof(TEST_DATA) - 1; ++i) {
    if (file.read() != TEST_DATA[i]) matches = false;
  }
  file.close();
  if (!matches) {
    Serial.println("FAIL: Test file did not read back correctly.");
    return;
  }
  if (!SD.remove(TEST_FILE)) {
    Serial.println("FAIL: Read/write passed, but test file cleanup failed.");
    return;
  }
  Serial.println("PASS: SD card mounted; write, read, and cleanup succeeded.");
}

void loop() {}
