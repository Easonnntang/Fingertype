#include "fp_module.h"

namespace fp {

namespace {
constexpr uint8_t PID_COMMAND = 0x01;
constexpr uint8_t PID_ACK = 0x07;
constexpr uint8_t ADDR[4] = {0xFF, 0xFF, 0xFF, 0xFF};

constexpr uint8_t CMD_GET_IMAGE = 0x01;
constexpr uint8_t CMD_GEN_CHAR = 0x02;
constexpr uint8_t CMD_SEARCH = 0x04;
constexpr uint8_t CMD_REG_MODEL = 0x05;
constexpr uint8_t CMD_STORE = 0x06;
constexpr uint8_t CMD_DELETE = 0x0C;
constexpr uint8_t CMD_EMPTY = 0x0D;
constexpr uint8_t CMD_READ_SYS_PARA = 0x0F;
constexpr uint8_t CMD_TEMPLATE_NUM = 0x1D;
constexpr uint8_t CMD_READ_INDEX = 0x1F;
constexpr uint8_t CMD_LED = 0x3C;
}  // namespace

const char* describe(uint8_t code) {
  switch (code) {
    case OK: return "成功";
    case ERR_PACKET: return "通信包错误";
    case NO_FINGER: return "没有检测到手指";
    case ERR_IMAGE: return "采集图像失败";
    case ERR_TOO_DRY_OR_WET: return "手指太干或太湿";
    case ERR_TOO_MESSY: return "图像太乱";
    case ERR_FEATURE: return "特征点太少";
    case NOT_MATCH: return "指纹不匹配";
    case NOT_FOUND: return "没有找到匹配的指纹";
    case ERR_MERGE: return "合并特征失败，请重新录入";
    case ERR_ID_RANGE: return "编号超出范围";
    case ERR_STORE: return "写入模块存储失败";
    case TIMEOUT: return "模块无响应（检查接线）";
    case BAD_REPLY: return "模块回复格式异常";
    case CANCELLED: return "已取消";
    default: return "未知错误";
  }
}

void Module::begin(HardwareSerial& serial, int rxPin, int txPin, uint32_t baud) {
  serial_ = &serial;
  serial_->begin(baud, SERIAL_8N1, rxPin, txPin);
  delay(300);  // 模块上电后需要一点时间就绪（部分固件会先发一个 0x55）
  while (serial_->available()) serial_->read();
}

bool Module::readPacket(uint8_t* buf, uint16_t cap, uint16_t& len, uint32_t timeoutMs) {
  const uint32_t start = millis();
  auto readByte = [&](uint8_t& b) -> bool {
    while (!serial_->available()) {
      if (millis() - start > timeoutMs) return false;
      delay(1);
    }
    b = serial_->read();
    return true;
  };

  // 找包头 EF 01
  uint8_t b = 0, prev = 0;
  while (true) {
    if (!readByte(b)) return false;
    if (prev == 0xEF && b == 0x01) break;
    prev = b;
  }
  uint8_t hdr[7];  // 地址4 + 包标识1 + 长度2
  for (auto& h : hdr) if (!readByte(h)) return false;
  const uint8_t pid = hdr[4];
  const uint16_t total = (uint16_t(hdr[5]) << 8) | hdr[6];  // 内容 + 2 字节校验
  if (pid != PID_ACK || total < 3 || total - 2 > cap) return false;

  uint16_t sum = pid + hdr[5] + hdr[6];
  len = total - 2;
  for (uint16_t i = 0; i < len; i++) {
    if (!readByte(buf[i])) return false;
    sum += buf[i];
  }
  uint8_t c1, c2;
  if (!readByte(c1) || !readByte(c2)) return false;
  return ((uint16_t(c1) << 8) | c2) == sum;
}

// 发送前等串口安静下来：把还在路上的迟到回复也一并丢掉（最多等 200ms）
void Module::drainQuiet() {
  const uint32_t start = millis();
  uint32_t lastByte = millis();
  while (millis() - start < 200) {
    if (serial_->available()) {
      serial_->read();
      lastByte = millis();
    } else if (millis() - lastByte >= 15) {
      return;
    } else {
      delay(1);
    }
  }
}

// 模块的回复不带“是哪条命令的回复”的编号，所以要按长度校验：
// expectLen 是成功时确认码之后应有的字节数（-1 表示不校验）。长度对不上的成功回复视为迟到的旧回复，丢掉继续等。
uint8_t Module::command(const uint8_t* payload, uint16_t len, uint8_t* reply, uint16_t replyCap,
                        uint16_t* replyLen, uint32_t timeoutMs, int expectLen) {
  if (!serial_) return TIMEOUT;
  drainQuiet();

  const uint16_t total = len + 2;
  uint8_t head[9] = {0xEF, 0x01, ADDR[0], ADDR[1], ADDR[2], ADDR[3], PID_COMMAND,
                     uint8_t(total >> 8), uint8_t(total & 0xFF)};
  uint16_t sum = PID_COMMAND + head[7] + head[8];
  for (uint16_t i = 0; i < len; i++) sum += payload[i];
  serial_->write(head, sizeof(head));
  serial_->write(payload, len);
  serial_->write(uint8_t(sum >> 8));
  serial_->write(uint8_t(sum & 0xFF));

  uint8_t buf[64];
  uint16_t got = 0;
  const uint32_t start = millis();
  while (true) {
    const uint32_t elapsed = millis() - start;
    if (elapsed >= timeoutMs || !readPacket(buf, sizeof(buf), got, timeoutMs - elapsed)) {
      // 回复超时：模块可能稍后才回，等它安静下来再返回，免得迟到的回复被当成下一条命令的回复
      drainQuiet();
      return TIMEOUT;
    }
    if (got < 1) continue;
    const bool lengthOk = expectLen < 0 || buf[0] != OK || got - 1 == uint16_t(expectLen);
    if (lengthOk) break;
    // 长度不对：是别的命令迟到的回复，丢掉继续等
  }
  if (reply && replyLen) {
    *replyLen = min<uint16_t>(got - 1, replyCap);
    memcpy(reply, buf + 1, *replyLen);
  }
  return buf[0];
}

bool Module::probe() {
  const uint8_t p[] = {CMD_READ_SYS_PARA};
  uint8_t r[16];
  uint16_t n = 0;
  online_ = command(p, sizeof(p), r, sizeof(r), &n, 1000, 16) == OK;
  // 系统参数：状态(2) 系统识别码(2) 指纹库大小(2) 安全等级(2) 地址(4) 包大小(2) 波特率(2)
  if (online_ && n >= 6) {
    const uint16_t cap = (uint16_t(r[4]) << 8) | r[5];
    if (cap > 0 && cap <= 1000) capacity_ = cap;
  }
  return online_;
}

uint8_t Module::getImage() {
  const uint8_t p[] = {CMD_GET_IMAGE};
  return command(p, sizeof(p), nullptr, 0, nullptr, 1000, 0);
}

uint8_t Module::genChar(uint8_t bufferId) {
  const uint8_t p[] = {CMD_GEN_CHAR, bufferId};
  return command(p, sizeof(p), nullptr, 0, nullptr, 2000, 0);
}

uint8_t Module::search(uint8_t bufferId, uint16_t& id, uint16_t& score) {
  const uint8_t p[] = {CMD_SEARCH, bufferId, 0, 0, uint8_t(capacity_ >> 8), uint8_t(capacity_ & 0xFF)};
  uint8_t r[4];
  uint16_t n = 0;
  const uint8_t code = command(p, sizeof(p), r, sizeof(r), &n, 3000, 4);
  if (code == OK) {
    if (n < 4) return BAD_REPLY;
    id = (uint16_t(r[0]) << 8) | r[1];
    score = (uint16_t(r[2]) << 8) | r[3];
  }
  return code;
}

uint8_t Module::regModel() {
  const uint8_t p[] = {CMD_REG_MODEL};
  return command(p, sizeof(p), nullptr, 0, nullptr, 3000, 0);
}

uint8_t Module::store(uint8_t bufferId, uint16_t id) {
  const uint8_t p[] = {CMD_STORE, bufferId, uint8_t(id >> 8), uint8_t(id & 0xFF)};
  return command(p, sizeof(p), nullptr, 0, nullptr, 2000, 0);
}

uint8_t Module::remove(uint16_t id) {
  const uint8_t p[] = {CMD_DELETE, uint8_t(id >> 8), uint8_t(id & 0xFF), 0, 1};
  return command(p, sizeof(p), nullptr, 0, nullptr, 1000, 0);
}

uint8_t Module::clearAll() {
  const uint8_t p[] = {CMD_EMPTY};
  return command(p, sizeof(p), nullptr, 0, nullptr, 3000, 0);
}

bool Module::readIndex(uint8_t page, uint8_t bitmap[32]) {
  const uint8_t p[] = {CMD_READ_INDEX, page};
  for (int attempt = 0; attempt < 2; attempt++) {  // 回复长度不对说明错位了，重试一次
    uint16_t n = 0;
    if (command(p, sizeof(p), bitmap, 32, &n, 1000, 32) == OK && n == 32) return true;
    delay(50);
  }
  return false;
}

bool Module::isUsed(uint16_t id) {
  uint8_t bm[32];
  if (!readIndex(id / 256, bm)) return false;
  const uint16_t bit = id % 256;
  return bm[bit / 8] & (1 << (bit % 8));
}

int Module::firstFreeId() {
  uint8_t bm[32];
  for (uint16_t page = 0; page * 256 < capacity_; page++) {
    if (!readIndex(page, bm)) return -1;
    for (uint16_t bit = 0; bit < 256; bit++) {
      const uint16_t id = page * 256 + bit;
      if (id >= capacity_) return -1;
      if (!(bm[bit / 8] & (1 << (bit % 8)))) return id;
    }
  }
  return -1;
}

int Module::firstUsedId() {
  uint8_t bm[32];
  for (uint16_t page = 0; page * 256 < capacity_; page++) {
    if (!readIndex(page, bm)) return -1;
    for (uint16_t bit = 0; bit < 256; bit++) {
      const uint16_t id = page * 256 + bit;
      if (id >= capacity_) return -1;
      if (bm[bit / 8] & (1 << (bit % 8))) return id;
    }
  }
  return -1;
}

int Module::templateCount() {
  const uint8_t p[] = {CMD_TEMPLATE_NUM};
  uint8_t r[2];
  for (int attempt = 0; attempt < 2; attempt++) {
    uint16_t n = 0;
    if (command(p, sizeof(p), r, sizeof(r), &n, 1000, 2) == OK && n == 2) return (int(r[0]) << 8) | r[1];
    delay(50);
  }
  return -1;  // 读不到：调用方必须按“有指纹”处理（失败即保守）
}

void Module::led(LedMode mode, LedColor color, uint8_t cycles) {
  const uint8_t p[] = {CMD_LED, mode, color, color, cycles};
  // 实测模块回复光圈命令较慢，必须等到回复，否则迟到的回复会和下一条命令错位
  command(p, sizeof(p), nullptr, 0, nullptr, 1500, 0);
}

}  // namespace fp
