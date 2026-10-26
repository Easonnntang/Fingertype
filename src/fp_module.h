#pragma once
#include <Arduino.h>

// 海凌科 HLK-ZW101 / ZW111 指纹模块驱动。
// 模块使用通用的 "EF01" 串口协议（与 AS608/R30x 系列相同的包格式）：
//   EF 01 | 地址(4) | 包标识(1) | 长度(2) | 内容 | 校验和(2)
// 指纹模板存在模块内部，比对也在模块内部完成，指纹数据不会传给主控。
namespace fp {

// 模块返回的确认码（部分）
enum Code : uint8_t {
  OK = 0x00,
  ERR_PACKET = 0x01,
  NO_FINGER = 0x02,
  ERR_IMAGE = 0x03,
  ERR_TOO_DRY_OR_WET = 0x04,
  ERR_TOO_MESSY = 0x06,
  ERR_FEATURE = 0x07,
  NOT_MATCH = 0x08,
  NOT_FOUND = 0x09,
  ERR_MERGE = 0x0A,
  ERR_ID_RANGE = 0x0B,
  ERR_STORE = 0x18,
  // 以下是本驱动自定义的，不是模块返回的
  TIMEOUT = 0xF0,
  BAD_REPLY = 0xF1,
  CANCELLED = 0xF2,  // 用户在网页上点了取消
};

const char* describe(uint8_t code);

// LED 光圈控制（ControlBLN 0x3C）。颜色编号已在 HLK-ZW111 上实测确认。
enum LedMode : uint8_t { LED_BREATH = 1, LED_FLASH = 2, LED_ON = 3, LED_OFF = 4 };
enum LedColor : uint8_t { COLOR_NONE = 0, COLOR_BLUE = 1, COLOR_GREEN = 2, COLOR_RED = 4 };

class Module {
 public:
  void begin(HardwareSerial& serial, int rxPin, int txPin, uint32_t baud);

  // 读取系统参数，确认模块在线并获取指纹库容量
  bool probe();
  bool online() const { return online_; }
  uint16_t capacity() const { return capacity_; }

  uint8_t getImage();                       // 采集图像；没手指返回 NO_FINGER
  uint8_t genChar(uint8_t bufferId);        // 图像 -> 特征，存入缓冲区
  uint8_t search(uint8_t bufferId, uint16_t& id, uint16_t& score);
  uint8_t regModel();                       // 合并缓冲区特征为模板
  uint8_t store(uint8_t bufferId, uint16_t id);
  uint8_t remove(uint16_t id);
  uint8_t clearAll();
  bool readIndex(uint8_t page, uint8_t bitmap[32]);  // 每页 256 个编号的占用位图
  bool isUsed(uint16_t id);
  int firstFreeId();                        // 没有空位返回 -1
  int firstUsedId();                        // 没有指纹返回 -1
  int templateCount();                      // 读取失败返回 -1
  void led(LedMode mode, LedColor color, uint8_t cycles = 0);

 private:
  uint8_t command(const uint8_t* payload, uint16_t len, uint8_t* reply = nullptr,
                  uint16_t replyCap = 0, uint16_t* replyLen = nullptr, uint32_t timeoutMs = 1000,
                  int expectLen = -1);
  bool readPacket(uint8_t* buf, uint16_t cap, uint16_t& len, uint32_t timeoutMs);
  void drainQuiet();
  int findId(bool used);  // 第一个已用 / 空闲的编号，没有返回 -1

  HardwareSerial* serial_ = nullptr;
  bool online_ = false;
  uint16_t capacity_ = 50;
};

}  // namespace fp
