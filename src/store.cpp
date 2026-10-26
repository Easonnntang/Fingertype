#include "store.h"

#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/gcm.h>

#include "config.h"

namespace store {

namespace {
Preferences prefs;
constexpr const char* NS = "ftype";
constexpr size_t KEY_LEN = 32, NONCE_LEN = 12, TAG_LEN = 16;

String key(const char* prefix, int index, const char* suffix) {
  return String(prefix) + index + suffix;  // 例如 "a0n"、"f12a"，NVS 键名最长 15 字节
}

// ---- 加密密钥来源 ------------------------------------------------------------
// 第一阶段（当前）：随机生成的 256 位密钥存在 NVS 里。
//   这只能防止“直接看到明文”，并不能防止有人拆下芯片读出整个闪存。
// 第三阶段（安全加固）：改为用 ESP32-S3 的 HMAC 硬件从 eFuse 里的密钥派生，
//   该 eFuse 密钥软件不可读；并开启闪存加密 + 安全启动。到时只需替换这个函数。
bool deviceKey(uint8_t out[KEY_LEN]) {
  if (prefs.getBytesLength("dk") == KEY_LEN) {
    return prefs.getBytes("dk", out, KEY_LEN) == KEY_LEN;
  }
  esp_fill_random(out, KEY_LEN);
  return prefs.putBytes("dk", out, KEY_LEN) == KEY_LEN;
}

bool encrypt(int index, const char* plain, size_t len, uint8_t* blob, size_t& blobLen) {
  uint8_t k[KEY_LEN];
  if (!deviceKey(k)) return false;
  uint8_t* nonce = blob;
  uint8_t* tag = blob + NONCE_LEN;
  uint8_t* ct = blob + NONCE_LEN + TAG_LEN;
  esp_fill_random(nonce, NONCE_LEN);
  const uint8_t aad[2] = {'a', uint8_t(index)};  // 把密文绑定到账户位置，防止被挪到别的账户

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, k, KEY_LEN * 8);
  if (rc == 0) {
    rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, len, nonce, NONCE_LEN, aad, sizeof(aad),
                                   reinterpret_cast<const uint8_t*>(plain), ct, TAG_LEN, tag);
  }
  mbedtls_gcm_free(&gcm);
  memset(k, 0, sizeof(k));
  blobLen = NONCE_LEN + TAG_LEN + len;
  return rc == 0;
}

bool decrypt(int index, const uint8_t* blob, size_t blobLen, char* out, size_t cap) {
  if (blobLen < NONCE_LEN + TAG_LEN) return false;
  const size_t len = blobLen - NONCE_LEN - TAG_LEN;
  if (len + 1 > cap) return false;
  uint8_t k[KEY_LEN];
  if (!deviceKey(k)) return false;
  const uint8_t aad[2] = {'a', uint8_t(index)};

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, k, KEY_LEN * 8);
  if (rc == 0) {
    rc = mbedtls_gcm_auth_decrypt(&gcm, len, blob, NONCE_LEN, aad, sizeof(aad), blob + NONCE_LEN, TAG_LEN,
                                  blob + NONCE_LEN + TAG_LEN, reinterpret_cast<uint8_t*>(out));
  }
  mbedtls_gcm_free(&gcm);
  memset(k, 0, sizeof(k));
  if (rc != 0) {
    memset(out, 0, cap);
    return false;
  }
  out[len] = '\0';
  return true;
}
}  // namespace

void begin() { prefs.begin(NS, false); }

bool getAccount(int index, Account& out) {
  out = Account();
  if (index < 0 || index >= MAX_ACCOUNTS) return false;
  if (!prefs.isKey(key("a", index, "p").c_str())) return false;
  out.used = true;
  out.name = prefs.getString(key("a", index, "n").c_str(), "");
  out.enter = prefs.getBool(key("a", index, "e").c_str(), true);
  return true;
}

bool setAccount(int index, const String& name, const String& password, bool enter) {
  if (index < 0 || index >= MAX_ACCOUNTS) return false;
  const bool exists = prefs.isKey(key("a", index, "p").c_str());
  if (password.length() == 0 && !exists) return false;
  if (password.length() > 0) {
    uint8_t blob[NONCE_LEN + TAG_LEN + MAX_PASSWORD_LEN];
    size_t blobLen = 0;
    if (password.length() > MAX_PASSWORD_LEN) return false;
    const bool ok = encrypt(index, password.c_str(), password.length(), blob, blobLen);
    if (!ok || prefs.putBytes(key("a", index, "p").c_str(), blob, blobLen) != blobLen) {
      memset(blob, 0, sizeof(blob));
      return false;
    }
    memset(blob, 0, sizeof(blob));
  }
  prefs.putString(key("a", index, "n").c_str(), name);
  prefs.putBool(key("a", index, "e").c_str(), enter);
  return true;
}

void deleteAccount(int index) {
  prefs.remove(key("a", index, "p").c_str());
  prefs.remove(key("a", index, "n").c_str());
  prefs.remove(key("a", index, "e").c_str());
}

bool readPassword(int index, char* out, size_t cap) {
  const String k = key("a", index, "p");
  const size_t n = prefs.getBytesLength(k.c_str());
  if (n == 0 || n > NONCE_LEN + TAG_LEN + MAX_PASSWORD_LEN) return false;
  uint8_t blob[NONCE_LEN + TAG_LEN + MAX_PASSWORD_LEN];
  prefs.getBytes(k.c_str(), blob, n);
  const bool ok = decrypt(index, blob, n, out, cap);
  memset(blob, 0, sizeof(blob));
  return ok;
}

bool getFinger(uint16_t id, Finger& out) {
  out = Finger();
  if (!prefs.isKey(key("f", id, "n").c_str())) return false;
  out.known = true;
  out.name = prefs.getString(key("f", id, "n").c_str(), "");
  out.account = prefs.getChar(key("f", id, "a").c_str(), -1);
  const uint16_t combo = prefs.getUShort(key("f", id, "k").c_str(), 0);
  out.mod = combo >> 8;
  out.key = combo & 0xFF;
  return true;
}

void setFinger(uint16_t id, const String& name, int8_t account, uint8_t mod, uint8_t key_) {
  prefs.putString(key("f", id, "n").c_str(), name);
  prefs.putChar(key("f", id, "a").c_str(), account);
  prefs.putUShort(key("f", id, "k").c_str(), (uint16_t(mod) << 8) | key_);
}

void deleteFinger(uint16_t id) {
  prefs.remove(key("f", id, "n").c_str());
  prefs.remove(key("f", id, "a").c_str());
  prefs.remove(key("f", id, "k").c_str());
}

bool hasAnyFinger(uint16_t maxId) {
  for (uint16_t id = 0; id < maxId; id++) {
    if (prefs.isKey(key("f", id, "n").c_str())) return true;
  }
  return false;
}

int16_t summonFinger() { return prefs.getShort("summon", -1); }
void setSummonFinger(int16_t id) { prefs.putShort("summon", id); }

uint16_t wakeDelayMs() { return prefs.getUShort("wake", DEFAULT_WAKE_DELAY_MS); }
void setWakeDelayMs(uint16_t ms) { prefs.putUShort("wake", ms); }

uint32_t failCount() { return prefs.getUInt("fails", 0); }
void setFailCount(uint32_t n) {
  if (failCount() != n) prefs.putUInt("fails", n);
}

void wipe() { prefs.clear(); }

}  // namespace store
