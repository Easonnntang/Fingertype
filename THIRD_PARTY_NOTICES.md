# 第三方组件

Fingertype 的固件在编译时会用到下面这些开源组件。它们各自的许可证都允许和 GPL-3.0 一起使用；完整的许可证文本见各项目的仓库。

| 组件 | 用途 | 许可证 | 来源 |
|---|---|---|---|
| Arduino core for ESP32 | 开发框架（USB、串口、闪存、升级等） | LGPL-2.1 | https://github.com/espressif/arduino-esp32 |
| ESP-IDF | 乐鑫底层 SDK（随 Arduino core 一起提供） | Apache-2.0 | https://github.com/espressif/esp-idf |
| NimBLE-Arduino | 蓝牙低功耗协议栈（键盘、配置通道） | Apache-2.0 | https://github.com/h2zero/NimBLE-Arduino |
| ArduinoJson | 配置命令的 JSON 解析 | MIT | https://github.com/bblanchon/ArduinoJson |
| Mbed TLS | AES-GCM 加密、SHA-256、Base64（随 ESP-IDF 提供） | Apache-2.0 | https://github.com/Mbed-TLS/mbedtls |
| Lucide | 设置网页里的图标 | ISC | https://lucide.dev |

发布的固件文件（`.bin`）包含上述组件编译后的代码。按照 GPL-3.0 的要求，生成这些固件所需的全部源代码就在本仓库里，用 PlatformIO 即可重新编译。
