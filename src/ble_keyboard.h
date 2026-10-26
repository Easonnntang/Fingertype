#pragma once
#include <Arduino.h>

// 蓝牙（BLE）键盘。
//
// 配对规则（防止陌生电脑偷偷配对后接收密码）：
//   - 设备还没有配对过任何电脑时，允许配对；
//   - 已经配对过之后，只有在“配对窗口”打开期间（网页上验证指纹后打开，默认 60 秒）
//     才接受新电脑；窗口外的新配对会被立即删除并断开。
//   - 只有完成加密的已配对连接才会收到按键。
namespace blekb {

void begin(const char* deviceName);
void loop();

bool ready();                    // 有已加密的已配对连接
bool waitReady(uint32_t timeoutMs);

bool tapShift();                 // 轻按 Shift（唤醒屏幕，不输入字符）
bool typeText(const char* text);  // 只支持可打印 ASCII（美式键盘布局）
bool pressEnter();
bool sendShortcut(uint8_t modifier, uint8_t keycode);  // 按下并松开一个组合键

// 蓝牙配置通道：网页通过 Web Bluetooth 发送与 USB 串口相同的 JSON 行命令
bool configConnected();
bool configReadLine(String& out);    // 取出一条完整的命令（以 \n 结尾）
void configSend(const String& text);  // 发送给网页（自动分包）

void openPairing(uint32_t ms);
bool pairingOpen();
uint32_t pairingRemainingMs();
int bondCount();
void clearBonds();

// 可打印 ASCII -> HID 键码（美式布局）。供 USB 和蓝牙共用
bool asciiToHid(char c, uint8_t& modifier, uint8_t& keycode);

}  // namespace blekb
