// Fingertype 固件 —— 蓝牙为主，插数据线时 USB 优先
//
// 工作方式：
//   - 平时它是一个蓝牙键盘，Type-C 只用来供电；插着数据线连电脑时改用 USB 键盘（更快更稳）。
//   - 按下已录入的手指 -> 模块内部比对 -> 抬起手指时执行该手指绑定的动作
//     （输入某个密码 / 按一个快捷键）。按住“召唤指纹”数秒则改为弹出配置盘。
//   - 配置网页（web/index.html，也嵌在配置盘里的 Settings.html）通过蓝牙或 USB 串口
//     发送一行一个的 JSON 命令。密码只能写入，设备不会把密码传回电脑。
//     有已录入指纹时，任何修改都要先在设备上按一次已录入的手指。

#include <Arduino.h>
#include <ArduinoJson.h>
#include <USB.h>
#include <USBHIDKeyboard.h>

#include "ble_keyboard.h"
#include "config.h"
#include "config_drive.h"
#include "fp_module.h"
#include "ota.h"
#include "store.h"
#include "tusb.h"

USBCDC Cdc;
USBHIDKeyboard Keyboard;
fp::Module finger;

namespace {

// 固件识别标记 + 版本号：网页导入 .bin 文件时，靠它确认是本项目的固件并读出版本号
const char FW_ID[] = FIRMWARE_MARKER FIRMWARE_VERSION;

uint32_t authUntil = 0;    // 配置授权到期时间（millis）
uint32_t lockedUntil = 0;  // 连续失败锁定到期时间（millis）
String lineBuf;
bool busy = false;             // 正在执行需要等手指的命令（录入、验证、识别）
bool cancelRequested = false;  // 网页在等待期间发来了“取消”

bool readCommandLine(String& out);
void handleLine(const String& line);
void dispatch(JsonVariantConst req, const String& cmd);

// 等手指期间调用：处理新到的命令。“cancel”会让当前等待尽快结束，其他命令回复 busy。
bool checkCancel() {
  String line;
  while (readCommandLine(line)) handleLine(line);
  return cancelRequested;
}

// ---------------------------------------------------------------------------
// 工具函数

uint32_t remainingMs(uint32_t until) {
  const int32_t d = int32_t(until - millis());
  return d > 0 ? uint32_t(d) : 0;
}

bool isLocked() { return remainingMs(lockedUntil) > 0; }

// 回复和事件同时发往两个配置通道：蓝牙（日常使用）和 USB 串口（开发调试）
void send(JsonDocument& doc) {
  const bool usb = bool(Cdc);  // 网页没打开串口时不发，避免写入阻塞拖慢打字
  if (!usb && !blekb::configConnected()) return;
  String out;
  serializeJson(doc, out);
  out += '\n';
  if (usb) Cdc.print(out);
  blekb::configSend(out);
}

void event(const char* type, const char* msg = nullptr) {
  JsonDocument doc;
  doc["evt"] = type;
  if (msg) doc["msg"] = msg;
  send(doc);
}

void userLed(bool on) { digitalWrite(PIN_USER_LED, on ? LOW : HIGH); }

bool touchActive() {
  if (!DETECT_BY_TOUCH_PIN || PIN_FP_TOUCH < 0) return true;  // 不用触摸脚时，靠采图结果判断
  return digitalRead(PIN_FP_TOUCH) == HIGH;
}

// 等待手指按下并采集到一张图像
uint8_t waitFinger(uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (checkCancel()) return fp::CANCELLED;
    if (touchActive()) {
      const uint8_t c = finger.getImage();
      if (c == fp::OK) return fp::OK;
      if (c == fp::TIMEOUT) return c;
    }
    delay(FP_POLL_MS);
  }
  return fp::TIMEOUT;
}

// 手指是否还放在传感器上（优先用触摸脚，没接则靠采图判断）
bool fingerPresent() {
  if (DETECT_BY_TOUCH_PIN && PIN_FP_TOUCH >= 0) return touchActive();
  return finger.getImage() != fp::NO_FINGER;
}

void waitLift(uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (!fingerPresent() || checkCancel()) return;
    delay(FP_POLL_MS);
  }
}

// 已采集图像 -> 特征 -> 在指纹库中搜索
uint8_t identifyCaptured(uint16_t& id, uint16_t& score) {
  const uint8_t c = finger.genChar(1);
  if (c != fp::OK) return c;
  return finger.search(1, id, score);
}

// 模块在线时以模块为准；模块离线（比如被拔掉）或读数失败时以设备里的指纹记录为准，
// 防止“拔掉指纹模块 / 通信出错就不用验证”
bool anyFingerEnrolled() {
  if (finger.online() || finger.probe()) {
    const int count = finger.templateCount();
    if (count > 0) return true;
    if (count == 0) return store::hasAnyFinger(1000);
  }
  return store::hasAnyFinger(1000) || finger.online();  // 读不到又在线：保守地当作有指纹
}

