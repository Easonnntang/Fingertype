#include "ota.h"

#include <Update.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include "config.h"

namespace ota {

namespace {
bool running = false;
size_t total = 0, done = 0;
uint8_t expected[32];
mbedtls_sha256_context sha;
uint32_t lastAt = 0;

// 边收边找固件里的识别标记 FIRMWARE_MARKER，没找到的不是本项目的固件，拒绝切换
const char MARK[] = FIRMWARE_MARKER;
constexpr size_t MARK_LEN = sizeof(MARK) - 1;
size_t markPos = 0;
bool markFound = false;

void scanMarker(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n && !markFound; i++) {
    if (p[i] == uint8_t(MARK[markPos])) {
      if (++markPos == MARK_LEN) markFound = true;
    } else {
      markPos = p[i] == uint8_t(MARK[0]) ? 1 : 0;  // 标记里没有重复的前缀，这样回退就够了
    }
  }
}

bool parseHex(const String& s, uint8_t out[32]) {
  if (s.length() != 64) return false;
  for (int i = 0; i < 32; i++) {
    char buf[3] = {s[2 * i], s[2 * i + 1], 0};
    char* end = nullptr;
    out[i] = strtoul(buf, &end, 16);
    if (end != buf + 2) return false;
  }
  return true;
}

void stop() {
  if (running) mbedtls_sha256_free(&sha);
  running = false;
  total = done = 0;
}
}  // namespace

bool begin(size_t size, const String& sha256, const char*& error) {
  abort();
  if (size < 1024 || !parseHex(sha256, expected)) { error = "bad_value"; return false; }
  if (!Update.begin(size, U_FLASH)) { error = "ota_too_large"; return false; }
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  running = true;
  total = size;
  done = 0;
  markPos = 0;
  markFound = false;
  lastAt = millis();
  return true;
}

bool write(size_t offset, const String& base64, size_t& written, const char*& error) {
  written = done;
  if (!running) { error = "ota_not_started"; return false; }
  if (offset != done) { error = "ota_offset"; return false; }  // 网页会按回复里的 off 重发
  static uint8_t buf[OTA_CHUNK + 4];
  size_t n = 0;
  if (mbedtls_base64_decode(buf, sizeof(buf), &n, reinterpret_cast<const uint8_t*>(base64.c_str()), base64.length()) != 0 ||
      n == 0 || done + n > total) {
    error = "bad_value";
    return false;
  }
  if (done == 0 && buf[0] != 0xE9) {  // ESP32 固件文件的第一个字节固定是 0xE9
    abort();
    error = "ota_bad_image";
    return false;
  }
  if (Update.write(buf, n) != n) {
    abort();
    error = "ota_write_failed";
    return false;
  }
  mbedtls_sha256_update(&sha, buf, n);
  scanMarker(buf, n);
  done += n;
  written = done;
  lastAt = millis();
  return true;
}

bool finish(const char*& error) {
  if (!running) { error = "ota_not_started"; return false; }
  if (done != total) { error = "ota_incomplete"; return false; }
  uint8_t actual[32];
  mbedtls_sha256_finish(&sha, actual);
  if (memcmp(actual, expected, 32) != 0) { abort(); error = "ota_checksum"; return false; }
  if (!markFound) { abort(); error = "ota_bad_image"; return false; }
  if (!Update.end(true)) { stop(); error = "ota_write_failed"; return false; }  // 设为下次启动的固件
  stop();
  return true;
}

void abort() {
  if (!running) return;
  Update.abort();
  stop();
}

bool active() { return running; }
size_t size() { return total; }

void loop() {
  if (running && millis() - lastAt > OTA_IDLE_TIMEOUT_MS) abort();
}

}  // namespace ota
