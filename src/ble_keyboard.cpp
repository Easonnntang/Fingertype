#include "ble_keyboard.h"

#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Preferences.h>

#include "config.h"

namespace blekb {

namespace {
constexpr uint8_t REPORT_ID = 1;
constexpr uint8_t MOD_LEFT_SHIFT = 0x02;
constexpr uint8_t KEY_ENTER = 0x28;
constexpr uint16_t APPEARANCE_KEYBOARD = 0x03C1;
constexpr uint16_t NO_CONN = 0xFFFF;

// 标准键盘报告描述符：输入 8 字节（修饰键、保留、6 个按键），输出 1 字节（指示灯）
const uint8_t REPORT_MAP[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, REPORT_ID,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
};

// 配置通道（自定义 GATT 服务）：网页通过 Web Bluetooth 读写，内容与 USB 串口的 JSON 命令完全相同。
// 两个特征值都要求加密，并且只接受通过了配对检查的连接。
const char* CFG_SERVICE_UUID = "f1e70001-74a0-4461-b8ef-90a81cb4120e";
const char* CFG_RX_UUID = "f1e70002-74a0-4461-b8ef-90a81cb4120e";  // 网页 -> 设备（写）
const char* CFG_TX_UUID = "f1e70003-74a0-4461-b8ef-90a81cb4120e";  // 设备 -> 网页（通知）
constexpr uint32_t UNVERIFIED_TIMEOUT_MS = 15000;  // 连上后迟迟不加密的连接会被断开
constexpr int MAX_LINKS = 3;

// 服务列表版本：增删或修改 GATT 服务/特征值时 +1。
// 已配对的电脑会缓存服务列表；它连上来时若上次见到的版本不同，就发“服务已变更”让它刷新。
constexpr uint32_t GATT_DB_VERSION = 3;  // 1：只有键盘；2：增加配置通道；3：配置通道支持无应答写入（升级固件更快）
Preferences blePrefs;  // 记录每台已配对电脑见过的服务列表版本

NimBLEServer* server = nullptr;
NimBLECharacteristic* input = nullptr;
NimBLECharacteristic* cfgTx = nullptr;
volatile uint16_t secureConn = NO_CONN;  // 已加密、已配对的连接（键盘输出用）
std::vector<NimBLEAddress> bondsAtConnect;  // 连接建立时已配对的电脑（身份地址）
volatile uint32_t pairingUntil = 0;

// 每个连接的状态：是否通过了配对检查、连接时间
struct Link {
  uint16_t handle = NO_CONN;
  bool approved = false;
  uint32_t connectedAt = 0;
};
Link links[MAX_LINKS];
volatile uint16_t cfgConn = NO_CONN;  // 订阅了配置通知的连接

// 网页写来的数据缓冲（NimBLE 任务写入，主循环读取）
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;
char rxBuf[4096];  // 升级固件时一行约 2KB
size_t rxLen = 0;

uint32_t remaining(uint32_t until) {
  const int32_t d = int32_t(until - millis());
  return d > 0 ? uint32_t(d) : 0;
}

Link* findLink(uint16_t handle) {
  for (auto& l : links) {
    if (l.handle == handle) return &l;
  }
  return nullptr;
}

bool isApproved(uint16_t handle) {
  const Link* l = findLink(handle);
  return l && l->approved;
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    bondsAtConnect.clear();
    for (int i = 0; i < NimBLEDevice::getNumBonds(); i++) bondsAtConnect.push_back(NimBLEDevice::getBondedAddress(i));
    if (Link* l = findLink(NO_CONN)) {
      *l = Link();
      l->handle = info.getConnHandle();
      l->connectedAt = millis();
    }
    NimBLEDevice::startSecurity(info.getConnHandle());  // 已配对的电脑会直接加密
    // 连上后继续广播，这样同一台电脑上的 Chrome 也能搜到设备来做配置
    if (s->getConnectedCount() < MAX_LINKS) NimBLEDevice::getAdvertising()->start();
  }

  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    if (info.getConnHandle() == secureConn) secureConn = NO_CONN;
    if (info.getConnHandle() == cfgConn) cfgConn = NO_CONN;
    if (Link* l = findLink(info.getConnHandle())) *l = Link();
  }

  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    if (!info.isEncrypted()) {
      server->disconnect(info);
      return;
    }
    // 加密完成后拿到的是电脑的身份地址，和连接前的已配对列表比对，判断是不是新电脑
    bool newBond = true;
    for (const auto& a : bondsAtConnect) {
      if (a == info.getIdAddress()) newBond = false;
    }
    if (newBond && !(bondsAtConnect.empty() || remaining(pairingUntil) > 0)) {
      NimBLEDevice::deleteBond(info.getIdAddress());  // 窗口外的陌生电脑：删掉并断开
      server->disconnect(info);
      return;
    }
    if (newBond) pairingUntil = 0;  // 一次窗口只配对一台
    secureConn = info.getConnHandle();
    if (Link* l = findLink(info.getConnHandle())) l->approved = true;