// 设置入口指纹：手动选过就用选的；没选过且只录了一枚指纹时，自动用这一枚
int effectiveSummonFinger() {
  const int chosen = store::summonFinger();
  if (chosen >= 0) return chosen;
  return finger.templateCount() == 1 ? finger.firstUsedId() : -1;
}

bool needsAuth() { return anyFingerEnrolled() && remainingMs(authUntil) == 0; }

void registerSuccess() { store::setFailCount(0); }

void registerFail() {
  const uint32_t n = store::failCount() + 1;
  store::setFailCount(n);
  if (n % LOCKOUT_AFTER == 0) {
    const uint32_t rounds = n / LOCKOUT_AFTER;
    const uint32_t shift = min<uint32_t>(rounds - 1, 10);
    lockedUntil = millis() + min<uint32_t>(LOCKOUT_BASE_MS << shift, LOCKOUT_MAX_MS);
  }
}

bool validPassword(const String& s) {
  if (s.length() > MAX_PASSWORD_LEN) return false;
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s[i];
    if (c < 0x20 || c > 0x7E) return false;  // 目前只支持可打印 ASCII（英文键盘布局）
  }
  return true;
}

// ---------------------------------------------------------------------------
// 键盘输出

void wakeHost() {
  if (tud_suspended()) {
    tud_remote_wakeup();
    const uint32_t start = millis();
    while (tud_suspended() && millis() - start < 2000) delay(10);
  }
}

bool usbDataConnected() { return tud_mounted() || tud_suspended(); }  // 只接充电头时为 false

// 选择输出方式：蓝牙已连接 -> 蓝牙；否则插着数据线 -> USB；都没有 -> 等蓝牙重连
// 选择输出方式：插着数据线连电脑 -> USB（最快最稳）；否则蓝牙；蓝牙暂时断开 -> 等它重连
const char* chooseOutput() {
  if (usbDataConnected()) return "usb";
  if (blekb::ready()) return "ble";
  if (blekb::bondCount() > 0 && blekb::waitReady(BLE_RECONNECT_WAIT_MS)) return "ble";
  return nullptr;
}

// 按一个组合键；成功时返回所用的输出方式（"ble" / "usb"），失败返回 nullptr
const char* sendShortcut(uint8_t mod, uint8_t key) {
  const char* via = chooseOutput();
  if (!via) return nullptr;
  if (strcmp(via, "ble") == 0) return blekb::sendShortcut(mod, key) ? via : nullptr;
  wakeHost();
  KeyReport report = {mod, 0, {key, 0, 0, 0, 0, 0}};
  Keyboard.sendReport(&report);
  delay(15);
  Keyboard.releaseAll();
  return via;
}

// 输入密码；成功时返回所用的输出方式（"ble" / "usb"），失败返回 nullptr
const char* typeAccount(int index) {
  store::Account acct;
  if (!store::getAccount(index, acct)) return nullptr;
  const char* via = chooseOutput();
  if (!via) return nullptr;

  char pw[MAX_PASSWORD_LEN + 1];
  if (!store::readPassword(index, pw, sizeof(pw))) return nullptr;
  bool ok = true;

  // 先轻按一下 Shift 唤醒屏幕（不会输入任何字符），等锁屏界面出现，再输入密码
  if (strcmp(via, "ble") == 0) {
    ok = blekb::tapShift();
    delay(store::wakeDelayMs());
    ok = ok && blekb::typeText(pw);
    if (ok && acct.enter) ok = blekb::pressEnter();
  } else {
    wakeHost();
    Keyboard.press(KEY_LEFT_SHIFT);
    delay(30);
    Keyboard.releaseAll();
    delay(store::wakeDelayMs());
    for (char* p = pw; *p; ++p) {
      Keyboard.write(uint8_t(*p));
      delay(8);
    }
    if (acct.enter) Keyboard.write(KEY_RETURN);
  }
  memset(pw, 0, sizeof(pw));
  return ok ? via : nullptr;
}

// ---------------------------------------------------------------------------
// 平时：检测手指 -> 识别 -> 输入密码

