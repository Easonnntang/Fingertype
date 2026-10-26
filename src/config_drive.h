#pragma once
#include <Arduino.h>

// 配置盘：插着数据线时，设备在电脑上显示为一个只读 U 盘（卷名见 config.h 的 DRIVE_LABEL），
// 里面有 Settings.html（即固件里嵌入的配置网页）和一份使用说明。
//
// 什么时候出现：
//   - 设备还没录任何指纹（第一次设置）时，插上电脑自动出现；
//   - 设置好之后不会再自动出现，需要用“设置入口指纹”长按 5 秒才会出现。
// 在访达里推出后，它会隐藏，直到下次被召唤。
namespace drive {

void begin();      // 必须在 USB.begin() 之前调用
void show();
void hide();
bool shown();

}  // namespace drive
