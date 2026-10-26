#pragma once
#include <Arduino.h>

// 配置盘：插着数据线时，设备在 Mac 上显示为一个只读 U 盘（卷名 FINGERTYPE），
// 里面只有一个 Settings.html（即固件里嵌入的配置网页）。
//
// 什么时候出现：
//   - 设备还没录任何指纹（第一次设置）时，插上 Mac 自动出现；
//   - 设置好之后不会再自动出现，需要按住“召唤指纹”几秒才会出现。
// 在访达里推出后，它会隐藏，直到下次被召唤。
namespace drive {

void begin();      // 必须在 USB.begin() 之前调用
void show();
void hide();
bool shown();

}  // namespace drive