void handleTouch() {
  if (!touchActive()) return;
  if (finger.getImage() != fp::OK) return;

  if (isLocked()) {
    finger.led(fp::LED_FLASH, fp::COLOR_RED, 3);
    JsonDocument doc;
    doc["evt"] = "locked";
    doc["seconds"] = remainingMs(lockedUntil) / 1000;
    send(doc);
    waitLift(5000);
    return;
  }

  const uint32_t touchedAt = millis();
  // 还没录任何指纹：不比对、不计失败次数（否则第一次设置时随手碰几下就会被锁定）
  if (finger.templateCount() == 0) {
    finger.led(fp::LED_FLASH, fp::COLOR_BLUE, 1);
    event("no_fingers", "还没有录入任何指纹");
    waitLift(5000);
    return;
  }

  uint16_t id = 0, score = 0;
  const uint8_t c = identifyCaptured(id, score);
  JsonDocument doc;
  if (c == fp::OK) {
    registerSuccess();
    store::Finger f;
    store::getFinger(id, f);
    doc["evt"] = "match";
    doc["id"] = id;
    doc["name"] = f.name;

    // 执行这枚指纹绑定的操作，返回所用的输出方式（"ble" / "usb"），没执行返回 nullptr
    auto runAction = [&]() -> const char* {
      if (f.known && f.account >= 0) return typeAccount(f.account);
      if (f.known && f.account == ACTION_SHORTCUT) return sendShortcut(f.mod, f.key);
      return nullptr;
    };
    auto finish = [&](const char* via) {
      finger.led(via ? fp::LED_ON : fp::LED_FLASH, via ? fp::COLOR_GREEN : fp::COLOR_BLUE, via ? 0 : 2);
      doc["typed"] = via != nullptr;
      if (via) doc["via"] = via;
    };

    // 只有“插着数据线 + 设置入口指纹”时，才需要区分“按一下”和“按住 SUMMON_HOLD_MS”：
    // 等手指抬起再执行；按住够久则弹出配置盘、不执行（避免把密码打进当前窗口）。
    // 其他情况（纯蓝牙，或其他手指）识别成功立即执行，和 Touch ID 一样。
    const bool mayOpenDrive = usbDataConnected() && id == effectiveSummonFinger();
    if (!mayOpenDrive) {
      finish(runAction());
    } else {
      finger.led(fp::LED_ON, fp::COLOR_BLUE);
      bool summoned = false;
      while (fingerPresent() && millis() - touchedAt < MAX_HOLD_MS) {
        if (millis() - touchedAt >= SUMMON_HOLD_MS) {
          summoned = true;
          drive::show();
          finger.led(fp::LED_FLASH, fp::COLOR_GREEN, 3);
          break;
        }
        delay(30);
      }
      if (summoned) {
        doc["evt"] = "summon";
        doc["drive"] = drive::shown();
      } else {
        finish(runAction());
      }
    }
  } else if (c == fp::NOT_FOUND || c == fp::NOT_MATCH) {
    registerFail();
    finger.led(fp::LED_FLASH, fp::COLOR_RED, 2);
    doc["evt"] = "nomatch";
  } else {
    doc["evt"] = "bad_image";  // 图像质量不好，不计入失败次数
    doc["msg"] = fp::describe(c);
  }
  send(doc);
  waitLift(5000);
  delay(300);
  finger.led(fp::LED_OFF, fp::COLOR_NONE);
}

// ---------------------------------------------------------------------------
// 配置命令

void reply(JsonDocument& res, JsonVariantConst req, bool ok, const char* error = nullptr) {
  if (!req["id"].isNull()) res["re"] = req["id"];
  res["ok"] = ok;
  if (error) res["error"] = error;
  send(res);
}

void fail(JsonVariantConst req, const char* error) {
  JsonDocument res;
  reply(res, req, false, error);
}

bool requireAuth(JsonVariantConst req) {
  if (!needsAuth()) return true;
  fail(req, "auth_required");
  return false;
}

