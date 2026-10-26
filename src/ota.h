#pragma once
#include <Arduino.h>

// 在线升级固件：网页把 .bin 文件分段发过来（蓝牙或 USB 都行），写进另一个程序分区，
// 全部收完并校验 SHA-256 无误后才切换启动分区。中途断开或出错，旧固件不受影响。
namespace ota {

// 开始一次升级。size：固件字节数；sha256：64 位十六进制。失败时 error 为错误代码
bool begin(size_t size, const String& sha256, const char*& error);
// 写入一段数据。offset 必须等于已写入的字节数（按顺序发送）；data 为 Base64
bool write(size_t offset, const String& base64, size_t& written, const char*& error);
// 收完后校验并设为下次启动的固件。成功后由调用方回复网页再重启
bool finish(const char*& error);
void abort();

bool active();
size_t size();
// 主循环调用：太久没收到数据就放弃
void loop();

}  // namespace ota