    // 这台电脑缓存的服务列表是旧版本 -> 通知它重新读取
    String k = "g" + String(info.getIdAddress().toString().c_str());
    k.replace(":", "");
    if (blePrefs.getUInt(k.c_str(), 1) != GATT_DB_VERSION) {
      server->sendServiceChangedIndication();
      blePrefs.putUInt(k.c_str(), GATT_DB_VERSION);
    }
  }
};

class ConfigRxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    if (!info.isEncrypted() || !isApproved(info.getConnHandle())) return;
    const NimBLEAttValue v = c->getValue();
    portENTER_CRITICAL(&rxMux);
    if (rxLen + v.length() <= sizeof(rxBuf)) {
      memcpy(rxBuf + rxLen, v.data(), v.length());
      rxLen += v.length();
    } else {
      rxLen = 0;  // 溢出：丢弃整段，网页会因超时报错
    }
    portEXIT_CRITICAL(&rxMux);
  }
};

class ConfigTxCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& info, uint16_t subValue) override {
    if (subValue && info.isEncrypted() && isApproved(info.getConnHandle())) {
      cfgConn = info.getConnHandle();
    } else if (info.getConnHandle() == cfgConn) {
      cfgConn = NO_CONN;
    }
  }
};

bool sendReport(uint8_t modifier, uint8_t key) {
  if (!ready()) return false;
  uint8_t report[8] = {modifier, 0, key, 0, 0, 0, 0, 0};
  input->setValue(report, sizeof(report));
  input->notify();
  delay(12);
  uint8_t release[8] = {0};
  input->setValue(release, sizeof(release));
  input->notify();
  delay(12);
  return true;
}

// 可打印 ASCII -> HID 键码（美式布局）
bool asciiToHid(char c, uint8_t& modifier, uint8_t& keycode) {
  modifier = 0;
  if (c >= 'a' && c <= 'z') { keycode = 0x04 + (c - 'a'); return true; }
  if (c >= 'A' && c <= 'Z') { keycode = 0x04 + (c - 'A'); modifier = MOD_LEFT_SHIFT; return true; }
  if (c >= '1' && c <= '9') { keycode = 0x1E + (c - '1'); return true; }
  if (c == '0') { keycode = 0x27; return true; }
  if (c == ' ') { keycode = 0x2C; return true; }

  static const char plain[] = "-=[]\\;'`,./";
  static const char shifted[] = "_+{}|:\"~<>?";
  static const uint8_t codes[] = {0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};
  for (size_t i = 0; i < sizeof(codes); i++) {
    if (c == plain[i]) { keycode = codes[i]; return true; }
    if (c == shifted[i]) { keycode = codes[i]; modifier = MOD_LEFT_SHIFT; return true; }
  }
  static const char digitsShifted[] = "!@#$%^&*()";  // Shift + 1..0
  for (size_t i = 0; i < 10; i++) {
    if (c == digitsShifted[i]) { keycode = 0x1E + i; modifier = MOD_LEFT_SHIFT; return true; }
  }
  return false;
}
}  // namespace