void cmdInfo(JsonVariantConst req) {
  JsonDocument res;
  res["fw"] = FIRMWARE_VERSION;
  res["fwId"] = FW_ID;
  res["otaChunk"] = OTA_CHUNK;
  JsonObject mod = res["module"].to<JsonObject>();
  mod["online"] = finger.online();
  mod["capacity"] = finger.capacity();
  mod["count"] = finger.online() ? max(finger.templateCount(), 0) : 0;
  res["needAuth"] = needsAuth();
  res["authSeconds"] = remainingMs(authUntil) / 1000;
  res["lockedSeconds"] = remainingMs(lockedUntil) / 1000;
  res["usb"] = tud_mounted();
  res["wakeDelay"] = store::wakeDelayMs();
  res["summonFinger"] = store::summonFinger();          // 手动选择的（-1 表示没选）
  res["summonEffective"] = effectiveSummonFinger();     // 实际生效的（含“只有一枚时自动使用”）
  res["summonHoldSeconds"] = SUMMON_HOLD_MS / 1000;
  res["driveShown"] = drive::shown();
  JsonObject ble = res["ble"].to<JsonObject>();
  ble["connected"] = blekb::ready();
  ble["bonds"] = blekb::bondCount();
  ble["pairingOpen"] = blekb::pairingOpen();
  ble["pairingSeconds"] = blekb::pairingRemainingMs() / 1000;

  JsonArray accounts = res["accounts"].to<JsonArray>();
  for (int i = 0; i < MAX_ACCOUNTS; i++) {
    store::Account a;
    if (!store::getAccount(i, a)) continue;
    JsonObject o = accounts.add<JsonObject>();
    o["i"] = i;
    o["name"] = a.name;
    o["enter"] = a.enter;
  }
  res["maxAccounts"] = MAX_ACCOUNTS;

  JsonArray fingers = res["fingers"].to<JsonArray>();
  if (finger.online()) {
    uint8_t bm[32];
    for (uint16_t page = 0; page * 256 < finger.capacity(); page++) {
      if (!finger.readIndex(page, bm)) break;
      for (uint16_t bit = 0; bit < 256; bit++) {
        if (!(bm[bit / 8] & (1 << (bit % 8)))) continue;
        const uint16_t id = page * 256 + bit;
        store::Finger f;
        store::getFinger(id, f);
        JsonObject o = fingers.add<JsonObject>();
        o["id"] = id;
        o["name"] = f.known ? f.name : String("未命名");
        JsonObject action = o["action"].to<JsonObject>();
        if (f.account >= 0) {
          action["type"] = "password";
          action["account"] = f.account;
        } else if (f.account == ACTION_SHORTCUT) {
          action["type"] = "shortcut";
          action["mod"] = f.mod;
          action["key"] = f.key;
        } else {
          action["type"] = "none";
        }
      }
    }
  }
  reply(res, req, true);
}

void cmdAuth(JsonVariantConst req) {
  if (!needsAuth()) {
    JsonDocument res;
    res["authSeconds"] = remainingMs(authUntil) / 1000;
    reply(res, req, true);
    return;
  }
  if (isLocked()) return fail(req, "locked");
  event("prompt", "请按已录入的手指");
  finger.led(fp::LED_BREATH, fp::COLOR_BLUE);
  uint8_t c = waitFinger(FINGER_WAIT_MS);
  uint16_t id = 0, score = 0;
  if (c == fp::OK) c = identifyCaptured(id, score);
  finger.led(fp::LED_OFF, fp::COLOR_NONE);
  if (c != fp::OK) {
    if (c == fp::CANCELLED) return fail(req, "cancelled");
    if (c == fp::NOT_FOUND || c == fp::NOT_MATCH) registerFail();
    waitLift(3000);
    return fail(req, c == fp::TIMEOUT ? "timeout" : "not_matched");
  }
  registerSuccess();
  authUntil = millis() + AUTH_SESSION_MS;
  finger.led(fp::LED_ON, fp::COLOR_GREEN);
  waitLift(3000);
  finger.led(fp::LED_OFF, fp::COLOR_NONE);
  JsonDocument res;
  res["authSeconds"] = AUTH_SESSION_MS / 1000;
  reply(res, req, true);
}

void cmdSetAccount(JsonVariantConst req) {
  if (!requireAuth(req)) return;
  const int i = req["i"] | -1;
  const String name = req["name"] | "";
  const String password = req["password"] | "";
  const bool enter = req["enter"] | true;
  if (i < 0 || i >= MAX_ACCOUNTS) return fail(req, "bad_index");
  if (name.length() == 0 || name.length() > MAX_NAME_LEN) return fail(req, "bad_name");
  if (!validPassword(password)) return fail(req, "bad_password");
  if (!store::setAccount(i, name, password, enter)) return fail(req, "save_failed");
  JsonDocument res;
  reply(res, req, true);
}

void cmdDelAccount(JsonVariantConst req) {
  if (!requireAuth(req)) return;
  const int i = req["i"] | -1;
  if (i < 0 || i >= MAX_ACCOUNTS) return fail(req, "bad_index");
  store::deleteAccount(i);
  JsonDocument res;
  reply(res, req, true);
}

// 快捷键是否合法：修饰键只用左侧四个（⌃⇧⌥⌘），按键是标准键码 a..右方向键等
bool validShortcut(int mod, int key) { return mod >= 0 && mod <= 0x0F && key >= 0x04 && key <= 0x65; }

// 解析网页发来的动作：
//   {"type":"password","account":0} / {"type":"shortcut","mod":9,"key":20} / {"type":"none"}
struct Action {
  int8_t account = ACTION_NONE;
  uint8_t mod = 0, key = 0;
};

