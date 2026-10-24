#pragma once

// Standalone phone. Messages are standard 16 kHz mono WAV files on microSD.
constexpr int SD_CS_PIN = 13;
constexpr char SD_RECORDING_DIR[] = "/secrets_hotline";
constexpr char SD_TEMP_FILE[] = "/secrets_hotline/RECORD.TMP";
constexpr uint32_t SD_RECORD_SAMPLES = RECORD_RATE * 120;
constexpr uint32_t SD_DIAL_END_TRIM_SAMPLES = RECORD_RATE * 2;
constexpr unsigned long SD_PICKUP_PAUSE_MS = 1500;
constexpr unsigned long SD_CODE_TIMEOUT_MS = 30000;
constexpr unsigned long MENU_REPEAT_MS = 15000;
bool sd_ready = false;
unsigned long sd_mount_attempt_at = 0;
bool sd_previous_up = false;
unsigned long sd_menu_repeat_at = 0;
char sd_entered[4] = {};
uint8_t sd_entered_count = 0;
unsigned long sd_code_started_at = 0;
bool sd_issued[729] = {};
uint16_t sd_next_key = 0;
uint16_t sd_key_stride = 1;

void sdFileName(unsigned key, char *path) {
  snprintf(path, 32, "/secrets_hotline/MSG%03u.WAV", key);
}

bool sdMessageExists(unsigned key) {
  char path[32];
  sdFileName(key, path);
  return SD.exists(path);
}

bool sdPrepareCard() {
  if (!SD.exists(SD_RECORDING_DIR) && !SD.mkdir(SD_RECORDING_DIR)) return false;
  if (SD.exists(SD_TEMP_FILE)) SD.remove(SD_TEMP_FILE);
  return true;
}

unsigned sdKeyAt(uint16_t index) {
  const unsigned n = index % 729;
  return 100 * (n / 81 + 1) + 10 * ((n / 9) % 9 + 1) + n % 9 + 1;
}

bool sdReservedKey(unsigned key) {
  return key == 420 || key == 666 || key == 777 || key == 911 || key == 999;
}

