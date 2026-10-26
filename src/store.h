#pragma once
#include <Arduino.h>

// 设备上的持久化数据（ESP32 的 NVS 存储区）：
//   - 账户：名称、是否自动回车、加密后的密码（AES-256-GCM）
//   - 指纹元数据：模块里每个指纹编号对应的名称和账户
//   - 设置与失败计数
//
// 密码只能写入、覆盖、删除，没有任何接口能把明文传出设备；
// 解密只在打字前的一瞬间发生，用完立即清零。
namespace store {

struct Account {
  bool used = false;
  String name;
  bool enter = true;  // 输入密码后是否自动按回车
};

struct Finger {
  bool known = false;
  String name;
  int8_t account = -1;  // 0..3：输入该账户密码；ACTION_NONE；ACTION_SHORTCUT（见 config.h）
  uint8_t mod = 0;      // 快捷键的修饰键位：⌃=0x01 ⇧=0x02 ⌥=0x04 ⌘=0x08
  uint8_t key = 0;      // 快捷键的 HID 键码
};

void begin();

bool getAccount(int index, Account& out);
// password 为空时：账户已存在则保留原密码，只改名称/回车设置；账户不存在则失败
bool setAccount(int index, const String& name, const String& password, bool enter);
void deleteAccount(int index);
// 解密密码到 out（以 \0 结尾），调用方用完必须清零。仅供打字使用。
bool readPassword(int index, char* out, size_t cap);

bool getFinger(uint16_t id, Finger& out);
void setFinger(uint16_t id, const String& name, int8_t account, uint8_t mod = 0, uint8_t key = 0);
void deleteFinger(uint16_t id);
bool hasAnyFinger(uint16_t maxId);  // 设备里是否有任何指纹记录（编号 0..maxId-1）

// “召唤指纹”：插着数据线时按住它几秒，配置盘才会出现。-1 表示还没设置
int16_t summonFinger();
void setSummonFinger(int16_t id);

uint16_t wakeDelayMs();
void setWakeDelayMs(uint16_t ms);

uint32_t failCount();
void setFailCount(uint32_t n);

// 清除所有账户、指纹元数据、设置和加密密钥
void wipe();

}  // namespace store