bool parseAction(JsonVariantConst v, Action& out) {
  const String type = v["type"] | "none";
  if (type == "none") {
    out = Action();
    return true;
  }
  if (type == "password") {
    const int a = v["account"] | -1;
    if (a < 0 || a >= MAX_ACCOUNTS) return false;
    out.account = a;
    return true;
  }
  if (type == "shortcut") {
    const int mod = v["mod"] | -1, key = v["key"] | -1;
    if (!validShortcut(mod, key)) return false;
    out.account = ACTION_SHORTCUT;
    out.mod = mod;
    out.key = key;
    return true;
  }
  return false;
}

void cmdEnroll(JsonVariantConst req) {
  if (!requireAuth(req)) return;
  const String name = req["name"] | "";
  Action action;
  if (name.length() == 0 || name.length() > MAX_NAME_LEN) return fail(req, "bad_name");
  if (!parseAction(req["action"], action)) return fail(req, "bad_action");
  const int id = finger.firstFreeId();
  if (id < 0) return fail(req, "library_full");

  for (int step = 1; step <= ENROLL_SAMPLES; step++) {
    JsonDocument ev;
    ev["evt"] = "enroll";
    ev["step"] = step;
    ev["of"] = ENROLL_SAMPLES;
    ev["action"] = "press";
    send(ev);
    finger.led(fp::LED_BREATH, fp::COLOR_BLUE);

    uint8_t c = waitFinger(FINGER_WAIT_MS);
    if (c != fp::OK) {
      finger.led(fp::LED_OFF, fp::COLOR_NONE);
      return fail(req, c == fp::CANCELLED ? "cancelled" : "timeout");
    }
    c = finger.genChar(step);
    if (c != fp::OK) {
      finger.led(fp::LED_FLASH, fp::COLOR_RED, 2);
      event("retry", fp::describe(c));
      waitLift(5000);
      step--;  // 这一次不算，重按
      continue;
    }
    if (step == 1) {  // 第一次就检查是不是已经录过的手指
      uint16_t existing = 0, score = 0;
      if (finger.search(1, existing, score) == fp::OK) {
        finger.led(fp::LED_FLASH, fp::COLOR_RED, 2);
        waitLift(5000);
        JsonDocument res;
        res["existing"] = existing;
        return reply(res, req, false, "already_enrolled");
      }
    }
    finger.led(fp::LED_ON, fp::COLOR_GREEN);
    ev["action"] = "lift";
    send(ev);
    waitLift(10000);
    if (cancelRequested) {
      finger.led(fp::LED_OFF, fp::COLOR_NONE);
      return fail(req, "cancelled");
    }
  }

  uint8_t c = finger.regModel();
  if (c == fp::OK) c = finger.store(1, id);
  if (c != fp::OK) {
    finger.led(fp::LED_FLASH, fp::COLOR_RED, 3);
    JsonDocument res;
    res["detail"] = fp::describe(c);
    return reply(res, req, false, "enroll_failed");
  }
  store::setFinger(id, name, action.account, action.mod, action.key);
  finger.led(fp::LED_FLASH, fp::COLOR_GREEN, 2);
  JsonDocument res;
  res["fingerId"] = id;
  reply(res, req, true);
}

void cmdSetFinger(JsonVariantConst req) {
  if (!requireAuth(req)) return;
  const int id = req["fingerId"] | -1;
  const String name = req["name"] | "";
  Action action;
  if (id < 0 || !finger.isUsed(id)) return fail(req, "bad_finger");
  if (name.length() == 0 || name.length() > MAX_NAME_LEN) return fail(req, "bad_name");
  if (!parseAction(req["action"], action)) return fail(req, "bad_action");
  store::setFinger(id, name, action.account, action.mod, action.key);
  JsonDocument res;
  reply(res, req, true);
}

// 测试一个快捷键（不绑定到任何指纹）。网页上点“测试”时用
void cmdTestShortcut(JsonVariantConst req) {
  if (!requireAuth(req)) return;
  const int mod = req["mod"] | -1, key = req["key"] | -1;
  if (!validShortcut(mod, key)) return fail(req, "bad_action");
  const char* via = sendShortcut(mod, key);
  if (!via) return fail(req, "output_unavailable");
  JsonDocument res;
  res["via"] = via;
  reply(res, req, true);
}

void cmdDelFinger(JsonVariantConst req) {
  if (!requireAuth(req)) return;
  const int id = req["fingerId"] | -1;
  if (id < 0 || id >= finger.capacity()) return fail(req, "bad_finger");
  const uint8_t c = finger.remove(id);
  if (c != fp::OK) return fail(req, "delete_failed");
  store::deleteFinger(id);
  if (store::summonFinger() == id) store::setSummonFinger(-1);
  JsonDocument res;
  reply(res, req, true);
}

