#pragma once

// Standalone phone. Messages are standard 16 kHz mono WAV files on microSD.
constexpr int SD_CS_PIN = 13;
constexpr char SD_TEMP_FILE[] = "/RECORD.TMP";
constexpr unsigned long MENU_REPEAT_MS = 15000;
bool sd_ready = false;
unsigned long sd_mount_attempt_at = 0;
bool sd_previous_up = false;
unsigned long sd_menu_repeat_at = 0;
char sd_entered[4] = {};
uint8_t sd_entered_count = 0;
bool sd_issued[729] = {};
uint16_t sd_next_key = 0;
uint16_t sd_key_stride = 1;

void sdFileName(unsigned key, char *path) {
  snprintf(path, 13, "/MSG%03u.WAV", key);
}

unsigned sdKeyAt(uint16_t index) {
  const unsigned n = index % 729;
  return 100 * (n / 81 + 1) + 10 * ((n / 9) % 9 + 1) + n % 9 + 1;
}

bool sdAllocateKey(unsigned &key) {
  for (unsigned tries = 0; tries < 729; ++tries) {
    const uint16_t index = sd_next_key;
    sd_next_key = (sd_next_key + sd_key_stride) % 729;
    if (sd_issued[index]) continue;
    const unsigned candidate = sdKeyAt(index);
    char path[13];
    sdFileName(candidate, path);
    if (SD.exists(path)) {
      sd_issued[index] = true;
      continue;
    }
    sd_issued[index] = true;
    key = candidate;
    return true;
  }
  return false;
}

// RIFF PCM header; field sizes are explicit so the file opens on any computer.
void sdWavHeader(uint8_t *header, uint32_t samples_count) {
  memset(header, 0, 44);
  memcpy(header, "RIFF", 4);
  const uint32_t data_bytes = samples_count * 2;
  const uint32_t riff_bytes = data_bytes + 36;
  memcpy(header + 4, &riff_bytes, 4);
  memcpy(header + 8, "WAVEfmt ", 8);
  const uint32_t fmt_bytes = 16;
  memcpy(header + 16, &fmt_bytes, 4);
  header[20] = 1; header[22] = 1; // PCM, mono
  const uint32_t rate = RECORD_RATE;
  const uint32_t byte_rate = RECORD_RATE * 2;
  memcpy(header + 24, &rate, 4);
  memcpy(header + 28, &byte_rate, 4);
  header[32] = 2; header[34] = 16; // block alignment, bits/sample
  memcpy(header + 36, "data", 4);
  memcpy(header + 40, &data_bytes, 4);
}

bool sdRecord(unsigned key) {
  if (SD.exists(SD_TEMP_FILE)) SD.remove(SD_TEMP_FILE);
  File file = SD.open(SD_TEMP_FILE, FILE_WRITE);
  if (!file) return false;
  uint8_t header[44];
  sdWavHeader(header, HOST_RECORD_SAMPLES);
  bool ok = file.write(header, sizeof(header)) == sizeof(header);
  uint32_t captured = 0;
  size_t buffered = 0;
  if (ok) discardMicStartup();
  const unsigned long started = millis();
  while (ok && captured < HOST_RECORD_SAMPLES && !handsetDown(false)) {
    size_t bytes_read = 0;
    if (i2s_read(I2S_PORT, samples, sizeof(samples), &bytes_read,
                 portMAX_DELAY) != ESP_OK) { ok = false; break; }
    const size_t frames = bytes_read / (2 * sizeof(int32_t));
    size_t count = 0;
    for (size_t frame = 0; frame + 1 < frames &&
                           captured + count < HOST_RECORD_SAMPLES; frame += 2) {
      const int32_t a = samples[frame * 2] >> 16;
      const int32_t b = samples[(frame + 1) * 2] >> 16;
      recording_chunk[count++] = static_cast<int16_t>((a + b) / 2);
    }
    if (count == 0) { ok = false; break; }
    const size_t bytes = count * sizeof(int16_t);
    if (buffered + bytes > sizeof(recording_file_chunk)) {
      ok = writeAudioFile(file, recording_file_chunk, buffered);
      buffered = 0;
    }
    if (!ok) break;
    memcpy(recording_file_chunk + buffered, recording_chunk, bytes);
    buffered += bytes;
    captured += count;
    if (buffered == sizeof(recording_file_chunk) ||
        captured == HOST_RECORD_SAMPLES) {
      ok = writeAudioFile(file, recording_file_chunk, buffered);
      buffered = 0;
    }
  }
  file.close();
  ok = ok && captured == HOST_RECORD_SAMPLES && !handsetDown(false);
  if (ok) {
    char path[13]; sdFileName(key, path);
    ok = SD.rename(SD_TEMP_FILE, path);
  }
  if (!ok) SD.remove(SD_TEMP_FILE);
  PHONE_LOG_PRINTF("SD capture: %lu samples, %lu ms, %s\n",
      static_cast<unsigned long>(captured),
      static_cast<unsigned long>(millis() - started), ok ? "saved" : "aborted");
  return ok;
}

