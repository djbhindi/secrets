#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

constexpr int SD_SCK = 18;
constexpr int SD_MISO = 19;
constexpr int SD_MOSI = 23;
constexpr int SD_CS = 13;
constexpr char TEST_FILE[] = "/secrets_hotline/WRITECHK.TMP";
constexpr size_t TEST_BYTES = 512 * 1024;
uint8_t test_chunk[4096];

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
  Serial.printf("Free space: %llu MB\n",
                (SD.totalBytes() - SD.usedBytes()) / (1024ULL * 1024ULL));
  if (!SD.exists("/secrets_hotline") && !SD.mkdir("/secrets_hotline")) {
    Serial.println("FAIL: Could not create message directory.");
    return;
  }

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
  for (size_t i = 0; i < sizeof(test_chunk); ++i)
    test_chunk[i] = static_cast<uint8_t>(i);
  size_t written = 0;
  while (written < TEST_BYTES) {
    const size_t bytes = file.write(test_chunk, sizeof(test_chunk));
    if (bytes != sizeof(test_chunk)) {
      Serial.printf("FAIL: Write stopped at %u bytes (block returned %u).\n",
                    static_cast<unsigned>(written), static_cast<unsigned>(bytes));
      file.close();
      SD.remove(TEST_FILE);
      return;
    }
    written += bytes;
  }
  file.close();
  if (written != TEST_BYTES) {
    Serial.println("FAIL: Could not write complete test data.");
    return;
  }

  file = SD.open(TEST_FILE, FILE_READ);
  if (!file) {
    Serial.println("FAIL: Could not reopen test file.");
    return;
  }
  bool matches = file.size() == TEST_BYTES;
  size_t checked = 0;
  while (matches && checked < TEST_BYTES) {
    uint8_t read_chunk[4096];
    if (file.read(read_chunk, sizeof(read_chunk)) != sizeof(read_chunk)) {
      matches = false;
      break;
    }
    for (size_t i = 0; i < sizeof(read_chunk); ++i) {
      if (read_chunk[i] != test_chunk[i]) { matches = false; break; }
    }
    checked += sizeof(read_chunk);
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