// 调试用：在 seconds 秒内实时报告触摸脚电平和模块采图结果（不识别、不计失败次数）
void cmdSensorTest(JsonVariantConst req) {
  const uint32_t ms = min<uint32_t>(uint32_t(req["seconds"] | 10) * 1000, 30000);
  const uint32_t start = millis();
  int lastTouch = -1, lastImage = -1;
  finger.led(fp::LED_BREATH, fp::COLOR_BLUE);
  while (millis() - start < ms) {
    const int touch = PIN_FP_TOUCH >= 0 ? digitalRead(PIN_FP_TOUCH) : -1;
    const int image = finger.getImage();
    if (touch != lastTouch || image != lastImage) {
      JsonDocument ev;
      ev["evt"] = "sensor";
      ev["t"] = millis() - start;
      ev["touchPin"] = touch;
      ev["image"] = fp::describe(image);
      send(ev);
      lastTouch = touch;
      lastImage = image;
    }
    delay(100);
  }
  finger.led(fp::LED_OFF, fp::COLOR_NONE);
  JsonDocument res;
  reply(res, req, true);
}

// 调试用：给指纹模块发原始命令，例如 {"cmd":"fpRaw","bytes":[31,0]}（0x1F 读索引表第 0 页）
void cmdFpRaw(JsonVariantConst req) {
  uint8_t p[16];
  uint16_t len = 0;
  for (JsonVariantConst b : req["bytes"].as<JsonArrayConst>()) {
    if (len < sizeof(p)) p[len++] = b.as<uint8_t>();
  }
  if (len == 0) return fail(req, "bad_value");
  uint8_t r[64];
  uint16_t n = 0;
  const uint8_t code = finger.raw(p, len, r, sizeof(r), &n);
  JsonDocument res;
  res["code"] = code;
  res["meaning"] = fp::describe(code);
  JsonArray data = res["data"].to<JsonArray>();
  for (uint16_t i = 0; i < n; i++) data.add(r[i]);
  const uint32_t listen = min<uint32_t>(req["listenMs"] | 0, 5000);
  if (listen) {  // 继续听一段时间，看模块有没有多发回复
    uint8_t extra[8];
    const int k = finger.drain(listen, extra, 8);
    JsonArray ex = res["extra"].to<JsonArray>();
    for (int i = 0; i < k; i++) ex.add(extra[i]);
  }
  reply(res, req, true);
}

// 设置“召唤指纹”（fingerId = -1 表示取消）
void cmdSetSummon(JsonVariantConst req) {
  if (!requireAuth(req)) return;
  const int id = req["fingerId"] | -2;
  if (id < -1 || (id >= 0 && !finger.isUsed(id))) return fail(req, "bad_finger");
  store::setSummonFinger(id);
  JsonDocument res;
  reply(res, req, true);
}

void cmdSettings(JsonVariantConst req) {
  if (!requireAuth(req)) return;
  const int wake = req["wakeDelay"] | -1;
  if (wake < 0 || wake > 5000) return fail(req, "bad_value");
  store::setWakeDelayMs(wake);
  JsonDocument res;
  reply(res, req, true);
}

// 只识别、不打字，用来测试指纹
void cmdIdentify(JsonVariantConst req) {
  if (isLocked()) return fail(req, "locked");
  event("prompt", "请按手指");
  uint8_t c = waitFinger(FINGER_WAIT_MS);
  if (c == fp::CANCELLED) return fail(req, "cancelled");
  uint16_t id = 0, score = 0;
  if (c == fp::OK) c = identifyCaptured(id, score);
  waitLift(3000);
  if (c != fp::OK) {
    if (c == fp::NOT_FOUND || c == fp::NOT_MATCH) registerFail();
    return fail(req, c == fp::TIMEOUT ? "timeout" : "not_matched");
  }
  registerSuccess();
  store::Finger f;
  store::getFinger(id, f);
  JsonDocument res;
  res["fingerId"] = id;
  res["name"] = f.name;
  res["score"] = score;
  reply(res, req, true);
}

// 测试输出通道：打出一段测试文字（不涉及任何已存密码），via = "ble" 或 "usb"
// text 可选，只能是可打印 ASCII，最长 64 个字符
void cmdTypeTest(JsonVariantConst req) {
  if (!requireAuth(req)) return;
  const String via = req["via"] | "ble";
  const String custom = req["text"] | "Fingertype test 123";
  if (custom.length() == 0 || !validPassword(custom)) return fail(req, "bad_value");
  const char* text = custom.c_str();
  bool ok = false;
  if (via == "ble") {
    ok = blekb::ready() && blekb::typeText(text);
  } else if (usbDataConnected()) {
    Keyboard.print(text);
    ok = true;
  }
  if (!ok) return fail(req, "output_unavailable");
  JsonDocument res;
  reply(res, req, true);
}