void begin(const char* deviceName) {
  blePrefs.begin("ftypeble", false);
  NimBLEDevice::init(deviceName);
  NimBLEDevice::setSecurityAuth(true /*配对后保存*/, false, true /*LE 安全连接*/);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  server->advertiseOnDisconnect(true);

  auto* hid = new NimBLEHIDDevice(server);
  hid->setManufacturer(PRODUCT_NAME " DIY");
  hid->setPnp(0x02, 0xE502, 0xA111, 0x0100);
  hid->setHidInfo(0x00, 0x02);  // 0x02：支持远程唤醒
  hid->setReportMap(const_cast<uint8_t*>(REPORT_MAP), sizeof(REPORT_MAP));
  input = hid->getInputReport(REPORT_ID);
  hid->getOutputReport(REPORT_ID);
  hid->setBatteryLevel(100);

  NimBLEDevice::setMTU(247);
  NimBLEService* cfg = server->createService(CFG_SERVICE_UUID);
  NimBLECharacteristic* rx = cfg->createCharacteristic(CFG_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
  rx->setCallbacks(new ConfigRxCallbacks());
  cfgTx = cfg->createCharacteristic(CFG_TX_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC);
  cfgTx->setCallbacks(new ConfigTxCallbacks());
  server->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setAppearance(APPEARANCE_KEYBOARD);
  adv->addServiceUUID(hid->getHidService()->getUUID());
  adv->setName(deviceName);
  adv->start();
}

void loop() {
  static uint32_t last = 0;
  if (!server || millis() - last < 1000) return;
  last = millis();
  // 连上后 15 秒还没通过配对检查的连接（陌生设备）断开，免得占着连接名额
  for (auto& l : links) {
    if (l.handle != NO_CONN && !l.approved && millis() - l.connectedAt > UNVERIFIED_TIMEOUT_MS) {
      server->disconnect(l.handle);
      l.connectedAt = millis();  // 等断开回调清理，避免每秒重复断开
    }
  }
  // 兜底：名额没满时保持广播
  if (server->getConnectedCount() < MAX_LINKS && !NimBLEDevice::getAdvertising()->isAdvertising()) {
    NimBLEDevice::getAdvertising()->start();
  }
}

bool configConnected() { return cfgConn != NO_CONN && cfgTx != nullptr; }

bool configReadLine(String& out) {
  static char line[sizeof(rxBuf) + 1];  // 较大，放静态区避免占用任务栈
  size_t n = 0;
  bool found = false;
  portENTER_CRITICAL(&rxMux);
  for (size_t i = 0; i < rxLen; i++) {
    if (rxBuf[i] == '\n') {
      n = i;
      memcpy(line, rxBuf, n);
      memmove(rxBuf, rxBuf + i + 1, rxLen - i - 1);
      rxLen -= i + 1;
      found = true;
      break;
    }
  }
  portEXIT_CRITICAL(&rxMux);
  if (!found) return false;
  line[n] = '\0';
  out = line;
  memset(line, 0, n);  // 可能含密码，用完清掉
  return true;
}

void configSend(const String& text) {
  const uint16_t h = cfgConn;
  if (h == NO_CONN || !cfgTx) return;
  const uint16_t mtu = server->getPeerMTU(h);
  const size_t chunk = mtu > 23 ? mtu - 3 : 20;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(text.c_str());
  size_t left = text.length();
  while (left > 0) {
    const size_t n = min(left, chunk);
    if (!cfgTx->notify(p, n, h)) break;
    p += n;
    left -= n;
    delay(4);  // 给蓝牙协议栈留出发送时间
  }
}

bool ready() { return secureConn != NO_CONN && input != nullptr; }

bool waitReady(uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (!ready() && millis() - start < timeoutMs) delay(20);
  return ready();
}

bool tapShift() { return sendReport(MOD_LEFT_SHIFT, 0); }

bool typeText(const char* text) {
  for (const char* p = text; *p; ++p) {
    uint8_t mod, key;
    if (!asciiToHid(*p, mod, key)) return false;
    if (!sendReport(mod, key)) return false;
  }
  return true;
}

bool pressEnter() { return sendReport(0, KEY_ENTER); }

bool sendShortcut(uint8_t modifier, uint8_t keycode) { return sendReport(modifier, keycode); }

void openPairing(uint32_t ms) { pairingUntil = millis() + ms; }
bool pairingOpen() { return NimBLEDevice::getNumBonds() == 0 || remaining(pairingUntil) > 0; }
uint32_t pairingRemainingMs() { return remaining(pairingUntil); }
int bondCount() { return NimBLEDevice::getNumBonds(); }

void clearBonds() {
  if (server) {
    for (uint8_t i = server->getConnectedCount(); i > 0; i--) {
      server->disconnect(server->getPeerInfo(i - 1));
    }
  }
  NimBLEDevice::deleteAllBonds();
  blePrefs.clear();
  secureConn = NO_CONN;
}

}  // namespace blekb