bool sdPlay(unsigned key) {
  char path[13]; sdFileName(key, path);
  File file = SD.open(path, FILE_READ);
  if (!file) return false;
  uint8_t header[44];
  bool ok = file.read(header, sizeof(header)) == sizeof(header) &&
      memcmp(header, "RIFF", 4) == 0 &&
      memcmp(header + 8, "WAVEfmt ", 8) == 0 &&
      memcmp(header + 36, "data", 4) == 0 &&
      header[20] == 1 && header[22] == 1 && header[34] == 16;
  uint32_t rate = 0, data_bytes = 0;
  memcpy(&rate, header + 24, 4);
  memcpy(&data_bytes, header + 40, 4);
  ok = ok && rate == RECORD_RATE && data_bytes > 0 &&
       data_bytes % 2 == 0 && data_bytes <= HOST_RECORD_SAMPLES * 2 &&
       file.size() >= 44 + data_bytes;
  if (!ok) { file.close(); return false; }

  // Match the laptop's playback: remove the microphone's DC offset before
  // setting voice gain. Check the file before announcing playback.
  int32_t minimum = 32767;
  int32_t maximum = -32768;
  int64_t sum = 0;
  uint32_t measured = 0;
  uint32_t checked = 0;
  while (checked < data_bytes) {
    const size_t bytes = min(static_cast<uint32_t>(sizeof(recording_file_chunk)),
                             data_bytes - checked);
    if (file.read(recording_file_chunk, bytes) != bytes) {
      file.close(); return false;
    }
    for (size_t i = 0; i < bytes / 2; ++i) {
      if (checked / 2 + i < RECORD_RATE * 15 / 100) continue;
      int16_t sample;
      memcpy(&sample, recording_file_chunk + i * 2, sizeof(sample));
      const int32_t value = sample;
      minimum = min(minimum, value);
      maximum = max(maximum, value);
      sum += value;
      ++measured;
    }
    checked += bytes;
  }
  const int32_t mean = measured ? lround(static_cast<double>(sum) / measured) : 0;
  const int32_t peak = measured ? max(abs(minimum - mean), abs(maximum - mean)) : 0;
  const float gain = peak ? min(8.0f, 7000.0f / peak) : 1.0f;
  file.seek(44);
  if (!playVoiceClip(2)) { file.close(); return false; }
  uint32_t played = 0;
  while (played < data_bytes && !handsetDown(false)) {
    const size_t bytes = min(static_cast<uint32_t>(sizeof(recording_file_chunk)),
                             data_bytes - played);
    if (file.read(recording_file_chunk, bytes) != bytes) {
      ok = false; break;
    }
    const size_t frames = bytes / 2;
    for (size_t offset = 0; offset < frames; offset += FRAMES_PER_READ) {
      if (handsetDown(false)) { ok = false; break; }
      const size_t block = min(static_cast<size_t>(FRAMES_PER_READ), frames - offset);
      memcpy(host_play_chunk, recording_file_chunk + offset * 2,
             block * sizeof(int16_t));
      for (size_t i = 0; i < block; ++i) {
        const uint32_t sample_index = played / 2 + offset + i;
        const uint32_t edge = min(sample_index,
                                  data_bytes / 2 - 1 - sample_index);
        const float fade = min(1.0f, edge / 160.0f);
        const int32_t scaled = static_cast<int32_t>(
            (host_play_chunk[i] - mean) * gain * fade);
        const int16_t value = constrain(scaled, -32768, 32767);
        stereo_chunk[2 * i] = value;
        stereo_chunk[2 * i + 1] = value;
      }
      size_t written = 0;
      if (i2s_write(SPEAKER_PORT, stereo_chunk, block * 4,
                    &written, portMAX_DELAY) != ESP_OK || written != block * 4) {
        ok = false; break;
      }
    }
    if (!ok) break;
    played += bytes;
  }
  file.close();
  if (handsetDown(false)) ok = false;
  delay(50);
  i2s_zero_dma_buffer(SPEAKER_PORT);
  PHONE_LOG_PRINTF("SD playback: %s, mean %ld, gain %.2f, %s\n", path,
                   static_cast<long>(mean), gain,
                   ok ? "complete" : "aborted");
  return ok;
}