// 打开蓝牙配对窗口，让一台新电脑配对
void cmdPairing(JsonVariantConst req) {
  if (!requireAuth(req)) return;
  blekb::openPairing(PAIRING_WINDOW_MS);
  JsonDocument res;
  res["seconds"] = PAIRING_WINDOW_MS / 1000;
  reply(res, req, true);
}

// 删除所有已配对的电脑
void cmdClearBonds(JsonVariantConst req) {
  if (!requireAuth(req)) return;
  blekb::clearBonds();
  JsonDocument res;
  reply(res, req, true);
}

// ---- 在线升级固件 ----
// 网页流程：otaBegin（要验证指纹）-> 按顺序发 otaData -> otaEnd（校验通过后设备重启进新固件）
void cmdOtaBegin(JsonVariantConst req) {
  if (!requireAuth(req)) return;
  const char* error = nullptr;
  if (!ota::begin(req["size"] | 0, req["sha256"] | "", error)) return fail(req, error);
  JsonDocument res;
  res["chunk"] = OTA_CHUNK;
  reply(res, req, true);
}

void cmdOtaData(JsonVariantConst req) {
  const char* error = nullptr;
  size_t written = 0;
  const bool ok = ota::write(req["off"] | 0, req["d"] | "", written, error);
  JsonDocument res;
  res["off"] = written;  // 已写入的字节数：出错时网页从这里继续
  reply(res, req, ok, error);
}

void cmdOtaEnd(JsonVariantConst req) {
  const char* error = nullptr;
  if (!ota::finish(error)) return fail(req, error);
  JsonDocument res;
  reply(res, req, true);
  delay(500);  // 等回复发出去
  ESP.restart();
}

void cmdOtaAbort(JsonVariantConst req) {
  ota::abort();
  JsonDocument res;
  reply(res, req, true);
}

void factoryReset() {
  finger.clearAll();
  store::wipe();
  blekb::clearBonds();
  authUntil = 0;
  lockedUntil = 0;
}

void cmdReset(JsonVariantConst req) {
  if (!requireAuth(req)) return;
  factoryReset();
  JsonDocument res;
  reply(res, req, true);
}

void handleLine(const String& line) {
  JsonDocument req;
  if (deserializeJson(req, line) != DeserializationError::Ok) {
    JsonDocument res;
    res["ok"] = false;
    res["error"] = "bad_json";
    send(res);
    return;
  }
  const String cmd = req["cmd"] | "";
  if (busy) {  // 正在等手指：只处理“取消”，其他命令请网页稍后再试
    if (cmd == "cancel") {
      cancelRequested = true;
      JsonDocument res;
      reply(res, req, true);
    } else {
      fail(req, "busy");
    }
    return;
  }
  if (cmd == "cancel") {  // 没有进行中的操作，直接回复成功
    JsonDocument res;
    reply(res, req, true);
    return;
  }
  busy = true;
  cancelRequested = false;
  dispatch(req, cmd);
  busy = false;
  cancelRequested = false;
}

void dispatch(JsonVariantConst req, const String& cmd) {
  const bool needsModule = cmd == "auth" || cmd == "enroll" || cmd == "setFinger" || cmd == "delFinger" ||
                           cmd == "identify" || cmd == "reset" || cmd == "sensorTest" || cmd == "fpRaw";
  if (needsModule && !finger.online() && !finger.probe()) return fail(req, "module_offline");

  if (cmd == "info") cmdInfo(req);
  else if (cmd == "auth") cmdAuth(req);
  else if (cmd == "logout") { authUntil = 0; JsonDocument res; reply(res, req, true); }
  else if (cmd == "setAccount") cmdSetAccount(req);
  else if (cmd == "delAccount") cmdDelAccount(req);
  else if (cmd == "enroll") cmdEnroll(req);
  else if (cmd == "setFinger") cmdSetFinger(req);
  else if (cmd == "delFinger") cmdDelFinger(req);
  else if (cmd == "settings") cmdSettings(req);
  else if (cmd == "identify") cmdIdentify(req);
  else if (cmd == "typeTest") cmdTypeTest(req);
  else if (cmd == "testShortcut") cmdTestShortcut(req);
  else if (cmd == "setSummon") cmdSetSummon(req);
  else if (cmd == "sensorTest") cmdSensorTest(req);
  else if (cmd == "fpRaw") cmdFpRaw(req);
  else if (cmd == "pairing") cmdPairing(req);
  else if (cmd == "clearBonds") cmdClearBonds(req);
  else if (cmd == "reset") cmdReset(req);
  else if (cmd == "otaBegin") cmdOtaBegin(req);
  else if (cmd == "otaData") cmdOtaData(req);
  else if (cmd == "otaEnd") cmdOtaEnd(req);
  else if (cmd == "otaAbort") cmdOtaAbort(req);
  else fail(req, "unknown_command");
}

