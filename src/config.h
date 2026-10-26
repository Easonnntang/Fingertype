#pragma once
#include <stdint.h>

// ---- 接线（XIAO ESP32S3） -------------------------------------------------
// 指纹模块 TX -> XIAO D7 (GPIO44)，指纹模块 RX -> XIAO D4 (GPIO5)
//   注意：模块 RX 不能接 D6 (GPIO43)。那是 ESP32 默认的串口日志输出脚，
//   每次开机芯片都会在上面打印启动日志，模块收到这些乱码后协议解析会错位
//   （之后每条命令都收到上一条命令的回复），而且模块不断电就一直错下去。
// 指纹模块触摸输出（手指按下时为高电平）-> XIAO D1 (GPIO2)
// 指纹模块 VCC -> 3V3，GND -> GND
// 指纹模块触摸电源 -> XIAO D3 (GPIO4)：XIAO 只有一个 3V3 针脚，
//   触摸检测电路耗电不到 25μA，用普通针脚输出高电平供电即可，免焊接
constexpr int PIN_FP_RX = 44;     // XIAO 接收，接模块 TX
constexpr int PIN_FP_TX = 5;      // XIAO 发送（D4），接模块 RX
constexpr int PIN_FP_TOUCH = 2;   // 设为 -1 则不用触摸脚，改为定时轮询
// 实测 ZW111 的触摸脚比模块自己检测到手指晚约 0.2 秒，所以默认直接高频询问模块（更快、不漏按）。
// 触摸脚仍然接着，留给以后做低功耗唤醒。
constexpr bool DETECT_BY_TOUCH_PIN = false;
constexpr uint32_t FP_POLL_MS = 40;  // 询问模块“有没有手指”的间隔
constexpr int PIN_FP_TOUCH_POWER = 4;  // 设为 -1 则表示触摸电源直接接了 3V3
constexpr int PIN_BOOT_BUTTON = 0;  // 板载 BOOT 键：运行中长按 10 秒 = 恢复出厂
constexpr int PIN_USER_LED = 21;    // 板载橙色 LED，低电平点亮

constexpr uint32_t FP_BAUD = 57600;

// ---- 业务参数 ---------------------------------------------------------------
constexpr int MAX_ACCOUNTS = 4;          // 最多保存几组密码
// 指纹绑定的动作（存在 Finger.account 里）：0..MAX_ACCOUNTS-1 = 输入该账户的密码
constexpr int ACTION_NONE = -1;          // 什么都不做
constexpr int ACTION_SHORTCUT = -2;      // 按一个快捷键（修饰键 + 按键，例如 ⌃⌘Q 锁屏）
constexpr int MAX_PASSWORD_LEN = 64;
constexpr int MAX_NAME_LEN = 24;          // 字节数（UTF-8 中文约 8 个字）
constexpr int ENROLL_SAMPLES = 4;         // 录入一枚指纹要按几次
constexpr uint32_t AUTH_SESSION_MS = 2 * 60 * 1000;  // 验证指纹后可修改配置的时长
constexpr uint32_t FINGER_WAIT_MS = 15000;           // 等待按手指的超时

// 连续失败锁定：失败 LOCKOUT_AFTER 次后锁定，每多失败一轮时长翻倍，最长 LOCKOUT_MAX_MS
constexpr int LOCKOUT_AFTER = 5;
constexpr uint32_t LOCKOUT_BASE_MS = 30 * 1000;
constexpr uint32_t LOCKOUT_MAX_MS = 10 * 60 * 1000;

constexpr uint16_t DEFAULT_WAKE_DELAY_MS = 800;  // 唤醒屏幕后等多久再输入密码

// 插着数据线时，“设置入口指纹”按住超过这个时长则改为弹出配置盘（不执行动作）
constexpr uint32_t SUMMON_HOLD_MS = 5000;
constexpr uint32_t MAX_HOLD_MS = 30000;     // 按住超过这个时长视为异常，放弃本次

// ---- 名称 -------------------------------------------------------------------
#define PRODUCT_NAME "Fingertype"
#define DRIVE_LABEL "FINGERTYPE "  // 配置盘卷标：正好 11 个字符，大写，不足补空格

// ---- 蓝牙 -------------------------------------------------------------------
#define BLE_DEVICE_NAME PRODUCT_NAME
constexpr uint32_t PAIRING_WINDOW_MS = 60 * 1000;  // 网页上点“允许配对”后的有效时间
constexpr uint32_t BLE_RECONNECT_WAIT_MS = 6000;   // 按下手指时蓝牙未连接，最多等它重连多久

#define FIRMWARE_VERSION "0.6.6"
// 固件文件里的识别标记：网页导入固件时靠它确认是本项目的固件，并读出版本号
#define FIRMWARE_MARKER "FTYPFW1:"

// ---- 在线升级固件 -------------------------------------------------------------
constexpr size_t OTA_CHUNK = 1536;             // 每条命令默认携带的固件字节数（Base64 后约 2KB，蓝牙用这个）
constexpr size_t OTA_CHUNK_MAX = 4096;         // 网页可以要求的最大分段（USB 用，Base64 后约 5.5KB）
constexpr size_t USB_LINE_MAX = 8192;          // USB 串口一条命令的最大长度
constexpr uint32_t OTA_IDLE_TIMEOUT_MS = 30000;  // 升级中途这么久没收到数据就放弃