bool sdSpeakKey(unsigned key) {
  return playVoiceClip(7 + key / 100 - 1) &&
         playVoiceClip(7 + (key / 10) % 10 - 1) &&
         playVoiceClip(7 + key % 10 - 1);
}

void sdResetCall() {
  sd_entered_count = 0;
  sd_entered[0] = 0;
  sd_menu_repeat_at = 0;
  sd_dial_read = sd_dial_write;
  i2s_zero_dma_buffer(SPEAKER_PORT);
}

void sdHandleDigit(unsigned digit) {
  sd_menu_repeat_at = 0;
  if (digit == 0) {
    sd_entered_count = 0;
    unsigned key = 0;
    if (!sdAllocateKey(key)) { playVoiceClip(1); return; }
    if (!playVoiceClip(0) || !playBeep()) return;
    if (!sdRecord(key)) return;
    if (!playBeep()) {
      char path[13]; sdFileName(key, path);
      SD.remove(path);
      return;
    }
    if (!playVoiceClip(3) || !sdSpeakKey(key) ||
        !playVoiceClip(4)) return;
    sdSpeakKey(key);
    return;
  }
  if (!playVoiceClip(7 + digit - 1)) return;
  sd_entered[sd_entered_count++] = '0' + digit;
  if (sd_entered_count != 3) return;
  sd_entered[3] = 0;
  const unsigned key = (sd_entered[0] - '0') * 100 +
                       (sd_entered[1] - '0') * 10 + sd_entered[2] - '0';
  sd_entered_count = 0;
  char path[13]; sdFileName(key, path);
  if (SD.exists(path)) {
    if (sdPlay(key)) playBeep();
  } else {
    if (playVoiceClip(5) && sdSpeakKey(key)) playVoiceClip(6);
  }
}

void setupSdPhone() {
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  SPI.begin(18, 19, 23, SD_CS_PIN);
  sd_mount_attempt_at = millis();
  sd_ready = SD.begin(SD_CS_PIN, SPI, 1000000) && SD.cardType() != CARD_NONE;
  if (sd_ready && SD.exists(SD_TEMP_FILE)) SD.remove(SD_TEMP_FILE);
  sd_next_key = esp_random() % 729;
  do { sd_key_stride = 1 + esp_random() % 728; } while (sd_key_stride % 3 == 0);
  sd_previous_up = cradle_stable == HIGH;
  PHONE_LOG_PRINTF("SD phone: %s; card %llu MB; free heap %u\n",
                   sd_ready ? "ready" : "mount failed",
                   sd_ready ? SD.cardSize() / (1024ULL * 1024ULL) : 0,
                   ESP.getFreeHeap());
}

void loopSdPhone() {
  if (!sd_ready && millis() - sd_mount_attempt_at >= 3000) {
    sd_mount_attempt_at = millis();
    SD.end();
    digitalWrite(SD_CS_PIN, HIGH);
    sd_ready = SD.begin(SD_CS_PIN, SPI, 1000000) && SD.cardType() != CARD_NONE;
    if (sd_ready) {
      if (SD.exists(SD_TEMP_FILE)) SD.remove(SD_TEMP_FILE);
      PHONE_LOG_PRINTF("SD card mounted: %llu MB\n",
                       SD.cardSize() / (1024ULL * 1024ULL));
    } else {
      PHONE_LOG_PRINTLN("SD card mount retry failed");
    }
  }
  pollCradle();
  pollDial();
  const bool up = cradle_stable == HIGH;
  if (!up) {
    if (sd_previous_up) sdResetCall();
    sd_previous_up = false;
    delay(10);
    return;
  }
  if (!sd_previous_up) {
    sd_previous_up = true;
    sdResetCall();
    if (sd_ready) {
      playMenu(true);
      sd_menu_repeat_at = millis() + MENU_REPEAT_MS;
    }
  }
  if (!sd_ready) { delay(100); return; }
  pollDial();
  if (sd_dial_read != sd_dial_write) {
    const uint32_t edges = sd_dial_queue[sd_dial_read];
    sd_dial_read = (sd_dial_read + 1) % 8;
    if (edges == 20) sdHandleDigit(0);
    else if (edges >= 2 && edges <= 18 && edges % 2 == 0)
      sdHandleDigit(edges / 2);
    return;
  }
  if (sd_menu_repeat_at && static_cast<int32_t>(millis() - sd_menu_repeat_at) >= 0) {
    dial_turn_at_lift = dial_turn_id;
    playMenu(false);
    sd_menu_repeat_at = millis() + MENU_REPEAT_MS;
  }
  delay(10);
}