// 取一条完整的命令（蓝牙或 USB 串口）
bool readCommandLine(String& out) {
  while (blekb::configReadLine(out)) {
    out.trim();
    if (out.length()) return true;
  }
  while (Cdc.available()) {
    const char c = Cdc.read();
    if (c == '\n' || c == '\r') {
      if (lineBuf.length()) {
        out = lineBuf;
        lineBuf = "";
        return true;
      }
    } else if (lineBuf.length() < 4096) {  // 升级固件时一行约 2KB
      lineBuf += c;
    } else {
      lineBuf = "";  // 太长的行直接丢弃
    }
  }
  return false;
}

void pollSerial() {
  String line;
  while (readCommandLine(line)) handleLine(line);
}

// 运行中长按 BOOT 键 10 秒 = 恢复出厂（清空指纹和密码）。
// 注意：不能“按住 BOOT 再上电”，那样会进入刷机模式。
void pollBootButton() {
  static uint32_t pressedAt = 0;
  if (digitalRead(PIN_BOOT_BUTTON) == LOW) {
    if (!pressedAt) pressedAt = millis();
    userLed((millis() / 200) % 2);
    if (millis() - pressedAt > 10000) {
      factoryReset();
      event("factory_reset");
      pressedAt = 0;
      userLed(false);
      while (digitalRead(PIN_BOOT_BUTTON) == LOW) delay(10);
    }
  } else if (pressedAt) {
    pressedAt = 0;
    userLed(false);
  }
}

// 还没录任何指纹（第一次设置）时，插上电脑就自动出现配置盘；每次插上只出现一次。
// 录过指纹之后不再自动出现，只能按住“召唤指纹”召唤。
void autoShowDrive() {
  static bool handledThisMount = false;
  static uint32_t mountedAt = 0;
  if (!tud_mounted()) {
    handledThisMount = false;
    mountedAt = 0;
    return;
  }
  if (handledThisMount) return;
  if (!mountedAt) mountedAt = millis();
  if (millis() - mountedAt < 1500) return;  // 等电脑完成识别、指纹模块上线
  handledThisMount = true;
  if (!anyFingerEnrolled()) drive::show();
}

}  // namespace

void setup() {
  pinMode(PIN_USER_LED, OUTPUT);
  userLed(false);
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  if (PIN_FP_TOUCH_POWER >= 0) {
    pinMode(PIN_FP_TOUCH_POWER, OUTPUT);
    digitalWrite(PIN_FP_TOUCH_POWER, HIGH);
  }
  if (PIN_FP_TOUCH >= 0) pinMode(PIN_FP_TOUCH, INPUT_PULLDOWN);

  USB.productName(PRODUCT_NAME);
  USB.manufacturerName(PRODUCT_NAME " DIY");
  USB.usbAttributes(TUSB_DESC_CONFIG_ATT_SELF_POWERED | TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP);
  Keyboard.begin();
  Cdc.begin();
  drive::begin();
  USB.begin();

  store::begin();
  // 断电重启不能绕过锁定：之前已经连续失败的，上电后先锁一段时间
  if (store::failCount() >= LOCKOUT_AFTER) lockedUntil = millis() + LOCKOUT_BASE_MS;

  blekb::begin(BLE_DEVICE_NAME);

  finger.begin(Serial1, PIN_FP_RX, PIN_FP_TX, FP_BAUD);
  if (finger.probe()) {
    // 一枚指纹都没有时，失败计数没有意义（没有可被暴力破解的指纹），清零并解除锁定
    if (finger.templateCount() == 0 && !store::hasAnyFinger(1000)) {
      store::setFailCount(0);
      lockedUntil = 0;
    }
    // 注意：这里不再闪光圈。怀疑模块对闪烁类光圈命令会多发一个迟到的回复，开机时会导致后续命令错位。
  }
}

void loop() {
  pollSerial();
  pollBootButton();
  blekb::loop();
  autoShowDrive();
  ota::loop();
  if (ota::active()) {  // 升级固件期间不响应指纹，避免把密码打进别的窗口
    delay(2);
    return;
  }

  static uint32_t lastProbe = 0;
  if (!finger.online()) {
    if (millis() - lastProbe > 2000) {
      lastProbe = millis();
      finger.probe();
    }
    return;
  }

  static uint32_t lastPoll = 0;
  // 用触摸脚时随时响应；否则每 FP_POLL_MS 毫秒问一次模块
  if ((DETECT_BY_TOUCH_PIN && PIN_FP_TOUCH >= 0) || millis() - lastPoll > FP_POLL_MS) {
    lastPoll = millis();
    handleTouch();
  }
  delay(5);
}