bool sdAllocateKey(unsigned &key) {
  for (unsigned tries = 0; tries < 729; ++tries) {
    const uint16_t index = sd_next_key;
    sd_next_key = (sd_next_key + sd_key_stride) % 729;
    if (sd_issued[index]) continue;
    const unsigned candidate = sdKeyAt(index);
    if (sdReservedKey(candidate)) {
      sd_issued[index] = true;
      continue;
    }
    if (sdMessageExists(candidate)) {
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
  // The final 16 bytes contain playback levels in a standard extra WAV chunk.
  const uint32_t riff_bytes = data_bytes + 36 + 16;
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

enum class SdRecordResult { Failed, TimedOut, DialEnded, HungUp };

SdRecordResult sdRecord(unsigned key) {
  if (SD.exists(SD_TEMP_FILE)) SD.remove(SD_TEMP_FILE);
  File file = SD.open(SD_TEMP_FILE, FILE_WRITE);
  if (!file) return SdRecordResult::Failed;
  uint8_t header[44];
  sdWavHeader(header, 0);
  bool ok = file.write(header, sizeof(header)) == sizeof(header);
  uint32_t captured = 0;
  size_t buffered = 0;
  int32_t minimum = 32767;
  int32_t maximum = -32768;
  int64_t sum = 0;
  uint32_t measured = 0;
  bool dial_ended = false;
  sd_dial_read = sd_dial_write;
  if (ok) discardMicStartup();
  const unsigned long started = millis();
  while (ok && captured < SD_RECORD_SAMPLES && !handsetDown(false)) {
    pollDial();
    while (sd_dial_read != sd_dial_write) {
      const uint8_t edges = sd_dial_queue[sd_dial_read];
      sd_dial_read = (sd_dial_read + 1) % 8;
      if (edges == 20 || edges == 21) dial_ended = true;
    }
    if (dial_ended) break;
    size_t bytes_read = 0;
    if (i2s_read(I2S_PORT, samples, sizeof(samples), &bytes_read,
                 portMAX_DELAY) != ESP_OK) { ok = false; break; }
    const size_t frames = bytes_read / (2 * sizeof(int32_t));
    size_t count = 0;
    for (size_t frame = 0; frame + 1 < frames &&
                           captured + count < SD_RECORD_SAMPLES; frame += 2) {
      const int32_t a = samples[frame * 2] >> 16;
      const int32_t b = samples[(frame + 1) * 2] >> 16;
      recording_chunk[count++] = static_cast<int16_t>((a + b) / 2);
    }
    if (count == 0) { ok = false; break; }
    for (size_t i = 0; i < count; ++i) {
      if (captured + i < RECORD_RATE * 15 / 100) continue;
      const int32_t value = recording_chunk[i];
      minimum = min(minimum, value);
      maximum = max(maximum, value);
      sum += value;
      ++measured;
    }
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
        captured == SD_RECORD_SAMPLES) {
      ok = writeAudioFile(file, recording_file_chunk, buffered);
      buffered = 0;
    }
  }
  const bool hung_up = handsetDown(false);
  PHONE_LOG_PRINTF("SD stop: dial=%u, down=%u, adc=%d, write=%u, samples=%lu\n",
                   dial_ended, hung_up, cradle_adc, ok,
                   static_cast<unsigned long>(captured));
  if (ok && buffered != 0) ok = writeAudioFile(file, recording_file_chunk, buffered);
  ok = ok && captured != 0 && !hung_up;
  if (ok) {
    const int32_t mean = measured ? lround(static_cast<double>(sum) / measured) : 0;
    const int32_t peak = measured ? max(abs(minimum - mean), abs(maximum - mean)) : 0;
    uint8_t levels[16] = {};
    memcpy(levels, dial_ended ? "LVL2" : "LVLS", 4);
    const uint32_t chunk_bytes = 8;
    memcpy(levels + 4, &chunk_bytes, 4);
    memcpy(levels + 8, &mean, 4);
    memcpy(levels + 12, &peak, 4);
    ok = file.write(levels, sizeof(levels)) == sizeof(levels);
  }
  if (ok) {
    sdWavHeader(header, captured);
    ok = file.seek(0) && file.write(header, sizeof(header)) == sizeof(header);
  }
  file.close();
  if (ok) {
    char path[32]; sdFileName(key, path);
    ok = SD.rename(SD_TEMP_FILE, path);
  }
  if (!ok) SD.remove(SD_TEMP_FILE);
  PHONE_LOG_PRINTF("SD capture: %lu samples, %lu ms, %s; down=%u, dial=%u\n",
      static_cast<unsigned long>(captured),
      static_cast<unsigned long>(millis() - started), ok ? "saved" : "aborted",
      hung_up, dial_ended);
  if (!ok) return hung_up ? SdRecordResult::HungUp : SdRecordResult::Failed;
  return dial_ended ? SdRecordResult::DialEnded : SdRecordResult::TimedOut;
}

bool sdPlay(unsigned key) {
  char path[32]; sdFileName(key, path);
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
       data_bytes % 2 == 0 && data_bytes <= SD_RECORD_SAMPLES * 2 &&
       file.size() >= 44 + data_bytes;
  if (!ok) { file.close(); return false; }

  // Recording saved the microphone offset and peak, so playback needs no scan.
  int32_t mean = 0;
  int32_t peak = 0;
  bool have_levels = false;
  bool skip_tail = false;
  if (file.size() >= 44 + data_bytes + 16 && file.seek(44 + data_bytes)) {
    uint8_t levels[16];
    if (file.read(levels, sizeof(levels)) == sizeof(levels) &&
        (memcmp(levels, "LVLS", 4) == 0 || memcmp(levels, "LVL2", 4) == 0) &&
        levels[4] == 8 &&
        levels[5] == 0 && levels[6] == 0 && levels[7] == 0) {
      skip_tail = memcmp(levels, "LVL2", 4) == 0;
      memcpy(&mean, levels + 8, 4);
      memcpy(&peak, levels + 12, 4);
      have_levels = peak >= 0 && peak <= 65535 &&
                    mean >= -32768 && mean <= 32767;
    }
  }
  // Preloaded clips can be ordinary PCM WAV files without the recording levels.
  // Those files play at their authored level; phone recordings retain auto gain.
  const float gain = have_levels && peak ? min(8.0f, 7000.0f / peak) : 1.0f;
  const uint32_t playable_bytes = skip_tail && data_bytes > RECORD_RATE
      ? data_bytes - min(SD_DIAL_END_TRIM_SAMPLES * 2,
                         data_bytes - static_cast<uint32_t>(RECORD_RATE))
      : data_bytes;
  if (!file.seek(44)) { file.close(); return false; }
  if (!playVoiceClip(2)) { file.close(); return false; }
  uint32_t played = 0;
  while (played < playable_bytes && !handsetDown(false)) {
    const size_t bytes = min(static_cast<uint32_t>(sizeof(recording_file_chunk)),
                             playable_bytes - played);
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
                                  playable_bytes / 2 - 1 - sample_index);
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
  sd_code_started_at = 0;
  sd_menu_repeat_at = 0;
  sd_dial_read = sd_dial_write;
  i2s_zero_dma_buffer(SPEAKER_PORT);
}

void sdHandleDigit(unsigned digit) {
  sd_menu_repeat_at = 0;
  if (digit == 0 && sd_entered_count == 0) {
    unsigned key = 0;
    if (!sdAllocateKey(key)) { playVoiceClip(1); return; }
    if (!playVoiceClip(0) || !playBeep()) return;
    const SdRecordResult result = sdRecord(key);
    if (result == SdRecordResult::Failed || result == SdRecordResult::HungUp)
      return;
    if (!playBeep(true)) {
      char path[32]; sdFileName(key, path);
      SD.remove(path);
      return;
    }
    if (!playVoiceClip(3) || !sdSpeakKey(key) ||
        !playVoiceClip(4)) return;
    sdSpeakKey(key);
    return;
  }
  if (sd_entered_count == 0) sd_code_started_at = millis();
  if (digit != 0 && !playVoiceClip(7 + digit - 1)) return;
  sd_entered[sd_entered_count++] = '0' + digit;
  if (sd_entered_count != 3) return;
  sd_entered[3] = 0;
  const unsigned key = (sd_entered[0] - '0') * 100 +
                       (sd_entered[1] - '0') * 10 + sd_entered[2] - '0';
  sd_entered_count = 0;
  sd_code_started_at = 0;
  if (sdMessageExists(key)) {
    if (sdPlay(key)) playBeep();
  } else {
    // The spoken digit clips cover 1-9; a missing code containing 0 gets
    // the generic response instead of announcing the wrong digit.
    if (key % 10 == 0 || (key / 10) % 10 == 0) playVoiceClip(6);
    else if (playVoiceClip(5) && sdSpeakKey(key)) playVoiceClip(6);
  }
}

void setupSdPhone() {
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  SPI.begin(18, 19, 23, SD_CS_PIN);
  sd_mount_attempt_at = millis();
  sd_ready = SD.begin(SD_CS_PIN, SPI, 1000000) && SD.cardType() != CARD_NONE;
  if (sd_ready) sd_ready = sdPrepareCard();
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
    if (sd_ready) sd_ready = sdPrepareCard();
    if (sd_ready) {
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
      const unsigned long lifted_at = millis();
      while (millis() - lifted_at < SD_PICKUP_PAUSE_MS) {
        pollDial();
        if (handsetDown(false) || dial_pending_edges != 0 ||
            sd_dial_read != sd_dial_write) break;
        delay(10);
      }
      if (!handsetDown(false) && dial_pending_edges == 0 &&
          sd_dial_read == sd_dial_write) {
        playMenu(true);
        sd_menu_repeat_at = millis() + MENU_REPEAT_MS;
      }
    }
  }
  if (!sd_ready) { delay(100); return; }
  pollDial();
  if (sd_entered_count && millis() - sd_code_started_at >= SD_CODE_TIMEOUT_MS) {
    sd_entered_count = 0;
    sd_entered[0] = 0;
    sd_code_started_at = 0;
    sd_dial_read = sd_dial_write;
    dial_turn_at_lift = dial_turn_id;
    playMenu(false);
    sd_menu_repeat_at = millis() + MENU_REPEAT_MS;
    return;
  }
  if (sd_dial_read != sd_dial_write) {
    const uint32_t edges = sd_dial_queue[sd_dial_read];
    sd_dial_read = (sd_dial_read + 1) % 8;
    if (edges == 20 || edges == 21) sdHandleDigit(0);
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
